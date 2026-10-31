#include "visual/FastEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include "core/ThreadPool.h"
#include "core/Util.h"
#include "midi/Notes.h"

using std::floor;

FastEngine::FastEngine( const Song &song, int iThreads )
    : m_Song( song ), m_Store( song.visual ), m_Times( song.stream.times ), m_iThreads( Parallel::ThreadCount( iThreads ) )
{
    Reset();
}

FastEngine::~FastEngine()
{
    m_bCancelBuild = true;
    if ( m_Builder.joinable() ) m_Builder.join();
}

void FastEngine::SetHidden( const std::vector< uint8_t > &vHiddenSlot, bool bSync )
{
    bool bAny = false;
    for ( uint8_t b : vHiddenSlot ) bAny = bAny || b;
    m_vMask = bAny ? vHiddenSlot : std::vector< uint8_t >();
    if ( !bAny || ( m_pView && m_vViewMask == m_vMask ) )
    {
        if ( !bAny ) { m_pView.reset(); m_vViewMask.clear(); m_uViewGen++; }
        m_bCancelBuild = true;
        return;
    }
    if ( m_Builder.joinable() && m_vBuildMask == m_vMask && !m_bCancelBuild ) return; // already building it
    m_bCancelBuild = true;
    if ( m_Builder.joinable() ) m_Builder.join();
    m_bCancelBuild = false;
    m_vBuildMask = m_vMask;
    if ( bSync )
    {
        auto p = std::make_shared< VisualStore >();
        if ( BuildFilteredStore( m_Song, m_vMask, *p, m_iThreads, &m_bCancelBuild ) )
        {
            m_pView = p;
            m_vViewMask = m_vMask;
            m_uViewGen++;
        }
        return;
    }
    std::vector< uint8_t > vMask = m_vMask;
    int iThreads = m_iThreads;
    m_Builder = std::thread( [this, vMask, iThreads]() {
        auto p = std::make_shared< VisualStore >();
        if ( BuildFilteredStore( m_Song, vMask, *p, iThreads, &m_bCancelBuild ) )
        {
            std::lock_guard< std::mutex > lock( m_mxReady );
            m_pReady = p;
            m_vReadyMask = vMask;
        }
    } );
}

void FastEngine::SetLod( bool bOn, bool bSync )
{
    m_bLod = bOn;
    m_bLodSync = bSync;
    if ( bOn && !m_pLod ) m_pLod.reset( new LodCache() );
    if ( !bOn ) { m_LodView = LodCache::View(); m_pLod.reset(); }
}

bool FastEngine::ViewPending() const
{
    return !m_vMask.empty() && !( m_pView && m_vViewMask == m_vMask );
}

void FastEngine::CposFor( const VisualStore &S, uint64_t *pCpos ) const
{
    for ( int k = 0; k < 256; k++ )
        pCpos[k] = m_iCut == 0 ? 0 : m_iCut == 1 ? UINT64_MAX : S.aKeys[k].LowerBoundOrder( m_llCutU, m_llCutOrder );
}

//-----------------------------------------------------------------------------
// State: the same four values ModelEngine keeps (DESIGN.md R4, R9)
//-----------------------------------------------------------------------------

void FastEngine::Reset()
{
    m_llJump = LLONG_MIN;
    m_bResumed = false;
    m_llProcessed = LLONG_MIN;
    m_llEndHW = LLONG_MIN;
    m_iCut = 0;
    m_uJumpGen++;
    for ( uint64_t &c : m_aCpos ) c = 0;
}

void FastEngine::AdvanceEnd( long long llEndTime )
{
    m_llEndHW = std::max( m_llEndHW, llEndTime );
}

void FastEngine::Process( long long llStartTime )
{
    m_bResumed = true;
    m_llProcessed = std::max( m_llProcessed, llStartTime );
}

void FastEngine::Jump( long long llTarget, long long, long long llEndTime )
{
    m_llJump = llTarget;
    m_bResumed = false;
    m_llProcessed = LLONG_MIN;
    m_llEndHW = llEndTime;
    m_iCut = 0;
    m_uJumpGen++;
    if ( m_Store.bWeird ) ComputeJumpCutoff();
    CposFor( m_Store, m_aCpos );
}

uint64_t FastEngine::LowerBoundTime( long long t ) const
{
    return t <= 0 ? 0 : m_Times.LowerBound( (uint64_t)t );
}

uint64_t FastEngine::UpperBoundTime( long long t ) const
{
    return t < 0 ? 0 : m_Times.UpperBound( (uint64_t)t );
}

void FastEngine::GetBounds( const VisualStore &S, int k, Bounds &b ) const
{
    const KeyStore &ks = S.aKeys[k];
    b.iJ = m_llJump == LLONG_MIN ? 0 : ks.LowerBoundStart( LowerBoundTime( m_llJump ) );
    if ( m_bResumed )
    {
        size_t iP = ks.LowerBoundStart( UpperBoundTime( m_llProcessed ) );
        b.iB = std::max( b.iJ, iP );
        b.llEth = std::max( m_llProcessed, m_llJump );
    }
    else
    {
        b.iB = b.iJ;
        b.llEth = m_llJump;
    }
    b.iE = m_llEndHW == LLONG_MIN ? 0 : ks.LowerBoundStart( LowerBoundTime( m_llEndHW ) );
    if ( b.iE < b.iB ) b.iE = b.iB;
    b.llUEth1 = b.llEth == LLONG_MIN ? 0 : LowerBoundTime( b.llEth + 1 );
}

// Sounding (or jump-carried) for a note before iB; notes in [iB, iE) are upcoming and always visible
bool FastEngine::Visible( const VisualStore &S, const uint64_t *pCpos, int k, size_t pos, const NoteView &n, const Bounds &b ) const
{
    if ( n.bUnpaired ) return false;
    if ( pos < b.iJ && pos < pCpos[k] ) return false;            // left out by the jump walk
    uint32_t uCount = 0;
    const OffRef *pOffs = n.bWeird ? S.aKeys[k].WeirdOffs( pos, uCount ) : nullptr;
    if ( !pOffs ) return Time( n.llEndU ) > b.llEth;
    // Re-paired notes: every off pointing at the note removes it (UpdateState)
    if ( !m_bResumed ) return Time( n.llEndU ) > m_llJump;
    if ( pos >= b.iJ ) return Time( pOffs[0].llU ) > m_llProcessed;
    if ( Time( n.llEndU ) <= m_llJump ) return false;
    for ( uint32_t i = 0; i < uCount; i++ )
    {
        long long t = Time( pOffs[i].llU );
        if ( t >= m_llJump && t <= m_llProcessed ) return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// The original's float geometry (Scene::DrawNote, R6)
//-----------------------------------------------------------------------------

int FastEngine::Beta( long long d ) const
{
    float y = m_F.fNotesY + m_F.fNotesCY * ( 1.0f - static_cast< float >( d ) / m_F.llSpan );
    y = floor( y + 0.5f );
    return y > 1e9f ? 1000000000 : y < -1e9f ? -1000000000 : (int)y;
}

int FastEngine::Height( long long D ) const
{
    float cy = m_F.fNotesCY * ( static_cast< float >( D ) / m_F.llSpan );
    cy = floor( cy + 0.5f );
    return cy > 1e9f ? 1000000000 : cy < -1e9f ? -1000000000 : (int)cy;
}

// Pixel rows [a, b) the outer rect of a note covers, after the original clipping
bool FastEngine::Coverage( long long s, long long e, int &a, int &b, int iBottom ) const
{
    float cy = m_F.fNotesCY * ( static_cast< float >( e - s ) / m_F.llSpan );
    cy = floor( cy + 0.5f );
    // Beta's expression rounded the same way: the row (a whole float, exact well within 2^24)
    float y = iBottom != INT_MIN ? (float)iBottom : floor( m_F.fNotesY + m_F.fNotesCY * ( 1.0f - static_cast< float >( s - m_F.R ) / m_F.llSpan ) + 0.5f );
    // Clean drawing, as Scene::NoteVertices
    if ( m_F.uClean & Clean::EdgeRound ) cy = y - floor( m_F.fNotesY + m_F.fNotesCY * ( 1.0f - static_cast< float >( e - m_F.R ) / m_F.llSpan ) + 0.5f );
    if ( ( m_F.uClean & Clean::MinHeight ) && cy < 1.0f ) cy = 1.0f;
    float fMinY = m_F.fNotesY - 5.0f;
    float fMaxY = m_F.fNotesY + m_F.fNotesCY + 5.0f;
    if ( y > fMaxY )
    {
        cy -= ( y - fMaxY );
        y = fMaxY;
    }
    if ( y - cy < fMinY )
    {
        cy -= ( fMinY - ( y - cy ) );
        y = fMinY + cy;
    }
    if ( !( cy > 0.0f ) ) return false;     // empty, or culled (one negative extent)
    float fTop = ( y - cy ) - 0.5f;         // Renderer::DrawRect's vertex rows
    float fBottom = fTop + cy;
    if ( fTop > 1e9f || fBottom < -1e9f ) return false;
    a = (int)std::ceil( std::max( fTop, -1e9f ) );
    b = (int)std::ceil( std::min( fBottom, 1e9f ) );
    return a < b;
}

void FastEngine::PrepareFrame( const FrameGlobals &g )
{
    Frame &F = m_F;
    F.R = g.llRndStartTime;
    F.llSpan = g.llTimeSpan;
    F.llUsPP = std::max( 1LL, static_cast< long long >( g.llTimeSpan / g.fNotesCY + 0.5f ) );   // as Scene::ComputeGlobals
    F.fNotesY = g.fNotesY;
    F.fNotesCY = g.fNotesCY;
    F.uClean = g.uClean;
    float fDeflate = g.fWhiteCX * 0.15f / 2.0f;
    fDeflate = floor( fDeflate + 0.5f );
    F.fDeflate = std::max( std::min( fDeflate, 3.0f ), 1.0f );
    float fMinY = g.fNotesY - 5.0f, fMaxY = g.fNotesY + g.fNotesCY + 5.0f;
    F.iRowLo = std::max( 0, (int)std::ceil( fMinY - 0.5f ) );
    F.iRowHi = std::min( g.iBufferHeight, (int)std::ceil( fMaxY - 0.5f ) );
    if ( F.iRowHi < F.iRowLo ) F.iRowHi = F.iRowLo;

    // Settings-time tables (8.3): Theta for every composited row, G for every height
    if ( m_fCacheY != F.fNotesY || m_fCacheCY != F.fNotesCY || m_llCacheSpan != F.llSpan || m_iCacheLo != F.iRowLo || m_iCacheHi != F.iRowHi )
    {
        m_fCacheY = F.fNotesY;
        m_fCacheCY = F.fNotesCY;
        m_llCacheSpan = F.llSpan;
        m_iCacheLo = F.iRowLo;
        m_iCacheHi = F.iRowHi;
        long long llRange = F.llSpan * 64 + 1000000000LL;
        F.iThetaLo = F.iRowLo - 1;
        F.vTheta.resize( F.iRowHi - F.iRowLo + 4 );
        for ( size_t i = 0; i < F.vTheta.size(); i++ )
        {
            int b = F.iThetaLo + (int)i;
            long long lo = -llRange, hi = llRange;
            while ( lo < hi )
            {
                long long mid = lo + ( ( hi - lo ) >> 1 );
                if ( Beta( mid ) <= b ) hi = mid; else lo = mid + 1;
            }
            F.vTheta[i] = lo;
        }
        F.vG.resize( F.iRowHi - F.iRowLo + 4 );
        for ( size_t h = 0; h < F.vG.size(); h++ )
        {
            long long lo = 0, hi = llRange;
            while ( lo < hi )
            {
                long long mid = lo + ( ( hi - lo ) >> 1 );
                if ( Height( mid ) >= (int)h ) hi = mid; else lo = mid + 1;
            }
            F.vG[h] = lo;
        }
    }
    // From one frame to the next R moves by a few rows, so each row's tick is searched for from where
    // it was last frame (or from the row before: Theta decreases down the rows)
    bool bHints = F.vURow.size() == F.vTheta.size();
    F.vURow.resize( F.vTheta.size() );
    for ( size_t i = 0; i < F.vTheta.size(); i++ )
    {
        long long t = F.R + F.vTheta[i];
        size_t iHint = bHints ? (size_t)F.vURow[i] : i ? (size_t)F.vURow[i - 1] : 0;
        F.vURow[i] = t <= 0 ? 0 : m_Times.LowerBoundFrom( (uint64_t)t, iHint );
    }
}

//-----------------------------------------------------------------------------
// Per-key compositing (8.5)
//-----------------------------------------------------------------------------

void FastEngine::SweepKey( const VisualStore &S, const uint64_t *pCpos, int k, const Bounds &bd, const std::vector< TrackSettings > &vTracks,
                           bool bDegenerate, std::vector< Owner > &vOwners, std::vector< uint64_t > &vRows, size_t &iQueries ) const
{
    vOwners.clear();
    const KeyStore &ks = S.aKeys[k];
    if ( !ks.Size() ) return;
    auto Hidden = [&]( uint32_t uSlot ) {
        return m_bSkipHidden && ( uSlot >> 4 ) < vTracks.size() && vTracks[uSlot >> 4].aChannels[uSlot & 15].bHidden;
    };
    NoteView n;

    // Keys too narrow for the inner rect: its flipped quad leaves the key's rows and columns, so every
    // visible note is drawn, as the original does (8.9)
    if ( bDegenerate )
    {
        size_t hi = bd.iB;
        int64_t j;
        while ( ( j = ks.RightmostEnd( 0, hi, bd.llUEth1 ) ) >= 0 )
        {
            iQueries++;
            ks.Get( (size_t)j, n );
            if ( Visible( S, pCpos, k, (size_t)j, n, bd ) && !Hidden( n.uSlot ) )
                vOwners.push_back( { (uint64_t)j, Time( n.llStartU ), Time( n.llEndU ), n.uSlot, INT_MIN, 0, false } );
            hi = (size_t)j;
        }
        std::reverse( vOwners.begin(), vOwners.end() );
        for ( size_t i = bd.iB; i < bd.iE; i++ )
        {
            ks.Get( i, n );
            if ( !n.bUnpaired && !Hidden( n.uSlot ) ) vOwners.push_back( { i, Time( n.llStartU ), Time( n.llEndU ), n.uSlot, INT_MIN, 0, false } );
        }
        return;
    }

    const Frame &F = m_F;
    int iRows = F.iRowHi - F.iRowLo;
    if ( iRows <= 0 ) return;
    // A bit per unpainted row, then a bit per row word that has any (so the highest is found in a step
    // or two however much below it is painted)
    size_t iWords = ( iRows + 63 ) / 64, iSum = ( iWords + 63 ) / 64;
    vRows.assign( iWords + iSum, ~0ull );
    uint64_t *pRows = vRows.data(), *pSum = pRows + iWords;
    if ( iRows & 63 ) pRows[iWords - 1] = ( 1ull << ( iRows & 63 ) ) - 1;
    if ( iWords & 63 ) pSum[iSum - 1] = ( 1ull << ( iWords & 63 ) ) - 1;

    // Highest (largest) unpainted row below iLimit
    auto MaxUnpainted = [&]( int iLimit ) -> int {
        int lim = std::min( iLimit, F.iRowHi ) - F.iRowLo;
        if ( lim <= 0 ) return INT_MIN;
        int w = ( lim - 1 ) >> 6;
        int nb = lim - 64 * w;
        uint64_t v = pRows[w] & ( nb >= 64 ? ~0ull : ( 1ull << nb ) - 1 );
        if ( v ) return F.iRowLo + 64 * w + 63 - __builtin_clzll( v );
        // The highest word below w with any
        if ( --w < 0 ) return INT_MIN;
        int s = w >> 6;
        uint64_t m = pSum[s] & ( ( w & 63 ) == 63 ? ~0ull : ( 2ull << ( w & 63 ) ) - 1 );
        while ( !m )
        {
            if ( --s < 0 ) return INT_MIN;
            m = pSum[s];
        }
        w = s * 64 + 63 - __builtin_clzll( m );
        return F.iRowLo + 64 * w + 63 - __builtin_clzll( pRows[w] );
    };
    // Marks the rows a note covers as painted, and adds it as an owner drawn over the span of the rows
    // that were still unpainted (empty span: owns nothing). Drawing only that span is exact: a row in
    // it that the note does not own is owned by a note drawn later, which covers it again. The owner is
    // written in place, field by field (a copy of one built apart stalls on store forwarding).
    auto Paint = [&]( uint64_t llPos, long long s, long long e, uint32_t uSlot, int iBottom ) -> Owner & {
        Owner &o = vOwners.emplace_back();
        o.llPos = llPos;
        o.llStart = s;
        o.llEnd = e;
        o.uSlot = uSlot;
        o.iRowA = INT_MAX;
        o.iRowB = INT_MIN;
        o.bShort = false;
        int a, b;
        if ( !Coverage( s, e, a, b, iBottom ) ) return o;
        // [a, b) is the clipped outer rect (b - a its height cy), which the inner rect insets by
        // fDeflate on each side: nothing of it is drawn when cy <= 2 * fDeflate
        o.bShort = (float)( b - a ) <= F.fDeflate * 2.0f;
        a = std::max( a, F.iRowLo ) - F.iRowLo;
        b = std::min( b, F.iRowHi ) - F.iRowLo;
        for ( int r = a; r < b; )
        {
            int w = r >> 6, r1 = std::min( b, ( w + 1 ) * 64 );
            int n0 = r & 63, n1 = r1 - w * 64;
            uint64_t mask = ( n1 >= 64 ? ~0ull : ( 1ull << n1 ) - 1 ) & ~( ( 1ull << n0 ) - 1 );
            if ( uint64_t v = pRows[w] & mask )
            {
                o.iRowA = std::min( o.iRowA, F.iRowLo + 64 * w + __builtin_ctzll( v ) );
                o.iRowB = std::max( o.iRowB, F.iRowLo + 64 * w + 64 - __builtin_clzll( v ) );
                if ( !( pRows[w] &= ~mask ) ) pSum[w >> 6] &= ~( 1ull << ( w & 63 ) );
            }
            r = r1;
        }
        return o;
    };

    // Buckets of equal bottom row, from the top of the screen down. Notes at or above the first
    // composited row cover nothing; within a bucket the bottom is fixed, so reaching row u is
    // exactly "duration >= G[bottom - u]".
    size_t hi = std::min( bd.iE, ks.LowerBoundStart( F.URow( F.iRowLo ) ) );
    int iBeta = F.iRowLo;
    while ( hi > 0 )
    {
        // The bucket of the last note left: Beta of its start time, which is the first row whose first
        // tick it starts at or after (URow is Theta's ticks, and Theta inverts Beta); the buckets only go down
        size_t i = hi - 1;
        uint64_t llS = ks.StartU( i );
        while ( iBeta < F.iRowHi && F.URow( iBeta ) > llS ) iBeta++;
        if ( iBeta >= F.iRowHi ) break;
        // From here down, from the LOD cache when every start below this bucket's top is built and
        // before Ehw (so no note the cache dropped could be drawn while the one covering it is not):
        // one pass over the cached notes from the top, a new bucket at every start below the current
        // bucket's range. The exact queries cost a store search per bucket; this costs one step per
        // cached note.
        long long tB = F.R + F.Theta( iBeta - 1 ), tLow = F.R + F.Theta( F.iRowHi - 1 );
        if ( m_bLodFrame && tB <= m_llEndHW && m_LodView.Covers( tLow, tB ) )
        {
            iQueries += size_t( 1 ) << 40;     // counted apart: LOD sweeps in the high bits
            int iB = iBeta, u = INT_MIN;
            long long tA = LLONG_MAX;
            bool bDone = false;
            for ( long long t = tB - 1; !bDone; )
            {
                const LodCache::Segment *pSeg = m_LodView.At( t );
                const std::vector< LodCache::Entry > &v = pSeg->av[k];
                for ( size_t q = pSeg->LowerBound( k, hi ); q-- > 0; )
                {
                    const LodCache::Entry &e = v[q];
                    long long llS = pSeg->Start( e );
                    if ( llS < tA )
                    {
                        // Its bucket, Beta( llS - R ): the first row whose Theta it reaches (from the
                        // last bucket's, as the starts only go down; the first is at or below iBeta)
                        while ( iB < F.iRowHi && F.Theta( iB ) > llS - F.R ) iB++;
                        if ( iB >= F.iRowHi ) { bDone = true; break; }
                        tA = F.R + F.Theta( iB );
                        u = MaxUnpainted( iB );
                    }
                    if ( u == INT_MIN ) continue;
                    long long llD = pSeg->Dur( k, q, e );
                    long long llE = llS + llD;
                    // Reaches row u (clean drawing as the exact queries below)
                    if ( !( ( F.uClean & Clean::MinHeight ) && iB - u <= 1 ) &&
                         ( ( F.uClean & Clean::EdgeRound ) ? llE - F.R < F.Theta( u ) : llD < F.vG[iB - u] ) ) continue;
                    size_t pos = (size_t)e.Pos();
                    bool bVisible;
                    if ( pos >= bd.iB ) bVisible = true;
                    else if ( e.Weird() ) { ks.Get( pos, n ); bVisible = Visible( S, pCpos, k, pos, n, bd ); }
                    else bVisible = !( pos < bd.iJ && pos < pCpos[k] ) && llE > bd.llEth;
                    if ( !bVisible ) continue;
                    Paint( pos, llS, llE, e.uSlot, iB );
                    u = MaxUnpainted( iB );
                }
                long long llSegStart = pSeg->llIndex * m_LodView.llSegUs;
                if ( bDone || llSegStart <= tLow ) break;
                t = llSegStart - 1;
            }
            // Where the exact sweep would have stopped: the first note with its bottom at or below iRowHi
            hi = std::min( hi, ks.LowerBoundStart( F.URow( F.iRowHi - 1 ) ) );
            break;
        }

        size_t lo = ks.LowerBoundStartBelow( F.URow( iBeta ), hi );
        int u = MaxUnpainted( iBeta );
        while ( u != INT_MIN && hi > lo )
        {
            // Clean drawing: a note at least a row tall reaches the bucket's last row whatever its
            // length; one whose top is rounded from its end reaches row u when its end maps at or above u
            bool bAny = ( F.uClean & Clean::MinHeight ) && iBeta - u <= 1;
            int64_t j = ( F.uClean & Clean::EdgeRound ) ? ks.RightmostEnd( lo, hi, bAny ? 0 : F.URow( u ) )
                                                        : ks.RightmostDur( lo, hi, bAny ? 0 : (uint64_t)F.vG[iBeta - u], m_Times );
            iQueries++;
            if ( j < 0 ) break;
            ks.Get( (size_t)j, n );
            bool bVisible = (size_t)j >= bd.iB || Visible( S, pCpos, k, (size_t)j, n, bd );
            if ( bVisible && !Hidden( n.uSlot ) )
            {
                Paint( (uint64_t)j, Time( n.llStartU ), Time( n.llEndU ), n.uSlot, iBeta );
                u = MaxUnpainted( iBeta );
            }
            hi = (size_t)j;
        }
        hi = lo;
    }

    // Notes whose bottom is below every composited row (sounding notes from the hit line down, and
    // upcoming ones too when the window cuts the field at the hit line): a note reaches the lowest
    // unpainted row u only if its end time maps at or above row u + 1. Upcoming notes are always
    // visible; sounding ones must also end after llEth.
    while ( hi > 0 )
    {
        int u = MaxUnpainted( F.iRowHi );
        if ( u == INT_MIN ) break;
        bool bUpcoming = hi > bd.iB;
        uint64_t llX = bUpcoming ? F.URow( u + 2 ) : std::max( F.URow( u + 2 ), bd.llUEth1 );
        int64_t j = ks.RightmostEnd( bUpcoming ? bd.iB : 0, hi, llX );
        iQueries++;
        if ( j < 0 )
        {
            if ( bUpcoming ) { hi = bd.iB; continue; }
            break;
        }
        ks.Get( (size_t)j, n );
        bool bVisible = (size_t)j >= bd.iB ? !n.bUnpaired : Visible( S, pCpos, k, (size_t)j, n, bd );
        if ( bVisible && !Hidden( n.uSlot ) )
        {
            const Owner &o = Paint( (uint64_t)j, Time( n.llStartU ), Time( n.llEndU ), n.uSlot, INT_MIN );
            if ( !( o.iRowA < o.iRowB ) ) vOwners.pop_back();
        }
        hi = (size_t)j;
    }
    std::reverse( vOwners.begin(), vOwners.end() );
}

//-----------------------------------------------------------------------------
// Drawing
//-----------------------------------------------------------------------------

void FastEngine::DrawNotes( Renderer *pRenderer, const FrameGlobals &g, const std::vector< TrackSettings > &vTracks )
{
    long long llStart = Timer::NowMicroSecs();
    m_Stats = FrameStats();
    m_vGlow.clear();
    for ( auto &v : m_aOwners ) v.clear();
    for ( size_t &c : m_aVertCount ) c = 0;
    if ( m_llEndHW == LLONG_MIN && !m_bResumed && m_llJump == LLONG_MIN ) return;

    // The draw view, when one matching the hidden channels is ready
    {
        std::lock_guard< std::mutex > lock( m_mxReady );
        if ( m_pReady )
        {
            if ( m_vReadyMask == m_vMask ) { m_pView = m_pReady; m_vViewMask = m_vReadyMask; m_uViewGen++; }
            m_pReady.reset();
        }
    }
    // What to draw from (see SetHidden)
    const VisualStore *pS = &m_Store;
    m_bSkipHidden = false;
    m_vDrawMask.clear();
    if ( !m_vMask.empty() )
    {
        if ( m_pView && m_vViewMask == m_vMask ) { pS = m_pView.get(); m_vDrawMask = m_vMask; }
        else if ( !m_Store.bCompact ) { m_bSkipHidden = true; m_vDrawMask = m_vMask; }
        else if ( m_pView ) { pS = m_pView.get(); m_vDrawMask = m_vViewMask; }
    }
    bool bView = pS != &m_Store;
    const VisualStore &S = *pS;
    if ( bView && ( m_uViewCposGen != m_uJumpGen || m_uViewCposView != m_uViewGen ) )
    {
        CposFor( S, m_aViewCpos );
        m_uViewCposGen = m_uJumpGen;
        m_uViewCposView = m_uViewGen;
    }
    const uint64_t *pCpos = bView ? m_aViewCpos : m_aCpos;
    m_Stats.bView = bView;
    PrepareFrame( g );
    if ( m_F.iRowHi <= m_F.iRowLo ) return;

    // The LOD cache for this setting and store (not while hidden notes are skipped one by one: a
    // note it dropped could be visible while the one covering it is hidden)
    m_bLodFrame = false;
    if ( m_bLod && m_pLod && !m_bSkipHidden )
    {
        LodCache::Setting &ls = m_LodSetting;
        ls.fNotesY = m_F.fNotesY;
        ls.fNotesCY = m_F.fNotesCY;
        ls.llSpan = m_F.llSpan;
        ls.llUsPP = m_F.llUsPP;
        ls.iRowLo = m_F.iRowLo;
        ls.iRowHi = m_F.iRowHi;
        ls.uClean = m_F.uClean & ( Clean::MinHeight | Clean::EdgeRound );
        std::shared_ptr< const VisualStore > pStore = bView ? m_pView : std::shared_ptr< const VisualStore >( std::shared_ptr< const VisualStore >(), &m_Store );
        m_pLod->Configure( ls, m_F.vTheta, pStore, bView ? 1 + 2 * m_uViewGen : 0, &m_Times );
        long long t0 = m_F.R + m_F.vTheta.back(), t1 = m_F.R + m_F.vTheta.front();
        long long llBuild = Timer::NowMicroSecs();
        m_pLod->Retire( m_LodView );                  // the segments only it held are freed off this thread
        m_pLod->Get( m_LodView, t0, t1, t1 + 2 * ls.SegmentUs(), m_bLodSync, m_iThreads );
        m_bLodFrame = !m_LodView.v.empty();
        // A synchronous build is not part of drawing the frame
        llBuild = Timer::NowMicroSecs() - llBuild;
        m_Stats.dLodBuildMs = llBuild / 1000.0;
        llStart += llBuild;
    }

    // Per key: columns (with the flipped inner rect of narrow keys), degenerate flag, on screen?
    float fDeflate = g.fWhiteCX * 0.15f / 2.0f;
    fDeflate = floor( fDeflate + 0.5f );
    fDeflate = std::max( std::min( fDeflate, 3.0f ), 1.0f );
    // (by Scene::KeyLayer: white keys, black keys, and the 31-EDO layout's full sharps and flats)
    std::vector< KeyCols > *aClass = m_aClass;
    for ( int c = 0; c < Scene::KeyLayers; c++ ) aClass[c].clear();
    for ( int k = 0; k < 256; k++ )
    {
        if ( !S.aKeys[k].Size() ) continue;
        const Scene::NoteKey nkCols = Scene::GetNoteKey( g, k );
        float x = nkCols.x;
        float cx = nkCols.cx;
        float fInner = cx - fDeflate * 2.0f;
        float fLo = x - 0.5f, fHi = fLo + cx;
        float fILo = ( x + fDeflate ) - 0.5f, fIHi = fILo + fInner;
        float fMin = std::min( fLo, std::min( fILo, fIHi ) ), fMax = std::max( fHi, std::max( fILo, fIHi ) );
        if ( !( fMax > -1e9f && fMin < 1e9f ) ) continue;
        KeyCols kc = { k, (int)std::ceil( fMin - 1.0f / 128 ), (int)std::ceil( fMax + 1.0f / 128 ), fInner < 0.0f };
        if ( kc.iHi <= 0 || kc.iLo >= g.iBufferWidth ) continue;
        aClass[Scene::KeyLayer( g, k )].push_back( kc );
    }

    std::vector< int > &vActive = m_vActive;
    vActive.clear();
    bool abDegenerate[256] = {};
    for ( int c = 0; c < Scene::KeyLayers; c++ )
        for ( const KeyCols &kc : aClass[c] ) { vActive.push_back( kc.iKey ); abDegenerate[kc.iKey] = kc.bDegenerate; }
    std::vector< size_t > &vQueries = m_vQueries;
    std::vector< long long > &vSweepUs = m_vSweepUs, &vVertUs = m_vVertUs;
    std::vector< double > &vPixels = m_vPixels;
    vQueries.assign( vActive.size(), 0 );
    vSweepUs.assign( vActive.size(), 0 );
    vVertUs.assign( vActive.size(), 0 );
    vPixels.assign( vActive.size(), 0.0 );
    std::vector< std::vector< uint64_t > > &vScratch = m_vScratch;
    if ( vScratch.size() < (size_t)m_iThreads + 1 ) vScratch.resize( m_iThreads + 1 );
    const bool bInst = pRenderer->InstancingOn() && m_Song.info.iMaxNote < 128;
    // Keys that may share a pixel column with another are drawn interleaved with it, owner by owner
    // (below), so only the others merge rects across owners
    bool abMerge[256] = {};
    for ( int c = 0; c < Scene::KeyLayers && m_bMergeRows; c++ )
    {
        std::vector< KeyCols > &v = aClass[c];
        std::sort( v.begin(), v.end(), []( const KeyCols &a, const KeyCols &b ) { return a.iLo < b.iLo; } );
        for ( size_t i = 0; i < v.size(); )
        {
            size_t j = i + 1;
            int iMaxHi = v[i].iHi;
            while ( j < v.size() && v[j].iLo < iMaxHi ) { iMaxHi = std::max( iMaxHi, v[j].iHi ); j++; }
            if ( j == i + 1 ) abMerge[v[i].iKey] = true;
            i = j;
        }
    }
    std::vector< size_t > &vMerged = m_vMergedCount;
    vMerged.assign( vActive.size(), 0 );
    Parallel::For( m_iThreads, vActive.size(), [&]( size_t i, int t ) {
        int k = vActive[i];
        long long tK0 = Timer::NowMicroSecs();
        Bounds bd;
        GetBounds( S, k, bd );
        // The key's owners, rows and query count in locals while it is swept: the vectors' headers sit
        // side by side in arrays, and other threads' keys writing theirs would share cache lines
        std::vector< Owner > vOwners;
        std::vector< uint64_t > vRows;
        vOwners.swap( m_aOwners[k] );
        vRows.swap( vScratch[t] );
        size_t iQueries = 0;
        SweepKey( S, pCpos, k, bd, vTracks, abDegenerate[k], vOwners, vRows, iQueries );
        vQueries[i] = iQueries;
        vScratch[t].swap( vRows );
        m_aOwners[k].swap( vOwners );
        long long tK1 = Timer::NowMicroSecs();
        // The owners' vertices, exactly as DrawNote makes them, minus rects the rasteriser culls
        // anyway (one negative extent: back-facing, R7; or zero area), such as the inner rect of a
        // note at most 2d rows tall
        // (or, instancing, one instance per owner with the rects it skips made empty)
        std::vector< Renderer::SCREEN_VERTEX > &vVerts = m_aVerts[k];
        std::vector< Renderer::NoteInstance > &vInst = m_aInst[k];
        std::vector< uint32_t > &vStart = m_aVertStart[k];
        if ( bInst ) { if ( vInst.size() < m_aOwners[k].size() ) vInst.resize( m_aOwners[k].size() ); }
        else if ( vVerts.size() < m_aOwners[k].size() * 8 ) vVerts.resize( m_aOwners[k].size() * 8 );  // grows only: no per-frame fill
        vStart.resize( m_aOwners[k].size() + 1 );
        size_t iOut = 0;
        Renderer::SCREEN_VERTEX aNote[12];
        const Scene::NoteKey nk = Scene::GetNoteKey( g, k );
        // Merging: the last rect out is a one-colour outer rect with positive extents, which a next
        // such rect of the same colour and columns, directly above or below it, can extend. They do
        // not overlap and nothing is drawn between them, so the pixels are the same
        bool bPrevFlat = false;
        size_t iMerged = 0;
        // Short owners skip NoteVertices: their outer rect clipped to the owned span is exactly the
        // span's vertex rows (iRowA - 0.5 to iRowB - 0.5) in the key's outer columns
        Renderer::SCREEN_VERTEX aCol[2];
        Renderer::RectVertices( nk.x, 0.0f, nk.cx, 1.0f, 0, 0, 0, 0, aCol );
        const float fX0 = aCol[0].x, fX1 = aCol[1].x;
        const bool bShortPath = !m_bPixelStats && fX1 > fX0;
        const Effects::NoteLight *pLight = m_pLight;
        std::vector< Renderer::GlowRect > &vGlow = m_aGlow[k];
        vGlow.clear();
        for ( size_t o = 0; o < m_aOwners[k].size(); o++ )
        {
            vStart[o] = (uint32_t)iOut;
            const Owner &ow = m_aOwners[k][o];
            // Effects: a note playing at the hit line is drawn lit, and glows over the rows it owns
            const ChannelSettings *pcs = &vTracks[ow.uSlot >> 4].aChannels[ow.uSlot & 15];
            ChannelSettings csLit;
            if ( pLight && ow.llStart <= pLight->llNow && ow.llEnd > pLight->llNow )
            {
                const ChannelSettings &base = ( *pLight->pBase )[ow.uSlot >> 4].aChannels[ow.uSlot & 15];
                const float fLight = Effects::Held( *pLight->pFx, pLight->llNow - ow.llStart );
                csLit = pLight->bOutlines ? pLight->pLook->LitOutline( base, *pcs, fLight, pLight->bLightUp )
                                          : pLight->pLook->Lit( base, *pcs, fLight, pLight->bLightUp );
                pcs = &csLit;
                if ( pLight->bGlow )
                {
                    // Over the owned rows: the note, or its outline (the note, then its inside in black)
                    Renderer::SCREEN_VERTEX aOuter[12];
                    Scene::NoteVertices( g, nk, ow.llStart, ow.llEnd, base, aOuter );
                    auto Push = [&]( float x, float cx, float y0, float y1, D3DCOLOR c ) {
                        if ( ow.iRowA != INT_MIN ) { y0 = std::max( y0, (float)ow.iRowA ); y1 = std::min( y1, (float)ow.iRowB ); }
                        if ( y1 > y0 && cx > 0.0f ) vGlow.push_back( { x, y0, cx, y1 - y0, c } );
                    };
                    const float y0 = aOuter[0].y + 0.5f, y1 = aOuter[2].y + 0.5f;
                    Push( nk.x, nk.cx, y0, y1, pLight->pLook->Glow( base, fLight ) );
                    if ( pLight->bOutlines ) Push( nk.x + nk.fDeflate, nk.cx - nk.fDeflate * 2.0f, y0 + nk.fDeflate, y1 - nk.fDeflate, 0 );
                }
            }
            const ChannelSettings &cs = *pcs;
            if ( bShortPath && ow.bShort && ow.iRowA != INT_MIN )
            {
                if ( ow.iRowA >= ow.iRowB ) continue;       // owns nothing
                const float y0 = ow.iRowA - 0.5f, y1 = ow.iRowB - 0.5f;
                const D3DCOLOR c = cs.iVeryDarkRGB;
                if ( bInst )
                {
                    if ( bPrevFlat )
                    {
                        Renderer::NoteInstance &q = vInst[iOut - 1];
                        if ( q.cOuter == c )
                        {
                            if ( q.y[0] == y1 ) { q.y[0] = y0; iMerged++; continue; }
                            if ( q.y[1] == y0 ) { q.y[1] = y1; iMerged++; continue; }
                        }
                    }
                    Renderer::NoteInstance &in = vInst[iOut++];
                    in.y[0] = y0;
                    in.y[1] = y1;
                    in.y[2] = in.y[3] = y0;
                    in.cOuter = in.cLeft = in.cRight = c;
                    in.fKey = (float)k;
                }
                else
                {
                    if ( bPrevFlat )
                    {
                        Renderer::SCREEN_VERTEX *q = &vVerts[iOut - 4];
                        if ( q[0].color == c && q[0].x == fX0 && q[1].x == fX1 )
                        {
                            if ( q[0].y == y1 ) { q[0].y = q[1].y = y0; iMerged++; continue; }
                            if ( q[2].y == y0 ) { q[2].y = q[3].y = y1; iMerged++; continue; }
                        }
                    }
                    Renderer::SCREEN_VERTEX *p = &vVerts[iOut];
                    p[0] = { fX0, y0, 0.5f, 1.0f, c };
                    p[1] = { fX1, y0, 0.5f, 1.0f, c };
                    p[2] = { fX1, y1, 0.5f, 1.0f, c };
                    p[3] = { fX0, y1, 0.5f, 1.0f, c };
                    iOut += 4;
                }
                bPrevFlat = abMerge[k];
                continue;
            }
            Scene::NoteVertices( g, nk, ow.llStart, ow.llEnd, cs, aNote );
            bool abDraw[2] = { false, false };
            for ( int r = 0; r < 12; r += 6 )
            {
                float dx = aNote[r + 1].x - aNote[r].x, dy = aNote[r + 2].y - aNote[r].y;
                if ( ow.iRowA != INT_MIN && dx > 0.0f && dy > 0.0f )
                {
                    // Only the owned span [iRowA, iRowB) (vertex rows are whole rows - 0.5). The outer
                    // rect is one colour, so it is cut to the span; the inner rect's gradient would
                    // round differently on a cut triangle, so it is drawn whole, or not at all when
                    // the span misses its rows.
                    float y0 = std::max( aNote[r].y, ow.iRowA - 0.5f ), y1 = std::min( aNote[r + 2].y, ow.iRowB - 0.5f );
                    if ( !( y1 > y0 ) ) continue;
                    if ( r == 0 )
                    {
                        aNote[0].y = aNote[1].y = aNote[3].y = y0;
                        aNote[2].y = aNote[4].y = aNote[5].y = y1;
                        dy = y1 - y0;
                    }
                }
                if ( ( dx > 0.0f && dy > 0.0f ) || ( dx < 0.0f && dy < 0.0f ) )
                {
                    if ( m_bPixelStats )
                    {
                        // Pixels this rect covers on screen (statistics: overdraw)
                        float x0 = std::max( 0.0f, std::min( aNote[r].x, aNote[r + 1].x ) ), x1 = std::min( (float)g.iBufferWidth, std::max( aNote[r].x, aNote[r + 1].x ) );
                        float y0 = std::max( 0.0f, std::min( aNote[r].y, aNote[r + 2].y ) ), y1 = std::min( (float)g.iBufferHeight, std::max( aNote[r].y, aNote[r + 2].y ) );
                        if ( x1 > x0 && y1 > y0 ) vPixels[i] += (double)( x1 - x0 ) * ( y1 - y0 );
                    }
                    if ( bInst ) { abDraw[r / 6] = true; continue; }
                    const bool bFlat = r == 0 && dx > 0.0f && dy > 0.0f && abMerge[k];
                    if ( bFlat && bPrevFlat )
                    {
                        Renderer::SCREEN_VERTEX *q = &vVerts[iOut - 4];
                        if ( q[0].color == aNote[0].color && q[0].x == aNote[0].x && q[1].x == aNote[1].x )
                        {
                            if ( q[0].y == aNote[2].y ) { q[0].y = q[1].y = aNote[0].y; iMerged++; continue; }
                            if ( q[2].y == aNote[0].y ) { q[2].y = q[3].y = aNote[2].y; iMerged++; continue; }
                        }
                    }
                    bPrevFlat = bFlat;
                    // As a quad: vertices 3 and 4 of the rect repeat 0 and 2 (Renderer::DrawQuads)
                    Renderer::SCREEN_VERTEX *p = &vVerts[iOut];
                    p[0] = aNote[r];
                    p[1] = aNote[r + 1];
                    p[2] = aNote[r + 2];
                    p[3] = aNote[r + 5];
                    iOut += 4;
                }
            }
            if ( bInst && ( abDraw[0] || abDraw[1] ) )
            {
                // An outer rect alone (the inner one empty) merges like a quad
                const bool bFlat = abDraw[0] && !abDraw[1] && aNote[1].x > aNote[0].x && aNote[2].y > aNote[0].y && abMerge[k];
                if ( bFlat && bPrevFlat )
                {
                    Renderer::NoteInstance &q = vInst[iOut - 1];
                    if ( q.cOuter == aNote[0].color )
                    {
                        if ( q.y[0] == aNote[2].y ) { q.y[0] = aNote[0].y; iMerged++; continue; }
                        if ( q.y[1] == aNote[0].y ) { q.y[1] = aNote[2].y; iMerged++; continue; }
                    }
                }
                bPrevFlat = bFlat;
                Renderer::NoteInstance &in = vInst[iOut++];
                in.y[0] = aNote[0].y;
                in.y[1] = abDraw[0] ? aNote[2].y : aNote[0].y;
                in.y[2] = aNote[6].y;
                in.y[3] = abDraw[1] ? aNote[8].y : aNote[6].y;
                in.cOuter = aNote[0].color;
                in.cLeft = aNote[6].color;
                in.cRight = aNote[7].color;
                in.fKey = (float)k;
            }
        }
        vStart[m_aOwners[k].size()] = (uint32_t)iOut;
        m_aVertCount[k] = iOut;
        vMerged[i] = iMerged;
        vSweepUs[i] = tK1 - tK0;
        vVertUs[i] = Timer::NowMicroSecs() - tK1;
    } );
    for ( size_t i = 0; i < vActive.size(); i++ )
    {
        m_Stats.dSweepCpuMs += vSweepUs[i] / 1000.0; m_Stats.dVertCpuMs += vVertUs[i] / 1000.0; m_Stats.dPixels += vPixels[i];
        m_Stats.iRects += bInst ? m_aVertCount[vActive[i]] : m_aVertCount[vActive[i]] / 4;
        m_Stats.iMerged += vMerged[i];
    }
    for ( size_t q : vQueries ) { m_Stats.iQueries += q & ( ( size_t( 1 ) << 40 ) - 1 ); m_Stats.iLodBuckets += q >> 40; }
    m_Stats.dQueryMs = ( Timer::NowMicroSecs() - llStart ) / 1000.0;
    m_Stats.iKeys = vActive.size();
    if ( m_pLight && m_pLight->bGlow )
        for ( int k : vActive ) m_vGlow.insert( m_vGlow.end(), m_aGlow[k].begin(), m_aGlow[k].end() );

    // Instancing: the keys' columns, as RectVertices makes them (the -0.5, then + cx)
    if ( bInst )
    {
        float aCols[128][4] = {};
        for ( int k = 0; k < 128; k++ )
        {
            const Scene::NoteKey nk = Scene::GetNoteKey( g, k );
            aCols[k][0] = nk.x - 0.5f;
            aCols[k][1] = aCols[k][0] + nk.cx;
            aCols[k][2] = ( nk.x + nk.fDeflate ) - 0.5f;
            aCols[k][3] = aCols[k][2] + ( nk.cx - nk.fDeflate * 2.0f );
        }
        pRenderer->SetNoteColumns( aCols );
    }
    // Owners [a, b) of key k (in m_aVertStart's units)
    auto Emit = [&]( int k, size_t a, size_t b ) {
        if ( bInst ) pRenderer->DrawNoteInstances( m_aInst[k].data() + a, b - a );
        else pRenderer->DrawQuads( m_aVerts[k].data() + a, ( b - a ) / 4 );
    };

    // Emit the owners in the original order: white keys, then black keys (31-EDO: then the full sharps
    // and flats), ascending index. Keys
    // whose pixel columns cannot touch are independent; keys that might share a column (sliver
    // edges, or flipped narrow-key rects) are merged by (start, OrderKey), the global index order.
    NoteView n;
    std::vector< MergeItem > &vMerge = m_vMerge;
    for ( int c = 0; c < Scene::KeyLayers; c++ )
    {
        std::vector< KeyCols > &v = aClass[c];
        std::sort( v.begin(), v.end(), []( const KeyCols &a, const KeyCols &b ) { return a.iLo < b.iLo; } );
        for ( size_t i = 0; i < v.size(); )
        {
            size_t j = i + 1;
            int iMaxHi = v[i].iHi;
            while ( j < v.size() && v[j].iLo < iMaxHi ) { iMaxHi = std::max( iMaxHi, v[j].iHi ); j++; }
            if ( j == i + 1 )
            {
                Emit( v[i].iKey, 0, m_aVertCount[v[i].iKey] );
                m_Stats.iOwners += m_aOwners[v[i].iKey].size();
            }
            else
            {
                m_Stats.bOrdered = true;
                vMerge.clear();
                for ( size_t c = i; c < j; c++ )
                    for ( size_t o = 0; o < m_aOwners[v[c].iKey].size(); o++ )
                    {
                        S.aKeys[v[c].iKey].Get( m_aOwners[v[c].iKey][o].llPos, n );
                        vMerge.push_back( { n.llStartU, OrderKey( n.uSlot, n.uRank ), v[c].iKey, o } );
                    }
                std::sort( vMerge.begin(), vMerge.end(), []( const MergeItem &a, const MergeItem &b ) {
                    return a.llU != b.llU ? a.llU < b.llU : a.llOrder < b.llOrder;
                } );
                for ( const MergeItem &it : vMerge )
                {
                    const std::vector< uint32_t > &vStart = m_aVertStart[it.iKey];
                    Emit( it.iKey, vStart[it.llIdx], vStart[it.llIdx + 1] );
                }
                m_Stats.iOwners += vMerge.size();
            }
            i = j;
        }
    }
    m_Stats.dSweepMs = ( Timer::NowMicroSecs() - llStart ) / 1000.0;
}

void FastEngine::GetKeySlots( int aSlot[256], long long *pStart ) const
{
    NoteView n;
    for ( int k = 0; k < 256; k++ )
    {
        aSlot[k] = -1;
        if ( pStart ) pStart[k] = 0;
        const KeyStore &ks = m_Store.aKeys[k];
        if ( !ks.Size() ) continue;
        Bounds bd;
        GetBounds( m_Store, k, bd );
        size_t hi = bd.iB;
        int64_t j;
        while ( ( j = ks.RightmostEnd( 0, hi, bd.llUEth1 ) ) >= 0 )
        {
            ks.Get( (size_t)j, n );
            if ( Visible( m_Store, m_aCpos, k, (size_t)j, n, bd ) )
            {
                aSlot[k] = (int)n.uSlot;
                if ( pStart ) pStart[k] = Time( n.llStartU );
                break;
            }
            hi = (size_t)j;
        }
    }
}

//-----------------------------------------------------------------------------
// The original's jump walk, for songs with re-paired notes (DESIGN.md 6.4). Without them the walk
// always finds every note sounding at the target, and the cutoff stays at 0.
//-----------------------------------------------------------------------------

namespace
{
    struct Member { uint64_t llU, llOrder; uint32_t uDup; int iKey; };

    // Adds to vOrders the OrderKeys of the NoteOns at start tick s that the same-start cull dropped in
    // favour of a kept note whose drawn end is at or after uX, on the keys flagged in abKeys. The tick's
    // NoteOns and their order come from the event stream; the culled ones (the NoteOns that are not
    // kept) are matched to their kept notes by the counts, or by the class codes (KeyStore::DupCodes)
    void AddDroppedOrders( const EventStream &es, const VisualStore &S, uint64_t s, uint64_t uX, const bool *abKeys,
                           std::vector< uint64_t > &vOrders )
    {
        std::vector< uint64_t > avOns[256];
        uint32_t uOrd = 0;
        es.Walk( s, s + 1, [&]( size_t, const StreamEvent &e ) {
            if ( ( e.cStatus >> 4 ) == 0x9 && e.cData2 && abKeys[e.cData1] ) avOns[e.cData1].push_back( OrderKey( e.uTrack * 16, uOrd ) );
            uOrd++;
        }, [&]( size_t, uint32_t, bool ) { uOrd = 0; return true; } );
        struct Class { bool bSounding; };
        std::vector< Class > vClasses;
        std::vector< uint64_t > vCulled;
        NoteView n;
        for ( int k = 0; k < 256; k++ )
        {
            if ( !abKeys[k] ) continue;
            const KeyStore &ks = S.aKeys[k];
            size_t i0 = ks.LowerBoundStart( s ), i1 = ks.LowerBoundStart( s + 1 );
            vClasses.clear();
            vCulled.clear();
            uint64_t llDups = 0;
            const std::vector< uint64_t > &vOns = avOns[k];
            size_t q = 0;
            for ( size_t p = i0; p < i1; p++ )
            {
                ks.Get( p, n );
                uint64_t o = OrderKey( n.uSlot, n.uRank );
                while ( q < vOns.size() && vOns[q] < o ) vCulled.push_back( vOns[q++] );
                if ( q < vOns.size() && vOns[q] == o ) q++;
                if ( uint32_t d = ks.DupCount( p ) ) { vClasses.push_back( { n.llEndU >= uX } ); llDups += d; }
            }
            while ( q < vOns.size() ) vCulled.push_back( vOns[q++] );
            if ( vCulled.size() < llDups ) continue;    // cannot happen: the stream holds every NoteOn
            size_t iClasses = vClasses.size() + ( vCulled.size() > llDups ? 1 : 0 );
            uint64_t llBit = 0;
            unsigned uWidth = 0;
            bool bCoded = iClasses > 1 && ks.DupCodes( s, i0, i1, llBit, uWidth );
            for ( size_t c = 0; c < vCulled.size(); c++ )
            {
                // One class: all of them belong to it (the first kept note with drops, or none)
                uint32_t uClass = bCoded ? ks.DupCode( llBit + (uint64_t)c * uWidth, uWidth ) : 0;
                if ( uClass < vClasses.size() && vClasses[uClass].bSounding ) vOrders.push_back( vCulled[c] );
            }
        }
    }
}

void FastEngine::ComputeJumpCutoff()
{
    const VisualStore &S = m_Store;
    NoteView n;

    // prev: the last paired NoteOn before the target, which is the last paired NoteOn of its tick;
    // K: its Simultaneous + 1 (paired ons before it minus pairing offs before it, PostProcess)
    uint64_t uJ = LowerBoundTime( m_llJump );
    size_t t = (size_t)( std::lower_bound( S.vOnTicks.begin(), S.vOnTicks.end(), uJ ) - S.vOnTicks.begin() );
    if ( !t ) { m_iCut = 1; return; }
    uint64_t uP = S.vOnTicks[t - 1];
    long long K = S.vJumpK[t - 1];
    if ( K <= 0 ) { m_iCut = 1; return; }

    // The walk counts notes from prev backwards whose drawn end is after prev's time, and stops at
    // the K-th: the jump set is the notes from there on whose drawn end is after the target. A kept
    // note counts for itself and for the notes the same-start cull dropped in its favour, which have
    // its start and end and come before it
    uint64_t uX = UpperBoundTime( Time( uP ) );
    std::vector< Member > v;
    long long llTotal = 0;
    for ( int k = 0; k < 256; k++ )
    {
        const KeyStore &ks = S.aKeys[k];
        size_t hi = ks.LowerBoundStart( uP + 1 );
        int64_t j;
        while ( ( j = ks.RightmostEnd( 0, hi, uX ) ) >= 0 )
        {
            ks.Get( (size_t)j, n );
            if ( !n.bUnpaired )
            {
                uint32_t uDup = ks.DupCount( (size_t)j );
                v.push_back( { n.llStartU, OrderKey( n.uSlot, n.uRank ), uDup, k } );
                llTotal += 1 + (long long)uDup;
            }
            hi = (size_t)j;
        }
    }
    if ( llTotal < K ) return;  // the walk reaches the first note: cutoff 0
    auto Later = []( const Member &a, const Member &b ) { return a.llU != b.llU ? a.llU > b.llU : a.llOrder > b.llOrder; };
    m_iCut = 2;
    if ( llTotal == (long long)v.size() )
    {
        std::nth_element( v.begin(), v.begin() + ( K - 1 ), v.end(), Later );
        m_llCutU = v[K - 1].llU;
        m_llCutOrder = v[K - 1].llOrder;
        return;
    }

    // Dropped notes: the tick of the K-th latest from the counts, its order there from the stream
    std::sort( v.begin(), v.end(), Later );
    size_t i = 0, e = 0;
    long long llAbove = 0;
    for ( ;; )
    {
        long long w = 0;
        for ( e = i; e < v.size() && v[e].llU == v[i].llU; e++ ) w += 1 + (long long)v[e].uDup;
        if ( llAbove + w >= K ) break;
        llAbove += w;
        i = e;
    }
    std::vector< uint64_t > vOrders;
    bool abKeys[256] = {};
    bool bDrops = false;
    for ( size_t q = i; q < e; q++ )
    {
        vOrders.push_back( v[q].llOrder );
        if ( v[q].uDup ) abKeys[v[q].iKey] = bDrops = true;
    }
    if ( bDrops ) AddDroppedOrders( m_Song.stream, S, v[i].llU, uX, abKeys, vOrders );
    size_t r = (size_t)( K - llAbove );
    if ( r > vOrders.size() ) r = vOrders.size();   // cannot happen: the counts match the stream
    std::nth_element( vOrders.begin(), vOrders.begin() + ( r - 1 ), vOrders.end(), std::greater< uint64_t >() );
    m_llCutU = v[i].llU;
    m_llCutOrder = vOrders[r - 1];
}
