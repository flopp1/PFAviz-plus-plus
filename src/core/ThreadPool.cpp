#include "core/ThreadPool.h"

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
                vThreads.emplace_back( [this, w, llNow] { Worker( w, llNow ); } );
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
    for ( int t = 1; t < iThreads; t++ ) vThreads.emplace_back( Work, t );
    Work( 0 );
    for ( auto &t : vThreads ) t.join();
}
