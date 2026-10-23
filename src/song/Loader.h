// Parallel MIDI loader, phases 0-4 of DESIGN.md Section 4:
//   0 open and map the file, pick the I/O strategy (SSD: prefetch; HDD: one sequential reader)
//   1 header and chunk discovery (incremental, behind the reader on HDDs)
//   2 count pass, parallel per track: group sizes, checkpoints, track info, tempo and signatures
//   3 global tick merge: unique ticks (bitmap rank or sort), group order, payload offsets, TimeTable
//   4 parallel parse straight into the final event stream, split at checkpoints inside big tracks
//     (fused single read, HDD and a file too large to stay cached: the count pass also packs every track,
//     and this phase copies the groups out of those packs instead of reading the file again)
//   5 note pairing and the visual store, parallel by key (song/VisualStore.h)
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "song/Song.h"

struct LoadProgress
{
    enum Stage { Reading, Counting, Merging, Building, Pairing, Finalising, StageCount };
    static const wchar_t *const StageNames[StageCount];

    std::atomic< int > iStage{ -1 };
    std::atomic< uint64_t > aDone[StageCount] = {}, aTotal[StageCount] = {};
    std::atomic< uint64_t > llFileSize{ 0 }, llBytesResident{ 0 };
    std::atomic< uint64_t > llTracksFound{ 0 }, llTracksDone{ 0 }, llEvents{ 0 }, llNotes{ 0 };
    std::atomic< bool > bCancel{ false };
    std::atomic< bool > bWaitingOnDisk{ false };
    std::atomic< long long > llCountEndUs{ 0 }; // Timer::NowMicroSecs() when the count pass finished
    std::atomic< uint64_t > llPrivateNow{ 0 }, llPrivatePeak{ 0 };    // the process's private memory in RAM (Util::ProcessMemory), sampled every 2 ms
    bool bSkipPairing = false;          // tools only: stop after the event stream (no visual store)
    bool bFullVisual = false;           // tools only: keep the visual store in the Full layout
    bool bLzVisual = false;             // the Performance tab's "Compact visual store" (LZ groups)
    bool bNoteStats = true;             // the extra statistics (Song::noteStats)
    bool bFirstPairingOnly = false;     // the Performance tab's "Ignore weird notes" (Song::bFirstPairingOnly)
    int iFusedRead = -1;                // tools only: 1 forces the fused single read (DESIGN.md 4.2a), 0 the two-pass read
};

struct LoadStats
{
    std::wstring sFile;
    uint64_t llFileSize = 0;
    bool bSeekPenalty = false, bSequentialRead = false, bFusedRead = false;
    uint64_t llFusedBytes = 0;          // fused read: the packed per-track streams (freed after the build)
    int iThreads = 0;
    uint64_t llOutOfRange = 0;          // channel events dropped for a key, controller or program past 127
    uint64_t llTracks = 0, llMalformedTracks = 0, llEvents = 0, llNotes = 0, llUniqueTicks = 0, llGroups = 0, llPayloadBytes = 0;
    bool bBitmapMerge = false;
    size_t aMem[7] = {}; // stream (compressed), stream index, tick table, time table, unused, metadata, notes
    uint64_t llPairedNotes = 0, llUnpairedNotes = 0, llWeirdNotes = 0;
    double adStageSeconds[LoadProgress::StageCount] = {};
    // The parts of each stage, in order: name, seconds, process private bytes at its end
    struct Step { int iStage; const wchar_t *sName; double dSeconds; uint64_t llPrivate; };
    std::vector< Step > vSteps;
    double dReaderSeconds = 0;          // spinning disks: time the sequential reader spent reading
    double dTotalSeconds = 0;
    // Process private bytes (commit): before the load (the previous song is still open), the peak
    // during it, after it, and the peak within each stage
    uint64_t llPrivateBefore = 0, llPrivatePeak = 0, llPrivateAfter = 0;
    uint64_t aStagePeak[LoadProgress::StageCount] = {};

    std::wstring Format() const;
};

namespace Loader
{
    std::unique_ptr< Song > LoadFile( const std::wstring &sPath, int iThreads, LoadProgress &progress, LoadStats &stats, std::wstring &sError );
    std::unique_ptr< Song > LoadMemory( const uint8_t *pData, size_t iSize, int iThreads, LoadProgress &progress, LoadStats &stats, std::wstring &sError );
}
