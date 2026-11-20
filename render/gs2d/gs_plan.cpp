#include "gs_plan.h"

#include <algorithm>

namespace rc::rd
{
    const char *gsGroupName(uint32_t bit)
    {
        static const char *const names[kGsGroupCount] = {"FRAME", "ZBUF", "SCISSOR", "XYOFFSET", "TEST", "ALPHA", "TEX0", "TEX1",
                                                         "CLAMP", "MIPTBP", "TEXA", "PRIM/PRMODECONT", "PABE", "FBA", "COLCLAMP", "DTHE"};
        return bit < kGsGroupCount ? names[bit] : "?";
    }

    const char *gsGuardName(uint32_t guard)
    {
        static const char *const names[kGuardCount] = {"none", "PABE_BLEND", "DEST_ALPHA", "FRAME", "ZBUF", "FBA", "DTHE", "COLCLAMP",
                                                       "FGE", "AA1", "CTXT", "BLEND_FORM", "MIPMAP", "TEX_FORMAT", "ZTE"};
        return guard < kGuardCount ? names[guard] : "?";
    }

    int gsSourcePart(uint64_t alpha, int cs, int as)
    {
        const uint32_t asel = alpha & 3u, bsel = (alpha >> 2) & 3u, csel = (alpha >> 4) & 3u, dsel = (alpha >> 6) & 3u;
        const int fix = static_cast<int>((alpha >> 32) & 0xFFu);
        const auto pick = [cs](uint32_t sel) { return sel == 0u ? cs : 0; }; // Cd = 0
        const int c = csel == 0u ? as : csel == 2u ? fix : 0;            // Ad is not a source quantity
        return ((pick(asel) - pick(bsel)) * c >> 7) + pick(dsel);
    }

    namespace
    {
        // A coefficient c0 + m * F as a GsCoef; with a constant F of 0 or 0x80 it is an integer.
        uint8_t coefOf(int c0, int m, bool fConst, uint32_t f)
        {
            if (fConst && (f == 0u || f == 0x80u))
            {
                const int v = c0 + (f ? m : 0);
                return v == 0 ? kCoefZero : v == 1 ? kCoefOne : v == 2 ? kCoefTwo : kCoefMinusOne;
            }
            if (m == 0)
                return c0 ? kCoefOne : kCoefZero;
            if (m > 0)
                return c0 ? kCoefOnePlusF : kCoefF;
            return c0 ? kCoefOneMinusF : kCoefMinusF;
        }
        bool nonNegative(uint8_t c) { return c != kCoefMinusF && c != kCoefMinusOne; }
        bool nonPositive(uint8_t c) { return c == kCoefZero || c == kCoefMinusF || c == kCoefMinusOne; }
    }

    GsPlan planState(const GsState &s)
    {
        GsPlan p{};
        auto guard = [&p](GsGuard g, bool drop = false)
        {
            p.guards = static_cast<uint16_t>(p.guards | (1u << g));
            if (drop && !p.unsupported)
                p.unsupported = g;
        };

        // Frame and Z buffers.
        const uint32_t fbp = static_cast<uint32_t>(s.frame & 0x1FFu), fbw = static_cast<uint32_t>((s.frame >> 16) & 0x3Fu);
        const uint32_t fpsm = static_cast<uint32_t>((s.frame >> 24) & 0x3Fu), fbmsk = static_cast<uint32_t>(s.frame >> 32);
        uint8_t colour = kMaskRgba;
        if (fbmsk == 0xFF000000u)
            colour = kMaskRgb;
        else if (fbmsk == 0x00FFFFFFu)
            colour = kMaskA;
        else if (fbmsk != 0u)
            guard(kGuardFrame, true);
        if (fpsm != 0u || fbw != 10u || (fbp != 0u && fbp != 0x50u))
            guard(kGuardFrame, true);
        p.target = fbp == 0x50u ? 1u : 0u;
        const uint32_t zbp = static_cast<uint32_t>(s.zbuf & 0x1FFu), zpsm = static_cast<uint32_t>((s.zbuf >> 24) & 0xFu);
        const bool zmsk = (s.zbuf >> 32) & 1u;
        if (zbp != 0xA0u || (zpsm != 0u && zpsm != 1u))
            guard(kGuardZbuf, true);

        // Tests.
        const bool ate = s.test & 1u;
        uint8_t atst = ate ? static_cast<uint8_t>((s.test >> 1) & 7u) : static_cast<uint8_t>(kAtstAlways);
        const uint8_t aref = static_cast<uint8_t>((s.test >> 4) & 0xFFu), afail = static_cast<uint8_t>((s.test >> 12) & 3u);
        if ((s.test >> 14) & 1u)
            guard(kGuardDestAlpha);
        if (!((s.test >> 16) & 1u))
            guard(kGuardZte);
        p.depthCompare = static_cast<uint8_t>((s.test >> 17) & 3u);
        p.passColorMask = colour;
        p.passDepthWrite = zmsk ? 0u : 1u;
        switch (afail)
        {
        case 0: // KEEP
            p.failColorMask = 0;
            p.failDepthWrite = 0;
            break;
        case 1: // FB_ONLY
            p.failColorMask = colour;
            p.failDepthWrite = 0;
            break;
        case 2: // ZB_ONLY
            p.failColorMask = 0;
            p.failDepthWrite = p.passDepthWrite;
            break;
        default: // RGB_ONLY (CT32 frame)
            p.failColorMask = static_cast<uint8_t>(colour & kMaskRgb);
            p.failDepthWrite = 0;
            break;
        }
        if (atst == kAtstNever)
        {
            // Every pixel fails: it writes what AFAIL says, unconditionally.
            p.passColorMask = p.failColorMask;
            p.passDepthWrite = p.failDepthWrite;
            atst = kAtstAlways;
        }
        if (atst == kAtstAlways || (p.failColorMask == p.passColorMask && p.failDepthWrite == p.passDepthWrite))
        {
            atst = kAtstAlways; // the test cannot change what is written
            p.failColorMask = p.passColorMask;
            p.failDepthWrite = p.passDepthWrite;
        }
        p.atst = atst;
        p.aref = aref;
        const bool failWrites = p.failColorMask || p.failDepthWrite;
        p.alphaByBlend = atst != kAtstAlways && afail == 3u && !p.passDepthWrite && p.failColorMask == (p.passColorMask & kMaskRgb) ? 1u : 0u;
        p.split = atst != kAtstAlways && failWrites && !p.alphaByBlend ? 1u : 0u;

        // The features the renderer leaves out.
        if (s.fba & 1u)
            guard(kGuardFba);
        if (s.dthe & 1u)
            guard(kGuardDthe);
        if (!(s.colclamp & 1u))
            guard(kGuardColclamp);
        if (s.fge())
            guard(kGuardFge);
        if (s.aa1())
            guard(kGuardAa1);
        if (s.ctxt())
            guard(kGuardCtxt);

        // Blend (4.2).
        p.blend = kBlendOff;
        p.term = kTermExact;
        p.ks = kCoefOne;
        p.kd = kCoefZero;
        p.abcd = 0;
        p.exactBlend = 1;
        if (s.abe())
        {
            const uint32_t asel = s.alpha & 3u, bsel = (s.alpha >> 2) & 3u, dsel = (s.alpha >> 6) & 3u;
            uint32_t csel = (s.alpha >> 4) & 3u;
            uint32_t fix = static_cast<uint32_t>((s.alpha >> 32) & 0xFFu);
            const bool pabePre = ate && (atst == kAtstLess || atst == kAtstLequal) && aref < 0x80u && afail == 0u;
            if ((s.pabe & 1u) && !pabePre)
                guard(kGuardPabeBlend);
            if (!((s.pabe & 1u) && pabePre))
            {
                if (csel == 1u)
                {
                    guard(kGuardDestAlpha);
                    csel = 2u;
                    fix = 0x80u;
                }
                if (csel == 3u) // reserved; the reference reads it as FIX
                    csel = 2u;
                const bool fConst = csel == 2u;
                const auto is = [](uint32_t sel, uint32_t what) { return sel == what ? 1 : 0; };
                const uint8_t ks = coefOf(is(dsel, 0), is(asel, 0) - is(bsel, 0), fConst, fix);
                const uint8_t kd = coefOf(is(dsel, 1), is(asel, 1) - is(bsel, 1), fConst, fix);
                p.ks = ks;
                p.kd = kd;
                p.factor = fConst ? kFactorFix : kFactorAs;
                p.fix = static_cast<uint8_t>(fix);
                p.abcd = static_cast<uint8_t>(asel | bsel << 2 | csel << 4 | dsel << 6);
                if (kd == kCoefZero)
                    p.blend = kBlendOff; // a function of Cs alone: the shader evaluates the GS rule
                else if (kd == kCoefOne)
                {
                    // Cd outside the floor (or F 0 / 0x80): x = P(Cs) + Cd exactly.
                    if (nonNegative(ks))
                        p.blend = kBlendMix;
                    else if (nonPositive(ks))
                        p.blend = kBlendMixSub;
                    else
                    {
                        guard(kGuardBlendForm);
                        p.blend = kBlendOff;
                    }
                }
                else if (kd == kCoefF || kd == kCoefOneMinusF)
                {
                    p.term = kTermLinear;
                    p.exactBlend = 0;
                    if (nonNegative(ks))
                        p.blend = kBlendMix;
                    else
                        p.blend = kBlendMixSub; // ks = -F: the bias goes on the subtracted source, always available
                }
                else
                {
                    // 1+F, -F, 2, -1: not expressible with the three configurations. Drawn as its source part.
                    guard(kGuardBlendForm);
                    p.blend = kBlendOff;
                    p.kd = kCoefZero;
                }
            }
        }

        // Texture.
        if (s.tme())
        {
            p.tfx = static_cast<uint8_t>((s.tex0 >> 35) & 3u);
            p.tcc = static_cast<uint8_t>((s.tex0 >> 34) & 1u);
            const uint32_t mmag = (s.tex1 >> 5) & 1u, mmin = (s.tex1 >> 6) & 7u;
            p.linear = (mmag != 0u || mmin == 1u || (mmin & 4u) != 0u) ? 1u : 0u;
            p.wrapU = static_cast<uint8_t>(s.clamp & 3u);
            p.wrapV = static_cast<uint8_t>((s.clamp >> 2) & 3u);
            if ((s.tex1 >> 2) & 7u)
                guard(kGuardMipmap);
            if (s.texture == 0xFFFFFFFFu)
                guard(kGuardTexFormat, true);
        }
        return p;
    }
}
