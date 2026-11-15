// Decompiled from PSXDEFS.c and 3DMATH.c: 16-bit fixed-point vector maths, written for the PlayStation's geometry
// coprocessor and kept here as ordinary code. gteMat, gteVec and gteOut stand in for the coprocessor's registers, so
// callers can load a matrix, run an operation and fetch its result in separate calls.

#include <algorithm>

#include "ee_math.h"
#include "matrix_fuzz.h"

namespace rc {

namespace {

bool fits16(uint32_t value)
{
    const uint32_t top = value & 0xFFFF8000u;
    return top == 0u || top == 0xFFFF8000u;
}

// The original prints a debug warning here; these show up only on values that have already overflowed.
void warnOverflow(const char *) {}

int32_t matrixRow(int16_t x, int16_t y, int16_t z, int16_t a, int16_t b, int16_t c)
{
    return (x * a + y * b + z * c) >> 12;
}

void gteMatrixMul(int16_t x, int16_t y, int16_t z)
{
    const sMat &m = G(gteMat);
    G(gteOut).x = matrixRow(x, y, z, m.Xx, m.Xy, m.Xz);
    G(gteOut).y = matrixRow(x, y, z, m.Yx, m.Yy, m.Yz);
    G(gteOut).z = matrixRow(x, y, z, m.Zx, m.Zy, m.Zz);
}

void writeWarned(sVec16 &out, uint32_t x, uint32_t y, uint32_t z, const char *warning)
{
    if (!fits16(x) || !fits16(y) || !fits16(z))
        warnOverflow(warning);
    out.x = static_cast<int16>(x);
    out.y = static_cast<int16>(y);
    out.z = static_cast<int16>(z);
}

uint32_t lengthSquared(const sVec16 &v)
{
    return static_cast<uint32_t>(v.x * v.x + v.y * v.y + v.z * v.z);
}

int32_t leadingSignBits(int32_t value) // The EE's plzcw: identical bits below the sign bit.
{
    const uint32_t bits = static_cast<uint32_t>(value < 0 ? ~value : value);
    return bits == 0u ? 31 : __builtin_clz(bits) - 1;
}

void divScaler(sVec16 &out, const sVec16 &vec, int16_t scale)
{
    const int32_t s = scale;
    int32_t x, y, z;
    if (s < 0x3E9)
    {
        x = ee_div(vec.x << 12, s);
        y = ee_div(vec.y << 12, s);
        z = ee_div(vec.z << 12, s);
    }
    else
    {
        const uint32_t inverse = static_cast<uint32_t>(ee_div(0x1000000, s));
        if (!fits16(inverse))
            warnOverflow("Bollox in Vec16DivScaler");
        z = static_cast<int32_t>(vec.z * inverse) >> 12;
        x = static_cast<int32_t>(vec.x * inverse) >> 12;
        y = static_cast<int32_t>(vec.y * inverse) >> 12;
    }
    writeWarned(out, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z), "Overflow in Vec16DivScaler");
}

} // namespace

// Forms of the operations below for vectors held in local variables (ee_math.h).
void gteMulVec16(const sVec16 &v) { gteMatrixMul(v.x, v.y, v.z); }
uint32_t vec16LengthSquared(const sVec16 &v) { return lengthSquared(v); }
void vec16MulScaler(sVec16 &out, const sVec16 &v, int16_t scale)
{
    writeWarned(out, static_cast<uint32_t>((v.x * scale) >> 12), static_cast<uint32_t>((v.y * scale) >> 12),
                static_cast<uint32_t>((v.z * scale) >> 12), "Overflow in Vec16MulScaler");
}

uint16 Sqr32(uint32 n); // math_core.cpp

void PS2C_Vec16DivScaler(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec, int16 scale)
{
    divScaler(*pOut, *pVec, scale);
}

void PS2C_Vec16CrossVec16(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec1, ptr32<_sVec16> pVec2)
{
    const sVec16 a = *pVec1, b = *pVec2;
    writeWarned(*pOut, static_cast<uint32_t>((a.y * b.z - a.z * b.y) >> 12), static_cast<uint32_t>((a.z * b.x - a.x * b.z) >> 12),
                static_cast<uint32_t>((a.x * b.y - a.y * b.x) >> 12), "Overflow in Vec16CrossVec");
}

void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec)
{
    gteMatrixMul(pVec->x, pVec->y, pVec->z);
}

void PS2C_SetGteMatrix(ptr32<_sMat> pMat)
{
    G(gteMat) = *pMat;
}

void PS2C_GteMatrixMulMatrix(ptr32<_sMat> pMatOut, ptr32<_sMat> pMat1, ptr32<_sMat> pMat2)
{
    G(gteMat) = *pMat1;
    const int16_t *const right = reinterpret_cast<const int16_t *>(pMat2.get());
    int16_t *const out = reinterpret_cast<int16_t *>(pMatOut.get());
    for (int column = 0; column < 3; ++column)
    {
        gteMatrixMul(right[column], right[column + 3], right[column + 6]);
        const sVec32 result = G(gteOut);
        if (!fits16(static_cast<uint32_t>(result.x)) || !fits16(static_cast<uint32_t>(result.y)) || !fits16(static_cast<uint32_t>(result.z)))
            warnOverflow("Overflow in GteVec16GteResult");
        // Stored in this order: the output can overlap the second input.
        out[column + 3] = static_cast<int16_t>(result.y);
        out[column] = static_cast<int16_t>(result.x);
        out[column + 6] = static_cast<int16_t>(result.z);
    }
}

void PS2C_GteVecMatrixMul()
{
    const sVec32 v = G(gteVec);
    const sMat &m = G(gteMat);
    G(gteOut).x = (v.x * m.Xx + v.y * m.Xy + v.z * m.Xz) >> 12;
    G(gteOut).z = (v.x * m.Zx + v.y * m.Zy + v.z * m.Zz) >> 12;
    G(gteOut).y = (v.x * m.Yx + v.y * m.Yy + v.z * m.Yz) >> 12;
}

void PS2C_Vec16PlusVec16(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec1, ptr32<_sVec16> pVec2)
{
    const sVec16 a = *pVec1, b = *pVec2;
    writeWarned(*pOut, static_cast<uint32_t>(a.x + b.x), static_cast<uint32_t>(a.y + b.y), static_cast<uint32_t>(a.z + b.z), "Overflow in Vec16PlusVec16");
}

void PS2C_Vec16MinusVec16(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec1, ptr32<_sVec16> pVec2)
{
    const sVec16 a = *pVec1, b = *pVec2;
    writeWarned(*pOut, static_cast<uint32_t>(a.x - b.x), static_cast<uint32_t>(a.y - b.y), static_cast<uint32_t>(a.z - b.z), "Overflow in Vec16MinusVec16");
}

void PS2C_Vec16MulScaler(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec, int16 scale)
{
    const sVec16 v = *pVec;
    vec16MulScaler(*pOut, v, scale);
}

int32 PS2C_Vec16DotVec16(ptr32<_sVec16> pVec1, ptr32<_sVec16> pVec2)
{
    const sVec16 a = *pVec1, b = *pVec2;
    return (a.x * b.x + a.y * b.y + a.z * b.z) >> 12;
}

void PS2C_Vec16Squared(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec)
{
    const sVec16 v = *pVec;
    writeWarned(*pOut, static_cast<uint32_t>((v.x * v.x) >> 12), static_cast<uint32_t>((v.y * v.y) >> 12),
                static_cast<uint32_t>((v.z * v.z) >> 12), "Overflow in Vec16Squared");
}

uint32 PS2C_Vec16LengthSquared(ptr32<_sVec16> pVec)
{
    return lengthSquared(*pVec);
}

uint32 PS2C_Vec16Length(ptr32<_sVec16> pVec)
{
    return Sqr32(lengthSquared(*pVec));
}

namespace {

uint32_t normalise16(sVec16 &out, const sVec16 &vec)
{
    const int16_t scale = static_cast<int16_t>(Sqr32(lengthSquared(vec)));
    divScaler(out, vec, scale);
    // The original returns Sqr32's result as is, zero-extended, not the sign-extended scale.
    return static_cast<uint16_t>(scale);
}

// The shift that brings a 32-bit vector's largest component to 13 significant bits (none for smaller vectors).
uint32_t narrowShift(const sVec32 &v)
{
    const int32_t bits = std::min({leadingSignBits(v.x), leadingSignBits(v.y), leadingSignBits(v.z)});
    const int32_t shift = 18 - bits;
    return shift < 0 ? 0u : static_cast<uint32_t>(shift);
}

sVec16 narrow(const sVec32 &v, uint32_t shift)
{
    sVec16 out{};
    out.x = static_cast<int16>(v.x >> (shift & 31u));
    out.y = static_cast<int16>(v.y >> (shift & 31u));
    out.z = static_cast<int16>(v.z >> (shift & 31u));
    return out;
}

} // namespace

uint32 PS2C_Vec16Normalise(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec)
{
    return normalise16(*pOut, *pVec);
}

void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut)
{
    const sVec32 result = G(gteOut);
    writeWarned(*pOut, static_cast<uint32_t>(result.x), static_cast<uint32_t>(result.y), static_cast<uint32_t>(result.z), "Overflow in GteVec16GteResult");
}

void PS2C_GteMatrixMulVec16Output(ptr32<_sVec16> pVec, ptr32<_sVec16> pOut)
{
    gteMatrixMul(pVec->x, pVec->y, pVec->z);
    PS2C_GetVec16GteResult(pOut);
}

int32 PS2C_GetDotProductResult() { return G(gteOut).x; }

void PS2C_SetGteMatrixForDotProduct(ptr32<_sVec16> pVec)
{
    G(gteMat).Xx = pVec->x;
    G(gteMat).Xy = pVec->y;
    G(gteMat).Xz = pVec->z;
}

void PS2C_Vec16MulMat(ptr32<_sMat> pMat, ptr32<_sVec16> pIn, ptr32<_sVec32> pOut)
{
    const sVec16 v = *pIn;
    pOut->x = matrixRow(v.x, v.y, v.z, pMat->Xx, pMat->Xy, pMat->Xz);
    pOut->y = matrixRow(v.x, v.y, v.z, pMat->Yx, pMat->Yy, pMat->Yz);
    pOut->z = matrixRow(v.x, v.y, v.z, pMat->Zx, pMat->Zy, pMat->Zz);
}

void PS2C_NullTransMatrix()
{
    G(gteTransVec).x = 0;
    G(gteTransVec).z = 0;
    G(gteTransVec).y = 0;
}

void PS2C_SetGteMatrixXxXy(int16 x, int16 y)
{
    G(gteMat).Xx = x;
    G(gteMat).Xy = y;
}

void PS2C_SetGteMatrixXzYx(int16 x, int16 y)
{
    G(gteMat).Yx = y;
    G(gteMat).Xz = x;
}

void PS2C_SetGteMatrixYyYz(int16 x, int16 y)
{
    G(gteMat).Yz = y;
    G(gteMat).Yy = x;
}

void PS2C_SetGteMatrixZxZyZz(int16 x, int16 y, int16 z)
{
    G(gteMat).Zz = z;
    G(gteMat).Zx = x;
    G(gteMat).Zy = y;
}

void PS2C_SetGteVecMatrixMultiplier(ptr32<_sVec16> sv1)
{
    G(gteVec).x = sv1->x;
    G(gteVec).y = sv1->y;
    G(gteVec).z = sv1->z;
}

int32 PS2C_GetDistN() { return G(gteOut).x; }
int32 PS2C_GetDistLP() { return G(gteOut).y; }
int32 PS2C_GetDist1() { return G(gteOut).z; }

uint32 PS2C_Vec32Normalise(ptr32<_sVec16> pOut, ptr32<_sVec32> pVec32)
{
    const sVec32 v = *pVec32;
    int32_t bits = std::min({leadingSignBits(v.x), leadingSignBits(v.y), leadingSignBits(v.z)});
    int32_t shift = 18 - bits;
    if (shift < 0)
        shift = 0;
    sVec16 narrow{};
    narrow.x = static_cast<int16>(v.x >> (shift & 31));
    narrow.y = static_cast<int16>(v.y >> (shift & 31));
    narrow.z = static_cast<int16>(v.z >> (shift & 31));
    const int16_t scale = static_cast<int16_t>(Sqr32(lengthSquared(narrow)));
    divScaler(*pOut, narrow, scale);
    return static_cast<uint32_t>(static_cast<int32_t>(scale) << (shift & 31));
}

void PS2C_SetGteMatrixForDotProduct2(ptr32<_sVec16> pVec)
{
    G(gteMat).Yx = pVec->x;
    G(gteMat).Yy = pVec->y;
    G(gteMat).Yz = pVec->z;
}

void PS2C_SetGteMatrixForDotProduct3(ptr32<_sVec16> pVec)
{
    G(gteMat).Zx = pVec->x;
    G(gteMat).Zy = pVec->y;
    G(gteMat).Zz = pVec->z;
}

int32 PS2C_GetDotProductResult2() { return G(gteOut).y; }
int32 PS2C_GetDotProductResult3() { return G(gteOut).z; }

void PS2C_NulGteMatrix(ptr32<_sMat>)
{
    sMat &m = G(gteMat);
    m.Xx = m.Zz = m.Xy = m.Xz = m.Yx = m.Yy = m.Yz = m.Zx = m.Zy = 0;
}

void PS2C_SetTransMatrix(ptr32<_sVec16> pVec)
{
    G(gteTransVec).x = pVec->x;
    G(gteTransVec).y = pVec->y;
    G(gteTransVec).z = pVec->z;
}

void PS2C_GetVec32GteResult(ptr32<_sVec32> pOut)
{
    *pOut = G(gteOut);
}

// The PlayStation-era forms (PSXDEFS.c) that the game still calls.

constexpr uint32_t kPrintf = 0x12e9e8u, kWarningFormat = 0x324b88u; // "\nPMATHS:PSXDEFS WARNING : %s\n"

// Normalises a short vector, first scaling a short one up by 16 at a time (up to 4096) while its length is under
// 1024, so the division keeps its precision. Returns the length (1 for a zero vector, which leaves pOut alone).
uint32 ps2__Vec16Normalise(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec)
{
    sVec16 vec = *pVec;
    const uint32_t length = Sqr32(lengthSquared(vec));
    if (length == 0u)
        return 1;
    uint32_t scale = length;
    for (int step = 0; step < 3 && scale < 0x400u; ++step)
    {
        scale = length << (4 * (step + 1));
        vec.x = static_cast<int16>(static_cast<uint16_t>(vec.x) << 4);
        vec.y = static_cast<int16>(static_cast<uint16_t>(vec.y) << 4);
        vec.z = static_cast<int16>(static_cast<uint16_t>(vec.z) << 4);
    }
    divScaler(*pOut, vec, static_cast<int16_t>(scale));
    return length;
}

void GteMatrixMulMatrix(ptr32<_sMat> pMatOut, ptr32<_sMat> pMat1, ptr32<_sMat> pMat2)
{
    PS2C_GteMatrixMulMatrix(pMatOut, pMat1, pMat2);
}

// The real warning: printf (a tail call). The native forms above skip it.
void IssueWarning(ptr32<char> warning)
{
    guest_call<int32_t>(kPrintf, ptr32<char>(kWarningFormat), warning);
}

void VSyncCallback(ptr32<void>) {}

uint32 Vec16Length(ptr32<_sVec16> pVec)
{
    return Sqr32(lengthSquared(*pVec));
}

// The length of a long vector, narrowed to shorts for the square root and scaled back.
uint32 ps2__Vec32Length(ptr32<_sVec32> pVec32)
{
    const sVec32 v = *pVec32;
    const uint32_t shift = narrowShift(v);
    return static_cast<uint32_t>(Sqr32(lengthSquared(narrow(v, shift)))) << shift;
}

uint32 ps2__Vec32Normalise(ptr32<_sVec16> pOut, ptr32<_sVec32> pVec32)
{
    const sVec32 v = *pVec32;
    const uint32_t shift = narrowShift(v);
    return normalise16(*pOut, narrow(v, shift)) << shift;
}

// 3DMATH.c

uint32 VecLength(ptr32<_sVec16> pVec)
{
    return Sqr32(lengthSquared(*pVec));
}

} // namespace rc

RC_DECOMP_FN(PS2C_Vec16DivScaler, 0x19eca0)
RC_DECOMP_FN(PS2C_Vec16CrossVec16, 0x19ee48)
RC_DECOMP_FN(PS2C_GteMatrixMulVec16, 0x19ef58)
RC_DECOMP_FN(PS2C_GteMatrixMulMatrix, 0x19f010)
RC_DECOMP_FN(PS2C_GteVecMatrixMul, 0x19f1c0)
RC_DECOMP_FN(PS2C_Vec16PlusVec16, 0x19f268)
RC_DECOMP_FN(PS2C_Vec16MinusVec16, 0x19f350)
RC_DECOMP_FN(PS2C_Vec16MulScaler, 0x19f438)
RC_DECOMP_FN(PS2C_Vec16DotVec16, 0x19f528)
RC_DECOMP_FN(PS2C_Vec16Squared, 0x19f5b0)
RC_DECOMP_FN(PS2C_Vec16LengthSquared, 0x19f698)
RC_DECOMP_FN(PS2C_Vec16Length, 0x19f6c0)
RC_DECOMP_FN(PS2C_Vec16Normalise, 0x19f6f8)
RC_DECOMP_FN(PS2C_SetGteMatrix, 0x19f770)
RC_DECOMP_FN(PS2C_GteMatrixMulVec16Output, 0x19f7b0)
RC_DECOMP_FN(PS2C_GetVec16GteResult, 0x19f880)
RC_DECOMP_FN(PS2C_GetDotProductResult, 0x19f958)
RC_DECOMP_FN(PS2C_SetGteMatrixForDotProduct, 0x19f968)
RC_DECOMP_FN(PS2C_Vec16MulMat, 0x19f990)
RC_DECOMP_FN(PS2C_NullTransMatrix, 0x19fa18)
RC_DECOMP_FN(PS2C_SetGteMatrixXxXy, 0x19fa30)
RC_DECOMP_FN(PS2C_SetGteMatrixXzYx, 0x19fa48)
RC_DECOMP_FN(PS2C_SetGteMatrixYyYz, 0x19fa60)
RC_DECOMP_FN(PS2C_SetGteMatrixZxZyZz, 0x19fa78)
RC_DECOMP_FN(PS2C_SetGteVecMatrixMultiplier, 0x19fa90)
RC_DECOMP_FN(PS2C_GetDistN, 0x19fab8)
RC_DECOMP_FN(PS2C_GetDistLP, 0x19fac8)
RC_DECOMP_FN(PS2C_GetDist1, 0x19fad8)
RC_DECOMP_FN(PS2C_Vec32Normalise, 0x19fc28)
RC_DECOMP_FN(PS2C_SetGteMatrixForDotProduct2, 0x19fd08)
RC_DECOMP_FN(PS2C_SetGteMatrixForDotProduct3, 0x19fd30)
RC_DECOMP_FN(PS2C_GetDotProductResult2, 0x19fd58)
RC_DECOMP_FN(PS2C_GetDotProductResult3, 0x19fd68)
RC_DECOMP_FN(PS2C_NulGteMatrix, 0x19fd78)
RC_DECOMP_FN(PS2C_SetTransMatrix, 0x19fda8)
RC_DECOMP_FN(PS2C_GetVec32GteResult, 0x19fdd0)
RC_DECOMP_FN(VecLength, 0x1a0478)
RC_DECOMP_FN_SETUP(ps2__Vec16Normalise, 0x19eb68, rc::fuzz::vec16Pair)
RC_DECOMP_FN(GteMatrixMulMatrix, 0x19ec88)
RC_DECOMP_FN_SETUP(IssueWarning, 0x19ec90, rc::fuzz::warningText)
RC_DECOMP_FN(VSyncCallback, 0x19f260)
RC_DECOMP_FN_SETUP(Vec16Length, 0x19fc00, rc::fuzz::vec16Pair)
RC_DECOMP_FN_SETUP(ps2__Vec32Length, 0x19fae8, rc::fuzz::vec32Pair)
RC_DECOMP_FN_SETUP(ps2__Vec32Normalise, 0x19fb78, rc::fuzz::vec32Pair)
