// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set=0,binding=0) uniform texture2D images[2];
layout(set=0,binding=1) uniform sampler samp;
layout(location=0) out vec4 color;
void main() {
    // Both the real slot and the dummy alias the same source: snapshot once,
    // replace every descriptor, never mutate the immutable source table.
    ivec2 p=ivec2(gl_FragCoord.xy);
    color=(texelFetch(sampler2D(images[0],samp),p,0)+texelFetch(sampler2D(images[1],samp),p,0))*0.5+vec4(0.25,0,0,0);
}
