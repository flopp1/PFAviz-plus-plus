// Hover tips for a dialog's controls: the details their short labels leave out (a disabled control shows
// none). The tip window belongs to the dialog and goes with it.
#pragma once

#include <windows.h>
#include <commctrl.h>
#include <initializer_list>
#include <utility>

inline VOID AddTips( HWND hDlg, std::initializer_list< std::pair< int, const wchar_t * > > tips )
{
    HWND hTip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, hDlg, nullptr, GetModuleHandleW( nullptr ), nullptr );
    if ( !hTip ) return;
    // (wrapped at about 60 characters, shown while the mouse stays)
    const UINT uDpi = GetDpiForWindow( hDlg ) ? GetDpiForWindow( hDlg ) : 96;
    SendMessageW( hTip, TTM_SETMAXTIPWIDTH, 0, MulDiv( 420, (int)uDpi, 96 ) );
    SendMessageW( hTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000 );
    for ( const auto &t : tips )
    {
        HWND hCtl = GetDlgItem( hDlg, t.first );
        if ( !hCtl ) continue;
        TTTOOLINFOW ti = {};
        ti.cbSize = sizeof( ti );
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hDlg;
        ti.uId = (UINT_PTR)hCtl;
        ti.lpszText = const_cast< wchar_t * >( t.second );
        SendMessageW( hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti );
    }
}
