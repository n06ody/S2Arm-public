// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(constant_id = 3) const uint red = 0u;
layout(constant_id = 7) const bool enabled = false;
layout(location = 0) out vec4 color;
void main() { color = vec4(enabled ? float(red) / 255.0 : 0.0, 1, 0, 1); }
