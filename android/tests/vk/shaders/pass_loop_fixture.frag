#version 450
layout(std430, set = 1, binding = 1) readonly buffer Source { uint words[]; } source_data;
layout(set = 2, binding = 0) uniform sampler2D color_source;
#ifdef PROBE
layout(std430, set = 3, binding = 0) buffer Probe { uint words[]; } probe;
#endif
layout(location = 0) out vec4 rgba;
layout(location = 1) out uvec4 data;
void main() {
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint record = pixel.x / 32;
    uint word252 = record * 144 + 63;
    uint word508 = record * 144 + 127;
    uint raw252 = source_data.words[word252];
    uint raw508 = source_data.words[word508];
    rgba = texture(color_source, vec2(0.5));
    data = uvec4(raw252, raw508, 0, 1);
#ifdef PROBE
    uvec2 cell = pixel / 4;
    uvec2 phase = uvec2(probe.words[4], probe.words[5]);
    if (all(equal(pixel % 4, phase / 2))) {
        uint start = 8 + 10 * (cell.y * 16 + cell.x);
        if (atomicAdd(probe.words[start], 1) == 0) {
            probe.words[start + 1] = min(raw252, 63);
            probe.words[start + 2] = min(raw508, 63);
            probe.words[start + 3] = record;
            probe.words[start + 4] = raw252;
            probe.words[start + 5] = raw508;
            probe.words[start + 6] = record;
            probe.words[start + 7] = record;
            probe.words[start + 8] = word252;
            probe.words[start + 9] = word508;
        }
    }
#endif
}
