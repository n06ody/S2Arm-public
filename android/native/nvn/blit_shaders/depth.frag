// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
#ifdef BLIT_MSAA
layout(set=0, binding=0) uniform sampler2DMS sourceDepth;
#else
layout(set=0, binding=0) uniform sampler2D sourceDepth;
#endif
layout(push_constant) uniform Params { vec4 originScale; vec4 destination; uint bit; } p;
void main() {
    ivec2 at = ivec2(floor(p.originScale.xy + (gl_FragCoord.xy - p.destination.xy) * p.originScale.zw));
    gl_FragDepth = texelFetch(sourceDepth, at, 0).r;
}
