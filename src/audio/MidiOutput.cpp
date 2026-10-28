#include "audio/MidiOutput.h"
#include "audio/Tuning31.h"

#include <mmsystem.h>

#include <algorithm>
#include <climits>

#include "core/Config.h"

void MidiOutput::AllNotesOff()
{
    for ( uint32_t ch = 0; ch < 16; ch++ ) Send( 0xB0 | ch | ( 0x7B << 8 ) );  // All notes off
    for ( uint32_t ch = 0; ch < 16; ch++ ) Send( 0xB0 | ch | ( 0x40 << 8 ) );  // Sustain off
}

namespace
{
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
            if ( result == MMSYSERR_NOERROR )
                while ( !( hdr.dwFlags & MHDR_DONE ) ) Sleep( 1 );
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
        typedef UINT ( WINAPI *PFN_LONG )( MIDIHDR * );
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
        void SendBatch( const uint32_t *pMsgs, size_t n ) override
        {
            Kdmapi::PFN_SEND pSend = m_pSend;
            for ( size_t i = 0; i < n; i++ ) pSend( pMsgs[i] );
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
            if ( k.pPrepare( &hdr ) != MMSYSERR_NOERROR ) return false;
            UINT result = k.pLong( &hdr );
            if ( result == MMSYSERR_NOERROR )
                while ( !( hdr.dwFlags & MHDR_DONE ) ) Sleep( 1 );
            k.pUnprepare( &hdr );
            return result == MMSYSERR_NOERROR;
        }

    private:
        Kdmapi::PFN_SEND m_pSend = nullptr;
        bool m_bOpen = false;
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
        void Send( uint32_t message ) override { m_pOut->Send( message ); }
        void SendBatch( const uint32_t *messages, size_t n ) override { m_pOut->SendBatch( messages, n ); }
        void SetVolume( double volume ) override { m_pOut->SetVolume( volume ); }
        const std::wstring &Name() const override { return m_pOut->Name(); }
        bool IsKdmapi() const override { return m_pOut->IsKdmapi(); }

    private:
        std::unique_ptr< MidiOutput > m_pOut;
    };
}

bool MidiOutputs::KdmapiAvailable() { return Kdmapi::Get().bAvailable; }
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

std::unique_ptr< MidiOutput > MidiOutputs::OpenConfigured( bool bTuning31 )
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
    auto tuned = std::make_unique< TunedOutput >( std::move( output ) );
    if ( !tuned->Init() ) return nullptr;
    return tuned;
}
