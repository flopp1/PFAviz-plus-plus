#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct PitchBendRange
{
    int semitones = 2, cents = 0, wheel = 8192;
    int rpnMSB = 127, rpnLSB = 127;
    int nrpnMSB = 127, nrpnLSB = 127;
    bool nrpn = false;

    double Range() const { return semitones + cents / 100.0; }
    double OutputRange() const { return std::min( 127.99, Range() + 1.0 ); }
    int Bend( double offsetCents ) const
    {
        double pitch = ( wheel - 8192 ) * Range() / 8192.0 + offsetCents / 100.0;
        return std::clamp( (int)std::lround( 8192.0 + pitch * 8192.0 / OutputRange() ), 0, 16383 );
    }
    bool Control( unsigned controller, unsigned value )
    {
        switch ( controller )
        {
            case 101: rpnMSB = value; nrpn = false; break;
            case 100: rpnLSB = value; nrpn = false; break;
            case 99: nrpnMSB = value; nrpn = true; break;
            case 98: nrpnLSB = value; nrpn = true; break;
            case 121:
                wheel = 8192;
                rpnMSB = rpnLSB = nrpnMSB = nrpnLSB = 127;
                nrpn = false;
                return true;
        }
        if ( nrpn || rpnMSB != 0 || rpnLSB != 0 ) return false;
        switch ( controller )
        {
            case 6: semitones = value; return true;
            case 38: cents = std::min( value, 99u ); return true;
            case 96: semitones = std::min( semitones + 1, 127 ); return true;
            case 97: semitones = std::max( semitones - 1, 0 ); return true;
        }
        return false;
    }
};
