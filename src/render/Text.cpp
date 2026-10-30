#include "render/Text.h"

#include <d3dx9core.h>
#include <algorithm>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

const TextRenderer::FontDesc TextRenderer::Fonts[5] =
{
    { 15, FW_NORMAL, L"Tahoma" },
    { 15, FW_BOLD, L"Tahoma" },
    { 20, FW_BOLD, L"Comic Sans MS" },
    { 25, FW_NORMAL, L"Tahoma" },
    { 35, FW_NORMAL, L"Tahoma" },
};

//-----------------------------------------------------------------------------
// D3DX9 path (d3dx9_43.dll loaded at runtime; no link-time dependency)
//-----------------------------------------------------------------------------

namespace
{
    typedef HRESULT ( WINAPI *PFN_D3DXCreateFontW )( LPDIRECT3DDEVICE9, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCWSTR, LPD3DXFONT * );
    typedef HRESULT ( WINAPI *PFN_D3DXCreateSprite )( LPDIRECT3DDEVICE9, LPD3DXSPRITE * );

    class D3DXText : public TextRenderer
    {
    public:
        ~D3DXText()
        {
            for ( auto f : m_pFonts ) if ( f ) f->Release();
            if ( m_pStats ) m_pStats->Release();
            if ( m_pSprite ) m_pSprite->Release();
        }

        bool Init( IDirect3DDevice9 *pDevice )
        {
            HMODULE hMod = LoadLibraryExW( L"d3dx9_43.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 );
            if ( !hMod ) return false;
            auto pCreateFont = ( PFN_D3DXCreateFontW )GetProcAddress( hMod, "D3DXCreateFontW" );
            auto pCreateSprite = ( PFN_D3DXCreateSprite )GetProcAddress( hMod, "D3DXCreateSprite" );
            if ( !pCreateFont || !pCreateSprite ) return false;
            if ( FAILED( pCreateSprite( pDevice, &m_pSprite ) ) ) return false;
            m_pDevice = pDevice;
            m_pCreateFont = pCreateFont;
            for ( int i = 0; i < 5; i++ )
                if ( FAILED( pCreateFont( pDevice, Fonts[i].iHeight, 0, Fonts[i].iWeight, 1, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, PROOF_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                          Fonts[i].sFace, &m_pFonts[i] ) ) )
                    return false;
            return true;
        }

        void OnLostDevice() override
        {
            m_pSprite->OnLostDevice();
            for ( auto f : m_pFonts ) f->OnLostDevice();
            if ( m_pStats ) m_pStats->OnLostDevice();
        }
        void OnResetDevice() override
        {
            m_pSprite->OnResetDevice();
            for ( auto f : m_pFonts ) f->OnResetDevice();
            if ( m_pStats ) m_pStats->OnResetDevice();
        }
        HRESULT Begin() override { return m_pSprite->Begin( D3DXSPRITE_ALPHABLEND | D3DXSPRITE_SORT_TEXTURE ); }
        HRESULT DrawText( int iFont, const WCHAR *sText, INT iChars, LPRECT rcPos, DWORD dwFormat, DWORD dwColor ) override
        {
            LPD3DXFONT pFont = iFont == 5 ? m_pStats : m_pFonts[iFont];
            if ( !pFont ) return E_FAIL;
            return pFont->DrawTextW( m_pSprite, sText, iChars, rcPos, dwFormat, dwColor ) ? S_OK : E_FAIL;
        }
        HRESULT End() override { return m_pSprite->End(); }
        bool IsD3DX() const override { return true; }
        void SetStatsHeight( int iHeight ) override
        {
            if ( m_pStats && iHeight == m_iStatsHeight ) return;
            if ( m_pStats ) { m_pStats->Release(); m_pStats = nullptr; }
            m_iStatsHeight = iHeight;
            m_pCreateFont( m_pDevice, iHeight, 0, FW_NORMAL, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, PROOF_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Tahoma", &m_pStats );
        }

    private:
        LPD3DXSPRITE m_pSprite = nullptr;
        LPD3DXFONT m_pFonts[5] = {};
        LPD3DXFONT m_pStats = nullptr;
        int m_iStatsHeight = 0;
        IDirect3DDevice9 *m_pDevice = nullptr;
        PFN_D3DXCreateFontW m_pCreateFont = nullptr;
    };

    //-------------------------------------------------------------------------
    // GDI fallback: text is laid out and rasterised by GDI DrawText into a DIB,
    // converted to an alpha texture and drawn as a quad. Strings are cached, and the textures of
    // strings that leave the cache are reused (the stats lines change every frame: creating a
    // texture for each would cost a driver allocation per line per frame).
    //-------------------------------------------------------------------------

    class GdiText : public TextRenderer
    {
    public:
        ~GdiText()
        {
            ClearCache();
            for ( auto f : m_hFonts ) if ( f ) DeleteObject( f );
            if ( m_hStats ) DeleteObject( m_hStats );
            if ( m_hDC ) DeleteDC( m_hDC );
            if ( m_hDIB ) DeleteObject( m_hDIB );
        }

        bool Init( IDirect3DDevice9 *pDevice )
        {
            m_pDevice = pDevice;
            m_hDC = CreateCompatibleDC( nullptr );
            for ( int i = 0; i < 5; i++ )
                m_hFonts[i] = CreateFontW( Fonts[i].iHeight, 0, 0, 0, Fonts[i].iWeight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                           DEFAULT_PITCH | FF_DONTCARE, Fonts[i].sFace );
            return m_hDC != nullptr;
        }

        void OnLostDevice() override { ClearCache(); }
        void OnResetDevice() override {}
        HRESULT Begin() override { return S_OK; }
        HRESULT End() override { return S_OK; }
        bool IsD3DX() const override { return false; }
        void SetStatsHeight( int iHeight ) override
        {
            if ( m_hStats && iHeight == m_iStatsHeight ) return;
            if ( m_hStats ) { ClearCache(); DeleteObject( m_hStats ); }     // its strings were cached by index
            m_iStatsHeight = iHeight;
            m_hStats = CreateFontW( iHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma" );
        }

        HRESULT DrawText( int iFont, const WCHAR *sText, INT iChars, LPRECT rcPos, DWORD dwFormat, DWORD dwColor ) override
        {
            std::wstring sStr = iChars < 0 ? std::wstring( sText ) : std::wstring( sText, iChars );
            HFONT hFont = iFont == 5 ? m_hStats : m_hFonts[iFont];
            if ( !hFont ) return E_FAIL;
            SelectObject( m_hDC, hFont );
            if ( dwFormat & DT_CALCRECT )
            {
                ::DrawTextW( m_hDC, sStr.c_str(), (int)sStr.size(), rcPos, dwFormat );
                return S_OK;
            }

            int iRcW = rcPos->right - rcPos->left, iRcH = rcPos->bottom - rcPos->top;
            std::wstring sKey = std::to_wstring( iFont ) + L"|" + std::to_wstring( dwFormat ) + L"|" +
                                std::to_wstring( iRcW ) + L"x" + std::to_wstring( iRcH ) + L"|" + sStr;
            Entry *pEntry = Lookup( sKey );
            if ( !pEntry ) pEntry = Rasterise( sKey, sStr, iRcW, iRcH, dwFormat );
            if ( !pEntry || !pEntry->pTex ) return S_OK;

            struct V { float x, y, z, h; D3DCOLOR c; float u, v; };
            float x = (float)( rcPos->left + pEntry->iOffX ) - 0.5f, y = (float)( rcPos->top + pEntry->iOffY ) - 0.5f;
            float w = (float)pEntry->iW, h = (float)pEntry->iH;
            float u1 = w / pEntry->iTexW, v1 = h / pEntry->iTexH;   // the string is the texture's top-left part
            V v[4] = { { x, y, 0.5f, 1.0f, dwColor, 0, 0 }, { x + w, y, 0.5f, 1.0f, dwColor, u1, 0 },
                       { x, y + h, 0.5f, 1.0f, dwColor, 0, v1 }, { x + w, y + h, 0.5f, 1.0f, dwColor, u1, v1 } };

            m_pDevice->SetFVF( D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 );
            m_pDevice->SetTexture( 0, pEntry->pTex );
            m_pDevice->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
            m_pDevice->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE );
            m_pDevice->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_POINT );
            m_pDevice->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
            m_pDevice->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
            m_pDevice->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
            m_pDevice->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, v, sizeof( V ) );
            m_pDevice->SetTexture( 0, nullptr );
            m_pDevice->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_INVSRCALPHA );
            m_pDevice->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_SRCALPHA );
            return S_OK;
        }

    private:
        struct Entry
        {
            IDirect3DTexture9 *pTex = nullptr;
            int iTexW = 0, iTexH = 0;       // the texture's size (iW x iH rounded up to TexStep)
            int iOffX = 0, iOffY = 0, iW = 0, iH = 0;
            std::list< std::wstring >::iterator itLru;
        };
        struct FreeTex { IDirect3DTexture9 *pTex; int iW, iH; };
        static const int TexStep = 32;
        static const size_t MaxFree = 64;

        // A texture of at least iW x iH: a free one of that size class, or a new one
        IDirect3DTexture9 *TakeTexture( int iW, int iH, int &iTexW, int &iTexH )
        {
            iTexW = ( iW + TexStep - 1 ) / TexStep * TexStep;
            iTexH = ( iH + TexStep - 1 ) / TexStep * TexStep;
            for ( size_t i = 0; i < m_vFree.size(); i++ )
                if ( m_vFree[i].iW == iTexW && m_vFree[i].iH == iTexH )
                {
                    IDirect3DTexture9 *p = m_vFree[i].pTex;
                    m_vFree[i] = m_vFree.back();
                    m_vFree.pop_back();
                    return p;
                }
            IDirect3DTexture9 *p = nullptr;
            if ( FAILED( m_pDevice->CreateTexture( iTexW, iTexH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &p, nullptr ) ) ) return nullptr;
            return p;
        }
        void GiveTexture( IDirect3DTexture9 *p, int iW, int iH )
        {
            if ( !p ) return;
            if ( m_vFree.size() >= MaxFree ) { m_vFree.front().pTex->Release(); m_vFree.erase( m_vFree.begin() ); }
            m_vFree.push_back( { p, iW, iH } );
        }

        Entry *Lookup( const std::wstring &sKey )
        {
            auto it = m_mCache.find( sKey );
            if ( it == m_mCache.end() ) return nullptr;
            m_lLru.splice( m_lLru.begin(), m_lLru, it->second.itLru );
            return &it->second;
        }

        Entry *Rasterise( const std::wstring &sKey, const std::wstring &sStr, int iRcW, int iRcH, DWORD dwFormat )
        {
            RECT rcCalc = { 0, 0, iRcW, iRcH };
            ::DrawTextW( m_hDC, sStr.c_str(), (int)sStr.size(), &rcCalc, dwFormat | DT_CALCRECT );
            int iMargin = std::max( rcCalc.right - rcCalc.left, rcCalc.bottom - rcCalc.top ) + 8;
            int iW = iRcW + 2 * iMargin, iH = iRcH + 2 * iMargin;
            EnsureDIB( iW, iH );
            memset( m_pBits, 0, (size_t)m_iDibW * m_iDibH * 4 );

            RECT rc = { iMargin, iMargin, iMargin + iRcW, iMargin + iRcH };
            SetTextColor( m_hDC, RGB( 255, 255, 255 ) );
            SetBkMode( m_hDC, TRANSPARENT );
            // Not clipped to the rect: a glyph's ink can pass its advance (Wine's substitute fonts cut the
            // last letter of right-aligned text otherwise); the margins hold it
            ::DrawTextW( m_hDC, sStr.c_str(), (int)sStr.size(), &rc, dwFormat | DT_NOCLIP );
            GdiFlush();

            int x0 = iW, y0 = iH, x1 = -1, y1 = -1;
            for ( int y = 0; y < iH; y++ )
                for ( int x = 0; x < iW; x++ )
                    if ( m_pBits[(size_t)y * m_iDibW + x] & 0xFFFFFF )
                    {
                        x0 = std::min( x0, x ); x1 = std::max( x1, x );
                        y0 = std::min( y0, y ); y1 = std::max( y1, y );
                    }

            if ( m_mCache.size() >= 256 )
            {
                auto it = m_mCache.find( m_lLru.back() );
                GiveTexture( it->second.pTex, it->second.iTexW, it->second.iTexH );
                m_mCache.erase( it );
                m_lLru.pop_back();
            }
            m_lLru.push_front( sKey );
            Entry &e = m_mCache[sKey];
            e.itLru = m_lLru.begin();
            if ( x1 < 0 ) return &e;

            e.iOffX = x0 - iMargin; e.iOffY = y0 - iMargin;
            e.iW = x1 - x0 + 1; e.iH = y1 - y0 + 1;
            if ( !( e.pTex = TakeTexture( e.iW, e.iH, e.iTexW, e.iTexH ) ) ) return &e;
            D3DLOCKED_RECT lr;
            if ( SUCCEEDED( e.pTex->LockRect( 0, &lr, nullptr, D3DLOCK_DISCARD ) ) )
            {
                for ( int y = 0; y < e.iH; y++ )
                {
                    DWORD *pDst = ( DWORD * )( ( BYTE * )lr.pBits + y * lr.Pitch );
                    const DWORD *pSrc = m_pBits + (size_t)( y0 + y ) * m_iDibW + x0;
                    for ( int x = 0; x < e.iW; x++ )
                    {
                        DWORD c = pSrc[x];
                        DWORD a = std::max( std::max( c & 0xFF, ( c >> 8 ) & 0xFF ), ( c >> 16 ) & 0xFF );
                        pDst[x] = ( a << 24 ) | 0x00FFFFFF;
                    }
                }
                e.pTex->UnlockRect( 0 );
            }
            return &e;
        }

        void EnsureDIB( int iW, int iH )
        {
            if ( m_hDIB && iW <= m_iDibW && iH <= m_iDibH ) return;
            if ( m_hDIB ) DeleteObject( m_hDIB );
            m_iDibW = std::max( iW, m_iDibW );
            m_iDibH = std::max( iH, m_iDibH );
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof( BITMAPINFOHEADER );
            bmi.bmiHeader.biWidth = m_iDibW;
            bmi.bmiHeader.biHeight = -m_iDibH;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            m_hDIB = CreateDIBSection( m_hDC, &bmi, DIB_RGB_COLORS, ( void ** )&m_pBits, nullptr, 0 );
            SelectObject( m_hDC, m_hDIB );
        }

        void ClearCache()
        {
            for ( auto &kv : m_mCache ) if ( kv.second.pTex ) kv.second.pTex->Release();
            m_mCache.clear();
            m_lLru.clear();
            for ( FreeTex &f : m_vFree ) f.pTex->Release();
            m_vFree.clear();
        }

        IDirect3DDevice9 *m_pDevice = nullptr;
        HDC m_hDC = nullptr;
        HBITMAP m_hDIB = nullptr;
        DWORD *m_pBits = nullptr;
        int m_iDibW = 0, m_iDibH = 0;
        HFONT m_hFonts[5] = {};
        HFONT m_hStats = nullptr;
        int m_iStatsHeight = 0;
        std::unordered_map< std::wstring, Entry > m_mCache;
        std::list< std::wstring > m_lLru;
        std::vector< FreeTex > m_vFree;     // textures of strings that left the cache
    };
}

TextRenderer *TextRenderer::Create( IDirect3DDevice9 *pDevice )
{
    // PFAVIZ_GDITEXT=1 (developer switch): the GDI fallback even where D3DX is installed
    bool bWine = GetProcAddress( GetModuleHandleW( L"ntdll.dll" ), "wine_get_version" ) != nullptr;
    if ( !getenv( "PFAVIZ_GDITEXT" ) && !bWine )
    {
        D3DXText *pD3DX = new D3DXText();
        if ( pD3DX->Init( pDevice ) ) return pD3DX;
        delete pD3DX;
    }
    GdiText *pGdi = new GdiText();
    pGdi->Init( pDevice );
    return pGdi;
}
