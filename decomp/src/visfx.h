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
void sortPosFromInts(uint32_t fx, int32_t x, int32_t y, int32_t z, uint32_t frameSize);

// The sort position of a car's effect: the car's physics position in whole units.
void sortPosFromCar(uint32_t fx, ptr32<_sObject> pObj, uint32_t frameSize);

// VU0 vadd/vsub on one vector, as sceVu0AddVector and sceVu0SubVector do it (the vector registers are not updated).
void vu0Add(uint32_t dst, uint32_t a, uint32_t b);
void vu0Sub(uint32_t dst, uint32_t a, uint32_t b);

} // namespace vfx
} // namespace rc
