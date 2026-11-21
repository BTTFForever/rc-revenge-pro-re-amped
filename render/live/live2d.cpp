#include "live2d.h"

#include "../tex/gs_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rc::live
{
    using namespace rc::rec;
    using namespace rc::rd;

    void appendList(DrawList &to, const DrawList &from, bool persistentOnly)
    {
        for (const DrawCmd &c : from.cmds)
        {
            if (persistentOnly && c.op != kOpTexture && c.op != kOpImageBlit)
                continue;
            DrawCmd &n = to.add(c.op, from.data(c), c.payloadBytes);
            const uint32_t offset = n.payloadOffset;
            n = c;
            n.payloadOffset = offset;
        }
    }

    // --- Mailbox -------------------------------------------------------------------------------------------------
    void Mailbox::push(Published &&p)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_pushed;
        m_queue.push_back(std::move(p));
        while (m_queue.size() > kMaxPending)
        {
            // The presenter is behind (a minimised window, a stall): the oldest list's draws are of a picture nobody will
            // see, but its TEXTURE commands name generations later lists rely on and its uploads stay in the targets.
            Published &oldest = m_queue.front();
            Published &next = m_queue[1];
            DrawList merged;
            appendList(merged, oldest.list, true);
            const uint32_t shift = static_cast<uint32_t>(merged.cmds.size());
            appendList(merged, next.list);
            next.list = std::move(merged);
            for (uint32_t &r : next.k2Ranges)
                r += shift;
            m_queue.pop_front();
            ++m_stripped;
        }
    }

    void Mailbox::take(std::vector<Published> &out)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Published &p : m_queue)
            out.push_back(std::move(p));
        m_queue.clear();
    }

    // --- Reader ----------------------------------------------------------------------------------------------------
    namespace
    {
        FrameReader::Options readerOptions(bool fast)
        {
            FrameReader::Options o;
            o.fast = fast;
            o.gif.cullOffscreen = true;
            o.gif.path1Prims = true;   // (step 8 batch 4) K2 is drawn
            o.gif.frameUploads = true; // uploads into the frame buffers, in order with the draws
            return o;
        }
    }

    Reader::Reader(Mailbox &box, bool live, bool fast) : m_box(box), m_frame(readerOptions(fast)), m_live(live)
    {
        if (live)
            m_frame.startLive();
    }

    void Reader::open(const FileInfo &info)
    {
        m_frame.open(info);
        if (info.header.height)
            m_height = info.header.height;
    }

    void Reader::keyframe(const Keyframe &kf)
    {
        // Live: only a save-state load sends one (the capture's live keyframe). Off-line: the dump's, as the frame
        // reader and FrameBlit take them (the first, and one after a load).
        m_frame.keyframe(kf);
    }

    void Reader::cycleBegin(uint64_t cycle)
    {
        m_cycleNs = 0.0;
        m_frame.cycleBegin(cycle);
    }

    void Reader::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa)
    {
        m_frame.hw(address, width, value, callerRa);
    }

    void Reader::kick(const KickView &k, const RamView &ram)
    {
        const Clock::time_point t0 = Clock::now();
        const uint64_t guarded = m_frame.stats().guardedKicks;
        m_frame.kick(k, ram);
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        m_cycleNs += ns;
        if (k.channel == 1u)
        {
            ++m_stats.path1Kicks;
            ++m_window.path1Kicks;
            m_stats.path1Ns += ns;
            m_window.path1Ns += ns;
            m_stats.maxPath1Ns = std::max(m_stats.maxPath1Ns, ns);
            m_window.maxPath1Ns = std::max(m_window.maxPath1Ns, ns);
            if (m_frame.stats().guardedKicks != guarded)
            {
                ++m_stats.guarded;
                ++m_window.guarded;
                if (m_live && m_stats.guarded <= 5u)
                    std::fprintf(stderr, "[vk-native] cycle %llu: a guard stopped a VIF1 kick: %s\n",
                                 static_cast<unsigned long long>(k.cycle), m_frame.guardText().c_str());
            }
        }
        else if (k.channel == 2u)
        {
            ++m_stats.path3Kicks;
            ++m_window.path3Kicks;
        }
    }

    void Reader::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        const Clock::time_point t0 = Clock::now();
        m_frame.cycleEnd(info, refRgba);
        Published p;
        p.cycle = info.end.cycle;
        p.dispfb2 = info.end.dispfb2;
        p.fbp = static_cast<uint32_t>(info.end.dispfb2 & 0x1FFu);
        p.width = 640u;
        p.height = info.end.refHeight ? info.end.refHeight : m_height;
        p.list = m_frame.list();
        p.k2Ranges = m_frame.path1Ranges();
        if (m_sink)
            m_sink(p.cycle, p.list, static_cast<uint32_t>(p.k2Ranges.size() / 2u));
        m_cycleNs += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        p.readerMs = m_cycleNs / 1e6;
        m_box.push(std::move(p));
        ++m_stats.cycles;
        ++m_window.cycles;
        m_stats.ns += m_cycleNs;
        m_window.ns += m_cycleNs;
        m_stats.maxCycleNs = std::max(m_stats.maxCycleNs, m_cycleNs);
        m_window.maxCycleNs = std::max(m_window.maxCycleNs, m_cycleNs);
        if (m_reportEvery && m_window.cycles >= m_reportEvery)
        {
            const Stats &w = m_window;
            std::fprintf(stderr,
                         "[vk-native] cycles %llu-%llu: reader %.2f ms a cycle on the EE thread (largest %.2f); VIF1 kicks %llu, %.2f ms each "
                         "(largest %.2f); PATH3 kicks %llu; guarded %llu\n",
                         static_cast<unsigned long long>(m_windowFirst), static_cast<unsigned long long>(info.end.cycle), w.ns / w.cycles / 1e6,
                         w.maxCycleNs / 1e6, static_cast<unsigned long long>(w.path1Kicks), w.path1Kicks ? w.path1Ns / w.path1Kicks / 1e6 : 0.0,
                         w.maxPath1Ns / 1e6, static_cast<unsigned long long>(w.path3Kicks), static_cast<unsigned long long>(w.guarded));
            m_window = Stats{};
            m_windowFirst = info.end.cycle + 1u;
        }
    }

    // --- Presenter -------------------------------------------------------------------------------------------------
    Presenter::~Presenter() { destroy(); }

    bool Presenter::create(const Options &o, std::string *why)
    {
        m_opt = o;
        Gs2dVk::Options vo;
        vo.blend = o.blend;
        m_vk.setOptions(vo);
        if (!m_vk.create(why))
            return false;
        // The targets start as GS memory does at reset: zero (alpha 0), Z 0.
        const std::vector<uint32_t> zero(static_cast<size_t>(Gs2dVk::kWidth) * Gs2dVk::kHeight, 0u);
        if (!m_vk.seed(0u, zero.data(), zero.data()) || !m_vk.seed(1u, zero.data(), zero.data()))
        {
            if (why)
                *why = "seeding the targets failed";
            m_vk.destroy();
            return false;
        }
        m_created = true;
        return true;
    }

    bool Presenter::seedFromMemory(const uint8_t *mem, uint32_t bytes)
    {
        if (!m_created || !mem || bytes < kGsMemBytes)
            return false;
        const LayoutTables &lt = layoutTables();
        const uint32_t w = Gs2dVk::kWidth, h = Gs2dVk::kHeight;
        std::vector<uint32_t> planes[2], z(static_cast<size_t>(w) * h);
        for (uint32_t t = 0; t < 2; ++t)
        {
            planes[t].resize(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    std::memcpy(&planes[t][y * w + x], mem + (gsAddr32(lt, t ? 0x50u * 32u : 0u, 10u, x, y) & (kGsBytes - 1u)), 4);
        }
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                uint32_t v;
                std::memcpy(&v, mem + (gsAddrZ32(lt, 0xa0u * 32u, 10u, x, y) & (kGsBytes - 1u)), 4);
                z[y * w + x] = v & 0xFFFFFFu;
            }
        // seed() writes the depth with each target: the second call leaves the Z plane as read.
        return m_vk.seed(0u, planes[0].data(), z.data()) && m_vk.seed(1u, planes[1].data(), z.data());
    }

    void Presenter::destroy()
    {
        if (!m_created)
            return;
        rhi_wait_idle();
        m_vk.destroy();
        m_created = false;
    }

    Presenter::View Presenter::record(RhiCommandList cl, std::vector<Published> &lists)
    {
        if (!m_created)
            return m_view;
        ++m_stats.frames;
        ++m_window.frames;
        const uint32_t slot = rhi_frame_slot();
        if (m_stamped[slot])
        {
            // This slot's last lists are done (rhi_wait_frame recycled it): their GPU time, and K2's (U3).
            m_stamped[slot] = false;
            uint64_t ns[RHI_MAX_TIMESTAMPS] = {};
            const uint32_t n = rhi_read_timestamps(ns, RHI_MAX_TIMESTAMPS);
            if (n >= 2u && ns[0] && ns[1] >= ns[0])
            {
                const double ms = static_cast<double>(ns[1] - ns[0]) / 1e6;
                for (Stats *s : {&m_stats, &m_window})
                {
                    ++s->gpuSamples;
                    s->gpuMs += ms;
                    s->maxGpuMs = std::max(s->maxGpuMs, ms);
                }
            }
            double frameK2 = 0.0;
            uint32_t timed = 0;
            for (uint32_t k = 0; k < m_k2Stamps[slot]; ++k)
            {
                const uint32_t b = 2u + 2u * k, e = b + 1u;
                if (e >= n || !ns[b] || ns[e] < ns[b])
                    continue;
                const double ms = static_cast<double>(ns[e] - ns[b]) / 1e6;
                frameK2 += ms;
                ++timed;
                for (Stats *s : {&m_stats, &m_window})
                {
                    ++s->k2KicksTimed;
                    s->k2Ms += ms;
                    s->maxK2KickMs = std::max(s->maxK2KickMs, ms);
                }
            }
            if (timed)
                for (Stats *s : {&m_stats, &m_window})
                {
                    ++s->k2FramesTimed;
                    s->k2FrameMs += frameK2;
                    s->maxK2FrameMs = std::max(s->maxK2FrameMs, frameK2);
                }
            m_k2Stamps[slot] = 0;
        }
        if (lists.empty())
            return m_view;
        ++m_stats.framesWithLists;
        ++m_window.framesWithLists;
        m_stats.lists += lists.size();
        m_window.lists += lists.size();
        const DrawList *one = &lists.front().list;
        const std::vector<uint32_t> *ranges = &lists.front().k2Ranges;
        if (lists.size() > 1u)
        {
            m_merged.clear();
            m_ranges.clear();
            for (const Published &p : lists)
            {
                const uint32_t shift = static_cast<uint32_t>(m_merged.cmds.size());
                appendList(m_merged, p.list);
                for (uint32_t r : p.k2Ranges)
                    m_ranges.push_back(r + shift);
            }
            one = &m_merged;
            ranges = &m_ranges;
        }
        if (m_vk.record(*one, cl, m_opt.timestamps, ranges))
        {
            m_stamped[slot] = m_opt.timestamps && rhi_timestamps_supported();
            const Gs2dVk::ListStats &l = m_vk.lastList();
            m_k2Stamps[slot] = m_stamped[slot] ? l.rangeStamps : 0u;
            for (Stats *s : {&m_stats, &m_window})
            {
                s->readPrims += l.readPrims;
                s->barriers += l.barriers;
                s->draws += l.draws;
                s->k2KicksUntimed += l.rangesUnstamped;
            }
        }
        else
        {
            ++m_stats.failures;
            ++m_window.failures;
        }

        const Published &last = lists.back();
        View v;
        v.cycle = last.cycle;
        v.width = last.width;
        v.height = last.height;
        if (last.fbp != 0x0u && last.fbp != 0x50u)
        {
            ++m_stats.otherFbp; // drawn as Gs2dVk's plan does: FBP 0x50 is target 1, anything else target 0
            ++m_window.otherFbp;
        }
        m_viewTarget = last.fbp == 0x50u ? 1u : 0u;
        v.texture = m_vk.sampleTarget(cl, m_viewTarget);
        v.texHeight = Gs2dVk::kHeight;
        v.valid = v.texture.id != 0u;
        m_view = v;
        return m_view;
    }

    bool Presenter::readShown(std::vector<uint8_t> &rgba)
    {
        if (!m_view.valid)
            return false;
        const size_t bytes = static_cast<size_t>(m_view.width) * m_view.height * 4u;
        std::vector<uint32_t> words;
        if (!m_vk.readColour(m_viewTarget, words) || words.size() < static_cast<size_t>(Gs2dVk::kWidth) * m_view.height)
            return false;
        rgba.resize(bytes);
        for (uint32_t y = 0; y < m_view.height; ++y)
            std::memcpy(rgba.data() + static_cast<size_t>(y) * m_view.width * 4u, words.data() + static_cast<size_t>(y) * Gs2dVk::kWidth,
                        m_view.width * 4u);
        // readColour left the target in COPY_SRC: back to SHADER_READ for the next present.
        RhiCommandList cl = rhi_begin_commands();
        m_vk.sampleTarget(cl, m_viewTarget);
        rhi_submit(cl);
        rhi_wait_idle();
        return true;
    }
}
