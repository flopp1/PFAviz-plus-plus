// Frames rendered off-screen for the video renderer and read back to system memory a few frames
// later (a ring of render targets, each read when it comes round again), so the GPU works ahead
// instead of stopping at every readback. Frames come out as BGRX, or, with bYuv, as planar YUV 4:2:0
// (BT.709, limited range) packed on the GPU: 1.5 bytes a pixel instead of 4 to read back and pipe.
#pragma once

#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>

class Renderer;

class FrameCapture
{
public:
    ~FrameCapture();

    // iDepth frames in flight (1: each frame is read back at once). bYuv needs a width that is a
    // multiple of 8; Init falls back to BGRX otherwise, or when the device cannot run the packing pass
    bool Init( Renderer &r, int iWidth, int iHeight, bool bYuv, int iDepth, std::wstring &sError );
    bool Yuv() const { return m_bYuv; }
    size_t FrameBytes() const { return m_bYuv ? (size_t)m_iW * m_iH * 3 / 2 : (size_t)m_iW * m_iH * 4; }

    // A frame: Begin (it is drawn into this slot's target), draw it, End (packed and queued for readback)
    void Begin();
    void End();
    // A frame whose readback has to be taken before the next Begin (the ring is full), or any queued
    // frame with bFlush; Read waits for the oldest and copies it into pOut (FrameBytes)
    bool Ready( bool bFlush ) const { return m_iQueued > 0 && ( bFlush || m_iQueued == (int)m_vSlots.size() ); }
    bool Read( uint8_t *pOut );

private:
    struct Slot
    {
        IDirect3DSurface9 *pTarget = nullptr;           // BGRX: the frame
        IDirect3DSurface9 *apPlane[3] = {};             // YUV: Y, U, V packed four samples a pixel
        IDirect3DSurface9 *apSys[3] = {};               // system memory copies (BGRX: [0] only)
    };
    void Release();
    void Pack( IDirect3DSurface9 *pTarget, int iPlaneW, int iPlaneH, const float aCoef[4], float fStep, bool bLinear );

    Renderer *m_pRenderer = nullptr;
    IDirect3DDevice9 *m_pDevice = nullptr;
    int m_iW = 0, m_iH = 0;
    bool m_bYuv = false;
    IDirect3DTexture9 *m_pScene = nullptr;              // YUV: the frame, sampled by the packing pass
    IDirect3DSurface9 *m_pSceneSurface = nullptr;
    IDirect3DPixelShader9 *m_pPack = nullptr;
    IDirect3DSurface9 *m_pOldTarget = nullptr;
    std::vector< Slot > m_vSlots;
    int m_iNext = 0;                                    // slot of the next frame
    int m_iQueued = 0;                                  // frames drawn and not read yet
};
