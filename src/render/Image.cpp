#include "render/Image.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>

namespace
{
    template < class T > struct ComPtr
    {
        T *p = nullptr;
        ~ComPtr() { if ( p ) p->Release(); }
        T **operator&() { return &p; }
        T *operator->() const { return p; }
    };

    std::wstring HrText( const wchar_t *sWhat, HRESULT hr )
    {
        wchar_t buf[160];
        swprintf( buf, 160, L"%ls (error 0x%08lX).", sWhat, (unsigned long)hr );
        return buf;
    }
}

bool LoadImageFile( const std::wstring &sFile, int iMaxSide, Image &img, std::wstring &sError )
{
    // COM for WIC: initialized once per thread that loads (S_FALSE: it already was)
    static thread_local HRESULT s_hrCom = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    if ( FAILED( s_hrCom ) && s_hrCom != RPC_E_CHANGED_MODE ) { sError = HrText( L"COM could not start", s_hrCom ); return false; }

    ComPtr< IWICImagingFactory > pFactory;
    HRESULT hr = CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &pFactory.p ) );
    if ( FAILED( hr ) ) { sError = HrText( L"The Windows Imaging Component is not available", hr ); return false; }
    ComPtr< IWICBitmapDecoder > pDecoder;
    hr = pFactory->CreateDecoderFromFilename( sFile.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder );
    if ( FAILED( hr ) )
    {
        sError = hr == WINCODEC_ERR_COMPONENTNOTFOUND ? L"The file is not an image format Windows can read."
                                                      : HrText( L"The image could not be opened", hr );
        return false;
    }
    ComPtr< IWICBitmapFrameDecode > pFrame;
    if ( FAILED( hr = pDecoder->GetFrame( 0, &pFrame ) ) ) { sError = HrText( L"The image could not be read", hr ); return false; }
    UINT w = 0, h = 0;
    pFrame->GetSize( &w, &h );
    if ( !w || !h ) { sError = L"The image is empty."; return false; }

    // Scaled down (Fant: an area average) to fit the largest texture side, keeping its shape
    IWICBitmapSource *pSource = pFrame.p;
    ComPtr< IWICBitmapScaler > pScaler;
    if ( (int)std::max( w, h ) > iMaxSide )
    {
        double d = (double)iMaxSide / std::max( w, h );
        UINT w2 = std::max( 1u, (UINT)( w * d + 0.5 ) ), h2 = std::max( 1u, (UINT)( h * d + 0.5 ) );
        if ( SUCCEEDED( pFactory->CreateBitmapScaler( &pScaler ) ) && SUCCEEDED( pScaler->Initialize( pSource, w2, h2, WICBitmapInterpolationModeFant ) ) )
        {
            pSource = pScaler.p;
            w = w2;
            h = h2;
        }
    }
    ComPtr< IWICFormatConverter > pConvert;
    if ( FAILED( hr = pFactory->CreateFormatConverter( &pConvert ) ) ||
         FAILED( hr = pConvert->Initialize( pSource, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom ) ) )
    {
        sError = HrText( L"The image's pixels could not be converted", hr );
        return false;
    }
    std::vector< uint32_t > v( (size_t)w * h );
    if ( FAILED( hr = pConvert->CopyPixels( nullptr, w * 4, (UINT)( v.size() * 4 ), (BYTE *)v.data() ) ) )
    {
        sError = HrText( L"The image could not be decoded", hr );
        return false;
    }
    img.iWidth = (int)w;
    img.iHeight = (int)h;
    img.vPixels.swap( v );
    img.sFile = sFile;
    return true;
}
