// Small helpers: timer, colour conversion, string conversion, single-producer queue.
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

// High-precision timer (QueryPerformanceCounter). Replaces the original's 1 ms timeGetTime timer.
class Timer
{
public:
    Timer();
    void Start();
    long long GetMicroSecs() const;   // elapsed since Start()
    static long long NowMicroSecs();

private:
    long long m_llStart = 0;
    bool m_bStarted = false;
};

namespace Util
{
    std::wstring Utf8ToWide( const std::string &s );
    std::string WideToUtf8( const std::wstring &s );

    // Exact copies of the original colour helpers: note colours depend on their integer rounding.
    unsigned RandColor();
    void RGBtoHSV( int R, int G, int B, int &H, int &S, int &V );
    void HSVtoRGB( int H, int S, int V, int &R, int &G, int &B );
    void CommaPrintf( wchar_t buf[32], long long iVal );

    // The process's private memory in RAM (Task Manager's "Memory" column): the private working set.
    // Not the commit charge, which counts memory reserved but never touched: OmniMIDI sized from RAM
    // commits half of it (16 GB here) at startup. Falls back to the commit charge before Windows 10 1809.
    uint64_t ProcessMemory();
}

// Lock-free queue for one producer thread and one consumer thread.
template < typename T, int Size = 1024 >
class SpscQueue
{
public:
    bool Push( const T &t )
    {
        int iWrite = m_iWrite.load( std::memory_order_relaxed );
        int iNext = ( iWrite + 1 ) % Size;
        if ( iNext == m_iRead.load( std::memory_order_acquire ) ) return false;
        m_aData[iWrite] = t;
        m_iWrite.store( iNext, std::memory_order_release );
        return true;
    }
    bool Pop( T &t )
    {
        int iRead = m_iRead.load( std::memory_order_relaxed );
        if ( iRead == m_iWrite.load( std::memory_order_acquire ) ) return false;
        t = m_aData[iRead];
        m_iRead.store( ( iRead + 1 ) % Size, std::memory_order_release );
        return true;
    }
    void ForcePush( const T &t ) { while ( !Push( t ) ) Sleep( 0 ); }

private:
    T m_aData[Size];
    std::atomic< int > m_iWrite{ 0 }, m_iRead{ 0 };
};
