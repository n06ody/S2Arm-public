// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location=0) flat out vec4 color;
void main() {
    const vec2 pos[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    const vec4 rgb[3] = vec4[3](vec4(1,0,0,1), vec4(0,1,0,1), vec4(0,0,1,1));
    gl_Position = vec4(pos[gl_VertexIndex], 0, 1);
    color = rgb[gl_VertexIndex];
}
