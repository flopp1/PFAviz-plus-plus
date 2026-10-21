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

    const int aKeyModeIds[4] = { IDC_SHOWALLKEYS, IDC_SHOWSONGKEYS, IDC_SHOWCUSTOMKEYS, IDC_SHOWALL128KEYS }; // by KeysShown

    VOID EnableCustomKeys( HWND hWnd, BOOL bEnable )
    {
        EnableWindow( GetDlgItem( hWnd, IDC_FIRSTKEY ), bEnable );
        EnableWindow( GetDlgItem( hWnd, IDC_THROUGH ), bEnable );
        EnableWindow( GetDlgItem( hWnd, IDC_LASTKEY ), bEnable );
    }

    // Sets the values in the visual settings page. Used at init and when restoring defaults
    VOID SetVisualProc( HWND hWnd, const VisualSettings &cVisual, bool bShowFPS )
    {
        CheckDlgButton( hWnd, IDC_SHOWTIME, cVisual.bShowTime ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_DISPLAYFPS, bShowFPS ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hWnd, IDC_SHOWSCORE, cVisual.bShowScore ? BST_CHECKED : BST_UNCHECKED );
        for ( int i = 0; i < 4; i++ )
            CheckDlgButton( hWnd, aKeyModeIds[i], i == cVisual.eKeysShown ? BST_CHECKED : BST_UNCHECKED );
        EnableCustomKeys( hWnd, cVisual.eKeysShown == VisualSettings::Custom );
        CheckDlgButton( hWnd, IDC_SHOWCONTROLS, cVisual.bAlwaysShowControls ? BST_CHECKED : BST_UNCHECKED );
        SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_SETCURSEL, cVisual.iFirstKey, 0 );
        SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_SETCURSEL, cVisual.iLastKey, 0 );
        for ( int i = 0; i < IDC_COLOR6 - IDC_COLOR1 + 1; i++ )
            SetWindowLongPtrW( GetDlgItem( hWnd, IDC_COLOR1 + i ), GWLP_USERDATA, cVisual.colors[i] );
        SetWindowLongPtrW( GetDlgItem( hWnd, IDC_BKGCOLOR ), GWLP_USERDATA, cVisual.iBkgColor );
    }

    INT_PTR CALLBACK VisualProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
                for ( int i = 0; i < 128; i++ )
                {
                    SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_ADDSTRING, 0, ( LPARAM )Notes::NoteName( i ) );
                    SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_ADDSTRING, 0, ( LPARAM )Notes::NoteName( i ) );
                }
                SetVisualProc( hWnd, Config::GetConfig().GetVisualSettings(), Config::GetConfig().GetVideoSettings().bShowFPS );
                return TRUE;
            // Draws the coloured buttons
            case WM_DRAWITEM:
            {
                LPDRAWITEMSTRUCT pdis = ( LPDRAWITEMSTRUCT )lParam;
                if ( ( pdis->CtlID < IDC_COLOR1 || pdis->CtlID > IDC_COLOR6 ) && pdis->CtlID != IDC_BKGCOLOR )
                    return FALSE;
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
                    case IDC_SHOWALLKEYS: case IDC_SHOWSONGKEYS: case IDC_SHOWALL128KEYS:
                        EnableCustomKeys( hWnd, FALSE );
                        return TRUE;
                    // Colour buttons: pop up the colour chooser
                    case IDC_COLOR1: case IDC_COLOR2: case IDC_COLOR3:
                    case IDC_COLOR4: case IDC_COLOR5: case IDC_COLOR6:
                    case IDC_BKGCOLOR:
                    {
                        static COLORREF acrCustClr[16] = { 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF,
                                                           0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF };
                        HWND hWndBtn = ( HWND )lParam;
                        CHOOSECOLORW cc = {};
                        cc.lStructSize = sizeof( cc );
                        cc.hwndOwner = hWnd;
                        cc.lpCustColors = acrCustClr;
                        cc.rgbResult = ( COLORREF )GetWindowLongPtrW( hWndBtn, GWLP_USERDATA );
                        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                        if ( !ChooseColorW( &cc ) ) return TRUE;
                        SetWindowLongPtrW( hWndBtn, GWLP_USERDATA, cc.rgbResult );
                        InvalidateRect( hWndBtn, nullptr, FALSE );
                        return TRUE;
                    }
                    case IDC_RESTOREDEFAULTS:
                    {
                        VisualSettings cVisual;
                        cVisual.LoadDefaultValues();
                        VideoSettings cVideo;
                        cVideo.LoadDefaultValues();
                        SendMessageW( hWnd, WM_SETREDRAW, FALSE, 0 );
                        SetVisualProc( hWnd, cVisual, cVideo.bShowFPS );
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

                    cVisual.eKeysShown = VisualSettings::All128;
                    for ( int i = 0; i < 4; i++ )
                        if ( IsDlgButtonChecked( hWnd, aKeyModeIds[i] ) == BST_CHECKED )
                            cVisual.eKeysShown = static_cast< VisualSettings::KeysShown >( i );
                    cVisual.bAlwaysShowControls = IsDlgButtonChecked( hWnd, IDC_SHOWCONTROLS ) == BST_CHECKED;
                    cVisual.bShowTime = IsDlgButtonChecked( hWnd, IDC_SHOWTIME ) == BST_CHECKED;
                    cVisual.bShowScore = IsDlgButtonChecked( hWnd, IDC_SHOWSCORE ) == BST_CHECKED;
                    VideoSettings cVideo = config.GetVideoSettings();
                    cVideo.bShowFPS = IsDlgButtonChecked( hWnd, IDC_DISPLAYFPS ) == BST_CHECKED;
                    config.SetVideoSettings( cVideo );
                    cVisual.iFirstKey = (int)SendDlgItemMessageW( hWnd, IDC_FIRSTKEY, CB_GETCURSEL, 0, 0 );
                    cVisual.iLastKey = (int)SendDlgItemMessageW( hWnd, IDC_LASTKEY, CB_GETCURSEL, 0, 0 );
                    for ( int i = 0; i < IDC_COLOR6 - IDC_COLOR1 + 1; i++ )
                        cVisual.colors[i] = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_COLOR1 + i ), GWLP_USERDATA );
                    cVisual.iBkgColor = ( unsigned )GetWindowLongPtrW( GetDlgItem( hWnd, IDC_BKGCOLOR ), GWLP_USERDATA );

                    config.SetVisualSettings( cVisual );
                    if ( cView.GetFullScreen() && bAlwaysShowControls != cVisual.bAlwaysShowControls )
                        cView.SetControls( cView.GetControls(), true );
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
                return TRUE;
            }
            case WM_COMMAND:
                if ( HIWORD( wParam ) == EN_CHANGE ) Changed( hWnd );
                break;
            case WM_NOTIFY:
            {
                LPNMHDR lpnmhdr = ( LPNMHDR )lParam;
                if ( lpnmhdr->code == UDN_DELTAPOS )
                {
                    LPNMUPDOWN lpnmud = ( LPNMUPDOWN )lParam;
                    if ( lpnmhdr->idFrom == IDC_LRARROWSSPIN ) StepEdit( GetDlgItem( hWnd, IDC_LRARROWS ), lpnmud->iDelta, 0.1 );
                    else if ( lpnmhdr->idFrom == IDC_UDARROWSSPIN ) StepEdit( GetDlgItem( hWnd, IDC_UDARROWS ), lpnmud->iDelta, 1.0 );
                    return TRUE;
                }
                if ( lpnmhdr->code == PSN_APPLY )
                {
                    Config &config = Config::GetConfig();
                    ControlsSettings cControls = config.GetControlsSettings();
                    if ( !ReadEdit( hWnd, IDC_LRARROWS, cControls.dFwdBackSecs, L"Please specify a numeric value for the left and right arrows" ) ) return TRUE;
                    if ( !ReadEdit( hWnd, IDC_UDARROWS, cControls.dSpeedUpPct, L"Please specify a numeric value for the up and down arrows" ) ) return TRUE;
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

    int MaxThreadChoice()
    {
        unsigned n = std::thread::hardware_concurrency();
        return (int)std::max( 64u, n ? n : 4 );
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

                HWND hWndThreads = GetDlgItem( hWnd, IDC_PERF_THREADS );
                wchar_t buf[64];
                unsigned n = std::thread::hardware_concurrency();
                swprintf( buf, 64, L"Auto (%u)", n ? n : 4 );
                SendMessageW( hWndThreads, CB_ADDSTRING, 0, ( LPARAM )buf );
                for ( int i = 1; i <= MaxThreadChoice(); i++ )
                {
                    swprintf( buf, 64, L"%d", i );
                    SendMessageW( hWndThreads, CB_ADDSTRING, 0, ( LPARAM )buf );
                }
                SendMessageW( hWndThreads, CB_SETCURSEL, std::min( cPerf.iWorkerThreads, MaxThreadChoice() ), 0 );

                CheckDlgButton( hWnd, IDC_PERF_SKIPPING, cPerf.bVelocitySkip ? BST_CHECKED : BST_UNCHECKED );
                SendDlgItemMessageW( hWnd, IDC_PERF_MAXLAGSPIN, UDM_SETRANGE32, 1, 10000 );
                SendDlgItemMessageW( hWnd, IDC_PERF_MAXLAGSPIN, UDM_SETPOS32, 0, cPerf.iSkipLagMs );

                CheckDlgButton( hWnd, IDC_PERF_LOADSTATS, cPerf.bShowLoadStats ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_COMPACT, cPerf.bCompactVisual ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_LOD, cPerf.bLodCache ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_PERF_CACHEMB, (UINT)cPerf.iBlockCacheMB, FALSE );
                CheckDlgButton( hWnd, IDC_PERF_KDMAPI, cPerf.bPreferKDMAPI ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_PLAYSTATS, cPerf.bShowPlaybackStats ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_COLLAPSE, cPerf.bCollapseStacks ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_VELFLOOR, cPerf.bVelocityFloor ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_PERF_MINVEL, (UINT)cPerf.iMinVelocity, FALSE );
                CheckDlgButton( hWnd, IDC_PERF_IGNOREWEIRD, cPerf.bIgnoreWeird ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_PERF_SENDLIMIT, cPerf.bSendLimit ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_PERF_MAXSENDS, (UINT)cPerf.iMaxSendsPerSec, FALSE );
                CheckDlgButton( hWnd, IDC_PERF_EXTRASTATS, cPerf.bExtraStats ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_LIMITFPS, cVideo.bLimitFPS ? BST_CHECKED : BST_UNCHECKED );
                EnableWindow( GetDlgItem( hWnd, IDC_PERF_KDMAPI ), MidiOutputs::KdmapiAvailable() || cPerf.bPreferKDMAPI );
                return TRUE;
            }
            case WM_COMMAND:
            {
                int iCode = HIWORD( wParam );
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

                    cPerf.iWorkerThreads = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_PERF_THREADS, CB_GETCURSEL, 0, 0 ) );
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
                    cPerf.iBlockCacheMB = (int)uCache;
                    cPerf.bPreferKDMAPI = IsDlgButtonChecked( hWnd, IDC_PERF_KDMAPI ) == BST_CHECKED;
                    cPerf.bShowPlaybackStats = IsDlgButtonChecked( hWnd, IDC_PERF_PLAYSTATS ) == BST_CHECKED;
                    cPerf.bExtraStats = IsDlgButtonChecked( hWnd, IDC_PERF_EXTRASTATS ) == BST_CHECKED;
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
