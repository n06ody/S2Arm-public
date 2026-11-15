// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
// #1072 第 1 项：顶点阶段写存储缓冲（要 vertexPipelineStoresAndAtomics）
layout(set = 0, binding = 0, std430) buffer Out { uint w[]; } o;
void main() {
    o.w[gl_VertexIndex] = 1u;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
