// Eye candy (Preferences > Effects; core/Config.h EffectSettings): the colours a frame is drawn with
// under the effects, the light of notes and keys over time, and what the bloom glows from. With every
// effect off nothing here is used, and frames are drawn exactly as before.
//
// Light: everything is drawn by a light of 0 to 1, the same way for each part (notes, short notes,
// keys, background): at 0 darkened by the part's dark room, at 1 its own colour lightened by its
// light-up, and between the two in proportion; it glows by its light too. A note rests at the rest
// light; one playing at the hit line rises from there to 1 and falls, while it is held, along the
// decay curve to its floor; the key it presses likewise, and a key let go keeps its light a while,
// falling to 0 over the release time.
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
    // c's brightness times f: above 1, what goes past full in one channel is shared out to the others,
    // so a colour at full goes on towards white
    D3DCOLOR Bright( D3DCOLOR c, float f );

    // The light of a note held for llUs (1 at its start), and what is left of fFrom llUs after its key
    // was let go
    float Held( const EffectSettings &fx, long long llUs );
    float Released( const EffectSettings &fx, float fFrom, long long llUs );

    // The frame's colours under the effects. Prepare once a frame: the notes' colours at rest are copied
    // only when what they depend on changed (uTracksGen: the caller's count of changes to the tracks).
    class Look
    {
    public:
        void Prepare( const EffectSettings &fx, const std::vector< TrackSettings > &vTracks, unsigned uTracksGen, const SceneColors &c );

        float BackgroundDark() const { return m_aKeep[EffectSettings::PartBackground]; }  // what the background (and its image) is multiplied by
        const SceneColors &Colors() const { return m_Colors; }  // the scene's (darkened; the flat look's lines)
        float RestLight() const { return m_fRest; }
        // The share of a note's light its outline and its inside get (EffectSettings::iLightOutlines,
        // iLightInsides; the outline's at most 1)
        float OutlineShare() const { return std::min( m_fOutline, 1.0f ); }
        float InsideShare() const { return m_fInside; }
        bool Glows( int ePart ) const { return m_aGlow[ePart] > 0.0f; }
        // The notes' colours at rest, PartNotes or PartShort (vTracks itself when nothing changes them)
        const std::vector< TrackSettings > &Rest( int ePart, const std::vector< TrackSettings > &vTracks ) const;
        // A part at a light, from its own colours: from its unlit colours, or from 'from' (a key let go:
        // its own colours, towards its note's)
        ChannelSettings Lit( int ePart, const ChannelSettings &base, float fLight ) const { return Lit( ePart, base, Unlit( ePart, base ), fLight ); }
        ChannelSettings Lit( int ePart, const ChannelSettings &base, const ChannelSettings &from, float fLight ) const;
        D3DCOLOR Glow( int ePart, const ChannelSettings &base, float fLight ) const;    // 0: none
        D3DCOLOR BarGlow() const { return m_cBarGlow; }

    private:
        ChannelSettings Unlit( int ePart, const ChannelSettings &cs ) const;    // darkened, flattened

        float m_aKeep[EffectSettings::PartCount] = { 1.0f, 1.0f, 1.0f, 1.0f };  // 1 - dark room
        float m_aUp[EffectSettings::PartCount] = {}, m_aGlow[EffectSettings::PartCount] = {};
        float m_fRest = 0.0f, m_fOutline = 1.0f, m_fInside = 1.0f;
        D3DCOLOR m_cBarGlow = 0;
        bool m_bFlat = false;
        SceneColors m_Colors;
        // The rest colours: notes', and short notes' when they differ
        bool m_abOwnRest[2] = {};
        std::vector< TrackSettings > m_avRest[2];
        std::vector< long long > m_vRestKey;                    // what they were made from
    };

    // What FastEngine needs to draw the notes by their light (and collect their glow)
    struct NoteLight
    {
        const EffectSettings *pFx = nullptr;
        const Look *pLook = nullptr;
        const std::vector< TrackSettings > *pBase = nullptr;    // the tracks' own colours
        const std::vector< TrackSettings > *pShortRest = nullptr;   // short notes' at rest (the notes' are the ones drawn with)
        long long llNow = 0;                                    // the frame's time: playing notes started at or before it and end after it
        bool bGlow = false;                                     // glow rects are collected
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
        // The key's part of the hit line: 1 while pressed, then falling over its own fade; and its note's
        // slot (or -1)
        float HitLight( int k ) const { return m_aHit[k]; }
        int HitSlot( int k ) const { return m_aKey[k].iHitSlot; }

    private:
        struct Key { int iSlot = -1, iHitSlot = -1; bool bDown = false; long long llOn = 0, llOff = 0; float fAtOff = 0.0f; };
        Key m_aKey[256];
        float m_aLight[256] = {}, m_aHit[256] = {};
        long long m_llLast = 0;
        bool m_bAny = false;
    };
}
