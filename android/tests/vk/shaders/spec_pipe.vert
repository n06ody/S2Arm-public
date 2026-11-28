// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
const vec2 points[3] = vec2[3](vec2(-.85,.25), vec2(-.25,.25), vec2(-.85,.85));
void main() { gl_Position = vec4(points[gl_VertexIndex], .625, 1); }
