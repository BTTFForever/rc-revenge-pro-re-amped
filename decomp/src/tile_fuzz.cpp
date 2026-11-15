// Builds the random-test inputs of TILE.c. See tile_fuzz.h.

#include "tile_fuzz.h"

#include <cmath>

namespace rc::fuzz {

namespace {

constexpr uint32_t kTile = 0x1f46f0u, kView3d = 0x1f4808u, kLightVecs = 0x1f45a0u;
// Scratch layout: the tile list, a track unit, a handle, the shapes.
constexpr uint32_t kList = 0x0u, kUnit = 0x200u, kHandle = 0x300u, kShapes = 0x400u, kShapeSize = 0x60u;
constexpr uint32_t kMaxTiles = 12;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
float frnd(uint64_t &state, float scale) { return (static_cast<float>(static_cast<int32_t>(nextFuzz(state) % 20001u) - 10000) / 10000.0f) * scale; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

int16_t fixed(uint64_t &state) { return static_cast<int16_t>(chance(state, 90) ? static_cast<int32_t>(rnd(state, 0x2001)) - 0x1000 : static_cast<int32_t>(nextFuzz(state))); }

// The four orientation matrices: the game's (from TileInit) or random 1.3.12 entries.
void varyMatrices(uint64_t &state)
{
    if (chance(state, 50))
        return;
    for (uint32_t m = 0; m < 4; ++m)
        for (uint32_t i = 0; i < 9; ++i)
            mem<int16_t>(kTile + m * 0x20u + i * 2u) = fixed(state);
}

// A tile list in scratch: its count, cell size and shapes (version 2.0, 3.0 or neither, random contents).
uint32_t buildTiles(uint64_t &state, uint32_t scratch)
{
    const uint32_t count = 1 + rnd(state, kMaxTiles);
    const uint32_t list = scratch + kList;
    mem<uint16_t>(list) = static_cast<uint16_t>(count);
    static const uint16_t kSizes[] = {0x800, 0x400, 0x1000, 0x800};
    mem<uint16_t>(list + 2u) = chance(state, 80) ? kSizes[rnd(state, 4)] : static_cast<uint16_t>(nextFuzz(state));
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t shape = scratch + kShapes + i * kShapeSize;
        mem<uint32_t>(list + 4u + i * 4u) = shape;
        const uint32_t kind = rnd(state, 10);
        mem<float>(shape) = kind < 4 ? 2.0f : kind < 8 ? 3.0f : kind == 8 ? 1.0f : frnd(state, 4.0f);
        for (uint32_t f = 0x8; f < 0x44; f += 4)
            mem<float>(shape + f) = frnd(state, 4000.0f);
        mem<uint32_t>(shape + 0x48u) = chance(state, 10) ? 0u : scratch + 0x2000u + rnd(state, 0x100) * 16u; // pTri
        mem<int16_t>(shape + 0x6au) = static_cast<int16_t>(nextFuzz(state)); // TileGetYStart's word (past the shape)
    }
    mem<uint32_t>(kTile + 0x100u) = list;
    return count;
}

void varyLights(uint64_t &state)
{
    if (chance(state, 50))
        return;
    for (uint32_t i = 0; i < 12; ++i)
        mem<float>(kLightVecs + i * 4u) = chance(state, 5) ? -0.0f : frnd(state, 1.0f);
}

uint32_t buildUnit(uint64_t &state, uint32_t scratch, uint32_t count)
{
    const uint32_t unit = scratch + kUnit;
    mem<int16_t>(unit) = static_cast<int16_t>(rnd(state, count));
    mem<int16_t>(unit + 2u) = static_cast<int16_t>(chance(state, 80) ? static_cast<int32_t>(rnd(state, 0x1000)) - 0x800 : static_cast<int32_t>(nextFuzz(state)));
    mem<uint32_t>(unit + 4u) = chance(state, 90) ? rnd(state, 4) : 4 + rnd(state, 4);
    return unit;
}

uint32_t cell(uint64_t &state) { return chance(state, 90) ? rnd(state, 32) : rnd(state, 256); }
uint32_t junk(uint64_t &state) { return chance(state, 10) ? nextFuzz(state) & 0xffffff00u : 0u; }

} // namespace

void tileNone(R5900Context *, uint64_t &state, uint32_t scratch)
{
    varyMatrices(state);
    if (chance(state, 50))
        buildTiles(state, scratch);
    for (uint32_t i = 0; i < 9; ++i)
        mem<int16_t>(kView3d + 0x4cu + i * 2u) = fixed(state); // psxTViewMatrix
    for (uint32_t i = 0; i < 3; ++i)
        mem<int32_t>(kView3d + 0x20u + i * 4u) = static_cast<int32_t>(nextFuzz(state)); // the view's translation
}

void tileYStart(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t count = buildTiles(state, scratch);
    setArg(ctx, 0, rnd(state, count) | junk(state) << 8);
}

void tileRender(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    varyMatrices(state);
    varyLights(state);
    const uint32_t count = buildTiles(state, scratch);
    setArg(ctx, 0, cell(state) | junk(state));
    setArg(ctx, 1, cell(state) | junk(state));
    setArg(ctx, 2, cell(state) | junk(state));
    setArg(ctx, 3, cell(state) | junk(state));
    setArg(ctx, 4, buildUnit(state, scratch, count));
    setArg(ctx, 5, rnd(state, 6));
}

void tileEditor(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    varyMatrices(state);
    varyLights(state);
    const uint32_t count = buildTiles(state, scratch);
    setArg(ctx, 0, cell(state) | junk(state));
    setArg(ctx, 1, cell(state) | junk(state));
    setArg(ctx, 2, buildUnit(state, scratch, count));
    setArg(ctx, 3, rnd(state, 6));
}

void tileDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    // Ids whose handle is already a pointer (low bits clear), which ResLoad returns as it is (see hud_fuzz.cpp).
    const resClass &res = G(resVarG);
    const uint32_t maxId = res.lookupHeader.maxId;
    const uint32_t handles = res.handlesHandle.address;
    const bool ok = handles < 0x02000000u && maxId >= 2 && maxId <= 0x10000;
    const uint32_t count = chance(state, 10) ? 0u : 1 + rnd(state, kMaxTiles);
    const uint32_t list = scratch + kList;
    mem<uint16_t>(list) = static_cast<uint16_t>(count);
    mem<uint16_t>(list + 2u) = static_cast<uint16_t>(nextFuzz(state));
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t id = 0;
        for (int tries = 0; ok && tries < 200; ++tries)
        {
            const uint32_t candidate = 1 + rnd(state, maxId - 1);
            if ((mem<uint32_t>(handles + candidate * 4u) & 3u) == 0)
            {
                id = candidate;
                break;
            }
        }
        mem<uint32_t>(list + 4u + i * 4u) = id;
    }
    mem<uint32_t>(scratch + kHandle) = list;
    setArg(ctx, 0, scratch + kHandle);
}

} // namespace rc::fuzz
