#include "song/PackedArray.h"

#include <algorithm>

#include "core/ThreadPool.h"

static unsigned BitsFor( uint64_t v )
{
    unsigned n = 0;
    while ( v ) { n++; v >>= 1; }
    return n;
}

void PackedMonotone::Build( size_t n, const std::function< uint64_t( size_t ) > &get, int iThreads )
{
    BuildBlocks( n, [&]( size_t i0, size_t i1, uint64_t *p ) { for ( size_t i = i0; i < i1; i++ ) *p++ = get( i ); }, iThreads );
}

void PackedMonotone::BuildBlocks( size_t n, const std::function< void( size_t, size_t, uint64_t * ) > &fill, int iThreads )
{
    m_n = n;
    size_t iBlocks = ( n + BlockSize - 1 ) / BlockSize;
    std::vector< uint64_t > vBase( iBlocks, 0 ), vBitOffset( iBlocks + 1, 0 );
    std::vector< uint8_t > vWidth( iBlocks, 0 );

    Parallel::For( iThreads, iBlocks, [&]( size_t b, int ) {
        size_t i0 = b * BlockSize, i1 = std::min( n, i0 + BlockSize );
        uint64_t a[BlockSize];
        fill( i0, i1, a );
        vBase[b] = a[0];
        vWidth[b] = (uint8_t)BitsFor( a[i1 - i0 - 1] - a[0] );
    } );
    for ( size_t b = 0; b < iBlocks; b++ )
    {
        size_t iCount = std::min( n - b * BlockSize, BlockSize );
        vBitOffset[b + 1] = vBitOffset[b] + (uint64_t)vWidth[b] * iCount;
    }
    m_vBits.assign( ( vBitOffset[iBlocks] + 63 ) / 64 + 1, 0 );

    // Neighbouring blocks can share a 64-bit word at their edges, so bits are OR-ed in atomically
    Parallel::For( iThreads, iBlocks, [&]( size_t b, int ) {
        unsigned w = vWidth[b];
        if ( !w ) return;
        size_t i0 = b * BlockSize, i1 = std::min( n, i0 + BlockSize );
        uint64_t a[BlockSize];
        fill( i0, i1, a );
        uint64_t llBit = vBitOffset[b];
        for ( size_t i = i0; i < i1; i++, llBit += w )
        {
            uint64_t v = a[i - i0] - vBase[b];
            uint64_t *pWord = &m_vBits[llBit >> 6];
            unsigned s = llBit & 63;
            __atomic_fetch_or( pWord, v << s, __ATOMIC_RELAXED );
            if ( s + w > 64 ) __atomic_fetch_or( pWord + 1, v >> ( 64 - s ), __ATOMIC_RELAXED );
        }
    } );
    m_vBlocks.resize( iBlocks );
    for ( size_t b = 0; b < iBlocks; b++ ) m_vBlocks[b] = { vBase[b], vBitOffset[b] << 8 | vWidth[b] };
}

size_t PackedMonotone::BoundInBlock( size_t b, uint64_t v, bool bStrict ) const
{
    size_t lo = b << BlockShift, hi = std::min( m_n, lo + BlockSize );
    while ( lo < hi )
    {
        size_t mid = lo + ( hi - lo ) / 2;
        uint64_t x = Get( mid );
        if ( bStrict ? x <= v : x < v ) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// The blocks' bases first (one record each), then the one block that can hold the answer
size_t PackedMonotone::LowerBound( uint64_t v ) const
{
    size_t lo = 0, hi = m_vBlocks.size();
    while ( lo < hi )
    {
        size_t mid = lo + ( hi - lo ) / 2;
        if ( m_vBlocks[mid].llBase < v ) lo = mid + 1; else hi = mid;
    }
    return lo ? BoundInBlock( lo - 1, v, false ) : 0;
}

size_t PackedMonotone::LowerBoundFrom( uint64_t v, size_t iHint ) const
{
    // The answer is in (lo, hi]: grow a step outwards from the hint until it brackets it
    if ( !m_n ) return 0;
    size_t lo, hi;
    if ( iHint >= m_n || Get( iHint ) >= v )
    {
        hi = std::min( iHint, m_n );
        size_t step = 1;
        for ( ;; )
        {
            if ( hi < step ) { lo = 0; if ( Get( 0 ) >= v ) return 0; break; }
            lo = hi - step;
            if ( Get( lo ) < v ) break;
            hi = lo;
            step *= 2;
        }
    }
    else
    {
        lo = iHint;
        size_t step = 1;
        for ( ;; )
        {
            hi = lo + step;
            if ( hi >= m_n ) { hi = m_n; break; }
            if ( Get( hi ) >= v ) break;
            lo = hi;
            step *= 2;
        }
    }
    // Get( lo ) < v, and hi == m_n or Get( hi ) >= v
    for ( lo++; lo < hi; )
    {
        size_t mid = lo + ( hi - lo ) / 2;
        if ( Get( mid ) < v ) lo = mid + 1; else hi = mid;
    }
    return lo;
}

size_t PackedMonotone::UpperBound( uint64_t v ) const
{
    size_t lo = 0, hi = m_vBlocks.size();
    while ( lo < hi )
    {
        size_t mid = lo + ( hi - lo ) / 2;
        if ( m_vBlocks[mid].llBase <= v ) lo = mid + 1; else hi = mid;
    }
    return lo ? BoundInBlock( lo - 1, v, true ) : 0;
}

size_t PackedMonotone::MemoryBytes() const
{
    return m_vBlocks.size() * sizeof( Block ) + m_vBits.size() * 8;
}
