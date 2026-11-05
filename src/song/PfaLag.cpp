#include "song/PfaLag.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

#include "song/Song.h"

namespace
{
    // The working set per note: a sounding note's event object (96 bytes on the heap) and its line of
    // the event pointer array; an event in view or played, its object and pointer
    const double SoundingBytes = 160.0, EventBytes = 104.0;

    // Costs measured from stock PFA 1.1.0 (its own VS2010 build, with the usual winmm.dll patch that
    // sends to KDMAPI) on a Ryzen 5 5500 (L1 32 KB, L2 512 KB, L3 16 MB): its frames logged by a d3d9
    // proxy over eleven black MIDIs, from 2,500 fps down to 49 s frames, fitted by least squares with
    // the walks counted per millisecond (each stretch weighted by its length, so crash frames count).
    // Each cost is given at five working-set sizes, the caches' and past them (L1, L2, L3, 4 x L3,
    // 32 x L3), and taken between them in proportion to log2 of the size, so other cache sizes move the
    // curves with them. (Without the patch, WinMM's 13-28 us a message on Windows 11 also flushed the
    // caches between events, and every event and walk cost several times more.)
    const double LogicBaseUs = 7.65;
    // An event played (its loops, its state; PFA reads them in order, so the memory streams)
    const double EventNs[5] = { 162.1, 162.1, 162.1, 162.1, 162.1 };
    // A sounding note, per note-off, by the sounding notes' size (past L3, no new measurements: the
    // earlier ones); and more by the frame's events, which evict the notes as they stream past
    const double WalkNs[5] = { 0.0, 1.26, 1.62, 9.56, 9.56 };
    const double WalkEvictNs[5] = { 0.0, 0.0, 0.040, 0.434, 0.411 };
    const double RenderBaseUs = 394.2;
    // A sounding note or an event in view (past 4 x L3, the last measured cost)
    const double RenderWalkNs[5] = { 0.0, 18.79, 42.99, 113.19, 113.19 };
    const double DrawnNs = 60.4;                                       // a note drawn

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

    double AtSize( const PfaLag::Cpu &cpu, const double ay[5], double dBytes )
    {
        double h[5];
        Hats( cpu, dBytes, h );
        double v = 0.0;
        for ( int i = 0; i < 5; i++ ) v += h[i] * ay[i];
        return v;
    }

    // A piece of song time within one bin of the note statistics and of the CC counts: its share of
    // the bin's note-ons, note-offs (the note-ons less what the polyphony grew by) and other events,
    // and the polyphony at its middle
    struct Piece
    {
        long long llStart, llEnd;
        double dOns, dOffs, dOthers, dPoly;
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
            Piece q = { t, t1, 0.0, 0.0, 0.0, 0.0 };
            if ( b < nb )
            {
                const double dOn = (double)ns.vOns[b];
                const double p0 = b > 0 ? (double)ns.vPoly[b - 1] : 0.0, p1 = (double)ns.vPoly[b];
                const double fr = (double)( t1 - t ) / W;
                q.dOns = dOn * fr;
                q.dOffs = std::max( 0.0, dOn - ( p1 - p0 ) ) * fr;
                q.dPoly = p0 + ( p1 - p0 ) * ( ( t + t1 ) * 0.5 - (double)( b * W ) ) / W;
            }
            if ( c < nc ) q.dOthers = (double)cc.vCount[c] * (double)( t1 - t ) / CW;
            f( q );
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

    // Logic: each event played (sent, unless a note-on while muted); each note-off walks the notes
    // sounding at its time
    f.dLogicUs = LogicBaseUs;
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
        const double dEventUs = AtSize( cpu, EventNs, dEvents * EventBytes ) / 1000.0;
        const double dEvictUs = AtSize( cpu, WalkEvictNs, dEvents * EventBytes ) / 1000.0;     // a walked note's
        dWalk += dWalked * dEvictUs;
        const double dEventCostUs = dEvents * dEventUs;
        const double dSent = dEvents - ( p.bMuted ? dOns : 0.0 );
        const double dWork = LogicBaseUs + dEventCostUs + dWalk;
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
        f.dLogicUs = dWork * Spread( dWork ) + dSend;

        // The ramp: song time against Logic's time, at equal shares of its work. The work of a piece
        // is its events, their sends and its walks (the frame's own spread scales it all alike)
        const double dSendEach = dSent > 0.0 ? dSend / dSent : p.dSendUs;
        const double dTotal = dEventCostUs + dWalk + dSent * dSendEach;
        if ( dTotal > 0.0 )
        {
            const double dScale = f.dLogicUs / dTotal;
            const int nSteps = RampPoints - 1;
            double dCum = 0.0;
            int k = 1;
            ForPieces( song, llFrom, llTo, [&]( const Piece &q ) {
                if ( k >= nSteps ) return;
                const double dSentQ = q.dOns * ( p.bMuted ? 0.0 : 1.0 ) + q.dOffs + q.dOthers;
                const double dQ = ( q.dOns + q.dOffs + q.dOthers ) * dEventUs + WalkUs( cpu, q ) + q.dOffs * q.dPoly * dEvictUs + dSentQ * dSendEach;
                if ( dQ <= 0.0 ) return;
                while ( k < nSteps && dCum + dQ >= dTotal * k / nSteps )
                {
                    const double dAt = ( dTotal * k / nSteps - dCum ) / dQ;
                    const long long llAt = q.llStart + (long long)( ( q.llEnd - q.llStart ) * dAt );
                    const long long llPrev = f.aRamp[f.nRamp - 1].llSong;
                    f.aRamp[f.nRamp] = { dTotal * k / nSteps * dScale, std::max( llAt, llPrev ) };
                    f.nRamp++;
                    k++;
                }
                dCum += dQ;
            } );
        }
    }
    f.aRamp[f.nRamp++] = { f.dLogicUs, llTo };

    // Render: the sounding notes and the events in view, walked; the notes drawn
    const double dPolyTo = (double)ns.Polyphony( llTo );
    {
        auto Ons = [&]( long long a, long long b ) { return b > a ? (double)( ns.Passed( b ) - ns.Passed( a ) ) : 0.0; };
        auto Others = [&]( long long a, long long b ) { return b > a && !cc.Empty() ? (double)( cc.Passed( b ) - cc.Passed( a ) ) : 0.0; };
        const long long llEnd = llTo + std::max< long long >( 1, p.llSpanUs );
        const double dOnsW = Ons( llTo, llEnd );
        const double dOffsW = std::clamp( dOnsW - ( (double)ns.Polyphony( llEnd ) - dPolyTo ), 0.0, dOnsW + dPolyTo );
        const double dEventsW = dOnsW + dOffsW + Others( llTo, llEnd );
        const double dDrawn = dPolyTo + dOnsW;
        const double dRender = RenderBaseUs + ( ( dPolyTo + dEventsW ) * AtSize( cpu, RenderWalkNs, dPolyTo * SoundingBytes + dEventsW * EventBytes ) + dDrawn * DrawnNs ) / 1000.0;
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
