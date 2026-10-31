#include "visual/Effects.h"

#include <algorithm>
#include <cmath>

D3DCOLOR Effects::Mix( D3DCOLOR a, D3DCOLOR b, float t )
{
    D3DCOLOR r = a & 0xFF000000;
    for ( int s = 0; s < 24; s += 8 )
    {
        float ca = ( a >> s ) & 0xFF, cb = ( b >> s ) & 0xFF;
        r |= (D3DCOLOR)std::clamp( (int)( ca + ( cb - ca ) * t + 0.5f ), 0, 255 ) << s;
    }
    return r;
}

D3DCOLOR Effects::Scale( D3DCOLOR c, float f )
{
    return Mix( c & 0xFF000000, c, f );
}

D3DCOLOR Effects::Bright( D3DCOLOR c, float f )
{
    if ( f <= 1.0f ) return Scale( c, f );
    float a[3], fLeft = 0.0f;
    for ( int i = 0; i < 3; i++ )
    {
        a[i] = ( ( c >> ( i * 8 ) ) & 0xFF ) * f;
        if ( a[i] > 255.0f ) { fLeft += a[i] - 255.0f; a[i] = 255.0f; }
    }
    // What is left over, shared evenly by the channels not yet full (twice at most: one may fill)
    for ( int iPass = 0; iPass < 2 && fLeft > 0.0f; iPass++ )
    {
        int iOpen = 0;
        for ( float v : a ) iOpen += v < 255.0f;
        if ( !iOpen ) break;
        const float fEach = fLeft / iOpen;
        fLeft = 0.0f;
        for ( float &v : a )
            if ( v < 255.0f ) { v += fEach; if ( v > 255.0f ) { fLeft += v - 255.0f; v = 255.0f; } }
    }
    D3DCOLOR r = c & 0xFF000000;
    for ( int i = 0; i < 3; i++ ) r |= (D3DCOLOR)std::clamp( (int)( a[i] + 0.5f ), 0, 255 ) << ( i * 8 );
    return r;
}

// The curve from 1 at x = 0 to 0 at x = 1
static float Fall( int eDecay, float x )
{
    x = std::clamp( x, 0.0f, 1.0f );
    switch ( eDecay )
    {
        case EffectSettings::DecayLinear: return 1.0f - x;
        case EffectSettings::DecayExponential:
        {
            static const float fEnd = std::exp( -4.0f );    // as e^-4x, scaled to reach 0 at 1
            return ( std::exp( -4.0f * x ) - fEnd ) / ( 1.0f - fEnd );
        }
        case EffectSettings::DecaySmooth: return 1.0f - x * x * ( 3.0f - 2.0f * x );
    }
    return 1.0f;
}

float Effects::Held( const EffectSettings &fx, long long llUs )
{
    if ( fx.eDecay == EffectSettings::DecayNone ) return 1.0f;
    const float fFloor = fx.iDecayFloor / 100.0f;
    return fFloor + ( 1.0f - fFloor ) * Fall( fx.eDecay, llUs / ( fx.iDecayMs * 1000.0f ) );
}

float Effects::Released( const EffectSettings &fx, float fFrom, long long llUs )
{
    if ( fx.eDecay == EffectSettings::DecayNone || fx.iReleaseMs <= 0 ) return 0.0f;
    return fFrom * Fall( fx.eDecay, llUs / ( fx.iReleaseMs * 1000.0f ) );
}

//-----------------------------------------------------------------------------
// Look
//-----------------------------------------------------------------------------

ChannelSettings Effects::Look::Unlit( int ePart, const ChannelSettings &cs ) const
{
    ChannelSettings r = cs;
    r.iPrimaryRGB = Scale( cs.iPrimaryRGB, m_aKeep[ePart] );
    r.iDarkRGB = Scale( cs.iDarkRGB, m_aKeep[ePart] );
    r.iVeryDarkRGB = Scale( cs.iVeryDarkRGB, m_aKeep[ePart] );
    if ( m_bFlat ) r.iDarkRGB = r.iVeryDarkRGB = r.iPrimaryRGB;
    return r;
}

void Effects::Look::Prepare( const EffectSettings &fx, const std::vector< TrackSettings > &vTracks, unsigned uTracksGen, const SceneColors &c )
{
    for ( int p = 0; p < EffectSettings::PartCount; p++ )
    {
        m_aKeep[p] = 1.0f - fx.aDark[p] / 100.0f;
        m_aUp[p] = fx.aLightUp[p] / 100.0f;
        m_aGlow[p] = fx.iBloom > 0 ? fx.aGlow[p] / 100.0f : 0.0f;
    }
    m_fRest = fx.iRestLight / 100.0f;
    m_fOutline = fx.iLightOutlines / 100.0f;
    m_fInside = fx.iLightInsides / 100.0f;
    m_bFlat = fx.bFlat;

    // The scene's colours, darkened (the background and the keys each their own); the flat look's
    // lines one colour (the keys' own are DrawKeysFlat's)
    m_Colors = c;
    for ( ChannelSettings *p : { &m_Colors.csBackground, &m_Colors.csKBRed, &m_Colors.csKBWhite, &m_Colors.csKBSharp,
                                 &m_Colors.csKBBackground, &m_Colors.csKBHalf, &m_Colors.csKBSharp31 } )
    {
        const int ePart = p == &m_Colors.csBackground ? EffectSettings::PartBackground : p == &m_Colors.csKBRed ? EffectSettings::PartBar : EffectSettings::PartKeys;
        const float f = m_aKeep[ePart], fUp = ePart == EffectSettings::PartBar ? m_aUp[ePart] : 0.0f;
        p->iPrimaryRGB = Mix( Scale( p->iPrimaryRGB, f ), 0x00FFFFFF, fUp );
        p->iDarkRGB = Mix( Scale( p->iDarkRGB, f ), 0x00FFFFFF, fUp );
        p->iVeryDarkRGB = Mix( Scale( p->iVeryDarkRGB, f ), 0x00FFFFFF, fUp );
    }
    m_cBarGlow = Glow( EffectSettings::PartBar, c.csKBRed, 1.0f );
    if ( m_bFlat ) m_Colors.csBackground.iVeryDarkRGB = m_Colors.csBackground.iDarkRGB;

    // The notes' at rest: only when anything changes them, and the short notes' only when they differ
    // (copied again when what they are made from changes)
    const int iN = EffectSettings::PartNotes, iS = EffectSettings::PartShort;
    auto Changes = [&]( int p ) { return fx.aDark[p] > 0 || fx.bFlat || ( fx.iRestLight > 0 && ( fx.aDark[p] > 0 || fx.aLightUp[p] > 0 || fx.iLightOutlines > 100 ) ); };
    m_abOwnRest[0] = Changes( iN );
    m_abOwnRest[1] = Changes( iS ) && ( fx.aDark[iS] != fx.aDark[iN] || fx.aLightUp[iS] != fx.aLightUp[iN] );
    if ( !m_abOwnRest[0] && !m_abOwnRest[1] ) { m_avRest[0].clear(); m_avRest[1].clear(); m_vRestKey.clear(); return; }
    const std::vector< long long > vKey = { fx.aDark[iN], fx.aDark[iS], fx.aLightUp[iN], fx.aLightUp[iS], fx.bFlat, fx.iLightOutlines, fx.iLightInsides,
                                            fx.iRestLight, uTracksGen, (long long)vTracks.size(), m_abOwnRest[0], m_abOwnRest[1] };
    if ( vKey == m_vRestKey ) return;
    m_vRestKey = vKey;
    for ( int i = 0; i < 2; i++ )
    {
        m_avRest[i].resize( m_abOwnRest[i] ? vTracks.size() : 0 );
        if ( !m_abOwnRest[i] ) continue;
        for ( size_t t = 0; t < vTracks.size(); t++ )
            for ( int ch = 0; ch < 16; ch++ )
                m_avRest[i][t].aChannels[ch] = Lit( i ? iS : iN, vTracks[t].aChannels[ch], m_fRest );
    }
}

const std::vector< TrackSettings > &Effects::Look::Rest( int ePart, const std::vector< TrackSettings > &vTracks ) const
{
    if ( ePart == EffectSettings::PartShort && m_abOwnRest[1] ) return m_avRest[1];
    return m_abOwnRest[0] ? m_avRest[0] : vTracks;
}

ChannelSettings Effects::Look::Lit( int ePart, const ChannelSettings &base, const ChannelSettings &from, float fLight ) const
{
    ChannelSettings r = from;
    const float fUp = m_aUp[ePart];
    const D3DCOLOR cWhite = 0x00FFFFFF;
    D3DCOLOR p = Mix( base.iPrimaryRGB, cWhite, fUp ), d = Mix( base.iDarkRGB, cWhite, fUp ), vd = Mix( base.iVeryDarkRGB, cWhite, fUp );
    if ( m_bFlat ) d = vd = p;
    // A long note's light split: the inside its share; the outline (the very dark colour) its share up
    // to 1, and past it on towards the main colour, then white. A short note (the very dark colour
    // only, or the main colour) and a key are lit whole.
    float fIn = 1.0f, fOut = 1.0f;
    if ( ePart == EffectSettings::PartNotes )
    {
        fIn = m_fInside;
        fOut = std::min( m_fOutline, 1.0f );
        if ( m_fOutline > 2.0f ) vd = Mix( p, cWhite, m_fOutline - 2.0f );
        else if ( m_fOutline > 1.0f ) vd = Mix( vd, p, m_fOutline - 1.0f );
    }
    r.iPrimaryRGB = Mix( from.iPrimaryRGB, p, fLight * fIn );
    r.iDarkRGB = Mix( from.iDarkRGB, d, fLight * fIn );
    r.iVeryDarkRGB = Mix( from.iVeryDarkRGB, vd, fLight * fOut );
    return r;
}

D3DCOLOR Effects::Look::Glow( int ePart, const ChannelSettings &base, float fLight ) const
{
    const float f = fLight * m_aGlow[ePart];
    return f > 0.0f ? Bright( base.iPrimaryRGB, f ) & 0x00FFFFFF : 0;
}

//-----------------------------------------------------------------------------
// KeyLights
//-----------------------------------------------------------------------------

void Effects::KeyLights::Reset()
{
    for ( Key &k : m_aKey ) k = Key();
    for ( float &f : m_aLight ) f = 0.0f;
    for ( float &f : m_aHit ) f = 0.0f;
    m_bAny = false;
}

void Effects::KeyLights::Update( const EffectSettings &fx, long long llNow, const int aSlot[256], const long long aStart[256] )
{
    if ( m_bAny && llNow < m_llLast ) Reset();
    m_llLast = llNow;
    m_bAny = true;
    for ( int k = 0; k < 256; k++ )
    {
        Key &key = m_aKey[k];
        if ( aSlot[k] >= 0 )
        {
            // A new note (another start or channel) starts the light again
            if ( !key.bDown || key.llOn != aStart[k] || key.iSlot != aSlot[k] ) { key.llOn = aStart[k]; key.iSlot = aSlot[k]; }
            key.bDown = true;
            key.iHitSlot = aSlot[k];
            m_aLight[k] = Held( fx, llNow - key.llOn );
            m_aHit[k] = 1.0f;
        }
        else
        {
            if ( key.bDown )
            {
                key.bDown = false;
                key.llOff = llNow;
                key.fAtOff = Held( fx, llNow - key.llOn );
            }
            m_aLight[k] = key.iSlot >= 0 ? Released( fx, key.fAtOff, llNow - key.llOff ) : 0.0f;
            if ( m_aLight[k] <= 0.0f ) key.iSlot = -1;
            const bool bFades = fx.eHitLineCurve != EffectSettings::DecayNone && fx.iHitLineFadeMs > 0;
            m_aHit[k] = key.iHitSlot >= 0 && bFades ? Fall( fx.eHitLineCurve, ( llNow - key.llOff ) / ( fx.iHitLineFadeMs * 1000.0f ) ) : 0.0f;
            if ( m_aHit[k] <= 0.0f ) key.iHitSlot = -1;
        }
    }
}
