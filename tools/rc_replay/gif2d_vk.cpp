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
            {"no-barrier", Gs2dVk::kFaultNoBarrier},     {"one-group", Gs2dVk::kFaultOneGroup},
            // (step 8 batch 2, RENDERER_3D_DESIGN.md 10.3, 11.4)
            {"no-first-barrier", Gs2dVk::kFaultNoFirstBarrier}, {"cover-own-batch", Gs2dVk::kFaultCoverOwnBatch},
            {"no-stamp-other", Gs2dVk::kFaultNoStampOther},     {"epoch-no-barrier", Gs2dVk::kFaultEpochNoBarrier},
            {"gequal-greater", Gs2dVk::kFaultGequalAsGreater},  {"z-plus-one", Gs2dVk::kFaultZOffByOne},
            {"repeat-clamp", Gs2dVk::kFaultRepeatAsClamp},      {"no-q", Gs2dVk::kFaultNoQ},
            {"draw-culled", Gs2dVk::kFaultDrawCulled},          {"cull-drawn", Gs2dVk::kFaultCullDrawn},
        };
        auto it = names.find(name);
        if (it == names.end())
            return false;
        bits |= it->second;
        return true;
    }

    // The settings check (c) runs: fixed function with the bias off (0) or on (1), the exact blend by local read with the
    // barriers by covered pixels (2), the same with batch 4b's box groups (3, --vk-boxes; RENDERER_3D_DESIGN.md 10.4).
    constexpr int kSettings = 4, kRead = 2, kReadBoxes = 3;
    bool isRead(int s) { return s >= kRead; }
    const char *settingName(int s) { return s == kReadBoxes ? "read, box rule" : s == kRead ? "read" : s ? "ff bias on" : "ff bias off"; }
    // Which kicks: K1 and the clear (channel 2, check (c)), K2 (channel 1, check (d)).
    const char *const kWhich[2] = {"(c) K1", "(d) K2"};

    struct Gif2dVkCheck::Impl
    {
        Gif2dVkOptions opt;
        uint32_t settings = 0; // bit per setting, resolved at start()
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
            int ch = 0;               // 0: a channel 2 kick (K1, the clear), 1: a channel 1 kick (K2)
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
            double ms = 0.0, gpuMs = 0.0, gpuMaxMs = 0.0;
            uint64_t gpuKicks = 0;
            std::string gpuMaxWhere;
            uint64_t readPrims = 0, readGroups = 0, barriers = 0, maxGroups = 0, maxBarriers = 0;
            std::string maxGroupsWhere;
            // (step 8 batch 2) per kick: draws, binds, primitives drawn; the CPU's list build and cover map; guards
            uint64_t draws = 0, maxDraws = 0, binds = 0, prims = 0, maxPrims = 0, coverPixels = 0, beyondViewport = 0, zAbove24 = 0;
            double buildMs = 0.0, buildMaxMs = 0.0, coverMs = 0.0, coverMaxMs = 0.0;
            std::string zAbove24Where;
            uint64_t barrierHist[6] = {}; // kicks by barriers: 0, 1-4, 5-16, 17-32, 33-64, 65 or more
        } tot[2][kSettings];
        uint32_t largestL = 0;
        std::string largestLWhere;
        uint64_t lHist[64] = {}; // pixel-kicks with L = n (n > 0; 63: 63 or more)
        uint64_t kicksSeen[2] = {}, kicksNoState[2] = {}, kicksUnjudged = 0, zTopByte = 0, drawFailures = 0, readFailures = 0;
        double readMs = 0.0;

        void runSetting(int setting, const std::vector<uint32_t> (&ref)[2], const std::vector<uint32_t> &refZ);
        std::string primsAt(int x, int y, uint32_t target) const;
        std::string zAbove24At() const;
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
        const bool local = m->vk.readAvailable();
        const std::string &b = m->opt.blend;
        if (b == "read" && !local)
        {
            if (why)
                *why = "--vk-blend read: the device has no local read (RhiLimits.localRead)";
            return false;
        }
        const bool read = b == "read" || b == "both" || (b == "auto" && local);
        const bool ff = b == "ff" || b == "both" || (b == "auto" && !local);
        m->settings = (read ? 1u << kRead : 0u) | (read && m->opt.boxes ? 1u << kReadBoxes : 0u) | (ff ? (m->opt.biasMask & 3u) : 0u);
        std::string names;
        for (int s = 0; s < kSettings; ++s)
            if (m->settings & (1u << s))
                names += std::string(names.empty() ? "" : ", ") + settingName(s);
        std::printf("gif2d --vk: %s, validation %s; local read %s; settings: %s; planted faults 0x%x\n", m->adapter.c_str(),
                    rhi_limits()->validationLayer ? "on" : "off", local ? "yes" : "no", names.c_str(), m->opt.faults);
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
        p.ch = k.channel == 1u ? 1 : 0;
        ++s.kicksSeen[p.ch];
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
            ++s.kicksNoState[p.ch];
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
                if (kind == kKindTriangles) // the vertices, in GS units (1/16 pixel, before the offset)
                    out += fmt("; xy %u,%u %u,%u %u,%u", v[i * per].x, v[i * per].y, v[i * per + 1].x, v[i * per + 1].y, v[i * per + 2].x, v[i * per + 2].y);
            }
        }
        return out.empty() ? std::string(" none") : out;
    }

    // The kick's triangles with a vertex Z at or above 2^24 (RENDERER_3D_DESIGN.md 9.1): the pixels each covers inside
    // the scissor and how many of them get an interpolated Z at or above 2^24 by the model (the reference's arithmetic).
    // Only there could the shader's clamp and the reference's Z24 store differ.
    std::string Gif2dVkCheck::Impl::zAbove24At() const
    {
        std::string out;
        const GsState *cur = nullptr;
        uint32_t n = 0;
        for (const DrawCmd &c : p.sub.cmds)
        {
            if (c.op == kOpState)
            {
                cur = &p.sub.state(c);
                continue;
            }
            if (c.op != kOpScreenPrims || !cur)
                continue;
            if (c.a != kKindTriangles)
            {
                n += c.b;
                continue;
            }
            const GsVertex *v = p.sub.vertices(c);
            const uint32_t ofx = static_cast<uint32_t>(cur->xyoffset & 0xFFFFu), ofy = static_cast<uint32_t>((cur->xyoffset >> 32) & 0xFFFFu);
            for (uint32_t i = 0; i < c.b; ++i, ++n)
            {
                const GsVertex *tv = v + i * 3u;
                if (tv[0].z < (1u << 24) && tv[1].z < (1u << 24) && tv[2].z < (1u << 24))
                    continue;
                GsTri t;
                if (!gsTriSetup(tv, ofx, ofy, t))
                    continue;
                uint64_t covered = 0, above = 0;
                uint32_t zMax = 0;
                covered = forEachCovered(*cur, kKindTriangles, tv, [&](int px, int py) {
                    GsTriPixel q;
                    gsTriPixel(t, tv, *cur, px, py, q);
                    above += q.z >= (1u << 24);
                    zMax = std::max(zMax, q.z);
                });
                out += fmt(" (primitive %u, %s: vertex Z %x %x %x; %" PRIu64 " pixels covered inside the scissor, %" PRIu64
                           " with a pixel Z >= 2^24, largest pixel Z %x)",
                           n, stateKey(*cur).c_str(), tv[0].z, tv[1].z, tv[2].z, covered, above, zMax);
            }
        }
        return out;
    }

    void Gif2dVkCheck::Impl::runSetting(int setting, const std::vector<uint32_t> (&ref)[2], const std::vector<uint32_t> &refZ)
    {
        Totals &T = tot[p.ch][setting];
        const auto t0 = std::chrono::steady_clock::now();
        Gs2dVk::Options o;
        o.blend = isRead(setting) ? Gs2dVk::kBlendRead : Gs2dVk::kBlendFixed;
        o.grouping = setting == kReadBoxes ? Gs2dVk::kGroupBoxes : Gs2dVk::kGroupPixels;
        o.sourceBias = setting == 1;
        o.faults = opt.faults;
        o.measureNoFill = opt.noFill;
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
        const Gs2dVk::ListStats &ls = vk.lastList();
        T.gpuMs += ls.gpuMs;
        T.gpuKicks += ls.gpuMs > 0.0;
        if (ls.gpuMs > T.gpuMaxMs)
        {
            T.gpuMaxMs = ls.gpuMs;
            T.gpuMaxWhere = fmt("cycle %" PRIu64 " kick %u, %" PRIu64 " draws, %" PRIu64 " barriers", p.cycle, p.index, ls.draws, ls.barriers);
        }
        T.readPrims += ls.readPrims;
        T.readGroups += ls.readGroups;
        T.barriers += ls.barriers;
        T.draws += ls.draws;
        T.maxDraws = std::max(T.maxDraws, ls.draws);
        T.binds += ls.binds;
        T.prims += ls.prims;
        T.maxPrims = std::max(T.maxPrims, ls.prims);
        T.coverPixels += ls.coverPixels;
        T.buildMs += ls.buildMs;
        T.buildMaxMs = std::max(T.buildMaxMs, ls.buildMs);
        T.coverMs += ls.coverMs;
        T.coverMaxMs = std::max(T.coverMaxMs, ls.coverMs);
        T.beyondViewport += ls.beyondViewport;
        if (ls.zAbove24 && T.zAbove24Where.empty())
            T.zAbove24Where = fmt("cycle %" PRIu64 " kick %u", p.cycle, p.index) + zAbove24At();
        T.zAbove24 += ls.zAbove24;
        ++T.barrierHist[ls.barriers == 0 ? 0 : ls.barriers <= 4 ? 1 : ls.barriers <= 16 ? 2 : ls.barriers <= 32 ? 3 : ls.barriers <= 64 ? 4 : 5];
        T.maxBarriers = std::max(T.maxBarriers, ls.barriers);
        if (ls.readGroups > T.maxGroups)
        {
            T.maxGroups = ls.readGroups;
            T.maxGroupsWhere = fmt("cycle %" PRIu64 " kick %u", p.cycle, p.index);
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
                const int lo = isRead(setting) ? 0 : -static_cast<int>(l), hi = isRead(setting) ? 0 : setting ? static_cast<int>(l) : 2 * static_cast<int>(l);
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
                        T.failures.push_back(fmt("cycle %" PRIu64 " kick %u target %u pixel %d,%d: %s Vulkan %08x, emulated %08x, before %08x (channel %d off by %d, L %u, bound %d..%d); primitives over it:",
                                                 p.cycle, p.index, t, x, y, settingName(setting), g[i], r[i], p.before[t][i], worstC, worstD, l, lo, hi) +
                                             primsAt(x, y, t)); // appended: fmt's buffer would cut the list
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
        const std::string stem = fmt("%s/c%" PRIu64 "_k%u_%s_t%u", opt.pngDir.c_str(), p.cycle, p.index, setting == kReadBoxes ? "readboxes" : setting == kRead ? "read" : setting ? "ffbiason" : "ffbiasoff", t);
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
        for (int setting = 0; setting < kSettings; ++setting)
            if (s.settings & (1u << setting))
                s.runSetting(setting, ref, refZ);
        s.carry.clear(); // given to the GPU with this kick
    }

    bool Gif2dVkCheck::failed() const
    {
        uint64_t bad = m->drawFailures + m->readFailures + m->validationErrors + m->kicksNoState[0] + m->kicksNoState[1];
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < kSettings; ++b)
                if (m->settings & (1u << b))
                {
                    const Impl::Totals &T = m->tot[c][b];
                    bad += T.out + T.depthDiff + T.alphaDiff + T.beyondViewport + (T.kicks != m->kicksSeen[c] - m->kicksNoState[c] ? 1u : 0u);
                }
        return bad != 0;
    }

    void Gif2dVkCheck::summary()
    {
        // The device is shut down first: the validation layer's count is final then.
        const bool layer = m->rhiUp && rhi_limits() && rhi_limits()->validationLayer;
        stop();
        const Impl &s = *m;
        const uint32_t vErrors = s.validationErrors;
        std::printf("(c)/(d) Vulkan over the emulated frame and Z buffers: %" PRIu64 " K1 / clear kicks and %" PRIu64 " K2 kicks judged (%" PRIu64
                    " not run by the machine, %" PRIu64 " / %" PRIu64 " without a state); %.0f ms reading GS memory; validation errors %u%s; draw/read failures %" PRIu64
                    "/%" PRIu64 "; Z words with a top byte before a kick %" PRIu64 "\n",
                    s.kicksSeen[0], s.kicksSeen[1], s.kicksUnjudged, s.kicksNoState[0], s.kicksNoState[1], s.readMs, vErrors, layer ? "" : " (layer off)",
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
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < kSettings; ++b)
            {
                if (!(s.settings & (1u << b)))
                    continue;
                const Impl::Totals &T = s.tot[c][b];
                if (!T.kicks && !s.kicksSeen[c])
                    continue;
                const char *name = settingName(b);
                const char *which = kWhich[c];
                const double n = T.kicks ? static_cast<double>(T.kicks) : 1.0;
                std::printf("%s %s: GPU time of the draws %.3f ms a kick on average (%" PRIu64 " kicks timed), largest %.3f ms (%s); wall time per kick (seed, "
                            "draw, read-back) %.2f ms\n",
                            which, name, T.gpuKicks ? T.gpuMs / static_cast<double>(T.gpuKicks) : 0.0, T.gpuKicks, T.gpuMaxMs,
                            T.gpuMaxWhere.empty() ? "-" : T.gpuMaxWhere.c_str(), T.kicks ? T.ms / n : 0.0);
                std::printf("%s %s: per kick %.1f draws (largest %" PRIu64 "), %.1f binds, %.1f primitives drawn (largest %" PRIu64 "); CPU: the list's build %.3f ms "
                            "(largest %.3f), of it the cover map %.3f ms (largest %.3f, %.0f pixels looked at); beyond the viewport %" PRIu64 "; triangles with a "
                            "vertex Z >= 2^24 %" PRIu64 "%s%s\n",
                            which, name, T.draws / n, T.maxDraws, T.binds / n, T.prims / n, T.maxPrims, T.buildMs / n, T.buildMaxMs, T.coverMs / n, T.coverMaxMs,
                            T.coverPixels / n, T.beyondViewport, T.zAbove24, T.zAbove24Where.empty() ? "" : " first at ", T.zAbove24Where.c_str());
                if (isRead(b))
                    std::printf("%s %s: %" PRIu64 " reading primitives in %" PRIu64 " groups, %" PRIu64 " barriers (%.2f a kick); largest in a kick: %" PRIu64
                                " groups (%s), %" PRIu64 " barriers; kicks by barriers 0 / 1-4 / 5-16 / 17-32 / 33-64 / 65+: %" PRIu64 " / %" PRIu64 " / %" PRIu64
                                " / %" PRIu64 " / %" PRIu64 " / %" PRIu64 "\n",
                                which, name, T.readPrims, T.readGroups, T.barriers, T.barriers / n, T.maxGroups,
                                T.maxGroupsWhere.empty() ? "-" : T.maxGroupsWhere.c_str(), T.maxBarriers, T.barrierHist[0], T.barrierHist[1], T.barrierHist[2],
                                T.barrierHist[3], T.barrierHist[4], T.barrierHist[5]);
                std::printf("%s %s (bound %s): %" PRIu64 " kicks (%" PRIu64 " with primitives), %.1f s on the GPU; %" PRIu64 " pixels compared: %" PRIu64
                            " equal, %" PRIu64 " within the bound, %" PRIu64 " out of it; alpha differs %" PRIu64 ", depth differs %" PRIu64 " of %" PRIu64
                            "; colour channels differing %" PRIu64 " (sum of |d| %" PRIu64 "); worst %d (%s) .. %+d (%s); largest |d|/L %.2f; failing kicks %" PRIu64 "\n",
                            which, name, isRead(b) ? "equal" : b ? "-L..+L" : "-L..+2L", T.kicks, T.k1Kicks, T.gpuMs / 1000.0, T.pixels, T.equal, T.within, T.out,
                            T.alphaDiff, T.depthDiff, T.depthPixels, T.channelNonZero, T.absSum, T.minD, T.minWhere.empty() ? "-" : T.minWhere.c_str(), T.maxD,
                            T.maxWhere.empty() ? "-" : T.maxWhere.c_str(), T.maxRatio, T.failingKicks);
                auto histText = [](const Impl::Bucket &bk)
                {
                    std::string h;
                    for (int d = -kDiffRange; d < kDiffRange; ++d)
                        if (bk.hist[d + kDiffRange])
                            h += fmt(" %+d:%" PRIu64, d, bk.hist[d + kDiffRange]);
                    return h;
                };
                for (const auto &[key, bk] : T.byState)
                    std::printf("C\t%s %s\tstate\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%s\n", c ? "K2" : "K1", name, key.c_str(), bk.pixels,
                                bk.equal, bk.within, bk.out, histText(bk).c_str());
                for (const auto &[l, bk] : T.byL)
                    std::printf("C\t%s %s\tL\t%u%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%s\n", c ? "K2" : "K1", name, l, l == 63 ? "+" : "",
                                bk.pixels, bk.equal, bk.within, bk.out, histText(bk).c_str());
                for (const std::string &f : T.failures)
                    std::printf("FAIL %s %s\n", which, f.c_str());
            }
    }
}
