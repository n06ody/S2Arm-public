#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D t;
layout(set=0,binding=2) uniform sampler2D ta[];
layout(push_constant) uniform P { int i; } p;
void main(){ o = texelFetch(t, ivec2(0), 0) + texelFetch(ta[nonuniformEXT(p.i)], ivec2(0), 0); }
