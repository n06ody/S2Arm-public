#version 450
layout(triangles, equal_spacing, ccw) in;
layout(set = 0, binding = 1) uniform EvalParams {
    vec4 tint;
};
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec4 out_color;
void main() {
    vec4 pos = gl_in[0].gl_Position * gl_TessCoord.x
             + gl_in[1].gl_Position * gl_TessCoord.y
             + gl_in[2].gl_Position * gl_TessCoord.z;
    gl_Position = pos;
    // lvl=1 只有外边界顶点（min_bary == 0）；lvl=3 内环三角顶点在 (2/9, 2/9, 5/9)（min_bary = 2/9 ≈ 0.222 > 0.15）。
    float min_bary = min(gl_TessCoord.x, min(gl_TessCoord.y, gl_TessCoord.z));
    float interior = min_bary > 0.15 ? 1.0 : 0.0;
    out_color = vec4(interior, tint.g, tint.b, 1.0);
}
