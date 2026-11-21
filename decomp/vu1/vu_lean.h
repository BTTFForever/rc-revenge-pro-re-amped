// The lean build's arithmetic (RENDERER_P2_DESIGN.md 4.1): included by vu_shape.h when RC_VU1_LEAN is 1, after the
// strict operations (named faddStrict ... there), never on its own.
//
// Under MXCSR = round toward zero (0x6000) + flush to zero (0x8000) + denormals are zero (0x0040) the host's single-
// precision ADD, SUB and MUL are the VU's (vu_shape.h's norm and finish) on every operand but an infinity or a NaN,
// which the VU reads as the largest finite value: such an operand always gives an infinity or NaN result, and that
// result sends the operation to the strict one. MADD rounds the product to single precision and then adds it; the
// strict MADD neither flushes nor clamps that product (a product in the denormal range takes part in the sum; after
// an overflowing product the sum is clamped; the sum's sign and its flush are the exact sum's, the product unrounded),
// which the mode cannot give: a zero product of two non-zero factors, a product of the largest magnitude, and a sum
// below 2^-101 from a non-zero accumulator go to the strict one too. The first two MADD cases
// were found by shape_lean.cpp's random self-test, the third by rc_vu1_test's edge triples (an exact cancellation
// after an inexact product: never met in the 67 dumps, where the lean build without it was equal); none comes from a
// description of the VU.
//
// A fall-back runs the strict operation with FTZ and DAZ cleared for its duration (round toward zero kept), which is
// the strict build's mode. Division, the float/integer conversions and reciprocalLength are vu_shape.h's in both
// builds: their operands are results of the operations above or values the strict code normalises itself.
#pragma once

#include <cmath>
#include <cstdint>

#include <xmmintrin.h>

namespace rc::RC_VU1_NAMESPACE {

// The bits a lean run sets in MXCSR (vu1l::run, ScopedMode): round toward zero, flush to zero, denormals are zero.
constexpr uint32_t kLeanMxcsr = 0x6000u | 0x8000u | 0x0040u;

// Sets the lean mode on top of the caller's MXCSR (its exception masks are kept) and restores the caller's on exit.
struct ScopedMode
{
    uint32_t saved;
    ScopedMode() : saved(_mm_getcsr()) { _mm_setcsr(saved | kLeanMxcsr); }
    ~ScopedMode() { _mm_setcsr(saved); }
    ScopedMode(const ScopedMode &) = delete;
    ScopedMode &operator=(const ScopedMode &) = delete;
};

// An infinity or a NaN.
inline bool special(float v) { return (bits(v) & 0x7F800000u) == 0x7F800000u; }

// The strict operation under the strict build's mode: FTZ and DAZ off for its duration.
template <typename Op> [[gnu::noinline]] Fmac strictFallBack(Op op)
{
    const uint32_t csr = _mm_getcsr();
    _mm_setcsr(csr & ~0x8040u);
    const Fmac r = op();
    _mm_setcsr(csr);
    return r;
}

inline Fmac fadd(float a, float b)
{
    const float r = a + b;
    if (__builtin_expect(special(r), 0))
        return strictFallBack([&] { return faddStrict(a, b); });
    return {r, std::signbit(r)};
}

inline Fmac fsub(float a, float b)
{
    const float r = a - b;
    if (__builtin_expect(special(r), 0))
        return strictFallBack([&] { return fsubStrict(a, b); });
    return {r, std::signbit(r)};
}

inline Fmac fmul(float a, float b)
{
    const float r = a * b;
    if (__builtin_expect(special(r), 0))
        return strictFallBack([&] { return fmulStrict(a, b); });
    return {r, std::signbit(r)};
}

inline Fmac fmadd(float acc, float a, float b)
{
    const float p = a * b;
    // A zero product of two non-zero factors was tiny (the strict MADD keeps it, a denormal, for the sum); a product
    // of the largest magnitude may have overflowed (the strict MADD then clamps the sum).
    if (__builtin_expect((p == 0.0f && (bits(a) & 0x7FFFFFFFu) != 0u && (bits(b) & 0x7FFFFFFFu) != 0u) || (bits(p) & 0x7FFFFFFFu) >= 0x7F7FFFFFu, 0))
        return strictFallBack([&] { return fmaddStrict(acc, a, b); });
    const float r = acc + p;
    // A sum below 2^-101 from a non-zero accumulator: the product's lost bits (delta, less than one unit u in its last
    // place, of the product's sign) are in the strict MADD's exact sum, which decides the sign and the flush. An exact
    // cancellation (acc = -p) is +0 here and a signed zero, or +0 with the negative flag, there; a sum r >= the
    // smallest normal whose exact value r - delta is below it is flushed there. The second needs u * 2^-24 below the
    // smallest normal (r - delta is at least that), so u < 2^-102 and r < 2^-126 + 2^-102: below 2^-101.
    if (__builtin_expect(special(r) || special(acc) || ((bits(r) & 0x7FFFFFFFu) < 0x0D000000u && (bits(acc) & 0x7FFFFFFFu) != 0u), 0))
        return strictFallBack([&] { return fmaddStrict(acc, a, b); });
    return {r, std::signbit(r)};
}

} // namespace rc::RC_VU1_NAMESPACE
