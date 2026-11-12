#include "app/OnScreen.h"

#include <algorithm>
#include <vector>

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

    LRESULT CALLBACK Hook( int nCode, WPARAM wParam, LPARAM lParam )
    {
        if ( nCode == HC_ACTION )
        {
            const CWPRETSTRUCT &c = *reinterpret_cast< const CWPRETSTRUCT * >( lParam );
            switch ( c.message )
            {
                case WM_SHOWWINDOW:
                    if ( c.wParam ) OnScreen::Keep( c.hwnd );
                    break;

            }
        }
        return CallNextHookEx( nullptr, nCode, wParam, lParam );
    }
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
