// Inputs for the random test of RCMUSIC.c, MUSIC.c, MPEG.c, MPDISPLAY.c, PCFILE.c and FM.c. Each has the signature
// RC_DECOMP_FN_SETUP expects.
//
// Offline the layers beneath are replaced (quietStream in the .cpp), each fake logging its call and a hash of what it
// was sent into scratch and answering from a script the setup seeds:
// - the IOP music module: sceSifCallRpc (the command, the words sent, the stack argument; it writes the reply into
//   the receive buffer), sceSifBindRpc (fails, or binds after some calls), sceSifInitRpc;
// - the disc: sceCdSearchFile (finds after some failures, or never; the position and size come from the name),
//   sceCdRead (refused now and then; when accepted it writes a pattern made from the sector numbers), sceCdSync (busy
//   now and then), sceCdSeek, sceCdDiskReady;
// - the host files: sceOpen, sceRead (writes a pattern, answers short now and then), sceWrite (hashes what it is
//   given), sceLseek, sceClose;
// - the movie decoder: sceMpegInit, Create, Delete, AddBs, Reset, IsEnd (true after some calls), GetPicture (fails
//   now and then; sets the picture size and frame number in the decoder struct);
// - the GS and interrupts: sceGsSyncV (alternating answers at random), sceGsSyncPath (busy now and then),
//   sceGsPutDispEnv (hashes the display settings), AddIntcHandler (answers an id), RemoveIntcHandler; printf and free
//   are logged; stores to hardware registers (loadImage's DMA kicks) are logged and dropped.
// EnableIntc and DisableIntc stay real: originals reach EnableIntc by a tail jump, which calls the recompiled target
// directly, past any replacement.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

// FM
void fmPrepareEx(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a buffer, a1 a size
void fmCacheMode(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a mode; the chunk size
void fmUnPrepare(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // no arguments; a heap block as the cache (whole RAM)
void fmFileOpen(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a file name; slots, directory cache
void fmFileRead(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a buffer, a1 a size, a2 a slot; the cache in scratch
void fmState(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // no arguments; fmVarG varied
void fmPrepare(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a size (whole RAM)
void fmFileArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a slot, a1 an offset, a2 a seek mode
void fmFetch(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a sector, a1 a buffer, a2 a count
// PCFILE
void pcCreat(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a name, a1 anything
void pcLseek(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a handle, a1 an offset, a2 a mode, a3 anything
void pcReadWrite(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a handle, a1 a buffer (any alignment), a2 a length
void pcClose(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a handle
// MPEG, MPDISPLAY
void mpegFile(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a movie name (whole RAM: read through FM)
void mpegReadFile(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a name, a1 a handle, a2 a size word, a3 high or low
void mpegLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a name, a1 a buffer index, a2 high, a3 clear
void mpegPlay(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a buffer index, a1 looping
void mpegTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a buffer index; the buffers heap blocks (whole RAM)
void mpegClear(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a colour
void mpegPlayMpeg(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 the decoder, a1 a stream, a2 a size
void mpegFrame(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a vblank count; the player state
void mpegState(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // no arguments or a0 anything; the player state
void mpegSplashTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch); // no arguments; the splash buffer a heap block
void mpegDisplay(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 0 or 1 (or anything); the counters
void mpegSync(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a frame that does not hang syncDisplay
// RCMUSIC, MUSIC
void rcLists(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // no arguments; rcmusicVarG varied
void rcLevel(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a file count
void rcMusic(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0-a2 small numbers; rcmusicVarG varied
void rcChain(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a track or 100-104, a1 a non-zero rate
void rcVolume(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a non-zero fade, a1 a flag
void rcInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a number of files
void musicArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0-a2 any numbers; the IOP's answers
void musicRpc(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a command, a1 data (a pointer for 0x802x)

} // namespace rc::fuzz
