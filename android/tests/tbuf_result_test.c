// Test-only comparison of production result-parser bindings, using real compiler output.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/nvn_vk_program_result.h"

static int load(const char *path, struct fd_program *p) {
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END)) return -1;
    long n = ftell(f);
    if (n < 1 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    char *s = malloc((size_t)n);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); fclose(f); return -1; }
    fclose(f);
    char why[256] = "";
    int rc = nvn_vk_program_parse_graphics((const uint8_t *)s, (size_t)n, p, why, sizeof why);
    free(s);
    if (rc) fprintf(stderr, "parser setup failed: %s\n", why);
    return rc;
}

static const struct fd_binding *texture(const struct fd_program *p) {
    for (int i = 0; i < p->binding_count; ++i)
        if (p->bindings[i].set == 2 && p->bindings[i].binding == 0 && !strcmp(p->bindings[i].stage, "fs"))
            return &p->bindings[i];
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    struct fd_program buffer = {0}, texture2d = {0};
    int rc = load(argv[1], &buffer) || load(argv[2], &texture2d);
    const struct fd_binding *a = texture(&buffer), *b = texture(&texture2d);
    if (rc || !a || !b) { fprintf(stderr, "parser setup failed: expected real FS set=2 binding=0\n"); rc = 2; }
    else if (!memcmp(a, b, sizeof *a)) {
        fprintf(stderr, "FAIL: parser loses buffer-texture classification; parsed bindings are identical\n"); rc = 1;
    } else { puts("PASS: parsed buffer and image bindings remain distinguishable"); rc = 0; }
    nvn_vk_program_free(&buffer); nvn_vk_program_free(&texture2d);
    return rc;
}
