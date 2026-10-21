// Tick -> microseconds with the exact integer arithmetic of the original (DESIGN.md R3), identical to
// RefMidi::TickToMicroSecs. Every valid SetTempo event starts a segment, even a redundant one.
#pragma once

#include <algorithm>
#include <vector>

#include "midi/RefMidi.h"

struct TempoMap
{
    struct Seg { long long llTick, llMicroSec, llMicroSecsPerBeat; };
    std::vector< Seg > vSegs;
    int iDivision = 0;

    long long At( long long llTick ) const
    {
        auto it = std::upper_bound( vSegs.begin(), vSegs.end(), llTick, []( long long t, const Seg &s ) { return t < s.llTick; } );
        const Seg &s = ( it == vSegs.begin() ) ? vSegs.front() : *( it - 1 );
        if ( iDivision & 0x8000 )
        {
            int iFramesPerSec100 = -( ( iDivision | static_cast< int >( 0xFFFF0000 ) ) >> 8 ) * 100;
            if ( iFramesPerSec100 == 2900 ) iFramesPerSec100 = 2997;
            long long llTicksPerSec100 = (long long)( iDivision & 0xFF ) * iFramesPerSec100;
            if ( llTicksPerSec100 <= 0 ) return s.llMicroSec;
            return s.llMicroSec + ( 100000000LL * ( llTick - s.llTick ) ) / llTicksPerSec100;
        }
        return s.llMicroSec + ( s.llMicroSecsPerBeat * ( llTick - s.llTick ) ) / iDivision;
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
                long long llTime = At( m.llTick );
                long long llUspb = ( (long long)m.aData[0] << 16 ) | ( m.aData[1] << 8 ) | m.aData[2];
                vSegs.push_back( { m.llTick, llTime, llUspb } );
            }
    }
};
