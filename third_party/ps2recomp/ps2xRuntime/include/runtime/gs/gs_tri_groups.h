#pragma once
// GOW-Port: groups of consecutive triangles that cover disjoint pixels (GS hardware backend, shadow volumes).
// The shadow-volume passes blend with wrapping arithmetic (COLCLAMP=0) in the shader, which reads the destination:
// a triangle must see the result of every earlier triangle that covers the same pixels, so the backend drew one
// triangle per call with a texture barrier in between (~3000 calls and barriers per frame on the ship, which kept the
// GPU busy). Triangles whose interiors do not overlap can share a call: each pixel is still written once and in order.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gs_tri_groups
{
    struct Tri
    {
        float x[3], y[3];
        float minX, minY, maxX, maxY;
    };

    inline Tri makeTri(const float *v0, const float *v1, const float *v2)
    {
        Tri t{{v0[0], v1[0], v2[0]}, {v0[1], v1[1], v2[1]}, 0, 0, 0, 0};
        t.minX = std::min({t.x[0], t.x[1], t.x[2]});
        t.maxX = std::max({t.x[0], t.x[1], t.x[2]});
        t.minY = std::min({t.y[0], t.y[1], t.y[2]});
        t.maxY = std::max({t.y[0], t.y[1], t.y[2]});
        return t;
    }

    inline bool hasVertex(const Tri &t, float x, float y)
    {
        for (int i = 0; i < 3; ++i)
            if (t.x[i] == x && t.y[i] == y)
                return true;
        return false;
    }

    // True unless an edge of one triangle separates the two. Interiors that only touch along an edge both triangles
    // share exactly (same two vertices: adjacent triangles of a mesh) do not overlap, since the rasteriser gives the
    // pixels of a shared edge to one of them; any other contact counts as overlap.
    inline bool overlap(const Tri &a, const Tri &b)
    {
        if (a.maxX < b.minX || b.maxX < a.minX || a.maxY < b.minY || b.maxY < a.minY)
            return false;
        const Tri *tris[2] = {&a, &b};
        for (int k = 0; k < 2; ++k)
        {
            const Tri &e = *tris[k];
            const Tri &o = *tris[1 - k];
            for (int i = 0; i < 3; ++i)
            {
                const int j = (i + 1) % 3;
                const float nx = e.y[j] - e.y[i], ny = e.x[i] - e.x[j];
                float minA = 1e30f, maxA = -1e30f, minB = 1e30f, maxB = -1e30f;
                for (int v = 0; v < 3; ++v)
                {
                    const float pa = nx * e.x[v] + ny * e.y[v], pb = nx * o.x[v] + ny * o.y[v];
                    minA = std::min(minA, pa), maxA = std::max(maxA, pa);
                    minB = std::min(minB, pb), maxB = std::max(maxB, pb);
                }
                const bool shared = hasVertex(o, e.x[i], e.y[i]) && hasVertex(o, e.x[j], e.y[j]);
                if (shared ? (maxA <= minB || maxB <= minA) : (maxA < minB || maxB < minA))
                    return false;
            }
        }
        return true;
    }

    // ends[g] is one past the last triangle of group g (groups are consecutive and cover [0, triCount)).
    // `xy` points at the first vertex's x; each vertex is `strideFloats` floats, y follows x.
    inline void build(const float *xy, size_t strideFloats, size_t triCount, std::vector<uint32_t> &ends,
                      size_t maxGroup = 64u)
    {
        ends.clear();
        std::vector<Tri> group;
        group.reserve(maxGroup);
        for (size_t t = 0; t < triCount; ++t)
        {
            const float *v = xy + t * 3u * strideFloats;
            const Tri tri = makeTri(v, v + strideFloats, v + 2u * strideFloats);
            bool clash = group.size() >= maxGroup;
            for (size_t g = 0; !clash && g < group.size(); ++g)
                clash = overlap(group[g], tri);
            if (clash)
            {
                ends.push_back(static_cast<uint32_t>(t));
                group.clear();
            }
            group.push_back(tri);
        }
        if (triCount != 0u)
            ends.push_back(static_cast<uint32_t>(triCount));
    }
}
