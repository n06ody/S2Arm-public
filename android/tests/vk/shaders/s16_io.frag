#version 450
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
layout(location = 0) flat in u16vec4 v_in;
layout(location = 0) out vec4 color;
void main() {
    uvec4 c = uvec4(v_in);
    color = vec4(float(c.x) / 255.0, float(c.y) / 255.0, float(c.z) / 255.0, float(c.w) / 255.0);
}
