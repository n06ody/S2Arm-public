#version 450
layout(location = 0) in vec4 aPos;
void main() {
    gl_Position = aPos;
#ifdef S2A_TEST_POINT_SIZE
    gl_PointSize = 1.0;
#endif
}
