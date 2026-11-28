// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) flat out int tag;
void main() {
    tag = gl_VertexIndex;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
