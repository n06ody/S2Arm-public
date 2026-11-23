#version 450
// 绑定号 ≥ 256 的 UBO：预编库程序保留全部 arg_id，绑定号 = arg_id × 6 + k 可到 262 以上（8E 全黑那次的 fp_t_… = 262）。
// 262 和 6 低 8 位相同：布局合并表若把 binding 截成 8 位，两项会合并成一项、布局里就没有 262。
layout(set = 0, binding = 262) uniform A { vec4 a; } ua;
layout(set = 0, binding = 6) uniform B { vec4 b; } ub;
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(ua.a.x, ub.b.y, 0.0, 1.0); }
