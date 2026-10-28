#include "audio/MidiOutput.h"
#include "audio/Tuning31.h"
#include "audio/PitchBendTuning.h"

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <climits>

#include "core/Config.h"

void MidiOutput::AllNotesOff()
{
    for ( uint32_t ch = 0; ch < 16; ch++ ) Send( 0xB0 | ch | ( 0x7B << 8 ) );  // All notes off
    for ( uint32_t ch = 0; ch < 16; ch++ ) Send( 0xB0 | ch | ( 0x40 << 8 ) );  // Sustain off
}

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
    std::atomic< size_t > g_iKdmapiLongBytes{ 65535 };

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
        void Send( uint32_t uMsg ) override { Flush(); m_pSend( uMsg ); }
        // Long sends: channel messages as raw bytes with running status, gathered across batches until
        // the buffer fills or Flush (the sender flushes before it waits, so a buffer only grows while it
        // is behind: on time, a batch is a message). Anything else goes short, in its place
        void SendBatch( const uint32_t *pMsgs, size_t n ) override
        {
            Kdmapi::PFN_SEND pSend = m_pSend;
            if ( !g_bKdmapiLong.load( std::memory_order_relaxed ) || m_bLongFailed )
            {
                Flush();
                for ( size_t i = 0; i < n; i++ ) pSend( pMsgs[i] );
                return;
            }
            // The size, as set now (a smaller one sends what is gathered first)
            size_t iMax = g_iKdmapiLongBytes.load( std::memory_order_relaxed );
            if ( m_iLong > iMax ) Flush();
            if ( m_vLong.size() < iMax ) m_vLong.resize( iMax );
            uint8_t *pLong = m_vLong.data();
            for ( size_t i = 0; i < n; i++ )
            {
                uint32_t m = pMsgs[i];
                unsigned st = m & 0xFF;
                if ( st < 0x80 || st >= 0xF0 ) { Flush(); pSend( m ); continue; }
                bool bTwo = ( st & 0xE0 ) == 0xC0;  // program change and channel pressure: one data byte
                if ( m_iLong + ( bTwo ? 2 : 3 ) > iMax ) Flush();
                if ( m_bLongFailed ) { pSend( m ); continue; }
                if ( st != m_uRun ) { pLong[m_iLong++] = (uint8_t)st; m_uRun = st; }
                pLong[m_iLong++] = (uint8_t)( ( m >> 8 ) & 0x7F );
                if ( !bTwo ) pLong[m_iLong++] = (uint8_t)( ( m >> 16 ) & 0x7F );
            }
        }
        void Flush() override
        {
            if ( !m_iLong ) return;
            SendLongBuffer( m_iLong );      // (not taken: these events are lost, once; short sends after)
            m_iLong = 0;
            m_uRun = 0;
        }
        const std::wstring &Name() const override { static const std::wstring s = MidiOutputs::KdmapiName(); return s; }
        bool IsKdmapi() const override { return true; }
        bool SendSysEx( const uint8_t *pData, size_t n ) override
        {
            Flush();
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
        std::vector< uint8_t > m_vLong;         // the long message being gathered (as large as the size set)
        size_t m_iLong = 0;                     // bytes gathered
        unsigned m_uRun = 0;                    // the running status in them

        bool SendLongBuffer( size_t len )
        {
            Kdmapi &k = Kdmapi::Get();
            if ( !k.pLong || !k.pPrepare || !k.pUnprepare ) { m_bLongFailed = true; return false; }
            MIDIHDR hdr = {};
            hdr.lpData = (LPSTR)m_vLong.data();
            hdr.dwBufferLength = hdr.dwBytesRecorded = (DWORD)len;
            if ( k.pPrepare( &hdr, sizeof( hdr ) ) != MMSYSERR_NOERROR ) { m_bLongFailed = true; return false; }
            bool bSent = k.pLong( &hdr, sizeof( hdr ) ) == MMSYSERR_NOERROR;
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

    class TunedOutput : public MidiOutput
    {
    public:
        explicit TunedOutput( std::unique_ptr< MidiOutput > p ) : m_pOut( std::move( p ) ) {}
        ~TunedOutput() override
        {
            m_pOut->AllNotesOff();
            for ( const auto &message : Tuning31::ScaleMessages )
            {
                uint8_t neutral[33];
                std::copy( message, message + 33, neutral );
                for ( size_t i = 8; i < 32; i += 2 ) { neutral[i] = 0x40; neutral[i + 1] = 0; }
                m_pOut->SendSysEx( neutral, sizeof( neutral ) );
            }
        }
        bool Init()
        {
            for ( const auto &message : Tuning31::ScaleMessages )
                if ( !m_pOut->SendSysEx( message, sizeof( message ) ) ) return false;
            return true;
        }
        void Send( uint32_t message ) override
        {
            unsigned type = message & 0xF0, channel = message & 0x0F;
            if ( type == 0x80 || type == 0x90 || type == 0xA0 )
            {
                auto pitch = Tuning31::Pitch( ( message >> 8 ) & 0xFF, channel );
                if ( pitch.second < 128 ) m_pOut->Send( ( message & ~0xFF0Fu ) | pitch.first | ( uint32_t( pitch.second ) << 8 ) );
            }
            else if ( channel != 9 && type >= 0xB0 && type <= 0xE0 )
            {
                unsigned base = Tuning31::GroupBase( channel );
                for ( unsigned lane = 0; lane < 3; lane++ ) m_pOut->Send( ( message & ~0x0Fu ) | ( base + lane ) );
            }
            else m_pOut->Send( message );
        }
        void SetVolume( double volume ) override { m_pOut->SetVolume( volume ); }
        const std::wstring &Name() const override { return m_pOut->Name(); }
        bool IsKdmapi() const override { return m_pOut->IsKdmapi(); }

    private:
        std::unique_ptr< MidiOutput > m_pOut;
    };
}

bool MidiOutputs::KdmapiAvailable() { return Kdmapi::Get().bAvailable; }
void MidiOutputs::SetKdmapiLongSends( bool bOn, int iMaxBytes )
{
    g_iKdmapiLongBytes.store( (size_t)std::max( 3, iMaxBytes ), std::memory_order_relaxed );
    g_bKdmapiLong.store( bOn, std::memory_order_relaxed );
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
    if ( !k.hMod ) return L"Not available: OmniMIDI.dll was not found in System32 or next to PFAviz++.exe.";
    if ( !k.bAvailable ) return L"Not available: OmniMIDI.dll was found but does not report KDMAPI as available.";
    std::wstring s = L"Available";
    s += L". It is used instead of the device above when \"Prefer KDMAPI\" is checked in the Performance tab.";
    return s;
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

std::unique_ptr< MidiOutput > MidiOutputs::OpenConfigured( bool bTuning31, bool bPitchBend )
{
    Config &config = Config::GetConfig();
    std::unique_ptr< MidiOutput > output;
    if ( config.GetPerformanceSettings().bPreferKDMAPI ) output = OpenKdmapi();
    if ( !output )
    {
        int iDevice;
        {
            std::lock_guard< std::mutex > lock( config.AudioMutex() );
            iDevice = config.GetAudioSettings().iOutDevice;
        }
        output = OpenWinMM( iDevice );
    }
    if ( !output || !bTuning31 ) return output;
    if ( bPitchBend ) return std::make_unique< PitchBendTunedOutput >( std::move( output ) );
    auto tuned = std::make_unique< TunedOutput >( std::move( output ) );
    if ( !tuned->Init() ) return nullptr;
    return tuned;
}
