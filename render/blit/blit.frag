#version 450
// render/blit (renderer plan P1 step 6): the displayed picture, one texel a pixel (texelFetch at the target pixel: no
// filtering, no coordinate arithmetic), with the runtime's presentation transform: R, G, B as stored, alpha 1 (the GS
// alpha is not shown; RENDERER_PLAN.md step 4, check A3). Bind group 2: texture slot 1 = binding 17, sampler slot 1 =
// binding 33 (third_party/ico_rhi/VENDORED.md); the sampler is only there because GLSL needs one to fetch.
layout(set = 2, binding = 17) uniform texture2D uPicture;
layout(set = 2, binding = 33) uniform sampler uSampler;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(texelFetch(sampler2D(uPicture, uSampler), ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
}
