// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler samp;
layout(location=0) out vec4 color;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy)%textureSize(sampler2D(image,samp),0);
    color=texelFetch(sampler2D(image,samp),p,0)+vec4(0.25,0,0,0);
}
