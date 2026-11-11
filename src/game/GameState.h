// Game states: intro (blank), splash (startup song) and the main screen.
// MainScreen is a close port of the original MainScreen with input, learn, scoring, labels and loop
// removed. Notes come from the fast renderer (visual/FastEngine), which the test harness proves
// pixel-identical to the original's event-by-event drawing (DESIGN.md Section 12).
#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio/Clock.h"
#include "audio/MidiOut.h"
#include "audio/PlaybackEngine.h"
#include "core/Util.h"
#include "midi/RefMidi.h"
#include "render/Image.h"
#include "render/Renderer.h"
#include "song/PfaLag.h"
#include "visual/Effects.h"
#include "visual/FastEngine.h"
#include "visual/Scene.h"

struct VideoJob;

class GameState
{
public:
    enum GameError { Success = 0, BadPointer, OutOfMemory, DirectXError };

    static const wchar_t *const Errors[];
    // The screen replaced is deleted on a thread of its own (Dispose)
    static GameError ChangeState( GameState *pNextState, GameState **pDestObj );
    // Deletes a screen off the game thread, so a big song's teardown (its notes freed, its threads
    // joined, its synth stopped) does not stop the frames. Its audio goes first
    static void Dispose( GameState *pState );
    static void WaitForReleasedAudio();     // until the audio of every screen disposed of is released

    GameState( HWND hWnd, Renderer *pRenderer ) : m_hWnd( hWnd ), m_pRenderer( pRenderer ) {}
    virtual ~GameState() {}

    virtual GameError Init() = 0;
    virtual GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) = 0;
    virtual GameError Logic() = 0;
    virtual GameError Render() = 0;
    // Renders the open song to a video (app/VideoExport.h); the job is done when this returns
    virtual void RenderVideo( const std::shared_ptr< VideoJob > &pJob );
    // Stops the screen's audio and closes its MIDI output (Dispose, before deleting it)
    virtual void ReleaseAudio() {}
    // Its sound stopped at once, on the window's thread, as it is closed (ReleaseAudio follows)
    virtual void SilenceAudio() {}
    // Whether Init opens a MIDI output: it then waits for the screens before it to close theirs
    virtual bool UsesAudio() const { return false; }

    GameState *NextState() { return m_pNextState; }
    void SetHWnd( HWND hWnd ) { m_hWnd = hWnd; }
    void SetRenderer( Renderer *pRenderer ) { m_pRenderer = pRenderer; }

protected:
    HWND m_hWnd;
    Renderer *m_pRenderer;
    GameState *m_pNextState = nullptr;
};

class IntroScreen : public GameState
{
public:
    IntroScreen( HWND hWnd, Renderer *pRenderer ) : GameState( hWnd, pRenderer ) {}
    GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;
    GameError Init() override { return Success; }
    GameError Logic() override;
    GameError Render() override;
};

class SplashScreen : public GameState
{
public:
    SplashScreen( HWND hWnd, Renderer *pRenderer );
    GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;
    GameError Init() override { return Success; }
    void ReleaseAudio() override { m_OutDevice.Close(); }
    bool UsesAudio() const override { return true; }
    GameError Logic() override;
    GameError Render() override;

private:
    void InitState();
    void SetChannelColors( const std::vector< unsigned > &vColor );
    void UpdateState( int iPos );
    void RenderGlobals();
    void RenderNotes();
    void RenderNote( int iPos );
    float GetNoteX( int iNote );

    RefMidi m_MIDI;
    int m_iStartPos, m_iEndPos;
    long long m_llStartTime;
    std::vector< int > m_vState;
    Timer m_Timer;
    double m_dVolume;
    bool m_bPaused, m_bMute;
    MidiOut m_OutDevice;

    static const float SharpRatio;
    static const long long TimeSpan = 3000000;
    std::vector< TrackSettings > m_vTrackSettings;

    int m_iStartNote, m_iEndNote;
    float m_fNotesX, m_fNotesY, m_fNotesCX, m_fNotesCY;
    int m_iAllWhiteKeys;
    float m_fWhiteCX;
    long long m_llRndStartTime;
};

// Fading text message ("Muting Track 3"), from the original TextPath.
class TextPath
{
public:
    struct TextPathVertex { float x, y, t; int a; };
    void SetPath( const std::vector< TextPathVertex > &vPath ) { m_vPath = vPath; }
    void SetFont( Renderer::FontSize fFont ) { m_fFont = fFont; }
    void Reset( float xOffset, float yOffset, unsigned iColor, const wchar_t *sText )
        { m_t = 0.0f; m_iPos = 0; m_xOffset = xOffset; m_yOffset = yOffset; m_iColor = iColor; m_sText = sText; }
    bool IsAlive() const { return m_iPos < static_cast< int >( m_vPath.size() ) - 1; }
    void Kill() { m_iPos = (int)m_vPath.size() - 1; }
    void Logic( long long llElapsed );
    void Render( Renderer *pRenderer, float xOffset, float yOffset );

private:
    std::vector< TextPathVertex > m_vPath;
    Renderer::FontSize m_fFont = Renderer::Large;
    float m_xOffset = 0, m_yOffset = 0, m_t = 0, m_x = 0, m_y = 0;
    int m_iPos = 0;
    unsigned m_iColor = 0;
    const wchar_t *m_sText = nullptr;
};

class MainScreen : public GameState
{
public:
    static const float KBPercent;

    // bPlayback false: no playback engine (File > Render Song: nothing is sent to the synth)
    MainScreen( std::shared_ptr< const Song > pSong, HWND hWnd, Renderer *pRenderer, bool bPlayback = true );
    ~MainScreen();
    void ReleaseAudio() override { m_pPlayback.reset(); }
    void SilenceAudio() override { if ( m_pPlayback ) m_pPlayback->Silence(); }
    bool UsesAudio() const override { return m_bWithPlayback; }

    GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;
    GameError Init() override;
    GameError Logic() override;
    GameError Render() override;
    void RenderVideo( const std::shared_ptr< VideoJob > &pJob ) override;

    bool IsValid() const { return m_pSong != nullptr; }
    const Song &GetSong() const { return *m_pSong; }
    // Whether Init pauses the song or plays it. Set when the song is loaded and applied when this screen
    // takes over, so the screen it replaces (still running until then) cannot change it
    void SetStartPaused( bool b ) { m_bStartPaused = b; }

    void MuteChannel( int iTrack, int iChannel, bool bMuted );
    void HideChannel( int iTrack, int iChannel, bool bHidden );
    void ColorChannel( int iTrack, int iChannel, unsigned int iColor, bool bRandom = false );
    ChannelSettings *GetChannelSettings( int iChannel, int *piTrack = nullptr, int *piChannel = nullptr );
    // The first 16 colour slots (track, channel with notes) take the configured colours, the rest random ones
    void SetChannelColors( const unsigned *aColor, int iColors );

private:
    void InitState();
    void InitSong();
    void JumpTo( long long llStartTime, bool bUpdateGUI = true );
    void AdvanceIterators( long long llTime, bool bIsJump );
    long long GetMinTime() const { return m_pSong->info.llFirstNote - 3000000; }
    long long GetMaxTime() const { return m_pSong->info.llTotalMicroSecs + 500000; }

    // At song time llSongTime (the view's start: FastEngine::ToView, through the window's engine)
    ViewInputs GetViewInputs( int iWidth, int iHeight, long long llSongTime ) const;
    void RenderGlobals();
    // The scene (background, measure lines, notes, keyboard, border) under the effects (visual/Effects.h),
    // then fnStats( fDark ) for the stats box: inside the effects (fDark: their darkening) when they apply
    // to it, else over them (1). With the effects off, exactly the original's drawing. pNotesUs: the
    // times DrawNotes started and ended.
    void DrawFrame( Renderer *pRenderer, const FrameGlobals &g, FastEngine &engine, const MeterTracker &meter, long long iTick,
                    Effects::Look &look, Effects::KeyLights &lights, const std::function< void( float ) > &fnStats, long long *pNotesUs = nullptr );
    void RenderStats( float fDark );        // the live stats box
    void RenderText();                      // the live messages
    // The stats box: its line count (0: not drawn) and its text, with the lines the settings show
    // The stats box for the lines in uStats (VisualSettings::Stat bits)
    static int StatusLines( unsigned uStats );
    // Where the box goes in an iWidth x iHeight picture: its size set in pixels, or as at 1080p scaled
    // with the height (Visual tab), with its line spacing, padding and font from its height over the lines, in the configured corner (iBottomY: where
    // a bottom one ends)
    struct StatsBox
    {
        RECT rc;
        int iLineCY, iPadX, iPadY, iShadowX, iShadowY;
        int iFontCY;                    // 0: the Small font (fixed pixels); else Renderer::Stats at this height
                                        // (in VisualSettings::sStatsFont)
    };
    // iTextCX: the widest line's text (StatsTextCX); the box is at least as wide, with its padding
    // The time the notes drawn reach to: the frame's start plus its span (with Clean::KeepBottom, past it
    // by at least the row the start's rounding up adds at the top)
    long long ViewEnd( long long llStartTime ) const;
    static StatsBox StatsLayout( int iWidth, int iHeight, int iLines, int iBottomY, int iTextCX = 0 );
    // The box for the lines in uStats, as wide as its font needs for them (pRenderer: to measure)
    static StatsBox FitStats( Renderer *pRenderer, int iWidth, int iHeight, unsigned uStats, int iBottomY );
    static void DrawStatsBackground( Renderer *pRenderer, const StatsBox &box );     // (unless it is off)
    // The bottom corners' iBottomY: the top of the keyboard's red bar, or the picture's bottom without it
    int StatsBottom( const FrameGlobals &g, int iHeight ) const;
    // llPastEnd: how long the live view has stood at the song's end, so the NPS falls to 0 there too
    // fDark: the text's colours times this (the effects' dark room)
    void RenderStatus( Renderer *pRenderer, const StatsBox &box, long long llTime, double dFPS, unsigned uStats, long long llPastEnd = 0, float fDark = 1.0f );
    void RenderMessage( LPRECT prcMsg, const wchar_t *sMsg );
    // The graphs (VisualSettings::bGraph) in an iWidth x iHeight picture, at llTime (llPastEnd as
    // RenderStatus's); pStats: the stats box drawn, if any (the graphs go next to it in one corner);
    // iBottomY as StatsLayout's. Each graph's top and bottom follow its values shown, eased by song time
    // (GraphScale: kept from frame to frame)
    struct GraphScale { double adTop[3] = {}, adBottom[3] = {}; long long llLast = LLONG_MIN; };
    void RenderGraph( Renderer *pRenderer, int iWidth, int iHeight, int iBottomY, long long llTime, long long llPastEnd,
                      const StatsBox *pStats, float fDark, GraphScale &scale );

    // Song
    std::shared_ptr< const Song > m_pSong;
    FastEngine *m_pEngine = nullptr;
    MeterTracker *m_pMeter = nullptr;

    // Playback
    long long m_llStartTime, m_llTimeSpan;
    long long m_llPastEndUs = 0;        // real time spent at the song's end (capped), for the NPS line
    long long m_iStartTick;
    double m_dSpeed;
    bool m_bPaused;
    bool m_bStartPaused = false;
    Timer m_Timer;
    bool m_bMute;
    double m_dVolume, m_dVolumeCorrect;

    // Notifications
    wchar_t m_sBuf[128];
    TextPath m_tpMessage;

    // FPS
    unsigned m_uStats = 0;              // the stats box lines shown live (VisualSettings::uStats, less what has no data)
    int m_iFPSCount;
    long long m_llFPSTime;
    double m_dFPS;

    // Debug statistics (Performance tab option; not shown by default). The synth's are sampled every
    // SynthStepUs, and its rates taken over the last SynthSamples - 1 steps (half a second)
    static const long long SynthStepUs = 100000;
    static const int SynthSamples = 6;
    struct SynthSample { long long llUs = 0; PlaybackEngine::Stats s; };
    SynthSample m_aSynth[SynthSamples];     // a ring; m_iSynth is the newest
    int m_iSynth = 0, m_iSynthCount = 0;
    long long m_llSynthUs = 0, m_llSynthStepUs = 0;
    double m_dSentPerSec = 0.0, m_dSkippedPerSec = 0.0, m_dCollapsedPerSec = 0.0;
    // The playback pipeline over that window: messages decoded per second, the share of the time each
    // thread worked, and the ring's average and peak fill (share of its slices)
    double m_dDecodedPerSec = 0.0, m_dDecoderBusy = 0.0, m_dSenderBusy = 0.0, m_dRingAvg = 0.0, m_dRingMax = 0.0;
    int m_iLanes = 1;                   // the most decoding threads in that window
    // Frame time parts (playback statistics): outside Render (Logic, messages, waiting), DrawNotes,
    // the rest of the scene, Present. Sums and maxima of the current half second, then shown.
    enum FramePart { PartOutside, PartNotes, PartScene, PartPresent, PartCount };
    double m_adPartSum[PartCount] = {}, m_adPartMax[PartCount] = {};
    double m_adPartAvgShown[PartCount] = {}, m_adPartMaxShown[PartCount] = {};
    int m_iPartFrames = 0;
    long long m_llRenderEnd = 0;
    long long m_llEnvRunUs = 0;         // PFAVIZ_EXIT_AFTER
    PlaybackEngine::Stats m_Stats;
    int m_iSynthVoices = -1;            // KDMAPI's GetActiveVoices and GetRenderingTime (-1: not available)
    wchar_t m_sRendered[48] = L"";      // audio rendered ahead: how far it has got, or its problem
    std::wstring m_sRenderedShown;      // the problem last told on screen
    wchar_t m_sDegrade[48] = L"";       // how much lighter the rendered audio playing is
    std::wstring m_sWarningShown;       // the rendered audio's load warnings last told
    double m_dSynthCpu = -1.0;

    // Audio: the clock drives both the visuals (sampled once per frame) and the playback threads
    PlaybackClock m_Clock;
    // The PFA lag simulation (song/PfaLag.h): the original's frame running (song time m_llSimCur,
    // from wall time m_llSimWallStart to m_llSimWallEnd), and the time of the last one it drew. Its
    // frames run back to back, many per frame here when they are short. They are worked out a quarter
    // second ahead (m_vSim, the running one first) and the clock is given all their ramps at once, so
    // its pace never waits on this thread's frames
    struct SimFrame
    {
        long long llWallStart, llWallEnd, llFrom, llTo;
        std::vector< PlaybackClock::RampKnot > vKnots;
    };
    void SimulateFrame( long long llMaxTime );
    void ResetSim( long long llTime );
    static PfaLag::Cpu SimCpu( const VisualSettings &cVisual );
    std::vector< SimFrame > m_vSim;
    PfaLag::LagTracker m_SimLag;        // how long it has been lagging (the stats box's "Lag time")
    long long m_llSimNextWall = 0, m_llSimNextFrom = 0, m_llSimNoiseWall = 0;   // where the next frame to work out starts
    unsigned long long m_llSimKey = 0;  // the settings the frames ahead were worked out with
    bool m_bSimOn = false, m_bSimWarned = false;
    PfaLag::Cpu m_SimCpu;
    PfaLag::Noise m_SimNoise;
    long long m_llSimWallStart = 0, m_llSimWallEnd = 0, m_llSimCur = 0, m_llSimShown = 0;
    double m_dSimFrameUs = 0.0;     // the last frame's length
    long long m_llSimStatsTime = 0; // how far its Logic had played when last read (the statistics, with bPfaLiveStats)
    int m_iSimFrames = 0;           // its frames done since the frame rate was last computed
    unsigned m_uSimVisualGen = 0;   // the visual settings the cache sizes were read from
    double m_dRefreshUs = 0.0;      // the display's refresh period (frames wait for it with Limit FPS)
    std::unique_ptr< PlaybackEngine > m_pPlayback;
    bool m_bWithPlayback = true;        // false: no m_pPlayback (File > Render Song)

    // Visual
    bool m_bShowKB;
    SceneColors m_Colors;
    std::vector< TrackSettings > m_vTrackSettings;
    unsigned m_uTracksGen = 0;      // counts changes to m_vTrackSettings (the effects copy its colours)
    std::vector< unsigned > m_vPalette;     // the track colours (and bLoopColors, bUniqueColors, last) the song was coloured from (empty: not seen yet)
    Util::UniqueColors *m_pUnique = nullptr;    // SetChannelColors, unique random colours: their source while it colours
    Effects::Look m_Look;
    Effects::KeyLights m_KeyLights;
    GraphScale m_GraphScale;
    bool m_bHiddenChanged = false;  // the draw view needs the new hidden channels
    // The background image, loaded when its file setting changes (shared with the video renderer)
    std::shared_ptr< const Image > m_pBkgImage;
    std::wstring m_sBkgImage;
    bool m_bBkgApplied = false;
    unsigned m_uVisualGen = 0;          // Config::VisualGen when the file was last looked at
    FrameGlobals m_G;

    float m_fZoomX, m_fOffsetX, m_fOffsetY;
    float m_fTempZoomX, m_fTempOffsetX, m_fTempOffsetY;
    bool m_bZoomMove, m_bTrackPos, m_bTrackZoom;
    POINT m_ptStartZoom, m_ptLastPos;
};
