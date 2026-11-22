#pragma once

// The track program's family (RENDERER_P2_DESIGN.md 4.2): TRdr_VUShapeProg entry 0 (batch 3), EnvRdr_VUShapeProg entries
// 0 and 0x78 and TERdr_VUShapeProg entry 0 (batch 4). The three share the triangle format (ten quadwords: normal and
// winding, three positions, three S T Q with the depth bias in the first w, three colours), setupTriangle, the quick
// path and the clippers; they differ in what happens to the colours and textures between setupTriangle and
// drawTriangle, in the loop over texture blocks, and in a few settings of their ShapeLayout:
//
// | | TRdr | Env | TERdr |
// |---|---|---|---|
// | header packet, constants | 247, always | none, at entry 0 only (0x78: the registers as they are) | 248, always (four quadwords earlier) |
// | texture blocks | runTextureBlocks | its own loop: the first quadword is always a block; quadword 3 takes VF31, with 10.xy in place of xy under the map | runTextureBlocks |
// | depth bias | setupTriangle | setupTriangle | the quick path, after the clip flags |
// | colours | applyTint of the integers | the map off: applyTint of the integers of the floats; on: one tinted colour a block from 10.w * VF31.w | lightCorners: four lights and a colour matrix |
// | textures | as read | the map off: as read; on: from the normal in the colours' fractions and the view direction (environmentCorners) | as read |
// | registers every triangle writes | VF11-VF20, VF25.xyz | the same; under the map VF14-VF19, VF26-VF28 (VF26.w reaches water's packets): the run's last triangle is the step's | the same; every lit triangle VF24-VF30 from VU memory 18-24 (VF25.w, VF26, VF29 reach packets): reloaded after a run whose last lit triangle was fast |
// | wrap of the packet buffer in the quick path | flush after | none | flush after |
//
// Each source instantiates runFamily for its program (fast_trdr.cpp, fast_env.cpp, fast_terdr.cpp).

#include "fast_simd.h"

namespace rc::shape::detail
{
    enum class Family
    {
        Trdr,
        Env,
        Terdr
    };

    template <Family K> bool runFamily(uint8_t *mem, float (*vf)[4], uint32_t pc, uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &opt)
    {
        constexpr uint32_t prog = K == Family::Trdr ? v::kTrdr : K == Family::Env ? v::kEnv : v::kTerdr;
        FastCounts &st = stats.by[prog];
        // 4.3: the count walk must stay within the input buffer, or the run is the reference's. It also finds the run's
        // last triangle (Env: an env-mapped run hands it to the step).
        const int start = static_cast<int>(top & 0x3FFu);
        const int maxIn = static_cast<int>(kMaxRunInput);
        int lastTri = -1;
        {
            int r = start;
            int c = count(mem, r);
            if constexpr (K == Family::Env)
            {
                // The program's loop: a texture block (whatever its count), its triangles, then register blocks.
                if (c <= 0)
                {
                    ++st.declinedParse;
                    return false;
                }
                for (;;)
                {
                    r += 4;
                    lastTri = r + 10 * (c - 1);
                    r += 10 * c;
                    if (r - start >= maxIn)
                    {
                        ++st.declinedParse;
                        return false;
                    }
                    c = count(mem, r);
                    while (c < 0)
                    {
                        r += 1 - c;
                        if (r - start >= maxIn)
                        {
                            ++st.declinedParse;
                            return false;
                        }
                        c = count(mem, r);
                    }
                    if (c == 0)
                        break;
                }
            }
            else
            {
                // runTextureBlocks
                for (;;)
                {
                    while (c < 0)
                    {
                        r += 1 - c;
                        if (r - start >= maxIn)
                        {
                            ++st.declinedParse;
                            return false;
                        }
                        c = count(mem, r);
                        if (c == 0)
                            break;
                    }
                    if (c <= 0)
                        break;
                    r += 4 + 10 * c;
                    if (r - start >= maxIn)
                    {
                        ++st.declinedParse;
                        return false;
                    }
                    c = count(mem, r);
                }
            }
            if (static_cast<uint32_t>(r - start + 1) > stats.maxExtent)
                stats.maxExtent = static_cast<uint32_t>(r - start + 1);
        }
        ++st.runs;

        const v::ScopedMode mode; // the caller's MXCSR (round toward zero) with FTZ and DAZ, restored on return or a throw
        const v::ShapeLayout &layout = K == Family::Trdr ? v::trdrLayout() : K == Family::Env ? v::envLayout() : v::terdrLayout();
        v::ShapeProgram p(mem, vf, layout, &onKick, &sink);

        // Header packet, then the constants.
        if constexpr (K == Family::Trdr)
        {
            p.kick(247);
            p.loadConstants();
        }
        else if constexpr (K == Family::Terdr)
        {
            p.kick(248);
            p.loadConstants();
        }
        else if (pc == 0u)
            p.loadConstants(); // at 0x78 the constants are still in the registers
        const bool envMap = K == Family::Env && p.loadInt(10, 2) > 0;
        if (envMap)
            ++st.envRuns;

        const auto vec = [&](int reg) { return _mm_loadu_ps(p.r[reg].f); };
        const __m128 M[4] = {vec(1), vec(2), vec(3), vec(4)};
        const __m128 r21 = vec(21);
        const __m128 tint = ld(mem, 11); // applyTint's quadword: nothing in a run writes it
        const __m128i kXYZ = lanes(7), kXY = lanes(3), kAll = lanes(15), kZ = lanes(4);
        const __m128 one = _mm_set1_ps(1.0f), tintW = _mm_shuffle_ps(tint, tint, 0xFF);
        const float nearNeg = v::fsub(0.0f, p.r[23].f[2]).value; // setupTriangle's nearTest
        QuickCtx qc;
        qc.r9 = vec(9);
        qc.r10 = vec(10);
        {
            const uint32_t wBits = v::bits(p.r[9].f[3]);
            qc.limit = _mm_set1_epi32((wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF); // clip()
        }
        qc.roundX = faultOn(opt, FastFault::RoundX, prog);
        qc.edgeTwo = faultOn(opt, FastFault::EdgeTwo, prog);
        const uint32_t tw[3] = {v::bits(p.r[layout.quickTemp[0]].f[3]), v::bits(p.r[layout.quickTemp[1]].f[3]), v::bits(p.r[layout.quickTemp[2]].f[3])};
        const bool fBias = faultOn(opt, FastFault::Bias, prog), fFused = faultOn(opt, FastFault::Fused, prog),
                   fHardQuick = faultOn(opt, FastFault::HardQuick, prog), fLast = faultOn(opt, FastFault::LastStep, prog),
                   fLit = faultOn(opt, FastFault::LitRegs, prog);

        // Env under the map: the rows and settings of VU memory 7-10 (constant for the run), and per block the colour.
        const __m128 row0 = ld(mem, 7), row1 = ld(mem, 8), row2 = ld(mem, 9);
        const v::V settings = p.load(10);
        __m128i blockColour = _mm_setzero_si128();
        float intensity = 0.0f;
        // TERdr: the light registers (lightCorners loads VF24-VF30 from VU memory 18-24 for every lit triangle).
        const __m128 L[7] = {ld(mem, 18), ld(mem, 19), ld(mem, 20), ld(mem, 21), ld(mem, 22), ld(mem, 23), ld(mem, 24)};
        bool lit = false; // a fast triangle was lit since the registers were last the program's
        const auto relight = [&]
        {
            if (lit && !fLit)
                for (int i = 0; i < 7; ++i)
                    p.r[24 + i] = p.load(18 + i);
            lit = false;
        };

        // The env-mapped corners (environmentCorners): the textures from the normal packed in the colours' fractions and
        // the direction from VF21 to the corner; reciprocalLength as the reference's (scalar).
        const auto envCorners = [&](Chk &k, const __m128 V[3], int reader, __m128 T[3])
        {
            const __m128 half = _mm_set1_ps(0.5f), two = _mm_set1_ps(2.0f), zOne = _mm_set_ps(0.0f, 1.0f, 0.0f, 0.0f);
            float scale[3];
            __m128 n[3];
            for (int c = 0; c < 3; ++c)
            {
                // Only x, y, z reach a texture: the w lanes reach VF26-VF28.w, which the run's last step leaves right.
                const __m128 packed = ld(mem, reader + 7 + c);
                const __m128 whole = _mm_cvtepi32_ps(ftoi(packed));
                __m128 nn = chk(k, _mm_sub_ps(packed, whole), kXYZ);
                nn = chk(k, _mm_sub_ps(nn, half), kXYZ);
                nn = chk(k, _mm_add_ps(nn, nn), kXYZ);
                __m128 eye = chk(k, _mm_sub_ps(V[c], r21), kXYZ);
                v::V e;
                _mm_storeu_ps(e.f, eye);
                const float rl = v::reciprocalLength(e);
                eye = chk(k, _mm_mul_ps(eye, _mm_set1_ps(rl)), kXYZ);
                nn = chk(k, _mm_mul_ps(nn, nn), kXYZ);
                nn = chk(k, _mm_mul_ps(nn, eye), kXYZ);
                nn = chk(k, _mm_mul_ps(nn, two), kXYZ);
                nn = chk(k, _mm_sub_ps(nn, eye), kXYZ);
                __m128 acc = chk(k, _mm_mul_ps(nn, row0), kXYZ);
                acc = madd(k, acc, nn, row1, kXYZ);
                nn = madd(k, acc, nn, row2, kXYZ);
                nn = sel(chk(k, _mm_add_ps(nn, zOne), kZ), nn, kZ);
                v::V nv;
                _mm_storeu_ps(nv.f, nn);
                scale[c] = v::reciprocalLength(nv);
                n[c] = nn;
            }
            for (int c = 0; c < 3; ++c)
            {
                __m128 nn = chk(k, _mm_mul_ps(n[c], _mm_set1_ps(scale[c])), kXY);
                nn = chk(k, _mm_mul_ps(nn, half), kXY);
                const __m128 st2 = chk(k, _mm_add_ps(nn, half), kXY);
                alignas(16) float t[4];
                _mm_store_ps(t, st2);
                t[2] = 1.0f;
                t[3] = c == 0 ? intensity : 0.0f;
                T[c] = _mm_load_ps(t);
            }
        };

        // One triangle at `reader`: true when the path drew it or found that it draws nothing; false: the step's.
        const auto fast = [&](int reader) -> bool
        {
            ++st.in;
            Chk k;
            const __m128 N = ld(mem, reader), T0 = ld(mem, reader + 4);
            // setupTriangle: the facing test's products, the corners through the matrix, the depth bias
            const __m128 rel = chk(k, _mm_sub_ps(ld(mem, reader + 1), r21), kXYZ);
            const __m128 d = chk(k, _mm_mul_ps(rel, N), kXYZ);
            const bool setupBias = K != Family::Terdr && !fBias;
            const __m128 bias = _mm_and_ps(_mm_castsi128_ps(kZ), _mm_shuffle_ps(T0, T0, 0xFF));
            __m128 V[3];
            for (int c = 0; c < 3; ++c)
            {
                const __m128 acc = transform4(k, M, ld(mem, reader + 1 + c), kAll, fFused);
                V[c] = setupBias ? sel(chk(k, _mm_add_ps(acc, bias), kZ), acc, kZ) : acc; // the bias on z only
            }
            alignas(16) float df[4];
            _mm_store_ps(df, d);
            const float facing = sc(k, sc(k, df[0] + df[1]) + df[2]);
            const bool negative = std::signbit(facing);
            const int16_t winding = static_cast<int16_t>(v::bits(lane(N, 3)));
            int behind = 0;
            if (std::signbit(sc(k, nearNeg + lane(V[0], 2))))
                behind |= 128;
            if (std::signbit(sc(k, nearNeg + lane(V[1], 2))))
                behind |= 64;
            if (std::signbit(sc(k, nearNeg + lane(V[2], 2))))
                behind |= 32;
            if (k.any())
            {
                ++st.odd;
                return false;
            }
            if (winding == (negative ? 128 : 0))
            {
                ++st.culled;
                return true;
            }
            if constexpr (K == Family::Terdr)
                lit = true; // lightCorners runs for every triangle that faces the viewer
            if (behind == 224)
            {
                ++st.allBehind;
                return true;
            }
            if (behind != 0 && !fHardQuick)
                return false;

            // The colours and textures as drawTriangle finds them.
            __m128i colour[3];
            __m128 T[3] = {T0, ld(mem, reader + 5), ld(mem, reader + 6)};
            if constexpr (K == Family::Terdr)
            {
                // lightCorners
                for (int c = 0; c < 3; ++c)
                {
                    const __m128 n = ld(mem, reader + 7 + c);
                    __m128 acc = chk(k, _mm_mul_ps(L[0], splat(n, 0)), kAll);
                    acc = madd(k, acc, L[1], splat(n, 1), kAll);
                    const __m128 value = madd(k, acc, L[2], splat(n, 2), kAll);
                    const __m128 light = _mm_and_ps(_mm_cmpgt_ps(value, _mm_setzero_ps()), value); // value > 0 ? value : +0
                    acc = chk(k, _mm_mul_ps(L[3], splat(light, 0)), kXYZ);
                    acc = madd(k, acc, L[4], splat(light, 1), kXYZ);
                    acc = madd(k, acc, L[5], splat(light, 2), kXYZ);
                    acc = madd(k, acc, L[6], splat(light, 3), kXYZ);
                    colour[c] = ftoi(sel(acc, _mm_set1_ps(127.0f), kXYZ));
                }
            }
            else if (envMap)
            {
                colour[0] = colour[1] = colour[2] = blockColour;
                envCorners(k, V, reader, T);
            }
            else
            {
                // applyTint (Env: of the integers of the colour registers' floats)
                for (int c = 0; c < 3; ++c)
                {
                    const __m128 raw = ld(mem, reader + 7 + c);
                    __m128i ints = _mm_castps_si128(raw);
                    if constexpr (K == Family::Env)
                    {
                        // ftoi0 on every lane: an infinity or NaN would be the largest value first
                        chk(k, raw, kAll);
                        ints = ftoi(raw);
                    }
                    const __m128 f = _mm_cvtepi32_ps(ints);
                    const __m128 acc = chk(k, _mm_mul_ps(f, tintW), kXYZ);
                    const __m128 r = madd(k, acc, tint, one, kXYZ);
                    colour[c] = _mm_castps_si128(sel(_mm_castsi128_ps(ftoi(r)), _mm_castsi128_ps(ints), kXYZ));
                }
            }

            alignas(16) uint32_t nine[36];
            const float zb = fBias ? 0.0f : lane(T0, 3);
            switch (quickTriangle(k, V, T, colour, tw, qc, K == Family::Terdr ? &zb : nullptr, nine))
            {
            case Quick::Odd: ++st.odd; return false;
            case Quick::Hard: return false;
            case Quick::Edge: ++st.edge; return true;
            case Quick::Drawn: break;
            }
            emitQuick(p, sink, st, nine);
            return true;
        };

        const auto step = [&](int reader)
        {
            if constexpr (K == Family::Trdr)
                v::trdrStep(p, reader);
            else if constexpr (K == Family::Env)
                v::envStep(p, reader, envMap);
            else
            {
                relight();
                v::terdrStep(p, reader);
            }
        };

        int reader = start;
        if constexpr (K == Family::Env)
        {
            // The program's loop (env_shape.cpp body).
            int n = count(mem, reader);
            for (;;)
            {
                // Four quadwords of texture state go out through a packet at the start of VU memory.
                p.r[31] = p.load(reader);
                if (envMap)
                {
                    p.r[31].f[0] = settings.f[0];
                    p.r[31].f[1] = settings.f[1];
                    // environmentCorners' intensity and the colour every corner of the block takes, tinted (applyTint)
                    intensity = v::fmul(settings.f[3], p.r[31].f[3]).value;
                    const float scaled = v::fmul(v::fadd(0.0f, 127.0f).value, intensity).value;
                    const float c = v::ftoi0(scaled < 127.0f ? scaled : 127.0f);
                    alignas(16) float col[4];
                    for (int l = 0; l < 3; ++l)
                    {
                        const float acc = v::fmul(v::itof0(c), lane(tint, 3)).value;
                        col[l] = v::ftoi0(v::fmadd(acc, lane(tint, l), 1.0f).value);
                    }
                    col[3] = v::ftoi0(v::fadd(1.0f, 127.0f).value);
                    blockColour = _mm_castps_si128(_mm_load_ps(col));
                }
                p.store(3, p.r[31]);
                for (int i = 1; i < 4; ++i)
                    p.store(3 + i, p.load(reader + i));
                reader += 4;
                p.kick(0);
                p.vi11 = 26;
                ++st.blocks;
                for (; n != 0; --n, reader += 10)
                {
                    if (envMap && reader == lastTri && !fLast)
                    {
                        // The run's last triangle: its step leaves VF26-VF28 (and the rest) as the program does.
                        ++st.in;
                        ++st.lastSteps;
                        step(reader);
                    }
                    else if (!fast(reader))
                    {
                        ++st.hard;
                        step(reader);
                    }
                }
                // A negative count is a block of GS registers: kick it and step over it.
                n = count(mem, reader);
                while (n < 0)
                {
                    p.kick(reader + 1);
                    reader += 1 - n;
                    n = count(mem, reader);
                }
                if (n == 0)
                    break;
            }
        }
        else
        {
            // runTextureBlocks
            int n = count(mem, reader);
            for (;;)
            {
                bool end = false;
                while (n < 0)
                {
                    p.kick(reader + 1);
                    reader += 1 - n;
                    n = count(mem, reader);
                    if (n == 0)
                    {
                        end = true;
                        break;
                    }
                }
                if (end || n == 0)
                    break;
                for (int i = 0; i < 4; ++i)
                    p.store(3 + i, p.load(reader + i));
                reader += 4;
                p.kick(0);
                p.vi11 = layout.firstVertex;
                ++st.blocks;
                for (; n != 0; --n, reader += 10)
                    if (!fast(reader))
                    {
                        ++st.hard;
                        step(reader);
                    }
                n = count(mem, reader);
            }
        }
        if constexpr (K == Family::Terdr)
            relight();
        std::memcpy(vf, p.r, sizeof(p.r));
        return true;
    }
}
