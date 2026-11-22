// The skin program's fast path (RENDERER_P2_DESIGN.md 4.2; renderer plan P2 step 9 batch 4): Rdr_VUSkinProg. Each
// triangle is sixteen quadwords: the usual ten, then six giving each corner two bone matrices (by their VU memory
// addresses) and a blend. The six bone transforms and the blend (skin_shape.cpp loadAndTransform) in SSE in front of the
// track program's quick path; no facing test, no tint, no depth bias, no flush on the quick path's wrap. The quick path's
// scratch registers are VF01-VF03, whose w lanes (the last bone matrix's) reach the packet.
//
// The program writes every register it writes for every triangle it reads (VF01-VF04, VF11-VF20, VF24-VF30: VF25.w,
// VF26 and VF29 among them), so the run's last triangle is handed to the reference's step (rc::vu1l::skinStep), which
// leaves all of them as the program does; hard and odd triangles go to the same step.
#define RC_VU1_LEAN 1
#include "fast_simd.h"

namespace rc::shape
{
    using namespace detail;

    bool runSkin(uint8_t *mem, float (*vf)[4], FastSink &sink, FastStats &stats, const FastOptions &opt)
    {
        FastCounts &st = stats.by[v::kSkin];
        // 4.3: the walk from quadword 512 (the program's fixed input) must stay in VU memory; a block with a count that is
        // not positive would make the program's do-while run 65,536 times less that: the reference's.
        const int start = static_cast<int>(kSkinInput);
        int lastTri = -1;
        {
            int r = start;
            for (;;)
            {
                const int c = count(mem, r);
                if (c <= 0)
                {
                    ++st.declinedParse;
                    return false;
                }
                r += 1;
                lastTri = r + 16 * (c - 1);
                r += 16 * c;
                if (r - start >= static_cast<int>(kMaxSkinInput))
                {
                    ++st.declinedParse;
                    return false;
                }
                if (count(mem, r) == 0)
                    break;
            }
            if (static_cast<uint32_t>(r - start + 1) > stats.maxExtent)
                stats.maxExtent = static_cast<uint32_t>(r - start + 1);
        }
        ++st.runs;

        const v::ScopedMode mode; // the caller's MXCSR (round toward zero) with FTZ and DAZ, restored on return or a throw
        v::ShapeProgram p(mem, vf, v::skinLayout(), &onKick, &sink);
        v::skinPrologue(p);

        const auto vec = [&](int reg) { return _mm_loadu_ps(p.r[reg].f); };
        const __m128i kXYZ = lanes(7), kAll = lanes(15);
        // loadAndTransform's first three lines: VF08.xyz takes the near test (the view matrix's last row, which the
        // corners' w of zero then cancels in every lane but the sign of a zero).
        const float nearNeg = v::fsub(0.0f, p.r[23].f[2]).value;
        const __m128 M5[4] = {vec(5), vec(6), vec(7), _mm_set_ps(p.r[8].f[3], nearNeg, nearNeg, nearNeg)};
        const __m128 C12 = ld(mem, 12); // nothing in a run writes it
        QuickCtx qc;
        qc.r9 = vec(9);
        qc.r10 = vec(10);
        {
            const uint32_t wBits = v::bits(p.r[9].f[3]);
            qc.limit = _mm_set1_epi32((wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF); // clip()
        }
        qc.roundX = faultOn(opt, FastFault::RoundX, v::kSkin);
        qc.edgeTwo = faultOn(opt, FastFault::EdgeTwo, v::kSkin);
        const bool fFused = faultOn(opt, FastFault::Fused, v::kSkin), fHardQuick = faultOn(opt, FastFault::HardQuick, v::kSkin),
                   fLast = faultOn(opt, FastFault::LastStep, v::kSkin);

        const auto matrix = [&](int address, __m128 m[4])
        {
            for (int i = 0; i < 4; ++i)
                m[i] = ld(mem, address + i);
        };

        // One triangle at `reader`: true when the path drew it or found that it draws nothing; false: the step's.
        const auto fast = [&](int reader) -> bool
        {
            ++st.in;
            Chk k;
            const int w = reader + 10;
            __m128 m[4];
            // Each corner: the second position through one bone (xyz: its w reaches only VF28-VF30.w, which the run's
            // last step leaves right), the first through the other.
            matrix(u16(mem, w, 3), m);
            __m128 r28 = transform4(k, m, ld(mem, w + 1), kXYZ);
            matrix(u16(mem, w, 2), m);
            __m128 r11 = transform4(k, m, ld(mem, reader + 1), kAll);
            matrix(u16(mem, w + 2, 3), m);
            __m128 r29 = transform4(k, m, ld(mem, w + 3), kXYZ);
            matrix(u16(mem, w + 2, 2), m);
            __m128 r12 = transform4(k, m, ld(mem, reader + 2), kAll);
            matrix(u16(mem, w + 4, 3), m);
            __m128 r30 = transform4(k, m, ld(mem, w + 5), kXYZ);
            matrix(u16(mem, w + 4, 2), m);
            __m128 r13 = transform4(k, m, ld(mem, reader + 3), kAll);
            // The quick path's scratch registers VF01-VF03 hold that last matrix's rows; their w lanes reach the packet.
            const uint32_t tw[3] = {v::bits(lane(m[0], 3)), v::bits(lane(m[1], 3)), v::bits(lane(m[2], 3))};
            // The blend: each by its weight (xyz), summed, less the offset of VU memory 12 (all four lanes).
            const __m128 w0 = ld(mem, w), w2 = ld(mem, w + 2), w4 = ld(mem, w + 4);
            r11 = sel(chk(k, _mm_mul_ps(r11, splat(w0, 0)), kXYZ), r11, kXYZ);
            r28 = chk(k, _mm_mul_ps(r28, splat(w0, 1)), kXYZ);
            r12 = sel(chk(k, _mm_mul_ps(r12, splat(w2, 0)), kXYZ), r12, kXYZ);
            r29 = chk(k, _mm_mul_ps(r29, splat(w2, 1)), kXYZ);
            r13 = sel(chk(k, _mm_mul_ps(r13, splat(w4, 0)), kXYZ), r13, kXYZ);
            r30 = chk(k, _mm_mul_ps(r30, splat(w4, 1)), kXYZ);
            r11 = sel(chk(k, _mm_add_ps(r11, r28), kXYZ), r11, kXYZ);
            r12 = sel(chk(k, _mm_add_ps(r12, r29), kXYZ), r12, kXYZ);
            r13 = sel(chk(k, _mm_add_ps(r13, r30), kXYZ), r13, kXYZ);
            r11 = chk(k, _mm_sub_ps(r11, C12), kAll);
            r12 = chk(k, _mm_sub_ps(r12, C12), kAll);
            r13 = chk(k, _mm_sub_ps(r13, C12), kAll);
            // Through the view matrix.
            const __m128 V[3] = {transform4(k, M5, r11, kAll, fFused), transform4(k, M5, r12, kAll, fFused), transform4(k, M5, r13, kAll, fFused)};
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
            if (behind == 224)
            {
                ++st.allBehind;
                return true;
            }
            if (behind != 0 && !fHardQuick)
                return false;

            const __m128 T[3] = {ld(mem, reader + 4), ld(mem, reader + 5), ld(mem, reader + 6)};
            const __m128i colour[3] = {_mm_castps_si128(ld(mem, reader + 7)), _mm_castps_si128(ld(mem, reader + 8)), _mm_castps_si128(ld(mem, reader + 9))};
            alignas(16) uint32_t nine[36];
            switch (quickTriangle(k, V, T, colour, tw, qc, nullptr, nine))
            {
            case Quick::Odd: ++st.odd; return false;
            case Quick::Hard: return false;
            case Quick::Edge: ++st.edge; return true;
            case Quick::Drawn: break;
            }
            emitQuick(p, sink, st, nine);
            return true;
        };

        // The program's loop (skin_shape.cpp body).
        int reader = start;
        for (;;)
        {
            p.r[31] = p.load(reader);
            p.store(4, p.r[31]);
            p.kick(0);
            int n = count(mem, reader);
            ++reader;
            p.vi11 = p.layout.firstVertex;
            ++st.blocks;
            for (; n != 0; --n, reader += 16)
            {
                if (reader == lastTri && !fLast)
                {
                    // The run's last triangle: its step leaves every register as the program does.
                    ++st.in;
                    ++st.lastSteps;
                    v::skinStep(p, reader);
                }
                else if (!fast(reader))
                {
                    ++st.hard;
                    v::skinStep(p, reader);
                }
            }
            if (count(mem, reader) == 0)
                break;
        }
        std::memcpy(vf, p.r, sizeof(p.r));
        return true;
    }
}
