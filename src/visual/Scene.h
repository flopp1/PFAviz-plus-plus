// Shared frame drawing, ported from the original MainScreen (RenderGlobals, RenderLines, RenderNote,
// RenderKeys, RenderBorder). Used by the app and by the test harness, so both draw with one copy of
// the code. Every float expression is kept as in the original (DESIGN.md R6, R10).
#pragma once

#include <vector>

#include "midi/RefMidi.h"
#include "render/Renderer.h"

struct ChannelSettings
{
    ChannelSettings() { bHidden = bMuted = false; SetColor( 0x00000000 ); }
    void SetColor();
    void SetColor( unsigned int iColor, double dDark = 0.5, double dVeryDark = 0.2 );

    bool bHidden, bMuted;
    unsigned int iPrimaryRGB, iDarkRGB, iVeryDarkRGB, iOrigBGR;
};
struct TrackSettings { ChannelSettings aChannels[16]; };

// Fixed scene colours (MainScreen::InitColors), plus the configurable background
struct SceneColors
{
    SceneColors();
    void SetBackground( unsigned int iBkgColor );
    ChannelSettings csBackground, csKBRed, csKBWhite, csKBSharp, csKBBackground;
    ChannelSettings csKBHalf, csKBSharp31;    // 31-EDO layout: half sharps and flats, full sharps (full flats: csKBSharp)
};

// Inputs and outputs of MainScreen::RenderGlobals
// Clean drawing (Visual tab): each flag leaves one of the original's rounding quirks out
namespace Clean
{
    enum : unsigned
    {
        MinHeight = 1,      // a note rounding to 0 rows is drawn 1 row tall (the original skips it)
        KeepBottom = 2,     // the frame's start is rounded up, not down, to whole rows, so a note leaves
                            // the hit line no later than it stops sounding (the original rounds down, so a
                            // note can be gone while its last row would still show above the keyboard)
        EdgeRound = 4,      // a note's top and bottom rows are each rounded from its end and start time
                            // (the original rounds its start and its length, so two notes ending
                            // together can have tops a row apart and the lower layer peeks out)
        All = 7,
    };
}

struct ViewInputs
{
    int iBufferWidth = 0, iBufferHeight = 0;
    int eKeysShown = 0;
    int iFirstKey = 21, iLastKey = 108;
    int iSongMinNote = 0, iSongMaxNote = 127;
    float fOffsetX = 0.0f, fOffsetY = 0.0f, fZoomX = 1.0f;   // already include the temporary move/zoom
    bool bShowKB = true;
    long long llStartTime = 0, llTimeSpan = 3000000;
    unsigned uClean = 0;              // Clean flags (0: the original's drawing)
    bool bLines = true;               // the key dividers and measure lines
    bool bEdo31 = false;              // the 256-key view in the 31-EDO layout (Visual tab)
    bool bFlat = false;               // the effects' flat look (DrawKeys)
};

struct FrameGlobals
{
    int iStartNote, iEndNote;
    int iMiddleC = 60;                // the key with the middle C mark (Notes::C4; in the 256-key view key 127, the middle white key)
    float fNotesX, fNotesY, fNotesCX, fNotesCY;
    int iAllWhiteKeys;
    float fWhiteCX;
    long long llStartTime, llTimeSpan, llRndStartTime;
    int iBufferWidth, iBufferHeight;
    unsigned uClean;
    bool bLines;
    bool bEdo31 = false;              // 31-EDO layout: 8 octaves of 31 keys and C to D#, by Scene's key table
    bool bFlat = false;               // flat keys, from the notes' bottom down, without the red bar
};

// The effects' changes to the keyboard (visual/Effects.h)
struct KeyLook
{
    const ChannelSettings *apUnlit[256] = {};   // an unpressed key's colours instead of the scene's (its light fading out), or null
    D3DCOLOR aGlow[256] = {};                   // each key's glow (0: none, drawn black, so it hides the glow of the keys under it)
    std::vector< Renderer::GlowRect > *pGlow = nullptr;     // the keys' glow rects go here (null: none)
    bool bBorders = true;                                   // the flat look: gaps between the white keys (off: they meet)
};

namespace Scene
{
    extern const float SharpRatio;
    extern const float KBPercent;
    extern const float KeyRatio;

    FrameGlobals ComputeGlobals( const ViewInputs &in );
    float GetNoteX( const FrameGlobals &g, int iNote );
    // The layer a key is drawn in: 0 white, 1 black (31-EDO: half sharps and flats), 2 (31-EDO only)
    // full sharps and flats, each over the ones before. Notes are drawn in the same order
    int KeyLayer( const FrameGlobals &g, int iNote );
    static const int KeyLayers = 3;

    // RenderNote for one note (normal practice-mode drawing: opaque, outer + inner rect)
    void DrawNote( Renderer *pRenderer, const FrameGlobals &g, int iNote, long long llNoteStart, long long llNoteEnd,
                   const ChannelSettings &cs );
    // The 12 vertices DrawNote submits (outer rect, then inner rect)
    void NoteVertices( const FrameGlobals &g, int iNote, long long llNoteStart, long long llNoteEnd,
                       const ChannelSettings &cs, Renderer::SCREEN_VERTEX *pOut );
    // The same in two parts: what depends only on the key, then each note of it
    struct NoteKey { float x, cx, fDeflate; };
    NoteKey GetNoteKey( const FrameGlobals &g, int iNote );
    void NoteVertices( const FrameGlobals &g, const NoteKey &nk, long long llNoteStart, long long llNoteEnd,
                       const ChannelSettings &cs, Renderer::SCREEN_VERTEX *pOut );

    void DrawBackground( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c ); // field + vertical lines
    // pKeyColor[i] is the colour of the note lighting key i, or nullptr when the key is not lit
    void DrawKeys( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c, const ChannelSettings *const pKeyColor[256],
                   const KeyLook *pLook = nullptr );
    // The top of the keyboard's red bar (DrawKeys; the flat look: of the keys)
    float RedBarY( const FrameGlobals &g );
    void DrawBorder( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c );
}

// Tempo / time signature tracking for measure lines and the metronome
// (MainScreen::AdvanceIterators, GetPrevious, GetCurrentTick, GetBeat*, RenderLines horizontal part).
class MeterTracker
{
public:
    explicit MeterTracker( const RefMidi &midi ) : MeterTracker( midi.GetMetaEvents(), midi.GetInfo() ) {}
    MeterTracker( const std::vector< RefMetaEvent > &vMeta, const RefMidiInfo &info );

    void Jump( long long llTime );       // AdvanceIterators( llTime, true )
    void Advance( long long llTime );    // AdvanceIterators( llTime, false )
    long long GetCurrentTick( long long llTime ) const;
    long long GetBeat( long long iTick, long long iBeatType, long long iLastSignatureTick ) const;
    long long GetBeatTick( long long iTick, long long iBeatType, long long iLastSignatureTick ) const;
    long long GetMetTick( long long iTick, long long iClocksPerMet, long long iLastSignatureTick ) const;
    void DrawMeasureLines( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c, long long iStartTick ) const;

    long long BeatsPerMeasure() const { return m_iBeatsPerMeasure; }
    long long BeatType() const { return m_iBeatType; }
    long long LastSignatureTick() const { return m_iLastSignatureTick; }

private:
    typedef std::vector< std::pair< long long, int > > eventvec_t;
    long long GetTickTime( long long iTick, long long iLastTempoTick, long long llLastTempoTime, long long iMicroSecsPerBeat ) const;
    const RefMetaEvent *GetPrevious( eventvec_t::const_iterator &itCurrent, const eventvec_t &vEventMap, int iDataLen ) const;

    const RefMidiInfo &m_Info;
    const std::vector< RefMetaEvent > &m_vMetaEvents;
    eventvec_t m_vTempo, m_vSignature;
    eventvec_t::const_iterator m_itNextTempo, m_itNextSignature;
    long long m_iMicroSecsPerBeat = 500000, m_iLastTempoTick = 0, m_llLastTempoTime = 0;
    long long m_iBeatsPerMeasure = 4, m_iBeatType = 4, m_iClocksPerMet = 24, m_iLastSignatureTick = 0;
};
