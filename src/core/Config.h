// PFAviz++ configuration. Same settings as the original (minus input, learn, labels and library),
// plus the Performance tab. Stored as UTF-8 INI in %APPDATA%\PFAviz++\config.ini.
#pragma once

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
                StatDecoded, StatDecoderBusy, StatSenderBusy, StatRing, StatSendCost, StatTempo, StatVoices, StatSynthCpu, StatCC, StatCount };
    static constexpr unsigned NoteStats = 7u << StatNotes;      // counted while a song loads (Song::noteStats)
    static constexpr unsigned PipelineStats = 31u << StatDecoded;
    static constexpr unsigned VoiceStats = 3u << StatVoices;    // asked of the synth (KDMAPI only)
    static constexpr unsigned SynthStats = 31u << StatSent | PipelineStats | VoiceStats;   // need a MIDI output
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
    unsigned int colors[16], iBkgColor;
    bool bBkgImage;             // the background image is shown
    std::wstring sBkgImage;     // its file, drawn over iBkgColor
    int iBkgFit;                // Renderer::ImageFit
    int iBkgOpacity;            // percent
    unsigned uClean;            // Clean flags (visual/Scene.h): the original's rounding quirks left out
    bool bHideLines;            // no key dividers or measure lines over the background
    bool bHideSpeedBoxes;       // the toolbar as the original's: sliders only, without the exact speed boxes
    bool bLoopColors;           // the track colors repeat (channel n gets colors[n % 16]); off: random after the 16th
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
    std::wstring sAudioFile;    // muxed in when set (anything ffmpeg reads: WAV up to 64-bit float, FLAC up to 24-bit, ...)
    bool bAudioAlign;           // its first sound is moved to where the song's first note sounds
    int iAudioOffsetMs;         // and then by this much (positive: later)
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
    bool bKdmapiLongSends;      // KDMAPI: batches go out as long messages of raw MIDI bytes (off: one call an event)
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
    bool bGpuInstancing;        // notes drawn by Renderer::DrawNoteInstances (not pixel-exact), off by default
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

private:
    bool m_bControls, m_bKeyboard, m_bOnTop, m_bFullScreen, m_bZoomMove;
    float m_fOffsetX, m_fOffsetY, m_fZoomX;
    int m_iMainLeft, m_iMainTop, m_iMainWidth, m_iMainHeight;
};

class Config
{
public:
    static Config &GetConfig();
    static std::wstring GetFolder();   // %APPDATA%\PFAviz++ (created on demand)

    void LoadDefaultValues();
    void LoadConfigValues();
    bool SaveConfigValues();
    void LoadMIDIDevices() { std::lock_guard< std::mutex > lock( m_mxAudio ); m_AudioSettings.LoadMIDIDevices(); }
    // The audio settings change on the UI thread and are read by the game and playback threads
    // (MidiOutputs): those readers hold this, and so do the two writers above and below
    std::mutex &AudioMutex() { return m_mxAudio; }

    const VisualSettings &GetVisualSettings() const { return m_VisualSettings; }
    const AudioSettings &GetAudioSettings() const { return m_AudioSettings; }
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
    Config();
    Config( const Config & ) = delete;
    Config &operator=( const Config & ) = delete;

    VisualSettings m_VisualSettings;
    mutable std::mutex m_mxVisual;
    std::atomic< unsigned > m_uVisualGen{ 0 };
    std::mutex m_mxAudio;
    AudioSettings m_AudioSettings;
    VideoSettings m_VideoSettings;
    ControlsSettings m_ControlsSettings;
    PerformanceSettings m_PerformanceSettings;
    ExportSettings m_ExportSettings;
    PlaybackSettings m_PlaybackSettings;
    ViewSettings m_ViewSettings;
};
