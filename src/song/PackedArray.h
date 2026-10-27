// Non-decreasing 64-bit sequence stored as blocks of 256 values: a base value plus fixed-width
// bit-packed offsets from it (frame of reference). O(1) random access, typically 1-2 bytes/value.
// Used for the unique-tick table and the TimeTable (DESIGN.md 5.1).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

class PackedMonotone
{
public:
    static const int BlockShift = 8;
    static const size_t BlockSize = size_t( 1 ) << BlockShift;

    // Builds n values; get(i) must be non-decreasing in i. Blocks are built in parallel.
    void Build( size_t n, const std::function< uint64_t( size_t ) > &get, int iThreads );

    size_t Size() const { return m_n; }
    uint64_t Get( size_t i ) const
    {
        const Block &k = m_vBlocks[i >> BlockShift];
        unsigned w = (unsigned)( k.llBitWidth & 0xFF );
        if ( !w ) return k.llBase;
        uint64_t llBit = ( k.llBitWidth >> 8 ) + ( uint64_t )( i & ( BlockSize - 1 ) ) * w;
        uint64_t lo = m_vBits[llBit >> 6] >> ( llBit & 63 );
        if ( ( llBit & 63 ) + w > 64 ) lo |= m_vBits[( llBit >> 6 ) + 1] << ( 64 - ( llBit & 63 ) );
        return k.llBase + ( w == 64 ? lo : ( lo & ( ( uint64_t( 1 ) << w ) - 1 ) ) );
    }
    // First index with value >= v (n if none)
    size_t LowerBound( uint64_t v ) const;
    // The same, searching outwards from iHint (cheap when the answer is near it: a few steps)
    size_t LowerBoundFrom( uint64_t v, size_t iHint ) const;
    // First index with value > v (n if none)
    size_t UpperBound( uint64_t v ) const;
    size_t MemoryBytes() const;

private:
    // Per block, in one record so a lookup reads one cache line besides the bits: the base, and the
    // block's first bit << 8 | the width
    struct Block { uint64_t llBase, llBitWidth; };
    size_t m_n = 0;
    std::vector< Block > m_vBlocks;
    std::vector< uint64_t > m_vBits;
    // The first index in block b's range with value >= v (bStrict: > v), or the block's end
    size_t BoundInBlock( size_t b, uint64_t v, bool bStrict ) const;
};
