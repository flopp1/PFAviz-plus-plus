#include "song/VisualStore.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/Lz4.h"
#include "core/ThreadPool.h"
#include "core/Util.h"
#include "song/Song.h"

namespace
{
    unsigned Bits( uint64_t v ) { return v ? 64 - __builtin_clzll( v ) : 0; }

    // Codes a[0..n) as base + j * stride + v[j] with v[j] >= 0, choosing the stride (the end-to-end
    // slope, or 0) that needs the fewest bits; returns the width
    unsigned Progression( const uint32_t *a, unsigned n, int64_t &llBase, int32_t &iStride )
    {
        llBase = 0;
        iStride = 0;
        if ( !n ) return 0;
        int64_t llMin = a[0], llMax = a[0];
        for ( unsigned j = 1; j < n; j++ ) { llMin = std::min< int64_t >( llMin, a[j] ); llMax = std::max< int64_t >( llMax, a[j] ); }
        llBase = llMin;
        iStride = 0;
        unsigned w = Bits( (uint64_t)( llMax - llMin ) );
        if ( n < 3 || !w ) return w;
        int64_t s = ( (int64_t)a[n - 1] - a[0] ) / (int64_t)( n - 1 );
        if ( !s || s > INT32_MAX || s < INT32_MIN ) return w;
        int64_t eMin = INT64_MAX, eMax = INT64_MIN;
        for ( unsigned j = 0; j < n; j++ )
        {
            int64_t e = (int64_t)a[j] - (int64_t)j * s;
            eMin = std::min( eMin, e );
            eMax = std::max( eMax, e );
        }
        unsigned w2 = Bits( (uint64_t)( eMax - eMin ) );
        if ( w2 < w ) { llBase = eMin; iStride = (int32_t)s; return w2; }
        return w;
    }
}

//-----------------------------------------------------------------------------
// LZ groups: per-thread caches of decoded groups
//-----------------------------------------------------------------------------

namespace
{
    std::atomic< uint64_t > g_llLzIds{ 0 };
    std::atomic< size_t > g_iLzCacheBytes{ 256u << 20 };

    inline uint64_t ReadVarint( const uint8_t *&p )
    {
        uint64_t v = 0;
        unsigned s = 0, c;
        do { c = *p++; v |= ( uint64_t )( c & 0x7F ) << s; s += 7; } while ( c & 0x80 );
        return v;
    }
    inline uint8_t *WriteVarint( uint8_t *p, uint64_t v )
    {
        while ( v >= 0x80 ) { *p++ = (uint8_t)( v | 0x80 ); v >>= 7; }
        *p++ = (uint8_t)v;
        return p;
    }
}

struct KeyStore::LzCache
{
    struct Entry { uint64_t llGroup = UINT64_MAX, llUse = 0; std::unique_ptr< LzNote[] > p; };
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::vector< Entry > v;
    std::vector< uint8_t > vRaw;
    uint64_t llClock = 0;
    size_t iLast = 0;
};

KeyStore::KeyStore() = default;
KeyStore::~KeyStore() = default;
KeyStore::KeyStore( KeyStore && ) noexcept = default;
KeyStore &KeyStore::operator=( KeyStore && ) noexcept = default;

size_t KeyStore::LzCacheBytes() const
{
    size_t n = 0;
    if ( m_pLzCache ) for ( const LzCache::Entry &e : m_pLzCache->v ) n += e.p ? sizeof( LzNote ) << GroupShift : 0;
    return n;
}

void SetLzCacheBytes( size_t iBytes )
{
    g_iLzCacheBytes = std::max< size_t >( iBytes, 16u << 20 );
}

KeyStore::LzNote KeyStore::LzGet( size_t i ) const
{
    LzCache &c = *m_pLzCache;
    while ( c.lock.test_and_set( std::memory_order_acquire ) ) YieldProcessor();
    uint64_t g = i >> GroupShift;
    size_t e = c.iLast;
    if ( c.v[e].llGroup != g )
    {
        for ( e = 0; e < c.v.size() && c.v[e].llGroup != g; e++ ) {}
        if ( e == c.v.size() )
        {
            // Decode into the least recently used entry
            e = 0;
            for ( size_t k = 1; k < c.v.size(); k++ ) if ( c.v[k].llUse < c.v[e].llUse ) e = k;
            LzCache::Entry &en = c.v[e];
            if ( !en.p ) en.p.reset( new LzNote[size_t( 1 ) << GroupShift] );
            LzDecode( g, en.p.get(), c.vRaw );
            en.llGroup = g;
        }
        c.iLast = e;
    }
    c.v[e].llUse = ++c.llClock;
    LzNote v = c.v[e].p[i & ( ( size_t( 1 ) << GroupShift ) - 1 )];
    c.lock.clear( std::memory_order_release );
    return v;
}

void KeyStore::LzDecode( uint64_t g, LzNote *pOut, std::vector< uint8_t > &vRaw ) const
{
    const LzGroup &lg = m_vLzGroups[g];
    if ( vRaw.size() < lg.uRaw + Lz4::Slack ) vRaw.resize( lg.uRaw + Lz4::Slack );
    if ( lg.uSize == lg.uRaw ) memcpy( vRaw.data(), m_vLzData.data() + lg.llOffset, lg.uRaw );
    else Lz4::Decompress( m_vLzData.data() + lg.llOffset, lg.uSize, vRaw.data(), lg.uRaw );
    size_t n = std::min< size_t >( size_t( 1 ) << GroupShift, m_n - ( g << GroupShift ) );
    const uint8_t *q = vRaw.data();
    uint64_t llStart = lg.llStartBase;
    int64_t llSlot = 0;
    for ( size_t j = 0; j < n; j++ )
    {
        LzNote &v = pOut[j];
        llStart += ReadVarint( q );
        uint64_t z = ReadVarint( q );
        llSlot += (int64_t)( z >> 1 ) ^ -(int64_t)( z & 1 );
        v.llStartU = llStart;
        v.uSlot = (uint32_t)llSlot;
        v.uRank = (uint32_t)ReadVarint( q );
        v.llEndU = llStart + ReadVarint( q );
    }
}

void KeyStore::GetRange( size_t i, size_t n, NoteView *pOut ) const
{
    if ( !m_bLz )
    {
        for ( size_t j = 0; j < n; j++ ) Get( i + j, pOut[j] );
        return;
    }
    // A group decoded per thread, apart from the key's cache
    thread_local std::unique_ptr< LzNote[] > tGroup;
    thread_local std::vector< uint8_t > tRaw;
    thread_local uint64_t tStoreId = 0, tGroupIdx = UINT64_MAX;
    if ( !tGroup ) tGroup.reset( new LzNote[size_t( 1 ) << GroupShift] );
    for ( size_t j = 0; j < n; j++ )
    {
        size_t p = i + j;
        uint64_t g = p >> GroupShift;
        if ( tStoreId != m_llLzId || tGroupIdx != g )
        {
            LzDecode( g, tGroup.get(), tRaw );
            tStoreId = m_llLzId;
            tGroupIdx = g;
        }
        const LzNote &v = tGroup[p & ( ( size_t( 1 ) << GroupShift ) - 1 )];
        NoteView &o = pOut[j];
        o.llStartU = v.llStartU;
        o.llEndU = v.llEndU;
        o.uSlot = v.uSlot;
        o.uRank = v.uRank;
        o.uEndRank = 0;
        o.bUnpaired = false;
        o.bWeird = !m_vWeird.empty() && IsWeird( p );
    }
}

void KeyStore::ToLz()
{
    if ( !m_bCompact || m_bLz ) return;
    size_t nb = Blocks();
    std::vector< uint64_t > vBlockStart( nb );
    for ( size_t b = 0; b < nb; b++ ) vBlockStart[b] = BlockStartBase( b );
    size_t iGroups = ( m_n + ( size_t( 1 ) << GroupShift ) - 1 ) >> GroupShift;
    std::vector< LzGroup > vGroups( iGroups );
    std::vector< uint8_t > vData, vRaw( ( size_t( 1 ) << GroupShift ) * 40 ), vPacked( Lz4::Bound( vRaw.size() ) );
    NoteView n;
    for ( size_t g = 0; g < iGroups; g++ )
    {
        size_t i0 = g << GroupShift, i1 = std::min( m_n, i0 + ( size_t( 1 ) << GroupShift ) );
        uint8_t *q = vRaw.data();
        uint64_t llPrev = StartU( i0 );
        int64_t llSlot = 0;
        vGroups[g].llStartBase = llPrev;
        for ( size_t i = i0; i < i1; i++ )
        {
            Get( i, n );
            q = WriteVarint( q, n.llStartU - llPrev );
            int64_t d = (int64_t)n.uSlot - llSlot;
            q = WriteVarint( q, (uint64_t)( ( d << 1 ) ^ ( d >> 63 ) ) );
            q = WriteVarint( q, n.uRank );
            q = WriteVarint( q, n.llEndU - n.llStartU );
            llPrev = n.llStartU;
            llSlot = n.uSlot;
        }
        size_t iRaw = (size_t)( q - vRaw.data() );
        size_t iSize = Lz4::Compress( vRaw.data(), iRaw, vPacked.data() );
        const uint8_t *pSrc = vPacked.data();
        if ( iSize >= iRaw ) { iSize = iRaw; pSrc = vRaw.data(); }
        vGroups[g].llOffset = vData.size();
        vGroups[g].uSize = (uint32_t)iSize;
        vGroups[g].uRaw = (uint32_t)iRaw;
        vData.insert( vData.end(), pSrc, pSrc + iSize );
    }
    vData.resize( vData.size() + Lz4::Slack );
    vData.shrink_to_fit();
    m_vLzBlockStart.swap( vBlockStart );
    m_vLzGroups.swap( vGroups );
    m_vLzData.swap( vData );
    m_vCBlocks.clear(); m_vCBlocks.shrink_to_fit();
    m_vPalette.clear(); m_vPalette.shrink_to_fit();
    m_vBits.Resize( 0 );
    m_llLzId = ++g_llLzIds;
    // The key's share of the block cache, in decoded groups (at least 4)
    m_pLzCache.reset( new LzCache() );
    size_t iEntries = g_iLzCacheBytes / 128 / ( sizeof( LzNote ) << GroupShift );
    m_pLzCache->v.resize( std::max< size_t >( 4, std::min< size_t >( iEntries, iGroups ) ) );
    m_bLz = true;
}

//-----------------------------------------------------------------------------
// KeyStore: decoding and queries
//-----------------------------------------------------------------------------

void KeyStore::Get( size_t i, NoteView &n ) const
{
    if ( m_bLz )
    {
        LzNote v = LzGet( i );
        n.llStartU = v.llStartU;
        n.llEndU = v.llEndU;
        n.uSlot = v.uSlot;
        n.uRank = v.uRank;
        n.uEndRank = 0;
        n.bUnpaired = false;
        n.bWeird = !m_vWeird.empty() && IsWeird( i );
        return;
    }
    uint64_t j = i & ( BlockSize - 1 );
    if ( m_bCompact )
    {
        const CBlock &c = m_vCBlocks[i >> BlockShift];
        uint64_t llBit = c.llBit + j * (unsigned)( c.wStart + c.wPal + c.wRank + c.wDur );
        uint64_t dS = Read( llBit, c.wStart ); llBit += c.wStart;
        uint64_t uPal = Read( llBit, c.wPal ); llBit += c.wPal;
        uint64_t uRank = Read( llBit, c.wRank ); llBit += c.wRank;
        uint64_t dD = Read( llBit, c.wDur );
        n.llStartU = c.llStartBase + dS;
        n.llEndU = n.llStartU + dD;
        n.uSlot = m_vPalette[c.uPalette + uPal];
        n.uRank = (uint32_t)uRank;
        n.uEndRank = 0;
        n.bUnpaired = false;
        n.bWeird = !m_vWeird.empty() && IsWeird( i );
        return;
    }
    const Block &b = m_vBlocks[i >> BlockShift];
    uint64_t llBit = b.llBitStart + j * b.uStartBits;
    uint64_t dS = Read( llBit, b.wStart ); llBit += b.wStart;
    uint64_t uSlot = Read( llBit, b.wSlot ); llBit += b.wSlot;
    uint64_t uRank = Read( llBit, b.wRank );
    llBit = b.llBitEnd + j * b.uEndBits;
    uint64_t dD = Read( llBit, b.wDur ); llBit += b.wDur;
    uint64_t uEndRank = Read( llBit, b.wEndRank ); llBit += b.wEndRank;
    uint64_t uFlags = Read( llBit, b.wFlags );
    n.llStartU = b.llStartBase + dS;
    n.llEndU = n.llStartU + dD;
    n.uSlot = b.uSlotBase + (uint32_t)uSlot;
    n.uRank = (uint32_t)( b.llRankBase + (int64_t)j * b.iRankStride + (int64_t)uRank );
    n.uEndRank = (uint32_t)( b.llEndRankBase + (int64_t)j * b.iEndRankStride + (int64_t)uEndRank );
    n.bUnpaired = uFlags & 1;
    n.bWeird = ( uFlags >> 1 ) & 1;
}

unsigned KeyStore::GetBlock( size_t b, NoteView *pOut ) const
{
    size_t i0 = b << BlockShift;
    unsigned n = (unsigned)std::min< size_t >( BlockSize, m_n - i0 );
    if ( m_bCompact )
    {
        for ( unsigned j = 0; j < n; j++ ) Get( i0 + j, pOut[j] );
        return n;
    }
    const Block &k = m_vBlocks[b];
    uint64_t llS = k.llBitStart, llE = k.llBitEnd;
    for ( unsigned j = 0; j < n; j++ )
    {
        NoteView &v = pOut[j];
        uint64_t dS = Read( llS, k.wStart ); llS += k.wStart;
        uint64_t uSlot = Read( llS, k.wSlot ); llS += k.wSlot;
        uint64_t uRank = Read( llS, k.wRank ); llS += k.wRank;
        uint64_t dD = Read( llE, k.wDur ); llE += k.wDur;
        uint64_t uEndRank = Read( llE, k.wEndRank ); llE += k.wEndRank;
        uint64_t uFlags = Read( llE, k.wFlags ); llE += k.wFlags;
        v.llStartU = k.llStartBase + dS;
        v.llEndU = v.llStartU + dD;
        v.uSlot = k.uSlotBase + (uint32_t)uSlot;
        v.uRank = (uint32_t)( k.llRankBase + (int64_t)j * k.iRankStride + (int64_t)uRank );
        v.uEndRank = (uint32_t)( k.llEndRankBase + (int64_t)j * k.iEndRankStride + (int64_t)uEndRank );
        v.bUnpaired = uFlags & 1;
        v.bWeird = ( uFlags >> 1 ) & 1;
    }
    return n;
}

uint64_t KeyStore::StartU( size_t i ) const
{
    if ( m_bLz ) return LzGet( i ).llStartU;
    if ( m_bCompact )
    {
        const CBlock &c = m_vCBlocks[i >> BlockShift];
        return c.llStartBase + Read( c.llBit + ( uint64_t )( i & ( BlockSize - 1 ) ) * (unsigned)( c.wStart + c.wPal + c.wRank + c.wDur ), c.wStart );
    }
    const Block &b = m_vBlocks[i >> BlockShift];
    return b.llStartBase + Read( b.llBitStart + ( uint64_t )( i & ( BlockSize - 1 ) ) * b.uStartBits, b.wStart );
}

uint64_t KeyStore::EndU( size_t i ) const
{
    if ( m_bLz ) return LzGet( i ).llEndU;
    uint64_t j = i & ( BlockSize - 1 );
    if ( m_bCompact )
    {
        const CBlock &c = m_vCBlocks[i >> BlockShift];
        uint64_t llBit = c.llBit + j * (unsigned)( c.wStart + c.wPal + c.wRank + c.wDur );
        return c.llStartBase + Read( llBit, c.wStart ) + Read( llBit + c.wStart + c.wPal + c.wRank, c.wDur );
    }
    const Block &b = m_vBlocks[i >> BlockShift];
    return b.llStartBase + Read( b.llBitStart + j * b.uStartBits, b.wStart ) + Read( b.llBitEnd + j * b.uEndBits, b.wDur );
}

size_t KeyStore::LowerBoundStart( uint64_t llU ) const
{
    // First block whose first note starts at or after llU; the answer is in the block before it
    size_t lo = 0, hi = Blocks();
    while ( lo < hi )
    {
        size_t mid = ( lo + hi ) / 2;
        if ( BlockStartBase( mid ) < llU ) lo = mid + 1; else hi = mid;
    }
    if ( lo == 0 ) return 0;
    size_t i0 = ( lo - 1 ) << BlockShift, i1 = std::min( m_n, lo << BlockShift );
    while ( i0 < i1 )
    {
        size_t mid = ( i0 + i1 ) / 2;
        if ( StartU( mid ) < llU ) i0 = mid + 1; else i1 = mid;
    }
    return i0;
}

size_t KeyStore::LowerBoundOrder( uint64_t llU, uint64_t llOrder ) const
{
    size_t i = LowerBoundStart( llU );
    NoteView n;
    for ( ; i < m_n; i++ )
    {
        Get( i, n );
        if ( n.llStartU != llU || OrderKey( n.uSlot, n.uRank ) >= llOrder ) break;
    }
    return i;
}

int64_t KeyStore::ScanEnd( size_t lo, size_t hi, uint64_t llEndU ) const
{
    if ( lo >= hi || BlockMaxEnd( lo >> BlockShift ) < llEndU ) return -1;
    for ( size_t i = hi; i-- > lo; )
        if ( EndU( i ) >= llEndU ) return (int64_t)i;
    return -1;
}

int64_t KeyStore::ScanDur( size_t lo, size_t hi, uint64_t llDurUs, const PackedMonotone &times ) const
{
    if ( lo >= hi || BlockMaxDur( lo >> BlockShift ) < std::min< uint64_t >( llDurUs, UINT32_MAX ) ) return -1;
    NoteView n;
    for ( size_t i = hi; i-- > lo; )
    {
        Get( i, n );
        if ( times.Get( n.llEndU ) - times.Get( n.llStartU ) >= llDurUs ) return (int64_t)i;
    }
    return -1;
}

int64_t KeyStore::TreeEnd( size_t node, size_t nl, size_t nr, size_t a, size_t b, uint64_t x ) const
{
    if ( nr < a || nl > b || m_vTreeEnd[node] < x ) return -1;
    if ( nl == nr ) return (int64_t)nl;
    size_t mid = ( nl + nr ) / 2;
    int64_t r = TreeEnd( node * 2 + 1, mid + 1, nr, a, b, x );
    return r >= 0 ? r : TreeEnd( node * 2, nl, mid, a, b, x );
}

int64_t KeyStore::TreeDur( size_t node, size_t nl, size_t nr, size_t a, size_t b, uint32_t x ) const
{
    if ( nr < a || nl > b || m_vTreeDur[node] < x ) return -1;
    if ( nl == nr ) return (int64_t)nl;
    size_t mid = ( nl + nr ) / 2;
    int64_t r = TreeDur( node * 2 + 1, mid + 1, nr, a, b, x );
    return r >= 0 ? r : TreeDur( node * 2, nl, mid, a, b, x );
}

int64_t KeyStore::RightmostEnd( size_t lo, size_t hi, uint64_t llEndU ) const
{
    hi = std::min( hi, m_n );
    if ( lo >= hi ) return -1;
    size_t bLo = lo >> BlockShift, bHi = ( hi - 1 ) >> BlockShift;
    if ( bLo == bHi ) return ScanEnd( lo, hi, llEndU );
    int64_t r = ScanEnd( bHi << BlockShift, hi, llEndU );
    if ( r >= 0 ) return r;
    if ( bHi - 1 >= bLo + 1 )
    {
        int64_t b = TreeEnd( 1, 0, m_iLeaves - 1, bLo + 1, bHi - 1, llEndU );
        if ( b >= 0 ) return ScanEnd( (size_t)b << BlockShift, ( (size_t)b + 1 ) << BlockShift, llEndU );
    }
    return ScanEnd( lo, ( bLo + 1 ) << BlockShift, llEndU );
}

int64_t KeyStore::RightmostDur( size_t lo, size_t hi, uint64_t llDurUs, const PackedMonotone &times ) const
{
    hi = std::min( hi, m_n );
    if ( lo >= hi ) return -1;
    size_t bLo = lo >> BlockShift, bHi = ( hi - 1 ) >> BlockShift;
    if ( bLo == bHi ) return ScanDur( lo, hi, llDurUs, times );
    int64_t r = ScanDur( bHi << BlockShift, hi, llDurUs, times );
    if ( r >= 0 ) return r;
    uint32_t x = (uint32_t)std::min< uint64_t >( llDurUs, UINT32_MAX );
    // A saturated block maximum can be a false positive for thresholds above 2^32 - 1 us
    for ( size_t bEnd = bHi - 1; bEnd >= bLo + 1; )
    {
        int64_t b = TreeDur( 1, 0, m_iLeaves - 1, bLo + 1, bEnd, x );
        if ( b < 0 ) break;
        r = ScanDur( (size_t)b << BlockShift, ( (size_t)b + 1 ) << BlockShift, llDurUs, times );
        if ( r >= 0 ) return r;
        if ( (size_t)b == bLo + 1 ) break;
        bEnd = (size_t)b - 1;
    }
    return ScanDur( lo, ( bLo + 1 ) << BlockShift, llDurUs, times );
}

bool KeyStore::IsWeird( size_t i ) const
{
    if ( m_vWeird.empty() ) return false;
    size_t b = i >> BlockShift;
    if ( m_bCompact ? !( ( m_vWeirdBlocks[b >> 6] >> ( b & 63 ) ) & 1 ) : !m_vBlocks[b].bWeird ) return false;
    auto it = std::lower_bound( m_vWeird.begin(), m_vWeird.end(), (uint64_t)i, []( const Weird &w, uint64_t p ) { return w.llPos < p; } );
    return it != m_vWeird.end() && it->llPos == i;
}

uint32_t KeyStore::DupCount( size_t i ) const
{
    if ( m_vDupBlocks.empty() ) return 0;
    uint64_t v = m_vDupBlocks[i >> BlockShift];
    unsigned w = (unsigned)( v & 255 );
    return (uint32_t)ReadBits( m_vDupBits.data(), ( v >> 8 ) + ( i & ( BlockSize - 1 ) ) * w, w );
}

uint64_t KeyStore::TickUnpaired( uint64_t llU ) const
{
    auto it = std::lower_bound( m_vDupUnpaired.begin(), m_vDupUnpaired.end(), llU, []( const UnpairedAt &a, uint64_t u ) { return a.llU < u; } );
    return it != m_vDupUnpaired.end() && it->llU == llU ? it->llCount : 0;
}

void KeyStore::TickClasses( uint64_t llU, size_t i0, size_t i1, uint64_t &llCulled, unsigned &uClasses ) const
{
    llCulled = 0;
    uClasses = 0;
    for ( size_t i = i0; i < i1; i++ )
        if ( uint32_t d = DupCount( i ) ) { llCulled += d; uClasses++; }
    if ( !uClasses ) return;    // nothing dropped: no unpaired count is kept either
    uint64_t llUnpaired = TickUnpaired( llU );
    llCulled += llUnpaired;
    uClasses += llUnpaired ? 1 : 0;
}

bool KeyStore::DupCodes( uint64_t llU, size_t i0, size_t i1, uint64_t &llBit, unsigned &uWidth ) const
{
    if ( m_vCodeBase.empty() ) return false;
    // From the offset at the last base position, over the ticks whose kept notes start before i0
    size_t p = i0 >> CodeBaseShift << CodeBaseShift;
    llBit = m_vCodeBase[i0 >> CodeBaseShift];
    if ( p > 0 && p < i0 && StartU( p - 1 ) == StartU( p ) )
        for ( uint64_t u = StartU( p ); p < i0 && StartU( p ) == u; ) p++;   // a tick that started before the base
    uint64_t llCulled;
    unsigned uClasses;
    while ( p < i0 )
    {
        uint64_t u = StartU( p );
        size_t q = p;
        while ( q < i0 && StartU( q ) == u ) q++;
        TickClasses( u, p, q, llCulled, uClasses );
        if ( uClasses > 1 ) llBit += llCulled * Bits( uClasses - 1 );
        p = q;
    }
    TickClasses( llU, i0, i1, llCulled, uClasses );
    if ( uClasses < 2 ) return false;
    uWidth = Bits( uClasses - 1 );
    return true;
}

const OffRef *KeyStore::WeirdOffs( size_t i, uint32_t &uCount ) const
{
    uCount = 0;
    if ( !IsWeird( i ) ) return nullptr;
    auto it = std::lower_bound( m_vWeird.begin(), m_vWeird.end(), (uint64_t)i, []( const Weird &w, uint64_t p ) { return w.llPos < p; } );
    if ( it == m_vWeird.end() || it->llPos != i ) return nullptr;
    uCount = it->uCount;
    return &m_vWeirdOffs[it->uFirst];
}

bool KeyStore::IsUnpaired( size_t i ) const
{
    return std::binary_search( m_vUnpaired.begin(), m_vUnpaired.end(), (uint64_t)i );
}

size_t KeyStore::UnpairedBefore( size_t i ) const
{
    return (size_t)( std::lower_bound( m_vUnpaired.begin(), m_vUnpaired.end(), (uint64_t)i ) - m_vUnpaired.begin() );
}

void KeyStore::BuildTrees( const std::vector< uint64_t > &vMaxEnd, const std::vector< uint32_t > &vMaxDur )
{
    size_t nb = vMaxEnd.size();
    m_iLeaves = 1;
    while ( m_iLeaves < std::max< size_t >( nb, 1 ) ) m_iLeaves *= 2;
    m_vTreeEnd.assign( m_iLeaves * 2, 0 );
    m_vTreeDur.assign( m_iLeaves * 2, 0 );
    for ( size_t b = 0; b < nb; b++ )
    {
        m_vTreeEnd[m_iLeaves + b] = vMaxEnd[b];
        m_vTreeDur[m_iLeaves + b] = vMaxDur[b];
    }
    for ( size_t k = m_iLeaves; k-- > 1; )
    {
        m_vTreeEnd[k] = std::max( m_vTreeEnd[k * 2], m_vTreeEnd[k * 2 + 1] );
        m_vTreeDur[k] = std::max( m_vTreeDur[k * 2], m_vTreeDur[k * 2 + 1] );
    }
}

void KeyStore::BuildFullTrees( const PackedMonotone &times )
{
    std::vector< uint64_t > vMaxEnd( m_vBlocks.size(), 0 );
    std::vector< uint32_t > vMaxDur( m_vBlocks.size(), 0 );
    NoteView a[BlockSize];
    for ( size_t b = 0; b < m_vBlocks.size(); b++ )
    {
        unsigned n = GetBlock( b, a );
        uint64_t llMaxDurUs = 0;
        for ( unsigned j = 0; j < n; j++ )
        {
            vMaxEnd[b] = std::max( vMaxEnd[b], a[j].llEndU );
            if ( a[j].llEndU != a[j].llStartU ) llMaxDurUs = std::max( llMaxDurUs, times.Get( a[j].llEndU ) - times.Get( a[j].llStartU ) );
        }
        vMaxDur[b] = (uint32_t)std::min< uint64_t >( llMaxDurUs, UINT32_MAX );
    }
    BuildTrees( vMaxEnd, vMaxDur );
}

void KeyStore::MemoryParts( size_t a[MemoryPartCount] ) const
{
    a[0] += m_vBlocks.capacity() * sizeof( Block );
    a[1] += m_vCBlocks.capacity() * sizeof( CBlock );
    a[2] += m_vPalette.capacity() * 4;
    a[3] += m_vBits.CommittedBytes();
    a[4] += m_vTreeEnd.capacity() * 8 + m_vTreeDur.capacity() * 4;
    a[5] += m_vWeird.capacity() * sizeof( Weird ) + m_vWeirdOffs.capacity() * sizeof( OffRef ) + m_vUnpaired.capacity() * 8;
    a[6] += m_vLzBlockStart.capacity() * 8 + m_vLzGroups.capacity() * sizeof( LzGroup ) + m_vLzData.capacity();
    a[7] += LzCacheBytes();
    a[8] += DupBytes();
}

size_t KeyStore::DupBytes() const
{
    return m_vWeirdBlocks.capacity() * 8 + m_vDupBlocks.capacity() * 8 + m_vDupBits.capacity() * 8 + m_vCodeBase.capacity() * 8 +
           m_vDupUnpaired.capacity() * sizeof( UnpairedAt ) + m_vDupCodeBits.capacity() * 8;
}

size_t KeyStore::MemoryBytes() const
{
    return m_vBlocks.capacity() * sizeof( Block ) + m_vCBlocks.capacity() * sizeof( CBlock ) + m_vPalette.capacity() * 4 +
           m_vLzBlockStart.capacity() * 8 + m_vLzGroups.capacity() * sizeof( LzGroup ) + m_vLzData.capacity() +
           LzCacheBytes() + DupBytes() +
           m_vBits.CommittedBytes() + m_vTreeEnd.capacity() * 8 + m_vTreeDur.capacity() * 4 +
           m_vWeird.capacity() * sizeof( Weird ) + m_vWeirdOffs.capacity() * sizeof( OffRef ) + m_vUnpaired.capacity() * 8;
}

size_t VisualStore::MemoryBytes() const
{
    size_t n = ( vOnTicks.capacity() + vOnLast.capacity() + vJumpK.capacity() ) * 8;
    for ( const KeyStore &k : aKeys ) n += k.MemoryBytes();
    return n;
}

//-----------------------------------------------------------------------------
// Building: one KeyBuilder per key, owned by one thread
//-----------------------------------------------------------------------------

class KeyBuilder
{
public:
    // iExpected: the key's NoteOns (from the count pass), so every table is sized once.
    // bEndRanks: keep the ordinals of the offs (only the Full layout of songs with weird notes needs them)
    // bKeepRanks: remember each pairing off's ordinal until its block is packed. Needed for the end
    // ordinals and for the offs of a note paired again; a pass that has no weird notes, or that
    // throws itself away on the first, and does not keep end ordinals, does without.
    KeyBuilder( KeyStore &out, const PackedMonotone &times, uint64_t iExpected, bool bEndRanks, bool bKeepRanks = true )
        : m_Out( out ), m_Times( times ), m_bEndRanks( bEndRanks ), m_bKeepRanks( bEndRanks || bKeepRanks )
    {
        size_t nb = (size_t)( iExpected / KeyStore::BlockSize + 1 );
        m_Out.m_vBlocks.reserve( nb );
        m_vResolvedCount.reserve( nb );
        m_vEnd.reserve( nb );
        if ( m_bKeepRanks ) m_vRank.reserve( nb );
        m_vPaired.reserve( nb );
        m_vEarly.reserve( nb );
        m_vLive.reserve( nb );
        m_vResolved.reserve( nb );
        m_Out.m_vBits.Reserve( (size_t)iExpected * 4 + 1024 );   // at most about 230 bits per note (address space only)
    }

    // A NoteOn: its start fields are final at once and packed as soon as its block is full, so a
    // note waiting for its off costs only a few bits until it is paired (a crash can hold a
    // billion notes at once)
    uint64_t Append( uint64_t llU, uint32_t uSlot, uint32_t uRank )
    {
        uint64_t pos = m_n++;
        size_t b = pos >> KeyStore::BlockShift;
        if ( b >= m_nBlocks )
        {
            m_nBlocks++;
            m_vResolvedCount.push_back( 0 );
            m_vEnd.emplace_back();
            if ( m_bKeepRanks ) m_vRank.emplace_back();
            m_vPaired.push_back( 0 );
            m_vEarly.push_back( 0 );
            m_vLive.push_back( 0 );
            m_vResolved.push_back( 0 );
            m_Out.m_vBlocks.emplace_back();
        }
        m_aOpen[pos & 63] = { llU, uSlot, uRank };
        if ( ( pos & 63 ) == 63 ) PackStart( b, 64 );
        return pos;
    }

    // A note paired early (PrePair) is not paired here until its off comes
    bool IsPaired( uint64_t pos ) const { return ( ( m_vPaired[pos >> 6] & ~m_vEarly[pos >> 6] ) >> ( pos & 63 ) ) & 1; }
    // Paired, or paired early: what the block's records say
    bool PairedBit( uint64_t pos ) const { return ( m_vPaired[pos >> 6] >> ( pos & 63 ) ) & 1; }

    // Whether PrePair can be used: not when the offs' ordinals are kept, as the off is not known yet
    bool CanPrePair() const { return !m_bKeepRanks; }

    // A note whose off the count pass found (Song::vKnownEnd), at its NoteOn: final at once, with that
    // end. To the pairing rules it is unpaired until the off comes (IsPaired, Pair). Only for a note
    // paired once (a note paired again would end at its last off)
    void PrePair( uint64_t pos, uint64_t llEndU )
    {
        size_t b = pos >> 6;
        unsigned j = pos & 63;
        EndPending &e = End( b );
        e.aPairs[j] = 1;
        e.SetEndU( j, llEndU );
        m_vPaired[b] |= uint64_t( 1 ) << j;
        m_vEarly[b] |= uint64_t( 1 ) << j;
        Resolve( pos );
    }
    bool IsLive( uint64_t pos ) const { return ( m_vLive[pos >> 6] >> ( pos & 63 ) ) & 1; }
    bool IsResolved( uint64_t pos ) const { return ( m_vResolved[pos >> 6] >> ( pos & 63 ) ) & 1; }
    void SetLive( uint64_t pos, bool bLive )
    {
        uint64_t &w = m_vLive[pos >> 6];
        if ( bLive ) w |= uint64_t( 1 ) << ( pos & 63 );
        else w &= ~( uint64_t( 1 ) << ( pos & 63 ) );
    }

    // Song::bFirstPairingOnly: a note keeps its first pairing (no weird notes)
    void SetFirstPairingOnly( bool b ) { m_bFirstPairingOnly = b; }

    void Pair( uint64_t pos, uint64_t llU, uint32_t uOrd, bool bFromSlot )
    {
        // The off of a note paired early: it is paired now
        if ( ( m_vEarly[pos >> 6] >> ( pos & 63 ) ) & 1 )
        {
            m_vEarly[pos >> 6] &= ~( uint64_t( 1 ) << ( pos & 63 ) );
            if ( bFromSlot ) SetLive( pos, false );
            return;
        }
        EndPending &e = End( pos >> 6 );
        unsigned j = pos & 63;
        if ( m_bFirstPairingOnly && IsPaired( pos ) )
        {
            // The off still takes the note out of its slot; its end stays the first off's
            if ( bFromSlot ) SetLive( pos, false );
            if ( !IsLive( pos ) ) Resolve( pos );
            return;
        }
        // Re-paired (DESIGN.md 6.3): the original's sister moves to this off, its own stack's, and the
        // note ends here. (The original also lets the first off, which another channel's overflowing
        // stack gave it, take it out of the drawn state; here only this one does, so the note is an
        // ordinary note to its drawn end and the song has no weird notes.)
        if ( e.aPairs[j] < 255 ) e.aPairs[j]++;
        e.SetEndU( j, llU );
        SetRank( pos, uOrd );
        m_vPaired[pos >> 6] |= uint64_t( 1 ) << j;
        if ( bFromSlot ) SetLive( pos, false );
        if ( !IsLive( pos ) ) Resolve( pos );
    }

    // Final: paired and out of every stack slot, or its track has ended (unpaired if never paired)
    void Resolve( uint64_t pos )
    {
        if ( IsResolved( pos ) ) return;
        size_t b = pos >> 6;
        m_vResolved[b] |= uint64_t( 1 ) << ( pos & 63 );
        if ( ++m_vResolvedCount[b] == KeyStore::BlockSize && Blk( b ).bStartPacked ) PackEnd( b );
    }

    // A note that is already final (copied from another store)
    void AppendFinal( const NoteView &n, const OffRef *pOffs, uint32_t uOffs )
    {
        uint64_t pos = Append( n.llStartU, n.uSlot, n.uRank );
        EndPending &e = End( pos >> 6 );
        unsigned j = pos & 63;
        e.SetEndU( j, n.llEndU );
        SetRank( pos, n.uEndRank );
        e.aPairs[j] = pOffs && uOffs > 1 ? 2 : 1;
        m_vPaired[pos >> 6] |= uint64_t( 1 ) << j;
        if ( pOffs && uOffs > 1 )
        {
            for ( uint32_t i = 0; i < uOffs; i++ ) m_vWeirdTmp.push_back( { pos, pOffs[i] } );
            m_bWeird = true;
        }
        Resolve( pos );
    }

    void Finish()
    {
        size_t nb = m_nBlocks;
        if ( nb && !Blk( nb - 1 ).bStartPacked ) PackStart( nb - 1, (uint32_t)( m_n - ( ( nb - 1 ) << 6 ) ) );
        // Everything is resolved when every track has ended; anything left is final as it is
        for ( size_t b = 0; b < nb; b++ )
            if ( !Blk( b ).bEndPacked ) PackEnd( b );
        m_vEnd.clear();
        m_vRank.clear(); m_vRank.shrink_to_fit();
        m_vPaired.clear(); m_vPaired.shrink_to_fit();
        m_vEarly.clear(); m_vEarly.shrink_to_fit();
        m_vLive.clear(); m_vLive.shrink_to_fit();
        m_vResolved.clear(); m_vResolved.shrink_to_fit();
        m_vResolvedCount.clear(); m_vResolvedCount.shrink_to_fit();
        m_Out.m_n = m_n;
        m_Out.m_vBits.Resize( ( m_llBits + 63 ) / 64 + 1 );
        m_Out.m_vBlocks.shrink_to_fit();

        std::stable_sort( m_vWeirdTmp.begin(), m_vWeirdTmp.end(), []( const WeirdTmp &a, const WeirdTmp &b ) { return a.llPos < b.llPos; } );
        for ( size_t i = 0; i < m_vWeirdTmp.size(); )
        {
            size_t j = i;
            while ( j < m_vWeirdTmp.size() && m_vWeirdTmp[j].llPos == m_vWeirdTmp[i].llPos ) j++;
            m_Out.m_vWeird.push_back( { m_vWeirdTmp[i].llPos, (uint32_t)m_Out.m_vWeirdOffs.size(), (uint32_t)( j - i ) } );
            for ( size_t k = i; k < j; k++ ) m_Out.m_vWeirdOffs.push_back( m_vWeirdTmp[k].off );
            i = j;
        }
        m_vWeirdTmp.clear();
        m_vWeirdTmp.shrink_to_fit();
        std::sort( m_Out.m_vUnpaired.begin(), m_Out.m_vUnpaired.end() );
    }

    bool Weird() const { return m_bWeird; }

private:
    struct StartRec { uint64_t llStartU; uint32_t uSlot, uRank; };
    // End fields of a block, kept only while it has paired notes and is not complete yet: the end
    // tick in 40 bits (unique tick indices), the pairing count; 384 bytes. The offs' ordinals are
    // apart, only when kept (m_vRank).
    struct EndPending
    {
        uint32_t aEndLo[64];
        uint8_t aEndHi[64];
        uint8_t aPairs[64];
        uint64_t EndU( unsigned j ) const { return aEndLo[j] | ( (uint64_t)aEndHi[j] << 32 ); }
        void SetEndU( unsigned j, uint64_t llU ) { aEndLo[j] = (uint32_t)llU; aEndHi[j] = (uint8_t)( llU >> 32 ); }
    };
    uint32_t RankOf( uint64_t pos ) const
    {
        if ( !m_bKeepRanks ) return 0;
        const std::unique_ptr< uint32_t[] > &p = m_vRank[pos >> 6];
        return p ? p[pos & 63] : 0;
    }
    void SetRank( uint64_t pos, uint32_t uOrd )
    {
        if ( !m_bKeepRanks ) return;
        std::unique_ptr< uint32_t[] > &p = m_vRank[pos >> 6];
        if ( !p ) p.reset( new uint32_t[64]() );
        p[pos & 63] = uOrd;
    }
    struct WeirdTmp { uint64_t llPos; OffRef off; };

    KeyStore::Block &Blk( size_t b ) { return m_Out.m_vBlocks[b]; }

    EndPending &End( size_t b )
    {
        std::unique_ptr< EndPending > &p = m_vEnd[b];
        if ( !p ) p.reset( new EndPending() );
        return *p;
    }

    uint64_t Reserve( uint64_t llBits )
    {
        uint64_t llAt = m_llBits;
        m_llBits += llBits;
        uint64_t llNeed = ( m_llBits + 63 ) / 64 + 1;
        if ( m_Out.m_vBits.size() < llNeed ) m_Out.m_vBits.Resize( (size_t)llNeed, true );
        return llAt;
    }

    void Write( uint64_t llBit, unsigned w, uint64_t v )
    {
        if ( !w ) return;
        uint64_t *p = &m_Out.m_vBits[llBit >> 6];
        unsigned s = llBit & 63;
        p[0] |= v << s;
        if ( s + w > 64 ) p[1] |= v >> ( 64 - s );
    }

    // A full block's starts (n < 64: the last block)
    void PackStart( size_t b, uint32_t n )
    {
        KeyStore::Block &blk = Blk( b );
        const StartRec *pOpen = m_aOpen;
        uint64_t llBase = pOpen[0].llStartU, llMaxDS = 0;
        uint32_t uSlotMin = UINT32_MAX, uSlotMax = 0, aRank[64];
        for ( uint32_t j = 0; j < n; j++ )
        {
            llMaxDS = std::max( llMaxDS, pOpen[j].llStartU - llBase );
            uSlotMin = std::min( uSlotMin, pOpen[j].uSlot );
            uSlotMax = std::max( uSlotMax, pOpen[j].uSlot );
            aRank[j] = pOpen[j].uRank;
        }
        blk.llStartBase = llBase;
        blk.uSlotBase = uSlotMin;
        blk.uCount = (uint8_t)n;
        blk.wStart = (uint8_t)Bits( llMaxDS );
        blk.wSlot = (uint8_t)Bits( uSlotMax - uSlotMin );
        blk.wRank = (uint8_t)Progression( aRank, n, blk.llRankBase, blk.iRankStride );
        blk.uStartBits = (uint8_t)( blk.wStart + blk.wSlot + blk.wRank );
        uint64_t llBit = blk.llBitStart = Reserve( (uint64_t)n * blk.uStartBits );
        for ( uint32_t j = 0; j < n; j++ )
        {
            Write( llBit, blk.wStart, pOpen[j].llStartU - llBase ); llBit += blk.wStart;
            Write( llBit, blk.wSlot, pOpen[j].uSlot - uSlotMin ); llBit += blk.wSlot;
            Write( llBit, blk.wRank, (uint64_t)( (int64_t)pOpen[j].uRank - blk.llRankBase - (int64_t)j * blk.iRankStride ) ); llBit += blk.wRank;
        }
        blk.bStartPacked = 1;
        if ( m_vResolvedCount[b] == KeyStore::BlockSize ) PackEnd( b );
    }

    // Every note of block b is final
    void PackEnd( size_t b )
    {
        KeyStore::Block &blk = Blk( b );
        uint32_t n = blk.uCount;
        EndPending *pE = m_vEnd[b].get();
        uint64_t aEnd[64];
        uint32_t aEndRank[64] = {};
        uint8_t aFlags[64];
        uint64_t llMaxDur = 0;
        bool bUnpaired = false, bWeird = false;
        for ( uint32_t j = 0; j < n; j++ )
        {
            uint64_t pos = ( (uint64_t)b << 6 ) + j;
            uint64_t llStart = blk.llStartBase + m_Out.Read( blk.llBitStart + (uint64_t)j * blk.uStartBits, blk.wStart );
            bool bPaired = PairedBit( pos ) && pE;
            aEnd[j] = bPaired ? pE->EndU( j ) : llStart;
            aEndRank[j] = bPaired ? RankOf( pos ) : 0;
            aFlags[j] = ( bPaired ? 0 : 1 ) | ( bPaired && pE->aPairs[j] > 1 ? 2 : 0 );
            if ( !bPaired ) m_Out.m_vUnpaired.push_back( pos );
            bUnpaired |= !bPaired;
            bWeird |= ( aFlags[j] & 2 ) != 0;
            llMaxDur = std::max( llMaxDur, aEnd[j] - llStart );
            aEnd[j] -= llStart;
        }
        blk.wDur = (uint8_t)Bits( llMaxDur );
        blk.wEndRank = m_bEndRanks ? (uint8_t)Progression( aEndRank, n, blk.llEndRankBase, blk.iEndRankStride ) : 0;
        blk.wFlags = bWeird ? 2 : bUnpaired ? 1 : 0;
        blk.uEndBits = (uint8_t)( blk.wDur + blk.wEndRank + blk.wFlags );
        blk.bWeird = bWeird;
        uint64_t llBit = blk.llBitEnd = Reserve( (uint64_t)n * blk.uEndBits );
        for ( uint32_t j = 0; j < n; j++ )
        {
            Write( llBit, blk.wDur, aEnd[j] ); llBit += blk.wDur;
            Write( llBit, blk.wEndRank, (uint64_t)( (int64_t)aEndRank[j] - blk.llEndRankBase - (int64_t)j * blk.iEndRankStride ) ); llBit += blk.wEndRank;
            Write( llBit, blk.wFlags, aFlags[j] ); llBit += blk.wFlags;
        }
        blk.bEndPacked = 1;
        m_vEnd[b].reset();
        if ( m_bKeepRanks ) m_vRank[b].reset();
    }

    KeyStore &m_Out;
    const PackedMonotone &m_Times;
    uint64_t m_n = 0, m_llBits = 0;
    StartRec m_aOpen[64];
    size_t m_nBlocks = 0;
    // Per block
    std::vector< uint8_t > m_vResolvedCount;
    std::vector< std::unique_ptr< EndPending > > m_vEnd;    // while needed
    std::vector< std::unique_ptr< uint32_t[] > > m_vRank;   // with m_bKeepRanks, while needed
    std::vector< uint64_t > m_vPaired, m_vLive, m_vResolved; // one word per block
    std::vector< uint64_t > m_vEarly;                       // per block: paired early, its off still to come
    std::vector< WeirdTmp > m_vWeirdTmp;
    bool m_bWeird = false;
    bool m_bFirstPairingOnly = false;
    bool m_bEndRanks = true, m_bKeepRanks = true;
};

//-----------------------------------------------------------------------------
// The Compact layout: culled notes in records of four fields
//-----------------------------------------------------------------------------

class CompactBuilder
{
public:
    // bDups: keep the dropped-duplicate counts and class codes (songs with weird notes, for the jump walk)
    CompactBuilder( KeyStore &out, const PackedMonotone &times, size_t iMaxNotes, bool bDups ) : m_Out( out ), m_Times( times ), m_bDups( bDups )
    {
        m_Out.m_vBits.Reserve( iMaxNotes * 3 + 1024 );     // at most about 170 bits per note
    }

    void Add( uint64_t llStartU, uint64_t llEndU, uint32_t uSlot, uint32_t uRank, uint32_t uDup = 0 )
    {
        m_a[m_k++] = { llStartU, llEndU, uSlot, uRank, uDup };
        if ( m_k == KeyStore::BlockSize ) Flush();
    }

    // A weird note, with every off that paired it
    void AddWeird( const NoteView &n, const OffRef *pOffs, uint32_t uOffs )
    {
        m_Out.m_vWeird.push_back( { m_n + m_k, (uint32_t)m_Out.m_vWeirdOffs.size(), uOffs } );
        m_Out.m_vWeirdOffs.insert( m_Out.m_vWeirdOffs.end(), pOffs, pOffs + uOffs );
        Add( n.llStartU, n.llEndU, n.uSlot, n.uRank );
    }

    // Before the kept notes of a start tick (the code offsets, KeyStore::DupCodes)
    void BeginTick()
    {
        if ( !m_bDups ) return;
        uint64_t llPos = m_n + m_k;
        while ( ( (uint64_t)m_Out.m_vCodeBase.size() << KeyStore::CodeBaseShift ) <= llPos ) m_Out.m_vCodeBase.push_back( m_llCodeBits );
    }

    // The unpaired NoteOns of a start tick with dropped notes
    void AddUnpaired( uint64_t llU, uint64_t llCount ) { m_Out.m_vDupUnpaired.push_back( { llU, llCount } ); }

    // The class of every culled NoteOn of the start tick, in event order (KeyStore::DupCodes)
    void AddCodes( const std::vector< uint32_t > &vCodes, unsigned uWidth )
    {
        for ( uint32_t c : vCodes ) { WriteTo( m_Out.m_vDupCodeBits, m_llCodeBits, uWidth, c ); m_llCodeBits += uWidth; }
    }

    void Finish()
    {
        if ( m_k ) Flush();
        m_Out.m_bCompact = true;
        m_Out.m_n = m_n;
        m_Out.m_vBits.Resize( ( m_llBits + 63 ) / 64 + 1 );
        m_Out.m_vCBlocks.shrink_to_fit();
        m_Out.m_vPalette.shrink_to_fit();
        m_Out.BuildTrees( m_vMaxEnd, m_vMaxDur );
        if ( !m_Out.m_vWeird.empty() )
        {
            m_Out.m_vWeirdBlocks.assign( ( m_Out.m_vCBlocks.size() + 63 ) / 64, 0 );
            for ( const KeyStore::Weird &w : m_Out.m_vWeird )
            {
                size_t b = w.llPos >> KeyStore::BlockShift;
                m_Out.m_vWeirdBlocks[b >> 6] |= uint64_t( 1 ) << ( b & 63 );
            }
        }
        m_Out.m_vWeird.shrink_to_fit();
        m_Out.m_vWeirdOffs.shrink_to_fit();
        if ( m_bDups )
        {
            // One word of slack after each bit array (ReadBits may touch the next word)
            m_Out.m_vDupBits.resize( ( m_llDupBits + 63 ) / 64 + 1 );
            m_Out.m_vDupCodeBits.resize( ( m_llCodeBits + 63 ) / 64 + 1 );
            m_Out.m_vDupBlocks.shrink_to_fit();
            m_Out.m_vDupBits.shrink_to_fit();
            while ( ( (uint64_t)m_Out.m_vCodeBase.size() << KeyStore::CodeBaseShift ) < m_n ) m_Out.m_vCodeBase.push_back( m_llCodeBits );
            m_Out.m_vCodeBase.shrink_to_fit();
            m_Out.m_vDupUnpaired.shrink_to_fit();
            m_Out.m_vDupCodeBits.shrink_to_fit();
        }
    }

private:
    struct Rec { uint64_t llStartU, llEndU; uint32_t uSlot, uRank, uDup; };

    void Flush()
    {
        KeyStore::CBlock c;
        c.llStartBase = m_a[0].llStartU;
        uint32_t aSlots[KeyStore::BlockSize];
        uint64_t llMaxDS = 0, llMaxDur = 0, llMaxEnd = 0, llMaxDurUs = 0;
        uint32_t uMaxRank = 0, uMaxDup = 0;
        // (notes of a block mostly share their start and end: each is looked up once in a row)
        uint64_t llStartAt = UINT64_MAX, llEndAt = UINT64_MAX, llStartUs = 0, llEndUs = 0;
        for ( unsigned j = 0; j < m_k; j++ )
        {
            const Rec &r = m_a[j];
            llMaxDS = std::max( llMaxDS, r.llStartU - c.llStartBase );
            llMaxDur = std::max( llMaxDur, r.llEndU - r.llStartU );
            llMaxEnd = std::max( llMaxEnd, r.llEndU );
            uMaxRank = std::max( uMaxRank, r.uRank );
            uMaxDup = std::max( uMaxDup, r.uDup );
            if ( r.llEndU != r.llStartU )
            {
                if ( r.llStartU != llStartAt ) { llStartAt = r.llStartU; llStartUs = m_Times.Get( llStartAt ); }
                if ( r.llEndU != llEndAt ) { llEndAt = r.llEndU; llEndUs = m_Times.Get( llEndAt ); }
                llMaxDurUs = std::max( llMaxDurUs, llEndUs - llStartUs );
            }
        }
        // The palette: the block's distinct slots, sorted (notes come in runs of one slot, and the runs
        // of a tick in track order: a slot is looked up once per run, and mostly goes at the end)
        unsigned iPal = 0;
        for ( unsigned j = 0, uLast = UINT32_MAX; j < m_k; j++ )
        {
            uint32_t uSlot = m_a[j].uSlot;
            if ( uSlot == uLast ) continue;
            uLast = uSlot;
            if ( !iPal || uSlot > aSlots[iPal - 1] ) { aSlots[iPal++] = uSlot; continue; }
            uint32_t *pAt = std::lower_bound( aSlots, aSlots + iPal, uSlot );
            if ( pAt < aSlots + iPal && *pAt == uSlot ) continue;
            memmove( pAt + 1, pAt, ( aSlots + iPal - pAt ) * sizeof( uint32_t ) );
            *pAt = uSlot;
            iPal++;
        }
        c.uPalette = (uint32_t)m_Out.m_vPalette.size();
        m_Out.m_vPalette.insert( m_Out.m_vPalette.end(), aSlots, aSlots + iPal );
        c.wStart = (uint8_t)Bits( llMaxDS );
        c.wPal = (uint8_t)Bits( iPal - 1 );
        c.wRank = (uint8_t)Bits( uMaxRank );
        c.wDur = (uint8_t)Bits( llMaxDur );
        unsigned w = c.wStart + c.wPal + c.wRank + c.wDur;
        c.llBit = m_llBits;
        m_llBits += (uint64_t)m_k * w;
        uint64_t llNeed = ( m_llBits + 63 ) / 64 + 1;
        if ( m_Out.m_vBits.size() < llNeed ) m_Out.m_vBits.Resize( (size_t)llNeed, true );
        uint64_t llBit = c.llBit;
        uint32_t uPalSlot = UINT32_MAX, uPal = 0;
        for ( unsigned j = 0; j < m_k; j++ )
        {
            const Rec &r = m_a[j];
            if ( r.uSlot != uPalSlot )
            {
                uPalSlot = r.uSlot;
                if ( uPal + 1 < iPal && aSlots[uPal + 1] == uPalSlot ) uPal++;    // (the next run's slot, usually)
                else uPal = (uint32_t)( std::lower_bound( aSlots, aSlots + iPal, uPalSlot ) - aSlots );
            }
            Write( llBit, c.wStart, r.llStartU - c.llStartBase ); llBit += c.wStart;
            Write( llBit, c.wPal, uPal ); llBit += c.wPal;
            Write( llBit, c.wRank, r.uRank ); llBit += c.wRank;
            Write( llBit, c.wDur, r.llEndU - r.llStartU ); llBit += c.wDur;
        }
        if ( m_bDups )
        {
            unsigned wDup = Bits( uMaxDup );
            m_Out.m_vDupBlocks.push_back( m_llDupBits << 8 | wDup );
            for ( unsigned j = 0; j < m_k; j++ ) { WriteTo( m_Out.m_vDupBits, m_llDupBits, wDup, m_a[j].uDup ); m_llDupBits += wDup; }
        }
        m_Out.m_vCBlocks.push_back( c );
        m_vMaxEnd.push_back( llMaxEnd );
        m_vMaxDur.push_back( (uint32_t)std::min< uint64_t >( llMaxDurUs, UINT32_MAX ) );
        m_n += m_k;
        m_k = 0;
    }

    void Write( uint64_t llBit, unsigned w, uint64_t v )
    {
        if ( !w ) return;
        uint64_t *p = &m_Out.m_vBits[llBit >> 6];
        unsigned s = llBit & 63;
        p[0] |= v << s;
        if ( s + w > 64 ) p[1] |= v >> ( 64 - s );
    }

    static void WriteTo( std::vector< uint64_t > &v, uint64_t llBit, unsigned w, uint64_t x )
    {
        if ( !w ) return;
        size_t iNeed = (size_t)( ( llBit + w + 63 ) / 64 );
        if ( v.size() < iNeed ) v.resize( std::max( iNeed, v.size() + v.size() / 2 ), 0 );
        unsigned s = llBit & 63;
        v[llBit >> 6] |= x << s;
        if ( s + w > 64 ) v[( llBit >> 6 ) + 1] |= x >> ( 64 - s );
    }

    KeyStore &m_Out;
    const PackedMonotone &m_Times;
    bool m_bDups;
    Rec m_a[KeyStore::BlockSize];
    unsigned m_k = 0;
    uint64_t m_n = 0, m_llBits = 0, m_llDupBits = 0, m_llCodeBits = 0;
    std::vector< uint64_t > m_vMaxEnd;
    std::vector< uint32_t > m_vMaxDur;
};

// CompactKey for a key whose Full blocks arrive one at a time, in order, and are freed after (no
// weird notes, no duplicate data): per same-start group, the last position of each distinct end
// with that note's slot and ordinal, so the group needs nothing from the Full layout when it is
// flushed. The kept notes, their order and their blocks are exactly those of CompactKey.
class StreamCompactor
{
public:
    StreamCompactor( KeyStore &out, const PackedMonotone &times, size_t iMaxNotes, const std::vector< uint8_t > *pHidden )
        : m_cb( out, times, iMaxNotes, false ), m_pHidden( pHidden ) {}

    // A paired note (unpaired ones are not fed)
    void Feed( uint64_t llStartU, uint64_t llEndU, uint32_t uSlot, uint32_t uRank, uint64_t pos )
    {
        if ( llStartU != m_llGroupStart )
        {
            Flush();
            m_llGroupStart = llStartU;
        }
        if ( m_pHidden && uSlot < m_pHidden->size() && ( *m_pHidden )[uSlot] ) return;
        End &e = Find( llEndU );
        e.llLast = pos;
        e.uSlot = uSlot;
        e.uRank = uRank;
    }

    void Finish() { Flush(); m_cb.Finish(); }

private:
    struct End { uint64_t llEnd, llLast; uint32_t uSlot, uRank; };

    End &Find( uint64_t e )
    {
        if ( m_iLast < m_vEnds.size() && m_vEnds[m_iLast].llEnd == e ) return m_vEnds[m_iLast];
        size_t k = m_vEnds.size();
        if ( !m_mapEnds.empty() )
        {
            auto it = m_mapEnds.find( e );
            if ( it != m_mapEnds.end() ) k = it->second;
        }
        else
            for ( size_t q = 0; q < m_vEnds.size(); q++ )
                if ( m_vEnds[q].llEnd == e ) { k = q; break; }
        if ( k == m_vEnds.size() )
        {
            m_vEnds.push_back( { e, 0, 0, 0 } );
            if ( !m_mapEnds.empty() ) m_mapEnds.emplace( e, (uint32_t)k );
            else if ( m_vEnds.size() > 32 )
                for ( size_t q = 0; q < m_vEnds.size(); q++ ) m_mapEnds.emplace( m_vEnds[q].llEnd, (uint32_t)q );
        }
        m_iLast = k;
        return m_vEnds[k];
    }

    void Flush()
    {
        if ( m_vEnds.size() > 1 ) std::sort( m_vEnds.begin(), m_vEnds.end(), []( const End &a, const End &b ) { return a.llLast < b.llLast; } );
        for ( const End &e : m_vEnds ) m_cb.Add( m_llGroupStart, e.llEnd, e.uSlot, e.uRank );
        m_vEnds.clear();
        if ( !m_mapEnds.empty() ) m_mapEnds.clear();    // (clear() zeroes every bucket, even of an empty map)
        m_iLast = 0;
    }

    CompactBuilder m_cb;
    const std::vector< uint8_t > *m_pHidden;
    uint64_t m_llGroupStart = UINT64_MAX;
    std::vector< End > m_vEnds;
    std::unordered_map< uint64_t, uint32_t > m_mapEnds;
    size_t m_iLast = 0;
};

// The key builder of a pass that compacts while pairing (no weird notes, no end ordinals), for songs
// whose unique ticks fit 32 bits. A note is a plain record until it is final, and each block of 64 goes
// to the compactor as soon as it and every block before it are final. The newest blocks are records in
// a ring, grown to at most RingCap blocks; a block that leaves the ring before it is fed has its start
// fields packed and its ends kept apart (a note held a while can keep tens of millions of later notes
// of its key waiting). The pairing rules' bookkeeping is KeyBuilder's, one record per block.
class StreamKey
{
public:
    StreamKey( StreamCompactor &sc, uint64_t iExpected ) : m_Stream( sc )
    {
        m_vBits.Reserve( (size_t)iExpected * 3 / 2 + 1024 );    // at most 96 bits per note (address space only)
    }

    bool CanPrePair() const { return true; }
    void SetFirstPairingOnly( bool b ) { m_bFirstPairingOnly = b; }

    uint64_t Append( uint64_t llU, uint32_t uSlot, uint32_t uRank )
    {
        uint64_t pos = m_n++;
        if ( !( pos & 63 ) ) NewBlock();
        m_pOpen[pos & 63] = { (uint32_t)llU, (uint32_t)llU, uSlot, uRank };
        return pos;
    }

    // Blocks before m_bBase were fed and dropped: every note there is final, and paired unless it was
    // paired early and its off is still to come
    bool IsPaired( uint64_t pos ) const
    {
        size_t b = pos >> 6;
        if ( b < m_bBase ) return m_setEarly.empty() || !m_setEarly.count( pos );
        const Meta &m = m_vMeta[b - m_bBase];
        return ( ( m.paired & ~m.early ) >> ( pos & 63 ) ) & 1;
    }
    void PrePair( uint64_t pos, uint64_t llEndU )
    {
        Meta &m = m_vMeta[( pos >> 6 ) - m_bBase];
        SetEnd( pos, llEndU );
        m.paired |= Bit( pos );
        m.early |= Bit( pos );
        Resolve( pos );
    }
    void SetLive( uint64_t pos, bool bLive )
    {
        size_t b = pos >> 6;
        if ( b < m_bBase ) return;
        uint64_t &w = m_vMeta[b - m_bBase].live;
        w = bLive ? w | Bit( pos ) : w & ~Bit( pos );
    }
    // As KeyBuilder::Pair. A note before m_bBase is final, so out of every slot: only the off of a
    // note paired early comes for it
    void Pair( uint64_t pos, uint64_t llU, uint32_t, bool bFromSlot )
    {
        size_t b = pos >> 6;
        if ( b < m_bBase ) { if ( !m_setEarly.empty() ) m_setEarly.erase( pos ); return; }
        Meta &m = m_vMeta[b - m_bBase];
        uint64_t bit = Bit( pos );
        if ( bFromSlot ) m.live &= ~bit;
        if ( m.early & bit ) { m.early &= ~bit; return; }
        if ( !m_bFirstPairingOnly || !( m.paired & bit ) )
        {
            SetEnd( pos, llU );
            m.paired |= bit;
        }
        if ( !( m.live & bit ) ) Resolve( pos );
    }
    void Resolve( uint64_t pos )
    {
        size_t b = pos >> 6;
        if ( b < m_bBase ) return;
        Meta &m = m_vMeta[b - m_bBase];
        if ( m.resolved & Bit( pos ) ) return;
        m.resolved |= Bit( pos );
        if ( m.resolved == ~uint64_t( 0 ) && b == m_bFed ) Advance();
    }

    // Every track has ended: what is left is final as it is
    void Finish()
    {
        for ( ; m_bFed < m_nBlocks; m_bFed++ ) FeedBlock( m_bFed, (unsigned)std::min< uint64_t >( 64, m_n - ( (uint64_t)m_bFed << 6 ) ) );
        m_Stream.Finish();
        std::vector< Meta >().swap( m_vMeta );
        std::deque< Packed >().swap( m_dqPacked );
        m_pRing.reset();
        std::unordered_set< uint64_t >().swap( m_setEarly );
        m_vBits = WordArena();
    }

    uint64_t FullSize() const { return m_n; }
    uint64_t UnpairedCount() const { return m_llUnpaired; }
    uint64_t FirstPairedStartU() const { return m_llFirstPairedU; }    // UINT64_MAX: none

private:
    struct Rec { uint32_t uStart, uEnd, uSlot, uRank; };      // the end is the start until paired
    struct Meta { uint64_t paired, early, live, resolved; };  // per note of the block, as KeyBuilder's
    // A block that left the ring unfed: starts, slots and ordinals packed from llBit (as KeyStore::Block);
    // the ends in pEnd, or, when every note was final as it left (a block waiting behind the frontier
    // usually is), as durations of wDur bits from llEndBit
    struct Packed
    {
        uint64_t llBit = 0, llEndBit = 0;
        uint32_t uStartBase = 0, uSlotBase = 0;
        int64_t llRankBase = 0;
        int32_t iRankStride = 0;
        uint8_t wStart = 0, wSlot = 0, wRank = 0, wDur = 0;
        std::unique_ptr< uint32_t[] > pEnd;
    };
    static const size_t RingCap = 256;
    static uint64_t Bit( uint64_t pos ) { return uint64_t( 1 ) << ( pos & 63 ); }
    Rec *Page( size_t b ) const { return &m_pRing[( b & ( m_iRing - 1 ) ) << 6]; }

    void SetEnd( uint64_t pos, uint64_t llU )
    {
        size_t b = pos >> 6;
        if ( b >= m_bRawFrom ) Page( b )[pos & 63].uEnd = (uint32_t)llU;
        else m_dqPacked[b - m_bPackedFrom].pEnd[pos & 63] = (uint32_t)llU;
    }

    // The ring holds blocks m_bRawFrom on (none of them fed); the oldest packed block is m_bPackedFrom,
    // and the packed ones are the blocks between it and m_bRawFrom
    void NewBlock()
    {
        size_t b = m_nBlocks++;
        m_vMeta.push_back( {} );
        if ( b - m_bRawFrom >= m_iRing )
        {
            if ( m_iRing < RingCap ) GrowRing();
            else Evict();
        }
        m_pOpen = Page( b );
    }
    void GrowRing()
    {
        size_t iRing = m_iRing ? m_iRing * 2 : 4;
        std::unique_ptr< Rec[] > p( new Rec[iRing << 6] );
        for ( size_t x = m_bRawFrom; x + 1 < m_nBlocks; x++ ) memcpy( &p[( x & ( iRing - 1 ) ) << 6], Page( x ), 64 * sizeof( Rec ) );
        m_pRing = std::move( p );
        m_iRing = iRing;
    }
    void Evict()
    {
        size_t b = m_bRawFrom++;
        if ( m_dqPacked.empty() ) m_bPackedFrom = b;
        m_dqPacked.emplace_back();
        Packed &k = m_dqPacked.back();
        const Rec *p = Page( b );
        uint32_t uSlotMin = UINT32_MAX, uSlotMax = 0, aRank[64];
        for ( unsigned j = 0; j < 64; j++ )
        {
            uSlotMin = std::min( uSlotMin, p[j].uSlot );
            uSlotMax = std::max( uSlotMax, p[j].uSlot );
            aRank[j] = p[j].uRank;
        }
        k.uStartBase = p[0].uStart;
        k.uSlotBase = uSlotMin;
        k.wStart = (uint8_t)Bits( p[63].uStart - p[0].uStart );     // (a key's starts only grow)
        k.wSlot = (uint8_t)Bits( uSlotMax - uSlotMin );
        k.wRank = (uint8_t)Progression( aRank, 64, k.llRankBase, k.iRankStride );
        unsigned w = k.wStart + k.wSlot + k.wRank;
        uint64_t llBit = k.llBit = Alloc( 64 * w );
        for ( unsigned j = 0; j < 64; j++ )
        {
            Write( llBit, k.wStart, p[j].uStart - k.uStartBase ); llBit += k.wStart;
            Write( llBit, k.wSlot, p[j].uSlot - uSlotMin ); llBit += k.wSlot;
            Write( llBit, k.wRank, (uint64_t)( (int64_t)p[j].uRank - k.llRankBase - (int64_t)j * k.iRankStride ) ); llBit += k.wRank;
        }
        uint32_t aStart[64], aEnd[64];
        for ( unsigned j = 0; j < 64; j++ ) { aStart[j] = p[j].uStart; aEnd[j] = p[j].uEnd; }
        if ( m_vMeta[b - m_bBase].resolved == ~uint64_t( 0 ) ) PackEnds( k, aStart, aEnd );
        else
        {
            k.pEnd.reset( new uint32_t[64] );
            memcpy( k.pEnd.get(), aEnd, sizeof( aEnd ) );
        }
    }

    uint64_t Alloc( uint64_t llBits )
    {
        uint64_t llAt = m_llBits;
        m_llBits += llBits;
        uint64_t llNeed = ( m_llBits + 63 ) / 64 + 1;
        if ( m_vBits.size() < llNeed ) m_vBits.Resize( (size_t)llNeed, true );
        return llAt;
    }
    void PackEnds( Packed &k, const uint32_t *pStart, const uint32_t *pEnd )
    {
        uint32_t uMaxDur = 0;
        for ( unsigned j = 0; j < 64; j++ ) uMaxDur = std::max( uMaxDur, pEnd[j] - pStart[j] );
        k.wDur = (uint8_t)Bits( uMaxDur );
        uint64_t llBit = k.llEndBit = Alloc( 64 * k.wDur );
        for ( unsigned j = 0; j < 64; j++, llBit += k.wDur ) Write( llBit, k.wDur, pEnd[j] - pStart[j] );
    }

    void Write( uint64_t llBit, unsigned w, uint64_t v )
    {
        if ( !w ) return;
        uint64_t *p = &m_vBits[llBit >> 6];
        unsigned s = llBit & 63;
        p[0] |= v << s;
        if ( s + w > 64 ) p[1] |= v >> ( 64 - s );
    }
    uint64_t Read( uint64_t llBit, unsigned w ) const
    {
        if ( !w ) return 0;
        const uint64_t *p = &m_vBits[llBit >> 6];
        unsigned s = llBit & 63;
        uint64_t v = p[0] >> s;
        if ( s + w > 64 ) v |= p[1] << ( 64 - s );
        return w == 64 ? v : v & ( ( uint64_t( 1 ) << w ) - 1 );
    }

    void FeedBlock( size_t b, unsigned n )
    {
        uint64_t llPaired = m_vMeta[b - m_bBase].paired, pos = (uint64_t)b << 6;
        if ( b >= m_bRawFrom )
        {
            const Rec *p = Page( b );
            for ( unsigned j = 0; j < n; j++ )
                if ( ( llPaired >> j ) & 1 ) Feed( p[j].uStart, p[j].uEnd, p[j].uSlot, p[j].uRank, pos + j );
                else m_llUnpaired++;
            return;
        }
        Packed &k = m_dqPacked.front();
        uint64_t llBit = k.llBit, llEndBit = k.llEndBit;
        for ( unsigned j = 0; j < n; j++ )
        {
            uint64_t llStart = k.uStartBase + Read( llBit, k.wStart ); llBit += k.wStart;
            uint32_t uSlot = k.uSlotBase + (uint32_t)Read( llBit, k.wSlot ); llBit += k.wSlot;
            uint32_t uRank = (uint32_t)( k.llRankBase + (int64_t)j * k.iRankStride + (int64_t)Read( llBit, k.wRank ) ); llBit += k.wRank;
            uint64_t llEnd = k.pEnd ? k.pEnd[j] : llStart + Read( llEndBit, k.wDur );
            llEndBit += k.wDur;
            if ( ( llPaired >> j ) & 1 ) Feed( llStart, llEnd, uSlot, uRank, pos + j );
            else m_llUnpaired++;
        }
        m_dqPacked.pop_front();
        m_bPackedFrom++;
    }
    void Feed( uint64_t llStartU, uint64_t llEndU, uint32_t uSlot, uint32_t uRank, uint64_t pos )
    {
        if ( m_llFirstPairedU == UINT64_MAX ) m_llFirstPairedU = llStartU;
        m_Stream.Feed( llStartU, llEndU, uSlot, uRank, pos );
    }

    // Feeds the final blocks at the frontier and drops them
    void Advance()
    {
        size_t bFed0 = m_bFed;
        while ( m_bFed < m_nBlocks && m_vMeta[m_bFed - m_bBase].resolved == ~uint64_t( 0 ) ) FeedBlock( m_bFed++, 64 );
        if ( m_bFed == bFed0 ) return;
        m_bRawFrom = std::max( m_bRawFrom, m_bFed );
        // Blocks are packed in order: the bits from the oldest packed block's on are live
        m_vBits.DecommitBefore( (size_t)( ( m_dqPacked.empty() ? m_llBits : m_dqPacked.front().llBit ) >> 6 ) );
        // Drop the records before the frontier, when they are at least as many as the rest
        size_t iDead = m_bFed - m_bBase, iLive = m_nBlocks - m_bFed;
        if ( iDead >= 4096 && iDead >= iLive )
        {
            for ( size_t i = 0; i < iDead; i++ )
                for ( uint64_t w = m_vMeta[i].early; w; w &= w - 1 ) m_setEarly.insert( ( ( m_bBase + i ) << 6 ) + __builtin_ctzll( w ) );
            m_vMeta.erase( m_vMeta.begin(), m_vMeta.begin() + iDead );
            m_bBase = m_bFed;
            // After a long wait (a held note) the records have grown to the queue; give that back
            if ( m_vMeta.capacity() > 4 * m_vMeta.size() + 4096 ) m_vMeta.shrink_to_fit();
        }
    }

    StreamCompactor &m_Stream;
    uint64_t m_n = 0, m_llBits = 0;
    Rec *m_pOpen = nullptr;                 // the newest block's records
    std::unique_ptr< Rec[] > m_pRing;       // blocks m_bRawFrom on, by b % m_iRing
    size_t m_iRing = 0;
    size_t m_nBlocks = 0, m_bBase = 0, m_bFed = 0, m_bRawFrom = 0, m_bPackedFrom = 0;
    std::vector< Meta > m_vMeta;            // blocks m_bBase on
    std::deque< Packed > m_dqPacked;        // blocks m_bPackedFrom to m_bRawFrom
    WordArena m_vBits;
    std::unordered_set< uint64_t > m_setEarly;  // paired early before m_bBase, the off still to come
    bool m_bFirstPairingOnly = false;
    uint64_t m_llUnpaired = 0, m_llFirstPairedU = UINT64_MAX;
};

namespace
{
    // Full -> Compact: drops unpaired and hidden notes, and every note with a later note of the same
    // start and end on this key, neither of them weird (the same-start cull, see VisualStore.h). One
    // pass over the blocks; per same-start group only the last position of each distinct end is kept
    // (a crash can put millions of notes in one group, usually with very few distinct ends).
    // bDups (songs with weird notes): count the notes each kept note dropped, and where those counts
    // do not say which kept note a culled NoteOn belongs to, code it (KeyStore::DupCodes)
    void CompactKey( const KeyStore &src, KeyStore &out, const PackedMonotone &times, const std::vector< uint8_t > *pHidden, bool bDups )
    {
        CompactBuilder cb( out, times, src.Size(), bDups );
        struct End { uint64_t llEnd, llLast; uint32_t uCount, uClass; };
        std::vector< End > vEnds;                               // the group's distinct ends of paired, non-weird notes
        std::unordered_map< uint64_t, uint32_t > mapEnds;      // end -> vEnds index, once a group has many ends
        std::vector< uint64_t > vWeird;                         // the group's weird notes (all kept)
        struct Keep { uint64_t llPos; uint32_t uDup; };
        std::vector< Keep > vKeep;
        std::vector< uint32_t > vCodes;
        uint64_t llGroupStart = UINT64_MAX, llGroupPos = 0, llUnpaired = 0;
        size_t iLast = 0;       // vEnds index of the latest end, the usual repeat
        auto Find = [&]( uint64_t e ) -> End & {
            if ( iLast < vEnds.size() && vEnds[iLast].llEnd == e ) return vEnds[iLast];
            size_t k = vEnds.size();
            if ( !mapEnds.empty() )
            {
                auto it = mapEnds.find( e );
                if ( it != mapEnds.end() ) k = it->second;
            }
            else
                for ( size_t q = 0; q < vEnds.size(); q++ )
                    if ( vEnds[q].llEnd == e ) { k = q; break; }
            if ( k == vEnds.size() )
            {
                vEnds.push_back( { e, 0, 0, 0 } );
                if ( !mapEnds.empty() ) mapEnds.emplace( e, (uint32_t)k );
                else if ( vEnds.size() > 32 )
                    for ( size_t q = 0; q < vEnds.size(); q++ ) mapEnds.emplace( vEnds[q].llEnd, (uint32_t)q );
            }
            iLast = k;
            return vEnds[k];
        };
        auto Hidden = [&]( const NoteView &v ) { return pHidden && v.uSlot < pHidden->size() && ( *pHidden )[v.uSlot]; };
        // The group is the positions [llGroupPos, llGroupEnd)
        auto Flush = [&]( uint64_t llGroupEnd ) {
            vKeep.clear();
            for ( const End &e : vEnds ) vKeep.push_back( { e.llLast, e.uCount - 1 } );
            for ( uint64_t p : vWeird ) vKeep.push_back( { p, 0 } );
            std::sort( vKeep.begin(), vKeep.end(), []( const Keep &a, const Keep &b ) { return a.llPos < b.llPos; } );
            NoteView n;
            uint32_t uClasses = 0;
            if ( !vKeep.empty() ) cb.BeginTick();
            for ( const Keep &k : vKeep )
            {
                src.Get( (size_t)k.llPos, n );
                if ( n.bWeird )
                {
                    uint32_t uOffs = 0;
                    const OffRef *pOffs = src.WeirdOffs( (size_t)k.llPos, uOffs );
                    cb.AddWeird( n, pOffs, uOffs );
                }
                else cb.Add( n.llStartU, n.llEndU, n.uSlot, n.uRank, bDups ? k.uDup : 0 );
                if ( k.uDup ) Find( n.llEndU ).uClass = uClasses++;
            }
            // Classes: the kept notes that dropped some, in position order, then the unpaired notes
            if ( bDups && uClasses && llUnpaired ) cb.AddUnpaired( llGroupStart, llUnpaired );
            if ( bDups && uClasses + ( llUnpaired ? 1 : 0 ) > 1 )
            {
                vCodes.clear();
                for ( uint64_t p = llGroupPos; p < llGroupEnd; p++ )
                {
                    src.Get( (size_t)p, n );
                    if ( n.bUnpaired ) { vCodes.push_back( uClasses ); continue; }
                    if ( n.bWeird ) continue;
                    const End &e = Find( n.llEndU );
                    if ( e.llLast != p ) vCodes.push_back( e.uClass );
                }
                cb.AddCodes( vCodes, Bits( uClasses + ( llUnpaired ? 1 : 0 ) - 1 ) );
            }
            vEnds.clear();
            if ( !mapEnds.empty() ) mapEnds.clear();     // (clear() zeroes every bucket, even of an empty map)
            vWeird.clear();
            llUnpaired = 0;
            iLast = 0;
        };
        NoteView aBlock[KeyStore::BlockSize];
        for ( size_t b = 0; b < src.BlockCount(); b++ )
        {
            unsigned n = src.GetBlock( b, aBlock );
            for ( unsigned j = 0; j < n; j++ )
            {
                const NoteView &v = aBlock[j];
                uint64_t pos = ( (uint64_t)b << KeyStore::BlockShift ) + j;
                if ( v.llStartU != llGroupStart )
                {
                    Flush( pos );
                    llGroupStart = v.llStartU;
                    llGroupPos = pos;
                }
                if ( Hidden( v ) ) continue;
                if ( v.bUnpaired ) { llUnpaired++; continue; }
                if ( v.bWeird ) { vWeird.push_back( pos ); continue; }
                End &e = Find( v.llEndU );
                e.llLast = pos;
                e.uCount++;
            }
        }
        Flush( src.Size() );
        cb.Finish();
    }

    void CompactStore( VisualStore &vs, const PackedMonotone &times, const std::vector< uint8_t > *pHidden, int iThreads, bool bDups )
    {
        Parallel::For( iThreads, 128, [&]( size_t k, int ) {
            KeyStore tmp;
            CompactKey( vs.aKeys[k], tmp, times, pHidden, bDups );
            vs.aKeys[k] = std::move( tmp );     // frees the Full layout of this key at once
        } );
        vs.llStored = 0;
        for ( const KeyStore &ks : vs.aKeys ) vs.llStored += ks.Size();
        vs.bCompact = true;
    }
}

//-----------------------------------------------------------------------------
// The pairing pass: MIDI::ConnectNotes per track, for this thread's keys
//-----------------------------------------------------------------------------

namespace
{
    // The original's per-(track, pitch) list of NoteOns, as runs of consecutive positions: in a
    // chord or crash, one track's notes of a key at one tick are consecutive in the key's list.
    // Only the top is used, and a stack can hold hundreds of runs (tombstones under live notes): the
    // top runs are inline, in the cache line after the rest of the state, the older ones on the heap
    class RunStack
    {
    public:
        bool Empty() const { return !m_nTop; }
        uint32_t Back() const { return m_aTop[m_nTop - 1].uStart + m_aTop[m_nTop - 1].uLen - 1; }
        void PopBack()
        {
            if ( !--m_aTop[m_nTop - 1].uLen && !--m_nTop && m_pSpill && !m_pSpill->empty() ) Refill();
        }
        void Push( uint32_t pos )
        {
            if ( m_nTop && m_aTop[m_nTop - 1].uStart + m_aTop[m_nTop - 1].uLen == pos ) { m_aTop[m_nTop - 1].uLen++; return; }
            if ( m_nTop == Inline ) Spill();
            m_aTop[m_nTop++] = { pos, 1 };
        }
        template < class F > void ForEach( F &&fn ) const
        {
            auto Each = [&]( const Run &r ) { for ( uint32_t i = 0; i < r.uLen; i++ ) fn( r.uStart + i ); };
            if ( m_pSpill ) for ( const Run &r : *m_pSpill ) Each( r );
            for ( unsigned i = 0; i < m_nTop; i++ ) Each( m_aTop[i] );
        }
        void Clear() { m_nTop = 0; m_pSpill.reset(); }

    private:
        struct Run { uint32_t uStart, uLen; };
        static const unsigned Inline = 6, Kept = 2;     // runs inline; kept there when they spill
        // The oldest Inline - Kept runs move to the heap
        void Spill()
        {
            if ( !m_pSpill ) m_pSpill.reset( new std::vector< Run >() );
            m_pSpill->insert( m_pSpill->end(), m_aTop, m_aTop + ( Inline - Kept ) );
            memmove( m_aTop, m_aTop + ( Inline - Kept ), Kept * sizeof( Run ) );
            m_nTop = Kept;
        }
        // The inline runs are used up: the newest Inline - Kept from the heap
        void Refill()
        {
            size_t n = std::min< size_t >( Inline - Kept, m_pSpill->size() );
            memcpy( m_aTop, m_pSpill->data() + m_pSpill->size() - n, n * sizeof( Run ) );
            m_pSpill->resize( m_pSpill->size() - n );
            m_nTop = (uint8_t)n;
        }
        Run m_aTop[Inline];
        std::unique_ptr< std::vector< Run > > m_pSpill;
        uint8_t m_nTop = 0;
    };
}


namespace
{
    // A pairing worker's NoteOns and releasing NoteOffs per NoteStats bin. A worker gets its events
    // in stream order, so its bin only moves forward: a bin is written once, when it is left, as
    // varints (bins since the last one written, ons, offs).
    class StatsRecorder
    {
    public:
        StatsRecorder( const PackedMonotone &times, long long llBinUs ) : m_pTimes( &times ), m_llBinUs( (uint64_t)llBinUs ) {}
        void On( uint64_t u ) { if ( u >= m_uNext ) Move( u ); m_llOns++; }
        void Off( uint64_t u ) { if ( u >= m_uNext ) Move( u ); m_llOffs++; }
        void Flush()
        {
            if ( !m_llOns && !m_llOffs ) return;
            Put( m_llBin - m_llLastBin );
            Put( m_llOns );
            Put( m_llOffs );
            m_llLastBin = m_llBin;
            m_llOns = m_llOffs = 0;
        }
        // Calls fn( bin, ons, offs ) for every bin written
        template < class F > void ForEach( F &&fn ) const
        {
            const uint8_t *p = vBytes.data(), *pEnd = p + vBytes.size();
            uint64_t llBin = 0;
            while ( p < pEnd )
            {
                llBin += Get( p );
                uint64_t llOns = Get( p );
                fn( llBin, llOns, Get( p ) );
            }
        }

    private:
        void Move( uint64_t u )
        {
            Flush();
            m_llBin = m_pTimes->Get( u ) / m_llBinUs;
            m_uNext = m_pTimes->LowerBound( ( m_llBin + 1 ) * m_llBinUs );
        }
        void Put( uint64_t v )
        {
            while ( v >= 0x80 ) { vBytes.push_back( (uint8_t)( v | 0x80 ) ); v >>= 7; }
            vBytes.push_back( (uint8_t)v );
        }
        static uint64_t Get( const uint8_t *&p )
        {
            uint64_t v = 0;
            unsigned s = 0;
            uint8_t c;
            do { c = *p++; v |= ( uint64_t )( c & 0x7F ) << s; s += 7; } while ( c & 0x80 );
            return v;
        }

        const PackedMonotone *m_pTimes;
        uint64_t m_llBinUs;
        uint64_t m_uNext = 0;           // first unique tick past the current bin (0: none yet)
        uint64_t m_llBin = 0, m_llLastBin = 0, m_llOns = 0, m_llOffs = 0;
        std::vector< uint8_t > vBytes;
    };
}

namespace
{
    // The pairing pass's shared decode (DESIGN.md 5.5): the stream is parsed by a few decoder threads
    // ("lanes") that route every NoteOn and NoteOff to the worker owning its key, in batches. The
    // stream is cut at seek points into segments of about one chunk:
    //   - a normal segment g is parsed by lane g % L, for every worker;
    //   - a giant one (a tick spanning many chunks: a crash) by every lane, each routing only the keys
    //     of its own workers (worker w belongs to lane w % L), so a crash keeps every lane busy.
    // A worker takes the segments in order, each from the lane that has its records, so every key
    // sees its events in stream order, as if it had walked the stream itself. A lane runs at most
    // Window batches ahead of the workers reading it: a few MB.
    struct NoteRec
    {
        uint32_t uDU;           // unique tick - the batch's base
        uint32_t uTrack;
        uint32_t uOrd;          // ordinal of the event in its track's group (OrderKey)
        uint8_t cKey;
        uint8_t cCh;            // channel, | 0x80 for a NoteOn with velocity > 0
    };

    class DecodeLanes
    {
    public:
        static const int Window = 8;
        static const size_t BatchRecs = size_t( 1 ) << 14;
        // A lane must hold a whole normal segment ahead of the workers to run beside the others
        static const uint64_t SegmentBytes = EventStream::ChunkSize;
        static const uint64_t GiantBytes = 4 * EventStream::ChunkSize;

        DecodeLanes( const EventStream &es, const int aOwner[128], int iWorkers, int iLanes ) : m_es( es ), m_iWorkers( iWorkers )
        {
            size_t U = es.UniqueTicks();
            m_vCuts.push_back( 0 );
            std::vector< uint64_t > vRaw = { 0 };
            uint64_t llNext = SegmentBytes;
            for ( const EventStream::SeekPoint &sp : es.vSeek )
                if ( sp.llRaw >= llNext && sp.llU > m_vCuts.back() && sp.llU < U )
                {
                    m_vCuts.push_back( sp.llU );
                    vRaw.push_back( sp.llRaw );
                    llNext = sp.llRaw + SegmentBytes;
                }
            m_vCuts.push_back( U );
            vRaw.push_back( es.llRawBytes );
            size_t iSegments = m_vCuts.size() - 1;
            m_vGiant.resize( iSegments );
            for ( size_t g = 0; g < iSegments; g++ ) m_vGiant[g] = vRaw[g + 1] - vRaw[g] > GiantBytes;
            iLanes = std::max( 1, std::min( iLanes, iWorkers ) );
            for ( int k = 0; k < 128; k++ ) m_aRoute[k] = (int8_t)aOwner[k];
            for ( int l = 0; l < iLanes; l++ )
            {
                m_vLanes.emplace_back( new Lane( iWorkers, ( iWorkers - l + iLanes - 1 ) / iLanes, iSegments ) );
                // A giant segment: every lane, each for the keys of its own workers (worker w: lane w % L)
                for ( int k = 0; k < 128; k++ ) m_vLanes.back()->aRouteGiant[k] = (int8_t)( aOwner[k] >= 0 && aOwner[k] % iLanes == l ? aOwner[k] : -1 );
            }
        }

        int Lanes() const { return (int)m_vLanes.size(); }
        void Abort()
        {
            m_bAbort.store( true );
            for ( auto &pl : m_vLanes ) { std::lock_guard< std::mutex > lock( pl->mx ); pl->cv.notify_all(); }
        }

        // Decoder thread of lane l
        void Produce( int l, const std::atomic< bool > *pCancel, const std::atomic< bool > *pStop )
        {
            Lane &ln = *m_vLanes[l];
            size_t L = m_vLanes.size(), iSegments = m_vCuts.size() - 1;
            uint64_t s = 0;     // the lane's batch sequence
            for ( size_t g = 0; g < iSegments && !m_bAbort.load(); g++ )
            {
                bool bGiant = m_vGiant[g];
                if ( !bGiant && g % L != (size_t)l ) continue;
                int iReaders = bGiant ? ln.iMine : m_iWorkers;
                {
                    std::lock_guard< std::mutex > lock( ln.mx );
                    ln.vSegFirst[g] = s;
                    ln.cv.notify_all();
                }
                uint64_t u0 = m_vCuts[g], u1 = m_vCuts[g + 1];
                Batch *pB = Acquire( ln, s );
                if ( !pB ) break;
                pB->llBaseU = pB->llCompleteU = u0;
                size_t n = 0;
                // The stream's grammar (Song.h) parsed here rather than through Walk: per key byte, one
                // table lookup gives the worker, or -1 (not routed by this lane)
                const int8_t *pRoute = bGiant ? ln.aRouteGiant : m_aRoute;
                bool bOk = true;
                {
                    const EventStream::SeekPoint &sp = m_es.SeekPointFor( (size_t)u0 );
                    StreamBytes r;
                    r.Start( m_es, sp.llRaw );
                    uint64_t u = sp.llU;
                    int64_t llPrev = -1;
                    uint64_t llGroups = 0;
                    std::vector< NoteRec > *pPer = pB->vPer.data();
                    uint64_t llBase = pB->llBaseU;
                    while ( u < u1 )
                    {
                        r.Need();
                        uint64_t hdr = EventStream::ReadVar( r.p );
                        uint32_t uTrack = (uint32_t)( llPrev + 1 + (int64_t)( hdr >> 1 ) );
                        bool bEmit = u >= u0;
                        uint32_t uOrd = 0;
                        unsigned h;
                        do
                        {
                            r.Need();
                            h = *r.p++;
                            unsigned op = h >> 5;
                            if ( op > 2 ) { r.p += ( op == 5 || op == 6 ) ? 1 : 2; uOrd++; continue; }
                            // NoteOn (op 0, velocity > 0) or NoteOff (op 1, or 2: a NoteOn of velocity 0)
                            if ( op != 2 ) r.p++;
                            uint8_t cCh = (uint8_t)( ( h & 0x0F ) | ( op == 0 ? 0x80 : 0 ) );
                            unsigned k;
                            do
                            {
                                r.Need();
                                k = *r.p++;
                                unsigned key = k & 0x7F;
                                int w = pRoute[key];
                                if ( w >= 0 && bEmit )
                                {
                                    if ( n >= BatchRecs || u - llBase >= UINT32_MAX )
                                    {
                                        uint64_t llComplete = pB->llCompleteU;
                                        Publish( ln, s++, iReaders, false );
                                        pB = Acquire( ln, s );
                                        n = 0;
                                        if ( !pB ) { bOk = false; break; }
                                        pB->llBaseU = llBase = u;
                                        pB->llCompleteU = llComplete;
                                        pPer = pB->vPer.data();
                                    }
                                    pPer[w].push_back( { (uint32_t)( u - llBase ), uTrack, uOrd, (uint8_t)key, cCh } );
                                    n++;
                                }
                                uOrd++;
                            } while ( !( k & 0x80 ) );
                        } while ( bOk && !( h & 0x10 ) );
                        if ( !bOk ) break;
                        if ( hdr & 1 )
                        {
                            if ( bEmit ) pB->llCompleteU = u + 1;
                            u++;
                            llPrev = -1;
                        }
                        else llPrev = uTrack;
                        if ( ( ++llGroups & 1023 ) == 0 &&
                             ( m_bAbort.load( std::memory_order_relaxed ) || ( pCancel && pCancel->load( std::memory_order_relaxed ) ) ||
                               ( pStop && pStop->load( std::memory_order_relaxed ) ) ) )
                        {
                            bOk = false;
                            break;
                        }
                    }
                }
                if ( !bOk || !pB ) { Abort(); break; }
                pB->llCompleteU = u1;
                Publish( ln, s++, iReaders, true );     // the segment's last batch
            }
            std::lock_guard< std::mutex > lock( ln.mx );
            ln.bDone = true;
            ln.cv.notify_all();
        }

        // Worker w: fn( records, base tick, ticks below this are complete ) for each batch in stream
        // order; false from fn stops everything
        template < class F > bool Consume( int w, F &&fn )
        {
            size_t L = m_vLanes.size(), iSegments = m_vCuts.size() - 1;
            for ( size_t g = 0; g < iSegments; g++ )
            {
                Lane &ln = *m_vLanes[m_vGiant[g] ? (size_t)w % L : g % L];
                uint64_t s;
                {
                    std::unique_lock< std::mutex > lock( ln.mx );
                    ln.cv.wait( lock, [&] { return ln.vSegFirst[g] != UINT64_MAX || ln.bDone || m_bAbort.load(); } );
                    if ( m_bAbort.load() || ln.vSegFirst[g] == UINT64_MAX ) return false;
                    s = ln.vSegFirst[g];
                }
                for ( bool bSegEnd = false; !bSegEnd; s++ )
                {
                    Batch *pB;
                    {
                        std::unique_lock< std::mutex > lock( ln.mx );
                        ln.cv.wait( lock, [&] { return ln.llProduced > s || ln.bDone || m_bAbort.load(); } );
                        if ( m_bAbort.load() || ln.llProduced <= s ) return false;
                        pB = &ln.aSlots[s % Window];
                    }
                    bSegEnd = pB->bSegEnd;
                    bool bOk = fn( pB->vPer[w], pB->llBaseU, pB->llCompleteU );
                    {
                        std::lock_guard< std::mutex > lock( ln.mx );
                        if ( --pB->iPending == 0 ) { pB->bBusy = false; ln.cv.notify_all(); }
                    }
                    if ( !bOk ) { Abort(); return false; }
                }
            }
            return true;
        }

    private:
        struct Batch
        {
            uint64_t llBaseU = 0, llCompleteU = 0;
            std::vector< std::vector< NoteRec > > vPer;     // by worker
            int iPending = 0;
            bool bBusy = false, bSegEnd = false;
        };
        struct Lane
        {
            Lane( int iWorkers, int iMineWorkers, size_t iSegments ) : iMine( iMineWorkers ), vSegFirst( iSegments, UINT64_MAX )
            {
                for ( Batch &bt : aSlots ) bt.vPer.resize( iWorkers );
            }
            int iMine;                          // workers reading this lane's giant segments
            int8_t aRouteGiant[128];            // per key: its worker, in a giant segment (-1: another lane's)
            std::mutex mx;
            std::condition_variable cv;
            Batch aSlots[Window];
            uint64_t llProduced = 0;
            std::vector< uint64_t > vSegFirst;  // per segment: the lane's first batch of it (UINT64_MAX: not this lane's, or not yet)
            bool bDone = false;
        };

        // The slot for batch s, once every reader is done with batch s - Window (nullptr: aborted)
        Batch *Acquire( Lane &ln, uint64_t s )
        {
            std::unique_lock< std::mutex > lock( ln.mx );
            Batch &b = ln.aSlots[s % Window];
            ln.cv.wait( lock, [&] { return !b.bBusy || m_bAbort.load(); } );
            if ( m_bAbort.load() ) return nullptr;
            for ( auto &v : b.vPer ) v.clear();
            b.bBusy = true;
            return &b;
        }
        void Publish( Lane &ln, uint64_t s, int iReaders, bool bSegEnd )
        {
            std::lock_guard< std::mutex > lock( ln.mx );
            Batch &b = ln.aSlots[s % Window];
            b.iPending = iReaders;
            b.bSegEnd = bSegEnd;
            ln.llProduced = s + 1;
            ln.cv.notify_all();
        }

        const EventStream &m_es;
        int m_iWorkers;
        int8_t m_aRoute[128];                       // per key: its worker (-1: none)
        std::vector< uint64_t > m_vCuts;            // segment g is ticks [m_vCuts[g], m_vCuts[g + 1])
        std::vector< bool > m_vGiant;
        std::vector< std::unique_ptr< Lane > > m_vLanes;
        std::atomic< bool > m_bAbort{ false };
    };
}

// KB: KeyBuilder (the Full layout) or StreamKey (compacting while pairing)
template < class KB > class VisualStoreBuilder
{
public:
    VisualStoreBuilder( const Song &song, std::vector< std::unique_ptr< KB > > &vBuilders,
                        const std::vector< std::pair< uint64_t, uint32_t > > &vTrackEnds, const std::vector< int > &vKeys )
        : m_Song( song ), m_vBuilders( vBuilders ), m_vTrackEnds( vTrackEnds ), m_vKeys( vKeys )
    {
        memset( m_aMine, -1, sizeof( m_aMine ) );
        for ( size_t i = 0; i < vKeys.size(); i++ ) m_aMine[vKeys[i]] = (int)i;
        m_iTracks = song.vTracks.size();
        m_vLight.resize( m_iTracks * vKeys.size() );
    }

    // Worker w: MIDI::ConnectNotes for this thread's keys, from the lanes' records in stream order.
    // A track's end (after the last event of its last tick) is applied before the first record of
    // a later tick, or once a batch says its tick is complete: the pairing state is per track and
    // key, so this is the same as at the end of the tick.
    // pStop: a pass that does not keep the Full layout stops as soon as a weird note shows up (it
    // will be paired again with bFull)
    bool RunShared( DecodeLanes &dl, int w, std::atomic< uint64_t > *pDone, const std::atomic< bool > *pCancel,
                    const std::atomic< bool > *pStop = nullptr, StatsRecorder *pStats = nullptr )
    {
        m_pStats = pStats;
        size_t U = m_Song.stream.UniqueTicks(), iTrackEnd = 0;
        auto EndBefore = [&]( uint64_t u ) {
            while ( iTrackEnd < m_vTrackEnds.size() && m_vTrackEnds[iTrackEnd].first < u ) EndTrack( m_vTrackEnds[iTrackEnd++].second );
        };
        bool bOk = dl.Consume( w, [&]( const std::vector< NoteRec > &v, uint64_t llBase, uint64_t llComplete ) {
            for ( const NoteRec &r : v )
            {
                uint64_t u = llBase + r.uDU;
                EndBefore( u );
                if ( r.cCh & 0x80 ) On( r.uTrack, r.cCh & 0x0F, r.cKey, u, r.uOrd );
                else Off( r.uTrack, r.cCh & 0x0F, r.cKey, u, r.uOrd );
            }
            EndBefore( llComplete );
            if ( pDone ) pDone->store( llComplete, std::memory_order_relaxed );
            if ( pCancel && pCancel->load( std::memory_order_relaxed ) ) return false;
            if ( pStop && pStop->load( std::memory_order_relaxed ) ) return false;
            return true;
        } );
        if ( m_pStats ) m_pStats->Flush();
        if ( !bOk ) return false;
        EndBefore( U );
        if ( pDone ) pDone->store( U, std::memory_order_relaxed );
        return true;
    }

private:
    // A song can have a million (track, key) states at once, and a NoteOn or NoteOff touches one at
    // random among its thread's. Nearly every state holds at most one note at a time, on one channel:
    // that is a Light record of 16 bytes in a table by track and key, so an event touches one cache
    // line (a track's chord mostly the same one). A state that comes to hold more, or uses a second
    // channel, moves to a KeyState until its track ends.
    struct Light
    {
        uint32_t uOns;          // NoteOns so far
        uint32_t uSlot;         // the held note's position (cCount 1); with Heavy, its KeyState in m_vPool
        uint32_t uKnownLo;      // its next note in Song::vKnownId, in 40 bits (NoKnown: none left)
        uint8_t cKnownHi;
        int8_t cCh;             // the channel of its first NoteOn (-1: none yet)
        uint8_t cCount;         // NoteOns minus NoteOffs on cCh: 0 or 1
        uint8_t cFlags;
    };
    static_assert( sizeof( Light ) == 16, "four to a cache line" );
    static const uint8_t Seen = 1, Heavy = 2;
    // A KeyState has the first channel used in it, the others in an extra record.
    // The stack: from a channel's first overflow, every NoteOn (paired ones are tombstones), the
    // unpaired slots' first; the overflow pairs the latest unpaired NoteOn of the key in the track
    struct KeyExtra
    {
        uint32_t aCount[16];
        uint32_t aSlots[16 * 10];
    };
    struct alignas( 64 ) KeyState
    {
        uint32_t aSlots0[10];
        uint32_t uCount0;                   // NoteOns minus NoteOffs on cCh0
        uint32_t uExtra;                    // m_vExtra index + 1 (0: none)
        int8_t cCh0;                        // the channel of aSlots0
        bool bStacked;                      // since a channel first overflowed
        alignas( 64 ) RunStack stack;
    };
    static_assert( sizeof( KeyState ) == 128, "two cache lines" );
    static constexpr uint64_t NoKnown = ( uint64_t( 1 ) << 40 ) - 1;

    // Index i of Song::vKnownId if it is a note of the state with this id prefix, else NoKnown
    void SetKnown( Light &l, uint64_t i, uint64_t llPrefix ) const
    {
        const std::vector< uint64_t > &v = m_Song.vKnownId;
        if ( i >= v.size() || ( v[i] >> 40 ) != ( llPrefix >> 40 ) ) i = NoKnown;
        l.uKnownLo = (uint32_t)i;
        l.cKnownHi = (uint8_t)( i >> 32 );
    }

    KeyExtra &Extra( KeyState &s )
    {
        if ( !s.uExtra )
        {
            if ( !m_vExtraFree.empty() ) { s.uExtra = m_vExtraFree.back(); m_vExtraFree.pop_back(); }
            else { m_vExtra.emplace_back(); s.uExtra = (uint32_t)m_vExtra.size(); }
            KeyExtra &x = m_vExtra[s.uExtra - 1];
            memset( x.aCount, 0, sizeof( x.aCount ) );
        }
        return m_vExtra[s.uExtra - 1];
    }
    uint32_t Count( const KeyState &s, unsigned ch ) const
    {
        if ( s.cCh0 == (int)ch ) return s.uCount0;
        return s.uExtra ? m_vExtra[s.uExtra - 1].aCount[ch] : 0;
    }
    uint32_t &CountRef( KeyState &s, unsigned ch ) { return s.cCh0 == (int)ch ? s.uCount0 : Extra( s ).aCount[ch]; }
    uint32_t *Slots( KeyState &s, unsigned ch ) { return s.cCh0 == (int)ch ? s.aSlots0 : &Extra( s ).aSlots[ch * 10]; }

    Light &LightOf( uint32_t uTrack, unsigned key ) { return m_vLight[uTrack * m_vKeys.size() + m_aMine[key]]; }

    // The light state's note (if any) and channel move to a KeyState
    KeyState &ToHeavy( Light &l )
    {
        uint32_t idx;
        if ( !m_vFree.empty() ) { idx = m_vFree.back(); m_vFree.pop_back(); }
        else { idx = (uint32_t)m_vPool.size(); m_vPool.emplace_back(); }
        KeyState &s = m_vPool[idx];
        s.aSlots0[0] = l.uSlot;
        s.uCount0 = l.cCount;
        s.uExtra = 0;
        s.cCh0 = l.cCh;
        s.bStacked = false;
        s.stack.Clear();
        l.uSlot = idx;
        l.cFlags |= Heavy;
        return s;
    }

    void On( uint32_t uTrack, unsigned ch, unsigned key, uint64_t u, uint32_t uOrd )
    {
        if ( m_pStats ) m_pStats->On( u );
        KB &kb = *m_vBuilders[key];
        uint64_t pos = kb.Append( u, uTrack * 16 + ch, uOrd );
        Light &l = LightOf( uTrack, key );
        uint64_t llPrefix = ( (uint64_t)uTrack << 48 ) | ( (uint64_t)key << 40 );
        if ( !( l.cFlags & Seen ) )
        {
            l.cFlags = Seen;
            l.cCh = -1;
            const std::vector< uint64_t > &v = m_Song.vKnownId;
            SetKnown( l, (uint64_t)( std::lower_bound( v.begin(), v.end(), llPrefix ) - v.begin() ), llPrefix );
        }
        // A note whose pairing the count pass found is final now: one no off will pair would otherwise be
        // live in its slot until its track ends, a long one until its off, and every later note of the
        // key would wait for it
        uint64_t llKnown = l.uKnownLo | ( (uint64_t)l.cKnownHi << 32 );
        bool bMatch = llKnown != NoKnown && m_Song.vKnownId[llKnown] == ( llPrefix | l.uOns ), bKnown = bMatch;
        uint64_t llKnownEnd = bKnown ? m_Song.vKnownEnd[llKnown] : UINT64_MAX;
        if ( bKnown && llKnownEnd != UINT64_MAX && !kb.CanPrePair() ) bKnown = false;
        if ( bMatch ) SetKnown( l, llKnown + 1, llPrefix );
        l.uOns++;
        if ( !( l.cFlags & Heavy ) )
        {
            if ( l.cCh < 0 ) l.cCh = (int8_t)ch;
            if ( l.cCh == (int)ch && !l.cCount )
            {
                l.uSlot = (uint32_t)pos;
                l.cCount = 1;
                if ( !bKnown ) kb.SetLive( pos, true );
                else if ( llKnownEnd == UINT64_MAX ) kb.Resolve( pos );
                else kb.PrePair( pos, llKnownEnd );
                return;
            }
            ToHeavy( l );
        }
        KeyState &s = m_vPool[l.uSlot];
        uint32_t &uCount = CountRef( s, ch );
        if ( uCount < 10 )
        {
            Slots( s, ch )[uCount] = (uint32_t)pos;
            if ( !bKnown ) kb.SetLive( pos, true );
        }
        else if ( !s.bStacked )
        {
            // The first overflow: the unpaired NoteOns in slots go into the stack, in order
            uint32_t aPos[160];
            unsigned n = 0;
            for ( unsigned c = 0; c < 16; c++ )
            {
                uint32_t k = Count( s, c );
                if ( !k ) continue;
                const uint32_t *pSlots = Slots( s, c );
                for ( uint32_t j = 0; j < std::min< uint32_t >( k, 10 ); j++ )
                    if ( !kb.IsPaired( pSlots[j] ) ) aPos[n++] = pSlots[j];
            }
            std::sort( aPos, aPos + n );
            for ( unsigned j = 0; j < n; j++ ) s.stack.Push( aPos[j] );
            s.bStacked = true;
        }
        if ( s.bStacked )
        {
            RunStack &st = s.stack;
            while ( !st.Empty() && kb.IsPaired( st.Back() ) ) st.PopBack();
            st.Push( (uint32_t)pos );
        }
        if ( bKnown && llKnownEnd == UINT64_MAX ) kb.Resolve( pos );
        else if ( bKnown ) kb.PrePair( pos, llKnownEnd );
        CountRef( s, ch )++;    // (the extra record may have moved)
    }

    void Off( uint32_t uTrack, unsigned ch, unsigned key, uint64_t u, uint32_t uOrd )
    {
        Light &l = LightOf( uTrack, key );
        if ( !( l.cFlags & Heavy ) )
        {
            if ( !l.cCount || l.cCh != (int)ch ) return;
            if ( m_pStats ) m_pStats->Off( u );    // releases a held note (NoteStats)
            m_vBuilders[key]->Pair( l.uSlot, u, uOrd, true );
            l.cCount = 0;
            return;
        }
        KeyState &s = m_vPool[l.uSlot];
        uint32_t uCount = Count( s, ch );
        if ( !uCount ) return;
        if ( m_pStats ) m_pStats->Off( u );
        KB &kb = *m_vBuilders[key];
        if ( uCount <= 10 )
            kb.Pair( Slots( s, ch )[uCount - 1], u, uOrd, true );
        else
        {
            // Overflow: the latest unpaired NoteOn of this pitch in the track, on any channel (the state
            // is stacked: a channel overflowed)
            RunStack &st = s.stack;
            while ( !st.Empty() && kb.IsPaired( st.Back() ) ) st.PopBack();
            if ( !st.Empty() ) kb.Pair( st.Back(), u, uOrd, false );
        }
        CountRef( s, ch )--;
    }

    // Nothing in the track can change any more: provisional pairings are final, unpaired ons are dropped
    void EndTrack( uint32_t uTrack )
    {
        for ( size_t m = 0; m < m_vKeys.size(); m++ )
        {
            Light &l = m_vLight[uTrack * m_vKeys.size() + m];
            if ( !( l.cFlags & Seen ) ) continue;
            KB &kb = *m_vBuilders[m_vKeys[m]];
            if ( !( l.cFlags & Heavy ) )
            {
                if ( l.cCount )
                {
                    kb.SetLive( l.uSlot, false );
                    kb.Resolve( l.uSlot );
                }
                l = Light();
                continue;
            }
            KeyState &s = m_vPool[l.uSlot];
            for ( unsigned ch = 0; ch < 16; ch++ )
            {
                uint32_t k = Count( s, ch );
                if ( !k ) continue;
                const uint32_t *pSlots = Slots( s, ch );
                for ( uint32_t j = 0; j < std::min< uint32_t >( k, 10 ); j++ )
                {
                    kb.SetLive( pSlots[j], false );
                    kb.Resolve( pSlots[j] );
                }
            }
            s.stack.ForEach( [&]( uint32_t pos ) { if ( !kb.IsPaired( pos ) ) kb.Resolve( pos ); } );
            s.stack.Clear();
            if ( s.uExtra ) m_vExtraFree.push_back( s.uExtra );
            m_vFree.push_back( l.uSlot );
            l = Light();
        }
    }

    const Song &m_Song;
    std::vector< std::unique_ptr< KB > > &m_vBuilders;
    const std::vector< std::pair< uint64_t, uint32_t > > &m_vTrackEnds;
    const std::vector< int > &m_vKeys;
    int m_aMine[128];
    size_t m_iTracks = 0;
    std::vector< Light > m_vLight;          // by track, then this thread's key
    std::vector< KeyState > m_vPool;
    std::vector< uint32_t > m_vFree;
    std::vector< KeyExtra > m_vExtra;
    std::vector< uint32_t > m_vExtraFree;
    StatsRecorder *m_pStats = nullptr;
};

namespace
{
// The jump walk's tables (VisualStore::vOnTicks, vOnLast, vJumpK), from the Full layout with end
// ordinals. Per unique tick: the paired NoteOns, the OrderKey of the last of them (prev of a jump is
// always the last paired NoteOn of its tick), the pairing offs, and the pairing offs that come before
// that last NoteOn. Equal ticks in a row are added at once (a crash's notes share their start, and
// usually their end).
void BuildJumpTables( VisualStore &vs, size_t U, int iThreads )
{
    std::vector< uint64_t > vOns( U, 0 ), vLast( U, 0 ), vOffs( U, 0 ), vBefore( U, 0 );   // vLast: OrderKey + 1
    auto Add = []( uint64_t &x, uint64_t v ) { if ( v ) __atomic_fetch_add( &x, v, __ATOMIC_RELAXED ); };
    auto Max = []( uint64_t &x, uint64_t v ) {
        uint64_t c = __atomic_load_n( &x, __ATOMIC_RELAXED );
        while ( c < v && !__atomic_compare_exchange_n( &x, &c, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED ) ) {}
    };
    struct Run { uint64_t u = UINT64_MAX, n = 0; };
    // Calls fn( u, OrderKey ) for every pairing off of the note at pos
    auto ForOffs = []( const KeyStore &ks, size_t pos, const NoteView &v, auto &&fn ) {
        uint32_t uCount = 0;
        const OffRef *pOffs = v.bWeird ? ks.WeirdOffs( pos, uCount ) : nullptr;
        if ( pOffs ) for ( uint32_t i = 0; i < uCount; i++ ) fn( pOffs[i].llU, OrderKey( v.uSlot, pOffs[i].uOrd ) );
        else fn( v.llEndU, OrderKey( v.uSlot, v.uEndRank ) );
    };
    Parallel::For( iThreads, 128, [&]( size_t k, int ) {
        const KeyStore &ks = vs.aKeys[k];
        NoteView a[KeyStore::BlockSize];
        Run on, off;
        uint64_t llLast = 0;
        for ( size_t b = 0; b < ks.BlockCount(); b++ )
        {
            unsigned n = ks.GetBlock( b, a );
            for ( unsigned j = 0; j < n; j++ )
            {
                const NoteView &v = a[j];
                if ( v.bUnpaired ) continue;
                if ( v.llStartU != on.u )
                {
                    if ( on.n ) { Add( vOns[on.u], on.n ); Max( vLast[on.u], llLast ); }
                    on = { v.llStartU, 0 };
                    llLast = 0;
                }
                on.n++;
                llLast = std::max( llLast, OrderKey( v.uSlot, v.uRank ) + 1 );
                ForOffs( ks, ( b << KeyStore::BlockShift ) + j, v, [&]( uint64_t u, uint64_t ) {
                    if ( u != off.u ) { if ( off.n ) Add( vOffs[off.u], off.n ); off = { u, 0 }; }
                    off.n++;
                } );
            }
        }
        if ( on.n ) { Add( vOns[on.u], on.n ); Max( vLast[on.u], llLast ); }
        if ( off.n ) Add( vOffs[off.u], off.n );
    } );
    Parallel::For( iThreads, 128, [&]( size_t k, int ) {
        const KeyStore &ks = vs.aKeys[k];
        NoteView a[KeyStore::BlockSize];
        Run before;
        for ( size_t b = 0; b < ks.BlockCount(); b++ )
        {
            unsigned n = ks.GetBlock( b, a );
            for ( unsigned j = 0; j < n; j++ )
            {
                const NoteView &v = a[j];
                if ( v.bUnpaired ) continue;
                ForOffs( ks, ( b << KeyStore::BlockShift ) + j, v, [&]( uint64_t u, uint64_t llOrder ) {
                    if ( llOrder + 1 >= vLast[u] ) return;
                    if ( u != before.u ) { if ( before.n ) Add( vBefore[before.u], before.n ); before = { u, 0 }; }
                    before.n++;
                } );
            }
        }
        if ( before.n ) Add( vBefore[before.u], before.n );
    } );
    // K = Simultaneous( prev ) + 1 = paired NoteOns up to prev minus the pairing offs before it
    uint64_t llOnPrefix = 0, llOffPrefix = 0;
    for ( size_t u = 0; u < U; u++ )
    {
        if ( vOns[u] )
        {
            vs.vOnTicks.push_back( u );
            vs.vOnLast.push_back( vLast[u] - 1 );
            vs.vJumpK.push_back( (int64_t)( llOnPrefix + vOns[u] ) - (int64_t)( llOffPrefix + vBefore[u] ) );
        }
        llOnPrefix += vOns[u];
        llOffPrefix += vOffs[u];
    }
    vs.vOnTicks.shrink_to_fit();
    vs.vOnLast.shrink_to_fit();
    vs.vJumpK.shrink_to_fit();
}

// The process's private memory in RAM (the load statistics' memory per step)
uint64_t PrivateBytes() { return Util::ProcessMemory(); }

// The pairing pass into the Full layout, with the counts.
// bEndRanks: keep the end ordinals; a song with weird notes then gets the jump-walk tables.
// bStopOnWeird: return as soon as a weird note shows up, with only bWeird set (the song is then
// paired again with bEndRanks).
// bStream: compact while pairing, culling with pHidden (DESIGN.md 5.5); vs is then Compact
// (vs.bCompact) and the Full layout never exists whole. Only for passes with no weird note (the pass
// pairs none twice, see KeyBuilder::Pair), and only for songs whose unique ticks fit 32 bits (else vs
// is Full, for the caller to compact).
// pStats: also count the NoteStats, from the events the workers get anyway (DESIGN.md 5.6)
bool PairAll( const Song &song, VisualStore &vs, int iThreads, std::atomic< uint64_t > *pDone, const std::atomic< bool > *pCancel, bool bEndRanks,
              bool bStopOnWeird, double *pSteps = nullptr, bool bStream = false, const std::vector< uint8_t > *pHidden = nullptr,
              NoteStats *pStats = nullptr )
{
    double adLocal[VisualStore::StepCount] = {};
    if ( !pSteps ) pSteps = adLocal;
    long long llT = Timer::NowMicroSecs();
    auto Mark = [&]( int i ) { long long t = Timer::NowMicroSecs(); pSteps[i] += ( t - llT ) / 1e6; llT = t; vs.adStepPrivate[i] = PrivateBytes(); };
    vs = VisualStore();
    const EventStream &es = song.stream;
    size_t U = es.UniqueTicks();
    if ( U > UINT32_MAX ) bStream = false;     // (StreamKey keeps ticks in 32 bits)

    // The last unique tick of every track, in tick order
    std::vector< std::pair< uint64_t, uint32_t > > vTrackEnds;
    for ( size_t t = 0; t < song.vTrackLastU.size(); t++ )
        if ( song.vTrackLastU[t] != UINT64_MAX ) vTrackEnds.push_back( { song.vTrackLastU[t], (uint32_t)t } );
    std::sort( vTrackEnds.begin(), vTrackEnds.end() );

    // Keys over threads, largest first onto the least loaded thread
    std::vector< int > vOrder;
    for ( int k = 0; k < 128; k++ )
        if ( song.aKeyNoteCounts[k] ) vOrder.push_back( k );
    std::sort( vOrder.begin(), vOrder.end(), [&]( int a, int b ) { return song.aKeyNoteCounts[a] > song.aKeyNoteCounts[b]; } );
    int iWorkers = std::max( 1, std::min< int >( Parallel::ThreadCount( iThreads ), (int)vOrder.size() ) );
    std::vector< std::vector< int > > vSets( iWorkers );
    std::vector< uint64_t > vLoad( iWorkers, 0 );
    for ( int k : vOrder )
    {
        int t = (int)( std::min_element( vLoad.begin(), vLoad.end() ) - vLoad.begin() );
        vSets[t].push_back( k );
        vLoad[t] += song.aKeyNoteCounts[k] + 1;
    }

    std::vector< std::unique_ptr< KeyBuilder > > vBuilders( bStream ? 0 : 128 );
    std::vector< std::unique_ptr< StreamKey > > vStreamKeys( bStream ? 128 : 0 );
    std::vector< KeyStore > vCompact( bStream ? 128 : 0 );
    std::vector< std::unique_ptr< StreamCompactor > > vStream( bStream ? 128 : 0 );
    for ( int k = 0; k < 128; k++ )
    {
        if ( bStream )
        {
            vStream[k].reset( new StreamCompactor( vCompact[k], es.times, (size_t)song.aKeyNoteCounts[k], pHidden ) );
            vStreamKeys[k].reset( new StreamKey( *vStream[k], song.aKeyNoteCounts[k] ) );
            vStreamKeys[k]->SetFirstPairingOnly( song.bFirstPairingOnly );
        }
        else
        {
            vBuilders[k].reset( new KeyBuilder( vs.aKeys[k], es.times, song.aKeyNoteCounts[k], bEndRanks, !bStopOnWeird ) );
            vBuilders[k]->SetFirstPairingOnly( song.bFirstPairingOnly );
        }
    }

    // NoteStats bins: 1 ms, wider when the song would need more than MaxBins
    uint64_t llLastUs = U ? es.times.Get( U - 1 ) : 0;
    long long llBinUs = std::max< long long >( 1000, (long long)( llLastUs / NoteStats::MaxBins ) + 1 );
    std::vector< StatsRecorder > vRec;
    if ( pStats ) vRec.assign( iWorkers, StatsRecorder( es.times, llBinUs ) );

    std::atomic< bool > bOk{ true };
    if ( !vOrder.empty() )
    {
        // One decode shared by the workers: a lane per four threads (each parses about 250 M events/s)
        int aOwner[128];
        for ( int k = 0; k < 128; k++ ) aOwner[k] = -1;
        for ( int t = 0; t < iWorkers; t++ ) for ( int k : vSets[t] ) aOwner[k] = t;
        DecodeLanes dl( es, aOwner, iWorkers, std::max( 1, std::min( 4, Parallel::ThreadCount( iThreads ) / 4 ) ) );
        int iLanes = dl.Lanes();
        // Every item needs its own thread (workers wait on the lanes): as many threads as items
        Parallel::For( iWorkers + iLanes, (size_t)( iWorkers + iLanes ), [&]( size_t i, int ) {
            if ( (int)i < iLanes ) { dl.Produce( (int)i, pCancel, nullptr ); return; }
            int t = (int)i - iLanes;
            StatsRecorder *pRec = pStats ? &vRec[t] : nullptr;
            bool bDone;
            if ( bStream ) bDone = VisualStoreBuilder< StreamKey >( song, vStreamKeys, vTrackEnds, vSets[t] ).RunShared( dl, t, t == 0 ? pDone : nullptr, pCancel, nullptr, pRec );
            else bDone = VisualStoreBuilder< KeyBuilder >( song, vBuilders, vTrackEnds, vSets[t] ).RunShared( dl, t, t == 0 ? pDone : nullptr, pCancel, nullptr, pRec );
            if ( !bDone ) bOk = false;
        } );
    }
    Mark( VisualStore::StepPair );
    if ( !bOk ) return false;

    if ( pStats )
    {
        // The workers' bins, added up; the polyphony is the running sum of ons - offs
        NoteStats &ns = *pStats;
        size_t iBins = (size_t)( llLastUs / (uint64_t)llBinUs ) + 1;
        ns = NoteStats();
        ns.llBinUs = llBinUs;
        ns.vOns.assign( iBins, 0 );
        std::vector< int64_t > vDelta( iBins, 0 );
        for ( const StatsRecorder &r : vRec )
            r.ForEach( [&]( uint64_t b, uint64_t llOns, uint64_t llOffs ) {
                ns.vOns[b] = (uint32_t)std::min< uint64_t >( (uint64_t)ns.vOns[b] + llOns, UINT32_MAX );
                vDelta[b] += (int64_t)llOns - (int64_t)llOffs;
            } );
        vRec.clear();
        ns.vPoly.resize( iBins );
        ns.vCum.resize( ( ( iBins - 1 ) >> NoteStats::CumShift ) + 1 );
        int64_t llPoly = 0;
        uint64_t llCum = 0;
        for ( size_t b = 0; b < iBins; b++ )
        {
            if ( !( b & ( ( size_t( 1 ) << NoteStats::CumShift ) - 1 ) ) ) ns.vCum[b >> NoteStats::CumShift] = llCum;
            llCum += ns.vOns[b];
            llPoly += vDelta[b];
            ns.vPoly[b] = (uint32_t)std::min< int64_t >( std::max< int64_t >( llPoly, 0 ), UINT32_MAX );
        }
    }

    Parallel::For( iThreads, 128, [&]( size_t k, int ) {
        if ( bStream ) { vStreamKeys[k]->Finish(); vs.aKeys[k] = std::move( vCompact[k] ); vStream[k].reset(); }
        else vBuilders[k]->Finish();
    } );
    Mark( VisualStore::StepFinish );

    if ( bStream )
    {
        for ( int k = 0; k < 128; k++ )
        {
            const StreamKey &kb = *vStreamKeys[k];
            vs.llUnpaired += kb.UnpairedCount();
            vs.llNotes += kb.FullSize() - kb.UnpairedCount();
            if ( kb.FirstPairedStartU() != UINT64_MAX )
            {
                long long t = (long long)es.times.Get( kb.FirstPairedStartU() );
                if ( vs.llFirstNote < 0 || t < vs.llFirstNote ) vs.llFirstNote = t;
            }
        }
        vs.bCompact = true;
        vs.llStored = 0;
        for ( const KeyStore &ks : vs.aKeys ) vs.llStored += ks.Size();
        return true;
    }
    for ( int k = 0; k < 128; k++ ) vs.bWeird |= vBuilders[k]->Weird();
    if ( vs.bWeird && bStopOnWeird ) return true;
    for ( int k = 0; k < 128; k++ )
    {
        const KeyStore &ks = vs.aKeys[k];
        vs.llUnpaired += ks.UnpairedCount();
        vs.llNotes += ks.Size() - ks.UnpairedCount();
        vs.llWeirdNotes += ks.WeirdCount();
        // First paired note of the key
        size_t i = 0;
        while ( i < ks.Size() && ks.IsUnpaired( i ) ) i++;
        if ( i < ks.Size() )
        {
            long long t = (long long)es.times.Get( ks.StartU( i ) );
            if ( vs.llFirstNote < 0 || t < vs.llFirstNote ) vs.llFirstNote = t;
        }
    }
    if ( vs.bWeird && bEndRanks ) BuildJumpTables( vs, U, iThreads );
    Mark( VisualStore::StepJump );
    vs.llStored = 0;
    for ( const KeyStore &ks : vs.aKeys ) vs.llStored += ks.Size();
    return true;
}
}

bool BuildVisualStore( Song &song, int iThreads, std::atomic< uint64_t > *pDone, const std::atomic< bool > *pCancel, bool bCompact, bool bLz,
                       bool bNoteStats )
{
    NoteStats *pStats = bNoteStats ? &song.noteStats : nullptr;
    // The compacting path pairs without end ordinals first; a song with weird notes is paired again
    // with them, for the jump-walk tables
    double adSteps[VisualStore::StepCount] = {};
    uint64_t adPrivate[VisualStore::StepCount] = {};
    if ( !PairAll( song, song.visual, iThreads, pDone, pCancel, !bCompact, bCompact, adSteps, bCompact, nullptr, pStats ) ) return false;
    std::copy( song.visual.adStepPrivate, song.visual.adStepPrivate + VisualStore::StepCount, adPrivate );
    if ( song.visual.bWeird && bCompact )
    {
        song.visual = VisualStore();
        double adAgain[VisualStore::StepCount] = {};
        if ( !PairAll( song, song.visual, iThreads, pDone, pCancel, true, false, adAgain, false, nullptr, pStats ) ) return false;
        for ( double d : adAgain ) adSteps[VisualStore::StepRepair] += d;
        adPrivate[VisualStore::StepRepair] = PrivateBytes();
    }
    long long llStart = Timer::NowMicroSecs();
    if ( bCompact )
    {
        if ( !song.visual.bCompact ) CompactStore( song.visual, song.stream.times, nullptr, iThreads, song.visual.bWeird );
        adSteps[VisualStore::StepCompact] = ( Timer::NowMicroSecs() - llStart ) / 1e6;
        adPrivate[VisualStore::StepCompact] = PrivateBytes();
        llStart = Timer::NowMicroSecs();
        if ( bLz ) Parallel::For( iThreads, 128, [&]( size_t k, int ) { song.visual.aKeys[k].ToLz(); } );
        adSteps[VisualStore::StepLz] = ( Timer::NowMicroSecs() - llStart ) / 1e6;
        adPrivate[VisualStore::StepLz] = PrivateBytes();
    }
    else
    {
        Parallel::For( iThreads, 128, [&]( size_t k, int ) { song.visual.aKeys[k].BuildFullTrees( song.stream.times ); } );
        adSteps[VisualStore::StepCompact] = ( Timer::NowMicroSecs() - llStart ) / 1e6;
        adPrivate[VisualStore::StepCompact] = PrivateBytes();
    }
    std::copy( adSteps, adSteps + VisualStore::StepCount, song.visual.adStepSeconds );
    std::copy( adPrivate, adPrivate + VisualStore::StepCount, song.visual.adStepPrivate );
    return true;
}

bool BuildFilteredStore( const Song &song, const std::vector< uint8_t > &vHiddenSlot, VisualStore &out, int iThreads,
                         const std::atomic< bool > *pCancel )
{
    // song.visual was culled with every note visible: pair again and cull among the visible notes
    // (compacting while pairing when the song has no weird note: the pairing is the same as the
    // song's, which never pairs a note twice)
    if ( !PairAll( song, out, iThreads, nullptr, pCancel, false, false, nullptr, !song.visual.bWeird, &vHiddenSlot ) ) return false;
    if ( !out.bCompact ) CompactStore( out, song.stream.times, &vHiddenSlot, iThreads, false );
    out.llNotes = out.llStored;
    out.llUnpaired = 0;
    out.llWeirdNotes = 0;
    for ( const KeyStore &ks : out.aKeys ) out.llWeirdNotes += ks.WeirdCount();
    return true;
}
