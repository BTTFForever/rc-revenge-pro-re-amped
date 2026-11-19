#version 450
// render/gs2d (renderer plan P1 step 7; RENDERER_2D_DESIGN.md 6.1, 8.1): places a primitive record's vertices, nothing
// else. Window coordinates (1/16 pixel) moved half a pixel, so the GPU's pixel-centre samples are the software GS's
// integer sample points; the viewport is 4096 x 4096 at (-2048, -2048), so ndc = framebuffer / 2048 and back is exact.
// The RHI's clip space is y-up (third_party/ico_rhi/VENDORED.md): y is negated. Built twice: triangles (three
// vertices a record) and, with GS2D_POINTS, one point a record (the line pixels, gl_PointSize 1 at the pixel centre).
// Records: Gs2dPrim (gs2d_vk.h), 44 words, in the storage buffer of bind group 1 slot 0 (binding 16).
layout(std430, set = 1, binding = 16) readonly buffer Prims { uint w[]; } prims;
layout(location = 0) flat out uint vPrim;

void main()
{
#ifdef GS2D_POINTS
    const uint prim = uint(gl_VertexIndex);
    const uint vi = 0u;
#else
    const uint prim = uint(gl_VertexIndex) / 3u;
    const uint vi = uint(gl_VertexIndex) % 3u;
#endif
    const uint base = prim * 44u;
    const int x = int(prims.w[base + vi]);
    const int y = int(prims.w[base + 3u + vi]);
    const bool noHalf = (prims.w[base + 40u] & 1u) != 0u; // planted fault: the half pixel left out
    const float h = noHalf ? 0.0 : 0.5;
    precise vec2 pos = vec2(float(x), float(y)) / 16.0 + h;
    gl_Position = vec4(pos.x / 2048.0, -pos.y / 2048.0, 0.5, 1.0);
#ifdef GS2D_POINTS
    gl_PointSize = 1.0;
#endif
    vPrim = prim;
}
