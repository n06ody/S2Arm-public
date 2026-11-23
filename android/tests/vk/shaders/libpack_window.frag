// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
// 存储缓冲窗口读回夹具的片元：不碰任何资源，只让图形库的顶点 / 片元两段能成对。顶点段的存储读写才是被验的东西。
layout(location = 0) out vec4 o;
void main() { o = vec4(0.0, 1.0, 0.0, 1.0); }
