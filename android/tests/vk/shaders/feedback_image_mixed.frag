// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
#ifdef WRITE_STORAGE
layout(set=0,binding=0,rgba8) uniform image2D img;
layout(set=0,binding=1) uniform sampler2D snapshot;
#else
layout(set=0,binding=0,rgba8) uniform readonly image2D img;
#endif
layout(location=1) out vec4 color;
void main() {
    ivec2 xy = ivec2(gl_FragCoord.xy);
    vec4 before = imageLoad(img, xy);
#ifdef WRITE_STORAGE
    imageStore(img, xy, vec4(before.r + .25, before.gba));
    before = texelFetch(snapshot, xy, 0);
#endif
    color = vec4(before.r, 1, 0, 1);
}
