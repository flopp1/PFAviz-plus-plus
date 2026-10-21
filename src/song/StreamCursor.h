// Resumable reader over the event stream: returns events one at a time in global order and can stop
// anywhere, including inside a run of a tick with millions of events. Chunks are decoded as it
// moves on (Song.h).
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>

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
            r.Need();
            uint64_t hdr = EventStream::ReadVar( r.p );
            m_uTrack = (uint32_t)( m_llPrev + 1 + (int64_t)( hdr >> 1 ) );
            m_bLastGroup = hdr & 1;
            m_bInGroup = true;
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
};
