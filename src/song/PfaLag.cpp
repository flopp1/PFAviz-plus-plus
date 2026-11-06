#include "song/PfaLag.h"

#include <windows.h>
#include <cpuid.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <vector>

#include "song/Song.h"

namespace
{
    // The working set per note: a sounding note's event object (96 bytes on the heap) and its line of
    // the event pointer array; an event in view or played, its object and pointer
    const double SoundingBytes = 160.0, EventBytes = 104.0;

    // Costs measured from stock PFA 1.1.0 (its own VS2010 build, with the usual winmm.dll patch that
    // sends to KDMAPI) on a Ryzen 5 5500 (L1 32 KB, L2 512 KB, L3 16 MB): its frames logged by a d3d9
    // proxy over seventeen black MIDIs (up to 184,000 polyphony), from 2,500 fps down to 216 s frames,
    // fitted by least squares with the walks counted per millisecond, each at the polyphony it walks
    // (each stretch weighted by its length, so crash frames count); checked against synthetic slams
    // (polyphony grown, held, renewed and released at set rates). Each cost is given at five working-set sizes, the caches' and
    // past them (L1, L2, L3, 4 x L3, 32 x L3), and taken between them in proportion to log2 of the size,
    // so other cache sizes move the curves with them. (Without the patch, WinMM's 13-28 us a message on
    // Windows 11 also flushed the caches between events, and every event and walk cost several times
    // more.)
    const double LogicBaseUs = 7.60;
    // An event played (its loops, its state; PFA reads them in order, so the memory streams); 44 ns
    // in all, of which its two passes without sends (ViewInNs, ReplayNs) take 20
    const double EventNs[5] = { 24.2, 24.2, 24.2, 24.2, 24.2 };
    // A sounding note, per note-off, by the sounding notes' size; and more by the frame's events, which
    // evict the notes as they stream past
    const double WalkNs[5] = { 1.177, 1.177, 2.028, 7.365, 7.365 };
    const double WalkEvictNs[5] = { 0.0, 0.0, 0.0, 0.305, 0.469 };
    const double RenderBaseUs = 393.3;
    // Render walks a sounding note and an event in view (by the two together's size): a sounding note
    // costs far more once they outgrow L2 (it is drawn over the whole view, from the list of sounding
    // notes); and each note drawn
    const double RenderSoundingNs[5] = { 0.0, 0.0, 281.4, 281.4, 281.4 };
    const double RenderViewNs[5] = { 31.7, 27.0, 48.7, 155.9, 155.9 };
    const double DrawnNs = 44.7;
    // Logic's passes without sends, per event (by the events' size; measured from when stock PFA sent
    // its first and last message of each frame): the events coming into view, read before the frame's
    // sends; the events played, read again after them
    const double ViewInNs[5] = { 14.0, 14.0, 14.0, 14.0, 14.0 };
    const double ReplayNs[5] = { 6.0, 6.0, 6.0, 6.0, 6.0 };

    // The noise (PfaLag::Noise): other programs take a share of L2 / L3 that wanders (mean, spread,
    // how fast it moves); a frame's CPU work spreads by a lognormal factor, less for long frames; a
    // message's send cost spreads (per message, and a slow state of the send path), with rare spikes
    // (the scheduler, the driver's locks); and the odd stall of the whole frame
    const double ContentionMean = 0.15, ContentionSd = 0.08, ContentionTauUs = 4e6, ContentionMax = 0.6;
    const double SendStateSd = 0.2, SendStateTauUs = 1.5e6;
    const double SendMsgSd = 0.36;                  // a message's relative spread (lognormal sigma 0.35)
    const double SendSpikeEvery = 4000.0, SendSpikeMult = 40.0;
    const double WorkSd = 0.08, WorkSdFloor = 0.02, WorkSdMs = 4.0;
    const double StallEveryUs = 300000.0, StallUs = 1500.0;

    void Hats( const PfaLag::Cpu &cpu, double dBytes, double h[5] )
    {
        const double ax[5] = { (double)cpu.llL1, (double)cpu.llL2, (double)cpu.llL3, 4.0 * cpu.llL3, 32.0 * cpu.llL3 };
        for ( int i = 0; i < 5; i++ ) h[i] = 0.0;
        if ( dBytes <= ax[0] ) { h[0] = 1.0; return; }
        for ( int i = 1; i < 5; i++ )
            if ( dBytes <= ax[i] )
            {
                double f = std::log2( dBytes / ax[i - 1] ) / std::log2( ax[i] / ax[i - 1] );
                h[i - 1] = 1.0 - f;
                h[i] = f;
                return;
            }
        h[4] = 1.0;
    }

    // A cost at that working-set size: of the walks (bMem: scaled by this CPU's speed at each size),
    // or of the other work (by its core's)
    double AtSize( const PfaLag::Cpu &cpu, const double ay[5], double dBytes, bool bMem = true )
    {
        double h[5];
        Hats( cpu, dBytes, h );
        double v = 0.0;
        for ( int i = 0; i < 5; i++ ) v += h[i] * ay[i] * ( bMem ? cpu.aMem[i] : cpu.dCore );
        return v;
    }

    // A piece of song time within one bin of the note statistics and of the CC counts: its share of
    // the bin's note-ons, note-offs (the note-ons less what the polyphony grew by) and other events,
    // and the polyphony at its middle and its ends (inside a bin, taken along its events: a tick that
    // ends a hundred thousand notes is a bin whose polyphony falls as its offs go)
    struct Piece
    {
        long long llStart, llEnd;
        double dOns, dOffs, dOthers, dPoly, dPolyA, dPolyB;
    };

    template< class F > void ForPieces( const Song &song, long long llFrom, long long llTo, F &&f )
    {
        const NoteStats &ns = song.noteStats;
        const EventCounts &cc = song.ccCounts;
        if ( ns.Empty() || llTo <= llFrom ) return;
        const long long W = ns.llBinUs, CW = EventCounts::BinUs;
        const long long nb = (long long)ns.vOns.size(), nc = (long long)cc.vCount.size();
        const long long tEnd = std::min( llTo, std::max( nb * W, nc * CW ) );
        for ( long long t = std::max< long long >( llFrom, 0 ); t < tEnd; )
        {
            const long long b = t / W, c = t / CW;
            const long long t1 = std::min( { tEnd, ( b + 1 ) * W, ( c + 1 ) * CW } );
            Piece q = { t, t1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            if ( b < nb )
            {
                const double dOn = (double)ns.vOns[b];
                const double p0 = b > 0 ? (double)ns.vPoly[b - 1] : 0.0, p1 = (double)ns.vPoly[b];
                const double fr = (double)( t1 - t ) / W;
                q.dOns = dOn * fr;
                q.dOffs = std::max( 0.0, dOn - ( p1 - p0 ) ) * fr;
                q.dPolyA = p0 + ( p1 - p0 ) * (double)( t - b * W ) / W;
                q.dPolyB = p0 + ( p1 - p0 ) * (double)( t1 - b * W ) / W;
                q.dPoly = ( q.dPolyA + q.dPolyB ) * 0.5;
            }
            if ( c < nc ) q.dOthers = (double)cc.vCount[c] * (double)( t1 - t ) / CW;
            // Where the polyphony moves a lot inside the piece (a mass release, a slam), in parts, so
            // each walk is costed at the size it has: a release's walks are mostly of the notes still
            // many, a working set the caches may not hold
            const double dMove = std::abs( q.dPolyB - q.dPolyA ), dMax = std::max( q.dPolyA, q.dPolyB );
            const int nParts = q.dOffs > 0.0 && dMove > 0.08 * dMax ? std::min( 16, (int)std::ceil( dMove / ( 0.08 * dMax ) ) ) : 1;
            if ( nParts == 1 || q.llEnd - q.llStart < nParts ) f( q );
            else
                for ( int i = 0; i < nParts; i++ )
                {
                    Piece s = q;
                    s.llStart = q.llStart + ( q.llEnd - q.llStart ) * i / nParts;
                    s.llEnd = q.llStart + ( q.llEnd - q.llStart ) * ( i + 1 ) / nParts;
                    const double a = (double)( s.llStart - q.llStart ) / ( q.llEnd - q.llStart ), b = (double)( s.llEnd - q.llStart ) / ( q.llEnd - q.llStart );
                    s.dOns = q.dOns * ( b - a );
                    s.dOffs = q.dOffs * ( b - a );
                    s.dOthers = q.dOthers * ( b - a );
                    s.dPolyA = q.dPolyA + ( q.dPolyB - q.dPolyA ) * a;
                    s.dPolyB = q.dPolyA + ( q.dPolyB - q.dPolyA ) * b;
                    s.dPoly = ( s.dPolyA + s.dPolyB ) * 0.5;
                    f( s );
                }
            t = t1;
        }
    }

    double WalkUs( const PfaLag::Cpu &cpu, const Piece &q )
    {
        return q.dOffs > 0.0 ? q.dOffs * q.dPoly * AtSize( cpu, WalkNs, q.dPoly * SoundingBytes ) / 1000.0 : 0.0;
    }
}

PfaLag::Cpu PfaLag::Cpu::Detect()
{
    Cpu c;
    DWORD dwLen = 0;
    GetLogicalProcessorInformationEx( RelationCache, nullptr, &dwLen );
    if ( !dwLen ) return c;
    std::vector< uint8_t > v( dwLen );
    auto *p = reinterpret_cast< SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX * >( v.data() );
    if ( !GetLogicalProcessorInformationEx( RelationCache, p, &dwLen ) ) return c;
    uint64_t a[4] = {};
    for ( DWORD dwOff = 0; dwOff < dwLen; )
    {
        auto *e = reinterpret_cast< SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX * >( v.data() + dwOff );
        if ( e->Relationship == RelationCache && e->Cache.Level >= 1 && e->Cache.Level <= 3 &&
             ( e->Cache.Type == CacheData || e->Cache.Type == CacheUnified ) )
            a[e->Cache.Level] = std::max< uint64_t >( a[e->Cache.Level], e->Cache.CacheSize );
        if ( !e->Size ) break;
        dwOff += e->Size;
    }
    if ( a[1] ) c.llL1 = a[1];
    if ( a[2] ) c.llL2 = a[2];
    if ( a[3] ) c.llL3 = a[3];
    // (no L3: memory comes after L2)
    else if ( a[2] ) c.llL3 = a[2] * 2;
    return c;
}

void PfaLag::Cpu::Faster( double dTimes )
{
    if ( !( dTimes > 0.0 ) ) return;
    for ( double &d : aMem ) d /= dTimes;
    dCore /= dTimes;
}

namespace
{
    // The benchmark measured on the reference CPU (the costs' own: a Ryzen 5 5500), and its name
    const char *const RefBrand = "AMD Ryzen 5 5500";
    const double RefWalkNs[4] = { 0.657, 0.994, 1.268, 5.49 };
    const double RefEventNs = 15.98;

    // An event object as the original's (MIDIChannelEvent, 96 bytes on the heap)
    struct BenchEv
    {
        long long llTime;
        BenchEv *pSister;
        int iTrack;
        unsigned char uType, uChannel, uKey, uVelocity;
        char aPad[96 - 24];
    };

    double Seconds()
    {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter( &t );
        QueryPerformanceFrequency( &f );
        return (double)t.QuadPart / (double)f.QuadPart;
    }

    // The original's note-off walk over nNotes sounding notes (UpdateState): each a slot of m_vState,
    // an index into m_vEvents (eight entries apart, the other events between) whose pointer goes to a
    // scattered object; the walk compares the pointer and reads the key. ns per note walked
    double BenchWalkNs( size_t nNotes, std::mt19937_64 &rng, volatile size_t &sink )
    {
        nNotes = std::max< size_t >( nNotes, 16 );
        std::vector< BenchEv > vObj( nNotes );
        std::vector< size_t > vOrder( nNotes );
        std::iota( vOrder.begin(), vOrder.end(), 0 );
        std::shuffle( vOrder.begin(), vOrder.end(), rng );
        std::vector< BenchEv * > vEvents( nNotes * 8, &vObj[0] );
        std::vector< int > vState( nNotes );
        for ( size_t i = 0; i < nNotes; i++ )
        {
            BenchEv &e = vObj[vOrder[i]];
            e.uKey = (unsigned char)( rng() & 127 );
            e.uType = 1;
            vState[i] = (int)( i * 8 + ( rng() & 7 ) );
            vEvents[vState[i]] = &e;
        }
        // Enough walks for about 12 M notes (at least 3), the first not timed (it brings them in)
        const size_t nWalks = std::max< size_t >( 3, 12000000 / nNotes ) + 1;
        size_t llHits = 0;
        double t0 = 0.0;
        for ( size_t w = 0; w < nWalks; w++ )
        {
            if ( w == 1 ) t0 = Seconds();
            const BenchEv *pSearch = vEvents[vState[rng() % nNotes]];
            const unsigned uKey = pSearch->uKey;
            int iLast = -1;
            for ( int i : vState )
            {
                const BenchEv *p = vEvents[i];
                if ( p == pSearch ) llHits++;
                else if ( p->uKey == uKey ) iLast = i;
            }
            llHits += (size_t)( iLast & 1 );
        }
        const double dNs = ( Seconds() - t0 ) * 1e9 / ( (double)( nWalks - 1 ) * nNotes );
        sink = sink + llHits;
        return dNs;
    }

    // The original's per-event work, as the core does it: each event's fields read, a chain of
    // dependent arithmetic (its clock and latency) and a branch on its unpredictable type, its state
    // updated, over events that stay in L1 and L2 (heavier than a bare loop, so the code's placement
    // does not move it): ns per event
    double BenchEventNs( std::mt19937_64 &rng, volatile size_t &sink )
    {
        const size_t N = 2048;
        std::vector< BenchEv > vObj( N );
        std::vector< BenchEv * > vEvents( N );
        for ( size_t i = 0; i < N; i++ )
        {
            vObj[i].uType = (unsigned char)( rng() % 3 );
            vObj[i].uKey = (unsigned char)( rng() & 127 );
            vObj[i].uVelocity = (unsigned char)( rng() & 127 );
            vObj[i].llTime = (long long)( rng() >> 20 );
            vEvents[i] = &vObj[i];
        }
        int aNoteState[128];
        std::fill( aNoteState, aNoteState + 128, -1 );
        uint64_t x = 0x9E3779B97F4A7C15ull, llOut = 0;
        const int nPasses = 400;
        double t0 = 0.0;
        for ( int r = 0; r <= nPasses; r++ )
        {
            if ( r == 1 ) t0 = Seconds();
            for ( size_t i = 0; i < N; i++ )
            {
                const BenchEv *p = vEvents[i];
                x ^= (uint64_t)p->llTime + p->uKey;
                for ( int k = 0; k < 8; k++ ) x = ( x ^ ( x >> 29 ) ) * 0xBF58476D1CE4E5B9ull;
                switch ( ( p->uType + x ) % 3 )
                {
                case 0: aNoteState[p->uKey] = (int)i; llOut += p->uVelocity; break;
                case 1: llOut += (uint64_t)aNoteState[p->uKey] ^ x; aNoteState[p->uKey] = -1; break;
                default: llOut += x >> 60; break;
                }
            }
        }
        const double dNs = ( Seconds() - t0 ) * 1e9 / ( (double)nPasses * N );
        sink = sink + llOut + (size_t)aNoteState[5];
        return dNs;
    }
}

const PfaLag::Bench &PfaLag::RunBench()
{
    static Bench s_b;
    static std::once_flag s_once;
    std::call_once( s_once, []() {
        unsigned r[12] = {};
        if ( __get_cpuid( 0x80000002, &r[0], &r[1], &r[2], &r[3] ) && __get_cpuid( 0x80000003, &r[4], &r[5], &r[6], &r[7] ) &&
             __get_cpuid( 0x80000004, &r[8], &r[9], &r[10], &r[11] ) )
        {
            memcpy( s_b.sBrand, r, 48 );
            // (trimmed of the padding spaces)
            char *p = s_b.sBrand + strlen( s_b.sBrand );
            while ( p > s_b.sBrand && p[-1] == ' ' ) *--p = 0;
            const char *q = s_b.sBrand;
            while ( *q == ' ' ) q++;
            memmove( s_b.sBrand, q, strlen( q ) + 1 );
        }
        s_b.bReference = strncmp( s_b.sBrand, RefBrand, strlen( RefBrand ) ) == 0 &&
                         ( s_b.sBrand[strlen( RefBrand )] == 0 || s_b.sBrand[strlen( RefBrand )] == ' ' );
        const Cpu cpu = Cpu::Detect();
        // (the working set of a note: its slot, its pointer's line and its object)
        const double dBytes[4] = { cpu.llL1 / 2.0, cpu.llL2 / 2.0, cpu.llL3 / 2.0, std::min( 4.0 * cpu.llL3, 256.0 * 1048576.0 ) };
        std::mt19937_64 rng( 12345 );
        volatile size_t sink = 0;
        // The quickest of two runs of each
        for ( int k = 0; k < 4; k++ )
        {
            const size_t n = (size_t)( dBytes[k] / SoundingBytes );
            s_b.aWalkNs[k] = std::min( BenchWalkNs( n, rng, sink ), BenchWalkNs( n, rng, sink ) );
        }
        s_b.dEventNs = std::min( BenchEventNs( rng, sink ), BenchEventNs( rng, sink ) );
    } );
    return s_b;
}

void PfaLag::ApplyBench( Cpu &cpu, const Bench &b )
{
    if ( b.bReference || RefEventNs <= 0.0 ) return;
    for ( int k = 0; k < 5; k++ )
    {
        const int j = std::min( k, 3 );
        if ( RefWalkNs[j] > 0.0 && b.aWalkNs[j] > 0.0 ) cpu.aMem[k] *= std::clamp( b.aWalkNs[j] / RefWalkNs[j], 0.2, 5.0 );
    }
    if ( b.dEventNs > 0.0 ) cpu.dCore *= std::clamp( b.dEventNs / RefEventNs, 0.2, 5.0 );
}

PfaLag::Noise::Noise( uint64_t llSeed ) : m_llState( llSeed ? llSeed : 1 ), m_dContention( ContentionMean ), m_dSendLog( 0.0 )
{
    // (start the slow processes somewhere in their range)
    m_dContention = std::clamp( ContentionMean + ContentionSd * Normal(), 0.0, ContentionMax );
    m_dSendLog = SendStateSd * Normal();
}

double PfaLag::Noise::Uniform()
{
    // splitmix64
    uint64_t z = ( m_llState += 0x9E3779B97F4A7C15ull );
    z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
    z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return ( ( z >> 11 ) + 0.5 ) * ( 1.0 / 9007199254740992.0 );
}

double PfaLag::Noise::Normal()
{
    return std::sqrt( -2.0 * std::log( Uniform() ) ) * std::cos( 6.283185307179586 * Uniform() );
}

double PfaLag::Noise::Exponential( double dMean )
{
    return -dMean * std::log( Uniform() );
}

int PfaLag::Noise::Poisson( double dMean )
{
    if ( dMean <= 0.0 ) return 0;
    if ( dMean > 30.0 ) return std::max( 0, (int)std::lround( dMean + std::sqrt( dMean ) * Normal() ) );
    const double l = std::exp( -dMean );
    int k = 0;
    for ( double p = Uniform(); p > l; p *= Uniform() ) k++;
    return k;
}

void PfaLag::Noise::Advance( double dWallUs )
{
    if ( dWallUs <= 0.0 ) return;
    // Ornstein-Uhlenbeck steps: each pulled back to its mean at its own rate, with fresh noise
    double a = std::exp( -dWallUs / ContentionTauUs );
    m_dContention = std::clamp( ContentionMean + ( m_dContention - ContentionMean ) * a + ContentionSd * std::sqrt( 1.0 - a * a ) * Normal(), 0.0, ContentionMax );
    a = std::exp( -dWallUs / SendStateTauUs );
    m_dSendLog = m_dSendLog * a + SendStateSd * std::sqrt( 1.0 - a * a ) * Normal();
}

bool PfaLag::Ready( const Song &song )
{
    return !song.noteStats.Empty();
}

void PfaLag::WalkBands( const Song &song, const Cpu &cpu, long long llFrom, long long llTo, double aBands[5], double *pEvents )
{
    for ( int i = 0; i < 5; i++ ) aBands[i] = 0.0;
    double dEvents = 0.0;
    ForPieces( song, llFrom, llTo, [&]( const Piece &q ) {
        dEvents += q.dOns + q.dOffs + q.dOthers;
        if ( q.dOffs <= 0.0 ) return;
        double h[5];
        Hats( cpu, q.dPoly * SoundingBytes, h );
        for ( int i = 0; i < 5; i++ ) aBands[i] += h[i] * q.dOffs * q.dPoly;
    } );
    if ( pEvents ) *pEvents = dEvents;
}

PfaLag::FrameCost PfaLag::Cost( const Song &song, const Cpu &cpuIn, const Params &p, long long llFrom, long long llTo, Noise *pNoise )
{
    FrameCost f;
    const NoteStats &ns = song.noteStats;
    if ( ns.Empty() ) return f;
    const EventCounts &cc = song.ccCounts;

    // Other programs' share of the caches
    Cpu cpu = cpuIn;
    if ( pNoise )
    {
        const double c = pNoise->Contention();
        cpu.llL2 = std::max< uint64_t >( cpu.llL1 + 1, (uint64_t)( cpu.llL2 * ( 1.0 - 0.5 * c ) ) );
        cpu.llL3 = std::max< uint64_t >( cpu.llL2 + 1, (uint64_t)( cpu.llL3 * ( 1.0 - c ) ) );
    }
    // A frame's CPU work: a lognormal factor of mean 1, narrower the more work there is
    auto Spread = [&]( double dUs ) {
        if ( !pNoise ) return 1.0;
        const double ms = dUs / 1000.0;
        const double s = std::sqrt( WorkSd * WorkSd / ( 1.0 + ms / WorkSdMs ) + WorkSdFloor * WorkSdFloor );
        return std::exp( s * pNoise->Normal() - s * s / 2.0 );
    };

    // Events in song time (a, b]: note-ons, note-offs (from the polyphony) and the rest
    auto Ons = [&]( long long a, long long b ) { return b > a ? (double)( ns.Passed( b ) - ns.Passed( a ) ) : 0.0; };
    auto Others = [&]( long long a, long long b ) { return b > a && !cc.Empty() ? (double)( cc.Passed( b ) - cc.Passed( a ) ) : 0.0; };
    auto Events = [&]( long long a, long long b ) {
        if ( b <= a ) return 0.0;
        const double dOn = Ons( a, b ), pa = (double)ns.Polyphony( a ), pb = (double)ns.Polyphony( b );
        return dOn + std::clamp( dOn - ( pb - pa ), 0.0, dOn + pa ) + Others( a, b );
    };

    // Logic, in three passes over the events: the ones coming into view (its end position, before it
    // sends anything); then each event played, sent (unless a note-on while muted), each note-off
    // walking the notes sounding at its time; then the ones played again (its input and learning
    // positions). So it sends nothing for a while after a frame is shown, and again before the next
    f.dLogicUs = LogicBaseUs * cpu.dCore;
    f.aRamp[0] = { 0.0, llFrom };
    f.nRamp = 1;
    if ( !p.bPaused && llTo > llFrom )
    {
        double dOns = 0.0, dOffs = 0.0, dOthers = 0.0, dWalk = 0.0, dWalked = 0.0;
        ForPieces( song, llFrom, llTo, [&]( const Piece &q ) {
            dOns += q.dOns;
            dOffs += q.dOffs;
            dOthers += q.dOthers;
            dWalk += WalkUs( cpu, q );
            dWalked += q.dOffs * q.dPoly;
        } );
        const double dEvents = dOns + dOffs + dOthers;
        const double dEventUs = AtSize( cpu, EventNs, dEvents * EventBytes, false ) / 1000.0;
        const double dEvictUs = AtSize( cpu, WalkEvictNs, dEvents * EventBytes ) / 1000.0;     // a walked note's
        dWalk += dWalked * dEvictUs;
        const double dEventCostUs = dEvents * dEventUs;
        const long long llSpan = std::max< long long >( 1, p.llSpanUs );
        const double dViewIn = Events( llFrom + llSpan, llTo + llSpan );
        const double dPre = LogicBaseUs * cpu.dCore + dViewIn * AtSize( cpu, ViewInNs, dViewIn * EventBytes, false ) / 1000.0;
        const double dPost = dEvents * AtSize( cpu, ReplayNs, dEvents * EventBytes, false ) / 1000.0;
        const double dSent = dEvents - ( p.bMuted ? dOns : 0.0 );
        const double dLoop = dEventCostUs + dWalk;
        double dSend = dSent * p.dSendUs;
        if ( pNoise && dSent > 0.0 )
        {
            // The send path's slow state and the messages' own spread (averaging out over many), and
            // the rare message that stalls
            const double s = SendStateSd;
            double g = std::exp( pNoise->SendState() - s * s / 2.0 ) * std::max( 0.3, 1.0 + SendMsgSd / std::sqrt( dSent ) * pNoise->Normal() );
            dSend *= g;
            const int nSpikes = pNoise->Poisson( dSent / SendSpikeEvery );
            for ( int i = 0; i < nSpikes && i < 64; i++ ) dSend += pNoise->Exponential( SendSpikeMult * p.dSendUs );
            if ( nSpikes > 64 ) dSend += ( nSpikes - 64 ) * SendSpikeMult * p.dSendUs;
        }
        const double g = Spread( dPre + dLoop + dPost );
        const double dPreUs = dPre * g, dLoopUs = dLoop * g + dSend, dPostUs = dPost * g;
        f.dLogicUs = dPreUs + dLoopUs + dPostUs;
        f.aRamp[f.nRamp++] = { dPreUs, llFrom };

        // The sends' ramp: song time against Logic's time. Its knots are spaced evenly along work and
        // song time together, so both where it crawls (the walks) and where it races (light song time
        // after them) are followed closely. Inside a piece the work is its events and sends, evenly,
        // and its walks, each of the polyphony there (falling through a mass release, rising through
        // a slam); the decoder takes a time inside a millisecond as that share of its events
        const double dSendEach = dSent > 0.0 ? dSend / dSent : p.dSendUs;
        const double dTotal = dEventCostUs + dWalk + dSent * dSendEach;
        const double dSong = (double)( llTo - llFrom );
        const int M = RampPoints - 4;
        const double dScale = dTotal > 0.0 ? dLoopUs / dTotal : 0.0;
        const double dArcAll = ( dTotal > 0.0 ? 1.0 : 0.0 ) + 1.0;
        double dArc = 0.0, dWork = 0.0;
        int k = 1;
        ForPieces( song, llFrom, llTo, [&]( const Piece &q ) {
            if ( k > M ) return;
            const double dSentQ = q.dOns * ( p.bMuted ? 0.0 : 1.0 ) + q.dOffs + q.dOthers;
            const double dWalkEach = q.dOffs > 0.0 ? AtSize( cpu, WalkNs, q.dPoly * SoundingBytes ) / 1000.0 + dEvictUs : 0.0;
            const double A = ( q.dOns + q.dOffs + q.dOthers ) * dEventUs + dSentQ * dSendEach + q.dOffs * q.dPolyA * dWalkEach;
            const double B = q.dOffs * ( q.dPolyB - q.dPolyA ) * dWalkEach * 0.5;
            const double c1 = ( dTotal > 0.0 ? A / dTotal : 0.0 ) + (double)( q.llEnd - q.llStart ) / dSong;
            const double c2 = dTotal > 0.0 ? B / dTotal : 0.0;
            const double dArcQ = c1 + c2;
            while ( k <= M && dArc + dArcQ >= dArcAll * k / ( M + 1 ) )
            {
                // Where in the piece the arc reaches the knot: c1 x + c2 x^2 = d
                const double d = dArcAll * k / ( M + 1 ) - dArc;
                const double r = c1 + std::sqrt( std::max( 0.0, c1 * c1 + 4.0 * c2 * d ) );
                const double x = std::clamp( r > 0.0 ? 2.0 * d / r : 0.0, 0.0, 1.0 );
                const RampPoint &prev = f.aRamp[f.nRamp - 1];
                const double dAt = std::min( dPreUs + dLoopUs, dPreUs + ( dWork + A * x + B * x * x ) * dScale );
                const long long llAt = q.llStart + (long long)( ( q.llEnd - q.llStart ) * x );
                f.aRamp[f.nRamp++] = { std::max( dAt, prev.dUs ), std::max( llAt, prev.llSong ) };
                k++;
            }
            dArc += dArcQ;
            dWork += A + B;
        } );
        f.aRamp[f.nRamp++] = { dPreUs + dLoopUs, llTo };
    }
    {
        const double dLast = f.aRamp[f.nRamp - 1].dUs;
        f.aRamp[f.nRamp++] = { std::max( f.dLogicUs, dLast ), llTo };
    }

    // Render: the sounding notes and the events in view, walked; the notes drawn
    const double dPolyTo = (double)ns.Polyphony( llTo );
    {
        const long long llEnd = llTo + std::max< long long >( 1, p.llSpanUs );
        const double dOnsW = Ons( llTo, llEnd );
        const double dOffsW = std::clamp( dOnsW - ( (double)ns.Polyphony( llEnd ) - dPolyTo ), 0.0, dOnsW + dPolyTo );
        const double dEventsW = dOnsW + dOffsW + Others( llTo, llEnd );
        const double dDrawn = dPolyTo + dOnsW;
        const double dBytes = dPolyTo * SoundingBytes + dEventsW * EventBytes;
        const double dRender = ( RenderBaseUs + dDrawn * DrawnNs / 1000.0 ) * cpu.dCore + ( dPolyTo * AtSize( cpu, RenderSoundingNs, dBytes ) + dEventsW * AtSize( cpu, RenderViewNs, dBytes ) ) / 1000.0;
        f.dRenderUs = dRender * Spread( dRender );
        f.llInView = (uint64_t)dDrawn;
    }
    // The odd stall (another thread scheduled in, a page fault): with the frame's Render
    if ( pNoise )
    {
        const int nStalls = pNoise->Poisson( ( f.dLogicUs + f.dRenderUs ) / StallEveryUs );
        for ( int i = 0; i < nStalls && i < 64; i++ ) f.dRenderUs += pNoise->Exponential( StallUs );
    }
    f.llPolyphony = (uint64_t)dPolyTo;
    return f;
}
