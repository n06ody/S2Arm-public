// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Exercise the real provider stop branch and frame-log producer with a retained frontend.
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "direct/s2a_direct.h"
#include "direct/vk_device.h"
#include "nvn_vk_frame_log.h"
static int test_expired;
void nvn_vk_frame_log_snapshot_retained(uint64_t) __attribute__((weak));
#include "retained_stop.h"
int nvn_vk_dt_last_gap(uint64_t *a, uint64_t *b) { (void)a; (void)b; return 0; }
int nvn_vk_first_draw_gpu_last(double *a, int *b, uint64_t *c) { (void)a; (void)b; (void)c; return 0; }

int main(int argc, char **argv) {
    int blocked = argc > 1 && !strcmp(argv[1], "blocked");
    test_expired = argc > 1 && !strcmp(argv[1], "expired");
    struct provider_context ctx = {0};
    ctx.engine_ok = 1; ctx.state = S2A_DIRECT_ACTIVE;
    s2a_frame_log_present();
    s2a_frame_log_present();
    if (blocked) for (int i = 0; i < 16000; i++) s2a_frame_log_present();
    int rc = request_stop_impl(&ctx);
    if (rc != S2A_DIRECT_RES_RETIREMENT || g_retained_context != &ctx) {
        fprintf(stderr, "FAIL: active frontend must remain retained\n"); return 1;
    }
    if (!blocked && request_stop_impl(&ctx) != S2A_DIRECT_RES_RETIREMENT) return 1;
    fprintf(stdout, "retained stop preserved\n");
    if (blocked) { fflush(stdout); _exit(0); }
    return 0;
}
