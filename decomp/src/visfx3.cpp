// Decompiled from VISFX2.c and VISFX3.c, continued: the scenery effects' list and factory, the handlers of the steam,
// smoke and fire scenery, and the renderers that draw particle rings as sprites. (See visfx2.cpp for how a `_sVfx_TSFX`
// and its ring of particles are laid out.)

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

#include <algorithm>

namespace rc {

using vfx::at;
using vfx::ringAtTail;
using vfx::ringHead;
using vfx::ringNext;
using vfx::ringPush;
using vfx::sortAtScenery;
using vfx::sortPosFromCar;

namespace {

constexpr uint32_t kLifeSpan = 0x10;
constexpr uint32_t kRing = 0x40; // the ring header: mask, count, head, tail
constexpr uint32_t kParticles = 0x44;

} // namespace

namespace {

constexpr uint32_t kTaskSet = sym::TaskSet, kTaskDelete = sym::TaskDelete, kTaskCachedInsert = sym::TaskCachedInsert;

// The scenery instance's position in whole units, as shorts (the renderers keep it in a local).
struct SceneryPos
{
    int16_t x, y, z;
};

SceneryPos sceneryPosition(uint32_t mem)
{
    const uint32_t tob = at<uint32_t>(mem + 0x10);
    return {static_cast<int16_t>(at<int32_t>(tob + 4) >> 8), static_cast<int16_t>(at<int32_t>(tob + 8) >> 8),
            static_cast<int16_t>(at<int32_t>(tob + 0xc) >> 8)};
}

// A particle's start slot: the ring's tail, as the signed byte the original multiplies by the particle size.
uint32_t tailSlot(uint32_t mem, uint32_t stride)
{
    return mem + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(mem + kRing + 3))) * stride;
}

void sceneryMovers(uint32_t mem)
{
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(mem + kRing), ptr32<_VFX_SIMPLEPARTICLE>(mem + kParticles));
}

} // namespace

// Starts the scenery effects' task, which runs their handlers every 25 ticks, with an empty list.
void Vfx2TSFXLvlInit()
{
    G(visFxVarG).vfxTSFXHeader = nullptr;
    G(visFxVarG).vfxTSFXFooter = nullptr;
    guest_call(sym::TSOPreProcess, sym::Vfx2CallBack);                                          // TSOPreProcess(Vfx2CallBack)
    guest_call(kTaskSet, ptr32<uint8>((sym::visFxVarG + 0x18u)), sym::Vfx2UpdateTSFX, 1, 0);            // TaskSet(&TSFXTask, Vfx2UpdateTSFX, 1, 0)
    guest_call(kTaskCachedInsert, ptr32<uint8>((sym::visFxVarG + 0x18u)));
}

// Stops the task and frees every scenery effect.
void Vfx2TSFXLvlTerm()
{
    guest_call(kTaskDelete, ptr32<uint8>((sym::visFxVarG + 0x18u)));
    vfx::freeList(G(visFxVarG).vfxTSFXHeader.address);
    G(visFxVarG).vfxTSFXFooter = nullptr;
    G(visFxVarG).vfxTSFXHeader = nullptr;
}

// Runs every scenery effect's handler (the ones that have one), then sets the task to run again 25 ticks later. A
// handler may add effects, so each next pointer is read after the handler has run.
void Vfx2UpdateTSFX(ptr32<void>, int32 time)
{
    {
        GuestFrame frame(0x50);
        for (uint32_t fx = G(visFxVarG).vfxTSFXHeader.address; fx != 0; fx = at<uint32_t>(fx))
            if (const uint32_t handler = at<uint32_t>(fx + 8))
                guest_call(handler, ptr32<_sVfx_TSFX>(fx));
    }
    at<int32_t>(sym::visFxVarG + 0x1c) = time + 0x19; // TSFXTask.time
    guest_call(kTaskCachedInsert, ptr32<uint8>((sym::visFxVarG + 0x18u)));
}

// Adds a scenery effect of the given type for a scenery instance. The effect is allocated at the end of the list: the
// block's address goes straight into the last effect's `Next` (or into the list head), which MemBlockAllocLo's handle
// is. Types 0 to 0x1c each have a size, a handler and renderer and some start values of their own, below; a larger type
// allocates nothing and links in whatever is at the handle (a null effect: a failed allocation does the same).
// `flags` is three bytes of the scenery's own settings the handlers look at, `iSize` one more for some types.
ptr32<_sVfx_TSFX> Vfx2AddTSFX(ptr32<_sTOBInst> pTOBInst, uint32 flags, uint32 iSize, uint32 iType)
{
    visFxClass &list = G(visFxVarG);
    const uint32_t handle = list.vfxTSFXHeader ? list.vfxTSFXFooter.address : guest_addr(&list.vfxTSFXHeader);
    const uint32_t tob = pTOBInst.address;
    if (iType < 0x1d)
    {
        int32_t r = 0;
        const auto alloc = [&](uint32_t size) { MemBlockAllocLo(ptr32<memHandle>(handle), size); };
        const auto put8 = [&](uint32_t offset, uint32_t value) { at<uint8_t>(at<uint32_t>(handle) + offset) = static_cast<uint8_t>(value); };
        const auto put16 = [&](uint32_t offset, uint32_t value) { at<uint16_t>(at<uint32_t>(handle) + offset) = static_cast<uint16_t>(value); };
        const auto put32 = [&](uint32_t offset, uint32_t value) { at<uint32_t>(at<uint32_t>(handle) + offset) = value; };
        switch (iType)
        {
        case 0: // size 0x1a0
            alloc(0x1a0);
            put32(0x40, 15);
            put32(0x8, sym::Vfx2Handler_CartoonTrainSmoke); // handler Vfx2Handler_CartoonTrainSmoke
            put32(0xc, sym::Vfx2Renderer_CartoonTrainSmoke); // renderer Vfx2Renderer_CartoonTrainSmoke
            put16(0x184, at<int32_t>(tob + 0x4) >> 8);
            put16(0x186, at<int32_t>(tob + 0x8) >> 8);
            put16(0x188, at<int32_t>(tob + 0xc) >> 8);
            put16(0x18c, 0);
            put16(0x18e, 0);
            put16(0x190, 0);
            break;
        case 1: // size 0x50
            alloc(0x50);
            put8(0x48, flags);
            put32(0x8, sym::Vfx2Handler_Lights); // handler Vfx2Handler_Lights
            put32(0xc, sym::Vfx2Renderer_LightGlow); // renderer Vfx2Renderer_LightGlow
            put8(0x49, flags >> 8);
            r = vfx::random15();
            put8(0x4a, flags >> 16);
            put8(0x4b, r);
            put32(0x4c, iSize);
            break;
        case 2: // size 0x50
            alloc(0x50);
            put8(0x48, flags);
            put32(0x8, sym::Vfx2Handler_Lights); // handler Vfx2Handler_Lights
            put32(0xc, sym::Vfx2Renderer_LightFlicker); // renderer Vfx2Renderer_LightFlicker
            put8(0x49, flags >> 8);
            r = vfx::random15();
            put8(0x4a, flags >> 16);
            put8(0x4b, r);
            put32(0x4c, iSize);
            break;
        case 3: // size 0x190
            alloc(0x190);
            put32(0x8, sym::Vfx2Handler_Steaming); // handler Vfx2Handler_Steaming
            put32(0xc, sym::Vfx2Renderer_Steaming); // renderer Vfx2Renderer_Steaming
            put32(0x40, 15);
            break;
        case 4: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_PipeSteam); // handler Vfx2Handler_PipeSteam
            put32(0xc, sym::Vfx2Renderer_PipeSteam); // renderer Vfx2Renderer_PipeSteam
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            r = vfx::random15();
            put32(0x40, 15);
            put16(0x186, ((r & 0xf) + 1));
            break;
        case 5: // size 0x2d0
            alloc(0x2d0);
            put32(0x8, sym::Vfx2Handler_Bats); // handler Vfx2Handler_Bats
            r = vfx::random15();
            put32(0xc, sym::Vfx2Renderer_Bats); // renderer Vfx2Renderer_Bats
            put32(0x40, 31);
            put16(0x2c6, ((r & 0xf) + 1));
            break;
        case 6: // size 0x190
            alloc(0x190);
            put32(0x8, sym::Vfx2Handler_Gulls); // handler Vfx2Handler_Gulls
            put32(0xc, sym::Vfx2Renderer_Gulls); // renderer Vfx2Renderer_Gulls
            put32(0x40, 15);
            put16(0x184, 0);
            break;
        case 7: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_Smoking); // handler Vfx2Handler_Smoking
            put32(0xc, sym::Vfx2Renderer_Smoking); // renderer Vfx2Renderer_Smoking
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            put8(0x18b, iSize);
            put32(0x40, 15);
            break;
        case 8: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_Smoking); // handler Vfx2Handler_Smoking
            put32(0xc, sym::Vfx2Renderer_Smoking); // renderer Vfx2Renderer_Smoking
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            put8(0x18b, 160);
            put32(0x40, 15);
            break;
        case 9: // size 0x350
            alloc(0x350);
            put8(0x348, flags);
            put32(0x8, sym::Vfx2Handler_JungleFire); // handler Vfx2Handler_JungleFire
            put32(0xc, sym::Vfx2Renderer_JungleFire); // renderer Vfx2Renderer_JungleFire
            put8(0x349, flags >> 8);
            put8(0x34a, flags >> 16);
            put8(0x34b, 245);
            put32(0x40, 15);
            break;
        case 10: // size 0x350
            alloc(0x350);
            put8(0x348, flags);
            put32(0x8, sym::Vfx2Handler_JungleFire); // handler Vfx2Handler_JungleFire
            put32(0xc, sym::Vfx2Renderer_JungleFire); // renderer Vfx2Renderer_JungleFire
            put8(0x349, flags >> 8);
            put8(0x34a, flags >> 16);
            put8(0x34b, 105);
            put32(0x40, 15);
            break;
        case 11: // size 0x60
            alloc(0x60);
            put8(0x54, flags);
            put32(0x8, sym::Vfx2Handler_TumbleWeed); // handler Vfx2Handler_TumbleWeed
            put32(0xc, sym::Vfx2Renderer_TumbleWeed); // renderer Vfx2Renderer_TumbleWeed
            put16(0x4a, 50);
            put16(0x40, 0);
            put16(0x42, 0);
            put16(0x44, 0);
            put16(0x48, 0);
            put16(0x4c, 0);
            put8(0x55, flags >> 8);
            put8(0x56, flags >> 16);
            put8(0x57, 128);
            break;
        case 12: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_SciFiSteam); // handler Vfx2Handler_SciFiSteam
            put32(0xc, sym::Vfx2Renderer_SciFiSteam); // renderer Vfx2Renderer_SciFiSteam
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            r = vfx::random15();
            put32(0x40, 15);
            put16(0x186, ((r & 0xf) + 1));
            break;
        case 13: // size 0x190
            alloc(0x190);
            put32(0x8, sym::Vfx2Handler_Feathers); // handler Vfx2Handler_Feathers
            put32(0xc, sym::Vfx2Renderer_Feathers); // renderer Vfx2Renderer_Feathers
            put32(0x40, 15);
            put16(0x184, 0);
            break;
        case 14: // size 0x50
            alloc(0x50);
            put8(0x48, flags);
            put32(0x8, sym::Vfx2Handler_Lights); // handler Vfx2Handler_Lights
            put32(0xc, sym::Vfx2Renderer_LightCandle); // renderer Vfx2Renderer_LightCandle
            put8(0x49, flags >> 8);
            r = vfx::random15();
            put8(0x4a, flags >> 16);
            put8(0x4b, r);
            break;
        case 15: // size 0x350
            alloc(0x350);
            put8(0x344, flags);
            put32(0x8, sym::Vfx2Handler_Fountain); // handler Vfx2Handler_Fountain
            put32(0xc, sym::Vfx2Renderer_Fountain); // renderer Vfx2Renderer_Fountain
            put8(0x345, flags >> 8);
            put8(0x346, flags >> 16);
            put32(0x40, 31);
            break;
        case 16: // size 0x50
            alloc(0x50);
            put8(0x48, flags);
            put32(0x8, sym::Vfx2Handler_Lights); // handler Vfx2Handler_Lights
            put32(0xc, sym::Vfx2Renderer_LightFlare); // renderer Vfx2Renderer_LightFlare
            put8(0x49, flags >> 8);
            r = vfx::random15();
            put8(0x4a, flags >> 16);
            put8(0x4b, r);
            put32(0x4c, iSize);
            break;
        case 17: // size 0x350
            alloc(0x350);
            put8(0x348, flags);
            put32(0x8, sym::Vfx2Handler_ScifiRocket); // handler Vfx2Handler_ScifiRocket
            put32(0xc, sym::Vfx2Renderer_ScifiRocket); // renderer Vfx2Renderer_ScifiRocket
            put8(0x349, flags >> 8);
            put8(0x34a, flags >> 16);
            put8(0x34b, 255);
            put32(0x40, 15);
            break;
        case 18: // size 0x350
            alloc(0x350);
            put8(0x348, flags);
            put32(0x8, sym::Vfx2Handler_LavaSpit); // handler Vfx2Handler_LavaSpit
            put32(0xc, sym::Vfx2Renderer_LavaSpit); // renderer Vfx2Renderer_LavaSpit
            put8(0x349, flags >> 8);
            put8(0x34a, flags >> 16);
            put8(0x34b, iSize);
            put32(0x40, 31);
            break;
        case 19: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_Gargoyle); // handler Vfx2Handler_Gargoyle
            put32(0xc, sym::Vfx2Renderer_Gargoyle); // renderer Vfx2Renderer_Gargoyle
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            r = vfx::random15();
            put32(0x40, 15);
            put16(0x186, ((r & 0xf) + 1));
            break;
        case 20: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_SciFiBubbles); // handler Vfx2Handler_SciFiBubbles
            put32(0xc, sym::Vfx2Renderer_SciFiBubbles); // renderer Vfx2Renderer_SciFiBubbles
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            r = vfx::random15();
            put32(0x40, 15);
            put16(0x186, ((r & 0xf) + 1));
            break;
        case 21: // size 0x550
            alloc(0x550);
            put32(0x8, sym::Vfx2Handler_Bees); // handler Vfx2Handler_Bees
            put32(0xc, sym::Vfx2Renderer_Bees); // renderer Vfx2Renderer_Bees
            put32(0x40, 63);
            put16(0x544, 0);
            break;
        case 22: // size 0x2e0
            alloc(0x2e0);
            put32(0x8, sym::Vfx2Handler_MonkeySmoke); // handler Vfx2Handler_MonkeySmoke
            put32(0xc, sym::Vfx2Renderer_MonkeySmoke); // renderer Vfx2Renderer_MonkeySmoke
            put32(0x40, 15);
            break;
        case 23: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_BigVolcano); // handler Vfx2Handler_BigVolcano
            put32(0xc, sym::Vfx2Renderer_BigVolcano); // renderer Vfx2Renderer_BigVolcano
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            put8(0x18b, iSize);
            put32(0x40, 15);
            break;
        case 24: // size 0x550
            alloc(0x550);
            put32(0x8, sym::Vfx2Handler_Flies); // handler Vfx2Handler_Flies
            put32(0xc, sym::Vfx2Renderer_Flies); // renderer Vfx2Renderer_Flies
            put32(0x40, 63);
            put16(0x544, 0);
            break;
        case 25: // size 0x190
            alloc(0x190);
            put8(0x188, flags);
            put32(0x8, sym::Vfx2Handler_Cannon); // handler Vfx2Handler_Cannon
            put32(0xc, sym::Vfx2Renderer_Cannon); // renderer Vfx2Renderer_Cannon
            put8(0x189, flags >> 8);
            put8(0x18a, flags >> 16);
            r = vfx::random15();
            put32(0x40, 15);
            put16(0x186, ((r & 0xf) + 1));
            break;
        case 26: // size 0x650
            alloc(0x650);
            put8(0x648, flags);
            put32(0x8, sym::Vfx2Handler_PirateLava); // handler Vfx2Handler_PirateLava
            put32(0xc, sym::Vfx2Renderer_PirateLava); // renderer Vfx2Renderer_PirateLava
            put8(0x649, flags >> 8);
            put8(0x64a, flags >> 16);
            put8(0x64b, iSize);
            put32(0x40, 63);
            break;
        case 27: // size 0x350
            alloc(0x350);
            put8(0x344, flags);
            put32(0x8, sym::Vfx2Handler_MobyDick); // handler Vfx2Handler_MobyDick
            put32(0xc, sym::Vfx2Renderer_MobyDick); // renderer Vfx2Renderer_MobyDick
            put8(0x345, flags >> 8);
            put8(0x346, flags >> 16);
            put32(0x40, 31);
            break;
        case 28: // size 0x350
            alloc(0x350);
            put8(0x348, flags);
            put32(0x8, sym::Vfx2Handler_ScifiRocket); // handler Vfx2Handler_ScifiRocket
            put32(0xc, sym::Vfx2Renderer_ScifiRocketOld); // renderer Vfx2Renderer_ScifiRocketOld
            put8(0x349, flags >> 8);
            put8(0x34a, flags >> 16);
            put8(0x34b, 255);
            put32(0x40, 15);
            break;

        }
    }
    const uint32_t effect = at<uint32_t>(handle);
    at<uint32_t>(effect + 0x10) = tob;
    at<uint32_t>(effect + 4) = list.vfxTSFXFooter.address;
    list.vfxTSFXFooter = ptr32<_sVfx_TSFX>(effect);
    at<uint32_t>(effect + 0x30) = 0;
    at<uint32_t>(effect) = 0;
    return ptr32<_sVfx_TSFX>(effect);
}

// The oil-covered cars' and barges' dust: puffs of smoke every frame at the scenery's position, rising.
void Vfx2Handler_Steaming(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t tick = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
    at<uint16_t>(m + 0x184) = tick;
    if ((tick & 3) == 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = tailSlot(m, 0x14);
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 10);
        at<uint16_t>(p + 0x10) = 0x3f;
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    sceneryMovers(m);
    sortAtScenery(m, 0x60);
}

// Draws the steam as sprites around the scenery's position: white to grey as it ages, bigger as it rises.
void Vfx2Renderer_Steaming(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    for (int32_t index = ringHead(m); !ringAtTail(m, index); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t life = at<uint16_t>(p + 0x10);
        uint32_t sp = vfx::sprite();
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 1));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 1));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 1));
        sp = vfx::sprite();
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(sp + vfx::spr::kRgb) = life * 0x10001 + life * 0x100;
        at<float>(sp + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(0x15e - life * 4));
        VfxPS2SpriteRotated3D(0.0f);
    }
}

// The pipe's steam: it puffs on and off (a countdown at +0x186 flips the on/off count at +0x184 after a random 4 to 35
// frames); while on, a puff starts each frame, moving off with the pipe's own direction (the third row of the
// instance's matrix).
void Vfx2Handler_PipeSteam(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t countdown = static_cast<uint16_t>(at<uint16_t>(m + 0x186) - 1);
    at<uint16_t>(m + 0x186) = countdown;
    if (countdown == 0)
    {
        at<uint16_t>(m + 0x184) = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
        at<uint16_t>(m + 0x186) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 4);
    }
    if ((at<uint16_t>(m + 0x184) & 1) != 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t matrix = at<uint32_t>(m + 0x10) + 0x18;
        const uint32_t p = tailSlot(m, 0x14);
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) + (at<int16_t>(matrix + 2) >> 7) - 8);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0xf) + (at<int16_t>(matrix + 8) >> 7));
        at<uint16_t>(p + 0x10) = 0x28;
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xf) + (at<int16_t>(matrix + 0xe) >> 7) - 8);
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    sceneryMovers(m);
    sortAtScenery(m, 0x70);
}

// The monkey statue's smoke: a puff above the scenery's position every frame the ring has room.
void Vfx2Handler_MonkeySmoke(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    if (at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = tailSlot(m, 0x14);
        at<uint16_t>(p + 8) = 0;
        at<uint16_t>(p + 0xa) = 0;
        at<uint16_t>(p + 0xc) = 0;
        at<uint16_t>(p) = static_cast<uint16_t>(where.x);
        at<uint16_t>(p + 2) = static_cast<uint16_t>(where.y + 0x5a);
        at<uint16_t>(p + 4) = static_cast<uint16_t>(where.z);
        at<uint16_t>(p + 0x10) = 0xf;
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    sceneryMovers(m);
    sortAtScenery(m, 0x70, 0x10);
}

// The volcano: every 8th frame a lump starts at the scenery's position; once the scenery's own level (+0x18b) is past
// 0x50 it is thrown with a random velocity, otherwise it keeps whatever the slot held.
void Vfx2Handler_BigVolcano(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t tick = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
    at<uint16_t>(m + 0x184) = tick;
    if ((tick & 7) == 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = tailSlot(m, 0x14);
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        if (at<uint8_t>(m + 0x18b) > 0x50)
        {
            at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 0xf);
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
        }
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 0x60);
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    sortAtScenery(m, 0x60);
    sceneryMovers(m);
}

// The jungle fire: a flame particle (0x18 bytes) starts every frame the ring has room, with a random upward velocity,
// a spin, and the scenery's own colour byte (+0x34b).
void Vfx2Handler_JungleFire(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    at<uint16_t>(m + 0x344) = static_cast<uint16_t>(at<uint16_t>(m + 0x344) + 1);
    if (at<uint8_t>(m + kRing + 1) < 0x1f)
    {
        const uint32_t p = tailSlot(m, 0x18);
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 0x22);
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
        at<uint16_t>(p + 0x14) = static_cast<uint16_t>((vfx::random15() & 7) + 0x18);
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x3f);
        at<uint8_t>(p + 0x17) = 0xff;
        at<uint8_t>(p + 0x16) = at<uint8_t>(m + 0x34b);
        ringPush(m);
    }
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    sortAtScenery(m, 0x60);
}

// The sci-fi rocket's exhaust: the particles move first, then (if the ring has room) a new one starts with a wide random
// spread and a strong downward velocity, in the scenery's colour (+0x34b) with alpha 0xfc.
void Vfx2Handler_ScifiRocket(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    if (at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = tailSlot(m, 0x18);
        at<uint16_t>(p) = static_cast<uint16_t>((vfx::random15() & 0x3ff) - 0x200);
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = static_cast<uint16_t>((vfx::random15() & 0x3ff) - 0x200);
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xff) - 0x80);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(-500 - (vfx::random15() & 0x1ff));
        at<uint16_t>(p + 0x14) = 0xf;
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xff) - 0x80);
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x3f);
        at<uint8_t>(p + 0x17) = 0xfc;
        at<uint8_t>(p + 0x16) = at<uint8_t>(m + 0x34b);
        ringPush(m);
    }
    sortAtScenery(m, 0x60);
}

// Smoke from a chimney or a stack: every 8th frame a puff; once the scenery's level (+0x18b) is above 0x50 the puff is
// thrown harder and higher.
void Vfx2Handler_Smoking(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t tick = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
    at<uint16_t>(m + 0x184) = tick;
    if ((tick & 7) == 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = tailSlot(m, 0x14);
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        if (at<uint8_t>(m + 0x18b) < 0x51)
        {
            at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0xf) + 0x18);
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 7) + 0x28);
        }
        else
        {
            at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 0x20);
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xf) - 8);
        }
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 0x60);
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    sceneryMovers(m);
    sortAtScenery(m, 0x70);
}

// The feathers: sprites around the scenery's position, grey-blue, spinning by their own x position; the ones to the left
// of the middle are mirrored.
void Vfx2Renderer_Feathers(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x60);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    for (int32_t index = ringHead(m); !ringAtTail(m, index); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        uint32_t sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x15);
        at<uint8_t>(sp + vfx::spr::kCode) = 2;
        sp = vfx::sprite();
        const int32_t angle = at<int16_t>(p) >> 1;
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 2));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2));
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x404050;
        at<float>(sp + vfx::spr::kScale) = 32.0f;
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(angle);
        at<uint8_t>(sp + vfx::spr::kFlipX) = angle < 0xa01 ? 1 : 0;
        VfxPS2SpriteRotated3D(0.0f);
    }
}

// The bees: a cloud of small dark sprites that jitter randomly around each particle every frame they are drawn.
void Vfx2Renderer_Bees(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x70);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 4;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(1);
    sp = vfx::sprite();
    at<float>(sp + vfx::spr::kScale) = 16.0f;
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x303030;
    for (int32_t index = ringHead(m); !ringAtTail(m, index); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        at<float>(vfx::sprite() + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2) + (vfx::random15() & 0x1f) - 0xf);
        at<float>(vfx::sprite() + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 2) + (vfx::random15() & 0x1f) - 0xf);
        at<float>(vfx::sprite() + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2) + (vfx::random15() & 0x1f) - 0xf);
        at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = static_cast<uint8_t>(vfx::random15() & 1);
        VfxPS2SpriteRotated3D(-10.0f);
    }
}

// The flies: as the bees, smaller and lighter, with a jitter of 8 units.
void Vfx2Renderer_Flies(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x70);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 4;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(1);
    sp = vfx::sprite();
    at<float>(sp + vfx::spr::kScale) = 9.0f;
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x505050;
    for (int32_t index = ringHead(m); !ringAtTail(m, index); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        at<float>(vfx::sprite() + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 4) + (vfx::random15() & 7) - 3);
        at<float>(vfx::sprite() + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 4) + (vfx::random15() & 7) - 3);
        at<float>(vfx::sprite() + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 4) + (vfx::random15() & 7) - 4);
        at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = static_cast<uint8_t>(vfx::random15() & 1);
        VfxPS2SpriteRotated3D(-10.0f);
    }
}

// The gulls: only drawn while the scenery instance is on (bit 0 of its flags); a large sprite per particle, banked by
// its position, mirrored on one side.
void Vfx2Renderer_Gulls(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x60);
    const uint32_t m = mem.address;
    if ((at<uint16_t>(at<uint32_t>(m + 0x10) + 0x16) & 1) == 0)
        return;
    const SceneryPos where = sceneryPosition(m);
    for (int32_t index = ringHead(m); !ringAtTail(m, index); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        uint32_t sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x15);
        at<uint8_t>(sp + vfx::spr::kCode) = 2;
        sp = vfx::sprite();
        const int32_t angle = (-static_cast<int32_t>(at<int16_t>(p)) >> 2) + 0xa00;
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2));
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 2));
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x707070;
        at<float>(sp + vfx::spr::kScale) = 64.0f;
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(angle);
        at<uint8_t>(sp + vfx::spr::kFlipX) = angle < 0xa01 ? 1 : 0;
        VfxPS2SpriteRotated3D(0.0f);
    }
}

// The tumbleweed: one sprite, at its own position (+0x40, relative to the scenery's), sized by the scenery's radius.
// The scenery's position is cut to shorts and its height lowered by a quarter of the radius, in a local.
void Vfx2Renderer_TumbleWeed(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x20);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const int16_t x = static_cast<int16_t>(at<int32_t>(tob + 4) >> 8);
    const int16_t y = static_cast<int16_t>((at<int32_t>(tob + 8) >> 8) - (at<uint16_t>(tob + 0x2c) >> 2));
    const int16_t z = static_cast<int16_t>(at<int32_t>(tob + 0xc) >> 8);
    const uint32_t colour = at<uint8_t>(m + 0x54) + (static_cast<uint32_t>(at<uint8_t>(m + 0x55)) << 8) + (static_cast<uint32_t>(at<uint8_t>(m + 0x56)) << 16);
    uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x15);
    sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<float>(sp + vfx::spr::kPos) = ee_itof(x + (at<int16_t>(m + 0x40) >> 3));
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(y + (at<int16_t>(m + 0x42) >> 3));
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(z + (at<int16_t>(m + 0x44) >> 3));
    at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint16_t>(at<uint32_t>(m + 0x10) + 0x2c) >> 1);
    at<uint32_t>(sp + vfx::spr::kRgb) = colour;
    at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(m + 0x50);
    VfxPS2SpriteRotated3D(0.0f);
}

// The cartoon cloud under the carousel car: a flat shadow sprite, then a spinning star above it, both at the car's x and z.
void Vfx2CloudCarouselCar(ptr32<_sObject> pObj)
{
    GuestFrame frame(0x40);
    uint32_t sp = vfx::sprite();
    const uint32_t rendObj = pObj->pRendObj.address;
    at<uint8_t>(sp + vfx::spr::kCode) = 4;
    at<uint32_t>(sp + vfx::spr::kTexture) = at<uint32_t>(pObj.address + 0x34);
    sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    at<float>(sp + vfx::spr::kPos + 4) = 312.0f;
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int32_t>(rendObj + 4) >> 8) - ee_float(20.0f);
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int32_t>(rendObj + 0xc) >> 8);
    at<float>(sp + vfx::spr::kScale) = 50.0f;
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x707070;
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    VfxPS2SpriteRotated3D(-20.0f);
    const ee_float angleScale = at<float>(sym::lit4_3278bc);
    const int32_t time = G(mainVarG).time;
    sp = vfx::sprite();
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x15);
    at<float>(sp + vfx::spr::kScale) = 30.0f;
    ee_float a = ee_itof((time >> 1) & 0xfff);
    // The game's own sinf is run (not ee_sinf) so that it leaves its frame on the stack as the original does: the sprite
    // drawn next reads a word of stack nobody wrote.
    const ee_float spin = callOriginalFloat(sym::sinf, (a + a) * angleScale) * ee_float(4096.0f);
    a = ee_itof((((time >> 3) & 0xfff) * ee_ftoi(spin)) & 0xfff);
    const ee_float tilt = callOriginalFloat(sym::sinf, (a + a) * angleScale) * ee_float(4096.0f);
    sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x606060;
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(ee_ftoi(tilt) >> 5);
    VfxPS2SpriteRotated3D(-30.0f);
}

// The target circle under a car: a sprite 60 units wide at the car's position, 10 below it, spinning with the clock.
void Vfx2TargetCar(ptr32<_sObject> pObj)
{
    uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    const uint32_t rendObj = pObj->pRendObj.address;
    sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    const uint32_t texture = vfx::vfxTexture(0x16);
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int32_t>(rendObj + 4) >> 8);
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof((at<int32_t>(rendObj + 8) >> 8) - 10);
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int32_t>(rendObj + 0xc) >> 8);
    at<float>(sp + vfx::spr::kScale) = 60.0f;
    sp = vfx::sprite();
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x78;
    at<uint32_t>(sp + vfx::spr::kTexture) = texture;
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(G(mainVarG).time << 1);
    VfxPS2SpriteRotated3D(-80.0f);
}

// The skull's fart: puffs from the car (a ring of 0x18 bytes at +0x44, the car at +0x1c4). While the effect has more
// than 0x1f frames left a puff starts each frame the ring has room, behind the car and thrown back along its velocity
// (the car's rotation, rows of the matrix it reports), random in direction half the time.
void Vfx2Handler_SkullFart(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x1c4));
    const uint16_t life = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
    at<uint16_t>(m + kLifeSpan) = life;
    if (life >= 0x20 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        guest_call(sym::VfxCarVelocityPoi, car); // VfxCarVelocityPoi
        const uint32_t matrix = guest_call<ptr32<MATRIX>>(sym::VfxCarMatrixPoi, car).address; // VfxCarMatrixPoi
        const uint32_t p = tailSlot(m, 0x18);
        at<uint16_t>(p) = static_cast<uint16_t>((vfx::random15() & 0x3f) - 0x20);
        at<uint16_t>(p + 2) = static_cast<uint16_t>((vfx::random15() & 0x3ff) - 0x200);
        at<uint16_t>(p + 4) = static_cast<uint16_t>((vfx::random15() & 0x3f) - 0x20);
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0x3f) - ((at<int16_t>(matrix) >> 6) + 0x20));
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) - ((at<int16_t>(matrix + 6) >> 6) + 0x10));
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0x3f) - ((at<int16_t>(matrix + 0xc) >> 6) + 0x20));
        if ((vfx::random15() & 1) != 0)
        {
            at<uint16_t>(p + 8) = static_cast<uint16_t>(-at<uint16_t>(p + 8));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(-at<uint16_t>(p + 0xa));
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>(-at<uint16_t>(p + 0xc));
        }
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>((vfx::random15() & 0x80) - 0x3f);
        at<uint16_t>(p + 0x14) = static_cast<uint16_t>((vfx::random15() & 0xf) + 0xf);
        at<uint8_t>(p + 0x17) = 0xff;
        at<uint8_t>(p + 0x16) = 0x20;
        ringPush(m);
    }
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    sortPosFromCar(m, car, 0x20); // VfxCarSortPos, a tail call
}

// Draws the skull's fart as sprites around the car's position: green, drawn only when the car is near the camera.
void Vfx2Renderer_SkullFart(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x70);
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x1c4));
    if (VfxHasCarBeenRendered(car) >= 0x157c)
        return;
    const uint32_t where = guest_call<ptr32<SVECTOR>>(sym::VfxCarPositionPoi, car).address; // VfxCarPositionPoi
    uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0xe);
    for (int32_t index = ringHead(m); !ringAtTail(m, index); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x18u;
        sp = vfx::sprite();
        const uint32_t life = at<uint16_t>(p + 0x14);
        at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(where) + (at<int16_t>(p) >> 4));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(where + 2) + (at<int16_t>(p + 2) >> 4));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(where + 4) + (at<int16_t>(p + 4) >> 4));
        at<uint8_t>(sp + vfx::spr::kFlipX) = at<uint8_t>(p + 0x16) & 1;
        sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kRgb) = (life << 2) + (life << 18);
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16) + 5);
        VfxPS2SpriteRotated3D(0.0f);
    }
}

// The right-the-car effect: on its first frame (life 0x10) it sets the car's "righting" flag (+0x18b) and throws a whole
// ring of particles; when its life runs out it clears the flag. The effect is sorted at the car and its particles move.
void Vfx2Handler_CarRight(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    uint16_t life = at<uint16_t>(m + kLifeSpan);
    const ptr32<_sObject> car(at<uint32_t>(m + 0x1c4));
    if (life == 0x10)
    {
        at<uint8_t>(car.address + 0x18b) = 1;
        const uint32_t count = at<uint8_t>(m + kRing);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t p = m + kParticles + i * 0x18;
            guest_call(sym::VfxCarVelocityPoi, car); // VfxCarVelocityPoi
            at<uint16_t>(p) = static_cast<uint16_t>((vfx::random15() & 0x7ff) - 0x400);
            at<uint16_t>(p + 2) = static_cast<uint16_t>((vfx::random15() & 0x3ff) - 0x364);
            const int16_t z = static_cast<int16_t>((vfx::random15() & 0x7ff) - 0x400);
            at<uint16_t>(p + 0xa) = 0xa0;
            at<uint16_t>(p + 8) = static_cast<uint16_t>(at<int16_t>(p) >> 3);
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>(z >> 3);
            at<uint16_t>(p + 4) = static_cast<uint16_t>(z);
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
            at<uint16_t>(p + 0x12) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x3f);
            at<uint16_t>(p + 0x14) = static_cast<uint16_t>((vfx::random15() & 7) + 8);
            at<uint8_t>(p + 0x17) = 0xff;
            at<uint8_t>(p + 0x16) = static_cast<uint8_t>(vfx::random15() % 0x3f + 0x20);
        }
        at<uint8_t>(m + kRing + 1) = at<uint8_t>(m + kRing);
        at<uint8_t>(m + kRing + 3) = at<uint8_t>(m + kRing);
        life = at<uint16_t>(m + kLifeSpan);
    }
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(life - 1);
    if (static_cast<uint16_t>(life - 1) == 0)
        at<uint8_t>(car.address + 0x18b) = 0;
    sortPosFromCar(m, car, 0xA0 + 0x20); // VfxCarSortPos
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles)); // (a tail call)
}

// The oil on a car: four sprites around the car's position, a big dark splat and three spinning layers on it, but only when
// the car is near the camera.
void VfxRenderer_OilCovered(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x40));
    {
        GuestFrame frame(0x60);
        if (VfxHasCarBeenRendered(car) >= 0x157c)
            return;
        uint32_t sp = vfx::sprite();
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        sp = vfx::sprite();
        at<uint8_t>(sp + vfx::spr::kCode) = 2;
        sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x8c8c8c;
        const ptr32<ee_float> position = VfxVUCarPositionPoi(car);
        sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x6e6e6e;
        at<float>(sp + vfx::spr::kPos) = position[0];
        at<float>(sp + vfx::spr::kPos + 4) = position[1];
        at<float>(sp + vfx::spr::kPos + 8) = position[2];
        const int16_t size = at<int16_t>(m + 0x46);
        at<uint8_t>(sp + vfx::spr::kCode) = 4;
        at<uint16_t>(sp + vfx::spr::kAngle) = 0xfe00;
        at<float>(sp + vfx::spr::kScale) = ee_itof(size + 10);
        at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(6);
        VfxPS2SpriteRotated3D(-60.0f);
        sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(7);
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(m + 0x44);
        at<uint8_t>(sp + vfx::spr::kCode) = 2;
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x46;
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<int16_t>(m + 0x46) + 0xf);
        VfxPS2SpriteRotated3D(-70.0f);
        sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x4600;
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(at<int16_t>(m + 0x44) >> 1);
        VfxPS2SpriteRotated3D(-70.0f);
        sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x460000;
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(-at<uint16_t>(m + 0x44));
    }
    VfxPS2SpriteRotated3D(-70.0f); // a tail call: the frame is gone
}

// The water spray from a boat or car: ribbons from the spray's particle positions (relative to the car's position and the
// height of its wheel or hull) to where its velocity takes it, brighter as the particle's life lengthens.
void VfxRenderer_Spray(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xA0);
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x44));
    if (at<uint8_t>(m + kRing + 1) == 0 || VfxHasCarBeenRendered(car) >= 0x157c)
        return;
    const uint64_t position = VfxCarPositionVal(car);
    const ptr32<SVECTOR> from = frame.local<SVECTOR>(0), to = frame.local<SVECTOR>(8), where = frame.local<SVECTOR>(0x10);
    where->vx = static_cast<int16>(position);
    where->vy = static_cast<int16>(position >> 16);
    where->vz = static_cast<int16>(position >> 32);
    guest_call(sym::VfxCarMatrixPoi, car); // VfxCarMatrixPoi
    const uint32_t physics = car->pPhysics.address;
    if (guest_call<int32>(sym::VfxAreWeInCar, car) != 0) // VfxAreWeInCar: the wheel's height less the wheel's radius
    {
        const uint32_t data = at<uint32_t>(physics + 0x1e4);
        const ee_float wheel = ee_float(at<float>(physics + 0xb0)) * ee_float(0.00390625f);
        const ee_float radius = ee_itof(ee_ftoi(at<float>(data + 0x2c)) >> 8);
        where->vy = static_cast<int16>(ee_ftoi(wheel - radius));
    }
    else // a boat: the third sphere's bottom, adjusted for the liquid under it
    {
        const uint32_t sphere = at<uint32_t>(physics + 0x1e0);
        const ee_float bottom = ee_float(at<float>(sphere + 0x324)) - ee_float(at<float>(sphere + 0x378));
        where->vy = static_cast<int16>(ee_ftoi(bottom + ee_float(at<float>(sphere + 0x398))) >> 8);
    }
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + 0x48 + index * 0x1c;
        const int32_t x = where->vx + at<int16_t>(p + 8), y = where->vy + at<int16_t>(p + 0xa), z = where->vz + at<int16_t>(p + 0xc);
        from->vx = static_cast<int16>(x);
        from->vy = static_cast<int16>(y);
        from->vz = static_cast<int16>(z);
        to->vx = static_cast<int16>(x + (at<int16_t>(p) >> 2));
        to->vy = static_cast<int16>(y + (at<int16_t>(p + 2) >> 2));
        to->vz = static_cast<int16>(z + (at<int16_t>(p + 4) >> 2));
        const uint32_t brightness = (static_cast<uint32_t>(at<uint16_t>(p + 0x18)) * 0x20 + 0x15) * 0x10101;
        VfxPutGT4rgba0(0);
        VfxPutGT4rgba1(0);
        VfxPutGT4rgba2(brightness);
        VfxPutGT4rgba3(brightness);
        VfxPutGT4FacingPoly(from, to, ptr32<_sGfxTexture>(vfx::vfxTexture(10)), 2, 20.0f, -10.0f);
    }
}


} // namespace rc

namespace rc::fuzz { // visfx_fuzz.cpp
void vfxTSFXAdd(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTSFXLvlInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTSFXLvlTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTSFXUpdate(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSteamingHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSteamingRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxPipeSteam(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxMonkeySmoke(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBigVolcano(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxJungleFire(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxScifiRocket(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSmoking(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFeathers(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBees(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFlies(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGulls(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTumbleWeedRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCloudCarousel(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTargetCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSkullFartHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSkullFartRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarRight(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxOilCoveredRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSprayRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP_HEAP(Vfx2TSFXLvlInit, rc::fuzz::vfxTSFXLvlInit)
RC_DECOMP_FN_SETUP_HEAP(Vfx2TSFXLvlTerm, rc::fuzz::vfxTSFXLvlTerm)
RC_DECOMP_FN_SETUP_HEAP(Vfx2AddTSFX, rc::fuzz::vfxTSFXAdd)
RC_DECOMP_FN_SETUP_HEAP(Vfx2UpdateTSFX, rc::fuzz::vfxTSFXUpdate)
RC_DECOMP_FN_SETUP(Vfx2Handler_Steaming, rc::fuzz::vfxSteamingHandler)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Steaming, rc::fuzz::vfxSteamingRenderer)
RC_DECOMP_FN_SETUP(Vfx2Handler_PipeSteam, rc::fuzz::vfxPipeSteam)
RC_DECOMP_FN_SETUP(Vfx2Handler_MonkeySmoke, rc::fuzz::vfxMonkeySmoke)
RC_DECOMP_FN_SETUP(Vfx2Handler_BigVolcano, rc::fuzz::vfxBigVolcano)
RC_DECOMP_FN_SETUP(Vfx2Handler_JungleFire, rc::fuzz::vfxJungleFire)
RC_DECOMP_FN_SETUP(Vfx2Handler_ScifiRocket, rc::fuzz::vfxScifiRocket)
RC_DECOMP_FN_SETUP(Vfx2Handler_Smoking, rc::fuzz::vfxSmoking)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Feathers, rc::fuzz::vfxFeathers)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Bees, rc::fuzz::vfxBees)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Flies, rc::fuzz::vfxFlies)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Gulls, rc::fuzz::vfxGulls)
RC_DECOMP_FN_SETUP(Vfx2Renderer_TumbleWeed, rc::fuzz::vfxTumbleWeedRenderer)
// Vfx2CloudCarouselCar (0x170998) is not registered: the quad's fourth lane after its second sprite is a stack word that
// sinf's callees left, and the random test cannot make the native one's frames leave the same ones (it matches only when
// the stack below was already as the original leaves it). It stays recompiled.
RC_DECOMP_FN_SETUP(Vfx2TargetCar, rc::fuzz::vfxTargetCar)
RC_DECOMP_FN_SETUP(Vfx2Handler_SkullFart, rc::fuzz::vfxSkullFartHandler)
RC_DECOMP_FN_SETUP(Vfx2Renderer_SkullFart, rc::fuzz::vfxSkullFartRenderer)
RC_DECOMP_FN_SETUP(Vfx2Handler_CarRight, rc::fuzz::vfxCarRight)
RC_DECOMP_FN_SETUP(VfxRenderer_OilCovered, rc::fuzz::vfxOilCoveredRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_Spray, rc::fuzz::vfxSprayRenderer)
