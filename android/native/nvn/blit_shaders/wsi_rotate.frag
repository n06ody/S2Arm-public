// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450

layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D upright;
layout(push_constant) uniform Rotation {
    uint surfaceTransform;
} pc;

void main() {
    ivec2 dst = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(upright, 0);
    ivec2 src = dst;
    // Vulkan surface transform values. Map physical framebuffer pixels back
    // to the upright composition, with y increasing downwards.
    if (pc.surfaceTransform == 2u) src = ivec2(dst.y, size.y - 1 - dst.x);
    if (pc.surfaceTransform == 4u) src = size - 1 - dst;
    if (pc.surfaceTransform == 8u) src = ivec2(size.x - 1 - dst.y, dst.x);
    color = texelFetch(upright, src, 0);
}
