// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) in vec4 instanceColor;
layout(location = 0) out vec4 color;
void main() {
    vec2 corner = vec2(gl_VertexIndex >> 1, gl_VertexIndex & 1);
    float column = float(gl_InstanceIndex & 7);
    gl_Position = vec4(-1.0 + (column + corner.x) * 0.25, corner.y * 2.0 - 1.0, 0.0, 1.0);
    color = instanceColor;
}
