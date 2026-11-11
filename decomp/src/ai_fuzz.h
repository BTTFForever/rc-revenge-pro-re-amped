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

} // namespace rc::fuzz
