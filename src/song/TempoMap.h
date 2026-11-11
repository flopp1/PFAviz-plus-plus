// Tick -> microseconds with the exact integer arithmetic of the original (DESIGN.md R3), identical to
// RefMidi::TickToMicroSecs. Every valid SetTempo event starts a segment, even a redundant one.
// bExact (the Performance tab's "Exact tempo timing"): the original rounds each segment down to whole
// microseconds on its own, so a segment shorter than one (a tempo of 1 us a beat over 7 ticks at a division
// of 8) takes no time, and a song of millions of them collapses into an instant. Exact: the parts of a
// microsecond are carried from one segment to the next (llRem, in 1/iDivision us), so every tick's time is
// the exact sum rounded down once.
#pragma once

#include <algorithm>
#include <vector>

#include "midi/RefMidi.h"

struct TempoMap
{
    struct Seg { long long llTick, llMicroSec, llMicroSecsPerBeat; long long llRem = 0; };
    std::vector< Seg > vSegs;
    int iDivision = 0;
    bool bExact = false;

    long long At( long long llTick ) const
    {
        auto it = std::upper_bound( vSegs.begin(), vSegs.end(), llTick, []( long long t, const Seg &s ) { return t < s.llTick; } );
        return In( ( it == vSegs.begin() ) ? vSegs.front() : *( it - 1 ), llTick );
    }
    // The time of a tick in segment s
    long long In( const Seg &s, long long llTick ) const
    {
        if ( IsSmpteDivision( iDivision ) )
        {
            int iFramesPerSec100 = -( ( iDivision | static_cast< int >( 0xFFFF0000 ) ) >> 8 ) * 100;
            if ( iFramesPerSec100 == 2900 ) iFramesPerSec100 = 2997;
            long long llTicksPerSec100 = (long long)( iDivision & 0xFF ) * iFramesPerSec100;
            if ( llTicksPerSec100 <= 0 ) return s.llMicroSec;
            return s.llMicroSec + ( 100000000LL * ( llTick - s.llTick ) ) / llTicksPerSec100;
        }
        return s.llMicroSec + ( s.llRem + s.llMicroSecsPerBeat * ( llTick - s.llTick ) ) / iDivision;
    }

    // At() for n ticks in ascending order: one search, then the segments walked forwards
    void AtSorted( const uint64_t *pTicks, size_t n, uint64_t *pOut ) const
    {
        if ( !n ) return;
        size_t s = (size_t)( std::upper_bound( vSegs.begin(), vSegs.end(), (long long)pTicks[0], []( long long t, const Seg &x ) { return t < x.llTick; } ) - vSegs.begin() );
        for ( size_t i = 0; i < n; i++ )
        {
            long long t = (long long)pTicks[i];
            while ( s < vSegs.size() && vSegs[s].llTick <= t ) s++;
            pOut[i] = (uint64_t)In( vSegs[s ? s - 1 : 0], t );
        }
    }

    // Last tick whose time is <= llTime (time is non-decreasing in tick)
    long long TickAt( long long llTime ) const
    {
        if ( llTime < 0 ) return -1;
        long long lo = 0, hi = 1;
        while ( At( hi ) <= llTime && hi < ( 1LL << 62 ) ) hi *= 2;
        while ( lo < hi )
        {
            long long mid = lo + ( hi - lo + 1 ) / 2;
            if ( At( mid ) <= llTime ) lo = mid; else hi = mid - 1;
        }
        return lo;
    }

    // vMeta in global order (tempo events of length 3 among them)
    void Build( const std::vector< RefMetaEvent > &vMeta, int iDiv )
    {
        iDivision = iDiv;
        vSegs.assign( 1, { 0, 0, 500000 } );
        for ( const RefMetaEvent &m : vMeta )
            if ( m.cType == RefMetaEvent::SetTempo )
            {
                long long llTime = At( m.llTick ), llRem = 0;
                if ( bExact && !IsSmpteDivision( iDivision ) )
                {
                    // (the time at the tick, and its part of a microsecond, from the segment it ends)
                    const Seg &s = vSegs.back();
                    const long long n = s.llRem + s.llMicroSecsPerBeat * ( m.llTick - s.llTick );
                    llTime = s.llMicroSec + n / iDivision;
                    llRem = n % iDivision;
                }
                long long llUspb = ( (long long)m.aData[0] << 16 ) | ( m.aData[1] << 8 ) | m.aData[2];
                vSegs.push_back( { m.llTick, llTime, llUspb, llRem } );
            }
    }
};
