// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 渲染倍率用例：按支撑缓冲 render_scale[0]（偏移 192 的 .x）把 FragCoord 换回游戏坐标，红 / 绿 = 游戏 x / y 的低 8 位（同 Ryujinx 翻译器对 FragCoord 的换算）
#version 450
layout(set=0,binding=0,std140) uniform Support { layout(offset=192) vec4 rs0; } sb;
layout(location=0) out vec4 color;
void main(){uint gx=uint(gl_FragCoord.x/sb.rs0.x),gy=uint(gl_FragCoord.y/sb.rs0.x);color=vec4(float(gx&255u)/255.0,float(gy&255u)/255.0,0.0,1.0);}
