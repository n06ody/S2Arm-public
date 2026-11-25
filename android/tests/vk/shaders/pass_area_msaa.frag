// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) out vec4 color;

void main() {
    // Match small in t_replay_pass_area.c with an exclusive right and bottom edge.
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (any(lessThan(pixel, ivec2(11, 9))) || any(greaterThanEqual(pixel, ivec2(22, 19))))
        discard;
    color = vec4(0.0, 1.0, 0.0, 1.0);
}
