// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) flat in uint vertex_marker;
layout(std430, set = 0, binding = 0) readonly buffer Counters { uint words[]; } counters;
layout(std140, set = 0, binding = 1) uniform Driver { uvec4 words[2]; } drv;
layout(location = 0) out vec4 color;
layout(location = 1) out uvec4 integers;
void main() {
    uint record = min((uint(gl_FragCoord.x) / 4u) * 64u, 64u);
    uint offset = drv.words[1].x;
    uint word = ((record * 20u + 12u + offset) - (offset & ~15u)) >> 2;
    uint count = counters.words[word];
    uint sum = 0;
    if (count != 0u) {
        for (uint i = 0; i < count; i++) sum++;
    }
    color = vec4(vec3(uvec3(11 + sum, vertex_marker, 57)) / 255, 1);
    integers = uvec4(23 + sum, 71, vertex_marker, 199);
}
