// Inputs for the random test of MEM.c, MEMFRAG.c, HEAP.c and TASK.c. Each has the signature RC_DECOMP_FN_SETUP
// expects. The allocators run on the dump's real heap (whole RAM: RC_DECOMP_FN_SETUP_HEAP), fragmented first by
// freeing and allocating real blocks, sometimes filled until little or nothing is left, sometimes with a free block's
// size made not a multiple of 64 so the exact-fit tests are reached; handles live in scratch. Fragment pools are made
// on the heap with the native MEMFRAG functions. The task functions work on the dump's task list with tasks from
// scratch added, or on a list of scratch tasks whose callbacks are small game functions.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void memAlloc(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a handle, a1 a size (fits a free block, or not)
void memFree(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a handle to a used block
void memSet(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 scratch, a1 a value, a2 a size
void memCopy(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0, a1 scratch (overlapping or not), a2 a size
void memGrow(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a handle, a1 a size near the next free block's
void memShrink(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a handle, a1 a size up to the block's
void memResize(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // either of the two above
void memInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a pool in scratch, a1 a size
void memCounters(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // no arguments; memVarG's counters varied
void memFreeAll(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments; the real heap, or a small one in scratch
void memFragmented(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // no arguments; the heap fragmented
void fragPool(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a fragment pool, used and partly freed
void fragPoolHandle(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 the handle of such a pool
void fragPoolCreate(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a handle, a1-a3, t0 the pool's shape
void fragDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a pool, a1 one of its fragments (or not)
void fragBlock(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a pool, a1 one of its blocks (or another)
void heapInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a size
void heapTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // no arguments; heapVarsG's block real or new
void heapVars(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a size or pointer; heapVarsG varied
void taskSet(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a scratch task, a1-a3 any values
void taskList(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // no arguments; the task list varied
void taskInsert(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a new task (time near the list's)
void taskDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a task in the list (or not)
void taskProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a time; the real list, or scratch tasks

} // namespace rc::fuzz
