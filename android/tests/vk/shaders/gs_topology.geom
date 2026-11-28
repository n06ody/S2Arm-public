// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
#if defined(ADJ_LINES)
layout(lines_adjacency) in;
#elif defined(ADJ_TRIANGLES)
layout(triangles_adjacency) in;
#elif defined(LINES)
layout(lines) in;
#else
layout(triangles) in;
#endif
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) flat in int tag[];
layout(location = 0) flat out vec4 color;
void main() {
    float left = -1.0 + float(gl_PrimitiveIDIn) * 0.25;
// Sum each group independently so legal rotations preserve the expected color.
#if defined(ADJ_LINES)
    vec4 ordered = vec4(tag[1] + tag[2], tag[0] + tag[3], 4, 255) / 255.0;
#elif defined(ADJ_TRIANGLES)
    vec4 ordered = vec4(tag[0] + tag[2] + tag[4], tag[1] + tag[3] + tag[5], 6, 255) / 255.0;
#elif defined(LINES)
    vec4 ordered = vec4(tag[0], tag[1], 0, 255) / 255.0;
#else
    vec4 ordered = vec4(tag[0], tag[1], tag[2], 255) / 255.0;
#endif
    for (int i = 0; i < 4; i++) {
        color = ordered;
        gl_Position = vec4(left + 0.25 * float(i & 1), i < 2 ? -1.0 : 1.0, 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
