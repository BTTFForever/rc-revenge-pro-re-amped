// The float sine and cosine from the newlib in the PS2 SDK (kf_sin.c, kf_cos.c, sf_sin.c, sf_cos.c, er_rem_pio2f.c).
// Written natively so that callers can be compared call by call, without calling back into recompiled code.

#include <cstdio>
#include <cstdlib>

#include "ee_math.h"

namespace rc {

namespace {

constexpr uint32_t kNpio2Hw = 0x326a58u; // Table of the top bits of n * pi / 2.

ee_float kernelSin(ee_float x, ee_float y, int iy)
{
    if ((float_bits(x) & 0x7FFFFFFFu) < 0x32000000u && static_cast<int32_t>(float_bits(x)) == 0)
        return x;
    const ee_float z = x * x;
    const ee_float v = z * x;
    const ee_float r = z * (z * (z * (z * ee_float(1.589691e-10f) + ee_float(-2.505076e-08f)) + ee_float(2.7557314e-06f)) +
                            ee_float(-0.0001984127f)) +
                       ee_float(0.008333334f);
    if (iy != 0)
        return x - ((z * (y * ee_float(0.5f) - v * r) - y) - v * ee_float(-0.16666667f));
    return x + v * (z * r + ee_float(-0.16666667f));
}

ee_float kernelCos(ee_float x, ee_float y)
{
    const uint32_t ix = float_bits(x) & 0x7FFFFFFFu;
    if (ix < 0x32000000u && static_cast<int32_t>(float_bits(x)) == 0)
        return 1.0f;
    const ee_float z = x * x;
    const ee_float r = z * (z * (z * (z * (z * (z * ee_float(-1.1359648e-11f) + ee_float(2.0875723e-09f)) + ee_float(-2.7557314e-07f)) +
                                      ee_float(2.4801588e-05f)) +
                                 ee_float(-0.0013888889f)) +
                            ee_float(0.041666668f));
    if (ix > 0x3E999999u)
    {
        const ee_float q = ix < 0x3F480001u ? float_from_bits(ix - 0x01000000u) : ee_float(0.28125f);
        return (ee_float(1.0f) - q) - ((z * ee_float(0.5f) - q) - (z * r - x * y));
    }
    return ee_float(1.0f) - (z * ee_float(0.5f) - (z * r - x * y));
}

constexpr float kPio2_1 = 1.5707855f, kPio2_1t = 1.0804334e-05f;
constexpr float kPio2_2 = 1.0804273e-05f, kPio2_2t = 6.0771e-11f;
constexpr float kPio2_3 = 6.0770944e-11f, kPio2_3t = 6.123234e-17f;
constexpr float kInvPio2 = 0.6366198f;

// Reduces x to y[0] + y[1] in [-pi/4, pi/4], returning the quadrant.
int32_t remPio2(ee_float x, ee_float y[2])
{
    const uint32_t hx = float_bits(x);
    const uint32_t ix = hx & 0x7FFFFFFFu;
    if (ix < 0x3F490FD9u)
    {
        y[0] = x;
        y[1] = 0.0f;
        return 0;
    }

    if (ix < 0x4016CBE4u)
    {
        const bool positive = static_cast<int32_t>(hx) > 0;
        ee_float z = positive ? x - ee_float(kPio2_1) : x + ee_float(kPio2_1);
        ee_float t = kPio2_1t;
        if ((hx & 0x7FFFFFF0u) == 0x3FC90FD0u)
        {
            t = kPio2_2t;
            z = positive ? z - ee_float(kPio2_2) : z + ee_float(kPio2_2);
        }
        y[0] = positive ? z - t : z + t;
        y[1] = positive ? (z - y[0]) - t : (z - y[0]) + t;
        return positive ? 1 : -1;
    }

    if (ix > 0x43490F80u)
    {
        if (ix > 0x7F7FFFFFu)
        {
            y[0] = x - x;
            y[1] = x - x;
            return 0;
        }
        // Not reached: sinf and cosf hand these arguments to the originals (see below).
        std::fprintf(stderr, "[decomp] sinf/cosf argument %g needs __kernel_rem_pio2f, which is not ported\n", static_cast<double>(x.v));
        std::abort();
    }

    const int32_t e0 = static_cast<int32_t>(ix) >> 23;
    const ee_float magnitude = ee_abs(x);
    const int32_t n = ee_ftoi(magnitude * ee_float(kInvPio2) + ee_float(0.5f));
    const ee_float fn = ee_itof(n);
    ee_float r = magnitude - fn * ee_float(kPio2_1);
    ee_float w = fn * ee_float(kPio2_1t);
    uint32_t table = 0u;
    if (n < 32)
        std::memcpy(&table, guest_host(kNpio2Hw + static_cast<uint32_t>(n - 1) * 4u), 4);
    if (n < 32 && (hx & 0x7FFFFF00u) != table)
    {
        y[0] = r - w;
    }
    else
    {
        y[0] = r - w;
        if (static_cast<int32_t>(e0 - ((float_bits(y[0]) >> 23) & 0xFFu)) >= 9)
        {
            ee_float t = r;
            r = t - fn * ee_float(kPio2_2);
            w = fn * ee_float(kPio2_2t) - ((t - r) - fn * ee_float(kPio2_2));
            y[0] = r - w;
            if (static_cast<int32_t>(e0 - ((float_bits(y[0]) >> 23) & 0xFFu)) > 25)
            {
                t = r;
                r = t - fn * ee_float(kPio2_3);
                w = fn * ee_float(kPio2_3t) - ((t - r) - fn * ee_float(kPio2_3));
                y[0] = r - w;
            }
        }
    }
    y[1] = (r - y[0]) - w;
    if (static_cast<int32_t>(hx) >= 0)
        return n;
    y[0] = -y[0];
    y[1] = -y[1];
    return -n;
}

} // namespace

// Arguments past 2^7 * pi/2 need __kernel_rem_pio2f, which is not ported: the game's own sinf and cosf handle
// them (firing a weapon passes 32768).
const uint32_t kNeedsLongReduction = 0x43490F80u;

ee_float ee_sinf(ee_float x)
{
    const uint32_t ix = float_bits(x) & 0x7FFFFFFFu;
    if (ix > kNeedsLongReduction && ix < 0x7F800000u)
        return callOriginalFloat(0x1c85d0, x);
    if (ix < 0x3F490FD9u)
        return kernelSin(x, 0.0f, 0);
    if (ix >= 0x7F800000u)
        return x - x;
    ee_float y[2];
    switch (remPio2(x, y) & 3)
    {
    case 0:
        return kernelSin(y[0], y[1], 1);
    case 1:
        return kernelCos(y[0], y[1]);
    case 2:
        return -kernelSin(y[0], y[1], 1);
    default:
        return -kernelCos(y[0], y[1]);
    }
}

ee_float ee_cosf(ee_float x)
{
    const uint32_t ix = float_bits(x) & 0x7FFFFFFFu;
    if (ix > kNeedsLongReduction && ix < 0x7F800000u)
        return callOriginalFloat(0x1c84e8, x);
    if (ix < 0x3F490FD9u)
        return kernelCos(x, 0.0f);
    if (ix >= 0x7F800000u)
        return x - x;
    ee_float y[2];
    switch (remPio2(x, y) & 3)
    {
    case 0:
        return kernelCos(y[0], y[1]);
    case 1:
        return -kernelSin(y[0], y[1], 1);
    case 2:
        return -kernelCos(y[0], y[1]);
    default:
        return kernelSin(y[0], y[1], 1);
    }
}

// Exposed under the library's own names for comparison against the recompiled originals.
float sinf(ee_float x) { return ee_sinf(x); }
float cosf(ee_float x) { return ee_cosf(x); }

} // namespace rc

RC_DECOMP_FN(sinf, 0x1c85d0)
RC_DECOMP_FN(cosf, 0x1c84e8)
