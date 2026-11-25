// SPDX-License-Identifier: Apache-2.0
#version 450
layout(location=0) out vec4 lighting;
#if MRT_CLASS == 2
#define MRT4 ivec4
#define MRT2 ivec2
#elif MRT_CLASS == 1
#define MRT4 uvec4
#define MRT2 uvec2
#else
#define MRT4 vec4
#define MRT2 vec2
#endif
layout(location=1) out MRT4 material1;
layout(location=2) out MRT4 material2;
layout(location=3) out MRT2 material3;
void main() {
    lighting = vec4(0.25, 0.5, 0.75, 1.0);
    material1 = MRT4(13, 37, 69, 255);
    material2 = MRT4(21, 58, 95, 121);
    material3 = MRT2(257, 513);
}
