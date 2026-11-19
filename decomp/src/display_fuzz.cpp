// Builds the random-test inputs of DISPLAY.c, CELL.c, VIEW3D.c, INIT3D.c, VERTANIM.c, SKINANIM.c, SVF.c, RDR.c and
// LDIMAGE.c. See display_fuzz.h.

#include "display_fuzz.h"

#include <cstring>

// The runtime's hook on stores to hardware registers (ps2_runtime.cpp), for the oracle only.
extern bool (*g_ps2HardwareStoreHook)(uint32_t address, uint32_t value);

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

namespace fuzz {

namespace {

constexpr uint32_t kValidWindow = 0x7f645459u;
// Functions replaced offline by logged calls: the buffer swap and the GS sync (both wait), FlushCache (a syscall),
// a texture level's upload (DMA) and GfxP2_Init (resets the GS).
constexpr uint32_t kGfxP2_SwapBuffers = sym::GfxP2_SwapBuffers, kSceGsSyncPath = sym::sceGsSyncPath, kFlushCache = sym::FlushCache,
                   kLoadTexMip = sym::GfxP2_LoadTexMip, kGfxP2Init = sym::GfxP2_Init;
constexpr uint32_t kLog = kFuzzScratch + 0x7c00u, kLogMax = 50u; // count word, then 5 words an entry
constexpr uint32_t kHardwareStore = 1u;                          // an entry's kind for a hardware store

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
uint64_t wide(uint64_t &state) { return (static_cast<uint64_t>(nextFuzz(state)) << 32) | nextFuzz(state); }

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t count = mem<uint32_t>(kLog);
    if (count >= kLogMax)
        return;
    const uint32_t at = kLog + 4u + count * 20u;
    mem<uint32_t>(at) = what;
    mem<uint32_t>(at + 4u) = a;
    mem<uint32_t>(at + 8u) = b;
    mem<uint32_t>(at + 12u) = c;
    mem<uint32_t>(at + 16u) = d;
    mem<uint32_t>(kLog) = count + 1u;
}

// A call logged with the argument registers the function reads (the others hold whatever the caller left).
template <uint32_t Address, int Args>
void loggedCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto arg = [ctx](int i) { return i < Args ? getRegU32(ctx, 4 + i) : 0u; };
    logEntry(Address, arg(0), arg(1), arg(2), arg(3));
    SET_GPR_S32(ctx, 2, 0);
    ctx->pc = getRegU32(ctx, 31);
}

bool loggedStore(uint32_t address, uint32_t value)
{
    logEntry(kHardwareStore, address, value, 0, 0);
    return true; // dropped
}

// Offline: the waits, syscalls and hardware become logged calls and stores (see display_fuzz.h); the log starts empty.
void quietHardware()
{
    if (!g_offline)
        return;
    static bool installed = false;
    if (!installed)
    {
        installed = true;
        g_runtime->replaceFunction(kGfxP2_SwapBuffers, &loggedCall<kGfxP2_SwapBuffers, 0>);
        g_runtime->replaceFunction(kSceGsSyncPath, &loggedCall<kSceGsSyncPath, 2>);
        g_runtime->replaceFunction(kFlushCache, &loggedCall<kFlushCache, 1>);
        g_runtime->replaceFunction(kLoadTexMip, &loggedCall<kLoadTexMip, 4>);
        g_runtime->replaceFunction(kGfxP2Init, &loggedCall<kGfxP2Init, 2>);
        g_ps2HardwareStoreHook = &loggedStore;
    }
    mem<uint32_t>(kLog) = 0;
}

// Mostly ordinary values, some zeros of either sign, some raw bit patterns.
ee_float floatValue(uint64_t &state)
{
    const uint32_t kind = rnd(state, 20);
    if (kind == 0)
        return ee_float(0.0f);
    if (kind == 1)
        return ee_float(-0.0f);
    if (kind == 2)
    {
        const uint32_t bits = nextFuzz(state);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }
    const float magnitude = static_cast<float>(nextFuzz(state) >> 8) / static_cast<float>(1u << rnd(state, 30));
    return ee_float(chance(state, 50) ? -magnitude : magnitude);
}

void floats(uint64_t &state, uint32_t at, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        mem<ee_float>(at + i * 4u) = floatValue(state);
}

void bytes(uint64_t &state, uint32_t at, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
        mem<uint8_t>(at + i) = static_cast<uint8_t>(nextFuzz(state));
}

int32_t small(uint64_t &state, uint32_t range) { return static_cast<int32_t>(rnd(state, 2u * range + 1u)) - static_cast<int32_t>(range); }

int16_t edge16(uint64_t &state)
{
    static const int16_t kEdges[] = {0, 1, -1, 2, 0x7fff, -0x8000, 640, 240, 320, 0x100};
    return chance(state, 40) ? kEdges[rnd(state, 10)] : static_cast<int16_t>(nextFuzz(state));
}

// A texture as the loader leaves it: power-of-two sizes mostly, the palette formats (0x13, 0x14) among others.
void texture(uint64_t &state, uint32_t at)
{
    bytes(state, at, 0x4c);
    static const int32_t kSizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 3, 0, -64};
    static const int32_t kFormats[] = {0, 1, 2, 0x13, 0x14, 0x1b, 0x24};
    _sGfxTexture &t = mem<_sGfxTexture>(at);
    t.wd = chance(state, 90) ? kSizes[rnd(state, 11)] : kSizes[rnd(state, 14)];
    t.ht = chance(state, 90) ? kSizes[rnd(state, 11)] : kSizes[rnd(state, 14)];
    t.fmt = chance(state, 90) ? kFormats[rnd(state, 7)] : static_cast<s32>(nextFuzz(state));
    t.frames = rnd(state, 4);
}

// A DMA tag list ending in an END tag (id 0x70), as the loaders make the shapes' triangle lists.
void tagChain(uint64_t &state, uint32_t at)
{
    for (uint32_t i = 0, n = rnd(state, 4); i < n; ++i)
    {
        const uint32_t qwc = rnd(state, 3);
        mem<uint16_t>(at) = static_cast<uint16_t>(qwc);
        mem<uint8_t>(at + 3u) = static_cast<uint8_t>(chance(state, 50) ? 0x10 : 0x30);
        at += (qwc + 1u) * 16u;
    }
    mem<uint16_t>(at) = static_cast<uint16_t>(rnd(state, 3));
    mem<uint8_t>(at + 3u) = 0x70;
}

// Texture fix-ups for a shape or a skin: four textures (some null), and n fix-up areas whose first word indexes them.
void fixups(uint64_t &state, uint32_t scratch, uint32_t &texList, uint32_t &fixList, uint32_t &count)
{
    texList = scratch + 0x180u;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t tex = scratch + 0x400u + i * 0x50u;
        texture(state, tex);
        mem<uint32_t>(texList + i * 4u) = chance(state, 10) ? 0u : tex;
    }
    fixList = scratch + 0x800u;
    count = rnd(state, 6);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t area = scratch + 0xa00u + i * 0x50u;
        mem<uint32_t>(fixList + i * 4u) = area;
        mem<uint32_t>(area) = rnd(state, 4);
        for (const uint32_t offset : {0x10u, 0x20u, 0x24u, 0x30u, 0x34u})
            if (chance(state, 50))
                mem<uint32_t>(area + offset) = 0;
    }
}

uint32_t pickRes(uint64_t &state, int32_t res)
{
    switch (rnd(state, 8))
    {
    case 0: return static_cast<uint32_t>(-1 - static_cast<int32_t>(rnd(state, 100)));
    case 1: return 0u;
    case 2: return static_cast<uint32_t>(res - 1);
    case 3: return static_cast<uint32_t>(res);
    case 4: return static_cast<uint32_t>(res + 1 + static_cast<int32_t>(rnd(state, 100)));
    case 5: return chance(state, 50) ? nextFuzz(state) : static_cast<uint32_t>(res + small(state, 3));
    default: return rnd(state, static_cast<uint32_t>(res > 0 ? res : 1));
    }
}

void randomView(uint64_t &state, uint32_t scratch)
{
    view3dClass &v = G(view3dVarG);
    const uint32_t base = guest_addr(&v);
    bytes(state, base + 0xcu, 0xb8u);
    v.dUX = edge16(state);
    v.dUY = edge16(state);
    if (chance(state, 30))
    {
        v.pPsxViewMatrix = ptr32<_matPsx>(scratch + 0x600u + 2u * rnd(state, 8));
        v.pPsxViewMatrixNegY = ptr32<_matPsx>(scratch + 0x640u);
        v.pPsxTViewMatrix = ptr32<_matPsx>(v.pPsxViewMatrix.address + (chance(state, 50) ? 0u : 2u * rnd(state, 20)));
    }
    G(init3dVarG).currentD = static_cast<uint16>(edge16(state));
    const uint32_t window = scratch + 0x200u;
    bytes(state, window, 0x6c);
    mem<int32_t>(window + 0x40u) = edge16(state);
    mem<int32_t>(window + 0x44u) = edge16(state);
    G(init3dVarG).window3dDefinitionPtr = ptr32<window3dDefinition>(window);
}

} // namespace

// DISPLAY.c

void displayMode(R5900Context *, uint64_t &state, uint32_t)
{
    quietHardware();
    displayClass &d = G(displayVarG);
    bytes(state, guest_addr(&d.state), 0x18);
    bytes(state, guest_addr(&d.dispDef), 0x10);
    d.viewBuffer = nextFuzz(state);
}

void displaySwap(R5900Context *, uint64_t &state, uint32_t)
{
    displayClass &d = G(displayVarG);
    d.viewBuffer = chance(state, 80) ? rnd(state, 2) : nextFuzz(state);
    d.pDB = ptr32<_sDB>(nextFuzz(state));
}

// The window's drawing area is placed so that each edge lands before, on or past the display's edges; the display's
// definition is the real one (640x240) or a small random one.
void displayWindow(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    displayClass &d = G(displayVarG);
    if (chance(state, 30))
    {
        const uint32_t def = scratch + 0x80u;
        bytes(state, def, 0x10);
        mem<int32_t>(def) = chance(state, 80) ? static_cast<int32_t>(rnd(state, 1024)) : static_cast<int32_t>(nextFuzz(state));
        mem<int32_t>(def + 4u) = chance(state, 80) ? static_cast<int32_t>(rnd(state, 512)) : static_cast<int32_t>(nextFuzz(state));
        d.state.pDispDef = ptr32<_sDisplayDefinition>(def);
    }
    const int32_t xRes = d.state.pDispDef->xResolution, yRes = d.state.pDispDef->yResolution;
    const int32_t originX = chance(state, 80) ? small(state, 300) : static_cast<int32_t>(nextFuzz(state));
    const int32_t originY = chance(state, 80) ? small(state, 300) : static_cast<int32_t>(nextFuzz(state));
    const uint32_t x1 = pickRes(state, xRes), x2 = pickRes(state, xRes), y1 = pickRes(state, yRes), y2 = pickRes(state, yRes);
    const int32_t winTopX = small(state, 400), winTopY = small(state, 400);
    const int32_t winBotX = chance(state, 85) ? winTopX + 1 + static_cast<int32_t>(rnd(state, 600)) : winTopX - static_cast<int32_t>(rnd(state, 3));
    const int32_t winBotY = chance(state, 85) ? winTopY + 1 + static_cast<int32_t>(rnd(state, 600)) : winTopY - static_cast<int32_t>(rnd(state, 3));
    const uint32_t window = scratch + 0x100u;
    setArg(ctx, 0, window);
    setArg(ctx, 1, static_cast<uint32_t>(winTopX));
    setArg(ctx, 2, static_cast<uint32_t>(winTopY));
    setArg(ctx, 3, static_cast<uint32_t>(winBotX));
    setArg(ctx, 4, static_cast<uint32_t>(winBotY));
    setArg(ctx, 5, nextFuzz(state));
    setArg(ctx, 6, nextFuzz(state));
    setArg(ctx, 7, x1 - static_cast<uint32_t>(originX));
    // The last five arguments, in the caller's frame (the high words are not part of them).
    const uint32_t sp = getRegU32(ctx, 29);
    const uint32_t rest[5] = {y1 - static_cast<uint32_t>(originY), x2 - static_cast<uint32_t>(originX),
                              y2 - static_cast<uint32_t>(originY), static_cast<uint32_t>(originX), static_cast<uint32_t>(originY)};
    for (uint32_t i = 0; i < 5; ++i)
    {
        mem<uint32_t>(sp + 8u * i) = rest[i];
        mem<uint32_t>(sp + 8u * i + 4u) = nextFuzz(state);
    }
}

void displayRegister(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t window = scratch + 0x100u;
    for (uint32_t i = 0; i < 0x48u; i += 4u)
        mem<int32_t>(window + i) = chance(state, 70) ? small(state, 1000) : static_cast<int32_t>(nextFuzz(state));
    setArg(ctx, 0, window);
    setArg(ctx, 1, nextFuzz(state));
}

// CELL.c

void cellTrans(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t trans = scratch + 0x100u + 4u * rnd(state, 4);
    for (uint32_t i = 0; i < 3; ++i)
        mem<int32_t>(trans + i * 4u) = chance(state, 70) ? small(state, 100000) : static_cast<int32_t>(nextFuzz(state));
    if (chance(state, 40))
    {
        const uint32_t matrix = scratch + 0x200u;
        bytes(state, matrix, 0x20);
        G(view3dVarG).pPsxViewMatrixNegY = ptr32<_matPsx>(matrix);
    }
    setArg(ctx, 0, trans);
}

// VIEW3D.c

void viewVars(R5900Context *, uint64_t &state, uint32_t scratch) { randomView(state, scratch); }

void viewcone(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    randomView(state, scratch);
    const uint32_t d = G(init3dVarG).currentD;
    setArg(ctx, 0, chance(state, 40) ? (d | (chance(state, 30) ? nextFuzz(state) << 16 : 0u)) : nextFuzz(state));
}

// A window (its ID valid in most inputs), a view matrix; the view layer's matrix pointers real, or moved to scratch
// copies that may overlap (the transpose is done in place then), and the window stored or not.
void viewFrame(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t window = scratch + 0x200u, matrix = scratch + 0x300u;
    bytes(state, window, 0x6c);
    mem<int32_t>(window + 0x40u) = edge16(state);
    mem<int32_t>(window + 0x44u) = edge16(state);
    mem<uint32_t>(window + 0x68u) = chance(state, 80) ? kValidWindow : nextFuzz(state);
    bytes(state, matrix, 0x20);
    G(mirrorVarG).bMirrored = chance(state, 80) ? static_cast<bool_>(rnd(state, 2)) : static_cast<bool_>(nextFuzz(state));
    view3dClass &v = G(view3dVarG);
    if (chance(state, 40))
    {
        const uint32_t in = scratch + 0x400u + 2u * rnd(state, 4);
        bytes(state, in, 0x60);
        v.pPsxViewMatrix = chance(state, 50) ? ptr32<_matPsx>(in) : v.pPsxViewMatrix;
        v.pPsxTViewMatrix = ptr32<_matPsx>(chance(state, 30) ? v.pPsxViewMatrix.address : in + 2u * rnd(state, 24));
    }
    init3dClass &i = G(init3dVarG);
    i.winStore = ptr32<void>(chance(state, 50) ? window : nextFuzz(state));
    i.currentD = static_cast<uint16>(edge16(state));
    i.frameCount = nextFuzz(state);
    setArg(ctx, 0, window);
    setArg(ctx, 1, matrix);
    setArg(ctx, 2, nextFuzz(state));
    setArg(ctx, 3, chance(state, 50) ? rnd(state, 4) : nextFuzz(state));
}

// INIT3D.c

void init3d(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietHardware();
    bytes(state, guest_addr(&G(init3dVarG)), sizeof(init3dClass));
    setArg(ctx, 0, chance(state, 85) ? (rnd(state, 0x4000) + 1u) * 4u : nextFuzz(state));
    setArg(ctx, 1, chance(state, 50) ? 0x100u + rnd(state, 0x200) : nextFuzz(state));
    for (int i = 2; i < 6; ++i)
        setArg(ctx, i, chance(state, 70) ? static_cast<uint32_t>(small(state, 100000)) : nextFuzz(state));
    setArg(ctx, 6, chance(state, 70) ? rnd(state, 0x40000) : nextFuzz(state));
}

void init3dClean(R5900Context *, uint64_t &state, uint32_t)
{
    G(init3dVarG).winStore = ptr32<void>(nextFuzz(state));
}

// VERTANIM.c, SKINANIM.c, SVF.c

// The loader's daemon table, partly scrambled so that each set or cleared entry shows.
void resDaemons(R5900Context *, uint64_t &state, uint32_t)
{
    const resClass &res = G(resVarG);
    const uint32_t list = res.pConstructDaemonsList.address;
    for (int32_t i = 0; i < res.types && i < 0x400; ++i)
        if (list != 0 && chance(state, 60))
        {
            mem<uint32_t>(list + static_cast<uint32_t>(i) * 8u) = nextFuzz(state);
            mem<uint32_t>(list + static_cast<uint32_t>(i) * 8u + 4u) = nextFuzz(state);
        }
    G(vaVarsG).active = nextFuzz(state);
    G(pTestTOB) = ptr32<_sRendObjHead>(nextFuzz(state));
}

void vuShape(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t handle = scratch, shape = scratch + 0x100u;
    bytes(state, shape, sizeof(sRdrVUShape));
    sRdrVUShape &s = mem<sRdrVUShape>(shape);
    uint32_t texList = 0, fixList = 0, count = 0;
    fixups(state, scratch, texList, fixList, count);
    s.pTex = ptr32<ptr32<_sGfxTexture>>(texList);
    s.pTexFixList = ptr32<u32>(fixList);
    s.numFixups = count;
    const uint32_t chain = scratch + 0x1400u;
    tagChain(state, chain);
    s.pTri = ptr32<u128>(chance(state, 85) ? chain : 0u);
    mem<uint32_t>(handle) = chance(state, 90) ? shape : 0u;
    G(vaVarsG).active = nextFuzz(state);
    setArg(ctx, 0, handle);
}

void vaInstance(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t handle = scratch, instance = scratch + 0x100u;
    const uint32_t target = chance(state, 10) ? instance + 4u - 0x7cu : scratch + 0x200u + 4u * rnd(state, 64);
    mem<uint32_t>(handle) = instance;
    mem<uint32_t>(instance) = target;
    mem<uint32_t>(instance + 4u) = chance(state, 50) ? (nextFuzz(state) | 0x100u) : (nextFuzz(state) & ~0x100u);
    setArg(ctx, 0, handle);
}

void vuSkin(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t handle = scratch, skin = scratch + 0x100u;
    bytes(state, skin, sizeof(sRdrVUSkin));
    sRdrVUSkin &s = mem<sRdrVUSkin>(skin);
    uint32_t texList = 0, fixList = 0, count = 0;
    fixups(state, scratch, texList, fixList, count);
    s.pTex = ptr32<ptr32<_sGfxTexture>>(texList);
    s.pTexFixList = ptr32<uint32>(fixList);
    s.numFixups = count;
    const uint32_t chain = scratch + 0x1400u;
    tagChain(state, chain);
    s.pTri = ptr32<u128>(chain);
    mem<uint32_t>(handle) = skin;
    setArg(ctx, 0, handle);
}

// A skinned instance with a skin and an animation output of up to four nodes (the skin sometimes missing).
void skinRef(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t ref = scratch + 0x100u, rso = scratch + 0x180u, skin = scratch + 0x200u, output = scratch + 0x300u;
    bytes(state, ref, sizeof(_sRSRef));
    bytes(state, rso, 0xc);
    bytes(state, skin, sizeof(sRdrVUSkin));
    floats(state, skin + 8u, 12);
    mem<uint32_t>(skin + 0x48u) = scratch + 0x2000u + 16u * rnd(state, 64); // pTri
    mem<uint32_t>(rso + 8u) = chance(state, 90) ? skin : 0u;
    const uint32_t nodes = 1u + rnd(state, 4);
    sAnimOutput &out = mem<sAnimOutput>(output);
    out.numNodes = static_cast<s32>(nodes);
    out.aPos = ptr32<ptr32<_sVec3>>(output + 0x20u);
    out.aMat = ptr32<ptr32<_sMat33>>(output + 0x40u);
    for (uint32_t i = 0; i < nodes; ++i)
    {
        const uint32_t pos = scratch + 0x400u + i * 0x10u, mat = scratch + 0x500u + i * 0x30u;
        floats(state, pos, 3);
        floats(state, mat, 9);
        mem<uint32_t>(output + 0x20u + i * 4u) = pos;
        mem<uint32_t>(output + 0x40u + i * 4u) = mat;
    }
    mem<uint32_t>(ref + 0x34u) = rso;
    mem<uint32_t>(ref + 0x3cu) = output;
    setArg(ctx, 0, ref);
    setArg(ctx, 1, nextFuzz(state));
}

// A new skinned instance: an animation of up to six nodes (or a wild count), a rate, and the resource area's
// allocation pointer moved to scratch, with room or not.
void skinInstance(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t handle = scratch, ref = scratch + 0x100u, rso = scratch + 0x180u, anim = scratch + 0x1a0u,
                   skin = scratch + 0x200u;
    bytes(state, ref, sizeof(_sRSRef));
    bytes(state, skin, sizeof(sRdrVUSkin));
    bytes(state, anim, sizeof(sMxAnim));
    mem<uint32_t>(anim) = chance(state, 90) ? rnd(state, 7) : nextFuzz(state) | 0x80000000u; // a wild count is negative
    mem<uint32_t>(anim + 4u) = chance(state, 80) ? rnd(state, 100) : nextFuzz(state);
    mem<uint32_t>(rso) = anim;
    mem<uint32_t>(rso + 8u) = skin;
    mem<uint32_t>(ref + 0x34u) = rso;
    mem<ee_float>(ref + 0x30u) = chance(state, 50) ? ee_float(30.0f) : floatValue(state);
    mem<uint32_t>(ref + 0x40u) = chance(state, 50) ? (nextFuzz(state) | 0x100u) : (nextFuzz(state) & ~0x100u);
    mem<uint32_t>(handle) = ref;
    resClass &res = G(resVarG);
    res.pCurr = ptr32<uint8>(scratch + 0x7000u - 16u * rnd(state, 16));
    res.pTop = ptr32<uint8>(chance(state, 80) ? scratch + 0x3000u : chance(state, 50) ? scratch + 0x6e00u : nextFuzz(state));
    setArg(ctx, 0, handle);
}

void mxCtrl(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t ctrl = scratch + 0x100u, anim = scratch + 0x180u;
    bytes(state, ctrl, sizeof(sMxAnimCtrl));
    static const uint32_t kFrames[] = {0u, 1u, 2u, 0x80000000u, 0xffffffffu, 0x80000001u};
    mem<uint32_t>(anim + 4u) = chance(state, 30) ? kFrames[rnd(state, 6)] : chance(state, 70) ? rnd(state, 1000) : nextFuzz(state);
    mem<uint32_t>(ctrl) = anim;
    mem<int32_t>(ctrl + 4u) = chance(state, 50) ? static_cast<int32_t>(rnd(state, 100000)) : static_cast<int32_t>(nextFuzz(state));
    setArg(ctx, 0, ctrl);
    setArg(ctx, 1, anim);
    ctx->f[12] = chance(state, 30) ? ee_float(30.0f) : floatValue(state);
}

// The node matrices of a frame, placed by an object matrix; the outputs sometimes overlap the object matrix or the
// frame, so the order of the original's loads and stores matters.
void mxOutput(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t anim = scratch + 0x100u, ctrl = scratch + 0x140u, output = scratch + 0x180u, tsoPos = scratch + 0x300u,
                   tsoMat = scratch + 0x340u, frames = scratch + 0x1000u;
    const uint32_t nodes = rnd(state, 6), frame = rnd(state, 4);
    mem<uint32_t>(anim) = nodes;
    mem<uint32_t>(anim + 0x10u) = frames;
    mem<uint32_t>(ctrl) = anim;
    mem<int32_t>(ctrl + 0x18u) = static_cast<int32_t>(frame);
    floats(state, frames, 4u * 6u * 12u);
    floats(state, tsoPos, 3);
    floats(state, tsoMat, 9);
    mem<uint32_t>(output + 8u) = output + 0x20u;
    mem<uint32_t>(output + 0xcu) = output + 0x40u;
    for (uint32_t i = 0; i < 6; ++i)
    {
        uint32_t pos = scratch + 0x400u + i * 0x10u, mat = scratch + 0x600u + i * 0x30u;
        if (chance(state, 8))
            mat = tsoMat + 4u * static_cast<uint32_t>(small(state, 8)); // over the object matrix, any word apart
        else if (chance(state, 8))
            mat = frames + frame * nodes * 0x30u + rnd(state, 6) * 0x30u;
        if (chance(state, 8))
            pos = tsoPos;
        mem<uint32_t>(output + 0x20u + i * 4u) = pos;
        mem<uint32_t>(output + 0x40u + i * 4u) = mat;
    }
    setArg(ctx, 0, output);
    setArg(ctx, 1, ctrl);
    setArg(ctx, 2, nextFuzz(state));
    setArg(ctx, 3, tsoPos);
    setArg(ctx, 4, tsoMat);
}

// An SVF table of up to six slices (none in some inputs: then the slice is the caller's $s0, also in scratch), each
// with a vector count (1 or more: the original divides by it) and a first index.
void svfNormal(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t header = scratch + 0x100u, vectors = scratch + 0x400u, normal = scratch + 0x80u, spare = scratch + 0x1c0u;
    mem<uint16_t>(header) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint16_t>(header + 2u) = static_cast<uint16_t>(rnd(state, 7));
    mem<uint16_t>(header + 4u) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint32_t>(header + 8u) = vectors;
    for (uint32_t i = 0; i < 7; ++i)
    {
        const uint32_t slice = (i < 6 ? header + 0xcu + i * 4u : spare);
        mem<uint16_t>(slice) = static_cast<uint16_t>(chance(state, 30) ? 1u : chance(state, 80) ? 1u + rnd(state, 16) : 1u + rnd(state, 0xffff));
        mem<uint16_t>(slice + 2u) = static_cast<uint16_t>(rnd(state, 64));
    }
    for (uint32_t i = 0; i < 64 * 4; ++i)
        mem<int16_t>(vectors + i * 2u) = chance(state, 70) ? static_cast<int16_t>(small(state, 4096)) : static_cast<int16_t>(nextFuzz(state));
    for (uint32_t i = 0; i < 4; ++i)
        mem<int16_t>(normal + i * 2u) = chance(state, 70) ? static_cast<int16_t>(small(state, 4096)) : edge16(state);
    G(svfVarG).pSvf = ptr32<svfHeader>(header);
    SET_GPR_S32(ctx, 16, static_cast<int32_t>(spare));
    setArg(ctx, 0, normal);
}

void svfTable(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t handle = scratch, header = scratch + 0x100u;
    mem<uint16_t>(header) = static_cast<uint16_t>(chance(state, 90) ? rnd(state, 41) : nextFuzz(state) & 0x3ff);
    // The vectors right after the header, or overlapping it (the count is read again each time round).
    mem<uint32_t>(header + 8u) = chance(state, 10) ? header : header + 0x10u + 8u * rnd(state, 4);
    mem<uint32_t>(handle) = header;
    setArg(ctx, 0, handle);
}

// RDR.c

void rdrInit(R5900Context *, uint64_t &state, uint32_t scratch)
{
    // The block Rdr_Init will get (the allocator takes the lowest that fits) filled with garbage first, so that its
    // clearing shows.
    const ptr32<memHandle> handle(scratch);
    if (MemBlockAllocLo(handle, 32000))
    {
        bytes(state, mem<uint32_t>(scratch), 32000);
        MemBlockFree(handle);
    }
    bytes(state, guest_addr(&G(gRdrCamera)), sizeof(_sRdrCamera));
    sRdr &rdr = G(gRdr);
    rdr.pCamera = ptr32<_sRdrCamera>(nextFuzz(state));
    rdr.frameNumber = nextFuzz(state);
    rdr.currDmaTag = static_cast<s32>(nextFuzz(state));
}

void rdrCamera(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t camera = scratch + 0x100u;
    static const uint32_t kAngles[] = {0u, 1u, 2u, 0x2aaau, 0x4000u, 0x8000u, 0xfffeu, 0xffffu};
    G(gGfxP2).wd = chance(state, 50) ? 640 : chance(state, 70) ? static_cast<s32>(rnd(state, 2048)) : static_cast<s32>(nextFuzz(state));
    setArg(ctx, 0, camera);
    setArg(ctx, 1, chance(state, 30) ? kAngles[rnd(state, 8)] : nextFuzz(state));
}

// A camera (mostly the identity turned a little, at the origin) and two points around it, some behind the near plane;
// the GS packet in scratch with its open tag missing, far from full or at the edge of full.
void rdrLine(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t camera = scratch + 0x100u, p1 = scratch + 0x200u, p2 = scratch + 0x210u, packet = scratch + 0x1000u;
    bytes(state, camera, sizeof(_sRdrCamera));
    _sRdrCamera &c = mem<_sRdrCamera>(camera);
    if (chance(state, 60))
    {
        const ee_float m[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(&c.mat, m, sizeof(m));
        if (chance(state, 50))
            c.mat.Xy = c.mat.Yx = ee_float(static_cast<float>(small(state, 100)) / 400.0f);
        c.pos = {0.0f, 0.0f, 0.0f};
    }
    else
    {
        floats(state, guest_addr(&c.mat), 9);
        floats(state, guest_addr(&c.pos), 3);
    }
    c.minZ = chance(state, 80) ? ee_float(static_cast<float>(1u + rnd(state, 50))) : floatValue(state);
    c.dValX = chance(state, 80) ? ee_float(static_cast<float>(100u + rnd(state, 500))) : floatValue(state);
    c.dValY = chance(state, 80) ? ee_float(static_cast<float>(100u + rnd(state, 500))) : floatValue(state);
    for (const uint32_t p : {p1, p2})
    {
        const float z = static_cast<float>(small(state, 600)) + 300.0f;
        const float spread = z > 0.0f ? z * 3.0f : 100.0f;
        mem<ee_float>(p + 8u) = chance(state, 90) ? ee_float(z) : floatValue(state);
        mem<ee_float>(p) = chance(state, 90) ? ee_float(static_cast<float>(small(state, 1000)) * spread / 1000.0f) : floatValue(state);
        mem<ee_float>(p + 4u) = chance(state, 90) ? ee_float(static_cast<float>(small(state, 1000)) * spread / 1000.0f) : floatValue(state);
    }
    G(gRdr).pCamera = ptr32<_sRdrCamera>(camera);
    sGfxP2 &gfx = G(gGfxP2);
    gfx.pDmaBuff = ptr32<u128>(packet);
    gfx.buffIdx = static_cast<s32>(rnd(state, 20));
    gfx.gifTagIdx = chance(state, 30) ? -1 : static_cast<s32>(rnd(state, 20));
    if (gfx.gifTagIdx != -1)
    {
        static const uint16_t kCounts[] = {0, 1, 0x7ffb, 0x7ffc, 0x7ffd, 0x7ffe, 0x7fff, 0xfffc};
        mem<uint16_t>(packet + static_cast<uint32_t>(gfx.gifTagIdx) * 16u) = chance(state, 60) ? kCounts[rnd(state, 8)] : static_cast<uint16_t>(nextFuzz(state));
    }
    setArg(ctx, 0, p1);
    setArg(ctx, 1, p2);
    setArg(ctx, 2, nextFuzz(state));
}

// The frame's tag chain and the GS packet in scratch; even and odd frames; the animated textures as loaded, or none.
void rdrEndFrame(R5900Context *, uint64_t &state, uint32_t scratch)
{
    quietHardware();
    sRdr &rdr = G(gRdr);
    rdr.pDmaTag = ptr32<sDmaTag>(scratch + 0x100u);
    rdr.currDmaTag = static_cast<s32>(rnd(state, 40));
    rdr.frameNumber = chance(state, 50) ? rnd(state, 2) : nextFuzz(state);
    sGfxP2 &gfx = G(gGfxP2);
    gfx.pDmaBuff = ptr32<u128>(scratch + 0x1000u);
    gfx.gifTagIdx = chance(state, 30) ? -1 : static_cast<s32>(rnd(state, 40));
    if (chance(state, 30))
        G(gNumAnimTextures) = 0;
}

// A tag address for the DMA kick (the hardware stores are logged).
void hardwareArg(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietHardware();
    setArg(ctx, 0, chance(state, 70) ? scratch + 16u * rnd(state, 0x400) : nextFuzz(state));
}

// GFX_P2.c: the GS set-up and the packet's send, offline only (the GS library's hardware stores and waits logged).

// a0, a1 the screen's size (the game's, or odd ones); the background colour.
void gfxInit(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietHardware();
    static const int32_t kWidths[] = {640, 512, 320, 0, -2}, kHeights[] = {224, 240, 256, 448, 480, 0, -1};
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 85) ? kWidths[rnd(state, 5)] : static_cast<int32_t>(nextFuzz(state) >> rnd(state, 32))));
    setArg(ctx, 1, static_cast<uint32_t>(chance(state, 85) ? kHeights[rnd(state, 7)] : static_cast<int32_t>(nextFuzz(state) >> rnd(state, 32))));
    G(gR) = static_cast<u8>(nextFuzz(state));
    G(gG) = static_cast<u8>(nextFuzz(state));
    G(gB) = static_cast<u8>(nextFuzz(state));
    bytes(state, sym::gGfxP2, sizeof(sGfxP2));
}

// The two packets and the texture buffer: null, the dump's, or scratch (free ignores what it did not hand out).
void gfxTerm(R5900Context *, uint64_t &state, uint32_t scratch)
{
    const auto pick = [&](uint32_t current) { return chance(state, 30) ? 0u : chance(state, 50) ? current : scratch + 16u * rnd(state, 0x400); };
    G(gpGfx_DmaBuff0).address = pick(G(gpGfx_DmaBuff0).address);
    G(gpGfx_DmaBuff1).address = pick(G(gpGfx_DmaBuff1).address);
    G(gGfxP2).pTexBuff.address = pick(G(gGfxP2).pTexBuff.address);
}

// The two packets in scratch, the one being filled either (or neither), its last GIF tag anywhere or none (-1), and a
// quadword count either side of the 6 that sends it.
void gfxDmaSend(R5900Context *, uint64_t &state, uint32_t scratch)
{
    quietHardware();
    const uint32_t buff0 = scratch + 0x1000u, buff1 = scratch + 0x3000u;
    bytes(state, buff0, 0x400);
    bytes(state, buff1, 0x400);
    G(gpGfx_DmaBuff0).address = buff0;
    G(gpGfx_DmaBuff1).address = buff1;
    sGfxP2 &gfx = G(gGfxP2);
    gfx.pDmaBuff.address = chance(state, 45) ? buff0 : chance(state, 90) ? buff1 : scratch + 0x5000u;
    gfx.gifTagIdx = chance(state, 30) ? -1 : static_cast<s32>(rnd(state, 0x40));
    gfx.buffIdx = chance(state, 80) ? static_cast<s32>(rnd(state, 12)) : static_cast<s32>(nextFuzz(state));
    G(gLastTex).address = nextFuzz(state);
}

// a0 a texture address (its GS block in bits 8-23), a1 a texture in scratch with its bitmap list, a2 a mip level, a3 a frame.
void gfxLoadTex(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietHardware();
    const uint32_t tex = scratch + 0x200u;
    texture(state, tex);
    for (uint32_t i = 0; i < 28; ++i)
        mem<uint32_t>(tex + 0x48u + 4u * i) = scratch + 0x1000u + 0x100u * i;
    setArg(ctx, 0, chance(state, 70) ? (0x1e0000u + 0x4000u * rnd(state, 0x40)) : nextFuzz(state));
    setArg(ctx, 1, tex);
    setArg(ctx, 2, chance(state, 85) ? rnd(state, 7) : rnd(state, 32)); // the shifts use 5 bits; a bigger level would index past RAM
    setArg(ctx, 3, chance(state, 85) ? rnd(state, 4) : rnd(state, 0x40));
}

// LDIMAGE.c

namespace {

void imageArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch, int first)
{
    // Tags at the start of scratch, in its uncached mirror or not; up to 10x10 tiles (0x60 bytes each).
    const uint32_t tags = scratch + 16u * rnd(state, 16);
    setArg(ctx, 0, chance(state, 30) ? (tags | 0x20000000u) : tags);
    setArg(ctx, 1, nextFuzz(state));
    for (int i = first; i < first + 2; ++i)
        setArg(ctx, i, chance(state, 70) ? static_cast<uint32_t>(small(state, 1024)) : nextFuzz(state));
    for (int i = first + 2; i < first + 4; ++i)
        setArg(ctx, i, static_cast<uint32_t>(small(state, 30) + static_cast<int32_t>(rnd(state, 161))));
}

} // namespace

void imageTags(R5900Context *ctx, uint64_t &state, uint32_t scratch) { imageArgs(ctx, state, scratch, 2); }

void imageTagsYX(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    imageArgs(ctx, state, scratch, 3);
    setArg(ctx, 2, chance(state, 50) ? 0x400u : nextFuzz(state));
}

void packerArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    for (int reg = 5; reg < 12; ++reg)
        SET_GPR_U64(ctx, reg, chance(state, 30) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(rnd(state, 0x40)))) : wide(state));
    setArg(ctx, 0, scratch + 16u * rnd(state, 0x700));
}

// sceVif1PkAddUpkData32N: a packet in scratch whose cursor is in scratch (mostly) or in the game's globals, data in
// scratch or anywhere in RAM, and a count of words up to 0x1000 (mostly under 64, now and then 0) that keeps the copy
// inside the area it starts in. Plain random arguments gave counts of millions of words, which copy all of RAM over
// itself (the original then ran past the 60 s limit; the oracle reported a skip that hid a difference) and wild
// cursors whose words went to the hardware registers in the original and wrapped into RAM in the native: neither is a
// call the game can make. The copy writes only where the harness puts memory back between the two runs (scratch and
// the globals): a write elsewhere that the copy then read back would give the native run other data than the original.
void vifUpkData32N(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t count = chance(state, 10) ? 0u : chance(state, 70) ? 1u + rnd(state, 64) : rnd(state, 0x1001);
    const uint32_t bytes = 4u * count;
    const auto within = [&](uint32_t begin, uint32_t end) { return begin + 4u * rnd(state, (end - begin - bytes) / 4u + 1u); };
    const uint32_t to = chance(state, 75) ? within(scratch, scratch + kFuzzScratchSize) : within(kFuzzDataStart, kFuzzDataEnd);
    const uint32_t from = chance(state, 60) ? within(scratch, scratch + kFuzzScratchSize) : within(sym::sec_text, kFuzzScratch);
    const uint32_t packet = scratch + 0x7000u + 16u * rnd(state, 0x80);
    mem<uint32_t>(packet) = chance(state, 10) ? (to | 0x20000000u) : to; // the uncached mirror now and then
    setArg(ctx, 0, packet);
    setArg(ctx, 1, from);
    setArg(ctx, 2, count);
}

} // namespace fuzz
} // namespace rc
