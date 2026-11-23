// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location=0) in vec4 vColor;
// 与翻译器的声明同长：4096 个 vec4 = 64 KiB
layout(set=0,binding=1,std140) uniform Cbuf { uvec4 v[4096]; } cb;
layout(location=0) out vec4 color;
void main(){color=vec4(cb.v[4095])/255.0;}
