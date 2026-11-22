#pragma once
// A batch of track triangles drawn by a vertex shader instead of VU1 (decomp/vu1/track_draw.cpp).
//
// It travels through the GS command stream as a GS register write to an unused address, carrying a pointer, so it
// stays in order with everything else. The backend frees it after drawing it; any other consumer that drops the
// marker (the software GS, the compute GS, PS2X_GS_DISCARD_DRAWS, no backend) frees it with gsFreeTrackBatch.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

constexpr uint8_t kGSTrackBatchRegister = 0x7Eu;
constexpr uint8_t kGSTrackBatchMarker = 0xEEu; // In GSPrimitiveBatch::vertexCount; the pointer is in vertices[0].z.

struct GSTrackBatch
{
    uint32_t triangles;
    uint32_t quadBytes; // Size of quads, if not ten quadwords per triangle. Such a batch is never merged with another.
    float draw[64]; // The shader's TrackDraw block; the backend fills in gsToClip and gsDepthRange.
    float quads[1]; // triangles * 40 floats: ten quadwords each, as the game sends them to VU1.
};

// Skin batches (draw[41] == 3, quadBytes 0x4000: all of VU memory): draw[kGSTrackSkinRun] holds the VU1 run's number
// (uint32 bits) and draw[kGSTrackSkinShared] is 1.0 when the batch carries no memory of its own: it reads what the
// run's first batch carried. A consumer that draws skin keeps that copy.
constexpr int kGSTrackSkinRun = 61;
constexpr int kGSTrackSkinShared = 62;

// If `batch` (a GSPrimitiveBatch) is a track-batch marker, frees the batch it carries and returns true.
template <class PrimitiveBatch>
inline bool gsFreeTrackBatch(const PrimitiveBatch &batch)
{
    if (batch.vertexCount != kGSTrackBatchMarker)
        return false;
    GSTrackBatch *track = nullptr;
    std::memcpy(&track, &batch.vertices[0].z, sizeof(track));
    std::free(track);
    return true;
}

// GLSL for the track vertex shader, set before the backend first builds its program.
extern const char *g_gsTrackVertexShader;
