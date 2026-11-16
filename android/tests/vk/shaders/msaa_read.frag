#version 450
layout(set = 0, binding = 0) uniform sampler2DMS tex;
layout(location = 0) out vec4 colour;
void main() { colour = texelFetch(tex, ivec2(0, 0), 0); }
