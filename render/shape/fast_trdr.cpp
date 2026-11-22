// The track program's fast path (RENDERER_P2_DESIGN.md 4.2; renderer plan P2 step 9 batch 3): TRdr_VUShapeProg entry 0
// with its common path in SSE, from tools/rc_replay/shape_lean.cpp's fastTrdr (measured there: 281.5 M quick
// triangles of the 67 dumps, nine quadwords each, equal to the reference's). See fast_run.h.
//
// The run is the reference's (trdr_shape.cpp body, vu_shape.h runTextureBlocks) step for step: the header packet, the
// constants, the register blocks and texture blocks kicked from VU memory as the program kicks them, the packet
// buffer's position (vi11) and its flush packets. Per triangle the path computes what the program computes up to the
// point where the outcomes part (culled, all behind, beyond one edge, quick, hard); a hard triangle, or one with a value
// the mode cannot treat as the VU does anywhere in it, goes to the reference's own step (rc::vu1l::trdrStep) on a
// ShapeProgram kept for the run, which holds every register lane a hard triangle reads from an earlier one (VF26,
// VF29, VF25.w: only hard triangles write them) and the packet buffer's position.
#define RC_VU1_LEAN 1
#include "vu_shape.h"

#include "fast_run.h"

#include <emmintrin.h>
#include <smmintrin.h>

#include <cmath>
#include <cstring>

namespace rc::shape
{
    namespace v = rc::vu1l;

    namespace
    {
        // The checks of 4.1's third column, accumulated over a triangle and tested once.
        struct Chk
        {
            __m128i special = _mm_setzero_si128(); // an infinity or NaN result (an infinity or NaN operand gives one)
            __m128i madd = _mm_setzero_si128();    // a MADD the mode cannot give (vu_lean.h fmadd's three fall-backs)
            bool scalar = false;
            bool any() const { return scalar || _mm_movemask_epi8(_mm_or_si128(special, madd)) != 0; }
        };
        const __m128i kExp = _mm_set1_epi32(0x7F800000);
        const __m128i kMag = _mm_set1_epi32(0x7FFFFFFF);
        inline __m128i lanes(int mask) { return _mm_set_epi32((mask & 8) ? -1 : 0, (mask & 4) ? -1 : 0, (mask & 2) ? -1 : 0, (mask & 1) ? -1 : 0); }
        inline __m128 chk(Chk &k, __m128 r, __m128i mask)
        {
            k.special = _mm_or_si128(k.special, _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(r), kExp), kExp), mask));
            return r;
        }
        // acc + a * b with the product rounded first (the VU's MADD), on the lanes of `mask`. Left to the reference
        // (vu_lean.h fmadd): a zero product of two non-zero factors (it was tiny: the reference keeps it for the sum); a
        // product of the largest magnitude (it may have overflowed: the reference clamps the sum); a sum below 2^-101
        // from a non-zero accumulator (the reference takes the sign and the flush from the exact sum: batch 1's third
        // fall-back).
        inline __m128 madd(Chk &k, __m128 acc, __m128 a, __m128 b, __m128i mask)
        {
            const __m128i zero = _mm_setzero_si128();
            const __m128 p = _mm_mul_ps(a, b);
            const __m128i pm = _mm_and_si128(_mm_castps_si128(p), kMag);
            const __m128i factorZero = _mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(a), kMag), zero),
                                                    _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(b), kMag), zero));
            const __m128i tinyProduct = _mm_andnot_si128(factorZero, _mm_cmpeq_epi32(pm, zero));
            const __m128i bigProduct = _mm_cmpgt_epi32(pm, _mm_set1_epi32(0x7F7FFFFE));
            const __m128 r = _mm_add_ps(acc, p);
            const __m128i tinySum = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(acc), kMag), zero),
                                                     _mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(r), kMag), _mm_set1_epi32(0x0D000000)));
            k.madd = _mm_or_si128(k.madd, _mm_and_si128(_mm_or_si128(_mm_or_si128(tinyProduct, bigProduct), tinySum), mask));
            return chk(k, r, mask);
        }
        // Planted fault Fused: the product not rounded before the sum (the sum of the exact values, rounded once).
        inline __m128 maddFused(Chk &k, __m128 acc, __m128 a, __m128 b, __m128i mask)
        {
            alignas(16) float fa[4], fb[4], fc[4], out[4];
            _mm_store_ps(fa, acc);
            _mm_store_ps(fb, a);
            _mm_store_ps(fc, b);
            for (int i = 0; i < 4; ++i)
                out[i] = static_cast<float>(static_cast<double>(fa[i]) + static_cast<double>(fb[i]) * static_cast<double>(fc[i]));
            return chk(k, _mm_load_ps(out), mask);
        }
        inline float sc(Chk &k, float r)
        {
            if (v::special(r))
                k.scalar = true;
            return r;
        }
        // lanes of `a` where the mask is set, of `b` elsewhere
        inline __m128 sel(__m128 a, __m128 b, __m128i mask) { return _mm_or_ps(_mm_and_ps(_mm_castsi128_ps(mask), a), _mm_andnot_ps(_mm_castsi128_ps(mask), b)); }
        // ftoiRaw: truncation, 0x7FFFFFFF from 2^31 up (the host's conversion gives 0x80000000 below -2^31, as the VU)
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
        inline __m128 ld(const uint8_t *mem, int quad) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + (quad & 0x3FF) * 16)); }
        inline int16_t count(const uint8_t *mem, int quad)
        {
            int16_t c;
            std::memcpy(&c, mem + (quad & 0x3FF) * 16 + 8, 2);
            return c;
        }

        // A GIF tag that makes the packet at its quadword exactly a triangle packet of 160 bytes: one PACKED tag, NLOOP 3,
        // NREG 3, EOP, no PRE (the PRIM field is then ignored, as the GIF reader ignores it), REGS ST RGBAQ XYZ2.
        constexpr uint64_t kTagFields = 0x7FFFull | (1ull << 15) | (1ull << 46) | (3ull << 58) | (15ull << 60);
        constexpr uint64_t kTriangleTag = 3ull | (1ull << 15) | (3ull << 60);
        constexpr uint64_t kTriangleRegs = 0x512u;
        // A tag with EOP and NLOOP 0: the packet at it is the tag alone (16 bytes).
        constexpr uint64_t kFlushFields = 0x7FFFull | (1ull << 15);
        constexpr uint64_t kFlushTag = 1ull << 15;

        void onKick(void *user, const uint8_t *data, uint32_t bytes) { static_cast<FastSink *>(user)->packet(data, bytes); }
    }

    bool parseFastFault(const char *name, FastFault &out)
    {
        for (uint32_t i = 1; i < static_cast<uint32_t>(FastFault::Count); ++i)
            if (std::strcmp(name, fastFaultName(static_cast<FastFault>(i))) == 0)
            {
                out = static_cast<FastFault>(i);
                return true;
            }
        return false;
    }

    const char *fastFaultName(FastFault f)
    {
        static const char *const names[] = {"none", "bias", "fused", "round-x", "edge-two", "hard-quick"};
        return static_cast<uint32_t>(f) < static_cast<uint32_t>(FastFault::Count) ? names[static_cast<uint32_t>(f)] : "?";
    }

    bool runTrdr(uint8_t *mem, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &st, const FastOptions &opt)
    {
        // 4.3: the count walk must stay within the input buffer, or the run is the reference's.
        const int start = static_cast<int>(top & 0x3FFu);
        {
            int r = start;
            int c = count(mem, r);
            for (;;)
            {
                while (c < 0)
                {
                    r += 1 - c;
                    if (r - start >= static_cast<int>(kMaxRunInput))
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
                if (r - start >= static_cast<int>(kMaxRunInput))
                {
                    ++st.declinedParse;
                    return false;
                }
                c = count(mem, r);
            }
            if (static_cast<uint32_t>(r - start + 1) > st.maxExtent)
                st.maxExtent = static_cast<uint32_t>(r - start + 1);
        }
        ++st.runs;

        const v::ScopedMode mode; // the caller's MXCSR (round toward zero) with FTZ and DAZ, restored on return or a throw
        const v::ShapeLayout &layout = v::trdrLayout();
        v::ShapeProgram p(mem, vf, layout, &onKick, &sink);

        // Header packet, then the constants (trdr_shape.cpp body).
        p.kick(247);
        p.loadConstants();

        const auto vec = [&](int reg) { return _mm_loadu_ps(p.r[reg].f); };
        const __m128 m1 = vec(1), m2 = vec(2), m3 = vec(3), m4 = vec(4), r9 = vec(9), r10 = vec(10), r21 = vec(21);
        const __m128 tint = ld(mem, 11); // applyTint's quadword: nothing in a run writes it
        const __m128i kXYZ = lanes(7), kXY = lanes(3), kXYW = lanes(11), kAll = lanes(15), kZ = lanes(4);
        const __m128 one = _mm_set1_ps(1.0f), tintW = _mm_shuffle_ps(tint, tint, 0xFF);
        const float nearNeg = v::fsub(0.0f, p.r[23].f[2]).value; // setupTriangle's nearTest
        const uint32_t wBits = v::bits(p.r[9].f[3]);
        const __m128i limit = _mm_set1_epi32((wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF); // clip()
        const uint32_t tw[3] = {v::bits(p.r[layout.quickTemp[0]].f[3]), v::bits(p.r[layout.quickTemp[1]].f[3]), v::bits(p.r[layout.quickTemp[2]].f[3])};
        const __m128 sixteen = _mm_set1_ps(16.0f);
        const bool fBias = opt.fault == FastFault::Bias, fFused = opt.fault == FastFault::Fused, fRound = opt.fault == FastFault::RoundX,
                   fEdge = opt.fault == FastFault::EdgeTwo, fHardQuick = opt.fault == FastFault::HardQuick;

        const auto flush = [&]
        {
            // The flush packet: the tag alone when it is one with EOP and NLOOP 0 (always, in the dumps), else as kicked.
            uint64_t lo;
            std::memcpy(&lo, mem + (layout.flushQuad & 0x3FF) * 16, 8);
            if ((lo & kFlushFields) == kFlushTag)
                sink.packet(mem + (layout.flushQuad & 0x3FF) * 16, 16u);
            else
                p.kick(layout.flushQuad);
        };

        const auto position = [&](__m128 xy, __m128 zsrc, uint32_t w)
        {
            const __m128 scaled = _mm_mul_ps(xy, sixteen);
            __m128i xyI = ftoi(scaled);
            if (fRound)
            {
                const __m128i rounded = ftoi(_mm_round_ps(scaled, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
                xyI = _mm_castps_si128(sel(_mm_castsi128_ps(rounded), _mm_castsi128_ps(xyI), lanes(1)));
            }
            const __m128i zI = ftoi(zsrc);
            alignas(16) uint32_t o[4], zz[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(o), xyI);
            _mm_store_si128(reinterpret_cast<__m128i *>(zz), zI);
            o[2] = zz[2];
            o[3] = w;
            return _mm_load_si128(reinterpret_cast<const __m128i *>(o));
        };

        // One triangle at `reader`: true when the path drew it or found that it draws nothing; false: the step's.
        const auto fast = [&](int reader) -> bool
        {
            ++st.in;
            Chk k;
            const __m128 N = ld(mem, reader), T0 = ld(mem, reader + 4);
            const __m128 P[3] = {ld(mem, reader + 1), ld(mem, reader + 2), ld(mem, reader + 3)};
            // setupTriangle: the facing test's products, the corners through the matrix, the depth bias
            const __m128 rel = chk(k, _mm_sub_ps(P[0], r21), kXYZ);
            const __m128 d = chk(k, _mm_mul_ps(rel, N), kXYZ);
            const __m128 bias = fBias ? _mm_setzero_ps() : _mm_and_ps(_mm_castsi128_ps(kZ), _mm_shuffle_ps(T0, T0, 0xFF));
            __m128 V[3];
            for (int c = 0; c < 3; ++c)
            {
                const __m128 x = _mm_shuffle_ps(P[c], P[c], 0x00), y = _mm_shuffle_ps(P[c], P[c], 0x55), z = _mm_shuffle_ps(P[c], P[c], 0xAA),
                             w = _mm_shuffle_ps(P[c], P[c], 0xFF);
                __m128 acc = chk(k, _mm_mul_ps(m1, x), kAll);
                acc = madd(k, acc, m2, y, kAll);
                acc = fFused ? maddFused(k, acc, m3, z, kAll) : madd(k, acc, m3, z, kAll);
                acc = madd(k, acc, m4, w, kAll);
                V[c] = sel(chk(k, _mm_add_ps(acc, bias), kZ), acc, kZ); // the bias on z only
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
            if (behind == 224)
            {
                ++st.allBehind;
                return true;
            }
            if (behind != 0 && !fHardQuick)
                return false;

            // writeQuickTriangle, in the program's order.
            const __m128 q0 = _mm_set1_ps(v::vuDivide(1.0f, z0)), q1 = _mm_set1_ps(v::vuDivide(1.0f, z1)), q2 = _mm_set1_ps(v::vuDivide(1.0f, z2));
            const __m128 T1 = ld(mem, reader + 5), T2 = ld(mem, reader + 6);
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
            // clip(): |x| or |y| beyond the limit, compared on the bits as the program compares them
            const __m128i fa = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(A), kMag), limit), kXY);
            const __m128i fb = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(B), kMag), limit), kXY);
            const __m128i fc = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(C), kMag), limit), kXY);
            // applyTint
            __m128i colour[3];
            for (int c = 0; c < 3; ++c)
            {
                const __m128 raw = ld(mem, reader + 7 + c);
                const __m128 f = _mm_cvtepi32_ps(_mm_castps_si128(raw));
                const __m128 acc = chk(k, _mm_mul_ps(f, tintW), kXYZ);
                const __m128 r = madd(k, acc, tint, one, kXYZ);
                colour[c] = _mm_castps_si128(sel(_mm_castsi128_ps(ftoi(r)), raw, kXYZ));
            }
            if (k.any())
            {
                ++st.odd;
                return false;
            }
            if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(fa, fb), fc)) != 0)
            {
                // scissor's first test: all three corners beyond the same edge draw nothing; anything else is clipped
                const int sa = _mm_movemask_ps(_mm_castsi128_ps(fa)), sb = _mm_movemask_ps(_mm_castsi128_ps(fb)), sc3 = _mm_movemask_ps(_mm_castsi128_ps(fc));
                const int na = _mm_movemask_ps(A), nb = _mm_movemask_ps(B), nc = _mm_movemask_ps(C);
                for (int axis = 0; axis < 2; ++axis)
                {
                    const int bit = 1 << axis;
                    const bool an = (na & bit) != 0, bn = (nb & bit) != 0, cn = (nc & bit) != 0;
                    if (fEdge ? ((sa & bit) && (sb & bit) && an == bn) : ((sa & bit) && (sb & bit) && (sc3 & bit) && an == bn && bn == cn))
                    {
                        ++st.edge;
                        return true;
                    }
                }
                return false;
            }

            // The packet: the slot's tag (from the shape's packet, in VU memory) and the nine quadwords.
            alignas(16) uint32_t nine[36];
            __m128i *o = reinterpret_cast<__m128i *>(nine);
            _mm_store_si128(o + 0, _mm_castps_si128(S0));
            _mm_store_si128(o + 1, colour[0]);
            _mm_store_si128(o + 2, position(a2, A, tw[0]));
            _mm_store_si128(o + 3, _mm_castps_si128(S1));
            _mm_store_si128(o + 4, colour[1]);
            _mm_store_si128(o + 5, position(b2, B, tw[1]));
            _mm_store_si128(o + 6, _mm_castps_si128(S2));
            _mm_store_si128(o + 7, colour[2]);
            _mm_store_si128(o + 8, position(c2, C, tw[2]));
            ++st.quick;
            const int slot = p.vi11;
            const uint8_t *tag = mem + ((slot - 1) & 0x3FF) * 16;
            uint64_t lo, hi;
            std::memcpy(&lo, tag, 8);
            std::memcpy(&hi, tag + 8, 8);
            if ((lo & kTagFields) == kTriangleTag && (hi & 0xFFFu) == kTriangleRegs)
            {
                if (sink.triangle(nine))
                    ++st.direct;
                else
                {
                    alignas(16) uint8_t packet[160];
                    std::memcpy(packet, tag, 16);
                    std::memcpy(packet + 16, nine, sizeof(nine));
                    sink.packet(packet, 160u);
                    ++st.rebuilt;
                }
            }
            else
            {
                // Not the triangle tag: the packet as the program sends it, from VU memory.
                for (int i = 0; i < 9; ++i)
                    std::memcpy(mem + ((slot + i) & 0x3FF) * 16, nine + 4 * i, 16);
                p.kick(slot - 1);
                ++st.rebuilt;
            }
            p.vi11 += 10;
            if (p.vi11 == layout.wrapAt)
            {
                p.vi11 = layout.firstVertex;
                flush(); // QuickWrap::Flush: after the triangle
            }
            return true;
        };

        // runTextureBlocks
        int reader = start;
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
                    v::trdrStep(p, reader);
                }
            n = count(mem, reader);
        }
        std::memcpy(vf, p.r, sizeof(p.r));
        return true;
    }
}
