// The playback engine (DESIGN.md 7): a decoder thread runs PlaybackDecoder a few milliseconds ahead
// of the clock and hands fixed-size slices to a sender thread, which owns the MIDI output and sends
// each slice when it is due. Slices of an older epoch (before a seek) are dropped. Neither thread
// ever waits on the render thread, and the clock never waits on them. Every event is sent unless
// stack collapsing, the send budget or velocity skipping is on (PlaybackDecoder).
// When one thread cannot decode fast enough (the song's precomputed note-on counts say so for the
// next 20 ms, or decoding falls behind), the decoder thread decodes in parallel batches with helper
// threads, started the first time they are needed, up to the loading and drawing threads setting
// (DESIGN.md 7.11).
#pragma once

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/Clock.h"
#include "audio/MidiOutput.h"
#include "audio/PlaybackDecoder.h"
#include "song/Song.h"

class PlaybackEngine
{
public:
    struct Stats
    {
        uint64_t llSent = 0;            // messages handed to the output by the sender thread
        uint64_t llDecoded = 0, llNoteOns = 0, llSkipped = 0, llCollapsed = 0;     // counted by the decoder
        long long llLagUs = 0;          // how late the last slice was sent
        double dNsPerMsg = 0.0;         // the output's measured cost per message
        int iSkipVelocity = 0;          // note-ons below this velocity are being skipped (0: none)
        PlaybackDecoder::BudgetCut budget;  // the send budget's cut
        int iLanes = 1;                 // the most threads decoding since the last GetStats (1: the decoder thread alone)
        // Where the time goes: microseconds each thread spent working (the decoder in Fill and the
        // chase, the sender in SendBatch), and the ring's fill (in slices) summed and counted each
        // time the sender takes a slice. All cumulative: the stats box shows the change per interval.
        uint64_t llDecodeBusyUs = 0, llSendBusyUs = 0;
        uint64_t llRingSum = 0, llRingSamples = 0;
        int iRingMax = 0;               // the fullest the ring was since the last GetStats
    };
    static const int RingSlices = 63;   // the ring's capacity in slices (RingSize - 1)

    // iThreads: the most threads decoding may use (0: the logical core count)
    PlaybackEngine( std::shared_ptr< const Song > pSong, PlaybackClock &clock, int iThreads = 0 );
    ~PlaybackEngine();

    // Opens the configured output (on the sender thread) and starts both threads
    void Start( long long llStartTime );
    void Stop();

    // Jump: AllNotesOff, chase, then play from llTarget. llClamped is the new clock time and
    // llPrevTime the playback time before the jump.
    void Seek( long long llTarget, long long llClamped, long long llPrevTime );
    void AllNotesOff();                 // pause and mute
    void ReopenOutput();                // the device list changed

    void SetMute( bool bMute ) { m_bMute.store( bMute, std::memory_order_relaxed ); }
    void SetVolumeCorrect( double d ) { m_dVolumeCorrect.store( d, std::memory_order_relaxed ); }
    void SetMetronome( int iMode ) { m_iMetronome.store( iMode, std::memory_order_relaxed ); }
    void SetChannelMuted( int iTrack, int iChannel, bool bMuted );
    void SetSkipping( bool bVelocitySkip, int iSkipLagMs );
    // Stack collapsing and the send budget (messages per second, 0: none); see PlaybackDecoder::Params
    void SetSendLimits( bool bCollapse, long long llMaxSendsPerSec );
    // 31-EDO tuning (Performance tab), applied at once: a change reopens the output in the new mode
    // (the caller then seeks to the current time, so the new output gets the controllers there).
    // True when it changed
    bool SetTuning( bool bTuning31, bool bPitchBend );
    void SetMinVelocity( int iMinVelocity ) { m_iMinVelocity.store( iMinVelocity, std::memory_order_relaxed ); }     // 0: none
    // The most threads decoding may use (0: the logical core count); takes effect at the next decode
    void SetMaxThreads( int iThreads );

    std::wstring OutputName() const;
    bool HasOutput() const { return m_bHasOutput.load( std::memory_order_acquire ); }
    Stats GetStats();

private:
    static const size_t SliceMsgs = 8192;
    static const int RingSize = 64;
    struct Slice
    {
        unsigned uEpoch;
        long long llDue;
        bool bImmediate;
        uint32_t n;
        uint32_t aMsgs[SliceMsgs];
    };

    void DecoderThread();
    void SenderThread();
    // Parallel decoding: how many lanes the load ahead needs (1: none), the helper threads, a batch
    int LanesWanted( const PlaybackDecoder::Params &p, long long llNow );
    void EnsureLanes( int iLanes );
    void LaneThread( int k, unsigned uSeen );
    void RunOnLanes( int iLanes, const std::function< void( int ) > &fn );     // fn( k ) for every lane, 0 on this thread
    bool RunBatch( int iLanes, const PlaybackDecoder::Params &p, long long llNow, unsigned uEpoch, HANDLE hTimer );
    Slice *WriteSlot();
    void Commit();

    std::shared_ptr< const Song > m_pSong;
    PlaybackClock &m_Clock;
    PlaybackDecoder m_Decoder;
    std::unique_ptr< std::atomic< uint16_t >[] > m_pMuteMask;

    // Ring of slices (single producer: decoder, single consumer: sender)
    std::unique_ptr< Slice[] > m_pRing;
    std::atomic< int > m_iWrite{ 0 }, m_iRead{ 0 };

    // Seek requests: written under m_mxSeek together with the clock change
    std::mutex m_mxSeek;
    bool m_bSeekReset = true;
    long long m_llSeekTarget = 0, m_llSeekPrev = 0;

    // Parameters
    std::atomic< bool > m_bMute{ false };
    std::atomic< double > m_dVolumeCorrect{ 1.0 };
    std::atomic< int > m_iMetronome{ 0 }, m_iSkipLagMs{ 100 };
    std::atomic< bool > m_bVelocitySkip{ false }, m_bCollapse{ false };
    std::atomic< bool > m_bTuning31{ false }, m_bTuningBend{ false };
    std::atomic< long long > m_llMaxSendsPerSec{ 0 };
    std::atomic< int > m_iMinVelocity{ 0 };

    // Feedback and stats
    std::atomic< double > m_dNsPerMsg{ 0.0 };
    std::atomic< long long > m_llLagUs{ 0 };
    std::atomic< int > m_iSkipVelocity{ 0 }, m_iBudgetVel{ 0 };
    std::atomic< double > m_dBudgetKeep{ 1.0 };
    std::atomic< uint64_t > m_llDecoded{ 0 }, m_llNoteOns{ 0 }, m_llSkipped{ 0 }, m_llCollapsed{ 0 };
    std::atomic< uint64_t > m_llSent{ 0 }, m_llDecodeBusyUs{ 0 }, m_llSendBusyUs{ 0 }, m_llRingSum{ 0 }, m_llRingSamples{ 0 };
    std::atomic< int > m_iRingMax{ 0 };

    // Output (used by the sender thread only)
    std::unique_ptr< MidiOutput > m_pOut;
    mutable std::mutex m_mxName;
    std::wstring m_sOutputName;
    std::atomic< bool > m_bHasOutput{ false }, m_bNotesOff{ false }, m_bReopen{ true }, m_bStop{ false };

    std::thread m_DecoderThread, m_SenderThread;

    // Parallel decoding (decoder thread; lane k > 0 runs on helper thread k)
    std::atomic< int > m_iMaxLanes{ 1 };
    int m_iThreadsSetting = -1;         // what SetMaxThreads was last given (its caller's thread only)
    double m_dRate = 150.0;             // one thread's decoding speed, events per microsecond (measured)
    std::vector< std::unique_ptr< PlaybackDecoder > > m_vLaneDec;
    std::vector< PlaybackDecoder::LaneOut > m_vLaneOut;
    std::vector< std::thread > m_vLaneThreads;
    std::mutex m_mxLanes;
    std::condition_variable m_cvLanes, m_cvLanesDone;
    unsigned m_uLaneJob = 0;
    int m_iJobLanes = 0, m_iLanesLeft = 0;
    bool m_bLanesStop = false;
    const std::function< void( int ) > *m_pJobFn = nullptr;
    std::atomic< int > m_iLanesNow{ 1 }, m_iLanesPeak{ 1 };
};
