#version 450
// render/gs2d: one triangle covering the target (the seed of the colour target and the depth texture).
void main()
{
    const vec2 pos = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
