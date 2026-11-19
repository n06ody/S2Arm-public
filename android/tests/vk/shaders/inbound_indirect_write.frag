// SPDX-License-Identifier: Apache-2.0
#version 450
layout(set=0,binding=0,std430) buffer Output { uvec4 args; } output_data;
layout(location=0) out vec4 color;
void main(){if(ivec2(gl_FragCoord.xy)==ivec2(0))output_data.args=uvec4(3,1,0,0);color=vec4(0,0,0,1);}
