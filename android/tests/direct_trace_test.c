// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "../native/gfx_plat/direct_diag.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void pause_ms(long ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000}; nanosleep(&t, NULL); }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (s2a_direct_diag_start(argv[1], 123)) return 3;
    if (!strcmp(argv[2], "off")) { if (s2a_direct_diag_enabled()) return 4; pause_ms(350); return 0; }
    if (!s2a_direct_diag_enabled()) return 5;
    uint8_t id[32]; memset(id, 0xab, sizeof id);
    s2a_direct_diag_frame();
    uint64_t token = s2a_direct_diag_compile_begin(id, 7, 1, 0);
    pause_ms(180); // begin and end cross completed segment boundary
    s2a_direct_diag_compile_end(token, 1);
    s2a_direct_diag_frame();
    if (!strcmp(argv[2], "overflow")) for (int i = 0; i < 5000; ++i) s2a_direct_diag_frame();
    if (!strcmp(argv[2], "inflight")) (void)s2a_direct_diag_compile_begin(id, 32, 1, 0);
    pause_ms(500);
    puts("READY"); fflush(stdout);
    for (;;) pause_ms(1000);
}
