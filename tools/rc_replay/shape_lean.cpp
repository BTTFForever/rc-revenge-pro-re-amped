// rc_replay's shapestat consumer, second half (RENDERER_P2_DESIGN.md section 3): a second build of the C++ VU1
// references of decomp/vu1 whose arithmetic is the host's own under MXCSR = round toward zero + flush to zero +
// denormals are zero, with the reference's operation only where that mode cannot give the VU's result. Measurement
// only: nothing here is the renderer. The sources are included as text under another namespace (rc::vu1lean), so the
// file-local layouts and loops are in reach for the classification below. Since step 9's batch 1 the operations are
// decomp/vu1/vu_lean.h's (the lean build, library rc_vu1_lean), which began here.
//
// What the mode gives without help (shape_survey.cpp's self-test compares every operation with the reference on
// random bit patterns): a denormal operand is zero (norm's first rule), a tiny result is a zero with the exact result's
// sign (finish's underflow), an overflow under round toward zero is the largest finite value (finish's clamp). What it
// cannot: an infinity or NaN operand (norm makes it the largest finite value), seen as an infinity or NaN result; and
// MADD's product, which the reference rounds to single precision but neither flushes nor clamps before the sum (a
// denormal product takes part in the sum; after an overflowing product the sum is clamped), seen as a zero product of
// two non-zero factors or a product of the largest magnitude. Those fall back to the reference's operation, run under
// the reference's mode (round toward zero only).

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <emmintrin.h>

#include "shape_lean.h"

// The lean build's own header (vu_shape.h with RC_VU1_LEAN 1: decomp/vu1/vu_lean.h's operations, which began here),
// under the name rc::vu1lean so that it does not meet rc_vu1_lean's rc::vu1l at link time.
#define vu1 vu1lean
#define RC_VU1_LEAN 1
#define RC_VU1_NAMESPACE vu1lean
#include "vu_shape.h"

#include "vu_shape.cpp"
#define kLayout kLayoutTrdr
#define body bodyTrdr
#include "trdr_shape.cpp"
#undef kLayout
#undef body
#define kLayout kLayoutEnv
#define body bodyEnv
#include "env_shape.cpp"
#undef kLayout
#undef body
#define kLayout kLayoutWater
#define body bodyWater
#include "water_shape.cpp"
#undef kLayout
#undef body
#define kLayout kLayoutSkin
#define body bodySkin
#include "skin_shape.cpp"
#undef kLayout
#undef body
#define kLayout kLayoutTerdr
#define body bodyTerdr
#include "terdr_shape.cpp"
#undef kLayout
#undef body
#undef vu1

namespace rc::replay::lean
{
    namespace v = rc::vu1lean;

    namespace
    {
        constexpr uint32_t kLeanCsr = 0x6000u | 0x8000u | 0x0040u; // RZ, FTZ, DAZ

        struct Sink
        {
            std::vector<uint8_t> *bytes;
            std::vector<uint32_t> *sizes;
        };
        void onPacket(void *user, const uint8_t *data, uint32_t n)
        {
            Sink &s = *static_cast<Sink *>(user);
            s.bytes->insert(s.bytes->end(), data, data + n);
            s.sizes->push_back(n);
        }
        void countPacket(void *user, const uint8_t *, uint32_t) { ++*static_cast<uint32_t *>(user); }

        struct Classifier
        {
            v::ShapeProgram &p;
            RunClass &out;
            const uint32_t &packets; // XGKICKs so far (the counting callback's)
            bool anyNonPlain = false;
            double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300}, biasLo = 1e300, biasHi = -1e300;
            bool wOne = true;

            void bounds(int reader, bool bias)
            {
                for (int corner = 0; corner < 3; ++corner)
                {
                    const v::V pos = p.load(reader + 1 + corner);
                    for (int l = 0; l < 3; ++l)
                    {
                        lo[l] = std::min(lo[l], static_cast<double>(pos.f[l]));
                        hi[l] = std::max(hi[l], static_cast<double>(pos.f[l]));
                    }
                    if (pos.f[3] != 1.0f)
                        wOne = false;
                }
                const double b = bias ? static_cast<double>(p.load(reader + 4).f[3]) : 0.0;
                biasLo = std::min(biasLo, b);
                biasHi = std::max(biasHi, b);
            }

            // After the triangle's corners are in r11-r13 in view space (setupTriangle or the skin's own transform).
            void drawn(bool facing)
            {
                ++out.in;
                if (!facing)
                {
                    ++out.culled;
                    return;
                }
                if (p.behind == 224)
                {
                    ++out.allBehind;
                    anyNonPlain = true;
                    return;
                }
                if (p.behind != 0)
                {
                    ++out.nearCut;
                    anyNonPlain = true;
                    return;
                }
                const uint32_t before = packets;
                p.vi11 = p.layout.firstVertex;
                p.writeQuickTriangle();
                if ((p.clipFlags & 0xF3CFu) == 0u)
                {
                    ++out.quick;
                    return;
                }
                anyNonPlain = true;
                bool early = false;
                for (uint32_t mask : {0xFFEFBEu, 0xFFDF7Du, 0xFFBEFBu, 0xFF7DF7u})
                    if ((p.clipFlags | mask) == 0xFFFFFFu)
                        early = true;
                if (early)
                    ++out.guardEarly;
                if (packets == before)
                    ++out.guardNone;
                else
                {
                    ++out.guardFan;
                    out.guardFanTriangles += packets - before;
                }
            }

            // The run-level test a batch reader could make without transforming a triangle: the corners of the run's
            // bounding box (and the two extremes of the depth bias) in front of the near plane and inside the guard
            // band, with a margin. Conservative for a convex set in front of the eye.
            void box(const v::ShapeLayout &layout)
            {
                if (out.in == 0)
                    return;
                out.boxTested = true;
                if (!wOne)
                    return;
                const v::V m0 = p.r[1], m1 = p.r[2], m2 = p.r[3], m3 = p.r[4];
                const double nearZ = p.r[23].f[2];
                const uint32_t wBits = v::bits(p.r[9].f[3]);
                const double limit = (wBits & 0x7F800000u) != 0u ? static_cast<double>(v::fromBits(wBits & 0x7FFFFFFFu)) : 0.0;
                const double sx = std::fabs(p.r[10].f[0]), sy = std::fabs(p.r[10].f[1]);
                (void)layout;
                bool ok = limit > 0.0 && nearZ > 0.0;
                for (int c = 0; c < 8 && ok; ++c)
                {
                    const double x = (c & 1) ? hi[0] : lo[0], y = (c & 2) ? hi[1] : lo[1], z = (c & 4) ? hi[2] : lo[2];
                    const double vx = m0.f[0] * x + m1.f[0] * y + m2.f[0] * z + m3.f[0];
                    const double vy = m0.f[1] * x + m1.f[1] * y + m2.f[1] * z + m3.f[1];
                    const double vz = m0.f[2] * x + m1.f[2] * y + m2.f[2] * z + m3.f[2];
                    for (const double b : {biasLo, biasHi})
                    {
                        const double zz = vz + b;
                        if (!(zz >= nearZ * 1.0001 + 1e-3))
                            ok = false;
                        else if (std::fabs(vx / zz) * sx > limit * 0.9999 || std::fabs(vy / zz) * sy > limit * 0.9999)
                            ok = false;
                    }
                }
                out.boxPlain = ok;
            }
        };
    }

    uint32_t csr(uint32_t kickCsr) { return (kickCsr & ~0x6000u) | kLeanCsr; }

    bool run(int program, uint8_t *memory, float (*vf)[4], uint32_t startPc, uint32_t top, std::vector<uint8_t> &bytes, std::vector<uint32_t> &sizes)
    {
        Sink sink{&bytes, &sizes};
        v::Io io{memory, vf, startPc, top, &onPacket, &sink};
        switch (program)
        {
        case 0: v::runTrdrShapeIo(io); return true;
        case 1: v::runEnvShapeIo(io); return true;
        case 2: v::runWaterShapeIo(io); return true;
        case 3: v::runSkinShapeIo(io); return true;
        case 4: v::runTerdrShapeIo(io); return true;
        default: return false;
        }
    }

    // `memory` is a scratch copy (it is changed). Programs: vu1_ref.h's order (0 TRdr, 1 Env, 2 water, 3 skin, 4 TERdr).
    void classify(int program, uint8_t *memory, const float (*vf)[4], uint32_t startPc, uint32_t top, RunClass &out)
    {
        out = RunClass{};
        if (startPc != 0u || program < 0 || program > 4)
        {
            out.direct = true;
            return;
        }
        const v::ShapeLayout &layout = program == 0 ? v::kLayoutTrdr : program == 1 ? v::kLayoutEnv : program == 2 ? v::kLayoutWater : program == 3 ? v::kLayoutSkin : v::kLayoutTerdr;
        uint32_t packets = 0;
        v::ShapeProgram p(memory, vf, layout, &countPacket, &packets);
        Classifier c{p, out, packets};
        int guard = 0;
        try
        {
        if (program == 0 || program == 4)
        {
            p.loadConstants();
            v::runTextureBlocks(p, static_cast<int>(top & 0x3FFu), [&](int &reader)
            {
                if (++guard > 4096)
                    throw 0; // garbage in VU memory
                c.bounds(reader, layout.hasDepthBias);
                c.drawn(p.setupTriangle(reader));
            });
            ++out.blocks; // not counted per block here
        }
        else if (program == 1 || program == 2)
        {
            p.loadConstants();
            int reader = static_cast<int>(top & 0x3FFu);
            int16_t count = p.loadInt(reader, 2);
            for (;;)
            {
                reader += 4;
                ++out.blocks;
                do
                {
                    if (++guard > 4096)
                        break;
                    c.bounds(reader, layout.hasDepthBias);
                    c.drawn(p.setupTriangle(reader));
                } while (--count != 0);
                if (guard > 4096)
                    break;
                count = p.loadInt(reader, 2);
                while (program == 1 && count < 0)
                {
                    reader += 1 - count;
                    count = p.loadInt(reader, 2);
                }
                if (count == 0)
                    break;
            }
        }
        else
        {
            for (int i = 0; i < 4; ++i)
                p[5 + i] = p.load(21 + i);
            p[9] = p.load(15);
            p[10] = p.load(16);
            p[21] = p.load(14);
            p[22] = p.load(13);
            p[23] = p.load(10);
            int reader = 512;
            for (;;)
            {
                int16_t count = p.loadInt(reader, 2);
                ++reader;
                ++out.blocks;
                do
                {
                    if (++guard > 4096)
                        break;
                    v::loadAndTransform(p, reader);
                    c.drawn(true);
                } while (--count != 0);
                if (guard > 4096 || p.loadInt(reader, 2) == 0)
                    break;
            }
        }
        }
        catch (int)
        {
            out.garbage = true;
        }
        out.plain = !c.anyNonPlain;
        if (program != 3)
            c.box(layout);
    }

    // The lean operations against the reference's, on random bit patterns: each operation of the pair is run under
    // its own MXCSR (lean: RZ + FTZ + DAZ; reference: RZ). Returns the number of differing results.
    uint64_t selfTest(uint64_t cases, uint64_t *done)
    {
        uint64_t state = 0x9E3779B97F4A7C15ull, bad = 0, n = 0;
        const auto next = [&]
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };
        const auto pick = [&]() -> float
        {
            const uint64_t r = next();
            uint32_t b = static_cast<uint32_t>(r >> 32);
            switch (r & 15u)
            {
            case 0: b &= 0x807FFFFFu; break;                                   // a denormal or zero
            case 1: b = (b & 0x807FFFFFu) | 0x7F800000u; break;                // infinity or NaN
            case 2: b = (b & 0x80FFFFFFu) | 0x00800000u; break;                // the smallest binade
            case 3: b = (b & 0x80FFFFFFu) | 0x7F000000u; break;                // the largest
            case 4: b = (b & 0x80000000u); break;                              // a signed zero
            case 5: case 6: b = (b & 0x807FFFFFu) | ((0x70u + (b >> 23 & 31u)) << 23); break; // near one
            case 7: b = (b & 0x807FFFFFu) | ((0x10u + (b >> 23 & 63u)) << 23); break;         // small
            default: break;
            }
            return v::fromBits(b);
        };
        const uint32_t saved = _mm_getcsr();
        const uint32_t refCsr = (saved & ~0xE040u) | 0x6000u, leanCsr = csr(saved);
        for (uint64_t i = 0; i < cases; ++i)
        {
            const float a = pick(), b = pick(), c = pick();
            v::Fmac r[4], l[4];
            _mm_setcsr(refCsr);
            r[0] = v::faddStrict(a, b);
            r[1] = v::fsubStrict(a, b);
            r[2] = v::fmulStrict(a, b);
            r[3] = v::fmaddStrict(c, a, b);
            _mm_setcsr(leanCsr);
            l[0] = v::fadd(a, b);
            l[1] = v::fsub(a, b);
            l[2] = v::fmul(a, b);
            l[3] = v::fmadd(c, a, b);
            for (int k = 0; k < 4; ++k, ++n)
                if (v::bits(r[k].value) != v::bits(l[k].value) || r[k].negative != l[k].negative)
                {
                    ++bad;
                    static int shown[4];
                    if (shown[k]++ < 6)
                        std::fprintf(stderr, "selftest op %d a %08x b %08x c %08x ref %08x/%d lean %08x/%d\n", k, v::bits(a), v::bits(b), v::bits(c), v::bits(r[k].value), r[k].negative, v::bits(l[k].value), l[k].negative);
                }
        }
        _mm_setcsr(saved);
        if (done)
            *done = n;
        return bad;
    }

    namespace
    {
        struct Chk
        {
            __m128i special = _mm_setzero_si128();
            __m128 tiny = _mm_setzero_ps();
            bool scalar = false;
        };
        const __m128i kExp = _mm_set1_epi32(0x7F800000);
        inline __m128i lanes(int mask) { return _mm_set_epi32((mask & 8) ? -1 : 0, (mask & 4) ? -1 : 0, (mask & 2) ? -1 : 0, (mask & 1) ? -1 : 0); }
        inline __m128 chk(Chk &k, __m128 r, __m128i mask)
        {
            k.special = _mm_or_si128(k.special, _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(r), kExp), kExp), mask));
            return r;
        }
        // acc + a * b on the lanes of `mask`, the product rounded first; left to the reference: a zero product of two
        // non-zero factors, a product of the largest magnitude (it may have overflowed: the reference clamps the sum),
        // and (step 9 batch 3: batch 1's third fall-back, vu_lean.h) a sum below 2^-101 from a non-zero accumulator.
        inline __m128 madd(Chk &k, __m128 acc, __m128 a, __m128 b, __m128i mask = _mm_set1_epi32(-1))
        {
            const __m128 zero = _mm_setzero_ps();
            const __m128i mag = _mm_set1_epi32(0x7FFFFFFF);
            const __m128 p = _mm_mul_ps(a, b);
            k.tiny = _mm_or_ps(k.tiny, _mm_and_ps(_mm_castsi128_ps(mask), _mm_and_ps(_mm_cmpeq_ps(p, zero), _mm_and_ps(_mm_cmpneq_ps(a, zero), _mm_cmpneq_ps(b, zero)))));
            k.special = _mm_or_si128(k.special, _mm_and_si128(mask, _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(p), mag), _mm_set1_epi32(0x7F7FFFFF))));
            const __m128 r = _mm_add_ps(acc, p);
            const __m128i tinySum = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(acc), mag), _mm_setzero_si128()),
                                                     _mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(r), mag), _mm_set1_epi32(0x0D000000)));
            k.special = _mm_or_si128(k.special, _mm_and_si128(mask, tinySum));
            return chk(k, r, mask);
        }
        inline float sc(Chk &k, float r)
        {
            if (v::special(r))
                k.scalar = true;
            return r;
        }
        // lanes of `a` where the mask is set, of `b` elsewhere
        inline __m128 sel(__m128 a, __m128 b, __m128i mask) { return _mm_or_ps(_mm_and_ps(_mm_castsi128_ps(mask), a), _mm_andnot_ps(_mm_castsi128_ps(mask), b)); }
        inline __m128i ftoi(__m128 x)
        {
            const __m128i big = _mm_castps_si128(_mm_cmpge_ps(x, _mm_set1_ps(2147483648.0f)));
            const __m128i t = _mm_cvttps_epi32(x);
            return _mm_or_si128(_mm_and_si128(big, _mm_set1_epi32(0x7FFFFFFF)), _mm_andnot_si128(big, t));
        }
        inline float lane(__m128 x, int i)
        {
            alignas(16) float f[4];
            _mm_store_ps(f, x);
            return f[i];
        }
    }

    void fastTrdr(const uint8_t *memory, const float (*vf)[4], uint32_t top, std::vector<uint8_t> &out, FastCount &count)
    {
        const auto ld = [&](int quad) { return _mm_loadu_ps(reinterpret_cast<const float *>(memory + (quad & 0x3FF) * 16)); };
        const auto cnt = [&](int quad)
        {
            int16_t c;
            std::memcpy(&c, memory + (quad & 0x3FF) * 16 + 8, 2);
            return c;
        };
        const __m128 m1 = ld(18), m2 = ld(19), m3 = ld(20), m4 = ld(21), r9 = ld(16), r10 = ld(17), r21 = ld(15), r23 = ld(12), tint = ld(11);
        const __m128i kXYZ = lanes(7), kXY = lanes(3), kXYW = lanes(11), kAll = lanes(15);
        const __m128 one = _mm_set1_ps(1.0f), tintW = _mm_shuffle_ps(tint, tint, 0xFF);
        const float nearNeg = 0.0f - lane(r23, 2);
        const uint32_t wBits = v::bits(lane(r9, 3));
        const __m128i limit = _mm_set1_epi32((wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF);
        const __m128i kMag = _mm_set1_epi32(0x7FFFFFFF);
        const uint32_t tw[3] = {v::bits(vf[5][3]), v::bits(vf[6][3]), v::bits(vf[7][3])};

        int reader = static_cast<int>(top & 0x3FFu);
        int count16 = cnt(reader);
        int guard = 0;
        for (;;)
        {
            while (count16 < 0)
            {
                reader += 1 - count16;
                count16 = cnt(reader);
                if (count16 == 0 || ++guard > 4096)
                    return;
            }
            if (count16 == 0)
                return;
            reader += 4; // the texture block
            ++count.blocks;
            for (; count16 != 0; --count16, reader += 10)
            {
                if (++guard > 4096)
                    return;
                ++count.in;
                Chk k;
                const __m128 N = ld(reader), T0 = ld(reader + 4);
                const __m128 P[3] = {ld(reader + 1), ld(reader + 2), ld(reader + 3)};
                const __m128 rel = chk(k, _mm_sub_ps(P[0], r21), kXYZ);
                const __m128 d = chk(k, _mm_mul_ps(rel, N), kXYZ);
                const __m128 bias = _mm_and_ps(_mm_castsi128_ps(lanes(4)), _mm_shuffle_ps(T0, T0, 0xFF));
                __m128 V[3];
                for (int c = 0; c < 3; ++c)
                {
                    const __m128 x = _mm_shuffle_ps(P[c], P[c], 0x00), y = _mm_shuffle_ps(P[c], P[c], 0x55), z = _mm_shuffle_ps(P[c], P[c], 0xAA),
                                 w = _mm_shuffle_ps(P[c], P[c], 0xFF);
                    __m128 acc = chk(k, _mm_mul_ps(m1, x), kAll);
                    acc = madd(k, acc, m2, y);
                    acc = madd(k, acc, m3, z);
                    acc = madd(k, acc, m4, w);
                    // the depth bias: z only (the other lanes add zero bits and must stay as they are)
                    V[c] = sel(chk(k, _mm_add_ps(acc, bias), lanes(4)), acc, lanes(4));
                }
                alignas(16) float df[4];
                _mm_store_ps(df, d);
                const float facing = sc(k, sc(k, df[0] + df[1]) + df[2]);
                const bool negative = std::signbit(facing);
                const int16_t winding = static_cast<int16_t>(v::bits(lane(N, 3)));
                const float z0 = lane(V[0], 2), z1 = lane(V[1], 2), z2 = lane(V[2], 2);
                int behind = 0;
                if (std::signbit(sc(k, nearNeg + z0)))
                    behind |= 128;
                if (std::signbit(sc(k, nearNeg + z1)))
                    behind |= 64;
                if (std::signbit(sc(k, nearNeg + z2)))
                    behind |= 32;
                const bool odd = k.scalar || _mm_movemask_epi8(k.special) != 0 || _mm_movemask_ps(k.tiny) != 0;
                if (odd)
                {
                    ++count.odd;
                    ++count.hard;
                    continue;
                }
                if (winding == (negative ? 128 : 0))
                {
                    ++count.culled;
                    continue;
                }
                if (behind == 224)
                {
                    ++count.allBehind;
                    continue;
                }
                if (behind != 0)
                {
                    ++count.hard;
                    continue;
                }
                // The quick path, in the program's order.
                const __m128 q0 = _mm_set1_ps(v::vuDivide(1.0f, z0)), q1 = _mm_set1_ps(v::vuDivide(1.0f, z1)), q2 = _mm_set1_ps(v::vuDivide(1.0f, z2));
                const __m128 T1 = ld(reader + 5), T2 = ld(reader + 6);
                __m128 A = sel(chk(k, _mm_mul_ps(V[0], q0), kXYW), V[0], kXYW);
                const __m128 S0 = sel(chk(k, _mm_mul_ps(T0, q0), kXYZ), T0, kXYZ);
                const auto transform = [&](__m128 r)
                {
                    // z takes w, then the viewport scale on all four lanes
                    const __m128 zw = _mm_shuffle_ps(r, r, _MM_SHUFFLE(3, 3, 1, 0));
                    return chk(k, _mm_mul_ps(zw, r10), kAll);
                };
                A = transform(A);
                __m128 B = transform(V[1]);
                B = sel(chk(k, _mm_mul_ps(B, q1), kXYZ), B, kXYZ);
                const __m128 S1 = sel(chk(k, _mm_mul_ps(T1, q1), kXYZ), T1, kXYZ);
                __m128 C = transform(V[2]);
                const __m128 a2 = chk(k, _mm_add_ps(A, r9), kXY), b2 = chk(k, _mm_add_ps(B, r9), kXY);
                C = sel(chk(k, _mm_mul_ps(C, q2), kXYZ), C, kXYZ);
                const __m128 S2 = sel(chk(k, _mm_mul_ps(T2, q2), kXYZ), T2, kXYZ);
                const __m128 c2 = chk(k, _mm_add_ps(C, r9), kXY);
                // The guard band: |x| or |y| beyond the limit, compared as the program compares (on the bits).
                const __m128i fa = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(A), kMag), limit), kXY);
                const __m128i fb = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(B), kMag), limit), kXY);
                const __m128i fc = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(C), kMag), limit), kXY);
                // The tint.
                __m128i colour[3];
                for (int c = 0; c < 3; ++c)
                {
                    const __m128 raw = ld(reader + 7 + c);
                    const __m128 f = _mm_cvtepi32_ps(_mm_castps_si128(raw));
                    const __m128 acc = chk(k, _mm_mul_ps(f, tintW), kXYZ);
                    const __m128 r = madd(k, acc, tint, one, kXYZ); // applyTint's fmadd(acc, tint, 1.0)
                    colour[c] = _mm_castps_si128(sel(_mm_castsi128_ps(ftoi(r)), raw, kXYZ));
                }
                if (k.scalar || _mm_movemask_epi8(k.special) != 0 || _mm_movemask_ps(k.tiny) != 0)
                {
                    ++count.odd;
                    ++count.hard;
                    continue;
                }
                if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(fa, fb), fc)) != 0)
                {
                    // beyond one edge with all three corners: nothing; otherwise the clipper
                    const int sa = _mm_movemask_ps(_mm_castsi128_ps(fa)), sb = _mm_movemask_ps(_mm_castsi128_ps(fb)), sc3 = _mm_movemask_ps(_mm_castsi128_ps(fc));
                    const int na = _mm_movemask_ps(A), nb = _mm_movemask_ps(B), nc = _mm_movemask_ps(C);
                    bool early = false;
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        const int bit = 1 << axis;
                        if ((sa & bit) && (sb & bit) && (sc3 & bit))
                        {
                            const bool an = (na & bit) != 0, bn = (nb & bit) != 0, cn = (nc & bit) != 0;
                            if (an == bn && bn == cn)
                                early = true;
                        }
                    }
                    if (early)
                        ++count.guardEarly;
                    else
                        ++count.hard;
                    continue;
                }
                const __m128 sixteen = _mm_set1_ps(16.0f);
                const auto position = [&](__m128 xy, __m128 zsrc, uint32_t w)
                {
                    const __m128i xyI = ftoi(_mm_mul_ps(xy, sixteen));
                    const __m128i zI = ftoi(zsrc);
                    alignas(16) uint32_t o[4], zz[4];
                    _mm_store_si128(reinterpret_cast<__m128i *>(o), xyI);
                    _mm_store_si128(reinterpret_cast<__m128i *>(zz), zI);
                    o[2] = zz[2];
                    o[3] = w;
                    return _mm_load_si128(reinterpret_cast<const __m128i *>(o));
                };
                const size_t at = out.size();
                out.resize(at + 144);
                __m128i *o = reinterpret_cast<__m128i *>(out.data() + at);
                _mm_storeu_si128(o + 0, _mm_castps_si128(S0));
                _mm_storeu_si128(o + 1, colour[0]);
                _mm_storeu_si128(o + 2, position(a2, A, tw[0]));
                _mm_storeu_si128(o + 3, _mm_castps_si128(S1));
                _mm_storeu_si128(o + 4, colour[1]);
                _mm_storeu_si128(o + 5, position(b2, B, tw[1]));
                _mm_storeu_si128(o + 6, _mm_castps_si128(S2));
                _mm_storeu_si128(o + 7, colour[2]);
                _mm_storeu_si128(o + 8, position(c2, C, tw[2]));
                ++count.quick;
            }
            count16 = cnt(reader);
        }
    }
}
