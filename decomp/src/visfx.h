// Helpers shared by visfx.cpp (VISFX) and visfx2.cpp (VISFX2, VISFX3): reading effect objects by offset, the sort
// position helpers, and the functions the effects call that live in other files.
#pragma once

#include "ee_math.h"

namespace rc {

// The game's newlib rand (ai.cpp), the polygon builder (rdr.cpp) and the heap (mem.cpp).
int32 rand();
void RdrPoly(int32_t channel, ptr32<_sRdrPoly> poly);
void RdrPolyTexture(int32_t channel, ptr32<_sGfxTexture> pTexture, u32 style);
void MemBlockFree(ptr32<memHandle> pHandle);
void sceVu0ScaleVector(ptr32<ee_float> dst, ptr32<ee_float> src, ee_float scale);          // vifpk.cpp
void sceVu0ApplyMatrix(ptr32<ee_float> dst, ptr32<ee_float> m, ptr32<ee_float> v);      // vifpk.cpp

// Defined in visfx.cpp.
void VfxPS2SpriteRotated3D(ee_float zOffset);
void VfxParticleMover(ptr32<_sVfx_PartHeader> pHeader, ptr32<_VFX_PARTICLE> Particles);
void VfxSimpleParticleMover(ptr32<_sVfx_PartHeader> pHeader, ptr32<_VFX_SIMPLEPARTICLE> Particles);
uint16 VfxHasCarBeenRendered(ptr32<_sObject> pObj);
ptr32<ee_float> VfxVUCarPositionPoi(ptr32<_sObject> pObj);
void VfxPutGT4rgba0(uint32 v1);
void VfxPutGT4rgba1(uint32 v1);
void VfxPutGT4rgba2(uint32 v1);
void VfxPutGT4rgba3(uint32 v1);
void VfxPutGT4FacingPoly(ptr32<SVECTOR> svA, ptr32<SVECTOR> svB, ptr32<_sGfxTexture> texture, int32 style, ee_float width, ee_float fZOffset);
void VfxPutGT4FacingPolyVU(ptr32<ee_float> vA, ptr32<ee_float> vB, ptr32<_sGfxTexture> texture, int32 style, ee_float width, ee_float fZOffset);
void VfxPutGT4FacingContinueVU(ptr32<ee_float> vA, ptr32<ee_float> vB);
ptr32<ee_float> VfxPSXToVUMatrix(ptr32<MATRIX> pMat);
uint64_t VfxCarAerialVal(ptr32<_sObject> pObj, int32 n);
uint64_t VfxCarPositionVal(ptr32<_sObject> pObj);
ptr32<_sVfx_Object> VfxAddNewPlace(int32 iX, int32 iY, int32 iZ, int32 iType);
bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);                                  // mem.cpp
void sceVu0CopyVector(ptr32<ee_float> dst, ptr32<ee_float> src);                                // vifpk.cpp

namespace vfx {

template <typename T>
T &at(uint32_t address)
{
    return *static_cast<T *>(guest_host(address));
}

constexpr uint32_t kSortPos = 0x20; // vuSortPos: the position the effect is depth-sorted by

// sceVu0ITOF0Vector as the effects call it: converts the three integers the caller put at the start of a 16-byte local
// to floats and stores all four lanes of the destination (the effect's sort position, at fx + 0x20). The fourth integer
// is a word of the caller's stack frame it never wrote, so it holds whatever an earlier call left there; frameSize is
// the original's frame size (for a function that calls another, the two frames together) and the local is at its sp+0.
// localOffset is where in that frame the caller's integer vector (iv) sits: the stale word is its fourth lane.
void sortPosFromInts(uint32_t fx, int32_t x, int32_t y, int32_t z, uint32_t frameSize, uint32_t localOffset = 0);

// The sort position of a car's effect: the car's physics position in whole units.
void sortPosFromCar(uint32_t fx, ptr32<_sObject> pObj, uint32_t frameSize, uint32_t localOffset = 0);

// VU0 vadd/vsub on one vector, as sceVu0AddVector and sceVu0SubVector do it (the vector registers are not updated).
void vu0Add(uint32_t dst, uint32_t a, uint32_t b);
void vu0Sub(uint32_t dst, uint32_t a, uint32_t b);

// The particle ring most effects keep from +0x40: four bytes (mask, count, head, tail), then the particles. The head and
// tail are indexes into the particle array; the original loads the head with lb and compares it with the tail as a
// byte, which only matters for a ring of 128 or more.
inline constexpr uint32_t kRingHeader = 0x40;

// Adds one to the ring's count and moves its tail on.
inline void ringPush(uint32_t mem)
{
    uint8_t &tail = at<uint8_t>(mem + kRingHeader + 3);
    tail = static_cast<uint8_t>(at<uint8_t>(mem + kRingHeader) & (tail + 1));
    at<uint8_t>(mem + kRingHeader + 1) = static_cast<uint8_t>(at<uint8_t>(mem + kRingHeader + 1) + 1);
}

// The next index after one, as a signed byte as the original loads it.
inline int32_t ringNext(uint32_t mem, int32_t index)
{
    return static_cast<int8_t>(static_cast<uint8_t>(at<uint8_t>(mem + kRingHeader) & static_cast<uint8_t>(index + 1)));
}

inline int32_t ringHead(uint32_t mem) { return static_cast<int8_t>(at<uint8_t>(mem + kRingHeader + 2)); }
inline bool ringAtTail(uint32_t mem, int32_t index) { return static_cast<uint32_t>(index) == at<uint8_t>(mem + kRingHeader + 3); }

// The common sprite (visFxVarG.sprite) the effects fill in before VfxPS2SpriteRotated3D draws it.
namespace spr
{
constexpr uint32_t kScale = 0, kPos = 0x10, kTexture = 0x20, kCode = 0x24, kFlipX = 0x25, kAngle = 0x26, kRgb = 0x28;
}
inline uint32_t sprite() { return G(visFxVarG).sprite.address; }
// The effects' textures: word `index` of the level's effects resource.
inline uint32_t vfxTexture(uint32_t index) { return at<uint32_t>(G(visFxVarG).pVfxRes.address + 4 * index); }
// rand() >> 15, the bits the effects use of the game's random numbers.
inline int32_t random15() { return rand() >> 15; }

// The scenery object instance's position, in whole units, becomes the sort position.
inline void sortAtScenery(uint32_t mem, uint32_t frameSize, uint32_t localOffset = 0)
{
    const uint32_t tob = at<uint32_t>(mem + 0x10);
    sortPosFromInts(mem, at<int32_t>(tob + 4) >> 8, at<int32_t>(tob + 8) >> 8, at<int32_t>(tob + 0xc) >> 8, frameSize, localOffset);
}

// A lightning bolt of sixteen camera-facing segments from `from` to `to` (visfx9.cpp; diff, offset, shake and last are
// the caller's frame locals the original uses).
void lightningBolt(uint32_t from, uint32_t to, uint32_t colour, uint32_t texture, ee_float width, ee_float zOffset, uint32_t diff, uint32_t offset,
                   uint32_t shake, uint32_t last);

// Frees every block of an effect list that starts at `head` (VfxLvlTerm and Vfx2TSFXLvlTerm).
void freeList(uint32_t head);

} // namespace vfx
} // namespace rc
