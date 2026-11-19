// SPDX-License-Identifier: Apache-2.0
#version 450
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler samp;
layout(location=0) out vec4 color;
void main(){color=texelFetch(sampler2D(image,samp),ivec2(0,0),0);}
