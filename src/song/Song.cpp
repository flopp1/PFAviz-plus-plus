#include "song/Song.h"

#include <algorithm>

#include "core/Lz4.h"
#include "core/ThreadPool.h"

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
               vChase.capacity() * sizeof( ChaseEvent ) + vTrackLastU.capacity() * 8 + visual.MemoryBytes() + noteStats.MemoryBytes();
    for ( const auto &v : vTrackMeta ) n += v.capacity() * sizeof( RefMetaEvent );
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

void Song::BuildChaseIndex( int iThreads )
{
    const EventStream &es = stream;
    size_t U = es.UniqueTicks();
    size_t iParts = U ? std::min< size_t >( U, (size_t)Parallel::ThreadCount( iThreads ) * 8 ) : 0;
    std::vector< std::vector< ChaseEvent > > vParts( iParts );
    Parallel::For( iThreads, iParts, [&]( size_t k, int ) {
        size_t u0 = U * k / iParts, u1 = U * ( k + 1 ) / iParts;
        std::vector< ChaseEvent > &v = vParts[k];
        size_t uTime = SIZE_MAX;
        int64_t llTime = 0;
        es.ForEachNonNote( u0, u1, [&]( size_t u, const StreamEvent &e ) {
            unsigned t = e.cStatus >> 4;
            if ( t != 0xB && t != 0xC ) return;
            if ( u != uTime ) { uTime = u; llTime = (int64_t)es.times.Get( u ); }
            uint32_t uMsg = e.cStatus | ( (uint32_t)e.cData1 << 8 );
            if ( t == 0xB ) uMsg |= (uint32_t)e.cData2 << 16;
            v.push_back( { llTime, uMsg } );
        } );
    } );
    size_t n = 0;
    for ( auto &v : vParts ) n += v.size();
    vChase.clear();
    vChase.reserve( n );
    for ( auto &v : vParts ) vChase.insert( vChase.end(), v.begin(), v.end() );
}
