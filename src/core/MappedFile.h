// Read-only memory-mapped file, plus storage detection for the loader's I/O strategy (DESIGN.md 4.0).
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

class MappedFile
{
public:
    ~MappedFile() { Close(); }

    bool Open( const std::wstring &sPath, std::wstring &sError );
    void Close();

    const uint8_t *Data() const { return m_pData; }
    uint64_t Size() const { return m_llSize; }
    bool SeekPenalty() const { return m_bSeekPenalty; }       // the volume is a spinning disk
    const std::wstring &Path() const { return m_sPath; }

    // Asks the OS to read the whole view in (SSD path)
    void PrefetchAll() const;

    // HDD path: reads the file front to back in large sequential chunks, filling the system cache,
    // and publishes how far it got. Workers wait on Resident() instead of faulting pages out of order.
    void StartSequentialRead( std::atomic< bool > *pCancel );
    void StopSequentialRead();
    uint64_t Resident() const { return m_llResident.load( std::memory_order_acquire ); }
    void WaitResident( uint64_t llEnd, std::atomic< bool > *pCancel ) const;
    long long ReaderMicroSecs() const { return m_llReaderUs.load( std::memory_order_relaxed ); } // time spent reading

private:
    static DWORD WINAPI ReaderThread( LPVOID p );
    static bool DetectSeekPenalty( const std::wstring &sPath );

    std::wstring m_sPath;
    HANDLE m_hFile = INVALID_HANDLE_VALUE;
    HANDLE m_hMap = nullptr;
    const uint8_t *m_pData = nullptr;
    uint64_t m_llSize = 0;
    bool m_bSeekPenalty = false;

    HANDLE m_hReader = nullptr;
    std::atomic< uint64_t > m_llResident{ 0 };
    std::atomic< bool > m_bStopReader{ false };
    std::atomic< bool > *m_pCancel = nullptr;
    std::atomic< long long > m_llReaderUs{ 0 };
};
