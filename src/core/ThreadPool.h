// Minimal parallel-for: dynamic scheduling of items over N threads, kept in pools between calls.
#pragma once

#include <cstddef>
#include <functional>

namespace Parallel
{
    // Number of worker threads to use: iRequested > 0 as given, otherwise the logical core count
    int ThreadCount( int iRequested );

    // Calls fn( item, threadIndex ) for every item in [0, iCount), items claimed dynamically.
    // Runs inline when iThreads <= 1 or iCount <= 1.
    void For( int iThreads, size_t iCount, const std::function< void( size_t, int ) > &fn );

    // Sets the calling thread's ideal processor to the k-th in an order that takes each core's first
    // logical processor before any core's second: Windows otherwise hands a process's new threads
    // consecutive logical processors, and two busy threads on SMT siblings of one core run at about
    // two thirds of their speed. A thread that hands out work (For's caller) takes k = 0 itself.
    void SpreadThread( int k );
}
