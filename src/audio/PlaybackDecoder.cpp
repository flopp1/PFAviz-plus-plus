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
    for ( KeyState &k : m_aKey ) { k.uPending = k.uHeld = k.uVoices = k.uSentOffs = 0; k.uVoiceTick = SIZE_MAX; }
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

void PlaybackDecoder::Seek( long long llTarget, long long, std::vector< uint32_t > &vOut )
{
    // JumpTo starts with AllNotesOff
    for ( uint32_t ch = 0; ch < 16; ch++ ) vOut.push_back( 0xB0 | ch | ( 0x7B << 8 ) );
    for ( uint32_t ch = 0; ch < 16; ch++ ) vOut.push_back( 0xB0 | ch | ( 0x40 << 8 ) );

    // The oracle's start position: the first event with time >= target
    m_Cursor.Seek( llTarget <= 0 ? 0 : m_Stream.times.LowerBound( (uint64_t)llTarget ) );
    m_uCachedTick = SIZE_MAX;

    Chase( llTarget, vOut );

    // AdvanceIterators( llStartTime, true ): the next beat at or after the current tick
    SetBeat( GetBeatTick( CurrentTick( llTarget ) ) );

    // Everything sounding was stopped, so the skip balance starts over
    ResetSendState();
}

// The chase (DESIGN.md 7.7): Reset All Controllers on every channel, then the last value before the
// target of every item the song sets, in the order those were set (so a bank select stays before its
// program change), then GM defaults for what has none: volume and pan on every channel, and bank,
// program, reverb and chorus where the song sets them only later. Found by scanning back from the
// target until every item the song sets is found, so a seek usually reads a few seconds of the list.
void PlaybackDecoder::Chase( long long llTarget, std::vector< uint32_t > &vOut )
{
    // What CC 121 resets: modulation, expression, the four pedals, the (N)RPN numbers, bend and pressure
    static const int aReset[] = { 1, 11, 64, 65, 66, 67, 98, 99, 100, 101, 121, 122 };
    struct Default { int iItem, iValue; bool bAlways; };
    static const Default aDefault[] = { { 7, 100, true }, { 10, 64, true }, { 0, 0, false }, { 32, 0, false },
                                        { 120, 0, false }, { 91, 40, false }, { 93, 0, false } };
    for ( uint32_t ch = 0; ch < 16; ch++ ) vOut.push_back( 0xB0 | ch | ( 121 << 8 ) );

    const std::vector< ChaseEvent > &vChase = m_Song.vChase;
    size_t iNew = (size_t)( std::upper_bound( vChase.begin(), vChase.end(), llTarget,
                                              []( long long t, const ChaseEvent &e ) { return t < e.llTime; } ) - vChase.begin() );
    // Back from the target: an item's first event is its value; a CC 121 settles the items it resets
    // that have none after it (at their reset values, sent above)
    uint8_t aFound[16][ChaseItems] = {};
    std::vector< size_t > vPick;
    uint32_t uLeft = m_Song.uChaseItems;
    for ( size_t i = iNew; i-- > 0 && uLeft; )
    {
        uint32_t uMsg = vChase[i].uMsg, ch = uMsg & 0x0F;
        int it = ChaseItem( uMsg );
        if ( it >= 0 )
        {
            if ( !aFound[ch][it] ) { aFound[ch][it] = 1; uLeft--; vPick.push_back( i ); }
        }
        else if ( ( uMsg & 0xFFF0 ) == ( 0xB0 | ( 121 << 8 ) ) )
            for ( int r : aReset )
                if ( !aFound[ch][r] ) { aFound[ch][r] = 1; uLeft -= m_Song.aChaseUsed[ch][r]; }
    }
    for ( auto it = vPick.rbegin(); it != vPick.rend(); ++it ) vOut.push_back( vChase[*it].uMsg );
    for ( uint32_t ch = 0; ch < 16; ch++ )
        for ( const Default &d : aDefault )
            if ( !aFound[ch][d.iItem] && ( d.bAlways || m_Song.aChaseUsed[ch][d.iItem] ) )
                vOut.push_back( d.iItem == 120 ? 0xC0 | ch | ( d.iValue << 8 ) : 0xB0 | ch | ( d.iItem << 8 ) | ( d.iValue << 16 ) );
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
    m_dDepth = dDepth;
    if ( m_bLane )
    {
        // A lane: the window is whole bins from the tick's (counted again for each bin), the cut weighs
        // it against the batch's tokens, and the lane's bucket fills over its own song time
        size_t b = (size_t)( std::max( 0LL, tE ) / VelocityBins::BinUs );
        if ( b != m_uWinBin )
        {
            memset( m_aWindow, 0, sizeof( m_aWindow ) );
            long long llEnd = tE + (long long)( BudgetWindowUs * p.dSpeed );
            for ( size_t bb = b; bb < vb.Bins() && (long long)bb * VelocityBins::BinUs <= llEnd; bb++ ) vb.AddTo( bb, m_aWindow );
            m_uWinBin = b;
            m_Cut = BudgetThreshold( m_aWindow, ( std::max( 0.0, m_dLaneTokens ) + dDepth ) / 2.0, p.iMinVelocity );
        }
        if ( tE > m_llTokenTime ) m_dTokens = std::min( dDepth, m_dTokens + dRate * ( tE - m_llTokenTime ) / 1e6 / p.dSpeed );
        m_llTokenTime = tE;
        return;
    }
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
    while ( n < iCap && iRead < iMaxRead && !m_Cursor.Stopped() )
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
                        ks.uSentOffs++;
                        pOut[n++] = uBase | ( ( k & 0x7F ) << 8 );
                    }
                }
                else
                {
                    ks.uSentOffs++;
                    pOut[n++] = uBase | ( ( k & 0x7F ) << 8 );
                }
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
    m_llRead += iRead;
    return n;
}

//-----------------------------------------------------------------------------
// Parallel decoding (DESIGN.md 7.11)
//-----------------------------------------------------------------------------

bool PlaybackDecoder::PlanBatch( int iLanes, long long llHorizon, uint64_t llMaxBytes, long long llNow, Batch &b )
{
    if ( m_Cursor.AtEnd() || EventTime() > llHorizon ) return false;
    const std::vector< EventStream::SeekPoint > &vSeek = m_Stream.vSeek;
    size_t U = m_Stream.UniqueTicks(), u = m_Cursor.NextTick();
    uint64_t llRaw0 = m_Cursor.RawPos();
    // To the first tick after the horizon, and to a tick start within llMaxBytes (a seek point)
    size_t uEnd = llHorizon >= LLONG_MAX / 2 ? U : m_Stream.times.LowerBound( (uint64_t)std::max( 0LL, llHorizon ) + 1 );
    auto itCap = std::upper_bound( vSeek.begin(), vSeek.end(), llRaw0 + llMaxBytes,
                                   []( uint64_t v, const EventStream::SeekPoint &s ) { return v < s.llRaw; } );
    if ( itCap != vSeek.begin() ) uEnd = std::min( uEnd, (size_t)( itCap - 1 )->llU );
    uEnd = std::min( U, std::max( uEnd, u + 1 ) );
    // Where it ends in the stream, about: the first tick start at or after uEnd
    auto itEnd = std::lower_bound( vSeek.begin(), vSeek.end(), (uint64_t)uEnd,
                                   []( const EventStream::SeekPoint &s, uint64_t v ) { return s.llU < v; } );
    uint64_t llRawEnd = itEnd != vSeek.end() ? itEnd->llRaw : m_Stream.llRawBytes;
    if ( llRawEnd <= llRaw0 ) llRawEnd = m_Stream.llRawBytes;
    b.uEnd = uEnd;
    b.vCut.resize( iLanes + 1 );
    for ( int k = 0; k < iLanes; k++ ) b.vCut[k] = llRaw0 + ( llRawEnd - llRaw0 ) * (uint64_t)k / (uint64_t)iLanes;
    b.vCut[iLanes] = UINT64_MAX;    // the last lane ends at uEnd
    b.llNow = llNow;
    b.dTokens = m_llTokenTime == LLONG_MIN ? 1e300 : m_dTokens;   // a full bucket before the first tick
    return true;
}

void PlaybackDecoder::FillLane( const Params &p, long long llNow, LaneOut &out )
{
    uint32_t aBuf[8192];
    for ( ;; )
    {
        long long llDue;
        size_t n = Fill( aBuf, 8192, LLONG_MAX - 1, llNow, p, llDue );
        if ( n )
        {
            out.vMsgs.insert( out.vMsgs.end(), aBuf, aBuf + n );
            out.vGroups.push_back( { (uint32_t)n, llDue } );
        }
        if ( m_Cursor.Stopped() || ( !n && llDue == LLONG_MAX ) ) break;
    }
}

void PlaybackDecoder::RunMainLane( const Batch &b, const Params &p, LaneOut &out )
{
    out.Clear();
    m_Cursor.SetStop( b.vCut.size() > 2 ? b.vCut[1] : UINT64_MAX, b.uEnd );
    FillLane( p, b.llNow, out );
}

void PlaybackDecoder::RunLane( const Batch &b, int k, const Params &p, LaneOut &out )
{
    out.Clear();
    // From the last tick start at or before the cut
    const std::vector< EventStream::SeekPoint > &vSeek = m_Stream.vSeek;
    auto it = std::upper_bound( vSeek.begin(), vSeek.end(), b.vCut[k], []( uint64_t v, const EventStream::SeekPoint &s ) { return v < s.llRaw; } );
    if ( it == vSeek.begin() ) return;
    --it;
    m_Cursor.SeekGroup( it->llRaw, (size_t)it->llU, b.vCut[k], b.uEnd );
    m_Cursor.SetStop( b.vCut[k + 1], b.uEnd );
    // No state: the stitch carries it over
    ResetSendState();
    m_Counters = Counters();
    m_llRead = 0;
    m_iSkipVelocity = 0;
    m_uCachedTick = SIZE_MAX;
    m_bCollapse = p.bCollapse;
    m_bLane = true;
    m_bScout = true;                    // counting, with nothing in m_aRem to take out
    m_uWinBin = SIZE_MAX;
    m_dLaneTokens = std::min( b.dTokens, BudgetDepth( p.llMaxSendsPerSec ) );
    m_dTokens = 0.0;
    m_llTokenTime = EventTime();
    Params q = p;
    q.iMetronome = MetOff;              // clicks come from lane 0 and after the batch
    FillLane( q, b.llNow, out );
}

void PlaybackDecoder::FinishBatch( const Batch &b, PlaybackDecoder *const *aLanes, LaneOut *aOut, int iLanes,
                                   const std::function< void( const std::function< void( int ) > & ) > &parallel,
                                   const std::function< void( const uint32_t *, size_t, long long ) > &emit )
{
    // Per channel and key: offs owed (note-ons not sent: pending skips and held notes, which drop
    // offs alike), voices sounding and the tick of the last. Lane 0 is this decoder's own: exact.
    std::vector< uint64_t > vOwed( 2048 ), vVoices( 2048 );
    std::vector< size_t > vVoiceTick( 2048 );
    for ( int i = 0; i < 2048; i++ )
    {
        const KeyState &ks = m_aKey[i];
        vOwed[i] = ks.uPending + ( ks.uHeld - ks.uVoices );
        vVoices[i] = ks.uVoices;
        vVoiceTick[i] = ks.uVoiceTick;
    }
    // Each later lane owes, per key, the first of the offs it sent, as many as are owed then
    // (counted here, dropped by the lanes themselves at once)
    std::vector< uint32_t > vQuota( (size_t)iLanes * 2048 );
    std::vector< char > vDrops( iLanes );
    // The bucket at the batch's end is the last lane's (with what read anything): earlier lanes'
    // leftovers belong to earlier song time, and spending them later could exceed the budget
    double dTokens = m_dTokens;
    uint64_t llDropped = 0;
    for ( int k = 1; k < iLanes; k++ )
    {
        PlaybackDecoder &L = *aLanes[k];
        uint32_t *pQuota = &vQuota[(size_t)k * 2048];
        for ( int i = 0; i < 2048; i++ )
        {
            const KeyState &ks = L.m_aKey[i];
            if ( vOwed[i] && ks.uSentOffs )
            {
                uint32_t q = (uint32_t)std::min< uint64_t >( vOwed[i], ks.uSentOffs );
                pQuota[i] = q;
                vOwed[i] -= q;
                llDropped += q;
                vDrops[k] = 1;
            }
            // What the lane leaves owing and sounding adds to what was carried in
            vOwed[i] += ks.uPending + ( ks.uHeld - ks.uVoices );
            vVoices[i] = std::min< uint64_t >( vVoices[i] + ks.uVoices, 1u << 30 );
            if ( ks.uVoiceTick != SIZE_MAX ) vVoiceTick[i] = ks.uVoiceTick;
        }
        m_Counters.llSent += L.m_Counters.llSent;
        m_Counters.llNoteOns += L.m_Counters.llNoteOns;
        m_Counters.llSkipped += L.m_Counters.llSkipped;
        m_Counters.llCollapsed += L.m_Counters.llCollapsed;
        m_llRead += L.m_llRead;
        if ( L.m_llRead ) { dTokens = L.m_dTokens; m_iSkipVelocity = L.m_iSkipVelocity; m_Cut = L.m_Cut; }
    }
    m_Counters.llSent -= llDropped;
    if ( llDropped )
        parallel( [&]( int k ) {
            if ( !vDrops[k] ) return;
            // The lane's first offs of each owing key, in order
            uint32_t *pQuota = &vQuota[(size_t)k * 2048];
            LaneOut &o = aOut[k];
            size_t r = 0, w = 0, g = 0;
            for ( auto &grp : o.vGroups )
            {
                size_t w0 = w;
                for ( uint32_t i = 0; i < grp.first; i++ )
                {
                    uint32_t m = o.vMsgs[r++], s = m & 0xF0;
                    if ( s == 0x80 || ( s == 0x90 && !( m & 0x7F0000 ) ) )
                    {
                        uint32_t &q = pQuota[( ( m & 0x0F ) << 7 ) | ( ( m >> 8 ) & 0x7F )];
                        if ( q ) { q--; continue; }
                    }
                    o.vMsgs[w++] = m;
                }
                if ( w > w0 ) o.vGroups[g++] = { (uint32_t)( w - w0 ), grp.second };
            }
            o.vMsgs.resize( w );
            o.vGroups.resize( g );
        } );
    for ( int k = 0; k < iLanes; k++ )
    {
        const uint32_t *pm = aOut[k].vMsgs.data();
        for ( const auto &g : aOut[k].vGroups ) { emit( pm, g.first, g.second ); pm += g.first; }
    }
    // The carried state becomes this decoder's: owed offs as pending skips (they drop offs alike)
    for ( int i = 0; i < 2048; i++ )
    {
        KeyState &ks = m_aKey[i];
        ks.uPending = (uint32_t)std::min< uint64_t >( vOwed[i], UINT32_MAX );
        ks.uHeld = ks.uVoices = (uint32_t)vVoices[i];
        ks.uVoiceTick = vVoiceTick[i];
        ks.uCursorTick = SIZE_MAX;
    }
    // On from the batch's end: the budget's window starts over at the next tick, the bucket keeps
    // what the lanes left, and the metronome goes to the next beat
    m_Cursor.Seek( b.uEnd );
    m_uCachedTick = SIZE_MAX;
    m_bAtTickStart = true;
    m_bScout = false;
    long long llEnd = EventTime();
    if ( llEnd == LLONG_MAX ) llEnd = m_Song.info.llTotalMicroSecs;
    if ( m_llTokenTime != LLONG_MIN )
    {
        m_dTokens = std::min( dTokens, m_dDepth );
        m_llTokenTime = std::max( m_llTokenTime, llEnd );
    }
    if ( m_iMetronome != MetOff && m_llNextBeatTime < llEnd ) SetBeat( GetBeatTick( CurrentTick( llEnd ) ) );
}
