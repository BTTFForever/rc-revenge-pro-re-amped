// Maths shared between decompiled files.
#pragma once

#include "rc_decomp.h"

namespace rc {

// The game's float sine and cosine, from the newlib in its SDK.
ee_float ee_sinf(ee_float x);
ee_float ee_cosf(ee_float x);

// Reinterprets a float's bits, as the library's bit tests do.
inline uint32_t float_bits(ee_float x)
{
    uint32_t bits;
    std::memcpy(&bits, &x.v, 4);
    return bits;
}

inline ee_float float_from_bits(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

// In-place forms of the normal operations, for normals held in local variables.
uint16 normaliseInPlace(normal_ &r);
void crossInPlace(const normal_ &a, const normal_ &b, normal_ &dst);

void fmatMul(_sFMat &out, const _sFMat &a, const _sFMat &b);

// psxdefs.cpp: 16-bit vector operations on local vectors. gteMulVec16 multiplies by gteMat into gteOut.
void gteMulVec16(const sVec16 &v);
uint32_t vec16LengthSquared(const sVec16 &v);
void vec16MulScaler(sVec16 &out, const sVec16 &v, int16_t scale);

// SVECTOR and sVec16 are the same four shorts under two names.
inline sVec16 &asVec16(SVECTOR &v) { return reinterpret_cast<sVec16 &>(v); }
inline const sVec16 &asVec16(const SVECTOR &v) { return reinterpret_cast<const sVec16 &>(v); }

} // namespace rc
