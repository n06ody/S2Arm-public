#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D ta[4];
layout(push_constant) uniform P { int i; } p;
void main(){ o = texelFetch(ta[p.i], ivec2(0), 0); }
