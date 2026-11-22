// The reader's thread: see kick_queue.h and RENDERER_P2_DESIGN.md section 8.
#include "kick_queue.h"

#include "../record/chain_walk.h"

#include <pthread.h>
#include <xmmintrin.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace rc::live
{
    using namespace rc::rec;

    namespace
    {
        using Clock = std::chrono::steady_clock;
        double nsSince(Clock::time_point t0)
        {
            return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        }
        constexpr uint32_t kNone = ~0u;
    }

    KickQueue::KickQueue(Consumer &downstream, const Options &options) : m_down(downstream), m_opt(options)
    {
        m_image.reset();
        m_thread = std::thread(&KickQueue::run, this);
    }

    KickQueue::~KickQueue() { stop(); }

    KickQueue::Batch &KickQueue::current()
    {
        if (!m_current)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_free.empty())
            {
                m_current = std::move(m_free.back());
                m_free.pop_back();
            }
        }
        if (!m_current)
            m_current = std::make_unique<Batch>();
        return *m_current;
    }

    void KickQueue::submit()
    {
        if (!m_current || m_current->events.empty())
            return;
        m_closedWaiting += m_current->closes;
        m_queue.push_back(std::move(m_current));
        m_work.notify_one();
    }

    // --- EE side ---------------------------------------------------------------------------------------------------
    void KickQueue::open(const FileInfo &info)
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        Batch &b = current();
        Event e;
        e.kind = Event::Open;
        e.index = static_cast<uint32_t>(b.infos.size());
        b.infos.push_back(info);
        b.events.push_back(e);
    }

    void KickQueue::keyframe(const Keyframe &kf)
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        const Clock::time_point t0 = Clock::now();
        Batch &b = current();
        KeyData d;
        d.header = kf.header;
        for (const StateChunk &c : kf.chunks)
            d.chunks.emplace_back(c.id, std::vector<uint8_t>(c.data, c.data + c.bytes));
        Event e;
        e.kind = Event::Keyframe;
        e.index = static_cast<uint32_t>(b.keys.size());
        b.keys.push_back(std::move(d));
        b.events.push_back(e);
        m_cycleEeNs += nsSince(t0);
    }

    void KickQueue::cycleBegin(uint64_t cycle)
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        Event e;
        e.kind = Event::CycleBegin;
        e.value = cycle;
        current().events.push_back(e);
    }

    void KickQueue::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa)
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        Event e;
        e.kind = Event::Hw;
        e.a = address;
        e.b = width;
        e.c = callerRa;
        e.value = value;
        current().events.push_back(e);
        ++m_cyc.events;
    }

    void KickQueue::crt(int interlaced, int mode, int ffmd)
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        Event e;
        e.kind = Event::Crt;
        e.a = static_cast<uint32_t>(interlaced);
        e.b = static_cast<uint32_t>(mode);
        e.c = static_cast<uint32_t>(ffmd);
        current().events.push_back(e);
    }

    void KickQueue::copyKick(KickData &d, const KickView &k, const RamView &ram)
    {
        d.view = k;
        d.spans.clear();
        d.tags.clear();
        d.tagBytes.clear();
        d.labels.clear();
        d.runs.clear();
        d.pages.clear();
        // Spans and labels: their tags copied, with the payloads; the pointers are set on the reader thread (copies of
        // the same tag are kept apart: a span's and a label's tag are copied once each). TagView::payload holds the
        // payload's offset into tagBytes until then, TagView pointers in spans and labels the tag's index.
        const auto copyTag = [&d](const TagView *t) -> uint32_t
        {
            if (!t)
                return kNone;
            TagView c = *t;
            const size_t at = d.tagBytes.size();
            if (t->payload && t->payloadBytes)
                d.tagBytes.insert(d.tagBytes.end(), static_cast<const uint8_t *>(t->payload), static_cast<const uint8_t *>(t->payload) + t->payloadBytes);
            c.payload = reinterpret_cast<const void *>(static_cast<uintptr_t>(t->payload ? at : ~static_cast<uintptr_t>(0)));
            d.tags.push_back(c);
            return static_cast<uint32_t>(d.tags.size() - 1u);
        };
        for (uint32_t i = 0; i < k.spanCount; ++i)
        {
            Span s = k.spans[i];
            s.tag = reinterpret_cast<const TagView *>(static_cast<uintptr_t>(copyTag(k.spans[i].tag)));
            d.spans.push_back(s);
        }
        for (uint32_t i = 0; i < k.labelCount; ++i)
            d.labels.push_back(reinterpret_cast<const TagView *>(static_cast<uintptr_t>(copyTag(k.labels[i]))));
        d.hasRegs = k.regs != nullptr;
        if (d.hasRegs)
            std::memcpy(d.regs, k.regs, sizeof(d.regs));
        // The page shadow: every page the kick's stream lies on, new or changed since its last capture.
        struct Touch final : WalkVisitor
        {
            PageTracker *tracker = nullptr;
            void range(const WalkRange &r) override { tracker->touch(r.physAddr, r.bytes, r.scratch); }
        } touch;
        touch.tracker = &m_tracker;
        m_tracker.beginKick();
        walkChain(k, ram, &touch, false);
        const uint64_t before = m_tracker.pagesCompared();
        const uint32_t copied = m_tracker.collect(ram.ram, ram.scratchpad, [&d](uint32_t first, uint32_t count, const uint8_t *data)
                                                  {
                                                      d.runs.push_back({first, count, d.pages.size()});
                                                      d.pages.insert(d.pages.end(), data, data + static_cast<size_t>(count) * kPageBytes);
                                                  });
        m_cyc.pagesCompared += m_tracker.pagesCompared() - before;
        m_cyc.pagesCopied += copied;
    }

    void KickQueue::kick(const KickView &k, const RamView &ram)
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        const Clock::time_point t0 = Clock::now();
        Batch &b = current();
        if (b.kicks.size() <= b.kickCount)
            b.kicks.emplace_back();
        KickData &d = b.kicks[b.kickCount];
        copyKick(d, k, ram);
        Event e;
        e.kind = Event::Kick;
        e.index = b.kickCount++;
        b.events.push_back(e);
        ++m_cyc.kicks;
        if (m_opt.testUnlockedKick)
            submit(); // the planted fault: no lock (the thread sanitizer must report it)
        else
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            submit();
        }
        m_cycleEeNs += nsSince(t0);
    }

    void KickQueue::cycleEnd(const CycleInfo &info, const uint8_t *)
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        const Clock::time_point t0 = Clock::now();
        Batch &b = current();
        Event e;
        e.kind = Event::CycleEnd;
        e.info = info;
        b.events.push_back(e);
        ++b.closes;
        m_cycleEeNs += nsSince(t0);
        m_cyc.cycles = 1u;
        m_cyc.eeNs = m_cyc.maxCycleEeNs = m_cycleEeNs;
        const auto fold = [](Stats &to, const Stats &c)
        {
            to.cycles += c.cycles;
            to.kicks += c.kicks;
            to.events += c.events;
            to.pagesCompared += c.pagesCompared;
            to.pagesCopied += c.pagesCopied;
            to.eeNs += c.eeNs;
            to.maxCycleEeNs = std::max(to.maxCycleEeNs, c.maxCycleEeNs);
        };
        fold(m_window, m_cyc);
        Stats report;
        bool reportNow = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            submit();
            fold(m_stats, m_cyc);
            m_window.maxClosedWaiting = std::max(m_window.maxClosedWaiting, m_closedWaiting);
            m_stats.maxClosedWaiting = std::max(m_stats.maxClosedWaiting, m_closedWaiting);
            if (m_closedWaiting > m_opt.maxClosed && !m_exit)
            {
                // 8.3: the reader is more than maxClosed cycles behind; the game waits until it has taken one.
                const Clock::time_point w0 = Clock::now();
                m_done.wait(lock, [this] { return m_closedWaiting <= m_opt.maxClosed || m_exit; });
                const double ns = nsSince(w0);
                ++m_stats.waits;
                m_stats.waitNs += ns;
                m_stats.maxWaitNs = std::max(m_stats.maxWaitNs, ns);
                ++m_window.waits;
                m_window.waitNs += ns;
                m_window.maxWaitNs = std::max(m_window.maxWaitNs, ns);
            }
            if (m_opt.reportEvery && m_window.cycles >= m_opt.reportEvery)
            {
                report = m_window;
                report.readerCycles = m_stats.readerCycles - m_windowReaderCyclesAtStart;
                report.readerNs = m_stats.readerNs - m_windowReaderNsAtStart;
                report.applyNs = m_stats.applyNs - m_windowApplyNsAtStart;
                m_windowReaderCyclesAtStart = m_stats.readerCycles;
                m_windowReaderNsAtStart = m_stats.readerNs;
                m_windowApplyNsAtStart = m_stats.applyNs;
                reportNow = true;
            }
        }
        m_cycleEeNs = 0.0;
        m_cyc = Stats{};
        if (reportNow)
        {
            const Stats &w = report;
            const char *who = m_opt.live ? "the EE thread" : "the feeding thread";
            std::fprintf(stderr,
                         "[vk-native] reader thread, cycles %llu-%llu: %s %.2f ms a cycle (largest %.2f; %.1f MB of pages compared, %.2f MB "
                         "copied a cycle); the reader thread %.2f ms a cycle (pages %.2f); %llu waits (%.1f ms, largest %.1f); at most %u closed "
                         "cycles waiting\n",
                         static_cast<unsigned long long>(m_windowFirst), static_cast<unsigned long long>(info.end.cycle), who, w.eeNs / w.cycles / 1e6,
                         w.maxCycleEeNs / 1e6, static_cast<double>(w.pagesCompared) * kPageBytes / w.cycles / 1048576.0,
                         static_cast<double>(w.pagesCopied) * kPageBytes / w.cycles / 1048576.0, w.readerCycles ? w.readerNs / w.readerCycles / 1e6 : 0.0,
                         w.readerCycles ? w.applyNs / w.readerCycles / 1e6 : 0.0, static_cast<unsigned long long>(w.waits), w.waitNs / 1e6,
                         w.maxWaitNs / 1e6, w.maxClosedWaiting);
            m_window = Stats{};
            m_windowFirst = info.end.cycle + 1u;
        }
    }

    void KickQueue::drain()
    {
        if (m_stopped.load(std::memory_order_relaxed))
            return;
        std::unique_lock<std::mutex> lock(m_mutex);
        submit();
        m_done.wait(lock, [this] { return (m_queue.empty() && !m_busy) || m_exit; });
    }

    void KickQueue::stop()
    {
        if (m_stopped.exchange(true))
            return;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // Events the EE thread had not handed over yet are dropped with the queue's later ones.
            m_done.wait(lock, [this] { return m_queue.empty() && !m_busy; });
            m_exit = true;
        }
        m_work.notify_all();
        m_done.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    KickQueue::Stats KickQueue::stats()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    // --- Reader thread ---------------------------------------------------------------------------------------------
    void KickQueue::run()
    {
        pthread_setname_np(pthread_self(), "rc-reader"); // (for top -H, perf --per-thread)
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;)
        {
            m_work.wait(lock, [this] { return !m_queue.empty() || m_exit; });
            if (m_queue.empty())
                break;
            std::unique_ptr<Batch> b = std::move(m_queue.front());
            m_queue.pop_front();
            m_busy = true;
            lock.unlock();
            handle(*b);
            lock.lock();
            m_closedWaiting -= std::min(m_closedWaiting, b->closes);
            m_busy = false;
            b->clear();
            m_free.push_back(std::move(b));
            m_done.notify_all();
        }
    }

    void KickQueue::handle(Batch &b)
    {
        double readerNs = 0.0, applyNs = 0.0, maxCycle = 0.0;
        uint64_t cycles = 0;
        for (const Event &e : b.events)
        {
            const Clock::time_point t0 = Clock::now();
            switch (e.kind)
            {
            case Event::Open:
                m_down.open(b.infos[e.index]);
                break;
            case Event::Keyframe:
            {
                KeyData &d = b.keys[e.index];
                Keyframe kf;
                kf.header = d.header;
                for (auto &[id, data] : d.chunks)
                    kf.chunks.push_back({id, data.data(), data.size()});
                m_down.keyframe(kf);
                break;
            }
            case Event::CycleBegin:
                m_down.cycleBegin(e.value);
                break;
            case Event::Hw:
                m_down.hw(e.a, e.b, e.value, e.c);
                break;
            case Event::Crt:
                m_down.crt(static_cast<int>(e.a), static_cast<int>(e.b), static_cast<int>(e.c));
                break;
            case Event::Kick:
            {
                KickData &d = b.kicks[e.index];
                for (const PageRun &r : d.runs)
                    m_image.apply(r.first, r.count, d.pages.data() + r.offset);
                applyNs += nsSince(t0);
                for (TagView &t : d.tags)
                {
                    const uintptr_t at = reinterpret_cast<uintptr_t>(t.payload);
                    t.payload = at == ~static_cast<uintptr_t>(0) ? nullptr : d.tagBytes.data() + at;
                }
                for (Span &s : d.spans)
                {
                    const uint32_t i = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.tag));
                    s.tag = i == kNone ? nullptr : &d.tags[i];
                }
                for (const TagView *&l : d.labels)
                {
                    const uint32_t i = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(l));
                    l = i == kNone ? nullptr : &d.tags[i];
                }
                KickView k = d.view;
                k.spans = d.spans.data();
                k.spanCount = static_cast<uint32_t>(d.spans.size());
                k.labels = d.labels.data();
                k.labelCount = static_cast<uint32_t>(d.labels.size());
                k.regs = d.hasRegs ? d.regs : nullptr;
                // The reader runs under the EE thread's MXCSR at the kick, as it did on the EE thread.
                _mm_setcsr(k.mxcsr);
                m_down.kick(k, m_image.view());
                break;
            }
            case Event::CycleEnd:
                if (m_opt.testDelayMs)
                    std::this_thread::sleep_for(std::chrono::milliseconds(m_opt.testDelayMs));
                m_down.cycleEnd(e.info, nullptr);
                break;
            }
            const double ns = nsSince(t0);
            m_cycleReaderNs += ns;
            readerNs += ns;
            if (e.kind == Event::CycleEnd)
            {
                maxCycle = std::max(maxCycle, m_cycleReaderNs);
                m_cycleReaderNs = 0.0;
                ++cycles;
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.readerNs += readerNs;
        m_stats.applyNs += applyNs;
        m_stats.readerCycles += cycles;
        m_stats.maxCycleReaderNs = std::max(m_stats.maxCycleReaderNs, maxCycle);
    }
}
