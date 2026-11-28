// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location=0) flat in vec4 color;
layout(location=0) out vec4 out_color;
void main() { out_color = color; }
