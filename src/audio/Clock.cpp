#include "audio/Clock.h"

#include <algorithm>

#include "core/Util.h"

long long PlaybackClock::NowLocked( long long llWall ) const
{
    if ( m_bPaused ) return m_llBase;
    return m_llBase + static_cast< long long >( ( llWall - m_llWallBase ) * m_dSpeed + 0.5 );
}

long long PlaybackClock::Now() const
{
    return At( Timer::NowMicroSecs() );
}

long long PlaybackClock::At( long long llWall ) const
{
    AcquireSRWLockShared( &m_lock );
    long long t;
    if ( !m_bRamp ) t = NowLocked( llWall );
    else if ( m_nRamp <= 0 ) t = m_llBase;
    else if ( llWall <= m_aRamp[0].llWall ) t = m_aRamp[0].llSong;
    else if ( llWall >= m_aRamp[m_nRamp - 1].llWall ) t = m_aRamp[m_nRamp - 1].llSong;
    else
    {
        // The first knot past llWall (knots ascend)
        int lo = 1, hi = m_nRamp - 1;
        while ( lo < hi )
        {
            int mid = ( lo + hi ) / 2;
            if ( llWall < m_aRamp[mid].llWall ) hi = mid; else lo = mid + 1;
        }
        const RampKnot &a = m_aRamp[lo - 1], &b = m_aRamp[lo];
        t = a.llSong + (long long)( (double)( b.llSong - a.llSong ) * ( llWall - a.llWall ) / ( b.llWall - a.llWall ) );
    }
    ReleaseSRWLockShared( &m_lock );
    return t;
}

long long PlaybackClock::FreeNow() const
{
    long long llWall = Timer::NowMicroSecs();
    AcquireSRWLockShared( &m_lock );
    long long t = NowLocked( llWall );
    ReleaseSRWLockShared( &m_lock );
    return t;
}

long long PlaybackClock::FreeAt( long long llWall ) const
{
    AcquireSRWLockShared( &m_lock );
    long long t = NowLocked( llWall );
    ReleaseSRWLockShared( &m_lock );
    return t;
}

void PlaybackClock::SetRamp( const RampKnot *pKnots, int nKnots )
{
    AcquireSRWLockExclusive( &m_lock );
    m_bRamp = true;
    m_nRamp = std::min( std::max( nKnots, 0 ), MaxRampKnots );
    if ( (int)m_aRamp.size() < std::max( m_nRamp, 1 ) ) m_aRamp.resize( std::max( m_nRamp, 1 ) );
    for ( int i = 0; i < m_nRamp; i++ )
    {
        m_aRamp[i] = pKnots[i];
        // (both ascending)
        if ( i )
        {
            m_aRamp[i].llWall = std::max( m_aRamp[i].llWall, m_aRamp[i - 1].llWall );
            m_aRamp[i].llSong = std::max( m_aRamp[i].llSong, m_aRamp[i - 1].llSong );
        }
    }
    ReleaseSRWLockExclusive( &m_lock );
}

void PlaybackClock::SetRamp( long long llWall0, long long llWall1, long long llFrom, long long llTo )
{
    const RampKnot a[2] = { { llWall0, llFrom }, { llWall1, llTo } };
    SetRamp( a, 2 );
}

void PlaybackClock::EndRamp()
{
    AcquireSRWLockExclusive( &m_lock );
    m_bRamp = false;
    ReleaseSRWLockExclusive( &m_lock );
}

bool PlaybackClock::Ramped() const
{
    AcquireSRWLockShared( &m_lock );
    bool b = m_bRamp;
    ReleaseSRWLockShared( &m_lock );
    return b;
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
    m_aRamp[0] = { m_llWallBase, llTime };
    m_nRamp = 1;
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
