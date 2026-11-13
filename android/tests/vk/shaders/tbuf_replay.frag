#version 450
layout(set=0,binding=0) uniform usamplerBuffer data_buf;
layout(location=0) out vec4 colour;
void main() {
    uint x = (texelFetch(data_buf, 1).x >> 8) & 255u;
    uint y = (texelFetch(data_buf, 2).x >> 8) & 255u;
    colour = vec4(float(x), float(y), float(textureSize(data_buf)), 255.0) / 255.0;
}
