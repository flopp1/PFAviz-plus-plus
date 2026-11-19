// Audio rendered ahead (Preferences > Audio > Pre-Render): instead of sending MIDI to a synth as the song
// plays, the song is made into audio in the background, ahead of the playback position, and played
// from there through the sound device (AudioOut), so even the densest passages play every note.
//
// The song is cut into blocks of one second of audio on its own clock (at the current speed: a change
// of speed starts again). As it plays, BASSMIDI and Kestrel make them one after another with a synth kept
// from block to block (their continuous render, StreamBlock: started at a block as a piece is, with its
// preroll and chase, then fed the events as the audio is made). Otherwise (Merlin, the video's audio, and
// blocks made again in full) a piece is a run of blocks made in one go: one block next to the playback
// position (a quick start after a seek), up to RunBlocks further ahead. Several pieces are made at once
// on the CPU (BASSMIDI), one at a time on the graphics card (Kestrel, Merlin). A piece is written as a
// small MIDI file in memory and handed to the synth (RenderBackends):
//   - It starts PrerollSecs early, and that part is thrown away, so notes and reverb already ringing
//     sound right. A note still held there moves the start back to its own start (at most HeldSecs);
//     notes held from even earlier are struck again at the start, out of hearing.
//   - Its first event is the seek chase: every controller, program and bend as they are there.
//   - Its events are the playback decoder's (mute, the velocity correction and the velocity floor as
//     set), each at its exact time; nothing is skipped or collapsed.
//   - It runs CrossfadeFrames past its end; the next piece fades in over them, so the joins are smooth.
// Blocks further than RamAheadSecs ahead are kept in a temporary file once more than those are made, and
// read back before they play, so a long buffer costs disk space, not memory. What was played is thrown
// away at once (BehindBlocks), and so is the old engine's audio on a switch of engine.
// Offline (the video renderer), it has no sound device: RenderOffline makes the audio from one time
// to another in order, waiting for each block, with the same joins, gain and limiter.
//
// A change that alters the sound (the velocity floor, collapsing, 31-EDO) makes everything
// again from the playback position on; until then the old audio plays. The volume is a gain on the
// output (squared, as velocity is), so moving it does not make anything again. A limiter keeps the
// joined audio under full scale.
#pragma once

#include <windows.h>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/AudioOut.h"
#include "audio/Clock.h"
#include "audio/PlaybackDecoder.h"
#include "audio/RenderBackends.h"
#include "song/Song.h"

class RenderAhead
{
public:
    struct Settings
    {
        bool bOn = false;
        int eEngine = RenderBackends::EngineBass;
        std::vector< std::wstring > vFonts;
        int iBassVoices = 1000, iKestrelVoices = 100000;
        int iBassThreads = 1;           // BASSMIDI instances a piece is split over (by key), each with iBassVoices
        bool bBassByChannel = false;    // ... and by channel: 16 x iBassThreads instances (OmniMIDI's delegation)
        int iAheadSecs = 30;            // (MinAheadSecs to MaxAheadSecs)
        // BASSMIDI: each instance takes at most this many notes a second (0: no limit), the quietest let go
        // first as it nears it (OmniMIDI's limiter, an instance's own, as BASSMIDI synths limit each instance)
        int iBassNps = 0;
        bool bLimiter = true, bFx = true;
        RenderBackends::KestrelOpts kb;
        // Kestrel and Merlin: once this much is made ahead, they rest between pieces so they keep the
        // graphics card at most iGpuShare percent of the time (the rest is the picture's)
        int iGpuShare = 50;
        // Stacks of identical notes (channel, key, velocity, tick) sent once with their count: Merlin
        // reads Spiderweb's repeat marks; Kestrel merges such copies into one voice at their summed
        // loudness itself (its coalescing, on for every piece). Lossless; BASSMIDI gets every note
        bool bLossless = true;
        // When the audio would not be made in time it is made lighter: BASSMIDI's continuous render thins
        // out the quiet note-ons as it goes (just in time, by how far ahead it is and how fast it makes
        // audio; never every note of a velocity), Kestrel lowers its voice cap, a piece that would be late
        // skips its quietest notes (BASSMIDI) or goes to Kestrel with a voice cap (Merlin). Made again in
        // full once well ahead
        bool bAutoDegrade = true;
        // Offline (the video's audio): Kestrel and Merlin make the song in their own whole renders, a piece of
        // up to WholeRunBlocks at a time, one after another (no continuous render, no short pieces);
        // BASSMIDI as before
        bool bWhole = false;
        static const int GpuCalmSecs = 5;
        // How far ahead it can be made: past any usual song (most are under ten minutes, a few an hour)
        static const int MinAheadSecs = 5, MaxAheadSecs = 12 * 3600;
        bool operator==( const Settings & ) const = default;
    };
    static Settings FromConfig();
    // The saved settings' soundfonts loaded in the background now, when audio rendered ahead is on (at
    // start-up, and when the settings change), so a song plays at once
    static void PreloadFromConfig();
    // What of the song Merlin does not do properly yet (Kestrel's README: channel volume and expression,
    // CC7 and CC11, are not applied properly; many bends render slowly); empty when none of it is there
    static std::wstring MerlinIssues( const Song &song );

    struct Status
    {
        std::wstring sEngine;           // "BASSMIDI", "Kestrel", ...
        std::wstring sState;            // loading, an error, or empty while working
        double dReadySecs = 0.0;        // audio made from the playback position on
        double dAheadSecs = 0.0;        // what it aims for
        int iBusy = 0;                  // pieces being made
        uint64_t llGaps = 0;            // frames played as silence because nothing was made yet
        uint64_t llDegraded = 0;        // pieces made lighter to be in time
        std::wstring sDegrade;          // how much lighter the audio playing is ("off" when auto-degrade is)
        long long llVoices = -1;        // the voices sounding in the audio playing (-1: not known: pieces, or none playing)
        long long llVoiceCap = 0;       // ... the voice limit it was made with, in all (lowered when degraded)
        long long llVoiceMax = 0;       // ... the voice limit as set, in all
        std::wstring sWarning;          // what loading the soundfonts reported, Merlin refusing them, ...
        int iGpuShare = 0;
    };

    RenderAhead( std::shared_ptr< const Song > pSong, PlaybackClock &clock );
    // Offline, at uRate frames a second and the song's own speed (its own clock, no sound device)
    RenderAhead( std::shared_ptr< const Song > pSong, unsigned uRate );
    ~RenderAhead();

    void Start( const Settings &s );
    void Stop();
    // The sound stopped now (the device let go of; Stop, later, finishes the workers)
    void Silence();
    // A change of engine, soundfonts, voices or effects loads and makes everything again; the rest
    // applies at once
    void SetSettings( const Settings &s );
    // What the decoder makes of the song, the same for every engine and as the MIDI output gets it (but
    // the lag's velocity skipping: rendering ahead never lags): changes make the audio again from the
    // playback position on
    struct Sound
    {
        double dVelocityCorrect = 1.0;
        int iMinVelocity = 0;
        bool bCollapse = false;             // stack collapsing
        long long llMaxSendsPerSec = 0;     // the send budget (0: none)
        // 31-EDO (Performance tab): the pieces are tuned as the MIDI output is, by scale tuning (the MIDI
        // Tuning Standard's scale/octave messages, which BASSMIDI and Kestrel read) or by pitch bends
        bool bTuning31 = false, bTuningBend = false;
        bool operator==( const Sound & ) const = default;
    };
    void SetSound( const Sound &snd );
    void SetMute( bool bMute ) { m_bMute.store( bMute, std::memory_order_relaxed ); }
    void SetVolume( double dVolume ) { m_dGain.store( dVolume * dVolume, std::memory_order_relaxed ); }
    Status GetStatus();
    // Held: nothing is made or played, and what was made is thrown away (returns once no piece is being
    // made, so the engines are free for another user); let go, it starts again from the playback position
    void Hold( bool bHold );
    // Offline: the stereo audio from llFromUs to llToUs (song time), in order, to fnWrite (false: stop).
    // *pDone: frames written so far; *pUntil (when given): frames wanted, which may be lowered meanwhile.
    // False when stopped by pCancel or fnWrite, or when the soundfonts did not load (sError says why)
    bool RenderOffline( long long llFromUs, long long llToUs, const std::function< bool( const float *, unsigned ) > &fnWrite,
                        std::atomic< long long > *pDone, const std::atomic< long long > *pUntil, const std::atomic< bool > *pCancel,
                        std::wstring &sError );
    // Frames from one song time to another, offline
    long long OfflineFrames( long long llFromUs, long long llToUs ) const;
    // Offline: called (on RenderOffline's thread) once the soundfonts have loaded and the audio is being made
    void OnOfflineLoaded( std::function< void() > fn ) { m_fnLoaded = std::move( fn ); }

    static const int RunBlocks = 6, LongRunBlocks = 20;
    // (Settings::bWhole: ten minutes, most songs in one piece; a piece's audio is in memory a few times over as it lands)
    static const int WholeRunBlocks = 600;
    // On the graphics card a piece costs about half a second however long it is (Merlin), so even next to
    // the playback position it is two blocks; further on shorter than on the CPU, so the picture waits
    // less behind each
    static const int GpuFirstRunBlocks = 2, GpuRunBlocks = 4, OfflineGpuRunBlocks = 16;
    // Merlin's pieces once far ahead (from LongRunBlocks on): about 0.4 s of each piece's time is the piece's
    // own, so fewer, longer pieces share it
    static const int MerlinLongRunBlocks = 12;
    static const int FirstRunBlocks = 1;
    static constexpr double PrerollSecs = 1.0;
    static constexpr double HeldSecs = 10.0;
    // Made and played this far past the song's last event, so releases and reverb ring out
    static constexpr long long TailSecs = 8;
    static constexpr double PostSecs = 0.3;  // (events past a piece's end: Merlin's look-ahead; Spiderweb measured 0.3 s as a whole render)
    // Blocks kept in memory around the playback position, at most (the rest on disk): ahead and behind as
    // far as the buffer goes, up to these
    static const int RamAheadSecs = 600;
    static const int BehindBlocks = 1;      // kept behind the block playing (its overrun: the next piece's fade-in)

private:
    // The temporary file blocks are kept in (one per timebase: slots of one block's audio and overrun),
    // deleted when closed. A slot is freed when the last block using it goes
    class SpillFile
    {
    public:
        explicit SpillFile( size_t nSlotFloats ) : m_nSlotFloats( nSlotFloats ) {}
        ~SpillFile();
        long long Write( const float *pAudio, size_t nAudio, const float *pOver, size_t nOver );    // the slot, or -1
        bool Read( long long llSlot, float *pAudio, size_t nAudio, float *pOver, size_t nOver );
        void Free( long long llSlot );
    private:
        bool Open();
        std::mutex m_mx;
        HANDLE m_hFile = INVALID_HANDLE_VALUE;
        size_t m_nSlotFloats;
        long long m_llNext = 0;
        std::vector< long long > m_vFree;
    };
    struct Slot
    {
        std::shared_ptr< SpillFile > pFile;
        long long llSlot = -1;
        ~Slot() { if ( pFile && llSlot >= 0 ) pFile->Free( llSlot ); }
    };
    struct Block
    {
        unsigned uGen = 0;              // the sound it was made with (older ones play until replaced)
        bool bDegraded = false;         // made lighter to be in time (made again once well ahead)
        int iDegradeVel = 0;            // ... BASSMIDI's pieces: note-ons below this velocity skipped
        int iThinPct = 0;               // ... BASSMIDI's continuous render: its thinning at the most (percent of level)
        unsigned uDegradeCap = 0;       // ... this voice cap (Kestrel's in all; BASSMIDI's each instance's)
        // The continuous renders: the voices sounding at the end of each eighth of it (empty: not known), and
        // the voice limits in all it was made with and as set
        std::vector< int32_t > vVoices;
        long long llVoiceCap = 0, llVoiceMax = 0;
        bool bPieceStart = false;       // the first block of its piece: fades in over the piece before's overrun
        std::vector< float > vAudio;    // stereo frames
        std::vector< float > vOver;     // the last block of a piece: CrossfadeFrames after it
        // Its copy on disk (vAudio and vOver empty: only there, read back before it plays)
        std::shared_ptr< const Slot > pSlot;
        size_t nAudio = 0, nOver = 0;   // (their sizes, floats)
        bool Spilled() const { return vAudio.empty(); }
    };
    using BlockPtr = std::shared_ptr< const Block >;
    struct Claim
    {
        long long llFirst, llCount;
        unsigned uGen;
        std::shared_ptr< std::atomic< bool > > pCancel;
        // Once its MIDI is made: its note-ons, when it began, and whether it was made lighter (auto-degrade
        // cancels a full piece that will be late, to make it again lighter)
        uint64_t llNotes = 0;
        long long llStartUs = 0;
        bool bLight = false;
        bool bMerlin = false;           // (a Merlin piece: timed by m_CostMerlin)
    };

    void LoaderThread();
    void WorkerThread( int k );
    void PagerThread();                 // blocks to and from the disk
    // The continuous render (Kestrel's, or BASSMIDI's as it plays; a worker's): one block, b, made by it,
    // started (again) there when it is not where the run is, with a piece's preroll and chase. False when
    // there is none (pieces then). Called and returns under m_mx; unlocked while it works
    struct StreamRun;
    bool StreamBlock( StreamRun &run, long long b, std::unique_lock< std::mutex > &lock, const RenderBackends::PieceOpts &opts );
    static constexpr double StreamLookSecs = 4.0;   // (how far past what it pushes it reads, for stacked copies' offs)
    void Fill( float *pOut, unsigned n, long long llWallUs, unsigned uRate );

    // Frames and song time at the current timebase (speed and rate), frames counted from song time 0
    double SongToFrame( double dSongUs ) const { return dSongUs * m_uRate / ( 1e6 * m_dSpeed ); }
    double FrameToSong( double dFrame ) const { return dFrame * 1e6 * m_dSpeed / m_uRate; }
    long long BlockFrames() const { return m_uRate; }
    long long PlayBlock() const;        // the block the playback position is in (under m_mx)
    long long ReadyBlocks( long long llPlay ) const;         // made from that block on, in a row (under m_mx)
    void NewTimebase( double dSpeed, unsigned uRate );      // (under m_mx)
    void NewGeneration();               // (under m_mx)
    void CancelUnwanted( long long llPlay );                 // (under m_mx)
    void Evict( long long llPlay );     // (under m_mx)

    // The piece's MIDI file: false when the timebase changed meanwhile. llCount may come back smaller
    // (a dense piece is cut at a block's end to stay under PieceBytesMax). bSilent: no notes to make
    // What a piece's notes are like: its note-ons by velocity (as sent) and the most notes held at once
    struct PieceStats { uint64_t llNoteOns = 0; uint64_t aVel[128] = {}; uint64_t llPolyMax = 0; };
    // How long pieces take on the bank in use: a fixed part and a part per note-on, fitted to the pieces
    // made (exponential averages of the notes, the time and their products)
    struct CostModel
    {
        int n = 0;
        double sN = 0, sT = 0, sNN = 0, sNT = 0;
        void Add( double dNotes, double dUs );
        bool Ready() const { return n >= 1; }
        double Fixed() const, PerNote() const;
    };
    // iParts: the notes split by key over that many files (each the whole piece's other events);
    // bRepeat: stacks of identical notes written once with a repeat mark (one part only)
    bool BuildPiece( PlaybackDecoder &dec, long long llFirst, long long &llCount, const Sound &snd, double dSpeed, unsigned uRate,
                     int iParts, bool bByChannel, long long llNps, bool bRepeat, PieceStats &st, std::vector< std::vector< uint8_t > > &vMidi, uint64_t &llSkip, uint64_t &llFrames, bool &bSilent, std::vector< uint32_t > &vBuf,
                     const std::atomic< bool > *pCancel, bool bShortPreroll = false );
    long long HeldEarliest( long long llAt, long long llFloor ) const;
    void HeldNotes( long long llAt, std::vector< uint32_t > &vOut, double dVelocityCorrect ) const;

    std::shared_ptr< const Song > m_pSong;
    const unsigned m_uOfflineRate = 0;  // offline: its rate (else 0)
    PlaybackClock m_OwnClock;           // (offline: the clock, at the block being written)
    PlaybackClock &m_Clock;
    AudioOut m_Out;

    std::mutex m_mx;
    std::condition_variable m_cv;
    Settings m_Settings;
    std::shared_ptr< RenderBackends::Bank > m_pBank;
    std::wstring m_sState;              // loading, or why nothing can be made
    std::wstring m_sRenderError;        // why the last piece failed (cleared by one that works)
    std::wstring m_sWarning;            // what the bank's loading reported
    int m_eLoadedEngine = -1;           // the engine the bank was last loaded for
    unsigned m_uBankGen = 0, m_uLoadedGen = 0;      // asked for and loaded settings
    Sound m_Sound;
    unsigned m_uGen = 1;                // the sound's generation
    unsigned m_uBase = 1;               // the timebase's: blocks of another are thrown away
    double m_dSpeed = 1.0;
    unsigned m_uRate = 48000;
    std::map< long long, BlockPtr > m_mBlocks;
    CostModel m_Cost;                   // (the bank's: started afresh with each)
    CostModel m_CostMerlin;             // (Merlin's pieces: a cost of their own, much more per piece)
    std::set< long long > m_sNoRedo;    // lighter blocks not made again in full (it would not be in time)
    std::atomic< uint64_t > m_llDegraded{ 0 };
    std::atomic< int > m_iThinNow{ 0 };     // BASSMIDI's continuous render: its thinning now (percent of the level)
    std::vector< Claim > m_vClaims;
    bool m_bStop = false;
    bool m_bHold = false;
    // BASSMIDI's continuous render (worker 0's) could not start with the bank loaded: the other workers make
    // pieces too, as they would without it (cleared when another bank is loaded)
    bool m_bBassNoStream = false;
    // The player at the song's end: playing (so the tail plays on once the clock stops there), the tail played
    // out (the player stays at its end), or neither (paused elsewhere, or after a seek)
    enum { TailNone, TailArmed, TailDone };
    int m_iTail = TailNone;
    std::shared_ptr< SpillFile > m_pSpill;      // (the timebase's)
    int m_iWorkers = 0;
    long long m_llEndUs = 0;            // the song's last event plus a ring-out
    uint16_t m_uChannels = 0;           // the channels with notes (bit n: channel n)
    // (Settings::bWhole: the piece being made, its blocks, and how far it is)
    long long m_llWholeFirst = LLONG_MIN, m_llWholeCount = 0;   // (LLONG_MIN: none being made)
    std::atomic< float > m_fWholeProgress{ 0.0f };
    std::function< void() > m_fnLoaded;
    long long m_llOfflineLast = LLONG_MAX;      // (offline: the last block RenderOffline wants)

    std::thread m_Loader, m_Pager;
    std::vector< std::thread > m_vWorkers;

    // The player (AudioOut's thread)
    std::atomic< bool > m_bMute{ false };
    std::atomic< double > m_dGain{ 1.0 };
    std::atomic< bool > m_bLimiter{ true };
    std::atomic< uint64_t > m_llGaps{ 0 };
    double m_dCursor = 0.0;             // the frame being played (fractional while catching up)
    unsigned m_uCursorBase = 0, m_uCursorEpoch = 0;
    float m_fFade = 0.0f;               // gain ramp for pause, mute and jumps
    // Following a ramped clock (the PFA lag simulation), stretched at the music's own pitch (Fill): the two
    // grains sounding (where each reads from), how far into the hop it is, the gain at the hop's start and
    // end (fading out while the clock stands), and where the clock was at the hop's start
    bool m_bGrains = false;
    long long m_llGrainPrev = 0, m_llGrainCur = 0, m_llGrainPhase = 0;
    float m_fGrainGain0 = 1.0f, m_fGrainGain1 = 1.0f;
    double m_dGrainWant = 0.0;
    float m_fLimGain = 1.0f;
    std::vector< float > m_vMix;
};
