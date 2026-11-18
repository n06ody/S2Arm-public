// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Included by t_backend_vk.c: real Vulkan, same helpers and validation as C74/C79.
// Four color slots x 15 nonempty masks = 60 legal keys. Every clear has its own
// four-row stripe; untouched channels and the later ordinary draw are checked.
struct c1501_target {
    nvn_gpu_tex tex[4];
    nvn_gpu_view view[4];
    nvn_gpu_buf readback[4];
};

static void c1501_make(struct c1501_target *t) {
    for (int i = 0; i < 4; i++) {
        t->tex[i] = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
        t->view[i] = make_view(t->tex[i]);
        t->readback[i] = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    }
}

static void c1501_release(struct c1501_target *t) {
    for (int i = 0; i < 4; i++) {
        nvn_gpu_view_release(t->view[i]);
        nvn_gpu_tex_release(t->tex[i]);
        nvn_gpu_buf_release(t->readback[i], NULL, NULL);
    }
}

static nvn_gpu_cmd c1501_record(struct c1501_target *t, unsigned keys,
                               nvn_gpu_pipe pipe, nvn_gpu_buf vb) {
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 4; p.w = p.h = 64; p.layers = 1;
    for (int i = 0; i < 4; i++) {
        p.color[i].view = t->view[i];
        p.color[i].load = NVN_GPU_LOAD_CLEAR;
        p.color[i].store = NVN_GPU_STORE_STORE;
        p.color[i].clear.f[0] = p.color[i].clear.f[3] = 0.25f;
        p.color[i].clear.f[1] = 0.5f; p.color[i].clear.f[2] = 0.75f;
    }
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_clear cl = {0};
    cl.w = 32; cl.h = 4;
    for (int i = 0; i < 4; i++) cl.value.f[i] = 1;
    for (unsigned i = 0; i < keys; i++) {
        cl.color = (int)(i / 15); cl.color_mask = (i % 15) + 1;
        cl.y = (int)(i % 15) * 4;
        nvn_gpu_clear_rect(c, &cl);
    }
    // Same overflow key twice: a reuse must still use this clear's rectangle.
    cl.y = 60; nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.sc[0] = 32; dyn.sc[2] = 32;
    nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    for (int i = 0; i < 4; i++)
        nvn_gpu_copy_tex_buf(c, t->tex[i], 0, 0, NULL, t->readback[i], 0, 64 * 4, 64 * 64 * 4);
    return c;
}

static void c1501_pixels(struct c1501_target *t, unsigned keys, unsigned segment) {
    const uint8_t base[4] = {64, 128, 191, 64}, blue[4] = {0, 0, 255, 255};
    unsigned bad = 0;
    for (unsigned slot = 0; slot < 4; slot++) {
        const uint8_t *p = buf_cpu(t->readback[slot]);
        for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
            unsigned mask = 0;
            if (x < 32) {
                if (y < 60 && slot * 15 + y / 4 < keys) mask = y / 4 + 1;
                if (y >= 60 && slot == (keys - 1) / 15) mask = (keys - 1) % 15 + 1;
            }
            for (unsigned ch = 0; ch < 4; ch++) {
                uint8_t want = (slot == 0 && x >= 32) ? blue[ch] : (mask & (1u << ch)) ? 255 : base[ch];
                if (p[(y * 64 + x) * 4 + ch] != want) bad++;
            }
        }
    }
    CHECK(bad == 0, "C1501 keys=%u segment=%u masked clear + later draw: mismatched bytes=%u", keys, segment, bad);
    printf("C1501 keys=%u segment=%u checked_bytes=65536 bad=%u\n", keys, segment, bad);
}

static void c1501_capacity_arm(const char *dir, unsigned keys) {
    int before = g_fail;
    if (!open_backend()) { CHECK(0, "C1501 open"); return; }
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs, &fs, &vm, &fm)) {
        CHECK(0, "C1501 shaders"); return;
    }
    nvn_gpu_pipe_desc pd; fill_gpipe_desc(&pd, vs, fs, 0);
    for (int i = 1; i < 4; i++) pd.cfmt[i] = NVN_FORMAT_RGBA8;
    nvn_gpu_err e = {0}; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C1501 ordinary pipe: %s", e.why);
    nvn_gpu_buf vb = full_vb();
    struct c1501_target target[3] = {0};
    for (int i = 0; i < 3; i++) c1501_make(&target[i]);
    // All three are recorded before any submit. Completing/aborting one must
    // not invalidate another's still-unsubmitted clear pipelines.
    nvn_gpu_cmd cmd[3];
    for (int i = 0; i < 3; i++) cmd[i] = c1501_record(&target[i], keys, pipe, vb);
    for (unsigned i = 0; i < 2; i++) {
        if (i == 1) nvn_gpu_cmd_abort(cmd[2]);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(cmd[i], i + 1, on_done, NULL, &e);
        CHECK(OK(r), "C1501 keys=%u segment=%u commit must continue: rc=%d %s", keys, i, r.code, e.why);
        r = nvn_gpu_fence_wait(i + 1, 5000000000ull, &e);
        CHECK(OK(r), "C1501 keys=%u segment=%u completion: rc=%d %s", keys, i, r.code, e.why);
        if (OK(r)) c1501_pixels(&target[i], keys, i);
    }
    for (int i = 0; i < 3; i++) c1501_release(&target[i]);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1501 close: %s", e.why);
    printf("C1501 keys=%u failures=%d\n", keys, g_fail - before);
}

static void c1501_clear_capacity(const char *dir) {
    c1501_capacity_arm(dir, 48);   // Boundary negative control: already legal before the fix.
    c1501_capacity_arm(dir, 60);
}
