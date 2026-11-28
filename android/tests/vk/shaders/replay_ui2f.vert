// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location = 0) in vec4 position;
layout(location = 3) in vec4 code;
layout(location = 0) out vec4 vColor;
void main() {
    float value = abs(code.x);
    vec2 p = position.xy;
    if (value == 1.0) {
        p *= 0.5;
    } else {
        p.x = p.x * 0.25 + float(gl_InstanceIndex - 3) * 0.5 - 0.75;
    }
    gl_Position = vec4(p, 0.0, 1.0);
    if (position.z != 0.0 || position.w != 1.0 || code.w != 1.0) {
        vColor = vec4(0, 0, 1, 1);
    } else if (value == 11.0) {
        vColor = vec4(1, 0, 0, 1);
    } else if (value == 22.0 || value == 1.0) {
        vColor = vec4(0, 1, 0, 1);
    } else {
        vColor = vec4(0, 0, 1, 1);
    }
}
