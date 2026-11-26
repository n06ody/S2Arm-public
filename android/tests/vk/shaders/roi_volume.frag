#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform texture3D volume;
layout(set = 0, binding = 1) uniform sampler nearestSampler;
layout(set = 0, binding = 2, std140) uniform Coefficients { vec4 value[9]; } coefficients;
void main() {
    vec3 coord = vec3(0.5 + uv.y, 0.25 + uv.x, 0.625);
    vec4 sampled = textureLod(sampler3D(volume, nearestSampler), coord, 0.0);
    color = vec4(sampled.rg, sampled.b * coefficients.value[8].w, sampled.a);
}
