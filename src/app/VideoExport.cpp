// Offline video rendering: the ffmpeg pipe, the settings popup and the progress popup.
#include "app/VideoExport.h"

#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <cmath>
#include <cstdarg>
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

namespace
{
    std::wstring Quoted( const std::wstring &s ) { return L"\"" + s + L"\""; }

    std::wstring Format( const wchar_t *sFormat, ... )
    {
        wchar_t buf[1024];
        va_list va;
        va_start( va, sFormat );
        vswprintf( buf, 1024, sFormat, va );
        va_end( va );
        return buf;
    }

    // The last lines of ffmpeg's output (its own progress and the SVT encoder's notes left out)
    std::wstring LastLines( const std::string &s, size_t iLines )
    {
        std::vector< std::string > v;
        for ( size_t i = 0; i < s.size(); )
        {
            size_t j = std::min( s.find_first_of( "\r\n", i ), s.size() );
            std::string sLine = s.substr( i, j - i );
            if ( !sLine.empty() && sLine.rfind( "Svt[info]", 0 ) != 0 ) v.push_back( sLine );
            i = j + 1;
        }
        std::string sOut;
        for ( size_t i = v.size() > iLines ? v.size() - iLines : 0; i < v.size(); i++ ) sOut += v[i] + "\n";
        return Util::Utf8ToWide( sOut );
    }
}

std::wstring FfmpegPipe::EncoderArgs( const VideoJob &job )
{
    typedef ExportSettings ES;
    // The preset (x264's ultrafast ... veryslow) onto each encoder's speed levels, and the quality
    // (x264's CRF scale) onto each one's own
    int iP = std::clamp( job.iPreset, 0, ES::PresetCount - 1 ), iLast = ES::PresetCount - 1;
    const wchar_t *sPreset = ES::Presets[iP];
    const wchar_t *sAmf = iP < 3 ? L"speed" : iP < 6 ? L"balanced" : L"quality";
    const wchar_t *sQsv = ES::Presets[std::max( iP, 2 )];   // QSV has no ultrafast or superfast
    int q = std::clamp( job.iCRF, 0, 51 );
    const wchar_t *sEnc = ES::EncoderName( job.iCodec, job.iBackend );
    // HEVC in MP4 and MOV: the tag Apple's players need
    const wchar_t *sTag = job.iCodec == ES::Hevc && ( job.iContainer == ES::Mp4 || job.iContainer == ES::Mov ) ? L" -tag:v hvc1" : L"";
    switch ( job.iCodec )
    {
        case ES::ProRes: return L"-c:v prores_ks -profile:v 3 -vendor apl0 -pix_fmt yuv422p10le";
        // RGB with an opaque alpha plane: 8-bit RGB without one decodes as padded 0RGB, which VLC shows
        // with its channels shifted (yellow); the alpha costs a few percent
        case ES::Ffv1: return L"-c:v ffv1 -level 3 -g 1 -slices 16 -slicecrc 1 -pix_fmt bgra";
    }
    switch ( job.iBackend )
    {
        case ES::Nvenc:
            return Format( L"-c:v %ls -preset p%d -tune hq -rc vbr -cq %d -b:v 0 -pix_fmt yuv420p%ls", sEnc, 1 + iP * 6 / iLast, std::max( q, 1 ), sTag );
        case ES::Amf:
        {
            int qp = job.iCodec == ES::Av1 ? q * 255 / 51 : q;     // AV1's QP goes to 255
            return Format( L"-c:v %ls -quality %ls -rc cqp -qp_i %d -qp_p %d -pix_fmt yuv420p%ls", sEnc, sAmf, qp, qp, sTag );
        }
        case ES::Qsv:
            return Format( L"-c:v %ls -preset %ls -global_quality %d -pix_fmt nv12%ls", sEnc, sQsv, std::max( q, 1 ), sTag );
    }
    switch ( job.iCodec )
    {
        case ES::Hevc:
            return Format( L"-c:v libx265 -preset %ls %ls -pix_fmt yuv420p%ls", sPreset,
                           q ? Format( L"-crf %d -x265-params log-level=error", q ).c_str() : L"-x265-params lossless=1:log-level=error", sTag );
        case ES::Av1:   // SVT-AV1: presets 12 (fastest here) down to 4, CRF 1-63
            return Format( L"-c:v libsvtav1 -preset %d -crf %d -pix_fmt yuv420p", 12 - iP, std::max( 1, q * 63 / 51 ) );
        default:        // x264: quality 0 is lossless
            return q ? Format( L"-c:v libx264 -preset %ls -crf %d -pix_fmt yuv420p", sPreset, q )
                     : Format( L"-c:v libx264 -preset %ls -qp 0 -pix_fmt yuv420p", sPreset );
    }
}

int FfmpegPipe::Run( const std::wstring &sFfmpeg, const std::wstring &sArgs, std::string &sOutput, DWORD dwTimeoutMs )
{
    sOutput.clear();
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if ( !CreatePipe( &hRead, &hWrite, &sa, 1 << 16 ) ) return NotRun;
    SetHandleInformation( hRead, HANDLE_FLAG_INHERIT, 0 );
    HANDLE hNul = CreateFileW( L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr );
    std::wstring sCmd = Quoted( sFfmpeg ) + L" " + sArgs;
    std::vector< wchar_t > vCmd( sCmd.begin(), sCmd.end() );
    vCmd.push_back( 0 );
    STARTUPINFOW si = {};
    si.cb = sizeof( si );
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNul;
    si.hStdOutput = si.hStdError = hWrite;
    PROCESS_INFORMATION pi = {};
    BOOL bStarted = CreateProcessW( sFfmpeg.c_str(), vCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi );
    CloseHandle( hWrite );
    if ( hNul != INVALID_HANDLE_VALUE ) CloseHandle( hNul );
    if ( !bStarted ) { CloseHandle( hRead ); return NotRun; }
    CloseHandle( pi.hThread );
    // Read until ffmpeg closes its end (it exits), stopping it at the time limit
    ULONGLONG llEnd = GetTickCount64() + dwTimeoutMs;
    bool bTimedOut = false;
    char buf[4096];
    for ( ;; )
    {
        DWORD dwAvail = 0;
        if ( !PeekNamedPipe( hRead, nullptr, 0, nullptr, &dwAvail, nullptr ) ) break;     // closed: done
        if ( dwAvail )
        {
            DWORD dwRead = 0;
            if ( !ReadFile( hRead, buf, std::min< DWORD >( dwAvail, sizeof( buf ) ), &dwRead, nullptr ) ) break;
            if ( sOutput.size() < ( 1u << 20 ) ) sOutput.append( buf, dwRead );
            continue;
        }
        if ( GetTickCount64() > llEnd ) { bTimedOut = true; TerminateProcess( pi.hProcess, 1 ); break; }
        Sleep( 5 );
    }
    WaitForSingleObject( pi.hProcess, 5000 );
    DWORD dwExit = 1;
    GetExitCodeProcess( pi.hProcess, &dwExit );
    CloseHandle( pi.hProcess );
    CloseHandle( hRead );
    return bTimedOut ? NotRun : (int)dwExit;
}

bool FfmpegPipe::TryEncoder( const VideoJob &job, std::wstring &sError )
{
    std::wstring sArgs = Format( L"-hide_banner -nostats -loglevel error -f lavfi -i color=c=black:s=%dx%d:r=%d:d=1 -frames:v 1 ",
                                 job.iWidth, job.iHeight, job.iFPS ) + EncoderArgs( job ) + L" -f null -";
    std::string sOut;
    int iExit = Run( job.sFfmpeg, sArgs, sOut, 60000 );
    if ( iExit == 0 ) return true;
    sError = Format( L"The %ls encoder (%ls in ffmpeg) cannot encode %d x %d video with these settings", ExportSettings::Codecs[job.iCodec],
                     ExportSettings::EncoderName( job.iCodec, job.iBackend ), job.iWidth, job.iHeight );
    if ( iExit == NotRun ) sError += L": ffmpeg did not answer.";
    else
    {
        sError += L".\n\nffmpeg said:\n" + LastLines( sOut, 6 );
        if ( job.iBackend != ExportSettings::Software )
            sError += L"\nHardware encoders need a graphics card and driver that have them (NVENC on NVIDIA, AMF on AMD, Quick Sync on "
                      L"Intel; AV1 only on recent ones), and have smaller size limits than the software encoders.";
    }
    return false;
}

double FfmpegPipe::FirstSound( const std::wstring &sFfmpeg, const std::wstring &sAudio, std::wstring &sError )
{
    std::string sOut;
    int iExit = Run( sFfmpeg, L"-hide_banner -nostats -t 600 -i " + Quoted( sAudio ) +
                     L" -map 0:a:0 -af silencedetect=noise=-60dB:duration=0.01 -f null -", sOut, 120000 );
    if ( iExit != 0 )
    {
        sError = L"The audio file could not be read";
        sError += iExit == NotRun ? L": ffmpeg did not answer." : L".\n\nffmpeg said:\n" + LastLines( sOut, 6 );
        return -1.0;
    }
    // Silence from the start: the sound starts where it ends (all silent: at 0)
    size_t i = sOut.find( "silence_start: " );
    if ( i == std::string::npos || atof( sOut.c_str() + i + 15 ) > 0.001 ) return 0.0;
    size_t j = sOut.find( "silence_end: ", i );
    return j == std::string::npos ? 0.0 : std::max( 0.0, atof( sOut.c_str() + j + 13 ) );
}

bool FfmpegPipe::Start( const VideoJob &job, std::wstring &sError )
{
    typedef ExportSettings ES;
    // The audio's place: its first sound at the first note's, then the offset (late: delayed; early: cut)
    double dAudioDelay = job.iAudioOffsetMs / 1000.0;
    if ( !job.sAudioFile.empty() && job.bAudioAlign )
    {
        double dFirst = FirstSound( job.sFfmpeg, job.sAudioFile, sError );
        if ( dFirst < 0.0 ) return false;
        dAudioDelay += job.dFirstNoteSecs - dFirst;
    }

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

    // Raw frames in: YUV 4:2:0 already converted (BT.709, limited range), or BGRX that ffmpeg converts
    // (to BT.709 limited-range YUV; FFV1 keeps it RGB). The frames carry the colour description: ffmpeg
    // takes it from them, not from the output options.
    bool bRgb = job.iCodec == ES::Ffv1;
    std::wstring sArgs = Format( L"-hide_banner -loglevel warning -f rawvideo -pix_fmt %ls -video_size %dx%d -framerate %d -i -",
                                 job.bYuv ? L"yuv420p" : L"bgr0", job.iWidth, job.iHeight, job.iFPS );
    bool bAudio = !job.sAudioFile.empty();
    if ( bAudio )
    {
        if ( dAudioDelay < 0.0 ) sArgs += Format( L" -ss %.6f", -dAudioDelay );
        sArgs += L" -i " + Quoted( job.sAudioFile ) + L" -map 0:v:0 -map 1:a:0";
    }
    const wchar_t *sTags = L"setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv";
    if ( job.bYuv ) sArgs += Format( L" -vf %ls", sTags );
    else if ( !bRgb ) sArgs += Format( L" -vf scale=out_color_matrix=bt709:out_range=tv,%ls", sTags );
    sArgs += L" " + EncoderArgs( job );
    if ( !bRgb ) sArgs += L" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
    if ( bAudio )
    {
        // Each format's usual audio: AAC in MP4 (and MOV, but for ProRes), FLAC (up to 24-bit) in MKV, PCM in AVI
        if ( dAudioDelay > 0.0 ) sArgs += Format( L" -af adelay=delays=%lld:all=1", (long long)std::llround( dAudioDelay * 1000.0 ) );
        switch ( job.iContainer )
        {
            case ES::Mkv: sArgs += L" -c:a flac -sample_fmt s32"; break;
            case ES::Avi: sArgs += L" -c:a pcm_s24le"; break;
            case ES::Mov: sArgs += job.iCodec == ES::ProRes ? L" -c:a pcm_s24le" : L" -c:a aac -b:a 320k"; break;
            default: sArgs += L" -c:a aac -b:a 320k"; break;
        }
        // No longer than the video
        sArgs += Format( L" -t %.6f", job.dDurationSecs );
    }
    else sArgs += L" -an";
    if ( job.iContainer == ES::Mp4 || job.iContainer == ES::Mov ) sArgs += L" -movflags +faststart";
    std::wstring sCmd = Quoted( job.sFfmpeg ) + L" " + sArgs + L" -y " + Quoted( job.sOutFile );
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
    size_t iBytes = job.bYuv ? (size_t)job.iWidth * job.iHeight * 3 / 2 : (size_t)job.iWidth * job.iHeight * 4;
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
        { 15360, 8640, L"15360 x 8640 (16K)" },
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

    // The file name with the format's extension in place of its own
    std::wstring WithExtension( const std::wstring &sFile, int iContainer )
    {
        size_t iSlash = sFile.find_last_of( L"\\/" ), iDot = sFile.find_last_of( L'.' );
        std::wstring sBase = iDot != std::wstring::npos && ( iSlash == std::wstring::npos || iDot > iSlash ) ? sFile.substr( 0, iDot ) : sFile;
        return sBase + L"." + ExportSettings::Extensions[iContainer];
    }

    // The saved file's folder (or the Videos folder) and the open song's name
    std::wstring DefaultOutFile( const std::wstring &sSaved, int iContainer )
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
        if ( !sSong.empty() && Config::GetConfig().GetPlaybackSettings().GetPlayMode() == PlayMode::Play )
        {
            sName = sSong.substr( sSong.find_last_of( L"\\/" ) + 1 );
            sName = sName.substr( 0, sName.find_last_of( L'.' ) ) + L".mp4";
        }
        sName = WithExtension( sName, iContainer );
        return sDir.empty() ? sName : InFolder( sDir, sName.c_str() );
    }

    // The audio area is in use only while Add audio is checked
    void UpdateAudioControls( HWND hWnd )
    {
        BOOL bOn = IsDlgButtonChecked( hWnd, IDC_VID_AUDIOON ) == BST_CHECKED;
        for ( int id : { IDC_VID_AUDIOLABEL, IDC_VID_AUDIO, IDC_VID_AUDIOBROWSE, IDC_VID_AUDIOALIGN, IDC_VID_AUDIOMOVE, IDC_VID_AUDIOOFFSET, IDC_VID_AUDIOMS } )
            EnableWindow( GetDlgItem( hWnd, id ), bOn );
    }

    // The encoder list for the codec: the hardware encoders only for H.264, HEVC and AV1
    void UpdateEncoderControls( HWND hWnd )
    {
        int iCodec = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_CODEC, CB_GETCURSEL, 0, 0 ) );
        int iBackend = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_ENCODER, CB_GETCURSEL, 0, 0 ) );
        bool bAny = ExportSettings::HasBackend( iCodec, ExportSettings::Nvenc );
        if ( !bAny )
        {
            SendDlgItemMessageW( hWnd, IDC_VID_ENCODER, CB_SETCURSEL, ExportSettings::Software, 0 );
            iBackend = ExportSettings::Software;
        }
        EnableWindow( GetDlgItem( hWnd, IDC_VID_ENCODER ), bAny );
        std::wstring sNote = std::wstring( L"ffmpeg's " ) + ExportSettings::EncoderName( iCodec, iBackend );
        if ( iCodec == ExportSettings::ProRes || iCodec == ExportSettings::Ffv1 ) sNote += L" (no quality or preset)";
        SetDlgItemTextW( hWnd, IDC_VID_ENCNOTE, sNote.c_str() );
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
        s.iCodec = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_CODEC, CB_GETCURSEL, 0, 0 ) );
        s.iBackend = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_ENCODER, CB_GETCURSEL, 0, 0 ) );
        s.iContainer = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_CONTAINER, CB_GETCURSEL, 0, 0 ) );
        if ( !ExportSettings::HasBackend( s.iCodec, s.iBackend ) ) s.iBackend = ExportSettings::Software;
        wchar_t sAudio[MAX_PATH * 4] = {};
        GetDlgItemTextW( hWnd, IDC_VID_AUDIO, sAudio, MAX_PATH * 4 );
        s.sAudioFile = sAudio;
        s.bAudio = IsDlgButtonChecked( hWnd, IDC_VID_AUDIOON ) == BST_CHECKED;
        s.bAudioAlign = IsDlgButtonChecked( hWnd, IDC_VID_AUDIOALIGN ) == BST_CHECKED;

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
        if ( !ExportSettings::Holds( s.iContainer, s.iCodec ) )
        {
            wchar_t buf[256];
            swprintf( buf, 256, L"%ls cannot be stored in %ls files. MKV holds every codec; MP4 holds H.264, HEVC and AV1; MOV holds H.264, "
                      L"HEVC and ProRes; AVI holds H.264 and FFV1.", ExportSettings::Codecs[s.iCodec], ExportSettings::Extensions[s.iContainer] );
            return Bad( IDC_VID_CODEC, buf );
        }
        if ( !s.bUseWindowSize )
        {
            // Any size ffmpeg takes (each encoder's own limit is checked when rendering starts); 4:2:0
            // and 4:2:2 video needs even sizes, FFV1's RGB does not
            bool bEven = s.iCodec != ExportSettings::Ffv1;
            wchar_t buf[128];
            swprintf( buf, 128, L"Please specify %ls width between 16 and %d pixels.", bEven ? L"an even" : L"a", ExportSettings::MaxSide );
            u = GetDlgItemInt( hWnd, IDC_VID_WIDTH, &bOk, FALSE );
            if ( !bOk || u < 16 || u > (UINT)ExportSettings::MaxSide || ( bEven && ( u & 1 ) ) ) return Bad( IDC_VID_WIDTH, buf );
            s.iWidth = (int)u;
            swprintf( buf, 128, L"Please specify %ls height between 16 and %d pixels.", bEven ? L"an even" : L"a", ExportSettings::MaxSide );
            u = GetDlgItemInt( hWnd, IDC_VID_HEIGHT, &bOk, FALSE );
            if ( !bOk || u < 16 || u > (UINT)ExportSettings::MaxSide || ( bEven && ( u & 1 ) ) ) return Bad( IDC_VID_HEIGHT, buf );
            s.iHeight = (int)u;
        }
        u = GetDlgItemInt( hWnd, IDC_VID_FPS, &bOk, FALSE );
        if ( !bOk || u < 1 || u > 240 ) return Bad( IDC_VID_FPS, L"Please specify a frame rate between 1 and 240 fps." );
        s.iFPS = (int)u;
        u = GetDlgItemInt( hWnd, IDC_VID_CRF, &bOk, FALSE );
        if ( !bOk || u > 51 ) return Bad( IDC_VID_CRF, L"Please specify a quality between 0 and 51." );
        s.iCRF = (int)u;
        int iOffset = (int)GetDlgItemInt( hWnd, IDC_VID_AUDIOOFFSET, &bOk, TRUE );
        bool bOffsetOk = bOk && iOffset >= -3600000 && iOffset <= 3600000;
        if ( !bOffsetOk && s.bAudio ) return Bad( IDC_VID_AUDIOOFFSET, L"Please specify the audio's move in milliseconds (negative: earlier)." );
        if ( bOffsetOk ) s.iAudioOffsetMs = iOffset;     // (audio off: a bad one is left as it was)
        if ( s.bAudio && s.sAudioFile.empty() ) return Bad( IDC_VID_AUDIO, L"Please choose an audio file, or uncheck Add audio for a silent video." );
        if ( s.bAudio && !IsFile( s.sAudioFile ) ) return Bad( IDC_VID_AUDIO, L"The audio file was not found. Please choose an existing file, or uncheck Add audio for a silent video." );
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

    // The popup can be resized: its size and its controls' places as it opened, which the preview's
    // growth moves (the file name, bar and status widen; the preview takes the rest; Cancel stays at
    // the bottom right). The size it was left at is kept for the next render
    struct ProgressLayout { SIZE szMin; SIZE szClient; RECT aCtl[5]; };
    const int aProgressIds[5] = { IDC_VIDP_FILE, IDC_VIDP_PREVIEW, IDC_VIDP_BAR, IDC_VIDP_STATUS, IDCANCEL };
    ProgressLayout g_Layout;
    SIZE g_szProgress = {};             // the window size last used (0: the dialog's own)

    VOID LayoutProgress( HWND hWnd, VideoJob *pJob )
    {
        RECT rc;
        GetClientRect( hWnd, &rc );
        int dx = rc.right - g_Layout.szClient.cx, dy = rc.bottom - g_Layout.szClient.cy;
        HDWP hDwp = BeginDeferWindowPos( 5 );
        for ( int i = 0; i < 5 && hDwp; i++ )
        {
            RECT r = g_Layout.aCtl[i];
            switch ( aProgressIds[i] )
            {
                case IDC_VIDP_FILE: r.right += dx; break;
                case IDC_VIDP_PREVIEW: r.right += dx; r.bottom += dy; break;
                case IDC_VIDP_BAR: case IDC_VIDP_STATUS: r.right += dx; OffsetRect( &r, 0, dy ); break;
                default: OffsetRect( &r, dx, dy ); break;
            }
            hDwp = DeferWindowPos( hDwp, GetDlgItem( hWnd, aProgressIds[i] ), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                   SWP_NOZORDER | SWP_NOACTIVATE );
        }
        if ( hDwp ) EndDeferWindowPos( hDwp );
        // The preview is made as wide as it is shown (letterboxed to the frame's shape)
        RECT rp = g_Layout.aCtl[1];
        int cx = rp.right - rp.left + dx, cy = rp.bottom - rp.top + dy;
        if ( pJob && pJob->iWidth > 0 && pJob->iHeight > 0 )
            pJob->iPreviewWant = std::max( 1, std::min( cx, (int)( (long long)cy * pJob->iWidth / pJob->iHeight ) ) );
        InvalidateRect( hWnd, nullptr, TRUE );
        InvalidateRect( GetDlgItem( hWnd, IDC_VIDP_PREVIEW ), nullptr, FALSE );
    }

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
                {
                    RECT rc;
                    GetWindowRect( hWnd, &rc );
                    g_Layout.szMin = { rc.right - rc.left, rc.bottom - rc.top };
                    GetClientRect( hWnd, &rc );
                    g_Layout.szClient = { rc.right, rc.bottom };
                    for ( int i = 0; i < 5; i++ )
                    {
                        GetWindowRect( GetDlgItem( hWnd, aProgressIds[i] ), &g_Layout.aCtl[i] );
                        MapWindowPoints( nullptr, hWnd, reinterpret_cast< POINT * >( &g_Layout.aCtl[i] ), 2 );
                    }
                    if ( g_szProgress.cx >= g_Layout.szMin.cx && g_szProgress.cy >= g_Layout.szMin.cy )
                        SetWindowPos( hWnd, nullptr, 0, 0, g_szProgress.cx, g_szProgress.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
                    LayoutProgress( hWnd, pJob );
                }
                SetTimer( hWnd, 1, 100, nullptr );
                return TRUE;
            case WM_GETMINMAXINFO:
                if ( g_Layout.szMin.cx ) reinterpret_cast< MINMAXINFO * >( lParam )->ptMinTrackSize = { g_Layout.szMin.cx, g_Layout.szMin.cy };
                return TRUE;
            case WM_SIZE:
                if ( pJob && g_Layout.szClient.cx )
                {
                    LayoutProgress( hWnd, pJob );
                    RECT rc;
                    GetWindowRect( hWnd, &rc );
                    if ( wParam == SIZE_RESTORED ) g_szProgress = { rc.right - rc.left, rc.bottom - rc.top };
                }
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
                                fprintf( f, "render %s: %s | %lld frames in %.2f s (%.1f fps), %s frames, encoder %s\n", bOk ? "ok" : "FAILED",
                                         Util::WideToUtf8( sResult ).c_str(), llFrame, dElapsed, dFps, pJob->bYuv ? "YUV" : "BGRX",
                                         Util::WideToUtf8( ExportSettings::EncoderName( pJob->iCodec, pJob->iBackend ) ).c_str() );
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
                std::lock_guard< std::mutex > lk( pJob->mx );
                if ( pJob->vPreview.empty() ) { FillRect( pdis->hDC, &rc, ( HBRUSH )GetStockObject( BLACK_BRUSH ) ); return TRUE; }
                // Letterboxed to the frame's shape; only the bars are cleared, so the picture does not
                // flicker black between frames
                int cx = rc.right - rc.left, cy = rc.bottom - rc.top;
                int w = cx, h = (int)( (long long)cx * pJob->iHeight / pJob->iWidth );
                if ( h > cy ) { h = cy; w = (int)( (long long)cy * pJob->iWidth / pJob->iHeight ); }
                int x0 = rc.left + ( cx - w ) / 2, y0 = rc.top + ( cy - h ) / 2;
                SaveDC( pdis->hDC );
                ExcludeClipRect( pdis->hDC, x0, y0, x0 + w, y0 + h );
                FillRect( pdis->hDC, &rc, ( HBRUSH )GetStockObject( BLACK_BRUSH ) );
                RestoreDC( pdis->hDC, -1 );
                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize = sizeof( bmi.bmiHeader );
                bmi.bmiHeader.biWidth = pJob->iPreviewW;
                bmi.bmiHeader.biHeight = -pJob->iPreviewH;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode( pdis->hDC, HALFTONE );
                StretchDIBits( pdis->hDC, x0, y0, w, h, 0, 0, pJob->iPreviewW, pJob->iPreviewH,
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
        pJob->iPreset = s.iPreset;
        pJob->iCodec = s.iCodec;
        pJob->iBackend = s.iBackend;
        pJob->iContainer = s.iContainer;
        pJob->sAudioFile = s.bAudio ? s.sAudioFile : std::wstring();   // empty: a silent video
        pJob->bAudioAlign = s.bAudioAlign;
        pJob->iAudioOffsetMs = s.iAudioOffsetMs;
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
        if ( cPlayback.GetPlayMode() != PlayMode::Play )
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

        // The encoder tried at this size first, so its own limits (and a missing GPU) show now
        std::shared_ptr< VideoJob > pJob = MakeJob( s, sFfmpeg, sOut );
        std::wstring sError;
        HCURSOR hOld = SetCursor( LoadCursorW( nullptr, IDC_WAIT ) );
        bool bEncodes = FfmpegPipe::TryEncoder( *pJob, sError );
        SetCursor( hOld );
        if ( !bEncodes )
        {
            MessageBoxW( hWnd, sError.c_str(), L"Render Video", MB_OK | MB_ICONEXCLAMATION );
            return false;
        }
        RunJob( hWnd, pJob );
        return true;
    }

    INT_PTR CALLBACK ExportProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                const ExportSettings &s = Config::GetConfig().GetExportSettings();
                SetDlgItemTextW( hWnd, IDC_VID_FILE, DefaultOutFile( s.sOutFile, s.iContainer ).c_str() );
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
                for ( int i = 0; i < ExportSettings::ContainerCount; i++ )
                    SendDlgItemMessageW( hWnd, IDC_VID_CONTAINER, CB_ADDSTRING, 0, ( LPARAM )ExportSettings::Containers[i] );
                SendDlgItemMessageW( hWnd, IDC_VID_CONTAINER, CB_SETCURSEL, s.iContainer, 0 );
                for ( int i = 0; i < ExportSettings::CodecCount; i++ )
                    SendDlgItemMessageW( hWnd, IDC_VID_CODEC, CB_ADDSTRING, 0, ( LPARAM )ExportSettings::Codecs[i] );
                SendDlgItemMessageW( hWnd, IDC_VID_CODEC, CB_SETCURSEL, s.iCodec, 0 );
                for ( int i = 0; i < ExportSettings::BackendCount; i++ )
                    SendDlgItemMessageW( hWnd, IDC_VID_ENCODER, CB_ADDSTRING, 0, ( LPARAM )ExportSettings::Backends[i] );
                SendDlgItemMessageW( hWnd, IDC_VID_ENCODER, CB_SETCURSEL, s.iBackend, 0 );
                CheckDlgButton( hWnd, IDC_VID_STATS, s.bShowStats ? BST_CHECKED : BST_UNCHECKED );
                CheckDlgButton( hWnd, IDC_VID_AUDIOON, s.bAudio ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemTextW( hWnd, IDC_VID_AUDIO, s.sAudioFile.c_str() );
                CheckDlgButton( hWnd, IDC_VID_AUDIOALIGN, s.bAudioAlign ? BST_CHECKED : BST_UNCHECKED );
                SetDlgItemInt( hWnd, IDC_VID_AUDIOOFFSET, (UINT)s.iAudioOffsetMs, TRUE );
                UpdateSizeControls( hWnd );
                UpdateEncoderControls( hWnd );
                UpdateAudioControls( hWnd );
                return TRUE;
            }
            case WM_COMMAND:
                switch ( LOWORD( wParam ) )
                {
                    case IDC_VID_WINDOWSIZE:
                        UpdateSizeControls( hWnd );
                        return TRUE;
                    case IDC_VID_AUDIOON:
                        UpdateAudioControls( hWnd );
                        return TRUE;
                    case IDC_VID_RES:
                        if ( HIWORD( wParam ) == CBN_SELCHANGE ) UpdateSizeControls( hWnd );
                        return TRUE;
                    case IDC_VID_CODEC: case IDC_VID_ENCODER:
                        if ( HIWORD( wParam ) == CBN_SELCHANGE ) UpdateEncoderControls( hWnd );
                        return TRUE;
                    case IDC_VID_CONTAINER:
                        // The file name follows the format
                        if ( HIWORD( wParam ) == CBN_SELCHANGE )
                        {
                            int iContainer = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_CONTAINER, CB_GETCURSEL, 0, 0 ) );
                            wchar_t sFile[MAX_PATH * 4] = {};
                            GetDlgItemTextW( hWnd, IDC_VID_FILE, sFile, MAX_PATH * 4 );
                            if ( sFile[0] ) SetDlgItemTextW( hWnd, IDC_VID_FILE, WithExtension( sFile, iContainer ).c_str() );
                            g_sBrowsed.clear();
                        }
                        return TRUE;
                    case IDC_VID_AUDIOBROWSE:
                    {
                        wchar_t sFile[MAX_PATH * 4] = {};
                        GetDlgItemTextW( hWnd, IDC_VID_AUDIO, sFile, MAX_PATH * 4 );
                        OPENFILENAMEW ofn = {};
                        ofn.lStructSize = sizeof( ofn );
                        ofn.hwndOwner = hWnd;
                        ofn.lpstrFilter = L"Audio (*.wav;*.flac;*.mp3;*.ogg;*.opus;*.m4a;*.aac;*.aiff;*.w64)\0*.wav;*.flac;*.mp3;*.ogg;*.opus;*.m4a;*.aac;*.aif;*.aiff;*.w64\0All files (*.*)\0*.*\0";
                        ofn.lpstrFile = sFile;
                        ofn.nMaxFile = MAX_PATH * 4;
                        ofn.lpstrTitle = L"Choose the audio for the video";
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                        if ( GetOpenFileNameW( &ofn ) ) SetDlgItemTextW( hWnd, IDC_VID_AUDIO, sFile );
                        return TRUE;
                    }
                    case IDC_VID_BROWSE:
                    {
                        wchar_t sFile[MAX_PATH * 4] = {};
                        GetDlgItemTextW( hWnd, IDC_VID_FILE, sFile, MAX_PATH * 4 );
                        OPENFILENAMEW ofn = {};
                        ofn.lStructSize = sizeof( ofn );
                        ofn.hwndOwner = hWnd;
                        // The filter of the chosen format, whose extension a typed name gets
                        int iContainer = std::max( 0, (int)SendDlgItemMessageW( hWnd, IDC_VID_CONTAINER, CB_GETCURSEL, 0, 0 ) );
                        ofn.lpstrFilter = L"MP4 video (*.mp4)\0*.mp4\0Matroska video (*.mkv)\0*.mkv\0QuickTime video (*.mov)\0*.mov\0AVI video (*.avi)\0*.avi\0";
                        ofn.nFilterIndex = iContainer + 1;
                        ofn.lpstrFile = sFile;
                        ofn.nMaxFile = MAX_PATH * 4;
                        ofn.lpstrDefExt = ExportSettings::Extensions[iContainer];
                        // No change of folder: ffmpeg is looked for in the current one
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                        if ( GetSaveFileNameW( &ofn ) )
                        {
                            // The format follows the filter the file was saved with
                            SendDlgItemMessageW( hWnd, IDC_VID_CONTAINER, CB_SETCURSEL, std::clamp( (int)ofn.nFilterIndex - 1, 0, ExportSettings::ContainerCount - 1 ), 0 );
                            std::wstring sChosen = WithExtension( sFile, std::clamp( (int)ofn.nFilterIndex - 1, 0, ExportSettings::ContainerCount - 1 ) );
                            SetDlgItemTextW( hWnd, IDC_VID_FILE, sChosen.c_str() );
                            g_sBrowsed = sChosen;
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
                        else
                        {
                            s.sOutFile = sRead.sOutFile;
                            s.bUseWindowSize = sRead.bUseWindowSize;
                            s.bShowStats = sRead.bShowStats;
                            s.iPreset = sRead.iPreset;
                            s.iCodec = sRead.iCodec;
                            s.iBackend = sRead.iBackend;
                            s.iContainer = sRead.iContainer;
                            s.bAudio = sRead.bAudio;
                            s.sAudioFile = sRead.sAudioFile;
                            s.bAudioAlign = sRead.bAudioAlign;
                        }
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
    // PFAVIZ_CODEC=n, PFAVIZ_BACKEND=n: ExportSettings::Codec and Backend instead of the saved ones (the
    // format follows the file's extension)
    ExportSettings s = Config::GetConfig().GetExportSettings();
    if ( const char *sCodec = getenv( "PFAVIZ_CODEC" ) ) s.iCodec = std::clamp( atoi( sCodec ), 0, ExportSettings::CodecCount - 1 );
    if ( const char *sBackend = getenv( "PFAVIZ_BACKEND" ) ) s.iBackend = std::clamp( atoi( sBackend ), 0, ExportSettings::BackendCount - 1 );
    if ( !ExportSettings::HasBackend( s.iCodec, s.iBackend ) ) s.iBackend = ExportSettings::Software;
    for ( int i = 0; i < ExportSettings::ContainerCount; i++ )
        if ( WithExtension( sOutFile, i ) == sOutFile ) s.iContainer = i;
    if ( !sFfmpeg.empty() && dwLen > 0 && dwLen < MAX_PATH * 4 )
        RunJob( g_hWnd, MakeJob( s, sFfmpeg, sFull ) );
    PostMessageW( g_hWnd, WM_CLOSE, 0, 0 );
}

VOID DoVideoExport( HWND hWndOwner )
{
    DialogBoxParamW( g_hInstance, MAKEINTRESOURCEW( IDD_VIDEOEXPORT ), hWndOwner, ExportProc, 0 );
}
