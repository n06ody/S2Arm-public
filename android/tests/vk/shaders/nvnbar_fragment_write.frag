// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set = 0, binding = 0, std430) writeonly buffer Output { vec4 words[4]; } data;
layout(location = 0) out vec4 color;
void main() {
    if (ivec2(gl_FragCoord.xy) == ivec2(0))
        data.words[3] = vec4(0, 1, 0, 1);
    color = vec4(0, 1, 0, 1);
}
