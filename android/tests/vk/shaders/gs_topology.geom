// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
#ifdef LINES
layout(lines) in;
#else
layout(triangles) in;
#endif
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) flat in int tag[];
layout(location = 0) flat out vec4 color;
void main() {
    float left = -1.0 + float(gl_PrimitiveIDIn) * 0.25;
#ifdef LINES
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
