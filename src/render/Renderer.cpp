#include "render/Renderer.h"

#include "render/Text.h"

#include <algorithm>
#include <cstring>

Renderer::Renderer() { m_vVerts.reserve( VBVertices ); }

Renderer::~Renderer()
{
    DestroyDeviceObjects();
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
    m_vQuads.clear();
    m_pText->OnResetDevice();
    RestoreStates();
    return S_OK;
}

void Renderer::DestroyDeviceObjects()
{
    if ( m_pReadback ) { m_pReadback->Release(); m_pReadback = nullptr; }
    if ( m_pText ) m_pText->OnLostDevice();
    if ( m_pVB ) { m_pVB->Release(); m_pVB = nullptr; }
    if ( m_pIB ) { m_pIB->Release(); m_pIB = nullptr; }
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
    if ( iQuads < 1024 )
    {
        m_vQuads.insert( m_vQuads.end(), pVerts, pVerts + iQuads * 4 );
        if ( m_vQuads.size() >= IBQuads * 4 ) FlushQuads();
        return;
    }
    // Large batches go straight to the vertex buffer, after what is queued (same order)
    FlushQuads();
    SubmitQuads( pVerts, iQuads );
}

void Renderer::FlushQuads()
{
    if ( m_vQuads.empty() ) return;
    if ( m_pVB && m_pIB ) SubmitQuads( m_vQuads.data(), m_vQuads.size() / 4 );
    m_vQuads.clear();
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
