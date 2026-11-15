// Inputs for the random test of the pickups (PICKUP.c). Each has the signature RC_DECOMP_FN_SETUP expects. All run on
// the dump's real race (whole RAM: RC_DECOMP_FN_SETUP_HEAP): its pickup list, cars and level tables, varied; the
// game's random seeds are set from the input.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void pickupNone(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments
void pickupSpawn(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a time; positions real or in scratch
void pickupOne(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a pickup of the list, a1 a time
void pickupRender(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a time; the pickups and camera varied
void pickupUltimate(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a weapon (9 mostly), a1 flags
void pickupSpecial(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 an object type
void pickupGive(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a pickup, a1 a player type, a2 a position
void pickupRace(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments; the list's count varied
void pickupHit(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a car, a1 a live pickup
void pickupLevel(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // no arguments; the level's tables

} // namespace rc::fuzz
