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
    m_n = n;
    size_t iBlocks = ( n + BlockSize - 1 ) / BlockSize;
    m_vBase.assign( iBlocks, 0 );
    m_vWidth.assign( iBlocks, 0 );
    m_vBitOffset.assign( iBlocks + 1, 0 );

    Parallel::For( iThreads, iBlocks, [&]( size_t b, int ) {
        size_t i0 = b * BlockSize, i1 = std::min( n, i0 + BlockSize );
        uint64_t llBase = get( i0 ), llLast = get( i1 - 1 );
        m_vBase[b] = llBase;
        m_vWidth[b] = (uint8_t)BitsFor( llLast - llBase );
    } );
    for ( size_t b = 0; b < iBlocks; b++ )
    {
        size_t iCount = std::min( n - b * BlockSize, BlockSize );
        m_vBitOffset[b + 1] = m_vBitOffset[b] + (uint64_t)m_vWidth[b] * iCount;
    }
    m_vBits.assign( ( m_vBitOffset[iBlocks] + 63 ) / 64 + 1, 0 );

    // Neighbouring blocks can share a 64-bit word at their edges, so bits are OR-ed in atomically
    Parallel::For( iThreads, iBlocks, [&]( size_t b, int ) {
        unsigned w = m_vWidth[b];
        if ( !w ) return;
        size_t i0 = b * BlockSize, i1 = std::min( n, i0 + BlockSize );
        uint64_t llBit = m_vBitOffset[b];
        for ( size_t i = i0; i < i1; i++, llBit += w )
        {
            uint64_t v = get( i ) - m_vBase[b];
            uint64_t *pWord = &m_vBits[llBit >> 6];
            unsigned s = llBit & 63;
            __atomic_fetch_or( pWord, v << s, __ATOMIC_RELAXED );
            if ( s + w > 64 ) __atomic_fetch_or( pWord + 1, v >> ( 64 - s ), __ATOMIC_RELAXED );
        }
    } );
}

size_t PackedMonotone::LowerBound( uint64_t v ) const
{
    size_t lo = 0, hi = m_n;
    while ( lo < hi )
    {
        size_t mid = lo + ( hi - lo ) / 2;
        if ( Get( mid ) < v ) lo = mid + 1; else hi = mid;
    }
    return lo;
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
    size_t lo = 0, hi = m_n;
    while ( lo < hi )
    {
        size_t mid = lo + ( hi - lo ) / 2;
        if ( Get( mid ) <= v ) lo = mid + 1; else hi = mid;
    }
    return lo;
}

size_t PackedMonotone::MemoryBytes() const
{
    return m_vBase.size() * 8 + m_vBitOffset.size() * 8 + m_vBits.size() * 8 + m_vWidth.size();
}
