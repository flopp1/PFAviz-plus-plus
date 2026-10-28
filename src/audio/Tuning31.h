#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Tuning31
{
    inline uint8_t GroupBase( uint8_t channel )
    {
        return channel < 9 ? channel - channel % 3 : channel == 9 ? 9 : channel - ( channel - 10 ) % 3;
    }

    inline std::pair< uint8_t, uint8_t > Pitch( uint8_t note, uint8_t channel )
    {
        static constexpr uint8_t steps[31][2] = {
            { 0, 0 },
            { 0, 1 },
            { 1, 0 },
            { 1, 1 },
            { 2, 2 },
            { 2, 0 },
            { 2, 1 },
            { 3, 0 },
            { 3, 1 },
            { 4, 2 },
            { 4, 0 },
            { 4, 1 },
            { 5, 2 },
            { 5, 0 },
            { 5, 1 },
            { 6, 0 },
            { 6, 1 },
            { 7, 2 },
            { 7, 0 },
            { 7, 1 },
            { 8, 0 },
            { 8, 1 },
            { 9, 2 },
            { 9, 0 },
            { 9, 1 },
            { 10, 0 },
            { 10, 1 },
            { 11, 2 },
            { 11, 0 },
            { 11, 1 },
            { 12, 2 },
        };
        if ( channel == 9 ) return { channel, note };
        uint8_t base = GroupBase( channel );
        unsigned key = steps[note % 31][0] + 12 * ( note / 31 ) + 24 * ( channel - base );
        return { uint8_t( base + steps[note % 31][1] ), uint8_t( std::min( key, 127u ) ) };
    }

    inline constexpr uint8_t ScaleMessages[3][33] = {
        { 0xF0, 0x7F, 0x7F, 0x08, 0x09, 0x00, 0x48, 0x49, 0x40, 0x00, 0x31, 0x46, 0x3B, 0x6F, 0x2D, 0x35, 0x37, 0x5E, 0x42, 0x08, 0x33, 0x4E, 0x3D, 0x77, 0x2F, 0x3D, 0x39, 0x67, 0x2B, 0x2D, 0x35, 0x56, 0xF7, },
        { 0xF0, 0x7F, 0x7F, 0x08, 0x09, 0x01, 0x11, 0x12, 0x58, 0x63, 0x4A, 0x29, 0x54, 0x52, 0x46, 0x18, 0x50, 0x42, 0x5A, 0x6B, 0x4C, 0x31, 0x56, 0x5A, 0x48, 0x21, 0x52, 0x4A, 0x44, 0x10, 0x4E, 0x39, 0xF7, },
        { 0xF0, 0x7F, 0x7F, 0x08, 0x09, 0x02, 0x22, 0x24, 0x27, 0x1C, 0x18, 0x63, 0x23, 0x0C, 0x14, 0x52, 0x1E, 0x7B, 0x29, 0x25, 0x1A, 0x6B, 0x25, 0x14, 0x16, 0x5A, 0x21, 0x04, 0x12, 0x4A, 0x1C, 0x73, 0xF7, },
    };
}
