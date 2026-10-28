#include <cassert>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

#include "audio/PlaybackDecoder.h"
#include "core/Config.h"
#include "core/Ini.h"
#include "midi/Notes.h"
#include "song/Loader.h"
#include "visual/Scene.h"
#include "visual/FastEngine.h"

static std::vector< uint8_t > Midi( bool extended )
{
    std::vector< uint8_t > track;
    const int keys = extended ? 256 : 128;
    for ( int key = 0; key < keys; key++ )
        track.insert( track.end(), { 0, 0x90, (uint8_t)key, 100 } );
    for ( int key = 0; key < keys; key++ )
        track.insert( track.end(), { (uint8_t)( key == 0 ? 96 : 0 ), (uint8_t)( key % 2 ? 0x90 : 0x80 ), (uint8_t)key, 0 } );
    track.insert( track.end(), { 0, 0xFF, 0x2F, 0 } );
    std::vector< uint8_t > file = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k' };
    for ( int shift = 24; shift >= 0; shift -= 8 ) file.push_back( (uint8_t)( track.size() >> shift ) );
    file.insert( file.end(), track.begin(), track.end() );
    return file;
}

static void CheckSong( bool extended, bool full, bool lz, int fused, bool tuning )
{
    auto file = Midi( extended );
    LoadProgress progress;
    progress.bTuning31 = tuning;
    progress.bFullVisual = full;
    progress.bLzVisual = lz;
    progress.iFusedRead = fused;
    LoadStats stats;
    std::wstring error;
    auto song = Loader::LoadMemory( file.data(), file.size(), 2, progress, stats, error );
    if ( !song ) std::fwprintf( stderr, L"%ls\n", error.c_str() );
    assert( song );
    // Keys past 127 are kept only with 31-EDO tuning; otherwise dropped as the original does (each key's on and off)
    const int keys = extended && tuning ? 256 : 128;
    assert( song->bTuning31 == tuning );
    assert( song->info.iMinNote == 0 && song->info.iMaxNote == keys - 1 );
    assert( song->info.llNoteCount == keys );
    assert( stats.llOutOfRange == ( extended && !tuning ? 256u : 0u ) && stats.llMalformedTracks == 0 );
    for ( int key = 0; key < keys; key++ )
    {
        assert( song->aKeyNoteCounts[key] == 1 );
        const auto &store = song->visual.aKeys[key];
        assert( store.Size() == 1 );
        NoteView note;
        store.Get( 0, note );
        assert( !note.bUnpaired && note.llStartU == 0 && note.llEndU == 1 );
    }
    ViewInputs view;
    view.eKeysShown = extended ? VisualSettings::All256 : VisualSettings::All128;
    view.iBufferWidth = 3840;
    view.iBufferHeight = 1080;
    view.llStartTime = 100000;
    auto globals = Scene::ComputeGlobals( view );
    FastEngine engine( *song, 2 );
    engine.AdvanceEnd( view.llStartTime + view.llTimeSpan );
    engine.Process( view.llStartTime );
    int slots[256];
    engine.GetKeySlots( slots );
    const ChannelSettings *keyColors[256] = {};
    std::vector< TrackSettings > tracks( 1 );
    for ( int key = 0; key < keys; key++ )
    {
        assert( slots[key] == 0 );
        keyColors[key] = &tracks[0].aChannels[0];
    }
    Renderer renderer;
    std::vector< Renderer::SCREEN_VERTEX > vertices;
    renderer.SetCapture( &vertices );
    engine.DrawNotes( &renderer, globals, tracks );
    assert( engine.LastStats().iKeys == (size_t)keys && !vertices.empty() );
    SceneColors colors;
    Scene::DrawKeys( &renderer, globals, colors, keyColors );
    for ( const auto &vertex : vertices ) assert( std::isfinite( vertex.x ) && std::isfinite( vertex.y ) );
    std::vector< StreamEvent > events;
    song->stream.Walk( 0, song->stream.UniqueTicks(), [&]( size_t, const StreamEvent &e ) { events.push_back( e ); },
                       []( size_t, uint32_t, bool ) { return true; } );
    assert( events.size() == (size_t)keys * 2 );
    StreamCursor cursor;
    cursor.Attach( &song->stream );
    for ( size_t i = 0; i < events.size(); i++ )
    {
        assert( events[i].cData1 == i % keys );
        assert( events[i].cData2 == ( i < (size_t)keys ? 100 : 0 ) );
        uint32_t track, msg;
        assert( cursor.Next( track, msg ) );
        assert( ( ( msg >> 8 ) & 0xFF ) == events[i].cData1 );
        assert( ( msg >> 16 ) == events[i].cData2 );
    }
    RefMidi reference;
    assert( reference.Parse( file.data(), file.size() ) );
    assert( reference.GetInfo().llNoteCount == 128 );     // the original's parser: keys past 127 dropped
    for ( const auto &event : reference.GetEvents() ) assert( event.iSister >= 0 );
    for ( const auto &bins : song->aSendOns )
    {
        uint64_t ons = 0;
        for ( size_t bin = 0; bin < bins.Bins(); bin++ ) ons += bins.Total( bin );
        assert( ons == (uint64_t)( tuning ? keys : 128 ) );
    }
    for ( bool collapse : { false, true } )
    for ( int floor : { 0, 101 } )
    {
        PlaybackDecoder decoder( *song );
        decoder.Reset( 0 );
        PlaybackDecoder::Params params;
        params.bCollapse = collapse;
        params.iMinVelocity = floor;
        params.bTuning31 = tuning;
        std::vector< uint32_t > messages;
        for ( int attempt = 0; decoder.NextTime( 0 ) != LLONG_MAX; attempt++ )
        {
            assert( attempt < 100 );
            uint32_t out[64];
            long long due;
            size_t n = decoder.Fill( out, 64, 2000000, 0, params, due );
            messages.insert( messages.end(), out, out + n );
        }
        if ( extended && tuning && full && fused == 0 )
        {
            PlaybackDecoder mainLane( *song ), lane( *song );
            mainLane.Reset( 0 );
            PlaybackDecoder *lanes[] = { &mainLane, &lane };
            PlaybackDecoder::LaneOut outputs[2];
            std::vector< uint32_t > parallelMessages;
            for ( int attempt = 0; mainLane.NextTime( 0 ) != LLONG_MAX; attempt++ )
            {
                assert( attempt < 10 );
                PlaybackDecoder::Batch batch;
                assert( mainLane.PlanBatch( 2, 2000000, song->stream.llRawBytes + 1, 0, batch ) );
                mainLane.RunMainLane( batch, params, outputs[0] );
                lane.RunLane( batch, 1, params, outputs[1] );
                mainLane.FinishBatch( batch, lanes, outputs, 2,
                    []( const std::function< void( int ) > &fn ) { fn( 1 ); },
                    [&]( const uint32_t *out, size_t n, long long ) { parallelMessages.insert( parallelMessages.end(), out, out + n ); } );
            }
            assert( parallelMessages == messages );
        }
        const size_t playedKeys = tuning ? keys : 128;
        assert( messages.size() == ( floor ? 0 : playedKeys * 2 ) );
        for ( size_t i = 0; i < messages.size(); i++ )
        {
            assert( ( ( messages[i] >> 8 ) & 0xFF ) == i % playedKeys );
            assert( ( messages[i] >> 16 ) == ( i < playedKeys ? 100 : 0 ) );
        }
    }
}

int wmain( int argc, wchar_t **argv )
{
    if ( argc > 1 )
    {
        LoadProgress progress;
        LoadStats stats;
        std::wstring error;
        std::puts( "Loading supplied MIDI file" );
        std::fflush( stdout );
        auto song = Loader::LoadFile( argv[1], 2, progress, stats, error );
        if ( !song ) std::fwprintf( stderr, L"%ls\n", error.c_str() );
        assert( song );
        std::printf( "Loaded %lld notes, keys %d-%d\n", song->info.llNoteCount, song->info.iMinNote, song->info.iMaxNote );
        Renderer renderer;
        assert( SUCCEEDED( renderer.InitOffscreen( 1280, 720 ) ) );
        if ( GetProcAddress( GetModuleHandleW( L"ntdll.dll" ), "wine_get_version" ) ) assert( !renderer.UsingD3DXFonts() );
        ViewInputs view;
        view.iBufferWidth = 1280;
        view.iBufferHeight = 720;
        view.eKeysShown = VisualSettings::All256;
        view.llStartTime = song->info.llFirstNote;
        auto globals = Scene::ComputeGlobals( view );
        FastEngine engine( *song, 2 );
        engine.SetLod( true, true );
        engine.AdvanceEnd( view.llStartTime + view.llTimeSpan );
        engine.Process( view.llStartTime );
        std::vector< TrackSettings > tracks( song->vTracks.size() );
        assert( SUCCEEDED( renderer.BeginScene() ) );
        engine.DrawNotes( &renderer, globals, tracks );
        int slots[256];
        engine.GetKeySlots( slots );
        const ChannelSettings *keyColors[256] = {};
        for ( int key = 0; key < 256; key++ )
            if ( slots[key] >= 0 ) keyColors[key] = &tracks[slots[key] >> 4].aChannels[slots[key] & 15];
        SceneColors colors;
        Scene::DrawKeys( &renderer, globals, colors, keyColors );
        assert( SUCCEEDED( renderer.BeginText() ) );
        RECT rect = { 10, 10, 1000, 80 };
        assert( SUCCEEDED( renderer.DrawText( L"Extended MIDI playback", Renderer::Small, &rect, 0, 0xFFFFFFFF ) ) );
        assert( SUCCEEDED( renderer.EndText() ) );
        assert( SUCCEEDED( renderer.EndScene() ) );
        std::vector< DWORD > pixels;
        assert( renderer.ReadBack( pixels ) && pixels.size() == 1280 * 720 );
        std::puts( "Supplied MIDI loading and rendering checks passed" );

        return 0;
    }

    {
        auto file = Midi( false );
        LoadProgress progress;
        progress.bTuning31 = progress.bTuning31PitchBend = true;
        LoadStats stats;
        std::wstring error;
        auto song = Loader::LoadMemory( file.data(), file.size(), 2, progress, stats, error );
        assert( song && song->bTuning31PitchBend );
        const std::vector< uint32_t > history = { 0x000065B0, 0x000064B0, 0x003006B0, 0x007F65B0, 0x007F64B0, 0x006000E0 };
        song->vChase.clear();
        for ( uint32_t message : history ) song->vChase.push_back( { 100, message } );
        song->vChase.push_back( { 2000, 0x000006B0 } );
        PlaybackDecoder decoder( *song );
        decoder.SetBendChase( true );        // as the engine sets it for the pitch-bend output
        std::vector< uint32_t > chase;
        decoder.Seek( 1000, 0, chase );
        assert( chase.size() >= history.size() );
        assert( std::equal( history.begin(), history.end(), chase.end() - history.size() ) );
    }
    PerformanceSettings performance;
    performance.LoadDefaultValues();
    assert( !performance.bTuning31PitchBend );
    performance.bTuning31PitchBend = true;
    IniFile performanceIni;
    performance.Save( performanceIni );
    PerformanceSettings restoredPerformance;
    restoredPerformance.LoadDefaultValues();
    restoredPerformance.Load( performanceIni );
    assert( restoredPerformance.bTuning31PitchBend );
    VisualSettings settings;
    settings.LoadDefaultValues();
    settings.eKeysShown = VisualSettings::All256;
    settings.iLastKey = 255;
    IniFile ini;
    settings.Save( ini );
    VisualSettings restored;
    restored.LoadDefaultValues();
    restored.Load( ini );
    assert( restored.eKeysShown == VisualSettings::All256 && restored.iLastKey == 255 );
    assert( std::wcscmp( Notes::NoteName( 255 ), L"D#20" ) == 0 );
    ViewInputs view;
    view.eKeysShown = VisualSettings::All256;
    view.iBufferWidth = 1920;
    view.iBufferHeight = 1080;
    auto globals = Scene::ComputeGlobals( view );
    assert( globals.iStartNote == 0 && globals.iEndNote == 255 );
    assert( globals.iAllWhiteKeys == 149 && std::isfinite( globals.fWhiteCX ) && globals.fWhiteCX > 0 );
    float previous = -1;
    for ( int key = 0; key < 256; key++ )
    {
        float x = Scene::GetNoteX( globals, key );
        assert( std::isfinite( x ) && x > previous );
        previous = x;
    }
    for ( bool extended : { false, true } )
    for ( bool tuning : { false, true } )
        for ( int fused : { 0, 1 } )
        {
            CheckSong( extended, true, false, fused, tuning );
            CheckSong( extended, false, false, fused, tuning );
            CheckSong( extended, false, true, fused, tuning );
        }
    std::puts( "Extended key regression checks passed" );
    return 0;
}
