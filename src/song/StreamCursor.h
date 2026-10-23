// Resumable reader over the event stream: returns events one at a time in global order and can stop
// anywhere, including inside a run of a tick with millions of events. Chunks are decoded as it
// moves on (Song.h).
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "song/Song.h"

class StreamCursor
{
public:
    void Attach( const EventStream *pStream ) { m_p = pStream; Seek( 0 ); }

    // Positions the cursor at the first event of unique tick u (or the end)
    void Seek( size_t u )
    {
        const EventStream &es = *m_p;
        size_t U = es.UniqueTicks();
        if ( u > U ) u = U;
        m_bInGroup = m_bInRun = false;
        m_llPrev = -1;
        m_bStop = m_bStopped = false;
        if ( !U ) { m_u = 0; return; }
        const EventStream::SeekPoint &sp = es.SeekPointFor( u );
        m_bytes.Start( es, sp.llRaw );
        m_u = (size_t)sp.llU;
        uint32_t uTrack, uMsg;
        while ( Next( uTrack, uMsg, u ) ) {}   // skip to the start of tick u
    }

    bool AtEnd() const { return !m_bInGroup && m_u >= m_p->UniqueTicks(); }

    // Unique tick of the next event (UniqueTicks() at the end)
    size_t NextTick() const { return m_u; }
    long long NextTime() const
    {
        return m_u < m_p->UniqueTicks() ? (long long)m_p->times.Get( m_u ) : LLONG_MAX;
    }

    // Next event whose unique tick is below uEnd; false at uEnd or the end of the song.
    // uMsg is a short message: status | data1 << 8 | data2 << 16.
    inline bool Next( uint32_t &uTrack, uint32_t &uMsg, size_t uEnd = SIZE_MAX )
    {
        static const uint8_t aStatus[8] = { 0x90, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0 };
        StreamBytes &r = m_bytes;
        for ( ;; )
        {
            if ( m_bInRun )
            {
                r.Need();
                unsigned k = *r.p++;
                uTrack = m_uTrack;
                uMsg = m_uRunBase | ( ( k & 0x7F ) << 8 );
                if ( k & 0x80 )
                {
                    m_bInRun = false;
                    if ( m_bLastRun ) EndGroup();
                }
                return true;
            }
            if ( m_bInGroup )
            {
                r.Need();
                unsigned h = *r.p++;
                unsigned op = h >> 5;
                unsigned uStatus = aStatus[op] | ( h & 0x0F );
                m_bLastRun = ( h & 0x10 ) != 0;
                if ( op <= 2 )
                {
                    unsigned d2 = op == 2 ? 0 : *r.p++;
                    m_uRunBase = uStatus | ( d2 << 16 );
                    m_bInRun = true;
                    continue;
                }
                unsigned d1 = *r.p++;
                unsigned d2 = ( op == 5 || op == 6 ) ? 0 : *r.p++;
                uTrack = m_uTrack;
                uMsg = uStatus | ( d1 << 8 ) | ( d2 << 16 );
                if ( m_bLastRun ) EndGroup();
                return true;
            }
            // Next group
            if ( m_u >= m_p->UniqueTicks() || m_u >= uEnd ) return false;
            if ( __builtin_expect( m_bStop, 0 ) && ( m_u >= m_uStopTick || r.Pos() >= m_llStopRaw ) ) { m_bStopped = true; return false; }
            r.Need();
            uint64_t hdr = EventStream::ReadVar( r.p );
            m_uTrack = (uint32_t)( m_llPrev + 1 + (int64_t)( hdr >> 1 ) );
            m_bLastGroup = hdr & 1;
            m_bInGroup = true;
        }
    }

    // Offset in the raw stream of the next byte to read (at a group's start between groups)
    uint64_t RawPos() const { return m_bytes.Pos(); }

    // A stop for parallel decoding (PlaybackDecoder lanes): Next returns false, and Stopped() is true,
    // at the first group that starts at or after llRaw, or at tick uTick. Seek clears it.
    void SetStop( uint64_t llRaw, size_t uTick ) { m_bStop = true; m_bStopped = false; m_llStopRaw = llRaw; m_uStopTick = uTick; }
    bool Stopped() const { return m_bStopped; }

    // Positions the cursor at the first group that starts at or after llTarget (or at tick uLimit),
    // reading from tick uFrom, which starts at llFromRaw; whole groups are skipped without decoding
    void SeekGroup( uint64_t llFromRaw, size_t uFrom, uint64_t llTarget, size_t uLimit )
    {
        static const unsigned aDataBytes[8] = { 0, 0, 0, 2, 2, 1, 1, 2 };
        const EventStream &es = *m_p;
        size_t U = es.UniqueTicks();
        m_bInGroup = m_bInRun = false;
        m_bStop = m_bStopped = false;
        m_llPrev = -1;
        m_u = std::min( uFrom, U );
        if ( m_u >= U ) return;
        StreamBytes &r = m_bytes;
        r.Start( es, llFromRaw );
        uLimit = std::min( uLimit, U );
        while ( m_u < uLimit )
        {
            r.Need();
            if ( r.Pos() >= llTarget ) return;
            uint64_t hdr = EventStream::ReadVar( r.p );
            uint32_t uTrack = (uint32_t)( m_llPrev + 1 + (int64_t)( hdr >> 1 ) );
            unsigned h;
            do
            {
                r.Need();
                h = *r.p++;
                unsigned op = h >> 5;
                if ( op > 2 ) { r.p += aDataBytes[op]; continue; }
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
                    if ( r.p >= r.end ) { m_u = U; return; }
                }
            } while ( !( h & 0x10 ) );
            if ( hdr & 1 ) { m_u++; m_llPrev = -1; }
            else m_llPrev = uTrack;
        }
    }

    // Note runs taken whole (PlaybackDecoder::Fill): after Next returned a note, InRun says more keys
    // of its run follow. RunKeys gives the run's track and message without the key, and the key bytes
    // readable now (the run ends at the first with bit 7 set); TakeKeys moves past n of them, bEnd
    // when the last of those was the run's last.
    bool InRun() const { return m_bInRun; }
    const uint8_t *RunKeys( size_t &iAvail, uint32_t &uTrack, uint32_t &uBase )
    {
        m_bytes.Need();
        iAvail = (size_t)( m_bytes.end - m_bytes.p );
        uTrack = m_uTrack;
        uBase = m_uRunBase;
        return m_bytes.p;
    }
    void TakeKeys( size_t n, bool bEnd )
    {
        m_bytes.p += n;
        if ( bEnd )
        {
            m_bInRun = false;
            if ( m_bLastRun ) EndGroup();
        }
    }

private:
    void EndGroup()
    {
        m_bInGroup = false;
        if ( m_bLastGroup ) { m_u++; m_llPrev = -1; }
        else m_llPrev = m_uTrack;
    }

    const EventStream *m_p = nullptr;
    StreamBytes m_bytes;
    size_t m_u = 0;                 // tick of the next event
    int64_t m_llPrev = -1;          // previous group's track in this tick
    uint32_t m_uTrack = 0, m_uRunBase = 0;
    bool m_bInGroup = false, m_bInRun = false, m_bLastRun = false, m_bLastGroup = false;
    bool m_bStop = false, m_bStopped = false;
    uint64_t m_llStopRaw = UINT64_MAX;
    size_t m_uStopTick = SIZE_MAX;
};
