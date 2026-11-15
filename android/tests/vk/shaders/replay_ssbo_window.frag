// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location=0) in vec4 vColor;
layout(set=0,binding=1,std430) readonly buffer Sb { uint v[]; } sb;
layout(location=0) out vec4 color;
// 读窗口里的第 1 个 uint（字节 4）：游戏地址比对齐处多 4 字节时，编译器生成的取数就是这个下标
void main(){color=unpackUnorm4x8(sb.v[1]);}
