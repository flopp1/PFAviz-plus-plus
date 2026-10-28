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
    m_llPending = 0;
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

// RPN and NRPN values (pitch bend range, tuning, ...): a data entry (CC 6, 38) sets the parameter selected
// when it came, so these controllers are not chased by their last values (the data would go to the
// selection last made, and a range set before a tuning RPN was lost: X-Cool's bends after a seek).
// Per channel, each parameter set by the target is selected and given its values again, in the order
// they were first set, then the last selection is made again. A CC 121 clears the selection.
static void ChaseParameters( const std::vector< ChaseEvent > &vEvents, long long llTarget, std::vector< uint32_t > &vOut )
{
    struct Param { unsigned uId; int iMsb, iLsb; };     // uId: 0x4000 for an NRPN | number MSB << 7 | LSB
    struct State { unsigned aRpn[2] = { 127, 127 }, aNrpn[2] = { 127, 127 }; bool bNrpn = false; std::vector< Param > v; };
    State aState[16];
    for ( const ChaseEvent &e : vEvents )
    {
        if ( e.llTime > llTarget ) break;
        State &s = aState[e.uMsg & 0x0F];
        unsigned cc = ( e.uMsg >> 8 ) & 0x7F, v = ( e.uMsg >> 16 ) & 0x7F;
        switch ( cc )
        {
            case 101: s.aRpn[0] = v; s.bNrpn = false; break;
            case 100: s.aRpn[1] = v; s.bNrpn = false; break;
            case 99: s.aNrpn[0] = v; s.bNrpn = true; break;
            case 98: s.aNrpn[1] = v; s.bNrpn = true; break;
            case 121: s.aRpn[0] = s.aRpn[1] = s.aNrpn[0] = s.aNrpn[1] = 127; s.bNrpn = false; break;
            case 6: case 38:
            {
                const unsigned *pSel = s.bNrpn ? s.aNrpn : s.aRpn;
                if ( pSel[0] == 127 && pSel[1] == 127 ) break;  // the null parameter: no effect
                unsigned uId = ( s.bNrpn ? 0x4000u : 0u ) | pSel[0] << 7 | pSel[1];
                auto it = std::find_if( s.v.begin(), s.v.end(), [&]( const Param &p ) { return p.uId == uId; } );
                if ( it == s.v.end() ) it = s.v.insert( s.v.end(), { uId, -1, -1 } );
                ( cc == 6 ? it->iMsb : it->iLsb ) = (int)v;
                break;
            }
        }
    }
    for ( uint32_t ch = 0; ch < 16; ch++ )
    {
        const State &s = aState[ch];
        auto Select = [&]( bool bNrpn, unsigned uMsb, unsigned uLsb ) {
            vOut.push_back( 0xB0 | ch | ( bNrpn ? 99u : 101u ) << 8 | uMsb << 16 );
            vOut.push_back( 0xB0 | ch | ( bNrpn ? 98u : 100u ) << 8 | uLsb << 16 );
        };
        for ( const Param &p : s.v )
        {
            Select( p.uId & 0x4000, ( p.uId >> 7 ) & 0x7F, p.uId & 0x7F );
            if ( p.iMsb >= 0 ) vOut.push_back( 0xB0 | ch | 6 << 8 | (uint32_t)p.iMsb << 16 );
            if ( p.iLsb >= 0 ) vOut.push_back( 0xB0 | ch | 38 << 8 | (uint32_t)p.iLsb << 16 );
        }
        const unsigned *pSel = s.bNrpn ? s.aNrpn : s.aRpn;
        if ( !s.v.empty() || pSel[0] != 127 || pSel[1] != 127 ) Select( s.bNrpn, pSel[0], pSel[1] );
    }
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
    if ( m_bBendChase )
    {
        for ( uint32_t ch = 0; ch < 16; ch++ )
        {
            for ( const Default &d : aDefault ) vOut.push_back( 0xB0 | ch | ( d.iItem << 8 ) | ( d.iValue << 16 ) );
            vOut.push_back( 0xC0 | ch );
        }
        for ( const ChaseEvent &event : vChase )
        {
            if ( event.llTime > llTarget ) break;
            vOut.push_back( event.uMsg );
        }
        return;
    }
    size_t iNew = (size_t)( std::upper_bound( vChase.begin(), vChase.end(), llTarget,
                                              []( long long t, const ChaseEvent &e ) { return t < e.llTime; } ) - vChase.begin() );
    // Back from the target: an item's first event is its value; a CC 121 settles the items it resets
    // that have none after it (at their reset values, sent above). The RPN and NRPN controllers are
    // chased apart (ChaseParameters)
    uint8_t aFound[16][ChaseItems] = {};
    std::vector< size_t > vPick;
    uint32_t uLeft = m_Song.uChaseItems;
    for ( int ch = 0; ch < 16; ch++ )
        for ( int it : { 6, 38, 98, 99, 100, 101 } )
            if ( m_Song.aChaseUsed[ch][it] ) { aFound[ch][it] = 1; uLeft--; }
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
    ChaseParameters( m_Song.vParamChase, llTarget, vOut );
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
    // In integers (the counts are whole, so a sum fits the allowance exactly when it fits its floor):
    // this runs at every tick
    BudgetCut c;
    uint64_t llLimit = dAllowance >= 1.8e19 ? UINT64_MAX : dAllowance > 0.0 ? (uint64_t)dAllowance : 0, llSum = 0;
    for ( int v = 127; v >= std::max( 1, iFloor ); v-- )
    {
        if ( aWindow[v] <= llLimit - llSum ) { llSum += aWindow[v]; continue; }
        c.iVelocity = v;
        c.dKeep = std::max( 0.0, dAllowance - (double)llSum ) / (double)aWindow[v];
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

template < bool bCollapse, bool bCounting, bool bLane >
size_t PlaybackDecoder::Notes( uint32_t uTrack, uint32_t uBase, const uint8_t *pk, size_t iMax, bool &bEnd, uint32_t *__restrict pOut, size_t &rn,
                               size_t u, uint32_t uMinVel, const Params &p )
{
    bEnd = false;
    if ( ( uBase & 0x8000 ) && ( !p.bTuning31 || ( uBase & 0x0F ) == 9 ) )
    {
        size_t i = 0;
        while ( i < iMax )
            if ( pk[i++] & 0x80 ) { bEnd = true; break; }
        return i;
    }
    uint32_t ch = uBase & 0x0F, uKeys = ( ch << 8 ) | ( ( uBase >> 8 ) & 0x80 ), uVel = uBase >> 16;
    size_t i = 0, n = rn;
    // Every key sent as it is: the run's length (to its last key, or iMax), then the messages
    auto SendAll = [&]( uint32_t uOut ) {
        // Most runs of a many-track song are one key
        if ( pk[0] & 0x80 )
        {
            pOut[n++] = uOut | ( ( pk[0] & 0x7Fu ) << 8 );
            i = 1;
            bEnd = true;
            return;
        }
        while ( i + 8 <= iMax )
        {
            uint64_t w;
            memcpy( &w, pk + i, 8 );
            uint64_t m = w & 0x8080808080808080ull;
            if ( m ) { i += ( __builtin_ctzll( m ) >> 3 ) + 1; bEnd = true; break; }
            i += 8;
        }
        if ( !bEnd )
            while ( i < iMax )
                if ( pk[i++] & 0x80 ) { bEnd = true; break; }
        for ( size_t j = 0; j < i; j++ ) pOut[n + j] = uOut | ( ( pk[j] & 0x7Fu ) << 8 );
        n += i;
    };
    if ( ( uBase & 0xF0 ) == 0x90 && uVel )
    {
        // Muted: not sent, so its off is dropped too (DESIGN.md 1.6). Every other off is sent,
        // muted or not: a note that was sounding when its channel was muted still ends
        const bool bMuted = p.bMute || ( m_pMuteMask && ( ( m_pMuteMask[uTrack].load( std::memory_order_relaxed ) >> ch ) & 1 ) );
        const bool bBelow = uVel < uMinVel;
        const uint32_t uOutVel = m_aVelLut[uVel], uOut = ( uBase & 0x80FF ) | ( uOutVel << 16 );
        if ( !bCollapse && !bCounting && !bMuted && !bBelow )
        {
            SendAll( uOut );
            m_Counters.llNoteOns += i;
            rn = n;
            return i;
        }
        while ( i < iMax )
        {
            unsigned k = pk[i++];
            KeyState &ks = m_aKey[uKeys | ( k & 0x7F )];
            // The window counted it (muted or not), so it leaves the window here
            if ( bCounting && ( !bCollapse || ks.uCursorTick != u ) )
            {
                if ( bCollapse ) ks.uCursorTick = u;
                if ( m_aRem[uVel] ) { m_aRem[uVel]--; m_aWindow[uVel]--; }
            }
            if ( bMuted ) { ks.uPending++; m_llPending++; }
            else if ( m_Counters.llNoteOns++, bCollapse && ks.uVoices && ks.uVoiceTick == u )
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
                m_llPending++;
                m_Counters.llSkipped++;
            }
            else
            {
                if ( bCollapse )
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
        rn = n;
        return i;
    }
    // Offs (NoteOff, or NoteOn with velocity 0). With nothing to drop and no voices to count, all are
    // sent. A lane takes this only while nothing can be skipped, and does not count what it sends here
    // (FinishBatch counts them from its output if it needs them)
    if ( !bCollapse && !m_llPending && ( !bLane || m_bLaneFast ) )
    {
        SendAll( uBase );
        if ( bLane ) m_llFastOffs += i;
        rn = n;
        return i;
    }
    while ( i < iMax )
    {
        unsigned k = pk[i++];
        KeyState &ks = m_aKey[uKeys | ( k & 0x7F )];
        // Drop the one that belongs to a skipped on
        if ( ks.uPending ) { ks.uPending--; m_llPending--; }
        else if ( bCollapse && ks.uHeld )
        {
            // A held note ends; its voice ends only when no more notes are held than voices remain
            ks.uHeld--;
            if ( ks.uHeld < ks.uVoices )
            {
                ks.uVoices--;
                if ( bLane ) ks.uSentOffs++;
                pOut[n++] = uBase | ( ( k & 0x7F ) << 8 );
            }
        }
        else
        {
            if ( bLane ) ks.uSentOffs++;
            pOut[n++] = uBase | ( ( k & 0x7F ) << 8 );
        }
        if ( k & 0x80 ) { bEnd = true; break; }
    }
    rn = n;
    return i;
}

template < bool bCollapse, bool bCounting, bool bLane >
bool PlaybackDecoder::FillTick( uint32_t *__restrict pOut, size_t iCap, size_t &rn, size_t &rRead, size_t iMaxRead, size_t u, uint32_t uMinVel,
                                const Params &p )
{
    size_t n = rn, iRead = rRead;
    // Events that can be read before the caps are looked at again (each makes at most one message)
    size_t iRoom = std::min( iCap - n, iMaxRead - iRead );
    bool bReached = m_Cursor.Scan(
        u + 1,
        [&]( uint32_t uTrack, uint32_t uBase, const uint8_t *pk, size_t iAvail, bool &bEnd, bool &bStop ) -> size_t {
            size_t i = Notes< bCollapse, bCounting, bLane >( uTrack, uBase, pk, std::min( iAvail, iRoom ), bEnd, pOut, n, u, uMinVel, p );
            iRead += i;
            if ( !( iRoom -= i ) )
            {
                iRoom = std::min( iCap - n, iMaxRead - iRead );
                bStop = !iRoom;
            }
            return i;
        },
        [&]( uint32_t, uint32_t uMsg ) {
            if ( ( uMsg & 0xF0 ) != 0xA0 || !( uMsg & 0x8000 ) || ( p.bTuning31 && ( uMsg & 0x0F ) != 9 ) )
                pOut[n++] = uMsg;
            iRead++;
            if ( bCounting ) m_dTokens -= 1.0;
            if ( --iRoom ) return true;
            iRoom = std::min( iCap - n, iMaxRead - iRead );
            return iRoom != 0;
        } );
    rn = n;
    rRead = iRead;
    return bReached;
}

const PlaybackDecoder::FillTickFn PlaybackDecoder::s_aFillTick[8] = {
    &PlaybackDecoder::FillTick< false, false, false >, &PlaybackDecoder::FillTick< false, false, true >,
    &PlaybackDecoder::FillTick< false, true, false >,  &PlaybackDecoder::FillTick< false, true, true >,
    &PlaybackDecoder::FillTick< true, false, false >,  &PlaybackDecoder::FillTick< true, false, true >,
    &PlaybackDecoder::FillTick< true, true, false >,   &PlaybackDecoder::FillTick< true, true, true >,
};

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
        size_t u = m_uCachedTick;
        if ( bBudget && m_bAtTickStart ) BudgetTick( tE, p );
        const bool bCounting = bBudget && m_bScout;

        m_bAtTickStart = false;
        if ( n < iCap && iRead < iMaxRead )
            m_bAtTickStart = ( this->*s_aFillTick[p.bCollapse * 4 + bCounting * 2 + m_bLane] )( pOut, iCap, n, iRead, iMaxRead, u, uMinVel, p );
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
    // Straight into the output; with collapsing, into a buffer that stays in cache instead (a voice's
    // velocity is raised where it was written, anywhere in the call's output)
    uint32_t aBuf[8192];
    for ( ;; )
    {
        long long llDue;
        size_t n0 = out.vMsgs.size(), n;
        if ( p.bCollapse )
        {
            n = Fill( aBuf, 8192, LLONG_MAX - 1, llNow, p, llDue );
            out.vMsgs.insert( out.vMsgs.end(), aBuf, aBuf + n );
        }
        else
        {
            out.vMsgs.resize( n0 + 8192 );
            n = Fill( out.vMsgs.data() + n0, 8192, LLONG_MAX - 1, llNow, p, llDue );
            out.vMsgs.resize( n0 + n );
        }
        if ( n ) out.vGroups.push_back( { (uint32_t)n, llDue } );
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
    // Offs take the fast path (uncounted) only while nothing can be skipped
    m_bLaneFast = !( p.bMute || p.bVelocitySkip || p.llMaxSendsPerSec > 0 || p.iMinVelocity > 0 );
    m_llFastOffs = 0;
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
    std::vector< uint64_t > vOwed( 4096 ), vVoices( 4096 );
    std::vector< size_t > vVoiceTick( 4096 );
    for ( int i = 0; i < 4096; i++ )
    {
        const KeyState &ks = m_aKey[i];
        vOwed[i] = ks.uPending + ( ks.uHeld - ks.uVoices );
        vVoices[i] = ks.uVoices;
        vVoiceTick[i] = ks.uVoiceTick;
    }
    // Each later lane owes, per key, the first of the offs it sent, as many as are owed then
    std::vector< uint32_t > vQuota( (size_t)iLanes * 4096 );
    std::vector< uint64_t > vLeft( iLanes );   // offs each lane drops
    // A lane that sent offs on the fast path did not count them. Its quota needs them only up to what
    // can be owed by then (carried in, plus what the lanes before it left owing), so they are counted
    // again from its output up to that, until every such key has reached it
    bool bCount = false;
    {
        std::vector< uint64_t > vMax( vOwed );
        for ( int k = 1; k < iLanes; k++ )
        {
            PlaybackDecoder &L = *aLanes[k];
            uint32_t *pBound = &vQuota[(size_t)k * 4096];
            for ( int i = 0; i < 4096; i++ )
            {
                const KeyState &ks = L.m_aKey[i];
                if ( L.m_llFastOffs && vMax[i] ) { pBound[i] = (uint32_t)std::min< uint64_t >( vMax[i], UINT32_MAX ); bCount = true; }
                vMax[i] += ks.uPending + ( ks.uHeld - ks.uVoices );
            }
        }
    }
    if ( bCount )
        parallel( [&]( int k ) {
            PlaybackDecoder &L = *aLanes[k];
            uint32_t *pBound = &vQuota[(size_t)k * 4096];
            size_t iKeys = 0;
            for ( int i = 0; i < 4096; i++ ) iKeys += pBound[i] != 0;
            if ( !L.m_llFastOffs || !iKeys ) return;
            for ( KeyState &ks : L.m_aKey ) ks.uSentOffs = 0;
            for ( uint32_t m : aOut[k].vMsgs )
            {
                size_t i = ( ( m & 0x0F ) << 8 ) | ( ( m >> 8 ) & 0xFF );
                if ( IsOff( m ) && pBound[i] && ++L.m_aKey[i].uSentOffs == pBound[i] )
                {
                    pBound[i] = 0;
                    if ( !--iKeys ) break;
                }
            }
            for ( int i = 0; i < 4096; i++ ) pBound[i] = 0;
        } );
    // The bucket at the batch's end is the last lane's (with what read anything): earlier lanes'
    // leftovers belong to earlier song time, and spending them later could exceed the budget
    double dTokens = m_dTokens;
    uint64_t llDropped = 0;
    for ( int k = 1; k < iLanes; k++ )
    {
        PlaybackDecoder &L = *aLanes[k];
        uint32_t *pQuota = &vQuota[(size_t)k * 4096];
        for ( int i = 0; i < 4096; i++ )
        {
            const KeyState &ks = L.m_aKey[i];
            if ( vOwed[i] && ks.uSentOffs )
            {
                uint32_t q = (uint32_t)std::min< uint64_t >( vOwed[i], ks.uSentOffs );
                pQuota[i] = q;
                vOwed[i] -= q;
                vLeft[k] += q;
            }
            // What the lane leaves owing and sounding adds to what was carried in
            vOwed[i] += ks.uPending + ( ks.uHeld - ks.uVoices );
            vVoices[i] = std::min< uint64_t >( vVoices[i] + ks.uVoices, 1u << 30 );
            if ( ks.uVoiceTick != SIZE_MAX ) vVoiceTick[i] = ks.uVoiceTick;
        }
        llDropped += vLeft[k];
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
            uint64_t q = vLeft[k];
            if ( !q ) return;
            // The lane's first offs of each owing key, in order, dropped here on the lane's thread; once
            // all are, the rest moves down as it is
            uint32_t *pQuota = &vQuota[(size_t)k * 4096];
            LaneOut &o = aOut[k];
            uint32_t *pm = o.vMsgs.data();
            size_t r = 0, w = 0, g = 0, G = o.vGroups.size(), N = o.vMsgs.size();
            for ( size_t gi = 0; gi < G; gi++ )
            {
                const auto grp = o.vGroups[gi];
                size_t w0 = w, rEnd = r + grp.first;
                for ( ; r < rEnd && q; r++ )
                {
                    uint32_t m = pm[r];
                    if ( IsOff( m ) )
                    {
                        uint32_t &c = pQuota[( ( m & 0x0F ) << 8 ) | ( ( m >> 8 ) & 0xFF )];
                        if ( c ) { c--; q--; continue; }
                    }
                    pm[w++] = m;
                }
                if ( !q )
                {
                    size_t iInGroup = w - w0 + ( rEnd - r );
                    memmove( pm + w, pm + r, ( N - r ) * sizeof( uint32_t ) );
                    w += N - r;
                    if ( iInGroup ) o.vGroups[g++] = { (uint32_t)iInGroup, grp.second };
                    for ( size_t gj = gi + 1; gj < G; gj++ ) o.vGroups[g++] = o.vGroups[gj];
                    break;
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
    for ( int i = 0; i < 4096; i++ )
    {
        KeyState &ks = m_aKey[i];
        ks.uPending = (uint32_t)std::min< uint64_t >( vOwed[i], UINT32_MAX );
        ks.uHeld = ks.uVoices = (uint32_t)vVoices[i];
        ks.uVoiceTick = vVoiceTick[i];
        ks.uCursorTick = SIZE_MAX;
    }
    m_llPending = 0;
    for ( const KeyState &ks : m_aKey ) m_llPending += ks.uPending;
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
