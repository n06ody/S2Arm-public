// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
#ifndef FLOAT_INPUTS
#define FLOAT_INPUTS 0
#endif
#if FLOAT_INPUTS
#define INPUT_TYPE sampler2D
#else
#define INPUT_TYPE usampler2D
#endif
layout(set=0,binding=0) uniform INPUT_TYPE a;
layout(set=0,binding=1) uniform INPUT_TYPE b;
layout(location=0) out uvec4 result;
void main() {
    uvec4 rgba=uvec4(texelFetch(a,ivec2(gl_FragCoord.xy),0));
    uvec4 rg=uvec4(texelFetch(b,ivec2(gl_FragCoord.xy),0));
    result=uvec4(rgba.r,rgba.b,rg.r >> 8,rg.g & 255u);
}
