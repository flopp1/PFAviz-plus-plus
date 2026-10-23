// A loaded song: metadata plus the tick-major event stream used for playback (DESIGN.md 5.2).
//
// Event stream layout (milestone 6)
//   Unique ticks u = 0..U-1 (only ticks that carry channel events), with their tick value and exact
//   microsecond time (R3) in packed tables.
//   The raw stream is the ticks in order; a tick is its groups in ascending track order, and a group
//   is one track's channel events at that tick, in file order, so walking the stream gives the
//   original global order (tick, track, position) (R1). Everything is inline:
//     group  := varint( trackGap << 1 | lastGroupOfTick ) run+
//               track = previous group's track in this tick + 1 + trackGap (previous = -1 at a tick start)
//     run    := header byte: bits 7..5 op, bit 4 last run of the group, bits 3..0 channel
//       op 0 NoteOn (velocity > 0)  header, velocity, keys...
//       op 1 NoteOff                header, release velocity, keys...
//       op 2 NoteOn velocity 0      header, keys...
//          keys: one byte each, bit 7 set on the last key of the run
//       op 3 PolyAT, 4 Controller, 7 PitchBend: header + 2 data bytes (one event)
//       op 5 Program, 6 ChannelAT:              header + 1 data byte  (one event)
//     varint: 7 bits per byte, least significant first, bit 7 = more
//   The raw bytes are cut into 64 KiB chunks, each LZ4-compressed (or stored as is when that is not
//   smaller). Readers decode chunk after chunk (StreamBytes). A seek point per chunk gives the first
//   tick that starts in it and where, for random access by tick.
//   Meta events are not in the stream (tempo and time signatures are in Song::vMeta); SysEx is
//   ignored entirely (DESIGN.md 1.5).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "midi/RefMidi.h"
#include "song/PackedArray.h"
#include "song/VisualStore.h"

struct StreamEvent
{
    uint32_t uTrack;
    uint8_t cStatus, cData1, cData2;
};

class EventStream
{
public:
    static const unsigned ChunkShift = 16;
    static const size_t ChunkSize = size_t( 1 ) << ChunkShift;
    struct Chunk { uint64_t llOffset; uint32_t uSize; };   // in data; uSize == raw size: stored uncompressed
    struct SeekPoint { uint64_t llU, llRaw; };              // a tick and the raw offset where it starts

    PackedMonotone ticks;                   // tick of each unique tick
    PackedMonotone times;                   // microseconds of each unique tick (R3)
    std::vector< Chunk > vChunks;
    std::vector< SeekPoint > vSeek;         // ascending; the first tick starting in each chunk that has one
    std::unique_ptr< uint8_t[] > data;      // the stored chunks
    uint64_t llRawBytes = 0, llDataBytes = 0;
    uint64_t llEvents = 0;

    size_t UniqueTicks() const { return ticks.Size(); }
    size_t ChunkRaw( size_t c ) const { return (size_t)std::min< uint64_t >( ChunkSize, llRawBytes - ( (uint64_t)c << ChunkShift ) ); }
    bool DecodeChunk( size_t c, uint8_t *pOut ) const;
    const SeekPoint &SeekPointFor( size_t u ) const;  // the last seek point at or before tick u
    size_t MemoryBytes() const;

    static inline uint64_t ReadVar( const uint8_t *&p )
    {
        uint64_t v = 0;
        unsigned s = 0, c;
        do { c = *p++; v |= ( uint64_t )( c & 0x7F ) << s; s += 7; } while ( c & 0x80 );
        return v;
    }
    static inline unsigned VarSize( uint64_t v ) { unsigned n = 1; while ( v >= 0x80 ) { v >>= 7; n++; } return n; }
    static inline uint8_t *WriteVar( uint8_t *p, uint64_t v )
    {
        while ( v >= 0x80 ) { *p++ = (uint8_t)( v | 0x80 ); v >>= 7; }
        *p++ = (uint8_t)v;
        return p;
    }

    // Walks ticks [u0, u1) in global order: onEvent( u, event ) for every event, and
    // onGroupEnd( u, track, bLastOfTick ) after every group; returning false from it stops the walk
    // (and Walk returns false).
    template < class FE, class FG >
    bool Walk( size_t u0, size_t u1, FE &&onEvent, FG &&onGroupEnd ) const;

    // Calls fn( u, event ) for every event that is not a note (NoteOn/NoteOff runs are skipped by
    // scanning for their last key byte, eight bytes at a time)
    template < class F >
    void ForEachNonNote( size_t u0, size_t u1, F &&fn ) const;

    // Calls fn( uniqueTickIndex, event ) for every event of the song in global order
    template < class F >
    void ForEach( F &&fn ) const
    {
        Walk( 0, UniqueTicks(), fn, []( size_t, uint32_t, bool ) { return true; } );
    }
};

// The raw stream as consecutive bytes: chunks are decoded as the reader moves on. After Need(),
// at least 16 bytes are readable at p (fewer only at the very end of the stream); every element of
// the grammar is at most 11 bytes.
class StreamBytes
{
public:
    const uint8_t *p = nullptr, *end = nullptr;

    void Start( const EventStream &es, uint64_t llRaw )
    {
        m_es = &es;
        if ( !m_buf ) m_buf.reset( new uint8_t[EventStream::ChunkSize + 64] ); // tail + chunk + decoder slack
        m_iNext = (size_t)( llRaw >> EventStream::ChunkShift );
        p = end = m_buf.get();
        if ( m_iNext < es.vChunks.size() )
        {
            es.DecodeChunk( m_iNext, m_buf.get() );
            end = m_buf.get() + es.ChunkRaw( m_iNext );
            p = m_buf.get() + ( llRaw & ( EventStream::ChunkSize - 1 ) );
            m_iNext++;
        }
    }
    inline void Need() { if ( __builtin_expect( end - p < 16, 0 ) ) Refill(); }
    // Offset of p in the raw stream
    uint64_t Pos() const
    {
        if ( !m_es ) return 0;
        return std::min< uint64_t >( (uint64_t)m_iNext << EventStream::ChunkShift, m_es->llRawBytes ) - (uint64_t)( end - p );
    }
    __attribute__( ( noinline ) ) void Refill()
    {
        size_t iTail = (size_t)( end - p );
        memmove( m_buf.get(), p, iTail );
        p = m_buf.get();
        end = p + iTail;
        if ( m_iNext < m_es->vChunks.size() )
        {
            m_es->DecodeChunk( m_iNext, m_buf.get() + iTail );
            end += m_es->ChunkRaw( m_iNext );
            m_iNext++;
        }
    }

private:
    const EventStream *m_es = nullptr;
    std::unique_ptr< uint8_t[] > m_buf;
    size_t m_iNext = 0;
};

template < class FE, class FG >
bool EventStream::Walk( size_t u0, size_t u1, FE &&onEvent, FG &&onGroupEnd ) const
{
    static const uint8_t aStatus[8] = { 0x90, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0 };
    u1 = std::min( u1, UniqueTicks() );
    if ( u0 >= u1 ) return true;
    const SeekPoint &sp = SeekPointFor( u0 );
    StreamBytes r;
    r.Start( *this, sp.llRaw );
    size_t u = (size_t)sp.llU;
    int64_t llPrev = -1;
    StreamEvent e;
    while ( u < u1 )
    {
        r.Need();
        uint64_t hdr = ReadVar( r.p );
        e.uTrack = (uint32_t)( llPrev + 1 + (int64_t)( hdr >> 1 ) );
        bool bEmit = u >= u0;
        unsigned h;
        do
        {
            r.Need();
            h = *r.p++;
            unsigned op = h >> 5;
            e.cStatus = (uint8_t)( aStatus[op] | ( h & 0x0F ) );
            if ( op <= 2 )
            {
                e.cData2 = op == 2 ? 0 : *r.p++;
                unsigned k;
                do
                {
                    r.Need();
                    k = *r.p++;
                    e.cData1 = (uint8_t)( k & 0x7F );
                    if ( bEmit ) onEvent( u, e );
                } while ( !( k & 0x80 ) );
            }
            else
            {
                e.cData1 = *r.p++;
                e.cData2 = ( op == 5 || op == 6 ) ? 0 : *r.p++;
                if ( bEmit ) onEvent( u, e );
            }
        } while ( !( h & 0x10 ) );
        bool bLast = hdr & 1;
        if ( bEmit && !onGroupEnd( u, e.uTrack, bLast ) ) return false;
        if ( bLast ) { u++; llPrev = -1; }
        else llPrev = e.uTrack;
    }
    return true;
}

template < class F >
void EventStream::ForEachNonNote( size_t u0, size_t u1, F &&fn ) const
{
    static const uint8_t aStatus[8] = { 0x90, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0 };
    u1 = std::min( u1, UniqueTicks() );
    if ( u0 >= u1 ) return;
    const SeekPoint &sp = SeekPointFor( u0 );
    StreamBytes r;
    r.Start( *this, sp.llRaw );
    size_t u = (size_t)sp.llU;
    int64_t llPrev = -1;
    StreamEvent e;
    while ( u < u1 )
    {
        r.Need();
        uint64_t hdr = ReadVar( r.p );
        e.uTrack = (uint32_t)( llPrev + 1 + (int64_t)( hdr >> 1 ) );
        unsigned h;
        do
        {
            r.Need();
            h = *r.p++;
            unsigned op = h >> 5;
            if ( op <= 2 )
            {
                if ( op != 2 ) r.p++;
                // Skip to the key byte with bit 7 set
                for ( ;; )
                {
                    const uint8_t *q = r.p, *qEnd = r.end;
                    while ( q + 8 <= qEnd )
                    {
                        uint64_t w;
                        memcpy( &w, q, 8 );
                        uint64_t m = w & 0x8080808080808080ull;
                        if ( m ) { q += __builtin_ctzll( m ) >> 3; break; }
                        q += 8;
                    }
                    while ( q < qEnd && !( *q & 0x80 ) ) q++;
                    if ( q < qEnd ) { r.p = q + 1; break; }
                    r.p = qEnd;
                    r.Refill();
                    if ( r.p >= r.end ) return;
                }
            }
            else
            {
                e.cStatus = (uint8_t)( aStatus[op] | ( h & 0x0F ) );
                e.cData1 = *r.p++;
                e.cData2 = ( op == 5 || op == 6 ) ? 0 : *r.p++;
                if ( u >= u0 ) fn( u, e );
            }
        } while ( !( h & 0x10 ) );
        if ( hdr & 1 ) { u++; llPrev = -1; }
        else llPrev = e.uTrack;
    }
}

// A controller or program change, for the seek chase (DESIGN.md 7.7), in global order
struct ChaseEvent
{
    int64_t llTime;
    uint32_t uMsg;
};

// Notes passed, NPS and polyphony over song time (DESIGN.md 5.6), counted as fMCG counts them, per
// bin of llBinUs of song time (1 ms; wider for songs over about 70 minutes, at most MaxBins bins):
//   - the NoteOns (velocity > 0) in the bin;
//   - the polyphony at the bin's end: NoteOns minus the NoteOffs that release a held note of their
//     track, channel and key (an off with nothing held is ignored; notes still held when their track
//     ends stay counted).
// Built by the pairing pass from the events it already routes (VisualStore.cpp), when asked for.
// Per-bin counts saturate at 2^32 - 1.
struct NoteStats
{
    static const size_t MaxBins = size_t( 1 ) << 22;
    static const size_t CumShift = 10;          // a running total every 1024 bins
    long long llBinUs = 1000;
    std::vector< uint32_t > vOns, vPoly;
    std::vector< uint64_t > vCum;               // NoteOns before bin i << CumShift

    bool Empty() const { return vOns.empty(); }
    uint64_t Passed( long long llTime ) const;      // NoteOns up to llTime (to the end of its bin)
    uint64_t NPS( long long llTime ) const;         // NoteOns in the second up to llTime
    uint64_t Polyphony( long long llTime ) const;
    size_t MemoryBytes() const { return ( vOns.capacity() + vPoly.capacity() ) * 4 + vCum.capacity() * 8; }
};

// Note-ons (velocity > 0) per velocity in each bin of llBinUs of song time, for the send budget's
// look-ahead (DESIGN.md 7.5), so playback does not read the stream twice. Sparse: per bin, the
// velocities present and their counts. Two sets: every note-on, and only each channel and key's
// first note-on of a tick (what stack collapsing sends).
struct VelocityBins
{
    static const long long BinUs = 10000;
    std::vector< uint64_t > vOffset;        // bins + 1: where each bin's entries start in vData
    std::vector< uint8_t > vData;           // per entry: velocity, then the count (variable length)

    size_t Bins() const { return vOffset.empty() ? 0 : vOffset.size() - 1; }
    // Adds bin b's counts to a (nothing past the last bin)
    void AddTo( size_t b, uint64_t a[128] ) const
    {
        if ( b >= Bins() ) return;
        const uint8_t *q = vData.data() + vOffset[b], *qEnd = vData.data() + vOffset[b + 1];
        while ( q < qEnd )
        {
            unsigned v = *q++;
            a[v] += EventStream::ReadVar( q );
        }
    }
    // Note-ons in bin b
    uint64_t Total( size_t b ) const
    {
        if ( b >= Bins() ) return 0;
        uint64_t n = 0;
        const uint8_t *q = vData.data() + vOffset[b], *qEnd = vData.data() + vOffset[b + 1];
        while ( q < qEnd ) { q++; n += EventStream::ReadVar( q ); }
        return n;
    }
    size_t MemoryBytes() const { return vOffset.capacity() * 8 + vData.capacity(); }
};

struct Song
{
    RefMidiInfo info;                       // llFirstNote is set by the pairing pass
    std::vector< RefTrackInfo > vTracks;
    std::vector< RefMetaEvent > vMeta;      // tempo (length 3) and time signatures (length 4), global order, with times
    std::vector< std::vector< RefMetaEvent > > vTrackMeta; // the same, per track in file order
    std::vector< uint64_t > vTrackLastU;    // per track: unique tick of its last channel event (UINT64_MAX: none)
    uint32_t aKeyNoteCounts[128] = {};      // NoteOns (velocity > 0) per key, for later phases
    EventStream stream;
    std::vector< ChaseEvent > vChase;       // every CC and program change
    VisualStore visual;                     // notes per key, paired (built by BuildVisualStore)
    NoteStats noteStats;                    // for the overlay's extra statistics (built with visual, when asked for)
    VelocityBins aSendOns[2];               // [0] every note-on, [1] as collapsed (BuildSendCounts)
    // "Ignore weird notes": a note re-paired by a later off keeps its first pairing, so the song has no
    // weird notes (DESIGN.md 6.3): one pairing pass, but those notes end earlier than in the original
    bool bFirstPairingOnly = false;
    size_t MemoryBytes() const;
    void BuildChaseIndex( int iThreads );
    void BuildSendCounts( int iThreads );
};
