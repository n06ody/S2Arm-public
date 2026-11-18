// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler samp;
layout(location=0) out vec4 color;
void main() {
    float z=texelFetch(sampler2D(image,samp),ivec2(gl_FragCoord.xy),0).r;
    color=vec4(z+0.25,0,0,1);
    gl_FragDepth=z+0.25;
}
