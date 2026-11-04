// Deduplication (Performance tab, "Deduplicate repeated tracks"), after fMCG's layered walk and
// Kimrashi's repeated xz streams.
//
// Layered files: a MIDI's bytes cut into pieces (whole tracks, or slices cut mid-event), each piece
// compressed on its own, the compressed pieces concatenated with repeats, and that compressed again,
// several layers deep, so that a few MB expand to terabytes. Open:
//   1. Discovery, layer by layer: the file is cut into members (xz, gzip, bzip2, zstd and lz4
//      streams, found by their magic and cut at their exact ends: an xz stream at a footer whose index
//      walks back to its start, zstd and lz4 frames by their block sizes, gzip and bzip2 by decoding).
//      Equal members are kept once. Each distinct member is decoded once: when what it holds starts
//      with a magic, it is the next layer and is cut in turn (only after the whole layer around it has
//      been cut); otherwise it is a leaf, MIDI bytes. Each layer's sequence of members is held as a
//      grammar: runs, and patterns repeated in a row (A B C A B C ...), patterns within patterns.
//   2. Reading: the grammar is expanded front to back as the loader reads the MIDI's chunks. A part of
//      it (a member, a pattern) read whole from one chunk boundary to another is remembered with the
//      tracks it gave; when it comes again at a chunk boundary, those tracks are counted again as
//      copies instead of being decoded and read (Reader::Logical). A run of a pattern whose repeats
//      reach chunk boundaries only every few repeats is taken the same way, a block at a time.
//   With bAssume (fMCG's "risky fast dedup", Kimrashi's "trust the rest"): once the first pattern
//   of a layer repeats, if the rest of the layer is a whole number of its copies by size, it is taken
//   to be those copies without decoding it.
//
// Files made the usual way: identical MTrk chunks are found by a hash of their bytes (Hasher) and
// loaded once (Loader.cpp); from an archive (a .7z, a plain .mid.xz), a track is counted as it is
// read, and what it counted dropped once its hash shows it a copy. Either way a track that stands for several is drawn as the last of them
// (each copy hides the ones before it, so the pixels are the same), played once, and counted as many
// times as it stands for in the statistics (Song::vTrackCopies).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "song/Archive.h"

namespace Dedup
{
    // Opens sPath when it is a chain of compressed streams (else null with sError empty: not for this
    // file, which is then opened as usual; null with sError: failed). Its discovery reports *pDone of
    // *pTotal bytes decoded; a member of xz decodes on iThreads.
    std::unique_ptr< Archive::Reader > Open( const std::wstring &sPath, bool bAssume, std::atomic< uint64_t > *pDone,
                                             std::atomic< uint64_t > *pTotal, const std::atomic< bool > *pCancel, std::wstring &sError,
                                             int iThreads = 1 );

    // A 128-bit hash of bytes given in any pieces (equal bytes, equal hash, however they are cut). It
    // takes 32 bytes at a time in four lanes of two chains each, so the multiplies do not wait on one
    // another (one chain was about 1.7 GB/s, slower than the archive decoders feeding it)
    class Hasher
    {
    public:
        void Add( const uint8_t *p, size_t n )
        {
            m_llLen += n;
            if ( m_iCarry )
            {
                while ( m_iCarry < 32 && n ) { m_aCarry[m_iCarry++] = *p++; n--; }
                if ( m_iCarry < 32 ) return;
                Block( m_aCarry );
                m_iCarry = 0;
            }
            for ( ; n >= 32; p += 32, n -= 32 ) Block( p );
            while ( n ) { m_aCarry[m_iCarry++] = *p++; n--; }
        }
        struct Value
        {
            uint64_t a, b, llLen;
            bool operator==( const Value &o ) const { return a == o.a && b == o.b && llLen == o.llLen; }
        };
        Value Finish() const
        {
            Hasher h = *this;
            uint8_t aTail[32] = {};     // (the length, mixed in, tells the padding apart)
            memcpy( aTail, h.m_aCarry, h.m_iCarry );
            h.Block( aTail );
            uint64_t a = m_llLen, b = m_llLen << 1;
            for ( int i = 0; i < 4; i++ ) a = Mix( a ^ h.m_aX[i] ) + i, b = Mix( b ^ h.m_aY[i] ) + i;
            return { Mix( a + b ), Mix( b ^ ( a >> 7 ) ), m_llLen };
        }

    private:
        static uint64_t Mix( uint64_t h )
        {
            h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
            h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull;
            return h ^ ( h >> 33 );
        }
        void Block( const uint8_t *p )
        {
            for ( int i = 0; i < 4; i++ )
            {
                uint64_t w;
                memcpy( &w, p + 8 * i, 8 );
                uint64_t x = ( m_aX[i] ^ w ) * 0x9E3779B97F4A7C15ull;
                m_aX[i] = x ^ ( x >> 29 );
                m_aY[i] = ( ( m_aY[i] << 23 | m_aY[i] >> 41 ) ^ w ) * 0xD6E8FEB86659FD93ull;
            }
        }
        uint64_t m_aX[4] = { 0x243F6A8885A308D3ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull, 0x452821E638D01377ull };
        uint64_t m_aY[4] = { 0x13198A2E03707344ull, 0xBE5466CF34E90C6Cull, 0xC0AC29B7C97C50DDull, 0x3F84D5B5B5470917ull };
        uint64_t m_llLen = 0;
        uint8_t m_aCarry[32] = {};
        size_t m_iCarry = 0;
    };
}
