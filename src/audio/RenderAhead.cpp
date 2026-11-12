#include "audio/RenderAhead.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <climits>
#include <deque>
#include <unordered_map>

#include "audio/PitchBendTuning.h"
#include "audio/ScaleTuning.h"
#include "core/Util.h"

namespace
{
    // Developer switch PFAVIZ_RENDERLOG=file (see the worker)
    FILE *RenderLog()
    {
        static FILE *s_p = getenv( "PFAVIZ_RENDERLOG" ) ? fopen( getenv( "PFAVIZ_RENDERLOG" ), "w" ) : nullptr;
        return s_p;
    }

    long long FloorDiv( long long a, long long b ) { long long q = a / b; return ( a % b != 0 && ( ( a < 0 ) != ( b < 0 ) ) ) ? q - 1 : q; }

    // What a 31-EDO output (ScaleTunedOutput, PitchBendTunedOutput) makes of a piece's messages: gathered
    // here, to be written into the piece
    class PieceOut : public MidiOutput
    {
    public:
        std::vector< uint32_t > vMsgs;
        std::vector< std::vector< uint8_t > > vSysEx;
        void Send( uint32_t m ) override { vMsgs.push_back( m ); }
        void SendBatch( const uint32_t *p, size_t n ) override { vMsgs.insert( vMsgs.end(), p, p + n ); }
        bool SendSysEx( const uint8_t *p, size_t n ) override { vSysEx.emplace_back( p, p + n ); return true; }
        const std::wstring &Name() const override { static const std::wstring s = L"piece"; return s; }
    };

    // The piece's MIDI file: one tick is 1/24000 of a 120 BPM beat, 20.83 us (a frame at 48 kHz)
    const int PiecePpq = 24000;
    const int PieceTempo = 500000;
    const double TickUs = (double)PieceTempo / PiecePpq;
    const size_t PieceBytesMax = 192u << 20;   // a denser piece is cut at a block's end

    void Vlq( std::vector< uint8_t > &v, uint64_t n )
    {
        uint8_t a[10];
        int k = 0;
        a[k++] = (uint8_t)( n & 0x7F );
        while ( n >>= 7 ) a[k++] = (uint8_t)( 0x80 | ( n & 0x7F ) );
        while ( k ) v.push_back( a[--k] );
    }

    struct SmfWriter
    {
        std::vector< uint8_t > &v;
        uint64_t llTick = 0;
        uint8_t uStatus = 0;
        explicit SmfWriter( std::vector< uint8_t > &out ) : v( out )
        {
            v.clear();
            const uint8_t aHead[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, (uint8_t)( PiecePpq >> 8 ), (uint8_t)PiecePpq,
                                      'M', 'T', 'r', 'k', 0, 0, 0, 0,
                                      0, 0xFF, 0x51, 3, (uint8_t)( PieceTempo >> 16 ), (uint8_t)( PieceTempo >> 8 ), (uint8_t)PieceTempo };
            v.insert( v.end(), aHead, aHead + sizeof( aHead ) );
        }
        void Msg( uint64_t llAt, uint32_t m )
        {
            Vlq( v, llAt > llTick ? llAt - llTick : 0 );
            if ( llAt > llTick ) llTick = llAt;
            uint8_t s = (uint8_t)m;
            if ( s != uStatus ) { v.push_back( s ); uStatus = s; }     // (running status)
            v.push_back( (uint8_t)( ( m >> 8 ) & 0x7F ) );
            if ( ( s & 0xE0 ) != 0xC0 ) v.push_back( (uint8_t)( ( m >> 16 ) & 0x7F ) );
        }
        // Spiderweb's repeat mark: the next note event (on key) stands for n identical ones
        void Repeat( uint64_t llAt, uint32_t uKey, uint32_t n )
        {
            Vlq( v, llAt > llTick ? llAt - llTick : 0 );
            if ( llAt > llTick ) llTick = llAt;
            const uint8_t a[] = { 0xFF, 0x7F, 8, 'S', 'W', 'n', (uint8_t)uKey, (uint8_t)( n >> 24 ), (uint8_t)( n >> 16 ), (uint8_t)( n >> 8 ), (uint8_t)n };
            v.insert( v.end(), a, a + sizeof( a ) );
            uStatus = 0;    // (a meta event ends running status)
        }
        // A SysEx: its bytes from the F0 to the F7
        void SysEx( uint64_t llAt, const uint8_t *p, size_t n )
        {
            if ( n < 2 || p[0] != 0xF0 ) return;
            Vlq( v, llAt > llTick ? llAt - llTick : 0 );
            if ( llAt > llTick ) llTick = llAt;
            v.push_back( 0xF0 );
            Vlq( v, n - 1 );
            v.insert( v.end(), p + 1, p + n );
            uStatus = 0;
        }
        void End( uint64_t llAt )
        {
            Vlq( v, llAt > llTick ? llAt - llTick : 0 );
            const uint8_t aEnd[] = { 0xFF, 0x2F, 0 };
            v.insert( v.end(), aEnd, aEnd + 3 );
            uint32_t uLen = (uint32_t)( v.size() - 22 );
            v[18] = (uint8_t)( uLen >> 24 ); v[19] = (uint8_t)( uLen >> 16 ); v[20] = (uint8_t)( uLen >> 8 ); v[21] = (uint8_t)uLen;
        }
    };
}

RenderAhead::RenderAhead( std::shared_ptr< const Song > pSong, PlaybackClock &clock, const std::atomic< uint16_t > *pMuteMask )
    : m_pSong( std::move( pSong ) ), m_Clock( clock ), m_pMuteMask( pMuteMask )
{
    m_llEndUs = m_pSong->info.llTotalMicroSecs + 3000000;
    for ( const auto &t : m_pSong->vTracks )
        for ( int c = 0; c < 16; c++ ) if ( t.aNoteCount[c] > 0 ) m_uChannels |= (uint16_t)( 1u << c );
}

RenderAhead::RenderAhead( std::shared_ptr< const Song > pSong, const std::atomic< uint16_t > *pMuteMask, unsigned uRate )
    : m_pSong( std::move( pSong ) ), m_uOfflineRate( uRate ), m_Clock( m_OwnClock ), m_pMuteMask( pMuteMask )
{
    m_llEndUs = m_pSong->info.llTotalMicroSecs + 3000000;
    for ( const auto &t : m_pSong->vTracks )
        for ( int c = 0; c < 16; c++ ) if ( t.aNoteCount[c] > 0 ) m_uChannels |= (uint16_t)( 1u << c );
    m_OwnClock.SetSpeed( 1.0 );
    m_OwnClock.SetPaused( true );       // (so nothing is made lighter: nothing is ever late)
}

RenderAhead::~RenderAhead()
{
    Stop();
}

void RenderAhead::Start( const Settings &s )
{
    if ( m_Loader.joinable() ) return;
    m_Settings = s;
    m_bLimiter.store( s.bLimiter, std::memory_order_relaxed );
    m_bStop = false;
    std::wstring sError;
    if ( !m_uOfflineRate && !m_Out.Start( [this]( float *p, unsigned n, long long llWall, unsigned uRate ) { Fill( p, n, llWall, uRate ); }, sError ) )
        m_sState = L"no sound: " + sError;
    {
        std::lock_guard< std::mutex > lock( m_mx );
        NewTimebase( std::max( m_Clock.Speed(), 1e-6 ), m_uOfflineRate ? m_uOfflineRate : m_Out.Rate() ? m_Out.Rate() : 48000 );
        m_uBankGen++;
    }
    m_Loader = std::thread( &RenderAhead::LoaderThread, this );
    m_Pager = std::thread( &RenderAhead::PagerThread, this );
    m_iWorkers = std::clamp( (int)std::thread::hardware_concurrency() - 2, 1, 8 );
    for ( int k = 0; k < m_iWorkers; k++ ) m_vWorkers.emplace_back( &RenderAhead::WorkerThread, this, k );
}

void RenderAhead::Silence()
{
    const long long llUs = Timer::NowMicroSecs();
    m_Out.Stop();
    if ( FILE *f = RenderLog() ) { fprintf( f, "Q %lld %lld (silenced)\n", llUs, Timer::NowMicroSecs() ); fflush( f ); }
}

void RenderAhead::Stop()
{
    // The sound first: what was made stops at once, not once the workers are done with what they were
    // making (a soundfont load or a dense piece can take seconds)
    const long long llStopUs = Timer::NowMicroSecs();
    m_Out.Stop();
    if ( FILE *f = RenderLog() ) { fprintf( f, "S %lld %lld (stopped; sound off)\n", llStopUs, Timer::NowMicroSecs() ); fflush( f ); }
    {
        std::lock_guard< std::mutex > lock( m_mx );
        m_bStop = true;
        for ( Claim &c : m_vClaims ) c.pCancel->store( true );
    }
    m_cv.notify_all();
    if ( m_Loader.joinable() ) m_Loader.join();
    if ( m_Pager.joinable() ) m_Pager.join();
    for ( std::thread &t : m_vWorkers ) if ( t.joinable() ) t.join();
    m_vWorkers.clear();
    std::lock_guard< std::mutex > lock( m_mx );
    m_pBank.reset();
    m_mBlocks.clear();
}

void RenderAhead::SetSettings( const Settings &s )
{
    std::lock_guard< std::mutex > lock( m_mx );
    if ( s == m_Settings ) return;
    const Settings old = m_Settings;
    m_Settings = s;
    m_bLimiter.store( s.bLimiter, std::memory_order_relaxed );
    const bool bKestrel = s.eEngine != RenderBackends::EngineBass;
    if ( s.eEngine != old.eEngine || s.vFonts != old.vFonts || ( bKestrel && s.kb.bFilter != old.kb.bFilter ) )
    {
        m_uBankGen++;
        m_cv.notify_all();
    }
    else if ( ( bKestrel ? s.iKestrelVoices != old.iKestrelVoices || !( s.kb == old.kb )
                         : s.bFx != old.bFx || s.iBassVoices != old.iBassVoices || s.iBassThreads != old.iBassThreads || s.iBassNps != old.iBassNps ||
                           s.bBassByChannel != old.bBassByChannel ) ||
              ( s.bLossless != old.bLossless && s.eEngine == RenderBackends::EngineMerlin ) )
        NewGeneration();
    else m_cv.notify_all();     // (how far ahead, the graphics card's share)
}

void RenderAhead::SetSound( const Sound &snd )
{
    std::lock_guard< std::mutex > lock( m_mx );
    if ( snd == m_Sound ) return;
    m_Sound = snd;
    NewGeneration();
}

void RenderAhead::MuteChanged()
{
    std::lock_guard< std::mutex > lock( m_mx );
    NewGeneration();
}

RenderAhead::Status RenderAhead::GetStatus()
{
    Status st;
    std::lock_guard< std::mutex > lock( m_mx );
    st.sEngine = RenderBackends::EngineName( m_Settings.eEngine );
    st.sState = !m_sState.empty() ? m_sState : m_sRenderError;
    st.dAheadSecs = m_Settings.iAheadSecs;
    for ( const Claim &c : m_vClaims ) if ( !c.pCancel->load() ) st.iBusy++;
    st.llGaps = m_llGaps.load( std::memory_order_relaxed );
    st.llDegraded = m_llDegraded.load( std::memory_order_relaxed );
    const long long BF = BlockFrames();
    const double dFrame = SongToFrame( (double)m_Clock.Now() );
    long long b = FloorDiv( (long long)std::floor( dFrame ), BF );
    long long n = 0;
    for ( auto it = m_mBlocks.find( b ); it != m_mBlocks.end() && it->first == b + n; ++it ) n++;
    st.iGpuShare = m_Settings.eEngine != RenderBackends::EngineBass ? m_Settings.iGpuShare : 0;
    st.dReadySecs = n ? std::max( 0.0, ( ( b + n ) * BF - dFrame ) / m_uRate ) : 0.0;
    // Made to the song's end: nothing more to make, so as far ahead as it aims
    if ( n && b + n > FloorDiv( (long long)SongToFrame( (double)m_llEndUs ), BF ) ) st.dReadySecs = st.dAheadSecs;
    st.sWarning = m_sWarning;
    // How much lighter what plays is: the block playing (and the pieces made lighter so far)
    if ( !m_Settings.bAutoDegrade ) st.sDegrade = L"off";
    else
    {
        auto it = m_mBlocks.find( b );
        wchar_t s[64];
        if ( it == m_mBlocks.end() || !it->second->bDegraded ) swprintf( s, 64, L"none (%llu so far)", (unsigned long long)st.llDegraded );
        else if ( it->second->iDegradeVel ) swprintf( s, 64, L"vel < %d (%llu so far)", it->second->iDegradeVel, (unsigned long long)st.llDegraded );
        else swprintf( s, 64, L"%u voices (%llu so far)", it->second->uDegradeCap, (unsigned long long)st.llDegraded );
        st.sDegrade = s;
    }
    return st;
}

//-----------------------------------------------------------------------------
// Scheduling (under m_mx)
//-----------------------------------------------------------------------------

long long RenderAhead::PlayBlock() const
{
    return FloorDiv( (long long)std::floor( SongToFrame( (double)m_Clock.Now() ) ), BlockFrames() );
}

long long RenderAhead::ReadyBlocks( long long llPlay ) const
{
    long long n = 0;
    for ( auto it = m_mBlocks.find( llPlay ); it != m_mBlocks.end() && it->first == llPlay + n && it->second->uGen == m_uGen; ++it ) n++;
    return n;
}

void RenderAhead::NewTimebase( double dSpeed, unsigned uRate )
{
    m_dSpeed = dSpeed;
    m_uRate = uRate;
    m_uBase++;
    m_mBlocks.clear();
    m_pSpill = std::make_shared< SpillFile >( ( (size_t)uRate + uRate / 200 ) * 2 );
    m_sNoRedo.clear();
    for ( Claim &c : m_vClaims ) c.pCancel->store( true );
    m_cv.notify_all();
}

void RenderAhead::NewGeneration()
{
    m_uGen++;
    m_sNoRedo.clear();
    for ( Claim &c : m_vClaims ) if ( c.uGen != m_uGen ) c.pCancel->store( true );
    m_cv.notify_all();
}

void RenderAhead::CancelUnwanted( long long llPlay )
{
    const long long llLast = llPlay + m_Settings.iAheadSecs + RunBlocks;
    for ( Claim &c : m_vClaims )
        if ( c.uGen != m_uGen || c.llFirst + c.llCount <= llPlay || c.llFirst > llLast ) c.pCancel->store( true );
    // Auto-degrade: a full piece that will finish more than 0.3 s after its deadline (and has over a
    // second to go) is given up, to be made again lighter
    if ( !m_Settings.bAutoDegrade || ( !m_Cost.Ready() && !m_CostMerlin.Ready() ) || m_Clock.Paused() ) return;
    const double dFrameNow = SongToFrame( (double)m_Clock.Now() ), dNow = (double)Timer::NowMicroSecs();
    const long long BF = BlockFrames();
    for ( Claim &c : m_vClaims )
    {
        if ( !c.llNotes || c.bLight || c.pCancel->load() ) continue;
        const double dDue = (double)c.llFirst * BF <= dFrameNow ? (double)( c.llFirst + 1 ) * BF : (double)c.llFirst * BF;
        const double dDueUs = dNow + ( dDue - dFrameNow ) / m_uRate * 1e6;
        const CostModel &cm = c.bMerlin ? m_CostMerlin : m_Cost;
        if ( !cm.Ready() ) continue;
        const double dDoneUs = c.llStartUs + cm.Fixed() + cm.PerNote() * c.llNotes;
        if ( dDoneUs > dDueUs + 300000 && dDoneUs - dNow > 1e6 ) c.pCancel->store( true );
    }
}

void RenderAhead::Evict( long long llPlay )
{
    // What is wanted ahead, and what was made behind as far as kept (so going back plays at once); past
    // that, the furthest go first
    const long long llBehind = std::max( 2, m_Settings.iBehindSecs );
    const size_t iKeep = (size_t)( m_Settings.iAheadSecs + llBehind ) + RunBlocks;
    if ( m_mBlocks.size() <= iKeep ) return;
    const long long llLast = llPlay + m_Settings.iAheadSecs, llFirst = llPlay - llBehind;
    std::vector< std::pair< long long, long long > > v;     // distance, block
    for ( const auto &kv : m_mBlocks )
    {
        long long d = kv.first < llFirst ? llFirst - kv.first : kv.first > llLast ? kv.first - llLast : 0;
        v.push_back( { d, kv.first } );
    }
    std::sort( v.begin(), v.end(), []( auto &a, auto &b ) { return a.first > b.first; } );
    for ( size_t i = 0; i < v.size() && m_mBlocks.size() > iKeep && v[i].first > 0; i++ ) m_mBlocks.erase( v[i].second );
}

//-----------------------------------------------------------------------------
// Threads
//-----------------------------------------------------------------------------

void RenderAhead::LoaderThread()
{
    std::unique_lock< std::mutex > lock( m_mx );
    while ( !m_bStop )
    {
        if ( m_uBankGen == m_uLoadedGen ) { m_cv.wait( lock ); continue; }
        const unsigned uGen = m_uBankGen;
        RenderBackends::BankKey key;
        key.eEngine = m_Settings.eEngine;
        key.vFonts = m_Settings.vFonts;
        key.uRate = m_uRate;
        key.bFilter = key.eEngine == RenderBackends::EngineBass || m_Settings.kb.bFilter;
        m_pBank.reset();
        if ( m_sState.rfind( L"no sound", 0 ) != 0 ) m_sState = L"loading the soundfonts";
        lock.unlock();
        std::wstring sError;
        std::shared_ptr< RenderBackends::Bank > pBank = RenderBackends::Acquire( key, sError );
        // What the load reported, and whether Merlin takes the soundfonts (a one-note piece)
        std::wstring sWarning = pBank ? pBank->sWarnings : std::wstring();
        if ( pBank && key.eEngine == RenderBackends::EngineMerlin )
        {
            std::wstring sWhy;
            if ( !pBank->MerlinTakes( sWhy ) )
                sWarning = L"Merlin does not take these soundfonts, so Kestrel makes the audio (" + sWhy + L")" + ( sWarning.empty() ? L"" : L"\n" ) + sWarning;
        }
        lock.lock();
        if ( uGen != m_uBankGen ) continue;     // (asked for something else meanwhile)
        m_uLoadedGen = uGen;
        m_pBank = pBank;
        m_sWarning = sWarning;
        if ( FILE *f = RenderLog() ) { fprintf( f, "W %ls\n", sWarning.c_str() ); fflush( f ); }
        RenderBackends::SetNote( RenderBackends::NoteLoad, sWarning );
        if ( pBank && m_Settings.bDiscardOnSwitch && m_eLoadedEngine >= 0 && m_eLoadedEngine != key.eEngine ) m_mBlocks.clear();
        if ( pBank ) m_eLoadedEngine = key.eEngine;
        m_Cost = CostModel();
        m_CostMerlin = CostModel();
        if ( m_sState.rfind( L"no sound", 0 ) != 0 ) m_sState = pBank ? std::wstring() : sError;
        NewGeneration();
    }
}

//-----------------------------------------------------------------------------
// Kestrel's continuous render (Maestro's way): one synth kept from block to block
//-----------------------------------------------------------------------------

// What a continuous render keeps between blocks. Its events go through a queue that reads StreamLookSecs past
// what is pushed, so each note-on's off is known: with stack collapsing, copies of a note-on (channel, key,
// velocity, frame) that also end on the same frame are one voice at their summed loudness, as Kestrel's own
// coalescing merges voices identical to their release (copies ending apart, or past the look-ahead, are pushed
// as they are). The offs pair with the ons first in, first out, as Kestrel pairs them, so leaving out a merged
// copy's on and off together keeps every other pairing
struct RenderAhead::StreamRun
{
    std::shared_ptr< RenderBackends::Bank > pBank;
    std::unique_ptr< RenderBackends::Stream > p;
    std::unique_ptr< PlaybackDecoder > pDec;
    std::unique_ptr< MidiOutput > pTune;
    PieceOut *pTuned = nullptr;
    PlaybackDecoder::Params params;
    bool bUnavailable = false;          // (no continuous render here: pieces only)
    bool bLossless = true;
    unsigned uGen = 0, uBase = 0;
    long long llNext = LLONG_MIN;       // the block it makes next
    long long llFp = 0;                 // the frame (of the timebase) its frame 0 is
    long long tp = 0;                   // the song time it started at
    double dSpeed = 1.0;
    unsigned uRate = 48000;
    long long tDecoded = 0;             // decoded up to this song time
    bool bDecoded = false;              // to the song's end
    std::vector< float > vCarry;        // its next block's first CrossfadeFrames, made already
    unsigned uCap = 0, uMax = 0;        // the voice cap now, and as set
    double dSecUs = 5e5;                // what a second of audio takes it (an average)
    uint64_t llMerged = 0;              // copies merged into another note-on (for the log)
    long long llPushed = 0;             // frames pushed up to (exclusive)
    long long llRendered = 0;           // frames made

    struct Ev { uint64_t llFrame; uint32_t uMsg; uint32_t uCount; int64_t llPartner; uint64_t llOff; bool bDead; };
    std::deque< Ev > q;
    uint64_t llBase = 0;                // the sequence number of q.front()
    std::vector< std::deque< uint64_t > > vFifo = std::vector< std::deque< uint64_t > >( 2048 );
    std::vector< uint32_t > vBuf = std::vector< uint32_t >( 65536 );
    std::vector< uint64_t > vF;
    std::vector< uint32_t > vM;
    std::vector< float > vS;
    std::unordered_map< uint64_t, size_t > mGroup;

    void Reset()
    {
        q.clear();
        llBase = 0;
        for ( auto &d : vFifo ) d.clear();
        llPushed = 0;
        llRendered = 0;
    }
    void Append( uint64_t llFrame, uint32_t m )
    {
        uint32_t s = m & 0xF0;
        if ( s == 0x90 && !( m & 0x7F0000 ) ) { m = 0x80 | ( m & 0xFF0F ); s = 0x80; }
        const uint64_t llSeq = llBase + q.size();
        const uint32_t uSlot = ( m & 0xF ) << 7 | ( ( m >> 8 ) & 0x7F );
        Ev e{ llFrame, m, 1, -1, UINT64_MAX, false };
        if ( s == 0x90 ) vFifo[uSlot].push_back( llSeq );
        else if ( s == 0x80 && !vFifo[uSlot].empty() )
        {
            const uint64_t llOn = vFifo[uSlot].front();
            vFifo[uSlot].pop_front();
            e.llPartner = (int64_t)llOn;
            if ( llOn >= llBase )
            {
                Ev &on = q[(size_t)( llOn - llBase )];
                on.llOff = llFrame;
                on.llPartner = (int64_t)llSeq;
            }
        }
        q.push_back( e );
    }
    // Pushes what comes before frame llEnd (a frame's merged copies as one)
    void Push( long long llEnd )
    {
        while ( !q.empty() && (long long)q.front().llFrame < llEnd )
        {
            const uint64_t f = q.front().llFrame;
            size_t n = 0;
            while ( n < q.size() && q[n].llFrame == f ) n++;
            if ( bLossless )
            {
                mGroup.clear();
                for ( size_t i = 0; i < n; i++ )
                {
                    Ev &e = q[i];
                    if ( e.bDead || ( e.uMsg & 0xF0 ) != 0x90 || e.llPartner < 0 ) continue;
                    const uint64_t key = e.llOff << 18 | ( ( e.uMsg & 0xF ) << 7 | ( ( e.uMsg >> 8 ) & 0x7F ) ) << 7 | ( ( e.uMsg >> 16 ) & 0x7F );
                    auto it = mGroup.find( key );
                    if ( it == mGroup.end() ) { mGroup.emplace( key, i ); continue; }
                    q[it->second].uCount++;
                    llMerged++;
                    e.bDead = true;
                    q[(size_t)( (uint64_t)e.llPartner - llBase )].bDead = true;
                }
            }
            vF.clear(); vM.clear(); vS.clear();
            for ( size_t i = 0; i < n; i++ )
            {
                const Ev &e = q[i];
                if ( e.bDead ) continue;
                vF.push_back( e.llFrame );
                vM.push_back( e.uMsg );
                vS.push_back( (float)e.uCount );
            }
            if ( !vF.empty() ) p->Push( vF.data(), vM.data(), vS.data(), vF.size() );
            q.erase( q.begin(), q.begin() + (ptrdiff_t)n );
            llBase += n;
        }
        llPushed = std::max( llPushed, llEnd );
    }
};

namespace
{
    // (developer switch PFAVIZ_KSTREAM=0: Kestrel in pieces, as before)
    bool StreamWanted() { static const bool s_b = !getenv( "PFAVIZ_KSTREAM" ) || atoi( getenv( "PFAVIZ_KSTREAM" ) ) != 0; return s_b; }
}


void RenderAhead::WorkerThread( int k )
{
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
    PlaybackDecoder dec( *m_pSong );
    dec.SetMuteMask( m_pMuteMask );
    std::vector< std::vector< uint8_t > > vMidi;
    std::vector< uint32_t > vBuf( 65536 );
    std::vector< float > vAudio;
    auto pRun = std::make_unique< StreamRun >();

    std::unique_lock< std::mutex > lock( m_mx );
    while ( !m_bStop )
    {
        if ( m_bHold ) { pRun->p.reset(); m_cv.wait_for( lock, std::chrono::milliseconds( 200 ) ); continue; }
        RenderBackends::PieceOpts opts;
        opts.uVoices = (unsigned)( m_Settings.eEngine == RenderBackends::EngineBass ? m_Settings.iBassVoices : m_Settings.iKestrelVoices );
        opts.iBassThreads = m_Settings.iBassThreads;
        opts.bBassByChannel = m_Settings.bBassByChannel;
        opts.uChannels = m_uChannels;
        opts.bFx = m_Settings.bFx;
        opts.kb = m_Settings.kb;
        opts.eEngine = m_Settings.eEngine;
        // (the parts that work: by channel, those of the song's channels with notes)
        auto UsedParts = [&]( const RenderBackends::Bank &bank ) {
            const int iParts = bank.Parts( opts );
            if ( !opts.bBassByChannel || iParts < 16 ) return iParts;
            int nCh = 0;
            for ( int c = 0; c < 16; c++ ) nCh += ( m_uChannels >> c ) & 1;
            return std::max( 1, nCh ) * ( iParts / 16 );
        };
        if ( !m_pBank || k >= m_pBank->Parallel( UsedParts( *m_pBank ) ) ) { m_cv.wait_for( lock, std::chrono::milliseconds( 200 ) ); continue; }
        const bool bGpu = m_pBank->Engine() != RenderBackends::EngineBass;
        const bool bWhole = bGpu && m_uOfflineRate && m_Settings.bWhole;
        if ( bWhole && k > 0 ) { m_cv.wait_for( lock, std::chrono::milliseconds( 200 ) ); continue; }
        const long long P = PlayBlock();
        CancelUnwanted( P );
        const long long llEndBlock = FloorDiv( (long long)SongToFrame( (double)m_llEndUs ), BlockFrames() );
        // (offline: no further than RenderOffline goes)
        const long long llLast = std::min( { P + m_Settings.iAheadSecs - 1, llEndBlock, m_uOfflineRate ? m_llOfflineLast : LLONG_MAX } );
        // Missing or of an older sound first; once well ahead, those made lighter
        bool bRedo = false;
        auto Needed = [&]( long long b ) {
            for ( const Claim &c : m_vClaims )
                if ( !c.pCancel->load() && b >= c.llFirst && b < c.llFirst + c.llCount ) return false;
            auto it = m_mBlocks.find( b );
            return it == m_mBlocks.end() || it->second->uGen != m_uGen || ( bRedo && it->second->bDegraded && !m_sNoRedo.count( b ) );
        };
        long long b = P;
        while ( b <= llLast && !Needed( b ) ) b++;
        if ( b > llLast && ReadyBlocks( P ) >= std::min< long long >( Settings::GpuCalmSecs, m_Settings.iAheadSecs / 2 ) )
        {
            bRedo = true;
            for ( b = P; b <= llLast && !Needed( b ); ) b++;
        }
        if ( b > llLast ) { m_cv.wait_for( lock, std::chrono::milliseconds( 100 ) ); continue; }
        // Kestrel: the blocks from here on made by its continuous render, one after another (no synth, file and
        // preroll a piece); what is made again in full, and Merlin's, in pieces
        if ( bGpu && !bWhole && !bRedo && StreamWanted() && !pRun->bUnavailable )
        {
            std::wstring sWhy;
            const bool bKestrel = opts.eEngine == RenderBackends::EngineKestrel ||
                                  ( m_Sound.bTuning31 && !m_Sound.bTuningBend ) || !m_pBank->MerlinTakes( sWhy );
            if ( bKestrel )
            {
                // Where it cannot be in time (a start takes its preroll and first block), it goes on, or starts,
                // that far ahead: the blocks before are lost either way, and chasing the playback position
                // would never catch up
                long long t = b;
                const bool bSame = pRun->p && pRun->uGen == m_uGen && pRun->uBase == m_uBase && pRun->pBank == m_pBank;
                const long long nLead = (long long)std::ceil( 2.0 * pRun->dSecUs / 1e6 ) + 1;
                if ( bSame && pRun->llNext > b && pRun->llNext <= llLast && pRun->llNext <= P + nLead && Needed( pRun->llNext ) ) t = pRun->llNext;
                else if ( !( bSame && pRun->llNext == b ) && b <= P + 1 && !m_Clock.Paused() )
                {
                    t = std::min( llLast, P + nLead );
                    while ( t <= llLast && !Needed( t ) ) t++;
                    if ( t > llLast ) t = b;
                }
                if ( StreamBlock( *pRun, t, lock, opts ) ) continue;
            }
        }
        // Auto-degrade looks at every piece's deadline: when its first block starts playing (the
        // block playing: when it ends). Made again in full later, so not for those
        const bool bAtEdge = m_Settings.bAutoDegrade && !bRedo && !m_Clock.Paused();
        CostModel cost = m_Cost;
        const CostModel costMerlin = m_CostMerlin;
        const bool bLossless = m_Settings.bLossless;
        long long n = 1;
        // Short next to the playback position (in time after a seek); on the processor, long once far
        // ahead, where a piece's start costs the most (its preroll and held notes made again: 20-block
        // pieces took a fifth less processor time a second than 6-block ones). Offline nothing plays
        // meanwhile: long pieces, longer still on the graphics card, whose cost is mostly per piece
        // (developer switch PFAVIZ_RUNBLOCKS=n: offline pieces of n blocks)
        static const long long s_llRun = getenv( "PFAVIZ_RUNBLOCKS" ) ? std::max( 1, atoi( getenv( "PFAVIZ_RUNBLOCKS" ) ) ) : 0;
        const long long nMax = bWhole ? WholeRunBlocks : m_uOfflineRate && s_llRun ? s_llRun : m_uOfflineRate ? ( bGpu ? OfflineGpuRunBlocks : LongRunBlocks )
                             : b <= P + 1 ? ( bGpu ? GpuFirstRunBlocks : FirstRunBlocks ) : bGpu ? ( opts.eEngine == RenderBackends::EngineMerlin && b >= P + LongRunBlocks ? MerlinLongRunBlocks : GpuRunBlocks )
                             : b >= P + LongRunBlocks ? LongRunBlocks : RunBlocks;
        while ( n < nMax && b + n <= llLast && Needed( b + n ) ) n++;
        // At the far end of the buffer, as it moves on a block a second: while over half of it is still
        // ahead, wait for a full piece's worth (else it is made a block at a time, at several times the cost)
        if ( n < nMax && b + n > llLast && llLast < llEndBlock && b - P > m_Settings.iAheadSecs / 2 )
        {
            m_cv.wait_for( lock, std::chrono::milliseconds( 100 ) );
            continue;
        }

        Claim claim{ b, n, m_uGen, std::make_shared< std::atomic< bool > >( false ) };
        m_vClaims.push_back( claim );
        auto Unclaim = [&]() {
            for ( size_t i = 0; i < m_vClaims.size(); i++ )
                if ( m_vClaims[i].pCancel == claim.pCancel ) { m_vClaims.erase( m_vClaims.begin() + i ); break; }
        };
        std::shared_ptr< RenderBackends::Bank > pBank = m_pBank;
        Sound snd = m_Sound;
        const long long llNps = bGpu ? 0 : m_Settings.iBassNps;
        // (Merlin does not read scale tuning: those 31-EDO pieces are Kestrel's; pitch bends it plays)
        if ( snd.bTuning31 && !snd.bTuningBend && opts.eEngine == RenderBackends::EngineMerlin ) opts.bForceKestrel = true;
        const double dSpeed = m_dSpeed;
        const unsigned uRate = m_uRate, uBase = m_uBase;
        lock.unlock();
        const long long llT0 = Timer::NowMicroSecs();

        uint64_t llSkip = 0, llFrames = 0;
        bool bSilent = false, bOk;
        long long nGot = n;
        bool bRepeat = bLossless && pBank->RepeatMarks( opts );
        PieceStats st;
        // Merlin's piece: timed by a model of its own (measured: about 0.4 s a piece, then 0.2 ms per thousand
        // note-ons on a dense song, however few distinct waves they use); next to the playback position its
        // preroll is not taken back to held notes' starts
        std::wstring sWhyNot;
        const bool bMerlinPiece = bGpu && opts.eEngine == RenderBackends::EngineMerlin && !opts.bForceKestrel && pBank->MerlinTakes( sWhyNot );
        if ( bWhole )
        {
            m_fWholeProgress.store( 0.0f );
            opts.pProgress = &m_fWholeProgress;
        }
        bOk = BuildPiece( dec, b, nGot, snd, dSpeed, uRate, bRepeat ? 1 : pBank->Parts( opts ), opts.bBassByChannel, bRepeat ? 0 : llNps, bRepeat, st, vMidi, llSkip, llFrames, bSilent, vBuf,
                          claim.pCancel.get(), bMerlinPiece && b <= P + 1 );
        uint64_t llWork = st.llNoteOns;
        if ( bMerlinPiece ) cost = costMerlin;

        // Making a lighter piece again in full: only when it would be made before it plays (else that
        // block stays as it is)
        if ( bOk && !bSilent && bRedo && cost.Ready() )
        {
            const double dFrameNow = m_Clock.Now() * (double)uRate / ( 1e6 * dSpeed );
            const double dLeftUs = ( (double)b * uRate - dFrameNow ) / uRate * 1e6 - (double)( Timer::NowMicroSecs() - llT0 );
            if ( cost.Fixed() + cost.PerNote() * llWork > dLeftUs * 0.8 )
            {
                lock.lock();
                for ( long long i = 0; i < nGot; i++ ) m_sNoRedo.insert( b + i );
                Unclaim();
                continue;
            }
        }

        // Auto-degrade: would it be made before the block playing ends? If not, lighter
        bool bDegraded = false;
        int iDegradeVel = 0;
        unsigned uDegradeCap = 0;
        double dNeedLog = -1, dLeftLog = -1;
        if ( bOk && !bSilent && bAtEdge && cost.Ready() )
        {
            const double dFrameNow = m_Clock.Now() * (double)uRate / ( 1e6 * dSpeed );
            const double dDue = (double)b * uRate <= dFrameNow ? (double)( b + 1 ) * uRate : (double)b * uRate;
            const double dLeftUs = ( dDue - dFrameNow ) / uRate * 1e6 - (double)( Timer::NowMicroSecs() - llT0 );
            const double dNeedUs = cost.Fixed() + cost.PerNote() * llWork;
            dNeedLog = dNeedUs / 1000;
            dLeftLog = dLeftUs / 1000;
            if ( dNeedUs > dLeftUs )
            {
                // The share of the notes' cost that fits (with a fifth to spare)
                const double r = std::clamp( ( dLeftUs * 0.8 - cost.Fixed() ) / std::max( 1.0, cost.PerNote() * llWork ), 0.02, 1.0 );
                bDegraded = true;
                if ( pBank->Engine() == RenderBackends::EngineBass || bMerlinPiece )
                {
                    // The quietest note-ons skipped (with their offs) until what is left fits
                    Sound lighter = snd;
                    uint64_t llKeep = 0, llAllow = (uint64_t)( st.llNoteOns * r );
                    int v = 127;
                    while ( v > 1 && llKeep + st.aVel[v] <= llAllow ) llKeep += st.aVel[v--];
                    lighter.iMinVelocity = std::max( snd.iMinVelocity, v + 1 );
                    iDegradeVel = lighter.iMinVelocity;
                    nGot = n;
                    bOk = BuildPiece( dec, b, nGot, lighter, dSpeed, uRate, bRepeat ? 1 : pBank->Parts( opts ), opts.bBassByChannel, bRepeat ? 0 : llNps, bRepeat, st, vMidi, llSkip, llFrames,
                                      bSilent, vBuf, claim.pCancel.get(), bMerlinPiece && b <= P + 1 );
                    llWork = st.llNoteOns;
                }
                else
                {
                    // Kestrel's voice cap, a share of the most notes held at once (Merlin's piece goes to Kestrel)
                    opts.uVoices = (unsigned)std::clamp< double >( st.llPolyMax * r, 128.0, (double)opts.uVoices );
                    opts.bForceKestrel = true;
                    uDegradeCap = opts.uVoices;
                    if ( bRepeat )
                    {
                        bRepeat = false;
                        nGot = n;
                        bOk = BuildPiece( dec, b, nGot, snd, dSpeed, uRate, 1, false, 0, false, st, vMidi, llSkip, llFrames, bSilent, vBuf, claim.pCancel.get() );
                    }
                }
                m_llDegraded.fetch_add( 1, std::memory_order_relaxed );
            }
        }
        // (what the lateness check needs of it)
        lock.lock();
        for ( Claim &c : m_vClaims )
            if ( c.pCancel == claim.pCancel ) { c.llNotes = bSilent ? 0 : llWork; c.llStartUs = llT0; c.bLight = bDegraded; c.bMerlin = bMerlinPiece; }
        if ( bWhole ) { m_llWholeFirst = b; m_llWholeCount = nGot; }
        lock.unlock();
        std::wstring sError, sFellBack;
        const long long llT1 = Timer::NowMicroSecs();
        if ( bOk && !claim.pCancel->load() )
        {
            if ( bSilent ) vAudio.assign( (size_t)llFrames * 2, 0.0f );
            else
            {
                bOk = pBank->Render( vMidi, llSkip, llFrames, opts, claim.pCancel.get(), vAudio, sError );
                // Merlin refused the piece: on Kestrel (made again without repeat marks, which it plays once)
                if ( !bOk && opts.eEngine == RenderBackends::EngineMerlin && !opts.bForceKestrel && !claim.pCancel->load() )
                {
                    sFellBack = sError.empty() ? std::wstring( L"?" ) : sError;
                    nGot = n;
                    opts.bForceKestrel = true;
                    bOk = BuildPiece( dec, b, nGot, snd, dSpeed, uRate, 1, false, 0, false, st, vMidi, llSkip, llFrames, bSilent, vBuf, claim.pCancel.get() ) &&
                          pBank->Render( vMidi, llSkip, llFrames, opts, claim.pCancel.get(), vAudio, sError );
                }
            }
        }
        const long long llBusyUs = Timer::NowMicroSecs() - llT0;
        const long long llRenderUs = Timer::NowMicroSecs() - llT1;
        const bool bCancelled = claim.pCancel->load();
        // Developer switch PFAVIZ_RENDERLOG=file: a line per piece (wall ms, first block, blocks, the
        // playback block then, ms taken, silent, ok, cancelled, the MIDI's bytes, the error)
        FILE *s_pLog = RenderLog();
        if ( s_pLog && getenv( "PFAVIZ_PIECEDUMP" ) && !vMidi.empty() )
        {
            // (and with PFAVIZ_PIECEDUMP=folder, each piece's MIDI as <first block>.mid)
            if ( FILE *f = fopen( ( std::string( getenv( "PFAVIZ_PIECEDUMP" ) ) + "\\" + std::to_string( b ) + ".mid" ).c_str(), "wb" ) )
            {
                fwrite( vMidi[0].data(), 1, vMidi[0].size(), f );
                fclose( f );
            }
        }
        if ( s_pLog )
        {
            size_t nBytes = 0;
            for ( auto &v : vMidi ) nBytes += v.size();
            fprintf( s_pLog, "%lld %lld %lld %lld %.1f %d %d %d %zu %llu %d %d %.0f %.0f %d render %.1f %ls\n", llT0 / 1000, b, nGot, P, llBusyUs / 1000.0, bSilent, bOk,
                     bCancelled, nBytes, (unsigned long long)llWork, bDegraded, bRepeat, dNeedLog, dLeftLog, cost.n, llRenderUs / 1000.0,
                     ( sError + ( sFellBack.empty() ? L"" : L" [to Kestrel: " + sFellBack + L"]" ) ).c_str() );
            fflush( s_pLog );
        }
        // A failed piece is kept as silence (so it is not made again and again), with the reason shown
        if ( !bOk && !bCancelled ) vAudio.assign( (size_t)llFrames * 2, 0.0f );
        std::vector< BlockPtr > vNew;
        if ( !bCancelled && llFrames > 0 )
        {
            const size_t BF = uRate, XF = uRate / 200;
            for ( long long i = 0; i < nGot; i++ )
            {
                auto p = std::make_shared< Block >();
                p->uGen = claim.uGen;
                p->bDegraded = bDegraded;
                p->iDegradeVel = iDegradeVel;
                p->uDegradeCap = uDegradeCap;
                p->bPieceStart = i == 0;
                p->vAudio.assign( vAudio.begin() + i * BF * 2, vAudio.begin() + ( i + 1 ) * BF * 2 );
                if ( i == nGot - 1 ) p->vOver.assign( vAudio.begin() + nGot * BF * 2, vAudio.begin() + ( nGot * BF + XF ) * 2 );
                vNew.push_back( p );
            }
        }

        lock.lock();
        Unclaim();
        if ( bWhole ) m_llWholeFirst = LLONG_MIN;
        if ( !bOk && !bCancelled ) m_sRenderError = sError.empty() ? std::wstring( L"a piece could not be made" ) : sError;
        else if ( bOk && !bSilent ) m_sRenderError.clear();
        // (what full pieces take: the piece's own MIDI building counted in, as the decision counts it)
        if ( bOk && !bCancelled && !bSilent && !bDegraded && pBank == m_pBank ) ( bMerlinPiece ? m_CostMerlin : m_Cost ).Add( (double)llWork, (double)llBusyUs );
        if ( uBase == m_uBase )
        {
            for ( long long i = 0; i < (long long)vNew.size(); i++ )
            {
                // (a lighter block never replaces a full one of its sound)
                auto it = m_mBlocks.find( b + i );
                if ( it == m_mBlocks.end() || it->second->uGen < claim.uGen || ( it->second->uGen == claim.uGen && ( !bDegraded || it->second->bDegraded ) ) )
                    m_mBlocks[b + i] = vNew[i];
            }
            // (the block after, made apart from these (a continuous render's), fades in over their overrun)
            auto itNext = m_mBlocks.find( b + (long long)vNew.size() );
            if ( !vNew.empty() && itNext != m_mBlocks.end() && !itNext->second->bPieceStart && !itNext->second->Spilled() )
            {
                auto pCopy = std::make_shared< Block >( *itNext->second );
                pCopy->bPieceStart = true;
                itNext->second = pCopy;
            }
            Evict( P );
        }
        m_cv.notify_all();

        // Sharing the graphics card with the picture: once well ahead, rest so the card makes audio at
        // most iGpuShare of the time; running low (or a seek, a change) ends the rest at once
        if ( bGpu && !bWhole && !bSilent && m_Settings.iGpuShare < 100 )
        {
            const long long llRestUs = std::min< long long >( 3000000, llBusyUs * ( 100 - m_Settings.iGpuShare ) / std::max( 1, m_Settings.iGpuShare ) );
            const long long llUntil = Timer::NowMicroSecs() + llRestUs;
            const unsigned uGenNow = m_uGen, uBaseNow = m_uBase;
            auto Calm = [&]() {
                const long long Pn = PlayBlock();
                return ReadyBlocks( Pn ) >= std::min< long long >( Settings::GpuCalmSecs, m_Settings.iAheadSecs / 2 );
            };
            while ( !m_bStop && !m_bHold && m_uGen == uGenNow && m_uBase == uBaseNow && Calm() )
            {
                const long long llLeft = llUntil - Timer::NowMicroSecs();
                if ( llLeft <= 0 ) break;
                m_cv.wait_for( lock, std::chrono::microseconds( std::min< long long >( llLeft, 50000 ) ) );
            }
        }
    }
}

//-----------------------------------------------------------------------------
// The piece's MIDI
//-----------------------------------------------------------------------------

std::wstring RenderAhead::MerlinIssues( const Song &song )
{
    // Volume or expression set more than once on a channel: it changes during the song
    int aVolume[16] = {};
    uint64_t llBends = 0;
    for ( const ChaseEvent &e : song.vChase )
    {
        const uint32_t s = e.uMsg & 0xF0, ch = e.uMsg & 0xF, cc = ( e.uMsg >> 8 ) & 0x7F;
        if ( s == 0xB0 && ( cc == 7 || cc == 11 ) ) aVolume[ch]++;
        else if ( s == 0xE0 ) llBends++;
    }
    int nChannels = 0;
    for ( int n : aVolume ) nChannels += n > 1;
    std::wstring s;
    if ( nChannels )
        s = L"it changes channel volume or expression (CC7, CC11) on " + std::to_wstring( nChannels ) + ( nChannels == 1 ? L" channel" : L" channels" ) +
            L", which Merlin does not apply properly yet";
    if ( llBends > 10000 )
        s += ( s.empty() ? L"" : L"; " ) + std::wstring( L"it bends a lot (" ) + std::to_wstring( llBends ) + L" pitch bends), which Merlin renders slowly";
    return s;
}

long long RenderAhead::HeldEarliest( long long llAt, long long llFloor ) const
{
    const PackedMonotone &times = m_pSong->stream.times;
    if ( !times.Size() || llAt <= 0 ) return llAt;
    const uint64_t u0 = times.LowerBound( (uint64_t)llAt ), uH = llFloor <= 0 ? 0 : times.LowerBound( (uint64_t)llFloor );
    long long llBest = llAt;
    for ( int k = 0; k < 128; k++ )
    {
        const KeyStore &ks = m_pSong->visual.aKeys[k];
        if ( !ks.Size() ) continue;
        size_t hi = ks.LowerBoundStart( u0 ), lo = ks.LowerBoundStart( uH );
        if ( lo >= hi || ks.RightmostEnd( lo, hi, u0 ) < 0 ) continue;
        // The leftmost note of [lo, hi) still held at u0
        size_t a = lo, z = hi;
        while ( a < z )
        {
            size_t m = a + ( z - a ) / 2;
            if ( ks.RightmostEnd( lo, m + 1, u0 ) >= 0 ) z = m;
            else a = m + 1;
        }
        llBest = std::min( llBest, (long long)times.Get( ks.StartU( a ) ) );
    }
    return llBest;
}

// Notes held at llAt that started before it: struck again there (velocity 100, corrected), at most a few
// per key
void RenderAhead::HeldNotes( long long llAt, std::vector< uint32_t > &vOut, double dVelocityCorrect ) const
{
    const PackedMonotone &times = m_pSong->stream.times;
    if ( !times.Size() || llAt <= 0 ) return;
    const uint64_t u = times.LowerBound( (uint64_t)llAt );
    if ( u == 0 ) return;
    const uint32_t uVel = (uint32_t)std::clamp( (int)std::lround( 100 * dVelocityCorrect ), 1, 127 );
    const size_t nTracks = m_pSong->vTracks.size();
    NoteView n;
    for ( int k = 0; k < 128; k++ )
    {
        const KeyStore &ks = m_pSong->visual.aKeys[k];
        if ( !ks.Size() ) continue;
        size_t hi = ks.LowerBoundStart( u );
        int64_t j;
        for ( int c = 0; c < 4 && ( j = ks.RightmostEnd( 0, hi, u ) ) >= 0; c++ )
        {
            ks.Get( (size_t)j, n );
            const uint32_t uTrack = n.uSlot >> 4, uCh = n.uSlot & 15;
            if ( !( uTrack < nTracks && m_pMuteMask && ( m_pMuteMask[uTrack].load( std::memory_order_relaxed ) >> uCh ) & 1 ) )
                vOut.push_back( 0x90 | uCh | ( (uint32_t)k << 8 ) | ( uVel << 16 ) );
            hi = (size_t)j;
        }
    }
}

void RenderAhead::CostModel::Add( double dNotes, double dUs )
{
    const double w = n == 0 ? 1.0 : 0.2;
    sN += w * ( dNotes - sN );
    sT += w * ( dUs - sT );
    sNN += w * ( dNotes * dNotes - sNN );
    sNT += w * ( dNotes * dUs - sNT );
    n++;
}

double RenderAhead::CostModel::PerNote() const
{
    const double dVar = sNN - sN * sN;
    if ( dVar > 1e-6 * std::max( 1.0, sNN ) )
    {
        const double c = ( sNT - sN * sT ) / dVar;
        if ( c > 0.0 ) return c;
    }
    return sT / std::max( 1.0, sN );
}

double RenderAhead::CostModel::Fixed() const
{
    return std::max( 0.0, sT - PerNote() * sN );
}

bool RenderAhead::BuildPiece( PlaybackDecoder &dec, long long llFirst, long long &llCount, const Sound &snd, double dSpeed, unsigned uRate,
                              int iParts, bool bByChannel, long long llNps, bool bRepeat, PieceStats &st, std::vector< std::vector< uint8_t > > &vMidi, uint64_t &llSkip, uint64_t &llFrames, bool &bSilent, std::vector< uint32_t > &vBuf,
                              const std::atomic< bool > *pCancel, bool bShortPreroll )
{
    const long long BF = uRate, XF = uRate / 200, PRE = (long long)std::llround( PrerollSecs * uRate );
    auto F2S = [&]( double f ) { return f * 1e6 * dSpeed / uRate; };
    auto S2F = [&]( double t ) { return t * uRate / ( 1e6 * dSpeed ); };
    const long long F0 = llFirst * BF;
    long long FEnd = ( llFirst + llCount ) * BF + XF;

    // Early enough for what is ringing: the preroll, back to the start of a note still held there
    long long Fp = F0 - PRE;
    const long long tp0 = (long long)std::floor( F2S( (double)Fp ) );
    const long long tHeld = bShortPreroll ? tp0 : HeldEarliest( tp0, tp0 - (long long)( HeldSecs * 1e6 * dSpeed ) );
    if ( tHeld < tp0 ) Fp = std::min( Fp, (long long)std::floor( S2F( (double)tHeld ) ) - 1 );
    const double dTp = F2S( (double)Fp );
    const long long tp = (long long)std::ceil( dTp );
    const double dTicksPerFrame = 1e6 / uRate / TickUs;

    std::vector< uint32_t > vStart;
    dec.SetBendChase( snd.bTuning31 && snd.bTuningBend );
    dec.Seek( tp, tp, vStart );
    const size_t nChase = vStart.size();
    HeldNotes( tp, vStart, snd.dVelocityCorrect );
    bSilent = vStart.size() == nChase;

    // Split by key over iParts files (several BASSMIDI streams: more voices); the rest goes to all of them
    const size_t K = (size_t)std::max( 1, iParts );
    vMidi.resize( K );
    std::vector< SmfWriter > vW;
    vW.reserve( K );
    for ( size_t i = 0; i < K; i++ ) vW.emplace_back( vMidi[i] );
    // (a part without notes is left empty at the end: silent, nothing to make)
    std::vector< char > vHasNotes( K, K == 1 );
    RenderBackends::PieceOpts po;
    po.bBassByChannel = bByChannel;
    // Each instance's notes-a-second limit (llNps; OmniMIDI's NpsLimiter): its notes taken in the last
    // second, counted by millisecond (a tick is a 48000th of a second of audio), or its current millisecond's
    // at a second's rate (x 4/3) when more; a note-on is taken while velocity / 127 x the limit is above
    // that, so the quietest go first, and a note-on not taken takes its off with it
    struct NpsWindow { std::array< uint32_t, 1000 > a{}; uint64_t llTotal = 0; int64_t llMs = -1; std::vector< uint32_t > vMissed = std::vector< uint32_t >( 2048 ); };
    std::vector< NpsWindow > vNps( llNps > 0 ? K : 0 );
    auto NpsTakes = [&]( NpsWindow &w, uint64_t llTick, uint32_t m ) {
        const uint32_t s = m & 0xF0, v = ( m >> 16 ) & 0x7F, uSlot = ( m & 0xF ) << 7 | ( ( m >> 8 ) & 0x7F );
        if ( s == 0x90 && v )
        {
            const int64_t ms = (int64_t)( llTick / 48 );
            if ( w.llMs < 0 || ms - w.llMs >= 1000 ) { w.a.fill( 0 ); w.llTotal = 0; w.llMs = ms; }
            while ( w.llMs < ms ) { w.llMs++; uint32_t &c = w.a[(size_t)( w.llMs % 1000 )]; w.llTotal -= c; c = 0; }
            uint32_t &cNow = w.a[(size_t)( w.llMs % 1000 )];
            const uint64_t llRate = std::max< uint64_t >( w.llTotal, (uint64_t)cNow * 1000 * 4 / 3 );
            if ( (uint64_t)v * (uint64_t)llNps / 127 <= llRate ) { w.vMissed[uSlot]++; return false; }
            cNow++;
            w.llTotal++;
            return true;
        }
        if ( ( s == 0x80 || s == 0x90 ) && w.vMissed[uSlot] ) { w.vMissed[uSlot]--; return false; }
        return true;
    };
    // (bFree: past the limiter, for the offs of notes it took)
    auto Put = [&]( uint64_t llTick, uint32_t m, bool bFree = false ) {
        const uint32_t s = m & 0xF0;
        if ( s == 0x80 || s == 0x90 || s == 0xA0 )
        {
            const int i = K > 1 ? RenderBackends::Bank::PartOf( po, (int)K, m & 0xF, ( m >> 8 ) & 0x7F ) : 0;
            if ( !bFree && !vNps.empty() && s != 0xA0 && !NpsTakes( vNps[(size_t)i], llTick, m ) ) return false;
            vW[i].Msg( llTick, m );
            vHasNotes[i] = 1;
        }
        else for ( SmfWriter &w : vW ) w.Msg( llTick, m );
        return true;
    };
    auto Bytes = [&]() { size_t n = 0; for ( auto &v : vMidi ) n += v.size(); return n; };
    // The notes' statistics: note-ons by velocity, and the most held at once (each channel and key's
    // offs taken against its ons)
    st = PieceStats();
    std::vector< uint32_t > vHeld( 2048, 0 );
    uint64_t llHeld = 0;
    auto Count = [&]( uint32_t m, uint32_t n ) {
        const uint32_t s = m & 0xF0, uSlot = ( m & 0xF ) << 7 | ( ( m >> 8 ) & 0x7F ), v = ( m >> 16 ) & 0x7F;
        if ( s == 0x90 && v ) { st.llNoteOns += n; st.aVel[v] += n; vHeld[uSlot] += n; llHeld += n; st.llPolyMax = std::max( st.llPolyMax, llHeld ); }
        else if ( s == 0x80 || s == 0x90 ) { const uint32_t k = std::min( n, vHeld[uSlot] ); vHeld[uSlot] -= k; llHeld -= k; }
    };
    // 31-EDO: every message through the tuned output, as the MIDI output gets them (its scale tuning
    // first, at the start)
    std::unique_ptr< MidiOutput > pTune;
    PieceOut *pTuned = nullptr;
    if ( snd.bTuning31 )
    {
        auto pOut = std::make_unique< PieceOut >();
        pTuned = pOut.get();
        if ( snd.bTuningBend ) pTune = std::make_unique< PitchBendTunedOutput >( std::move( pOut ) );
        else
        {
            auto pScale = std::make_unique< ScaleTunedOutput >( std::move( pOut ) );
            pScale->Init();
            pTune = std::move( pScale );
        }
        for ( const auto &v : pTuned->vSysEx )
            for ( SmfWriter &w : vW ) w.SysEx( 0, v.data(), v.size() );
    }
    auto Tune = [&]( const uint32_t *&pm, size_t &n ) {
        if ( !pTune ) return;
        pTuned->vMsgs.clear();
        pTune->SendBatch( pm, n );
        pm = pTuned->vMsgs.data();
        n = pTuned->vMsgs.size();
    };
    {
        const uint32_t *pm = vStart.data();
        size_t n = vStart.size();
        Tune( pm, n );
        for ( size_t i = 0; i < n; i++ ) if ( Put( 0, pm[i] ) ) Count( pm[i], 1 );
    }

    // Repeat marks: a tick's notes (between its other events, which they must not pass) by channel and
    // key, in their order there; a run of identical ones is written once with its count. Notes of
    // different keys may change places (they never touch each other)
    std::vector< std::vector< std::pair< uint32_t, uint32_t > > > vSlot( bRepeat ? 2048 : 0 );
    std::vector< uint32_t > vOrder;
    auto PutTick = [&]( uint64_t llTick, const uint32_t *pm, size_t n ) {
        Tune( pm, n );
        auto IsNote = []( uint32_t m ) { const uint32_t s = m & 0xF0; return s == 0x80 || s == 0x90; };
        for ( size_t i = 0; i < n; )
        {
            if ( !bRepeat || !IsNote( pm[i] ) ) { if ( Put( llTick, pm[i] ) ) Count( pm[i], 1 ); i++; continue; }
            size_t j = i;
            for ( ; j < n && IsNote( pm[j] ); j++ )
            {
                // (a note-on of velocity 0 is an off: the same as a note-off of velocity 0)
                uint32_t m = pm[j];
                if ( ( m & 0xF0 ) == 0x90 && !( m & 0x7F0000 ) ) m = 0x80 | ( m & 0xFF0F );
                const uint32_t uSlot = ( m & 0xF ) << 7 | ( ( m >> 8 ) & 0x7F );
                auto &v = vSlot[uSlot];
                if ( v.empty() ) vOrder.push_back( uSlot );
                if ( !v.empty() && v.back().first == m ) v.back().second++;
                else v.push_back( { m, 1 } );
            }
            for ( uint32_t uSlot : vOrder )
            {
                for ( const auto &e : vSlot[uSlot] )
                {
                    if ( e.second > 1 ) vW[0].Repeat( llTick, ( e.first >> 8 ) & 0x7F, e.second );
                    Count( e.first, e.second );
                    vW[0].Msg( llTick, e.first );
                }
                vSlot[uSlot].clear();
            }
            vOrder.clear();
            i = j;
        }
    };

    PlaybackDecoder::Params p;
    p.dVolumeCorrect = snd.dVelocityCorrect;
    p.iMetronome = snd.iMetronome;
    p.iMinVelocity = snd.iMinVelocity;
    p.bCollapse = snd.bCollapse;
    p.llMaxSendsPerSec = snd.llMaxSendsPerSec;
    p.dSpeed = dSpeed;
    p.llGroupUs = 0;
    p.bTuning31 = snd.bTuning31;
    // The events go PostSecs past the piece's end (Merlin looks ahead), and every note still held there
    // is let go there: a piece's notes all end in it (Merlin will not render a note that never ends)
    const long long POST = (long long)std::llround( PostSecs * uRate );
    long long tEnd = (long long)std::ceil( F2S( (double)( FEnd + POST ) ) );
    // Where each block's end (and the fade after it) lies in the file, to cut a piece that grows too big
    std::vector< std::vector< size_t > > vCut( K, std::vector< size_t >( (size_t)llCount + 1, 0 ) );
    long long kNext = 1;
    for ( ;; )
    {
        if ( pCancel->load( std::memory_order_relaxed ) ) return false;
        long long llDue;
        size_t n = dec.Fill( vBuf.data(), vBuf.size(), tEnd, tp, p, llDue );
        if ( !n )
        {
            if ( llDue > tEnd || llDue == LLONG_MAX ) break;
            continue;
        }
        const double dFrame = S2F( (double)llDue );
        while ( kNext <= llCount && dFrame >= (double)( F0 + kNext * BF + XF ) )
        {
            for ( size_t i = 0; i < K; i++ ) vCut[i][(size_t)kNext] = vMidi[i].size();
            kNext++;
        }
        if ( kNext > 1 && Bytes() > PieceBytesMax )
        {
            llCount = kNext - 1;
            for ( size_t i = 0; i < K; i++ ) vMidi[i].resize( vCut[i][(size_t)llCount] );
            FEnd = ( llFirst + llCount ) * BF + XF;
            break;
        }
        if ( dFrame >= (double)FEnd && kNext <= llCount )
        {
            for ( size_t i = 0; i < K; i++ ) vCut[i][(size_t)kNext] = vMidi[i].size();
            kNext = llCount + 1;
        }
        const uint64_t llTick = (uint64_t)std::max( 0LL, std::llround( ( dFrame - Fp ) * dTicksPerFrame ) );
        const uint64_t llOns = st.llNoteOns;
        PutTick( llTick, vBuf.data(), n );
        if ( st.llNoteOns > llOns ) bSilent = false;
    }
    // The notes still held let go (cut short at a block's end, the counts are of what was read: an off
    // more than needed changes nothing), then the end
    const uint64_t llTickEnd = (uint64_t)std::llround( ( FEnd + POST - Fp ) * dTicksPerFrame );
    for ( uint32_t uSlot = 0; uSlot < 2048; uSlot++ )
    {
        uint32_t n = vHeld[uSlot];
        if ( !n ) continue;
        const uint32_t m = 0x80 | ( uSlot >> 7 ) | ( ( uSlot & 0x7F ) << 8 );
        if ( bRepeat )
        {
            if ( n > 1 ) vW[0].Repeat( llTickEnd, uSlot & 0x7F, n );
            vW[0].Msg( llTickEnd, m );
        }
        else for ( ; n; n-- ) Put( llTickEnd, m, true );
    }
    for ( SmfWriter &w : vW ) w.End( llTickEnd );
    for ( size_t i = 0; i < K; i++ ) if ( !vHasNotes[i] ) vMidi[i].clear();
    llSkip = (uint64_t)( F0 - Fp );
    llFrames = (uint64_t)( FEnd - F0 );
    return true;
}

//-----------------------------------------------------------------------------
// The player (AudioOut's thread)
//-----------------------------------------------------------------------------

void RenderAhead::Fill( float *pOut, unsigned n, long long llWallUs, unsigned uRate )
{
    // (offline: in order, at the cursor, with nothing faded)
    const bool bOffline = m_uOfflineRate != 0;
    const double dSpeed = std::max( m_Clock.Speed(), 1e-6 );   // (below the slowest playback speed, 1e-5)
    const bool bPaused = !bOffline && m_Clock.Paused();
    const unsigned uEpoch = m_Clock.Epoch();
    const long long L = uRate / 250;       // the limiter's look-ahead (4 ms)
    const long long XF = uRate / 200;
    // The PFA lag simulation drives the clock (a ramp): it holds through a freeze and races through what was
    // held, so the audio follows it frame by frame instead (below), never going back
    const bool bRamped = !bOffline && !bPaused && m_Clock.Ramped();
    const size_t N = (size_t)( n + L );

    std::vector< BlockPtr > vBlocks;
    long long llB0 = 0, BF = 0;
    unsigned uBase;
    double dWant;
    std::vector< double > vWant;        // (ramped: the clock's frame for each frame out)
    {
        std::lock_guard< std::mutex > lock( m_mx );
        if ( m_bHold )
        {
            std::fill( pOut, pOut + (size_t)n * 2, 0.0f );
            return;
        }
        if ( !bOffline && ( dSpeed != m_dSpeed || uRate != m_uRate ) )
        {
            if ( uRate != m_uRate ) m_uBankGen++;      // (Kestrel's soundfonts are loaded at the rate)
            NewTimebase( dSpeed, uRate );
        }
        uBase = m_uBase;
        if ( !bRamped ) m_bGrains = false;     // (started afresh when the ramp comes back)
        BF = BlockFrames();
        dWant = bOffline ? m_dCursor : SongToFrame( (double)m_Clock.At( llWallUs ) );
        if ( !bOffline && ( uBase != m_uCursorBase || uEpoch != m_uCursorEpoch || ( !bRamped && std::fabs( m_dCursor - dWant ) > 0.05 * uRate ) ) )
        {
            m_dCursor = dWant;
            m_uCursorBase = uBase;
            m_uCursorEpoch = uEpoch;
            m_fFade = 0.0f;
            m_bGrains = false;
        }
        double dFirst = m_dCursor, dLast = m_dCursor + ( n + L ) * 1.01 + 2;
        if ( bRamped )
        {
            // (the clock read every millisecond, in between in straight lines, as its knots are)
            vWant.resize( N );
            const size_t S = std::max< size_t >( 1, uRate / 1000 );
            double dA = dWant;
            for ( size_t i = 0; i < N; i += S )
            {
                const size_t j = std::min( N, i + S );
                const double dB = SongToFrame( (double)m_Clock.At( llWallUs + (long long)( j * 1e6 / uRate ) ) );
                for ( size_t k = i; k < j; k++ ) vWant[k] = dA + ( dB - dA ) * ( k - i ) / (double)( j - i );
                dA = dB;
            }
            // (where it may read: from the first of the clock and the grains, less the search, to the furthest and
            // a grain and the search, at most a minute; past that is silence)
            const double dG = 0.06 * uRate;
            dFirst = std::min( { vWant.front(), m_bGrains ? (double)std::min( m_llGrainPrev, m_llGrainCur ) : dWant } ) - dG;
            dLast = std::max( { vWant.back(), m_bGrains ? (double)std::max( m_llGrainPrev, m_llGrainCur ) : dWant } ) + N + 2 * dG;
            dLast = std::min( dLast, dFirst + 60.0 * uRate );
        }
        llB0 = FloorDiv( (long long)std::floor( dFirst ), BF ) - 1;
        const long long llB1 = FloorDiv( (long long)std::floor( dLast ), BF );
        for ( long long b = llB0; b <= llB1; b++ )
        {
            auto it = m_mBlocks.find( b );
            vBlocks.push_back( it == m_mBlocks.end() ? nullptr : it->second );
        }
    }

    // Catch up with the clock slowly (at most 0.3 %: unheard), or stand still while paused
    const double dStep = bPaused ? 0.0 : 1.0 + std::clamp( ( dWant - m_dCursor ) / ( 0.5 * uRate ), -0.003, 0.003 );
    bool bGap = false;
    auto Frame = [&]( long long f, float &l, float &r ) {
        const long long b = FloorDiv( f, BF ), o = f - b * BF;
        const size_t i = (size_t)( b - llB0 );
        const Block *pb = i < vBlocks.size() ? vBlocks[i].get() : nullptr;
        if ( !pb || pb->Spilled() ) { l = r = 0.0f; bGap = true; return; }
        l = pb->vAudio[(size_t)o * 2];
        r = pb->vAudio[(size_t)o * 2 + 1];
        if ( o < XF && pb->bPieceStart && i > 0 && vBlocks[i - 1] && !vBlocks[i - 1]->vOver.empty() )
        {
            const Block *pp = vBlocks[i - 1].get();
            const float w = ( o + 0.5f ) / XF;
            l = pp->vOver[(size_t)o * 2] * ( 1.0f - w ) + l * w;
            r = pp->vOver[(size_t)o * 2 + 1] * ( 1.0f - w ) + r * w;
        }
    };
    m_vMix.resize( N * 2 );
    uint64_t llGaps = 0;
    if ( bRamped )
    {
        // Following the clock, stretched at the music's own pitch (WSOLA): grains of 2H frames, a Hann window
        // each, one every H frames, each read from where the clock is as it starts, nudged by up to D so its
        // start matches the waveform the grain before it would go on with (so they join without beating). So
        // a slowed passage plays its notes at their pitches at the clock's pace, and a race through what was
        // held plays the music as the clock passes it, never going back. While the clock stands (a freeze)
        // the grains hold where they are and fade out (0.1 s), as held notes die away
        const long long H = std::max< long long >( 64, uRate / 40 ), D = uRate / 200, W = std::min< long long >( H, uRate / 100 );
        const float fDecay = std::exp( -(float)H / ( 0.1f * uRate ) );
        long long prev = m_llGrainPrev, cur = m_llGrainCur, phase = m_llGrainPhase;
        float g0 = m_fGrainGain0, g1 = m_fGrainGain1;
        double dHopWant = m_dGrainWant;
        if ( !m_bGrains )
        {
            cur = (long long)std::floor( vWant[0] );
            prev = cur - H;
            phase = 0;
            g0 = g1 = 1.0f;
            dHopWant = vWant[0];
        }
        auto Mono = [&]( long long f ) { float l, r; Frame( f, l, r ); return l + r; };
        std::vector< float > vSeek( (size_t)( 2 * D + W ) ), vNat( (size_t)W );
        for ( size_t i = 0; i < N; i++ )
        {
            if ( i == n )      // (the look-ahead's frames are not kept)
            {
                m_llGrainPrev = prev; m_llGrainCur = cur; m_llGrainPhase = phase;
                m_fGrainGain0 = g0; m_fGrainGain1 = g1; m_dGrainWant = dHopWant; m_bGrains = true;
                m_dCursor = (double)( cur + phase );
            }
            if ( phase >= H )
            {
                // The next grain: near the clock, where it best goes on from the grain before (normalised
                // correlation of the two, coarsely then finely)
                prev = cur;
                const long long p = (long long)std::floor( vWant[i] ), nat = prev + H;
                for ( long long j = 0; j < W; j++ ) vNat[(size_t)j] = Mono( nat + j );
                for ( long long j = 0; j < 2 * D + W; j++ ) vSeek[(size_t)j] = Mono( p - D + j );
                auto Score = [&]( long long d, long long st ) {
                    double c = 0.0, e = 1e-9;
                    for ( long long j = 0; j < W; j += st ) { const float x = vSeek[(size_t)( d + D + j )]; c += x * vNat[(size_t)j]; e += x * x; }
                    return c / std::sqrt( e );
                };
                long long dBest = 0;
                double sBest = -1e300;
                for ( long long d = -D; d <= D; d += 4 ) { const double sc = Score( d, 4 ); if ( sc > sBest ) { sBest = sc; dBest = d; } }
                const long long dCoarse = dBest;
                for ( long long d = std::max( -D, dCoarse - 3 ); d <= std::min( D, dCoarse + 3 ); d++ ) { const double sc = Score( d, 2 ); if ( d == std::max( -D, dCoarse - 3 ) || sc > sBest ) { sBest = sc; dBest = d; } }
                cur = p + dBest;
                // (standing: fading out; moving: back up within the hop)
                const bool bMoved = vWant[i] - dHopWant >= 1.0;
                g0 = g1;
                g1 = bMoved ? 1.0f : g1 * fDecay;
                dHopWant = vWant[i];
                phase = 0;
            }
            const float x = (float)phase / H;
            const float wCur = 0.5f - 0.5f * std::cos( 3.14159265f * x ), wPrev = 1.0f - wCur;
            float la, ra, lb, rb;
            bGap = false;
            Frame( prev + H + phase, la, ra );
            Frame( cur + phase, lb, rb );
            if ( bGap && i < n ) llGaps++;
            const float g = g0 + ( g1 - g0 ) * x;
            m_vMix[i * 2] = g * ( wPrev * la + wCur * lb );
            m_vMix[i * 2 + 1] = g * ( wPrev * ra + wCur * rb );
            phase++;
        }
        if ( N <= n )
        {
            m_llGrainPrev = prev; m_llGrainCur = cur; m_llGrainPhase = phase;
            m_fGrainGain0 = g0; m_fGrainGain1 = g1; m_dGrainWant = dHopWant; m_bGrains = true;
            m_dCursor = (double)( cur + phase );
        }
    }
    else for ( size_t i = 0; i < N; i++ )
    {
        const double pos = m_dCursor + i * ( dStep > 0.0 ? dStep : 1.0 );
        const long long f = (long long)std::floor( pos );
        const float fr = (float)( pos - f );
        float l0, r0, l1, r1;
        bGap = false;
        Frame( f, l0, r0 );
        if ( fr > 0.0f ) Frame( f + 1, l1, r1 );
        else { l1 = l0; r1 = r0; }
        if ( bGap && i < n ) llGaps++;
        m_vMix[i * 2] = l0 + ( l1 - l0 ) * fr;
        m_vMix[i * 2 + 1] = r0 + ( r1 - r0 ) * fr;
    }
    // Developer switch PFAVIZ_AUDIOCAP=file: the audio before gain, mute and limiter (raw float stereo),
    // and file.txt: per stretch the wall time, the frame the clock wants and the frame played
    static FILE *s_pCap = nullptr, *s_pCapLog = nullptr;
    static const char *s_sCap = getenv( "PFAVIZ_AUDIOCAP" );
    if ( s_sCap && !s_pCap && !bOffline )
    {
        s_pCap = fopen( s_sCap, "wb" );
        s_pCapLog = fopen( ( std::string( s_sCap ) + ".txt" ).c_str(), "w" );
    }
    if ( s_pCap && !bOffline )
    {
        fwrite( m_vMix.data(), sizeof( float ), (size_t)n * 2, s_pCap );
        fprintf( s_pCapLog, "%lld %.2f %.2f %u %d %u %llu\n", llWallUs, dWant, m_dCursor, n, bPaused ? 1 : 0, uRate, (unsigned long long)llGaps );
        fflush( s_pCapLog );
    }
    const bool bSound = !bPaused && !m_bMute.load( std::memory_order_relaxed );
    if ( bSound ) m_llGaps.fetch_add( llGaps, std::memory_order_relaxed );

    // Gain, the fade (pause, mute and jumps: 5 ms) and the limiter (4 ms look-ahead, 150 ms release)
    const float fGain = (float)m_dGain.load( std::memory_order_relaxed );
    const float fFadeStep = 1.0f / ( 0.005f * uRate );
    const bool bLimiter = m_bLimiter.load( std::memory_order_relaxed );
    const float fCeil = 0.98f;
    const float fRelease = 1.0f - std::exp( -1.0f / ( 0.15f * uRate ) );
    const float fAttack = 1.0f - std::exp( -4.0f / std::max< float >( 1.0f, (float)L ) );
    std::vector< float > vNeed;
    if ( bLimiter )
    {
        // The gain each frame needs, then the least of it over the look-ahead
        std::vector< float > vAt( N );
        for ( size_t i = 0; i < N; i++ )
        {
            const float a = std::max( std::fabs( m_vMix[i * 2] ), std::fabs( m_vMix[i * 2 + 1] ) ) * fGain;
            vAt[i] = a > fCeil ? fCeil / a : 1.0f;
        }
        vNeed.resize( n );
        for ( size_t i = 0; i < n; i++ )
        {
            float m = 1.0f;
            for ( size_t j = i; j <= i + (size_t)L && j < N; j++ ) m = std::min( m, vAt[j] );
            vNeed[i] = m;
        }
    }
    for ( size_t i = 0; i < n; i++ )
    {
        m_fFade = bSound ? std::min( 1.0f, m_fFade + fFadeStep ) : std::max( 0.0f, m_fFade - fFadeStep );
        float g = fGain * m_fFade;
        if ( bLimiter )
        {
            const float need = vNeed[i];
            m_fLimGain += ( need < m_fLimGain ? fAttack : fRelease ) * ( need - m_fLimGain );
            m_fLimGain = std::min( m_fLimGain, 1.0f );
            g *= m_fLimGain;
        }
        float l = m_vMix[i * 2] * g, r = m_vMix[i * 2 + 1] * g;
        if ( bLimiter ) { l = std::clamp( l, -fCeil, fCeil ); r = std::clamp( r, -fCeil, fCeil ); }
        pOut[i * 2] = l;
        pOut[i * 2 + 1] = r;
    }
    if ( dStep > 0.0 && !bRamped ) m_dCursor += n * dStep;
}

//-----------------------------------------------------------------------------
// Holding, offline rendering
//-----------------------------------------------------------------------------

void RenderAhead::Hold( bool bHold )
{
    std::unique_lock< std::mutex > lock( m_mx );
    if ( bHold == m_bHold ) return;
    m_bHold = bHold;
    if ( bHold )
    {
        m_mBlocks.clear();
        m_sNoRedo.clear();
        for ( Claim &c : m_vClaims ) c.pCancel->store( true );
    }
    m_cv.notify_all();
    while ( bHold && !m_vClaims.empty() ) m_cv.wait_for( lock, std::chrono::milliseconds( 20 ) );
}

long long RenderAhead::OfflineFrames( long long llFromUs, long long llToUs ) const
{
    auto F = [&]( long long t ) { return (long long)std::floor( (double)t * m_uOfflineRate / 1e6 ); };
    return std::max( 0LL, F( llToUs ) - F( llFromUs ) );
}

bool RenderAhead::RenderOffline( long long llFromUs, long long llToUs, const std::function< bool( const float *, unsigned ) > &fnWrite,
                                 std::atomic< long long > *pDone, const std::atomic< long long > *pUntil, const std::atomic< bool > *pCancel,
                                 std::wstring &sError )
{
    const unsigned uRate = m_uOfflineRate;
    if ( !uRate ) { sError = L"not made offline"; return false; }
    const long long F0 = (long long)std::floor( (double)llFromUs * uRate / 1e6 ), llTotal = OfflineFrames( llFromUs, llToUs );
    const long long L = uRate / 250;
    {
        std::lock_guard< std::mutex > lock( m_mx );
        m_dCursor = (double)F0;
        m_llOfflineLast = FloorDiv( F0 + llTotal + L + 2, BlockFrames() );
        m_fFade = 1.0f;
        m_fLimGain = 1.0f;
    }
    const unsigned uChunk = uRate / 10;
    std::vector< float > v( (size_t)uChunk * 2 );
    long long llDone = 0;
    bool bLoaded = false;
    auto Stopped = [&]() { return pCancel && pCancel->load(); };
    for ( ;; )
    {
        const long long llWant = pUntil ? std::min( llTotal, pUntil->load() ) : llTotal;
        if ( llDone >= llWant ) break;
        if ( Stopped() ) { sError = L"stopped"; return false; }
        const unsigned n = (unsigned)std::min< long long >( uChunk, llWant - llDone );
        {
            // The workers make the blocks from this one on; every block these frames use is waited for
            std::unique_lock< std::mutex > lock( m_mx );
            const long long BF = BlockFrames(), f = F0 + llDone;
            m_Clock.Set( (long long)std::floor( FrameToSong( (double)f ) ) );
            m_cv.notify_all();
            const long long llEndBlock = FloorDiv( (long long)SongToFrame( (double)m_llEndUs ), BF );
            const long long b0 = FloorDiv( f, BF ), b1 = std::min( FloorDiv( f + n + L + 2, BF ), llEndBlock );
            auto Ready = [&]() {
                for ( long long b = b0; b <= b1; b++ )
                {
                    auto it = m_mBlocks.find( b );
                    if ( it == m_mBlocks.end() || it->second->uGen != m_uGen || it->second->Spilled() ) return false;
                }
                return true;
            };
            while ( !Ready() )
            {
                if ( m_bStop || Stopped() ) { sError = L"stopped"; return false; }
                if ( !m_pBank && m_uLoadedGen == m_uBankGen && !m_sState.empty() ) { sError = m_sState; return false; }
                if ( m_pBank && !bLoaded ) { bLoaded = true; if ( m_fnLoaded ) m_fnLoaded(); }
                // (a whole render: how far its piece is, as frames done, so its progress shows as it is made)
                if ( pDone && m_llWholeFirst != LLONG_MIN )
                {
                    const double dAt = ( (double)m_llWholeFirst + m_llWholeCount * (double)m_fWholeProgress.load() ) * BF - (double)F0;
                    const long long llAt = std::clamp< long long >( (long long)dAt, llDone, std::max( llDone, llWant - 1 ) );
                    if ( llAt > pDone->load() ) pDone->store( llAt );
                    static long long s_llLogged = 0;
                    if ( FILE *fl = RenderLog() )
                        if ( Timer::NowMicroSecs() - s_llLogged > 1000000 )
                        {
                            s_llLogged = Timer::NowMicroSecs();
                            fprintf( fl, "P %lld %.4f\n", s_llLogged / 1000, m_fWholeProgress.load() );
                            fflush( fl );
                        }
                }
                m_cv.wait_for( lock, std::chrono::milliseconds( 50 ) );
            }
        }
        if ( !bLoaded ) { bLoaded = true; if ( m_fnLoaded ) m_fnLoaded(); }
        Fill( v.data(), n, 0, uRate );
        if ( !fnWrite( v.data(), n ) ) { sError = L"the audio could not be written"; return false; }
        llDone += n;
        if ( pDone && llDone > pDone->load() ) pDone->store( llDone );
    }
    return true;
}

//-----------------------------------------------------------------------------
// Blocks on disk
//-----------------------------------------------------------------------------

RenderAhead::SpillFile::~SpillFile()
{
    if ( m_hFile != INVALID_HANDLE_VALUE ) CloseHandle( m_hFile );     // (deleted on close)
}

bool RenderAhead::SpillFile::Open()
{
    if ( m_hFile != INVALID_HANDLE_VALUE ) return true;
    wchar_t sDir[MAX_PATH + 1] = {}, sFile[MAX_PATH + 1] = {};
    if ( !GetTempPathW( MAX_PATH + 1, sDir ) || !GetTempFileNameW( sDir, L"PFA", 0, sFile ) ) return false;
    m_hFile = CreateFileW( sFile, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr );
    return m_hFile != INVALID_HANDLE_VALUE;
}

namespace
{
    bool FileIo( HANDLE h, bool bWrite, float *p, size_t n, unsigned long long llAt )
    {
        if ( !n ) return true;
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)llAt;
        ov.OffsetHigh = (DWORD)( llAt >> 32 );
        DWORD dw = 0;
        const DWORD dwWant = (DWORD)( n * sizeof( float ) );
        return ( bWrite ? WriteFile( h, p, dwWant, &dw, &ov ) : ReadFile( h, p, dwWant, &dw, &ov ) ) && dw == dwWant;
    }
}

long long RenderAhead::SpillFile::Write( const float *pAudio, size_t nAudio, const float *pOver, size_t nOver )
{
    if ( nAudio + nOver > m_nSlotFloats ) return -1;
    std::lock_guard< std::mutex > lock( m_mx );
    if ( !Open() ) return -1;
    long long llSlot;
    if ( !m_vFree.empty() ) { llSlot = m_vFree.back(); m_vFree.pop_back(); }
    else llSlot = m_llNext++;
    const unsigned long long llAt = (unsigned long long)llSlot * m_nSlotFloats * sizeof( float );
    if ( !FileIo( m_hFile, true, const_cast< float * >( pAudio ), nAudio, llAt ) ||
         !FileIo( m_hFile, true, const_cast< float * >( pOver ), nOver, llAt + nAudio * sizeof( float ) ) )
    {
        m_vFree.push_back( llSlot );
        return -1;
    }
    return llSlot;
}

bool RenderAhead::SpillFile::Read( long long llSlot, float *pAudio, size_t nAudio, float *pOver, size_t nOver )
{
    std::lock_guard< std::mutex > lock( m_mx );
    if ( m_hFile == INVALID_HANDLE_VALUE ) return false;
    const unsigned long long llAt = (unsigned long long)llSlot * m_nSlotFloats * sizeof( float );
    return FileIo( m_hFile, false, pAudio, nAudio, llAt ) && FileIo( m_hFile, false, pOver, nOver, llAt + nAudio * sizeof( float ) );
}

void RenderAhead::SpillFile::Free( long long llSlot )
{
    std::lock_guard< std::mutex > lock( m_mx );
    m_vFree.push_back( llSlot );
}

void RenderAhead::PagerThread()
{
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
    // Developer switch PFAVIZ_RAMSECS=n: n seconds ahead kept in memory (and at most a minute behind)
    const int iRamSecs = getenv( "PFAVIZ_RAMSECS" ) ? std::max( 2, atoi( getenv( "PFAVIZ_RAMSECS" ) ) ) : 0;
    std::unique_lock< std::mutex > lock( m_mx );
    bool bBusy = false;
    while ( !m_bStop )
    {
        if ( !bBusy ) m_cv.wait_for( lock, std::chrono::milliseconds( 100 ) );
        bBusy = false;
        if ( m_bStop || m_bHold || !m_pSpill ) continue;
        long long llAhead = std::min( RamAheadSecs, m_Settings.iAheadSecs ) + RunBlocks, llBehind = std::min( RamBehindSecs, std::max( 2, m_Settings.iBehindSecs ) );
        if ( iRamSecs ) { llAhead = iRamSecs; llBehind = std::min( llBehind, (long long)iRamSecs ); }
        const long long P = PlayBlock(), lo = P - llBehind, hi = P + llAhead;
        auto Meta = []( const Block &b ) {
            auto p = std::make_shared< Block >();
            p->uGen = b.uGen;
            p->bDegraded = b.bDegraded;
            p->iDegradeVel = b.iDegradeVel;
            p->uDegradeCap = b.uDegradeCap;
            p->bPieceStart = b.bPieceStart;
            p->pSlot = b.pSlot;
            p->nAudio = b.Spilled() ? b.nAudio : b.vAudio.size();
            p->nOver = b.Spilled() ? b.nOver : b.vOver.size();
            return p;
        };

        // Back from the disk: the nearest first, from the playback position on, then behind it
        long long bIn = 0;
        BlockPtr pIn;
        auto itP = m_mBlocks.lower_bound( P );
        for ( auto it = itP; it != m_mBlocks.end() && it->first <= hi && !pIn; ++it )
            if ( it->second->Spilled() ) { pIn = it->second; bIn = it->first; }
        for ( auto it = std::make_reverse_iterator( itP ); it != m_mBlocks.rend() && it->first >= lo && !pIn; ++it )
            if ( it->second->Spilled() ) { pIn = it->second; bIn = it->first; }
        if ( pIn )
        {
            lock.unlock();
            std::shared_ptr< Block > p = Meta( *pIn );
            p->vAudio.resize( p->nAudio );
            p->vOver.resize( p->nOver );
            const bool bOk = p->nAudio && pIn->pSlot && pIn->pSlot->pFile->Read( pIn->pSlot->llSlot, p->vAudio.data(), p->nAudio, p->vOver.data(), p->nOver );
            lock.lock();
            auto it = m_mBlocks.find( bIn );
            if ( it != m_mBlocks.end() && it->second == pIn )
            {
                if ( bOk ) it->second = p;
                else m_mBlocks.erase( it );     // (made again)
            }
            bBusy = true;
            continue;
        }

        // To the disk: once more than the blocks around the playback position are in memory, the
        // furthest from them
        size_t nRam = 0;
        for ( const auto &kv : m_mBlocks ) nRam += !kv.second->Spilled();
        if ( nRam <= (size_t)( llAhead + llBehind ) ) continue;
        long long bOut = 0, dOut = 0;
        BlockPtr pOut;
        for ( auto it = m_mBlocks.begin(); it != m_mBlocks.end() && it->first < lo; ++it )
            if ( !it->second->Spilled() ) { pOut = it->second; bOut = it->first; dOut = lo - it->first; break; }
        for ( auto it = m_mBlocks.rbegin(); it != m_mBlocks.rend() && it->first > hi; ++it )
            if ( !it->second->Spilled() )
            {
                if ( it->first - hi > dOut ) { pOut = it->second; bOut = it->first; }
                break;
            }
        if ( !pOut ) continue;
        std::shared_ptr< SpillFile > pFile = m_pSpill;
        lock.unlock();
        std::shared_ptr< Block > p = Meta( *pOut );
        if ( !p->pSlot )
        {
            const long long llSlot = pFile->Write( pOut->vAudio.data(), pOut->vAudio.size(), pOut->vOver.data(), pOut->vOver.size() );
            if ( llSlot >= 0 )
            {
                auto pSlot = std::make_shared< Slot >();
                pSlot->pFile = pFile;
                pSlot->llSlot = llSlot;
                p->pSlot = pSlot;
            }
        }
        lock.lock();
        auto it = m_mBlocks.find( bOut );
        if ( it != m_mBlocks.end() && it->second == pOut )
        {
            if ( p->pSlot ) it->second = p;
            else m_mBlocks.erase( it );         // (the disk is full: made again when wanted)
        }
        bBusy = true;
    }
}

bool RenderAhead::StreamBlock( StreamRun &run, long long b, std::unique_lock< std::mutex > &lock, const RenderBackends::PieceOpts &opts )
{
    std::shared_ptr< RenderBackends::Bank > pBank = m_pBank;
    const bool bRestart = !run.p || run.uGen != m_uGen || run.uBase != m_uBase || run.pBank != pBank || run.llNext != b;
    Claim claim{ b, 1, m_uGen, std::make_shared< std::atomic< bool > >( false ) };
    m_vClaims.push_back( claim );
    auto Unclaim = [&]() {
        for ( size_t i = 0; i < m_vClaims.size(); i++ )
            if ( m_vClaims[i].pCancel == claim.pCancel ) { m_vClaims.erase( m_vClaims.begin() + i ); break; }
    };
    const Sound snd = m_Sound;
    const unsigned uGen = m_uGen, uBase = m_uBase;
    const double dSpeed = m_dSpeed;
    const unsigned uRate = m_uRate;
    const bool bDegrade = m_Settings.bAutoDegrade && !m_Clock.Paused();
    const bool bLossless = m_Settings.bLossless;
    // (next to the playback position: no time to start back at a held note's start)
    const bool bUrgent = !m_uOfflineRate && b - PlayBlock() < 3;
    const bool bKeepCap = bDegrade && run.uGen == m_uGen && run.uMax == std::max( 1u, opts.uVoices );
    lock.unlock();
    const long long llT0 = Timer::NowMicroSecs();

    const long long BF = uRate, XF = uRate / 200, PRE = (long long)std::llround( PrerollSecs * uRate );
    const long long KB = std::clamp( opts.kb.iBlockFrames, 64, 16384 );     // (pushed this far past what is made)
    auto F2S = [&]( double f ) { return f * 1e6 * dSpeed / uRate; };
    auto S2F = [&]( double t ) { return t * uRate / ( 1e6 * dSpeed ); };
    std::wstring sError;
    bool bOk = true;

    // Decodes to llFrame (stream frames), and StreamLookSecs past it while the queue is not too long
    auto Decode = [&]( long long llFrame ) {
        const long long tNeed = (long long)std::ceil( F2S( (double)( run.llFp + llFrame ) ) );
        const long long tTo = tNeed + (long long)( StreamLookSecs * 1e6 * dSpeed );
        while ( !run.bDecoded && run.tDecoded < tTo && ( run.tDecoded < tNeed || run.q.size() < ( 4u << 20 ) ) )
        {
            long long llDue;
            size_t n = run.pDec->Fill( run.vBuf.data(), run.vBuf.size(), tTo, run.tp, run.params, llDue );
            if ( !n )
            {
                if ( llDue == LLONG_MAX ) { run.bDecoded = true; break; }
                if ( llDue > tTo ) { run.tDecoded = tTo; break; }
                continue;
            }
            run.tDecoded = llDue;
            const uint32_t *pm = run.vBuf.data();
            if ( run.pTune )
            {
                run.pTuned->vMsgs.clear();
                run.pTune->SendBatch( pm, n );
                pm = run.pTuned->vMsgs.data();
                n = run.pTuned->vMsgs.size();
            }
            const uint64_t llF = (uint64_t)std::max( 0LL, std::llround( S2F( (double)llDue ) ) - run.llFp );
            for ( size_t i = 0; i < n; i++ ) run.Append( llF, pm[i] );
        }
    };
    auto Render = [&]( float *pOut, long long llFrames ) {
        // (in steps, each with everything up to a Kestrel block past it pushed)
        while ( bOk && llFrames > 0 )
        {
            if ( claim.pCancel->load() ) { bOk = false; break; }
            const long long n = std::min< long long >( llFrames, BF );
            const long long llEnd = run.llRendered + n + KB;
            Decode( llEnd );
            run.Push( llEnd );
            bOk = run.p->Render( pOut, (unsigned)n, sError );
            run.llRendered += n;
            pOut += n * 2;
            llFrames -= n;
        }
    };

    if ( bRestart )
    {
        // Started at the block, as a piece would be: its preroll (back to a note still held), the chase,
        // the held notes struck again
        run.p.reset();
        run.pBank = pBank;
        run.p = pBank->OpenStream( opts, sError );
        if ( !run.p )
        {
            lock.lock();
            Unclaim();
            run.bUnavailable = true;
            m_sRenderError = L"Kestrel's continuous render could not start (" + sError + L"): pieces instead";
            return false;
        }
        run.uGen = uGen;
        run.uBase = uBase;
        run.dSpeed = dSpeed;
        run.uRate = uRate;
        run.bLossless = bLossless;
        run.Reset();
        long long Fp = b * BF - PRE;
        const long long tp0 = (long long)std::floor( F2S( (double)Fp ) );
        const long long tHeld = bUrgent ? tp0 : HeldEarliest( tp0, tp0 - (long long)( HeldSecs * 1e6 * dSpeed ) );
        if ( tHeld < tp0 ) Fp = std::min( Fp, (long long)std::floor( S2F( (double)tHeld ) ) - 1 );
        run.llFp = Fp;
        run.tp = (long long)std::ceil( F2S( (double)Fp ) );
        run.pDec = std::make_unique< PlaybackDecoder >( *m_pSong );
        run.pDec->SetMuteMask( m_pMuteMask );
        run.pDec->SetBendChase( snd.bTuning31 && snd.bTuningBend );
        std::vector< uint32_t > vStart;
        run.pDec->Seek( run.tp, run.tp, vStart );
        HeldNotes( run.tp, vStart, snd.dVelocityCorrect );
        run.params = PlaybackDecoder::Params();
        run.params.dVolumeCorrect = snd.dVelocityCorrect;
        run.params.iMetronome = snd.iMetronome;
        run.params.iMinVelocity = snd.iMinVelocity;
        run.params.bCollapse = snd.bCollapse;
        run.params.llMaxSendsPerSec = snd.llMaxSendsPerSec;
        run.params.dSpeed = dSpeed;
        run.params.llGroupUs = 0;
        run.params.bTuning31 = snd.bTuning31;
        run.tDecoded = run.tp;
        run.bDecoded = false;
        run.pTune.reset();
        run.pTuned = nullptr;
        if ( snd.bTuning31 )
        {
            auto pOut = std::make_unique< PieceOut >();
            run.pTuned = pOut.get();
            if ( snd.bTuningBend ) run.pTune = std::make_unique< PitchBendTunedOutput >( std::move( pOut ) );
            else
            {
                auto pScale = std::make_unique< ScaleTunedOutput >( std::move( pOut ) );
                pScale->Init();
                run.pTune = std::move( pScale );
            }
            for ( const auto &v : run.pTuned->vSysEx ) run.p->SysEx( 0, v.data(), v.size() );
            run.pTuned->vMsgs.clear();
            run.pTune->SendBatch( vStart.data(), vStart.size() );
            vStart = run.pTuned->vMsgs;
        }
        for ( uint32_t m : vStart ) run.Append( 0, m );
        run.uMax = std::max( 1u, opts.uVoices );
        if ( !bKeepCap ) run.uCap = run.uMax;
        run.uCap = std::min( run.uCap, run.uMax );
        if ( run.uCap < run.uMax ) run.p->SetCap( run.uCap );
        // The preroll, thrown away, then the block's first fade frames
        std::vector< float > vSkip( (size_t)BF * 2 );
        for ( long long left = b * BF - Fp; bOk && left > 0; )
        {
            const long long n = std::min( left, BF );
            Render( vSkip.data(), n );
            left -= n;
        }
        run.vCarry.assign( (size_t)XF * 2, 0.0f );
        Render( run.vCarry.data(), XF );
    }

    // The block: what was made of it already, then the rest and the next one's first frames (its overrun)
    auto pBlock = std::make_shared< Block >();
    std::vector< float > vNew( (size_t)BF * 2 );
    if ( bOk ) Render( vNew.data(), BF );
    const long long llBusyUs = Timer::NowMicroSecs() - llT0;
    if ( bOk )
    {
        // (the average a second takes, a start's preroll counted)
        const double dSecs = (double)( bRestart ? b * BF - run.llFp + BF + XF : BF ) / BF;
        run.dSecUs += 0.3 * ( llBusyUs / dSecs - run.dSecUs );
        pBlock->uGen = uGen;
        pBlock->bPieceStart = bRestart;
        pBlock->vAudio.assign( run.vCarry.begin(), run.vCarry.end() );
        pBlock->vAudio.insert( pBlock->vAudio.end(), vNew.begin(), vNew.begin() + ( BF - XF ) * 2 );
        pBlock->vOver.assign( vNew.begin() + ( BF - XF ) * 2, vNew.end() );
        run.vCarry = pBlock->vOver;
        pBlock->bDegraded = run.uCap < run.uMax;
        pBlock->uDegradeCap = pBlock->bDegraded ? run.uCap : 0;
        run.llNext = b + 1;
    }
    else run.p.reset();
    FILE *s_pLog = RenderLog();
    if ( s_pLog )
    {
        fprintf( s_pLog, "%lld %lld 1 %lld %.1f 0 %d %d 0 %llu %d 0 -1 -1 -1 stream%s cap %u merged %llu%ls\n", llT0 / 1000, b, PlayBlock(), llBusyUs / 1000.0, bOk,
                 claim.pCancel->load() ? 1 : 0, (unsigned long long)run.q.size(), pBlock->bDegraded, bRestart ? " start" : "", run.uCap, (unsigned long long)run.llMerged, sError.c_str() );
        fflush( s_pLog );
    }

    lock.lock();
    Unclaim();
    if ( !bOk && !claim.pCancel->load() ) m_sRenderError = sError.empty() ? std::wstring( L"Kestrel's continuous render failed" ) : sError;
    else if ( bOk ) m_sRenderError.clear();
    if ( bOk && !claim.pCancel->load() && uBase == m_uBase )
    {
        auto it = m_mBlocks.find( b );
        if ( it == m_mBlocks.end() || it->second->uGen < uGen || ( it->second->uGen == uGen && ( !pBlock->bDegraded || it->second->bDegraded ) ) )
            m_mBlocks[b] = pBlock;
        Evict( PlayBlock() );
    }
    m_cv.notify_all();

    // Auto-degrade: a block that takes longer than it plays while little is ready lowers the voice cap
    // (Kestrel steals down to it), by how much too long; once well ahead it rises again
    if ( bOk && run.p && bDegrade )
    {
        const long long Pn = PlayBlock(), nReady = ReadyBlocks( Pn );
        const double dBlockUs = 1e6;
        unsigned uCap = run.uCap;
        if ( nReady < 3 && llBusyUs > 0.8 * dBlockUs )
            uCap = (unsigned)std::max< double >( 128.0, uCap * std::clamp( 0.7 * dBlockUs / (double)llBusyUs, 0.25, 0.85 ) );
        else if ( nReady >= 5 && uCap < run.uMax )
            uCap = (unsigned)std::min< double >( run.uMax, uCap * ( nReady >= 10 ? 2.0 : 1.25 ) + 1 );
        if ( uCap != run.uCap )
        {
            if ( uCap < run.uCap ) m_llDegraded.fetch_add( 1, std::memory_order_relaxed );
            run.uCap = uCap;
            run.p->SetCap( uCap );
        }
    }
    else if ( bOk && run.p && run.uCap != run.uMax )
    {
        run.uCap = run.uMax;
        run.p->SetCap( run.uCap );
    }

    // Sharing the graphics card with the picture, as between pieces
    if ( bOk && m_Settings.iGpuShare < 100 )
    {
        const long long llRestUs = std::min< long long >( 3000000, llBusyUs * ( 100 - m_Settings.iGpuShare ) / std::max( 1, m_Settings.iGpuShare ) );
        const long long llUntil = Timer::NowMicroSecs() + llRestUs;
        const unsigned uGenNow = m_uGen, uBaseNow = m_uBase;
        auto Calm = [&]() { return ReadyBlocks( PlayBlock() ) >= std::min< long long >( Settings::GpuCalmSecs, m_Settings.iAheadSecs / 2 ); };
        while ( !m_bStop && !m_bHold && m_uGen == uGenNow && m_uBase == uBaseNow && Calm() )
        {
            const long long llLeft = llUntil - Timer::NowMicroSecs();
            if ( llLeft <= 0 ) break;
            m_cv.wait_for( lock, std::chrono::microseconds( std::min< long long >( llLeft, 50000 ) ) );
        }
    }
    return true;
}
