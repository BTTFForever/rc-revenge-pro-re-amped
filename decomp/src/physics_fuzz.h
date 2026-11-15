// Inputs for the random test of PHYSICS.c, GAMETASK.c, TBOX.c and LEVEL.c. Each has the signature RC_DECOMP_FN_SETUP
// expects. Most run on the dump's real race (whole RAM: RC_DECOMP_FN_SETUP_HEAP), its cars, trigger boxes and level
// varied; the float-only ones work on copies of a real car in scratch.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void physicsRace(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a flag; the race's cars and start varied
void physicsTask(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 = a1 a time; the game task unlinked
void physicsObject(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a real car, its contacts varied
void physicsResponse(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a real car's physics, its contacts varied
void physicsAddCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a real car, a1 its definition (or a copy)
void physicsCarDef(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a real car's physics, a1 its definition
void physicsBoatSwap(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a real car's physics, a1 the car
void physicsForce(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a car copy, a1/a2 float vectors, a3 flags
void physicsIntForce(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a car copy, a1/a2 short vectors, a3 flags
void physicsCarFlag(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a car copy (spheres too), a1 a flag
void tboxRace(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // no arguments; cameras and cars near boxes
void tboxUpdate(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a point near a box, a1 a mask, a2 an object
// a0 a box of the callback's type (real, or made with data for it), a1 a car:
void tboxReverb(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void tboxEnv(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void tboxSfx(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void tboxCam(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void tboxAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void tboxTint(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void tboxTunnel(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void tboxCol(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void levelData(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; pLvlG real or in scratch
void levelInitTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // no arguments

} // namespace rc::fuzz
