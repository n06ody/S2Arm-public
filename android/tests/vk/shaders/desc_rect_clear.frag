// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(set = 0, binding = 0) uniform texture2D image;
layout(set = 0, binding = 1) uniform sampler nearestSampler;
layout(set = 0, binding = 2, std140) uniform Tone { vec4 scale; } tone;
layout(location = 0) out vec4 color;
void main() {
    color = texelFetch(sampler2D(image, nearestSampler), ivec2(0), 0) * tone.scale;
}
