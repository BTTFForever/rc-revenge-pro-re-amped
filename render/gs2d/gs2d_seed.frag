#version 450
// render/gs2d: seeds a colour target and the depth texture from CPU planes (check (c)'s emulated memory, the tests'
// patterned destinations): colour from an RGBA8 texture, depth = Z * 2^-24 from an R32_UINT texture (exact below 2^24).
layout(set = 2, binding = 17) uniform texture2D uColour;
layout(set = 2, binding = 18) uniform utexture2D uDepth;
layout(set = 2, binding = 33) uniform sampler uSampler;
layout(location = 0) out vec4 outColor;

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    outColor = texelFetch(sampler2D(uColour, uSampler), p, 0);
    const uint z = texelFetch(usampler2D(uDepth, uSampler), p, 0).r;
    gl_FragDepth = float(z) * (1.0 / 16777216.0);
}
