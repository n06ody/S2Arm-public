// SPDX-License-Identifier: Apache-2.0
#version 450
layout(set=0,binding=0) uniform sampler2D data;
layout(location=0) out vec4 color;
void main(){color=texelFetch(data,ivec2(0),0);}
