// Audio to the default sound device through WASAPI (shared mode, event driven, 32-bit float stereo at
// the device's own rate), for audio rendered ahead (RenderAhead.h). A thread of its own asks Fill for
// each stretch the device wants, saying when its first frame will be heard. When the device goes away
// (unplugged, or the default changed), it opens the default device again.
#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

class AudioOut
{
public:
    // pOut: n interleaved stereo frames to write; llWallUs: when the first of them will be heard
    // (Timer::NowMicroSecs's clock); uRate: the device's rate (it can change when the device does)
    using FillFn = std::function< void( float *pOut, unsigned n, long long llWallUs, unsigned uRate ) >;

    ~AudioOut() { Stop(); }
    // Opens the device on its thread and waits for it (up to 3 s); false with the reason when it fails
    bool Start( FillFn fn, std::wstring &sError );
    void Stop();
    unsigned Rate() const { return m_uRate.load( std::memory_order_acquire ); }
    // The default device's rate, as Start would play at (0 when there is none); COM started on the thread
    static unsigned DeviceRate();

private:
    void Thread();

    FillFn m_Fill;
    std::thread m_Thread;
    std::atomic< bool > m_bStop{ false };
    std::atomic< unsigned > m_uRate{ 0 };
    HANDLE m_hReady = nullptr;
    std::wstring m_sError;
};
