#pragma once

// The reader's thread (renderer plan P2 step 9 batch 6; RENDERER_P2_DESIGN.md section 8).
//
// KickQueue is a live consumer in the capture's list (EE thread). Per event it appends a job and returns:
//   - hardware stores, CRT changes, cycle begin and close: copied as they are;
//   - a kick: the KickView with its spans, their tags and its labels (and the GS register file when the capture gives it),
//     and the pages its stream touches that are new or changed since their last capture, from a PageTracker of its own
//     (one walkChain over the kick with a visitor that touches the ranges, then collect: the frame record's page shadow,
//     so the queue is a dump that is never written);
//   - a keyframe (after a save-state load): copied.
// A reader thread owns an rc::rec::RamImage, applies each kick's pages to it and calls the downstream consumer (the live
// rc::live::Reader) with the image's RamView, so by the page shadow's guarantee it sees every byte of the kick's stream
// as the EE thread saw it at the kick, and makes the same lists. It sets its MXCSR to the kick's before each kick.
//
// 8.3's wait: when more than `maxClosed` (2) closed cycles are waiting for the reader, the EE thread waits at the cycle
// close until one is taken. Waits and their time are counted. drain() (the capture, before it closes a recording) waits
// until the reader has handled everything; stop() drains, ends the thread and drops later events.
//
// Nothing flows back to the game. RC_VK_READER_THREAD=0 does without the queue (the reader on the EE thread, as in P1).

#include "../record/consumer.h"
#include "../record/page_tracker.h"
#include "../record/ram_image.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rc::live
{
    class KickQueue final : public rc::rec::Consumer
    {
    public:
        struct Options
        {
            uint32_t maxClosed = 2;   // 8.3: closed cycles the reader may be behind before the EE thread waits
            uint64_t reportEvery = 0; // a line on stderr every n cycles (0: none)
            bool live = true;         // the report's wording: "the EE thread" (live) or "the feeding thread" (rc_replay)
        };
        struct Stats
        {
            uint64_t cycles = 0, kicks = 0, events = 0, pagesCompared = 0, pagesCopied = 0;
            double eeNs = 0.0, maxCycleEeNs = 0.0;      // the EE thread's share: the queue's own time in its hooks
            uint64_t waits = 0;                          // cycle closes that waited (8.3)
            double waitNs = 0.0, maxWaitNs = 0.0;
            uint32_t maxClosedWaiting = 0;               // the most closed cycles waiting at a cycle close
            double readerNs = 0.0, maxCycleReaderNs = 0.0; // the reader thread's time (page application + the consumer)
            double applyNs = 0.0;                        // of it, the pages into the image
            uint64_t readerCycles = 0;
        };

        KickQueue(rc::rec::Consumer &downstream, const Options &options);
        ~KickQueue() override;
        KickQueue(const KickQueue &) = delete;
        KickQueue &operator=(const KickQueue &) = delete;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void crt(int interlaced, int mode, int ffmd) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;
        void close() override { drain(); }
        void drain() override;
        // Drains, ends the reader thread; events after it are dropped. Any thread; idempotent.
        void stop();
        // A consistent copy (under the queue's lock): the EE side's figures as of the last cycle close, the reader's as of
        // the last cycle it finished.
        Stats stats();

    private:
        struct Event
        {
            enum Kind : uint8_t { Open, Keyframe, CycleBegin, Hw, Crt, Kick, CycleEnd } kind = Hw;
            uint32_t a = 0, b = 0, c = 0; // hw: address, width, callerRa; crt: interlaced, mode, ffmd
            uint64_t value = 0;           // hw value; cycleBegin cycle
            uint32_t index = 0;           // Kick: into Batch::kicks; Keyframe: into Batch::keys; Open: info
            rc::rec::CycleInfo info{};
        };
        struct PageRun
        {
            uint32_t first, count;
            size_t offset; // into KickData::pages
        };
        struct KickData
        {
            rc::rec::KickView view{};
            std::vector<rc::rec::Span> spans;
            std::vector<rc::rec::TagView> tags;   // the spans' tags, then the labels'
            std::vector<uint8_t> tagBytes;        // their payloads
            std::vector<const rc::rec::TagView *> labels;
            uint64_t regs[rc::rec::kGsRegCount] = {};
            bool hasRegs = false;
            std::vector<PageRun> runs;
            std::vector<uint8_t> pages;
        };
        struct KeyData
        {
            rc::rec::KeyframeHeader header{};
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> chunks;
        };
        struct Batch
        {
            std::vector<Event> events;
            std::vector<KickData> kicks; // [0, kickCount) in use; the rest keep their capacity
            uint32_t kickCount = 0;
            std::vector<KeyData> keys;
            std::vector<rc::rec::FileInfo> infos;
            uint32_t closes = 0; // CycleEnd events in it
            void clear()
            {
                events.clear();
                kickCount = 0;
                keys.clear();
                infos.clear();
                closes = 0;
            }
        };

        // EE side.
        Batch &current();
        void submit(); // the current batch to the reader (under the lock)
        void copyKick(KickData &d, const rc::rec::KickView &k, const rc::rec::RamView &ram);
        // Reader side.
        void run();
        void handle(Batch &b);

        rc::rec::Consumer &m_down;
        Options m_opt;
        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_work, m_done;
        std::deque<std::unique_ptr<Batch>> m_queue; // submitted, oldest first
        std::vector<std::unique_ptr<Batch>> m_free;
        bool m_busy = false, m_exit = false;
        uint32_t m_closedWaiting = 0; // closed cycles submitted and not yet handled
        std::atomic<bool> m_stopped{false};
        Stats m_stats; // under m_mutex for the reader's and the copies; the EE side's are written by the EE thread under it

        // EE thread only.
        std::unique_ptr<Batch> m_current;
        rc::rec::PageTracker m_tracker;
        double m_cycleEeNs = 0.0;
        Stats m_cyc;    // EE side of the cycle in progress (kicks, events, pages)
        Stats m_window; // EE side of the report window
        uint64_t m_windowFirst = 0, m_windowReaderCyclesAtStart = 0;
        double m_windowReaderNsAtStart = 0.0, m_windowApplyNsAtStart = 0.0;

        // Reader thread only.
        rc::rec::RamImage m_image;
        double m_cycleReaderNs = 0.0;
    };
}
