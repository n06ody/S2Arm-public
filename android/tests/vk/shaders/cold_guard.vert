// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) flat out uint vertex_marker;
void main() {
    const vec2 positions[] = {vec2(-1, -1), vec2(3, -1), vec2(-1, 3)};
    gl_Position = vec4(positions[gl_VertexIndex - 5], 0, 1);
    vertex_marker = uint(gl_VertexIndex + 17 * gl_InstanceIndex);
}
