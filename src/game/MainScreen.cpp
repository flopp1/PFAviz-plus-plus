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
#include "song/PfaLag.h"

using std::max;
using std::min;
using std::pow;

const float MainScreen::KBPercent = 0.25f;

MainScreen::MainScreen( std::shared_ptr< const Song > pSong, HWND hWnd, Renderer *pRenderer, bool bPlayback )
    : GameState( hWnd, pRenderer ), m_pSong( std::move( pSong ) )
{
    m_bWithPlayback = bPlayback;
    InitSong();
}

void MainScreen::InitSong()
{
    if ( !m_pSong ) return;

    // Developer switches: PFAVIZ_DRAWTHREADS=n draws with n threads, PFAVIZ_NOMERGE=1 turns off merging rects
    int iDrawThreads = getenv( "PFAVIZ_DRAWTHREADS" ) ? atoi( getenv( "PFAVIZ_DRAWTHREADS" ) ) : Config::GetConfig().GetPerformanceSettings().iWorkerThreads;
    m_pEngine = new FastEngine( *m_pSong, iDrawThreads );
    m_pEngine->SetMergeRows( !getenv( "PFAVIZ_NOMERGE" ) );
    m_pEngine->SetLod( Config::GetConfig().GetPerformanceSettings().bLodCache );
    m_pMeter = new MeterTracker( m_pSong->vMeta, m_pSong->info );
    m_vTrackSettings.resize( m_pSong->info.iNumTracks );
    if ( m_pSong && m_bWithPlayback ) m_pPlayback = std::make_unique< PlaybackEngine >( m_pSong, m_Clock, Config::GetConfig().GetPerformanceSettings().iPlaybackThreads );

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
    PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    cPlayback.SetPaused( m_bStartPaused, true );
    m_bPaused = m_bStartPaused;
    if ( m_pPlayback )
    {
        const PerformanceSettings &cPerf = config.GetPerformanceSettings();
        m_pPlayback->SetSkipping( cPerf.bVelocitySkip, cPerf.iSkipLagMs );
        m_pPlayback->SetSendLimits( cPerf.bCollapseStacks, cPerf.bSendLimit ? cPerf.iMaxSendsPerSec : 0 );
        m_pPlayback->SetMinVelocity( cPerf.bVelocityFloor ? cPerf.iMinVelocity : 0 );
        m_pPlayback->SetTuning( cPerf.bTuning31, cPerf.bTuning31PitchBend );
        MidiOutputs::SetKdmapiLongSends( cPerf.bKdmapiLongSends );
        m_pPlayback->SetMute( m_bMute );
        m_pPlayback->SetMetronome( cPlayback.GetMetronome() );
        m_Clock.SetSpeed( cPlayback.GetSpeed() );
        m_Clock.SetPaused( cPlayback.GetPaused() );
        m_pPlayback->Start( m_llStartTime );
    }
    else m_Clock.Set( m_llStartTime );
    // Developer switch PFAVIZ_START=S: start S seconds into the song
    if ( const char *sStart = getenv( "PFAVIZ_START" ) ) JumpTo( (long long)( atof( sStart ) * 1e6 ) );
    m_Timer.Start();
    return Success;
}

void MainScreen::HideChannel( int iTrack, int iChannel, bool bHidden )
{
    bool &b = m_vTrackSettings[iTrack].aChannels[iChannel].bHidden;
    if ( b != bHidden ) m_bHiddenChanged = true;
    m_uTracksGen++;
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
    m_uTracksGen++;
}

ChannelSettings *MainScreen::GetChannelSettings( int iTrack, int *piTrack, int *piChannel )
{
    m_uTracksGen++;     // (the caller may change it)
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

void MainScreen::SetChannelColors( const unsigned *aColor, int iColors )
{
    const std::vector< RefTrackInfo > &vTracks = m_pSong->vTracks;
    bool bLoop = Config::GetConfig().GetVisualSettings().bLoopColors;
    m_uTracksGen++;
    size_t iPos = 0, n = (size_t)std::max( 1, iColors );
    // Deduplicated (Song::vLogicalTracks): every track of the file counts, and a track takes the colours
    // of the place of its last copy (the one drawn), as it would without deduplication
    const std::vector< uint32_t > &vLogical = m_pSong->vLogicalTracks;
    std::vector< size_t > vLast;
    if ( !vLogical.empty() )
    {
        vLast.assign( vTracks.size(), SIZE_MAX );
        for ( size_t p = 0; p < vLogical.size(); p++ ) vLast[vLogical[p]] = p;
    }
    for ( size_t p = 0, iEnd = vLogical.empty() ? vTracks.size() : vLogical.size(); p < iEnd; p++ )
    {
        size_t i = vLogical.empty() ? p : vLogical[p];
        for ( int j = 0; j < 16; j++ )
            if ( vTracks[i].aNoteCount[j] > 0 )
            {
                if ( vLogical.empty() || vLast[i] == p )
                {
                    if ( iPos < n || bLoop ) ColorChannel( (int)i, j, aColor[iPos % n] );
                    else ColorChannel( (int)i, j, 0, true );
                }
                else if ( iPos >= n && !bLoop )
                {
                    ChannelSettings cs;     // (a copy not drawn still draws its random colour, so the ones after are as without deduplication)
                    cs.SetColor();
                }
                iPos++;
            }
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
                        cPlayback.SetNSpeed( cPlayback.GetNSpeed() * ( 1.0 + cControls.dNSpeedUpPct / 100.0 ), true );
                    else if ( !bAlt && !bShift )
                        cPlayback.SetSpeed( cPlayback.GetSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return Success;
                case VK_DOWN:
                    if ( bAlt && !bShift && !bCtrl )
                        cPlayback.SetVolume( max( cPlayback.GetVolume() - 0.1, 0.0 ), true );
                    else if ( bShift && !bAlt && !bCtrl )
                        cPlayback.SetNSpeed( cPlayback.GetNSpeed() / ( 1.0 + cControls.dNSpeedUpPct / 100.0 ), true );
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
    // The track colours changed (Preferences > Visual): the song coloured again from them (as it was when
    // loaded; channels coloured by hand too)
    std::vector< unsigned > vPalette( cVisual.colors, cVisual.colors + cVisual.iColorCount );
    vPalette.push_back( cVisual.bLoopColors );
    if ( !m_vPalette.empty() && vPalette != m_vPalette ) SetChannelColors( cVisual.colors, cVisual.iColorCount );
    m_vPalette.swap( vPalette );
    m_Colors.SetBar( cVisual.iBarColor );
    // The background image: read here when its file changes (a failure is said once, as a message)
    bool bBkgChanged = !m_bBkgApplied;
    if ( !m_bBkgApplied || config.VisualGen() != m_uVisualGen )
    {
        m_uVisualGen = config.VisualGen();
        std::wstring sBkgImage = config.GetBkgImage();
        bBkgChanged |= sBkgImage != m_sBkgImage;
        m_sBkgImage.swap( sBkgImage );
    }
    if ( bBkgChanged )
    {
        m_bBkgApplied = true;
        m_pBkgImage.reset();
        if ( !m_sBkgImage.empty() )
        {
            auto pImage = std::make_shared< Image >();
            std::wstring sError;
            if ( LoadImageFile( m_sBkgImage, std::min( 8192, m_pRenderer->MaxTextureSide() ), *pImage, sError ) ) m_pBkgImage = pImage;
            else
            {
                swprintf( m_sBuf, 128, L"Background image: %ls", sError.c_str() );
                m_tpMessage.Reset( m_pRenderer->GetBufferWidth() / 2.0f, m_pRenderer->GetBufferHeight() * ( 1.0f - KBPercent ) / 2.0f - 35.0f / 2.0f, 0xFFFFFFFF, m_sBuf );
            }
        }
        m_pRenderer->SetBackgroundImage( m_pBkgImage );
    }
    m_pRenderer->SetBackgroundStyle( cVisual.iBkgFit, cVisual.iBkgOpacity / 100.0f );

    // The velocities are evened out (the average towards 127, no note above it), then the volume slider
    // scales that. (The slider used to go into the average's target before the cap, so above some
    // point, often half way, it changed nothing.)
    double dMaxCorrect = ( mInfo.iMaxVolume > 0 ? 127.0 / mInfo.iMaxVolume : 1.0 );
    double dVolumeCorrect = ( mInfo.llVolumeSum > 0 ? ( 127.0 * mInfo.llNoteCount ) / mInfo.llVolumeSum : 1.0 );
    m_dVolumeCorrect = m_dVolume * min( dVolumeCorrect, dMaxCorrect );
    if ( m_pPlayback )
    {
        static const PerformanceSettings &cPerf = config.GetPerformanceSettings();
        m_pPlayback->SetMute( m_bMute );
        m_pPlayback->SetVolumeCorrect( m_dVolumeCorrect );
        m_pPlayback->SetMetronome( cPlayback.GetMetronome() );
        m_pPlayback->SetSkipping( cPerf.bVelocitySkip, cPerf.iSkipLagMs );
        m_pPlayback->SetSendLimits( cPerf.bCollapseStacks, cPerf.bSendLimit ? cPerf.iMaxSendsPerSec : 0 );
        m_pPlayback->SetMinVelocity( cPerf.bVelocityFloor ? cPerf.iMinVelocity : 0 );
        m_pPlayback->SetMaxThreads( cPerf.iPlaybackThreads );
        // 31-EDO changed: the output reopens in the new mode, and playback seeks to where it is so the
        // new output gets the controllers there
        MidiOutputs::SetKdmapiLongSends( cPerf.bKdmapiLongSends );
        if ( m_pPlayback->SetTuning( cPerf.bTuning31, cPerf.bTuning31PitchBend ) )
            m_pPlayback->Seek( m_llStartTime, m_llStartTime, m_llStartTime );
    }
    static const bool bEnvDrawThreads = getenv( "PFAVIZ_DRAWTHREADS" ) != nullptr;
    if ( !bEnvDrawThreads ) m_pEngine->SetThreads( config.GetPerformanceSettings().iWorkerThreads );
    if ( m_pEngine->LodOn() != config.GetPerformanceSettings().bLodCache ) m_pEngine->SetLod( config.GetPerformanceSettings().bLodCache );
    // The tick-based view changed: the engine starts again where the song is, in the new placing
    if ( m_pEngine->TickBased() != cVisual.bTickBased )
    {
        m_pEngine->SetTickBased( cVisual.bTickBased );
        long long llView = m_pEngine->ToView( m_llStartTime );
        m_pEngine->Jump( llView, llView, ViewEnd( llView ) );
    }
    m_pRenderer->SetInstancing( config.GetPerformanceSettings().bGpuInstancing );

    // Time stuff
    long long llMaxTime = GetMaxTime();
    long long llElapsed = m_Timer.GetMicroSecs();
    m_Timer.Start();

    // The PFA lag simulation, on or off
    {
        const bool bWant = cVisual.bPfaLag;
        const bool bReady = bWant && PfaLag::Ready( *m_pSong );
        if ( bWant && !bReady && !m_bSimWarned )
        {
            m_bSimWarned = true;
            wcscpy_s( m_sBuf, L"PFA lag simulation: open the song again (it needs the note statistics)" );
            m_tpMessage.Reset( m_pRenderer->GetBufferWidth() / 2.0f, m_pRenderer->GetBufferHeight() * ( 1.0f - KBPercent ) / 2.0f - 35.0f / 2.0f, 0xFFFFFFFF, m_sBuf );
        }
        if ( !bWant ) m_bSimWarned = false;
        if ( bReady != m_bSimOn )
        {
            m_bSimOn = bReady;
            if ( bReady )
            {
                m_SimCpu = SimCpu( cVisual );
                DEVMODEW dm = { };
                dm.dmSize = sizeof( dm );
                m_dRefreshUs = EnumDisplaySettingsW( nullptr, ENUM_CURRENT_SETTINGS, &dm ) && dm.dmDisplayFrequency > 1 ? 1e6 / dm.dmDisplayFrequency : 1e6 / 60.0;
                long long llWall = Timer::NowMicroSecs(), t = m_Clock.FreeNow();
                m_Clock.SetRamp( llWall, llWall, t, t );
                LARGE_INTEGER qpc;
                QueryPerformanceCounter( &qpc );
                m_SimNoise = PfaLag::Noise( (uint64_t)qpc.QuadPart ^ 0x5DEECE66Dull );
                ResetSim( t );
                m_llSimNoiseWall = 0;
                m_llSimKey = 0;
                m_dSimFrameUs = 0.0;
                m_iSimFrames = 0;
            }
            else
            {
                // Played on from what the simulation last drew
                m_Clock.EndRamp();
                JumpTo( m_llSimShown );
            }
        }
        else if ( m_bSimOn && config.VisualGen() != m_uSimVisualGen )
        {
            // (the cache sizes or the speed may have changed: the frames ahead are worked out again)
            m_SimCpu = SimCpu( cVisual );
            m_llSimKey = 0;
        }
        m_uSimVisualGen = config.VisualGen();
        if ( m_bSimOn ) SimulateFrame( llMaxTime );
    }

    // The synth's stats every SynthStepUs: the latest values, and rates between the newest sample and
    // the oldest of the ring (half a second back once it is full)
    m_llSynthUs += llElapsed;
    m_llSynthStepUs += llElapsed;
    if ( m_pPlayback && m_llSynthStepUs >= SynthStepUs )
    {
        m_llSynthStepUs = 0;
        m_iSynth = ( m_iSynth + 1 ) % SynthSamples;
        m_iSynthCount = std::min( m_iSynthCount + 1, SynthSamples );
        m_aSynth[m_iSynth] = { m_llSynthUs, m_pPlayback->GetStats() };
        m_Stats = m_aSynth[m_iSynth].s;
        if ( m_iSynthCount > 1 )
        {
            const SynthSample &oldest = m_aSynth[( m_iSynth + SynthSamples - m_iSynthCount + 1 ) % SynthSamples];
            const PlaybackEngine::Stats &a = oldest.s, &b = m_Stats;
            double dUs = (double)( m_llSynthUs - oldest.llUs );
            double dSecs = dUs / 1000000.0;
            m_dDecodedPerSec = ( b.llDecoded - a.llDecoded ) / dSecs;
            m_dDecoderBusy = std::min( 1.0, ( b.llDecodeBusyUs - a.llDecodeBusyUs ) / dUs );
            m_dSenderBusy = std::min( 1.0, ( b.llSendBusyUs - a.llSendBusyUs ) / dUs );
            uint64_t llSamples = b.llRingSamples - a.llRingSamples;
            m_dRingAvg = llSamples ? ( b.llRingSum - a.llRingSum ) / (double)llSamples / PlaybackEngine::RingSlices : 0.0;
            m_dSentPerSec = ( b.llSent - a.llSent ) / dSecs;
            m_dSkippedPerSec = ( b.llSkipped - a.llSkipped ) / dSecs;
            m_dCollapsedPerSec = ( b.llCollapsed - a.llCollapsed ) / dSecs;
            // Peaks: each sample holds its own step's (GetStats resets them); the window's is the highest
            int iRingMax = 0;
            m_iLanes = 1;
            for ( int i = 0; i < m_iSynthCount - 1; i++ )
            {
                const PlaybackEngine::Stats &s = m_aSynth[( m_iSynth + SynthSamples - i ) % SynthSamples].s;
                iRingMax = std::max( iRingMax, s.iRingMax );
                m_iLanes = std::max( m_iLanes, s.iLanes );
            }
            m_dRingMax = iRingMax / (double)PlaybackEngine::RingSlices;
        }
        m_iSynthVoices = -1;
        m_dSynthCpu = -1.0;
        if ( ( m_uStats & VisualSettings::VoiceStats ) && m_pPlayback->OutputName() == MidiOutputs::KdmapiName() )
        {
            unsigned uVoices = 0;
            float fCpu = 0.0f;
            if ( MidiOutputs::KdmapiActiveVoices( uVoices ) ) m_iSynthVoices = (int)uVoices;
            if ( MidiOutputs::KdmapiRenderingTime( fCpu ) ) m_dSynthCpu = fCpu;
        }
    }

    // Compute FPS every half a second
    m_llFPSTime += llElapsed;
    m_iFPSCount++;
    if ( m_llFPSTime >= 500000 )
    {
        m_dFPS = m_iFPSCount / ( m_llFPSTime / 1000000.0 );
        // Simulating PFA: its frame rate (a frame longer than the interval: from how long it has run)
        if ( m_bSimOn )
        {
            double dLong = std::max( m_dSimFrameUs, (double)( Timer::NowMicroSecs() - m_llSimWallStart ) );
            m_dFPS = m_iSimFrames > 0 ? m_iSimFrames / ( m_llFPSTime / 1000000.0 ) : dLong > 0.0 ? 1e6 / dLong : 0.0;
            m_iSimFrames = 0;
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
                fprintf( f, "t=%.2f fps=%.1f other %.2f/%.2f notes %.2f/%.2f scene %.2f/%.2f present %.2f/%.2f owners %zu", m_llStartTime / 1e6, m_dFPS,
                         m_adPartAvgShown[0], m_adPartMaxShown[0], m_adPartAvgShown[1], m_adPartMaxShown[1], m_adPartAvgShown[2], m_adPartMaxShown[2],
                         m_adPartAvgShown[3], m_adPartMaxShown[3], fs.iOwners );
                if ( m_bSimOn ) fprintf( f, " pfasim frame %.1f ms, at %.2f s", m_dSimFrameUs / 1000.0, m_llSimCur / 1e6 );
                // The synth sender (and long sends: messages and their average and largest size over the half second)
                if ( m_pPlayback )
                {
                    static uint64_t llLastMsgs = 0, llLastBytes = 0;
                    uint64_t llMsgs, llBytes;
                    size_t iLargest;
                    MidiOutputs::KdmapiLongCounts( llMsgs, llBytes, iLargest );
                    uint64_t dM = llMsgs - llLastMsgs, dB = llBytes - llLastBytes;
                    llLastMsgs = llMsgs;
                    llLastBytes = llBytes;
                    fprintf( f, " sent/s %.0f sender %.0f%% lag %.1f ns/msg %.1f skipped/s %.0f long %llu avg %.0f max %zu",
                             m_dSentPerSec, m_dSenderBusy * 100.0, m_Stats.llLagUs / 1000.0, m_Stats.dNsPerMsg, m_dSkippedPerSec,
                             (unsigned long long)dM, dM ? (double)dB / dM : 0.0, iLargest );
                }
                fputc( '\n', f );
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
        m_llStartTime = m_bSimOn ? m_llSimShown : m_Clock.Now();
    m_iStartTick = m_pMeter->GetCurrentTick( m_llStartTime );
    const long long llView = m_pEngine->ToView( m_llStartTime );
    long long llEndTime = ViewEnd( llView );

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
        m_pEngine->Process( llView );

    if ( m_tpMessage.IsAlive() ) m_tpMessage.Logic( llElapsed );

    AdvanceIterators( m_llStartTime, false );

    // Update the position slider
    long long llFirstTime = GetMinTime();
    long long llLastTime = GetMaxTime();
    long long llOldPos = ( ( llOldStartTime - llFirstTime ) * 1000 ) / ( llLastTime - llFirstTime );
    long long llNewPos = ( ( m_llStartTime - llFirstTime ) * 1000 ) / ( llLastTime - llFirstTime );
    if ( llOldPos != llNewPos ) cPlayback.SetPosition( static_cast< int >( llNewPos ) );

    // At the end the clock stops, but the NPS window keeps sliding at the playback speed, as song time
    // would, so it falls to 0 as the window does
    if ( m_llStartTime >= llMaxTime )
        m_llPastEndUs = std::min< long long >( m_llPastEndUs + (long long)( llElapsed * m_Clock.Speed() ), 2000000 );
    else m_llPastEndUs = 0;

    // Song's over
    if ( !m_bPaused && m_llStartTime >= llMaxTime )
    {
        m_Clock.SetPaused( true );
        cPlayback.SetPaused( true, true );
    }
    return Success;
}

// The PFA lag simulation's frames: each of the original's frames starts as the last ends (it runs
// flat out), plays the song time that passed meanwhile while its Logic runs (the clock's ramp, slow
// where the work is) and then draws it. The last one done is what is shown (m_llSimShown). Frames are
// worked out a quarter second of real time ahead and the clock gets the ramps of all of them, so its
// pace goes on from one frame to the next by itself however seldom this runs; those ahead are worked
// out again when a setting that shapes them changes
void MainScreen::SimulateFrame( long long llMaxTime )
{
    const long long llWall = Timer::NowMicroSecs();
    static const VideoSettings &cVideo = Config::GetConfig().GetVideoSettings();
    PfaLag::Params p;
    p.llSpanUs = m_llTimeSpan;
    p.bMuted = m_bMute;
    p.bPaused = m_bPaused;
    p.dSendUs = Config::GetConfig().GetVisualSettings().iPfaSendNs / 1000.0;
    const bool bLimit = cVideo.bLimitFPS && m_dRefreshUs > 0.0;
    // A frame runs ten minutes at most (the original's worst measured was 200 s; turning the
    // simulation off ends one at once)
    const double dCap = 600e6;
    // (at most this many worked out per call: after a long stall here, the next starts now)
    const int MaxFrames = 20000;
    const long long AheadUs = 250000;
    const size_t MaxKnots = PlaybackClock::MaxRampKnots - 8;

    // The frames that ended
    auto Ended = [&]( const SimFrame &f ) {
        m_llSimShown = f.llTo;
        m_dSimFrameUs = (double)( f.llWallEnd - f.llWallStart );
        m_iSimFrames++;
    };
    size_t iDone = 0;
    while ( iDone < m_vSim.size() && m_vSim[iDone].llWallEnd <= llWall ) Ended( m_vSim[iDone++] );
    m_vSim.erase( m_vSim.begin(), m_vSim.begin() + iDone );

    // What shapes the frames ahead: changed, those not yet started are worked out again
    const double dSpeed = m_Clock.Speed();
    unsigned long long llKey = (unsigned long long)p.llSpanUs * 1000003ull ^ (unsigned long long)( p.dSendUs * 1e6 ) * 7919ull ^
                               (unsigned long long)( dSpeed * 1e6 ) * 104729ull ^ ( p.bMuted ? 1ull : 0ull ) ^ ( p.bPaused ? 2ull : 0ull ) ^ ( bLimit ? 4ull : 0ull );
    if ( llKey != m_llSimKey )
    {
        m_llSimKey = llKey;
        while ( m_vSim.size() > 1 || ( !m_vSim.empty() && m_vSim.back().llWallStart > llWall ) ) m_vSim.pop_back();
        if ( m_vSim.empty() ) { m_llSimNextWall = 0; m_llSimNextFrom = m_llSimShown; }
        else { m_llSimNextWall = m_vSim.back().llWallEnd; m_llSimNextFrom = m_vSim.back().llTo; }
    }

    // The frames ahead
    size_t nKnots = 0;
    for ( const SimFrame &f : m_vSim ) nKnots += f.vKnots.size();
    long long llStart = m_llSimNextWall ? m_llSimNextWall : llWall;
    for ( int i = 0; llStart < llWall + AheadUs && nKnots < MaxKnots; i++ )
    {
        if ( i == MaxFrames ) llStart = std::max( llStart, llWall );
        if ( i > 4 * MaxFrames ) break;
        const long long llFrom = m_llSimNextFrom;
        long long llTo = std::max( m_Clock.FreeAt( llStart ), llFrom );
        // (the original stops at the song's end)
        if ( llFrom >= llMaxTime ) llTo = llFrom;
        if ( m_llSimNoiseWall ) m_SimNoise.Advance( (double)( llStart - m_llSimNoiseWall ) );
        m_llSimNoiseWall = llStart;
        const PfaLag::FrameCost c = PfaLag::Cost( *m_pSong, m_SimCpu, p, llFrom, llTo, &m_SimNoise );
        double dTotal = std::min( c.dLogicUs + c.dRenderUs, dCap );
        // With Limit FPS, Present waits for the display's next refresh
        if ( bLimit ) dTotal = std::ceil( dTotal / m_dRefreshUs ) * m_dRefreshUs;
        SimFrame f = { llStart, llStart + std::max< long long >( 1, (long long)dTotal ), llFrom, llTo, {} };
        m_llSimNextWall = f.llWallEnd;
        m_llSimNextFrom = llTo;
        llStart = f.llWallEnd;
        if ( f.llWallEnd <= llWall ) { Ended( f ); continue; }
        // Its Logic sends its song time as it gets to it (a short frame: from its start to its end)
        if ( c.dLogicUs < 1000.0 )
            f.vKnots = { { f.llWallStart, llFrom }, { f.llWallStart + (long long)c.dLogicUs, llTo } };
        else
            for ( int k = 0; k < c.nRamp; k++ ) f.vKnots.push_back( { f.llWallStart + (long long)std::min( c.aRamp[k].dUs, dCap ), c.aRamp[k].llSong } );
        nKnots += f.vKnots.size();
        // Developer switch PFAVIZ_SIMLOG=FILE: each frame worked out, and each call's clock
        static const char *sSimLog = getenv( "PFAVIZ_SIMLOG" );
        if ( sSimLog )
            if ( FILE *fl = fopen( sSimLog, "a" ) )
            {
                fprintf( fl, "F %lld %lld %.1f %.1f %lld %lld\n", f.llWallStart, f.llWallEnd, c.dLogicUs, c.dRenderUs, llFrom, llTo );
                fclose( fl );
            }
        m_vSim.push_back( std::move( f ) );
    }

    // The running frame, and the clock's ramp through every frame ahead
    if ( !m_vSim.empty() )
    {
        m_llSimWallStart = m_vSim.front().llWallStart;
        m_llSimWallEnd = m_vSim.front().llWallEnd;
        m_llSimCur = m_vSim.front().llTo;
        std::vector< PlaybackClock::RampKnot > vKnots;
        vKnots.reserve( nKnots );
        for ( const SimFrame &f : m_vSim ) vKnots.insert( vKnots.end(), f.vKnots.begin(), f.vKnots.end() );
        m_Clock.SetRamp( vKnots.data(), (int)vKnots.size() );
    }
    if ( const char *sLog = getenv( "PFAVIZ_SIMLOG" ) )
        if ( FILE *fl = fopen( sLog, "a" ) )
        {
            fprintf( fl, "C %lld %lld %lld %zu\n", llWall, m_Clock.At( llWall ), m_llSimShown, m_vSim.size() );
            fclose( fl );
        }
}

// The CPU the simulation runs on: this one's caches (or the sizes typed in), its speed against the
// one the costs were measured on (the benchmark, run once), times the speed setting
PfaLag::Cpu MainScreen::SimCpu( const VisualSettings &cVisual )
{
    PfaLag::Cpu cpu = PfaLag::Cpu::Detect();
    if ( cVisual.iPfaL2KB > 0 ) cpu.llL2 = (uint64_t)cVisual.iPfaL2KB << 10;
    if ( cVisual.iPfaL3KB > 0 ) cpu.llL3 = (uint64_t)cVisual.iPfaL3KB << 10;
    PfaLag::ApplyBench( cpu, PfaLag::RunBench() );
    cpu.Faster( cVisual.iPfaCpuPct / 100.0 );
    return cpu;
}

void MainScreen::ResetSim( long long llTime )
{
    m_vSim.clear();
    m_llSimCur = m_llSimShown = m_llSimNextFrom = llTime;
    m_llSimNextWall = 0;
    m_llSimWallEnd = 0;
    m_llSimWallStart = Timer::NowMicroSecs();
}

long long MainScreen::ViewEnd( long long llStartTime ) const
{
    // (a row is the span over the notes area's height, which is well over 32 pixels)
    const bool bKeep = ( Config::GetConfig().GetVisualSettings().uClean & Clean::KeepBottom ) != 0;
    return llStartTime + m_llTimeSpan + ( bKeep ? m_llTimeSpan / 32 + 1 : 0 );
}

void MainScreen::JumpTo( long long llStartTime, bool bUpdateGUI )
{
    long long llFirstTime = GetMinTime();
    long long llLastTime = GetMaxTime();
    long long llPrevTime = m_llStartTime;
    m_llStartTime = min( max( llStartTime, llFirstTime ), llLastTime );
    const long long llView = m_pEngine->ToView( m_llStartTime );
    m_pEngine->Jump( m_pEngine->ToView( llStartTime ), llView, ViewEnd( llView ) );

    // AllNotesOff, PlaySkippedEvents and the clock move happen in the engine
    if ( m_pPlayback ) m_pPlayback->Seek( llStartTime, m_llStartTime, llPrevTime );
    else m_Clock.Set( m_llStartTime );
    // (the clock's ramp holds at the new time: the simulation starts a frame there)
    ResetSim( m_llStartTime );
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
    long long aNotesUs[2];
    DrawFrame( m_pRenderer, m_G, *m_pEngine, *m_pMeter, m_iStartTick, m_Look, m_KeyLights, [&]( float fDark ) { RenderStats( fDark ); }, aNotesUs );
    long long tB = aNotesUs[0], tC = aNotesUs[1];
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

ViewInputs MainScreen::GetViewInputs( int iWidth, int iHeight, long long llSongTime ) const
{
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    ViewInputs in;
    in.iBufferWidth = iWidth;
    in.iBufferHeight = iHeight;
    in.eKeysShown = cVisual.eKeysShown;     // as set, also for a file with notes past key 127
    in.iFirstKey = cVisual.iFirstKey;
    in.iLastKey = cVisual.iLastKey;
    in.iSongMinNote = m_pSong->info.iMinNote;
    in.iSongMaxNote = m_pSong->info.iMaxNote;
    in.fOffsetX = m_fOffsetX + m_fTempOffsetX;
    in.fOffsetY = m_fOffsetY + m_fTempOffsetY;
    in.fZoomX = m_fZoomX * m_fTempZoomX;
    in.bShowKB = m_bShowKB;
    in.llStartTime = m_pEngine->ToView( llSongTime );
    in.llSongTime = llSongTime;
    in.llTimeSpan = m_llTimeSpan;
    in.uClean = cVisual.uClean;
    in.bLines = !cVisual.bHideLines;
    in.bEdo31 = cVisual.bEdo31Keys;
    const bool bFx = cVisual.fx.bOn;
    in.bFlat = bFx && cVisual.fx.bFlat;
    in.bNoBar = bFx && cVisual.fx.bNoBar;
    in.iHitLine = bFx && cVisual.fx.bHitLine ? cVisual.fx.iHitLineSize : -1;
    return in;
}

// The scene's drawing, shared by the window and the video renderer
void MainScreen::DrawFrame( Renderer *pRenderer, const FrameGlobals &g, FastEngine &engine, const MeterTracker &meter, long long iTick,
                            Effects::Look &look, Effects::KeyLights &lights, const std::function< void( float ) > &fnStats, long long *pNotesUs )
{
    const EffectSettings &fx = Config::GetConfig().GetVisualSettings().fx;
    if ( !fx.Any() )
    {
        Scene::DrawBackground( pRenderer, g, m_Colors );
        meter.DrawMeasureLines( pRenderer, g, m_Colors, iTick, engine.TickBased() );
        if ( pNotesUs ) pNotesUs[0] = Timer::NowMicroSecs();
        engine.DrawNotes( pRenderer, g, m_vTrackSettings );
        if ( pNotesUs ) pNotesUs[1] = Timer::NowMicroSecs();
        if ( m_bShowKB )
        {
            int aSlot[256];
            engine.GetKeySlots( aSlot );
            const ChannelSettings *pKeyColor[256];
            for ( int i = 0; i < 256; i++ )
                pKeyColor[i] = aSlot[i] < 0 ? nullptr : &m_vTrackSettings[aSlot[i] >> 4].aChannels[aSlot[i] & 15];
            Scene::DrawKeys( pRenderer, g, m_Colors, pKeyColor );
        }
        Scene::DrawBorder( pRenderer, g, m_Colors );
        fnStats( 1.0f );
        return;
    }

    // The colours, and the post pass for the bloom and the vignette (sizes as at 1080p)
    look.Prepare( fx, m_vTrackSettings, m_uTracksGen, m_Colors );
    const SceneColors &c = look.Colors();
    Renderer::PostParams post;
    bool bAnyGlow = false;
    for ( int p = 0; p < EffectSettings::PartCount; p++ ) bAnyGlow |= look.Glows( p );
    bAnyGlow |= fx.bHitLine && fx.iHitLineGlow > 0;
    // (auto exposure: as the last frame left it; it only turns the glow down)
    const float fExposure = lights.Exposure();
    if ( bAnyGlow )
    {
        post.fBloom = fx.iBloom / 100.0f * std::min( fExposure, 1.0f );
        post.fLimit = fx.iBloomLimit / 100.0f;
        post.fRadius = fx.iBloomRadius * g.iBufferHeight / 1080.0f;
        post.fThreshold = fx.iBloomThreshold / 100.0f;
        post.fSceneGlow = fx.aGlow[EffectSettings::PartBackground] / 100.0f;
    }
    post.fVignette = fx.iVignette / 100.0f;
    const bool bPost = ( post.fBloom > 0.0f || post.fVignette > 0.0f ) && pRenderer->BeginPost( post );
    const bool bBloom = bPost && post.fBloom > 0.0f;

    pRenderer->SetBackgroundTint( look.BackgroundDark() );
    Scene::DrawBackground( pRenderer, g, c );
    pRenderer->SetBackgroundTint( 1.0f );
    meter.DrawMeasureLines( pRenderer, g, c, iTick, engine.TickBased() );

    // The notes, by their light
    Effects::NoteLight light;
    light.pFx = &fx;
    light.pLook = &look;
    light.pBase = &m_vTrackSettings;
    light.pShortRest = &look.Rest( EffectSettings::PartShort, m_vTrackSettings );
    light.llNow = g.llStartTime;
    light.bGlow = bBloom && ( look.Glows( EffectSettings::PartNotes ) || look.Glows( EffectSettings::PartShort ) );
    light.fExposure = fExposure;
    engine.SetLight( &light );
    if ( pNotesUs ) pNotesUs[0] = Timer::NowMicroSecs();
    engine.DrawNotes( pRenderer, g, look.Rest( EffectSettings::PartNotes, m_vTrackSettings ) );
    if ( pNotesUs ) pNotesUs[1] = Timer::NowMicroSecs();
    engine.SetLight( nullptr );

    // The keys, by their light: pressed ones from unlit in their note's colours; let go, from their own
    std::vector< Renderer::GlowRect > vKeyGlow;
    {
        int aSlot[256];
        long long aStart[256];
        engine.GetKeySlots( aSlot, aStart );
        // (in song time, so the keys fade alike at every tempo in the tick-based view)
        if ( engine.TickBased() )
            for ( int k = 0; k < 256; k++ )
                if ( aSlot[k] >= 0 ) aStart[k] = engine.FromView( aStart[k] );
        lights.Update( fx, g.llSongTime, aSlot, aStart );
        lights.UpdateExposure( fx, g.llSongTime, g.iStartNote, g.iEndNote );
    }
    if ( m_bShowKB )
    {
        const int eKeys = EffectSettings::PartKeys;
        const bool bGlowKeys = bBloom && ( look.Glows( eKeys ) || look.Glows( EffectSettings::PartBar ) );
        static thread_local ChannelSettings aLit[256], aFade[256];
        const ChannelSettings *pKeyColor[256];
        KeyLook kl;
        if ( bGlowKeys ) kl.pGlow = &vKeyGlow;
        kl.bBorders = fx.bFlatBorders;
        kl.fTopWhite = fx.abKeyWhite[fx.bFlat] ? fx.aiKeyWhite[fx.bFlat] / 100.0f : -1.0f;
        kl.cBarGlow = look.BarGlow();
        for ( int k = 0; k < 256; k++ )
        {
            pKeyColor[k] = nullptr;
            const int iSlot = lights.Slot( k );
            if ( iSlot < 0 || (size_t)( iSlot >> 4 ) >= m_vTrackSettings.size() ) continue;
            const ChannelSettings &base = m_vTrackSettings[iSlot >> 4].aChannels[iSlot & 15];
            const float fLight = std::min( 1.0f, lights.Light( k ) * fExposure );
            if ( lights.Pressed( k ) )
            {
                aLit[k] = look.Lit( eKeys, base, fLight );
                pKeyColor[k] = &aLit[k];
            }
            else
            {
                const ChannelSettings &own = Scene::KeyLayer( g, k ) == 0 ? c.csKBWhite
                                           : g.bEdo31 && Scene::KeyLayer( g, k ) == 1 ? c.csKBHalf : c.csKBSharp;
                aFade[k] = look.Lit( eKeys, base, own, fLight );
                kl.apUnlit[k] = &aFade[k];
            }
            kl.aGlow[k] = look.Glow( eKeys, base, fLight );
        }
        Scene::DrawKeys( pRenderer, g, c, pKeyColor, &kl );
    }
    Scene::DrawBorder( pRenderer, g, c );

    // The hit line, over everything: across in its own colour, and over the keys being played (and let go, fading)
    if ( fx.bHitLine )
    {
        static thread_local HitLineLook h;
        const float fScale = g.iBufferHeight / 1080.0f, fStrength = fx.iHitLineStrength / 100.0f;
        const unsigned cr = fx.iHitLineColor;
        const D3DCOLOR cLine = ( cr & 0xFF ) << 16 | ( cr & 0xFF00 ) | ( cr >> 16 & 0xFF );
        h.fCore = m_bShowKB ? g.fHitLineCY : fx.iHitLineSize > 0 ? std::max( 1.0f, std::floor( fx.iHitLineSize * fScale + 0.5f ) ) : 0.0f;
        h.fSoft = fx.iHitLineSoft * fScale;
        h.fSmear = fx.iHitLineSmear * fScale;
        h.fStrength = fStrength;
        h.cLine = cLine;
        h.bAcross = !fx.bHitLinePlaying;
        h.pGlow = bBloom && fx.iHitLineGlow > 0 ? &vKeyGlow : nullptr;
        h.fGlow = fx.iHitLineGlow / 100.0f;
        const bool bKeys = fx.bHitLinePlaying || fx.bHitLineNoteColor;
        for ( int k = 0; k < 256; k++ )
        {
            const int iSlot = lights.HitSlot( k );
            h.afKey[k] = 0.0f;
            if ( !bKeys || iSlot < 0 || (size_t)( iSlot >> 4 ) >= m_vTrackSettings.size() ) continue;
            h.afKey[k] = lights.HitLight( k );
            h.aKey[k] = fx.bHitLineNoteColor ? m_vTrackSettings[iSlot >> 4].aChannels[iSlot & 15].iPrimaryRGB : cLine;
        }
        Scene::DrawHitLine( pRenderer, g, h );
    }

    if ( fx.bStats ) fnStats( look.BackgroundDark() );
    if ( bPost )
    {
        if ( bBloom )
        {
            const std::vector< Renderer::GlowRect > &vNoteGlow = engine.Glow();
            pRenderer->DrawGlow( vNoteGlow.data(), vNoteGlow.size() );
            pRenderer->DrawGlow( vKeyGlow.data(), vKeyGlow.size() );
        }
        pRenderer->EndPost();
    }
    if ( !fx.bStats ) fnStats( 1.0f );
}

void MainScreen::RenderStats( float fDark )
{
    const int iLines = StatusLines( m_uStats );
    const int iBufferCX = m_pRenderer->GetBufferWidth(), iBufferCY = m_pRenderer->GetBufferHeight();
    const int iBottom = StatsBottom( m_G, iBufferCY );
    StatsBox box = {};
    // Simulating PFA: the last frame it drew, or (Visual > Timing) how far its running frame's Logic has
    // played, as the audio does (through a crash: crawling on while the picture holds)
    long long llTime = m_llStartTime;
    if ( m_bSimOn && Config::GetConfig().GetVisualSettings().bPfaLiveStats )
    {
        // (paused: where it got to; a seek since moves it with the frames)
        if ( !m_bPaused ) m_llSimStatsTime = m_Clock.Now();
        llTime = std::clamp( m_llSimStatsTime, m_llSimShown, std::max( m_llSimShown, m_llSimCur ) );
    }
    if ( iLines )
    {
        box = FitStats( m_pRenderer, iBufferCX, iBufferCY, m_uStats, iBottom );
        DrawStatsBackground( m_pRenderer, box );
        m_pRenderer->BeginText();
        RenderStatus( m_pRenderer, box, llTime, m_dFPS, m_uStats, m_llPastEndUs, fDark );
        m_pRenderer->EndText();
    }
    RenderGraph( m_pRenderer, iBufferCX, iBufferCY, iBottom, llTime, m_llPastEndUs, iLines ? &box : nullptr, fDark, m_GraphScale );
}

static long long TempoAt( const std::vector< RefMetaEvent > &vMeta, long long llTime );

// Per pixel column of a plot, each series' value: the notes per second at its end, the highest polyphony
// in it (from at most 64 of its bins), the tempo at its end. A graph's top eases towards the highest value
// shown (quickly up, slowly down), and its bottom (unless at zero) towards the lowest
void MainScreen::RenderGraph( Renderer *pRenderer, int iWidth, int iHeight, int iBottomY, long long llTime, long long llPastEnd,
                              const StatsBox *pStats, float fDark, GraphScale &scale )
{
    typedef VisualSettings::GraphSettings GS;
    const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    const GS &gs = cVisual.graph;
    if ( !cVisual.bGraph ) return;
    int aiShown[GS::SeriesCount], iShown = 0;
    for ( int iPlace = 0; iPlace < GS::SeriesCount; iPlace++ )
        for ( int i = 0; i < GS::SeriesCount; i++ ) if ( gs.abShow[i] && gs.aiPlace[i] == iPlace ) aiShown[iShown++] = i;
    if ( !iShown ) return;
    const double u = iHeight / 1080.0;
    auto Px = [&]( double v ) { return std::max( 1, (int)std::lround( v * u ) ); };
    const int iCX = std::min( iWidth, Px( gs.iCX ) ), iCY = std::min( iHeight, Px( gs.iCY ) );
    const int iBoxes = gs.bTogether ? 1 : iShown;
    const bool bBottom = ( gs.iCorner & VisualSettings::StatsBottom ) != 0;
    // The boxes from the corner inwards, below (or above) the stats box when it is in the same corner
    const int x = ( gs.iCorner & VisualSettings::StatsLeft ) ? 0 : iWidth - iCX;
    int y = bBottom ? std::clamp( iBottomY, iCY, std::max( iCY, iHeight ) ) - iCY : 0;
    if ( pStats && gs.iCorner == cVisual.iStatsCorner ) y = bBottom ? pStats->rc.top - iCY : pStats->rc.bottom;
    if ( bBottom ) y -= ( iBoxes - 1 ) * iCY;     // (the top one's: in either corner the first place is at the top)

    auto Dark = [&]( D3DCOLOR c ) { return fDark < 1.0f ? ( c & 0xFF000000 ) | ( Effects::Scale( c, fDark ) & 0x00FFFFFF ) : c; };
    D3DCOLOR aColor[GS::SeriesCount];     // (no alpha)
    for ( int s = 0; s < GS::SeriesCount; s++ ) aColor[s] = ( gs.aColor[s] & 0xFF ) << 16 | ( gs.aColor[s] & 0xFF00 ) | ( gs.aColor[s] >> 16 & 0xFF );
    const int iFontCY = Px( gs.iFontPx ), iPad = Px( 4 ), iLabelCY = iFontCY + iPad;
    pRenderer->SetGraphFont( iFontCY, cVisual.sStatsFont );
    const NoteStats &ns = m_pSong->noteStats;

    // The values (counts: none when the song was loaded without them)
    const int n = iCX - iPad * 2;
    if ( n < 2 ) return;
    const long long llNow = llTime + llPastEnd, llSpan = gs.iSpanMs * 1000LL;
    const long long llT0 = llNow - ( gs.bMiddle ? llSpan / 2 : llSpan );
    static thread_local std::vector< double > av[GS::SeriesCount];
    double adMax[GS::SeriesCount], adMin[GS::SeriesCount];
    bool abHave[GS::SeriesCount];
    for ( int s = 0; s < GS::SeriesCount; s++ )
    {
        av[s].assign( n, 0.0 );
        adMax[s] = 0.0;
        adMin[s] = 1e300;
        abHave[s] = s == GS::Tempo || !ns.Empty();
    }
    // (the NPS as NoteStats::NPS sums it, the second's bins up to the column's end, by a window slid along
    // the columns: their ends only go forward)
    const long long llWindow = std::max< long long >( 1, 1000000 / ns.llBinUs ), llLastBin = (long long)ns.vOns.size() - 1;
    long long llLo = 0, llHi = -1;
    uint64_t uWindow = 0;
    for ( int i = 0; i < n; i++ )
    {
        const long long a = llT0 + llSpan * i / n, b = llT0 + llSpan * ( i + 1 ) / n;
        if ( gs.abShow[GS::NPS] && abHave[GS::NPS] && b >= 0 )
        {
            const long long llBin = b / ns.llBinUs;
            const long long lo = std::max( 0LL, llBin - llWindow + 1 ), hi = std::min( llBin, llLastBin );
            if ( lo <= hi )
            {
                if ( lo > llHi ) { llLo = llHi = lo; uWindow = ns.vOns[lo]; }
                while ( llHi < hi ) uWindow += ns.vOns[++llHi];
                while ( llLo < lo ) uWindow -= ns.vOns[llLo++];
                av[GS::NPS][i] = (double)uWindow;
            }
        }
        if ( gs.abShow[GS::Polyphony] && abHave[GS::Polyphony] && b >= 0 )
        {
            uint64_t llPoly = 0;
            const long long llStep = std::max( ns.llBinUs, ( b - a ) / 64 );
            for ( long long t = std::max( 0LL, a ); t < b; t += llStep ) llPoly = std::max( llPoly, ns.Polyphony( t ) );
            av[GS::Polyphony][i] = (double)llPoly;
        }
        if ( gs.abShow[GS::Tempo] ) av[GS::Tempo][i] = 60000000.0 / TempoAt( m_pSong->vMeta, b );
        for ( int s = 0; s < GS::SeriesCount; s++ ) { adMax[s] = std::max( adMax[s], av[s][i] ); adMin[s] = std::min( adMin[s], av[s][i] ); }
    }
    // The scales, eased (a jump, a first frame or a pause of over 2 s: straight there)
    const long long llDt = scale.llLast == LLONG_MIN ? -1 : llNow - scale.llLast;
    const bool bSnap = llDt < 0 || llDt > 2000000;
    auto Ease = [&]( int iMs ) { return bSnap || iMs <= 0 ? 1.0 : 1.0 - std::exp( -llDt / ( iMs * 1000.0 ) ); };
    const double kFast = Ease( gs.iGrowMs ), kSlow = Ease( gs.iShrinkMs );
    scale.llLast = llNow;
    for ( int s = 0; s < GS::SeriesCount; s++ )
    {
        // (a little room above the highest, and below the lowest when the bottom follows it: a level line
        // is drawn halfway up, not along an edge)
        const double dPad = std::max( ( adMax[s] - ( gs.bZero ? 0.0 : adMin[s] ) ) * 0.05, gs.bZero ? 0.0 : 0.5 );
        const double dTop = std::max( adMax[s] + dPad, gs.bZero ? 1.0 : adMin[s] + 1.0 ), dBottom = gs.bZero ? 0.0 : adMin[s] - dPad;
        scale.adTop[s] += ( dTop - scale.adTop[s] ) * ( dTop > scale.adTop[s] ? kFast : kSlow );
        scale.adBottom[s] += ( dBottom - scale.adBottom[s] ) * ( dBottom < scale.adBottom[s] ? kFast : kSlow );
        if ( scale.adTop[s] < scale.adBottom[s] + 1.0 ) scale.adTop[s] = scale.adBottom[s] + 1.0;
    }

    auto Count = []( double d, int s ) {
        if ( s == GS::Tempo ) { wchar_t b[32]; swprintf( b, 32, L"%.2f", d ); return std::wstring( b ); }
        std::wstring t = std::to_wstring( (unsigned long long)std::llround( d ) );
        for ( int i = (int)t.size() - 3; i > 0; i -= 3 ) t.insert( i, L"," );
        return t;
    };
    static const wchar_t *const asName[GS::SeriesCount] = { L"NPS", L"Polyphony", L"BPM" };
    const int iNowCol = gs.bMiddle ? n / 2 : n - 1;
    for ( int iBox = 0; iBox < iBoxes; iBox++ )
    {
        const int yBox = y + iBox * iCY;
        if ( cVisual.bStatsBackground ) pRenderer->DrawRect( (float)x, (float)yBox, (float)iCX, (float)iCY, 0x80000000 );
        const int iX0 = x + iPad, iY0 = yBox + iLabelCY, iY1 = yBox + iCY - iPad, iPlotCY = iY1 - iY0;
        if ( iPlotCY < 2 ) continue;
        const int iFirst = gs.bTogether ? 0 : iBox, iLast = gs.bTogether ? iShown : iBox + 1;
        // Filled ones first, lines over them
        for ( int bLines = 0; bLines < 2; bLines++ )
            for ( int k = iFirst; k < iLast; k++ )
            {
                const int s = aiShown[k];
                if ( gs.abLine[s] != ( bLines != 0 ) || !abHave[s] ) continue;
                const double dLo = scale.adBottom[s], dRange = scale.adTop[s] - dLo;
                auto Y = [&]( double v ) { return iY1 - (int)std::lround( std::clamp( ( v - dLo ) / dRange, 0.0, 1.0 ) * iPlotCY ); };
                const std::vector< double > &v = av[s];
                if ( !gs.abLine[s] )
                {
                    // Columns up from the bottom (a run of one height as one rect)
                    const D3DCOLOR c = Dark( 0x50000000 | aColor[s] );
                    for ( int i = 0; i < n; )
                    {
                        int j = i + 1, yTop = Y( v[i] );
                        while ( j < n && Y( v[j] ) == yTop ) j++;
                        if ( yTop < iY1 ) pRenderer->DrawRect( (float)( iX0 + i ), (float)yTop, (float)( j - i ), (float)( iY1 - yTop ), c );
                        i = j;
                    }
                }
                else
                {
                    // A step line, each level joined to the next by an upright
                    const D3DCOLOR c = Dark( aColor[s] );
                    const int iThick = Px( 2 );
                    int yPrev = INT_MIN;
                    for ( int i = 0; i < n; )
                    {
                        int j = i + 1, yv = Y( v[i] );
                        while ( j < n && Y( v[j] ) == yv ) j++;
                        const int yLine = std::min( yv, iY1 - iThick );
                        pRenderer->DrawRect( (float)( iX0 + i ), (float)yLine, (float)( j - i ), (float)iThick, c );
                        if ( yPrev != INT_MIN && yPrev != yLine )
                            pRenderer->DrawRect( (float)( iX0 + i ), (float)std::min( yPrev, yLine ), (float)iThick, (float)( std::abs( yPrev - yLine ) + iThick ), c );
                        yPrev = yLine;
                        i = j;
                    }
                }
            }
        if ( gs.bMiddle ) pRenderer->DrawRect( (float)( iX0 + n / 2 ), (float)iY0, 1.0f, (float)iPlotCY, Dark( 0x80FFFFFF ) );

        // The text: each series' name and value now, the left ones from the left, the right ones from the
        // right, in their colours
        pRenderer->BeginText();
        int xLeft = iX0, xRight = x + iCX - iPad;
        for ( int k = iFirst; k < iLast; k++ )
        {
            const int s = aiShown[k];
            const std::wstring sText = abHave[s] ? std::wstring( asName[s] ) + L" " + Count( av[s][iNowCol], s ) : std::wstring( asName[s] ) + L": open the song again";
            RECT rcCalc = { 0, 0, 0, 0 };
            pRenderer->DrawText( sText.c_str(), Renderer::Graph, &rcCalc, DT_CALCRECT | DT_SINGLELINE, 0 );
            const int iTextCX = rcCalc.right - rcCalc.left;
            RECT rc = { 0, yBox + iPad / 2, 0, yBox + iPad / 2 + iFontCY + 2 };
            if ( gs.abRight[s] ) { rc.right = xRight; rc.left = xRight - iTextCX; xRight -= iTextCX + iFontCY; }
            else { rc.left = xLeft; rc.right = xLeft + iTextCX; xLeft += iTextCX + iFontCY; }
            pRenderer->DrawText( sText.c_str(), Renderer::Graph, &rc, DT_SINGLELINE, Dark( 0xFF000000 | aColor[s] ) );
        }
        pRenderer->EndText();
    }
}

void MainScreen::RenderText()
{
    int iMsgCY = 200;
    RECT rcMsg = { 0, static_cast< int >( m_pRenderer->GetBufferHeight() * ( 1.0f - KBPercent ) - iMsgCY ) / 2, 0, 0 };
    rcMsg.right = m_pRenderer->GetBufferWidth();
    rcMsg.bottom = rcMsg.top + iMsgCY;

    unsigned iBkgColor = 0x40000000;
    if ( m_bZoomMove )
        m_pRenderer->DrawRect( static_cast< float >( rcMsg.left ), static_cast< float >( rcMsg.top ),
            static_cast< float >( rcMsg.right - rcMsg.left ), static_cast< float >( rcMsg.bottom - rcMsg.top ), iBkgColor );

    m_pRenderer->BeginText();
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

void MainScreen::DrawStatsBackground( Renderer *pRenderer, const StatsBox &box )
{
    if ( !Config::GetConfig().GetVisualSettings().bStatsBackground ) return;
    const RECT &rc = box.rc;
    pRenderer->DrawRect( static_cast< float >( rc.left ), static_cast< float >( rc.top ),
                         static_cast< float >( rc.right - rc.left ), static_cast< float >( rc.bottom - rc.top ), 0x80000000 );
}

// Each line's label and a value as wide as its can get (in the font's digits), so the box keeps its width
// as the values change; measured again when the font or the lines change
MainScreen::StatsBox MainScreen::FitStats( Renderer *pRenderer, int iWidth, int iHeight, unsigned uStats, int iBottomY )
{
    typedef VisualSettings VS;
    const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    const int iLines = StatusLines( uStats );
    StatsBox box = StatsLayout( iWidth, iHeight, iLines, iBottomY );
    static const wchar_t *const aWidest[VS::StatCount] = {
        L"00:00.0 / 00:00.0", L"000.0", L"N/A", L"000,000,000", L"00,000,000", L"000,000", L"000.0K", L"000.0K", L"000.0K",
        L"0000.0 ms", L"< 000, 000 at 000%", L"00.00 / 00.00", L"00.00 / 00.00", L"00.00 / 00.00", L"00.00 / 00.00",
        L"000.00M", L"000% \u00d700", L"000%", L"000% avg, 000% max", L"0000.0 ns/msg", L"000.00 BPM", L"0000", L"000.0%",
        L"000,000,000", L"00,000,000", L"000,000" };
    static std::wstring sFace;
    static int iFontCY = -1, iTextCX = 0;
    static unsigned uMeasured = 0;
    if ( sFace != cVisual.sStatsFont || iFontCY != box.iFontCY || uMeasured != uStats )
    {
        sFace = cVisual.sStatsFont;
        iFontCY = box.iFontCY;
        uMeasured = uStats;
        Renderer::FontSize eFont = Renderer::Small;
        if ( box.iFontCY ) { pRenderer->SetStatsFont( box.iFontCY, sFace ); eFont = Renderer::Stats; }
        auto Width = [&]( const wchar_t *s ) { RECT rc = { 0, 0, 0, 0 }; pRenderer->DrawText( s, eFont, &rc, DT_CALCRECT | DT_SINGLELINE, 0 ); return (int)( rc.right - rc.left ); };
        iTextCX = 0;
        for ( int i = 0; i < VS::StatCount; i++ )
            if ( ( uStats >> i ) & 1 ) iTextCX = std::max( iTextCX, Width( VS::StatLabels[i] ) + box.iPadX + Width( aWidest[i] ) );
    }
    return StatsLayout( iWidth, iHeight, iLines, iBottomY, iTextCX );
}

MainScreen::StatsBox MainScreen::StatsLayout( int iWidth, int iHeight, int iLines, int iBottomY, int iTextCX )
{
    const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    StatsBox b;
    // The size set, its height shared by the lines as the original's: 16 a line with 3 above and below,
    // Tahoma 15, 6 at the sides, the shadow 2 across and 1 down (all in proportion)
    const double u = cVisual.bStatsScaled ? iHeight / 1080.0 : 1.0;
    int iBoxCX = std::max( 1, (int)std::lround( cVisual.iStatsCX * u ) );
    const int iSetCY = std::max( 1, (int)std::lround( cVisual.iStatsCY * u ) );
    const double k = iSetCY / ( std::max( iLines, 1 ) + 6.0 / 16.0 ) / 16.0;
    auto Px = [&]( double v ) { return std::max( 1, (int)std::lround( v * k ) ); };
    b.iLineCY = std::max( 1, (int)( k * 16.0 + 1e-9 ) );
    b.iPadY = std::max( 0, ( iSetCY - b.iLineCY * iLines ) / 2 );
    b.iPadX = Px( 6 ); b.iShadowX = Px( 2 ); b.iShadowY = Px( 1 );
    b.iFontCY = Px( 15 );
    // At the original's line height in Tahoma: its own font (Renderer::Small)
    if ( b.iLineCY == 16 && cVisual.sStatsFont == L"Tahoma" ) b.iFontCY = 0;
    if ( !cVisual.bStatsShadow ) b.iShadowX = b.iShadowY = 0;
    // The shadow mirrored with the corner: to the left in the left ones, up in the bottom ones
    if ( cVisual.bStatsShadowMirror )
    {
        if ( cVisual.iStatsCorner & VisualSettings::StatsLeft ) b.iShadowX = -b.iShadowX;
        if ( cVisual.iStatsCorner & VisualSettings::StatsBottom ) b.iShadowY = -b.iShadowY;
    }
    iBoxCX = std::min( iWidth, std::max( iBoxCX, iTextCX + b.iPadX * 2 ) );
    int iBoxCY = b.iPadY * 2 + b.iLineCY * iLines;
    int x = ( cVisual.iStatsCorner & VisualSettings::StatsLeft ) ? 0 : iWidth - iBoxCX;
    int y = ( cVisual.iStatsCorner & VisualSettings::StatsBottom ) ? std::clamp( iBottomY, iBoxCY, std::max( iBoxCY, iHeight ) ) - iBoxCY : 0;
    b.rc = { x, y, x + iBoxCX, y + iBoxCY };
    return b;
}

int MainScreen::StatsBottom( const FrameGlobals &g, int iHeight ) const
{
    return m_bShowKB ? (int)std::lround( Scene::RedBarY( g ) ) : iHeight;
}

// The tempo in effect at llTime: the last tempo event at or before it. Before any, MIDI's default of
// 500000 (120 BPM), but for the lead-in before the song starts, which shows a tempo set at its start.
static long long TempoAt( const std::vector< RefMetaEvent > &vMeta, long long llTime )
{
    auto Tempo = []( const RefMetaEvent &e ) { return e.cType == RefMetaEvent::SetTempo && e.iDataLen == 3; };
    auto Value = []( const RefMetaEvent &e ) { return std::max( 1, ( e.aData[0] << 16 ) | ( e.aData[1] << 8 ) | e.aData[2] ); };
    auto it = std::upper_bound( vMeta.begin(), vMeta.end(), std::max( llTime, 0LL ), []( long long t, const RefMetaEvent &e ) { return t < e.llMicroSec; } );
    while ( it != vMeta.begin() )
        if ( Tempo( *--it ) ) return Value( *it );
    return 500000;
}

// The top-right stats area's text (the video renderer passes its own renderer, time and frame rate):
// every line's value, then the shown ones in the configured order
void MainScreen::RenderStatus( Renderer *pRenderer, const StatsBox &box, long long llTime, double dFPS, unsigned uStats, long long llPastEnd, float fDark )
{
    typedef VisualSettings VS;
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    const RefMidiInfo &mInfo = m_pSong->info;
    auto Shown = [&]( int iStat ) { return ( uStats >> iStat ) & 1; };
    wchar_t aValue[VS::StatCount][48] = {};

    if ( llTime >= 0 )
        swprintf( aValue[VS::StatTime], 48, L"%lld:%04.1lf / %lld:%04.1lf",
            llTime / 60000000, ( llTime % 60000000 ) / 1000000.0,
            mInfo.llTotalMicroSecs / 60000000, ( mInfo.llTotalMicroSecs % 60000000 ) / 1000000.0 );
    else
        swprintf( aValue[VS::StatTime], 48, L"\t-%lld:%04.1lf / %lld:%04.1lf",
            -llTime / 60000000, ( -llTime % 60000000 ) / 1000000.0,
            mInfo.llTotalMicroSecs / 60000000, ( mInfo.llTotalMicroSecs % 60000000 ) / 1000000.0 );
    swprintf( aValue[VS::StatFPS], 48, L"%.1lf%ls", dFPS, pRenderer == m_pRenderer && m_bSimOn ? L" (PFA sim)" : L"" );
    // Scoring was removed; the line is kept as the original shows it with no input device
    wcscpy( aValue[VS::StatScore], L"N/A" );
    if ( Shown( VS::StatTempo ) ) swprintf( aValue[VS::StatTempo], 48, L"%.2f BPM", 60000000.0 / TempoAt( m_pSong->vMeta, llTime ) );

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
    if ( Shown( VS::StatCC ) ) Count( aValue[VS::StatCC], m_pSong->ccCounts.Passed( llTime ) );
    if ( uStats & VS::NoteStats )
    {
        // From the tables counted while loading (n/a: the song was loaded with them off)
        const NoteStats &ns = m_pSong->noteStats;
        if ( ns.Empty() ) for ( int i : { VS::StatNotes, VS::StatNPS, VS::StatPolyphony, VS::StatMaxNPS, VS::StatMaxPoly } ) wcscpy( aValue[i], L"n/a" );
        else
        {
            Count( aValue[VS::StatNotes], ns.Passed( llTime ) );
            Count( aValue[VS::StatNPS], ns.NPS( llTime + llPastEnd ) );
            Count( aValue[VS::StatPolyphony], ns.Polyphony( llTime ) );
            // (the most up to the point played)
            Count( aValue[VS::StatMaxNPS], ns.MaxNPS( llTime ) );
            Count( aValue[VS::StatMaxPoly], ns.MaxPolyphony( llTime ) );
        }
    }

    if ( uStats & ( VS::SynthStats | VS::FrameStats ) )
    {
        auto Rate = []( wchar_t *s, double d ) {
            if ( d >= 1e6 ) swprintf( s, 48, L"%.2lfM", d / 1e6 );
            else if ( d >= 1e3 ) swprintf( s, 48, L"%.1lfK", d / 1e3 );
            else swprintf( s, 48, L"%.0lf", d );
        };
        Rate( aValue[VS::StatSent], m_dSentPerSec );
        Rate( aValue[VS::StatSkipped], m_dSkippedPerSec );
        Rate( aValue[VS::StatCollapsed], m_dCollapsedPerSec );
        swprintf( aValue[VS::StatLag], 48, L"%.1lf ms", std::max< long long >( 0, m_Stats.llLagUs ) / 1000.0 );
        // The lag's threshold, or the send budget's cut when that is higher ("v at k%": part of velocity v kept)
        const PlaybackDecoder::BudgetCut &cut = m_Stats.budget;
        wchar_t *sSkipping = aValue[VS::StatSkipping];
        if ( m_Stats.iSkipVelocity > 127 ) swprintf( sSkipping, 48, L"all" );
        else if ( cut.iVelocity > 0 && cut.iVelocity >= m_Stats.iSkipVelocity )
            swprintf( sSkipping, 48, L"< %d, %d at %.0f%%", cut.iVelocity, cut.iVelocity, cut.dKeep * 100.0 );
        else if ( m_Stats.iSkipVelocity > 0 ) swprintf( sSkipping, 48, L"vel < %d", m_Stats.iSkipVelocity );
        else swprintf( sSkipping, 48, L"none" );
        // Frame time parts, average / maximum over the last half second, in ms
        for ( int i = 0; i < PartCount; i++ ) swprintf( aValue[VS::StatOtherMs + i], 48, L"%.2f / %.2f", m_adPartAvgShown[i], m_adPartMaxShown[i] );
        // The playback pipeline: a busy decoder with an empty ring is the decoder's limit; a full ring with
        // a busy sender is the output's (a rising send cost: the synth pushing back)
        Rate( aValue[VS::StatDecoded], m_dDecodedPerSec );
        if ( m_iLanes > 1 ) swprintf( aValue[VS::StatDecoderBusy], 48, L"%.0f%% ×%d", m_dDecoderBusy * 100.0, m_iLanes );    // x threads
        else swprintf( aValue[VS::StatDecoderBusy], 48, L"%.0f%%", m_dDecoderBusy * 100.0 );
        swprintf( aValue[VS::StatSenderBusy], 48, L"%.0f%%", m_dSenderBusy * 100.0 );
        swprintf( aValue[VS::StatRing], 48, L"%.0f%% avg, %.0f%% max", m_dRingAvg * 100.0, m_dRingMax * 100.0 );
        swprintf( aValue[VS::StatSendCost], 48, L"%.1f ns/msg", m_Stats.dNsPerMsg );
        // The synth's own counts, when it is KDMAPI and its version has them
        if ( m_iSynthVoices >= 0 ) swprintf( aValue[VS::StatVoices], 48, L"%d", m_iSynthVoices );
        else wcscpy( aValue[VS::StatVoices], L"N/A" );
        if ( m_dSynthCpu >= 0.0 ) swprintf( aValue[VS::StatSynthCpu], 48, L"%.1f%%", m_dSynthCpu );
        else wcscpy( aValue[VS::StatSynthCpu], L"N/A" );
    }

    RECT rcLine = box.rc;
    RECT *prcStatus = &rcLine;
    InflateRect( prcStatus, -box.iPadX, -box.iPadY );
    Renderer::FontSize eFont = Renderer::Small;
    if ( box.iFontCY ) { pRenderer->SetStatsFont( box.iFontCY, Config::GetConfig().GetVisualSettings().sStatsFont ); eFont = Renderer::Stats; }

    // A line: shadow, then text (the original's offsets: the first line at +1, each next a line lower)
    const D3DCOLOR cShadow = fDark < 1.0f ? Effects::Scale( 0xFF404040, fDark ) : 0xFF404040;
    const D3DCOLOR cText = fDark < 1.0f ? Effects::Scale( 0xFFFFFFFF, fDark ) : 0xFFFFFFFF;
    bool bFirst = true;
    for ( int i = 0; i < VS::StatCount; i++ )
    {
        int iStat = cVisual.aStatOrder[i];
        if ( !Shown( iStat ) ) continue;
        OffsetRect( prcStatus, box.iShadowX, bFirst ? box.iShadowY : box.iLineCY + box.iShadowY );
        bFirst = false;
        if ( cVisual.bStatsShadow )
        {
            pRenderer->DrawText( VS::StatLabels[iStat], eFont, prcStatus, 0, cShadow );
            pRenderer->DrawText( aValue[iStat], eFont, prcStatus, DT_RIGHT, cShadow );
        }
        OffsetRect( prcStatus, -box.iShadowX, -box.iShadowY );
        pRenderer->DrawText( VS::StatLabels[iStat], eFont, prcStatus, 0, cText );
        pRenderer->DrawText( aValue[iStat], eFont, prcStatus, DT_RIGHT, cText );
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
