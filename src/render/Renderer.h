// Direct3D 9Ex renderer. Keeps the original D3D9Renderer's exact rasterisation path:
// pre-transformed XYZRHW vertices with the -0.5 px offset, the same two-triangle vertex order,
// fixed-function Gouraud colour, and the inverted blend SRC=INVSRCALPHA / DEST=SRCALPHA.
// Only the batching differs (bigger batches, same draw order).
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

class TextRenderer;

class Renderer
{
public:
    enum FontSize { Small, SmallBold, SmallComic, Medium, Large, FontCount };

    struct SCREEN_VERTEX
    {
        float x, y, z, h;
        D3DCOLOR color;
        static const DWORD FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    };

    Renderer();
    ~Renderer();

    HRESULT Init( HWND hWnd, bool bLimitFPS );
    // Test harness: render into a hidden window's back buffer of a fixed size, then read it back
    HRESULT InitOffscreen( int iWidth, int iHeight );
    HRESULT ResizeOffscreen( int iWidth, int iHeight );
    bool ReadBack( std::vector< DWORD > &vPixels );           // X8R8G8B8, row-major, no padding
    bool ReadBackRaw( void *pPixels );                          // the same with the X byte as it is (width x height x 4 bytes)
    void SetCapture( std::vector< SCREEN_VERTEX > *pCapture ) { m_pCapture = pCapture; }
    HRESULT ResetDeviceIfNeeded();
    HRESULT ResetDevice();
    HRESULT Clear( DWORD color );
    HRESULT BeginScene();
    HRESULT EndScene();
    HRESULT Present();

    HRESULT BeginText();
    HRESULT DrawText( const WCHAR *sText, FontSize fsFont, LPRECT rcPos, DWORD dwFormat, DWORD dwColor, INT iChars = -1 );
    HRESULT EndText();

    HRESULT DrawRect( float x, float y, float cx, float cy, DWORD color );
    HRESULT DrawRect( float x, float y, float cx, float cy, DWORD c1, DWORD c2, DWORD c3, DWORD c4 );
    // The six vertices DrawRect submits (pure function, so they can be built on other threads)
    static void RectVertices( float x, float y, float cx, float cy, DWORD c1, DWORD c2, DWORD c3, DWORD c4, SCREEN_VERTEX *pOut );
    // Submits ready-made triangles (a multiple of 3 vertices) in order, like that many DrawRect calls
    void DrawVertices( const SCREEN_VERTEX *pVerts, size_t iCount );
    // The same for rects given as quads: the first, second, third and sixth of the six vertices
    // RectVertices makes (the fourth and fifth repeat the first and third). Drawn indexed, as the
    // same two triangles with their vertices in the same order, from two thirds of the bytes.
    void DrawQuads( const SCREEN_VERTEX *pVerts, size_t iQuads );
    HRESULT DrawSkew( float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, DWORD color );
    HRESULT DrawSkew( float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
                      DWORD c1, DWORD c2, DWORD c3, DWORD c4 );

    bool GetLimitFPS() const { return m_bLimitFPS; }
    HRESULT SetLimitFPS( bool bLimitFPS );
    int GetBufferWidth() const { return m_iBufferWidth; }
    int GetBufferHeight() const { return m_iBufferHeight; }
    bool UsingD3DXFonts() const;

    IDirect3DDevice9Ex *GetDevice() const { return m_pDevice; }
    void FlushBuffer();
    void RestoreStates();

private:
    void FillPresentParams();
    IDirect3DSurface9 *CopyBackBuffer();                        // into m_pReadback; nullptr on failure
    HRESULT RestoreDeviceObjects();
    void DestroyDeviceObjects();
    void Blit( const SCREEN_VERTEX *pVerts );
    void Submit( const SCREEN_VERTEX *pVerts, size_t iCount );    // draws now, through the dynamic vertex buffer
    void SubmitQuads( const SCREEN_VERTEX *pVerts, size_t iQuads );
    void FlushQuads();

    static const UINT VBVertices = 6 * 16384;
    static const UINT IBQuads = 16384;              // 16-bit indices: 65536 vertices per draw

    HWND m_hWnd = nullptr;
    IDirect3D9Ex *m_pD3D = nullptr;
    IDirect3DDevice9Ex *m_pDevice = nullptr;
    IDirect3DVertexBuffer9 *m_pVB = nullptr;
    IDirect3DIndexBuffer9 *m_pIB = nullptr;        // 0 1 2 0 2 3, then the same + 4 for each quad
    IDirect3DSurface9 *m_pReadback = nullptr;      // ReadBack's system-memory copy
    int m_iReadbackW = 0, m_iReadbackH = 0;
    UINT m_uVBPos = 0;
    D3DPRESENT_PARAMETERS m_d3dPP = {};
    bool m_bFlipEx = true;
    bool m_bIsDeviceValid = false;
    bool m_bLimitFPS = true;
    int m_iBufferWidth = 0, m_iBufferHeight = 0;
    std::vector< SCREEN_VERTEX > m_vVerts;
    std::vector< SCREEN_VERTEX > m_vQuads;          // queued quads (at most one of the two queues holds anything)
    TextRenderer *m_pText = nullptr;
    bool m_bOffscreen = false;
    int m_iOffscreenW = 0, m_iOffscreenH = 0;
    std::vector< SCREEN_VERTEX > *m_pCapture = nullptr;
};
