// rc_replay's vif consumer: see vif.h and RENDERER_3D_DESIGN.md section 11.
#include "vif.h"

#include "format.h"

#include "../../render/frame/frame_reader.h"
#include "../../render/gs2d/gs2d_encode.h"
#include "../../render/tex/gif_walk.h"

#include "ps2_runtime.h"
#if RC_HAVE_VU1_CODE
#include "ps2_vu1_impl.inl"
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if RC_HAVE_VU1_CODE
#define RC_VIF_IMAGES(X) \
    X(0x6EC8B9FF8FA44221ull) X(0xBAC581270F262987ull) X(0xAB0E7743A9BA7393ull) X(0xEC08B22243D4DC8Cull) X(0xE96770E4143D2DA7ull) \
    X(0xC3952C0FAEB52EADull)
#define RC_VIF_DECLARE(hash) \
    template <> VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<hash>(VU1Interpreter &, const Vu1RunArgs &);
RC_VIF_IMAGES(RC_VIF_DECLARE)
#endif

using namespace rc::rec;
using namespace rc::rd;

namespace rc::replay
{
    namespace
    {
        std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
        std::string fmt(const char *f, ...)
        {
            char buf[1024];
            va_list ap;
            va_start(ap, f);
            std::vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        // The GIF register (or tag) a byte of a PACKED/REGLIST/IMAGE packet belongs to, for the first-difference report.
        std::string gifPlace(const uint8_t *p, uint32_t bytes, uint32_t offset)
        {
            static const char *const packed[16] = {"PRIM", "RGBAQ", "ST", "UV", "XYZF2", "XYZ2", "TEX0_1", "TEX0_2",
                                                   "CLAMP_1", "CLAMP_2", "FOG", "(reserved)", "XYZF3", "XYZ3", "A+D", "NOP"};
            uint32_t at = 0;
            while (at + 16u <= bytes)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, p + at, 8);
                std::memcpy(&hi, p + at + 8, 8);
                const uint32_t nloop = lo & 0x7FFFu, flg = static_cast<uint32_t>(lo >> 58) & 3u;
                uint32_t nreg = static_cast<uint32_t>(lo >> 60) & 15u;
                nreg = nreg ? nreg : 16u;
                const uint32_t quads = flg == 0u ? nloop * nreg : flg == 1u ? (nloop * nreg + 1u) / 2u : nloop;
                if (offset < at + 16u)
                    return fmt("the GIF tag at byte %u (NLOOP %u, FLG %u, NREG %u)", at, nloop, flg, nreg);
                const uint32_t end = at + 16u + quads * 16u;
                if (offset < end)
                {
                    const uint32_t q = (offset - at - 16u) / 16u;
                    if (flg == 0u)
                    {
                        const uint32_t d = static_cast<uint32_t>(hi >> (4u * (q % nreg))) & 15u;
                        if (d == 14u)
                            return fmt("A+D register 0x%02x (loop %u)", p[at + 16u + q * 16u + 8u], q / nreg);
                        return fmt("PACKED %s (loop %u, word %u)", packed[d], q / nreg, ((offset - at - 16u) % 16u) / 4u);
                    }
                    if (flg == 1u)
                        return fmt("REGLIST register %u", static_cast<uint32_t>(hi >> (4u * (((offset - at - 16u) / 8u) % nreg))) & 15u);
                    return "IMAGE data";
                }
                at = end;
            }
            return "past the packet's tags";
        }

        constexpr uint32_t kFails = 40;

        // Debugging aid for (c2) (RC_VIF_LIST_TRACE=1): the register file at every drawn triangle of a GIF stream, so
        // the encoded list can be compared with the raw packets triangle by triangle.
        struct TriTrace final : GifSink
        {
            uint64_t regs[0x80] = {};
            uint64_t q[3][3] = {};
            uint32_t queued = 0;
            std::vector<std::vector<uint64_t>> tris;
            uint32_t vq = 0;
            void vertexQ(uint32_t q) override { vq = q; }
            void reg(uint8_t a, uint64_t v) override
            {
                if (a >= 0x80)
                    return;
                regs[a] = v;
                if (a == 0)
                    queued = 0;
                if (a == 4 || a == 5 || a == 0xC || a == 0xD)
                {
                    const uint32_t i = queued < 3 ? queued : 2;
                    q[i][0] = v;
                    q[i][1] = (regs[1] & 0xFFFFFFFFull) | static_cast<uint64_t>(vq) << 32;
                    q[i][2] = regs[2];
                    if (++queued == 3)
                    {
                        if (a == 4 || a == 5)
                        {
                            std::vector<uint64_t> t(regs, regs + 0x63);
                            for (auto &c : q)
                                t.insert(t.end(), c, c + 3);
                            tris.push_back(std::move(t));
                        }
                        queued = 0;
                    }
                }
            }
            void image(const uint8_t *, uint32_t) override {}
        };
        void traceCompare(const std::vector<uint8_t> &raw, const std::vector<uint32_t> &sizes, const std::vector<uint8_t> &enc, uint64_t cycle, uint32_t kick, const uint64_t *before)
        {
            TriTrace a, b;
            std::memcpy(a.regs, before, 0x63 * 8);
            std::memcpy(b.regs, before, 0x63 * 8);
            GifWalker wa, wb;
            const uint8_t *p = raw.data();
            for (uint32_t n : sizes)
            {
                wa.feed(p, n, a);
                p += n;
            }
            wb.feed(enc.data(), static_cast<uint32_t>(enc.size()), b);
            std::printf("trace cycle %" PRIu64 " kick %u: %zu raw triangles, %zu encoded\n", cycle, kick, a.tris.size(), b.tris.size());

            for (size_t i = 0; i < std::min(a.tris.size(), b.tris.size()); ++i)
            {
                std::string d;
                for (size_t r = 0; r < a.tris[i].size(); ++r)
                    if (r >= 0x63 && a.tris[i][r] != b.tris[i][r])
                        d += fmt(" [v%zu] %016" PRIx64 "/%016" PRIx64, r - 0x63, a.tris[i][r], b.tris[i][r]);
                    else if (a.tris[i][r] != b.tris[i][r] && r != 1 && r != 2 && r != 3 && r != 4 && r != 5 && r != 0xA && r != 0xC && r != 0xD && r != 0x3B)
                        d += fmt(" [%zx] %016" PRIx64 "/%016" PRIx64, r, a.tris[i][r], b.tris[i][r]);
                bool vdiff = false;
                for (size_t r = 0x63; r < a.tris[i].size(); ++r)
                    vdiff |= a.tris[i][r] != b.tris[i][r];
                if (vdiff)
                    d += " (vertex data)";
                static int shown = 0;
                if (!d.empty() && shown++ < 8)
                    std::printf("trace: triangle %zu (PRIM %" PRIx64 ") differs:%s\n", i, a.tris[i][0], d.c_str());
                if (vdiff && shown < 3)
                    for (size_t j = i > 2 ? i - 2 : 0; j <= i + 1 && j < a.tris.size(); ++j)
                    {
                        std::printf("  tri %zu raw:", j);
                        for (size_t r = 0x63; r < a.tris[j].size(); ++r)
                            std::printf(" %016" PRIx64, a.tris[j][r]);
                        std::printf("\n  tri %zu enc:", j);
                        for (size_t r = 0x63; r < b.tris[j].size(); ++r)
                            std::printf(" %016" PRIx64, b.tris[j][r]);
                        std::printf("\n");
                    }
            }
        }
    }

    struct VifCheck::Impl
    {
        VifOptions opt;
        PS2Runtime *rt = nullptr;
        FrameReader own;
        FrameReader *fr = &own;
        bool attached = false; // the frame reader is another consumer's (gif2d --with-emu): it drives it
        FileHeader header{};
        bool exact = false;
        uint64_t cycle = 0;
        uint64_t failures = 0;
        std::vector<std::string> failText;
        void fail(const std::string &s)
        {
            ++failures;
            if (failText.size() < kFails)
                failText.push_back(s);
        }

        // (a) state at keyframes
        uint64_t kfCompared = 0, kfVifOk = 0, kfMicroOk = 0, kfDataOk = 0, kfVfOk = 0, kfRegsOk = 0, kfRegsCompared = 0, kfPoisonedLanes = 0;
        // (a) register files
        uint64_t regKicks = 0, regCompared = 0, regDiffer = 0, regDifferBy[0x80] = {};
        // (b) without the machine
        uint64_t hashKicks = 0, hashOk = 0;
        std::vector<std::string> hashFailKicks;
        // timing per K2 kick
        uint64_t timedKicks = 0, maxRefNs = 0, maxWalkNs = 0, maxSinkNs = 0, maxTotalNs = 0;

        // ---- beside the machine ----
        bool divert = false;
        struct MachineRun
        {
            int program = -1;
            uint32_t pc = 0, top = 0;
            bool cut = false, other = false;
            std::vector<std::vector<uint8_t>> packets;
            std::vector<uint8_t> mem = std::vector<uint8_t>(0x4000u);
            float vf[32][4] = {};
            float vfBefore[32][4] = {};
        };
        std::vector<MachineRun> runs;
        size_t runCount = 0;
        bool handled = false;
        uint64_t handledCycle = ~0ull;
        uint32_t handledIndex = 0;
        // The tap's state for the kick in progress.
        struct Tap final : Path1Tap
        {
            Impl *m = nullptr;
            const KickView *k = nullptr;
            size_t ri = 0;            // runs begun by the reader in this kick
            MachineRun *cur = nullptr;
            uint32_t packetIndex = 0;
            bool runFailed = false;
            std::string beginLanes; // lanes of the reader's registers that differ from the machine's when the run starts
            Vu1Run run;
            std::vector<uint8_t> memBefore = std::vector<uint8_t>(0x4000u);
            float vfBefore[32][4] = {};
            std::vector<std::vector<uint8_t>> readerPackets; // kept only for a written-out run
            void runBegin(const Vu1Run &r) override;
            bool packet(const uint8_t *data, uint32_t bytes) override;
            void runEnd(const Vu1Run &r, uint8_t *vuData, float (*vf)[4]) override;
        } tap;
        bool wroteRun = false;
        uint64_t runsCompared = 0, runsDiffer = 0, packetsCompared = 0, packetsDiffer = 0, countDiffer = 0, memCompared = 0, memDiffer = 0;
        uint64_t vfCompared = 0, vfDiffer = 0, vfPoisoned = 0, vfLaneDiffer[kVu1ProgramCount][128] = {};
        uint64_t runMismatch = 0, cutRuns = 0, cutKicks = 0, cutPacketsLeftOut = 0, otherResults = 0;
        uint64_t kickVifCompared = 0, kickVifDiffer = 0, kickMicroDiffer = 0;
        std::vector<std::string> cutList;
        bool kickHadCut = false;
        // (c)
        std::vector<uint8_t> subBytes;
        uint64_t encodedKicks = 0, encodedPrims = 0, encodedBytes = 0, restored = 0, unrestorable = 0;

        Impl(const VifOptions &o, PS2Runtime *r) : opt(o), rt(r), own(makeOptions(o)) {}
        static FrameReader::Options makeOptions(const VifOptions &o)
        {
            FrameReader::Options f;
            f.gif.seed = true;
            f.gif.textures = false;
            f.gif.cullOffscreen = o.withEmu && o.substitute == "list-cull";
            f.deferPath1 = o.withEmu;
            f.collectPath1 = o.withEmu && (o.substitute == "bytes" || std::getenv("RC_VIF_LIST_TRACE"));
            f.fast = o.fast;
            f.fastPaths = o.fastPaths;
            return f;
        }

        void compareRegs(const KickView &k)
        {
            if (!k.regs || !fr->started())
                return;
            ++regKicks;
            const Gif2dReader &g = fr->gif();
            for (uint32_t a = 0; a < kGsRegFile; ++a)
            {
                if (a == kGsHwreg || (g.provenance()[a] != kProvStream && g.provenance()[a] != kProvSeeded))
                    continue;
                ++regCompared;
                if (g.regs()[a] != k.regs[a])
                {
                    ++regDiffer;
                    ++regDifferBy[a];
                    fail(fmt("(a) cycle %" PRIu64 " kick %u (ch%u): GS register 0x%02x is %016" PRIx64 " in the reader (%s), recorded %016" PRIx64, k.cycle,
                             k.index, k.channel, a, g.regs()[a], g.provenance()[a] == kProvStream ? "from the stream" : "seeded", k.regs[a]));
                }
            }
        }

        void path1Kick(const KickView &k, const RamView &ram, bool beside)
        {
            if (!attached)
                compareRegs(k);
            if (!fr->started())
                return;
            const Vif1Reader::Stats before = fr->vif().stats();
            if (beside)
            {
                tap.m = this;
                tap.k = &k;
                tap.ri = 0;
                tap.cur = nullptr;
                kickHadCut = false;
                fr->setPath1Tap(&tap);
            }
            const bool ok = fr->kickPath1(k, ram, k.mxcsr);
            fr->setPath1Tap(nullptr);
            if (!ok)
                fail(fmt("(a) %s", fr->guardText().c_str()));
            const Vif1Reader::Stats &after = fr->vif().stats();
            const uint64_t ref = after.refNs - before.refNs, walk = after.walkNs - before.walkNs, sink = after.sinkNs - before.sinkNs;
            ++timedKicks;
            maxRefNs = std::max(maxRefNs, ref);
            maxWalkNs = std::max(maxWalkNs, walk);
            maxSinkNs = std::max(maxSinkNs, sink);
            maxTotalNs = std::max(maxTotalNs, ref + walk + sink);
            if (beside)
            {
                if (tap.ri != runCount)
                {
                    ++runMismatch;
                    fail(fmt("(b) cycle %" PRIu64 " kick %u: the reader ran %zu programs, the machine %zu", k.cycle, k.index, tap.ri, runCount));
                }
                if (kickHadCut)
                {
                    ++cutKicks;
                    if (cutList.size() < 400)
                        cutList.push_back(fmt("%" PRIu64 ":%u", k.cycle, k.index));
                }
                // The VIF1 registers and micro memory after the kick.
                const VIFRegisters &mv = rt->memory().vif1_regs;
                const VifRegs &rv = fr->vif().regs();
                ++kickVifCompared;
                const bool same = mv.cycle == rv.cycle && mv.mode == rv.mode && mv.mask == rv.mask && mv.base == rv.base && mv.ofst == rv.ofst &&
                                  mv.tops == rv.tops && mv.itops == rv.itops && mv.top == rv.top && mv.itop == rv.itop &&
                                  (((mv.stat >> 7) & 1u) != 0u) == rv.dbf;
                if (!same)
                {
                    ++kickVifDiffer;
                    fail(fmt("(a) cycle %" PRIu64 " kick %u: VIF1 registers after the kick differ from the machine's (TOPS %x/%x BASE %x/%x OFST %x/%x DBF %u/%u)",
                             k.cycle, k.index, rv.tops, mv.tops, rv.base, mv.base, rv.ofst, mv.ofst, rv.dbf ? 1u : 0u, (mv.stat >> 7) & 1u));
                }
                if (std::memcmp(fr->vif().vuMicro(), rt->memory().getVU1Code(), 0x4000u) != 0)
                {
                    ++kickMicroDiffer;
                    fail(fmt("(a) cycle %" PRIu64 " kick %u: micro memory after the kick differs from the machine's", k.cycle, k.index));
                }
            }
            else if (k.hasGifHash && exact && !opt.fast) // the fast reader keeps no PATH1 hash (D-P2-3)
            {
                // (b) without the machine: the bytes as the GS would receive them.
                ++hashKicks;
                if (fr->path1Hash() == k.gifHash && fr->path1ByteCount() == k.gifBytes)
                    ++hashOk;
                else
                {
                    fail(fmt("(b) cycle %" PRIu64 " kick %u: PATH1 %" PRIu64 " bytes hash %016" PRIx64 ", recorded %u bytes hash %016" PRIx64, k.cycle, k.index,
                             fr->path1ByteCount(), fr->path1Hash(), k.gifBytes, k.gifHash));
                    if (hashFailKicks.size() < 400)
                        hashFailKicks.push_back(fmt("%" PRIu64 ":%u", k.cycle, k.index));
                }
            }
        }

        void writeRun(const MachineRun &mr, const std::vector<std::vector<uint8_t>> &mine)
        {
            if (opt.outDir.empty() || wroteRun)
                return;
            wroteRun = true;
            const std::string stem = opt.outDir + fmt("/vu1-%" PRIu64 "-%u-%u", tap.k->cycle, tap.k->index, tap.run.index);
            // shader_check's run record: VU data memory before the run, the registers before it, TOP.
            if (std::FILE *f = std::fopen((stem + ".bin").c_str(), "wb"))
            {
                std::fwrite(tap.memBefore.data(), 1, 0x4000u, f);
                std::fwrite(tap.vfBefore, 1, sizeof(tap.vfBefore), f);
                std::fwrite(&tap.run.top, 1, 4, f);
                std::fclose(f);
            }
            // Both packet lists: count, then (size, bytes) each; the machine's first.
            if (std::FILE *f = std::fopen((stem + ".packets").c_str(), "wb"))
            {
                for (const auto *list : {&mr.packets, &mine})
                {
                    const uint32_t n = static_cast<uint32_t>(list->size());
                    std::fwrite(&n, 1, 4, f);
                    for (const std::vector<uint8_t> &p : *list)
                    {
                        const uint32_t s = static_cast<uint32_t>(p.size());
                        std::fwrite(&s, 1, 4, f);
                        std::fwrite(p.data(), 1, s, f);
                    }
                }
                std::fclose(f);
            }
            std::printf("vif: wrote %s.bin (shader_check's run record) and %s.packets\n", stem.c_str(), stem.c_str());
        }
    };

    void VifCheck::Impl::Tap::runBegin(const Vu1Run &r)
    {
        run = r;
        packetIndex = 0;
        runFailed = false;
        readerPackets.clear();
        std::memcpy(memBefore.data(), m->fr->vif().vuData(), 0x4000u);
        std::memcpy(vfBefore, m->fr->vif().vf(), sizeof(vfBefore));
        cur = ri < m->runCount ? &m->runs[ri] : nullptr;
        ++ri;
        if (!cur)
            return;
        beginLanes.clear();
        for (uint32_t lane = 0; lane < 128; ++lane)
            if (std::memcmp(&vfBefore[lane / 4][lane & 3], &cur->vfBefore[lane / 4][lane & 3], 4) != 0)
                beginLanes += fmt(" VF%02u.%c", lane / 4, "xyzw"[lane & 3]);
        ++m->runsCompared;
        if (cur->program != r.program || cur->pc != r.pc || cur->top != r.top)
        {
            m->fail(fmt("(b) cycle %" PRIu64 " kick %u MSCAL %u (stream offset %u): the reader runs %s at 0x%04x TOP 0x%03x, the machine %s at 0x%04x TOP 0x%03x",
                        k->cycle, k->index, r.index, r.streamOffset, vu1Program(r.program).name, r.pc, r.top,
                        cur->program >= 0 ? vu1Program(cur->program).name : "?", cur->pc, cur->top));
            runFailed = true;
            cur = nullptr;
        }
    }

    bool VifCheck::Impl::Tap::packet(const uint8_t *data, uint32_t bytes)
    {
        const uint32_t j = packetIndex++;
        if (!cur)
            return true;
        if (cur->cut && j >= cur->packets.size())
        {
            ++m->cutPacketsLeftOut;
            return false; // beyond the machine's cut: left out (RENDERER_3D_DESIGN.md 6.3)
        }
        if (!m->opt.outDir.empty() && !m->wroteRun)
            readerPackets.emplace_back(data, data + bytes);
        ++m->packetsCompared;
        if (j >= cur->packets.size())
        {
            ++m->packetsDiffer;
            if (!runFailed)
                m->fail(fmt("(b) cycle %" PRIu64 " kick %u MSCAL %u (stream offset %u) %s at 0x%04x TOP 0x%03x: packet %u is beyond the machine's %zu", k->cycle,
                            k->index, run.index, run.streamOffset, vu1Program(run.program).name, run.pc, run.top, j, cur->packets.size()));
            runFailed = true;
            return true;
        }
        const std::vector<uint8_t> &e = cur->packets[j];
        if (e.size() == bytes && std::memcmp(e.data(), data, bytes) == 0)
            return true;
        ++m->packetsDiffer;
        if (!runFailed)
        {
            uint32_t at = 0;
            while (at < bytes && at < e.size() && e[at] == data[at])
                ++at;
            const uint32_t word = at & ~3u;
            uint32_t mine = 0, theirs = 0;
            if (word + 4u <= bytes)
                std::memcpy(&mine, data + word, 4);
            if (word + 4u <= e.size())
                std::memcpy(&theirs, e.data() + word, 4);
            m->fail(fmt("(b) cycle %" PRIu64 " kick %u MSCAL %u (stream offset %u) %s at 0x%04x TOP 0x%03x: packet %u differs at byte %u (%u bytes, machine "
                        "%zu): word %08x, machine %08x, in %s",
                        k->cycle, k->index, run.index, run.streamOffset, vu1Program(run.program).name, run.pc, run.top, j, at, bytes, e.size(), mine, theirs,
                        gifPlace(e.data(), static_cast<uint32_t>(e.size()), at).c_str()));
        }
        runFailed = true;
        return true;
    }

    void VifCheck::Impl::Tap::runEnd(const Vu1Run &r, uint8_t *vuData, float (*vf)[4])
    {
        if (!cur)
            return;
        if (cur->cut)
        {
            // The machine abandoned this run at its cycle budget: the reader takes the machine's state after it.
            ++m->cutRuns;
            m->kickHadCut = true;
            std::memcpy(vuData, cur->mem.data(), 0x4000u);
            std::memcpy(vf, cur->vf, sizeof(cur->vf));
            return;
        }
        if (packetIndex != cur->packets.size())
        {
            ++m->countDiffer;
            if (!runFailed)
                m->fail(fmt("(b) cycle %" PRIu64 " kick %u MSCAL %u %s at 0x%04x: %u packets, the machine %zu", k->cycle, k->index, r.index,
                            vu1Program(r.program).name, r.pc, packetIndex, cur->packets.size()));
            runFailed = true;
        }
        ++m->memCompared;
        if (std::memcmp(vuData, cur->mem.data(), 0x4000u) != 0)
        {
            ++m->memDiffer;
            uint32_t at = 0;
            while (at < 0x4000u && vuData[at] == cur->mem[at])
                ++at;
            m->fail(fmt("(a) cycle %" PRIu64 " kick %u MSCAL %u %s at 0x%04x: VU data memory after the run differs from the machine's at byte 0x%04x", k->cycle,
                        k->index, r.index, vu1Program(r.program).name, r.pc, at));
            runFailed = true;
        }
        ++m->vfCompared;
        bool vfSame = true;
        for (uint32_t lane = 0; lane < 128; ++lane)
            if (std::memcmp(&vf[lane / 4][lane & 3], &cur->vf[lane / 4][lane & 3], 4) != 0)
            {
                uint32_t b;
                std::memcpy(&b, &vf[lane / 4][lane & 3], 4);
                if (isVu1Poison(b, lane / 4, lane & 3))
                {
                    ++m->vfPoisoned;
                    continue;
                }
                ++m->vfLaneDiffer[r.program][lane];
                vfSame = false;
            }
        if (!vfSame)
        {
            ++m->vfDiffer;
            if (!m->opt.noPoison)
            {
                m->fail(fmt("(a) cycle %" PRIu64 " kick %u MSCAL %u %s at 0x%04x: the vector registers after the run differ from the machine's", k->cycle, k->index,
                            r.index, vu1Program(r.program).name, r.pc));
                runFailed = true;
            }
        }
        if (runFailed)
        {
            ++m->runsDiffer;
            if (m->runsDiffer <= 3)
                m->fail(fmt("(a) ... that run started with these register lanes different from the machine's:%s", beginLanes.empty() ? " none" : beginLanes.c_str()));
            m->writeRun(*cur, readerPackets);
        }
    }

#if RC_HAVE_VU1_CODE
    namespace
    {
        VifCheck::Impl *g_vif = nullptr;
    }
    struct VifCheckAccess
    {
        template <uint64_t Hash>
        static VU1InterpreterAccess::StepResult wrapped(VU1Interpreter &vu, const Vu1RunArgs &args)
        {
            VifCheck::Impl *s = g_vif;
            if (!s)
                return VU1InterpreterAccess::runImage<Hash>(vu, args);
            int program = -1;
            for (uint32_t p = 0; p < kVu1ProgramCount; ++p)
                if (vu1Program(p).imageHash == Hash)
                    program = static_cast<int>(p);
            const VU1State st = VU1InterpreterAccess::effectiveState(vu);
            if (s->runCount == s->runs.size())
                s->runs.emplace_back();
            VifCheck::Impl::MachineRun &m = s->runs[s->runCount++];
            m.program = program;
            m.pc = st.pc;
            m.top = st.top;
            std::memcpy(m.vfBefore, st.vf, sizeof(m.vfBefore));
            m.packets.clear();
            vu1_impl::Vu1PacketSink sink;
            sink.forward = !s->divert;
            vu1_impl::Vu1PacketSink *previous = vu1_impl::packetSink;
            vu1_impl::packetSink = &sink;
            const auto result = VU1InterpreterAccess::runImage<Hash>(vu, args);
            vu1_impl::packetSink = previous;
            m.packets = std::move(sink.packets);
            m.cut = result == VU1InterpreterAccess::StepResult::Budget;
            m.other = result != VU1InterpreterAccess::StepResult::Ended && !m.cut;
            if (m.other)
                ++s->otherResults;
            std::memcpy(m.mem.data(), args.vuData, 0x4000u);
            const VU1State after = VU1InterpreterAccess::effectiveState(vu);
            std::memcpy(m.vf, after.vf, sizeof(m.vf));
            return result;
        }
    };
#endif

    VifCheck::VifCheck(const VifOptions &options, PS2Runtime *runtime, FrameReader *external) : m(std::make_unique<Impl>(options, runtime))
    {
        if (external)
        {
            m->fr = external;
            m->attached = true;
            m->opt.substitute = "none";
        }
        if (options.fault != VifFault::None)
            m->fr->setFault(options.fault, options.faultCycle, options.faultN);
        m->fr->vif().setPoison(!options.noPoison);
        {
            Vif1Reader::Options vo = m->fr->vif().options();
            vo.library = options.fast ? Vu1Library::Lean : options.library;
            m->fr->vif().setOptions(vo);
        }
        m->divert = m->opt.withEmu && m->opt.substitute != "none";
    }

    VifCheck::~VifCheck()
    {
#if RC_HAVE_VU1_CODE
        if (g_vif == m.get())
            g_vif = nullptr;
#endif
    }

    void VifCheck::installVu1Wrappers()
    {
#if RC_HAVE_VU1_CODE
        g_vif = m.get();
#define RC_VIF_REGISTER(hash) registerVu1CompiledProgram(hash, &VifCheckAccess::wrapped<hash>);
        RC_VIF_IMAGES(RC_VIF_REGISTER)
#undef RC_VIF_REGISTER
#else
        std::printf("vif: built without the generated VU1 programs: no comparison beside the machine\n");
#endif
    }

    Emu::Path1Substitute VifCheck::path1(const KickView &k, const RamView &ram)
    {
        Emu::Path1Substitute s;
        m->handled = true;
        m->handledCycle = k.cycle;
        m->handledIndex = k.index;
        const DrawList &l = m->fr->list();
        const uint32_t firstCmd = static_cast<uint32_t>(l.cmds.size());
        uint64_t before[kGsRegFile];
        std::memcpy(before, k.regs ? k.regs : m->fr->gif().regs(), sizeof(before));
        m->path1Kick(k, ram, true);
        m->runCount = 0;
        if (!m->divert || !m->fr->started())
            return s;
        if (m->opt.substitute == "bytes")
        {
            s.bytes = &m->fr->path1Bytes();
            s.packets = &m->fr->path1Packets();
            s.checkHash = true;
            return s;
        }
        // The kick's K2 draw list encoded back (RENDERER_3D_DESIGN.md 11.3 (c2)).
        const Gif2dReader &g = m->fr->gif();
        bool afterKnown[kGsRegFile];
        for (uint32_t a = 0; a < kGsRegFile; ++a)
            afterKnown[a] = a != kGsHwreg && g.provenance()[a] != kProvUnknown;
        const uint32_t first = std::min(firstCmd, g.kickFirstCommand());
        GsEncodeResult e = encodeDrawList(l, first, static_cast<uint32_t>(l.cmds.size()), before, g.regs(), afterKnown);
        ++m->encodedKicks;
        m->encodedPrims += e.prims;
        m->encodedBytes += e.gif.size();
        m->restored += e.restored;
        if (e.unrestorable)
        {
            m->unrestorable += e.unrestorable;
            m->fail(fmt("(c) cycle %" PRIu64 " kick %u: %u register(s) with side effects left different (first 0x%02x)", k.cycle, k.index, e.unrestorable,
                        e.firstUnrestorable));
        }
        m->subBytes = std::move(e.gif);
        if (std::getenv("RC_VIF_LIST_TRACE"))
            traceCompare(m->fr->path1Bytes(), m->fr->path1Packets(), m->subBytes, k.cycle, k.index, before);
        s.bytes = &m->subBytes;
        s.checkHash = false;
        return s;
    }

    void VifCheck::open(const FileInfo &info)
    {
        m->header = info.header;
        m->exact = (info.header.flags & kFileExact) != 0;
        if (!m->attached)
            m->fr->open(info);
    }

    void VifCheck::keyframe(const Keyframe &kf)
    {
        Impl &s = *m;
        if (s.fr->started() && !(kf.header.flags & kKeyAfterStateLoad))
        {
            const StateChunk *vif = kf.find(kStateVif1), *mem = kf.find(kStateVu1Memory), *regs = kf.find(kStateVu1Regs), *gs = kf.find(kStateGsRegs);
            if (vif && mem && regs && vif->bytes >= sizeof(Vif1Regs) && mem->bytes >= 0x8000u && regs->bytes >= sizeof(Vu1Regs))
            {
                ++s.kfCompared;
                const char *what = (kf.header.flags & kKeyFinal) ? "the final keyframe" : "the keyframe";
                Vif1Regs r;
                std::memcpy(&r, vif->data, sizeof(r));
                const VifRegs &v = s.fr->vif().regs();
                const bool vifSame = v.cycle == r.cycle && v.mode == r.mode && v.mask == r.mask && v.base == r.base && v.ofst == r.ofst && v.tops == r.tops &&
                                     v.itops == r.itops && v.top == r.top && v.itop == r.itop && v.dbf == (((r.stat >> 7) & 1u) != 0u) &&
                                     std::memcmp(v.row, r.row, 16) == 0 && std::memcmp(v.col, r.col, 16) == 0;
                if (vifSame)
                    ++s.kfVifOk;
                else
                    s.fail(fmt("(a) %s at cycle %" PRIu64 ": VIF1 registers differ (CYCLE %x/%x MODE %x/%x MASK %x/%x BASE %x/%x OFST %x/%x TOPS %x/%x ITOPS %x/%x TOP "
                               "%x/%x ITOP %x/%x DBF %u/%u)",
                               what, kf.header.cycle, v.cycle, r.cycle, v.mode, r.mode, v.mask, r.mask, v.base, r.base, v.ofst, r.ofst, v.tops, r.tops, v.itops,
                               r.itops, v.top, r.top, v.itop, r.itop, v.dbf ? 1u : 0u, (r.stat >> 7) & 1u));
                if (std::memcmp(s.fr->vif().vuMicro(), mem->data, 0x4000u) == 0)
                    ++s.kfMicroOk;
                else
                    s.fail(fmt("(a) %s at cycle %" PRIu64 ": VU1 micro memory differs", what, kf.header.cycle));
                const uint8_t *data = s.fr->vif().vuData();
                uint32_t at = 0;
                while (at < 0x4000u && data[at] == mem->data[0x4000u + at])
                    ++at;
                if (at == 0x4000u)
                    ++s.kfDataOk;
                else
                    s.fail(fmt("(a) %s at cycle %" PRIu64 ": VU1 data memory differs (first at 0x%04x)", what, kf.header.cycle, at));
                Vu1Regs vr;
                std::memcpy(&vr, regs->data, sizeof(vr));
                uint32_t lanes = 0, firstLane = 0;
                for (uint32_t lane = 128; lane-- > 0;)
                    if (std::memcmp(&s.fr->vif().vf()[lane / 4][lane & 3], &vr.vf[lane / 4][lane & 3], 4) != 0)
                    {
                        uint32_t b;
                        std::memcpy(&b, &s.fr->vif().vf()[lane / 4][lane & 3], 4);
                        if (isVu1Poison(b, lane / 4, lane & 3))
                        {
                            ++s.kfPoisonedLanes;
                            continue;
                        }
                        ++lanes;
                        firstLane = lane;
                    }
                if (!lanes)
                    ++s.kfVfOk;
                else
                    s.fail(fmt("(a) %s at cycle %" PRIu64 ": %u lanes of the vector registers differ (first VF%02u.%c)", what, kf.header.cycle, lanes, firstLane / 4,
                               "xyzw"[firstLane & 3]));
            }
            if (gs && gs->bytes >= kGsRegFile * 8u && s.fr->gif().regs())
            {
                ++s.kfRegsCompared;
                uint32_t n = 0, first = 0;
                for (uint32_t a = kGsRegFile; a-- > 0;)
                {
                    uint64_t v;
                    std::memcpy(&v, gs->data + a * 8u, 8);
                    if (a != kGsHwreg && s.fr->gif().provenance()[a] != kProvUnknown && v != s.fr->gif().regs()[a])
                    {
                        ++n;
                        first = a;
                    }
                }
                if (!n)
                    ++s.kfRegsOk;
                else
                    s.fail(fmt("(a) keyframe at cycle %" PRIu64 ": %u GS registers differ from GSRG (first 0x%02x)", kf.header.cycle, n, first));
            }
        }
        if (!s.attached)
            s.fr->keyframe(kf);
    }

    void VifCheck::cycleBegin(uint64_t cycle)
    {
        m->cycle = cycle;
        if (!m->attached)
            m->fr->cycleBegin(cycle);
    }

    void VifCheck::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa)
    {
        if (!m->attached)
            m->fr->hw(address, width, value, callerRa);
    }

    void VifCheck::kick(const KickView &k, const RamView &ram)
    {
        Impl &s = *m;
        if (k.channel == 1u)
        {
            if (s.opt.withEmu)
            {
                if (s.handled && s.handledCycle == k.cycle && s.handledIndex == k.index)
                {
                    s.handled = false;
                    return;
                }
                // The machine did not run this kick (before its first keyframe): the reader takes it alone.
                s.runCount = 0;
            }
            s.path1Kick(k, ram, false);
            return;
        }
        if (s.attached)
            return;
        s.compareRegs(k);
        s.fr->kick(k, ram);
    }

    void VifCheck::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        if (!m->attached)
            m->fr->cycleEnd(info, refRgba);
    }

    bool VifCheck::failed() const { return m->failures != 0; }

    void VifCheck::summary() const
    {
        const Impl &s = *m;
        const Vif1Reader::Stats &v = s.fr->vif().stats();
        const Gif2dReader::Stats &g = s.fr->gif().stats();
        std::printf("vif: %" PRIu64 " channel 1 kicks, %" PRIu64 " VIF codes, %" PRIu64 " UNPACKs (%.1f MB), %" PRIu64 " MPGs, %" PRIu64 " MSCALs; %" PRIu64
                    " PATH1 packets (%.1f MB); %" PRIu64 " seeds, %" PRIu64 " FBRST resets, %" PRIu64 " codes with the interrupt bit\n",
                    v.kicks, v.codes, v.unpacks, v.unpackBytes / 1048576.0, v.mpgs, v.mscals, v.packets, v.packetBytes / 1048576.0, v.seeds, v.fbrstResets,
                    v.interruptBits);
        std::string runs, guards;
        for (uint32_t p = 0; p < kVu1ProgramCount; ++p)
            if (v.runs[p])
                runs += fmt(" %s %" PRIu64 ";", vu1Program(p).name, v.runs[p]);
        for (uint32_t i = 1; i < kVifGuardCount; ++i)
            if (v.guards[i])
                guards += fmt(" %s %" PRIu64, vifGuardName(i), v.guards[i]);
        std::printf("vif: runs by program:%s guards:%s\n", runs.empty() ? " none" : runs.c_str(), guards.empty() ? " none" : guards.c_str());
        std::printf("vif: GIF reader: %" PRIu64 " PATH1 packets, %" PRIu64 " left a tag open, %" PRIu64 " transfer writes on PATH1, %" PRIu64
                    " kicks guarded; %" PRIu64 " primitives (%" PRIu64 " triangles), dropped %" PRIu64 ", culled %" PRIu64 " (no area) + %" PRIu64
                    " (outside the scissor)\n",
                    g.path1Packets, g.path1OpenTags, g.path1Transfers, g.path1Guarded, g.prims, g.primsByKind[kKindTriangles], g.dropped, g.culledZeroArea,
                    g.culledOffscreen);
        std::printf("vif: vertices whose Q (the reference's) is not the RGBAQ register's: %" PRIu64 "\n", g.vertexQNotRgbaq);
        std::string need;
        for (uint32_t gr = 0; gr < kGsGroupCount; ++gr)
            if (g.neededSeeded[gr] || g.neededDefault[gr])
                need += fmt(" %s %" PRIu64 "/%" PRIu64, gsGroupName(gr), g.neededSeeded[gr], g.neededDefault[gr]);
        std::printf("vif: primitives that needed a seeded/default register after the dump's first clear (seeded/default):%s; before it %" PRIu64 "\n",
                    need.empty() ? " none" : need.c_str(), g.neededBeforeFirstClear);
        const double ms = s.fr->vif().nsPerTick() / 1e6;
        if (s.timedKicks)
            std::printf("vif: time a channel 1 kick (mean / largest)%s: references (%s) %.2f / %.2f ms, VIF walk %.2f / %.2f ms, %s %.2f / %.2f ms, "
                        "all %.2f / %.2f ms\n",
                        s.opt.withEmu ? " (with the comparisons beside the machine)" : s.opt.fast ? " (the fast reader)" : "", vu1LibraryName(s.fr->vif().options().library), v.refNs * ms / s.timedKicks, s.maxRefNs * ms,
                        v.walkNs * ms / s.timedKicks, s.maxWalkNs * ms, s.opt.withEmu ? "comparison, GIF walk and list" : "GIF walk and list",
                        v.sinkNs * ms / s.timedKicks, s.maxSinkNs * ms, (v.refNs + v.walkNs + v.sinkNs) * ms / s.timedKicks, s.maxTotalNs * ms);
        std::printf("(a) keyframes: %" PRIu64 " compared: VIF1 registers %" PRIu64 ", micro memory %" PRIu64 ", data memory %" PRIu64 ", vector registers %" PRIu64
                    " equal (%" PRIu64 " lanes excluded: not reproduced, poisoned); GS register file %" PRIu64 "/%" PRIu64 " equal to GSRG\n",
                    s.kfCompared, s.kfVifOk, s.kfMicroOk, s.kfDataOk, s.kfVfOk, s.kfPoisonedLanes, s.kfRegsOk, s.kfRegsCompared);
        std::string by;
        for (uint32_t a = 0; a < 0x80; ++a)
            if (s.regDifferBy[a])
                by += fmt(" 0x%02x x%" PRIu64, a, s.regDifferBy[a]);
        std::printf("(a) register files: %" PRIu64 " values compared at %" PRIu64 " kicks, %" PRIu64 " differ%s%s\n", s.regCompared, s.regKicks, s.regDiffer,
                    by.empty() ? "" : ":", by.c_str());
        if (s.opt.withEmu)
        {
            std::printf("(a) beside the machine: VU memory after %" PRIu64 " runs, %" PRIu64 " differ; vector registers after %" PRIu64 " runs, %" PRIu64
                        " differ (%" PRIu64 " lane values excluded: not reproduced, poisoned); VIF1 registers after %" PRIu64 " kicks, %" PRIu64
                        " differ; micro memory %" PRIu64 " differ\n",
                        s.memCompared, s.memDiffer, s.vfCompared, s.vfDiffer, s.vfPoisoned, s.kickVifCompared, s.kickVifDiffer, s.kickMicroDiffer);
            for (uint32_t p = 0; p < kVu1ProgramCount; ++p)
            {
                std::string lanes;
                for (uint32_t lane = 0; lane < 128; ++lane)
                    if (s.vfLaneDiffer[p][lane])
                        lanes += fmt(" VF%02u.%c x%" PRIu64, lane / 4, "xyzw"[lane & 3], s.vfLaneDiffer[p][lane]);
                if (!lanes.empty())
                    std::printf("(a) %s: lanes left different from the machine's after a run:%s\n", vu1Program(p).name, lanes.c_str());
                for (uint32_t lane = 0; lane < 128; ++lane)
                    if (s.vfLaneDiffer[p][lane])
                        std::printf("L\t%s\tVF%02u.%c\t%" PRIu64 "\n", vu1Program(p).name, lane / 4, "xyzw"[lane & 3], s.vfLaneDiffer[p][lane]);
            }
            std::printf("(b) beside the machine: %" PRIu64 " runs compared, %" PRIu64 " differ; %" PRIu64 " packets compared, %" PRIu64 " differ; %" PRIu64
                        " runs with another packet count; %" PRIu64 " kicks with another number of runs; %" PRIu64 " machine runs ended otherwise\n",
                        s.runsCompared, s.runsDiffer, s.packetsCompared, s.packetsDiffer, s.countDiffer, s.runMismatch, s.otherResults);
            std::printf("(b) runs the machine cut short at its cycle budget: %" PRIu64 " in %" PRIu64 " kicks; %" PRIu64
                        " reader packets beyond the cut left out, the machine's state taken after each%s",
                        s.cutRuns, s.cutKicks, s.cutPacketsLeftOut, s.cutList.empty() ? "\n" : ": kicks");
            if (!s.cutList.empty())
            {
                for (const std::string &c : s.cutList)
                    std::printf(" %s", c.c_str());
                std::printf("\n");
            }
            if (s.encodedKicks)
                std::printf("(c) %" PRIu64 " channel 1 kicks drawn from their K2 draw list encoded back (%" PRIu64 " primitives, %.1f MB, %" PRIu64
                            " registers written back, %" PRIu64 " left different)\n",
                            s.encodedKicks, s.encodedPrims, s.encodedBytes / 1048576.0, s.restored, s.unrestorable);
        }
        else
        {
            if (s.opt.fast)
                std::printf("(b) not available: the fast reader (--reader fast) keeps no PATH1 hash; check (e) is rc_replay --consumer lists\n");
            std::printf("(b) PATH1 bytes against the recorded GIF hash: %" PRIu64 "/%" PRIu64 " channel 1 kicks equal%s", s.hashOk, s.hashKicks,
                        s.hashFailKicks.empty() ? "\n" : "; differ:");
            if (!s.hashFailKicks.empty())
            {
                for (const std::string &c : s.hashFailKicks)
                    std::printf(" %s", c.c_str());
                std::printf("\n");
            }
        }
        for (const std::string &t : s.failText)
            std::printf("FAIL %s\n", t.c_str());
        if (s.failures > s.failText.size())
            std::printf("FAIL ... %" PRIu64 " failures in all\n", s.failures);
    }
}
