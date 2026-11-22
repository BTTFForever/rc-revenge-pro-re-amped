// rc_replay's lists consumer: check (e), see lists.h.
#include "lists.h"

#include "dlst.h"

#include "../../render/frame/dlst.h"
#include "../../render/frame/frame_reader.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rc::rec;
using namespace rc::rd;

namespace rc::replay
{
    namespace
    {
        // The six lanes of RENDERER_3D_DESIGN.md 3.4 (register * 4 + lane).
        constexpr uint32_t kLanes[6] = {5 * 4 + 3, 6 * 4 + 3, 7 * 4 + 3, 25 * 4 + 3, 26 * 4 + 3, 29 * 4 + 3};
        constexpr const char *kLaneNames[6] = {"VF05.w", "VF06.w", "VF07.w", "VF25.w", "VF26.w", "VF29.w"};

        uint8_t faultCode(const std::string &name)
        {
            if (name == "adc")
                return Gif2dReader::Options::kFastFaultAdc;
            if (name == "clut-memo")
                return Gif2dReader::Options::kFastFaultClutMemo;
            if (name == "state-memo")
                return Gif2dReader::Options::kFastFaultStateMemo;
            if (name == "cull")
                return Gif2dReader::Options::kFastFaultCull;
            return Gif2dReader::Options::kFastFaultNone;
        }

        FrameReader::Options readerOptions(const ListsOptions &o, bool fast)
        {
            FrameReader::Options f;
            f.gif.seed = !o.fromReset;   // GSRG at the first keyframe (the chained replay's way); from boot nothing is seeded
            f.gif.cullOffscreen = true; // as the live reader and the chained replay
            f.gif.path1Prims = true;
            f.gif.frameUploads = true;
            f.fast = fast;
            rc::shape::FastFault path = rc::shape::FastFault::None;
            uint8_t programs = 0xFF;
            if (fast && !o.fault.empty() && rc::shape::parseFastFault(o.fault.c_str(), path, &programs))
            {
                f.fastFault = path;
                f.fastFaultPrograms = programs;
                f.fastFaultCycle = o.faultCycle;
            }
            else if (fast && !o.fault.empty())
            {
                f.gif.fastFault = faultCode(o.fault);
                f.gif.fastFaultCycle = o.faultCycle;
            }
            return f;
        }

        double msOf(std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
    }

    bool parseListsFault(const std::string &name)
    {
        rc::shape::FastFault f;
        return faultCode(name) != Gif2dReader::Options::kFastFaultNone || rc::shape::parseFastFault(name.c_str(), f);
    }

    struct ListsCheck::Impl
    {
        ListsOptions opt;
        FrameReader strict, fast;

        // The VU registers after each run of the strict reader's kick, then compared run by run with the fast reader's.
        struct Lanes
        {
            uint32_t v[128];
        };
        struct StrictTap final : Path1Tap
        {
            std::vector<Lanes> *runs = nullptr;
            bool packet(const uint8_t *, uint32_t) override { return true; }
            bool fastTriangles() const override { return true; }
            bool readsVuMemory() const override { return false; }
            void runEnd(const Vu1Run &, uint8_t *, float (*vf)[4]) override
            {
                Lanes l;
                std::memcpy(l.v, vf, sizeof(l.v));
                runs->push_back(l);
            }
        } strictTap;
        struct FastTap final : Path1Tap
        {
            Impl *m = nullptr;
            const KickView *k = nullptr;
            size_t ri = 0;
            bool packet(const uint8_t *, uint32_t) override { return true; }
            bool fastTriangles() const override { return true; }
            bool readsVuMemory() const override { return false; }
            void runEnd(const Vu1Run &run, uint8_t *, float (*vf)[4]) override
            {
                ++m->runs;
                if (ri >= m->strictRuns.size())
                {
                    ++ri;
                    return;
                }
                const Lanes &s = m->strictRuns[ri++];
                uint32_t f[128];
                std::memcpy(f, vf, sizeof(f));
                bool six = false;
                for (uint32_t i = 0; i < 6; ++i)
                    if (s.v[kLanes[i]] != f[kLanes[i]])
                    {
                        if (!six)
                            m->fail("cycle " + std::to_string(k->cycle) + " kick " + std::to_string(k->index) + " run " + std::to_string(run.index) +
                                    " (" + rc::vu1::programName(vu1Program(static_cast<uint32_t>(run.program)).program) + "): " + kLaneNames[i] +
                                    " strict " + hex(s.v[kLanes[i]]) + ", fast " + hex(f[kLanes[i]]));
                        six = true;
                    }
                if (six)
                    ++m->runsSixDiffer;
                else if (std::memcmp(s.v, f, sizeof(f)) != 0)
                    ++m->runsOtherDiffer;
            }
            static std::string hex(uint32_t v)
            {
                char t[16];
                std::snprintf(t, sizeof t, "%08x", v);
                return t;
            }
        } fastTap;
        std::vector<Lanes> strictRuns;

        // Counts.
        uint64_t cycles = 0, cyclesEqual = 0, cyclesDiffer = 0, commands = 0, listBytes = 0;
        uint64_t path1Kicks = 0, path3Kicks = 0, guardedStrict = 0, guardedFast = 0, guardDiffer = 0;
        uint64_t regKicks = 0, regKicksDiffer = 0, runs = 0, runsSixDiffer = 0, runsOtherDiffer = 0, runCountDiffer = 0;
        int64_t firstFailCycle = -1;
        std::string firstFailWhat;
        double strictMs = 0.0, fastMs = 0.0, strictMaxMs = 0.0, fastMaxMs = 0.0;
        uint64_t cycleNow = 0;

        std::vector<std::string> failures;
        uint64_t failuresDropped = 0;
        std::vector<uint8_t> encS, encF;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

        explicit Impl(const ListsOptions &o) : opt(o), strict(readerOptions(o, false)), fast(readerOptions(o, true))
        {
            strictTap.runs = &strictRuns;
            fastTap.m = this;
            if (o.fromReset)
            {
                strict.startLive();
                fast.startLive();
            }
        }

        void fail(const std::string &line)
        {
            if (firstFailCycle < 0)
            {
                firstFailCycle = static_cast<int64_t>(cycleNow);
                firstFailWhat = line;
            }
            if (failures.size() < 16u)
                failures.push_back(line);
            else
                ++failuresDropped;
            if (!opt.quiet)
                std::printf("DIFFERS: %s\n", line.c_str());
        }

        void compareRegs(const KickView &k)
        {
            ++regKicks;
            const Gif2dReader &a = strict.gif(), &b = fast.gif();
            for (uint32_t r = 0; r < kGsRegFile; ++r)
                if (a.regs()[r] != b.regs()[r] || a.provenance()[r] != b.provenance()[r])
                {
                    ++regKicksDiffer;
                    char t[200];
                    std::snprintf(t, sizeof t, "cycle %" PRIu64 " kick %u (ch%u): GS register 0x%02x strict %016" PRIx64 " (provenance %u), fast %016" PRIx64
                                               " (provenance %u)",
                                  k.cycle, k.index, k.channel, r, a.regs()[r], a.provenance()[r], b.regs()[r], b.provenance()[r]);
                    fail(t);
                    return;
                }
        }
    };

    ListsCheck::ListsCheck(const ListsOptions &options) : m(std::make_unique<Impl>(options)) {}

    ListsCheck::~ListsCheck() = default;

    void ListsCheck::open(const FileInfo &info)
    {
        m->strict.open(info);
        m->fast.open(info);
    }

    void ListsCheck::keyframe(const Keyframe &kf)
    {
        // From boot: as the live reader, only a keyframe after a save-state load. Otherwise every keyframe (each reader
        // takes the first, and one after a load).
        if (m->opt.fromReset && !(kf.header.flags & kKeyAfterStateLoad))
            return;
        m->strict.keyframe(kf);
        m->fast.keyframe(kf);
    }

    void ListsCheck::cycleBegin(uint64_t cycle)
    {
        m->cycleNow = cycle;
        m->strict.cycleBegin(cycle);
        m->fast.cycleBegin(cycle);
    }

    void ListsCheck::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa)
    {
        m->strict.hw(address, width, value, callerRa);
        m->fast.hw(address, width, value, callerRa);
    }

    void ListsCheck::kick(const KickView &k, const RamView &ram)
    {
        Impl &s = *m;
        const bool ch1 = k.channel == 1u;
        const uint64_t gs = s.strict.stats().guardedKicks, gf = s.fast.stats().guardedKicks;
        if (ch1)
        {
            s.strictRuns.clear();
            s.strict.setPath1Tap(&s.strictTap);
            s.fastTap.k = &k;
            s.fastTap.ri = 0;
            s.fast.setPath1Tap(&s.fastTap);
        }
        auto t0 = std::chrono::steady_clock::now();
        s.strict.kick(k, ram);
        auto t1 = std::chrono::steady_clock::now();
        s.fast.kick(k, ram);
        auto t2 = std::chrono::steady_clock::now();
        s.strict.setPath1Tap(nullptr);
        s.fast.setPath1Tap(nullptr);
        if (ch1)
        {
            ++s.path1Kicks;
            const double a = msOf(t1 - t0), b = msOf(t2 - t1);
            s.strictMs += a;
            s.fastMs += b;
            s.strictMaxMs = std::max(s.strictMaxMs, a);
            s.fastMaxMs = std::max(s.fastMaxMs, b);
            if (s.fastTap.ri != s.strictRuns.size())
            {
                ++s.runCountDiffer;
                s.fail("cycle " + std::to_string(k.cycle) + " kick " + std::to_string(k.index) + ": the strict reader ran " + std::to_string(s.strictRuns.size()) +
                       " programs, the fast " + std::to_string(s.fastTap.ri));
            }
            const bool guardS = s.strict.stats().guardedKicks != gs, guardF = s.fast.stats().guardedKicks != gf;
            s.guardedStrict += guardS;
            s.guardedFast += guardF;
            if (guardS != guardF)
            {
                ++s.guardDiffer;
                s.fail("cycle " + std::to_string(k.cycle) + " kick " + std::to_string(k.index) + ": a guard stopped the " + (guardS ? "strict" : "fast") +
                       " reader only");
            }
        }
        else if (k.channel == 2u)
            ++s.path3Kicks;
        if (s.strict.started() && s.fast.started())
            s.compareRegs(k);
    }

    void ListsCheck::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        Impl &s = *m;
        s.strict.cycleEnd(info, refRgba);
        s.fast.cycleEnd(info, refRgba);
        ++s.cycles;
        const uint64_t cycle = info.end.cycle;
        encodeDlst(s.strict.list(), cycle, static_cast<uint32_t>(s.strict.path1Ranges().size() / 2u), s.encS);
        encodeDlst(s.fast.list(), cycle, static_cast<uint32_t>(s.fast.path1Ranges().size() / 2u), s.encF);
        s.commands += s.strict.list().cmds.size();
        s.listBytes += s.encS.size();
        if (s.encS.size() == s.encF.size() && std::memcmp(s.encS.data(), s.encF.data(), s.encS.size()) == 0)
        {
            ++s.cyclesEqual;
            return;
        }
        ++s.cyclesDiffer;
        s.fail("cycle " + std::to_string(cycle) + ": the lists differ: " +
               dlstFirstDifference(s.encS.data(), s.encS.size(), s.encF.data(), s.encF.size(), "strict / fast"));
    }

    bool ListsCheck::failed() const
    {
        return m->cyclesDiffer || m->regKicksDiffer || m->runsSixDiffer || m->runCountDiffer || m->guardDiffer || m->cyclesEqual != m->cycles || m->cycles == 0;
    }

    void ListsCheck::summary()
    {
        Impl &s = *m;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t0).count();
        std::printf("lists: %s, %" PRIu64 " cycles: %" PRIu64 " lists equal byte for byte, %" PRIu64 " differ; %" PRIu64 " commands, %.1f MB encoded\n",
                    s.opt.fromReset ? "from boot (both readers from reset)" : "seeded at the first keyframe", s.cycles, s.cyclesEqual, s.cyclesDiffer,
                    s.commands, s.listBytes / 1048576.0);
        std::printf("lists: %" PRIu64 " VIF1 kicks (guarded: strict %" PRIu64 ", fast %" PRIu64 ", differing %" PRIu64 "), %" PRIu64
                    " PATH3 kicks; register file at %" PRIu64 " kicks: %" PRIu64 " differ\n",
                    s.path1Kicks, s.guardedStrict, s.guardedFast, s.guardDiffer, s.path3Kicks, s.regKicks, s.regKicksDiffer);
        std::printf("lists: %" PRIu64 " VU1 runs compared: the six lanes differ after %" PRIu64 ", other lanes after %" PRIu64 " (not a failure); %" PRIu64
                    " kicks with another run count\n",
                    s.runs, s.runsSixDiffer, s.runsOtherDiffer, s.runCountDiffer);
        const Gif2dReader::Stats &gs = s.strict.gif().stats(), &gf = s.fast.gif().stats();
        const Vif1Reader::Stats &vf = s.fast.vif().stats();
        std::printf("lists: fast reader: %" PRIu64 " PATH1 packets: %" PRIu64 " triangles straight into the list (%" PRIu64 " recognised but walked), %" PRIu64
                    " flushes dropped, %" PRIu64 " state packets skipped by the memo, %" PRIu64 " other packets walked; %" PRIu64 " direct-entry runs; CLUT loads %" PRIu64
                    " (strict %" PRIu64 "), %" PRIu64 " of them from the load memo, %" PRIu64 " TEX0 writes answered by the memo; palette checks skipped %" PRIu64 "\n",
                    gf.path1Packets, gf.path1Triangles, gf.path1TrianglesWalked, gf.path1Flushes, gf.stateMemoHits, gf.path1Walked, vf.directRuns, gf.clutLoads,
                    gs.clutLoads, gf.clutLoadsFromMemo, gf.clutMemoHits, s.fast.gif().cache().stats().paletteChecksSkipped);
        const rc::shape::FastStats &fst = vf.fast;
        const rc::shape::FastCounts fs = fst.total();
        std::string byProgram;
        for (uint32_t p = 0; p < 6; ++p)
            if (fst.by[p].declined)
                byProgram += std::string(byProgram.empty() ? "" : ", ") + rc::vu1::programName(static_cast<rc::vu1::Program>(p)) + " " + std::to_string(fst.by[p].declined) +
                             (fst.by[p].declinedParse ? " (" + std::to_string(fst.by[p].declinedParse) + " by the parse test)" : "");
        std::printf("lists: fast paths: %" PRIu64 " runs (largest input %u quadwords), %" PRIu64 " declined (%s; of them %" PRIu64
                    " runs whose input does not parse within the buffer); %" PRIu64 " triangles read: %" PRIu64 " culled, %" PRIu64 " all behind, %" PRIu64
                    " beyond one edge, %" PRIu64 " quick (%" PRIu64 " straight into the list, %" PRIu64 " as packets), %" PRIu64 " hard (%" PRIu64
                    " of them for a value the mode cannot take), %" PRIu64 " last triangles to the step; %" PRIu64 " texture blocks; triangles into the list without a packet %" PRIu64 "\n",
                    fs.runs, fst.maxExtent, fs.declined, byProgram.empty() ? "none" : byProgram.c_str(), fs.declinedParse, fs.in, fs.culled, fs.allBehind, fs.edge,
                    fs.quick, fs.direct, fs.rebuilt, fs.hard, fs.odd, fs.lastSteps, fs.blocks, gf.path1FastTriangles);
        for (uint32_t p = 0; p < 6; ++p)
        {
            const rc::shape::FastCounts &c = fst.by[p];
            if (!c.runs && !c.declined)
                continue;
            std::printf("lists: fast path %s: %" PRIu64 " runs%s, %" PRIu64 " declined (%" PRIu64 " by the parse test); %" PRIu64 " triangles read: %" PRIu64
                        " culled, %" PRIu64 " all behind, %" PRIu64 " beyond one edge, %" PRIu64 " quick, %" PRIu64 " hard (%" PRIu64 " odd), %" PRIu64
                        " last to the step; %" PRIu64 " blocks\n",
                        rc::vu1::programName(static_cast<rc::vu1::Program>(p)), c.runs,
                        p == rc::vu1::kEnv ? (" (" + std::to_string(c.envRuns) + " env-mapped)").c_str() : "", c.declined, c.declinedParse, c.in, c.culled,
                        c.allBehind, c.edge, c.quick, c.hard, c.odd, c.lastSteps, c.blocks);
        }
        if (vf.views)
            std::printf("lists: %s\n", Vif1Reader::viewsLine(vf).c_str());
        const double tS = s.strict.vif().nsPerTick() / 1e6, tF = s.fast.vif().nsPerTick() / 1e6;
        const Vif1Reader::Stats &vs = s.strict.vif().stats();
        if (s.path1Kicks)
        {
            const double n = static_cast<double>(s.path1Kicks);
            std::printf("lists: time a VIF1 kick (mean, ms; one process, the two readers in turn): strict %.2f (largest %.2f) = references %.2f + VIF walk %.2f + "
                        "GIF walk and list %.2f; fast %.2f (largest %.2f) = references %.2f + VIF walk %.2f + GIF walk and list %.2f\n",
                        s.strictMs / n, s.strictMaxMs, vs.refNs * tS / n, vs.walkNs * tS / n, vs.sinkNs * tS / n, s.fastMs / n, s.fastMaxMs, vf.refNs * tF / n,
                        vf.walkNs * tF / n, vf.sinkNs * tF / n);
        }
        if (s.firstFailCycle >= 0)
            std::printf("lists: first failure at cycle %" PRId64 ": %s\n", s.firstFailCycle, s.firstFailWhat.c_str());
        for (const std::string &f : s.failures)
            std::printf("FAIL lists %s\n", f.c_str());
        if (s.failuresDropped)
            std::printf("FAIL lists ... and %" PRIu64 " more\n", s.failuresDropped);
        std::printf("lists: %.1f s\n", secs);
    }
}
