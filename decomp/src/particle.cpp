// Decompiled from RDR_PARTICLE.c: particle systems. Only rain (type 0) is implemented: particles fall through a box
// and wrap around its sides. The renderer half was never finished (RdrParticleRender and RdrParticleDMAToRenderer
// do nothing), so the systems are simulated but not drawn by this code.

#include "ee_math.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
uint32 RandomGet();

namespace {

// Wraps one coordinate of a particle back into the box, moving its previous position by the same amount.
void wrap(ee_float &pos, ee_float &old, ee_float min, ee_float max, ee_float size)
{
    const ee_float p = pos;
    if (p.v < min.v)
    {
        pos = p + size;
        old = old + size;
    }
    else if (max.v < p.v)
    {
        pos = p - size;
        old = old - size;
    }
}

} // namespace

// Moves each raindrop by its velocity and wraps it around the box. All three axes add velocity.x: an original
// slip, kept (rain falls diagonally).
void RdrParticleRainProcess(ptr32<_sRdrParticleSystem> pParticleSystem)
{
    _sRdrParticleSystem &sys = *pParticleSystem;
    const ee_float sizeX = sys.bbMax.x - sys.bbMin.x;
    const ee_float sizeY = sys.bbMax.y - sys.bbMin.y;
    const u32 count = sys.numParticles;
    const ee_float sizeZ = sys.bbMax.z - sys.bbMin.z;
    for (u32 i = 0; i < count; ++i)
    {
        _sRdrParticleRenderData &render = sys.pParticlesRenderData[static_cast<int32_t>(i)];
        _sRdrParticleLocalData &local = sys.pParticlesLocalData[static_cast<int32_t>(i)];
        local.oldPosition.x = render.position.x;
        const ee_float step = local.velocity.x;
        local.oldPosition.y = render.position.y;
        local.oldPosition.z = render.position.z;
        render.position.x = render.position.x + step;
        render.position.y = render.position.y + local.velocity.x;
        render.position.z = render.position.z + local.velocity.x;
        wrap(render.position.x, local.oldPosition.x, sys.bbMin.x, sys.bbMax.x, sizeX);
        wrap(render.position.y, local.oldPosition.y, sys.bbMin.y, sys.bbMax.y, sizeY);
        wrap(render.position.z, local.oldPosition.z, sys.bbMin.z, sys.bbMax.z, sizeZ);
    }
}

// Never finished: clears a local matrix and returns.
void RdrParticleDMAToRenderer(ptr32<_sRdrParticleSystem>)
{
    GuestFrame frame(0x50);
    std::memset(frame.local<uint8_t>(0).get(), 0, 0x40);
}

// Unlinks a system. Removing the first one takes the new first from *$a1, a register the caller never sets: an
// original bug, kept (the game never removes a system, so it doesn't matter).
void RdrParticleRemoveSystem(ptr32<_sRdrParticleSystem> pRemoveSystem, ptr32<ptr32<_sRdrParticleSystem>> strayA1)
{
    sRdrParticleSystemClass &systems = G(gRdrParticleSystemClass);
    if (systems.pFirstSystem == pRemoveSystem)
    {
        systems.pFirstSystem = *strayA1;
        return;
    }
    ptr32<_sRdrParticleSystem> previous = systems.pFirstSystem;
    if (!previous)
        return;
    for (ptr32<_sRdrParticleSystem> at = previous->pNextSystem; at != pRemoveSystem; at = at->pNextSystem)
    {
        if (!at)
            return;
        previous = at;
    }
    previous->pNextSystem = pRemoveSystem->pNextSystem;
}

void RdrParticleRender() {}

// Scatters the raindrops through the box, each moving (15, -100, 15) a frame.
void RdrParticleRainPopulate(ptr32<_sRdrParticleSystem> pParticleSystem)
{
    _sRdrParticleSystem &sys = *pParticleSystem;
    const ee_float minX = sys.bbMin.x, maxX = sys.bbMax.x, maxY = sys.bbMax.y, minY = sys.bbMin.y, maxZ = sys.bbMax.z, minZ = sys.bbMin.z;
    const auto ee_mod = [](int32_t a, int32_t b) { return b == 0 ? a : (a == INT32_MIN && b == -1 ? 0 : a % b); };
    u32 count = 0;
    for (u32 i = 0; i < sys.numParticles; ++i)
    {
        _sRdrParticleLocalData &local = sys.pParticlesLocalData[static_cast<int32_t>(i)];
        _sRdrParticleRenderData &render = sys.pParticlesRenderData[static_cast<int32_t>(i)];
        render.position.x = sys.bbMin.x + ee_itof(ee_mod(static_cast<int32_t>(RandomGet()), ee_ftoi(maxX - minX)));
        render.position.y = sys.bbMin.y + ee_itof(ee_mod(static_cast<int32_t>(RandomGet()), ee_ftoi(maxY - minY)));
        const int32_t r = static_cast<int32_t>(RandomGet());
        count = sys.numParticles;
        render.position.z = sys.bbMin.z + ee_itof(ee_mod(r, ee_ftoi(maxZ - minZ)));
        local.velocity.x = 15.0f;
        local.velocity.y = -100.0f;
        local.velocity.z = 15.0f;
        local.oldPosition.x = render.position.x - ee_float(15.0f);
        local.oldPosition.y = render.position.y - ee_float(-100.0f);
        local.oldPosition.z = render.position.z - ee_float(15.0f);
    }
    sys.numParticlesAlive = count;
}

// Frees every system. Each pointer is passed to MemBlockFree through a local, as the original does.
void RdrParticleTerm()
{
    GuestFrame frame(0x30);
    const ptr32<ptr32<_sRdrParticleSystem>> handle = frame.local<ptr32<_sRdrParticleSystem>>(0);
    for (ptr32<_sRdrParticleSystem> sys = G(gRdrParticleSystemClass).pFirstSystem; sys;)
    {
        const ptr32<_sRdrParticleSystem> next = sys->pNextSystem;
        *handle = sys;
        MemBlockFree(handle.cast<memHandle>());
        sys = next;
    }
}

void RdrParticleProcess()
{
    for (ptr32<_sRdrParticleSystem> sys = G(gRdrParticleSystemClass).pFirstSystem; sys; sys = sys->pNextSystem)
        if (sys->type == 0)
            RdrParticleRainProcess(sys);
}

// Allocates a system with room for maxParticles (render data, then local data, after the header) and puts it first
// in the list.
ptr32<_sRdrParticleSystem> RdrParticleNewSystem(u32 type, u32 maxParticles, ptr32<_sVec3> boundBoxMin, ptr32<_sVec3> boundBoxMax)
{
    GuestFrame frame(0x70);
    const _sVec3 min = *boundBoxMin, max = *boundBoxMax;
    const ptr32<ptr32<_sRdrParticleSystem>> handle = frame.local<ptr32<_sRdrParticleSystem>>(0x20);
    if (!MemBlockAllocLo(handle.cast<memHandle>(), maxParticles * 0x34u + 0x40u))
        return nullptr;
    const ptr32<_sRdrParticleSystem> sysPtr = *handle;
    _sRdrParticleSystem &sys = *sysPtr;
    sys.numParticles = maxParticles;
    sys.pTexture = nullptr;
    sys.pParticlesLocalData = ptr32<_sRdrParticleLocalData>(sysPtr.address + 0x40u + maxParticles * 0x10u);
    sys.type = type;
    sys.pParticlesRenderData = ptr32<_sRdrParticleRenderData>(sysPtr.address + 0x40u);
    sys.numParticlesAlive = 0;
    sys.bbMin = min;
    sys.bbMax = max;
    sys.pNextSystem = G(gRdrParticleSystemClass).pFirstSystem;
    G(gRdrParticleSystemClass).pFirstSystem = sysPtr;
    if (type == 0)
        RdrParticleRainPopulate(sysPtr);
    return *handle;
}

void RdrParticleInit()
{
    G(gRdrParticleSystemClass).pFirstSystem = nullptr;
    G(gVIF1_DMA) = ptr32<anon_0db2ace9>(guest_call<uint32_t>(sym::sceDmaGetChan, 1)); // sceDmaGetChan(1)
}

} // namespace rc

RC_DECOMP_FN_NOFUZZ(RdrParticleRainProcess)
RC_DECOMP_FN(RdrParticleDMAToRenderer)
RC_DECOMP_FN_NOFUZZ(RdrParticleRemoveSystem)
RC_DECOMP_FN(RdrParticleRender)
RC_DECOMP_FN_NOFUZZ(RdrParticleRainPopulate)
RC_DECOMP_FN_NOFUZZ(RdrParticleTerm)
RC_DECOMP_FN_NOFUZZ(RdrParticleProcess)
RC_DECOMP_FN_NOFUZZ(RdrParticleNewSystem)
RC_DECOMP_FN_NOFUZZ(RdrParticleInit)
