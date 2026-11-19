#version 450
// RC_RENDERER=vk (platform/vk_host.cpp): one triangle covering the viewport, no vertex buffer. The RHI's clip space
// is y-up (third_party/ico_rhi/VENDORED.md), so y = 1 is the top of the picture, which is v = 0 (the first row).
layout(location = 0) out vec2 outUv;

void main()
{
    const vec2 pos = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);
    outUv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
    gl_Position = vec4(pos, 0.0, 1.0);
}
