// Decompiled from src\RANDOM.c, src\VECTOR.c, src\TRIG.c, src\MATHS.c and src\ACOL.c of the original game.
// Each function behaves exactly as the shipped one; the names come from the game's own debug data.

#include "rc_decomp.h"

namespace rc {

// RANDOM.c: a linear congruential generator; the "ND" (non-deterministic) stream is kept separately
// so that cosmetic randomness cannot disturb the stream the game's logic and replays depend on.

static uint32_t nextRandom(uint32_t &seed)
{
    seed = seed * 0x19660Du + 0x3C6EF35Fu;
    return seed ^ (seed >> 10);
}

static uint16_t scaleRandom(uint32_t randomNum, uint16_t lower, uint16_t upper)
{
    const uint32_t span = static_cast<uint32_t>(upper) - static_cast<uint32_t>(lower);
    return static_cast<uint16_t>((((randomNum >> 16) + 1u) * span >> 16) + lower);
}

void RandomSetSeed(uint32 seed)
{
    G(randomVarG).seed = seed;
}

uint32 RandomGet()
{
    return nextRandom(G(randomVarG).seed);
}

uint16 RandomGetRange(uint16 lower, uint16 upper)
{
    return scaleRandom(nextRandom(G(randomVarG).seed), lower, upper);
}

void RandomSetNDSeed(uint32 seedND)
{
    G(randomVarG).seedND = seedND;
}

uint32 RandomGetND()
{
    return nextRandom(G(randomVarG).seedND);
}

uint16 RandomGetNDRange(uint16 lower, uint16 upper)
{
    return scaleRandom(nextRandom(G(randomVarG).seedND), lower, upper);
}

// MATHS.c

// Integer square root: a table gives a first estimate, then one Newton step refines it.
uint16 Sqr32(uint32 n)
{
    uint32_t r = n;
    if (r == 0u)
        return 0u;

    int32_t count = 0;
    while ((r & 0xC0000000u) == 0u)
    {
        r <<= 2;
        ++count;
    }

    r = G(sqrTableG)[((r >> 23) + 1u) >> 1];
    if (count != 0)
        r = (r + (1u << (count - 1))) >> count;

    r = (r + ee_divu(n, r)) >> 1;
    return static_cast<uint16_t>(r > 0xFFFFu ? 0xFFFFu : r);
}

// TRIG.c

// The position of the highest set bit, plus one; zero for zero.
uint32 GetMSB(uint32 src)
{
    uint32_t bits = 0u;
    while (src != 0u)
    {
        src >>= 1;
        ++bits;
    }
    return bits;
}

// VECTOR.c. Normals are fixed point with 12 fractional bits.

uint32 Vec16Magnitude(ptr32<vec16_> p)
{
    const int32_t x = p->x, y = p->y, z = p->z;
    return Sqr32(static_cast<uint32_t>(x * x + y * y + z * z));
}

uint16 normaliseInPlace(normal_ &r)
{
    const int32_t x = r.x, y = r.y, z = r.z;
    const uint16_t magnitude = Sqr32(static_cast<uint32_t>(x * x + y * y + z * z));
    if (magnitude == 0u)
        return 0u;

    int32_t s = static_cast<int16_t>(magnitude);
    if (s < 0)
    {
        // Too large for a signed 16-bit divisor: halve everything.
        s = magnitude >> 1;
        r.x >>= 1;
        r.y >>= 1;
        r.z >>= 1;
    }

    r.x = static_cast<frac>(ee_div(r.x << 12, s));
    r.y = static_cast<frac>(ee_div(r.y << 12, s));
    r.z = static_cast<frac>(ee_div(r.z << 12, s));
    return magnitude;
}

uint16 NormalNormalise(ptr32<normal_> r)
{
    return normaliseInPlace(*r);
}

void NormalDotProduct(ptr32<normal_> a, ptr32<normal_> b, ptr32<frac> cosine)
{
    *cosine = static_cast<frac>((a->x * b->x + a->y * b->y + a->z * b->z) >> 12);
}

// dst may be a or b, so each component is stored before the next is read, as the original does.
void crossInPlace(const normal_ &a, const normal_ &b, normal_ &dst)
{
    dst.x = static_cast<frac>((a.z * b.y - a.y * b.z) >> 12);
    dst.y = static_cast<frac>((a.x * b.z - a.z * b.x) >> 12);
    dst.z = static_cast<frac>((a.y * b.x - a.x * b.y) >> 12);
}

void NormalCrossProduct(ptr32<normal_> a, ptr32<normal_> b, ptr32<normal_> dstVector)
{
    crossInPlace(*a, *b, *dstVector);
}

// A fast length estimate: the largest component plus half of the next.
int32 Vec32QQMagnitude(ptr32<vec32_> vecPtr)
{
    int32_t absX = vecPtr->x, absY = vecPtr->y, absZ = vecPtr->z;
    if (absX < 0)
        absX = -absX;
    if (absY < 0)
        absY = -absY;
    if (absZ < 0)
        absZ = -absZ;

    const int32_t temp = absX < absY ? absY + (absX >> 1) : absX + (absY >> 1);
    return temp < absZ ? absZ + (temp >> 1) : temp + (absZ >> 1);
}

// ACOL.c: world position to collision grid cell. Cells are 512 units wide.
void AColCellXZFromPos(ptr32<_sColGridPSX> pGrid, ee_float x, ee_float z, ptr32<int32> pX, ptr32<int32> pZ)
{
    *pX = ee_ftoi(x - pGrid->min.x) / 512;
    *pZ = ee_ftoi(z - pGrid->min.z) / 512;
}

} // namespace rc

RC_DECOMP_FN(RandomSetSeed)
RC_DECOMP_FN(RandomGet)
RC_DECOMP_FN(RandomGetRange)
RC_DECOMP_FN(RandomSetNDSeed)
RC_DECOMP_FN(RandomGetND)
RC_DECOMP_FN(RandomGetNDRange)
RC_DECOMP_FN(Sqr32)
RC_DECOMP_FN(GetMSB)
RC_DECOMP_FN(Vec16Magnitude)
RC_DECOMP_FN(NormalNormalise)
RC_DECOMP_FN(NormalDotProduct)
RC_DECOMP_FN(NormalCrossProduct)
RC_DECOMP_FN(Vec32QQMagnitude)
RC_DECOMP_FN(AColCellXZFromPos)
