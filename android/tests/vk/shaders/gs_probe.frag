#version 450
layout(set=0,binding=2) uniform F { vec4 color; } f;
layout(location=0) in vec4 color;
layout(location=0) out vec4 out_color;
void main() { out_color = color + f.color; }
