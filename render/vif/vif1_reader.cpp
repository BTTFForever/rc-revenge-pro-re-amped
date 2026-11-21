#include "vif1_reader.h"

#include "../record/chain_walk.h"

#include <x86intrin.h>
#include <xmmintrin.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace rc::rd
{
    using namespace rc::rec;

    namespace
    {
        // Time stamps by the TSC: the clock can be a system call (an HPET clocksource), far too slow per packet. The
        // nanoseconds a tick are calibrated against steady_clock over the reader's life (Vif1Reader::nsPerTick).
        uint64_t nowNs() { return __rdtsc(); }
        uint64_t clockNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }

    const char *vifGuardName(uint32_t guard)
    {
        static const char *const names[kVifGuardCount] = {"none", "VIF_CODE", "VIF_UNPACK", "VIF_TRUNCATED", "VIF_PAGE", "VU_PROGRAM", "VU_ENTRY", "VU_RUNAWAY"};
        return guard < kVifGuardCount ? names[guard] : "?";
    }

    bool parseVifFault(const std::string &name, VifFault &out)
    {
        static const char *const names[] = {"none", "unpack", "tops", "dbf", "mpg", "mscal", "mxcsr", "lane", "packet", "vumem"};
        for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (name == names[i])
            {
                out = static_cast<VifFault>(i);
                return true;
            }
        return false;
    }

    const char *vifFaultName(VifFault f)
    {
        static const char *const names[] = {"none", "unpack", "tops", "dbf", "mpg", "mscal", "mxcsr", "lane", "packet", "vumem"};
        return names[static_cast<uint32_t>(f)];
    }

    bool parseVu1Library(const std::string &name, Vu1Library &out)
    {
        if (name == "strict")
            out = Vu1Library::Strict;
        else if (name == "lean")
            out = Vu1Library::Lean;
        else
            return false;
        return true;
    }

    const char *vu1LibraryName(Vu1Library l) { return l == Vu1Library::Lean ? "lean" : "strict"; }

    Vif1Reader::Vif1Reader() : m_data(0x4000u, 0), m_micro(0x4000u, 0) { reset(); }
    Vif1Reader::Vif1Reader(const Options &options) : m_options(options), m_data(0x4000u, 0), m_micro(0x4000u, 0) { reset(); }

    void Vif1Reader::reset()
    {
        m_vif = VifRegs{};
        std::fill(m_data.begin(), m_data.end(), 0);
        std::fill(m_micro.begin(), m_micro.end(), 0);
        std::memset(m_vf, 0, sizeof(m_vf));
        m_vf[0][3] = 1.0f;
        m_program = -1;
        m_programDirty = true;
    }

    bool Vif1Reader::seed(const Keyframe &kf)
    {
        const StateChunk *vif = kf.find(kStateVif1), *mem = kf.find(kStateVu1Memory), *regs = kf.find(kStateVu1Regs);
        if (!vif || vif->bytes < sizeof(Vif1Regs) || !mem || mem->bytes < 0x8000u || !regs || regs->bytes < sizeof(Vu1Regs))
            return false;
        Vif1Regs r;
        std::memcpy(&r, vif->data, sizeof(r));
        m_vif.cycle = r.cycle;
        m_vif.mode = r.mode;
        m_vif.mask = r.mask;
        m_vif.base = r.base;
        m_vif.ofst = r.ofst;
        m_vif.tops = r.tops;
        m_vif.itops = r.itops;
        m_vif.top = r.top;
        m_vif.itop = r.itop;
        m_vif.dbf = ((r.stat >> 7) & 1u) != 0u;
        std::memcpy(m_vif.row, r.row, sizeof(m_vif.row));
        std::memcpy(m_vif.col, r.col, sizeof(m_vif.col));
        std::memcpy(m_micro.data(), mem->data, 0x4000u);
        std::memcpy(m_data.data(), mem->data + 0x4000u, 0x4000u);
        Vu1Regs v;
        std::memcpy(&v, regs->data, sizeof(v));
        std::memcpy(m_vf, v.vf, sizeof(m_vf));
        m_programDirty = true;
        ++m_stats.seeds;
        return true;
    }

    void Vif1Reader::hw(uint32_t address, uint64_t value)
    {
        // RT/src/lib/ps2_memory.cpp's VIF1 register stores.
        const uint32_t v = static_cast<uint32_t>(value);
        switch (address & 0x1FFFFFFFu)
        {
        case 0x10003C10u:
            if (v & 1u)
            {
                m_vif = VifRegs{};
                ++m_stats.fbrstResets;
            }
            break;
        case 0x10003C40u: m_vif.cycle = v & 0xFFFFu; break;
        case 0x10003C50u: m_vif.mode = v & 3u; break;
        case 0x10003C70u: m_vif.mask = v; break;
        case 0x10003C90u: m_vif.itops = v & 0x3FFu; break;
        case 0x10003CA0u: m_vif.base = v & 0x3FFu; break;
        case 0x10003CB0u: m_vif.ofst = v & 0x3FFu; break;
        case 0x10003CC0u: m_vif.tops = v & 0x3FFu; break;
        case 0x10003CD0u: m_vif.itop = v & 0x3FFu; break;
        case 0x10003CE0u: m_vif.top = v & 0x3FFu; break;
        default: break;
        }
    }

    bool Vif1Reader::guard(uint32_t g, const KickView &k, uint32_t offset, uint32_t code, const char *what)
    {
        m_guard = g;
        ++m_stats.guards[g];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "guard %s: cycle %" PRIu64 " kick %u, stream offset %u, code %08x: %s", vifGuardName(g), k.cycle, k.index, offset,
                      code, what);
        m_guardText = buf;
        return false;
    }

    namespace
    {
        struct IoUser
        {
            Path1Sink *sink;
            Vif1Reader::Stats *stats;
            uint64_t sinkNs;
            uint32_t packets;
        };
        struct Runaway
        {
        };
        void onPacket(void *user, const uint8_t *data, uint32_t bytes)
        {
            IoUser *u = static_cast<IoUser *>(user);
            if (++u->packets > Vif1Reader::kMaxRunPackets || bytes >= 0x10000u * 16u)
                throw Runaway{}; // unwinds out of the reference (RAII only); the run is abandoned
            ++u->stats->packets;
            u->stats->packetBytes += bytes;
            const uint64_t t0 = nowNs();
            u->sink->packet(data, bytes);
            u->sinkNs += nowNs() - t0;
        }
    }

    namespace
    {
        // The entries that send the packet at TOP unchanged (RENDERER_3D_DESIGN.md 5.3; trdr_shape.cpp, env_shape.cpp,
        // terdr_shape.cpp: `p.kick(top)` and return).
        bool isDirectEntry(rc::vu1::Program program, uint32_t pc)
        {
            switch (program)
            {
            case rc::vu1::kTrdr: return pc == 0x2050u;
            case rc::vu1::kEnv: return pc == 0x2370u;
            case rc::vu1::kTerdr: return pc == 0x2198u;
            default: return false;
            }
        }
    }

    bool Vif1Reader::runProgram(const KickView &k, uint32_t pc, uint32_t offset, uint32_t mxcsr, Path1Sink &sink)
    {
        if (m_programDirty)
        {
            m_program = matchVu1Program(m_micro.data(), &m_wholeHash);
            m_programDirty = false;
            ++m_stats.programChanges;
        }
        if (m_program < 0)
        {
            char what[96];
            std::snprintf(what, sizeof(what), "micro memory image %016" PRIx64 " is no known program", m_wholeHash);
            return guard(kVifGuardProgram, k, offset, 0x14000000u | (pc / 8u), what);
        }
        const Vu1ProgramInfo &info = vu1Program(static_cast<uint32_t>(m_program));
        if (!rc::vu1::isEntry(info.program, pc))
        {
            char what[96];
            std::snprintf(what, sizeof(what), "%s has no entry at 0x%04x", info.name, pc);
            return guard(kVifGuardEntry, k, offset, 0x14000000u | (pc / 8u), what);
        }
        Vu1Run run;
        run.program = m_program;
        run.pc = pc;
        run.top = m_vif.top;
        run.itop = m_vif.itop;
        run.index = m_mscalInKick;
        run.streamOffset = offset;
        uint64_t t0 = nowNs();
        sink.runBegin(run);
        const uint64_t t1 = nowNs();
        IoUser user{&sink, &m_stats, t1 - t0, 0u};
        if (m_fault == VifFault::Lane && static_cast<int64_t>(k.cycle) == m_faultCycle)
        {
            const uint32_t lane = m_faultN ? m_faultN : 25u * 4u + 3u;
            m_vf[(lane / 4u) & 31u][lane & 3u] = 0.0f;
        }
        rc::vu1::Io io{m_data.data(), m_vf, pc, m_vif.top, &onPacket, &user};
        const bool direct = m_options.directEntries && isDirectEntry(info.program, pc);
        const uint32_t saved = _mm_getcsr();
        if (!(m_fault == VifFault::NoMxcsr && static_cast<int64_t>(k.cycle) == m_faultCycle))
            _mm_setcsr(mxcsr | 0x6000u);
        bool runaway = false;
        try
        {
            // The lean build sets RZ + FTZ + DAZ on top of the MXCSR set here and restores it (so the NoMxcsr fault
            // has no effect on it: no caller can forget the mode).
            if (direct)
            {
                // What the reference's direct entry does (ShapeProgram::kick at TOP): one packet copied out of VU memory,
                // no register touched. onPacket applies the same runaway cut.
                m_direct.clear();
                rc::vu1::copyGifPacket(m_data.data(), static_cast<int>(m_vif.top & 0x3FFu), m_direct);
                onPacket(&user, m_direct.data(), static_cast<uint32_t>(m_direct.size()));
                ++m_stats.directRuns;
            }
            else if (m_options.library == Vu1Library::Lean)
                rc::vu1l::run(info.program, io);
            else
                rc::vu1::run(info.program, io);
        }
        catch (const Runaway &)
        {
            runaway = true;
        }
        _mm_setcsr(saved);
        if (runaway)
        {
            char what[96];
            std::snprintf(what, sizeof(what), "%s at 0x%04x sent more than %u packets", info.name, pc, kMaxRunPackets);
            return guard(kVifGuardRunaway, k, offset, 0x14000000u | (pc / 8u), what);
        }
        if (m_poison)
            for (uint32_t reg = 0; reg < 32; ++reg)
                if (const uint8_t lanes = info.notReproduced[reg])
                    for (uint32_t lane = 0; lane < 4; ++lane)
                        if (lanes & (1u << lane))
                        {
                            const uint32_t b = vu1PoisonBits(reg, lane);
                            std::memcpy(&m_vf[reg][lane], &b, 4);
                        }
        const uint64_t t2 = nowNs();
        sink.runEnd(run, m_data.data(), m_vf);
        const uint64_t t3 = nowNs();
        user.sinkNs += t3 - t2;
        m_stats.sinkNs += user.sinkNs;
        m_stats.refNs += (t3 - t0) - user.sinkNs;
        ++m_stats.runs[m_program];
        return true;
    }

    double Vif1Reader::nsPerTick() const
    {
        return m_calTicks1 > m_calTicks0 ? static_cast<double>(m_calNs1 - m_calNs0) / static_cast<double>(m_calTicks1 - m_calTicks0) : 0.0;
    }

    bool Vif1Reader::kick(const KickView &k, const RamView &ram, uint32_t mxcsr, Path1Sink &sink)
    {
        if (!m_calNs0)
        {
            m_calNs0 = clockNs();
            m_calTicks0 = __rdtsc();
        }
        const uint64_t start = nowNs();
        const uint64_t refBefore = m_stats.refNs, sinkBefore = m_stats.sinkNs;
        ++m_stats.kicks;
        m_guard = kVifGuardNone;
        m_mscalInKick = 0;
        if (m_faultCounterCycle != k.cycle)
        {
            m_faultCounterCycle = k.cycle;
            m_faultMpg = m_faultMscal = 0;
        }
        const bool faultCycle = static_cast<int64_t>(k.cycle) == m_faultCycle;
        if (m_fault == VifFault::VuMemory && faultCycle)
            std::fill(m_data.begin(), m_data.end(), 0);

        // The stream as ranges: each DMA tag's upper 8 bytes (TTE), then its payload. The decoder is a small state
        // machine over them, so a code whose data starts in the next range (a code in a DMA tag) continues there.
        struct Walk final : WalkVisitor
        {
            Vif1Reader *self;
            const KickView *k;
            const RamView *ram;
            uint32_t mxcsr;
            Path1Sink *sink;
            bool tte;
            bool stopped = false;
            // The code in progress.
            uint32_t code = 0, codeAt = 0;
            uint8_t partial[4] = {};
            uint32_t partialFill = 0;
            uint32_t left = 0;     // data bytes still to come
            enum Dest : uint8_t { None, Data, Micro, Mask, Row, Col } dest = None;
            uint32_t destAt = 0;   // byte offset in the destination (VU memory wraps at 16 KB)
            uint32_t offset = 0;   // stream offset of the next byte

            void bytes(const uint8_t *p, uint32_t n)
            {
                uint32_t at = 0;
                while (at < n && !stopped)
                {
                    if (left)
                    {
                        const uint32_t take = std::min(left, n - at);
                        sinkData(p + at, take);
                        at += take;
                        offset += take;
                        left -= take;
                        continue;
                    }
                    // Assemble the next code word.
                    const uint32_t take = std::min(4u - partialFill, n - at);
                    std::memcpy(partial + partialFill, p + at, take);
                    partialFill += take;
                    at += take;
                    offset += take;
                    if (partialFill < 4u)
                        continue;
                    partialFill = 0;
                    std::memcpy(&code, partial, 4);
                    codeAt = offset - 4u;
                    decode();
                }
            }

            void sinkData(const uint8_t *p, uint32_t n)
            {
                switch (dest)
                {
                case Data:
                {
                    uint32_t done = 0;
                    while (done < n)
                    {
                        const uint32_t room = std::min(n - done, 0x4000u - destAt);
                        std::memcpy(self->m_data.data() + destAt, p + done, room);
                        done += room;
                        destAt = (destAt + room) & 0x3FFFu;
                    }
                    break;
                }
                case Micro:
                {
                    const uint32_t room = destAt < 0x4000u ? std::min(n, 0x4000u - destAt) : 0u;
                    if (room)
                        std::memcpy(self->m_micro.data() + destAt, p, room);
                    destAt += n;
                    break;
                }
                case Mask:
                    std::memcpy(reinterpret_cast<uint8_t *>(&self->m_vif.mask) + destAt, p, n);
                    destAt += n;
                    break;
                case Row:
                    std::memcpy(reinterpret_cast<uint8_t *>(self->m_vif.row) + destAt, p, n);
                    destAt += n;
                    break;
                case Col:
                    std::memcpy(reinterpret_cast<uint8_t *>(self->m_vif.col) + destAt, p, n);
                    destAt += n;
                    break;
                default:
                    break;
                }
            }

            void stop(uint32_t g, const char *what)
            {
                self->guard(g, *k, codeAt, code, what);
                stopped = true;
            }

            void decode()
            {
                Vif1Reader &r = *self;
                VifRegs &v = r.m_vif;
                const uint8_t op = (code >> 24) & 0x7Fu;
                const uint32_t imm = code & 0xFFFFu, num = (code >> 16) & 0xFFu;
                ++r.m_stats.codes;
                ++r.m_stats.codesByOp[op];
                if (code & 0x80000000u)
                    ++r.m_stats.interruptBits;
                const bool fc = static_cast<int64_t>(k->cycle) == r.m_faultCycle;
                dest = None;
                destAt = 0;
                switch (op)
                {
                case 0x00: // NOP
                case 0x07: // MARK (the reader keeps no MARK register)
                case 0x10: // FLUSHE
                case 0x11: // FLUSH
                case 0x13: // FLUSHA: every run is complete before the next code, as in the runtime
                    return;
                case 0x01: v.cycle = imm; return;
                case 0x02:
                    v.ofst = imm & 0x3FFu;
                    v.tops = v.base & 0x3FFu;
                    if (!(r.m_fault == VifFault::OffsetDbf && fc))
                        v.dbf = false;
                    return;
                case 0x03: v.base = imm & 0x3FFu; return;
                case 0x04: v.itops = imm & 0x3FFu; return;
                case 0x05: v.mode = imm & 3u; return;
                case 0x20: dest = Mask; left = 4; return;
                case 0x30: dest = Row; left = 16; return;
                case 0x31: dest = Col; left = 16; return;
                case 0x4A: // MPG
                {
                    dest = Micro;
                    destAt = imm * 8u;
                    left = (num ? num : 256u) * 8u;
                    ++r.m_stats.mpgs;
                    r.m_stats.mpgBytes += left;
                    r.m_programDirty = true;
                    if (r.m_fault == VifFault::DropMpg && fc && r.m_faultMpg++ == r.m_faultN)
                        dest = None;
                    return;
                }
                case 0x14: // MSCAL
                {
                    v.top = v.tops & 0x3FFu;
                    v.itop = v.itops & 0x3FFu;
                    if (!(r.m_fault == VifFault::TopsSwap && fc))
                    {
                        v.tops = v.dbf ? (v.base & 0x3FFu) : ((v.base + v.ofst) & 0x3FFu);
                        v.dbf = !v.dbf;
                    }
                    ++r.m_stats.mscals;
                    const bool skip = r.m_fault == VifFault::SkipMscal && fc && r.m_faultMscal++ == r.m_faultN;
                    if (!skip && !r.runProgram(*k, imm * 8u, codeAt, mxcsr, *sink))
                        stopped = true;
                    ++r.m_mscalInKick;
                    return;
                }
                default:
                    break;
                }
                if ((op & 0x60u) == 0x60u)
                {
                    // UNPACK: only V4-32 (vn 3, vl 0), unmasked, CL = WL, mode 0.
                    if ((op & 0x1Fu) != 0x0Cu || (op & 0x10u) || (v.cycle & 0xFFu) != ((v.cycle >> 8) & 0xFFu) || (v.mode & 3u) != 0u)
                    {
                        stop(kVifGuardUnpack, "UNPACK other than V4-32, unmasked, CL = WL, mode 0");
                        return;
                    }
                    const uint32_t n = num ? num : 256u;
                    uint32_t addr = (imm & 0x3FFu) + ((imm & 0x8000u) ? v.tops : 0u);
                    if (r.m_fault == VifFault::UnpackDest && fc)
                        ++addr;
                    dest = Data;
                    destAt = (addr & 0x3FFu) * 16u;
                    left = n * 16u;
                    ++r.m_stats.unpacks;
                    r.m_stats.unpackBytes += left;
                    return;
                }
                stop(kVifGuardCode, "a VIF code the reader does not implement");
            }

            bool present(const WalkRange &w) const
            {
                if (!ram->present || !w.bytes)
                    return true;
                const uint32_t first = RamView::pageOf(w.physAddr, w.scratch), last = RamView::pageOf(w.physAddr + w.bytes - 1u, w.scratch);
                for (uint32_t page = first; page <= last; ++page)
                    if (!ram->has(page))
                        return false;
                return true;
            }

            void range(const WalkRange &w) override
            {
                if (stopped || (w.isTag && !tte) || !w.bytes)
                    return;
                if (!present(w))
                {
                    codeAt = offset;
                    stop(kVifGuardPage, "a range on a page the RAM view does not hold");
                    return;
                }
                if (w.isTag)
                    bytes(w.data + 8, 8);
                else
                    bytes(w.data, w.bytes);
            }
        } walk;
        walk.self = this;
        walk.k = &k;
        walk.ram = &ram;
        walk.mxcsr = mxcsr;
        walk.sink = &sink;
        walk.tte = k.tte;
        const WalkResult wr = walkChain(k, ram, &walk);
        if (!walk.stopped && (wr.absentPageReads || wr.untranslatable))
            walk.stop(kVifGuardPage, "the walk read a page the RAM view does not hold");
        if (!walk.stopped && (walk.left || walk.partialFill))
        {
            walk.codeAt = walk.offset;
            walk.stop(kVifGuardTruncated, walk.left ? "a code's data cut by the end of the stream" : "bytes left after the last code");
        }
        const uint64_t total = nowNs() - start;
        m_stats.walkNs += total - (m_stats.refNs - refBefore) - (m_stats.sinkNs - sinkBefore);
        m_calNs1 = clockNs();
        m_calTicks1 = __rdtsc();
        return !walk.stopped;
    }
}
