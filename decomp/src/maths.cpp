// Decompiled from F3DMATH.c, MATHS.c, MATRIX.c, TRIG.c and MATHTRIG.c. Matrices are 3x3 fixed point with 12 fractional
// bits unless they are sFMat, which holds the same scale in floats.

#include "ee_math.h"

namespace rc {

namespace {

constexpr float kOneOver4096 = 0.00024414062f;
const ee_float kAngleScale = float_from_bits(0x3A490FDBu); // pi / 4096: half an angle unit, once the angle is doubled.

// Sine and cosine of a game angle, scaled to 12 fractional bits. The caller says how the angle is reduced.
int32_t cosine12(int32_t reduced)
{
    const ee_float a = ee_itof(reduced);
    return ee_ftoi(ee_cosf((a + a) * kAngleScale) * ee_float(4096.0f));
}

int32_t sine12(int32_t reduced)
{
    const ee_float a = ee_itof(reduced);
    return ee_ftoi(ee_sinf((a + a) * kAngleScale) * ee_float(4096.0f));
}

frac mix(int32_t a, int32_t aScale, int32_t b, int32_t bScale)
{
    return static_cast<frac>((a * aScale + b * bScale) >> 12);
}

// The low 32 bits of (a * b) >> 12, computed in 64 bits as the original does.
uint32_t mul12(int32_t a, int32_t b)
{
    return static_cast<uint32_t>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) >> 12);
}

} // namespace

// F3DMATH.c

void fmatMul(_sFMat &out, const _sFMat &left, const _sFMat &right)
{
    const _sFMat a = left, b = right; // out can be either input.
    const ee_float scale = kOneOver4096;
    out.Xx = (a.Xx * b.Xx + a.Xy * b.Yx + a.Xz * b.Zx) * scale;
    out.Xy = (a.Xx * b.Xy + a.Xy * b.Yy + a.Xz * b.Zy) * scale;
    out.Xz = (a.Xx * b.Xz + a.Xy * b.Yz + a.Xz * b.Zz) * scale;
    out.Yx = (a.Yx * b.Xx + a.Yy * b.Yx + a.Yz * b.Zx) * scale;
    out.Yy = (a.Yx * b.Xy + a.Yy * b.Yy + a.Yz * b.Zy) * scale;
    out.Yz = (a.Yx * b.Xz + a.Yy * b.Yz + a.Yz * b.Zz) * scale;
    out.Zx = (a.Zx * b.Xx + a.Zy * b.Yx + a.Zz * b.Zx) * scale;
    out.Zy = (a.Zx * b.Xy + a.Zy * b.Yy + a.Zz * b.Zy) * scale;
    out.Zz = (a.Zx * b.Xz + a.Zy * b.Yz + a.Zz * b.Zz) * scale;
}

void FMatMulFMat(ptr32<_sFMat> pMatOut, ptr32<_sFMat> pMat1, ptr32<_sFMat> pMat2)
{
    fmatMul(*pMatOut, *pMat1, *pMat2);
}

// Rebuilds a matrix from its Y and Z columns so that it is orthonormal again.
void FMatNormalise(ptr32<_sFMat> pMatOut, ptr32<_sFMat> pMatIn)
{
    const ee_float scale = kOneOver4096;
    const ee_float zX = pMatIn->Xz, zY = pMatIn->Yz, zZ = pMatIn->Zz;
    ee_float yX = pMatIn->Xy, yY = pMatIn->Yy, yZ = pMatIn->Zy;

    const ee_float yScale = ee_rsqrt(4096.0f, yX * yX + yY * yY + yZ * yZ);
    yX = yX * yScale;
    yY = yY * yScale;
    yZ = yZ * yScale;
    pMatOut->Xy = yX;
    pMatOut->Yy = yY;
    pMatOut->Zy = yZ;

    ee_float xX = (yY * zZ - yZ * zY) * scale;
    ee_float xY = (yZ * zX - yX * zZ) * scale;
    ee_float xZ = (yX * zY - yY * zX) * scale;
    const ee_float xScale = ee_rsqrt(4096.0f, xX * xX + xY * xY + xZ * xZ);
    xZ = xZ * xScale;
    xY = xY * xScale;
    xX = xX * xScale;
    pMatOut->Zx = xZ;
    pMatOut->Yx = xY;
    pMatOut->Xx = xX;

    pMatOut->Xz = (xY * yZ - xZ * yY) * scale;
    pMatOut->Yz = (xZ * yX - xX * yZ) * scale;
    pMatOut->Zz = (xX * yY - xY * yX) * scale;
}

// MATHS.c

void Math_Init() {}
void Math_Term() {}

// An approximate square root straight from the table, without the refining step Sqr32 has.
uint16 SqrQ32(uint32 n)
{
    if (n == 0u)
        return 0u;
    uint32_t shifts = 0u;
    uint32_t top;
    if ((n & 0xC0000000u) == 0u)
    {
        uint32_t previous;
        do
        {
            previous = n;
            n <<= 2;
            ++shifts;
        } while ((n & 0xC0000000u) == 0u);
        top = (previous & 0x3FFFFFFFu) >> 21;
    }
    else
    {
        top = n >> 23;
    }
    uint16_t root = G(sqrTableG)[(top + 1u) >> 1];
    if (shifts != 0u)
        root = static_cast<uint16_t>((static_cast<uint32_t>(root) + (1u << ((shifts - 1u) & 31u))) >> (shifts & 31u));
    return root;
}

// Fills the tables of 1/x mantissas and exponents.
void Math_InitInvTable()
{
    for (int32_t i = 0; i < 0x8000; ++i)
    {
        uint32_t value = static_cast<uint32_t>(ee_ftoi(ee_float(1.0f) / (ee_itof(i) * ee_float(3.0517578e-05f) + ee_float(1.0f)) * ee_float(8388608.0f)));
        if (static_cast<int32_t>(value) < 0x800000)
            value = (value & 0x3FFFFFu) << 2 | 1u;
        else
            value = (value & 0x7FFFFFu) << 1;
        G(gaInvTab)[i] = static_cast<s32>(value);
    }
    for (int32_t i = 0; i < 0x100; ++i)
        G(gaInvExpTab)[i] = 0xFE - i;
}

// MATRIX.c. The rotations combine two rows or columns, keeping 12 fractional bits.

void Mat3nHeadingRotation(ptr32<_mat3n> matrix, ptr32<_matAngles> angles, int32_t preFlag)
{
    _mat3n m = *matrix;
    const int32_t a = angles->head >> 4;
    const int32_t c = cosine12(a), s = sine12(a);
    if (preFlag == 0)
    {
        matrix->Xx = mix(m.Xz, s, m.Xx, c);
        matrix->Xz = mix(m.Xz, c, m.Xx, -s);
        matrix->Yz = mix(m.Yz, c, m.Yx, -s);
        matrix->Yx = mix(m.Yz, s, m.Yx, c);
        matrix->Zx = mix(m.Zz, s, m.Zx, c);
        matrix->Zz = mix(m.Zz, c, m.Zx, -s);
    }
    else
    {
        matrix->Xx = mix(m.Zx, -s, m.Xx, c);
        matrix->Xy = mix(m.Zy, -s, m.Xy, c);
        matrix->Xz = mix(m.Zz, -s, m.Xz, c);
        matrix->Zx = mix(m.Zx, c, m.Xx, s);
        matrix->Zy = mix(m.Zy, c, m.Xy, s);
        matrix->Zz = mix(m.Zz, c, m.Xz, s);
    }
}

void Mat3nPitchRotation(ptr32<_mat3n> matrix, ptr32<_matAngles> angles, int32_t preFlag)
{
    _mat3n m = *matrix;
    const int32_t a = angles->pitch >> 4;
    const int32_t c = cosine12(a), s = sine12(a);
    if (preFlag == 0)
    {
        matrix->Xy = mix(m.Xz, -s, m.Xy, c);
        matrix->Xz = mix(m.Xz, c, m.Xy, s);
        matrix->Yy = mix(m.Yz, -s, m.Yy, c);
        matrix->Yz = mix(m.Yz, c, m.Yy, s);
        matrix->Zy = mix(m.Zz, -s, m.Zy, c);
        matrix->Zz = mix(m.Zz, c, m.Zy, s);
    }
    else
    {
        matrix->Yx = mix(m.Zx, s, m.Yx, c);
        matrix->Yy = mix(m.Zy, s, m.Yy, c);
        matrix->Yz = mix(m.Zz, s, m.Yz, c);
        matrix->Zx = mix(m.Zx, c, m.Yx, -s);
        matrix->Zy = mix(m.Zy, c, m.Yy, -s);
        matrix->Zz = mix(m.Zz, c, m.Yz, -s);
    }
}

void Mat3nRollRotation(ptr32<_mat3n> matrix, ptr32<_matAngles> angles, int32_t preFlag)
{
    _mat3n m = *matrix;
    const int32_t a = angles->roll >> 4;
    const int32_t c = cosine12(a), s = sine12(a);
    if (preFlag == 0)
    {
        matrix->Xx = mix(m.Xy, -s, m.Xx, c);
        matrix->Xy = mix(m.Xy, c, m.Xx, s);
        matrix->Yx = mix(m.Yy, -s, m.Yx, c);
        matrix->Yy = mix(m.Yy, c, m.Yx, s);
        matrix->Zx = mix(m.Zy, -s, m.Zx, c);
        matrix->Zy = mix(m.Zy, c, m.Zx, s);
    }
    else
    {
        matrix->Xx = mix(m.Yx, s, m.Xx, c);
        matrix->Xy = mix(m.Yy, s, m.Xy, c);
        matrix->Xz = mix(m.Yz, s, m.Xz, c);
        matrix->Yx = mix(m.Yx, c, m.Xx, -s);
        matrix->Yy = mix(m.Yy, c, m.Xy, -s);
        matrix->Yz = mix(m.Yz, c, m.Xz, -s);
    }
}

void Rotate2dPrimitive(ptr32<int16> x, ptr32<int16> y, angle angleVal)
{
    const int32_t reduced = static_cast<int32_t>(angleVal) >> 4;
    // The first cosine uses the signed angle, the others the same angle masked to twelve bits.
    const int32_t cosX = cosine12(reduced);
    const int32_t masked = reduced & 0xFFF;
    const int32_t sinA = sine12(masked);
    const int32_t cosY = cosine12(masked);
    const int32_t oldX = *x, oldY = *y;
    *x = static_cast<int16>((sinA * oldY + cosX * oldX) >> 12);
    *y = static_cast<int16>((sinA * -oldX + cosY * oldY) >> 12);
}

// Builds a rotation matrix from an angle about a unit axis, using the half-angle (quaternion) form.
void AngleAxisToMatrix(ptr32<_matPsx> pMat, ptr32<vec16_> pAxis, angle ang)
{
    const int32_t w = cosine12(ang >> 4);
    const int32_t s = sine12(static_cast<int32_t>(ang) >> 4 & 0xFFF);
    const int32_t qz = pAxis->z * s >> 12;
    const int32_t qx = pAxis->x * s >> 12;
    const int32_t qy = pAxis->y * s >> 12;

    const int16_t xx = static_cast<int16_t>(qx * qx >> 11);
    const int16_t yy = static_cast<int16_t>(qy * qy >> 11);
    const int16_t zz = static_cast<int16_t>(qz * qz >> 11);
    const int16_t wz = static_cast<int16_t>(w * qz >> 11);
    const int16_t xy = static_cast<int16_t>(qx * qy >> 11);
    const int16_t xz = static_cast<int16_t>(qx * qz >> 11);
    const int16_t wy = static_cast<int16_t>(w * qy >> 11);
    const int16_t wx = static_cast<int16_t>(w * qx >> 11);
    const int16_t yz = static_cast<int16_t>(qy * qz >> 11);

    _mat3n &m = pMat->mat;
    m.Zz = static_cast<frac>(0x1000 - (xx + yy));
    m.Xy = static_cast<frac>(xy + wz);
    m.Yx = static_cast<frac>(xy - wz);
    m.Xz = static_cast<frac>(xz - wy);
    m.Zx = static_cast<frac>(xz + wy);
    m.Yz = static_cast<frac>(yz + wx);
    m.Zy = static_cast<frac>(yz - wx);
    m.Xx = static_cast<frac>(0x1000 - (yy + zz));
    m.Yy = static_cast<frac>(0x1000 - (xx + zz));
}

void MatPsxInit(ptr32<_matPsx> in)
{
    std::memset(&in->mat, 0, 18);
    std::memset(reinterpret_cast<uint8_t *>(in.get()) + 16, 0, 4); // Zz and the padding after it.
    in->trans.x = 0;
    in->trans.y = 0;
    in->trans.z = 0;
}

void Mat3nXxyz(ptr32<_mat3n> p, ptr32<normal_> pn)
{
    pn->x = p->Xx;
    pn->y = p->Xy;
    pn->z = p->Xz;
}

void Mat3nYxyz(ptr32<_mat3n> p, ptr32<normal_> pn)
{
    pn->x = p->Yx;
    pn->y = p->Yy;
    pn->z = p->Yz;
}

void Mat3nZxyz(ptr32<_mat3n> p, ptr32<normal_> pn)
{
    pn->x = p->Zx;
    pn->y = p->Zy;
    pn->z = p->Zz;
}

void Mat3nReOrthogonalise(ptr32<_mat3n> p)
{
    normal_ x{p->Xx, p->Xy, p->Xz};
    normal_ z{p->Zx, p->Zy, p->Zz};
    normal_ y;
    normaliseInPlace(x);
    crossInPlace(x, z, y);
    normaliseInPlace(y);
    crossInPlace(y, x, z);
    p->Xx = x.x;
    p->Zz = z.z;
    p->Xy = x.y;
    p->Xz = x.z;
    p->Yx = y.x;
    p->Yy = y.y;
    p->Yz = y.z;
    p->Zx = z.x;
    p->Zy = z.y;
}

void Vec32MulMatPsx(ptr32<vec32_> p, ptr32<_matPsx> mp, ptr32<vec32_> r)
{
    const int32_t x = p->x, y = p->y, z = p->z;
    const _mat3n &m = mp->mat;
    r->x = static_cast<int32>(mul12(z, m.Zx) + mul12(y, m.Yx) + mul12(x, m.Xx));
    r->y = static_cast<int32>(mul12(z, m.Zy) + mul12(y, m.Yy) + mul12(x, m.Xy));
    r->z = static_cast<int32>(mul12(z, m.Zz) + mul12(y, m.Yz) + mul12(x, m.Xz));
}

void Vec32MulMatPsxT(ptr32<vec32_> p, ptr32<_matPsx> mp, ptr32<vec32_> r)
{
    const int32_t x = p->x, y = p->y, z = p->z;
    const _mat3n &m = mp->mat;
    r->x = static_cast<int32>(mul12(z, m.Xz) + mul12(y, m.Xy) + mul12(x, m.Xx));
    r->y = static_cast<int32>(mul12(z, m.Yz) + mul12(y, m.Yy) + mul12(x, m.Yx));
    r->z = static_cast<int32>(mul12(z, m.Zz) + mul12(y, m.Zy) + mul12(x, m.Zx));
}

// TRIG.c

uint32 GetMSB(uint32 src); // math_core.cpp

// The angle of (x, y), from a table of arctangents over one octant.
angle ArcTan(int32 x, int32 y)
{
    if (x == 0)
    {
        if (y == 0)
            return 0;
        return static_cast<angle>(y < 1 ? 0xC000 : 0x4000);
    }

    uint32_t absX = static_cast<uint32_t>(x < 0 ? -x : x);
    uint32_t absY = static_cast<uint32_t>(y < 0 ? -y : y);
    const bool steep = absX < absY;
    uint32_t large = steep ? absY : absX;
    const uint32_t small = steep ? absX : absY;

    uint32_t numerator = small << 12;
    if (small >= 0x10000u)
    {
        const uint32_t msb = GetMSB(small);
        if (msb >= 0x15u)
        {
            large >>= (msb - 0x14u) & 31u;
            numerator = small << ((0xCu - (msb - 0x14u)) & 31u);
        }
    }

    const int32_t quotient = ee_div(static_cast<int32_t>(numerator), static_cast<int32_t>(large));
    const int32_t remainder = static_cast<int32_t>(numerator) - quotient * static_cast<int32_t>(large);
    const uint32_t roundUp = large < static_cast<uint32_t>(remainder << 1) ? 1u : 0u;
    const int16_t value = G(arcTanTableG)[quotient + static_cast<int32_t>(roundUp)];

    if (x < 0)
    {
        if (y < 0)
            return static_cast<angle>(steep ? -0x4000 - value : value + 0x8000);
        return static_cast<angle>(steep ? value + 0x4000 : -0x8000 - value);
    }
    if (y < 0)
        return static_cast<angle>(steep ? value + 0xC000 : -value);
    return static_cast<angle>(steep ? 0x4000 - value : value);
}

// MATHTRIG.c

void Trig_Init() {}
void Trig_Term() {}

} // namespace rc

RC_DECOMP_FN(FMatMulFMat, 0x199390)
RC_DECOMP_FN(FMatNormalise, 0x199570)
RC_DECOMP_FN(Math_Init, 0x19aa38)
RC_DECOMP_FN(Math_Term, 0x19aa40)
RC_DECOMP_FN(SqrQ32, 0x19b3e0)
RC_DECOMP_FN(Math_InitInvTable, 0x19b468)
RC_DECOMP_FN(Mat3nHeadingRotation, 0x19b8f8)
RC_DECOMP_FN(Mat3nPitchRotation, 0x19c420)
RC_DECOMP_FN(Mat3nRollRotation, 0x19cf38)
RC_DECOMP_FN(Rotate2dPrimitive, 0x19da70)
RC_DECOMP_FN(AngleAxisToMatrix, 0x19e3e8)
RC_DECOMP_FN(MatPsxInit, 0x19e5a8)
RC_DECOMP_FN(Mat3nXxyz, 0x19e5d0)
RC_DECOMP_FN(Mat3nYxyz, 0x19e5f0)
RC_DECOMP_FN(Mat3nZxyz, 0x19e610)
RC_DECOMP_FN(Mat3nReOrthogonalise, 0x19e630)
// The originals sum with a trapping add, so random inputs that overflow have no meaningful result.
RC_DECOMP_FN_NOFUZZ(Vec32MulMatPsx, 0x19e8d8)
RC_DECOMP_FN_NOFUZZ(Vec32MulMatPsxT, 0x19ea20)
RC_DECOMP_FN(ArcTan, 0x19fdf8)
RC_DECOMP_FN(Trig_Init, 0x1a7508)
RC_DECOMP_FN(Trig_Term, 0x1a7510)
