#version 450
#extension GL_EXT_multiview : require
layout(location = 0) flat out vec4 v_color;
void main() {
    vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    v_color = (gl_ViewIndex == 0) ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 0.0, 1.0, 1.0);
}
