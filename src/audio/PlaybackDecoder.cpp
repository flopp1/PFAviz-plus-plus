#include "audio/PlaybackDecoder.h"

#include <algorithm>
#include <cstring>

PlaybackDecoder::PlaybackDecoder( const Song &song ) : m_Song( song ), m_Stream( song.stream )
{
    m_Cursor.Attach( &m_Stream );
    m_Tempo.Build( song.vMeta, song.info.iDivision );
    for ( const RefMetaEvent &m : song.vMeta )
        if ( m.cType == RefMetaEvent::TimeSignature && m.iDataLen == 4 )
            m_vSignatures.push_back( { m.llTick, m.aData[0], 1LL << std::min< int >( m.aData[1], 30 ) } );
    m_bMetronome = !( song.info.iDivision & 0x8000 ) && song.info.iDivision > 0;
    m_Scout.Attach( &m_Stream );
    ResetSendState();
}

// Everything sounding was stopped (start, seek): the skip, collapse and budget state start over
void PlaybackDecoder::ResetSendState()
{
    memset( m_aPendingSkip, 0, sizeof( m_aPendingSkip ) );
    memset( m_aHeld, 0, sizeof( m_aHeld ) );
    memset( m_aVoices, 0, sizeof( m_aVoices ) );
    std::fill( m_aVoiceTick, m_aVoiceTick + 2048, SIZE_MAX );
    m_bScout = false;
    m_bAtTickStart = true;
    m_llTokenTime = LLONG_MIN;
    m_Cut = BudgetCut();
    m_dKeepAcc = 0.0;
}

//-----------------------------------------------------------------------------
// Positioning
//-----------------------------------------------------------------------------

void PlaybackDecoder::Reset( long long llStartTime )
{
    m_Cursor.Seek( 0 );
    m_uCachedTick = SIZE_MAX;
    m_iLastMetronomeNote = 76;
    SetBeat( GetBeatTick( CurrentTick( llStartTime ) ) );
    ResetSendState();
    m_iSkipVelocity = 0;
}

void PlaybackDecoder::Seek( long long llTarget, long long llPrevTime, std::vector< uint32_t > &vOut )
{
    // JumpTo starts with AllNotesOff
    for ( uint32_t ch = 0; ch < 16; ch++ ) vOut.push_back( 0xB0 | ch | ( 0x7B << 8 ) );
    for ( uint32_t ch = 0; ch < 16; ch++ ) vOut.push_back( 0xB0 | ch | ( 0x40 << 8 ) );

    // The oracle's start position: the first event with time >= target
    m_Cursor.Seek( llTarget <= 0 ? 0 : m_Stream.times.LowerBound( (uint64_t)llTarget ) );
    m_uCachedTick = SIZE_MAX;

    // PlaySkippedEvents
    const std::vector< ChaseEvent > &vChase = m_Song.vChase;
    auto UpperBound = [&]( long long llTime ) {
        return (size_t)( std::upper_bound( vChase.begin(), vChase.end(), llTime,
                                           []( long long t, const ChaseEvent &e ) { return t < e.llTime; } ) - vChase.begin() );
    };
    size_t iNew = UpperBound( llTarget ), iOld = UpperBound( llPrevTime );
    if ( iOld != iNew )
    {
        bool aControl[16][128], aProgram[16];
        memset( aControl, 0, sizeof( aControl ) );
        memset( aProgram, 0, sizeof( aProgram ) );
        std::vector< uint32_t > vControl;
        size_t iEnd = iOld < iNew ? iOld : 0;
        for ( size_t i = iNew; i-- > iEnd; )
        {
            uint32_t uMsg = vChase[i].uMsg, ch = uMsg & 0x0F;
            if ( ( uMsg & 0xF0 ) == 0xB0 )
            {
                uint32_t cc = ( uMsg >> 8 ) & 0x7F;
                if ( !aControl[ch][cc] ) { aControl[ch][cc] = true; vControl.push_back( uMsg ); }
            }
            else if ( !aProgram[ch] )
            {
                aProgram[ch] = true;
                vOut.push_back( uMsg );
            }
        }
        for ( auto it = vControl.rbegin(); it != vControl.rend(); ++it ) vOut.push_back( *it );
    }

    // AdvanceIterators( llStartTime, true ): the next beat at or after the current tick
    SetBeat( GetBeatTick( CurrentTick( llTarget ) ) );

    // Everything sounding was stopped, so the skip balance starts over
    ResetSendState();
}

//-----------------------------------------------------------------------------
// Metronome (MainScreen::PlayMetronome and the MeterTracker helpers, 64-bit)
//-----------------------------------------------------------------------------

const PlaybackDecoder::Signature &PlaybackDecoder::SignatureAt( long long llTick ) const
{
    static const Signature sDefault = { 0, 4, 4 };
    auto it = std::upper_bound( m_vSignatures.begin(), m_vSignatures.end(), llTick,
                                []( long long t, const Signature &s ) { return t < s.llTick; } );
    return it == m_vSignatures.begin() ? sDefault : *( it - 1 );
}

// Rounds up to the nearest beat
long long PlaybackDecoder::GetBeat( long long iTick, const Signature &s ) const
{
    long long iDivision = m_Song.info.iDivision;
    long long iTickOffset = iTick - s.llTick;
    if ( iTickOffset > 0 )
        return ( iTickOffset * s.iBeatType - 1 ) / ( iDivision * 4LL ) + 1;
    return ( iTickOffset * s.iBeatType ) / ( iDivision * 4LL );
}

long long PlaybackDecoder::GetBeatTick( long long iTick ) const
{
    if ( !m_bMetronome ) return LLONG_MIN;
    const Signature &s = SignatureAt( iTick );
    long long iDivision = m_Song.info.iDivision;
    return s.llTick + ( GetBeat( iTick, s ) * iDivision * 4 ) / s.iBeatType;
}

// MeterTracker::GetCurrentTick with the tempo in effect at llTime
long long PlaybackDecoder::CurrentTick( long long llTime ) const
{
    if ( !m_bMetronome ) return -1;
    const std::vector< TempoMap::Seg > &v = m_Tempo.vSegs;
    auto it = std::upper_bound( v.begin(), v.end(), llTime, []( long long t, const TempoMap::Seg &s ) { return t < s.llMicroSec; } );
    const TempoMap::Seg &s = it == v.begin() ? v.front() : *( it - 1 );
    long long iDivision = m_Song.info.iDivision;
    if ( s.llMicroSecsPerBeat <= 0 ) return -1;
    if ( llTime >= s.llMicroSec )
        return s.llTick + ( iDivision * ( llTime - s.llMicroSec ) ) / s.llMicroSecsPerBeat;
    return s.llTick - ( ( iDivision * ( s.llMicroSec - llTime ) + 1 ) / s.llMicroSecsPerBeat ) - 1;
}

void PlaybackDecoder::SetBeat( long long llBeatTick )
{
    m_llNextBeatTick = llBeatTick;
    m_llNextBeatTime = m_bMetronome && llBeatTick != LLONG_MIN ? m_Tempo.At( llBeatTick ) : LLONG_MAX;
}

//-----------------------------------------------------------------------------
// Decoding
//-----------------------------------------------------------------------------

long long PlaybackDecoder::EventTime()
{
    size_t u = m_Cursor.NextTick();
    if ( u != m_uCachedTick )
    {
        m_uCachedTick = u;
        m_llCachedTime = u < m_Stream.UniqueTicks() ? (long long)m_Stream.times.Get( u ) : LLONG_MAX;
    }
    return m_llCachedTime;
}

long long PlaybackDecoder::NextTime( int iMetronome )
{
    long long t = EventTime();
    return iMetronome != MetOff ? std::min( t, m_llNextBeatTime ) : t;
}

// Note-ons below this velocity are skipped at a lag of llLagUs: none up to llSkipLagUs, then a
// threshold rising linearly from 1 to 128 (every note) at 4 x llSkipLagUs, so the quietest notes go
// first and the loud ones last (DESIGN.md 7.5)
int PlaybackDecoder::VelocityThreshold( long long llLagUs, long long llSkipLagUs )
{
    llSkipLagUs = std::max< long long >( 1, llSkipLagUs );
    if ( llLagUs <= llSkipLagUs ) return 0;
    long long v = 1 + 127 * ( llLagUs - llSkipLagUs ) / ( 3 * llSkipLagUs );
    return (int)std::min< long long >( 128, v );
}

// Keeps the loudest note-ons whose count fits dAllowance; the velocity where it runs out is kept at
// the rate that fills it exactly (a file with one velocity is thinned evenly instead of all or nothing)
PlaybackDecoder::BudgetCut PlaybackDecoder::BudgetThreshold( const uint64_t aWindow[128], double dAllowance )
{
    BudgetCut c;
    double dSum = 0.0;
    for ( int v = 127; v >= 1; v-- )
    {
        if ( dSum + (double)aWindow[v] <= dAllowance ) { dSum += (double)aWindow[v]; continue; }
        c.iVelocity = v;
        c.dKeep = std::max( 0.0, dAllowance - dSum ) / (double)aWindow[v];
        break;
    }
    return c;
}

// At the start of each tick: the scout counts up to BudgetWindowUs ahead, the bucket refills, and
// the cut is chosen so the window's note-ons (2 messages each, with their offs) fit what the bucket
// holds plus what it gains over the window
void PlaybackDecoder::BudgetTick( long long tE, const Params &p )
{
    double dRate = (double)p.llMaxSendsPerSec, dDepth = BudgetDepth( p.llMaxSendsPerSec );
    uint32_t uFloor = (uint32_t)std::max( 0, p.iMinVelocity );
    if ( m_bScout && ( m_bScoutCollapse != p.bCollapse || m_uScoutFloor != uFloor ) ) m_bScout = false;   // the counting rule changed
    if ( !m_bScout )
    {
        // In step from the start of a tick, so every note-on the scout counts is later read by the cursor
        if ( !m_bAtTickStart ) return;
        m_Scout.Seek( m_Cursor.NextTick() );
        memset( m_aWindow, 0, sizeof( m_aWindow ) );
        std::fill( m_aScoutTick, m_aScoutTick + 2048, SIZE_MAX );
        std::fill( m_aCursorTick, m_aCursorTick + 2048, SIZE_MAX );
        m_bScoutCollapse = p.bCollapse;
        m_uScoutFloor = uFloor;
        m_bScout = true;
    }
    // Whole ticks: at least past the cursor's tick, then to the end of the window
    long long llEnd = tE + (long long)( BudgetWindowUs * p.dSpeed );
    size_t U = m_Stream.UniqueTicks();
    while ( m_Scout.NextTick() < U && ( m_Scout.NextTick() <= m_uCachedTick || m_Scout.NextTime() <= llEnd ) )
    {
        size_t u = m_Scout.NextTick();
        uint32_t uTrack, uMsg;
        while ( m_Scout.Next( uTrack, uMsg, u + 1 ) )
        {
            uint32_t uVel = uMsg >> 16;
            if ( ( uMsg & 0xF0 ) != 0x90 || !uVel || uVel < uFloor ) continue;    // below the floor: never sent, not counted
            if ( p.bCollapse )
            {
                size_t &t = m_aScoutTick[( ( uMsg & 0x0F ) << 7 ) | ( ( uMsg >> 8 ) & 0x7F )];
                if ( t == u ) continue;
                t = u;
            }
            m_aWindow[uVel]++;
        }
    }
    // The bucket
    if ( m_llTokenTime == LLONG_MIN ) m_dTokens = dDepth;
    else if ( tE > m_llTokenTime ) m_dTokens = std::min( dDepth, m_dTokens + dRate * ( tE - m_llTokenTime ) / 1e6 / p.dSpeed );
    m_llTokenTime = tE;
    m_Cut = BudgetThreshold( m_aWindow, ( std::max( 0.0, m_dTokens ) + dDepth ) / 2.0 );
}

// The budget's decision for a note-on (counted in the window already); a kept one pays for itself and its off
bool PlaybackDecoder::BudgetKeep( uint32_t uVel )
{
    bool bKeep = (int)uVel > m_Cut.iVelocity;
    if ( (int)uVel == m_Cut.iVelocity )
    {
        m_dKeepAcc += m_Cut.dKeep;
        if ( m_dKeepAcc >= 1.0 ) { m_dKeepAcc -= 1.0; bKeep = true; }
    }
    if ( bKeep && m_dTokens < 2.0 ) bKeep = false;
    if ( bKeep ) m_dTokens -= 2.0;
    return bKeep;
}

size_t PlaybackDecoder::Fill( uint32_t *pOut, size_t iCap, long long llHorizon, long long llNow, const Params &p, long long &llDue )
{
    if ( p.dVolumeCorrect != m_dLutCorrect )
    {
        m_dLutCorrect = p.dVolumeCorrect;
        for ( int v = 0; v < 128; v++ ) m_aVelLut[v] = (uint8_t)static_cast< int >( v * p.dVolumeCorrect + 0.5 );
    }
    // Turning the metronome on resynchronises it to the next beat
    if ( p.iMetronome != MetOff && m_iMetronome == MetOff )
        SetBeat( GetBeatTick( CurrentTick( llNow ) ) );
    m_iMetronome = p.iMetronome;

    llDue = NextTime( p.iMetronome );
    if ( llDue > llHorizon ) return 0;
    long long llLimit = std::min( llHorizon, llDue + std::max< long long >( 1, (long long)( 500 * p.dSpeed ) ) );

    const RefMidiInfo &info = m_Song.info;
    const std::atomic< uint16_t > *pMute = m_pMuteMask;
    // Switching collapsing on or off: the held counts start over (a voice still sounding gets the off
    // of its first note)
    if ( p.bCollapse != m_bCollapse )
    {
        m_bCollapse = p.bCollapse;
        memset( m_aHeld, 0, sizeof( m_aHeld ) );
        memset( m_aVoices, 0, sizeof( m_aVoices ) );
        std::fill( m_aVoiceTick, m_aVoiceTick + 2048, SIZE_MAX );
    }
    const bool bBudget = p.llMaxSendsPerSec > 0;
    if ( !bBudget ) { m_bScout = false; m_Cut = BudgetCut(); }
    m_uCall++;
    // Work per call is bounded too, not only output: in a crash where almost every note-on is
    // skipped, one call could otherwise read hundreds of millions of events before returning
    const size_t iMaxRead = 262144;
    size_t n = 0, iRead = 0;
    while ( n < iCap && iRead < iMaxRead )
    {
        long long tE = EventTime();
        if ( p.iMetronome != MetOff && m_llNextBeatTime < tE && m_llNextBeatTime <= llLimit )
        {
            const Signature &s = SignatureAt( m_llNextBeatTick );
            long long iBeat = GetBeat( m_llNextBeatTick, s );
            bool bIsMeasure = s.iBeatsPerMeasure != 0 && !( ( iBeat < 0 ? -iBeat : iBeat ) % s.iBeatsPerMeasure );
            if ( !p.bMute && info.llNoteCount > 0 && ( p.iMetronome == MetEveryBeat || ( bIsMeasure && p.iMetronome == MetEveryMeasure ) ) )
            {
                m_iLastMetronomeNote = m_iLastMetronomeNote == 76 ? 77 : 76;
                int iVel = static_cast< int >( info.llVolumeSum * p.dVolumeCorrect / info.llNoteCount + -.5 );
                pOut[n++] = 0x99 | ( m_iLastMetronomeNote << 8 ) | ( (uint32_t)( iVel & 0xFF ) << 16 );
            }
            SetBeat( GetBeatTick( m_llNextBeatTick + 1 ) );
            continue;
        }
        if ( tE > llLimit ) break;

        m_iSkipVelocity = p.bVelocitySkip ? VelocityThreshold( llNow - tE + p.llSenderLagUs, p.llSkipLagUs ) : 0;
        uint32_t uFloor = (uint32_t)std::max( 0, p.iMinVelocity );
        uint32_t uMinVel = std::max( (uint32_t)m_iSkipVelocity, uFloor );
        m_iSkipVelocity = (int)uMinVel;     // the statistics show the floor too
        size_t u = m_uCachedTick, uEnd = u + 1;
        if ( bBudget && m_bAtTickStart ) BudgetTick( tE, p );
        const bool bCounting = bBudget && m_bScout;
        uint32_t uTrack, uMsg;
        m_bAtTickStart = false;
        while ( n < iCap && iRead < iMaxRead && ( m_bAtTickStart = !m_Cursor.Next( uTrack, uMsg, uEnd ) ) == false )
        {
            iRead++;
            uint32_t uStatus = uMsg & 0xF0;
            if ( uStatus != 0x90 && uStatus != 0x80 )
            {
                pOut[n++] = uMsg;
                if ( bCounting ) m_dTokens -= 1.0;
                continue;
            }
            uint32_t ch = uMsg & 0x0F, idx = ( ch << 7 ) | ( ( uMsg >> 8 ) & 0x7F ), uVel = uMsg >> 16;
            bool bMuted = uStatus == 0x90 && ( p.bMute || ( pMute && ( ( pMute[uTrack].load( std::memory_order_relaxed ) >> ch ) & 1 ) ) );
            if ( uStatus == 0x90 && uVel )
            {
                // The scout counted it (muted or not), so it leaves the window here
                if ( bCounting && uVel >= uFloor && ( !p.bCollapse || m_aCursorTick[idx] != u ) )
                {
                    if ( p.bCollapse ) m_aCursorTick[idx] = u;
                    if ( m_aWindow[uVel] ) m_aWindow[uVel]--;
                }
                if ( bMuted ) continue;
                m_Counters.llNoteOns++;
                uint32_t uOutVel = m_aVelLut[uVel];
                if ( p.bCollapse && m_aVoices[idx] && m_aVoiceTick[idx] == u )
                {
                    // Another note-on of a sounding voice's tick: held silently (the voice takes the loudest)
                    m_aHeld[idx]++;
                    m_Counters.llCollapsed++;
                    if ( m_aVoiceCall[idx] == m_uCall && ( ( pOut[m_aVoiceOut[idx]] >> 16 ) & 0xFF ) < uOutVel )
                        pOut[m_aVoiceOut[idx]] = ( pOut[m_aVoiceOut[idx]] & 0xFFFF ) | ( uOutVel << 16 );
                    continue;
                }
                if ( uVel < uMinVel || ( bCounting && !BudgetKeep( uVel ) ) )
                {
                    m_aPendingSkip[idx]++;
                    m_Counters.llSkipped++;
                    continue;
                }
                if ( p.bCollapse )
                {
                    m_aHeld[idx]++;
                    m_aVoices[idx]++;
                    m_aVoiceTick[idx] = u;
                    m_aVoiceOut[idx] = (uint32_t)n;
                    m_aVoiceCall[idx] = m_uCall;
                }
                pOut[n++] = ( uMsg & 0xFFFF ) | ( uOutVel << 16 );
                continue;
            }
            // An off (NoteOff, or NoteOn with velocity 0): drop the one that belongs to a skipped on
            if ( m_aPendingSkip[idx] ) { m_aPendingSkip[idx]--; continue; }
            if ( p.bCollapse && m_aHeld[idx] )
            {
                // A held note ends; its voice ends only when no more notes are held than voices remain
                m_aHeld[idx]--;
                if ( m_aHeld[idx] >= m_aVoices[idx] ) continue;
                m_aVoices[idx]--;
            }
            if ( bMuted ) continue;
            pOut[n++] = uMsg;
        }
    }
    m_Counters.llSent += n;
    return n;
}
