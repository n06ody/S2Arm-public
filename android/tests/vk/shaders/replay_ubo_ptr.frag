#version 450
layout(set = 0, binding = 0, std430) buffer Data { uvec4 color; } data;
layout(location = 0) out vec4 outColor;
void main() {
#if WRITABLE
    data.color = uvec4(7, 9, 11, 255);
#endif
    outColor = vec4(data.color) / 255.0;
}
