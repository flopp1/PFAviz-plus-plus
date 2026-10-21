// LZ4 block format (DESIGN.md 5.2): a small greedy compressor and a fast decompressor, no dependency.
// Used for the event stream's 64 KiB chunks; decoding runs at several GB/s per thread.
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

    // Decompresses exactly iRaw bytes into dst; false if the input is malformed. dst must have Slack
    // writable bytes after iRaw, and src Slack readable bytes after iSize (they are scratch).
    bool Decompress( const uint8_t *src, size_t iSize, uint8_t *dst, size_t iRaw );
}
