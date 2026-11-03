// Compressed and archived MIDI files: zip, 7z, rar, tar, gz, bz2, xz, lzma, zst and lz4 (libarchive),
// nested up to MaxDepth layers deep (a .mid.xz inside a .7z, a .rar.xz, ...). Nothing is expanded to
// disk: each layer is decompressed straight from the one around it (libarchive over callbacks), and
// the loader reads the MIDI's chunks front to back as they come out (Loader's streamed count pass).
// A layer that needs to seek (a 7z inside another layer) is read again from its start to go back.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

    // A MIDI file in an archive, and which of several to read: its index, or -1 to cancel
    struct Entry { std::wstring sName; int64_t llSize; };     // llSize: -1 when the archive does not say
    typedef std::function< int( const std::vector< Entry > & ) > Chooser;

    // Opens the MIDI inside sPath. In an archive only files named .mid or .midi (also compressed, as
    // .mid.xz) count, and none is an error; of several, fnChoose picks (none given: the first). A
    // compressed archive (a .tar.xz) cannot be listed without decompressing all of it, so there the
    // first is taken. Null: sError says why.
    std::unique_ptr< Reader > Open( const std::wstring &sPath, const std::atomic< bool > *pCancel, std::wstring &sError,
                                    const Chooser &fnChoose = nullptr );
}
