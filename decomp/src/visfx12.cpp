// Decompiled from VISFX.c, VISFX2.c and VISFX3.c, the last of them (the effect system's core): the effect constructor
// (VfxAddNew), the effects task (VfxUpdate), the two depth-sorted draw lists (VfxCallRenderers, Vfx2RenderTSFX), the
// scenery tag dispatcher (Vfx2CallBack), the podium celebration (Vfx2PodiumPlay), the sky (VfxSkyChangeStuff,
// VfxSkyBackdropMerge) and the rain (Vfx3Rain). See visfx5.cpp for the conventions.
//
// All of these keep the original's frame (GuestFrame): they pass locals by address (the heap handle, the sort list, the
// rain's vectors) or call functions that draw sprites, whose stale stack lanes come from where the frame sits. A tail
// call (`j`) in the original runs after its frame is popped, so it is made outside the GuestFrame's scope here.

#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

namespace rc {

int32 VfxIsWheelTouching(ptr32<_sObject> pObj, int32 iWheel);                                   // visfx.cpp
void VfxCallHandlers();                                                                          // visfx.cpp
void VfxSkyActualSlowChange(ptr32<uint8> pTo, ptr32<uint8> pFrom);                               // visfx.cpp
void VfxSkyBackdrop(ptr32<uint8> pSky);                                                          // visfx.cpp
void VfxPutUV0(ee_float u, ee_float v);                                                          // visfx.cpp
void VfxPutUV1(ee_float u, ee_float v);
void VfxPutUV2(ee_float u, ee_float v);
void VfxPutUV3(ee_float u, ee_float v);
void VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR> svNormal, ee_float zOffset);                    // visfx.cpp
void Vfx2FinishText(int16 pos, ptr32<char> t);                                                   // visfx2.cpp
void Vfx3AddRain(ptr32<_sTOBInst> pTOB);                                                         // visfx2.cpp
ptr32<_sVfx_TSFX> Vfx2AddTSFX(ptr32<_sTOBInst> pTOBInst, uint32 flags, uint32 iSize, uint32 iType); // visfx3.cpp
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId); // collision.cpp
bool_ InputHeld(uint8 key);                                                                      // input
uint32 FontLength(ptr32<char> pString);                                                          // font.cpp
void FontSet(ptr32<font> pFont);
void FontSetColour(uint8 red, uint8 green, uint8 blue);
ptr32<char> FontPrint(int16 x, int16 y, ptr32<char> pString);
void VfxSkyBackdropMerge(int16 sM);

namespace {

using vfx::at;

constexpr uint32_t kVisFx = sym::visFxVarG;       // visFxVarG (its task is its first member)
constexpr uint32_t kFxHeader = (sym::visFxVarG + 0x2710u), kFxFooter = (sym::visFxVarG + 0x2714u); // visFxVarG.vfxHeader / vfxFooter
constexpr uint32_t kTsfxHeader = (sym::visFxVarG + 0x2718u);  // visFxVarG.vfxTSFXHeader
constexpr uint32_t kSkyTerror = (sym::visFxVarG + 0x2728u), kCueForward = (sym::visFxVarG + 0x272cu), kResultTrigger = (sym::visFxVarG + 0x2730u);
constexpr uint32_t kCameraPos = (sym::visFxVarG + 0x160u);   // visFxVarG.ps2CameraTrans: the camera position the effects are sorted from
constexpr uint32_t kCameraDir = (sym::visFxVarG + 0x190u);   // the camera's view direction (sort depth = dot with it)
constexpr uint32_t kObjects = sym::objectVarG;     // objectVarG.list; the count at +0x170
constexpr uint32_t kPlayerStates = sym::u8PlayerStates; // u8PlayerStates: per player sNew (+0) and sOld (+2)
constexpr uint32_t kMainToDo = (sym::mainVarG + 0x4u), kMainTime = (sym::mainVarG + 0x1cu), kWhichSplitScreen = (sym::mainVarG + 0x378u);
constexpr uint32_t kFrontEnd = sym::frontEndVarG;    // frontEndVarG: pFELevelData first; active at +0x2d9
constexpr uint32_t kAi = sym::aiVarG;          // aiVarG: player1CameIn +0x64, player2CameIn +0x66, podium objects +0x68/+0x6a
constexpr uint32_t kSky = (sym::visVarG + 0x14u);         // visVarG.sky
constexpr uint32_t kLvl = sym::pLvlG;         // pLvlG
constexpr uint32_t kMenuSystem = sym::pMenuSystemG;  // pMenuSystemG
constexpr uint32_t kTextBuffer = sym::buff;  // buff
constexpr uint32_t kCrackle = sym::crackle_528;     // Vfx2PodiumPlay's static crackle_528
constexpr uint32_t kTestFX = sym::iTestFX, kTestRP = sym::iTestRP;

constexpr uint32_t kQsort = sym::qsort, kSortFunc = sym::VfxSortFunc, kTaskCachedInsert = sym::TaskCachedInsert, kInputHit = sym::InputHit;
constexpr uint32_t kSfxPlay = sym::SfxPlay, kHandoverToFE = sym::HandoverToFE, kHudSprite = sym::HudSprite, kSprintf = sym::sprintf;
constexpr uint32_t kMulMatrix = sym::sceVu0MulMatrix, kCopyMatrix = sym::sceVu0CopyMatrix;

inline int32_t sh16(uint32_t address) { return at<int16_t>(address); }
inline uint32_t vfxRes() { return G(visFxVarG).pVfxRes.address; }

// sceVu0InnerProduct: vmul.xyz, then x + y, then + z, each step with VU0's clamping.
ee_float innerProduct(uint32_t a, uint32_t b)
{
    const __m128 va = _mm_loadu_ps(&at<float>(a & ~0xFu));
    const __m128 vb = _mm_loadu_ps(&at<float>(b & ~0xFu));
    const __m128 product = PS2_VMUL(_mm_and_ps(va, _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0))), vb);
    const __m128 y = _mm_shuffle_ps(product, product, _MM_SHUFFLE(1, 1, 1, 1));
    const __m128 z = _mm_shuffle_ps(product, product, _MM_SHUFFLE(2, 2, 2, 2));
    return _mm_cvtss_f32(PS2_VADD(PS2_VADD(product, y), z));
}

// --- VfxAddNew ----------------------------------------------------------------------------------------------------------

// An effect's handler and renderer (guest addresses).
struct Calls
{
    uint32_t handler, renderer;
};
constexpr Calls kCarSmoke{sym::VfxHandler_CarSmoke, sym::VfxRenderer_CarSmoke}, kCarFire{sym::VfxHandler_CarFire, sym::VfxRenderer_CarFire}, kAerialRibbon{sym::VfxHandler_CarAerialRibbon, sym::VfxRenderer_CarAerialRibbon};
constexpr Calls kOilDump{sym::VfxHandler_OilDump, sym::VfxRenderer_OilDump}, kHeatRayAerial{sym::VfxHandler_CarHeatRayAerial, sym::VfxRenderer_CarHeatRayAerial}, kHeatRayBlast{sym::VfxHandler_CarHeatRayBlast, sym::VfxRenderer_CarHeatRayBlast};
constexpr Calls kHeatRayDamage{sym::VfxHandler_HeatRayDamage, sym::VfxRenderer_HeatRayDamage}, kSkidMark{sym::VfxHandler_CarSkidMark, sym::VfxRenderer_CarSkidMark}, kWaterBall{sym::VfxHandler_CarWaterBall, sym::VfxRenderer_CarWaterBall};
constexpr Calls kWheelDust{sym::VfxHandler_WheelDust, sym::VfxRenderer_WheelDust}, kBoatWake{sym::VfxHandler_BoatWake, sym::VfxRenderer_BoatWake}, kShadow{sym::VfxHandler_Shadow, sym::VfxRenderer_Shadow};
constexpr Calls kExplosion{sym::VfxHandler_Explosion, sym::VfxRenderer_Explosion}, kOilSlick{sym::VfxHandler_OilSlick, sym::VfxRenderer_OilSlick}, kWaterSploosh{sym::VfxHandler_WaterSploosh, sym::VfxRenderer_WaterSploosh};
constexpr Calls kAerial{sym::VfxHandler_Aerial, sym::VfxRenderer_Aerial}, kAerialLightning{sym::VfxHandler_AerialLightning, sym::VfxRenderer_AerialLightning}, kBlastLightning{sym::VfxHandler_BlastLightning, sym::VfxRenderer_BlastLightning};
constexpr Calls kBlastMoonrover{sym::VfxHandler_BlastMoonrover, sym::VfxRenderer_BlastMoonrover}, kDamageLightning{sym::VfxHandler_DamageLightning, sym::VfxRenderer_DamageLightning}, kGiveTerror{sym::VfxHandler_GiveTerror, sym::VfxRenderer_GiveTerror};
constexpr Calls kTakeTerror{sym::VfxHandler_TakeTerror, sym::VfxRenderer_TakeTerror}, kSpinPickup{sym::VfxHandler_SpinPickup, sym::VfxRenderer_SpinPickup}, kKaboom{sym::VfxHandler_Kaboom, sym::VfxRenderer_Kaboom};
constexpr Calls kWaterCovered{sym::VfxHandler_CarWaterCovered, sym::VfxRenderer_CarWaterCovered}, kQuake{sym::VfxHandler_Quake, sym::VfxRenderer_Quake}, kQuake2{sym::VfxHandler_Quake2, sym::VfxRenderer_Quake2};
constexpr Calls kSargeLights{sym::VfxHandler_SargeLights, sym::VfxRenderer_SargeLights}, kSpeedUp{sym::VfxHandler_SpeedUp, sym::VfxRenderer_SpeedUp}, kBoostSpeedUp{sym::VfxHandler_BoostSpeedUp, sym::VfxRenderer_SpeedUp};
constexpr Calls kChangeOver{sym::VfxHandler_CarChangeOver, sym::VfxRenderer_CarChangeOver}, kSparks{sym::VfxHandler_Sparks, sym::VfxRenderer_Sparks}, kWaterBallFlying{sym::VfxHandler_WaterBallFlying, sym::VfxRenderer_WaterBallFlying};
constexpr Calls kRocketSmoke{sym::VfxHandler_RocketSmoke, sym::VfxRenderer_RocketSmoke}, kReadySteadyGo{sym::VfxHandler_ReadySteadyGo, sym::VfxRenderer_ReadySteadyGo}, kInvincible{sym::VfxHandler_Invincible, sym::VfxRenderer_Invincible};
constexpr Calls kBomb{sym::VfxHandler_Bomb, sym::VfxRenderer_Bomb}, kFirework{sym::VfxHandler_Firework, sym::VfxRenderer_Firework}, kCarRespawn{sym::VfxHandler_CarRespawn, sym::VfxRenderer_CarRespawn};
constexpr Calls kSkullFart{sym::Vfx2Handler_SkullFart, sym::Vfx2Renderer_SkullFart}, kSkullWave{sym::VfxHandler_Quake, sym::Vfx2Renderer_SkullWave}, kCarSplash{sym::VfxHandler_CarSplash, sym::VfxRenderer_CarSplash};
constexpr Calls kUfoBurner{sym::Vfx2Handler_UFOBurner, sym::Vfx2Renderer_UFOBurner}, kButterflies{sym::Vfx2Handler_Butterflies, sym::Vfx2Renderer_Butterflies}, kMilk{sym::VfxHandler_Milk, sym::VfxRenderer_Milk};
constexpr Calls kCarRight{sym::Vfx2Handler_CarRight, sym::Vfx2Renderer_CarRight}, kFinishPosition{sym::Vfx2Handler_FinishPosition, sym::Vfx2Renderer_FinishPosition}, kTTclock{sym::Vfx2Handler_TTclock, sym::Vfx2Renderer_TTclock};
constexpr Calls kBargeOil{sym::VfxHandler_BargeOil, sym::VfxRenderer_BargeOil}, kOilCovered{sym::VfxHandler_OilCovered, sym::VfxRenderer_OilCovered}, kArmCannon{sym::VfxHandler_ArmCannon, sym::VfxRenderer_ArmCannon};
constexpr Calls kRouteFinder{sym::Vfx2Handler_RouteFinder, sym::Vfx2Renderer_RouteFinder}, kSpray{sym::VfxHandler_Spray, sym::VfxRenderer_Spray}, kCarRocket{sym::VfxHandler_CarRocket, sym::VfxRenderer_CarRocket};
constexpr Calls kRocketCar{sym::Vfx3Handler_RocketCar, sym::Vfx3Renderer_RocketCar};

// The fields every effect sets: handler (+8), renderer (+0xc) and life (+0x10, 0xffff for one that lives until it is
// deleted).
inline void start(uint32_t m, Calls calls, uint16_t life)
{
    at<uint32_t>(m + 8) = calls.handler;
    at<uint32_t>(m + 0xc) = calls.renderer;
    at<uint16_t>(m + 0x10) = life;
}
// Most effects keep a particle ring at +0x40; its first word is the mask (the ring's size less one), the rest zero.
inline void ring(uint32_t m, uint32_t mask) { at<uint32_t>(m + 0x40) = mask; }
// Three shorts: a position (the effect's own place, or the car's position in whole units).
inline void put3(uint32_t address, int32_t x, int32_t y, int32_t z)
{
    at<int16_t>(address) = static_cast<int16_t>(x);
    at<int16_t>(address + 2) = static_cast<int16_t>(y);
    at<int16_t>(address + 4) = static_cast<int16_t>(z);
}
inline int32_t random15() { return rand() >> 15; }

// Makes the effect of the given type and puts it on the end of the effect list. A type above 0x400 is a placed effect:
// the first three arguments are then its position (x, y, z) instead of a car and two parameters. Effects that follow a
// car start at the car's position in whole units (which also goes to the shared svVelTemp).
ptr32<_sVfx_Object> addNew(ptr32<_sObject> pPlayerObj, int32 iY, int32 iZ, int32 iType)
{
    GuestFrame frame(0xb0);
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    at<uint32_t>(handle.address) = 0;
    const uint32_t obj = pPlayerObj.address;
    int32_t x, y, z;
    if (iType < 0x401)
    {
        const uint32_t integer = at<uint32_t>(at<uint32_t>(obj + 0x20) + 0x1e8);
        SVECTOR &pos = G(svVelTemp);
        pos.vz = static_cast<int16>(at<int32_t>(integer + 0xc) >> 8);
        pos.vy = static_cast<int16>(at<int32_t>(integer + 8) >> 8);
        pos.vx = static_cast<int16>(at<int32_t>(integer + 4) >> 8);
        x = pos.vx;
        y = pos.vy;
        z = pos.vz;
    }
    else
    {
        iType -= 0x400;
        x = static_cast<int32_t>(obj);
        y = iY;
        z = iZ;
    }

    ee_float radius = 0.0f; // the distance beyond the camera's near side at which it is still drawn
    const auto alloc = [&](uint32_t size) { return MemBlockAllocLo(handle, size) != 0; };
    const auto mem = [&]() { return at<uint32_t>(handle.address); };
    // The shapes several effects share.
    const auto positioned = [&](uint32_t m, Calls calls, uint16_t life, uint32_t mask, uint32_t posOffset) {
        start(m, calls, life);
        ring(m, mask);
        put3(m + posOffset, x, y, z);
    };
    const auto waterBall = [&](uint32_t m, Calls calls, uint16_t life) {
        start(m, calls, life);
        ring(m, 0x1f);
        at<int32_t>(m + 0x654) = random15();
        at<uint32_t>(m + 0x650) = obj;
    };
    const auto lightning = [&](uint32_t m, Calls calls, uint16_t life) {
        start(m, calls, life);
        at<uint32_t>(m + 0x78) = obj;
        for (uint32_t offset = 0x40; offset < 0x78; offset += 8) // seven bolt points of three shorts
            put3(m + offset, 0, 0, 0);
    };

    switch (static_cast<uint32_t>(iType))
    {
    case 0x00: // car smoke
    case 0x25: // skull fart
        if (alloc(0x1d0))
        {
            const uint32_t m = mem();
            start(m, iType == 0 ? kCarSmoke : kSkullFart, iType == 0 ? 100 : 0xffff);
            ring(m, 0xf);
            radius = 500.0f;
            at<uint32_t>(m + 0x1c4) = obj;
        }
        break;
    case 0x01: // car fire
        if (alloc(0x350))
        {
            const uint32_t m = mem();
            start(m, kCarFire, 0x78);
            ring(m, 0x1f);
            at<uint32_t>(m + 0x344) = obj;
            radius = 500.0f;
            at<int32_t>(m + 0x348) = iY;
        }
        break;
    case 0x02: // the aerial's ribbon
        if (alloc(0x130))
        {
            const uint32_t m = mem();
            start(m, kAerialRibbon, 0xffff);
            ring(m, 7);
            at<uint32_t>(m + 0x124) = obj;
            put3(m + 0x128, 0, 0, 0);
        }
        break;
    case 0x03: // oil dump: the car loses its weapon
        if (alloc(0x1e0))
        {
            const uint32_t m = mem();
            start(m, kOilDump, 200);
            ring(m, 0xf);
            at<uint16_t>(m + 0x1ca) = 0x8044;
            at<uint32_t>(m + 0x1c4) = obj;
            at<uint32_t>(m + 0x1d0) = 0;
            at<uint32_t>(m + 0x1d4) = 0;
            at<uint8_t>(obj + 0x194) = 0;
        }
        break;
    case 0x04: // heat ray aerial
    case 0x06: // heat ray damage
        if (alloc(0x350))
        {
            const uint32_t m = mem();
            start(m, iType == 4 ? kHeatRayAerial : kHeatRayDamage, iType == 4 ? 0xffff : 0x50);
            ring(m, iType == 4 ? 0xf : 0x1f);
            at<uint32_t>(m + 0x344) = obj;
        }
        break;
    case 0x05: // heat ray blast
    case 0x11: // blast lightning
        if (alloc(0x50))
        {
            const uint32_t m = mem();
            start(m, iType == 5 ? kHeatRayBlast : kBlastLightning, 0xffff);
            at<uint32_t>(m + 0x40) = obj;
            at<int32_t>(m + 0x44) = iY;
            at<uint16_t>(m + 0x48) = 0;
        }
        break;
    case 0x07: // skid mark (iY the wheel)
        if (alloc(0x190))
        {
            const uint32_t m = mem();
            at<uint8_t>(m + 0x188) = static_cast<uint8_t>(iY);
            start(m, kSkidMark, 1);
            ring(m, 0xf);
            at<uint32_t>(m + 0x184) = obj;
            at<uint8_t>(m + 0x189) = 0;
            radius = 1500.0f;
            at<uint8_t>(m + 0x18a) = 0;
            at<uint8_t>(m + 0x18b) = 0;
        }
        break;
    case 0x08: // wheel dust (iY the wheel, iZ the material)
        if (alloc(0x1d0))
        {
            const uint32_t m = mem();
            start(m, kWheelDust, 0x20);
            ring(m, 0xf);
            at<uint32_t>(m + 0x1c4) = obj;
            at<int16_t>(m + 0x1c8) = static_cast<int16_t>(iY);
            radius = 1500.0f;
            at<uint16_t>(m + 0x1ca) = static_cast<uint16_t>(iZ & 0xff);
        }
        break;
    case 0x09: // explosion
        if (alloc(0x1d0))
        {
            positioned(mem(), kExplosion, 200, 0xf, 0x1c4);
            radius = 500.0f;
        }
        break;
    case 0x0a: // oil slick
        if (alloc(0x650))
        {
            positioned(mem(), kOilSlick, 200, 0x3f, 0x644);
            radius = 500.0f;
        }
        break;
    case 0x0b: // the car's water ball
        if (alloc(0x660))
            waterBall(mem(), kWaterBall, 0xffff);
        break;
    case 0x0c: // water sploosh
    case 0x16: // kaboom
        if (alloc(iType == 0xc ? 0x560 : 0x550))
        {
            positioned(mem(), iType == 0xc ? kWaterSploosh : kKaboom, iType == 0xc ? 0x32 : 0x40, 0x3f, 0x544);
            radius = 500.0f;
        }
        break;
    case 0x0d: // boat wake
        if (alloc(0x290))
        {
            const uint32_t m = mem();
            at<uint16_t>(m + 0x10) = 1;
            at<uint8_t>(m + 0x209) = 0;
            radius = 2500.0f;
            at<uint32_t>(m + 8) = kBoatWake.handler;
            at<uint32_t>(m + 0xc) = kBoatWake.renderer;
            ring(m, 0xf);
            at<uint32_t>(m + 0x204) = obj;
            at<uint16_t>(m + 0x20a) = 0;
        }
        break;
    case 0x0e: // shadow
    case 0x22: // milk
        if (alloc(0x50))
        {
            const uint32_t m = mem();
            start(m, iType == 0xe ? kShadow : kMilk, iType == 0xe ? 1 : 0x32);
            at<uint32_t>(m + 0x40) = obj;
        }
        break;
    case 0x0f: // aerial
        if (alloc(0xe0))
        {
            const uint32_t m = mem();
            start(m, kAerial, 1);
            at<uint32_t>(m + 0xb0) = obj;
            for (uint32_t offset : {0xb4u, 0xc0u, 0xc4u, 0xc8u, 0xd0u, 0xd4u, 0xd8u})
                at<uint32_t>(m + offset) = 0;
        }
        break;
    case 0x10: // aerial lightning
    case 0x12: // damage lightning
        if (alloc(0x80))
            lightning(mem(), iType == 0x10 ? kAerialLightning : kDamageLightning, iType == 0x10 ? 0xffff : 0x40);
        break;
    case 0x13: // give terror
        if (alloc(0x2e0))
        {
            const uint32_t m = mem();
            start(m, kGiveTerror, 0x60);
            ring(m, 0x1f);
            at<uint32_t>(m + 0x2cc) = obj;
            at<uint16_t>(m + 0x2d0) = 0x20;
        }
        break;
    case 0x14: // take terror: a random start and offset
        if (alloc(0x2e0))
        {
            const uint32_t m = mem();
            at<uint32_t>(m + 0x2cc) = obj;
            start(m, kTakeTerror, 0x60);
            ring(m, 0x1f);
            at<uint16_t>(m + 0x2d0) = static_cast<uint16_t>(random15() & 0xff);
            at<uint16_t>(m + 0x2d4) = static_cast<uint16_t>((random15() & 0x3f) - 0x20);
            at<uint16_t>(m + 0x2d6) = static_cast<uint16_t>((random15() & 0x3f) - 0x20);
        }
        break;
    case 0x15: // spin pickup: iY the pickup (its position, floats at +0x78), iZ a parameter
        if (alloc(0x100))
        {
            const uint32_t m = mem();
            const ee_float py = ee_float(at<float>(static_cast<uint32_t>(iY) + 0x7c)) + ee_float(48.0f);
            const ee_float px = at<float>(static_cast<uint32_t>(iY) + 0x78);
            const ee_float pz = at<float>(static_cast<uint32_t>(iY) + 0x80);
            start(m, kSpinPickup, 0x3f);
            put3(m + 0xe8, ee_ftoi(px), ee_ftoi(py), ee_ftoi(pz));
            ring(m, 7);
            at<uint32_t>(m + 0x44) = obj;
            at<int32_t>(m + 0xfc) = iZ;
        }
        break;
    case 0x17: // the car covered in water
        if (alloc(0x660))
            waterBall(mem(), kWaterCovered, 200);
        break;
    case 0x18: // quake
    case 0x26: // skull wave (the quake's handler)
        if (alloc(0x670))
        {
            const uint32_t m = mem();
            start(m, iType == 0x18 ? kQuake : kSkullWave, 0x20);
            ring(m, 0x1f);
            radius = 2500.0f;
            at<uint32_t>(m + 0x650) = obj;
        }
        break;
    case 0x19: // sarge lights
    case 0x28: // UFO burner
        if (alloc(0x660))
        {
            const uint32_t m = mem();
            start(m, iType == 0x19 ? kSargeLights : kUfoBurner, 1);
            at<uint32_t>(m + 0x650) = obj;
        }
        break;
    case 0x1a: // speed up
        if (alloc(0x650))
        {
            const uint32_t m = mem();
            start(m, kSpeedUp, 0xffff);
            ring(m, 0x3f);
            radius = 500.0f;
            at<uint32_t>(m + 0x644) = obj;
        }
        break;
    case 0x1b: // car change-over (iY the new kind): the car starts its change
        if (alloc(0x2d0))
        {
            const uint32_t m = mem();
            start(m, kChangeOver, 0x50);
            ring(m, 0x1f);
            at<int32_t>(m + 0x2c8) = iY;
            at<uint32_t>(m + 0x2c4) = obj;
            at<uint8_t>(obj + 0x176) = 1;
        }
        break;
    case 0x1c: // sparks
        if (alloc(0xf0))
        {
            const uint32_t m = mem();
            start(m, kSparks, 1);
            ring(m, 7);
            at<uint32_t>(m + 0x44) = obj;
        }
        break;
    case 0x1d: // the flying water ball: a random life of 60000 or more
        if (alloc(0x660))
        {
            const uint32_t m = mem();
            at<uint32_t>(m + 8) = kWaterBallFlying.handler;
            at<uint32_t>(m + 0xc) = kWaterBallFlying.renderer;
            const int32_t life = random15();
            radius = 500.0f;
            ring(m, 0x1f);
            at<uint16_t>(m + 0x10) = static_cast<uint16_t>((life & 0xfff) - 0x15a0);
            put3(m + 0x14, x, y, z);
            at<int32_t>(m + 0x654) = random15();
        }
        break;
    case 0x1e: // rocket smoke
        if (alloc(0x2e0))
        {
            const uint32_t m = mem();
            positioned(m, kRocketSmoke, 0xffff, 0x1f, 0x14);
            put3(m + 0x2cc, 0, 0, 0);
            radius = 1500.0f;
            at<uint32_t>(m + 0x2d4) = 0;
        }
        break;
    case 0x1f: // ready, steady, go
    case 0x2b: // finish position
        if (alloc(0x1b0))
        {
            const uint32_t m = mem();
            start(m, iType == 0x1f ? kReadySteadyGo : kFinishPosition, iType == 0x1f ? 0x3f : 0x43f);
            ring(m, 0xf);
            at<uint32_t>(m + 0x44) = obj;
            at<int32_t>(m + 0x1a4) = iY;
        }
        break;
    case 0x20: // invincible
        if (alloc(0x260))
        {
            const uint32_t m = mem();
            start(m, kInvincible, 0xffff);
            ring(m, 0x1f);
            at<uint32_t>(m + 0x250) = obj;
            at<int32_t>(m + 0x254) = iY;
        }
        break;
    case 0x21: // bomb
        if (alloc(0x2e0))
        {
            const uint32_t m = mem();
            positioned(m, kBomb, 0xffff, 0x1f, 0x14);
            radius = 500.0f;
            at<uint32_t>(m + 0x2d0) = 0;
        }
        break;
    case 0x23: // firework
        if (alloc(0x550))
            positioned(mem(), kFirework, 0x40, 0x3f, 0x544);
        break;
    case 0x24: // car respawn: the car stops colliding while it reappears
        if (alloc(0x2d0))
        {
            const uint32_t m = mem();
            start(m, kCarRespawn, 100);
            ring(m, 0x1f);
            at<uint32_t>(m + 0x2c4) = obj;
            at<uint8_t>(obj + 0x18a) = 1;
            at<uint32_t>(obj + 0x6c) = 0;
        }
        break;
    case 0x27: // car splash
        if (alloc(0x550))
        {
            const uint32_t m = mem();
            start(m, kCarSplash, 0x50);
            ring(m, 0x3f);
            at<uint32_t>(m + 0x544) = obj;
        }
        break;
    case 0x29: // butterflies
        if (alloc(0x760))
        {
            const uint32_t m = mem();
            at<uint32_t>(m + 8) = kButterflies.handler;
            at<uint32_t>(m + 0xc) = kButterflies.renderer;
            ring(m, 0xf);
            put3(m + 0x750, x, y, z);
            at<uint16_t>(m + 0x10) = 1;
        }
        break;
    case 0x2a: // the car is right (way up)
        if (alloc(0x1d0))
        {
            const uint32_t m = mem();
            start(m, kCarRight, 0x10);
            ring(m, 0xf);
            at<uint32_t>(m + 0x1c4) = obj;
        }
        break;
    case 0x2c: // time-trial clock (iZ the time)
        if (alloc(0x120))
        {
            const uint32_t m = mem();
            start(m, kTTclock, 0x80);
            ring(m, 7);
            at<uint32_t>(m + 0x44) = obj;
            at<int32_t>(m + 0x114) = iZ;
        }
        break;
    case 0x2d: // boost speed up (the speed up's renderer)
        if (alloc(0x650))
        {
            const uint32_t m = mem();
            start(m, kBoostSpeedUp, 0x14);
            ring(m, 0x3f);
            at<uint32_t>(m + 0x644) = obj;
        }
        break;
    case 0x2f: // quake 2: the shock wave's centre is the car's position
        if (alloc(0x670))
        {
            const uint32_t m = mem();
            start(m, kQuake2, 0x20);
            ring(m, 0x1f);
            at<uint32_t>(m + 0x650) = obj;
            put3(m + 0x654, x, y, z);
            radius = 2500.0f;
        }
        break;
    case 0x30: // moon rover blast: the car's position as floats
        if (alloc(0x90))
        {
            const uint32_t m = mem();
            at<float>(m + 0x50) = at<float>(obj + 0xbc);
            at<uint32_t>(m + 8) = kBlastMoonrover.handler;
            at<float>(m + 0x54) = at<float>(obj + 0xc0);
            at<uint32_t>(m + 0xc) = kBlastMoonrover.renderer;
            at<uint16_t>(m + 0x10) = 0xffff;
            at<float>(m + 0x58) = at<float>(obj + 0xc4);
            at<uint32_t>(m + 0x40) = obj;
            at<uint16_t>(m + 0x80) = 0;
        }
        break;
    case 0x31: // barge oil: a random angle, size 10, and the alpha style (1)
        if (alloc(0x50))
        {
            const uint32_t m = mem();
            start(m, kBargeOil, 0xea60);
            put3(m + 0x14, x, y, z);
            at<int16_t>(m + 0x44) = static_cast<int16_t>(random15());
            at<uint16_t>(m + 0x46) = 10;
            at<uint32_t>(m + 0x48) = 1;
        }
        break;
    case 0x32: // the car covered in oil: a random life of 60000 or more
        if (alloc(0x50))
        {
            const uint32_t m = mem();
            at<uint32_t>(m + 8) = kOilCovered.handler;
            at<uint32_t>(m + 0xc) = kOilCovered.renderer;
            at<uint16_t>(m + 0x10) = static_cast<uint16_t>((random15() & 0xfff) - 0x15a0);
            at<int16_t>(m + 0x44) = static_cast<int16_t>(random15());
            at<uint32_t>(m + 0x40) = obj;
            at<uint16_t>(m + 0x46) = 0x4b;
            at<uint32_t>(m + 0x48) = 1;
        }
        break;
    case 0x33: // arm cannon
        if (alloc(0x1a0))
        {
            const uint32_t m = mem();
            positioned(m, kArmCannon, 0xffff, 0xf, 0x184);
            put3(m + 0x14, x, y, z);
            put3(m + 0x18c, 0, 0, 0);
            radius = 1000.0f;
        }
        break;
    case 0x34: // route finder
        if (alloc(0x50))
        {
            const uint32_t m = mem();
            start(m, kRouteFinder, 0xffff);
            at<uint32_t>(m + 0x40) = obj;
        }
        break;
    case 0x35: // spray
        if (alloc(0x130))
        {
            const uint32_t m = mem();
            start(m, kSpray, 1);
            ring(m, 7);
            radius = 500.0f;
            at<uint32_t>(m + 0x44) = obj;
        }
        break;
    case 0x36: // car rocket
    case 0x37: // rocket car
        if (alloc(0x360))
        {
            const uint32_t m = mem();
            start(m, iType == 0x36 ? kCarRocket : kRocketCar, 0xffff);
            ring(m, 0xf);
            at<uint32_t>(m + 0x350) = obj;
        }
        break;
    default: // 0x2e and anything beyond 0x37 make nothing
        break;
    }

    const uint32_t m = mem();
    if (m == 0)
        return ptr32<_sVfx_Object>(0u);
    const uint32_t footer = at<uint32_t>(kFxFooter);
    at<uint16_t>(m + 0x12) = static_cast<uint16_t>(iType);
    at<float>(m + 0x28) = 1000000.0f; // the sort position: far until the handler first sets it
    at<float>(m + 0x30) = radius;
    at<uint32_t>(m + 4) = footer;
    at<uint32_t>(m) = 0;
    at<uint32_t>(m + 0x20) = 0;
    at<uint32_t>(m + 0x24) = 0;
    if (footer)
        at<uint32_t>(footer) = m;
    if (at<uint32_t>(kFxHeader) == 0)
        at<uint32_t>(kFxHeader) = m;
    at<uint32_t>(kFxFooter) = m;
    return ptr32<_sVfx_Object>(m);
}

// --- the draw lists -----------------------------------------------------------------------------------------------------

// The draw list of VfxCallRenderers and Vfx2RenderTSFX: the effects that pass `draws` go into a heap block of (effect,
// depth) pairs, those within the depth range are sorted farthest first (qsort with VfxSortFunc) and drawn in that order.
// If the block cannot be had, every effect that passes `fallback` is drawn in list order instead.
template <typename Draws, typename Fallback>
void drawSorted(uint32_t header, ee_float farthest, Draws draws, Fallback fallback)
{
    GuestFrame frame(0x90);
    const uint32_t vu = frame.local<float>(0).address;
    const ptr32<memHandle> list = frame.local<memHandle>(0x10);
    at<uint32_t>(list.address) = 0;
    int32_t count = 0;
    for (uint32_t fx = at<uint32_t>(header); fx; fx = at<uint32_t>(fx))
        if (draws(fx))
            ++count;
    if (count == 0)
        return;
    if (!MemBlockAllocLo(list, static_cast<uint32_t>(count) << 3))
    {
        for (uint32_t fx = at<uint32_t>(header); fx; fx = at<uint32_t>(fx))
            if (fallback(fx))
                guest_call(at<uint32_t>(fx + 0xc), ptr32<uint8>(fx));
        return;
    }
    uint32_t entry = at<uint32_t>(list.address);
    int32_t n = 0;
    for (uint32_t fx = at<uint32_t>(header); fx; fx = at<uint32_t>(fx))
    {
        if (!draws(fx))
            continue;
        vfx::vu0Sub(vu, fx + 0x20, kCameraPos);
        const ee_float zed = innerProduct(vu, kCameraDir);
        at<float>(entry + 4) = zed; // written for a rejected effect too (the next accepted one overwrites it)
        if (-ee_float(at<float>(fx + 0x30)) < zed && zed < farthest)
        {
            at<uint32_t>(entry) = fx;
            ++n;
            entry += 8;
        }
    }
    const uint32_t first = at<uint32_t>(list.address);
    guest_call(kQsort, ptr32<uint8>(first), n, 8, kSortFunc);
    for (int32_t i = 0; i < n; ++i)
    {
        const uint32_t fx = at<uint32_t>(first + 8 * static_cast<uint32_t>(i));
        guest_call(at<uint32_t>(fx + 0xc), ptr32<uint8>(fx));
    }
    MemBlockFree(list);
}

// --- Vfx2CallBack -------------------------------------------------------------------------------------------------------

// The scenery tags that start a scenery effect, with Vfx2AddTSFX's colour, size and type (read from the original's
// binary search, ranges included). Tags 0x100004bd and 0x10000679 start two effects, 0x10000623 is a rain area.
struct TagEffect
{
    uint32_t first, last, colour, size, type;
};
constexpr TagEffect kTagEffects[] = {
    {0x3e8, 0x3e8, 0x700000, 0x10, 1},           {0x3e9, 0x3e9, 0x606000, 0x10, 1},
    {0x3ea, 0x3ea, 0x700000, 0x10, 2},           {0x3eb, 0x3eb, 0x606000, 0x10, 2},
    {0x3ec, 0x3ec, 0x600060, 0x10, 2},           {0x3ed, 0x3ed, 0x606000, 0, 4},
    {0x3ee, 0x3ef, 0x5050, 0x20, 0x10},          {0x3f0, 0x3f0, 0x505050, 0x40, 0x10},
    {0x3f1, 0x3f1, 0x505000, 0x40, 0x10},        {0x3f2, 0x3f2, 0x5050, 0x40, 0x10},
    {0x1000023a, 0x1000023a, 0x606060, 0, 0x1c}, {0x10000244, 0x10000244, 0xf0f0f0, 0, 0xb},
    {0x10000245, 0x10000245, 0, 0, 0},           {0x1000025e, 0x1000025e, 0x606060, 0, 0x1c},
    {0x10000264, 0x10000264, 0x2060, 0, 0xe},    {0x10000265, 0x10000265, 0x4870, 0, 0xe},
    {0x10000269, 0x10000269, 0x505050, 0, 0xc},  {0x1000026a, 0x1000026a, 0x606060, 0x10, 1},
    {0x1000026b, 0x1000026b, 0x808080, 0x50, 7}, {0x1000026c, 0x1000026c, 0x806040, 0, 8},
    {0x1000026d, 0x1000026d, 0x6060, 0, 4},      {0x1000026e, 0x1000026e, 0x6000, 0x10, 1},
    {0x1000026f, 0x1000026f, 0x606060, 0, 3},    {0x10000270, 0x10000270, 0x686868, 0, 0xf},
    {0x10000271, 0x10000271, 0x806040, 0, 9},    {0x10000272, 0x10000272, 0x806040, 0, 0xa},
    {0x10000273, 0x10000273, 0x606060, 0, 4},    {0x10000274, 0x10000274, 0x606060, 0, 0xd},
    {0x10000275, 0x10000275, 0x606060, 0, 6},    {0x10000276, 0x10000276, 0x900000, 0x40, 0xe},
    {0x10000277, 0x10000277, 0x505050, 0x20, 0x10}, {0x10000279, 0x10000279, 0x505000, 0x20, 0x10},
    {0x1000027a, 0x1000027a, 0x6000, 0, 4},      {0x1000027b, 0x1000027b, 0x606060, 0x10, 2},
    {0x1000027c, 0x1000027c, 0x606060, 0, 5},    {0x1000027d, 0x1000027d, 0x6000, 0x10, 2},
    {0x1000027e, 0x1000027e, 0x6060, 0x10, 2},   {0x1000027f, 0x1000027f, 0x209060, 0, 4},
    {0x10000280, 0x10000280, 0x505000, 0x20, 0x10}, {0x10000281, 0x10000281, 0x505050, 0x20, 0x10},
    {0x1000034e, 0x1000034e, 0x606060, 0, 0x16}, {0x1000035c, 0x1000035c, 0x6060, 0, 0x13},
    {0x1000036f, 0x1000036f, 0x6060, 0, 0x19},   {0x10000374, 0x10000374, 0x6060, 0, 0x13},
    {0x10000428, 0x10000428, 0x606060, 0, 0x1c}, {0x10000457, 0x10000457, 0x606060, 0, 0x11},
    {0x10000476, 0x10000476, 0x808080, 0, 0x14}, {0x1000047b, 0x1000047b, 0, 0, 0},
    {0x10000480, 0x10000480, 0x6060, 0, 0x15},   {0x100004d6, 0x100004d9, 0x6060, 0, 0x19},
    {0x100004ef, 0x100004ef, 0x606060, 0, 0x11}, {0x100004f2, 0x100004f2, 0x606060, 0, 0x11},
    {0x10000501, 0x10000501, 0, 0, 0},           {0x10000540, 0x10000540, 0x60, 0x10, 1},
    {0x100005d4, 0x100005d4, 0, 0, 0},           {0x100005f1, 0x100005f3, 0x6060, 0, 0x19},
    {0x100005f4, 0x100005f4, 0x6060, 0, 0x18},   {0x10000649, 0x10000649, 0x606060, 0, 0x1c},
    {0x10000678, 0x10000678, 0x706000, 0, 0x1b},
};

// --- the sky --------------------------------------------------------------------------------------------------------------

// The sky's brightness while the sky terror plays: rising for its first half, falling for the second, as a grey.
void skyColour()
{
    int32_t level = at<int32_t>(kSkyTerror);
    level = level < 0x40 ? level << 1 : (0x7f - level) << 1;
    const uint32_t colour = static_cast<uint32_t>(level + (level << 8) + (level << 16));
    VfxPutGT4rgba0(colour);
    VfxPutGT4rgba1(colour);
    VfxPutGT4rgba2(colour);
    VfxPutGT4rgba3(colour);
}

// One layer of the merged sky: a dome of 9 x 9 quads, each `size` wide, centred on the camera, `top` high in the middle
// and falling with the square of the distance from the centre. xShift and zShift move the grid; the texture coordinates
// (ua, ub across, va, vb along) scroll with the time.
void skyLayer(uint32_t style, ee_float size, ee_float top, ee_float xShift, ee_float zShift, ee_float ua, ee_float ub, ee_float va, ee_float vb)
{
    RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0x12)), style);
    for (ee_float row = 0.0f; row < ee_float(9.0f);)
    {
        const ee_float a = row - ee_float(4.0f), b = row - ee_float(5.0f);
        const ee_float a2 = a * a, b2 = b * b;
        const ee_float z0 = a * size - zShift, z1 = z0 - size;
        for (ee_float col = 0.0f; col < ee_float(9.0f);)
        {
            const ee_float c = col - ee_float(4.0f), d = col - ee_float(3.0f);
            col = col + ee_float(1.0f);
            const ee_float c2 = c * c, d2 = d * d;
            const uint32_t quad = G(visFxVarG).pGT4.address;
            at<float>(quad + 0x38) = z1;
            at<float>(quad + 0x08) = z0;
            at<float>(quad + 0x18) = z0;
            at<float>(quad + 0x28) = z1;
            const ee_float x0 = c * size - xShift, x1 = x0 + size;
            const ee_float hundred = 100.0f;
            at<float>(quad + 0x20) = x0;
            at<float>(quad + 0x00) = x0;
            at<float>(quad + 0x30) = x1;
            at<float>(quad + 0x04) = top - (c2 + a2) * hundred;
            at<float>(quad + 0x14) = top - (d2 + a2) * hundred;
            at<float>(quad + 0x24) = top - (c2 + b2) * hundred;
            at<float>(quad + 0x34) = top - (d2 + b2) * hundred;
            at<float>(quad + 0x10) = x1;
            VfxPutUV0(ua, vb);
            VfxPutUV1(ub, vb);
            VfxPutUV2(ua, va);
            VfxPutUV3(ub, va);
            for (uint32_t corner = 0; corner < 0x40; corner += 0x10)
            {
                const uint32_t v = G(visFxVarG).pGT4.address + corner;
                vfx::vu0Add(v, v, kCameraPos);
            }
            RdrPoly(0, G(visFxVarG).pGT4);
        }
        row = row + ee_float(1.0f);
    }
}

inline ee_float skyTime(int32_t shift) { return ee_itof((at<int32_t>(kMainTime) >> shift) & 0x7f); }

// The text and sprite colour of a finishing place (1st, 2nd, 3rd), grey for the rest.
uint32_t placeColour(uint32_t place)
{
    const int32_t index = static_cast<int32_t>(place) - 1;
    if (index == 1)
        return 0x808080;
    if (index < 2)
        return index == 0 ? 0x8080 : 0x606060;
    return index == 2 ? 0x306080 : 0x606060;
}

// The finish text followed by "PLAYER n" (the string at `name`), centred at the top. The finish text's last character is
// overwritten by the separator byte at 0x328110 first, as the original's copies do.
void printFinishLine(uint32_t name)
{
    const uint32_t buff = kTextBuffer;
    uint32_t n = 0;
    while (at<char>(buff + n) != 0)
        ++n;
    at<int8_t>(buff + n - 1) = at<int8_t>((sym::sec_sdata + 0x510u));
    at<int8_t>(buff + n) = at<int8_t>((sym::sec_sdata + 0x511u));
    for (uint32_t i = 0; i < 10; ++i)
        at<int8_t>(buff + n + i) = at<int8_t>(name + i);
}

void printCentredTop()
{
    const uint32_t length = FontLength(ptr32<char>(kTextBuffer));
    const int32_t half = static_cast<int32_t>(length << 15) >> 16;
    FontPrint(static_cast<int16>(0x140 - half), 0x50, ptr32<char>(kTextBuffer));
}

// "Player n" left of the place sprite on the podium line y; returns the half width the sprite is placed after.
int32_t printPlayerName(uint32_t name, int32_t y)
{
    guest_call(kSprintf, ptr32<char>(kTextBuffer), ptr32<char>(name));
    const uint32_t length = FontLength(ptr32<char>(kTextBuffer));
    const int32_t width = static_cast<int16_t>((length >> 1) + 0x18);
    FontPrint(static_cast<int16>(0x140 - width), static_cast<int16>(y), ptr32<char>(kTextBuffer));
    return width;
}

inline uint32_t placeSprite(uint32_t hud, uint32_t place) { return at<uint32_t>(hud + 0x24 + 4 * (place - 1)); }

// A podium car stops sliding and spinning; on its wheels it now and then hops (the speed at `hop`).
void holdPodiumCar(uint32_t obj, uint32_t hop, bool spinToo)
{
    const bool touching = VfxIsWheelTouching(ptr32<_sObject>(obj), 0) != 0;
    if (touching)
    {
        const int32_t r = rand();
        const uint32_t physics = at<uint32_t>(obj + 0x20);
        if (((r >> 15) & 0x1f) == 0)
            at<float>(physics + 0x24) = at<float>(hop);
    }
    const uint32_t physics = at<uint32_t>(obj + 0x20);
    if (spinToo)
    {
        at<uint32_t>(physics + 0x48) = 0;
        at<uint32_t>(physics + 0x20) = 0;
        at<uint32_t>(physics + 0x28) = 0;
        at<uint32_t>(physics + 0x40) = 0;
        at<uint32_t>(physics + 0x44) = 0;
    }
    else
    {
        at<uint32_t>(physics + 0x28) = 0;
        at<uint32_t>(physics + 0x20) = 0;
    }
}

} // namespace

ptr32<_sVfx_Object> VfxAddNew(ptr32<_sObject> pPlayerObj, int32 iY, int32 iZ, int32 iType)
{
    return addNew(pPlayerObj, iY, iZ, iType);
}

// The effects task, every frame: gives each car that has none yet its attached effects (skid marks and sparks in the
// front end's car viewer, the aerial, the UFO burner, the rocket car's flame, the route finder), runs the handlers,
// advances the sky terror, frees the effects whose life ran out, starts the boat wake and spray for players whose state
// changed, and puts the task back in the queue. The aerial is stored in the first car, whichever car it was made for
// (as in the original).
void VfxUpdate(ptr32<void> arg0, tTime time)
{
    RC_DIVERT_IF_CHANGED(VfxUpdate, arg0, time);
    GuestFrame frame(0xb0);
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    const uint32_t first = at<uint32_t>(kObjects);
    if (at<int32_t>(kMainToDo) != 1 && first != 0)
    {
        const uint32_t integer = at<uint32_t>(at<uint32_t>(first + 0x20) + 0x1e8);
        SVECTOR &pos = G(svVelTemp);
        pos.vz = static_cast<int16>(at<int32_t>(integer + 0xc) >> 8);
        pos.vx = static_cast<int16>(at<int32_t>(integer + 4) >> 8);
        pos.vy = static_cast<int16>(at<int32_t>(integer + 8) >> 8);
    }
    const auto count = []() { return at<uint32_t>(kObjects + 0x170); };
    const auto object = [](uint32_t i) { return at<uint32_t>(kObjects + 4 * i); };
    const auto add = [](uint32_t obj, int32_t y, int32_t type) { return addNew(ptr32<_sObject>(obj), y, 0, type).address; };
    for (uint32_t i = 0; i < count(); ++i)
    {
        at<uint16_t>(kPlayerStates + 4 * i) = 0;
        if (at<uint8_t>(object(i) + 0x177) != 0)
            continue;
        if (at<uint8_t>(kFrontEnd + 0x2d9) == 5)
        {
            const uint32_t left = add(object(i), 2, 7);
            at<uint32_t>(object(i) + 0x178) = left;
            const uint32_t right = add(object(i), 3, 7);
            at<uint32_t>(object(i) + 0x17c) = right;
            const uint32_t sparks = add(object(i), 0, 0x1c);
            at<uint32_t>(object(i) + 0x180) = sparks;
        }
        const uint32_t aerial = add(object(i), 0, 0xf);
        const uint32_t obj = object(i);
        at<uint32_t>(first + 0x40) = aerial;
        const int32_t type = at<int32_t>(at<uint32_t>(obj + 0xc) + 4);
        if (type == 0xf || type == 0x22) // UFO, UFO boat
            add(obj, 0, 0x28);
        const int32_t type2 = at<int32_t>(at<uint32_t>(object(i) + 0xc) + 4);
        if (type2 == 0x13 || type2 == 0x26) // rocket car, rocket car boat
            add(object(i), 0, 0x37);
        if (at<uint8_t>(object(i) + 0x44) != 2)
            add(object(i), 0, 0x34);
        at<uint8_t>(object(i) + 0x177) = 1;
    }
    if (at<int32_t>(kMainToDo) != 1)
    {
        const uint32_t lvl = at<uint32_t>(kLvl);
        VfxSkyActualSlowChange(ptr32<uint8>(at<uint32_t>(lvl + 0x50)), ptr32<uint8>(at<uint32_t>(lvl + 0x4c)));
    }
    VfxCallHandlers();
    const int32_t terror = at<int32_t>(kSkyTerror);
    if (terror != 0)
    {
        at<int32_t>(kSkyTerror) = terror + 1;
        if (terror + 1 == 0x80)
            at<int32_t>(kSkyTerror) = 0;
    }

    // Free the effects whose life has run out.
    at<uint32_t>(handle.address) = at<uint32_t>(kFxHeader);
    while (const uint32_t fx = at<uint32_t>(handle.address))
    {
        if (at<uint16_t>(fx + 0x10) != 0)
        {
            at<uint32_t>(handle.address) = at<uint32_t>(fx);
            continue;
        }
        const uint32_t prev = at<uint32_t>(fx + 4);
        if (prev)
            at<uint32_t>(prev) = at<uint32_t>(fx);
        const uint32_t next = at<uint32_t>(fx);
        if (next)
            at<uint32_t>(next + 4) = at<uint32_t>(fx + 4);
        if (fx == at<uint32_t>(kFxHeader))
        {
            const uint32_t head = at<uint32_t>(fx);
            at<uint32_t>(kFxHeader) = head;
            if (head)
                at<uint32_t>(head + 4) = 0;
        }
        if (fx == at<uint32_t>(kFxFooter))
        {
            const uint32_t foot = at<uint32_t>(fx + 4);
            at<uint32_t>(kFxFooter) = foot;
            if (foot)
                at<uint32_t>(foot) = 0;
        }
        const uint32_t after = at<uint32_t>(fx);
        MemBlockFree(handle);
        at<uint32_t>(handle.address) = after;
    }

    // A player whose state changed: onto the water starts a boat wake, and a wet state without the 0x40 bit a spray.
    for (uint32_t i = 0; i < count(); ++i)
    {
        const int16_t now = at<int16_t>(kPlayerStates + 4 * i);
        if (now == at<int16_t>(kPlayerStates + 4 * i + 2))
            continue;
        at<int16_t>(kPlayerStates + 4 * i + 2) = now;
        if ((now & 6) == 2)
            add(object(i), 0, 0xd);
        if ((now & 0x42) == 2)
            add(object(i), 0, 0x35);
    }
    at<int32_t>(kVisFx + 4) = time + 0x19; // visFxVarG.Task.time
    guest_call(kTaskCachedInsert, ptr32<uint8>(kVisFx));

    // A debug key pair that steps a test effect number.
    if (first == 0)
        return;
    if (InputHeld(0x18) || InputHeld(0x19))
    {
        if (at<int32_t>(kTestRP) != 0)
            return;
        InputHeld(0x18);
        if (InputHeld(0x19))
        {
            at<int32_t>(kTestFX) = at<int32_t>(kTestFX) + 1;
            if (at<int32_t>(kTestFX) == 8)
                at<int32_t>(kTestFX) = 0;
        }
        at<int32_t>(kTestRP) = 1;
    }
    else
        at<int32_t>(kTestRP) = 0;
}

// Draws the effects that have life and a renderer, depth-sorted, within 10000 units.
void VfxCallRenderers()
{
    drawSorted(
        kFxHeader, 10000.0f, [](uint32_t fx) { return at<uint16_t>(fx + 0x10) != 0 && at<uint32_t>(fx + 0xc) != 0; },
        [](uint32_t fx) { return at<uint16_t>(fx + 0x10) != 0 && at<uint32_t>(fx + 8) != 0; });
}

// Draws the scenery effects that have a renderer, depth-sorted, within 32000 units.
void Vfx2RenderTSFX()
{
    drawSorted(
        kTsfxHeader, 32000.0f, [](uint32_t fx) { return at<uint32_t>(fx + 0xc) != 0; }, [](uint32_t fx) { return at<uint32_t>(fx + 8) != 0; });
}

// The scenery's tag callback: a tagged instance starts its scenery effect (or a rain area).
void Vfx2CallBack(ptr32<_sTOBInst> pTOBInst, uint32 tag)
{
    if (tag == 0x10000623)
    {
        Vfx3AddRain(pTOBInst);
        return;
    }
    if (tag == 0x100004bd || tag == 0x10000679)
    {
        {
            GuestFrame frame(0x20);
            if (tag == 0x100004bd)
                Vfx2AddTSFX(pTOBInst, 0x70f0, 0xff, 0x12);
            else
                Vfx2AddTSFX(pTOBInst, 0x50f0, 0xff, 0x1a);
        }
        Vfx2AddTSFX(pTOBInst, 0x807848, 0xff, 0x17);
        return;
    }
    for (const TagEffect &effect : kTagEffects)
        if (tag >= effect.first && tag <= effect.last)
        {
            Vfx2AddTSFX(pTOBInst, effect.colour, effect.size, effect.type);
            return;
        }
}

// The podium after a race (visFxVarG.iResultTrigger counts down from about 4000): the cars on the podium are released
// in turn (third, then second, then first and the last two), fireworks go up with their bangs and crackles, the options
// menu is set up, and a key (or the count reaching 0x100) hands over to the front end.
void Vfx2PodiumPlay()
{
    RC_DIVERT_IF_CHANGED(Vfx2PodiumPlay);
    {
        GuestFrame frame(0x50);
        const uint32_t start = at<uint32_t>(at<uint32_t>(kLvl) + 0x30); // pLvlG->pStartData: the start positions, 8 bytes each
        int32_t &trigger = at<int32_t>(kResultTrigger);
        if (trigger == 0)
            return;
        trigger = trigger - 1;
        // Drops a podium car (its flag 2 is held until now) onto its place: start position `place`'s height plus `lift`.
        const auto release = [&](uint32_t slot, uint32_t place, int32_t lift) {
            const uint32_t obj = at<uint32_t>(kObjects + 4 * slot);
            if (obj == 0)
                return;
            const uint32_t physics = at<uint32_t>(obj + 0x20);
            const uint8_t flags = at<uint8_t>(physics + 0x1cd);
            if ((flags & 2) == 0)
                return;
            const int32_t height = sh16(start + 8 * place + 2);
            at<uint8_t>(physics + 0x1cd) = static_cast<uint8_t>(flags - 2);
            at<float>(physics + 0x18) = ee_itof((height + lift) << 8);
        };
        switch (trigger)
        {
        case 0xf96:
            release(0x17, 4, 0x32c);
            release(0x16, 3, 0x32c);
            break;
        case 0xf6e:
            release(0x15, 2, 0x1cc);
            break;
        case 0xf26:
            release(0x14, 1, 0xa0);
            break;
        case 0xeea:
            release(0x13, 0, 0xa0);
            break;
        default:
            break;
        }

        int32_t spread = trigger < 0xe75 ? 0xf : 0x3f; // the chance of a firework; becomes its x once one goes up
        bool tail = false;
        if (((rand() >> 15) & spread) == 0)
        {
            if (trigger < 0x100)
                tail = true;
            else
            {
                const int32_t rx = rand();
                spread = sh16(at<uint32_t>(at<uint32_t>(kLvl) + 0x30) + 0x28) + ((rx >> 15) & 0x7ff) - 0x400;
                const int32_t ry = rand();
                const int32_t fy = sh16(at<uint32_t>(at<uint32_t>(kLvl) + 0x30) + 0x2a) + ((ry >> 15) & 0x3ff) + 0x384;
                const int32_t rz = rand();
                const int32_t fz = sh16(at<uint32_t>(at<uint32_t>(kLvl) + 0x30) + 0x2c) + ((rz >> 15) & 0xfff) - 0x7e8;
                VfxAddNewPlace(spread, fy, fz, 0x23);
                const int32_t distance = fz < 0 ? -fz : fz;
                const int32_t volume = (6000 - distance) / 0x7d + 0x60;
                const int32_t pitch = (rand() >> 15) % 64 + 0x100;
                guest_call(kSfxPlay, 0x3f, volume, 0xf, 0xf, pitch, 0, 0);
            }
        }
        if (!tail && trigger >= 0x100)
        {
            const int32_t bang = rand() >> 15;
            if (spread < 0)
                spread = -spread;
            // The original divides with a trap on zero (a firework at x = -60).
            if (spread + 0x3c != 0 && bang % (spread + 0x3c) == 0)
            {
                const int32_t a = (rand() >> 15) % 32 + 0x40;
                const int32_t b = (rand() >> 15) % 32 + 0x100;
                guest_call(kSfxPlay, 0x41, a, 0xf, 0xf, b, 0, 0);
            }
            int32_t &crackle = at<int32_t>(kCrackle);
            if (crackle < 0x20)
            {
                if ((rand() >> 15) % 3 == 0)
                {
                    const int32_t a = (rand() >> 15) % 32 + 0x60;
                    guest_call(kSfxPlay, 0x40, a, 0xf, 0xf, 0x100, 0, 0);
                    crackle = crackle + 1;
                }
            }
            else
            {
                const int32_t r = rand() >> 15;
                if (spread + 0x50 != 0 && r % (spread + 0x50) == 0)
                    crackle = 0;
            }
        }

        const int32_t now = trigger;
        if (now == 0 || now >= 0xdac)
            return;
        if (now == 0xdab) // the menu behind the podium becomes the options menu
            at<uint32_t>(at<uint32_t>(kMenuSystem)) = at<uint32_t>(at<uint32_t>(kFrontEnd) + 0xf4);
        if (now < 0x100)
            return;
        if (!guest_call<bool>(kInputHit, 0xe) && !guest_call<bool>(kInputHit, 0x1e) && trigger != 0x100)
            return;
        guest_call(kHandoverToFE);
        trigger = 0xff;
    }
}

// The sky: while the sky terror plays the merged sky is drawn (its weight rising, holding, flickering and falling); after
// a race the podium's sky and texts (the finishing place and the players' names and place sprites) and the podium cars
// held still; otherwise the level's sky backdrop.
void VfxSkyChangeStuff()
{
    const uint32_t hud = at<uint32_t>(at<uint32_t>(kLvl) + 0x40); // pLvlG->pHudRes
    const int32_t terror = at<int32_t>(kSkyTerror);
    if (terror != 0)
    {
        const int32_t t = at<int16_t>(kSkyTerror);
        if (t < 0x20)
        {
            at<int32_t>(kCueForward) = t;
            VfxSkyBackdropMerge(static_cast<int16>(t << 7));
            return;
        }
        if (t >= 0x61)
        {
            const int32_t cue = static_cast<int16_t>(0x7f - t);
            at<int32_t>(kCueForward) = cue;
            VfxSkyBackdropMerge(static_cast<int16>(cue << 7));
            return;
        }
        if ((terror & 0x10) == 0)
            VfxSkyBackdropMerge(static_cast<int16>((-terror & 7) << 9));
        else
            VfxSkyBackdropMerge(0x1000);
        return;
    }
    if (at<int32_t>(kResultTrigger) == 0)
    {
        if (at<uint16_t>(kSky) != 0)
            VfxSkyBackdrop(ptr32<uint8>(at<uint32_t>(at<uint32_t>(kLvl) + 0x4c)));
        return;
    }

    GuestFrame frame(0xa0);
    if (at<uint16_t>(kSky) != 0)
    {
        const int32_t t = at<int16_t>(kResultTrigger);
        int32_t merge = 0x1000;
        if (t >= 0xf01)
            merge = static_cast<int32_t>(static_cast<uint32_t>(t - 0xf00) << 16) >> 12;
        else if (t < 0x100)
            merge = static_cast<int32_t>(static_cast<uint32_t>(0x100 - t) << 16) >> 12;
        VfxSkyBackdropMerge(static_cast<int16>(merge));
    }
    if (at<int32_t>(kResultTrigger) < 0x100)
        return;

    int32_t y1 = 0x6e, y2 = 0x8c; // the podium lines of player 1 and player 2 (swapped if player 2 finished higher)
    FontSet(ptr32<font>(at<uint32_t>(at<uint32_t>(kFrontEnd) + 0x1c)));
    FontSetColour(0x7f, 0x7f, 0x20);
    const auto p1 = []() { return at<uint16_t>(kAi + 0x64); };
    const auto p2 = []() { return at<uint16_t>(kAi + 0x66); };
    if (p2() == 0)
    {
        if (p1() < 4)
        {
            Vfx2FinishText(static_cast<int16>(p1()), ptr32<char>(kTextBuffer));
            printCentredTop();
        }
    }
    else if (p1() < 4 && p1() < p2())
    {
        Vfx2FinishText(static_cast<int16>(p1()), ptr32<char>(kTextBuffer));
        printFinishLine((sym::sec_rodata + 0x1f70u)); // "PLAYER 1"
        printCentredTop();
    }
    else if (p2() < 4 && p2() < p1())
    {
        Vfx2FinishText(static_cast<int16>(p2()), ptr32<char>(kTextBuffer));
        y1 = 0x8c;
        y2 = 0x6e;
        printFinishLine((sym::sec_rodata + 0x1f80u)); // "PLAYER 2"
        printCentredTop();
    }
    FontSetColour(0x7f, 0x7f, 0x7f);

    if (p1() != 0)
    {
        if (at<int32_t>(kResultTrigger) < 4000)
        {
            const uint32_t colour = placeColour(p1()) | 0x2000000;
            if (p2() != 0)
            {
                const int32_t width = printPlayerName((sym::sec_rodata + 0x1f90u), y1); // "Player 1"
                guest_call(kHudSprite, ptr32<uint8>(placeSprite(hud, p1())), ee_itof(width + 0x122), ee_itof(y1 - 10), colour);
            }
            else
                guest_call(kHudSprite, ptr32<uint8>(placeSprite(hud, p1())), ee_float(288.0f), ee_itof(y1 - 10), colour);
        }
        const uint32_t obj = at<uint32_t>(kObjects + 4 * at<uint16_t>(kAi + 0x68));
        if (obj)
            holdPodiumCar(obj, sym::lit4_327894, true);
    }
    if (p2() != 0)
    {
        if (at<int32_t>(kResultTrigger) < 0xf3c)
        {
            const int32_t width = printPlayerName((sym::sec_rodata + 0x1fa0u), y2); // "Player 2"
            const uint32_t colour = placeColour(p2()) | 0x2000000;
            guest_call(kHudSprite, ptr32<uint8>(placeSprite(hud, p2())), ee_itof(width + 0x122), ee_itof(y2 - 10), colour);
        }
        const uint32_t obj = at<uint32_t>(kObjects + 4 * at<uint16_t>(kAi + 0x6a));
        if (obj)
            holdPodiumCar(obj, sym::lit4_327898, false);
    }
}

// The sky terror's merged sky: three layers of a camera-centred dome in the sky texture, lit by the terror's weight and
// scrolling with the time at different speeds (the argument is not used).
void VfxSkyBackdropMerge(int16)
{
    GuestFrame frame(0x160);
    const ee_float size = at<float>(sym::lit4_327860);   // 6000: a quad's width
    const ee_float shiftA = at<float>(sym::lit4_327864); // 3000
    const ee_float shiftB = at<float>(sym::lit4_327868); // 3000
    const ee_float one = 1.0f, quarterTurn = 0.0078125f;

    skyColour();
    const ee_float u1 = -skyTime(5) * quarterTurn, v1 = u1 + one;
    skyLayer(0x12, size, 2000.0f, shiftB, shiftA, u1, v1, u1, v1);

    skyColour();
    const ee_float half = size * ee_float(0.5f);
    const ee_float t2 = skyTime(6) * quarterTurn;
    skyLayer(0x14, size, ee_float(2000.0f) - ee_float(500.0f), half, half, t2, t2 + one, t2, t2 - one);

    skyColour();
    const ee_float t3 = skyTime(5) * quarterTurn;
    const ee_float u3 = -t3;
    skyLayer(0x14, size, ee_float(1500.0f) - ee_float(500.0f), half, half, u3, u3 - one, t3, t3 + one);
}

// The rain, each frame in a level with rain areas: it rains when the camera is inside one (up to two drops start per
// frame outside, all inside). A new drop starts high above a random point ahead of the camera (the camera's motion over
// the last frames is projected forward), finds the ground below with a collision cast and lives until it falls that far;
// a falling drop is drawn as a streak along its fall less the camera's motion; a drop that has just landed splashes (a
// ground sprite). The camera's position and matrix are kept for the next frame, per split screen.
void Vfx3Rain()
{
    GuestFrame frame(0x140);
    const uint32_t vu = frame.local<float>(0).address, vu2 = vu + 0x10, vu3 = vu + 0x20, mat = vu + 0x30, material = vu + 0x80;
    constexpr uint32_t kParts = (sym::visFxVarG + 0x1c0u), kRaining = (sym::visFxVarG + 0xe60u), kTobs = (sym::visFxVarG + 0xe64u), kCamPos16 = (sym::visFxVarG + 0x108u);
    at<int32_t>(kRaining) = 0;
    if (at<uint32_t>(kTobs) == 0)
        return;
    const int32_t screen = at<int32_t>(kWhichSplitScreen);
    const uint32_t oldCam = kVisFx + static_cast<uint32_t>(screen * 0x10 + 0xdc0);
    const uint32_t oldMat = kVisFx + static_cast<uint32_t>((screen << 6) + 0xde0);

    // Inside a rain area? (an octagonal distance from the camera, in whole units, against the area's radius)
    int32_t starts = 2; // the drops that may still start this frame: none outside the rain
    const int32_t cx = sh16(kCamPos16), cy = sh16(kCamPos16 + 2), cz = sh16(kCamPos16 + 4);
    const auto apart = [](int32_t v, int32_t c) {
        const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(v) - static_cast<uint32_t>(c));
        return d > 0 ? d : static_cast<int32_t>(static_cast<uint32_t>(c) - static_cast<uint32_t>(v));
    };
    for (uint32_t slot = kTobs;;)
    {
        const uint32_t tob = at<uint32_t>(slot);
        const int32_t dx = apart(at<int32_t>(tob + 4) >> 8, cx);
        const int32_t dy = apart(at<int32_t>(tob + 8) >> 8, cy);
        const int32_t dz = apart(at<int32_t>(tob + 0xc) >> 8, cz);
        int32_t d = dx < dy ? dy + (dx >> 1) : dx + (dy >> 1);
        d = d < dz ? dz + (d >> 1) : d + (dz >> 1);
        if (d < static_cast<int32_t>(at<uint16_t>(tob + 0x2c)))
        {
            at<int32_t>(kRaining) = 1;
            starts = 0;
            break;
        }
        slot += 4;
        if (at<uint32_t>(slot) == 0 || slot >= kTobs + 0x40)
            break;
    }

    // The camera's motion since the last frame, and its turn raised to the 32nd power ahead of the current matrix.
    vfx::vu0Sub(vu2, kCameraPos, oldCam);
    guest_call(kMulMatrix, ptr32<float>(mat), ptr32<float>((sym::visFxVarG + 0x170u)), ptr32<float>(oldMat));
    for (int i = 0; i < 5; ++i)
        guest_call(kMulMatrix, ptr32<float>(mat), ptr32<float>(mat), ptr32<float>(mat));
    guest_call(kMulMatrix, ptr32<float>(mat), ptr32<float>((sym::visFxVarG + 0x170u)), ptr32<float>(mat));

    int32_t drops = 0;
    for (uint32_t p = kParts; p < kParts + 0xc00; p += 0x30)
    {
        if (at<int16_t>(p + 0x10) == 0)
        {
            if (starts >= 2)
                continue;
            at<float>(vu + 4) = 1024.0f;
            at<int16_t>(p + 0x12) = static_cast<int16_t>(((rand() >> 15) & 0x3f) + 0x1e);
            const ee_float ahead = ee_itof(rand()) * float_from_bits(0x35bb8000u) + ee_float(150.0f);
            const ee_float spread = ahead - ee_float(140.0f);
            at<float>(vu + 8) = ahead;
            const ee_float across = ee_itof(rand());
            at<float>(vu) = spread * float_from_bits(0x30000000u) * (across * ee_float(1.5f)) - spread * ee_float(0.75f);
            sceVu0ApplyMatrix(ptr32<ee_float>(vu), ptr32<ee_float>(mat), ptr32<ee_float>(vu));
            vfx::vu0Add(vu, vu, kCameraPos);
            sceVu0ScaleVector(ptr32<ee_float>(vu3), ptr32<ee_float>(vu2), 23.0f);
            vfx::vu0Add(p, vu, vu3);
            if (AColDownCast(ptr32<_sColCellVecPSX>(p), ptr32<_sColCellVecPSX>(vu), ptr32<_sColNormalPSX>(vu3), ptr32<uint8>(material)))
            {
                const ee_float ground = at<float>(vu + 4);
                const ee_float life = (ee_float(at<float>(p + 4)) - ground) * float_from_bits(0x3ccccccdu);
                const ee_float k4096 = 4096.0f;
                at<float>(p + 0x18) = ground;
                at<int16_t>(p + 0x10) = static_cast<int16_t>(ee_ftoi(life));
                at<int16_t>(p + 0x1c) = static_cast<int16_t>(ee_ftoi(ee_float(at<float>(vu3)) * k4096));
                at<int16_t>(p + 0x1e) = static_cast<int16_t>(ee_ftoi(ee_float(at<float>(vu3 + 4)) * k4096));
                at<int16_t>(p + 0x20) = static_cast<int16_t>(ee_ftoi(ee_float(at<float>(vu3 + 8)) * k4096));
            }
            else
            {
                at<int16_t>(p + 0x10) = 0x32;
                at<float>(p + 0x18) = -1000.0f;
                at<int16_t>(p + 0x1e) = 0x1000;
                at<int16_t>(p + 0x1c) = 0;
                at<int16_t>(p + 0x20) = 0;
            }
            ee_float scale = spread * ee_float(0.25f);
            if (ee_float(200.0f) < scale)
                scale = 200.0f;
            if (scale < ee_float(30.0f))
                scale = 30.0f;
            at<float>(p + 0x14) = scale;
            ++starts;
            ++drops;
        }
        else
        {
            const ee_float y = ee_float(at<float>(p + 4)) - ee_float(40.0f);
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>(at<uint16_t>(p + 0x10) - 1);
            ++drops;
            at<float>(vu) = at<float>(p);
            at<float>(vu + 8) = at<float>(p + 8);
            at<float>(vu + 4) = y - ee_float(500.0f);
            at<float>(p + 4) = y;
            sceVu0ScaleVector(ptr32<ee_float>(vu3), ptr32<ee_float>(vu2), 8.0f);
            vfx::vu0Sub(vu, vu, vu3);
            const int32_t shade = at<int16_t>(p + 0x12);
            const uint32_t colour = static_cast<uint32_t>(shade + (shade << 16)) + (static_cast<uint32_t>(shade << 8) - 0x80000000u);
            VfxPutGT4rgba0(colour);
            VfxPutGT4rgba1(colour);
            VfxPutGT4rgba2(colour);
            VfxPutGT4rgba3(colour);
            VfxPutGT4FacingPolyVU(ptr32<ee_float>(vu), ptr32<ee_float>(p), ptr32<_sGfxTexture>(at<uint32_t>(vfxRes() + 0x74)), 0x20,
                                  at<float>(p + 0x14), -10.0f);
        }
    }
    at<int32_t>((sym::revSfxVarG + 0x116cu)) = drops; // revSfxVarG.rain: the rain sound's strength
    at<uint8_t>(G(visFxVarG).sprite.address + 0x24) = 2;

    // The splashes of the drops that have just landed.
    for (uint32_t p = kParts; p < kParts + 0xc00; p += 0x30)
    {
        if (static_cast<uint32_t>(at<uint16_t>(p + 0x10)) - 1u >= 0xcu)
            continue;
        sceVu0CopyVector(ptr32<ee_float>(G(visFxVarG).sprite.address + 0x10), ptr32<ee_float>(p));
        at<float>(G(visFxVarG).sprite.address + 0x14) = at<float>(p + 0x18);
        const ee_float scatter = float_from_bits(0x32c80000u), twentyFive = 25.0f;
        {
            const ee_float r = ee_itof(rand());
            const uint32_t s = G(visFxVarG).sprite.address;
            at<float>(s + 0x10) = ee_float(at<float>(s + 0x10)) + (r * scatter - twentyFive);
        }
        const ee_float r = ee_itof(rand());
        const uint32_t s = G(visFxVarG).sprite.address;
        at<float>(s + 0x18) = ee_float(at<float>(s + 0x18)) + (r * scatter - twentyFive);
        const int32_t life = at<int16_t>(p + 0x10);
        at<uint32_t>(s + 0x20) = at<uint32_t>(vfxRes() + 0x70);
        at<int32_t>(s + 0x28) = (life << 1) + (life << 9) + (life << 17);
        at<float>(s) = ee_itof((life << 4) + 8);
        VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(p + 0x1c), -40.0f);
    }
    sceVu0CopyVector(ptr32<ee_float>(oldCam), ptr32<ee_float>(kCameraPos));
    guest_call(kCopyMatrix, ptr32<float>(oldMat), ptr32<float>((sym::visFxVarG + 0x120u)));
}

} // namespace rc

// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxAddNew(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxUpdate(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCallRenderers(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRenderTSFX(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCallBack(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxPodiumPlay(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRain(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSkyMerge(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSkyChange(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP_HEAP(VfxAddNew, rc::fuzz::vfxAddNew)
RC_DECOMP_FN_SETUP_HEAP(VfxUpdate, rc::fuzz::vfxUpdate)
RC_DECOMP_FN_SETUP_HEAP(VfxCallRenderers, rc::fuzz::vfxCallRenderers)
RC_DECOMP_FN_SETUP_HEAP(Vfx2RenderTSFX, rc::fuzz::vfxRenderTSFX)
RC_DECOMP_FN_SETUP_HEAP(Vfx2CallBack, rc::fuzz::vfxCallBack)
RC_DECOMP_FN_SETUP_HEAP(Vfx2PodiumPlay, rc::fuzz::vfxPodiumPlay)
RC_DECOMP_FN_SETUP_HEAP(Vfx3Rain, rc::fuzz::vfxRain)
RC_DECOMP_FN_SETUP_HEAP(VfxSkyBackdropMerge, rc::fuzz::vfxSkyMerge)
RC_DECOMP_FN_SETUP_HEAP(VfxSkyChangeStuff, rc::fuzz::vfxSkyChange)
