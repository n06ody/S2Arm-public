// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set = 0, binding = 0, std140) uniform Color { vec4 value; } color;
layout(location = 0) out vec4 discarded_target;
layout(location = 1) out vec4 observed_target;
void main() {
    discarded_target = vec4(1, 0, 0, 1);
    observed_target = color.value;
}
