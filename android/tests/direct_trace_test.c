// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "../native/gfx_plat/direct_diag.h"
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void pause_ms(long ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000}; nanosleep(&t, NULL); }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (!strcmp(argv[2], "collision")) {
        char path[2048];
        snprintf(path, sizeof path, "%s/direct-trace-%ld-123-000000.jsonl", argv[1], (long)getpid());
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 || write(fd, "existing", 8) != 8 || close(fd)) return 6;
    }
    if (s2a_direct_diag_start(argv[1], 123)) return 3;
    if (!strcmp(argv[2], "off")) { if (s2a_direct_diag_enabled()) return 4; pause_ms(350); return 0; }
    if (!s2a_direct_diag_enabled()) return 5;
    if (!strcmp(argv[2], "collision")) {
        pause_ms(350); if (s2a_direct_diag_enabled()) return 7;
        puts("READY"); fflush(stdout); for (;;) pause_ms(1000);
    }
    if (!strcmp(argv[2], "pipeline")) {
        uint64_t p = s2a_direct_diag_pipe_begin(1, 0);
        if (!p) { fprintf(stderr, "FAIL: driver pipeline begin must be recorded\n"); return 9; }
        s2a_direct_diag_pipe_end(p, 1, 2);
        uint8_t id[32]; memset(id, 0xcd, sizeof id);
        s2a_direct_diag_pack(id, 3, 0);
        uint64_t up = s2a_direct_diag_upload_begin(3, 20);
        if (!up) { fprintf(stderr, "FAIL: upload begin must be recorded\n"); return 10; }
        s2a_direct_diag_upload_end(up, 1, 1, 320);
        pause_ms(300); puts("READY"); fflush(stdout); for (;;) pause_ms(1000);
    }
    uint8_t id[32]; memset(id, 0xab, sizeof id);
    s2a_direct_diag_frame();
    uint64_t token = s2a_direct_diag_compile_begin(id, 7, 1, 0);
    pause_ms(180); // begin and end cross completed segment boundary
    s2a_direct_diag_compile_end(token, 1);
    s2a_direct_diag_frame();
    if (!strcmp(argv[2], "overflow")) for (int i = 0; i < 20000; ++i) s2a_direct_diag_frame();
    if (!strcmp(argv[2], "inflight")) (void)s2a_direct_diag_compile_begin(id, 32, 1, 0);
    pause_ms(500);
    puts("READY"); fflush(stdout);
    for (;;) pause_ms(1000);
}
