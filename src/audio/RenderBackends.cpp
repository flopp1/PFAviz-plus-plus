#include "audio/RenderBackends.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

#include "audio/AudioOut.h"
#include "core/Util.h"
#include "resource.h"

namespace RenderBackends
{

const wchar_t *EngineName( int eEngine )
{
    switch ( eEngine )
    {
        case EngineKestrel: return L"Kestrel";
        case EngineMerlin: return L"Merlin";
        default: return L"BASSMIDI";
    }
}

namespace
{
    //-------------------------------------------------------------------------
    // Finding the DLLs: the folder chosen, next to the program, inside the program
    //-------------------------------------------------------------------------

    std::wstring ExeFolder()
    {
        wchar_t sExe[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW( nullptr, sExe, (DWORD)( sizeof( sExe ) / sizeof( sExe[0] ) ) );
        std::wstring s( sExe, n );
        size_t i = s.find_last_of( L"\\/" );
        return i == std::wstring::npos ? std::wstring( L"." ) : s.substr( 0, i );
    }

    std::wstring Join( const std::wstring &sDir, const wchar_t *sFile )
    {
        std::wstring s = sDir;
        if ( !s.empty() && s.back() != L'\\' && s.back() != L'/' ) s += L'\\';
        return s + sFile;
    }

    bool Exists( const std::wstring &s ) { return GetFileAttributesW( s.c_str() ) != INVALID_FILE_ATTRIBUTES; }

    struct Embedded { const wchar_t *sFile; int iRes; };
    const Embedded g_aEmbedded[] = { { L"bass.dll", IDR_ENGINE_BASS }, { L"bassmidi.dll", IDR_ENGINE_BASSMIDI }, { L"bassflac.dll", IDR_ENGINE_BASSFLAC },
                                     { L"kestrel_bridge.dll", IDR_ENGINE_KESTREL } };

    bool EmbeddedData( const wchar_t *sFile, const uint8_t *&p, DWORD &n )
    {
        for ( const Embedded &e : g_aEmbedded )
        {
            if ( _wcsicmp( e.sFile, sFile ) ) continue;
            HRSRC hRes = FindResourceW( nullptr, MAKEINTRESOURCEW( e.iRes ), RT_RCDATA );
            HGLOBAL h = hRes ? LoadResource( nullptr, hRes ) : nullptr;
            p = h ? (const uint8_t *)LockResource( h ) : nullptr;
            n = hRes ? SizeofResource( nullptr, hRes ) : 0;
            return p && n;
        }
        return false;
    }

    // The embedded DLLs are written out once, to a folder named after this build's copies (so a newer
    // build's never meets an older one's): %LOCALAPPDATA%\PFAviz++\engines\<hash>
    std::wstring EmbeddedFolder()
    {
        uint64_t h = 1469598103934665603ull;
        for ( const Embedded &e : g_aEmbedded )
        {
            const uint8_t *p;
            DWORD n;
            if ( !EmbeddedData( e.sFile, p, n ) ) continue;
            for ( DWORD i = 0; i < n; i += 64 ) h = ( h ^ p[i] ) * 1099511628211ull;
            h = ( h ^ n ) * 1099511628211ull;
        }
        wchar_t sBase[MAX_PATH];
        if ( FAILED( SHGetFolderPathW( nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, sBase ) ) ) return std::wstring();
        wchar_t sHash[32];
        swprintf( sHash, 32, L"%016llx", (unsigned long long)h );
        return std::wstring( sBase ) + L"\\PFAviz++\\engines\\" + sHash;
    }

    std::wstring WriteEmbedded( const wchar_t *sFile )
    {
        const uint8_t *p;
        DWORD n;
        if ( !EmbeddedData( sFile, p, n ) ) return std::wstring();
        std::wstring sDir = EmbeddedFolder();
        if ( sDir.empty() ) return std::wstring();
        SHCreateDirectoryExW( nullptr, sDir.c_str(), nullptr );
        std::wstring s = Join( sDir, sFile );
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if ( GetFileAttributesExW( s.c_str(), GetFileExInfoStandard, &fa ) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow == n ) return s;
        // (written beside, then moved into place: a half-written DLL is never there)
        std::wstring sTmp = s + L".part";
        HANDLE hf = CreateFileW( sTmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( hf == INVALID_HANDLE_VALUE ) return std::wstring();
        DWORD w = 0;
        BOOL bOk = WriteFile( hf, p, n, &w, nullptr ) && w == n;
        CloseHandle( hf );
        if ( !bOk || !MoveFileExW( sTmp.c_str(), s.c_str(), MOVEFILE_REPLACE_EXISTING ) ) { DeleteFileW( sTmp.c_str() ); return std::wstring(); }
        return s;
    }

    // Where a DLL comes from (built in first): L"" when nowhere
    std::wstring Locate( const wchar_t *sFile, bool bWrite, std::wstring *pFrom = nullptr )
    {
        const uint8_t *p;
        DWORD n;
        if ( EmbeddedData( sFile, p, n ) )
        {
            if ( pFrom ) *pFrom = L"built in";
            return bWrite ? WriteEmbedded( sFile ) : std::wstring( L"(built in)" );
        }
        if ( Exists( Join( ExeFolder(), sFile ) ) ) { if ( pFrom ) *pFrom = L"next to PFAviz++.exe"; return Join( ExeFolder(), sFile ); }
        return std::wstring();
    }

    // Its own dependencies are looked for beside it (bassmidi.dll needs bass.dll, which is loaded first
    // and so found whichever folder it came from)
    HMODULE LoadDll( const wchar_t *sFile )
    {
        std::wstring s = Locate( sFile, true );
        return s.empty() ? nullptr : LoadLibraryExW( s.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
    }

    std::wstring Utf8( const char *s ) { return s ? Util::Utf8ToWide( s ) : std::wstring(); }

    //-------------------------------------------------------------------------
    // BASSMIDI (bass.h / bassmidi.h, declared here: only what is used)
    //-------------------------------------------------------------------------

    typedef DWORD HSTREAM_, HSOUNDFONT_;
    struct BassMidiFont { HSOUNDFONT_ font; int preset; int bank; };
    const DWORD BASS_SAMPLE_FLOAT = 0x100, BASS_STREAM_DECODE = 0x200000, BASS_UNICODE = 0x80000000u;
    const DWORD BASS_DATA_FLOAT = 0x40000000, BASS_MIDI_DECAYEND = 0x1000, BASS_MIDI_NOFX = 0x2000;
    const DWORD BASS_ATTRIB_MIDI_CPU = 0x12001, BASS_ATTRIB_MIDI_VOICES = 0x12003;
    const DWORD BASS_CONFIG_MIDI_COMPACT = 0x10400;
    const int BASS_ERROR_ALREADY = 14;

    struct BassApi
    {
        HMODULE hBass = nullptr, hMidi = nullptr;
        BOOL ( WINAPI *Init )( int, DWORD, DWORD, HWND, const void * ) = nullptr;
        int ( WINAPI *ErrorGetCode )() = nullptr;
        BOOL ( WINAPI *SetConfig )( DWORD, DWORD ) = nullptr;
        DWORD ( WINAPI *ChannelGetData )( DWORD, void *, DWORD ) = nullptr;
        BOOL ( WINAPI *ChannelSetAttribute )( DWORD, DWORD, float ) = nullptr;
        BOOL ( WINAPI *StreamFree )( HSTREAM_ ) = nullptr;
        HSTREAM_ ( WINAPI *MidiStreamCreateFile )( BOOL, const void *, uint64_t, uint64_t, DWORD, DWORD ) = nullptr;
        HSOUNDFONT_ ( WINAPI *MidiFontInit )( const void *, DWORD ) = nullptr;
        BOOL ( WINAPI *MidiFontFree )( HSOUNDFONT_ ) = nullptr;
        BOOL ( WINAPI *MidiStreamSetFonts )( HSTREAM_, const void *, DWORD ) = nullptr;
        BOOL ( WINAPI *MidiStreamLoadSamples )( HSTREAM_ ) = nullptr;
        DWORD ( WINAPI *PluginLoad )( const void *, DWORD ) = nullptr;
        bool bInit = false, bFlac = false;

        template < class F > bool Get( HMODULE h, F &f, const char *sName )
        {
            f = reinterpret_cast< F >( reinterpret_cast< void * >( GetProcAddress( h, sName ) ) );
            return f != nullptr;
        }
        bool Load( std::wstring &sError )
        {
            if ( hBass && hMidi ) return true;
            if ( !hBass ) hBass = LoadDll( L"bass.dll" );
            if ( !hBass ) { sError = L"bass.dll was not found"; return false; }
            if ( !hMidi ) hMidi = LoadDll( L"bassmidi.dll" );
            if ( !hMidi ) { sError = L"bassmidi.dll was not found"; return false; }
            bool b = Get( hBass, Init, "BASS_Init" ) && Get( hBass, ErrorGetCode, "BASS_ErrorGetCode" ) && Get( hBass, SetConfig, "BASS_SetConfig" ) &&
                     Get( hBass, ChannelGetData, "BASS_ChannelGetData" ) && Get( hBass, ChannelSetAttribute, "BASS_ChannelSetAttribute" ) &&
                     Get( hBass, StreamFree, "BASS_StreamFree" ) && Get( hMidi, MidiStreamCreateFile, "BASS_MIDI_StreamCreateFile" ) &&
                     Get( hMidi, MidiFontInit, "BASS_MIDI_FontInit" ) && Get( hMidi, MidiFontFree, "BASS_MIDI_FontFree" ) &&
                     Get( hMidi, MidiStreamSetFonts, "BASS_MIDI_StreamSetFonts" ) && Get( hMidi, MidiStreamLoadSamples, "BASS_MIDI_StreamLoadSamples" );
            if ( !b ) { sError = L"bass.dll or bassmidi.dll is too old (a function is missing)"; return false; }
            Get( hBass, PluginLoad, "BASS_PluginLoad" );
            return true;
        }
        // "No sound" device: decoding only
        bool Start( unsigned uRate, std::wstring &sError )
        {
            if ( bInit ) return true;
            if ( !Init( 0, uRate, 0, nullptr, nullptr ) && ErrorGetCode() != BASS_ERROR_ALREADY )
            {
                sError = L"BASS could not start (error " + std::to_wstring( ErrorGetCode() ) + L")";
                return false;
            }
            // Samples stay loaded between pieces (each piece is a stream of its own)
            SetConfig( BASS_CONFIG_MIDI_COMPACT, 0 );
            // An SFZ's FLAC samples: BASS's own FLAC plugin (without it, BASS asks Windows' codecs, which are
            // slower and missing on some editions)
            const std::wstring sFlac = Locate( L"bassflac.dll", true );
            bFlac = PluginLoad && !sFlac.empty() && PluginLoad( sFlac.c_str(), BASS_UNICODE ) != 0;
            bInit = true;
            return true;
        }
    };
    BassApi g_Bass;

    std::wstring BassError( int iCode )
    {
        switch ( iCode )
        {
            case 2: return L"the file could not be opened";
            case 3: return L"out of memory";
            case 41: return L"the file's format is not supported";
            case 44: return L"the file uses a codec that is not available";
            default: return L"BASS error " + std::to_wstring( iCode );
        }
    }

    int Cores() { return std::max( 1, (int)std::thread::hardware_concurrency() ); }

    class BassBank : public Bank
    {
    public:
        explicit BassBank( unsigned uRate ) : m_uRate( uRate ) {}
        ~BassBank()
        {
            for ( HSOUNDFONT_ f : m_vFonts ) g_Bass.MidiFontFree( f );
        }
        bool Open( const BankKey &key, std::wstring &sError )
        {
            for ( const std::wstring &s : key.vFonts )
            {
                HSOUNDFONT_ f = g_Bass.MidiFontInit( s.c_str(), BASS_UNICODE );
                if ( !f ) { sError = L"BASSMIDI could not open " + s + L": " + BassError( g_Bass.ErrorGetCode() ); return false; }
                m_vFonts.push_back( f );
            }
            return !m_vFonts.empty();
        }
        int Engine() const override { return EngineBass; }
        // All but two cores busy at most: pieces x the threads each renders its parts on (as many as it has
        // parts with notes, up to those cores)
        int Parallel( int iParts ) const override { return std::clamp( ( Cores() - 2 ) / std::clamp( iParts, 1, std::max( 1, Cores() - 2 ) ), 1, 8 ); }
        int Parts( const PieceOpts &o ) const override
        {
            const int iGroups = std::clamp( o.iBassThreads, 1, 64 );
            return o.bBassByChannel ? 16 * std::min( iGroups, 16 ) : iGroups;
        }
        bool Render( const std::vector< std::vector< uint8_t > > &vParts, uint64_t llSkip, uint64_t llFrames, const PieceOpts &o,
                     const std::atomic< bool > *pCancel, std::vector< float > &vOut, std::wstring &sError ) override
        {
            // The parts with notes (an empty one is silent), each an instance of its own
            std::vector< size_t > vUsed;
            for ( size_t i = 0; i < vParts.size(); i++ )
                if ( !vParts[i].empty() ) vUsed.push_back( i );
            const size_t K = vUsed.size();
            if ( K == 0 ) { vOut.assign( (size_t)llFrames * 2, 0.0f ); return true; }
            if ( K == 1 ) return RenderOne( vParts[vUsed[0]], llSkip, llFrames, o, pCancel, vOut, sError );
            // ... on a few threads, taking the next part each, summed
            std::vector< std::vector< float > > vAudio( K );
            std::vector< std::wstring > vError( K );
            std::vector< char > vOk( K, 0 );
            std::atomic< size_t > iNext{ 0 };
            auto Run = [&]() {
                for ( size_t i; ( i = iNext.fetch_add( 1 ) ) < K; )
                    vOk[i] = RenderOne( vParts[vUsed[i]], llSkip, llFrames, o, pCancel, vAudio[i], vError[i] );
            };
            const size_t T = std::min( K, (size_t)std::max( 1, Cores() - 2 ) );
            std::vector< std::thread > vThreads;
            for ( size_t t = 1; t < T; t++ ) vThreads.emplace_back( Run );
            Run();
            for ( std::thread &t : vThreads ) t.join();
            for ( size_t i = 0; i < K; i++ )
                if ( !vOk[i] ) { sError = vError[i]; return false; }
            vOut = std::move( vAudio[0] );
            for ( size_t i = 1; i < K; i++ )
                for ( size_t j = 0; j < vOut.size(); j++ ) vOut[j] += vAudio[i][j];
            return true;
        }

    private:
        bool RenderOne( const std::vector< uint8_t > &vMidi, uint64_t llSkip, uint64_t llFrames, const PieceOpts &o,
                        const std::atomic< bool > *pCancel, std::vector< float > &vOut, std::wstring &sError )
        {
            DWORD dwFlags = BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | BASS_MIDI_DECAYEND | ( o.bFx ? 0 : BASS_MIDI_NOFX );
            HSTREAM_ h = g_Bass.MidiStreamCreateFile( TRUE, vMidi.data(), 0, vMidi.size(), dwFlags, m_uRate );
            if ( !h ) { sError = L"BASSMIDI could not read a piece: " + BassError( g_Bass.ErrorGetCode() ); return false; }
            // Later soundfonts replace earlier ones; BASSMIDI uses the first it is given first
            std::vector< BassMidiFont > vFonts;
            for ( size_t i = m_vFonts.size(); i-- > 0; ) vFonts.push_back( { m_vFonts[i], -1, 0 } );
            g_Bass.MidiStreamSetFonts( h, vFonts.data(), (DWORD)vFonts.size() );
            g_Bass.ChannelSetAttribute( h, BASS_ATTRIB_MIDI_VOICES, (float)std::clamp( o.uVoices, 1u, 100000u ) );
            g_Bass.ChannelSetAttribute( h, BASS_ATTRIB_MIDI_CPU, 0.0f );
            g_Bass.MidiStreamLoadSamples( h );

            vOut.assign( (size_t)llFrames * 2, 0.0f );
            std::vector< float > vSkip;
            uint64_t llDone = 0, llTotal = llSkip + llFrames;
            const uint64_t Chunk = 4096;
            bool bOk = true;
            while ( llDone < llTotal )
            {
                if ( pCancel && pCancel->load( std::memory_order_relaxed ) ) { bOk = false; break; }
                uint64_t n = std::min( Chunk, llTotal - llDone );
                float *p;
                if ( llDone < llSkip )
                {
                    n = std::min( n, llSkip - llDone );
                    vSkip.resize( (size_t)n * 2 );
                    p = vSkip.data();
                }
                else p = vOut.data() + ( llDone - llSkip ) * 2;
                DWORD got = g_Bass.ChannelGetData( h, p, (DWORD)( n * 8 ) | BASS_DATA_FLOAT );
                if ( got == (DWORD)-1 || got == 0 ) break;     // (the sound has died away: silence from here)
                llDone += got / 8;
            }
            g_Bass.StreamFree( h );
            return bOk;
        }

        unsigned m_uRate;
        std::vector< HSOUNDFONT_ > m_vFonts;
    };

    //-------------------------------------------------------------------------
    // Kestrel and Merlin (kestrel_bridge.dll's C ABI)
    //-------------------------------------------------------------------------

    struct KbOptions
    {
        uint32_t size = sizeof( KbOptions );
        uint32_t sample_rate = 48000, block_frames = 4096, ahead_blocks = 6, max_voices = 1u << 20, lower_by_itself = 0;
        uint32_t limiter = 0;               // off: PFAviz++'s own limiter runs over the joined pieces
        float limiter_ceiling_db = 0.0f;
        uint32_t dc_blocker = 1;
        float dc_blocker_hz = 0.0f;
        uint32_t interpolation = 1, steal_rule = 2;
        float release_floor_db = -100.0f;
        uint32_t min_velocity = 0, admit_rule = 0, max_steal_percent = 25, max_layers = 16, filter = 1, lfo = 1, mod_env = 1;
        double clock_seed = 0.0;
        // (PFAviz++'s bridge) Kestrel's performance settings: 0 = its own; switches 1 off, 2 on
        uint32_t gate_frames = 0, workgroup_size = 0, max_render_workgroups = 0, submit_voices = 0, materialise_threads = 0;
        uint32_t sort_voices = 0, kahan_reduce = 0, unchecked_shaders = 0;
    };
    static_assert( sizeof( KbOptions ) == 120, "KbOptions must match kestrel_bridge's" );
    const uint32_t KB_RUNNING = 0, KB_DONE = 1, KB_FAILED = 2;

    struct KestrelApi
    {
        HMODULE h = nullptr;
        const char *( *Error )( const void * ) = nullptr;
        void *( *FontLoadWith )( const char *, uint32_t, uint32_t ) = nullptr;
        void ( *FontFree )( void * ) = nullptr;
        void *( *LoadSet )( const void *const *, uint32_t ) = nullptr;
        void ( *Unload )( void * ) = nullptr;
        void *( *PieceStartBytes )( const void *, const uint8_t *, uint64_t, const KbOptions *, uint32_t, uint64_t, uint64_t ) = nullptr;
        uint32_t ( *PieceWait )( const void *, uint32_t ) = nullptr;
        void ( *PieceCancel )( const void * ) = nullptr;
        const float *( *PieceAudio )( const void *, uint64_t * ) = nullptr;
        const char *( *PieceError )( const void * ) = nullptr;
        void ( *PieceFree )( void * ) = nullptr;
        // (how far a piece is; older bridges lack it)
        struct PieceStatusOut { uint32_t size, state; uint64_t frames; float progress; uint32_t wall_ms; };
        void ( *PieceStatus )( const void *, PieceStatusOut * ) = nullptr;
        const char *( *TakeWarnings )() = nullptr;     // (PFAviz++'s bridge; older ones lack it)
        // (PFAviz++'s bridge: the continuous render; older ones lack it)
        void *( *StreamOpen )( const void *, const KbOptions * ) = nullptr;
        void ( *StreamPush )( void *, const uint64_t *, const uint32_t *, const float *, uint32_t ) = nullptr;
        void ( *StreamSysEx )( void *, uint64_t, const uint8_t *, uint32_t ) = nullptr;
        uint32_t ( *StreamRender )( void *, float *, uint32_t ) = nullptr;
        void ( *StreamSetCap )( void *, uint32_t ) = nullptr;
        void ( *StreamFree )( void * ) = nullptr;

        template < class F > bool Get( F &f, const char *sName )
        {
            f = reinterpret_cast< F >( reinterpret_cast< void * >( GetProcAddress( h, sName ) ) );
            return f != nullptr;
        }
        bool Load( std::wstring &sError )
        {
            if ( h ) return true;
            h = LoadDll( L"kestrel_bridge.dll" );
            if ( !h ) { sError = L"kestrel_bridge.dll was not found"; return false; }
            bool b = Get( Error, "kb_error" ) && Get( FontLoadWith, "kb_font_load_with" ) && Get( FontFree, "kb_font_free" ) && Get( LoadSet, "kb_load" ) &&
                     Get( Unload, "kb_unload" ) && Get( PieceStartBytes, "kb_piece_start_bytes" ) && Get( PieceWait, "kb_piece_wait" ) &&
                     Get( PieceCancel, "kb_piece_cancel" ) && Get( PieceAudio, "kb_piece_audio" ) && Get( PieceError, "kb_piece_error" ) &&
                     Get( PieceFree, "kb_piece_free" );
            if ( !b ) { sError = L"kestrel_bridge.dll is too old (a function is missing)"; h = nullptr; return false; }
            Get( TakeWarnings, "kb_take_warnings" );
            Get( PieceStatus, "kb_piece_status" );
            if ( !( Get( StreamOpen, "kb_stream_open" ) && Get( StreamPush, "kb_stream_push" ) && Get( StreamSysEx, "kb_stream_sysex" ) &&
                    Get( StreamRender, "kb_stream_render" ) && Get( StreamSetCap, "kb_stream_set_cap" ) && Get( StreamFree, "kb_stream_free" ) ) )
                StreamOpen = nullptr;
            return true;
        }
        std::wstring LastError() { const char *s = Error( nullptr ); return s && *s ? Utf8( s ) : std::wstring( L"unknown error" ); }
        std::wstring Warnings() { const char *s = TakeWarnings ? TakeWarnings() : nullptr; return s ? Utf8( s ) : std::wstring(); }
    };
    KestrelApi g_Kb;
    std::mutex g_mxKb;      // the bridge's error text is one for the process

    // Soundfont files read for Kestrel, kept while they are in the list: (path, rate, filters) -> the bridge's
    struct FontKey { std::wstring sPath; unsigned uRate; bool bFilter; bool operator<( const FontKey &o ) const
        { return std::tie( sPath, uRate, bFilter ) < std::tie( o.sPath, o.uRate, o.bFilter ); } };
    std::map< FontKey, void * > g_mFonts;
    // Frees the read soundfonts no bank uses (all of them when vKeep is empty)
    void FreeFonts( const std::vector< FontKey > &vKeep )
    {
        for ( auto it = g_mFonts.begin(); it != g_mFonts.end(); )
        {
            bool bKeep = false;
            for ( const FontKey &k : vKeep ) bKeep |= !( k < it->first ) && !( it->first < k );
            if ( bKeep ) ++it;
            else { g_Kb.FontFree( it->second ); it = g_mFonts.erase( it ); }
        }
    }

    // A tiny MIDI file: one note (middle C, velocity 100) for a tenth of a second, to see Merlin take the soundfonts
    std::vector< uint8_t > OneNote()
    {
        const uint8_t a[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 13,
                              0, 0x90, 60, 100, 0x30, 0x80, 60, 0, 0, 0xFF, 0x2F, 0, 0 };
        return std::vector< uint8_t >( a, a + 34 );
    }

    class KestrelBank : public Bank
    {
    public:
        explicit KestrelBank( unsigned uRate ) : m_uRate( uRate ) {}
        ~KestrelBank()
        {
            if ( m_pLoaded ) g_Kb.Unload( m_pLoaded );
        }
        // (the soundfonts read are kept in g_mFonts, under g_mxBank)
        bool Open( const BankKey &key, std::wstring &sError )
        {
            std::lock_guard< std::mutex > lock( g_mxKb );
            g_Kb.Warnings();    // (what came before is not this load's)
            std::vector< void * > vFonts;
            for ( const std::wstring &s : key.vFonts )
            {
                FontKey fk{ s, m_uRate, key.bFilter };
                auto it = g_mFonts.find( fk );
                if ( it == g_mFonts.end() )
                {
                    void *f = g_Kb.FontLoadWith( Util::WideToUtf8( s ).c_str(), m_uRate, key.bFilter ? 1 : 0 );
                    if ( !f ) { sError = L"Kestrel could not open " + s + L": " + g_Kb.LastError(); return false; }
                    it = g_mFonts.emplace( fk, f ).first;
                }
                vFonts.push_back( it->second );
            }
            if ( vFonts.empty() ) return false;
            m_nFonts = vFonts.size();
            m_pLoaded = g_Kb.LoadSet( vFonts.data(), (uint32_t)vFonts.size() );
            sWarnings = g_Kb.Warnings();
            if ( !m_pLoaded ) { sError = L"Kestrel could not load the soundfonts on the graphics card: " + g_Kb.LastError(); return false; }
            return true;
        }
        int Engine() const override { return EngineKestrel; }
        int Parallel( int ) const override { return 1; }
        // (Merlin refuses several soundfonts, and Kestrel plays a marked note once)
        bool RepeatMarks( const PieceOpts &o ) override
        {
            std::wstring sWhy;
            return o.eEngine == EngineMerlin && !o.bForceKestrel && m_nFonts == 1 && MerlinTakes( sWhy );
        }
        bool MerlinTakes( std::wstring &sWhy ) override
        {
            std::lock_guard< std::mutex > lock( m_mxProbe );
            if ( m_iMerlin < 0 )
            {
                if ( m_nFonts > 1 ) { m_iMerlin = 0; m_sMerlinWhy = L"Merlin takes one soundfont, not several"; }
                else
                {
                    std::vector< float > v;
                    PieceOpts o;
                    m_iMerlin = RenderWith( 1, OneNote(), 0, m_uRate / 10, o, nullptr, v, m_sMerlinWhy ) ? 1 : 0;
                }
            }
            sWhy = m_sMerlinWhy;
            return m_iMerlin == 1;
        }
        bool Render( const std::vector< std::vector< uint8_t > > &vParts, uint64_t llSkip, uint64_t llFrames, const PieceOpts &o,
                     const std::atomic< bool > *pCancel, std::vector< float > &vOut, std::wstring &sError ) override
        {
            // Merlin first when chosen and it takes the soundfonts; else (or what it refuses) Kestrel
            std::wstring sWhy;
            if ( o.eEngine == EngineMerlin && !o.bForceKestrel && MerlinTakes( sWhy ) )
            {
                std::wstring sMerlin;
                if ( RenderWith( 1, vParts[0], llSkip, llFrames, o, pCancel, vOut, sMerlin ) ) return true;
                if ( pCancel && pCancel->load() ) return false;
                // (the caller makes it again for Kestrel, without repeat marks, which it plays once)
                sError = sMerlin;
                return false;
            }
            return RenderWith( 0, vParts[0], llSkip, llFrames, o, pCancel, vOut, sError );
        }

    private:
        KbOptions MakeOpts( const PieceOpts &o ) const
        {
            const KestrelOpts &k = o.kb;
            KbOptions opts;
            opts.sample_rate = m_uRate;
            opts.max_voices = std::max( 1u, o.uVoices );
            opts.block_frames = (uint32_t)std::clamp( k.iBlockFrames, 64, 16384 );
            opts.interpolation = (uint32_t)std::clamp( k.iInterpolation, 0, 2 );
            opts.steal_rule = (uint32_t)std::clamp( k.iStealRule, 0, 2 );
            opts.admit_rule = (uint32_t)std::clamp( k.iAdmitRule, 0, 1 );
            opts.max_steal_percent = (uint32_t)std::clamp( k.iMaxStealPercent, 1, 100 );
            opts.release_floor_db = (float)std::clamp( k.iReleaseFloorDb, -160, -20 );
            opts.max_layers = (uint32_t)std::clamp( k.iMaxLayers, 1, 64 );
            opts.filter = k.bFilter;
            opts.lfo = k.bLfo;
            opts.mod_env = k.bModEnv;
            opts.dc_blocker = k.bDcBlocker;
            opts.dc_blocker_hz = (float)std::clamp( k.iDcBlockerHz, 1, 200 );
            opts.gate_frames = (uint32_t)std::clamp( k.iGateFrames, 4, 1024 );
            if ( opts.block_frames % opts.gate_frames || opts.gate_frames % 4 ) opts.gate_frames = 32;   // (it must divide the block)
            opts.workgroup_size = (uint32_t)std::clamp( k.iWorkgroupSize, 32, 1024 );
            opts.max_render_workgroups = (uint32_t)std::clamp( k.iRenderWorkgroups, 1, 1 << 24 );
            opts.submit_voices = (uint32_t)std::max( 1024, k.iSubmitVoices );
            opts.materialise_threads = (uint32_t)std::clamp( k.iVoiceThreads, 1, 64 );
            opts.sort_voices = k.bSortVoices ? 2 : 1;
            opts.kahan_reduce = k.bKahan ? 2 : 1;
            opts.unchecked_shaders = k.bUnchecked ? 2 : 1;
            return opts;
        }

        bool RenderWith( uint32_t uEngine, const std::vector< uint8_t > &vMidi, uint64_t llSkip, uint64_t llFrames, const PieceOpts &o,
                         const std::atomic< bool > *pCancel, std::vector< float > &vOut, std::wstring &sError )
        {
            const KbOptions opts = MakeOpts( o );
            void *p;
            {
                std::lock_guard< std::mutex > lock( g_mxKb );
                p = g_Kb.PieceStartBytes( m_pLoaded, vMidi.data(), vMidi.size(), &opts, uEngine, llSkip, llFrames );
                if ( !p ) { sError = g_Kb.LastError(); return false; }
            }
            uint32_t uState;
            while ( ( uState = g_Kb.PieceWait( p, 20 ) ) == KB_RUNNING )
            {
                if ( pCancel && pCancel->load( std::memory_order_relaxed ) ) g_Kb.PieceCancel( p );
                if ( o.pProgress && g_Kb.PieceStatus )
                {
                    KestrelApi::PieceStatusOut st{ sizeof( KestrelApi::PieceStatusOut ) };
                    g_Kb.PieceStatus( p, &st );
                    o.pProgress->store( st.progress, std::memory_order_relaxed );
                }
            }
            if ( o.pProgress ) o.pProgress->store( 1.0f, std::memory_order_relaxed );
            bool bOk = false;
            if ( uState == KB_DONE )
            {
                uint64_t n = 0;
                const float *pAudio = g_Kb.PieceAudio( p, &n );
                vOut.assign( (size_t)llFrames * 2, 0.0f );
                if ( pAudio ) memcpy( vOut.data(), pAudio, (size_t)std::min( n, llFrames ) * 2 * sizeof( float ) );
                bOk = true;
            }
            else if ( uState == KB_FAILED )
            {
                std::lock_guard< std::mutex > lock( g_mxKb );
                const char *s = g_Kb.PieceError( p );
                sError = s ? Utf8( s ) : std::wstring( L"a piece failed" );
            }
            g_Kb.PieceFree( p );
            return bOk;
        }

        class KestrelStream : public Stream
        {
        public:
            explicit KestrelStream( void *p ) : m_p( p ) {}
            ~KestrelStream() override { g_Kb.StreamFree( m_p ); }
            void Push( const uint64_t *pFrames, const uint32_t *pMsgs, const float *pScales, size_t n ) override
            {
                for ( size_t i = 0; i < n; i += 1u << 20 )
                    g_Kb.StreamPush( m_p, pFrames + i, pMsgs + i, pScales ? pScales + i : nullptr, (uint32_t)std::min< size_t >( n - i, 1u << 20 ) );
            }
            void SysEx( uint64_t llFrame, const uint8_t *p, size_t n ) override { g_Kb.StreamSysEx( m_p, llFrame, p, (uint32_t)n ); }
            bool Render( float *pOut, unsigned uFrames, std::wstring &sError ) override
            {
                if ( g_Kb.StreamRender( m_p, pOut, uFrames ) ) return true;
                std::lock_guard< std::mutex > lock( g_mxKb );
                sError = g_Kb.LastError();
                return false;
            }
            void SetCap( unsigned uVoices ) override { g_Kb.StreamSetCap( m_p, std::max( 1u, uVoices ) ); }
        private:
            void *m_p;
        };

    public:
        std::unique_ptr< Stream > OpenStream( const PieceOpts &o, std::wstring &sError ) override
        {
            if ( !g_Kb.StreamOpen ) { sError = L"kestrel_bridge.dll has no continuous render"; return nullptr; }
            const KbOptions opts = MakeOpts( o );
            std::lock_guard< std::mutex > lock( g_mxKb );
            void *p = g_Kb.StreamOpen( m_pLoaded, &opts );
            if ( !p ) { sError = g_Kb.LastError(); return nullptr; }
            return std::make_unique< KestrelStream >( p );
        }

    private:
        unsigned m_uRate;
        size_t m_nFonts = 0;
        void *m_pLoaded = nullptr;
        std::mutex m_mxProbe;
        int m_iMerlin = -1;         // Merlin takes these soundfonts: -1 not tried yet
        std::wstring m_sMerlinWhy;
    };

    // The last bank of each kind: [0] BASSMIDI, [1] Kestrel and Merlin
    std::mutex g_mxBank;
    std::shared_ptr< Bank > g_apBank[2];
    BankKey g_aKey[2];
}

std::shared_ptr< Bank > Acquire( const BankKey &keyIn, std::wstring &sError )
{
    std::lock_guard< std::mutex > lock( g_mxBank );
    // (Kestrel and Merlin share a bank: which of them makes a piece is the piece's)
    BankKey key = keyIn;
    const int k = key.eEngine == EngineBass ? 0 : 1;
    if ( k ) key.eEngine = EngineKestrel;
    if ( g_apBank[k] && g_aKey[k] == key ) return g_apBank[k];
    g_apBank[k].reset();
    if ( key.vFonts.empty() ) { sError = L"no soundfont chosen (Preferences > Rendered audio)"; return nullptr; }
    for ( const std::wstring &s : key.vFonts )
        if ( !Exists( s ) ) { sError = L"the soundfont " + s + L" was not found"; return nullptr; }
    if ( !k )
    {
        if ( !g_Bass.Load( sError ) || !g_Bass.Start( key.uRate, sError ) ) return nullptr;
        auto p = std::make_shared< BassBank >( key.uRate );
        if ( !p->Open( key, sError ) ) return nullptr;
        g_apBank[0] = p;
    }
    else
    {
        if ( !g_Kb.Load( sError ) ) return nullptr;
        // (soundfonts read for the old set and not in the new one are freed)
        std::vector< FontKey > vKeep;
        for ( const std::wstring &s : key.vFonts ) vKeep.push_back( { s, key.uRate, key.bFilter } );
        FreeFonts( vKeep );
        auto p = std::make_shared< KestrelBank >( key.uRate );
        if ( !p->Open( key, sError ) ) return nullptr;
        g_apBank[1] = p;
    }
    g_aKey[k] = key;
    return g_apBank[k];
}

namespace
{
    std::mutex g_mxPreload;
    std::condition_variable g_cvPreload;
    // (its thread is detached as it starts, so no thread object is left to end with the program; it says
    // when it has stopped)
    BankKey g_PreloadKey;
    bool g_bPreload = false, g_bPreloadQuit = false, g_bPreloading = false, g_bPreloadStarted = false, g_bPreloadStopped = false;
    bool g_bLeftLoading = false;        // (CloseShared left a load under way, and the banks, alone)

    void PreloadThread()
    {
        CoInitializeEx( nullptr, COINIT_MULTITHREADED );
        SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
        std::unique_lock< std::mutex > lock( g_mxPreload );
        for ( ;; )
        {
            g_cvPreload.wait( lock, [] { return g_bPreload || g_bPreloadQuit; } );
            if ( g_bPreloadQuit ) break;
            BankKey key = g_PreloadKey;
            const bool bMerlin = key.eEngine == EngineMerlin;
            g_bPreload = false;
            g_bPreloading = true;
            lock.unlock();
            if ( !key.uRate ) key.uRate = AudioOut::DeviceRate();
            if ( !key.uRate ) key.uRate = 48000;
            std::wstring sError, sWhy;
            std::shared_ptr< Bank > p = Acquire( key, sError );
            if ( p && bMerlin ) p->MerlinTakes( sWhy );
            lock.lock();
            g_bPreloading = false;
        }
        g_bPreloadStopped = true;
        g_cvPreload.notify_all();
        lock.unlock();
        CoUninitialize();
    }
}

void Preload( const BankKey &key )
{
    std::lock_guard< std::mutex > lock( g_mxPreload );
    if ( g_bPreloadQuit ) return;
    g_PreloadKey = key;
    g_bPreload = true;
    if ( !g_bPreloadStarted )
    {
        g_bPreloadStarted = true;
        std::thread( PreloadThread ).detach();
    }
    g_cvPreload.notify_all();
}

bool Preloading()
{
    std::lock_guard< std::mutex > lock( g_mxPreload );
    return g_bPreloading || g_bLeftLoading;
}

void CloseShared()
{
    {
        std::unique_lock< std::mutex > lock( g_mxPreload );
        g_bPreloadQuit = true;
        g_cvPreload.notify_all();
        // A load under way is not waited for (a large soundfont takes seconds): the program ends the process
        // at once instead (Preloading), and the system frees all of it
        if ( g_bPreloading ) { g_bLeftLoading = true; return; }
        if ( g_bPreloadStarted ) g_cvPreload.wait_for( lock, std::chrono::seconds( 2 ), [] { return g_bPreloadStopped; } );
    }
    std::lock_guard< std::mutex > lock( g_mxBank );
    g_apBank[0].reset();
    g_apBank[1].reset();
    if ( g_Kb.h ) FreeFonts( {} );
}

namespace
{
    std::mutex g_mxNotes;
    std::wstring g_asNotes[NoteCount];
}

void SetNote( int eNote, const std::wstring &s )
{
    std::lock_guard< std::mutex > lock( g_mxNotes );
    if ( eNote >= 0 && eNote < NoteCount ) g_asNotes[eNote] = s;
}

std::wstring GetNote( int eNote )
{
    std::lock_guard< std::mutex > lock( g_mxNotes );
    return eNote >= 0 && eNote < NoteCount ? g_asNotes[eNote] : std::wstring();
}

std::wstring DllStatus()
{
    std::wstring sBass, sMidi, sKb;
    const bool bBass = !Locate( L"bass.dll", false, &sBass ).empty() && !Locate( L"bassmidi.dll", false, &sMidi ).empty();
    const bool bKb = !Locate( L"kestrel_bridge.dll", false, &sKb ).empty();
    std::wstring sFlac;
    const bool bFlac = !Locate( L"bassflac.dll", false, &sFlac ).empty();
    std::wstring s = bBass ? L"BASSMIDI: " + ( sBass == sMidi ? sBass : L"bass.dll " + sBass + L", bassmidi.dll " + sMidi ) +
                                 ( bFlac ? L" (FLAC samples: bassflac.dll " + sFlac + L")." : L" (FLAC samples: Windows' codecs)." )
                           : std::wstring( L"BASSMIDI: bass.dll and bassmidi.dll not found." );
    s += bKb ? L"   Kestrel and Merlin: " + sKb + L"." : std::wstring( L"   Kestrel and Merlin: kestrel_bridge.dll not found." );
    return s;
}

}
