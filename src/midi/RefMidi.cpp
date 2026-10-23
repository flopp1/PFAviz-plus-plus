#include "midi/RefMidi.h"

#include "song/Song.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

//-----------------------------------------------------------------------------
// Loading
//-----------------------------------------------------------------------------

bool RefMidi::LoadFile( const std::wstring &sFilename )
{
    HANDLE hFile = CreateFileW( sFilename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr );
    if ( hFile == INVALID_HANDLE_VALUE ) return false;
    LARGE_INTEGER liSize;
    if ( !GetFileSizeEx( hFile, &liSize ) || liSize.QuadPart <= 0 ) { CloseHandle( hFile ); return false; }
    std::vector< unsigned char > vData( (size_t)liSize.QuadPart );
    size_t iRead = 0;
    while ( iRead < vData.size() )
    {
        DWORD dwChunk = (DWORD)std::min< size_t >( vData.size() - iRead, 1u << 30 ), dwGot = 0;
        if ( !ReadFile( hFile, vData.data() + iRead, dwChunk, &dwGot, nullptr ) || dwGot == 0 ) break;
        iRead += dwGot;
    }
    CloseHandle( hFile );
    vData.resize( iRead );
    bool bOk = Parse( vData.data(), vData.size() );
    m_Info.sFilename = sFilename;
    return bOk;
}

static inline unsigned ReadBE( const unsigned char *p, int n )
{
    unsigned v = 0;
    for ( int i = 0; i < n; i++ ) v = ( v << 8 ) | p[i];
    return v;
}

// Variable-length quantity, at most 4 bytes (SMF spec). Returns false if truncated or longer than 4 bytes.
static bool ReadVarLen( const unsigned char *&p, const unsigned char *pEnd, long long &llOut )
{
    llOut = 0;
    for ( int i = 0; i < 4; i++ )
    {
        if ( p >= pEnd ) return false;
        unsigned char c = *p++;
        llOut = ( llOut << 7 ) | ( c & 0x7F );
        if ( !( c & 0x80 ) ) return true;
    }
    return false;
}

bool RefMidi::Parse( const unsigned char *pData, size_t iSize )
{
    m_Info = RefMidiInfo();
    m_vTracks.clear();
    m_vEvents.clear();
    m_vMeta.clear();
    m_vTempo.clear();

    // Header
    if ( iSize < 14 || memcmp( pData, "MThd", 4 ) != 0 ) return false;
    unsigned iHdrSize = std::max( ReadBE( pData + 4, 4 ), 6u );
    m_Info.iFormatType = (int)ReadBE( pData + 8, 2 );
    m_Info.iDivision = (int)ReadBE( pData + 12, 2 );
    if ( m_Info.iFormatType > 2 || m_Info.iDivision == 0 ) return false;

    // Chunks: MTrk chunks by declared length (clamped to the file); unknown chunks skipped
    std::vector< std::vector< TrackEvent > > vTrackEvents;
    std::vector< std::vector< RefMetaEvent > > vTrackMeta;
    size_t iPos = 8 + (size_t)iHdrSize;
    while ( iPos + 8 <= iSize )
    {
        const unsigned char *pChunk = pData + iPos;
        unsigned long long llLen = ReadBE( pChunk + 4, 4 );
        size_t iStart = iPos + 8;
        size_t iEnd = (size_t)std::min< unsigned long long >( iStart + llLen, iSize );
        if ( memcmp( pChunk, "MTrk", 4 ) == 0 )
        {
            int iTrack = (int)m_vTracks.size();
            m_vTracks.emplace_back();
            vTrackEvents.emplace_back();
            m_vMetaScratch.clear();
            ParseTrack( pData + iStart, pData + iEnd, iTrack, vTrackEvents.back(), m_vTracks.back() );
            vTrackMeta.push_back( m_vMetaScratch );
        }
        iPos = iEnd;
    }
    return Finalize( vTrackEvents, vTrackMeta );
}

bool RefMidi::BuildFromSong( const Song &song )
{
    int iFormat = song.info.iFormatType, iDivision = song.info.iDivision;
    m_Info = RefMidiInfo();
    m_Info.sFilename = song.info.sFilename;
    m_Info.iFormatType = iFormat;
    m_Info.iDivision = iDivision;
    m_vTracks = song.vTracks;
    m_vEvents.clear();
    m_vMeta.clear();
    m_vTempo.clear();

    std::vector< std::vector< TrackEvent > > vTrackEvents( m_vTracks.size() );
    song.stream.ForEach( [&]( size_t u, const StreamEvent &e ) {
        TrackEvent te;
        te.llTick = (long long)song.stream.ticks.Get( u );
        te.cCode = e.cStatus;
        te.cParam1 = e.cData1;
        te.cParam2 = e.cData2;
        vTrackEvents[e.uTrack].push_back( te );
    } );
    std::vector< std::vector< RefMetaEvent > > vTrackMeta = song.vTrackMeta;
    return Finalize( vTrackEvents, vTrackMeta );
}

// Shared by Parse and BuildFromSong: pairing, info aggregation, the global merge and PostProcess
bool RefMidi::Finalize( std::vector< std::vector< TrackEvent > > &vTrackEvents, std::vector< std::vector< RefMetaEvent > > &vTrackMeta )
{
    m_Info.iNumTracks = (int)m_vTracks.size();
    m_Info.llTotalTicks = m_Info.llEventCount = m_Info.llVolumeSum = m_Info.llNoteCount = 0;
    m_Info.iNumChannels = m_Info.iMinNote = m_Info.iMaxNote = m_Info.iMaxVolume = 0;

    // Pairing per track (ConnectNotes), before the merge
    for ( auto &v : vTrackEvents )
        ConnectNotes( v );

    // Track info aggregation (MIDIInfo::AddTrackInfo)
    for ( const RefTrackInfo &mti : m_vTracks )
    {
        m_Info.llTotalTicks = std::max( m_Info.llTotalTicks, mti.llTotalTicks );
        m_Info.llEventCount += mti.llEventCount;
        m_Info.iNumChannels += mti.iNumChannels;
        m_Info.llVolumeSum += mti.llVolumeSum;
        if ( mti.llNoteCount )
        {
            if ( !m_Info.llNoteCount )
            {
                m_Info.iMinNote = mti.iMinNote;
                m_Info.iMaxNote = mti.iMaxNote;
                m_Info.iMaxVolume = mti.iMaxVolume;
            }
            else
            {
                m_Info.iMinNote = std::min( mti.iMinNote, m_Info.iMinNote );
                m_Info.iMaxNote = std::max( mti.iMaxNote, m_Info.iMaxNote );
                m_Info.iMaxVolume = std::max( mti.iMaxVolume, m_Info.iMaxVolume );
            }
        }
        m_Info.llNoteCount += mti.llNoteCount;
    }

    // Merge channel events in (tick, track, position) order: the original MIDIPos order (R1)
    struct Key { long long llTick; int iTrack, iIdx; };
    std::vector< Key > vKeys;
    size_t iTotal = 0;
    for ( auto &v : vTrackEvents ) iTotal += v.size();
    vKeys.reserve( iTotal );
    for ( int t = 0; t < (int)vTrackEvents.size(); t++ )
        for ( int i = 0; i < (int)vTrackEvents[t].size(); i++ )
            vKeys.push_back( { vTrackEvents[t][i].llTick, t, i } );
    std::stable_sort( vKeys.begin(), vKeys.end(), []( const Key &a, const Key &b ) {
        return a.llTick != b.llTick ? a.llTick < b.llTick : a.iTrack < b.iTrack; } );

    std::vector< std::vector< int > > vGlobal( vTrackEvents.size() );
    for ( size_t t = 0; t < vTrackEvents.size(); t++ ) vGlobal[t].resize( vTrackEvents[t].size() );
    for ( size_t g = 0; g < vKeys.size(); g++ ) vGlobal[vKeys[g].iTrack][vKeys[g].iIdx] = (int)g;

    m_vEvents.resize( vKeys.size() );
    for ( size_t g = 0; g < vKeys.size(); g++ )
    {
        const TrackEvent &te = vTrackEvents[vKeys[g].iTrack][vKeys[g].iIdx];
        RefChannelEvent &e = m_vEvents[g];
        e.llTick = te.llTick;
        e.iTrack = vKeys[g].iTrack;
        e.cCode = te.cCode;
        e.cParam1 = te.cParam1;
        e.cParam2 = te.cParam2;
        e.iSister = te.iSister >= 0 ? vGlobal[vKeys[g].iTrack][te.iSister] : -1;
    }

    // Meta events (tempo, time signature) in the same order
    for ( auto &v : vTrackMeta ) m_vMeta.insert( m_vMeta.end(), v.begin(), v.end() );
    std::stable_sort( m_vMeta.begin(), m_vMeta.end(), []( const RefMetaEvent &a, const RefMetaEvent &b ) {
        return a.llTick != b.llTick ? a.llTick < b.llTick : a.iTrack < b.iTrack; } );

    // PostProcess: times, simultaneous counts, first note, total length
    BuildTempoMap();
    for ( RefMetaEvent &m : m_vMeta ) m.llMicroSec = TickToMicroSecs( m.llTick );
    int iSimultaneous = 0;
    long long llFirstNote = -1;
    for ( RefChannelEvent &e : m_vEvents )
    {
        e.llMicroSec = TickToMicroSecs( e.llTick );
        e.iSimultaneous = iSimultaneous;
        if ( e.iSister >= 0 )
        {
            if ( e.IsNoteOnVel() )
            {
                if ( llFirstNote < 0 ) llFirstNote = e.llMicroSec;
                iSimultaneous++;
            }
            else
                iSimultaneous--;
        }
    }
    m_Info.llTotalMicroSecs = TickToMicroSecs( m_Info.llTotalTicks );
    m_Info.llFirstNote = std::max( 0LL, llFirstNote );
    return true;
}

// One MTrk chunk, SMF semantics (DESIGN.md 1.5). Malformed data ends the track; earlier events are kept.
bool RefMidi::ParseTrack( const unsigned char *p, const unsigned char *pEnd, int iTrack,
                          std::vector< TrackEvent > &vEvents, RefTrackInfo &info )
{
    long long llTick = 0;
    int iRunning = 0;           // running status (channel status only); 0 = none

    auto AddInfo = [&]( long long llAt ) { info.llEventCount++; info.llTotalTicks = std::max( info.llTotalTicks, llAt ); };

    while ( p < pEnd )
    {
        int iStatus;
        long long llDelta;
        if ( !ReadVarLen( p, pEnd, llDelta ) ) { info.bMalformed = true; break; }
        llTick += llDelta;
        if ( p >= pEnd ) { info.bMalformed = true; break; }
        if ( *p >= 0x80 ) iStatus = *p++;
        else if ( iRunning ) iStatus = iRunning;
        else { info.bMalformed = true; break; }

        if ( iStatus < 0xF0 )
        {
            iRunning = iStatus;
            int iType = iStatus >> 4, iChannel = iStatus & 0xF;
            int iDataBytes = ( iType == 0xC || iType == 0xD ) ? 1 : 2;
            if ( pEnd - p < iDataBytes ) { info.bMalformed = true; break; }
            // Data bytes are read as data, as the original reads them. A key, controller or program past
            // 127 drops the event (its tick still counts for the length); a second byte past 127 is clamped
            unsigned char aData[2] = { p[0], iDataBytes == 2 ? std::min< unsigned char >( p[1], 0x7F ) : (unsigned char)0 };
            p += iDataBytes;
            if ( aData[0] >= 0x80 ) { info.llTotalTicks = std::max( info.llTotalTicks, llTick ); continue; }

            TrackEvent te;
            te.llTick = llTick;
            te.cCode = (unsigned char)iStatus;
            te.cParam1 = aData[0];
            te.cParam2 = aData[1];
            vEvents.push_back( te );
            AddInfo( llTick );

            // MIDITrackInfo::AddEventInfo
            if ( iType == 0x9 && aData[1] > 0 )
            {
                if ( !info.llNoteCount )
                {
                    info.iMinNote = info.iMaxNote = aData[0];
                    info.iMaxVolume = aData[1];
                }
                else
                {
                    info.iMinNote = std::min< int >( aData[0], info.iMinNote );
                    info.iMaxNote = std::max< int >( aData[0], info.iMaxNote );
                    info.iMaxVolume = std::max< int >( aData[1], info.iMaxVolume );
                }
                info.llNoteCount++;
                info.llVolumeSum += aData[1];
                if ( !info.aNoteCount[iChannel] ) info.iNumChannels++;
                info.aNoteCount[iChannel]++;
            }
            else if ( iType == 0xC && info.aProgram[iChannel] != aData[0] )
                info.aProgram[iChannel] = info.aNoteCount[iChannel] > 0 ? 128 : aData[0];
        }
        else if ( iStatus == 0xF0 || iStatus == 0xF7 )
        {
            iRunning = 0;
            long long llLen;
            if ( !ReadVarLen( p, pEnd, llLen ) || llLen > pEnd - p ) { info.bMalformed = true; break; }
            p += llLen;      // SysEx is ignored entirely
            AddInfo( llTick );
        }
        else if ( iStatus == 0xFF )
        {
            iRunning = 0;
            if ( p >= pEnd ) { info.bMalformed = true; break; }
            unsigned char cType = *p++;
            long long llLen;
            if ( !ReadVarLen( p, pEnd, llLen ) || llLen > pEnd - p ) { info.bMalformed = true; break; }
            const unsigned char *pMeta = p;
            p += llLen;
            AddInfo( llTick );
            if ( cType == 0x2F ) break;
            if ( ( cType == RefMetaEvent::SetTempo && llLen == 3 ) || ( cType == RefMetaEvent::TimeSignature && llLen == 4 ) )
            {
                RefMetaEvent m;
                m.llTick = llTick;
                m.iTrack = iTrack;
                m.cType = cType;
                m.iDataLen = (int)llLen;
                memcpy( m.aData, pMeta, (size_t)llLen );
                m_vMetaScratch.push_back( m );
            }
            else if ( cType == 0x03 )
                info.sSequenceName.assign( reinterpret_cast< const char * >( pMeta ), (size_t)llLen );
        }
        else
        {
            info.bMalformed = true;   // 0xF1-0xFE are not valid in a Standard MIDI File
            break;
        }
    }
    return !info.bMalformed;
}

// Exact emulation of MIDI::ConnectNotes, including the overflow fallback and re-pairing.
// The original's backward search is replaced by a tombstoned stack per pitch: entries are
// pushed in increasing event order, so its top is the latest unpaired NoteOn of that pitch.
void RefMidi::ConnectNotes( std::vector< TrackEvent > &vEvents )
{
    const int StackSize = 10;
    static int pSize[16][128];
    static int pStacks[16][128][StackSize];
    memset( pSize, 0, sizeof( pSize ) );
    std::vector< int > vUnpaired[128];

    auto SetSister = [&]( int iOn, int iOff ) { vEvents[iOn].iSister = iOff; vEvents[iOff].iSister = iOn; };

    for ( int i = 0; i < (int)vEvents.size(); i++ )
    {
        const TrackEvent &e = vEvents[i];
        int iType = e.cCode >> 4, iChannel = e.cCode & 0xF;
        int iNote = e.cParam1 & 0x7F, iVelocity = e.cParam2;
        if ( e.cParam1 >= 0x80 ) continue;

        if ( iType == 0x9 && iVelocity > 0 )
        {
            int &iSize = pSize[iChannel][iNote];
            if ( iSize < StackSize ) pStacks[iChannel][iNote][iSize] = i;
            iSize++;
            vUnpaired[iNote].push_back( i );
        }
        else if ( iType == 0x8 || iType == 0x9 )
        {
            int &iSize = pSize[iChannel][iNote];
            if ( iSize > 0 )
            {
                if ( iSize <= StackSize )
                    SetSister( pStacks[iChannel][iNote][iSize - 1], i );
                else
                {
                    std::vector< int > &v = vUnpaired[iNote];
                    while ( !v.empty() && vEvents[v.back()].iSister >= 0 ) v.pop_back();
                    if ( !v.empty() ) SetSister( v.back(), i );
                }
                iSize--;
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Time (DESIGN.md R3)
//-----------------------------------------------------------------------------

void RefMidi::BuildTempoMap()
{
    m_vTempo.clear();
    m_vTempo.push_back( { 0, 0, 500000 } );
    for ( const RefMetaEvent &m : m_vMeta )
        if ( m.cType == RefMetaEvent::SetTempo )
        {
            long long llTime = TickToMicroSecs( m.llTick );
            long long llUspb = ( (long long)m.aData[0] << 16 ) | ( m.aData[1] << 8 ) | m.aData[2];
            m_vTempo.push_back( { m.llTick, llTime, llUspb } );
        }
}

long long RefMidi::TickToMicroSecs( long long llTick ) const
{
    // Last segment whose start tick is <= llTick (any of several at the same tick give the same time)
    auto it = std::upper_bound( m_vTempo.begin(), m_vTempo.end(), llTick,
                                []( long long t, const TempoSeg &s ) { return t < s.llTick; } );
    const TempoSeg &s = ( it == m_vTempo.begin() ) ? m_vTempo.front() : *( it - 1 );
    int iDivision = m_Info.iDivision;
    if ( iDivision & 0x8000 )
    {
        // SMPTE: frames per second * 100 (29.97 kept exact) times ticks per frame
        int iFramesPerSec100 = -( ( iDivision | static_cast< int >( 0xFFFF0000 ) ) >> 8 ) * 100;
        if ( iFramesPerSec100 == 2900 ) iFramesPerSec100 = 2997;
        long long llTicksPerSec100 = (long long)( iDivision & 0xFF ) * iFramesPerSec100;
        if ( llTicksPerSec100 <= 0 ) return s.llMicroSec;
        return s.llMicroSec + ( 100000000LL * ( llTick - s.llTick ) ) / llTicksPerSec100;
    }
    return s.llMicroSec + ( s.llMicroSecsPerBeat * ( llTick - s.llTick ) ) / iDivision;
}
