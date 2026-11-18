#pragma once
// GOW-Port: ps2recomp-gs-sombras-tramos. Tramos consecutivos de triángulos con el mismo color (GS por hardware, volúmenes
// de sombra). Las dos pasadas de recuento de las sombras (G += 255 y G += 1) tienen el mismo estado del GS y solo cambian
// el color plano de sus vértices, así que comparten lote; el atajo de mezcla fija necesita un solo color por llamada.
// Partir el lote en tramos, en orden, conserva el orden de las operaciones sobre cada píxel.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gs_colour_runs
{
    // `verts`: vertexCount vértices de `stride` floats con el color (r, g, b) a partir del float `colourOffset`.
    // Deja en `ends` el vértice final (exclusivo) de cada tramo; los tramos empiezan siempre en un triángulo. Con un solo
    // color, un único tramo que lo abarca todo.
    inline void build(const float *verts, size_t stride, size_t colourOffset, size_t vertexCount, std::vector<uint32_t> &ends)
    {
        ends.clear();
        if (vertexCount == 0u)
            return;
        const auto colour = [&](size_t i) { return verts + i * stride + colourOffset; };
        const auto same = [&](size_t a, size_t b)
        {
            const float *ca = colour(a), *cb = colour(b);
            return ca[0] == cb[0] && ca[1] == cb[1] && ca[2] == cb[2];
        };
        size_t runStart = 0;
        for (size_t i = 3; i + 3u <= vertexCount; i += 3u)
        {
            if (!same(i, runStart))
            {
                ends.push_back(static_cast<uint32_t>(i));
                runStart = i;
            }
        }
        ends.push_back(static_cast<uint32_t>(vertexCount));
    }
}
