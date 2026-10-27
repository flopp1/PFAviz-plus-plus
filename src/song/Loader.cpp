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
#include "song/Archive.h"
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

    enum ScanMode { Count, Emit, Fused, Stream };

    // 8 bytes (a song can have hundreds of millions of groups, held from the count pass to the build)
    struct GroupRec
    {
        uint32_t uA;        // to the build: the tick less the track's previous group's (TrackScan::AddGroup);
                            // from the build: the offset of the group's runs in its window's buffer
        uint32_t uSize;     // bytes of its runs
    };
    const unsigned TickIndexShift = 12;     // TrackScan::vTickIndex: every 4096th group

    struct Checkpoint
    {
        uint64_t llPos;     // offset in the track of the delta of the event that opens the group
        int64_t llTick;     // tick before that delta
        uint64_t llGroup;
        uint8_t cRunning;
    };

    // The pairing pass's rules (VisualStoreBuilder::On and Off in VisualStore.cpp) over one track, in
    // its file order, which is its order in the stream: up to 10 slots per channel and key, LIFO; past
    // 10, the latest unpaired NoteOn of the key in the track, on any channel. Pairing is per track, so
    // this is exact. Kept: the NoteOns no off pairs, and the end of each note held llLongTicks or more
    // and paired once (24 bytes each until the merge, which keeps the longest of them; a cap per
    // track drops too many in a track of sustained notes). The pairing pass makes those final at their
    // NoteOn: otherwise a note is final only at its off (or, unpaired, at its track's end), and every
    // later note of its key waits for it (Song::vKnownId).
    class PairSim
    {
    public:
        explicit PairSim( int64_t llLongTicks ) : m_llLongTicks( llLongTicks ) {}
        void On( unsigned ch, unsigned key, int64_t llTick )
        {
            Key *pk = m_a[key].get();
            if ( !pk ) { m_a[key].reset( new Key() ); pk = m_a[key].get(); }
            Key &k = *pk;
            uint32_t id = k.n++;
            if ( !( id & 63 ) ) { k.vPaired.push_back( 0 ); k.vTwice.push_back( 0 ); }
            if ( k.aCount[ch] < 10 ) { k.aSlots[ch][k.aCount[ch]] = id; k.aSlotTick[ch][k.aCount[ch]] = llTick; }
            else if ( !k.bStacked )
            {
                // The first overflow: from here on the stack holds every NoteOn, from the unpaired slots'
                // (as the pairing pass keeps it: only an overflow reads the stack)
                struct At { uint32_t id; int64_t llTick; } a[160];
                unsigned n = 0;
                for ( unsigned c = 0; c < 16; c++ )
                    for ( uint32_t j = 0; j < std::min< uint32_t >( k.aCount[c], 10 ); j++ )
                        if ( !Paired( k, k.aSlots[c][j] ) ) a[n++] = { k.aSlots[c][j], k.aSlotTick[c][j] };
                std::sort( a, a + n, []( const At &x, const At &y ) { return x.id < y.id; } );
                for ( unsigned j = 0; j < n; j++ ) Push( k, a[j].id, a[j].llTick );
                k.bStacked = true;
            }
            k.aCount[ch]++;
            if ( !k.bStacked ) return;
            while ( !k.vStack.empty() && Paired( k, Back( k ) ) ) PopBack( k );
            Push( k, id, llTick );
        }
        void Off( unsigned ch, unsigned key, int64_t llTick )
        {
            if ( !m_a[key] ) return;
            Key &k = *m_a[key];
            if ( !k.aCount[ch] ) return;
            if ( k.aCount[ch] <= 10 ) Pair( k, k.aSlots[ch][k.aCount[ch] - 1], k.aSlotTick[ch][k.aCount[ch] - 1], llTick );
            else
            {
                while ( !k.vStack.empty() && Paired( k, Back( k ) ) ) PopBack( k );
                if ( !k.vStack.empty() ) Pair( k, Back( k ), k.vStack.back().llTick, llTick );
            }
            k.aCount[ch]--;
        }
        // Appends the track's known notes in id order: id = track << 48 | key << 40 | the note's
        // ordinal among the track's NoteOns of the key; end tick and duration -1: unpaired
        void Finish( uint64_t llTrack, std::vector< uint64_t > &vId, std::vector< int64_t > &vEnd, std::vector< int64_t > &vDur )
        {
            for ( unsigned key = 0; key < 128; key++ )
            {
                if ( !m_a[key] ) continue;
                Key &k = *m_a[key];
                std::sort( k.vLong.begin(), k.vLong.end(), []( const Long &a, const Long &b ) { return a.uId < b.uId; } );
                uint64_t llBase = ( llTrack << 48 ) | ( (uint64_t)key << 40 );
                size_t l = 0;
                auto LongBefore = [&]( uint64_t id ) {
                    for ( ; l < k.vLong.size() && k.vLong[l].uId < id; l++ )
                        if ( !Bit( k.vTwice, k.vLong[l].uId ) )
                        {
                            vId.push_back( llBase | k.vLong[l].uId );
                            vEnd.push_back( k.vLong[l].llEnd );
                            vDur.push_back( k.vLong[l].llDur );
                        }
                };
                for ( size_t w = 0; w < k.vPaired.size(); w++ )
                {
                    uint64_t v = ~k.vPaired[w];
                    if ( w + 1 == k.vPaired.size() && ( k.n & 63 ) ) v &= ( uint64_t( 1 ) << ( k.n & 63 ) ) - 1;
                    for ( ; v; v &= v - 1 )
                    {
                        uint64_t id = w * 64 + __builtin_ctzll( v );
                        LongBefore( id );
                        vId.push_back( llBase | id );
                        vEnd.push_back( -1 );
                        vDur.push_back( -1 );
                    }
                }
                LongBefore( UINT64_MAX );
                m_a[key].reset();
            }
        }

    private:
        struct Run { uint32_t uFirst, uLen; int64_t llTick; };     // consecutive NoteOns at one tick
        struct Long { uint32_t uId; int64_t llEnd, llDur; };        // a long note's first off
        struct Key
        {
            uint32_t aCount[16] = {};
            uint32_t aSlots[16][10];
            int64_t aSlotTick[16][10];
            uint32_t n = 0;
            bool bStacked = false;                                  // since a channel first overflowed
            std::vector< uint64_t > vPaired, vTwice;                // bits per NoteOn
            std::vector< Run > vStack;
            std::vector< Long > vLong;
        };
        static bool Bit( const std::vector< uint64_t > &v, uint32_t id ) { return ( v[id >> 6] >> ( id & 63 ) ) & 1; }
        static bool Paired( const Key &k, uint32_t id ) { return Bit( k.vPaired, id ); }
        void Pair( Key &k, uint32_t id, int64_t llOnTick, int64_t llTick )
        {
            if ( Paired( k, id ) ) { k.vTwice[id >> 6] |= uint64_t( 1 ) << ( id & 63 ); return; }
            k.vPaired[id >> 6] |= uint64_t( 1 ) << ( id & 63 );
            if ( llTick - llOnTick >= m_llLongTicks ) k.vLong.push_back( { id, llTick, llTick - llOnTick } );
        }
        static uint32_t Back( const Key &k ) { return k.vStack.back().uFirst + k.vStack.back().uLen - 1; }
        static void Push( Key &k, uint32_t id, int64_t llTick )
        {
            if ( !k.vStack.empty() && k.vStack.back().uFirst + k.vStack.back().uLen == id && k.vStack.back().llTick == llTick ) k.vStack.back().uLen++;
            else k.vStack.push_back( { id, 1, llTick } );
        }
        static void PopBack( Key &k ) { if ( !--k.vStack.back().uLen ) k.vStack.pop_back(); }
        int64_t m_llLongTicks;
        std::unique_ptr< Key > m_a[128];
    };

    struct TrackScan
    {
        uint64_t llBegin = 0, llEnd = 0;    // byte range of the track data in the file
        std::vector< GroupRec > vGroups;
        std::vector< int64_t > vTickIndex;      // per 4096 groups: the tick of the group before (INT64_MIN: none)
        std::vector< std::pair< uint64_t, int64_t > > vBigDelta;  // (group, delta) where uA could not hold it
        int64_t llLastTick = 0;                 // the last group's
        std::vector< Checkpoint > vCps;
        void AddGroup( int64_t llTick, uint32_t uSize )
        {
            uint64_t g = vGroups.size();
            if ( !( g & ( ( uint64_t( 1 ) << TickIndexShift ) - 1 ) ) ) vTickIndex.push_back( g ? llLastTick : INT64_MIN );
            int64_t d = llTick - ( g ? llLastTick : 0 );
            if ( d >= (int64_t)UINT32_MAX ) { vBigDelta.push_back( { g, d } ); vGroups.push_back( { UINT32_MAX, uSize } ); }
            else vGroups.push_back( { (uint32_t)d, uSize } );
            llLastTick = llTick;
        }
        RefTrackInfo info;
        std::vector< RefMetaEvent > vMeta;
        uint32_t aKeyCounts[128] = {};
        uint64_t llChannelEvents = 0;
        uint64_t llOutOfRange = 0;          // channel events dropped for a key, controller or program past 127
        int64_t llLongTicks = 0;            // PairSim's threshold
        std::vector< uint64_t > vKnownId;   // PairSim's, as Song::vKnownId and vKnownEnd (ticks here)
        std::vector< int64_t > vKnownEnd, vKnownDur;
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

    // A track's group ticks in order (TrackScan::AddGroup), from a group on
    struct TickCursor
    {
        const TrackScan *p;
        uint64_t g = 0;         // the next group
        int64_t llPrev = 0;     // the tick of the group before it (0 before the first: deltas start there)
        size_t iBig = 0;
        explicit TickCursor( const TrackScan &s ) : p( &s ) {}
        bool Done() const { return g >= p->vGroups.size(); }
        int64_t Peek() const { uint32_t a = p->vGroups[g].uA; return llPrev + ( a != UINT32_MAX ? (int64_t)a : p->vBigDelta[iBig].second ); }
        int64_t Next()
        {
            uint32_t a = p->vGroups[g++].uA;
            llPrev += a != UINT32_MAX ? (int64_t)a : p->vBigDelta[iBig++].second;
            return llPrev;
        }
        // Before the first group at tick t or later
        static TickCursor At( const TrackScan &s, int64_t t )
        {
            TickCursor c( s );
            const std::vector< int64_t > &vi = s.vTickIndex;
            size_t k = (size_t)( std::lower_bound( vi.begin(), vi.end(), t ) - vi.begin() );
            if ( k > 1 )
            {
                c.g = uint64_t( k - 1 ) << TickIndexShift;
                c.llPrev = vi[k - 1];
                c.iBig = (size_t)( std::lower_bound( s.vBigDelta.begin(), s.vBigDelta.end(), std::make_pair( c.g, INT64_MIN ) ) - s.vBigDelta.begin() );
            }
            while ( !c.Done() && c.Peek() < t ) c.Next();
            return c;
        }
    };

    // Stream: a track scanned in windows as it is read from an archive (Archive::Reader), as Fused; what
    // carries from one window to the next
    struct StreamState
    {
        explicit StreamState( int64_t llLongTicks ) : sim( llLongTicks ) {}
        bool bStarted = false, bDone = false;   // bDone: the track ended (end of track, or malformed)
        size_t iUsed = 0;                       // bytes of the window taken (whole events)
        long long llTick = 0, llGroupTick = LLONG_MIN;
        int iRunning = 0;
        int64_t llGroup = -1;
        uint32_t uRunKey = UINT32_MAX, uSize = 0, uEvents = 0;
        PairSim sim;
        std::unique_ptr< uint8_t[] > pStage;
        size_t iDst = 0, iStageCap = 0;
        ptrdiff_t iLastKey = -1, iRunHeader = -1;
        uint64_t llChunkFirst = 0;
    };

    // One track, SMF semantics identical to RefMidi::ParseTrack (DESIGN.md 1.5).
    // Count: fills pScan (groups with their encoded sizes, checkpoints, info, meta).
    // Emit:  re-reads from a checkpoint and writes groups [llFirstWrite, llStopGroup) to the payload
    //        (the groups' runs at their offsets in it, GroupRec::uA); the groups between the checkpoint
    //        and llFirstWrite go to pDiscard (room for the largest group).
    // Fused: Count, and also encodes the groups' runs into pScan->vPacked (DESIGN.md 4.2a).
    // Stream: Fused over [pTrack, pEnd), a window of the track (bLast: the rest of it), stopping before an
    //         event the window cuts (*pState: where it stopped, and what carries over)
    // A window ends inside an event when a read runs past it (MORE): back to the event's start
#define MORE( c ) if ( MODE == Stream && !bLast && ( c ) ) { p = pEventStart; llTick = llTickBefore; iRunning = iRunningBefore; bMore = true; break; }
    template < int MODE >
    void ScanTrack( const uint8_t *pTrack, const uint8_t *pEnd, const Checkpoint &start, uint64_t llStopGroup, int iTrack,
                    TrackScan *pScan, const GroupRec *pGroups, uint8_t *pPayload, uint64_t llFirstWrite = 0, uint8_t *pDiscard = nullptr,
                    StreamState *pState = nullptr, bool bLast = true )
    {
        constexpr bool COUNT = MODE != Emit, WRITE = MODE != Count, FUSED = MODE == Fused || MODE == Stream;
        const uint8_t *p = pTrack + start.llPos;
        long long llTick = start.llTick;
        int iRunning = start.cRunning;

        int64_t llGroup = (int64_t)start.llGroup - 1;
        long long llGroupTick = LLONG_MIN;
        uint32_t uRunKey = UINT32_MAX;
        // count state
        uint32_t uSize = 0, uEvents = 0;
        PairSim simLocal( COUNT && MODE != Stream ? pScan->llLongTicks : 0 );
        PairSim &sim = MODE == Stream ? pState->sim : simLocal;
        // emit state
        uint8_t *pDst = nullptr, *pLastKey = nullptr, *pRunHeader = nullptr;
        // fused state: the runs of the groups since the last chunk, in a buffer that grows for huge groups
        std::unique_ptr< uint8_t[] > pStage;
        uint8_t *pStageEnd = nullptr;
        uint64_t llChunkFirst = 0;
        bool bMore = false;     // Stream: the window ended inside the track
        if ( MODE == Stream && pState->bStarted )
        {
            StreamState &st = *pState;
            llTick = st.llTick; iRunning = st.iRunning; llGroup = st.llGroup; llGroupTick = st.llGroupTick;
            uRunKey = st.uRunKey; uSize = st.uSize; uEvents = st.uEvents; llChunkFirst = st.llChunkFirst;
            pStage = std::move( st.pStage );
            pDst = pStage.get() + st.iDst;
            pStageEnd = pStage.get() + st.iStageCap;
            pLastKey = st.iLastKey >= 0 ? pStage.get() + st.iLastKey : nullptr;
            pRunHeader = st.iRunHeader >= 0 ? pStage.get() + st.iRunHeader : nullptr;
        }
        else if ( FUSED )
        {
            pStage.reset( new uint8_t[FusedChunk * 2] );
            pDst = pStage.get();
            pStageEnd = pDst + FusedChunk * 2;
        }
        auto Reserve = [&]() {  // room for one event (at most 3 bytes)
            if ( !FUSED || pStageEnd - pDst >= 3 ) return;
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
            if ( !FUSED ) return;
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
        // The track info, in locals while the track is read (stores through pDst could alias pScan's)
        uint64_t llEventCount = 0, llChannelEvents = 0, llOutOfRange = 0, llNotes = 0, llVolumeSum = 0;
        long long llTotalTicks = 0;
        int iMinNote = 0, iMaxNote = 0, iMaxVolume = 0;
        uint32_t aKeyCounts[128] = {};
        auto AddInfo = [&]( long long llAt ) {
            if ( COUNT ) { llEventCount++; llTotalTicks = std::max( llTotalTicks, llAt ); }
        };
        if ( COUNT )
        {
            const RefTrackInfo &in = pScan->info;
            llEventCount = in.llEventCount; llTotalTicks = in.llTotalTicks; llNotes = in.llNoteCount; llVolumeSum = in.llVolumeSum;
            iMinNote = in.iMinNote; iMaxNote = in.iMaxNote; iMaxVolume = in.iMaxVolume;
        }

        bool bEnded = false;    // Stream: the track ended inside the window (end of track, or a bad status)
        while ( p < pEnd )
        {
            const uint8_t *pEventStart = p;
            long long llTickBefore = llTick;
            int iRunningBefore = iRunning;

            int iStatus;
            long long llDelta;
            if ( *p < 0x80 ) llDelta = *p++;    // (nearly every delta is one byte; p < pEnd here)
            else if ( !ReadVarLen( p, pEnd, llDelta ) ) { MORE( p >= pEnd ) if ( COUNT ) pScan->info.bMalformed = true; break; }
            llTick += llDelta;
            if ( p >= pEnd ) { MORE( true ) if ( COUNT ) pScan->info.bMalformed = true; break; }
            if ( *p >= 0x80 ) iStatus = *p++;
            else if ( iRunning ) iStatus = iRunning;
            else { if ( COUNT ) pScan->info.bMalformed = true; break; }

            if ( iStatus < 0xF0 )
            {
                iRunning = iStatus;
                unsigned iType = iStatus >> 4, iChannel = iStatus & 0xF;
                int iDataBytes = ( iType == 0xC || iType == 0xD ) ? 1 : 2;
                if ( pEnd - p < iDataBytes ) { MORE( true ) if ( COUNT ) pScan->info.bMalformed = true; break; }
                // Data bytes are read as data, as the original reads them (DESIGN.md 1.5). A key, controller
                // or program past 127 drops the event (its tick still counts for the length); a second
                // byte past 127 (velocity, value) is clamped
                uint8_t aData[2] = { p[0], iDataBytes == 2 ? std::min< uint8_t >( p[1], 0x7F ) : (uint8_t)0 };
                p += iDataBytes;
                if ( aData[0] >= 0x80 )
                {
                    if ( COUNT ) { llOutOfRange++; llTotalTicks = std::max( llTotalTicks, llTick ); }
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
                        if ( llGroup >= 0 ) pScan->AddGroup( llGroupTick, uSize );
                        if ( MODE == Count && llNext > 0 && llNext % CheckpointEvery == 0 )
                            pScan->vCps.push_back( { (uint64_t)( pEventStart - pTrack ), llTickBefore, llNext, (uint8_t)iRunningBefore } );
                        uSize = uEvents = 0;
                    }
                    if ( MODE == Emit ) pDst = llNext >= llFirstWrite ? pPayload + pGroups[llNext].uA : pDiscard;
                    else if ( FUSED && pDst - pStage.get() >= (ptrdiff_t)FusedChunk ) Flush( llNext );
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
                    if ( iType == 0x9 && aData[1] > 0 ) sim.On( iChannel, aData[0], llTick );
                    else if ( iType == 0x8 || iType == 0x9 ) sim.Off( iChannel, aData[0], llTick );
                    uEvents++;
                    llChannelEvents++;
                    AddInfo( llTick );
                    RefTrackInfo &info = pScan->info;
                    // MIDITrackInfo::AddEventInfo
                    if ( iType == 0x9 && aData[1] > 0 )
                    {
                        if ( !llNotes )
                        {
                            iMinNote = iMaxNote = aData[0];
                            iMaxVolume = aData[1];
                        }
                        else
                        {
                            iMinNote = std::min< int >( aData[0], iMinNote );
                            iMaxNote = std::max< int >( aData[0], iMaxNote );
                            iMaxVolume = std::max< int >( aData[1], iMaxVolume );
                        }
                        llNotes++;
                        llVolumeSum += aData[1];
                        if ( !info.aNoteCount[iChannel] ) info.iNumChannels++;
                        info.aNoteCount[iChannel]++;
                        aKeyCounts[aData[0]]++;
                    }
                    else if ( iType == 0xC && info.aProgram[iChannel] != aData[0] )
                        info.aProgram[iChannel] = info.aNoteCount[iChannel] > 0 ? 128 : aData[0];
                }
            }
            else if ( iStatus == 0xF0 || iStatus == 0xF7 )
            {
                iRunning = 0;
                long long llLen;
                if ( !ReadVarLen( p, pEnd, llLen ) || llLen > pEnd - p ) { MORE( p >= pEnd || llLen > pEnd - p ) if ( COUNT ) pScan->info.bMalformed = true; break; }
                p += llLen; // SysEx is ignored entirely
                AddInfo( llTick );
            }
            else if ( iStatus == 0xFF )
            {
                iRunning = 0;
                if ( p >= pEnd ) { MORE( true ) if ( COUNT ) pScan->info.bMalformed = true; break; }
                uint8_t cType = *p++;
                long long llLen;
                if ( !ReadVarLen( p, pEnd, llLen ) || llLen > pEnd - p ) { MORE( p >= pEnd || llLen > pEnd - p ) if ( COUNT ) pScan->info.bMalformed = true; break; }
                const uint8_t *pMeta = p;
                p += llLen;
                AddInfo( llTick );
                if ( cType == 0x2F ) { if ( MODE == Stream ) bEnded = true; break; }
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
                if ( MODE == Stream ) bEnded = true;
                break;
            }
        }

        // Stream: the window used up at an event's end (any other stop at its end ended the track)
        if ( MODE == Stream && !bLast && !bMore && !bEnded && p >= pEnd ) bMore = true;
        if ( !bMore ) CloseGroup();
        if ( COUNT )
        {
            RefTrackInfo &out = pScan->info;
            out.llEventCount = llEventCount; out.llTotalTicks = llTotalTicks; out.llNoteCount = llNotes; out.llVolumeSum = llVolumeSum;
            out.iMinNote = iMinNote; out.iMaxNote = iMaxNote; out.iMaxVolume = iMaxVolume;
            pScan->llChannelEvents += llChannelEvents;
            pScan->llOutOfRange += llOutOfRange;
            for ( int k = 0; k < 128; k++ ) pScan->aKeyCounts[k] += aKeyCounts[k];
        }
        if ( MODE == Stream )
        {
            StreamState &st = *pState;
            st.iUsed = (size_t)( p - pTrack );
            if ( bMore )
            {
                st.bStarted = true;
                st.llTick = llTick; st.iRunning = iRunning; st.llGroup = llGroup; st.llGroupTick = llGroupTick;
                st.uRunKey = uRunKey; st.uSize = uSize; st.uEvents = uEvents; st.llChunkFirst = llChunkFirst;
                st.iDst = (size_t)( pDst - pStage.get() );
                st.iStageCap = (size_t)( pStageEnd - pStage.get() );
                st.iLastKey = pLastKey ? pLastKey - pStage.get() : -1;
                st.iRunHeader = pRunHeader ? pRunHeader - pStage.get() : -1;
                st.pStage = std::move( pStage );
                return;
            }
            st.bDone = true;
        }
        if ( COUNT && llGroup >= 0 ) pScan->AddGroup( llGroupTick, uSize );
        if ( COUNT ) sim.Finish( (uint64_t)iTrack, pScan->vKnownId, pScan->vKnownEnd, pScan->vKnownDur );
        Flush( (uint64_t)( llGroup + 1 ) );
    }
#undef MORE

    double Seconds( long long llFrom ) { return ( Timer::NowMicroSecs() - llFrom ) / 1e6; }

    uint64_t PrivateBytes() { return Util::ProcessMemory(); }

    // pReader: a compressed file or archive, read front to back as it is decompressed (pData null,
    // llSize the file's size): a fused count pass over the tracks as they come out
    std::unique_ptr< Song > LoadImpl( const uint8_t *pData, uint64_t llSize, MappedFile *pFile, int iThreads,
                                      LoadProgress &prog, LoadStats &stats, std::wstring &sError, Archive::Reader *pReader = nullptr )
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
        bool bFused = prog.iFusedRead > 0 || pReader;
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
        auto BE = []( const uint8_t *q, int n ) { uint64_t v = 0; for ( int i = 0; i < n; i++ ) v = ( v << 8 ) | q[i]; return v; };
        uint64_t llHdr = 0;
        uint8_t aStreamHdr[6] = {};
        const uint8_t *pHdr = aStreamHdr;      // MThd's data
        if ( pReader )
        {
            char aType[4];
            uint64_t llLen = 0;
            size_t got = 0;
            if ( pReader->NextChunk( aType, llLen ) && memcmp( aType, "MThd", 4 ) == 0 )
                while ( got < 6 && got < llLen )
                {
                    size_t n = pReader->ReadData( aStreamHdr + got, 6 - got );
                    if ( !n ) break;
                    got += n;
                }
            if ( got < 6 ) { sError = !pReader->sError.empty() ? pReader->sError : L"Not a Standard MIDI File (no MThd header)."; return nullptr; }
        }
        else
        {
            WaitBytes( std::min< uint64_t >( 14, llSize ) );
            if ( llSize < 14 || memcmp( pData, "MThd", 4 ) != 0 ) { sError = L"Not a Standard MIDI File (no MThd header)."; return nullptr; }
            llHdr = std::max< uint64_t >( BE( pData + 4, 4 ), 6 );
            pHdr = pData + 8;
        }
        auto song = std::make_unique< Song >();
        RefMidiInfo &info = song->info;
        info.iFormatType = (int)BE( pHdr, 2 );
        info.iDivision = (int)BE( pHdr + 4, 2 );
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
        // A note held this long (4 beats) has its end found by the count pass (PairSim)
        const int64_t llLongTicks = (int64_t)( info.iDivision * ( getenv( "PFAVIZ_LONGBEATS" ) ? atof( getenv( "PFAVIZ_LONGBEATS" ) ) : 4.0 ) );   // TEMP
        std::vector< int64_t > vKnownEndTick;   // Song::vKnownEnd in ticks, until the unique ticks are known
        auto NewScan = [&]( uint64_t llBegin, uint64_t llEnd ) {
            vScans.push_back( std::make_unique< TrackScan >() );
            TrackScan *p = vScans.back().get();
            p->llBegin = llBegin;
            p->llEnd = llEnd;
            p->llLongTicks = llLongTicks;
            return p;
        };
        // Not following a sequential reader, the file is all there: every chunk first, and the largest
        // tracks parsed first, so a big track late in the file does not run on after the others
        std::vector< uint32_t > vOrder;
        std::atomic< size_t > iNextScan{ 0 };
        if ( !bSequential && !pReader )
        {
            for ( uint64_t q = llNextChunk; q + 8 <= llSize; )
            {
                uint64_t llBegin = q + 8, llEnd = std::min< uint64_t >( llBegin + BE( pData + q + 4, 4 ), llSize );
                if ( memcmp( pData + q, "MTrk", 4 ) == 0 ) NewScan( llBegin, llEnd );    // unknown chunks: skipped
                q = llEnd;
            }
            llNextChunk = llSize;
            vOrder.resize( vScans.size() );
            for ( size_t t = 0; t < vOrder.size(); t++ ) vOrder[t] = (uint32_t)t;
            std::stable_sort( vOrder.begin(), vOrder.end(), [&]( uint32_t a, uint32_t b ) {
                return vScans[a]->llEnd - vScans[a]->llBegin > vScans[b]->llEnd - vScans[b]->llBegin; } );
            prog.llTracksFound = vScans.size();
        }
        // An archive: the tracks as they come out of it, one thread reading at a time. A track up to
        // StreamWhole is read whole and counted with the reader free for the next; a longer one is
        // counted in windows as it is read (ScanTrack< Stream >)
        const size_t StreamWhole = 16 << 20;
        std::wstring sStreamError;
        bool bStreamEnd = false;
        uint64_t llStreamPos = 0;
        auto StreamTrack = [&]( std::vector< uint8_t > &vBuf ) {
            std::unique_lock< std::mutex > lock( mtx );
            auto Fail = [&]() {
                bStreamEnd = true;
                if ( sStreamError.empty() ) sStreamError = pReader->sError;
                return false;
            };
            auto Fill = [&]( uint8_t *p, size_t n ) {
                size_t got = 0;
                while ( got < n )
                {
                    size_t r = pReader->ReadData( p + got, n - got );
                    if ( !r ) break;
                    got += r;
                }
                return got;
            };
            auto Progress = [&]() {
                prog.aDone[LoadProgress::Reading] = pReader->Done();
                prog.aDone[LoadProgress::Counting] = pReader->Done();
            };
            TrackScan *pScan = nullptr;
            int iTrack = -1;
            uint64_t llLen = 0;
            while ( !pScan )
            {
                if ( bStreamEnd || Cancelled() ) return false;
                char aType[4];
                if ( !pReader->NextChunk( aType, llLen ) ) return Fail();
                llStreamPos += 8;
                if ( memcmp( aType, "MTrk", 4 ) != 0 ) { llStreamPos += llLen; continue; }  // unknown chunk: skipped
                iTrack = (int)vScans.size();
                pScan = NewScan( llStreamPos, llStreamPos + llLen );
                llStreamPos += llLen;
                prog.llTracksFound = vScans.size();
            }
            if ( llLen <= StreamWhole )
            {
                vBuf.resize( (size_t)llLen );
                size_t got = Fill( vBuf.data(), vBuf.size() );
                if ( !pReader->sError.empty() ) return Fail();
                Progress();
                lock.unlock();
                ScanTrack< Fused >( vBuf.data(), vBuf.data() + got, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, iTrack, pScan, nullptr, nullptr );
            }
            else
            {
                StreamState st( pScan->llLongTicks );
                std::vector< uint8_t > vWin( StreamWhole );
                size_t iHave = 0;
                uint64_t llLeft = llLen;
                for ( ;; )
                {
                    if ( Cancelled() ) return false;
                    size_t iWant = (size_t)std::min< uint64_t >( vWin.size() - iHave, llLeft ), n = Fill( vWin.data() + iHave, iWant );
                    if ( !pReader->sError.empty() ) return Fail();
                    iHave += n;
                    llLeft = n < iWant ? 0 : llLeft - n;   // short: the file ended inside the track
                    ScanTrack< Stream >( vWin.data(), vWin.data() + iHave, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, iTrack, pScan, nullptr, nullptr,
                                         0, nullptr, &st, llLeft == 0 );
                    Progress();
                    if ( st.bDone ) break;
                    memmove( vWin.data(), vWin.data() + st.iUsed, iHave - st.iUsed );
                    iHave -= st.iUsed;
                    if ( iHave == vWin.size() ) vWin.resize( vWin.size() * 2 );   // an event longer than the window
                }
                // What follows the track's end
                while ( llLeft )
                {
                    size_t n = Fill( vWin.data(), (size_t)std::min< uint64_t >( vWin.size(), llLeft ) );
                    if ( !n ) break;
                    llLeft -= n;
                }
                if ( !pReader->sError.empty() ) return Fail();
                Progress();
            }
            prog.llEvents += pScan->llChannelEvents;
            prog.llNotes += pScan->info.llNoteCount;
            prog.llTracksDone++;
            return true;
        };
        if ( pReader )
        {
            prog.aTotal[LoadProgress::Reading] = pReader->Total();
            prog.aTotal[LoadProgress::Counting] = pReader->Total();
        }
        Parallel::For( iThreads, (size_t)iThreads, [&]( size_t, int ) {
            if ( pReader )
            {
                std::vector< uint8_t > vBuf;
                while ( StreamTrack( vBuf ) ) {}
                return;
            }
            for ( ;; )
            {
                TrackScan *pScan = nullptr;
                int iTrack = -1;
                if ( !bSequential )
                {
                    size_t i = iNextScan++;
                    if ( i >= vOrder.size() || Cancelled() ) return;
                    iTrack = (int)vOrder[i];
                    pScan = vScans[iTrack].get();
                }
                else
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
                        pScan = NewScan( llBegin, llEnd );
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
        if ( !sStreamError.empty() ) { sError = sStreamError; return nullptr; }

        // Track info aggregation (MIDIInfo::AddTrackInfo)
        song->vTracks.resize( vScans.size() );
        song->vTrackMeta.resize( vScans.size() );
        // The long notes kept: the longest max( 1 M, notes / 256 ). (Each one whose block is compacted
        // before its off costs a set entry while it waits, so more is not better: ranking by the notes a
        // hold blocks, from a beat, made Night of Nights' pairing peak 0.3 GB higher than this)
        int64_t llKeepDur = 0;
        {
            uint64_t llNotes = 0;
            std::vector< int64_t > vDur;
            for ( auto &sc : vScans )
            {
                for ( int k = 0; k < 128; k++ ) llNotes += sc->aKeyCounts[k];
                for ( int64_t d : sc->vKnownDur ) if ( d >= 0 ) vDur.push_back( d );
            }
            size_t iCap = getenv( "PFAVIZ_KEEPALL" ) ? SIZE_MAX : (size_t)std::max< uint64_t >( 1 << 20, llNotes / 256 );   // TEMP
            if ( vDur.size() > iCap )
            {
                std::nth_element( vDur.begin(), vDur.begin() + ( vDur.size() - iCap ), vDur.end() );
                llKeepDur = vDur[vDur.size() - iCap];
            }
        }
        for ( size_t t = 0; t < vScans.size(); t++ )
        {
            const RefTrackInfo &mti = vScans[t]->info;
            song->vTracks[t] = mti;
            song->vTrackMeta[t] = vScans[t]->vMeta;
            stats.llMalformedTracks += mti.bMalformed;
            stats.llOutOfRange += vScans[t]->llOutOfRange;
            for ( int k = 0; k < 128; k++ ) song->aKeyNoteCounts[k] += vScans[t]->aKeyCounts[k];
            const TrackScan &sc = *vScans[t];
            for ( size_t i = 0; i < sc.vKnownId.size(); i++ )
                if ( sc.vKnownDur[i] < 0 || sc.vKnownDur[i] >= llKeepDur )
                {
                    song->vKnownId.push_back( sc.vKnownId[i] );
                    vKnownEndTick.push_back( sc.vKnownEnd[i] );
                }
            vScans[t]->vKnownId = std::vector< uint64_t >();
            vScans[t]->vKnownEnd = std::vector< int64_t >();
            vScans[t]->vKnownDur = std::vector< int64_t >();
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
        if ( getenv( "PFAVIZ_KEEPALL" ) || getenv( "PFAVIZ_LONGBEATS" ) || getenv( "PFAVIZ_KNOWNDBG" ) ) fprintf( stderr, "known notes kept: %zu\n", song->vKnownId.size() );   // TEMP

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
            if ( !s->vGroups.empty() ) llMaxTick = std::max( llMaxTick, s->llLastTick );
        }
        prog.aTotal[LoadProgress::Merging] = 4;
        EventStream &es = song->stream;
        es.llEvents = llEvents;
        // Transient (global order): every group's track, and each tick's first group
        std::vector< uint64_t > vTickGroupStart;
        std::vector< uint32_t > vGroupTrack;
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
                for ( TickCursor c( *vScans[t] ); !c.Done(); )
                {
                    int64_t llTick = c.Next();
                    __atomic_fetch_or( &vBits[(uint64_t)llTick >> 6], 1ull << ( llTick & 63 ), __ATOMIC_RELAXED );
                }
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
            for ( auto &s : vScans ) for ( TickCursor c( *s ); !c.Done(); ) vUnique.push_back( c.Next() );
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
        // The known ends as unique ticks (an off's tick is one of its track's group ticks)
        song->vKnownEnd.resize( vKnownEndTick.size() );
        Parallel::For( iThreads, ( vKnownEndTick.size() + 65535 ) >> 16, [&]( size_t c, int ) {
            for ( size_t i = c << 16; i < std::min( vKnownEndTick.size(), ( c + 1 ) << 16 ); i++ )
                song->vKnownEnd[i] = vKnownEndTick[i] < 0 ? UINT64_MAX : Rank( vKnownEndTick[i] );
        } );
        vKnownEndTick = std::vector< int64_t >();
        prog.aDone[LoadProgress::Merging] = 1;
        llStep = llPhase;
        Step( bBitmap ? L"unique ticks (bitmap)" : L"unique ticks (sort)" );

        // Groups per unique tick, then each group's track in global (tick, track) order
        std::vector< uint32_t > vCount( U, 0 );
        Parallel::For( iThreads, vScans.size(), [&]( size_t t, int ) {
            uint64_t u = 0;
            for ( TickCursor c( *vScans[t] ); !c.Done(); )
            {
                u = RankFrom( c.Next(), u );
                __atomic_fetch_add( &vCount[u], 1u, __ATOMIC_RELAXED );
            }
        } );
        vTickGroupStart.resize( U + 1 );
        uint64_t acc = 0;
        for ( uint64_t u = 0; u < U; u++ ) { vTickGroupStart[u] = acc; acc += vCount[u]; }
        vTickGroupStart[U] = acc;
        std::fill( vCount.begin(), vCount.end(), 0 );
        vGroupTrack.resize( G );
        song->vTrackLastU.assign( vScans.size(), UINT64_MAX );
        for ( size_t t = 0; t < vScans.size(); t++ )
            if ( !vScans[t]->vGroups.empty() ) song->vTrackLastU[t] = Rank( vScans[t]->llLastTick );
        prog.aDone[LoadProgress::Merging] = 2;

        // Tick ranges are owned by one thread each, and tracks are visited in order, so the groups of a
        // tick land in ascending track order with no atomics. Each tick's run bytes are added up on the
        // way (the headers then need nothing from the tracks).
        size_t iRanges = std::max< size_t >( 1, std::min< uint64_t >( U, (uint64_t)iThreads * 8 ) );
        std::vector< uint32_t > vTickRaw( U, 0 );
        std::atomic< bool > bTickTooBig{ false };
        Parallel::For( iThreads, iRanges, [&]( size_t r, int ) {
            uint64_t u0 = U * r / iRanges, u1 = U * ( r + 1 ) / iRanges;
            if ( u0 >= u1 ) return;
            int64_t t0 = vUnique[u0], t1 = u1 < U ? vUnique[u1] : INT64_MAX;
            for ( size_t t = 0; t < vScans.size(); t++ )
            {
                TickCursor c = TickCursor::At( *vScans[t], t0 );
                uint64_t u = u0;
                while ( !c.Done() && c.Peek() < t1 )
                {
                    u = RankFrom( c.Next(), u );
                    vGroupTrack[vTickGroupStart[u] + vCount[u]++] = (uint32_t)t;
                    if ( __builtin_add_overflow( vTickRaw[u], vScans[t]->vGroups[c.g - 1].uSize, &vTickRaw[u] ) ) bTickTooBig = true;
                }
            }
        } );
        vCount.clear(); vCount.shrink_to_fit();
        if ( bTickTooBig ) { sError = L"A tick holds too many events."; return nullptr; }
        vBits.clear(); vBits.shrink_to_fit();
        vWordRank.clear(); vWordRank.shrink_to_fit();
        prog.aDone[LoadProgress::Merging] = 3;
        Step( L"group order" );

        // Group headers in global order, as the stream will hold them (about a byte a group; the order
        // itself is then dropped: a header gives its group's track), raw offsets with a seek point for
        // the first tick that starts in each chunk, and slices cut at the first tick that starts in each
        // SliceBytes of the stream, which the build fills in parallel. Per tick range (as above): its
        // header and raw bytes, then from their sums its headers, seek points and slices.
        std::vector< uint8_t > vHdr;
        struct Slice { uint64_t llHdr, llRaw; };
        std::vector< Slice > vSlices;
        {
            uint64_t llEst = G;
            for ( uint32_t n : vTickRaw ) llEst += n;
            const uint64_t SliceBytes = std::max< uint64_t >( 4 << 20, llEst / 512 );
            struct Range { uint64_t llHdr = 0, llRaw = 0, llLastStart = 0; std::vector< EventStream::SeekPoint > vSeek; std::vector< Slice > vSlices; };
            std::vector< Range > vRanges( iRanges );
            // Calls fn( u, header ) for the range's groups in order, after onTick( u ) at each tick's start
            auto Walk = [&]( size_t r, auto &&onTick, auto &&fn ) {
                uint64_t u0 = U * r / iRanges, u1 = U * ( r + 1 ) / iRanges;
                for ( uint64_t u = u0, g = vTickGroupStart[u0]; u < u1; u++ )
                {
                    onTick( u );
                    for ( uint32_t uPrev = 0; g < vTickGroupStart[u + 1]; g++ )
                    {
                        uint32_t t = vGroupTrack[g];
                        uint64_t llGap = g == vTickGroupStart[u] ? t : t - uPrev - 1;
                        fn( ( llGap << 1 ) | ( g + 1 == vTickGroupStart[u + 1] ? 1 : 0 ) );
                        uPrev = t;
                    }
                }
            };
            Parallel::For( iThreads, iRanges, [&]( size_t r, int ) {
                Range &rg = vRanges[r];
                Walk( r, [&]( uint64_t u ) { rg.llLastStart = rg.llRaw; rg.llRaw += vTickRaw[u]; },
                      [&]( uint64_t llHeader ) {
                          unsigned n = EventStream::VarSize( llHeader );
                          rg.llHdr += n;
                          rg.llRaw += n;
                      } );
            } );
            std::vector< uint64_t > vHdrAt( iRanges + 1, 0 ), vRawAt( iRanges + 1, 0 );
            for ( size_t r = 0; r < iRanges; r++ ) { vHdrAt[r + 1] = vHdrAt[r] + vRanges[r].llHdr; vRawAt[r + 1] = vRawAt[r] + vRanges[r].llRaw; }
            vHdr.resize( (size_t)vHdrAt[iRanges] );
            Parallel::For( iThreads, iRanges, [&]( size_t r, int ) {
                Range &rg = vRanges[r];
                uint8_t *pHdr = vHdr.data() + vHdrAt[r];
                // The start of the tick before (the range before's last; none before the first); the runs
                // of the tick being walked, added at the next tick's start
                uint64_t llOff = vRawAt[r], llPrevStart = r ? vRawAt[r - 1] + vRanges[r - 1].llLastStart : UINT64_MAX, llTickRaw = 0;
                Walk( r,
                      [&]( uint64_t u ) {
                          llOff += llTickRaw;
                          llTickRaw = vTickRaw[u];
                          bool bFirst = llPrevStart == UINT64_MAX;
                          if ( bFirst || ( llPrevStart / SliceBytes ) != ( llOff / SliceBytes ) ) rg.vSlices.push_back( { (uint64_t)( pHdr - vHdr.data() ), llOff } );
                          if ( bFirst || ( llPrevStart >> EventStream::ChunkShift ) != ( llOff >> EventStream::ChunkShift ) ) rg.vSeek.push_back( { u, llOff } );
                          llPrevStart = llOff;
                      },
                      [&]( uint64_t llHeader ) {
                          uint8_t *q = EventStream::WriteVar( pHdr, llHeader );
                          llOff += (uint64_t)( q - pHdr );
                          pHdr = q;
                      } );
            } );
            for ( Range &rg : vRanges )
            {
                es.vSeek.insert( es.vSeek.end(), rg.vSeek.begin(), rg.vSeek.end() );
                vSlices.insert( vSlices.end(), rg.vSlices.begin(), rg.vSlices.end() );
            }
            vSlices.push_back( { vHdr.size(), vRawAt[iRanges] } );
            es.llRawBytes = vRawAt[iRanges];
        }
        vTickRaw.clear(); vTickRaw.shrink_to_fit();
        vGroupTrack.clear(); vGroupTrack.shrink_to_fit();
        vTickGroupStart.clear(); vTickGroupStart.shrink_to_fit();
        Step( L"group headers" );

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

        // Phase 4: the payload, window by window. A window is a run of slices; per window, the
        // slices count their groups per track (from the headers), which gives each slice its first
        // group of every track; each slice then writes its headers and gives its groups their raw
        // offsets (the track records' llTick from here on); the runs are copied in (fused: out of the
        // tracks' packed chunks; two-pass: the emit pass re-reads each track from its checkpoint at or
        // before the window, at most CheckpointEvery groups more); and the window's complete chunks
        // are LZ4-compressed. Windows of about WindowBytes, so the raw stream is never whole in memory.
        llPhase = llStep = Timer::NowMicroSecs();
        prog.iStage = LoadProgress::Building;
        const size_t T = vScans.size();
        const size_t iSlices = vSlices.size() - 1;
        const uint64_t WindowBytes = 256ull << 20;
        std::vector< size_t > vWinCuts( 1, 0 );   // first slice of each window
        for ( size_t s = 0; s < iSlices; s++ )
            if ( s > vWinCuts.back() && vSlices[s].llRaw - vSlices[vWinCuts.back()].llRaw >= WindowBytes ) vWinCuts.push_back( s );
        vWinCuts.push_back( iSlices );
        prog.aTotal[LoadProgress::Building] = vWinCuts.size() - 1;

        size_t iChunks = (size_t)( ( es.llRawBytes + EventStream::ChunkSize - 1 ) >> EventStream::ChunkShift );
        es.vChunks.resize( iChunks );
        // The stored chunks go straight to their place in the stream: one reservation (stored chunks are
        // at most raw), committed as each window appends its chunks
        if ( !es.Reserve() ) { sError = L"Out of memory for the event stream."; return nullptr; }
        uint64_t llData = 0;
        std::vector< std::unique_ptr< uint8_t[] > > vPacked( iChunks );
        std::vector< uint64_t > vCur( T, 0 );          // each track's next group
        std::vector< uint32_t > vCounts;                // per slice of the window, per track
        std::unique_ptr< uint8_t[] > pRaw, pPrev;
        uint64_t llPrevBase = 0, llRawCap = 0, llPrevCap = 0;
        size_t iNextChunk = 0;
        std::vector< std::vector< uint8_t > > vUnpack( bFused ? iThreads : 0 );
        // Two-pass: a scratch group per thread for the groups re-read before a window's first
        std::vector< std::unique_ptr< uint8_t[] > > vDiscard( bFused ? 0 : iThreads );
        if ( !bFused )
        {
            uint32_t uMax = 0;
            for ( auto &sc : vScans ) for ( const GroupRec &g : sc->vGroups ) uMax = std::max( uMax, g.uSize );
            for ( auto &p : vDiscard ) p.reset( new uint8_t[uMax + 16] );
        }
        struct Item { uint32_t uTrack, uChunk; uint64_t llLo, llHi; };
        std::vector< Item > vItems;
        double dCopy = 0, dPack = 0;
        for ( size_t w = 0; w + 1 < vWinCuts.size(); w++ )
        {
            if ( Cancelled() ) break;
            size_t s0 = vWinCuts[w], s1 = vWinCuts[w + 1], nS = s1 - s0;
            uint64_t llWin0 = vSlices[s0].llRaw, llWin1 = vSlices[s1].llRaw;
            // The buffer starts at the window's first chunk: the tail of the last window's partial chunk
            // comes along
            uint64_t llBase = ( llWin0 >> EventStream::ChunkShift ) << EventStream::ChunkShift;
            if ( llWin1 - llBase >= UINT32_MAX ) { sError = L"A tick holds too many events."; return nullptr; }   // GroupRec::uA
            // The last window's buffer is this one's when it is large enough (its pages are in already: a
            // fresh one faults in every page), its tail moved to the front
            uint64_t llNeed = llWin1 - llBase + 16;
            if ( pPrev && llPrevCap >= llNeed )
            {
                if ( llBase < llWin0 ) memmove( pPrev.get(), pPrev.get() + ( llBase - llPrevBase ), (size_t)( llWin0 - llBase ) );
                pRaw = std::move( pPrev );
                llRawCap = llPrevCap;
            }
            else
            {
                llRawCap = llNeed + llNeed / 8;     // (room for a somewhat larger window after)
                pRaw.reset( new ( std::nothrow ) uint8_t[llRawCap] );
                if ( !pRaw ) { sError = L"Out of memory for the event stream."; return nullptr; }
                if ( llBase < llWin0 ) memcpy( pRaw.get(), pPrev.get() + ( llBase - llPrevBase ), (size_t)( llWin0 - llBase ) );
                pPrev.reset();
            }
            long long llT0 = Timer::NowMicroSecs();

            // Groups per track per slice, then each slice's first group of each track
            vCounts.assign( nS * T, 0 );
            Parallel::For( iThreads, nS, [&]( size_t i, int ) {
                uint32_t *pCount = &vCounts[i * T];
                const uint8_t *p = vHdr.data() + vSlices[s0 + i].llHdr, *pEnd = vHdr.data() + vSlices[s0 + i + 1].llHdr;
                uint32_t uTrack = 0;
                bool bFirst = true;
                while ( p < pEnd )
                {
                    uint64_t h = EventStream::ReadVar( p );
                    uTrack = bFirst ? (uint32_t)( h >> 1 ) : uTrack + 1 + (uint32_t)( h >> 1 );
                    bFirst = h & 1;
                    pCount[uTrack]++;
                }
            } );
            std::vector< uint64_t > vCurEnd( vCur );
            Parallel::For( iThreads, ( T + 255 ) / 256, [&]( size_t c, int ) {
                for ( size_t t = c * 256; t < std::min( T, ( c + 1 ) * 256 ); t++ )
                {
                    uint64_t n = vCur[t];
                    for ( size_t i = 0; i < nS; i++ ) { uint32_t k = vCounts[i * T + t]; vCounts[i * T + t] = (uint32_t)( n - vCur[t] ); n += k; }
                    vCurEnd[t] = n;
                }
            } );
            // Headers, and each group's raw offset of its runs
            Parallel::For( iThreads, nS, [&]( size_t i, int ) {
                uint32_t *pRel = &vCounts[i * T];
                const uint8_t *p = vHdr.data() + vSlices[s0 + i].llHdr, *pEnd = vHdr.data() + vSlices[s0 + i + 1].llHdr;
                uint64_t llOff = vSlices[s0 + i].llRaw;
                uint32_t uTrack = 0;
                bool bFirst = true;
                while ( p < pEnd )
                {
                    const uint8_t *q = p;
                    uint64_t h = EventStream::ReadVar( p );
                    uTrack = bFirst ? (uint32_t)( h >> 1 ) : uTrack + 1 + (uint32_t)( h >> 1 );
                    bFirst = h & 1;
                    memcpy( pRaw.get() + ( llOff - llBase ), q, (size_t)( p - q ) );
                    llOff += (uint64_t)( p - q );
                    GroupRec &g = vScans[uTrack]->vGroups[vCur[uTrack] + pRel[uTrack]++];
                    uint32_t uSize = g.uSize;
                    g.uA = (uint32_t)( llOff - llBase );
                    llOff += uSize;
                }
            } );

            // The runs
            if ( bFused )
            {
                vItems.clear();
                for ( size_t t = 0; t < T; t++ )
                {
                    if ( vCurEnd[t] == vCur[t] ) continue;
                    const std::vector< TrackScan::PackedChunk > &vc = vScans[t]->vPacked;
                    size_t k = (size_t)( std::upper_bound( vc.begin(), vc.end(), vCur[t], []( uint64_t v, const TrackScan::PackedChunk &c ) { return v < c.llFirstGroup; } ) - vc.begin() ) - 1;
                    for ( ; k < vc.size() && vc[k].llFirstGroup < vCurEnd[t]; k++ ) vItems.push_back( { (uint32_t)t, (uint32_t)k, vCur[t], vCurEnd[t] } );
                }
                Parallel::For( iThreads, vItems.size(), [&]( size_t i, int iThread ) {
                    const Item &it = vItems[i];
                    TrackScan &s = *vScans[it.uTrack];
                    TrackScan::PackedChunk &c = s.vPacked[it.uChunk];
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
                    uint64_t g = c.llFirstGroup;
                    for ( ; p < pEnd && g < it.llHi; g++ )
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
                        if ( g >= it.llLo ) memcpy( pRaw.get() + s.vGroups[g].uA, p, (size_t)( q - p ) );
                        p = q;
                    }
                    if ( p >= pEnd ) c.data.reset();     // every group of the chunk is in
                } );
            }
            else
            {
                // Each track's groups of the window, from the checkpoint at or before the first, in
                // segments between checkpoints
                struct Seg { uint32_t uTrack; uint32_t uCp; uint64_t llFirst, llStop; };
                std::vector< Seg > vSegs;
                for ( size_t t = 0; t < T; t++ )
                {
                    if ( vCurEnd[t] == vCur[t] ) continue;
                    const std::vector< Checkpoint > &vc = vScans[t]->vCps;
                    // checkpoint k (1-based; 0: the track's start) begins at group vc[k - 1].llGroup
                    size_t k = (size_t)( std::upper_bound( vc.begin(), vc.end(), vCur[t], []( uint64_t v, const Checkpoint &c ) { return v < c.llGroup; } ) - vc.begin() );
                    for ( ; ; k++ )
                    {
                        uint64_t llFrom = k == 0 ? 0 : vc[k - 1].llGroup, llTo = k < vc.size() ? vc[k].llGroup : UINT64_MAX;
                        vSegs.push_back( { (uint32_t)t, (uint32_t)k, std::max( llFrom, vCur[t] ), std::min( llTo, vCurEnd[t] ) } );
                        if ( llTo >= vCurEnd[t] ) break;
                    }
                }
                Parallel::For( iThreads, vSegs.size(), [&]( size_t i, int iThread ) {
                    if ( Cancelled() ) return;
                    const Seg &sg = vSegs[i];
                    TrackScan &s = *vScans[sg.uTrack];
                    Checkpoint start = sg.uCp == 0 ? Checkpoint{ 0, 0, 0, 0 } : s.vCps[sg.uCp - 1];
                    ScanTrack< Emit >( pData + s.llBegin, pData + s.llEnd, start, sg.llStop, (int)sg.uTrack, nullptr, s.vGroups.data(), pRaw.get(), sg.llFirst,
                                       vDiscard[iThread].get() );
                } );
            }
            vCur.swap( vCurEnd );
            long long llT1 = Timer::NowMicroSecs();
            dCopy += ( llT1 - llT0 ) / 1e6;

            // LZ4 per 64 KiB chunk (stored as is when that is not smaller): the chunks that end in the
            // window (the last window: all the rest), then appended to the stream in order
            size_t iEnd = s1 == iSlices ? iChunks : (size_t)( llWin1 >> EventStream::ChunkShift );
            Parallel::For( iThreads, iEnd - iNextChunk, [&]( size_t i, int ) {
                size_t c = iNextChunk + i;
                const uint8_t *pSrc = pRaw.get() + ( ( (uint64_t)c << EventStream::ChunkShift ) - llBase );
                size_t iRaw = es.ChunkRaw( c );
                std::unique_ptr< uint8_t[] > pTmp( new uint8_t[Lz4::Bound( iRaw )] );
                size_t iSize = Lz4::Compress( pSrc, iRaw, pTmp.get() );
                if ( iSize >= iRaw ) { iSize = iRaw; memcpy( pTmp.get(), pSrc, iRaw ); }
                vPacked[c].reset( new uint8_t[iSize] );
                memcpy( vPacked[c].get(), pTmp.get(), iSize );
                es.vChunks[c].uSize = (uint32_t)iSize;
            } );
            for ( size_t c = iNextChunk; c < iEnd; c++ ) { es.vChunks[c].llOffset = llData; llData += es.vChunks[c].uSize; }
            if ( !es.Commit( llData ) ) { sError = L"Out of memory for the event stream."; return nullptr; }
            Parallel::For( iThreads, iEnd - iNextChunk, [&]( size_t i, int ) {
                size_t c = iNextChunk + i;
                memcpy( es.data.get() + es.vChunks[c].llOffset, vPacked[c].get(), es.vChunks[c].uSize );
                vPacked[c].reset();
            } );
            iNextChunk = iEnd;
            dPack += ( Timer::NowMicroSecs() - llT1 ) / 1e6;
            pPrev = std::move( pRaw );
            llPrevBase = llBase;
            llPrevCap = llRawCap;
            prog.aDone[LoadProgress::Building] = w + 1;
        }
        pPrev.reset();
        vUnpack.clear();
        vDiscard.clear();
        vHdr.clear(); vHdr.shrink_to_fit();
        // The per-track scans (group records, checkpoints, packed tracks) are not needed after the
        // build; freed here rather than after pairing, where they would add to its peak
        vScans.clear();
        vScans.shrink_to_fit();
        if ( pFile ) pFile->Close(); // the mapping is not needed after the build (4.0)
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }
        stats.vSteps.push_back( { prog.iStage.load(), bFused ? L"copy from packed tracks" : L"parse into the stream", dCopy, PrivateBytes() } );
        stats.vSteps.push_back( { prog.iStage.load(), L"LZ4 chunks", dPack, PrivateBytes() } );
        llStep = Timer::NowMicroSecs();
        es.llDataBytes = llData;
        vPacked = std::vector< std::unique_ptr< uint8_t[] > >();
        stats.adStageSeconds[LoadProgress::Building] = Seconds( llPhase );
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }

        // Pairing and the visual store
        llPhase = Timer::NowMicroSecs();
        prog.aTotal[LoadProgress::Pairing] = es.UniqueTicks();
        prog.iStage = LoadProgress::Pairing;
        song->bFirstPairingOnly = prog.bFirstPairingOnly;
        if ( !prog.bSkipPairing && !BuildVisualStore( *song, iThreads, &prog.aDone[LoadProgress::Pairing], &prog.bCancel, !prog.bFullVisual, prog.bLzVisual, prog.bNoteStats ) )
        { sError = L"Cancelled."; return nullptr; }
        song->vKnownId = std::vector< uint64_t >();
        song->vKnownEnd = std::vector< uint64_t >();
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
    std::unique_ptr< Song > song;
    if ( Archive::IsCompressed( sPath ) )
    {
        // A compressed file or archive: its MIDI read as it is decompressed, never expanded (song/Archive.h)
        progress.iStage = LoadProgress::Reading;
        std::unique_ptr< Archive::Reader > reader = Archive::Open( sPath, &progress.bCancel, sError );
        if ( !reader ) return nullptr;
        stats.adStageSeconds[LoadProgress::Reading] = ( Timer::NowMicroSecs() - llStart ) / 1e6;
        song = LoadImpl( nullptr, reader->Total(), nullptr, iThreads, progress, stats, sError, reader.get() );
        stats.sUnpacked = reader->Summary();
    }
    else
    {
        MappedFile file;
        if ( !file.Open( sPath, sError ) ) return nullptr;
        stats.adStageSeconds[LoadProgress::Reading] = ( Timer::NowMicroSecs() - llStart ) / 1e6;
        song = LoadImpl( file.Data(), file.Size(), &file, iThreads, progress, stats, sError );
    }
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
        L"File: %ls\r\n%ls"
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
        sFile.c_str(), sUnpacked.empty() ? L"" : ( L"Unpacked: " + sUnpacked + L"\r\n" ).c_str(), MB( (double)llFileSize ),
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
