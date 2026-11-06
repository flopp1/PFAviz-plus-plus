// The master playback clock (DESIGN.md 7.1): T(now) = T0 + (now - t0) * speed, rebased on every
// pause, seek or speed change. The render thread samples it once per frame; the playback threads
// read it continuously, so audio is no longer quantised to frames.
#pragma once

#include <windows.h>
#include <atomic>
#include <vector>

class PlaybackClock
{
public:
    PlaybackClock() { InitializeSRWLock( &m_lock ); }

    long long Now() const;              // song time in microseconds (with a ramp on, the ramp's)
    long long At( long long llWall ) const;     // Now at that wall time (Timer::NowMicroSecs; a ramp past its last knot holds there)
    long long FreeNow() const;          // the same without the ramp
    long long FreeAt( long long llWall ) const;     // FreeNow at that wall time (Timer::NowMicroSecs)
    bool Paused() const;
    double Speed() const;
    unsigned Epoch() const { return m_uEpoch.load( std::memory_order_acquire ); }

    void Set( long long llTime );       // seek: also bumps the epoch
    void SetPaused( bool bPaused );
    void SetSpeed( double dSpeed );

    // The PFA lag simulation (song/PfaLag.h): Now() goes through the knots (wall time, song time) in
    // straight lines and stays at the last, as the original sends a frame's events while its Logic
    // runs (slowly where they cost the most), then draws. Set (a seek) holds it at the new time until
    // the next ramp. EndRamp: Now() is FreeNow() again. While a ramp is on, playback reads a time
    // inside a millisecond as a share of that millisecond's events (PlaybackDecoder::Params::bShares),
    // as the ramp's knots are placed
    struct RampKnot { long long llWall, llSong; };
    static const int MaxRampKnots = 16384;     // (the simulation's frames a quarter second ahead)
    void SetRamp( const RampKnot *pKnots, int nKnots );
    void SetRamp( long long llWall0, long long llWall1, long long llFrom, long long llTo );
    void EndRamp();
    bool Ramped() const;

private:
    long long NowLocked( long long llWall ) const;

    mutable SRWLOCK m_lock;
    long long m_llBase = 0, m_llWallBase = 0;
    double m_dSpeed = 1.0;
    bool m_bPaused = true;
    bool m_bRamp = false;
    std::vector< RampKnot > m_aRamp = std::vector< RampKnot >( 1 );
    int m_nRamp = 0;
    std::atomic< unsigned > m_uEpoch{ 0 };
};
