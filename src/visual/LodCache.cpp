#include "visual/LodCache.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

#include "core/ThreadPool.h"
#include "visual/Scene.h"

namespace
{
    // How many segments around the playhead may be kept beyond what a frame wants
    const long long KeepMargin = 2;

    inline long long FloorDiv( long long a, long long b ) { long long q = a / b; return ( a % b != 0 && ( ( a < 0 ) != ( b < 0 ) ) ) ? q - 1 : q; }
}

bool LodCache::View::Covers( long long t0, long long t1 ) const
{
    if ( t1 <= t0 ) return true;
    long long a = FloorDiv( t0, llSegUs ), b = FloorDiv( t1 - 1, llSegUs );
    for ( long long i = a; i <= b; i++ )
    {
        long long j = i - llFirst;
        if ( j < 0 || j >= (long long)v.size() || !v[(size_t)j] ) return false;
    }
    return true;
}

const LodCache::Segment *LodCache::View::At( long long t ) const
{
    long long j = FloorDiv( t, llSegUs ) - llFirst;
    return j >= 0 && j < (long long)v.size() ? v[(size_t)j].get() : nullptr;
}

size_t LodCache::Segment::LowerBound( int k, uint64_t llPos ) const
{
    const std::vector< Entry > &v = av[k];
    return (size_t)( std::lower_bound( v.begin(), v.end(), llPos, []( const Entry &e, uint64_t p ) { return e.Pos() < p; } ) - v.begin() );
}

LodCache::LodCache()
{
    m_Thread = std::thread( &LodCache::Worker, this );
}

LodCache::~LodCache()
{
    {
        std::lock_guard< std::mutex > lock( m_mx );
        m_bStop = true;
        m_uGen++;
    }
    m_cv.notify_all();
    if ( m_Thread.joinable() ) m_Thread.join();
}

void LodCache::Configure( const Setting &s, const std::vector< long long > &vTheta, std::shared_ptr< const VisualStore > pStore, unsigned uStoreGen,
                          const PackedMonotone *pTimes )
{
    std::lock_guard< std::mutex > lock( m_mx );
    if ( m_S.Same( s ) && m_pStore.get() == pStore.get() && m_uStoreGen == uStoreGen ) return;
    m_S = s;
    m_S.vTheta = vTheta;
    m_pStore = std::move( pStore );
    m_uStoreGen = uStoreGen;
    m_pTimes = pTimes;
    m_uGen++;
    for ( auto &p : m_vSeg ) if ( p ) m_vRetired.push_back( std::move( p ) );
    m_vSeg.clear();
    if ( !m_vRetired.empty() ) m_cv.notify_one();
    m_vWanted.clear();
    m_iBytes = 0;
    m_vRes.clear();
    for ( long long t : m_S.vTheta )
    {
        long long r = t % s.llUsPP;
        if ( r < 0 ) r += s.llUsPP;
        m_vRes.push_back( r );
    }
    std::sort( m_vRes.begin(), m_vRes.end() );
    m_vRes.erase( std::unique( m_vRes.begin(), m_vRes.end() ), m_vRes.end() );
}

void LodCache::Get( View &v, long long t0, long long t1, long long llPrefetchEnd, bool bSync, int iThreads )
{
    v.v.clear();
    std::unique_lock< std::mutex > lock( m_mx );
    if ( !m_pStore ) return;
    const long long U = m_S.SegmentUs();
    long long a = FloorDiv( t0, U ), b = FloorDiv( std::max( t0, t1 - 1 ), U );
    long long c = std::max( b, FloorDiv( llPrefetchEnd, U ) );

    // Keep [a - margin, c + margin]; drop the rest
    long long llKeepLo = a - KeepMargin, llKeepHi = c + KeepMargin;
    if ( llKeepLo != m_llKeepLo || llKeepHi != m_llKeepHi )
    {
        m_llKeepLo = llKeepLo;
        m_llKeepHi = llKeepHi;
        if ( !m_vSeg.empty() )
        {
            std::vector< std::shared_ptr< const Segment > > vKeep( (size_t)( llKeepHi - llKeepLo + 1 ) );
            for ( size_t i = 0; i < m_vSeg.size(); i++ )
            {
                long long idx = m_llFirst + (long long)i;
                if ( m_vSeg[i] && idx >= llKeepLo && idx <= llKeepHi ) vKeep[(size_t)( idx - llKeepLo )] = m_vSeg[i];
                else if ( m_vSeg[i] ) { m_iBytes -= m_vSeg[i]->iBytes; m_vRetired.push_back( std::move( m_vSeg[i] ) ); }
            }
            m_vSeg.swap( vKeep );
            if ( !m_vRetired.empty() ) m_cv.notify_one();
        }
        else m_vSeg.assign( (size_t)( llKeepHi - llKeepLo + 1 ), nullptr );
        m_llFirst = llKeepLo;
    }
    auto Ready = [&]( long long i ) { long long j = i - m_llFirst; return j >= 0 && j < (long long)m_vSeg.size() && m_vSeg[(size_t)j]; };

    if ( bSync )
    {
        unsigned uGen = m_uGen;
        for ( long long i = a; i <= b; i++ )
        {
            if ( Ready( i ) ) continue;
            lock.unlock();
            std::shared_ptr< Segment > p = Build( i, iThreads, &m_uGen, uGen );
            lock.lock();
            if ( !p || m_uGen != uGen ) return;
            long long j = i - m_llFirst;
            if ( j >= 0 && j < (long long)m_vSeg.size() && !m_vSeg[(size_t)j] ) { m_vSeg[(size_t)j] = p; m_iBytes += p->iBytes; }
        }
    }

    // What the worker should build: what this frame needs first, then ahead
    m_vWanted.clear();
    for ( long long i = a; i <= c; i++ ) if ( !Ready( i ) ) m_vWanted.push_back( i );
    if ( !m_vWanted.empty() ) m_cv.notify_one();

    v.llFirst = a;
    v.llSegUs = U;
    for ( long long i = a; i <= b; i++ )
        v.v.push_back( Ready( i ) ? m_vSeg[(size_t)( i - m_llFirst )] : nullptr );
}

void LodCache::Retire( View &v )
{
    std::lock_guard< std::mutex > lock( m_mx );
    for ( auto &p : v.v ) if ( p ) m_vRetired.push_back( std::move( p ) );
    v.v.clear();
    if ( !m_vRetired.empty() ) m_cv.notify_one();
}

size_t LodCache::MemoryBytes() const
{
    std::lock_guard< std::mutex > lock( m_mx );
    return m_iBytes;
}

void LodCache::Worker()
{
    std::unique_lock< std::mutex > lock( m_mx );
    for ( ;; )
    {
        m_cv.wait( lock, [&]() { return m_bStop || !m_vWanted.empty() || !m_vRetired.empty(); } );
        if ( m_bStop ) return;
        if ( !m_vRetired.empty() )
        {
            // Free outside the lock (the last reference of most of these is here)
            std::vector< std::shared_ptr< const Segment > > v;
            v.swap( m_vRetired );
            lock.unlock();
            v.clear();
            lock.lock();
            continue;
        }
        long long i = m_vWanted.front();
        m_vWanted.erase( m_vWanted.begin() );
        long long j = i - m_llFirst;
        if ( j < 0 || j >= (long long)m_vSeg.size() || m_vSeg[(size_t)j] ) continue;
        unsigned uGen = m_uGen;
        lock.unlock();
        // A quarter of the cores: the frames keep the rest
        int iThreads = std::max( 1, Parallel::ThreadCount( 0 ) / 4 );
        std::shared_ptr< Segment > p = Build( i, iThreads, &m_uGen, uGen );
        lock.lock();
        if ( !p || m_uGen != uGen ) continue;
        j = i - m_llFirst;
        if ( j >= 0 && j < (long long)m_vSeg.size() && !m_vSeg[(size_t)j] ) { m_vSeg[(size_t)j] = p; m_iBytes += p->iBytes; }
    }
}

std::shared_ptr< LodCache::Segment > LodCache::Build( long long llIndex, int iThreads, const std::atomic< unsigned > *pCancelGen, unsigned uGen )
{
    // A consistent snapshot of what the segment is built for
    Setting S;
    std::vector< long long > vRes;
    std::shared_ptr< const VisualStore > pStore;
    const PackedMonotone *pTimes;
    {
        std::lock_guard< std::mutex > lock( m_mx );
        if ( m_uGen != uGen ) return nullptr;
        S = m_S;
        vRes = m_vRes;
        pStore = m_pStore;
        pTimes = m_pTimes;
    }
    if ( !pStore || !pTimes || S.llUsPP <= 0 || vRes.empty() ) return nullptr;
    const long long t0 = llIndex * S.SegmentUs(), t1 = t0 + S.SegmentUs();
    const long long R = (long long)vRes.size(), U = S.llUsPP;
    const int iCap = S.iRowHi - S.iRowLo + 8;      // every height from here on reaches every composited row
    auto Height = [&]( long long D ) {
        float cy = S.fNotesCY * ( static_cast< float >( D ) / S.llSpan );
        cy = std::floor( cy + 0.5f );
        return cy >= (float)iCap ? iCap : cy <= 0.0f ? 0 : (int)cy;
    };
    auto Cell = [&]( long long t ) {
        long long k = FloorDiv( t, U ), m = t - k * U;
        return k * R + (long long)( std::upper_bound( vRes.begin(), vRes.end(), m ) - vRes.begin() );
    };
    auto LowerBoundTime = [&]( long long t ) { return t <= 0 ? (uint64_t)0 : pTimes->LowerBound( (uint64_t)t ); };
    const bool bMinHeight = ( S.uClean & Clean::MinHeight ) != 0, bEdgeRound = ( S.uClean & Clean::EdgeRound ) != 0;

    auto pSeg = std::make_shared< Segment >();
    pSeg->llIndex = llIndex;
    pSeg->llStartUs = t0;
    std::atomic< bool > bCancelled{ false };
    Parallel::For( iThreads, 256, [&]( size_t k, int ) {
        const KeyStore &ks = pStore->aKeys[k];
        if ( !ks.Size() ) return;
        size_t i0 = ks.LowerBoundStart( LowerBoundTime( t0 ) ), i1 = ks.LowerBoundStart( LowerBoundTime( t1 ) );
        std::vector< Entry > &vOut = pSeg->av[k];
        std::vector< std::pair< uint32_t, long long > > &vLong = pSeg->avLong[k];
        struct Note { uint64_t llPos; bool bWeird; long long llStart, llEnd; uint32_t uSlot; int h; };
        std::vector< Note > vCell, vKeep;
        // Suffix maximum of kept ends over heights (Fenwick tree on reversed heights)
        std::vector< long long > vTree( iCap + 2, LLONG_MIN );
        std::vector< int > vTouched;
        auto Update = [&]( int h, long long e ) {
            for ( int x = iCap + 1 - h; x <= iCap + 1; x += x & -x )
            {
                if ( vTree[x] == LLONG_MIN ) vTouched.push_back( x );
                vTree[x] = std::max( vTree[x], e );
            }
        };
        auto MaxFrom = [&]( int h ) {   // max end over kept heights >= h
            long long m = LLONG_MIN;
            for ( int x = iCap + 1 - h; x > 0; x -= x & -x ) m = std::max( m, vTree[x] );
            return m;
        };
        auto FlushCell = [&]() {
            vKeep.clear();
            for ( size_t q = vCell.size(); q-- > 0; )
            {
                const Note &n = vCell[q];
                if ( n.bWeird ) { vKeep.push_back( n ); continue; }
                if ( MaxFrom( n.h ) >= n.llEnd ) continue;    // a later note covers it in every frame
                vKeep.push_back( n );
                Update( n.h, n.llEnd );
            }
            for ( int x : vTouched ) vTree[x] = LLONG_MIN;
            vTouched.clear();
            for ( size_t q = vKeep.size(); q-- > 0; )
            {
                const Note &n = vKeep[q];
                long long llDur = n.llEnd - n.llStart;
                Entry e;
                e.uPosStart = n.llPos | ( (uint64_t)n.bWeird << Entry::WeirdShift ) | ( (uint64_t)( n.llStart - t0 ) << Entry::StartShift );
                e.uSlot = n.uSlot;
                e.uDur = llDur < (long long)Entry::LongDur ? (uint32_t)llDur : Entry::LongDur;
                if ( e.uDur == Entry::LongDur ) vLong.push_back( { (uint32_t)vOut.size(), llDur } );
                vOut.push_back( e );
            }
            vCell.clear();
        };
        long long llCell = LLONG_MIN;
        // In spans read in one pass (KeyStore::GetRange: an LZ store's group cache is left to the frames)
        const size_t SpanNotes = 1024;
        std::unique_ptr< NoteView[] > pSpan( new NoteView[SpanNotes] );
        for ( size_t i1Span, iSpan = i0; iSpan < i1; iSpan = i1Span )
        {
            if ( pCancelGen->load( std::memory_order_relaxed ) != uGen ) { bCancelled = true; return; }
            i1Span = std::min( i1, iSpan + SpanNotes );
            ks.GetRange( iSpan, i1Span - iSpan, pSpan.get() );
            for ( size_t i = iSpan; i < i1Span; i++ )
            {
                const NoteView &n = pSpan[i - iSpan];
                if ( n.bUnpaired ) continue;
                long long s = (long long)pTimes->Get( n.llStartU ), e = (long long)pTimes->Get( n.llEndU );
                int h = Height( e - s );
                long long c = Cell( s );
                // Clean drawing (see the header): with EdgeRound one height for all, the end alone deciding
                if ( bEdgeRound ) h = bMinHeight || Cell( e ) != c ? 1 : 0;
                else if ( bMinHeight ) h = std::max( h, 1 );
                if ( h <= 0 ) continue;                 // the bucket sweep never selects a note of height 0
                if ( c != llCell ) { FlushCell(); llCell = c; }
                vCell.push_back( { (uint64_t)i, n.bWeird, s, e, n.uSlot, h } );
            }
        }
        FlushCell();
        vOut.shrink_to_fit();
        vLong.shrink_to_fit();
    } );
    if ( bCancelled ) return nullptr;
    for ( int k = 0; k < 256; k++ )
        pSeg->iBytes += pSeg->av[k].capacity() * sizeof( Entry ) + pSeg->avLong[k].capacity() * sizeof( pSeg->avLong[k][0] );
    return pSeg;
}
