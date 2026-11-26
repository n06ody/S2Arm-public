#version 450
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform texture2D uTex[2];
layout(set = 1, binding = 1) uniform sampler uSmp;
#ifdef SPECIALIZE_IMAGE
layout(constant_id = 0) const int imageIndex = 0;
#else
const int imageIndex = 0;
#endif
void main() {
    oColor = vec4(texture(sampler2D(uTex[imageIndex], uSmp), vec2(0.5)).r, 0, 0, 1);
}
