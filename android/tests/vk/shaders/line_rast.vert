#version 450
void main() {
    // 水平线段穿过 32x32 帧缓冲正中行（y=16），x 覆盖像素 [3..28]
    vec2 pos = (gl_VertexIndex == 0) ? vec2(-0.8125, 0.03125) : vec2(0.8125, 0.03125);
    gl_Position = vec4(pos, 0.0, 1.0);
}
