#include "app/MainWindow.h"

#include "app/LoadingDialog.h"
#include "app/Preferences.h"
#include "app/VideoExport.h"

#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>

#include "core/Config.h"
#include "game/GameState.h"
#include "resource.h"

namespace
{
    // The note speed slider (0 to 1000): logarithmic in two halves, from the slowest note speed (10,
    // left) through the default 0.06 (the middle) to the fastest (0.005, right), so the low values
    // most songs use get half of it. Config's limits are the same.
    const double NSpeedSlow = 10.0, NSpeedMid = 0.06, NSpeedFast = 0.005;
    const int NSpeedSteps = 1000;

    double SliderToNSpeed( int iPos )
    {
        double t = std::clamp( iPos, 0, NSpeedSteps ) / (double)NSpeedSteps;
        if ( t <= 0.5 ) return NSpeedSlow * std::pow( NSpeedMid / NSpeedSlow, t / 0.5 );
        return NSpeedMid * std::pow( NSpeedFast / NSpeedMid, ( t - 0.5 ) / 0.5 );
    }

    int NSpeedToSlider( double d )
    {
        d = std::clamp( d, NSpeedFast, NSpeedSlow );
        double t = d >= NSpeedMid ? 0.5 * std::log( d / NSpeedSlow ) / std::log( NSpeedMid / NSpeedSlow )
                                  : 0.5 + 0.5 * std::log( d / NSpeedMid ) / std::log( NSpeedFast / NSpeedMid );
        return (int)std::lround( t * NSpeedSteps );
    }
}

using std::max;
using std::min;

static WNDPROC g_pPrevBarProc; // The toolbar proc is overridden to make controls transparent

static VOID GetChannelRect( HWND hWnd, RECT *rcChannel );
static VOID GetThumbRect( HWND hWnd, int iPosition, const RECT *rcChannel, RECT *rcThumb );
static INT GetThumbPosition( short iXPos, RECT *rcChannel );
static VOID MoveThumbPosition( int iPositionNew, int &iPosition, HWND hWnd, RECT *rcChannel, RECT *rcThumbOld, BOOL bUpdateGame = TRUE );
static VOID DrawSliderChannel( LPNMCUSTOMDRAW lpnmcd, HWND hWndOwner );
static LRESULT WINAPI BarProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
static LRESULT CALLBACK SliderProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uId, DWORD_PTR dwRef );
static INT_PTR WINAPI AboutProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
static VOID CopyMenuState( HMENU hMenuSrc, HMENU hMenuDest );

//-----------------------------------------------------------------------------
// Main window
//-----------------------------------------------------------------------------

LRESULT WINAPI WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static PlaybackSettings &cPlayback = Config::GetConfig().GetPlaybackSettings();
    static ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const ControlsSettings &cControls = Config::GetConfig().GetControlsSettings();
    static bool bInSizeMove = false;

    switch ( msg )
    {
        case WM_COMMAND:
        {
            int iId = LOWORD( wParam );
            int iCode = HIWORD( wParam );
            switch ( iId )
            {
                case ID_FILE_PLAYSONG:
                {
                    CheckActivity( TRUE );
                    OPENFILENAMEW ofn = { 0 };
                    wchar_t sFilename[1024] = { 0 };
                    ofn.lStructSize = sizeof( ofn );
                    ofn.hwndOwner = hWnd;
                    ofn.lpstrFilter = L"MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
                    ofn.lpstrFile = sFilename;
                    ofn.nMaxFile = sizeof( sFilename ) / sizeof( wchar_t );
                    ofn.lpstrTitle = L"Please select a song to play";
                    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if ( GetOpenFileNameW( &ofn ) )
                        PlayFile( sFilename );
                    return 0;
                }
                case ID_FILE_CLOSEFILE:
                    if ( cPlayback.GetPlayMode() != PlayMode::Play ) break;
                    cPlayback.SetPlayMode( PlayMode::Intro, true );
                    cPlayback.SetPlayable( false, true );
                    cPlayback.SetPosition( 0 );
                    SetWindowTextW( g_hWnd, APPNAME );
                    HandOffMsg( WM_COMMAND, ID_CHANGESTATE, ( LPARAM )new IntroScreen( nullptr, nullptr ) );
                    return 0;
                case ID_PLAY_PLAY:
                    if ( cPlayback.GetPlayMode() == PlayMode::Play ) cPlayback.SetPaused( false, true );
                    return 0;
                case ID_PLAY_PAUSE:
                    cPlayback.SetPaused( true, true );
                    return 0;
                case ID_PLAY_PLAYPAUSE:
                    if ( cPlayback.GetPlayMode() ) cPlayback.TogglePaused( true );
                    return 0;
                case ID_PLAY_STOP:
                case ID_PLAY_SKIPFWD: case ID_PLAY_SKIPBACK:
                    if ( cPlayback.GetPlayMode() == PlayMode::Play ) HandOffMsg( msg, wParam, lParam );
                    return 0;
                case ID_PLAY_INCREASERATE:
                    cPlayback.SetSpeed( cPlayback.GetSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_DECREASERATE:
                    cPlayback.SetSpeed( cPlayback.GetSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_RESETRATE:
                    cPlayback.SetSpeed( 1.0, true );
                    return 0;
                case ID_PLAY_NFASTER:
                    cPlayback.SetNSpeed( cPlayback.GetNSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_NSLOWER:
                    cPlayback.SetNSpeed( cPlayback.GetNSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_NRESET:
                    cPlayback.SetNSpeed( NSpeedMid, true );
                    return 0;
                case ID_PLAY_VOLUMEUP:
                    cPlayback.SetVolume( min( cPlayback.GetVolume() + 0.1, 1.0 ), true );
                    return 0;
                case ID_PLAY_VOLUMEDOWN:
                    cPlayback.SetVolume( max( cPlayback.GetVolume() - 0.1, 0.0 ), true );
                    return 0;
                case ID_PLAY_MUTE:
                    cPlayback.ToggleMute( true );
                    return 0;
                case ID_VIEW_CONTROLS:
                    cView.ToggleControls( true );
                    return 0;
                case ID_VIEW_KEYBOARD:
                    cView.ToggleKeyboard( true );
                    return 0;
                case ID_VIEW_ALWAYSONTOP:
                    cView.ToggleOnTop( true );
                    return 0;
                case ID_VIEW_FULLSCREEN:
                    cView.ToggleFullScreen( true );
                    return 0;
                case ID_VIEW_MOVEANDZOOM:
                case ID_VIEW_RESETMOVEANDZOOM:
                    HandOffMsg( msg, wParam, lParam );
                    return 0;
                case ID_VIEW_NOFULLSCREEN:
                    if ( cView.GetZoomMove() ) HandOffMsg( msg, ID_VIEW_CANCELMOVEANDZOOM, lParam );
                    else if ( cView.GetFullScreen() ) cView.SetFullScreen( false, true );
                    return 0;
                case ID_OPTIONS_PREFERENCES:
                    CheckActivity( TRUE );
                    DoPreferences( hWnd );
                    return 0;
                case ID_HELP_ABOUT:
                    DialogBoxW( g_hInstance, MAKEINTRESOURCEW( IDD_ABOUT ), g_hWnd, AboutProc );
                    return 0;
                case IDC_METRONOME:
                    if ( iCode == CBN_SELCHANGE )
                        cPlayback.SetMetronome( ( PlaybackSettings::Metronome )SendMessage( ( HWND )lParam, CB_GETCURSEL, 0, 0 ) );
                    else if ( iCode == CBN_CLOSEUP && GetFocus() == ( HWND )lParam )
                        SetFocus( g_hWndGfx ); // give the keyboard back to the song once the list closes
                    return 0;
                case ID_GAMEERROR:
                    MessageBoxW( hWnd, GameState::Errors[lParam], L"Error", MB_OK | MB_ICONEXCLAMATION );
                    return 0;
            }
            break;
        }
        case WM_ACTIVATE:
            if ( LOWORD( wParam ) != WA_INACTIVE )
                SetFocus( g_hWndGfx );
            return 0;
        case WM_SYSCOMMAND:
            if ( wParam == SC_SCREENSAVE || wParam == SC_MONITORPOWER )
                if ( cPlayback.GetPlayMode() && !cPlayback.GetPaused() )
                    return 0;
            break;
        case WM_GETMINMAXINFO:
        {
            LPMINMAXINFO lpmmi = ( LPMINMAXINFO )lParam;
            lpmmi->ptMinTrackSize.x = MINWIDTH;
            lpmmi->ptMinTrackSize.y = MINHEIGHT;
            return 0;
        }
        case WM_SIZE:
            if ( wParam == SIZE_MINIMIZED ) return 0;
            SizeWindows( LOWORD( lParam ), HIWORD( lParam ) );
            if ( wParam != SIZE_MAXIMIZED && !cView.GetFullScreen() )
            {
                RECT rcMain;
                GetWindowRect( hWnd, &rcMain );
                cView.SetMainSize( rcMain.right - rcMain.left, rcMain.bottom - rcMain.top );
            }
            if ( !bInSizeMove )
                HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
            return 0;
        case WM_MOVE:
        {
            RECT rcMain;
            GetWindowRect( hWnd, &rcMain );
            // A minimized window is parked at -32000,-32000: never save that as the position
            if ( !cView.GetFullScreen() && !IsZoomed( hWnd ) && !IsIconic( hWnd ) ) cView.SetMainPos( rcMain.left, rcMain.top );
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            bInSizeMove = true;
            return 0;
        case WM_EXITSIZEMOVE:
            HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
            bInSizeMove = false;
            return 0;
        case WM_DEVICECHANGE:
            // Plugging a device in sends a burst of these: the list is read again once they settle
            SetTimer( hWnd, IDT_DEVICECHANGE, 200, nullptr );
            break;
        case WM_TIMER:
            if ( wParam != IDT_DEVICECHANGE ) break;
            KillTimer( hWnd, IDT_DEVICECHANGE );
            Config::GetConfig().LoadMIDIDevices();
            HandOffMsg( WM_DEVICECHANGE, 0, 0 );
            return 0;
        case WM_APP_LOADDONE:
            FinishLoad( lParam );
            return 0;
        case WM_APP_RENDERVIDEO:
            RenderVideoUnattended( reinterpret_cast< const wchar_t * >( lParam ) );
            return 0;
        case WM_DESTROY:
            PostQuitMessage( 0 );
            return 0;
    }

    return DefWindowProcW( hWnd, msg, wParam, lParam );
}

// Used in place of GetMenu because the menu gets detached when in full screen
HMENU GetMainMenu()
{
    static HMENU hMenu = nullptr;
    if ( !hMenu ) hMenu = GetMenu( g_hWnd );
    return hMenu;
}

VOID SizeWindows( int iMainWidth, int iMainHeight )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    int iBarHeight = 0;
    UINT swpFlags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;

    if ( !iMainWidth || !iMainHeight )
    {
        RECT rcMain;
        GetClientRect( g_hWnd, &rcMain );
        iMainWidth = rcMain.right;
        iMainHeight = rcMain.bottom;
    }

    HDWP hdwp = BeginDeferWindowPos( cView.GetControls() + 1 );
    if ( cView.GetControls() )
    {
        RECT rcBarDlg;
        GetWindowRect( g_hWndBar, &rcBarDlg );
        iBarHeight = rcBarDlg.bottom - rcBarDlg.top;
        if ( hdwp ) hdwp = DeferWindowPos( hdwp, g_hWndBar, nullptr, 0, 0, iMainWidth, iBarHeight, swpFlags );
    }
    if ( cView.GetFullScreen() && !cVisual.bAlwaysShowControls ) iBarHeight = 0;
    if ( hdwp ) hdwp = DeferWindowPos( hdwp, g_hWndGfx, nullptr, 0, iBarHeight, iMainWidth, iMainHeight - iBarHeight, swpFlags );
    if ( hdwp ) EndDeferWindowPos( hdwp );
}

//-----------------------------------------------------------------------------
// Graphics window
//-----------------------------------------------------------------------------

LRESULT WINAPI GfxProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    static bool bShowBar;
    static int iBarHeight;
    static bool bTrack = false, bTrackL = false, bTrackR = false;
    static HMENU hMenu;

    if ( ( msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST ) || ( msg >= WM_KEYFIRST && msg <= WM_KEYLAST ) ||
         msg == WM_CAPTURECHANGED || msg == WM_MOUSELEAVE )
        HandOffMsg( msg, wParam, lParam );

    switch ( msg )
    {
        case WM_CREATE:
            hMenu = LoadMenuW( g_hInstance, MAKEINTRESOURCEW( IDR_CONTEXTMENU ) );
            SetTimer( hWnd, IDC_INACTIVITYTIMER, 2500, nullptr );
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
            SetFocus( hWnd );
            if ( !bTrackR && !bTrackL ) SetCapture( hWnd );
            if ( msg == WM_LBUTTONDOWN ) bTrackL = true;
            else bTrackR = true;
            return 0;
        case WM_LBUTTONUP: case WM_RBUTTONUP:
            if ( msg == WM_LBUTTONUP ) bTrackL = false;
            else bTrackR = false;
            if ( !bTrackR && !bTrackL ) ReleaseCapture();
            if ( cView.GetZoomMove() ) return 0;
            break;
        case WM_CAPTURECHANGED:
            bTrackR = bTrackL = false;
            return 0;
        case WM_CONTEXTMENU:
        {
            POINT ptContext = { ( short )LOWORD( lParam ), ( short )HIWORD( lParam ) };
            HWND hWndContext = ( HWND )wParam;
            if ( hWndContext != g_hWndGfx || cView.GetZoomMove() ) break;

            // VK_APPS or Shift F10 were pressed. Figure out where to display the menu
            if ( ptContext.x < 0 && ptContext.y < 0 )
            {
                RECT rcGfx;
                POINT ptMouse;
                GetCursorPos( &ptMouse );
                GetWindowRect( g_hWndGfx, &rcGfx );
                if ( PtInRect( &rcGfx, ptMouse ) )
                    ptContext = ptMouse;
                else
                {
                    ptContext.x = rcGfx.left;
                    ptContext.y = rcGfx.top;
                }
            }

            HMENU hMenuPopup = GetSubMenu( hMenu, 0 );
            HMENU hMenuMain = GetMainMenu();
            CopyMenuState( GetSubMenu( hMenuMain, 0 ), hMenuPopup );
            CopyMenuState( GetSubMenu( hMenuMain, 1 ), hMenuPopup );
            CopyMenuState( GetSubMenu( hMenuMain, 2 ), hMenuPopup );

            CheckActivity( TRUE, nullptr, TRUE );
            TrackPopupMenuEx( hMenuPopup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, ptContext.x, ptContext.y, g_hWnd, nullptr );
            CheckActivity( TRUE, nullptr, TRUE );
            return 0;
        }
        // Send me everything except the tab character
        case WM_GETDLGCODE:
        {
            LRESULT lr = DLGC_WANTARROWS | DLGC_WANTCHARS;
            if ( lParam && wParam != VK_TAB ) lr |= DLGC_WANTMESSAGE;
            return lr;
        }
        // Pop up the controls if the mouse is close enough in full screen
        case WM_MOUSEMOVE:
        {
            if ( !bTrack )
            {
                TRACKMOUSEEVENT tme = { sizeof( TRACKMOUSEEVENT ), TME_LEAVE, hWnd, HOVER_DEFAULT };
                TrackMouseEvent( &tme );
                bTrack = true;
            }

            short iYPos = HIWORD( lParam );
            POINT pt = { ( short )LOWORD( lParam ), iYPos };
            ClientToScreen( hWnd, &pt );
            CheckActivity( TRUE, &pt );

            if ( cView.GetFullScreen() && !cVisual.bAlwaysShowControls )
            {
                if ( !iBarHeight )
                {
                    RECT rcBar;
                    GetWindowRect( g_hWndBar, &rcBar );
                    iBarHeight = rcBar.bottom - rcBar.top;
                }
                if ( iYPos < iBarHeight && cView.GetControls() )
                {
                    ShowWindow( g_hWndBar, SW_SHOWNA );
                    bShowBar = true;
                }
                else if ( bShowBar )
                {
                    ShowWindow( g_hWndBar, SW_HIDE );
                    SetFocus( g_hWndGfx );
                    bShowBar = false;
                }
            }
            break;
        }
        case WM_MOUSELEAVE:
            bTrack = false;
            break;
        case WM_TIMER:
            if ( wParam == IDC_INACTIVITYTIMER )
                CheckActivity( FALSE );
            return 0;
        case WM_DESTROY:
            DestroyMenu( hMenu );
            KillTimer( hWnd, IDC_INACTIVITYTIMER );
            return 0;
    }

    return DefWindowProcW( hWnd, msg, wParam, lParam );
}

static VOID CopyMenuState( HMENU hMenuSrc, HMENU hMenuDest )
{
    MENUITEMINFOW mii;
    mii.cbSize = sizeof( mii );
    mii.fMask = MIIM_STATE | MIIM_CHECKMARKS | MIIM_ID;
    int iCount = GetMenuItemCount( hMenuSrc );
    for ( int i = 0; i < iCount; i++ )
        if ( GetMenuItemInfoW( hMenuSrc, i, TRUE, &mii ) )
            SetMenuItemInfoW( hMenuDest, mii.wID, FALSE, &mii );
}

//-----------------------------------------------------------------------------
// Rebar / toolbar
//-----------------------------------------------------------------------------

// Override of the toolbar proc to draw transparent controls
static LRESULT WINAPI BarProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static PlaybackSettings &cPlayback = Config::GetConfig().GetPlaybackSettings();

    switch ( msg )
    {
        // WM_HSCROLL doesn't percolate up, so got to handle it here
        case WM_HSCROLL:
        {
            int iCode = LOWORD( wParam );
            if ( iCode == TB_LINEUP || iCode == TB_LINEDOWN || iCode == TB_PAGEUP ||
                 iCode == TB_PAGEDOWN || iCode == TB_THUMBTRACK || iCode == TB_THUMBPOSITION )
            {
                HWND hWndTrackbar = ( HWND )lParam;
                int iPos = ( iCode == TB_THUMBTRACK || iCode == TB_THUMBPOSITION ? HIWORD( wParam ) : (int)SendMessage( hWndTrackbar, TBM_GETPOS, 0, 0 ) );
                switch ( GetDlgCtrlID( hWndTrackbar ) )
                {
                    case IDC_VOLUME:
                        cPlayback.SetVolume( iPos / 100.0, false );
                        return 0;
                    case IDC_SPEED:
                        if ( iPos < 108 && iPos > 92 && iPos != 100 ) cPlayback.SetSpeed( 1.0, true );
                        else cPlayback.SetSpeed( iPos / 100.0, false );
                        return 0;
                    case IDC_NSPEED:
                        // Snaps to the default around the middle
                        if ( std::abs( iPos - NSpeedSteps / 2 ) < 8 && iPos != NSpeedSteps / 2 ) cPlayback.SetNSpeed( NSpeedMid, true );
                        else cPlayback.SetNSpeed( SliderToNSpeed( iPos ), false );
                        return 0;
                }
            }
            break;
        }
        // This makes our static text have a transparent background
        case WM_CTLCOLORSTATIC:
        {
            HDC hDC = ( HDC )wParam;
            SetBkMode( hDC, TRANSPARENT );
            return ( INT_PTR )GetStockObject( NULL_BRUSH );
        }
        // Slider controls: draw our own channel
        case WM_NOTIFY:
        {
            LPNMHDR lpnmhdr = ( LPNMHDR )lParam;
            if ( lpnmhdr->code == NM_CUSTOMDRAW &&
                 ( lpnmhdr->idFrom == IDC_VOLUME || lpnmhdr->idFrom == IDC_SPEED || lpnmhdr->idFrom == IDC_NSPEED ) )
            {
                LPNMCUSTOMDRAW lpnmcd = ( LPNMCUSTOMDRAW )lParam;
                switch ( lpnmcd->dwDrawStage )
                {
                    case CDDS_PREPAINT:
                        return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT:
                        if ( lpnmcd->dwItemSpec != TBCD_CHANNEL )
                            return CDRF_DODEFAULT;
                        DrawSliderChannel( lpnmcd, hWnd );
                        return CDRF_DODEFAULT;
                }
            }
            break;
        }
        case WM_MOUSEMOVE:
        {
            POINT pt = { ( short )LOWORD( lParam ), ( short )HIWORD( lParam ) };
            ClientToScreen( hWnd, &pt );
            CheckActivity( TRUE, &pt );
            break;
        }
    }

    return CallWindowProcW( g_pPrevBarProc, hWnd, msg, wParam, lParam );
}

// A click on a slider gives it the keyboard focus, after which the arrow keys move the slider instead of
// controlling playback. Hand the focus back to the song when a mouse adjustment ends; tabbing to a slider
// still leaves it keyboard controlled.
static LRESULT CALLBACK SliderProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
{
    LRESULT lr = DefSubclassProc( hWnd, msg, wParam, lParam );
    if ( ( msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP ) && GetFocus() == hWnd )
        SetFocus( g_hWndGfx );
    else if ( msg == WM_NCDESTROY )
        RemoveWindowSubclass( hWnd, SliderProc, 0 );
    return lr;
}

HWND CreateRebar( HWND hWndOwner )
{
    HWND hWndRebar = CreateWindowExW( WS_EX_CONTROLPARENT, REBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | CCS_NODIVIDER | RBS_VARHEIGHT,
        0, 0, 0, 0, hWndOwner, ( HMENU )IDC_TOPREBAR, g_hInstance, nullptr );
    if ( !hWndRebar ) return nullptr;

    // Create the system font
    const INT ITEM_POINT_SIZE = 10;
    HDC hDC = GetDC( hWndOwner );
    INT nFontHeight = MulDiv( ITEM_POINT_SIZE, GetDeviceCaps( hDC, LOGPIXELSY ), 72 );
    HFONT hFont = CreateFontW( nFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Shell Dlg 2" );
    ReleaseDC( hWndOwner, hDC );

    HIMAGELIST hIml = ImageList_LoadImageW( g_hInstance, MAKEINTRESOURCEW( IDB_MEDIAICONSSMALL ),
                                            16, 20, RGB( 255, 255, 0 ), IMAGE_BITMAP, LR_CREATEDIBSECTION );

    HWND hWndToolbar = CreateWindowExW( WS_EX_CONTROLPARENT, TOOLBARCLASSNAMEW, nullptr,
                                        WS_CHILD | WS_TABSTOP | CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS,
                                        0, 0, 0, 0, hWndRebar, ( HMENU )IDC_TOPTOOLBAR, g_hInstance, nullptr );
    if ( !hWndToolbar ) return nullptr;

    TBBUTTON tbButtons[8] =
    {
        { MAKELONG( 0, 0 ), ID_PLAY_PLAY, 0, BTNS_BUTTON, { 0 }, 0, ( INT_PTR )L"Play" },
        { MAKELONG( 1, 0 ), ID_PLAY_PAUSE, 0, BTNS_BUTTON, { 0 }, 0, ( INT_PTR )L"Pause" },
        { MAKELONG( 2, 0 ), ID_PLAY_STOP, 0, BTNS_BUTTON, { 0 }, 0, ( INT_PTR )L"Stop" },
        { 0, 0, TBSTATE_ENABLED, BTNS_SEP, { 0 }, 0, 0 },
        { MAKELONG( 3, 0 ), ID_PLAY_SKIPBACK, 0, BTNS_BUTTON, { 0 }, 0, ( INT_PTR )L"Skip Back" },
        { MAKELONG( 4, 0 ), ID_PLAY_SKIPFWD, 0, BTNS_BUTTON, { 0 }, 0, ( INT_PTR )L"Skip Fwd" },
        { 0, 0, TBSTATE_ENABLED, BTNS_SEP, { 0 }, 0, 0 },
        { MAKELONG( 5, 0 ), ID_PLAY_MUTE, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, ( INT_PTR )L"Mute" }
    };

    g_pPrevBarProc = ( WNDPROC )SetWindowLongPtrW( hWndToolbar, GWLP_WNDPROC, ( LONG_PTR )BarProc );
    SendMessageW( hWndToolbar, TB_SETIMAGELIST, 0, ( LPARAM )hIml );
    SendMessageW( hWndToolbar, TB_SETMAXTEXTROWS, 0, 0 );
    SendMessageW( hWndToolbar, TB_BUTTONSTRUCTSIZE, sizeof( TBBUTTON ), 0 );
    SendMessageW( hWndToolbar, TB_ADDBUTTONSW, sizeof( tbButtons ) / sizeof( TBBUTTON ), ( LPARAM )&tbButtons );
    SendMessageW( hWndToolbar, TB_SETBUTTONSIZE, 0, MAKELONG( 32, 29 ) );

    HWND hWndVolume = CreateWindowExW( 0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_BOTH | TBS_NOTICKS | TBS_TOOLTIPS,
                                       209, 2, 75, 26, hWndToolbar, ( HMENU )IDC_VOLUME, g_hInstance, nullptr );
    SendMessageW( hWndVolume, TBM_SETRANGE, FALSE, MAKELONG( 0, 100 ) );
    SendMessageW( hWndVolume, TBM_SETLINESIZE, 0, 5 );

    HWND hWndStatic1 = CreateWindowExW( 0, WC_STATICW, nullptr, WS_CHILD | WS_VISIBLE | SS_BLACKFRAME,
                                        288, 2, 1, 25, hWndToolbar, nullptr, g_hInstance, nullptr );
    HWND hWndStatic2 = CreateWindowExW( 0, WC_STATICW, nullptr, WS_CHILD | WS_VISIBLE | SS_WHITEFRAME,
                                        289, 2, 1, 25, hWndToolbar, nullptr, g_hInstance, nullptr );

    HWND hWndStatic3 = CreateWindowExW( 0, WC_STATICW, L"Metronome:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        297, 8, 60, 13, hWndToolbar, nullptr, g_hInstance, nullptr );
    HWND hWndMetronome = CreateWindowExW( 0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          361, 4, 99, 100, hWndToolbar, ( HMENU )IDC_METRONOME, g_hInstance, nullptr );
    SendMessageW( hWndMetronome, CB_ADDSTRING, 0, ( LPARAM )L"(Off)" );
    SendMessageW( hWndMetronome, CB_ADDSTRING, 0, ( LPARAM )L"Every Beat" );
    SendMessageW( hWndMetronome, CB_ADDSTRING, 0, ( LPARAM )L"Every Measure" );

    HWND hWndStatic4 = CreateWindowExW( 0, WC_STATICW, nullptr, WS_CHILD | WS_VISIBLE | SS_BLACKFRAME,
                                        468, 2, 1, 25, hWndToolbar, nullptr, g_hInstance, nullptr );
    HWND hWndStatic5 = CreateWindowExW( 0, WC_STATICW, nullptr, WS_CHILD | WS_VISIBLE | SS_WHITEFRAME,
                                        469, 2, 1, 25, hWndToolbar, nullptr, g_hInstance, nullptr );

    HWND hWndStatic6 = CreateWindowExW( 0, WC_STATICW, L"Playback:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        477, 8, 44, 13, hWndToolbar, nullptr, g_hInstance, nullptr );
    HWND hWndSpeed = CreateWindowExW( 0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_BOTH | TBS_NOTICKS,
                                      522, 2, 100, 26, hWndToolbar, ( HMENU )IDC_SPEED, g_hInstance, nullptr );
    SendMessageW( hWndSpeed, TBM_SETRANGE, FALSE, MAKELONG( 5, 195 ) );
    SendMessageW( hWndSpeed, TBM_SETLINESIZE, 0, 10 );

    HWND hWndStatic7 = CreateWindowExW( 0, WC_STATICW, L"Notes:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        629, 8, 35, 13, hWndToolbar, nullptr, g_hInstance, nullptr );
    HWND hWndNSpeed = CreateWindowExW( 0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_BOTH | TBS_NOTICKS,
                                       665, 2, 100, 26, hWndToolbar, ( HMENU )IDC_NSPEED, g_hInstance, nullptr );
    SendMessageW( hWndNSpeed, TBM_SETRANGE, FALSE, MAKELONG( 0, NSpeedSteps ) );
    SendMessageW( hWndNSpeed, TBM_SETLINESIZE, 0, 10 );

    for ( HWND h : { hWndVolume, hWndSpeed, hWndNSpeed } )
        SetWindowSubclass( h, SliderProc, 0, 0 );

    HWND hWndPosn = CreateWindowExW( 0, POSNCLASSNAME, nullptr, WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                     0, 0, 0, 0, hWndRebar, ( HMENU )IDC_POSNCTRL, g_hInstance, nullptr );

    // Set the font to the dialog font
    HWND aFontWnds[] = { hWndVolume, hWndStatic1, hWndStatic2, hWndStatic3, hWndMetronome, hWndStatic4, hWndStatic5, hWndStatic6, hWndStatic7, hWndSpeed };
    for ( HWND h : aFontWnds )
        SendMessageW( h, WM_SETFONT, ( WPARAM )hFont, FALSE );

    REBARBANDINFOW rbbi = {};
    rbbi.cbSize = sizeof( rbbi );
    rbbi.fMask = RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_STYLE | RBBIM_TEXT;

    rbbi.fStyle = RBBS_NOGRIPPER | RBBS_VARIABLEHEIGHT;
    rbbi.lpText = ( LPWSTR )L"";
    rbbi.hwndChild = hWndToolbar;
    rbbi.cxMinChild = rbbi.cyIntegral = 0;
    rbbi.cyMinChild = rbbi.cyChild = rbbi.cyMaxChild = 31;
    SendMessageW( hWndRebar, RB_INSERTBANDW, (WPARAM)-1, ( LPARAM )&rbbi );

    rbbi.fStyle = RBBS_NOGRIPPER | RBBS_VARIABLEHEIGHT | RBBS_BREAK;
    rbbi.hwndChild = hWndPosn;
    rbbi.cyMinChild = rbbi.cyChild = rbbi.cyMaxChild = 20;
    SendMessageW( hWndRebar, RB_INSERTBANDW, (WPARAM)-1, ( LPARAM )&rbbi );

    // Now that the controls are created and set up, fill out the default values
    const PlaybackSettings &cPlayback = Config::GetConfig().GetPlaybackSettings();
    g_hWndBar = hWndRebar; // SetMute needs it
    SetMute( cPlayback.GetMute() );
    SendMessageW( hWndSpeed, TBM_SETPOS, TRUE, ( LONG )( 100 * cPlayback.GetSpeed() + .5 ) );
    SendMessageW( hWndNSpeed, TBM_SETPOS, TRUE, NSpeedToSlider( cPlayback.GetNSpeed() ) );
    SendMessageW( hWndVolume, TBM_SETPOS, TRUE, ( LONG )( 100 * cPlayback.GetVolume() + .5 ) );
    SendMessageW( hWndMetronome, CB_SETCURSEL, cPlayback.GetMetronome(), 0 );

    return hWndRebar;
}

// Transparent slider. Draw the parent's background into a mem DC, blit to screen.
static VOID DrawSliderChannel( LPNMCUSTOMDRAW lpnmcd, HWND hWndOwner )
{
    static HDC hdcMem = nullptr;
    static HBITMAP hBitmap = nullptr;

    RECT rcCtrl, rcOwner;
    GetWindowRect( lpnmcd->hdr.hwndFrom, &rcCtrl );
    GetWindowRect( hWndOwner, &rcOwner );
    RECT rcCtrlClient = rcCtrl, rcOwnerClient = rcOwner;
    OffsetRect( &rcCtrlClient, -rcCtrl.left, -rcCtrl.top );
    OffsetRect( &rcOwnerClient, -rcOwner.left, -rcOwner.top );

    // Only make the copy of the parent once
    if ( !hdcMem && !hBitmap )
    {
        hdcMem = CreateCompatibleDC( lpnmcd->hdc );
        hBitmap = CreateCompatibleBitmap( lpnmcd->hdc, rcOwner.right - rcOwner.left, rcOwner.bottom - rcOwner.top );
        SelectObject( hdcMem, hBitmap );
        FillRect( hdcMem, &rcOwnerClient, ( HBRUSH )( COLOR_BTNFACE + 1 ) );
        SendMessageW( hWndOwner, WM_PRINTCLIENT, ( WPARAM )hdcMem, PRF_CLIENT | PRF_NONCLIENT );
    }

    BitBlt( lpnmcd->hdc, 0, 0, rcCtrl.right - rcCtrl.left, rcCtrl.bottom - rcCtrl.top,
            hdcMem, rcCtrl.left - rcOwner.left, rcCtrl.top - rcOwner.top, SRCCOPY );
    if ( GetFocus() == lpnmcd->hdr.hwndFrom )
        DrawFocusRect( lpnmcd->hdc, &rcCtrlClient );
}

//-----------------------------------------------------------------------------
// Position slider
//-----------------------------------------------------------------------------

LRESULT WINAPI PosnProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static BOOL bEnabled = TRUE, bTracking = FALSE;
    static int iPosition = 0;
    static HIMAGELIST hIml = nullptr;
    static HBITMAP hBackbuffer = nullptr;
    static HBITMAP hBackground = nullptr;

    switch ( msg )
    {
        case WM_CREATE:
            hIml = ImageList_LoadImageW( g_hInstance, MAKEINTRESOURCEW( IDB_MEDIAICONSSMALL ),
                                         16, 20, CLR_DEFAULT, IMAGE_BITMAP, LR_CREATEDIBSECTION );
            bEnabled = ( ( GetWindowLongPtrW( hWnd, GWL_STYLE ) & WS_DISABLED ) == 0 );
            return 0;
        case WM_ENABLE:
        {
            bEnabled = (BOOL)wParam;
            if ( !bEnabled )
            {
                iPosition = 0;
                if ( bTracking )
                {
                    bTracking = FALSE;
                    ReleaseCapture();
                }
            }
            RECT rc;
            SendMessageW( g_hWndBar, RB_GETRECT, 1, ( LPARAM )&rc );
            RedrawWindow( g_hWndBar, &rc, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN );
            return 0;
        }
        // 3 draws, but complicated in order to prevent flicker
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hDC = BeginPaint( hWnd, &ps );
            HDC hDCMem = CreateCompatibleDC( hDC );
            HDC hDCBkg = CreateCompatibleDC( hDCMem );

            RECT rcCtrl, rcOwner;
            GetWindowRect( hWnd, &rcCtrl );
            GetWindowRect( g_hWndBar, &rcOwner );
            RECT rcCtrlClient = rcCtrl, rcOwnerClient = rcOwner;
            OffsetRect( &rcCtrlClient, -rcCtrl.left, -rcCtrl.top );
            OffsetRect( &rcOwnerClient, -rcOwner.left, -rcOwner.top );

            HGDIOBJ hbmpOld2 = nullptr;
            if ( !hBackbuffer )
            {
                hBackbuffer = CreateCompatibleBitmap( hDC, rcCtrlClient.right, rcCtrlClient.bottom );
                hBackground = CreateCompatibleBitmap( hDC, rcOwner.right - rcOwner.left, rcOwner.bottom - rcOwner.top );
                hbmpOld2 = SelectObject( hDCBkg, hBackground );
                FillRect( hDCBkg, &rcOwnerClient, ( HBRUSH )( COLOR_BTNFACE + 1 ) );
                SendMessageW( g_hWndBar, WM_PRINT, ( WPARAM )hDCBkg, PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND );
            }
            else
                hbmpOld2 = SelectObject( hDCBkg, hBackground );
            HGDIOBJ hbmpOld1 = SelectObject( hDCMem, hBackbuffer );

            RECT rcChannel, rcThumb;
            GetChannelRect( hWnd, &rcChannel );
            GetThumbRect( hWnd, iPosition, &rcChannel, &rcThumb );

            BitBlt( hDCMem, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                    hDCBkg, rcCtrl.left - rcOwner.left + ps.rcPaint.left, rcCtrl.top - rcOwner.top + ps.rcPaint.top, SRCCOPY );
            if ( bEnabled )
            {
                SetDCBrushColor( hDCMem, RGB( 255, 255, 255 ) );
                FillRect( hDCMem, &rcChannel, ( HBRUSH )GetStockObject( DC_BRUSH ) );
            }
            DrawEdge( hDCMem, &rcChannel, BDR_SUNKENOUTER, BF_RECT );
            ImageList_DrawEx( hIml, 7 + bEnabled, hDCMem, rcThumb.left, rcThumb.top, rcThumb.right - rcThumb.left, rcThumb.bottom - rcThumb.top,
                              CLR_DEFAULT, CLR_DEFAULT, ILD_NORMAL );
            BitBlt( hDC, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                    hDCMem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY );

            SelectObject( hDCBkg, hbmpOld2 );
            DeleteDC( hDCBkg );
            SelectObject( hDCMem, hbmpOld1 );
            DeleteDC( hDCMem );
            EndPaint( hWnd, &ps );
            return 0;
        }
        case WM_SIZE:
            if ( hBackbuffer ) DeleteObject( hBackbuffer );
            if ( hBackground ) DeleteObject( hBackground );
            hBackbuffer = nullptr;
            hBackground = nullptr;
            return 0;
        case WM_LBUTTONDOWN:
        {
            if ( !bEnabled ) return 0;
            POINT pt = { ( SHORT )LOWORD( lParam ), ( SHORT )HIWORD( lParam ) };

            RECT rcChannel, rcThumb;
            GetChannelRect( hWnd, &rcChannel );
            GetThumbRect( hWnd, iPosition, &rcChannel, &rcThumb );
            InflateRect( &rcChannel, 0, 4 );

            if ( PtInRect( &rcChannel, pt ) || PtInRect( &rcThumb, pt ) )
            {
                int iPositionNew = GetThumbPosition( ( SHORT )pt.x, &rcChannel );
                MoveThumbPosition( iPositionNew, iPosition, hWnd, &rcChannel, &rcThumb );
                bTracking = TRUE;
                SetCapture( hWnd );
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            bTracking = false;
            return 0;
        case WM_LBUTTONUP:
            if ( bTracking ) ReleaseCapture();
            bTracking = FALSE;
            return 0;
        case WM_MOUSEMOVE:
        {
            if ( !bTracking ) return 0;
            short iXPos = LOWORD( lParam );
            RECT rcChannel, rcThumbOld;
            GetChannelRect( hWnd, &rcChannel );
            GetThumbRect( hWnd, iPosition, &rcChannel, &rcThumbOld );
            int iPositionNew = GetThumbPosition( iXPos, &rcChannel );
            MoveThumbPosition( iPositionNew, iPosition, hWnd, &rcChannel, &rcThumbOld );
            return 0;
        }
        case TBM_SETPOS:
        {
            RECT rcChannel, rcThumbOld;
            GetChannelRect( hWnd, &rcChannel );
            GetThumbRect( hWnd, iPosition, &rcChannel, &rcThumbOld );
            int iPositionNew = max( min( (int)lParam, 1000 ), 0 );
            MoveThumbPosition( iPositionNew, iPosition, hWnd, &rcChannel, &rcThumbOld, FALSE );
            return 0;
        }
        case WM_DESTROY:
            if ( hBackbuffer ) DeleteObject( hBackbuffer );
            if ( hBackground ) DeleteObject( hBackground );
            ImageList_Destroy( hIml );
            return 0;
    }

    return DefWindowProcW( hWnd, msg, wParam, lParam );
}

static VOID GetChannelRect( HWND hWnd, RECT *rcChannel )
{
    static const int iSize = 7;
    GetClientRect( hWnd, rcChannel );
    InflateRect( rcChannel, -iSize, -( rcChannel->bottom - rcChannel->top - iSize ) / 2 );
    rcChannel->right--;
    if ( rcChannel->bottom - rcChannel->top == iSize + 1 )
        rcChannel->bottom--;
}

static VOID GetThumbRect( HWND hWnd, int iPosition, const RECT *rcChannel, RECT *rcThumb )
{
    int iSize = rcChannel->bottom - rcChannel->top;
    int iPixel = ( 2 * iPosition * ( rcChannel->right - rcChannel->left - 1 ) + 1000 ) / ( 2 * 1000 );
    rcThumb->left = rcChannel->left + iPixel - iSize / 2 - 3;
    rcThumb->top = rcChannel->top - 4;
    rcThumb->right = rcThumb->left + iSize + 6;
    rcThumb->bottom = rcThumb->top + iSize + 8;
}

static INT GetThumbPosition( short iXPos, RECT *rcChannel )
{
    int iWidth = max( 1, (int)( rcChannel->right - rcChannel->left ) );
    int iPositionNew = ( 2 * 1000 * ( iXPos - rcChannel->left ) + iWidth ) / ( 2 * iWidth );
    return max( min( iPositionNew, 1000 ), 0 );
}

static VOID MoveThumbPosition( int iPositionNew, int &iPosition, HWND hWnd, RECT *rcChannel, RECT *rcThumbOld, BOOL bUpdateGame )
{
    RECT rcThumbNew, rcInvalid;
    GetThumbRect( hWnd, iPositionNew, rcChannel, &rcThumbNew );
    UnionRect( &rcInvalid, rcThumbOld, &rcThumbNew );
    InflateRect( &rcInvalid, 10, 0 );

    if ( iPositionNew != iPosition )
    {
        MSG msg;
        iPosition = iPositionNew;
        RedrawWindow( hWnd, &rcInvalid, nullptr, RDW_INVALIDATE | RDW_UPDATENOW );
        if ( bUpdateGame )
        {
            HandOffMsg( TBM_SETPOS, 0, iPosition );
            while ( PeekMessageW( &msg, hWnd, TBM_SETPOS, TBM_SETPOS, PM_REMOVE ) );
        }
    }
}

//-----------------------------------------------------------------------------
// About box
//-----------------------------------------------------------------------------

static INT_PTR WINAPI AboutProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static HANDLE hSplash = nullptr;

    switch ( msg )
    {
        case WM_INITDIALOG:
        {
            if ( !hSplash ) hSplash = LoadImageW( g_hInstance, MAKEINTRESOURCEW( IDB_SPLASH ), IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR );
            SendMessageW( GetDlgItem( hWnd, IDC_PICTURE ), STM_SETIMAGE, IMAGE_BITMAP, ( LPARAM )hSplash );

            RECT rcPos, rcParent;
            GetClientRect( hWnd, &rcPos );
            GetWindowRect( GetParent( hWnd ), &rcParent );
            SetWindowPos( hWnd, nullptr, rcParent.left + ( rcParent.right - rcParent.left - rcPos.right ) / 2,
                rcParent.top + ( rcParent.bottom - rcParent.top - rcPos.bottom ) / 2,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOACTIVATE );
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode( ( HDC )wParam, TRANSPARENT );
            return ( INT_PTR )GetStockObject( WHITE_BRUSH );
        case WM_COMMAND:
            switch ( LOWORD( wParam ) )
            {
                case IDOK: case IDCANCEL:
                    EndDialog( hWnd, IDOK );
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

//-----------------------------------------------------------------------------
// Helpers involved with user interaction and the GUI
//-----------------------------------------------------------------------------

VOID HandOffMsg( UINT msg, WPARAM wParam, LPARAM lParam )
{
    MSG msgGameThread = { g_hWndGfx, msg, wParam, lParam, 0, { 0, 0 } };
    g_MsgQueue.ForcePush( msgGameThread );
}

VOID ShowControls( BOOL bShow )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    SizeWindows( 0, 0 );
    if ( !cView.GetFullScreen() || cVisual.bAlwaysShowControls )
        ShowWindow( g_hWndBar, bShow ? SW_SHOWNA : SW_HIDE );
    else
        ShowWindow( g_hWndBar, SW_HIDE );
    HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
    CheckMenuItem( GetMainMenu(), ID_VIEW_CONTROLS, MF_BYCOMMAND | ( bShow ? MF_CHECKED : MF_UNCHECKED ) );
}

VOID ShowKeyboard( BOOL bShow )
{
    CheckMenuItem( GetMainMenu(), ID_VIEW_KEYBOARD, MF_BYCOMMAND | ( bShow ? MF_CHECKED : MF_UNCHECKED ) );
}

VOID SetOnTop( BOOL bOnTop )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    if ( !cView.GetFullScreen() )
        SetWindowPos( g_hWnd, bOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
    CheckMenuItem( GetMainMenu(), ID_VIEW_ALWAYSONTOP, MF_BYCOMMAND | ( bOnTop ? MF_CHECKED : MF_UNCHECKED ) );
}

VOID SetFullScreen( BOOL bFullScreen )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    static RECT rcOld = { 0 };
    HMENU hMenu = GetMainMenu();

    if ( bFullScreen )
    {
        // Full screen on the monitor holding the window (the original used the desktop rect)
        GetWindowRect( g_hWnd, &rcOld );
        MONITORINFO mi = { sizeof( mi ) };
        GetMonitorInfoW( MonitorFromWindow( g_hWnd, MONITOR_DEFAULTTONEAREST ), &mi );
        RECT rcDesktop = mi.rcMonitor;

        SetMenu( g_hWnd, nullptr );
        if ( !cVisual.bAlwaysShowControls ) ShowWindow( g_hWndBar, SW_HIDE );
        SetWindowLongPtrW( g_hWnd, GWL_STYLE, GetWindowLongPtrW( g_hWnd, GWL_STYLE ) & ~WS_CAPTION & ~WS_THICKFRAME );
        SetWindowPos( g_hWnd, HWND_TOPMOST, rcDesktop.left, rcDesktop.top,
                      rcDesktop.right - rcDesktop.left, rcDesktop.bottom - rcDesktop.top,
                      SWP_NOACTIVATE | SWP_FRAMECHANGED );
        HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
        CheckMenuItem( hMenu, ID_VIEW_FULLSCREEN, MF_BYCOMMAND | MF_CHECKED );
    }
    else
    {
        SetMenu( g_hWnd, hMenu );
        if ( cView.GetControls() ) ShowWindow( g_hWndBar, SW_SHOWNA );
        SetWindowLongPtrW( g_hWnd, GWL_STYLE, GetWindowLongPtrW( g_hWnd, GWL_STYLE ) | WS_CAPTION | WS_THICKFRAME );
        SetWindowPos( g_hWnd, cView.GetOnTop() ? HWND_TOPMOST : HWND_NOTOPMOST, rcOld.left, rcOld.top,
                      rcOld.right - rcOld.left, rcOld.bottom - rcOld.top,
                      SWP_NOACTIVATE | SWP_FRAMECHANGED );
        HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
        CheckMenuItem( hMenu, ID_VIEW_FULLSCREEN, MF_BYCOMMAND | MF_UNCHECKED );
    }
}

VOID SetZoomMove( BOOL bZoomMove )
{
    CheckMenuItem( GetMainMenu(), ID_VIEW_MOVEANDZOOM, MF_BYCOMMAND | ( bZoomMove ? MF_CHECKED : MF_UNCHECKED ) );
}

VOID SetMute( BOOL bMute )
{
    HWND hWndToolbar = GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR );
    SendMessageW( hWndToolbar, TB_CHANGEBITMAP, ID_PLAY_MUTE, bMute ? 6 : 5 );
    CheckMenuItem( GetMainMenu(), ID_PLAY_MUTE, MF_BYCOMMAND | ( bMute ? MF_CHECKED : MF_UNCHECKED ) );
}

VOID SetSpeed( double dSpeed )
{
    HWND hWndToolbar = GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR );
    SendMessageW( GetDlgItem( hWndToolbar, IDC_SPEED ), TBM_SETPOS, TRUE, ( LONG )( 100 * dSpeed + .5 ) );
}

VOID SetNSpeed( double dNSpeed )
{
    HWND hWndToolbar = GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR );
    SendMessageW( GetDlgItem( hWndToolbar, IDC_NSPEED ), TBM_SETPOS, TRUE, NSpeedToSlider( dNSpeed ) );
}

VOID SetVolume( double dVolume )
{
    HWND hWndToolbar = GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR );
    SendMessageW( GetDlgItem( hWndToolbar, IDC_VOLUME ), TBM_SETPOS, TRUE, ( LONG )( 100 * dVolume + .5 ) );
}

VOID SetPosition( INT iPosition )
{
    PostMessageW( GetDlgItem( g_hWndBar, IDC_POSNCTRL ), TBM_SETPOS, 0, iPosition );
}

VOID SetMetronome( INT iMetronome )
{
    HWND hWndToolbar = GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR );
    SendMessageW( GetDlgItem( hWndToolbar, IDC_METRONOME ), CB_SETCURSEL, iMetronome, 0 );
}

VOID SetPlayable( BOOL bPlayable )
{
    SendMessageW( GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR ), TB_ENABLEBUTTON, ID_PLAY_PLAY, bPlayable );
}

VOID SetPlayMode( INT ePlayMode )
{
    HWND hWndToolbar = GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR );
    HMENU hMenu = GetMainMenu();
    BOOL bPlay = ( ePlayMode == PlayMode::Play );

    int iPlayButtons[] = { ID_PLAY_PLAY, ID_PLAY_PAUSE, ID_PLAY_STOP, ID_PLAY_SKIPFWD, ID_PLAY_SKIPBACK };
    for ( int id : iPlayButtons )
        SendMessageW( hWndToolbar, TB_ENABLEBUTTON, id, bPlay );

    SendMessageW( hWndToolbar, TB_PRESSBUTTON, ID_PLAY_PLAY, TRUE );
    SetZoomMove( FALSE );

    int iMenuItems[] = { ID_FILE_CLOSEFILE, ID_PLAY_PLAYPAUSE, ID_PLAY_STOP, ID_VIEW_MOVEANDZOOM, ID_PLAY_SKIPFWD, ID_PLAY_SKIPBACK };
    for ( int id : iMenuItems )
        EnableMenuItem( hMenu, id, MF_BYCOMMAND | ( bPlay ? MF_ENABLED : MF_GRAYED ) );

    EnableWindow( GetDlgItem( g_hWndBar, IDC_POSNCTRL ), bPlay );
}

VOID SetPlayPauseStop( BOOL bPlay, BOOL bPause, BOOL bStop )
{
    HWND hWndToolbar = GetDlgItem( g_hWndBar, IDC_TOPTOOLBAR );
    SendMessageW( hWndToolbar, TB_PRESSBUTTON, ID_PLAY_PLAY, bPlay );
    SendMessageW( hWndToolbar, TB_PRESSBUTTON, ID_PLAY_PAUSE, bPause );
    SendMessageW( hWndToolbar, TB_PRESSBUTTON, ID_PLAY_STOP, bStop );
}

// Starts the asynchronous load; MainScreen is created in FinishLoad (LoadingDialog.cpp)
BOOL PlayFile( const std::wstring &sFile )
{
    if ( IsLoading() )
    {
        MessageBoxW( g_hWnd, L"A file is already loading.", APPNAME, MB_OK | MB_ICONINFORMATION );
        return FALSE;
    }
    return StartLoad( sFile ) ? TRUE : FALSE;
}

VOID CheckActivity( BOOL bIsActive, POINT *ptNew, BOOL bToggleEnable )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    static bool bEnabled = true;
    static bool bWasActive = true;
    static bool bMouseHidden = false;
    static POINT ptOld;

    if ( !bEnabled && !bToggleEnable ) return;
    if ( bToggleEnable ) bEnabled = !bEnabled;

    bool bSamePt;
    if ( ptNew )
    {
        bSamePt = ( ptNew->x == ptOld.x && ptNew->y == ptOld.y );
        ptOld = *ptNew;
    }
    else
    {
        POINT pt;
        GetCursorPos( &pt );
        bSamePt = ( pt.x == ptOld.x && pt.y == ptOld.y );
        ptOld = pt;
    }

    if ( ( bIsActive && !ptNew ) || !bSamePt || !cView.GetFullScreen() )
    {
        bWasActive = true;
        if ( bMouseHidden ) bMouseHidden = ( ShowCursor( TRUE ) < 0 );
    }
    else if ( !bIsActive && GetFocus() == g_hWndGfx && ( !IsWindowVisible( g_hWndBar ) || cVisual.bAlwaysShowControls ) )
    {
        if ( bWasActive )
            bWasActive = false;
        else if ( !bMouseHidden )
            bMouseHidden = ( ShowCursor( FALSE ) < 0 );
    }
}
