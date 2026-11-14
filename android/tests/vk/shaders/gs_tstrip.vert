#version 450
// #696：每个顶点把自己的 gl_VertexIndex 交给 GS（位置不用，GS 自己出矩形）。
layout(location = 0) out flat int vid;
void main() { vid = gl_VertexIndex; gl_Position = vec4(0, 0, 0, 1); }
