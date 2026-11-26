// SPDX-License-Identifier: Apache-2.0
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) flat in int instance_layer[];
layout(location = 0) flat out int layer;
void main() {
    for (int i = 0; i < 3; ++i) {
        layer = instance_layer[0];
        gl_Layer = layer;
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
