// Compressed and archived MIDI files: zip, 7z, rar, tar, gz, bz2, xz, lzma, zst and lz4 (libarchive),
// nested up to MaxDepth layers deep (a .mid.xz inside a .7z, a .rar.xz, ...). Nothing is expanded to
// disk: each layer is decompressed straight from the one around it (libarchive over callbacks), and
// the loader reads the MIDI's chunks front to back as they come out (Loader's streamed count pass).
// A layer that needs to seek (a 7z inside another layer) is read again from its start to go back.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace Archive
{
    static const int MaxDepth = 8;      // archive and stream layers followed (guards against archive bombs)

    // The file is compressed or an archive, from its first bytes (a plain MIDI is not)
    bool IsCompressed( const std::wstring &sPath );

    // The chunks of the MIDI inside, front to back
    class Reader
    {
    public:
        virtual ~Reader() {}
        // The next chunk's type and length (its unread data skipped). False: the end, or an error (sError)
        virtual bool NextChunk( char aType[4], uint64_t &llLen ) = 0;
        // Up to n bytes of the current chunk's data; 0: none left (or the file ended early, or an error)
        virtual size_t ReadData( uint8_t *p, size_t n ) = 0;
        // Progress, in bytes of the outermost file
        virtual uint64_t Done() const = 0;
        virtual uint64_t Total() const = 0;
        virtual std::wstring Summary() const = 0;   // what was unpacked, for the load statistics
        std::wstring sError;
    };

    // Opens the MIDI inside sPath (of several files in an archive: the first named .mid/.midi, else the
    // largest). Null: sError says why.
    std::unique_ptr< Reader > Open( const std::wstring &sPath, const std::atomic< bool > *pCancel, std::wstring &sError );
}
