#pragma once
// exp/quick-dirty (EXPERIMENT.md): how long the game thread waits on the VU1/GIF and GS worker threads (back-pressure),
// for the frame-dip log (RC_EXP_DIPS). Each wait site adds its time only on the game thread.
#include <atomic>
#include <chrono>
#include <cstdint>

namespace ps2x::expstats
{
    enum Site : int
    {
        VifQueue,  // the VU1 worker's job queue full (asyncvif::enqueue)
        VifSync,   // waiting for the VU1 worker and GIF stage to finish (asyncvif::sync / syncVu): register reads, VU memory
        GsQueue,   // the GS thread's queue full (GSThreadedBackend::PublishLocked)
        GsDrain,   // waiting for the GS thread to finish (GSThreadedBackend::Drain)
        kSites
    };
    extern std::atomic<uint64_t> g_ns[kSites];
    extern std::atomic<uint64_t> g_count[kSites];
    extern thread_local bool t_gameThread;
    // Waits of more than 0.5 ms on the game thread: their call stacks (as offsets into the executable) are counted;
    // dumpSlowStacks prints the heaviest (addr2line -f -C -e <binary> <offsets> names them) and starts over.
    void recordSlow(Site site, uint64_t ns);
    void dumpSlowStacks(int top);

    struct Wait
    {
        Site site;
        std::chrono::steady_clock::time_point start;
        bool on;
        explicit Wait(Site s) : site(s), on(t_gameThread)
        {
            if (on)
                start = std::chrono::steady_clock::now();
        }
        ~Wait()
        {
            if (!on)
                return;
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            g_ns[site].fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
            if (ns > 20000)
                g_count[site].fetch_add(1u, std::memory_order_relaxed);
            if (ns > 500000)
                recordSlow(site, static_cast<uint64_t>(ns));
        }
    };
}
