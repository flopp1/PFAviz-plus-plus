#include "audio/RenderBackends.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>
#include <array>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <tuple>

#include "audio/AudioOut.h"
#include "core/ThreadPool.h"
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
    // A soundfont's preset (spreset, sbank; -1: all) given as another (dpreset, dbank; -1: the same)
    struct BassMidiFontEx { HSOUNDFONT_ font; int spreset; int sbank; int dpreset; int dbank; int dbanklsb; };
    struct BassMidiFontInfo { const char *name, *copyright, *comment; DWORD presets, samsize, samload, samtype; };
    const DWORD BASS_MIDI_FONT_EX = 0x1000000;
    const DWORD BASS_SAMPLE_FLOAT = 0x100, BASS_STREAM_DECODE = 0x200000, BASS_UNICODE = 0x80000000u;
    const DWORD BASS_DATA_FLOAT = 0x40000000, BASS_MIDI_DECAYEND = 0x1000, BASS_MIDI_NOFX = 0x2000;
    const DWORD BASS_ATTRIB_MIDI_CPU = 0x12001, BASS_ATTRIB_MIDI_VOICES = 0x12003, BASS_ATTRIB_MIDI_VOICES_ACTIVE = 0x12004;
    const DWORD BASS_MIDI_EVENTS_RAW = 0x10000;
    const DWORD BASS_CONFIG_MIDI_COMPACT = 0x10400, BASS_CONFIG_MIDI_SAMPLETHREADS = 0x10406;
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
        // (optional: the continuous render's)
        HSTREAM_ ( WINAPI *MidiStreamCreate )( DWORD, DWORD, DWORD ) = nullptr;
        DWORD ( WINAPI *MidiStreamEvents )( HSTREAM_, DWORD, const void *, DWORD ) = nullptr;
        BOOL ( WINAPI *ChannelGetAttribute )( DWORD, DWORD, float * ) = nullptr;
        BOOL ( WINAPI *MidiFontGetInfo )( HSOUNDFONT_, BassMidiFontInfo * ) = nullptr;     // (these two optional)
        BOOL ( WINAPI *MidiFontGetPresets )( HSOUNDFONT_, DWORD * ) = nullptr;
        BOOL ( WINAPI *MidiFontLoad )( HSOUNDFONT_, int, int ) = nullptr;                  // (optional)
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
            Get( hMidi, MidiFontGetInfo, "BASS_MIDI_FontGetInfo" );
            Get( hMidi, MidiStreamCreate, "BASS_MIDI_StreamCreate" );
            Get( hMidi, MidiStreamEvents, "BASS_MIDI_StreamEvents" );
            Get( hBass, ChannelGetAttribute, "BASS_ChannelGetAttribute" );
            Get( hMidi, MidiFontGetPresets, "BASS_MIDI_FontGetPresets" );
            Get( hMidi, MidiFontLoad, "BASS_MIDI_FontLoad" );
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
            // ... and are loaded by the instance that needs them, as it renders, not by BASSMIDI's background
            // loading threads (where a crash as the soundfonts changed was, BassBank::Open has more; they crashed
            // more often in a test of it than without). A render ahead loses nothing by it: the instance waits
            // for its samples instead of playing on without them, and the audio was the same. Developer switch
            // PFAVIZ_SAMPLETHREADS=n: n loading threads, to compare
            const char *sThreads = getenv( "PFAVIZ_SAMPLETHREADS" );
            SetConfig( BASS_CONFIG_MIDI_SAMPLETHREADS, sThreads ? (DWORD)std::clamp( atoi( sThreads ), 0, 64 ) : 0 );
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
            case 0: return L"BASS error 0 (no reason given)";
            default: return L"BASS error " + std::to_wstring( iCode );
        }
    }

    //-------------------------------------------------------------------------
    // An SFZ BASSMIDI cannot open for an #include it cannot find (BASS error 7000): one whose include paths
    // are made of #define variables ("#include "$DIR/$DYN.txt""), which BASSMIDI does not expand there. It
    // is read here instead, its includes put in place and its variables expanded, into one file of its own
    // (in %TEMP%, deleted once BASSMIDI has read it), its sample paths made absolute (default_path, from the
    // SFZ's folder, as BASSMIDI reads them) so its samples are found from there
    //-------------------------------------------------------------------------

    const int BASS_ERROR_MIDI_INCLUDE = 7000;

    bool ReadBytes( const std::wstring &s, std::string &v )
    {
        HANDLE h = CreateFileW( s.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr );
        if ( h == INVALID_HANDLE_VALUE ) return false;
        LARGE_INTEGER n{};
        bool b = GetFileSizeEx( h, &n ) && n.QuadPart < ( 64ll << 20 );
        DWORD r = 0;
        if ( b ) v.resize( (size_t)n.QuadPart );
        if ( b && !v.empty() ) b = ReadFile( h, v.data(), (DWORD)v.size(), &r, nullptr ) && r == v.size();
        CloseHandle( h );
        return b;
    }

    class SfzFlattener
    {
    public:
        // The flattened SFZ's text, or false (sError why)
        bool Flatten( const std::wstring &sFile, std::string &sOut, std::wstring &sError )
        {
            const size_t i = sFile.find_last_of( L"\\/" );
            m_sDir = i == std::wstring::npos ? std::wstring( L"." ) : sFile.substr( 0, i );
            if ( !Read( sFile, m_sDir, 0 ) ) { sError = m_sError; return false; }
            sOut = Absolute( m_sOut );
            return true;
        }

    private:
        static bool IsName( char c ) { return isalnum( (unsigned char)c ) || c == '_'; }
        static std::string Trim( const std::string &s )
        {
            size_t a = 0, b = s.size();
            while ( a < b && isspace( (unsigned char)s[a] ) ) a++;
            while ( b > a && isspace( (unsigned char)s[b - 1] ) ) b--;
            return s.substr( a, b - a );
        }
        // Each $variable replaced by its value (the longest name defined that matches)
        std::string Expand( const std::string &s ) const
        {
            if ( s.find( '$' ) == std::string::npos ) return s;
            std::string r;
            for ( size_t i = 0; i < s.size(); )
            {
                const std::pair< std::string, std::string > *pBest = nullptr;
                if ( s[i] == '$' )
                    for ( const auto &kv : m_vDefs )
                        if ( s.compare( i, kv.first.size(), kv.first ) == 0 && ( !pBest || kv.first.size() > pBest->first.size() ) ) pBest = &kv;
                if ( pBest ) { r += pBest->second; i += pBest->first.size(); }
                else r += s[i++];
            }
            return r;
        }
        // An include's file: UTF-8, else the ANSI code page; from the SFZ's folder, else the including file's
        std::wstring Find( const std::string &sPath, const std::wstring &sFrom ) const
        {
            std::vector< std::wstring > vNames{ Util::Utf8ToWide( sPath ) };
            if ( int n = MultiByteToWideChar( CP_ACP, 0, sPath.data(), (int)sPath.size(), nullptr, 0 ) )
            {
                std::wstring w( (size_t)n, L'\0' );
                MultiByteToWideChar( CP_ACP, 0, sPath.data(), (int)sPath.size(), w.data(), n );
                vNames.push_back( w );
            }
            for ( const std::wstring &w : vNames )
            {
                const bool bAbs = w.size() > 1 && ( w[1] == L':' || w[0] == L'\\' || w[0] == L'/' );
                for ( const std::wstring &sDir : { m_sDir, sFrom } )
                {
                    const std::wstring s = bAbs ? w : Join( sDir, w.c_str() );
                    if ( Exists( s ) ) return s;
                }
            }
            return std::wstring();
        }
        bool Read( const std::wstring &sFile, const std::wstring &sFrom, int iDepth )
        {
            std::string v;
            if ( iDepth > 32 ) { m_sError = L"its #includes go too deep (" + sFile + L")"; return false; }
            if ( !ReadBytes( sFile, v ) ) { m_sError = L"it could not read " + sFile; return false; }
            const size_t i = sFile.find_last_of( L"\\/" );
            const std::wstring sHere = i == std::wstring::npos ? sFrom : sFile.substr( 0, i );
            for ( size_t a = 0; a < v.size(); )
            {
                size_t b = v.find( '\n', a );
                if ( b == std::string::npos ) b = v.size();
                std::string sLine = v.substr( a, b - a );
                a = b + 1;
                if ( !sLine.empty() && sLine.back() == '\r' ) sLine.pop_back();
                const std::string t = Trim( sLine );
                if ( t.compare( 0, 7, "#define" ) == 0 && t.size() > 7 && isspace( (unsigned char)t[7] ) )
                {
                    // #define $NAME value (to the line's end, a // comment left out)
                    std::string r = Trim( t.substr( 7 ) );
                    size_t n = 1;
                    while ( n < r.size() && IsName( r[n] ) ) n++;
                    if ( r.empty() || r[0] != '$' || n < 2 ) continue;
                    std::string sValue = r.substr( n );
                    const size_t c = sValue.find( "//" );
                    if ( c != std::string::npos ) sValue.resize( c );
                    sValue = Expand( Trim( sValue ) );
                    const std::string sName = r.substr( 0, n );
                    auto it = std::find_if( m_vDefs.begin(), m_vDefs.end(), [&]( const auto &kv ) { return kv.first == sName; } );
                    if ( it != m_vDefs.end() ) it->second = sValue;
                    else m_vDefs.push_back( { sName, sValue } );
                    continue;
                }
                if ( t.compare( 0, 8, "#include" ) == 0 )
                {
                    const size_t q0 = t.find( '"' ), q1 = q0 == std::string::npos ? q0 : t.find( '"', q0 + 1 );
                    if ( q1 == std::string::npos ) continue;
                    const std::string sPath = Expand( t.substr( q0 + 1, q1 - q0 - 1 ) );
                    const std::wstring s = Find( sPath, sHere );
                    if ( s.empty() ) { m_sError = L"its #include \"" + Util::Utf8ToWide( sPath ) + L"\" was not found"; return false; }
                    if ( !Read( s, sHere, iDepth + 1 ) ) return false;
                    continue;
                }
                m_sOut += Expand( sLine );
                m_sOut += '\n';
            }
            return true;
        }
        // The folder, as the text gives it: forward slashes, ending in one (its short name when it has
        // characters outside ASCII and one is to be had, else UTF-8)
        std::string Folder() const
        {
            std::wstring s = m_sDir;
            if ( std::any_of( s.begin(), s.end(), []( wchar_t c ) { return c > 127; } ) )
            {
                wchar_t sShort[MAX_PATH * 4];
                const DWORD n = GetShortPathNameW( s.c_str(), sShort, MAX_PATH * 4 );
                if ( n && n < MAX_PATH * 4 && std::none_of( sShort, sShort + n, []( wchar_t c ) { return c > 127; } ) ) s.assign( sShort, n );
            }
            std::replace( s.begin(), s.end(), L'\\', L'/' );
            if ( s.empty() || s.back() != L'/' ) s += L'/';
            return Util::WideToUtf8( s );
        }
        // Sample paths from the SFZ's folder: each relative default_path made absolute, and each <control>
        // header given the folder first (an SFZ without one, one at its start), as its own default_path would
        // otherwise be the flattened file's folder
        std::string Absolute( const std::string &s ) const
        {
            const std::string sDir = Folder();
            std::string r;
            size_t a = 0;
            static const std::string Key = "default_path=";
            for ( size_t i; ( i = s.find( Key, a ) ) != std::string::npos; )
            {
                const size_t v = i + Key.size();
                // (the value runs to the line's end, a comment, the next header or the next opcode)
                size_t e = v;
                while ( e < s.size() && s[e] != '\n' && s[e] != '<' && s.compare( e, 2, "//" ) != 0 )
                {
                    if ( isspace( (unsigned char)s[e] ) )
                    {
                        size_t k = e;
                        while ( k < s.size() && ( s[k] == ' ' || s[k] == '\t' ) ) k++;
                        size_t n = k;
                        while ( n < s.size() && ( IsName( s[n] ) || s[n] == '$' ) ) n++;
                        if ( n > k && n < s.size() && s[n] == '=' ) break;
                    }
                    e++;
                }
                const std::string sVal = Trim( s.substr( v, e - v ) );
                const bool bAbs = sVal.size() > 1 && ( sVal[1] == ':' || sVal[0] == '/' || sVal[0] == '\\' );
                r.append( s, a, v - a );
                r += bAbs ? sVal : sDir + sVal;
                a = e;
            }
            r.append( s, a, std::string::npos );
            static const std::string Control = "<control>";
            const std::string sSet = "\ndefault_path=" + sDir + "\n";
            std::string o;
            a = 0;
            for ( size_t i; ( i = r.find( Control, a ) ) != std::string::npos; a = i + Control.size() ) o.append( r, a, i + Control.size() - a ) += sSet;
            o.append( r, a, std::string::npos );
            return a == 0 ? Control + sSet + o : o;
        }

        std::wstring m_sDir, m_sError;
        std::vector< std::pair< std::string, std::string > > m_vDefs;
        std::string m_sOut;
    };

    // The soundfont, opened by BASSMIDI: an SFZ it cannot open for an #include it cannot find, flattened first
    // (developer switch PFAVIZ_SFZFLAT=1: every SFZ flattened, to compare)
    HSOUNDFONT_ OpenBassFont( const std::wstring &sFile, std::wstring &sError )
    {
        static const bool s_bFlatAll = getenv( "PFAVIZ_SFZFLAT" ) && atoi( getenv( "PFAVIZ_SFZFLAT" ) ) != 0;
        const bool bSfz = sFile.size() > 4 && _wcsicmp( sFile.c_str() + sFile.size() - 4, L".sfz" ) == 0;
        HSOUNDFONT_ f = s_bFlatAll && bSfz ? 0 : g_Bass.MidiFontInit( sFile.c_str(), BASS_UNICODE );
        if ( f ) return f;
        const int iCode = s_bFlatAll && bSfz ? BASS_ERROR_MIDI_INCLUDE : g_Bass.ErrorGetCode();
        sError = L"BASSMIDI could not open " + sFile + L": " + ( iCode == BASS_ERROR_MIDI_INCLUDE ? L"an SFZ #include it could not find" : BassError( iCode ) );
        if ( iCode != BASS_ERROR_MIDI_INCLUDE || !bSfz ) return 0;
        std::string sText;
        std::wstring sWhy;
        if ( !SfzFlattener().Flatten( sFile, sText, sWhy ) ) { sError += L" (and read here, " + sWhy + L")"; return 0; }
        static std::atomic< unsigned > s_uNext{ 0 };
        wchar_t sTemp[MAX_PATH];
        const DWORD n = GetTempPathW( MAX_PATH, sTemp );
        if ( !n || n >= MAX_PATH ) return 0;
        const std::wstring sDir = std::wstring( sTemp ) + L"PFAviz++";
        CreateDirectoryW( sDir.c_str(), nullptr );
        const std::wstring sFlat = sDir + L"\\sfz-" + std::to_wstring( GetCurrentProcessId() ) + L"-" + std::to_wstring( s_uNext++ ) + L".sfz";
        HANDLE h = CreateFileW( sFlat.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr );
        if ( h == INVALID_HANDLE_VALUE ) { sError += L" (and its flattened copy could not be written to " + sDir + L")"; return 0; }
        DWORD w = 0;
        const bool bOk = WriteFile( h, sText.data(), (DWORD)sText.size(), &w, nullptr ) && w == sText.size();
        CloseHandle( h );
        f = bOk ? g_Bass.MidiFontInit( sFlat.c_str(), BASS_UNICODE ) : 0;
        if ( !f && bOk ) sError += L" (nor flattened: " + BassError( g_Bass.ErrorGetCode() ) + L")";
        DeleteFileW( sFlat.c_str() );
        if ( f ) sError.clear();
        return f;
    }

    int Cores() { return std::max( 1, (int)std::thread::hardware_concurrency() ); }
}

bool OldSplit() { static const bool s_b = getenv( "PFAVIZ_OLDSPLIT" ) && atoi( getenv( "PFAVIZ_OLDSPLIT" ) ) != 0; return s_b; }

namespace
{
    // BASSMIDI's continuous render (Kiva's way: the synth kept from block to block, fed the song's events as
    // its audio is made). Each part (key group, or channel and key group: PartOf) is a BASSMIDI instance of
    // its own, made on a thread of its own and summed. An event goes in at its frame, or at most StepFrames
    // after it where events are denser than that (each instance is asked for audio up to its next events). A
    // part is made only once it has had a note, and rests again once its voices have died away; meanwhile the
    // controllers, programs, bends and SysEx it gets are kept as they stand and given to it before its next
    // note. Each part takes at most llBassNps note-ons a second (as the pieces' limiter does), and the
    // thinning (SetThin) leaves out a share of the quiet note-ons, never all of a velocity
    class BassStream : public Stream
    {
    public:
        static const unsigned StepFrames = 16;
        BassStream( unsigned uRate, const PieceOpts &o, int iParts, std::vector< BassMidiFontEx > vFonts )
            : m_uRate( uRate ), m_o( o ), m_vFonts( std::move( vFonts ) ), m_vParts( (size_t)std::max( 1, iParts ) ) {}
        ~BassStream() override
        {
            if ( m_bVoiceLog && m_llLogFrames )
            {
                long long llMax = 0;
                for ( long long v : m_vPeak ) { llMax = std::max( llMax, v ); }
                fprintf( stderr, "voices: %zu instances, cap %u each; mean %.0f sounding, peak %lld; busiest instance's peak %lld; "
                                 "an instance at its cap %.1f%% of the time\n", m_vParts.size(), m_o.uVoices, m_dVoiceFrames / (double)m_llLogFrames,
                         m_llPeakTotal, llMax, 100.0 * (double)m_llCapFrames / (double)m_llLogFrames );
            }
            for ( Part &p : m_vParts ) if ( p.h ) g_Bass.StreamFree( p.h );
        }

        void Push( const uint64_t *pFrames, const uint32_t *pMsgs, const float *, size_t n ) override
        {
            const int K = (int)m_vParts.size();
            for ( size_t i = 0; i < n; i++ )
            {
                const uint32_t m = pMsgs[i], s = m & 0xF0;
                const uint64_t f = pFrames[i];
                if ( s == 0x80 || s == 0x90 || s == 0xA0 )
                {
                    Part &p = m_vParts[(size_t)( K > 1 ? Bank::PartOf( m_o, K, m & 0xF, ( m >> 8 ) & 0x7F ) : 0 )];
                    if ( !Takes( p, f, m ) ) continue;
                    if ( s == 0x90 && ( m >> 16 & 0x7F ) && !p.bLive ) Wake( p, f );
                    if ( p.bLive ) p.vQ.push_back( { f, m, -1, SharedEnd() } );
                    continue;
                }
                Share( { f, m, -1, 0 } );
            }
        }
        void SysEx( uint64_t llFrame, const uint8_t *pData, size_t n ) override
        {
            m_vSysEx.emplace_back( pData, pData + n );
            Share( { llFrame, 0, (int)m_vSysEx.size() - 1, 0 } );
        }
        bool Render( float *pOut, unsigned uFrames, std::wstring &sError ) override
        {
            std::fill( pOut, pOut + (size_t)uFrames * 2, 0.0f );
            const uint64_t llStart = m_llRendered, llEnd = llStart + uFrames;
            m_llRendered = llEnd;
            // (an instance that could not be made: an error, so the render starts again rather than leave
            // that part silent while the buffer fills)
            if ( !m_sFailed.empty() ) { sError = m_sFailed; return false; }
            std::vector< Part * > vLive;
            for ( Part &p : m_vParts ) if ( p.bLive ) vLive.push_back( &p );
            if ( vLive.empty() ) return true;
            // (BASS's error code is the thread's own: the failing part's, kept from its thread)
            std::atomic< bool > bFailed{ false };
            std::atomic< int > iFailCode{ 0 };
            const int T = std::clamp( (int)std::thread::hardware_concurrency() - 2, 1, (int)vLive.size() );
            Parallel::ForBackground( T, vLive.size(), [&]( size_t i, int ) {
                if ( !Make( *vLive[i], llStart, llEnd ) ) { iFailCode = g_Bass.ErrorGetCode(); bFailed = true; }
            } );
            if ( bFailed ) { sError = L"BASSMIDI's continuous render failed: " + BassError( iFailCode.load() ); return false; }
            for ( Part *p : vLive )
                for ( size_t j = 0, n = (size_t)uFrames * 2; j < n; j++ ) pOut[j] += p->vAudio[j];
            // (PFAVIZ_VOICELOG=1: each instance's voices sounding, logged when the render ends)
            if ( m_bVoiceLog && g_Bass.ChannelGetAttribute )
            {
                if ( m_vPeak.size() != m_vParts.size() ) m_vPeak.assign( m_vParts.size(), 0 );
                long long llTotal = 0;
                bool bCapped = false;
                for ( size_t i = 0; i < m_vParts.size(); i++ )
                {
                    float f = 0.0f;
                    if ( !m_vParts[i].bLive || !m_vParts[i].h || !g_Bass.ChannelGetAttribute( m_vParts[i].h, BASS_ATTRIB_MIDI_VOICES_ACTIVE, &f ) ) continue;
                    const long long v = (long long)( f + 0.5f );
                    llTotal += v;
                    m_vPeak[i] = std::max( m_vPeak[i], v );
                    bCapped |= v + 1 >= (long long)m_o.uVoices;
                }
                m_dVoiceFrames += (double)llTotal * uFrames;
                m_llLogFrames += uFrames;
                m_llPeakTotal = std::max( m_llPeakTotal, llTotal );
                if ( bCapped ) m_llCapFrames += uFrames;
            }
            // (a part whose voices have all ended two seconds ago rests)
            for ( Part *p : vLive )
            {
                float fVoices = 1.0f;
                const bool bQuiet = p->iHead >= p->vQ.size() && p->llShared >= SharedEnd() && g_Bass.ChannelGetAttribute &&
                                    g_Bass.ChannelGetAttribute( p->h, BASS_ATTRIB_MIDI_VOICES_ACTIVE, &fVoices ) && fVoices < 0.5f;
                p->llQuiet = bQuiet ? p->llQuiet + uFrames : 0;
                if ( p->llQuiet >= 2ull * m_uRate ) { p->bLive = false; p->vQ.clear(); p->iHead = 0; p->llQuiet = 0; }
            }
            Compact();
            return true;
        }
        void SetCap( unsigned uVoices ) override
        {
            for ( Part &p : m_vParts ) if ( p.h ) g_Bass.ChannelSetAttribute( p.h, BASS_ATTRIB_MIDI_VOICES, (float)std::clamp( uVoices, 1u, 100000u ) );
            m_o.uVoices = uVoices;
        }
        void SetThin( double dLevel ) override { m_dThin = std::clamp( dLevel, 0.0, 1.0 ); }
        long long Voices() const override
        {
            if ( !g_Bass.ChannelGetAttribute ) return -1;
            long long n = 0;
            for ( const Part &p : m_vParts )
            {
                float f = 0.0f;
                if ( p.bLive && p.h && g_Bass.ChannelGetAttribute( p.h, BASS_ATTRIB_MIDI_VOICES_ACTIVE, &f ) ) n += (long long)( f + 0.5f );
            }
            return n;
        }

    private:
        // (llAt: in a part's queue, how many of the shared channel events come before it)
        struct Ev { uint64_t llFrame; uint32_t uMsg; int iSysEx; uint64_t llAt = 0; };
        struct Part
        {
            HSTREAM_ h = 0;
            bool bLive = false;
            uint64_t llQuiet = 0;
            std::vector< Ev > vQ;           // (live) its notes to come, in order (woken: the state it kept first)
            size_t iHead = 0;
            // The shared channel events it has had (live), or has kept (resting), up to
            uint64_t llShared = 0;
            // (resting) the channel events as they stand: the last of each controller, program, bend and
            // pressure, the RPN and NRPN ones and SysEx all in order
            std::vector< Ev > vState;
            size_t nDead = 0;               // (vState's entries since replaced)
            std::unordered_map< uint32_t, size_t > mState;
            // Its notes-a-second window (by millisecond), and the offs of note-ons not taken, by channel and key
            std::array< uint32_t, 1000 > aNps{};
            uint64_t llNpsTotal = 0;
            int64_t llMs = -1;
            std::vector< uint32_t > vMissed = std::vector< uint32_t >( 2048 );
            std::vector< float > vAudio;
            std::vector< uint8_t > vRaw;
        };

        // Whether a note event goes in: a note-on past the limit or thinned out is left out, and its off with it
        bool Takes( Part &p, uint64_t f, uint32_t m )
        {
            const uint32_t s = m & 0xF0, v = ( m >> 16 ) & 0x7F, uSlot = ( m & 0xF ) << 7 | ( ( m >> 8 ) & 0x7F );
            if ( s == 0x90 && v )
            {
                // The thinning: each velocity keeps its share of its note-ons (by an accumulator, so evenly)
                if ( m_dThin > 0.0 )
                {
                    const double dKeep = Stream::ThinKeep( m_dThin, (int)v );
                    double &acc = m_adKeep[v];
                    acc += dKeep;
                    if ( acc < 1.0 ) { p.vMissed[uSlot]++; return false; }
                    acc -= 1.0;
                }
                if ( m_o.llBassNps > 0 )
                {
                    const int64_t ms = (int64_t)( f * 1000 / m_uRate );
                    if ( p.llMs < 0 || ms - p.llMs >= 1000 ) { p.aNps.fill( 0 ); p.llNpsTotal = 0; p.llMs = ms; }
                    while ( p.llMs < ms ) { p.llMs++; uint32_t &c = p.aNps[(size_t)( p.llMs % 1000 )]; p.llNpsTotal -= c; c = 0; }
                    uint32_t &cNow = p.aNps[(size_t)( p.llMs % 1000 )];
                    const uint64_t llRate = std::max< uint64_t >( p.llNpsTotal, (uint64_t)cNow * 1000 * 4 / 3 );
                    if ( (uint64_t)v * (uint64_t)m_o.llBassNps / 127 <= llRate ) { p.vMissed[uSlot]++; return false; }
                    cNow++;
                    p.llNpsTotal++;
                }
                return true;
            }
            if ( ( s == 0x80 || s == 0x90 ) && p.vMissed[uSlot] ) { p.vMissed[uSlot]--; return false; }
            return true;
        }

        // The channel events (controllers, programs, bends, pressure, SysEx) go to every part, so they are kept
        // once, in m_vShared: a live part takes them between its notes as it makes its audio (by each note's
        // llAt), and the resting parts keep them as they stand (Keep) now and then, and as one wakes. Pushed
        // into each part's queue or kept by each resting part as they came, one controller cost a push or a
        // hash map update per part on the one thread that feeds them all
        uint64_t SharedEnd() const { return m_llSharedBase + m_vShared.size(); }
        const Ev &Shared( uint64_t i ) const { return m_vShared[(size_t)( i - m_llSharedBase )]; }
        void Share( const Ev &e )
        {
            m_vShared.push_back( e );
            if ( SharedEnd() - m_llCaughtUp >= 4096 ) { for ( Part &p : m_vParts ) if ( !p.bLive ) CatchUp( p ); m_llCaughtUp = SharedEnd(); Compact(); }
        }
        // A resting part's state brought up to date: of the shared events it has not kept, the ones that still
        // stand at the end (the last of each controller, every ordered one), which Keep leaves the same as
        // keeping them all (a newer value replaces an older one and goes last). The pass back from the end
        // marks them once for every resting part behind it
        void CatchUp( Part &p )
        {
            const uint64_t llEnd = SharedEnd();
            if ( p.llShared >= llEnd ) return;
            if ( m_llMarkedEnd != llEnd || m_llMarkedFrom > p.llShared )
            {
                if ( m_llMarkedEnd != llEnd ) { m_llMarkedFrom = llEnd; m_vStands.assign( m_vShared.size(), 0 ); m_uSeenGen++; }
                if ( m_vSeen.empty() ) m_vSeen.assign( 65536, 0 );
                m_vStands.resize( m_vShared.size() );
                for ( uint64_t i = m_llMarkedFrom; i-- > p.llShared; )
                {
                    const Ev &e = Shared( i );
                    const uint32_t s = e.uMsg & 0xF0, d1 = ( e.uMsg >> 8 ) & 0x7F;
                    const bool bOrdered = e.iSysEx >= 0 || ( s == 0xB0 && ( d1 == 6 || d1 == 38 || ( d1 >= 96 && d1 <= 101 ) ) );
                    bool bStands = bOrdered;
                    if ( !bOrdered )
                    {
                        uint32_t &u = m_vSeen[s == 0xB0 ? ( e.uMsg & 0xFFFF ) : ( e.uMsg & 0xFF )];
                        bStands = u != m_uSeenGen;
                        u = m_uSeenGen;
                    }
                    m_vStands[(size_t)( i - m_llSharedBase )] = bStands;
                }
                m_llMarkedFrom = p.llShared;
                m_llMarkedEnd = llEnd;
            }
            for ( uint64_t i = p.llShared; i < llEnd; i++ )
                if ( m_vStands[(size_t)( i - m_llSharedBase )] ) { const Ev &e = Shared( i ); Keep( p, e.llFrame, e.uMsg, e.iSysEx ); }
            p.llShared = llEnd;
        }
        // The shared events every part has had or kept, let go
        void Compact()
        {
            uint64_t llMin = SharedEnd();
            for ( const Part &p : m_vParts ) llMin = std::min( llMin, p.llShared );
            const size_t n = (size_t)( llMin - m_llSharedBase );
            if ( n < 4096 || n * 2 < m_vShared.size() ) return;
            m_vShared.erase( m_vShared.begin(), m_vShared.begin() + (ptrdiff_t)n );
            m_llSharedBase = llMin;
            m_llMarkedEnd = UINT64_MAX;
        }

        // A resting part's channel event kept as it stands. A newer value of one already kept goes last (the
        // old one left out), so it comes after any SysEx or RPN kept since, as it did in the song
        void Keep( Part &p, uint64_t f, uint32_t m, int iSysEx )
        {
            const uint32_t s = m & 0xF0, d1 = ( m >> 8 ) & 0x7F;
            const bool bOrdered = iSysEx >= 0 || ( s == 0xB0 && ( d1 == 6 || d1 == 38 || ( d1 >= 96 && d1 <= 101 ) ) );
            if ( !bOrdered )
            {
                const uint32_t key = s == 0xB0 ? ( m & 0xFFFF ) : ( m & 0xFF );
                auto it = p.mState.find( key );
                if ( it != p.mState.end() )
                {
                    if ( it->second + 1 == p.vState.size() ) { p.vState.back() = { f, m, -1 }; return; }
                    p.vState[it->second].iSysEx = DeadEv;
                    p.nDead++;
                    it->second = p.vState.size();
                }
                else p.mState.emplace( key, p.vState.size() );
            }
            p.vState.push_back( { f, m, iSysEx } );
            // (a long rest's dead entries cleared out now and then: once they are most of it. By the live
            // entries' count, a state of more than 4096 RPN or SysEx entries, which are never replaced, was
            // cleared out again at every event kept, and a pitch-bend tuned 31-EDO render through a song
            // full of controllers stalled)
            if ( p.vState.size() > 4096 && p.nDead * 2 > p.vState.size() )
            {
                std::vector< Ev > v;
                for ( const Ev &e : p.vState ) if ( e.iSysEx != DeadEv ) v.push_back( e );
                p.vState.swap( v );
                p.nDead = 0;
                p.mState.clear();
                for ( size_t i = 0; i < p.vState.size(); i++ )
                {
                    const Ev &e = p.vState[i];
                    const uint32_t es = e.uMsg & 0xF0, ed1 = ( e.uMsg >> 8 ) & 0x7F;
                    if ( e.iSysEx >= 0 || ( es == 0xB0 && ( ed1 == 6 || ed1 == 38 || ( ed1 >= 96 && ed1 <= 101 ) ) ) ) continue;
                    p.mState[es == 0xB0 ? ( e.uMsg & 0xFFFF ) : ( e.uMsg & 0xFF )] = i;
                }
            }
        }
        static constexpr int DeadEv = -2;   // (a kept event since replaced)

        // A resting part woken by a note at f: its instance (made the first time), then what it missed
        void Wake( Part &p, uint64_t f )
        {
            if ( !p.h )
            {
                // (a few tries, as for a piece: now and then BASSMIDI turns one down without a reason)
                const DWORD dwFlags = BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | ( m_o.bFx ? 0 : BASS_MIDI_NOFX );
                int iCode = 0;
                for ( int iTry = 0; iTry < 3 && !p.h; iTry++ )
                {
                    if ( iTry ) Sleep( 10 << iTry );
                    p.h = g_Bass.MidiStreamCreate( 16, dwFlags, m_uRate );
                    if ( p.h && !g_Bass.MidiStreamSetFonts( p.h, m_vFonts.data(), (DWORD)m_vFonts.size() | BASS_MIDI_FONT_EX ) )
                    {
                        iCode = g_Bass.ErrorGetCode();
                        g_Bass.StreamFree( p.h );
                        p.h = 0;
                    }
                    else if ( !p.h ) iCode = g_Bass.ErrorGetCode();
                }
                if ( !p.h )
                {
                    if ( m_sFailed.empty() ) m_sFailed = L"BASSMIDI could not start an instance: " + BassError( iCode );
                    return;
                }
                g_Bass.ChannelSetAttribute( p.h, BASS_ATTRIB_MIDI_VOICES, (float)std::clamp( m_o.uVoices, 1u, 100000u ) );
                g_Bass.ChannelSetAttribute( p.h, BASS_ATTRIB_MIDI_CPU, 0.0f );
                g_Bass.MidiStreamLoadSamples( p.h );
            }
            CatchUp( p );
            p.bLive = true;
            p.llQuiet = 0;
            p.vQ.clear();
            p.iHead = 0;
            // (at the note's frame, just before it)
            for ( const Ev &e : p.vState ) if ( e.iSysEx != DeadEv ) p.vQ.push_back( { f, e.uMsg, e.iSysEx, p.llShared } );
            p.vState.clear();
            p.nDead = 0;
            p.mState.clear();
        }

        // A live part's audio from llStart to llEnd, its events put in as it gets to them
        bool Make( Part &p, uint64_t llStart, uint64_t llEnd )
        {
            p.vAudio.assign( (size_t)( llEnd - llStart ) * 2, 0.0f );
            if ( !p.h ) return true;
            // (its next event: the shared ones before its next note first)
            const uint64_t llShEnd = SharedEnd();
            auto Next = [&]() -> const Ev * {
                const uint64_t llAt = p.iHead < p.vQ.size() ? p.vQ[p.iHead].llAt : llShEnd;
                if ( p.llShared < llAt ) return &Shared( p.llShared );
                return p.iHead < p.vQ.size() ? &p.vQ[p.iHead] : nullptr;
            };
            auto Take = [&]() {
                const uint64_t llAt = p.iHead < p.vQ.size() ? p.vQ[p.iHead].llAt : llShEnd;
                if ( p.llShared < llAt ) p.llShared++;
                else p.iHead++;
            };
            uint64_t f = llStart;
            while ( f < llEnd )
            {
                // Everything due by f + StepFrames in, at once
                p.vRaw.clear();
                for ( const Ev *pe; ( pe = Next() ) && pe->llFrame < f + StepFrames; )
                {
                    const Ev &e = *pe;
                    Take();
                    if ( e.iSysEx >= 0 ) { const auto &v = m_vSysEx[(size_t)e.iSysEx]; p.vRaw.insert( p.vRaw.end(), v.begin(), v.end() ); continue; }
                    const uint32_t s = e.uMsg & 0xF0;
                    p.vRaw.push_back( (uint8_t)( e.uMsg & 0xFF ) );
                    p.vRaw.push_back( (uint8_t)( ( e.uMsg >> 8 ) & 0x7F ) );
                    if ( s != 0xC0 && s != 0xD0 ) p.vRaw.push_back( (uint8_t)( ( e.uMsg >> 16 ) & 0x7F ) );
                }
                if ( !p.vRaw.empty() ) g_Bass.MidiStreamEvents( p.h, BASS_MIDI_EVENTS_RAW, p.vRaw.data(), (DWORD)p.vRaw.size() );
                // ... then the audio up to the next events (a step at least)
                const Ev *pNext = Next();
                uint64_t g = pNext ? std::max< uint64_t >( pNext->llFrame, f + StepFrames ) : llEnd;
                g = std::min( g, llEnd );
                const DWORD dwBytes = (DWORD)( ( g - f ) * 8 );
                const DWORD dwGot = g_Bass.ChannelGetData( p.h, p.vAudio.data() + ( f - llStart ) * 2, dwBytes | BASS_DATA_FLOAT );
                if ( dwGot == (DWORD)-1 ) return false;
                f = g;
            }
            if ( p.iHead > 4096 && p.iHead * 2 > p.vQ.size() ) { p.vQ.erase( p.vQ.begin(), p.vQ.begin() + (ptrdiff_t)p.iHead ); p.iHead = 0; }
            return true;
        }

        unsigned m_uRate;
        PieceOpts m_o;
        std::vector< BassMidiFontEx > m_vFonts;
        std::vector< Part > m_vParts;
        std::vector< std::vector< uint8_t > > m_vSysEx;
        std::vector< Ev > m_vShared;                // (the shared channel events from m_llSharedBase on)
        uint64_t m_llSharedBase = 0, m_llCaughtUp = 0;
        std::vector< uint8_t > m_vStands;           // (CatchUp's marks, for m_llMarkedFrom to m_llMarkedEnd)
        std::vector< uint32_t > m_vSeen;
        uint32_t m_uSeenGen = 0;
        uint64_t m_llMarkedFrom = 0, m_llMarkedEnd = UINT64_MAX;
        uint64_t m_llRendered = 0;
        std::wstring m_sFailed;         // (an instance that could not be made)
        double m_dThin = 0.0;
        std::array< double, 128 > m_adKeep{};
        const bool m_bVoiceLog = getenv( "PFAVIZ_VOICELOG" ) != nullptr;
        std::vector< long long > m_vPeak;
        double m_dVoiceFrames = 0.0;
        long long m_llLogFrames = 0, m_llPeakTotal = 0, m_llCapFrames = 0;
    };

    class BassBank : public Bank
    {
    public:
        explicit BassBank( unsigned uRate ) : m_uRate( uRate ) {}
        // Each soundfont is opened once and kept open while PFAviz++ runs, every bank using the same handle
        // (never freed with its bank): BASSMIDI shares an SFZ's samples with other SFZs that use the same
        // sample files (as Loud and Proud Remastered's presets do), and freeing one of them while another
        // played crashed inside bassmidi.dll. A test changing the soundfonts 40 times while six instances
        // played crashed in about half the runs, freeing the old ones once nothing used them; with the
        // soundfonts kept, none did
        bool Open( const BankKey &key, std::wstring &sError )
        {
            static std::mutex s_mx;
            static std::map< std::wstring, HSOUNDFONT_ > s_mOpen;
            std::lock_guard< std::mutex > lock( s_mx );
            for ( const std::wstring &s : key.vFonts )
            {
                // (by path and when the file was last written, so a soundfont changed on disk is read again)
                std::wstring sKey = s;
                CharLowerBuffW( sKey.data(), (DWORD)sKey.size() );
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if ( GetFileAttributesExW( s.c_str(), GetFileExInfoStandard, &fa ) )
                    sKey += L"|" + std::to_wstring( ( (uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 ) | fa.ftLastWriteTime.dwLowDateTime );
                HSOUNDFONT_ &f = s_mOpen[sKey];
                if ( !f )
                {
                    f = OpenBassFont( s, sError );
                    if ( !f ) { s_mOpen.erase( sKey ); return false; }
                    // An SFZ's samples all loaded now, on this one thread: BASSMIDI loading an SFZ's sample files
                    // as notes need them, from several instances at once, crashed inside bassmidi.dll (Loud and
                    // Proud Remastered, 8 instances striking the same keys: 5 runs of 5; loaded first, 8 of 8
                    // ran through, its 333 MB in 0.3 s from the disk cache). An SF2's samples, in the file
                    // itself, did not (6 runs of 6), so they are still loaded as they are needed
                    // (developer switch PFAVIZ_SFZPRELOAD=0: not loaded ahead, to compare)
                    static const bool s_bNoPreload = getenv( "PFAVIZ_SFZPRELOAD" ) && atoi( getenv( "PFAVIZ_SFZPRELOAD" ) ) == 0;
                    const bool bSfz = s.size() > 4 && _wcsicmp( s.c_str() + s.size() - 4, L".sfz" ) == 0;
                    if ( bSfz && !s_bNoPreload && g_Bass.MidiFontLoad && !g_Bass.MidiFontLoad( f, -1, -1 ) )
                        sWarnings += ( sWarnings.empty() ? L"" : L"\n" ) + std::wstring( L"BASSMIDI could not load all of " ) + s +
                                     L"'s samples ahead (" + BassError( g_Bass.ErrorGetCode() ) + L"): they load as they are needed";
                }
                m_vFonts.push_back( f );
            }
            Fallbacks();
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
        std::unique_ptr< Stream > OpenStream( const PieceOpts &o, std::wstring &sError ) override
        {
            if ( !g_Bass.MidiStreamCreate || !g_Bass.MidiStreamEvents ) { sError = L"bassmidi.dll has no live streams"; return nullptr; }
            return std::make_unique< BassStream >( m_uRate, o, Parts( o ), Fonts() );
        }

    private:
        bool RenderOne( const std::vector< uint8_t > &vMidi, uint64_t llSkip, uint64_t llFrames, const PieceOpts &o,
                        const std::atomic< bool > *pCancel, std::vector< float > &vOut, std::wstring &sError )
        {
            // A piece is played into a live BASSMIDI instance, its events read back from its file, as the continuous
            // render plays the song: BASSMIDI reading the file itself let the notes go short after their note-offs,
            // the sustain pedal and releases cut (a pedalled chord at a song's end died in half a second instead of
            // ringing on), and now and then turned a piece down (BASS error 0, or out of memory for a dense piece's
            // event list). Only without live instances (an old bassmidi.dll) does it read the file, a few tries apart
            // (developer switch PFAVIZ_BASSFILEPIECE=1: always the file, to compare)
            const DWORD dwFlags = BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | ( o.bFx ? 0 : BASS_MIDI_NOFX );
            static const bool s_bFileOnly = getenv( "PFAVIZ_BASSFILEPIECE" ) && atoi( getenv( "PFAVIZ_BASSFILEPIECE" ) ) != 0;
            HSTREAM_ h = 0;
            int iCode = 0;
            DWORD dwWin = 0;
            std::vector< LiveEv > vLive;
            std::vector< uint8_t > vRaw;
            bool bLive = false;
            if ( !s_bFileOnly && g_Bass.MidiStreamCreate && g_Bass.MidiStreamEvents )
            {
                if ( !ReadPiece( vMidi, vLive, vRaw ) ) { sError = L"a piece's MIDI could not be read back (" + std::to_wstring( vMidi.size() ) + L" bytes)"; return false; }
                for ( int iTry = 0; iTry < 3 && !h; iTry++ )
                {
                    if ( iTry ) Sleep( 10 << iTry );
                    if ( pCancel && pCancel->load( std::memory_order_relaxed ) ) return false;
                    h = g_Bass.MidiStreamCreate( 16, dwFlags, m_uRate );
                    if ( !h ) iCode = g_Bass.ErrorGetCode();
                }
                bLive = h != 0;
            }
            for ( int iTry = 0; iTry < 4 && !h; iTry++ )
            {
                if ( iTry ) Sleep( 25 << iTry );
                if ( pCancel && pCancel->load( std::memory_order_relaxed ) ) return false;
                SetLastError( 0 );
                h = g_Bass.MidiStreamCreateFile( TRUE, vMidi.data(), 0, vMidi.size(), dwFlags | BASS_MIDI_DECAYEND, m_uRate );
                if ( !h ) { iCode = g_Bass.ErrorGetCode(); dwWin = GetLastError(); }
            }
            if ( !h )
            {
                sError = L"BASSMIDI could not make a piece: " + BassError( iCode ) + L" (as a live instance, then from its file; " +
                         std::to_wstring( vMidi.size() ) + L" bytes, " + std::to_wstring( m_uRate ) + L" Hz" +
                         ( dwWin ? L", Windows error " + std::to_wstring( dwWin ) : std::wstring() ) + L")";
                return false;
            }
            const std::vector< BassMidiFontEx > vFonts = Fonts();
            if ( !g_Bass.MidiStreamSetFonts( h, vFonts.data(), (DWORD)vFonts.size() | BASS_MIDI_FONT_EX ) )
            {
                // (a piece without its soundfonts would be silent: an error, so it is made again)
                sError = L"BASSMIDI could not give a piece its soundfonts: " + BassError( g_Bass.ErrorGetCode() );
                g_Bass.StreamFree( h );
                return false;
            }
            g_Bass.ChannelSetAttribute( h, BASS_ATTRIB_MIDI_VOICES, (float)std::clamp( o.uVoices, 1u, 100000u ) );
            g_Bass.ChannelSetAttribute( h, BASS_ATTRIB_MIDI_CPU, 0.0f );
            g_Bass.MidiStreamLoadSamples( h );

            vOut.assign( (size_t)llFrames * 2, 0.0f );
            std::vector< float > vSkip;
            uint64_t llDone = 0, llTotal = llSkip + llFrames;
            const uint64_t Chunk = 4096;
            bool bOk = true;
            size_t iLive = 0;
            while ( llDone < llTotal )
            {
                if ( pCancel && pCancel->load( std::memory_order_relaxed ) ) { bOk = false; break; }
                uint64_t n = std::min( Chunk, llTotal - llDone );
                if ( bLive )
                {
                    // (everything due by StepFrames on in, then the audio up to the next events, a step at least)
                    const size_t i0 = iLive;
                    while ( iLive < vLive.size() && vLive[iLive].llFrame < llDone + BassStream::StepFrames ) iLive++;
                    if ( iLive > i0 )
                        g_Bass.MidiStreamEvents( h, BASS_MIDI_EVENTS_RAW, vRaw.data() + vLive[i0].uAt,
                                                 (DWORD)( ( iLive < vLive.size() ? vLive[iLive].uAt : vRaw.size() ) - vLive[i0].uAt ) );
                    if ( iLive < vLive.size() ) n = std::min( n, std::max< uint64_t >( vLive[iLive].llFrame, llDone + BassStream::StepFrames ) - llDone );
                }
                float *p;
                if ( llDone < llSkip )
                {
                    n = std::min( n, llSkip - llDone );
                    vSkip.resize( (size_t)n * 2 );
                    p = vSkip.data();
                }
                else p = vOut.data() + ( llDone - llSkip ) * 2;
                DWORD got = g_Bass.ChannelGetData( h, p, (DWORD)( n * 8 ) | BASS_DATA_FLOAT );
                if ( bLive && got == (DWORD)-1 ) { bOk = false; sError = L"BASSMIDI's live instance for a piece failed"; break; }
                if ( got == (DWORD)-1 || got == 0 ) break;     // (the sound has died away: silence from here)
                llDone += got / 8;
            }
            g_Bass.StreamFree( h );
            return bOk;
        }

        // A piece's events read back from its file (as SmfWriter writes it: one track, 48000 ticks a second,
        // running status, SysEx, meta events left out), as raw MIDI bytes at their frames
        struct LiveEv { uint64_t llFrame; uint32_t uAt; };
        bool ReadPiece( const std::vector< uint8_t > &v, std::vector< LiveEv > &vEv, std::vector< uint8_t > &vRaw ) const
        {
            if ( v.size() < 22 || memcmp( v.data(), "MThd", 4 ) || memcmp( v.data() + 14, "MTrk", 4 ) ) return false;
            const uint8_t *p = v.data() + 22, *pEnd = v.data() + v.size();
            auto Vlq = [&]( uint64_t &n ) {
                n = 0;
                for ( int k = 0; k < 10; k++ )
                {
                    if ( p >= pEnd ) return false;
                    const uint8_t b = *p++;
                    n = n << 7 | ( b & 0x7F );
                    if ( !( b & 0x80 ) ) return true;
                }
                return false;
            };
            uint64_t llTick = 0, d = 0;
            uint8_t uStatus = 0;
            while ( p < pEnd )
            {
                if ( !Vlq( d ) ) return false;
                llTick += d;
                if ( p >= pEnd ) return false;
                const uint64_t llFrame = llTick * m_uRate / 48000;
                const uint8_t b = *p;
                if ( b == 0xFF )
                {
                    if ( pEnd - p < 2 ) return false;
                    const uint8_t uType = p[1];
                    p += 2;
                    uint64_t n;
                    if ( !Vlq( n ) || n > (uint64_t)( pEnd - p ) ) return false;
                    p += n;
                    uStatus = 0;
                    if ( uType == 0x2F ) break;
                    continue;
                }
                if ( b == 0xF0 )
                {
                    p++;
                    uint64_t n;
                    if ( !Vlq( n ) || n > (uint64_t)( pEnd - p ) ) return false;
                    vEv.push_back( { llFrame, (uint32_t)vRaw.size() } );
                    vRaw.push_back( 0xF0 );
                    vRaw.insert( vRaw.end(), p, p + n );
                    p += n;
                    uStatus = 0;
                    continue;
                }
                if ( b & 0x80 ) { uStatus = b; p++; }
                if ( !uStatus || uStatus >= 0xF0 ) return false;
                const int nData = ( uStatus & 0xE0 ) == 0xC0 ? 1 : 2;
                if ( pEnd - p < nData ) return false;
                vEv.push_back( { llFrame, (uint32_t)vRaw.size() } );
                vRaw.push_back( uStatus );
                vRaw.insert( vRaw.end(), p, p + nData );
                p += nData;
                if ( vRaw.size() > 0xFFFFFFF0u ) return false;
            }
            return true;
        }

        // Later soundfonts replace earlier ones; BASSMIDI uses the first it is given first. Then the programs none
        // of them has, as the fallback preset
        std::vector< BassMidiFontEx > Fonts() const
        {
            std::vector< BassMidiFontEx > vFonts;
            for ( size_t i = m_vFonts.size(); i-- > 0; ) vFonts.push_back( { m_vFonts[i], -1, -1, -1, 0, 0 } );
            vFonts.insert( vFonts.end(), m_vFallback.begin(), m_vFallback.end() );
            return vFonts;
        }

        // BASSMIDI plays nothing on a channel whose program no soundfont has (in its bank or bank 0), so a
        // piano-only soundfont is silent for a song whose channels choose other instruments. Kestrel and Merlin
        // fall back to a preset they have; here each melodic program missing from bank 0 of every soundfont
        // is given as the fallback: bank 0 program 0 (the piano) from the last soundfont that has it, else the
        // last soundfont's first melodic preset. Drums (bank 128) are left as they are
        void Fallbacks()
        {
            m_vFallback.clear();
            if ( !g_Bass.MidiFontGetInfo || !g_Bass.MidiFontGetPresets ) return;
            bool abHave[128] = {};
            HSOUNDFONT_ fFrom = 0;
            int iFromPreset = -1, iFromBank = 0;
            bool bFromPiano = false;
            for ( HSOUNDFONT_ f : m_vFonts )
            {
                BassMidiFontInfo info = {};
                if ( !g_Bass.MidiFontGetInfo( f, &info ) || !info.presets ) continue;
                std::vector< DWORD > v( info.presets );
                if ( !g_Bass.MidiFontGetPresets( f, v.data() ) ) continue;
                bool bFirst = true;     // (this soundfont's first melodic preset)
                for ( DWORD d : v )
                {
                    const int iPreset = LOWORD( d ), iBank = HIWORD( d );
                    if ( iBank >= 128 || iPreset >= 128 ) continue;
                    if ( iBank == 0 ) abHave[iPreset] = true;
                    // (later soundfonts win, the piano over the rest)
                    const bool bPiano = iBank == 0 && iPreset == 0;
                    if ( bPiano || ( bFirst && !bFromPiano ) )
                    {
                        fFrom = f;
                        iFromPreset = iPreset;
                        iFromBank = iBank;
                        bFromPiano = bPiano;
                    }
                    bFirst = false;
                }
            }
            if ( iFromPreset < 0 ) return;
            for ( int i = 0; i < 128; i++ )
                if ( !abHave[i] ) m_vFallback.push_back( { fFrom, iFromPreset, iFromBank, i, 0, 0 } );
        }

        unsigned m_uRate;
        std::vector< HSOUNDFONT_ > m_vFonts;
        std::vector< BassMidiFontEx > m_vFallback;
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
        uint64_t ( *StreamVoices )( const void * ) = nullptr;     // (optional: the voices sounding)

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
            Get( StreamVoices, "kb_stream_voices" );
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

    std::wstring FileName( const std::wstring &s )
    {
        const size_t i = s.find_last_of( L"\\/" );
        return i == std::wstring::npos ? s : s.substr( i + 1 );
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
            if ( m_pMerlin && m_pMerlin != m_pLoaded ) g_Kb.Unload( m_pMerlin );
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
            m_vFonts = vFonts;
            m_vNames = key.vFonts;
            m_pLoaded = g_Kb.LoadSet( vFonts.data(), (uint32_t)vFonts.size() );
            sWarnings = g_Kb.Warnings();
            if ( !m_pLoaded ) { sError = L"Kestrel could not load the soundfonts on the graphics card: " + g_Kb.LastError(); return false; }
            return true;
        }
        int Engine() const override { return EngineKestrel; }
        int Parallel( int ) const override { return 1; }
        // (Kestrel plays a marked note once)
        bool RepeatMarks( const PieceOpts &o ) override
        {
            std::wstring sWhy;
            return o.eEngine == EngineMerlin && !o.bForceKestrel && MerlinTakes( sWhy );
        }
        // Whether Merlin plays: one soundfont, tried with a one-note piece. Merlin takes one soundfont only, so of
        // several it plays the lowest in the list it takes (the one that overrides the rest), alone: the others
        // are left out of its pieces (Kestrel, making what Merlin hands it, has them all). sWhy: why not, or which
        // one it plays when it leaves soundfonts out
        bool MerlinTakes( std::wstring &sWhy ) override
        {
            std::lock_guard< std::mutex > lock( m_mxProbe );
            if ( m_iMerlin < 0 )
            {
                m_iMerlin = 0;
                std::wstring sLast;
                for ( size_t i = m_vFonts.size(); i-- > 0 && m_iMerlin == 0; )
                {
                    void *pSet = m_nFonts == 1 ? m_pLoaded : nullptr;
                    if ( !pSet )
                    {
                        std::lock_guard< std::mutex > lockKb( g_mxKb );
                        pSet = g_Kb.LoadSet( &m_vFonts[i], 1 );
                        g_Kb.Warnings();    // (the full set's load said what there is to say)
                        if ( !pSet ) { sLast = g_Kb.LastError(); continue; }
                    }
                    std::vector< float > v;
                    PieceOpts o;
                    if ( RenderWith( 1, OneNote(), 0, m_uRate / 10, o, nullptr, v, sLast, pSet ) )
                    {
                        m_iMerlin = 1;
                        m_pMerlin = pSet;
                        if ( m_nFonts > 1 )
                            m_sMerlinWhy = L"Merlin takes one soundfont, so it plays " + FileName( m_vNames[i] ) + L" alone (" +
                                           std::to_wstring( m_nFonts - 1 ) + ( m_nFonts == 2 ? L" other left out)" : L" others left out)" );
                    }
                    else if ( pSet != m_pLoaded ) g_Kb.Unload( pSet );
                }
                if ( !m_iMerlin ) m_sMerlinWhy = m_nFonts > 1 ? L"it takes none of the soundfonts (the last: " + sLast + L")" : sLast;
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
                if ( RenderWith( 1, vParts[0], llSkip, llFrames, o, pCancel, vOut, sMerlin, m_pMerlin ) ) return true;
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
                         const std::atomic< bool > *pCancel, std::vector< float > &vOut, std::wstring &sError, void *pSet = nullptr )
        {
            const KbOptions opts = MakeOpts( o );
            void *p;
            {
                std::lock_guard< std::mutex > lock( g_mxKb );
                p = g_Kb.PieceStartBytes( pSet ? pSet : m_pLoaded,vMidi.data(), vMidi.size(), &opts, uEngine, llSkip, llFrames );
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
            long long Voices() const override { return g_Kb.StreamVoices ? (long long)g_Kb.StreamVoices( m_p ) : -1; }
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
        std::vector< void * > m_vFonts;     // (the soundfonts read, in list order, and their files)
        std::vector< std::wstring > m_vNames;
        void *m_pMerlin = nullptr;          // what Merlin plays with: the set, or the one soundfont of it it takes
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
    if ( key.vFonts.empty() ) { sError = L"no soundfont chosen (Preferences > Audio > Pre-Render)"; return nullptr; }
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
