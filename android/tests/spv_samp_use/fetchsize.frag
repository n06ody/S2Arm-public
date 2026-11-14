#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D t;
void main(){ o = texelFetch(t, ivec2(0), 0) + vec4(vec2(textureSize(t, 0)), 0.0, 0.0); }
