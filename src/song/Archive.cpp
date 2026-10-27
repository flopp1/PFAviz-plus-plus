#include "song/Archive.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <vector>

#define LIBARCHIVE_STATIC
#include <archive.h>
#include <archive_entry.h>

#include "core/Util.h"

namespace
{
    const uint8_t SevenZipMagic[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };

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

    bool IsMidiName( const char *s )
    {
        if ( !s ) return false;
        size_t n = strlen( s );
        return ( n >= 4 && _stricmp( s + n - 4, ".mid" ) == 0 ) || ( n >= 5 && _stricmp( s + n - 5, ".midi" ) == 0 );
    }

    // One layer: libarchive reading the stream around it through callbacks (stacked filters, such as
    // .xz.xz, all at once), on the entry to take: the first named .mid/.midi, else the largest
    class ArchiveStream : public Stream
    {
    public:
        explicit ArchiveStream( const std::atomic< bool > *pCancel ) : m_pCancel( pCancel ) {}
        ~ArchiveStream() { if ( m_a ) archive_read_free( m_a ); }

        // bSeek: the layer may seek in its parent (a 7z needs to)
        bool Open( std::unique_ptr< Stream > pParent, bool bSeek, bool bOuter, std::wstring &sErr )
        {
            m_pParent = std::move( pParent );
            m_bSeek = bSeek;
            if ( !Start() ) { sErr = Error( L"The file could not be opened as an archive or compressed file" ); return false; }
            archive_entry *e = nullptr;
            int i = 0, iBest = -1, iRet;
            int64_t llBest = -1;
            std::string sBest;
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
                    if ( bRaw || IsMidiName( s ) )
                    {
                        if ( !bRaw ) m_sName = s;
                        m_iEntry = i;
                        m_llSize = ll;
                        return Check( bOuter, sErr );
                    }
                    if ( ll > llBest ) { llBest = ll; iBest = i; sBest = s ? s : ""; }
                }
                if ( archive_read_data_skip( m_a ) == ARCHIVE_FATAL ) break;
                i++;
            }
            if ( Cancelled( m_pCancel ) ) { sErr = L"Cancelled."; return false; }
            if ( iRet == ARCHIVE_FATAL || iRet == ARCHIVE_WARN ) { sErr = Error( L"The archive could not be read" ); return false; }
            if ( iBest < 0 ) { sErr = L"The archive holds no file."; return false; }
            m_iEntry = iBest;
            m_sName = sBest;
            m_llSize = llBest;
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

    // The MIDI's chunks from the innermost stream
    class StreamReader : public Archive::Reader
    {
    public:
        StreamReader( std::unique_ptr< Stream > p, FileStream *pFile, std::wstring sLayers, std::wstring sName )
            : m_p( std::move( p ) ), m_pFile( pFile ), m_sLayers( std::move( sLayers ) ), m_sName( std::move( sName ) ) {}
        bool NextChunk( char aType[4], uint64_t &llLen ) override
        {
            if ( m_llLeft && !m_p->Skip( m_llLeft ) ) { sError = m_p->sError; return false; }
            m_llLeft = 0;
            uint8_t a[8];
            size_t got = 0;
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
        uint64_t Done() const override { return m_pFile->Pos(); }
        uint64_t Total() const override { return (uint64_t)m_pFile->Size(); }
        std::wstring Summary() const override
        {
            return m_sLayers + ( m_sName.empty() ? L"MIDI" : m_sName ) + L", " + Bytes( (double)m_p->Pos() );
        }

    private:
        std::unique_ptr< Stream > m_p;
        FileStream *m_pFile;
        std::wstring m_sLayers, m_sName;
        uint64_t m_llLeft = 0;      // the current chunk's unread data
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

std::unique_ptr< Archive::Reader > Archive::Open( const std::wstring &sPath, const std::atomic< bool > *pCancel, std::wstring &sError )
{
    std::unique_ptr< FileStream > file( new FileStream( pCancel ) );
    if ( !file->Open( sPath, sError ) ) return nullptr;
    FileStream *pFile = file.get();
    std::unique_ptr< Stream > cur = std::move( file );
    std::wstring sLayers;
    for ( int iDepth = 0; iDepth < MaxDepth; iDepth++ )
    {
        // Seeking costs nothing in the file itself; inside another layer only a 7z needs it (going
        // back reads that layer again from its start)
        size_t got = 0;
        const uint8_t *pHead = cur->Peek( 6, got );
        if ( !pHead ) { sError = cur->sError; return nullptr; }
        bool bSeek = !iDepth || ( got == 6 && memcmp( pHead, SevenZipMagic, 6 ) == 0 );
        std::unique_ptr< ArchiveStream > layer( new ArchiveStream( pCancel ) );
        if ( !layer->Open( std::move( cur ), bSeek, !iDepth, sError ) ) return nullptr;
        sLayers += layer->Kind();
        pHead = layer->Peek( 4, got );
        if ( !pHead ) { sError = layer->sError; return nullptr; }
        if ( got < 4 ) { sError = L"The archive holds an empty file."; return nullptr; }
        if ( IsMidiHead( pHead, got ) )
        {
            std::wstring sName = Util::Utf8ToWide( layer->Name() );
            return std::unique_ptr< Reader >( new StreamReader( std::move( layer ), pFile, sLayers, sName ) );
        }
        cur = std::move( layer );
    }
    sError = L"The file is nested more deeply than PFAviz++ follows.";
    return nullptr;
}
