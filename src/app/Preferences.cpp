// Port of the original ConfigProcs.cpp property pages. Visual gains "Show all 128 keys" (the
// default) and full 0-127 key ranges; "Set Using Piano" is gone. Audio replaces the MIDI input list
// with the KDMAPI status and its options. Controls keeps only its General group. Performance is new.
// The tall pages are laid out in two columns, and a page cut short to fit the screen scrolls.
#include "app/Preferences.h"

#include <algorithm>
#include <colordlg.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <thread>
#include <vector>

#include "app/DarkMode.h"
#include "app/MainWindow.h"
#include "app/VideoExport.h"
#include "audio/MidiOutput.h"
#include "core/Config.h"
#include "core/Ini.h"
#include "midi/Notes.h"
#include "resource.h"
#include "song/PfaLag.h"

namespace
{
    VOID Changed( HWND hWnd )
    {
        SendMessageW( GetParent( hWnd ), PSM_CHANGED, ( WPARAM )hWnd, 0 );
    }

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

    //-------------------------------------------------------------------------
    // Visual
    //-------------------------------------------------------------------------

    const int aKeyModeIds[5] = { IDC_SHOWALLKEYS, IDC_SHOWSONGKEYS, IDC_SHOWCUSTOMKEYS, IDC_SHOWALL128KEYS, IDC_SHOWALL256KEYS }; // by KeysShown

    VOID EnableCustomKeys( HWND hWnd, BOOL bEnable )
    {
        EnableWindow( GetDlgItem( hWnd, IDC_FIRSTKEY ), bEnable );
        EnableWindow( GetDlgItem( hWnd, IDC_THROUGH ), bEnable );
        EnableWindow( GetDlgItem( hWnd, IDC_LASTKEY ), bEnable );
    }

    // The stats box lines chosen in the Statistics dialog, and their order, saved with the Visual page
    struct StatsChoice
    {
        unsigned uStats = 0;
        unsigned char aOrder[VisualSettings::StatCount] = {};
    };
    StatsChoice g_Stats;

    bool SameGraph( const VisualSettings::GraphSettings &a, const VisualSettings::GraphSettings &b )
    {
        const int n = VisualSettings::GraphSettings::SeriesCount;
        return std::equal( a.abShow, a.abShow + n, b.abShow ) && std::equal( a.abLine, a.abLine + n, b.abLine ) &&
               std::equal( a.abRight, a.abRight + n, b.abRight ) && std::equal( a.aColor, a.aColor + n, b.aColor ) &&
               std::equal( a.aiPlace, a.aiPlace + n, b.aiPlace ) && a.bTogether == b.bTogether && a.bZero == b.bZero &&
               a.bMiddle == b.bMiddle && a.iCorner == b.iCorner && a.iSpanMs == b.iSpanMs && a.iCX == b.iCX && a.iCY == b.iCY &&
               a.iFontPx == b.iFontPx && a.iGrowMs == b.iGrowMs && a.iShrinkMs == b.iShrinkMs &&
               a.bScale == b.bScale && a.bMaxLine == b.bMaxLine && a.uMaxColor == b.uMaxColor;
    }

    // The graphs' settings, chosen in the Graphs dialog and saved with the Visual page
    VisualSettings::GraphSettings g_Graph;

    bool PickColor( HWND hWnd, COLORREF &cr );
    VOID DrawSwatch( LPDRAWITEMSTRUCT pdis );

    INT_PTR CALLBACK GraphProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        typedef VisualSettings::GraphSettings GS;
        GS *p = ( GS * )GetWindowLongPtrW( hWnd, GWLP_USERDATA );
        static int s_aiPlace[GS::SeriesCount];     // the places shown (before a change: to swap with)
        // The controls as settings, and back (one box: only one filled, the one just set if any)
        auto Read = [&]( GS &g ) {
            for ( int i = 0; i < GS::SeriesCount; i++ )
            {
                g.abShow[i] = IsDlgButtonChecked( hWnd, IDC_GRAPH_SHOW0 + i ) == BST_CHECKED;
                g.abLine[i] = SendDlgItemMessageW( hWnd, IDC_GRAPH_STYLE0 + i, CB_GETCURSEL, 0, 0 ) == 1;
                g.abRight[i] = SendDlgItemMessageW( hWnd, IDC_GRAPH_ALIGN0 + i, CB_GETCURSEL, 0, 0 ) == 1;
                g.aiPlace[i] = (int)SendDlgItemMessageW( hWnd, IDC_GRAPH_PLACE0 + i, CB_GETCURSEL, 0, 0 );
                g.aColor[i] = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_GRAPH_COLOR0 + i ), GWLP_USERDATA );
            }
            g.bTogether = IsDlgButtonChecked( hWnd, IDC_GRAPH_TOGETHER ) == BST_CHECKED;
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
                CheckDlgButton( hWnd, IDC_GRAPH_TOGETHER, p->bTogether ? BST_CHECKED : BST_UNCHECKED );
                ShowStyles( *p );
                for ( const wchar_t *s : { L"At zero", L"At the lowest value shown" } )
                    SendDlgItemMessageW( hWnd, IDC_GRAPH_BOTTOM, CB_ADDSTRING, 0, ( LPARAM )s );
                SendDlgItemMessageW( hWnd, IDC_GRAPH_BOTTOM, CB_SETCURSEL, p->bZero ? 0 : 1, 0 );
                // By VisualSettings::StatsCorner bits
                for ( const wchar_t *s : { L"Top right", L"Top left", L"Bottom right", L"Bottom left" } )
                    SendDlgItemMessageW( hWnd, IDC_GRAPH_CORNER, CB_ADDSTRING, 0, ( LPARAM )s );
                SendDlgItemMessageW( hWnd, IDC_GRAPH_CORNER, CB_SETCURSEL, p->iCorner, 0 );
                for ( const wchar_t *s : { L"The right edge", L"The middle" } )
                    SendDlgItemMessageW( hWnd, IDC_GRAPH_NOW, CB_ADDSTRING, 0, ( LPARAM )s );
                SendDlgItemMessageW( hWnd, IDC_GRAPH_NOW, CB_SETCURSEL, p->bMiddle, 0 );
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
                if ( bStyle || bShow || bPlace || iId == IDC_GRAPH_TOGETHER )
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
                    // (one just made filled in one box: the others become lines)
                    if ( bStyle && g.bTogether && !g.abLine[iId - IDC_GRAPH_STYLE0] )
                        for ( int i = 0; i < GS::SeriesCount; i++ ) if ( i != iId - IDC_GRAPH_STYLE0 ) g.abLine[i] = true;
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
                    p->bMiddle = SendDlgItemMessageW( hWnd, IDC_GRAPH_NOW, CB_GETCURSEL, 0, 0 ) == 1;
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

    VOID ShowStatsCount( HWND hWnd )
    {
        wchar_t buf[64];
        int iShown = __builtin_popcount( g_Stats.uStats );
        if ( !iShown ) swprintf( buf, 64, L"Hidden (no lines)" );
        else swprintf( buf, 64, L"%d of %d lines", iShown, VisualSettings::StatCount );
        SetDlgItemTextW( hWnd, IDC_STATSCOUNT, buf );
    }

    // Statistics...: a checked list of every line in its order (moved with the buttons), and a
    // three-state box per section that shows or hides all of its lines
    struct StatsDialog
    {
        StatsChoice *pOut;
        StatsChoice c;
        bool bFilling = false;

        HWND List( HWND hWnd ) const { return GetDlgItem( hWnd, IDC_STATS_LIST ); }

        // The list from c, with item iSel selected
        void Fill( HWND hWnd, int iSel )
        {
            HWND hList = List( hWnd );
            bFilling = true;
            SendMessageW( hList, WM_SETREDRAW, FALSE, 0 );
            ListView_DeleteAllItems( hList );
            for ( int i = 0; i < VisualSettings::StatCount; i++ )
            {
                int iStat = c.aOrder[i];
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
            ShowSections( hWnd );
        }

        // Each section's box: checked when all its lines are shown, unchecked when none, else mixed
        void ShowSections( HWND hWnd ) const
        {
            for ( int iSec = 0; iSec < VisualSettings::SectionCount; iSec++ )
            {
                int iOn = 0, iAll = 0;
                for ( int i = 0; i < VisualSettings::StatCount; i++ )
                    if ( VisualSettings::StatSections[i] == iSec ) { iAll++; iOn += ( c.uStats >> i ) & 1; }
                CheckDlgButton( hWnd, IDC_STATS_SECTION0 + iSec, iOn == 0 ? BST_UNCHECKED : iOn == iAll ? BST_CHECKED : BST_INDETERMINATE );
            }
        }

        void Move( HWND hWnd, int iDelta )
        {
            int i = ListView_GetNextItem( List( hWnd ), -1, LVNI_SELECTED );
            int j = i + iDelta;
            if ( i < 0 || j < 0 || j >= VisualSettings::StatCount ) return;
            std::swap( c.aOrder[i], c.aOrder[j] );
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
                return TRUE;
            }
            case WM_NOTIFY:
            {
                LPNMHDR pnm = ( LPNMHDR )lParam;
                if ( !pDlg || pnm->idFrom != IDC_STATS_LIST || pnm->code != LVN_ITEMCHANGED || pDlg->bFilling ) break;
                // A checkbox clicked (its state image changed)
                LPNMLISTVIEW pnlv = ( LPNMLISTVIEW )lParam;
                if ( !( pnlv->uChanged & LVIF_STATE ) || ( ( pnlv->uNewState ^ pnlv->uOldState ) & LVIS_STATEIMAGEMASK ) == 0 ) break;
                int iStat = pDlg->c.aOrder[pnlv->iItem];
                if ( ListView_GetCheckState( pnm->hwndFrom, pnlv->iItem ) ) pDlg->c.uStats |= 1u << iStat;
                else pDlg->c.uStats &= ~( 1u << iStat );
                pDlg->ShowSections( hWnd );
                return TRUE;
            }
            case WM_COMMAND:
            {
                int iId = LOWORD( wParam );
                if ( iId >= IDC_STATS_SECTION0 && iId < IDC_STATS_SECTION0 + VisualSettings::SectionCount )
                {
                    // All of the section's lines shown: hide them; otherwise show them all
                    int iSec = iId - IDC_STATS_SECTION0;
                    bool bShow = IsDlgButtonChecked( hWnd, iId ) != BST_CHECKED;
                    for ( int i = 0; i < VisualSettings::StatCount; i++ )
                        if ( VisualSettings::StatSections[i] == iSec )
                        {
                            if ( bShow ) pDlg->c.uStats |= 1u << i;
                            else pDlg->c.uStats &= ~( 1u << i );
                        }
                    pDlg->Fill( hWnd, ListView_GetNextItem( pDlg->List( hWnd ), -1, LVNI_SELECTED ) );
                    return TRUE;
                }
                switch ( iId )
                {
                    case IDC_STATS_UP: pDlg->Move( hWnd, -1 ); return TRUE;
                    case IDC_STATS_DOWN: pDlg->Move( hWnd, 1 ); return TRUE;
                    case IDC_STATS_RESETORDER:
                        std::copy( VisualSettings::DefaultStatOrder, VisualSettings::DefaultStatOrder + VisualSettings::StatCount, pDlg->c.aOrder );
                        pDlg->Fill( hWnd, 0 );
                        return TRUE;
                    case IDC_STATS_ALL: case IDC_STATS_NONE:
                        pDlg->c.uStats = iId == IDC_STATS_ALL ? VisualSettings::AllStats : 0;
                        pDlg->Fill( hWnd, ListView_GetNextItem( pDlg->List( hWnd ), -1, LVNI_SELECTED ) );
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

    // The image's controls only while it is shown
    VOID EnableBackground( HWND hWnd )
    {
        BOOL bOn = IsDlgButtonChecked( hWnd, IDC_BKGON ) == BST_CHECKED;
        for ( int iId : { IDC_BKGIMAGE, IDC_BKGBROWSE, IDC_BKGFIT, IDC_BKGOPACITY } ) EnableWindow( GetDlgItem( hWnd, iId ), bOn );
    }

    // Sets the values in the visual settings page. Used at init and when restoring defaults
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

    VOID SetVisualProc( HWND hWnd, const VisualSettings &cVisual )
    {
        g_Stats.uStats = cVisual.uStats;
        std::copy( cVisual.aStatOrder, cVisual.aStatOrder + VisualSettings::StatCount, g_Stats.aOrder );
        g_Graph = cVisual.graph;
        CheckDlgButton( hWnd, IDC_GRAPHON, cVisual.bGraph ? BST_CHECKED : BST_UNCHECKED );     // (also View > Graphs)
        ShowStatsCount( hWnd );
        for ( int i = 0; i < 5; i++ )
            CheckDlgButton( hWnd, aKeyModeIds[i], i == cVisual.eKeysShown ? BST_CHECKED : BST_UNCHECKED );
        EnableCustomKeys( hWnd, cVisual.eKeysShown == VisualSettings::Custom );
        CheckDlgButton( hWnd, IDC_SHOWCONTROLS, cVisual.bAlwaysShowControls ? BST_CHECKED : BST_UNCHECKED );
        SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_SETCURSEL, cVisual.iFirstKey, 0 );
        SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_SETCURSEL, cVisual.iLastKey, 0 );
        std::copy( cVisual.colors, cVisual.colors + VisualSettings::MaxColors, g_aPalette );
        SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETRANGE32, 1, VisualSettings::MaxColors );
        SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETPOS32, 0, cVisual.iColorCount );
        InvalidateRect( GetDlgItem( hWnd, IDC_PALETTE ), nullptr, FALSE );
        SetWindowLongPtrW( GetDlgItem( hWnd, IDC_BKGCOLOR ), GWLP_USERDATA, cVisual.iBkgColor );
        SetWindowLongPtrW( GetDlgItem( hWnd, IDC_BARCOLOR ), GWLP_USERDATA, cVisual.iBarColor );
        InvalidateRect( GetDlgItem( hWnd, IDC_BKGCOLOR ), nullptr, FALSE );
        InvalidateRect( GetDlgItem( hWnd, IDC_BARCOLOR ), nullptr, FALSE );
        CheckDlgButton( hWnd, IDC_BKGON, cVisual.bBkgImage ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_HIDELINES, cVisual.bHideLines ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_TICKBASED, cVisual.bTickBased ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_PFALAG, cVisual.bPfaLag ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_PFASTATS, cVisual.bPfaLiveStats ? BST_CHECKED : BST_UNCHECKED );
        {
            // The cache sizes in KB (empty: this computer's, which the label says)
            const int aiId[2] = { IDC_PFAL2, IDC_PFAL3 }, aiKB[2] = { cVisual.iPfaL2KB, cVisual.iPfaL3KB };
            for ( int i = 0; i < 2; i++ )
                if ( aiKB[i] > 0 ) SetDlgItemInt( hWnd, aiId[i], (UINT)aiKB[i], FALSE );
                else SetDlgItemTextW( hWnd, aiId[i], L"" );
            // (left empty, this computer's sizes show greyed in the boxes)
            PfaLag::Cpu cpu = PfaLag::Cpu::Detect();
            wchar_t buf[32];
            swprintf( buf, 32, L"%llu", (unsigned long long)( cpu.llL2 >> 10 ) );
            SendDlgItemMessageW( hWnd, IDC_PFAL2, EM_SETCUEBANNER, TRUE, ( LPARAM )buf );
            swprintf( buf, 32, L"%llu", (unsigned long long)( cpu.llL3 >> 10 ) );
            SendDlgItemMessageW( hWnd, IDC_PFAL3, EM_SETCUEBANNER, TRUE, ( LPARAM )buf );
            swprintf( buf, 32, L"%g", cVisual.iPfaSendNs / 1000.0 );
            SetDlgItemTextW( hWnd, IDC_PFASEND, buf );
            SetDlgItemInt( hWnd, IDC_PFASPEED, (UINT)cVisual.iPfaCpuPct, FALSE );
            SetPfaBenchText( hWnd, cVisual.bPfaLag );
            for ( int iId : { IDC_PFAL2, IDC_PFAL3, IDC_PFASEND, IDC_PFASPEED, IDC_PFASTATS } ) EnableWindow( GetDlgItem( hWnd, iId ), cVisual.bPfaLag );
        }
        CheckDlgButton( hWnd, IDC_LOOPCOLORS, cVisual.bLoopColors ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_STATSSCALED, cVisual.bStatsScaled ? BST_CHECKED : BST_UNCHECKED );
        SetDlgItemInt( hWnd, IDC_STATSSIZE, (UINT)cVisual.iStatsCX, FALSE );
        SetDlgItemInt( hWnd, IDC_STATSHEIGHT, (UINT)cVisual.iStatsCY, FALSE );
        // (the font's size or the height: one or the other)
        CheckDlgButton( hWnd, IDC_STATSFONTSET, cVisual.bStatsFontSet ? BST_CHECKED : BST_UNCHECKED );
        SetDlgItemInt( hWnd, IDC_STATSFONTPX, (UINT)cVisual.iStatsFontPx, FALSE );
        EnableWindow( GetDlgItem( hWnd, IDC_STATSHEIGHT ), !cVisual.bStatsFontSet );
        EnableWindow( GetDlgItem( hWnd, IDC_STATSFONTPX ), cVisual.bStatsFontSet );
        SendDlgItemMessageW( hWnd, IDC_STATSCORNER, CB_SETCURSEL, cVisual.iStatsCorner, 0 );
        SetDlgItemTextW( hWnd, IDC_STATSFONT, cVisual.sStatsFont.c_str() );
        CheckDlgButton( hWnd, IDC_STATSBKG, cVisual.bStatsBackground ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_STATSSHADOW, cVisual.bStatsShadow ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_STATSMIRROR, cVisual.bStatsShadowMirror ? BST_CHECKED : BST_UNCHECKED );
        EnableWindow( GetDlgItem( hWnd, IDC_STATSMIRROR ), cVisual.bStatsShadow );
        CheckDlgButton( hWnd, IDC_HIDESPEEDBOXES, cVisual.bHideSpeedBoxes ? BST_CHECKED : BST_UNCHECKED );
        SetDlgItemTextW( hWnd, IDC_BKGIMAGE, cVisual.sBkgImage.c_str() );
        SendDlgItemMessageW( hWnd, IDC_BKGFIT, CB_SETCURSEL, cVisual.iBkgFit, 0 );
        SetDlgItemInt( hWnd, IDC_BKGOPACITY, (UINT)cVisual.iBkgOpacity, FALSE );
        for ( int i = 0; i < 3; i++ )
            CheckDlgButton( hWnd, IDC_CLEAN_MINHEIGHT + i, ( cVisual.uClean >> i ) & 1 ? BST_CHECKED : BST_UNCHECKED );
        EnableBackground( hWnd );
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
        MessageBoxW( hWnd, buf, L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, iId ), TRUE );
        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
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

    INT_PTR CALLBACK VisualProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
                for ( int i = 0; i < 256; i++ )
                {
                    SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_ADDSTRING, 0, ( LPARAM )Notes::NoteName( i ) );
                    SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_ADDSTRING, 0, ( LPARAM )Notes::NoteName( i ) );
                }
                // By Renderer::ImageFit
                for ( const wchar_t *s : { L"Fill (cropped)", L"Fit (whole image)", L"Stretch", L"Center", L"Tile" } )
                    SendDlgItemMessageW( hWnd, IDC_BKGFIT, CB_ADDSTRING, 0, ( LPARAM )s );
                // By VisualSettings::StatsCorner bits
                for ( const wchar_t *s : { L"Top right", L"Top left", L"Bottom right", L"Bottom left" } )
                    SendDlgItemMessageW( hWnd, IDC_STATSCORNER, CB_ADDSTRING, 0, ( LPARAM )s );
                // The installed fonts (each family once; not the vertical "@" ones)
                {
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
                }
                // By DarkMode::Theme
                for ( const wchar_t *s : { L"Light", L"Dark", L"Follow Windows" } )
                    SendDlgItemMessageW( hWnd, IDC_THEME, CB_ADDSTRING, 0, ( LPARAM )s );
                SendDlgItemMessageW( hWnd, IDC_THEME, CB_SETCURSEL, Config::GetConfig().GetViewSettings().GetTheme(), 0 );
                SetVisualProc( hWnd, Config::GetConfig().GetVisualSettings() );
                g_vPalettes = Config::GetConfig().GetVisualSettings().vPalettes;
                ShowPalettes( hWnd, -1 );
                SetWindowSizeEdits( hWnd );
                return TRUE;
            // Draws the coloured buttons
            case WM_DRAWITEM:
            {
                LPDRAWITEMSTRUCT pdis = ( LPDRAWITEMSTRUCT )lParam;
                if ( pdis->CtlID == IDC_PALETTE )
                {
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
                if ( pdis->CtlID != IDC_BKGCOLOR && pdis->CtlID != IDC_BARCOLOR ) return FALSE;
                DrawSwatch( pdis );
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
                    case IDC_STATSSHADOW:
                        EnableWindow( GetDlgItem( hWnd, IDC_STATSMIRROR ), IsDlgButtonChecked( hWnd, IDC_STATSSHADOW ) == BST_CHECKED );
                        return TRUE;
                    case IDC_STATSFONTSET:
                    {
                        const BOOL bSet = IsDlgButtonChecked( hWnd, IDC_STATSFONTSET ) == BST_CHECKED;
                        EnableWindow( GetDlgItem( hWnd, IDC_STATSHEIGHT ), !bSet );
                        EnableWindow( GetDlgItem( hWnd, IDC_STATSFONTPX ), bSet );
                        return TRUE;
                    }
                    case IDC_PFALAG:
                    {
                        BOOL bOn = IsDlgButtonChecked( hWnd, IDC_PFALAG ) == BST_CHECKED;
                        for ( int iId : { IDC_PFAL2, IDC_PFAL3, IDC_PFASEND, IDC_PFASPEED, IDC_PFASTATS } ) EnableWindow( GetDlgItem( hWnd, iId ), bOn );
                        SetPfaBenchText( hWnd, bOn );
                        return TRUE;
                    }
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
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_STATS ), GetParent( hWnd ), StatsProc,
                                              ( LPARAM )&c ) == IDOK &&
                             ( c.uStats != g_Stats.uStats || !std::equal( c.aOrder, c.aOrder + VisualSettings::StatCount, g_Stats.aOrder ) ) )
                        {
                            g_Stats = c;
                            ShowStatsCount( hWnd );
                            Changed( hWnd );
                        }
                        return TRUE;
                    }
                    case IDC_COLORCOUNT:
                        if ( HIWORD( wParam ) == EN_CHANGE ) InvalidateRect( GetDlgItem( hWnd, IDC_PALETTE ), nullptr, FALSE );
                        return TRUE;
                    // Saved palettes: one chosen fills the editor; the editor's saved under a name; the
                    // default palette as a new one
                    case IDC_PALETTES:
                    {
                        if ( HIWORD( wParam ) != CBN_SELCHANGE ) return TRUE;
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_PALETTES, CB_GETCURSEL, 0, 0 );
                        if ( i >= 0 && i < (int)g_vPalettes.size() ) ShowColors( hWnd, g_vPalettes[i].vColors.data(), (int)g_vPalettes[i].vColors.size() );
                        EnableWindow( GetDlgItem( hWnd, IDC_PALETTEDELETE ), i >= 0 );
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
                    // A colour: the chooser. In the palette, the cell clicked (with the keyboard, the first); one
                    // past the colours in use adds it, and those before it
                    case IDC_PALETTE:
                    case IDC_BKGCOLOR:
                    case IDC_BARCOLOR:
                    {
                        HWND hWndBtn = ( HWND )lParam;
                        int iCell = -1;
                        if ( iId == IDC_PALETTE )
                        {
                            DWORD dwPos = GetMessagePos();
                            POINT pt = { (short)LOWORD( dwPos ), (short)HIWORD( dwPos ) };
                            ScreenToClient( hWndBtn, &pt );
                            RECT rc;
                            GetClientRect( hWndBtn, &rc );
                            iCell = 0;
                            if ( PtInRect( &rc, pt ) )
                                for ( int i = 0; i < VisualSettings::MaxColors; i++ )
                                {
                                    RECT rcCell = PaletteCell( rc, i );
                                    if ( PtInRect( &rcCell, pt ) ) { iCell = i; break; }
                                }
                        }
                        COLORREF cr = iCell >= 0 ? g_aPalette[iCell] : ( COLORREF )GetWindowLongPtrW( hWndBtn, GWLP_USERDATA ), crOld = cr;
                        if ( !PickColor( hWnd, cr ) ) return TRUE;
                        if ( cr != crOld || ( iCell >= 0 && iCell >= PaletteCount( hWnd ) ) ) Changed( hWnd );
                        if ( iCell >= 0 )
                        {
                            g_aPalette[iCell] = cr;
                            if ( iCell >= PaletteCount( hWnd ) ) SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETPOS32, 0, iCell + 1 );
                        }
                        else SetWindowLongPtrW( hWndBtn, GWLP_USERDATA, cr );
                        InvalidateRect( hWndBtn, nullptr, FALSE );
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
                    case IDC_RESTOREDEFAULTS:
                    {
                        VisualSettings cVisual;
                        cVisual.LoadDefaultValues();
                        SendMessageW( hWnd, WM_SETREDRAW, FALSE, 0 );
                        SetVisualProc( hWnd, cVisual );
                        SendMessageW( hWnd, WM_SETREDRAW, TRUE, 0 );
                        InvalidateRect( hWnd, nullptr, FALSE );
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
                    VisualSettings cVisual = config.GetVisualSettings();
                    ViewSettings &cView = config.GetViewSettings();
                    bool bAlwaysShowControls = cVisual.bAlwaysShowControls;
                    int iWidth = (int)g_szWindow.cx, iHeight = (int)g_szWindow.cy;
                    BOOL bOk = FALSE;
                    UINT uOpacity = GetDlgItemInt( hWnd, IDC_BKGOPACITY, &bOk, FALSE );
                    bool bBkgOn = IsDlgButtonChecked( hWnd, IDC_BKGON ) == BST_CHECKED;
                    if ( !bOk && !bBkgOn ) uOpacity = (UINT)config.GetVisualSettings().iBkgOpacity;    // not checked while the image is off
                    else if ( bBkgOn && ( !bOk || uOpacity > 100 ) )
                    {
                        MessageBoxW( hWnd, L"Please specify a background image opacity between 0 and 100 percent", L"Error", MB_OK | MB_ICONEXCLAMATION );
                        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_BKGOPACITY ), TRUE );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }
                    // The stats box's size
                    bool bStatsScaled = IsDlgButtonChecked( hWnd, IDC_STATSSCALED ) == BST_CHECKED;
                    // (the width, then the height or the font's size, whichever is set: the other kept)
                    const bool bStatsFontSet = IsDlgButtonChecked( hWnd, IDC_STATSFONTSET ) == BST_CHECKED;
                    int aiStatsSize[3] = { 0, config.GetVisualSettings().iStatsCY, config.GetVisualSettings().iStatsFontPx };
                    {
                        const int aiId[3] = { IDC_STATSSIZE, IDC_STATSHEIGHT, IDC_STATSFONTPX };
                        const int aiMin[3] = { VisualSettings::StatsCXMin, VisualSettings::StatsCYMin, VisualSettings::StatsFontMin };
                        const int aiMax[3] = { VisualSettings::StatsCXMax, VisualSettings::StatsCYMax, VisualSettings::StatsFontMax };
                        static const wchar_t *const asWhat[3] = { L"width", L"height", L"font size" };
                        for ( int i = 0; i < 3; i++ )
                        {
                            if ( i && ( i == 2 ) != bStatsFontSet ) continue;
                            UINT u = GetDlgItemInt( hWnd, aiId[i], &bOk, FALSE );
                            if ( !bOk || u < (UINT)aiMin[i] || u > (UINT)aiMax[i] )
                            {
                                wchar_t sMsg[128];
                                swprintf( sMsg, 128, L"Please specify a stats box %ls between %d and %d pixels", asWhat[i], aiMin[i], aiMax[i] );
                                MessageBoxW( hWnd, sMsg, L"Error", MB_OK | MB_ICONEXCLAMATION );
                                PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, aiId[i] ), TRUE );
                                SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                                return TRUE;
                            }
                            aiStatsSize[i] = (int)u;
                        }
                    }
                    wchar_t sImage[1024] = {};
                    GetDlgItemTextW( hWnd, IDC_BKGIMAGE, sImage, 1024 );
                    if ( bBkgOn && !sImage[0] )
                    {
                        MessageBoxW( hWnd, L"Please choose a background image file, or uncheck the background image.", L"Error", MB_OK | MB_ICONEXCLAMATION );
                        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_BKGIMAGE ), TRUE );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }
                    if ( bBkgOn && GetFileAttributesW( sImage ) == INVALID_FILE_ATTRIBUTES )
                    {
                        MessageBoxW( hWnd, L"The background image file was not found. Please choose an existing file, or leave the box empty for no image.", L"Error", MB_OK | MB_ICONEXCLAMATION );
                        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_BKGIMAGE ), TRUE );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }
                    if ( !cView.GetFullScreen() )
                    {
                        SIZE szMin = MainWindowMinSize(), szMax = MainWindowMaxSize();
                        if ( !ReadSize( hWnd, IDC_WINWIDTH, (int)szMin.cx, (int)szMax.cx, (int)g_szWindow.cx, L"width", iWidth ) ) return TRUE;
                        if ( !ReadSize( hWnd, IDC_WINHEIGHT, (int)szMin.cy, (int)szMax.cy, (int)g_szWindow.cy, L"height", iHeight ) ) return TRUE;
                    }

                    cVisual.eKeysShown = VisualSettings::All128;
                    for ( int i = 0; i < 5; i++ )
                        if ( IsDlgButtonChecked( hWnd, aKeyModeIds[i] ) == BST_CHECKED )
                            cVisual.eKeysShown = static_cast< VisualSettings::KeysShown >( i );
                    cVisual.bAlwaysShowControls = IsDlgButtonChecked( hWnd, IDC_SHOWCONTROLS ) == BST_CHECKED;
                    cVisual.uStats = g_Stats.uStats;
                    cVisual.graph = g_Graph;
                    std::copy( g_Stats.aOrder, g_Stats.aOrder + VisualSettings::StatCount, cVisual.aStatOrder );
                    cVisual.iFirstKey = (int)SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_GETCURSEL, 0, 0 );
                    cVisual.iLastKey = (int)SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_GETCURSEL, 0, 0 );
                    std::copy( g_aPalette, g_aPalette + VisualSettings::MaxColors, cVisual.colors );
                    cVisual.vPalettes = g_vPalettes;
                    cVisual.iColorCount = PaletteCount( hWnd );
                    cVisual.iBkgColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_BKGCOLOR ), GWLP_USERDATA );
                    cVisual.iBarColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_BARCOLOR ), GWLP_USERDATA );
                    cVisual.bBkgImage = bBkgOn;
                    cVisual.sBkgImage = sImage;
                    cVisual.iBkgOpacity = std::min( 100, (int)uOpacity );
                    cVisual.bHideLines = IsDlgButtonChecked( hWnd, IDC_HIDELINES ) == BST_CHECKED;
                    cVisual.bTickBased = IsDlgButtonChecked( hWnd, IDC_TICKBASED ) == BST_CHECKED;
                    cVisual.bPfaLag = IsDlgButtonChecked( hWnd, IDC_PFALAG ) == BST_CHECKED;
                    cVisual.bPfaLiveStats = IsDlgButtonChecked( hWnd, IDC_PFASTATS ) == BST_CHECKED;
                    {
                        const int aiId[2] = { IDC_PFAL2, IDC_PFAL3 };
                        int *apKB[2] = { &cVisual.iPfaL2KB, &cVisual.iPfaL3KB };
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
                        if ( pEnd != sSend && dSend >= 0.0 ) cVisual.iPfaSendNs = (int)( std::min( dSend, 10000.0 ) * 1000.0 + 0.5 );
                        // (a percentage, 10 to 1000; empty or unreadable: this computer's)
                        BOOL bNum = FALSE;
                        UINT uPct = GetDlgItemInt( hWnd, IDC_PFASPEED, &bNum, FALSE );
                        cVisual.iPfaCpuPct = bNum ? std::clamp( (int)uPct, 10, 1000 ) : 100;
                    }
                    cVisual.bLoopColors = IsDlgButtonChecked( hWnd, IDC_LOOPCOLORS ) == BST_CHECKED;
                    cVisual.bStatsScaled = bStatsScaled;
                    cVisual.iStatsCX = aiStatsSize[0];
                    cVisual.iStatsCY = aiStatsSize[1];
                    cVisual.bStatsFontSet = bStatsFontSet;
                    cVisual.iStatsFontPx = aiStatsSize[2];
                    cVisual.iStatsCorner = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_STATSCORNER, CB_GETCURSEL, 0, 0 ) );
                    wchar_t sFont[LF_FACESIZE] = L"";
                    GetDlgItemTextW( hWnd, IDC_STATSFONT, sFont, LF_FACESIZE );
                    cVisual.sStatsFont = sFont[0] ? sFont : L"Tahoma";
                    cVisual.bStatsBackground = IsDlgButtonChecked( hWnd, IDC_STATSBKG ) == BST_CHECKED;
                    cVisual.bStatsShadow = IsDlgButtonChecked( hWnd, IDC_STATSSHADOW ) == BST_CHECKED;
                    cVisual.bStatsShadowMirror = IsDlgButtonChecked( hWnd, IDC_STATSMIRROR ) == BST_CHECKED;
                    bool bOldHideBoxes = cVisual.bHideSpeedBoxes;
                    cVisual.bHideSpeedBoxes = IsDlgButtonChecked( hWnd, IDC_HIDESPEEDBOXES ) == BST_CHECKED;
                    cVisual.iBkgFit = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_BKGFIT, CB_GETCURSEL, 0, 0 ) );
                    cVisual.uClean = 0;
                    for ( int i = 0; i < 3; i++ )
                        if ( IsDlgButtonChecked( hWnd, IDC_CLEAN_MINHEIGHT + i ) == BST_CHECKED ) cVisual.uClean |= 1u << i;

                    config.SetVisualSettings( cVisual );
                    if ( bOldHideBoxes != cVisual.bHideSpeedBoxes ) ShowSpeedBoxes( !cVisual.bHideSpeedBoxes );
                    if ( cView.GetFullScreen() && bAlwaysShowControls != cVisual.bAlwaysShowControls )
                        cView.SetControls( cView.GetControls(), true );
                    if ( iWidth != g_szWindow.cx || iHeight != g_szWindow.cy )
                    {
                        SetMainWindowSize( iWidth, iHeight );
                        SetWindowSizeEdits( hWnd );
                    }
                    bool bGraph = IsDlgButtonChecked( hWnd, IDC_GRAPHON ) == BST_CHECKED;
                    if ( bGraph != config.GetVisualSettings().bGraph )
                    {
                        VisualSettings c = config.GetVisualSettings();
                        c.bGraph = bGraph;
                        config.SetVisualSettings( c );
                        ShowGraphs( bGraph );
                    }
                    int eTheme = (int)SendDlgItemMessageW( hWnd, IDC_THEME, CB_GETCURSEL, 0, 0 );
                    if ( eTheme >= 0 && eTheme < DarkMode::ThemeCount && eTheme != cView.GetTheme() ) cView.SetTheme( eTheme, true );
                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_NOERROR );
                    return TRUE;
                }
                break;
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Audio
    //-------------------------------------------------------------------------

    VOID SetAudioProc( HWND hWnd )
    {
        Config::GetConfig().LoadMIDIDevices();
        const AudioSettings &cAudio = Config::GetConfig().GetAudioSettings();
        HWND hWndOutDevs = GetDlgItem( hWnd, IDC_MIDIOUT );
        SendMessageW( hWndOutDevs, LB_RESETCONTENT, 0, 0 );
        for ( const std::wstring &s : cAudio.vMIDIOutDevices )
            SendMessageW( hWndOutDevs, LB_ADDSTRING, 0, ( LPARAM )s.c_str() );
        SendMessageW( hWndOutDevs, LB_SETCURSEL, cAudio.iOutDevice, 0 );
        SetDlgItemTextW( hWnd, IDC_KDMAPISTATUS, MidiOutputs::KdmapiStatus().c_str() );
    }

    INT_PTR CALLBACK AudioProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                SetAudioProc( hWnd );
                const PerformanceSettings &cPerf = Config::GetConfig().GetPerformanceSettings();
                CheckDlgButton( hWnd, IDC_PERF_KDMAPI, cPerf.bPreferKDMAPI ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_LONGSENDS, cPerf.bKdmapiLongSends ? BST_CHECKED : BST_UNCHECKED );
                EnableWindow( GetDlgItem( hWnd, IDC_PERF_KDMAPI ), MidiOutputs::KdmapiAvailable() || cPerf.bPreferKDMAPI );
                // 31-EDO: the keys' layout and the tuning
                CheckDlgButton( hWnd, IDC_EDO31KEYS, Config::GetConfig().GetVisualSettings().bEdo31Keys ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_TUNING31, cPerf.bTuning31 ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_TUNING31BEND, cPerf.bTuning31PitchBend ? BST_CHECKED : BST_UNCHECKED );
                EnableWindow( GetDlgItem( hWnd, IDC_PERF_TUNING31BEND ), cPerf.bTuning31 );
                return TRUE;
            }
            case WM_COMMAND:
                if ( LOWORD( wParam ) == IDC_PERF_TUNING31 && HIWORD( wParam ) == BN_CLICKED )
                    EnableWindow( GetDlgItem( hWnd, IDC_PERF_TUNING31BEND ), IsDlgButtonChecked( hWnd, IDC_PERF_TUNING31 ) == BST_CHECKED );
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                break;
            case WM_DEVICECHANGE:
                Sleep( 200 );
                SetAudioProc( hWnd );
                break;
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    AudioSettings cAudio = config.GetAudioSettings();
                    cAudio.iOutDevice = (int)SendDlgItemMessageW( hWnd, IDC_MIDIOUT, LB_GETCURSEL, 0, 0 );
                    if ( cAudio.iOutDevice >= 0 && cAudio.iOutDevice < (int)cAudio.vMIDIOutDevices.size() )
                        cAudio.sDesiredOut = cAudio.vMIDIOutDevices[cAudio.iOutDevice];
                    bool bChanged = cAudio.iOutDevice != config.GetAudioSettings().iOutDevice;
                    config.SetAudioSettings( cAudio );
                    PerformanceSettings cPerf = config.GetPerformanceSettings();
                    bool bKdmapi = IsDlgButtonChecked( hWnd, IDC_PERF_KDMAPI ) == BST_CHECKED;
                    bChanged |= bKdmapi != cPerf.bPreferKDMAPI;
                    cPerf.bPreferKDMAPI = bKdmapi;
                    cPerf.bKdmapiLongSends = IsDlgButtonChecked( hWnd, IDC_PERF_LONGSENDS ) == BST_CHECKED;
                    cPerf.bTuning31 = IsDlgButtonChecked( hWnd, IDC_PERF_TUNING31 ) == BST_CHECKED;
                    cPerf.bTuning31PitchBend = IsDlgButtonChecked( hWnd, IDC_PERF_TUNING31BEND ) == BST_CHECKED;
                    config.SetPerformanceSettings( cPerf );
                    VisualSettings cVisual = config.GetVisualSettings();
                    cVisual.bEdo31Keys = IsDlgButtonChecked( hWnd, IDC_EDO31KEYS ) == BST_CHECKED;
                    config.SetVisualSettings( cVisual );
                    if ( bChanged ) HandOffMsg( WM_DEVICECHANGE, 0, 0 );
                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_NOERROR );
                    return TRUE;
                }
                break;
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

    // The hit line's settings mean nothing without it
    VOID EnableFxHitLine( HWND hWnd )
    {
        BOOL bOn = IsDlgButtonChecked( hWnd, IDC_FX_HITLINE ) == BST_CHECKED;
        for ( int iId : { IDC_FX_HITCOLOR, IDC_FX_HITNOTECOLOR, IDC_FX_HITSIZE, IDC_FX_HITSOFT, IDC_FX_HITSTRENGTH, IDC_FX_HITGLOW, IDC_FX_HITPLAYING,
                           IDC_FX_HITFADEMS, IDC_FX_HITCURVE, IDC_FX_HITSMEAR } )
            EnableWindow( GetDlgItem( hWnd, iId ), bOn );
    }

    // The look chosen (EffectSettings::Look)
    int FxLook( HWND hWnd )
    {
        return std::clamp( (int)SendDlgItemMessageW( hWnd, IDC_FX_FLAT, CB_GETCURSEL, 0, 0 ), 0, EffectSettings::LookCount - 1 );
    }

    // Each look's own settings mean nothing without it (and merged outlines nothing with the flat look: it has none)
    VOID EnableFxFlat( HWND hWnd )
    {
        BOOL bOn = FxLook( hWnd ) == EffectSettings::LookFlat;
        EnableWindow( GetDlgItem( hWnd, IDC_FX_FLATBORDERS ), bOn );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_SEMIOUTLINE ), FxLook( hWnd ) == EffectSettings::LookSemiFlat );
        EnableWindow( GetDlgItem( hWnd, IDC_FX_SEMILIP ), FxLook( hWnd ) == EffectSettings::LookSemiFlat );
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

    // The whole tab by its first box; on, each setting as the ones it depends on allow
    VOID EnableFx( HWND hWnd )
    {
        const BOOL bOn = IsDlgButtonChecked( hWnd, IDC_FX_ON ) == BST_CHECKED;
        // (the profiles stay usable)
        for ( HWND h = GetWindow( hWnd, GW_CHILD ); h; h = GetWindow( h, GW_HWNDNEXT ) )
        {
            const int iId = GetDlgCtrlID( h );
            if ( iId != IDC_FX_ON && ( iId < IDC_FX_PROFILES || iId > IDC_FX_PROFILENEW ) && iId != IDC_FX_PROFILEKEEP && h != GetWindow( GetDlgItem( hWnd, IDC_FX_PROFILES ), GW_HWNDPREV ) )
                EnableWindow( h, bOn );
        }
        if ( bOn )
        {
            EnableFxFlat( hWnd );
            EnableFxDecay( hWnd );
            EnableFxHitLine( hWnd );
            ShowKeyWhite( hWnd );
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

    // The profile with every setting as fx's (the switch aside), or -1
    int MatchingFxProfile( const EffectSettings &fx )
    {
        auto Text = []( EffectSettings f ) { f.bOn = true; IniFile ini; f.Save( ini ); return ini.Text(); };
        const std::string s = Text( fx );
        for ( size_t i = 0; i < g_vFxProfiles.size(); i++ )
            if ( Text( g_vFxProfiles[i].fx ) == s ) return (int)i;
        return -1;
    }

    // Every setting's control but the effects' switch, from fx
    VOID ShowFx( HWND hWnd, const EffectSettings &fx )
    {
        g_FxShown = fx;
        {
                SendDlgItemMessageW( hWnd, IDC_FX_FLAT, CB_SETCURSEL, fx.eLook, 0 );
                CheckDlgButton( hWnd, IDC_FX_FLATBORDERS, fx.bFlatBorders ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_SEMILIP, fx.bSemiFlatLip ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_FX_NOBAR, fx.bNoBar ? BST_CHECKED : BST_UNCHECKED );
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
                std::copy( fx.abKeyWhite, fx.abKeyWhite + EffectSettings::LookCount, g_abKeyWhite );
                std::copy( fx.aiKeyWhite, fx.aiKeyWhite + EffectSettings::LookCount, g_aiKeyWhite );
                g_iKeyWhiteLook = fx.eLook;
                ShowKeyWhite( hWnd );
                EnableFx( hWnd );
        }
        UpdateFxSave( hWnd );
    }

    // The controls over fx (a disabled number keeps fx's); a number out of its range: that one, and
    // nothing changed
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
        fx.eLook = FxLook( hWnd );
        fx.bFlatBorders = IsDlgButtonChecked( hWnd, IDC_FX_FLATBORDERS ) == BST_CHECKED;
        fx.bSemiFlatLip = IsDlgButtonChecked( hWnd, IDC_FX_SEMILIP ) == BST_CHECKED;
        fx.bNoBar = IsDlgButtonChecked( hWnd, IDC_FX_NOBAR ) == BST_CHECKED;
        fx.bShortColor = IsDlgButtonChecked( hWnd, IDC_FX_SHORTCOLOR ) == BST_CHECKED;
        fx.bHollow = IsDlgButtonChecked( hWnd, IDC_FX_HOLLOW ) == BST_CHECKED;
        fx.bMergeOutlines = IsDlgButtonChecked( hWnd, IDC_FX_MERGE ) == BST_CHECKED;
        fx.bMergeShort = IsDlgButtonChecked( hWnd, IDC_FX_MERGESHORT ) == BST_CHECKED;
        fx.bMergeShortLong = IsDlgButtonChecked( hWnd, IDC_FX_MERGESHORTLONG ) == BST_CHECKED;
        fx.bStats = IsDlgButtonChecked( hWnd, IDC_FX_STATS ) == BST_CHECKED;
        fx.bOn = IsDlgButtonChecked( hWnd, IDC_FX_ON ) == BST_CHECKED;
        KeepKeyWhite( hWnd );
        std::copy( g_abKeyWhite, g_abKeyWhite + EffectSettings::LookCount, fx.abKeyWhite );
        std::copy( g_aiKeyWhite, g_aiKeyWhite + EffectSettings::LookCount, fx.aiKeyWhite );
        fx.bHitLine = IsDlgButtonChecked( hWnd, IDC_FX_HITLINE ) == BST_CHECKED;
        fx.bHitLinePlaying = IsDlgButtonChecked( hWnd, IDC_FX_HITPLAYING ) == BST_CHECKED;
        fx.bHitLineNoteColor = IsDlgButtonChecked( hWnd, IDC_FX_HITNOTECOLOR ) == BST_CHECKED;
        fx.iHitLineColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_FX_HITCOLOR ), GWLP_USERDATA );
        fx.eDecay = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_FX_DECAY, CB_GETCURSEL, 0, 0 ) );
        fx.eHitLineCurve = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_FX_HITCURVE, CB_GETCURSEL, 0, 0 ) );
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
            auto Text = []( EffectSettings f ) { f.bOn = true; IniFile ini; f.Save( ini ); return ini.Text(); };
            EffectSettings fx = g_FxShown;
            bDiffer = ReadFx( hWnd, fx ) != nullptr || Text( fx ) != Text( g_vFxProfiles[i].fx );
        }
        EnableWindow( GetDlgItem( hWnd, IDC_FX_PROFILEKEEP ), bDiffer );
    }

    // A number out of range: said, and its box focused
    VOID FxNumberError( HWND hWnd, const FxNumber &n )
    {
        wchar_t buf[160];
        swprintf( buf, 160, L"Please specify %ls", n.sName );
        MessageBoxW( hWnd, buf, L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, n.iId ), TRUE );
    }

    INT_PTR CALLBACK EffectsProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
                // By EffectSettings::Look and Decay
                for ( const wchar_t *s : { L"Normal", L"Flat", L"Semi-flat" } )
                    SendDlgItemMessageW( hWnd, IDC_FX_FLAT, CB_ADDSTRING, 0, ( LPARAM )s );
                for ( const wchar_t *s : { L"None (steady)", L"Linear", L"Exponential", L"Smooth" } )
                    SendDlgItemMessageW( hWnd, IDC_FX_DECAY, CB_ADDSTRING, 0, ( LPARAM )s );
                for ( const wchar_t *s : { L"None (off at once)", L"Linear", L"Exponential", L"Smooth" } )
                    SendDlgItemMessageW( hWnd, IDC_FX_HITCURVE, CB_ADDSTRING, 0, ( LPARAM )s );
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
                    case IDC_FX_PROFILES:
                    {
                        if ( HIWORD( wParam ) != CBN_SELCHANGE ) return TRUE;
                        int i = (int)SendDlgItemMessageW( hWnd, IDC_FX_PROFILES, CB_GETCURSEL, 0, 0 );
                        if ( i >= 0 && i < (int)g_vFxProfiles.size() )
                        {
                            EffectSettings fx = g_vFxProfiles[i].fx;
                            fx.bOn = IsDlgButtonChecked( hWnd, IDC_FX_ON ) == BST_CHECKED;
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
    // Controls (the General group of the original)
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
        MessageBoxW( hWnd, sError, L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )hWndEdit, TRUE );
        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
        return false;
    }

    INT_PTR CALLBACK ControlsProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                const ControlsSettings &cControls = Config::GetConfig().GetControlsSettings();
                wchar_t buf[32];
                swprintf( buf, 32, L"%g", cControls.dFwdBackSecs );
                SetDlgItemTextW( hWnd, IDC_LRARROWS, buf );
                swprintf( buf, 32, L"%g", cControls.dSpeedUpPct );
                SetDlgItemTextW( hWnd, IDC_UDARROWS, buf );
                swprintf( buf, 32, L"%g", cControls.dNSpeedUpPct );
                SetDlgItemTextW( hWnd, IDC_NSARROWS, buf );
                CheckDlgButton( hWnd, IDC_STARTPAUSED, cControls.bStartPaused ? BST_CHECKED : BST_UNCHECKED );
                return TRUE;
            }
            case WM_COMMAND:
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
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
                    ControlsSettings cControls = config.GetControlsSettings();
                    if ( !ReadEdit( hWnd, IDC_LRARROWS, cControls.dFwdBackSecs, L"Please specify a numeric value for the left and right arrows" ) ) return TRUE;
                    if ( !ReadEdit( hWnd, IDC_UDARROWS, cControls.dSpeedUpPct, L"Please specify a numeric value for the up and down arrows" ) ) return TRUE;
                    if ( !ReadEdit( hWnd, IDC_NSARROWS, cControls.dNSpeedUpPct, L"Please specify a numeric value for Shift+up and down" ) ) return TRUE;
                    cControls.bStartPaused = IsDlgButtonChecked( hWnd, IDC_STARTPAUSED ) == BST_CHECKED;
                    config.SetControlsSettings( cControls );
                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_NOERROR );
                    return TRUE;
                }
                break;
            }
        }
        return FALSE;
    }

    //-------------------------------------------------------------------------
    // Performance (new)
    //-------------------------------------------------------------------------

    // A thread count edit: 0 is auto (one per logical processor)
    const int MaxThreads = 256;

    bool ReadThreads( HWND hWnd, int iId, const wchar_t *sWhat, int &iValue )
    {
        BOOL bOk = FALSE;
        UINT u = GetDlgItemInt( hWnd, iId, &bOk, FALSE );
        if ( bOk && (int)u <= MaxThreads ) { iValue = (int)u; return true; }
        wchar_t buf[128];
        swprintf( buf, 128, L"Please specify %ls between 0 (auto) and %d", sWhat, MaxThreads );
        MessageBoxW( hWnd, buf, L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, iId ), TRUE );
        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
        return false;
    }

    INT_PTR CALLBACK PerformanceProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                Config &config = Config::GetConfig();
                const PerformanceSettings &cPerf = config.GetPerformanceSettings();
                const VideoSettings &cVideo = config.GetVideoSettings();

                wchar_t buf[128];
                unsigned n = std::thread::hardware_concurrency();
                SetDlgItemInt( hWnd, IDC_PERF_THREADS, (UINT)cPerf.iWorkerThreads, FALSE );
                SetDlgItemInt( hWnd, IDC_PERF_PLAYTHREADS, (UINT)cPerf.iPlaybackThreads, FALSE );
                swprintf( buf, 128, L"(0 = auto: %u) drawing at once, loading next file", n ? n : 4 );
                SetDlgItemTextW( hWnd, IDC_PERF_THREADSNOTE, buf );
                swprintf( buf, 128, L"(0 = auto: %u) takes effect at once", n ? n : 4 );
                SetDlgItemTextW( hWnd, IDC_PERF_PLAYTHREADSNOTE, buf );

                CheckDlgButton( hWnd, IDC_PERF_SKIPPING, cPerf.bVelocitySkip ? BST_CHECKED : BST_UNCHECKED );
                SendDlgItemMessageW( hWnd, IDC_PERF_MAXLAGSPIN, UDM_SETRANGE32, 1, 10000 );
                SendDlgItemMessageW( hWnd, IDC_PERF_MAXLAGSPIN, UDM_SETPOS32, 0, cPerf.iSkipLagMs );

                CheckDlgButton( hWnd, IDC_PERF_LOADSTATS, cPerf.bShowLoadStats ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_COMPACT, cPerf.bCompactVisual ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_LOD, cPerf.bLodCache ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_INSTANCING, cPerf.bGpuInstancing ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_DEDUP, cPerf.bDedup ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_DEDUPASSUME, cPerf.bDedupAssume ? BST_CHECKED : BST_UNCHECKED );
                EnableWindow( GetDlgItem( hWnd, IDC_PERF_DEDUPASSUME ), cPerf.bDedup );
                SetDlgItemInt( hWnd, IDC_PERF_CACHEMB, (UINT)cPerf.iBlockCacheMB, FALSE );
                CheckDlgButton( hWnd, IDC_PERF_COLLAPSE, cPerf.bCollapseStacks ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_VELFLOOR, cPerf.bVelocityFloor ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_PERF_MINVEL, (UINT)cPerf.iMinVelocity, FALSE );
                CheckDlgButton( hWnd, IDC_PERF_IGNOREWEIRD, cPerf.bIgnoreWeird ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_ENDOPEN, cPerf.bEndOpenNotes ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_SENDLIMIT, cPerf.bSendLimit ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_PERF_MAXSENDS, (UINT)cPerf.iMaxSendsPerSec, FALSE );
                CheckDlgButton( hWnd, IDC_LIMITFPS, cVideo.bLimitFPS ? BST_CHECKED : BST_UNCHECKED );
                return TRUE;
            }
            case WM_COMMAND:
            {
                int iCode = HIWORD( wParam );
                if ( LOWORD( wParam ) == IDC_PERF_DEDUP && iCode == BN_CLICKED )
                    EnableWindow( GetDlgItem( hWnd, IDC_PERF_DEDUPASSUME ), IsDlgButtonChecked( hWnd, IDC_PERF_DEDUP ) == BST_CHECKED );
                if ( LOWORD( wParam ) == IDC_PERF_VIDEO )
                {
                    if ( iCode == BN_CLICKED ) DoVideoExport( GetParent( hWnd ) );
                    return TRUE;
                }
                if ( UserChange( wParam, lParam ) ) Changed( hWnd );
                break;
            }
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    PerformanceSettings cPerf = config.GetPerformanceSettings();
                    VideoSettings cVideo = config.GetVideoSettings();

                    int iThreads = 0, iPlayThreads = 0;
                    if ( !ReadThreads( hWnd, IDC_PERF_THREADS, L"a thread count for loading and drawing", iThreads ) ) return TRUE;
                    if ( !ReadThreads( hWnd, IDC_PERF_PLAYTHREADS, L"a thread count for playback decoding", iPlayThreads ) ) return TRUE;
                    BOOL bOk = FALSE;
                    UINT uLag = GetDlgItemInt( hWnd, IDC_PERF_MAXLAG, &bOk, FALSE );
                    if ( !bOk || uLag < 1 || uLag > 10000 )
                    {
                        MessageBoxW( hWnd, L"Please specify a skipping lag between 1 and 10000 ms", L"Error", MB_OK | MB_ICONEXCLAMATION );
                        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_PERF_MAXLAG ), TRUE );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }
                    UINT uMinVel = GetDlgItemInt( hWnd, IDC_PERF_MINVEL, &bOk, FALSE );
                    if ( !bOk || uMinVel < 1 || uMinVel > 128 )
                    {
                        MessageBoxW( hWnd, L"Please specify a velocity between 1 and 128 to skip notes below", L"Error", MB_OK | MB_ICONEXCLAMATION );
                        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_PERF_MINVEL ), TRUE );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }
                    UINT uSends = GetDlgItemInt( hWnd, IDC_PERF_MAXSENDS, &bOk, FALSE );
                    if ( !bOk || uSends < 1000 || uSends > 1000000000 )
                    {
                        MessageBoxW( hWnd, L"Please specify a sending limit between 1,000 and 1,000,000,000 messages per second", L"Error", MB_OK | MB_ICONEXCLAMATION );
                        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_PERF_MAXSENDS ), TRUE );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }
                    UINT uCache = GetDlgItemInt( hWnd, IDC_PERF_CACHEMB, &bOk, FALSE );
                    if ( !bOk || uCache < 16 || uCache > 65536 )
                    {
                        MessageBoxW( hWnd, L"Please specify a block cache between 16 and 65536 MB", L"Error", MB_OK | MB_ICONEXCLAMATION );
                        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_PERF_CACHEMB ), TRUE );
                        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
                        return TRUE;
                    }

                    cPerf.iWorkerThreads = iThreads;
                    cPerf.iPlaybackThreads = iPlayThreads;
                    cPerf.bVelocitySkip = IsDlgButtonChecked( hWnd, IDC_PERF_SKIPPING ) == BST_CHECKED;
                    cPerf.iSkipLagMs = (int)uLag;
                    cPerf.bCollapseStacks = IsDlgButtonChecked( hWnd, IDC_PERF_COLLAPSE ) == BST_CHECKED;
                    cPerf.bVelocityFloor = IsDlgButtonChecked( hWnd, IDC_PERF_VELFLOOR ) == BST_CHECKED;
                    cPerf.iMinVelocity = (int)uMinVel;
                    cPerf.bIgnoreWeird = IsDlgButtonChecked( hWnd, IDC_PERF_IGNOREWEIRD ) == BST_CHECKED;
                    cPerf.bEndOpenNotes = IsDlgButtonChecked( hWnd, IDC_PERF_ENDOPEN ) == BST_CHECKED;
                    cPerf.bSendLimit = IsDlgButtonChecked( hWnd, IDC_PERF_SENDLIMIT ) == BST_CHECKED;
                    cPerf.iMaxSendsPerSec = (int)uSends;
                    cPerf.bShowLoadStats = IsDlgButtonChecked( hWnd, IDC_PERF_LOADSTATS ) == BST_CHECKED;
                    cPerf.bCompactVisual = IsDlgButtonChecked( hWnd, IDC_PERF_COMPACT ) == BST_CHECKED;
                    cPerf.bLodCache = IsDlgButtonChecked( hWnd, IDC_PERF_LOD ) == BST_CHECKED;
                    cPerf.bGpuInstancing = IsDlgButtonChecked( hWnd, IDC_PERF_INSTANCING ) == BST_CHECKED;
                    cPerf.bDedup = IsDlgButtonChecked( hWnd, IDC_PERF_DEDUP ) == BST_CHECKED;
                    cPerf.bDedupAssume = IsDlgButtonChecked( hWnd, IDC_PERF_DEDUPASSUME ) == BST_CHECKED;
                    cPerf.iBlockCacheMB = (int)uCache;
                    cVideo.bLimitFPS = IsDlgButtonChecked( hWnd, IDC_LIMITFPS ) == BST_CHECKED;

                    config.SetPerformanceSettings( cPerf );
                    config.SetVideoSettings( cVideo );
                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_NOERROR );
                    return TRUE;
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

    const int MaxPages = 8;
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
        if ( msg == WM_NCDESTROY ) RemovePropW( hWnd, L"PFAviz.Page" );
        if ( PageScroll( hWnd, msg, wParam, lParam, (int)i ) ) return TRUE;
        // An up-down's arrows change its field's number (UserChange)
        if ( ( msg == WM_VSCROLL || msg == WM_HSCROLL ) && lParam && LOWORD( wParam ) == SB_THUMBPOSITION )
        {
            wchar_t sClass[32];
            if ( GetClassNameW( ( HWND )lParam, sClass, 32 ) && _wcsicmp( sClass, UPDOWN_CLASSW ) == 0 ) Changed( hWnd );
        }
        INT_PTR r = g_aPageProcs[i]( hWnd, msg, wParam, lParam );
        if ( msg == WM_INITDIALOG && g_aPageBars[i] )
        {
            SetWindowLongW( hWnd, GWL_STYLE, GetWindowLongW( hWnd, GWL_STYLE ) | (LONG)g_aPageBars[i] );
            SetWindowPos( hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
            PageScrollRanges( hWnd, (int)i );
        }
        return r;
    }
}

VOID DoPreferences( HWND hWndOwner )
{
    const int aDialogs[] = { IDD_PP1_VISUAL, IDD_PP6_EFFECTS, IDD_PP2_AUDIO, IDD_PP4_CONTROLS, IDD_PP5_PERFORMANCE };
    const DLGPROC aProcs[] = { VisualProc, EffectsProc, AudioProc, ControlsProc, PerformanceProc };
    const wchar_t *const aTitles[] = { L"Visual", L"Effects", L"Audio", L"Controls", L"Performance" };
    const int iPages = 5;
    static_assert( iPages <= MaxPages, "MaxPages" );
    PROPSHEETPAGEW psp[iPages] = {};
    std::vector< uint8_t > vTemplates[iPages];
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
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP | PSH_USECALLBACK;
    // The sheet's frame is the property sheet's own dialog, which DarkMode does not see made
    psh.pfnCallback = []( HWND hWnd, UINT msg, LPARAM ) -> int { if ( msg == PSCB_INITIALIZED ) DarkMode::Dialog( hWnd ); return 0; };
    psh.hwndParent = hWndOwner;
    psh.hInstance = g_hInstance;
    psh.pszCaption = L"Preferences";
    psh.nPages = iPages;
    psh.ppsp = psp;
    PropertySheetW( &psh );
}
