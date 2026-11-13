// Inputs for the random test of the route node code (RC_SHADOW_FUZZ). Each has the signature RC_DECOMP_FN_SETUP
// expects.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void aiCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 car, a1 distance, a2 count
void aiCarAndFork(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 car, a1 node with a fork
void aiMatrixAndCar(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 result buffer, a1 car
void aiPoint(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 point near a node
void aiSearch(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // search state set; a0 node, a1 count, a2 direction
// visfx_fuzz.cpp, with the car builder they share:
void aiFinishDistance(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 car with its lap state, race globals
void aiNodeRender(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments: the dump's route and cars
void playerCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a human player's car, a1 time, a2 owner
void playerInput(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 player 0 or 1, pad state set
void physicsCarCopy(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a physics car copied with its spheres and data
void physicsCars(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // the physics car list: copies of the race's cars
void carColObjects(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // the object list: copies of the race's cars
void colDrawPoly(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a collision poly, a1-a3 a colour

} // namespace rc::fuzz
