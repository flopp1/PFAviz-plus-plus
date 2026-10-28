#include "song/Song.h"

#include <algorithm>
#include <windows.h>

#include "core/Lz4.h"
#include "core/ThreadPool.h"

void EventStream::DataFree::operator()( uint8_t *p ) const { VirtualFree( p, 0, MEM_RELEASE ); }

bool EventStream::Reserve()
{
    m_llCommitted = 0;
    data.reset( (uint8_t *)VirtualAlloc( nullptr, (SIZE_T)( llRawBytes + Lz4::Slack ), MEM_RESERVE, PAGE_READWRITE ) );
    return (bool)data;
}

bool EventStream::Commit( uint64_t llEnd )
{
    const uint64_t Page = 4096;
    llEnd = std::min( ( llEnd + Lz4::Slack + Page - 1 ) & ~( Page - 1 ), llRawBytes + Lz4::Slack );
    if ( llEnd <= m_llCommitted ) return true;
    if ( !VirtualAlloc( data.get() + m_llCommitted, (SIZE_T)( llEnd - m_llCommitted ), MEM_COMMIT, PAGE_READWRITE ) ) return false;
    m_llCommitted = llEnd;
    return true;
}

bool EventStream::DecodeChunk( size_t c, uint8_t *pOut ) const
{
    const Chunk &k = vChunks[c];
    size_t iRaw = ChunkRaw( c );
    if ( k.uSize == iRaw ) { memcpy( pOut, data.get() + k.llOffset, iRaw ); return true; }
    return Lz4::Decompress( data.get() + k.llOffset, k.uSize, pOut, iRaw );
}

const EventStream::SeekPoint &EventStream::SeekPointFor( size_t u ) const
{
    auto it = std::upper_bound( vSeek.begin(), vSeek.end(), (uint64_t)u, []( uint64_t v, const SeekPoint &s ) { return v < s.llU; } );
    return it == vSeek.begin() ? vSeek.front() : *( it - 1 );
}

size_t EventStream::MemoryBytes() const
{
    return ticks.MemoryBytes() + times.MemoryBytes() + vChunks.capacity() * sizeof( Chunk ) + vSeek.capacity() * sizeof( SeekPoint ) +
           (size_t)llDataBytes;
}

size_t Song::MemoryBytes() const
{
    size_t n = stream.MemoryBytes() + vTracks.capacity() * sizeof( RefTrackInfo ) + vMeta.capacity() * sizeof( RefMetaEvent ) +
               vChase.capacity() * sizeof( ChaseEvent ) + vTrackLastU.capacity() * 8 + visual.MemoryBytes() + noteStats.MemoryBytes() + ccCounts.MemoryBytes() +
               aSendOns[0].MemoryBytes() + aSendOns[1].MemoryBytes();
    for ( const auto &v : vTrackMeta ) n += v.capacity() * sizeof( RefMetaEvent );
    return n;
}

// One pass over the stream in parallel parts of equal size (whole ticks from a seek point to the next
// part's): note-on runs are counted (their keys checked against the tick's earlier note-ons of the
// channel, for the collapsed counts), all else skipped. A bin split between two parts is summed.
void Song::BuildSendCounts( int iThreads )
{
    const EventStream &es = stream;
    for ( VelocityBins &vb : aSendOns ) { vb.vOffset.clear(); vb.vData.clear(); }
    vChase.clear();
    ccCounts = EventCounts();
    memset( aChaseUsed, 0, sizeof( aChaseUsed ) );
    uChaseItems = 0;
    size_t U = es.UniqueTicks(), S = es.vSeek.size();
    if ( !U || !S ) return;
    const long long B = VelocityBins::BinUs;
    size_t iBins = (size_t)( es.times.Get( U - 1 ) / B ) + 1;
    size_t iParts = std::min< size_t >( S, (size_t)Parallel::ThreadCount( iThreads ) * 8 );
    // A part's bins: from bFirst on, each with its entries in vData from vOffset
    // and its controller and program changes
    struct Part { size_t bFirst = 0; std::vector< uint64_t > vOffset[2]; std::vector< uint8_t > vData[2]; std::vector< ChaseEvent > vChase; std::vector< uint64_t > vCC; };
    std::vector< Part > vParts( iParts );
    auto Encode = []( std::vector< uint8_t > &v, uint64_t a[128] ) {
        for ( int vel = 1; vel < 128; vel++ )
            if ( a[vel] )
            {
                uint8_t buf[12];
                buf[0] = (uint8_t)vel;
                uint8_t *e = EventStream::WriteVar( buf + 1, a[vel] );
                v.insert( v.end(), buf, e );
                a[vel] = 0;
            }
    };
    Parallel::For( iThreads, iParts, [&]( size_t k, int ) {
        static const unsigned aDataBytes[8] = { 0, 0, 0, 2, 2, 1, 1, 2 };
        size_t u0 = k ? (size_t)es.vSeek[S * k / iParts].llU : 0, u1 = k + 1 == iParts ? U : (size_t)es.vSeek[S * ( k + 1 ) / iParts].llU;
        if ( u0 >= u1 ) return;
        Part &part = vParts[k];
        uint64_t aCount[2][128] = {};
        uint64_t llCC = 0;                                  // controller events in the bin
        std::vector< size_t > vSeen( 4096, SIZE_MAX );     // per channel and key: the last tick with a note-on
        size_t b = part.bFirst = (size_t)( es.times.Get( u0 ) / B );
        auto Flush = [&]() {
            for ( int c = 0; c < 2; c++ )
            {
                part.vOffset[c].push_back( part.vData[c].size() );
                Encode( part.vData[c], aCount[c] );
            }
            part.vCC.push_back( llCC );
            llCC = 0;
        };
        StreamBytes r;
        const EventStream::SeekPoint &sp = es.SeekPointFor( u0 );
        r.Start( es, sp.llRaw );
        size_t u = (size_t)sp.llU, uTimed = SIZE_MAX;
        int64_t llTime = 0;
        while ( u < u1 )
        {
            if ( u >= u0 && u != uTimed )
            {
                uTimed = u;
                llTime = (int64_t)es.times.Get( u );
                size_t bu = (size_t)( llTime / B );
                while ( b < bu ) { Flush(); b++; }
            }
            r.Need();
            uint64_t hdr = EventStream::ReadVar( r.p );
            bool bCount = u >= u0;
            unsigned h;
            do
            {
                r.Need();
                h = *r.p++;
                unsigned op = h >> 5;
                if ( op > 2 )
                {
                    if ( op >= 4 && bCount )
                    {
                        // Controller, program, channel pressure, pitch bend: 0xB0, 0xC0, 0xD0, 0xE0
                        uint32_t uMsg = ( 0x70u + ( op << 4 ) ) | ( h & 0x0F ) | ( (uint32_t)r.p[0] << 8 );
                        if ( aDataBytes[op] == 2 ) uMsg |= (uint32_t)r.p[1] << 16;
                        part.vChase.push_back( { llTime, uMsg } );
                        if ( op == 4 ) llCC++;
                    }
                    r.p += aDataBytes[op];
                    continue;
                }
                unsigned v = op == 2 ? 0 : *r.p++;
                if ( op == 0 && ( v & 0x7F ) && ( !( v & 0x80 ) || ( bTuning31 && ( h & 0x0F ) != 9 ) ) && bCount )
                {
                    uint32_t uCh = ( ( h & 0x0F ) << 8 ) | ( v & 0x80 );
                    v &= 0x7F;
                    uint64_t n = 0, nFirst = 0;
                    unsigned key;
                    do
                    {
                        r.Need();
                        key = *r.p++;
                        size_t &t = vSeen[uCh | ( key & 0x7F )];
                        nFirst += t != u;
                        t = u;
                        n++;
                    } while ( !( key & 0x80 ) );
                    aCount[0][v] += n;
                    aCount[1][v] += nFirst;
                    continue;
                }
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
                    if ( r.p >= r.end ) break;
                }
            } while ( !( h & 0x10 ) );
            if ( hdr & 1 ) u++;
        }
        Flush();
    } );
    // The controller counts: parts that share a bin add up in it
    ccCounts = EventCounts();
    ccCounts.vCount.assign( iBins, 0 );
    for ( auto &part : vParts )
    {
        for ( size_t i = 0; i < part.vCC.size() && part.bFirst + i < iBins; i++ )
            ccCounts.vCount[part.bFirst + i] += part.vCC[i];
        std::vector< uint64_t >().swap( part.vCC );
    }
    ccCounts.vCum.resize( ( ( iBins - 1 ) >> EventCounts::CumShift ) + 1 );
    for ( size_t b = 0, llCum = 0; b < iBins; b++ )
    {
        if ( !( b & ( ( size_t( 1 ) << EventCounts::CumShift ) - 1 ) ) ) ccCounts.vCum[b >> EventCounts::CumShift] = llCum;
        llCum += ccCounts.vCount[b];
    }
    size_t iChase = 0;
    for ( auto &part : vParts ) iChase += part.vChase.size();
    vChase.reserve( iChase );
    for ( auto &part : vParts ) { vChase.insert( vChase.end(), part.vChase.begin(), part.vChase.end() ); std::vector< ChaseEvent >().swap( part.vChase ); }
    for ( const ChaseEvent &e : vChase )
    {
        int i = ChaseItem( e.uMsg );
        uint8_t &b = aChaseUsed[e.uMsg & 0x0F][i < 0 ? 0 : i];
        if ( i >= 0 && !b ) { b = 1; uChaseItems++; }
    }
    for ( int c = 0; c < 2; c++ )
    {
        VelocityBins &vb = aSendOns[c];
        size_t n = 0;
        for ( auto &part : vParts ) n += part.vData[c].size();
        vb.vData.reserve( n );
        vb.vOffset.reserve( iBins + 1 );
        for ( auto &part : vParts )
        {
            size_t iSkip = 0;
            if ( !part.vOffset[c].empty() && part.bFirst + 1 == vb.vOffset.size() )
            {
                // The part starts in the last bin so far: that bin's counts are summed and written again
                uint64_t a[128] = {};
                VelocityBins one;
                one.vOffset = { 0, vb.vData.size() - vb.vOffset.back() };
                one.vData.assign( vb.vData.begin() + vb.vOffset.back(), vb.vData.end() );
                one.AddTo( 0, a );
                one.vOffset = { 0, ( part.vOffset[c].size() > 1 ? part.vOffset[c][1] : part.vData[c].size() ) };
                one.vData.assign( part.vData[c].begin(), part.vData[c].begin() + one.vOffset[1] );
                one.AddTo( 0, a );
                vb.vData.resize( vb.vOffset.back() );
                Encode( vb.vData, a );
                iSkip = 1;
            }
            while ( !part.vOffset[c].empty() && vb.vOffset.size() < part.bFirst ) vb.vOffset.push_back( vb.vData.size() );   // bins with nothing
            size_t iBase = vb.vData.size() - ( iSkip && part.vOffset[c].size() > 1 ? part.vOffset[c][1] : iSkip ? part.vData[c].size() : 0 );
            for ( size_t i = iSkip; i < part.vOffset[c].size(); i++ ) vb.vOffset.push_back( iBase + part.vOffset[c][i] );
            size_t iFrom = iSkip ? ( part.vOffset[c].size() > 1 ? part.vOffset[c][1] : part.vData[c].size() ) : 0;
            vb.vData.insert( vb.vData.end(), part.vData[c].begin() + iFrom, part.vData[c].end() );
            std::vector< uint64_t >().swap( part.vOffset[c] );
            std::vector< uint8_t >().swap( part.vData[c] );
        }
        while ( vb.vOffset.size() < iBins ) vb.vOffset.push_back( vb.vData.size() );
        vb.vOffset.push_back( vb.vData.size() );
    }
}

uint64_t EventCounts::Passed( long long llTime ) const
{
    if ( vCount.empty() || llTime < 0 ) return 0;
    size_t b = std::min< size_t >( (size_t)( llTime / BinUs ), vCount.size() - 1 );
    uint64_t n = vCum[b >> CumShift];
    for ( size_t i = ( b >> CumShift ) << CumShift; i <= b; i++ ) n += vCount[i];
    return n;
}

uint64_t NoteStats::Passed( long long llTime ) const
{
    if ( vOns.empty() || llTime < 0 ) return 0;
    size_t b = std::min< size_t >( (size_t)( llTime / llBinUs ), vOns.size() - 1 );
    uint64_t n = vCum[b >> CumShift];
    for ( size_t i = ( b >> CumShift ) << CumShift; i <= b; i++ ) n += vOns[i];
    return n;
}

uint64_t NoteStats::NPS( long long llTime ) const
{
    if ( vOns.empty() || llTime < 0 ) return 0;
    long long b = llTime / llBinUs;
    long long lo = std::max< long long >( 0, b - std::max< long long >( 1, 1000000 / llBinUs ) + 1 );
    long long hi = std::min< long long >( b, (long long)vOns.size() - 1 );
    uint64_t n = 0;
    for ( long long i = lo; i <= hi; i++ ) n += vOns[i];
    return n;
}

uint64_t NoteStats::Polyphony( long long llTime ) const
{
    if ( vPoly.empty() || llTime < 0 ) return 0;
    return vPoly[std::min< size_t >( (size_t)( llTime / llBinUs ), vPoly.size() - 1 )];
}

