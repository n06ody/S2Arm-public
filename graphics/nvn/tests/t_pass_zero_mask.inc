// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Included by t_nvn_replay.c. Observe the actual backend pass and pipeline
// descriptions; do not emulate a driver corrupting a zero-mask attachment.
static void zm_mask(stream *s, uint32_t mask) {
    chmask_st cm = {mask}; p_bind_state *b = put(s, OP_BIND_STATE, sizeof *b);
    b->kind = 2; memcpy(b->bytes, &cm, sizeof cm);
}

static void zm_targets(stream *s, const tex_t *a, const tex_t *b, const tex_t *depth) {
    p_set_rt r = {0}; r.n = 2; r.color[0] = a; r.color[1] = b; r.depth = depth;
    uint8_t *p = put(s, OP_SET_RT, set_rt_size(&r)); p_set_rt_c *h = set_rt_put(p, &r);
    const tex_t **pt = (const tex_t **)(h + 1); tex_t *snap = (tex_t *)(p + set_rt_snap_off(h));
    snap[0] = *a; pt[0] = &snap[0]; snap[1] = *b; pt[1] = &snap[1];
    if (depth) { snap[2] = *depth; pt[2] = &snap[2]; }
}

static void t_pass_zero_mask(void) {
    int checks = g_checks, fails = g_fail;
    // A disabled color slot is physically absent; nonzero channel subsets are
    // retained, and depth/stencil stays available independently of color masks.
    for (int masked = 0; masked < 2; masked++) {
        pass_fresh(); g_resolve_rc = g_cresolve_rc = 0;
        stream s = {0}; q_env(&s);
        tex_t b = mk_tex(15012, 0x4000, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
        tex_t z = mk_tex(15013, 0x8000, NVN_FORMAT_DEPTH24_STENCIL8, 64, 36, 1, 0);
        zm_targets(&s, &g_qrt, &b, &z); zm_mask(&s, masked ? 0x1 : 0x21);
        int draws = g_gdraws; q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_gdraws == draws + 1 && g_npd == 1);
        CHECK(g_pd[0].color[0].view && (!!g_pd[0].color[1].view == !masked));
        CHECK(g_pd[0].depth.view && g_pd[0].stencil.view);
        CHECK(g_last_pdesc.cfmt[0] == NVN_FORMAT_RGBA8 &&
              g_last_pdesc.cfmt[1] == (masked ? 0u : NVN_FORMAT_RGBA8));
        CHECK(g_last_pdesc.chmask.mask == (masked ? 0x1u : 0x21u));
        CHECK(nvn_be_tex_gpu_owned(b.serial) == !masked);
        CHECK(nvn_unimpl_kinds() == 0);
    }
    // Only a change in the retained slot set cuts the pass. Nonzero -> nonzero
    // changes the pipeline mask but must preserve pass continuation.
    pass_fresh(); stream transitions = {0}; q_env(&transitions);
    tex_t b = mk_tex(15014, 0x4000, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    zm_targets(&transitions, &g_qrt, &b, NULL);
    int draws = g_gdraws;
    zm_mask(&transitions, 0xf); q_draw(&transitions);
    zm_mask(&transitions, 0x7); q_draw(&transitions);
    zm_mask(&transitions, 0xf0); q_draw(&transitions);
    zm_mask(&transitions, 0xf); q_draw(&transitions);
    end(&transitions); submit1(&transitions); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == draws + 4 && g_npd == 3 && g_pass_ends == 3);
    CHECK(g_pd[0].color[0].view && !g_pd[0].color[1].view);
    CHECK(!g_pd[1].color[0].view && g_pd[1].color[1].view);
    CHECK(g_pd[2].color[0].view && !g_pd[2].color[1].view);
    CHECK(g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
    CHECK(nvn_unimpl_kinds() == 0);

    // A full clear has its own mask. Hiding its target from an ordinary draw
    // must neither consume nor discard that clear; submission still lands it.
    pass_fresh(); stream clear = {0}; q_env(&clear);
    zm_targets(&clear, &g_qrt, &b, NULL); zm_mask(&clear, 0xf);
    p_clear_color *cc = put(&clear, OP_CLEAR_COLOR, sizeof *cc);
    cc->index = 1; cc->mask = 0xf; cc->rgba[1] = 0.75f;
    q_draw(&clear); end(&clear); submit1(&clear); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 2 && g_pd[0].color[0].view && !g_pd[0].color[1].view);
    CHECK(g_pd[1].color[1].view && g_pd[1].color[1].load == NVN_GPU_LOAD_CLEAR &&
          g_pd[1].color[1].clear.f[1] == 0.75f);
    CHECK(nvn_unimpl_kinds() == 0);

    // A partial clear restores an excluded target for the clear. The next
    // ordinary draw excludes it again and keeps its previous pipeline shape.
    pass_fresh(); stream partial = {0}; q_env(&partial);
    zm_targets(&partial, &g_qrt, &b, NULL); zm_mask(&partial, 0xf); q_draw(&partial);
    cc = put(&partial, OP_CLEAR_COLOR, sizeof *cc); cc->index = 1; cc->mask = 2; cc->rgba[1] = 1;
    q_draw(&partial); end(&partial); submit1(&partial); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ncr == 1 && g_npd == 3);
    CHECK(g_cr[0].npd == 2 && g_cr[0].d.color == 1 && g_cr[0].d.color_mask == 2);
    CHECK(!g_pd[0].color[1].view && g_pd[1].color[1].view && !g_pd[2].color[1].view);
    CHECK(g_last_pdesc.cfmt[1] == 0 && nvn_unimpl_kinds() == 0);

    // Shader output mapping is part of the effective write mask, already used
    // by the pipeline. Attachment selection must use the same value.
    pass_fresh(); stream output = {0}; q_env(&output); g_fprog.color_out_mask = 0xf;
    zm_targets(&output, &g_qrt, &b, NULL); q_draw(&output); end(&output);
    submit1(&output); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pd[0].color[0].view && !g_pd[0].color[1].view);
    CHECK(g_last_pdesc.cfmt[1] == 0 && nvn_unimpl_kinds() == 0);

    // Deduplication follows filtering: a disabled earlier alias must not hide
    // a writable later slot referring to the same image subresource.
    pass_fresh(); stream duplicate = {0}; q_env(&duplicate);
    zm_targets(&duplicate, &g_qrt, &g_qrt, NULL); zm_mask(&duplicate, 0xf0);
    q_draw(&duplicate); end(&duplicate); submit1(&duplicate); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && !g_pd[0].color[0].view && g_pd[0].color[1].view);
    CHECK(g_last_pdesc.cfmt[0] == 0 && g_last_pdesc.cfmt[1] == NVN_FORMAT_RGBA8);
    CHECK(nvn_unimpl_kinds() == 0);

    // All disabled is still a real draw (fragment side effects / queries), with
    // an attachment-free pass retaining the logical target extent.
    pass_fresh(); g_caps_extra = NVN_GPU_CAP_NO_ATTACH_PASS;
    stream none = {0}; q_env(&none); zm_targets(&none, &g_qrt, &b, NULL); zm_mask(&none, 0);
    draws = g_gdraws; q_draw(&none); end(&none); submit1(&none); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == draws + 1 && g_npd == 1);
    CHECK(!g_pd[0].color[0].view && !g_pd[0].color[1].view && !g_pd[0].depth.view);
    CHECK(g_pd[0].w == 64 && g_pd[0].h == 36);
    CHECK(g_last_pdesc.cfmt[0] == 0 && g_last_pdesc.cfmt[1] == 0);
    CHECK(nvn_unimpl_kinds() == 0);
    // Capability absent (the current Vulkan backend): keep the original
    // attachments for an all-zero draw, rather than newly skipping the draw.
    g_caps_extra = 0; pass_fresh(); stream retained = {0}; q_env(&retained);
    zm_targets(&retained, &g_qrt, &b, NULL); zm_mask(&retained, 0);
    draws = g_gdraws; q_draw(&retained); end(&retained); submit1(&retained); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == draws + 1 && g_npd == 1);
    CHECK(g_pd[0].color[0].view && g_pd[0].color[1].view);
    CHECK(g_last_pdesc.chmask.mask == 0 && nvn_unimpl_kinds() == 0);
    pass_fresh();
    printf("C1501 zero-mask replay: checks=%d failures=%d\n", g_checks - checks, g_fail - fails);
}
