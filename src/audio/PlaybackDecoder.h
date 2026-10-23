// The playback decoder (DESIGN.md 7.2, 7.5, 7.7, 7.8): turns the event stream into timed batches of
// short messages, applying mute, the velocity correction, stack collapsing, the send budget, velocity
// skipping, the seek chase and the metronome. It has no threads and no clock of its own, so the harness can drive it directly;
// PlaybackEngine runs it on the decoder thread.
#pragma once

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <vector>

#include "song/Song.h"
#include "song/StreamCursor.h"
#include "song/TempoMap.h"

class PlaybackDecoder
{
public:
    enum Metronome { MetOff = 0, MetEveryBeat = 1, MetEveryMeasure = 2 };      // PlaybackSettings::Metronome
    struct Params
    {
        bool bMute = false;
        double dVolumeCorrect = 1.0;
        int iMetronome = MetOff;
        // Velocity skipping (off: every event is sent). Above llSkipLagUs of lag (song time), note-ons
        // quieter than a threshold are skipped; it rises from velocity 1 at that lag to every note at
        // 4x that lag (VelocityThreshold)
        bool bVelocitySkip = false;
        long long llSkipLagUs = 100000;
        double dSpeed = 1.0;
        // Feedback from the sender
        long long llSenderLagUs = 0;
        // Stack collapsing: of the note-ons of one channel and key at one tick, only one is sent (the
        // loudest of those in the same batch); the others are held silently. Offs are dropped while
        // more notes are held than voices were sent, so each voice ends with the last note it stands for.
        bool bCollapse = false;
        // Send budget: at most this many messages per second of real time (0: no limit). Note-ons
        // are skipped quietest first to fit (each counts with its off); see BudgetThreshold.
        long long llMaxSendsPerSec = 0;
        // Velocity floor: note-ons quieter than this are always skipped, with their offs (0: none)
        int iMinVelocity = 0;
    };

    struct Counters
    {
        uint64_t llSent = 0, llNoteOns = 0, llSkipped = 0, llCollapsed = 0;
    };
    // The send budget's current cut: note-ons below iVelocity are skipped, those at iVelocity are kept
    // at the rate dKeep (iVelocity 0: nothing skipped)
    struct BudgetCut { int iVelocity = 0; double dKeep = 1.0; };

    explicit PlaybackDecoder( const Song &song );

    // Mute bits per track (bit c = channel c); may be changed by another thread at any time
    void SetMuteMask( const std::atomic< uint16_t > *pMask ) { m_pMuteMask = pMask; }

    // MainScreen::InitState: the cursor goes to the first event, nothing is chased
    void Reset( long long llStartTime );
    // MainScreen::JumpTo: the cursor goes to the first event with time >= llTarget. Appends
    // AllNotesOff and the chased controllers and programs (PlaySkippedEvents) to vOut.
    // llPrevTime is the playback time before the jump (the old position of the chase iterator).
    void Seek( long long llTarget, long long llPrevTime, std::vector< uint32_t > &vOut );

    // Time of the next message (event, or metronome click when it is on), LLONG_MAX at the end
    long long NextTime( int iMetronome );
    // Appends up to iCap messages whose time is <= llHorizon, all within 0.5 ms (x speed) of the
    // first one, whose time is returned in llDue. llNow is the current playback time (for lag).
    // It reads at most 256 k events per call, so it can return fewer (even none) with
    // llDue <= llHorizon: call again.
    size_t Fill( uint32_t *pOut, size_t iCap, long long llHorizon, long long llNow, const Params &p, long long &llDue );

    const Counters &GetCounters() const { return m_Counters; }
    int SkipVelocity() const { return m_iSkipVelocity; }  // the last threshold (0: not skipping)
    BudgetCut GetBudgetCut() const { return m_Cut; }
    static int VelocityThreshold( long long llLagUs, long long llSkipLagUs );
    // The cut that keeps the loudest of aWindow's note-ons (by velocity) within dAllowance of them
    static BudgetCut BudgetThreshold( const uint64_t aWindow[128], double dAllowance );
    static const long long BudgetWindowUs = 50000;     // the scout's look-ahead and the token bucket's depth, real time
    // The token bucket's depth: BudgetWindowUs of the budget, and at least two note-ons
    static double BudgetDepth( long long llMaxSendsPerSec ) { return std::max( 4.0, llMaxSendsPerSec * (double)BudgetWindowUs / 1e6 ); }

private:
    struct Signature { long long llTick, iBeatsPerMeasure, iBeatType; };

    long long EventTime();
    void ResetSendState();
    void BudgetTick( long long tE, const Params &p );
    bool BudgetKeep( uint32_t uVel );
    const Signature &SignatureAt( long long llTick ) const;
    long long GetBeat( long long iTick, const Signature &s ) const;
    long long GetBeatTick( long long iTick ) const;
    long long CurrentTick( long long llTime ) const;
    void SetBeat( long long llBeatTick );

    const Song &m_Song;
    const EventStream &m_Stream;
    StreamCursor m_Cursor;
    TempoMap m_Tempo;
    std::vector< Signature > m_vSignatures;
    const std::atomic< uint16_t > *m_pMuteMask = nullptr;

    // Cached time of the cursor's next unique tick
    size_t m_uCachedTick = SIZE_MAX;
    long long m_llCachedTime = LLONG_MAX;

    // Velocity correction
    double m_dLutCorrect = -1.0;
    uint8_t m_aVelLut[128] = {};

    // Metronome
    bool m_bMetronome = false;          // ticks are musical (not SMPTE)
    long long m_llNextBeatTick = 0, m_llNextBeatTime = LLONG_MAX;
    int m_iLastMetronomeNote = 76;
    int m_iMetronome = MetOff;

    // Per channel and key, the note-offs still to drop for note-ons not sent (skipped or muted)
    uint32_t m_aPendingSkip[2048] = {};
    int m_iSkipVelocity = 0;

    // Stack collapsing, per channel and key: notes held and voices sent (sent <= held), the tick of
    // the last voice, and where it is in this call's output (to raise its velocity)
    bool m_bCollapse = false;
    uint32_t m_aHeld[2048] = {}, m_aVoices[2048] = {};
    size_t m_aVoiceTick[2048];
    uint32_t m_aVoiceOut[2048] = {}, m_aVoiceCall[2048] = {};
    uint32_t m_uCall = 1;

    // Send budget (DESIGN.md 7.5): a scout cursor counts the note-ons of the next BudgetWindowUs (real
    // time) by velocity, so a burst is cut by velocity before it starts; a token bucket is the hard cap
    StreamCursor m_Scout;
    bool m_bScout = false;              // the scout is in step with the cursor (synced at a tick start)
    bool m_bAtTickStart = true;         // the cursor is at the first event of its tick
    bool m_bScoutCollapse = false;      // the counting rule the window was built with (collapsing, floor)
    uint32_t m_uScoutFloor = 0;
    uint64_t m_aWindow[128] = {};       // note-ons from the cursor to the scout, by velocity
    size_t m_aScoutTick[2048], m_aCursorTick[2048];     // with collapsing, only a key's first note-on of a tick counts
    double m_dTokens = 0.0;
    long long m_llTokenTime = LLONG_MIN;
    BudgetCut m_Cut;
    double m_dKeepAcc = 0.0;

    Counters m_Counters;
};
