// Port of the original GameState.cpp (Brian Pantano, 2010). Rendering code is kept expression for
// expression so float rounding matches the original (DESIGN.md R6, R10, R11).
#include "game/GameState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/MainWindow.h"
#include "app/VideoExport.h"
#include "core/Config.h"
#include "midi/Notes.h"
#include "resource.h"

using std::max;
using std::min;
using std::floor;
using std::pow;

const wchar_t *const GameState::Errors[] =
{
    L"Success.",
    L"Invalid pointer passed.",
    L"Out of memory.",
    L"Error calling Direct3D.",
};

void GameState::RenderVideo( const std::shared_ptr< VideoJob > &pJob )
{
    pJob->Finish( false, L"Please open a song first." );
}

GameState::GameError GameState::ChangeState( GameState *pNextState, GameState **pDestObj )
{
    if ( !pNextState ) return Success;
    if ( !pDestObj ) return BadPointer;

    if ( *pDestObj )
    {
        if ( !pNextState->m_hWnd ) pNextState->m_hWnd = ( *pDestObj )->m_hWnd;
        if ( !pNextState->m_pRenderer ) pNextState->m_pRenderer = ( *pDestObj )->m_pRenderer;
        delete *pDestObj;
    }
    *pDestObj = pNextState;
    GameError iResult = pNextState->Init();
    if ( iResult )
    {
        *pDestObj = new IntroScreen( pNextState->m_hWnd, pNextState->m_pRenderer );
        delete pNextState;
        ( *pDestObj )->Init();
        return iResult;
    }
    return Success;
}

//-----------------------------------------------------------------------------
// IntroScreen
//-----------------------------------------------------------------------------

GameState::GameError IntroScreen::MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( msg == WM_COMMAND )
    {
        switch ( LOWORD( wParam ) )
        {
            case ID_CHANGESTATE: m_pNextState = reinterpret_cast< GameState * >( lParam ); return Success;
            case ID_VIEW_RESETDEVICE: m_pRenderer->ResetDevice(); return Success;
        }
    }
    return Success;
}

GameState::GameError IntroScreen::Logic()
{
    Sleep( 10 );
    return Success;
}

GameState::GameError IntroScreen::Render()
{
    if ( FAILED( m_pRenderer->ResetDeviceIfNeeded() ) ) return DirectXError;
    m_pRenderer->Clear( D3DCOLOR_XRGB( 0, 0, 0 ) );
    m_pRenderer->BeginScene();
    m_pRenderer->DrawRect( 0.0f, 0.0f, static_cast< float >( m_pRenderer->GetBufferWidth() ),
                           static_cast< float >( m_pRenderer->GetBufferHeight() ), 0x00000000 );
    m_pRenderer->EndScene();
    m_pRenderer->Present();
    return Success;
}

//-----------------------------------------------------------------------------
// SplashScreen
//-----------------------------------------------------------------------------

const float SplashScreen::SharpRatio = 0.65f;

SplashScreen::SplashScreen( HWND hWnd, Renderer *pRenderer ) : GameState( hWnd, pRenderer )
{
    HRSRC hResInfo = FindResourceW( nullptr, MAKEINTRESOURCEW( IDR_SPLASHMIDI ), L"MIDI" );
    HGLOBAL hRes = LoadResource( nullptr, hResInfo );
    DWORD iSize = SizeofResource( nullptr, hResInfo );
    const unsigned char *pData = ( const unsigned char * )LockResource( hRes );
    if ( pData ) m_MIDI.Parse( pData, iSize );

    m_vTrackSettings.resize( m_MIDI.GetInfo().iNumTracks );
    m_vState.reserve( 128 );
    InitState();
}

void SplashScreen::InitState()
{
    static Config &config = Config::GetConfig();
    static const PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static const VisualSettings &cVisual = config.GetVisualSettings();

    m_iStartPos = 0;
    m_iEndPos = -1;
    m_llStartTime = m_MIDI.GetInfo().llFirstNote - 3000000;
    m_bPaused = cPlayback.GetPaused();
    m_bMute = cPlayback.GetMute();
    m_dVolume = cPlayback.GetVolume();

    SetChannelColors( std::vector< unsigned >( cVisual.colors, cVisual.colors + 16 ) );

    m_OutDevice.OpenConfigured();
    m_OutDevice.SetVolume( 1.0 );
}

void SplashScreen::SetChannelColors( const std::vector< unsigned > &vColor )
{
    const std::vector< RefTrackInfo > &vTracks = m_MIDI.GetTracks();
    bool bLoop = Config::GetConfig().GetVisualSettings().bLoopColors;
    size_t iPos = 0;
    for ( size_t i = 0; i < vTracks.size(); i++ )
        for ( int j = 0; j < 16; j++ )
            if ( vTracks[i].aNoteCount[j] > 0 )
            {
                if ( iPos < vColor.size() || ( bLoop && !vColor.empty() ) )
                    m_vTrackSettings[i].aChannels[j].SetColor( vColor[iPos % vColor.size()] );
                else
                    m_vTrackSettings[i].aChannels[j].SetColor();
                iPos++;
            }
}

GameState::GameError SplashScreen::MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();

    switch ( msg )
    {
        case WM_COMMAND:
            switch ( LOWORD( wParam ) )
            {
                case ID_CHANGESTATE: m_pNextState = reinterpret_cast< GameState * >( lParam ); return Success;
                case ID_VIEW_RESETDEVICE: m_pRenderer->ResetDevice(); return Success;
            }
            break;
        case WM_DEVICECHANGE:
            if ( m_OutDevice.GetDevice() != MidiOutputs::ConfiguredName() )
                m_OutDevice.OpenConfigured();
            break;
        case WM_KEYDOWN:
            if ( wParam == VK_SPACE ) cPlayback.TogglePaused( true );
            return Success;
    }
    return Success;
}

GameState::GameError SplashScreen::Logic()
{
    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    const RefMidiInfo &mInfo = m_MIDI.GetInfo();
    const std::vector< RefChannelEvent > &vEvents = m_MIDI.GetEvents();

    bool bPaused = cPlayback.GetPaused();
    bool bMute = cPlayback.GetMute();
    bool bMuteChanged = ( bMute != m_bMute );
    bool bPausedChanged = ( bPaused != m_bPaused );
    m_bMute = bMute;
    m_bPaused = bPaused;
    m_dVolume = cPlayback.GetVolume();

    double dMaxCorrect = ( mInfo.iMaxVolume > 0 ? 127.0 / mInfo.iMaxVolume : 1.0 );
    double dVolumeCorrect = ( mInfo.llVolumeSum > 0 ? ( m_dVolume * 127.0 * mInfo.llNoteCount ) / mInfo.llVolumeSum : 1.0 );
    dVolumeCorrect = min( dVolumeCorrect, dMaxCorrect );

    long long llMaxTime = mInfo.llTotalMicroSecs + 500000;
    long long llElapsed = m_Timer.GetMicroSecs();
    m_Timer.Start();

    if ( ( bPausedChanged || bMuteChanged ) && ( m_bPaused || m_bMute ) )
        m_OutDevice.AllNotesOff();

    long long llNextStartTime = m_llStartTime + llElapsed;
    if ( !bPaused && m_llStartTime < llMaxTime )
        m_llStartTime = llNextStartTime;
    long long llEndTime = m_llStartTime + TimeSpan;

    RenderGlobals();

    int iEventCount = (int)vEvents.size();
    while ( m_iEndPos + 1 < iEventCount && vEvents[m_iEndPos + 1].llMicroSec < llEndTime )
        m_iEndPos++;

    while ( m_iStartPos < iEventCount && vEvents[m_iStartPos].llMicroSec <= m_llStartTime )
    {
        const RefChannelEvent &e = vEvents[m_iStartPos];
        if ( e.GetType() != RefChannelEvent::NoteOn )
            m_OutDevice.PlayEvent( e.cCode, e.cParam1, e.cParam2 );
        else if ( !m_bMute && !m_vTrackSettings[e.iTrack].aChannels[e.GetChannel()].bMuted )
            m_OutDevice.PlayEvent( e.cCode, e.cParam1, static_cast< int >( e.cParam2 * dVolumeCorrect + 0.5 ) );
        UpdateState( m_iStartPos );
        m_iStartPos++;
    }
    return Success;
}

void SplashScreen::UpdateState( int iPos )
{
    const std::vector< RefChannelEvent > &vEvents = m_MIDI.GetEvents();
    const RefChannelEvent &e = vEvents[iPos];
    if ( e.iSister < 0 ) return;

    if ( e.IsNoteOnVel() )
        m_vState.push_back( iPos );
    else
    {
        int iSearch = e.iSister;
        auto it = m_vState.begin();
        while ( it != m_vState.end() )
        {
            if ( *it == iSearch ) it = m_vState.erase( it );
            else ++it;
        }
    }
}

GameState::GameError SplashScreen::Render()
{
    if ( FAILED( m_pRenderer->ResetDeviceIfNeeded() ) ) return DirectXError;
    m_pRenderer->Clear( D3DCOLOR_XRGB( 0, 0, 0 ) );
    m_pRenderer->BeginScene();
    m_pRenderer->DrawRect( 0.0f, 0.0f, static_cast< float >( m_pRenderer->GetBufferWidth() ),
                           static_cast< float >( m_pRenderer->GetBufferHeight() ), 0x00000000 );
    RenderNotes();
    m_pRenderer->EndScene();
    m_pRenderer->Present();
    return Success;
}

void SplashScreen::RenderGlobals()
{
    const RefMidiInfo &mInfo = m_MIDI.GetInfo();
    m_iStartNote = mInfo.iMinNote;
    m_iEndNote = mInfo.iMaxNote;

    m_fNotesX = 0.0f;
    m_fNotesCX = static_cast< float >( m_pRenderer->GetBufferWidth() );
    m_fNotesY = 0.0f;
    m_fNotesCY = static_cast< float >( m_pRenderer->GetBufferHeight() );

    m_iAllWhiteKeys = Notes::WhiteCount( m_iStartNote, m_iEndNote + 1 );
    float fBuffer = ( Notes::IsSharp( m_iStartNote ) ? SharpRatio / 2.0f : 0.0f ) +
                    ( Notes::IsSharp( m_iEndNote ) ? SharpRatio / 2.0f : 0.0f );
    m_fWhiteCX = m_fNotesCX / ( m_iAllWhiteKeys + fBuffer );

    long long llMicroSecsPP = static_cast< long long >( TimeSpan / m_fNotesCY + 0.5f );
    m_llRndStartTime = m_llStartTime - ( m_llStartTime < 0 ? llMicroSecsPP : 0 );
    m_llRndStartTime = ( m_llRndStartTime / llMicroSecsPP ) * llMicroSecsPP;
}

void SplashScreen::RenderNotes()
{
    const std::vector< RefChannelEvent > &vEvents = m_MIDI.GetEvents();
    if ( m_iEndPos < 0 || m_iStartPos >= static_cast< int >( vEvents.size() ) )
        return;

    bool bHasSharp = false;
    for ( int iPos : m_vState )
        if ( !Notes::IsSharp( vEvents[iPos].cParam1 ) ) RenderNote( iPos );
        else bHasSharp = true;

    for ( int i = m_iStartPos; i <= m_iEndPos; i++ )
    {
        const RefChannelEvent &e = vEvents[i];
        if ( e.IsNoteOnVel() && e.iSister >= 0 )
        {
            if ( !Notes::IsSharp( e.cParam1 ) ) RenderNote( i );
            else bHasSharp = true;
        }
    }

    if ( bHasSharp )
    {
        for ( int iPos : m_vState )
            if ( Notes::IsSharp( vEvents[iPos].cParam1 ) ) RenderNote( iPos );
        for ( int i = m_iStartPos; i <= m_iEndPos; i++ )
        {
            const RefChannelEvent &e = vEvents[i];
            if ( e.IsNoteOnVel() && e.iSister >= 0 && Notes::IsSharp( e.cParam1 ) )
                RenderNote( i );
        }
    }
}

void SplashScreen::RenderNote( int iPos )
{
    const std::vector< RefChannelEvent > &vEvents = m_MIDI.GetEvents();
    const RefChannelEvent &note = vEvents[iPos];
    int iNote = note.cParam1;
    long long llNoteStart = note.llMicroSec;
    long long llNoteEnd = vEvents[note.iSister].llMicroSec;

    ChannelSettings &csTrack = m_vTrackSettings[note.iTrack].aChannels[note.GetChannel()];
    if ( csTrack.bHidden ) return;

    float x = GetNoteX( iNote );
    float y = m_fNotesY + m_fNotesCY * ( 1.0f - static_cast< float >( llNoteStart - m_llRndStartTime ) / TimeSpan );
    float cx = Notes::IsSharp( iNote ) ? m_fWhiteCX * SharpRatio : m_fWhiteCX;
    float cy = m_fNotesCY * ( static_cast< float >( llNoteEnd - llNoteStart ) / TimeSpan );
    float fDeflate = m_fWhiteCX * 0.15f / 2.0f;

    cy = floor( cy + 0.5f );
    y = floor( y + 0.5f );
    fDeflate = floor( fDeflate + 0.5f );
    fDeflate = max( min( fDeflate, 3.0f ), 1.0f );

    float fMinY = m_fNotesY - 5.0f;
    float fMaxY = m_fNotesY + m_fNotesCY + 5.0f;
    if ( y > fMaxY )
    {
        cy -= ( y - fMaxY );
        y = fMaxY;
    }
    if ( y - cy < fMinY )
    {
        cy -= ( fMinY - ( y - cy ) );
        y = fMinY + cy;
    }

    long long llDuration = m_llStartTime - ( m_MIDI.GetInfo().llFirstNote - 3000000 );
    int iAlpha = 0xFF - static_cast< int >( ( 0xFF * min( 1500000LL, llDuration ) ) / 1500000 );
    int iAlpha1 = static_cast< int >( ( 0xFF * ( m_fNotesCY - y ) / m_fNotesCY ) + 0.5f );
    int iAlpha2 = static_cast< int >( ( 0xFF * ( m_fNotesCY - ( y + cy ) ) / m_fNotesCY ) + 0.5f );
    iAlpha1 = max( iAlpha1, 0 );
    iAlpha2 = min( iAlpha1, 0xFF ); // sic: the original's bug, kept (DESIGN.md R11)
    (void)iAlpha2;
    unsigned uAlpha = (unsigned)iAlpha << 24;
    unsigned uAlpha1 = (unsigned)iAlpha1 << 24;
    unsigned uAlpha2 = (unsigned)iAlpha2 << 24;
    m_pRenderer->DrawRect( x, y - cy, cx, cy, csTrack.iVeryDarkRGB | uAlpha );
    m_pRenderer->DrawRect( x + fDeflate, y - cy + fDeflate,
                           cx - fDeflate * 2.0f, cy - fDeflate * 2.0f,
                           csTrack.iPrimaryRGB | uAlpha1, csTrack.iDarkRGB | uAlpha1, csTrack.iDarkRGB | uAlpha2, csTrack.iPrimaryRGB | uAlpha2 );
}

float SplashScreen::GetNoteX( int iNote )
{
    int iWhiteKeys = Notes::WhiteCount( m_iStartNote, iNote );
    float fStartX = ( Notes::IsSharp( m_iStartNote ) - Notes::IsSharp( iNote ) ) * SharpRatio / 2.0f;
    if ( Notes::IsSharp( iNote ) )
    {
        Notes::Note eNote = Notes::NoteVal( iNote );
        if ( eNote == Notes::CS || eNote == Notes::FS ) fStartX -= SharpRatio / 5.0f;
        else if ( eNote == Notes::AS || eNote == Notes::DS ) fStartX += SharpRatio / 5.0f;
    }
    return m_fNotesX + m_fWhiteCX * ( iWhiteKeys + fStartX );
}

//-----------------------------------------------------------------------------
// TextPath
//-----------------------------------------------------------------------------

void TextPath::Logic( long long llElapsed )
{
    float fElapsed = llElapsed / 1000000.0f;
    m_t += fElapsed;

    int iSize = (int)m_vPath.size();
    while ( m_iPos < iSize - 1 && m_vPath[m_iPos + 1].t < m_t )
        m_iPos++;
    if ( m_iPos >= iSize - 1 ) return;

    const TextPathVertex &curr = m_vPath[m_iPos];
    const TextPathVertex &next = m_vPath[m_iPos + 1];
    m_x = curr.x + ( next.x - curr.x ) * ( m_t - curr.t ) / ( next.t - curr.t );
    m_y = curr.y + ( next.y - curr.y ) * ( m_t - curr.t ) / ( next.t - curr.t );
    int a = curr.a + static_cast< int >( ( next.a - curr.a ) * ( m_t - curr.t ) / ( next.t - curr.t ) + 0.5f );
    m_iColor = ( m_iColor & 0x00FFFFFF ) | ( (unsigned)a << 24 );
}

void TextPath::Render( Renderer *pRenderer, float xOffset, float yOffset )
{
    if ( !IsAlive() ) return;

    RECT rcPos = { 0 };
    rcPos.left = static_cast< long >( m_x + m_xOffset + xOffset );
    rcPos.top = static_cast< long >( m_y + m_yOffset + yOffset );
    rcPos.right = rcPos.left;
    rcPos.bottom = rcPos.top;

    const int iShift = ( m_fFont == Renderer::Large || m_fFont == Renderer::Medium ? -2 : 1 );
    OffsetRect( &rcPos, -iShift, -iShift );
    pRenderer->DrawText( m_sText, m_fFont, &rcPos, DT_CENTER | DT_NOCLIP, 0xFF000000 & m_iColor );
    OffsetRect( &rcPos, iShift, iShift );
    pRenderer->DrawText( m_sText, m_fFont, &rcPos, DT_CENTER | DT_NOCLIP, m_iColor );
}

