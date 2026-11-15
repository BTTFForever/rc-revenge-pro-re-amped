// Inputs for the random test of the cars (OBJECT.c). Each has the signature RC_DECOMP_FN_SETUP expects. Most run on
// the dump's real race (whole RAM: RC_DECOMP_FN_SETUP_HEAP): the cars are the race's, their state varied; objects in
// scratch are copies of a real car whose parts (render object, physics, wheels, sounds) are repointed into scratch.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void objectNone(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments
void objectDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a handle to a definition of loaded ids
void objectDefOnly(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a definition in scratch
void objectScratchDef(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a car copy, a1 a definition, a2 a time
void objectInitObject(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // as objectScratchDef, the race varied
void objectCreate(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a real definition, a1 a time, a2 a type
void objectDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a real car or a new one
void objectRace(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; the cars varied
void objectSfxAll(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 flags, a1 a time; the cars' sounds varied
void objectWeapons(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a real car; weapons held and suffered
void objectWeaponList(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a real car; a weapon list in scratch
void objectChangeOver(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a real car with a boat, a1 0-3
void objectCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a real car, a1 flags, a2 a time
void objectScrape(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a real car whose spheres touch things
void objectOvertaking(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a car that may have passed one, a2 a time
void objectAffect(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a car under a weapon's effect, a1 a time
void objectFrontEnd(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 the carousel's choice; cars on it
void objectCreateCars(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a time; the car list and starts

} // namespace rc::fuzz
