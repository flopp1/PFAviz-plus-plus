// The master playback clock (DESIGN.md 7.1): T(now) = T0 + (now - t0) * speed, rebased on every
// pause, seek or speed change. The render thread samples it once per frame; the playback threads
// read it continuously, so audio is no longer quantised to frames.
#pragma once

#include <windows.h>
#include <atomic>

class PlaybackClock
{
public:
    PlaybackClock() { InitializeSRWLock( &m_lock ); }

    long long Now() const;              // song time in microseconds
    bool Paused() const;
    double Speed() const;
    unsigned Epoch() const { return m_uEpoch.load( std::memory_order_acquire ); }

    void Set( long long llTime );       // seek: also bumps the epoch
    void SetPaused( bool bPaused );
    void SetSpeed( double dSpeed );

private:
    long long NowLocked( long long llWall ) const;

    mutable SRWLOCK m_lock;
    long long m_llBase = 0, m_llWallBase = 0;
    double m_dSpeed = 1.0;
    bool m_bPaused = true;
    std::atomic< unsigned > m_uEpoch{ 0 };
};
