#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out uint color;
void main() { color = uv.x < 0.5 ? 37u : 73u; }
