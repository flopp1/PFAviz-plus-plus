// MainScreen: port of the original practice-mode screen (now just playback). Notes and key lighting come from
// FastEngine and Scene, which the test harness checks against the original's logic every frame.
// Audio comes from PlaybackEngine on its own threads; this screen only drives the clock.
#include "game/GameState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/MainWindow.h"
#include "core/Config.h"
#include "resource.h"

using std::max;
using std::min;
using std::pow;

const float MainScreen::KBPercent = 0.25f;

MainScreen::MainScreen( std::shared_ptr< const Song > pSong, HWND hWnd, Renderer *pRenderer )
    : GameState( hWnd, pRenderer ), m_pSong( std::move( pSong ) )
{
    InitSong();
}

void MainScreen::InitSong()
{
    if ( !m_pSong ) return;

    m_pEngine = new FastEngine( *m_pSong, Config::GetConfig().GetPerformanceSettings().iWorkerThreads );
    m_pEngine->SetLod( Config::GetConfig().GetPerformanceSettings().bLodCache );
    m_pMeter = new MeterTracker( m_pSong->vMeta, m_pSong->info );
    m_vTrackSettings.resize( m_pSong->info.iNumTracks );
    if ( m_pSong ) m_pPlayback = std::make_unique< PlaybackEngine >( m_pSong, m_Clock );

    std::vector< TextPath::TextPathVertex > vPath;
    vPath.push_back( { 0.0f, 0.0f, 0.0f, 0xFF } );
    vPath.push_back( { 0.0f, 0.0f, 1.0f, 0xFF } );
    vPath.push_back( { 0.0f, 0.0f, 1.1f, 0x00 } );
    m_tpMessage.SetPath( vPath );
    m_tpMessage.SetFont( Renderer::Large );
    m_tpMessage.Kill();

    InitState();
}

MainScreen::~MainScreen()
{
    m_pPlayback.reset();
    delete m_pEngine;
    delete m_pMeter;
}

void MainScreen::InitState()
{
    static Config &config = Config::GetConfig();
    static const PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static const ViewSettings &cView = config.GetViewSettings();

    m_pEngine->Reset();
    m_llStartTime = GetMinTime();
    m_bTrackPos = m_bTrackZoom = false;
    m_fTempZoomX = 1.0f;
    m_fTempOffsetX = m_fTempOffsetY = 0.0f;
    m_ptLastPos = { 0, 0 };
    m_ptStartZoom = { 0, 0 };
    m_dFPS = 0.0;
    m_iFPSCount = 0;
    m_llFPSTime = 0;
    m_dSpeed = -1.0;
    m_dVolumeCorrect = 1.0;

    m_fZoomX = cView.GetZoomX();
    m_fOffsetX = cView.GetOffsetX();
    m_fOffsetY = cView.GetOffsetY();
    m_bPaused = false;
    m_bMute = cPlayback.GetMute();
    m_dVolume = cPlayback.GetVolume();
    m_llTimeSpan = static_cast< long long >( 3.0 * cPlayback.GetNSpeed() * 1000000 );
    m_bShowKB = cView.GetKeyboard();
    m_bZoomMove = false;

    AdvanceIterators( m_llStartTime, true );
}

GameState::GameError MainScreen::Init()
{
    static Config &config = Config::GetConfig();
    const PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    if ( m_pPlayback )
    {
        const PerformanceSettings &cPerf = config.GetPerformanceSettings();
        m_pPlayback->SetSkipping( cPerf.bVelocitySkip, cPerf.iSkipLagMs );
        m_pPlayback->SetSendLimits( cPerf.bCollapseStacks, cPerf.bSendLimit ? cPerf.iMaxSendsPerSec : 0 );
        m_pPlayback->SetMinVelocity( cPerf.bVelocityFloor ? cPerf.iMinVelocity : 0 );
        m_pPlayback->SetMute( m_bMute );
        m_pPlayback->SetMetronome( cPlayback.GetMetronome() );
        m_Clock.SetSpeed( cPlayback.GetSpeed() );
        m_Clock.SetPaused( cPlayback.GetPaused() );
        m_pPlayback->Start( m_llStartTime );
    }
    else m_Clock.Set( m_llStartTime );
    m_Timer.Start();
    return Success;
}

void MainScreen::HideChannel( int iTrack, int iChannel, bool bHidden )
{
    bool &b = m_vTrackSettings[iTrack].aChannels[iChannel].bHidden;
    if ( b != bHidden ) m_bHiddenChanged = true;
    b = bHidden;
}

void MainScreen::MuteChannel( int iTrack, int iChannel, bool bMuted )
{
    m_vTrackSettings[iTrack].aChannels[iChannel].bMuted = bMuted;
    if ( m_pPlayback ) m_pPlayback->SetChannelMuted( iTrack, iChannel, bMuted );
}

void MainScreen::ColorChannel( int iTrack, int iChannel, unsigned int iColor, bool bRandom )
{
    if ( bRandom ) m_vTrackSettings[iTrack].aChannels[iChannel].SetColor();
    else m_vTrackSettings[iTrack].aChannels[iChannel].SetColor( iColor );
}

ChannelSettings *MainScreen::GetChannelSettings( int iTrack, int *piTrack, int *piChannel )
{
    const std::vector< RefTrackInfo > &vTracks = m_pSong->vTracks;
    int iPos = 0;
    for ( size_t i = 0; i < vTracks.size(); i++ )
        for ( int j = 0; j < 16; j++ )
            if ( vTracks[i].aNoteCount[j] > 0 )
            {
                if ( iPos == iTrack )
                {
                    if ( piTrack ) *piTrack = (int)i;
                    if ( piChannel ) *piChannel = j;
                    return &m_vTrackSettings[i].aChannels[j];
                }
                iPos++;
            }
    return nullptr;
}

void MainScreen::SetChannelColors( const unsigned aColor[16] )
{
    const std::vector< RefTrackInfo > &vTracks = m_pSong->vTracks;
    size_t iPos = 0;
    for ( size_t i = 0; i < vTracks.size(); i++ )
        for ( int j = 0; j < 16; j++ )
            if ( vTracks[i].aNoteCount[j] > 0 )
            {
                if ( iPos < 16 ) ColorChannel( (int)i, j, aColor[iPos] );
                else ColorChannel( (int)i, j, 0, true );
                iPos++;
            }
}

GameState::GameError MainScreen::MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static ViewSettings &cView = config.GetViewSettings();
    static const ControlsSettings &cControls = config.GetControlsSettings();

    switch ( msg )
    {
        case WM_COMMAND:
        {
            int iId = LOWORD( wParam );
            switch ( iId )
            {
                case ID_CHANGESTATE:
                    m_pNextState = reinterpret_cast< GameState * >( lParam );
                    return Success;
                case ID_PLAY_STOP:
                    JumpTo( GetMinTime() );
                    cPlayback.SetStopped( true );
                    return Success;
                case ID_PLAY_SKIPFWD:
                    JumpTo( static_cast< long long >( m_llStartTime + cControls.dFwdBackSecs * 1000000 ) );
                    return Success;
                case ID_PLAY_SKIPBACK:
                    JumpTo( static_cast< long long >( m_llStartTime - cControls.dFwdBackSecs * 1000000 ) );
                    return Success;
                case ID_VIEW_RESETDEVICE:
                    m_pRenderer->ResetDevice();
                    return Success;
                case ID_VIEW_MOVEANDZOOM:
                    if ( cView.GetZoomMove() )
                    {
                        cView.SetOffsetX( cView.GetOffsetX() + m_fTempOffsetX );
                        cView.SetOffsetY( cView.GetOffsetY() + m_fTempOffsetY );
                        cView.SetZoomX( cView.GetZoomX() * m_fTempZoomX );
                    }
                    else
                    {
                        cView.SetZoomMove( true, true );
                        return Success;
                    }
                    [[fallthrough]];
                case ID_VIEW_CANCELMOVEANDZOOM:
                    cView.SetZoomMove( false, true );
                    m_bTrackPos = m_bTrackZoom = false;
                    m_fTempOffsetX = 0.0f;
                    m_fTempOffsetY = 0.0f;
                    m_fTempZoomX = 1.0f;
                    return Success;
                case ID_VIEW_RESETMOVEANDZOOM:
                    cView.SetOffsetX( 0.0f );
                    cView.SetOffsetY( 0.0f );
                    cView.SetZoomX( 1.0f );
                    m_fTempOffsetX = 0.0f;
                    m_fTempOffsetY = 0.0f;
                    m_fTempZoomX = 1.0f;
                    return Success;
            }
            break;
        }
        // Doubled from the main window accelerators: removes the Ctrl requirement
        case WM_KEYDOWN:
        {
            bool bCtrl = GetKeyState( VK_CONTROL ) < 0;
            bool bAlt = GetKeyState( VK_MENU ) < 0;
            bool bShift = GetKeyState( VK_SHIFT ) < 0;

            switch ( wParam )
            {
                case VK_SPACE:
                    cPlayback.TogglePaused( true );
                    return Success;
                case VK_OEM_PERIOD:
                    JumpTo( GetMinTime() );
                    cPlayback.SetStopped( true );
                    return Success;
                case VK_UP:
                    if ( bAlt && !bCtrl )
                        cPlayback.SetVolume( min( cPlayback.GetVolume() + 0.1, 1.0 ), true );
                    else if ( bShift && !bCtrl )
                        cPlayback.SetNSpeed( cPlayback.GetNSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    else if ( !bAlt && !bShift )
                        cPlayback.SetSpeed( cPlayback.GetSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return Success;
                case VK_DOWN:
                    if ( bAlt && !bShift && !bCtrl )
                        cPlayback.SetVolume( max( cPlayback.GetVolume() - 0.1, 0.0 ), true );
                    else if ( bShift && !bAlt && !bCtrl )
                        cPlayback.SetNSpeed( cPlayback.GetNSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    else if ( !bAlt && !bShift )
                        cPlayback.SetSpeed( cPlayback.GetSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return Success;
                case 'R':
                    cPlayback.SetSpeed( 1.0, true );
                    return Success;
                case VK_LEFT:
                    JumpTo( static_cast< long long >( m_llStartTime - cControls.dFwdBackSecs * 1000000 ) );
                    return Success;
                case VK_RIGHT:
                    JumpTo( static_cast< long long >( m_llStartTime + cControls.dFwdBackSecs * 1000000 ) );
                    return Success;
                case 'M':
                    cPlayback.ToggleMute( true );
                    return Success;
                case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': case '0':
                {
                    if ( bAlt ) return Success;
                    int iTrack = 0, iChannel = 0;
                    ChannelSettings *pSettings = GetChannelSettings( wParam == '0' ? 9 : (int)wParam - '1', &iTrack, &iChannel );
                    if ( !pSettings ) return Success;

                    if ( bCtrl ) HideChannel( iTrack, iChannel, !pSettings->bHidden );
                    else MuteChannel( iTrack, iChannel, !pSettings->bMuted );

                    swprintf( m_sBuf, 128, L"%ls Track %d",
                        bCtrl && pSettings->bHidden ? L"Hiding" : bCtrl ? L"Showing" : pSettings->bMuted ? L"Muting" : L"Unmuting",
                        wParam == '0' ? 9 + 1 : (int)wParam - '1' + 1 );
                    m_tpMessage.Reset( m_pRenderer->GetBufferWidth() / 2.0f, m_pRenderer->GetBufferHeight() * ( 1.0f - KBPercent ) / 2.0f - 35.0f / 2.0f, pSettings->iPrimaryRGB, m_sBuf );
                    return Success;
                }
            }
            break;
        }
        case WM_DEVICECHANGE:
            if ( m_pPlayback && m_pPlayback->OutputName() != MidiOutputs::ConfiguredName() )
                m_pPlayback->ReopenOutput();
            break;
        case TBM_SETPOS:
        {
            long long llFirstTime = GetMinTime();
            long long llLastTime = GetMaxTime();
            JumpTo( llFirstTime + ( ( llLastTime - llFirstTime ) * lParam ) / 1000, false );
            break;
        }
        case WM_LBUTTONDOWN:
            if ( m_bZoomMove )
            {
                m_ptLastPos.x = ( SHORT )LOWORD( lParam );
                m_ptLastPos.y = ( SHORT )HIWORD( lParam );
                m_bTrackPos = true;
            }
            return Success;
        case WM_RBUTTONDOWN:
            if ( !m_bZoomMove ) return Success;
            m_ptLastPos.x = ( SHORT )LOWORD( lParam );
            m_ptLastPos.y = ( SHORT )HIWORD( lParam );
            m_ptStartZoom.x = static_cast< int >( ( m_ptLastPos.x - m_fOffsetX - m_fTempOffsetX ) / ( m_fZoomX * m_fTempZoomX ) );
            m_ptStartZoom.y = static_cast< int >( m_ptLastPos.y - m_fOffsetY - m_fTempOffsetY );
            m_bTrackZoom = true;
            return Success;
        case WM_CAPTURECHANGED:
            m_bTrackPos = m_bTrackZoom = false;
            return Success;
        case WM_LBUTTONUP:
            m_bTrackPos = false;
            return Success;
        case WM_RBUTTONUP:
            m_bTrackZoom = false;
            return Success;
        case WM_MOUSEMOVE:
        {
            if ( !m_bTrackPos && !m_bTrackZoom ) return Success;
            short x = LOWORD( lParam );
            short y = HIWORD( lParam );
            short dx = static_cast< short >( x - m_ptLastPos.x );
            short dy = static_cast< short >( y - m_ptLastPos.y );

            if ( m_bTrackPos )
            {
                m_fTempOffsetX += dx;
                m_fTempOffsetY += dy;
            }
            if ( m_bTrackZoom )
            {
                float fOldX = m_fOffsetX + m_fTempOffsetX + m_ptStartZoom.x * m_fZoomX * m_fTempZoomX;
                m_fTempZoomX *= pow( 2.0f, dx / 200.0f );
                float fNewX = m_fOffsetX + m_fTempOffsetX + m_ptStartZoom.x * m_fZoomX * m_fTempZoomX;
                m_fTempOffsetX = m_fTempOffsetX - ( fNewX - fOldX );
            }

            m_ptLastPos.x = x;
            m_ptLastPos.y = y;
            return Success;
        }
    }

    return Success;
}

GameState::GameError MainScreen::Logic()
{
    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static const ViewSettings &cView = config.GetViewSettings();
    static const VisualSettings &cVisual = config.GetVisualSettings();
    static const VideoSettings &cVideo = config.GetVideoSettings();
    const RefMidiInfo &mInfo = m_pSong->info;

    // Detect changes in state
    bool bPaused = cPlayback.GetPaused();
    double dSpeed = cPlayback.GetSpeed();
    double dNSpeed = cPlayback.GetNSpeed();
    // Developer switches (environment): PFAVIZ_MUTE=1 mutes, PFAVIZ_EXIT_AFTER=N closes the window
    // N seconds after the song starts, PFAVIZ_FRAMELOG=FILE appends the frame time parts every half second
    static const bool bEnvMute = getenv( "PFAVIZ_MUTE" ) != nullptr;
    static const double dEnvExitAfter = getenv( "PFAVIZ_EXIT_AFTER" ) ? atof( getenv( "PFAVIZ_EXIT_AFTER" ) ) : 0.0;
    static const char *const sEnvFrameLog = getenv( "PFAVIZ_FRAMELOG" );
    bool bMute = cPlayback.GetMute() || bEnvMute;
    long long llTimeSpan = static_cast< long long >( 3.0 * dNSpeed * 1000000 );
    bool bPausedChanged = ( bPaused != m_bPaused );
    bool bMuteChanged = ( bMute != m_bMute );
    if ( dSpeed != m_dSpeed ) m_Clock.SetSpeed( dSpeed );
    if ( bPaused != m_Clock.Paused() ) m_Clock.SetPaused( bPaused );

    // Set the state
    m_bPaused = bPaused;
    m_dSpeed = dSpeed;
    m_bMute = bMute;
    m_llTimeSpan = llTimeSpan;
    m_dVolume = cPlayback.GetVolume();
    m_bShowKB = cView.GetKeyboard();
    m_bZoomMove = cView.GetZoomMove();
    m_fOffsetX = cView.GetOffsetX();
    m_fOffsetY = cView.GetOffsetY();
    m_fZoomX = cView.GetZoomX();
    if ( !m_bZoomMove ) m_bTrackPos = m_bTrackZoom = false;
    m_uStats = cVisual.uStats & ( m_pPlayback ? VisualSettings::AllStats : ~VisualSettings::SynthStats );
    m_pRenderer->SetLimitFPS( cVideo.bLimitFPS );
    m_Colors.SetBackground( cVisual.iBkgColor );

    double dMaxCorrect = ( mInfo.iMaxVolume > 0 ? 127.0 / mInfo.iMaxVolume : 1.0 );
    double dVolumeCorrect = ( mInfo.llVolumeSum > 0 ? ( m_dVolume * 127.0 * mInfo.llNoteCount ) / mInfo.llVolumeSum : 1.0 );
    m_dVolumeCorrect = min( dVolumeCorrect, dMaxCorrect );
    if ( m_pPlayback )
    {
        static const PerformanceSettings &cPerf = config.GetPerformanceSettings();
        m_pPlayback->SetMute( m_bMute );
        m_pPlayback->SetVolumeCorrect( m_dVolumeCorrect );
        m_pPlayback->SetMetronome( cPlayback.GetMetronome() );
        m_pPlayback->SetSkipping( cPerf.bVelocitySkip, cPerf.iSkipLagMs );
        m_pPlayback->SetSendLimits( cPerf.bCollapseStacks, cPerf.bSendLimit ? cPerf.iMaxSendsPerSec : 0 );
        m_pPlayback->SetMinVelocity( cPerf.bVelocityFloor ? cPerf.iMinVelocity : 0 );
    }
    if ( m_pEngine->LodOn() != config.GetPerformanceSettings().bLodCache ) m_pEngine->SetLod( config.GetPerformanceSettings().bLodCache );

    // Time stuff
    long long llMaxTime = GetMaxTime();
    long long llElapsed = m_Timer.GetMicroSecs();
    m_Timer.Start();

    // Compute FPS every half a second
    m_llFPSTime += llElapsed;
    m_iFPSCount++;
    if ( m_llFPSTime >= 500000 )
    {
        m_dFPS = m_iFPSCount / ( m_llFPSTime / 1000000.0 );
        if ( m_pPlayback )
        {
            m_Stats = m_pPlayback->GetStats();
            m_dSentPerSec = ( m_Stats.llSent - m_llStatSent ) / ( m_llFPSTime / 1000000.0 );
            m_dSkippedPerSec = ( m_Stats.llSkipped - m_llStatSkipped ) / ( m_llFPSTime / 1000000.0 );
            m_dCollapsedPerSec = ( m_Stats.llCollapsed - m_llStatCollapsed ) / ( m_llFPSTime / 1000000.0 );
            m_llStatCollapsed = m_Stats.llCollapsed;
            m_llStatSent = m_Stats.llSent;
            m_llStatSkipped = m_Stats.llSkipped;
        }
        m_llFPSTime = m_iFPSCount = 0;
        for ( int i = 0; i < PartCount; i++ )
        {
            m_adPartAvgShown[i] = m_iPartFrames ? m_adPartSum[i] / m_iPartFrames : 0.0;
            m_adPartMaxShown[i] = m_adPartMax[i];
            m_adPartSum[i] = m_adPartMax[i] = 0.0;
        }
        m_iPartFrames = 0;
        if ( sEnvFrameLog )
            if ( FILE *f = fopen( sEnvFrameLog, "a" ) )
            {
                const FastEngine::FrameStats &fs = m_pEngine->LastStats();
                fprintf( f, "t=%.2f fps=%.1f other %.2f/%.2f notes %.2f/%.2f scene %.2f/%.2f present %.2f/%.2f owners %zu\n", m_llStartTime / 1e6, m_dFPS,
                         m_adPartAvgShown[0], m_adPartMaxShown[0], m_adPartAvgShown[1], m_adPartMaxShown[1], m_adPartAvgShown[2], m_adPartMaxShown[2],
                         m_adPartAvgShown[3], m_adPartMaxShown[3], fs.iOwners );
                fclose( f );
            }
        m_llEnvRunUs += 500000;
        if ( dEnvExitAfter > 0 && m_llEnvRunUs >= (long long)( dEnvExitAfter * 1e6 ) ) PostMessageW( g_hWnd, WM_CLOSE, 0, 0 );
    }

    // If we just paused, kill the music
    if ( ( bPausedChanged || bMuteChanged ) && ( m_bPaused || m_bMute ) && m_pPlayback )
        m_pPlayback->AllNotesOff();

    // Figure out start and end times for display: the clock is sampled once per frame
    long long llOldStartTime = m_llStartTime;
    if ( !m_bPaused && m_llStartTime < llMaxTime )
        m_llStartTime = m_Clock.Now();
    m_iStartTick = m_pMeter->GetCurrentTick( m_llStartTime );
    long long llEndTime = m_llStartTime + m_llTimeSpan;

    RenderGlobals();

    // Hidden channels changed: rebuild the draw view in the background
    if ( m_bHiddenChanged )
    {
        m_bHiddenChanged = false;
        std::vector< uint8_t > vHidden( m_vTrackSettings.size() * 16 );
        for ( size_t t = 0; t < m_vTrackSettings.size(); t++ )
            for ( int c = 0; c < 16; c++ ) vHidden[t * 16 + c] = m_vTrackSettings[t].aChannels[c].bHidden;
        m_pEngine->SetHidden( vHidden );
    }

    // Advance the end position, then the start position (the music plays on the playback threads)
    m_pEngine->AdvanceEnd( llEndTime );
    if ( !m_bPaused )
        m_pEngine->Process( m_llStartTime );

    if ( m_tpMessage.IsAlive() ) m_tpMessage.Logic( llElapsed );

    AdvanceIterators( m_llStartTime, false );

    // Update the position slider
    long long llFirstTime = GetMinTime();
    long long llLastTime = GetMaxTime();
    long long llOldPos = ( ( llOldStartTime - llFirstTime ) * 1000 ) / ( llLastTime - llFirstTime );
    long long llNewPos = ( ( m_llStartTime - llFirstTime ) * 1000 ) / ( llLastTime - llFirstTime );
    if ( llOldPos != llNewPos ) cPlayback.SetPosition( static_cast< int >( llNewPos ) );

    // Song's over
    if ( !m_bPaused && m_llStartTime >= llMaxTime )
    {
        m_Clock.SetPaused( true );
        cPlayback.SetPaused( true, true );
    }
    return Success;
}

void MainScreen::JumpTo( long long llStartTime, bool bUpdateGUI )
{
    long long llFirstTime = GetMinTime();
    long long llLastTime = GetMaxTime();
    long long llPrevTime = m_llStartTime;
    m_llStartTime = min( max( llStartTime, llFirstTime ), llLastTime );
    m_pEngine->Jump( llStartTime, m_llStartTime, m_llStartTime + m_llTimeSpan );

    // AllNotesOff, PlaySkippedEvents and the clock move happen in the engine
    if ( m_pPlayback ) m_pPlayback->Seek( llStartTime, m_llStartTime, llPrevTime );
    else m_Clock.Set( m_llStartTime );
    AdvanceIterators( llStartTime, true );
    m_iStartTick = m_pMeter->GetCurrentTick( m_llStartTime );

    if ( bUpdateGUI )
    {
        static PlaybackSettings &cPlayback = Config::GetConfig().GetPlaybackSettings();
        long long llNewPos = ( ( m_llStartTime - llFirstTime ) * 1000 ) / ( llLastTime - llFirstTime );
        cPlayback.SetPosition( static_cast< int >( llNewPos ) );
    }
}

// Advance tempo and signature (for the measure lines; the playback threads track their own)
void MainScreen::AdvanceIterators( long long llTime, bool bIsJump )
{
    if ( bIsJump ) m_pMeter->Jump( llTime );
    else m_pMeter->Advance( llTime );
}

GameState::GameError MainScreen::Render()
{
    if ( FAILED( m_pRenderer->ResetDeviceIfNeeded() ) ) return DirectXError;

    long long tA = Timer::NowMicroSecs();
    m_pRenderer->Clear( 0x00000000 );
    m_pRenderer->BeginScene();
    Scene::DrawBackground( m_pRenderer, m_G, m_Colors );
    m_pMeter->DrawMeasureLines( m_pRenderer, m_G, m_Colors, m_iStartTick );
    long long tB = Timer::NowMicroSecs();
    m_pEngine->DrawNotes( m_pRenderer, m_G, m_vTrackSettings );
    long long tC = Timer::NowMicroSecs();
    if ( m_bShowKB )
    {
        int aSlot[128];
        m_pEngine->GetKeySlots( aSlot );
        const ChannelSettings *pKeyColor[128];
        for ( int i = 0; i < 128; i++ )
            pKeyColor[i] = aSlot[i] < 0 ? nullptr : &m_vTrackSettings[aSlot[i] >> 4].aChannels[aSlot[i] & 15];
        Scene::DrawKeys( m_pRenderer, m_G, m_Colors, pKeyColor );
    }
    Scene::DrawBorder( m_pRenderer, m_G, m_Colors );
    RenderText();
    m_pRenderer->EndScene();
    long long tD = Timer::NowMicroSecs();
    m_pRenderer->Present();
    long long tE = Timer::NowMicroSecs();
    double adPart[PartCount] = { m_llRenderEnd ? ( tA - m_llRenderEnd ) / 1000.0 : 0.0, ( tC - tB ) / 1000.0,
                                 ( ( tB - tA ) + ( tD - tC ) ) / 1000.0, ( tE - tD ) / 1000.0 };
    for ( int i = 0; i < PartCount; i++ ) { m_adPartSum[i] += adPart[i]; m_adPartMax[i] = std::max( m_adPartMax[i], adPart[i] ); }
    m_iPartFrames++;
    m_llRenderEnd = tE;
    return Success;
}

void MainScreen::RenderGlobals()
{
    m_G = Scene::ComputeGlobals( GetViewInputs( m_pRenderer->GetBufferWidth(), m_pRenderer->GetBufferHeight(), m_llStartTime ) );
}

ViewInputs MainScreen::GetViewInputs( int iWidth, int iHeight, long long llStartTime ) const
{
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    ViewInputs in;
    in.iBufferWidth = iWidth;
    in.iBufferHeight = iHeight;
    in.eKeysShown = cVisual.eKeysShown;
    in.iFirstKey = cVisual.iFirstKey;
    in.iLastKey = cVisual.iLastKey;
    in.iSongMinNote = m_pSong->info.iMinNote;
    in.iSongMaxNote = m_pSong->info.iMaxNote;
    in.fOffsetX = m_fOffsetX + m_fTempOffsetX;
    in.fOffsetY = m_fOffsetY + m_fTempOffsetY;
    in.fZoomX = m_fZoomX * m_fTempZoomX;
    in.bShowKB = m_bShowKB;
    in.llStartTime = llStartTime;
    in.llTimeSpan = m_llTimeSpan;
    return in;
}

void MainScreen::RenderText()
{
    int iLines = StatusLines( m_uStats );

    RECT rcStatus = { m_pRenderer->GetBufferWidth() - 156, 0, m_pRenderer->GetBufferWidth(), 6 + 16 * iLines };

    int iMsgCY = 200;
    RECT rcMsg = { 0, static_cast< int >( m_pRenderer->GetBufferHeight() * ( 1.0f - KBPercent ) - iMsgCY ) / 2, 0, 0 };
    rcMsg.right = m_pRenderer->GetBufferWidth();
    rcMsg.bottom = rcMsg.top + iMsgCY;

    unsigned iBkgColor = 0x40000000;
    if ( iLines )
        m_pRenderer->DrawRect( static_cast< float >( rcStatus.left ), static_cast< float >( rcStatus.top ),
            static_cast< float >( rcStatus.right - rcStatus.left ), static_cast< float >( rcStatus.bottom - rcStatus.top ), 0x80000000 );
    if ( m_bZoomMove )
        m_pRenderer->DrawRect( static_cast< float >( rcMsg.left ), static_cast< float >( rcMsg.top ),
            static_cast< float >( rcMsg.right - rcMsg.left ), static_cast< float >( rcMsg.bottom - rcMsg.top ), iBkgColor );

    m_pRenderer->BeginText();
    RenderStatus( m_pRenderer, &rcStatus, m_llStartTime, m_dFPS, m_uStats );
    if ( m_pEngine->DrawsPreviousMask() )
    {
        // Hidden channels change once their draw view is built (FastEngine::SetHidden)
        RECT rcApply = { 9, 5, 400, 25 };
        m_pRenderer->DrawText( L"Applying hidden channels...", Renderer::Small, &rcApply, 0, 0xFF404040 );
        OffsetRect( &rcApply, -1, -1 );
        m_pRenderer->DrawText( L"Applying hidden channels...", Renderer::Small, &rcApply, 0, 0xFFFFFFFF );
    }
    if ( m_bZoomMove )
        RenderMessage( &rcMsg, L"- Left-click and drag to move the screen\n- Right-click and drag to zoom horizontally\n- Press Escape to abort changes\n- Press Ctrl+V to save changes" );
    else if ( m_tpMessage.IsAlive() )
        m_tpMessage.Render( m_pRenderer, 0.0f, 0.0f );
    m_pRenderer->EndText();
}

int MainScreen::StatusLines( unsigned uStats )
{
    return __builtin_popcount( uStats & VisualSettings::AllStats );
}

// The top-right stats area's text (the video renderer passes its own renderer, time and frame rate)
void MainScreen::RenderStatus( Renderer *pRenderer, LPRECT prcStatus, long long llTime, double dFPS, unsigned uStats )
{
    wchar_t sTime[128];
    const RefMidiInfo &mInfo = m_pSong->info;
    if ( llTime >= 0 )
        swprintf( sTime, 128, L"%lld:%04.1lf / %lld:%04.1lf",
            llTime / 60000000, ( llTime % 60000000 ) / 1000000.0,
            mInfo.llTotalMicroSecs / 60000000, ( mInfo.llTotalMicroSecs % 60000000 ) / 1000000.0 );
    else
        swprintf( sTime, 128, L"\t-%lld:%04.1lf / %lld:%04.1lf",
            -llTime / 60000000, ( -llTime % 60000000 ) / 1000000.0,
            mInfo.llTotalMicroSecs / 60000000, ( mInfo.llTotalMicroSecs % 60000000 ) / 1000000.0 );

    wchar_t sFPS[128];
    swprintf( sFPS, 128, L"%.1lf", dFPS );

    // Scoring was removed; the line is kept as the original shows it with no input device
    const wchar_t *sScore = L"N/A";

    InflateRect( prcStatus, -6, -3 );

    // A line: shadow, then text (the original's offsets: the first line at +1, each next 16 lower)
    bool bFirst = true;
    auto Line = [&]( const wchar_t *sLabel, const wchar_t *sValue ) {
        OffsetRect( prcStatus, 2, bFirst ? 1 : 16 + 1 );
        bFirst = false;
        pRenderer->DrawText( sLabel, Renderer::Small, prcStatus, 0, 0xFF404040 );
        pRenderer->DrawText( sValue, Renderer::Small, prcStatus, DT_RIGHT, 0xFF404040 );
        OffsetRect( prcStatus, -2, -1 );
        pRenderer->DrawText( sLabel, Renderer::Small, prcStatus, 0, 0xFFFFFFFF );
        pRenderer->DrawText( sValue, Renderer::Small, prcStatus, DT_RIGHT, 0xFFFFFFFF );
    };
    auto Shown = [&]( int iStat ) { return ( uStats >> iStat ) & 1; };
    if ( Shown( VisualSettings::StatTime ) ) Line( L"Time:", sTime );
    if ( Shown( VisualSettings::StatFPS ) ) Line( L"FPS:", sFPS );
    if ( Shown( VisualSettings::StatScore ) ) Line( L"Score:", sScore );

    if ( uStats & VisualSettings::NoteStats )
    {
        // From the tables counted while loading (n/a: the song was loaded with them off)
        const NoteStats &ns = m_pSong->noteStats;
        auto Count = []( wchar_t *s, uint64_t n ) {
            wchar_t buf[32];
            int iDigits = swprintf( buf, 32, L"%llu", (unsigned long long)n ), j = 0;
            for ( int i = 0; i < iDigits; i++ )
            {
                if ( i && ( iDigits - i ) % 3 == 0 ) s[j++] = L',';
                s[j++] = buf[i];
            }
            s[j] = 0;
        };
        wchar_t aValues[3][40];
        if ( ns.Empty() ) for ( auto &v : aValues ) wcscpy( v, L"n/a" );
        else
        {
            Count( aValues[0], ns.Passed( llTime ) );
            Count( aValues[1], ns.NPS( llTime ) );
            Count( aValues[2], ns.Polyphony( llTime ) );
        }
        const wchar_t *aLabels[3] = { L"Notes:", L"NPS:", L"Polyphony:" };
        for ( int i = 0; i < 3; i++ ) if ( Shown( VisualSettings::StatNotes + i ) ) Line( aLabels[i], aValues[i] );
    }

    if ( uStats & ( VisualSettings::SynthStats | VisualSettings::FrameStats ) )
    {
        auto Rate = []( wchar_t *s, double d ) {
            if ( d >= 1e6 ) swprintf( s, 32, L"%.2lfM", d / 1e6 );
            else if ( d >= 1e3 ) swprintf( s, 32, L"%.1lfK", d / 1e3 );
            else swprintf( s, 32, L"%.0lf", d );
        };
        wchar_t aValues[5 + PartCount][32];
        Rate( aValues[0], m_dSentPerSec );
        Rate( aValues[1], m_dSkippedPerSec );
        Rate( aValues[2], m_dCollapsedPerSec );
        swprintf( aValues[3], 32, L"%.1lf ms", std::max< long long >( 0, m_Stats.llLagUs ) / 1000.0 );
        // The lag's threshold, or the send budget's cut when that is higher ("v at k%": part of velocity v kept)
        const PlaybackDecoder::BudgetCut &cut = m_Stats.budget;
        if ( m_Stats.iSkipVelocity > 127 ) swprintf( aValues[4], 32, L"all" );
        else if ( cut.iVelocity > 0 && cut.iVelocity >= m_Stats.iSkipVelocity )
            swprintf( aValues[4], 32, L"< %d, %d at %.0f%%", cut.iVelocity, cut.iVelocity, cut.dKeep * 100.0 );
        else if ( m_Stats.iSkipVelocity > 0 ) swprintf( aValues[4], 32, L"vel < %d", m_Stats.iSkipVelocity );
        else swprintf( aValues[4], 32, L"none" );
        // Frame time parts, average / maximum over the last half second, in ms
        for ( int i = 0; i < PartCount; i++ ) swprintf( aValues[5 + i], 32, L"%.2f / %.2f", m_adPartAvgShown[i], m_adPartMaxShown[i] );
        const wchar_t *aLabels[5 + PartCount] = { L"Sent/s:", L"Skipped/s:", L"Collapsed/s:", L"Lag:", L"Skipping:",
                                                  L"Other ms:", L"Notes ms:", L"Scene ms:", L"Present ms:" };
        for ( int i = 0; i < 5 + PartCount; i++ ) if ( Shown( VisualSettings::StatSent + i ) ) Line( aLabels[i], aValues[i] );
    }
}

void MainScreen::RenderMessage( LPRECT prcMsg, const wchar_t *sMsg )
{
    RECT rcMsg = { 0 };
    Renderer::FontSize eFontSize = Renderer::Medium;
    m_pRenderer->DrawText( sMsg, eFontSize, &rcMsg, DT_CALCRECT, 0xFF000000 );
    if ( rcMsg.right > m_pRenderer->GetBufferWidth() )
    {
        eFontSize = Renderer::Small;
        m_pRenderer->DrawText( sMsg, eFontSize, &rcMsg, DT_CALCRECT, 0xFF000000 );
    }

    OffsetRect( &rcMsg, 2 + prcMsg->left + ( prcMsg->right - prcMsg->left - rcMsg.right ) / 2,
                2 + prcMsg->top + ( prcMsg->bottom - prcMsg->top - rcMsg.bottom ) / 2 );
    m_pRenderer->DrawText( sMsg, eFontSize, &rcMsg, 0, 0xFF404040 );
    OffsetRect( &rcMsg, -2, -2 );
    m_pRenderer->DrawText( sMsg, eFontSize, &rcMsg, 0, 0xFFFFFFFF );
}
