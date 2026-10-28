#include "audio/PlaybackEngine.h"

#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/ThreadPool.h"
#include "core/Util.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
    HANDLE CreateWaitTimer()
    {
        HANDLE h = CreateWaitableTimerExW( nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS );
        if ( !h ) h = CreateWaitableTimerExW( nullptr, nullptr, 0, TIMER_ALL_ACCESS );
        return h;
    }

    // Sleeps about llUs microseconds: spins for short waits, uses a high-resolution timer otherwise
    void WaitUs( HANDLE hTimer, long long llUs )
    {
        if ( llUs <= 0 ) return;
        if ( llUs < 250 || !hTimer )
        {
            if ( llUs >= 250 ) { Sleep( 1 ); return; }
            long long llEnd = Timer::NowMicroSecs() + llUs;
            while ( Timer::NowMicroSecs() < llEnd ) YieldProcessor();
            return;
        }
        LARGE_INTEGER li;
        li.QuadPart = -llUs * 10;
        if ( SetWaitableTimer( hTimer, &li, 0, nullptr, nullptr, FALSE ) ) WaitForSingleObject( hTimer, INFINITE );
        else Sleep( 1 );
    }
}

PlaybackEngine::PlaybackEngine( std::shared_ptr< const Song > pSong, PlaybackClock &clock, int iThreads )
    : m_pSong( std::move( pSong ) ), m_Clock( clock ), m_Decoder( *m_pSong )
{
    SetMaxThreads( iThreads );
    size_t iTracks = std::max< size_t >( 1, m_pSong->vTracks.size() );
    m_pMuteMask.reset( new std::atomic< uint16_t >[iTracks] );
    for ( size_t i = 0; i < iTracks; i++ ) m_pMuteMask[i].store( 0, std::memory_order_relaxed );
    m_Decoder.SetMuteMask( m_pMuteMask.get() );
    m_pRing.reset( new Slice[RingSize] );
}

PlaybackEngine::~PlaybackEngine()
{
    Stop();
}

void PlaybackEngine::Start( long long llStartTime )
{
    if ( m_DecoderThread.joinable() ) return;
    {
        std::lock_guard< std::mutex > lock( m_mxSeek );
        m_bSeekReset = true;
        m_llSeekTarget = m_llSeekPrev = llStartTime;
        m_Clock.Set( llStartTime );
    }
    m_bStop = false;
    m_bReopen = true;
    m_SenderThread = std::thread( &PlaybackEngine::SenderThread, this );
    m_DecoderThread = std::thread( &PlaybackEngine::DecoderThread, this );
}

void PlaybackEngine::Stop()
{
    m_bStop = true;
    if ( m_DecoderThread.joinable() ) m_DecoderThread.join();
    if ( m_SenderThread.joinable() ) m_SenderThread.join();
    {
        std::lock_guard< std::mutex > lock( m_mxLanes );
        m_bLanesStop = true;
    }
    m_cvLanes.notify_all();
    for ( std::thread &t : m_vLaneThreads ) if ( t.joinable() ) t.join();
    m_vLaneThreads.clear();
    m_vLaneDec.clear();
    m_vLaneOut.clear();
    m_bLanesStop = false;
}

void PlaybackEngine::Seek( long long llTarget, long long llClamped, long long llPrevTime )
{
    std::lock_guard< std::mutex > lock( m_mxSeek );
    m_bSeekReset = false;
    m_llSeekTarget = llTarget;
    m_llSeekPrev = llPrevTime;
    m_Clock.Set( llClamped );
}

void PlaybackEngine::AllNotesOff()
{
    m_bNotesOff.store( true, std::memory_order_release );
}

void PlaybackEngine::ReopenOutput()
{
    m_bReopen.store( true, std::memory_order_release );
}

void PlaybackEngine::SetChannelMuted( int iTrack, int iChannel, bool bMuted )
{
    if ( iTrack < 0 || (size_t)iTrack >= m_pSong->vTracks.size() || iChannel < 0 || iChannel > 15 ) return;
    uint16_t uBit = (uint16_t)( 1u << iChannel );
    if ( bMuted ) m_pMuteMask[iTrack].fetch_or( uBit, std::memory_order_relaxed );
    else m_pMuteMask[iTrack].fetch_and( (uint16_t)~uBit, std::memory_order_relaxed );
}

void PlaybackEngine::SetMaxThreads( int iThreads )
{
    if ( iThreads == m_iThreadsSetting ) return;
    m_iThreadsSetting = iThreads;
    m_iMaxLanes.store( std::max( 1, Parallel::ThreadCount( iThreads ) ), std::memory_order_relaxed );
}

void PlaybackEngine::SetSkipping( bool bVelocitySkip, int iSkipLagMs )
{
    m_bVelocitySkip.store( bVelocitySkip, std::memory_order_relaxed );
    m_iSkipLagMs.store( iSkipLagMs, std::memory_order_relaxed );
}

void PlaybackEngine::SetSendLimits( bool bCollapse, long long llMaxSendsPerSec )
{
    m_bCollapse.store( bCollapse, std::memory_order_relaxed );
    m_llMaxSendsPerSec.store( std::max< long long >( 0, llMaxSendsPerSec ), std::memory_order_relaxed );
}

std::wstring PlaybackEngine::OutputName() const
{
    std::lock_guard< std::mutex > lock( m_mxName );
    return m_sOutputName;
}

PlaybackEngine::Stats PlaybackEngine::GetStats()
{
    Stats s;
    s.llSent = m_llSent.load( std::memory_order_relaxed );
    s.llDecoded = m_llDecoded.load( std::memory_order_relaxed );
    s.llDecodeBusyUs = m_llDecodeBusyUs.load( std::memory_order_relaxed );
    s.llSendBusyUs = m_llSendBusyUs.load( std::memory_order_relaxed );
    s.llRingSum = m_llRingSum.load( std::memory_order_relaxed );
    s.llRingSamples = m_llRingSamples.load( std::memory_order_relaxed );
    s.iRingMax = m_iRingMax.exchange( 0, std::memory_order_relaxed );
    s.llNoteOns = m_llNoteOns.load( std::memory_order_relaxed );
    s.llSkipped = m_llSkipped.load( std::memory_order_relaxed );
    s.llLagUs = m_llLagUs.load( std::memory_order_relaxed );
    s.dNsPerMsg = m_dNsPerMsg.load( std::memory_order_relaxed );
    s.iSkipVelocity = m_iSkipVelocity.load( std::memory_order_relaxed );
    s.llCollapsed = m_llCollapsed.load( std::memory_order_relaxed );
    s.budget.iVelocity = m_iBudgetVel.load( std::memory_order_relaxed );
    s.budget.dKeep = m_dBudgetKeep.load( std::memory_order_relaxed );
    s.iLanes = m_iLanesPeak.exchange( 1, std::memory_order_relaxed );
    return s;
}

PlaybackEngine::Slice *PlaybackEngine::WriteSlot()
{
    int iWrite = m_iWrite.load( std::memory_order_relaxed );
    if ( ( iWrite + 1 ) % RingSize == m_iRead.load( std::memory_order_acquire ) ) return nullptr;
    return &m_pRing[iWrite];
}

void PlaybackEngine::Commit()
{
    m_iWrite.store( ( m_iWrite.load( std::memory_order_relaxed ) + 1 ) % RingSize, std::memory_order_release );
}

//-----------------------------------------------------------------------------
// Decoder thread
//-----------------------------------------------------------------------------

void PlaybackEngine::DecoderThread()
{
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_HIGHEST );
    HANDLE hTimer = CreateWaitTimer();
    unsigned uEpoch = m_Clock.Epoch() - 1;
    std::vector< uint32_t > vChase;
    PlaybackDecoder::Params p;

    while ( !m_bStop.load( std::memory_order_relaxed ) )
    {
        // A seek (or the start) bumped the epoch
        unsigned e;
        bool bReset;
        long long llTarget, llPrev;
        {
            std::lock_guard< std::mutex > lock( m_mxSeek );
            e = m_Clock.Epoch();
            bReset = m_bSeekReset;
            llTarget = m_llSeekTarget;
            llPrev = m_llSeekPrev;
        }
        if ( e != uEpoch )
        {
            uEpoch = e;
            if ( bReset ) m_Decoder.Reset( llTarget );
            else
            {
                vChase.clear();
                long long t0 = Timer::NowMicroSecs();
                m_Decoder.Seek( llTarget, llPrev, vChase );
                m_llDecodeBusyUs.fetch_add( Timer::NowMicroSecs() - t0, std::memory_order_relaxed );
                for ( size_t i = 0; i < vChase.size() && !m_bStop; )
                {
                    Slice *s = WriteSlot();
                    if ( !s ) { WaitUs( hTimer, 300 ); continue; }
                    size_t n = std::min( vChase.size() - i, SliceMsgs );
                    std::copy( vChase.begin() + i, vChase.begin() + i + n, s->aMsgs );
                    s->uEpoch = uEpoch;
                    s->llDue = LLONG_MIN;
                    s->bImmediate = true;
                    s->n = (uint32_t)n;
                    Commit();
                    i += n;
                }
            }
            continue;
        }

        Slice *s = WriteSlot();
        if ( !s ) { WaitUs( hTimer, 500 ); continue; }

        p.dSpeed = std::max( m_Clock.Speed(), 1e-6 );
        p.bMute = m_bMute.load( std::memory_order_relaxed );
        p.dVolumeCorrect = m_dVolumeCorrect.load( std::memory_order_relaxed );
        p.iMetronome = m_iMetronome.load( std::memory_order_relaxed );
        p.bVelocitySkip = m_bVelocitySkip.load( std::memory_order_relaxed );
        p.llSkipLagUs = (long long)( m_iSkipLagMs.load( std::memory_order_relaxed ) * 1000.0 * p.dSpeed );
        p.llSenderLagUs = std::max< long long >( 0, m_llLagUs.load( std::memory_order_relaxed ) );
        p.bCollapse = m_bCollapse.load( std::memory_order_relaxed );
        p.llMaxSendsPerSec = m_llMaxSendsPerSec.load( std::memory_order_relaxed );
        p.iMinVelocity = m_iMinVelocity.load( std::memory_order_relaxed );

        long long llNow = m_Clock.Now();

        // Parallel when one thread cannot keep up
        int iLanes = m_iMaxLanes.load( std::memory_order_relaxed ) > 1 ? LanesWanted( p, llNow ) : 1;
        m_iLanesNow.store( iLanes, std::memory_order_relaxed );
        if ( iLanes > m_iLanesPeak.load( std::memory_order_relaxed ) ) m_iLanesPeak.store( iLanes, std::memory_order_relaxed );
        if ( iLanes > 1 && RunBatch( iLanes, p, llNow, uEpoch, hTimer ) ) continue;

        long long llHorizon = llNow + std::max< long long >( 1000, (long long)( 4000 * p.dSpeed ) );
        long long llDue, t0 = Timer::NowMicroSecs();
        uint64_t llRead0 = m_Decoder.EventsRead();
        size_t n = m_Decoder.Fill( s->aMsgs, SliceMsgs, llHorizon, llNow, p, llDue );
        long long llBusy = Timer::NowMicroSecs() - t0;
        m_llDecodeBusyUs.fetch_add( llBusy, std::memory_order_relaxed );
        // One thread's speed, from calls that read enough to time
        uint64_t llRead = m_Decoder.EventsRead() - llRead0;
        if ( llRead >= 32768 && llBusy > 0 ) m_dRate = std::max( 10.0, m_dRate * 0.8 + 0.2 * llRead / llBusy );

        const PlaybackDecoder::Counters &c = m_Decoder.GetCounters();
        m_llDecoded.store( c.llSent, std::memory_order_relaxed );
        m_llNoteOns.store( c.llNoteOns, std::memory_order_relaxed );
        m_llSkipped.store( c.llSkipped, std::memory_order_relaxed );
        m_iSkipVelocity.store( m_Decoder.SkipVelocity(), std::memory_order_relaxed );
        m_llCollapsed.store( c.llCollapsed, std::memory_order_relaxed );
        m_iBudgetVel.store( m_Decoder.GetBudgetCut().iVelocity, std::memory_order_relaxed );
        m_dBudgetKeep.store( m_Decoder.GetBudgetCut().dKeep, std::memory_order_relaxed );

        if ( n )
        {
            s->uEpoch = uEpoch;
            s->llDue = llDue;
            s->bImmediate = false;
            s->n = (uint32_t)n;
            Commit();
            continue;
        }
        if ( llDue <= llHorizon ) continue;     // progress without messages (a silent beat)

        // Nothing due yet
        long long llWait = 1000;
        if ( llDue != LLONG_MAX && !m_Clock.Paused() )
            llWait = std::min< long long >( 1000, (long long)( ( llDue - llHorizon ) / p.dSpeed ) + 1 );
        WaitUs( hTimer, std::max< long long >( llWait, 300 ) );
    }
    if ( hTimer ) CloseHandle( hTimer );
}

//-----------------------------------------------------------------------------
// Parallel decoding (DESIGN.md 7.11)
//-----------------------------------------------------------------------------

// Lanes for the next 20 ms of real time: its note-ons (the song's bins, with their offs) against one
// thread's measured speed, each lane at most about half busy; all of them when decoding is 10 ms late.
// Once parallel, it stays so until the load falls to about a third of one thread.
int PlaybackEngine::LanesWanted( const PlaybackDecoder::Params &p, long long llNow )
{
    long long tNext = m_Decoder.NextTime( PlaybackDecoder::MetOff );
    if ( tNext == LLONG_MAX ) return 1;
    const VelocityBins &vb = m_pSong->aSendOns[0];
    const double dWindowUs = 20000.0;
    long long llWin = (long long)( dWindowUs * p.dSpeed );
    uint64_t llOns = 0;
    for ( size_t b = (size_t)( std::max( 0LL, tNext ) / VelocityBins::BinUs ); (long long)b * VelocityBins::BinUs <= tNext + llWin && b < vb.Bins(); b++ )
        llOns += vb.Total( b );
    double dShare = llOns * 2.2 / m_dRate / dWindowUs;      // of one thread's time
    double dLateUs = ( llNow - tNext ) / p.dSpeed;
    bool bParallel = m_iLanesNow.load( std::memory_order_relaxed ) > 1;
    int iMax = m_iMaxLanes.load( std::memory_order_relaxed );
    if ( !( bParallel ? dShare > 0.3 || dLateUs > 2000 : dShare > 0.6 || dLateUs > 10000 ) ) return 1;
    if ( iMax < 2 ) return 1;
    int n = dLateUs > 10000 ? iMax : (int)std::ceil( dShare / 0.5 );
    return std::clamp( n, 2, iMax );
}

void PlaybackEngine::EnsureLanes( int iLanes )
{
    // The helpers only touch these vectors during a job, and they only grow between jobs (here)
    if ( m_vLaneDec.empty() )
    {
        m_vLaneDec.emplace_back();      // lane 0 is the decoder itself
        m_vLaneOut.emplace_back();
    }
    while ( (int)m_vLaneDec.size() < iLanes )
    {
        int k = (int)m_vLaneDec.size();
        m_vLaneDec.emplace_back( new PlaybackDecoder( *m_pSong ) );
        m_vLaneDec.back()->SetMuteMask( m_pMuteMask.get() );
        m_vLaneOut.emplace_back();
        // Its first job is the next one (jobs are only posted on this thread, after this)
        unsigned uJob;
        {
            std::lock_guard< std::mutex > lock( m_mxLanes );
            uJob = m_uLaneJob;
        }
        m_vLaneThreads.emplace_back( &PlaybackEngine::LaneThread, this, k, uJob );
    }
}

void PlaybackEngine::LaneThread( int k, unsigned uSeen )
{
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_HIGHEST );
    Parallel::SpreadThread( k );
    std::unique_lock< std::mutex > lock( m_mxLanes );
    for ( ;; )
    {
        m_cvLanes.wait( lock, [&] { return m_bLanesStop || m_uLaneJob != uSeen; } );
        if ( m_bLanesStop ) return;
        uSeen = m_uLaneJob;
        if ( k >= m_iJobLanes ) continue;
        const std::function< void( int ) > *pFn = m_pJobFn;
        lock.unlock();
        ( *pFn )( k );
        lock.lock();
        if ( --m_iLanesLeft == 0 ) m_cvLanesDone.notify_one();
    }
}

void PlaybackEngine::RunOnLanes( int iLanes, const std::function< void( int ) > &fn )
{
    {
        std::lock_guard< std::mutex > lock( m_mxLanes );
        m_pJobFn = &fn;
        m_iJobLanes = iLanes;
        m_iLanesLeft = iLanes - 1;
        m_uLaneJob++;
    }
    m_cvLanes.notify_all();
    fn( 0 );
    std::unique_lock< std::mutex > lock( m_mxLanes );
    m_cvLanesDone.wait( lock, [&] { return m_iLanesLeft == 0; } );
}

// A batch up to 16 ms (x speed) ahead of the clock, about 2 MB of the stream per lane: lane 0 here,
// the others on the helper threads, then stitched into slices (grouped as Fill groups them)
bool PlaybackEngine::RunBatch( int iLanes, const PlaybackDecoder::Params &p, long long llNow, unsigned uEpoch, HANDLE hTimer )
{
    PlaybackDecoder::Batch b;
    // Only once the next event is within one thread's horizon (4 ms), and then 16 ms ahead: about 12 ms
    // of song per batch, so the lanes' work outweighs waking them (a batch on every wait would hold
    // well under a millisecond)
    if ( m_Decoder.NextTime( PlaybackDecoder::MetOff ) > llNow + std::max< long long >( 1000, (long long)( 4000 * p.dSpeed ) ) ) return false;
    long long llHorizon = llNow + std::max< long long >( 4000, (long long)( 16000 * p.dSpeed ) );
    if ( !m_Decoder.PlanBatch( iLanes, llHorizon, (uint64_t)iLanes << 21, llNow, b ) ) return false;
    EnsureLanes( iLanes );
    long long t0 = Timer::NowMicroSecs();
    RunOnLanes( iLanes, [&]( int k ) {
        if ( k == 0 ) m_Decoder.RunMainLane( b, p, m_vLaneOut[0] );
        else m_vLaneDec[k]->RunLane( b, k, p, m_vLaneOut[k] );
    } );

    std::vector< PlaybackDecoder * > vLanes( iLanes );
    for ( int k = 1; k < iLanes; k++ ) vLanes[k] = m_vLaneDec[k].get();
    long long llGroupUs = std::max< long long >( 1, (long long)( 500 * p.dSpeed ) );
    Slice *pCur = nullptr;
    bool bAbort = false;
    auto Parallel = [&]( const std::function< void( int ) > &fn ) { RunOnLanes( iLanes, [&]( int k ) { if ( k ) fn( k ); } ); };
    bool bTimed = false;
    m_Decoder.FinishBatch( b, vLanes.data(), m_vLaneOut.data(), iLanes, Parallel, [&]( const uint32_t *pm, size_t n, long long llDue ) {
        // The decoding is done once the messages come (only waiting for the ring after)
        if ( !bTimed ) { bTimed = true; m_llDecodeBusyUs.fetch_add( Timer::NowMicroSecs() - t0, std::memory_order_relaxed ); }
        while ( n && !bAbort )
        {
            if ( pCur && ( pCur->n + n > SliceMsgs || llDue - pCur->llDue > llGroupUs ) ) { Commit(); pCur = nullptr; }
            if ( !pCur )
            {
                while ( !( pCur = WriteSlot() ) )
                {
                    // A seek or stop while the ring is full: the rest is not needed
                    if ( m_bStop.load( std::memory_order_relaxed ) || m_Clock.Epoch() != uEpoch ) { bAbort = true; return; }
                    WaitUs( hTimer, 300 );
                }
                pCur->uEpoch = uEpoch;
                pCur->llDue = llDue;
                pCur->bImmediate = false;
                pCur->n = 0;
            }
            size_t k = std::min( n, SliceMsgs - pCur->n );
            memcpy( pCur->aMsgs + pCur->n, pm, k * sizeof( uint32_t ) );
            pCur->n += (uint32_t)k;
            pm += k;
            n -= k;
        }
    } );
    if ( pCur && !bAbort ) Commit();
    if ( !bTimed ) m_llDecodeBusyUs.fetch_add( Timer::NowMicroSecs() - t0, std::memory_order_relaxed );

    const PlaybackDecoder::Counters &c = m_Decoder.GetCounters();
    m_llDecoded.store( c.llSent, std::memory_order_relaxed );
    m_llNoteOns.store( c.llNoteOns, std::memory_order_relaxed );
    m_llSkipped.store( c.llSkipped, std::memory_order_relaxed );
    m_iSkipVelocity.store( m_Decoder.SkipVelocity(), std::memory_order_relaxed );
    m_llCollapsed.store( c.llCollapsed, std::memory_order_relaxed );
    m_iBudgetVel.store( m_Decoder.GetBudgetCut().iVelocity, std::memory_order_relaxed );
    m_dBudgetKeep.store( m_Decoder.GetBudgetCut().dKeep, std::memory_order_relaxed );
    return true;
}

//-----------------------------------------------------------------------------
// Sender thread
//-----------------------------------------------------------------------------

void PlaybackEngine::SenderThread()
{
    DWORD dwTask = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW( L"Pro Audio", &dwTask );
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
    HANDLE hTimer = CreateWaitTimer();
    double dEma = 0.0;
    unsigned uSentEpoch = m_Clock.Epoch();

    while ( !m_bStop.load( std::memory_order_relaxed ) )
    {
        if ( m_bReopen.exchange( false, std::memory_order_acq_rel ) )
        {
            if ( m_pOut ) m_pOut->AllNotesOff();
            m_pOut.reset();
            m_pOut = MidiOutputs::OpenConfigured( m_pSong->bTuning31, m_pSong->bTuning31PitchBend );
            if ( m_pOut ) m_pOut->SetVolume( 1.0 );
            {
                std::lock_guard< std::mutex > lock( m_mxName );
                m_sOutputName = m_pOut ? m_pOut->Name() : std::wstring();
            }
            m_bHasOutput.store( m_pOut != nullptr, std::memory_order_release );
            dEma = 0.0;
            m_dNsPerMsg.store( 0.0, std::memory_order_relaxed );
        }
        if ( m_bNotesOff.exchange( false, std::memory_order_acq_rel ) && m_pOut )
            m_pOut->AllNotesOff();

        int r = m_iRead.load( std::memory_order_relaxed );
        if ( r == m_iWrite.load( std::memory_order_acquire ) ) { WaitUs( hTimer, 500 ); continue; }
        Slice &s = m_pRing[r];

        if ( s.uEpoch == m_Clock.Epoch() )
        {
            if ( s.uEpoch != uSentEpoch )
            {
                if ( m_pOut ) m_pOut->ResetPlaybackState();
                uSentEpoch = s.uEpoch;
            }
            if ( !s.bImmediate )
            {
                long long llNow = m_Clock.Now();
                if ( s.llDue > llNow )
                {
                    if ( m_Clock.Paused() ) { WaitUs( hTimer, 1000 ); continue; }
                    long long llWall = (long long)( ( s.llDue - llNow ) / std::max( m_Clock.Speed(), 1e-6 ) );
                    WaitUs( hTimer, llWall > 1000 ? 1000 : llWall > 300 ? llWall - 200 : llWall );
                    continue;
                }
                m_llLagUs.store( llNow - s.llDue, std::memory_order_relaxed );
            }
            if ( m_pOut && s.n )
            {
                long long t0 = Timer::NowMicroSecs();
                m_pOut->SendBatch( s.aMsgs, s.n );
                long long t1 = Timer::NowMicroSecs();
                m_llSendBusyUs.fetch_add( t1 - t0, std::memory_order_relaxed );
                m_llSent.fetch_add( s.n, std::memory_order_relaxed );
                double dNs = ( t1 - t0 ) * 1000.0 / s.n;
                dEma = dEma > 0.0 ? dEma * 0.9 + dNs * 0.1 : dNs;
                m_dNsPerMsg.store( dEma, std::memory_order_relaxed );
            }
        }
        // The ring's fill as each slice is taken: full means the output is what holds playback back,
        // empty (with the decoder busy) the decoder
        int iFill = ( m_iWrite.load( std::memory_order_relaxed ) - r + RingSize ) % RingSize;
        m_llRingSum.fetch_add( iFill, std::memory_order_relaxed );
        m_llRingSamples.fetch_add( 1, std::memory_order_relaxed );
        if ( iFill > m_iRingMax.load( std::memory_order_relaxed ) ) m_iRingMax.store( iFill, std::memory_order_relaxed );
        m_iRead.store( ( r + 1 ) % RingSize, std::memory_order_release );
    }

    if ( m_pOut ) m_pOut->AllNotesOff();
    m_pOut.reset();
    m_bHasOutput.store( false, std::memory_order_release );
    if ( hTimer ) CloseHandle( hTimer );
    if ( hTask ) AvRevertMmThreadCharacteristics( hTask );
}
