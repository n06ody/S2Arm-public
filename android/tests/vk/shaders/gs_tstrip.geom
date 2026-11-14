#version 450
// #696：每个输入三角形在 4x1 目标的第 gl_PrimitiveIDIn 列画满一格，颜色 = (gl_in[0..2] 的顶点号, 图元号) / 255，读回即可逐个对顶点序。
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in flat int vid[];
layout(location = 0) out vec4 color;
void main() {
    int p = gl_PrimitiveIDIn;
    float x0 = -1.0 + 0.5 * float(p), x1 = x0 + 0.5;
    vec4 c = vec4(float(vid[0]), float(vid[1]), float(vid[2]), float(p)) / 255.0;
    color = c; gl_Position = vec4(x0, -1, 0, 1); EmitVertex();
    color = c; gl_Position = vec4(x1, -1, 0, 1); EmitVertex();
    color = c; gl_Position = vec4(x0, 1, 0, 1); EmitVertex();
    color = c; gl_Position = vec4(x1, 1, 0, 1); EmitVertex();
    EndPrimitive();
}
