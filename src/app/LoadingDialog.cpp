#include "app/LoadingDialog.h"

#include <commctrl.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "app/LoadView.h"
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
        long long allStageEnd[LoadProgress::StageCount] = {};   // when the stage was first seen done
        HANDLE hThread = nullptr;
        bool bFinished = false;
        bool bRenderOnly = false;
        // Cancelled loads still stopping: this load starts once they have (their memory freed). Its own handles.
        std::vector< HANDLE > vWaitFor;
        std::atomic< bool > bWaiting{ false };
    };

    LoadJob *g_pJob = nullptr;
    // Cancelled loads let go of at once: their threads stop at the loader's next check of the cancel and
    // free what they loaded, then post WM_APP_LOADDONE as usual, and FinishLoad deletes them
    std::vector< LoadJob * > g_vStopping;
    HWND g_hWndDlg = nullptr;

    // Of several MIDI files in an archive, the one the user picks (the loader asks, the window's thread shows)
    struct ChooseMidi { const std::vector< Archive::Entry > *pEntries; int iChosen; };

    // A size in the largest unit it is at least 1 of
    std::wstring SizeText( int64_t llBytes )
    {
        static const wchar_t *const aUnit[] = { L"bytes", L"KB", L"MB", L"GB", L"TB" };
        double d = (double)llBytes;
        int i = 0;
        while ( d >= 1024.0 && i < 4 ) { d /= 1024.0; i++; }
        wchar_t s[32];
        if ( i == 0 ) swprintf( s, 32, L"%lld bytes", (long long)llBytes );
        else swprintf( s, 32, L"%.1f %ls", d, aUnit[i] );
        return s;
    }

    // The list fills the dialog; the buttons keep to its bottom right
    VOID LayOutChooser( HWND hWnd )
    {
        RECT rc, rcOk, rcList;
        GetClientRect( hWnd, &rc );
        HWND hList = GetDlgItem( hWnd, IDC_CHOOSEMIDI_LIST ), hOk = GetDlgItem( hWnd, IDOK ), hCancel = GetDlgItem( hWnd, IDCANCEL );
        GetWindowRect( hOk, &rcOk );
        GetWindowRect( hList, &rcList );
        MapWindowPoints( nullptr, hWnd, ( LPPOINT )&rcList, 2 );
        const int cxButton = rcOk.right - rcOk.left, cyButton = rcOk.bottom - rcOk.top, iMargin = rcList.left, iGap = iMargin / 2 + 1;
        const int yButtons = rc.bottom - iMargin - cyButton;
        SetWindowPos( hList, nullptr, iMargin, rcList.top, rc.right - iMargin * 2, yButtons - iMargin - rcList.top, SWP_NOZORDER );
        SetWindowPos( hCancel, nullptr, rc.right - iMargin - cxButton, yButtons, 0, 0, SWP_NOZORDER | SWP_NOSIZE );
        SetWindowPos( hOk, nullptr, rc.right - iMargin - cxButton * 2 - iGap, yButtons, 0, 0, SWP_NOZORDER | SWP_NOSIZE );
        InvalidateRect( hWnd, nullptr, TRUE );
    }

    INT_PTR CALLBACK ChooseMidiProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        static SIZE szMin;      // the dialog's own size: it is not made smaller
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, lParam );
                RECT rcWnd;
                GetWindowRect( hWnd, &rcWnd );
                szMin = { rcWnd.right - rcWnd.left, rcWnd.bottom - rcWnd.top };
                const ChooseMidi &c = *( const ChooseMidi * )lParam;
                HWND hList = GetDlgItem( hWnd, IDC_CHOOSEMIDI_LIST );
                // (as wide as the longest name scrolls sideways)
                HDC hdc = GetDC( hList );
                HGDIOBJ hOld = SelectObject( hdc, ( HFONT )SendMessageW( hList, WM_GETFONT, 0, 0 ) );
                int iExtent = 0;
                for ( const Archive::Entry &e : *c.pEntries )
                {
                    const std::wstring s = e.llSize >= 0 ? e.sName + L"   (" + SizeText( e.llSize ) + L")" : e.sName;
                    SendMessageW( hList, LB_ADDSTRING, 0, ( LPARAM )s.c_str() );
                    SIZE sz;
                    if ( GetTextExtentPoint32W( hdc, s.c_str(), (int)s.size(), &sz ) ) iExtent = std::max< int >( iExtent, sz.cx + 8 );
                }
                SelectObject( hdc, hOld );
                ReleaseDC( hList, hdc );
                SendMessageW( hList, LB_SETHORIZONTALEXTENT, iExtent, 0 );
                SendMessageW( hList, LB_SETCURSEL, 0, 0 );
                return TRUE;
            }
            case WM_SIZE:
                LayOutChooser( hWnd );
                return TRUE;
            case WM_GETMINMAXINFO:
                if ( szMin.cx ) { ( ( MINMAXINFO * )lParam )->ptMinTrackSize = { szMin.cx, szMin.cy }; return TRUE; }
                break;
            case WM_COMMAND:
            {
                ChooseMidi &c = *( ChooseMidi * )GetWindowLongPtrW( hWnd, GWLP_USERDATA );
                const bool bPick = LOWORD( wParam ) == IDOK || ( LOWORD( wParam ) == IDC_CHOOSEMIDI_LIST && HIWORD( wParam ) == LBN_DBLCLK );
                if ( bPick ) c.iChosen = (int)SendDlgItemMessageW( hWnd, IDC_CHOOSEMIDI_LIST, LB_GETCURSEL, 0, 0 );
                if ( bPick || LOWORD( wParam ) == IDCANCEL ) EndDialog( hWnd, 0 );
                return TRUE;
            }
        }
        return FALSE;
    }

    DWORD WINAPI LoadThread( LPVOID p )
    {
        LoadJob *pJob = static_cast< LoadJob * >( p );
        // A cancelled load still stopping: waited for (two loads at once would hold both songs' memory)
        if ( !pJob->vWaitFor.empty() )
        {
            pJob->bWaiting = true;
            while ( !pJob->progress.bCancel &&
                    WaitForMultipleObjects( (DWORD)pJob->vWaitFor.size(), pJob->vWaitFor.data(), TRUE, 50 ) == WAIT_TIMEOUT ) {}
            for ( HANDLE h : pJob->vWaitFor ) CloseHandle( h );
            pJob->vWaitFor.clear();
            pJob->bWaiting = false;
            if ( pJob->progress.bCancel )
            {
                PostMessageW( g_hWnd, WM_APP_LOADDONE, 0, ( LPARAM )pJob );
                return 0;
            }
        }
        pJob->progress.fnChoose = []( const std::vector< Archive::Entry > &v ) {
            ChooseMidi c = { &v, -1 };
            SendMessageW( g_hWnd, WM_APP_CHOOSEMIDI, 0, ( LPARAM )&c );
            return c.iChosen;
        };
        const PerformanceSettings &cPerf = Config::GetConfig().GetPerformanceSettings();
        pJob->progress.bLzVisual = cPerf.bCompactVisual;
        pJob->progress.bNoteStats = ( Config::GetConfig().GetVisualSettings().uStats & VisualSettings::NoteStats ) != 0 ||
                                    Config::GetConfig().GetVisualSettings().bGraph || Config::GetConfig().GetVisualSettings().bPfaLag;
        pJob->progress.bFirstPairingOnly = cPerf.bIgnoreWeird;
        pJob->progress.bEndOpenNotes = cPerf.bEndOpenNotes;
        pJob->progress.bDedup = cPerf.bDedup;
        pJob->progress.bDedupAssume = cPerf.bDedupAssume;
        SetLzCacheBytes( (size_t)cPerf.iBlockCacheMB << 20 );
        pJob->song = Loader::LoadFile( pJob->sFile, cPerf.iWorkerThreads, pJob->progress, pJob->stats, pJob->sError );
        if ( pJob->song && !pJob->progress.bCancel )
        {
            if ( pJob->song->visual.llNotes == 0 )
                pJob->sError = L"The file has no notes.";
        }
        // Cancelled: the song freed here, not on the window's thread (a large one takes a while)
        if ( pJob->progress.bCancel ) pJob->song.reset();
        PostMessageW( g_hWnd, WM_APP_LOADDONE, 0, ( LPARAM )pJob );
        return 0;
    }

    // Numbers, sizes and times as the window shows them
    std::wstring Commas( long long ll )
    {
        wchar_t s[32];
        Util::CommaPrintf( s, ll );
        return s;
    }
    std::wstring Compact( double d )     // 1,234,567 -> "1.23M" (the exact counts are in the sections)
    {
        static const wchar_t aUnit[] = { L'K', L'M', L'B', L'T' };
        if ( d < 1e5 ) return Commas( (long long)d );
        int i = 0;
        double v = d / 1e3;
        while ( v >= 999.5 && i < 3 ) { v /= 1000.0; i++; }
        wchar_t s[48];
        swprintf( s, 48, v >= 99.95 ? L"%.0f%lc" : v >= 9.995 ? L"%.1f%lc" : L"%.2f%lc", v, aUnit[i] );
        return s;
    }
    std::wstring MBText( double dBytes )
    {
        wchar_t s[32];
        double dMB = dBytes / 1048576.0;
        if ( dMB >= 10240.0 ) swprintf( s, 32, L"%.1f GB", dMB / 1024.0 );
        else if ( dMB >= 1000.0 ) swprintf( s, 32, L"%.2f GB", dMB / 1024.0 );
        else if ( dMB >= 100.0 ) swprintf( s, 32, L"%.0f MB", dMB );
        else swprintf( s, 32, L"%.1f MB", dMB );
        return s;
    }
    std::wstring Fmt( const wchar_t *sFormat, ... )
    {
        wchar_t s[256];
        va_list va;
        va_start( va, sFormat );
        vswprintf( s, 256, sFormat, va );
        va_end( va );
        return s;
    }

    // The heading: the file's name, and its folder
    void Heading( LoadView::Model &m, const std::wstring &sFile )
    {
        size_t i = sFile.find_last_of( L"\\/" );
        m.sTitle = i == std::wstring::npos ? sFile : sFile.substr( i + 1 );
        m.sSub = i == std::wstring::npos ? L"" : sFile.substr( 0, i );
    }

    LoadView::Row MakeRow( const std::wstring &sLabel, const std::wstring &sValue, int iIndent = 0 )
    {
        LoadView::Row r;
        r.sLabel = sLabel;
        r.sValue = sValue;
        r.iIndent = iIndent;
        return r;
    }

    HWND View() { return GetDlgItem( g_hWndDlg, IDC_LOAD_VIEW ); }

    void UpdateProgress()
    {
        if ( !g_pJob || !g_hWndDlg || g_pJob->bFinished ) return;
        LoadProgress &p = g_pJob->progress;
        int iStage = p.iStage.load();
        long long llNow = Timer::NowMicroSecs();
        static const double aWeight[LoadProgress::StageCount] = { 0.0, 0.30, 0.10, 0.25, 0.30, 0.05 };

        LoadView::Model m;
        Heading( m, g_pJob->sFile );
        if ( g_pJob->bWaiting ) m.sStatus = L"Waiting for the cancelled load to stop...";
        else if ( p.bWaitingOnDisk ) m.sStatus = L"Waiting on the disk (I/O bound)";
        else if ( iStage >= 0 && iStage < LoadProgress::StageCount ) m.sStatus = std::wstring( LoadProgress::StageNames[iStage] ) + L"...";

        LoadView::Section stages;
        stages.sTitle = L"PROGRESS";
        double dOverall = 0.0;
        for ( int i = 0; i < LoadProgress::StageCount; i++ )
        {
            if ( iStage == i && !g_pJob->allStageStart[i] ) g_pJob->allStageStart[i] = llNow;
            uint64_t llTotal = p.aTotal[i], llDone = p.aDone[i];
            double dFrac = iStage > i ? 1.0 : iStage == i && llTotal ? std::min( 1.0, (double)llDone / llTotal ) : 0.0;
            dOverall += aWeight[i] * dFrac;
            // A stage done: how long it took (one too quick to be seen running started when the one before ended)
            if ( iStage > i && !g_pJob->allStageEnd[i] )
            {
                g_pJob->allStageEnd[i] = llNow;
                if ( !g_pJob->allStageStart[i] ) g_pJob->allStageStart[i] = i > 0 && g_pJob->allStageEnd[i - 1] ? g_pJob->allStageEnd[i - 1] : g_pJob->llStart;
            }
            LoadView::Row r = MakeRow( LoadProgress::StageNames[i], L"" );
            r.dBar = dFrac;
            r.bActive = iStage == i;
            if ( iStage > i ) { r.sValue = Fmt( L"%.1f s", ( g_pJob->allStageEnd[i] - g_pJob->allStageStart[i] ) / 1e6 ); r.sNote = L"done"; }
            else if ( iStage == i && g_pJob->allStageStart[i] )
            {
                r.sValue = Fmt( L"%.1f s", ( llNow - g_pJob->allStageStart[i] ) / 1e6 );
                if ( llTotal ) r.sNote = Fmt( L"%.0f%%", dFrac * 100.0 );
            }
            stages.vRows.push_back( r );
        }
        LoadView::Row overall = MakeRow( L"Overall", Fmt( L"%.0f%%", dOverall * 100.0 ) );
        overall.dBar = dOverall;
        stages.vRows.push_back( overall );

        double dSecs = ( llNow - g_pJob->llStart ) / 1e6;
        long long llCountEnd = p.llCountEndUs.load(); // the counting rate stops when counting does
        double dCountSecs = ( ( llCountEnd ? llCountEnd : llNow ) - g_pJob->llStart ) / 1e6;
        uint64_t llBytes = p.aDone[LoadProgress::Counting].load();
        m.vTiles = { { Fmt( L"%.1f s", dSecs ), L"Elapsed" },
                     { Compact( (double)p.llNotes.load() ), L"Notes" },
                     { Compact( (double)p.llEvents.load() ), L"Events" },
                     { MBText( (double)p.llPrivateNow.load() ), L"Memory" } };
        LoadView::Section details;
        details.sTitle = L"COUNTING";
        details.vRows.push_back( MakeRow( L"Tracks counted", Commas( (long long)p.llTracksDone.load() ) + L" of " + Commas( (long long)p.llTracksFound.load() ) + L" found" ) );
        details.vRows.push_back( MakeRow( L"Read", MBText( (double)llBytes ) + L" of " + MBText( (double)p.llFileSize.load() ) ) );
        details.vRows.push_back( MakeRow( L"Speed", Fmt( L"%.0f MB/s", dCountSecs > 0 ? llBytes / 1048576.0 / dCountSecs : 0.0 ) ) );
        details.vRows.push_back( MakeRow( L"Peak memory", MBText( (double)p.llPrivatePeak.load() ) ) );
        m.vSections = { stages, details };
        LoadView::Set( View(), std::move( m ) );
    }

    // The statistics, laid out (the plain text of them is what Copy stats copies)
    LoadView::Model StatsModel( const LoadStats &st, const std::wstring &sFile, const std::wstring &sError )
    {
        LoadView::Model m;
        Heading( m, sFile );
        if ( !sError.empty() ) m.sAlert = L"Load failed: " + sError;
        const double dTotal = st.dTotalSeconds;

        // The file and how it was read
        LoadView::Section file;
        file.sTitle = L"FILE";
        file.vRows.push_back( MakeRow( L"Size", MBText( (double)st.llFileSize ) ) );
        if ( !st.sUnpacked.empty() ) file.vRows.push_back( MakeRow( L"Unpacked", st.sUnpacked ) );
        file.vRows.push_back( MakeRow( L"Storage", st.bSeekPenalty ? L"Spinning disk" : L"SSD or cached" ) );
        file.vRows.push_back( MakeRow( L"Read", st.bFusedRead ? L"One sequential read, fused parse" : st.bSequentialRead ? L"Sequential reader" : L"Mapped, read in parallel" ) );
        file.vRows.push_back( MakeRow( L"Threads", Commas( st.iThreads ) ) );
        if ( st.dReaderSeconds > 0 ) file.vRows.push_back( MakeRow( L"Disk read speed", Fmt( L"%.0f MB/s", st.llFileSize / 1048576.0 / st.dReaderSeconds ) ) );
        if ( st.bFusedRead ) file.vRows.push_back( MakeRow( L"Packed tracks", MBText( (double)st.llFusedBytes ) + L" (freed after the build)" ) );
        if ( st.llGroupBytes ) file.vRows.push_back( MakeRow( L"Group records", MBText( (double)st.llGroupBytes ) + L" (freed after the build)" ) );

        if ( dTotal <= 0.0 )   // (failed before it read anything)
        {
            m.vSections = { file };
            return m;
        }
        m.vTiles = { { dTotal >= 100.0 ? Fmt( L"%.0f s", dTotal ) : Fmt( L"%.2f s", dTotal ), L"Load time" },
                     { Compact( (double)st.llNotes ), L"Notes" },
                     { Fmt( L"%.0f MB/s", st.llFileSize / 1048576.0 / dTotal ), Compact( st.llEvents / dTotal ) + L" events/s" },
                     { MBText( (double)st.llPrivatePeak ), L"Peak memory" } };

        // Where the time went: each stage's share of the load, and its parts
        LoadView::Section time;
        time.sTitle = L"TIME BY STAGE";
        for ( int i = 0; i < LoadProgress::StageCount; i++ )
        {
            LoadView::Row r = MakeRow( LoadProgress::StageNames[i], Fmt( L"%.3f s", st.adStageSeconds[i] ) );
            r.dBar = st.adStageSeconds[i] / dTotal;
            if ( st.aStagePeak[i] ) r.sNote = L"peak " + MBText( (double)st.aStagePeak[i] );
            time.vRows.push_back( r );
            for ( const LoadStats::Step &s : st.vSteps )
                if ( s.iStage == i )
                {
                    LoadView::Row part = MakeRow( s.sName, Fmt( L"%.3f s", s.dSeconds ), 1 );
                    part.dBar = s.dSeconds / dTotal;
                    if ( s.llPrivate ) part.sNote = L"then " + MBText( (double)s.llPrivate );
                    time.vRows.push_back( part );
                }
        }

        // What it holds
        LoadView::Section song;
        song.sTitle = L"SONG";
        song.vRows.push_back( MakeRow( L"Tracks", Commas( (long long)st.llTracks ) ) );
        if ( st.llMalformedTracks ) song.vRows.push_back( MakeRow( L"Malformed", Commas( (long long)st.llMalformedTracks ), 1 ) );
        if ( st.llKeptTracks ) song.vRows.push_back( MakeRow( L"Loaded", Commas( (long long)st.llKeptTracks ) + L" (the rest identical copies)", 1 ) );
        song.vRows.push_back( MakeRow( L"Channel events", Commas( (long long)st.llEvents ) ) );
        if ( st.llOutOfRange ) song.vRows.push_back( MakeRow( L"Dropped", Commas( (long long)st.llOutOfRange ) + L" (key or value past 127)", 1 ) );
        song.vRows.push_back( MakeRow( L"Notes", Commas( (long long)st.llNotes ) ) );
        song.vRows.push_back( MakeRow( L"Paired", Commas( (long long)st.llPairedNotes ), 1 ) );
        song.vRows.push_back( MakeRow( L"Unpaired", Commas( (long long)st.llUnpairedNotes ), 1 ) );
        song.vRows.push_back( MakeRow( L"Paired more than once", Commas( (long long)st.llWeirdNotes ), 1 ) );
        song.vRows.push_back( MakeRow( L"Unique ticks", Commas( (long long)st.llUniqueTicks ) ) );
        song.vRows.push_back( MakeRow( L"Tick groups", Commas( (long long)st.llGroups ) + ( st.bBitmapMerge ? L" (bitmap merge)" : L" (sort merge)" ) ) );

        // The process's memory
        LoadView::Section mem;
        mem.sTitle = L"PROCESS MEMORY";
        mem.vRows.push_back( MakeRow( L"Before loading", MBText( (double)st.llPrivateBefore ) ) );
        mem.vRows.push_back( MakeRow( L"Peak during the load", MBText( (double)st.llPrivatePeak ) + L" (+" +
                                      MBText( (double)( st.llPrivatePeak > st.llPrivateBefore ? st.llPrivatePeak - st.llPrivateBefore : 0 ) ) + L")" ) );
        mem.vRows.push_back( MakeRow( L"After loading", MBText( (double)st.llPrivateAfter ) ) );

        // The song's memory, by part
        LoadView::Section parts;
        parts.sTitle = L"SONG IN MEMORY";
        size_t iMem = 0;
        for ( size_t s : st.aMem ) iMem += s;
        const double dMem = std::max< double >( 1.0, (double)iMem );
        auto Part = [&]( const wchar_t *sName, size_t iBytes, const std::wstring &sNote ) {
            LoadView::Row r = MakeRow( sName, MBText( (double)iBytes ) );
            r.dBar = iBytes / dMem;
            r.sNote = sNote;
            parts.vRows.push_back( r );
        };
        const double dEvents = std::max< double >( 1.0, (double)st.llEvents ), dNotes = std::max< double >( 1.0, (double)st.llNotes );
        Part( L"Event stream (LZ4)", st.aMem[0], Fmt( L"%.3f B/event", st.aMem[0] / dEvents ) );
        Part( L"Stream index", st.aMem[1], L"" );
        Part( L"Tick table", st.aMem[2], L"" );
        Part( L"Time table", st.aMem[3], L"" );
        Part( L"Metadata", st.aMem[5], L"" );
        Part( L"Notes (visual store)", st.aMem[6], Fmt( L"%.3f B/note", st.aMem[6] / dNotes ) );
        Part( L"Total", iMem, Fmt( L"%.3f B/event", iMem / dEvents ) );
        // (wide: the time and the song's memory on the left, the rest on the right)
        song.iColumn = mem.iColumn = file.iColumn = 1;
        m.vSections = { time, song, parts, mem, file };
        return m;
    }

    // The buttons along the bottom, the view above them
    void LayOut( HWND hWnd )
    {
        RECT rc, rcBtn;
        GetClientRect( hWnd, &rc );
        HWND hCancel = GetDlgItem( hWnd, IDCANCEL ), hCopy = GetDlgItem( hWnd, IDC_LOAD_COPY );
        GetWindowRect( hCancel, &rcBtn );
        int cxBtn = rcBtn.right - rcBtn.left, cyBtn = rcBtn.bottom - rcBtn.top;
        GetWindowRect( hCopy, &rcBtn );
        int cxCopy = rcBtn.right - rcBtn.left;
        const int iPad = cyBtn / 2 + 2, yBtn = rc.bottom - iPad - cyBtn;
        SetWindowPos( GetDlgItem( hWnd, IDC_LOAD_VIEW ), nullptr, 0, 0, rc.right, std::max< int >( 0, yBtn - iPad ), SWP_NOZORDER );
        SetWindowPos( hCancel, nullptr, rc.right - iPad - cxBtn, yBtn, 0, 0, SWP_NOZORDER | SWP_NOSIZE );
        SetWindowPos( hCopy, nullptr, iPad, yBtn, cxCopy, cyBtn, SWP_NOZORDER );
        InvalidateRect( hWnd, nullptr, TRUE );
    }

    void ShowStats()
    {
        LoadView::Set( View(), StatsModel( g_pJob->stats, g_pJob->sFile, g_pJob->sError ) );
        ShowWindow( GetDlgItem( g_hWndDlg, IDC_LOAD_COPY ), SW_SHOW );
        SetDlgItemTextW( g_hWndDlg, IDCANCEL, L"Close" );
        SetWindowTextW( g_hWndDlg, L"Loading statistics - PFAviz++" );
        // Wide enough for two columns, then as tall as they need, as the screen allows (kept centred where it was)
        HWND hView = View();
        RECT rcView, rcDlg;
        GetWindowRect( g_hWndDlg, &rcDlg );
        MONITORINFO mi = { sizeof( mi ) };
        GetMonitorInfoW( MonitorFromWindow( g_hWndDlg, MONITOR_DEFAULTTONEAREST ), &mi );
        const RECT &w = mi.rcWork;
        int cx = std::max< int >( rcDlg.right - rcDlg.left, std::min< int >( 1060, ( w.right - w.left ) * 95 / 100 ) );
        int x = std::clamp< int >( ( rcDlg.left + rcDlg.right - cx ) / 2, w.left, w.right - cx );
        SetWindowPos( g_hWndDlg, nullptr, x, rcDlg.top, cx, rcDlg.bottom - rcDlg.top, SWP_NOZORDER );     // (the view laid out again at this width)
        GetWindowRect( g_hWndDlg, &rcDlg );
        GetClientRect( hView, &rcView );
        const int cyMax = ( w.bottom - w.top ) * 9 / 10;
        int cy = std::min< int >( cyMax, rcDlg.bottom - rcDlg.top + LoadView::ContentHeight( hView ) - rcView.bottom );
        if ( cy != rcDlg.bottom - rcDlg.top )
        {
            int y = std::clamp< int >( ( rcDlg.top + rcDlg.bottom - cy ) / 2, w.top, w.bottom - cy );
            SetWindowPos( g_hWndDlg, nullptr, rcDlg.left, y, rcDlg.right - rcDlg.left, cy, SWP_NOZORDER );
        }
    }

    void CopyStats()
    {
        if ( !g_pJob ) return;
        std::wstring s = g_pJob->stats.Format();
        if ( !g_pJob->sError.empty() ) s = L"Load failed: " + g_pJob->sError + L"\r\n\r\n" + s;
        if ( !OpenClipboard( g_hWndDlg ) ) return;
        EmptyClipboard();
        size_t n = s.size() + 1;
        HGLOBAL h = GlobalAlloc( GMEM_MOVEABLE, n * sizeof( wchar_t ) );
        if ( h )
        {
            memcpy( GlobalLock( h ), s.c_str(), n * sizeof( wchar_t ) );
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
                LayOut( hWnd );
                RECT rcDlg, rcMain;
                GetWindowRect( hWnd, &rcDlg );
                GetWindowRect( g_hWnd, &rcMain );
                SetWindowPos( hWnd, nullptr, rcMain.left + ( rcMain.right - rcMain.left - ( rcDlg.right - rcDlg.left ) ) / 2,
                              rcMain.top + ( rcMain.bottom - rcMain.top - ( rcDlg.bottom - rcDlg.top ) ) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER );
                SetTimer( hWnd, 1, 100, nullptr );
                UpdateProgress();
                return TRUE;
            }
            case WM_SIZE:
                LayOut( hWnd );
                return TRUE;
            case WM_GETMINMAXINFO:
            {
                MINMAXINFO *p = ( MINMAXINFO * )lParam;
                p->ptMinTrackSize.x = 300;     // (narrower than the content: it scrolls sideways)
                p->ptMinTrackSize.y = 240;
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
                        // Loading: let go of at once. Its thread stops by itself (FinishLoad deletes it)
                        if ( g_pJob && !g_pJob->bFinished )
                        {
                            g_pJob->progress.bCancel = true;
                            g_vStopping.push_back( g_pJob );
                            g_pJob = nullptr;
                        }
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
bool LoadThreadsRunning() { return IsLoading() || !g_vStopping.empty(); }
HWND GetLoadingDialog() { return g_hWndDlg; }

bool StartLoad( const std::wstring &sFileIn, bool bRenderOnly )
{
    if ( IsLoading() ) return false;
    if ( g_hWndDlg ) DestroyWindow( g_hWndDlg ); // a previous statistics window

    wchar_t sFull[MAX_PATH * 4];
    DWORD dwLen = GetFullPathNameW( sFileIn.c_str(), MAX_PATH * 4, sFull, nullptr );
    g_pJob = new LoadJob();
    g_pJob->sFile = ( dwLen > 0 && dwLen < MAX_PATH * 4 ) ? std::wstring( sFull ) : sFileIn;
    g_pJob->llStart = Timer::NowMicroSecs();
    g_pJob->bRenderOnly = bRenderOnly;
    for ( LoadJob *pOld : g_vStopping )
    {
        HANDLE h = nullptr;
        if ( DuplicateHandle( GetCurrentProcess(), pOld->hThread, GetCurrentProcess(), &h, SYNCHRONIZE, FALSE, 0 ) ) g_pJob->vWaitFor.push_back( h );
    }

    LoadView::ClassName( g_hInstance );     // (the dialog's view, registered)
    CreateDialogParamW( g_hInstance, MAKEINTRESOURCEW( IDD_LOADING ), g_hWnd, LoadingProc, 0 );
    if ( g_hWndDlg ) ShowWindow( g_hWndDlg, SW_SHOW );
    g_pJob->hThread = CreateThread( nullptr, 0, LoadThread, g_pJob, 0, nullptr );
    if ( !g_pJob->hThread )
    {
        if ( g_hWndDlg ) DestroyWindow( g_hWndDlg );
        for ( HANDLE h : g_pJob->vWaitFor ) CloseHandle( h );
        delete g_pJob;
        g_pJob = nullptr;
        return false;
    }
    return true;
}

void ChooseMidiEntry( LPARAM lParam )
{
    DialogBoxParamW( g_hInstance, MAKEINTRESOURCEW( IDD_CHOOSEMIDI ), g_hWndDlg ? g_hWndDlg : g_hWnd, ChooseMidiProc, lParam );
}

static std::wstring g_sOpenedFile;
const std::wstring &GetOpenedFile() { return g_sOpenedFile; }

void FinishLoad( LPARAM lParam )
{
    LoadJob *pJob = reinterpret_cast< LoadJob * >( lParam );
    WaitForSingleObject( pJob->hThread, INFINITE );
    CloseHandle( pJob->hThread );
    pJob->bFinished = true;

    // A load cancelled earlier, now stopped: nothing to show (its window closed when it was cancelled)
    auto itStopping = std::find( g_vStopping.begin(), g_vStopping.end(), pJob );
    if ( itStopping != g_vStopping.end() )
    {
        g_vStopping.erase( itStopping );
        if ( pJob->song ) std::thread( [p = std::move( pJob->song )]() mutable { p.reset(); } ).detach();  // (finished just as it was cancelled)
        delete pJob;
        return;
    }

    const PerformanceSettings &cPerf = Config::GetConfig().GetPerformanceSettings();
    bool bCancelled = pJob->progress.bCancel;
    bool bOk = !bCancelled && pJob->song && pJob->sError.empty();

    if ( bOk )
    {
        PlaybackSettings &cPlayback = Config::GetConfig().GetPlaybackSettings();
        ViewSettings &cView = Config::GetConfig().GetViewSettings();
        std::shared_ptr< const Song > pSong( pJob->song.release() );
        MainScreen *pGameState = new MainScreen( pSong, nullptr, nullptr, !pJob->bRenderOnly );
        const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
        pGameState->SetChannelColors( cVisual.colors, cVisual.iColorCount );
        if ( !cPlayback.GetPlayable() ) cPlayback.SetPlayable( true, true );
        if ( cPlayback.GetPlayMode() != PlayMode::Play ) cPlayback.SetPlayMode( PlayMode::Play, true );
        pGameState->SetStartPaused( pJob->bRenderOnly || Config::GetConfig().GetControlsSettings().bStartPaused );
        cPlayback.SetPosition( 0 );
        cView.SetZoomMove( false, true );
        std::wstring sTitle = pJob->sFile.substr( pJob->sFile.find_last_of( L'\\' ) + 1 );
        SetWindowTextW( g_hWnd, ( pJob->bRenderOnly ? sTitle + L" (render only, no playback)" : sTitle ).c_str() );
        g_sOpenedFile = pJob->sFile;
        HandOffMsg( WM_COMMAND, ID_CHANGESTATE, ( LPARAM )pGameState );
        if ( const wchar_t *sRender = _wgetenv( L"PFAVIZ_RENDER" ) ) PostMessageW( g_hWnd, WM_APP_RENDERVIDEO, 0, ( LPARAM )sRender );
        else if ( pJob->bRenderOnly ) PostMessageW( g_hWnd, WM_APP_RENDERSONG, 0, 0 );
    }
    pJob->song.reset();

    bool bKeepOpen = !bCancelled && ( cPerf.bShowLoadStats || !bOk );
    if ( g_hWndDlg && bKeepOpen )
        ShowStats();
    else if ( g_hWndDlg )
        DestroyWindow( g_hWndDlg ); // also frees the job
    if ( !g_hWndDlg && g_pJob == pJob ) { delete pJob; g_pJob = nullptr; }
}
