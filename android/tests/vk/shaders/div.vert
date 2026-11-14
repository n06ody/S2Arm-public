#version 450
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;
// #977 实例除数用例：每个实例右移半个 NDC 单位（64 宽的目标上是 16 像素一列）。
void main() { vec4 p = aPos; p.x += 0.5 * float(gl_InstanceIndex); gl_Position = p; vColor = aColor; }
