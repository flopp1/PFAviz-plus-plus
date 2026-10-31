// Eye candy (Preferences > Effects; core/Config.h EffectSettings): the colours a frame is drawn with
// under the effects, the light of playing notes and pressed keys over time, and what the bloom glows
// from. With every effect off nothing here is used, and frames are drawn exactly as before.
//
// Light: a note playing at the hit line, and the key it presses, are lit with a light of 0 to 1 that
// starts at 1 and falls, while the note is held, along the decay curve to its floor; a key let go
// keeps its light a while, falling to 0 over the release time. Lit things are lightened towards white
// (light-up), are spared the dark room's darkening, and glow (bloom), each in proportion to the light.
#pragma once

#include <vector>

#include "core/Config.h"
#include "render/Renderer.h"
#include "visual/Scene.h"

namespace Effects
{
    // a to b by t (0 to 1), each channel (a's alpha); c times f
    D3DCOLOR Mix( D3DCOLOR a, D3DCOLOR b, float t );
    D3DCOLOR Scale( D3DCOLOR c, float f );

    // The light of a note held for llUs (1 at its start), and what is left of fFrom llUs after its key
    // was let go
    float Held( const EffectSettings &fx, long long llUs );
    float Released( const EffectSettings &fx, float fFrom, long long llUs );

    // The frame's colours under the effects. Prepare once a frame: the notes' colours are copied only
    // when what they depend on changed (uTracksGen: the caller's count of changes to the tracks).
    class Look
    {
    public:
        void Prepare( const EffectSettings &fx, const std::vector< TrackSettings > &vTracks, unsigned uTracksGen, const SceneColors &c );

        float Dark() const { return m_fDark; }                  // what unlit colours are multiplied by
        const SceneColors &Colors() const { return m_Colors; }  // the scene's (darkened; the flat look's lines)
        // The notes' colours when not playing (vTracks itself when nothing changes them)
        const std::vector< TrackSettings > &Rest( const std::vector< TrackSettings > &vTracks ) const { return m_bOwnRest ? m_vRest : vTracks; }
        // Something lit, from its own colours and the light: rest is how it looks unlit
        ChannelSettings Lit( const ChannelSettings &base, const ChannelSettings &rest, float fLight, bool bLightUp ) const;
        // A note lit only in its outline (the very dark colour), which goes towards its main colour
        ChannelSettings LitOutline( const ChannelSettings &base, const ChannelSettings &rest, float fLight, bool bLightUp ) const;
        D3DCOLOR Glow( const ChannelSettings &base, float fLight ) const { return Scale( base.iPrimaryRGB, fLight ) & 0x00FFFFFF; }

    private:
        ChannelSettings Unlit( const ChannelSettings &cs ) const;    // darkened, flattened

        float m_fDark = 1.0f, m_fLightUp = 0.0f;
        bool m_bFlat = false;
        SceneColors m_Colors;
        bool m_bOwnRest = false;
        std::vector< TrackSettings > m_vRest;
        int m_iRestDark = -1;
        bool m_bRestFlat = false;
        unsigned m_uRestGen = 0;
        size_t m_iRestTracks = 0;
    };

    // What FastEngine needs to draw the notes playing at the hit line lit (and collect their glow)
    struct NoteLight
    {
        const EffectSettings *pFx = nullptr;
        const Look *pLook = nullptr;
        const std::vector< TrackSettings > *pBase = nullptr;    // the tracks' own colours
        long long llNow = 0;                                    // the frame's time: playing notes started at or before it and end after it
        bool bLightUp = false;                                  // lightened
        bool bGlow = false;                                     // their glow rects are collected
        bool bOutlines = false;                                 // only their outlines are lit, and glow
    };

    // Each key's light: the note pressing it (from when), or, let go, when and with what light left
    class KeyLights
    {
    public:
        void Reset();
        // aSlot, aStart: FastEngine::GetKeySlots (-1: not pressed). A frame earlier than the last one
        // (a jump back) forgets the keys let go.
        void Update( const EffectSettings &fx, long long llNow, const int aSlot[256], const long long aStart[256] );
        float Light( int k ) const { return m_aLight[k]; }
        int Slot( int k ) const { return m_aKey[k].iSlot; }     // the note's (pressed or fading), or -1
        bool Pressed( int k ) const { return m_aKey[k].bDown; }

    private:
        struct Key { int iSlot = -1; bool bDown = false; long long llOn = 0, llOff = 0; float fAtOff = 0.0f; };
        Key m_aKey[256];
        float m_aLight[256] = {};
        long long m_llLast = 0;
        bool m_bAny = false;
    };
}
