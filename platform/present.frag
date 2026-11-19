#version 450
// RC_RENDERER=vk (platform/vk_host.cpp): the emulated GS's displayed picture, bilinear (as the raylib window draws
// it), opaque. Bind group 2: texture slot 1 = binding 17, sampler slot 1 = binding 33 (third_party/ico_rhi/VENDORED.md).
layout(set = 2, binding = 17) uniform texture2D uPicture;
layout(set = 2, binding = 33) uniform sampler uSampler;
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(texture(sampler2D(uPicture, uSampler), inUv).rgb, 1.0);
}
