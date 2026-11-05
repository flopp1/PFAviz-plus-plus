#include "song/Loader.h"

#include <algorithm>
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>

#include "core/Lz4.h"
#include "core/MappedFile.h"
#include "song/Archive.h"
#include "song/Dedup.h"
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

    const unsigned TickIndexShift = 12;     // TrackScan::vIndex: every 4096th group

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
        // ordinal among the track's NoteOns of the key; a run of consecutive unpaired ones as its first,
        // with end tick -(its length) and duration -1 (a track of NoteOns with no offs: one entry a key,
        // not 24 bytes a note)
        void Finish( uint64_t llTrack, std::vector< uint64_t > &vId, std::vector< int64_t > &vEnd, std::vector< int64_t > &vDur )
        {
            for ( unsigned key = 0; key < 256; key++ )
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
                uint64_t llRun = 0, llRunLen = 0;
                auto FlushRun = [&]() {
                    if ( !llRunLen ) return;
                    vId.push_back( llBase | llRun );
                    vEnd.push_back( -(int64_t)llRunLen );
                    vDur.push_back( -1 );
                    llRunLen = 0;
                };
                for ( size_t w = 0; w < k.vPaired.size(); w++ )
                {
                    uint64_t v = ~k.vPaired[w];
                    if ( w + 1 == k.vPaired.size() && ( k.n & 63 ) ) v &= ( uint64_t( 1 ) << ( k.n & 63 ) ) - 1;
                    for ( ; v; v &= v - 1 )
                    {
                        uint64_t id = w * 64 + __builtin_ctzll( v );
                        if ( llRunLen && llRun + llRunLen == id ) { llRunLen++; continue; }
                        FlushRun();
                        LongBefore( id );
                        llRun = id;
                        llRunLen = 1;
                    }
                }
                FlushRun();
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
            size_t iFoldAt = size_t( 1 ) << 18;                    // vStack's size at which Push folds it
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
            // A stack that only grows (NoteOns with no offs, a run a tick): its older half folded into runs
            // of consecutive NoteOns at their first one's tick. A note in them paired later may then be taken
            // for longer than it is, which only keeps it among the known notes, with its end exact
            if ( k.vStack.size() > k.iFoldAt )
            {
                size_t iHalf = k.vStack.size() / 2, w = 0;
                for ( size_t r = 1; r < iHalf; r++ )
                {
                    Run &a = k.vStack[w], &b = k.vStack[r];
                    if ( a.uFirst + a.uLen == b.uFirst ) a.uLen += b.uLen;
                    else k.vStack[++w] = b;
                }
                k.vStack.erase( k.vStack.begin() + (ptrdiff_t)( w + 1 ), k.vStack.begin() + (ptrdiff_t)iHalf );
                k.iFoldAt = std::max< size_t >( k.iFoldAt, k.vStack.size() * 2 );    // (what did not fold is not tried again soon)
            }
        }
        static void PopBack( Key &k ) { if ( !--k.vStack.back().uLen ) k.vStack.pop_back(); }
        int64_t m_llLongTicks;
        std::unique_ptr< Key > m_a[256];
    };

    struct TrackScan
    {
        uint64_t llBegin = 0, llEnd = 0;    // byte range of the track data in the file
        // The groups (a song can have billions, held from the count pass to the build), each as its tick
        // less the previous group's and the bytes of its runs: varint records in pages that grow with
        // the track, a record repeated as one record and a count (note spam at a steady step: a few bytes
        // for thousands of groups), about 2 bytes a group otherwise. A page holds whole records; a record
        // starts at every 4096th group (vIndex), where a cursor can start.
        //   record := varint( delta << 1 ) varint( size ) [ varint( repeats - 1 << 1 | 1 ) ]
        struct Page { std::unique_ptr< uint8_t[] > data; uint32_t uUsed = 0, uCap = 0; };
        struct IndexPoint { int64_t llTick; uint32_t uPage, uOff; };  // llTick: the group before's (INT64_MIN: none)
        std::vector< Page > vPages;
        std::vector< IndexPoint > vIndex;       // per 4096 groups
        uint64_t llGroups = 0;
        uint32_t uMaxGroup = 0;                 // the largest group's bytes
        int64_t llLastTick = 0;                 // the last group's
        int64_t llPendDelta = 0;                // the record not yet written, and how many groups it stands for
        uint32_t uPendSize = 0;
        uint64_t llPendCount = 0;
        std::vector< Checkpoint > vCps;
        void AddGroup( int64_t llTick, uint32_t uSize )
        {
            uint64_t g = llGroups++;
            int64_t d = llTick - ( g ? llLastTick : 0 );
            llLastTick = llTick;
            uMaxGroup = std::max( uMaxGroup, uSize );
            if ( !( g & ( ( uint64_t( 1 ) << TickIndexShift ) - 1 ) ) )
            {
                FlushRecord();
                Room();
                vIndex.push_back( { g ? llTick - d : INT64_MIN, (uint32_t)( vPages.size() - 1 ), vPages.back().uUsed } );
            }
            else if ( llPendCount && d == llPendDelta && uSize == uPendSize ) { llPendCount++; return; }
            else FlushRecord();
            llPendDelta = d;
            uPendSize = uSize;
            llPendCount = 1;
        }
        // Writes the pending record, and gives the last page its exact size
        void Seal()
        {
            FlushRecord();
            if ( vPages.empty() || vPages.back().uUsed == vPages.back().uCap ) return;
            Page &pg = vPages.back();
            std::unique_ptr< uint8_t[] > p( new uint8_t[pg.uUsed + 1] );
            memcpy( p.get(), pg.data.get(), pg.uUsed );
            pg.data = std::move( p );
            pg.uCap = pg.uUsed;
        }
        void FlushRecord()
        {
            if ( !llPendCount ) return;
            Room();
            Page &pg = vPages.back();
            uint8_t *q = pg.data.get() + pg.uUsed;
            q = WriteVar( q, (uint64_t)llPendDelta << 1 );
            q = WriteVar( q, uPendSize );
            if ( llPendCount > 1 ) q = WriteVar( q, ( ( llPendCount - 1 ) << 1 ) | 1 );
            pg.uUsed = (uint32_t)( q - pg.data.get() );
            llPendCount = 0;
        }
        void Room()     // room for a record (at most 30 bytes)
        {
            if ( !vPages.empty() && vPages.back().uCap - vPages.back().uUsed >= 30 ) return;
            Page pg;
            pg.uCap = vPages.empty() ? 256 : std::min< uint32_t >( 1 << 20, vPages.back().uCap * 2 );
            pg.data.reset( new uint8_t[pg.uCap] );
            vPages.push_back( std::move( pg ) );
        }
        static uint8_t *WriteVar( uint8_t *q, uint64_t v )
        {
            while ( v >= 0x80 ) { *q++ = (uint8_t)( v | 0x80 ); v >>= 7; }
            *q++ = (uint8_t)v;
            return q;
        }
        size_t GroupBytes() const { size_t n = 0; for ( const Page &pg : vPages ) n += pg.uCap; return n + vIndex.size() * sizeof( IndexPoint ); }
        RefTrackInfo info;
        std::vector< RefMetaEvent > vMeta;
        uint32_t aKeyCounts[256] = {};
        uint64_t llChannelEvents = 0;
        uint64_t llOutOfRange = 0;          // channel events dropped for a controller or program past 127
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
        // Deduplication (song/Dedup.h): the hash of the track's bytes (bHashed), and the track it is a
        // copy of when it was not counted for being one (-1: none)
        Dedup::Hasher::Value hash = {};
        bool bHashed = false;
        int iCopyOf = -1;
    };

    // Up to 5 bytes: the spec's limit is 4 (28 bits), but some writers encode 32-bit values in 5 (nut3's
    // track starts, past tick 2^28)
    inline bool ReadVarLen( const uint8_t *&p, const uint8_t *pEnd, long long &llOut )
    {
        llOut = 0;
        for ( int i = 0; i < 5; i++ )
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
        uint32_t uSize = 0;     // the bytes of the group Next returned last
        // The record being read: its groups left, delta and size; where the next one starts
        uint64_t llLeft = 0;
        int64_t llDelta = 0;
        uint32_t uRecSize = 0, uPage = 0;
        const uint8_t *q = nullptr;
        explicit TickCursor( const TrackScan &s ) : p( &s ) { if ( !s.vPages.empty() ) q = s.vPages[0].data.get(); }
        bool Done() const { return g >= p->llGroups; }
        int64_t Peek() { if ( !llLeft ) Load(); return llPrev + llDelta; }
        int64_t Next()
        {
            if ( !llLeft ) Load();
            llLeft--;
            g++;
            uSize = uRecSize;
            return llPrev += llDelta;
        }
        // Before the first group at tick t or later
        static TickCursor At( const TrackScan &s, int64_t t ) { return At( TickCursor( s ), t ); }
        // The same from cursor from on (from's next group, or the index point past it)
        static TickCursor At( const TickCursor &from, int64_t t )
        {
            const TrackScan &s = *from.p;
            TickCursor c = from;
            if ( c.Done() || c.Peek() >= t ) return c;
            const std::vector< TrackScan::IndexPoint > &vi = s.vIndex;
            size_t k = (size_t)( std::lower_bound( vi.begin(), vi.end(), t, []( const TrackScan::IndexPoint &a, int64_t v ) { return a.llTick < v; } ) - vi.begin() );
            if ( k > 1 && ( uint64_t( k - 1 ) << TickIndexShift ) > from.g )
            {
                c = TickCursor( s );
                c.g = uint64_t( k - 1 ) << TickIndexShift;
                c.llPrev = vi[k - 1].llTick;
                c.uPage = vi[k - 1].uPage;
                c.q = s.vPages[c.uPage].data.get() + vi[k - 1].uOff;
            }
            while ( !c.Done() && c.Peek() < t ) c.Next();
            return c;
        }
    private:
        static uint64_t Var( const uint8_t *&q )
        {
            uint64_t v = 0;
            for ( unsigned s = 0;; s += 7 ) { uint8_t b = *q++; v |= (uint64_t)( b & 0x7F ) << s; if ( !( b & 0x80 ) ) return v; }
        }
        void Load()
        {
            if ( q == p->vPages[uPage].data.get() + p->vPages[uPage].uUsed ) q = p->vPages[++uPage].data.get();
            llDelta = (int64_t)( Var( q ) >> 1 );
            uRecSize = (uint32_t)Var( q );
            llLeft = 1;
            const TrackScan::Page &pg = p->vPages[uPage];
            if ( q < pg.data.get() + pg.uUsed && ( *q & 1 ) ) llLeft += Var( q ) >> 1;
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
    //        (the groups' runs at their offsets in it, pOff from group llFirstWrite on); the groups between the checkpoint
    //        and llFirstWrite go to pDiscard (room for the largest group).
    // Fused: Count, and also encodes the groups' runs into pScan->vPacked (DESIGN.md 4.2a).
    // Stream: Fused over [pTrack, pEnd), a window of the track (bLast: the rest of it), stopping before an
    //         event the window cuts (*pState: where it stopped, and what carries over)
    // A window ends inside an event when a read runs past it (MORE): back to the event's start
    // Note keys past 127 are kept as keys 128-255 (256-key files; the stream carries the key's top bit
    // as the run's velocity bit 7); other data bytes past 127 drop their event, as the original does
#define MORE( c ) if ( MODE == Stream && !bLast && ( c ) ) { p = pEventStart; llTick = llTickBefore; iRunning = iRunningBefore; bMore = true; break; }
    template < int MODE >
    void ScanTrack( const uint8_t *pTrack, const uint8_t *pEnd, const Checkpoint &start, uint64_t llStopGroup, int iTrack,
                    TrackScan *pScan, const uint32_t *pOff, uint8_t *pPayload, uint64_t llFirstWrite = 0, uint8_t *pDiscard = nullptr,
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
        uint32_t aKeyCounts[256] = {};
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
                uint8_t aData[2] = { p[0], iDataBytes == 2 ? std::min< uint8_t >( p[1], 0x7F ) : (uint8_t)0 };
                p += iDataBytes;
                if ( aData[0] >= 0x80 && iType != 0x8 && iType != 0x9 && iType != 0xA )
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
                    if ( MODE == Emit ) pDst = llNext >= llFirstWrite ? pPayload + pOff[llNext - llFirstWrite] : pDiscard;
                    else if ( FUSED && pDst - pStage.get() >= (ptrdiff_t)FusedChunk ) Flush( llNext );
                    llGroup = (int64_t)llNext;
                    llGroupTick = llTick;
                }

                unsigned op = OpOf( iType, aData[1] );
                unsigned uBank = ( iType == 0x8 || iType == 0x9 ) ? aData[0] & 0x80 : 0;
                if ( op == 2 && uBank ) op = 1;
                unsigned uVelocity = aData[1] | uBank;
                Reserve();
                if ( op <= 2 )
                {
                    uint32_t uKey = ( op << 12 ) | ( iChannel << 8 ) | ( op == 2 ? 0 : uVelocity );
                    if ( uKey == uRunKey )
                    {
                        if ( WRITE ) { pLastKey = pDst; *pDst++ = aData[0] & 0x7F; }
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
                            if ( op != 2 ) *pDst++ = (uint8_t)uVelocity;
                            pLastKey = pDst;
                            *pDst++ = aData[0] & 0x7F;
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
                    // A tempo of 0 (a damaged file: time would stop there) is dropped, as one of the wrong length
                    if ( ( cType == RefMetaEvent::SetTempo && llLen == 3 && ( pMeta[0] | pMeta[1] | pMeta[2] ) ) || ( cType == RefMetaEvent::TimeSignature && llLen == 4 ) )
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
            for ( int k = 0; k < 256; k++ ) pScan->aKeyCounts[k] += aKeyCounts[k];
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
        // Fused single read (4.0, 4.2a) on a disk with a seek penalty, always: cached, it loads as fast as
        // the two-pass read; and the two-pass emit pass needs the file still cached, which the load's own
        // memory can evict (nut3, 7.7 GB peaking at 21 GB: the second read took 68 s from the disk)
        bool bFused = prog.iFusedRead > 0 || pReader || ( prog.iFusedRead < 0 && bSequential );
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
        // A 12-byte MThd (fMCG's quirk): no division, the "division" being the next chunk's 'M','T'. Such
        // files are type 1 at 480 ticks a quarter; the chunks go on from the 'M'
        bool bShortMThd = pHdr[4] == 'M' && pHdr[5] == 'T';
        if ( bShortMThd )
        {
            info.iDivision = 480;
            if ( pReader ) pReader->PutBack( pHdr + 4, 2 );
        }
        if ( info.iFormatType > 2 || info.iDivision == 0 ) { sError = L"Unsupported MIDI header (format or division)."; return nullptr; }

        // Phase 1+2: chunk discovery and count pass. Workers claim the next chunk under a lock; on an HDD
        // discovery and parsing follow the sequential reader, so the disk only sees one forward stream.
        long long llPhase = Timer::NowMicroSecs();
        prog.iStage = LoadProgress::Counting;
        prog.aTotal[LoadProgress::Counting] = llSize;
        std::mutex mtx;
        uint64_t llNextChunk = bShortMThd ? 12 : 8 + llHdr;
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
        std::unordered_map< uint64_t, std::vector< uint32_t > > mSeqFirst;     // deduplication, sequential read: tracks by hash
        if ( !bSequential && !pReader )
        {
            for ( uint64_t q = llNextChunk; q + 8 <= llSize; )
            {
                uint64_t llBegin = q + 8, llEnd = std::min< uint64_t >( llBegin + BE( pData + q + 4, 4 ), llSize );
                if ( memcmp( pData + q, "MTrk", 4 ) == 0 ) NewScan( llBegin, llEnd );    // unknown chunks: skipped
                q = llEnd;
            }
            llNextChunk = llSize;
            if ( prog.bDedup )
            {
                // Identical tracks: only the first of them is counted (song/Dedup.h)
                Parallel::For( iThreads, vScans.size(), [&]( size_t t, int ) {
                    Dedup::Hasher h;
                    h.Add( pData + vScans[t]->llBegin, (size_t)( vScans[t]->llEnd - vScans[t]->llBegin ) );
                    vScans[t]->hash = h.Finish();
                    vScans[t]->bHashed = true;
                } );
                std::unordered_map< uint64_t, std::vector< uint32_t > > mFirst;
                for ( size_t t = 0; t < vScans.size(); t++ )
                {
                    std::vector< uint32_t > &v = mFirst[vScans[t]->hash.a];
                    for ( uint32_t c : v ) if ( vScans[c]->hash == vScans[t]->hash ) { vScans[t]->iCopyOf = (int)c; break; }
                    if ( vScans[t]->iCopyOf < 0 ) v.push_back( (uint32_t)t );
                }
            }
            for ( size_t t = 0; t < vScans.size(); t++ ) if ( vScans[t]->iCopyOf < 0 ) vOrder.push_back( (uint32_t)t );
            std::stable_sort( vOrder.begin(), vOrder.end(), [&]( uint32_t a, uint32_t b ) {
                return vScans[a]->llEnd - vScans[a]->llBegin > vScans[b]->llEnd - vScans[b]->llBegin; } );
            prog.llTracksFound = vScans.size();
        }
        // An archive: one thread decompresses (ReadTracks, its own) while the workers count. It cuts the
        // tracks into pieces of up to PieceBytes, with at most about QueueBytes of them waiting: a track
        // of one piece is counted whole (Fused) by any worker; a longer one by the worker that takes it,
        // piece by piece as they come (ScanTrack< Stream >). Decompression is the floor, so it never
        // waits on counting (the workers used to take turns at the reader, a long track counted in turn).
        // Deduplicating, a track is taken once read whole or HoldBytes of it are, so a copy of a track
        // before it (known from its hash at its end) is never counted
        const size_t PieceBytes = 16 << 20, PoolBytes = 1 << 20;    // a piece from PoolBytes: a PieceBytes buffer, reused
        const uint64_t QueueBytes = 256 << 20, HoldBytes = 64 << 20;
        struct Piece { std::unique_ptr< uint8_t[] > p; size_t iCap = 0, n = 0; bool bLast = false; };   // (not zeroed)
        struct Job                              // a track
        {
            TrackScan *pScan = nullptr;
            int iTrack = -1;
            std::deque< Piece > q;              // its pieces read, not yet taken
            uint64_t llRead = 0;                // bytes of them
            bool bEnd = false;                  // no more will come (its last read, or the reader stopped)
            bool Ready( bool bDedup ) const { return bEnd || !bDedup || llRead >= HoldBytes; }
        };
        std::mutex mq;
        std::condition_variable cvJob, cvPiece, cvSpace;
        std::deque< std::shared_ptr< Job > > qJobs;     // tracks not yet taken
        std::vector< std::unique_ptr< uint8_t[] > > vPool;
        uint64_t llQueued = 0;                  // capacity of the pieces waiting or being counted
        bool bReadDone = false;
        std::wstring sStreamError;
        auto ReadTracks = [&]() {
            uint64_t llStreamPos = 0;
            std::shared_ptr< Job > job;
            for ( ;; )
            {
                if ( Cancelled() ) break;
                char aType[4];
                uint64_t llLen = 0;
                if ( !pReader->NextChunk( aType, llLen ) ) break;
                llStreamPos += 8;
                if ( memcmp( aType, "MTrk", 4 ) != 0 ) { llStreamPos += llLen; continue; }  // unknown chunk: skipped
                job = std::make_shared< Job >();
                {
                    std::lock_guard< std::mutex > lock( mtx );
                    job->iTrack = (int)vScans.size();
                    job->pScan = NewScan( llStreamPos, llStreamPos + llLen );
                    prog.llTracksFound = vScans.size();
                }
                llStreamPos += llLen;
                uint64_t llLeft = llLen;
                bool bFirst = true;
                Dedup::Hasher hasher;
                do
                {
                    size_t iWant = (size_t)std::min< uint64_t >( llLeft, PieceBytes );
                    Piece piece;
                    {
                        std::unique_lock< std::mutex > lock( mq );
                        // (rechecked now and then: a cancel is not signalled, and the workers may stop freeing)
                        while ( llQueued + iWant > QueueBytes && llQueued && !Cancelled() ) cvSpace.wait_for( lock, std::chrono::milliseconds( 50 ) );
                        piece.iCap = iWant >= PoolBytes ? PieceBytes : iWant;
                        llQueued += piece.iCap;
                        if ( iWant >= PoolBytes && !vPool.empty() ) { piece.p = std::move( vPool.back() ); vPool.pop_back(); }
                    }
                    if ( !piece.p ) piece.p.reset( new uint8_t[piece.iCap] );
                    while ( piece.n < iWant )
                    {
                        size_t r = pReader->ReadData( piece.p.get() + piece.n, iWant - piece.n );
                        if ( !r ) break;
                        piece.n += r;
                    }
                    llLeft = piece.n < iWant ? 0 : llLeft - piece.n;   // short: the file ended inside the track
                    piece.bLast = !llLeft;
                    if ( prog.bDedup )
                    {
                        hasher.Add( piece.p.get(), piece.n );
                        if ( piece.bLast )
                        {
                            // A copy of a track before it: its worker drops what it counted (tracks are
                            // read in order, so every one before has its hash)
                            TrackScan *pScan = job->pScan;
                            pScan->hash = hasher.Finish();
                            pScan->bHashed = true;
                            std::lock_guard< std::mutex > lock( mtx );
                            std::vector< uint32_t > &v = mSeqFirst[pScan->hash.a];
                            for ( uint32_t c : v ) if ( vScans[c]->hash == pScan->hash ) { pScan->iCopyOf = (int)c; break; }
                            if ( pScan->iCopyOf < 0 ) v.push_back( (uint32_t)job->iTrack );
                        }
                    }
                    prog.aDone[LoadProgress::Reading] = pReader->Done();
                    prog.aDone[LoadProgress::Counting] = pReader->Done();
                    std::lock_guard< std::mutex > lock( mq );
                    bool bWasReady = !bFirst && job->Ready( prog.bDedup );
                    job->llRead += piece.n;
                    job->q.push_back( std::move( piece ) );
                    job->bEnd = !llLeft;
                    if ( bFirst ) qJobs.push_back( job );
                    if ( !bWasReady && job->Ready( prog.bDedup ) ) cvJob.notify_all();
                    cvPiece.notify_all();
                    bFirst = false;
                } while ( llLeft && pReader->sError.empty() && !Cancelled() );
                if ( !pReader->sError.empty() ) break;
            }
            std::lock_guard< std::mutex > lock( mq );
            sStreamError = pReader->sError;
            if ( job ) job->bEnd = true;
            bReadDone = true;
            cvJob.notify_all();
            cvPiece.notify_all();
        };
        auto CountTracks = [&]() {
            std::vector< uint8_t > vWin;    // a long track's unfinished event, then the next piece
            auto Release = [&]( Piece &piece ) {
                std::lock_guard< std::mutex > lock( mq );
                llQueued -= piece.iCap;
                if ( piece.iCap == PieceBytes ) vPool.push_back( std::move( piece.p ) );
                cvSpace.notify_one();
            };
            for ( ;; )
            {
                std::shared_ptr< Job > job;
                bool bCopy = false;     // read whole, and a copy: not counted
                {
                    std::unique_lock< std::mutex > lock( mq );
                    // (tracks come in order, so only the last can be still coming)
                    cvJob.wait( lock, [&]() { return ( !qJobs.empty() && qJobs.front()->Ready( prog.bDedup ) ) || bReadDone || Cancelled(); } );
                    if ( qJobs.empty() || Cancelled() ) return;
                    job = std::move( qJobs.front() );
                    qJobs.pop_front();
                    bCopy = job->bEnd && job->pScan->iCopyOf >= 0;
                }
                TrackScan *pScan = job->pScan;
                StreamState st( pScan->llLongTicks );
                size_t iHave = 0;
                for ( bool bFirst = true;; bFirst = false )
                {
                    Piece piece;
                    {
                        std::unique_lock< std::mutex > lock( mq );
                        cvPiece.wait( lock, [&]() { return !job->q.empty() || job->bEnd || Cancelled(); } );
                        if ( job->q.empty() ) break;    // the reader stopped (an error, or cancelled)
                        piece = std::move( job->q.front() );
                        job->q.pop_front();
                    }
                    if ( bCopy ) {}
                    else if ( bFirst && piece.bLast )
                        ScanTrack< Fused >( piece.p.get(), piece.p.get() + piece.n, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, job->iTrack, pScan, nullptr, nullptr );
                    else if ( !st.bDone && !Cancelled() )   // (what follows the track's end is dropped)
                    {
                        // The piece itself, or after the unfinished event of the one before
                        const uint8_t *pB = piece.p.get();
                        size_t iSize = piece.n;
                        if ( iHave )
                        {
                            vWin.resize( iHave + piece.n );
                            memcpy( vWin.data() + iHave, piece.p.get(), piece.n );
                            pB = vWin.data();
                            iSize = iHave + piece.n;
                        }
                        ScanTrack< Stream >( pB, pB + iSize, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, job->iTrack, pScan, nullptr, nullptr,
                                             0, nullptr, &st, piece.bLast );
                        iHave = st.bDone ? 0 : iSize - st.iUsed;
                        if ( iHave && pB == vWin.data() ) memmove( vWin.data(), vWin.data() + st.iUsed, iHave );
                        else if ( iHave ) vWin.assign( pB + st.iUsed, pB + iSize );
                    }
                    bool bLast = piece.bLast;
                    Release( piece );
                    if ( bLast ) break;
                }
                if ( pScan->iCopyOf >= 0 )
                {
                    // A copy (ReadTracks, before this last piece): only what places it is kept
                    TrackScan kept;
                    kept.llBegin = pScan->llBegin, kept.llEnd = pScan->llEnd, kept.llLongTicks = pScan->llLongTicks;
                    kept.hash = pScan->hash, kept.bHashed = true, kept.iCopyOf = pScan->iCopyOf;
                    *pScan = std::move( kept );
                    prog.llTracksDone++;
                    continue;
                }
                prog.llEvents += pScan->llChannelEvents;
                prog.llNotes += pScan->info.llNoteCount;
                prog.llTracksDone++;
            }
        };
        if ( pReader )
        {
            prog.aTotal[LoadProgress::Reading] = pReader->Total();
            prog.aTotal[LoadProgress::Counting] = pReader->Total();
        }
        std::thread reader;
        if ( pReader ) reader = std::thread( ReadTracks );
        int iWorkers = pReader ? std::max( 1, iThreads - 1 ) : iThreads;   // (an archive: the reader is one)
        Parallel::For( iWorkers, (size_t)iWorkers, [&]( size_t, int ) {
            if ( pReader ) { CountTracks(); return; }
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
                if ( prog.bDedup && bSequential )
                {
                    Dedup::Hasher h;
                    h.Add( pData + pScan->llBegin, (size_t)( pScan->llEnd - pScan->llBegin ) );
                    pScan->hash = h.Finish();
                    pScan->bHashed = true;
                    std::lock_guard< std::mutex > lock( mtx );
                    std::vector< uint32_t > &v = mSeqFirst[pScan->hash.a];
                    for ( uint32_t c : v ) if ( vScans[c]->hash == pScan->hash ) { pScan->iCopyOf = (int)c; break; }
                    if ( pScan->iCopyOf < 0 ) v.push_back( (uint32_t)iTrack );
                }
                if ( pScan->iCopyOf >= 0 )
                {
                    prog.llTracksDone++;
                    prog.aDone[LoadProgress::Counting] = llBytesCounted += pScan->llEnd - pScan->llBegin + 8;
                    continue;
                }
                if ( bFused ) ScanTrack< Fused >( pData + pScan->llBegin, pData + pScan->llEnd, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, iTrack, pScan, nullptr, nullptr );
                else ScanTrack< Count >( pData + pScan->llBegin, pData + pScan->llEnd, Checkpoint{ 0, 0, 0, 0 }, UINT64_MAX, iTrack, pScan, nullptr, nullptr );
                prog.llEvents += pScan->llChannelEvents;
                prog.llNotes += pScan->info.llNoteCount;
                prog.llTracksDone++;
                prog.aDone[LoadProgress::Counting] = llBytesCounted += pScan->llEnd - pScan->llBegin + 8;
                if ( bSequential ) prog.aDone[LoadProgress::Reading] = pFile->Resident();
            }
        } );
        if ( reader.joinable() ) reader.join();
        if ( bSequential ) pFile->StopSequentialRead();
        if ( bSequential ) stats.dReaderSeconds = pFile->ReaderMicroSecs() / 1e6;
        if ( bFused && pFile ) pFile->Close(); // nothing reads the file after the fused count pass
        prog.aDone[LoadProgress::Reading] = llSize;
        prog.aDone[LoadProgress::Counting] = llSize;
        stats.adStageSeconds[LoadProgress::Counting] = Seconds( llPhase );
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }
        if ( !sStreamError.empty() ) { sError = sStreamError; return nullptr; }

        // Deduplication (song/Dedup.h): every track of the file (copies the reader did not hand out included,
        // and tracks found identical by their hash) as the track it copies. A track is drawn as its last
        // copy, which hides the others: the tracks kept are put in the order of their last copies, with
        // that copy's place for their colours (Song::vLogicalTracks).
        uint64_t llLogicalTracks = vScans.size();
        if ( prog.bDedup || ( pReader && pReader->Logical() ) )
        {
            const size_t S = vScans.size();
            std::vector< uint32_t > vLog;
            if ( pReader && pReader->Logical() ) vLog = *pReader->Logical();
            else { vLog.resize( S ); for ( size_t t = 0; t < S; t++ ) vLog[t] = (uint32_t)t; }
            std::vector< uint32_t > vCanon( S );
            std::unordered_map< uint64_t, std::vector< uint32_t > > mFirst;
            for ( size_t t = 0; t < S; t++ )
            {
                vCanon[t] = (uint32_t)t;
                TrackScan &sc = *vScans[t];
                if ( sc.iCopyOf >= 0 ) { vCanon[t] = vCanon[sc.iCopyOf]; continue; }
                if ( !sc.bHashed ) continue;
                std::vector< uint32_t > &v = mFirst[sc.hash.a];
                for ( uint32_t c : v ) if ( vScans[c]->hash == sc.hash ) { vCanon[t] = c; break; }
                if ( vCanon[t] == t ) v.push_back( (uint32_t)t );
            }
            std::vector< uint64_t > vCopies( S, 0 ), vLast( S, 0 );
            for ( size_t i = 0; i < vLog.size(); i++ )
            {
                uint32_t c = vLog[i] < S ? vCanon[vLog[i]] : 0;
                vLog[i] = c;
                vCopies[c]++;
                vLast[c] = i;
            }
            std::vector< uint32_t > vKept;
            for ( size_t t = 0; t < S; t++ ) if ( vCanon[t] == t && vCopies[t] ) vKept.push_back( (uint32_t)t );
            std::stable_sort( vKept.begin(), vKept.end(), [&]( uint32_t a, uint32_t b ) { return vLast[a] < vLast[b]; } );
            bool bSame = vKept.size() == S && vLog.size() == S;
            for ( size_t i = 0; bSame && i < S; i++ ) bSame = vKept[i] == i;
            if ( !bSame )
            {
                std::vector< uint32_t > vNew( S, UINT32_MAX );
                std::vector< std::unique_ptr< TrackScan > > vKeptScans;
                for ( size_t i = 0; i < vKept.size(); i++ )
                {
                    vNew[vKept[i]] = (uint32_t)i;
                    std::unique_ptr< TrackScan > &sc = vScans[vKept[i]];
                    for ( RefMetaEvent &m : sc->vMeta ) m.iTrack = (int)i;
                    for ( uint64_t &id : sc->vKnownId ) id = ( id & ( ( uint64_t( 1 ) << 48 ) - 1 ) ) | (uint64_t)i << 48;
                    vKeptScans.push_back( std::move( sc ) );
                }
                vScans = std::move( vKeptScans );
                song->vTrackCopies.resize( vKept.size() );
                for ( size_t i = 0; i < vKept.size(); i++ ) song->vTrackCopies[i] = vCopies[vKept[i]];
                song->vLogicalTracks.resize( vLog.size() );
                for ( size_t i = 0; i < vLog.size(); i++ ) song->vLogicalTracks[i] = vNew[vLog[i]];
                llLogicalTracks = vLog.size();
                stats.llKeptTracks = vKept.size();
            }
        }

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
                for ( int k = 0; k < 256; k++ ) llNotes += sc->aKeyCounts[k];
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
            for ( int k = 0; k < 256; k++ ) song->aKeyNoteCounts[k] += vScans[t]->aKeyCounts[k];
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
            const long long llCopies = (long long)song->TrackCopies( t );     // (the notes counted as many times as the track stands for)
            info.llVolumeSum += mti.llVolumeSum * llCopies;
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
            info.llNoteCount += mti.llNoteCount * llCopies;
        }
        info.iNumTracks = (int)vScans.size();
        info.llFirstNote = -1;
        stats.llTracks = llLogicalTracks;
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
            s->Seal();
            G += s->llGroups;
            llEvents += s->llChannelEvents;
            if ( s->llGroups ) llMaxTick = std::max( llMaxTick, s->llLastTick );
            stats.llGroupBytes += s->GroupBytes();
        }
        prog.aTotal[LoadProgress::Merging] = 3;
        EventStream &es = song->stream;
        es.llEvents = llEvents;
        stats.llEvents = llEvents;
        stats.llGroups = G;

        // The unique ticks: a bitmap over the ticks with each word's rank (kept until the tick and time
        // tables are built, which read the ticks from it), or when that would be too large, a sorted
        // list. (A list beside the bitmap took 8 bytes a tick: 8 GB of nut3's 26.5 GB peak.)
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
        }
        else
        {
            vUnique.reserve( G );
            for ( auto &s : vScans ) for ( TickCursor c( *s ); !c.Done(); ) vUnique.push_back( c.Next() );
            std::sort( vUnique.begin(), vUnique.end() );
            vUnique.erase( std::unique( vUnique.begin(), vUnique.end() ), vUnique.end() );
        }
        const uint64_t U = bBitmap ? vWordRank[llWords] : vUnique.size();
        // The ticks ranked [u, u + n), into pOut
        auto TicksAt = [&]( uint64_t u, size_t n, uint64_t *pOut ) {
            if ( !bBitmap ) { for ( size_t i = 0; i < n; i++ ) pOut[i] = (uint64_t)vUnique[u + i]; return; }
            if ( !n ) return;
            uint64_t w = (uint64_t)( std::upper_bound( vWordRank.begin(), vWordRank.begin() + llWords, u ) - vWordRank.begin() ) - 1;
            uint64_t b = vBits[w];
            for ( uint64_t k = u - vWordRank[w]; k; k-- ) b &= b - 1;   // the bits ranked below u
            for ( size_t i = 0; i < n; )
            {
                while ( !b ) b = vBits[++w];
                pOut[i++] = w * 64 + __builtin_ctzll( b );
                b &= b - 1;
            }
        };
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
                song->vKnownEnd[i] = vKnownEndTick[i] < 0 ? UINT64_MAX - (uint64_t)( -vKnownEndTick[i] - 1 ) : Rank( vKnownEndTick[i] );
        } );
        vKnownEndTick = std::vector< int64_t >();
        prog.aDone[LoadProgress::Merging] = 1;
        llStep = llPhase;
        Step( bBitmap ? L"unique ticks (bitmap)" : L"unique ticks (sort)" );

        // Groups and their run bytes per block of 256 unique ticks, for cutting the build into windows
        // (the groups' global order is made window by window there, so nothing here is a tick or a group
        // each), and each track's last unique tick
        const unsigned TickBlockShift = 8;
        const uint64_t llTickBlocks = ( U >> TickBlockShift ) + 1;
        std::vector< uint32_t > vBlockGroups( llTickBlocks, 0 );
        std::vector< uint64_t > vBlockRaw( llTickBlocks, 0 );
        song->vTrackLastU.assign( vScans.size(), UINT64_MAX );
        Parallel::For( iThreads, vScans.size(), [&]( size_t t, int ) {
            uint64_t u = 0, b = UINT64_MAX, n = 0, llRaw = 0;
            auto Add = [&]() { if ( n ) { __atomic_fetch_add( &vBlockGroups[b], (uint32_t)n, __ATOMIC_RELAXED ); __atomic_fetch_add( &vBlockRaw[b], llRaw, __ATOMIC_RELAXED ); } };
            for ( TickCursor c( *vScans[t] ); !c.Done(); )
            {
                u = RankFrom( c.Next(), u );
                if ( ( u >> TickBlockShift ) != b ) { Add(); b = u >> TickBlockShift; n = llRaw = 0; }
                n++;
                llRaw += c.uSize;
            }
            Add();
            if ( vScans[t]->llGroups ) song->vTrackLastU[t] = u;
        } );
        prog.aDone[LoadProgress::Merging] = 2;
        Step( L"groups per tick block" );

        // Meta in global order, tempo map, TimeTable, packed tick table
        for ( size_t t = 0; t < vScans.size(); t++ ) song->vMeta.insert( song->vMeta.end(), vScans[t]->vMeta.begin(), vScans[t]->vMeta.end() );
        std::stable_sort( song->vMeta.begin(), song->vMeta.end(), []( const RefMetaEvent &a, const RefMetaEvent &b ) {
            return a.llTick != b.llTick ? a.llTick < b.llTick : a.iTrack < b.iTrack; } );
        TempoMap tempo;
        tempo.Build( song->vMeta, info.iDivision );
        for ( RefMetaEvent &m : song->vMeta ) m.llMicroSec = tempo.At( m.llTick );
        for ( auto &v : song->vTrackMeta ) for ( RefMetaEvent &m : v ) m.llMicroSec = tempo.At( m.llTick );
        info.llTotalMicroSecs = tempo.At( info.llTotalTicks );
        es.ticks.BuildBlocks( U, [&]( size_t u0, size_t u1, uint64_t *p ) { TicksAt( u0, u1 - u0, p ); }, iThreads );
        es.times.BuildBlocks( U, [&]( size_t u0, size_t u1, uint64_t *p ) { TicksAt( u0, u1 - u0, p ); tempo.AtSorted( p, u1 - u0, p ); }, iThreads );
        vUnique.clear(); vUnique.shrink_to_fit();
        vBits.clear(); vBits.shrink_to_fit();
        vWordRank.clear(); vWordRank.shrink_to_fit();
        prog.aDone[LoadProgress::Merging] = 3;
        Step( L"tempo, tick, time tables" );
        stats.adStageSeconds[LoadProgress::Merging] = Seconds( llPhase );
        if ( Cancelled() ) { sError = L"Cancelled."; return nullptr; }

        // Phase 4: the payload, window by window. A window is a run of whole blocks of 256 unique ticks
        // (about WindowBytes of stream, WindowGroups groups and WindowTicks ticks, or one block), and the
        // groups' global order is made for each window alone: the tracks are cut into K runs of about
        // equal groups and the window's ticks into C columns; every (run, column) item counts its groups
        // per tick, the tracks of a run in order (a track has at most one group a tick, so its groups in
        // two columns never meet in a tick); the counts give each item its place in each tick; the items
        // place their groups (track and bytes), and each track's groups their places; the headers are
        // written and each group gets the offset of its runs; the runs are copied in (fused: out of the
        // tracks' packed chunks; two-pass: the emit pass re-reads each track from its checkpoint at or
        // before the window, at most CheckpointEvery groups more); and the window's complete chunks are
        // LZ4-compressed. Nothing is held per tick or group of the whole song but the tick and time
        // tables and the tracks' compact group records.
        llPhase = llStep = Timer::NowMicroSecs();
        prog.iStage = LoadProgress::Building;
        const size_t T = vScans.size();
        const uint64_t WindowBytes = 256ull << 20, WindowGroups = 16ull << 20;
        // The runs of tracks: no more than the groups a tick on average (each costs a count a tick), and
        // the columns a window is cut into: enough for the heaviest run's groups to be shared out
        size_t K = (size_t)std::max< uint64_t >( 1, std::min< uint64_t >( { (uint64_t)T, (uint64_t)iThreads, U ? G / U : 1 } ) );
        std::vector< size_t > vRunAt( 1, 0 );   // each run's first track
        {
            uint64_t llSum = 0;
            for ( size_t t = 0; t + 1 < T; t++ )
            {
                llSum += vScans[t]->llGroups;
                if ( vRunAt.size() < K && llSum * K >= G * vRunAt.size() ) vRunAt.push_back( t + 1 );
            }
            K = vRunAt.size();
            vRunAt.push_back( T );
        }
        uint64_t llHeaviest = 0;
        for ( size_t k = 0; k < K; k++ )
        {
            uint64_t n = 0;
            for ( size_t t = vRunAt[k]; t < vRunAt[k + 1]; t++ ) n += vScans[t]->llGroups;
            llHeaviest = std::max( llHeaviest, n );
        }
        const uint64_t CMax = G ? std::min< uint64_t >( (uint64_t)iThreads * 2, std::max< uint64_t >( 1, ( llHeaviest * iThreads * 2 + G - 1 ) / G ) ) : 1;
        const uint64_t WindowTicks = std::max< uint64_t >( 1 << TickBlockShift, ( 16ull << 20 ) / K );
        std::vector< uint64_t > vWinAt( 1, 0 );    // each window's first tick block
        uint64_t llRunBytes = 0;
        {
            uint64_t llBytes = 0, llGroups = 0, llTicks = 0;
            for ( uint64_t b = 0; b < llTickBlocks; b++ )
            {
                uint64_t n = std::min< uint64_t >( U, ( b + 1 ) << TickBlockShift ) - std::min< uint64_t >( U, b << TickBlockShift );
                uint64_t llB = vBlockRaw[b] + vBlockGroups[b];
                if ( b > vWinAt.back() && ( llBytes + llB > WindowBytes || llGroups + vBlockGroups[b] > WindowGroups || llTicks + n > WindowTicks ) )
                {
                    vWinAt.push_back( b );
                    llBytes = llGroups = llTicks = 0;
                }
                llBytes += llB;
                llGroups += vBlockGroups[b];
                llTicks += n;
                llRunBytes += vBlockRaw[b];
            }
            vWinAt.push_back( llTickBlocks );
        }
        vBlockGroups = std::vector< uint32_t >();
        vBlockRaw = std::vector< uint64_t >();
        prog.aTotal[LoadProgress::Building] = vWinAt.size() - 1;

        // The raw size at most (a header is at most the gap to the track before, up to T), until the
        // build has it
        es.llRawBytes = llRunBytes + G * EventStream::VarSize( ( (uint64_t)T << 1 ) | 1 );
        size_t iChunks = (size_t)( ( es.llRawBytes + EventStream::ChunkSize - 1 ) >> EventStream::ChunkShift );
        es.vChunks.resize( iChunks );
        // The stored chunks go straight to their place in the stream: one reservation (stored chunks are
        // at most raw), committed as each window appends its chunks
        if ( !es.Reserve() ) { sError = L"Out of memory for the event stream."; return nullptr; }
        uint64_t llData = 0, llRawPos = 0, llPrevTickStart = UINT64_MAX;
        std::vector< std::unique_ptr< uint8_t[] > > vPacked( iChunks );
        std::vector< TickCursor > vCursor, vNextCursor;     // each track's, at the window's first tick
        std::vector< TickCursor > vColCursor;               // per column but the first, per track: at its first tick
        vCursor.reserve( T );
        for ( size_t t = 0; t < T; t++ ) vCursor.emplace_back( *vScans[t] );
        vNextCursor = vCursor;
        std::vector< uint64_t > vCur( T, 0 ), vCurEnd( T, 0 );  // each track's first group in the window, and past its last
        std::vector< uint32_t > vCnt;           // per run, per tick of the window: its groups there, then where they go
        std::vector< uint32_t > vTickAt;        // per tick of the window, and one past: its first group
        std::vector< uint32_t > vGroupTrack, vGroupOff;     // per group of the window: its track; its bytes, then its runs' offset
        std::vector< uint32_t > vTrackOff;      // the window's groups track by track: their places, then their runs' offsets
        std::vector< uint32_t > vColCount, vColBase;        // per column, per track: its groups, and where they are in vTrackOff
        std::unique_ptr< uint8_t[] > pRaw, pPrev;
        uint64_t llPrevBase = 0, llRawCap = 0, llPrevCap = 0;
        size_t iNextChunk = 0;
        std::vector< std::vector< uint8_t > > vUnpack( bFused ? iThreads : 0 );
        // Two-pass: a scratch group per thread for the groups re-read before a window's first
        std::vector< std::unique_ptr< uint8_t[] > > vDiscard( bFused ? 0 : iThreads );
        if ( !bFused )
        {
            uint32_t uMax = 0;
            for ( auto &sc : vScans ) uMax = std::max( uMax, sc->uMaxGroup );
            for ( auto &p : vDiscard ) p.reset( new uint8_t[uMax + 16] );
        }
        struct Item { uint32_t uTrack, uChunk; uint64_t llLo, llHi; };
        std::vector< Item > vItems;
        double dCopy = 0, dPack = 0;
        for ( size_t w = 0; w + 1 < vWinAt.size(); w++ )
        {
            if ( Cancelled() ) break;
            const uint64_t u0 = std::min< uint64_t >( U, vWinAt[w] << TickBlockShift ), u1 = std::min< uint64_t >( U, vWinAt[w + 1] << TickBlockShift ), Uw = u1 - u0;
            if ( !Uw ) { prog.aDone[LoadProgress::Building] = w + 1; continue; }
            long long llT0 = Timer::NowMicroSecs();
            const int64_t llTickEnd = u1 < U ? (int64_t)es.ticks.Get( u1 ) : INT64_MAX;
            const size_t C = (size_t)std::min< uint64_t >( CMax, Uw );
            auto ColU = [&]( size_t j ) { return u0 + Uw * j / C; };
            auto ColTick = [&]( size_t j ) { return j == C ? llTickEnd : (int64_t)es.ticks.Get( ColU( j ) ); };
            // Each track's cursor at each column's first tick (column 0: vCursor), each from the one before
            if ( C > 1 ) vColCursor.resize( ( C - 1 ) * T, TickCursor( *vScans[0] ) );
            if ( C > 1 )
                Parallel::For( iThreads, ( T + 63 ) / 64, [&]( size_t b, int ) {
                    for ( size_t t = b * 64; t < std::min( T, ( b + 1 ) * 64 ); t++ )
                        for ( size_t j = 1; j < C; j++ ) vColCursor[( j - 1 ) * T + t] = TickCursor::At( j == 1 ? vCursor[t] : vColCursor[( j - 2 ) * T + t], ColTick( j ) );
                } );
            auto ColCursor = [&]( size_t t, size_t j ) { return j ? vColCursor[( j - 1 ) * T + t] : vCursor[t]; };
            // Each item's groups per tick, and each track's per column
            vCnt.assign( K * Uw, 0 );
            vColCount.assign( C * T, 0 );
            Parallel::For( iThreads, K * C, [&]( size_t i, int ) {
                size_t k = i / C, j = i % C;
                uint32_t *pCnt = vCnt.data() + k * Uw;
                const int64_t llEnd = ColTick( j + 1 );
                for ( size_t t = vRunAt[k]; t < vRunAt[k + 1]; t++ )
                {
                    TickCursor c = ColCursor( t, j );
                    uint64_t u = ColU( j );
                    uint32_t n = 0;
                    for ( ; !c.Done() && c.Peek() < llEnd; n++ )
                    {
                        u = es.ticks.LowerBoundFrom( (uint64_t)c.Next(), u );
                        pCnt[u - u0]++;
                    }
                    vColCount[j * T + t] = n;
                }
            } );
            // Each tick's first group, and each run's first place in each tick
            vTickAt.resize( Uw + 1 );
            const size_t R = (size_t)std::min< uint64_t >( Uw, (uint64_t)iThreads * 4 );
            auto RangeU = [&]( size_t r ) { return Uw * r / R; };
            std::vector< uint64_t > vRangeGroups( R + 1, 0 );
            Parallel::For( iThreads, R, [&]( size_t r, int ) {
                uint64_t n = 0;
                for ( uint64_t u = RangeU( r ); u < RangeU( r + 1 ); u++ ) for ( size_t k = 0; k < K; k++ ) n += vCnt[k * Uw + u];
                vRangeGroups[r + 1] = n;
            } );
            for ( size_t r = 0; r < R; r++ ) vRangeGroups[r + 1] += vRangeGroups[r];
            const uint64_t Gw = vRangeGroups[R];
            if ( Gw >= UINT32_MAX ) { sError = L"A tick holds too many events."; return nullptr; }
            Parallel::For( iThreads, R, [&]( size_t r, int ) {
                uint64_t n = vRangeGroups[r];
                for ( uint64_t u = RangeU( r ); u < RangeU( r + 1 ); u++ )
                {
                    vTickAt[u] = (uint32_t)n;
                    for ( size_t k = 0; k < K; k++ ) { uint32_t c = vCnt[k * Uw + u]; vCnt[k * Uw + u] = (uint32_t)n; n += c; }
                }
            } );
            vTickAt[Uw] = (uint32_t)Gw;
            // Each track's groups in vTrackOff, column after column
            vColBase.resize( C * T );
            {
                uint32_t n = 0;
                for ( size_t t = 0; t < T; t++ ) for ( size_t j = 0; j < C; j++ ) { vColBase[j * T + t] = n; n += vColCount[j * T + t]; }
            }
            // The groups in place
            vGroupTrack.resize( Gw );
            vGroupOff.resize( Gw );
            vTrackOff.resize( Gw );
            Parallel::For( iThreads, K * C, [&]( size_t i, int ) {
                size_t k = i / C, j = i % C;
                uint32_t *pCnt = vCnt.data() + k * Uw;
                for ( size_t t = vRunAt[k]; t < vRunAt[k + 1]; t++ )
                {
                    TickCursor c = ColCursor( t, j );
                    uint64_t u = ColU( j );
                    uint32_t *pOut = vTrackOff.data() + vColBase[j * T + t];
                    for ( uint32_t n = vColCount[j * T + t]; n; n-- )
                    {
                        u = es.ticks.LowerBoundFrom( (uint64_t)c.Next(), u );
                        uint32_t uPos = pCnt[u - u0]++;
                        vGroupTrack[uPos] = (uint32_t)t;
                        vGroupOff[uPos] = c.uSize;
                        *pOut++ = uPos;
                    }
                    if ( j + 1 == C ) vNextCursor[t] = c;
                }
            } );
            for ( size_t t = 0; t < T; t++ ) { vCur[t] = vCursor[t].g; vCurEnd[t] = vNextCursor[t].g; }
            vCursor.swap( vNextCursor );

            // The headers' and runs' bytes per tick range, then where each range starts
            auto Header = [&]( uint64_t g, uint64_t u ) -> uint64_t {
                uint32_t t = vGroupTrack[g];
                uint64_t llGap = g == vTickAt[u] ? t : t - vGroupTrack[g - 1] - 1;
                return ( llGap << 1 ) | ( g + 1 == vTickAt[u + 1] ? 1 : 0 );
            };
            struct Range { uint64_t llBytes = 0, llLastStart = 0; std::vector< EventStream::SeekPoint > vSeek; };
            std::vector< Range > vRanges( R );
            Parallel::For( iThreads, R, [&]( size_t r, int ) {
                Range &rg = vRanges[r];
                for ( uint64_t u = RangeU( r ); u < RangeU( r + 1 ); u++ )
                {
                    rg.llLastStart = rg.llBytes;
                    for ( uint64_t g = vTickAt[u]; g < vTickAt[u + 1]; g++ ) rg.llBytes += EventStream::VarSize( Header( g, u ) ) + vGroupOff[g];
                }
            } );
            std::vector< uint64_t > vRangeAt( R + 1, llRawPos );
            for ( size_t r = 0; r < R; r++ ) vRangeAt[r + 1] = vRangeAt[r] + vRanges[r].llBytes;
            const uint64_t llWin0 = llRawPos, llWin1 = vRangeAt[R];
            // The buffer starts at the window's first chunk: the tail of the last window's partial chunk
            // comes along
            uint64_t llBase = ( llWin0 >> EventStream::ChunkShift ) << EventStream::ChunkShift;
            if ( llWin1 - llBase >= UINT32_MAX ) { sError = L"A tick holds too many events."; return nullptr; }   // (offsets are 32-bit)
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
            // Headers, each group's runs' offset, and a seek point for the first tick that starts in each chunk
            Parallel::For( iThreads, R, [&]( size_t r, int ) {
                Range &rg = vRanges[r];
                uint64_t llOff = vRangeAt[r];
                uint64_t llPrevStart = r ? vRangeAt[r - 1] + vRanges[r - 1].llLastStart : llPrevTickStart;
                for ( uint64_t u = RangeU( r ); u < RangeU( r + 1 ); u++ )
                {
                    if ( llPrevStart == UINT64_MAX || ( llPrevStart >> EventStream::ChunkShift ) != ( llOff >> EventStream::ChunkShift ) ) rg.vSeek.push_back( { u0 + u, llOff } );
                    llPrevStart = llOff;
                    for ( uint64_t g = vTickAt[u]; g < vTickAt[u + 1]; g++ )
                    {
                        uint8_t *q = EventStream::WriteVar( pRaw.get() + ( llOff - llBase ), Header( g, u ) );
                        llOff = llBase + (uint64_t)( q - pRaw.get() );
                        uint32_t uSize = vGroupOff[g];
                        vGroupOff[g] = (uint32_t)( llOff - llBase );
                        llOff += uSize;
                    }
                }
            } );
            for ( Range &rg : vRanges ) es.vSeek.insert( es.vSeek.end(), rg.vSeek.begin(), rg.vSeek.end() );
            llPrevTickStart = vRangeAt[R - 1] + vRanges[R - 1].llLastStart;
            llRawPos = llWin1;
            Parallel::For( iThreads, ( Gw + 65535 ) >> 16, [&]( size_t c, int ) {
                for ( uint64_t i = c << 16; i < std::min< uint64_t >( Gw, ( c + 1 ) << 16 ); i++ ) vTrackOff[i] = vGroupOff[vTrackOff[i]];
            } );
            // Track t's groups of the window, from vCur[t] on: their runs' offsets
            auto TrackOffs = [&]( size_t t ) { return vTrackOff.data() + vColBase[t]; };
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
                        if ( g >= it.llLo ) memcpy( pRaw.get() + TrackOffs( it.uTrack )[g - it.llLo], p, (size_t)( q - p ) );
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
                    ScanTrack< Emit >( pData + s.llBegin, pData + s.llEnd, start, sg.llStop, (int)sg.uTrack, nullptr, TrackOffs( sg.uTrack ) + ( sg.llFirst - vCur[sg.uTrack] ), pRaw.get(), sg.llFirst,
                                       vDiscard[iThread].get() );
                } );
            }
            long long llT1 = Timer::NowMicroSecs();
            dCopy += ( llT1 - llT0 ) / 1e6;

            // LZ4 per 64 KiB chunk (stored as is when that is not smaller): the chunks that end in the
            // window (the last window: all the rest), then appended to the stream in order
            if ( u1 == U ) es.llRawBytes = llWin1;
            size_t iEnd = (size_t)( u1 == U ? ( llWin1 + EventStream::ChunkSize - 1 ) >> EventStream::ChunkShift : llWin1 >> EventStream::ChunkShift );
            Parallel::For( iThreads, iEnd - iNextChunk, [&]( size_t i, int ) {
                size_t c = iNextChunk + i;
                const uint8_t *pSrc = pRaw.get() + ( ( (uint64_t)c << EventStream::ChunkShift ) - llBase );
                size_t iRaw = es.ChunkRaw( c );
                std::unique_ptr< uint8_t[] > pTmp( new uint8_t[Lz4::Bound( iRaw )] );
                size_t iSize = Lz4::CompressHC( pSrc, iRaw, pTmp.get() );
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
        es.llRawBytes = llRawPos;
        es.vChunks.resize( (size_t)( ( llRawPos + EventStream::ChunkSize - 1 ) >> EventStream::ChunkShift ) );
        es.vChunks.shrink_to_fit();
        es.vSeek.shrink_to_fit();
        pPrev.reset();
        vUnpack.clear();
        vDiscard.clear();
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
        // (layered compressed files only: their makers leave the offs out on purpose, where in another
        // song a NoteOn without its off is a slip the original never draws)
        song->bEndOpenNotes = prog.bEndOpenNotes && pReader && pReader->Layers() >= 2;
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
        std::unique_ptr< Archive::Reader > reader;
        // Deduplication: a file of compressed streams, layered or not, is read as its distinct pieces
        // (song/Dedup.h); anything else (an archive, a MIDI inside not found) is opened as usual
        if ( progress.bDedup )
        {
            reader = Dedup::Open( sPath, progress.bDedupAssume, &progress.aDone[LoadProgress::Reading], &progress.aTotal[LoadProgress::Reading],
                                  &progress.bCancel, sError, Parallel::ThreadCount( iThreads ) );
            if ( !reader && !sError.empty() ) return nullptr;
        }
        if ( !reader ) reader = Archive::Open( sPath, &progress.bCancel, sError, progress.fnChoose, Parallel::ThreadCount( iThreads ) );
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
    wchar_t sDisk[240] = L"";
    if ( dReaderSeconds > 0 ) swprintf( sDisk, 160, L"  (disk read %.0f MB/s)\r\n", llFileSize / 1048576.0 / dReaderSeconds );
    if ( bFusedRead ) swprintf( sDisk + wcslen( sDisk ), 80, L"  (packed tracks %.1f MB)\r\n", llFusedBytes / 1048576.0 );
    swprintf( sDisk + wcslen( sDisk ), 80, L"  (group records %.1f MB)\r\n", llGroupBytes / 1048576.0 );
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
    wchar_t sKept[96] = L"";
    if ( llKeptTracks ) swprintf( sKept, 96, L", loaded as %llu (the rest identical copies)", (unsigned long long)llKeptTracks );
    if ( llOutOfRange ) swprintf( sOutOfRange, 64, L" (%llu past key/value 127, dropped)", (unsigned long long)llOutOfRange );
    swprintf( buf, 8192,
        L"File: %ls\r\n%ls"
        L"Size: %.1f MB, %ls%ls\r\n"
        L"Threads: %d\r\n\r\n"
        L"Tracks: %llu (%llu malformed)%ls\r\n"
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
        (unsigned long long)llTracks, (unsigned long long)llMalformedTracks, sKept, (unsigned long long)llEvents, sOutOfRange,
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
