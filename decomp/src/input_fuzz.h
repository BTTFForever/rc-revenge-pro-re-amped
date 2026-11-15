// Inputs for the random test of INPUT.c. Each has the signature RC_DECOMP_FN_SETUP expects. inputVarG is random (its
// states and counters mostly in range); offline the pad library is a scripted pad whose calls are logged into scratch
// (input_fuzz.cpp).
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void inputKeys(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a key, a1 an interest mask, a2 the values
void inputPad(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 an eInputPad (0-7), a1 a pad mode; the pad scripted
void inputController(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a controller (0 or 1); the pad scripted

} // namespace rc::fuzz
