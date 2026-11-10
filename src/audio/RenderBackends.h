// The synths that make audio rendered ahead (RenderAhead.h) out of small MIDI files in memory, each
// loaded at run time, so the program runs without them:
//   - BASSMIDI (bass.dll + bassmidi.dll, un4seen): on the CPU. Each piece can be split by key over
//     several BASSMIDI instances on threads of their own, each with the voice limit set (one instance
//     keeps up with only a few thousand voices), and their audio summed.
//   - Kestrel and Merlin (kestrel_bridge.dll: Spiderweb's C connector over the Kestrel GPU synth): on
//     the graphics card, one piece at a time. Merlin renders per key and never drops a note; Kestrel
//     has a voice limit. Neither reads every soundfont opcode or controller BASSMIDI does.
// The DLLs are built into PFAviz++.exe (a build with vendor/engines/ holding them carries them, and
// writes them out to %LOCALAPPDATA%\PFAviz++\engines the first time they are needed); a build
// without them looks next to PFAviz++.exe.
// A bank is the soundfonts loaded for BASSMIDI, or for Kestrel and Merlin together (they share one), at
// one sample rate. The last bank of each kind stays loaded across songs and engine switches, so going
// back to an engine is instant; and each soundfont file stays read for Kestrel while it is in the list,
// so adding or removing another only sends the set to the graphics card again (CloseShared frees all).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RenderBackends
{
    enum Engine { EngineBass = 0, EngineKestrel = 1, EngineMerlin = 2, EngineCount };
    const wchar_t *EngineName( int eEngine );

    // Kestrel's and Merlin's advanced settings (Preferences > Rendered audio > Advanced), Kestrel's own
    // defaults. Merlin reads the soundfont ones (filters, LFOs, modulation envelopes, layers, the DC
    // blocker) but not the voice ones (it has no voice limit) or the interpolation
    struct KestrelOpts
    {
        int iInterpolation = 1;             // 0 nearest, 1 linear, 2 cubic
        int iStealRule = 2;                 // when the voice limit is reached: 0 oldest, 1 drop the new note, 2 quietest
        int iAdmitRule = 0;                 // which notes of a burst get voices: 0 the loudest, 1 evenly
        int iMaxStealPercent = 25;          // most voices stolen in one block (1 to 100)
        int iReleaseFloorDb = -100;         // a releasing voice ends below this level (-160 to -20)
        int iMaxLayers = 16;                // most soundfont layers a note sounds (1 to 64)
        bool bFilter = true;                // the soundfont's filters (applies when the soundfonts load)
        bool bLfo = true, bModEnv = true;   // the soundfont's LFOs and modulation envelopes
        bool bDcBlocker = true;
        int iDcBlockerHz = 15;              // 1 to 200
        // Frames the card makes at a time: smaller shares the card with the picture more evenly (Kestrel's
        // own is 4096; at 1024 a dense song's frame rate held 90 % of its rate without audio, against 61 %)
        int iBlockFrames = 1024;
        // Kestrel's performance settings (its own defaults)
        int iGateFrames = 32;               // frames between note-off, controller and LFO updates on the card
        int iWorkgroupSize = 256;           // card threads per render workgroup
        int iRenderWorkgroups = 2048;       // most workgroups one render pass dispatches (voices past them loop)
        int iSubmitVoices = 4194304;        // most voices one piece of card work renders before a block is split
        int iVoiceThreads = 4;              // CPU threads building each block's new voices
        bool bSortVoices = true;            // voices reading the same samples run together (by far the biggest speedup)
        bool bKahan = false;                // compensated summation: less rounding error, slower
        bool bUnchecked = false;            // shaders without bounds checks: a little faster, a bug a GPU memory error
        bool operator==( const KestrelOpts & ) const = default;
    };

    struct BankKey
    {
        int eEngine = EngineBass;
        std::vector< std::wstring > vFonts;     // in order: each replaces what the ones before it define
        unsigned uRate = 48000;
        bool bFilter = true;                    // Kestrel: the soundfonts' filters (read at load)
        bool operator==( const BankKey & ) const = default;
    };

    // Per piece: what a synth is asked for besides the MIDI
    struct PieceOpts
    {
        unsigned uVoices = 1000;                // BASSMIDI's per instance, Kestrel's (Merlin has none)
        int iBassThreads = 1;                   // BASSMIDI instances a piece is split over, by key (key groups)
        // ... and by channel as well (OmniMIDI's delegation): 16 x iBassThreads instances, one per channel and
        // key group, each with uVoices; the song's channels with notes (bit n: channel n) say how many work
        bool bBassByChannel = false;
        uint16_t uChannels = 0xFFFF;
        bool bFx = true;                        // BASSMIDI's reverb and chorus
        int eEngine = EngineBass;               // Kestrel or Merlin, on a Kestrel bank
        bool bForceKestrel = false;             // Merlin chosen: this piece on Kestrel (auto-degrade: its voice cap)
        KestrelOpts kb;
        std::atomic< float > *pProgress = nullptr;  // (when given: how far the piece is, 0 to 1, as the engine knows it)
    };

    // A continuous render (Kestrel: one synth kept from block to block, as a live host keeps it): events
    // pushed at their frame from its start, before the audio holding them is asked for; then the next
    // frames, in order. The voice cap can move while it plays
    class Stream
    {
    public:
        virtual ~Stream() {}
        // n messages (status | d1 << 8 | d2 << 16) at frames (non-decreasing), each note-on's loudness
        // multiplied by pScales[i] (null: 1)
        virtual void Push( const uint64_t *pFrames, const uint32_t *pMsgs, const float *pScales, size_t n ) = 0;
        virtual void SysEx( uint64_t llFrame, const uint8_t *p, size_t n ) = 0;
        // The next uFrames frames (everything up to 16384 frames past them pushed already)
        virtual bool Render( float *pOut, unsigned uFrames, std::wstring &sError ) = 0;
        virtual void SetCap( unsigned uVoices ) = 0;
    };

    class Bank
    {
    public:
        virtual ~Bank() {}
        virtual int Engine() const = 0;         // EngineBass, or EngineKestrel for Kestrel's and Merlin's
        // What loading the soundfonts reported (Kestrel: unsupported SFZ opcodes and the like), one a line
        std::wstring sWarnings;
        // Whether Merlin takes these soundfonts (a one-note piece tried once; false with the reason: its
        // pieces then go to Kestrel at once)
        virtual bool MerlinTakes( std::wstring &sWhy ) { sWhy.clear(); return false; }
        // Pieces worth making at once, when each takes iParts threads
        virtual int Parallel( int iParts ) const = 0;
        // The MIDI files a piece is split into (by key; the other events in each)
        virtual int Parts( const PieceOpts &o ) const { return 1; }
        // Which part a note goes to (channel, key), of Parts( o )
        static int PartOf( const PieceOpts &o, int iParts, uint32_t uCh, uint32_t uKey )
        {
            if ( iParts <= 1 ) return 0;
            if ( !o.bBassByChannel ) return (int)( uKey % (uint32_t)iParts );
            const uint32_t uGroups = (uint32_t)iParts / 16;
            return (int)( uCh * uGroups + uKey % uGroups );
        }
        // Pieces for it may carry Spiderweb's repeat marks (FF 7F 08 "SWn" key count before a note
        // event: it stands for that many identical ones): Merlin on one soundfont
        virtual bool RepeatMarks( const PieceOpts & ) { return false; }
        // The piece's audio (its parts' summed) as interleaved stereo floats: llSkip frames thrown away at
        // the start, then exactly llFrames frames (silence where the synth ends sooner). False when it
        // failed (sError) or was cancelled (*pCancel turned true). llBusyUs: the time it took
        virtual bool Render( const std::vector< std::vector< uint8_t > > &vParts, uint64_t llSkip, uint64_t llFrames, const PieceOpts &o,
                             const std::atomic< bool > *pCancel, std::vector< float > &vOut, std::wstring &sError ) = 0;
        // A continuous render with o's settings (Kestrel; null when this bank or its DLL has none, sError why)
        virtual std::unique_ptr< Stream > OpenStream( const PieceOpts &o, std::wstring &sError ) { sError = L"no continuous render"; return nullptr; }
    };

    // The bank for these settings: the loaded one when it matches, else loaded now (which can take a
    // while for a large soundfont). Null with sError when it cannot be
    std::shared_ptr< Bank > Acquire( const BankKey &key, std::wstring &sError );
    // The same in the background (a thread of its own, the latest asked for loaded; uRate 0: the sound
    // device's), so the soundfonts are ready before a song needs them; Merlin is tried on them too
    void Preload( const BankKey &key );
    bool Preloading();      // a Preload is loading now (or was when CloseShared left it alone)
    void CloseShared();     // (the preloading stopped first; one under way is left alone: see Preloading)

    // For the Preferences page: whether the engines' DLLs are there (built in, or next to the program)
    std::wstring DllStatus();
    // Notes for the Preferences page: NoteSong, what of the open song Merlin does not do properly
    // (RenderAhead::MerlinIssues); NoteLoad, what the last soundfont load reported
    enum Note { NoteSong, NoteLoad, NoteCount };
    void SetNote( int eNote, const std::wstring &s );
    std::wstring GetNote( int eNote );
}
