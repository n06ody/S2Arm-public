// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) out vec4 a;
layout(location = 1) out vec4 b;
void main() {
    a = vec4(1, 0, 0, 1);
    b = vec4(0, 1, 0, 1);
}
