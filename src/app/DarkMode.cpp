#include "app/DarkMode.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <string>

namespace
{
    int g_eTheme = DarkMode::ThemeLight;
    bool g_bOn = false;
    unsigned g_uGen = 0;
    HHOOK g_hHook = nullptr;

    const COLORREF cBack = RGB( 32, 32, 32 ), cField = RGB( 45, 45, 45 ), cText = RGB( 235, 235, 235 ), cDim = RGB( 125, 125, 125 ),
                   cEdge = RGB( 85, 85, 85 ), cHot = RGB( 62, 62, 62 ), cSelected = RGB( 52, 52, 52 );
    HBRUSH g_hBack = nullptr, g_hField = nullptr, g_hEdge = nullptr, g_hHot = nullptr, g_hSelected = nullptr;

    // uxtheme's unnamed exports for dark pop-up menus (Windows 10 1903 on: by ordinal, as other apps
    // use them; missing, the menus stay light)
    enum AppMode { AppDefault, AppAllowDark, AppForceDark, AppForceLight };
    int ( WINAPI *pSetPreferredAppMode )( int ) = nullptr;                // 135
    BOOL ( WINAPI *pAllowDarkModeForWindow )( HWND, BOOL ) = nullptr;     // 133
    void ( WINAPI *pFlushMenuThemes )() = nullptr;                        // 136

    std::wstring ClassOf( HWND h )
    {
        wchar_t s[64] = {};
        GetClassNameW( h, s, 64 );
        return s;
    }

    bool SystemDark()
    {
        DWORD dw = 1, cb = sizeof( dw );
        if ( RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                           RRF_RT_REG_DWORD, nullptr, &dw, &cb ) != ERROR_SUCCESS ) return false;
        return dw == 0;
    }

    void TitleBar( HWND h )
    {
        // DWMWA_USE_IMMERSIVE_DARK_MODE: 20, and 19 before Windows 10 20H1
        BOOL b = g_bOn;
        if ( FAILED( DwmSetWindowAttribute( h, 20, &b, sizeof( b ) ) ) ) DwmSetWindowAttribute( h, 19, &b, sizeof( b ) );
        if ( pAllowDarkModeForWindow ) pAllowDarkModeForWindow( h, b );
    }

    //-------------------------------------------------------------------------
    // Check boxes, radio buttons and group boxes: Windows draws their text black whatever the colours
    // asked for, so they are drawn here (the boxes and circles from the dark theme's own parts)
    //-------------------------------------------------------------------------

    void PaintButton( HWND h, HDC hdc )
    {
        RECT rc;
        GetClientRect( h, &rc );
        const LONG lStyle = GetWindowLongW( h, GWL_STYLE );
        const UINT uType = lStyle & BS_TYPEMASK;
        const bool bEnabled = IsWindowEnabled( h ) != FALSE;
        const LRESULT lUi = SendMessageW( h, WM_QUERYUISTATE, 0, 0 );
        const UINT uPrefix = ( lUi & UISF_HIDEACCEL ) ? DT_HIDEPREFIX : 0;
        wchar_t sText[512] = {};
        GetWindowTextW( h, sText, 512 );
        HGDIOBJ hOldFont = SelectObject( hdc, ( HFONT )SendMessageW( h, WM_GETFONT, 0, 0 ) );
        SetBkMode( hdc, TRANSPARENT );
        SetTextColor( hdc, bEnabled ? cText : cDim );
        TEXTMETRICW tm;
        GetTextMetricsW( hdc, &tm );

        if ( uType == BS_GROUPBOX )
        {
            // The frame through the middle of the caption, the caption over it
            RECT rcFrame = rc;
            rcFrame.top += tm.tmHeight / 2;
            FrameRect( hdc, &rcFrame, g_hEdge );
            if ( sText[0] )
            {
                RECT rcText = { rc.left + 8, rc.top, rc.right - 8, rc.top + tm.tmHeight };
                DrawTextW( hdc, sText, -1, &rcText, DT_SINGLELINE | DT_CALCRECT | uPrefix );
                RECT rcGap = rcText;
                InflateRect( &rcGap, 2, 0 );
                FillRect( hdc, &rcGap, g_hBack );
                DrawTextW( hdc, sText, -1, &rcText, DT_SINGLELINE | uPrefix );
            }
            SelectObject( hdc, hOldFont );
            return;
        }

        FillRect( hdc, &rc, g_hBack );
        const bool bRadio = uType == BS_RADIOBUTTON || uType == BS_AUTORADIOBUTTON;
        const int iPart = bRadio ? BP_RADIOBUTTON : BP_CHECKBOX;
        const LRESULT lCheck = SendMessageW( h, BM_GETCHECK, 0, 0 ), lState = SendMessageW( h, BM_GETSTATE, 0, 0 );
        // Each part's states: normal, hot, pressed, disabled, for unchecked, checked and mixed
        int iState = lCheck == BST_CHECKED ? CBS_CHECKEDNORMAL : lCheck == BST_INDETERMINATE && !bRadio ? CBS_MIXEDNORMAL : CBS_UNCHECKEDNORMAL;
        iState += !bEnabled ? 3 : ( lState & BST_PUSHED ) ? 2 : GetPropW( h, L"DarkModeHot" ) ? 1 : 0;
        HTHEME hTheme = OpenThemeData( h, L"Button" );
        SIZE szGlyph = { 13, 13 };
        if ( hTheme ) GetThemePartSize( hTheme, hdc, iPart, iState, nullptr, TS_DRAW, &szGlyph );
        RECT rcGlyph = { rc.left, rc.top + ( rc.bottom - rc.top - szGlyph.cy ) / 2, rc.left + szGlyph.cx, 0 };
        rcGlyph.bottom = rcGlyph.top + szGlyph.cy;
        if ( hTheme ) DrawThemeBackground( hTheme, hdc, iPart, iState, &rcGlyph, nullptr );
        else DrawFrameControl( hdc, &rcGlyph, DFC_BUTTON, ( bRadio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK ) | ( lCheck ? DFCS_CHECKED : 0 ) );
        if ( hTheme ) CloseThemeData( hTheme );

        // The text after it, centred down the button (several lines with BS_MULTILINE)
        RECT rcText = { rcGlyph.right + 4, rc.top, rc.right, rc.bottom };
        const UINT uFormat = uPrefix | ( ( lStyle & BS_MULTILINE ) ? DT_WORDBREAK : DT_SINGLELINE );
        RECT rcCalc = rcText;
        DrawTextW( hdc, sText, -1, &rcCalc, uFormat | DT_CALCRECT );
        OffsetRect( &rcCalc, 0, ( rcText.bottom - rcText.top - ( rcCalc.bottom - rcCalc.top ) ) / 2 );
        DrawTextW( hdc, sText, -1, &rcCalc, uFormat );
        if ( GetFocus() == h && !( lUi & UISF_HIDEFOCUS ) )
        {
            InflateRect( &rcCalc, 1, 1 );
            SetTextColor( hdc, cText );
            DrawFocusRect( hdc, &rcCalc );
        }
        SelectObject( hdc, hOldFont );
    }

    LRESULT CALLBACK ButtonProc( HWND h, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
    {
        if ( msg == WM_NCDESTROY ) RemoveWindowSubclass( h, ButtonProc, 0 );
        if ( !g_bOn ) return DefSubclassProc( h, msg, wParam, lParam );
        switch ( msg )
        {
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint( h, &ps );
                PaintButton( h, hdc );
                EndPaint( h, &ps );
                return 0;
            }
            case WM_PRINTCLIENT:
                PaintButton( h, ( HDC )wParam );
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_MOUSEMOVE:
                if ( !GetPropW( h, L"DarkModeHot" ) )
                {
                    TRACKMOUSEEVENT tme = { sizeof( tme ), TME_LEAVE, h, 0 };
                    TrackMouseEvent( &tme );
                    SetPropW( h, L"DarkModeHot", ( HANDLE )1 );
                    InvalidateRect( h, nullptr, FALSE );
                }
                break;
            case WM_MOUSELEAVE:
                RemovePropW( h, L"DarkModeHot" );
                InvalidateRect( h, nullptr, FALSE );
                break;
            // Windows draws these changes at once, light: drawn again over it
            case WM_ENABLE: case WM_SETTEXT: case BM_SETCHECK: case BM_SETSTATE: case BM_CLICK: case WM_SETFOCUS: case WM_KILLFOCUS:
            case WM_UPDATEUISTATE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_KEYDOWN: case WM_KEYUP:
            {
                LRESULT lr = DefSubclassProc( h, msg, wParam, lParam );
                InvalidateRect( h, nullptr, FALSE );
                return lr;
            }
            case WM_NCDESTROY:
                RemovePropW( h, L"DarkModeHot" );
                break;
        }
        return DefSubclassProc( h, msg, wParam, lParam );
    }

    // Any theme: a check box clicked keeps the focus, so the space bar would go on toggling it. After a
    // click the space bar is let go, until the focus leaves it (tabbed to, it toggles as usual)
    LRESULT CALLBACK ClickProc( HWND h, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
    {
        switch ( msg )
        {
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
                SetPropW( h, L"PFAviz.Clicked", ( HANDLE )1 );
                break;
            case WM_KILLFOCUS:
                RemovePropW( h, L"PFAviz.Clicked" );
                break;
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
                if ( wParam == VK_SPACE && GetPropW( h, L"PFAviz.Clicked" ) ) return 0;
                break;
            case WM_NCDESTROY:
                RemovePropW( h, L"PFAviz.Clicked" );
                RemoveWindowSubclass( h, ClickProc, 0 );
                break;
        }
        return DefSubclassProc( h, msg, wParam, lParam );
    }

    //-------------------------------------------------------------------------
    // Tabs (the property sheet's): the tab control has no dark theme
    //-------------------------------------------------------------------------

    void PaintTabs( HWND h, HDC hdc )
    {
        RECT rc;
        GetClientRect( h, &rc );
        FillRect( hdc, &rc, g_hBack );
        const int n = TabCtrl_GetItemCount( h ), iSel = TabCtrl_GetCurSel( h ), iHot = ( int )( INT_PTR )GetPropW( h, L"DarkModeHot" ) - 1;
        // The pages' frame, from under the tabs
        RECT rcItem = {};
        if ( n > 0 ) TabCtrl_GetItemRect( h, 0, &rcItem );
        RECT rcFrame = { rc.left, rcItem.bottom, rc.right, rc.bottom };
        FrameRect( hdc, &rcFrame, g_hEdge );

        HGDIOBJ hOldFont = SelectObject( hdc, ( HFONT )SendMessageW( h, WM_GETFONT, 0, 0 ) );
        SetBkMode( hdc, TRANSPARENT );
        SetTextColor( hdc, cText );
        const UINT uPrefix = ( SendMessageW( h, WM_QUERYUISTATE, 0, 0 ) & UISF_HIDEACCEL ) ? DT_HIDEPREFIX : 0;
        for ( int i = 0; i < n; i++ )
        {
            RECT r;
            TabCtrl_GetItemRect( h, i, &r );
            if ( i == iSel ) { r.top -= 2; r.left -= 2; r.right += 2; r.bottom += 1; }
            FillRect( hdc, &r, i == iSel ? g_hSelected : i == iHot ? g_hHot : g_hBack );
            // Its top and sides (the selected one open into its page)
            RECT rcLine = { r.left, r.top, r.right, r.top + 1 };
            FillRect( hdc, &rcLine, g_hEdge );
            rcLine = { r.left, r.top, r.left + 1, r.bottom };
            FillRect( hdc, &rcLine, g_hEdge );
            rcLine = { r.right - 1, r.top, r.right, r.bottom };
            FillRect( hdc, &rcLine, g_hEdge );
            if ( i == iSel )
            {
                rcLine = { r.left + 1, r.bottom - 1, r.right - 1, r.bottom };
                FillRect( hdc, &rcLine, g_hSelected );
            }
            wchar_t s[128] = {};
            TCITEMW ti = {};
            ti.mask = TCIF_TEXT;
            ti.pszText = s;
            ti.cchTextMax = 128;
            TabCtrl_GetItem( h, i, &ti );
            DrawTextW( hdc, s, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | uPrefix );
        }
        SelectObject( hdc, hOldFont );
    }

    LRESULT CALLBACK TabProc( HWND h, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
    {
        if ( msg == WM_NCDESTROY ) { RemovePropW( h, L"DarkModeHot" ); RemoveWindowSubclass( h, TabProc, 0 ); }
        if ( !g_bOn ) return DefSubclassProc( h, msg, wParam, lParam );
        switch ( msg )
        {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint( h, &ps );
                PaintTabs( h, hdc );
                EndPaint( h, &ps );
                return 0;
            }
            case WM_MOUSEMOVE:
            {
                TCHITTESTINFO hti = { { ( short )LOWORD( lParam ), ( short )HIWORD( lParam ) }, 0 };
                const INT_PTR iHot = TabCtrl_HitTest( h, &hti ) + 1;
                if ( iHot != ( INT_PTR )GetPropW( h, L"DarkModeHot" ) )
                {
                    SetPropW( h, L"DarkModeHot", ( HANDLE )iHot );
                    TRACKMOUSEEVENT tme = { sizeof( tme ), TME_LEAVE, h, 0 };
                    TrackMouseEvent( &tme );
                    InvalidateRect( h, nullptr, FALSE );
                }
                break;
            }
            case WM_MOUSELEAVE:
                RemovePropW( h, L"DarkModeHot" );
                InvalidateRect( h, nullptr, FALSE );
                break;
        }
        return DefSubclassProc( h, msg, wParam, lParam );
    }

    //-------------------------------------------------------------------------
    // Statics Windows draws in system colours (white rects, black and white frames, etched lines),
    // and disabled text, which it draws embossed
    //-------------------------------------------------------------------------

    void PaintStatic( HWND h, HDC hdc )
    {
        const LONG lStyle = GetWindowLongW( h, GWL_STYLE );
        const UINT uType = lStyle & SS_TYPEMASK;
        RECT rc;
        GetClientRect( h, &rc );
        HBRUSH hBlack = ( HBRUSH )GetStockObject( DC_BRUSH );
        SetDCBrushColor( hdc, RGB( 16, 16, 16 ) );
        switch ( uType )
        {
            case SS_WHITERECT: case SS_GRAYRECT: FillRect( hdc, &rc, g_hBack ); return;
            case SS_BLACKRECT: FillRect( hdc, &rc, hBlack ); return;
            case SS_BLACKFRAME: FrameRect( hdc, &rc, hBlack ); return;
            case SS_WHITEFRAME: case SS_GRAYFRAME: case SS_ETCHEDFRAME: FrameRect( hdc, &rc, g_hEdge ); return;
            case SS_ETCHEDHORZ: rc.bottom = rc.top + 1; FillRect( hdc, &rc, g_hEdge ); return;
            case SS_ETCHEDVERT: rc.right = rc.left + 1; FillRect( hdc, &rc, g_hEdge ); return;
            case SS_LEFT: case SS_CENTER: case SS_RIGHT: case SS_LEFTNOWORDWRAP: case SS_SIMPLE:
            {
                wchar_t s[512] = {};
                GetWindowTextW( h, s, 512 );
                FillRect( hdc, &rc, g_hBack );
                HGDIOBJ hOldFont = SelectObject( hdc, ( HFONT )SendMessageW( h, WM_GETFONT, 0, 0 ) );
                SetBkMode( hdc, TRANSPARENT );
                SetTextColor( hdc, cDim );
                UINT uFormat = uType == SS_CENTER ? DT_CENTER : uType == SS_RIGHT ? DT_RIGHT : DT_LEFT;
                uFormat |= ( uType == SS_LEFTNOWORDWRAP || uType == SS_SIMPLE ) ? DT_SINGLELINE : DT_WORDBREAK;
                if ( lStyle & SS_NOPREFIX ) uFormat |= DT_NOPREFIX;
                DrawTextW( hdc, s, -1, &rc, uFormat );
                SelectObject( hdc, hOldFont );
                return;
            }
        }
    }

    LRESULT CALLBACK StaticProc( HWND h, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
    {
        if ( msg == WM_NCDESTROY ) RemoveWindowSubclass( h, StaticProc, 0 );
        if ( !g_bOn ) return DefSubclassProc( h, msg, wParam, lParam );
        switch ( msg )
        {
            case WM_PAINT:
            {
                // (enabled text, icons and bitmaps: Windows' own drawing)
                const UINT uType = GetWindowLongW( h, GWL_STYLE ) & SS_TYPEMASK;
                const bool bText = uType == SS_LEFT || uType == SS_CENTER || uType == SS_RIGHT || uType == SS_LEFTNOWORDWRAP || uType == SS_SIMPLE;
                if ( bText ? IsWindowEnabled( h ) != FALSE : !( ( uType >= SS_BLACKRECT && uType <= SS_WHITEFRAME ) || ( uType >= SS_ETCHEDHORZ && uType <= SS_ETCHEDFRAME ) ) ) break;
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint( h, &ps );
                PaintStatic( h, hdc );
                EndPaint( h, &ps );
                return 0;
            }
            case WM_ENABLE:
            {
                LRESULT lr = DefSubclassProc( h, msg, wParam, lParam );
                InvalidateRect( h, nullptr, TRUE );
                return lr;
            }
        }
        return DefSubclassProc( h, msg, wParam, lParam );
    }

    // The playback bar (a rebar): its own drawing is light whatever the theme
    LRESULT CALLBACK RebarProc( HWND h, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
    {
        if ( msg == WM_NCDESTROY ) RemoveWindowSubclass( h, RebarProc, 0 );
        if ( !g_bOn ) return DefSubclassProc( h, msg, wParam, lParam );
        switch ( msg )
        {
            case WM_ERASEBKGND: case WM_PRINTCLIENT:
            {
                RECT rc;
                GetClientRect( h, &rc );
                FillRect( ( HDC )wParam, &rc, g_hBack );
                return 1;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint( h, &ps );
                FillRect( hdc, &ps.rcPaint, g_hBack );
                EndPaint( h, &ps );
                return 0;
            }
            // Its toolbar's buttons: hot, checked and pressed in greys, not the theme's light blue
            case WM_NOTIFY:
            {
                NMTBCUSTOMDRAW *p = ( NMTBCUSTOMDRAW * )lParam;
                if ( p->nmcd.hdr.code != NM_CUSTOMDRAW || ClassOf( p->nmcd.hdr.hwndFrom ) != L"ToolbarWindow32" ) break;
                if ( p->nmcd.dwDrawStage == CDDS_PREPAINT ) return CDRF_NOTIFYITEMDRAW;
                if ( p->nmcd.dwDrawStage != CDDS_ITEMPREPAINT ) break;
                const UINT uState = p->nmcd.uItemState;
                const bool bPressed = ( uState & CDIS_SELECTED ) != 0, bChecked = ( uState & CDIS_CHECKED ) != 0, bHot = ( uState & CDIS_HOT ) != 0;
                if ( bPressed || bChecked || bHot )
                {
                    SetDCBrushColor( p->nmcd.hdc, bPressed ? RGB( 78, 78, 78 ) : bChecked ? RGB( 58, 58, 58 ) : cHot );
                    FillRect( p->nmcd.hdc, &p->nmcd.rc, ( HBRUSH )GetStockObject( DC_BRUSH ) );
                    SetDCBrushColor( p->nmcd.hdc, bChecked || bPressed ? RGB( 110, 110, 110 ) : cEdge );
                    FrameRect( p->nmcd.hdc, &p->nmcd.rc, ( HBRUSH )GetStockObject( DC_BRUSH ) );
                }
                return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOOFFSET;
            }
        }
        return DefSubclassProc( h, msg, wParam, lParam );
    }

    //-------------------------------------------------------------------------
    // Dialogs
    //-------------------------------------------------------------------------

    void ApplyOne( HWND h );

    // A shell dialog (the file dialogs) has its own dark mode, and is left to it
    bool IsShellDialog( HWND h )
    {
        bool bShell = false;
        EnumChildWindows( h, []( HWND c, LPARAM l ) -> BOOL {
            const std::wstring s = ClassOf( c );
            if ( s == L"DUIViewWndClassName" || s == L"DirectUIHWND" || s == L"SHELLDLL_DefView" ) { *( bool * )l = true; return FALSE; }
            return TRUE;
        }, ( LPARAM )&bShell );
        return bShell;
    }

    LRESULT CALLBACK DialogProc( HWND h, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
    {
        switch ( msg )
        {
            case WM_NCDESTROY:
                RemoveWindowSubclass( h, DialogProc, 0 );
                break;
            case WM_INITDIALOG:
            {
                LRESULT lr = DefSubclassProc( h, msg, wParam, lParam );
                if ( IsShellDialog( h ) ) RemoveWindowSubclass( h, DialogProc, 0 );
                else DarkMode::Apply( h );
                return lr;
            }
            case WM_ERASEBKGND:
                if ( !g_bOn ) break;
                {
                    RECT rc;
                    GetClientRect( h, &rc );
                    FillRect( ( HDC )wParam, &rc, g_hBack );
                }
                return 1;
            case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            {
                LRESULT lr;
                if ( DarkMode::CtlColor( msg, wParam, lr, ( HWND )lParam ) ) return lr;
                break;
            }
        }
        return DefSubclassProc( h, msg, wParam, lParam );
    }

    // The app's dialogs (made from its own templates) darkened as they are made
    LRESULT CALLBACK CbtProc( int iCode, WPARAM wParam, LPARAM lParam )
    {
        if ( iCode == HCBT_CREATEWND )
        {
            HWND h = ( HWND )wParam;
            const CBT_CREATEWNDW *p = ( const CBT_CREATEWNDW * )lParam;
            if ( p->lpcs->hInstance == GetModuleHandleW( nullptr ) && ClassOf( h ) == L"#32770" )
                SetWindowSubclass( h, DialogProc, 0, 0 );
        }
        return CallNextHookEx( g_hHook, iCode, wParam, lParam );
    }

    void ApplyOne( HWND h )
    {
        const std::wstring s = ClassOf( h );
        const wchar_t *sExplorer = g_bOn ? L"DarkMode_Explorer" : nullptr;
        if ( s == L"Button" )
        {
            const UINT uType = GetWindowLongW( h, GWL_STYLE ) & BS_TYPEMASK;
            if ( uType == BS_OWNERDRAW ) return;
            SetWindowTheme( h, sExplorer, nullptr );
            if ( uType == BS_CHECKBOX || uType == BS_AUTOCHECKBOX || uType == BS_3STATE || uType == BS_AUTO3STATE ||
                 uType == BS_RADIOBUTTON || uType == BS_AUTORADIOBUTTON || uType == BS_GROUPBOX )
                SetWindowSubclass( h, ButtonProc, 0, 0 );
            if ( uType == BS_CHECKBOX || uType == BS_AUTOCHECKBOX || uType == BS_3STATE || uType == BS_AUTO3STATE )
                SetWindowSubclass( h, ClickProc, 0, 0 );
        }
        else if ( s == L"Edit" ) SetWindowTheme( h, g_bOn ? L"DarkMode_CFD" : nullptr, nullptr );
        else if ( s == L"ComboBox" )
        {
            SetWindowTheme( h, g_bOn ? L"DarkMode_CFD" : nullptr, nullptr );
            COMBOBOXINFO cbi = { sizeof( cbi ) };
            if ( GetComboBoxInfo( h, &cbi ) && cbi.hwndList ) SetWindowTheme( cbi.hwndList, sExplorer, nullptr );
        }
        else if ( s == L"ListBox" || s == L"ScrollBar" || s == L"msctls_updown32" ) SetWindowTheme( h, sExplorer, nullptr );
        else if ( s == L"SysListView32" )
        {
            SetWindowTheme( h, sExplorer, nullptr );
            SetWindowTheme( ListView_GetHeader( h ), g_bOn ? L"DarkMode_ItemsView" : nullptr, nullptr );
            ListView_SetBkColor( h, g_bOn ? cField : GetSysColor( COLOR_WINDOW ) );
            ListView_SetTextBkColor( h, g_bOn ? cField : GetSysColor( COLOR_WINDOW ) );
            ListView_SetTextColor( h, g_bOn ? cText : GetSysColor( COLOR_WINDOWTEXT ) );
        }
        else if ( s == L"SysTabControl32" ) SetWindowSubclass( h, TabProc, 0, 0 );
        else if ( s == L"ReBarWindow32" ) SetWindowSubclass( h, RebarProc, 0, 0 );
        // (their theme as it was, told again: they drop the background they kept from the old one)
        else if ( s == L"msctls_trackbar32" ) SetWindowTheme( h, nullptr, nullptr );
        else if ( s == L"Static" ) SetWindowSubclass( h, StaticProc, 0, 0 );
        else if ( s == L"#32770" && ( GetWindowLongW( h, GWL_STYLE ) & WS_CHILD ) )
        {
            // A property page: its own background, not the theme's light one
            SetWindowSubclass( h, DialogProc, 0, 0 );
            EnableThemeDialogTexture( h, g_bOn ? ETDT_DISABLE : ETDT_ENABLETAB );
        }
    }

    //-------------------------------------------------------------------------
    // The menu bar: Windows draws it light whatever the theme. It asks its window to draw it with
    // these undocumented messages (as other apps answer them)
    //-------------------------------------------------------------------------

    const UINT WM_UAHDRAWMENU = 0x0091, WM_UAHDRAWMENUITEM = 0x0092;
    struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
    union UAHMENUITEMMETRICS { struct { DWORD cx, cy; } rgsizeBar[2]; struct { DWORD cx, cy; } rgsizePopup[4]; };
    struct UAHMENUPOPUPMETRICS { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; };
    struct UAHMENUITEM { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; };
    struct UAHDRAWMENUITEM { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; };

    // The line Windows draws between the menu bar and the client area, light
    void MenuLine( HWND h )
    {
        MENUBARINFO mbi = { sizeof( mbi ) };
        if ( !GetMenuBarInfo( h, OBJID_MENU, 0, &mbi ) ) return;
        RECT rcClient, rcWindow;
        GetClientRect( h, &rcClient );
        MapWindowPoints( h, nullptr, ( POINT * )&rcClient, 2 );
        GetWindowRect( h, &rcWindow );
        OffsetRect( &rcClient, -rcWindow.left, -rcWindow.top );
        RECT rcLine = { rcClient.left, rcClient.top - 1, rcClient.right, rcClient.top };
        HDC hdc = GetWindowDC( h );
        FillRect( hdc, &rcLine, g_hBack );
        ReleaseDC( h, hdc );
    }
}

void DarkMode::Install()
{
    g_hBack = CreateSolidBrush( cBack );
    g_hField = CreateSolidBrush( cField );
    g_hEdge = CreateSolidBrush( cEdge );
    g_hHot = CreateSolidBrush( cHot );
    g_hSelected = CreateSolidBrush( cSelected );
    // The pop-up menus' exports, from Windows 10 1903 (build 18362)
    using GetVersionFn = void ( WINAPI * )( DWORD *, DWORD *, DWORD * );
    DWORD dwMajor = 0, dwMinor = 0, dwBuild = 0;
    if ( auto pGetVersion = ( GetVersionFn )( void * )GetProcAddress( GetModuleHandleW( L"ntdll.dll" ), "RtlGetNtVersionNumbers" ) )
        pGetVersion( &dwMajor, &dwMinor, &dwBuild );
    dwBuild &= 0xFFFF;
    HMODULE hUx = LoadLibraryExW( L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 );
    if ( hUx && dwMajor >= 10 && dwBuild >= 18362 )
    {
        pSetPreferredAppMode = ( int ( WINAPI * )( int ) )( void * )GetProcAddress( hUx, MAKEINTRESOURCEA( 135 ) );
        pAllowDarkModeForWindow = ( BOOL ( WINAPI * )( HWND, BOOL ) )( void * )GetProcAddress( hUx, MAKEINTRESOURCEA( 133 ) );
        pFlushMenuThemes = ( void ( WINAPI * )() )( void * )GetProcAddress( hUx, MAKEINTRESOURCEA( 136 ) );
    }
    g_hHook = SetWindowsHookExW( WH_CBT, CbtProc, nullptr, GetCurrentThreadId() );
}

void DarkMode::SetTheme( int eTheme )
{
    g_eTheme = eTheme;
    const bool bOn = eTheme == ThemeDark || ( eTheme == ThemeSystem && SystemDark() );
    if ( pSetPreferredAppMode ) pSetPreferredAppMode( bOn ? AppForceDark : AppForceLight );
    if ( pFlushMenuThemes ) pFlushMenuThemes();
    if ( bOn == g_bOn ) return;
    g_bOn = bOn;
    g_uGen++;
    // The open windows themed and drawn again (their frames too: the title bars)
    EnumThreadWindows( GetCurrentThreadId(), []( HWND h, LPARAM ) -> BOOL {
        Apply( h );
        SetWindowPos( h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
        RedrawWindow( h, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN );
        return TRUE;
    }, 0 );
}

bool DarkMode::On() { return g_bOn; }
unsigned DarkMode::Generation() { return g_uGen; }

void DarkMode::OnSettingChange( LPARAM lParam )
{
    if ( g_eTheme == ThemeSystem && lParam && lstrcmpW( ( LPCWSTR )lParam, L"ImmersiveColorSet" ) == 0 ) SetTheme( ThemeSystem );
}

COLORREF DarkMode::Back() { return cBack; }
COLORREF DarkMode::Field() { return cField; }
COLORREF DarkMode::Text() { return cText; }
COLORREF DarkMode::Edge() { return cEdge; }
HBRUSH DarkMode::BackBrush() { return g_hBack; }
HBRUSH DarkMode::FieldBrush() { return g_hField; }

HIMAGELIST DarkMode::Images( HINSTANCE hInstance, UINT uId, int cx, COLORREF cMask )
{
    HBITMAP hbm = ( HBITMAP )LoadImageW( hInstance, MAKEINTRESOURCEW( uId ), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION );
    if ( !hbm ) return nullptr;
    DIBSECTION ds = {};
    GetObjectW( hbm, sizeof( ds ), &ds );
    const int iBytes = ds.dsBm.bmBitsPixel / 8;
    if ( g_bOn && ds.dsBm.bmBits && iBytes >= 3 )
        for ( int y = 0; y < ds.dsBm.bmHeight; y++ )
        {
            BYTE *p = ( BYTE * )ds.dsBm.bmBits + y * ds.dsBm.bmWidthBytes;
            for ( int x = 0; x < ds.dsBm.bmWidth; x++, p += iBytes )
                if ( RGB( p[2], p[1], p[0] ) != cMask && p[0] == p[1] && p[1] == p[2] )
                    p[0] = p[1] = p[2] = ( BYTE )( 230 - p[0] * 200 / 255 );
        }
    HIMAGELIST hIml = ImageList_Create( cx, ds.dsBm.bmHeight, ILC_COLOR24 | ILC_MASK, ds.dsBm.bmWidth / cx, 0 );
    ImageList_AddMasked( hIml, hbm, cMask );
    DeleteObject( hbm );
    return hIml;
}

void DarkMode::Apply( HWND hWnd )
{
    if ( !( GetWindowLongW( hWnd, GWL_STYLE ) & WS_CHILD ) ) TitleBar( hWnd );
    ApplyOne( hWnd );
    EnumChildWindows( hWnd, []( HWND h, LPARAM ) -> BOOL { ApplyOne( h ); return TRUE; }, 0 );
}

void DarkMode::Dialog( HWND hDlg )
{
    SetWindowSubclass( hDlg, DialogProc, 0, 0 );
    Apply( hDlg );
}

bool DarkMode::CtlColor( UINT msg, WPARAM wParam, LRESULT &lr, HWND hCtl )
{
    if ( !g_bOn ) return false;
    HDC hdc = ( HDC )wParam;
    switch ( msg )
    {
        case WM_CTLCOLORDLG:
            lr = ( LRESULT )g_hBack;
            return true;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
            SetTextColor( hdc, hCtl && !IsWindowEnabled( hCtl ) ? cDim : cText );
            SetBkColor( hdc, cBack );
            lr = ( LRESULT )g_hBack;
            return true;
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            SetTextColor( hdc, cText );
            SetBkColor( hdc, cField );
            lr = ( LRESULT )g_hField;
            return true;
    }
    return false;
}

bool DarkMode::MenuBar( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &lr )
{
    if ( !g_bOn ) return false;
    switch ( msg )
    {
        case WM_UAHDRAWMENU:
        {
            const UAHMENU *p = ( const UAHMENU * )lParam;
            MENUBARINFO mbi = { sizeof( mbi ) };
            GetMenuBarInfo( hWnd, OBJID_MENU, 0, &mbi );
            RECT rcWindow;
            GetWindowRect( hWnd, &rcWindow );
            RECT rc = mbi.rcBar;
            OffsetRect( &rc, -rcWindow.left, -rcWindow.top );
            FillRect( p->hdc, &rc, g_hBack );
            lr = 0;
            return true;
        }
        case WM_UAHDRAWMENUITEM:
        {
            UAHDRAWMENUITEM *p = ( UAHDRAWMENUITEM * )lParam;
            wchar_t s[128] = {};
            MENUITEMINFOW mii = { sizeof( mii ) };
            mii.fMask = MIIM_STRING;
            mii.dwTypeData = s;
            mii.cch = 127;
            GetMenuItemInfoW( p->um.hmenu, p->umi.iPosition, TRUE, &mii );
            const UINT uState = p->dis.itemState;
            FillRect( p->um.hdc, &p->dis.rcItem, ( uState & ( ODS_HOTLIGHT | ODS_SELECTED ) ) ? g_hHot : g_hBack );
            SetBkMode( p->um.hdc, TRANSPARENT );
            SetTextColor( p->um.hdc, ( uState & ( ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED ) ) ? cDim : cText );
            DrawTextW( p->um.hdc, s, -1, &p->dis.rcItem, DT_CENTER | DT_SINGLELINE | DT_VCENTER | ( ( uState & ODS_NOACCEL ) ? DT_HIDEPREFIX : 0 ) );
            lr = 0;
            return true;
        }
        case WM_NCPAINT: case WM_NCACTIVATE:
            lr = DefWindowProcW( hWnd, msg, wParam, lParam );
            MenuLine( hWnd );
            return true;
    }
    return false;
}
