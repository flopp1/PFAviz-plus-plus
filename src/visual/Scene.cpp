// Ported from the original GameState.cpp (Brian Pantano, 2010), expression for expression.
#include "visual/Scene.h"

#include <algorithm>
#include <cmath>

#include "core/Util.h"
#include "midi/Notes.h"

using std::floor;
using std::max;
using std::min;

const float Scene::SharpRatio = 0.65f;
const float Scene::KBPercent = 0.25f;
const float Scene::KeyRatio = 0.1775f;

namespace
{
    // The 31-EDO layout's keys, by step in the octave: left edge and width in sevenths of a white key
    // from the octave's C, and kind. A half sharp or flat is 3/7 wide, ending (half sharp) or starting
    // (half flat) at the edge between two white keys; a full sharp or flat 2/7 the same way, over them.
    // Between E and F, and B and C, there are only the halves
    enum { Natural, Half, Sharp, Flat };
    struct Key31 { unsigned char x, cx, kind; };
    const Key31 aKeys31[31] = {
        {  0, 7, Natural }, {  4, 3, Half }, {  5, 2, Sharp }, {  7, 2, Flat }, {  7, 3, Half },     // C C‡ C# Db Dd
        {  7, 7, Natural }, { 11, 3, Half }, { 12, 2, Sharp }, { 14, 2, Flat }, { 14, 3, Half },     // D
        { 14, 7, Natural }, { 18, 3, Half }, { 21, 3, Half },                                         // E E‡ Fd
        { 21, 7, Natural }, { 25, 3, Half }, { 26, 2, Sharp }, { 28, 2, Flat }, { 28, 3, Half },     // F
        { 28, 7, Natural }, { 32, 3, Half }, { 33, 2, Sharp }, { 35, 2, Flat }, { 35, 3, Half },     // G
        { 35, 7, Natural }, { 39, 3, Half }, { 40, 2, Sharp }, { 42, 2, Flat }, { 42, 3, Half },     // A
        { 42, 7, Natural }, { 46, 3, Half }, { 49, 3, Half },                                         // B B‡ Cd
    };
    const int Octave31 = 49;            // sevenths of a white key
    const int Whites31 = 58;            // 8 octaves and C, D: the last key, 255, is the D# ending at D's edge
    const int MiddleC31 = 124;          // the mark: the 29th of the 58 white keys (C of the fifth octave), left of the exact centre

    inline const Key31 &Key31Of( int iNote ) { return aKeys31[( iNote % 31 + 31 ) % 31]; }
    // A key's left edge in white keys from key 0's
    inline float Left31( int iNote )
    {
        int iOct = iNote >= 0 ? iNote / 31 : -( ( 30 - iNote ) / 31 );
        return ( iOct * Octave31 + Key31Of( iNote ).x ) / 7.0f;
    }
}

//-----------------------------------------------------------------------------
// Colours
//-----------------------------------------------------------------------------

void ChannelSettings::SetColor()
{
    SetColor( Util::RandColor(), 0.6, 0.2 );
}

// Flips around windows format (ABGR) -> direct x format (ARGB)
void ChannelSettings::SetColor( unsigned int iColor, double dDark, double dVeryDark )
{
    int R = ( iColor >> 0 ) & 0xFF, dR, vdR;
    int G = ( iColor >> 8 ) & 0xFF, dG, vdG;
    int B = ( iColor >> 16 ) & 0xFF, dB, vdB;
    int A = ( iColor >> 24 ) & 0xFF;

    int H, S, V;
    Util::RGBtoHSV( R, G, B, H, S, V );
    Util::HSVtoRGB( H, S, min( 100, static_cast< int >( V * dDark ) ), dR, dG, dB );
    Util::HSVtoRGB( H, S, min( 100, static_cast< int >( V * dVeryDark ) ), vdR, vdG, vdB );

    iOrigBGR = iColor;
    iPrimaryRGB = ( A << 24 ) | ( R << 16 ) | ( G << 8 ) | ( B << 0 );
    iDarkRGB = ( A << 24 ) | ( dR << 16 ) | ( dG << 8 ) | ( dB << 0 );
    iVeryDarkRGB = ( A << 24 ) | ( vdR << 16 ) | ( vdG << 8 ) | ( vdB << 0 );
}

SceneColors::SceneColors()
{
    csBackground.SetColor( 0x00464646, 0.7f, 1.3f );
    csKBBackground.SetColor( 0x00999999, 0.4f, 0.0f );
    csKBRed.SetColor( 0x000D0A98, 0.5f );
    csKBWhite.SetColor( 0x00FFFFFF, 0.8f, 0.6f );
    csKBSharp.SetColor( 0x00404040, 0.5f, 0.0f );
    // As csKBSharp's shading (the top's dark band and highlight), lighter: the top shows the very dark
    csKBHalf.SetColor( 0x00E0E0E0, 0.75f, 0.78f );
    csKBSharp31.SetColor( 0x00909090, 0.55f, 0.6f );
}

void SceneColors::SetBackground( unsigned int iBkgColor )
{
    if ( iBkgColor != csBackground.iOrigBGR ) csBackground.SetColor( iBkgColor, 0.7f, 1.3f );
}

//-----------------------------------------------------------------------------
// Globals and notes
//-----------------------------------------------------------------------------

FrameGlobals Scene::ComputeGlobals( const ViewInputs &in )
{
    FrameGlobals g;
    g.iBufferWidth = in.iBufferWidth;
    g.iBufferHeight = in.iBufferHeight;
    g.llStartTime = in.llStartTime;
    g.llTimeSpan = in.llTimeSpan;
    g.uClean = in.uClean;
    g.bLines = in.bLines;

    g.iStartNote = min( in.iFirstKey, in.iLastKey );
    g.iEndNote = max( in.iFirstKey, in.iLastKey );
    if ( in.eKeysShown == 0 ) // All
    {
        g.iStartNote = min( g.iStartNote, Notes::A0 );
        g.iEndNote = max( g.iEndNote, Notes::C8 );
    }
    else if ( in.eKeysShown == 1 ) // Song
    {
        g.iStartNote = in.iSongMinNote;
        g.iEndNote = in.iSongMaxNote;
    }
    else if ( in.eKeysShown == 3 ) // All 128 MIDI keys (PFAviz++ addition)
    {
        g.iStartNote = 0;
        g.iEndNote = 127;
    }
    else if ( in.eKeysShown == 4 ) // All 256 keys (31-EDO): the mark on the middle white key (74 on each side)
    {
        g.iStartNote = 0;
        g.iEndNote = 255;
        g.iMiddleC = 127;
    }

    // Screen X info
    g.fNotesX = in.fOffsetX;
    g.fNotesCX = in.iBufferWidth * in.fZoomX;

    // 31-EDO layout: from key 0's left edge to key 255's right one, D's (no buffer)
    g.bEdo31 = in.bEdo31 && in.eKeysShown == 4;
    if ( g.bEdo31 )
    {
        g.iMiddleC = MiddleC31;
        g.iAllWhiteKeys = Whites31;
        g.fWhiteCX = g.fNotesCX / Whites31;
    }
    // Keys info
    else
    {
        g.iAllWhiteKeys = Notes::WhiteCount( g.iStartNote, g.iEndNote + 1 );
        // Key 255 (D#) is the last there is: the field ends at its middle (its nudge right of the white key
        // edge) instead of half a sharp past it, so no part of the key after it is shown
        float fBuffer = ( Notes::IsSharp( g.iStartNote ) ? SharpRatio / 2.0f : 0.0f ) +
                        ( g.iEndNote == 255 ? SharpRatio / 5.0f : Notes::IsSharp( g.iEndNote ) ? SharpRatio / 2.0f : 0.0f );
        g.fWhiteCX = g.fNotesCX / ( g.iAllWhiteKeys + fBuffer );
    }

    // Screen Y info
    g.fNotesY = in.fOffsetY;
    if ( !in.bShowKB )
        g.fNotesCY = static_cast< float >( in.iBufferHeight );
    else
    {
        float fMaxKeyCY = in.iBufferHeight * KBPercent;
        float fIdealKeyCY = g.fWhiteCX / KeyRatio;
        // .95 for the top vs near. 2.0 for the spacer. .93 for the transition and the red. ESTIMATE.
        fIdealKeyCY = ( fIdealKeyCY / 0.95f + 2.0f ) / 0.93f;
        g.fNotesCY = floor( in.iBufferHeight - min( fIdealKeyCY, fMaxKeyCY ) + 0.5f );
    }

    // Round down start time. This is only used for rendering purposes
    long long llMicroSecsPP = static_cast< long long >( in.llTimeSpan / g.fNotesCY + 0.5f );
    if ( llMicroSecsPP < 1 ) llMicroSecsPP = 1;
    g.llRndStartTime = in.llStartTime - ( in.llStartTime < 0 ? llMicroSecsPP : 0 );
    g.llRndStartTime = ( g.llRndStartTime / llMicroSecsPP ) * llMicroSecsPP;
    // Clean drawing: rounded up instead (division truncates towards 0, up for negative times)
    if ( in.uClean & Clean::KeepBottom )
        g.llRndStartTime = ( in.llStartTime >= 0 ? ( in.llStartTime + llMicroSecsPP - 1 ) / llMicroSecsPP : in.llStartTime / llMicroSecsPP ) * llMicroSecsPP;
    return g;
}

int Scene::KeyLayer( const FrameGlobals &g, int iNote )
{
    if ( !g.bEdo31 ) return Notes::IsSharp( iNote ) ? 1 : 0;
    int k = Key31Of( iNote ).kind;
    return k == Natural ? 0 : k == Half ? 1 : 2;
}

float Scene::GetNoteX( const FrameGlobals &g, int iNote )
{
    if ( g.bEdo31 ) return g.fNotesX + g.fWhiteCX * Left31( iNote );
    int iWhiteKeys = Notes::WhiteCount( g.iStartNote, iNote );
    float fStartX = ( Notes::IsSharp( g.iStartNote ) - Notes::IsSharp( iNote ) ) * SharpRatio / 2.0f;
    if ( Notes::IsSharp( iNote ) )
    {
        Notes::Note eNote = Notes::NoteVal( iNote );
        if ( eNote == Notes::CS || eNote == Notes::FS ) fStartX -= SharpRatio / 5.0f;
        else if ( eNote == Notes::AS || eNote == Notes::DS ) fStartX += SharpRatio / 5.0f;
    }
    return g.fNotesX + g.fWhiteCX * ( iWhiteKeys + fStartX );
}

void Scene::DrawNote( Renderer *pRenderer, const FrameGlobals &g, int iNote, long long llNoteStart, long long llNoteEnd,
                      const ChannelSettings &csTrack )
{
    Renderer::SCREEN_VERTEX aVerts[12];
    NoteVertices( g, iNote, llNoteStart, llNoteEnd, csTrack, aVerts );
    pRenderer->DrawVertices( aVerts, 12 );
}

void Scene::NoteVertices( const FrameGlobals &g, int iNote, long long llNoteStart, long long llNoteEnd,
                          const ChannelSettings &csTrack, Renderer::SCREEN_VERTEX *pOut )
{
    NoteVertices( g, GetNoteKey( g, iNote ), llNoteStart, llNoteEnd, csTrack, pOut );
}

Scene::NoteKey Scene::GetNoteKey( const FrameGlobals &g, int iNote )
{
    NoteKey nk;
    nk.x = GetNoteX( g, iNote );
    nk.cx = g.bEdo31 ? g.fWhiteCX * ( Key31Of( iNote ).cx / 7.0f ) : Notes::IsSharp( iNote ) ? g.fWhiteCX * SharpRatio : g.fWhiteCX;
    float fDeflate = g.fWhiteCX * 0.15f / 2.0f;
    fDeflate = floor( fDeflate + 0.5f );
    nk.fDeflate = max( min( fDeflate, 3.0f ), 1.0f );
    return nk;
}

void Scene::NoteVertices( const FrameGlobals &g, const NoteKey &nk, long long llNoteStart, long long llNoteEnd,
                          const ChannelSettings &csTrack, Renderer::SCREEN_VERTEX *pOut )
{
    // Compute true positions
    const float x = nk.x, cx = nk.cx, fDeflate = nk.fDeflate;
    float y = g.fNotesY + g.fNotesCY * ( 1.0f - static_cast< float >( llNoteStart - g.llRndStartTime ) / g.llTimeSpan );
    float cy = g.fNotesCY * ( static_cast< float >( llNoteEnd - llNoteStart ) / g.llTimeSpan );

    // Rounding to make everything consistent
    cy = floor( cy + 0.5f ); // constant cy across rendering
    y = floor( y + 0.5f );
    // Clean drawing: the top from the end time, rounded as the bottom is (FastEngine::Coverage too)
    if ( g.uClean & Clean::EdgeRound )
        cy = y - floor( g.fNotesY + g.fNotesCY * ( 1.0f - static_cast< float >( llNoteEnd - g.llRndStartTime ) / g.llTimeSpan ) + 0.5f );
    if ( ( g.uClean & Clean::MinHeight ) && cy < 1.0f ) cy = 1.0f;

    // Clipping :/
    float fMinY = g.fNotesY - 5.0f;
    float fMaxY = g.fNotesY + g.fNotesCY + 5.0f;
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

    // Visualize! (alpha is always 0 = opaque now that learn-mode fades are gone)
    Renderer::RectVertices( x, y - cy, cx, cy, csTrack.iVeryDarkRGB, csTrack.iVeryDarkRGB, csTrack.iVeryDarkRGB, csTrack.iVeryDarkRGB, pOut );
    Renderer::RectVertices( x + fDeflate, y - cy + fDeflate,
                            cx - fDeflate * 2.0f, cy - fDeflate * 2.0f,
                            csTrack.iPrimaryRGB, csTrack.iDarkRGB, csTrack.iDarkRGB, csTrack.iPrimaryRGB, pOut + 6 );
}

//-----------------------------------------------------------------------------
// Background, keys, border
//-----------------------------------------------------------------------------

void Scene::DrawBackground( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c )
{
    pRenderer->DrawRect( g.fNotesX, g.fNotesY, g.fNotesCX, g.fNotesCY, c.csBackground.iPrimaryRGB );
    if ( pRenderer->HasBackgroundImage() ) pRenderer->DrawBackgroundImage( g.fNotesX, g.fNotesY, g.fNotesCX, g.fNotesCY );
    if ( !g.bLines ) return;

    // Vertical lines (31-EDO: where the 12-key layout has them, left of each F and C)
    for ( int i = g.iStartNote + 1; i <= g.iEndNote; i++ )
        if ( g.bEdo31 )
        {
            if ( i % 31 != 13 && i % 31 != 0 ) continue;
            float x = floor( GetNoteX( g, i ) + 0.5f );
            pRenderer->DrawRect( x - 1.0f, g.fNotesY, 3.0f, g.fNotesCY,
                c.csBackground.iDarkRGB, c.csBackground.iVeryDarkRGB, c.csBackground.iVeryDarkRGB, c.csBackground.iDarkRGB );
        }
        else if ( !Notes::IsSharp( i - 1 ) && !Notes::IsSharp( i ) )
        {
            int iWhiteKeys = Notes::WhiteCount( g.iStartNote, i );
            float fStartX = Notes::IsSharp( g.iStartNote ) * SharpRatio / 2.0f;
            float x = g.fNotesX + g.fWhiteCX * ( iWhiteKeys + fStartX );
            x = floor( x + 0.5f ); // Needs to be rounded because of the gradient
            pRenderer->DrawRect( x - 1.0f, g.fNotesY, 3.0f, g.fNotesCY,
                c.csBackground.iDarkRGB, c.csBackground.iVeryDarkRGB, c.csBackground.iVeryDarkRGB, c.csBackground.iDarkRGB );
        }
}

void Scene::DrawKeys( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c, const ChannelSettings *const pKeyColor[256] )
{
    // Screen info
    float fKeysY = g.fNotesY + g.fNotesCY;
    float fKeysCY = g.iBufferHeight - g.fNotesCY;

    float fTransitionPct = .02f;
    float fTransitionCY = max( 3.0f, floor( fKeysCY * fTransitionPct + 0.5f ) );
    float fRedPct = .05f;
    float fRedCY = floor( fKeysCY * fRedPct + 0.5f );
    float fSpacerCY = 2.0f;
    float fTopCY = floor( ( fKeysCY - fSpacerCY - fRedCY - fTransitionCY ) * 0.95f + 0.5f );
    float fNearCY = fKeysCY - fSpacerCY - fRedCY - fTransitionCY - fTopCY;

    // Draw the background
    pRenderer->DrawRect( g.fNotesX, fKeysY, g.fNotesCX, fKeysCY, c.csKBBackground.iVeryDarkRGB );
    pRenderer->DrawRect( g.fNotesX, fKeysY, g.fNotesCX, fTransitionCY,
        c.csBackground.iPrimaryRGB, c.csBackground.iPrimaryRGB, c.csKBBackground.iVeryDarkRGB, c.csKBBackground.iVeryDarkRGB );
    pRenderer->DrawRect( g.fNotesX, fKeysY + fTransitionCY, g.fNotesCX, fRedCY,
        c.csKBRed.iDarkRGB, c.csKBRed.iDarkRGB, c.csKBRed.iPrimaryRGB, c.csKBRed.iPrimaryRGB );
    pRenderer->DrawRect( g.fNotesX, fKeysY + fTransitionCY + fRedCY, g.fNotesCX, fSpacerCY,
        c.csKBBackground.iDarkRGB, c.csKBBackground.iDarkRGB, c.csKBBackground.iDarkRGB, c.csKBBackground.iDarkRGB );

    // Keys info
    float fKeyGap = max( 1.0f, floor( g.fWhiteCX * 0.05f + 0.5f ) );
    float fKeyGap1 = fKeyGap - floor( fKeyGap / 2.0f + 0.5f );

    int iStartRender = ( Notes::IsSharp( g.iStartNote ) ? g.iStartNote - 1 : g.iStartNote );
    int iEndRender = ( Notes::IsSharp( g.iEndNote ) && g.iEndNote < 255 ? g.iEndNote + 1 : g.iEndNote );   // no white key past 255
    float fStartX = ( Notes::IsSharp( g.iStartNote ) ? g.fWhiteCX * ( SharpRatio / 2.0f - 1.0f ) : 0.0f );
    float fSharpCY = fTopCY * 0.67f;

    const ChannelSettings &csKBW = c.csKBWhite, &csKBB = c.csKBBackground, &csKBS = c.csKBSharp;

    // Draw the white keys
    float fCurX = g.fNotesX + fStartX;
    float fCurY = fKeysY + fTransitionCY + fRedCY + fSpacerCY;
    // (31-EDO: keys 0 to 255, the whites side by side from 0 as here)
    for ( int i = iStartRender; i <= iEndRender; i++ )
        if ( g.bEdo31 ? KeyLayer( g, i ) == 0 : !Notes::IsSharp( i ) )
        {
            const ChannelSettings *pLit = ( i >= 0 && i < 256 ) ? pKeyColor[i] : nullptr;
            if ( !pLit )
            {
                pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY, g.fWhiteCX - fKeyGap, fTopCY + fNearCY,
                    csKBW.iDarkRGB, csKBW.iDarkRGB, csKBW.iPrimaryRGB, csKBW.iPrimaryRGB );
                pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY + fTopCY, g.fWhiteCX - fKeyGap, fNearCY,
                    csKBW.iDarkRGB, csKBW.iDarkRGB, csKBW.iVeryDarkRGB, csKBW.iVeryDarkRGB );
                pRenderer->DrawRect( fCurX + fKeyGap1, fCurY + fTopCY, g.fWhiteCX - fKeyGap, 2.0f,
                    csKBB.iDarkRGB, csKBB.iDarkRGB, csKBW.iVeryDarkRGB, csKBW.iVeryDarkRGB );

                if ( i == g.iMiddleC )
                {
                    float fMXGap = floor( g.fWhiteCX * 0.25f + 0.5f );
                    float fMCX = g.fWhiteCX - fMXGap * 2.0f - fKeyGap;
                    float fMY = max( fCurY + fTopCY - fMCX - 5.0f, fCurY + fSharpCY + 5.0f );
                    pRenderer->DrawRect( fCurX + fKeyGap1 + fMXGap, fMY, fMCX, fCurY + fTopCY - 5.0f - fMY, csKBW.iDarkRGB );
                }
            }
            else
            {
                const ChannelSettings &csKBWhite = *pLit;
                pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY, g.fWhiteCX - fKeyGap, fTopCY + fNearCY - 2.0f,
                    csKBWhite.iDarkRGB, csKBWhite.iDarkRGB, csKBWhite.iPrimaryRGB, csKBWhite.iPrimaryRGB );
                pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY + fTopCY + fNearCY - 2.0f, g.fWhiteCX - fKeyGap, 2.0f, csKBWhite.iDarkRGB );

                if ( i == g.iMiddleC )
                {
                    float fMXGap = floor( g.fWhiteCX * 0.25f + 0.5f );
                    float fMCX = g.fWhiteCX - fMXGap * 2.0f - fKeyGap;
                    float fMY = max( fCurY + fTopCY + fNearCY - fMCX - 7.0f, fCurY + fSharpCY + 5.0f );
                    pRenderer->DrawRect( fCurX + fKeyGap1 + fMXGap, fMY, fMCX, fCurY + fTopCY + fNearCY - 7.0f - fMY, csKBWhite.iDarkRGB );
                }
            }
            pRenderer->DrawRect( floor( fCurX + fKeyGap1 + g.fWhiteCX - fKeyGap + 0.5f ), fCurY, fKeyGap, fTopCY + fNearCY,
                csKBB.iVeryDarkRGB, csKBB.iPrimaryRGB, csKBB.iPrimaryRGB, csKBB.iVeryDarkRGB );

            fCurX += g.fWhiteCX;
        }

    // A black key (31-EDO: also the half and full sharps and flats, fSharpCY tall from the black keys' top)
    auto DrawSharp = [&]( float x, float cx, float fSharpTopX1, float fSharpTopX2, float fSharpCY,
                          const ChannelSettings &csKBS, const ChannelSettings *pLit ) {
        if ( !pLit )
        {
            pRenderer->DrawSkew( fSharpTopX1, fCurY + fSharpCY - fNearCY,
                                 fSharpTopX2, fCurY + fSharpCY - fNearCY,
                                 x + cx, fCurY + fSharpCY, x, fCurY + fSharpCY,
                                 csKBS.iPrimaryRGB, csKBS.iPrimaryRGB, csKBS.iVeryDarkRGB, csKBS.iVeryDarkRGB );
            pRenderer->DrawSkew( fSharpTopX1, fCurY - fNearCY,
                                 fSharpTopX1, fCurY + fSharpCY - fNearCY,
                                 x, fCurY + fSharpCY, x, fCurY,
                                 csKBS.iPrimaryRGB, csKBS.iPrimaryRGB, csKBS.iVeryDarkRGB, csKBS.iVeryDarkRGB );
            pRenderer->DrawSkew( fSharpTopX2, fCurY + fSharpCY - fNearCY,
                                 fSharpTopX2, fCurY - fNearCY,
                                 x + cx, fCurY, x + cx, fCurY + fSharpCY,
                                 csKBS.iPrimaryRGB, csKBS.iPrimaryRGB, csKBS.iVeryDarkRGB, csKBS.iVeryDarkRGB );
            pRenderer->DrawRect( fSharpTopX1, fCurY - fNearCY, fSharpTopX2 - fSharpTopX1, fSharpCY, csKBS.iVeryDarkRGB );
            pRenderer->DrawSkew( fSharpTopX1, fCurY - fNearCY,
                                 fSharpTopX2, fCurY - fNearCY,
                                 fSharpTopX2, fCurY - fNearCY + fSharpCY * 0.45f,
                                 fSharpTopX1, fCurY - fNearCY + fSharpCY * 0.35f,
                                 csKBS.iDarkRGB, csKBS.iDarkRGB, csKBS.iPrimaryRGB, csKBS.iPrimaryRGB );
            pRenderer->DrawSkew( fSharpTopX1, fCurY - fNearCY + fSharpCY * 0.35f,
                                 fSharpTopX2, fCurY - fNearCY + fSharpCY * 0.45f,
                                 fSharpTopX2, fCurY - fNearCY + fSharpCY * 0.65f,
                                 fSharpTopX1, fCurY - fNearCY + fSharpCY * 0.55f,
                                 csKBS.iPrimaryRGB, csKBS.iPrimaryRGB, csKBS.iVeryDarkRGB, csKBS.iVeryDarkRGB );
        }
        else
        {
            const ChannelSettings &csKBSharp = *pLit;
            const float fNewNear = fNearCY * 0.25f;

            pRenderer->DrawSkew( fSharpTopX1, fCurY + fSharpCY - fNewNear,
                                 fSharpTopX2, fCurY + fSharpCY - fNewNear,
                                 x + cx, fCurY + fSharpCY, x, fCurY + fSharpCY,
                                 csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
            pRenderer->DrawSkew( fSharpTopX1, fCurY - fNewNear,
                                 fSharpTopX1, fCurY + fSharpCY - fNewNear,
                                 x, fCurY + fSharpCY, x, fCurY,
                                 csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
            pRenderer->DrawSkew( fSharpTopX2, fCurY + fSharpCY - fNewNear,
                                 fSharpTopX2, fCurY - fNewNear,
                                 x + cx, fCurY, x + cx, fCurY + fSharpCY,
                                 csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
            pRenderer->DrawRect( fSharpTopX1, fCurY - fNewNear, fSharpTopX2 - fSharpTopX1, fSharpCY, csKBSharp.iDarkRGB );
            pRenderer->DrawSkew( fSharpTopX1, fCurY - fNewNear,
                                 fSharpTopX2, fCurY - fNewNear,
                                 fSharpTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                                 fSharpTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                                 csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB );
            pRenderer->DrawSkew( fSharpTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                                 fSharpTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                                 fSharpTopX2, fCurY - fNewNear + fSharpCY * 0.75f,
                                 fSharpTopX1, fCurY - fNewNear + fSharpCY * 0.65f,
                                 csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
        }
    };

    // Draw the sharps
    // 31-EDO: the halves (as tall as black keys), then the full sharps and flats over them (half as tall,
    // from the same top), each with the black keys' shading
    if ( g.bEdo31 )
    {
        fCurY = fKeysY + fTransitionCY + fRedCY + fSpacerCY;
        for ( int iLayer = 1; iLayer <= 2; iLayer++ )
            for ( int i = g.iStartNote; i <= g.iEndNote; i++ )
            {
                if ( KeyLayer( g, i ) != iLayer ) continue;
                const NoteKey nk = GetNoteKey( g, i );
                const float x = nk.x, cx = nk.cx;
                const float fSharpTopX1 = x + cx * 0.15f, fSharpTopX2 = x + cx * 0.85f;
                const ChannelSettings &cs = iLayer == 1 ? c.csKBHalf : Key31Of( i ).kind == Sharp ? c.csKBSharp31 : csKBS;
                DrawSharp( x, cx, fSharpTopX1, fSharpTopX2, iLayer == 1 ? fSharpCY : fSharpCY * 0.5f, cs, pKeyColor[i] );
            }
        return;
    }

    iStartRender = ( g.iStartNote != Notes::A0 && !Notes::IsSharp( g.iStartNote ) && g.iStartNote > 0 && Notes::IsSharp( g.iStartNote - 1 ) ? g.iStartNote - 1 : g.iStartNote );
    iEndRender = ( g.iEndNote != Notes::C8 && !Notes::IsSharp( g.iEndNote ) && g.iEndNote != 127 && g.iEndNote < 255 && Notes::IsSharp( g.iEndNote + 1 ) ? g.iEndNote + 1 : g.iEndNote );
    fStartX = ( Notes::IsSharp( g.iStartNote ) ? g.fWhiteCX * SharpRatio / 2.0f : 0.0f );

    float fSharpTop = SharpRatio * 0.7f;
    fCurX = g.fNotesX + fStartX;
    fCurY = fKeysY + fTransitionCY + fRedCY + fSpacerCY;
    for ( int i = iStartRender; i <= iEndRender; i++ )
        if ( !Notes::IsSharp( i ) )
            fCurX += g.fWhiteCX;
        else
        {
            float fNudgeX = 0.0;
            Notes::Note eNote = Notes::NoteVal( i );
            if ( eNote == Notes::CS || eNote == Notes::FS ) fNudgeX = -SharpRatio / 5.0f;
            else if ( eNote == Notes::AS || eNote == Notes::DS ) fNudgeX = SharpRatio / 5.0f;

            const float cx = g.fWhiteCX * SharpRatio;
            const float x = fCurX - g.fWhiteCX * ( SharpRatio / 2.0f - fNudgeX );
            const float fSharpTopX1 = x + g.fWhiteCX * ( SharpRatio - fSharpTop ) / 2.0f;
            const float fSharpTopX2 = fSharpTopX1 + g.fWhiteCX * fSharpTop;

            DrawSharp( x, cx, fSharpTopX1, fSharpTopX2, fSharpCY, csKBS, ( i >= 0 && i < 256 ) ? pKeyColor[i] : nullptr );
        }
}

void Scene::DrawBorder( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c )
{
    // Top, bottom, left, right
    const unsigned iBlack = 0x00000000;
    float fBufferCY = static_cast< float >( g.iBufferHeight );
    pRenderer->DrawRect( g.fNotesX - 50.0f, g.fNotesY - 50.0f, g.fNotesCX + 100.0f, 50.0f, iBlack );
    pRenderer->DrawRect( g.fNotesX - 50.0f, g.fNotesY + fBufferCY, g.fNotesCX + 100.0f, 50.0f, iBlack );
    pRenderer->DrawRect( g.fNotesX - g.fWhiteCX, g.fNotesY - 50.0f, g.fWhiteCX, fBufferCY + 100.0f, iBlack );
    pRenderer->DrawRect( g.fNotesX + g.fNotesCX, g.fNotesY - 50.0f, g.fWhiteCX, fBufferCY + 100.0f, iBlack );

    const float fPad = 10.0f;
    const unsigned iBkg = c.csBackground.iPrimaryRGB;
    pRenderer->DrawSkew( g.fNotesX, g.fNotesY + fBufferCY, g.fNotesX + g.fNotesCX, g.fNotesY + fBufferCY,
                         g.fNotesX + g.fNotesCX + fPad, g.fNotesY + fBufferCY + fPad, g.fNotesX - fPad, g.fNotesY + fBufferCY + fPad,
                         iBkg, iBkg, iBlack, iBlack );
    pRenderer->DrawSkew( g.fNotesX - fPad, g.fNotesY - fPad, g.fNotesX + g.fNotesCX + fPad, g.fNotesY - fPad,
                         g.fNotesX + g.fNotesCX, g.fNotesY, g.fNotesX, g.fNotesY,
                         iBlack, iBlack, iBkg, iBkg );
    pRenderer->DrawSkew( g.fNotesX - fPad, g.fNotesY - fPad, g.fNotesX, g.fNotesY,
                         g.fNotesX, g.fNotesY + fBufferCY, g.fNotesX - fPad, g.fNotesY + fBufferCY + fPad,
                         iBlack, iBkg, iBkg, iBlack );
    pRenderer->DrawSkew( g.fNotesX + g.fNotesCX, g.fNotesY, g.fNotesX + g.fNotesCX + fPad, g.fNotesY - fPad,
                         g.fNotesX + g.fNotesCX + fPad, g.fNotesY + fBufferCY + fPad, g.fNotesX + g.fNotesCX, g.fNotesY + fBufferCY,
                         iBkg, iBlack, iBlack, iBkg );
}

//-----------------------------------------------------------------------------
// MeterTracker
//-----------------------------------------------------------------------------

MeterTracker::MeterTracker( const std::vector< RefMetaEvent > &vMeta, const RefMidiInfo &info ) : m_Info( info ), m_vMetaEvents( vMeta )
{
    for ( int i = 0; i < (int)m_vMetaEvents.size(); i++ )
    {
        const RefMetaEvent &m = m_vMetaEvents[i];
        if ( m.cType == RefMetaEvent::SetTempo )
            m_vTempo.push_back( std::make_pair( m.llMicroSec, i ) );
        else if ( m.cType == RefMetaEvent::TimeSignature )
            m_vSignature.push_back( std::make_pair( m.llMicroSec, i ) );
    }
    m_itNextTempo = m_vTempo.begin();
    m_itNextSignature = m_vSignature.begin();
}

void MeterTracker::Jump( long long llTime )
{
    m_itNextTempo = upper_bound( m_vTempo.begin(), m_vTempo.end(), std::pair< long long, int >( llTime, (int)m_vMetaEvents.size() ) );
    const RefMetaEvent *pPrevious = GetPrevious( m_itNextTempo, m_vTempo, 3 );
    if ( pPrevious )
    {
        m_iMicroSecsPerBeat = ( pPrevious->aData[0] << 16 ) | ( pPrevious->aData[1] << 8 ) | pPrevious->aData[2];
        m_iLastTempoTick = pPrevious->llTick;
        m_llLastTempoTime = pPrevious->llMicroSec;
    }
    else
    {
        m_iMicroSecsPerBeat = 500000;
        m_llLastTempoTime = m_iLastTempoTick = 0;
    }

    m_itNextSignature = upper_bound( m_vSignature.begin(), m_vSignature.end(), std::pair< long long, int >( llTime, (int)m_vMetaEvents.size() ) );
    pPrevious = GetPrevious( m_itNextSignature, m_vSignature, 4 );
    if ( pPrevious )
    {
        m_iBeatsPerMeasure = pPrevious->aData[0];
        m_iBeatType = 1LL << pPrevious->aData[1];
        m_iClocksPerMet = pPrevious->aData[2];
        m_iLastSignatureTick = pPrevious->llTick;
    }
    else
    {
        m_iBeatsPerMeasure = 4;
        m_iBeatType = 4;
        m_iClocksPerMet = 24;
        m_iLastSignatureTick = 0;
    }
}

void MeterTracker::Advance( long long llTime )
{
    for ( ; m_itNextTempo != m_vTempo.end() && m_itNextTempo->first <= llTime; ++m_itNextTempo )
    {
        const RefMetaEvent &m = m_vMetaEvents[m_itNextTempo->second];
        m_iMicroSecsPerBeat = ( m.aData[0] << 16 ) | ( m.aData[1] << 8 ) | m.aData[2];
        m_iLastTempoTick = m.llTick;
        m_llLastTempoTime = m.llMicroSec;
    }
    for ( ; m_itNextSignature != m_vSignature.end() && m_itNextSignature->first <= llTime; ++m_itNextSignature )
    {
        const RefMetaEvent &m = m_vMetaEvents[m_itNextSignature->second];
        m_iBeatsPerMeasure = m.aData[0];
        m_iBeatType = 1LL << m.aData[1];
        m_iClocksPerMet = m.aData[2];
        m_iLastSignatureTick = m.llTick;
    }
}

// Might change the value of itCurrent
const RefMetaEvent *MeterTracker::GetPrevious( eventvec_t::const_iterator &itCurrent, const eventvec_t &vEventMap, int iDataLen ) const
{
    eventvec_t::const_iterator it = itCurrent;
    if ( itCurrent != vEventMap.begin() )
    {
        while ( it != vEventMap.begin() )
            if ( m_vMetaEvents[( --it )->second].iDataLen == iDataLen )
                return &m_vMetaEvents[it->second];
    }
    else if ( vEventMap.size() > 0 && itCurrent->first <= m_Info.llFirstNote && m_vMetaEvents[itCurrent->second].iDataLen == iDataLen )
    {
        const RefMetaEvent *pPrevious = &m_vMetaEvents[itCurrent->second];
        ++itCurrent;
        return pPrevious;
    }
    return nullptr;
}

long long MeterTracker::GetCurrentTick( long long llStartTime ) const
{
    int iDivision = m_Info.iDivision;
    if ( !( iDivision & 0x8000 ) && m_iMicroSecsPerBeat > 0 )
    {
        if ( llStartTime >= m_llLastTempoTime )
            return m_iLastTempoTick + ( ( iDivision * ( llStartTime - m_llLastTempoTime ) ) / m_iMicroSecsPerBeat );
        else
            return m_iLastTempoTick - ( ( iDivision * ( m_llLastTempoTime - llStartTime ) + 1 ) / m_iMicroSecsPerBeat ) - 1;
    }
    return -1;
}

long long MeterTracker::GetTickTime( long long iTick, long long iLastTempoTick, long long llLastTempoTime, long long iMicroSecsPerBeat ) const
{
    int iDivision = m_Info.iDivision;
    if ( !( iDivision & 0x8000 ) )
        return llLastTempoTime + ( iMicroSecsPerBeat * ( iTick - iLastTempoTick ) ) / iDivision;
    return -1;
}

// Rounds up to the nearest beat
long long MeterTracker::GetBeat( long long iTick, long long iBeatType, long long iLastSignatureTick ) const
{
    int iDivision = m_Info.iDivision;
    long long iTickOffset = iTick - iLastSignatureTick;
    if ( !( iDivision & 0x8000 ) )
    {
        if ( iTickOffset > 0 )
            return ( iTickOffset * iBeatType - 1 ) / ( iDivision * 4LL ) + 1;
        else
            return ( iTickOffset * iBeatType ) / ( iDivision * 4LL );
    }
    return -1;
}

long long MeterTracker::GetBeatTick( long long iTick, long long iBeatType, long long iLastSignatureTick ) const
{
    int iDivision = m_Info.iDivision;
    if ( !( iDivision & 0x8000 ) && iBeatType > 0 )
        return iLastSignatureTick + ( GetBeat( iTick, iBeatType, iLastSignatureTick ) * iDivision * 4 ) / iBeatType;
    return -1;
}

long long MeterTracker::GetMetTick( long long iTick, long long iClocksPerMet, long long iLastSignatureTick ) const
{
    int iDivision = m_Info.iDivision;
    long long iTickOffset = iTick - iLastSignatureTick;
    if ( !( iDivision & 0x8000 ) && iClocksPerMet > 0 )
    {
        long long iMet = 0;
        if ( iTickOffset > 0 )
            iMet = ( ( iTickOffset * 24 - 1 ) / ( iDivision * iClocksPerMet ) + 1 );
        else
            iMet = ( iTickOffset * 24 ) / ( iDivision * iClocksPerMet );
        return iLastSignatureTick + ( iMet * iClocksPerMet * iDivision ) / 24;
    }
    return -1;
}

// The horizontal half of MainScreen::RenderLines
void MeterTracker::DrawMeasureLines( Renderer *pRenderer, const FrameGlobals &g, const SceneColors &c, long long iStartTick ) const
{
    int iDivision = m_Info.iDivision;
    if ( ( iDivision & 0x8000 ) || !g.bLines ) return;

    // Copy time state vars
    long long iCurrTick = iStartTick - 1;
    long long llEndTime = g.llStartTime + g.llTimeSpan;

    // Copy tempo state vars
    long long iLastTempoTick = m_iLastTempoTick;
    long long iMicroSecsPerBeat = m_iMicroSecsPerBeat;
    long long llLastTempoTime = m_llLastTempoTime;
    eventvec_t::const_iterator itNextTempo = m_itNextTempo;

    // Copy signature state vars
    long long iLastSignatureTick = m_iLastSignatureTick;
    long long iBeatsPerMeasure = m_iBeatsPerMeasure;
    long long iBeatType = m_iBeatType;
    eventvec_t::const_iterator itNextSignature = m_itNextSignature;

    long long llNextBeatTime = 0;
    int iGuard = 0;
    do
    {
        long long iNextBeatTick = GetBeatTick( iCurrTick + 1, iBeatType, iLastSignatureTick );

        // Next beat crosses the next tempo event. handle the event and recalculate next beat time
        while ( itNextTempo != m_vTempo.end() && m_vMetaEvents[itNextTempo->second].iDataLen == 3 &&
                iNextBeatTick > m_vMetaEvents[itNextTempo->second].llTick )
        {
            const RefMetaEvent &m = m_vMetaEvents[itNextTempo->second];
            iMicroSecsPerBeat = ( m.aData[0] << 16 ) | ( m.aData[1] << 8 ) | m.aData[2];
            iLastTempoTick = m.llTick;
            llLastTempoTime = m.llMicroSec;
            ++itNextTempo;
        }
        while ( itNextSignature != m_vSignature.end() && m_vMetaEvents[itNextSignature->second].iDataLen == 4 &&
                iNextBeatTick > m_vMetaEvents[itNextSignature->second].llTick )
        {
            const RefMetaEvent &m = m_vMetaEvents[itNextSignature->second];
            iBeatsPerMeasure = m.aData[0];
            iBeatType = 1LL << m.aData[1];
            iLastSignatureTick = m.llTick;
            iNextBeatTick = GetBeatTick( iLastSignatureTick + 1, iBeatType, iLastSignatureTick );
            ++itNextSignature;
        }

        // Finally render the beat or measure
        long long iNextBeat = GetBeat( iNextBeatTick, iBeatType, iLastSignatureTick );
        bool bIsMeasure = iBeatsPerMeasure != 0 && !( ( iNextBeat < 0 ? -iNextBeat : iNextBeat ) % iBeatsPerMeasure );
        llNextBeatTime = GetTickTime( iNextBeatTick, iLastTempoTick, llLastTempoTime, iMicroSecsPerBeat );
        float y = g.fNotesY + g.fNotesCY * ( 1.0f - static_cast< float >( llNextBeatTime - g.llRndStartTime ) / g.llTimeSpan );
        y = floor( y + 0.5f );
        if ( bIsMeasure && y + 1.0f > g.fNotesY )
            pRenderer->DrawRect( g.fNotesX, y - 1.0f, g.fNotesCX, 3.0f,
                c.csBackground.iDarkRGB, c.csBackground.iDarkRGB, c.csBackground.iVeryDarkRGB, c.csBackground.iVeryDarkRGB );

        if ( iNextBeatTick <= iCurrTick ) break; // degenerate tempo or signature: no progress
        iCurrTick = iNextBeatTick;
    }
    while ( llNextBeatTime <= llEndTime && ++iGuard < 100000 );
}
