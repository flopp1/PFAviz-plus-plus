// A growable array of 64-bit words that never moves: address space is reserved up front and pages
// are committed as it grows, so growing costs no copy and no slack beyond the last page
// (the visual store's bit arenas, where a vector's 1.5x growth cost gigabytes on huge files).
// Committed memory is zeroed by the system.
#pragma once

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

class WordArena
{
public:
    WordArena() = default;
    ~WordArena() { Release(); }
    WordArena( const WordArena & ) = delete;
    WordArena &operator=( const WordArena & ) = delete;
    WordArena( WordArena &&o ) noexcept { Swap( o ); }
    WordArena &operator=( WordArena &&o ) noexcept { if ( this != &o ) { Release(); Swap( o ); } return *this; }

    // Reserves room for at least iWords (call before the first Resize to set the reservation)
    void Reserve( size_t iWords )
    {
        if ( m_p || !iWords ) return;
        m_iReserved = ( iWords * 8 + ReserveGranule - 1 ) / ReserveGranule * ReserveGranule;
        m_p = static_cast< uint64_t * >( VirtualAlloc( nullptr, m_iReserved, MEM_RESERVE, PAGE_READWRITE ) );
        if ( !m_p ) m_iReserved = 0;
    }

    // Grows (new words are zero) or shrinks (pages past the end are released). bAhead: growing,
    // commit extra room so the next growth needs no system call; Resize to a final size passes false.
    void Resize( size_t iWords, bool bAhead = false )
    {
        size_t iBytes = ( iWords * 8 + Granule - 1 ) / Granule * Granule;
        if ( iBytes > m_iReserved )
        {
            // Past the reservation: move to a bigger one (rare; the reservation is a generous bound)
            WordArena bigger;
            bigger.Reserve( std::max( iWords, m_iReserved / 4 ) );
            bigger.Resize( iWords );
            if ( m_iCommitted > m_iDropped )
                memcpy( reinterpret_cast< uint8_t * >( bigger.m_p ) + m_iDropped, reinterpret_cast< uint8_t * >( m_p ) + m_iDropped,
                        std::min( m_iCommitted, bigger.m_iCommitted ) - m_iDropped );
            size_t iDropped = m_iDropped;
            *this = std::move( bigger );
            m_iSize = iWords;
            DecommitBefore( iDropped / 8 );
            return;
        }
        if ( iBytes > m_iCommitted )
        {
            // Grow ahead (1 MB or an eighth, within the reservation) so a growing arena commits
            // rarely; Resize to the final size trims it back
            size_t iTarget = bAhead ? std::min( m_iReserved, std::max( iBytes, m_iCommitted + std::max< size_t >( 1u << 20, m_iCommitted / 8 ) ) ) : iBytes;
            if ( !VirtualAlloc( reinterpret_cast< uint8_t * >( m_p ) + m_iCommitted, iTarget - m_iCommitted, MEM_COMMIT, PAGE_READWRITE ) ) throw std::bad_alloc();
            m_iCommitted = iTarget;
        }
        else if ( iBytes < m_iCommitted )
        {
            size_t iFrom = std::max( iBytes, m_iDropped );
            if ( iFrom < m_iCommitted ) VirtualFree( reinterpret_cast< uint8_t * >( m_p ) + iFrom, m_iCommitted - iFrom, MEM_DECOMMIT );
            m_iCommitted = iBytes;
            m_iDropped = std::min( m_iDropped, m_iCommitted );
        }
        m_iSize = iWords;
    }

    // Gives the whole pages below word iWord back to the system (a sliding window: the words there
    // are never read again; the size and the addresses of the other words stay as they are)
    void DecommitBefore( size_t iWord )
    {
        size_t iBytes = std::min( iWord * 8 / Granule * Granule, m_iCommitted );
        if ( iBytes <= m_iDropped ) return;
        VirtualFree( reinterpret_cast< uint8_t * >( m_p ) + m_iDropped, iBytes - m_iDropped, MEM_DECOMMIT );
        m_iDropped = iBytes;
    }

    size_t size() const { return m_iSize; }
    size_t CommittedBytes() const { return m_iCommitted - m_iDropped; }
    uint64_t *data() { return m_p; }
    const uint64_t *data() const { return m_p; }
    uint64_t &operator[]( size_t i ) { return m_p[i]; }
    const uint64_t &operator[]( size_t i ) const { return m_p[i]; }

private:
    static const size_t Granule = 4096;             // commit: one page
    static const size_t ReserveGranule = 65536;     // reserve: the allocation granularity

    void Release()
    {
        if ( m_p ) VirtualFree( m_p, 0, MEM_RELEASE );
        m_p = nullptr;
        m_iReserved = m_iCommitted = m_iSize = m_iDropped = 0;
    }
    void Swap( WordArena &o )
    {
        std::swap( m_p, o.m_p );
        std::swap( m_iReserved, o.m_iReserved );
        std::swap( m_iCommitted, o.m_iCommitted );
        std::swap( m_iSize, o.m_iSize );
        std::swap( m_iDropped, o.m_iDropped );
    }

    uint64_t *m_p = nullptr;
    size_t m_iReserved = 0, m_iCommitted = 0, m_iSize = 0;
    size_t m_iDropped = 0;                          // bytes at the start given back (DecommitBefore)
};
