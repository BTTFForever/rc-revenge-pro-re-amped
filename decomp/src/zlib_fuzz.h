// Inputs for the random test of the game's zlib (ADLER32.c, ZUTIL.c, UNCOMPR.c, INFLATE.c, INFBLOCK.c, INFCODES.c, INFFAST.c,
// INFTREES.c, INFUTIL.c). See zlib_fuzz.cpp.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void zAdler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zErrorArg(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zAlloc(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zFree(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zUncompress(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zInit2(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zEnd(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zReset(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zSync(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zSetDict(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zSyncPoint(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zInflate(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zBlocksNew(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zBlocks(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zBlocksReset(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zBlocksFree(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zBlocksSetDictionary(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zBlocksSyncPoint(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zCodes(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zCodesNew(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zCodesFree(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zFast(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zHuftBuild(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zTreesBits(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zTreesDynamic(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zTreesFixed(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void zFlush(R5900Context *ctx, uint64_t &state, uint32_t scratch);

} // namespace rc::fuzz
