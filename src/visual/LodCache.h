// The LOD cache (DESIGN.md 8.11): for one display setting, the notes each key's bucket sweep can
// ever need, with every note dropped that a later one covers in every frame.
//
// For a setting (NotesY, CY, note speed, composited rows) the frame start R is always a multiple of
// usPP, so a note's bottom row is Beta( start - R ) and the times where it changes form the lattice
// { Theta_r + k * usPP }. Notes whose starts lie between two consecutive lattice points (a cell)
// have the same bottom row in every frame. Within a cell, a note A is dropped when a later note B,
// neither of them re-paired, has a height (Height of the duration) at least A's and an end at least
// as late: B's rows contain A's after the original clipping, B is drawn after A, and B is visible
// whenever A is (upcoming once A is, ends no earlier, comes later in the jump order). Notes of
// height 0 are dropped too: the bucket sweep never selects them. Unpaired notes are left out.
//
// The cache is built in segments of start time (Setting::SegmentUs), in the background around the playhead,
// and only replaces the bucket loop of FastEngine::SweepKey for buckets whose whole start range is
// built; everything else (narrow keys, sounding notes below the field, the Ehw edge, hidden notes
// being skipped) keeps the exact store queries.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "song/Song.h"

class LodCache
{
public:
    struct Setting
    {
        float fNotesY = 0, fNotesCY = 0;
        long long llSpan = 0, llUsPP = 0;
        int iRowLo = 0, iRowHi = 0;
        std::vector< long long > vTheta;    // the frame's Theta table (rows iRowLo - 1 ...)
        // Segment length: 8 note spans, from 1 to 10 s, so fast note speeds (few notes per cell
        // dropped) keep a few seconds of song around the playhead rather than a minute
        long long SegmentUs() const { return std::min( 10000000LL, std::max( 1000000LL, llSpan * 8 ) ); }
        bool Same( const Setting &o ) const
        {
            return fNotesY == o.fNotesY && fNotesCY == o.fNotesCY && llSpan == o.llSpan && llUsPP == o.llUsPP &&
                   iRowLo == o.iRowLo && iRowHi == o.iRowHi;
        }
    };
    struct Entry
    {
        static const uint64_t WeirdBit = 1ull << 63;
        uint64_t llPos;                     // store position, | WeirdBit for a re-paired note
        long long llStart, llEnd;           // microseconds
        uint32_t uSlot;                     // track * 16 + channel
    };
    struct Segment
    {
        static const unsigned BlockShift = 4;
        long long llIndex = 0;              // covers starts in [llIndex, llIndex + 1) * SegmentUs
        std::vector< Entry > av[128];       // per key, ascending position
        std::vector< long long > avMaxDur[128]; // per key, the longest duration of every 16 entries
        size_t iBytes = 0;

        size_t LowerBound( int k, uint64_t llPos ) const;   // first entry of key k at or after a position
        // Rightmost entry of key k in [lo, hi) lasting at least llDur microseconds, or -1
        int64_t RightmostDur( int k, size_t lo, size_t hi, long long llDur ) const;
    };
    // What a frame reads: the ready segments for a range of segment indices (null where not built)
    struct View
    {
        long long llFirst = 0, llSegUs = 1;
        std::vector< std::shared_ptr< const Segment > > v;
        bool Covers( long long t0, long long t1 ) const;   // every start in [t0, t1) is in a ready segment
        const Segment *At( long long t ) const;
    };

    LodCache();
    ~LodCache();

    // The setting and store of the current frame; a change discards everything built
    void Configure( const Setting &s, std::shared_ptr< const VisualStore > pStore, unsigned uStoreGen, const PackedMonotone *pTimes );
    // The segments covering starts in [t0, t1), and a request to build what is missing around them
    // (bSync: build it now, on the calling thread's pool, before returning)
    View Get( long long t0, long long t1, long long llPrefetchEnd, bool bSync, int iThreads );
    // Hands a view the frame no longer needs to the worker thread, which frees the segments only it
    // still held: freeing a segment (tens of MB at fast note speeds) cost frames 2-4 ms
    void Retire( View &&v );
    size_t MemoryBytes() const;

private:
    std::shared_ptr< Segment > Build( long long llIndex, int iThreads, const std::atomic< unsigned > *pCancelGen, unsigned uGen );
    void Worker();

    mutable std::mutex m_mx;
    std::condition_variable m_cv;
    std::thread m_Thread;
    bool m_bStop = false;

    Setting m_S;
    std::vector< long long > m_vRes;        // the lattice residues mod usPP, sorted
    std::shared_ptr< const VisualStore > m_pStore;
    const PackedMonotone *m_pTimes = nullptr;
    unsigned m_uStoreGen = ~0u;
    std::atomic< unsigned > m_uGen{ 0 };    // bumped by every reconfiguration

    std::vector< std::shared_ptr< const Segment > > m_vSeg;     // by segment index - m_llFirst
    long long m_llFirst = 0;
    std::vector< long long > m_vWanted;     // segment indices to build, most urgent first
    std::vector< std::shared_ptr< const Segment > > m_vRetired;    // dropped, freed by the worker
    long long m_llKeepLo = 0, m_llKeepHi = -1;
    size_t m_iBytes = 0;
};
