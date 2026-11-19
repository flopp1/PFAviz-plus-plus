#include "app/OnScreen.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "core/Config.h"

namespace
{
    // The screens' work areas (the desktop less the taskbar)
    std::vector< RECT > WorkAreas()
    {
        std::vector< RECT > v;
        EnumDisplayMonitors( nullptr, nullptr, []( HMONITOR hMon, HDC, LPRECT, LPARAM l ) -> BOOL {
            MONITORINFO mi = { sizeof( mi ) };
            if ( GetMonitorInfoW( hMon, &mi ) ) reinterpret_cast< std::vector< RECT > * >( l )->push_back( mi.rcWork );
            return TRUE;
        }, ( LPARAM )&v );
        return v;
    }

    // A window this applies to: shown, with a title bar, neither minimized nor maximized, not a child
    bool Applies( HWND hWnd )
    {
        if ( !hWnd || !IsWindow( hWnd ) ) return false;
        const LONG_PTR lStyle = GetWindowLongPtrW( hWnd, GWL_STYLE );
        return ( lStyle & WS_CAPTION ) == WS_CAPTION && !( lStyle & WS_CHILD ) && !IsIconic( hWnd ) && !IsZoomed( hWnd );
    }

    // The windows whose places are kept (OnScreen::Track), by the title each had when first shown
    std::map< HWND, std::wstring > g_mTracked;

    // The app's own dialogs (made from its templates), and the ones it tracks itself
    bool Tracks( HWND hWnd )
    {
        if ( g_mTracked.count( hWnd ) ) return true;
        wchar_t sClass[16] = {};
        GetClassNameW( hWnd, sClass, 16 );
        return wcscmp( sClass, L"#32770" ) == 0 && ( HINSTANCE )GetWindowLongPtrW( hWnd, GWLP_HINSTANCE ) == GetModuleHandleW( nullptr );
    }

    bool Resizable( HWND hWnd ) { return ( GetWindowLongPtrW( hWnd, GWL_STYLE ) & WS_THICKFRAME ) != 0; }

    // Where it is now, kept for next time (as it would open there), with its size when bSize (a resizable
    // window resized by hand; else the size kept before stays)
    void Remember( HWND hWnd, bool bSize )
    {
        auto it = g_mTracked.find( hWnd );
        if ( it == g_mTracked.end() || it->second.empty() || !Applies( hWnd ) ) return;
        RECT r;
        if ( !GetWindowRect( hWnd, &r ) ) return;
        const POINT pt = OnScreen::Fixed( r );
        ViewSettings &cView = Config::GetConfig().GetViewSettings();
        ViewSettings::Place place;
        cView.GetPlace( it->second, place );
        place.x = pt.x;
        place.y = pt.y;
        if ( bSize && Resizable( hWnd ) ) { place.cx = r.right - r.left; place.cy = r.bottom - r.top; }
        cView.SetPlace( it->second, place );
    }

    // Puts it where the place kept for this title is (and at its size, if one is kept and it can be resized),
    // then on a screen
    void Restore( HWND hWnd, const std::wstring &sTitle )
    {
        ViewSettings::Place place;
        if ( !sTitle.empty() && Config::GetConfig().GetViewSettings().GetPlace( sTitle, place ) )
        {
            const bool bSize = place.cx > 0 && place.cy > 0 && Resizable( hWnd );
            SetWindowPos( hWnd, nullptr, place.x, place.y, place.cx, place.cy, ( bSize ? 0 : SWP_NOSIZE ) | SWP_NOZORDER | SWP_NOACTIVATE );
        }
        OnScreen::Keep( hWnd );
    }

    LRESULT CALLBACK Hook( int nCode, WPARAM wParam, LPARAM lParam )
    {
        if ( nCode == HC_ACTION )
        {
            const CWPRETSTRUCT &c = *reinterpret_cast< const CWPRETSTRUCT * >( lParam );
            switch ( c.message )
            {
                case WM_SHOWWINDOW:
                    if ( c.wParam )
                    {
                        // First shown: where it was last (then kept on a screen)
                        if ( Applies( c.hwnd ) && Tracks( c.hwnd ) && g_mTracked[c.hwnd].empty() )
                        {
                            wchar_t sTitle[256] = {};
                            GetWindowTextW( c.hwnd, sTitle, 256 );
                            g_mTracked[c.hwnd] = sTitle;
                            Restore( c.hwnd, sTitle );
                        }
                        else OnScreen::Keep( c.hwnd );
                    }
                    else Remember( c.hwnd, false );
                    break;
                case WM_SETTEXT:
                {
                    // A shown window given a new title (the loading window becoming its statistics) is another
                    // window from then on, put where the new title's place is (the old title's is as it was last
                    // moved by hand: the moves that come with the change are not kept for it)
                    auto it = g_mTracked.find( c.hwnd );
                    if ( it == g_mTracked.end() || it->second.empty() || !c.lResult || !c.lParam ) break;
                    const std::wstring sNew = reinterpret_cast< const wchar_t * >( c.lParam );
                    if ( sNew.empty() || sNew == it->second ) break;
                    it->second = sNew;
                    if ( Applies( c.hwnd ) ) Restore( c.hwnd, sNew );
                    break;
                }
                case WM_EXITSIZEMOVE:
                    // (moved or resized by hand: kept at once)
                    Remember( c.hwnd, true );
                    break;
                case WM_DESTROY:
                    Remember( c.hwnd, false );
                    g_mTracked.erase( c.hwnd );
                    break;
            }
        }
        return CallNextHookEx( nullptr, nCode, wParam, lParam );
    }
}

void OnScreen::Track( HWND hWnd )
{
    g_mTracked.emplace( hWnd, std::wstring() );
}

void OnScreen::Install()
{
    static thread_local HHOOK s_hHook = nullptr;
    if ( !s_hHook ) s_hHook = SetWindowsHookExW( WH_CALLWNDPROCRET, Hook, nullptr, GetCurrentThreadId() );
}

bool OnScreen::Keep( HWND hWnd )
{
    if ( !Applies( hWnd ) ) return false;
    RECT r;
    if ( !GetWindowRect( hWnd, &r ) ) return false;
    const POINT pt = Fixed( r );
    if ( pt.x == r.left && pt.y == r.top ) return false;
    SetWindowPos( hWnd, nullptr, pt.x, pt.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    return true;
}

POINT OnScreen::Fixed( const RECT &r )
{
    const POINT ptSame = { r.left, r.top };
    const int cx = r.right - r.left, cy = r.bottom - r.top;
    if ( cx <= 0 || cy <= 0 ) return ptSame;
    const std::vector< RECT > vWork = WorkAreas();
    if ( vWork.empty() ) return ptSame;

    // The title bar: the top of the window, its frame and caption
    const int iCaption = std::min( cy, GetSystemMetrics( SM_CYCAPTION ) + GetSystemMetrics( SM_CYSIZEFRAME ) + GetSystemMetrics( SM_CXPADDEDBORDER ) );
    const LONG lTop = r.top, lBottom = r.top + iCaption;
    // How much of its width is on screens that hold it from top to bottom
    long lSeen = 0;
    for ( const RECT &w : vWork )
        if ( lTop >= w.top && lBottom <= w.bottom ) lSeen += std::max< long >( 0, std::min( r.right, w.right ) - std::max( r.left, w.left ) );
    if ( lSeen >= std::min< long >( cx, std::max< long >( 200, cx / 2 ) ) ) return ptSame;

    // Onto the nearest screen: wholly when it fits, else its top left there
    HMONITOR hMon = MonitorFromRect( &r, MONITOR_DEFAULTTONEAREST );
    MONITORINFO mi = { sizeof( mi ) };
    if ( !GetMonitorInfoW( hMon, &mi ) ) return ptSame;
    const RECT &w = mi.rcWork;
    POINT pt;
    pt.x = cx <= w.right - w.left ? std::clamp< int >( r.left, w.left, w.right - cx ) : w.left;
    pt.y = cy <= w.bottom - w.top ? std::clamp< int >( r.top, w.top, w.bottom - cy ) : w.top;
    return pt;
}
