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
    ResetSendState();
}

// Everything sounding was stopped (start, seek): the skip, collapse and budget state start over
void PlaybackDecoder::ResetSendState()
{
    for ( KeyState &k : m_aKey ) { k.uPending = k.uHeld = k.uVoices = 0; k.uVoiceTick = SIZE_MAX; }
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
PlaybackDecoder::BudgetCut PlaybackDecoder::BudgetThreshold( const uint64_t aWindow[128], double dAllowance, int iFloor )
{
    BudgetCut c;
    double dSum = 0.0;
    for ( int v = 127; v >= std::max( 1, iFloor ); v-- )
    {
        if ( dSum + (double)aWindow[v] <= dAllowance ) { dSum += (double)aWindow[v]; continue; }
        c.iVelocity = v;
        c.dKeep = std::max( 0.0, dAllowance - dSum ) / (double)aWindow[v];
        break;
    }
    return c;
}

// At the start of each tick: the window moves to the cursor's bin and reaches BudgetWindowUs ahead,
// the bucket refills, and the cut is chosen so the window's note-ons (2 messages each, with their
// offs) fit what the bucket holds plus what it gains over the window
void PlaybackDecoder::BudgetTick( long long tE, const Params &p )
{
    double dRate = (double)p.llMaxSendsPerSec, dDepth = BudgetDepth( p.llMaxSendsPerSec );
    const VelocityBins &vb = m_Song.aSendOns[p.bCollapse];
    if ( m_bScout && m_bScoutCollapse != p.bCollapse ) m_bScout = false;   // the counting rule changed
    size_t b = (size_t)( std::max( 0LL, tE ) / VelocityBins::BinUs );
    if ( !m_bScout )
    {
        // From the start of a tick: its whole bin is counted, and the note-ons of the bin before the
        // tick, which the cursor never reads, leave the window when the cursor leaves the bin
        if ( !m_bAtTickStart ) return;
        memset( m_aRem, 0, sizeof( m_aRem ) );
        for ( KeyState &k : m_aKey ) k.uCursorTick = SIZE_MAX;
        vb.AddTo( b, m_aRem );
        memcpy( m_aWindow, m_aRem, sizeof( m_aWindow ) );
        m_uWinBin = b;
        m_uNextBin = b + 1;
        m_bScoutCollapse = p.bCollapse;
        m_bScout = true;
    }
    else if ( b > m_uWinBin )
    {
        // The cursor left its bin: what it did not read of it leaves the window, and the new bin's
        // counts become what is left to read
        for ( int v = 0; v < 128; v++ ) m_aWindow[v] -= m_aRem[v];
        memset( m_aRem, 0, sizeof( m_aRem ) );
        vb.AddTo( b, m_aRem );
        if ( b >= m_uNextBin )
        {
            for ( int v = 0; v < 128; v++ ) m_aWindow[v] += m_aRem[v];
            m_uNextBin = b + 1;
        }
        m_uWinBin = b;
    }
    // Whole bins to the end of the window
    long long llEnd = tE + (long long)( BudgetWindowUs * p.dSpeed );
    while ( m_uNextBin < vb.Bins() && (long long)m_uNextBin * VelocityBins::BinUs <= llEnd ) vb.AddTo( m_uNextBin++, m_aWindow );
    // The bucket
    if ( m_llTokenTime == LLONG_MIN ) m_dTokens = dDepth;
    else if ( tE > m_llTokenTime ) m_dTokens = std::min( dDepth, m_dTokens + dRate * ( tE - m_llTokenTime ) / 1e6 / p.dSpeed );
    m_llTokenTime = tE;
    m_Cut = BudgetThreshold( m_aWindow, ( std::max( 0.0, m_dTokens ) + dDepth ) / 2.0, p.iMinVelocity );
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
        for ( KeyState &k : m_aKey ) { k.uHeld = k.uVoices = 0; k.uVoiceTick = SIZE_MAX; }
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

        // Keys of one note run (one track, channel, status and velocity), so what holds for the whole
        // run is decided once: up to the key with bit 7 set (the run's last, then bEnd) or iMax keys,
        // and while there is room in pOut. Returns the keys taken.
        auto Notes = [&]( uint32_t uTrack, uint32_t uBase, const uint8_t *pk, size_t iMax, bool &bEnd ) -> size_t {
            uint32_t ch = uBase & 0x0F, uKeys = ch << 7, uVel = uBase >> 16;
            size_t i = 0;
            bEnd = false;
            if ( ( uBase & 0xF0 ) == 0x90 && uVel )
            {
                // Muted: not sent, so its off is dropped too (DESIGN.md 1.6). Every other off is sent,
                // muted or not: a note that was sounding when its channel was muted still ends
                const bool bMuted = p.bMute || ( pMute && ( ( pMute[uTrack].load( std::memory_order_relaxed ) >> ch ) & 1 ) );
                const bool bBelow = uVel < uMinVel;
                const uint32_t uOutVel = m_aVelLut[uVel], uOut = ( uBase & 0xFF ) | ( uOutVel << 16 );
                while ( i < iMax && n < iCap )
                {
                    unsigned k = pk[i++];
                    KeyState &ks = m_aKey[uKeys | ( k & 0x7F )];
                    // The window counted it (muted or not), so it leaves the window here
                    if ( bCounting && ( !p.bCollapse || ks.uCursorTick != u ) )
                    {
                        if ( p.bCollapse ) ks.uCursorTick = u;
                        if ( m_aRem[uVel] ) { m_aRem[uVel]--; m_aWindow[uVel]--; }
                    }
                    if ( bMuted ) ks.uPending++;
                    else if ( m_Counters.llNoteOns++, p.bCollapse && ks.uVoices && ks.uVoiceTick == u )
                    {
                        // Another note-on of a sounding voice's tick: held silently (the voice takes the loudest)
                        ks.uHeld++;
                        m_Counters.llCollapsed++;
                        if ( ks.uVoiceCall == m_uCall && ( ( pOut[ks.uVoiceOut] >> 16 ) & 0xFF ) < uOutVel )
                            pOut[ks.uVoiceOut] = ( pOut[ks.uVoiceOut] & 0xFFFF ) | ( uOutVel << 16 );
                    }
                    else if ( bBelow || ( bCounting && !BudgetKeep( uVel ) ) )
                    {
                        ks.uPending++;
                        m_Counters.llSkipped++;
                    }
                    else
                    {
                        if ( p.bCollapse )
                        {
                            ks.uHeld++;
                            ks.uVoices++;
                            ks.uVoiceTick = u;
                            ks.uVoiceOut = (uint32_t)n;
                            ks.uVoiceCall = m_uCall;
                        }
                        pOut[n++] = uOut | ( ( k & 0x7F ) << 8 );
                    }
                    if ( k & 0x80 ) { bEnd = true; break; }
                }
                return i;
            }
            // Offs (NoteOff, or NoteOn with velocity 0)
            while ( i < iMax && n < iCap )
            {
                unsigned k = pk[i++];
                KeyState &ks = m_aKey[uKeys | ( k & 0x7F )];
                // Drop the one that belongs to a skipped on
                if ( ks.uPending ) ks.uPending--;
                else if ( p.bCollapse && ks.uHeld )
                {
                    // A held note ends; its voice ends only when no more notes are held than voices remain
                    ks.uHeld--;
                    if ( ks.uHeld < ks.uVoices )
                    {
                        ks.uVoices--;
                        pOut[n++] = uBase | ( ( k & 0x7F ) << 8 );
                    }
                }
                else pOut[n++] = uBase | ( ( k & 0x7F ) << 8 );
                if ( k & 0x80 ) { bEnd = true; break; }
            }
            return i;
        };

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
            // The note, then the rest of its run straight from the stream's bytes
            bool bEnd;
            uint8_t k1 = (uint8_t)( ( ( uMsg >> 8 ) & 0x7F ) | 0x80 );
            Notes( uTrack, uMsg & 0xFF00FF, &k1, 1, bEnd );
            while ( m_Cursor.InRun() && n < iCap && iRead < iMaxRead )
            {
                size_t iAvail;
                uint32_t uRunTrack, uBase;
                const uint8_t *pk = m_Cursor.RunKeys( iAvail, uRunTrack, uBase );
                size_t iTaken = Notes( uRunTrack, uBase, pk, std::min( iAvail, iMaxRead - iRead ), bEnd );
                iRead += iTaken;
                m_Cursor.TakeKeys( iTaken, bEnd );
            }
        }
    }
    m_Counters.llSent += n;
    return n;
}
