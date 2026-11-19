// SPDX-License-Identifier: Apache-2.0
#version 450
layout(set=0,binding=0,std430) readonly buffer Input { vec4 word; } data;
layout(location=0) out vec4 color;
void main(){color=data.word;}
