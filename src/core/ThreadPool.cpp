#include "core/ThreadPool.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

int Parallel::ThreadCount( int iRequested )
{
    if ( iRequested > 0 ) return iRequested;
    unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 4;
}

void Parallel::SpreadThread( int k )
{
    // The logical processors, each core's first, then each core's second, ...
    static const std::vector< PROCESSOR_NUMBER > s_vOrder = [] {
        std::vector< PROCESSOR_NUMBER > v;
        DWORD dwLen = 0;
        GetLogicalProcessorInformationEx( RelationProcessorCore, nullptr, &dwLen );
        std::vector< uint8_t > vBuf( dwLen );
        auto *pInfo = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)vBuf.data();
        if ( !dwLen || !GetLogicalProcessorInformationEx( RelationProcessorCore, pInfo, &dwLen ) ) return v;
        std::vector< std::vector< PROCESSOR_NUMBER > > vCores;
        for ( DWORD at = 0; at < dwLen; )
        {
            auto *p = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)( vBuf.data() + at );
            std::vector< PROCESSOR_NUMBER > vLogical;
            for ( WORD g = 0; g < p->Processor.GroupCount; g++ )
                for ( BYTE b = 0; b < 64; b++ )
                    if ( p->Processor.GroupMask[g].Mask >> b & 1 )
                    {
                        PROCESSOR_NUMBER pn = {};
                        pn.Group = p->Processor.GroupMask[g].Group;
                        pn.Number = b;
                        vLogical.push_back( pn );
                    }
            if ( !vLogical.empty() ) vCores.push_back( vLogical );
            at += p->Size;
        }
        for ( size_t r = 0;; r++ )
        {
            size_t n = v.size();
            for ( const auto &c : vCores ) if ( r < c.size() ) v.push_back( c[r] );
            if ( v.size() == n ) break;
        }
        return v;
    }();
    if ( k < 0 || s_vOrder.empty() ) return;
    PROCESSOR_NUMBER pn = s_vOrder[(size_t)k % s_vOrder.size()];
    SetThreadIdealProcessorEx( GetCurrentThread(), &pn, nullptr );
}

namespace
{
    // Threads kept between calls: starting and joining a dozen threads costs about 1 ms on Windows,
    // which a frame cannot spare. One pool serves one For at a time; a concurrent caller (the LOD
    // builder while a frame draws, or a nested For) takes the next free pool, or threads of its own.
    struct Pool
    {
        std::mutex mxUse;                   // held by the For using this pool
        std::mutex mx;
        std::condition_variable cv, cvDone;
        std::vector< std::thread > vThreads;
        unsigned long long llGen = 0;
        const std::function< void( size_t, int ) > *pFn = nullptr;
        size_t iCount = 0;
        std::atomic< size_t > iNext{ 0 };
        int iWant = 0, iPending = 0;

        void Work( int iThread )
        {
            for ( size_t i; ( i = iNext.fetch_add( 1, std::memory_order_relaxed ) ) < iCount; )
                ( *pFn )( i, iThread );
        }
        // llSeen: the job count when the thread was made, so it never takes a finished job for a new one
        void Worker( int w, unsigned long long llSeen )
        {
            std::unique_lock< std::mutex > lock( mx );
            for ( ;; )
            {
                cv.wait( lock, [&] { return llGen != llSeen; } );
                llSeen = llGen;
                if ( w >= iWant ) continue;
                lock.unlock();
                Work( w + 1 );
                lock.lock();
                if ( --iPending == 0 ) cvDone.notify_one();
            }
        }
        void Run( int iThreads, size_t iItems, const std::function< void( size_t, int ) > &fn )
        {
            while ( (int)vThreads.size() < iThreads - 1 )
            {
                int w = (int)vThreads.size();
                unsigned long long llNow;
                { std::lock_guard< std::mutex > lock( mx ); llNow = llGen; }
                vThreads.emplace_back( [this, w, llNow] {
                    Parallel::SpreadThread( w + 1 );
                    Worker( w, llNow );
                } );
                vThreads.back().detach();   // never joined: the pools live until the process exits
            }
            {
                std::lock_guard< std::mutex > lock( mx );
                pFn = &fn;
                iCount = iItems;
                iNext.store( 0, std::memory_order_relaxed );
                iWant = iPending = iThreads - 1;
                llGen++;
            }
            cv.notify_all();
            Work( 0 );
            std::unique_lock< std::mutex > lock( mx );
            cvDone.wait( lock, [&] { return iPending == 0; } );
        }
    };
    Pool *g_aPools[2] = { new Pool, new Pool };
}

void Parallel::For( int iThreads, size_t iCount, const std::function< void( size_t, int ) > &fn )
{
    if ( iThreads <= 1 || iCount <= 1 )
    {
        for ( size_t i = 0; i < iCount; i++ ) fn( i, 0 );
        return;
    }
    iThreads = (int)std::min< size_t >( iThreads, iCount );
    // The caller works as thread 0 (once per thread)
    static thread_local bool s_bSpread = false;
    if ( !s_bSpread )
    {
        s_bSpread = true;
        SpreadThread( 0 );
    }
    for ( Pool *p : g_aPools )
        if ( p->mxUse.try_lock() )
        {
            p->Run( iThreads, iCount, fn );
            p->mxUse.unlock();
            return;
        }
    std::atomic< size_t > iNext{ 0 };
    auto Work = [&]( int iThread ) {
        for ( size_t i; ( i = iNext.fetch_add( 1, std::memory_order_relaxed ) ) < iCount; )
            fn( i, iThread );
    };
    std::vector< std::thread > vThreads;
    vThreads.reserve( iThreads - 1 );
    for ( int t = 1; t < iThreads; t++ )
        vThreads.emplace_back( [&Work, t] {
            Parallel::SpreadThread( t );
            Work( t );
        } );
    Work( 0 );
    for ( auto &t : vThreads ) t.join();
}
