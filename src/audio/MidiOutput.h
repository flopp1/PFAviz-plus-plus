// MIDI outputs: WinMM devices and KDMAPI (OmniMIDI and compatible synths), behind one interface.
// Only one thread may send to an output at a time (KDMAPI is not documented as thread-safe).
#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MidiOutput
{
public:
    virtual ~MidiOutput() {}
    virtual void Send( uint32_t uMsg ) = 0;                     // status | data1 << 8 | data2 << 16
    virtual void SendBatch( const uint32_t *pMsgs, size_t n ) { for ( size_t i = 0; i < n; i++ ) Send( pMsgs[i] ); }
    virtual void Flush() {}                                     // sends what SendBatch has gathered (KDMAPI long sends)
    virtual bool SendSysEx( const uint8_t *pData, size_t n ) { return false; }
    virtual void SetVolume( double dVolume ) {}
    virtual void ResetPlaybackState() {}
    virtual const std::wstring &Name() const = 0;
    virtual bool IsKdmapi() const { return false; }

    // The original's AllNotesOff: All Notes Off and Sustain off on all 16 channels
    void AllNotesOff();
};

namespace MidiOutputs
{
    bool KdmapiAvailable();                       // OmniMIDI.dll loads and reports KDMAPI available
    std::wstring KdmapiName();
    std::wstring KdmapiStatus();                  // one line for the Audio tab
    // The synth's voice count (GetVoiceCount, or GetActiveVoices in some versions) and rendering time
    // (CPU busy, percent): only newer KDMAPI synths have them (false: this one does not)
    bool KdmapiActiveVoices( unsigned &uVoices );
    bool KdmapiRenderingTime( float &fPercent );
    std::unique_ptr< MidiOutput > OpenKdmapi();
    // KDMAPI sends batches as long messages (SendDirectLongData) of raw MIDI bytes with running status,
    // up to 64 KB each, instead of a call per event (Performance tab; off by default). Read per batch
    void SetKdmapiLongSends( bool bOn );
    std::unique_ptr< MidiOutput > OpenWinMM( int iDevice );
    // The device chosen in the settings: KDMAPI when preferred and available, else the WinMM device
    std::unique_ptr< MidiOutput > OpenConfigured( bool bTuning31 = false, bool bPitchBend = false );
    // The name OpenConfigured would open now (empty when nothing would open)
    std::wstring ConfiguredName();
}
