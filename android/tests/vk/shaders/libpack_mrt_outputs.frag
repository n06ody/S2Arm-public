// SPDX-License-Identifier: Apache-2.0
#version 450
layout(location=0) out vec4 lighting;
#if MRT_CLASS == 2
#define MRT4 ivec4
#define MRT2 ivec2
#elif MRT_CLASS == 1 || MRT_CLASS == 3
#define MRT4 uvec4
#define MRT2 uvec2
#else
#define MRT4 vec4
#define MRT2 vec2
#endif
layout(location=1) out MRT4 material1;
layout(location=2) out MRT4 material2;
#if MRT_CLASS == 3
layout(location=3) out ivec2 material3;
#else
layout(location=3) out MRT2 material3;
#endif
void main() {
    lighting = vec4(0.25, 0.5, 0.75, 1.0);
#if MRT_CLASS == 2
    material1 = ivec4(-13, 37, -69, 99);
#else
    material1 = MRT4(13, 37, 69, 255);
#endif
    material2 = MRT4(21, 58, 95, 121);
#if MRT_CLASS == 3
    material3 = ivec2(-257, 513);
#else
    material3 = MRT2(257, 513);
#endif
}
