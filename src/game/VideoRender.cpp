// MainScreen::RenderVideo: the whole song rendered off-screen, frame by frame at the video's rate,
// and streamed to ffmpeg (app/VideoExport.h). A frame is the live window's scene at the video's size
// (background, measure lines, notes, keyboard, border) plus, optionally, the top-right stats area;
// the notes come from an engine of its own, so the live view is left as it was.
#include "game/GameState.h"

#include <algorithm>
#include <cmath>

#include "app/VideoExport.h"
#include "core/Config.h"

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
    FfmpegPipe pipe;
    std::wstring sError;
    if ( !pipe.Start( job, sError ) ) return job.Finish( false, sError );

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
    job.llFrames = llFrames;
    job.llSongEnd = llLast;
    job.eState = VideoJob::Rendering;

    // The stats area as the window shows it (its frame rate line shows the video's)
    unsigned uStats = Config::GetConfig().GetVisualSettings().uStats & ~( VisualSettings::SynthStats | VisualSettings::FrameStats );
    int iLines = StatusLines( uStats );
    RECT rcStatusBox = { iW - 156, 0, iW, 6 + 16 * iLines };

    const int iPreviewW = std::min( iW, 480 ), iPreviewH = std::max( 1, (int)( (long long)iH * iPreviewW / iW ) );
    long long llLastPreview = 0;
    long long f = 0;
    for ( ; f < llFrames && !job.bCancel && !pipe.Failed(); f++ )
    {
        long long llTime = llFirst + (long long)std::llround( f * 1e6 / job.iFPS );
        if ( f == 0 )
        {
            engine.Jump( llTime, llTime, llTime + m_llTimeSpan );
            meter.Jump( llTime );
        }
        engine.AdvanceEnd( llTime + m_llTimeSpan );
        if ( f ) engine.Process( llTime );
        meter.Advance( llTime );
        FrameGlobals g = Scene::ComputeGlobals( GetViewInputs( iW, iH, llTime ) );

        r.Clear( 0x00000000 );
        r.BeginScene();
        Scene::DrawBackground( &r, g, m_Colors );
        meter.DrawMeasureLines( &r, g, m_Colors, meter.GetCurrentTick( llTime ) );
        engine.DrawNotes( &r, g, m_vTrackSettings );
        if ( m_bShowKB )
        {
            int aSlot[128];
            engine.GetKeySlots( aSlot );
            const ChannelSettings *pKeyColor[128];
            for ( int i = 0; i < 128; i++ )
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

        std::vector< uint8_t > *pFrame = pipe.Acquire();
        if ( !pFrame ) break;
        if ( !r.ReadBackRaw( pFrame->data() ) )
        {
            pipe.Submit( pFrame );
            sError = L"Reading the rendered frame back from Direct3D failed.";
            break;
        }

        // A scaled-down copy for the progress popup, a few times a second
        long long llNow = Timer::NowMicroSecs();
        if ( llNow - llLastPreview >= 200000 || f + 1 == llFrames )
        {
            llLastPreview = llNow;
            std::lock_guard< std::mutex > lk( job.mx );
            job.vPreview.resize( (size_t)iPreviewW * iPreviewH );
            const DWORD *pSrc = reinterpret_cast< const DWORD * >( pFrame->data() );
            for ( int y = 0; y < iPreviewH; y++ )
            {
                const DWORD *pRow = pSrc + (size_t)( (long long)y * iH / iPreviewH ) * iW;
                for ( int x = 0; x < iPreviewW; x++ ) job.vPreview[(size_t)y * iPreviewW + x] = pRow[(long long)x * iW / iPreviewW];
            }
            job.iPreviewW = iPreviewW;
            job.iPreviewH = iPreviewH;
            job.uPreviewGen++;
        }
        pipe.Submit( pFrame );
        job.llFrame = f + 1;
        job.llSongTime = llTime;
    }

    // ffmpeg finishes what it got (a cancelled render leaves a shorter, playable video)
    job.eState = VideoJob::Finishing;
    std::wstring sFfmpegError;
    bool bEncoded = pipe.Finish( &job.bAbort, sFfmpegError );
    if ( !sError.empty() ) return job.Finish( false, sError );
    if ( !bEncoded ) return job.Finish( false, sFfmpegError );
    if ( pipe.Failed() ) return job.Finish( false, L"ffmpeg stopped reading frames." );

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    double dMB = GetFileAttributesExW( job.sOutFile.c_str(), GetFileExInfoStandard, &fad ) ? ( ( (ULONGLONG)fad.nFileSizeHigh << 32 ) | fad.nFileSizeLow ) / 1048576.0 : 0.0;
    wchar_t buf[256];
    if ( job.bCancel )
        swprintf( buf, 256, L"Stopped at %lld of %lld frames; the part rendered so far was saved (%.1f MB).", f, llFrames, dMB );
    else
        swprintf( buf, 256, L"Done: saved %.1f MB.", dMB );
    job.Finish( true, buf );
}
