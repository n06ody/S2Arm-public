// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
// 存储缓冲窗口读回夹具（私有真实顶点段那一档）的片元：读顶点段 location 2 的输出当颜色（它是存储缓冲里实例数据的函数）。
layout(location = 2) in vec4 v;
layout(location = 0) out vec4 o;
void main() { o = vec4(clamp(v.xyz, 0.0, 1.0), 1.0); }
