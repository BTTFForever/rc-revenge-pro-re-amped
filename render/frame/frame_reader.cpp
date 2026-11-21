#include "frame_reader.h"

#include <algorithm>
#include <cstring>

namespace rc::rd
{
    using namespace rc::rec;

    namespace
    {
        // A packet whose first GIF tag is PACKED with only A+D descriptors: the game's state packets on PATH1
        // (RENDERER_3D_DESIGN.md 3.5, NLOOP 6 NREG 1 A+D: SCISSOR, ZBUF...).
        bool stateOnly(const uint8_t *data, uint32_t bytes)
        {
            if (bytes < 16u)
                return false;
            uint64_t lo, hi;
            std::memcpy(&lo, data, 8);
            std::memcpy(&hi, data + 8, 8);
            const uint32_t flg = static_cast<uint32_t>(lo >> 58) & 3u, nreg = static_cast<uint32_t>(lo >> 60) & 15u, nloop = lo & 0x7FFFu;
            if (flg != 0u || nloop == 0u)
                return false;
            for (uint32_t i = 0; i < (nreg ? nreg : 16u); ++i)
                if (((hi >> (4u * i)) & 15u) != 0xEu)
                    return false;
            return true;
        }
    }

    FrameReader::FrameReader() : FrameReader(Options{}) {}

    FrameReader::Options FrameReader::resolve(Options o)
    {
        if (o.fast)
        {
            o.gif.path1Direct = true;
            o.gif.clutMemo = true;
            o.gif.stateMemo = true;
        }
        return o;
    }

    FrameReader::FrameReader(const Options &options) : m_opt(resolve(options)), m_gif(m_opt.gif)
    {
        m_bridge.self = this;
        if (m_opt.fast)
        {
            Vif1Reader::Options v = m_vif.options();
            v.library = Vu1Library::Lean;
            v.directEntries = true;
            m_vif.setOptions(v);
        }
    }

    void FrameReader::setFault(VifFault f, int64_t cycle, uint32_t n)
    {
        m_fault = f;
        m_faultCycle = cycle;
        m_faultN = n;
        if (f != VifFault::Packet)
            m_vif.setFault(f, cycle, n);
    }

    void FrameReader::startLive()
    {
        m_vif.reset();
        m_gif.startBlank();
        m_started = true;
    }

    void FrameReader::open(const FileInfo &info) { m_gif.open(info); }

    void FrameReader::keyframe(const Keyframe &kf)
    {
        // As the GIF reader's shadow: the first keyframe, and one after a save-state load, start the reader; later ones
        // hold what the kicks since have built (the checks compare them before this is called).
        if ((kf.header.flags & kKeyFinal) == 0 && (!m_started || (kf.header.flags & kKeyAfterStateLoad)))
        {
            if (m_vif.seed(kf))
                ++m_stats.seeds;
            else
            {
                ++m_stats.seedMissing;
                m_vif.reset();
            }
            m_started = true;
        }
        m_gif.keyframe(kf);
    }

    void FrameReader::cycleBegin(uint64_t cycle)
    {
        m_cycle = cycle;
        m_faultSeen = 0;
        m_path1Ranges.clear();
        m_gif.cycleBegin(cycle);
    }

    void FrameReader::hw(uint32_t address, uint32_t, uint64_t value, uint32_t)
    {
        const uint32_t phys = address & 0x1FFFFFFFu;
        if (phys >= 0x10003C00u && phys < 0x10003E00u)
            m_vif.hw(phys, value);
    }

    void FrameReader::kick(const KickView &k, const RamView &ram)
    {
        if (k.channel == 1u)
        {
            if (!m_opt.deferPath1)
                kickPath1(k, ram, k.mxcsr);
            return;
        }
        if (k.channel == 2u)
            ++m_stats.path3Kicks;
        m_gif.kick(k, ram);
    }

    void FrameReader::cycleEnd(const CycleInfo &info, const uint8_t *refRgba) { m_gif.cycleEnd(info, refRgba); }

    void FrameReader::scopesUntil(uint32_t offset)
    {
        while (m_kick && m_nextSpan < m_spanOrder.size() && m_kick->spans[m_spanOrder[m_nextSpan]].streamOffset <= offset)
            m_gif.scope(m_kick->spans[m_spanOrder[m_nextSpan++]]);
    }

    void FrameReader::Bridge::runBegin(const Vu1Run &run)
    {
        self->scopesUntil(run.streamOffset);
        if (self->m_tap)
            self->m_tap->runBegin(run);
    }

    void FrameReader::Bridge::packet(const uint8_t *data, uint32_t bytes)
    {
        FrameReader &f = *self;
        if (f.m_tap && !f.m_tap->packet(data, bytes))
            return;
        if (f.m_fault == VifFault::Packet && static_cast<int64_t>(f.m_cycle) == f.m_faultCycle && stateOnly(data, bytes) && f.m_faultSeen++ == f.m_faultN)
        {
            ++f.m_stats.packetsDropped;
            return;
        }
        if (!f.m_opt.fast)
            f.m_path1Hash.update(data, bytes);
        if (f.m_opt.collectPath1)
        {
            f.m_path1Bytes.insert(f.m_path1Bytes.end(), data, data + bytes);
            f.m_path1Packets.push_back(bytes);
        }
        f.m_gif.feedPath1(data, bytes);
    }

    void FrameReader::Bridge::runEnd(const Vu1Run &run, uint8_t *vuData, float (*vf)[4])
    {
        if (self->m_tap)
            self->m_tap->runEnd(run, vuData, vf);
    }

    bool FrameReader::kickPath1(const KickView &k, const RamView &ram, uint32_t mxcsr)
    {
        ++m_stats.path1Kicks;
        m_path1Hash = Hash64{};
        m_path1Bytes.clear();
        m_path1Packets.clear();
        const uint32_t firstCmd = static_cast<uint32_t>(m_gif.list().cmds.size());
        m_gif.beginPath1Kick();
        m_kick = &k;
        m_spanOrder.resize(k.spanCount);
        for (uint32_t i = 0; i < k.spanCount; ++i)
            m_spanOrder[i] = i;
        std::stable_sort(m_spanOrder.begin(), m_spanOrder.end(), [&k](uint32_t a, uint32_t b) { return k.spans[a].streamOffset < k.spans[b].streamOffset; });
        m_nextSpan = 0;
        const bool ok = m_vif.kick(k, ram, mxcsr, m_bridge);
        scopesUntil(~0u);
        m_kick = nullptr;
        m_gif.endPath1Kick(!ok);
        m_path1Ranges.push_back(firstCmd);
        m_path1Ranges.push_back(static_cast<uint32_t>(m_gif.list().cmds.size()));
        if (!ok)
            ++m_stats.guardedKicks;
        m_path1Count = m_path1Hash.length();
        m_path1HashValue = m_path1Hash.finish();
        return ok;
    }
}
