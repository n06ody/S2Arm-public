// Separate image and sampler descriptors acquire their relationship at OpSampledImage.
// Work on the module sent to Vulkan. An unrecognized value flow leaves the stage unproven.
enum { SEP_ANY_ELEMENT = UINT16_MAX };

struct sep_pair {
    uint16_t sampler, sampler_elem, image, image_elem;
};

struct sep_spv_id {
    const uint32_t *def;
    uint32_t set_plus_one, binding_plus_one;
};

static int sep_spv_resource(const struct nvn_gpu_shader_s *sh,
                            const struct sep_spv_id *ids, uint32_t bound, uint32_t value,
                            uint32_t cls, uint16_t *ent, uint16_t *elem) {
    uint32_t index = 0;
    int access = 0, load = 0;
    for (unsigned hop = 0; hop < 16; hop++) {
        if (!value || value >= bound || !ids[value].def) return 0;
        const uint32_t *d = ids[value].def;
        const uint32_t words = d[0] >> 16;
        switch ((SpvOp)(d[0] & 0xffffu)) {
        case SpvOpCopyObject:
            value = d[3];
            break;
        case SpvOpLoad:
            if (load++) return 0;
            value = d[3];
            break;
        case SpvOpAccessChain:
        case SpvOpInBoundsAccessChain: {
            if (!load || access++ || words != 5) return 0;
            index = SEP_ANY_ELEMENT;
            const uint32_t *i = d[4] < bound ? ids[d[4]].def : NULL;
            if (i && (i[0] == ((4u << 16) | SpvOpConstant) || i[0] == ((3u << 16) | SpvOpConstantNull))) {
                const uint32_t *ty = i[1] < bound ? ids[i[1]].def : NULL;
                if (ty && ty[0] == ((4u << 16) | SpvOpTypeInt) && ty[2] == 32)
                    index = (i[0] & 0xffffu) == SpvOpConstant ? i[3] : 0;
            }
            value = d[3];
            break;
        }
        case SpvOpVariable:
            if (!load || d[3] != SpvStorageClassUniformConstant ||
                !ids[value].set_plus_one || !ids[value].binding_plus_one) return 0;
            for (uint32_t j = 0; j < sh->nent; j++) {
                const nvn_gpu_layout_ent *e = &sh->ents[j];
                if (e->cls != cls || e->set + 1u != ids[value].set_plus_one ||
                    e->binding + 1u != ids[value].binding_plus_one) continue;
                const uint32_t count = e->count ? e->count : 1;
                if ((!access && count != 1) || (index != SEP_ANY_ELEMENT && index >= count)) return 0;
                *ent = (uint16_t)j;
                *elem = (uint16_t)index;
                return 1;
            }
            return 0;
        default:
            return 0;
        }
    }
    return 0;
}

static void sep_spv_pairs(struct nvn_gpu_shader_s *sh, const uint32_t *code, size_t bytes) {
    unsigned samp = 0;
    for (uint32_t i = 0; i < sh->nent; i++) samp |= sh->ents[i].cls == NVN_GPU_RES_SAMP;
    if (!samp || bytes % 4 || bytes < 20 || bytes / 4 > UINT32_MAX || code[0] != SpvMagicNumber ||
        !code[3] || code[3] > (1u << 20)) return;
    const uint32_t bound = code[3], end = (uint32_t)(bytes / 4);
    struct sep_spv_id *ids = calloc(bound, sizeof *ids);
    if (!ids) return;
    uint32_t samples = 0;
    struct sep_pair *pairs = NULL;
    for (uint32_t pos = 5; pos < end;) {
        const uint32_t *d = code + pos;
        const uint32_t words = d[0] >> 16;
        if (!words || words > end - pos) goto done;
        const SpvOp op = (SpvOp)(d[0] & 0xffffu);
        uint32_t result = 0;
        switch (op) {
        case SpvOpTypeInt:
            if (words == 4) result = d[1];
            break;
        case SpvOpConstantNull:
            if (words == 3) result = d[2];
            break;
        case SpvOpConstant:
        case SpvOpVariable:
        case SpvOpLoad:
        case SpvOpCopyObject:
        case SpvOpAccessChain:
        case SpvOpInBoundsAccessChain:
            if (words >= 4) result = d[2];
            break;
        case SpvOpDecorate:
            if (words == 4 && d[1] < bound) {
                if (d[2] == SpvDecorationDescriptorSet) ids[d[1]].set_plus_one = d[3] + 1u;
                if (d[2] == SpvDecorationBinding) ids[d[1]].binding_plus_one = d[3] + 1u;
            }
            break;
        case SpvOpSampledImage:
            if (words != 5) goto done;
            samples++;
            break;
        default:
            break;
        }
        if (result >= bound) goto done;
        if (result) ids[result].def = d;
        pos += words;
    }
    // A successfully scanned module can have unused sampler declarations.
    pairs = calloc(samples ? samples : 1, sizeof *pairs);
    if (!pairs) goto done;
    uint32_t n = 0;
    for (uint32_t pos = 5; pos < end; pos += code[pos] >> 16) {
        const uint32_t *d = code + pos;
        if ((d[0] & 0xffffu) != SpvOpSampledImage) continue;
        struct sep_pair *p = &pairs[n++];
        if (!sep_spv_resource(sh, ids, bound, d[3], NVN_GPU_RES_TEX, &p->image, &p->image_elem) ||
            !sep_spv_resource(sh, ids, bound, d[4], NVN_GPU_RES_SAMP, &p->sampler, &p->sampler_elem)) goto done;
    }
    sh->sep_pairs = pairs;
    sh->nsep_pairs = samples;
    pairs = NULL;
done:
    free(pairs);
    free(ids);
}
