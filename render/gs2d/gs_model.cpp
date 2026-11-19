#include "gs_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rc::rd
{
    namespace
    {
        uint8_t clampU8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

        // Round half up of a non-negative float, by truncation and a comparison (the reference's ExactLround for the
        // bilinear filter's values, which lie in 0..255).
        int roundHalfUp(float v)
        {
            int t = static_cast<int>(v);
            const float f = v - static_cast<float>(t);
            if (v >= 0.0f)
            {
                if (f >= 0.5f)
                    ++t;
            }
            else if (f <= -0.5f)
                --t;
            return t;
        }

        // F's value for a plan (FIX, or As), and |ks| * 128 and kd * 128 as integers.
        uint32_t coefTimes128(uint8_t coef, uint32_t f)
        {
            switch (coef)
            {
            case kCoefZero: return 0u;
            case kCoefOne: return 128u;
            case kCoefF: return f;
            case kCoefOneMinusF: return 128u - f;
            case kCoefMinusF: return f;
            case kCoefMinusOne: return 128u;
            default: return 0u;
            }
        }
    }

    Rgba gsTextureFunction(uint32_t tfx, uint32_t tcc, Rgba t, Rgba v)
    {
        Rgba o;
        switch (tfx & 3u)
        {
        case 0: // MODULATE
            o.r = clampU8((t.r * v.r) >> 7);
            o.g = clampU8((t.g * v.g) >> 7);
            o.b = clampU8((t.b * v.b) >> 7);
            o.a = tcc ? clampU8((t.a * v.a) >> 7) : v.a;
            break;
        case 1: // DECAL
            o = t;
            o.a = tcc ? t.a : v.a;
            break;
        case 2: // HIGHLIGHT
            o.r = clampU8(((t.r * v.r) >> 7) + v.a);
            o.g = clampU8(((t.g * v.g) >> 7) + v.a);
            o.b = clampU8(((t.b * v.b) >> 7) + v.a);
            o.a = tcc ? clampU8(t.a + v.a) : v.a;
            break;
        default: // HIGHLIGHT2
            o.r = clampU8(((t.r * v.r) >> 7) + v.a);
            o.g = clampU8(((t.g * v.g) >> 7) + v.a);
            o.b = clampU8(((t.b * v.b) >> 7) + v.a);
            o.a = tcc ? t.a : v.a;
            break;
        }
        return o;
    }

    bool gsAlphaPass(uint32_t atst, uint32_t aref, uint32_t as)
    {
        switch (atst & 7u)
        {
        case kAtstNever: return false;
        case kAtstAlways: return true;
        case kAtstLess: return as < aref;
        case kAtstLequal: return as <= aref;
        case kAtstEqual: return as == aref;
        case kAtstGequal: return as >= aref;
        case kAtstGreater: return as > aref;
        default: return as != aref;
        }
    }

    Write gsAlphaWrite(const GsState &s, uint32_t as)
    {
        const bool zmsk = (s.zbuf >> 32) & 1u;
        Write w{true, true, !zmsk};
        const bool ate = s.test & 1u;
        if (!ate || gsAlphaPass(static_cast<uint32_t>((s.test >> 1) & 7u), static_cast<uint32_t>((s.test >> 4) & 0xFFu), as))
            return w;
        switch ((s.test >> 12) & 3u)
        {
        case 0: return {false, false, false};          // KEEP
        case 1: return {true, true, false};            // FB_ONLY
        case 2: return {false, false, !zmsk};          // ZB_ONLY
        default: return {true, false, false};          // RGB_ONLY (CT32 frame)
        }
    }

    bool gsDepthPass(uint32_t ztst, uint32_t z, uint32_t stored)
    {
        switch (ztst & 3u)
        {
        case 0: return false;
        case 1: return true;
        case 2: return z >= stored;
        default: return z > stored;
        }
    }

    uint32_t gsBlend(uint64_t alpha, uint32_t cs, uint32_t cd, uint32_t as, uint32_t ad)
    {
        const uint32_t asel = alpha & 3u, bsel = (alpha >> 2) & 3u, csel = (alpha >> 4) & 3u, dsel = (alpha >> 6) & 3u;
        const int fix = static_cast<int>((alpha >> 32) & 0xFFu);
        auto pick = [&](uint32_t sel) { return sel == 0u ? static_cast<int>(cs) : sel == 1u ? static_cast<int>(cd) : 0; };
        const int c = csel == 0u ? static_cast<int>(as) : csel == 1u ? static_cast<int>(ad) : fix;
        const int v = ((pick(asel) - pick(bsel)) * c >> 7) + pick(dsel);
        return clampU8(v);
    }

    bool ffBiasApplies(const GsPlan &p, uint32_t cs, uint32_t as)
    {
        if (p.term != kTermLinear)
            return false;
        if (p.blend == kBlendMixSub)
            return true;
        const uint32_t f = p.factor == kFactorAs ? as : p.fix;
        // a = cs * n / 128 levels; the bias (127/256) applies where a >= 127/256, i.e. 2 * cs * n >= 127.
        return 2u * cs * coefTimes128(p.ks, f) >= 127u;
    }

    Ff ffBlend(const GsPlan &p, uint32_t cs, uint32_t cd, uint32_t as, bool bias)
    {
        if (p.term == kTermExact)
        {
            const int part = gsSourcePart(static_cast<uint64_t>(p.abcd) | static_cast<uint64_t>(p.fix) << 32, static_cast<int>(cs), static_cast<int>(as));
            int v;
            if (p.blend == kBlendOff)
                v = part;
            else if (p.blend == kBlendMix)
                v = part + static_cast<int>(cd) * (p.kd == kCoefOne ? 1 : 0);
            else
                v = static_cast<int>(cd) * (p.kd == kCoefOne ? 1 : 0) - (-part);
            const uint32_t c = clampU8(v);
            return {c, c};
        }
        // Linear: x * 128 = |ks|128 * cs (+ or -) kd128 * cd, a multiple of 1/128 level.
        const uint32_t f = p.factor == kFactorAs ? as : p.fix;
        const int64_t s128 = static_cast<int64_t>(cs) * coefTimes128(p.ks, f);
        const int64_t d128 = static_cast<int64_t>(cd) * coefTimes128(p.kd, f);
        const int64_t x128 = p.blend == kBlendMixSub ? d128 - s128 : d128 + s128;
        // The blender stores round(x - b) with the bias, round(x) without; x - b never ties (4.3).
        auto clamp = [](int64_t v) { return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
        const int64_t fl = x128 >= 0 ? x128 / 128 : -((-x128 + 127) / 128);
        const int64_t frac = x128 - fl * 128; // 0..127
        if (bias && ffBiasApplies(p, cs, as))
            return {clamp(fl), clamp(fl)};
        if (frac > 64)
            return {clamp(fl + 1), clamp(fl + 1)};
        if (frac < 64)
            return {clamp(fl), clamp(fl)};
        return {clamp(fl), clamp(fl + 1)};
    }

    Rgba gsPixel(const GsState &s, Rgba src, Rgba dst, uint32_t z, uint32_t &storedZ, bool *written)
    {
        if (written)
            *written = false;
        const Write w = gsAlphaWrite(s, src.a);
        if (!w.rgb && !w.a && !w.z)
            return dst;
        if (!gsDepthPass(static_cast<uint32_t>((s.test >> 17) & 3u), z, storedZ))
            return dst;
        Rgba out = dst;
        if (w.rgb || w.a)
        {
            Rgba c = src;
            if (s.abe() && !((s.pabe & 1u) && (src.a & 0x80u) == 0u))
            {
                c.r = static_cast<uint8_t>(gsBlend(s.alpha, src.r, dst.r, src.a, dst.a));
                c.g = static_cast<uint8_t>(gsBlend(s.alpha, src.g, dst.g, src.a, dst.a));
                c.b = static_cast<uint8_t>(gsBlend(s.alpha, src.b, dst.b, src.a, dst.a));
            }
            if (w.a && (s.fba & 1u))
                c.a = static_cast<uint8_t>(c.a | 0x80u);
            uint32_t px = c.packed();
            const uint32_t fbmsk = static_cast<uint32_t>(s.frame >> 32);
            px = (px & ~fbmsk) | (dst.packed() & fbmsk);
            if (!w.a)
                px = (px & 0x00FFFFFFu) | (dst.packed() & 0xFF000000u);
            out = Rgba::unpack(px);
        }
        if (w.z)
            storedZ = z & 0xFFFFFFu; // Z24
        if (written)
            *written = true;
        return out;
    }

    bool gsTriSetup(const GsVertex v[3], uint32_t ofx, uint32_t ofy, GsTri &t)
    {
        for (int i = 0; i < 3; ++i)
        {
            t.x[i] = static_cast<int64_t>(v[i].x) - static_cast<int64_t>(ofx);
            t.y[i] = static_cast<int64_t>(v[i].y) - static_cast<int64_t>(ofy);
        }
        for (int i = 0; i < 3; ++i)
        {
            const int p = (i + 1) % 3, q = (i + 2) % 3;
            t.a[i] = -(t.y[q] - t.y[p]);
            t.b[i] = t.x[q] - t.x[p];
            t.c[i] = -(t.a[i] * t.x[p] + t.b[i] * t.y[p]);
        }
        t.area = t.a[0] * t.x[0] + t.b[0] * t.y[0] + t.c[0];
        if (t.area == 0)
            return false;
        if (t.area < 0)
        {
            t.area = -t.area;
            for (int i = 0; i < 3; ++i)
            {
                t.a[i] = -t.a[i];
                t.b[i] = -t.b[i];
                t.c[i] = -t.c[i];
            }
        }
        for (int i = 0; i < 3; ++i)
            t.bias[i] = (t.a[i] > 0 || (t.a[i] == 0 && t.b[i] > 0)) ? 1 : 0;
        t.invArea = 1.0 / static_cast<double>(t.area);
        auto floorDiv16 = [](int64_t q) { return q >= 0 ? q / 16 : -((-q + 15) / 16); };
        const int64_t minX = std::min({t.x[0], t.x[1], t.x[2]}), maxX = std::max({t.x[0], t.x[1], t.x[2]});
        const int64_t minY = std::min({t.y[0], t.y[1], t.y[2]}), maxY = std::max({t.y[0], t.y[1], t.y[2]});
        t.minX = static_cast<int>(-floorDiv16(-minX));
        t.maxX = static_cast<int>(floorDiv16(maxX));
        t.minY = static_cast<int>(-floorDiv16(-minY));
        t.maxY = static_cast<int>(floorDiv16(maxY));
        return true;
    }

    GsRect coverSprite(const GsVertex v[2], uint32_t ofx, uint32_t ofy)
    {
        // Integer arithmetic in 1/16: first = ceil(min / 16), last = ceil(max / 16) - 1 (the float version is exact for
        // these values).
        auto ceil16 = [](int64_t q) { return q >= 0 ? (q + 15) / 16 : -((-q) / 16); };
        const int64_t x0 = static_cast<int64_t>(v[0].x) - ofx, x1 = static_cast<int64_t>(v[1].x) - ofx;
        const int64_t y0 = static_cast<int64_t>(v[0].y) - ofy, y1 = static_cast<int64_t>(v[1].y) - ofy;
        GsRect r;
        r.x0 = static_cast<int>(ceil16(std::min(x0, x1)));
        r.x1 = static_cast<int>(ceil16(std::max(x0, x1))) - 1;
        r.y0 = static_cast<int>(ceil16(std::min(y0, y1)));
        r.y1 = static_cast<int>(ceil16(std::max(y0, y1))) - 1;
        return r;
    }

    void expandLine(const GsVertex v[2], bool iip, uint32_t ofx, uint32_t ofy, std::vector<GsLinePixel> &out)
    {
        // GSCpuBackend::DrawLine's expressions: the end points truncated (X / 16 as a float, then int), Bresenham
        // with both ends, t = step / total in float.
        const int ox = static_cast<int>(ofx >> 4), oy = static_cast<int>(ofy >> 4);
        int x0 = static_cast<int>(static_cast<float>(v[0].x) / 16.0f) - ox;
        int y0 = static_cast<int>(static_cast<float>(v[0].y) / 16.0f) - oy;
        const int x1 = static_cast<int>(static_cast<float>(v[1].x) / 16.0f) - ox;
        const int y1 = static_cast<int>(static_cast<float>(v[1].y) / 16.0f) - oy;
        const double z0 = gsVertexZ(v[0].z), z1 = gsVertexZ(v[1].z);
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        int total = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        if (total == 0)
            total = 1;
        int step = 0;
        for (;;)
        {
            const float t = static_cast<float>(step) / static_cast<float>(total);
            GsLinePixel p;
            p.x = x0;
            p.y = y0;
            if (iip)
            {
                p.c.r = clampU8(static_cast<int>(v[0].r + (v[1].r - v[0].r) * t));
                p.c.g = clampU8(static_cast<int>(v[0].g + (v[1].g - v[0].g) * t));
                p.c.b = clampU8(static_cast<int>(v[0].b + (v[1].b - v[0].b) * t));
                p.c.a = clampU8(static_cast<int>(v[0].a + (v[1].a - v[0].a) * t));
            }
            else
                p.c = {v[1].r, v[1].g, v[1].b, v[1].a};
            const double z = z0 + (z1 - z0) * t;
            p.z = static_cast<uint32_t>(z);
            out.push_back(p);
            if (x0 == x1 && y0 == y1)
                break;
            const int e2 = 2 * err;
            if (e2 >= dy)
            {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx)
            {
                err += dx;
                y0 += sy;
            }
            ++step;
        }
    }

    int gsWrap(int coord, int size, uint32_t mode, uint32_t minC, uint32_t maxC)
    {
        switch (mode & 3u)
        {
        case 0: return static_cast<int>(static_cast<uint32_t>(coord) & static_cast<uint32_t>(size - 1));
        case 1: return coord < 0 ? 0 : coord > size - 1 ? size - 1 : coord;
        case 2: return std::min(std::max(coord, static_cast<int>(minC)), static_cast<int>(maxC));
        default: return static_cast<int>((static_cast<uint32_t>(coord) & minC) | maxC);
        }
    }

    Rgba gsBilinear(Rgba c00, Rgba c10, Rgba c01, Rgba c11, float fu, float fv)
    {
        auto ch = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        {
            const float top = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * fu;
            const float bot = static_cast<float>(c) + (static_cast<float>(d) - static_cast<float>(c)) * fu;
            return clampU8(roundHalfUp(top + (bot - top) * fv));
        };
        return {ch(c00.r, c10.r, c01.r, c11.r), ch(c00.g, c10.g, c01.g, c11.g), ch(c00.b, c10.b, c01.b, c11.b), ch(c00.a, c10.a, c01.a, c11.a)};
    }

    void gsTriPixel(const GsTri &t, const GsVertex v[3], const GsState &s, int px, int py, GsTriPixel &o)
    {
        int64_t e[3];
        for (int i = 0; i < 3; ++i)
        {
            e[i] = gsTriEdge(t, i, px, py);
            o.w[i] = gsTriWeight(t, e[i]);
        }
        const float w0 = o.w[0], w1 = o.w[1], w2 = o.w[2];
        const double z0 = gsVertexZ(v[0].z), z1 = gsVertexZ(v[1].z), z2 = gsVertexZ(v[2].z);
        const double z = z0 + (z1 - z0) * static_cast<double>(w1) + (z2 - z0) * static_cast<double>(w2);
        o.z = static_cast<uint32_t>(std::clamp(z + 0.5, 0.0, 4294967295.0));
        if (s.iip())
        {
            o.vertex.r = clampU8(static_cast<int>(v[0].r * w0 + v[1].r * w1 + v[2].r * w2));
            o.vertex.g = clampU8(static_cast<int>(v[0].g * w0 + v[1].g * w1 + v[2].g * w2));
            o.vertex.b = clampU8(static_cast<int>(v[0].b * w0 + v[1].b * w1 + v[2].b * w2));
            o.vertex.a = clampU8(static_cast<int>(v[0].a * w0 + v[1].a * w1 + v[2].a * w2));
        }
        else
            o.vertex = {v[2].r, v[2].g, v[2].b, v[2].a};
        if (!s.tme())
            return;
        const int texW = 1 << std::min<uint32_t>((s.tex0 >> 26) & 0xFu, 10u);
        const int texH = 1 << std::min<uint32_t>((s.tex0 >> 30) & 0xFu, 10u);
        if (s.fst())
        {
            const uint16_t iu = static_cast<uint16_t>(v[0].u * w0 + v[1].u * w1 + v[2].u * w2);
            const uint16_t iv = static_cast<uint16_t>(v[0].v * w0 + v[1].v * w1 + v[2].v * w2);
            o.uf = static_cast<float>(iu) / 16.0f;
            o.vf = static_cast<float>(iv) / 16.0f;
        }
        else
        {
            const float is = v[0].s * w0 + v[1].s * w1 + v[2].s * w2;
            const float it = v[0].t * w0 + v[1].t * w1 + v[2].t * w2;
            const float iq = v[0].q * w0 + v[1].q * w1 + v[2].q * w2;
            o.invQ = 1.0f / (std::fabs(iq) > 1.0e-8f ? iq : 1.0f);
            o.uf = is * o.invQ * static_cast<float>(texW);
            o.vf = it * o.invQ * static_cast<float>(texH);
        }
        const uint32_t mmag = (s.tex1 >> 5) & 1u, mmin = (s.tex1 >> 6) & 7u;
        const bool linear = mmag != 0u || mmin == 1u || (mmin & 4u) != 0u;
        if (linear)
        {
            const float su = o.uf - 0.5f, sv = o.vf - 0.5f;
            o.iu = static_cast<int>(std::floor(su));
            o.iv = static_cast<int>(std::floor(sv));
            o.fu = su - static_cast<float>(o.iu);
            o.fv = sv - static_cast<float>(o.iv);
        }
        else
        {
            o.iu = static_cast<int>(o.uf);
            o.iv = static_cast<int>(o.vf);
        }
    }

    GsModel::GsModel()
    {
        colour[0].assign(kWidth * kHeight, 0u);
        colour[1].assign(kWidth * kHeight, 0u);
        depth.assign(kWidth * kHeight, 0u);
    }

    Rgba GsModel::sample(const GsTriPixel &p) const
    {
        if (!m_tex || m_tex->rgba.empty())
            return {};
        const int w = static_cast<int>(m_tex->width), h = static_cast<int>(m_tex->height);
        const uint64_t cl = m_state.clamp;
        const uint32_t wu = cl & 3u, wv = (cl >> 2) & 3u;
        const uint32_t minU = (cl >> 4) & 0x3FFu, maxU = (cl >> 14) & 0x3FFu, minV = (cl >> 24) & 0x3FFu, maxV = (cl >> 34) & 0x3FFu;
        auto texel = [&](int u, int v)
        {
            Rgba c;
            if (u < 0 || v < 0 || u >= w || v >= h)
                return c; // outside the decoded level (a region mode can reach past it): the reference reads memory there
            std::memcpy(&c, m_tex->rgba.data() + (static_cast<size_t>(v) * w + u) * 4u, 4);
            return c;
        };
        if (!m_plan.linear)
            return texel(gsWrap(p.iu, w, wu, minU, maxU), gsWrap(p.iv, h, wv, minV, maxV));
        const int u0 = gsWrap(p.iu, w, wu, minU, maxU), u1 = gsWrap(p.iu + 1, w, wu, minU, maxU);
        const int v0 = gsWrap(p.iv, h, wv, minV, maxV), v1 = gsWrap(p.iv + 1, h, wv, minV, maxV);
        return gsBilinear(texel(u0, v0), texel(u1, v0), texel(u0, v1), texel(u1, v1), p.fu, p.fv);
    }

    void GsModel::shade(int x, int y, Rgba source, uint32_t z)
    {
        const uint32_t sx0 = m_state.scissor & 0x7FFu, sx1 = (m_state.scissor >> 16) & 0x7FFu;
        const uint32_t sy0 = (m_state.scissor >> 32) & 0x7FFu, sy1 = (m_state.scissor >> 48) & 0x7FFu;
        if (x < static_cast<int>(sx0) || x > static_cast<int>(sx1) || y < static_cast<int>(sy0) || y > static_cast<int>(sy1))
            return;
        if (x < 0 || y < 0 || x >= static_cast<int>(kWidth) || y >= static_cast<int>(kHeight))
            return;
        const size_t i = static_cast<size_t>(y) * kWidth + x;
        uint32_t &c = colour[m_plan.target][i];
        bool written = false;
        c = gsPixel(m_state, source, Rgba::unpack(c), z, depth[i], &written).packed();
        stats.pixels += written;
    }

    void GsModel::draw(const DrawList &list)
    {
        for (const DrawCmd &cmd : list.cmds)
        {
            switch (cmd.op)
            {
            case kOpTexture:
            {
                Texture &t = m_textures[cmd.a];
                t.width = cmd.c & 0xFFFFu;
                t.height = cmd.c >> 16;
                t.rgba.assign(list.data(cmd), list.data(cmd) + cmd.payloadBytes);
                ++stats.textures;
                break;
            }
            case kOpState:
            {
                m_state = list.state(cmd);
                m_plan = planState(m_state);
                m_haveState = true;
                ++stats.states;
                m_tex = nullptr;
                if (m_state.tme())
                {
                    auto it = m_textures.find(m_state.texture);
                    m_tex = it == m_textures.end() ? nullptr : &it->second;
                }
                if (m_plan.unsupported || (cmd.a & 1u))
                    ++stats.unsupported;
                break;
            }
            case kOpClear:
            {
                if (!m_haveState || m_plan.unsupported)
                    break;
                const GsVertex *v = list.vertices(cmd);
                const GsRect r = coverSprite(v, m_state.xyoffset & 0xFFFFu, (m_state.xyoffset >> 32) & 0xFFFFu);
                const Rgba c{v[1].r, v[1].g, v[1].b, v[1].a};
                for (int y = r.y0; y <= r.y1; ++y)
                    for (int x = r.x0; x <= r.x1; ++x)
                        shade(x, y, c, static_cast<uint32_t>(gsVertexZ(v[1].z)));
                ++stats.clears;
                break;
            }
            case kOpScreenPrims:
            {
                if (!m_haveState || m_plan.unsupported || (cmd.a != kKindTriangles && cmd.a != kKindLines && cmd.a != kKindPoints))
                    break;
                const GsVertex *v = list.vertices(cmd);
                const uint32_t ofx = m_state.xyoffset & 0xFFFFu, ofy = (m_state.xyoffset >> 32) & 0xFFFFu;
                for (uint32_t p = 0; p < cmd.b; ++p)
                {
                    ++stats.prims;
                    if (cmd.a == kKindPoints)
                    {
                        const GsVertex &q = v[p];
                        const int x = static_cast<int>(static_cast<float>(q.x) / 16.0f) - static_cast<int>(ofx >> 4);
                        const int y = static_cast<int>(static_cast<float>(q.y) / 16.0f) - static_cast<int>(ofy >> 4);
                        shade(x, y, {q.r, q.g, q.b, q.a}, static_cast<uint32_t>(gsVertexZ(q.z)));
                        continue;
                    }
                    if (cmd.a == kKindLines)
                    {
                        std::vector<GsLinePixel> px;
                        expandLine(v + p * 2u, m_state.iip(), ofx, ofy, px);
                        for (const GsLinePixel &l : px)
                            shade(l.x, l.y, l.c, l.z);
                        stats.linePixels += px.size();
                        continue;
                    }
                    const GsVertex *tv = v + p * 3u;
                    GsTri t;
                    if (!gsTriSetup(tv, ofx, ofy, t))
                        continue;
                    const int sx0 = static_cast<int>(m_state.scissor & 0x7FFu), sx1 = static_cast<int>((m_state.scissor >> 16) & 0x7FFu);
                    const int sy0 = static_cast<int>((m_state.scissor >> 32) & 0x7FFu), sy1 = static_cast<int>((m_state.scissor >> 48) & 0x7FFu);
                    const int x0 = std::max(t.minX, sx0), x1 = std::min(t.maxX, sx1), y0 = std::max(t.minY, sy0), y1 = std::min(t.maxY, sy1);
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                        {
                            int64_t e[3] = {gsTriEdge(t, 0, x, y), gsTriEdge(t, 1, x, y), gsTriEdge(t, 2, x, y)};
                            if (!gsTriCovers(t, e))
                                continue;
                            GsTriPixel q;
                            gsTriPixel(t, tv, m_state, x, y, q);
                            Rgba src = q.vertex;
                            if (m_state.tme())
                                src = gsTextureFunction(m_plan.tfx, m_plan.tcc, sample(q), q.vertex);
                            shade(x, y, src, q.z);
                        }
                }
                break;
            }
            default:
                break;
            }
        }
    }
}
