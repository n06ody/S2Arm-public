// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
// #1072 第 1 项：顶点阶段写剔除距离（要 shaderCullDistance）
out float gl_CullDistance[1];
void main() {
    gl_CullDistance[0] = 1.0;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
