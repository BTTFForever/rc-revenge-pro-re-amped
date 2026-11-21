// rc_vu1_test: check (s) of RENDERER_P2_DESIGN.md section 9, the lean build's four operations (decomp/vu1/vu_lean.h)
// against the strict ones (vu_shape.h), each side under its own MXCSR: strict round toward zero, lean round toward
// zero + flush to zero + denormals are zero (vu1l::ScopedMode, as rc::vu1l::run sets it). Operands: every triple of a
// set of edge values (zeros, the denormal range's ends, the smallest and largest normals, one, infinities, NaNs, and
// factors whose products land on either side of the denormal and overflow limits), then random bit patterns weighted
// toward denormals, the smallest and largest binades, zeros, infinities and NaNs (shape_lean.cpp's generator, where the
// first two MADD fall-backs were found), then MADD triples whose sum cancels or lands on the smallest normal (the
// third: an exact cancellation after an inexact product, found by the edge triples). Also checks that rc::vu1l::run restores the caller's MXCSR.
//   rc_vu1_test [--results N] [--seed S]     default 10^8 random results (N / 4 operand triples) and N / 4 cancelling
//                                            results; exit 1 if any differ
// The strict operations in this program are vu_shape.h's text compiled under RC_VU1_LEAN (faddStrict ...), which is
// the text of the strict build's fadd ..., under the strict build's flags.

#define RC_VU1_LEAN 1
#include "vu_shape.h"
#include "vu1_ref.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <xmmintrin.h>

namespace v = rc::vu1l;

namespace
{
    struct Counts
    {
        uint64_t results = 0, bad[4] = {};
    };
    const char *const kOpName[4] = {"ADD", "SUB", "MUL", "MADD"};

    void compare(float a, float b, float c, uint32_t strictCsr, uint32_t leanCsr, Counts &n)
    {
        v::Fmac r[4], l[4];
        _mm_setcsr(strictCsr);
        r[0] = v::faddStrict(a, b);
        r[1] = v::fsubStrict(a, b);
        r[2] = v::fmulStrict(a, b);
        r[3] = v::fmaddStrict(c, a, b);
        _mm_setcsr(leanCsr);
        l[0] = v::fadd(a, b);
        l[1] = v::fsub(a, b);
        l[2] = v::fmul(a, b);
        l[3] = v::fmadd(c, a, b);
        for (int k = 0; k < 4; ++k)
        {
            ++n.results;
            if (v::bits(r[k].value) != v::bits(l[k].value) || r[k].negative != l[k].negative)
            {
                if (n.bad[k]++ < 8)
                    std::printf("differs: %s a %08x b %08x c %08x: strict %08x%s, lean %08x%s\n", kOpName[k], v::bits(a), v::bits(b), v::bits(c),
                                v::bits(r[k].value), r[k].negative ? " (negative)" : "", v::bits(l[k].value), l[k].negative ? " (negative)" : "");
            }
        }
    }

    std::vector<float> edgeValues()
    {
        const uint32_t magnitudes[] = {
            0x00000000u, 0x00000001u, 0x00400000u, 0x007FFFFFu, // zero, the denormals' ends and middle
            0x00800000u, 0x00800001u, 0x00FFFFFFu,              // the smallest normals
            0x1F800000u, 0x1F7FFFFFu, 0x20000000u,              // 2^-64 and neighbours: products near the smallest normal
            0x3F800000u, 0x3F7FFFFFu, 0x3F800001u,              // one and its neighbours
            0x5F800000u, 0x5F7FFFFFu, 0x5F800001u,              // 2^64 and neighbours: products near the largest
            0x7F000000u, 0x7F7FFFFEu, 0x7F7FFFFFu,              // the largest binade
            0x7F800000u, 0x7F800001u, 0x7FC00000u, 0x7FFFFFFFu, // infinity, NaNs
        };
        std::vector<float> out;
        for (uint32_t m : magnitudes)
        {
            out.push_back(v::fromBits(m));
            out.push_back(v::fromBits(m | 0x80000000u));
        }
        return out;
    }
}

int main(int argc, char **argv)
{
    uint64_t results = 100000000ull, seed = 0x9E3779B97F4A7C15ull;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--results" && i + 1 < argc)
            results = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--seed" && i + 1 < argc)
            seed = std::strtoull(argv[++i], nullptr, 0) | 1u;
        else
        {
            std::fprintf(stderr, "usage: rc_vu1_test [--results N] [--seed S]\n");
            return 2;
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t saved = _mm_getcsr();
    const uint32_t strictCsr = (saved & ~0xE040u) | 0x6000u;
    uint32_t leanCsr;
    {
        const v::ScopedMode mode;
        leanCsr = _mm_getcsr();
    }
    bool ok = true;
    if (_mm_getcsr() != saved || leanCsr != (saved | v::kLeanMxcsr))
    {
        std::printf("ScopedMode: MXCSR %08x inside (want %08x), %08x after (want %08x)\n", leanCsr, saved | v::kLeanMxcsr, _mm_getcsr(), saved);
        ok = false;
    }

    // rc::vu1l::run sets the mode and restores the caller's: an entry that is not one returns at once, a direct entry
    // (TRdr 0x2050: one packet from VU memory) runs.
    {
        std::vector<uint8_t> mem(0x4000u, 0);
        alignas(16) float vf[32][4] = {};
        uint32_t packets = 0;
        uint32_t csrInside = 0;
        struct U
        {
            uint32_t *packets, *csr;
        } u{&packets, &csrInside};
        // A packet at quadword 0: one GIF tag with EOP and NLOOP 0.
        const uint64_t tag = 1ull << 15;
        std::memcpy(mem.data(), &tag, 8);
        v::Io io{mem.data(), vf, 0x2050u, 0u,
                 [](void *user, const uint8_t *, uint32_t)
                 {
                     U *x = static_cast<U *>(user);
                     ++*x->packets;
                     *x->csr = _mm_getcsr();
                 },
                 &u};
        const uint32_t caller = (saved & ~0xE040u) | 0x2000u; // round down, no FTZ/DAZ: run must still set RZ + FTZ + DAZ
        _mm_setcsr(caller);
        const bool ran = rc::vu1l::run(rc::vu1l::kTrdr, io);
        const uint32_t after = _mm_getcsr();
        io.startPc = 0x1234u;
        const bool notEntry = rc::vu1l::run(rc::vu1l::kTrdr, io);
        _mm_setcsr(saved);
        if (!ran || notEntry || packets != 1u || csrInside != (caller | v::kLeanMxcsr) || after != caller)
        {
            std::printf("vu1l::run: ran %d, non-entry ran %d, packets %u, MXCSR inside %08x (want %08x), after %08x (want %08x)\n", ran, notEntry, packets,
                        csrInside, caller | v::kLeanMxcsr, after, caller);
            ok = false;
        }
        else
            std::printf("vu1l::run: sets MXCSR %08x over the caller's %08x and restores it\n", csrInside, caller);
    }

    Counts edge, random;
    const std::vector<float> e = edgeValues();
    for (float a : e)
        for (float b : e)
            for (float c : e)
                compare(a, b, c, strictCsr, leanCsr, edge);

    uint64_t state = seed;
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
        case 0: b &= 0x807FFFFFu; break;                                                  // a denormal or zero
        case 1: b = (b & 0x807FFFFFu) | 0x7F800000u; break;                               // infinity or NaN
        case 2: b = (b & 0x80FFFFFFu) | 0x00800000u; break;                               // the smallest binade
        case 3: b = (b & 0x80FFFFFFu) | 0x7F000000u; break;                               // the largest
        case 4: b = (b & 0x80000000u); break;                                             // a signed zero
        case 5: case 6: b = (b & 0x807FFFFFu) | ((0x70u + (b >> 23 & 31u)) << 23); break; // near one
        case 7: b = (b & 0x807FFFFFu) | ((0x10u + (b >> 23 & 63u)) << 23); break;         // small
        case 8: b = (b & 0x807FFFFFu) | ((0x50u + (b >> 23 & 31u)) << 23); break;         // large: products near overflow
        default: break;
        }
        return v::fromBits(b);
    };
    while (random.results < results)
    {
        const float a = pick(), b = pick(), c = pick();
        compare(a, b, c, strictCsr, leanCsr, random);
    }
    // Cancelling triples: c = -(a * b) as the lean mode rounds the product, sometimes a unit in the last place
    // either side, or a sum aimed at the smallest normal (the MADD sums that are zero or tiny after an inexact
    // product: the third fall-back).
    Counts cancel;
    while (cancel.results < results / 4u)
    {
        const float a = pick(), b = pick();
        _mm_setcsr(leanCsr);
        const float p = a * b;
        _mm_setcsr(saved);
        uint32_t cb = v::bits(p) ^ 0x80000000u;
        const uint64_t r = next();
        switch (r & 7u)
        {
        case 0: cb += 1u; break;
        case 1: cb -= 1u; break;
        case 2: // acc = -(p) + the smallest normal, with p's sign: the sum is the smallest normal if exact
        {
            _mm_setcsr(strictCsr);
            const float target = v::fromBits((v::bits(p) & 0x80000000u) | 0x00800000u);
            cb = v::bits(target - p);
            _mm_setcsr(saved);
            break;
        }
        default: break;
        }
        compare(a, b, v::fromBits(cb), strictCsr, leanCsr, cancel);
    }
    _mm_setcsr(saved);

    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint64_t bad = 0;
    for (const Counts *n : {&edge, &random, &cancel})
    {
        const uint64_t b = n->bad[0] + n->bad[1] + n->bad[2] + n->bad[3];
        bad += b;
        std::printf("%s: %" PRIu64 " results, %" PRIu64 " differ (ADD %" PRIu64 ", SUB %" PRIu64 ", MUL %" PRIu64 ", MADD %" PRIu64 ")\n",
                    n == &edge ? "edge triples" : n == &random ? "random" : "cancelling triples", n->results, b, n->bad[0], n->bad[1], n->bad[2], n->bad[3]);
    }
    std::printf("(s) the lean operations: %" PRIu64 " of %" PRIu64 " results differ; %s (%.1f s)\n", bad, edge.results + random.results + cancel.results,
                bad == 0 && ok ? "pass" : "FAIL", s);
    return bad == 0 && ok ? 0 : 1;
}
