// Inputs for the random test of the camera code (CAMERA.c). Each has the signature RC_DECOMP_FN_SETUP expects.
// The camera is a copy of one of the dump's two cameras in the scratch area, perturbed, with its target a copy of a
// race car (object, physics, integer state, render object) and its own path nodes and start-grid path.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void cameraOne(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 camera (any mode)
void cameraZoom(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 camera, a1 distance
void cameraDecelerate(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 src, a1 dest, a2 rate, a3-t1 axes, t2 out
void cameraOffsets(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 camera, a1/a2 vectors (null, or aliasing)
void cameraWithMatrix(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 camera, a1 a matrix buffer
void cameraLookAt(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 camera, a1 a point to look at
void cameraAINode(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 camera, a1 route node, a2 height
void cameraMode(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 camera, a1 mode
void cameraLvlInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 camera, a1 split, a2 camera number
void cameraLevel(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 split; main state varied (whole RAM)
void cameraPath(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 camera on a start-grid path, a1 node
void cameraNewPath(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 camera, a1/a2 callbacks, a3 path
void cameraTag(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 path, a1 position, a2 node, a3 camera
void cameraSpline(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 camera, a1 t, a2 out, a3 angles
void cameraCoeffs(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 camera, a1 segment, a2 angles
void cameraPad(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 camera, the pad's keys and stick varied
void cameraProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 time, a1 split; both cameras varied
void cameraCell(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a level cell, a1/a2 segment, a3/t0 out
void cameraRay(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0/a1 segment, a2/a3 out

} // namespace rc::fuzz
