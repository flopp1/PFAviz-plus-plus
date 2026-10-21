// Reference MIDI model: a simple, unoptimised in-memory representation with the exact
// semantics of the original MIDI::ConnectNotes / MIDI::PostProcess (DESIGN.md R1-R3, Section 6).
// Parsing follows the SMF spec (DESIGN.md 1.5). Used for the splash song and as the test oracle;
// the fast loader replaces it for real playback.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RefChannelEvent
{
    long long llTick = 0;
    long long llMicroSec = 0;
    int iTrack = 0;
    unsigned char cCode = 0, cParam1 = 0, cParam2 = 0;
    int iSister = -1;           // index into the merged channel event list; the original's m_pSister
    int iSimultaneous = 0;

    enum ChannelEventType { NoteOff = 0x8, NoteOn, NoteAftertouch, Controller, ProgramChange, ChannelAftertouch, PitchBend };
    ChannelEventType GetType() const { return static_cast< ChannelEventType >( cCode >> 4 ); }
    int GetChannel() const { return cCode & 0xF; }
    bool IsNoteOnVel() const { return GetType() == NoteOn && cParam2 > 0; }
};

struct RefMetaEvent
{
    enum Type { SetTempo = 0x51, TimeSignature = 0x58 };
    long long llTick = 0;
    long long llMicroSec = 0;
    int iTrack = 0;
    unsigned char cType = 0;
    unsigned char aData[4] = {};
    int iDataLen = 0;
};

struct RefTrackInfo
{
    int aNoteCount[16] = {};
    int aProgram[16] = {};
    int iNumChannels = 0;
    int iMinNote = 0, iMaxNote = 0, iMaxVolume = 0;
    long long llNoteCount = 0, llVolumeSum = 0, llEventCount = 0, llTotalTicks = 0;
    std::string sSequenceName;
    bool bMalformed = false;
};

struct RefMidiInfo
{
    std::wstring sFilename;
    int iFormatType = 0, iNumTracks = 0, iNumChannels = 0, iDivision = 0;
    int iMinNote = 0, iMaxNote = 0, iMaxVolume = 0;
    long long llNoteCount = 0, llEventCount = 0, llVolumeSum = 0;
    long long llTotalTicks = 0, llTotalMicroSecs = 0, llFirstNote = 0;
};

class RefMidi
{
public:
    bool LoadFile( const std::wstring &sFilename );
    bool Parse( const unsigned char *pData, size_t iSize );
    bool BuildFromSong( const struct Song &song );   // same result as Parse, from the fast loader's output
    bool IsValid() const { return m_vTracks.size() > 0 && m_Info.llNoteCount > 0 && m_Info.iDivision > 0; }

    const RefMidiInfo &GetInfo() const { return m_Info; }
    const std::vector< RefTrackInfo > &GetTracks() const { return m_vTracks; }
    const std::vector< RefChannelEvent > &GetEvents() const { return m_vEvents; }
    std::vector< RefChannelEvent > &GetEvents() { return m_vEvents; }
    const std::vector< RefMetaEvent > &GetMetaEvents() const { return m_vMeta; }

    long long TickToMicroSecs( long long llTick ) const;

private:
    struct TrackEvent { long long llTick; unsigned char cCode, cParam1, cParam2; int iSister = -1; };
    struct TempoSeg { long long llTick, llMicroSec; long long llMicroSecsPerBeat; };

    bool ParseTrack( const unsigned char *p, const unsigned char *pEnd, int iTrack,
                     std::vector< TrackEvent > &vEvents, RefTrackInfo &info );
    static void ConnectNotes( std::vector< TrackEvent > &vEvents );
    bool Finalize( std::vector< std::vector< TrackEvent > > &vTrackEvents, std::vector< std::vector< RefMetaEvent > > &vTrackMeta );
    void BuildTempoMap();

    RefMidiInfo m_Info;
    std::vector< RefTrackInfo > m_vTracks;
    std::vector< RefChannelEvent > m_vEvents;
    std::vector< RefMetaEvent > m_vMeta;
    std::vector< TempoSeg > m_vTempo;
    std::vector< RefMetaEvent > m_vMetaScratch;   // meta events of the track being parsed
};
