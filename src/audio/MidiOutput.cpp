#include "audio/MidiOutput.h"

#include <mmsystem.h>

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
        typedef BOOL ( WINAPI *PFN_VER )( DWORD *, DWORD *, DWORD *, DWORD * );
        HMODULE hMod = nullptr;
        PFN_BOOL pIsAvailable = nullptr, pInit = nullptr, pTerminate = nullptr;
        PFN_SEND pSend = nullptr;
        PFN_VER pVersion = nullptr;
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
                k.pVersion = ( PFN_VER )GetProcAddress( k.hMod, "ReturnKDMAPIVer" );
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

    private:
        Kdmapi::PFN_SEND m_pSend = nullptr;
        bool m_bOpen = false;
    };
}

bool MidiOutputs::KdmapiAvailable() { return Kdmapi::Get().bAvailable; }
std::wstring MidiOutputs::KdmapiName() { return L"KDMAPI (OmniMIDI)"; }

std::wstring MidiOutputs::KdmapiStatus()
{
    Kdmapi &k = Kdmapi::Get();
    if ( !k.hMod ) return L"Not available: OmniMIDI.dll was not found in System32 or next to PFAviz++.exe.";
    if ( !k.bAvailable ) return L"Not available: OmniMIDI.dll was found but does not report KDMAPI as available.";
    std::wstring s = L"Available";
    DWORD a = 0, b = 0, c = 0, d = 0;
    if ( k.pVersion && k.pVersion( &a, &b, &c, &d ) )
    {
        wchar_t buf[64];
        swprintf( buf, 64, L" (version %lu.%lu.%lu.%lu)", a, b, c, d );
        s += buf;
    }
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

std::unique_ptr< MidiOutput > MidiOutputs::OpenConfigured()
{
    Config &config = Config::GetConfig();
    if ( config.GetPerformanceSettings().bPreferKDMAPI )
        if ( auto p = OpenKdmapi() ) return p;
    int iDevice;
    {
        std::lock_guard< std::mutex > lock( config.AudioMutex() );
        iDevice = config.GetAudioSettings().iOutDevice;
    }
    return OpenWinMM( iDevice );
}
