// A model of how long the original Piano From Above (1.1.0) takes per frame for a song, so the app can
// play it the way the original would lag on this computer (MainScreen: the PFA lag simulation).
//
// The original runs Logic then Render on one thread, its clock being the time that passed (no cap):
//   - Logic goes through every event since the last frame in order: it sends it with midiOutShortMsg
//     and updates the notes sounding (UpdateState, a vector of them). A note-on is pushed; a note-off
//     walks the whole vector, reading each sounding note's event object, and erases its own. So a
//     frame's Logic costs, besides the events and their sends, the sum over its note-offs of the
//     polyphony at that moment: a constant 10,000 notes held cost nothing more while they are held,
//     but renewed by new notes, each ending one walks all 10,000. How fast a walk goes depends on
//     whether those objects fit in the CPU's caches.
//   - Render walks the sounding notes and every event in view, twice (white keys, then black), and
//     draws each note as two rects, flushing the vertex buffer (a lock, a draw call) every 50 notes.
// A frame that takes longer lets more song time pass before the next, whose Logic has more to do:
// once a second of the song takes more than a second, each frame is longer than the last (the
// original's freeze). The music is sent as Logic reaches each event, so it holds at a heavy stretch
// for as long as its walks take and then races through the light song time after it, then stops for
// the frame's Render: during a freeze, a burst of music, a gap, the next frame.
//
// The work is counted per millisecond of song with its note statistics (Song::noteStats, ccCounts):
// each note-off walks the polyphony of its millisecond. The costs were measured from stock PFA 1.1.0
// itself (PfaLag.cpp). Noise (optional) adds what the rest of the computer does to it: other programs
// taking part of the caches, the frame's own spread, a message's send cost varying, the odd stall.
#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <vector>

struct Song;

namespace PfaLag
{
    // The CPU's data caches (bytes; per core for L1 and L2, the largest for L3)
    struct Cpu
    {
        uint64_t llL1 = 32 << 10, llL2 = 512 << 10, llL3 = 16 << 20;
        // How much longer the original's work takes on this CPU than on the one its costs were
        // measured on (a Ryzen 5 5500): its walks at each of the five working-set sizes (the caches'
        // speed and the memory's), and its other work (the core's clock and how much it does a clock).
        // 1: the same. From Bench (ApplyBench), and the speed setting (Faster)
        double aMem[5] = { 1.0, 1.0, 1.0, 1.0, 1.0 };
        double dCore = 1.0;
        static Cpu Detect();
        void Faster( double dTimes );       // a CPU dTimes as fast (0.5: half as fast)
    };

    // A short benchmark of the original's work on this CPU (about half a second, once per run): its
    // note-off walk (a list of indices into an array of pointers to event objects scattered in
    // memory, each compared and its key read) at four working-set sizes by this CPU's caches (half of
    // L1, of L2 and of L3, and four times L3), and its per-event loop, in ns. bReference: this is the
    // CPU the costs were measured on, which is taken as is
    struct Bench
    {
        double aWalkNs[4] = {}, dEventNs = 0.0;
        char sBrand[49] = {};
        bool bReference = false;
    };
    const Bench &RunBench();
    void ApplyBench( Cpu &cpu, const Bench &b );

    struct Params
    {
        long long llSpanUs = 3000000;       // the song time in view (the note speed)
        // One midiOutShortMsg, the typical cost: 0.05 us with the usual winmm.dll patch that sends to
        // KDMAPI (measured, into OmniMIDI-compatible synths); unpatched, WinMM's own, 13-30 us on Windows
        // 11 builds whose WinMM goes through the new MIDI service (even the GS Wavetable Synth)
        double dSendUs = 0.05;
        bool bMuted = false;                // note-ons are not sent (everything else is)
        bool bPaused = false;               // no Logic work
    };

    // What the rest of the computer does to the frames: slow random processes (cache taken by other
    // programs, the send path's state) moved along with wall time, and draws per frame. A seeded
    // generator: the same seed gives the same run
    class Noise
    {
    public:
        explicit Noise( uint64_t llSeed = 1 );
        void Advance( double dWallUs );     // the slow processes move on by this much wall time
        double Contention() const { return m_dContention; }     // the share of L2 / L3 others use
        double SendState() const { return m_dSendLog; }         // log of the send path's slow factor

        double Uniform();                   // (0, 1)
        double Normal();
        double Exponential( double dMean );
        int Poisson( double dMean );

    private:
        uint64_t m_llState;
        double m_dContention, m_dSendLog;
    };

    // Logic's progress: by dUs after it starts it has sent the song up to llSong
    struct RampPoint
    {
        double dUs;
        long long llSong;
    };
    static const int RampPoints = 129;

    struct FrameCost
    {
        double dLogicUs = 0.0;      // sending and tracking the events since the last frame
        double dRenderUs = 0.0;     // drawing the frame (with presenting it)
        uint64_t llPolyphony = 0;   // sounding at the frame's time
        uint64_t llInView = 0;      // notes drawn
        int nRamp = 0;              // aRamp: from (0, llFrom) to (dLogicUs, llTo), spaced by the work
        RampPoint aRamp[RampPoints];
    };

    // The frame that plays song time (llFrom, llTo] and draws llTo. Needs song.noteStats (Ready).
    // Without noise, the measured costs as they are
    bool Ready( const Song &song );
    FrameCost Cost( const Song &song, const Cpu &cpu, const Params &p, long long llFrom, long long llTo, Noise *pNoise = nullptr );

    // For calibration (pfacheck --pfafeat): the note-off walks of (llFrom, llTo], each millisecond's
    // note-offs times its polyphony, split over the five working-set sizes the costs are given at
    // (PfaLag.cpp) as the cost interpolates between them; and the events there
    void WalkBands( const Song &song, const Cpu &cpu, long long llFrom, long long llTo, double aBands[5], double *pEvents );

    // The song time in (llFrom, llTo] where the notes a second or the polyphony rise suddenly (LLONG_MIN: none;
    // LagTracker): the notes of the RiseAfterUs after a point at least RiseRatio times, and NpsFloor a second
    // more than, those of the RiseBeforeUs before it, or the polyphony RiseAfterUs on at least PolyRatio times,
    // and PolyFloor more than, the most of the RiseBeforeUs before; points every RiseStepUs, the biggest rise
    static constexpr long long RiseBeforeUs = 1000000, RiseAfterUs = 250000, RiseStepUs = 10000;
    static constexpr double RiseRatio = 2.0, NpsFloor = 20000.0, PolyRatio = 1.5, PolyFloor = 2000.0;
    long long RiseIn( const Song &song, long long llFrom, long long llTo );

    // How long the original has been lagging: a frame that takes ThresholdUs or more (under 1 fps) is lag,
    // and a lag spike is a run of such frames with less than ThresholdUs of quicker ones between them. It
    // starts where the slowing down into it does: at the first of the frames of LeadUs or more (under 1.33 fps)
    // that lead straight into its first slow frame. Its duration counts from there without a pause, through
    // the quicker frames between slow ones, to the end of its last slow frame, and stays there until the next
    // spike starts it again from 0. It ends the same way it starts: after the frames of LeadUs or more straight
    // after its last slow frame (the speeding up out of it). With the song given (pSong), a sudden rise in notes
    // a second or polyphony in the RiseBackUs before that start (RiseIn: where the dense part begins, which
    // usually brings the lag on) moves the start back to the frame that played it. The frames are known before they run (worked out ahead), so a spike
    // counts from the moment its first frame starts (when the slow frame it leads to is worked out by then),
    // and ends exactly at its last slow frame's end, given the frames worked out at least ThresholdUs ahead.
    // Wall times in microseconds
    struct LagTracker
    {
        static constexpr long long ThresholdUs = 1000000;
        // The search for the dense part a spike comes from: this much wall time back from where it starts
        static constexpr long long RiseBackUs = 3000000;
        long long LeadUs = 750000;
        const Song *pSong = nullptr;            // (its note statistics: the start moved to a sudden rise; none: not)
        long long llStart = 0, llLastEnd = 0;   // the spike: where it starts, its end (last slow frame's, then its lead-out's)
        long long llRunStart = 0;               // the frames of LeadUs or more in a row up to now: the first's start
        long long llRiseUs = 0;                 // how much earlier the last spike starts for a rise (0: none found)
        bool bAny = false, bRun = false, bOut = false;  // bOut: the run goes on from the spike's last slow frame
        int iSpikes = 0;
        struct Past { long long llWallStart, llWallEnd, llFrom, llTo; };
        std::vector< Past > vPast;              // the frames of the last RiseBackUs and more (song time by wall time)
        // A frame that ended: from llWallStart to llWallEnd, playing song time (llFrom, llTo]
        void Ended( long long llWallStart, long long llWallEnd, long long llFrom, long long llTo )
        {
            if ( pSong )
            {
                vPast.push_back( { llWallStart, llWallEnd, llFrom, llTo } );
                size_t iKeep = 0;
                while ( iKeep + 1 < vPast.size() && vPast[iKeep + 1].llWallStart < llWallStart - RiseBackUs - ThresholdUs ) iKeep++;
                if ( iKeep ) vPast.erase( vPast.begin(), vPast.begin() + (ptrdiff_t)iKeep );
            }
            const long long d = llWallEnd - llWallStart;
            if ( d < LeadUs ) { bRun = bOut = false; return; }
            if ( !bRun ) { bRun = true; llRunStart = llWallStart; }
            // (the speeding up out of it: frames of LeadUs or more straight after its last slow one)
            if ( d < ThresholdUs ) { if ( bOut ) llLastEnd = std::max( llLastEnd, llWallEnd ); return; }
            if ( !bAny || llWallStart - llLastEnd >= ThresholdUs )
            {
                llStart = std::min( llRunStart, llWallStart );
                iSpikes++;
                llRiseUs = 0;
                // A sudden rise in notes a second or polyphony shortly before: it starts there (the dense part)
                if ( pSong && !vPast.empty() )
                {
                    const Past *pAt = nullptr, *pBack = &vPast.front();
                    for ( const Past &p : vPast )
                    {
                        if ( !pAt && p.llWallStart >= llStart ) pAt = &p;
                        if ( p.llWallStart <= llStart - RiseBackUs ) pBack = &p;
                    }
                    if ( pAt && pBack->llFrom < pAt->llFrom )
                    {
                        const long long llRise = RiseIn( *pSong, pBack->llFrom, pAt->llFrom );
                        if ( llRise != LLONG_MIN )
                            for ( const Past &p : vPast )
                                if ( llRise <= p.llTo )
                                {
                                    if ( p.llWallStart < llStart ) { llRiseUs = llStart - p.llWallStart; llStart = p.llWallStart; }
                                    break;
                                }
                    }
                }
            }
            bAny = bOut = true;
            llLastEnd = std::max( llLastEnd, llWallEnd );
        }
        // The duration at llNow, given the frames not ended yet (the running one and those worked out ahead, in
        // order: n of wall start, wall end, song from, song to at pFrames); bLagging: in a spike
        long long Duration( long long llNow, const long long *pFrames, size_t n, bool *pbLagging = nullptr ) const
        {
            LagTracker t = *this;
            for ( size_t i = 0; i < n; i++ )
            {
                const long long *f = pFrames + 4 * i;
                if ( f[0] <= llNow ) { t.Ended( f[0], f[1], f[2], f[3] ); continue; }
                // (ahead: only what goes on the spike under way, or on the slowing down under way now)
                const bool bGoesOn = t.bAny && f[0] - t.llLastEnd < ThresholdUs, bLeadIn = t.bRun && t.llRunStart <= llNow;
                if ( !bGoesOn && !bLeadIn ) break;
                t.Ended( f[0], f[1], f[2], f[3] );
            }
            const bool bLag = t.bAny && llNow >= t.llStart && llNow < t.llLastEnd;
            if ( pbLagging ) *pbLagging = bLag;
            return !t.bAny ? 0 : bLag ? llNow - t.llStart : t.Length();
        }
        long long Length() const { return llLastEnd - llStart; }    // (the last spike's)
    };
}
