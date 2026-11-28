#version 450
layout(vertices = 3) out;
layout(set = 0, binding = 0) uniform TessParams {
    vec4 outer_lvl;
    vec4 inner_lvl;
};
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; } gl_out[];
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = outer_lvl.x;
        gl_TessLevelOuter[1] = outer_lvl.y;
        gl_TessLevelOuter[2] = outer_lvl.z;
        gl_TessLevelInner[0] = inner_lvl.x;
    }
}
