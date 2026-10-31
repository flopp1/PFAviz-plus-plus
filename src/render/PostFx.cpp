// The effects' post pass (Renderer::BeginPost / DrawGlow / EndPost; visual/Effects.h). The frame is
// drawn into a texture; the glow's sources (the rects given to DrawGlow, drawn at full size so thin
// ones are not lost, and the frame's own bright parts) are box-filtered down to a quarter of the size
// (and again to an eighth for a wide glow), blurred there with a Gaussian in passes, and added over the
// frame as it is drawn into the target, with the vignette. The shaders are ps_2_0 (the vertices are
// pre-transformed), so each stays within its 64 arithmetic instructions.
#include "render/Renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // Down: 4 bilinear taps (c0 to c3: their offsets) averaged, each above the threshold first
    // (c4: x the threshold, y 1 / (1 - threshold) / 4)
    const char *DownPS = R"(
sampler2D s0 : register( s0 );
float4 cTap[4] : register( c0 );
float4 cT : register( c4 );
float4 main( float2 t : TEXCOORD0 ) : COLOR
{
    float3 a = 0;
    for ( int i = 0; i < 4; i++ ) a += max( tex2D( s0, t + cTap[i].xy ).rgb - cT.x, 0 );
    return float4( a * cT.y, 1 );
}
)";
    // Blur: 13 bilinear taps of a Gaussian along one axis (xy: the offset, z: the weight)
    const char *BlurPS = R"(
sampler2D s0 : register( s0 );
float4 cTap[13] : register( c0 );
float4 main( float2 t : TEXCOORD0 ) : COLOR
{
    float3 a = 0;
    for ( int i = 0; i < 13; i++ ) a += tex2D( s0, t + cTap[i].xy ).rgb * cTap[i].z;
    return float4( a, 1 );
}
)";
    // Composite: the frame, plus the glow times x, darkened towards the corners by y
    const char *CompositePS = R"(
sampler2D s0 : register( s0 );
sampler2D s1 : register( s1 );
float4 c0 : register( c0 );
float4 main( float2 t : TEXCOORD0 ) : COLOR
{
    float3 a = tex2D( s0, t ).rgb + tex2D( s1, t ).rgb * c0.x;
    float d = length( ( t - 0.5 ) * 2 ) * 0.70710678;
    a *= 1 - c0.y * smoothstep( 0.25, 1.0, d );
    return float4( a, 1 );
}
)";
}

bool Renderer::CreatePostShaders()
{
    if ( m_pCompositePS ) return true;
    if ( m_bPostFailed ) return false;
    m_bPostFailed = true;
    D3DCAPS9 caps;
    if ( FAILED( m_pDevice->GetDeviceCaps( &caps ) ) || caps.PixelShaderVersion < D3DPS_VERSION( 2, 0 ) ) return false;
    static HMODULE hCompiler = LoadLibraryW( L"d3dcompiler_47.dll" );
    pD3DCompile pCompile = hCompiler ? (pD3DCompile)(void *)GetProcAddress( hCompiler, "D3DCompile" ) : nullptr;
    if ( !pCompile ) return false;
    auto Make = [&]( const char *sSrc, IDirect3DPixelShader9 *&pPS ) {
        ID3DBlob *pCode = nullptr;
        bool bOk = SUCCEEDED( pCompile( sSrc, strlen( sSrc ), "post.ps", nullptr, nullptr, "main", "ps_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pCode, nullptr ) ) &&
                   SUCCEEDED( m_pDevice->CreatePixelShader( (const DWORD *)pCode->GetBufferPointer(), &pPS ) );
        if ( pCode ) pCode->Release();
        return bOk;
    };
    if ( !Make( DownPS, m_pDownPS ) || !Make( BlurPS, m_pBlurPS ) || !Make( CompositePS, m_pCompositePS ) )
    {
        for ( IDirect3DPixelShader9 **pp : { &m_pDownPS, &m_pBlurPS, &m_pCompositePS } )
            if ( *pp ) { ( *pp )->Release(); *pp = nullptr; }
        return false;
    }
    m_bPostFailed = false;
    return true;
}

// A render target texture of the size wanted (kept while the size stays)
bool Renderer::PostTarget( IDirect3DTexture9 *&pTex, IDirect3DSurface9 *&pSurf, int &iW, int &iH, int iWantW, int iWantH )
{
    if ( pTex && iW == iWantW && iH == iWantH ) return true;
    if ( pSurf ) { pSurf->Release(); pSurf = nullptr; }
    if ( pTex ) { pTex->Release(); pTex = nullptr; }
    iW = iH = 0;
    if ( FAILED( m_pDevice->CreateTexture( iWantW, iWantH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &pTex, nullptr ) ) ) return false;
    if ( FAILED( pTex->GetSurfaceLevel( 0, &pSurf ) ) ) { pTex->Release(); pTex = nullptr; return false; }
    iW = iWantW;
    iH = iWantH;
    return true;
}

void Renderer::ReleasePostTargets()
{
    for ( PostTex *p : { &m_PostScene, &m_PostSource, &m_PostQuarter, &m_aPostGlow[0], &m_aPostGlow[1] } )
    {
        if ( p->pSurf ) p->pSurf->Release();
        if ( p->pTex ) p->pTex->Release();
        *p = PostTex();
    }
    if ( m_pPostTarget ) { m_pPostTarget->Release(); m_pPostTarget = nullptr; }
    m_bInPost = false;
}

bool Renderer::BeginPost( const PostParams &p )
{
    if ( m_bInPost || !m_pDevice || !CreatePostShaders() ) return false;
    FlushBuffer();
    IDirect3DSurface9 *pTarget = nullptr;
    D3DSURFACE_DESC d;
    if ( FAILED( m_pDevice->GetRenderTarget( 0, &pTarget ) ) || FAILED( pTarget->GetDesc( &d ) ) )
    {
        if ( pTarget ) pTarget->Release();
        return false;
    }
    const int iW = (int)d.Width, iH = (int)d.Height;
    // The glow is blurred at a quarter of the frame's size, or an eighth for a wide one
    m_Post = p;
    m_iPostDiv = p.fRadius > 48.0f ? 8 : 4;
    bool bOk = PostTarget( m_PostScene.pTex, m_PostScene.pSurf, m_PostScene.iW, m_PostScene.iH, iW, iH );
    if ( bOk && p.fBloom > 0.0f )
    {
        const int iQW = ( iW + 3 ) / 4, iQH = ( iH + 3 ) / 4;
        const int iGW = ( iW + m_iPostDiv - 1 ) / m_iPostDiv, iGH = ( iH + m_iPostDiv - 1 ) / m_iPostDiv;
        bOk = PostTarget( m_PostSource.pTex, m_PostSource.pSurf, m_PostSource.iW, m_PostSource.iH, iW, iH ) &&
              ( m_iPostDiv == 4 || PostTarget( m_PostQuarter.pTex, m_PostQuarter.pSurf, m_PostQuarter.iW, m_PostQuarter.iH, iQW, iQH ) );
        for ( PostTex &g : m_aPostGlow ) bOk = bOk && PostTarget( g.pTex, g.pSurf, g.iW, g.iH, iGW, iGH );
    }
    if ( !bOk )
    {
        pTarget->Release();
        ReleasePostTargets();
        return false;
    }
    m_pPostTarget = pTarget;
    m_pDevice->SetRenderTarget( 0, m_PostScene.pSurf );
    m_bInPost = true;
    m_bSourceUsed = false;
    return true;
}

void Renderer::DrawGlow( const GlowRect *pRects, size_t iCount )
{
    if ( !m_bInPost || m_Post.fBloom <= 0.0f || !iCount ) return;
    FlushBuffer();
    m_pDevice->SetRenderTarget( 0, m_PostSource.pSurf );
    if ( !m_bSourceUsed ) m_pDevice->Clear( 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0 );
    m_bSourceUsed = true;
    for ( size_t i = 0; i < iCount; i++ )
    {
        const GlowRect &r = pRects[i];
        DrawRect( r.x, r.y, r.cx, r.cy, r.color & 0x00FFFFFF );
    }
    FlushBuffer();
    m_pDevice->SetRenderTarget( 0, m_PostScene.pSurf );
}

// A quad over all of pTarget (iW x iH) with texture coordinates 0 to 1, through the pixel shader set
void Renderer::PostQuad( IDirect3DSurface9 *pTarget, int iW, int iH )
{
    m_pDevice->SetRenderTarget( 0, pTarget );
    struct V { float x, y, z, h, u, v; };
    const float x1 = iW - 0.5f, y1 = iH - 0.5f;
    const V v[4] = { { -0.5f, -0.5f, 0.5f, 1.0f, 0, 0 }, { x1, -0.5f, 0.5f, 1.0f, 1, 0 },
                     { -0.5f, y1, 0.5f, 1.0f, 0, 1 }, { x1, y1, 0.5f, 1.0f, 1, 1 } };
    m_pDevice->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
    m_pDevice->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, v, sizeof( V ) );
}

void Renderer::EndPost()
{
    if ( !m_bInPost ) return;
    FlushBuffer();
    m_bInPost = false;
    for ( int s = 0; s < 2; s++ )
    {
        m_pDevice->SetSamplerState( s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
        m_pDevice->SetSamplerState( s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
        m_pDevice->SetSamplerState( s, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
        m_pDevice->SetSamplerState( s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
        m_pDevice->SetSamplerState( s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
    }
    m_pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );

    const bool bBloom = m_Post.fBloom > 0.0f && ( m_bSourceUsed || m_Post.bSceneGlow );
    if ( bBloom )
    {
        PostTex &g0 = m_aPostGlow[0], &g1 = m_aPostGlow[1];
        // Down by 4 (into the glow, or for a wide one into the quarter first): 4 bilinear taps a texel
        // off each output pixel's centre both ways, each the average of 2 x 2, so of 4 x 4 in all. A
        // wide glow then goes down by 2 more: the 4 taps on the centre, the average of 2 x 2.
        PostTex &q = m_iPostDiv == 4 ? g0 : m_PostQuarter;
        m_pDevice->SetPixelShader( m_pDownPS );
        auto Down = [&]( const PostTex &from, PostTex &to, float fOff, float fThreshold ) {
            float aTap[5][4] = {};
            for ( int i = 0; i < 4; i++ )
            {
                aTap[i][0] = ( i & 1 ? fOff : -fOff ) / from.iW;
                aTap[i][1] = ( i & 2 ? fOff : -fOff ) / from.iH;
            }
            aTap[4][0] = fThreshold;
            aTap[4][1] = 0.25f / ( 1.0f - fThreshold );
            m_pDevice->SetPixelShaderConstantF( 0, &aTap[0][0], 5 );
            m_pDevice->SetTexture( 0, from.pTex );
            PostQuad( to.pSurf, to.iW, to.iH );
        };
        m_pDevice->SetRenderTarget( 0, q.pSurf );
        m_pDevice->Clear( 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0 );
        if ( m_bSourceUsed ) Down( m_PostSource, q, 1.0f, 0.0f );
        if ( m_Post.bSceneGlow )
        {
            // Added to what the rects gave
            m_pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
            m_pDevice->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ONE );
            m_pDevice->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ONE );
            Down( m_PostScene, q, 1.0f, std::clamp( m_Post.fThreshold, 0.0f, 0.99f ) );
            m_pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
        }
        if ( &q != &g0 ) Down( q, g0, 0.0f, 0.0f );

        // The blur: a Gaussian of sigma = radius / 2, in passes of at most sigma 6 glow pixels (13
        // bilinear taps cover offsets to 12; the passes' variances add up)
        const double dSigma = std::max( 0.5, m_Post.fRadius / 2.0 / m_iPostDiv );
        const int iPasses = std::max( 1, (int)std::ceil( ( dSigma / 6.0 ) * ( dSigma / 6.0 ) ) );
        const double dPass = dSigma / std::sqrt( (double)iPasses );
        double aW[13];
        double dSum = 0.0;
        for ( int i = 0; i <= 12; i++ ) { aW[i] = std::exp( -i * i / ( 2.0 * dPass * dPass ) ); dSum += i ? 2.0 * aW[i] : aW[i]; }
        for ( double &w : aW ) w /= dSum;
        // Pairs of neighbouring offsets as one bilinear tap between them
        double aOff[7], aPairW[7];
        aOff[0] = 0.0;
        aPairW[0] = aW[0];
        for ( int p = 1; p <= 6; p++ )
        {
            int i1 = 2 * p - 1, i2 = 2 * p;
            aPairW[p] = aW[i1] + aW[i2];
            aOff[p] = aPairW[p] > 0.0 ? ( i1 * aW[i1] + i2 * aW[i2] ) / aPairW[p] : i1;
        }
        m_pDevice->SetPixelShader( m_pBlurPS );
        auto Blur = [&]( PostTex &from, PostTex &to, bool bVertical ) {
            float aBlur[13][4] = {};
            const float fU = bVertical ? 0.0f : 1.0f / from.iW, fV = bVertical ? 1.0f / from.iH : 0.0f;
            for ( int t = 0; t < 13; t++ )
            {
                int p = ( t + 1 ) / 2;
                float fSign = t == 0 ? 0.0f : ( t & 1 ) ? 1.0f : -1.0f;
                aBlur[t][0] = (float)( fSign * aOff[p] ) * fU;
                aBlur[t][1] = (float)( fSign * aOff[p] ) * fV;
                aBlur[t][2] = (float)aPairW[p];
            }
            m_pDevice->SetPixelShaderConstantF( 0, &aBlur[0][0], 13 );
            m_pDevice->SetTexture( 0, from.pTex );
            PostQuad( to.pSurf, to.iW, to.iH );
        };
        for ( int i = 0; i < iPasses; i++ )
        {
            Blur( g0, g1, false );
            Blur( g1, g0, true );
        }
    }

    // Into the target
    const float aComp[4] = { bBloom ? m_Post.fBloom : 0.0f, m_Post.fVignette, 0.0f, 0.0f };
    m_pDevice->SetPixelShader( m_pCompositePS );
    m_pDevice->SetPixelShaderConstantF( 0, aComp, 1 );
    m_pDevice->SetTexture( 0, m_PostScene.pTex );
    m_pDevice->SetTexture( 1, bBloom ? m_aPostGlow[0].pTex : m_PostScene.pTex );
    PostQuad( m_pPostTarget, m_PostScene.iW, m_PostScene.iH );

    m_pDevice->SetPixelShader( nullptr );
    m_pDevice->SetTexture( 0, nullptr );
    m_pDevice->SetTexture( 1, nullptr );
    m_pPostTarget->Release();
    m_pPostTarget = nullptr;
    RestoreStates();
}
