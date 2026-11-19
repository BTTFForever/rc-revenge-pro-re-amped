#version 450
// render/blit (renderer plan P1 step 6): one triangle covering the target, no vertex buffer (as platform/present.vert).
void main()
{
    const vec2 pos = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
