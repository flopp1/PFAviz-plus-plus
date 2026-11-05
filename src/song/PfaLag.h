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

#include <cstdint>

struct Song;

namespace PfaLag
{
    // The CPU's data caches (bytes; per core for L1 and L2, the largest for L3)
    struct Cpu
    {
        uint64_t llL1 = 32 << 10, llL2 = 512 << 10, llL3 = 16 << 20;
        static Cpu Detect();
    };

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
    static const int RampPoints = 33;

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
}
