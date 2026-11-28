#version 450
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
layout(location = 0) flat out u16vec4 v_out;
void main() {
    vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    v_out = u16vec4(uint16_t(100), uint16_t(200), uint16_t(150), uint16_t(255));
}
