// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location=0) flat in uint observedVS;
layout(set=0,binding=1,std140) uniform DriverFS { layout(offset=1604) uint base; } fs;
layout(location=0) out vec4 color;
void main(){color=vec4(float(observedVS&255u)/255.0,float(fs.base&255u)/255.0,0.0,1.0);}
