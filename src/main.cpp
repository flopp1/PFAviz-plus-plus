// PFAviz++ entry point: creates the windows, runs the UI message loop, and runs the game
// (logic + render) loop on its own thread, as the original did.
#include <windows.h>
#include <algorithm>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <ctime>

#include "app/LoadingDialog.h"
#include "app/MainWindow.h"
#include "app/VideoExport.h"
#include "core/Config.h"
#include "game/GameState.h"
#include "render/Renderer.h"
#include "resource.h"

HINSTANCE g_hInstance = nullptr;
HWND g_hWnd = nullptr;
HWND g_hWndBar = nullptr;
HWND g_hWndGfx = nullptr;
SpscQueue< MSG > g_MsgQueue;

static DWORD WINAPI GameThread( LPVOID lpParameter )
{
    if ( !g_hWndGfx ) return 0;

    Renderer *pRenderer = new Renderer();
    if ( FAILED( pRenderer->Init( g_hWndGfx, Config::GetConfig().GetVideoSettings().bLimitFPS ) ) )
    {
        MessageBoxW( g_hWnd, L"Fatal error initializing Direct3D 9Ex.", L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( g_hWnd, WM_QUIT, 1, 0 );
        delete pRenderer;
        return 1;
    }

    GameState *pGameState = reinterpret_cast< GameState * >( lpParameter );
    pGameState->SetHWnd( g_hWndGfx );
    pGameState->SetRenderer( pRenderer );
    pGameState->Init();
    GameState::GameError ge = GameState::Success;

    MSG msg = { 0 };
    while ( msg.message != WM_QUIT )
    {
        while ( g_MsgQueue.Pop( msg ) && msg.message != WM_QUIT )
        {
            // Video rendering runs here, on the game thread, until the job is done
            if ( msg.message == WM_COMMAND && LOWORD( msg.wParam ) == ID_VIDEO_RENDER )
            {
                std::unique_ptr< std::shared_ptr< VideoJob > > pJob( reinterpret_cast< std::shared_ptr< VideoJob > * >( msg.lParam ) );
                // A song opened just before is switched to first
                if ( ( ge = GameState::ChangeState( pGameState->NextState(), &pGameState ) ) != GameState::Success )
                    PostMessageW( g_hWnd, WM_COMMAND, ID_GAMEERROR, ge );
                pGameState->RenderVideo( *pJob );
                continue;
            }
            pGameState->MsgProc( msg.hwnd, msg.message, msg.wParam, msg.lParam );
        }
        if ( msg.message == WM_QUIT ) break;

        if ( ( ge = GameState::ChangeState( pGameState->NextState(), &pGameState ) ) != GameState::Success )
            PostMessageW( g_hWnd, WM_COMMAND, ID_GAMEERROR, ge );
        pGameState->Logic();
        pGameState->Render();
    }

    delete pGameState;
    delete pRenderer;
    return 0;
}

int WINAPI wWinMain( HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow )
{
    g_hInstance = hInstance;
    srand( ( unsigned )time( nullptr ) );
    timeBeginPeriod( 1 );

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof( INITCOMMONCONTROLSEX );
    icex.dwICC = ICC_WIN95_CLASSES | ICC_COOL_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx( &icex );

    if ( FAILED( CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED ) ) ) return 1;

    WNDCLASSEXW wc;
    wc.cbSize = sizeof( wc );
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW( hInstance, MAKEINTRESOURCEW( IDI_PFAICON ) );
    wc.hCursor = LoadCursor( nullptr, IDC_ARROW );
    wc.hbrBackground = nullptr; // Container only, never seen
    wc.lpszMenuName = MAKEINTRESOURCEW( IDM_MAINMENU );
    wc.lpszClassName = CLASSNAME;
    wc.hIconSm = nullptr;
    if ( !RegisterClassExW( &wc ) ) return 1;

    wc.style = CS_OWNDC;
    wc.lpfnWndProc = GfxProc;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = GFXCLASSNAME;
    if ( !RegisterClassExW( &wc ) ) return 1;

    wc.style = 0;
    wc.lpfnWndProc = PosnProc;
    wc.lpszClassName = POSNCLASSNAME;
    if ( !RegisterClassExW( &wc ) ) return 1;

    // Triggers loading of the saved config
    Config &config = Config::GetConfig();
    ViewSettings &cView = config.GetViewSettings();
    PlaybackSettings &cPlayback = config.GetPlaybackSettings();

    // Developer switches for timed measurement runs (see also MainScreen::Logic): PFAVIZ_UNCAPPED=1
    // turns off the frame limit, PFAVIZ_NOTESPEED=x sets the note speed, PFAVIZ_MAXIMIZE=1 maximizes
    if ( getenv( "PFAVIZ_UNCAPPED" ) ) { VideoSettings v = config.GetVideoSettings(); v.bLimitFPS = false; config.SetVideoSettings( v ); }
    if ( getenv( "PFAVIZ_NOTESPEED" ) ) cPlayback.SetNSpeed( atof( getenv( "PFAVIZ_NOTESPEED" ) ), false );
    if ( getenv( "PFAVIZ_MAXIMIZE" ) ) nCmdShow = SW_MAXIMIZE;

    // A saved position that is on no monitor (an old minimized position, or a disconnected screen)
    // falls back to the default position
    int iLeft = cView.GetMainLeft(), iTop = cView.GetMainTop();
    if ( iLeft != CW_USEDEFAULT )
    {
        RECT rcSaved = { iLeft, iTop, iLeft + std::max( cView.GetMainWidth(), 1 ), iTop + std::max( cView.GetMainHeight(), 1 ) };
        if ( !MonitorFromRect( &rcSaved, MONITOR_DEFAULTTONULL ) )
        {
            iLeft = iTop = CW_USEDEFAULT;
            cView.SetMainPos( iLeft, iTop );
        }
    }
    g_hWnd = CreateWindowExW( 0, CLASSNAME, APPNAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, iLeft, iTop,
                              cView.GetMainWidth(), cView.GetMainHeight(), nullptr, nullptr, hInstance, nullptr );
    if ( !g_hWnd ) return 1;

    // Creation order (z-order) matters for full screen
    g_hWndBar = CreateRebar( g_hWnd );
    if ( !g_hWndBar ) return 1;

    g_hWndGfx = CreateWindowExW( 0, GFXCLASSNAME, nullptr, WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                 0, 0, 800, 600, g_hWnd, nullptr, hInstance, nullptr );
    if ( !g_hWndGfx ) return 1;

    HACCEL hAccel = LoadAcceleratorsW( hInstance, MAKEINTRESOURCEW( IDA_MAINMENU ) );
    if ( !hAccel ) return 1;

    HANDLE hThread = CreateThread( nullptr, 0, GameThread, new SplashScreen( nullptr, nullptr ), 0, nullptr );
    if ( !hThread ) return 1;

    // Set up GUI and show
    SetPlayMode( PlayMode::Splash );
    SetOnTop( cView.GetOnTop() );
    ShowControls( cView.GetControls() );
    ShowKeyboard( cView.GetKeyboard() );
    ShowWindow( g_hWndGfx, SW_SHOW );
    ShowWindow( g_hWnd, nCmdShow );
    UpdateWindow( g_hWnd );
    SetFocus( g_hWndGfx );
    cPlayback.SetPaused( false, false );

    // A file passed on the command line (or by a file association) is opened right away
    int iArgs = 0;
    LPWSTR *pArgs = CommandLineToArgvW( GetCommandLineW(), &iArgs );
    if ( pArgs && iArgs >= 2 )
        PlayFile( pArgs[1] );
    LocalFree( pArgs );

    MSG msg = { 0 };
    while ( GetMessageW( &msg, nullptr, 0, 0 ) )
    {
        HWND hDlg = GetLoadingDialog();
        if ( hDlg && IsDialogMessageW( hDlg, &msg ) ) continue;
        if ( ( IsSpeedEdit( GetFocus() ) || !TranslateAcceleratorW( g_hWnd, hAccel, &msg ) ) && !IsDialogMessageW( g_hWnd, &msg ) )
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
    }

    // The configuration is saved first: a synth plugin can deadlock the shutdown below
    if ( !getenv( "PFAVIZ_EXIT_AFTER" ) ) config.SaveConfigValues();     // a timed developer run leaves the config alone

    // Signal the game thread to exit and wait for it. If it does not finish, the process ends anyway:
    // a DLL that waits inside its DllMain while holding the loader lock (seen with OmniMIDI's
    // LoudMax VST unloading) blocks every exiting thread, including the ones the game thread joins,
    // and ExitProcess would block on the same lock.
    msg.message = WM_QUIT;
    g_MsgQueue.ForcePush( msg );
    if ( WaitForSingleObject( hThread, 5000 ) == WAIT_TIMEOUT )
        TerminateProcess( GetCurrentProcess(), 0 );

    UnregisterClassW( CLASSNAME, hInstance );
    CoUninitialize();
    timeEndPeriod( 1 );
    return 0;
}
