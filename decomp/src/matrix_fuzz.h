// Inputs for the random test of MATRIX.c, MATHS.c and PSXDEFS.c functions that random bytes serve badly. Each has the
// signature RC_DECOMP_FN_SETUP expects.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void noInput(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // nothing to vary (the table builders)
void floatBits(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // f12 any bit pattern, every exponent
void vec16Pair(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0, a1 short vectors of every length scale
void vec32Pair(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a short vector or a long one, a1 a long one
void normalMatrix(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a normal, a1 a matrix, a2 the output
void debugFormat(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a short format, a1-a3 its arguments
void warningText(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a short string

} // namespace rc::fuzz
