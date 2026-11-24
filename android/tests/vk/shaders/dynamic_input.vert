// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) in vec2 position;
void main() {
    gl_Position = vec4(position, 0.5, 1.0);
}
