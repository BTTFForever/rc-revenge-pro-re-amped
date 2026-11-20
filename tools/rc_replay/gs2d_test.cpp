// rc_gs2d_test: the 2D packet's pixel pipeline through three machines (renderer plan P1 step 7 batch 2;
// RENDERER_2D_DESIGN.md section 9): synthetic GIF packets (register writes, texture uploads and vertices as a 2D
// packet holds them) drawn by (1) the emulated GS (the runtime's software GS alone, the reference every REFI picture
// comes from), (2) gs_model on the CPU and (3) Gs2dVk on the RTX 2060 (headless RHI, read back), after going through
// the reader (Gif2dReader) into a draw list.
//
//   rc_gs2d_test [--only model,T1,...,T10] [--blend read|ff|both] [--no-faults] [--validation] [--quiet]
//
// Runs the model's self-check (9.1), T1-T8 (9.2, 18.3), then each planted fault of 9.3 and 18.3 against the test that
// must catch it. --blend: how Gs2dVk draws the fractional blends (batch 4b, RENDERER_2D_DESIGN.md 18): read, the exact
// blend by local read (every plane equal to the emulated GS); ff, fixed function with D-2D-1's measured bound (T1:
// per channel -1..+1 with the source bias, -1..+2 without, the integer-exact rows equal; T2: -L..+L); both (the
// default where the device has local read; else ff). T3-T6 and T8 run in read mode where it is available, T7 once.
// (Step 8 batch 2, RENDERER_3D_DESIGN.md 9.2 and 10.3) T9: what K2 uses that K1 did not (per-vertex Z under GREATER
// and GEQUAL, every PATH1 state on an alpha-ramped 256 x 128 texture, REPEAT to +-1,000 texels, big triangles, ADC
// vertices); T10: the barriers by covered pixels (water's three passes over a mesh, exactly the predicted barriers;
// reading and exact triangles interleaved; GsCoverMap's spans against gsTriCovers; the box rule on the same scenes).
// --only blenddbg prints a small table of the fixed-function blender's results. Ends PASS or FAIL. GPU:
// __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only (NOTES.md); synchronisation validation:
// VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true with --validation.

#include "../../render/gs2d/gif2d_reader.h"
#include "../../render/gs2d/gs_cover.h"
#include "../../render/gs2d/gs2d_vk.h"
#include "../../render/gs2d/gs_model.h"
#include "../../render/gs2d/gs_plan.h"

#include "ps2_runtime.h"
extern "C"
{
#include "rhi_native.h" // no extern "C" guard of its own
}
#include "vk/rhi_vk.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// No recompiled game code is linked (as rc_replay).
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace GSRaster
{
    std::array<uint64_t, 8> &FallbackCounters();
}

using namespace rc::rd;

namespace
{
    constexpr uint32_t kW = 640, kH = 256, kPix = kW * kH;
    constexpr int kOfx = 1728, kOfy = 1928;
    constexpr uint64_t kFrame0 = 0xA0000ull, kFrame1 = 0xA0050ull, kZbuf = 0x10000A0ull, kZmsk = 1ull << 32;
    constexpr uint64_t kTestNone = 0x30000ull; // no alpha test, ZTE 1, ZTST ALWAYS

    bool g_quiet = false;

    // Gs2dVk's options for a run: the blend mode and, for fixed function, the bias; planted faults kept.
    void setMode(Gs2dVk *vk, bool read, bool bias = true)
    {
        if (!vk)
            return;
        Gs2dVk::Options o = vk->options();
        o.blend = read ? Gs2dVk::kBlendRead : Gs2dVk::kBlendFixed;
        o.sourceBias = bias;
        vk->setOptions(o);
    }

    uint32_t fbits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    uint32_t rgbaOf(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return r | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; }

    // ---------------------------------------------------------------- GIF packets
    struct Gif
    {
        std::vector<uint64_t> out;  // quadwords as pairs of 64-bit words
        std::vector<uint64_t> ad_;  // pending A+D (value, address) pairs

        void ad(uint8_t reg, uint64_t v)
        {
            ad_.push_back(v);
            ad_.push_back(reg);
        }
        void flushAd()
        {
            size_t at = 0;
            while (at < ad_.size())
            {
                const size_t n = std::min<size_t>((ad_.size() - at) / 2u, 0x7FFFu);
                out.push_back(n | 1ull << 60); // NLOOP n, PACKED, NREG 1
                out.push_back(0xE);            // A+D
                out.insert(out.end(), ad_.begin() + at, ad_.begin() + at + n * 2u);
                at += n * 2u;
            }
            ad_.clear();
        }
        // Host-to-local transfer of a w x h rectangle at block address bp (width bw in 64 pixels) in format psm.
        void image(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h, const std::vector<uint8_t> &data)
        {
            ad(0x50, static_cast<uint64_t>(bp) << 32 | static_cast<uint64_t>(bw) << 48 | static_cast<uint64_t>(psm) << 56); // BITBLTBUF
            ad(0x51, 0);                                                    // TRXPOS
            ad(0x52, w | static_cast<uint64_t>(h) << 32);                    // TRXREG
            ad(0x53, 0);                                                    // TRXDIR host -> local
            flushAd();
            std::vector<uint8_t> d = data;
            d.resize((d.size() + 15u) & ~size_t{15}, 0);
            const size_t q = d.size() / 16u;
            out.push_back(q | 2ull << 58); // IMAGE
            out.push_back(0);
            const size_t at = out.size();
            out.resize(at + q * 2u);
            std::memcpy(out.data() + at, d.data(), d.size());
        }
        std::vector<uint8_t> bytes()
        {
            flushAd();
            std::vector<uint8_t> b(out.size() * 8u);
            std::memcpy(b.data(), out.data(), b.size());
            return b;
        }
    };

    struct V
    {
        double x = 0, y = 0; // pixels (window), multiples of 1/16
        uint32_t z = 0;
        uint8_t r = 0x80, g = 0x80, b = 0x80, a = 0x80;
        float s = 0, t = 0, q = 1;
    };
    void vertex(Gif &g, const V &v)
    {
        g.ad(kGsRgbaq, rgbaOf(v.r, v.g, v.b, v.a) | static_cast<uint64_t>(fbits(v.q)) << 32);
        g.ad(kGsSt, fbits(v.s) | static_cast<uint64_t>(fbits(v.t)) << 32);
        const uint64_t X = static_cast<uint64_t>(std::llround((v.x + kOfx) * 16.0)) & 0xFFFFu;
        const uint64_t Y = static_cast<uint64_t>(std::llround((v.y + kOfy) * 16.0)) & 0xFFFFu;
        g.ad(kGsXyz2, X | Y << 16 | static_cast<uint64_t>(v.z) << 32);
    }
    void tri(Gif &g, uint32_t prim, const V &a, const V &b, const V &c)
    {
        g.ad(kGsPrim, prim);
        vertex(g, a);
        vertex(g, b);
        vertex(g, c);
    }
    // A rectangle as K1 sends it (two triangles); tl's colour and z everywhere, S/T from tl (s0, t0) to br (s1, t1).
    void rect(Gif &g, uint32_t prim, V tl, V br)
    {
        V tr = tl, bl = tl;
        tr.x = br.x;
        tr.s = br.s;
        bl.y = br.y;
        bl.t = br.t;
        V brv = tl;
        brv.x = br.x;
        brv.y = br.y;
        brv.s = br.s;
        brv.t = br.t;
        tri(g, prim, tl, tr, bl);
        tri(g, prim, tr, brv, bl);
    }
    void line(Gif &g, uint32_t prim, const V &a, const V &b)
    {
        g.ad(kGsPrim, prim);
        vertex(g, a);
        vertex(g, b);
    }
    void sprite(Gif &g, uint32_t prim, const V &a, const V &b)
    {
        g.ad(kGsPrim, prim);
        vertex(g, a);
        vertex(g, b);
    }

    struct Env
    {
        uint64_t frame = kFrame0, zbuf = kZbuf | kZmsk, test = kTestNone, alpha = 0, tex0 = 0, tex1 = 0, clamp = 5, texa = 0x80;
        uint64_t scissor = 639ull << 16 | 255ull << 48;
        uint32_t pabe = 0;
    };
    void env(Gif &g, const Env &e)
    {
        g.ad(kGsPrmodecont, 1);
        g.ad(kGsFrame, e.frame);
        g.ad(kGsZbuf, e.zbuf);
        g.ad(kGsXyoffset, static_cast<uint64_t>(kOfx * 16) | static_cast<uint64_t>(kOfy * 16) << 32);
        g.ad(kGsScissor, e.scissor);
        g.ad(kGsTest, e.test);
        g.ad(kGsAlpha, e.alpha);
        g.ad(kGsPabe, e.pabe);
        g.ad(kGsFba, 0);
        g.ad(kGsColclamp, 1);
        g.ad(kGsDthe, 0);
        g.ad(kGsTexa, e.texa);
        g.ad(kGsTex1, e.tex1);
        g.ad(kGsClamp, e.clamp);
        if (e.tex0)
            g.ad(kGsTex0, e.tex0);
    }
    uint64_t scissorOf(int x0, int x1, int y0, int y1)
    {
        return static_cast<uint64_t>(x0) | static_cast<uint64_t>(x1) << 16 | static_cast<uint64_t>(y0) << 32 | static_cast<uint64_t>(y1) << 48;
    }
    uint64_t testOf(uint32_t atst, uint32_t aref, uint32_t afail, uint32_t ztst)
    {
        return 1u | atst << 1 | aref << 4 | afail << 12 | 1u << 16 | ztst << 17;
    }
    uint64_t tex0Of(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t tcc, uint32_t tfx, uint32_t cbp = 0, uint32_t cld = 0)
    {
        Tex0 t;
        t.tbp0 = tbp;
        t.tbw = static_cast<uint8_t>(tbw);
        t.psm = static_cast<uint8_t>(psm);
        t.tw = static_cast<uint8_t>(tw);
        t.th = static_cast<uint8_t>(th);
        t.tcc = static_cast<uint8_t>(tcc);
        t.tfx = static_cast<uint8_t>(tfx);
        t.cbp = cbp;
        t.cld = static_cast<uint8_t>(cld);
        return t.pack();
    }

    // ---------------------------------------------------------------- machines
    struct Planes
    {
        std::vector<uint32_t> c[2], z;
        Planes()
        {
            c[0].assign(kPix, 0);
            c[1].assign(kPix, 0);
            z.assign(kPix, 0);
        }
    };

    struct Machines
    {
        Gs2dVk *vk = nullptr;
        bool useVk = true, useModel = true;
        Gif2dReader::Options reader;
        uint64_t scenes = 0;

        struct Result
        {
            Planes emu, model, vk;
            DrawList list;
            bool vkOk = true, modelRun = false;
            uint64_t dropped = 0; // primitives the reader left out (FRAME.FBW 0, or a register with no default unknown)
        };

        Result run(const std::vector<uint8_t> &packet, const Planes &seed)
        {
            ++scenes;
            Result r;
            // (1) the emulated GS, alone, on 4 MB of local memory.
            {
                static std::vector<uint8_t> vram(4u << 20);
                std::fill(vram.begin(), vram.end(), 0);
                auto gs = std::make_unique<GS>();
                gs->init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
                for (uint32_t t = 0; t < 2; ++t)
                    for (uint32_t y = 0; y < kH; ++y)
                        for (uint32_t x = 0; x < kW; ++x)
                            gs->WriteVram(GS_PSM_CT32, t ? 0x50u * 32u : 0u, 10, x, y, seed.c[t][y * kW + x]);
                for (uint32_t y = 0; y < kH; ++y)
                    for (uint32_t x = 0; x < kW; ++x)
                        gs->WriteVram(GS_PSM_Z24, 0xA0u * 32u, 10, x, y, seed.z[y * kW + x]);
                gs->processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
                for (uint32_t t = 0; t < 2; ++t)
                    for (uint32_t y = 0; y < kH; ++y)
                        for (uint32_t x = 0; x < kW; ++x)
                            r.emu.c[t][y * kW + x] = gs->ReadVram(GS_PSM_CT32, t ? 0x50u * 32u : 0u, 10, x, y);
                for (uint32_t y = 0; y < kH; ++y)
                    for (uint32_t x = 0; x < kW; ++x)
                        r.emu.z[y * kW + x] = gs->ReadVram(GS_PSM_Z24, 0xA0u * 32u, 10, x, y) & 0xFFFFFFu;
            }
            // The reader: the same bytes as one PATH3 kick.
            {
                Gif2dReader rd(reader);
                rd.startBlank();
                rd.cycleBegin(0);
                rd.feedPath3(packet.data(), static_cast<uint32_t>(packet.size()));
                rd.cycleEnd(rc::rec::CycleInfo{}, nullptr);
                r.list = rd.list();
                r.dropped = rd.stats().dropped;
            }
            // (2) the model.
            if (useModel)
            {
                GsModel m;
                m.colour[0] = seed.c[0];
                m.colour[1] = seed.c[1];
                m.depth = seed.z;
                m.draw(r.list);
                r.model.c[0] = m.colour[0];
                r.model.c[1] = m.colour[1];
                r.model.z = m.depth;
                r.modelRun = true;
            }
            // (3) Vulkan.
            if (useVk && vk)
            {
                r.vkOk = vk->seed(1, seed.c[1].data(), seed.z.data()) && vk->seed(0, seed.c[0].data(), seed.z.data()) && vk->draw(r.list) &&
                         vk->readColour(0, r.vk.c[0]) && vk->readColour(1, r.vk.c[1]) && vk->readDepth(r.vk.z);
            }
            return r;
        }
    };

    // Plane comparison: count and first difference.
    struct Diff
    {
        uint64_t n = 0;
        uint32_t x = 0, y = 0, a = 0, b = 0;
        int plane = -1;
    };
    Diff compare(const Planes &p, const Planes &q, bool colour = true, bool depth = true)
    {
        Diff d;
        for (int t = 0; t < 3; ++t)
        {
            if ((t < 2 && !colour) || (t == 2 && !depth))
                continue;
            const std::vector<uint32_t> &a = t < 2 ? p.c[t] : p.z, &b = t < 2 ? q.c[t] : q.z;
            for (uint32_t i = 0; i < kPix; ++i)
                if (a[i] != b[i] && d.n++ == 0)
                {
                    d.x = i % kW;
                    d.y = i / kW;
                    d.a = a[i];
                    d.b = b[i];
                    d.plane = t;
                }
        }
        return d;
    }
    std::string diffText(const Diff &d, const char *an, const char *bn)
    {
        char buf[200];
        static const char *const planes[3] = {"FBP 0x0", "FBP 0x50", "Z"};
        std::snprintf(buf, sizeof(buf), "%" PRIu64 " differ (first %s at %u,%u: %s %08x, %s %08x)", d.n, d.plane >= 0 ? planes[d.plane] : "-", d.x,
                      d.y, an, d.a, bn, d.b);
        return buf;
    }

    // ---------------------------------------------------------------- tests
    struct Test
    {
        std::string name;
        uint64_t checks = 0, failures = 0;
        bool failFast = false;
        uint64_t readRowFails = 0, exactRowFails = 0; // T1: rows (scenes) failing, by plan (a reading one or integer exact)
        std::vector<std::string> notes;
        bool fail(const std::string &what)
        {
            if (++failures <= 8 && !g_quiet)
                std::printf("  FAIL %s: %s\n", name.c_str(), what.c_str());
            return !failFast;
        }
        bool ok() const { return failures == 0; }
    };

    // Exact equality of the three machines (where the test asks for it).
    bool threeEqual(Test &t, const Machines::Result &r, const char *what, bool model = true, bool vk = true)
    {
        ++t.checks;
        bool good = true;
        if (model && r.modelRun) // the fault runs leave the model out
        {
            const Diff d = compare(r.emu, r.model);
            if (d.n)
                good = t.fail(std::string(what) + ": model " + diffText(d, "emu", "model"));
        }
        if (vk)
        {
            if (!r.vkOk)
                return t.fail(std::string(what) + ": the Vulkan path failed");
            const Diff d = compare(r.emu, r.vk);
            if (d.n)
                good = t.fail(std::string(what) + ": Vulkan " + diffText(d, "emu", "vk")) && good;
        }
        return good;
    }

    Planes pattern(uint32_t seed, uint8_t alpha)
    {
        Planes p;
        uint32_t s = seed * 2654435761u + 1u;
        for (int t = 0; t < 2; ++t)
            for (uint32_t i = 0; i < kPix; ++i)
            {
                s = s * 1664525u + 1013904223u;
                p.c[t][i] = (s >> 8 & 0x00FFFFFFu) | static_cast<uint32_t>(alpha) << 24;
            }
        return p;
    }

    // --- 9.1: the model against itself, and 4.3's percentages.
    struct Form
    {
        uint64_t alpha;  // ALPHA without FIX
        bool fixForm;
        bool pabe;
        const char *what;
    };
    const Form kForms[] = {
        {0x29, true, false, "Cd*F+Cs (0x29)"},       {0x68, true, false, "(Cs*F>>7)+Cd (0x68)"}, {0xA1, true, false, "(Cd-Cs)*F (0xa1)"},
        {0x62, true, false, "(-Cs*F>>7)+Cd (0x62)"}, {0x21, true, false, "lerp FIX (0x21)"},      {0x21, true, true, "0x21 with PABE"},
        {0x09, false, false, "Cd*As+Cs (0x09)"},     {0x01, false, false, "lerp As (0x01)"},     {0x44, false, false, "(Cs-Cd)*As+Cd (0x44)"},
        {0x08, false, false, "(Cs*As>>7)+Cs (0x08)"}, {0x44, false, true, "0x44 with PABE"},
    };
    const uint32_t kFixes[] = {0x15, 0x20, 0x3F, 0x40, 0x7F, 0x80};

    GsState formState(const Form &f, uint32_t fix)
    {
        GsState s{};
        s.frame = kFrame0;
        s.zbuf = kZbuf;
        s.prim = 0x43;
        s.alpha = f.alpha | static_cast<uint64_t>(fix) << 32;
        s.pabe = f.pabe ? 1 : 0;
        s.colclamp = 1;
        s.test = f.pabe ? testOf(3, 0x7F, 0, 1) : kTestNone;
        s.texture = 0xFFFFFFFFu;
        return s;
    }

    void modelSelfCheck(Test &t)
    {
        uint64_t total = 0;
        for (const Form &f : kForms)
        {
            const std::vector<uint32_t> fs = f.fixForm ? (f.pabe ? std::vector<uint32_t>{0x80} : std::vector<uint32_t>(std::begin(kFixes), std::end(kFixes)))
                                                       : std::vector<uint32_t>{};
            uint64_t exact = 0, plus = 0, tie = 0, n = 0, bad = 0;
            auto one = [&](const GsPlan &p, uint32_t fix, uint32_t as, const GsState &s)
            {
                for (uint32_t cs = 0; cs < 256; ++cs)
                    for (uint32_t cd = 0; cd < 256; ++cd)
                    {
                        uint32_t gs = gsBlend(s.alpha, cs, cd, as);
                        if (f.pabe && as < 0x80)
                            gs = cs;
                        const Ff ff = ffBlend(p, cs, cd, as);
                        ++n;
                        const bool certain = p.exactBlend || ffBiasApplies(p, cs, as);
                        if (ff.lo < gs || ff.hi > gs + 1u || ff.lo > ff.hi || (certain && (ff.lo != gs || ff.hi != gs)))
                        {
                            if (bad++ == 0)
                            {
                                char buf[160];
                                std::snprintf(buf, sizeof(buf), "%s FIX %02x As %02x Cs %u Cd %u: GS %u, ffBlend %u..%u", f.what, fix, as, cs, cd, gs, ff.lo, ff.hi);
                                t.fail(buf);
                            }
                        }
                        if (ff.lo == ff.hi)
                            (ff.lo == gs ? exact : plus)++;
                        else
                            ++tie;
                    }
                (void)fix;
            };
            if (f.fixForm)
                for (uint32_t fix : fs)
                {
                    const GsState s = formState(f, fix);
                    one(planState(s), fix, 0x40, s);
                }
            else
                for (uint32_t as = 0; as <= (f.pabe ? 0x7Fu : 0x80u); ++as)
                {
                    const GsState s = formState(f, 0);
                    one(planState(s), 0, as, s);
                }
            total += n;
            ++t.checks;
            if (!g_quiet)
                std::printf("  model %-24s %10" PRIu64 " triples: GS exactly %6.2f%%, one above %5.2f%%, tie %5.2f%%%s\n", f.what, n, 100.0 * exact / n,
                            100.0 * plus / n, 100.0 * tie / n, bad ? "  WRONG" : "");
        }
        // 4.3's figures: over Cs, Cd and F in 0..0x80 (F = FIX), with and without the bias.
        for (uint32_t cmax : {0x80u, 0xFFu})
        for (int lerp = 0; lerp < 2; ++lerp)
            for (int bias = 0; bias < 2; ++bias)
            {
                uint64_t n = 0, exact = 0, plus = 0, tie = 0;
                for (uint32_t fix = 0; fix <= 0x80; ++fix)
                {
                    const GsState s = formState(kForms[lerp ? 4 : 0], fix);
                    const GsPlan p = planState(s);
                    for (uint32_t cs = 0; cs <= cmax; ++cs)
                        for (uint32_t cd = 0; cd <= cmax; ++cd)
                        {
                            const uint32_t gs = gsBlend(s.alpha, cs, cd, 0x40);
                            const Ff ff = ffBlend(p, cs, cd, 0x40, bias != 0);
                            ++n;
                            if (ff.lo != ff.hi)
                                ++tie;
                            else
                                (ff.lo == gs ? exact : plus)++;
                        }
                }
                if (!g_quiet)
                    std::printf("  4.3 %s, bias %s, Cs Cd in 0..0x%02x, F in 0..0x80: exact %.2f%%, one above %.2f%%, tie %.2f%%\n",
                                lerp ? "lerps (0x21)" : "Cd*F+Cs (0x29)", bias ? "on " : "off", cmax, 100.0 * exact / n, 100.0 * plus / n, 100.0 * tie / n);
                t.notes.push_back(std::string(lerp ? "lerp" : "Cd*F+Cs") + (bias ? " bias" : " nobias") + (cmax == 0x80 ? " C<=0x80" : " C<=0xff") + ": exact " + std::to_string(100.0 * exact / n) +
                                  "% +1 " + std::to_string(100.0 * plus / n) + "% tie " + std::to_string(100.0 * tie / n) + "%");
            }
        if (!g_quiet)
            std::printf("  model: %" PRIu64 " (Cs, Cd, F) triples checked\n", total);
    }

    // --- T1 blend sweep.
    // read: Vulkan = gsBlend on every channel (the exact blend, 18.3). Fixed function: the integer-exact rows within
    // ffBlend's lo..hi (equal to the GS), the others within D-2D-1's measured bound (-1..+1 with the bias, -1..+2 without).
    void testT1(Test &t, Machines &m, bool bias, bool read)
    {
        const Gs2dVk::Options before = m.vk ? m.vk->options() : Gs2dVk::Options{};
        setMode(m.vk, read, bias);
        const char *mode = read ? "read" : bias ? "ff bias on" : "ff bias off";
        Planes seed;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
            {
                const uint32_t c = x & 255u;
                seed.c[0][y * kW + x] = rgbaOf(static_cast<uint8_t>(c), static_cast<uint8_t>(255 - c), static_cast<uint8_t>((37 * c) & 255), 0x40);
            }
        uint64_t pixels = 0, equal = 0, above = 0, designMisses = 0;
        std::map<std::string, std::array<uint64_t, 3>> perForm;
        std::map<std::string, std::array<uint64_t, 9>> hist; // Vulkan - GS per channel, -4..4
        for (const Form &f : kForms)
        {
            std::vector<std::pair<uint32_t, uint32_t>> runs; // (fix, as)
            if (f.fixForm)
                for (uint32_t fix : (f.pabe ? std::vector<uint32_t>{0x80} : std::vector<uint32_t>(std::begin(kFixes), std::end(kFixes))))
                    runs.push_back({fix, f.pabe ? 0x40u : 0x80u});
            else
                for (uint32_t as = 0; as <= (f.pabe ? 0x7Fu : 0x80u); ++as)
                    runs.push_back({0, as});
            for (const auto &[fix, as] : runs)
            {
                Gif g;
                Env e;
                e.zbuf = kZbuf | kZmsk;
                e.alpha = f.alpha | static_cast<uint64_t>(fix) << 32;
                e.pabe = f.pabe;
                e.test = f.pabe ? testOf(3, 0x7F, 0, 1) : kTestNone;
                env(g, e);
                for (uint32_t y = 0; y < 256; ++y)
                {
                    V a;
                    a.x = 0;
                    a.y = y;
                    a.r = a.g = static_cast<uint8_t>(y);
                    a.b = static_cast<uint8_t>(255 - y);
                    a.a = static_cast<uint8_t>(as);
                    V b = a;
                    b.x = 256;
                    b.y = y + 1;
                    rect(g, 0x43, a, b);
                }
                const Machines::Result r = m.run(g.bytes(), seed);
                const GsState s = formState(f, fix);
                const GsPlan p = planState(s);
                char what[96];
                std::snprintf(what, sizeof(what), "%s FIX %02x As %02x %s", f.what, fix, as, mode);
                // Emulated = gsBlend everywhere; Vulkan within ffBlend; stored alpha As.
                uint64_t emuBad = 0, vkBad = 0, designBad = 0;
                std::string first, firstDesign;
                for (uint32_t y = 0; y < 256 && (emuBad + vkBad == 0 || !t.failFast); ++y)
                    for (uint32_t x = 0; x < 256; ++x)
                    {
                        const uint32_t i = y * kW + x;
                        const Rgba d = Rgba::unpack(seed.c[0][i]);
                        const Rgba src{static_cast<uint8_t>(y), static_cast<uint8_t>(y), static_cast<uint8_t>(255 - y), static_cast<uint8_t>(as)};
                        const Rgba em = Rgba::unpack(r.emu.c[0][i]);
                        const uint8_t sc[3] = {src.r, src.g, src.b}, dc[3] = {d.r, d.g, d.b}, ec[3] = {em.r, em.g, em.b};
                        for (int k = 0; k < 3; ++k)
                        {
                            uint32_t gs = gsBlend(e.alpha, sc[k], dc[k], as);
                            if (f.pabe && as < 0x80)
                                gs = sc[k];
                            if (ec[k] != gs && emuBad++ == 0)
                                first = "emulated " + std::to_string(ec[k]) + " gsBlend " + std::to_string(gs) + " at " + std::to_string(x) + "," + std::to_string(y);
                        }
                        if (em.a != as && emuBad++ == 0)
                            first = "emulated alpha";
                        if (!m.vk)
                            continue;
                        const Rgba vk = Rgba::unpack(r.vk.c[0][i]);
                        const uint8_t vc[3] = {vk.r, vk.g, vk.b};
                        ++pixels;
                        bool same = true, plus = false;
                        for (int k = 0; k < 3; ++k)
                        {
                            uint32_t gs = gsBlend(e.alpha, sc[k], dc[k], as);
                            if (f.pabe && as < 0x80)
                                gs = sc[k];
                            const int dv = static_cast<int>(vc[k]) - static_cast<int>(gs);
                            const Ff ff = ffBlend(p, sc[k], dc[k], as, bias);
                            // read: equality with the GS. Fixed function: an integer-exact row within ffBlend's lo..hi
                            // (equality with the GS), the others within D-2D-1's bound.
                            const bool bad = read ? dv != 0 : p.exactBlend ? (vc[k] < ff.lo || vc[k] > ff.hi) : (dv < -1 || dv > (bias ? 1 : 2));
                            if (bad)
                            {
                                const std::string at = "Vulkan " + std::to_string(vc[k]) + " GS " + std::to_string(gs) + " ffBlend " + std::to_string(ff.lo) + ".." +
                                                       std::to_string(ff.hi) + " at " + std::to_string(x) + "," + std::to_string(y) + " channel " + std::to_string(k);
                                if (p.exactBlend || read)
                                {
                                    if (vkBad++ == 0)
                                        first = at;
                                }
                                else if (designBad++ == 0)
                                    firstDesign = at;
                            }
                            hist[f.what][std::clamp(static_cast<int>(vc[k]) - static_cast<int>(ec[k]), -4, 4) + 4]++;
                            if (vc[k] != ec[k])
                                same = false;
                            if (vc[k] == ec[k] + 1u)
                                plus = true;

                        }
                        if (vk.a != as && vkBad++ == 0)
                            first = "Vulkan alpha " + std::to_string(vk.a);
                        equal += same;
                        above += plus;
                        auto &pf = perForm[f.what];
                        ++pf[0];
                        pf[1] += same;
                        pf[2] += plus;
                    }
                ++t.checks;
                if (vkBad + designBad)
                    ++(p.exactBlend ? t.exactRowFails : t.readRowFails);
                if (emuBad || vkBad)
                {
                    if (!t.fail(std::string(what) + ": " + std::to_string(emuBad) + " emulated, " + std::to_string(vkBad) + " Vulkan; " + first))
                        break;
                }
                designMisses += designBad;
                if (designBad)
                {
                    if (!t.fail(std::string(what) + ": D-2D-1's bound (-1.." + (bias ? "+1" : "+2") + "): " + std::to_string(designBad) + " channels; " + firstDesign))
                        break;
                }
                if (!r.vkOk && !t.fail(std::string(what) + ": Vulkan path failed"))
                    break;
            }
        }
        if (!g_quiet)
        {
            for (const auto &[name, h] : hist)
            {
                std::printf("  T1 %-11s %-24s Vulkan - GS per channel:", mode, name.c_str());
                for (int k = 0; k < 9; ++k)
                    if (h[k])
                        std::printf(" %+d: %" PRIu64, k - 4, h[k]);
                std::printf("\n");
            }
            for (const auto &[name, c] : perForm)
                std::printf("  T1 %-11s %-24s %9" PRIu64 " pixels: equal to the GS %6.2f%%, a channel one above %5.2f%%\n", mode, name.c_str(),
                            c[0], 100.0 * c[1] / c[0], 100.0 * c[2] / c[0]);
            std::printf("  T1 %s: %" PRIu64 " pixels, %" PRIu64 " equal to the GS in all channels, %" PRIu64 " with a channel one above\n", mode,
                        pixels, equal, above);
        }
        t.notes.push_back(std::string(mode) + ": " + std::to_string(pixels) + " pixels, " + std::to_string(equal) + " equal, " +
                          std::to_string(above) + " +1; channels outside D-2D-1's bound " + std::to_string(designMisses));
        for (const auto &[name, h] : hist)
        {
            std::string line = std::string(mode) + " " + name + ":";
            for (int k = 0; k < 9; ++k)
                if (h[k])
                    line += " " + std::string(k >= 4 ? "+" : "") + std::to_string(k - 4) + ": " + std::to_string(h[k]);
            t.notes.push_back(line);
        }
        if (m.vk)
            m.vk->setOptions(before);
    }

    // --- L (4.4): inexactly blended primitives over each pixel, from the list.
    std::vector<uint8_t> layerCount(const DrawList &list)
    {
        std::vector<uint8_t> L(kPix, 0);
        GsState s{};
        GsPlan p{};
        for (const DrawCmd &c : list.cmds)
        {
            if (c.op == kOpState)
            {
                s = list.state(c);
                p = planState(s);
            }
            else if (c.op == kOpScreenPrims && c.a == kKindTriangles)
            {
                const GsVertex *v = list.vertices(c);
                for (uint32_t k = 0; k < c.b; ++k)
                {
                    const GsVertex *tv = v + k * 3u;
                    if (p.blend == kBlendOff || p.exactBlend)
                        continue;
                    GsTri t;
                    if (!gsTriSetup(tv, s.xyoffset & 0xFFFFu, (s.xyoffset >> 32) & 0xFFFFu, t))
                        continue;
                    const int x0 = std::max({t.minX, 0, static_cast<int>(s.scissor & 0x7FF)}), x1 = std::min({t.maxX, 639, static_cast<int>((s.scissor >> 16) & 0x7FF)});
                    const int y0 = std::max({t.minY, 0, static_cast<int>((s.scissor >> 32) & 0x7FF)}), y1 = std::min({t.maxY, 255, static_cast<int>((s.scissor >> 48) & 0x7FF)});
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                            ++L[y * kW + x];
                }
            }
        }
        return L;
    }

    // --- T2 layers.
    // read: every plane equal in the three machines (no L). Fixed function (bias on): -L..+L (D-2D-1).
    void testT2(Test &t, Machines &m, bool read)
    {
        const Gs2dVk::Options before = m.vk ? m.vk->options() : Gs2dVk::Options{};
        setMode(m.vk, read, true);
        struct Layer
        {
            uint64_t alpha;
            uint32_t fix;
            Rgba c;
            int x0, y0, x1, y1;
        };
        const Layer layers[][6] = {
            {{0x09, 0, {0, 3, 200, 0x30}, 10, 10, 300, 200},
             {0x01, 0, {1, 90, 0, 0x50}, 50, 30, 350, 220},
             {0x21, 0x20, {0, 2, 160, 0x80}, 90, 50, 400, 240},
             {0x29, 0x80, {5, 0, 1, 0x40}, 130, 5, 450, 150},
             {0xA1, 0x80, {3, 7, 0, 0x40}, 20, 100, 500, 250},
             {0x44, 0, {255, 1, 0, 0x60}, 0, 0, 640, 256}},
            {{0x21, 0x7F, {1, 1, 1, 0x80}, 0, 0, 640, 256},
             {0x21, 0x15, {0, 0, 2, 0x80}, 0, 0, 640, 256},
             {0x29, 0x3F, {0, 1, 0, 0x80}, 0, 0, 640, 256},
             {0x01, 0, {2, 0, 1, 0x7F}, 0, 0, 640, 256},
             {0x09, 0, {0, 1, 0, 0x41}, 0, 0, 640, 256},
             {0x68, 0x33, {9, 200, 3, 0x80}, 0, 0, 640, 256}},
        };
        uint32_t maxL = 0;
        uint64_t plusPixels = 0, designMisses = 0;
        double lowest = 0.0, highest = 0.0;
        for (int v = 0; v < 2; ++v)
            for (int seedN = 0; seedN < 4; ++seedN)
            {
                const Planes seed = pattern(100 + seedN + v * 10, 0x40);
                Gif g;
                Env e;
                env(g, e);
                for (const Layer &l : layers[v])
                {
                    g.ad(kGsAlpha, l.alpha | static_cast<uint64_t>(l.fix) << 32);
                    V a;
                    a.x = l.x0;
                    a.y = l.y0;
                    a.r = l.c.r;
                    a.g = l.c.g;
                    a.b = l.c.b;
                    a.a = l.c.a;
                    V b = a;
                    b.x = l.x1;
                    b.y = l.y1;
                    rect(g, 0x43, a, b);
                }
                const Machines::Result r = m.run(g.bytes(), seed);
                const std::string what = std::string(read ? "read" : "ff") + " layers " + std::to_string(v) + " seed " + std::to_string(seedN);
                threeEqual(t, r, what.c_str(), true, read);
                if (!m.vk || read)
                    continue;
                const std::vector<uint8_t> L = layerCount(r.list);
                uint64_t bad = 0, hard = 0;
                std::string first, firstHard;
                for (uint32_t i = 0; i < kPix; ++i)
                {
                    const Rgba a = Rgba::unpack(r.emu.c[0][i]), b = Rgba::unpack(r.vk.c[0][i]);
                    const int d[3] = {b.r - a.r, b.g - a.g, b.b - a.b};
                    maxL = std::max<uint32_t>(maxL, L[i]);
                    bool plus = false;
                    for (int k = 0; k < 3; ++k)
                    {
                        if ((d[k] < -L[i] || d[k] > L[i]) && bad++ == 0)
                            first = "at " + std::to_string(i % kW) + "," + std::to_string(i / kW) + " channel " + std::to_string(k) + ": Vulkan - GS " +
                                    std::to_string(d[k]) + ", L " + std::to_string(L[i]);
                        if (L[i] == 0 && d[k] != 0 && hard++ == 0)
                            firstHard = "at " + std::to_string(i % kW) + "," + std::to_string(i / kW) + ": no inexact layer but Vulkan - GS " + std::to_string(d[k]);
                        plus |= d[k] > 0;
                        lowest = std::min(lowest, L[i] ? static_cast<double>(d[k]) / L[i] : 0.0);
                        highest = std::max(highest, L[i] ? static_cast<double>(d[k]) / L[i] : 0.0);
                    }
                    plusPixels += plus;
                    if (a.a != b.a && hard++ == 0)
                        firstHard = "alpha differs";
                }
                ++t.checks;
                designMisses += bad;
                if (bad)
                    t.fail(what + ": D-2D-1's bound: " + std::to_string(bad) + " channels outside -L..+L; " + first);
                if (hard)
                    t.fail(what + ": " + std::to_string(hard) + " exact pixels differ; " + firstHard);
                if (r.emu.z != r.vk.z)
                    t.fail(what + ": Z differs");
            }
        if (m.vk)
            m.vk->setOptions(before);
        if (read)
            return;
        char buf[200];
        std::snprintf(buf, sizeof(buf), "ff: largest L %u, pixels with a channel above the GS %" PRIu64 " of %u, channels outside -L..+L %" PRIu64
                      ", (Vulkan - GS) / L from %.2f to %.2f", maxL, plusPixels, kPix * 8u, designMisses, lowest, highest);
        if (!g_quiet)
            std::printf("  T2: %s\n", buf);
        t.notes.push_back(buf);
    }

    // --- T3 alpha test.
    // read: also the front end's panel state (0x01 (Cd-Cs)*As+Cs, ATST LEQUAL, RGB_ONLY, Z GREATER with Z write) on
    // the textured ramp and the overlapping rectangles: the split and the read together (18.3).
    void testT3(Test &t, Machines &m, bool read)
    {
        static const uint32_t arefs[] = {0, 1, 0x2F, 0x3F, 0x80};
        Planes seed = pattern(7, 0x33);
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
                seed.z[y * kW + x] = y % 3 == 0 ? 0x4000u : y % 3 == 1 ? 0x8000u : 0xC000u;
        for (uint32_t y = 16; y < 40; ++y) // the overlap region: Z below everything
            for (uint32_t x = 0; x < kW; ++x)
                seed.z[y * kW + x] = 0x1000u;
        // A 256 x 1 CT32 alpha ramp: texel c has alpha min(c, 0x80) and a colour pattern.
        std::vector<uint8_t> ramp(256 * 4);
        for (uint32_t c = 0; c < 256; ++c)
        {
            ramp[c * 4] = static_cast<uint8_t>(c * 3);
            ramp[c * 4 + 1] = static_cast<uint8_t>(255 - c);
            ramp[c * 4 + 2] = static_cast<uint8_t>(c ^ 0x5A);
            ramp[c * 4 + 3] = static_cast<uint8_t>(std::min<uint32_t>(c, 0x80));
        }
        uint32_t config = 0;
        for (uint32_t atst = 0; atst < 8; ++atst)
            for (uint32_t afail = 0; afail < 4; ++afail)
                for (uint32_t zmsk = 0; zmsk < 2; ++zmsk)
                    for (uint32_t aref : arefs)
                    {
                        ++config;
                        Gif g;
                        g.image(0x3000, 4, 0x00, 256, 1, ramp);
                        Env e;
                        e.frame = (config & 1u) ? kFrame1 : kFrame0;
                        e.zbuf = kZbuf | (zmsk ? kZmsk : 0);
                        e.test = testOf(atst, aref, afail, 3); // Z GREATER
                        e.tex0 = tex0Of(0x3000, 4, 0x00, 8, 0, 1, 1); // DECAL, TCC 1
                        e.tex1 = 0;                                     // nearest
                        env(g, e);
                        // Flat columns, As = 0..0x80 (one value a primitive: decided on the CPU).
                        for (uint32_t c = 0; c <= 0x80; ++c)
                        {
                            V a;
                            a.x = c * 2;
                            a.y = 0;
                            a.z = 0x8000;
                            a.r = static_cast<uint8_t>(c * 2);
                            a.g = static_cast<uint8_t>(255 - c);
                            a.b = 77;
                            a.a = static_cast<uint8_t>(c);
                            V b = a;
                            b.x = c * 2 + 2;
                            b.y = 7;
                            rect(g, 0x03, a, b);
                        }
                        // The textured ramp: As per pixel.
                        V a;
                        a.x = 0;
                        a.y = 8;
                        a.z = 0x8000;
                        a.s = 0.5f / 256.0f;
                        a.t = 0.5f;
                        V b = a;
                        b.x = 129;
                        b.y = 15;
                        b.s = 129.5f / 256.0f;
                        b.t = 0.5f;
                        rect(g, 0x13, a, b);
                        // Two overlapping textured rectangles in one batch, at equal Z then at rising Z.
                        for (int k = 0; k < 2; ++k)
                        {
                            V p = a, q = b;
                            p.y = 16 + k * 12;
                            q.y = p.y + 10;
                            p.x = 0;
                            q.x = 120;
                            p.s = 0.5f / 256.0f;
                            q.s = 120.5f / 256.0f;
                            rect(g, 0x13, p, q);
                            p.x = 60;
                            q.x = 200;
                            p.s = 100.5f / 256.0f; // a different ramp phase, so the parts differ in the overlap
                            q.s = 240.5f / 256.0f;
                            p.z = q.z = k ? 0x9000u : 0x8000u;
                            rect(g, 0x13, p, q);
                        }
                        const Machines::Result r = m.run(g.bytes(), seed);
                        char what[96];
                        std::snprintf(what, sizeof(what), "ATST %u AREF %02x AFAIL %u ZMSK %u", atst, aref, afail, zmsk);
                        if (!threeEqual(t, r, what) && t.failFast)
                            return;
                    }
        uint32_t panels = 0;
        for (uint32_t aref : {1u, 0x2Fu, 0x3Fu})
            for (uint32_t frame = 0; read && frame < 2; ++frame)
                for (uint32_t tfx : {0u, 1u})
                {
                    ++panels;
                    Gif g;
                    g.image(0x3000, 4, 0x00, 256, 1, ramp);
                    Env e;
                    e.frame = frame ? kFrame1 : kFrame0;
                    e.zbuf = kZbuf;                                     // Z write on
                    e.test = testOf(3, aref, 3, 3);                     // LEQUAL, RGB_ONLY, Z GREATER
                    e.alpha = 0x01;                                     // (Cd - Cs) * As + Cs
                    e.tex0 = tex0Of(0x3000, 4, 0x00, 8, 0, 1, tfx);     // TCC 1, MODULATE or DECAL
                    e.tex1 = 0x60;                                      // bilinear, as the front end
                    env(g, e);
                    V a;
                    a.x = 0;
                    a.y = 8;
                    a.z = 0x8000;
                    a.r = 0x60;
                    a.g = 0x90;
                    a.b = 0x40;
                    a.a = 0x80;
                    a.s = 0.5f / 256.0f;
                    a.t = 0.5f;
                    V b = a;
                    b.x = 129;
                    b.y = 15;
                    b.s = 129.5f / 256.0f;
                    rect(g, 0x53, a, b);
                    for (int k = 0; k < 2; ++k)
                    {
                        V p = a, q = b;
                        p.y = 16 + k * 12;
                        q.y = p.y + 10;
                        p.x = 0;
                        q.x = 120;
                        p.s = 0.5f / 256.0f;
                        q.s = 120.5f / 256.0f;
                        rect(g, 0x53, p, q);
                        p.x = 60;
                        q.x = 200;
                        p.s = 100.5f / 256.0f;
                        q.s = 240.5f / 256.0f;
                        p.z = q.z = k ? 0x9000u : 0x8000u;
                        rect(g, 0x53, p, q);
                    }
                    const Machines::Result r = m.run(g.bytes(), seed);
                    char what[96];
                    std::snprintf(what, sizeof(what), "panels (0x01, RGB_ONLY) AREF %02x TFX %u FBP %s", aref, tfx, frame ? "0x50" : "0x0");
                    if (!threeEqual(t, r, what) && t.failFast)
                        return;
                }
        if (!g_quiet)
            std::printf("  T3: %u alpha-test configurations, %u with the panels' state (read)\n", config, panels);
    }

    // --- T4 depth.
    void testT4(Test &t, Machines &m)
    {
        static const uint32_t stored[] = {0, 1, 2, 0x7FFFFF, 0xFFFFFD, 0xFFFFFE, 0xFFFFFF};
        static const uint32_t drawn[] = {0, 1, 0xFFFFFE, 0xFFFFFF, 2, 0xFFFFFD};
        Planes seed = pattern(3, 0x22);
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
                seed.z[y * kW + x] = stored[x % 7];
        for (uint32_t ztst = 0; ztst < 4; ++ztst)
            for (uint32_t zmsk = 0; zmsk < 2; ++zmsk)
            {
                Gif g;
                Env e;
                e.zbuf = kZbuf | (zmsk ? kZmsk : 0);
                e.test = 1u << 16 | ztst << 17;
                env(g, e);
                for (uint32_t k = 0; k < 6; ++k)
                {
                    V a;
                    a.x = 0;
                    a.y = k * 8;
                    a.z = drawn[k];
                    a.r = static_cast<uint8_t>(40 * k);
                    a.g = 200;
                    a.b = static_cast<uint8_t>(k);
                    a.a = 0x55;
                    V b = a;
                    b.x = 640;
                    b.y = k * 8 + 8;
                    rect(g, 0x03, a, b);
                }
                // The flip's clear (Z32, Z 1), then a band over it.
                g.ad(kGsZbuf, 0xA0);
                g.ad(kGsTest, kTestNone);
                V c;
                c.x = 0;
                c.y = 100;
                c.z = 1;
                c.r = 0x10;
                c.g = 0x20;
                c.b = 0x30;
                c.a = 0xFF;
                V d = c;
                d.x = 640;
                d.y = 200;
                sprite(g, 0x06, c, d);
                g.ad(kGsZbuf, kZbuf | (zmsk ? kZmsk : 0));
                g.ad(kGsTest, 1u << 16 | ztst << 17);
                V a;
                a.x = 0;
                a.y = 120;
                a.z = 1;
                a.r = 0xEE;
                V b = a;
                b.x = 320;
                b.y = 140;
                rect(g, 0x03, a, b);
                a.y = 150;
                a.z = 2;
                b.y = 170;
                rect(g, 0x03, a, b);
                const Machines::Result r = m.run(g.bytes(), seed);
                char what[64];
                std::snprintf(what, sizeof(what), "ZTST %u ZMSK %u", ztst, zmsk);
                if (!threeEqual(t, r, what) && t.failFast)
                    return;
            }
    }

    // --- T5 coverage (additive blend: R counts the primitives over a pixel).
    void testT5(Test &t, Machines &m)
    {
        const Planes zero;
        auto addEnv = [](Gif &g, uint64_t scissor)
        {
            Env e;
            e.alpha = 0x29 | 0x80ull << 32; // Cd + Cs
            e.scissor = scissor;
            env(g, e);
        };
        auto cv = [](double x, double y, uint8_t id)
        {
            V v;
            v.x = x;
            v.y = y;
            v.r = 1;
            v.g = id;
            v.b = 0;
            v.a = 0x80;
            return v;
        };
        const uint64_t full = scissorOf(0, 639, 0, 255);
        std::vector<std::pair<std::string, std::vector<uint8_t>>> scenes;
        { // every subpixel offset of each vertex in turn
            Gif g;
            addEnv(g, full);
            uint32_t cell = 0;
            for (int vi = 0; vi < 3; ++vi)
                for (int dy = 0; dy < 16; ++dy)
                    for (int dx = 0; dx < 16; ++dx, ++cell)
                    {
                        const double ox = (cell % 40) * 16, oy = (cell / 40) * 12;
                        double px[3] = {2, 11, 4}, py[3] = {1, 4, 9};
                        px[vi] += dx / 16.0;
                        py[vi] += dy / 16.0;
                        tri(g, 0x43, cv(ox + px[0], oy + py[0], cell & 255), cv(ox + px[1], oy + py[1], cell & 255), cv(ox + px[2], oy + py[2], cell & 255));
                    }
            scenes.push_back({"subpixel offsets", g.bytes()});
        }
        { // rectangle halves with subpixel corners, slivers, fans through pixel centres
            Gif g;
            addEnv(g, full);
            for (int k = 0; k < 64; ++k)
            {
                const double x0 = (k % 16) * 40 + (k * 3 % 16) / 16.0, y0 = (k / 16) * 30 + (k * 5 % 16) / 16.0;
                const double x1 = x0 + 20 + (k * 7 % 16) / 16.0, y1 = y0 + 14 + (k * 11 % 16) / 16.0;
                V a = cv(x0, y0, static_cast<uint8_t>(k)), b = cv(x1, y1, static_cast<uint8_t>(k));
                rect(g, 0x43, a, b);
            }
            for (int k = 0; k < 8; ++k) // slivers: 1/16 pixel high, slanted
            {
                const double y = 130 + k * 3 + k / 16.0;
                tri(g, 0x43, cv(5, y, 1), cv(600, y + 1.0 / 16 + k / 8.0, 1), cv(5, y + 1.0 / 16, 1));
            }
            for (int f = 0; f < 4; ++f) // fans around an integer centre, outer vertices on integers
            {
                const double cx = 80 + f * 150, cy = 200;
                const int n = 12;
                for (int k = 0; k < n; ++k)
                {
                    const double a0 = 2 * M_PI * k / n, a1 = 2 * M_PI * (k + 1) / n;
                    tri(g, 0x43, cv(cx, cy, 2), cv(std::round(cx + 40 * std::cos(a0)), std::round(cy + 40 * std::sin(a0)), 2),
                        cv(std::round(cx + 40 * std::cos(a1)), std::round(cy + 40 * std::sin(a1)), 2));
                }
            }
            scenes.push_back({"halves, slivers, fans", g.bytes()});
        }
        { // a triangle reaching 1500 pixels off screen, every scissor edge
            Gif g;
            addEnv(g, full);
            tri(g, 0x43, cv(-1500, 100.5, 3), cv(300.25, -1400, 3), cv(2000, 1700.75, 3));
            const int sc[][4] = {{10, 100, 20, 60}, {0, 0, 0, 255}, {639, 639, 0, 255}, {0, 639, 255, 255}, {200, 210, 0, 0}, {300, 420, 100, 101}};
            for (const auto &s : sc)
            {
                g.ad(kGsScissor, scissorOf(s[0], s[1], s[2], s[3]));
                tri(g, 0x43, cv(-5.5, -3, 4), cv(700, 10.25, 4), cv(100, 300, 4));
            }
            scenes.push_back({"far vertices, scissor edges", g.bytes()});
        }
        { // lines in all eight octants with subpixel ends, crossing the scissor
            Gif g;
            addEnv(g, scissorOf(200, 440, 50, 200));
            for (int k = 0; k < 48; ++k)
            {
                const double ang = 2 * M_PI * (k + 0.37) / 48.0;
                const double x0 = 320 + (k % 16) / 16.0, y0 = 128 + (k * 7 % 16) / 16.0;
                const double x1 = std::round((320 + 160 * std::cos(ang)) * 16) / 16, y1 = std::round((128 + 120 * std::sin(ang)) * 16) / 16;
                V a = cv(x0, y0, static_cast<uint8_t>(k)), b = cv(x1, y1, static_cast<uint8_t>(k));
                a.z = 0x400;
                b.z = 0xAF4;
                line(g, 0x41, a, b);
            }
            g.ad(kGsScissor, full);
            for (int k = 0; k < 16; ++k) // short lines and single points
            {
                V a = cv(20 + k * 3 + k / 16.0, 240 + (k % 4) / 4.0, 9), b = cv(20 + k * 3 + (k % 3), 240 + (k % 5), 9);
                line(g, 0x49, a, b); // Gouraud (IIP) line
            }
            scenes.push_back({"lines", g.bytes()});
        }
        { // sprites (the clear's state) with subpixel corners
            Gif g;
            Env e;
            env(g, e);
            for (int k = 0; k < 120; ++k)
            {
                const double x0 = (k % 12) * 52 + (k % 16) / 16.0, y0 = (k / 12) * 25 + (k * 3 % 16) / 16.0;
                V a;
                a.x = x0;
                a.y = y0;
                V b;
                b.x = x0 + 30 + (k * 5 % 16) / 16.0;
                b.y = y0 + 12 + (k * 9 % 16) / 16.0;
                b.r = static_cast<uint8_t>(k * 2);
                b.g = static_cast<uint8_t>(255 - k);
                b.b = 7;
                b.a = 0xFF;
                b.z = 1;
                if (k & 1)
                    std::swap(a.x, b.x); // corners given in either order
                sprite(g, 0x06, a, b);
            }
            scenes.push_back({"sprites", g.bytes()});
        }
        for (auto &[name, bytes] : scenes)
        {
            const Machines::Result r = m.run(bytes, zero);
            uint64_t covered = 0;
            for (uint32_t i = 0; i < kPix; ++i)
                covered += (r.emu.c[0][i] & 0xFFu) != 0;
            if (!g_quiet)
                std::printf("  T5 %-28s %7" PRIu64 " pixels covered\n", name.c_str(), covered);
            if (!threeEqual(t, r, name.c_str()) && t.failFast)
                return;
        }
    }

    // --- T6 texture.
    struct TexSpec
    {
        const char *name;
        uint32_t psm, tw, th, tbw;
        uint64_t texa;
    };
    std::vector<uint8_t> textureBytes(const TexSpec &ts, uint32_t seed)
    {
        const uint32_t w = 1u << ts.tw, h = 1u << ts.th;
        std::vector<uint8_t> d;
        uint32_t s = seed * 747796405u + 1u;
        auto rnd = [&]() { return s = s * 1664525u + 1013904223u, s >> 8; };
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t r = rnd();
                switch (ts.psm)
                {
                case 0x00: // CT32: alpha spread 0..0xa0
                    d.push_back(static_cast<uint8_t>(r));
                    d.push_back(static_cast<uint8_t>(r >> 8));
                    d.push_back(static_cast<uint8_t>(x * 4 + y));
                    d.push_back(static_cast<uint8_t>((r >> 16) % 0xA1));
                    break;
                case 0x02: // CT16: some black texels for AEM
                {
                    const uint16_t v = (x + y) % 7 == 0 ? static_cast<uint16_t>(r & 0x8000u) : static_cast<uint16_t>(r);
                    d.push_back(static_cast<uint8_t>(v));
                    d.push_back(static_cast<uint8_t>(v >> 8));
                    break;
                }
                case 0x13: // T8
                    d.push_back(static_cast<uint8_t>(r));
                    break;
                default: // T4: two a byte, low nibble first
                    if (x & 1u)
                        d.back() = static_cast<uint8_t>(d.back() | (r & 0xF0u));
                    else
                        d.push_back(static_cast<uint8_t>(r & 0x0Fu));
                    break;
                }
            }
        return d;
    }

    void testT6(Test &t, Machines &m)
    {
        const TexSpec specs[] = {
            {"CT32 64x64", 0x00, 6, 6, 1, 0x80},
            {"CT16 64x64 (TEXA)", 0x02, 6, 6, 1, 0x30 | 1ull << 15 | 0x70ull << 32},
            {"T8 32x32", 0x13, 5, 5, 2, 0x80},
            {"T4 32x32", 0x14, 5, 5, 2, 0x80},
        };
        const Planes seed = pattern(11, 0x10);
        for (const TexSpec &ts : specs)
            for (int sceneN = 0; sceneN < 2; ++sceneN)
        {
            Gif g;
            const uint32_t tbp = 0x3000, cbp = 0x3800;
            g.image(tbp, ts.tbw, ts.psm, 1u << ts.tw, 1u << ts.th, textureBytes(ts, ts.psm));
            if (ts.psm == 0x13 || ts.psm == 0x14) // a CT32 palette (16 x 16, or 8 x 2)
            {
                const uint32_t pw = ts.psm == 0x13 ? 16 : 8, ph = ts.psm == 0x13 ? 16 : 2;
                std::vector<uint8_t> pal;
                for (uint32_t i = 0; i < pw * ph; ++i)
                {
                    pal.push_back(static_cast<uint8_t>(i * 37));
                    pal.push_back(static_cast<uint8_t>(255 - i * 11));
                    pal.push_back(static_cast<uint8_t>(i * 5));
                    pal.push_back(static_cast<uint8_t>(i % 9 == 0 ? 0 : (i * 3) % 0x90));
                }
                g.image(cbp, 1, 0x00, pw, ph, pal);
            }
            Env e;
            e.texa = ts.texa;
            env(g, e);
            const uint32_t tw = 1u << ts.tw, th = 1u << ts.th;
            int cell = 0;
            // One rectangle: position, size, S/T range, colours, TEX0/TEX1/CLAMP choices.
            auto draw = [&](double w, double h, float s0, float s1, float t0, float t1, uint32_t tfx, uint32_t tcc, uint32_t tex1, uint64_t clamp,
                            bool gouraud, bool oneColourIip, uint8_t vr, uint8_t va)
            {
                const double x0 = (cell % 5) * 128, y0 = (cell / 5) * 64;
                ++cell;
                g.ad(kGsTex0, tex0Of(tbp, ts.tbw, ts.psm, ts.tw, ts.th, tcc, tfx, cbp, 1));
                g.ad(kGsTex1, tex1);
                g.ad(kGsClamp, clamp);
                V a;
                a.x = x0;
                a.y = y0;
                a.s = s0;
                a.t = t0;
                a.r = vr;
                a.g = static_cast<uint8_t>(vr / 2 + 20);
                a.b = 0x80;
                a.a = va;
                V b = a;
                b.x = x0 + w;
                b.y = y0 + h;
                b.s = s1;
                b.t = t1;
                const uint32_t prim = 0x13 | ((gouraud || oneColourIip) ? 0x08u : 0u);
                if (!gouraud)
                {
                    rect(g, prim, a, b);
                    return;
                }
                V tr = a, bl = a, br = b;
                tr.x = b.x;
                tr.s = b.s;
                tr.r = 0xFF;
                tr.g = 0x10;
                bl.y = b.y;
                bl.t = b.t;
                bl.b = 0x20;
                br.r = 0x30;
                br.g = 0xC0;
                tri(g, prim, a, tr, bl);
                tri(g, prim, tr, br, bl);
            };
            const float hu = 0.5f / tw, hv = 0.5f / th;
            // Texels per pixel 1, 2, 1/2, 1.07, mirrored; K1's sampling on texel boundaries; nearest and linear.
            for (uint32_t tex1 : {0x00u, 0x60u})
            {
                if (sceneN != 0)
                    break;
                draw(tw, th, 0, 1, 0, 1, 0, 1, tex1, 5, false, false, 0x80, 0x80);
                draw(tw / 2.0, th / 2.0, 0, 1, 0, 1, 0, 1, tex1, 5, false, false, 0x80, 0x80);
                draw(std::min(2.0 * tw, 127.0), std::min(2.0 * th, 63.0), 0, 1, 0, 1, 0, 1, tex1, 5, false, false, 0x80, 0x80);
                draw(tw / 1.07, th / 1.07, 0.03f, 1.0f, 0.02f, 1.0f, 0, 1, tex1, 5, false, false, 0x80, 0x80);
                draw(tw, th, 1, 0, 1, 0, 0, 1, tex1, 5, false, false, 0x80, 0x80);                               // mirrored
                draw(tw, th * 0.75, hu, 1 + hu, 0, 1, 0, 1, tex1, 5, true, false, 0x80, 0x80);                  // Gouraud
                draw(tw, th, 0, 1, hv, 1 + hv, 0, 1, tex1, 5, false, true, 0x93, 0x80);                         // one colour under IIP
                draw(tw, th, 0, 1, 0, 1, 0, 1, tex1, 5, false, false, 0xC0, 0x80);                              // colours above 0x80
            }
            // Wrap modes past the borders, linear.
            const uint64_t regionClamp = 2 | 2 << 2 | 4ull << 4 | 28ull << 14 | 3ull << 24 | 20ull << 34;
            const uint64_t regionRepeat = 3 | 3 << 2 | 0x0Full << 4 | 0x10ull << 14 | 0x07ull << 24 | 0x08ull << 34;
            for (uint64_t clamp : {uint64_t{0}, uint64_t{5}, regionClamp, regionRepeat})
                if (sceneN == 1)
                    draw(100, 60, -0.5f, 1.5f, -0.4f, 1.3f, 0, 1, 0x60, clamp, false, false, 0x80, 0x80);
            // TFX x TCC, vertex alpha 0x40.
            for (uint32_t tfx = 0; tfx < 4 && sceneN == 1; ++tfx)
                for (uint32_t tcc = 0; tcc < 2; ++tcc)
                    draw(tw, th, 0, 1, 0, 1, tfx, tcc, 0x60, 5, false, false, 0x70, 0x40);
            // The rotated quad with subpixel vertices, and one with Q other than 1.
            if (sceneN == 1)
            {
                g.ad(kGsTex0, tex0Of(tbp, ts.tbw, ts.psm, ts.tw, ts.th, 1, 0, cbp, 1));
                g.ad(kGsTex1, 0x60);
                g.ad(kGsClamp, 5);
                const double cx = 560.3125, cy = 200.6875, r = 45;
                V q[4];
                for (int k = 0; k < 4; ++k)
                {
                    const double ang = 0.4 + k * M_PI / 2;
                    q[k].x = std::round((cx + r * std::cos(ang)) * 16) / 16;
                    q[k].y = std::round((cy + r * std::sin(ang)) * 16) / 16;
                    q[k].s = (k == 1 || k == 2) ? 1.0f : 0.0f;
                    q[k].t = k >= 2 ? 1.0f : 0.0f;
                    q[k].r = q[k].g = q[k].b = 0x80;
                    q[k].a = 0x80;
                }
                tri(g, 0x5B, q[0], q[1], q[3]);
                tri(g, 0x5B, q[1], q[2], q[3]);
                for (int k = 0; k < 4; ++k)
                {
                    q[k].x -= 100;
                    q[k].q = 1.0f + 0.37f * k;
                    q[k].s *= q[k].q;
                    q[k].t *= q[k].q;
                }
                tri(g, 0x1B, q[0], q[1], q[3]);
                tri(g, 0x1B, q[1], q[2], q[3]);
            }
            const Machines::Result r = m.run(g.bytes(), seed);
            uint64_t modelDiff = compare(r.emu, r.model).n;
            const std::string what = std::string(ts.name) + (sceneN ? " wraps, TFX, quads" : " scales");
            if (!g_quiet)
                std::printf("  T6 %-34s %2d rectangles; the model differs from the emulated GS on %" PRIu64 " values\n", what.c_str(), cell, modelDiff);
            t.notes.push_back(what + ": model differs on " + std::to_string(modelDiff));
            if (!threeEqual(t, r, what.c_str(), false, true) && t.failFast)
                return;
        }
    }

    // --- T8 order (18.3): the exact blend's groups and barriers.
    void testT8(Test &t, Machines &m)
    {
        const Gs2dVk::Options before = m.vk ? m.vk->options() : Gs2dVk::Options{};
        setMode(m.vk, true);
        auto run = [&](const std::string &what, Gif &g, const Planes &seed)
        {
            const Machines::Result r = m.run(g.bytes(), seed);
            return threeEqual(t, r, what.c_str()) || !t.failFast;
        };
        // A colour and alpha per rectangle, all different, so every order of the layers gives another picture.
        auto layer = [](uint32_t k, double x0, double y0, double x1, double y1, uint32_t z)
        {
            V a;
            a.x = x0;
            a.y = y0;
            a.z = z;
            a.r = static_cast<uint8_t>(17 + 53 * k);
            a.g = static_cast<uint8_t>(240 - 29 * k);
            a.b = static_cast<uint8_t>((91 * k + 5) & 0xFF);
            a.a = static_cast<uint8_t>(0x09 + (k * 0x2B) % 0x78);
            V b = a;
            b.x = x1;
            b.y = y1;
            return std::make_pair(a, b);
        };
        const Planes patterned = pattern(808, 0x40);
        // (i) 20 mutually overlapping rectangles of a reading state in one batch (each overlaps every other in the
        // middle), for each fractional form, with Z ALWAYS and with Z GREATER at rising Z (as K1).
        static const struct
        {
            uint64_t alpha;
            uint32_t fix;
            const char *what;
        } forms[] = {{0x01, 0, "0x01 lerp As"}, {0x09, 0, "0x09 Cd*As+Cs"}, {0x21, 0x20, "0x21 FIX 0x20"}, {0x29, 0x3F, "0x29 FIX 0x3f"},
                     {0x44, 0, "0x44 (Cs-Cd)*As+Cd"}, {0xA1, 0x55, "0xa1 FIX 0x55"}};
        for (const auto &fm : forms)
            for (int zmode = 0; zmode < 2; ++zmode)
            {
                Gif g;
                Env e;
                e.alpha = fm.alpha | static_cast<uint64_t>(fm.fix) << 32;
                e.zbuf = zmode ? kZbuf : (kZbuf | kZmsk);
                e.test = zmode ? (1u << 16 | 3u << 17) : kTestNone;
                env(g, e);
                for (uint32_t k = 0; k < 20; ++k)
                {
                    const auto [a, b] = layer(k, 10 + 9 * k, 5 + 5 * k, 260 + 9 * k, 150 + 5 * k, 0x100 + k);
                    rect(g, 0x43, a, b);
                }
                if (!run(std::string("T8 (i) ") + fm.what + (zmode ? " Z GREATER" : " Z ALWAYS"), g, patterned))
                    return;
            }
        // (ii) the same pixels by an alternation of fixed-function and reading states, in separate batches (one rectangle
        // each) and in shared ones (four overlapping rectangles a batch).
        static const struct
        {
            uint64_t alpha;
            uint32_t fix;
            uint32_t prim;
        } alt[] = {{0x29, 0x80, 0x43}, {0x01, 0, 0x43}, {0xA1, 0x80, 0x43}, {0x09, 0, 0x43}, {0, 0, 0x03}, {0x21, 0x20, 0x43}, {0x68, 0x33, 0x43}};
        for (int shared = 0; shared < 2; ++shared)
        {
            Gif g;
            Env e;
            env(g, e);
            uint32_t k = 0;
            for (int round = 0; round < 3; ++round)
                for (const auto &st : alt)
                {
                    g.ad(kGsAlpha, st.alpha | static_cast<uint64_t>(st.fix) << 32);
                    for (int n = 0; n < (shared ? 4 : 1); ++n, ++k)
                    {
                        const auto [a, b] = layer(k, 40 + 3 * (k % 11), 20 + 2 * (k % 13), 330 - 2 * (k % 7), 200 - (k % 5), 0);
                        rect(g, st.prim, a, b);
                    }
                }
            if (!run(std::string("T8 (ii) alternating states, ") + (shared ? "shared batches" : "separate batches"), g, patterned))
                return;
        }
        // (iii) one reading batch of 256 rectangles that do not overlap (one group) over a patterned destination.
        for (uint64_t alpha : {0x01ull, 0x09ull})
        {
            Gif g;
            Env e;
            e.alpha = alpha;
            env(g, e);
            for (uint32_t k = 0; k < 256; ++k)
            {
                const double x = (k % 32) * 20, y = (k / 32) * 32;
                const auto [a, b] = layer(k, x, y, x + 19, y + 31, 0);
                rect(g, 0x43, a, b);
            }
            if (!run(std::string("T8 (iii) 256 disjoint rectangles, ALPHA ") + (alpha == 1 ? "0x01" : "0x09"), g, patterned))
                return;
        }
        // (iv) both targets alternately: overlapping reading rectangles into FBP 0x0 and 0x50 in turn.
        {
            Gif g;
            Env e;
            e.alpha = 0x01;
            env(g, e);
            for (uint32_t k = 0; k < 24; ++k)
            {
                g.ad(kGsFrame, (k & 1u) ? kFrame1 : kFrame0);
                g.ad(kGsAlpha, (k % 3) == 2 ? (0x21ull | 0x40ull << 32) : 0x01ull);
                const auto [a, b] = layer(k, 20 + 7 * k, 10 + 3 * k, 300 + 7 * k, 180 + 2 * k, 0);
                rect(g, 0x43, a, b);
            }
            if (!run("T8 (iv) both targets alternately", g, patterned))
                return;
        }
        if (m.vk)
            m.vk->setOptions(before);
    }

    // ---------------------------------------------------------------- step 8 batch 2: K2's paths and the pixel rule
    // (RENDERER_3D_DESIGN.md 9.2 and 10.3)

    // A vertex sent as XYZ3 (ADC: queued without the drawing kick), as 21.3 M PATH1 vertices arrive.
    void vertexAdc(Gif &g, const V &v)
    {
        g.ad(kGsRgbaq, rgbaOf(v.r, v.g, v.b, v.a) | static_cast<uint64_t>(fbits(v.q)) << 32);
        g.ad(kGsSt, fbits(v.s) | static_cast<uint64_t>(fbits(v.t)) << 32);
        const uint64_t X = static_cast<uint64_t>(std::llround((v.x + kOfx) * 16.0)) & 0xFFFFu;
        const uint64_t Y = static_cast<uint64_t>(std::llround((v.y + kOfy) * 16.0)) & 0xFFFFu;
        g.ad(kGsXyz3, X | Y << 16 | static_cast<uint64_t>(v.z) << 32);
    }

    // The K2 texture: 256 x 128 CT32, random colour, alpha a ramp over x from 0 to 0x90 (crossing AREF 0x2f, 0x3f and
    // PABE's 0x80), at TBP 0x3000 (TBW 4).
    constexpr uint32_t kK2Tbp = 0x3000;
    void uploadK2Texture(Gif &g, uint32_t seed)
    {
        std::vector<uint8_t> d;
        uint32_t s = seed * 2246822519u + 7u;
        for (uint32_t y = 0; y < 128; ++y)
            for (uint32_t x = 0; x < 256; ++x)
            {
                s = s * 1664525u + 1013904223u;
                d.push_back(static_cast<uint8_t>(s >> 8));
                d.push_back(static_cast<uint8_t>(s >> 16));
                d.push_back(static_cast<uint8_t>((s >> 24) ^ y));
                d.push_back(static_cast<uint8_t>((x * 0x90u) / 255u));
            }
        g.image(kK2Tbp, 4, 0x00, 256, 128, d);
    }
    uint64_t k2Tex0() { return tex0Of(kK2Tbp, 4, 0x00, 8, 7, 1, 0); }

    // Z planes: a ramp over the picture (x and y), so per-vertex Z crosses the stored values.
    Planes zRamp(uint32_t seed, uint8_t alpha)
    {
        Planes p = pattern(seed, alpha);
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
                p.z[y * kW + x] = (x * 0x6A00u + y * 0x9D00u + (x * y) % 0x3FFu) & 0xFFFFFFu;
        return p;
    }

    // PATH1's draw states (RENDERER_3D_DESIGN.md 3.6; TEST and ALPHA as the dumps hold them).
    struct K2State
    {
        const char *what;
        uint32_t prim;
        uint64_t alpha, test;
        uint32_t pabe;
        bool zmsk;
        uint64_t tex1, clamp;
    };
    const K2State kK2States[] = {
        {"0x44 PABE, LEQUAL 0x2f, Z GREATER (the track)", 0x5B, 0x44, 0x702F7, 1, false, 0xF6000000160ull, 5},
        {"no blend, LEQUAL 0x2f, Z GREATER", 0x1B, 0, 0x702F7, 0, false, 0xF6000000160ull, 5},
        {"0x29 FIX 0x80, LEQUAL 0x2f, Z GEQUAL (env pass)", 0x5B, 0x8000000029ull, 0x502F7, 0, false, 0xF6000000160ull, 5},
        {"0x09, Z GREATER, ZMSK, REPEAT (water 1)", 0x5B, 0x09, 0x70000, 0, true, 0xF6000000160ull, 0},
        {"0x62 FIX 0x15, Z GEQUAL, ZMSK, REPEAT (water 2)", 0x5B, 0x1500000062ull, 0x50000, 0, true, 0xF6000000160ull, 0},
        {"0x68 FIX 0x15, Z GEQUAL, ZMSK, REPEAT (water 3)", 0x5B, 0x1500000068ull, 0x50000, 0, true, 0xF6000000160ull, 0},
        {"0x29 FIX 0x80, LEQUAL 0x3f, ZMSK, TEX1 0x60", 0x5B, 0x8000000029ull, 0x703F7, 0, true, 0x60, 5},
        {"0x08, LEQUAL 0x2f, Z GREATER", 0x5B, 0x08, 0x702F7, 0, false, 0xF6000000160ull, 5},
        {"0x68 FIX 0x80, Z GREATER, ZMSK", 0x5B, 0x8000000068ull, 0x70000, 0, true, 0xF6000000160ull, 5},
        {"0xa1 FIX 0x80, LEQUAL 0x3f, ZMSK, TEX1 0x60", 0x5B, 0x80000000A1ull, 0x703F7, 0, true, 0x60, 5},
        {"0x01, Z GREATER, ZMSK", 0x5B, 0x01, 0x70000, 0, true, 0xF6000000160ull, 5},
        {"0x29 FIX 0x7f, LEQUAL 0x2f, ZMSK", 0x5B, 0x7F00000029ull, 0x702F7, 0, true, 0xF6000000160ull, 5},
        {"0x01, LEQUAL 0x3f RGB_ONLY, ZMSK, TEX1 0x60", 0x5B, 0x8000000001ull, 0x733F7, 0, true, 0x60, 5},
        {"0x44 PABE, MXL 3 (mipmapped, level 0)", 0x5B, 0x44, 0x702F7, 1, false, 0xF600000016Cull, 5},
        {"0x44 PABE, MXL 4 (mipmapped, level 0)", 0x5B, 0x44, 0x702F7, 1, false, 0xF6000000170ull, 5},
        {"0x21 FIX 0x80 PABE, LEQUAL 0x3f", 0x5B, 0x8000000021ull, 0x703F7, 1, false, 0xF6000000160ull, 5},
        {"0x09, LEQUAL 0x3f, Z GREATER, Z write", 0x5B, 0x09, 0x703F7, 0, false, 0xF6000000160ull, 5},
        {"no blend, TEX1 0x60, ZMSK", 0x1B, 0, 0x70000, 0, true, 0x60, 5},
    };
    void k2StateWrites(Gif &g, const K2State &st)
    {
        g.ad(kGsAlpha, st.alpha);
        g.ad(kGsTest, st.test);
        g.ad(kGsPabe, st.pabe);
        g.ad(kGsZbuf, kZbuf | (st.zmsk ? kZmsk : 0));
        g.ad(kGsTex0, k2Tex0());
        g.ad(kGsTex1, st.tex1);
        g.ad(kGsClamp, st.clamp);
        // The mip levels' addresses, as PATH1 writes them (the reference never reads them: level 0 is drawn).
        g.ad(kGsMiptbp1, 0x3400ull | 2ull << 14 | 0x3480ull << 20 | 1ull << 34 | 0x34A0ull << 40 | 1ull << 54);
        g.ad(kGsMiptbp2, 0x34B0ull | 1ull << 14 | 0x34B8ull << 20 | 1ull << 34 | 0x34BCull << 40 | 1ull << 54);
    }
    // A perspective quad (two triangles) over x0..x1 x y0..y1: Q per vertex, S and T over s0..s1 / t0..t1 (times Q),
    // Gouraud colour, an alpha ramp over the vertices, Z per vertex.
    void k2Quad(Gif &g, uint32_t prim, double x0, double y0, double x1, double y1, float s0, float s1, float t0, float t1, const uint32_t z[4],
                const uint8_t a[4], uint32_t seed, bool adc = false)
    {
        const float qs[4] = {0.6f, 1.0f, 1.7f, 2.3f};
        V v[4];
        for (int k = 0; k < 4; ++k)
        {
            v[k].x = (k & 1) ? x1 : x0;
            v[k].y = (k & 2) ? y1 : y0;
            v[k].q = qs[(k + seed) & 3];
            v[k].s = ((k & 1) ? s1 : s0) * v[k].q;
            v[k].t = ((k & 2) ? t1 : t0) * v[k].q;
            v[k].z = z[k];
            v[k].r = static_cast<uint8_t>(0x40 + 37 * k + 11 * seed);
            v[k].g = static_cast<uint8_t>(0x90 - 23 * k + 5 * seed);
            v[k].b = static_cast<uint8_t>(0x60 + 17 * k);
            v[k].a = a[k];
        }
        g.ad(kGsPrim, prim);
        if (adc)
            vertexAdc(g, v[0]);
        else
            vertex(g, v[0]);
        vertex(g, v[1]);
        vertex(g, v[2]);
        g.ad(kGsPrim, prim);
        if (adc)
            vertexAdc(g, v[1]);
        else
            vertex(g, v[1]);
        vertex(g, v[3]);
        vertex(g, v[2]);
    }

    // --- T9: what K2 uses that K1 did not (9.2).
    void testT9(Test &t, Machines &m)
    {
        auto run = [&](const std::string &what, Gif &g, const Planes &seed, bool model = true)
        {
            const Machines::Result r = m.run(g.bytes(), seed);
            const uint64_t changed = compare(r.emu, seed).n;
            if (!g_quiet)
            {
                const Diff d = compare(r.emu, r.model);
                std::printf("  T9 %-56s %6" PRIu64 " values drawn; model %s\n", what.c_str(), changed, d.n ? diffText(d, "emu", "model").c_str() : "equal");
            }
            ++t.checks;
            if (r.dropped || !changed)
                return t.fail(what + ": " + std::to_string(r.dropped) + " primitives dropped by the reader, " + std::to_string(changed) +
                              " values drawn by the emulated GS (the scene is not what it should test)");
            return threeEqual(t, r, what.c_str(), model, true) || !t.failFast;
        };
        // (i) Per-vertex Z under GREATER and GEQUAL, with and without the Z write, against a stored ramp; every triangle
        // drawn twice (the second with other colours: under GEQUAL it lands, under GREATER it does not).
        for (uint32_t ztst : {3u, 2u})
            for (int zmsk = 0; zmsk < 2; ++zmsk)
            {
                Gif g;
                Env e;
                e.zbuf = kZbuf | (zmsk ? kZmsk : 0);
                e.test = 1u << 16 | ztst << 17;
                env(g, e);
                for (int pass = 0; pass < 2; ++pass)
                {
                    uint32_t s = 99;
                    for (int k = 0; k < 14; ++k)
                    {
                        V v[3];
                        for (int i = 0; i < 3; ++i)
                        {
                            s = s * 1664525u + 1013904223u;
                            v[i].x = std::round((static_cast<double>(s >> 16 & 0x3FF) * 0.75 - 60.0) * 16.0) / 16.0 + (i == 2 ? 0.3125 : 0.0);
                            s = s * 1664525u + 1013904223u;
                            v[i].y = std::round((static_cast<double>(s >> 16 & 0x1FF) * 0.6 - 20.0) * 16.0) / 16.0;
                            s = s * 1664525u + 1013904223u;
                            v[i].z = (s >> 8) & 0xFFFFFFu;
                            if (k % 5 == 0)
                                v[i].z = 0xFFFFFF - (i * 3u); // near the top of Z24
                            v[i].r = static_cast<uint8_t>(pass ? 0x20 + 9 * k : 0xF0 - 7 * k);
                            v[i].g = static_cast<uint8_t>(0x30 * i + pass * 0x40);
                            v[i].b = static_cast<uint8_t>(k * 13);
                            v[i].a = 0x80;
                        }
                        tri(g, 0x0B, v[0], v[1], v[2]);
                    }
                }
                char what[96];
                std::snprintf(what, sizeof(what), "(i) per-vertex Z, %s, %s", ztst == 3 ? "GREATER" : "GEQUAL", zmsk ? "ZMSK" : "Z write");
                if (!run(what, g, zRamp(31 + ztst + zmsk, 0x30)))
                    return;
            }
        // (ii) every PATH1 state of 3.6 on a 256 x 128 CT32 texture with an alpha ramp (LEQUAL 0x2f and 0x3f, PABE's 0x80
        // all crossed), perspective Q, Gouraud colour and an alpha ramp over the vertices, Z per vertex over a stored
        // ramp; two overlapping quads a state (so the reading states read what the first drew).
        const size_t nStates = sizeof(kK2States) / sizeof(kK2States[0]);
        for (size_t first = 0; first < nStates; first += 6)
        {
            Gif g;
            uploadK2Texture(g, 5);
            Env e;
            e.zbuf = kZbuf;
            env(g, e);
            int cell = 0;
            for (size_t i = first; i < std::min(nStates, first + 6); ++i, ++cell)
            {
                const K2State &st = kK2States[i];
                k2StateWrites(g, st);
                const double x0 = (cell % 3) * 213.0 + 3.0625, y0 = (cell / 3) * 128.0 + 2.5;
                const uint32_t z1[4] = {0x200000, 0x900000, 0x500000, 0xF00000};
                const uint32_t z2[4] = {0xE00000, 0x100000, 0x800000, 0x480000};
                const uint8_t a1[4] = {0x80, 0x60, 0x80, 0x18};
                const uint8_t a2[4] = {0x80, 0x80, 0x30, 0x80};
                k2Quad(g, st.prim, x0, y0, x0 + 150.0, y0 + 90.0, 0.02f, 0.97f, 0.03f, 0.99f, z1, a1, static_cast<uint32_t>(i));
                k2Quad(g, st.prim, x0 + 40.0625, y0 + 25.75, x0 + 205.0, y0 + 122.0, -0.3f, 1.4f, 0.1f, 0.8f, z2, a2, static_cast<uint32_t>(i) + 1);
            }
            if (!run("(ii) PATH1 states " + std::to_string(first) + "-" + std::to_string(std::min(nStates, first + 6) - 1), g, zRamp(7 + first, 0x50)))
                return;
        }
        // (iii) REPEAT to +-1,000 texels (S and T far outside 0..1, perspective), linear and nearest, both water states.
        for (uint64_t tex1 : {0xF6000000160ull, 0ull})
        {
            Gif g;
            uploadK2Texture(g, 9);
            Env e;
            env(g, e);
            int cell = 0;
            for (const float span : {3.9f, -3.9f, 7.8f})
                for (const K2State *st : {&kK2States[3], &kK2States[8]})
                {
                    K2State s = *st;
                    s.tex1 = tex1;
                    s.clamp = 0;
                    k2StateWrites(g, s);
                    const double x0 = (cell % 3) * 213.0, y0 = (cell / 3) * 128.0;
                    const uint32_t z[4] = {0xFFFE00, 0xFFFE40, 0xFFFE80, 0xFFFEC0}; // above the stored ramp: the Z test passes
                    const uint8_t a[4] = {0x80, 0x70, 0x80, 0x50};
                    k2Quad(g, s.prim, x0, y0, x0 + 200.0, y0 + 120.0, -span, span, span * 0.5f, -span * 2.0f, z, a, static_cast<uint32_t>(cell));
                    ++cell;
                }
            if (!run(std::string("(iii) REPEAT to +-1,000 texels, ") + (tex1 ? "bilinear" : "nearest"), g, zRamp(77, 0x40)))
                return;
        }
        // (iv) big triangles: vertices to 2,047 pixels from the window origin (the viewport's range), perspective,
        // REPEAT, per-vertex Z.
        {
            Gif g;
            uploadK2Texture(g, 13);
            Env e;
            e.zbuf = kZbuf;
            e.test = 1u << 16 | 3u << 17;
            env(g, e);
            K2State s = kK2States[1];
            s.clamp = 0;
            k2StateWrites(g, s);
            const double far[6][2] = {{-1727.5, -1927.0}, {2047.0, -200.0}, {300.0, 2047.0}, {-1700.0, 2040.0}, {2046.9375, 2046.0}, {-1000.0, -1900.0}};
            for (int k = 0; k < 6; ++k)
            {
                V v[3];
                v[0].x = far[k][0];
                v[0].y = far[k][1];
                v[1].x = 320.5 + 40 * k;
                v[1].y = 30.25 + 20 * k;
                v[2].x = 100.0 + 70 * k;
                v[2].y = 230.0 - 15 * k;
                for (int i = 0; i < 3; ++i)
                {
                    v[i].q = 0.4f + 0.9f * static_cast<float>((i + k) % 3);
                    v[i].s = static_cast<float>(v[i].x / 64.0) * v[i].q;
                    v[i].t = static_cast<float>(v[i].y / 64.0) * v[i].q;
                    v[i].z = 0x10000u * static_cast<uint32_t>(20 + 30 * i + 7 * k);
                    v[i].r = static_cast<uint8_t>(0x50 + 30 * i);
                    v[i].g = static_cast<uint8_t>(0x80 - 10 * k);
                    v[i].b = 0x70;
                    v[i].a = 0x80;
                }
                tri(g, s.prim, v[0], v[1], v[2]);
            }
            if (!run("(iv) big triangles, vertices to 2,047 pixels out", g, zRamp(91, 0x20)))
                return;
        }
        // (v) ADC vertices (XYZ3: queued, no kick) as PATH1 sends them, and one extra XYZ3 vertex shifting the queue.
        {
            Gif g;
            uploadK2Texture(g, 17);
            Env e;
            e.zbuf = kZbuf;
            env(g, e);
            k2StateWrites(g, kK2States[0]);
            const uint32_t z[4] = {0x300000, 0x310000, 0x320000, 0x330000};
            const uint8_t a[4] = {0x10, 0x20, 0x28, 0x2F};
            for (int k = 0; k < 6; ++k)
                k2Quad(g, 0x5B, 20.0 + 100 * k, 10.0, 110.0 + 100 * k, 100.0, 0.0f, 1.0f, 0.0f, 1.0f, z, a, static_cast<uint32_t>(k), true);
            // An extra XYZ3 vertex first: the queue then draws (extra, a, b), and (c, d, e) after it.
            V w[6];
            for (int k = 0; k < 6; ++k)
            {
                w[k].x = 300.0 + 40.0 * k + (k & 1) * 13.0;
                w[k].y = (k & 1) ? 250.0 : 120.0 + 7 * k;
                w[k].z = 0x380000 + 0x1000u * static_cast<uint32_t>(k);
                w[k].q = 0.8f + 0.3f * k;
                w[k].s = 0.15f * k * w[k].q;
                w[k].t = ((k & 1) ? 0.9f : 0.1f) * w[k].q;
                w[k].a = static_cast<uint8_t>(0x08 + 6 * k);
                w[k].r = static_cast<uint8_t>(0x70 + 20 * k);
            }
            g.ad(kGsPrim, 0x5B);
            vertexAdc(g, w[0]);
            for (int k = 1; k < 6; ++k)
                vertex(g, w[k]);
            if (!run("(v) ADC (XYZ3) vertices", g, zRamp(5, 0x40)))
                return;
        }
        // (vi) a vertex with S, T and Q all 0, as PATH1's clipping sends them (the reference reads Q 0 as 1.0).
        {
            Gif g;
            uploadK2Texture(g, 19);
            Env e;
            env(g, e);
            k2StateWrites(g, kK2States[1]);
            for (int k = 0; k < 8; ++k)
            {
                V v[3];
                v[0].x = 40.0 + 70 * k;
                v[0].y = 20.0;
                v[0].q = 0.001f + 0.0002f * k;
                v[0].s = 1.6f * v[0].q;
                v[0].t = 0.0f;
                v[1].x = 80.0 + 70 * k;
                v[1].y = 128.0;
                v[1].q = 0.0f;
                v[1].s = v[1].t = 0.0f;
                v[2].x = 20.0 + 70 * k;
                v[2].y = 230.0;
                v[2].q = 0.0008f;
                v[2].s = 1.6f * v[2].q;
                v[2].t = v[2].q;
                for (V &w : v)
                {
                    w.z = 0x800000;
                    w.r = 0x70;
                    w.g = 0x60;
                    w.b = 0x50;
                    w.a = 0x80;
                }
                tri(g, 0x1B, v[0], v[1], v[2]);
            }
            if (!run("(vi) a vertex with Q 0 (read as 1.0)", g, zRamp(6, 0x40)))
                return;
        }
    }

    // --- T10: the barrier rule by covered pixels (10.3).

    // The rule computed independently (dense pixel sets from gsTriCovers per pixel, no spans): the barriers a draw list
    // of one target needs. Reading: a fractional blend (planState: blend on, not exactBlend).
    uint64_t predictBarriers(const DrawList &list)
    {
        std::vector<uint8_t> dirty(kPix, 0);
        bool drawn = false, readSeen = false;
        uint64_t barriers = 0;
        GsState st{};
        GsPlan plan{};
        bool have = false;
        int target = -1;
        for (const DrawCmd &c : list.cmds)
        {
            if (c.op == kOpState)
            {
                st = list.state(c);
                plan = planState(st);
                have = !(plan.unsupported || (c.a & 1u));
                continue;
            }
            if (c.op != kOpScreenPrims || !have || c.a != kKindTriangles)
                continue;
            if (static_cast<int>(plan.target) != target)
            {
                // Another target: another pass, which orders everything before it.
                target = static_cast<int>(plan.target);
                drawn = readSeen = false;
                std::fill(dirty.begin(), dirty.end(), 0);
            }
            const GsVertex *v = list.vertices(c);
            const uint32_t ofx = st.xyoffset & 0xFFFFu, ofy = (st.xyoffset >> 32) & 0xFFFFu;
            const int sx0 = static_cast<int>(st.scissor & 0x7FFu), sx1 = std::min(static_cast<int>((st.scissor >> 16) & 0x7FFu), static_cast<int>(kW) - 1);
            const int sy0 = static_cast<int>((st.scissor >> 32) & 0x7FFu), sy1 = std::min(static_cast<int>((st.scissor >> 48) & 0x7FFu), static_cast<int>(kH) - 1);
            const bool reads = plan.blend != kBlendOff && !plan.exactBlend;
            for (uint32_t p = 0; p < c.b; ++p)
            {
                GsTri t;
                if (!gsTriSetup(v + p * 3u, ofx, ofy, t))
                    continue;
                std::vector<uint32_t> px;
                for (int y = std::max(t.minY, sy0); y <= std::min(t.maxY, sy1); ++y)
                    for (int x = std::max(t.minX, sx0); x <= std::min(t.maxX, sx1); ++x)
                    {
                        const int64_t e[3] = {gsTriEdge(t, 0, x, y), gsTriEdge(t, 1, x, y), gsTriEdge(t, 2, x, y)};
                        if (gsTriCovers(t, e))
                            px.push_back(static_cast<uint32_t>(y) * kW + static_cast<uint32_t>(x));
                    }
                if (px.empty())
                    continue;
                if (reads)
                {
                    bool need = false;
                    if (!readSeen)
                    {
                        readSeen = true;
                        need = drawn;
                    }
                    else
                        for (uint32_t i : px)
                            need |= dirty[i] != 0;
                    if (need)
                    {
                        ++barriers;
                        std::fill(dirty.begin(), dirty.end(), 0);
                    }
                }
                if (readSeen)
                    for (uint32_t i : px)
                        dirty[i] = 1;
                drawn = true;
            }
        }
        return barriers;
    }

    void testT10(Test &t, Machines &m)
    {
        // (o) GsCoverMap's spans against the reference's per-pixel coverage: random triangles (subpixel, slivers, large,
        // partly off the target), each span set equal to gsTriCovers over the box.
        {
            ++t.checks;
            uint32_t s = 4242;
            auto rnd = [&]() { return s = s * 1664525u + 1013904223u, s >> 8; };
            uint64_t tris = 0, pixels = 0, bad = 0;
            std::vector<GsSpan> sp;
            for (int k = 0; k < 20000; ++k)
            {
                GsVertex v[3]{};
                const int scale = (k % 4 == 0) ? 4000 : (k % 4 == 1) ? 60 : (k % 4 == 2) ? 700 : 3;
                const int cx = static_cast<int>(rnd() % 800) - 80, cy = static_cast<int>(rnd() % 320) - 30;
                for (int i = 0; i < 3; ++i)
                {
                    const int dx = static_cast<int>(rnd() % (2 * scale * 16 + 1)) - scale * 16, dy = static_cast<int>(rnd() % (2 * scale * 16 + 1)) - scale * 16;
                    v[i].x = static_cast<uint16_t>(std::clamp((cx + kOfx) * 16 + dx, 0, 65535));
                    v[i].y = static_cast<uint16_t>(std::clamp((cy + kOfy) * 16 + dy, 0, 65535));
                }
                if (k % 7 == 0)
                    v[2].y = v[1].y; // a flat edge
                GsTri tr;
                if (!gsTriSetup(v, kOfx * 16, kOfy * 16, tr))
                    continue;
                const int x0 = std::max(tr.minX, 0), x1 = std::min(tr.maxX, static_cast<int>(kW) - 1);
                const int y0 = std::max(tr.minY, 0), y1 = std::min(tr.maxY, static_cast<int>(kH) - 1);
                if (x1 < x0 || y1 < y0)
                    continue;
                ++tris;
                sp.clear();
                GsCoverMap::triangle(tr, x0, y0, x1, y1, sp);
                std::vector<uint8_t> byspan(static_cast<size_t>(x1 - x0 + 1) * (y1 - y0 + 1), 0);
                for (const GsSpan &q : sp)
                    for (int x = q.x0; x <= q.x1; ++x)
                        byspan[static_cast<size_t>(q.y - y0) * (x1 - x0 + 1) + (x - x0)] = 1;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                    {
                        const int64_t e[3] = {gsTriEdge(tr, 0, x, y), gsTriEdge(tr, 1, x, y), gsTriEdge(tr, 2, x, y)};
                        const bool c = gsTriCovers(tr, e);
                        pixels += c;
                        bad += c != (byspan[static_cast<size_t>(y - y0) * (x1 - x0 + 1) + (x - x0)] != 0);
                    }
            }
            if (!g_quiet)
                std::printf("  T10 (o) spans against gsTriCovers: %" PRIu64 " triangles, %" PRIu64 " covered pixels, %" PRIu64 " differ\n", tris, pixels, bad);
            if (bad)
                t.fail("(o) GsCoverMap::triangle differs from gsTriCovers on " + std::to_string(bad) + " pixels");
        }
        const Gs2dVk::Options before = m.vk ? m.vk->options() : Gs2dVk::Options{};
        setMode(m.vk, true);
        auto run = [&](const std::string &what, Gif &g, const Planes &seed, int64_t expect)
        {
            const Machines::Result r = m.run(g.bytes(), seed);
            const uint64_t predicted = predictBarriers(r.list);
            const uint64_t issued = m.vk ? m.vk->lastList().barriers : 0;
            const bool boxes = m.vk && m.vk->options().grouping == Gs2dVk::kGroupBoxes;
            if (!g_quiet)
                std::printf("  T10 %-62s barriers %" PRIu64 " (predicted %" PRIu64 "%s), draws %" PRIu64 "\n", what.c_str(), issued, predicted,
                            expect >= 0 ? (", the design's " + std::to_string(expect)).c_str() : "", m.vk ? m.vk->lastList().draws : 0);
            bool ok = threeEqual(t, r, what.c_str());
            // The fault runs judge the pictures only; the box rule (kGroupBoxes) must pass with at least as many barriers.
            if (m.vk && m.vk->options().faults == 0 && r.vkOk)
            {
                ++t.checks;
                if (boxes ? issued < predicted : (issued != predicted || (expect >= 0 && static_cast<int64_t>(issued) != expect)))
                    ok = t.fail(what + ": " + std::to_string(issued) + " barriers, predicted " + std::to_string(predicted) +
                                (expect >= 0 ? ", the design's " + std::to_string(expect) : std::string()) + (boxes ? " (box rule: at least)" : "")) &&
                         ok;
            }
            return ok || !t.failFast;
        };
        // The water mesh: a fan of 32 triangles around a centre and a strip-like mesh of 32 (a grid two cells high),
        // sharing edges; `moved`: every fourth triangle shifted to overlap its neighbour.
        auto mesh = [](bool moved)
        {
            std::vector<std::array<V, 3>> tris;
            const double cx = 160.4375, cy = 128.5625, r = 110.0;
            for (int k = 0; k < 32; ++k)
            {
                std::array<V, 3> tr;
                tr[0].x = cx;
                tr[0].y = cy;
                for (int i = 0; i < 2; ++i)
                {
                    const double a = (k + i) * 2.0 * M_PI / 32.0;
                    tr[1 + i].x = std::round((cx + r * std::cos(a)) * 16.0) / 16.0;
                    tr[1 + i].y = std::round((cy + r * std::sin(a)) * 16.0) / 16.0;
                }
                tris.push_back(tr);
            }
            for (int k = 0; k < 32; ++k)
            {
                const int col = k / 2, row = 0;
                const double x0 = 330.0 + col * 18.6875, y0 = 30.25 + row * 90.0, w = 18.6875, h = 95.5;
                std::array<V, 3> tr;
                if (k % 2 == 0)
                {
                    tr[0].x = x0, tr[0].y = y0, tr[1].x = x0 + w, tr[1].y = y0, tr[2].x = x0, tr[2].y = y0 + h;
                }
                else
                {
                    tr[0].x = x0 + w, tr[0].y = y0, tr[1].x = x0 + w, tr[1].y = y0 + h, tr[2].x = x0, tr[2].y = y0 + h;
                }
                tris.push_back(tr);
            }
            if (moved)
                for (size_t k = 3; k < tris.size(); k += 4)
                    for (V &v : tris[k])
                    {
                        v.x += 6.5;
                        v.y += 3.25;
                    }
            for (size_t k = 0; k < tris.size(); ++k)
                for (int i = 0; i < 3; ++i)
                {
                    V &v = tris[k][i];
                    v.q = 0.7f + 0.5f * static_cast<float>((k + i) % 3);
                    v.s = static_cast<float>(v.x / 37.0) * v.q;
                    v.t = static_cast<float>(v.y / 23.0) * v.q;
                    v.z = 0x400000 + static_cast<uint32_t>(k) * 0x100 + static_cast<uint32_t>(i);
                    v.r = static_cast<uint8_t>(0x30 + 5 * k);
                    v.g = static_cast<uint8_t>(0x80 - 2 * k + 30 * i);
                    v.b = 0x60;
                    v.a = static_cast<uint8_t>(0x30 + (k * 7 + i * 20) % 0x50);
                }
            return tris;
        };
        // An opaque background over the mesh (drawn in the pass before the first reading primitive).
        auto background = [](Gif &g)
        {
            g.ad(kGsAlpha, 0);
            g.ad(kGsTest, 1u << 16 | 1u << 17);
            g.ad(kGsZbuf, kZbuf | kZmsk);
            V a;
            a.x = 0;
            a.y = 0;
            a.r = 0x22;
            a.g = 0x44;
            a.b = 0x66;
            a.a = 0x80;
            V b = a;
            b.x = 640;
            b.y = 256;
            rect(g, 0x03, a, b);
        };
        // Water's three passes per triangle (0x09 reading; 0x62 FIX 0x15 and 0x68 FIX 0x15 exact, GEQUAL), ZMSK, REPEAT.
        auto water = [&](Gif &g, const std::vector<std::array<V, 3>> &tris)
        {
            for (const auto &tr : tris)
                for (int pass = 3; pass <= 5; ++pass)
                {
                    k2StateWrites(g, kK2States[pass]);
                    tri(g, 0x5B, tr[0], tr[1], tr[2]);
                }
        };
        const Planes patterned = zRamp(4040, 0x40);
        for (int moved = 0; moved < 2; ++moved)
        {
            Gif g;
            uploadK2Texture(g, 21);
            Env e;
            env(g, e);
            background(g);
            water(g, mesh(moved != 0));
            if (!run(moved ? "(ii) the mesh with every fourth triangle moved" : "(i) water's three passes over a fan and a strip, one barrier", g, patterned,
                     moved ? -1 : 1))
                return;
        }
        // (iii) reading triangles interleaved with exact additive ones on the same pixels (0x01 lerp, then 0x68 FIX 0x80
        // over a neighbour, then 0x01 again over both).
        {
            Gif g;
            uploadK2Texture(g, 23);
            Env e;
            env(g, e);
            const auto tris = mesh(false);
            for (size_t k = 0; k + 1 < tris.size(); k += 2)
            {
                k2StateWrites(g, kK2States[10]); // 0x01 reading
                tri(g, 0x5B, tris[k][0], tris[k][1], tris[k][2]);
                k2StateWrites(g, kK2States[8]); // 0x68 FIX 0x80 exact
                tri(g, 0x5B, tris[k + 1][0], tris[k + 1][1], tris[k + 1][2]);
                k2StateWrites(g, kK2States[10]);
                tri(g, 0x5B, tris[k + 1][0], tris[k + 1][1], tris[k + 1][2]);
                tri(g, 0x5B, tris[k][0], tris[k][1], tris[k][2]);
            }
            if (!run("(iii) reading and exact additive triangles on the same pixels", g, patterned, -1))
                return;
        }
        // (iv) a pass per target, alternating: in each, a large textured opaque triangle pair, then reading triangles
        // over it (the barrier of each pass's first reading primitive).
        {
            Gif g;
            uploadK2Texture(g, 29);
            Env e;
            env(g, e);
            const auto tris = mesh(false);
            for (int k = 0; k < 24; ++k)
            {
                g.ad(kGsFrame, (k & 1) ? kFrame1 : kFrame0);
                k2StateWrites(g, kK2States[17]); // no blend: the mesh, last triangle first
                for (size_t i = tris.size(); i-- > 0;)
                {
                    std::array<V, 3> tr = tris[i];
                    for (V &v : tr)
                    {
                        v.r = static_cast<uint8_t>(v.r + 31 * k);
                        v.g = static_cast<uint8_t>(v.g ^ (k * 8));
                    }
                    tri(g, 0x1B, tr[0], tr[1], tr[2]);
                }
                k2StateWrites(g, kK2States[10]); // 0x01 reading, over the mesh's triangles in order (the first over the last drawn)
                for (const auto &tr : tris)
                    tri(g, 0x5B, tr[0], tr[1], tr[2]);
            }
            if (!run("(iv) a pass per target: an opaque mesh, then reading it", g, patterned, 24))
                return;
        }
        if (m.vk)
            m.vk->setOptions(before);
    }

    // --- T7 device arithmetic.
    void testT7(Test &t, Machines &m)
    {
        if (!m.vk)
            return;
        struct Tri
        {
            V v[3];
            uint32_t prim;
        };
        auto mk = [](double x, double y, float s, float tt, float q)
        {
            V v;
            v.x = x;
            v.y = y;
            v.s = s * q;
            v.t = tt * q;
            v.q = q;
            v.r = 0x80;
            v.g = 0x40;
            v.b = 0x20;
            v.a = 0x80;
            return v;
        };
        const Tri tris[6] = {
            {{mk(-20, -10, 0, 0, 1), mk(660, 5, 1, 0, 1), mk(30, 270, 0, 1, 1)}, 0x13},
            {{mk(639.5, 255.25, 1, 1, 1), mk(0.0625, 250, 0, 1, 1), mk(600, 0.4375, 1, 0, 1)}, 0x13},
            {{mk(10.3125, 3.75, 0.1f, 0.2f, 1), mk(630.5, 128.125, 0.9f, 0.5f, 1), mk(17, 251.9375, 0.3f, 0.95f, 1)}, 0x1B},
            {{mk(-300, 128, 0, 0, 1.0f), mk(900, 120, 1, 0.1f, 2.5f), mk(320, 400, 0.5f, 1, 0.7f)}, 0x13},
            {{mk(0, 0, 0.0f, 0.0f, 3), mk(640, 0, 1, 0, 0.25f), mk(0, 256, 0, 1, 1.75f)}, 0x13},
            {{mk(320.5, -50, 0.5f, -0.2f, 1), mk(700, 300, 1.3f, 1.1f, 1), mk(-60, 230.0625, -0.25f, 0.9f, 1)}, 0x13},
        };
        std::vector<uint8_t> tex(64 * 64 * 4, 0x80);
        uint64_t pixels = 0, bad[8] = {}, outside = 0;
        static const char *const names[8] = {"w0", "w1", "w2", "invQ", "uf", "vf", "fu", "fv"};
        for (const Tri &tr : tris)
        {
            Gif g;
            g.image(0x3000, 1, 0x00, 64, 64, tex);
            Env e;
            e.tex0 = tex0Of(0x3000, 1, 0x00, 6, 6, 1, 0);
            e.tex1 = 0x60;
            env(g, e);
            tri(g, tr.prim, tr.v[0], tr.v[1], tr.v[2]);
            const std::vector<uint8_t> bytes = g.bytes();
            Gif2dReader rd(m.reader);
            rd.startBlank();
            rd.cycleBegin(0);
            rd.feedPath3(bytes.data(), static_cast<uint32_t>(bytes.size()));
            rd.cycleEnd(rc::rec::CycleInfo{}, nullptr);
            const DrawList &list = rd.list();
            std::vector<uint32_t> p0, p1;
            if (!m.vk->probe(list, false, p0) || !m.vk->probe(list, true, p1))
            {
                t.fail("probe failed");
                return;
            }
            // CPU: the reference's expressions (gs_model).
            GsState s{};
            const GsVertex *v = nullptr;
            for (const DrawCmd &c : list.cmds)
            {
                if (c.op == kOpState)
                    s = list.state(c);
                if (c.op == kOpScreenPrims)
                    v = list.vertices(c);
            }
            GsTri gt;
            if (!v || !gsTriSetup(v, s.xyoffset & 0xFFFF, (s.xyoffset >> 32) & 0xFFFF, gt))
            {
                t.fail("no triangle");
                return;
            }
            std::vector<uint8_t> cpuCovered(kPix, 0);
            for (int y = std::max(gt.minY, 0); y <= std::min(gt.maxY, 255); ++y)
                for (int x = std::max(gt.minX, 0); x <= std::min(gt.maxX, 639); ++x)
                {
                    const int64_t ev[3] = {gsTriEdge(gt, 0, x, y), gsTriEdge(gt, 1, x, y), gsTriEdge(gt, 2, x, y)};
                    if (!gsTriCovers(gt, ev))
                        continue;
                    GsTriPixel q;
                    gsTriPixel(gt, v, s, x, y, q);
                    const size_t i = static_cast<size_t>(y) * kW + x;
                    cpuCovered[i] = 1;
                    ++pixels;
                    const float cpu[8] = {q.w[0], q.w[1], q.w[2], q.invQ, q.uf, q.vf, q.fu, q.fv};
                    const uint32_t gpu[8] = {p0[i * 4], p0[i * 4 + 1], p0[i * 4 + 2], p0[i * 4 + 3], p1[i * 4], p1[i * 4 + 1], p1[i * 4 + 2], p1[i * 4 + 3]};
                    for (int k = 0; k < 8; ++k)
                        if (fbits(cpu[k]) != gpu[k] && bad[k]++ == 0 && !g_quiet)
                        {
                            float gf;
                            std::memcpy(&gf, &gpu[k], 4);
                            std::printf("  T7 first %s difference at %d,%d: CPU %.9g (%08x), GPU %.9g (%08x)\n", names[k], x, y, cpu[k], fbits(cpu[k]), gf, gpu[k]);
                        }
                }
            for (uint32_t i = 0; i < kPix; ++i)
                if (!cpuCovered[i] && (p0[i * 4] | p0[i * 4 + 1] | p0[i * 4 + 2] | p0[i * 4 + 3]))
                    ++outside;
        }
        ++t.checks;
        std::string summary = std::to_string(pixels) + " pixels;";
        for (int k = 0; k < 8; ++k)
            summary += std::string(" ") + names[k] + " " + std::to_string(bad[k]);
        summary += "; GPU pixels outside the CPU's coverage " + std::to_string(outside);
        if (!g_quiet)
            std::printf("  T7: %s\n", summary.c_str());
        t.notes.push_back(summary);
        for (int k = 0; k < 8; ++k)
            if (bad[k])
                t.fail(std::string(names[k]) + " differs on " + std::to_string(bad[k]) + " pixels");
        if (outside)
            t.fail("coverage differs");
    }
}

int main(int argc, char **argv)
{
    std::set<std::string> only;
    bool faults = true, validation = false;
    std::string blendArg = "both";
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--only" && i + 1 < argc)
        {
            std::string s = argv[++i];
            size_t p = 0;
            while (p <= s.size())
            {
                const size_t q = std::min(s.find(',', p), s.size());
                only.insert(s.substr(p, q - p));
                p = q + 1;
            }
        }
        else if (a == "--no-faults")
            faults = false;
        else if (a == "--validation")
            validation = true;
        else if (a == "--quiet")
            g_quiet = true;
        else if (a == "--blend" && i + 1 < argc && (std::string(argv[i + 1]) == "read" || std::string(argv[i + 1]) == "ff" || std::string(argv[i + 1]) == "both"))
            blendArg = argv[++i];
        else
        {
            std::fprintf(stderr, "usage: rc_gs2d_test [--only model,T1,...,T10,blenddbg] [--blend read|ff|both] [--no-faults] [--validation] [--quiet]\n");
            return 2;
        }
    }
    // The bare synchronous software GS (as rc_replay's emu machine).
    setenv("PS2X_GS_THREAD", "0", 1);
    setenv("PS2X_GS_GPU", "0", 1);
    setenv("PS2X_GS_HW", "0", 1);
    setenv("PS2X_GS_VK", "0", 1);

    const auto t0 = std::chrono::steady_clock::now();
    RhiDeviceDesc desc{};
    desc.sdlWindow = nullptr;
    desc.debugLayers = validation;
    desc.appName = "rc_gs2d_test";
    if (!rhi_init(&desc))
    {
        std::printf("rc_gs2d_test: rhi_init failed (no Vulkan device?)\nFAIL\n");
        return 1;
    }
    const bool localRead = rhi_limits()->localRead;
    std::printf("device: %s; shaderFloat64 %s, dual-source blend %s, D32F attachment %s, local read %s, validation %s\n", rhi_adapter_name(),
                rhi_limits()->shaderFloat64 ? "yes" : "no", rhi_limits()->dualSourceBlend ? "yes" : "no", rhi_limits()->depth32Attachment ? "yes" : "no",
                localRead ? "yes" : "no", rhi_limits()->validationLayer ? "on" : "off");
    if (blendArg != "ff" && !localRead)
    {
        std::printf("rc_gs2d_test: the device has no local read: the exact blend is not available; running fixed function only\n");
        if (blendArg == "read")
        {
            rhi_shutdown();
            std::printf("FAIL\n");
            return 1;
        }
        blendArg = "ff";
    }
    const bool runRead = blendArg != "ff", runFf = blendArg != "read";
    // Is synchronisation validation on? Two clears of one image with no barrier between them (recorded natively: the RHI
    // orders its own copies) are a write-after-write hazard only it reports (with --validation and
    // VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true); its errors are not counted against the run.
    uint32_t probeErrors = 0;
    if (validation)
    {
        const uint32_t e0 = rhi_vk_validation_error_count();
        RhiTextureDesc td{};
        td.width = td.height = 4u;
        td.mipLevels = 1u;
        td.format = RHI_FMT_RGBA8_UNORM;
        td.usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST;
        td.debugName = "sync probe";
        const RhiTexture tx = rhi_create_texture(&td);
        RhiNativeDevice nd{};
        RhiNativeTexture nt{};
        struct Range
        {
            uint32_t aspect, baseMip, mips, baseLayer, layers;
        };
        using GetProc = void *(*)(void *, const char *);
        using Clear = void (*)(void *, uint64_t, int, const float *, uint32_t, const Range *);
        if (rhi_native_device(&nd) && nd.kind == RHI_NATIVE_VULKAN && rhi_native_texture(tx, &nt))
            if (Clear clear = reinterpret_cast<Clear>(reinterpret_cast<GetProc>(nd.getDeviceProcAddr)(nd.device, "vkCmdClearColorImage")))
            {
                rhi_wait_frame();
                RhiCommandList cl = rhi_begin_commands();
                const RhiTextureBarrier toCopy{tx, RHI_STATE_UNDEFINED, RHI_STATE_COPY_DST};
                rhi_cmd_barrier(cl, &toCopy, 1u);
                void *cb = nullptr;
                if (rhi_cmd_external_begin(cl) && rhi_native_command_list(cl, &cb))
                {
                    const float c[4] = {0, 0, 0, 0};
                    const Range r{1u, 0u, 1u, 0u, 1u};
                    clear(cb, nt.image, 7 /* TRANSFER_DST_OPTIMAL */, c, 1u, &r);
                    clear(cb, nt.image, 7, c, 1u, &r);
                    rhi_cmd_external_end(cl);
                }
                rhi_submit(cl);
                rhi_wait_idle();
            }
        rhi_destroy_texture(tx);
        probeErrors = rhi_vk_validation_error_count() - e0;
        std::printf("synchronisation validation: %s (a planted write-after-write hazard gave %u error%s, not counted)\n", probeErrors ? "on" : "OFF", probeErrors,
                    probeErrors == 1 ? "" : "s");
    }
    Gs2dVk vk;
    std::string why;
    const auto tc = std::chrono::steady_clock::now();
    if (!vk.create(&why))
    {
        std::printf("rc_gs2d_test: Gs2dVk::create failed: %s\nFAIL\n", why.c_str());
        rhi_shutdown();
        return 1;
    }
    std::printf("Gs2dVk: pipelines made in %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc).count());

    Machines m;
    m.vk = &vk;
    auto want = [&](const char *n) { return only.empty() || only.count(n) != 0; };
    struct Entry
    {
        const char *name;
        std::function<void(Test &, Machines &)> run;
    };
    if (only.count("blenddbg"))
    {
        for (uint32_t fix : {0x20u, 0x15u})
        for (int bias = 0; bias < 2; ++bias)
        {
            setMode(&vk, false, bias != 0);
            Planes seed;
            for (uint32_t y = 0; y < kH; ++y)
                for (uint32_t x = 0; x < kW; ++x)
                {
                    const uint32_t c = x & 255u;
                    seed.c[0][y * kW + x] = rgbaOf(static_cast<uint8_t>(c), static_cast<uint8_t>(255 - c), static_cast<uint8_t>((37 * c) & 255), 0x40);
                }
            Gif g;
            Env e;
            e.alpha = 0x29 | static_cast<uint64_t>(fix) << 32;
            env(g, e);
            for (uint32_t y = 0; y < 8; ++y)
            {
                V a;
                a.x = 0;
                a.y = y;
                a.r = a.g = static_cast<uint8_t>(y);
                a.b = static_cast<uint8_t>(255 - y);
                a.a = 0x80;
                V b = a;
                b.x = 256;
                b.y = y + 1;
                rect(g, 0x43, a, b);
            }
            const Machines::Result r = m.run(g.bytes(), seed);
            std::printf("FIX %02x bias %d: R channel, Cs = row, Cd = column\n", fix, bias);
            for (uint32_t y = 0; y < 4; ++y)
            {
                std::printf(" Cs %u:", y);
                for (uint32_t x = 0; x < 12; ++x)
                    std::printf(" %u/%u", r.vk.c[0][y * kW + x] & 0xFF, r.emu.c[0][y * kW + x] & 0xFF);
                std::printf("\n");
            }
            std::printf(" G at Cs 0, Cd 252..245:");
            for (uint32_t x = 3; x < 11; ++x)
                std::printf(" %u/%u", (r.vk.c[0][x] >> 8) & 0xFF, (r.emu.c[0][x] >> 8) & 0xFF);
            std::printf("\n");
        }
        rhi_shutdown();
        return 0;
    }
    // T3-T6 in read mode where it runs, else fixed function (bias on); T7 does not depend on the mode.
    const bool primaryRead = runRead;
    auto primary = [&](Machines &mm) { setMode(mm.vk, primaryRead, true); };
    const std::vector<Entry> tests = {
        {"model", [](Test &t, Machines &) { modelSelfCheck(t); }},
        {"T1", [&](Test &t, Machines &mm)
         {
             if (runRead)
                 testT1(t, mm, false, true);
             if (runFf)
             {
                 testT1(t, mm, true, false);
                 testT1(t, mm, false, false);
             }
         }},
        {"T2", [&](Test &t, Machines &mm)
         {
             if (runRead)
                 testT2(t, mm, true);
             if (runFf)
                 testT2(t, mm, false);
         }},
        {"T3", [&](Test &t, Machines &mm) { primary(mm); testT3(t, mm, primaryRead); }},
        {"T4", [&](Test &t, Machines &mm) { primary(mm); testT4(t, mm); }},
        {"T5", [&](Test &t, Machines &mm) { primary(mm); testT5(t, mm); }},
        {"T6", [&](Test &t, Machines &mm) { primary(mm); testT6(t, mm); }},
        {"T7", [&](Test &t, Machines &mm) { primary(mm); testT7(t, mm); }},
        {"T8", [&](Test &t, Machines &mm)
         {
             if (runRead)
                 testT8(t, mm);
             else
                 t.notes.push_back("not run: no local read");
         }},
        {"T9", [&](Test &t, Machines &mm) { primary(mm); testT9(t, mm); }},
        {"T10", [&](Test &t, Machines &mm)
         {
             if (!runRead)
             {
                 t.notes.push_back("not run: no local read");
                 return;
             }
             testT10(t, mm);
             // The box rule of batch 4b (kGroupBoxes) on the same scenes: equal pictures, at least the pixel rule's barriers.
             if (mm.vk && mm.vk->options().faults == 0)
             {
                 Gs2dVk::Options o = mm.vk->options();
                 const Gs2dVk::Options keep = o;
                 o.grouping = Gs2dVk::kGroupBoxes;
                 mm.vk->setOptions(o);
                 if (!g_quiet)
                     std::printf("  T10 with the box rule (kGroupBoxes):\n");
                 testT10(t, mm);
                 mm.vk->setOptions(keep);
             }
         }},
    };
    bool allOk = true;
    std::vector<std::string> summary;
    for (const Entry &e : tests)
    {
        if (!want(e.name))
            continue;
        Test t;
        t.name = e.name;
        const auto ts = std::chrono::steady_clock::now();
        std::printf("%s\n", e.name);
        e.run(t, m);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
        std::printf("%s: %s (%" PRIu64 " checks, %" PRIu64 " failures, %.1f s)\n", e.name, t.ok() ? "pass" : "FAIL", t.checks, t.failures, s);
        for (const std::string &n : t.notes)
            std::printf("  note %s: %s\n", e.name, n.c_str());
        allOk &= t.ok();
    }

    if (faults)
    {
        // Each must fail its test. read: run in read mode (where available), else fixed function; "T1": the reading rows
        // and the integer-exact rows are counted separately (FIX off by one must fail both in read mode).
        struct FaultCase
        {
            const char *name, *test;
            uint32_t vkFault;
            bool texa;
            const char *notApplicable = nullptr;
            bool readOnly = false;
            const char *expectPass = nullptr; // cannot change a pixel (why); run and reported, not counted
        };
        const FaultCase cases[] = {
            {"ATST LEQUAL as LESS", "T3", Gs2dVk::kFaultAtstLess, false},
            {"failing part of the split dropped", "T3", Gs2dVk::kFaultDropFail, false},
            {"split per batch", "T3", Gs2dVk::kFaultSplitPerBatch, false},
            {"ZTST GREATER as GEQUAL", "T4", Gs2dVk::kFaultZGequal, false},
            {"half pixel left out", "T5", Gs2dVk::kFaultNoHalfPixel, false},
            {"bias where a < b", "T1", Gs2dVk::kFaultBiasAlways, false,
             "the exact blend has no bias; on fixed function D-2D-1's measured bound (-1..+1) cannot see it (15.4, 16.3)"},
            {"FIX off by one", "T1", Gs2dVk::kFaultFixOffByOne, false},
            {"TEXA ignored", "T6", 0, true},
            {"texel fetch one row off", "T6", Gs2dVk::kFaultTexelRow, false},
            {"vertex colour channel swapped", "T6", Gs2dVk::kFaultColourSwap, false},
            {"barrier left out (read)", "T8", Gs2dVk::kFaultNoBarrier, false, nullptr, true},
            {"reading batch as one group (read)", "T8", Gs2dVk::kFaultOneGroup, false, nullptr, true},
            {"no barrier at a pass's first read", "T10", Gs2dVk::kFaultNoFirstBarrier, false, nullptr, true},
            {"a reading batch sees its own stamps", "T10", Gs2dVk::kFaultCoverOwnBatch, false, nullptr, true},
            {"no stamps for non-reading prims", "T10", Gs2dVk::kFaultNoStampOther, false, nullptr, true},
            {"read epoch advanced, no barrier", "T10", Gs2dVk::kFaultEpochNoBarrier, false, nullptr, true},
            {"ZTST GEQUAL as GREATER", "T9", Gs2dVk::kFaultGequalAsGreater, false},
            {"the Z of a vertex off by one", "T9", Gs2dVk::kFaultZOffByOne, false},
            {"REPEAT drawn as CLAMP", "T9", Gs2dVk::kFaultRepeatAsClamp, false},
            {"Q left out", "T9", Gs2dVk::kFaultNoQ, false},
            {"a drawn triangle culled", "T9", Gs2dVk::kFaultCullDrawn, false},
            {"a culled triangle drawn", "T9", Gs2dVk::kFaultDrawCulled, false, nullptr, false,
             "the cull is the reference's own early exit (no area, or a box outside the scissor): drawn, such a triangle covers no pixel"},
        };
        std::printf("planted faults (each must fail its test; mode %s)\n", primaryRead ? "read" : "fixed function");
        const bool quiet = g_quiet;
        g_quiet = true;
        for (const FaultCase &f : cases)
        {
            if (f.notApplicable || (f.readOnly && !runRead))
            {
                std::printf("  %-36s %s: not applicable: %s\n", f.name, f.test, f.notApplicable ? f.notApplicable : "no local read");
                continue;
            }
            Test t;
            t.name = f.test;
            t.failFast = std::string(f.test) != "T1" && std::string(f.test) != "T8";
            Gs2dVk::Options o;
            o.blend = primaryRead ? Gs2dVk::kBlendRead : Gs2dVk::kBlendFixed;
            o.faults = f.vkFault;
            vk.setOptions(o);
            m.reader.faultIgnoreTexa = f.texa;
            m.useModel = false;
            const auto ts = std::chrono::steady_clock::now();
            if (std::string(f.test) == "T1")
                testT1(t, m, true, primaryRead);
            else
                for (const Entry &e : tests)
                    if (std::string(e.name) == f.test)
                        e.run(t, m);
            const bool caught = !t.ok();
            std::string extra;
            if (std::string(f.test) == "T1")
                extra = " (rows failing: " + std::to_string(t.readRowFails) + " reading or fractional, " + std::to_string(t.exactRowFails) + " integer exact)";
            else if (std::string(f.test) == "T8")
                extra = " (" + std::to_string(t.failures) + " of " + std::to_string(t.checks) + " scenes differ)";
            const bool both = std::string(f.test) != "T1" || !primaryRead || f.vkFault != Gs2dVk::kFaultFixOffByOne || (t.readRowFails && t.exactRowFails);
            if (f.expectPass)
            {
                std::printf("  %-36s %s: %s (expected: %s), %.1f s\n", f.name, f.test, caught ? "FAILS (unexpected)" : "passes", f.expectPass,
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
                allOk &= !caught;
            }
            else
            {
                std::printf("  %-36s %s: %s%s, %.1f s\n", f.name, f.test, caught && both ? "fails (caught)" : "PASSES (not caught)", extra.c_str(),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
                allOk &= caught && both;
            }
            m.reader.faultIgnoreTexa = false;
            m.useModel = true;
            vk.setOptions(Gs2dVk::Options{});
        }
        g_quiet = quiet;
    }

    const Gs2dVk::Stats &st = vk.stats();
    std::printf("Gs2dVk: %" PRIu64 " lists, %" PRIu64 " draws, %" PRIu64 " primitives, %" PRIu64 " records, %" PRIu64 " line pixels, %" PRIu64
                " clears, %" PRIu64 " textures, split: %" PRIu64 " primitives in %" PRIu64 " groups, %" PRIu64 " decided on the CPU; culled %" PRIu64
                ", beyond the viewport %" PRIu64 ", unsupported states %" PRIu64 "; exact blend: %" PRIu64 " reading primitives in %" PRIu64 " groups, %" PRIu64
                " barriers\n",
                st.lists, st.draws, st.prims, st.records, st.linePixels, st.clears, st.textures, st.splitPrims, st.splitGroups, st.decidedOnCpu, st.culled,
                st.beyondViewport, st.unsupported, st.readPrims, st.readGroups, st.barriers);
    const auto &fb = GSRaster::FallbackCounters();
    std::printf("emulated GS: %" PRIu64 " scenes; fast-path fallbacks", m.scenes);
    for (uint64_t c : fb)
        std::printf(" %" PRIu64, c);
    std::printf("\n");
    vk.destroy();
    const uint32_t errors = rhi_vk_validation_error_count() - probeErrors;
    rhi_shutdown();
    std::printf("validation errors: %u%s\n", errors, validation ? "" : " (layer off)");
    std::printf("time %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    allOk &= errors == 0;
    std::printf("%s\n", allOk ? "PASS" : "FAIL");
    return allOk ? 0 : 1;
}
