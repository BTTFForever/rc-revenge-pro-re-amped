// TRdr_VUShapeProg, EnvRdr_VUShapeProg and Rdr_VUSkinProg as a vertex shader (shapeMode selects which). The buffer holds the triangle data exactly as the game sends it to VU1,
// ten quadwords per triangle: normal (w: a winding flag), three positions, three (s, t, q), three integer colours.
//
// Draw 3 * triangles vertices with no attributes. Cutting at the near plane uses gl_ClipDistance[0] and the screen
// edges are left to the GPU's clipper, so the original program's two clipping paths have no counterpart here.
//
// This file is compiled into the game's GS backend and, with TRDR_STANDALONE defined, into tools/shader_check.cpp.
layout(std430, binding = 0) readonly buffer Triangles { vec4 quad[]; };

layout(std140, binding = 1) uniform TrackDraw
{
    vec4 matrix[4];    // VU memory 18..21: model to view.
    vec4 screenOffset; // VU memory 16: GS window offset in x, y.
    vec4 screenScale;  // VU memory 17.
    vec4 lightPos;     // VU memory 15: the viewer's position, for the facing test.
    vec4 clipRange;    // VU memory 12: z holds the near plane.
    vec4 tint;         // VU memory 11: colour * tint.w + tint.rgb.
    vec4 gsToClip;     // Set by the backend: target offset x, y, sampling offset, and 1 if depth is inverted.
    float gsDepthRange;
    int shapeMode;     // 0: track, colours are integers. 1: EnvRdr, colours are floats. 2: EnvRdr with its environment map on. 3: skin.
                       // 4, 5, 6: water's three passes (Rdr_VUWaterProg, water_shape.cpp).
    vec4 envRows[3];   // VU memory 7..9. Skin: [0] is VU memory 12, subtracted from each blended position. Water: [0] the ripple
                       // centre (VU memory 11), [1] the ripple amounts (VU memory 9).
    vec4 envSettings;  // VU memory 10: w is the reflection strength. Water: the colour of passes 2 and 3 (integers).
    vec4 envBlock;     // The first quadword of this batch's texture block: w scales the reflection strength. Skin: x is the first triangle's quadword address, and the buffer is all of VU memory.
};

struct TrdrVertex
{
    vec3 gs;      // GS window x, y and depth, in GS units.
    vec3 stq;     // What the original sends: s, t, q divided by depth.
    vec3 st;      // The stored texture coordinates, for perspective-correct GPU interpolation.
    vec4 colour;  // On the PS2 scale, 128 is full brightness.
    float keep;   // 0 if the facing test rejects the triangle.
    float viewZ;
};

// A skinned corner: its position through one bone matrix, and a second position through another, blended.
vec4 skinnedPosition(int weights, int corner, vec4 position)
{
    const vec4 blend = quad[weights + corner * 2];
    const vec4 second = quad[weights + corner * 2 + 1];
    const int matrixA = floatBitsToInt(blend.z) & 0x3FF;
    const int matrixB = floatBitsToInt(blend.w) & 0x3FF;
    const vec4 first = quad[matrixA] * position.x + quad[matrixA + 1] * position.y + quad[matrixA + 2] * position.z + quad[matrixA + 3] * position.w;
    const vec3 other = (quad[matrixB] * second.x + quad[matrixB + 1] * second.y + quad[matrixB + 2] * second.z + quad[matrixB + 3] * second.w).xyz;
    return vec4(first.xyz * blend.x + other * blend.y, first.w) - envRows[0];
}

TrdrVertex trdrVertex(int index)
{
    const int triangle = index / 3;
    const int corner = index % 3;
    int base = shapeMode == 3 ? int(envBlock.x) + triangle * 16 : triangle * 10;
#ifdef TRDR_TRIANGLE_SLOTS
    // The GS backend's multi-texture batches join a skin run's blocks: each triangle's first quadword is in its table.
    if (shapeMode == 3 && uMulti != 0)
        base = int(triangleSlot[triangle] >> 16);
#endif

    const vec4 normal = quad[base];
    vec4 position = quad[base + 1 + corner];
    if (shapeMode == 3)
        position = skinnedPosition(base + 10, corner, position);
    const vec4 stored = quad[base + 7 + corner];
    const bool water = shapeMode >= 4;
    ivec4 rawColour = (shapeMode == 0 || shapeMode == 3 || shapeMode == 4) ? floatBitsToInt(stored)
                      : water ? floatBitsToInt(envSettings) : ivec4(trunc(stored));

    TrdrVertex o;
    // The facing test uses the triangle's first corner, before it is transformed.
    const float facing = dot(quad[base + 1].xyz - lightPos.xyz, normal.xyz);
    const int winding = (floatBitsToInt(normal.w) << 16) >> 16;
    if (water)
        o.keep = facing < 0.0 ? 1.0 : 0.0; // No winding flag: drawn only when it faces the viewer.
    else
        o.keep = shapeMode == 3 || winding != (facing < 0.0 ? 128 : 0) ? 1.0 : 0.0; // Skinned triangles have no facing test.

    vec4 view = matrix[0] * position.x + matrix[1] * position.y + matrix[2] * position.z + matrix[3] * position.w;
    if (shapeMode != 3 && !water)
        view.z += quad[base + 4].w; // A depth bias stored in the first corner's texture coordinate w.
    o.viewZ = view.z;

    const float q = 1.0 / view.z;
    o.gs = vec3(view.xy * q * screenScale.xy + screenOffset.xy, view.w * q * screenScale.z);
    o.st = quad[base + 4 + corner].xyz;
    if (shapeMode == 2)
    {
        // The fraction of each colour lane holds a surface normal. These are the original's own per-component products,
        // not a textbook reflection.
        const vec3 surface = (stored.xyz - trunc(stored.xyz) - 0.5) * 2.0;
        vec3 eye = view.xyz - lightPos.xyz;
        const float eyeLength = length(eye);
        eye *= eyeLength != 0.0 ? 1.0 / eyeLength : 0.0;
        vec3 reflected = (surface * surface * eye * 2.0 - eye) * (envRows[0].xyz + envRows[1].xyz + envRows[2].xyz);
        reflected.z += 1.0;
        const float reflectedLength = length(reflected);
        o.st = vec3(reflected.xy * (reflectedLength != 0.0 ? 1.0 / reflectedLength : 0.0) * 0.5 + 0.5, 1.0);
        rawColour = ivec4(ivec3(vec3(min(127.0 * envSettings.w * envBlock.w, 127.0))), 128);
    }
    if (shapeMode == 6)
    {
        // Water's third pass: the texture coordinates pushed out from a point on the surface, by an amount that doesn't
        // depend on the distance (the corner's stored position, in x and z).
        const vec2 away = vec2(envRows[0].x - position.x, envRows[0].z - position.z);
        const float awayLength = length(away);
        o.st.xy += away * (awayLength != 0.0 ? 1.0 / awayLength : 0.0) * envRows[1].xy;
    }
    o.stq = o.st * q;
    o.colour = vec4(trunc(vec3(rawColour.rgb) * tint.w + tint.rgb), float(rawColour.a));
    return o;
}

#ifdef TRDR_STANDALONE
layout(location = 0) out vec3 gsPosition;
layout(location = 1) out vec3 stq;
layout(location = 2) out vec4 colour;
layout(location = 3) out float keep;
layout(location = 4) out float viewZ;

void main()
{
    const TrdrVertex v = trdrVertex(gl_VertexID);
    gsPosition = v.gs;
    stq = v.stq;
    colour = v.colour;
    keep = v.keep;
    viewZ = v.viewZ;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
#else
out float gl_ClipDistance[1];

void trackMain()
{
    const TrdrVertex v = trdrVertex(gl_VertexID);
    gl_ClipDistance[0] = v.viewZ - clipRange.z;
    vCol = v.colour;
    vColFlat = v.colour;
    vSTQ = v.st; // Interpolated with perspective by the GPU.
    vUV = vec2(0.0);
    vUVClamp = vec4(-1e30, -1e30, 1e30, 1e30);
    if (v.keep == 0.0)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // Outside the clip volume: the whole triangle is discarded.
        return;
    }
    const vec2 pixels = v.gs.xy - gsToClip.xy + gsToClip.z;
    float depth = clamp(v.gs.z / gsDepthRange, 0.0, 1.0);
    if (gsToClip.w > 0.5)
        depth = 1.0 - depth;
    gl_Position = vec4((pixels * uInvSize * 2.0 - 1.0) * v.viewZ, (depth * 2.0 - 1.0) * v.viewZ, v.viewZ);
}
#endif
