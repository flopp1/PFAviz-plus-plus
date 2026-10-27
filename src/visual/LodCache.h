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
// Clean drawing (Setting::uClean) changes what a note covers: with MinHeight every note is at least
// a row tall (none is dropped for its height); with EdgeRound its top is rounded from its end time,
// so a later note ending no earlier covers it whatever the two durations (and a note whose end is in
// its start's cell is dropped, being 0 rows tall in every frame, unless MinHeight).
//
// The cache is built in segments of start time (Setting::SegmentUs), in the background around the playhead,
// and only replaces the bucket loop of FastEngine::SweepKey for buckets whose whole start range is
// built; everything else (narrow keys, sounding notes below the field, the Ehw edge, hidden notes
// being skipped) keeps the exact store queries.
#pragma once

#include <algorithm>
#include <atomic>
#include <climits>
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
        unsigned uClean = 0;                // Clean::MinHeight and Clean::EdgeRound (visual/Scene.h); other flags 0
        std::vector< long long > vTheta;    // the frame's Theta table (rows iRowLo - 1 ...), set by Configure
        // Segment length: 8 note spans, from 1 to 10 s, so fast note speeds (few notes per cell
        // dropped) keep a few seconds of song around the playhead rather than a minute
        long long SegmentUs() const { return std::min( 10000000LL, std::max( 1000000LL, llSpan * 8 ) ); }
        bool Same( const Setting &o ) const
        {
            return fNotesY == o.fNotesY && fNotesCY == o.fNotesCY && llSpan == o.llSpan && llUsPP == o.llUsPP &&
                   iRowLo == o.iRowLo && iRowHi == o.iRowHi && uClean == o.uClean;
        }
    };
    // A kept note in 16 bytes: its store position (39 bits), the re-paired flag and its start in
    // microseconds from the segment's start (24 bits: a segment is at most 10 s) in one word; its slot;
    // its duration in microseconds (LongDur: 71 minutes or more, kept apart in Segment::avLong)
    struct Entry
    {
        static const unsigned WeirdShift = 39, StartShift = 40;
        static const uint64_t PosMask = ( 1ull << WeirdShift ) - 1;
        static const uint32_t LongDur = UINT32_MAX;
        uint64_t uPosStart;
        uint32_t uSlot;                     // track * 16 + channel
        uint32_t uDur;
        uint64_t Pos() const { return uPosStart & PosMask; }
        bool Weird() const { return ( uPosStart >> WeirdShift ) & 1; }
    };
    struct Segment
    {
        long long llIndex = 0;              // covers starts in [llIndex, llIndex + 1) * SegmentUs
        long long llStartUs = 0;            // llIndex * SegmentUs
        std::vector< Entry > av[128];       // per key, ascending position
        std::vector< std::pair< uint32_t, long long > > avLong[128];   // per key: (entry, duration) of the LongDur entries
        size_t iBytes = 0;

        long long Start( const Entry &e ) const { return llStartUs + (long long)( e.uPosStart >> Entry::StartShift ); }
        long long Dur( int k, size_t q, const Entry &e ) const
        {
            if ( e.uDur != Entry::LongDur ) return e.uDur;
            const std::vector< std::pair< uint32_t, long long > > &v = avLong[k];
            return std::lower_bound( v.begin(), v.end(), std::make_pair( (uint32_t)q, LLONG_MIN ) )->second;
        }
        size_t LowerBound( int k, uint64_t llPos ) const;   // first entry of key k at or after a position
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

    // The setting (its Theta table apart: copied only when the setting changes) and store of the
    // current frame; a change discards everything built
    void Configure( const Setting &s, const std::vector< long long > &vTheta, std::shared_ptr< const VisualStore > pStore, unsigned uStoreGen,
                    const PackedMonotone *pTimes );
    // Into v: the segments covering starts in [t0, t1), and a request to build what is missing around
    // them (bSync: build it now, on the calling thread's pool, before returning)
    void Get( View &v, long long t0, long long t1, long long llPrefetchEnd, bool bSync, int iThreads );
    // Hands the segments of a view the frame no longer needs to the worker thread, which frees the
    // ones only it still held (freeing a segment, tens of MB at fast note speeds, cost frames 2-4 ms);
    // v is left empty, its capacity kept
    void Retire( View &v );
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
