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
}
