#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D ta[4];
void main(){ o = texelFetch(ta[2], ivec2(0), 0); }
