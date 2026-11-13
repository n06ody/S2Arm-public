// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Included after a production shader fixture and the extracted production
// provider Stop/context functions. The scheduling hooks only pause threads.
static void *resolve_thread(void *arg) {
    struct rp_shader_req *q = arg; const struct rp_prog *out = NULL;
    (void)rp_shader_resolve(q, &out); return NULL;
}
static void *activate_thread(void *arg) { return (void *)(intptr_t)activate(arg); }
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "late") && strcmp(argv[1], "race") && strcmp(argv[1], "control") && strcmp(argv[1], "activate"))) return 2;
    stop_test_mode = !strcmp(argv[1], "control") ? 0 : !strcmp(argv[1], "race") ? 2 : 1;
    struct provider_context ctx = {0}; ctx.state = S2A_DIRECT_ACTIVE; ctx.engine_ok = 1;
    ctx.session = 4; ctx.generation = 6; ctx.direct_abi = 1; ctx.device_ok = 1;
    ctx.device.properties.limits.minStorageBufferOffsetAlignment = 16;
    snprintf(ctx.profile_json, sizeof ctx.profile_json, "%s", profile); ctx.profile_bytes = strlen(ctx.profile_json);
    snprintf(ctx.profile_hash, sizeof ctx.profile_hash, "%s", phash); snprintf(ctx.closure, sizeof ctx.closure, "%s", "closure-test");
    g_active_context = &ctx; atomic_store(&g_stop_published, 0); atomic_store(&g_session_active, 1);
    rp_state_reset(); s2a_source_clear(); source(); bind_pools(7); compiler_calls = 0;
    if (!strcmp(argv[1], "activate")) {
        ctx.state = S2A_DIRECT_PREPARED; ctx.engine_ok = 0; stop_test_mode = 3;
        pthread_t t; if (pthread_create(&t, NULL, activate_thread, &ctx)) return 2;
        pthread_mutex_lock(&stop_test_mu);
        while (!stop_test_entered) pthread_cond_wait(&stop_test_cv, &stop_test_mu);
        pthread_mutex_unlock(&stop_test_mu);
        int32_t stop_rc = request_stop_impl(&ctx);
        int retained = g_retained_context == &ctx && ctx.activation_started;
        pthread_mutex_lock(&stop_test_mu); stop_test_release = 1; pthread_cond_broadcast(&stop_test_cv); pthread_mutex_unlock(&stop_test_mu);
        void *result = NULL; pthread_join(t, &result);
        printf("activation/stop: stop=%d activate=%ld retained=%d stop_published=%d active=%d\n", stop_rc, (long)(intptr_t)result, retained, atomic_load(&g_stop_published), atomic_load(&g_session_active));
        if (stop_rc != S2A_DIRECT_RES_RETIREMENT || (intptr_t)result != S2A_DIRECT_RES_RETIREMENT || !retained || !atomic_load(&g_stop_published) || atomic_load(&g_session_active)) {
            fprintf(stderr, "FAIL: stop during activation must retain context and prevent activation publication\n"); return 1;
        }
        return 0;
    }
    if (stop_test_mode == 0) {
        struct rp_shader_req control = {0}; const struct rp_prog *out = NULL;
        control.prim = 4; (void)rp_shader_resolve(&control, &out);
        control.prim = 0; (void)rp_shader_resolve(&control, &out);
        printf("control: compiler calls=%d\n", compiler_calls);
        if (compiler_calls != 2) { fprintf(stderr, "FAIL: active provider must accept both distinct requests\n"); return 1; }
        rp_shader_reset(); s2a_source_clear(); return 0;
    }
    struct rp_shader_req q = {0}; q.prim = 4; pthread_t th;
    if (pthread_create(&th, NULL, resolve_thread, &q)) return 2;
    pthread_mutex_lock(&stop_test_mu);
    while (!stop_test_entered) pthread_cond_wait(&stop_test_cv, &stop_test_mu);
    pthread_mutex_unlock(&stop_test_mu);
    int stop_rc = request_stop_impl(&ctx);
    if (stop_rc != S2A_DIRECT_RES_RETIREMENT || !atomic_load(&g_stop_published) || !ctx.engine_ok) {
        fprintf(stderr, "SETUP: real provider stop must retain in-flight session\n"); return 2;
    }
    pthread_mutex_lock(&stop_test_mu); stop_test_release = 1; pthread_cond_broadcast(&stop_test_cv); pthread_mutex_unlock(&stop_test_mu);
    pthread_join(th, NULL);
    int failed = 0;
    if (stop_test_mode == 2) {
        if (compiler_calls != 0) { fprintf(stderr, "FAIL: stop won admission race but compiler entered %d time(s)\n", compiler_calls); failed = 1; }
    } else {
        if (compiler_calls != 1) { fprintf(stderr, "SETUP: first compile did not enter\n"); return 2; }
        q.prim = 0; const struct rp_prog *out = NULL; (void)rp_shader_resolve(&q, &out);
        if (compiler_calls != 1) { fprintf(stderr, "FAIL: new shader compile admitted after provider stop (%d calls)\n", compiler_calls); failed = 1; }
    }
    printf("stop/%s: calls=%d, stop_rc=%d, context retained=%d\n", argv[1], compiler_calls, stop_rc, ctx.engine_ok);
    rp_shader_reset(); s2a_source_clear(); return failed;
}
