// Inputs for the random test of DISPLAY.c, CELL.c, VIEW3D.c, INIT3D.c, VERTANIM.c, SKINANIM.c, SVF.c, RDR.c and
// LDIMAGE.c. Each has the signature RC_DECOMP_FN_SETUP expects. Structures are built in scratch: windows whose
// drawing areas cross every edge of the display, view matrices (the view layer's pointers sometimes moved to overlapping
// scratch copies), shapes and skins with texture fix-ups and tag chains, animations, SVF tables, cameras and lines.
// Offline, waits and hardware are replaced (quietHardware in the .cpp): the GS and DMA syncs, the buffer swap,
// FlushCache, texture uploads and GfxP2_Init become calls logged into scratch with their arguments, and every store the
// runtime sends to a hardware register is logged there too and dropped, so both runs' hardware writes are compared.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void displayMode(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments; displayVarG scrambled; GfxP2_Init logged
void displaySwap(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments; viewBuffer 0, 1 or random
void displayWindow(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a window, 12 coordinates (5 on the stack)
void displayRegister(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a window with random fields
void cellTrans(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a vector; the Y-negated view matrix real or scratch
void viewVars(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; view3dVarG, currentD and the window varied
void viewcone(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a D value, new or the current one
void viewFrame(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a window (valid or not), a1 a view matrix, a2-a3
void init3d(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0-t2 Init3d's sizes and depths (whole RAM)
void init3dClean(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments (whole RAM)
void resDaemons(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // no arguments; the loader's daemon table scrambled
void vuShape(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a handle to a shape with fix-ups (or null)
void vaInstance(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a handle to a vertex animation instance
void vuSkin(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a handle to a skin with fix-ups (whole RAM)
void skinRef(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a skinned instance, posed (whole RAM)
void skinInstance(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a handle to a new skinned instance (whole RAM)
void mxCtrl(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 an animation control, a1 an animation, f12 a rate
void mxOutput(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 an output, a1 a control, a3 a position, t0 a matrix
void svfNormal(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a normal; an SVF table; $s0 a slice
void svfTable(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a handle to an SVF table
void rdrInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // no arguments; the renderer's camera scrambled (whole RAM)
void rdrCamera(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a camera, a1 an angle; the screen width varied
void rdrLine(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0, a1 points, a2 a colour; a camera and a packet
void rdrEndFrame(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments; tag chain and packet in scratch
void hardwareArg(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a tag address
void imageTags(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 tags, a1 an image, a2-t1 a rectangle
void imageTagsYX(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // as imageTags with a2 a skip
void packerArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a quadword, a1-t3 random 64-bit values

} // namespace rc::fuzz
