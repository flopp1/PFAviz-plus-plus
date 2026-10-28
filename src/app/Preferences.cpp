// Port of the original ConfigProcs.cpp property pages. Visual gains "Show all 128 keys" (the
// default) and full 0-127 key ranges; "Set Using Piano" is gone. Audio replaces the MIDI input list
// with the KDMAPI status. Controls keeps only its General group. Performance is new.
#include "app/Preferences.h"

#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <thread>

#include "app/MainWindow.h"
#include "app/VideoExport.h"
#include "audio/MidiOutput.h"
#include "core/Config.h"
#include "midi/Notes.h"
#include "resource.h"

namespace
{
    VOID Changed( HWND hWnd )
    {
        SendMessageW( GetParent( hWnd ), PSM_CHANGED, ( WPARAM )hWnd, 0 );
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

    // The track colors being edited (the count is its edit box): a grid of every colour the palette can
    // hold, PaletteCols to a row, the used ones filled
    unsigned g_aPalette[VisualSettings::MaxColors];
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
    VOID SetVisualProc( HWND hWnd, const VisualSettings &cVisual )
    {
        g_Stats.uStats = cVisual.uStats;
        std::copy( cVisual.aStatOrder, cVisual.aStatOrder + VisualSettings::StatCount, g_Stats.aOrder );
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
        CheckDlgButton( hWnd, IDC_BKGON, cVisual.bBkgImage ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_HIDELINES, cVisual.bHideLines ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_LOOPCOLORS, cVisual.bLoopColors ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_COLORBYKEY, cVisual.bColorByKey ? BST_CHECKED : BST_UNCHECKED );
        EnableWindow( GetDlgItem( hWnd, IDC_LOOPCOLORS ), !cVisual.bColorByKey );
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
        else swprintf( buf, 96, L"pixels (%d x %d to %ld x %ld)", MINWIDTH, MINHEIGHT, szMax.cx, szMax.cy );
        SetDlgItemTextW( hWnd, IDC_WINSIZEMAX, buf );
        EnableWindow( GetDlgItem( hWnd, IDC_WINWIDTH ), !bFullScreen );
        EnableWindow( GetDlgItem( hWnd, IDC_WINHEIGHT ), !bFullScreen );
    }

    // Reads a window size edit within [iMin, iMax]; otherwise says so and keeps the sheet open
    bool ReadSize( HWND hWnd, int iId, int iMin, int iMax, const wchar_t *sWhat, int &iValue )
    {
        BOOL bOk = FALSE;
        UINT u = GetDlgItemInt( hWnd, iId, &bOk, FALSE );
        if ( bOk && (int)u >= iMin && (int)u <= iMax ) { iValue = (int)u; return true; }
        wchar_t buf[128];
        swprintf( buf, 128, L"Please specify a window %ls between %d and %d pixels", sWhat, iMin, iMax );
        MessageBoxW( hWnd, buf, L"Error", MB_OK | MB_ICONEXCLAMATION );
        PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, iId ), TRUE );
        SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_INVALID );
        return false;
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
                SetVisualProc( hWnd, Config::GetConfig().GetVisualSettings() );
                SetWindowSizeEdits( hWnd );
                return TRUE;
            // Draws the coloured buttons
            case WM_DRAWITEM:
            {
                LPDRAWITEMSTRUCT pdis = ( LPDRAWITEMSTRUCT )lParam;
                if ( pdis->CtlID == IDC_PALETTE )
                {
                    // Each colour in use filled, the rest outlined; a 1-pixel gap between cells
                    FillRect( pdis->hDC, &pdis->rcItem, GetSysColorBrush( COLOR_BTNFACE ) );
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
                        else FrameRect( pdis->hDC, &rc, GetSysColorBrush( COLOR_BTNSHADOW ) );
                    }
                    if ( pdis->itemState & ODS_FOCUS ) DrawFocusRect( pdis->hDC, &pdis->rcItem );
                    return TRUE;
                }
                if ( pdis->CtlID != IDC_BKGCOLOR ) return FALSE;
                SetDCBrushColor( pdis->hDC, ( COLORREF )GetWindowLongPtrW( pdis->hwndItem, GWLP_USERDATA ) );
                FillRect( pdis->hDC, &pdis->rcItem, ( HBRUSH )GetStockObject( DC_BRUSH ) );
                return TRUE;
            }
            case WM_COMMAND:
            {
                int iId = LOWORD( wParam );
                Changed( hWnd );
                switch ( iId )
                {
                    case IDC_SHOWCUSTOMKEYS:
                        EnableCustomKeys( hWnd, TRUE );
                        return TRUE;
                    case IDC_SHOWALLKEYS: case IDC_SHOWSONGKEYS: case IDC_SHOWALL128KEYS: case IDC_SHOWALL256KEYS:
                        EnableCustomKeys( hWnd, FALSE );
                        return TRUE;
                    case IDC_STATSBOX:
                    {
                        StatsChoice c = g_Stats;
                        if ( DialogBoxParamW( GetModuleHandleW( nullptr ), MAKEINTRESOURCEW( IDD_STATS ), GetParent( hWnd ), StatsProc,
                                              ( LPARAM )&c ) == IDOK )
                        {
                            g_Stats = c;
                            ShowStatsCount( hWnd );
                        }
                        return TRUE;
                    }
                    case IDC_COLORBYKEY:     // (looping is for colouring by channel)
                        EnableWindow( GetDlgItem( hWnd, IDC_LOOPCOLORS ), IsDlgButtonChecked( hWnd, IDC_COLORBYKEY ) != BST_CHECKED );
                        return TRUE;
                    case IDC_COLORCOUNT:
                        if ( HIWORD( wParam ) == EN_CHANGE ) InvalidateRect( GetDlgItem( hWnd, IDC_PALETTE ), nullptr, FALSE );
                        return TRUE;
                    // A colour: the chooser. In the palette, the cell clicked (with the keyboard, the first); one
                    // past the colours in use adds it, and those before it
                    case IDC_PALETTE:
                    case IDC_BKGCOLOR:
                    {
                        static COLORREF acrCustClr[16] = { 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF,
                                                           0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF };
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
                        CHOOSECOLORW cc = {};
                        cc.lStructSize = sizeof( cc );
                        cc.hwndOwner = hWnd;
                        cc.lpCustColors = acrCustClr;
                        cc.rgbResult = iCell >= 0 ? g_aPalette[iCell] : ( COLORREF )GetWindowLongPtrW( hWndBtn, GWLP_USERDATA );
                        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                        if ( !ChooseColorW( &cc ) ) return TRUE;
                        if ( iCell >= 0 )
                        {
                            g_aPalette[iCell] = cc.rgbResult;
                            if ( iCell >= PaletteCount( hWnd ) ) SendDlgItemMessageW( hWnd, IDC_COLORCOUNTSPIN, UDM_SETPOS32, 0, iCell + 1 );
                        }
                        else SetWindowLongPtrW( hWndBtn, GWLP_USERDATA, cc.rgbResult );
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
                        if ( GetOpenFileNameW( &ofn ) ) SetDlgItemTextW( hWnd, IDC_BKGIMAGE, sFile );
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
                        SIZE szMax = MainWindowMaxSize();
                        if ( !ReadSize( hWnd, IDC_WINWIDTH, MINWIDTH, (int)szMax.cx, L"width", iWidth ) ) return TRUE;
                        if ( !ReadSize( hWnd, IDC_WINHEIGHT, MINHEIGHT, (int)szMax.cy, L"height", iHeight ) ) return TRUE;
                    }

                    cVisual.eKeysShown = VisualSettings::All128;
                    for ( int i = 0; i < 5; i++ )
                        if ( IsDlgButtonChecked( hWnd, aKeyModeIds[i] ) == BST_CHECKED )
                            cVisual.eKeysShown = static_cast< VisualSettings::KeysShown >( i );
                    cVisual.bAlwaysShowControls = IsDlgButtonChecked( hWnd, IDC_SHOWCONTROLS ) == BST_CHECKED;
                    cVisual.uStats = g_Stats.uStats;
                    std::copy( g_Stats.aOrder, g_Stats.aOrder + VisualSettings::StatCount, cVisual.aStatOrder );
                    cVisual.iFirstKey = (int)SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_GETCURSEL, 0, 0 );
                    cVisual.iLastKey = (int)SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_GETCURSEL, 0, 0 );
                    std::copy( g_aPalette, g_aPalette + VisualSettings::MaxColors, cVisual.colors );
                    cVisual.iColorCount = PaletteCount( hWnd );
                    cVisual.iBkgColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_BKGCOLOR ), GWLP_USERDATA );
                    cVisual.bBkgImage = bBkgOn;
                    cVisual.sBkgImage = sImage;
                    cVisual.iBkgOpacity = std::min( 100, (int)uOpacity );
                    cVisual.bHideLines = IsDlgButtonChecked( hWnd, IDC_HIDELINES ) == BST_CHECKED;
                    cVisual.bLoopColors = IsDlgButtonChecked( hWnd, IDC_LOOPCOLORS ) == BST_CHECKED;
                    cVisual.bColorByKey = IsDlgButtonChecked( hWnd, IDC_COLORBYKEY ) == BST_CHECKED;
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
                SetAudioProc( hWnd );
                return TRUE;
            case WM_COMMAND:
                if ( HIWORD( wParam ) == LBN_SELCHANGE ) Changed( hWnd );
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
                    if ( bChanged ) HandOffMsg( WM_DEVICECHANGE, 0, 0 );
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
                if ( HIWORD( wParam ) == EN_CHANGE || HIWORD( wParam ) == BN_CLICKED ) Changed( hWnd );
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
                SetDlgItemInt( hWnd, IDC_PERF_CACHEMB, (UINT)cPerf.iBlockCacheMB, FALSE );
                CheckDlgButton( hWnd, IDC_PERF_TUNING31, cPerf.bTuning31 ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_TUNING31BEND, cPerf.bTuning31PitchBend ? BST_CHECKED : BST_UNCHECKED );
                EnableWindow( GetDlgItem( hWnd, IDC_PERF_TUNING31BEND ), cPerf.bTuning31 );
                CheckDlgButton( hWnd, IDC_PERF_KDMAPI, cPerf.bPreferKDMAPI ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_LONGSENDS, cPerf.bKdmapiLongSends ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_COLLAPSE, cPerf.bCollapseStacks ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_VELFLOOR, cPerf.bVelocityFloor ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_PERF_MINVEL, (UINT)cPerf.iMinVelocity, FALSE );
                CheckDlgButton( hWnd, IDC_PERF_IGNOREWEIRD, cPerf.bIgnoreWeird ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_SENDLIMIT, cPerf.bSendLimit ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_PERF_MAXSENDS, (UINT)cPerf.iMaxSendsPerSec, FALSE );
                CheckDlgButton( hWnd, IDC_LIMITFPS, cVideo.bLimitFPS ? BST_CHECKED : BST_UNCHECKED );
                EnableWindow( GetDlgItem( hWnd, IDC_PERF_KDMAPI ), MidiOutputs::KdmapiAvailable() || cPerf.bPreferKDMAPI );
                return TRUE;
            }
            case WM_COMMAND:
            {
                int iCode = HIWORD( wParam );
                if ( LOWORD( wParam ) == IDC_PERF_TUNING31 && iCode == BN_CLICKED )
                    EnableWindow( GetDlgItem( hWnd, IDC_PERF_TUNING31BEND ), IsDlgButtonChecked( hWnd, IDC_PERF_TUNING31 ) == BST_CHECKED );
                if ( LOWORD( wParam ) == IDC_PERF_VIDEO )
                {
                    if ( iCode == BN_CLICKED ) DoVideoExport( GetParent( hWnd ) );
                    return TRUE;
                }
                if ( iCode == BN_CLICKED || iCode == CBN_SELCHANGE || iCode == EN_CHANGE ) Changed( hWnd );
                break;
            }
            case WM_NOTIFY:
                if ( ( ( LPNMHDR )lParam )->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    PerformanceSettings cPerf = config.GetPerformanceSettings();
                    VideoSettings cVideo = config.GetVideoSettings();
                    bool bOldKdmapi = cPerf.bPreferKDMAPI;

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
                    cPerf.bSendLimit = IsDlgButtonChecked( hWnd, IDC_PERF_SENDLIMIT ) == BST_CHECKED;
                    cPerf.iMaxSendsPerSec = (int)uSends;
                    cPerf.bShowLoadStats = IsDlgButtonChecked( hWnd, IDC_PERF_LOADSTATS ) == BST_CHECKED;
                    cPerf.bCompactVisual = IsDlgButtonChecked( hWnd, IDC_PERF_COMPACT ) == BST_CHECKED;
                    cPerf.bLodCache = IsDlgButtonChecked( hWnd, IDC_PERF_LOD ) == BST_CHECKED;
                    cPerf.bGpuInstancing = IsDlgButtonChecked( hWnd, IDC_PERF_INSTANCING ) == BST_CHECKED;
                    cPerf.iBlockCacheMB = (int)uCache;
                    cPerf.bTuning31 = IsDlgButtonChecked( hWnd, IDC_PERF_TUNING31 ) == BST_CHECKED;
                    cPerf.bTuning31PitchBend = IsDlgButtonChecked( hWnd, IDC_PERF_TUNING31BEND ) == BST_CHECKED;
                    cPerf.bPreferKDMAPI = IsDlgButtonChecked( hWnd, IDC_PERF_KDMAPI ) == BST_CHECKED;
                    cPerf.bKdmapiLongSends = IsDlgButtonChecked( hWnd, IDC_PERF_LONGSENDS ) == BST_CHECKED;
                    cVideo.bLimitFPS = IsDlgButtonChecked( hWnd, IDC_LIMITFPS ) == BST_CHECKED;

                    config.SetPerformanceSettings( cPerf );
                    config.SetVideoSettings( cVideo );
                    if ( bOldKdmapi != cPerf.bPreferKDMAPI ) HandOffMsg( WM_DEVICECHANGE, 0, 0 );
                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, PSNRET_NOERROR );
                    return TRUE;
                }
                break;
        }
        return FALSE;
    }
}

VOID DoPreferences( HWND hWndOwner )
{
    const int aDialogs[] = { IDD_PP1_VISUAL, IDD_PP2_AUDIO, IDD_PP4_CONTROLS, IDD_PP5_PERFORMANCE };
    const DLGPROC aProcs[] = { VisualProc, AudioProc, ControlsProc, PerformanceProc };
    const wchar_t *const aTitles[] = { L"Visual", L"Audio", L"Controls", L"Performance" };
    PROPSHEETPAGEW psp[4] = {};
    for ( int i = 0; i < 4; i++ )
    {
        psp[i].dwSize = sizeof( PROPSHEETPAGEW );
        psp[i].dwFlags = PSP_USETITLE;
        psp[i].hInstance = g_hInstance;
        psp[i].pszTemplate = MAKEINTRESOURCEW( aDialogs[i] );
        psp[i].pfnDlgProc = aProcs[i];
        psp[i].pszTitle = aTitles[i];
    }
    PROPSHEETHEADERW psh = {};
    psh.dwSize = sizeof( PROPSHEETHEADERW );
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP;
    psh.hwndParent = hWndOwner;
    psh.hInstance = g_hInstance;
    psh.pszCaption = L"Preferences";
    psh.nPages = 4;
    psh.ppsp = psp;
    PropertySheetW( &psh );
}
