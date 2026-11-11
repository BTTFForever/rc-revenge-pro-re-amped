// Inputs for the random test of the collision code (RC_SHADOW_FUZZ). The collision functions follow pointers from the
// level's grid to its cells, polys and cars, so random bytes would be meaningless: these build a small random level
// in the test's scratch area instead. Each has the signature RC_DECOMP_FN_SETUP expects.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void pointAndPoly(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void rayAndPoly(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void pointOnRayAndPoly(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void downCast(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void downCastInt(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void generalRayCast(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void cellCastCell(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void positionAndTolerance(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void carsInCells(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void objectForCarCol(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void scifiDoor(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void scifiTriggers(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void trackModules(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void trackModulesOnHeap(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void trackExtents(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void trackPolysForGrid(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void unsortedCells(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void trackToBuild(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void cellLinkLists(R5900Context *ctx, uint64_t &state, uint32_t scratch);

} // namespace rc::fuzz
