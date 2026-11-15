// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
// #1072 第 1 项负控：顶点阶段只读存储缓冲（readonly = NonWritable，不要 vertexPipelineStoresAndAtomics）
layout(set = 0, binding = 0, std430) readonly buffer In { uint w[]; } i;
void main() {
    gl_Position = vec4(float(i.w[gl_VertexIndex]), 0.0, 0.0, 1.0);
}
