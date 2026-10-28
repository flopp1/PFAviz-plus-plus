// Game states: intro (blank), splash (startup song) and the main screen.
// MainScreen is a close port of the original MainScreen with input, learn, scoring, labels and loop
// removed. Notes come from the fast renderer (visual/FastEngine), which the test harness proves
// pixel-identical to the original's event-by-event drawing (DESIGN.md Section 12).
#pragma once

#include <windows.h>
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
#include "visual/FastEngine.h"
#include "visual/Scene.h"

struct VideoJob;

class GameState
{
public:
    enum GameError { Success = 0, BadPointer, OutOfMemory, DirectXError };

    static const wchar_t *const Errors[];
    static GameError ChangeState( GameState *pNextState, GameState **pDestObj );

    GameState( HWND hWnd, Renderer *pRenderer ) : m_hWnd( hWnd ), m_pRenderer( pRenderer ) {}
    virtual ~GameState() {}

    virtual GameError Init() = 0;
    virtual GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) = 0;
    virtual GameError Logic() = 0;
    virtual GameError Render() = 0;
    // Renders the open song to a video (app/VideoExport.h); the job is done when this returns
    virtual void RenderVideo( const std::shared_ptr< VideoJob > &pJob );

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

    MainScreen( std::shared_ptr< const Song > pSong, HWND hWnd, Renderer *pRenderer );
    ~MainScreen();

    GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;
    GameError Init() override;
    GameError Logic() override;
    GameError Render() override;
    void RenderVideo( const std::shared_ptr< VideoJob > &pJob ) override;

    bool IsValid() const { return m_pSong != nullptr; }
    const Song &GetSong() const { return *m_pSong; }

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

    ViewInputs GetViewInputs( int iWidth, int iHeight, long long llStartTime ) const;
    void RenderGlobals();
    void RenderText();
    // The stats box: its line count (0: not drawn) and its text, with the lines the settings show
    // The stats box for the lines in uStats (VisualSettings::Stat bits)
    static int StatusLines( unsigned uStats );
    // llPastEnd: how long the live view has stood at the song's end, so the NPS falls to 0 there too
    void RenderStatus( Renderer *pRenderer, LPRECT prcPos, long long llTime, double dFPS, unsigned uStats, long long llPastEnd = 0 );
    void RenderMessage( LPRECT prcMsg, const wchar_t *sMsg );

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
    double m_dSynthCpu = -1.0;

    // Audio: the clock drives both the visuals (sampled once per frame) and the playback threads
    PlaybackClock m_Clock;
    std::unique_ptr< PlaybackEngine > m_pPlayback;

    // Visual
    bool m_bShowKB;
    SceneColors m_Colors;
    std::vector< TrackSettings > m_vTrackSettings;
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
