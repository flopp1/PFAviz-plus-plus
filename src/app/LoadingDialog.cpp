#include "app/LoadingDialog.h"

#include <commctrl.h>
#include <cstdio>
#include <memory>

#include "app/MainWindow.h"
#include "core/Config.h"
#include "core/Util.h"
#include "game/GameState.h"
#include "resource.h"
#include "song/Loader.h"

namespace
{
    struct LoadJob
    {
        std::wstring sFile;
        LoadProgress progress;
        LoadStats stats;
        std::unique_ptr< Song > song;
        std::wstring sError;
        long long llStart = 0;
        long long allStageStart[LoadProgress::StageCount] = {};
        HANDLE hThread = nullptr;
        bool bFinished = false;
    };

    LoadJob *g_pJob = nullptr;
    HWND g_hWndDlg = nullptr;
    HFONT g_hFixedFont = nullptr;

    DWORD WINAPI LoadThread( LPVOID p )
    {
        LoadJob *pJob = static_cast< LoadJob * >( p );
        const PerformanceSettings &cPerf = Config::GetConfig().GetPerformanceSettings();
        pJob->progress.bLzVisual = cPerf.bCompactVisual;
        pJob->progress.bNoteStats = ( Config::GetConfig().GetVisualSettings().uStats & VisualSettings::NoteStats ) != 0;
        pJob->progress.bFirstPairingOnly = cPerf.bIgnoreWeird;
        SetLzCacheBytes( (size_t)cPerf.iBlockCacheMB << 20 );
        pJob->song = Loader::LoadFile( pJob->sFile, cPerf.iWorkerThreads, pJob->progress, pJob->stats, pJob->sError );
        if ( pJob->song && !pJob->progress.bCancel )
        {
            if ( pJob->song->visual.llNotes == 0 )
                pJob->sError = L"The file has no notes.";
        }
        PostMessageW( g_hWnd, WM_APP_LOADDONE, 0, ( LPARAM )pJob );
        return 0;
    }

    void SetText( int iId, const wchar_t *s )
    {
        wchar_t buf[512];
        GetDlgItemTextW( g_hWndDlg, iId, buf, 512 );
        if ( wcscmp( buf, s ) != 0 ) SetDlgItemTextW( g_hWndDlg, iId, s );
    }

    void UpdateProgress()
    {
        if ( !g_pJob || !g_hWndDlg ) return;
        LoadProgress &p = g_pJob->progress;
        int iStage = p.iStage.load();
        long long llNow = Timer::NowMicroSecs();
        static const double aWeight[LoadProgress::StageCount] = { 0.0, 0.30, 0.10, 0.25, 0.30, 0.05 };
        double dOverall = 0.0;
        for ( int i = 0; i < LoadProgress::StageCount; i++ )
        {
            if ( iStage == i && !g_pJob->allStageStart[i] ) g_pJob->allStageStart[i] = llNow;
            uint64_t llTotal = p.aTotal[i], llDone = p.aDone[i];
            double dFrac = iStage > i ? 1.0 : ( llTotal ? (double)llDone / llTotal : 0.0 );
            dOverall += aWeight[i] * dFrac;
            SendDlgItemMessageW( g_hWndDlg, IDC_LOAD_BAR0 + i, PBM_SETPOS, (WPARAM)( dFrac * 1000 ), 0 );

            wchar_t sTime[64] = L"";
            if ( iStage > i && g_pJob->allStageStart[i] ) swprintf( sTime, 64, L"done" );
            else if ( iStage == i && g_pJob->allStageStart[i] ) swprintf( sTime, 64, L"%.1f s", ( llNow - g_pJob->allStageStart[i] ) / 1e6 );
            SetText( IDC_LOAD_TIME0 + i, sTime );
        }
        SendDlgItemMessageW( g_hWndDlg, IDC_LOAD_OVERALL, PBM_SETPOS, (WPARAM)( dOverall * 1000 ), 0 );

        double dSecs = ( llNow - g_pJob->llStart ) / 1e6;
        long long llCountEnd = p.llCountEndUs.load(); // the counting rate stops when counting does
        double dCountSecs = ( ( llCountEnd ? llCountEnd : llNow ) - g_pJob->llStart ) / 1e6;
        wchar_t sTracks[32], sFound[32], sEvents[32], sNotes[32], sCounters[512];
        Util::CommaPrintf( sTracks, (long long)p.llTracksDone.load() );
        Util::CommaPrintf( sFound, (long long)p.llTracksFound.load() );
        Util::CommaPrintf( sEvents, (long long)p.llEvents.load() );
        Util::CommaPrintf( sNotes, (long long)p.llNotes.load() );
        uint64_t llBytes = p.aDone[LoadProgress::Counting].load();
        swprintf( sCounters, 512, L"Elapsed: %.1f s\nTracks counted: %ls of %ls found\nEvents: %ls    Notes: %ls\nCounted: %.1f of %.1f MB (%.0f MB/s)\nMemory: %.0f MB (peak %.0f MB)%ls",
                  dSecs, sTracks, sFound, sEvents, sNotes,
                  llBytes / 1048576.0, p.llFileSize.load() / 1048576.0, dCountSecs > 0 ? llBytes / 1048576.0 / dCountSecs : 0.0,
                  p.llPrivateNow.load() / 1048576.0, p.llPrivatePeak.load() / 1048576.0,
                  p.bWaitingOnDisk ? L"\nWaiting on the disk (I/O bound)" : L"" );
        SetText( IDC_LOAD_COUNTERS, sCounters );
    }

    void ShowStats()
    {
        std::wstring s = g_pJob->stats.Format();
        if ( !g_pJob->sError.empty() ) s = L"Load failed: " + g_pJob->sError + L"\r\n\r\n" + s;
        for ( int i = 0; i < LoadProgress::StageCount; i++ )
        {
            ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_STAGE0 + i ), SW_HIDE );
            ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_BAR0 + i ), SW_HIDE );
            ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_TIME0 + i ), SW_HIDE );
        }
        ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_OVERALL ), SW_HIDE );
        ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_OVERALLLABEL ), SW_HIDE );
        ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_COUNTERS ), SW_HIDE );
        SetDlgItemTextW( g_hWndDlg, IDC_LOAD_STATS, s.c_str() );
        ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_STATS ), SW_SHOW );
        ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_COPY ), SW_SHOW );
        SetDlgItemTextW( g_hWndDlg, IDCANCEL, L"Close" );
        SetWindowTextW( g_hWndDlg, L"Loading statistics - PFAviz++" );
    }

    void CopyStats()
    {
        int n = GetWindowTextLengthW( GetDlgItem( g_hWndDlg, IDC_LOAD_STATS ) );
        std::wstring s( n + 1, L'\0' );
        GetDlgItemTextW( g_hWndDlg, IDC_LOAD_STATS, s.data(), n + 1 );
        if ( !OpenClipboard( g_hWndDlg ) ) return;
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc( GMEM_MOVEABLE, ( n + 1 ) * sizeof( wchar_t ) );
        if ( h )
        {
            memcpy( GlobalLock( h ), s.c_str(), ( n + 1 ) * sizeof( wchar_t ) );
            GlobalUnlock( h );
            SetClipboardData( CF_UNICODETEXT, h );
        }
        CloseClipboard();
    }

    INT_PTR WINAPI LoadingProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                g_hWndDlg = hWnd;
                SetDlgItemTextW( hWnd, IDC_LOAD_FILE, g_pJob->sFile.c_str() );
                for ( int i = 0; i < LoadProgress::StageCount; i++ )
                {
                    SetDlgItemTextW( hWnd, IDC_LOAD_STAGE0 + i, LoadProgress::StageNames[i] );
                    SendDlgItemMessageW( hWnd, IDC_LOAD_BAR0 + i, PBM_SETRANGE32, 0, 1000 );
                }
                SendDlgItemMessageW( hWnd, IDC_LOAD_OVERALL, PBM_SETRANGE32, 0, 1000 );
                if ( !g_hFixedFont )
                    g_hFixedFont = CreateFontW( -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                                CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas" );
                SendDlgItemMessageW( hWnd, IDC_LOAD_STATS, WM_SETFONT, ( WPARAM )g_hFixedFont, FALSE );

                RECT rcDlg, rcMain;
                GetWindowRect( hWnd, &rcDlg );
                GetWindowRect( g_hWnd, &rcMain );
                SetWindowPos( hWnd, nullptr, rcMain.left + ( rcMain.right - rcMain.left - ( rcDlg.right - rcDlg.left ) ) / 2,
                              rcMain.top + ( rcMain.bottom - rcMain.top - ( rcDlg.bottom - rcDlg.top ) ) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER );
                SetTimer( hWnd, 1, 100, nullptr );
                UpdateProgress();
                return TRUE;
            }
            case WM_TIMER:
                UpdateProgress();
                return TRUE;
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDC_LOAD_COPY:
                        CopyStats();
                        return TRUE;
                    case IDCANCEL:
                        if ( g_pJob && !g_pJob->bFinished )
                        {
                            g_pJob->progress.bCancel = true;
                            SetDlgItemTextW( hWnd, IDCANCEL, L"Cancelling..." );
                            EnableWindow( GetDlgItem( hWnd, IDCANCEL ), FALSE );
                        }
                        else
                            DestroyWindow( hWnd );
                        return TRUE;
                }
                break;
            case WM_DESTROY:
                KillTimer( hWnd, 1 );
                g_hWndDlg = nullptr;
                if ( g_pJob && g_pJob->bFinished ) { delete g_pJob; g_pJob = nullptr; }
                return TRUE;
        }
        return FALSE;
    }
}

bool IsLoading() { return g_pJob && !g_pJob->bFinished; }
HWND GetLoadingDialog() { return g_hWndDlg; }

bool StartLoad( const std::wstring &sFileIn )
{
    if ( IsLoading() ) return false;
    if ( g_hWndDlg ) DestroyWindow( g_hWndDlg ); // a previous statistics window

    wchar_t sFull[MAX_PATH * 4];
    DWORD dwLen = GetFullPathNameW( sFileIn.c_str(), MAX_PATH * 4, sFull, nullptr );
    g_pJob = new LoadJob();
    g_pJob->sFile = ( dwLen > 0 && dwLen < MAX_PATH * 4 ) ? std::wstring( sFull ) : sFileIn;
    g_pJob->llStart = Timer::NowMicroSecs();

    CreateDialogParamW( g_hInstance, MAKEINTRESOURCEW( IDD_LOADING ), g_hWnd, LoadingProc, 0 );
    if ( g_hWndDlg ) ShowWindow( g_hWndDlg, SW_SHOW );
    g_pJob->hThread = CreateThread( nullptr, 0, LoadThread, g_pJob, 0, nullptr );
    if ( !g_pJob->hThread )
    {
        if ( g_hWndDlg ) DestroyWindow( g_hWndDlg );
        delete g_pJob;
        g_pJob = nullptr;
        return false;
    }
    return true;
}

static std::wstring g_sOpenedFile;
const std::wstring &GetOpenedFile() { return g_sOpenedFile; }

void FinishLoad( LPARAM lParam )
{
    LoadJob *pJob = reinterpret_cast< LoadJob * >( lParam );
    WaitForSingleObject( pJob->hThread, INFINITE );
    CloseHandle( pJob->hThread );
    pJob->bFinished = true;

    const PerformanceSettings &cPerf = Config::GetConfig().GetPerformanceSettings();
    bool bCancelled = pJob->progress.bCancel;
    bool bOk = !bCancelled && pJob->song && pJob->sError.empty();

    if ( bOk )
    {
        PlaybackSettings &cPlayback = Config::GetConfig().GetPlaybackSettings();
        ViewSettings &cView = Config::GetConfig().GetViewSettings();
        std::shared_ptr< const Song > pSong( pJob->song.release() );
        MainScreen *pGameState = new MainScreen( pSong, nullptr, nullptr );
        const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
        pGameState->SetChannelColors( cVisual.colors, cVisual.iColorCount );
        if ( !cPlayback.GetPlayable() ) cPlayback.SetPlayable( true, true );
        if ( cPlayback.GetPlayMode() != PlayMode::Play ) cPlayback.SetPlayMode( PlayMode::Play, true );
        cPlayback.SetPaused( Config::GetConfig().GetControlsSettings().bStartPaused, true );
        cPlayback.SetPosition( 0 );
        cView.SetZoomMove( false, true );
        SetWindowTextW( g_hWnd, pJob->sFile.c_str() + ( pJob->sFile.find_last_of( L'\\' ) + 1 ) );
        g_sOpenedFile = pJob->sFile;
        HandOffMsg( WM_COMMAND, ID_CHANGESTATE, ( LPARAM )pGameState );
        if ( const wchar_t *sRender = _wgetenv( L"PFAVIZ_RENDER" ) ) PostMessageW( g_hWnd, WM_APP_RENDERVIDEO, 0, ( LPARAM )sRender );
    }
    pJob->song.reset();

    bool bKeepOpen = !bCancelled && ( cPerf.bShowLoadStats || !bOk );
    if ( g_hWndDlg && bKeepOpen )
        ShowStats();
    else if ( g_hWndDlg )
        DestroyWindow( g_hWndDlg ); // also frees the job
    if ( !g_hWndDlg && g_pJob == pJob ) { delete pJob; g_pJob = nullptr; }
}
