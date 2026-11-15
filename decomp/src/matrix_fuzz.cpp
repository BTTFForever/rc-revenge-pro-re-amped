// Builds the random-test inputs of MATRIX.c, MATHS.c and PSXDEFS.c. See matrix_fuzz.h.

#include "matrix_fuzz.h"

#include <cstring>

namespace rc::fuzz {

namespace {

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

// A value of up to the given number of bits, either sign.
int32_t scaled(uint64_t &state, uint32_t bits)
{
    const int32_t magnitude = bits >= 31 ? static_cast<int32_t>(nextFuzz(state) >> 1) : static_cast<int32_t>(rnd(state, 1u << bits));
    return chance(state, 50) ? -magnitude : magnitude;
}

// A short vector whose components share a scale of 0 to 15 bits (some zero), so every normalising step is reached.
uint32_t vec16(uint64_t &state, uint32_t at)
{
    const uint32_t bits = rnd(state, 16);
    for (uint32_t i = 0; i < 4; ++i)
        mem<int16_t>(at + i * 2u) = static_cast<int16_t>(chance(state, 10) ? 0 : scaled(state, bits));
    return at;
}

uint32_t vec32(uint64_t &state, uint32_t at)
{
    const uint32_t bits = rnd(state, 32);
    for (uint32_t i = 0; i < 3; ++i)
        mem<int32_t>(at + i * 4u) = chance(state, 10) ? 0 : chance(state, 3) ? static_cast<int32_t>(0x80000000u) : scaled(state, bits);
    return at;
}

void text(uint64_t &state, uint32_t at, const char *base)
{
    std::strcpy(static_cast<char *>(guest_host(at)), base);
    const size_t length = std::strlen(base);
    for (uint32_t i = 0, n = rnd(state, 12); i < n; ++i)
        mem<char>(at + static_cast<uint32_t>(length) + i) = static_cast<char>('a' + rnd(state, 26));
    mem<char>(at + static_cast<uint32_t>(length) + 12u) = 0;
    for (uint32_t i = static_cast<uint32_t>(length); i < length + 12u; ++i)
        if (mem<char>(at + i) == 0)
            break;
}

} // namespace

void noInput(R5900Context *, uint64_t &, uint32_t) {}

void floatBits(R5900Context *ctx, uint64_t &state, uint32_t)
{
    uint32_t bits = nextFuzz(state);
    if (chance(state, 20))
        bits = (bits & 0x807fffffu) | (0x7fu + rnd(state, 9) - 4u) << 23; // near 1
    if (chance(state, 10))
        bits &= 0xff8000ffu; // a power of two as far as the 15 mantissa bits used go
    std::memcpy(&ctx->f[12], &bits, 4);
}

void vec16Pair(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t a = vec16(state, scratch + 0x100u + rnd(state, 0x40) * 8u);
    const uint32_t b = chance(state, 10) ? a : vec16(state, scratch + 0x400u + rnd(state, 0x40) * 8u);
    setArg(ctx, 0, a);
    setArg(ctx, 1, b);
}

void vec32Pair(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t a = vec32(state, scratch + 0x100u + rnd(state, 0x40) * 16u);
    const uint32_t b = chance(state, 10) ? a : vec32(state, scratch + 0x800u + rnd(state, 0x40) * 16u);
    setArg(ctx, 0, a);
    setArg(ctx, 1, b);
}

void normalMatrix(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    // NormalMulMatPsxT reads stale stack words: vary the 0x100 bytes below $sp.
    const uint32_t sp = getRegU32(ctx, 29);
    for (uint32_t at = sp - 0x100u; at < sp; at += 4u)
        mem<uint32_t>(at) = nextFuzz(state);
    const uint32_t matrix = scratch + 0x100u + rnd(state, 0x20) * 0x20u;
    for (uint32_t i = 0; i < 9; ++i)
        mem<int16_t>(matrix + i * 2u) = static_cast<int16_t>(chance(state, 85) ? scaled(state, 12) : scaled(state, 15));
    const uint32_t normal = vec16(state, scratch + 0x800u + rnd(state, 0x40) * 8u);
    setArg(ctx, 0, normal);
    setArg(ctx, 1, matrix);
    setArg(ctx, 2, chance(state, 10) ? normal : scratch + 0xc00u + rnd(state, 0x40) * 8u);
}

void debugFormat(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static const char *const kFormats[] = {"dbg %d %d ", "%x:%x ", "%d ", "value ", "%u %d %x "};
    text(state, scratch + 0x100u, kFormats[rnd(state, 5)]);
    setArg(ctx, 0, scratch + 0x100u);
    for (int i = 1; i < 4; ++i)
        setArg(ctx, i, nextFuzz(state));
}

void warningText(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static const char *const kWarnings[] = {"Overflow in Vec16DivScaler", "Bollox in Vec16DivScaler", "x"};
    text(state, scratch + 0x100u, kWarnings[rnd(state, 3)]);
    setArg(ctx, 0, scratch + 0x100u);
}

} // namespace rc::fuzz
