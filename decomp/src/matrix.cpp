// Decompiled from MATRIX.c and MATHS.c: the rest of both units. The matrix builders go through libvu0's float rotations
// (still recompiled, called with guest_call) and convert to 1.3.12 fixed point; the square-root tables and their
// table-free forms are built with double-precision maths, which the game does through libgcc's software doubles and
// newlib's sqrt (both correctly rounded, so host doubles give the same bits) between single-precision EE operations.

#include <cmath>

#include "ee_math.h"
#include "matrix_fuzz.h"

namespace rc {

// maths.cpp
void Rotate2dPrimitive(ptr32<int16> x, ptr32<int16> y, angle angleVal);
angle ArcTan(int32 x, int32 y);
void ArcTanDistance(int32 x, int32 y, ptr32<angle> anglePtr, ptr32<uint32> distance);
// psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_NullTransMatrix();
void PS2C_GteMatrixMulVec16Output(ptr32<_sVec16> pVec, ptr32<_sVec16> pOut);
// ai.cpp
void srand(uint32 seed);
int32 rand();

namespace {

constexpr uint32_t kUnitMatrix = 0x1c7ac8u, kRotMatrixX = 0x1c7c10u, kRotMatrixY = 0x1c7cb8u, kRotMatrixZ = 0x1c7b68u,
                   kRotMatrix = 0x1c7d60u;
constexpr uint32_t kVsprintf = 0x1bac98u, kStrcat = 0x1b8af0u, kPrintf = 0x12e9e8u;
constexpr uint32_t kNewline = 0x3282e8u, kDifferent = 0x324b78u; // "\n", "Different %f %f"

ee_float constant(uint32_t address) { return *static_cast<ee_float *>(guest_host(address)); }

// A 12-bit game angle (wrapped to -2048..2047) in radians: angle / 2048 times the constant given (pi).
ee_float radians(int16 value, uint32_t piAddress)
{
    const int32_t wrapped = ((static_cast<int32_t>(value) + 0x800) & 0xfff) - 0x800;
    return ee_itof(wrapped) * ee_float(float_from_bits(0x3A000000u)) * constant(piAddress);
}

// The libvu0 work matrix (rows of four floats) to 1.3.12 fixed point.
void storeMatrix(ptr32<ee_float> work, ptr32<MATRIX> pMat)
{
    const ee_float scale = 4096.0f;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            pMat->m[row][column] = static_cast<int16>(ee_ftoi(work[row * 4 + column] * scale));
}

enum class Axis { X, Y, Z };

// One libvu0 rotation of the work matrix. The game's Y angles are negated: libvu0 turns the other way.
void rotate(ptr32<ee_float> work, Axis axis, int16 value, uint32_t piAddress)
{
    if (value == 0)
        return;
    const ee_float angleRad = radians(value, piAddress);
    if (axis == Axis::X)
        guest_call(kRotMatrixX, work, work, angleRad);
    else if (axis == Axis::Y)
        guest_call(kRotMatrixY, work, work, -angleRad);
    else
        guest_call(kRotMatrixZ, work, work, angleRad);
}

// The three axis rotations in turn, each skipped for a zero angle. pi is kept per call site (all equal).
void rotMatrix3(ptr32<SVECTOR> pVec, ptr32<MATRIX> pMat, const Axis (&order)[3], const uint32_t (&piAt)[3])
{
    GuestFrame frame(0x70);
    const ptr32<ee_float> work = frame.local<ee_float>(0);
    guest_call(kUnitMatrix, work);
    for (int i = 0; i < 3; ++i)
    {
        const int16 value = order[i] == Axis::X ? pVec->vx : order[i] == Axis::Y ? pVec->vy : pVec->vz;
        rotate(work, order[i], value, piAt[i]);
    }
    storeMatrix(work, pMat);
}

// Square root of a double to the nearest float, as dptofp(sqrt(x)) gives it.
ee_float sqrtToFloat(double x) { return static_cast<float>(std::sqrt(x)); }

int32_t halfExponent(int32_t exponent) { return exponent < 0 ? -((1 - exponent) >> 1) : exponent >> 1; }

// dbg_msg called with two doubles in $a1 and $a2, as Math_Test passes them.
void debugMessage(uint32_t format, double first, double second);

} // namespace

// MATRIX.c

void OldRotMatrixXYZ(ptr32<SVECTOR> pVec, ptr32<MATRIX> pMat)
{
    rotMatrix3(pVec, pMat, {Axis::X, Axis::Y, Axis::Z}, {0x327b8cu, 0x327b90u, 0x327b94u});
}

void OldRotMatrixYXZ(ptr32<SVECTOR> pVec, ptr32<MATRIX> pMat)
{
    rotMatrix3(pVec, pMat, {Axis::Y, Axis::X, Axis::Z}, {0x327b98u, 0x327b9cu, 0x327ba0u});
}

void OldRotMatrixZYX(ptr32<SVECTOR> pVec, ptr32<MATRIX> pMat)
{
    rotMatrix3(pVec, pMat, {Axis::Z, Axis::Y, Axis::X}, {0x327ba4u, 0x327ba8u, 0x327bacu});
}

// All three angles at once through sceVu0RotMatrix (rotation vector at sp+0x40), none skipped.
void OldRotMatrix(ptr32<SVECTOR> pVec, ptr32<MATRIX> pMat)
{
    GuestFrame frame(0x70);
    const ptr32<ee_float> work = frame.local<ee_float>(0), rot = frame.local<ee_float>(0x40);
    const ee_float y = radians(pVec->vy, 0x327bb0u), x = radians(pVec->vx, 0x327bb0u), z = radians(pVec->vz, 0x327bb0u);
    rot[0] = x;
    rot[2] = z;
    rot[1] = -y;
    guest_call(kUnitMatrix, work);
    guest_call(kRotMatrix, work, work, rot);
    storeMatrix(work, pMat);
}

// Angles to a matrix (Y, then X, then Z), pitch and heading negated; angles are 16-bit, rotations 12-bit.
void Mat3nBuild(ptr32<_mat3n> m, ptr32<_matAngles> a)
{
    GuestFrame frame(0x20);
    const ptr32<SVECTOR> vec = frame.local<SVECTOR>(0);
    vec->vx = static_cast<int16>(-static_cast<int32_t>(a->pitch) >> 4);
    vec->vy = static_cast<int16>(-static_cast<int32_t>(a->head) >> 4);
    vec->vz = static_cast<int16>(a->roll >> 4);
    OldRotMatrixYXZ(vec, m.cast<MATRIX>());
}

void Mat3nBuildXYZ(ptr32<_mat3n> pMatrix, ptr32<_matAngles> pAngles)
{
    GuestFrame frame(0x20);
    const ptr32<SVECTOR> vec = frame.local<SVECTOR>(0);
    vec->vx = static_cast<int16>(pAngles->pitch >> 4);
    vec->vy = static_cast<int16>(pAngles->head >> 4);
    vec->vz = static_cast<int16>(pAngles->roll >> 4);
    OldRotMatrixXYZ(vec, pMatrix.cast<MATRIX>());
}

// Heading and pitch from the Z axis, then roll from the X axis turned back by both.
void Mat3nDecompose(ptr32<_mat3n> matrix, ptr32<_matAngles> angles)
{
    GuestFrame frame(0x60);
    const ptr32<uint32> distance = frame.local<uint32>(0x10);
    ArcTanDistance(matrix->Zz, matrix->Zx, ptr32<angle>(angles.address + 2u), distance);
    angles->pitch = static_cast<angle>(-ArcTan(static_cast<int32>(*distance), matrix->Zy));
    const ptr32<int16> axis = frame.local<int16>(0);
    axis[0] = matrix->Xx;
    axis[1] = matrix->Xy;
    axis[2] = matrix->Xz;
    Rotate2dPrimitive(axis, axis + 2, static_cast<angle>(-angles->head));
    Rotate2dPrimitive(axis + 2, axis + 1, static_cast<angle>(-angles->pitch));
    angles->roll = ArcTan(axis[0], axis[1]);
}

void NormalMulMatPsx(ptr32<normal_> p, ptr32<_matPsx> m, ptr32<normal_> r)
{
    GuestFrame frame(0x30);
    const ptr32<_sVec16> vec = frame.local<_sVec16>(0);
    const frac z = p->z, x = p->x, y = p->y;
    vec->x = x;
    vec->y = y;
    vec->z = z;
    PS2C_SetGteMatrix(m.cast<_sMat>());
    PS2C_NullTransMatrix();
    PS2C_GteMatrixMulVec16Output(vec, r.cast<_sVec16>());
}

// Meant to multiply by the transpose, but the transposed matrix is copied from an uninitialised local: every entry is
// a stale stack word, or part of the caller's $s0 (saved at sp+0x40) or of the matrix pointer (stored at sp+0x30
// after one of its halfwords was already read). mp itself is never read. Nothing calls it.
void NormalMulMatPsxT(ptr32<normal_> p, ptr32<_matPsx> mp, ptr32<normal_> r)
{
    const uint64_t callerS0 = GPR_U64((&g_runtime->eeScheduler().runningContext()), 16);
    GuestFrame frame(0x60);
    const auto half = [&](uint32_t offset) -> uint16_t & { return *frame.local<uint16_t>(offset); };
    const auto word = [&](uint32_t offset) -> uint32_t & { return *frame.local<uint32_t>(offset); };
    *frame.local<uint64_t>(0x40) = callerS0;
    const uint16_t stale = half(0x30);
    const uint16_t x = static_cast<uint16_t>(p->x), y = static_cast<uint16_t>(p->y), z = static_cast<uint16_t>(p->z);
    half(0) = x;
    half(2) = y;
    half(4) = z;
    half(0x10) = stale;
    word(0x24) = word(0x44);
    half(0x18) = half(0x38);
    word(0x28) = word(0x48);
    half(0x20) = half(0x40);
    const uint16_t xy = half(0x32);
    const uint32_t transZ = word(0x4c);
    word(0x30) = mp.address;
    half(0x16) = xy;
    word(0x2c) = transZ;
    half(0x1c) = half(0x34);
    half(0x1e) = half(0x3a);
    half(0x12) = half(0x36);
    half(0x14) = half(0x3c);
    half(0x1a) = half(0x3e);
    PS2C_SetGteMatrix(frame.local<_sMat>(0x10));
    PS2C_NullTransMatrix();
    PS2C_GteMatrixMulVec16Output(frame.local<_sVec16>(0), r.cast<_sVec16>());
}

// MATHS.c

// gaSqrtTab: the mantissa of sqrt(m) for 15-bit mantissas m, the first half for odd exponents (times sqrt 2), in
// 23-bit fixed point. gaSqrtExpTab: the halved exponent, in place.
void Math_InitSqrtTable()
{
    auto &table = G(gaSqrtTab);
    for (int32_t half = 0; half < 2; ++half)
    {
        const bool odd = ((half - 0x7f) & 1) != 0;
        for (int32_t i = 0; i < 0x8000; ++i)
        {
            double root = std::sqrt(static_cast<double>(i) * 0x1p-15 + 1.0);
            if (odd)
                root = root * std::sqrt(2.0);
            table[(half << 15) | i] = static_cast<s32>((root - 1.0) * 8388608.0);
        }
    }
    auto &exponents = G(gaSqrtExpTab);
    for (int32_t i = 0; i < 0x100; ++i)
        exponents[i] = (halfExponent(i - 0x7f) + 0x7f) * 0x800000;
}

// gaInvSqrtTab: 1 / sqrt(m) in 23-bit fixed point (single precision this time), the first half for odd exponents,
// stored as the mantissa shifted up by one with bit 0 set when the result fell below 1. gaInvSqrtExpTab: the exponent.
void Math_InitInvSqrtTable()
{
    auto &table = G(gaInvSqrtTab);
    const ee_float one = 1.0f, step = float_from_bits(0x38000000u), scale = 8388608.0f;
    for (int32_t half = 0; half < 2; ++half)
    {
        const bool odd = ((0x7f - half) & 1) != 0;
        for (int32_t i = 0; i < 0x8000; ++i)
        {
            const ee_float mantissa = ee_itof(i) * step + one;
            ee_float inverse = one / sqrtToFloat(mantissa.v);
            if (odd)
                inverse = inverse * sqrtToFloat(2.0);
            const int32_t value = ee_ftoi(inverse * scale);
            const uint32_t bits = static_cast<uint32_t>(value);
            table[half * 0x8000 + i] = static_cast<s32>(value > 0x7fffff ? (bits & 0x7fffffu) << 1 : (((bits << 1) & 0x7fffffu) << 1) | 1u);
        }
    }
    auto &exponents = G(gaInvSqrtExpTab);
    for (int32_t i = 0; i < 0x100; ++i)
        exponents[i] = halfExponent(0x7f - i) + 0x7f;
}

// Compares divisions done two ways on 1000 sets of rand() numbers; the comparison was compiled to 0 > 0, so it only
// runs rand 4000 times after srand(0). Nothing calls it.
void Math_Test()
{
    srand(0);
    const ee_float one = 1.0f;
    const auto check = [](ee_float value)
    {
        double difference = static_cast<double>(0.0f);
        if (difference < 0.0)
            difference = 0.0 - difference;
        if (difference > 0.0)
            debugMessage(kDifferent, static_cast<double>(value.v), static_cast<double>(value.v));
    };
    for (int32_t i = 999; i >= 0; --i)
    {
        ee_float a = ee_itof(rand());
        ee_float b = ee_itof(rand());
        ee_float c = ee_itof(rand());
        const ee_float d = ee_itof(rand());
        const ee_float ab = a / b;
        a = a * c;
        b = b / a;
        c = ab * c;
        const ee_float e = c / d;
        check(one / ab);
        check(one / b);
        check(one / c);
        check(one / e);
    }
}

// sqrt(f) from the mantissa's top 15 bits and the exponent, without the table.
ee_float Math_SqrtNoTable(ee_float f)
{
    const int32_t bits = static_cast<int32_t>(float_bits(f));
    const ee_float one = 1.0f;
    const ee_float mantissa = ee_itof((bits >> 8) & 0x7fff) * ee_float(float_from_bits(0x38000000u)) + one;
    const int32_t exponent = ((bits >> 23) & 0xff) - 0x7f;
    ee_float root = sqrtToFloat(mantissa.v);
    const int32_t half = halfExponent(exponent);
    if ((exponent & 1) != 0)
        root = root * sqrtToFloat(2.0) - one;
    else
        root = root - one;
    const uint32_t fraction = static_cast<uint32_t>(ee_ftoi(root * ee_float(8388608.0f)));
    return float_from_bits(fraction | static_cast<uint32_t>(half + 0x7f) << 23);
}

// 1 / sqrt(f) the same way, with 15 bits of mantissa.
ee_float Math_InvSqrtNoTable(ee_float f)
{
    const int32_t bits = static_cast<int32_t>(float_bits(f));
    const ee_float one = 1.0f;
    const ee_float mantissa = ee_itof((bits >> 8) & 0x7fff) * ee_float(float_from_bits(0x38000000u)) + one;
    ee_float inverse = one / sqrtToFloat(mantissa.v);
    const int32_t exponent = 0x7f - ((bits >> 23) & 0xff);
    int32_t half = halfExponent(exponent);
    if ((exponent & 1) != 0)
        inverse = inverse * sqrtToFloat(2.0);
    if (inverse < one)
    {
        inverse = inverse + inverse;
        --half;
    }
    inverse = inverse - one;
    const uint32_t fraction = static_cast<uint32_t>(ee_ftoi(inverse * ee_float(32768.0f)));
    return float_from_bits(fraction << 8 | static_cast<uint32_t>(half + 0x7f) << 23);
}

// printf with a newline added, through a 256-byte buffer. Variadic: the prologue saves the argument registers in the
// frame and vsprintf gets sp+0x104 as its argument pointer.
void dbg_msg(ptr32<char> fmt)
{
    R5900Context &ctx = g_runtime->eeScheduler().runningContext();
    const uint64_t a1 = GPR_U64((&ctx), 5), a2 = GPR_U64((&ctx), 6), a3 = GPR_U64((&ctx), 7);
    const uint64_t t0 = GPR_U64((&ctx), 8), t1 = GPR_U64((&ctx), 9), t2 = GPR_U64((&ctx), 10), t3 = GPR_U64((&ctx), 11);
    const float f12 = ctx.f[12], f14 = ctx.f[14], f16 = ctx.f[16], f18 = ctx.f[18];
    GuestFrame frame(0x1a0);
    *frame.local<uint64_t>(0x168) = a1;
    *frame.local<uint64_t>(0x170) = a2;
    *frame.local<uint32_t>(0x100) = fmt.address;
    *frame.local<uint64_t>(0x178) = a3;
    *frame.local<uint64_t>(0x180) = t0;
    *frame.local<uint64_t>(0x188) = t1;
    *frame.local<uint64_t>(0x190) = t2;
    *frame.local<uint64_t>(0x198) = t3;
    *frame.local<float>(0x158) = f12;
    *frame.local<float>(0x15c) = f14;
    *frame.local<float>(0x160) = f16;
    *frame.local<float>(0x164) = f18;
    const ptr32<char> buffer = frame.local<char>(0);
    guest_call<int32_t>(kVsprintf, buffer, fmt, frame.local<void>(0x104));
    guest_call<uint32_t>(kStrcat, buffer, ptr32<char>(kNewline));
    guest_call<int32_t>(kPrintf, buffer);
}

namespace {

void debugMessage(uint32_t format, double first, double second)
{
    R5900Context &ctx = g_runtime->eeScheduler().runningContext();
    uint64_t a, b;
    std::memcpy(&a, &first, 8);
    std::memcpy(&b, &second, 8);
    SET_GPR_U64((&ctx), 5, a);
    SET_GPR_U64((&ctx), 6, b);
    dbg_msg(ptr32<char>(format));
}

} // namespace

} // namespace rc

RC_DECOMP_FN(OldRotMatrixXYZ, 0x19dd40)
RC_DECOMP_FN(OldRotMatrixYXZ, 0x19def8)
RC_DECOMP_FN(OldRotMatrixZYX, 0x19e0b0)
RC_DECOMP_FN(OldRotMatrix, 0x19e268)
RC_DECOMP_FN(Mat3nBuild, 0x19e710)
RC_DECOMP_FN(Mat3nBuildXYZ, 0x19e768)
RC_DECOMP_FN(Mat3nDecompose, 0x19dc78)
RC_DECOMP_FN_SETUP(NormalMulMatPsx, 0x19e7c0, rc::fuzz::normalMatrix)
RC_DECOMP_FN_SETUP(NormalMulMatPsxT, 0x19e818, rc::fuzz::normalMatrix)
RC_DECOMP_FN_SETUP_HEAP(Math_InitSqrtTable, 0x19aa48, rc::fuzz::noInput)
RC_DECOMP_FN_SETUP_HEAP(Math_InitInvSqrtTable, 0x19ad60, rc::fuzz::noInput)
RC_DECOMP_FN_SETUP(Math_Test, 0x19b0b0, rc::fuzz::noInput)
RC_DECOMP_FN_SETUP(Math_SqrtNoTable, 0x19b668, rc::fuzz::floatBits)
RC_DECOMP_FN_SETUP(Math_InvSqrtNoTable, 0x19b768, rc::fuzz::floatBits)
RC_DECOMP_FN_SETUP(dbg_msg, 0x19b888, rc::fuzz::debugFormat)
