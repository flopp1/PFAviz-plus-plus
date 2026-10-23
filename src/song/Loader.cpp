#include "song/Loader.h"

#include <algorithm>
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/Lz4.h"
#include "core/MappedFile.h"
#include "core/ThreadPool.h"
#include "core/Util.h"
#include "song/TempoMap.h"

const wchar_t *const LoadProgress::StageNames[StageCount] =
{
    L"Reading file", L"Counting events", L"Merging ticks", L"Building event stream", L"Pairing notes", L"Finalising"
};

namespace
{
    const uint64_t CheckpointEvery = 4096; // groups between resume points inside a track
    const size_t FusedChunk = 256 << 10;    // fused read: raw bytes per packed chunk (closed at the next group)

    enum ScanMode { Count, Emit, Fused };

    struct GroupRec
    {
        int64_t llTick;     // after the merge: the group's payload offset
        uint32_t uSize;
        uint32_t uEvents;
    };

    struct Checkpoint
    {
        uint64_t llPos;     // offset in the track of the delta of the event that opens the group
        int64_t llTick;     // tick before that delta
        uint64_t llGroup;
        uint8_t cRunning;
    };

    struct TrackScan
    {
        uint64_t llBegin = 0, llEnd = 0;    // byte range of the track data in the file
        std::vector< GroupRec > vGroups;
        std::vector< Checkpoint > vCps;
        RefTrackInfo info;
        std::vector< RefMetaEvent > vMeta;
        uint32_t aKeyCounts[128] = {};
        uint64_t llChannelEvents = 0;
        uint64_t llOutOfRange = 0;          // channel events dropped for a key, controller or program past 127
        // Fused read (DESIGN.md 4.2a): the track's groups as encoded by the emit pass (runs only, no group
        // headers), LZ4-compressed in chunks that hold whole groups
        struct PackedChunk
        {
            std::unique_ptr< uint8_t[] > data;  // uPacked bytes plus Lz4::Slack; uPacked == uRaw: stored as is
            uint32_t uPacked = 0, uRaw = 0;
            uint64_t llFirstGroup = 0;
        };
        std::vector< PackedChunk > vPacked;
        uint64_t llPackedBytes = 0;
    };

    inline bool ReadVarLen( const uint8_t *&p, const uint8_t *pEnd, long long &llOut )
    {
        llOut = 0;
        for ( int i = 0; i < 4; i++ )
        {
            if ( p >= pEnd ) return false;
            uint8_t c = *p++;
            llOut = ( llOut << 7 ) | ( c & 0x7F );
            if ( !( c & 0x80 ) ) return true;
        }
        return false;
    }

    inline unsigned OpOf( unsigned iType, unsigned d2 )
    {
        switch ( iType )
        {
            case 0x8: return 1;
            case 0x9: return d2 ? 0 : 2;
            default: return iType - 0xA + 3; // A..E -> 3..7
        }
    }

    // One track, SMF semantics identical to RefMidi::ParseTrack (DESIGN.md 1.5).
    // Count: fills pScan (groups with their encoded sizes, checkpoints, info, meta).
    // Emit:  re-reads from a checkpoint and writes groups [llFirstGroup, llStopGroup) to the payload.
    // Fused: Count, and also encodes the groups' runs into pScan->vPacked (DESIGN.md 4.2a).
    template < int MODE >
    void ScanTrack( const uint8_t *pTrack, const uint8_t *pEnd, const Checkpoint &start, uint64_t llStopGroup, int iTrack,
                    TrackScan *pScan, const GroupRec *pGroups, uint8_t *pPayload )
    {
        constexpr bool COUNT = MODE != Emit, WRITE = MODE != Count;
        const uint8_t *p = pTrack + start.llPos;
        long long llTick = start.llTick;
        int iRunning = start.cRunning;

        int64_t llGroup = (int64_t)start.llGroup - 1;
        long long llGroupTick = LLONG_MIN;
        uint32_t uRunKey = UINT32_MAX;
        // count state
        uint32_t uSize = 0, uEvents = 0;
        // emit state
        uint8_t *pDst = nullptr, *pLastKey = nullptr, *pRunHeader = nullptr;
        // fused state: the runs of the groups since the last chunk, in a buffer that grows for huge groups
        std::unique_ptr< uint8_t[] > pStage;
        uint8_t *pStageEnd = nullptr;
        uint64_t llChunkFirst = 0;
        if ( MODE == Fused )
        {
            pStage.reset( new uint8_t[FusedChunk * 2] );
            pDst = pStage.get();
            pStageEnd = pDst + FusedChunk * 2;
        }
        auto Reserve = [&]() {  // room for one event (at most 3 bytes)
            if ( MODE != Fused || pStageEnd - pDst >= 3 ) return;
            size_t iUsed = (size_t)( pDst - pStage.get() ), iCap = (size_t)( pStageEnd - pStage.get() ) * 2;
            uint8_t *pNew = new uint8_t[iCap];
            memcpy( pNew, pStage.get(), iUsed );
            if ( pLastKey ) pLastKey = pNew + ( pLastKey - pStage.get() );
            if ( pRunHeader ) pRunHeader = pNew + ( pRunHeader - pStage.get() );
            pStage.reset( pNew );
            pDst = pNew + iUsed;
            pStageEnd = pNew + iCap;
        };
        auto Flush = [&]( uint64_t llNextGroup ) {  // the staged groups [llChunkFirst, llNextGroup) become a chunk
            if ( MODE != Fused ) return;
            size_t iRaw = (size_t)( pDst - pStage.get() );
            if ( iRaw )
            {
                TrackScan::PackedChunk c;
                std::unique_ptr< uint8_t[] > pTmp( new uint8_t[Lz4::Bound( iRaw )] );
                size_t iSize = Lz4::Compress( pStage.get(), iRaw, pTmp.get() );
                const uint8_t *pSrc = pTmp.get();
                if ( iSize >= iRaw ) { iSize = iRaw; pSrc = pStage.get(); }
                c.data.reset( new uint8_t[iSize + Lz4::Slack] );
                memcpy( c.data.get(), pSrc, iSize );
                c.uPacked = (uint32_t)iSize;
                c.uRaw = (uint32_t)iRaw;
                c.llFirstGroup = llChunkFirst;
                pScan->llPackedBytes += iSize;
                pScan->vPacked.push_back( std::move( c ) );
            }
            pDst = pStage.get();
            llChunkFirst = llNextGroup;
        };

        auto CloseRun = [&]() { if ( WRITE && pLastKey ) { *pLastKey |= 0x80; pLastKey = nullptr; } uRunKey = UINT32_MAX; };
        auto CloseGroup = [&]() { CloseRun(); if ( WRITE && pRunHeader ) { *pRunHeader |= 0x10; pRunHeader = nullptr; } };
        auto AddInfo = [&]( long long llAt ) {
            if ( COUNT ) { pScan->info.llEventCount++; pScan->info.llTotalTicks = std::max( pScan->info.llTotalTicks, llAt ); }
        };

        while ( p < pEnd )
        {
            const uint8_t *pEventStart = p;
            long long llTickBefore = llTick;
            int iRunningBefore = iRunning;

            int iStatus;
            long long llDelta;
            if ( !ReadVarLen( p, pEnd, llDelta ) ) { if ( COUNT ) pScan->info.bMalformed = true; break; }
            llTick += llDelta;
            if ( p >= pEnd ) { if ( COUNT ) pScan->info.bMalformed = true; break; }
            if ( *p >= 0x80 ) iStatus = *p++;
            else if ( iRunning ) iStatus = iRunning;
            else { if ( COUNT ) pScan->info.bMalformed = true; break; }

            if ( iStatus < 0xF0 )
            {
                iRunning = iStatus;
                unsigned iType = iStatus >> 4, iChannel = iStatus & 0xF;
                int iDataBytes = ( iType == 0xC || iType == 0xD ) ? 1 : 2;
                if ( pEnd - p < iDataBytes ) { if ( COUNT ) pScan->info.bMalformed = true; break; }
                // Data bytes are read as data, as the original reads them (DESIGN.md 1.5). A key, controller
                // or program past 127 drops the event (its tick still counts for the length); a second
                // byte past 127 (velocity, value) is clamped
                uint8_t aData[2] = { p[0], iDataBytes == 2 ? std::min< uint8_t >( p[1], 0x7F ) : (uint8_t)0 };
                p += iDataBytes;
                if ( aData[0] >= 0x80 )
                {
                    if ( COUNT ) { pScan->llOutOfRange++; pScan->info.llTotalTicks = std::max( pScan->info.llTotalTicks, llTick ); }
                    continue;
                }

                // A channel event at a new tick opens the next group
                if ( llTick != llGroupTick )
                {
                    uint64_t llNext = (uint64_t)( llGroup + 1 );
                    CloseGroup();
                    if ( MODE == Emit && llNext >= llStopGroup ) break;
                    if ( COUNT )
                    {
                        if ( llGroup >= 0 ) pScan->vGroups.push_back( { llGroupTick, uSize, uEvents } );
                        if ( MODE == Count && llNext > 0 && llNext % CheckpointEvery == 0 )
                            pScan->vCps.push_back( { (uint64_t)( pEventStart - pTrack ), llTickBefore, llNext, (uint8_t)iRunningBefore } );
                        uSize = uEvents = 0;
                    }
                    if ( MODE == Emit ) pDst = pPayload + pGroups[llNext].llTick;
                    else if ( MODE == Fused && pDst - pStage.get() >= (ptrdiff_t)FusedChunk ) Flush( llNext );
                    llGroup = (int64_t)llNext;
                    llGroupTick = llTick;
                }

                unsigned op = OpOf( iType, aData[1] );
                Reserve();
                if ( op <= 2 )
                {
                    uint32_t uKey = ( op << 12 ) | ( iChannel << 8 ) | ( op == 2 ? 0 : aData[1] );
                    if ( uKey == uRunKey )
                    {
                        if ( WRITE ) { pLastKey = pDst; *pDst++ = aData[0]; }
                        if ( COUNT ) uSize += 1;
                    }
                    else
                    {
                        CloseRun();
                        uRunKey = uKey;
                        if ( WRITE )
                        {
                            pRunHeader = pDst;
                            *pDst++ = (uint8_t)( ( op << 5 ) | iChannel );
                            if ( op != 2 ) *pDst++ = aData[1];
                            pLastKey = pDst;
                            *pDst++ = aData[0];
                        }
                        if ( COUNT ) uSize += ( op == 2 ? 2 : 3 );
                    }
                }
                else
                {
                    CloseRun();
                    if ( WRITE )
                    {
                        pRunHeader = pDst;
                        *pDst++ = (uint8_t)( ( op << 5 ) | iChannel );
                        *pDst++ = aData[0];
                        if ( iDataBytes == 2 ) *pDst++ = aData[1];
                    }
                    if ( COUNT ) uSize += 1 + iDataBytes;
                }

                if ( COUNT )
                {
                    uEvents++;
                    pScan->llChannelEvents++;
                    AddInfo( llTick );
                    RefTrackInfo &info = pScan->info;
                    // MIDITrackInfo::AddEventInfo
                    if ( iType == 0x9 && aData[1] > 0 )
                    {
                        if ( !info.llNoteCount )
                        {
                            info.iMinNote = info.iMaxNote = aData[0];
                            info.iMaxVolume = aData[1];
                        }
                        else
                        {
                            info.iMinNote = std::min< int >( aData[0], info.iMinNote );
                            info.iMaxNote = std::max< int >( aData[0], info.iMaxNote );
                            info.iMaxVolume = std::max< int >( aData[1], info.iMaxVolume );
                        }
                        info.llNoteCount++;
                        info.llVolumeSum += aData[1];
                        if ( !info.aNoteCount[iChannel] ) info.iNumChannels++;
                        info.aNoteCount[iChannel]++;
                        pScan->aKeyCounts[aData[0]]++;
                    }
                    else if ( iType == 0xC && info.aProgram[iChannel] != aData[0] )
                        info.aProgram[iChannel] = info.aNoteCount[iChannel] > 0 ? 128 : aData[0];
                }
            }
            else if ( iStatus == 0xF0 || iStatus == 0xF7 )
            {
                iRunning = 0;
                long long llLen;
                if ( !ReadVarLen( p, pEnd, llLen ) || llLen > pEnd - p ) { if ( COUNT ) pScan->info.bMalformed = true; break; }
                p += llLen; // SysEx is ignored entirely
                AddInfo( llTick );
            }
            else if ( iStatus == 0xFF )
            {
                iRunning = 0;
                if ( p >= pEnd ) { if ( COUNT ) pScan->info.bMalformed = true; break; }
                uint8_t cType = *p++;
                long long llLen;
                if ( !ReadVarLen( p, pEnd, llLen ) || llLen > pEnd - p ) { if ( COUNT ) pScan->info.bMalformed = true; break; }
                const uint8_t *pMeta = p;
                p += llLen;
                AddInfo( llTick );
                if ( cType == 0x2F ) break;
                if ( COUNT )
                {
                    if ( ( cType == RefMetaEvent::SetTempo && llLen == 3 ) || ( cType == RefMetaEvent::TimeSignature && llLen == 4 ) )
                    {
                        RefMetaEvent m;
                        m.llTick = llTick;
                        m.iTrack = iTrack;
                        m.cType = cType;
                        m.iDataLen = (int)llLen;
                        memcpy( m.aData, pMeta, (size_t)llLen );
                        pScan->vMeta.push_back( m );
                    }
                }
            }
            else
            {
                if ( COUNT ) pScan->info.bMalformed = true; // 0xF1-0xFE are not valid in a Standard MIDI File
                break;
            }
        }

        CloseGroup();
        if ( COUNT && llGroup >= 0 ) pScan->vGroups.push_back( { llGroupTick, uSize, uEvents } );
        Flush( (uint64_t)( llGroup + 1 ) );
    }

    double Seconds( long long llFrom ) { return ( Timer::NowMicroSecs() - llFrom ) / 1e6; }

    uint64_t PrivateBytes() { return Util::ProcessMemory(); }

    std::unique_ptr< Song > LoadImpl( const uint8_t *pData, uint64_t llSize, MappedFile *pFile, int iThreads,
                                      LoadProgress &prog, LoadStats &stats, std::wstring &sError )
    {
        long long llStart = Timer::NowMicroSecs();
        long long llStep = llStart;
        // Ends a part of the current stage (LoadStats::vSteps)
        auto Step = [&]( const wchar_t *sName ) {
            long long t = Timer::NowMicroSecs();
            stats.vSteps.push_back( { prog.iStage.load(), sName, ( t - llStep ) / 1e6, PrivateBytes() } );
            llStep = t;
        };
        iThreads = Parallel::ThreadCount( iThreads );
        stats.iThreads = iThreads;
        stats.llFileSize = llSize;
        prog.llFileSize = llSize;
        bool bSequential = pFile && pFile->SeekPenalty();
        stats.bSeekPenalty = pFile && pFile->SeekPenalty();
        stats.bSequentialRead = bSequential;
        // Fused single read (4.0, 4.2a) when the emit pass could not be served from the page cache
        bool bFused = prog.iFusedRead > 0;
        if ( prog.iFusedRead < 0 && bSequential )
        {
            MEMORYSTATUSEX ms = { sizeof( ms ) };
            bFused = GlobalMemoryStatusEx( &ms ) && (double)llSize > 0.6 * (double)ms.ullAvailPhys;
        }
        stats.bFusedRead = bFused;
        auto Cancelled = [&]() { return prog.bCancel.load( std::memory_order_relaxed ); };
        auto WaitBytes = [&]( uint64_t llEnd ) {
            if ( !bSequential ) return;
            if ( pFile->Resident() < llEnd ) { prog.bWaitingOnDisk = true; pFile->WaitResident( llEnd, &prog.bCancel ); prog.bWaitingOnDisk = false; }
        };

        // Phase 0/1: header
        prog.iStage = LoadProgress::Reading;
        prog.aTotal[LoadProgress::Reading] = llSize;
        if ( bSequential ) pFile->StartSequentialRead( &prog.bCancel );
        else if ( pFile ) pFile->PrefetchAll();
        WaitBytes( std::min< uint64_t >( 14, llSize ) );
        if ( llSize < 14 || memcmp( pData, "MThd", 4 ) != 0 ) { sError = L"Not a Standard MIDI File (no MThd header)."; return nullptr; }
        auto BE = []( const uint8_t *q, int n ) { uint64_t v = 0; for ( int i = 0; i < n; i++ ) v = ( v << 8 ) | q[i]; return v; };
        uint64_t llHdr = std::max< uint64_t >( BE( pData + 4, 4 ), 6 );
        auto song = std::make_unique< Song >();
        RefMidiInfo &info = song->info;
        info.iFormatType = (int)BE( pData + 8, 2 );
        info.iDivision = (int)BE( pData + 12, 2 );
        if ( info.iFormatType > 2 || info.iDivision == 0 ) { sError = L"Unsupported MIDI header (format or division)."; return nullptr; }

        // Phase 1+2: chunk discovery and count pass. Workers claim the next chunk under a lock; on an HDD
        // discovery and parsing follow the sequential reader, so the disk only sees one forward stream.
        long long llPhase = Timer::NowMicroSecs();
        prog.iStage = LoadProgress::Counting;
        prog.aTotal[LoadProgress::Counting] = llSize;
        std::mutex mtx;
        uint64_t llNextChunk = 8 + llHdr;
        std::vector< std::unique_ptr< TrackScan > > vScans;
        std::atomic< uint64_t > llBytesCounted{ 0 };
        Parallel::For( iThreads, (size_t)iThreads, [&]( size_t, int ) {
            for ( ;; )
            {
                TrackScan *pScan = nullptr;
                int iTrack = -1;
                {
                    std::lock_guard< std::mutex > lock( mtx );
                    while ( !pScan && !Cancelled() && llNextChunk + 8 <= llSize )
                    {
                        WaitBytes( llNextChunk + 8 );
                        const uint8_t *pChunk = pData + llNextChunk;
                        uint64_t llBegin = llNextChunk + 8;
                        uint64_t llEnd = std::min< uint64_t >( llBegin + BE( pChunk + 4, 4 ), llSize );
                        llNextChunk = llEnd;
                        if ( memcmp( pChunk, "MTrk", 4 ) != 0 ) continue; // unknown chunk: skipped
                        iTrack = (int)vScans.size();
                        vScans.push_back( std::make_unique< TrackScan >() );
                        pScan = vScans.back().get();
                        pScan->llBegin = llBegin;
                        pScan->llEnd = llEnd;
                        prog.llTracksFound = vScans.size();
                    }
                }
                if ( !pScan ) return;
                WaitBytes( pScan->llEnd );
                if ( bFused ) ScanTrack< Fused >( pData + pScan->llBegin, pData + pScan->llEnd, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, iTrack, pScan, nullptr, nullptr );
                else ScanTrack< Count >( pData + pScan->llBegin, pData + pScan->llEnd, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, iTrack, pScan, nullptr, nullptr );
                prog.llEvents += pScan->llChannelEvents;
                prog.llNotes += pScan->info.llNoteCount;
                prog.llTracksDone++;
                prog.aDone[LoadProgress::Counting] = llBytesCounted += pScan->llEnd - pScan->llBegin + 8;
                if ( bSequential ) prog.aDone[LoadProgress::Reading] = pFile->Resident();
            }
        } );
        if ( bSequential ) pFile->StopSequentialRead();
        if ( bSequential ) stats.dReaderSeconds = pFile->ReaderMicroSecs() / 1e6;
        if ( bFused && pFile ) pFile->Close(); // nothing reads the file after the fused count pass
        prog.aDone[LoadProgress::Reading] = llSize;
        prog.aDone[LoadProgress::Counting] = llSize;
        stats.adStageSeconds[LoadProgress::Counting] = Seconds( llPhase );
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }

        // Track info aggregation (MIDIInfo::AddTrackInfo)
        song->vTracks.resize( vScans.size() );
        song->vTrackMeta.resize( vScans.size() );
        for ( size_t t = 0; t < vScans.size(); t++ )
        {
            const RefTrackInfo &mti = vScans[t]->info;
            song->vTracks[t] = mti;
            song->vTrackMeta[t] = vScans[t]->vMeta;
            stats.llMalformedTracks += mti.bMalformed;
            stats.llOutOfRange += vScans[t]->llOutOfRange;
            for ( int k = 0; k < 128; k++ ) song->aKeyNoteCounts[k] += vScans[t]->aKeyCounts[k];
            info.llTotalTicks = std::max( info.llTotalTicks, mti.llTotalTicks );
            info.llEventCount += mti.llEventCount;
            info.iNumChannels += mti.iNumChannels;
            info.llVolumeSum += mti.llVolumeSum;
            if ( mti.llNoteCount )
            {
                if ( !info.llNoteCount ) { info.iMinNote = mti.iMinNote; info.iMaxNote = mti.iMaxNote; info.iMaxVolume = mti.iMaxVolume; }
                else
                {
                    info.iMinNote = std::min( mti.iMinNote, info.iMinNote );
                    info.iMaxNote = std::max( mti.iMaxNote, info.iMaxNote );
                    info.iMaxVolume = std::max( mti.iMaxVolume, info.iMaxVolume );
                }
            }
            info.llNoteCount += mti.llNoteCount;
        }
        info.iNumTracks = (int)vScans.size();
        info.llFirstNote = -1;
        stats.llTracks = vScans.size();
        for ( auto &s : vScans ) stats.llFusedBytes += s->llPackedBytes;
        stats.llNotes = info.llNoteCount;

        // Phase 3: merge
        llPhase = Timer::NowMicroSecs();
        prog.llCountEndUs = llPhase;
        prog.iStage = LoadProgress::Merging;
        uint64_t G = 0, llEvents = 0;
        int64_t llMaxTick = 0;
        for ( auto &s : vScans )
        {
            G += s->vGroups.size();
            llEvents += s->llChannelEvents;
            if ( !s->vGroups.empty() ) llMaxTick = std::max( llMaxTick, s->vGroups.back().llTick );
        }
        prog.aTotal[LoadProgress::Merging] = 4;
        EventStream &es = song->stream;
        es.llEvents = llEvents;
        // Transient (global order): every group's track << 32 | run bytes (then, once its header is
        // written, the offset of its runs in the raw stream), and each tick's first group
        std::vector< uint64_t > vTickGroupStart;
        std::vector< uint64_t > vGroups;
        stats.llEvents = llEvents;
        stats.llGroups = G;

        std::vector< int64_t > vUnique;
        std::vector< uint64_t > vBits, vWordRank;
        uint64_t llWords = (uint64_t)llMaxTick / 64 + 1;
        bool bBitmap = G > 0 && llWords * 16 <= std::max< uint64_t >( 256ull << 20, G * 8 );
        stats.bBitmapMerge = bBitmap;
        if ( bBitmap )
        {
            vBits.assign( llWords, 0 );
            Parallel::For( iThreads, vScans.size(), [&]( size_t t, int ) {
                for ( const GroupRec &g : vScans[t]->vGroups )
                    __atomic_fetch_or( &vBits[(uint64_t)g.llTick >> 6], 1ull << ( g.llTick & 63 ), __ATOMIC_RELAXED );
            } );
            vWordRank.resize( llWords + 1 );
            uint64_t r = 0;
            for ( uint64_t w = 0; w < llWords; w++ ) { vWordRank[w] = r; r += __builtin_popcountll( vBits[w] ); }
            vWordRank[llWords] = r;
            vUnique.resize( r );
            Parallel::For( iThreads, (size_t)( ( llWords + 4095 ) / 4096 ), [&]( size_t c, int ) {
                for ( uint64_t w = c * 4096; w < std::min< uint64_t >( llWords, ( c + 1 ) * 4096 ); w++ )
                {
                    uint64_t b = vBits[w], k = vWordRank[w];
                    while ( b ) { vUnique[k++] = (int64_t)( w * 64 + __builtin_ctzll( b ) ); b &= b - 1; }
                }
            } );
        }
        else
        {
            vUnique.reserve( G );
            for ( auto &s : vScans ) for ( const GroupRec &g : s->vGroups ) vUnique.push_back( g.llTick );
            std::sort( vUnique.begin(), vUnique.end() );
            vUnique.erase( std::unique( vUnique.begin(), vUnique.end() ), vUnique.end() );
        }
        const uint64_t U = vUnique.size();
        auto Rank = [&]( int64_t t ) -> uint64_t {
            if ( bBitmap )
            {
                uint64_t w = (uint64_t)t >> 6;
                return vWordRank[w] + __builtin_popcountll( vBits[w] & ( ( 1ull << ( t & 63 ) ) - 1 ) );
            }
            return (uint64_t)( std::lower_bound( vUnique.begin(), vUnique.end(), t ) - vUnique.begin() );
        };
        // The same for a tick at or after the one ranked uHint (a track's ticks only increase): without
        // the bitmap, a search forwards from uHint, a few steps when the track's groups are close
        auto RankFrom = [&]( int64_t t, uint64_t uHint ) -> uint64_t {
            if ( bBitmap ) return Rank( t );
            uint64_t lo = uHint, step = 1, hi;
            for ( ;; )
            {
                hi = lo + step;
                if ( hi >= vUnique.size() ) { hi = vUnique.size(); break; }
                if ( vUnique[hi] >= t ) break;
                lo = hi;
                step *= 2;
            }
            return (uint64_t)( std::lower_bound( vUnique.begin() + lo, vUnique.begin() + hi, t ) - vUnique.begin() );
        };
        stats.llUniqueTicks = U;
        prog.aDone[LoadProgress::Merging] = 1;
        llStep = llPhase;
        Step( bBitmap ? L"unique ticks (bitmap)" : L"unique ticks (sort)" );

        // Groups per unique tick, then each group's slot in global (tick, track) order
        std::vector< uint32_t > vCount( U, 0 );
        Parallel::For( iThreads, vScans.size(), [&]( size_t t, int ) {
            uint64_t u = 0;
            for ( const GroupRec &g : vScans[t]->vGroups )
            {
                u = RankFrom( g.llTick, u );
                __atomic_fetch_add( &vCount[u], 1u, __ATOMIC_RELAXED );
            }
        } );
        vTickGroupStart.resize( U + 1 );
        uint64_t acc = 0;
        for ( uint64_t u = 0; u < U; u++ ) { vTickGroupStart[u] = acc; acc += vCount[u]; }
        vTickGroupStart[U] = acc;
        std::fill( vCount.begin(), vCount.end(), 0 );
        vGroups.resize( G );
        song->vTrackLastU.assign( vScans.size(), UINT64_MAX );
        for ( size_t t = 0; t < vScans.size(); t++ )
            if ( !vScans[t]->vGroups.empty() ) song->vTrackLastU[t] = Rank( vScans[t]->vGroups.back().llTick );
        prog.aDone[LoadProgress::Merging] = 2;

        // Tick ranges are owned by one thread each, and tracks are visited in order, so the groups of a
        // tick land in ascending track order with no atomics
        size_t iRanges = std::max< size_t >( 1, std::min< uint64_t >( U, (uint64_t)iThreads * 8 ) );
        Parallel::For( iThreads, iRanges, [&]( size_t r, int ) {
            uint64_t u0 = U * r / iRanges, u1 = U * ( r + 1 ) / iRanges;
            if ( u0 >= u1 ) return;
            int64_t t0 = vUnique[u0], t1 = u1 < U ? vUnique[u1] : INT64_MAX;
            for ( size_t t = 0; t < vScans.size(); t++ )
            {
                std::vector< GroupRec > &vg = vScans[t]->vGroups;
                auto it = std::lower_bound( vg.begin(), vg.end(), t0, []( const GroupRec &g, int64_t v ) { return g.llTick < v; } );
                uint64_t u = u0;
                for ( ; it != vg.end() && it->llTick < t1; ++it )
                {
                    u = RankFrom( it->llTick, u );
                    uint64_t slot = vTickGroupStart[u] + vCount[u]++;
                    vGroups[slot] = ( (uint64_t)t << 32 ) | it->uSize;
                    // The size now lives in the global entry; keep the slot in the size/event fields.
                    // llTick must stay intact: other threads binary-search this track by tick.
                    it->uSize = (uint32_t)slot;
                    it->uEvents = (uint32_t)( slot >> 32 );
                }
            }
        } );
        vCount.clear(); vCount.shrink_to_fit();
        vBits.clear(); vBits.shrink_to_fit();
        vWordRank.clear(); vWordRank.shrink_to_fit();
        prog.aDone[LoadProgress::Merging] = 3;
        Step( L"group order" );

        // Raw offsets: prefix sum in global order over group header + runs, with a seek point for the
        // first tick that starts in each chunk, kept only where each tick range starts; then each range
        // writes its group headers, and each group's entry becomes the offset of its runs, which the
        // track's group records take (no per-group offset table: 8 bytes a group at the load's peak)
        auto Track = [&]( uint64_t g ) { return (uint32_t)( vGroups[g] >> 32 ); };
        auto Header = [&]( uint64_t g, uint64_t u, uint32_t uPrevTrack ) {
            uint64_t llGap = g == vTickGroupStart[u] ? Track( g ) : Track( g ) - uPrevTrack - 1;
            return ( llGap << 1 ) | ( g + 1 == vTickGroupStart[u + 1] ? 1 : 0 );
        };
        std::vector< uint64_t > vRangeOff( iRanges + 1 );
        uint64_t llOff = 0;
        for ( uint64_t u = 0, g = 0, r = 0; u < U; u++ )
        {
            while ( r < iRanges && U * r / iRanges == u ) vRangeOff[r++] = llOff;
            if ( es.vSeek.empty() || ( es.vSeek.back().llRaw >> EventStream::ChunkShift ) != ( llOff >> EventStream::ChunkShift ) )
                es.vSeek.push_back( { u, llOff } );
            for ( ; g < vTickGroupStart[u + 1]; g++ )
                llOff += EventStream::VarSize( Header( g, u, g ? Track( g - 1 ) : 0 ) ) + (uint32_t)vGroups[g];
        }
        es.llRawBytes = llOff;
        std::unique_ptr< uint8_t[] > pRaw( new ( std::nothrow ) uint8_t[llOff + 16] );
        if ( !pRaw ) { sError = L"Out of memory for the event stream."; return nullptr; }
        Parallel::For( iThreads, iRanges, [&]( size_t r, int ) {
            uint64_t u0 = U * r / iRanges, u1 = U * ( r + 1 ) / iRanges;
            uint64_t llAt = vRangeOff[r];
            uint32_t uPrevTrack = 0;
            for ( uint64_t u = u0; u < u1; u++ )
                for ( uint64_t g = vTickGroupStart[u]; g < vTickGroupStart[u + 1]; g++ )
                {
                    uint32_t uTrack = Track( g ), uSize = (uint32_t)vGroups[g];
                    uint8_t *p = EventStream::WriteVar( pRaw.get() + llAt, Header( g, u, uPrevTrack ) );
                    vGroups[g] = (uint64_t)( p - pRaw.get() );
                    llAt = vGroups[g] + uSize;
                    uPrevTrack = uTrack;
                }
        } );
        Parallel::For( iThreads, vScans.size(), [&]( size_t t, int ) {
            for ( GroupRec &g : vScans[t]->vGroups ) g.llTick = (int64_t)vGroups[g.uSize | ( (uint64_t)g.uEvents << 32 )];
        } );
        vGroups.clear(); vGroups.shrink_to_fit();
        vTickGroupStart.clear(); vTickGroupStart.shrink_to_fit();
        Step( L"offsets and headers" );

        // Meta in global order, tempo map, TimeTable, packed tick table
        for ( size_t t = 0; t < vScans.size(); t++ ) song->vMeta.insert( song->vMeta.end(), vScans[t]->vMeta.begin(), vScans[t]->vMeta.end() );
        std::stable_sort( song->vMeta.begin(), song->vMeta.end(), []( const RefMetaEvent &a, const RefMetaEvent &b ) {
            return a.llTick != b.llTick ? a.llTick < b.llTick : a.iTrack < b.iTrack; } );
        TempoMap tempo;
        tempo.Build( song->vMeta, info.iDivision );
        for ( RefMetaEvent &m : song->vMeta ) m.llMicroSec = tempo.At( m.llTick );
        for ( auto &v : song->vTrackMeta ) for ( RefMetaEvent &m : v ) m.llMicroSec = tempo.At( m.llTick );
        info.llTotalMicroSecs = tempo.At( info.llTotalTicks );
        es.ticks.Build( U, [&]( size_t u ) { return (uint64_t)vUnique[u]; }, iThreads );
        es.times.Build( U, [&]( size_t u ) { return (uint64_t)tempo.At( vUnique[u] ); }, iThreads );
        vUnique.clear(); vUnique.shrink_to_fit();
        prog.aDone[LoadProgress::Merging] = 4;
        Step( L"tempo, tick, time tables" );
        stats.adStageSeconds[LoadProgress::Merging] = Seconds( llPhase );
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }

        // Phase 4: the payload. Two-pass: parse the file again, work items are track segments between
        // checkpoints. Fused: copy each group out of its track's packed chunks, a chunk per work item.
        llPhase = llStep = Timer::NowMicroSecs();
        prog.iStage = LoadProgress::Building;
        struct Item { uint32_t uTrack; uint32_t uSeg; };
        std::vector< Item > vItems;
        for ( size_t t = 0; t < vScans.size(); t++ )
            if ( bFused )
                for ( uint32_t k = 0; k < vScans[t]->vPacked.size(); k++ ) vItems.push_back( { (uint32_t)t, k } );
            else if ( !vScans[t]->vGroups.empty() )
                for ( uint32_t k = 0; k <= vScans[t]->vCps.size(); k++ ) vItems.push_back( { (uint32_t)t, k } );
        prog.aTotal[LoadProgress::Building] = vItems.size();
        std::atomic< uint64_t > llItemsDone{ 0 };
        std::vector< std::vector< uint8_t > > vUnpack( bFused ? iThreads : 0 );
        Parallel::For( iThreads, vItems.size(), [&]( size_t i, int iThread ) {
            if ( Cancelled() ) return;
            TrackScan &s = *vScans[vItems[i].uTrack];
            uint32_t k = vItems[i].uSeg;
            if ( bFused )
            {
                TrackScan::PackedChunk &c = s.vPacked[k];
                const uint8_t *p = c.data.get();
                if ( c.uPacked != c.uRaw )
                {
                    std::vector< uint8_t > &v = vUnpack[iThread];
                    if ( v.size() < c.uRaw + Lz4::Slack ) v.resize( c.uRaw + Lz4::Slack );
                    Lz4::Decompress( c.data.get(), c.uPacked, v.data(), c.uRaw );
                    p = v.data();
                }
                // A group ends with the run whose header has bit 4 set
                const uint8_t *pEnd = p + c.uRaw;
                for ( uint64_t g = c.llFirstGroup; p < pEnd; g++ )
                {
                    const uint8_t *q = p;
                    unsigned h;
                    do
                    {
                        h = *q++;
                        unsigned op = h >> 5;
                        if ( op <= 2 ) { q += op != 2; while ( !( *q++ & 0x80 ) ) {} }
                        else q += ( op == 5 || op == 6 ) ? 1 : 2;
                    } while ( !( h & 0x10 ) );
                    memcpy( pRaw.get() + s.vGroups[g].llTick, p, (size_t)( q - p ) );
                    p = q;
                }
                c.data.reset();
            }
            else
            {
                Checkpoint start = k == 0 ? Checkpoint{ 0, 0, 0, 0 } : s.vCps[k - 1];
                uint64_t llStop = k < s.vCps.size() ? s.vCps[k].llGroup : UINT64_MAX;
                ScanTrack< Emit >( pData + s.llBegin, pData + s.llEnd, start, llStop, (int)vItems[i].uTrack, nullptr, s.vGroups.data(), pRaw.get() );
            }
            prog.aDone[LoadProgress::Building] = ++llItemsDone;
        } );
        vUnpack.clear();
        // The per-track scans (group records, checkpoints, packed tracks) are not needed after the
        // build; freed here rather than after pairing, where they would add to its peak
        vScans.clear();
        vScans.shrink_to_fit();
        if ( pFile ) pFile->Close(); // the mapping is not needed after the build (4.0)
        Step( bFused ? L"copy from packed tracks" : L"parse into the stream" );
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }

        // LZ4 per 64 KiB chunk (stored as is when that is not smaller), then one allocation
        size_t iChunks = (size_t)( ( es.llRawBytes + EventStream::ChunkSize - 1 ) >> EventStream::ChunkShift );
        es.vChunks.resize( iChunks );
        std::vector< std::unique_ptr< uint8_t[] > > vPacked( iChunks );
        Parallel::For( iThreads, iChunks, [&]( size_t c, int ) {
            const uint8_t *pSrc = pRaw.get() + ( (uint64_t)c << EventStream::ChunkShift );
            size_t iRaw = es.ChunkRaw( c );
            std::unique_ptr< uint8_t[] > pTmp( new uint8_t[Lz4::Bound( iRaw )] );
            size_t iSize = Lz4::Compress( pSrc, iRaw, pTmp.get() );
            if ( iSize >= iRaw ) { iSize = iRaw; memcpy( pTmp.get(), pSrc, iRaw ); }
            vPacked[c].reset( new uint8_t[iSize] );
            memcpy( vPacked[c].get(), pTmp.get(), iSize );
            es.vChunks[c].uSize = (uint32_t)iSize;
        } );
        Step( L"LZ4 chunks" );
        pRaw.reset();
        uint64_t llData = 0;
        for ( size_t c = 0; c < iChunks; c++ ) { es.vChunks[c].llOffset = llData; llData += es.vChunks[c].uSize; }
        es.llDataBytes = llData;
        es.data.reset( new ( std::nothrow ) uint8_t[llData + Lz4::Slack] );
        if ( !es.data ) { sError = L"Out of memory for the event stream."; return nullptr; }
        for ( size_t c = 0; c < iChunks; c++ ) { memcpy( es.data.get() + es.vChunks[c].llOffset, vPacked[c].get(), es.vChunks[c].uSize ); vPacked[c].reset(); }
        Step( L"stream into one block" );
        stats.adStageSeconds[LoadProgress::Building] = Seconds( llPhase );
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }

        // Pairing and the visual store
        llPhase = Timer::NowMicroSecs();
        prog.aTotal[LoadProgress::Pairing] = es.UniqueTicks();
        prog.iStage = LoadProgress::Pairing;
        song->bFirstPairingOnly = prog.bFirstPairingOnly;
        if ( !prog.bSkipPairing && !BuildVisualStore( *song, iThreads, &prog.aDone[LoadProgress::Pairing], &prog.bCancel, !prog.bFullVisual, prog.bLzVisual, prog.bNoteStats ) )
        { sError = L"Cancelled."; return nullptr; }
        song->info.llFirstNote = std::max( 0LL, song->visual.llFirstNote );
        stats.llPairedNotes = song->visual.llNotes;
        stats.llUnpairedNotes = song->visual.llUnpaired;
        stats.llWeirdNotes = song->visual.llWeirdNotes;
        stats.adStageSeconds[LoadProgress::Pairing] = Seconds( llPhase );
        {
            static const wchar_t *const aNames[VisualStore::StepCount] = { L"pairing pass", L"finishing keys", L"jump tables",
                                                                            L"pairing again (weird)", L"compacting / trees", L"LZ groups" };
            for ( int i = 0; i < VisualStore::StepCount; i++ )
                if ( song->visual.adStepSeconds[i] > 0 )
                    stats.vSteps.push_back( { LoadProgress::Pairing, aNames[i], song->visual.adStepSeconds[i], song->visual.adStepPrivate[i] } );
            llStep = Timer::NowMicroSecs();
        }

        // Finalising
        llPhase = Timer::NowMicroSecs();
        prog.iStage = LoadProgress::Finalising;
        song->BuildChaseIndex( iThreads );
        song->BuildSendCounts( iThreads );
        stats.llPayloadBytes = es.llRawBytes;
        stats.aMem[0] = (size_t)es.llDataBytes;
        stats.aMem[1] = es.vChunks.capacity() * sizeof( EventStream::Chunk ) + es.vSeek.capacity() * sizeof( EventStream::SeekPoint );
        stats.aMem[2] = es.ticks.MemoryBytes();
        stats.aMem[3] = es.times.MemoryBytes();
        stats.aMem[4] = 0;
        stats.aMem[6] = song->visual.MemoryBytes();
        stats.aMem[5] = song->MemoryBytes() - es.MemoryBytes() - stats.aMem[6]; // includes the chase index
        stats.adStageSeconds[LoadProgress::Finalising] = Seconds( llPhase );
        stats.dTotalSeconds = Seconds( llStart );
        prog.aTotal[LoadProgress::Finalising] = prog.aDone[LoadProgress::Finalising] = 1;
        prog.iStage = LoadProgress::StageCount;
        return song;
    }
}

namespace
{
    // Samples the process's private bytes every 2 ms while a load runs: the current value and peak
    // for the loading popup, and the peak of each stage for the statistics. Short spikes between
    // samples are caught by the lifetime peak: when it rose during the load, it is the load's peak.
    class MemorySampler
    {
    public:
        MemorySampler( LoadProgress &prog, LoadStats &stats ) : m_Prog( prog ), m_Stats( stats )
        {
            m_Stats.llPrivateBefore = PrivateBytes();
            m_Prog.llPrivateNow = m_Prog.llPrivatePeak = m_Stats.llPrivatePeak = m_Stats.llPrivateBefore;
            m_Thread = std::thread( [this] {
                std::unique_lock< std::mutex > lock( m_mx );
                while ( !m_bStop )
                {
                    Sample();
                    m_cv.wait_for( lock, std::chrono::milliseconds( 2 ) );
                }
            } );
        }
        ~MemorySampler()
        {
            { std::lock_guard< std::mutex > lock( m_mx ); m_bStop = true; }
            m_cv.notify_all();
            m_Thread.join();
            Sample();
            m_Stats.llPrivateAfter = PrivateBytes();
            m_Prog.llPrivatePeak = m_Stats.llPrivatePeak;
        }

    private:
        void Sample()
        {
            uint64_t ll = PrivateBytes();
            m_Prog.llPrivateNow = ll;
            if ( ll > m_Stats.llPrivatePeak ) m_Prog.llPrivatePeak = m_Stats.llPrivatePeak = ll;
            int iStage = m_Prog.iStage.load( std::memory_order_relaxed );
            if ( iStage >= 0 && iStage < LoadProgress::StageCount ) m_Stats.aStagePeak[iStage] = std::max( m_Stats.aStagePeak[iStage], ll );
        }

        LoadProgress &m_Prog;
        LoadStats &m_Stats;
        std::mutex m_mx;
        std::condition_variable m_cv;
        bool m_bStop = false;
        std::thread m_Thread;
    };
}

std::unique_ptr< Song > Loader::LoadFile( const std::wstring &sPath, int iThreads, LoadProgress &progress, LoadStats &stats, std::wstring &sError )
{
    MemorySampler sampler( progress, stats );
    long long llStart = Timer::NowMicroSecs();
    stats.sFile = sPath;
    MappedFile file;
    progress.iStage = LoadProgress::Reading;
    if ( !file.Open( sPath, sError ) ) return nullptr;
    stats.adStageSeconds[LoadProgress::Reading] = ( Timer::NowMicroSecs() - llStart ) / 1e6;
    std::unique_ptr< Song > song = LoadImpl( file.Data(), file.Size(), &file, iThreads, progress, stats, sError );
    if ( song ) song->info.sFilename = sPath;
    stats.dTotalSeconds = ( Timer::NowMicroSecs() - llStart ) / 1e6;
    return song;
}

std::unique_ptr< Song > Loader::LoadMemory( const uint8_t *pData, size_t iSize, int iThreads, LoadProgress &progress, LoadStats &stats, std::wstring &sError )
{
    MemorySampler sampler( progress, stats );
    return LoadImpl( pData, iSize, nullptr, iThreads, progress, stats, sError );
}

std::wstring LoadStats::Format() const
{
    auto MB = []( double b ) { return b / ( 1024.0 * 1024.0 ); };
    size_t iMem = 0;
    for ( size_t m : aMem ) iMem += m;
    wchar_t buf[8192];
    wchar_t aStages[2048] = L"";
    wchar_t sDisk[160] = L"";
    if ( dReaderSeconds > 0 ) swprintf( sDisk, 160, L"  (disk read %.0f MB/s)\r\n", llFileSize / 1048576.0 / dReaderSeconds );
    if ( bFusedRead ) swprintf( sDisk + wcslen( sDisk ), 80, L"  (packed tracks %.1f MB)\r\n", llFusedBytes / 1048576.0 );
    for ( int i = 0; i < LoadProgress::StageCount; i++ )
    {
        wchar_t s[96];
        if ( aStagePeak[i] ) swprintf( s, 96, L"  %-22ls %8.3f s   peak %7.0f MB\r\n", LoadProgress::StageNames[i], adStageSeconds[i], MB( (double)aStagePeak[i] ) );
        else swprintf( s, 96, L"  %-22ls %8.3f s\r\n", LoadProgress::StageNames[i], adStageSeconds[i] );
        wcscat( aStages, s );
        for ( const Step &st : vSteps )
            if ( st.iStage == i )
            {
                if ( st.llPrivate ) swprintf( s, 96, L"    %-20ls %8.3f s   then %7.0f MB\r\n", st.sName, st.dSeconds, MB( (double)st.llPrivate ) );
                else swprintf( s, 96, L"    %-20ls %8.3f s\r\n", st.sName, st.dSeconds );
                wcscat( aStages, s );
            }
    }
    wchar_t sOutOfRange[64] = L"";
    if ( llOutOfRange ) swprintf( sOutOfRange, 64, L" (%llu past key/value 127, dropped)", (unsigned long long)llOutOfRange );
    swprintf( buf, 8192,
        L"File: %ls\r\n"
        L"Size: %.1f MB, %ls%ls\r\n"
        L"Threads: %d\r\n\r\n"
        L"Tracks: %llu (%llu malformed)\r\n"
        L"Channel events: %llu%ls\r\n"
        L"Notes: %llu (%llu paired, %llu unpaired, %llu paired more than once)\r\n"
        L"Unique ticks: %llu\r\n"
        L"Tick groups: %llu (%ls merge)\r\n\r\n"
        L"Load time: %.3f s\r\n%ls%ls"
        L"Throughput: %.0f MB/s, %.1f M events/s\r\n\r\n"
        L"Process memory (private, in RAM): %.0f MB before, peak %.0f MB during the load (+%.0f MB), %.0f MB after\r\n\r\n"
        L"Memory: %.1f MB total\r\n"
        L"  Event stream (LZ4)   %8.1f MB  (%.3f bytes/event; %.3f raw)\r\n"
        L"  Stream index         %8.1f MB\r\n"
        L"  Tick table           %8.1f MB\r\n"
        L"  Time table           %8.1f MB\r\n"
        L"  Metadata             %8.1f MB\r\n"
        L"  Notes (visual store) %8.1f MB  (%.3f bytes/note)\r\n"
        L"  Overall              %.3f bytes/event\r\n",
        sFile.c_str(), MB( (double)llFileSize ),
        bSeekPenalty ? L"spinning disk" : L"SSD or cached",
        bFusedRead ? L" (one sequential read, fused parse)" : bSequentialRead ? L" (sequential reader)" : L"",
        iThreads,
        (unsigned long long)llTracks, (unsigned long long)llMalformedTracks, (unsigned long long)llEvents, sOutOfRange,
        (unsigned long long)llNotes, (unsigned long long)llPairedNotes, (unsigned long long)llUnpairedNotes, (unsigned long long)llWeirdNotes,
        (unsigned long long)llUniqueTicks, (unsigned long long)llGroups, bBitmapMerge ? L"bitmap" : L"sort",
        dTotalSeconds, aStages, sDisk,
        dTotalSeconds > 0 ? MB( (double)llFileSize ) / dTotalSeconds : 0.0, dTotalSeconds > 0 ? llEvents / dTotalSeconds / 1e6 : 0.0,
        MB( (double)llPrivateBefore ), MB( (double)llPrivatePeak ), MB( (double)( llPrivatePeak > llPrivateBefore ? llPrivatePeak - llPrivateBefore : 0 ) ),
        MB( (double)llPrivateAfter ),
        MB( (double)iMem ),
        MB( (double)aMem[0] ), llEvents ? (double)aMem[0] / llEvents : 0.0, llEvents ? (double)llPayloadBytes / llEvents : 0.0,
        MB( (double)aMem[1] ), MB( (double)aMem[2] ), MB( (double)aMem[3] ), MB( (double)aMem[5] ),
        MB( (double)aMem[6] ), llNotes ? (double)aMem[6] / llNotes : 0.0,
        llEvents ? (double)iMem / llEvents : 0.0 );
    return buf;
}
