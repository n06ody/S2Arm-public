#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D t;
vec4 f(sampler2D s){ return texelFetch(s, ivec2(0), 0); }
void main(){ o = f(t); }
