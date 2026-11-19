// The Preferences sheet, from the original ConfigProcs.cpp property pages: eight tabs by task (Notes &
// keys, Stats, Effects, Audio, Interface, Performance, Video rendering, Misc), each setting the parts of
// the settings it shows (the Audio tab's subtabs choose the output, and Pre-Render's engine, at once), with the rarer settings in dialogs of their own
// (Advanced..., Settings..., Each part..., Bloom..., Hit line...) and the details in hover tips. Each page
// has Restore Defaults in the same place, and Apply lights only while a page differs from what it last
// applied. A page cut short to fit the screen scrolls.
#include "app/Preferences.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <colordlg.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/DarkMode.h"
#include "app/MainWindow.h"
#include "app/OnScreen.h"
#include "app/VideoExport.h"
#include "audio/MidiOutput.h"
#include "app/Tips.h"
#include "audio/RenderAhead.h"
#include "audio/RenderBackends.h"
#include "core/Config.h"
#include "core/Ini.h"
#include "core/ThreadPool.h"
#include "core/Util.h"
#include "app/LoadingDialog.h"
#include "midi/Notes.h"
#include "resource.h"
#include "song/PfaLag.h"
#include "song/Song.h"

namespace
{
    //-------------------------------------------------------------------------
    // Apply lights only while a page differs from what it showed when it was opened or last applied: its
    // state is each control's (check boxes, fields, lists, colour swatches) and the settings it keeps
    // outside them (g_aPageExtra), as text
    //-------------------------------------------------------------------------

    const int MaxPages = 12;
    typedef std::wstring ( *PageExtraFn )();
    PageExtraFn g_aPageExtra[MaxPages] = {};
    std::map< HWND, std::wstring > g_mPageBase;

    int PageIndex( HWND hWnd )
    {
        return (int)( INT_PTR )GetPropW( hWnd, L"PFAviz.Page" ) - 1;
    }

    std::wstring PageState( HWND hWnd )
    {
        std::wstring s;
        std::vector< wchar_t > buf( 4096 );
        for ( HWND h = GetWindow( hWnd, GW_CHILD ); h; h = GetWindow( h, GW_HWNDNEXT ) )
        {
            const int iId = GetDlgCtrlID( h );
            wchar_t sClass[32] = {};
            GetClassNameW( h, sClass, 32 );
            std::wstring t;
            if ( _wcsicmp( sClass, L"Button" ) == 0 )
            {
                const LONG lType = GetWindowLongW( h, GWL_STYLE ) & BS_TYPEMASK;
                if ( lType == BS_OWNERDRAW ) t = std::to_wstring( ( long long )GetWindowLongPtrW( h, GWLP_USERDATA ) );
                else if ( lType == BS_CHECKBOX || lType == BS_AUTOCHECKBOX || lType == BS_3STATE || lType == BS_AUTO3STATE ||
                          lType == BS_RADIOBUTTON || lType == BS_AUTORADIOBUTTON )
                    t = std::to_wstring( ( long long )SendMessageW( h, BM_GETCHECK, 0, 0 ) );
            }
            else if ( _wcsicmp( sClass, L"Edit" ) == 0 )
            {
                GetWindowTextW( h, buf.data(), (int)buf.size() );
                t = buf.data();
            }
            else if ( _wcsicmp( sClass, L"ComboBox" ) == 0 )
            {
                // (not the saved palettes' and profiles' lists: which one is chosen is not a setting)
                if ( iId == IDC_PALETTES || iId == IDC_FX_PROFILES ) continue;
                GetWindowTextW( h, buf.data(), (int)buf.size() );
                t = std::to_wstring( ( long long )SendMessageW( h, CB_GETCURSEL, 0, 0 ) ) + L"," + buf.data();
            }
            else if ( _wcsicmp( sClass, L"ListBox" ) == 0 )
            {
                // (the MIDI output device applies as it is chosen, as the Audio tab's subtabs do)
                if ( iId == IDC_MIDIOUT ) continue;
                t = std::to_wstring( ( long long )SendMessageW( h, LB_GETCURSEL, 0, 0 ) );
                for ( int i = 0, n = (int)SendMessageW( h, LB_GETCOUNT, 0, 0 ); i < n; i++ )
                {
                    const int iLen = (int)SendMessageW( h, LB_GETTEXTLEN, i, 0 );
                    if ( iLen < 0 || iLen >= (int)buf.size() ) continue;
                    SendMessageW( h, LB_GETTEXT, i, ( LPARAM )buf.data() );
                    t += L"|" + std::wstring( buf.data() );
                }
            }
            else continue;
            s += std::to_wstring( iId ) + L":" + t + L"\n";
        }
        const int i = PageIndex( hWnd );
        if ( i >= 0 && i < MaxPages && g_aPageExtra[i] ) s += g_aPageExtra[i]();
        return s;
    }

    // The page as it is now is what Apply leaves (opened, or applied)
    VOID Baseline( HWND hWnd )
    {
        g_mPageBase[hWnd] = PageState( hWnd );
        SendMessageW( GetParent( hWnd ), PSM_UNCHANGED, ( WPARAM )hWnd, 0 );
    }

    // Whether the page now differs from that (before its first baseline: still being filled in)
    bool PageDiffers( HWND hWnd )
    {
        auto it = g_mPageBase.find( hWnd );
        return it != g_mPageBase.end() && PageState( hWnd ) != it->second;
    }

    VOID Changed( HWND hWnd )
    {
        if ( g_mPageBase.find( hWnd ) == g_mPageBase.end() ) return;
        SendMessageW( GetParent( hWnd ), PageDiffers( hWnd ) ? PSM_CHANGED : PSM_UNCHANGED, ( WPARAM )hWnd, 0 );
    }

    // Settings as text, for the pages' states
    std::wstring Widen( const std::string &s ) { return std::wstring( s.begin(), s.end() ); }

    // A control's WM_COMMAND that is the user changing a setting: a check box or radio button
    // clicked, a list's choice, text typed. Not text the code puts in a field (only the focused one
    // is being typed in; an up-down's arrows: PageProc), a focus change, or a push button: one that
    // changes something calls Changed itself, one that opens a dialog only when it was changed there
    bool UserChange( WPARAM wParam, LPARAM lParam )
    {
        HWND hCtl = ( HWND )lParam;
        wchar_t sClass[32];
        if ( !hCtl || !GetClassNameW( hCtl, sClass, 32 ) ) return false;
        const WORD wCode = HIWORD( wParam );
        const bool bTyping = GetFocus() == hCtl || IsChild( hCtl, GetFocus() );
        // (the codes are each class's own: CBN_SELCHANGE and LBN_SELCHANGE are both 1)
        if ( _wcsicmp( sClass, L"Button" ) == 0 )
        {
            LONG lType = GetWindowLongW( hCtl, GWL_STYLE ) & BS_TYPEMASK;
            return wCode == BN_CLICKED && ( lType == BS_CHECKBOX || lType == BS_AUTOCHECKBOX || lType == BS_3STATE ||
                                            lType == BS_AUTO3STATE || lType == BS_RADIOBUTTON || lType == BS_AUTORADIOBUTTON );
        }
        if ( _wcsicmp( sClass, L"ComboBox" ) == 0 ) return wCode == CBN_SELCHANGE || ( wCode == CBN_EDITCHANGE && bTyping );
        if ( _wcsicmp( sClass, L"ListBox" ) == 0 ) return wCode == LBN_SELCHANGE;
        if ( _wcsicmp( sClass, L"Edit" ) == 0 ) return wCode == EN_CHANGE && bTyping;
        return false;
    }

    // A check box's state
    bool Checked( HWND hWnd, int iId ) { return IsDlgButtonChecked( hWnd, iId ) == BST_CHECKED; }
    VOID Check( HWND hWnd, int iId, bool b ) { CheckDlgButton( hWnd, iId, b ? BST_CHECKED : BST_UNCHECKED ); }

    // The page brought to the front (OK applies every page, so a value one cannot take may be on a page
    // not shown)
    VOID ShowPage( HWND hWnd )
    {
        HWND hSheet = GetParent( hWnd );
        if ( PropSheet_GetCurrentPageHwnd( hSheet ) == hWnd ) return;
        const int i = PropSheet_HwndToIndex( hSheet, hWnd );
        if ( i >= 0 ) PropSheet_SetCurSel( hSheet, nullptr, i );
    }

    // A value a page cannot take: its page shown, said, its field focused, and the sheet kept open
    BOOL Invalid( HWND hWnd, int iId, const wchar_t *sMsg )
    {
        ShowPage( hWnd );
        MessageBoxW( hWnd, sMsg, L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, iId ), TRUE );
        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
        return TRUE;
    }

    // A page's settings applied
    BOOL Applied( HWND hWnd )
    {
        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_NOERROR );
        return TRUE;
    }

    //-------------------------------------------------------------------------
    // Visual settings: what the Notes & keys, Stats and Misc pages share
    //-------------------------------------------------------------------------

    const int aKeyModeIds[5] = { IDC_SHOWALLKEYS, IDC_SHOWSONGKEYS, IDC_SHOWCUSTOMKEYS, IDC_SHOWALL128KEYS, IDC_SHOWALL256KEYS }; // by KeysShown

    VOID EnableCustomKeys( HWND hWnd, BOOL bEnable )
    {
        EnableWindow( GetDlgItem( hWnd, IDC_FIRSTKEY ), bEnable );
        EnableWindow( GetDlgItem( hWnd, IDC_THROUGH ), bEnable );
        EnableWindow( GetDlgItem( hWnd, IDC_LASTKEY ), bEnable );
    }

    // The stats box lines chosen in the Statistics dialog, and their order, saved with the Stats page
    struct StatsChoice
    {
        unsigned uStats = 0;
        unsigned char aOrder[VisualSettings::StatCount] = {};
        bool bAdvanced = false;         // (the dialog lists the advanced lines too)
    };

    // The lines the Statistics dialog lists and the box can show: every one but the retired one, the
    // advanced ones only with them on
    unsigned ListedStats( bool bAdvanced ) { return VisualSettings::AllStats & ( bAdvanced ? ~0u : ~VisualSettings::AdvancedStats ); }
    StatsChoice g_Stats;

    bool SameGraph( const VisualSettings::GraphSettings &a, const VisualSettings::GraphSettings &b )
    {
        const int n = VisualSettings::GraphSettings::SeriesCount;
        return std::equal( a.abShow, a.abShow + n, b.abShow ) && std::equal( a.abLine, a.abLine + n, b.abLine ) &&
               std::equal( a.abRight, a.abRight + n, b.abRight ) && std::equal( a.aColor, a.aColor + n, b.aColor ) &&
               std::equal( a.aiPlace, a.aiPlace + n, b.aiPlace ) && a.bZero == b.bZero && a.iCorner == b.iCorner && a.iSpanMs == b.iSpanMs && a.iCX == b.iCX && a.iCY == b.iCY &&
               a.iFontPx == b.iFontPx && a.iGrowMs == b.iGrowMs && a.iShrinkMs == b.iShrinkMs &&
               a.bScale == b.bScale && a.bMaxLine == b.bMaxLine && a.uMaxColor == b.uMaxColor;
    }

    // The graphs' settings, chosen in the Graphs dialog and saved with the Stats page
    VisualSettings::GraphSettings g_Graph;

    // The Clean flags (visual/Scene.h), set all on or off by the Clean drawing switch or each in its
    // Advanced dialog, and saved with the Notes & keys page. The switch shows partly checked for some of them.
    unsigned g_uClean = 7;

    VOID ShowClean( HWND hWnd )
    {
        CheckDlgButton( hWnd, IDC_CLEAN, g_uClean == 7 ? BST_CHECKED : g_uClean ? BST_INDETERMINATE : BST_UNCHECKED );
    }

    INT_PTR CALLBACK CleanAdvProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        unsigned *pClean = ( unsigned * )GetWindowLongPtrW( hWnd, DWLP_USER );
        switch ( msg )
        {
            case WM_INITDIALOG:
                SetWindowLongPtrW( hWnd, DWLP_USER, lParam );
                pClean = ( unsigned * )lParam;
                for ( int i = 0; i < 3; i++ )
                    CheckDlgButton( hWnd, IDC_CLEAN_MINHEIGHT + i, ( *pClean >> i ) & 1 ? BST_CHECKED : BST_UNCHECKED );
                return TRUE;
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDOK:
                        *pClean = 0;
                        for ( int i = 0; i < 3; i++ )
                            if ( IsDlgButtonChecked( hWnd, IDC_CLEAN_MINHEIGHT + i ) == BST_CHECKED ) *pClean |= 1u << i;
                        EndDialog( hWnd, IDOK );
                        return TRUE;
                    case IDCANCEL:
                        EndDialog( hWnd, IDCANCEL );
                        return TRUE;
                }
                break;
        }
        return FALSE;
    }

    bool PickColor( HWND hWnd, COLORREF &cr );
    VOID DrawSwatch( LPDRAWITEMSTRUCT pdis );

    INT_PTR CALLBACK GraphProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        typedef VisualSettings::GraphSettings GS;
        GS *p = ( GS * )GetWindowLongPtrW( hWnd, GWLP_USERDATA );
        static int s_aiPlace[GS::SeriesCount];     // the places shown (before a change: to swap with)
        // The controls as settings, and back
        auto Read = [&]( GS &g ) {
            for ( int i = 0; i < GS::SeriesCount; i++ )
            {
                g.abShow[i] = IsDlgButtonChecked( hWnd, IDC_GRAPH_SHOW0 + i ) == BST_CHECKED;
                g.abLine[i] = SendDlgItemMessageW( hWnd, IDC_GRAPH_STYLE0 + i, CB_GETCURSEL, 0, 0 ) == 1;
                g.abRight[i] = SendDlgItemMessageW( hWnd, IDC_GRAPH_ALIGN0 + i, CB_GETCURSEL, 0, 0 ) == 1;
                g.aiPlace[i] = (int)SendDlgItemMessageW( hWnd, IDC_GRAPH_PLACE0 + i, CB_GETCURSEL, 0, 0 );
                g.aColor[i] = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_GRAPH_COLOR0 + i ), GWLP_USERDATA );
            }
        };
        auto ShowStyles = [&]( const GS &g ) {
            for ( int i = 0; i < GS::SeriesCount; i++ )
            {
                SendDlgItemMessageW( hWnd, IDC_GRAPH_STYLE0 + i, CB_SETCURSEL, g.abLine[i], 0 );
                SendDlgItemMessageW( hWnd, IDC_GRAPH_PLACE0 + i, CB_SETCURSEL, g.aiPlace[i], 0 );
                EnableWindow( GetDlgItem( hWnd, IDC_GRAPH_STYLE0 + i ), g.abShow[i] );
                EnableWindow( GetDlgItem( hWnd, IDC_GRAPH_ALIGN0 + i ), g.abShow[i] );
            }
        };
        switch ( msg )
        {
            case WM_DRAWITEM:
            {
                const UINT uId = ( ( LPDRAWITEMSTRUCT )lParam )->CtlID;
                if ( ( uId < IDC_GRAPH_COLOR0 || uId >= IDC_GRAPH_COLOR0 + GS::SeriesCount ) && uId != IDC_GRAPH_MAXCOLOR ) return FALSE;
                DrawSwatch( ( LPDRAWITEMSTRUCT )lParam );
                return TRUE;
            }
            case WM_INITDIALOG:
            {
                p = ( GS * )lParam;
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, lParam );
                for ( int i = 0; i < GS::SeriesCount; i++ ) s_aiPlace[i] = p->aiPlace[i];
                for ( int i = 0; i < GS::SeriesCount; i++ )
                {
                    CheckDlgButton( hWnd, IDC_GRAPH_SHOW0 + i, p->abShow[i] ? BST_CHECKED : BST_UNCHECKED );
                    SetWindowLongPtrW( GetDlgItem( hWnd, IDC_GRAPH_COLOR0 + i ), GWLP_USERDATA, p->aColor[i] );
                    for ( const wchar_t *s : { L"Filled", L"A line" } ) SendDlgItemMessageW( hWnd, IDC_GRAPH_STYLE0 + i, CB_ADDSTRING, 0, ( LPARAM )s );
                    for ( const wchar_t *s : { L"The left", L"The right" } ) SendDlgItemMessageW( hWnd, IDC_GRAPH_ALIGN0 + i, CB_ADDSTRING, 0, ( LPARAM )s );
                    SendDlgItemMessageW( hWnd, IDC_GRAPH_ALIGN0 + i, CB_SETCURSEL, p->abRight[i], 0 );
                    for ( const wchar_t *s : { L"1st (top)", L"2nd", L"3rd" } ) SendDlgItemMessageW( hWnd, IDC_GRAPH_PLACE0 + i, CB_ADDSTRING, 0, ( LPARAM )s );
                }
                ShowStyles( *p );
                for ( const wchar_t *s : { L"At zero", L"At the lowest value shown" } )
                    SendDlgItemMessageW( hWnd, IDC_GRAPH_BOTTOM, CB_ADDSTRING, 0, ( LPARAM )s );
                SendDlgItemMessageW( hWnd, IDC_GRAPH_BOTTOM, CB_SETCURSEL, p->bZero ? 0 : 1, 0 );
                // By VisualSettings::StatsCorner bits
                for ( const wchar_t *s : { L"Top right", L"Top left", L"Bottom right", L"Bottom left" } )
                    SendDlgItemMessageW( hWnd, IDC_GRAPH_CORNER, CB_ADDSTRING, 0, ( LPARAM )s );
                SendDlgItemMessageW( hWnd, IDC_GRAPH_CORNER, CB_SETCURSEL, p->iCorner, 0 );
                SetDlgItemInt( hWnd, IDC_GRAPH_SPAN, p->iSpanMs, FALSE );
                SetDlgItemInt( hWnd, IDC_GRAPH_CX, p->iCX, FALSE );
                SetDlgItemInt( hWnd, IDC_GRAPH_CY, p->iCY, FALSE );
                SetDlgItemInt( hWnd, IDC_GRAPH_FONT, p->iFontPx, FALSE );
                SetDlgItemInt( hWnd, IDC_GRAPH_GROW, p->iGrowMs, FALSE );
                SetDlgItemInt( hWnd, IDC_GRAPH_SHRINK, p->iShrinkMs, FALSE );
                CheckDlgButton( hWnd, IDC_GRAPH_SCALE, p->bScale ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_GRAPH_MAXLINE, p->bMaxLine ? BST_CHECKED : BST_UNCHECKED );
                SetWindowLongPtrW( GetDlgItem( hWnd, IDC_GRAPH_MAXCOLOR ), GWLP_USERDATA, p->uMaxColor );
                return TRUE;
            }
            case WM_COMMAND:
            {
                const int iId = LOWORD( wParam );
                if ( ( iId >= IDC_GRAPH_COLOR0 && iId < IDC_GRAPH_COLOR0 + GS::SeriesCount ) || iId == IDC_GRAPH_MAXCOLOR )
                {
                    if ( HIWORD( wParam ) != BN_CLICKED ) return TRUE;
                    HWND hWndBtn = ( HWND )lParam;
                    COLORREF cr = ( COLORREF )GetWindowLongPtrW( hWndBtn, GWLP_USERDATA );
                    if ( !PickColor( hWnd, cr ) ) return TRUE;
                    SetWindowLongPtrW( hWndBtn, GWLP_USERDATA, cr );
                    InvalidateRect( hWndBtn, nullptr, FALSE );
                    return TRUE;
                }
                const bool bStyle = iId >= IDC_GRAPH_STYLE0 && iId < IDC_GRAPH_STYLE0 + GS::SeriesCount && HIWORD( wParam ) == CBN_SELCHANGE;
                const bool bShow = iId >= IDC_GRAPH_SHOW0 && iId < IDC_GRAPH_SHOW0 + GS::SeriesCount;
                const bool bPlace = iId >= IDC_GRAPH_PLACE0 && iId < IDC_GRAPH_PLACE0 + GS::SeriesCount && HIWORD( wParam ) == CBN_SELCHANGE;
                if ( bStyle || bShow || bPlace )
                {
                    GS g = *p;
                    Read( g );
                    // (a place just chosen: the one that had it takes this one's old place)
                    if ( bPlace )
                    {
                        const int iSet = iId - IDC_GRAPH_PLACE0;
                        for ( int i = 0; i < GS::SeriesCount; i++ )
                            if ( i != iSet && g.aiPlace[i] == g.aiPlace[iSet] ) g.aiPlace[i] = s_aiPlace[iSet];
                    }
                    for ( int i = 0; i < GS::SeriesCount; i++ ) s_aiPlace[i] = g.aiPlace[i];
                    g.Fix();
                    ShowStyles( g );
                    return TRUE;
                }
                if ( iId == IDOK )
                {
                    // (numbers out of range are brought into it)
                    auto Num = [&]( int iId, int iMin, int iMax, int iOld ) {
                        BOOL bOk;
                        UINT u = GetDlgItemInt( hWnd, iId, &bOk, FALSE );
                        return bOk ? std::clamp( (int)std::min( u, 10000000u ), iMin, iMax ) : iOld;
                    };
                    Read( *p );
                    p->bZero = SendDlgItemMessageW( hWnd, IDC_GRAPH_BOTTOM, CB_GETCURSEL, 0, 0 ) != 1;
                    p->iCorner = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_GRAPH_CORNER, CB_GETCURSEL, 0, 0 ) );
                    p->iSpanMs = Num( IDC_GRAPH_SPAN, 1, 600000, p->iSpanMs );     // (1 ms: the counts' bin)
                    p->iCX = Num( IDC_GRAPH_CX, 60, 3840, p->iCX );
                    p->iCY = Num( IDC_GRAPH_CY, 30, 2160, p->iCY );
                    p->iFontPx = Num( IDC_GRAPH_FONT, 6, 200, p->iFontPx );
                    p->iGrowMs = Num( IDC_GRAPH_GROW, 0, 10000, p->iGrowMs );
                    p->iShrinkMs = Num( IDC_GRAPH_SHRINK, 0, 10000, p->iShrinkMs );
                    p->bScale = IsDlgButtonChecked( hWnd, IDC_GRAPH_SCALE ) == BST_CHECKED;
                    p->bMaxLine = IsDlgButtonChecked( hWnd, IDC_GRAPH_MAXLINE ) == BST_CHECKED;
                    p->uMaxColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_GRAPH_MAXCOLOR ), GWLP_USERDATA );
                    p->Fix();
                    EndDialog( hWnd, IDOK );
                }
                else if ( iId == IDCANCEL ) EndDialog( hWnd, IDCANCEL );
                return TRUE;
            }
        }
        return FALSE;
    }

    // The track colors being edited (the count is its edit box): a grid of every colour the palette can
    // hold, PaletteCols to a row, the used ones filled
    unsigned g_aPalette[VisualSettings::MaxColors];
    std::vector< VisualSettings::Palette > g_vPalettes;     // the saved ones, as edited (kept on Apply)

    VOID ShowPalettes( HWND hWnd, int iSelect )
    {
        HWND hCombo = GetDlgItem( hWnd, IDC_PALETTES );
        SendMessageW( hCombo, CB_RESETCONTENT, 0, 0 );
        for ( const VisualSettings::Palette &p : g_vPalettes ) SendMessageW( hCombo, CB_ADDSTRING, 0, ( LPARAM )p.sName.c_str() );
        SendMessageW( hCombo, CB_SETCURSEL, iSelect, 0 );
        EnableWindow( GetDlgItem( hWnd, IDC_PALETTES ), !g_vPalettes.empty() );
        EnableWindow( GetDlgItem( hWnd, IDC_PALETTEDELETE ), iSelect >= 0 );
    }

    // The palette editor's colours and count
    VOID ShowColors( HWND hWnd, const unsigned *pColors, int iCount )
    {
        iCount = std::clamp( iCount, 1, VisualSettings::MaxColors );
        std::copy( pColors, pColors + iCount, g_aPalette );
        SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETPOS32, 0, iCount );
        InvalidateRect( GetDlgItem( hWnd, IDC_PALETTE ), nullptr, FALSE );
    }

    // The name dialog asks for a palette's, or with this set, an effects profile's
    bool g_bNameProfile = false;

    INT_PTR CALLBACK PaletteNameProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
                if ( g_bNameProfile )
                {
                    SetWindowTextW( hWnd, L"Save Effects Profile" );
                    SetDlgItemTextW( hWnd, IDC_PALETTENAMETEXT, L"Name (a saved profile of the same name is replaced):" );
                }
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, lParam );
                SetDlgItemTextW( hWnd, IDC_PALETTENAME, ( ( std::wstring * )lParam )->c_str() );
                SendDlgItemMessageW( hWnd, IDC_PALETTENAME, EM_SETSEL, 0, -1 );
                return TRUE;
            case WM_COMMAND:
                if ( LOWORD( wParam ) == IDOK )
                {
                    wchar_t s[256] = L"";
                    GetDlgItemTextW( hWnd, IDC_PALETTENAME, s, 256 );
                    std::wstring sName( s );
                    sName.erase( 0, sName.find_first_not_of( L' ' ) );
                    sName.erase( sName.find_last_not_of( L' ' ) + 1 );
                    if ( sName.empty() ) { MessageBeep( MB_ICONEXCLAMATION ); return TRUE; }
                    *( std::wstring * )GetWindowLongPtrW( hWnd, GWLP_USERDATA ) = sName;
                    EndDialog( hWnd, IDOK );
                }
                else if ( LOWORD( wParam ) == IDCANCEL ) EndDialog( hWnd, IDCANCEL );
                return TRUE;
        }
        return FALSE;
    }
    const int PaletteCols = 32, PaletteRows = VisualSettings::MaxColors / PaletteCols;
    int PaletteCount( HWND hWnd )
    {
        BOOL bOk = FALSE;
        int n = (int)GetDlgItemInt( hWnd, IDC_COLORCOUNT, &bOk, FALSE );
        return bOk ? std::clamp( n, 1, VisualSettings::MaxColors ) : 1;
    }
    RECT PaletteCell( const RECT &rc, int i )
    {
        int w = rc.right - rc.left, h = rc.bottom - rc.top, c = i % PaletteCols, r = i / PaletteCols;
        return { rc.left + w * c / PaletteCols, rc.top + h * r / PaletteRows, rc.left + w * ( c + 1 ) / PaletteCols, rc.top + h * ( r + 1 ) / PaletteRows };
    }

    // The stats box's size: set, its height by the font's size, or the original's (156 wide, its height by
    // the lines shown, unscaled: shown, greyed, in the size's boxes); the font's size or the original's,
    // one at a time. The height's box always shows the height the box is drawn at (by the font's size: as
    // MainScreen::StatsLayout makes it, its lines and padding from the font's)
    VOID ShowStatsSize( HWND hWnd )
    {
        const bool bFont = IsDlgButtonChecked( hWnd, IDC_STATSFONTSET ) == BST_CHECKED;
        const bool bPfa = IsDlgButtonChecked( hWnd, IDC_STATSPFASIZE ) == BST_CHECKED;
        const int iLines = std::max( 1, __builtin_popcount( g_Stats.uStats & ListedStats( IsDlgButtonChecked( hWnd, IDC_STATS_ADVANCED ) == BST_CHECKED ) ) );
        if ( bPfa )
        {
            SetDlgItemInt( hWnd, IDC_STATSSIZE, 156, FALSE );
            SetDlgItemInt( hWnd, IDC_STATSHEIGHT, (UINT)( 6 + 16 * iLines ), FALSE );
            CheckDlgButton( hWnd, IDC_STATSSCALED, BST_UNCHECKED );
        }
        else if ( bFont )
        {
            BOOL bOk = FALSE;
            const UINT uPx = GetDlgItemInt( hWnd, IDC_STATSFONTPX, &bOk, FALSE );
            if ( bOk && uPx >= (UINT)VisualSettings::StatsFontMin && uPx <= (UINT)VisualSettings::StatsFontMax )
            {
                const double k = uPx / 15.0;
                const int iLineCY = std::max( 1, (int)( k * 16.0 + 1e-9 ) ), iPadY = std::max( 1, (int)std::lround( 3.0 * k ) );
                SetDlgItemInt( hWnd, IDC_STATSHEIGHT, (UINT)( iPadY * 2 + iLineCY * iLines ), FALSE );
            }
        }
        for ( int iId : { IDC_STATSSIZE, IDC_STATSSCALED } ) EnableWindow( GetDlgItem( hWnd, iId ), !bPfa );
        EnableWindow( GetDlgItem( hWnd, IDC_STATSHEIGHT ), !bFont && !bPfa );
        EnableWindow( GetDlgItem( hWnd, IDC_STATSFONTPX ), bFont );
    }

    VOID ShowStatsCount( HWND hWnd )
    {
        wchar_t buf[64];
        const unsigned uListed = ListedStats( IsDlgButtonChecked( hWnd, IDC_STATS_ADVANCED ) == BST_CHECKED );
        int iShown = __builtin_popcount( g_Stats.uStats & uListed );
        if ( !iShown ) swprintf( buf, 64, L"Hidden (no lines)" );
        else swprintf( buf, 64, L"%d of %d lines", iShown, __builtin_popcount( uListed ) );
        SetDlgItemTextW( hWnd, IDC_STATSCOUNT, buf );
    }

    // Statistics...: a checked list of the lines in their order (moved with the buttons): every one but
    // the advanced ones unless they are on
    struct StatsDialog
    {
        StatsChoice *pOut;
        StatsChoice c;
        bool bFilling = false;
        std::vector< int > vPos;        // the list's rows: their lines' places in c.aOrder
        // Resizing: each control's place at the start, the client's size then, and the smallest window
        std::vector< std::pair< HWND, RECT > > vPlaces;
        SIZE szClient = {}, szMin = {};

        HWND List( HWND hWnd ) const { return GetDlgItem( hWnd, IDC_STATS_LIST ); }

        // The columns share the list's width (the line's name three fifths)
        void FitColumns( HWND hWnd ) const
        {
            HWND hList = List( hWnd );
            RECT rc;
            GetClientRect( hList, &rc );
            const int iWidth = rc.right - GetSystemMetrics( SM_CXVSCROLL );
            ListView_SetColumnWidth( hList, 0, iWidth * 3 / 5 );
            ListView_SetColumnWidth( hList, 1, iWidth - iWidth * 3 / 5 );
        }

        // The list takes the room gained; what is right of it moves right, what is under it (OK and
        // Cancel) moves down too
        void Layout( HWND hWnd, int cx, int cy ) const
        {
            const int dx = cx - szClient.cx, dy = cy - szClient.cy;
            HWND hList = List( hWnd );
            RECT rcList = {};
            for ( const auto &p : vPlaces ) if ( p.first == hList ) rcList = p.second;
            HDWP hdwp = BeginDeferWindowPos( (int)vPlaces.size() );
            for ( const auto &p : vPlaces )
            {
                RECT r = p.second;
                if ( p.first == hList ) { r.right += dx; r.bottom += dy; }
                else if ( r.top >= rcList.bottom ) OffsetRect( &r, dx, dy );
                else if ( r.left >= rcList.right ) OffsetRect( &r, dx, 0 );
                else continue;
                if ( hdwp ) hdwp = DeferWindowPos( hdwp, p.first, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE );
            }
            if ( hdwp ) EndDeferWindowPos( hdwp );
            FitColumns( hWnd );
            InvalidateRect( hWnd, nullptr, TRUE );
        }

        // The list from c, with item iSel selected
        void Fill( HWND hWnd, int iSel )
        {
            HWND hList = List( hWnd );
            bFilling = true;
            SendMessageW( hList, WM_SETREDRAW, FALSE, 0 );
            ListView_DeleteAllItems( hList );
            vPos.clear();
            const unsigned uListed = ListedStats( c.bAdvanced );
            for ( int i = 0; i < VisualSettings::StatCount; i++ )
                if ( ( uListed >> c.aOrder[i] ) & 1 ) vPos.push_back( i );
            for ( int i = 0; i < (int)vPos.size(); i++ )
            {
                int iStat = c.aOrder[vPos[i]];
                LVITEMW lvi = {};
                lvi.mask = LVIF_TEXT | LVIF_PARAM;
                lvi.iItem = i;
                lvi.pszText = const_cast< LPWSTR >( VisualSettings::StatNames[iStat] );
                lvi.lParam = iStat;
                ListView_InsertItem( hList, &lvi );
                ListView_SetItemText( hList, i, 1, const_cast< LPWSTR >( VisualSettings::SectionNames[VisualSettings::StatSections[iStat]] ) );
                ListView_SetCheckState( hList, i, ( c.uStats >> iStat ) & 1 );
            }
            if ( iSel >= 0 )
            {
                ListView_SetItemState( hList, iSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED );
                ListView_EnsureVisible( hList, iSel, FALSE );
            }
            SendMessageW( hList, WM_SETREDRAW, TRUE, 0 );
            bFilling = false;
        }

        void Move( HWND hWnd, int iDelta )
        {
            int i = ListView_GetNextItem( List( hWnd ), -1, LVNI_SELECTED );
            int j = i + iDelta;
            if ( i < 0 || j < 0 || j >= (int)vPos.size() ) return;
            std::swap( c.aOrder[vPos[i]], c.aOrder[vPos[j]] );
            Fill( hWnd, j );
        }
    };

    INT_PTR CALLBACK StatsProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        StatsDialog *pDlg = reinterpret_cast< StatsDialog * >( GetWindowLongPtrW( hWnd, GWLP_USERDATA ) );
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                pDlg = new StatsDialog;
                pDlg->pOut = reinterpret_cast< StatsChoice * >( lParam );
                pDlg->c = *pDlg->pOut;
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, ( LONG_PTR )pDlg );
                HWND hList = pDlg->List( hWnd );
                ListView_SetExtendedListViewStyle( hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT );
                RECT rc;
                GetClientRect( hList, &rc );
                int iScroll = GetSystemMetrics( SM_CXVSCROLL );
                LVCOLUMNW col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = const_cast< LPWSTR >( L"Line" );
                col.cx = ( rc.right - iScroll ) * 3 / 5;
                ListView_InsertColumn( hList, 0, &col );
                col.pszText = const_cast< LPWSTR >( L"Section" );
                col.cx = rc.right - iScroll - col.cx;
                ListView_InsertColumn( hList, 1, &col );
                pDlg->Fill( hWnd, 0 );
                AddTips( hWnd, { { IDC_STATS_LIST, L"Notes lines are counted as a song loads (open it again after turning them on). The synth "
                                                   L"lines need playback; voices and synth CPU need KDMAPI. More lines: Stats > Advanced lines." } } );
                // Its places, for resizing (it starts at its smallest)
                RECT rcClient, rcWnd;
                GetClientRect( hWnd, &rcClient );
                GetWindowRect( hWnd, &rcWnd );
                pDlg->szClient = { rcClient.right, rcClient.bottom };
                pDlg->szMin = { rcWnd.right - rcWnd.left, rcWnd.bottom - rcWnd.top };
                for ( HWND h = GetWindow( hWnd, GW_CHILD ); h; h = GetWindow( h, GW_HWNDNEXT ) )
                {
                    RECT r;
                    GetWindowRect( h, &r );
                    MapWindowPoints( nullptr, hWnd, ( LPPOINT )&r, 2 );
                    pDlg->vPlaces.push_back( { h, r } );
                }
                return TRUE;
            }
            case WM_SIZE:
                if ( pDlg && !pDlg->vPlaces.empty() && wParam != SIZE_MINIMIZED ) pDlg->Layout( hWnd, LOWORD( lParam ), HIWORD( lParam ) );
                return TRUE;
            case WM_GETMINMAXINFO:
                if ( pDlg && pDlg->szMin.cx ) ( ( MINMAXINFO * )lParam )->ptMinTrackSize = { pDlg->szMin.cx, pDlg->szMin.cy };
                return TRUE;
            case WM_NOTIFY:
            {
                LPNMHDR pnm = ( LPNMHDR )lParam;
                if ( !pDlg || pnm->idFrom != IDC_STATS_LIST || pnm->code != LVN_ITEMCHANGED || pDlg->bFilling ) break;
                // A checkbox clicked (its state image changed)
                LPNMLISTVIEW pnlv = ( LPNMLISTVIEW )lParam;
                if ( !( pnlv->uChanged & LVIF_STATE ) || ( ( pnlv->uNewState ^ pnlv->uOldState ) & LVIS_STATEIMAGEMASK ) == 0 ) break;
                if ( pnlv->iItem < 0 || pnlv->iItem >= (int)pDlg->vPos.size() ) break;
                int iStat = pDlg->c.aOrder[pDlg->vPos[pnlv->iItem]];
                if ( ListView_GetCheckState( pnm->hwndFrom, pnlv->iItem ) ) pDlg->c.uStats |= 1u << iStat;
                else pDlg->c.uStats &= ~( 1u << iStat );
                return TRUE;
            }
            case WM_COMMAND:
            {
                int iId = LOWORD( wParam );
                switch ( iId )
                {
                    case IDC_STATS_UP: pDlg->Move( hWnd, -1 ); return TRUE;
                    case IDC_STATS_DOWN: pDlg->Move( hWnd, 1 ); return TRUE;
                    case IDC_STATS_RESETORDER:
                        std::copy( VisualSettings::DefaultStatOrder, VisualSettings::DefaultStatOrder + VisualSettings::StatCount, pDlg->c.aOrder );
                        pDlg->Fill( hWnd, 0 );
                        return TRUE;
                    case IDOK:
                        *pDlg->pOut = pDlg->c;
                        EndDialog( hWnd, IDOK );
                        return TRUE;
                    case IDCANCEL:
                        EndDialog( hWnd, IDCANCEL );
                        return TRUE;
                }
                break;
            }
            case WM_DESTROY:
                delete pDlg;
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, 0 );
                break;
        }
        return FALSE;
    }

    // What the benchmark found for this computer (run when the simulation is on: it takes a moment)
    VOID SetPfaBenchText( HWND hWnd, bool bRun )
    {
        static bool s_bRan = false;
        wchar_t buf[160];
        if ( !bRun && !s_bRan ) swprintf( buf, 160, L"%% of this computer's" );
        else
        {
            s_bRan = true;
            const PfaLag::Bench &b = PfaLag::RunBench();
            PfaLag::Cpu cpu;
            PfaLag::ApplyBench( cpu, b );
            if ( b.bReference ) swprintf( buf, 160, L"%% of this computer's (the CPU it was measured on)" );
            else
            {
                // (how fast this one did the walks in L3, and the other work, against the reference)
                swprintf( buf, 160, L"%% of this computer's (measured %.2fx a Ryzen 5 5500)", 2.0 / ( cpu.aMem[2] + cpu.dCore ) );
            }
        }
        SetDlgItemTextW( hWnd, IDC_PFABENCH, buf );
    }

    // The main window's size when the page opened, or when it was last applied (applied again only
    // when it is changed)
    SIZE g_szWindow = {};

    VOID SetWindowSizeEdits( HWND hWnd )
    {
        g_szWindow = MainWindowSize();
        SIZE szMax = MainWindowMaxSize();
        SetDlgItemInt( hWnd, IDC_WINWIDTH, (UINT)g_szWindow.cx, FALSE );
        SetDlgItemInt( hWnd, IDC_WINHEIGHT, (UINT)g_szWindow.cy, FALSE );
        bool bFullScreen = Config::GetConfig().GetViewSettings().GetFullScreen();
        wchar_t buf[96];
        if ( bFullScreen ) swprintf( buf, 96, L"pixels (not in full screen)" );
        else swprintf( buf, 96, L"notes and keyboard, up to %ld x %ld", szMax.cx, szMax.cy );
        SetDlgItemTextW( hWnd, IDC_WINSIZEMAX, buf );
        EnableWindow( GetDlgItem( hWnd, IDC_WINWIDTH ), !bFullScreen );
        EnableWindow( GetDlgItem( hWnd, IDC_WINHEIGHT ), !bFullScreen );
    }

    // Reads a window size edit within [iMin, iMax] (or the size it showed, iShown, as it is);
    // otherwise says so and keeps the sheet open
    bool ReadSize( HWND hWnd, int iId, int iMin, int iMax, int iShown, const wchar_t *sWhat, int &iValue )
    {
        BOOL bOk = FALSE;
        UINT u = GetDlgItemInt( hWnd, iId, &bOk, FALSE );
        if ( bOk && ( (int)u == iShown || ( (int)u >= iMin && (int)u <= iMax ) ) ) { iValue = (int)u; return true; }
        wchar_t buf[128];
        swprintf( buf, 128, L"Please specify a view %ls (notes and keyboard) between %d and %d pixels", sWhat, iMin, iMax );
        Invalid( hWnd, iId, buf );
        return false;
    }

    // The colour dialog's hex box (added under its buttons), kept in step with its Red, Green and Blue
    // boxes both ways
    const int IDC_HEXCOLOR = 0x7001, IDC_HEXLABEL = 0x7002;
    bool g_bHexSync = false;      // one side is being set from the other

    // "#RRGGBB", "RRGGBB" or "#RGB" (spaces around ignored); false: not one
    bool ParseHex( const wchar_t *s, COLORREF &cr )
    {
        while ( *s == L' ' ) s++;
        if ( *s == L'#' ) s++;
        unsigned v = 0;
        int n = 0;
        for ( ; iswxdigit( *s ); s++, n++ ) v = v * 16 + ( iswdigit( *s ) ? *s - L'0' : towlower( *s ) - L'a' + 10 );
        while ( *s == L' ' ) s++;
        if ( *s || ( n != 6 && n != 3 ) ) return false;
        if ( n == 3 ) v = ( v >> 8 & 0xF ) * 0x110000 + ( v >> 4 & 0xF ) * 0x1100 + ( v & 0xF ) * 0x11;
        cr = RGB( v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF );
        return true;
    }

    VOID ShowHex( HWND hDlg, COLORREF cr )
    {
        wchar_t s[16];
        swprintf( s, 16, L"#%02X%02X%02X", GetRValue( cr ), GetGValue( cr ), GetBValue( cr ) );
        g_bHexSync = true;
        SetDlgItemTextW( hDlg, IDC_HEXCOLOR, s );
        g_bHexSync = false;
    }

    UINT_PTR CALLBACK ColorHook( HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        if ( msg == WM_INITDIALOG )
        {
            // The hex box where OK and Cancel were; they move to a row of their own at the bottom right,
            // under Add to Custom Colors, and the dialog grows to fit it
            auto Rect = [&]( HWND h ) { RECT rc; GetWindowRect( h, &rc ); MapWindowPoints( nullptr, hDlg, ( LPPOINT )&rc, 2 ); return rc; };
            HWND hOk = GetDlgItem( hDlg, IDOK ), hCancel = GetDlgItem( hDlg, IDCANCEL );
            const RECT rcOk = Rect( hOk ), rcCancel = Rect( hCancel ), rcAdd = Rect( GetDlgItem( hDlg, COLOR_ADD ) );
            RECT rcClient, rcWnd;
            GetClientRect( hDlg, &rcClient );
            const int cy = rcOk.bottom - rcOk.top, cxButton = rcCancel.right - rcCancel.left, cxGap = rcCancel.left - rcOk.right;
            const int y = std::max( rcOk.bottom, rcAdd.bottom ) + cy / 2, iMargin = rcClient.bottom - rcOk.bottom;
            const HFONT hFont = ( HFONT )SendMessageW( hDlg, WM_GETFONT, 0, 0 );
            HWND hLabel = CreateWindowExW( 0, L"Static", L"He&x:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, rcOk.left, rcOk.top,
                                           rcCancel.left - rcOk.left, cy, hDlg, ( HMENU )(INT_PTR)IDC_HEXLABEL, nullptr, nullptr );
            HWND hEdit = CreateWindowExW( WS_EX_CLIENTEDGE, L"Edit", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, rcCancel.left, rcCancel.top,
                                          cxButton, cy, hDlg, ( HMENU )(INT_PTR)IDC_HEXCOLOR, nullptr, nullptr );
            SendMessageW( hLabel, WM_SETFONT, ( WPARAM )hFont, FALSE );
            SendMessageW( hEdit, WM_SETFONT, ( WPARAM )hFont, FALSE );
            SendMessageW( hEdit, EM_SETLIMITTEXT, 9, 0 );
            // (tab order: the hex box after Define Custom Colors, OK and Cancel last)
            SetWindowPos( hLabel, GetDlgItem( hDlg, COLOR_MIX ), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
            SetWindowPos( hEdit, hLabel, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
            SetWindowPos( hOk, HWND_BOTTOM, rcAdd.right - cxButton * 2 - cxGap, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE );
            SetWindowPos( hCancel, HWND_BOTTOM, rcAdd.right - cxButton, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE );
            GetWindowRect( hDlg, &rcWnd );
            SetWindowPos( hDlg, nullptr, 0, 0, rcWnd.right - rcWnd.left, rcWnd.bottom - rcWnd.top + ( y + cy + iMargin - rcClient.bottom ),
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
            ShowHex( hDlg, ( ( const CHOOSECOLORW * )lParam )->rgbResult );
            DarkMode::Dialog( hDlg );
        }
        else if ( msg == WM_COMMAND && HIWORD( wParam ) == EN_CHANGE && !g_bHexSync )
        {
            // (COLOR_RED, COLOR_GREEN, COLOR_BLUE: the dialog's own boxes, set by it as the colour changes)
            const int iId = LOWORD( wParam );
            if ( iId == IDC_HEXCOLOR )
            {
                wchar_t s[16];
                COLORREF cr;
                GetDlgItemTextW( hDlg, IDC_HEXCOLOR, s, 16 );
                if ( !ParseHex( s, cr ) ) return 0;
                g_bHexSync = true;
                SetDlgItemInt( hDlg, COLOR_RED, GetRValue( cr ), FALSE );
                SetDlgItemInt( hDlg, COLOR_GREEN, GetGValue( cr ), FALSE );
                SetDlgItemInt( hDlg, COLOR_BLUE, GetBValue( cr ), FALSE );
                g_bHexSync = false;
            }
            else if ( iId == COLOR_RED || iId == COLOR_GREEN || iId == COLOR_BLUE )
                ShowHex( hDlg, RGB( std::min( 255u, GetDlgItemInt( hDlg, COLOR_RED, nullptr, FALSE ) ),
                                    std::min( 255u, GetDlgItemInt( hDlg, COLOR_GREEN, nullptr, FALSE ) ),
                                    std::min( 255u, GetDlgItemInt( hDlg, COLOR_BLUE, nullptr, FALSE ) ) ) );
        }
        return 0;
    }

    // Windows' colour dialog from cr (darkened as it opens, with a hex box); false: cancelled
    bool PickColor( HWND hWnd, COLORREF &cr )
    {
        static COLORREF acrCustClr[16] = { 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF,
                                           0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF };
        CHOOSECOLORW cc = {};
        cc.lStructSize = sizeof( cc );
        cc.hwndOwner = hWnd;
        cc.lpCustColors = acrCustClr;
        cc.rgbResult = cr;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ENABLEHOOK;
        cc.lpfnHook = ColorHook;
        if ( !ChooseColorW( &cc ) ) return false;
        cr = cc.rgbResult;
        return true;
    }

    // A colour button: its colour (kept in its user data)
    VOID DrawSwatch( LPDRAWITEMSTRUCT pdis )
    {
        SetDCBrushColor( pdis->hDC, ( COLORREF )GetWindowLongPtrW( pdis->hwndItem, GWLP_USERDATA ) );
        FillRect( pdis->hDC, &pdis->rcItem, ( HBRUSH )GetStockObject( DC_BRUSH ) );
        if ( pdis->itemState & ODS_FOCUS ) DrawFocusRect( pdis->hDC, &pdis->rcItem );
    }

    //-------------------------------------------------------------------------
    // Notes & keys (VisualSettings: the keys shown, the track colours, how notes are drawn)
    //-------------------------------------------------------------------------

    VOID ShowKeys( HWND hWnd, const VisualSettings &v )
    {
        for ( int i = 0; i < 5; i++ ) Check( hWnd, aKeyModeIds[i], i == v.eKeysShown );
        EnableCustomKeys( hWnd, v.eKeysShown == VisualSettings::Custom );
        SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_SETCURSEL, v.iFirstKey, 0 );
        SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_SETCURSEL, v.iLastKey, 0 );
        Check( hWnd, IDC_EDO31KEYS, v.bEdo31Keys );
        std::copy( v.colors, v.colors + VisualSettings::MaxColors, g_aPalette );
        SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETRANGE32, 1, VisualSettings::MaxColors );
        SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETPOS32, 0, v.iColorCount );
        InvalidateRect( GetDlgItem( hWnd, IDC_PALETTE ), nullptr, FALSE );
        SendDlgItemMessageW( hWnd, IDC_LOOPCOLORS, CB_SETCURSEL, v.bLoopColors ? 0 : v.bUniqueColors ? 2 : 1, 0 );
        Check( hWnd, IDC_HIDELINES, v.bHideLines );
        g_uClean = v.uClean & 7;
        ShowClean( hWnd );
    }

    INT_PTR CALLBACK KeysProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                for ( int i = 0; i < 256; i++ )
                {
                    SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_ADDSTRING, 0, ( LPARAM )Notes::NoteName( i ) );
                    SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_ADDSTRING, 0, ( LPARAM )Notes::NoteName( i ) );
                }
                // After the track colours: the colours again, random ones, or random ones each different
                for ( const wchar_t *s : { L"then loop", L"then random", L"then unique" } )
                    SendDlgItemMessageW( hWnd, IDC_LOOPCOLORS, CB_ADDSTRING, 0, ( LPARAM )s );
                SendDlgItemMessageW( hWnd, IDC_LOOPCOLORS, CB_SETDROPPEDWIDTH, 200, 0 );
                const VisualSettings &v = Config::GetConfig().GetVisualSettings();
                ShowKeys( hWnd, v );
                g_vPalettes = v.vPalettes;
                ShowPalettes( hWnd, -1 );
                AddTips( hWnd, {
                    { IDC_SHOWSONGKEYS, L"From the song's lowest key to its highest." },
                    { IDC_EDO31KEYS, L"With Show 256 keys: the keys laid out as 31-EDO's, 31 to an octave. Misc > 31-EDO tunes them." },
                    { IDC_LOOPCOLORS, L"After the track colours: the same colours again, random ones, or random ones each different." },
                    { IDC_CLEAN, L"Every note at least one pixel tall; short notes kept on screen from the top edge until they reach the keyboard; "
                                 L"note edges rounded evenly, so lower layers never show past notes above. Advanced turns each on or off." },
                } );
                return TRUE;
            }
            case WM_DRAWITEM:
            {
                LPDRAWITEMSTRUCT pdis = ( LPDRAWITEMSTRUCT )lParam;
                if ( pdis->CtlID != IDC_PALETTE ) return FALSE;
                // Each colour in use filled, the rest outlined; a 1-pixel gap between cells
                FillRect( pdis->hDC, &pdis->rcItem, DarkMode::On() ? DarkMode::BackBrush() : GetSysColorBrush( COLOR_BTNFACE ) );
                int n = PaletteCount( hWnd );
                for ( int i = 0; i < VisualSettings::MaxColors; i++ )
                {
                    RECT rc = PaletteCell( pdis->rcItem, i );
                    rc.right--;
                    rc.bottom--;
                    if ( i < n )
                    {
                        SetDCBrushColor( pdis->hDC, g_aPalette[i] );
                        FillRect( pdis->hDC, &rc, ( HBRUSH )GetStockObject( DC_BRUSH ) );
                    }
                    else
                    {
                        SetDCBrushColor( pdis->hDC, DarkMode::On() ? DarkMode::Edge() : GetSysColor( COLOR_BTNSHADOW ) );
                        FrameRect( pdis->hDC, &rc, ( HBRUSH )GetStockObject( DC_BRUSH ) );
                    }
                }
                if ( pdis->itemState & ODS_FOCUS ) DrawFocusRect( pdis->hDC, &pdis->rcItem );
                return TRUE;
            }
            case WM_COMMAND:
            {
                int iId = LOWORD( wParam );
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                switch ( iId )
                {
                    case IDC_SHOWCUSTOMKEYS:
                        EnableCustomKeys( hWnd, TRUE );
                        return TRUE;
                    case IDC_SHOWALLKEYS: case IDC_SHOWSONGKEYS: case IDC_SHOWALL128KEYS: case IDC_SHOWALL256KEYS:
                        EnableCustomKeys( hWnd, FALSE );
                        return TRUE;
                    case IDC_COLORCOUNT:
                        if ( HIWORD( wParam ) == EN_CHANGE ) { InvalidateRect( GetDlgItem( hWnd, IDC_PALETTE ), nullptr, FALSE ); Changed( hWnd ); }
                        return TRUE;
                    // Saved palettes: one chosen fills the editor; the editor's saved under a name; the
                    // default palette as a new one
                    case IDC_PALETTES:
                    {
                        if ( HIWORD( wParam ) != CBN_SELCHANGE ) return TRUE;
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_PALETTES, CB_GETCURSEL, 0, 0 );
                        if ( i >= 0 && i < (int)g_vPalettes.size() ) ShowColors( hWnd, g_vPalettes[i].vColors.data(), (int)g_vPalettes[i].vColors.size() );
                        EnableWindow( GetDlgItem( hWnd, IDC_PALETTEDELETE ), i >= 0 );
                        Changed( hWnd );
                        return TRUE;
                    }
                    case IDC_PALETTESAVE:
                    {
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_PALETTES, CB_GETCURSEL, 0, 0 );
                        std::wstring sName = i >= 0 && i < (int)g_vPalettes.size() ? g_vPalettes[i].sName : L"Palette " + std::to_wstring( g_vPalettes.size() + 1 );
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_PALETTENAME ), GetParent( hWnd ), PaletteNameProc,
                                              ( LPARAM )&sName ) != IDOK ) return TRUE;
                        VisualSettings::Palette pal = { sName, std::vector< unsigned >( g_aPalette, g_aPalette + PaletteCount( hWnd ) ) };
                        auto it = std::find_if( g_vPalettes.begin(), g_vPalettes.end(), [&]( const VisualSettings::Palette &p ) { return _wcsicmp( p.sName.c_str(), sName.c_str() ) == 0; } );
                        if ( it != g_vPalettes.end() ) *it = pal;
                        else it = g_vPalettes.insert( g_vPalettes.end(), pal );
                        ShowPalettes( hWnd, (int)( it - g_vPalettes.begin() ) );
                        Changed( hWnd );
                        return TRUE;
                    }
                    case IDC_PALETTEDELETE:
                    {
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_PALETTES, CB_GETCURSEL, 0, 0 );
                        if ( i < 0 || i >= (int)g_vPalettes.size() ) return TRUE;
                        std::wstring sAsk = L"Delete the palette \"" + g_vPalettes[i].sName + L"\"?";
                        if ( MessageBoxW( GetParent( hWnd ), sAsk.c_str(), L"Delete Palette", MB_YESNO | MB_ICONQUESTION ) != IDYES ) return TRUE;
                        g_vPalettes.erase( g_vPalettes.begin() + i );
                        ShowPalettes( hWnd, -1 );
                        Changed( hWnd );
                        return TRUE;
                    }
                    case IDC_PALETTENEW:
                    {
                        VisualSettings def;
                        def.LoadDefaultValues();
                        ShowColors( hWnd, def.colors, def.iColorCount );
                        ShowPalettes( hWnd, -1 );
                        Changed( hWnd );
                        return TRUE;
                    }
                    // A colour: the chooser, for the cell clicked (with the keyboard, the first); one past the
                    // colours in use adds it, and those before it
                    case IDC_PALETTE:
                    {
                        HWND hWndBtn = ( HWND )lParam;
                        DWORD dwPos = GetMessagePos();
                        POINT pt = { (short)LOWORD( dwPos ), (short)HIWORD( dwPos ) };
                        ScreenToClient( hWndBtn, &pt );
                        RECT rc;
                        GetClientRect( hWndBtn, &rc );
                        int iCell = 0;
                        if ( PtInRect( &rc, pt ) )
                            for ( int i = 0; i < VisualSettings::MaxColors; i++ )
                            {
                                RECT rcCell = PaletteCell( rc, i );
                                if ( PtInRect( &rcCell, pt ) ) { iCell = i; break; }
                            }
                        COLORREF cr = g_aPalette[iCell];
                        if ( !PickColor( hWnd, cr ) ) return TRUE;
                        // (the page's state once the colour is in: Apply lights by what changed)
                        g_aPalette[iCell] = cr;
                        if ( iCell >= PaletteCount( hWnd ) ) SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETPOS32, 0, iCell + 1 );
                        InvalidateRect( hWndBtn, nullptr, FALSE );
                        Changed( hWnd );
                        return TRUE;
                    }
                    case IDC_CLEAN:
                        g_uClean = g_uClean == 7 ? 0 : 7;
                        ShowClean( hWnd );
                        Changed( hWnd );
                        return TRUE;
                    case IDC_CLEANADV:
                    {
                        unsigned u = g_uClean;
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_CLEANADV ), GetParent( hWnd ), CleanAdvProc,
                                              ( LPARAM )&u ) == IDOK && u != g_uClean )
                        {
                            g_uClean = u;
                            ShowClean( hWnd );
                            Changed( hWnd );
                        }
                        return TRUE;
                    }
                    case IDC_RESTOREDEFAULTS:
                    {
                        VisualSettings d;
                        d.LoadDefaultValues();
                        ShowKeys( hWnd, d );
                        ShowPalettes( hWnd, -1 );
                        Changed( hWnd );
                        return TRUE;
                    }
                }
                break;
            }
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    VisualSettings v = config.GetVisualSettings();
                    v.eKeysShown = VisualSettings::All128;
                    for ( int i = 0; i < 5; i++ )
                        if ( Checked( hWnd, aKeyModeIds[i] ) ) v.eKeysShown = static_cast< VisualSettings::KeysShown >( i );
                    v.iFirstKey = (int)SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_GETCURSEL, 0, 0 );
                    v.iLastKey = (int)SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_GETCURSEL, 0, 0 );
                    v.bEdo31Keys = Checked( hWnd, IDC_EDO31KEYS );
                    std::copy( g_aPalette, g_aPalette + VisualSettings::MaxColors, v.colors );
                    v.vPalettes = g_vPalettes;
                    v.iColorCount = PaletteCount( hWnd );
                    int iAfter = (int)SendDlgItemMessageW( hWnd, IDC_LOOPCOLORS, CB_GETCURSEL, 0, 0 );
                    v.bLoopColors = iAfter == 0;
                    if ( iAfter > 0 ) v.bUniqueColors = iAfter == 2;     // (looping keeps the random kind for later)
                    v.bHideLines = Checked( hWnd, IDC_HIDELINES );
                    v.uClean = g_uClean;
                    config.SetVisualSettings( v );
                    return Applied( hWnd );
                }
                break;
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Stats (VisualSettings: the stats box's lines and look, the graphs)
    //-------------------------------------------------------------------------

    VOID ShowStatsPage( HWND hWnd, const VisualSettings &v )
    {
        g_Stats.uStats = v.uStats;
        std::copy( v.aStatOrder, v.aStatOrder + VisualSettings::StatCount, g_Stats.aOrder );
        g_Graph = v.graph;
        Check( hWnd, IDC_GRAPHON, v.bGraph );     // (also View > Graphs)
        Check( hWnd, IDC_STATS_ADVANCED, v.bAdvancedStats );
        ShowStatsCount( hWnd );
        Check( hWnd, IDC_STATSSCALED, v.bStatsScaled );
        SetDlgItemInt( hWnd, IDC_STATSSIZE, (UINT)v.iStatsCX, FALSE );
        SetDlgItemInt( hWnd, IDC_STATSHEIGHT, (UINT)v.iStatsCY, FALSE );
        // (the font's size or the height: one or the other)
        Check( hWnd, IDC_STATSFONTSET, v.bStatsFontSet );
        SetDlgItemInt( hWnd, IDC_STATSFONTPX, (UINT)v.iStatsFontPx, FALSE );
        Check( hWnd, IDC_STATSPFASIZE, v.bStatsPfaSize && !v.bStatsFontSet );
        ShowStatsSize( hWnd );
        SendDlgItemMessageW( hWnd, IDC_STATSCORNER, CB_SETCURSEL, v.iStatsCorner, 0 );
        SetDlgItemTextW( hWnd, IDC_STATSFONT, v.sStatsFont.c_str() );
        Check( hWnd, IDC_STATSBKG, v.bStatsBackground );
        Check( hWnd, IDC_STATSSHADOW, v.bStatsShadow );
    }

    INT_PTR CALLBACK StatsPageProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                // By VisualSettings::StatsCorner bits
                for ( const wchar_t *s : { L"Top right", L"Top left", L"Bottom right", L"Bottom left" } )
                    SendDlgItemMessageW( hWnd, IDC_STATSCORNER, CB_ADDSTRING, 0, ( LPARAM )s );
                // The installed fonts (each family once; not the vertical "@" ones)
                LOGFONTW lf = {};
                lf.lfCharSet = DEFAULT_CHARSET;
                HDC hdc = GetDC( hWnd );
                EnumFontFamiliesExW( hdc, &lf, []( const LOGFONTW *plf, const TEXTMETRICW *, DWORD, LPARAM l ) -> int {
                    HWND hCombo = ( HWND )l;
                    if ( plf->lfFaceName[0] != L'@' && SendMessageW( hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, ( LPARAM )plf->lfFaceName ) == CB_ERR )
                        SendMessageW( hCombo, CB_ADDSTRING, 0, ( LPARAM )plf->lfFaceName );
                    return 1;
                }, ( LPARAM )GetDlgItem( hWnd, IDC_STATSFONT ), 0 );
                ReleaseDC( hWnd, hdc );
                ShowStatsPage( hWnd, Config::GetConfig().GetVisualSettings() );
                AddTips( hWnd, {
                    { IDC_STATSPFASIZE, L"156 pixels wide and as tall as its lines, unscaled, as in the original." },
                    { IDC_STATSSCALED, L"The sizes are as at 1080p, and the box grows and shrinks with the window's height. Off: they are pixels." },
                    { IDC_STATS_ADVANCED, L"For troubleshooting playback: messages sent, skipped and collapsed, the lag, the decoder, sender and ring, "
                                          L"the send cost, and the frame time's parts. Chosen in Statistics... once on." },
                    { IDC_STATSHEIGHT, L"Always the height the box is drawn at: by the font's size or as PFA's, it follows them." },
                } );
                return TRUE;
            }
            case WM_COMMAND:
            {
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                switch ( LOWORD( wParam ) )
                {
                    case IDC_STATSFONTPX:
                        if ( HIWORD( wParam ) == EN_CHANGE ) ShowStatsSize( hWnd );
                        return TRUE;
                    case IDC_STATS_ADVANCED:
                        ShowStatsCount( hWnd );
                        ShowStatsSize( hWnd );
                        return TRUE;
                    case IDC_STATSFONTSET:
                    case IDC_STATSPFASIZE:
                        // (one or the other)
                        if ( Checked( hWnd, LOWORD( wParam ) ) )
                            CheckDlgButton( hWnd, LOWORD( wParam ) == IDC_STATSFONTSET ? IDC_STATSPFASIZE : IDC_STATSFONTSET, BST_UNCHECKED );
                        ShowStatsSize( hWnd );
                        return TRUE;
                    case IDC_GRAPHBTN:
                    {
                        VisualSettings::GraphSettings c = g_Graph;
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_GRAPH ), GetParent( hWnd ), GraphProc,
                                              ( LPARAM )&c ) == IDOK && !SameGraph( c, g_Graph ) )
                        {
                            g_Graph = c;
                            Changed( hWnd );
                        }
                        return TRUE;
                    }
                    case IDC_STATSBOX:
                    {
                        StatsChoice c = g_Stats;
                        c.bAdvanced = Checked( hWnd, IDC_STATS_ADVANCED );
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_STATS ), GetParent( hWnd ), StatsProc,
                                              ( LPARAM )&c ) == IDOK &&
                             ( c.uStats != g_Stats.uStats || !std::equal( c.aOrder, c.aOrder + VisualSettings::StatCount, g_Stats.aOrder ) ) )
                        {
                            g_Stats = c;
                            ShowStatsCount( hWnd );
                            ShowStatsSize( hWnd );
                            Changed( hWnd );
                        }
                        return TRUE;
                    }
                    case IDC_RESTOREDEFAULTS:
                    {
                        VisualSettings d;
                        d.LoadDefaultValues();
                        ShowStatsPage( hWnd, d );
                        Changed( hWnd );
                        return TRUE;
                    }
                }
                break;
            }
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    VisualSettings v = config.GetVisualSettings();
                    // The box's size: the width, then the height or the font's size, whichever is set (the other kept)
                    const bool bStatsFontSet = Checked( hWnd, IDC_STATSFONTSET );
                    const bool bStatsPfaSize = !bStatsFontSet && Checked( hWnd, IDC_STATSPFASIZE );
                    int aiStatsSize[3] = { 0, v.iStatsCY, v.iStatsFontPx };
                    {
                        const int aiId[3] = { IDC_STATSSIZE, IDC_STATSHEIGHT, IDC_STATSFONTPX };
                        const int aiMin[3] = { VisualSettings::StatsCXMin, VisualSettings::StatsCYMin, VisualSettings::StatsFontMin };
                        const int aiMax[3] = { VisualSettings::StatsCXMax, VisualSettings::StatsCYMax, VisualSettings::StatsFontMax };
                        static const wchar_t *const asWhat[3] = { L"width", L"height", L"font size" };
                        for ( int i = 0; i < 3; i++ )
                        {
                            BOOL bOk = FALSE;
                            UINT u = GetDlgItemInt( hWnd, aiId[i], &bOk, FALSE );
                            if ( i == 2 && !bStatsFontSet ) continue;
                            // (by the font's size, or as PFA's: the height as it shows, kept for when it is set again)
                            if ( i == 1 && ( bStatsFontSet || bStatsPfaSize ) )
                            {
                                if ( bOk ) aiStatsSize[1] = std::clamp( (int)std::min( u, 100000u ), aiMin[1], aiMax[1] );
                                continue;
                            }
                            if ( !bOk || u < (UINT)aiMin[i] || u > (UINT)aiMax[i] )
                            {
                                wchar_t sMsg[128];
                                swprintf( sMsg, 128, L"Please specify a stats box %ls between %d and %d pixels", asWhat[i], aiMin[i], aiMax[i] );
                                return Invalid( hWnd, aiId[i], sMsg );
                            }
                            aiStatsSize[i] = (int)u;
                        }
                    }
                    v.uStats = g_Stats.uStats;
                    v.bAdvancedStats = Checked( hWnd, IDC_STATS_ADVANCED );
                    std::copy( g_Stats.aOrder, g_Stats.aOrder + VisualSettings::StatCount, v.aStatOrder );
                    v.graph = g_Graph;
                    v.bStatsScaled = Checked( hWnd, IDC_STATSSCALED );
                    v.iStatsCX = aiStatsSize[0];
                    v.iStatsCY = aiStatsSize[1];
                    v.bStatsFontSet = bStatsFontSet;
                    v.bStatsPfaSize = bStatsPfaSize;
                    v.iStatsFontPx = aiStatsSize[2];
                    v.iStatsCorner = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_STATSCORNER, CB_GETCURSEL, 0, 0 ) );
                    wchar_t sFont[LF_FACESIZE] = L"";
                    GetDlgItemTextW( hWnd, IDC_STATSFONT, sFont, LF_FACESIZE );
                    v.sStatsFont = sFont[0] ? sFont : L"Tahoma";
                    v.bStatsBackground = Checked( hWnd, IDC_STATSBKG );
                    v.bStatsShadow = Checked( hWnd, IDC_STATSSHADOW );
                    const bool bGraph = Checked( hWnd, IDC_GRAPHON ), bGraphChanged = bGraph != v.bGraph;
                    v.bGraph = bGraph;
                    config.SetVisualSettings( v );
                    if ( bGraphChanged ) ShowGraphs( bGraph );
                    return Applied( hWnd );
                }
                break;
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Audio (AudioSettings, and KDMAPI's in PerformanceSettings; audio/RenderAhead.h): a subtab per output,
    // KDMAPI, WinMM and Pre-Render, and in Pre-Render one per engine. Choosing a subtab switches to that
    // output, or engine, at once (as the MIDI output device chosen does); the rest waits for Apply
    //-------------------------------------------------------------------------

    // The panels: each subtab's controls, after its marker in the template (IDC_PANEL_*, in this order)
    enum AudioPanel { PanelKdmapi, PanelWinmm, PanelPreRender, PanelBass, PanelKestrel, PanelMerlin, PanelCount };
    enum AudioOutput { OutKdmapi, OutWinmm, OutPreRender, OutCount };
    std::vector< HWND > g_avPanel[PanelCount];
    RenderBackends::KestrelOpts g_RenderKb;     // the advanced dialogs', applied with the page
    bool g_bSyncing = false;                    // one of a pair of fields being set from the other

    // The output in use, as the subtabs show it
    int CurrentOutput()
    {
        if ( Config::GetConfig().CopyAudioSettings().bRendered ) return OutPreRender;
        return Config::GetConfig().GetPerformanceSettings().bPreferKDMAPI ? OutKdmapi : OutWinmm;
    }

    // Only the chosen subtabs' controls shown
    VOID ShowAudioPanels( HWND hWnd )
    {
        const int iOut = TabCtrl_GetCurSel( GetDlgItem( hWnd, IDC_AUDIO_TABS ) );
        const int iEngine = TabCtrl_GetCurSel( GetDlgItem( hWnd, IDC_PR_TABS ) );
        for ( int p = 0; p < PanelCount; p++ )
        {
            const bool bShow = p < PanelBass ? p == iOut : iOut == OutPreRender && p - PanelBass == iEngine;
            for ( HWND h : g_avPanel[p] ) ShowWindow( h, bShow ? SW_SHOWNA : SW_HIDE );
        }
    }

    // A subtab chosen: that output (or Pre-Render's engine) in use at once
    VOID SetOutput( int iOut )
    {
        Config &config = Config::GetConfig();
        AudioSettings a = config.CopyAudioSettings();
        PerformanceSettings cPerf = config.GetPerformanceSettings();
        const bool bWasKdmapi = cPerf.bPreferKDMAPI, bWasRendered = a.bRendered;
        if ( iOut != OutPreRender ) cPerf.bPreferKDMAPI = iOut == OutKdmapi;
        a.bRendered = iOut == OutPreRender;
        config.SetAudioSettings( a );
        config.SetPerformanceSettings( cPerf );
        if ( bWasKdmapi != cPerf.bPreferKDMAPI ) HandOffMsg( WM_DEVICECHANGE, 0, 0 );
        if ( a.bRendered && !bWasRendered ) RenderAhead::PreloadFromConfig();
    }

    VOID SetEngine( int eEngine )
    {
        Config &config = Config::GetConfig();
        AudioSettings a = config.CopyAudioSettings();
        if ( a.eRenderEngine == eEngine ) return;
        a.eRenderEngine = eEngine;
        config.SetAudioSettings( a );
        RenderAhead::PreloadFromConfig();
    }

    // The WinMM devices, and what KDMAPI reports
    VOID ShowDevices( HWND hWnd )
    {
        Config::GetConfig().LoadMIDIDevices();
        const AudioSettings cAudio = Config::GetConfig().CopyAudioSettings();
        HWND hWndOutDevs = GetDlgItem( hWnd, IDC_MIDIOUT );
        SendMessageW( hWndOutDevs, LB_RESETCONTENT, 0, 0 );
        for ( const std::wstring &s : cAudio.vMIDIOutDevices )
            SendMessageW( hWndOutDevs, LB_ADDSTRING, 0, ( LPARAM )s.c_str() );
        SendMessageW( hWndOutDevs, LB_SETCURSEL, cAudio.iOutDevice, 0 );
        SetDlgItemTextW( hWnd, IDC_KDMAPISTATUS, MidiOutputs::KdmapiStatus().c_str() );
    }

    // The device chosen, in use at once
    VOID SetDevice( HWND hWnd )
    {
        Config &config = Config::GetConfig();
        AudioSettings cAudio = config.CopyAudioSettings();
        const int i = (int)SendDlgItemMessageW( hWnd, IDC_MIDIOUT, LB_GETCURSEL, 0, 0 );
        if ( i < 0 || i >= (int)cAudio.vMIDIOutDevices.size() || i == cAudio.iOutDevice ) return;
        cAudio.iOutDevice = i;
        cAudio.sDesiredOut = cAudio.vMIDIOutDevices[i];
        config.SetAudioSettings( cAudio );
        HandOffMsg( WM_DEVICECHANGE, 0, 0 );
    }

    // Kestrel's advanced settings, or Merlin's (the soundfont and output ones they share): each control the
    // dialog has
    INT_PTR CALLBACK KestrelAdvProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        static const int aBlocks[] = { 512, 1024, 2048, 4096, 8192, 16384 };
        static const int aGates[] = { 8, 16, 32, 64, 128, 256 };
        static const int aGroups[] = { 32, 64, 128, 256, 512, 1024 };
        auto Pick = []( const int *a, int n, int v, int d ) { for ( int i = 0; i < n; i++ ) if ( a[i] == v ) return i; return d; };
        auto Has = [&]( int iId ) { return GetDlgItem( hWnd, iId ) != nullptr; };
        auto Show = [&]( const RenderBackends::KestrelOpts &k ) {
            SendDlgItemMessageW( hWnd, IDC_KA_INTERP, CB_SETCURSEL, k.iInterpolation, 0 );
            SendDlgItemMessageW( hWnd, IDC_KA_STEAL, CB_SETCURSEL, k.iStealRule, 0 );
            SendDlgItemMessageW( hWnd, IDC_KA_ADMIT, CB_SETCURSEL, k.iAdmitRule, 0 );
            SetDlgItemInt( hWnd, IDC_KA_STEALPCT, k.iMaxStealPercent, FALSE );
            SetDlgItemInt( hWnd, IDC_KA_FLOOR, k.iReleaseFloorDb, TRUE );
            SetDlgItemInt( hWnd, IDC_KA_LAYERS, k.iMaxLayers, FALSE );
            CheckDlgButton( hWnd, IDC_KA_FILTER, k.bFilter ? BST_CHECKED : BST_UNCHECKED );
            CheckDlgButton( hWnd, IDC_KA_LFO, k.bLfo ? BST_CHECKED : BST_UNCHECKED );
            CheckDlgButton( hWnd, IDC_KA_MODENV, k.bModEnv ? BST_CHECKED : BST_UNCHECKED );
            CheckDlgButton( hWnd, IDC_KA_DC, k.bDcBlocker ? BST_CHECKED : BST_UNCHECKED );
            SetDlgItemInt( hWnd, IDC_KA_DCHZ, k.iDcBlockerHz, FALSE );
            EnableWindow( GetDlgItem( hWnd, IDC_KA_DCHZ ), k.bDcBlocker );
            SendDlgItemMessageW( hWnd, IDC_KA_BLOCK, CB_SETCURSEL, Pick( aBlocks, 6, k.iBlockFrames, 3 ), 0 );
            SendDlgItemMessageW( hWnd, IDC_KA_GATE, CB_SETCURSEL, Pick( aGates, 6, k.iGateFrames, 2 ), 0 );
            SendDlgItemMessageW( hWnd, IDC_KA_WORKGROUP, CB_SETCURSEL, Pick( aGroups, 6, k.iWorkgroupSize, 3 ), 0 );
            SetDlgItemInt( hWnd, IDC_KA_RENDERWG, k.iRenderWorkgroups, FALSE );
            SetDlgItemInt( hWnd, IDC_KA_SUBMIT, k.iSubmitVoices, FALSE );
            SetDlgItemInt( hWnd, IDC_KA_THREADS, k.iVoiceThreads, FALSE );
            CheckDlgButton( hWnd, IDC_KA_SORT, k.bSortVoices ? BST_CHECKED : BST_UNCHECKED );
            CheckDlgButton( hWnd, IDC_KA_KAHAN, k.bKahan ? BST_CHECKED : BST_UNCHECKED );
            CheckDlgButton( hWnd, IDC_KA_UNCHECKED, k.bUnchecked ? BST_CHECKED : BST_UNCHECKED );
        };
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                for ( const wchar_t *s : { L"Nearest", L"Linear", L"Cubic" } ) SendDlgItemMessageW( hWnd, IDC_KA_INTERP, CB_ADDSTRING, 0, ( LPARAM )s );
                for ( const wchar_t *s : { L"the oldest voice", L"no voice (drop the new note)", L"the quietest voice" } ) SendDlgItemMessageW( hWnd, IDC_KA_STEAL, CB_ADDSTRING, 0, ( LPARAM )s );
                for ( const wchar_t *s : { L"the loudest notes", L"notes evenly" } ) SendDlgItemMessageW( hWnd, IDC_KA_ADMIT, CB_ADDSTRING, 0, ( LPARAM )s );
                for ( int b : aBlocks ) { wchar_t s[16]; swprintf( s, 16, L"%d", b ); SendDlgItemMessageW( hWnd, IDC_KA_BLOCK, CB_ADDSTRING, 0, ( LPARAM )s ); }
                for ( int b : aGates ) { wchar_t s[16]; swprintf( s, 16, L"%d", b ); SendDlgItemMessageW( hWnd, IDC_KA_GATE, CB_ADDSTRING, 0, ( LPARAM )s ); }
                for ( int b : aGroups ) { wchar_t s[16]; swprintf( s, 16, L"%d", b ); SendDlgItemMessageW( hWnd, IDC_KA_WORKGROUP, CB_ADDSTRING, 0, ( LPARAM )s ); }
                Show( g_RenderKb );
                return TRUE;
            }
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDC_KA_DC:
                        EnableWindow( GetDlgItem( hWnd, IDC_KA_DCHZ ), IsDlgButtonChecked( hWnd, IDC_KA_DC ) == BST_CHECKED );
                        return TRUE;
                    case IDC_KA_DEFAULTS:
                    {
                        // (the dialog's own settings: Merlin's leaves Kestrel's voice and performance ones)
                        RenderBackends::KestrelOpts d;
                        Show( d );
                        return TRUE;
                    }
                    case IDOK:
                    {
                        RenderBackends::KestrelOpts k = g_RenderKb;
                        BOOL bOk;
                        int v;
                        if ( Has( IDC_KA_INTERP ) ) k.iInterpolation = std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_KA_INTERP, CB_GETCURSEL, 0, 0 ), 0, 2 );
                        if ( Has( IDC_KA_STEAL ) ) k.iStealRule = std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_KA_STEAL, CB_GETCURSEL, 0, 0 ), 0, 2 );
                        if ( Has( IDC_KA_ADMIT ) ) k.iAdmitRule = std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_KA_ADMIT, CB_GETCURSEL, 0, 0 ), 0, 1 );
                        v = (int)GetDlgItemInt( hWnd, IDC_KA_STEALPCT, &bOk, FALSE );
                        if ( bOk ) k.iMaxStealPercent = std::clamp( v, 1, 100 );
                        v = (int)GetDlgItemInt( hWnd, IDC_KA_FLOOR, &bOk, TRUE );
                        if ( bOk ) k.iReleaseFloorDb = std::clamp( v, -160, -20 );
                        v = (int)GetDlgItemInt( hWnd, IDC_KA_LAYERS, &bOk, FALSE );
                        if ( bOk ) k.iMaxLayers = std::clamp( v, 1, 64 );
                        k.bFilter = IsDlgButtonChecked( hWnd, IDC_KA_FILTER ) == BST_CHECKED;
                        k.bLfo = IsDlgButtonChecked( hWnd, IDC_KA_LFO ) == BST_CHECKED;
                        k.bModEnv = IsDlgButtonChecked( hWnd, IDC_KA_MODENV ) == BST_CHECKED;
                        k.bDcBlocker = IsDlgButtonChecked( hWnd, IDC_KA_DC ) == BST_CHECKED;
                        v = (int)GetDlgItemInt( hWnd, IDC_KA_DCHZ, &bOk, FALSE );
                        if ( bOk ) k.iDcBlockerHz = std::clamp( v, 1, 200 );
                        k.iBlockFrames = aBlocks[std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_KA_BLOCK, CB_GETCURSEL, 0, 0 ), 0, 5 )];
                        if ( Has( IDC_KA_GATE ) ) k.iGateFrames = aGates[std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_KA_GATE, CB_GETCURSEL, 0, 0 ), 0, 5 )];
                        if ( Has( IDC_KA_WORKGROUP ) ) k.iWorkgroupSize = aGroups[std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_KA_WORKGROUP, CB_GETCURSEL, 0, 0 ), 0, 5 )];
                        v = (int)GetDlgItemInt( hWnd, IDC_KA_RENDERWG, &bOk, FALSE );
                        if ( bOk ) k.iRenderWorkgroups = std::clamp( v, 1, 1 << 24 );
                        v = (int)GetDlgItemInt( hWnd, IDC_KA_SUBMIT, &bOk, FALSE );
                        if ( bOk ) k.iSubmitVoices = std::clamp( v, 1024, 1 << 30 );
                        v = (int)GetDlgItemInt( hWnd, IDC_KA_THREADS, &bOk, FALSE );
                        if ( bOk ) k.iVoiceThreads = std::clamp( v, 1, 64 );
                        if ( Has( IDC_KA_SORT ) ) k.bSortVoices = IsDlgButtonChecked( hWnd, IDC_KA_SORT ) == BST_CHECKED;
                        if ( Has( IDC_KA_KAHAN ) ) k.bKahan = IsDlgButtonChecked( hWnd, IDC_KA_KAHAN ) == BST_CHECKED;
                        if ( Has( IDC_KA_UNCHECKED ) ) k.bUnchecked = IsDlgButtonChecked( hWnd, IDC_KA_UNCHECKED ) == BST_CHECKED;
                        const bool bChanged = !( k == g_RenderKb );
                        g_RenderKb = k;
                        EndDialog( hWnd, bChanged ? IDOK : IDCANCEL );
                        return TRUE;
                    }
                    case IDCANCEL:
                        EndDialog( hWnd, IDCANCEL );
                        return TRUE;
                }
                break;
        }
        return FALSE;
    }

    // Merlin's weak spots and the open song's; what the last soundfont load reported
    VOID ShowRenderNotes( HWND hWnd )
    {
        const std::wstring sSong = RenderBackends::GetNote( RenderBackends::NoteSong );
        std::wstring s = L"Merlin never drops a note (it has no voice limit). Not supported yet: channel volume and expression (CC7, "
                         L"CC11), and more than one soundfont (of several, it plays the lowest it takes, alone). Many pitch bends "
                         L"render slowly. ";
        s += sSong.empty() ? L"The open song uses none of these (or no song is open)." : L"The open song: " + sSong + L".";
        SetDlgItemTextW( hWnd, IDC_RA_MERLINNOTES, s.c_str() );
        std::wstring t = RenderBackends::GetNote( RenderBackends::NoteLoad );
        for ( wchar_t &c : t ) if ( c == L'\n' ) c = L';';
        SetDlgItemTextW( hWnd, IDC_RA_NOTES, t.empty() ? L"" : ( L"Last soundfont load: " + t ).c_str() );
    }

    // BASSMIDI's voice limit in all, from the page's fields as they are typed: the voices per instance x the
    // instances (16 a key group when split by channel, at most 16 groups then; as RenderBackends' Parts)
    VOID ShowVoiceLimit( HWND hWnd )
    {
        BOOL bOk = FALSE;
        UINT u = GetDlgItemInt( hWnd, IDC_RA_VOICES, &bOk, FALSE );
        const long long llVoices = bOk ? std::clamp( (long long)u, 1LL, 100000LL ) : 0;
        u = GetDlgItemInt( hWnd, IDC_RA_THREADS, &bOk, FALSE );
        const int iGroups = bOk ? std::clamp( (int)std::min( u, 1000u ), 1, 64 ) : 0;
        const int iParts = Checked( hWnd, IDC_RA_BYCHANNEL ) ? 16 * std::min( iGroups, 16 ) : iGroups;
        auto Group = []( long long v ) {
            std::wstring s = std::to_wstring( v );
            for ( int i = (int)s.size() - 3; i > 0; i -= 3 ) s.insert( (size_t)i, L"," );
            return s;
        };
        std::wstring s = L"Voice limit: ";
        if ( !llVoices || !iParts ) s += L"-";
        else
        {
            s += Group( llVoices * iParts );
            if ( iParts > 1 ) s += L" (" + Group( llVoices ) + L" x " + std::to_wstring( iParts ) + L" instances)";
        }
        SetDlgItemTextW( hWnd, IDC_RA_VOICELIMIT, s.c_str() );
    }

    VOID EnableRenderButtons( HWND hWnd )
    {
        HWND hList = GetDlgItem( hWnd, IDC_RA_FONTS );
        int iSel = (int)SendMessageW( hList, LB_GETCURSEL, 0, 0 ), n = (int)SendMessageW( hList, LB_GETCOUNT, 0, 0 );
        EnableWindow( GetDlgItem( hWnd, IDC_RA_REMOVE ), iSel >= 0 );
        EnableWindow( GetDlgItem( hWnd, IDC_RA_UP ), iSel > 0 );
        EnableWindow( GetDlgItem( hWnd, IDC_RA_DOWN ), iSel >= 0 && iSel < n - 1 );
    }

    std::wstring ListText( HWND hList, int i )
    {
        int n = (int)SendMessageW( hList, LB_GETTEXTLEN, i, 0 );
        if ( n < 0 ) return std::wstring();
        std::wstring s( (size_t)n + 1, L'\0' );
        SendMessageW( hList, LB_GETTEXT, i, ( LPARAM )&s[0] );
        s.resize( (size_t)n );
        return s;
    }

    // The list box's horizontal scroll reaches its longest path
    VOID FitRenderList( HWND hList )
    {
        HDC hdc = GetDC( hList );
        HFONT hOld = ( HFONT )SelectObject( hdc, ( HFONT )SendMessageW( hList, WM_GETFONT, 0, 0 ) );
        int iMax = 0;
        for ( int i = 0, n = (int)SendMessageW( hList, LB_GETCOUNT, 0, 0 ); i < n; i++ )
        {
            std::wstring s = ListText( hList, i );
            SIZE sz;
            if ( GetTextExtentPoint32W( hdc, s.c_str(), (int)s.size(), &sz ) ) iMax = std::max( iMax, (int)sz.cx );
        }
        SelectObject( hdc, hOld );
        ReleaseDC( hList, hdc );
        SendMessageW( hList, LB_SETHORIZONTALEXTENT, iMax + 8, 0 );
    }

    // The page's settings from a and cPerf (and its soundfonts when bFonts); not the output or engine (the
    // subtabs, in use as they are chosen)
    VOID ShowAudio( HWND hWnd, const AudioSettings &a, const PerformanceSettings &cPerf, bool bFonts )
    {
        Check( hWnd, IDC_PERF_LONGSENDS, cPerf.bKdmapiLongSends );
        SetDlgItemInt( hWnd, IDC_RA_VOICES, a.iBassVoices, FALSE );
        SetDlgItemInt( hWnd, IDC_RA_KVOICES, a.iKestrelVoices, FALSE );
        Check( hWnd, IDC_RA_FX, a.bRenderFx );
        SetDlgItemInt( hWnd, IDC_RA_THREADS, a.iBassThreads, FALSE );
        g_bSyncing = true;
        SetDlgItemInt( hWnd, IDC_RA_GPUSHARE, a.iGpuShare, FALSE );
        SetDlgItemInt( hWnd, IDC_RA_GPUSHARE2, a.iGpuShare, FALSE );
        g_bSyncing = false;
        g_RenderKb = a.kb;
        Check( hWnd, IDC_RA_LIMITER, a.bLimiter );
        SetDlgItemInt( hWnd, IDC_RA_AHEAD, a.iAheadSecs, FALSE );
        SetDlgItemInt( hWnd, IDC_RA_NPS, (UINT)a.iBassNps, FALSE );
        Check( hWnd, IDC_RA_BYCHANNEL, a.bBassByChannel );
        ShowVoiceLimit( hWnd );
        if ( bFonts )
        {
            HWND hList = GetDlgItem( hWnd, IDC_RA_FONTS );
            SendMessageW( hList, LB_RESETCONTENT, 0, 0 );
            for ( size_t i = 0; i <= a.sSoundfonts.size(); )
            {
                size_t j = a.sSoundfonts.find( L'|', i );
                if ( j == std::wstring::npos ) j = a.sSoundfonts.size();
                if ( j > i ) SendMessageW( hList, LB_ADDSTRING, 0, ( LPARAM )a.sSoundfonts.substr( i, j - i ).c_str() );
                i = j + 1;
            }
            FitRenderList( hList );
        }
        Check( hWnd, IDC_RA_LOSSLESS, a.bRenderLossless );
        Check( hWnd, IDC_RA_LOSSLESS2, a.bRenderLossless );
        Check( hWnd, IDC_RA_DEGRADE, a.bRenderDegrade );
    }

    INT_PTR CALLBACK AudioProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                // The subtabs, each panel's controls (the template's order), the tabs under them
                HWND hTabs = GetDlgItem( hWnd, IDC_AUDIO_TABS ), hEngines = GetDlgItem( hWnd, IDC_PR_TABS );
                auto AddTab = []( HWND hTab, int i, const wchar_t *s ) {
                    TCITEMW ti = {};
                    ti.mask = TCIF_TEXT;
                    ti.pszText = const_cast< LPWSTR >( s );
                    TabCtrl_InsertItem( hTab, i, &ti );
                };
                AddTab( hTabs, OutKdmapi, L"KDMAPI" );
                AddTab( hTabs, OutWinmm, L"WinMM" );
                AddTab( hTabs, OutPreRender, L"Pre-Render" );
                AddTab( hEngines, RenderBackends::EngineBass, L"BASSMIDI" );
                AddTab( hEngines, RenderBackends::EngineKestrel, L"Kestrel" );
                AddTab( hEngines, RenderBackends::EngineMerlin, L"Merlin" );
                for ( auto &v : g_avPanel ) v.clear();
                int iPanel = -1;
                for ( HWND h = GetWindow( hWnd, GW_CHILD ); h; h = GetWindow( h, GW_HWNDNEXT ) )
                {
                    const int iId = GetDlgCtrlID( h );
                    if ( iId >= IDC_PANEL_KDMAPI && iId <= IDC_PANEL_MERLIN ) iPanel = iId - IDC_PANEL_KDMAPI;
                    else if ( iId == IDC_PANEL_END ) iPanel = -1;
                    else if ( iPanel >= 0 ) g_avPanel[iPanel].push_back( h );
                }
                SetWindowPos( hEngines, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
                SetWindowPos( hTabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
                const AudioSettings a = Config::GetConfig().CopyAudioSettings();
                TabCtrl_SetCurSel( hTabs, CurrentOutput() );
                TabCtrl_SetCurSel( hEngines, std::clamp( a.eRenderEngine, 0, 2 ) );
                ShowAudioPanels( hWnd );
                AddTips( hWnd, {
                    { IDC_AUDIO_TABS, L"Choosing a tab switches to that output at once. KDMAPI: OmniMIDI's direct interface. WinMM: a MIDI output "
                                      L"device. Pre-Render: the song made into audio ahead of where it plays, so even the densest passages play "
                                      L"every note (a seek far away or a change of speed makes it again from there)." },
                    { IDC_PERF_LONGSENDS, L"Raw MIDI, one long message per batch of events." },
                    { IDC_MIDIOUT, L"The device chosen is in use at once." },
                    { IDC_PR_TABS, L"The engine that makes the audio: choosing a tab switches to it at once. BASSMIDI runs on the processor, "
                                   L"continuously; Kestrel on the graphics card, continuously; Merlin on the graphics card in pieces, never dropping "
                                   L"a note. The notes are the ones Performance > Sending's velocity floor, collapsing and send limit leave." },
                    { IDC_RA_AHEAD, L"5 to 43200 seconds (12 hours). Past 10 minutes ahead, it is kept on disk. What has played is thrown away "
                                    L"at once, so going back makes it again." },
                    { IDC_RA_DEGRADE, L"When the audio would not be made in time, it is made lighter: BASSMIDI skips its quietest notes as it "
                                      L"plays (never all of them), Kestrel and Merlin lower the voice cap. Made again in full once well ahead." },
                    { IDC_RA_LIMITER, L"Keeps the joined audio under full scale." },
                    { IDC_RA_VOICES, L"Each instance's: as many as the threads, or 16 times them when split by channel." },
                    { IDC_RA_THREADS, L"The song is split by key over this many BASSMIDI instances, each made on a thread of its own with the voices "
                                      L"set (one keeps up with a few thousand)." },
                    { IDC_RA_BYCHANNEL, L"16 times the instances, each with the voices set. Every channel's keys are spread over all of them, and one "
                                        L"key's channels go to different instances, so a busy channel or key fills many instances. Instances that get "
                                        L"no notes cost nothing. BASSMIDI renders many small instances faster than a few large ones." },
                    { IDC_RA_NPS, L"Separate from Performance > Sending's send limit: each instance (as many as the threads, or 16 times them split by channel) "
                                  L"limits its own notes a second, as OmniMIDI's limiter does, the quietest going first. 0: no limit." },
                    { IDC_RA_FX, L"BASSMIDI's reverb and chorus." },
                    { IDC_RA_KVOICES, L"Kestrel's voices in all." },
                    { IDC_RA_GPUSHARE, L"10 to 100. The renderer uses at most this % of the graphics card's time once 5 seconds are made ahead; "
                                       L"the rest is the visuals'. Kestrel's and Merlin's (one setting)." },
                    { IDC_RA_GPUSHARE2, L"10 to 100. The renderer uses at most this % of the graphics card's time once 5 seconds are made ahead; "
                                        L"the rest is the visuals'. Kestrel's and Merlin's (one setting)." },
                    { IDC_RA_LOSSLESS, L"Identical notes (channel, key, velocity, tick) sent once with their count: Kestrel's copies that also end "
                                       L"together are one voice at their summed loudness. Kestrel's and Merlin's (one setting)." },
                    { IDC_RA_LOSSLESS2, L"Identical notes (channel, key, velocity, tick) sent once with their count: Merlin reads Spiderweb's repeat "
                                        L"marks. Kestrel's and Merlin's (one setting)." },
                    { IDC_RA_ADVANCED, L"Kestrel's voice rules, soundfont and output settings, and performance." },
                    { IDC_RA_ADVANCED2, L"The soundfont and output settings Merlin reads (Kestrel's too)." },
                } );
                ShowDevices( hWnd );
                ShowAudio( hWnd, a, Config::GetConfig().GetPerformanceSettings(), true );
                ShowRenderNotes( hWnd );
                EnableRenderButtons( hWnd );
                return TRUE;
            }
            case WM_DEVICECHANGE:
                Sleep( 200 );
                ShowDevices( hWnd );
                break;
            case WM_COMMAND:
            {
                const WORD wId = LOWORD( wParam ), wCode = HIWORD( wParam );
                HWND hList = GetDlgItem( hWnd, IDC_RA_FONTS );
                if ( wId == IDC_MIDIOUT && wCode == LBN_SELCHANGE ) { SetDevice( hWnd ); return TRUE; }
                // (the two of a pair kept the same)
                if ( ( wId == IDC_RA_LOSSLESS || wId == IDC_RA_LOSSLESS2 ) && wCode == BN_CLICKED )
                    Check( hWnd, wId == IDC_RA_LOSSLESS ? IDC_RA_LOSSLESS2 : IDC_RA_LOSSLESS, Checked( hWnd, wId ) );
                if ( ( wId == IDC_RA_GPUSHARE || wId == IDC_RA_GPUSHARE2 ) && wCode == EN_CHANGE && !g_bSyncing )
                {
                    wchar_t buf[16] = L"";
                    GetDlgItemTextW( hWnd, wId, buf, 16 );
                    g_bSyncing = true;
                    SetDlgItemTextW( hWnd, wId == IDC_RA_GPUSHARE ? IDC_RA_GPUSHARE2 : IDC_RA_GPUSHARE, buf );
                    g_bSyncing = false;
                }
                if ( ( ( wId == IDC_RA_VOICES || wId == IDC_RA_THREADS ) && wCode == EN_CHANGE ) || ( wId == IDC_RA_BYCHANNEL && wCode == BN_CLICKED ) )
                    ShowVoiceLimit( hWnd );
                if ( ( wId == IDC_RA_ADVANCED || wId == IDC_RA_ADVANCED2 ) && wCode == BN_CLICKED )
                {
                    if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( wId == IDC_RA_ADVANCED ? IDD_KESTRELADV : IDD_MERLINADV ),
                                          GetParent( hWnd ), KestrelAdvProc, 0 ) == IDOK )
                        Changed( hWnd );
                    return TRUE;
                }
                else if ( wId == IDC_RA_FONTS && wCode == LBN_SELCHANGE ) { EnableRenderButtons( hWnd ); return TRUE; }
                else if ( wId == IDC_RA_ADD && wCode == BN_CLICKED )
                {
                    std::vector< wchar_t > vBuf( 65536, L'\0' );
                    OPENFILENAMEW ofn = {};
                    ofn.lStructSize = sizeof( ofn );
                    ofn.hwndOwner = hWnd;
                    ofn.lpstrFilter = L"Soundfonts (*.sf2, *.sf3, *.sfz, *.sf2pack)\0*.sf2;*.sf3;*.sfz;*.sf2pack\0All files (*.*)\0*.*\0";
                    ofn.lpstrFile = vBuf.data();
                    ofn.nMaxFile = (DWORD)vBuf.size();
                    ofn.lpstrTitle = L"Add soundfonts";
                    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;
                    if ( GetOpenFileNameW( &ofn ) )
                    {
                        // One file: its path; several: the folder, then each name
                        std::wstring sFirst( vBuf.data() );
                        const wchar_t *p = vBuf.data() + sFirst.size() + 1;
                        if ( !*p ) SendMessageW( hList, LB_ADDSTRING, 0, ( LPARAM )sFirst.c_str() );
                        for ( ; *p; p += wcslen( p ) + 1 )
                        {
                            std::wstring s = sFirst;
                            if ( !s.empty() && s.back() != L'\\' ) s += L'\\';
                            s += p;
                            SendMessageW( hList, LB_ADDSTRING, 0, ( LPARAM )s.c_str() );
                        }
                        FitRenderList( hList );
                        EnableRenderButtons( hWnd );
                        Changed( hWnd );
                    }
                    return TRUE;
                }
                else if ( ( wId == IDC_RA_REMOVE || wId == IDC_RA_UP || wId == IDC_RA_DOWN ) && wCode == BN_CLICKED )
                {
                    int i = (int)SendMessageW( hList, LB_GETCURSEL, 0, 0 ), n = (int)SendMessageW( hList, LB_GETCOUNT, 0, 0 );
                    if ( i < 0 ) return TRUE;
                    if ( wId == IDC_RA_REMOVE )
                    {
                        SendMessageW( hList, LB_DELETESTRING, i, 0 );
                        if ( n > 1 ) SendMessageW( hList, LB_SETCURSEL, std::min( i, n - 2 ), 0 );
                    }
                    else
                    {
                        int j = wId == IDC_RA_UP ? i - 1 : i + 1;
                        if ( j < 0 || j >= n ) return TRUE;
                        std::wstring s = ListText( hList, i );
                        SendMessageW( hList, LB_DELETESTRING, i, 0 );
                        SendMessageW( hList, LB_INSERTSTRING, j, ( LPARAM )s.c_str() );
                        SendMessageW( hList, LB_SETCURSEL, j, 0 );
                    }
                    FitRenderList( hList );
                    EnableRenderButtons( hWnd );
                    Changed( hWnd );
                    return TRUE;
                }
                else if ( wId == IDC_RESTOREDEFAULTS && wCode == BN_CLICKED )
                {
                    // (the soundfonts, the output and the engine kept)
                    AudioSettings d;
                    d.LoadDefaultValues();
                    PerformanceSettings cPerf;
                    cPerf.LoadDefaultValues();
                    ShowAudio( hWnd, d, cPerf, false );
                    Changed( hWnd );
                    return TRUE;
                }
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                break;
            }
            case WM_NOTIFY:
            {
                LPNMHDR pnm = ( LPNMHDR )lParam;
                if ( pnm->code == TCN_SELCHANGE && ( pnm->idFrom == IDC_AUDIO_TABS || pnm->idFrom == IDC_PR_TABS ) )
                {
                    const int i = TabCtrl_GetCurSel( pnm->hwndFrom );
                    if ( pnm->idFrom == IDC_AUDIO_TABS ) SetOutput( i );
                    else SetEngine( i );
                    ShowAudioPanels( hWnd );
                    ShowRenderNotes( hWnd );
                    return TRUE;
                }
                if ( pnm->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    AudioSettings a = config.CopyAudioSettings();
                    BOOL bOk = FALSE;
                    UINT u = GetDlgItemInt( hWnd, IDC_RA_VOICES, &bOk, FALSE );
                    if ( bOk ) a.iBassVoices = std::clamp( (int)std::min( u, 1000000u ), 1, 100000 );
                    SetDlgItemInt( hWnd, IDC_RA_VOICES, a.iBassVoices, FALSE );
                    u = GetDlgItemInt( hWnd, IDC_RA_KVOICES, &bOk, FALSE );
                    if ( bOk ) a.iKestrelVoices = std::clamp( (int)std::min( u, 1u << 30 ), 1, 1 << 24 );
                    SetDlgItemInt( hWnd, IDC_RA_KVOICES, a.iKestrelVoices, FALSE );
                    UINT uAhead = GetDlgItemInt( hWnd, IDC_RA_AHEAD, &bOk, FALSE );
                    if ( bOk ) a.iAheadSecs = std::clamp( (int)std::min( uAhead, 1000000u ), RenderAhead::Settings::MinAheadSecs, RenderAhead::Settings::MaxAheadSecs );
                    SetDlgItemInt( hWnd, IDC_RA_AHEAD, a.iAheadSecs, FALSE );
                    UINT uNps = GetDlgItemInt( hWnd, IDC_RA_NPS, &bOk, FALSE );
                    if ( bOk ) a.iBassNps = (int)std::min< UINT >( uNps, 1000000000u );
                    SetDlgItemInt( hWnd, IDC_RA_NPS, (UINT)a.iBassNps, FALSE );
                    a.bBassByChannel = Checked( hWnd, IDC_RA_BYCHANNEL );
                    a.bLimiter = Checked( hWnd, IDC_RA_LIMITER );
                    a.bRenderFx = Checked( hWnd, IDC_RA_FX );
                    UINT uThreads = GetDlgItemInt( hWnd, IDC_RA_THREADS, &bOk, FALSE );
                    if ( bOk ) a.iBassThreads = std::clamp( (int)std::min( uThreads, 1000u ), 1, 64 );
                    SetDlgItemInt( hWnd, IDC_RA_THREADS, a.iBassThreads, FALSE );
                    UINT uShare = GetDlgItemInt( hWnd, IDC_RA_GPUSHARE, &bOk, FALSE );
                    if ( bOk ) a.iGpuShare = std::clamp( (int)std::min( uShare, 1000u ), 10, 100 );
                    g_bSyncing = true;
                    SetDlgItemInt( hWnd, IDC_RA_GPUSHARE, a.iGpuShare, FALSE );
                    SetDlgItemInt( hWnd, IDC_RA_GPUSHARE2, a.iGpuShare, FALSE );
                    g_bSyncing = false;
                    a.kb = g_RenderKb;
                    HWND hList = GetDlgItem( hWnd, IDC_RA_FONTS );
                    a.sSoundfonts.clear();
                    for ( int i = 0, n = (int)SendMessageW( hList, LB_GETCOUNT, 0, 0 ); i < n; i++ )
                    {
                        if ( i ) a.sSoundfonts += L'|';
                        a.sSoundfonts += ListText( hList, i );
                    }
                    a.bRenderLossless = Checked( hWnd, IDC_RA_LOSSLESS );
                    a.bRenderDegrade = Checked( hWnd, IDC_RA_DEGRADE );
                    config.SetAudioSettings( a );
                    PerformanceSettings cPerf = config.GetPerformanceSettings();
                    cPerf.bKdmapiLongSends = Checked( hWnd, IDC_PERF_LONGSENDS );
                    config.SetPerformanceSettings( cPerf );
                    RenderAhead::PreloadFromConfig();   // (the chosen soundfonts loaded at once, song or not)
                    return Applied( hWnd );
                }
                break;
            }
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Effects (EffectSettings; visual/Effects.h)
    //-------------------------------------------------------------------------

    // Each number's setting (EffectSettings' parts are arrays)
    struct FxNumber { int iId; int &( *Get )( EffectSettings & ); int iMin, iMax; const wchar_t *sName; };
#define FX( m ) []( EffectSettings &fx ) -> int & { return fx.m; }
    const FxNumber aFxNumbers[] = {
        { IDC_FX_RESTLIGHT, FX( iRestLight ), 0, 100, L"the notes' resting light (0 to 100%)" },
        { IDC_FX_LIGHTOUTLINES, FX( iLightOutlines ), 0, 300, L"the outlines' share of the light (0 to 300%)" },
        { IDC_FX_LIGHTINSIDES, FX( iLightInsides ), 0, 100, L"the insides' share of the light (0 to 100%)" },
        { IDC_FX_DECAYMS, FX( iDecayMs ), 1, 60000, L"the fall's time (1 to 60000 ms)" },
        { IDC_FX_FLOOR, FX( iDecayFloor ), 0, 100, L"the light left after the fall (0 to 100%)" },
        { IDC_FX_RELEASEMS, FX( iReleaseMs ), 0, 60000, L"the let-go keys' fade (0 to 60000 ms)" },
        { IDC_FX_DARKNOTES, FX( aDark[EffectSettings::PartNotes] ), 0, 100, L"the notes' darkening (0 to 100%)" },
        { IDC_FX_DARKSHORT, FX( aDark[EffectSettings::PartShort] ), 0, 100, L"the short notes' darkening (0 to 100%)" },
        { IDC_FX_DARKKEYS, FX( aDark[EffectSettings::PartKeys] ), 0, 100, L"the keys' darkening (0 to 100%)" },
        { IDC_FX_DARKBKG, FX( aDark[EffectSettings::PartBackground] ), 0, 100, L"the background's darkening (0 to 100%)" },
        { IDC_FX_UPNOTES, FX( aLightUp[EffectSettings::PartNotes] ), 0, 100, L"the notes' lightening (0 to 100%)" },
        { IDC_FX_UPSHORT, FX( aLightUp[EffectSettings::PartShort] ), 0, 100, L"the short notes' lightening (0 to 100%)" },
        { IDC_FX_UPKEYS, FX( aLightUp[EffectSettings::PartKeys] ), 0, 100, L"the keys' lightening (0 to 100%)" },
        { IDC_FX_GLOWNOTES, FX( aGlow[EffectSettings::PartNotes] ), 0, 300, L"the notes' glow (0 to 300%)" },
        { IDC_FX_GLOWSHORT, FX( aGlow[EffectSettings::PartShort] ), 0, 300, L"the short notes' glow (0 to 300%)" },
        { IDC_FX_GLOWKEYS, FX( aGlow[EffectSettings::PartKeys] ), 0, 300, L"the keys' glow (0 to 300%)" },
        { IDC_FX_GLOWBKG, FX( aGlow[EffectSettings::PartBackground] ), 0, 300, L"the whole frame's glow (0 to 300%)" },
        { IDC_FX_DARKBAR, FX( aDark[EffectSettings::PartBar] ), 0, 100, L"the red bar's darkening (0 to 100%)" },
        { IDC_FX_UPBAR, FX( aLightUp[EffectSettings::PartBar] ), 0, 100, L"the red bar's lightening (0 to 100%)" },
        { IDC_FX_GLOWBAR, FX( aGlow[EffectSettings::PartBar] ), 0, 300, L"the red bar's glow (0 to 300%)" },
        { IDC_FX_BLOOM, FX( iBloom ), 0, 400, L"the bloom's strength (0 to 400%)" },
        { IDC_FX_RADIUS, FX( iBloomRadius ), 1, 200, L"the bloom's radius (1 to 200 pixels)" },
        { IDC_FX_THRESHOLD, FX( iBloomThreshold ), 0, 99, L"the bloom's threshold (0 to 99%)" },
        { IDC_FX_BLOOMLIMIT, FX( iBloomLimit ), 0, 1000, L"the bloom's soft limit (0 to 1000%)" },
        { IDC_FX_EXPOSURE, FX( iExposure ), 0, 100, L"the auto exposure's strength (0 to 100%)" },
        { IDC_FX_EXPOSUREMS, FX( iExposureMs ), 0, 60000, L"the auto exposure's time (0 to 60000 ms)" },
        { IDC_FX_VIGNETTE, FX( iVignette ), 0, 100, L"the vignette (0 to 100%)" },
        { IDC_FX_VIGFADE, FX( iVignetteFade ), 1, 100, L"how far in the vignette fades out (1 to 100%)" },
        { IDC_FX_HITSIZE, FX( iHitLineSize ), 0, 200, L"the hit line's thickness (0 to 200 pixels)" },
        { IDC_FX_HITSOFT, FX( iHitLineSoft ), 0, 400, L"the hit line's fade (0 to 400 pixels)" },
        { IDC_FX_HITSTRENGTH, FX( iHitLineStrength ), 0, 100, L"the hit line's strength (0 to 100%)" },
        { IDC_FX_HITGLOW, FX( iHitLineGlow ), 0, 300, L"the hit line's glow (0 to 300%)" },
        { IDC_FX_HITFADEMS, FX( iHitLineFadeMs ), 0, 60000, L"the hit line's fade (0 to 60000 ms)" },
        { IDC_FX_HITSMEAR, FX( iHitLineSmear ), 0, 400, L"the hit line's smear (0 to 400 pixels)" },
        { IDC_FX_SEMIOUTLINE, FX( iSemiFlatOutline ), 0, 100, L"how much darker the semi-flat look's outlines are (0 to 100%)" },
    };
#undef FX

    // The fade's times and floor mean nothing without a curve
    VOID EnableFxDecay( HWND hWnd )
    {
        BOOL bOn = SendDlgItemMessageW( hWnd, IDC_FX_DECAY, CB_GETCURSEL, 0, 0 ) > 0;
        for ( int iId : { IDC_FX_DECAYMS, IDC_FX_FLOOR, IDC_FX_RELEASEMS } ) EnableWindow( GetDlgItem( hWnd, iId ), bOn );
    }

    // The hit line's settings (its dialog) mean nothing without it
    VOID EnableFxHitLine( HWND hWnd )
    {
        BOOL bOn = IsDlgButtonChecked( hWnd, IDC_FX_HITLINE ) == BST_CHECKED;
        EnableWindow( GetDlgItem( hWnd, IDC_FX_HITADV ), bOn );
    }

    // The look chosen (EffectSettings::Look)
    int FxLook( HWND hWnd )
    {
        return std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_FX_FLAT, CB_GETCURSEL, 0, 0 ), 0, EffectSettings::LookCount - 1 );
    }

    // Each look's own settings mean nothing without it (and merged outlines nothing with the flat look: it
    // has none). The look's whether the effects are on or not; the merging's only with them on
    VOID EnableFxFlat( HWND hWnd )
    {
        BOOL bOn = FxLook( hWnd ) == EffectSettings::LookFlat;
        EnableWindow( GetDlgItem( hWnd, IDC_FX_FLATBORDERS ), bOn );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_SEMIOUTLINE ), FxLook( hWnd ) == EffectSettings::LookSemiFlat );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_SEMILIP ), FxLook( hWnd ) == EffectSettings::LookSemiFlat );
        if ( IsDlgButtonChecked( hWnd, IDC_FX_ON ) != BST_CHECKED ) return;
        const BOOL bMerge = !bOn && IsDlgButtonChecked( hWnd, IDC_FX_MERGE ) == BST_CHECKED;
        const BOOL bShort = bMerge && IsDlgButtonChecked( hWnd, IDC_FX_MERGESHORT ) == BST_CHECKED;
        EnableWindow( GetDlgItem( hWnd, IDC_FX_MERGE ), !bOn );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_MERGESHORT ), bMerge );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_MERGESHORTLONG ), bShort );
    }

    // Pressed keys whitening: each look its own, the row showing the look chosen (g_iKeyWhiteLook)
    bool g_abKeyWhite[EffectSettings::LookCount];
    int g_aiKeyWhite[EffectSettings::LookCount], g_iKeyWhiteLook;

    VOID ShowKeyWhite( HWND hWnd )
    {
        CheckDlgButton( hWnd, IDC_FX_KEYWHITE, g_abKeyWhite[g_iKeyWhiteLook] ? BST_CHECKED : BST_UNCHECKED );
        SetDlgItemInt( hWnd, IDC_FX_KEYWHITEPCT, g_aiKeyWhite[g_iKeyWhiteLook], FALSE );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_KEYWHITEPCT ), IsWindowEnabled( GetDlgItem( hWnd, IDC_FX_KEYWHITE ) ) && g_abKeyWhite[g_iKeyWhiteLook] );
    }

    VOID KeepKeyWhite( HWND hWnd )
    {
        BOOL bOk;
        UINT u = GetDlgItemInt( hWnd, IDC_FX_KEYWHITEPCT, &bOk, FALSE );
        g_abKeyWhite[g_iKeyWhiteLook] = IsDlgButtonChecked( hWnd, IDC_FX_KEYWHITE ) == BST_CHECKED;
        if ( bOk ) g_aiKeyWhite[g_iKeyWhiteLook] = (int)std::min( u, 100u );
    }

    // A control of the look (the box above the effects' switch, and what is in it): used with the effects off too
    bool IsLookControl( HWND hWnd, HWND hCtl )
    {
        RECT rcCtl, rcOn;
        GetWindowRect( hCtl, &rcCtl );
        GetWindowRect( GetDlgItem( hWnd, IDC_FX_ON ), &rcOn );
        return rcCtl.top < rcOn.top;
    }

    // The effects by the tab's switch (the look and the profiles always usable); each setting as the ones
    // it depends on allow
    VOID EnableFx( HWND hWnd )
    {
        const BOOL bOn = IsDlgButtonChecked( hWnd, IDC_FX_ON ) == BST_CHECKED;
        for ( HWND h = GetWindow( hWnd, GW_CHILD ); h; h = GetWindow( h, GW_HWNDNEXT ) )
        {
            const int iId = GetDlgCtrlID( h );
            if ( iId != IDC_FX_ON && ( iId < IDC_FX_PROFILES || iId > IDC_FX_PROFILENEW ) && iId != IDC_FX_PROFILEKEEP && h != GetWindow( GetDlgItem( hWnd, IDC_FX_PROFILES ), GW_HWNDPREV ) )
                EnableWindow( h, bOn || IsLookControl( hWnd, h ) );
        }
        EnableFxFlat( hWnd );
        ShowKeyWhite( hWnd );
        if ( bOn )
        {
            EnableFxDecay( hWnd );
            EnableFxHitLine( hWnd );
        }
    }

    // The page's settings as shown (what disabled ones keep), and the saved profiles as edited (kept on
    // Apply)
    EffectSettings g_FxShown;
    std::vector< VisualSettings::FxProfile > g_vFxProfiles;

    // The profiles kept at once (saving or deleting one is not a change of the page's settings, to apply)
    VOID KeepFxProfiles()
    {
        Config &config = Config::GetConfig();
        VisualSettings cVisual = config.GetVisualSettings();
        cVisual.vFxProfiles = g_vFxProfiles;
        config.SetVisualSettings( cVisual );
    }

    VOID UpdateFxSave( HWND hWnd );

    VOID ShowFxProfiles( HWND hWnd, int iSelect )
    {
        HWND hCombo = GetDlgItem( hWnd, IDC_FX_PROFILES );
        SendMessageW( hCombo, CB_RESETCONTENT, 0, 0 );
        for ( const VisualSettings::FxProfile &p : g_vFxProfiles ) SendMessageW( hCombo, CB_ADDSTRING, 0, ( LPARAM )p.sName.c_str() );
        SendMessageW( hCombo, CB_SETCURSEL, iSelect, 0 );
        EnableWindow( hCombo, !g_vFxProfiles.empty() );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_PROFILEDELETE ), iSelect >= 0 );
        UpdateFxSave( hWnd );
    }

    // A profile's settings as text: every effect setting, without the switch and the look chosen (which
    // profiles leave alone; each look's own settings are in them)
    std::string FxProfileText( EffectSettings f )
    {
        f.eLook = EffectSettings::LookNormal;
        f.bOn = true;
        IniFile ini;
        f.Save( ini );
        return ini.Text();
    }

    // The profile with every effect setting as fx's (the switch and the look chosen aside), or -1
    int MatchingFxProfile( const EffectSettings &fx )
    {
        auto Text = FxProfileText;
        const std::string s = Text( fx );
        for ( size_t i = 0; i < g_vFxProfiles.size(); i++ )
            if ( Text( g_vFxProfiles[i].fx ) == s ) return (int)i;
        return -1;
    }

    // The lists' choices, in the page or a dialog (each control that is there)
    VOID FillFxCombos( HWND hWnd )
    {
        // By EffectSettings::Look and Decay
        for ( const wchar_t *s : { L"Normal", L"Flat", L"Semi-flat", L"Kiva" } )
            SendDlgItemMessageW( hWnd, IDC_FX_FLAT, CB_ADDSTRING, 0, ( LPARAM )s );
        // (EffectSettings::bNoBar, bFlatTop)
        for ( const wchar_t *s : { L"The red bar", L"Nothing", L"Nothing, keys cut off flat (UMP)" } )
            SendDlgItemMessageW( hWnd, IDC_FX_NOBAR, CB_ADDSTRING, 0, ( LPARAM )s );
        for ( const wchar_t *s : { L"None (steady)", L"Linear", L"Exponential", L"Smooth" } )
            SendDlgItemMessageW( hWnd, IDC_FX_DECAY, CB_ADDSTRING, 0, ( LPARAM )s );
        for ( const wchar_t *s : { L"None (off at once)", L"Linear", L"Exponential", L"Smooth" } )
            SendDlgItemMessageW( hWnd, IDC_FX_HITCURVE, CB_ADDSTRING, 0, ( LPARAM )s );
        // By EffectSettings::VignetteSide
        for ( const wchar_t *s : { L"every edge", L"the top", L"the bottom", L"the left", L"the right" } )
            SendDlgItemMessageW( hWnd, IDC_FX_VIGSIDE, CB_ADDSTRING, 0, ( LPARAM )s );
    }

    // The controls there are (the page's, or a dialog's) from fx, but the effects' switch
    VOID ShowFxControls( HWND hWnd, const EffectSettings &fx )
    {
                SendDlgItemMessageW( hWnd, IDC_FX_FLAT, CB_SETCURSEL, fx.eLook, 0 );
                CheckDlgButton( hWnd, IDC_FX_FLATBORDERS, fx.bFlatBorders ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_SEMILIP, fx.bSemiFlatLip ? BST_CHECKED : BST_UNCHECKED );
                SendDlgItemMessageW( hWnd, IDC_FX_NOBAR, CB_SETCURSEL, !fx.bNoBar ? 0 : fx.bFlatTop ? 2 : 1, 0 );
                CheckDlgButton( hWnd, IDC_FX_SHORTCOLOR, fx.bShortColor ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_HOLLOW, fx.bHollow ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_MERGE, fx.bMergeOutlines ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_MERGESHORT, fx.bMergeShort ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_MERGESHORTLONG, fx.bMergeShortLong ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_STATS, fx.bStats ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_HITLINE, fx.bHitLine ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_HITPLAYING, fx.bHitLinePlaying ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_HITNOTECOLOR, fx.bHitLineNoteColor ? BST_CHECKED : BST_UNCHECKED );
                SetWindowLongPtrW( GetDlgItem( hWnd, IDC_FX_HITCOLOR ), GWLP_USERDATA, fx.iHitLineColor );
                InvalidateRect( GetDlgItem( hWnd, IDC_FX_HITCOLOR ), nullptr, FALSE );
                for ( const FxNumber &n : aFxNumbers ) SetDlgItemInt( hWnd, n.iId, (UINT)n.Get( const_cast< EffectSettings & >( fx ) ), FALSE );
                SendDlgItemMessageW( hWnd, IDC_FX_DECAY, CB_SETCURSEL, fx.eDecay, 0 );
                SendDlgItemMessageW( hWnd, IDC_FX_HITCURVE, CB_SETCURSEL, fx.eHitLineCurve, 0 );
                SendDlgItemMessageW( hWnd, IDC_FX_VIGSIDE, CB_SETCURSEL, fx.eVignetteSide, 0 );
                CheckDlgButton( hWnd, IDC_FX_VIGNOTES, fx.bVignetteNotes ? BST_CHECKED : BST_UNCHECKED );
    }

    // Every setting's control on the page but the effects' switch, from fx (and what its dialogs show)
    VOID ShowFx( HWND hWnd, const EffectSettings &fx )
    {
        g_FxShown = fx;
        ShowFxControls( hWnd, fx );
        std::copy( fx.abKeyWhite, fx.abKeyWhite + EffectSettings::LookCount, g_abKeyWhite );
        std::copy( fx.aiKeyWhite, fx.aiKeyWhite + EffectSettings::LookCount, g_aiKeyWhite );
        g_iKeyWhiteLook = fx.eLook;
        ShowKeyWhite( hWnd );
        EnableFx( hWnd );
        UpdateFxSave( hWnd );
    }

    // The controls over fx (a disabled number keeps fx's); a number out of its range: that one, and
    // nothing changed
    // The controls there are (the page's, or a dialog's) over fx (a disabled number keeps fx's); a number out
    // of its range: that one, and nothing changed
    const FxNumber *ReadFx( HWND hWnd, EffectSettings &fxOut )
    {
        EffectSettings fx = fxOut;
        for ( const FxNumber &n : aFxNumbers )
        {
            BOOL bOk;
            UINT u = GetDlgItemInt( hWnd, n.iId, &bOk, FALSE );
            if ( !IsWindowEnabled( GetDlgItem( hWnd, n.iId ) ) ) continue;
            if ( !bOk || u < (UINT)n.iMin || u > (UINT)n.iMax ) return &n;
            n.Get( fx ) = (int)u;
        }
        auto Has = [&]( int iId ) { return GetDlgItem( hWnd, iId ) != nullptr; };
        auto Box = [&]( int iId, bool &b ) { if ( Has( iId ) ) b = Checked( hWnd, iId ); };
        auto Pick = [&]( int iId, int &e ) { if ( Has( iId ) ) e = std::max( 0, (int)SendDlgItemMessageW( hWnd, iId, CB_GETCURSEL, 0, 0 ) ); };
        if ( Has( IDC_FX_FLAT ) ) fx.eLook = FxLook( hWnd );
        Box( IDC_FX_FLATBORDERS, fx.bFlatBorders );
        Box( IDC_FX_SEMILIP, fx.bSemiFlatLip );
        if ( Has( IDC_FX_NOBAR ) )
        {
            const int iBar = (int)SendDlgItemMessageW( hWnd, IDC_FX_NOBAR, CB_GETCURSEL, 0, 0 );
            fx.bNoBar = iBar >= 1;
            fx.bFlatTop = iBar == 2;
        }
        Box( IDC_FX_SHORTCOLOR, fx.bShortColor );
        Box( IDC_FX_HOLLOW, fx.bHollow );
        Box( IDC_FX_MERGE, fx.bMergeOutlines );
        Box( IDC_FX_MERGESHORT, fx.bMergeShort );
        Box( IDC_FX_MERGESHORTLONG, fx.bMergeShortLong );
        Box( IDC_FX_STATS, fx.bStats );
        Box( IDC_FX_ON, fx.bOn );
        if ( Has( IDC_FX_KEYWHITE ) )
        {
            KeepKeyWhite( hWnd );
            std::copy( g_abKeyWhite, g_abKeyWhite + EffectSettings::LookCount, fx.abKeyWhite );
            std::copy( g_aiKeyWhite, g_aiKeyWhite + EffectSettings::LookCount, fx.aiKeyWhite );
        }
        Box( IDC_FX_HITLINE, fx.bHitLine );
        Box( IDC_FX_HITPLAYING, fx.bHitLinePlaying );
        Box( IDC_FX_HITNOTECOLOR, fx.bHitLineNoteColor );
        if ( Has( IDC_FX_HITCOLOR ) ) fx.iHitLineColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_FX_HITCOLOR ), GWLP_USERDATA );
        Pick( IDC_FX_DECAY, fx.eDecay );
        Pick( IDC_FX_HITCURVE, fx.eHitLineCurve );
        Pick( IDC_FX_VIGSIDE, fx.eVignetteSide );
        Box( IDC_FX_VIGNOTES, fx.bVignetteNotes );
        fxOut = fx;
        return nullptr;
    }

    // Save: only with a profile chosen that the page's settings differ from (a number out of range
    // counts as a difference, so saving says what is wrong)
    VOID UpdateFxSave( HWND hWnd )
    {
        const int i = (int)SendDlgItemMessageW( hWnd, IDC_FX_PROFILES, CB_GETCURSEL, 0, 0 );
        bool bDiffer = false;
        if ( i >= 0 && i < (int)g_vFxProfiles.size() )
        {
            auto Text = FxProfileText;
            EffectSettings fx = g_FxShown;
            bDiffer = ReadFx( hWnd, fx ) != nullptr || Text( fx ) != Text( g_vFxProfiles[i].fx );
        }
        EnableWindow( GetDlgItem( hWnd, IDC_FX_PROFILEKEEP ), bDiffer );
    }

    // The look the page shows (as far as its numbers read: past a bad one, the one it was shown with)
    EffectSettings PageLook( HWND hWnd )
    {
        EffectSettings fx = g_FxShown;
        if ( ReadFx( hWnd, fx ) ) fx = g_FxShown;
        return fx;
    }

    // A number out of range: said, and its box focused
    VOID FxNumberError( HWND hWnd, const FxNumber &n )
    {
        wchar_t buf[160];
        swprintf( buf, 160, L"Please specify %ls", n.sName );
        MessageBoxW( hWnd, buf, L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, n.iId ), TRUE );
    }

    // Each part, Bloom and Hit line: a dialog over a copy of the page's settings (its own controls only)
    INT_PTR CALLBACK FxAdvProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        EffectSettings *pFx = ( EffectSettings * )GetWindowLongPtrW( hWnd, DWLP_USER );
        switch ( msg )
        {
            case WM_INITDIALOG:
                SetWindowLongPtrW( hWnd, DWLP_USER, lParam );
                pFx = ( EffectSettings * )lParam;
                FillFxCombos( hWnd );
                ShowFxControls( hWnd, *pFx );
                return TRUE;
            case WM_DRAWITEM:
                if ( ( ( LPDRAWITEMSTRUCT )lParam )->CtlID != IDC_FX_HITCOLOR ) return FALSE;
                DrawSwatch( ( LPDRAWITEMSTRUCT )lParam );
                return TRUE;
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDC_FX_HITCOLOR:
                    {
                        if ( HIWORD( wParam ) != BN_CLICKED ) return TRUE;
                        HWND hWndBtn = ( HWND )lParam;
                        COLORREF cr = ( COLORREF )GetWindowLongPtrW( hWndBtn, GWLP_USERDATA );
                        if ( !PickColor( hWnd, cr ) ) return TRUE;
                        SetWindowLongPtrW( hWndBtn, GWLP_USERDATA, cr );
                        InvalidateRect( hWndBtn, nullptr, FALSE );
                        return TRUE;
                    }
                    case IDOK:
                    {
                        EffectSettings fx = *pFx;
                        if ( const FxNumber *pBad = ReadFx( hWnd, fx ) ) { FxNumberError( hWnd, *pBad ); return TRUE; }
                        *pFx = fx;
                        EndDialog( hWnd, IDOK );
                        return TRUE;
                    }
                    case IDCANCEL:
                        EndDialog( hWnd, IDCANCEL );
                        return TRUE;
                }
                break;
        }
        return FALSE;
    }

    INT_PTR CALLBACK EffectsProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
                FillFxCombos( hWnd );
                AddTips( hWnd, {
                    { IDC_FX_LIGHTOUTLINES, L"0 to 300%: 200 is the main colour, 300 white." },
                    { IDC_FX_PARTSADV, L"How much the notes, short notes, keys, red bar and background darken unlit, lighten lit, and glow." },
                    { IDC_FX_HITADV, L"Its colour, size, softness, strength, smear, glow and fade." },
                } );
                CheckDlgButton( hWnd, IDC_FX_ON, cVisual.fx.bOn ? BST_CHECKED : BST_UNCHECKED );
                g_vFxProfiles = cVisual.vFxProfiles;
                ShowFxProfiles( hWnd, MatchingFxProfile( cVisual.fx ) );
                ShowFx( hWnd, cVisual.fx );
                return TRUE;
            }
            case WM_DRAWITEM:
                if ( ( ( LPDRAWITEMSTRUCT )lParam )->CtlID != IDC_FX_HITCOLOR ) return FALSE;
                DrawSwatch( ( LPDRAWITEMSTRUCT )lParam );
                return TRUE;
            case WM_COMMAND:
                if ( LOWORD( wParam ) == IDC_FX_HITCOLOR && HIWORD( wParam ) == BN_CLICKED )
                {
                    HWND hWndBtn = ( HWND )lParam;
                    COLORREF cr = ( COLORREF )GetWindowLongPtrW( hWndBtn, GWLP_USERDATA ), crOld = cr;
                    if ( !PickColor( hWnd, cr ) ) return TRUE;
                    SetWindowLongPtrW( hWndBtn, GWLP_USERDATA, cr );
                    InvalidateRect( hWndBtn, nullptr, FALSE );
                    if ( cr != crOld ) Changed( hWnd );
                    UpdateFxSave( hWnd );
                    return TRUE;
                }
                // Saved profiles: one chosen fills the page (but the effects' switch); the page saved
                // under a name; the defaults shown
                switch ( LOWORD( wParam ) )
                {
                    // A dialog for some of the settings: what it changes is the page's to apply
                    case IDC_FX_PARTSADV: case IDC_FX_BLOOMADV: case IDC_FX_HITADV:
                    {
                        if ( HIWORD( wParam ) != BN_CLICKED ) return TRUE;
                        const int iDlg = LOWORD( wParam ) == IDC_FX_PARTSADV ? IDD_FX_PARTS : LOWORD( wParam ) == IDC_FX_BLOOMADV ? IDD_FX_BLOOM : IDD_FX_HITLINE;
                        EffectSettings fx = g_FxShown;
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( iDlg ), GetParent( hWnd ), FxAdvProc, ( LPARAM )&fx ) == IDOK )
                        {
                            auto Text = []( const EffectSettings &f ) { IniFile ini; f.Save( ini ); return ini.Text(); };
                            if ( Text( fx ) != Text( g_FxShown ) ) Changed( hWnd );
                            g_FxShown = fx;
                        }
                        UpdateFxSave( hWnd );
                        return TRUE;
                    }
                    case IDC_FX_PROFILES:
                    {
                        if ( HIWORD( wParam ) != CBN_SELCHANGE ) return TRUE;
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_FX_PROFILES, CB_GETCURSEL, 0, 0 );
                        if ( i >= 0 && i < (int)g_vFxProfiles.size() )
                        {
                            EffectSettings fx = g_vFxProfiles[i].fx;
                            fx.bOn = IsDlgButtonChecked( hWnd, IDC_FX_ON ) == BST_CHECKED;
                            fx.eLook = PageLook( hWnd ).eLook;
                            ShowFx( hWnd, fx );
                            Changed( hWnd );
                        }
                        EnableWindow( GetDlgItem( hWnd, IDC_FX_PROFILEDELETE ), i >= 0 );
                        UpdateFxSave( hWnd );
                        return TRUE;
                    }
                    case IDC_FX_PROFILEKEEP:
                    {
                        // (the page's settings over the profile chosen, under its own name)
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_FX_PROFILES, CB_GETCURSEL, 0, 0 );
                        if ( i < 0 || i >= (int)g_vFxProfiles.size() ) return TRUE;
                        EffectSettings fx = g_FxShown;
                        if ( const FxNumber *pBad = ReadFx( hWnd, fx ) ) { FxNumberError( hWnd, *pBad ); return TRUE; }
                        fx.bOn = true;
                        g_vFxProfiles[i].fx = fx;
                        KeepFxProfiles();
                        UpdateFxSave( hWnd );
                        return TRUE;
                    }
                    case IDC_FX_PROFILESAVE:
                    {
                        EffectSettings fx = g_FxShown;
                        if ( const FxNumber *pBad = ReadFx( hWnd, fx ) ) { FxNumberError( hWnd, *pBad ); return TRUE; }
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_FX_PROFILES, CB_GETCURSEL, 0, 0 );
                        std::wstring sName = i >= 0 && i < (int)g_vFxProfiles.size() ? g_vFxProfiles[i].sName : L"Profile " + std::to_wstring( g_vFxProfiles.size() + 1 );
                        g_bNameProfile = true;
                        const INT_PTR r = DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_PALETTENAME ), GetParent( hWnd ), PaletteNameProc, ( LPARAM )&sName );
                        g_bNameProfile = false;
                        if ( r != IDOK ) return TRUE;
                        fx.bOn = true;
                        VisualSettings::FxProfile prof = { sName, fx };
                        auto it = std::find_if( g_vFxProfiles.begin(), g_vFxProfiles.end(), [&]( const VisualSettings::FxProfile &p ) { return _wcsicmp( p.sName.c_str(), sName.c_str() ) == 0; } );
                        if ( it != g_vFxProfiles.end() ) *it = prof;
                        else it = g_vFxProfiles.insert( g_vFxProfiles.end(), prof );
                        ShowFxProfiles( hWnd, (int)( it - g_vFxProfiles.begin() ) );
                        KeepFxProfiles();
                        return TRUE;
                    }
                    case IDC_FX_PROFILEDELETE:
                    {
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_FX_PROFILES, CB_GETCURSEL, 0, 0 );
                        if ( i < 0 || i >= (int)g_vFxProfiles.size() ) return TRUE;
                        std::wstring sAsk = L"Delete the effects profile \"" + g_vFxProfiles[i].sName + L"\"? (The page keeps its settings.)";
                        if ( MessageBoxW( GetParent( hWnd ), sAsk.c_str(), L"Delete Profile", MB_YESNO | MB_ICONQUESTION ) != IDYES ) return TRUE;
                        g_vFxProfiles.erase( g_vFxProfiles.begin() + i );
                        ShowFxProfiles( hWnd, -1 );
                        KeepFxProfiles();
                        return TRUE;
                    }
                    case IDC_FX_PROFILENEW:
                    {
                        EffectSettings def;
                        def.LoadDefaultValues();
                        def.bOn = IsDlgButtonChecked( hWnd, IDC_FX_ON ) == BST_CHECKED;
                        def.eLook = PageLook( hWnd ).eLook;
                        ShowFx( hWnd, def );
                        ShowFxProfiles( hWnd, -1 );
                        Changed( hWnd );
                        return TRUE;
                    }
                }
                if ( LOWORD( wParam ) == IDC_FX_ON ) EnableFx( hWnd );
                if ( LOWORD( wParam ) == IDC_FX_KEYWHITE ) { KeepKeyWhite( hWnd ); ShowKeyWhite( hWnd ); }
                if ( LOWORD( wParam ) == IDC_FX_MERGE || LOWORD( wParam ) == IDC_FX_MERGESHORT ) EnableFxFlat( hWnd );
                if ( LOWORD( wParam ) == IDC_FX_FLAT && HIWORD( wParam ) == CBN_SELCHANGE )
                {
                    // (the other look's whitening shown)
                    KeepKeyWhite( hWnd );
                    g_iKeyWhiteLook = FxLook( hWnd );
                    ShowKeyWhite( hWnd );
                }
                if ( LOWORD( wParam ) == IDC_FX_HITLINE ) EnableFxHitLine( hWnd );
                if ( LOWORD( wParam ) == IDC_FX_DECAY && HIWORD( wParam ) == CBN_SELCHANGE ) EnableFxDecay( hWnd );
                if ( LOWORD( wParam ) == IDC_FX_FLAT && HIWORD( wParam ) == CBN_SELCHANGE )
                    EnableFxFlat( hWnd );
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                UpdateFxSave( hWnd );
                break;
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    VisualSettings cVisual = config.GetVisualSettings();
                    EffectSettings fx = g_FxShown;
                    if ( const FxNumber *pBad = ReadFx( hWnd, fx ) )
                    {
                        ShowPage( hWnd );
                        FxNumberError( hWnd, *pBad );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }
                    cVisual.fx = fx;
                    g_FxShown = fx;
                    cVisual.vFxProfiles = g_vFxProfiles;
                    config.SetVisualSettings( cVisual );
                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_NOERROR );
                    return TRUE;
                }
                break;
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Edits stepped by up-down arrows (the Interface page's)
    //-------------------------------------------------------------------------

    // Spin buttons step the edit's number by dStep
    VOID StepEdit( HWND hWndEdit, int iDelta, double dStep )
    {
        wchar_t buf[32];
        double dOld = 0;
        if ( GetWindowTextW( hWndEdit, buf, 32 ) > 0 && swscanf( buf, L"%lf", &dOld ) == 1 )
        {
            swprintf( buf, 32, L"%g", dOld - iDelta * dStep );
            SetWindowTextW( hWndEdit, buf );
        }
    }

    bool ReadEdit( HWND hWnd, int iId, double &dValue, const wchar_t *sError )
    {
        wchar_t buf[32];
        HWND hWndEdit = GetDlgItem( hWnd, iId );
        if ( GetWindowTextW( hWndEdit, buf, 32 ) > 0 && swscanf( buf, L"%lf", &dValue ) == 1 ) return true;
        Invalid( hWnd, iId, sError );
        return false;
    }

    //-------------------------------------------------------------------------
    // Interface (the theme, the window and toolbar, the frame rate, the arrow keys' steps)
    //-------------------------------------------------------------------------

    VOID ShowInterface( HWND hWnd, const VisualSettings &v, const VideoSettings &cVideo, const ControlsSettings &c, int eTheme )
    {
        SendDlgItemMessageW( hWnd, IDC_THEME, CB_SETCURSEL, eTheme, 0 );
        Check( hWnd, IDC_SHOWCONTROLS, v.bAlwaysShowControls );
        Check( hWnd, IDC_LIMITFPS, cVideo.bLimitFPS );
        wchar_t buf[32];
        swprintf( buf, 32, L"%g", c.dFwdBackSecs );
        SetDlgItemTextW( hWnd, IDC_LRARROWS, buf );
        swprintf( buf, 32, L"%g", c.dSpeedUpPct );
        SetDlgItemTextW( hWnd, IDC_UDARROWS, buf );
        swprintf( buf, 32, L"%g", c.dNSpeedUpPct );
        SetDlgItemTextW( hWnd, IDC_NSARROWS, buf );
    }

    INT_PTR CALLBACK InterfaceProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                Config &config = Config::GetConfig();
                // By DarkMode::Theme
                for ( const wchar_t *s : { L"Light", L"Dark", L"Follow Windows" } )
                    SendDlgItemMessageW( hWnd, IDC_THEME, CB_ADDSTRING, 0, ( LPARAM )s );
                ShowInterface( hWnd, config.GetVisualSettings(), config.GetVideoSettings(), config.GetControlsSettings(), config.GetViewSettings().GetTheme() );
                SetWindowSizeEdits( hWnd );
                AddTips( hWnd, {
                    { IDC_SHOWCONTROLS, L"Off: in full screen, the toolbar shows only while the mouse is near the top." },
                    { IDC_NSARROWS, L"The note speed: how fast the notes fall." },
                } );
                return TRUE;
            }
            case WM_COMMAND:
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                if ( LOWORD( wParam ) == IDC_RESTOREDEFAULTS )
                {
                    VisualSettings v;
                    v.LoadDefaultValues();
                    VideoSettings cVideo;
                    cVideo.LoadDefaultValues();
                    ControlsSettings c;
                    c.LoadDefaultValues();
                    ViewSettings cView;
                    cView.LoadDefaultValues();
                    ShowInterface( hWnd, v, cVideo, c, cView.GetTheme() );
                    Changed( hWnd );
                    return TRUE;
                }
                break;
            case WM_NOTIFY:
            {
                LPNMHDR lpnmhdr = ( LPNMHDR )lParam;
                if ( lpnmhdr->code == UDN_DELTAPOS )
                {
                    LPNMUPDOWN lpnmud = ( LPNMUPDOWN )lParam;
                    if ( lpnmhdr->idFrom == IDC_LRARROWSSPIN ) StepEdit( GetDlgItem( hWnd, IDC_LRARROWS ), lpnmud->iDelta, 0.1 );
                    else if ( lpnmhdr->idFrom == IDC_UDARROWSSPIN ) StepEdit( GetDlgItem( hWnd, IDC_UDARROWS ), lpnmud->iDelta, 1.0 );
                    else if ( lpnmhdr->idFrom == IDC_NSARROWSSPIN ) StepEdit( GetDlgItem( hWnd, IDC_NSARROWS ), lpnmud->iDelta, 1.0 );
                    return TRUE;
                }
                if ( lpnmhdr->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    ViewSettings &cView = config.GetViewSettings();
                    ControlsSettings c = config.GetControlsSettings();
                    if ( !ReadEdit( hWnd, IDC_LRARROWS, c.dFwdBackSecs, L"Please specify a numeric value for the left and right arrows" ) ) return TRUE;
                    if ( !ReadEdit( hWnd, IDC_UDARROWS, c.dSpeedUpPct, L"Please specify a numeric value for the up and down arrows" ) ) return TRUE;
                    if ( !ReadEdit( hWnd, IDC_NSARROWS, c.dNSpeedUpPct, L"Please specify a numeric value for Shift+up and down" ) ) return TRUE;
                    int iWidth = (int)g_szWindow.cx, iHeight = (int)g_szWindow.cy;
                    if ( !cView.GetFullScreen() )
                    {
                        SIZE szMin = MainWindowMinSize(), szMax = MainWindowMaxSize();
                        if ( !ReadSize( hWnd, IDC_WINWIDTH, (int)szMin.cx, (int)szMax.cx, (int)g_szWindow.cx, L"width", iWidth ) ) return TRUE;
                        if ( !ReadSize( hWnd, IDC_WINHEIGHT, (int)szMin.cy, (int)szMax.cy, (int)g_szWindow.cy, L"height", iHeight ) ) return TRUE;
                    }
                    config.SetControlsSettings( c );
                    VisualSettings v = config.GetVisualSettings();
                    const bool bOldShowControls = v.bAlwaysShowControls;
                    v.bAlwaysShowControls = Checked( hWnd, IDC_SHOWCONTROLS );
                    config.SetVisualSettings( v );
                    if ( cView.GetFullScreen() && bOldShowControls != v.bAlwaysShowControls ) cView.SetControls( cView.GetControls(), true );
                    VideoSettings cVideo = config.GetVideoSettings();
                    cVideo.bLimitFPS = Checked( hWnd, IDC_LIMITFPS );
                    config.SetVideoSettings( cVideo );
                    int eTheme = (int)SendDlgItemMessageW( hWnd, IDC_THEME, CB_GETCURSEL, 0, 0 );
                    if ( eTheme >= 0 && eTheme < DarkMode::ThemeCount && eTheme != cView.GetTheme() ) cView.SetTheme( eTheme, true );
                    if ( iWidth != g_szWindow.cx || iHeight != g_szWindow.cy )
                    {
                        SetMainWindowSize( iWidth, iHeight );
                        SetWindowSizeEdits( hWnd );
                    }
                    return Applied( hWnd );
                }
                break;
            }
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Misc (the background and bar colours, the background image, the tick-based view, exact tempo, note
    // pairing, the PFA lag simulation, 31-EDO tuning, the loading statistics)
    //-------------------------------------------------------------------------

    // The simulation's settings, chosen in its dialog and saved with the page
    struct PfaChoice
    {
        int iL2KB = 0, iL3KB = 0, iSendNs = 50;
        double dCpuPct = 100.0;
        bool bStats = false, bVisual = false;
        bool operator==( const PfaChoice &o ) const
        {
            return iL2KB == o.iL2KB && iL3KB == o.iL3KB && iSendNs == o.iSendNs && dCpuPct == o.dCpuPct && bStats == o.bStats && bVisual == o.bVisual;
        }
    };
    PfaChoice g_Pfa;

    PfaChoice PfaFrom( const VisualSettings &v )
    {
        PfaChoice c;
        c.iL2KB = v.iPfaL2KB;
        c.iL3KB = v.iPfaL3KB;
        c.iSendNs = v.iPfaSendNs;
        c.dCpuPct = v.dPfaCpuPct;
        c.bStats = v.bPfaLiveStats;
        c.bVisual = v.bPfaLiveVisual;
        return c;
    }

    INT_PTR CALLBACK PfaLagProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        PfaChoice *p = ( PfaChoice * )GetWindowLongPtrW( hWnd, DWLP_USER );
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                SetWindowLongPtrW( hWnd, DWLP_USER, lParam );
                p = ( PfaChoice * )lParam;
                // The cache sizes in KB (empty: this computer's, shown greyed in the boxes)
                const int aiId[2] = { IDC_PFAL2, IDC_PFAL3 }, aiKB[2] = { p->iL2KB, p->iL3KB };
                for ( int i = 0; i < 2; i++ )
                    if ( aiKB[i] > 0 ) SetDlgItemInt( hWnd, aiId[i], (UINT)aiKB[i], FALSE );
                PfaLag::Cpu cpu = PfaLag::Cpu::Detect();
                wchar_t buf[32];
                swprintf( buf, 32, L"%llu", (unsigned long long)( cpu.llL2 >> 10 ) );
                SendDlgItemMessageW( hWnd, IDC_PFAL2, EM_SETCUEBANNER, TRUE, ( LPARAM )buf );
                swprintf( buf, 32, L"%llu", (unsigned long long)( cpu.llL3 >> 10 ) );
                SendDlgItemMessageW( hWnd, IDC_PFAL3, EM_SETCUEBANNER, TRUE, ( LPARAM )buf );
                swprintf( buf, 32, L"%g", p->iSendNs / 1000.0 );
                SetDlgItemTextW( hWnd, IDC_PFASEND, buf );
                swprintf( buf, 32, L"%g", p->dCpuPct );
                SetDlgItemTextW( hWnd, IDC_PFASPEED, buf );
                Check( hWnd, IDC_PFASTATS, p->bStats );
                Check( hWnd, IDC_PFAVISUAL, p->bVisual );
                SetPfaBenchText( hWnd, true );
                return TRUE;
            }
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDOK:
                    {
                        const int aiId[2] = { IDC_PFAL2, IDC_PFAL3 };
                        int *apKB[2] = { &p->iL2KB, &p->iL3KB };
                        for ( int i = 0; i < 2; i++ )
                        {
                            BOOL bNum = FALSE;
                            UINT u = GetDlgItemInt( hWnd, aiId[i], &bNum, FALSE );
                            *apKB[i] = bNum ? (int)std::min< UINT >( u, i ? 1u << 22 : 1u << 20 ) : 0;
                        }
                        // (microseconds, with decimals; unreadable: kept)
                        wchar_t sSend[32] = L"";
                        GetDlgItemTextW( hWnd, IDC_PFASEND, sSend, 32 );
                        wchar_t *pEnd = nullptr;
                        double dSend = wcstod( sSend, &pEnd );
                        if ( pEnd != sSend && dSend >= 0.0 ) p->iSendNs = (int)( std::min( dSend, 10000.0 ) * 1000.0 + 0.5 );
                        // (a percentage, decimals allowed, from 0.0001 to a billion; empty or unreadable: this computer's)
                        wchar_t sPct[64] = {};
                        GetDlgItemTextW( hWnd, IDC_PFASPEED, sPct, 64 );
                        wchar_t *pPctEnd = nullptr;
                        const double dPct = wcstod( sPct, &pPctEnd );
                        p->dCpuPct = pPctEnd != sPct && dPct > 0.0 ? std::clamp( dPct, VisualSettings::PfaCpuMin, VisualSettings::PfaCpuMax ) : 100.0;
                        p->bStats = Checked( hWnd, IDC_PFASTATS );
                        p->bVisual = Checked( hWnd, IDC_PFAVISUAL );
                        EndDialog( hWnd, IDOK );
                        return TRUE;
                    }
                    case IDCANCEL:
                        EndDialog( hWnd, IDCANCEL );
                        return TRUE;
                }
                break;
        }
        return FALSE;
    }

    // Lag timings: the simulation's lag spikes over the whole open song (PfaLag::Spikes), with the settings on
    // the page and in its Settings dialog (applied or not), and the note speed, playback speed, mute and Limit
    // FPS as they are now. Worked out on a thread that holds the song; closing the window stops it
    struct PfaTimesJob
    {
        std::shared_ptr< const Song > pSong;
        PfaChoice c;
        PfaLag::Params p;
        PfaLag::SpikeRun run;
        std::vector< PfaLag::Spike > v;
        std::atomic< bool > bCancel{ false }, bDone{ false };
        std::atomic< int > iPermille{ 0 };
        double dSecs = 0.0;
        HWND hWnd = nullptr;
    };
    const UINT WM_PFATIMES_DONE = WM_APP + 40;

    // A song time as m:ss.s (h:mm:ss.s past an hour; before the song's 0 s mark, with a minus)
    std::wstring SongTimeText( long long llUs )
    {
        const wchar_t *sSign = llUs < 0 ? L"-" : L"";
        const long long t = std::llabs( llUs ) / 100000;    // (tenths)
        wchar_t buf[48];
        if ( t >= 36000 ) swprintf( buf, 48, L"%ls%lld:%02lld:%02lld.%lld", sSign, t / 36000, t / 600 % 60, t / 10 % 60, t % 10 );
        else swprintf( buf, 48, L"%ls%lld:%02lld.%lld", sSign, t / 600, t / 10 % 60, t % 10 );
        return buf;
    }

    // A length: seconds under a minute, else as a time
    std::wstring LengthText( long long llUs )
    {
        if ( llUs < 60000000 )
        {
            wchar_t buf[32];
            swprintf( buf, 32, L"%.1f s", llUs / 1e6 );
            return buf;
        }
        return SongTimeText( llUs );
    }

    std::wstring PfaTimesRow( const PfaLag::Spike &s )
    {
        return L"From " + SongTimeText( s.llSongFrom ) + L" to " + SongTimeText( s.llSongTo ) + L" lasting " + LengthText( s.llLagUs );
    }

    INT_PTR CALLBACK PfaTimesProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        auto *pp = ( std::shared_ptr< PfaTimesJob > * )GetWindowLongPtrW( hWnd, DWLP_USER );
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                SetWindowLongPtrW( hWnd, DWLP_USER, lParam );
                pp = ( std::shared_ptr< PfaTimesJob > * )lParam;
                std::shared_ptr< PfaTimesJob > pJob = *pp;
                pJob->hWnd = hWnd;
                EnableWindow( GetDlgItem( hWnd, IDC_PFATIMES_COPY ), FALSE );
                SetDlgItemTextW( hWnd, IDC_PFATIMES_NOTE, L"Working out the lag spikes..." );
                SetTimer( hWnd, 1, 100, nullptr );
                std::thread( [pJob]() {
                    const long long llT0 = Timer::NowMicroSecs();
                    PfaLag::Cpu cpu = PfaLag::Cpu::Detect();
                    if ( pJob->c.iL2KB > 0 ) cpu.llL2 = (uint64_t)pJob->c.iL2KB << 10;
                    if ( pJob->c.iL3KB > 0 ) cpu.llL3 = (uint64_t)pJob->c.iL3KB << 10;
                    PfaLag::ApplyBench( cpu, PfaLag::RunBench() );
                    cpu.Faster( pJob->c.dCpuPct / 100.0 );
                    pJob->v = PfaLag::Spikes( *pJob->pSong, cpu, pJob->p, pJob->run, [&pJob]( double f ) {
                        pJob->iPermille = (int)( f * 1000.0 );
                        return !pJob->bCancel;
                    } );
                    pJob->dSecs = ( Timer::NowMicroSecs() - llT0 ) / 1e6;
                    pJob->pSong.reset();
                    pJob->bDone = true;
                    if ( !pJob->bCancel ) PostMessageW( pJob->hWnd, WM_PFATIMES_DONE, 0, 0 );
                } ).detach();
                return TRUE;
            }
            case WM_TIMER:
                if ( pp && !( *pp )->bDone )
                {
                    wchar_t buf[64];
                    swprintf( buf, 64, L"Working out the lag spikes... %d%%", ( *pp )->iPermille.load() / 10 );
                    SetDlgItemTextW( hWnd, IDC_PFATIMES_NOTE, buf );
                }
                return TRUE;
            case WM_PFATIMES_DONE:
            {
                KillTimer( hWnd, 1 );
                const PfaTimesJob &j = **pp;
                HWND hList = GetDlgItem( hWnd, IDC_PFATIMES_LIST );
                for ( const PfaLag::Spike &s : j.v ) SendMessageW( hList, LB_ADDSTRING, 0, ( LPARAM )PfaTimesRow( s ).c_str() );
                wchar_t buf[400];
                if ( j.v.empty() )
                    swprintf( buf, 400, L"No lag spikes: no frame of the song takes a second or more (worked out in %.1f s).", j.dSecs );
                else
                    swprintf( buf, 400, L"%zu lag spike%ls playing the song from its start: where each starts and ends in the song, and how "
                                        L"long it lags (worked out in %.1f s; runs vary a little, with the computer's noise).",
                              j.v.size(), j.v.size() == 1 ? L"" : L"s", j.dSecs );
                std::wstring sNote = buf;
                if ( j.run.bFrozen ) sNote += L" It stopped after 10 years of lag: the original would never finish.";
                SetDlgItemTextW( hWnd, IDC_PFATIMES_NOTE, sNote.c_str() );
                EnableWindow( GetDlgItem( hWnd, IDC_PFATIMES_COPY ), !j.v.empty() );
                return TRUE;
            }
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDC_PFATIMES_COPY:
                    {
                        if ( HIWORD( wParam ) != BN_CLICKED || !( *pp )->bDone ) return TRUE;
                        std::wstring s;
                        for ( const PfaLag::Spike &sp : ( *pp )->v ) s += PfaTimesRow( sp ) + L"\r\n";
                        if ( s.empty() || !OpenClipboard( hWnd ) ) return TRUE;
                        EmptyClipboard();
                        const size_t n = s.size() + 1;
                        if ( HGLOBAL h = GlobalAlloc( GMEM_MOVEABLE, n * sizeof( wchar_t ) ) )
                        {
                            memcpy( GlobalLock( h ), s.c_str(), n * sizeof( wchar_t ) );
                            GlobalUnlock( h );
                            if ( !SetClipboardData( CF_UNICODETEXT, h ) ) GlobalFree( h );
                        }
                        CloseClipboard();
                        return TRUE;
                    }
                    case IDOK:
                    case IDCANCEL:
                        KillTimer( hWnd, 1 );
                        ( *pp )->bCancel = true;
                        EndDialog( hWnd, IDCANCEL );
                        return TRUE;
                }
                break;
        }
        return FALSE;
    }

    VOID ShowPfaTimes( HWND hPage )
    {
        Config &config = Config::GetConfig();
        const PlaybackSettings &cPlayback = config.GetPlaybackSettings();
        std::shared_ptr< const Song > pSong = cPlayback.GetPlayMode() == PlayMode::Play ? GetOpenedSong() : nullptr;
        if ( !pSong )
        {
            MessageBoxW( GetParent( hPage ), L"Please open a song first.", L"PFA lag timings", MB_OK | MB_ICONEXCLAMATION );
            return;
        }
        if ( !PfaLag::Ready( *pSong ) )
        {
            MessageBoxW( GetParent( hPage ), L"Please open the song again with the PFA lag simulation on: the timings need the note statistics "
                                             L"it gathers as a song opens.", L"PFA lag timings", MB_OK | MB_ICONEXCLAMATION );
            return;
        }
        auto pJob = std::make_shared< PfaTimesJob >();
        pJob->pSong = pSong;
        pJob->c = g_Pfa;
        pJob->p.llSpanUs = (long long)( 3.0 * cPlayback.GetNSpeed() * 1000000 );
        pJob->p.bMuted = cPlayback.GetMute();
        pJob->p.dSendUs = g_Pfa.iSendNs / 1000.0;
        pJob->run.dSpeed = cPlayback.GetSpeed();
        if ( config.GetVideoSettings().bLimitFPS )
        {
            DEVMODEW dm = {};
            dm.dmSize = sizeof( dm );
            pJob->run.dRefreshUs = EnumDisplaySettingsW( nullptr, ENUM_CURRENT_SETTINGS, &dm ) && dm.dmDisplayFrequency > 1 ? 1e6 / dm.dmDisplayFrequency : 1e6 / 60.0;
        }
        pSong.reset();
        DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_PFATIMES ), GetParent( hPage ), PfaTimesProc, ( LPARAM )&pJob );
    }

    // The image's controls only while it is shown
    VOID EnableBackground( HWND hWnd )
    {
        BOOL bOn = IsDlgButtonChecked( hWnd, IDC_BKGON ) == BST_CHECKED;
        for ( int iId : { IDC_BKGIMAGE, IDC_BKGBROWSE, IDC_BKGFIT, IDC_BKGOPACITY } ) EnableWindow( GetDlgItem( hWnd, iId ), bOn );
    }

    VOID ShowMisc( HWND hWnd, const VisualSettings &v, const PerformanceSettings &cPerf )
    {
        SetWindowLongPtrW( GetDlgItem( hWnd, IDC_BKGCOLOR ), GWLP_USERDATA, v.iBkgColor );
        SetWindowLongPtrW( GetDlgItem( hWnd, IDC_BARCOLOR ), GWLP_USERDATA, v.iBarColor );
        InvalidateRect( GetDlgItem( hWnd, IDC_BKGCOLOR ), nullptr, FALSE );
        InvalidateRect( GetDlgItem( hWnd, IDC_BARCOLOR ), nullptr, FALSE );
        Check( hWnd, IDC_BKGON, v.bBkgImage );
        SetDlgItemTextW( hWnd, IDC_BKGIMAGE, v.sBkgImage.c_str() );
        SendDlgItemMessageW( hWnd, IDC_BKGFIT, CB_SETCURSEL, v.iBkgFit, 0 );
        SetDlgItemInt( hWnd, IDC_BKGOPACITY, (UINT)v.iBkgOpacity, FALSE );
        EnableBackground( hWnd );
        Check( hWnd, IDC_TICKBASED, v.bTickBased );
        Check( hWnd, IDC_PERF_EXACTTEMPO, cPerf.bExactTempo );
        Check( hWnd, IDC_PERF_ENDOPEN, cPerf.bEndOpenNotes );
        Check( hWnd, IDC_PFALAG, v.bPfaLag );
        EnableWindow( GetDlgItem( hWnd, IDC_PFAADV ), v.bPfaLag );
        g_Pfa = PfaFrom( v );
        Check( hWnd, IDC_PERF_TUNING31, cPerf.bTuning31 );
        Check( hWnd, IDC_PERF_TUNING31BEND, cPerf.bTuning31PitchBend );
        EnableWindow( GetDlgItem( hWnd, IDC_PERF_TUNING31BEND ), cPerf.bTuning31 );
        Check( hWnd, IDC_PERF_LOADSTATS, cPerf.bShowLoadStats );
        Check( hWnd, IDC_PERF_FULLLOADSTATS, cPerf.bFullLoadStats );
    }

    INT_PTR CALLBACK MiscProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
                // By Renderer::ImageFit
                for ( const wchar_t *s : { L"Fill (cropped)", L"Fit (whole image)", L"Stretch", L"Center", L"Tile" } )
                    SendDlgItemMessageW( hWnd, IDC_BKGFIT, CB_ADDSTRING, 0, ( LPARAM )s );
                ShowMisc( hWnd, Config::GetConfig().GetVisualSettings(), Config::GetConfig().GetPerformanceSettings() );
                AddTips( hWnd, {
                    { IDC_BARCOLOR, L"The bar above the keys." },
                    { IDC_TICKBASED, L"A quarter note is always as tall as half a second at 120 BPM, so a tempo change no longer stretches or squeezes "
                                     L"the notes. Key fades and the hit line keep real time." },
                    { IDC_PERF_EXACTTEMPO, L"Very fast tempos keep their length. Off: times are rounded at each tempo change, as the original's are "
                                           L"(pixel for pixel)." },
                    { IDC_PFALAG, L"Its frame rate, freezes and audio bursts on dense songs, from a model of its work on the computer set in Settings." },
                    { IDC_PFATIMES, L"Where the open song would lag, and for how long, played from its start with these settings "
                                    L"(a few seconds at most: the light parts are skipped)." },
                    { IDC_PERF_TUNING31, L"Off: keys past 127 are not played (Notes & keys > Show 256 keys still shows them)." },
                    { IDC_PERF_TUNING31BEND, L"For synths without MTS. Broken." },
                    { IDC_PERF_LOADSTATS, L"The loading window stays open with the statistics once a file is loaded. File > Loading Statistics "
                                          L"shows the last ones either way." },
                    { IDC_PERF_FULLLOADSTATS, L"Every stage's parts, the song's memory by part, the process's memory and how the file was read, "
                                              L"while loading and after. Off: a summary." },
                } );
                return TRUE;
            case WM_DRAWITEM:
                if ( ( ( LPDRAWITEMSTRUCT )lParam )->CtlID != IDC_BKGCOLOR && ( ( LPDRAWITEMSTRUCT )lParam )->CtlID != IDC_BARCOLOR ) return FALSE;
                DrawSwatch( ( LPDRAWITEMSTRUCT )lParam );
                return TRUE;
            case WM_COMMAND:
            {
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                switch ( LOWORD( wParam ) )
                {
                    case IDC_BKGCOLOR:
                    case IDC_BARCOLOR:
                    {
                        if ( HIWORD( wParam ) != BN_CLICKED ) return TRUE;
                        HWND hWndBtn = ( HWND )lParam;
                        COLORREF cr = ( COLORREF )GetWindowLongPtrW( hWndBtn, GWLP_USERDATA );
                        if ( !PickColor( hWnd, cr ) ) return TRUE;
                        // (the colour kept first: Apply lights by the page's state with it)
                        SetWindowLongPtrW( hWndBtn, GWLP_USERDATA, cr );
                        InvalidateRect( hWndBtn, nullptr, FALSE );
                        Changed( hWnd );
                        return TRUE;
                    }
                    case IDC_BKGON:
                        EnableBackground( hWnd );
                        return TRUE;
                    case IDC_BKGBROWSE:
                    {
                        wchar_t sFile[1024] = {};
                        GetDlgItemTextW( hWnd, IDC_BKGIMAGE, sFile, 1024 );
                        OPENFILENAMEW ofn = {};
                        ofn.lStructSize = sizeof( ofn );
                        ofn.hwndOwner = hWnd;
                        ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.heic;*.heif;*.jxr;*.ico\0All files (*.*)\0*.*\0";
                        ofn.lpstrFile = sFile;
                        ofn.nMaxFile = 1024;
                        ofn.lpstrTitle = L"Choose a background image";
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                        if ( GetOpenFileNameW( &ofn ) ) { SetDlgItemTextW( hWnd, IDC_BKGIMAGE, sFile ); Changed( hWnd ); }
                        return TRUE;
                    }
                    case IDC_PERF_TUNING31:
                        EnableWindow( GetDlgItem( hWnd, IDC_PERF_TUNING31BEND ), Checked( hWnd, IDC_PERF_TUNING31 ) );
                        return TRUE;
                    case IDC_PFALAG:
                        EnableWindow( GetDlgItem( hWnd, IDC_PFAADV ), Checked( hWnd, IDC_PFALAG ) );
                        return TRUE;
                    case IDC_PFATIMES:
                        if ( HIWORD( wParam ) == BN_CLICKED ) ShowPfaTimes( hWnd );
                        return TRUE;
                    case IDC_PFAADV:
                    {
                        if ( HIWORD( wParam ) != BN_CLICKED ) return TRUE;
                        PfaChoice c = g_Pfa;
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_PFALAG ), GetParent( hWnd ), PfaLagProc, ( LPARAM )&c ) == IDOK &&
                             !( c == g_Pfa ) )
                        {
                            g_Pfa = c;
                            Changed( hWnd );
                        }
                        return TRUE;
                    }
                    case IDC_RESTOREDEFAULTS:
                    {
                        VisualSettings v;
                        v.LoadDefaultValues();
                        PerformanceSettings cPerf;
                        cPerf.LoadDefaultValues();
                        ShowMisc( hWnd, v, cPerf );
                        Changed( hWnd );
                        return TRUE;
                    }
                }
                break;
            }
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    VisualSettings v = config.GetVisualSettings();
                    BOOL bOk = FALSE;
                    UINT uOpacity = GetDlgItemInt( hWnd, IDC_BKGOPACITY, &bOk, FALSE );
                    const bool bBkgOn = Checked( hWnd, IDC_BKGON );
                    if ( !bOk && !bBkgOn ) uOpacity = (UINT)v.iBkgOpacity;     // not checked while the image is off
                    else if ( bBkgOn && ( !bOk || uOpacity > 100 ) )
                        return Invalid( hWnd, IDC_BKGOPACITY, L"Please specify a background image opacity between 0 and 100 percent" );
                    wchar_t sImage[1024] = {};
                    GetDlgItemTextW( hWnd, IDC_BKGIMAGE, sImage, 1024 );
                    if ( bBkgOn && !sImage[0] )
                        return Invalid( hWnd, IDC_BKGIMAGE, L"Please choose a background image file, or uncheck the background image." );
                    if ( bBkgOn && GetFileAttributesW( sImage ) == INVALID_FILE_ATTRIBUTES )
                        return Invalid( hWnd, IDC_BKGIMAGE, L"The background image file was not found. Please choose an existing file, or leave the box empty for no image." );
                    v.iBkgColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_BKGCOLOR ), GWLP_USERDATA );
                    v.iBarColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_BARCOLOR ), GWLP_USERDATA );
                    v.bBkgImage = bBkgOn;
                    v.sBkgImage = sImage;
                    v.iBkgOpacity = std::min( 100, (int)uOpacity );
                    v.iBkgFit = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_BKGFIT, CB_GETCURSEL, 0, 0 ) );
                    v.bTickBased = Checked( hWnd, IDC_TICKBASED );
                    v.bPfaLag = Checked( hWnd, IDC_PFALAG );
                    v.iPfaL2KB = g_Pfa.iL2KB;
                    v.iPfaL3KB = g_Pfa.iL3KB;
                    v.iPfaSendNs = g_Pfa.iSendNs;
                    v.dPfaCpuPct = g_Pfa.dCpuPct;
                    v.bPfaLiveStats = g_Pfa.bStats;
                    v.bPfaLiveVisual = g_Pfa.bVisual;
                    config.SetVisualSettings( v );
                    PerformanceSettings cPerf = config.GetPerformanceSettings();
                    cPerf.bExactTempo = Checked( hWnd, IDC_PERF_EXACTTEMPO );
                    cPerf.bEndOpenNotes = Checked( hWnd, IDC_PERF_ENDOPEN );
                    cPerf.bTuning31 = Checked( hWnd, IDC_PERF_TUNING31 );
                    cPerf.bTuning31PitchBend = Checked( hWnd, IDC_PERF_TUNING31BEND );
                    cPerf.bShowLoadStats = Checked( hWnd, IDC_PERF_LOADSTATS );
                    cPerf.bFullLoadStats = Checked( hWnd, IDC_PERF_FULLLOADSTATS );
                    config.SetPerformanceSettings( cPerf );
                    return Applied( hWnd );
                }
                break;
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Performance (loading, drawing and memory; what is sent and how it is decoded)
    //-------------------------------------------------------------------------

    // A thread count edit: 0 is auto (Parallel::ThreadCount: half the logical processors)
    const int MaxThreads = 256;

    bool ReadThreads( HWND hWnd, int iId, const wchar_t *sWhat, int &iValue )
    {
        BOOL bOk = FALSE;
        UINT u = GetDlgItemInt( hWnd, iId, &bOk, FALSE );
        if ( bOk && (int)u <= MaxThreads ) { iValue = (int)u; return true; }
        wchar_t buf[128];
        swprintf( buf, 128, L"Please specify %ls between 0 (auto) and %d", sWhat, MaxThreads );
        Invalid( hWnd, iId, buf );
        return false;
    }

    VOID ShowPerformance( HWND hWnd, const PerformanceSettings &cPerf, const ControlsSettings &c )
    {
        SetDlgItemInt( hWnd, IDC_PERF_THREADS, (UINT)cPerf.iWorkerThreads, FALSE );
        Check( hWnd, IDC_PERF_LOD, cPerf.bLodCache );
        Check( hWnd, IDC_PERF_INSTANCING, cPerf.bGpuInstancing );
        Check( hWnd, IDC_PERF_COMPACT, cPerf.bCompactVisual );
        SetDlgItemInt( hWnd, IDC_PERF_CACHEMB, (UINT)cPerf.iBlockCacheMB, FALSE );
        Check( hWnd, IDC_PERF_DEDUP, cPerf.bDedup );
        Check( hWnd, IDC_PERF_DEDUPASSUME, cPerf.bDedupAssume );
        EnableWindow( GetDlgItem( hWnd, IDC_PERF_DEDUPASSUME ), cPerf.bDedup );
        Check( hWnd, IDC_PERF_SKIPPING, cPerf.bVelocitySkip );
        Check( hWnd, IDC_PERF_VELFLOOR, cPerf.bVelocityFloor );
        SetDlgItemInt( hWnd, IDC_PERF_MINVEL, (UINT)cPerf.iMinVelocity, FALSE );
        Check( hWnd, IDC_PERF_COLLAPSE, cPerf.bCollapseStacks );
        Check( hWnd, IDC_PERF_SENDLIMIT, cPerf.bSendLimit );
        SetDlgItemInt( hWnd, IDC_PERF_MAXSENDS, (UINT)cPerf.iMaxSendsPerSec, FALSE );
        SetDlgItemInt( hWnd, IDC_PERF_PLAYTHREADS, (UINT)cPerf.iPlaybackThreads, FALSE );
        Check( hWnd, IDC_STARTPAUSED, c.bStartPaused );
    }

    INT_PTR CALLBACK PerformanceProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                wchar_t buf[64];
                swprintf( buf, 64, L"(0: auto, %d)", Parallel::ThreadCount( 0 ) );
                SetDlgItemTextW( hWnd, IDC_PERF_THREADSNOTE, buf );
                SetDlgItemTextW( hWnd, IDC_PERF_PLAYTHREADSNOTE, buf );
                ShowPerformance( hWnd, Config::GetConfig().GetPerformanceSettings(), Config::GetConfig().GetControlsSettings() );
                AddTips( hWnd, {
                    { IDC_PERF_THREADS, L"0: auto, half the logical processors. Drawing takes a change at once, loading from the next file on." },
                    { IDC_PERF_LOD, L"Keeps, for the window's size and note speed, only the notes that can show, built in the background around the playhead." },
                    { IDC_PERF_INSTANCING, L"Each note goes to the graphics card as one 32-byte instance instead of two quads. A note's gradient can be off by "
                                           L"one colour level along a pixel column; its edges never are." },
                    { IDC_PERF_CACHEMB, L"For the compact store: how many decoded notes are kept, 16 to 65536 MB." },
                    { IDC_PERF_DEDUP, L"Also in layered compressed files, whose repeats are read once." },
                    { IDC_PERF_DEDUPASSUME, L"Fast; exact unless two layers' sizes happen to match. Assumed: the rest of a layer repeats its first part." },
                    { IDC_PERF_SKIPPING, L"A last resort for a synth that cannot keep up: once it lags over 100 ms behind, note-ons are skipped by "
                                         L"velocity, the quietest first, and at 400 ms every one. Off: every event is sent." },
                    { IDC_PERF_VELFLOOR, L"1 to 128. Their note-offs are skipped too." },
                    { IDC_PERF_COLLAPSE, L"One voice per channel, key and tick." },
                    { IDC_PERF_SENDLIMIT, L"1,000 to 1,000,000,000 messages a second; the quietest notes are skipped first." },
                    { IDC_PERF_PLAYTHREADS, L"The most threads decoding playback: 0 is auto, half the logical processors. Takes effect at once." },
                } );
                return TRUE;
            }
            case WM_COMMAND:
                if ( LOWORD( wParam ) == IDC_PERF_DEDUP && HIWORD( wParam ) == BN_CLICKED )
                    EnableWindow( GetDlgItem( hWnd, IDC_PERF_DEDUPASSUME ), Checked( hWnd, IDC_PERF_DEDUP ) );
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                if ( LOWORD( wParam ) == IDC_RESTOREDEFAULTS )
                {
                    PerformanceSettings cPerf;
                    cPerf.LoadDefaultValues();
                    ControlsSettings c;
                    c.LoadDefaultValues();
                    ShowPerformance( hWnd, cPerf, c );
                    Changed( hWnd );
                    return TRUE;
                }
                break;
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    int iThreads = 0, iPlayThreads = 0;
                    if ( !ReadThreads( hWnd, IDC_PERF_THREADS, L"a thread count for loading and drawing", iThreads ) ) return TRUE;
                    if ( !ReadThreads( hWnd, IDC_PERF_PLAYTHREADS, L"a thread count for playback decoding", iPlayThreads ) ) return TRUE;
                    BOOL bOk = FALSE;
                    UINT uCache = GetDlgItemInt( hWnd, IDC_PERF_CACHEMB, &bOk, FALSE );
                    if ( !bOk || uCache < 16 || uCache > 65536 ) return Invalid( hWnd, IDC_PERF_CACHEMB, L"Please specify a block cache between 16 and 65536 MB" );
                    UINT uMinVel = GetDlgItemInt( hWnd, IDC_PERF_MINVEL, &bOk, FALSE );
                    if ( !bOk || uMinVel < 1 || uMinVel > 128 ) return Invalid( hWnd, IDC_PERF_MINVEL, L"Please specify a velocity between 1 and 128 to skip notes below" );
                    UINT uSends = GetDlgItemInt( hWnd, IDC_PERF_MAXSENDS, &bOk, FALSE );
                    if ( !bOk || uSends < 1000 || uSends > 1000000000 )
                        return Invalid( hWnd, IDC_PERF_MAXSENDS, L"Please specify a sending limit between 1,000 and 1,000,000,000 messages per second" );
                    PerformanceSettings cPerf = config.GetPerformanceSettings();
                    cPerf.iWorkerThreads = iThreads;
                    cPerf.bLodCache = Checked( hWnd, IDC_PERF_LOD );
                    cPerf.bGpuInstancing = Checked( hWnd, IDC_PERF_INSTANCING );
                    cPerf.bCompactVisual = Checked( hWnd, IDC_PERF_COMPACT );
                    cPerf.iBlockCacheMB = (int)uCache;
                    cPerf.bDedup = Checked( hWnd, IDC_PERF_DEDUP );
                    cPerf.bDedupAssume = Checked( hWnd, IDC_PERF_DEDUPASSUME );
                    cPerf.bVelocitySkip = Checked( hWnd, IDC_PERF_SKIPPING );
                    cPerf.bVelocityFloor = Checked( hWnd, IDC_PERF_VELFLOOR );
                    cPerf.iMinVelocity = (int)uMinVel;
                    cPerf.bCollapseStacks = Checked( hWnd, IDC_PERF_COLLAPSE );
                    cPerf.bSendLimit = Checked( hWnd, IDC_PERF_SENDLIMIT );
                    cPerf.iMaxSendsPerSec = (int)uSends;
                    cPerf.iPlaybackThreads = iPlayThreads;
                    config.SetPerformanceSettings( cPerf );
                    ControlsSettings c = config.GetControlsSettings();
                    c.bStartPaused = Checked( hWnd, IDC_STARTPAUSED );
                    config.SetControlsSettings( c );
                    return Applied( hWnd );
                }
                break;
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Each page's settings kept outside its controls, as text (PageState)
    //-------------------------------------------------------------------------

    std::wstring KeysExtra()
    {
        std::wstring s = std::to_wstring( g_uClean ) + L";";
        for ( unsigned c : g_aPalette ) s += std::to_wstring( c ) + L",";
        for ( const VisualSettings::Palette &p : g_vPalettes )
        {
            s += L"|" + p.sName + L":";
            for ( unsigned c : p.vColors ) s += std::to_wstring( c ) + L",";
        }
        return s;
    }
    std::wstring StatsExtra()
    {
        VisualSettings v = Config::GetConfig().GetVisualSettings();
        v.uStats = g_Stats.uStats;
        std::copy( g_Stats.aOrder, g_Stats.aOrder + VisualSettings::StatCount, v.aStatOrder );
        v.graph = g_Graph;
        IniFile ini;
        v.Save( ini );
        return Widen( ini.Text() );
    }
    std::wstring EffectsExtra()
    {
        IniFile ini;
        g_FxShown.Save( ini );
        std::wstring s = Widen( ini.Text() );
        for ( int i = 0; i < EffectSettings::LookCount; i++ ) s += std::to_wstring( g_abKeyWhite[i] ) + L"," + std::to_wstring( g_aiKeyWhite[i] ) + L";";
        return s;
    }
    std::wstring AudioExtra()
    {
        // (the advanced dialogs' settings only: the output and engine are in use as they are chosen)
        AudioSettings a = Config::GetConfig().CopyAudioSettings();
        a.kb = g_RenderKb;
        IniFile ini;
        a.Save( ini );
        std::string s;
        for ( const auto &kv : ini.Section( "Audio" ) ) if ( kv.first.compare( 0, 7, "Kestrel" ) == 0 ) s += kv.first + "=" + kv.second + ";";
        return Widen( s );
    }
    std::wstring MiscExtra()
    {
        wchar_t buf[160];
        swprintf( buf, 160, L"%d,%d,%d,%.17g,%d,%d", g_Pfa.iL2KB, g_Pfa.iL3KB, g_Pfa.iSendNs, g_Pfa.dCpuPct, (int)g_Pfa.bStats, (int)g_Pfa.bVisual );
        return buf;
    }

    //-------------------------------------------------------------------------
    // Video rendering (ExportSettings; the controls are the Render Video dialog's: app/VideoExport.cpp)
    //-------------------------------------------------------------------------

    INT_PTR CALLBACK VideoProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
                VideoPage::Init( hWnd );
                return TRUE;
            case WM_COMMAND:
                if ( LOWORD( wParam ) == IDC_VID_START )
                {
                    // (the settings are saved as the render starts)
                    if ( HIWORD( wParam ) == BN_CLICKED )
                    {
                        if ( VideoPage::Start( hWnd ) ) Baseline( hWnd );
                        else Changed( hWnd );
                    }
                    return TRUE;
                }
                if ( LOWORD( wParam ) == IDC_RESTOREDEFAULTS )
                {
                    ExportSettings d;
                    d.LoadDefaultValues();
                    VideoPage::Show( hWnd, d );
                    Changed( hWnd );
                    return TRUE;
                }
                if ( VideoPage::Command( hWnd, wParam ) || UserChange( wParam, lParam ) ) Changed( hWnd );
                break;
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    // (only when changed: the Render Video dialog keeps the same settings)
                    if ( PageDiffers( hWnd ) ) VideoPage::Keep( hWnd );
                    return Applied( hWnd );
                }
                break;
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Fitting the screen: with a large scaling or a small screen the sheet can be taller (or wider)
    // than the work area, its OK and Apply out of reach. Each page is made from a copy of its
    // template, cut to what fits around the sheet's frame, tabs and buttons; a page cut short scrolls.
    //-------------------------------------------------------------------------

    DLGPROC g_aPageProcs[MaxPages];
    SIZE g_aPageFull[MaxPages];             // the templates' sizes, in dialog units
    DWORD g_aPageBars[MaxPages];            // the scroll bars of a page cut short (the sheet drops them from its template)

    // The largest page that fits the work area of the owner's monitor, in dialog units
    SIZE PageFit( HWND hWndOwner )
    {
        MONITORINFO mi = { sizeof( mi ) };
        GetMonitorInfoW( MonitorFromWindow( hWndOwner, MONITOR_DEFAULTTONEAREST ), &mi );
        int iWorkW = mi.rcWork.right - mi.rcWork.left, iWorkH = mi.rcWork.bottom - mi.rcWork.top;
        // (PFAVIZ_PREFSAREA=WxH: a smaller work area, for testing)
        if ( const wchar_t *s = _wgetenv( L"PFAVIZ_PREFSAREA" ) ) swscanf( s, L"%dx%d", &iWorkW, &iWorkH );
        // The pages' font gives the dialog units: 4 across its average character, 8 down its height
        HDC hdc = GetDC( nullptr );
        HFONT hFont = CreateFontW( -MulDiv( 8, GetDeviceCaps( hdc, LOGPIXELSY ), 72 ), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"MS Shell Dlg 2" );
        HGDIOBJ hOld = SelectObject( hdc, hFont );
        TEXTMETRICW tm = {};
        GetTextMetricsW( hdc, &tm );
        SIZE sz = {};
        GetTextExtentPoint32W( hdc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &sz );
        SelectObject( hdc, hOld );
        DeleteObject( hFont );
        ReleaseDC( nullptr, hdc );
        int iUnitX = std::max( 1, (int)( ( sz.cx / 26 + 1 ) / 2 ) ), iUnitY = std::max( 1, (int)tm.tmHeight );
        // Around a page: the frame, and the caption, the tabs and the buttons (about 50 units)
        int iFrame = 2 * ( GetSystemMetrics( SM_CXFRAME ) + GetSystemMetrics( SM_CXPADDEDBORDER ) );
        SIZE fit;
        fit.cx = std::max( 160, MulDiv( iWorkW - iFrame, 4, iUnitX ) - 16 );
        fit.cy = std::max( 80, MulDiv( iWorkH - iFrame - GetSystemMetrics( SM_CYCAPTION ), 8, iUnitY ) - 50 );
        return fit;
    }

    // A page cut short: scrolled by its scroll bars, the wheel, and to a field that takes the focus
    void ScrollPage( HWND hWnd, int iBar, int iPos )
    {
        SCROLLINFO si = { sizeof( si ), SIF_ALL };
        if ( !GetScrollInfo( hWnd, iBar, &si ) ) return;
        iPos = std::max( si.nMin, std::min( iPos, si.nMax - (int)si.nPage + 1 ) );
        if ( iPos == si.nPos ) return;
        int d = si.nPos - iPos;
        si.fMask = SIF_POS;
        si.nPos = iPos;
        SetScrollInfo( hWnd, iBar, &si, TRUE );
        ScrollWindowEx( hWnd, iBar == SB_HORZ ? d : 0, iBar == SB_VERT ? d : 0, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE );
    }
    void PageScrollRanges( HWND hWnd, int iPage )
    {
        RECT rcFull = { 0, 0, g_aPageFull[iPage].cx, g_aPageFull[iPage].cy }, rc;
        MapDialogRect( hWnd, &rcFull );
        GetClientRect( hWnd, &rc );
        LONG lStyle = GetWindowLongW( hWnd, GWL_STYLE );
        for ( int iBar : { SB_HORZ, SB_VERT } )
        {
            if ( !( lStyle & ( iBar == SB_HORZ ? WS_HSCROLL : WS_VSCROLL ) ) ) continue;
            SCROLLINFO si = { sizeof( si ), SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL };
            si.nMax = ( iBar == SB_HORZ ? rcFull.right : rcFull.bottom ) - 1;
            si.nPage = (UINT)( iBar == SB_HORZ ? rc.right : rc.bottom );
            SetScrollInfo( hWnd, iBar, &si, TRUE );
            ScrollPage( hWnd, iBar, GetScrollPos( hWnd, iBar ) );     // (within the new range)
        }
    }
    bool PageScroll( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, int iPage )
    {
        LONG lStyle = GetWindowLongW( hWnd, GWL_STYLE );
        if ( !( lStyle & ( WS_VSCROLL | WS_HSCROLL ) ) ) return false;
        switch ( msg )
        {
            case WM_VSCROLL: case WM_HSCROLL:
            {
                if ( lParam ) return false;     // an up-down or slider control's
                int iBar = msg == WM_VSCROLL ? SB_VERT : SB_HORZ;
                SCROLLINFO si = { sizeof( si ), SIF_ALL };
                GetScrollInfo( hWnd, iBar, &si );
                int iLine = 16, iPos = si.nPos;
                switch ( LOWORD( wParam ) )
                {
                    case SB_LINEUP: iPos -= iLine; break;
                    case SB_LINEDOWN: iPos += iLine; break;
                    case SB_PAGEUP: iPos -= (int)si.nPage; break;
                    case SB_PAGEDOWN: iPos += (int)si.nPage; break;
                    case SB_THUMBTRACK: case SB_THUMBPOSITION: iPos = si.nTrackPos; break;
                    case SB_TOP: iPos = si.nMin; break;
                    case SB_BOTTOM: iPos = si.nMax; break;
                }
                ScrollPage( hWnd, iBar, iPos );
                return true;
            }
            case WM_MOUSEWHEEL:
            {
                int iBar = ( lStyle & WS_VSCROLL ) ? SB_VERT : SB_HORZ;
                ScrollPage( hWnd, iBar, GetScrollPos( hWnd, iBar ) - GET_WHEEL_DELTA_WPARAM( wParam ) * 48 / WHEEL_DELTA );
                return true;
            }
            case WM_SIZE:
                PageScrollRanges( hWnd, iPage );
                return false;
            case WM_COMMAND:
            {
                // A field taking the focus (tabbed to, or an error pointing at it) brought into view
                WORD w = HIWORD( wParam );
                if ( lParam && ( w == EN_SETFOCUS || w == CBN_SETFOCUS || w == LBN_SETFOCUS || w == BN_SETFOCUS ) )
                {
                    RECT rc, rcCtl;
                    GetClientRect( hWnd, &rc );
                    GetWindowRect( ( HWND )lParam, &rcCtl );
                    MapWindowPoints( nullptr, hWnd, ( LPPOINT )&rcCtl, 2 );
                    if ( lStyle & WS_VSCROLL )
                    {
                        int iPos = GetScrollPos( hWnd, SB_VERT );
                        if ( rcCtl.top < 0 ) ScrollPage( hWnd, SB_VERT, iPos + rcCtl.top - 8 );
                        else if ( rcCtl.bottom > rc.bottom ) ScrollPage( hWnd, SB_VERT, iPos + rcCtl.bottom - rc.bottom + 8 );
                    }
                    if ( lStyle & WS_HSCROLL )
                    {
                        int iPos = GetScrollPos( hWnd, SB_HORZ );
                        if ( rcCtl.left < 0 ) ScrollPage( hWnd, SB_HORZ, iPos + rcCtl.left - 8 );
                        else if ( rcCtl.right > rc.right ) ScrollPage( hWnd, SB_HORZ, iPos + rcCtl.right - rc.right + 8 );
                    }
                }
                return false;
            }
        }
        return false;
    }

    // Every page's procedure: its own, behind the scrolling
    INT_PTR CALLBACK PageProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        if ( msg == WM_INITDIALOG ) SetPropW( hWnd, L"PFAviz.Page", ( HANDLE )( ( ( LPPROPSHEETPAGEW )lParam )->lParam + 1 ) );
        INT_PTR i = ( INT_PTR )GetPropW( hWnd, L"PFAviz.Page" ) - 1;
        if ( i < 0 || i >= MaxPages ) return FALSE;
        if ( msg == WM_NCDESTROY ) { RemovePropW( hWnd, L"PFAviz.Page" ); g_mPageBase.erase( hWnd ); }
        if ( PageScroll( hWnd, msg, wParam, lParam, (int)i ) ) return TRUE;
        // An up-down's arrows change its field's number (UserChange)
        if ( ( msg == WM_VSCROLL || msg == WM_HSCROLL ) && lParam && LOWORD( wParam ) == SB_THUMBPOSITION )
        {
            wchar_t sClass[32];
            if ( GetClassNameW( ( HWND )lParam, sClass, 32 ) && _wcsicmp( sClass, UPDOWN_CLASSW ) == 0 ) Changed( hWnd );
        }
        INT_PTR r = g_aPageProcs[i]( hWnd, msg, wParam, lParam );
        if ( msg == WM_INITDIALOG ) Baseline( hWnd );
        if ( msg == WM_NOTIFY && ( ( LPNMHDR )lParam )->code == PSN_APPLY && GetWindowLongPtrW( hWnd, DWLP_MSGRESULT ) == PSNRET_NOERROR )
            Baseline( hWnd );
        if ( msg == WM_INITDIALOG && g_aPageBars[i] )
        {
            SetWindowLongW( hWnd, GWL_STYLE, GetWindowLongW( hWnd, GWL_STYLE ) | (LONG)g_aPageBars[i] );
            SetWindowPos( hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
            PageScrollRanges( hWnd, (int)i );
        }
        return r;
    }
}

static HWND g_hWndPrefs = nullptr;

VOID DoPreferences( HWND hWndOwner )
{
    if ( g_hWndPrefs )
    {
        if ( IsIconic( g_hWndPrefs ) ) ShowWindow( g_hWndPrefs, SW_RESTORE );
        SetForegroundWindow( g_hWndPrefs );
        return;
    }
    // Each page sets only its own settings, so they apply in any order
    const int aDialogs[] = { IDD_PP_KEYS, IDD_PP_STATS, IDD_PP_EFFECTS, IDD_PP_AUDIO, IDD_PP_INTERFACE, IDD_PP_PERFORMANCE, IDD_PP_VIDEO,
                             IDD_PP_MISC };
    const DLGPROC aProcs[] = { KeysProc, StatsPageProc, EffectsProc, AudioProc, InterfaceProc, PerformanceProc, VideoProc, MiscProc };
    const PageExtraFn aExtras[] = { KeysExtra, StatsExtra, EffectsExtra, AudioExtra, nullptr, nullptr, nullptr, MiscExtra };
    // ("&&": a tab's title takes & as a mnemonic)
    const wchar_t *const aTitles[] = { L"Notes && keys", L"Stats", L"Look && effects", L"Audio", L"Interface", L"Performance", L"Video rendering", L"Misc" };
    const int iPages = 8;
    static_assert( iPages <= MaxPages, "MaxPages" );
    PROPSHEETPAGEW psp[iPages] = {};
    // (kept while the window is open: a page is made from its template when its tab is first shown)
    static std::vector< uint8_t > vTemplates[iPages];
    const SIZE fit = PageFit( hWndOwner );
    for ( int i = 0; i < iPages; i++ )
    {
        psp[i].dwSize = sizeof( PROPSHEETPAGEW );
        psp[i].dwFlags = PSP_USETITLE;
        psp[i].hInstance = g_hInstance;
        psp[i].pszTemplate = MAKEINTRESOURCEW( aDialogs[i] );
        psp[i].pfnDlgProc = PageProc;
        psp[i].pszTitle = aTitles[i];
        psp[i].lParam = i;
        g_aPageProcs[i] = aProcs[i];
        g_aPageExtra[i] = aExtras[i];
        // The template's copy (DLGTEMPLATEEX: cx and cy at 22 and 24), cut to fit
        HRSRC hRes = FindResourceW( g_hInstance, MAKEINTRESOURCEW( aDialogs[i] ), RT_DIALOG );
        HGLOBAL hGlobal = hRes ? LoadResource( g_hInstance, hRes ) : nullptr;
        const uint8_t *p = hGlobal ? ( const uint8_t * )LockResource( hGlobal ) : nullptr;
        DWORD n = hRes ? SizeofResource( g_hInstance, hRes ) : 0;
        if ( !p || n < 26 || p[2] != 0xFF || p[3] != 0xFF ) continue;
        vTemplates[i].assign( p, p + n );
        uint8_t *t = vTemplates[i].data();
        int16_t cx, cy;
        memcpy( &cx, t + 22, 2 );
        memcpy( &cy, t + 24, 2 );
        g_aPageFull[i] = { cx, cy };
        g_aPageBars[i] = 0;
        if ( cx > fit.cx ) { cx = (int16_t)fit.cx; g_aPageBars[i] |= WS_HSCROLL; }
        if ( cy > fit.cy ) { cy = (int16_t)fit.cy; g_aPageBars[i] |= WS_VSCROLL; }
        memcpy( t + 22, &cx, 2 );
        memcpy( t + 24, &cy, 2 );
        psp[i].dwFlags |= PSP_DLGINDIRECT;
        psp[i].pResource = ( LPCDLGTEMPLATEW )t;
    }
    PROPSHEETHEADERW psh = {};
    psh.dwSize = sizeof( PROPSHEETHEADERW );
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP | PSH_USECALLBACK | PSH_MODELESS;
    // The sheet's frame is the property sheet's own dialog, which DarkMode does not see made
    psh.pfnCallback = []( HWND hWnd, UINT msg, LPARAM ) -> int {
        if ( msg == PSCB_INITIALIZED ) { DarkMode::Dialog( hWnd ); OnScreen::Track( hWnd ); }
        return 0;
    };
    psh.hwndParent = hWndOwner;
    psh.hInstance = g_hInstance;
    psh.pszCaption = L"Preferences";
    psh.nPages = iPages;
    psh.ppsp = psp;
    g_hWndPrefs = ( HWND )PropertySheetW( &psh );
    if ( g_hWndPrefs == ( HWND )-1 ) g_hWndPrefs = nullptr;
}

BOOL PreferencesMessage( MSG *pMsg )
{
    if ( !g_hWndPrefs ) return FALSE;
    BOOL bHandled = PropSheet_IsDialogMessage( g_hWndPrefs, pMsg );
    // (OK, Cancel or the close button: the sheet has no page left, and is destroyed here)
    if ( !PropSheet_GetCurrentPageHwnd( g_hWndPrefs ) )
    {
        HWND hWndOwner = GetWindow( g_hWndPrefs, GW_OWNER );
        DestroyWindow( g_hWndPrefs );
        g_hWndPrefs = nullptr;
        if ( hWndOwner ) SetForegroundWindow( hWndOwner );
    }
    return bHandled;
}
