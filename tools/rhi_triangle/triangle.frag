#version 450
// rhi_triangle: the checkerboard texture, sampled nearest.
// The RHI's binding model (ico-pc port/rhi/rhi.h, "bind groups"): descriptor set = bind group, binding = slot plus
// 0 for uniform buffers (b), 16 for textures (t), 32 for samplers (s), 48 for storage writes (u). Group 2 holds
// textures t1.. and samplers s1.., so slot 1 is texture binding 17 and sampler binding 33.
layout(set = 2, binding = 17) uniform texture2D uTex;
layout(set = 2, binding = 33) uniform sampler uSmp;
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sampler2D(uTex, uSmp), inUv);
}
