// SPDX-License-Identifier: Apache-2.0
#version 450
void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    gl_FragDepth = 0.125 + 0.25 * float(at.x + 2 * at.y) + float(gl_SampleID) / 64.0;
}
