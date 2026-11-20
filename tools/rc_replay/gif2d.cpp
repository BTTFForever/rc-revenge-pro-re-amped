// rc_replay's gif2d consumer: see gif2d.h and RENDERER_2D_DESIGN.md section 10.
#include "gif2d.h"

#include "../../render/frame/frame_reader.h"
#include "../../render/gs2d/gif2d_reader.h"
#include "../../render/gs2d/gs2d_encode.h"
#include "../../render/gs2d/gs_plan.h"
// The software GS's own triangle set-up (header only): the tables' pixel counts are the reference's, as gif2dstat's.
#include "../../third_party/ps2recomp/ps2xRuntime/src/lib/gs/gs_triangle_rules.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace rc::rec;
using namespace rc::rd;

namespace rc::replay
{
    namespace
    {
        std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
        std::string fmt(const char *f, ...)
        {
            char buf[512];
            va_list ap;
            va_start(ap, f);
            std::vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }
        struct Count
        {
            uint64_t prims = 0, pixels = 0;
        };
        const char *blendName(uint8_t b) { return b == kBlendMix ? "MIX" : b == kBlendMixSub ? "MIXSUB" : "OFF"; }
        const char *coefName(uint8_t c)
        {
            static const char *const n[8] = {"0", "1", "F", "1-F", "-F", "1+F", "2", "-1"};
            return c < 8 ? n[c] : "?";
        }
    }

    struct Gif2dCheck::Impl
    {
        Gif2dOptions opt;
        // (step 8) The frame reader: PATH1 from the VIF1 reader into the same register file as PATH3, nothing excused.
        FrameReader fr;
        Gif2dReader &reader;
        uint64_t cycle = 0;

        // (a)
        uint64_t regKicks = 0, regCompared = 0, regMismatches = 0, mismatchByReg[0x80] = {};
        std::vector<std::string> mismatchText;
        bool havePath1Regs = false;
        uint64_t path1Regs[kGsRegFile] = {};
        uint64_t path1Pairs = 0, path1Changed[0x80] = {};

        // (b)
        struct Pending
        {
            bool valid = false;
            uint64_t cycle = 0;
            uint32_t index = 0;
            std::vector<uint8_t> gif;
        } pending;
        uint64_t encodedKicks = 0, substituted = 0, encodedWrites = 0, encodedPrims = 0, encodedStates = 0, restored = 0, unrestorable = 0;
        uint64_t notSubstitutedTransfers = 0, encodedBytes = 0;
        std::vector<std::string> unrestorableText;
        uint64_t faultPrimBase = 0;
        bool faultDone = false;

        // Tables
        std::map<std::string, std::map<std::string, Count>> hist;
        uint64_t largestList = 0, largestPayload = 0, cyclesWithList = 0;

        // (c)
        std::unique_ptr<Gif2dVkCheck> vk;
        // (d, step 8 batch 2) A channel 1 kick: GS memory before the machine ran it, and where its commands begin (the
        // vif consumer behind the machine appends them when it runs the kick).
        std::vector<uint8_t> path1Before;
        bool path1Pending = false;
        uint64_t path1Cycle = 0;
        uint32_t path1Index = 0, path1First = 0;

        explicit Impl(const Gif2dOptions &o) : opt(o), fr(makeFrameOptions(o)), reader(fr.gif())
        {
            if (o.vk)
                vk = std::make_unique<Gif2dVkCheck>(o.vkOptions);
        }
        static FrameReader::Options makeFrameOptions(const Gif2dOptions &o)
        {
            FrameReader::Options f;
            f.gif = makeReaderOptions(o);
            f.deferPath1 = o.withEmu; // beside the machine the vif consumer behind it runs channel 1 kicks (Emu::setPath1Substitute)
            return f;
        }
        static Gif2dReader::Options makeReaderOptions(const Gif2dOptions &o)
        {
            Gif2dReader::Options r;
            r.seed = true;
            r.textures = true;
            r.faultIgnoreTexa = o.vk && o.vkOptions.faultTexa;
            if (o.faultKind == "reg")
            {
                r.faultCycle = o.faultCycle;
                r.faultN = o.faultN;
            }
            return r;
        }
        void add(const char *cat, const std::string &key, uint64_t prims, uint64_t pixels)
        {
            Count &c = hist[cat][key];
            c.prims += prims;
            c.pixels += pixels;
        }
        uint64_t pixelsOf(const GsState &s, uint32_t kind, const GsVertex *v);
        void tables();
    };

    uint64_t Gif2dCheck::Impl::pixelsOf(const GsState &s, uint32_t kind, const GsVertex *v)
    {
        // As gif2dstat counts them (gif2d_survey.cpp): coverage by the reference's rules inside the scissor.
        const int ofx = static_cast<int>(s.xyoffset & 0xFFFFu), ofy = static_cast<int>((s.xyoffset >> 32) & 0xFFFFu);
        const int sx0 = static_cast<int>(s.scissor & 0x7FFu), sx1 = static_cast<int>((s.scissor >> 16) & 0x7FFu);
        const int sy0 = static_cast<int>((s.scissor >> 32) & 0x7FFu), sy1 = static_cast<int>((s.scissor >> 48) & 0x7FFu);
        if (kind == kKindTriangles)
        {
            GSTriangleRaster t;
            if (!gsTriangleSetup(v[0].x / 16.0f, v[0].y / 16.0f, v[1].x / 16.0f, v[1].y / 16.0f, v[2].x / 16.0f, v[2].y / 16.0f, ofx, ofy, t))
                return 0;
            uint64_t n = 0;
            for (int y = std::max(t.minY, sy0); y <= std::min(t.maxY, sy1); ++y)
                for (int x = std::max(t.minX, sx0); x <= std::min(t.maxX, sx1); ++x)
                {
                    const int64_t e[3] = {gsTriangleEdge(t, 0, x, y), gsTriangleEdge(t, 1, x, y), gsTriangleEdge(t, 2, x, y)};
                    n += gsTriangleCovers(t, e) ? 1u : 0u;
                }
            return n;
        }
        if (kind == kKindSprites)
        {
            const int xa = std::min<int>(v[0].x, v[1].x) - ofx, xb = std::max<int>(v[0].x, v[1].x) - ofx;
            const int ya = std::min<int>(v[0].y, v[1].y) - ofy, yb = std::max<int>(v[0].y, v[1].y) - ofy;
            const int bx0 = std::max((xa + 15) >> 4, sx0), bx1 = std::min(((xb + 15) >> 4) - 1, sx1);
            const int by0 = std::max((ya + 15) >> 4, sy0), by1 = std::min(((yb + 15) >> 4) - 1, sy1);
            return bx1 >= bx0 && by1 >= by0 ? static_cast<uint64_t>(bx1 - bx0 + 1) * (by1 - by0 + 1) : 0u;
        }
        if (kind == kKindLines)
        {
            const int xa = (v[0].x >> 4) - (ofx >> 4), ya = (v[0].y >> 4) - (ofy >> 4), xb = (v[1].x >> 4) - (ofx >> 4), yb = (v[1].y >> 4) - (ofy >> 4);
            return static_cast<uint64_t>(std::max(std::abs(xb - xa), std::abs(yb - ya))) + 1u;
        }
        return 1;
    }

    void Gif2dCheck::Impl::tables()
    {
        // RENDERER_2D_DESIGN.md section 3's tables from the reader's own list: the keys gif2dstat uses where the draw
        // list holds the information, and the plan of section 4 per draw state.
        static const char *const typeName[8] = {"point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"};
        const DrawList &l = reader.list();
        const GsState *cur = nullptr;
        for (const DrawCmd &c : l.cmds)
        {
            if (c.op == kOpState)
            {
                cur = &l.state(c);
                continue;
            }
            if (c.op == kOpTexture)
            {
                add("list", "TEXTURE commands", 1, c.payloadBytes);
                continue;
            }
            if ((c.op != kOpScreenPrims && c.op != kOpClear) || !cur)
                continue;
            const GsState &s = *cur;
            const uint32_t kind = c.op == kOpClear ? static_cast<uint32_t>(kKindSprites) : c.a, count = c.op == kOpClear ? 1u : c.b;
            const uint32_t per = verticesPerPrim(kind);
            const GsPlan p = planState(s);
            const uint32_t type = s.primType();
            const bool tme = s.tme(), iip = s.iip(), abe = s.abe(), zmsk = (s.zbuf >> 32) & 1u;
            const std::string alpha = abe ? fmt("%" PRIx64, s.alpha) : std::string("-");
            const Tex0 t = Tex0::unpack(s.tex0);
            const std::string texPart = tme ? fmt("psm=%02x tcc=%u tfx=%u tex1=%" PRIx64 " clamp=%" PRIx64, t.psm, t.tcc, t.tfx, s.tex1, s.clamp) : std::string("-");
            const std::string stateKey = fmt("%s prim=%03x alpha=%s test=%05" PRIx64 " pabe=%u fba=%u zbuf=%" PRIx64 " frame=fbw%u/psm%02x/msk%08x colclamp=%u dthe=%u ctx=%u tex[%s]",
                                             typeName[type], static_cast<unsigned>(s.prim), alpha.c_str(), s.test, s.pabe, s.fba, s.zbuf,
                                             static_cast<unsigned>((s.frame >> 16) & 0x3Fu), static_cast<unsigned>((s.frame >> 24) & 0x3Fu),
                                             static_cast<unsigned>(s.frame >> 32), s.colclamp, s.dthe, s.ctxt(), texPart.c_str());
            const std::string blendKey = fmt("%s%s%s alpha=%s test=%05" PRIx64 " zmsk=%u", typeName[type], tme ? " TME" : "", iip ? " IIP" : "",
                                             alpha.c_str(), s.test, zmsk);
            std::string planKey = fmt("%s term=%s ks=%s kd=%s%s%s", blendName(p.blend), p.term == kTermExact ? "exact" : "linear", coefName(p.ks),
                                      coefName(p.kd), p.exactBlend ? " exact" : " +1 possible", p.factor == kFactorFix ? fmt(" FIX %02x", p.fix).c_str() : p.factor == kFactorAs ? " F=As" : "");
            planKey += fmt(" atst=%u aref=%02x pass=%x/%u fail=%x/%u%s%s depth=%u", p.atst, p.aref, p.passColorMask, p.passDepthWrite, p.failColorMask,
                           p.failDepthWrite, p.split ? " SPLIT" : "", p.alphaByBlend ? " alpha-by-blend" : "", p.depthCompare);
            if (p.guards)
                for (uint32_t g = 1; g < kGuardCount; ++g)
                    if (p.guards & (1u << g))
                        planKey += std::string(" guard:") + gsGuardName(g);
            const int ofx = static_cast<int>(s.xyoffset & 0xFFFFu), ofy = static_cast<int>((s.xyoffset >> 32) & 0xFFFFu);
            const int sx0 = static_cast<int>(s.scissor & 0x7FFu), sx1 = static_cast<int>((s.scissor >> 16) & 0x7FFu);
            const int sy0 = static_cast<int>((s.scissor >> 32) & 0x7FFu), sy1 = static_cast<int>((s.scissor >> 48) & 0x7FFu);
            const GsVertex *v = l.vertices(c);
            uint64_t pixels = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const GsVertex *pv = v + i * per;
                const uint64_t px = pixelsOf(s, kind, pv);
                pixels += px;
                uint32_t zmin = 0xFFFFFFFFu, zmax = 0;
                for (uint32_t k = 0; k < per; ++k)
                {
                    zmin = std::min(zmin, pv[k].z);
                    zmax = std::max(zmax, pv[k].z);
                }
                if (kind == kKindSprites)
                    zmin = zmax = pv[1].z;
                add("z", fmt("%s", kind == kKindSprites ? "sprite" : "2D") + fmt(" z=%08x%s", zmin, zmin == zmax ? "" : " (varies in the primitive)"), 1, px);
            }
            add("state", stateKey, count, pixels);
            add("blend", blendKey, count, pixels);
            add("plan", blendKey + "  ->  " + planKey, count, pixels);
            add("reg", fmt("PRIM attributes iip=%u tme=%u fge=%u abe=%u aa1=%u fst=%u ctxt=%u fix=%u", iip, tme, s.fge(), abe, s.aa1(), s.fst(), s.ctxt(),
                           static_cast<unsigned>((s.prim >> 10) & 1u)),
                count, pixels);
            add("reg", fmt("SCISSOR %d..%d x %d..%d", sx0, sx1, sy0, sy1), count, pixels);
            add("reg", fmt("XYOFFSET %d.%d, %d.%d", ofx >> 4, ofx & 15, ofy >> 4, ofy & 15), count, pixels);
            add("reg", fmt("FRAME %" PRIx64, s.frame), count, pixels);
            add("reg", fmt("ZBUF %" PRIx64, s.zbuf), count, pixels);
            add("reg", fmt("TEST zte=%u ztst=%u date=%u", static_cast<unsigned>((s.test >> 16) & 1u), static_cast<unsigned>((s.test >> 17) & 3u),
                           static_cast<unsigned>((s.test >> 14) & 1u)),
                count, pixels);
            add("reg", fmt("PABE %u FBA %u COLCLAMP %u DTHE %u", s.pabe, s.fba, s.colclamp, s.dthe), count, pixels);
            if (tme)
            {
                add("reg", fmt("TEX1 %" PRIx64, s.tex1), count, pixels);
                add("reg", fmt("CLAMP %" PRIx64, s.clamp), count, pixels);
                add("reg", fmt("texture psm=%02x %ux%u tcc=%u tfx=%u", t.psm, t.width(), t.height(), t.tcc, t.tfx), count, pixels);
                add("tex", fmt("TEXA %" PRIx64 " (stored where the format reads it: psm %02x cpsm %02x)", s.texa, t.psm, t.cpsm), count, pixels);
            }
            add("draws", fmt("%s %s%s%s", c.op == kOpClear ? "CLEAR" : "SCREEN_PRIMS", kind == kKindTriangles ? "triangles" : kind == kKindLines ? "lines" : kind == kKindSprites ? "sprites" : "points",
                             c.op == kOpScreenPrims && (c.c & kPrimsOneColour) ? ", one colour" : "",
                             c.op == kOpScreenPrims && (c.c & kPrimsRectPairs) ? ", rectangle pairs" : ""),
                count, pixels);
        }
        add("list", "commands", l.cmds.size(), l.payload.size());
    }

    Gif2dCheck::Gif2dCheck(const Gif2dOptions &options) : m(std::make_unique<Impl>(options)) {}
    Gif2dCheck::~Gif2dCheck() = default;

    void Gif2dCheck::open(const FileInfo &info) { m->fr.open(info); }
    void Gif2dCheck::keyframe(const Keyframe &kf) { m->fr.keyframe(kf); }
    void Gif2dCheck::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) { m->fr.hw(address, width, value, callerRa); }
    rc::rd::FrameReader *Gif2dCheck::frameReader() { return &m->fr; }

    void Gif2dCheck::cycleBegin(uint64_t cycle)
    {
        m->cycle = cycle;
        m->faultPrimBase = 0;
        m->fr.cycleBegin(cycle);
        if (m->vk)
            m->vk->cycleBegin(cycle);
    }

    bool Gif2dCheck::startVk(std::string *why) { return !m->vk || m->vk->start(why); }

    void Gif2dCheck::afterKick(const KickView &k)
    {
        if (!m->vk || !m->opt.vram)
            return;
        if (k.channel == 1u)
        {
            // (d) The machine has run the channel 1 kick and the frame reader (through the vif consumer) has read it:
            // its K2 commands are list[path1First, end); judged over the memory saved before it.
            if (!m->path1Pending || m->path1Cycle != k.cycle || m->path1Index != k.index)
                return;
            m->path1Pending = false;
            const DrawList &l = m->reader.list();
            const uint32_t last = static_cast<uint32_t>(l.cmds.size());
            bool draws = false;
            for (uint32_t i = m->path1First; i < last && !draws; ++i)
                draws = l.cmds[i].op == kOpScreenPrims || l.cmds[i].op == kOpClear;
            m->vk->kick(k, l, m->path1First, last, draws, m->path1Before.data());
        }
        m->vk->after(k, m->opt.vram());
    }

    void Gif2dCheck::kick(const KickView &k, const RamView &ram)
    {
        Gif2dReader &r = m->reader;
        // (a) Before the reader takes the kick (and seeds what it does not know): what it knows must be recorded.
        if (k.regs)
        {
            ++m->regKicks;
            for (uint32_t a = 0; a < kGsRegFile; ++a)
            {
                if (a == kGsHwreg || (r.provenance()[a] != kProvStream && r.provenance()[a] != kProvSeeded))
                    continue;
                ++m->regCompared;
                if (r.regs()[a] != k.regs[a])
                {
                    ++m->regMismatches;
                    ++m->mismatchByReg[a];
                    if (m->mismatchText.size() < 20)
                        m->mismatchText.push_back(fmt("cycle %" PRIu64 " kick %u (ch%u): register 0x%02x is %016" PRIx64 " in the reader (%s), recorded %016" PRIx64,
                                                      k.cycle, k.index, k.channel, a, r.regs()[a], r.provenance()[a] == kProvStream ? "from the stream" : "seeded",
                                                      k.regs[a]));
                }
            }
            if (m->havePath1Regs)
            {
                ++m->path1Pairs;
                for (uint32_t a = 0; a < kGsRegFile; ++a)
                    m->path1Changed[a] += m->path1Regs[a] != k.regs[a];
                m->havePath1Regs = false;
            }
            if (k.channel == 1u)
            {
                std::memcpy(m->path1Regs, k.regs, sizeof(m->path1Regs));
                m->havePath1Regs = true;
            }
        }
        uint64_t before[kGsRegFile];
        std::memcpy(before, k.regs ? k.regs : r.regs(), sizeof(before));

        // (d) A channel 1 kick: the emulated GS memory before the machine runs it (its list is made behind the machine).
        if (m->vk && k.channel == 1u && m->opt.vram && m->opt.withEmu)
        {
            const uint8_t *v = m->opt.vram();
            m->path1Before.assign(v, v + (4u << 20));
            m->path1Pending = true;
            m->path1Cycle = k.cycle;
            m->path1Index = k.index;
            m->path1First = static_cast<uint32_t>(r.list().cmds.size());
        }

        m->fr.kick(k, ram);

        // (c) The kick's commands and the emulated GS memory before the machine runs it.
        if (m->vk && k.channel == 2u && m->opt.vram)
            m->vk->kick(k, r.list(), r.kickFirstCommand(), static_cast<uint32_t>(r.list().cmds.size()), r.kickPrims() != 0 && !r.kickHadTransfers(),
                        m->opt.vram());

        // (b) The kick's draw list encoded back, for the emu consumer behind this one.
        m->pending.valid = false;
        if (!m->opt.withEmu || k.channel != 2u || (!r.kickPrims() && !r.kickDropped()))
            return;
        if (r.kickHadTransfers())
        {
            ++m->notSubstitutedTransfers;
            return;
        }
        bool afterKnown[kGsRegFile];
        for (uint32_t a = 0; a < kGsRegFile; ++a)
            afterKnown[a] = a != kGsHwreg && r.provenance()[a] != kProvUnknown;
        GsEncodeFault fault;
        if (m->opt.faultKind == "field" && static_cast<int64_t>(k.cycle) >= m->opt.faultCycle)
            fault.zeroField = static_cast<int>(m->opt.faultN);
        const DrawList &l = r.list();
        const uint32_t first = r.kickFirstCommand(), last = static_cast<uint32_t>(l.cmds.size());
        if (m->opt.faultKind == "prim" && static_cast<int64_t>(k.cycle) == m->opt.faultCycle && !m->faultDone)
        {
            uint64_t n = 0;
            for (uint32_t i = first; i < last; ++i)
                n += l.cmds[i].op == kOpClear ? 1u : l.cmds[i].op == kOpScreenPrims ? l.cmds[i].b : 0u;
            if (m->opt.faultN >= m->faultPrimBase && m->opt.faultN < m->faultPrimBase + n)
            {
                fault.dropPrim = static_cast<int64_t>(m->opt.faultN - m->faultPrimBase);
                m->faultDone = true;
                std::printf("fault planted: cycle %" PRIu64 " kick %u: primitive %" PRId64 " of the kick (%u of the cycle) left out of the encoded stream\n",
                            k.cycle, k.index, fault.dropPrim, m->opt.faultN);
            }
            m->faultPrimBase += n;
        }
        GsEncodeResult e = encodeDrawList(l, first, last, before, r.regs(), afterKnown, fault);
        ++m->encodedKicks;
        m->encodedWrites += e.writes;
        m->encodedPrims += e.prims;
        m->encodedStates += e.states;
        m->restored += e.restored;
        m->encodedBytes += e.gif.size();
        if (e.unrestorable)
        {
            m->unrestorable += e.unrestorable;
            if (m->unrestorableText.size() < 10)
                m->unrestorableText.push_back(fmt("cycle %" PRIu64 " kick %u: %u register(s) with side effects left different (first 0x%02x)", k.cycle, k.index,
                                                  e.unrestorable, e.firstUnrestorable));
        }
        m->pending.valid = true;
        m->pending.cycle = k.cycle;
        m->pending.index = k.index;
        m->pending.gif = std::move(e.gif);
    }

    const std::vector<uint8_t> *Gif2dCheck::substitute(const KickView &k)
    {
        if (!m->pending.valid || m->pending.cycle != k.cycle || m->pending.index != k.index)
            return nullptr;
        m->pending.valid = false;
        ++m->substituted;
        return &m->pending.gif;
    }

    void Gif2dCheck::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        m->fr.cycleEnd(info, refRgba);
        const DrawList &l = m->reader.list();
        if (m->vk)
            m->vk->cycleEnd(l);
        if (!l.cmds.empty())
        {
            ++m->cyclesWithList;
            m->largestList = std::max<uint64_t>(m->largestList, l.cmds.size());
            m->largestPayload = std::max<uint64_t>(m->largestPayload, l.payload.size());
        }
        if (m->opt.tables)
            m->tables();
        if (!m->opt.quiet)
        {
            uint32_t states = 0, prims = 0, textures = 0;
            for (const DrawCmd &c : l.cmds)
            {
                states += c.op == kOpState;
                textures += c.op == kOpTexture;
                prims += c.op == kOpScreenPrims ? c.b : c.op == kOpClear ? 1u : 0u;
            }
            std::printf("cycle %" PRIu64 ": %zu commands (%u STATE, %u TEXTURE), %u primitives, %zu payload bytes\n", info.end.cycle, l.cmds.size(), states,
                        textures, prims, l.payload.size());
        }
    }

    bool Gif2dCheck::failed() const
    {
        const Gif2dReader::Stats &s = m->reader.stats();
        uint64_t guards = 0;
        for (uint32_t g = 1; g < kGuardCount; ++g)
            if (g != kGuardMipmap) // D-3D-2: MXL > 0 is drawn from level 0, as the reference draws it (counted, not a failure)
                guards += s.guardPrims[g];
        return m->regMismatches != 0 || m->unrestorable != 0 || s.walkerDropped != 0 || s.absentPages != 0 ||
               (m->opt.withEmu && m->substituted != m->encodedKicks) || (m->vk && (m->vk->failed() || guards != 0));
    }

    void Gif2dCheck::summary() const
    {
        const Gif2dReader::Stats &s = m->reader.stats();
        std::printf("gif2d: %" PRIu64 " cycles, %" PRIu64 " channel 2 kicks, %" PRIu64 " channel 1 kicks; %" PRIu64 " register writes, %" PRIu64 " vertices (%" PRIu64
                    " without a drawing kick, %" PRIu64 " XYZF)\n",
                    s.cycles, s.kicks, s.path1Kicks, s.regWrites, s.vertices, s.verticesNoKick, s.xyzfVertices);
        std::printf("gif2d: %" PRIu64 " primitives: %" PRIu64 " triangles, %" PRIu64 " lines, %" PRIu64 " sprites (%" PRIu64 " clears), %" PRIu64
                    " points; dropped %" PRIu64 " (FRAME.FBW 0: %" PRIu64 ", a register with no default unknown: %" PRIu64 ")\n",
                    s.prims, s.primsByKind[kKindTriangles], s.primsByKind[kKindLines], s.primsByKind[kKindSprites], s.clears, s.primsByKind[kKindPoints], s.dropped,
                    s.droppedFbw0, s.droppedUnknown);
        std::printf("gif2d: draw lists: %" PRIu64 " STATE, %" PRIu64 " TEXTURE (%.1f MB), %" PRIu64 " SCREEN_PRIMS, %" PRIu64 " scope commands; %" PRIu64
                    " cycles with a list, largest %" PRIu64 " commands / %" PRIu64 " payload bytes\n",
                    s.stateCmds, s.textureCmds, s.textureBytes / 1048576.0, s.primCmds, s.scopeCmds, m->cyclesWithList, m->largestList, m->largestPayload);
        std::printf("gif2d: vertices whose Q is not the RGBAQ register's: %" PRIu64 "\n", s.vertexQNotRgbaq);
        std::printf("gif2d: kicks that drew and uploaded: %" PRIu64 "; walker bytes dropped %" PRIu64 ", absent pages %" PRIu64 "; TEX0 with CLD other than 1 at a textured state %" PRIu64
                    "; vertices with Z >= 2^24 under Z24 %" PRIu64 "; draws with SCANMSK or CSM2 %" PRIu64 "\n",
                    s.kicksWithTransfersAndDraws, s.walkerDropped, s.absentPages, s.cldOther, s.zAbove24, s.sideRegsNonZero);
        std::string side;
        for (uint32_t a = 0; a < 0x80; ++a)
            if (s.sideRegs[a])
                side += fmt(" 0x%02x x%" PRIu64, a, s.sideRegs[a]);
        std::printf("gif2d: writes of registers outside GsState a draw could read:%s\n", side.empty() ? " none" : side.c_str());
        std::string guards;
        for (uint32_t g = 1; g < kGuardCount; ++g)
            if (s.guardPrims[g])
                guards += fmt(" %s %" PRIu64, gsGuardName(g), s.guardPrims[g]);
        std::printf("gif2d: guards fired (primitives):%s\n", guards.empty() ? " none" : guards.c_str());
        std::string need;
        for (uint32_t g = 0; g < kGsGroupCount; ++g)
            if (s.neededSeeded[g] || s.neededDefault[g])
                need += fmt(" %s %" PRIu64 "/%" PRIu64 "/%" PRIu64, gsGroupName(g), s.neededSeeded[g], s.neededSeededNotDefault[g], s.neededDefault[g]);
        std::printf("gif2d: primitives that needed a seeded/default register: before the dump's first clear %" PRIu64
                    "; after it, by register (seeded / of them not the 3.1 default / default):%s\n",
                    s.neededBeforeFirstClear, need.empty() ? " none" : need.c_str());
        std::printf("(a) register files: %" PRIu64 " values compared at %" PRIu64 " kicks, %" PRIu64 " differ\n", m->regCompared, m->regKicks, m->regMismatches);
        for (const std::string &t : m->mismatchText)
            std::printf("FAIL %s\n", t.c_str());
        std::string p1;
        for (uint32_t a = 0; a < kGsRegFile; ++a)
            if (m->path1Changed[a])
                p1 += fmt(" 0x%02x%s x%" PRIu64, a, path1Writes(static_cast<uint8_t>(a)) ? "" : "(!)", m->path1Changed[a]);
        std::printf("(a) registers a channel 1 kick changed (%" PRIu64 " kicks with a following file; (!) = not in path1Writes):%s\n", m->path1Pairs,
                    p1.empty() ? " none" : p1.c_str());
        if (m->opt.withEmu)
        {
            std::printf("(b) %" PRIu64 " drawing kicks encoded from the draw list (%" PRIu64 " STATE writes, %" PRIu64 " primitives, %" PRIu64 " register writes, %.1f MB), %" PRIu64
                        " substituted in front of the emulated GS; %" PRIu64 " registers written back at kick ends; %" PRIu64 " left different; %" PRIu64
                        " drawing kicks with uploads not substituted\n",
                        m->encodedKicks, m->encodedStates, m->encodedPrims, m->encodedWrites, m->encodedBytes / 1048576.0, m->substituted, m->restored, m->unrestorable,
                        m->notSubstitutedTransfers);
            for (const std::string &t : m->unrestorableText)
                std::printf("FAIL %s\n", t.c_str());
        }
        if (m->vk)
            m->vk->summary();
        if (m->opt.tables)
            for (const auto &[cat, keys] : m->hist)
                for (const auto &[key, c] : keys)
                    std::printf("H\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\n", cat.c_str(), key.c_str(), c.prims, c.pixels);
    }
}
