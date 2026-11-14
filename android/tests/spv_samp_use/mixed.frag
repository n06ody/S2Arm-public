#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D t;
void main(){ o = texelFetch(t, ivec2(0), 0) + textureGather(t, gl_FragCoord.xy); }
