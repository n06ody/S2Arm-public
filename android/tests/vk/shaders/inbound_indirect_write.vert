// SPDX-License-Identifier: Apache-2.0
#version 450
layout(set=0,binding=0,std430) buffer Output { uvec4 args; } output_data;
void main(){if(gl_VertexIndex==0)output_data.args=uvec4(3,1,0,0);vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.0-1.0,0,1);}
