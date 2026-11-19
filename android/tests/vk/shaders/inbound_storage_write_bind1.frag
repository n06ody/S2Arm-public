// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set=0,binding=1,std430) buffer Output { vec4 words[4]; } output_data;
layout(location=0) out vec4 color;
void main() {
    if (ivec2(gl_FragCoord.xy)==ivec2(0)) output_data.words[3]=vec4(1,0,0,1);
    color=vec4(0,0,0,1);
}
