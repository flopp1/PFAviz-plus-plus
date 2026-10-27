// The playback decoder (DESIGN.md 7.2, 7.5, 7.7, 7.8): turns the event stream into timed batches of
// short messages, applying mute, the velocity correction, stack collapsing, the send budget, velocity
// skipping, the seek chase and the metronome. It has no threads and no clock of its own, so the harness can drive it directly;
// PlaybackEngine runs it on the decoder thread.
#pragma once

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
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
        // When an off of that tick ends the key's last voice (a zero-length note), the next note-on of
        // the tick is sent too and sounds the key again.
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
    // AllNotesOff and the chase (Chase) to vOut. llPrevTime is the playback time before the jump
    // (unused: the chase does not depend on where playback was).
    void Seek( long long llTarget, long long llPrevTime, std::vector< uint32_t > &vOut );

    // Time of the next message (event, or metronome click when it is on), LLONG_MAX at the end
    long long NextTime( int iMetronome );
    // Appends up to iCap messages whose time is <= llHorizon, all within 0.5 ms (x speed) of the
    // first one, whose time is returned in llDue. llNow is the current playback time (for lag).
    // It reads at most 256 k events per call, so it can return fewer (even none) with
    // llDue <= llHorizon: call again.
    size_t Fill( uint32_t *pOut, size_t iCap, long long llHorizon, long long llNow, const Params &p, long long &llDue );

    // Parallel decoding (DESIGN.md 7.11), on demand when one thread cannot keep up: a batch of whole
    // ticks from the cursor, split by stream bytes into lanes. Lane 0 is this decoder, with its state;
    // each other lane is a decoder of its own (RunLane), from no state, starting at the first group at or
    // after its cut (inside a tick, for a tick too big for one lane). FinishBatch stitches the lanes'
    // messages in order: each channel and key's note-offs owed to note-ons not sent (skipped, muted or
    // held by collapsing) are dropped from the first offs the later lanes sent, so no note sounds
    // longer than it would have. Only the send budget's sharing and collapsing within a split tick
    // differ from one thread's decoding; with neither on, the messages are the same.
    struct Batch
    {
        size_t uEnd = 0;                    // the batch ends at this tick
        std::vector< uint64_t > vCut;       // lane k: from the first group at or after vCut[k] to vCut[k + 1] (raw stream bytes)
        long long llNow = 0;                // the playback time (for the lag)
        double dTokens = 0.0;               // the send budget's tokens at the start
    };
    // Leaves elements uninitialised on resize, so a lane decodes straight into its output
    template < class T > struct NoInit : std::allocator< T >
    {
        template < class U > struct rebind { using other = NoInit< U >; };
        template < class U, class... A > void construct( U *p, A &&...a ) { ::new ( (void *)p ) U( std::forward< A >( a )... ); }
        template < class U > void construct( U *p ) noexcept { ::new ( (void *)p ) U; }
    };
    struct LaneOut
    {
        std::vector< uint32_t, NoInit< uint32_t > > vMsgs;
        std::vector< std::pair< uint32_t, long long > > vGroups;    // messages and the time they are due, as Fill returns them
        void Clear() { vMsgs.clear(); vGroups.clear(); }
    };
    // Plans a batch of iLanes lanes up to the first tick after llHorizon and about llMaxBytes of the
    // stream (at least the rest of the cursor's tick); false when nothing is due by llHorizon
    bool PlanBatch( int iLanes, long long llHorizon, uint64_t llMaxBytes, long long llNow, Batch &b );
    void RunMainLane( const Batch &b, const Params &p, LaneOut &out );                  // lane 0, on this decoder
    void RunLane( const Batch &b, int k, const Params &p, LaneOut &out );               // lane k > 0, on its own decoder
    // Stitches the lanes (aLanes[k] ran lane k; aLanes[0] is unused) and moves this decoder to the
    // batch's end; emit( msgs, n, due ) gets the messages in order. parallel( fn ) runs fn( k ) for
    // every lane k > 0 at once (each lane drops its owed offs itself).
    void FinishBatch( const Batch &b, PlaybackDecoder *const *aLanes, LaneOut *aOut, int iLanes,
                      const std::function< void( const std::function< void( int ) > & ) > &parallel,
                      const std::function< void( const uint32_t *, size_t, long long ) > &emit );
    uint64_t EventsRead() const { return m_llRead; }

    const Counters &GetCounters() const { return m_Counters; }
    int SkipVelocity() const { return m_iSkipVelocity; }  // the last threshold (0: not skipping)
    BudgetCut GetBudgetCut() const { return m_Cut; }
    static int VelocityThreshold( long long llLagUs, long long llSkipLagUs );
    // The cut that keeps the loudest of aWindow's note-ons (by velocity, from iFloor up) within dAllowance of them
    static BudgetCut BudgetThreshold( const uint64_t aWindow[128], double dAllowance, int iFloor = 1 );
    static const long long BudgetWindowUs = 50000;     // the look-ahead and the token bucket's depth, real time
    // The token bucket's depth: BudgetWindowUs of the budget, and at least two note-ons
    static double BudgetDepth( long long llMaxSendsPerSec ) { return std::max( 4.0, llMaxSendsPerSec * (double)BudgetWindowUs / 1e6 ); }

private:
    struct Signature { long long llTick, iBeatsPerMeasure, iBeatType; };

    long long EventTime();
    void ResetSendState();
    void Chase( long long llTarget, std::vector< uint32_t > &vOut );
    void BudgetTick( long long tE, const Params &p );
    bool BudgetKeep( uint32_t uVel );
    const Signature &SignatureAt( long long llTick ) const;
    long long GetBeat( long long iTick, const Signature &s ) const;
    long long GetBeatTick( long long iTick ) const;
    long long CurrentTick( long long llTime ) const;
    void SetBeat( long long llBeatTick );
    void FillLane( const Params &p, long long llNow, LaneOut &out );
    // A NoteOff, or a NoteOn of velocity 0
    static bool IsOff( uint32_t m ) { return ( ( m & 0xF0 ) == 0x80 ) | ( ( m & 0x7F00F0 ) == 0x90 ); }
    // Fill's decoding of the cursor's events up to the end of tick u, one instance per mode; true when
    // it got there
    template < bool bCollapse, bool bCounting, bool bLane >
    bool FillTick( uint32_t *__restrict pOut, size_t iCap, size_t &rn, size_t &rRead, size_t iMaxRead, size_t u, uint32_t uMinVel, const Params &p );
    // Keys of one note run (one track, channel, status and velocity), so what holds for the whole run
    // is decided once: up to the key with bit 7 set (the run's last, then bEnd) or iMax keys (each
    // makes at most one message). Returns the keys taken.
    template < bool bCollapse, bool bCounting, bool bLane >
    size_t Notes( uint32_t uTrack, uint32_t uBase, const uint8_t *pk, size_t iMax, bool &bEnd, uint32_t *__restrict pOut, size_t &rn, size_t u,
                  uint32_t uMinVel, const Params &p );
    using FillTickFn = bool ( PlaybackDecoder::* )( uint32_t *, size_t, size_t &, size_t &, size_t, size_t, uint32_t, const Params & );
    static const FillTickFn s_aFillTick[8];

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

    // Per channel and key, in one record so a note touches one cache line:
    //   - uPending: the note-offs still to drop for note-ons not sent (skipped or muted);
    //   - stack collapsing: notes held and voices sent (sent <= held), the tick of the last voice, and
    //     where it is in this call's output (to raise its velocity);
    //   - the send budget: with collapsing, only a key's first note-on of a tick counts (uCursorTick)
    struct KeyState
    {
        size_t uVoiceTick = SIZE_MAX, uCursorTick = SIZE_MAX;
        uint32_t uPending = 0, uHeld = 0, uVoices = 0, uVoiceOut = 0, uVoiceCall = 0;
        uint32_t uSentOffs = 0;         // offs a lane sent, for the stitch (FinishBatch counts them again after any on the fast path)
    };
    KeyState m_aKey[2048];
    uint64_t m_llPending = 0;           // the keys' uPending summed: while 0, an off needs no key state (without collapsing)
    int m_iSkipVelocity = 0;
    bool m_bCollapse = false;
    uint32_t m_uCall = 1;

    // Send budget (DESIGN.md 7.5): the note-ons of the next BudgetWindowUs (real time) by velocity, from
    // the song's precomputed bins (Song::aSendOns), so a burst is cut by velocity before it starts; a
    // token bucket is the hard cap. The window is what is left of the cursor's bin (m_aRem: its counts
    // less what the cursor has read) plus the whole bins after it up to m_uNextBin; leaving a bin drops
    // whatever of it is left, so the window is exact again from each bin on.
    bool m_bScout = false;              // the window is in step with the cursor (synced at a tick start)
    bool m_bAtTickStart = true;         // the cursor is at the first event of its tick
    bool m_bScoutCollapse = false;      // the counting rule the window was built with (collapsing)
    uint64_t m_aWindow[128] = {}, m_aRem[128] = {};
    size_t m_uWinBin = 0, m_uNextBin = 0;
    double m_dTokens = 0.0;
    long long m_llTokenTime = LLONG_MIN;
    BudgetCut m_Cut;
    double m_dKeepAcc = 0.0;
    // A lane of a parallel batch: the budget's cut comes from the bins alone (with the batch's tokens),
    // and the lane's own tokens are what it gains over its stretch of song time
    bool m_bLane = false;
    bool m_bLaneFast = false;           // a lane's offs may take the fast path (nothing can be skipped)
    uint64_t m_llFastOffs = 0;          // the offs a lane sent on it (not counted per key)
    double m_dLaneTokens = 0.0;
    double m_dDepth = 0.0;              // the bucket's depth at the last tick

    Counters m_Counters;
    uint64_t m_llRead = 0;              // events read by Fill
};
