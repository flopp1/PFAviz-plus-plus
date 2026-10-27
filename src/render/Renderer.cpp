#include "render/Renderer.h"

#include "render/Image.h"
#include "render/Text.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // Instanced notes. Each corner of the template picks its x column, its y and its colour with
    // one-hot weights, so the selection is exact (products with 0 and 1, sums with 0). The columns
    // arrive in clip space; y is mapped here.
    const char *InstanceVS = R"(
float4 cCols[128] : register( c0 );     // clip-space x: outer left, outer right, inner left, inner right
float4 cY : register( c128 );           // y scale, y offset
struct VIn
{
    float4 xs : POSITION0;
    float4 ys : TEXCOORD0;
    float4 cs : TEXCOORD1;
    float4 y : TEXCOORD2;
    float4 c0 : TEXCOORD3;
    float4 c1 : TEXCOORD4;
    float4 c2 : TEXCOORD5;
    float key : TEXCOORD6;
};
struct VOut { float4 pos : POSITION; float4 col : COLOR0; };
VOut main( VIn i )
{
    VOut o;
    float x = dot( cCols[(int)i.key], i.xs );
    float y = dot( i.y, i.ys );
    o.pos = float4( x, y * cY.x + cY.y, 0.5, 1.0 );
    o.col = i.c0 * i.cs.x + i.c1 * i.cs.y + i.c2 * i.cs.z;
    return o;
}
)";
    const char *InstancePS = R"(
float4 main( float4 c : COLOR0 ) : COLOR { return c; }
)";

    struct TemplateVertex { float xs[4], ys[4], cs[4]; };
}

Renderer::Renderer() { m_vVerts.reserve( VBVertices ); }

Renderer::~Renderer()
{
    DestroyDeviceObjects();
    if ( m_pInstVS ) m_pInstVS->Release();
    if ( m_pInstPS ) m_pInstPS->Release();
    if ( m_pInstDecl ) m_pInstDecl->Release();
    delete m_pText;
    if ( m_pDevice ) m_pDevice->Release();
    if ( m_pD3D ) m_pD3D->Release();
}

void Renderer::FillPresentParams()
{
    ZeroMemory( &m_d3dPP, sizeof( m_d3dPP ) );
    m_d3dPP.Windowed = TRUE;
    m_d3dPP.BackBufferWidth = 0;
    m_d3dPP.BackBufferHeight = 0;
    m_d3dPP.PresentationInterval = ( m_bLimitFPS ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE );
    if ( m_bOffscreen )
    {
        m_d3dPP.SwapEffect = D3DSWAPEFFECT_DISCARD;
        m_d3dPP.BackBufferFormat = D3DFMT_X8R8G8B8;
        m_d3dPP.BackBufferWidth = m_iOffscreenW;
        m_d3dPP.BackBufferHeight = m_iOffscreenH;
    }
    else if ( m_bFlipEx )
    {
        m_d3dPP.SwapEffect = D3DSWAPEFFECT_FLIPEX;
        m_d3dPP.BackBufferFormat = D3DFMT_X8R8G8B8;
        m_d3dPP.BackBufferCount = 2;
    }
    else
    {
        m_d3dPP.SwapEffect = D3DSWAPEFFECT_DISCARD;
        m_d3dPP.BackBufferFormat = D3DFMT_UNKNOWN;
    }
}

HRESULT Renderer::Init( HWND hWnd, bool bLimitFPS )
{
    HRESULT hr;
    m_hWnd = hWnd;
    m_bLimitFPS = bLimitFPS;
    if ( FAILED( hr = Direct3DCreate9Ex( D3D_SDK_VERSION, &m_pD3D ) ) )
        return hr;

    const DWORD dwFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    for ( int iTry = 0; iTry < 2; iTry++ )
    {
        m_bFlipEx = ( iTry == 0 );
        FillPresentParams();
        hr = m_pD3D->CreateDeviceEx( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, dwFlags, &m_d3dPP, nullptr, &m_pDevice );
        if ( SUCCEEDED( hr ) ) break;
    }
    if ( FAILED( hr ) ) return hr;

    m_pText = TextRenderer::Create( m_pDevice );
    if ( FAILED( hr = RestoreDeviceObjects() ) )
        return hr;

    m_iBufferWidth = m_d3dPP.BackBufferWidth;
    m_iBufferHeight = m_d3dPP.BackBufferHeight;
    m_bIsDeviceValid = true;
    return S_OK;
}

HRESULT Renderer::InitOffscreen( int iWidth, int iHeight )
{
    HWND hWnd = CreateWindowExW( 0, L"STATIC", L"PFAviz++ offscreen", WS_POPUP, 0, 0, iWidth, iHeight,
                                 nullptr, nullptr, GetModuleHandleW( nullptr ), nullptr );
    if ( !hWnd ) return E_FAIL;
    m_bOffscreen = true;
    m_iOffscreenW = iWidth;
    m_iOffscreenH = iHeight;
    return Init( hWnd, false );
}

HRESULT Renderer::ResizeOffscreen( int iWidth, int iHeight )
{
    if ( iWidth == m_iOffscreenW && iHeight == m_iOffscreenH ) return S_OK;
    m_iOffscreenW = iWidth;
    m_iOffscreenH = iHeight;
    return ResetDevice();
}

IDirect3DSurface9 *Renderer::CopyBackBuffer()
{
    // One system-memory surface, kept while the size stays (the harness reads back every frame)
    if ( m_pReadback && ( m_iReadbackW != m_iBufferWidth || m_iReadbackH != m_iBufferHeight ) ) { m_pReadback->Release(); m_pReadback = nullptr; }
    if ( !m_pReadback &&
         SUCCEEDED( m_pDevice->CreateOffscreenPlainSurface( m_iBufferWidth, m_iBufferHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &m_pReadback, nullptr ) ) )
    {
        m_iReadbackW = m_iBufferWidth;
        m_iReadbackH = m_iBufferHeight;
    }
    IDirect3DSurface9 *pBack = nullptr;
    bool bOk = m_pReadback && SUCCEEDED( m_pDevice->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &pBack ) ) &&
               SUCCEEDED( m_pDevice->GetRenderTargetData( pBack, m_pReadback ) );
    if ( pBack ) pBack->Release();
    return bOk ? m_pReadback : nullptr;
}

bool Renderer::ReadBackRaw( void *pPixels )
{
    IDirect3DSurface9 *pSys = CopyBackBuffer();
    D3DLOCKED_RECT lr;
    if ( !pSys || FAILED( pSys->LockRect( &lr, nullptr, D3DLOCK_READONLY ) ) ) return false;
    size_t iRow = (size_t)m_iBufferWidth * 4;
    for ( int y = 0; y < m_iBufferHeight; y++ )
        memcpy( (BYTE *)pPixels + y * iRow, (const BYTE *)lr.pBits + (size_t)y * lr.Pitch, iRow );
    pSys->UnlockRect();
    return true;
}

bool Renderer::ReadBack( std::vector< DWORD > &vPixels )
{
    bool bOk = false;
    IDirect3DSurface9 *pSys = CopyBackBuffer();
    if ( pSys )
    {
        D3DLOCKED_RECT lr;
        if ( SUCCEEDED( pSys->LockRect( &lr, nullptr, D3DLOCK_READONLY ) ) )
        {
            vPixels.resize( (size_t)m_iBufferWidth * m_iBufferHeight );
            for ( int y = 0; y < m_iBufferHeight; y++ )
            {
                const DWORD *pRow = ( const DWORD * )( ( const BYTE * )lr.pBits + (size_t)y * lr.Pitch );
                for ( int x = 0; x < m_iBufferWidth; x++ )
                    vPixels[(size_t)y * m_iBufferWidth + x] = pRow[x] & 0x00FFFFFF;
            }
            pSys->UnlockRect();
            bOk = true;
        }
    }
    return bOk;
}

bool Renderer::UsingD3DXFonts() const { return m_pText && m_pText->IsD3DX(); }

void Renderer::RestoreStates()
{
    m_pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
    m_pDevice->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_INVSRCALPHA );
    m_pDevice->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_SRCALPHA );
    m_pDevice->SetRenderState( D3DRS_LIGHTING, FALSE );
    m_pDevice->SetFVF( SCREEN_VERTEX::FVF );
}

HRESULT Renderer::RestoreDeviceObjects()
{
    HRESULT hr;
    if ( FAILED( hr = m_pDevice->CreateVertexBuffer( VBVertices * sizeof( SCREEN_VERTEX ), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                     SCREEN_VERTEX::FVF, D3DPOOL_DEFAULT, &m_pVB, nullptr ) ) )
        return hr;
    if ( FAILED( hr = m_pDevice->CreateIndexBuffer( IBQuads * 6 * sizeof( WORD ), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_pIB, nullptr ) ) )
        return hr;
    void *pData = nullptr;
    if ( FAILED( hr = m_pIB->Lock( 0, 0, &pData, 0 ) ) )
        return hr;
    WORD *pIdx = (WORD *)pData;
    for ( UINT q = 0; q < IBQuads; q++, pIdx += 6 )
    {
        WORD w = (WORD)( q * 4 );
        pIdx[0] = w; pIdx[1] = w + 1; pIdx[2] = w + 2;
        pIdx[3] = w; pIdx[4] = w + 2; pIdx[5] = w + 3;
    }
    m_pIB->Unlock();
    m_uVBPos = 0;
    m_vVerts.clear();
    m_pQuadMap = nullptr;
    m_uMapUsed = 0;
    m_vInst.clear();
    CreateInstancing();
    m_pText->OnResetDevice();
    RestoreStates();
    return S_OK;
}

void Renderer::DestroyDeviceObjects()
{
    if ( m_pReadback ) { m_pReadback->Release(); m_pReadback = nullptr; }
    if ( m_pText ) m_pText->OnLostDevice();
    if ( m_pQuadMap ) { m_pVB->Unlock(); m_pQuadMap = nullptr; m_uMapUsed = 0; }
    if ( m_pVB ) { m_pVB->Release(); m_pVB = nullptr; }
    if ( m_pIB ) { m_pIB->Release(); m_pIB = nullptr; }
    if ( m_pTmplVB ) { m_pTmplVB->Release(); m_pTmplVB = nullptr; }
    if ( m_pTmplIB ) { m_pTmplIB->Release(); m_pTmplIB = nullptr; }
    if ( m_pInstVB ) { m_pInstVB->Release(); m_pInstVB = nullptr; }
    if ( m_pBkgTex ) { m_pBkgTex->Release(); m_pBkgTex = nullptr; }
    m_bIsDeviceValid = false;
}

HRESULT Renderer::SetLimitFPS( bool bLimitFPS )
{
    if ( bLimitFPS != m_bLimitFPS )
    {
        m_bLimitFPS = bLimitFPS;
        return ResetDevice();
    }
    return S_OK;
}

HRESULT Renderer::ResetDeviceIfNeeded()
{
    if ( !m_bIsDeviceValid )
    {
        HRESULT hr = m_pDevice->CheckDeviceState( m_hWnd );
        if ( hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED )
            return hr;
        return ResetDevice();
    }
    return S_OK;
}

HRESULT Renderer::ResetDevice()
{
    HRESULT hr;
    if ( m_bIsDeviceValid )
        DestroyDeviceObjects();

    FillPresentParams();
    if ( FAILED( hr = m_pDevice->ResetEx( &m_d3dPP, nullptr ) ) )
        return hr;
    if ( FAILED( hr = RestoreDeviceObjects() ) )
        return hr;

    m_iBufferWidth = m_d3dPP.BackBufferWidth;
    m_iBufferHeight = m_d3dPP.BackBufferHeight;
    m_bIsDeviceValid = true;
    return S_OK;
}

HRESULT Renderer::Clear( DWORD color )
{
    return m_pDevice->Clear( 0, nullptr, D3DCLEAR_TARGET, color, 1.0f, 0 );
}

HRESULT Renderer::BeginScene()
{
    return m_pDevice->BeginScene();
}

HRESULT Renderer::EndScene()
{
    FlushBuffer();
    return m_pDevice->EndScene();
}

HRESULT Renderer::Present()
{
    HRESULT hr = m_pDevice->PresentEx( nullptr, nullptr, nullptr, nullptr, 0 );
    if ( hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED )
        DestroyDeviceObjects();
    return hr;
}

HRESULT Renderer::BeginText()
{
    FlushBuffer();
    return m_pText->Begin();
}

HRESULT Renderer::DrawText( const WCHAR *sText, FontSize fsFont, LPRECT rcPos, DWORD dwFormat, DWORD dwColor, INT iChars )
{
    return m_pText->DrawText( fsFont, sText, iChars, rcPos, dwFormat, dwColor );
}

HRESULT Renderer::EndText()
{
    HRESULT hr = m_pText->End();
    RestoreStates();
    return hr;
}

HRESULT Renderer::DrawRect( float x, float y, float cx, float cy, DWORD color )
{
    return DrawRect( x, y, cx, cy, color, color, color, color );
}

// Vertex construction copied from the original D3D9Renderer::DrawRect.
void Renderer::RectVertices( float x, float y, float cx, float cy, DWORD c1, DWORD c2, DWORD c3, DWORD c4, SCREEN_VERTEX *pOut )
{
    x -= 0.5f;
    y -= 0.5f;
    pOut[0] = { x,      y,      0.5f, 1.0f, c1 };
    pOut[1] = { x + cx, y,      0.5f, 1.0f, c2 };
    pOut[2] = { x + cx, y + cy, 0.5f, 1.0f, c3 };
    pOut[3] = { x,      y,      0.5f, 1.0f, c1 };
    pOut[4] = { x + cx, y + cy, 0.5f, 1.0f, c3 };
    pOut[5] = { x,      y + cy, 0.5f, 1.0f, c4 };
}

HRESULT Renderer::DrawRect( float x, float y, float cx, float cy, DWORD c1, DWORD c2, DWORD c3, DWORD c4 )
{
    SCREEN_VERTEX vertices[6];
    RectVertices( x, y, cx, cy, c1, c2, c3, c4, vertices );
    Blit( vertices );
    return S_OK;
}

void Renderer::DrawVertices( const SCREEN_VERTEX *pVerts, size_t iCount )
{
    if ( !iCount ) return;
    if ( m_pCapture ) m_pCapture->insert( m_pCapture->end(), pVerts, pVerts + iCount );
    FlushQuads();
    if ( iCount < 4096 || !m_pVB )
    {
        m_vVerts.insert( m_vVerts.end(), pVerts, pVerts + iCount );
        if ( m_vVerts.size() >= VBVertices )
            FlushBuffer();
        return;
    }
    // Large batches go straight to the vertex buffer, after what is queued (same order)
    FlushBuffer();
    Submit( pVerts, iCount );
}

HRESULT Renderer::DrawSkew( float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, DWORD color )
{
    return DrawSkew( x1, y1, x2, y2, x3, y3, x4, y4, color, color, color, color );
}

HRESULT Renderer::DrawSkew( float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
                            DWORD c1, DWORD c2, DWORD c3, DWORD c4 )
{
    SCREEN_VERTEX vertices[6] =
    {
        { x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, c1 },
        { x2 - 0.5f, y2 - 0.5f, 0.5f, 1.0f, c2 },
        { x3 - 0.5f, y3 - 0.5f, 0.5f, 1.0f, c3 },
        { x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, c1 },
        { x3 - 0.5f, y3 - 0.5f, 0.5f, 1.0f, c3 },
        { x4 - 0.5f, y4 - 0.5f, 0.5f, 1.0f, c4 }
    };
    Blit( vertices );
    return S_OK;
}

void Renderer::Blit( const SCREEN_VERTEX *pVerts )
{
    FlushQuads();
    m_vVerts.insert( m_vVerts.end(), pVerts, pVerts + 6 );
    if ( m_pCapture ) m_pCapture->insert( m_pCapture->end(), pVerts, pVerts + 6 );
    if ( m_vVerts.size() >= VBVertices )
        FlushBuffer();
}

// Draw order is preserved exactly: everything is a triangle list submitted in call order.
void Renderer::DrawQuads( const SCREEN_VERTEX *pVerts, size_t iQuads )
{
    if ( !iQuads ) return;
    if ( m_pCapture || !m_pVB || !m_pIB )
    {
        // Captured as the six vertices; without device buffers, queued as triangles
        for ( size_t q = 0; q < iQuads; q++ )
        {
            const SCREEN_VERTEX *p = pVerts + q * 4;
            const SCREEN_VERTEX a6[6] = { p[0], p[1], p[2], p[0], p[2], p[3] };
            if ( m_pCapture ) m_pCapture->insert( m_pCapture->end(), a6, a6 + 6 );
            if ( !m_pVB || !m_pIB ) m_vVerts.insert( m_vVerts.end(), a6, a6 + 6 );
        }
        if ( !m_pVB || !m_pIB ) return;
    }
    if ( !m_vVerts.empty() ) FlushBuffer();
    FlushInstances();
    if ( iQuads < 1024 )
    {
        UINT uCount = (UINT)iQuads * 4;
        if ( m_pQuadMap && m_uMapUsed + uCount > m_uMapCap ) FlushQuads();
        if ( !m_pQuadMap )
        {
            // The rest of the buffer, up to what one draw can index (discarded only when this does not fit)
            DWORD dwLock = D3DLOCK_NOOVERWRITE;
            if ( m_uVBPos + uCount > VBVertices ) { m_uVBPos = 0; dwLock = D3DLOCK_DISCARD; }
            m_uMapCap = std::min( VBVertices - m_uVBPos, IBQuads * 4 );
            void *pData = nullptr;
            if ( FAILED( m_pVB->Lock( m_uVBPos * sizeof( SCREEN_VERTEX ), m_uMapCap * sizeof( SCREEN_VERTEX ), &pData, dwLock ) ) ) return;
            m_pQuadMap = (SCREEN_VERTEX *)pData;
            m_uMapUsed = 0;
        }
        memcpy( m_pQuadMap + m_uMapUsed, pVerts, uCount * sizeof( SCREEN_VERTEX ) );
        m_uMapUsed += uCount;
        return;
    }
    // Large batches go straight to the vertex buffer, after what is queued (same order)
    FlushQuads();
    SubmitQuads( pVerts, iQuads );
}

void Renderer::FlushQuads()
{
    FlushInstances();
    if ( !m_pQuadMap ) return;
    m_pVB->Unlock();
    m_pQuadMap = nullptr;
    if ( !m_uMapUsed ) return;
    m_pDevice->SetStreamSource( 0, m_pVB, 0, sizeof( SCREEN_VERTEX ) );
    m_pDevice->SetIndices( m_pIB );
    m_pDevice->SetFVF( SCREEN_VERTEX::FVF );
    m_pDevice->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, (INT)m_uVBPos, 0, m_uMapUsed, 0, m_uMapUsed / 2 );
    m_uVBPos += m_uMapUsed;
    m_uMapUsed = 0;
}

void Renderer::SubmitQuads( const SCREEN_VERTEX *pVerts, size_t iQuads )
{
    m_pDevice->SetStreamSource( 0, m_pVB, 0, sizeof( SCREEN_VERTEX ) );
    m_pDevice->SetIndices( m_pIB );
    m_pDevice->SetFVF( SCREEN_VERTEX::FVF );

    size_t iDone = 0;
    while ( iDone < iQuads )
    {
        UINT uQuads = (UINT)std::min< size_t >( iQuads - iDone, IBQuads ), uCount = uQuads * 4;
        DWORD dwLock = D3DLOCK_NOOVERWRITE;
        if ( m_uVBPos + uCount > VBVertices ) { m_uVBPos = 0; dwLock = D3DLOCK_DISCARD; }
        void *pData = nullptr;
        if ( FAILED( m_pVB->Lock( m_uVBPos * sizeof( SCREEN_VERTEX ), uCount * sizeof( SCREEN_VERTEX ), &pData, dwLock ) ) )
            break;
        memcpy( pData, pVerts + iDone * 4, uCount * sizeof( SCREEN_VERTEX ) );
        m_pVB->Unlock();
        m_pDevice->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, (INT)m_uVBPos, 0, uCount, 0, uQuads * 2 );
        m_uVBPos += uCount;
        iDone += uQuads;
    }
}

void Renderer::FlushBuffer()
{
    FlushQuads();
    if ( m_vVerts.empty() || !m_pVB ) { m_vVerts.clear(); return; }
    Submit( m_vVerts.data(), m_vVerts.size() );
    m_vVerts.clear();
}

void Renderer::Submit( const SCREEN_VERTEX *pVerts, size_t iCount )
{
    m_pDevice->SetStreamSource( 0, m_pVB, 0, sizeof( SCREEN_VERTEX ) );
    m_pDevice->SetFVF( SCREEN_VERTEX::FVF );

    size_t iDone = 0;
    while ( iDone < iCount )
    {
        UINT uCount = (UINT)std::min< size_t >( iCount - iDone, VBVertices );
        DWORD dwLock = D3DLOCK_NOOVERWRITE;
        if ( m_uVBPos + uCount > VBVertices ) { m_uVBPos = 0; dwLock = D3DLOCK_DISCARD; }
        void *pData = nullptr;
        if ( FAILED( m_pVB->Lock( m_uVBPos * sizeof( SCREEN_VERTEX ), uCount * sizeof( SCREEN_VERTEX ), &pData, dwLock ) ) )
            break;
        memcpy( pData, pVerts + iDone, uCount * sizeof( SCREEN_VERTEX ) );
        m_pVB->Unlock();
        m_pDevice->DrawPrimitive( D3DPT_TRIANGLELIST, m_uVBPos, uCount / 3 );
        m_uVBPos += uCount;
        iDone += uCount;
    }
}

//-----------------------------------------------------------------------------
// Instanced notes
//-----------------------------------------------------------------------------

void Renderer::CreateInstancing()
{
    // Shaders and declaration once per device; vs_3_0 and ps_3_0 (instancing needs vs_3_0)
    if ( !m_pInstVS )
    {
        D3DCAPS9 caps;
        if ( FAILED( m_pDevice->GetDeviceCaps( &caps ) ) || caps.VertexShaderVersion < D3DVS_VERSION( 3, 0 ) || caps.PixelShaderVersion < D3DPS_VERSION( 3, 0 ) )
            return;
        static HMODULE hCompiler = LoadLibraryW( L"d3dcompiler_47.dll" );
        pD3DCompile pCompile = hCompiler ? (pD3DCompile)(void *)GetProcAddress( hCompiler, "D3DCompile" ) : nullptr;
        if ( !pCompile ) return;
        ID3DBlob *pVS = nullptr, *pPS = nullptr;
        const UINT uFlags = D3DCOMPILE_IEEE_STRICTNESS;
        bool bOk = SUCCEEDED( pCompile( InstanceVS, strlen( InstanceVS ), "notes.vs", nullptr, nullptr, "main", "vs_3_0", uFlags, 0, &pVS, nullptr ) ) &&
                   SUCCEEDED( pCompile( InstancePS, strlen( InstancePS ), "notes.ps", nullptr, nullptr, "main", "ps_3_0", uFlags, 0, &pPS, nullptr ) );
        static const D3DVERTEXELEMENT9 aDecl[] =
        {
            { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
            { 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
            { 1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
            { 1, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
            { 1, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
            { 1, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 },
            { 1, 28, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6 },
            D3DDECL_END()
        };
        bOk = bOk && SUCCEEDED( m_pDevice->CreateVertexShader( (const DWORD *)pVS->GetBufferPointer(), &m_pInstVS ) ) &&
              SUCCEEDED( m_pDevice->CreatePixelShader( (const DWORD *)pPS->GetBufferPointer(), &m_pInstPS ) ) &&
              SUCCEEDED( m_pDevice->CreateVertexDeclaration( aDecl, &m_pInstDecl ) );
        if ( pVS ) pVS->Release();
        if ( pPS ) pPS->Release();
        if ( !bOk )
        {
            if ( m_pInstVS ) { m_pInstVS->Release(); m_pInstVS = nullptr; }
            if ( m_pInstPS ) { m_pInstPS->Release(); m_pInstPS = nullptr; }
            if ( m_pInstDecl ) { m_pInstDecl->Release(); m_pInstDecl = nullptr; }
            return;
        }
    }

    // The template: RectVertices' quad corners (top left, top right, bottom right, bottom left) of
    // the outer rect (very dark), then of the inner rect (primary on the left, dark on the right)
    static const TemplateVertex aTmpl[8] =
    {
        { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } },
        { { 0, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } },
        { { 0, 1, 0, 0 }, { 0, 1, 0, 0 }, { 1, 0, 0, 0 } },
        { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 1, 0, 0, 0 } },
        { { 0, 0, 1, 0 }, { 0, 0, 1, 0 }, { 0, 1, 0, 0 } },
        { { 0, 0, 0, 1 }, { 0, 0, 1, 0 }, { 0, 0, 1, 0 } },
        { { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0, 1, 0 } },
        { { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0, 1, 0, 0 } },
    };
    static const WORD aIdx[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    void *pData = nullptr;
    bool bOk = SUCCEEDED( m_pDevice->CreateVertexBuffer( sizeof( aTmpl ), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m_pTmplVB, nullptr ) ) &&
               SUCCEEDED( m_pTmplVB->Lock( 0, 0, &pData, 0 ) );
    if ( bOk ) { memcpy( pData, aTmpl, sizeof( aTmpl ) ); m_pTmplVB->Unlock(); }
    bOk = bOk && SUCCEEDED( m_pDevice->CreateIndexBuffer( sizeof( aIdx ), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_pTmplIB, nullptr ) ) &&
          SUCCEEDED( m_pTmplIB->Lock( 0, 0, &pData, 0 ) );
    if ( bOk ) { memcpy( pData, aIdx, sizeof( aIdx ) ); m_pTmplIB->Unlock(); }
    bOk = bOk && SUCCEEDED( m_pDevice->CreateVertexBuffer( VBInstances * sizeof( NoteInstance ), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                                          D3DPOOL_DEFAULT, &m_pInstVB, nullptr ) );
    m_uInstPos = 0;
    if ( !bOk )
    {
        // Without its buffers the path is off (InstancingOn checks the shaders)
        if ( m_pInstVS ) { m_pInstVS->Release(); m_pInstVS = nullptr; }
        if ( m_pInstPS ) { m_pInstPS->Release(); m_pInstPS = nullptr; }
    }
}

void Renderer::SetNoteColumns( const float ( *aCols )[4] )
{
    FlushInstances();
    memcpy( m_aCols, aCols, sizeof( m_aCols ) );
}

void Renderer::DrawNoteInstances( const NoteInstance *pInst, size_t iCount )
{
    if ( !iCount ) return;
    if ( !m_vVerts.empty() || m_pQuadMap ) FlushBuffer();
    if ( iCount < 1024 )
    {
        m_vInst.insert( m_vInst.end(), pInst, pInst + iCount );
        if ( m_vInst.size() >= VBInstances ) FlushInstances();
        return;
    }
    FlushInstances();
    SubmitInstances( pInst, iCount );
}

void Renderer::FlushInstances()
{
    if ( m_vInst.empty() ) return;
    SubmitInstances( m_vInst.data(), m_vInst.size() );
    m_vInst.clear();
}

void Renderer::SubmitInstances( const NoteInstance *pInst, size_t iCount )
{
    if ( !m_pInstVB || !m_pInstVS ) return;
    // Columns to clip space: the viewport maps X to (X + 1) * W / 2, so X = x * 2 / W - 1 (rounded
    // once, from double). Coverage then matches the quads; the inner gradient's colour can still
    // differ by one level along a column now and then (the shader path interpolates it differently)
    float aConst[129][4];
    const double dW = m_iBufferWidth, dH = m_iBufferHeight;
    for ( int k = 0; k < 128; k++ )
        for ( int c = 0; c < 4; c++ ) aConst[k][c] = (float)( m_aCols[k][c] * 2.0 / dW - 1.0 );
    aConst[128][0] = (float)( -2.0 / dH );
    aConst[128][1] = 1.0f;
    aConst[128][2] = aConst[128][3] = 0.0f;
    m_pDevice->SetVertexShaderConstantF( 0, &aConst[0][0], 129 );
    m_pDevice->SetVertexDeclaration( m_pInstDecl );
    m_pDevice->SetVertexShader( m_pInstVS );
    m_pDevice->SetPixelShader( m_pInstPS );
    m_pDevice->SetStreamSource( 0, m_pTmplVB, 0, sizeof( TemplateVertex ) );
    m_pDevice->SetIndices( m_pTmplIB );

    size_t iDone = 0;
    while ( iDone < iCount )
    {
        UINT uCount = (UINT)std::min< size_t >( iCount - iDone, VBInstances );
        DWORD dwLock = D3DLOCK_NOOVERWRITE;
        if ( m_uInstPos + uCount > VBInstances ) { m_uInstPos = 0; dwLock = D3DLOCK_DISCARD; }
        void *pData = nullptr;
        if ( FAILED( m_pInstVB->Lock( m_uInstPos * sizeof( NoteInstance ), uCount * sizeof( NoteInstance ), &pData, dwLock ) ) )
            break;
        memcpy( pData, pInst + iDone, uCount * sizeof( NoteInstance ) );
        m_pInstVB->Unlock();
        m_pDevice->SetStreamSourceFreq( 0, D3DSTREAMSOURCE_INDEXEDDATA | uCount );
        m_pDevice->SetStreamSource( 1, m_pInstVB, m_uInstPos * sizeof( NoteInstance ), sizeof( NoteInstance ) );
        m_pDevice->SetStreamSourceFreq( 1, D3DSTREAMSOURCE_INSTANCEDATA | 1u );
        m_pDevice->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 8, 0, 4 );
        m_uInstPos += uCount;
        iDone += uCount;
    }

    m_pDevice->SetStreamSourceFreq( 0, 1 );
    m_pDevice->SetStreamSourceFreq( 1, 1 );
    m_pDevice->SetStreamSource( 1, nullptr, 0, 0 );
    m_pDevice->SetVertexShader( nullptr );
    m_pDevice->SetPixelShader( nullptr );
    m_pDevice->SetFVF( SCREEN_VERTEX::FVF );
}

//-----------------------------------------------------------------------------
// Background image
//-----------------------------------------------------------------------------

void Renderer::SetBackgroundImage( std::shared_ptr< const Image > pImage )
{
    if ( pImage == m_pBkgImage ) return;
    if ( m_pBkgTex ) { m_pBkgTex->Release(); m_pBkgTex = nullptr; }
    m_pBkgImage = std::move( pImage );
    m_bBkgFailed = false;
}

int Renderer::MaxTextureSide() const
{
    D3DCAPS9 caps;
    if ( !m_pDevice || FAILED( m_pDevice->GetDeviceCaps( &caps ) ) ) return 4096;
    return (int)std::max< DWORD >( 256, std::min( caps.MaxTextureWidth, caps.MaxTextureHeight ) );
}

// The image with its mipmaps (each level a 2 x 2 box average of the one above), so a large image
// drawn small is smooth. The alpha is stored inverted, for DrawBackgroundImage's alpha stage.
bool Renderer::CreateBackgroundTexture()
{
    const Image &img = *m_pBkgImage;
    IDirect3DTexture9 *pSys = nullptr;
    if ( FAILED( m_pDevice->CreateTexture( img.iWidth, img.iHeight, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &pSys, nullptr ) ) ) return false;
    std::vector< uint32_t > vLevel( img.vPixels );
    int w = img.iWidth, h = img.iHeight;
    bool bOk = true;
    for ( DWORD l = 0; l < pSys->GetLevelCount() && bOk; l++ )
    {
        D3DLOCKED_RECT lr;
        if ( FAILED( pSys->LockRect( l, &lr, nullptr, 0 ) ) ) { bOk = false; break; }
        for ( int y = 0; y < h; y++ )
        {
            uint32_t *pRow = (uint32_t *)( (BYTE *)lr.pBits + (size_t)y * lr.Pitch );
            for ( int x = 0; x < w; x++ ) pRow[x] = vLevel[(size_t)y * w + x] ^ 0xFF000000u;
        }
        pSys->UnlockRect( l );
        // The next level down
        int w2 = std::max( 1, w / 2 ), h2 = std::max( 1, h / 2 );
        std::vector< uint32_t > vNext( (size_t)w2 * h2 );
        for ( int y = 0; y < h2; y++ )
            for ( int x = 0; x < w2; x++ )
            {
                int x0 = std::min( 2 * x, w - 1 ), x1 = std::min( 2 * x + 1, w - 1 ), y0 = std::min( 2 * y, h - 1 ), y1 = std::min( 2 * y + 1, h - 1 );
                uint32_t a[4] = { vLevel[(size_t)y0 * w + x0], vLevel[(size_t)y0 * w + x1], vLevel[(size_t)y1 * w + x0], vLevel[(size_t)y1 * w + x1] };
                uint32_t v = 0;
                for ( int c = 0; c < 32; c += 8 )
                    v |= ( ( ( ( a[0] >> c ) & 0xFF ) + ( ( a[1] >> c ) & 0xFF ) + ( ( a[2] >> c ) & 0xFF ) + ( ( a[3] >> c ) & 0xFF ) + 2 ) / 4 ) << c;
                vNext[(size_t)y * w2 + x] = v;
            }
        vLevel.swap( vNext );
        w = w2;
        h = h2;
    }
    IDirect3DTexture9 *pTex = nullptr;
    bOk = bOk && SUCCEEDED( m_pDevice->CreateTexture( img.iWidth, img.iHeight, pSys->GetLevelCount(), 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pTex, nullptr ) ) &&
          SUCCEEDED( m_pDevice->UpdateTexture( pSys, pTex ) );
    pSys->Release();
    if ( !bOk ) { if ( pTex ) pTex->Release(); return false; }
    m_pBkgTex = pTex;
    return true;
}

void Renderer::DrawBackgroundImage( float x, float y, float cx, float cy )
{
    if ( !m_pBkgImage || m_bBkgFailed || cx <= 0.0f || cy <= 0.0f || m_fBkgOpacity <= 0.0f ) return;
    if ( !m_pBkgTex && !CreateBackgroundTexture() ) { m_bBkgFailed = true; return; }
    FlushBuffer();

    // Where it goes and which part of it: u, v in image widths and heights
    const float iw = (float)m_pBkgImage->iWidth, ih = (float)m_pBkgImage->iHeight;
    float x0 = x, y0 = y, x1 = x + cx, y1 = y + cy, u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    switch ( m_eBkgFit )
    {
        case FitFill:   // covers the rect, the overflow cut off evenly
        {
            float s = std::max( cx / iw, cy / ih ), uw = cx / ( iw * s ), vh = cy / ( ih * s );
            u0 = ( 1.0f - uw ) / 2.0f; u1 = u0 + uw;
            v0 = ( 1.0f - vh ) / 2.0f; v1 = v0 + vh;
            break;
        }
        case FitFit:    // all of it, centred (the background colour shows around it)
        {
            float s = std::min( cx / iw, cy / ih ), dw = iw * s, dh = ih * s;
            x0 = x + ( cx - dw ) / 2.0f; x1 = x0 + dw;
            y0 = y + ( cy - dh ) / 2.0f; y1 = y0 + dh;
            break;
        }
        case FitCenter: // its own size, centred, cut to the rect
        {
            float fx = std::floor( x + ( cx - iw ) / 2.0f ), fy = std::floor( y + ( cy - ih ) / 2.0f );
            x0 = std::max( x, fx ); x1 = std::min( x + cx, fx + iw );
            y0 = std::max( y, fy ); y1 = std::min( y + cy, fy + ih );
            if ( x1 <= x0 || y1 <= y0 ) return;
            u0 = ( x0 - fx ) / iw; u1 = ( x1 - fx ) / iw;
            v0 = ( y0 - fy ) / ih; v1 = ( y1 - fy ) / ih;
            break;
        }
        case FitTile:   // its own size, repeated from the top left
            u1 = cx / iw;
            v1 = cy / ih;
            break;
        default:        // FitStretch
            break;
    }

    struct TexVertex { float x, y, z, h; D3DCOLOR c; float u, v; };
    // The diffuse alpha is 1 - opacity: with the image's inverted alpha, D3DTOP_ADDSMOOTH makes
    // 1 - opacity * image alpha, which the inverted blend (Renderer.h) takes as the image's share
    D3DCOLOR c = ( (DWORD)std::clamp( (int)( ( 1.0f - m_fBkgOpacity ) * 255.0f + 0.5f ), 0, 255 ) << 24 ) | 0x00FFFFFF;
    const TexVertex v[4] = {
        { x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, c, u0, v0 },
        { x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, c, u1, v0 },
        { x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, c, u0, v1 },
        { x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, c, u1, v1 },
    };
    static const D3DTEXTURESTAGESTATETYPE aStage[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2 };
    static const D3DSAMPLERSTATETYPE aSamp[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER };
    DWORD aOldStage[5], aOldSamp[5];
    for ( int i = 0; i < 5; i++ ) { m_pDevice->GetTextureStageState( 0, aStage[i], &aOldStage[i] ); m_pDevice->GetSamplerState( 0, aSamp[i], &aOldSamp[i] ); }
    const DWORD dwAddress = m_eBkgFit == FitTile ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
    const DWORD aNewStage[5] = { D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTOP_ADDSMOOTH, D3DTA_TEXTURE, D3DTA_DIFFUSE };
    const DWORD aNewSamp[5] = { dwAddress, dwAddress, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR };
    for ( int i = 0; i < 5; i++ ) { m_pDevice->SetTextureStageState( 0, aStage[i], aNewStage[i] ); m_pDevice->SetSamplerState( 0, aSamp[i], aNewSamp[i] ); }
    m_pDevice->SetTexture( 0, m_pBkgTex );
    m_pDevice->SetFVF( D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 );
    m_pDevice->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, v, sizeof( TexVertex ) );
    m_pDevice->SetTexture( 0, nullptr );
    for ( int i = 0; i < 5; i++ ) { m_pDevice->SetTextureStageState( 0, aStage[i], aOldStage[i] ); m_pDevice->SetSamplerState( 0, aSamp[i], aOldSamp[i] ); }
    m_pDevice->SetFVF( SCREEN_VERTEX::FVF );
}
