#include "render/FrameCapture.h"

#include <algorithm>
#include <cstring>

#include "render/Renderer.h"

namespace
{
    // ps_2_0, compiled from (D3DX, once; no D3DX is needed at run time):
    //   sampler2D s0 : register( s0 );
    //   float4 c0 : register( c0 );   // rgb: the coefficients; a: the offset
    //   float4 c1 : register( c1 );   // x: the step between the four samples, in texture coordinates
    //   float4 main( float2 t : TEXCOORD0 ) : COLOR
    //   {
    //       float2 d = float2( c1.x, 0 );
    //       float4 r;
    //       r.b = dot( tex2D( s0, t - 1.5 * d ).rgb, c0.rgb ) + c0.a;
    //       r.g = dot( tex2D( s0, t - 0.5 * d ).rgb, c0.rgb ) + c0.a;
    //       r.r = dot( tex2D( s0, t + 0.5 * d ).rgb, c0.rgb ) + c0.a;
    //       r.a = dot( tex2D( s0, t + 1.5 * d ).rgb, c0.rgb ) + c0.a;
    //       return r;
    //   }
    // Each output pixel holds four consecutive samples of a plane in memory order (B, G, R, A), so a
    // plane of width w is read back from a w / 4 wide A8R8G8B8 target exactly as its bytes.
    const DWORD aPackShader[] = {
        0xFFFF0200, 0x0030FFFE, 0x42415443, 0x0000001C, 0x0000008B, 0xFFFF0200, 0x00000003, 0x0000001C,
        0x00000100, 0x00000084, 0x00000058, 0x00000002, 0x00020001, 0x0000005C, 0x00000000, 0x0000006C,
        0x00010002, 0x00060001, 0x0000005C, 0x00000000, 0x0000006F, 0x00000003, 0x00020001, 0x00000074,
        0x00000000, 0xAB003063, 0x00030001, 0x00040001, 0x00000001, 0x00000000, 0x73003163, 0xABAB0030,
        0x000C0004, 0x00010001, 0x00000001, 0x00000000, 0x325F7370, 0x4D00305F, 0x6F726369, 0x74666F73,
        0x29522820, 0x534C4820, 0x6853204C, 0x72656461, 0x6D6F4320, 0x656C6970, 0x2E392072, 0x392E3932,
        0x332E3235, 0x00313131, 0x05000051, 0xA00F0002, 0x3FC00000, 0x00000000, 0x3F000000, 0x00000000,
        0x0200001F, 0x80000000, 0xB0030000, 0x0200001F, 0x90000000, 0xA00F0800, 0x02000001, 0x80050000,
        0xA0E40002, 0x03000005, 0x80010000, 0x80000000, 0xA0000001, 0x02000001, 0x80020000, 0xA0550002,
        0x03000002, 0x80030001, 0x81E40000, 0xB0E40000, 0x03000002, 0x80030000, 0x80E40000, 0xB0E40000,
        0x03000005, 0x80010002, 0x80AA0000, 0xA0000001, 0x02000001, 0x80020002, 0xA0550002, 0x03000002,
        0x80030003, 0x81E40002, 0xB0E40000, 0x03000002, 0x80030002, 0x80E40002, 0xB0E40000, 0x03000042,
        0x800F0000, 0x80E40000, 0xA0E40800, 0x03000042, 0x800F0001, 0x80E40001, 0xA0E40800, 0x03000042,
        0x800F0002, 0x80E40002, 0xA0E40800, 0x03000042, 0x800F0003, 0x80E40003, 0xA0E40800, 0x03000008,
        0x80080001, 0x80E40000, 0xA0E40000, 0x03000002, 0x80080000, 0x80FF0001, 0xA0FF0000, 0x03000008,
        0x80080002, 0x80E40001, 0xA0E40000, 0x03000002, 0x80040000, 0x80FF0002, 0xA0FF0000, 0x03000008,
        0x80080003, 0x80E40002, 0xA0E40000, 0x03000002, 0x80010000, 0x80FF0003, 0xA0FF0000, 0x03000008,
        0x80010001, 0x80E40003, 0xA0E40000, 0x03000002, 0x80020000, 0x80000001, 0xA0FF0000, 0x02000001,
        0x800F0800, 0x80E40000, 0x0000FFFF,
    };

    // BT.709, limited range, as fractions of 255: Y = 16 + 219 Y', Cb = 128 + 224 (B' - Y') / 1.8556,
    // Cr = 128 + 224 (R' - Y') / 1.5748
    const float Kr = 0.2126f, Kg = 0.7152f, Kb = 0.0722f;
    const float aCoefY[4] = { 219.0f / 255 * Kr, 219.0f / 255 * Kg, 219.0f / 255 * Kb, 16.0f / 255 };
    const float sCb = 224.0f / 255 / 1.8556f, sCr = 224.0f / 255 / 1.5748f;
    const float aCoefU[4] = { -Kr * sCb, -Kg * sCb, ( 1 - Kb ) * sCb, 128.0f / 255 };
    const float aCoefV[4] = { ( 1 - Kr ) * sCr, -Kg * sCr, -Kb * sCr, 128.0f / 255 };
}

FrameCapture::~FrameCapture()
{
    if ( m_pDevice && m_pOldTarget ) m_pDevice->SetRenderTarget( 0, m_pOldTarget );
    Release();
}

void FrameCapture::Release()
{
    for ( Slot &s : m_vSlots )
    {
        if ( s.pTarget ) s.pTarget->Release();
        for ( auto p : s.apPlane ) if ( p ) p->Release();
        for ( auto p : s.apSys ) if ( p ) p->Release();
    }
    m_vSlots.clear();
    if ( m_pSceneSurface ) m_pSceneSurface->Release();
    if ( m_pScene ) m_pScene->Release();
    if ( m_pPack ) m_pPack->Release();
    if ( m_pOldTarget ) m_pOldTarget->Release();
    m_pSceneSurface = nullptr;
    m_pScene = nullptr;
    m_pPack = nullptr;
    m_pOldTarget = nullptr;
}

bool FrameCapture::Init( Renderer &r, int iWidth, int iHeight, bool bYuv, int iDepth, std::wstring &sError )
{
    Release();
    m_pRenderer = &r;
    m_pDevice = r.GetDevice();
    m_iW = iWidth;
    m_iH = iHeight;
    m_iNext = m_iQueued = 0;
    D3DCAPS9 caps = {};
    m_pDevice->GetDeviceCaps( &caps );
    m_bYuv = bYuv && iWidth % 8 == 0 && iHeight % 2 == 0 && caps.PixelShaderVersion >= D3DPS_VERSION( 2, 0 ) &&
             SUCCEEDED( m_pDevice->CreatePixelShader( aPackShader, &m_pPack ) ) &&
             SUCCEEDED( m_pDevice->CreateTexture( iWidth, iHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_pScene, nullptr ) ) &&
             SUCCEEDED( m_pScene->GetSurfaceLevel( 0, &m_pSceneSurface ) );
    m_pDevice->GetRenderTarget( 0, &m_pOldTarget );

    auto Target = [&]( int w, int h, D3DFORMAT fmt, IDirect3DSurface9 **pp ) {
        return SUCCEEDED( m_pDevice->CreateRenderTarget( w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, pp, nullptr ) );
    };
    auto Sys = [&]( int w, int h, D3DFORMAT fmt, IDirect3DSurface9 **pp ) {
        return SUCCEEDED( m_pDevice->CreateOffscreenPlainSurface( w, h, fmt, D3DPOOL_SYSTEMMEM, pp, nullptr ) );
    };
    m_vSlots.resize( std::max( 1, iDepth ) );
    for ( Slot &s : m_vSlots )
    {
        bool bOk;
        if ( m_bYuv )
            bOk = Target( iWidth / 4, iHeight, D3DFMT_A8R8G8B8, &s.apPlane[0] ) && Target( iWidth / 8, iHeight / 2, D3DFMT_A8R8G8B8, &s.apPlane[1] ) &&
                  Target( iWidth / 8, iHeight / 2, D3DFMT_A8R8G8B8, &s.apPlane[2] ) && Sys( iWidth / 4, iHeight, D3DFMT_A8R8G8B8, &s.apSys[0] ) &&
                  Sys( iWidth / 8, iHeight / 2, D3DFMT_A8R8G8B8, &s.apSys[1] ) && Sys( iWidth / 8, iHeight / 2, D3DFMT_A8R8G8B8, &s.apSys[2] );
        else
            bOk = Target( iWidth, iHeight, D3DFMT_X8R8G8B8, &s.pTarget ) && Sys( iWidth, iHeight, D3DFMT_X8R8G8B8, &s.apSys[0] );
        if ( !bOk )
        {
            sError = L"Direct3D could not create the surfaces to read the frames back.";
            Release();
            return false;
        }
    }
    return true;
}

void FrameCapture::Begin()
{
    m_pDevice->SetRenderTarget( 0, m_bYuv ? m_pSceneSurface : m_vSlots[m_iNext].pTarget );
}

// One plane: every pixel of pTarget gets four samples of the frame, fStep apart (texture coordinates)
void FrameCapture::Pack( IDirect3DSurface9 *pTarget, int iPlaneW, int iPlaneH, const float aCoef[4], float fStep, bool bLinear )
{
    m_pDevice->SetRenderTarget( 0, pTarget );
    const float aStep[4] = { fStep, 0, 0, 0 };
    m_pDevice->SetPixelShaderConstantF( 0, aCoef, 1 );
    m_pDevice->SetPixelShaderConstantF( 1, aStep, 1 );
    D3DTEXTUREFILTERTYPE f = bLinear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    m_pDevice->SetSamplerState( 0, D3DSAMP_MINFILTER, f );
    m_pDevice->SetSamplerState( 0, D3DSAMP_MAGFILTER, f );
    struct V { float x, y, z, h, u, v; };
    float w = (float)iPlaneW - 0.5f, h = (float)iPlaneH - 0.5f;
    const V aQuad[4] = { { -0.5f, -0.5f, 0.5f, 1.0f, 0, 0 }, { w, -0.5f, 0.5f, 1.0f, 1, 0 }, { -0.5f, h, 0.5f, 1.0f, 0, 1 }, { w, h, 0.5f, 1.0f, 1, 1 } };
    m_pDevice->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, aQuad, sizeof( V ) );
}

void FrameCapture::End()
{
    Slot &s = m_vSlots[m_iNext];
    if ( m_bYuv )
    {
        // Y: four neighbouring pixels a sample (point sampling). U, V: a sample is the average of a
        // 2 x 2 block, which bilinear filtering gives exactly at the block's centre
        m_pDevice->BeginScene();
        m_pDevice->SetPixelShader( m_pPack );
        m_pDevice->SetTexture( 0, m_pScene );
        m_pDevice->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
        m_pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
        m_pDevice->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
        m_pDevice->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
        m_pDevice->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
        Pack( s.apPlane[0], m_iW / 4, m_iH, aCoefY, 1.0f / m_iW, false );
        Pack( s.apPlane[1], m_iW / 8, m_iH / 2, aCoefU, 2.0f / m_iW, true );
        Pack( s.apPlane[2], m_iW / 8, m_iH / 2, aCoefV, 2.0f / m_iW, true );
        m_pDevice->SetPixelShader( nullptr );
        m_pDevice->SetTexture( 0, nullptr );
        m_pDevice->EndScene();
        m_pRenderer->RestoreStates();
        for ( int i = 0; i < 3; i++ ) m_pDevice->GetRenderTargetData( s.apPlane[i], s.apSys[i] );
    }
    else
        m_pDevice->GetRenderTargetData( s.pTarget, s.apSys[0] );
    m_iNext = ( m_iNext + 1 ) % (int)m_vSlots.size();
    m_iQueued++;
}

bool FrameCapture::Read( uint8_t *pOut )
{
    if ( !m_iQueued ) return false;
    int n = (int)m_vSlots.size();
    Slot &s = m_vSlots[( m_iNext - m_iQueued + n ) % n];
    m_iQueued--;
    // Plane i: its rows of iRowBytes bytes
    auto Copy = [&]( IDirect3DSurface9 *pSys, int iRowBytes, int iRows, uint8_t *pDst ) {
        D3DLOCKED_RECT lr;
        if ( FAILED( pSys->LockRect( &lr, nullptr, D3DLOCK_READONLY ) ) ) return false;
        for ( int y = 0; y < iRows; y++ ) memcpy( pDst + (size_t)y * iRowBytes, (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, iRowBytes );
        pSys->UnlockRect();
        return true;
    };
    if ( !m_bYuv ) return Copy( s.apSys[0], m_iW * 4, m_iH, pOut );
    size_t iY = (size_t)m_iW * m_iH, iC = iY / 4;
    return Copy( s.apSys[0], m_iW, m_iH, pOut ) && Copy( s.apSys[1], m_iW / 2, m_iH / 2, pOut + iY ) &&
           Copy( s.apSys[2], m_iW / 2, m_iH / 2, pOut + iY + iC );
}
