// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set=0,binding=0) uniform sampler2D src;
layout(location=0) out vec4 color;
void main() {
    uvec2 xy=uvec2(gl_FragCoord.xy);
    color=vec4(vec3(xy.x,xy.y,xy.x ^ xy.y)/255.0,1.0);
    color.r+=texelFetch(src,ivec2(xy),0).r;
}
