// Exercise the production result parser and layout mapping with a real compiler result.
#include "../native/gfx_plat/nvk_shader.c"

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 2;
    int compute = argc == 3 && !strcmp(argv[2], "--compute");
    if (argc == 3 && !compute) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END)) return 2;
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return 2; }
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return 2; }
    fclose(f);
    struct fd_program p = {0}; struct nvn_vk_compute_program cp = {0}; char why[192] = "";
    int parsed = compute ? nvn_vk_program_parse_compute(data, (size_t)n, &cp, why, sizeof why) :
                           nvn_vk_program_parse_graphics(data, (size_t)n, &p, why, sizeof why);
    if (parsed) {
        fprintf(stderr, "SETUP FAILED: real result parse: %s\n", why);
        if (compute) nvn_vk_program_free_compute(&cp); else nvn_vk_program_free(&p);
        free(data); return 2;
    }
    int found = 0, ok = 0;
    for (int i = 0; i < (compute ? cp.binding_count : p.binding_count); ++i) {
        const struct fd_binding *b = compute ? &cp.bindings[i] : &p.bindings[i];
        if (b->kind != 2 || strcmp(b->stage, compute ? "cs" : "fs")) continue;
        nvn_gpu_layout_ent e;
        found++;
        ok = layout_ent(b, &e) && e.cls == NVN_GPU_RES_TBUF && e.src_kind == NVN_RB_TBUF &&
             e.src_reg == (compute ? 0 : 128) && e.src_reg2 == UINT16_MAX && e.count == 1;
        if (!ok) fprintf(stderr, "FAIL: BUFFER layout must be TBUF at texture slot %u without sampler (cls=%u src=%u slot=%u sampler=%u)\n",
                         compute ? 0u : 128u, e.cls, e.src_kind, e.src_reg, e.src_reg2);
    }
    if (compute) nvn_vk_program_free_compute(&cp); else nvn_vk_program_free(&p);
    free(data);
    if (found != 1) { fprintf(stderr, "SETUP FAILED: expected one FS buffer binding\n"); return 2; }
    if (!ok) return 1;
    puts(compute ? "PASS: real compute BUFFER result maps to TBUF source without sampler" : "PASS: real BUFFER result maps to TBUF source without sampler");
    return 0;
}
