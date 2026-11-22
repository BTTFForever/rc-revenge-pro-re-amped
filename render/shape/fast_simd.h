#pragma once

// What the fast paths share (RENDERER_P2_DESIGN.md 4.2; renderer plan P2 step 9 batches 3 and 4): the SSE operations
// with the checks of 4.1's third column, the quick path of writeQuickTriangle (vu_shape.cpp) on three transformed
// corners, and the triangle packet's way out (the slot's tag, FastSink::triangle, the packet buffer's position and its
// flush). Included by the paths' sources after `#define RC_VU1_LEAN 1` and vu_shape.h; nothing here is a runtime's.

#ifndef RC_VU1_LEAN
#error "fast_simd.h is included by the fast paths after #define RC_VU1_LEAN 1"
#endif
#include "vu_shape.h"
#include "vu1_ref.h"

#include "fast_run.h"

#include <emmintrin.h>
#include <smmintrin.h>

#include <cmath>
#include <cstring>

namespace rc::shape::detail
{
    namespace v = rc::vu1l;

    // The checks of 4.1's third column, accumulated over a triangle and tested once.
    struct Chk
    {
        __m128i special = _mm_setzero_si128(); // an infinity or NaN result (an infinity or NaN operand gives one)
        __m128i madd = _mm_setzero_si128();    // a MADD the mode cannot give (vu_lean.h fmadd's three fall-backs)
        bool scalar = false;
        bool any() const { return scalar || _mm_movemask_epi8(_mm_or_si128(special, madd)) != 0; }
    };
    inline __m128i kExp() { return _mm_set1_epi32(0x7F800000); }
    inline __m128i kMag() { return _mm_set1_epi32(0x7FFFFFFF); }
    inline __m128i lanes(int mask) { return _mm_set_epi32((mask & 8) ? -1 : 0, (mask & 4) ? -1 : 0, (mask & 2) ? -1 : 0, (mask & 1) ? -1 : 0); }
    inline __m128 chk(Chk &k, __m128 r, __m128i mask)
    {
        k.special = _mm_or_si128(k.special, _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(r), kExp()), kExp()), mask));
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
        const __m128i pm = _mm_and_si128(_mm_castps_si128(p), kMag());
        const __m128i factorZero = _mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(a), kMag()), zero),
                                                _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(b), kMag()), zero));
        const __m128i tinyProduct = _mm_andnot_si128(factorZero, _mm_cmpeq_epi32(pm, zero));
        const __m128i bigProduct = _mm_cmpgt_epi32(pm, _mm_set1_epi32(0x7F7FFFFE));
        const __m128 r = _mm_add_ps(acc, p);
        const __m128i tinySum = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(acc), kMag()), zero),
                                                 _mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(r), kMag()), _mm_set1_epi32(0x0D000000)));
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
    inline __m128 splat(__m128 x, int i)
    {
        switch (i)
        {
        case 0: return _mm_shuffle_ps(x, x, 0x00);
        case 1: return _mm_shuffle_ps(x, x, 0x55);
        case 2: return _mm_shuffle_ps(x, x, 0xAA);
        default: return _mm_shuffle_ps(x, x, 0xFF);
        }
    }
    inline __m128 ld(const uint8_t *mem, int quad) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + (quad & 0x3FF) * 16)); }
    inline int16_t count(const uint8_t *mem, int quad)
    {
        int16_t c;
        std::memcpy(&c, mem + (quad & 0x3FF) * 16 + 8, 2);
        return c;
    }
    inline uint16_t u16(const uint8_t *mem, int quad, int lane)
    {
        uint16_t c;
        std::memcpy(&c, mem + (quad & 0x3FF) * 16 + lane * 4, 2);
        return c;
    }
    // A matrix (four rows) times a vector, as the programs' MULx / MADDy / MADDz / MADDw chain (setupTriangle's corner
    // transform, the skin's transformBy): row 0 times x, then the others added in order. `fusedZ`: the planted fault.
    inline __m128 transform4(Chk &k, const __m128 m[4], __m128 x, __m128i mask, bool fusedZ = false)
    {
        __m128 acc = chk(k, _mm_mul_ps(m[0], splat(x, 0)), mask);
        acc = madd(k, acc, m[1], splat(x, 1), mask);
        acc = fusedZ ? maddFused(k, acc, m[2], splat(x, 2), mask) : madd(k, acc, m[2], splat(x, 2), mask);
        return madd(k, acc, m[3], splat(x, 3), mask);
    }

    // A GIF tag that makes the packet at its quadword exactly a triangle packet of 160 bytes: one PACKED tag, NLOOP 3,
    // NREG 3, EOP, no PRE (the PRIM field is then ignored, as the GIF reader ignores it), REGS ST RGBAQ XYZ2.
    constexpr uint64_t kTagFields = 0x7FFFull | (1ull << 15) | (1ull << 46) | (3ull << 58) | (15ull << 60);
    constexpr uint64_t kTriangleTag = 3ull | (1ull << 15) | (3ull << 60);
    constexpr uint64_t kTriangleRegs = 0x512u;
    // A tag with EOP and NLOOP 0: the packet at it is the tag alone (16 bytes).
    constexpr uint64_t kFlushFields = 0x7FFFull | (1ull << 15);
    constexpr uint64_t kFlushTag = 1ull << 15;

    inline void onKick(void *user, const uint8_t *data, uint32_t bytes) { static_cast<FastSink *>(user)->packet(data, bytes); }

    // What the quick path reads beside the triangle: constant for a run.
    struct QuickCtx
    {
        __m128 r9, r10;   // the screen offset, the viewport scale
        __m128i limit;    // clip()'s guard-band limit, as an integer of the bits
        bool roundX = false, edgeTwo = false; // planted faults
    };

    enum class Quick
    {
        Drawn, // `nine` holds the packet's nine quadwords
        Edge,  // all three corners beyond one guard-band edge: nothing drawn
        Hard,  // a guard-band flag that is not "beyond one edge": the step's
        Odd    // a value the mode cannot treat as the VU does: the step's
    };

    // writeQuickTriangle on the three corners setupTriangle left (V: transformed, with setupTriangle's bias where the
    // program has one; T: the texture registers as the program holds them; colour: the colour registers as stored),
    // the quick path's three scratch registers' w lanes (`tw`), and `zBias` (TERdr: the bias the quick path adds to z,
    // or null). In the program's order.
    inline Quick quickTriangle(Chk &k, const __m128 V[3], const __m128 T[3], const __m128i colour[3], const uint32_t tw[3], const QuickCtx &c,
                               const float *zBias, uint32_t nine[36])
    {
        const __m128i kXYZ = lanes(7), kXY = lanes(3), kXYW = lanes(11), kAll = lanes(15), kZ = lanes(4);
        const float z0 = lane(V[0], 2), z1 = lane(V[1], 2), z2 = lane(V[2], 2);
        const __m128 q0 = _mm_set1_ps(v::vuDivide(1.0f, z0)), q1 = _mm_set1_ps(v::vuDivide(1.0f, z1)), q2 = _mm_set1_ps(v::vuDivide(1.0f, z2));
        __m128 A = sel(chk(k, _mm_mul_ps(V[0], q0), kXYW), V[0], kXYW);
        const __m128 S0 = sel(chk(k, _mm_mul_ps(T[0], q0), kXYZ), T[0], kXYZ);
        const auto transform = [&](__m128 r)
        {
            // z takes w, then the viewport scale on all four lanes
            const __m128 zw = _mm_shuffle_ps(r, r, _MM_SHUFFLE(3, 3, 1, 0));
            return chk(k, _mm_mul_ps(zw, c.r10), kAll);
        };
        A = transform(A);
        __m128 B = transform(V[1]);
        B = sel(chk(k, _mm_mul_ps(B, q1), kXYZ), B, kXYZ);
        const __m128 S1 = sel(chk(k, _mm_mul_ps(T[1], q1), kXYZ), T[1], kXYZ);
        __m128 C = transform(V[2]);
        const __m128 a2 = chk(k, _mm_add_ps(A, c.r9), kXY), b2 = chk(k, _mm_add_ps(B, c.r9), kXY);
        C = sel(chk(k, _mm_mul_ps(C, q2), kXYZ), C, kXYZ);
        const __m128 S2 = sel(chk(k, _mm_mul_ps(T[2], q2), kXYZ), T[2], kXYZ);
        const __m128 c2 = chk(k, _mm_add_ps(C, c.r9), kXY);
        // clip(): |x| or |y| beyond the limit, compared on the bits as the program compares them
        const __m128i fa = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(A), kMag()), c.limit), kXY);
        const __m128i fb = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(B), kMag()), c.limit), kXY);
        const __m128i fc = _mm_and_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(C), kMag()), c.limit), kXY);
        // TERdr: the bias, after the clip flags, on the three corners' z
        __m128 zA = A, zB = B, zC = C;
        if (zBias)
        {
            const __m128 bias = _mm_set1_ps(*zBias);
            zA = sel(chk(k, _mm_add_ps(A, bias), kZ), A, kZ);
            zB = sel(chk(k, _mm_add_ps(B, bias), kZ), B, kZ);
            zC = sel(chk(k, _mm_add_ps(C, bias), kZ), C, kZ);
        }
        if (k.any())
            return Quick::Odd;
        if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(fa, fb), fc)) != 0)
        {
            // scissor's first test: all three corners beyond the same edge draw nothing; anything else is clipped
            const int sa = _mm_movemask_ps(_mm_castsi128_ps(fa)), sb = _mm_movemask_ps(_mm_castsi128_ps(fb)), sc3 = _mm_movemask_ps(_mm_castsi128_ps(fc));
            const int na = _mm_movemask_ps(A), nb = _mm_movemask_ps(B), nc = _mm_movemask_ps(C);
            for (int axis = 0; axis < 2; ++axis)
            {
                const int bit = 1 << axis;
                const bool an = (na & bit) != 0, bn = (nb & bit) != 0, cn = (nc & bit) != 0;
                if (c.edgeTwo ? ((sa & bit) && (sb & bit) && an == bn) : ((sa & bit) && (sb & bit) && (sc3 & bit) && an == bn && bn == cn))
                    return Quick::Edge;
            }
            return Quick::Hard;
        }

        const __m128 sixteen = _mm_set1_ps(16.0f);
        const auto position = [&](__m128 xy, __m128 zsrc, uint32_t w)
        {
            const __m128 scaled = _mm_mul_ps(xy, sixteen);
            __m128i xyI = ftoi(scaled);
            if (c.roundX)
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
        __m128i *o = reinterpret_cast<__m128i *>(nine);
        _mm_store_si128(o + 0, _mm_castps_si128(S0));
        _mm_store_si128(o + 1, colour[0]);
        _mm_store_si128(o + 2, position(a2, zA, tw[0]));
        _mm_store_si128(o + 3, _mm_castps_si128(S1));
        _mm_store_si128(o + 4, colour[1]);
        _mm_store_si128(o + 5, position(b2, zB, tw[1]));
        _mm_store_si128(o + 6, _mm_castps_si128(S2));
        _mm_store_si128(o + 7, colour[2]);
        _mm_store_si128(o + 8, position(c2, zC, tw[2]));
        return Quick::Drawn;
    }

    // The flush packet: the tag alone when it is one with EOP and NLOOP 0 (always, in the dumps), else as kicked.
    inline void flushPacket(v::ShapeProgram &p, FastSink &sink)
    {
        const int at = p.layout.flushQuad & 0x3FF;
        uint64_t lo;
        std::memcpy(&lo, p.mem + at * 16, 8);
        if ((lo & kFlushFields) == kFlushTag)
            sink.packet(p.mem + at * 16, 16u);
        else
            p.kick(p.layout.flushQuad);
    }

    // The quick triangle's packet: the slot's tag (from the shape's packet, in VU memory) and the nine quadwords; then
    // the packet buffer's position, its wrap and (QuickWrap::Flush) the flush after the triangle.
    inline void emitQuick(v::ShapeProgram &p, FastSink &sink, FastCounts &st, const uint32_t nine[36])
    {
        uint8_t *mem = p.mem;
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
                std::memcpy(packet + 16, nine, 144);
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
        if (p.vi11 == p.layout.wrapAt)
        {
            p.vi11 = p.layout.firstVertex;
            if (p.layout.quickWrap == v::QuickWrap::Flush)
                flushPacket(p, sink); // after the triangle
        }
    }

    inline bool faultOn(const FastOptions &o, FastFault f, uint32_t program) { return o.fault == f && (o.faultPrograms & (1u << program)) != 0; }
}
