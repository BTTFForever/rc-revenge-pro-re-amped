// Check (c) of RENDERER_2D_DESIGN.md 10.3: see gif2d_vk.h.
#include "gif2d_vk.h"

#include "png.h"

#include "../../render/gs2d/gs2d_vk.h"
#include "../../render/gs2d/gs_model.h"
#include "../../render/gs2d/gs_plan.h"
#include "../../render/tex/gs_layout.h"

#include "rhi.h"
#include "vk/rhi_vk.h"

#include <algorithm>
#include <array>
#include <chrono>
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
        constexpr uint32_t kW = Gs2dVk::kWidth, kH = Gs2dVk::kHeight, kPixels = kW * kH;
        constexpr int kDiffRange = 256; // histogram index d + 256

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

        std::string stateKey(const GsState &s)
        {
            static const char *const typeName[8] = {"point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"};
            const std::string alpha = s.abe() ? fmt("%" PRIx64, s.alpha) : std::string("-");
            return fmt("%s%s%s alpha=%s test=%05" PRIx64 " zmsk=%u", typeName[s.primType()], s.tme() ? " TME" : "", s.iip() ? " IIP" : "", alpha.c_str(),
                       s.test, static_cast<unsigned>((s.zbuf >> 32) & 1u));
        }

        // Calls f(x, y) for every pixel of the target a primitive covers by the reference's rules (triangles: the
        // edge functions with the top-left bias; lines: Bresenham between the truncated end points; points; sprites:
        // the covered rectangle), inside the scissor. Returns the pixel count.
        template <class F>
        uint64_t forEachCovered(const GsState &s, uint32_t kind, const GsVertex *v, F &&f)
        {
            const uint32_t ofx = static_cast<uint32_t>(s.xyoffset & 0xFFFFu), ofy = static_cast<uint32_t>((s.xyoffset >> 32) & 0xFFFFu);
            const int sx0 = std::max(static_cast<int>(s.scissor & 0x7FFu), 0), sx1 = std::min(static_cast<int>((s.scissor >> 16) & 0x7FFu), static_cast<int>(kW) - 1);
            const int sy0 = std::max(static_cast<int>((s.scissor >> 32) & 0x7FFu), 0), sy1 = std::min(static_cast<int>((s.scissor >> 48) & 0x7FFu), static_cast<int>(kH) - 1);
            auto inside = [&](int x, int y) { return x >= sx0 && x <= sx1 && y >= sy0 && y <= sy1; };
            uint64_t n = 0;
            if (kind == kKindTriangles)
            {
                GsTri t;
                if (!gsTriSetup(v, ofx, ofy, t))
                    return 0;
                for (int y = std::max(t.minY, sy0); y <= std::min(t.maxY, sy1); ++y)
                    for (int x = std::max(t.minX, sx0); x <= std::min(t.maxX, sx1); ++x)
                    {
                        const int64_t e[3] = {gsTriEdge(t, 0, x, y), gsTriEdge(t, 1, x, y), gsTriEdge(t, 2, x, y)};
                        if (gsTriCovers(t, e))
                        {
                            f(x, y);
                            ++n;
                        }
                    }
            }
            else if (kind == kKindLines)
            {
                std::vector<GsLinePixel> px;
                expandLine(v, s.iip(), ofx, ofy, px);
                for (const GsLinePixel &l : px)
                    if (inside(l.x, l.y))
                    {
                        f(l.x, l.y);
                        ++n;
                    }
            }
            else if (kind == kKindPoints)
            {
                const int x = static_cast<int>(static_cast<float>(v[0].x) / 16.0f) - static_cast<int>(ofx >> 4);
                const int y = static_cast<int>(static_cast<float>(v[0].y) / 16.0f) - static_cast<int>(ofy >> 4);
                if (inside(x, y))
                {
                    f(x, y);
                    ++n;
                }
            }
            else
            {
                const GsRect r = coverSprite(v, ofx, ofy);
                for (int y = std::max(r.y0, sy0); y <= std::min(r.y1, sy1); ++y)
                    for (int x = std::max(r.x0, sx0); x <= std::min(r.x1, sx1); ++x)
                    {
                        f(x, y);
                        ++n;
                    }
            }
            return n;
        }

        void copyCmd(DrawList &to, const DrawList &from, const DrawCmd &c)
        {
            DrawCmd &n = to.add(c.op, from.data(c), c.payloadBytes);
            n.flags = c.flags;
            n.a = c.a;
            n.b = c.b;
            n.c = c.c;
            n.d = c.d;
        }

        void toBytes(const std::vector<uint32_t> &words, std::vector<uint8_t> &bytes)
        {
            bytes.resize(words.size() * 4u);
            std::memcpy(bytes.data(), words.data(), bytes.size());
        }
    }

    bool gif2dVkFault(const std::string &name, uint32_t &bits)
    {
        static const std::map<std::string, uint32_t> names = {
            {"atst-less", Gs2dVk::kFaultAtstLess},       {"drop-fail", Gs2dVk::kFaultDropFail},       {"split-per-batch", Gs2dVk::kFaultSplitPerBatch},
            {"z-gequal", Gs2dVk::kFaultZGequal},         {"fix", Gs2dVk::kFaultFixOffByOne},          {"colour-swap", Gs2dVk::kFaultColourSwap},
            {"no-half-pixel", Gs2dVk::kFaultNoHalfPixel}, {"bias-always", Gs2dVk::kFaultBiasAlways}, {"texel-row", Gs2dVk::kFaultTexelRow},
        };
        auto it = names.find(name);
        if (it == names.end())
            return false;
        bits |= it->second;
        return true;
    }

    struct Gif2dVkCheck::Impl
    {
        Gif2dVkOptions opt;
        Gs2dVk vk;
        bool rhiUp = false, up = false;
        std::string adapter;
        uint32_t validationErrors = 0;

        // TEXTURE commands not given to the GPU yet, the cycle's cursor.
        DrawList carry;
        uint32_t cursor = 0;

        // The kick in judgement.
        struct Pending
        {
            bool valid = false;
            uint64_t cycle = 0;
            uint32_t index = 0;
            DrawList sub;             // carry + the state in force + the kick's commands
            DrawList kickTextures;    // the kick's own TEXTURE commands (back to carry if it is never drawn)
            uint32_t targets = 0;     // bit per colour target
            uint32_t zbp = 0xA0u;
            std::vector<uint32_t> before[2], beforeZ;
            std::vector<uint16_t> L[2], last[2];
            std::vector<std::string> keys; // by `last` index; 0: no inexact primitive over the pixel
            uint32_t prims = 0, inexactPrims = 0;
            bool k1 = false;
        } p;

        // Totals per bias setting.
        struct Bucket
        {
            uint64_t pixels = 0, equal = 0, within = 0, out = 0;
            std::array<uint64_t, 2 * kDiffRange> hist{}; // per channel value
            int minD = 0, maxD = 0;
        };
        struct Totals
        {
            uint64_t kicks = 0, k1Kicks = 0, pixels = 0, equal = 0, within = 0, out = 0;
            uint64_t alphaDiff = 0, depthDiff = 0, depthPixels = 0, channelNonZero = 0, absSum = 0;
            int minD = 0, maxD = 0;
            double maxRatio = 0.0;                 // the largest d / L (d > 0) and -d / L (d < 0)
            std::string minWhere, maxWhere;
            std::map<std::string, Bucket> byState; // the last inexact state over the pixel ("L=0" where none)
            std::map<uint32_t, Bucket> byL;
            std::vector<std::string> failures;
            uint64_t failingKicks = 0;
            bool pngWritten = false;
            double ms = 0.0;
        } tot[2];
        uint32_t largestL = 0;
        std::string largestLWhere;
        uint64_t lHist[64] = {}; // pixel-kicks with L = n (n > 0; 63: 63 or more)
        uint64_t kicksSeen = 0, kicksNoState = 0, kicksUnjudged = 0, zTopByte = 0, drawFailures = 0, readFailures = 0;
        double readMs = 0.0;

        void runBias(int bias, const std::vector<uint32_t> (&ref)[2], const std::vector<uint32_t> &refZ);
        std::string primsAt(int x, int y, uint32_t target) const;
    };

    Gif2dVkCheck::Gif2dVkCheck(const Gif2dVkOptions &options) : m(std::make_unique<Impl>()) { m->opt = options; }
    Gif2dVkCheck::~Gif2dVkCheck() { stop(); }

    bool Gif2dVkCheck::start(std::string *why)
    {
        RhiDeviceDesc desc{};
        desc.sdlWindow = nullptr;
        desc.vsync = false;
        desc.debugLayers = m->opt.validation;
        desc.appName = "rc_replay gif2d --vk";
        if (!rhi_init(&desc))
        {
            if (why)
                *why = "rhi_init failed (no Vulkan device?)";
            return false;
        }
        m->rhiUp = true;
        m->adapter = rhi_adapter_name() ? rhi_adapter_name() : "?";
        if (!m->vk.create(why))
            return false;
        m->up = true;
        std::printf("gif2d --vk: %s, validation %s; source bias %s; planted faults 0x%x\n", m->adapter.c_str(),
                    rhi_limits()->validationLayer ? "on" : "off", m->opt.biasMask == 3u ? "off and on" : (m->opt.biasMask & 2u) ? "on" : "off",
                    m->opt.faults);
        return true;
    }

    void Gif2dVkCheck::stop()
    {
        if (!m->rhiUp)
            return;
        m->vk.destroy();
        m->validationErrors += rhi_vk_validation_error_count();
        rhi_shutdown();
        m->rhiUp = m->up = false;
    }

    void Gif2dVkCheck::cycleBegin(uint64_t) { m->cursor = 0; }

    void Gif2dVkCheck::cycleEnd(const DrawList &list)
    {
        for (uint32_t i = m->cursor; i < list.cmds.size(); ++i)
            if (list.cmds[i].op == kOpTexture)
                copyCmd(m->carry, list, list.cmds[i]);
        m->cursor = static_cast<uint32_t>(list.cmds.size());
    }

    void Gif2dVkCheck::kick(const KickView &k, const DrawList &list, uint32_t first, uint32_t last, bool judge, const uint8_t *vram)
    {
        Impl &s = *m;
        Impl::Pending &p = s.p;
        // A kick left undrawn (the machine did not run it): its textures stay owed.
        if (p.valid)
        {
            for (const DrawCmd &c : p.kickTextures.cmds)
                copyCmd(s.carry, p.kickTextures, c);
            p.valid = false;
            ++s.kicksUnjudged;
        }
        for (uint32_t i = s.cursor; i < first && i < list.cmds.size(); ++i)
            if (list.cmds[i].op == kOpTexture)
                copyCmd(s.carry, list, list.cmds[i]);
        s.cursor = std::max(s.cursor, last);
        if (!judge || !s.up)
        {
            for (uint32_t i = first; i < last; ++i)
                if (list.cmds[i].op == kOpTexture)
                    copyCmd(s.carry, list, list.cmds[i]);
            return;
        }
        ++s.kicksSeen;
        p.sub.clear();
        p.kickTextures.clear();
        for (const DrawCmd &c : s.carry.cmds)
            copyCmd(p.sub, s.carry, c);
        // The state in force at the kick's first command (STATE is emitted only when it changes).
        int64_t stateAt = -1;
        for (int64_t i = static_cast<int64_t>(first) - 1; i >= 0; --i)
            if (list.cmds[static_cast<size_t>(i)].op == kOpState)
            {
                stateAt = i;
                break;
            }
        if (stateAt >= 0)
            copyCmd(p.sub, list, list.cmds[static_cast<size_t>(stateAt)]);
        for (uint32_t i = first; i < last; ++i)
        {
            copyCmd(p.sub, list, list.cmds[i]);
            if (list.cmds[i].op == kOpTexture)
                copyCmd(p.kickTextures, list, list.cmds[i]);
        }

        // L per pixel and the targets, from the kick's commands.
        p.targets = 0;
        p.prims = p.inexactPrims = 0;
        p.k1 = false;
        p.keys.assign(1, "L=0");
        for (int t = 0; t < 2; ++t)
        {
            p.L[t].assign(kPixels, 0);
            p.last[t].assign(kPixels, 0);
        }
        const GsState *cur = nullptr;
        GsPlan plan{};
        bool drawable = false;
        int curKey = -1;
        bool sawState = false;
        for (const DrawCmd &c : p.sub.cmds)
        {
            if (c.op == kOpState)
            {
                cur = &p.sub.state(c);
                plan = planState(*cur);
                drawable = !(plan.unsupported || (c.a & 1u));
                curKey = -1;
                sawState = true;
                continue;
            }
            if ((c.op != kOpScreenPrims && c.op != kOpClear) || !cur || !drawable)
                continue;
            const uint32_t kind = c.op == kOpClear ? static_cast<uint32_t>(kKindSprites) : c.a, count = c.op == kOpClear ? 1u : c.b;
            p.targets |= 1u << (plan.target & 1u);
            p.zbp = static_cast<uint32_t>(cur->zbuf & 0x1FFu);
            p.prims += count;
            p.k1 |= c.op == kOpScreenPrims;
            const bool inexact = plan.blend != kBlendOff && !plan.exactBlend;
            if (!inexact)
                continue;
            if (curKey < 0)
            {
                p.keys.push_back(stateKey(*cur));
                curKey = static_cast<int>(p.keys.size() - 1);
            }
            const GsVertex *v = p.sub.vertices(c);
            const uint32_t per = c.op == kOpClear ? 2u : verticesPerPrim(kind);
            std::vector<uint16_t> &L = p.L[plan.target & 1u], &lastKey = p.last[plan.target & 1u];
            for (uint32_t i = 0; i < count; ++i)
                p.inexactPrims += forEachCovered(*cur, kind, v + i * per, [&](int x, int y)
                                                 {
                                                     const size_t at = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
                                                     ++L[at];
                                                     lastKey[at] = static_cast<uint16_t>(curKey);
                                                 }) != 0;
        }
        if (!sawState)
        {
            ++s.kicksNoState;
            for (const DrawCmd &c : p.kickTextures.cmds)
                copyCmd(s.carry, p.kickTextures, c);
            return;
        }
        // The emulated planes before the kick.
        const auto t0 = std::chrono::steady_clock::now();
        const LayoutTables &lt = layoutTables();
        for (uint32_t t = 0; t < 2; ++t)
        {
            p.before[t].clear();
            if (!(p.targets & (1u << t)))
                continue;
            p.before[t].resize(kPixels);
            const uint32_t bp = t ? 0x50u * 32u : 0u;
            for (uint32_t y = 0; y < kH; ++y)
                for (uint32_t x = 0; x < kW; ++x)
                    std::memcpy(&p.before[t][y * kW + x], vram + (gsAddr32(lt, bp, 10u, x, y) & (kGsBytes - 1u)), 4);
        }
        p.beforeZ.resize(kPixels);
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
            {
                uint32_t z;
                std::memcpy(&z, vram + (gsAddrZ32(lt, p.zbp * 32u, 10u, x, y) & (kGsBytes - 1u)), 4);
                s.zTopByte += (z >> 24) != 0u;
                p.beforeZ[y * kW + x] = z & 0xFFFFFFu;
            }
        s.readMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        p.valid = true;
        p.cycle = k.cycle;
        p.index = k.index;
    }

    std::string Gif2dVkCheck::Impl::primsAt(int x, int y, uint32_t target) const
    {
        std::string out;
        const GsState *cur = nullptr;
        GsPlan plan{};
        uint32_t n = 0;
        for (const DrawCmd &c : p.sub.cmds)
        {
            if (c.op == kOpState)
            {
                cur = &p.sub.state(c);
                plan = planState(*cur);
                continue;
            }
            if ((c.op != kOpScreenPrims && c.op != kOpClear) || !cur || (plan.target & 1u) != target)
                continue;
            const uint32_t kind = c.op == kOpClear ? static_cast<uint32_t>(kKindSprites) : c.a, count = c.op == kOpClear ? 1u : c.b;
            const uint32_t per = c.op == kOpClear ? 2u : verticesPerPrim(kind);
            const GsVertex *v = p.sub.vertices(c);
            for (uint32_t i = 0; i < count; ++i, ++n)
            {
                bool covers = false;
                forEachCovered(*cur, kind, v + i * per, [&](int px, int py) { covers |= px == x && py == y; });
                if (!covers)
                    continue;
                const GsVertex &q = v[i * per + per - 1];
                out += fmt("\n      primitive %u (%s, %s%s; colour %02x %02x %02x %02x, z %06x)", n, stateKey(*cur).c_str(),
                           plan.blend == kBlendOff ? "blend off" : plan.exactBlend ? "exact blend" : "inexact blend", c.op == kOpClear ? ", the clear" : "",
                           q.r, q.g, q.b, q.a, q.z);
            }
        }
        return out.empty() ? std::string(" none") : out;
    }

    void Gif2dVkCheck::Impl::runBias(int bias, const std::vector<uint32_t> (&ref)[2], const std::vector<uint32_t> &refZ)
    {
        Totals &T = tot[bias];
        const auto t0 = std::chrono::steady_clock::now();
        Gs2dVk::Options o;
        o.sourceBias = bias != 0;
        o.faults = opt.faults;
        vk.setOptions(o);
        for (uint32_t t = 0; t < 2; ++t)
            if (p.targets & (1u << t))
                if (!vk.seed(t, p.before[t].data(), p.beforeZ.data()))
                {
                    ++drawFailures;
                    return;
                }
        if (!vk.draw(p.sub))
        {
            ++drawFailures;
            return;
        }
        std::vector<uint32_t> got[2], gotZ;
        for (uint32_t t = 0; t < 2; ++t)
            if ((p.targets & (1u << t)) && !vk.readColour(t, got[t]))
            {
                ++readFailures;
                return;
            }
        if (!vk.readDepth(gotZ))
            ++readFailures; // a depth that is not an integer Z: compared anyway (it differs)
        T.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++T.kicks;
        T.k1Kicks += p.k1;

        bool kickFailed = false;
        int failX = -1, failY = -1;
        uint32_t failT = 0;
        // Depth: one plane for both targets.
        for (uint32_t i = 0; i < kPixels; ++i)
        {
            ++T.depthPixels;
            if (gotZ.size() == kPixels && gotZ[i] == (refZ[i] & 0xFFFFFFu))
                continue;
            ++T.depthDiff;
            if (!kickFailed)
            {
                kickFailed = true;
                failX = static_cast<int>(i % kW);
                failY = static_cast<int>(i / kW);
                failT = (p.targets & 1u) ? 0u : 1u;
                if (T.failures.size() < 12)
                    T.failures.push_back(fmt("cycle %" PRIu64 " kick %u: depth at %d,%d is %06x on Vulkan, %06x in the emulated GS (before %06x)", p.cycle,
                                             p.index, failX, failY, gotZ.size() == kPixels ? gotZ[i] : 0u, refZ[i] & 0xFFFFFFu, p.beforeZ[i]));
            }
        }
        for (uint32_t t = 0; t < 2; ++t)
        {
            if (!(p.targets & (1u << t)))
                continue;
            const std::vector<uint32_t> &r = ref[t], &g = got[t];
            const std::vector<uint16_t> &L = p.L[t], &lk = p.last[t];
            for (uint32_t i = 0; i < kPixels; ++i)
            {
                const uint32_t l = L[i];
                const std::string &key = p.keys[lk[i]];
                Bucket &bs = T.byState[key];
                Bucket &bl = T.byL[std::min<uint32_t>(l, 63u)];
                ++T.pixels;
                ++bs.pixels;
                ++bl.pixels;
                const int lo = -static_cast<int>(l), hi = bias ? static_cast<int>(l) : 2 * static_cast<int>(l);
                bool equal = true, inBound = true;
                int worstD = 0, worstC = -1;
                for (int c = 0; c < 4; ++c)
                {
                    const int d = static_cast<int>((g[i] >> (8 * c)) & 0xFFu) - static_cast<int>((r[i] >> (8 * c)) & 0xFFu);
                    if (c == 3)
                    {
                        if (d)
                        {
                            equal = inBound = false;
                            ++T.alphaDiff;
                            worstC = 3;
                            worstD = d;
                        }
                        continue;
                    }
                    ++bs.hist[d + kDiffRange];
                    ++bl.hist[d + kDiffRange];
                    bs.minD = std::min(bs.minD, d);
                    bs.maxD = std::max(bs.maxD, d);
                    bl.minD = std::min(bl.minD, d);
                    bl.maxD = std::max(bl.maxD, d);
                    if (!d)
                        continue;
                    equal = false;
                    ++T.channelNonZero;
                    T.absSum += static_cast<uint64_t>(std::abs(d));
                    if (d < T.minD)
                    {
                        T.minD = d;
                        T.minWhere = fmt("cycle %" PRIu64 " kick %u pixel %u,%u target %u, L %u", p.cycle, p.index, i % kW, i / kW, t, l);
                    }
                    if (d > T.maxD)
                    {
                        T.maxD = d;
                        T.maxWhere = fmt("cycle %" PRIu64 " kick %u pixel %u,%u target %u, L %u", p.cycle, p.index, i % kW, i / kW, t, l);
                    }
                    if (l)
                        T.maxRatio = std::max(T.maxRatio, std::abs(static_cast<double>(d)) / l);
                    if (d < lo || d > hi)
                    {
                        inBound = false;
                        if (worstC < 0 || std::abs(d) > std::abs(worstD))
                        {
                            worstC = c;
                            worstD = d;
                        }
                    }
                }
                if (equal)
                {
                    ++T.equal;
                    ++bs.equal;
                    ++bl.equal;
                }
                else if (inBound)
                {
                    ++T.within;
                    ++bs.within;
                    ++bl.within;
                }
                else
                {
                    ++T.out;
                    ++bs.out;
                    ++bl.out;
                    if (T.failures.size() < 12 && !kickFailed)
                    {
                        const int x = static_cast<int>(i % kW), y = static_cast<int>(i / kW);
                        T.failures.push_back(fmt("cycle %" PRIu64 " kick %u target %u pixel %d,%d: %s Vulkan %08x, emulated %08x, before %08x (channel %d off by %d, L %u, bound %d..%d); primitives over it:%s",
                                                 p.cycle, p.index, t, x, y, bias ? "bias on" : "bias off", g[i], r[i], p.before[t][i], worstC, worstD, l, lo, hi,
                                                 primsAt(x, y, t).c_str()));
                    }
                    if (!kickFailed)
                    {
                        kickFailed = true;
                        failX = static_cast<int>(i % kW);
                        failY = static_cast<int>(i / kW);
                        failT = t;
                    }
                }
            }
        }
        if (!kickFailed)
            return;
        ++T.failingKicks;
        if (opt.pngDir.empty() || T.pngWritten)
            return;
        T.pngWritten = true;
        const uint32_t t = failT;
        if (!(p.targets & (1u << t)))
            return;
        const std::string stem = fmt("%s/c%" PRIu64 "_k%u_bias%s_t%u", opt.pngDir.c_str(), p.cycle, p.index, bias ? "on" : "off", t);
        std::vector<uint8_t> bytes, diff(static_cast<size_t>(kPixels) * 4u, 0u);
        toBytes(p.before[t], bytes);
        writePng(stem + "_before.png", bytes.data(), kW, kH);
        toBytes(got[t], bytes);
        writePng(stem + "_vk.png", bytes.data(), kW, kH);
        toBytes(ref[t], bytes);
        writePng(stem + "_ref.png", bytes.data(), kW, kH);
        for (uint32_t i = 0; i < kPixels; ++i)
            for (int c = 0; c < 3; ++c)
            {
                const int d = std::abs(static_cast<int>((got[t][i] >> (8 * c)) & 0xFFu) - static_cast<int>((ref[t][i] >> (8 * c)) & 0xFFu));
                diff[i * 4u + static_cast<uint32_t>(c)] = static_cast<uint8_t>(std::min(255, d ? 64 + d * 32 : 0));
            }
        writePng(stem + "_diff.png", diff.data(), kW, kH);
        std::printf("gif2d --vk: wrote %s_{before,vk,ref,diff}.png (first failure at %d,%d)\n", stem.c_str(), failX, failY);
    }

    void Gif2dVkCheck::after(const KickView &k, const uint8_t *vram)
    {
        Impl &s = *m;
        Impl::Pending &p = s.p;
        if (!p.valid || p.cycle != k.cycle || p.index != k.index)
            return;
        p.valid = false;
        const auto t0 = std::chrono::steady_clock::now();
        const LayoutTables &lt = layoutTables();
        std::vector<uint32_t> ref[2], refZ(kPixels);
        for (uint32_t t = 0; t < 2; ++t)
        {
            if (!(p.targets & (1u << t)))
                continue;
            ref[t].resize(kPixels);
            const uint32_t bp = t ? 0x50u * 32u : 0u;
            for (uint32_t y = 0; y < kH; ++y)
                for (uint32_t x = 0; x < kW; ++x)
                    std::memcpy(&ref[t][y * kW + x], vram + (gsAddr32(lt, bp, 10u, x, y) & (kGsBytes - 1u)), 4);
        }
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
                std::memcpy(&refZ[y * kW + x], vram + (gsAddrZ32(lt, p.zbp * 32u, 10u, x, y) & (kGsBytes - 1u)), 4);
        s.readMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (uint32_t t = 0; t < 2; ++t)
            if (p.targets & (1u << t))
                for (uint32_t i = 0; i < kPixels; ++i)
                {
                    const uint32_t l = p.L[t][i];
                    if (!l)
                        continue;
                    ++s.lHist[std::min<uint32_t>(l, 63u)];
                    if (l > s.largestL)
                    {
                        s.largestL = l;
                        s.largestLWhere = fmt("cycle %" PRIu64 " kick %u pixel %u,%u target %u", p.cycle, p.index, i % kW, i / kW, t);
                    }
                }
        for (int bias = 0; bias < 2; ++bias)
            if (s.opt.biasMask & (1u << bias))
                s.runBias(bias, ref, refZ);
        s.carry.clear(); // given to the GPU with this kick
    }

    bool Gif2dVkCheck::failed() const
    {
        uint64_t bad = m->drawFailures + m->readFailures + m->validationErrors + m->kicksNoState;
        for (int b = 0; b < 2; ++b)
            if (m->opt.biasMask & (1u << b))
                bad += m->tot[b].out + m->tot[b].depthDiff + m->tot[b].alphaDiff + (m->tot[b].kicks == 0 ? 1u : 0u);
        return bad != 0;
    }

    void Gif2dVkCheck::summary()
    {
        // The device is shut down first: the validation layer's count is final then.
        const bool layer = m->rhiUp && rhi_limits() && rhi_limits()->validationLayer;
        stop();
        const Impl &s = *m;
        const uint32_t vErrors = s.validationErrors;
        std::printf("(c) Vulkan over the emulated frame and Z buffers: %" PRIu64 " drawing kicks judged (%" PRIu64 " not run by the machine, %" PRIu64
                    " without a state); %.0f ms reading GS memory; validation errors %u%s; draw/read failures %" PRIu64 "/%" PRIu64 "; Z words with a top byte "
                    "before a kick %" PRIu64 "\n",
                    s.kicksSeen, s.kicksUnjudged, s.kicksNoState, s.readMs, vErrors, layer ? "" : " (layer off)",
                    s.drawFailures, s.readFailures, s.zTopByte);
        std::string lh;
        uint64_t lPix = 0;
        for (uint32_t l = 1; l < 64; ++l)
            if (s.lHist[l])
            {
                lh += fmt(" %u%s:%" PRIu64, l, l == 63 ? "+" : "", s.lHist[l]);
                lPix += s.lHist[l];
            }
        std::printf("(c) L (inexactly blended primitives covering a pixel): largest %u (%s); pixel-kicks with L > 0: %" PRIu64 " by L:%s\n",
                    s.largestL, s.largestLWhere.empty() ? "-" : s.largestLWhere.c_str(), lPix, lh.empty() ? " none" : lh.c_str());
        for (int b = 0; b < 2; ++b)
        {
            if (!(s.opt.biasMask & (1u << b)))
                continue;
            const Impl::Totals &T = s.tot[b];
            const char *name = b ? "bias on" : "bias off";
            std::printf("(c) %s (bound %s): %" PRIu64 " kicks (%" PRIu64 " with K1 primitives), %.1f s on the GPU; %" PRIu64 " pixels compared: %" PRIu64
                        " equal, %" PRIu64 " within the bound, %" PRIu64 " out of it; alpha differs %" PRIu64 ", depth differs %" PRIu64 " of %" PRIu64
                        "; colour channels differing %" PRIu64 " (sum of |d| %" PRIu64 "); worst %d (%s) .. %+d (%s); largest |d|/L %.2f; failing kicks %" PRIu64 "\n",
                        name, b ? "-L..+L" : "-L..+2L", T.kicks, T.k1Kicks, T.ms / 1000.0, T.pixels, T.equal, T.within, T.out, T.alphaDiff, T.depthDiff, T.depthPixels,
                        T.channelNonZero, T.absSum, T.minD, T.minWhere.empty() ? "-" : T.minWhere.c_str(), T.maxD, T.maxWhere.empty() ? "-" : T.maxWhere.c_str(),
                        T.maxRatio, T.failingKicks);
            auto histText = [](const Impl::Bucket &bk)
            {
                std::string h;
                for (int d = -kDiffRange; d < kDiffRange; ++d)
                    if (bk.hist[d + kDiffRange])
                        h += fmt(" %+d:%" PRIu64, d, bk.hist[d + kDiffRange]);
                return h;
            };
            for (const auto &[key, bk] : T.byState)
                std::printf("C\t%s\tstate\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%s\n", name, key.c_str(), bk.pixels, bk.equal, bk.within, bk.out,
                            histText(bk).c_str());
            for (const auto &[l, bk] : T.byL)
                std::printf("C\t%s\tL\t%u%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%s\n", name, l, l == 63 ? "+" : "", bk.pixels, bk.equal, bk.within,
                            bk.out, histText(bk).c_str());
            for (const std::string &f : T.failures)
                std::printf("FAIL (c) %s\n", f.c_str());
        }
    }
}
