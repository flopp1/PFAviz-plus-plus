#include "core/Lz4.h"

#include <algorithm>
#include <cstring>

namespace
{
    const int HashBits = 14;
    const size_t MinMatch = 4;
    const size_t LastLiterals = 5;          // the block format ends with at least 5 literals
    const size_t MatchSearchLimit = 12;     // no match may start in the last 12 bytes

    inline uint32_t Read32( const uint8_t *p ) { uint32_t v; memcpy( &v, p, 4 ); return v; }
    inline uint64_t Read64( const uint8_t *p ) { uint64_t v; memcpy( &v, p, 8 ); return v; }
    inline uint32_t Hash( uint32_t v ) { return ( v * 2654435761u ) >> ( 32 - HashBits ); }

    inline uint8_t *WriteLength( uint8_t *op, size_t n )
    {
        while ( n >= 255 ) { *op++ = 255; n -= 255; }
        *op++ = (uint8_t)n;
        return op;
    }
}

size_t Lz4::Compress( const uint8_t *src, size_t n, uint8_t *dst )
{
    uint8_t *op = dst;
    const uint8_t *ip = src, *anchor = src, *end = src + n;
    if ( n > MatchSearchLimit )
    {
        uint32_t aTable[1 << HashBits];
        memset( aTable, 0, sizeof( aTable ) ); // positions + 1; 0 = empty
        const uint8_t *limit = end - MatchSearchLimit;
        while ( ip < limit )
        {
            uint32_t h = Hash( Read32( ip ) );
            const uint8_t *ref = aTable[h] ? src + aTable[h] - 1 : nullptr;
            aTable[h] = (uint32_t)( ip - src ) + 1;
            if ( !ref || ip - ref > 65535 || Read32( ref ) != Read32( ip ) ) { ip++; continue; }

            // Extend the match (it may not run into the last literals)
            const uint8_t *matchEnd = end - LastLiterals;
            size_t len = MinMatch;
            // 8 bytes at a time: the first differing byte is the lowest set byte of the XOR
            while ( ip + len + 8 <= matchEnd )
            {
                uint64_t x = Read64( ref + len ) ^ Read64( ip + len );
                if ( x ) { len += __builtin_ctzll( x ) >> 3; goto extended; }
                len += 8;
            }
            while ( ip + len < matchEnd && ref[len] == ip[len] ) len++;
        extended:
            // and backwards over pending literals
            while ( ip > anchor && ref > src && ip[-1] == ref[-1] ) { ip--; ref--; len++; }

            size_t lit = (size_t)( ip - anchor ), ml = len - MinMatch;
            uint8_t *token = op++;
            *token = (uint8_t)( ( lit >= 15 ? 15 : lit ) << 4 );
            if ( lit >= 15 ) op = WriteLength( op, lit - 15 );
            memcpy( op, anchor, lit );
            op += lit;
            uint16_t off = (uint16_t)( ip - ref );
            *op++ = (uint8_t)off;
            *op++ = (uint8_t)( off >> 8 );
            *token |= (uint8_t)( ml >= 15 ? 15 : ml );
            if ( ml >= 15 ) op = WriteLength( op, ml - 15 );

            ip += len;
            anchor = ip;
            if ( ip < limit ) aTable[Hash( Read32( ip - 2 ) )] = (uint32_t)( ip - 2 - src ) + 1;
        }
    }
    // Last literals
    size_t lit = (size_t)( end - anchor );
    uint8_t *token = op++;
    *token = (uint8_t)( ( lit >= 15 ? 15 : lit ) << 4 );
    if ( lit >= 15 ) op = WriteLength( op, lit - 15 );
    memcpy( op, anchor, lit );
    op += lit;
    return (size_t)( op - dst );
}

bool Lz4::Decompress( const uint8_t *src, size_t iSize, uint8_t *dst, size_t iRaw )
{
    const uint8_t *ip = src, *iend = src + iSize;
    uint8_t *op = dst, *oend = dst + iRaw;
    auto Copy16 = []( uint8_t *d, const uint8_t *s ) { uint64_t a, b; memcpy( &a, s, 8 ); memcpy( &b, s + 8, 8 ); memcpy( d, &a, 8 ); memcpy( d + 8, &b, 8 ); };
    while ( ip < iend )
    {
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if ( lit == 15 )
        {
            unsigned s;
            do { if ( ip >= iend ) return false; s = *ip++; lit += s; } while ( s == 255 );
        }
        if ( lit > (size_t)( iend - ip ) || lit > (size_t)( oend - op ) ) return false;
        // Literals: 16 bytes at a time (over-reads and over-writes stay within the slack)
        if ( lit <= 16 ) Copy16( op, ip );
        else memcpy( op, ip, lit );
        ip += lit;
        op += lit;
        if ( ip >= iend ) break;    // the last sequence has literals only
        if ( iend - ip < 2 ) return false;
        size_t off = ip[0] | ( ip[1] << 8 );
        ip += 2;
        if ( !off || off > (size_t)( op - dst ) ) return false;
        size_t ml = token & 15;
        if ( ml == 15 )
        {
            unsigned s;
            do { if ( ip >= iend ) return false; s = *ip++; ml += s; } while ( s == 255 );
        }
        ml += MinMatch;
        if ( ml > (size_t)( oend - op ) ) return false;
        const uint8_t *ref = op - off;
        uint8_t *mEnd = op + ml;
        if ( off >= 16 )
            for ( ; op < mEnd; op += 16, ref += 16 ) Copy16( op, ref );
        else if ( off >= 8 )
            for ( ; op < mEnd; op += 8, ref += 8 ) { uint64_t a; memcpy( &a, ref, 8 ); memcpy( op, &a, 8 ); }
        else
        {
            // A short period: its first 8 to 14 bytes one at a time, then 8 at a time from a whole
            // number of periods back (at least 8 bytes, so a copy never reads what it writes)
            size_t iBack = off * ( ( 8 + off - 1 ) / off );
            uint8_t *pFirst = op + std::min< size_t >( iBack, ml );
            for ( ; op < pFirst; ) *op++ = *ref++;
            for ( ; op < mEnd; op += 8 ) { uint64_t a; memcpy( &a, op - iBack, 8 ); memcpy( op, &a, 8 ); }
        }
        op = mEnd;
    }
    return op == oend;
}
