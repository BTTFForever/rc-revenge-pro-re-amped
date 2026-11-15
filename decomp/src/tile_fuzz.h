// Inputs for the random test of TILE.c. Each has the signature RC_DECOMP_FN_SETUP expects. The tile list and its
// shapes are built in scratch (the dumps' races have none); the shapes draw through the race's own packet rings.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void tileNone(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // no arguments; tileVarG and the view varied
void tileYStart(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a tile id
void tileRender(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0-a3 x, z cells, x, z sizes; t0 a unit, t1 a theme
void tileEditor(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0, a1 cells, a2 a unit, a3 a theme
void tileDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a handle to a tile list of loaded ids

} // namespace rc::fuzz
