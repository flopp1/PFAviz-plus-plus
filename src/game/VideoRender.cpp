// MainScreen::RenderVideo: the whole song rendered off-screen, frame by frame at the video's rate,
// and streamed to ffmpeg (app/VideoExport.h). A frame is the live window's scene at the video's size
// (background, measure lines, notes, keyboard, border) plus, optionally, the top-right stats area;
// the notes come from an engine of its own, so the live view is left as it was. Frames are read back
// a few frames late (render/FrameCapture.h), packed as YUV 4:2:0 on the GPU where possible.
#include "game/GameState.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app/VideoExport.h"
#include "core/Config.h"
#include "render/FrameCapture.h"

void MainScreen::RenderVideo( const std::shared_ptr< VideoJob > &pJob )
{
    VideoJob &job = *pJob;
    const PerformanceSettings &cPerf = Config::GetConfig().GetPerformanceSettings();
    const int iW = job.iWidth, iH = job.iHeight;

    // The current settings (view, note speed, colours), and the live playback stops: the UI paused it
    Logic();
    if ( m_pPlayback )
    {
        m_Clock.SetPaused( true );
        m_pPlayback->AllNotesOff();
    }

    Renderer r;
    if ( FAILED( r.InitOffscreen( iW, iH ) ) || r.GetBufferWidth() != iW || r.GetBufferHeight() != iH )
    {
        wchar_t buf[128];
        swprintf( buf, 128, L"Direct3D could not create a %d x %d frame to render into.", iW, iH );
        return job.Finish( false, buf );
    }
    // Frames in flight between drawing and readback (developer switches: PFAVIZ_CAPTUREDEPTH=n,
    // PFAVIZ_BGRXPIPE=1 sends BGRX frames for ffmpeg to convert). Codecs that are not 4:2:0 get BGRX.
    int iDepth = getenv( "PFAVIZ_CAPTUREDEPTH" ) ? std::clamp( atoi( getenv( "PFAVIZ_CAPTUREDEPTH" ) ), 1, 8 ) : 3;
    FrameCapture cap;
    std::wstring sError;
    if ( !cap.Init( r, iW, iH, !getenv( "PFAVIZ_BGRXPIPE" ) && ExportSettings::Is420( job.iCodec ), iDepth, sError ) ) return job.Finish( false, sError );
    job.bYuv = cap.Yuv();
    r.SetInstancing( cPerf.bGpuInstancing );
    const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    r.SetBackgroundImage( m_pBkgImage );
    r.SetBackgroundStyle( cVisual.iBkgFit, cVisual.iBkgOpacity / 100.0f );

    // Its own engine, with the hidden channels and the LOD cache ready before each frame is drawn
    FastEngine engine( *m_pSong, cPerf.iWorkerThreads );
    engine.SetLod( cPerf.bLodCache, true );
    std::vector< uint8_t > vHidden( m_vTrackSettings.size() * 16 );
    bool bAnyHidden = false;
    for ( size_t t = 0; t < m_vTrackSettings.size(); t++ )
        for ( int c = 0; c < 16; c++ ) bAnyHidden |= ( vHidden[t * 16 + c] = m_vTrackSettings[t].aChannels[c].bHidden ) != 0;
    if ( bAnyHidden ) engine.SetHidden( vHidden, true );
    MeterTracker meter( m_pSong->vMeta, m_pSong->info );

    // Frame f shows song time llFirst + f / fps, from the start of playback to its end
    const long long llFirst = GetMinTime(), llLast = GetMaxTime();
    const long long llFrames = ( llLast - llFirst ) * job.iFPS / 1000000 + 1;
    auto FrameTime = [&]( long long f ) { return llFirst + (long long)std::llround( f * 1e6 / job.iFPS ); };
    job.llFrames = llFrames;
    job.llSongEnd = llLast;
    // For the audio: the first note sounds when it reaches the keyboard, at its start time
    job.dFirstNoteSecs = ( m_pSong->info.llFirstNote - llFirst ) / 1e6;
    job.dDurationSecs = (double)llFrames / job.iFPS;
    FfmpegPipe pipe;
    if ( !pipe.Start( job, sError ) ) return job.Finish( false, sError );
    job.eState = VideoJob::Rendering;

    // The stats area as the window shows it (its frame rate line shows the video's)
    unsigned uStats = Config::GetConfig().GetVisualSettings().uStats & ~( VisualSettings::SynthStats | VisualSettings::FrameStats );
    int iLines = StatusLines( uStats );
    RECT rcStatusBox = { iW - 156, 0, iW, 6 + 16 * iLines };

    const int iPreviewW = std::min( iW, 480 ), iPreviewH = std::max( 1, (int)( (long long)iH * iPreviewW / iW ) );
    long long llLastPreview = 0, llSent = 0;

    // The oldest frame read back goes to ffmpeg (and, a few times a second, to the progress popup)
    auto Emit = [&]() {
        std::vector< uint8_t > *pFrame = pipe.Acquire();
        if ( !pFrame ) return false;
        if ( !cap.Read( pFrame->data() ) )
        {
            pipe.Submit( pFrame );
            sError = L"Reading the rendered frame back from Direct3D failed.";
            return false;
        }
        long long llNow = Timer::NowMicroSecs();
        if ( llNow - llLastPreview >= 200000 || llSent + 1 == llFrames )
        {
            llLastPreview = llNow;
            std::lock_guard< std::mutex > lk( job.mx );
            job.vPreview.resize( (size_t)iPreviewW * iPreviewH );
            const uint8_t *p = pFrame->data();
            for ( int y = 0; y < iPreviewH; y++ )
            {
                int sy = (int)( (long long)y * iH / iPreviewH );
                for ( int x = 0; x < iPreviewW; x++ )
                {
                    int sx = (int)( (long long)x * iW / iPreviewW );
                    DWORD &d = job.vPreview[(size_t)y * iPreviewW + x];
                    if ( !cap.Yuv() ) { d = reinterpret_cast< const DWORD * >( p )[(size_t)sy * iW + sx]; continue; }
                    // BT.709 limited range back to RGB
                    size_t iC = (size_t)( sy / 2 ) * ( iW / 2 ) + sx / 2, iY = (size_t)iW * iH;
                    float Y = ( p[(size_t)sy * iW + sx] - 16 ) / 219.0f, Pb = ( p[iY + iC] - 128 ) / 224.0f, Pr = ( p[iY + iY / 4 + iC] - 128 ) / 224.0f;
                    float R = Y + 1.5748f * Pr, B = Y + 1.8556f * Pb, G = ( Y - 0.2126f * R - 0.0722f * B ) / 0.7152f;
                    auto C = []( float v ) { return (DWORD)std::clamp( (int)( v * 255.0f + 0.5f ), 0, 255 ); };
                    d = ( C( R ) << 16 ) | ( C( G ) << 8 ) | C( B );
                }
            }
            job.iPreviewW = iPreviewW;
            job.iPreviewH = iPreviewH;
            job.uPreviewGen++;
        }
        pipe.Submit( pFrame );
        job.llSongTime = FrameTime( llSent );
        job.llFrame = ++llSent;
        return true;
    };

    bool bOk = true;
    for ( long long f = 0; f < llFrames && !job.bCancel && !pipe.Failed() && bOk; f++ )
    {
        if ( cap.Ready( false ) && !( bOk = Emit() ) ) break;
        long long llTime = FrameTime( f );
        if ( f == 0 )
        {
            engine.Jump( llTime, llTime, llTime + m_llTimeSpan );
            meter.Jump( llTime );
        }
        engine.AdvanceEnd( llTime + m_llTimeSpan );
        if ( f ) engine.Process( llTime );
        meter.Advance( llTime );
        FrameGlobals g = Scene::ComputeGlobals( GetViewInputs( iW, iH, llTime ) );

        cap.Begin();
        r.Clear( 0x00000000 );
        r.BeginScene();
        Scene::DrawBackground( &r, g, m_Colors );
        meter.DrawMeasureLines( &r, g, m_Colors, meter.GetCurrentTick( llTime ) );
        engine.DrawNotes( &r, g, m_vTrackSettings );
        if ( m_bShowKB )
        {
            int aSlot[256];
            engine.GetKeySlots( aSlot );
            const ChannelSettings *pKeyColor[256];
            for ( int i = 0; i < 256; i++ )
                pKeyColor[i] = aSlot[i] < 0 ? nullptr : &m_vTrackSettings[aSlot[i] >> 4].aChannels[aSlot[i] & 15];
            Scene::DrawKeys( &r, g, m_Colors, pKeyColor );
        }
        Scene::DrawBorder( &r, g, m_Colors );
        if ( job.bShowStats && iLines )
        {
            RECT rcStatus = rcStatusBox;
            r.DrawRect( static_cast< float >( rcStatus.left ), static_cast< float >( rcStatus.top ),
                        static_cast< float >( rcStatus.right - rcStatus.left ), static_cast< float >( rcStatus.bottom - rcStatus.top ), 0x80000000 );
            r.BeginText();
            RenderStatus( &r, &rcStatus, llTime, job.iFPS, uStats );
            r.EndText();
        }
        r.EndScene();
        cap.End();
    }
    // The frames still in flight were drawn: they go to ffmpeg too
    while ( bOk && !pipe.Failed() && cap.Ready( true ) ) bOk = Emit();

    // ffmpeg finishes what it got (a cancelled render leaves a shorter, playable video)
    job.eState = VideoJob::Finishing;
    std::wstring sFfmpegError;
    bool bEncoded = pipe.Finish( &job.bAbort, sFfmpegError );
    if ( !sError.empty() ) return job.Finish( false, sError );
    if ( !bEncoded )
    {
        if ( job.iBackend != ExportSettings::Software )
            sFfmpegError += L"\n\nThe hardware encoders need a graphics card and driver that have them: NVENC on NVIDIA, AMF on AMD, "
                            L"Quick Sync on Intel. The software encoders run on any computer.";
        return job.Finish( false, sFfmpegError );
    }
    if ( pipe.Failed() ) return job.Finish( false, L"ffmpeg stopped reading frames." );

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    double dMB = GetFileAttributesExW( job.sOutFile.c_str(), GetFileExInfoStandard, &fad ) ? ( ( (ULONGLONG)fad.nFileSizeHigh << 32 ) | fad.nFileSizeLow ) / 1048576.0 : 0.0;
    wchar_t buf[256];
    if ( job.bCancel )
        swprintf( buf, 256, L"Stopped at %lld of %lld frames; the part rendered so far was saved (%.1f MB).", llSent, llFrames, dMB );
    else
        swprintf( buf, 256, L"Done: saved %.1f MB.", dMB );
    job.Finish( true, buf );
}
