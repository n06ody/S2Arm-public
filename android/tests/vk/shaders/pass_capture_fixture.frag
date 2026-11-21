#version 450
layout(set = 0, binding = 0) uniform sampler2D sampled;
layout(location = 0) out vec4 rgba;
layout(location = 1) out uvec4 data;
void main() {
    rgba = texture(sampled, vec2(0.5));
    data = uvec4(7);
}
