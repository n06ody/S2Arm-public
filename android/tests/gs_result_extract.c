// Parse a real compiler result with the production parser and export each module.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/nvn_vk_program_result.h"
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    if (fseek(f, 0, SEEK_END)) return 2; long len = ftell(f); rewind(f);
    if (len <= 0) return 2;
    unsigned char *bytes = malloc((size_t)len); if (!bytes) return 2;
    if (fread(bytes, 1, (size_t)len, f) != (size_t)len) return 2; fclose(f);
    struct fd_program p = {0}; char why[192] = {0};
    int rc = nvn_vk_program_parse_graphics(bytes, (size_t)len, &p, why, sizeof why); free(bytes);
    if (rc || !p.stage[0].present || !p.stage[1].present || !p.stage[2].present ||
        p.stage[3].present || p.stage[4].present || p.stage[5].present) {
        fprintf(stderr, "production result parse failed: %s\n", why); nvn_vk_program_free(&p); return 1;
    }
    const char *names[3] = {"vs", "fs", "gs"};
    for (int i = 0; i < 3; i++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s.spv", argv[2], names[i]);
        f = fopen(path, "wb"); if (!f || fwrite(p.stage[i].spirv, 1, p.stage[i].spirv_bytes, f) != p.stage[i].spirv_bytes) return 2;
        fclose(f);
    }
    printf("production parser: VS/FS/GS, %d bindings", p.binding_count);
    for (int i = 0; i < p.binding_count; i++) printf(" [%s kind=%d set=%d binding=%d slot=%d]", p.bindings[i].stage, p.bindings[i].kind, p.bindings[i].set, p.bindings[i].binding, p.bindings[i].slot);
    putchar('\n'); nvn_vk_program_free(&p); return 0;
}
