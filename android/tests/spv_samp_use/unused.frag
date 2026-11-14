#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D t;
layout(set=0,binding=2) uniform sampler2D u;
void main(){ o = texelFetch(u, ivec2(0), 0); }
