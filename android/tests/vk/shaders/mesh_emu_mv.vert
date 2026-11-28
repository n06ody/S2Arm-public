#version 450
layout(set = 0, binding = 0, std430) readonly buffer MeshVbo {
    vec4 pos[];
};

void main() {
    gl_Position = pos[gl_VertexIndex];
}
