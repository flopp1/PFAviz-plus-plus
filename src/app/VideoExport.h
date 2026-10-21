// Offline video rendering (Performance > Render video...). The UI thread collects the settings and
// finds ffmpeg; the game thread renders every frame off-screen (MainScreen::RenderVideo) and
// FfmpegPipe streams them to ffmpeg's stdin as raw BGRX frames. The progress popup polls the job.
#pragma once

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VideoJob
{
    // Set by the UI before the job goes to the game thread
    std::wstring sFfmpeg, sOutFile, sPreset;
    int iWidth = 0, iHeight = 0, iFPS = 60, iCRF = 14;
    bool bShowStats = true;

    // Progress, written by the game thread
    enum State { Starting, Rendering, Finishing, Done };
    std::atomic< int > eState{ Starting };
    std::atomic< long long > llFrame{ 0 }, llFrames{ 0 };      // frames sent to ffmpeg, of all
    std::atomic< long long > llSongTime{ 0 }, llSongEnd{ 0 };
    std::atomic< bool > bCancel{ false };   // set by the UI: stop rendering, ffmpeg finishes the file so far
    std::atomic< bool > bAbort{ false };    // set by the UI while finishing: kill ffmpeg

    std::mutex mx;                      // guards the rest
    bool bOk = false;
    std::wstring sResult;               // the outcome; on failure, with ffmpeg's last lines
    std::vector< DWORD > vPreview;      // the latest frame, scaled down (BGRX, top-down)
    int iPreviewW = 0, iPreviewH = 0;
    unsigned uPreviewGen = 0;

    void Finish( bool bSuccess, const std::wstring &s )
    {
        { std::lock_guard< std::mutex > lk( mx ); bOk = bSuccess; sResult = s; }
        eState = Done;
    }
};

// ffmpeg reading raw frames from a pipe. Frames are filled in pooled buffers and written to the
// pipe by a thread of its own, so rendering the next frame overlaps the encoder taking this one.
class FfmpegPipe
{
public:
    ~FfmpegPipe();
    // ffmpeg.exe in the current folder, next to PFAviz++.exe, or on the PATH (empty: not found)
    static std::wstring Find();

    // Starts ffmpeg for the job's size, rate and encoder settings; its messages go to a log file
    bool Start( const VideoJob &job, std::wstring &sError );
    // A frame buffer to fill (blocks while every buffer is queued); nullptr once writing failed
    std::vector< uint8_t > *Acquire();
    void Submit( std::vector< uint8_t > *pFrame );
    bool Failed() const { return m_bFailed; }
    // Closes ffmpeg's input and waits for it to finish the file. pAbort (polled): kill it instead.
    // Returns ffmpeg's success; sError gets the reason and the end of its log otherwise.
    bool Finish( const std::atomic< bool > *pAbort, std::wstring &sError );

private:
    void Writer();
    std::wstring LogTail() const;

    HANDLE m_hProcess = nullptr, m_hStdin = nullptr;
    std::wstring m_sLog;
    std::thread m_Thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    std::vector< std::unique_ptr< std::vector< uint8_t > > > m_vBufs;
    std::deque< std::vector< uint8_t > * > m_qFree, m_qFull;
    bool m_bStop = false;
    std::atomic< bool > m_bFailed{ false };
};

// The settings popup (opened from the Performance tab), which starts the render and shows its progress
VOID DoVideoExport( HWND hWndOwner );
// Developer switch PFAVIZ_RENDER=FILE: renders the song just opened with the saved settings, then
// closes the window (the outcome is appended to PFAVIZ_FRAMELOG when it is set)
VOID RenderVideoUnattended( const std::wstring &sOutFile );
