#version 430 core
// TRdr_VUShapeProg as a vertex shader. The buffer holds the triangle data exactly as the game sends it to VU1,
// ten quadwords per triangle: normal (w: a winding flag), three positions, three (s, t, q), three integer colours.
//
// Draw 3 * triangles vertices with no attributes. Cutting at the near plane uses gl_ClipDistance[0] and the screen
// edges are left to the GPU's clipper, so the original program's two clipping paths have no counterpart here.
layout(std430, binding = 0) readonly buffer Triangles { vec4 quad[]; };

layout(std140, binding = 1) uniform TrackDraw
{
    vec4 matrix[4];    // VU memory 18..21: model to view.
    vec4 screenOffset; // VU memory 16: GS window offset in x, y.
    vec4 screenScale;  // VU memory 17.
    vec4 lightPos;     // VU memory 15: the viewer's position, for the facing test.
    vec4 clipRange;    // VU memory 12: z holds the near plane.
    vec4 tint;         // VU memory 11: colour * tint.w + tint.rgb.
    vec4 gsToClip;     // Maps GS x, y to NDC: (centre x, centre y, half width, half height).
    float gsDepthRange;
};

layout(location = 0) out vec3 gsPosition; // GS window x, y and depth, in GS units.
layout(location = 1) out vec3 stq;
layout(location = 2) out vec4 colour;     // On the PS2 scale, 128 is full brightness.
layout(location = 3) out float keep;      // 0 if the facing test rejects the triangle.
layout(location = 4) out float viewZ;

void main()
{
    const int triangle = gl_VertexID / 3;
    const int corner = gl_VertexID % 3;
    const int base = triangle * 10;

    const vec4 normal = quad[base];
    const vec4 position = quad[base + 1 + corner];
    const ivec4 rawColour = floatBitsToInt(quad[base + 7 + corner]);

    // The facing test uses the triangle's first corner, before it is transformed.
    const float facing = dot(quad[base + 1].xyz - lightPos.xyz, normal.xyz);
    const int winding = (floatBitsToInt(normal.w) << 16) >> 16;
    keep = winding == (facing < 0.0 ? 128 : 0) ? 0.0 : 1.0;

    vec4 view = matrix[0] * position.x + matrix[1] * position.y + matrix[2] * position.z + matrix[3] * position.w;
    view.z += quad[base + 4].w; // A depth bias stored in the first corner's texture coordinate w.
    viewZ = view.z;

    const float q = 1.0 / view.z;
    gsPosition = vec3(view.xy * q * screenScale.xy + screenOffset.xy, view.w * q * screenScale.z);
    stq = quad[base + 4 + corner].xyz * q;
    colour = vec4(trunc(vec3(rawColour.rgb) * tint.w + tint.rgb), float(rawColour.a));

    gl_ClipDistance[0] = view.z - clipRange.z;
    if (keep == 0.0)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // Outside the clip volume: the whole triangle is discarded.
        return;
    }
    gl_Position = vec4((gsPosition.xy - gsToClip.xy) / gsToClip.zw * view.z, gsPosition.z / gsDepthRange * view.z, view.z);
}
