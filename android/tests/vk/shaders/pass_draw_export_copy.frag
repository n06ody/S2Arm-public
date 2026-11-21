#version 450
layout(set = 0, binding = 0) uniform sampler2D source_image;
#ifndef DRAW_EXPORT_DEPTH
layout(location = 0) out vec4 color;
#endif
void main() {
    vec4 sample_value = texelFetch(source_image, ivec2(gl_FragCoord.xy), 0);
#ifdef DRAW_EXPORT_DEPTH
    gl_FragDepth = sample_value.r;
#else
    color = sample_value;
#endif
}
