#version 450
layout(location=0) out vec4 o;
layout(set=0,binding=1) uniform sampler2D t;
void main(){ o = texture(t, gl_FragCoord.xy); }
