// The visual store (DESIGN.md 4.6, 5.3, 6): every NoteOn with velocity > 0, per key, in global index
// order, with its pairing as the original's ConnectNotes makes it. Built by a parallel pass over the
// event stream (each thread owns a set of keys and reads the whole stream, so every track's file
// order is kept), into packed blocks of 64 notes with per-block summaries and max trees for the
// per-frame queries of the fast renderer (visual/FastEngine).
//
// Per note: start (unique tick index), draw end (unique tick index of the off that paired it last,
// the original's sister), colour slot (track * 16 + channel), and the event ordinals of the on and
// of that off within their track's group at their tick. An on and the offs that pair it are in the
// same track, and a tick's groups are in track order, so (tick, track, ordinal) is the global event
// order (OrderKey): the order between notes of one tick, needed for the cross-key paint order and
// the jump walk.
//
// Two layouts:
//   Full    every NoteOn, unpaired ones flagged (positions are block * 64 + j), end ordinals, and
//           every off of a note paired more than once ("weird", DESIGN.md 6.3). Built by the pairing
//           pass; kept as the final layout only by tools (LoadProgress::bFullVisual).
//   Compact unpaired notes dropped (never drawn, never lit), the same-start cull (8.8.3) applied, no
//           end ordinals, a colour palette per block. Weird notes keep their offs.
//           Same-start cull: a note is dropped when a later note of the same key has the same start
//           and the same end, and neither is weird. The later one has identical geometry and is drawn
//           after it, at every setting and zoom (narrow keys included), and it lights the key whenever
//           the dropped one would. It is valid for keys whatever is hidden, and for drawing within the
//           hidden mask it was built with (the draw view is built from the stream, BuildFilteredStore).
//           Songs with weird notes also keep what the jump walk (DESIGN.md 6.4) needs of the dropped
//           notes: per kept note the number it dropped (DupCount), and for the start ticks where those
//           counts do not say which kept note each culled NoteOn belongs to (several kept notes with
//           drops, or drops and unpaired notes), a class code per culled NoteOn (DupCodes). The NoteOns
//           themselves and their order come from the event stream.
//   Compact + LZ (the Performance tab's "Compact visual store", DESIGN.md 5.3): the culled notes
//           of a Compact store in groups of 1024 as byte varints, LZ4-compressed. The per-block
//           summaries and trees stay as they are; each key keeps the groups it decoded last in a
//           small LRU of its own (SetLzCacheBytes), so the frame-to-frame working set stays decoded.
//           A key is drawn by one thread at a time, and the LOD builder reads with GetRange, which
//           decodes apart from the cache, so its spinlock is rarely contended.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/WordArena.h"
#include "song/PackedArray.h"

class EventStream;
struct Song;
struct NoteStats;

struct NoteView
{
    uint64_t llStartU, llEndU;      // unique tick indices; llEndU == llStartU for unpaired notes
    uint32_t uSlot;                 // track * 16 + channel
    uint32_t uRank, uEndRank;       // event ordinals within the track's group at the start and end ticks (end: Full only)
    bool bUnpaired;
    bool bWeird;
};

// The order of an event within its unique tick: track first, then the ordinal in the track's group
inline uint64_t OrderKey( uint32_t uSlot, uint32_t uOrd ) { return ( (uint64_t)( uSlot >> 4 ) << 32 ) | uOrd; }

struct OffRef { uint64_t llU; uint32_t uOrd; };   // an off of a weird note (same track as the note)

class KeyStore
{
public:
    static const unsigned BlockShift = 6;
    static const size_t BlockSize = 64;

    size_t Size() const { return m_n; }
    void Get( size_t i, NoteView &n ) const;
    // Notes [i, i + n) in one pass. A Compact + LZ store decodes their groups here, without its cache:
    // for long sweeps (the LOD builder), which would otherwise push out the groups frames are using
    void GetRange( size_t i, size_t n, NoteView *pOut ) const;
    // Every note of block b (64 except the last block); returns the count
    unsigned GetBlock( size_t b, NoteView *pOut ) const;
    size_t BlockCount() const { return Blocks(); }
    uint64_t StartU( size_t i ) const;
    uint64_t EndU( size_t i ) const;

    size_t LowerBoundStart( uint64_t llU ) const;               // first position with start >= llU
    // The same when it is known to be below hi (at most hi): searched from hi back, for answers near it
    size_t LowerBoundStartBelow( uint64_t llU, size_t hi ) const;
    size_t LowerBoundOrder( uint64_t llU, uint64_t llOrder ) const; // first position with (start, OrderKey) >= given
    // Rightmost position in [lo, hi) with end tick >= llEndU, or -1
    int64_t RightmostEnd( size_t lo, size_t hi, uint64_t llEndU ) const;
    // Rightmost position in [lo, hi) with times[end] - times[start] >= llDurUs, or -1
    int64_t RightmostDur( size_t lo, size_t hi, uint64_t llDurUs, const PackedMonotone &times ) const;

    // Offs of a weird note in event order (nullptr when the note is not weird)
    const OffRef *WeirdOffs( size_t i, uint32_t &uCount ) const;
    size_t UnpairedBefore( size_t i ) const;                   // unpaired notes at positions < i
    size_t UnpairedCount() const { return m_vUnpaired.size(); }
    bool IsUnpaired( size_t i ) const;
    bool IsWeird( size_t i ) const;
    // Compact layout of a song with weird notes: the paired NoteOns the same-start cull dropped in
    // favour of note i (same start and end, earlier on this key)
    uint32_t DupCount( size_t i ) const;
    bool HasDups() const { return !m_vDupBlocks.empty(); }
    // The culled NoteOns (dropped duplicates and unpaired) of start tick llU, in event order, belong to
    // classes: the kept notes with DupCount > 0 in position order, then "unpaired" if there are any.
    // Where more than one class is present, the class of each is coded with uWidth bits from llBit
    // (DupCode); otherwise this returns false. [i0, i1) are the kept notes of tick llU (not empty).
    bool DupCodes( uint64_t llU, size_t i0, size_t i1, uint64_t &llBit, unsigned &uWidth ) const;
    uint32_t DupCode( uint64_t llBit, unsigned uWidth ) const { return (uint32_t)ReadBits( m_vDupCodeBits.data(), llBit, uWidth ); }
    size_t WeirdCount() const { return m_vWeird.size(); }
    uint64_t WeirdPos( size_t w ) const { return m_vWeird[w].llPos; }
    const OffRef *WeirdOffsAt( size_t w, uint32_t &uCount ) const { uCount = m_vWeird[w].uCount; return &m_vWeirdOffs[m_vWeird[w].uFirst]; }
    bool Compact() const { return m_bCompact; }
    bool Lz() const { return m_bLz; }
    struct LzNote { uint64_t llStartU, llEndU; uint32_t uSlot, uRank; };   // a decoded note of an LZ group
    void ToLz();                                            // Compact -> Compact + LZ
    bool HasTrees() const { return m_iLeaves != 0; }
    void BuildFullTrees( const PackedMonotone &times );     // Full layout: block maxima from the notes
    size_t MemoryBytes() const;
    // Diagnostics: bytes in blocks, compact blocks, palette, bits, trees, weird and unpaired lists, LZ data,
    // LZ cache, dropped-duplicate data
    static const int MemoryPartCount = 9;
    void MemoryParts( size_t a[MemoryPartCount] ) const;

    KeyStore();
    ~KeyStore();
    KeyStore( KeyStore && ) noexcept;
    KeyStore &operator=( KeyStore && ) noexcept;

private:
    friend class KeyBuilder;
    friend class CompactBuilder;

    // A block's start fields (start, slot, rank) are packed as soon as it has 64 notes; its end
    // fields (duration, end rank, flags) once all of them are final
    struct Block
    {
        uint64_t llBitStart = 0;    // start records in m_vBits
        uint64_t llBitEnd = 0;      // end records in m_vBits
        uint64_t llStartBase = 0;   // start of the first note
        // Ordinals as base + j * stride + the stored value: a crash's notes in one block are usually
        // a regular progression, which then costs (almost) no bits
        int64_t llRankBase = 0, llEndRankBase = 0;
        int32_t iRankStride = 0, iEndRankStride = 0;
        uint32_t uSlotBase = 0;
        uint8_t wStart = 0, wSlot = 0, wRank = 0, uStartBits = 0;
        uint8_t wDur = 0, wEndRank = 0, wFlags = 0, uEndBits = 0;
        uint8_t uCount = 0, bWeird = 0, bStartPacked = 0, bEndPacked = 0;
    };
    struct Weird { uint64_t llPos; uint32_t uFirst, uCount; };
    // Compact layout: one record per note (start delta, palette index, ordinal, duration)
    struct CBlock
    {
        uint64_t llBit = 0;
        uint64_t llStartBase = 0;
        uint32_t uPalette = 0;      // first entry in m_vPalette
        uint8_t wStart = 0, wPal = 0, wRank = 0, wDur = 0;
    };

    // LZ groups: 1024 notes (16 blocks); varints per note: start - previous start, zigzag slot
    // delta, ordinal, duration
    static const unsigned GroupShift = 11;
    struct LzGroup { uint64_t llOffset; uint32_t uSize, uRaw; uint64_t llStartBase; };
    LzNote LzGet( size_t i ) const;
    // fn( pNotes ) on group g decoded in the key's cache (its notes from position g << GroupShift), under
    // the cache's lock: a scan of many of a group's notes for one lookup
    template < class F >
    auto LzWith( uint64_t g, F &&fn ) const;
    // Decodes group g into pOut (its notes, up to 1 << GroupShift), with vRaw as scratch
    void LzDecode( uint64_t g, LzNote *pOut, std::vector< uint8_t > &vRaw ) const;
    size_t LzCacheBytes() const;                            // decoded groups held now
    size_t DupBytes() const;
    struct LzCache;
    mutable std::unique_ptr< LzCache > m_pLzCache;

    size_t Blocks() const { return m_bLz ? m_vLzBlockStart.size() : m_bCompact ? m_vCBlocks.size() : m_vBlocks.size(); }
    uint64_t BlockStartBase( size_t b ) const
    {
        return m_bLz ? m_vLzBlockStart[b] : m_bCompact ? m_vCBlocks[b].llStartBase : m_vBlocks[b].llStartBase;
    }
    uint64_t BlockMaxEnd( size_t b ) const { return m_vTreeEnd[m_iLeaves + b]; }
    uint32_t BlockMaxDur( size_t b ) const { return m_vTreeDur[m_iLeaves + b]; }

    static uint64_t ReadBits( const uint64_t *pWords, uint64_t llBit, unsigned w ) { return ReadBitField( pWords, llBit, w ); }
    uint64_t Read( uint64_t llBit, unsigned w ) const { return w ? ReadBits( &m_vBits[0], llBit, w ) : 0; }
    int64_t ScanEnd( size_t lo, size_t hi, uint64_t llEndU ) const;
    // First position in [b << BlockShift, hi) with start >= llU, or hi (block b's first note starts before llU)
    size_t BlockLowerBound( size_t b, uint64_t llU, size_t hi ) const;
    int64_t ScanDur( size_t lo, size_t hi, uint64_t llDurUs, const PackedMonotone &times ) const;
    int64_t TreeEnd( size_t node, size_t nl, size_t nr, size_t a, size_t b, uint64_t x ) const;
    int64_t TreeDur( size_t node, size_t nl, size_t nr, size_t a, size_t b, uint32_t x ) const;
    void BuildTrees( const std::vector< uint64_t > &vMaxEnd, const std::vector< uint32_t > &vMaxDur );

    size_t m_n = 0;
    bool m_bCompact = false, m_bLz = false;
    uint64_t m_llLzId = 0;                  // identifies this store's groups in the thread caches
    std::vector< uint64_t > m_vLzBlockStart;
    std::vector< LzGroup > m_vLzGroups;
    std::vector< uint8_t > m_vLzData;
    std::vector< Block > m_vBlocks;
    std::vector< CBlock > m_vCBlocks;
    std::vector< uint32_t > m_vPalette;
    WordArena m_vBits;
    size_t m_iLeaves = 0;
    std::vector< uint64_t > m_vTreeEnd;
    std::vector< uint32_t > m_vTreeDur;
    std::vector< Weird > m_vWeird;          // sorted by position
    std::vector< OffRef > m_vWeirdOffs;
    std::vector< uint64_t > m_vUnpaired;    // sorted positions
    std::vector< uint64_t > m_vWeirdBlocks; // Compact: one bit per block that holds a weird note
    // Compact layout of a song with weird notes (DupCount, DupCodes)
    std::vector< uint64_t > m_vDupBlocks;   // per block: bit offset in m_vDupBits << 8 | bit width
    std::vector< uint64_t > m_vDupBits;
    // Class codes of the start ticks with more than one class, in position order; a tick's codes take
    // (dropped + unpaired NoteOns) * width bits, so they are found from the offset at the last multiple
    // of CodeBaseNotes kept notes before it: the codes of the first tick whose kept notes start there
    // or later
    static const unsigned CodeBaseShift = 8;
    std::vector< uint64_t > m_vCodeBase;
    struct UnpairedAt { uint64_t llU; uint64_t llCount; };
    std::vector< UnpairedAt > m_vDupUnpaired;   // unpaired NoteOns of start ticks with drops, by tick
    std::vector< uint64_t > m_vDupCodeBits;
    uint64_t TickUnpaired( uint64_t llU ) const;
    // Classes and culled NoteOns of the start tick whose kept notes are [i0, i1)
    void TickClasses( uint64_t llU, size_t i0, size_t i1, uint64_t &llCulled, unsigned &uClasses ) const;
};

struct VisualStore
{
    KeyStore aKeys[128];
    bool bWeird = false;                    // some note was paired more than once (Full layout)
    bool bCompact = false;                  // Compact layout (culled with the hidden mask it was built for)
    long long llFirstNote = -1;             // time of the first paired note (-1: none)
    uint64_t llNotes = 0, llUnpaired = 0, llWeirdNotes = 0;
    uint64_t llStored = 0;                  // notes kept in the store (after the culls)
    // Time of the parts of BuildVisualStore: the pairing pass, finishing the keys, the jump-walk
    // tables, pairing again (songs with weird notes, compact path), compacting or the Full trees, LZ
    enum Step { StepPair, StepFinish, StepJump, StepRepair, StepCompact, StepLz, StepCount };
    double adStepSeconds[StepCount] = {};
    uint64_t adStepPrivate[StepCount] = {};     // process private bytes at the end of each
    // Only for songs with weird notes, the jump walk (DESIGN.md 6.4): every unique tick with a paired
    // NoteOn, the OrderKey of its last paired NoteOn, and that NoteOn's Simultaneous + 1 (the walk's K)
    std::vector< uint64_t > vOnTicks, vOnLast;
    std::vector< int64_t > vJumpK;

    size_t MemoryBytes() const;
};

// Memory for each thread's cache of decoded LZ groups (the Performance tab's block cache)
void SetLzCacheBytes( size_t iBytes );

// The draw view for hidden channels (DESIGN.md 8.10): the paired notes whose slot is not hidden
// (vHiddenSlot[track * 16 + channel] != 0). The pairing pass is run again and the view is compacted
// and culled among the visible notes only. Unpaired notes are left out. Jump-walk data is not built:
// the walk always runs on song.visual.
bool BuildFilteredStore( const Song &song, const std::vector< uint8_t > &vHiddenSlot, VisualStore &out, int iThreads,
                         const std::atomic< bool > *pCancel );

// Pairs every note of the song (DESIGN.md 6) and builds song.visual (compacted unless bCompact is
// false). pDone counts unique ticks processed by the slowest thread (out of
// song.stream.UniqueTicks()). bNoteStats: also count song.noteStats, from the same pass.
bool BuildVisualStore( Song &song, int iThreads, std::atomic< uint64_t > *pDone, const std::atomic< bool > *pCancel, bool bCompact = true,
                       bool bLz = false, bool bNoteStats = false );
