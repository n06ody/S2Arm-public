// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#version 450
layout(location=0) in vec4 aPos;
layout(set=0,binding=0,std140) uniform DriverVS { layout(offset=1604) uint base; } vs;
layout(location=0) flat out uint observedVS;
void main(){gl_Position=aPos;observedVS=vs.base;}
