#include "song/Dedup.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <unordered_map>
#include <vector>

#define LZMA_API_STATIC
#include <bzlib.h>
#include <lz4frame.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

namespace
{
    const uint64_t Unknown = UINT64_MAX;
    const uint64_t MaxMember = uint64_t( 2 ) << 30;     // a member (compressed) longer than this is not cut out
    const uint64_t MaxOwned = uint64_t( 6 ) << 30;      // distinct members' bytes held (fMCG's limit)
    const uint64_t MaxTail = uint64_t( 1 ) << 30;       // bytes after a layer's members (a raw tail) held
    const size_t MaxPattern = 64;                       // longest pattern repeated in a row looked for (members)
    const size_t MaxLogical = size_t( 1 ) << 28;        // tracks of the file, copies included

    std::wstring Format( const wchar_t *sFormat, ... )
    {
        wchar_t buf[512];
        va_list va;
        va_start( va, sFormat );
        vswprintf( buf, 512, sFormat, va );
        va_end( va );
        return buf;
    }
    std::wstring Bytes( double d )
    {
        const wchar_t *aUnit[] = { L"bytes", L"KB", L"MB", L"GB", L"TB", L"PB", L"EB" };
        int i = 0;
        while ( d >= 1024.0 && i < 6 ) { d /= 1024.0; i++; }
        return i ? Format( L"%.1f %ls", d, aUnit[i] ) : Format( L"%.0f bytes", d );
    }
    std::wstring Count( uint64_t n )
    {
        std::wstring s = std::to_wstring( n );
        for ( int i = (int)s.size() - 3; i > 0; i -= 3 ) s.insert( (size_t)i, L"," );
        return s;
    }
    uint64_t LE32( const uint8_t *p ) { return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24; }
    // PFAVIZ_DEDUPLOG: the discovery and the reading, step by step, on stderr
    const bool g_bLog = getenv( "PFAVIZ_DEDUPLOG" ) != nullptr;
#define DLOG( ... ) do { if ( g_bLog ) fprintf( stderr, __VA_ARGS__ ); } while ( 0 )
    uint64_t SatMul( uint64_t a, uint64_t b ) { return a && b > UINT64_MAX / a ? UINT64_MAX : a * b; }
    uint64_t SatAdd( uint64_t a, uint64_t b ) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

    //-------------------------------------------------------------------------
    // The stream formats: their magics, where a stream ends, and decoding one
    //-------------------------------------------------------------------------

    enum Fmt : uint8_t { None, Xz, Gz, Bz2, Zst, Lz4 };
    const wchar_t *const FmtName[] = { L"MIDI", L"xz", L"gzip", L"bzip2", L"zstd", L"lz4" };
    const uint8_t XzMagic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
    const size_t SniffBytes = 10;

    // What a stream starting at p is, from its magic (bzip2: also its first block's or its end's)
    Fmt Sniff( const uint8_t *p, size_t n )
    {
        if ( n >= 6 && memcmp( p, XzMagic, 6 ) == 0 ) return Xz;
        if ( n >= 3 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 8 ) return Gz;
        if ( n >= 10 && p[0] == 'B' && p[1] == 'Z' && p[2] == 'h' && p[3] >= '1' && p[3] <= '9' &&
             ( memcmp( p + 4, "\x31\x41\x59\x26\x53\x59", 6 ) == 0 || memcmp( p + 4, "\x17\x72\x45\x38\x50\x90", 6 ) == 0 ) ) return Bz2;
        if ( n >= 4 && p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD ) return Zst;
        if ( n >= 4 && p[0] == 0x04 && p[1] == 0x22 && p[2] == 0x4D && p[3] == 0x18 ) return Lz4;
        return None;
    }

    bool XzVli( const uint8_t *b, size_t iEnd, size_t &i, uint64_t &v )
    {
        v = 0;
        for ( int k = 0; k < 9; k++ )
        {
            if ( i >= iEnd ) return false;
            uint8_t c = b[i++];
            v |= (uint64_t)( c & 0x7F ) << ( 7 * k );
            if ( !( c & 0x80 ) ) return true;
        }
        return false;
    }

    // b[0, fe) is exactly one xz stream: its footer's and index's CRCs hold and the index's blocks walk
    // back to its header (fMCG's xz_stream_exact: a copy of another stream inside never passes). *pUnc:
    // what it decodes to.
    bool XzExact( const uint8_t *b, size_t fe, uint64_t *pUnc )
    {
        if ( fe < 32 || memcmp( b, XzMagic, 6 ) != 0 ) return false;
        const uint8_t *f = b + fe - 12;
        if ( f[10] != 'Y' || f[11] != 'Z' || f[8] != b[6] || f[9] != b[7] ) return false;
        if ( crc32( 0, f + 4, 6 ) != LE32( f ) || crc32( 0, b + 6, 2 ) != LE32( b + 8 ) ) return false;
        uint64_t isz = ( LE32( f + 4 ) + 1 ) * 4;
        if ( isz + 24 > fe ) return false;
        size_t ix = fe - 12 - (size_t)isz;
        if ( b[ix] != 0 || crc32( 0, b + ix, (uInt)( isz - 4 ) ) != LE32( b + ix + isz - 4 ) ) return false;
        size_t i = ix + 1, iEnd = ix + (size_t)isz - 4;
        uint64_t nRec = 0, comp = 0, unc = 0;
        if ( !XzVli( b, iEnd, i, nRec ) ) return false;
        for ( uint64_t r = 0; r < nRec; r++ )
        {
            uint64_t up = 0, us = 0;
            if ( !XzVli( b, iEnd, i, up ) || !XzVli( b, iEnd, i, us ) ) return false;
            comp += ( up + 3 ) & ~uint64_t( 3 );
            unc += us;
            if ( comp > fe ) return false;
        }
        if ( 12 + comp != ix ) return false;
        if ( pUnc ) *pUnc = unc;
        return true;
    }

    // A zstd or lz4 frame starting at p, walked by its block sizes: its length, 0 if more bytes are
    // needed (than the n there), -1 if it is not one. *pUnc: what it decodes to, when it says.
    int64_t ZstEnd( const uint8_t *p, size_t n, uint64_t *pUnc )
    {
        if ( n < 6 ) return 0;
        unsigned fhd = p[4];
        if ( fhd & 0x08 ) return -1;
        unsigned fcs = fhd >> 6, single = ( fhd >> 5 ) & 1, chk = ( fhd >> 2 ) & 1, did = fhd & 3;
        static const unsigned aDid[4] = { 0, 1, 2, 4 };
        size_t q = 5 + ( single ? 0 : 1 ) + aDid[did];
        size_t iFcs = fcs ? ( size_t( 1 ) << fcs ) : single;
        if ( q + iFcs > n ) return 0;
        *pUnc = Unknown;
        if ( iFcs )
        {
            uint64_t v = 0;
            for ( size_t i = 0; i < iFcs; i++ ) v |= (uint64_t)p[q + i] << ( 8 * i );
            *pUnc = iFcs == 2 ? v + 256 : v;
        }
        q += iFcs;
        for ( ;; )
        {
            if ( q + 3 > n ) return 0;
            unsigned bh = (unsigned)( p[q] | p[q + 1] << 8 | p[q + 2] << 16 );
            unsigned type = ( bh >> 1 ) & 3;
            if ( type == 3 ) return -1;
            q += 3 + ( type == 1 ? 1 : ( bh >> 3 ) );
            if ( bh & 1 ) break;
            if ( q > MaxMember ) return -1;
        }
        q += chk ? 4 : 0;
        return q <= n ? (int64_t)q : 0;
    }
    int64_t Lz4End( const uint8_t *p, size_t n, uint64_t *pUnc )
    {
        if ( n < 7 ) return 0;
        unsigned flg = p[4];
        if ( ( flg >> 6 ) != 1 ) return -1;
        size_t q = 6;
        *pUnc = Unknown;
        if ( flg & 0x08 )
        {
            if ( q + 8 > n ) return 0;
            *pUnc = LE32( p + q ) | LE32( p + q + 4 ) << 32;
            q += 8;
        }
        q += ( flg & 0x01 ? 4 : 0 ) + 1;
        for ( ;; )
        {
            if ( q + 4 > n ) return 0;
            uint64_t sz = LE32( p + q );
            q += 4;
            if ( !sz ) break;
            q += ( sz & 0x7FFFFFFF ) + ( flg & 0x10 ? 4 : 0 );
            if ( q > MaxMember ) return -1;
        }
        q += flg & 0x04 ? 4 : 0;
        return q <= n ? (int64_t)q : 0;
    }

    // One stream of a format, decoded from bytes in memory (a whole member, or for gzip and bzip2 the
    // bytes it might end in: Consumed says where it did)
    class Decoder
    {
    public:
        Decoder() = default;
        Decoder( const Decoder & ) = delete;
        ~Decoder() { Close(); }
        bool Start( Fmt fmt, const uint8_t *p, size_t n, int iThreads = 1 )
        {
            Close();
            m_fmt = fmt;
            m_p = p;
            m_n = n;
            m_iUsed = 0;
            m_bEnd = m_bError = false;
            switch ( fmt )
            {
                case Xz:
                {
                    m_xz = LZMA_STREAM_INIT;
                    lzma_ret r;
                    if ( iThreads > 1 )
                    {
                        lzma_mt mt = {};
                        mt.flags = LZMA_CONCATENATED;
                        mt.threads = (uint32_t)iThreads;
                        mt.memlimit_threading = uint64_t( 2 ) << 30;
                        mt.memlimit_stop = UINT64_MAX;
                        r = lzma_stream_decoder_mt( &m_xz, &mt );
                    }
                    else r = lzma_stream_decoder( &m_xz, UINT64_MAX, LZMA_CONCATENATED );
                    m_xz.next_in = p;
                    m_xz.avail_in = n;
                    return m_bOpen = r == LZMA_OK;
                }
                case Gz:
                    m_gz = z_stream();
                    m_gz.next_in = (Bytef *)p;
                    m_gz.avail_in = (uInt)std::min< size_t >( n, UINT32_MAX );
                    return m_bOpen = inflateInit2( &m_gz, 16 + 15 ) == Z_OK;
                case Bz2:
                    m_bz = bz_stream();
                    m_bz.next_in = (char *)p;
                    m_bz.avail_in = (unsigned)std::min< size_t >( n, UINT32_MAX );
                    return m_bOpen = BZ2_bzDecompressInit( &m_bz, 0, 0 ) == BZ_OK;
                case Zst:
                    m_zst = ZSTD_createDStream();
                    if ( !m_zst ) return false;
                    ZSTD_DCtx_setParameter( m_zst, ZSTD_d_windowLogMax, 31 );
                    return m_bOpen = true;
                case Lz4:
                    return m_bOpen = !LZ4F_isError( LZ4F_createDecompressionContext( &m_lz4, LZ4F_VERSION ) );
                default:
                    return false;
            }
        }
        // Up to cap bytes; 0 at the stream's end (or an error)
        size_t Read( uint8_t *pOut, size_t cap )
        {
            if ( !m_bOpen || m_bEnd || m_bError ) return 0;
            for ( ;; )
            {
                size_t got = 0;
                switch ( m_fmt )
                {
                    case Xz:
                    {
                        m_xz.next_out = pOut;
                        m_xz.avail_out = cap;
                        lzma_ret r = lzma_code( &m_xz, LZMA_FINISH );
                        got = cap - m_xz.avail_out;
                        m_iUsed = m_n - m_xz.avail_in;
                        if ( r == LZMA_STREAM_END ) m_bEnd = true;
                        else if ( r != LZMA_OK ) m_bError = true;
                        break;
                    }
                    case Gz:
                    {
                        m_gz.next_out = pOut;
                        m_gz.avail_out = (uInt)std::min< size_t >( cap, UINT32_MAX );
                        uInt uOut = m_gz.avail_out;
                        int r = inflate( &m_gz, Z_NO_FLUSH );
                        got = uOut - m_gz.avail_out;
                        m_iUsed = (size_t)( (const uint8_t *)m_gz.next_in - m_p );
                        Refill( m_gz.avail_in, m_gz.next_in );
                        if ( r == Z_STREAM_END ) m_bEnd = true;
                        else if ( r != Z_OK && !( r == Z_BUF_ERROR && got ) ) m_bError = true;
                        break;
                    }
                    case Bz2:
                    {
                        m_bz.next_out = (char *)pOut;
                        m_bz.avail_out = (unsigned)std::min< size_t >( cap, UINT32_MAX );
                        unsigned uOut = m_bz.avail_out;
                        int r = BZ2_bzDecompress( &m_bz );
                        got = uOut - m_bz.avail_out;
                        m_iUsed = (size_t)( (const uint8_t *)m_bz.next_in - m_p );
                        if ( !m_bz.avail_in && m_iUsed < m_n )
                        {
                            m_bz.next_in = (char *)m_p + m_iUsed;
                            m_bz.avail_in = (unsigned)std::min< size_t >( m_n - m_iUsed, UINT32_MAX );
                        }
                        if ( r == BZ_STREAM_END ) m_bEnd = true;
                        else if ( r != BZ_OK ) m_bError = true;
                        break;
                    }
                    case Zst:
                    {
                        ZSTD_inBuffer in = { m_p, m_n, m_iUsed };
                        ZSTD_outBuffer out = { pOut, cap, 0 };
                        size_t r = ZSTD_decompressStream( m_zst, &out, &in );
                        got = out.pos;
                        m_iUsed = in.pos;
                        if ( ZSTD_isError( r ) ) m_bError = true;
                        else if ( !r ) m_bEnd = true;
                        break;
                    }
                    case Lz4:
                    {
                        size_t iOut = cap, iIn = m_n - m_iUsed;
                        size_t r = LZ4F_decompress( m_lz4, pOut, &iOut, m_p + m_iUsed, &iIn, nullptr );
                        got = iOut;
                        m_iUsed += iIn;
                        if ( LZ4F_isError( r ) ) m_bError = true;
                        else if ( !r ) m_bEnd = true;
                        break;
                    }
                    default:
                        m_bError = true;
                }
                if ( got ) return got;
                if ( m_bEnd || m_bError ) return 0;
                if ( m_iUsed >= m_n ) { m_bError = true; return 0; }    // the input ran out before the end
            }
        }
        bool Ended() const { return m_bEnd; }
        bool Failed() const { return m_bError; }
        size_t Consumed() const { return m_iUsed; }

    private:
        void Refill( uInt &uAvail, Bytef *&pNext )
        {
            if ( !uAvail && m_iUsed < m_n )
            {
                pNext = (Bytef *)m_p + m_iUsed;
                uAvail = (uInt)std::min< size_t >( m_n - m_iUsed, UINT32_MAX );
            }
        }
        void Close()
        {
            if ( !m_bOpen ) { if ( m_zst ) { ZSTD_freeDStream( m_zst ); m_zst = nullptr; } return; }
            switch ( m_fmt )
            {
                case Xz: lzma_end( &m_xz ); break;
                case Gz: inflateEnd( &m_gz ); break;
                case Bz2: BZ2_bzDecompressEnd( &m_bz ); break;
                case Zst: ZSTD_freeDStream( m_zst ); m_zst = nullptr; break;
                case Lz4: LZ4F_freeDecompressionContext( m_lz4 ); m_lz4 = nullptr; break;
                default: break;
            }
            m_bOpen = false;
        }
        Fmt m_fmt = None;
        const uint8_t *m_p = nullptr;
        size_t m_n = 0, m_iUsed = 0;
        bool m_bOpen = false, m_bEnd = false, m_bError = false;
        lzma_stream m_xz = LZMA_STREAM_INIT;
        z_stream m_gz = z_stream();
        bz_stream m_bz = bz_stream();
        ZSTD_DStream *m_zst = nullptr;
        LZ4F_dctx *m_lz4 = nullptr;
    };

    // A gzip or bzip2 stream at p: its length, found by decoding it (0: more bytes needed, -1: not one)
    int64_t DecodedEnd( Fmt fmt, const uint8_t *p, size_t n, bool bEof )
    {
        static thread_local std::vector< uint8_t > vSink( 1 << 20 );
        Decoder d;
        if ( !d.Start( fmt, p, n ) ) return -1;
        while ( d.Read( vSink.data(), vSink.size() ) ) {}
        if ( d.Ended() ) return (int64_t)d.Consumed();
        return d.Consumed() >= n && !bEof ? 0 : -1;
    }

    //-------------------------------------------------------------------------
    // The grammar
    //-------------------------------------------------------------------------

    struct Tok
    {
        uint32_t uNode;
        uint64_t llCount;
        bool operator==( const Tok &o ) const { return uNode == o.uNode && llCount == o.llCount; }
    };

    struct Node
    {
        enum Kind : uint8_t { Member, Seq, Raw } eKind = Member;
        Fmt fmt = None;
        bool bLeaf = false;             // Member: what it holds is MIDI bytes (else uSeq)
        const uint8_t *p = nullptr;     // Member: its bytes (in the file, or vOwned); Raw: the bytes
        uint64_t n = 0;
        std::vector< uint8_t > vOwned;
        uint64_t llUnc = Unknown;       // Member: what it decodes to, when known
        Dedup::Hasher::Value hv = {};   // Member: its bytes' hash (all that is kept of them once its layer is cut)
        uint32_t uSeq = UINT32_MAX;     // Member: the layer it holds
        std::vector< Tok > vToks;       // Seq: its parts in order, each repeated so many times in a row
        uint64_t llFlat = Unknown;      // MIDI bytes it expands to (Flat)
        int iDepth = 0;                 // layers below the file
    };

    class Grammar
    {
    public:
        std::vector< Node > vNodes;
        uint64_t llOwned = 0, llDecoded = 0, llMembers = 0, llAssumed = 0, llRawTails = 0;
        std::wstring sAssumed;
        Fmt aLayerFmt[16] = {};
        int iLayers = 0;

        // The member p[0, n) (equal ones are the same node)
        uint32_t InternMember( Fmt fmt, const uint8_t *p, uint64_t n, uint64_t llUnc, bool bOwned, int iDepth, std::wstring &sErr )
        {
            Dedup::Hasher hh;
            hh.Add( p, (size_t)n );
            Dedup::Hasher::Value hv = hh.Finish();
            auto range = m_mMembers.equal_range( hv.a );
            for ( auto it = range.first; it != range.second; ++it )
            {
                const Node &c = vNodes[it->second];
                if ( c.n == n && ( c.p ? memcmp( c.p, p, (size_t)n ) == 0 : c.hv == hv ) ) return it->second;
            }
            Node nd;
            nd.hv = hv;
            nd.fmt = fmt;
            nd.n = n;
            nd.llUnc = llUnc;
            nd.iDepth = iDepth;
            if ( bOwned )
            {
                llOwned += n;
                if ( llOwned > MaxOwned ) { sErr = L"The distinct pieces of the file take more than " + Bytes( (double)MaxOwned ) + L"."; return UINT32_MAX; }
                nd.vOwned.assign( p, p + n );
                nd.p = nd.vOwned.data();
            }
            else nd.p = p;
            vNodes.push_back( std::move( nd ) );
            uint32_t id = (uint32_t)( vNodes.size() - 1 );
            m_mMembers.emplace( hv.a, id );
            m_vQueue.push_back( id );
            if ( iDepth < 16 && !aLayerFmt[iDepth] ) aLayerFmt[iDepth] = fmt;
            iLayers = std::max( iLayers, iDepth + 1 );
            return id;
        }
        uint32_t NewRaw( std::vector< uint8_t > &&v )
        {
            Node nd;
            nd.eKind = Node::Raw;
            nd.vOwned = std::move( v );
            nd.p = nd.vOwned.data();
            nd.n = nd.vOwned.size();
            vNodes.push_back( std::move( nd ) );
            llRawTails++;
            return (uint32_t)( vNodes.size() - 1 );
        }
        // A pattern of parts (equal ones are the same node)
        uint32_t InternSeq( const std::vector< Tok > &v )
        {
            Dedup::Hasher hh;
            for ( const Tok &t : v ) { hh.Add( (const uint8_t *)&t.uNode, 4 ); hh.Add( (const uint8_t *)&t.llCount, 8 ); }
            uint64_t h = hh.Finish().a;
            auto range = m_mSeqs.equal_range( h );
            for ( auto it = range.first; it != range.second; ++it )
                if ( vNodes[it->second].vToks == v ) return it->second;
            Node nd;
            nd.eKind = Node::Seq;
            nd.vToks = v;
            vNodes.push_back( std::move( nd ) );
            uint32_t id = (uint32_t)( vNodes.size() - 1 );
            m_mSeqs.emplace( h, id );
            return id;
        }
        uint32_t NextQueued()
        {
            if ( m_iQueue >= m_vQueue.size() ) return UINT32_MAX;
            return m_vQueue[m_iQueue++];
        }
        // The sequence s as parts: a stretch that repeats in a row, by up to MaxPattern parts, becomes a
        // pattern (itself put the same way) with its count, taking the stretch that covers the most at
        // each point (greedy, front to back)
        std::vector< Tok > Compress( const std::vector< Tok > &s )
        {
            std::vector< Tok > out;
            size_t n = s.size();
            for ( size_t i = 0; i < n; )
            {
                size_t bestP = 1;
                uint64_t bestR = 1;
                for ( size_t p = 2; p <= MaxPattern && i + 2 * p <= n; p++ )
                {
                    uint64_t r = 1;
                    while ( i + ( r + 1 ) * p <= n && std::equal( s.begin() + (ptrdiff_t)i, s.begin() + (ptrdiff_t)( i + p ), s.begin() + (ptrdiff_t)( i + r * p ) ) ) r++;
                    if ( r > 1 && p * r > bestP * bestR ) { bestP = p; bestR = r; }
                }
                if ( bestP == 1 )
                {
                    if ( !out.empty() && out.back().uNode == s[i].uNode ) out.back().llCount += s[i].llCount;
                    else out.push_back( s[i] );
                    i++;
                    continue;
                }
                std::vector< Tok > pat( s.begin() + (ptrdiff_t)i, s.begin() + (ptrdiff_t)( i + bestP ) );
                uint32_t id = InternSeq( Compress( pat ) );
                if ( !out.empty() && out.back().uNode == id ) out.back().llCount += bestR;
                else out.push_back( { id, bestR } );
                i += bestP * bestR;
            }
            return out;
        }
        // MIDI bytes node i expands to (saturating; Unknown when a member does not say)
        uint64_t Flat( uint32_t i )
        {
            Node &nd = vNodes[i];
            if ( nd.llFlat != Unknown ) return nd.llFlat;
            uint64_t f = Unknown;
            if ( nd.eKind == Node::Raw ) f = nd.n;
            else if ( nd.eKind == Node::Member ) f = nd.bLeaf ? nd.llUnc : Flat( nd.uSeq );
            else
            {
                f = 0;
                for ( const Tok &t : std::vector< Tok >( nd.vToks ) )
                {
                    uint64_t c = Flat( t.uNode );
                    if ( c == Unknown ) { f = Unknown; break; }
                    f = SatAdd( f, SatMul( c, t.llCount ) );
                }
            }
            vNodes[i].llFlat = f;
            return f;
        }

    private:
        std::unordered_multimap< uint64_t, uint32_t > m_mMembers, m_mSeqs;
        std::vector< uint32_t > m_vQueue;
        size_t m_iQueue = 0;
    };

    //-------------------------------------------------------------------------
    // Cutting a layer into members
    //-------------------------------------------------------------------------

    // A layer's bytes: the file (all there), or decoded from a member as they are needed
    class LayerBytes
    {
    public:
        LayerBytes( const uint8_t *p, size_t n ) : m_pMap( p ), m_nMap( n ) {}
        explicit LayerBytes( Decoder *pDec ) : m_pDec( pDec ) {}
        const uint8_t *Ptr() const { return m_pMap ? m_pMap + m_iHead : m_vBuf.data() + m_iHead; }
        size_t Avail() const { return ( m_pMap ? m_nMap : m_vBuf.size() ) - m_iHead; }
        bool Eof() const { return m_pMap || m_bEof; }
        uint64_t Pos() const { return m_llBase + m_iHead; }     // bytes of the layer before Ptr()
        bool Owned() const { return !m_pMap; }
        void Take( size_t n ) { m_iHead += n; }
        // At least n bytes at Ptr() (fewer only at the end); false on a decoding error
        bool Ensure( size_t n, std::atomic< uint64_t > *pDone, uint64_t &llDecoded )
        {
            while ( Avail() < n && !Eof() )
            {
                if ( m_iHead > ( 64u << 20 ) && m_iHead * 2 > m_vBuf.size() )
                {
                    m_vBuf.erase( m_vBuf.begin(), m_vBuf.begin() + (ptrdiff_t)m_iHead );
                    m_llBase += m_iHead;
                    m_iHead = 0;
                }
                size_t iOld = m_vBuf.size(), iWant = std::max< size_t >( 4 << 20, n - Avail() );
                m_vBuf.resize( iOld + iWant );
                size_t got = 0;
                while ( got < iWant )
                {
                    size_t r = m_pDec->Read( m_vBuf.data() + iOld + got, iWant - got );
                    if ( !r ) break;
                    got += r;
                }
                m_vBuf.resize( iOld + got );
                llDecoded += got;
                if ( pDone ) *pDone += got;
                if ( !got ) { m_bEof = true; if ( m_pDec->Failed() ) return false; }
            }
            return true;
        }

    private:
        const uint8_t *m_pMap = nullptr;
        size_t m_nMap = 0;
        Decoder *m_pDec = nullptr;
        std::vector< uint8_t > m_vBuf;
        size_t m_iHead = 0;
        uint64_t m_llBase = 0;
        bool m_bEof = false;
    };

    class Discovery
    {
    public:
        Discovery( Grammar &g, bool bAssume, std::atomic< uint64_t > *pDone, std::atomic< uint64_t > *pTotal, const std::atomic< bool > *pCancel,
                   int iThreads )
            : G( g ), m_bAssume( bAssume ), m_pDone( pDone ), m_pTotal( pTotal ), m_pCancel( pCancel ), m_iThreads( iThreads ) {}

        // Cuts the layer in bytes into members, as a Seq node (*pSeq). False: it is not members (when
        // none was cut: not an error) or failed (sErr).
        bool Cut( LayerBytes &bytes, int iDepth, uint64_t llUnc, uint32_t *pSeq, std::wstring &sErr )
        {
            std::vector< Tok > vSeq;            // the members in order, a run of one as one part
            std::vector< uint32_t > vRecent;    // members lately cut, the latest first (fMCG: the next is usually one)
            struct Seen { uint32_t id; uint64_t llPos, llLen; };
            std::deque< Seen > dqSeen;          // this layer's latest members, for the assumption
            bool bAssumeOpen = m_bAssume && llUnc != Unknown;
            std::vector< size_t > vTried;       // tile lengths (in members) that did not fit: each tried once, at most 8
            auto Push = [&]( uint32_t id ) {
                if ( G.vNodes[id].eKind == Node::Member ) G.llMembers++;
                if ( !vSeq.empty() && vSeq.back().uNode == id ) vSeq.back().llCount++;
                else vSeq.push_back( { id, 1 } );
            };
            auto Remember = [&]( uint32_t id ) {
                auto it = std::find( vRecent.begin(), vRecent.end(), id );
                if ( it != vRecent.end() ) vRecent.erase( it );
                vRecent.insert( vRecent.begin(), id );
                if ( vRecent.size() > 32 ) vRecent.pop_back();
            };
            // fMCG's risky fast dedup (Kimrashi's trust_the_rest): the first stretch of up to MaxPattern
            // members that comes twice in a row, back to back, is the layer's tile, all before it a
            // header; if the rest of the layer is a whole number of tiles by size, it is taken to be
            // those tiles without decoding it. A tile that does not fit is not tried again, and a longer one
            // that repeats later is (a repeat within a track comes before the track's; up to 8 tries).
            // True: applied.
            auto Assume = [&]( uint32_t id, uint64_t llPos, uint64_t llLen ) -> bool {
                if ( !bAssumeOpen ) return false;
                dqSeen.push_back( { id, llPos, llLen } );
                if ( dqSeen.size() > 2 * MaxPattern ) dqSeen.pop_front();
                size_t n = dqSeen.size(), k = 0;
                for ( size_t c = 1; c <= n / 2 && !k; c++ )
                {
                    if ( std::find( vTried.begin(), vTried.end(), c ) != vTried.end() ) continue;
                    bool bSame = true;
                    for ( size_t i = 1; i <= c && bSame; i++ )
                        bSame = dqSeen[n - i].id == dqSeen[n - c - i].id && dqSeen[n - i].llLen == dqSeen[n - c - i].llLen;
                    if ( bSame && dqSeen[n - c].llPos - dqSeen[n - 2 * c].llPos == dqSeen[n - 1].llPos + dqSeen[n - 1].llLen - dqSeen[n - c].llPos ) k = c;
                }
                if ( !k ) return false;
                vTried.push_back( k );
                bAssumeOpen = vTried.size() < 8;
                uint64_t llHdr = dqSeen[n - 2 * k].llPos, P = dqSeen[n - k].llPos - llHdr;
                uint64_t llRest = llUnc > llHdr ? llUnc - llHdr : 0;
                if ( llRest < 2 * P || llRest % P ) return false;
                bAssumeOpen = false;
                uint64_t m = llRest / P;
                // The 2k members seen as they were, then the tile m - 2 more times
                std::vector< Tok > vTile;
                for ( size_t i = n - k; i < n; i++ )
                    if ( !vTile.empty() && vTile.back().uNode == dqSeen[i].id ) vTile.back().llCount++;
                    else vTile.push_back( { dqSeen[i].id, 1 } );
                if ( m > 2 )
                {
                    if ( vTile.size() == 1 ) Push( vTile[0].uNode ), vSeq.back().llCount += ( m - 2 ) * vTile[0].llCount - 1;
                    else vSeq.push_back( { G.InternSeq( vTile ), m - 2 } );
                    G.llMembers += ( m - 2 ) * k - ( vTile.size() == 1 ? 1 : 0 );   // (Push counted one)
                }
                G.llAssumed += m - 2;
                G.sAssumed += Format( L"%ls%ls bytes after %ls header bytes taken as a %ls-byte tile x %ls", G.sAssumed.empty() ? L"" : L"; ",
                                      Count( llRest - 2 * P ).c_str(), Count( llHdr ).c_str(), Count( P ).c_str(), Count( m ).c_str() );
                return true;
            };
            bool bAny = false;
            for ( ;; )
            {
                if ( Cancelled() ) { sErr = L"Cancelled."; return false; }
                if ( !bytes.Ensure( 16, m_pDone, G.llDecoded ) ) { sErr = L"A part of the file could not be decompressed."; return false; }
                if ( !bytes.Avail() ) break;
                const uint8_t *p = bytes.Ptr();
                size_t n = bytes.Avail();
                // Stream padding (xz): zeros by fours, then more members
                if ( bAny && n >= 4 && LE32( p ) == 0 )
                {
                    size_t z = 0;
                    for ( ;; )
                    {
                        while ( z + 4 <= bytes.Avail() && LE32( bytes.Ptr() + z ) == 0 ) z += 4;
                        if ( z + 4 <= bytes.Avail() || bytes.Eof() || z > ( 1 << 20 ) ) break;
                        if ( !bytes.Ensure( z + 16, m_pDone, G.llDecoded ) ) { sErr = L"A part of the file could not be decompressed."; return false; }
                    }
                    if ( !bytes.Ensure( z + 16, m_pDone, G.llDecoded ) ) { sErr = L"A part of the file could not be decompressed."; return false; }
                    if ( z < bytes.Avail() && Sniff( bytes.Ptr() + z, bytes.Avail() - z ) != None ) { bytes.Take( z ); continue; }
                    if ( z >= bytes.Avail() ) { bytes.Take( z ); continue; }   // padding to the end
                }
                Fmt fmt = Sniff( p, n );
                if ( fmt == None )
                {
                    if ( !bAny ) return false;
                    // Bytes after the members that are not one: a raw tail of the layer
                    while ( !bytes.Eof() && bytes.Avail() <= MaxTail )
                        if ( !bytes.Ensure( bytes.Avail() + ( 4 << 20 ), m_pDone, G.llDecoded ) ) { sErr = L"A part of the file could not be decompressed."; return false; }
                    if ( bytes.Avail() > MaxTail ) { sErr = L"A layer of the file ends in more than 1 GB that is not compressed."; return false; }
                    Push( G.NewRaw( std::vector< uint8_t >( bytes.Ptr(), bytes.Ptr() + bytes.Avail() ) ) );
                    bytes.Take( bytes.Avail() );
                    break;
                }
                // A member lately cut, here again: compared byte for byte
                uint32_t uFound = UINT32_MAX;
                for ( uint32_t id : vRecent )
                {
                    const Node &c = G.vNodes[id];
                    if ( c.fmt != fmt || !c.p ) continue;
                    if ( bytes.Avail() < c.n && !bytes.Ensure( (size_t)c.n, m_pDone, G.llDecoded ) ) { sErr = L"A part of the file could not be decompressed."; return false; }
                    if ( bytes.Avail() >= c.n && memcmp( bytes.Ptr(), G.vNodes[id].p, (size_t)c.n ) == 0 ) { uFound = id; break; }
                }
                uint64_t llLen = 0, llMemberUnc = Unknown;
                if ( uFound != UINT32_MAX ) llLen = G.vNodes[uFound].n;
                else
                {
                    int64_t r = FindEnd( fmt, bytes, vRecent, llMemberUnc, sErr );
                    if ( r < 0 ) return false;
                    if ( r == 0 )
                    {
                        if ( !bAny ) return false;
                        sErr = L"A compressed part of the file does not end where it should.";
                        return false;
                    }
                    llLen = (uint64_t)r;
                    uFound = G.InternMember( fmt, bytes.Ptr(), llLen, llMemberUnc, bytes.Owned(), iDepth, sErr );
                    if ( uFound == UINT32_MAX ) return false;
                }
                bAny = true;
                uint64_t llPos = bytes.Pos();
                Push( uFound );
                Remember( uFound );
                bytes.Take( (size_t)llLen );
                if ( Assume( uFound, llPos, llLen ) ) break;
            }
            *pSeq = G.InternSeq( G.Compress( vSeq ) );
            return true;
        }

        // Every member found, layer by layer (a member's layer is cut only once the one around it has
        // been): what it holds is decoded once and cut in turn, or found to be MIDI bytes (a leaf)
        bool Run( std::wstring &sErr )
        {
            for ( uint32_t id; ( id = G.NextQueued() ) != UINT32_MAX; )
            {
                if ( Cancelled() ) { sErr = L"Cancelled."; return false; }
                Node &nd = G.vNodes[id];
                Decoder dec;
                if ( !dec.Start( nd.fmt, nd.p, (size_t)nd.n, nd.n > ( 1 << 20 ) ? m_iThreads : 1 ) ) { sErr = L"A part of the file could not be decompressed."; return false; }
                LayerBytes bytes( &dec );
                if ( !bytes.Ensure( SniffBytes, m_pDone, G.llDecoded ) ) { sErr = L"A part of the file could not be decompressed."; return false; }
                if ( Sniff( bytes.Ptr(), bytes.Avail() ) == None ) { G.vNodes[id].bLeaf = true; DLOG( "node %u depth %d: leaf, %llu -> %llu\n", id, nd.iDepth, (unsigned long long)nd.n, (unsigned long long)nd.llUnc ); continue; }
                uint64_t llUnc = nd.llUnc;
                DLOG( "node %u depth %d: cutting, %llu -> %llu (decoded so far %llu)\n", id, nd.iDepth, (unsigned long long)nd.n, (unsigned long long)llUnc, (unsigned long long)G.llDecoded );
                if ( m_pTotal && llUnc != Unknown ) *m_pTotal += llUnc;
                uint32_t uSeq = UINT32_MAX;
                if ( !Cut( bytes, nd.iDepth + 1, llUnc, &uSeq, sErr ) )
                {
                    if ( !sErr.empty() ) return false;
                    G.vNodes[id].bLeaf = true;  // not members after all: MIDI bytes that start like one
                    continue;
                }
                G.vNodes[id].uSeq = uSeq;
                DLOG( "  node %u: %zu parts, decoded so far %llu, nodes %zu\n", id, G.vNodes[uSeq].vToks.size(), (unsigned long long)G.llDecoded, G.vNodes.size() );
                if ( !G.vNodes[id].vOwned.empty() )
                {
                    G.llOwned -= G.vNodes[id].vOwned.size();
                    std::vector< uint8_t >().swap( G.vNodes[id].vOwned );   // (its layer is all that is read)
                }
                G.vNodes[id].p = nullptr;
            }
            return true;
        }

    private:
        bool Cancelled() const { return m_pCancel && m_pCancel->load( std::memory_order_relaxed ); }
        // Where the member of format fmt at bytes.Ptr() ends: its length, 0 if it does not, -1 on an error
        int64_t FindEnd( Fmt fmt, LayerBytes &bytes, const std::vector< uint32_t > &vRecent, uint64_t &llUnc, std::wstring &sErr )
        {
            auto Fail = [&]() { sErr = L"A part of the file could not be decompressed."; return -1; };
            if ( fmt == Xz )
            {
                // The length of a member lately cut, then the footers ahead
                for ( uint32_t id : vRecent )
                {
                    if ( G.vNodes[id].fmt != Xz ) continue;
                    uint64_t L = G.vNodes[id].n;
                    if ( !bytes.Ensure( (size_t)L, m_pDone, G.llDecoded ) ) return Fail();
                    if ( bytes.Avail() >= L && XzExact( bytes.Ptr(), (size_t)L, &llUnc ) ) return (int64_t)L;
                }
                size_t s = 30;
                for ( ;; )
                {
                    const uint8_t *p = bytes.Ptr();
                    size_t n = bytes.Avail();
                    while ( s + 1 < n )
                    {
                        const uint8_t *y = (const uint8_t *)memchr( p + s, 'Y', n - 1 - s );
                        if ( !y ) { s = n - 1; break; }
                        size_t yi = (size_t)( y - p );
                        if ( p[yi + 1] == 'Z' && XzExact( p, yi + 2, &llUnc ) ) return (int64_t)( yi + 2 );
                        s = yi + 1;
                    }
                    if ( bytes.Eof() || n > MaxMember ) return 0;
                    if ( !bytes.Ensure( n + ( 4 << 20 ), m_pDone, G.llDecoded ) ) return Fail();
                    if ( Cancelled() ) { sErr = L"Cancelled."; return -1; }
                }
            }
            for ( ;; )
            {
                int64_t r = fmt == Zst ? ZstEnd( bytes.Ptr(), bytes.Avail(), &llUnc )
                          : fmt == Lz4 ? Lz4End( bytes.Ptr(), bytes.Avail(), &llUnc )
                          : DecodedEnd( fmt, bytes.Ptr(), bytes.Avail(), bytes.Eof() );
                if ( r != 0 ) return r < 0 ? 0 : r;
                if ( bytes.Eof() || bytes.Avail() > MaxMember ) return 0;
                if ( !bytes.Ensure( std::max< size_t >( bytes.Avail() * 2, 4 << 20 ), m_pDone, G.llDecoded ) ) return Fail();
                if ( Cancelled() ) { sErr = L"Cancelled."; return -1; }
            }
        }

        Grammar &G;
        bool m_bAssume;
        std::atomic< uint64_t > *m_pDone, *m_pTotal;
        const std::atomic< bool > *m_pCancel;
        int m_iThreads;
    };

    //-------------------------------------------------------------------------
    // Reading: the grammar expanded as the MIDI's chunks are read
    //-------------------------------------------------------------------------

    class DedupReader : public Archive::Reader
    {
    public:
        DedupReader( HANDLE hFile, HANDLE hMap, const uint8_t *pMap, uint64_t llFile, std::shared_ptr< Grammar > g, uint32_t uRoot, const std::atomic< bool > *pCancel,
                     int iThreads = 1 )
            : m_hFile( hFile ), m_hMap( hMap ), m_pMap( pMap ), m_llFile( llFile ), G( std::move( g ) ), m_pCancel( pCancel ), m_iThreads( iThreads )
        {
            m_vMemo.assign( G->vNodes.size(), Memo() );
            m_llTotal = G->Flat( uRoot );
            Enter( uRoot, false );
        }
        ~DedupReader()
        {
            m_dec.reset();
            if ( m_pMap ) UnmapViewOfFile( m_pMap );
            if ( m_hMap ) CloseHandle( m_hMap );
            if ( m_hFile != INVALID_HANDLE_VALUE ) CloseHandle( m_hFile );
        }
        // The first n bytes it expands to (read again from the start after)
        bool Head( uint8_t *p, size_t n )
        {
            size_t got = 0;
            while ( got < n )
            {
                int64_t r = Pull( p + got, n - got );
                if ( r <= 0 ) return false;
                got += (size_t)r;
            }
            return true;
        }

        bool NextChunk( char aType[4], uint64_t &llLen ) override
        {
            if ( m_llLeft && !Skip( m_llLeft ) ) return false;
            m_llLeft = 0;
            uint8_t a[8];
            size_t got = std::min< size_t >( m_vBack.size(), 8 );
            memcpy( a, m_vBack.data(), got );
            m_vBack.clear();
            m_bBoundary = !got;     // nothing of the next chunk read yet
            while ( got < 8 )
            {
                int64_t r = Pull( a + got, 8 - got );
                m_bBoundary = false;
                if ( r <= 0 ) return false;
                got += (size_t)r;
            }
            memcpy( aType, a, 4 );
            llLen = m_llLeft = (uint64_t)a[4] << 24 | (uint64_t)a[5] << 16 | (uint64_t)a[6] << 8 | a[7];
            if ( memcmp( a, "MTrk", 4 ) == 0 )
            {
                if ( !AddLogical( m_uOrdinal++ ) ) return false;
            }
            return true;
        }
        size_t ReadData( uint8_t *p, size_t n ) override
        {
            n = (size_t)std::min< uint64_t >( n, m_llLeft );
            if ( !n ) return 0;
            int64_t r = Pull( p, n );
            if ( r <= 0 ) { m_llLeft = 0; return 0; }
            m_llLeft -= (uint64_t)r;
            return (size_t)r;
        }
        void PutBack( const uint8_t *p, size_t n ) override { m_vBack.assign( p, p + n ); }
        uint64_t Done() const override { return m_llPos; }
        uint64_t Total() const override { return m_llTotal != Unknown ? m_llTotal : 0; }
        std::wstring Summary() const override
        {
            std::wstring s;
            for ( int i = 0; i < std::min( G->iLayers, 16 ); i++ ) s += std::wstring( FmtName[G->aLayerFmt[i]] ) + L" > ";
            s += L"MIDI";
            if ( m_llTotal != Unknown ) s += L", " + Bytes( (double)m_llTotal );
            size_t iDistinct = 0;
            for ( const Node &nd : G->vNodes ) iDistinct += nd.eKind == Node::Member;
            s += L"\r\nDeduplicated: " + Count( G->llMembers ) + L" compressed pieces in " + Count( iDistinct ) + L" distinct, "
               + Bytes( (double)G->llDecoded ) + L" decoded to find them; " + Count( m_vLogical.size() ) + L" tracks, "
               + Count( m_uOrdinal ) + L" read, the rest counted as copies";
            if ( G->llAssumed ) s += L"\r\nAssumed (\"assume the rest of a layer repeats\"): " + G->sAssumed;
            return s;
        }
        const std::vector< uint32_t > *Logical() const override { return &m_vLogical; }
        int Layers() const override { return G->iLayers; }

    private:
        struct Memo { uint64_t a = UINT64_MAX, b = 0; };    // the logical tracks a node gave, read whole from boundary to boundary
        struct Frame
        {
            uint32_t uSeq, uNode;           // the parts read, and the node entered (a member holding them, or them)
            size_t iTok = 0;
            uint64_t llRep = 0;             // repeats of the current part entered
            bool bB = false;                // a repeat of the current part began at a chunk boundary: which, and the tracks then
            uint64_t llRepB = 0;
            size_t iLogB = 0;
            bool bEntryB;                   // the node was entered at a chunk boundary, with the tracks then
            size_t iLogEntry;
        };

        bool Cancelled() const { return m_pCancel && m_pCancel->load( std::memory_order_relaxed ); }
        bool AddLogical( uint32_t u )
        {
            if ( m_vLogical.size() >= MaxLogical ) { sError = L"The file has more tracks than PFAviz++ can count."; return false; }
            m_vLogical.push_back( u );
            return true;
        }
        // The logical tracks [a, b) again, q times
        bool AddCopies( size_t a, size_t b, uint64_t q )
        {
            if ( b <= a || !q ) return true;
            if ( ( b - a ) * q > MaxLogical - m_vLogical.size() || q > MaxLogical ) { sError = L"The file has more tracks than PFAviz++ can count."; return false; }
            for ( uint64_t i = 0; i < q; i++ )
                for ( size_t j = a; j < b; j++ ) m_vLogical.push_back( m_vLogical[j] );
            return true;
        }
        void Skipped( uint32_t uNode, uint64_t q )
        {
            uint64_t f = G->Flat( uNode );
            if ( f != Unknown ) m_llPos = SatAdd( m_llPos, SatMul( f, q ) );
        }
        void Enter( uint32_t id, bool bBoundary )
        {
            Node &nd = G->vNodes[id];
            if ( m_iLogs < 300 ) { m_iLogs++; DLOG( "enter %u (%s) at %llu, boundary %d, tracks %zu\n", id, nd.eKind == Node::Seq ? "seq" : nd.eKind == Node::Raw ? "raw" : nd.bLeaf ? "leaf" : "member", (unsigned long long)m_llPos, (int)bBoundary, m_vLogical.size() ); }
            if ( nd.eKind == Node::Seq || ( nd.eKind == Node::Member && !nd.bLeaf ) )
            {
                Frame f;
                f.uSeq = nd.eKind == Node::Seq ? id : nd.uSeq;
                f.uNode = id;
                f.bEntryB = bBoundary;
                f.iLogEntry = m_vLogical.size();
                m_vStack.push_back( f );
                return;
            }
            m_uLeaf = id;
            m_bLeafEntryB = bBoundary;
            m_iLeafLog = m_vLogical.size();
            m_iRawPos = 0;
            if ( nd.eKind == Node::Member )
            {
                m_dec.reset( new Decoder );
                if ( !m_dec->Start( nd.fmt, nd.p, (size_t)nd.n, nd.n > ( 1 << 20 ) ? m_iThreads : 1 ) ) m_bFailed = true;
            }
        }
        void Exit( uint32_t id, bool bEntryB, size_t iLog )
        {
            Memo &m = m_vMemo[id];
            if ( bEntryB && m_bBoundary && m.a == UINT64_MAX ) { m.a = iLog; m.b = m_vLogical.size(); }
        }
        // The next bytes it expands to: up to n, 0 at the end, -1 on an error (sError)
        int64_t Pull( uint8_t *p, size_t n )
        {
            for ( ;; )
            {
                if ( Cancelled() ) { sError = L"Cancelled."; return -1; }
                if ( m_bFailed ) { if ( sError.empty() ) sError = L"A part of the file could not be decompressed."; return -1; }
                if ( m_uLeaf != UINT32_MAX )
                {
                    const Node &nd = G->vNodes[m_uLeaf];
                    size_t r = 0;
                    if ( nd.eKind == Node::Raw )
                    {
                        r = (size_t)std::min< uint64_t >( n, nd.n - m_iRawPos );
                        memcpy( p, nd.p + m_iRawPos, r );
                        m_iRawPos += r;
                    }
                    else
                    {
                        r = m_dec->Read( p, n );
                        if ( !r && m_dec->Failed() ) { m_bFailed = true; continue; }
                    }
                    if ( r ) { m_llPos += r; return (int64_t)r; }
                    m_dec.reset();
                    uint32_t id = m_uLeaf;
                    m_uLeaf = UINT32_MAX;
                    Exit( id, m_bLeafEntryB, m_iLeafLog );
                    continue;
                }
                if ( m_vStack.empty() ) return 0;
                Frame &f = m_vStack.back();
                const std::vector< Tok > &vToks = G->vNodes[f.uSeq].vToks;
                if ( f.iTok >= vToks.size() )
                {
                    Frame done = f;
                    m_vStack.pop_back();
                    Exit( done.uNode, done.bEntryB, done.iLogEntry );
                    continue;
                }
                const Tok t = vToks[f.iTok];
                if ( f.llRep >= t.llCount ) { f.iTok++; f.llRep = 0; f.bB = false; continue; }
                if ( m_bBoundary )
                {
                    // Repeats of this part from one chunk boundary to another: the rest of the run, by
                    // whole blocks of them, is those tracks again
                    if ( f.bB && f.llRep > f.llRepB )
                    {
                        uint64_t per = f.llRep - f.llRepB, q = ( t.llCount - f.llRep ) / per;
                        if ( q )
                        {
                            if ( !AddCopies( f.iLogB, m_vLogical.size(), q ) ) return -1;
                            Skipped( t.uNode, q * per );
                            f.llRep += q * per;
                        }
                    }
                    f.bB = true;
                    f.llRepB = f.llRep;
                    f.iLogB = m_vLogical.size();
                    if ( f.llRep >= t.llCount ) continue;
                    // A part read whole elsewhere, from boundary to boundary
                    const Memo &m = m_vMemo[t.uNode];
                    if ( m.a != UINT64_MAX )
                    {
                        if ( !AddCopies( (size_t)m.a, (size_t)m.b, 1 ) ) return -1;
                        Skipped( t.uNode, 1 );
                        f.llRep++;
                        continue;
                    }
                }
                f.llRep++;
                Enter( t.uNode, m_bBoundary );
            }
        }
        bool Skip( uint64_t n )
        {
            std::vector< uint8_t > v( (size_t)std::min< uint64_t >( n, 1 << 20 ) );
            while ( n )
            {
                int64_t r = Pull( v.data(), (size_t)std::min< uint64_t >( n, v.size() ) );
                if ( r < 0 ) return false;
                if ( !r ) return true;
                n -= (uint64_t)r;
            }
            return true;
        }

        HANDLE m_hFile, m_hMap;
        const uint8_t *m_pMap;
        uint64_t m_llFile;
        std::shared_ptr< Grammar > G;
        const std::atomic< bool > *m_pCancel;
        int m_iThreads;
        std::vector< Memo > m_vMemo;
        std::vector< Frame > m_vStack;
        uint32_t m_uLeaf = UINT32_MAX;
        bool m_bLeafEntryB = false;
        size_t m_iLeafLog = 0;
        uint64_t m_iRawPos = 0;
        std::unique_ptr< Decoder > m_dec;
        bool m_bFailed = false;
        bool m_bBoundary = false;           // in NextChunk, before a byte of the chunk header is read
        uint64_t m_llLeft = 0;              // the current chunk's unread data
        uint64_t m_llPos = 0, m_llTotal = 0;
        uint32_t m_uOrdinal = 0;            // MTrk chunks handed out
        std::vector< uint32_t > m_vLogical;
        std::vector< uint8_t > m_vBack;     // PutBack's
        int m_iLogs = 0;
    };
}

std::unique_ptr< Archive::Reader > Dedup::Open( const std::wstring &sPath, bool bAssume, std::atomic< uint64_t > *pDone,
                                                std::atomic< uint64_t > *pTotal, const std::atomic< bool > *pCancel, std::wstring &sError, int iThreads )
{
    sError.clear();
    HANDLE hFile = CreateFileW( sPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr );
    LARGE_INTEGER li = {};
    if ( hFile == INVALID_HANDLE_VALUE || !GetFileSizeEx( hFile, &li ) || li.QuadPart < 16 )
    {
        if ( hFile != INVALID_HANDLE_VALUE ) CloseHandle( hFile );
        return nullptr;
    }
    HANDLE hMap = CreateFileMappingW( hFile, nullptr, PAGE_READONLY, 0, 0, nullptr );
    const uint8_t *pMap = hMap ? (const uint8_t *)MapViewOfFile( hMap, FILE_MAP_READ, 0, 0, 0 ) : nullptr;
    auto Close = [&]() {
        if ( pMap ) UnmapViewOfFile( pMap );
        if ( hMap ) CloseHandle( hMap );
        CloseHandle( hFile );
        return nullptr;
    };
    if ( !pMap || Sniff( pMap, (size_t)std::min< int64_t >( li.QuadPart, 16 ) ) == None ) return Close();
    std::shared_ptr< Grammar > g( new Grammar );
    if ( pTotal ) *pTotal = (uint64_t)li.QuadPart;
    if ( pDone ) *pDone = 0;
    Discovery d( *g, bAssume, pDone, pTotal, pCancel, iThreads );
    LayerBytes file( pMap, (size_t)li.QuadPart );
    uint32_t uRoot = UINT32_MAX;
    if ( !d.Cut( file, 0, Unknown, &uRoot, sError ) ) return Close();   // (sError empty: not members)
    if ( !d.Run( sError ) ) return Close();
    // What it expands to must be a MIDI file (an archive inside, say, is opened as usual)
    {
        DedupReader probe( INVALID_HANDLE_VALUE, nullptr, nullptr, 0, g, uRoot, pCancel );
        uint8_t a[4];
        if ( !probe.Head( a, 4 ) || memcmp( a, "MThd", 4 ) != 0 ) { sError.clear(); return Close(); }
    }
    return std::unique_ptr< Archive::Reader >( new DedupReader( hFile, hMap, pMap, (uint64_t)li.QuadPart, std::move( g ), uRoot, pCancel, iThreads ) );
}
