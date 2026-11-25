// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set = 0, binding = 0, std430) buffer Count { uint fragments; } counter;
void main() {
    atomicAdd(counter.fragments, 1u);
}
