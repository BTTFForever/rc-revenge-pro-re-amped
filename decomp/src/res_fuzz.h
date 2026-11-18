// Inputs for the random test of RES.c, LOADING.c, SYSTIMER.c, TIMER.c and BACKDROP.c. Each has the signature
// RC_DECOMP_FN_SETUP expects.
//
// Offline the disc and the hardware are replaced (quietLoader in the .cpp): FMFileOpen and FMFileRead serve the real
// bytes of the disc image (game/<version>/RC Revenge Pro (<version>).iso, or RC_ISO) through the game's own file table, so the original
// and the native function read the same data, and a scratch word can make chosen reads fail (short, corrupt or
// empty, the position kept) to drive the retry and error paths. The interrupt handler calls, the IOP (sceSifCallRpc
// under sceSdRemote, the IOP heap, transfers), the MPEG player, the GS uploads and waits, the buffer swap, the pad
// snapshot (scripted) and the read error function become calls logged into scratch with their arguments (and a hash of
// all of them), and every store to a hardware register is logged and dropped. Timer 0 is stopped at a count the setup
// chooses. Functions the originals reach by a tail jump (EnableIntc, DisableIntc, sceSdRemote, VSyncCallback) are not
// replaced: recompiled code calls a tail jump's target directly, past the replacement.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void resInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);             // a0 "\RC2" or a variant, a1-a2 a buffer (whole RAM)
void resHandles(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // no arguments; the handle table allocated or not
void resTemporaryHandles(R5900Context *ctx, uint64_t &state, uint32_t scratch); // no arguments; the lookup file and the CAT
void resTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);             // no arguments; type list blocks and files
void resTypeList(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // no arguments; the type list file open
void resOpen(R5900Context *ctx, uint64_t &state, uint32_t scratch);             // no arguments; path and name, file slots
void resLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch);             // a0 a resource id; handles and daemons
void resLoadBlock(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 an id, a1 a handle
void resLoadImage(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 an id, a1 the area's end
void resFreeBlock(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a handle to a heap block
void resCrc(R5900Context *ctx, uint64_t &state, uint32_t scratch);              // a0 a CRC, a1 a buffer or null, a2 a length
void resChecksum(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a sum, a1 words, a2 a length
void resArea(R5900Context *ctx, uint64_t &state, uint32_t scratch);             // a0 a size; the area's pointers and header
void resGetHandle(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 an id; a handle table in scratch
void resClose(R5900Context *ctx, uint64_t &state, uint32_t scratch);            // no arguments; the three files
void resNames(R5900Context *ctx, uint64_t &state, uint32_t scratch);            // no arguments; a base name, extensions
void resCompType(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0, a1 pointers to type names
void resSTB(R5900Context *ctx, uint64_t &state, uint32_t scratch);              // a0 a handle to an id list
void resInitLoading(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // no arguments; the lookup file open
void resRelocate(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a resource as read (disc or made up)
void resAutoLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a resource with swizzles, a1 its id
void loadingInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a loading type; the movie state
void loadingFlags(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; bLoadingG, the logo counter
void sysTimer(R5900Context *ctx, uint64_t &state, uint32_t scratch);            // no arguments; the clock and the counter
void sysTimerAdd(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a timer, a1 a callback, a2 a frequency
void sysTimerRemove(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a timer or null (whole RAM)
void sysTimerHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a cause, a1 a word, a2 an EPC; a list
void timerArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch);           // a0 a timer, a1 a time, a2 flags
void backdropScreens(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments; a pad script (whole RAM)
void backdropBmp(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a screen's file name
void backdropVag(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a sound's file name; dwAddr
void backdropDisplay(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a BMP anywhere in RAM, a1 a buffer
void bbLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch);             // a0 a level, a1 flags; failed header reads (whole RAM)
void bbTransfer(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 the kind, a1-a2 a size and place, a3 a header, t0 the data
void backdropPlay(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; the voice addresses

// For other units' setups: the loader ready to load (see loadEnvironment in the .cpp; false without a disc) and the id
// of a small resource from the disc (now and then marked loaded already).
bool resLoadEnvironment(uint64_t &state, uint32_t scratch);
uint32_t resPickId(uint64_t &state);

} // namespace rc::fuzz
