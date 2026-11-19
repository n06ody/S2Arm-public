// SPDX-License-Identifier: Apache-2.0
#version 450
layout(set=0,binding=0,std430) buffer Output { vec4 words[4]; } output_data;
void main(){
    if (gl_VertexIndex == 0) output_data.words[3]=vec4(0,1,0,1);
    vec2 p=vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position=vec4(p * 2.0 - 1.0,0,1);
}
