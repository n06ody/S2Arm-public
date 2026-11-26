// SPDX-License-Identifier: Apache-2.0
#version 450
#ifdef WRITE_LAYER
#extension GL_ARB_shader_viewport_layer_array : require
#endif
layout(location = 0) in vec4 position;
layout(location = 0) flat out int layer;
void main() {
    gl_Position = position;
    layer = gl_InstanceIndex;
#ifdef WRITE_LAYER
    gl_Layer = layer;
#endif
}
