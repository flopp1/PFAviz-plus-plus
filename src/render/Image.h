// Images from files (the custom background), decoded by WIC: BMP, PNG, JPEG, GIF, TIFF, ICO,
// JPEG XR, and whatever other codecs Windows has installed (WebP, HEIF, ...).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Image
{
    int iWidth = 0, iHeight = 0;
    std::vector< uint32_t > vPixels;    // B8G8R8A8 (straight alpha), row-major, top-down
    std::wstring sFile;                 // where it came from
};

// Decodes the first frame of a file, scaled down to fit iMaxSide x iMaxSide when it is larger.
// Needs COM on the calling thread (it initializes it when it is not). False: sError says why.
bool LoadImageFile( const std::wstring &sFile, int iMaxSide, Image &img, std::wstring &sError );
