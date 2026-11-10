// Offline video rendering (Performance > Render video...). The UI thread collects the settings, finds
// ffmpeg and checks that the chosen encoder takes the frame size; the game thread renders every frame
// off-screen (MainScreen::RenderVideo, through FrameCapture) and FfmpegPipe streams them to ffmpeg's
// stdin as raw YUV 4:2:0 or BGRX frames, with an audio file muxed in when one is chosen. With the
// rendered audio, the audio renderer (audio/RenderAhead.h, offline) makes the song's audio from the
// video's first frame on while the frames are rendered, into a temporary file; the video is encoded
// on its own and the two are put together at the end. The progress popup polls the job.
#pragma once

#include <windows.h>
#include <atomic>
#include <climits>
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
    std::wstring sFfmpeg, sOutFile;
    int iWidth = 0, iHeight = 0, iFPS = 60, iCRF = 20, iPreset = 2;
    int iCodec = 0, iBackend = 0, iContainer = 0;     // ExportSettings::Codec, Backend, Container
    bool bShowStats = true;
    std::wstring sAudioFile;            // empty: no audio
    bool bAudioAlign = true;
    int iAudioOffsetMs = 0;
    bool bRenderedAudio = false;        // the audio renderer's audio instead of sAudioFile
    std::wstring sEncodeTo;             // (set by the renderer) where ffmpeg encodes the video, when not sOutFile
    // Set by the renderer
    bool bYuv = false;                  // frames are YUV 4:2:0 (packed on the GPU), else BGRX
    double dFirstNoteSecs = 0.0;        // the video time the song's first note sounds at
    double dDurationSecs = 0.0;         // the video's length

    // Progress, written by the game thread
    enum State { Starting, Rendering, Finishing, AudioWait, Muxing, Done };
    std::atomic< int > eState{ Starting };
    // The rendered audio: frames made of all, and how it is going
    enum AudioState { AudioNone, AudioLoading, AudioMaking, AudioDone, AudioFailed };
    std::atomic< int > eAudio{ AudioNone };
    std::atomic< long long > llAudioFrame{ 0 }, llAudioFrames{ 0 };
    mutable std::atomic< double > dMuxDone{ 0.0 };  // putting the audio into the video: how far (0 to 1)
    std::wstring sAudioEngine;          // (set before rendering starts)
    std::atomic< long long > llFrame{ 0 }, llFrames{ 0 };      // frames sent to ffmpeg, of all
    std::atomic< long long > llSongTime{ 0 }, llSongEnd{ 0 };
    std::atomic< bool > bCancel{ false };   // set by the UI: stop rendering, ffmpeg finishes the file so far
    std::atomic< bool > bAbort{ false };    // set by the UI while finishing: kill ffmpeg
    std::atomic< int > iPreviewWant{ 480 }; // set by the UI: the preview's shown width in pixels

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
    // Runs ffmpeg with sArgs, collecting what it writes; its exit code (ffmpeg's errors are negative), or
    // NotRun when it did not start, or ran longer than dwTimeoutMs and was stopped
    static const int NotRun = INT_MIN;
    // (pProgress: ffmpeg's -progress output read as it comes, the time written of dTotalSecs, 0 to 1)
    static int Run( const std::wstring &sFfmpeg, const std::wstring &sArgs, std::string &sOutput, DWORD dwTimeoutMs,
                    std::atomic< double > *pProgress = nullptr, double dTotalSecs = 0.0 );
    // Whether the job's encoder takes its frame size and settings, from it encoding one blank frame: so
    // each encoder's own limits apply (NVENC's H.264 4096 x 4096, x265's 16384 x 8704 samples, ...).
    // sError: why not, in ffmpeg's words.
    static bool TryEncoder( const VideoJob &job, std::wstring &sError );
    // Where an audio file's first sound starts, in seconds (its first moment above -60 dB; the
    // first 10 minutes are looked at). Negative when the file could not be read (sError says why).
    static double FirstSound( const std::wstring &sFfmpeg, const std::wstring &sAudio, std::wstring &sError );
    // The video file sVideo with raw stereo float audio (sRaw, at uRate) put in as the job's format has it,
    // dSecs long, into the job's output file
    static bool Mux( const VideoJob &job, const std::wstring &sVideo, const std::wstring &sRaw, unsigned uRate, double dSecs, std::wstring &sError );

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
    static std::wstring EncoderArgs( const VideoJob &job );     // -c:v, its options and -pix_fmt
    static std::wstring AudioArgs( const VideoJob &job );       // -c:a and its options for the format

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
