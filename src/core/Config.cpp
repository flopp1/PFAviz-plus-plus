#include "core/Config.h"

#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <algorithm>

#include "app/MainWindow.h"
#include "core/Ini.h"
#include "core/Util.h"
#include "midi/Notes.h"

Config &Config::GetConfig()
{
    static Config instance;
    return instance;
}

Config::Config()
{
    LoadDefaultValues();
    LoadConfigValues();
}

std::wstring Config::GetFolder()
{
    PWSTR pPath = nullptr;
    if ( FAILED( SHGetKnownFolderPath( FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &pPath ) ) )
        return std::wstring();
    std::wstring sFolder = std::wstring( pPath ) + L"\\" APPNAME;
    CoTaskMemFree( pPath );
    if ( GetFileAttributesW( sFolder.c_str() ) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW( sFolder.c_str(), nullptr ) )
        return std::wstring();
    return sFolder;
}

void Config::LoadDefaultValues()
{
    m_VisualSettings.LoadDefaultValues();
    m_AudioSettings.LoadDefaultValues();
    m_VideoSettings.LoadDefaultValues();
    m_ControlsSettings.LoadDefaultValues();
    m_PerformanceSettings.LoadDefaultValues();
    m_ExportSettings.LoadDefaultValues();
    m_PlaybackSettings.LoadDefaultValues();
    m_ViewSettings.LoadDefaultValues();
}

void Config::LoadConfigValues()
{
    std::wstring sFolder = GetFolder();
    if ( sFolder.empty() ) return;
    IniFile ini;
    if ( !ini.Load( sFolder + L"\\config.ini" ) ) return;
    m_VisualSettings.Load( ini );
    m_AudioSettings.Load( ini );
    m_VideoSettings.Load( ini );
    m_ControlsSettings.Load( ini );
    m_PerformanceSettings.Load( ini );
    m_ExportSettings.Load( ini );
    m_PlaybackSettings.Load( ini );
    m_ViewSettings.Load( ini );

    // Version 1 configs (before the preferences dialog existed) only ever saved the default
    // "all 88 keys"; the default is now all 128 keys
    if ( ini.GetInt( "General", "ConfigVersion", 1 ) < 2 && m_VisualSettings.eKeysShown == VisualSettings::All )
        m_VisualSettings.eKeysShown = VisualSettings::All128;
    // Version 2 configs saved LodCache=0 while the cache did not exist yet; it is on by default now
    if ( ini.GetInt( "General", "ConfigVersion", 1 ) < 3 ) m_PerformanceSettings.bLodCache = true;
}

bool Config::SaveConfigValues()
{
    std::wstring sFolder = GetFolder();
    if ( sFolder.empty() ) return false;
    IniFile ini;
    ini.SetInt( "General", "ConfigVersion", 3 );
    m_VisualSettings.Save( ini );
    m_AudioSettings.Save( ini );
    m_VideoSettings.Save( ini );
    m_ControlsSettings.Save( ini );
    m_PerformanceSettings.Save( ini );
    m_ExportSettings.Save( ini );
    m_PlaybackSettings.Save( ini );
    m_ViewSettings.Save( ini );
    return ini.Save( sFolder + L"\\config.ini" );
}

//-----------------------------------------------------------------------------
// Visual
//-----------------------------------------------------------------------------

void VisualSettings::LoadDefaultValues()
{
    eKeysShown = All128;
    bAlwaysShowControls = false;
    uStats = ( 1u << StatTime ) | ( 1u << StatScore ) | NoteStats;
    iFirstKey = Notes::A0;
    iLastKey = Notes::C8;

    iBkgColor = 0x00303030;
    int R, G, B, S = 80, V = 100;
    int iColors = sizeof( colors ) / sizeof( colors[0] );
    for ( int i = 10, count = 0; count < iColors; i = ( i + 7 ) % iColors, count++ )
    {
        Util::HSVtoRGB( 360 * i / iColors, S, V, R, G, B );
        colors[count] = RGB( R, G, B );
    }
    std::swap( colors[2], colors[4] );
}

static unsigned ParseRGB( const std::string &s, unsigned uDefault )
{
    int r, g, b;
    if ( sscanf( s.c_str(), "%d,%d,%d", &r, &g, &b ) != 3 ) return uDefault;
    return ( ( r & 0xFF ) << 0 ) | ( ( g & 0xFF ) << 8 ) | ( ( b & 0xFF ) << 16 );
}

static std::string FormatRGB( unsigned c )
{
    char buf[32];
    snprintf( buf, sizeof( buf ), "%u,%u,%u", ( c >> 0 ) & 0xFF, ( c >> 8 ) & 0xFF, ( c >> 16 ) & 0xFF );
    return buf;
}

void VisualSettings::Load( const IniFile &ini )
{
    eKeysShown = static_cast< KeysShown >( std::clamp( ini.GetInt( "Visual", "KeysShown", eKeysShown ), 0, 3 ) );
    bAlwaysShowControls = ini.GetBool( "Visual", "AlwaysShowControls", bAlwaysShowControls );
    if ( ini.Has( "Visual", "Stats" ) ) uStats = (unsigned)ini.GetInt( "Visual", "Stats", (int)uStats ) & AllStats;
    else
    {
        // A config from before the Statistics dialog: its separate stats box switches
        uStats = ( ini.GetBool( "Visual", "ShowTime", true ) ? 1u << StatTime : 0 ) | ( ini.GetBool( "Video", "ShowFPS", false ) ? 1u << StatFPS : 0 ) |
                 ( ini.GetBool( "Visual", "ShowScore", true ) ? 1u << StatScore : 0 ) | ( ini.GetBool( "Performance", "ExtraStats", true ) ? NoteStats : 0 ) |
                 ( ini.GetBool( "Performance", "ShowPlaybackStats", false ) ? SynthStats | FrameStats : 0 );
    }
    iFirstKey = std::clamp( ini.GetInt( "Visual", "FirstKey", iFirstKey ), 0, 127 );
    iLastKey = std::clamp( ini.GetInt( "Visual", "LastKey", iLastKey ), 0, 127 );
    for ( int i = 0; i < 16; i++ )
        colors[i] = ParseRGB( ini.GetString( "Visual", "Color" + std::to_string( i ) ), colors[i] );
    iBkgColor = ParseRGB( ini.GetString( "Visual", "BkgColor" ), iBkgColor );
}

void VisualSettings::Save( IniFile &ini ) const
{
    ini.SetInt( "Visual", "KeysShown", eKeysShown );
    ini.SetInt( "Visual", "AlwaysShowControls", bAlwaysShowControls );
    ini.SetInt( "Visual", "Stats", (int)uStats );
    ini.SetInt( "Visual", "FirstKey", iFirstKey );
    ini.SetInt( "Visual", "LastKey", iLastKey );
    for ( int i = 0; i < 16; i++ )
        ini.Set( "Visual", "Color" + std::to_string( i ), FormatRGB( colors[i] ) );
    ini.Set( "Visual", "BkgColor", FormatRGB( iBkgColor ) );
}

//-----------------------------------------------------------------------------
// Audio
//-----------------------------------------------------------------------------

void AudioSettings::LoadDefaultValues()
{
    iOutDevice = -1;
    LoadMIDIDevices();
}

void AudioSettings::LoadMIDIDevices()
{
    std::wstring sOldOut( iOutDevice >= 0 && iOutDevice < (int)vMIDIOutDevices.size() ? vMIDIOutDevices[iOutDevice] : L"" );
    iOutDevice = -1;
    vMIDIOutDevices.clear();
    int iNumOutDevs = midiOutGetNumDevs();
    for ( int i = 0; i < iNumOutDevs; i++ )
    {
        MIDIOUTCAPSW moc;
        midiOutGetDevCapsW( i, &moc, sizeof( moc ) );
        vMIDIOutDevices.push_back( moc.szPname );
        if ( sDesiredOut == vMIDIOutDevices[i] )
            iOutDevice = i;
        if ( sOldOut == vMIDIOutDevices[i] && iOutDevice < 0 )
            iOutDevice = i;
    }
    if ( iOutDevice < 0 )
        iOutDevice = iNumOutDevs - 1;
}

void AudioSettings::Load( const IniFile &ini )
{
    if ( ini.Has( "Audio", "MIDIOutDevice" ) )
    {
        sDesiredOut = Util::Utf8ToWide( ini.GetString( "Audio", "MIDIOutDevice" ) );
        for ( size_t i = 0; i < vMIDIOutDevices.size(); i++ )
            if ( vMIDIOutDevices[i] == sDesiredOut )
                iOutDevice = (int)i;
    }
}

void AudioSettings::Save( IniFile &ini ) const
{
    if ( !sDesiredOut.empty() )
        ini.Set( "Audio", "MIDIOutDevice", Util::WideToUtf8( sDesiredOut ) );
}

//-----------------------------------------------------------------------------
// Video, Controls, Performance
//-----------------------------------------------------------------------------

void VideoSettings::LoadDefaultValues()
{
    bLimitFPS = true;
}

void VideoSettings::Load( const IniFile &ini )
{
    bLimitFPS = ini.GetBool( "Video", "LimitFPS", bLimitFPS );
}

void VideoSettings::Save( IniFile &ini ) const
{
    ini.SetInt( "Video", "LimitFPS", bLimitFPS );
}

void ControlsSettings::LoadDefaultValues()
{
    dFwdBackSecs = 3.0;
    dSpeedUpPct = 10.0;
}

void ControlsSettings::Load( const IniFile &ini )
{
    dFwdBackSecs = ini.GetDouble( "Controls", "FwdBackSecs", dFwdBackSecs );
    dSpeedUpPct = ini.GetDouble( "Controls", "SpeedUpPct", dSpeedUpPct );
}

void ControlsSettings::Save( IniFile &ini ) const
{
    ini.SetDouble( "Controls", "FwdBackSecs", dFwdBackSecs );
    ini.SetDouble( "Controls", "SpeedUpPct", dSpeedUpPct );
}

const wchar_t *const ExportSettings::Presets[] = { L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast",
                                                   L"medium", L"slow", L"slower", L"veryslow" };
const int ExportSettings::PresetCount = sizeof( Presets ) / sizeof( Presets[0] );
const wchar_t *const ExportSettings::Encoders[] = { L"x264 (CPU)", L"NVIDIA NVENC H.264", L"NVIDIA NVENC HEVC", L"AMD AMF H.264",
                                                    L"AMD AMF HEVC", L"Intel QSV H.264", L"Intel QSV HEVC" };
const int ExportSettings::EncoderCount = sizeof( Encoders ) / sizeof( Encoders[0] );

void ExportSettings::LoadDefaultValues()
{
    sOutFile.clear();
    bUseWindowSize = false;
    iWidth = 1920;
    iHeight = 1080;
    iFPS = 60;
    iCRF = 20;
    iPreset = 2;    // veryfast
    iEncoder = X264;
    bShowStats = true;
}

void ExportSettings::Load( const IniFile &ini )
{
    sOutFile = Util::Utf8ToWide( ini.GetString( "Export", "OutFile" ) );
    bUseWindowSize = ini.GetBool( "Export", "UseWindowSize", bUseWindowSize );
    iWidth = std::clamp( ini.GetInt( "Export", "Width", iWidth ), 16, 8192 );
    iHeight = std::clamp( ini.GetInt( "Export", "Height", iHeight ), 16, 8192 );
    iFPS = std::clamp( ini.GetInt( "Export", "FPS", iFPS ), 1, 240 );
    iCRF = std::clamp( ini.GetInt( "Export", "CRF", iCRF ), 0, 51 );
    iPreset = std::clamp( ini.GetInt( "Export", "Preset", iPreset ), 0, PresetCount - 1 );
    iEncoder = std::clamp( ini.GetInt( "Export", "Encoder", iEncoder ), 0, EncoderCount - 1 );
    bShowStats = ini.GetBool( "Export", "ShowStats", bShowStats );
}

void ExportSettings::Save( IniFile &ini ) const
{
    if ( !sOutFile.empty() ) ini.Set( "Export", "OutFile", Util::WideToUtf8( sOutFile ) );
    ini.SetInt( "Export", "UseWindowSize", bUseWindowSize );
    ini.SetInt( "Export", "Width", iWidth );
    ini.SetInt( "Export", "Height", iHeight );
    ini.SetInt( "Export", "FPS", iFPS );
    ini.SetInt( "Export", "CRF", iCRF );
    ini.SetInt( "Export", "Preset", iPreset );
    ini.SetInt( "Export", "Encoder", iEncoder );
    ini.SetInt( "Export", "ShowStats", bShowStats );
}

void PerformanceSettings::LoadDefaultValues()
{
    iWorkerThreads = 0;
    bPreferKDMAPI = true;
    bVelocitySkip = false;
    iSkipLagMs = 100;
    bVelocityFloor = false;
    iMinVelocity = 20;
    bCollapseStacks = false;
    bIgnoreWeird = false;
    bSendLimit = false;
    iMaxSendsPerSec = 2000000;
    bCompactVisual = false;
    iBlockCacheMB = 64;
    bLodCache = true;
    bGpuInstancing = false;
    bShowLoadStats = true;
}

void PerformanceSettings::Load( const IniFile &ini )
{
    iWorkerThreads = std::clamp( ini.GetInt( "Performance", "WorkerThreads", iWorkerThreads ), 0, 256 );
    bPreferKDMAPI = ini.GetBool( "Performance", "PreferKDMAPI", bPreferKDMAPI );
    bVelocitySkip = ini.GetBool( "Performance", "VelocitySkip", bVelocitySkip );
    iSkipLagMs = std::clamp( ini.GetInt( "Performance", "SkipLagMs", iSkipLagMs ), 1, 10000 );
    bVelocityFloor = ini.GetBool( "Performance", "VelocityFloor", bVelocityFloor );
    iMinVelocity = std::clamp( ini.GetInt( "Performance", "MinVelocity", iMinVelocity ), 1, 128 );
    bCollapseStacks = ini.GetBool( "Performance", "CollapseStacks", bCollapseStacks );
    bIgnoreWeird = ini.GetBool( "Performance", "IgnoreWeirdNotes", bIgnoreWeird );
    bSendLimit = ini.GetBool( "Performance", "SendLimit", bSendLimit );
    iMaxSendsPerSec = std::clamp( ini.GetInt( "Performance", "MaxSendsPerSec", iMaxSendsPerSec ), 1000, 1000000000 );
    bCompactVisual = ini.GetBool( "Performance", "CompactVisual", bCompactVisual );
    iBlockCacheMB = std::clamp( ini.GetInt( "Performance", "BlockCacheMB", iBlockCacheMB ), 16, 65536 );
    bLodCache = ini.GetBool( "Performance", "LodCache", bLodCache );
    bGpuInstancing = ini.GetBool( "Performance", "GpuInstancing", bGpuInstancing );
    bShowLoadStats = ini.GetBool( "Performance", "ShowLoadStats", bShowLoadStats );
}

void PerformanceSettings::Save( IniFile &ini ) const
{
    ini.SetInt( "Performance", "WorkerThreads", iWorkerThreads );
    ini.SetInt( "Performance", "PreferKDMAPI", bPreferKDMAPI );
    ini.SetInt( "Performance", "VelocitySkip", bVelocitySkip );
    ini.SetInt( "Performance", "SkipLagMs", iSkipLagMs );
    ini.SetInt( "Performance", "VelocityFloor", bVelocityFloor );
    ini.SetInt( "Performance", "MinVelocity", iMinVelocity );
    ini.SetInt( "Performance", "CollapseStacks", bCollapseStacks );
    ini.SetInt( "Performance", "IgnoreWeirdNotes", bIgnoreWeird );
    ini.SetInt( "Performance", "SendLimit", bSendLimit );
    ini.SetInt( "Performance", "MaxSendsPerSec", iMaxSendsPerSec );
    ini.SetInt( "Performance", "CompactVisual", bCompactVisual );
    ini.SetInt( "Performance", "BlockCacheMB", iBlockCacheMB );
    ini.SetInt( "Performance", "LodCache", bLodCache );
    ini.SetInt( "Performance", "GpuInstancing", bGpuInstancing );
    ini.SetInt( "Performance", "ShowLoadStats", bShowLoadStats );
}

//-----------------------------------------------------------------------------
// Playback
//-----------------------------------------------------------------------------

void PlaybackSettings::LoadDefaultValues()
{
    m_ePlayMode = PlayMode::Splash;
    m_bMute = false;
    m_bPlayable = false;
    m_bPaused = true;
    m_dSpeed = 1.0;
    m_dNSpeed = 0.06;
    m_dVolume = 1.0;
    m_eMetronome = Off;
}

void PlaybackSettings::Load( const IniFile &ini )
{
    m_bMute = ini.GetBool( "Playback", "Mute", m_bMute );
    m_dNSpeed = std::clamp( ini.GetDouble( "Playback", "NoteSpeed", m_dNSpeed ), 0.005, 10.0 );
    m_dVolume = std::clamp( ini.GetDouble( "Playback", "Volume", m_dVolume ), 0.0, 1.0 );
    m_eMetronome = static_cast< Metronome >( std::clamp( ini.GetInt( "Playback", "Metronome", m_eMetronome ), 0, 2 ) );
}

void PlaybackSettings::Save( IniFile &ini ) const
{
    ini.SetInt( "Playback", "Mute", m_bMute );
    ini.SetDouble( "Playback", "Volume", m_dVolume );
    ini.SetDouble( "Playback", "NoteSpeed", m_dNSpeed );
    ini.SetInt( "Playback", "Metronome", m_eMetronome );
}

void PlaybackSettings::SetPosition( int iPosition ) { ::SetPosition( iPosition ); }
void PlaybackSettings::SetPlayMode( PlayMode::State e, bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetPlayMode( e ); m_ePlayMode = e; }
void PlaybackSettings::SetPlayable( bool b, bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetPlayable( b ); m_bPlayable = b; }
void PlaybackSettings::SetPaused( bool b, bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetPlayPauseStop( !b, b, false ); m_bPaused = b; }
void PlaybackSettings::SetStopped( bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetPlayPauseStop( false, false, true ); m_bPaused = true; }
void PlaybackSettings::SetSpeed( double d, bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetSpeed( d ); m_dSpeed = d; }
void PlaybackSettings::SetNSpeed( double d, bool bUpdateGUI ) { d = std::max( std::min( d, 10.0 ), 0.005 ); if ( bUpdateGUI ) ::SetNSpeed( d ); m_dNSpeed = d; }
void PlaybackSettings::SetVolume( double d, bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetVolume( d ); m_dVolume = d; }
void PlaybackSettings::SetMute( bool b, bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetMute( b ); m_bMute = b; }
void PlaybackSettings::SetMetronome( Metronome e, bool bUpdateGUI ) { if ( bUpdateGUI ) ::SetMetronome( e ); m_eMetronome = e; }

//-----------------------------------------------------------------------------
// View
//-----------------------------------------------------------------------------

void ViewSettings::LoadDefaultValues()
{
    m_bControls = true;
    m_bKeyboard = true;
    m_bOnTop = false;
    m_bFullScreen = false;
    m_bZoomMove = false;
    m_fOffsetX = 0.0f;
    m_fOffsetY = 0.0f;
    m_fZoomX = 1.0f;
    m_iMainLeft = CW_USEDEFAULT;
    m_iMainTop = CW_USEDEFAULT;
    m_iMainWidth = 960;
    m_iMainHeight = 589;
}

void ViewSettings::Load( const IniFile &ini )
{
    m_bControls = ini.GetBool( "View", "Controls", m_bControls );
    m_bKeyboard = ini.GetBool( "View", "Keyboard", m_bKeyboard );
    m_bOnTop = ini.GetBool( "View", "OnTop", m_bOnTop );
    m_fOffsetX = (float)ini.GetDouble( "View", "OffsetX", m_fOffsetX );
    m_fOffsetY = (float)ini.GetDouble( "View", "OffsetY", m_fOffsetY );
    m_fZoomX = (float)ini.GetDouble( "View", "ZoomX", m_fZoomX );
    m_iMainLeft = ini.GetInt( "View", "MainLeft", m_iMainLeft );
    m_iMainTop = ini.GetInt( "View", "MainTop", m_iMainTop );
    m_iMainWidth = ini.GetInt( "View", "MainWidth", m_iMainWidth );
    m_iMainHeight = ini.GetInt( "View", "MainHeight", m_iMainHeight );
}

void ViewSettings::Save( IniFile &ini ) const
{
    ini.SetInt( "View", "Controls", m_bControls );
    ini.SetInt( "View", "Keyboard", m_bKeyboard );
    ini.SetInt( "View", "OnTop", m_bOnTop );
    ini.SetDouble( "View", "OffsetX", m_fOffsetX );
    ini.SetDouble( "View", "OffsetY", m_fOffsetY );
    ini.SetDouble( "View", "ZoomX", m_fZoomX );
    ini.SetInt( "View", "MainLeft", m_iMainLeft );
    ini.SetInt( "View", "MainTop", m_iMainTop );
    ini.SetInt( "View", "MainWidth", m_iMainWidth );
    ini.SetInt( "View", "MainHeight", m_iMainHeight );
}

void ViewSettings::SetControls( bool b, bool bUpdateGUI ) { m_bControls = b; if ( bUpdateGUI ) ::ShowControls( b ); }
void ViewSettings::SetKeyboard( bool b, bool bUpdateGUI ) { m_bKeyboard = b; if ( bUpdateGUI ) ::ShowKeyboard( b ); }
void ViewSettings::SetOnTop( bool b, bool bUpdateGUI ) { m_bOnTop = b; if ( bUpdateGUI ) ::SetOnTop( b ); }
void ViewSettings::SetFullScreen( bool b, bool bUpdateGUI ) { m_bFullScreen = b; if ( bUpdateGUI ) ::SetFullScreen( b ); }
void ViewSettings::SetZoomMove( bool b, bool bUpdateGUI ) { m_bZoomMove = b; if ( bUpdateGUI ) ::SetZoomMove( b ); }
