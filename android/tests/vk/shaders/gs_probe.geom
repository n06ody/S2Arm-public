#version 450
layout(points) in;
layout(triangle_strip, max_vertices=3) out;
layout(set=0,binding=1) uniform G { vec4 color; } g;
layout(location=0) in vec4 vin[];
layout(location=0) out vec4 color;
void main() {
    color = vin[0] + g.color;
    gl_Position = vec4(-1,-1,0,1); EmitVertex();
    gl_Position = vec4(3,-1,0,1); EmitVertex();
    gl_Position = vec4(-1,3,0,1); EmitVertex();
    EndPrimitive();
}
