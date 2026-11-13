#version 450
layout(set=0,binding=0) uniform V { vec4 color; } v;
layout(location=0) out vec4 color;
void main() { gl_Position = vec4(0,0,0,1); color = v.color; }
