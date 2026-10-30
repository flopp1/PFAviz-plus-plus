// Text drawing for the overlay. Uses the real ID3DXFont from d3dx9_43.dll when it is installed
// (pixel-identical to the original), otherwise a GDI-rasterised replacement.
#pragma once

#include <windows.h>
#include <d3d9.h>

class TextRenderer
{
public:
    virtual ~TextRenderer() {}
    virtual void OnLostDevice() = 0;
    virtual void OnResetDevice() = 0;
    virtual HRESULT Begin() = 0;
    virtual HRESULT DrawText( int iFont, const WCHAR *sText, INT iChars, LPRECT rcPos, DWORD dwFormat, DWORD dwColor ) = 0;
    virtual HRESULT End() = 0;
    virtual bool IsD3DX() const = 0;
    // The Stats font (index 5): Tahoma at this height (recreated when it changes)
    virtual void SetStatsHeight( int iHeight ) = 0;

    // Fonts of the original renderer: Tahoma 15, Tahoma 15 bold, Comic Sans MS 20 bold, Tahoma 25, Tahoma 35.
    struct FontDesc { int iHeight; int iWeight; const wchar_t *sFace; };
    static const FontDesc Fonts[5];

    static TextRenderer *Create( IDirect3DDevice9 *pDevice );
};
