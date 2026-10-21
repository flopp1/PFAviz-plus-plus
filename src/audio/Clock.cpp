#include "audio/Clock.h"

#include "core/Util.h"

long long PlaybackClock::NowLocked( long long llWall ) const
{
    if ( m_bPaused ) return m_llBase;
    return m_llBase + static_cast< long long >( ( llWall - m_llWallBase ) * m_dSpeed + 0.5 );
}

long long PlaybackClock::Now() const
{
    long long llWall = Timer::NowMicroSecs();
    AcquireSRWLockShared( &m_lock );
    long long t = NowLocked( llWall );
    ReleaseSRWLockShared( &m_lock );
    return t;
}

bool PlaybackClock::Paused() const
{
    AcquireSRWLockShared( &m_lock );
    bool b = m_bPaused;
    ReleaseSRWLockShared( &m_lock );
    return b;
}

double PlaybackClock::Speed() const
{
    AcquireSRWLockShared( &m_lock );
    double d = m_dSpeed;
    ReleaseSRWLockShared( &m_lock );
    return d;
}

void PlaybackClock::Set( long long llTime )
{
    AcquireSRWLockExclusive( &m_lock );
    m_llBase = llTime;
    m_llWallBase = Timer::NowMicroSecs();
    m_uEpoch.fetch_add( 1, std::memory_order_acq_rel );
    ReleaseSRWLockExclusive( &m_lock );
}

void PlaybackClock::SetPaused( bool bPaused )
{
    AcquireSRWLockExclusive( &m_lock );
    long long llWall = Timer::NowMicroSecs();
    m_llBase = NowLocked( llWall );
    m_llWallBase = llWall;
    m_bPaused = bPaused;
    ReleaseSRWLockExclusive( &m_lock );
}

void PlaybackClock::SetSpeed( double dSpeed )
{
    AcquireSRWLockExclusive( &m_lock );
    long long llWall = Timer::NowMicroSecs();
    m_llBase = NowLocked( llWall );
    m_llWallBase = llWall;
    m_dSpeed = dSpeed;
    ReleaseSRWLockExclusive( &m_lock );
}
