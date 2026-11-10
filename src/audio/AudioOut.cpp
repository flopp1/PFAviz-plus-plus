#include "audio/AudioOut.h"

#include <avrt.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstring>

#include "core/Util.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace
{
    const GUID SubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

    template < class T > void Release( T *&p ) { if ( p ) { p->Release(); p = nullptr; } }

    struct Device
    {
        IMMDeviceEnumerator *pEnum = nullptr;
        IMMDevice *pDevice = nullptr;
        IAudioClient *pClient = nullptr;
        IAudioRenderClient *pRender = nullptr;
        UINT32 uBuffer = 0;
        unsigned uRate = 0;
        long long llLatencyUs = 0;

        void Close()
        {
            if ( pClient ) pClient->Stop();
            Release( pRender );
            Release( pClient );
            Release( pDevice );
            Release( pEnum );
        }
        HRESULT Open( HANDLE hEvent )
        {
            HRESULT hr = CoCreateInstance( __uuidof( MMDeviceEnumerator ), nullptr, CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ), (void **)&pEnum );
            if ( FAILED( hr ) ) return hr;
            if ( FAILED( hr = pEnum->GetDefaultAudioEndpoint( eRender, eConsole, &pDevice ) ) ) return hr;
            if ( FAILED( hr = pDevice->Activate( __uuidof( IAudioClient ), CLSCTX_ALL, nullptr, (void **)&pClient ) ) ) return hr;
            WAVEFORMATEX *pMix = nullptr;
            if ( FAILED( hr = pClient->GetMixFormat( &pMix ) ) ) return hr;
            uRate = pMix->nSamplesPerSec;
            CoTaskMemFree( pMix );

            // Float stereo at the device's rate; Windows converts the channels when it has more
            WAVEFORMATEXTENSIBLE f = {};
            f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            f.Format.nChannels = 2;
            f.Format.nSamplesPerSec = uRate;
            f.Format.wBitsPerSample = 32;
            f.Format.nBlockAlign = 8;
            f.Format.nAvgBytesPerSec = uRate * 8;
            f.Format.cbSize = sizeof( WAVEFORMATEXTENSIBLE ) - sizeof( WAVEFORMATEX );
            f.Samples.wValidBitsPerSample = 32;
            f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            f.SubFormat = SubtypeFloat;
            const REFERENCE_TIME hnsBuffer = 400000;    // 40 ms
            hr = pClient->Initialize( AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      hnsBuffer, 0, &f.Format, nullptr );
            if ( FAILED( hr ) ) return hr;
            if ( FAILED( hr = pClient->SetEventHandle( hEvent ) ) ) return hr;
            if ( FAILED( hr = pClient->GetBufferSize( &uBuffer ) ) ) return hr;
            if ( FAILED( hr = pClient->GetService( __uuidof( IAudioRenderClient ), (void **)&pRender ) ) ) return hr;
            REFERENCE_TIME hnsLatency = 0;
            pClient->GetStreamLatency( &hnsLatency );
            llLatencyUs = hnsLatency / 10;
            // Silence first, so the device starts with something
            BYTE *p = nullptr;
            if ( SUCCEEDED( pRender->GetBuffer( uBuffer, &p ) ) ) pRender->ReleaseBuffer( uBuffer, AUDCLNT_BUFFERFLAGS_SILENT );
            return pClient->Start();
        }
    };
}

bool AudioOut::Start( FillFn fn, std::wstring &sError )
{
    if ( m_Thread.joinable() ) return true;
    m_Fill = std::move( fn );
    m_bStop = false;
    m_sError.clear();
    m_hReady = CreateEventW( nullptr, TRUE, FALSE, nullptr );
    m_Thread = std::thread( &AudioOut::Thread, this );
    WaitForSingleObject( m_hReady, 3000 );
    if ( m_uRate.load() == 0 )
    {
        sError = m_sError.empty() ? std::wstring( L"the sound device did not open" ) : m_sError;
        return false;
    }
    return true;
}

void AudioOut::Stop()
{
    m_bStop = true;
    if ( m_Thread.joinable() ) m_Thread.join();
    if ( m_hReady ) { CloseHandle( m_hReady ); m_hReady = nullptr; }
}

void AudioOut::Thread()
{
    CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    DWORD dwTask = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW( L"Pro Audio", &dwTask );
    HANDLE hEvent = CreateEventW( nullptr, FALSE, FALSE, nullptr );
    Device d;
    bool bOpen = false;
    while ( !m_bStop.load( std::memory_order_relaxed ) )
    {
        if ( !bOpen )
        {
            HRESULT hr = d.Open( hEvent );
            if ( FAILED( hr ) )
            {
                d.Close();
                if ( m_uRate.load() == 0 && m_sError.empty() )
                {
                    wchar_t s[96];
                    swprintf( s, 96, L"the sound device did not open (0x%08lX)", (unsigned long)hr );
                    m_sError = s;
                    SetEvent( m_hReady );
                }
                Sleep( 500 );
                continue;
            }
            bOpen = true;
            m_uRate.store( d.uRate, std::memory_order_release );
            SetEvent( m_hReady );
        }
        WaitForSingleObject( hEvent, 100 );
        UINT32 uPadding = 0;
        HRESULT hr = d.pClient->GetCurrentPadding( &uPadding );
        if ( SUCCEEDED( hr ) )
        {
            UINT32 n = d.uBuffer - uPadding;
            BYTE *p = nullptr;
            if ( n > 0 && SUCCEEDED( hr = d.pRender->GetBuffer( n, &p ) ) )
            {
                long long llWall = Timer::NowMicroSecs() + (long long)uPadding * 1000000 / d.uRate + d.llLatencyUs;
                m_Fill( reinterpret_cast< float * >( p ), n, llWall, d.uRate );
                hr = d.pRender->ReleaseBuffer( n, 0 );
            }
        }
        if ( FAILED( hr ) )
        {
            // (AUDCLNT_E_DEVICE_INVALIDATED and the like: the default device again)
            d.Close();
            bOpen = false;
            Sleep( 200 );
        }
    }
    d.Close();
    CloseHandle( hEvent );
    if ( hTask ) AvRevertMmThreadCharacteristics( hTask );
    CoUninitialize();
}

unsigned AudioOut::DeviceRate()
{
    IMMDeviceEnumerator *pEnum = nullptr;
    IMMDevice *pDevice = nullptr;
    IAudioClient *pClient = nullptr;
    unsigned uRate = 0;
    if ( SUCCEEDED( CoCreateInstance( __uuidof( MMDeviceEnumerator ), nullptr, CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ), (void **)&pEnum ) ) &&
         SUCCEEDED( pEnum->GetDefaultAudioEndpoint( eRender, eConsole, &pDevice ) ) &&
         SUCCEEDED( pDevice->Activate( __uuidof( IAudioClient ), CLSCTX_ALL, nullptr, (void **)&pClient ) ) )
    {
        WAVEFORMATEX *pMix = nullptr;
        if ( SUCCEEDED( pClient->GetMixFormat( &pMix ) ) ) { uRate = pMix->nSamplesPerSec; CoTaskMemFree( pMix ); }
    }
    if ( pClient ) pClient->Release();
    if ( pDevice ) pDevice->Release();
    if ( pEnum ) pEnum->Release();
    return uRate;
}
