// PFAviz++ configuration. Same settings as the original (minus input, learn, labels and library),
// plus the Performance tab. Stored as UTF-8 INI in %APPDATA%\PFAviz++\config.ini.
#pragma once

#include "audio/RenderBackends.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class IniFile;

#define APPNAME L"PFAviz++"
#define CLASSNAME L"PFAvizPP"
#define GFXCLASSNAME L"PFAvizPPGfx"
#define POSNCLASSNAME L"PFAvizPPPosCtrl"
#define MINWIDTH 640
#define MINHEIGHT 469

namespace PlayMode { enum State { Intro = 0, Splash = 1, Play = 160 }; }

// Eye candy (Preferences > Effects; visual/Effects.h). All off by default, which draws the original's look
struct EffectSettings
{
    void LoadDefaultValues();
    void LoadConfigValues( const IniFile &ini );
    void Save( IniFile &ini ) const;
    bool Any() const;           // any effect to draw (none while bOn is off)

    bool bOn;                   // the effects at all (off: none, whatever they are set to)

    // Light: every note and key has a light of 0 to 1. A note rests at iRestLight; one playing at the
    // hit line, and the key it presses, rise from there to 1 and fall along the curve (eDecay). Each
    // part is drawn by its light: towards its own colour undarkened (aDark), lightened (aLightUp),
    // glowing (aGlow).
    // PartShort: notes too short to have an inside; PartBar: the keyboard's red bar, never lit (its
    // light-up and glow always as at full light)
    enum Part { PartNotes, PartShort, PartKeys, PartBackground, PartBar, PartCount };
    // The look: normal (the original's); flat (plain keys, notes one flat colour without outlines); semi-flat
    // (the normal look's bar over keys without depth, pressed and unpressed told apart by their colours
    // only; notes one colour, their outlines iSemiFlatOutline darker)
    enum Look { LookNormal, LookFlat, LookSemiFlat, LookCount };
    int eLook;
    bool bFlatBorders;          // the flat keys have gaps between them (off: the white keys meet)
    int iSemiFlatOutline;       // percent: the semi-flat look's note outlines that much darker than their insides
    bool bSemiFlatLip;          // the semi-flat look's pressed black keys keep their lip (off: one flat colour)
    // Pressed keys whiten towards their top, each look its own (by Look): on (the normal look's off:
    // darkening towards the top, as the original), and how far (percent)
    bool abKeyWhite[LookCount];
    int aiKeyWhite[LookCount];
    bool bNoBar;                // the keyboard without its red bar
    bool bHollow;               // the notes' insides are not drawn: only their outlines, the scene showing through
    bool bShortColor;           // short notes are drawn in their main colour, not their outline's
    // Merged outlines: touching notes of one track and channel share one outline, around them all (not with
    // the flat look, which has none). Short notes stay apart unless bMergeShort (then among themselves,
    // or with bMergeShortLong with the long notes too)
    bool bMergeOutlines, bMergeShort, bMergeShortLong;
    // percent: how much of a note's light reaches its outline and its inside (not short notes', which
    // have only the one colour): 100 lights it fully; an outline goes on, to 200 its note's main
    // colour, to 300 white
    int iLightOutlines;
    int iLightInsides;
    int iRestLight;             // percent: every note's light when not playing
    int aDark[PartCount];       // percent: an unlit part darkened that much (the background's also darkens the stats box, with bStats)
    int aLightUp[PartCount];    // percent: a fully lit part lightened that much towards white (not the background)
    int aGlow[PartCount];       // percent: a fully lit part's glow, of iBloom (the background's: the whole frame's, over iBloomThreshold)
    int iBloom;                 // percent: the glow's strength
    int iBloomRadius;           // the glow's reach, pixels at 1080p
    int iBloomThreshold;        // percent: the frame glows where it is brighter than this
    int iBloomLimit;            // percent: the glow added to a pixel tends to at most this, softly (0: no limit)
    // Auto exposure: the lit share of the keyboard, followed over iExposureMs, sets how far the notes' and
    // keys' light is turned up (little lit: fades last longer) or down (much lit), and the glow down
    int iExposure;              // percent: how strongly (0: off)
    int iExposureMs;
    int iVignette;              // percent: the corners darkened that much more
    enum VignetteSide { VignetteAround, VignetteTop, VignetteBottom, VignetteLeft, VignetteRight, VignetteSideCount };
    int eVignetteSide;          // from every edge (darkest in the corners), or from one side only
    bool bVignetteNotes;        // over the notes area only (not the keyboard, nor the rest of the picture)
    int iVignetteFade;          // percent of the way in from its edge over which it fades out (more: softer and wider)
    int eDecay;                 // Decay: how a played note's light falls while it is held
    int iDecayMs;               // the time it takes
    int iDecayFloor;            // percent: the light left after it
    int iReleaseMs;             // a key's light fading out after it is let go (not with DecayNone)
    bool bStats;                // the stats box is drawn before the effects, so they change it too
    // The hit line: a band over everything else, centred on the notes' bottom (where the red bar is),
    // solid in the middle and fading out above and below
    bool bHitLine;
    unsigned iHitLineColor;     // its colour (as Windows' COLORREF)
    int iHitLineSize;           // its solid middle's height, pixels at 1080p
    int iHitLineSoft;           // its fade above and below, pixels at 1080p
    int iHitLineStrength;       // percent: how opaque
    int iHitLineGlow;           // percent: its glow, of iBloom
    bool bHitLinePlaying;       // only over the keys being played, by their light
    bool bHitLineNoteColor;     // over the keys being played in their notes' colours (with the line across, over it)
    int iHitLineFadeMs;         // a key let go: its part of the line fades out over this long
    int eHitLineCurve;          // along this curve (Decay; DecayNone: gone at once)
    int iHitLineSmear;          // how far it spreads sideways past a key's column, pixels at 1080p (0: not at all)
    enum Decay { DecayNone, DecayLinear, DecayExponential, DecaySmooth, DecayCount };
};

struct VisualSettings
{
    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    // All = the 88 piano keys (the original's "Show all 88 keys"); All128 = every MIDI key (the default)
    enum KeysShown { All, Song, Custom, All128, All256 } eKeysShown;
    int iFirstKey, iLastKey;
    bool bAlwaysShowControls;
    // The stats box's lines (Visual > Statistics...); uStats has bit 1 << Stat for each one shown, and
    // aStatOrder lists them all top to bottom. New lines are added at the end, so saved bits stay put.
    enum Stat { StatTime, StatFPS, StatScore, StatNotes, StatNPS, StatPolyphony, StatSent, StatSkipped, StatCollapsed, StatLag,
                StatSkipping, StatOtherMs, StatNotesMs, StatSceneMs, StatPresentMs,
                StatDecoded, StatDecoderBusy, StatSenderBusy, StatRing, StatSendCost, StatTempo, StatVoices, StatSynthCpu, StatCC,
                StatMaxNPS, StatMaxPoly, StatRendered, StatDegrade, StatPfaLagTime, StatCount };
    static constexpr unsigned NoteStats = 7u << StatNotes | 3u << StatMaxNPS;      // counted while a song loads (Song::noteStats)
    static constexpr unsigned PipelineStats = 31u << StatDecoded;
    static constexpr unsigned VoiceStats = 3u << StatVoices;    // asked of the synth (KDMAPI only)
    static constexpr unsigned SynthStats = 31u << StatSent | PipelineStats | VoiceStats | 1u << StatRendered | 1u << StatDegrade;  // need playback
    static constexpr unsigned FrameStats = 15u << StatOtherMs;  // frame time parts, live only
    static constexpr unsigned AllStats = ( 1u << StatCount ) - 1;
    // The Statistics dialog's sections, each line's section, its label in the stats box and its name
    enum StatSection { SectionGeneral, SectionNotes, SectionSynth, SectionPipeline, SectionFrame, SectionCount };
    static const wchar_t *const SectionNames[SectionCount];
    static const int StatSections[StatCount];
    static const wchar_t *const StatLabels[StatCount];
    static const wchar_t *const StatNames[StatCount];
    static const unsigned char DefaultStatOrder[StatCount];
    unsigned uStats;
    unsigned char aStatOrder[StatCount];
    static const int MaxColors = 128;
    unsigned int colors[MaxColors], iBkgColor;     // the track colors: the first iColorCount are used
    unsigned int iBarColor;     // the keyboard's red bar
    int iColorCount;
    // The user's saved palettes (Visual tab: chosen, they replace the track colors), each its colours in order
    struct Palette { std::wstring sName; std::vector< unsigned > vColors; };
    std::vector< Palette > vPalettes;
    // The user's saved effects profiles (Effects tab: chosen, they fill it), each every effect setting
    // but bOn. A config from before them: its effects as the first one (VisualSettings::Load)
    struct FxProfile { std::wstring sName; EffectSettings fx; };
    std::vector< FxProfile > vFxProfiles;
    bool bBkgImage;             // the background image is shown
    std::wstring sBkgImage;     // its file, drawn over iBkgColor
    int iBkgFit;                // Renderer::ImageFit
    int iBkgOpacity;            // percent
    unsigned uClean;            // Clean flags (visual/Scene.h): the original's rounding quirks left out
    bool bHideLines;            // no key dividers or measure lines over the background
    bool bTickBased;            // notes placed by tick, not time (FastEngine::SetTickBased)
    bool bPfaLag;               // play as the original PFA would lag on this computer (song/PfaLag.h)
    bool bPfaLiveStats;         // its statistics (and graphs) follow how far its Logic has played, not its last drawn frame
    bool bPfaLiveVisual;        // no visual lag: every frame drawn at its playhead (the sound, frame rate and lag time still simulated)
    int iPfaL2KB, iPfaL3KB;     // its CPU's L2 and L3 caches (0: this computer's)
    int iPfaSendNs;             // what one MIDI message costs it on average (midiOutShortMsg), nanoseconds (50: the KDMAPI patch)
    double dPfaCpuPct;          // its CPU's speed, percent of this computer's (100: this one, as benchmarked; PfaCpuMin to PfaCpuMax)
    static constexpr double PfaCpuMin = 1e-4, PfaCpuMax = 1e9;
    bool bHideSpeedBoxes;       // the toolbar as the original's: sliders only, without the exact speed boxes
    bool bLoopColors;           // the track colors repeat (channel n gets colors[n % iColorCount]); off: random after them
    bool bUniqueColors;         // (not looping) the random colours each different, from each other and the track colours (Util::UniqueColors)
    bool bEdo31Keys;            // the 256-key view in the 31-EDO layout (31 keys an octave; Scene.cpp)
    bool bStatsScaled;          // the stats box's size is as at 1080p, scaled with the picture's height (off: pixels)
    int iStatsCX, iStatsCY;     // the box's size: its height over its lines sets the line height and font; the
                                // width is the least, widened to fit the text
    bool bStatsFontSet;         // the font's size set instead (iStatsFontPx), the height following it
    int iStatsFontPx;           // its height, pixels (scaled as the box is with bStatsScaled)
    bool bStatsPfaSize;         // the original's size: 156 wide, unscaled, its height by the lines shown
                                // (StatsDefaultCY), following them; iStatsCX/CY and bStatsScaled unused
                                // (bStatsFontSet wins)
    int iStatsCorner;           // StatsCorner bits: where the box sits (0: top right)
    std::wstring sStatsFont;    // the box's font (the original's: Tahoma)
    bool bStatsBackground;      // the box's translucent black background
    bool bStatsShadow;          // the text's shadow
    bool bStatsShadowMirror;    // the text's shadow mirrored with the corner (off: always down and right, as the original)
    // The graphs of notes per second, polyphony and tempo over time (on in View > Graphs; set in
    // Visual > Graphs...)
    bool bGraph;
    struct GraphSettings
    {
        enum Series { NPS, Polyphony, Tempo, SeriesCount };
        bool abShow[SeriesCount];       // drawn
        bool abLine[SeriesCount];       // as a line (off: filled)
        bool abRight[SeriesCount];      // its text at the right (off: the left)
        unsigned aColor[SeriesCount];   // its colour (a COLORREF)
        int aiPlace[SeriesCount];       // its place top to bottom, 0 first, in any corner (in one box: its
                                        // text's place); each place held by one
        bool bTogether;                 // all in one box (then only one filled), not a box each
        bool bZero;                     // the bottom at zero (off: at the lowest value shown)
        bool bMiddle;                   // the time now in the middle (off: at the right edge)
        int iCorner;                    // StatsCorner bits (with the stats box there: next to it)
        int iSpanMs;                    // the time shown, ms (at least the counts' 1 ms bin)
        int iCX, iCY;                   // a box's size, pixels as at 1080p
        int iFontPx;                    // the text's height, pixels as at 1080p
        int iGrowMs, iShrinkMs;         // how quickly a scale follows its values up and down: the time to
                                        // get most of the way there (0: at once), ms of song time
        bool bScale;                    // a vertical scale: values at the left, lines across at them
        bool bScaleSide;                // the scale on the side away from the boxes' corner (left corners: at the
                                        // right), so the graph stays put as its values' width changes
        bool bMaxLine;                  // a line across at the highest value shown, with it written
        unsigned uMaxColor;             // its colour (a COLORREF)
        void Fix();                     // together: only the first filled one stays filled; places not
                                        // each held once: back to the series' order
    } graph;
    enum StatsCorner { StatsLeft = 1, StatsBottom = 2 };    // bottom: on the keyboard's red bar
    static const int StatsCXMin = 20, StatsCXMax = 3840, StatsCYMin = 8, StatsCYMax = 2160, StatsFontMin = 4, StatsFontMax = 400;
    // The original's box: 156 pixels wide, 16 a line and 3 above and below
    static int StatsDefaultCY( unsigned uStats ) { return 6 + 16 * std::max( 1, __builtin_popcount( uStats & AllStats ) ); }
    EffectSettings fx;
};

struct AudioSettings
{
    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    void LoadMIDIDevices();
    std::vector< std::wstring > vMIDIOutDevices;
    int iOutDevice;
    std::wstring sDesiredOut;

    // Audio rendered ahead (audio/RenderAhead.h) instead of the MIDI output
    bool bRendered;
    int eRenderEngine;              // RenderBackends::Engine: BASSMIDI, Kestrel, Merlin
    std::wstring sSoundfonts;       // paths separated by |, each replacing what the ones before it define
    int iBassVoices, iKestrelVoices;
    int iBassThreads;               // BASSMIDI instances (threads) each piece is split over, by key
    int iGpuShare;                  // Kestrel and Merlin: most of the graphics card's time, once well ahead (percent)
    RenderBackends::KestrelOpts kb; // Kestrel's and Merlin's advanced settings
    int iAheadSecs;                 // how far ahead of the playback position it is made
    int iBehindSecs;                // how far behind it what was made is kept
    int iBassNps;                   // BASSMIDI: each instance's notes-a-second limit (0: none)
    bool bBassByChannel;            // BASSMIDI: pieces split by channel as well as key (16 x iBassThreads instances)
    bool bLimiter;                  // keeps the joined audio under full scale
    bool bRenderFx;                 // BASSMIDI's reverb and chorus
    bool bRenderLossless;           // stacks of identical notes sent once with their count (Kestrel, Merlin)
    bool bRenderDegrade;            // pieces made lighter when they would be late
    bool bRenderDiscard;            // switching engines throws away the old one's audio
};

struct VideoSettings
{
    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    bool bLimitFPS;
};

struct ControlsSettings
{
    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    double dFwdBackSecs, dSpeedUpPct;
    double dNSpeedUpPct;        // Shift+Up/Down: the note speed's step
    bool bStartPaused;          // a song is paused at its start once it has loaded
};

// Offline video rendering (Performance > Render video...): rendered off-screen and piped to ffmpeg
struct ExportSettings
{
    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    std::wstring sOutFile;
    bool bUseWindowSize;        // the graphics window's size instead of iWidth x iHeight
    int iWidth, iHeight;
    int iFPS;                   // 1-240
    int iCRF;                   // quality, 0-51 (x264's CRF scale, mapped onto each encoder's own; 0: lossless in software H.264 and HEVC)
    int iPreset;                // index into Presets (mapped onto each encoder's own speed levels)
    int iCodec, iBackend, iContainer;   // what is encoded (Codec), by which encoder (Backend), into which file format
    bool bShowStats;            // the top-right stats area
    bool bAudio;                // sAudioFile is muxed in (off: a silent video; the file is kept for next time)
    std::wstring sAudioFile;    // anything ffmpeg reads: WAV up to 64-bit float, FLAC up to 24-bit, ...
    bool bAudioAlign;           // its first sound is moved to where the song's first note sounds
    int iAudioOffsetMs;         // and then by this much (positive: later)
    bool bRenderedAudio;        // instead of the file: the audio made by the audio renderer (AudioSettings' rendered
                                // audio settings), made from the start for the video, in time with it
    static const wchar_t *const Presets[];
    static const int PresetCount;
    enum Codec { H264, Hevc, Av1, ProRes, Ffv1, CodecCount };
    enum Backend { Software, Nvenc, Amf, Qsv, BackendCount };
    enum Container { Mp4, Mkv, Mov, Avi, ContainerCount };
    static const wchar_t *const Codecs[CodecCount], *const Backends[BackendCount], *const Containers[ContainerCount], *const Extensions[ContainerCount];
    static bool HasBackend( int iCodec, int iBackend );     // ProRes and FFV1 are software only
    static bool Holds( int iContainer, int iCodec );        // the file format can store the codec
    static const wchar_t *EncoderName( int iCodec, int iBackend );     // ffmpeg's name for the encoder
    static bool Is420( int iCodec ) { return iCodec <= Av1; }   // encoded as 4:2:0 (the rest from RGB frames)
    // A frame side's limits: 65536 is the most any ffmpeg encoder takes (each one's own limit is found by
    // trying it, VideoExport.cpp); 4:2:0 and 4:2:2 need even sizes
    static const int MaxSide = 65536;
};

struct PerformanceSettings
{
    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    int iWorkerThreads;         // loading and drawing (and video rendering); 0 = auto
    int iPlaybackThreads;       // the most threads decoding playback; 0 = auto
    bool bPreferKDMAPI;
    bool bTuning31;
    bool bTuning31PitchBend;
    bool bKdmapiLongSends;      // KDMAPI: each batch goes out as one long message of raw MIDI bytes (off: one call an event)
    bool bVelocitySkip;         // skip quiet note-ons while the output lags (off: every event is sent)
    int iSkipLagMs;             // the lag above which velocity skipping starts
    bool bVelocityFloor;        // always skip note-ons quieter than iMinVelocity
    int iMinVelocity;
    bool bCollapseStacks;       // one voice per channel, key and tick (PlaybackDecoder::Params::bCollapse)
    bool bSendLimit;            // the send budget, iMaxSendsPerSec messages per second
    int iMaxSendsPerSec;
    bool bCompactVisual;
    int iBlockCacheMB;
    bool bLodCache;             // the fast renderer's LOD cache (DESIGN.md 8.11)
    bool bIgnoreWeird;          // keep each note's first pairing (Song::bFirstPairingOnly); off: weird notes end at their later off
    bool bEndOpenNotes;         // layered compressed files: notes without a note-off last until the song's end (Song::bEndOpenNotes); on by default
    bool bExactTempo;           // exact tempo timing (TempoMap::bExact); on by default, off: the original's rounding per tempo segment
    bool bGpuInstancing;        // notes drawn by Renderer::DrawNoteInstances (not pixel-exact), off by default
    bool bDedup;                // identical tracks, and repeats in layered compressed files, loaded once (song/Dedup.h); off by default
    bool bDedupAssume;          // with it: the rest of a layer taken as repeats of its first tile when its size fits (on by default)
    bool bShowLoadStats;
};

class PlaybackSettings
{
public:
    enum Metronome { Off, EveryBeat, EveryMeasure };

    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    void ToggleMute( bool bUpdateGUI = false ) { SetMute( !m_bMute, bUpdateGUI ); }
    void TogglePaused( bool bUpdateGUI = false ) { SetPaused( !m_bPaused, bUpdateGUI ); }
    void SetPosition( int iPosition );

    // Set accessors optionally update the GUI, as in the original.
    void SetPlayMode( PlayMode::State ePlayMode, bool bUpdateGUI = false );
    void SetPlayable( bool bPlayable, bool bUpdateGUI = false );
    void SetPaused( bool bPaused, bool bUpdateGUI = false );
    void SetStopped( bool bUpdateGUI = false );
    void SetSpeed( double dSpeed, bool bUpdateGUI = false );
    void SetNSpeed( double dNSpeed, bool bUpdateGUI = false );
    void SetVolume( double dVolume, bool bUpdateGUI = false );
    void SetMute( bool bMute, bool bUpdateGUI = false );
    void SetMetronome( Metronome eMetronome, bool bUpdateGUI = false );

    PlayMode::State GetPlayMode() const { return m_ePlayMode; }
    bool GetPlayable() const { return m_bPlayable; }
    bool GetPaused() const { return m_bPaused; }
    bool GetMute() const { return m_bMute; }
    double GetSpeed() const { return m_dSpeed; }
    double GetNSpeed() const { return m_dNSpeed; }
    double GetVolume() const { return m_dVolume; }
    Metronome GetMetronome() const { return m_eMetronome; }

private:
    PlayMode::State m_ePlayMode;
    bool m_bPlayable, m_bPaused, m_bMute;
    double m_dSpeed, m_dNSpeed, m_dVolume;
    Metronome m_eMetronome;
};

class ViewSettings
{
public:
    void LoadDefaultValues();
    void Load( const IniFile &ini );
    void Save( IniFile &ini ) const;

    void ToggleControls( bool bUpdateGUI = false ) { SetControls( !m_bControls, bUpdateGUI ); }
    void ToggleKeyboard( bool bUpdateGUI = false ) { SetKeyboard( !m_bKeyboard, bUpdateGUI ); }
    void ToggleOnTop( bool bUpdateGUI = false ) { SetOnTop( !m_bOnTop, bUpdateGUI ); }
    void ToggleFullScreen( bool bUpdateGUI = false ) { SetFullScreen( !m_bFullScreen, bUpdateGUI ); }

    void SetMainPos( int iMainLeft, int iMainTop ) { m_iMainLeft = iMainLeft; m_iMainTop = iMainTop; }
    void SetMainSize( int iMainWidth, int iMainHeight ) { m_iMainWidth = iMainWidth; m_iMainHeight = iMainHeight; }
    void SetOffsetX( float f ) { m_fOffsetX = f; }
    void SetOffsetY( float f ) { m_fOffsetY = f; }
    void SetZoomX( float f ) { m_fZoomX = f; }
    void SetControls( bool b, bool bUpdateGUI = false );
    void SetKeyboard( bool b, bool bUpdateGUI = false );
    void SetOnTop( bool b, bool bUpdateGUI = false );
    void SetFullScreen( bool b, bool bUpdateGUI = false );
    void SetZoomMove( bool b, bool bUpdateGUI = false );
    void SetTheme( int eTheme, bool bUpdateGUI = false );   // DarkMode::Theme

    int GetMainLeft() const { return m_iMainLeft; }
    int GetMainTop() const { return m_iMainTop; }
    int GetMainWidth() const { return m_iMainWidth; }
    int GetMainHeight() const { return m_iMainHeight; }
    float GetOffsetX() const { return m_fOffsetX; }
    float GetOffsetY() const { return m_fOffsetY; }
    float GetZoomX() const { return m_fZoomX; }
    bool GetControls() const { return m_bControls; }
    bool GetKeyboard() const { return m_bKeyboard; }
    bool GetOnTop() const { return m_bOnTop; }
    bool GetFullScreen() const { return m_bFullScreen; }
    bool GetZoomMove() const { return m_bZoomMove; }
    int GetTheme() const { return m_eTheme; }

private:
    bool m_bControls, m_bKeyboard, m_bOnTop, m_bFullScreen, m_bZoomMove;
    float m_fOffsetX, m_fOffsetY, m_fZoomX;
    int m_iMainLeft, m_iMainTop, m_iMainWidth, m_iMainHeight;
    int m_eTheme;
};

class Config
{
public:
    static Config &GetConfig();
    static std::wstring GetFolder();   // %APPDATA%\PFAviz++ (created on demand)

    void LoadDefaultValues();
    void LoadConfigValues();
    // bIfChanged: only when the settings differ from the last save (the main window's periodic save,
    // so settings are kept even when the program does not end normally)
    bool SaveConfigValues( bool bIfChanged = false );
    void LoadMIDIDevices() { std::lock_guard< std::mutex > lock( m_mxAudio ); m_AudioSettings.LoadMIDIDevices(); }
    // The audio settings change on the UI thread and are read by the game and playback threads
    // (MidiOutputs): those readers hold this, and so do the two writers above and below
    std::mutex &AudioMutex() { return m_mxAudio; }

    const VisualSettings &GetVisualSettings() const { return m_VisualSettings; }
    const AudioSettings &GetAudioSettings() const { return m_AudioSettings; }
    AudioSettings CopyAudioSettings() { std::lock_guard< std::mutex > lock( m_mxAudio ); return m_AudioSettings; }
    const VideoSettings &GetVideoSettings() const { return m_VideoSettings; }
    const ControlsSettings &GetControlsSettings() const { return m_ControlsSettings; }
    const PerformanceSettings &GetPerformanceSettings() const { return m_PerformanceSettings; }
    const ExportSettings &GetExportSettings() const { return m_ExportSettings; }
    PlaybackSettings &GetPlaybackSettings() { return m_PlaybackSettings; }
    ViewSettings &GetViewSettings() { return m_ViewSettings; }

    // The game thread reads the visual settings every frame: their string (the background image's file)
    // only through GetBkgImage, under the lock this takes, when VisualGen has changed
    void SetVisualSettings( const VisualSettings &s ) { std::lock_guard< std::mutex > lock( m_mxVisual ); m_VisualSettings = s; m_uVisualGen++; }
    std::wstring GetBkgImage() const    // empty when none is shown
    {
        std::lock_guard< std::mutex > lock( m_mxVisual );
        return m_VisualSettings.bBkgImage ? m_VisualSettings.sBkgImage : std::wstring();
    }
    unsigned VisualGen() const { return m_uVisualGen.load(); }
    void SetAudioSettings( const AudioSettings &s ) { std::lock_guard< std::mutex > lock( m_mxAudio ); m_AudioSettings = s; }
    void SetVideoSettings( const VideoSettings &s ) { m_VideoSettings = s; }
    void SetControlsSettings( const ControlsSettings &s ) { m_ControlsSettings = s; }
    void SetPerformanceSettings( const PerformanceSettings &s ) { m_PerformanceSettings = s; }
    void SetExportSettings( const ExportSettings &s ) { m_ExportSettings = s; }

private:
    IniFile Settings() const;               // this program's settings as keys
    void RememberSaved();                   // m_pMine and m_sSaved from the settings now
    Config();
    Config( const Config & ) = delete;
    Config &operator=( const Config & ) = delete;

    VisualSettings m_VisualSettings;
    mutable std::mutex m_mxVisual;
    std::atomic< unsigned > m_uVisualGen{ 0 };
    std::mutex m_mxAudio;
    std::string m_sSaved;                   // this program's settings as last loaded or saved (SaveConfigValues)
    IniFile *m_pMine = nullptr;             // the same, as keys: what it changes is what differs from these
    AudioSettings m_AudioSettings;
    VideoSettings m_VideoSettings;
    ControlsSettings m_ControlsSettings;
    PerformanceSettings m_PerformanceSettings;
    ExportSettings m_ExportSettings;
    PlaybackSettings m_PlaybackSettings;
    ViewSettings m_ViewSettings;
};
