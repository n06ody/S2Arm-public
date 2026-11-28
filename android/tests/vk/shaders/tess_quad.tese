#version 450
layout(quads, equal_spacing, ccw) in;
layout(set = 0, binding = 1) uniform EvalParams {
    vec4 tint;
};
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec4 out_color;
void main() {
    vec4 bottom = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x);
    vec4 top = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, gl_TessCoord.x);
    gl_Position = mix(bottom, top, gl_TessCoord.y);
    out_color = vec4(tint.r, tint.g, tint.b, 1.0);
}
