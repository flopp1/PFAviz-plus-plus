#include "song/Archive.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define LIBARCHIVE_STATIC
#include <archive.h>
#include <archive_entry.h>
#include <lzma.h>
#include <zlib.h>

#include "core/Util.h"

// (zlib declares it only for _LARGEFILE64_SOURCE; the library has it, its length 64-bit)
extern "C" uLong crc32_combine64( uLong crc1, uLong crc2, int64_t len2 );

namespace
{
    const uint8_t SevenZipMagic[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
    const uint8_t XzMagic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };

    bool IsMidiHead( const uint8_t *p, size_t n ) { return n >= 4 && memcmp( p, "MThd", 4 ) == 0; }
    uint64_t BE32( const uint8_t *p ) { return (uint64_t)p[0] << 24 | (uint64_t)p[1] << 16 | (uint64_t)p[2] << 8 | p[3]; }

    std::wstring Format( const wchar_t *sFormat, ... )
    {
        wchar_t buf[512];
        va_list va;
        va_start( va, sFormat );
        vswprintf( buf, 512, sFormat, va );
        va_end( va );
        return buf;
    }

    std::wstring Bytes( double d )
    {
        const wchar_t *aUnit[] = { L"bytes", L"KB", L"MB", L"GB", L"TB", L"PB", L"EB" };
        int i = 0;
        while ( d >= 1024.0 && i < 6 ) { d /= 1024.0; i++; }
        return i ? Format( L"%.1f %ls", d, aUnit[i] ) : Format( L"%.0f bytes", d );
    }

    bool Cancelled( const std::atomic< bool > *pCancel ) { return pCancel && pCancel->load( std::memory_order_relaxed ); }

    //-------------------------------------------------------------------------
    // Byte streams: the file, and each layer decompressed from the one around it
    //-------------------------------------------------------------------------

    class Stream
    {
    public:
        virtual ~Stream() {}
        // Up to n bytes; 0 at the end; -1 on an error (sError)
        int64_t Read( void *pv, size_t n )
        {
            int64_t r;
            if ( m_iPeek < m_vPeek.size() )
            {
                r = (int64_t)std::min( n, m_vPeek.size() - m_iPeek );
                memcpy( pv, m_vPeek.data() + m_iPeek, (size_t)r );
                m_iPeek += (size_t)r;
            }
            else if ( ( r = Raw( (uint8_t *)pv, n ) ) < 0 ) return -1;
            m_llPos += (uint64_t)r;
            return r;
        }
        // The next n bytes without taking them (got: fewer at the end); null on an error
        const uint8_t *Peek( size_t n, size_t &got )
        {
            m_vPeek.erase( m_vPeek.begin(), m_vPeek.begin() + m_iPeek );
            m_iPeek = 0;
            while ( m_vPeek.size() < n )
            {
                size_t iOld = m_vPeek.size();
                m_vPeek.resize( n );
                int64_t r = Raw( m_vPeek.data() + iOld, n - iOld );
                m_vPeek.resize( iOld + ( r > 0 ? (size_t)r : 0 ) );
                if ( r < 0 ) return nullptr;
                if ( !r ) break;
            }
            got = std::min( n, m_vPeek.size() );
            return m_vPeek.data();
        }
        // Forward n bytes, or to the end; false on an error
        bool Skip( uint64_t n )
        {
            if ( m_vSkip.empty() ) m_vSkip.resize( 1 << 20 );
            while ( n )
            {
                int64_t r = Read( m_vSkip.data(), (size_t)std::min< uint64_t >( n, m_vSkip.size() ) );
                if ( r < 0 ) return false;
                if ( !r ) return true;
                n -= (uint64_t)r;
            }
            return true;
        }
        // To byte x: forward by reading, back by starting over
        virtual bool Seek( uint64_t x )
        {
            if ( x < m_llPos && !Rewind() ) return false;
            return Skip( x - m_llPos );
        }
        bool Rewind()
        {
            m_vPeek.clear();
            m_iPeek = 0;
            m_llPos = 0;
            return DoRewind();
        }
        uint64_t Pos() const { return m_llPos; }
        // Bytes of the file read, for the progress (the file itself: its position)
        virtual uint64_t Progress() const { return m_llPos; }
        // Bytes in all (read through once when not known); -1 on an error
        virtual int64_t Size()
        {
            if ( m_llSize >= 0 ) return m_llSize;
            uint64_t llKeep = m_llPos;
            if ( !Skip( UINT64_MAX ) ) return -1;
            m_llSize = (int64_t)m_llPos;
            return Seek( llKeep ) ? m_llSize : -1;
        }
        std::wstring sError;

    protected:
        virtual int64_t Raw( uint8_t *p, size_t n ) = 0;
        virtual bool DoRewind() = 0;
        uint64_t m_llPos = 0;               // bytes handed out
        int64_t m_llSize = -1;

    private:
        std::vector< uint8_t > m_vPeek, m_vSkip;
        size_t m_iPeek = 0;
    };

    class FileStream : public Stream
    {
    public:
        explicit FileStream( const std::atomic< bool > *pCancel ) : m_pCancel( pCancel ) {}
        ~FileStream() { if ( m_h != INVALID_HANDLE_VALUE ) CloseHandle( m_h ); }
        bool Open( const std::wstring &sPath, std::wstring &sErr )
        {
            m_h = CreateFileW( sPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr );
            LARGE_INTEGER li = {};
            if ( m_h == INVALID_HANDLE_VALUE || !GetFileSizeEx( m_h, &li ) ) { sErr = L"The file could not be opened."; return false; }
            m_llSize = li.QuadPart;
            return true;
        }
        int64_t Size() override { return m_llSize; }
        bool Seek( uint64_t x ) override
        {
            Rewind();   // the peek buffer dropped; then straight to x
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)x;
            if ( !SetFilePointerEx( m_h, li, nullptr, FILE_BEGIN ) ) { sError = L"The file could not be read."; return false; }
            m_llPos = x;
            return true;
        }

    protected:
        int64_t Raw( uint8_t *p, size_t n ) override
        {
            if ( Cancelled( m_pCancel ) ) { sError = L"Cancelled."; return -1; }
            DWORD dw = 0;
            if ( !ReadFile( m_h, p, (DWORD)std::min< size_t >( n, 1u << 30 ), &dw, nullptr ) ) { sError = L"The file could not be read."; return -1; }
            return dw;
        }
        bool DoRewind() override
        {
            LARGE_INTEGER li = {};
            return SetFilePointerEx( m_h, li, nullptr, FILE_BEGIN ) != 0;
        }

    private:
        const std::atomic< bool > *m_pCancel;
        HANDLE m_h = INVALID_HANDLE_VALUE;
    };

    // A MIDI, also compressed: .mid or .midi, then perhaps more extensions (song.mid.xz, song.midi.7z)
    bool IsMidiEntry( const char *s )
    {
        if ( !s ) return false;
        std::string t( s );
        for ( char &c : t ) c = (char)tolower( (unsigned char)c );
        const size_t iSlash = t.find_last_of( "/\\" );
        if ( iSlash != std::string::npos ) t = t.substr( iSlash + 1 );
        for ( const char *sExt : { ".mid", ".midi" } )
            for ( size_t i = t.find( sExt ); i != std::string::npos && i > 0; i = t.find( sExt, i + 1 ) )
            {
                const size_t iEnd = i + strlen( sExt );
                if ( iEnd == t.size() || t[iEnd] == '.' ) return true;
            }
        return false;
    }

    // One layer: libarchive reading the stream around it through callbacks (stacked filters, such as
    // .xz.xz, all at once), on the entry to take (Archive::Open)
    class ArchiveStream : public Stream
    {
    public:
        explicit ArchiveStream( const std::atomic< bool > *pCancel ) : m_pCancel( pCancel ) {}
        ~ArchiveStream() { if ( m_a ) archive_read_free( m_a ); }

        // bSeek: the layer may seek in its parent (a 7z needs to)
        bool Open( std::unique_ptr< Stream > pParent, bool bSeek, bool bOuter, std::wstring &sErr, const Archive::Chooser &fnChoose )
        {
            m_pParent = std::move( pParent );
            m_bSeek = bSeek;
            if ( !Start() ) { sErr = Error( L"The file could not be opened as an archive or compressed file" ); return false; }
            archive_entry *e = nullptr;
            int i = 0, iRet;
            std::vector< Archive::Entry > vFound;
            std::vector< int > vIndex;
            std::vector< std::string > vName;
            while ( ( iRet = archive_read_next_header( m_a, &e ) ) == ARCHIVE_OK || iRet == ARCHIVE_WARN || ( i == 0 && SevenZipUnderFilter() ) )
            {
                // A 7z under a compression filter (a .7z.xz) cannot seek in it: this layer is the
                // filter alone, and the 7z the next
                if ( i == 0 && SevenZipUnderFilter() )
                {
                    m_bRawOnly = true;
                    archive_read_free( m_a );
                    m_a = nullptr;
                    if ( !m_pParent->Rewind() || !Start() ) { sErr = Error( L"The file could not be read again" ); return false; }
                    continue;
                }
                bool bRaw = archive_format( m_a ) == ARCHIVE_FORMAT_RAW;
                if ( bRaw || archive_entry_filetype( e ) == AE_IFREG )
                {
                    const char *s = archive_entry_pathname_utf8( e );
                    if ( !s ) s = archive_entry_pathname( e );
                    int64_t ll = archive_entry_size_is_set( e ) ? archive_entry_size( e ) : -1;
                    // A bare compressed file has just the one; in a compressed archive going back means
                    // decompressing again, so the first MIDI is read on (as without a choice)
                    if ( bRaw || ( IsMidiEntry( s ) && ( !fnChoose || archive_filter_count( m_a ) > 1 ) ) )
                    {
                        if ( !bRaw ) m_sName = s;
                        m_iEntry = i;
                        m_llSize = ll;
                        return Check( bOuter, sErr );
                    }
                    if ( IsMidiEntry( s ) )
                    {
                        vFound.push_back( { Util::Utf8ToWide( s ), ll } );
                        vIndex.push_back( i );
                        vName.push_back( s );
                    }
                }
                if ( archive_read_data_skip( m_a ) == ARCHIVE_FATAL ) break;
                i++;
            }
            if ( Cancelled( m_pCancel ) ) { sErr = L"Cancelled."; return false; }
            if ( iRet == ARCHIVE_FATAL || iRet == ARCHIVE_WARN ) { sErr = Error( L"The archive could not be read" ); return false; }
            if ( vFound.empty() ) { sErr = L"The archive holds no MIDI file (.mid or .midi)."; return false; }
            int k = vFound.size() > 1 ? fnChoose( vFound ) : 0;
            if ( k < 0 || k >= (int)vFound.size() || Cancelled( m_pCancel ) ) { sErr = L"Cancelled."; return false; }
            m_iEntry = vIndex[k];
            m_sName = vName[k];
            m_llSize = vFound[k].llSize;
            if ( !DoRewind() ) { sErr = sError; return false; }
            return Check( bOuter, sErr );
        }
        // The layer's filters and format, outermost first ("xz > 7-Zip > ")
        std::wstring Kind() const
        {
            std::wstring s;
            for ( int f = archive_filter_count( m_a ) - 2; f >= 0; f-- ) s += Util::Utf8ToWide( archive_filter_name( m_a, f ) ) + L" > ";
            if ( archive_format( m_a ) != ARCHIVE_FORMAT_RAW ) s += Util::Utf8ToWide( archive_format_name( m_a ) ) + L" > ";
            return s;
        }
        const std::string &Name() const { return m_sName; }
        // A 7z itself (not under a filter), and the entry taken: its index in the archive's files
        bool IsSevenZip() const { return archive_format( m_a ) == ARCHIVE_FORMAT_7ZIP && archive_filter_count( m_a ) <= 1; }
        // Its compression filters (stacked ones each: an .xz.xz is two) and its archive format, if any
        int LayerCount() const { return std::max( 0, archive_filter_count( m_a ) - 1 ) + ( archive_format( m_a ) != ARCHIVE_FORMAT_RAW ? 1 : 0 ); }
        int EntryIndex() const { return m_iEntry; }
        // The stream it reads, for another reader instead (this layer is not read after)
        std::unique_ptr< Stream > TakeParent()
        {
            archive_read_free( m_a );
            m_a = nullptr;
            return std::move( m_pParent );
        }

    protected:
        int64_t Raw( uint8_t *p, size_t n ) override
        {
            la_ssize_t r = archive_read_data( m_a, p, n );
            if ( r >= 0 ) return r;
            sError = Cancelled( m_pCancel ) ? L"Cancelled." : Error( L"The file could not be decompressed" );
            return -1;
        }
        bool DoRewind() override
        {
            if ( m_a ) archive_read_free( m_a );
            m_a = nullptr;
            if ( !m_pParent->Rewind() || !Start() ) { sError = Error( L"The file could not be read again" ); return false; }
            archive_entry *e = nullptr;
            for ( int i = 0; i <= m_iEntry; i++ )
            {
                int iRet = archive_read_next_header( m_a, &e );
                if ( iRet != ARCHIVE_OK && iRet != ARCHIVE_WARN ) { sError = Error( L"The archive could not be read" ); return false; }
                if ( i < m_iEntry ) archive_read_data_skip( m_a );
            }
            return true;
        }

    private:
        bool Start()
        {
            m_a = archive_read_new();
            archive_read_support_filter_all( m_a );
            if ( !m_bRawOnly ) archive_read_support_format_all( m_a );
            archive_read_support_format_raw( m_a );   // a bare compressed file: one entry, its contents
            archive_read_set_callback_data( m_a, this );
            archive_read_set_read_callback( m_a, CbRead );
            archive_read_set_skip_callback( m_a, CbSkip );
            if ( m_bSeek ) archive_read_set_seek_callback( m_a, CbSeek );
            return archive_read_open1( m_a ) == ARCHIVE_OK;
        }
        bool SevenZipUnderFilter() const { return !m_bRawOnly && archive_format( m_a ) == ARCHIVE_FORMAT_7ZIP && archive_filter_count( m_a ) > 1; }
        bool Check( bool bOuter, std::wstring &sErr )
        {
            if ( archive_format( m_a ) == ARCHIVE_FORMAT_RAW && archive_filter_count( m_a ) <= 1 )
            {
                sErr = bOuter ? L"The file is neither a MIDI file nor a compressed file PFAviz++ can read."
                              : L"The archive holds a file that is neither a MIDI file nor a compressed one.";
                return false;
            }
            return true;
        }
        std::wstring Error( const wchar_t *sWhat ) const
        {
            if ( !m_pParent->sError.empty() ) return m_pParent->sError;
            const char *s = m_a ? archive_error_string( m_a ) : nullptr;
            return s ? Format( L"%ls: %ls", sWhat, Util::Utf8ToWide( s ).c_str() ) : std::wstring( sWhat ) + L".";
        }
        static la_ssize_t CbRead( archive *a, void *pv, const void **pp )
        {
            ArchiveStream &s = *(ArchiveStream *)pv;
            if ( s.m_vBuf.empty() ) s.m_vBuf.resize( 1 << 20 );
            int64_t r = s.m_pParent->Read( s.m_vBuf.data(), s.m_vBuf.size() );
            if ( r < 0 ) { archive_set_error( a, EIO, "%s", Util::WideToUtf8( s.m_pParent->sError ).c_str() ); return -1; }
            *pp = s.m_vBuf.data();
            return (la_ssize_t)r;
        }
        static la_int64_t CbSkip( archive *, void *pv, la_int64_t n )
        {
            Stream &p = *( (ArchiveStream *)pv )->m_pParent;
            uint64_t llFrom = p.Pos();
            if ( n <= 0 || !p.Seek( llFrom + (uint64_t)n ) ) return 0;
            return (la_int64_t)( p.Pos() - llFrom );
        }
        static la_int64_t CbSeek( archive *, void *pv, la_int64_t llOff, int iWhence )
        {
            Stream &p = *( (ArchiveStream *)pv )->m_pParent;
            int64_t llTo = llOff;
            if ( iWhence == SEEK_CUR ) llTo += (int64_t)p.Pos();
            else if ( iWhence == SEEK_END )
            {
                int64_t llSize = p.Size();
                if ( llSize < 0 ) return ARCHIVE_FATAL;
                llTo += llSize;
            }
            if ( llTo < 0 || !p.Seek( (uint64_t)llTo ) ) return ARCHIVE_FATAL;
            return (la_int64_t)p.Pos();
        }

        const std::atomic< bool > *m_pCancel;
        std::unique_ptr< Stream > m_pParent;
        archive *m_a = nullptr;
        bool m_bSeek = false, m_bRawOnly = false;
        int m_iEntry = 0;
        std::string m_sName;
        std::vector< uint8_t > m_vBuf;
    };

    //-------------------------------------------------------------------------
    // Decoding on several threads (the loading threads): where a compressor working on several
    // threads left parts that decode apart
    //-------------------------------------------------------------------------

    // An xz layer, by liblzma's decoder on several threads (blocks that record their sizes, as xz -T
    // and 7-Zip write them, decode at once; others in turn), concatenated streams included
    class XzStream : public Stream
    {
    public:
        XzStream( std::unique_ptr< Stream > p, int iThreads, const std::atomic< bool > *pCancel )
            : m_p( std::move( p ) ), m_iThreads( iThreads ), m_pCancel( pCancel ) {}
        ~XzStream() { lzma_end( &m_z ); }
        bool Open( std::wstring &sErr )
        {
            if ( !Init() ) { sErr = L"The file could not be decompressed (xz)."; return false; }
            return true;
        }
        uint64_t Progress() const override { return m_p->Progress(); }

    protected:
        int64_t Raw( uint8_t *p, size_t n ) override
        {
            if ( m_bEnd || !n ) return 0;
            m_z.next_out = p;
            m_z.avail_out = n;
            while ( m_z.avail_out == n )
            {
                if ( Cancelled( m_pCancel ) ) { sError = L"Cancelled."; return -1; }
                if ( !m_z.avail_in && !m_bInEnd )
                {
                    int64_t r = m_p->Read( m_vIn.data(), m_vIn.size() );
                    if ( r < 0 ) { sError = m_p->sError; return -1; }
                    m_bInEnd = !r;
                    m_z.next_in = m_vIn.data();
                    m_z.avail_in = (size_t)r;
                }
                lzma_ret ret = lzma_code( &m_z, m_bInEnd ? LZMA_FINISH : LZMA_RUN );
                if ( ret == LZMA_STREAM_END ) { m_bEnd = true; break; }
                if ( ret != LZMA_OK ) { sError = L"The file could not be decompressed: its xz data is damaged."; return -1; }
            }
            return (int64_t)( n - m_z.avail_out );
        }
        bool DoRewind() override
        {
            if ( !m_p->Rewind() || !Init() ) { sError = m_p->sError.empty() ? L"The file could not be read again." : m_p->sError; return false; }
            return true;
        }

    private:
        bool Init()
        {
            lzma_end( &m_z );
            m_z = LZMA_STREAM_INIT;
            m_vIn.resize( 1 << 20 );
            m_bEnd = m_bInEnd = false;
            lzma_mt mt = {};
            mt.flags = LZMA_CONCATENATED;
            mt.threads = (uint32_t)std::max( 1, m_iThreads );
            mt.memlimit_threading = uint64_t( 1 ) << 30;    // past it (blocks too big to hold a thread's each), in turn
            mt.memlimit_stop = UINT64_MAX;
            return lzma_stream_decoder_mt( &m_z, &mt ) == LZMA_OK;
        }
        std::unique_ptr< Stream > m_p;
        int m_iThreads;
        const std::atomic< bool > *m_pCancel;
        lzma_stream m_z = LZMA_STREAM_INIT;
        std::vector< uint8_t > m_vIn;
        bool m_bEnd = false, m_bInEnd = false;
    };

    // Reads at an offset, from any thread
    bool ReadAt( HANDLE h, uint64_t llOff, void *p, DWORD n, DWORD &got )
    {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)llOff;
        ov.OffsetHigh = (DWORD)( llOff >> 32 );
        got = 0;
        return ReadFile( h, p, n, &got, &ov ) || GetLastError() == ERROR_HANDLE_EOF;
    }

    // 7z headers, as far as one file's place: the folder (the coders that decode a run of the packed
    // streams to one output) it is in, and where in that folder's output it is
    struct SzPlace
    {
        uint64_t llPack = 0, llPackSize = 0;    // the folder's packed stream, in the file
        uint64_t llSkip = 0, llSize = 0;        // the file in the folder's output
        uint32_t uDict = 0;                     // LZMA2's dictionary
        bool bCrc = false;
        uint32_t uCrc = 0;
    };

    struct SzReader
    {
        const uint8_t *p, *e;
        bool bBad = false;
        uint8_t Byte() { if ( p >= e ) { bBad = true; return 0; } return *p++; }
        uint64_t Num()      // 7z's numbers: the first byte's high bits say how many bytes follow
        {
            uint8_t first = Byte(), mask = 0x80;
            uint64_t v = 0;
            for ( int i = 0; i < 8; i++, mask >>= 1 )
            {
                if ( !( first & mask ) ) return v | (uint64_t)( first & ( mask - 1 ) ) << ( 8 * i );
                v |= (uint64_t)Byte() << ( 8 * i );
            }
            return v;
        }
        size_t Count() { uint64_t n = Num(); if ( n > (uint64_t)( e - p ) * 8 + 8 ) { bBad = true; return 0; } return (size_t)n; }
        void Skip( uint64_t n ) { if ( n > (uint64_t)( e - p ) ) { bBad = true; p = e; } else p += n; }
        uint32_t U32() { uint32_t v = 0; for ( int i = 0; i < 4; i++ ) v |= (uint32_t)Byte() << ( 8 * i ); return v; }
        std::vector< bool > Bits( size_t n )
        {
            std::vector< bool > v( n );
            uint8_t b = 0;
            for ( size_t i = 0; i < n; i++ ) { if ( !( i & 7 ) ) b = Byte(); v[i] = ( b >> ( 7 - ( i & 7 ) ) ) & 1; }
            return v;
        }
        std::vector< bool > Defined( size_t n ) { return Byte() ? std::vector< bool >( n, true ) : Bits( n ); }
    };

    struct SzCoder { std::vector< uint8_t > id, props; uint64_t nIn = 1, nOut = 1; };
    struct SzFolder
    {
        std::vector< SzCoder > vCoders;
        std::vector< uint64_t > vBoundOut;      // out streams bound to another coder's input
        uint64_t nPacked = 1;
        std::vector< uint64_t > vUnpack;        // each out stream's size
        bool bCrc = false;
        uint32_t uCrc = 0;
        uint64_t Unpack() const     // the folder's output: the out stream not bound
        {
            for ( size_t i = 0; i < vUnpack.size(); i++ )
                if ( std::find( vBoundOut.begin(), vBoundOut.end(), i ) == vBoundOut.end() ) return vUnpack[i];
            return 0;
        }
    };
    struct SzStreams
    {
        uint64_t llPackPos = 0;
        std::vector< uint64_t > vPackSizes;
        std::vector< SzFolder > vFolders;
        std::vector< uint64_t > vNumSub, vSubSizes;     // files per folder, and each file's size
        std::vector< bool > vSubCrc;
        std::vector< uint32_t > vSubCrcs;
    };

    bool SzReadStreams( SzReader &r, SzStreams &s )
    {
        for ( ;; )
        {
            uint64_t t = r.Num();
            if ( r.bBad ) return false;
            if ( !t ) break;
            if ( t == 0x06 )        // PackInfo
            {
                s.llPackPos = r.Num();
                s.vPackSizes.resize( r.Count() );
                for ( uint64_t u; ( u = r.Num() ) != 0 && !r.bBad; )
                {
                    if ( u == 0x09 ) for ( uint64_t &x : s.vPackSizes ) x = r.Num();
                    else if ( u == 0x0A ) { std::vector< bool > d = r.Defined( s.vPackSizes.size() ); for ( bool b : d ) if ( b ) r.U32(); }
                    else return false;
                }
            }
            else if ( t == 0x07 )   // UnpackInfo
            {
                if ( r.Num() != 0x0B ) return false;
                s.vFolders.resize( r.Count() );
                if ( r.Byte() != 0 ) return false;  // (folders kept elsewhere: not written by 7-Zip)
                for ( SzFolder &f : s.vFolders )
                {
                    size_t nc = r.Count();
                    if ( !nc || nc > 64 ) return false;
                    uint64_t nIn = 0, nOut = 0;
                    for ( size_t c = 0; c < nc && !r.bBad; c++ )
                    {
                        SzCoder cd;
                        uint8_t fl = r.Byte();
                        if ( fl & 0x80 ) return false;
                        for ( int i = 0; i < ( fl & 0x0F ); i++ ) cd.id.push_back( r.Byte() );
                        if ( fl & 0x10 ) { cd.nIn = r.Num(); cd.nOut = r.Num(); }
                        if ( fl & 0x20 ) { size_t n = r.Count(); for ( size_t i = 0; i < n; i++ ) cd.props.push_back( r.Byte() ); }
                        nIn += cd.nIn, nOut += cd.nOut;
                        f.vCoders.push_back( std::move( cd ) );
                    }
                    if ( !nOut || nOut > 64 || nIn > 64 || nIn < nOut - 1 ) return false;
                    for ( uint64_t b = 0; b + 1 < nOut; b++ ) { r.Num(); f.vBoundOut.push_back( r.Num() ); }
                    f.nPacked = nIn - ( nOut - 1 );
                    if ( f.nPacked > 1 ) for ( uint64_t k = 0; k < f.nPacked; k++ ) r.Num();
                    f.vUnpack.resize( (size_t)nOut );
                }
                if ( r.Num() != 0x0C ) return false;
                for ( SzFolder &f : s.vFolders ) for ( uint64_t &x : f.vUnpack ) x = r.Num();
                for ( uint64_t u; ( u = r.Num() ) != 0 && !r.bBad; )
                {
                    if ( u != 0x0A ) return false;
                    std::vector< bool > d = r.Defined( s.vFolders.size() );
                    for ( size_t i = 0; i < d.size(); i++ ) if ( d[i] ) { s.vFolders[i].bCrc = true; s.vFolders[i].uCrc = r.U32(); }
                }
            }
            else if ( t == 0x08 )   // SubStreamsInfo
            {
                s.vNumSub.assign( s.vFolders.size(), 1 );
                uint64_t u = r.Num();
                if ( u == 0x0D ) { for ( uint64_t &x : s.vNumSub ) x = r.Count(); u = r.Num(); }
                for ( size_t f = 0; f < s.vFolders.size(); f++ )
                {
                    if ( !s.vNumSub[f] ) continue;
                    uint64_t llSum = 0;
                    for ( uint64_t k = 1; k < s.vNumSub[f]; k++ )
                    {
                        if ( u != 0x09 ) return false;
                        uint64_t x = r.Num();
                        s.vSubSizes.push_back( x );
                        llSum += x;
                    }
                    s.vSubSizes.push_back( s.vFolders[f].Unpack() - llSum );
                }
                if ( u == 0x09 ) u = r.Num();
                size_t nDigests = 0;
                for ( size_t f = 0; f < s.vFolders.size(); f++ )
                    if ( s.vNumSub[f] != 1 || !s.vFolders[f].bCrc ) nDigests += (size_t)s.vNumSub[f];
                std::vector< bool > vDef;
                std::vector< uint32_t > vCrc;
                for ( ; u != 0 && !r.bBad; u = r.Num() )
                {
                    if ( u != 0x0A ) return false;
                    vDef = r.Defined( nDigests );
                    for ( bool b : vDef ) vCrc.push_back( b ? r.U32() : 0 );
                }
                for ( size_t f = 0, k = 0; f < s.vFolders.size(); f++ )
                    for ( uint64_t j = 0; j < s.vNumSub[f]; j++ )
                    {
                        if ( s.vNumSub[f] == 1 && s.vFolders[f].bCrc ) { s.vSubCrc.push_back( true ); s.vSubCrcs.push_back( s.vFolders[f].uCrc ); continue; }
                        bool b = k < vDef.size() && vDef[k];
                        s.vSubCrc.push_back( b );
                        s.vSubCrcs.push_back( b ? vCrc[k] : 0 );
                        k++;
                    }
            }
            else return false;
        }
        if ( s.vNumSub.empty() )
        {
            s.vNumSub.assign( s.vFolders.size(), 1 );
            for ( const SzFolder &f : s.vFolders ) { s.vSubSizes.push_back( f.Unpack() ); s.vSubCrc.push_back( f.bCrc ); s.vSubCrcs.push_back( f.uCrc ); }
        }
        return !r.bBad;
    }

    bool SzIs( const SzCoder &c, std::initializer_list< uint8_t > id ) { return c.id.size() == id.size() && std::equal( id.begin(), id.end(), c.id.begin() ); }

    // A folder of one coder (copy, LZMA or LZMA2) in memory: the encoded header's
    bool SzDecode( const SzFolder &f, const uint8_t *p, size_t n, std::vector< uint8_t > &out )
    {
        if ( f.vCoders.size() != 1 || f.Unpack() > ( 256u << 20 ) ) return false;
        const SzCoder &c = f.vCoders[0];
        out.resize( (size_t)f.Unpack() );
        if ( SzIs( c, { 0x00 } ) ) { if ( n < out.size() ) return false; memcpy( out.data(), p, out.size() ); return true; }
        lzma_filter flt[2] = { { SzIs( c, { 0x03, 0x01, 0x01 } ) ? LZMA_FILTER_LZMA1 : SzIs( c, { 0x21 } ) ? LZMA_FILTER_LZMA2 : LZMA_VLI_UNKNOWN, nullptr },
                               { LZMA_VLI_UNKNOWN, nullptr } };
        if ( flt[0].id == LZMA_VLI_UNKNOWN || lzma_properties_decode( &flt[0], nullptr, c.props.data(), c.props.size() ) != LZMA_OK ) return false;
        lzma_stream z = LZMA_STREAM_INIT;
        lzma_ret ret = lzma_raw_decoder( &z, flt );
        free( flt[0].options );
        if ( ret != LZMA_OK ) return false;
        z.next_in = p;
        z.avail_in = n;
        z.next_out = out.data();
        z.avail_out = out.size();
        while ( z.avail_out && ( ret = lzma_code( &z, LZMA_RUN ) ) == LZMA_OK && z.avail_in ) {}
        lzma_end( &z );
        return !z.avail_out;
    }

    std::wstring SzName( std::wstring s ) { std::replace( s.begin(), s.end(), L'\\', L'/' ); return s; }

    // The place of the archive's iEntry-th file (libarchive's order: the header's), named sName, when
    // its folder is one LZMA2 coder
    bool SevenZipPlace( HANDLE h, uint64_t llFile, int iEntry, const std::string &sName, SzPlace &pl )
    {
        uint8_t a[32];
        DWORD got = 0;
        if ( !ReadAt( h, 0, a, 32, got ) || got != 32 || memcmp( a, SevenZipMagic, 6 ) != 0 ) return false;
        uint64_t llNext = 0, llNextSize = 0;
        for ( int i = 7; i >= 0; i-- ) llNext = llNext << 8 | a[12 + i], llNextSize = llNextSize << 8 | a[20 + i];
        if ( !llNextSize || llNextSize > ( 64u << 20 ) || 32 + llNext + llNextSize > llFile ) return false;
        std::vector< uint8_t > vHdr( (size_t)llNextSize );
        if ( !ReadAt( h, 32 + llNext, vHdr.data(), (DWORD)vHdr.size(), got ) || got != vHdr.size() ) return false;
        // An encoded header: the header compressed, as a folder of its own
        for ( int k = 0; k < 4 && !vHdr.empty() && vHdr[0] == 0x17; k++ )
        {
            SzReader r{ vHdr.data() + 1, vHdr.data() + vHdr.size() };
            SzStreams s;
            if ( !SzReadStreams( r, s ) || s.vFolders.size() != 1 || s.vPackSizes.empty() || s.vPackSizes[0] > ( 64u << 20 ) ) return false;
            std::vector< uint8_t > vPacked( (size_t)s.vPackSizes[0] ), vOut;
            if ( !ReadAt( h, 32 + s.llPackPos, vPacked.data(), (DWORD)vPacked.size(), got ) || got != vPacked.size() ) return false;
            if ( !SzDecode( s.vFolders[0], vPacked.data(), vPacked.size(), vOut ) ) return false;
            vHdr = std::move( vOut );
        }
        SzReader r{ vHdr.data(), vHdr.data() + vHdr.size() };
        if ( r.Byte() != 0x01 ) return false;
        SzStreams s;
        std::vector< std::wstring > vNames;
        std::vector< bool > vEmpty;
        size_t nFiles = 0;
        for ( uint64_t t; ( t = r.Num() ) != 0; )
        {
            if ( r.bBad ) return false;
            if ( t == 0x02 ) { for ( uint64_t u; ( u = r.Num() ) != 0 && !r.bBad; ) r.Skip( r.Num() ); }
            else if ( t == 0x03 ) { SzStreams x; if ( !SzReadStreams( r, x ) ) return false; }
            else if ( t == 0x04 ) { if ( !SzReadStreams( r, s ) ) return false; }
            else if ( t == 0x05 )
            {
                nFiles = r.Count();
                vEmpty.assign( nFiles, false );
                vNames.assign( nFiles, L"" );
                for ( uint64_t u; ( u = r.Num() ) != 0; )
                {
                    uint64_t n = r.Num();
                    if ( r.bBad || n > (uint64_t)( r.e - r.p ) ) return false;
                    SzReader q{ r.p, r.p + n };
                    r.Skip( n );
                    if ( u == 0x0E ) vEmpty = q.Bits( nFiles );
                    else if ( u == 0x11 )
                    {
                        if ( q.Byte() != 0 ) return false;
                        for ( std::wstring &w : vNames )
                            for ( uint16_t c; !q.bBad && ( c = (uint16_t)( q.Byte() | q.Byte() << 8 ) ) != 0; ) w += (wchar_t)c;
                    }
                    if ( q.bBad ) return false;
                }
            }
            else return false;
        }
        if ( iEntry < 0 || (size_t)iEntry >= nFiles || vEmpty[iEntry] || SzName( vNames[iEntry] ) != SzName( Util::Utf8ToWide( sName ) ) ) return false;
        // Its stream: the files with one before it, through the folders
        uint64_t j = 0;
        for ( int i = 0; i < iEntry; i++ ) j += !vEmpty[i];
        uint64_t llPack = 32 + s.llPackPos;
        for ( size_t f = 0, iPack = 0, iSub = 0; f < s.vFolders.size(); f++ )
        {
            const SzFolder &fd = s.vFolders[f];
            if ( iPack + fd.nPacked > s.vPackSizes.size() ) return false;
            if ( j < s.vNumSub[f] )
            {
                if ( fd.vCoders.size() != 1 || fd.nPacked != 1 || !SzIs( fd.vCoders[0], { 0x21 } ) || fd.vCoders[0].props.size() != 1 ) return false;
                uint8_t d = fd.vCoders[0].props[0];
                if ( d > 40 ) return false;
                pl.uDict = d == 40 ? UINT32_MAX : ( 2u | ( d & 1 ) ) << ( d / 2 + 11 );
                pl.llPack = llPack;
                pl.llPackSize = s.vPackSizes[iPack];
                pl.llSkip = 0;
                for ( uint64_t k = 0; k < j; k++ ) pl.llSkip += s.vSubSizes[iSub + k];
                pl.llSize = s.vSubSizes[iSub + j];
                pl.bCrc = s.vSubCrc[iSub + j];
                pl.uCrc = s.vSubCrcs[iSub + j];
                return pl.llPack + pl.llPackSize <= llFile && pl.llSkip + pl.llSize <= fd.Unpack();
            }
            j -= s.vNumSub[f];
            iSub += (size_t)s.vNumSub[f];
            for ( uint64_t k = 0; k < fd.nPacked; k++ ) llPack += s.vPackSizes[iPack++];
        }
        return false;
    }

    // LZMA2's chunks, header by header: a control byte (0: the end), the sizes, then at most 64 KB
    struct Lzma2Walker
    {
        HANDLE h;
        uint64_t llPos, llEnd;
        std::vector< uint8_t > vBuf = std::vector< uint8_t >( 1 << 20 );
        uint64_t llBuf = 0;
        size_t iBuf = 0;                        // vBuf holds [llBuf, llBuf + iBuf)
        uint8_t c = 0;
        uint32_t uHdr = 0, uUnc = 0, uPacked = 0;
        // The chunk at llPos (false: unreadable, or not LZMA2)
        bool Next()
        {
            if ( llPos >= llEnd ) { c = 0; return true; }   // (no end marker)
            if ( llPos < llBuf || llPos + 6 > llBuf + iBuf )
            {
                DWORD got = 0;
                if ( !ReadAt( h, llPos, vBuf.data(), (DWORD)std::min< uint64_t >( vBuf.size(), llEnd - llPos ), got ) ) return false;
                llBuf = llPos;
                iBuf = got;
            }
            const uint8_t *p = vBuf.data() + ( llPos - llBuf );
            size_t iAvail = (size_t)( llBuf + iBuf - llPos );
            if ( !iAvail ) return false;
            c = p[0];
            if ( !c ) return true;
            if ( c == 1 || c == 2 )
            {
                if ( iAvail < 3 ) return false;
                uHdr = 3;
                uUnc = uPacked = ( (uint32_t)p[1] << 8 | p[2] ) + 1;
            }
            else if ( c >= 0x80 )
            {
                uHdr = c >= 0xC0 ? 6 : 5;
                if ( iAvail < uHdr ) return false;
                uUnc = ( (uint32_t)( c & 0x1F ) << 16 | (uint32_t)p[1] << 8 | p[2] ) + 1;
                uPacked = ( (uint32_t)p[3] << 8 | p[4] ) + 1;
            }
            else return false;
            return llPos + uHdr + uPacked <= llEnd;
        }
        void Skip() { llPos += uHdr + uPacked; }
        // A chunk that resets the dictionary and sets the properties: the stream from it decodes alone
        bool Fresh() const { return c >= 0xE0; }
    };

    // A 7z's MIDI in a folder of one LZMA2 coder, decoded on several threads. 7-Zip compressing on
    // several threads starts each block (4 dictionaries, 64 MB at its default level) with a dictionary
    // reset, so the blocks decode apart. One thread walks the chunk headers and cuts the folder into
    // units of whole blocks; each decoder takes the next unit and reads its bytes itself; the output
    // is handed out in order. A unit ahead of the one being read holds at most m_llHold decoded.
    class Lzma2Parallel : public Stream
    {
    public:
        Lzma2Parallel( std::unique_ptr< Stream > pFile, const SzPlace &pl, const std::atomic< bool > *pCancel )
            : m_pFile( std::move( pFile ) ), m_pl( pl ), m_pCancel( pCancel ) { m_llSize = (int64_t)pl.llSize; }
        ~Lzma2Parallel() { Stop(); if ( m_h != INVALID_HANDLE_VALUE ) CloseHandle( m_h ); }
        // False: not for this file (no block starts early on, so it would decode in turn)
        bool Open( const std::wstring &sPath, int iThreads )
        {
            m_h = CreateFileW( sPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
            if ( m_h == INVALID_HANDLE_VALUE ) return false;
            Lzma2Walker w{ m_h, m_pl.llPack, m_pl.llPack + m_pl.llPackSize };
            uint64_t llUnc = 0;
            for ( bool bFirst = true;; bFirst = false )
            {
                if ( !w.Next() || !w.c || w.llPos - m_pl.llPack > ProbeBytes ) return false;
                if ( !bFirst && w.Fresh() && llUnc >= UnitBytes ) break;
                llUnc += w.uUnc;
                w.Skip();
            }
            // As many decoders as hold a unit each in Budget
            m_iDecoders = (int)std::max< uint64_t >( 2, std::min< uint64_t >( (uint64_t)iThreads, Budget / llUnc ) );
            m_llHold = std::min( Budget / 2, std::max( llUnc, Budget / m_iDecoders ) );
            Start();
            return true;
        }
        int Decoders() const { return m_iDecoders; }
        void Keep( std::unique_ptr< Stream > pFile ) { m_pFile = std::move( pFile ); }
        uint64_t Progress() const override { return m_llFileDone; }

    protected:
        int64_t Raw( uint8_t *p, size_t n ) override
        {
            std::unique_lock< std::mutex > l( m_mx );
            for ( ;; )
            {
                if ( m_llOut >= m_pl.llSize ) return Finish( l ) ? 0 : -1;
                if ( !m_q.empty() )
                {
                    Unit &u = *m_q.front();
                    if ( !u.qOut.empty() )
                    {
                        std::vector< uint8_t > &v = u.qOut.front();
                        size_t k = std::min( n, v.size() - u.iFront );
                        memcpy( p, v.data() + u.iFront, k );
                        u.iFront += k;
                        m_llOut += k;
                        if ( u.iFront == v.size() ) { u.llHeld -= v.size(); u.qOut.pop_front(); u.iFront = 0; m_cv.notify_all(); }
                        return (int64_t)k;
                    }
                    if ( u.bDone ) { if ( !PopDone() ) return -1; continue; }
                }
                else if ( m_bSplitDone ) { sError = m_sFail.empty() ? L"The file could not be decompressed: it ends early." : m_sFail; return -1; }
                if ( Cancelled( m_pCancel ) ) { sError = L"Cancelled."; return -1; }
                m_cv.wait_for( l, std::chrono::milliseconds( 50 ) );
            }
        }
        bool DoRewind() override { Stop(); Start(); return true; }

    private:
        static const uint64_t UnitBytes = 16u << 20;    // a unit's output, at least (blocks joined)
        static const uint64_t Budget = 768u << 20;      // the units waiting, decoded
        static const uint64_t ProbeBytes = 64u << 20;   // a block starts within this much of the folder, or it is not split
        static const size_t PieceBytes = 4u << 20;
        struct Unit
        {
            uint64_t llOff = 0, llPacked = 0;           // its chunks in the file
            uint64_t llBegin = 0, llUnc = 0;            // its output, in the folder's
            std::deque< std::vector< uint8_t > > qOut;  // its part of the MIDI decoded, not yet handed out
            size_t iFront = 0;                          // of qOut.front(), handed out
            uint64_t llHeld = 0;                        // bytes in qOut
            uint32_t uCrc = 0;                          // of its part of the MIDI
            uint64_t llPart = 0;
            bool bTaken = false, bDone = false, bFailed = false;
        };

        void Start()
        {
            m_bStop = m_bSplitDone = m_bChecked = false;
            m_sFail.clear();
            m_q.clear();
            m_llOut = 0;
            m_uCrc = 0;
            m_llFileDone = m_pl.llPack;
            m_vThreads.emplace_back( [this]() { Split(); } );
            for ( int i = 0; i < m_iDecoders; i++ ) m_vThreads.emplace_back( [this]() { Decode(); } );
        }
        void Stop()
        {
            { std::lock_guard< std::mutex > l( m_mx ); m_bStop = true; }
            m_cv.notify_all();
            for ( std::thread &t : m_vThreads ) t.join();
            m_vThreads.clear();
        }
        void Fail( const wchar_t *s )
        {
            std::lock_guard< std::mutex > l( m_mx );
            if ( m_sFail.empty() ) m_sFail = s;
            m_bSplitDone = true;
            m_cv.notify_all();
        }
        // The front unit, decoded and read (m_mx held)
        bool PopDone()
        {
            Unit &u = *m_q.front();
            if ( u.bFailed ) { sError = L"The file could not be decompressed: its LZMA2 data is damaged."; return false; }
            m_uCrc = (uint32_t)crc32_combine64( m_uCrc, u.uCrc, (int64_t)u.llPart );
            m_llFileDone = u.llOff + u.llPacked;
            m_q.pop_front();
            m_cv.notify_all();
            return true;
        }
        // The MIDI read whole: its last unit decoded, and the CRC
        bool Finish( std::unique_lock< std::mutex > &l )
        {
            if ( m_bChecked ) return true;
            while ( !m_q.empty() && m_q.front()->llBegin < m_pl.llSkip + m_pl.llSize )
            {
                if ( m_q.front()->bDone ) { if ( !PopDone() ) return false; continue; }
                if ( Cancelled( m_pCancel ) ) { sError = L"Cancelled."; return false; }
                m_cv.wait_for( l, std::chrono::milliseconds( 50 ) );
            }
            m_bChecked = true;
            if ( m_pl.bCrc && m_uCrc != m_pl.uCrc ) { sError = L"The file is damaged: its CRC does not match."; return false; }
            return true;
        }

        void Split()
        {
            Lzma2Walker w{ m_h, m_pl.llPack, m_pl.llPack + m_pl.llPackSize };
            std::shared_ptr< Unit > cur;
            uint64_t llBegin = 0;
            for ( ;; )
            {
                if ( !w.Next() ) return Fail( L"The file could not be decompressed: its LZMA2 data is damaged." );
                if ( !w.c || ( cur && w.Fresh() && cur->llUnc >= UnitBytes ) )
                {
                    if ( cur )
                    {
                        cur->llPacked = w.llPos - cur->llOff;
                        llBegin += cur->llUnc;
                        std::unique_lock< std::mutex > l( m_mx );
                        m_cv.wait( l, [&]() { return m_bStop || m_q.size() < (size_t)m_iDecoders + 2; } );
                        if ( m_bStop ) return;
                        m_q.push_back( std::move( cur ) );
                        m_cv.notify_all();
                    }
                    if ( !w.c || llBegin >= m_pl.llSkip + m_pl.llSize ) break;     // (the files after it: not needed)
                }
                if ( !cur )
                {
                    if ( w.c != 1 && !w.Fresh() ) return Fail( L"The file could not be decompressed: its LZMA2 data is damaged." );
                    cur = std::make_shared< Unit >();
                    cur->llOff = w.llPos;
                    cur->llBegin = llBegin;
                }
                cur->llUnc += w.uUnc;
                w.Skip();
            }
            std::lock_guard< std::mutex > l( m_mx );
            m_bSplitDone = true;
            m_cv.notify_all();
        }

        void Decode()
        {
            std::vector< uint8_t > vIn( 1 << 20 );
            for ( ;; )
            {
                std::shared_ptr< Unit > u;
                {
                    std::unique_lock< std::mutex > l( m_mx );
                    m_cv.wait( l, [&]() {
                        if ( m_bStop || m_bSplitDone ) return true;
                        for ( auto &q : m_q ) if ( !q->bTaken ) return true;
                        return false; } );
                    if ( m_bStop ) return;
                    for ( auto &q : m_q ) if ( !q->bTaken ) { u = q; break; }
                    if ( !u ) return;   // all cut and taken
                    u->bTaken = true;
                }
                bool bOk = DecodeUnit( *u, vIn );
                std::lock_guard< std::mutex > l( m_mx );
                u->bDone = true;
                u->bFailed = !bOk;
                m_cv.notify_all();
            }
        }
        bool DecodeUnit( Unit &u, std::vector< uint8_t > &vIn )
        {
            lzma_options_lzma opt = {};
            opt.dict_size = std::max< uint32_t >( m_pl.uDict, LZMA_DICT_SIZE_MIN );
            lzma_filter flt[2] = { { LZMA_FILTER_LZMA2, &opt }, { LZMA_VLI_UNKNOWN, nullptr } };
            lzma_stream z = LZMA_STREAM_INIT;
            if ( lzma_raw_decoder( &z, flt ) != LZMA_OK ) return false;
            const uint64_t llA = m_pl.llSkip, llB = m_pl.llSkip + m_pl.llSize;  // the MIDI, in the folder's output
            uint64_t llIn = 0, llOut = 0;
            bool bMarker = false, bOk = true;
            std::vector< uint8_t > vOut;
            size_t iFill = 0;
            for ( ;; )
            {
                if ( Cancelled( m_pCancel ) ) { bOk = false; break; }
                if ( !z.avail_in && llIn < u.llPacked )
                {
                    DWORD got = 0;
                    if ( !ReadAt( m_h, u.llOff + llIn, vIn.data(), (DWORD)std::min< uint64_t >( vIn.size(), u.llPacked - llIn ), got ) || !got ) { bOk = false; break; }
                    z.next_in = vIn.data();
                    z.avail_in = got;
                    llIn += got;
                }
                else if ( !z.avail_in && !bMarker ) { vIn[0] = 0; z.next_in = vIn.data(); z.avail_in = 1; bMarker = true; }   // the end the unit stops before
                if ( vOut.empty() ) vOut.resize( PieceBytes );
                z.next_out = vOut.data() + iFill;
                z.avail_out = vOut.size() - iFill;
                lzma_ret ret = lzma_code( &z, LZMA_RUN );      // (no progress possible: LZMA_BUF_ERROR)
                if ( ret != LZMA_OK && ret != LZMA_STREAM_END ) { bOk = false; break; }
                iFill = vOut.size() - z.avail_out;
                if ( iFill < vOut.size() && ret != LZMA_STREAM_END ) continue;
                if ( llOut + iFill > u.llUnc ) { bOk = false; break; }
                // A piece decoded: its part of the MIDI, for the reader to take while more comes
                uint64_t a = std::max( llA, u.llBegin + llOut ), b = std::min( llB, u.llBegin + llOut + iFill );
                if ( a < b )
                {
                    size_t i0 = (size_t)( a - ( u.llBegin + llOut ) ), iLen = (size_t)( b - a );
                    if ( i0 ) memmove( vOut.data(), vOut.data() + i0, iLen );
                    vOut.resize( iLen );
                    u.uCrc = (uint32_t)crc32_z( u.uCrc, vOut.data(), iLen );
                    u.llPart += iLen;
                    std::unique_lock< std::mutex > l( m_mx );
                    u.llHeld += iLen;
                    u.qOut.push_back( std::move( vOut ) );
                    m_cv.notify_all();
                    m_cv.wait( l, [&]() { return m_bStop || u.llHeld < m_llHold || m_q.front().get() == &u; } );
                    if ( m_bStop ) { bOk = false; break; }
                }
                vOut = std::vector< uint8_t >();
                llOut += iFill;
                iFill = 0;
                if ( ret == LZMA_STREAM_END ) break;
            }
            lzma_end( &z );
            return bOk && llOut == u.llUnc;
        }

        std::unique_ptr< Stream > m_pFile;      // (kept for the reader's Total)
        SzPlace m_pl;
        const std::atomic< bool > *m_pCancel;
        HANDLE m_h = INVALID_HANDLE_VALUE;
        int m_iDecoders = 2;
        uint64_t m_llHold = 0;
        std::vector< std::thread > m_vThreads;
        std::mutex m_mx;
        std::condition_variable m_cv;
        std::deque< std::shared_ptr< Unit > > m_q;  // cut, not yet read whole
        bool m_bStop = false, m_bSplitDone = false, m_bChecked = false;
        std::wstring m_sFail;
        uint64_t m_llOut = 0;                   // of the MIDI, handed out
        uint32_t m_uCrc = 0;
        std::atomic< uint64_t > m_llFileDone{ 0 };
    };

    // The MIDI's chunks from the innermost stream
    class StreamReader : public Archive::Reader
    {
    public:
        StreamReader( std::unique_ptr< Stream > p, Stream *pFile, Stream *pProgress, std::wstring sLayers, std::wstring sName, int iLayers )
            : m_p( std::move( p ) ), m_pFile( pFile ), m_pProgress( pProgress ), m_sLayers( std::move( sLayers ) ), m_sName( std::move( sName ) ),
              m_iLayers( iLayers ) {}
        int Layers() const override { return m_iLayers; }
        bool NextChunk( char aType[4], uint64_t &llLen ) override
        {
            if ( m_llLeft && !m_p->Skip( m_llLeft ) ) { sError = m_p->sError; return false; }
            m_llLeft = 0;
            uint8_t a[8];
            size_t got = std::min< size_t >( m_vBack.size(), 8 );
            memcpy( a, m_vBack.data(), got );
            m_vBack.clear();
            while ( got < 8 )
            {
                int64_t r = m_p->Read( a + got, 8 - got );
                if ( r < 0 ) { sError = m_p->sError; return false; }
                if ( !r ) return false;
                got += (size_t)r;
            }
            memcpy( aType, a, 4 );
            llLen = m_llLeft = BE32( a + 4 );
            return true;
        }
        size_t ReadData( uint8_t *p, size_t n ) override
        {
            n = (size_t)std::min< uint64_t >( n, m_llLeft );
            if ( !n ) return 0;
            int64_t r = m_p->Read( p, n );
            if ( r < 0 ) sError = m_p->sError;
            if ( r <= 0 ) { m_llLeft = 0; return 0; }
            m_llLeft -= (uint64_t)r;
            return (size_t)r;
        }
        void PutBack( const uint8_t *p, size_t n ) override { m_vBack.assign( p, p + n ); }
        uint64_t Done() const override { return m_pProgress->Progress(); }
        uint64_t Total() const override { return (uint64_t)m_pFile->Size(); }
        std::wstring Summary() const override
        {
            return m_sLayers + ( m_sName.empty() ? L"MIDI" : m_sName ) + L", " + Bytes( (double)m_p->Pos() );
        }

    private:
        std::unique_ptr< Stream > m_p;
        Stream *m_pFile, *m_pProgress;
        std::wstring m_sLayers, m_sName;
        int m_iLayers;
        uint64_t m_llLeft = 0;      // the current chunk's unread data
        std::vector< uint8_t > m_vBack;     // PutBack's
    };
}

bool Archive::IsCompressed( const std::wstring &sPath )
{
    HANDLE h = CreateFileW( sPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
    if ( h == INVALID_HANDLE_VALUE ) return false;
    uint8_t a[8] = {};
    DWORD dw = 0;
    ReadFile( h, a, 8, &dw, nullptr );
    CloseHandle( h );
    if ( dw < 4 || IsMidiHead( a, dw ) || memcmp( a, "RIFF", 4 ) == 0 ) return false;
    // The signatures libarchive reads: xz, gz, bz2, zst, lz4, zip, 7z, rar; tar and lzma (no clear
    // signature) are tried by extension
    static const uint8_t aSig[][6] = { { 0xFD, '7', 'z', 'X', 'Z', 0 }, { 0x1F, 0x8B }, { 'B', 'Z', 'h' }, { 0x28, 0xB5, 0x2F, 0xFD },
                                       { 0x04, 0x22, 0x4D, 0x18 }, { 'P', 'K', 3, 4 }, { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C }, { 'R', 'a', 'r', '!', 0x1A, 0x07 } };
    static const int aLen[] = { 6, 2, 3, 4, 4, 4, 6, 6 };
    for ( int i = 0; i < 8; i++ )
        if ( (int)dw >= aLen[i] && memcmp( a, aSig[i], aLen[i] ) == 0 ) return true;
    size_t iDot = sPath.find_last_of( L'.' );
    std::wstring sExt = iDot == std::wstring::npos ? L"" : sPath.substr( iDot );
    return _wcsicmp( sExt.c_str(), L".tar" ) == 0 || _wcsicmp( sExt.c_str(), L".lzma" ) == 0;
}

std::unique_ptr< Archive::Reader > Archive::Open( const std::wstring &sPath, const std::atomic< bool > *pCancel, std::wstring &sError,
                                                  const Chooser &fnChoose, int iThreads )
{
    std::unique_ptr< FileStream > file( new FileStream( pCancel ) );
    if ( !file->Open( sPath, sError ) ) return nullptr;
    FileStream *pFile = file.get();
    Stream *pProgress = pFile;
    std::unique_ptr< Stream > cur = std::move( file );
    std::wstring sLayers, sName;
    int iLayers = 0;
    for ( int iDepth = 0; iDepth < MaxDepth; iDepth++ )
    {
        // Seeking costs nothing in the file itself; inside another layer only a 7z needs it (going
        // back reads that layer again from its start)
        size_t got = 0;
        const uint8_t *pHead = cur->Peek( 6, got );
        if ( !pHead ) { sError = cur->sError; return nullptr; }
        std::unique_ptr< Stream > layer;
        if ( iThreads > 1 && got == 6 && memcmp( pHead, XzMagic, 6 ) == 0 )
        {
            // xz, decoded on the loading threads
            std::unique_ptr< XzStream > xz( new XzStream( std::move( cur ), iThreads, pCancel ) );
            if ( !xz->Open( sError ) ) return nullptr;
            sLayers += L"xz > ";
            iLayers++;
            layer = std::move( xz );
        }
        else
        {
            bool bSeek = !iDepth || ( got == 6 && memcmp( pHead, SevenZipMagic, 6 ) == 0 );
            std::unique_ptr< ArchiveStream > arc( new ArchiveStream( pCancel ) );
            if ( !arc->Open( std::move( cur ), bSeek, !iDepth, sError, fnChoose ) ) return nullptr;
            if ( !arc->Name().empty() ) sName = Util::Utf8ToWide( arc->Name() );
            // The file a 7z whose MIDI is in a folder of LZMA2: its blocks decoded on the loading threads
            SzPlace pl;
            std::unique_ptr< Lzma2Parallel > par;
            if ( iThreads > 1 && !iDepth && arc->IsSevenZip() )
            {
                HANDLE h = CreateFileW( sPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
                LARGE_INTEGER li = {};
                if ( h != INVALID_HANDLE_VALUE && GetFileSizeEx( h, &li ) && SevenZipPlace( h, (uint64_t)li.QuadPart, arc->EntryIndex(), arc->Name(), pl ) )
                {
                    par.reset( new Lzma2Parallel( nullptr, pl, pCancel ) );
                    if ( !par->Open( sPath, iThreads ) ) par.reset();
                }
                if ( h != INVALID_HANDLE_VALUE ) CloseHandle( h );
            }
            if ( par )
            {
                sLayers += Format( L"7-Zip (LZMA2 on %d threads) > ", par->Decoders() );
                iLayers++;
                par->Keep( arc->TakeParent() );
                pProgress = par.get();
                layer = std::move( par );
            }
            else
            {
                sLayers += arc->Kind();
                iLayers += std::max( 1, arc->LayerCount() );
                layer = std::move( arc );
            }
        }
        pHead = layer->Peek( 4, got );
        if ( !pHead ) { sError = layer->sError; return nullptr; }
        if ( got < 4 ) { sError = L"The archive holds an empty file."; return nullptr; }
        if ( IsMidiHead( pHead, got ) ) return std::unique_ptr< Reader >( new StreamReader( std::move( layer ), pFile, pProgress, sLayers, sName, iLayers ) );
        cur = std::move( layer );
    }
    sError = L"The file is nested more deeply than PFAviz++ follows.";
    return nullptr;
}
