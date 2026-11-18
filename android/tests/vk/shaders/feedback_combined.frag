// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) out vec4 color;
void main() {
    color=texelFetch(image,ivec2(gl_FragCoord.xy),0)+vec4(0.25,0,0,0);
}
