#pragma once

// The covered-pixels rule for the exact blend's barriers and the split's groups (renderer plan P1 step 8 batch 2;
// RENDERER_3D_DESIGN.md 10.2). A map of the 640 x 256 target with two 16-bit stamps a pixel, `read` and `split`, each
// compared with a current epoch: a pixel "carries" a plane when its stamp equals that plane's epoch. Advancing an epoch
// forgets every stamp of the plane at once (the map is cleared only when the 16-bit epoch wraps).
//
// Coverage is the reference's (gs_model's gsTriSetup / gsTriCovers: the edge functions with the top-left bias, the
// functions T5 proved equal to the device's coverage), computed per row as one span (a triangle is convex) from the
// edge functions' linear inequalities, inside the scissored box; a point is its pixel, a flat rectangle its box.
//
// Gs2dVk uses it per pass (one target between two passes' boundaries) in draw-list order:
//   - a reading primitive needs a barrier before it when any pixel it covers carries `read` (written since the last
//     barrier), or when it is the pass's first reading primitive and anything was drawn in the pass before it;
//     the barrier advances `read`;
//   - once a reading primitive has been seen in the pass, every primitive stamps its pixels with `read`;
//   - a split plan's group (pass parts, then fail parts) ends before a primitive whose pixels carry `split` (stamped by
//     the group so far); the group's end advances `split`.
// No dependencies but gs_model.h, C++17.

#include "gs_model.h"

#include <cstdint>
#include <vector>

namespace rc::rd
{
    struct GsSpan
    {
        int16_t y, x0, x1; // x0..x1 inclusive
    };

    class GsCoverMap
    {
    public:
        static constexpr int kWidth = 640, kHeight = 256;
        enum Plane : int
        {
            kRead = 0,
            kSplit = 1,
        };

        GsCoverMap();

        // The pixels a triangle covers inside the box x0..x1 x y0..y1 (already clipped to the scissor and the target),
        // appended to out as spans. Returns the number of pixels.
        static uint64_t triangle(const GsTri &t, int x0, int y0, int x1, int y1, std::vector<GsSpan> &out);
        // A rectangle (the clear, a point), appended.
        static uint64_t rect(int x0, int y0, int x1, int y1, std::vector<GsSpan> &out);

        // A new pass: both planes forget everything.
        void beginPass();
        // Any pixel of the spans carries the plane's current epoch.
        bool any(const GsSpan *spans, size_t n, Plane p) const;
        // Stamp the spans with the plane's current epoch.
        void stamp(const GsSpan *spans, size_t n, Plane p);
        // Forget the plane's stamps (a barrier for `read`, a group's end for `split`).
        void advance(Plane p);

        uint64_t pixelsVisited() const { return m_visited; }

    private:
        std::vector<uint16_t> m_stamp[2];
        uint16_t m_epoch[2] = {1, 1};
        mutable uint64_t m_visited = 0;
    };
}
