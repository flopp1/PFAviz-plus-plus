#include "core/MappedFile.h"

#include <winioctl.h>
#include <vector>

bool MappedFile::Open( const std::wstring &sPath, std::wstring &sError )
{
    Close();
    m_sPath = sPath;
    m_hFile = CreateFileW( sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr );
    if ( m_hFile == INVALID_HANDLE_VALUE ) { sError = L"Could not open the file."; return false; }
    LARGE_INTEGER li;
    if ( !GetFileSizeEx( m_hFile, &li ) || li.QuadPart <= 0 ) { sError = L"The file is empty."; Close(); return false; }
    m_llSize = (uint64_t)li.QuadPart;
    m_hMap = CreateFileMappingW( m_hFile, nullptr, PAGE_READONLY, 0, 0, nullptr );
    if ( !m_hMap ) { sError = L"Could not map the file."; Close(); return false; }
    m_pData = ( const uint8_t * )MapViewOfFile( m_hMap, FILE_MAP_READ, 0, 0, 0 );
    if ( !m_pData ) { sError = L"Could not map a view of the file (out of address space?)."; Close(); return false; }
    m_bSeekPenalty = DetectSeekPenalty( sPath );
    m_llResident = 0;
    return true;
}

void MappedFile::Close()
{
    StopSequentialRead();
    if ( m_hPrefetch ) { WaitForSingleObject( m_hPrefetch, INFINITE ); CloseHandle( m_hPrefetch ); m_hPrefetch = nullptr; }
    if ( m_pData ) UnmapViewOfFile( m_pData );
    if ( m_hMap ) CloseHandle( m_hMap );
    if ( m_hFile != INVALID_HANDLE_VALUE ) CloseHandle( m_hFile );
    m_pData = nullptr;
    m_hMap = nullptr;
    m_hFile = INVALID_HANDLE_VALUE;
    m_llSize = 0;
}

void MappedFile::PrefetchAll()
{
    if ( !m_hPrefetch ) m_hPrefetch = CreateThread( nullptr, 0, PrefetchThread, this, 0, nullptr );
}

DWORD WINAPI MappedFile::PrefetchThread( LPVOID p )
{
    const MappedFile *pThis = static_cast< const MappedFile * >( p );
    WIN32_MEMORY_RANGE_ENTRY e;
    e.VirtualAddress = ( PVOID )pThis->m_pData;
    e.NumberOfBytes = (SIZE_T)pThis->m_llSize;
    PrefetchVirtualMemory( GetCurrentProcess(), 1, &e, 0 );
    return 0;
}

bool MappedFile::DetectSeekPenalty( const std::wstring &sPath )
{
    wchar_t sVolume[MAX_PATH];
    if ( !GetVolumePathNameW( sPath.c_str(), sVolume, MAX_PATH ) ) return false;
    std::wstring sDevice = sVolume;
    if ( sDevice.size() < 2 || sDevice[1] != L':' ) return false; // UNC or mount point: assume no penalty
    sDevice = L"\\\\.\\" + sDevice.substr( 0, 2 );
    HANDLE h = CreateFileW( sDevice.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
    if ( h == INVALID_HANDLE_VALUE ) return false;
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d = {};
    DWORD dwBytes = 0;
    bool bPenalty = false;
    if ( DeviceIoControl( h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof( q ), &d, sizeof( d ), &dwBytes, nullptr ) && dwBytes >= sizeof( d ) )
        bPenalty = d.IncursSeekPenalty != FALSE;
    CloseHandle( h );
    return bPenalty;
}

void MappedFile::StartSequentialRead( std::atomic< bool > *pCancel )
{
    StopSequentialRead();
    m_pCancel = pCancel;
    m_bStopReader = false;
    m_llResident = 0;
    m_llReaderUs = 0;
    m_hReader = CreateThread( nullptr, 0, ReaderThread, this, 0, nullptr );
    if ( !m_hReader ) m_llResident = m_llSize; // fall back to demand paging
}

void MappedFile::StopSequentialRead()
{
    if ( !m_hReader ) return;
    m_bStopReader = true;
    WaitForSingleObject( m_hReader, INFINITE );
    CloseHandle( m_hReader );
    m_hReader = nullptr;
}

// Reads through a separate handle into a scratch buffer: this fills the system cache that the
// mapped view shares, with large sequential requests the disk can stream.
DWORD WINAPI MappedFile::ReaderThread( LPVOID p )
{
    MappedFile *pThis = static_cast< MappedFile * >( p );
    // The parse workers keep every core busy; the reader must still issue its next request at once
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_HIGHEST );
    const DWORD dwChunk = 32u << 20;
    // No FILE_FLAG_SEQUENTIAL_SCAN: with it, pages read here are the first the system reuses when
    // memory is short, and the parse workers, which reach a track a little later, fault them back
    // in with small random reads that fight this stream for the disk head
    HANDLE h = CreateFileW( pThis->m_sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr );
    if ( h == INVALID_HANDLE_VALUE ) { pThis->m_llResident = pThis->m_llSize; return 0; }
    std::vector< uint8_t > vBuf( dwChunk );
    uint64_t llDone = 0;
    while ( llDone < pThis->m_llSize && !pThis->m_bStopReader && !( pThis->m_pCancel && *pThis->m_pCancel ) )
    {
        DWORD dwGot = 0;
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter( &t0 );
        bool bOk = ReadFile( h, vBuf.data(), dwChunk, &dwGot, nullptr ) && dwGot;
        QueryPerformanceCounter( &t1 );
        QueryPerformanceFrequency( &f );
        pThis->m_llReaderUs += ( t1.QuadPart - t0.QuadPart ) * 1000000 / f.QuadPart;
        if ( !bOk ) break;
        llDone += dwGot;
        pThis->m_llResident.store( llDone, std::memory_order_release );
    }
    CloseHandle( h );
    pThis->m_llResident.store( pThis->m_llSize, std::memory_order_release ); // anything left is demand-paged
    return 0;
}

void MappedFile::WaitResident( uint64_t llEnd, std::atomic< bool > *pCancel ) const
{
    while ( Resident() < llEnd && !( pCancel && *pCancel ) )
        Sleep( 1 );
}
