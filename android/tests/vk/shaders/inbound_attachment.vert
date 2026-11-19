// SPDX-License-Identifier: Apache-2.0
#version 450
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler samp;
layout(location=0) out vec4 value;
void main(){
    value=texelFetch(sampler2D(image,samp),ivec2(0,0),0);
    vec2 p=vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position=vec4(p * 2.0 - 1.0,0,1);
}
