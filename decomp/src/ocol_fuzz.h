// Inputs for the random test of the object collision code (OCOL.c). The functions follow pointers from the collision
// objects to cars, weapons, pickups and anims, so these build a small valid world in the scratch area. Each has the
// signature RC_DECOMP_FN_SETUP expects.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void ocolCarAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 car, a1 anim
void ocolAnimCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 anim, a1 car
void ocolWeaponAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 weapon, a1 anim
void ocolAnimWeapon(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 anim, a1 weapon
void ocolCarRock(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 car, a1 rock (weapon 15)
void ocolCarCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void ocolCarPickup(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void ocolPickupCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void ocolCarOil(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void ocolOilCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void ocolCarWeapon(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void ocolWeaponCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void ocolObject(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 one object, partly in lists
void ocolInitObject(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 object, a1 pos, a2 rad, a3 data
void ocolList(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // objects in the update list and cells
void ocolReorder(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // cars with contact lists

} // namespace rc::fuzz
