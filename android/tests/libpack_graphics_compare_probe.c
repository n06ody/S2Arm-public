// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Consume an original synthetic LibPack through the production adapter and parser.
#include "direct/libpack_vk.h"
#include "nvn/nvn_vk_program_result.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int key8(const char *s, uint8_t out[8]) {
    if (strlen(s) != 16) return -1;
    for (int i = 0; i < 8; i++) {
        unsigned b;
        if (sscanf(s + i * 2, "%2x", &b) != 1) return -1;
        out[i] = (uint8_t)b;
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    uint8_t keys[6][8] = {{0}};
    if (key8(argv[2], keys[0]) || key8(argv[3], keys[1])) return 2;
    struct s2a_libpack *lp = NULL;
    if (s2a_libpack_open(&lp, argv[1]) != 1) return 2;
    uint8_t *bytes = NULL; size_t n = 0; char why[512] = "";
    int rc = s2a_libpack_program_v1(lp, keys, 3, 1, "fixture-profile", "fixture-closure", &bytes, &n, why, sizeof why);
    if (rc) { fprintf(stderr, "libpack synthesis rc=%d: %s\n", rc, why); s2a_libpack_close(lp); return 1; }
    struct fd_program p = {0};
    rc = nvn_vk_program_parse_graphics(bytes, n, &p, why, sizeof why);
    if (rc) { fprintf(stderr, "graphics parser rejected: %s\n", why); free(bytes); s2a_libpack_close(lp); return 1; }
    printf("{\"parsedSupportBytes\":%u,\"program\":", p.support_bytes);
    nvn_vk_program_free(&p);
    if (fwrite(bytes, 1, n, stdout) != n || fputs("}", stdout) == EOF) rc = 1;
    free(bytes); s2a_libpack_close(lp); return rc;
}
