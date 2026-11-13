#version 450
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// The production result lists these descriptors even though this fixture does
// not sample them. Keep the SPIR-V declarations identical to that metadata.
layout(set=1, binding=BINDING0) uniform sampler2D tex0;
layout(set=1, binding=BINDING1) uniform sampler2D tex1;
layout(location=0) out vec4 c; void main(){c=vec4(1,0,0,1);}
