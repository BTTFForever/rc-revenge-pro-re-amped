#pragma once
// A batch of track triangles drawn by a vertex shader instead of VU1 (decomp/vu1/track_draw.cpp).
//
// It travels through the GS command stream as a GS register write to an unused address, carrying a pointer, so it
// stays in order with everything else. The backend frees it after drawing it.
#include <cstddef>
#include <cstdint>

constexpr uint8_t kGSTrackBatchRegister = 0x7Eu;
constexpr uint8_t kGSTrackBatchMarker = 0xEEu; // In GSPrimitiveBatch::vertexCount; the pointer is in vertices[0].z.

struct GSTrackBatch
{
    uint32_t triangles;
    uint32_t quadBytes; // Size of quads, if not ten quadwords per triangle. Such a batch is never merged with another.
    float draw[64]; // The shader's TrackDraw block; the backend fills in gsToClip and gsDepthRange.
    float quads[1]; // triangles * 40 floats: ten quadwords each, as the game sends them to VU1.
};

// GLSL for the track vertex shader, set before the backend first builds its program.
extern const char *g_gsTrackVertexShader;
