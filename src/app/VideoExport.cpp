// Offline video rendering: the ffmpeg pipe, the settings popup and the progress popup.
#include "app/VideoExport.h"

#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <shlobj.h>

#include "app/LoadingDialog.h"
#include "app/MainWindow.h"
#include "core/Config.h"
#include "core/Util.h"
#include "resource.h"

//-----------------------------------------------------------------------------
// FfmpegPipe
//-----------------------------------------------------------------------------

namespace
{
    bool IsFile( const std::wstring &s )
    {
        DWORD a = GetFileAttributesW( s.c_str() );
        return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
    }

    std::wstring InFolder( std::wstring sDir, const wchar_t *sName )
    {
        if ( !sDir.empty() && sDir.back() != L'\\' && sDir.back() != L'/' ) sDir += L'\\';
        return sDir + sName;
    }

    std::wstring CurrentFolder()
    {
        std::vector< wchar_t > buf( 32768 );
        DWORD n = GetCurrentDirectoryW( (DWORD)buf.size(), buf.data() );
        return n && n < buf.size() ? std::wstring( buf.data(), n ) : std::wstring();
    }
}

std::wstring FfmpegPipe::Find()
{
    std::vector< std::wstring > vDirs;
    vDirs.push_back( CurrentFolder() );
    std::vector< wchar_t > buf( 32768 );
    DWORD n = GetModuleFileNameW( nullptr, buf.data(), (DWORD)buf.size() );
    if ( n && n < buf.size() )
    {
        std::wstring sExe( buf.data(), n );
        vDirs.push_back( sExe.substr( 0, sExe.find_last_of( L"\\/" ) ) );
    }
    n = GetEnvironmentVariableW( L"PATH", buf.data(), (DWORD)buf.size() );
    if ( n && n < buf.size() )
    {
        std::wstring sPath( buf.data(), n );
        for ( size_t i = 0; i <= sPath.size(); )
        {
            size_t j = std::min( sPath.find( L';', i ), sPath.size() );
            std::wstring sDir = sPath.substr( i, j - i );
            sDir.erase( std::remove( sDir.begin(), sDir.end(), L'"' ), sDir.end() );
            if ( !sDir.empty() ) vDirs.push_back( sDir );
            i = j + 1;
        }
    }
    for ( const std::wstring &sDir : vDirs )
        if ( !sDir.empty() && IsFile( InFolder( sDir, L"ffmpeg.exe" ) ) )
            return InFolder( sDir, L"ffmpeg.exe" );
    return std::wstring();
}

FfmpegPipe::~FfmpegPipe()
{
    // Only reached without Finish on an error path: kill ffmpeg, which also unblocks the writer
    if ( m_hProcess && WaitForSingleObject( m_hProcess, 0 ) == WAIT_TIMEOUT ) TerminateProcess( m_hProcess, 1 );
    if ( m_Thread.joinable() )
    {
        { std::lock_guard< std::mutex > lk( m_mx ); m_bStop = true; }
        m_cv.notify_all();
        m_Thread.join();
    }
    if ( m_hStdin ) CloseHandle( m_hStdin );
    if ( m_hProcess ) CloseHandle( m_hProcess );
}

bool FfmpegPipe::Start( const VideoJob &job, std::wstring &sError )
{
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), nullptr, TRUE };
    HANDLE hRead = nullptr;
    if ( !CreatePipe( &hRead, &m_hStdin, &sa, 1 << 20 ) )
    {
        sError = L"Could not create the pipe to ffmpeg.";
        return false;
    }
    SetHandleInformation( m_hStdin, HANDLE_FLAG_INHERIT, 0 );

    wchar_t sTemp[MAX_PATH + 1] = {};
    GetTempPathW( MAX_PATH + 1, sTemp );
    m_sLog = std::wstring( sTemp ) + L"PFAviz++ ffmpeg.log";
    HANDLE hLog = CreateFileW( m_sLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );

    // Raw BGRX frames in; H.264 4:2:0 with BT.709 colours out (what players assume for HD video)
    wchar_t sArgs[1024];
    swprintf( sArgs, 1024,
              L"-hide_banner -loglevel warning -f rawvideo -pix_fmt bgr0 -video_size %dx%d -framerate %d -i - -an "
              L"-vf scale=out_color_matrix=bt709:out_range=tv -c:v libx264 -preset %ls -crf %d -pix_fmt yuv420p "
              L"-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -movflags +faststart -y",
              job.iWidth, job.iHeight, job.iFPS, job.sPreset.c_str(), job.iCRF );
    std::wstring sCmd = L"\"" + job.sFfmpeg + L"\" " + sArgs + L" \"" + job.sOutFile + L"\"";
    std::vector< wchar_t > vCmd( sCmd.begin(), sCmd.end() );
    vCmd.push_back( 0 );

    STARTUPINFOW si = {};
    si.cb = sizeof( si );
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hRead;
    si.hStdOutput = si.hStdError = hLog != INVALID_HANDLE_VALUE ? hLog : nullptr;
    PROCESS_INFORMATION pi = {};
    BOOL bStarted = CreateProcessW( job.sFfmpeg.c_str(), vCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi );
    DWORD dwErr = GetLastError();
    CloseHandle( hRead );
    if ( hLog != INVALID_HANDLE_VALUE ) CloseHandle( hLog );
    if ( !bStarted )
    {
        wchar_t buf[512];
        swprintf( buf, 512, L"Could not start ffmpeg (error %lu):\n%ls", dwErr, job.sFfmpeg.c_str() );
        sError = buf;
        return false;
    }
    CloseHandle( pi.hThread );
    m_hProcess = pi.hProcess;

    // Three frames in flight: one being filled, one being written, one waiting
    size_t iBytes = (size_t)job.iWidth * job.iHeight * 4;
    for ( int i = 0; i < 3; i++ )
    {
        m_vBufs.push_back( std::make_unique< std::vector< uint8_t > >( iBytes ) );
        m_qFree.push_back( m_vBufs.back().get() );
    }
    m_Thread = std::thread( &FfmpegPipe::Writer, this );
    return true;
}

void FfmpegPipe::Writer()
{
    for ( ;; )
    {
        std::vector< uint8_t > *pFrame;
        {
            std::unique_lock< std::mutex > lk( m_mx );
            m_cv.wait( lk, [this] { return m_bStop || !m_qFull.empty(); } );
            if ( m_qFull.empty() ) return;
            pFrame = m_qFull.front();
            m_qFull.pop_front();
        }
        bool bOk = !m_bFailed;
        for ( size_t i = 0; bOk && i < pFrame->size(); )
        {
            DWORD dwWritten = 0;
            DWORD dwChunk = (DWORD)std::min< size_t >( pFrame->size() - i, 1 << 24 );
            bOk = WriteFile( m_hStdin, pFrame->data() + i, dwChunk, &dwWritten, nullptr ) && dwWritten > 0;
            i += dwWritten;
        }
        {
            std::lock_guard< std::mutex > lk( m_mx );
            if ( !bOk ) m_bFailed = true;   // ffmpeg exited (it reports why in its log)
            m_qFree.push_back( pFrame );
        }
        m_cv.notify_all();
    }
}

std::vector< uint8_t > *FfmpegPipe::Acquire()
{
    std::unique_lock< std::mutex > lk( m_mx );
    m_cv.wait( lk, [this] { return m_bFailed || !m_qFree.empty(); } );
    if ( m_bFailed ) return nullptr;
    std::vector< uint8_t > *pFrame = m_qFree.front();
    m_qFree.pop_front();
    return pFrame;
}

void FfmpegPipe::Submit( std::vector< uint8_t > *pFrame )
{
    {
        std::lock_guard< std::mutex > lk( m_mx );
        m_qFull.push_back( pFrame );
    }
    m_cv.notify_all();
}

bool FfmpegPipe::Finish( const std::atomic< bool > *pAbort, std::wstring &sError )
{
    if ( m_Thread.joinable() )
    {
        { std::lock_guard< std::mutex > lk( m_mx ); m_bStop = true; }
        m_cv.notify_all();
        m_Thread.join();
    }
    if ( m_hStdin ) { CloseHandle( m_hStdin ); m_hStdin = nullptr; }
    if ( !m_hProcess ) return false;
    while ( WaitForSingleObject( m_hProcess, 100 ) == WAIT_TIMEOUT )
        if ( pAbort && *pAbort )
        {
            TerminateProcess( m_hProcess, 1 );
            WaitForSingleObject( m_hProcess, 5000 );
            sError = L"Aborted: ffmpeg was stopped before it finished the file, which is probably unplayable.";
            return false;
        }
    DWORD dwExit = 1;
    GetExitCodeProcess( m_hProcess, &dwExit );
    if ( dwExit == 0 ) return true;
    wchar_t buf[128];
    swprintf( buf, 128, L"ffmpeg failed (exit code %ld).", (long)dwExit );
    sError = buf;
    std::wstring sTail = LogTail();
    if ( !sTail.empty() ) sError += L"\n\nffmpeg said:\n" + sTail;
    return false;
}

// The last lines ffmpeg wrote
std::wstring FfmpegPipe::LogTail() const
{
    HANDLE h = CreateFileW( m_sLog.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
    if ( h == INVALID_HANDLE_VALUE ) return std::wstring();
    LARGE_INTEGER llSize = {};
    GetFileSizeEx( h, &llSize );
    LONGLONG llStart = std::max< LONGLONG >( 0, llSize.QuadPart - 2048 );
    LARGE_INTEGER llPos;
    llPos.QuadPart = llStart;
    SetFilePointerEx( h, llPos, nullptr, FILE_BEGIN );
    std::string s( (size_t)( llSize.QuadPart - llStart ), '\0' );
    DWORD dwRead = 0;
    if ( !s.empty() ) ReadFile( h, &s[0], (DWORD)s.size(), &dwRead, nullptr );
    CloseHandle( h );
    s.resize( dwRead );
    std::vector< std::string > vLines;
    for ( size_t i = 0; i < s.size(); )
    {
        size_t j = std::min( s.find_first_of( "\r\n", i ), s.size() );
        if ( j > i ) vLines.push_back( s.substr( i, j - i ) );
        i = j + 1;
    }
    if ( llStart > 0 && !vLines.empty() ) vLines.erase( vLines.begin() );     // cut in the middle
    std::string sOut;
    for ( size_t i = vLines.size() > 8 ? vLines.size() - 8 : 0; i < vLines.size(); i++ ) sOut += vLines[i] + "\n";
    return Util::Utf8ToWide( sOut );
}

//-----------------------------------------------------------------------------
// Popups
//-----------------------------------------------------------------------------

namespace
{
    struct Resolution { int iWidth, iHeight; const wchar_t *sName; };
    const Resolution aResolutions[] = {
        { 1280, 720, L"1280 x 720 (720p)" },
        { 1920, 1080, L"1920 x 1080 (1080p)" },
        { 2560, 1440, L"2560 x 1440 (1440p)" },
        { 3840, 2160, L"3840 x 2160 (4K)" },
        { 7680, 4320, L"7680 x 4320 (8K)" },
    };
    const int ResolutionCount = sizeof( aResolutions ) / sizeof( aResolutions[0] );   // then "Custom"

    std::wstring g_sBrowsed;            // the path the save dialog confirmed (no second overwrite question)

    // The graphics window's client size, rounded down to even numbers (4:2:0 video)
    void WindowSize( int &iWidth, int &iHeight )
    {
        RECT rc = {};
        GetClientRect( g_hWndGfx, &rc );
        iWidth = std::max( 16, (int)( rc.right - rc.left ) & ~1 );
        iHeight = std::max( 16, (int)( rc.bottom - rc.top ) & ~1 );
    }

    // The saved file's folder (or the Videos folder) and the open song's name
    std::wstring DefaultOutFile( const std::wstring &sSaved )
    {
        std::wstring sDir, sName = L"PFAviz++.mp4";
        size_t iSlash = sSaved.find_last_of( L"\\/" );
        if ( iSlash != std::wstring::npos )
        {
            sDir = sSaved.substr( 0, iSlash );
            sName = sSaved.substr( iSlash + 1 );
        }
        else
        {
            PWSTR pPath = nullptr;
            if ( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_Videos, 0, nullptr, &pPath ) ) ) sDir = pPath;
            CoTaskMemFree( pPath );
        }
        const std::wstring &sSong = GetOpenedFile();
        if ( !sSong.empty() && Config::GetConfig().GetPlaybackSettings().GetPlayMode() == PlayMode::Practice )
        {
            sName = sSong.substr( sSong.find_last_of( L"\\/" ) + 1 );
            sName = sName.substr( 0, sName.find_last_of( L'.' ) ) + L".mp4";
        }
        return sDir.empty() ? sName : InFolder( sDir, sName.c_str() );
    }

    void UpdateSizeControls( HWND hWnd )
    {
        bool bWindow = IsDlgButtonChecked( hWnd, IDC_VID_WINDOWSIZE ) == BST_CHECKED;
        int iSel = (int)SendDlgItemMessageW( hWnd, IDC_VID_RES, CB_GETCURSEL, 0, 0 );
        bool bCustom = iSel < 0 || iSel >= ResolutionCount;
        EnableWindow( GetDlgItem( hWnd, IDC_VID_RES ), !bWindow );
        EnableWindow( GetDlgItem( hWnd, IDC_VID_WIDTH ), !bWindow && bCustom );
        EnableWindow( GetDlgItem( hWnd, IDC_VID_HEIGHT ), !bWindow && bCustom );
        int iWidth = 0, iHeight = 0;
        if ( bWindow ) WindowSize( iWidth, iHeight );
        else if ( !bCustom ) { iWidth = aResolutions[iSel].iWidth; iHeight = aResolutions[iSel].iHeight; }
        else return;
        SetDlgItemInt( hWnd, IDC_VID_WIDTH, iWidth, FALSE );
        SetDlgItemInt( hWnd, IDC_VID_HEIGHT, iHeight, FALSE );
    }

    // Reads the popup into s. With bValidate, complains about the first bad value and returns false.
    bool ReadExport( HWND hWnd, ExportSettings &s, bool bValidate )
    {
        wchar_t sFile[MAX_PATH * 4] = {};
        GetDlgItemTextW( hWnd, IDC_VID_FILE, sFile, MAX_PATH * 4 );
        s.sOutFile = sFile;
        s.bUseWindowSize = IsDlgButtonChecked( hWnd, IDC_VID_WINDOWSIZE ) == BST_CHECKED;
        s.bShowStats = IsDlgButtonChecked( hWnd, IDC_VID_STATS ) == BST_CHECKED;
        s.iPreset = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_PRESET, CB_GETCURSEL, 0, 0 ) );

        auto Bad = [&]( int iId, const wchar_t *sError ) {
            if ( bValidate )
            {
                MessageBoxW( hWnd, sError, L"Render Video", MB_OK | MB_ICONEXCLAMATION );
                PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, iId ), TRUE );
            }
            return false;
        };
        BOOL bOk = FALSE;
        UINT u;
        if ( !s.bUseWindowSize )
        {
            u = GetDlgItemInt( hWnd, IDC_VID_WIDTH, &bOk, FALSE );
            if ( !bOk || u < 16 || u > 8192 || ( u & 1 ) ) return Bad( IDC_VID_WIDTH, L"Please specify an even width between 16 and 8192 pixels." );
            s.iWidth = (int)u;
            u = GetDlgItemInt( hWnd, IDC_VID_HEIGHT, &bOk, FALSE );
            if ( !bOk || u < 16 || u > 8192 || ( u & 1 ) ) return Bad( IDC_VID_HEIGHT, L"Please specify an even height between 16 and 8192 pixels." );
            s.iHeight = (int)u;
        }
        u = GetDlgItemInt( hWnd, IDC_VID_FPS, &bOk, FALSE );
        if ( !bOk || u < 1 || u > 240 ) return Bad( IDC_VID_FPS, L"Please specify a frame rate between 1 and 240 fps." );
        s.iFPS = (int)u;
        u = GetDlgItemInt( hWnd, IDC_VID_CRF, &bOk, FALSE );
        if ( !bOk || u > 51 ) return Bad( IDC_VID_CRF, L"Please specify an H.264 quality (CRF) between 0 and 51." );
        s.iCRF = (int)u;
        return true;
    }

    std::wstring Clock( double dSecs )
    {
        wchar_t buf[32];
        long long ll = (long long)std::max( 0.0, dSecs );
        if ( ll >= 3600 ) swprintf( buf, 32, L"%lld:%02lld:%02lld", ll / 3600, ll / 60 % 60, ll % 60 );
        else swprintf( buf, 32, L"%lld:%02lld", ll / 60, ll % 60 );
        return buf;
    }

    std::wstring SongClock( long long llUs )
    {
        wchar_t buf[32];
        bool bNeg = llUs < 0;
        llUs = bNeg ? -llUs : llUs;
        swprintf( buf, 32, L"%ls%lld:%04.1lf", bNeg ? L"-" : L"", llUs / 60000000, ( llUs % 60000000 ) / 1000000.0 );
        return buf;
    }

    // Progress popup: polls the job, shows the latest frame scaled down
    VideoJob *g_pProgress = nullptr;
    long long g_llProgressStart = 0;
    unsigned g_uShownGen = 0;
    bool g_bUnattended = false;         // PFAVIZ_RENDER: close when done

    INT_PTR CALLBACK ProgressProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        VideoJob *pJob = g_pProgress;
        switch ( msg )
        {
            case WM_INITDIALOG:
                g_pProgress = pJob = reinterpret_cast< VideoJob * >( lParam );
                g_llProgressStart = Timer::NowMicroSecs();
                g_uShownGen = 0;
                SetDlgItemTextW( hWnd, IDC_VIDP_FILE, pJob->sOutFile.c_str() );
                SendDlgItemMessageW( hWnd, IDC_VIDP_BAR, PBM_SETRANGE32, 0, 1000 );
                SetTimer( hWnd, 1, 100, nullptr );
                return TRUE;
            case WM_TIMER:
            {
                double dElapsed = ( Timer::NowMicroSecs() - g_llProgressStart ) / 1e6;
                long long llFrame = pJob->llFrame, llFrames = std::max< long long >( 1, pJob->llFrames );
                int eState = pJob->eState;
                {
                    std::lock_guard< std::mutex > lk( pJob->mx );
                    if ( pJob->uPreviewGen != g_uShownGen )
                    {
                        g_uShownGen = pJob->uPreviewGen;
                        InvalidateRect( GetDlgItem( hWnd, IDC_VIDP_PREVIEW ), nullptr, FALSE );
                    }
                }
                wchar_t buf[512];
                double dFps = dElapsed > 0 ? llFrame / dElapsed : 0;
                if ( eState == VideoJob::Done )
                {
                    KillTimer( hWnd, 1 );
                    std::wstring sResult;
                    bool bOk;
                    { std::lock_guard< std::mutex > lk( pJob->mx ); sResult = pJob->sResult; bOk = pJob->bOk; }
                    if ( bOk ) SendDlgItemMessageW( hWnd, IDC_VIDP_BAR, PBM_SETPOS, (WPARAM)( 1000 * llFrame / llFrames ), 0 );
                    swprintf( buf, 512, L"%ls\n%lld frames in %ls (%.1f fps).", sResult.c_str(), llFrame, Clock( dElapsed ).c_str(), dFps );
                    SetDlgItemTextW( hWnd, IDC_VIDP_STATUS, bOk ? buf : L"Failed. See the message for details." );
                    SetDlgItemTextW( hWnd, IDCANCEL, L"Close" );
                    EnableWindow( GetDlgItem( hWnd, IDCANCEL ), TRUE );
                    SetFocus( GetDlgItem( hWnd, IDCANCEL ) );
                    if ( g_bUnattended )
                    {
                        if ( const char *sLog = getenv( "PFAVIZ_FRAMELOG" ) )
                            if ( FILE *f = fopen( sLog, "a" ) )
                            {
                                fprintf( f, "render %s: %s | %lld frames in %.2f s (%.1f fps)\n", bOk ? "ok" : "FAILED",
                                         Util::WideToUtf8( sResult ).c_str(), llFrame, dElapsed, dFps );
                                fclose( f );
                            }
                        g_pProgress = nullptr;
                        EndDialog( hWnd, IDOK );
                        return TRUE;
                    }
                    if ( !bOk ) MessageBoxW( hWnd, sResult.c_str(), L"Render Video", MB_OK | MB_ICONEXCLAMATION );
                    else MessageBeep( MB_OK );
                    return TRUE;
                }
                SendDlgItemMessageW( hWnd, IDC_VIDP_BAR, PBM_SETPOS, (WPARAM)( 1000 * llFrame / llFrames ), 0 );
                if ( eState == VideoJob::Starting )
                    swprintf( buf, 512, L"Starting..." );
                else if ( eState == VideoJob::Finishing )
                    swprintf( buf, 512, L"ffmpeg is finishing the file...\n%lld frames rendered in %ls.", llFrame, Clock( dElapsed ).c_str() );
                else
                {
                    double dLeft = dFps > 0 ? ( llFrames - llFrame ) / dFps : 0;
                    swprintf( buf, 512, L"Frame %lld of %lld (%.1f %%), song time %ls of %ls\n%.1f fps (%.2fx real time), elapsed %ls, about %ls left%ls",
                              llFrame, llFrames, 100.0 * llFrame / llFrames, SongClock( pJob->llSongTime ).c_str(), SongClock( pJob->llSongEnd ).c_str(),
                              dFps, dFps / pJob->iFPS, Clock( dElapsed ).c_str(), Clock( dLeft ).c_str(),
                              pJob->bCancel ? L"\nStopping: ffmpeg will finish the part rendered so far." : L"" );
                }
                SetDlgItemTextW( hWnd, IDC_VIDP_STATUS, buf );
                return TRUE;
            }
            case WM_DRAWITEM:
            {
                LPDRAWITEMSTRUCT pdis = ( LPDRAWITEMSTRUCT )lParam;
                if ( pdis->CtlID != IDC_VIDP_PREVIEW ) return FALSE;
                RECT rc = pdis->rcItem;
                FillRect( pdis->hDC, &rc, ( HBRUSH )GetStockObject( BLACK_BRUSH ) );
                std::lock_guard< std::mutex > lk( pJob->mx );
                if ( pJob->vPreview.empty() ) return TRUE;
                // Letterboxed to the frame's shape
                int cx = rc.right - rc.left, cy = rc.bottom - rc.top;
                int w = cx, h = (int)( (long long)cx * pJob->iHeight / pJob->iWidth );
                if ( h > cy ) { h = cy; w = (int)( (long long)cy * pJob->iWidth / pJob->iHeight ); }
                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize = sizeof( bmi.bmiHeader );
                bmi.bmiHeader.biWidth = pJob->iPreviewW;
                bmi.bmiHeader.biHeight = -pJob->iPreviewH;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode( pdis->hDC, HALFTONE );
                StretchDIBits( pdis->hDC, rc.left + ( cx - w ) / 2, rc.top + ( cy - h ) / 2, w, h, 0, 0, pJob->iPreviewW, pJob->iPreviewH,
                               pJob->vPreview.data(), &bmi, DIB_RGB_COLORS, SRCCOPY );
                return TRUE;
            }
            case WM_COMMAND:
                if ( LOWORD( wParam ) == IDCANCEL )
                {
                    int eState = pJob->eState;
                    if ( eState == VideoJob::Done ) { g_pProgress = nullptr; EndDialog( hWnd, IDCANCEL ); }
                    else if ( eState == VideoJob::Finishing || pJob->bCancel )
                    {
                        pJob->bAbort = true;
                        pJob->bCancel = true;
                        EnableWindow( GetDlgItem( hWnd, IDCANCEL ), FALSE );
                    }
                    else
                    {
                        pJob->bCancel = true;
                        SetDlgItemTextW( hWnd, IDCANCEL, L"Abort" );
                    }
                    return TRUE;
                }
                break;
        }
        return FALSE;
    }

    std::shared_ptr< VideoJob > MakeJob( const ExportSettings &s, const std::wstring &sFfmpeg, const std::wstring &sOut )
    {
        auto pJob = std::make_shared< VideoJob >();
        pJob->sFfmpeg = sFfmpeg;
        pJob->sOutFile = sOut;
        pJob->sPreset = ExportSettings::Presets[s.iPreset];
        if ( s.bUseWindowSize ) WindowSize( pJob->iWidth, pJob->iHeight );
        else { pJob->iWidth = s.iWidth; pJob->iHeight = s.iHeight; }
        pJob->iFPS = s.iFPS;
        pJob->iCRF = s.iCRF;
        pJob->bShowStats = s.bShowStats;
        return pJob;
    }

    // The live playback pauses; the game thread renders until the job is done
    void RunJob( HWND hWndOwner, const std::shared_ptr< VideoJob > &pJob )
    {
        Config::GetConfig().GetPlaybackSettings().SetPaused( true, true );
        HandOffMsg( WM_COMMAND, ID_VIDEO_RENDER, ( LPARAM )new std::shared_ptr< VideoJob >( pJob ) );
        DialogBoxParamW( g_hInstance, MAKEINTRESOURCEW( IDD_VIDEOPROGRESS ), hWndOwner, ProgressProc, ( LPARAM )pJob.get() );
    }

    bool StartRender( HWND hWnd )
    {
        Config &config = Config::GetConfig();
        ExportSettings s = config.GetExportSettings();
        if ( !ReadExport( hWnd, s, true ) ) return false;
        config.SetExportSettings( s );

        PlaybackSettings &cPlayback = config.GetPlaybackSettings();
        if ( cPlayback.GetPlayMode() != PlayMode::Practice )
        {
            MessageBoxW( hWnd, L"Please open a song first.", L"Render Video", MB_OK | MB_ICONEXCLAMATION );
            return false;
        }
        std::wstring sFfmpeg = FfmpegPipe::Find();
        if ( sFfmpeg.empty() )
        {
            std::wstring sMsg = L"ffmpeg.exe was not found.\n\nPFAviz++ looks for it in the current folder (" + CurrentFolder() +
                                L"), next to PFAviz++.exe, and in the folders on the PATH.\n\nDownload an ffmpeg build for Windows (for example from "
                                L"https://ffmpeg.org/download.html), then put ffmpeg.exe in one of those places or add its folder to the PATH.";
            MessageBoxW( hWnd, sMsg.c_str(), L"Render Video", MB_OK | MB_ICONEXCLAMATION );
            return false;
        }
        if ( s.sOutFile.empty() )
        {
            MessageBoxW( hWnd, L"Please choose an output file.", L"Render Video", MB_OK | MB_ICONEXCLAMATION );
            PostMessageW( hWnd, WM_NEXTDLGCTL, ( WPARAM )GetDlgItem( hWnd, IDC_VID_FILE ), TRUE );
            return false;
        }
        // A full path, so ffmpeg does not depend on the current folder
        wchar_t sFull[MAX_PATH * 4];
        DWORD dwLen = GetFullPathNameW( s.sOutFile.c_str(), MAX_PATH * 4, sFull, nullptr );
        std::wstring sOut = dwLen > 0 && dwLen < MAX_PATH * 4 ? std::wstring( sFull ) : s.sOutFile;
        std::wstring sDir = sOut.substr( 0, sOut.find_last_of( L"\\/" ) );
        DWORD dwAttr = GetFileAttributesW( sDir.c_str() );
        if ( dwAttr == INVALID_FILE_ATTRIBUTES || !( dwAttr & FILE_ATTRIBUTE_DIRECTORY ) )
        {
            MessageBoxW( hWnd, ( L"The folder does not exist:\n" + sDir ).c_str(), L"Render Video", MB_OK | MB_ICONEXCLAMATION );
            return false;
        }
        if ( IsFile( sOut ) && _wcsicmp( sOut.c_str(), g_sBrowsed.c_str() ) != 0 &&
             MessageBoxW( hWnd, ( sOut + L" already exists.\nDo you want to replace it?" ).c_str(), L"Render Video", MB_YESNO | MB_ICONWARNING ) != IDYES )
            return false;

        RunJob( hWnd, MakeJob( s, sFfmpeg, sOut ) );
        return true;
    }

    INT_PTR CALLBACK ExportProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                const ExportSettings &s = Config::GetConfig().GetExportSettings();
                SetDlgItemTextW( hWnd, IDC_VID_FILE, DefaultOutFile( s.sOutFile ).c_str() );
                g_sBrowsed.clear();
                CheckDlgButton( hWnd, IDC_VID_WINDOWSIZE, s.bUseWindowSize ? BST_CHECKED : BST_UNCHECKED );
                int iSel = ResolutionCount;
                for ( int i = 0; i < ResolutionCount; i++ )
                {
                    SendDlgItemMessageW( hWnd, IDC_VID_RES, CB_ADDSTRING, 0, ( LPARAM )aResolutions[i].sName );
                    if ( aResolutions[i].iWidth == s.iWidth && aResolutions[i].iHeight == s.iHeight ) iSel = i;
                }
                SendDlgItemMessageW( hWnd, IDC_VID_RES, CB_ADDSTRING, 0, ( LPARAM )L"Custom" );
                SendDlgItemMessageW( hWnd, IDC_VID_RES, CB_SETCURSEL, iSel, 0 );
                SetDlgItemInt( hWnd, IDC_VID_WIDTH, s.iWidth, FALSE );
                SetDlgItemInt( hWnd, IDC_VID_HEIGHT, s.iHeight, FALSE );
                SetDlgItemInt( hWnd, IDC_VID_FPS, s.iFPS, FALSE );
                SetDlgItemInt( hWnd, IDC_VID_CRF, s.iCRF, FALSE );
                for ( int i = 0; i < ExportSettings::PresetCount; i++ )
                    SendDlgItemMessageW( hWnd, IDC_VID_PRESET, CB_ADDSTRING, 0, ( LPARAM )ExportSettings::Presets[i] );
                SendDlgItemMessageW( hWnd, IDC_VID_PRESET, CB_SETCURSEL, s.iPreset, 0 );
                CheckDlgButton( hWnd, IDC_VID_STATS, s.bShowStats ? BST_CHECKED : BST_UNCHECKED );
                UpdateSizeControls( hWnd );
                return TRUE;
            }
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDC_VID_WINDOWSIZE:
                        UpdateSizeControls( hWnd );
                        return TRUE;
                    case IDC_VID_RES:
                        if ( HIWORD( wParam ) == CBN_SELCHANGE ) UpdateSizeControls( hWnd );
                        return TRUE;
                    case IDC_VID_BROWSE:
                    {
                        wchar_t sFile[MAX_PATH * 4] = {};
                        GetDlgItemTextW( hWnd, IDC_VID_FILE, sFile, MAX_PATH * 4 );
                        OPENFILENAMEW ofn = {};
                        ofn.lStructSize = sizeof( ofn );
                        ofn.hwndOwner = hWnd;
                        ofn.lpstrFilter = L"MP4 video (*.mp4)\0*.mp4\0Matroska video (*.mkv)\0*.mkv\0All files (*.*)\0*.*\0";
                        ofn.lpstrFile = sFile;
                        ofn.nMaxFile = MAX_PATH * 4;
                        ofn.lpstrDefExt = L"mp4";
                        // No change of folder: ffmpeg is looked for in the current one
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                        if ( GetSaveFileNameW( &ofn ) )
                        {
                            SetDlgItemTextW( hWnd, IDC_VID_FILE, sFile );
                            g_sBrowsed = sFile;
                        }
                        return TRUE;
                    }
                    case IDOK:
                        StartRender( hWnd );
                        return TRUE;
                    case IDCANCEL:
                    {
                        // Keeps what was entered (values that do not parse keep their saved value)
                        Config &config = Config::GetConfig();
                        ExportSettings s = config.GetExportSettings();
                        ExportSettings sRead = s;
                        if ( ReadExport( hWnd, sRead, false ) ) s = sRead;
                        else { s.sOutFile = sRead.sOutFile; s.bUseWindowSize = sRead.bUseWindowSize; s.bShowStats = sRead.bShowStats; s.iPreset = sRead.iPreset; }
                        config.SetExportSettings( s );
                        EndDialog( hWnd, IDCANCEL );
                        return TRUE;
                    }
                }
                break;
        }
        return FALSE;
    }
}

VOID RenderVideoUnattended( const std::wstring &sOutFile )
{
    std::wstring sFfmpeg = FfmpegPipe::Find();
    wchar_t sFull[MAX_PATH * 4];
    DWORD dwLen = GetFullPathNameW( sOutFile.c_str(), MAX_PATH * 4, sFull, nullptr );
    g_bUnattended = true;
    if ( !sFfmpeg.empty() && dwLen > 0 && dwLen < MAX_PATH * 4 )
        RunJob( g_hWnd, MakeJob( Config::GetConfig().GetExportSettings(), sFfmpeg, sFull ) );
    PostMessageW( g_hWnd, WM_CLOSE, 0, 0 );
}

VOID DoVideoExport( HWND hWndOwner )
{
    DialogBoxParamW( g_hInstance, MAKEINTRESOURCEW( IDD_VIDEOEXPORT ), hWndOwner, ExportProc, 0 );
}
