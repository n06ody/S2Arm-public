#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform texture2D tx;
layout(set=0,binding=2) uniform sampler sm;
void main(){ o = texelFetch(sampler2D(tx, sm), ivec2(0), 0); }
