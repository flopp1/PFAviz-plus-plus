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

ChannelSettings Effects::Look::Unlit( const ChannelSettings &cs ) const
{
    ChannelSettings r = cs;
    r.iPrimaryRGB = Scale( cs.iPrimaryRGB, m_fDark );
    r.iDarkRGB = Scale( cs.iDarkRGB, m_fDark );
    r.iVeryDarkRGB = Scale( cs.iVeryDarkRGB, m_fDark );
    if ( m_bFlat ) r.iDarkRGB = r.iVeryDarkRGB = r.iPrimaryRGB;
    return r;
}

void Effects::Look::Prepare( const EffectSettings &fx, const std::vector< TrackSettings > &vTracks, unsigned uTracksGen, const SceneColors &c )
{
    m_fDark = 1.0f - fx.iDarkRoom / 100.0f;
    m_fLightUp = fx.iLightUp / 100.0f;
    m_bFlat = fx.bFlat;

    // The scene's colours, darkened; the flat look's lines one colour (the keys' own are DrawKeysFlat's)
    m_Colors = c;
    for ( ChannelSettings *p : { &m_Colors.csBackground, &m_Colors.csKBRed, &m_Colors.csKBWhite, &m_Colors.csKBSharp,
                                 &m_Colors.csKBBackground, &m_Colors.csKBHalf, &m_Colors.csKBSharp31 } )
    {
        p->iPrimaryRGB = Scale( p->iPrimaryRGB, m_fDark );
        p->iDarkRGB = Scale( p->iDarkRGB, m_fDark );
        p->iVeryDarkRGB = Scale( p->iVeryDarkRGB, m_fDark );
    }
    if ( m_bFlat ) m_Colors.csBackground.iVeryDarkRGB = m_Colors.csBackground.iDarkRGB;

    // The notes' (only when darkened or flat; copied again when that or the tracks change)
    m_bOwnRest = fx.iDarkRoom > 0 || fx.bFlat;
    if ( !m_bOwnRest ) { m_vRest.clear(); m_iRestDark = -1; return; }
    if ( m_iRestDark == fx.iDarkRoom && m_bRestFlat == fx.bFlat && m_uRestGen == uTracksGen && m_iRestTracks == vTracks.size() ) return;
    m_vRest.resize( vTracks.size() );
    for ( size_t t = 0; t < vTracks.size(); t++ )
        for ( int ch = 0; ch < 16; ch++ ) m_vRest[t].aChannels[ch] = Unlit( vTracks[t].aChannels[ch] );
    m_iRestDark = fx.iDarkRoom;
    m_bRestFlat = fx.bFlat;
    m_uRestGen = uTracksGen;
    m_iRestTracks = vTracks.size();
}

ChannelSettings Effects::Look::Lit( const ChannelSettings &base, const ChannelSettings &rest, float fLight, bool bLightUp ) const
{
    ChannelSettings r = rest;
    const float fUp = bLightUp ? m_fLightUp : 0.0f;
    const D3DCOLOR cWhite = 0x00FFFFFF;
    D3DCOLOR p = Mix( base.iPrimaryRGB, cWhite, fUp ), d = Mix( base.iDarkRGB, cWhite, fUp ), vd = Mix( base.iVeryDarkRGB, cWhite, fUp );
    if ( m_bFlat ) d = vd = p;
    r.iPrimaryRGB = Mix( rest.iPrimaryRGB, p, fLight );
    r.iDarkRGB = Mix( rest.iDarkRGB, d, fLight );
    r.iVeryDarkRGB = Mix( rest.iVeryDarkRGB, vd, fLight );
    return r;
}

ChannelSettings Effects::Look::LitOutline( const ChannelSettings &base, const ChannelSettings &rest, float fLight, bool bLightUp ) const
{
    ChannelSettings r = rest;
    const D3DCOLOR cLit = Mix( base.iPrimaryRGB, 0x00FFFFFF, bLightUp ? m_fLightUp : 0.0f );
    r.iVeryDarkRGB = Mix( rest.iVeryDarkRGB, cLit, fLight );
    return r;
}

//-----------------------------------------------------------------------------
// KeyLights
//-----------------------------------------------------------------------------

void Effects::KeyLights::Reset()
{
    for ( Key &k : m_aKey ) k = Key();
    for ( float &f : m_aLight ) f = 0.0f;
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
            m_aLight[k] = Held( fx, llNow - key.llOn );
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
        }
    }
}
