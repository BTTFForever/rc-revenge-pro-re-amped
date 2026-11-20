#version 450
// render/gs2d (renderer plan P1 step 7; RENDERER_2D_DESIGN.md sections 4-7): the software GS's per-pixel arithmetic
// (runtime gs_cpu_raster.cpp DrawTriangleFast, gs_triangle_rules.h, gs_cpu_raster_inline.h) for one primitive record,
// in the reference's operation order: coverage and weights from the edge functions in doubles, Z, colour, texture
// coordinates, bilinear filter, texture function, alpha test, the source term of the fixed-function blend, depth.
// Every float result is `precise` (no contraction), as the reference is built with -ffp-contract=off.
// Records: Gs2dPrim (gs2d_vk.h), 44 words. Bind group 1 slot 0: the records (binding 16); group 2: the texture
// (slot 1, binding 17) and a sampler (slot 1, binding 33; texelFetch only, no filtering by the GPU).
// GS2D_PROBE (test T7): no alpha test or blend; writes the bits of the weights and 1/Q, or of uf, vf, fu, fv.
// GS2D_DSTREAD (batch 4b, RENDERER_2D_DESIGN.md 18): the exact blend. The destination is read as an input attachment
// (colour attachment 0 under dynamic rendering local read; bind group 0 slot 0, binding 16) and the GS rule is computed
// in integers; one colour output, the pipeline's blend disabled. Gs2dVk issues a by-region barrier before each group of
// primitives that cannot overlap, so the read sees every earlier write.
layout(std430, set = 1, binding = 16) readonly buffer Prims { uint w[]; } prims;
layout(set = 2, binding = 17) uniform texture2D uTex;
layout(set = 2, binding = 33) uniform sampler uSampler;
layout(location = 0) flat in uint vPrim;
#ifdef GS2D_PROBE
layout(location = 0) out uvec4 outProbe;
#elif defined(GS2D_DSTREAD)
layout(input_attachment_index = 0, set = 0, binding = 16) uniform subpassInput uDst;
layout(location = 0) out vec4 out0;
#else
layout(location = 0, index = 0) out vec4 out0;
layout(location = 0, index = 1) out vec4 out1;
#endif

uint W(uint i) { return prims.w[vPrim * 44u + i]; }
int Wi(uint i) { return int(W(i)); }
float Wf(uint i) { return uintBitsToFloat(W(i)); }
uint ch(uint c, uint k) { return (c >> (8u * k)) & 0xFFu; }

// The reference's round half up of the bilinear value (ExactLround on values in 0..255).
int roundHalfUp(precise float v)
{
    int t = int(v);
    precise float f = v - float(t);
    if (v >= 0.0)
    {
        if (f >= 0.5)
            t += 1;
    }
    else if (f <= -0.5)
        t -= 1;
    return t;
}

int wrapCoord(int c, int size, uint mode, int mn, int mx)
{
    if (mode == 0u)
        return int(uint(c) & uint(size - 1));
    if (mode == 1u)
        return clamp(c, 0, size - 1);
    if (mode == 2u)
        return min(max(c, mn), mx);
    return int((uint(c) & uint(mn)) | uint(mx));
}

uvec4 texel(int u, int v)
{
    if ((W(40u) & 4u) != 0u) // planted fault: the fetch one row down
        v += 1;
    const vec4 t = texelFetch(sampler2D(uTex, uSampler), ivec2(u, v), 0);
    return uvec4(t * 255.0 + 0.5);
}

uint lerpChannel(uint c00, uint c10, uint c01, uint c11, float fu, float fv)
{
    precise float top = float(c00) + (float(c10) - float(c00)) * fu;
    precise float bot = float(c01) + (float(c11) - float(c01)) * fu;
    precise float v = top + (bot - top) * fv;
    return uint(clamp(roundHalfUp(v), 0, 255));
}

bool alphaPass(uint atst, uint aref, uint as)
{
    switch (atst)
    {
    case 0u: return false;
    case 1u: return true;
    case 2u: return as < aref;
    case 3u: return as <= aref;
    case 4u: return as == aref;
    case 5u: return as >= aref;
    case 6u: return as > aref;
    default: return as != aref;
    }
}

// ALPHA's source-only part ((A - B) * C >> 7) + D with Cd = 0 (gs_plan.cpp gsSourcePart).
int sourcePart(uint abcd, uint fix, int cs, int as)
{
    const uint asel = abcd & 3u, bsel = (abcd >> 2) & 3u, csel = (abcd >> 4) & 3u, dsel = (abcd >> 6) & 3u;
    const int a = asel == 0u ? cs : 0;
    const int b = bsel == 0u ? cs : 0;
    const int d = dsel == 0u ? cs : 0;
    const int c = csel == 0u ? as : csel == 2u ? int(fix) : 0;
    return ((a - b) * c >> 7) + d;
}

// A coefficient (GsCoef) times 128 for F: 0, 128, F, 128 - F, F (-F), ...
uint coef128(uint coef, uint f)
{
    switch (coef)
    {
    case 0u: return 0u;
    case 1u: return 128u;
    case 2u: return f;
    case 3u: return 128u - f;
    case 4u: return f;
    case 7u: return 128u;
    default: return 0u;
    }
}

void main()
{
    const uint flags = W(35u);
    const ivec2 px = ivec2(gl_FragCoord.xy);
    uvec4 vc;   // vertex colour (interpolated or flat)
    uint zu;
    precise float w0 = 0.0, w1 = 0.0, w2 = 0.0, invQ = 0.0, uf = 0.0, vf = 0.0, fu = 0.0, fv = 0.0;
    int iu = 0, iv = 0;
    const uint test = W(37u);
    const bool tme = (flags & 4u) != 0u;
    const bool linear = ((test >> 15) & 1u) != 0u;
    if ((flags & 1u) != 0u)
    {
        const uint c = W(8u);
        vc = uvec4(ch(c, 0u), ch(c, 1u), ch(c, 2u), ch(c, 3u));
        zu = W(20u);
    }
    else
    {
        // 6.2: e_i = a_i * 16x + b_i * 16y + c_i, exact in doubles; the reference's coverage test; w_i = float(e_i / area).
        const double invArea = packDouble2x32(uvec2(W(21u), W(22u)));
        double e[3];
        precise float wv[3];
        for (uint i = 0u; i < 3u; ++i)
        {
            const double c = packDouble2x32(uvec2(W(29u + 2u * i), W(30u + 2u * i)));
            precise double ei = double(Wi(23u + i)) * double(px.x * 16) + double(Wi(26u + i)) * double(px.y * 16) + c;
            e[i] = ei;
            const double bias = double((flags >> (8u + i)) & 1u);
            if (!(ei + bias > 0.0lf))
                discard;
            precise double we = ei * invArea;
            wv[i] = float(we);
        }
        w0 = wv[0];
        w1 = wv[1];
        w2 = wv[2];
        // Z: z0 + (z1 - z0) * w1 + (z2 - z0) * w2 in doubles, the vertex Z a float of the integer.
        const double z0 = double(float(W(18u))), z1 = double(float(W(19u))), z2 = double(float(W(20u)));
        precise double zd = z0 + (z1 - z0) * double(w1) + (z2 - z0) * double(w2);
        zu = uint(clamp(zd + 0.5lf, 0.0lf, 4294967295.0lf));
        const uint c0 = W(6u), c1 = W(7u), c2 = W(8u);
        if ((flags & 2u) != 0u)
        {
            for (uint k = 0u; k < 4u; ++k)
            {
                precise float v = float(ch(c0, k)) * w0 + float(ch(c1, k)) * w1 + float(ch(c2, k)) * w2;
                vc[k] = uint(clamp(int(v), 0, 255));
            }
        }
        else
            vc = uvec4(ch(c2, 0u), ch(c2, 1u), ch(c2, 2u), ch(c2, 3u));
        if (tme)
        {
            const float texW = float(1u << ((test >> 20) & 15u)), texH = float(1u << ((test >> 24) & 15u));
            if ((flags & 8u) != 0u)
            {
                // FST (not used by RC's 2D packet): UV in 1/16 texel, interpolated and truncated to 16 bits.
                precise float iuf = float(W(9u) & 0xFFFFu) * w0 + float(W(10u) & 0xFFFFu) * w1 + float(W(11u) & 0xFFFFu) * w2;
                precise float ivf = float(W(9u) >> 16) * w0 + float(W(10u) >> 16) * w1 + float(W(11u) >> 16) * w2;
                uf = float(uint(iuf) & 0xFFFFu) / 16.0;
                vf = float(uint(ivf) & 0xFFFFu) / 16.0;
            }
            else
            {
                precise float is = Wf(9u) * w0 + Wf(10u) * w1 + Wf(11u) * w2;
                precise float it = Wf(12u) * w0 + Wf(13u) * w1 + Wf(14u) * w2;
                precise float iq = Wf(15u) * w0 + Wf(16u) * w1 + Wf(17u) * w2;
                const float q = abs(iq) > 1.0e-8 ? iq : 1.0;
                // The float division is not correctly rounded on the device (T7: 4% of pixels one ULP off on the RTX
                // 2060); a double division rounded to float is (6.3's fallback).
                invQ = float(1.0lf / double(q));
                precise float u0 = is * invQ;
                uf = u0 * texW;
                precise float v0 = it * invQ;
                vf = v0 * texH;
            }
            if (linear)
            {
                precise float su = uf - 0.5;
                precise float sv = vf - 0.5;
                iu = int(floor(su));
                iv = int(floor(sv));
                fu = su - float(iu);
                fv = sv - float(iv);
            }
            else
            {
                iu = int(uf);
                iv = int(vf);
            }
        }
    }

#ifdef GS2D_PROBE
    if ((flags & (1u << 11)) != 0u)
        outProbe = uvec4(floatBitsToUint(uf), floatBitsToUint(vf), floatBitsToUint(fu), floatBitsToUint(fv));
    else
        outProbe = uvec4(floatBitsToUint(w0), floatBitsToUint(w1), floatBitsToUint(w2), floatBitsToUint(invQ));
#else
    // Texel and texture function (6.3).
    uvec4 src = vc;
    if (tme)
    {
        const int sw = 1 << int((test >> 20) & 15u), sh = 1 << int((test >> 24) & 15u);
        const uint wu = (test >> 16) & 3u, wv = (test >> 18) & 3u;
        const int minU = int(W(38u) & 0xFFFFu), maxU = int(W(38u) >> 16), minV = int(W(39u) & 0xFFFFu), maxV = int(W(39u) >> 16);
        uvec4 t;
        if (linear)
        {
            const int u0 = wrapCoord(iu, sw, wu, minU, maxU), u1 = wrapCoord(iu + 1, sw, wu, minU, maxU);
            const int v0 = wrapCoord(iv, sh, wv, minV, maxV), v1 = wrapCoord(iv + 1, sh, wv, minV, maxV);
            const uvec4 c00 = texel(u0, v0), c10 = texel(u1, v0), c01 = texel(u0, v1), c11 = texel(u1, v1);
            for (uint k = 0u; k < 4u; ++k)
                t[k] = lerpChannel(c00[k], c10[k], c01[k], c11[k], fu, fv);
        }
        else
            t = texel(wrapCoord(iu, sw, wu, minU, maxU), wrapCoord(iv, sh, wv, minV, maxV));
        const uint tfx = (test >> 12) & 3u;
        const bool tcc = ((test >> 14) & 1u) != 0u;
        if (tfx == 1u)
            src = uvec4(t.rgb, tcc ? t.a : vc.a);
        else
        {
            src.rgb = min((t.rgb * vc.rgb) >> 7u, uvec3(255u));
            if (tfx != 0u)
                src.rgb = min(src.rgb + uvec3(vc.a), uvec3(255u));
            src.a = !tcc ? vc.a : tfx == 0u ? min((t.a * vc.a) >> 7u, 255u) : tfx == 2u ? min(t.a + vc.a, 255u) : t.a;
        }
    }
    const uint as = src.a;

    // Alpha test (5.2): the part this draw writes.
    const bool pass = alphaPass(test & 7u, (test >> 4) & 0xFFu, as);
    const uint part = (flags >> 4) & 3u;
    if (part == 0u && !pass)
        discard;
    if (part == 2u && pass)
        discard;

#ifdef GS2D_DSTREAD
    // The GS rule (4.1) on the destination as stored: Cv = clamp(((A - B) * C >> 7) + D, 0, 255) per colour channel, A,
    // B, D each Cs, Cd or 0, C As, Ad or FIX, the shift arithmetic (floor) on a signed int. The ALPHA selectors and FIX
    // are the record's (word 36: abcd 24-31, fix 16-23). Alpha is As, but for an alpha-by-blend part (RGB_ONLY without
    // a depth write) a failing pixel keeps the stored alpha.
    const ivec4 cd = ivec4(subpassLoad(uDst) * 255.0 + 0.5);
    const uint blendW = W(36u);
    const uint fixR = (blendW >> 16) & 0xFFu, abcdR = blendW >> 24;
    const uint asel = abcdR & 3u, bsel = (abcdR >> 2) & 3u, csel = (abcdR >> 4) & 3u, dsel = (abcdR >> 6) & 3u;
    const int cf = csel == 0u ? int(as) : csel == 1u ? cd.a : int(fixR);
    ivec3 cv;
    for (uint k = 0u; k < 3u; ++k)
    {
        const int cs = int(src[k]);
        const int va = asel == 0u ? cs : asel == 1u ? cd[k] : 0;
        const int vb = bsel == 0u ? cs : bsel == 1u ? cd[k] : 0;
        const int vd = dsel == 0u ? cs : dsel == 1u ? cd[k] : 0;
        cv[k] = clamp((((va - vb) * cf) >> 7) + vd, 0, 255);
    }
    const int outA = (part == 3u && !pass) ? cd.a : int(as);
    out0 = vec4(vec3(cv) / 255.0, float(outA) / 255.0);
#else
    // Source term and destination factor (4.2, 4.3).
    const uint blend = W(36u);
    const uint term = blend & 3u, factor = (blend >> 2) & 3u, ks = (blend >> 4) & 15u, kd = (blend >> 8) & 15u;
    const uint config = (blend >> 12) & 3u, fix = (blend >> 16) & 0xFFu, abcd = blend >> 24;
    const uint f = factor == 1u ? as : fix;
    vec3 a;
    vec3 d;
    if (term == 0u)
    {
        for (uint k = 0u; k < 3u; ++k)
        {
            const int p = sourcePart(abcd, fix, int(src[k]), int(as));
            a[k] = float(config == 0u ? clamp(p, 0, 255) : config == 1u ? max(p, 0) : max(-p, 0));
        }
        d = vec3(kd == 1u ? 1.0 : 0.0);
    }
    else
    {
        const uint n = coef128(ks, f);
        const bool biasOn = (flags & (1u << 6)) != 0u;
        const bool biasAlways = (W(40u) & 2u) != 0u; // planted fault
        for (uint k = 0u; k < 3u; ++k)
        {
            precise float lv = float(src[k] * n) / 128.0;
            if (config == 2u)
            {
                if (biasOn)
                    lv = lv + 127.0 / 256.0;
            }
            else if (biasOn && (2u * src[k] * n >= 127u || biasAlways))
                lv = lv - 127.0 / 256.0;
            a[k] = lv;
        }
        d = vec3(float(coef128(kd, f)) / 128.0);
    }
    out0 = vec4(a / 255.0, float(as) / 255.0);
    out1 = vec4(d, part == 3u ? (pass ? 1.0 : 0.0) : 1.0);
#endif
#endif
    gl_FragDepth = float(min(zu, 0xFFFFFFu)) * (1.0 / 16777216.0);
}
