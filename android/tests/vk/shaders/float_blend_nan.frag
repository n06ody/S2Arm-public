// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(constant_id = 0) const float finite_red = 1.0;
layout(location = 0) out vec4 plain;
layout(location = 1) out vec4 blended;

void main() {
    float qnan = uintBitsToFloat(0x7fc12345u);
    float pinf = uintBitsToFloat(0x7f800000u);
    float ninf = uintBitsToFloat(0xff800000u);
    plain = vec4(qnan, pinf, ninf, 0.0);
    // Keep the scalar stores made by the production translator. Both exit paths
    // write A, but the transparent path stores it after the RGB components.
    if (gl_FragCoord.x < 2.0) {
        blended.r = qnan;
        blended.g = pinf;
        blended.b = ninf;
        blended.a = gl_FragCoord.x < 1.0 ? 0.0 : uintBitsToFloat(0x80000000u);
        return;
    }
    bool finite = gl_FragCoord.x < 3.0;
    blended.r = finite ? finite_red : qnan;
    blended.g = finite ? 0.5 : pinf;
    blended.b = finite ? 0.25 : ninf;
    blended.a = 0.5;
}
