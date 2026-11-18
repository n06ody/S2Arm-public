// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
#ifndef NO_SAMPLE
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler samp;
#endif
#ifdef READ_ONLY_COLOR
vec4 first;
#else
layout(location=0) out vec4 first;
#endif
#ifndef DEPTH_SOURCE
layout(location=1) out vec4 second;
#endif
void main() {
    // Deliberately not the fragment's own sample: STORE is not safe just because
    // a pixel-local read would happen before its own store operation.
#ifdef NO_SAMPLE
    float z=0.25;
#else
    ivec2 p=(ivec2(gl_FragCoord.xy)+ivec2(1,2))%textureSize(sampler2D(image,samp),0);
    float z=texelFetch(sampler2D(image,samp),p,0).r;
#endif
    first=vec4(z+0.25,0,0,1);
#ifdef DEPTH_SOURCE
    gl_FragDepth=z+0.25;
#else
    second=first;
#endif
}
