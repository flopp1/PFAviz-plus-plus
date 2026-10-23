#include "audio/PlaybackEngine.h"

#include <avrt.h>
#include <algorithm>

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

PlaybackEngine::PlaybackEngine( std::shared_ptr< const Song > pSong, PlaybackClock &clock )
    : m_pSong( std::move( pSong ) ), m_Clock( clock ), m_Decoder( *m_pSong )
{
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

        long long llHorizon = llNow + std::max< long long >( 1000, (long long)( 4000 * p.dSpeed ) );
        long long llDue, t0 = Timer::NowMicroSecs();
        size_t n = m_Decoder.Fill( s->aMsgs, SliceMsgs, llHorizon, llNow, p, llDue );
        m_llDecodeBusyUs.fetch_add( Timer::NowMicroSecs() - t0, std::memory_order_relaxed );

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
// Sender thread
//-----------------------------------------------------------------------------

void PlaybackEngine::SenderThread()
{
    DWORD dwTask = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW( L"Pro Audio", &dwTask );
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
    HANDLE hTimer = CreateWaitTimer();
    double dEma = 0.0;

    while ( !m_bStop.load( std::memory_order_relaxed ) )
    {
        if ( m_bReopen.exchange( false, std::memory_order_acq_rel ) )
        {
            if ( m_pOut ) m_pOut->AllNotesOff();
            m_pOut.reset();
            m_pOut = MidiOutputs::OpenConfigured();
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
