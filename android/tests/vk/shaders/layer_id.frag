// SPDX-License-Identifier: Apache-2.0
#version 450
layout(location = 0) flat in int layer;
layout(location = 0) out vec4 color;
void main() {
    color = vec4(float(layer + 1) / 255.0, 0.0, 0.0, 1.0);
}
