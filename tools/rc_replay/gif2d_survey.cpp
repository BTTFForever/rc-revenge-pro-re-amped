// rc_replay's gif2dstat consumer: see gif2d_survey.h and RENDERER_2D_DESIGN.md ("Measurements").
#include "gif2d_survey.h"

#include "../../render/record/chain_walk.h"
#include "../../render/tex/gif_walk.h"
// The software GS's own triangle set-up (header only), so coverage and weights here are the reference's.
#include "../../third_party/ps2recomp/ps2xRuntime/src/lib/gs/gs_triangle_rules.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cmath>
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
        enum : uint8_t
        {
            rPrim = 0x00, rRgbaq = 0x01, rSt = 0x02, rUv = 0x03, rXyzf2 = 0x04, rXyz2 = 0x05, rTex0 = 0x06, rClamp = 0x08,
            rXyzf3 = 0x0C, rXyz3 = 0x0D, rTex1 = 0x14, rXyoffset = 0x18, rPrmodecont = 0x1A, rPrmode = 0x1B, rTexa = 0x3B,
            rScissor = 0x40, rAlpha = 0x42, rDthe = 0x45, rColclamp = 0x46, rTest = 0x47, rPabe = 0x49, rFba = 0x4A,
            rFrame = 0x4C, rZbuf = 0x4E,
        };
        struct Vtx
        {
            int32_t x = 0, y = 0; // 12.4, as written (before XYOFFSET)
            uint32_t z = 0;
            uint8_t r = 0, g = 0, b = 0, a = 0;
            float s = 0, t = 0, q = 1;
            uint16_t u = 0, v = 0;
        };
        struct Count
        {
            uint64_t prims = 0, pixels = 0;
        };
        struct Box
        {
            int x0, y0, x1, y1;
            uint32_t zmax;
        };
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
    }

    struct Gif2dSurvey::Impl final : GifSink
    {
        PS2Runtime *rt = nullptr;
        uint32_t width = 640, height = 240;
        uint64_t regs[0x80] = {};
        GifWalker walker;
        Vtx queue[3];
        uint32_t queued = 0;
        bool stripOdd = false;
        std::map<std::string, std::map<std::string, Count>> hist;
        // per cycle
        std::vector<uint8_t> layers;   // inexactly blended layers per pixel
        std::vector<Box> zWriters;     // earlier PATH3 primitives of the cycle that may write Z
        uint64_t cyclePrims = 0, cyclePixels = 0, cycleDrawKicks = 0;
        // per kick
        uint64_t kickPrims = 0;
        uint32_t kickZmin = 0xFFFFFFFFu, zbufMaxBefore = 0;
        bool kickHasNonSprite = false;
        uint64_t noRegs = 0;

        void add(const char *cat, const std::string &key, uint64_t prims, uint64_t pixels)
        {
            Count &c = hist[cat][key];
            c.prims += prims;
            c.pixels += pixels;
        }
        void reg(uint8_t addr, uint64_t value) override;
        void image(const uint8_t *, uint32_t) override {}
        void vertex(uint64_t value, bool fogForm, bool kick);
        void primitive(const Vtx *v, uint32_t n, uint32_t type);
        uint32_t ctx() const
        {
            const uint64_t attr = (regs[rPrmodecont] & 1u) ? regs[rPrim] : regs[rPrmode];
            return static_cast<uint32_t>((attr >> 9) & 1u);
        }
    };

    void Gif2dSurvey::Impl::reg(uint8_t addr, uint64_t value)
    {
        switch (addr)
        {
        case rXyz2: vertex(value, false, true); return;
        case rXyzf2: vertex(value, true, true); return;
        case rXyz3: vertex(value, false, false); return;
        case rXyzf3: vertex(value, true, false); return;
        default: break;
        }
        if (addr < 0x80u)
            regs[addr] = value;
        if (addr == rPrim)
        {
            queued = 0;
            stripOdd = false;
        }
        if (addr >= 0x06 && (addr & 1u) && addr != rTex1 + 1u && (addr == 0x07 || addr == 0x09 || addr == 0x15 || addr == 0x17 || addr == 0x19 ||
                                                                 addr == 0x41 || addr == 0x43 || addr == 0x48 || addr == 0x4B || addr == 0x4D || addr == 0x4F))
            add("reg", fmt("context 2 register 0x%02x written", addr), 1, 0);
        if (addr == rPrmode || addr == 0x16 || addr == 0x1C || addr == 0x22 || addr == 0x3D || addr == 0x44 || addr == 0x0A)
            add("reg", fmt("register 0x%02x written (PRMODE/TEX2/TEXCLUT/SCANMSK/FOGCOL/DIMX/FOG)", addr), 1, 0);
    }

    void Gif2dSurvey::Impl::vertex(uint64_t value, bool fogForm, bool kick)
    {
        Vtx v;
        v.x = static_cast<int32_t>(value & 0xFFFFu);
        v.y = static_cast<int32_t>((value >> 16) & 0xFFFFu);
        v.z = fogForm ? static_cast<uint32_t>((value >> 32) & 0xFFFFFFu) : static_cast<uint32_t>(value >> 32);
        const uint64_t c = regs[rRgbaq];
        v.r = static_cast<uint8_t>(c);
        v.g = static_cast<uint8_t>(c >> 8);
        v.b = static_cast<uint8_t>(c >> 16);
        v.a = static_cast<uint8_t>(c >> 24);
        const uint32_t qb = static_cast<uint32_t>(c >> 32), sb = static_cast<uint32_t>(regs[rSt]), tb = static_cast<uint32_t>(regs[rSt] >> 32);
        std::memcpy(&v.q, &qb, 4);
        std::memcpy(&v.s, &sb, 4);
        std::memcpy(&v.t, &tb, 4);
        v.u = static_cast<uint16_t>(regs[rUv] & 0x3FFFu);
        v.v = static_cast<uint16_t>((regs[rUv] >> 16) & 0x3FFFu);
        if (fogForm)
            add("reg", "XYZF vertex", 1, 0);
        if (!kick)
            add("reg", "vertex without a drawing kick (XYZ3 / ADC)", 1, 0);

        const uint64_t attr = (regs[rPrmodecont] & 1u) ? regs[rPrim] : regs[rPrmode];
        (void)attr;
        const uint32_t type = static_cast<uint32_t>(regs[rPrim] & 7u);
        switch (type)
        {
        case 0: // point
            if (kick)
                primitive(&v, 1, type);
            break;
        case 1: // line list
        case 6: // sprite
            queue[queued++] = v;
            if (queued == 2)
            {
                if (kick)
                    primitive(queue, 2, type);
                queued = 0;
            }
            break;
        case 2: // line strip
            queue[queued++] = v;
            if (queued == 2)
            {
                if (kick)
                    primitive(queue, 2, type);
                queue[0] = queue[1];
                queued = 1;
            }
            break;
        case 3: // triangle list
            queue[queued++] = v;
            if (queued == 3)
            {
                if (kick)
                    primitive(queue, 3, type);
                queued = 0;
            }
            break;
        case 4: // strip
            queue[queued++] = v;
            if (queued == 3)
            {
                if (kick)
                    primitive(queue, 3, type);
                queue[0] = queue[1];
                queue[1] = queue[2];
                queued = 2;
            }
            break;
        case 5: // fan
            queue[queued++] = v;
            if (queued == 3)
            {
                if (kick)
                    primitive(queue, 3, type);
                queue[1] = queue[2];
                queued = 2;
            }
            break;
        default: break;
        }
    }

    void Gif2dSurvey::Impl::primitive(const Vtx *v, uint32_t n, uint32_t type)
    {
        static const char *const typeName[8] = {"point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"};
        const uint32_t c = ctx();
        const uint64_t prim = ((regs[rPrmodecont] & 1u) ? regs[rPrim] : ((regs[rPrmode] & ~7ull) | (regs[rPrim] & 7u))) & 0x7FFu;
        const bool iip = prim & 8u, tme = prim & 0x10u, fge = prim & 0x20u, abe = prim & 0x40u, aa1 = prim & 0x80u, fst = prim & 0x100u;
        const uint64_t alpha = regs[rAlpha + c], test = regs[rTest + c], zbuf = regs[rZbuf + c], frame = regs[rFrame + c];
        const uint64_t scissor = regs[rScissor + c], xyoff = regs[rXyoffset + c], tex0 = regs[rTex0 + c], tex1 = regs[rTex1 + c];
        const uint64_t clamp = regs[rClamp + c];
        const int ofx = static_cast<int>(xyoff & 0xFFFFu), ofy = static_cast<int>((xyoff >> 32) & 0xFFFFu);
        const int sx0 = static_cast<int>(scissor & 0x7FFu), sx1 = static_cast<int>((scissor >> 16) & 0x7FFu);
        const int sy0 = static_cast<int>((scissor >> 32) & 0x7FFu), sy1 = static_cast<int>((scissor >> 48) & 0x7FFu);
        const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu), tw = static_cast<uint32_t>((tex0 >> 26) & 0xFu),
                       th = static_cast<uint32_t>((tex0 >> 30) & 0xFu), tcc = static_cast<uint32_t>((tex0 >> 34) & 1u),
                       tfx = static_cast<uint32_t>((tex0 >> 35) & 3u);
        const int texW = 1 << std::min(tw, 10u), texH = 1 << std::min(th, 10u);
        const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
        const bool zte = (test >> 16) & 1u, zmsk = (zbuf >> 32) & 1u;
        const uint32_t csel = static_cast<uint32_t>((alpha >> 4) & 3u), fix = static_cast<uint32_t>((alpha >> 32) & 0xFFu);
        const bool inexactBlend = abe && !(csel == 2u && (fix == 0u || fix == 0x80u));

        ++kickPrims;
        ++cyclePrims;
        if (type != 6u)
            kickHasNonSprite = true;

        // Pixels: coverage as the software GS decides it.
        int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
        uint64_t pixels = 0, flatOff = 0, flatPix = 0, texCentre = 0, texMix = 0, fracU[7] = {}, fracV[7] = {};
        uint32_t zmin = 0xFFFFFFFFu, zmax = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            zmin = std::min(zmin, v[i].z);
            zmax = std::max(zmax, v[i].z);
        }
        if (type == 6u)
            zmin = zmax = v[1].z;
        const bool flat = !iip || (n == 3 && !std::memcmp(&v[0].r, &v[1].r, 4) && !std::memcmp(&v[0].r, &v[2].r, 4)) ||
                          (n == 2 && (type == 6u || !std::memcmp(&v[0].r, &v[1].r, 4))) || n == 1;
        const bool alphaFlat = !iip || n == 1 || type == 6u || (v[0].a == v[1].a && (n == 2 || v[0].a == v[2].a));
        std::vector<uint8_t> *lay = inexactBlend ? &layers : nullptr;
        if (n == 3)
        {
            GSTriangleRaster t;
            if (gsTriangleSetup(v[0].x / 16.0f, v[0].y / 16.0f, v[1].x / 16.0f, v[1].y / 16.0f, v[2].x / 16.0f, v[2].y / 16.0f, ofx, ofy, t))
            {
                bx0 = std::max(t.minX, sx0);
                bx1 = std::min(t.maxX, sx1);
                by0 = std::max(t.minY, sy0);
                by1 = std::min(t.maxY, sy1);
                for (int y = by0; y <= by1; ++y)
                    for (int x = bx0; x <= bx1; ++x)
                    {
                        const int64_t e[3] = {gsTriangleEdge(t, 0, x, y), gsTriangleEdge(t, 1, x, y), gsTriangleEdge(t, 2, x, y)};
                        if (!gsTriangleCovers(t, e))
                            continue;
                        ++pixels;
                        if (lay && x >= 0 && y >= 0 && x < 640 && y < 512)
                        {
                            uint8_t &l = (*lay)[static_cast<size_t>(y) * 640u + x];
                            if (l < 255)
                                ++l;
                        }
                        const float w0 = gsTriangleWeight(t, e[0]), w1 = gsTriangleWeight(t, e[1]), w2 = gsTriangleWeight(t, e[2]);
                        if (flat && iip)
                        {
                            ++flatPix;
                            const uint8_t ch[4] = {v[0].r, v[0].g, v[0].b, v[0].a};
                            bool off = false;
                            for (uint8_t cv : ch)
                                off |= static_cast<int>(cv * w0 + cv * w1 + cv * w2) != cv;
                            flatOff += off;
                        }
                        if (tme)
                        {
                            float uf, vf;
                            if (fst)
                            {
                                uf = static_cast<uint16_t>(v[0].u * w0 + v[1].u * w1 + v[2].u * w2) / 16.0f;
                                vf = static_cast<uint16_t>(v[0].v * w0 + v[1].v * w1 + v[2].v * w2) / 16.0f;
                            }
                            else
                            {
                                const float is = v[0].s * w0 + v[1].s * w1 + v[2].s * w2, it = v[0].t * w0 + v[1].t * w1 + v[2].t * w2;
                                const float iq = v[0].q * w0 + v[1].q * w1 + v[2].q * w2;
                                const float invQ = 1.0f / ((std::fabs(iq) > 1.0e-8f) ? iq : 1.0f);
                                uf = is * invQ * static_cast<float>(texW);
                                vf = it * invQ * static_cast<float>(texH);
                            }
                            const float fu = (uf - 0.5f) - std::floor(uf - 0.5f), fv = (vf - 0.5f) - std::floor(vf - 0.5f);
                            const auto near = [](float f) { return f < 1.0f / 512.0f || f > 1.0f - 1.0f / 512.0f; };
                            if (near(fu) && near(fv))
                                ++texCentre;
                            else
                                ++texMix;
                            // The weight as the software GS has it: 0, 1/2, another sixteenth, each exactly or only nearly.
                            const auto cls = [](float f) -> int
                            {
                                if (f == 0.0f) return 0;
                                if (f == 0.5f) return 1;
                                const float s16 = f * 16.0f, r = std::nearbyint(s16);
                                if (s16 == r) return 2;
                                if (std::fabs(f - 0.5f) < 1e-3f) return 4;
                                if (f < 1e-3f || f > 1.0f - 1e-3f) return 3;
                                if (std::fabs(s16 - r) < 1e-2f) return 5;
                                return 6;
                            };
                            ++fracU[cls(fu)];
                            ++fracV[cls(fv)];
                        }
                    }
            }
            else
                add("geom", "zero-area triangle", 1, 0);
        }
        else if (n == 2 && type == 6u)
        {
            const int xa = std::min(v[0].x, v[1].x) - ofx, xb = std::max(v[0].x, v[1].x) - ofx;
            const int ya = std::min(v[0].y, v[1].y) - ofy, yb = std::max(v[0].y, v[1].y) - ofy;
            bx0 = std::max((xa + 15) >> 4, sx0);
            bx1 = std::min(((xb + 15) >> 4) - 1, sx1);
            by0 = std::max((ya + 15) >> 4, sy0);
            by1 = std::min(((yb + 15) >> 4) - 1, sy1);
            if (bx1 >= bx0 && by1 >= by0)
                pixels = static_cast<uint64_t>(bx1 - bx0 + 1) * (by1 - by0 + 1);
        }
        else if (n == 2)
        {
            const int xa = (v[0].x >> 4) - (ofx >> 4), ya = (v[0].y >> 4) - (ofy >> 4), xb = (v[1].x >> 4) - (ofx >> 4), yb = (v[1].y >> 4) - (ofy >> 4);
            bx0 = std::min(xa, xb);
            bx1 = std::max(xa, xb);
            by0 = std::min(ya, yb);
            by1 = std::max(ya, yb);
            pixels = static_cast<uint64_t>(std::max(bx1 - bx0, by1 - by0)) + 1u;
            const int len = static_cast<int>(pixels);
            const char *dir = (xa == xb && ya == yb) ? "point" : ya == yb ? "horizontal" : xa == xb ? "vertical" : "diagonal";
            const bool frac = ((v[0].x - ofx) | (v[0].y - ofy) | (v[1].x - ofx) | (v[1].y - ofy)) & 15;
            add("line", fmt("%s len %s%s%s%s", dir, len <= 2 ? "1-2" : len <= 16 ? "3-16" : len <= 128 ? "17-128" : "129+", flat ? " flat" : " gradient",
                            frac ? " subpixel" : " whole", (bx0 < sx0 || bx1 > sx1 || by0 < sy0 || by1 > sy1) ? " crosses scissor" : ""),
                1, pixels);
        }
        else
            pixels = 1;
        cyclePixels += pixels;
        kickZmin = std::min(kickZmin, zmin);

        // The draw state.
        std::string texPart = "-";
        if (tme)
            texPart = fmt("psm=%02x tcc=%u tfx=%u tex1=%" PRIx64 " clamp=%" PRIx64, psm, tcc, tfx, tex1, clamp);
        add("state",
            fmt("%s prim=%03" PRIx64 " alpha=%s test=%05" PRIx64 " pabe=%u fba=%u zbuf=%" PRIx64 " frame=fbw%u/psm%02x/msk%08x colclamp=%u dthe=%u ctx=%u tex[%s]",
                typeName[type], prim, abe ? fmt("%" PRIx64, alpha).c_str() : "-", test, static_cast<unsigned>(regs[rPabe] & 1u),
                static_cast<unsigned>(regs[rFba + c] & 1u), zbuf, static_cast<unsigned>((frame >> 16) & 0x3Fu),
                static_cast<unsigned>((frame >> 24) & 0x3Fu), static_cast<unsigned>(frame >> 32), static_cast<unsigned>(regs[rColclamp] & 1u),
                static_cast<unsigned>(regs[rDthe] & 1u), c, texPart.c_str()),
            1, pixels);
        // The short form the design's tables use.
        add("blend", fmt("%s%s%s alpha=%s test=%05" PRIx64 " zmsk=%u", typeName[type], tme ? " TME" : "", iip ? " IIP" : "",
                         abe ? fmt("%" PRIx64, alpha).c_str() : "-", test, zmsk),
            1, pixels);
        add("reg", fmt("PRIM attributes iip=%u tme=%u fge=%u abe=%u aa1=%u fst=%u ctxt=%u fix=%u", iip, tme, fge, abe, aa1, fst, c,
                       static_cast<unsigned>((prim >> 10) & 1u)), 1, pixels);
        add("reg", fmt("SCISSOR %d..%d x %d..%d", sx0, sx1, sy0, sy1), 1, pixels);
        add("reg", fmt("XYOFFSET %d.%d, %d.%d", ofx >> 4, ofx & 15, ofy >> 4, ofy & 15), 1, pixels);
        add("reg", fmt("FRAME %" PRIx64, frame), 1, pixels);
        add("reg", fmt("ZBUF %" PRIx64, zbuf), 1, pixels);
        add("reg", fmt("PRMODECONT %" PRIx64, regs[rPrmodecont] & 1u), 1, pixels);
        add("reg", fmt("TEST zte=%u ztst=%u date=%u", zte, ztst, static_cast<unsigned>((test >> 14) & 1u)), 1, pixels);
        if (tme)
        {
            add("reg", fmt("TEX1 %" PRIx64, tex1), 1, pixels);
            add("reg", fmt("CLAMP %" PRIx64, clamp), 1, pixels);
            add("reg", fmt("TEXA %" PRIx64 " (psm %02x)", regs[rTexa], psm), 1, pixels);
            add("reg", fmt("texture psm=%02x %dx%d tcc=%u tfx=%u", psm, texW, texH, tcc, tfx), 1, pixels);
        }
        add("z", fmt("%s", type == 6u ? "sprite" : "2D") + fmt(" z=%08x%s", zmin, zmin == zmax ? "" : " (varies in the primitive)"), 1, pixels);

        // Geometry.
        if (n == 3)
        {
            bool frac = false;
            for (int i = 0; i < 3; ++i)
                frac |= ((v[i].x - ofx) | (v[i].y - ofy)) & 15;
            int reach = 0;
            for (int i = 0; i < 3; ++i)
                reach = std::max({reach, std::abs(v[i].x - ofx), std::abs(v[i].y - ofy)});
            add("geom", fmt("vertex reach from the origin %s", reach <= 10240 ? "<= 640 pixels" : reach <= 16384 ? "<= 1024 pixels" : "> 1024 pixels"), 1, pixels);
            int axis = 0;
            for (int i = 0; i < 3; ++i)
            {
                const Vtx &a = v[i], &b = v[(i + 1) % 3];
                axis += (a.x == b.x) || (a.y == b.y);
            }
            const bool q1 = fst || (v[0].q == 1.0f && v[1].q == 1.0f && v[2].q == 1.0f);
            const bool qEq = fst || (v[0].q == v[1].q && v[0].q == v[2].q);
            const uint8_t cmax = std::max({v[0].r, v[0].g, v[0].b, v[1].r, v[1].g, v[1].b, v[2].r, v[2].g, v[2].b});
            const uint8_t amax = std::max({v[0].a, v[1].a, v[2].a});
            add("geom", fmt("tri%s %s %s xy %s%s", tme ? " TME" : "", flat ? "flat colour" : alphaFlat ? "Gouraud rgb, flat alpha" : "Gouraud with alpha",
                            axis >= 2 ? "half of an axis-aligned rectangle" : "free triangle", frac ? "subpixel" : "whole pixels",
                            tme ? (q1 ? " q=1" : qEq ? " q equal, not 1" : " q varies") : ""),
                1, pixels);
            if (tme)
                add("geom", fmt("TME vertex rgb max %s, alpha max %s", cmax > 0x80 ? "> 0x80" : "<= 0x80", amax > 0x80 ? "> 0x80" : "<= 0x80"), 1, pixels);
            else
                add("geom", fmt("untextured vertex alpha max %s", amax > 0x80 ? "> 0x80" : amax == 0x80 ? "0x80" : "< 0x80"), 1, pixels);
            if (flat && iip)
            {
                add("flat", "Gouraud primitive with one colour: pixels the software GS shades with that colour", 0, flatPix - flatOff);
                add("flat", "Gouraud primitive with one colour: pixels the software GS shades one level off", flatOff ? 1 : 0, flatOff);
            }
            if (tme)
            {
                // Texel coordinates at the vertices; the scale along the two axis-aligned edges when there are two.
                float uu[3], vv[3];
                bool outside = false;
                for (int i = 0; i < 3; ++i)
                {
                    if (fst)
                    {
                        uu[i] = v[i].u / 16.0f;
                        vv[i] = v[i].v / 16.0f;
                    }
                    else
                    {
                        const float q = (std::fabs(v[i].q) > 1.0e-8f) ? v[i].q : 1.0f;
                        uu[i] = v[i].s / q * texW;
                        vv[i] = v[i].t / q * texH;
                    }
                    outside |= uu[i] < 0.0f || vv[i] < 0.0f || uu[i] > texW || vv[i] > texH;
                }
                std::string scale = "not a rectangle";
                if (axis >= 2)
                {
                    float du = 0, dv = 0;
                    bool have = false, swapped = false;
                    for (int i = 0; i < 3; ++i)
                    {
                        const Vtx &a = v[i], &b = v[(i + 1) % 3];
                        const float eu = uu[(i + 1) % 3] - uu[i], ev = vv[(i + 1) % 3] - vv[i];
                        if (a.y == b.y && a.x != b.x)
                        {
                            du = eu * 16.0f / (b.x - a.x);
                            swapped |= std::fabs(ev) > 1e-3f;
                            have = true;
                        }
                        if (a.x == b.x && a.y != b.y)
                        {
                            dv = ev * 16.0f / (b.y - a.y);
                            swapped |= std::fabs(eu) > 1e-3f;
                        }
                    }
                    const auto cls = [](float d)
                    {
                        d = std::fabs(d);
                        return std::fabs(d - 1.0f) < 1e-3f ? "1" : d < 1.0f ? (std::fabs(d - 0.5f) < 1e-3f ? "1/2" : "<1") : (std::fabs(d - 2.0f) < 1e-3f ? "2" : ">1");
                    };
                    scale = !have ? "?" : swapped ? "rotated or sheared" : fmt("texels per pixel u %s%s, v %s%s", cls(du), du < 0 ? " (mirrored)" : "", cls(dv), dv < 0 ? " (mirrored)" : "");
                }
                add("texmap", scale, 1, pixels);
                add("texpix", "pixels sampling at a texel centre (bilinear weights 0 or 1 within 1/512)", 0, texCentre);
                add("texpix", "pixels mixing texels", 0, texMix);
                static const char *const fracName[7] = {"exactly 0", "exactly 1/2", "exactly another sixteenth", "within 1/1000 of 0 or 1, not exact",
                                                        "within 1/1000 of 1/2, not exact", "near another sixteenth, not exact", "anything else"};
                for (int i = 0; i < 7; ++i)
                {
                    if (fracU[i])
                        add("texfrac", fmt("u weight %s", fracName[i]), 0, fracU[i]);
                    if (fracV[i])
                        add("texfrac", fmt("v weight %s", fracName[i]), 0, fracV[i]);
                }
                add("uvrange", fmt("%s, CLAMP %" PRIx64, outside ? "vertex texel coordinates outside 0..size" : "inside 0..size", clamp & 0xFu), 1, pixels);
            }
        }

        // Z order among PATH3's own primitives, and layers.
        if (type != 6u && zte && (ztst == 2u || ztst == 3u) && bx1 >= bx0 && by1 >= by0)
        {
            bool hidden = false;
            for (const Box &b : zWriters)
                if (b.x0 <= bx1 && bx0 <= b.x1 && b.y0 <= by1 && by0 <= b.y1 && (ztst == 3u ? b.zmax >= zmin : b.zmax > zmin))
                {
                    hidden = true;
                    break;
                }
            add("zorder", hidden ? "Z-tested 2D primitive overlapping (by box) an earlier 2D primitive with Z at or above its own" : "Z-tested 2D primitive above every earlier overlapping 2D Z write",
                1, pixels);
        }
        if (!zmsk && type != 6u && bx1 >= bx0 && by1 >= by0)
            zWriters.push_back({bx0, by0, bx1, by1, zmax});
    }

    Gif2dSurvey::Gif2dSurvey(PS2Runtime *runtime) : m(std::make_unique<Impl>())
    {
        m->rt = runtime;
        m->layers.assign(640u * 512u, 0);
    }
    Gif2dSurvey::~Gif2dSurvey() = default;

    void Gif2dSurvey::open(const FileInfo &info)
    {
        if (info.header.width)
            m->width = info.header.width;
        if (info.header.height)
            m->height = info.header.height;
    }

    void Gif2dSurvey::cycleBegin(uint64_t)
    {
        std::fill(m->layers.begin(), m->layers.end(), 0);
        m->zWriters.clear();
        m->cyclePrims = m->cyclePixels = m->cycleDrawKicks = 0;
    }

    void Gif2dSurvey::kick(const KickView &k, const RamView &ram)
    {
        if (k.channel != 2u)
            return;
        if (!k.regs)
        {
            ++m->noRegs;
            return;
        }
        std::memcpy(m->regs, k.regs, kGsRegCount * 8u);
        m->kickPrims = 0;
        m->kickZmin = 0xFFFFFFFFu;
        m->kickHasNonSprite = false;
        struct Feed final : WalkVisitor
        {
            Impl *self;
            void range(const WalkRange &r) override
            {
                if (!r.isTag && r.bytes)
                    self->walker.feed(r.data, r.bytes, *self);
            }
        } feed;
        feed.self = m.get();
        walkChain(k, ram, &feed);
        if (!m->kickPrims)
            return;
        ++m->cycleDrawKicks;
        if (!m->kickHasNonSprite)
            return;
        m->add("kick", fmt("2D kick with %s primitives", m->kickPrims <= 16 ? "1-16" : m->kickPrims <= 128 ? "17-128" : m->kickPrims <= 1024 ? "129-1024" : "1025+"),
               1, 0);
        if (m->rt)
        {
            // The emulated Z buffer before this kick (this consumer runs in front of emu): the largest Z24 value of
            // the pages under the picture, against the smallest Z this kick draws with.
            const uint64_t zbuf = m->regs[rZbuf], frame = m->regs[rFrame];
            const uint32_t zbp = static_cast<uint32_t>(zbuf & 0x1FFu), fbw = std::max<uint32_t>(1u, static_cast<uint32_t>((frame >> 16) & 0x3Fu));
            const uint8_t *vram = m->rt->memory().getGSVRAM();
            const uint32_t pageRows = m->height / 32u; // whole page rows inside the picture (rows below are never cleared)
            uint32_t zmaxBefore = 0;
            for (uint32_t p = 0; p < pageRows * fbw; ++p)
            {
                const uint8_t *page = vram + (((zbp + p) & 0x1FFu) << 13);
                for (uint32_t i = 0; i < 8192u; i += 4u)
                {
                    uint32_t w;
                    std::memcpy(&w, page + i, 4);
                    zmaxBefore = std::max(zmaxBefore, w & 0xFFFFFFu);
                }
            }
            m->add("zunder", m->kickZmin > zmaxBefore ? "2D kick whose smallest Z is above every Z of the picture under it" : "2D kick with a Z at or below a Z of the picture under it",
                   1, 0);
            m->add("zunder", fmt("largest Z under a 2D kick: %s", zmaxBefore < 0x10000u ? "< 0x10000" : zmaxBefore < 0x800000u ? "< 0x800000" : zmaxBefore < 0xFFFFFFu ? "< 0xffffff" : "0xffffff"), 1, 0);
        }
    }

    void Gif2dSurvey::cycleEnd(const CycleInfo &, const uint8_t *)
    {
        if (!m->cyclePrims)
            return;
        uint64_t byLayers[8] = {};
        uint32_t most = 0;
        for (uint8_t l : m->layers)
            if (l)
            {
                ++byLayers[std::min<uint32_t>(l, 7u)];
                most = std::max<uint32_t>(most, l);
            }
        for (uint32_t i = 1; i < 8; ++i)
            if (byLayers[i])
                m->add("layers", fmt("pixels under %u%s inexactly blended PATH3 layers in a cycle", i, i == 7 ? " or more" : ""), 0, byLayers[i]);
        m->add("layers", fmt("cycles whose deepest pixel has %s such layers", most <= 4 ? fmt("%u", most).c_str() : most <= 8 ? "5-8" : most <= 16 ? "9-16" : "17+"), 1, 0);
        m->add("kick", fmt("cycles with %" PRIu64 " PATH3 drawing kicks", m->cycleDrawKicks), 1, 0);
        m->add("kick", fmt("cycles with %s PATH3 primitives", m->cyclePrims <= 16 ? "1-16" : m->cyclePrims <= 128 ? "17-128" : m->cyclePrims <= 1024 ? "129-1024" : "1025+"), 1, m->cyclePixels);
    }

    void Gif2dSurvey::summary() const
    {
        if (m->noRegs)
            std::printf("H\tkick\tchannel 2 kicks without a register file (not an exact dump)\t%" PRIu64 "\t0\n", m->noRegs);
        for (const auto &[cat, keys] : m->hist)
            for (const auto &[key, c] : keys)
                std::printf("H\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\n", cat.c_str(), key.c_str(), c.prims, c.pixels);
    }
}
