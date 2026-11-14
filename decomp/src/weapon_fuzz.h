// Inputs for the random test of the weapons (WEAPON.c). Each has the signature RC_DECOMP_FN_SETUP expects. Most run on
// the dump's real race (whole RAM: RC_DECOMP_FN_SETUP_HEAP): the owner and target are real cars, and a live weapon is
// made by the original WeaponSpawn, as the game makes it, then has its state varied.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void weaponOnly(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a weapon record in scratch
void weaponTwoCars(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0, a1 car copies in scratch
void weaponOneCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a real car, the race varied
void weaponList(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // no arguments; live weapons spawned
void weaponShake(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a weapon (owner, misc)
void weaponBlast(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a weapon; cars under its effect
void weaponInList(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a live weapon, a1 a time
void weaponCreate(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 pos, a1 time, a2 definition
void weaponSpawn(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 pos, a1 definition, a2 time
void weaponRender(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 split; fakes and rocks spawned
void weaponHitCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a live weapon, a1 a real car
void weaponOil(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 an oil slick, a1 a real car on it
void weaponAcol(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a weapon near level polys, a1 out

// a0 a weapon of kind No built as WeaponCreate builds it, before its Setup; a1 a time.
template <int No>
void weaponSetup(R5900Context *ctx, uint64_t &state, uint32_t scratch);
// a0 a live weapon of kind No (spawned by the original), its state varied; a1 a time.
template <int No>
void weaponLive(R5900Context *ctx, uint64_t &state, uint32_t scratch);

} // namespace rc::fuzz
