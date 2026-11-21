#version 450
layout(set = 0, binding = 0) uniform sampler2D sampled;
layout(location = 0) out vec4 rgba;
layout(location = 1) out uvec4 data;
void main() {
    rgba = texture(sampled, vec2(0.5));
#if PASS_VARIANT == 1
    rgba.rgb = vec3(0);
    data = uvec4(8);
#elif PASS_VARIANT == 2
    data = uvec4(9);
#else
    data = uvec4(7);
#endif
}
