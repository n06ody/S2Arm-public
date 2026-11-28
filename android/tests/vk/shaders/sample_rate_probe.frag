#version 450
layout(location = 0) out vec4 out_color;
void main() {
    // No sample-qualified input or SampleId: only the pipeline can request sample shading.
    float red = fract(gl_FragCoord.x) < 0.5 ? 1.0 : 0.0;
    out_color = vec4(red, 1.0, 0.0, 1.0);
}
