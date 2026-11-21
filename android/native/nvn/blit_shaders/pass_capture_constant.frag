#version 450
layout(location = 0) out vec4 outColor;
#ifdef SECOND_UINT
layout(location = 1) out uvec4 outIndex;
#endif
void main() {
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
#ifdef SECOND_UINT
    outIndex = uvec4(0);
#endif
}
