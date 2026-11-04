// LZ4 block format (DESIGN.md 5.2): a small greedy compressor, liblz4's HC compressor, and a fast
// decompressor for both. Decoding runs at several GB/s per thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Lz4
{
    // Bytes the decompressor may write past the end of its output, and read past the end of its input
    const size_t Slack = 32;

    // Worst-case compressed size of n input bytes
    inline size_t Bound( size_t n ) { return n + n / 255 + 16; }

    // Compresses src[0, n) into dst (capacity at least Bound( n )); returns the compressed size
    size_t Compress( const uint8_t *src, size_t n, uint8_t *dst );

    // The same, searched harder (liblz4's HC at iLevel, 1 to 12): what is kept once loaded (the event
    // stream's chunks, the compact visual store's groups) takes 8-30 % less and decodes as fast
    const int HcLevel = 9;
    size_t CompressHC( const uint8_t *src, size_t n, uint8_t *dst, int iLevel = HcLevel );

    // Decompresses exactly iRaw bytes into dst; false if the input is malformed. dst must have Slack
    // writable bytes after iRaw, and src Slack readable bytes after iSize (they are scratch).
    bool Decompress( const uint8_t *src, size_t iSize, uint8_t *dst, size_t iRaw );
}
