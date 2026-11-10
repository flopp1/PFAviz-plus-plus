#include "audio/MidiOutput.h"
#include "audio/Tuning31.h"
#include "audio/PitchBendTuning.h"
#include "audio/ScaleTuning.h"

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <mutex>

#include "core/Config.h"

namespace
{
    // A sent SysEx's header until the synth marks it done (at most a second: one that never does is not
    // waited on forever)
    void WaitDone( const MIDIHDR &hdr, bool bSent )
    {
        for ( int i = 0; bSent && i < 1000 && !( hdr.dwFlags & MHDR_DONE ); i++ ) Sleep( 1 );
    }

    //-------------------------------------------------------------------------
    // WinMM
    //-------------------------------------------------------------------------

    class WinMMOutput : public MidiOutput
    {
    public:
        bool Open( int iDev )
        {
            MIDIOUTCAPSW moc;
            m_sName = midiOutGetDevCapsW( iDev, &moc, sizeof( moc ) ) == MMSYSERR_NOERROR ? moc.szPname : L"MIDI output";
            return midiOutOpen( &m_h, iDev, 0, 0, CALLBACK_NULL ) == MMSYSERR_NOERROR;
        }
        ~WinMMOutput() override
        {
            if ( m_h ) { midiOutReset( m_h ); midiOutClose( m_h ); }
        }
        void Send( uint32_t uMsg ) override { midiOutShortMsg( m_h, uMsg ); }
        bool SendSysEx( const uint8_t *pData, size_t n ) override
        {
            MIDIHDR hdr = {};
            hdr.lpData = (LPSTR)pData;
            hdr.dwBufferLength = (DWORD)n;
            if ( midiOutPrepareHeader( m_h, &hdr, sizeof( hdr ) ) != MMSYSERR_NOERROR ) return false;
            MMRESULT result = midiOutLongMsg( m_h, &hdr, sizeof( hdr ) );
            WaitDone( hdr, result == MMSYSERR_NOERROR );
            midiOutUnprepareHeader( m_h, &hdr, sizeof( hdr ) );
            return result == MMSYSERR_NOERROR;
        }
        void SetVolume( double dVolume ) override
        {
            DWORD dw = static_cast< DWORD >( 0xFFFF * dVolume + 0.5 );
            midiOutSetVolume( m_h, dw | ( dw << 16 ) );
        }
        const std::wstring &Name() const override { return m_sName; }

    private:
        HMIDIOUT m_h = nullptr;
        std::wstring m_sName;
    };

    //-------------------------------------------------------------------------
    // KDMAPI (OmniMIDI.dll, loaded from System32 or the program folder only)
    //-------------------------------------------------------------------------

    struct Kdmapi
    {
        typedef BOOL ( WINAPI *PFN_BOOL )();
        typedef VOID ( WINAPI *PFN_SEND )( DWORD );
        typedef UINT ( WINAPI *PFN_LONG )( MIDIHDR *, UINT );     // the header and its size, as midiOut*
        typedef unsigned long long ( WINAPI *PFN_VOICES )();     // 64 bits: Maestro returns one (OmniMIDI's 32 are the low half)
        typedef float ( WINAPI *PFN_RENDERTIME )();
        HMODULE hMod = nullptr;
        PFN_BOOL pIsAvailable = nullptr, pInit = nullptr, pTerminate = nullptr;
        PFN_SEND pSend = nullptr;
        PFN_LONG pLong = nullptr, pPrepare = nullptr, pUnprepare = nullptr;
        PFN_VOICES pActiveVoices = nullptr;
        PFN_RENDERTIME pRenderingTime = nullptr;
        bool bAvailable = false;

        // Loaded once, on first use from any thread (thread-safe static initialisation)
        static Kdmapi &Get()
        {
            static Kdmapi k = Load();
            return k;
        }

    private:
        static Kdmapi Load()
        {
            Kdmapi k;
            k.hMod = LoadLibraryExW( L"OmniMIDI.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR );
            if ( k.hMod )
            {
                k.pIsAvailable = ( PFN_BOOL )GetProcAddress( k.hMod, "IsKDMAPIAvailable" );
                k.pInit = ( PFN_BOOL )GetProcAddress( k.hMod, "InitializeKDMAPIStream" );
                k.pTerminate = ( PFN_BOOL )GetProcAddress( k.hMod, "TerminateKDMAPIStream" );
                k.pSend = ( PFN_SEND )GetProcAddress( k.hMod, "SendDirectData" );
                k.pLong = ( PFN_LONG )GetProcAddress( k.hMod, "SendDirectLongData" );
                k.pPrepare = ( PFN_LONG )GetProcAddress( k.hMod, "PrepareLongData" );
                k.pUnprepare = ( PFN_LONG )GetProcAddress( k.hMod, "UnprepareLongData" );
                k.pActiveVoices = ( PFN_VOICES )GetProcAddress( k.hMod, "GetVoiceCount" );
                if ( !k.pActiveVoices ) k.pActiveVoices = ( PFN_VOICES )GetProcAddress( k.hMod, "GetActiveVoices" );
                k.pRenderingTime = ( PFN_RENDERTIME )GetProcAddress( k.hMod, "GetRenderingTime" );
                k.bAvailable = k.pInit && k.pTerminate && k.pSend && ( !k.pIsAvailable || k.pIsAvailable() );
            }
            return k;
        }
    };

    std::atomic< bool > g_bKdmapiLong{ false };
    std::atomic< uint64_t > g_llLongMessages{ 0 }, g_llLongBytes{ 0 };
    std::atomic< size_t > g_iLongLargest{ 0 };

    class KdmapiOutput : public MidiOutput
    {
    public:
        bool Open()
        {
            Kdmapi &k = Kdmapi::Get();
            if ( !k.bAvailable || !k.pInit() ) return false;
            m_pSend = k.pSend;
            m_bOpen = true;
            return true;
        }
        ~KdmapiOutput() override
        {
            if ( m_bOpen ) { AllNotesOff(); Kdmapi::Get().pTerminate(); }
        }
        void Send( uint32_t uMsg ) override { m_pSend( uMsg ); }
        // Long sends: a batch (one of the sender's slices, at most PlaybackEngine::SliceMsgs events) goes
        // out as one long message of its channel messages, raw bytes with running status, however long
        // (Maestro takes any size; past about 1 KB a longer one costs no less an event, and gathering
        // batches would only hold late events back). Anything else goes short, in its place
        void SendBatch( const uint32_t *pMsgs, size_t n ) override
        {
            Kdmapi::PFN_SEND pSend = m_pSend;
            if ( !g_bKdmapiLong.load( std::memory_order_relaxed ) || m_bLongFailed )
            {
                for ( size_t i = 0; i < n; i++ ) pSend( pMsgs[i] );
                return;
            }
            if ( m_vLong.size() < n * 3 ) m_vLong.resize( n * 3 );
            uint8_t *pLong = m_vLong.data();
            size_t iLen = 0;
            unsigned uRun = 0;      // the running status
            // (a long message not taken: its events are lost, once; short sends after)
            auto SendGathered = [&]() { if ( iLen ) SendLongBuffer( iLen ); iLen = 0; uRun = 0; };
            for ( size_t i = 0; i < n; i++ )
            {
                uint32_t m = pMsgs[i];
                unsigned st = m & 0xFF;
                if ( st < 0x80 || st >= 0xF0 ) { SendGathered(); pSend( m ); continue; }
                if ( m_bLongFailed ) { pSend( m ); continue; }
                bool bTwo = ( st & 0xE0 ) == 0xC0;  // program change and channel pressure: one data byte
                if ( st != uRun ) { pLong[iLen++] = (uint8_t)st; uRun = st; }
                pLong[iLen++] = (uint8_t)( ( m >> 8 ) & 0x7F );
                if ( !bTwo ) pLong[iLen++] = (uint8_t)( ( m >> 16 ) & 0x7F );
            }
            SendGathered();
        }
        const std::wstring &Name() const override { static const std::wstring s = MidiOutputs::KdmapiName(); return s; }
        bool IsKdmapi() const override { return true; }
        bool SendSysEx( const uint8_t *pData, size_t n ) override
        {
            Kdmapi &k = Kdmapi::Get();
            if ( !k.pLong || !k.pPrepare || !k.pUnprepare ) return false;
            MIDIHDR hdr = {};
            hdr.lpData = (LPSTR)pData;
            hdr.dwBufferLength = (DWORD)n;
            if ( k.pPrepare( &hdr, sizeof( hdr ) ) != MMSYSERR_NOERROR ) return false;
            UINT result = k.pLong( &hdr, sizeof( hdr ) );
            WaitDone( hdr, result == MMSYSERR_NOERROR );
            k.pUnprepare( &hdr, sizeof( hdr ) );
            return result == MMSYSERR_NOERROR;
        }

    private:
        Kdmapi::PFN_SEND m_pSend = nullptr;
        bool m_bOpen = false;
        bool m_bLongFailed = false;             // a long message failed or never completed: short sends from then on
        std::vector< uint8_t > m_vLong;         // the long message being made (grows to the largest batch's)

        bool SendLongBuffer( size_t len )
        {
            Kdmapi &k = Kdmapi::Get();
            if ( !k.pLong || !k.pPrepare || !k.pUnprepare ) { m_bLongFailed = true; return false; }
            MIDIHDR hdr = {};
            hdr.lpData = (LPSTR)m_vLong.data();
            hdr.dwBufferLength = hdr.dwBytesRecorded = (DWORD)len;
            if ( k.pPrepare( &hdr, sizeof( hdr ) ) != MMSYSERR_NOERROR ) { m_bLongFailed = true; return false; }
            bool bSent = k.pLong( &hdr, sizeof( hdr ) ) == MMSYSERR_NOERROR;
            if ( bSent )
            {
                g_llLongMessages.fetch_add( 1, std::memory_order_relaxed );
                g_llLongBytes.fetch_add( len, std::memory_order_relaxed );
                if ( len > g_iLongLargest.load( std::memory_order_relaxed ) ) g_iLongLargest.store( len, std::memory_order_relaxed );
            }
            // Done at once by the synths tried; a short spin, then up to 50 ms before giving long sends up
            long long llEnd = 0;
            for ( int i = 0; bSent && !( hdr.dwFlags & MHDR_DONE ); i++ )
            {
                if ( i < 4096 ) { YieldProcessor(); continue; }
                LARGE_INTEGER t, f;
                QueryPerformanceCounter( &t );
                QueryPerformanceFrequency( &f );
                long long llNow = t.QuadPart * 1000 / f.QuadPart;
                if ( !llEnd ) llEnd = llNow + 50;
                if ( llNow > llEnd ) { m_bLongFailed = true; break; }
                Sleep( 0 );
            }
            for ( int i = 0; i < 50 && k.pUnprepare( &hdr, sizeof( hdr ) ) == MIDIERR_STILLPLAYING; i++ ) Sleep( 1 );
            if ( !bSent ) m_bLongFailed = true;
            return bSent;
        }
    };

}

bool MidiOutputs::KdmapiAvailable() { return Kdmapi::Get().bAvailable; }
void MidiOutputs::SetKdmapiLongSends( bool bOn )
{
    g_bKdmapiLong.store( bOn, std::memory_order_relaxed );
}

void MidiOutputs::KdmapiLongCounts( uint64_t &llMessages, uint64_t &llBytes, size_t &iLargest )
{
    llMessages = g_llLongMessages.load( std::memory_order_relaxed );
    llBytes = g_llLongBytes.load( std::memory_order_relaxed );
    iLargest = g_iLongLargest.exchange( 0, std::memory_order_relaxed );
}

std::wstring MidiOutputs::KdmapiName() { return L"KDMAPI (OmniMIDI)"; }

bool MidiOutputs::KdmapiActiveVoices( unsigned &uVoices )
{
    Kdmapi &k = Kdmapi::Get();
    if ( !k.bAvailable || !k.pActiveVoices ) return false;
    uVoices = (unsigned)std::min< unsigned long long >( k.pActiveVoices(), UINT_MAX );
    return true;
}

bool MidiOutputs::KdmapiRenderingTime( float &fPercent )
{
    Kdmapi &k = Kdmapi::Get();
    if ( !k.bAvailable || !k.pRenderingTime ) return false;
    fPercent = k.pRenderingTime();
    return true;
}

std::wstring MidiOutputs::KdmapiStatus()
{
    Kdmapi &k = Kdmapi::Get();
    // (a KDMAPI synth installs its OmniMIDI.dll, the name the interface began with)
    if ( !k.hMod ) return L"Not available: no KDMAPI synth was found (its OmniMIDI.dll, in System32 or next to PFAviz++.exe).";
    if ( !k.bAvailable ) return L"Not available: a KDMAPI synth was found (OmniMIDI.dll) but does not report KDMAPI as available.";
    return L"Available.";
}

std::unique_ptr< MidiOutput > MidiOutputs::OpenKdmapi()
{
    auto p = std::make_unique< KdmapiOutput >();
    if ( !p->Open() ) return nullptr;
    return p;
}

std::unique_ptr< MidiOutput > MidiOutputs::OpenWinMM( int iDevice )
{
    if ( iDevice < 0 ) return nullptr;
    auto p = std::make_unique< WinMMOutput >();
    if ( !p->Open( iDevice ) ) return nullptr;
    return p;
}

std::wstring MidiOutputs::ConfiguredName()
{
    Config &config = Config::GetConfig();
    if ( config.GetPerformanceSettings().bPreferKDMAPI && KdmapiAvailable() ) return KdmapiName();
    std::lock_guard< std::mutex > lock( config.AudioMutex() );
    const AudioSettings &cAudio = config.GetAudioSettings();
    if ( cAudio.iOutDevice >= 0 && cAudio.iOutDevice < (int)cAudio.vMIDIOutDevices.size() ) return cAudio.vMIDIOutDevices[cAudio.iOutDevice];
    return std::wstring();
}

namespace
{
    // The configured output stays open from its first use until the program ends or the setting changes:
    // opening a synth (KDMAPI's InitializeKDMAPIStream, or a WinMM synth driver's midiOutOpen) loads its
    // soundfonts again, which took seconds on every song. Each screen gets a handle to it instead; one
    // screen uses it at a time (ChangeState waits for the old screen's audio to be released)
    struct Shared
    {
        std::mutex mx;
        std::shared_ptr< MidiOutput > pOut;
        bool bKdmapi = false;
        int iDevice = -1;
        std::wstring sDevice;
    };
    Shared &TheShared() { static Shared *p = new Shared; return *p; }   // (closed by CloseShared, never by exit)

    class SharedOutput : public MidiOutput
    {
    public:
        explicit SharedOutput( std::shared_ptr< MidiOutput > p ) : m_pOut( std::move( p ) ) {}
        // Released: the synth is left as a fresh open would find it (sound off, controllers, programs,
        // banks, volume, pan and the pitch bend range at their defaults) for whatever opens it next
        ~SharedOutput() override
        {
            m_pOut->AllNotesOff();
            uint32_t a[16 * 14];
            size_t n = 0;
            for ( uint32_t ch = 0; ch < 16; ch++ )
            {
                auto CC = [&]( uint32_t cc, uint32_t v ) { a[n++] = 0xB0 | ch | ( cc << 8 ) | ( v << 16 ); };
                CC( 120, 0 );                           // all sound off
                CC( 121, 0 );                           // reset all controllers (pitch bend, modulation, expression...)
                CC( 101, 0 ); CC( 100, 0 ); CC( 6, 2 ); CC( 38, 0 );  // pitch bend range: 2 semitones
                CC( 101, 127 ); CC( 100, 127 );         // RPN null
                CC( 7, 100 ); CC( 10, 64 );
                CC( 0, 0 ); CC( 32, 0 );
                a[n++] = 0xC0 | ch;                     // program 0
            }
            m_pOut->SendBatch( a, n );
            m_pOut->Flush();
        }
        void Send( uint32_t uMsg ) override { m_pOut->Send( uMsg ); }
        void SendBatch( const uint32_t *pMsgs, size_t n ) override { m_pOut->SendBatch( pMsgs, n ); }
        void Flush() override { m_pOut->Flush(); }
        bool SendSysEx( const uint8_t *pData, size_t n ) override { return m_pOut->SendSysEx( pData, n ); }
        void SetVolume( double dVolume ) override { m_pOut->SetVolume( dVolume ); }
        void ResetPlaybackState() override { m_pOut->ResetPlaybackState(); }
        const std::wstring &Name() const override { return m_pOut->Name(); }
        bool IsKdmapi() const override { return m_pOut->IsKdmapi(); }

    private:
        std::shared_ptr< MidiOutput > m_pOut;
    };

    std::wstring DeviceName( int iDevice )
    {
        MIDIOUTCAPSW moc;
        return iDevice >= 0 && midiOutGetDevCapsW( iDevice, &moc, sizeof( moc ) ) == MMSYSERR_NOERROR ? std::wstring( moc.szPname ) : std::wstring();
    }
}

void MidiOutputs::CloseShared()
{
    Shared &s = TheShared();
    std::lock_guard< std::mutex > lock( s.mx );
    s.pOut.reset();
}

std::unique_ptr< MidiOutput > MidiOutputs::OpenConfigured( bool bTuning31, bool bPitchBend )
{
    Config &config = Config::GetConfig();
    int iDevice;
    {
        std::lock_guard< std::mutex > lock( config.AudioMutex() );
        iDevice = config.GetAudioSettings().iOutDevice;
    }
    bool bKdmapi = config.GetPerformanceSettings().bPreferKDMAPI && KdmapiAvailable();
    std::unique_ptr< MidiOutput > output;
    {
        Shared &s = TheShared();
        std::lock_guard< std::mutex > lock( s.mx );
        // Kept when it is still the one configured (a WinMM device by its index and name: the list can
        // change when devices come and go)
        std::wstring sDevice = bKdmapi ? std::wstring() : DeviceName( iDevice );
        if ( s.pOut && ( s.bKdmapi != bKdmapi || ( !bKdmapi && ( s.iDevice != iDevice || s.sDevice != sDevice ) ) ) )
            s.pOut.reset();     // (closed before the new one opens: KDMAPI is one stream)
        if ( !s.pOut )
        {
            std::unique_ptr< MidiOutput > p;
            if ( bKdmapi ) p = OpenKdmapi();
            if ( !p ) { bKdmapi = false; sDevice = DeviceName( iDevice ); p = OpenWinMM( iDevice ); }
            if ( p )
            {
                s.pOut = std::move( p );
                s.bKdmapi = bKdmapi;
                s.iDevice = iDevice;
                s.sDevice = sDevice;
            }
        }
        if ( s.pOut ) output = std::make_unique< SharedOutput >( s.pOut );
    }
    if ( !output || !bTuning31 ) return output;
    if ( bPitchBend ) return std::make_unique< PitchBendTunedOutput >( std::move( output ) );
    auto tuned = std::make_unique< ScaleTunedOutput >( std::move( output ) );
    if ( !tuned->Init() ) return nullptr;
    return tuned;
}
