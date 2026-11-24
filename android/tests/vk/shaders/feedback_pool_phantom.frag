// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set=0,binding=0) uniform texture2D images[3];
layout(set=0,binding=1) uniform sampler samp;
layout(location=0) out vec4 color;
#ifndef SAMPLE_ELEMENT
#define SAMPLE_ELEMENT 0
#endif
void main() {
    // 整池数组里槽 1 登记着本通道正在写的那张图，但着色器只采样槽 0：
    // 「整张贴图池都在描述符里」不等于「这一笔会读本通道的附件」。
    ivec2 p=ivec2(gl_FragCoord.xy);
    color=texelFetch(sampler2D(images[SAMPLE_ELEMENT],samp),p,0)+vec4(0.25,0,0,0);
}
