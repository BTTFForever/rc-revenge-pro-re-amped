// rc_replay's shapestat consumer: see shape_survey.h. Measurement for RENDERER_P2_DESIGN.md.

#include "shape_survey.h"

#include "chain_walk.h"
#include "shape_lean.h"
#include "../../render/vif/vif1_reader.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <emmintrin.h>
#include <x86intrin.h>

namespace rc::replay
{
    using namespace rc::rec;
    using rc::rd::Vu1Run;

    namespace
    {
        constexpr int kPrograms = 6;
        const char *const kNames[kPrograms] = {"trdr", "env", "water", "skin", "terdr", "rdr"};

        inline uint64_t mix(uint64_t h, uint64_t w)
        {
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            return h ^ (h >> 29);
        }
        uint64_t hashBytes(uint64_t h, const uint8_t *p, uint32_t n)
        {
            uint32_t i = 0;
            for (; i + 8 <= n; i += 8)
            {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h = mix(h, w);
            }
            if (i < n)
            {
                uint64_t w = 0;
                std::memcpy(&w, p + i, n - i);
                h = mix(h, w);
            }
            return h;
        }

        struct Instance // one unit called in a kick
        {
            uint32_t addr = 0, streamStart = 0, streamEnd = 0, bytes = 0, tags = 0, runs = 0, in = 0;
            uint64_t hash = 0;
        };

        struct Unit // a call target over the dump
        {
            uint64_t firstHash = 0, lastHash = 0;
            uint32_t calls = 0, kicks = 0, changes = 0, maxBytes = 0, firstKick = 0, lastKick = ~0u;
            uint64_t bytes = 0, runs = 0, in = 0, quick = 0, plainRuns = 0;
            uint32_t programs = 0;
        };
    }

    struct ShapeSurvey::Impl final : rc::rd::Path1Sink, WalkVisitor
    {
        rc::rd::Vif1Reader reader;
        bool started = false;
        uint32_t kickCsr = 0x1F80u;
        uint32_t kickIndex = 0; // channel 1 kicks so far
        uint32_t cycles = 0, cyclesWithKick = 0;
        bool cycleHadKick = false;

        // ---- units
        std::unordered_map<uint32_t, Unit> units;
        std::vector<Instance> instances; // this kick's, in stream order
        int depth = 0;
        int current = -1;
        bool pendingEnter = false, pendingLeave = false;
        uint32_t pendingAddr = 0;
        uint64_t tagsByDepth[3][8] = {};
        uint64_t inlineBytes = 0, refBytes[3] = {}, unitBytes = 0, nestedCalls = 0, streamBytes = 0;
        uint32_t kickInline = 0;
        size_t instanceCursor = 0;

        // ---- runs
        struct ProgramStats
        {
            uint64_t runs = 0, direct = 0, garbage = 0, blocks = 0, in = 0, culled = 0, quick = 0, allBehind = 0, nearCut = 0, guardNone = 0, guardFan = 0,
                     guardFanTriangles = 0, plainRuns = 0, plainIn = 0, plainQuick = 0, boxRuns = 0, boxIn = 0, boxQuick = 0, boxWrong = 0, boxTested = 0;
            uint64_t guardEarly = 0, hardRuns = 0, hardRunsIn = 0, hard = 0, fastBlocks = 0;
            uint64_t fastTicks = 0, fastIn = 0, fastQuick = 0, fastHard = 0, fastOdd = 0, fastCleanRuns = 0, fastCompared = 0, fastDiffer = 0, fastCountDiffer = 0,
                     fastClassDiffer = 0;
            uint64_t leanTicks = 0, packets = 0, packetBytes = 0, packetDiffer = 0, countDiffer = 0, memoryDiffer = 0, registerDiffer = 0, runsDiffer = 0;
        } prog[kPrograms];
        uint64_t constantChanges = 0, constantRuns = 0;
        uint64_t lastConstants = 0;
        std::unordered_set<uint64_t> kickConstants;
        uint64_t distinctConstants = 0;
        uint32_t maxRunsKick = 0, maxInKick = 0, kickRuns = 0, kickIn = 0;
        uint32_t maxDistinctConstants = 0, maxInstances = 0;
        uint64_t runsInUnits = 0, runsInline = 0, constantChangesInUnit = 0;
        uint32_t kickHard = 0, maxHardKick = 0, kickBlocks = 0, maxBlocksKick = 0;
        const Instance *lastRunInstance = nullptr;

        // ---- the run in progress
        std::vector<uint8_t> memLean = std::vector<uint8_t>(0x4000), memClass = std::vector<uint8_t>(0x4000), leanBytes;
        std::vector<uint32_t> leanSizes;
        alignas(16) float vfLean[32][4];
        size_t leanPacket = 0, leanOffset = 0;
        bool runDiffers = false;
        int runProgram = -1;
        lean::RunClass runClass;
        std::vector<uint8_t> fastOut;
        lean::FastCount fastCount;
        bool fastValid = false;
        uint32_t fastTriangle = 0;
        uint32_t printed = 0;
        uint64_t selfBad = 0, selfDone = 0;

        // WalkVisitor: the units of one kick
        void range(const WalkRange &r) override
        {
            if (r.isTag)
            {
                if (pendingEnter)
                {
                    pendingEnter = false;
                    if (++depth == 1)
                    {
                        Instance i;
                        i.addr = pendingAddr;
                        i.streamStart = r.streamOffset;
                        instances.push_back(i);
                        current = static_cast<int>(instances.size()) - 1;
                    }
                    else
                        ++nestedCalls;
                }
                if (pendingLeave)
                {
                    pendingLeave = false;
                    if (depth > 0 && --depth == 0 && current >= 0)
                    {
                        instances[current].streamEnd = r.streamOffset;
                        current = -1;
                    }
                }
                uint64_t tag;
                std::memcpy(&tag, r.data, 8);
                const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7u);
                ++tagsByDepth[std::min(depth, 2)][id];
                if (current >= 0)
                {
                    Instance &i = instances[current];
                    ++i.tags;
                    i.bytes += 16;
                    i.hash = hashBytes(i.hash, r.data, 16);
                }
                if (id == 5u)
                {
                    pendingEnter = true;
                    pendingAddr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFFu);
                }
                else if (id == 6u)
                    pendingLeave = true;
                lastTagId = id;
                return;
            }
            streamBytes += r.bytes;
            if (lastTagId == 3u || lastTagId == 4u)
                refBytes[std::min(depth, 2)] += r.bytes;
            if (current >= 0)
            {
                Instance &i = instances[current];
                i.bytes += r.bytes;
                i.hash = hashBytes(i.hash, r.data, r.bytes);
                unitBytes += r.bytes;
            }
            else
            {
                inlineBytes += r.bytes;
                kickInline += r.bytes;
            }
        }
        uint32_t lastTagId = 7;

        Instance *instanceAt(uint32_t offset)
        {
            while (instanceCursor < instances.size() && instances[instanceCursor].streamEnd != 0u && offset >= instances[instanceCursor].streamEnd)
                ++instanceCursor;
            if (instanceCursor < instances.size() && offset >= instances[instanceCursor].streamStart)
                return &instances[instanceCursor];
            return nullptr;
        }

        // Path1Sink
        void runBegin(const Vu1Run &run) override
        {
            runProgram = run.program >= 0 ? static_cast<int>(rc::rd::vu1Program(static_cast<uint32_t>(run.program)).program) : -1;
            runDiffers = false;
            leanBytes.clear();
            leanSizes.clear();
            leanPacket = 0;
            leanOffset = 0;
            if (runProgram < 0 || runProgram >= kPrograms)
                return;
            ProgramStats &s = prog[runProgram];
            ++s.runs;
            ++kickRuns;
            const uint8_t *vu = reader.vuData();

            // The constants a run starts with: quadwords 7-22 (water's are eleven further on, skin's bones come after).
            const uint32_t first = runProgram == 2 ? 18u : 7u;
            const uint64_t ch = hashBytes(static_cast<uint64_t>(runProgram) + 1u, vu + first * 16u, 16u * 16u);
            ++constantRuns;
            const bool constantsChanged = ch != lastConstants;
            if (constantsChanged)
                ++constantChanges;
            lastConstants = ch;
            kickConstants.insert(ch);

            std::memcpy(memClass.data(), vu, 0x4000);
            const uint32_t saved = _mm_getcsr();
            _mm_setcsr(lean::csr(kickCsr));
            lean::classify(runProgram, memClass.data(), reader.vf(), run.pc, run.top, runClass);
            _mm_setcsr(saved);
            const lean::RunClass &c = runClass;
            if (c.direct)
                ++s.direct;
            if (c.garbage)
                ++s.garbage;
            s.blocks += c.blocks;
            s.in += c.in;
            s.culled += c.culled;
            s.quick += c.quick;
            s.allBehind += c.allBehind;
            s.nearCut += c.nearCut;
            s.guardNone += c.guardNone;
            s.guardFan += c.guardFan;
            s.guardFanTriangles += c.guardFanTriangles;
            s.guardEarly += c.guardEarly;
            {
                const uint32_t hard = c.nearCut + c.guardNone + c.guardFan - c.guardEarly;
                s.hard += hard;
                kickHard += hard;
                kickBlocks += c.blocks;
                if (hard)
                {
                    ++s.hardRuns;
                    s.hardRunsIn += c.in;
                }
            }
            kickIn += c.in;
            if (!c.direct && c.plain)
            {
                ++s.plainRuns;
                s.plainIn += c.in;
                s.plainQuick += c.quick;
            }
            if (c.boxTested)
            {
                ++s.boxTested;
                if (c.boxPlain)
                {
                    ++s.boxRuns;
                    s.boxIn += c.in;
                    s.boxQuick += c.quick;
                    if (!c.plain)
                        ++s.boxWrong;
                }
            }
            Instance *const inst = instanceAt(run.streamOffset);
            if (inst && inst == lastRunInstance && constantsChanged)
                ++constantChangesInUnit;
            lastRunInstance = inst;
            if (Instance *i = inst)
            {
                ++i->runs;
                i->in += c.in;
                ++runsInUnits;
                Unit &u = units[i->addr];
                u.programs |= 1u << runProgram;
                u.quick += c.quick;
                if (!c.direct && c.plain)
                    ++u.plainRuns;
            }
            else
                ++runsInline;

            // The track program's common path with SSE (shape_lean.cpp), on the run's input as it stands.
            fastValid = false;
            fastTriangle = 0;
            if (runProgram == 0 && run.pc == 0u)
            {
                fastOut.clear();
                fastCount = lean::FastCount{};
                _mm_setcsr(lean::csr(kickCsr));
                const uint64_t f0 = __rdtsc();
                lean::fastTrdr(vu, reader.vf(), run.top, fastOut, fastCount);
                s.fastTicks += __rdtsc() - f0;
                _mm_setcsr(saved);
                s.fastIn += fastCount.in;
                s.fastQuick += fastCount.quick;
                s.fastHard += fastCount.hard;
                s.fastOdd += fastCount.odd;
                s.fastBlocks += fastCount.blocks;
                kickBlocks += fastCount.blocks;
                fastValid = fastCount.hard == 0;
                if (fastValid)
                    ++s.fastCleanRuns;
                if (fastCount.in != c.in || fastCount.culled != c.culled || fastCount.quick + fastCount.hard != c.in - c.culled - c.allBehind - c.guardEarly ||
                    fastCount.allBehind != c.allBehind || fastCount.guardEarly != c.guardEarly)
                    ++s.fastClassDiffer;
            }

            // The lean build of the same run, on copies.
            std::memcpy(memLean.data(), vu, 0x4000);
            std::memcpy(vfLean, reader.vf(), sizeof(vfLean));
            _mm_setcsr(lean::csr(kickCsr));
            const uint64_t t0 = __rdtsc();
            lean::run(runProgram, memLean.data(), vfLean, run.pc, run.top, leanBytes, leanSizes);
            s.leanTicks += __rdtsc() - t0;
            _mm_setcsr(saved);
        }

        void packet(const uint8_t *data, uint32_t bytes) override
        {
            if (runProgram < 0 || runProgram >= kPrograms)
                return;
            ProgramStats &s = prog[runProgram];
            ++s.packets;
            s.packetBytes += bytes;
            if (fastValid && bytes == 160u)
            {
                ++s.fastCompared;
                if (fastTriangle >= fastCount.quick || std::memcmp(fastOut.data() + static_cast<size_t>(fastTriangle) * 144u, data + 16, 144) != 0)
                {
                    ++s.fastDiffer;
                    if (printed < 12 && fastTriangle < fastCount.quick)
                    {
                        ++printed;
                        const uint8_t *f = fastOut.data() + static_cast<size_t>(fastTriangle) * 144u;
                        uint32_t at = 0;
                        while (at < 144u && f[at] == data[16 + at])
                            ++at;
                        uint32_t a = 0, b = 0;
                        std::memcpy(&a, data + 16 + at / 4 * 4, 4);
                        std::memcpy(&b, f + at / 4 * 4, 4);
                        std::printf("shapestat: fast differs: kick %u triangle %u quadword %u lane %u: reference %08x fast %08x\n", kickIndex, fastTriangle, at / 16,
                                    at / 4 % 4, a, b);
                    }
                }
                ++fastTriangle;
            }
            if (leanPacket >= leanSizes.size())
            {
                runDiffers = true;
                ++leanPacket;
                return;
            }
            const uint32_t n = leanSizes[leanPacket++];
            if (n != bytes || std::memcmp(leanBytes.data() + leanOffset, data, bytes) != 0)
            {
                ++s.packetDiffer;
                runDiffers = true;
                if (printed < 12)
                {
                    ++printed;
                    uint32_t at = 0;
                    while (at < std::min(n, bytes) && leanBytes[leanOffset + at] == data[at])
                        ++at;
                    uint32_t a = 0, b = 0;
                    if (at / 4 * 4 + 4 <= std::min(n, bytes))
                    {
                        std::memcpy(&a, data + at / 4 * 4, 4);
                        std::memcpy(&b, leanBytes.data() + leanOffset + at / 4 * 4, 4);
                    }
                    std::printf("shapestat: lean differs: kick %u %s packet %zu bytes %u/%u at byte %u: reference %08x lean %08x\n", kickIndex,
                                kNames[runProgram], leanPacket - 1, bytes, n, at, a, b);
                }
            }
            leanOffset += n;
        }

        void runEnd(const Vu1Run &, uint8_t *vuData, float (*vf)[4]) override
        {
            if (runProgram < 0 || runProgram >= kPrograms)
                return;
            ProgramStats &s = prog[runProgram];
            if (fastValid && fastTriangle != fastCount.quick)
                ++s.fastCountDiffer;
            if (leanPacket != leanSizes.size())
            {
                ++s.countDiffer;
                runDiffers = true;
            }
            if (std::memcmp(vuData, memLean.data(), 0x4000) != 0)
            {
                ++s.memoryDiffer;
                runDiffers = true;
            }
            if (std::memcmp(vf, vfLean, sizeof(vfLean)) != 0)
            {
                ++s.registerDiffer;
                runDiffers = true;
            }
            if (runDiffers)
                ++s.runsDiffer;
        }

        void closeKick()
        {
            for (const Instance &i : instances)
            {
                Unit &u = units[i.addr];
                if (u.calls == 0)
                {
                    u.firstHash = i.hash;
                    u.firstKick = kickIndex;
                }
                else if (i.hash != u.lastHash)
                    ++u.changes;
                u.lastHash = i.hash;
                ++u.calls;
                if (u.lastKick != kickIndex)
                {
                    ++u.kicks;
                    u.lastKick = kickIndex;
                }
                u.bytes += i.bytes;
                u.maxBytes = std::max(u.maxBytes, i.bytes);
                u.runs += i.runs;
                u.in += i.in;
            }
            maxInstances = std::max<uint32_t>(maxInstances, static_cast<uint32_t>(instances.size()));
            maxRunsKick = std::max(maxRunsKick, kickRuns);
            maxInKick = std::max(maxInKick, kickIn);
            maxHardKick = std::max(maxHardKick, kickHard);
            maxBlocksKick = std::max(maxBlocksKick, kickBlocks);
            distinctConstants += kickConstants.size();
            maxDistinctConstants = std::max<uint32_t>(maxDistinctConstants, static_cast<uint32_t>(kickConstants.size()));
        }
    };

    ShapeSurvey::ShapeSurvey() : m(new Impl)
    {
        m->reader.setPoison(false);
        m->selfBad = lean::selfTest(4000000, &m->selfDone);
    }
    ShapeSurvey::~ShapeSurvey() = default;

    void ShapeSurvey::open(const FileInfo &) {}

    void ShapeSurvey::keyframe(const Keyframe &kf)
    {
        if ((kf.header.flags & kKeyFinal) == 0 && (!m->started || (kf.header.flags & kKeyAfterStateLoad)))
        {
            if (!m->reader.seed(kf))
                m->reader.reset();
            m->started = true;
        }
    }

    void ShapeSurvey::hw(uint32_t address, uint32_t, uint64_t value, uint32_t)
    {
        const uint32_t phys = address & 0x1FFFFFFFu;
        if (phys >= 0x10003C00u && phys < 0x10003E00u)
            m->reader.hw(phys, value);
    }

    void ShapeSurvey::kick(const KickView &k, const RamView &ram)
    {
        if (k.channel != 1u)
            return;
        Impl &s = *m;
        s.cycleHadKick = true;
        s.instances.clear();
        s.depth = 0;
        s.current = -1;
        s.pendingEnter = s.pendingLeave = false;
        s.lastTagId = 7;
        s.kickInline = 0;
        s.instanceCursor = 0;
        s.kickRuns = s.kickIn = 0;
        s.kickHard = s.kickBlocks = 0;
        s.lastRunInstance = nullptr;
        s.kickConstants.clear();
        s.lastConstants = 0;
        walkChain(k, ram, &s);
        if (s.current >= 0)
            s.instances[s.current].streamEnd = ~0u;
        s.kickCsr = k.mxcsr;
        s.reader.kick(k, ram, k.mxcsr, s);
        s.closeKick();
        ++s.kickIndex;
    }

    void ShapeSurvey::cycleEnd(const CycleInfo &, const uint8_t *)
    {
        ++m->cycles;
        if (m->cycleHadKick)
            ++m->cyclesWithKick;
        m->cycleHadKick = false;
    }

    void ShapeSurvey::summary() const
    {
        const Impl &s = *m;
        const auto line = [](const char *cat, const char *key, uint64_t n, uint64_t mm = 0) { std::printf("V %s\t%s\t%" PRIu64 "\t%" PRIu64 "\n", cat, key, n, mm); };
        const auto top = [](const char *cat, const char *key, uint64_t n) { std::printf("X %s\t%s\t%" PRIu64 "\n", cat, key, n); };
        std::printf("shapestat: %u channel 1 kicks; lean operations self-test: %" PRIu64 " of %" PRIu64 " results differ from the reference's\n", s.kickIndex,
                    s.selfBad, s.selfDone);
        line("kicks", "vif1", s.kickIndex);
        line("cycles", "all, with a channel 1 kick", s.cycles, s.cyclesWithKick);
        line("selftest", "differ", s.selfBad, s.selfDone);

        // ---- the stream
        line("stream", "bytes", s.streamBytes);
        line("stream", "inline (depth 0) bytes", s.inlineBytes);
        line("stream", "unit bytes", s.unitBytes);
        line("stream", "nested calls", s.nestedCalls);
        for (int d = 0; d < 3; ++d)
        {
            static const char *const ids[8] = {"refe", "cnt", "next", "ref", "refs", "call", "ret", "end"};
            for (int id = 0; id < 8; ++id)
                if (s.tagsByDepth[d][id])
                {
                    char key[32];
                    std::snprintf(key, sizeof(key), "depth %d %s", d, ids[id]);
                    line("tags", key, s.tagsByDepth[d][id]);
                }
            char key[32];
            std::snprintf(key, sizeof(key), "ref bytes depth %d", d);
            line("stream", key, s.refBytes[d]);
        }

        // ---- units: fixed (the same bytes at every call) against rewritten
        uint64_t fixedN = 0, fixedBytes = 0, fixedCallBytes = 0, fixedCalls = 0, fixedRuns = 0, fixedIn = 0, fixedLate = 0, fixedLateBytes = 0, onceN = 0, onceBytes = 0,
                 onceRuns = 0, onceIn = 0, dynN = 0, dynCalls = 0, dynCallBytes = 0, dynRuns = 0, dynIn = 0, dynMaxBytes = 0, fixedNoRuns = 0, fixedNoRunsBytes = 0;
        std::map<uint32_t, uint64_t> lateByKick;
        uint64_t fixedByProgram[kPrograms] = {}, dynByProgram[kPrograms] = {};
        for (const auto &[addr, u] : s.units)
        {
            (void)addr;
            const bool fixed = u.changes == 0;
            if (fixed && u.kicks >= 2)
            {
                ++fixedN;
                fixedBytes += u.maxBytes;
                fixedCallBytes += u.bytes;
                fixedCalls += u.calls;
                fixedRuns += u.runs;
                fixedIn += u.in;
                if (u.runs == 0)
                {
                    ++fixedNoRuns;
                    fixedNoRunsBytes += u.maxBytes;
                }
                if (u.firstKick > 0)
                {
                    ++fixedLate;
                    fixedLateBytes += u.maxBytes;
                    lateByKick[u.firstKick] += u.maxBytes;
                }
                for (int p = 0; p < kPrograms; ++p)
                    if (u.programs & (1u << p))
                        fixedByProgram[p] += u.runs;
            }
            else if (fixed)
            {
                ++onceN;
                onceBytes += u.bytes;
                onceRuns += u.runs;
                onceIn += u.in;
            }
            else
            {
                ++dynN;
                dynCalls += u.calls;
                dynCallBytes += u.bytes;
                dynRuns += u.runs;
                dynIn += u.in;
                dynMaxBytes += u.maxBytes;
                for (int p = 0; p < kPrograms; ++p)
                    if (u.programs & (1u << p))
                        dynByProgram[p] += u.runs;
            }
        }
        line("units", "fixed: addresses", fixedN, fixedBytes);
        line("units", "fixed: calls, bytes delivered", fixedCalls, fixedCallBytes);
        line("units", "fixed: runs, triangles read", fixedRuns, fixedIn);
        line("units", "fixed without a run: addresses, bytes", fixedNoRuns, fixedNoRunsBytes);
        line("units", "fixed first called after the first kick: addresses, bytes", fixedLate, fixedLateBytes);
        uint64_t lateMax = 0;
        for (const auto &[kick, bytes] : lateByKick)
        {
            (void)kick;
            lateMax = std::max(lateMax, bytes);
        }
        top("units", "fixed bytes first called in one later kick", lateMax);
        line("units", "called in one kick only: addresses, bytes", onceN, onceBytes);
        line("units", "called in one kick only: runs, triangles read", onceRuns, onceIn);
        line("units", "rewritten: addresses, largest sizes summed", dynN, dynMaxBytes);
        line("units", "rewritten: calls, bytes delivered", dynCalls, dynCallBytes);
        line("units", "rewritten: runs, triangles read", dynRuns, dynIn);
        line("units", "runs inside a unit / at depth 0", s.runsInUnits, s.runsInline);
        top("units", "called in a kick", s.maxInstances);
        for (int p = 0; p < kPrograms; ++p)
            if (fixedByProgram[p] || dynByProgram[p])
            {
                char key[48];
                std::snprintf(key, sizeof(key), "%s runs in fixed / rewritten units", kNames[p]);
                line("units", key, fixedByProgram[p], dynByProgram[p]);
            }

        // ---- runs
        line("constants", "runs, changes from the run before", s.constantRuns, s.constantChanges);
        line("constants", "distinct in a kick, summed", s.distinctConstants);
        top("constants", "distinct in a kick", s.maxDistinctConstants);
        top("runs", "in a kick", s.maxRunsKick);
        top("triangles", "read in a kick", s.maxInKick);
        top("triangles", "hard in a kick", s.maxHardKick);
        top("blocks", "texture blocks in a kick", s.maxBlocksKick);
        line("constants", "changes between two runs of one unit call", s.constantChangesInUnit);
        const double ns = s.reader.nsPerTick();
        uint64_t leanTicks = 0;
        for (int p = 0; p < kPrograms; ++p)
        {
            const Impl::ProgramStats &q = s.prog[p];
            if (!q.runs)
                continue;
            leanTicks += q.leanTicks;
            const auto pl = [&](const char *key, uint64_t n, uint64_t mm = 0)
            {
                char cat[32];
                std::snprintf(cat, sizeof(cat), "prog %s", kNames[p]);
                line(cat, key, n, mm);
            };
            pl("runs, direct entries", q.runs, q.direct);
            pl("texture blocks, garbage runs", q.blocks, q.garbage);
            pl("triangles read, culled by facing", q.in, q.culled);
            pl("quick path", q.quick);
            pl("all behind the near plane", q.allBehind);
            pl("near-plane paths", q.nearCut);
            pl("guard band: nothing drawn / fans", q.guardNone, q.guardFan);
            pl("guard band: fan triangles", q.guardFanTriangles);
            pl("guard band: all corners beyond one edge", q.guardEarly);
            pl("hard triangles (near-plane paths, the clipper)", q.hard);
            pl("runs with a hard triangle, their triangles read", q.hardRuns, q.hardRunsIn);
            pl("plain runs, their triangles read", q.plainRuns, q.plainIn);
            pl("plain runs: quick triangles", q.plainQuick);
            pl("box test: runs tested, said plain", q.boxTested, q.boxRuns);
            pl("box test: triangles read, quick in runs it said plain", q.boxIn, q.boxQuick);
            pl("box test: said plain, was not", q.boxWrong);
            pl("packets, bytes", q.packets, q.packetBytes);
            pl("lean: packets differing, runs with another count", q.packetDiffer, q.countDiffer);
            pl("lean: runs leaving other VU memory / other registers", q.memoryDiffer, q.registerDiffer);
            pl("lean: runs differing in any way", q.runsDiffer);
            pl("lean: nanoseconds", static_cast<uint64_t>(static_cast<double>(q.leanTicks) * ns));
            if (q.fastIn)
            {
                pl("fast: triangles read, quick", q.fastIn, q.fastQuick);
                pl("fast: hard, of them values the host mode cannot treat", q.fastHard, q.fastOdd);
                pl("fast: runs without a hard triangle", q.fastCleanRuns);
                pl("fast: triangles compared with the reference's packets, differing", q.fastCompared, q.fastDiffer);
                pl("fast: runs with another triangle count / another classification", q.fastCountDiffer, q.fastClassDiffer);
                pl("fast: texture blocks", q.fastBlocks);
                pl("fast: nanoseconds", static_cast<uint64_t>(static_cast<double>(q.fastTicks) * ns));
            }
        }
        const rc::rd::Vif1Reader::Stats &st = s.reader.stats();
        line("time", "references, ns", static_cast<uint64_t>(static_cast<double>(st.refNs) * ns));
        line("time", "lean references, ns", static_cast<uint64_t>(static_cast<double>(leanTicks) * ns));
        line("time", "VIF walk, ns", static_cast<uint64_t>(static_cast<double>(st.walkNs) * ns));
        if (s.cyclesWithKick)
        {
            const double per = ns / 1e6 / s.cyclesWithKick;
            std::printf("shapestat: a cycle with a channel 1 kick (%u of %u): references %.2f ms, lean %.2f ms; the track program %.2f ms lean, %.2f ms its common "
                        "path with SSE; VIF walk %.2f ms\n",
                        s.cyclesWithKick, s.cycles, static_cast<double>(st.refNs) * per, static_cast<double>(leanTicks) * per,
                        static_cast<double>(s.prog[0].leanTicks) * per, static_cast<double>(s.prog[0].fastTicks) * per, static_cast<double>(st.walkNs) * per);
        }
    }
}
