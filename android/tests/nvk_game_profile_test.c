// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Compile the real Android platform file; only Android logging/properties are stubs.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include "debug_gate.h"
static bool host_log;
#define s2a_debug_entries_open() host_log
#ifndef PROFILE_SOURCE
#define PROFILE_SOURCE "../native/gfx_plat/plat_nvn.c"
#endif
char *profile_strdup(const char *s);
#define strdup profile_strdup
#include PROFILE_SOURCE
#undef strdup
char *profile_strdup(const char *s) { return getenv("PROFILE_FAIL_ROOT") ? NULL : strdup(s); }
static int logs;
int __android_log_print(int priority, const char *tag, const char *format, ...) {
    (void)priority; (void)tag;
    if (host_log) return 0;  // logcat and the host file are separate sinks.
    va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
    fputc('\n', stderr); logs++; return 0;
}
int __system_property_get(const char *name, char *value) { (void)name; value[0] = 0; return 0; }
const char *nvk_session_cache_dir(void) { return NULL; }
static void *read_profile(void *unused) { (void)unused; return (void *)s2a_nvn_prof(); }
int main(int argc, char **argv) {
    assert(argc == 3);
    if (!strcmp(argv[2], "host-log")) {
        // A C stream may split long lines. The owner writes to fd 2 independently
        // of stdio, so a FILE lock cannot keep those pieces together.
        static char buffer[1024], payload[2001];
        assert(setvbuf(stderr, buffer, _IOFBF, sizeof buffer) == 0);
        memset(payload, 'x', sizeof payload - 1);
        host_log = true;
        nvn_plat_log(NVN_PLAT_LOG_INFO, "land-readback", payload);
        static const char owner[] = "[s2a] direct owner loop exited: stopped\n";
        assert(write(STDERR_FILENO, owner, sizeof owner - 1) == (ssize_t)sizeof owner - 1);
        assert(fflush(stderr) == 0);
        return 0;
    }
    if (strcmp(argv[1], "unset")) setenv("S2A_SHADER_REVISION_ROOT", argv[1], 1);
    else unsetenv("S2A_SHADER_REVISION_ROOT");
    if (!strcmp(argv[2], "oom")) setenv("PROFILE_FAIL_ROOT", "1", 1);
    pthread_t threads[16]; void *results[16];
    for (int i = 0; i < 16; i++) assert(!pthread_create(&threads[i], NULL, read_profile, NULL));
    for (int i = 0; i < 16; i++) assert(!pthread_join(threads[i], &results[i]));
    for (int i = 1; i < 16; i++) assert(results[i] == results[0]);
    const s2a_game_tsv *t = &((const s2a_nvn_profile *)results[0])->t;
    if (!strcmp(argv[2], "valid")) {
        assert(t->app_pool == 0x12340000); assert(t->render_cond == 0);
        assert(t->fps_hold == 0); assert(t->tex_flush_upload == 0 && t->tex_dirty_upload == 0);
        assert(t->tex_dyn_slots == 0 && t->gpu_ts_segment == 0);
        assert(t->vconv_range == 1 && t->draw_barriers_game == 1);
        assert(t->present_display == 1);
        assert(!strcmp(t->alpha_from_red, "abc def"));
        assert(!strcmp(t->feedback_snap, "123")); assert(!strcmp(t->zero_skip, "abc:0:1:2"));
        unsetenv("S2A_FPS60"); assert(!DM_GAME_PRESENT_DISPLAY);
        setenv("S2A_FPS60", "1", 1); assert(DM_GAME_PRESENT_DISPLAY);
    } else {
        s2a_game_tsv expected; s2a_game_tsv_defaults(&expected);
        assert(!memcmp(t, &expected, sizeof expected));
    }
    if (!strcmp(argv[2], "oom")) {
        assert(logs == 16);
        unsetenv("PROFILE_FAIL_ROOT");
        const s2a_nvn_profile *loaded = s2a_nvn_prof();
        assert(loaded->t.app_pool == 0x12340000);
        s2a_game_tsv expected; s2a_game_tsv_defaults(&expected);
        assert(!memcmp(t, &expected, sizeof expected));
        puts("allocation failure fallback assertions passed"); return 0;
    }
    assert(logs == 1); // Exactly one load/fallback diagnosis despite concurrent readers.
    assert(&s2a_nvn_prof()->t == t); assert(logs == 1);
    if (!strcmp(argv[2], "valid")) {
        unsetenv("S2A_SHADER_REVISION_ROOT");
        s2a_game_tsv expected; s2a_game_tsv_defaults(&expected);
        assert(!memcmp(&s2a_nvn_prof()->t, &expected, sizeof expected));
        assert(logs == 2);
        setenv("S2A_SHADER_REVISION_ROOT", argv[1], 1);
        assert(s2a_nvn_prof()->t.app_pool == 0x12340000); assert(logs == 3);
        // A distinct revision with no TSV must not inherit the previous game.
        char next[4096]; snprintf(next, sizeof next, "%s/other", argv[1]);
        setenv("S2A_SHADER_REVISION_ROOT", next, 1);
        assert(!memcmp(&s2a_nvn_prof()->t, &expected, sizeof expected)); assert(logs == 4);
        setenv("S2A_SHADER_REVISION_ROOT", argv[1], 1);
        assert(s2a_nvn_prof()->t.app_pool == 0x12340000); assert(logs == 5);
        snprintf(next, sizeof next, "%s/unreadable", argv[1]);
        setenv("S2A_SHADER_REVISION_ROOT", next, 1);
        assert(!memcmp(&s2a_nvn_prof()->t, &expected, sizeof expected)); assert(logs == 6);
    }
    puts("profile assertions passed"); return 0;
}
int s2a_fps60_display_source(void) { return 0; }
