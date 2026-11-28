// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 任务 / 网格着色器通用计算模拟回放路径（对齐 iOS graphics/nvn/gfx/nvn/nvn_geometry.inc S2A_MESH_EMU）。
// 由 rp_draw.c include（共用 rp_S、rp_R、g_prog、g_pipe、g_pipe_cmd、g_draws、g_skips、g_mxe_bind、
// bind_stage_at、replay_pipe_desc、fill_dyn、caps_cached、tbuf_prepare、image_resolve 等）。
// 约束：共用回放层零后端符号、零 getenv（开关一律走 nvn_plat_flag）；所有超限与未支持项具名报红跳过，绝不静默兜底。

static nvn_hmap g_mxe_cpipes = {.klen = 8};       // nvn_gpu_shader -> nvn_gpu_pipe（计算模拟核管线缓存）
static nvn_hmap g_mxe_cpipe_bad = {.klen = 8};    // 确定性建失败的计算模拟核（整局不再重建）
static uint32_t g_mxe_red_logged;

static void mxe_red(const char *reason, const char *detail, const void *rec) {
    nvn_unimpl("mesh", reason, rec);
    g_skips++;
    if (g_mxe_red_logged < 32u) {
        g_mxe_red_logged++;
        char msg[320];
        snprintf(msg, sizeof msg, "【红】网格模拟跳过（%s）：%s", reason, detail ? detail : "");
        nvn_plat_log(NVN_PLAT_LOG_RED, "mesh", msg);
    }
}

static void rp_mesh_reset(void) {
    free(g_mxe_cpipes.keys); free(g_mxe_cpipes.vals); nvn_hm_init(&g_mxe_cpipes, 8);
    free(g_mxe_cpipe_bad.keys); free(g_mxe_cpipe_bad.vals); nvn_hm_init(&g_mxe_cpipe_bad, 8);
    memset(&g_mxe_bind, 0, sizeof g_mxe_bind);
    g_mxe_red_logged = 0;
}

static nvn_gpu_pipe mxe_cpipe_get(nvn_gpu_shader sh, const char *tag, const void *rec) {
    uint64_t key = (uint64_t)(uintptr_t)sh;
    nvn_gpu_pipe cp = nvn_hm_get(&g_mxe_cpipes, &key);
    if (cp) return cp;
    if (nvn_hm_get(&g_mxe_cpipe_bad, &key)) {
        nvn_unimpl("mesh", tag, rec);
        g_skips++;
        return NULL;
    }
    nvn_gpu_stage_desc sd; memset(&sd, 0, sizeof sd);
    sd.sh = sh;
    nvn_gpu_err err = {0};
    nvn_gpu_rc cr = nvn_gpu_cpipe_get(&sd, NVN_GPU_PIPE_SYNC, &cp, &err);
    if (!nvn_gpu_ok(cr) || !cp) {
        if (cr.code != NVN_GPU_E_STOPPED) nvn_hm_put(&g_mxe_cpipe_bad, &key, (void *)(uintptr_t)1);
        mxe_red(tag, err.why[0] ? err.why : "计算模拟管线创建失败", rec);
        return NULL;
    }
    nvn_hm_put(&g_mxe_cpipes, &key, cp);
    return cp;
}

// 解析任务或网格计算核（st = 3 任务 / 4 网格）的贴图 / 图像 / 缓冲贴图 / 整池快照，随后绑定并派发。
static int mxe_run_cs(nvn_gpu_cmd c, const struct rp_prog *pg, int st, nvn_gpu_pipe cp,
                      uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf ind, uint64_t ind_off, const void *rec) {
    static nvn_gpu_view tv[RP_PROG_ENT_MAX];
    static nvn_gpu_samp ts[RP_PROG_ENT_MAX];
    static tex_t img_snap[RP_PROG_ENT_MAX];
    const tex_t *images[RP_PROG_ENT_MAX];
    memset(tv, 0, sizeof tv);
    memset(ts, 0, sizeof ts);
    memset(images, 0, sizeof images);
    g_ntwb = 0;
    int ok = 0;
    rp_view_pin_begin();
    for (uint32_t i = 0; i < pg->nent[0]; i++) {
        const nvn_gpu_layout_ent *e = &pg->ent[0][i];
        if (e->src_kind == NVN_RB_TBUF) {
            if (!tbuf_prepare(c, e, st, pg->dtt[0][i], pg->dcls[0][i], &tv[i], rec)) goto out;
            continue;
        }
        if (e->src_kind == NVN_RB_IMG && e->cls == NVN_GPU_RES_IMAGE_TBUF) {
            const char *why = image_tbuf_resolve(c, pg, 0, i, st, &tv[i]);
            if (why) { mxe_red(why, "网格计算阶段存储纹素缓冲解析失败", rec); goto out; }
            continue;
        }
        if (e->src_kind == NVN_RB_IMG) {
            const char *why = image_resolve(c, pg, 0, i, st, &tv[i], &img_snap[i]);
            if (why) { mxe_red(why, "网格计算阶段存储图像解析失败", rec); goto out; }
            images[i] = &img_snap[i];
            continue;
        }
        if (e->src_kind == NVN_RB_TEXP || e->src_kind == NVN_RB_SAMPP) {
            const char *why = pool_table_resolve(c, pg, 0, i, pool_row(st));
            if (why) { mxe_red(why, "网格计算阶段整池表解析失败", rec); goto out; }
            continue;
        }
        if (e->src_kind == NVN_RB_TEXU) {
            if (e->count != 1 || e->src_reg2 >= pg->nusrc) { mxe_red("uniform_handle_shape", "网格计算阶段常量句柄形状非法", rec); goto out; }
            int tid = 0, sid = 0;
            const char *bad = usrc_ids(st, &pg->usrc[e->src_reg2], &tid, &sid);
            if (bad) { mxe_red(bad, "网格计算阶段常量句柄未解析", rec); goto out; }
            tv[i] = rp_view_by_ubo_handle_decl(tid, pg->dtt[0][i], pg->dcls[0][i], c, NULL);
            const int const_samp = pg->usrc[e->src_reg2].samp_pool;
            ts[i] = const_samp ? rp_samp_for_const_handle(sid, tid, rp_S.samp_base, rp_S.samp_pool_n) : rp_samp_for_tex(sid, tid);
            if (!ts[i] && const_samp) { mxe_red("const_sampler_unresolved", "网格计算阶段常量采样器未解析", rec); goto out; }
            continue;
        }
        if (e->src_kind != NVN_RB_TEX) continue;
        if (e->count > 1 || e->src_reg >= RP_NTEX || (e->src_reg2 != UINT16_MAX && !samp2_shape_ok(e->src_reg2))) {
            mxe_red("mesh_texture_array_unsupported", "网格计算阶段贴图数组形状不支持", rec);
            goto out;
        }
        const uint64_t h = e->src_reg >= 32 && e->src_reg < 40 ? rp_S.img[st][e->src_reg - 32] : rp_S.tex[st][e->src_reg];
        tv[i] = rp_view_by_id_decl(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, pg->dtt[0][i], pg->dcls[0][i], 0, c, NULL);
        if (e->src_reg2 != UINT16_MAX && (e->src_reg2 & NVN_GPU_SAMP2_CONST)) {
            ts[i] = rp_samp_for_const_handle(const_samp_id((uint32_t)h, e->src_reg2 & NVN_GPU_SAMP2_ID_MASK) + rp_S.samp_base,
                                             h ? (int)(h & 0xFFFFF) : rp_S.tex_base, rp_S.samp_base, rp_S.samp_pool_n);
            if (!ts[i]) { mxe_red("const_sampler_unresolved", "网格计算阶段常量采样器未解析", rec); goto out; }
        } else if (e->src_reg2 != UINT16_MAX) {
            if (!rp_S.samp_bound[st][e->src_reg2]) { mxe_red("separate_sampler_unbound", "网格计算阶段分离采样器未绑定", rec); goto out; }
            ts[i] = rp_samp_for_tex((int)(rp_S.samp[st][e->src_reg2] >> 20), h ? (int)(h & 0xFFFFF) : rp_S.tex_base);
            if (!ts[i]) { mxe_red("separate_sampler_unresolved", "网格计算阶段分离采样器未解析", rec); goto out; }
        } else {
            ts[i] = rp_samp_for_tex(h ? (int)(h >> 20) : rp_S.samp_base, h ? (int)(h & 0xFFFFF) : rp_S.tex_base);
            if (tv[i] && !ts[i] && pg->sampler_unused[0][i]) ts[i] = rp_sampler_for_query();
        }
    }
    nvn_gpu_set_pipe(c, cp);
    if (!bind_stage_at(c, pg, 0, st, 0, tv, ts, rec)) goto out;
    for (uint32_t i = 0; i < pg->nent[0]; i++)
        if (images[i] && (pg->ent[0][i].access & NVN_GPU_ACC_W)) rp_tex_mark_gpu_written(images[i]);
    nvn_gpu_dispatch(c, cp, x, y, z, ind, ind_off);
    texel_wb_flush(c);
    ok = 1;
out:
    g_ntwb = 0;
    for (uint32_t i = 0; i < pg->nent[0]; i++)
        if (texel_tmp(&pg->ent[0][i]) && tv[i]) nvn_gpu_view_release(tv[i]);
    rp_view_pin_end();
    return ok;
}

static uint64_t mxe_align_up(uint64_t v, uint64_t a) { return (v + a - 1u) / a * a; }

static void rp_draw_mesh_tasks(nvn_gpu_cmd c, const p_draw *dr, const void *rec) {
    if (!c || !dr) return;
    if (!dr->ind && dr->count <= 0) return;   // 0 组直接绘制：合法空操作（对齐 iOS）
    if (dr->tfc) { mxe_red("mesh_transform_feedback_count", "网格绘制不支持变换反馈计数", rec); return; }
    if (dr->mdi_cnt) { mxe_red("mesh_indirect_count_unsupported", "网格绘制不支持间接计数多绘", rec); return; }
    if (rp_S.tf_on) { mxe_red("mesh_transform_feedback_active", "网格绘制不支持变换反馈开启态", rec); return; }
    if (rp_S.rast_discard) { mxe_red("mesh_rasterizer_discard", "光栅丢弃态下网格模拟跳过", rec); return; }

    const nvn_gpu_caps *cp = caps_cached();
    if (!(cp->caps & NVN_GPU_CAP_MESH_EMU)) {
        mxe_red("mesh_emu_unsupported", "后端未声明 NVN_GPU_CAP_MESH_EMU（计算模拟辅助核未就绪）", rec);
        return;
    }
    const p_set_rt *rt = rp_pass_target();
    if (!rt || (!rt->n && !rt->depth)) {
        mxe_red("mesh_no_render_target", "未绑定颜色或深度渲染目标", rec);
        return;
    }
    if (!rp_shader_resolve_mesh) {
        mxe_red("mesh_resolver_missing", "未链接 rp_shader_resolve_mesh 着色器来源", rec);
        return;
    }

    struct rp_shader_req q; memset(&q, 0, sizeof q);
    q.prim = DM_PRIM_MESH_TASKS;
    q.rt_count = rt->n;
    for (int i = 0; i < rt->n && i < 8; i++)
        if (rt->color[i]) q.rt_format[i] = (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format);

    const struct rp_mesh_prog *mp = NULL;
    const int srx = rp_shader_resolve_mesh(&q, &mp);
    if (srx > 0) { nvn_unimpl("mesh", "mesh_program_not_ready", rec); g_skips++; return; }
    if (srx < 0 || !mp) { nvn_unimpl("mesh", "mesh_program_unresolved", rec); g_skips++; return; }

    // 限额与完备性校验（逐项对齐 iOS nvn_mtl.m / nvn_geometry.inc，超限具名报红跳过，绝不降级兜底）
    if (!mp->mesh_cs.sh[0] || !mp->raster.sh[0]) {
        mxe_red("mesh_stage_missing", "缺少网格计算核或光栅顶点阶段", rec);
        return;
    }
    if (mp->own_shared_spill) {
        mxe_red("mesh_own_shared_unsupported", "任务 / 网格自带共享内存溢出暂不支持", rec);
        return;
    }
    if (mp->mesh_wants_task && !mp->task_cs.sh[0]) {
        mxe_red("mesh_task_shader_missing", "网格阶段需要任务载荷但未提供任务计算核", rec);
        return;
    }
    const int has_task = mp->mesh_wants_task && mp->task_cs.sh[0] != NULL;
    if (mp->mesh_threads == 0 || mp->mesh_threads > RP_MESH_THREADS_MAX) {
        mxe_red("mesh_threads_over_limit", "网格工作组线程数越界（须在 1..1024）", rec);
        return;
    }
    if (has_task && (mp->task_threads == 0 || mp->task_threads > RP_MESH_THREADS_MAX)) {
        mxe_red("mesh_task_threads_over_limit", "任务工作组线程数越界（须在 1..1024）", rec);
        return;
    }
    if (mp->max_vertices == 0 || mp->max_vertices > RP_MESH_VERTICES_MAX) {
        mxe_red("mesh_max_vertices_over_limit", "网格每组最大顶点数越界（须在 1..256）", rec);
        return;
    }
    if (mp->max_primitives == 0 || mp->max_primitives > RP_MESH_PRIMITIVES_MAX) {
        mxe_red("mesh_max_primitives_over_limit", "网格每组最大图元数越界（须在 1..512）", rec);
        return;
    }
    if (mp->has_relay && !mp->relay_slice_words) {
        mxe_red("mesh_relay_slice_missing", "中转载荷网格缺少阶段内存片字数", rec);
        return;
    }
    if (has_task && mp->task_payload_bytes > RP_MESH_PAYLOAD_MAX && !mp->has_relay) {
        mxe_red("mesh_payload_over_limit", "任务载荷超过 16384 字节且未启用中转载荷", rec);
        return;
    }
    if (mp->vertex_stride == 0 || (mp->vertex_stride & 15u)) {
        mxe_red("mesh_vertex_stride_invalid", "网格顶点记录步长非法（须为非零 16 字节倍数）", rec);
        return;
    }
    if (dr->ind && (dr->ind & 3u)) {
        mxe_red("mesh_indirect_unaligned", "间接网格参数地址未 4 字节对齐", rec);
        return;
    }

    // 计算交换缓冲预算与布局（与 iOS nvn_geometry.inc mtk_emu_prep 同布局，且各子区间满足 Vulkan UBO/SSBO 对齐）
    int budget_mb = nvn_plat_flag("nvk_mesh_emu_mb", 32);
    if (budget_mb < 1) budget_mb = 1;
    uint64_t budget = (uint64_t)budget_mb << 20;
    if (cp->max_ssbo_range && budget > cp->max_ssbo_range) budget = cp->max_ssbo_range;
    uint32_t align = 256u;
    if (cp->min_ssbo_align > align) align = cp->min_ssbo_align;
    if (cp->min_ubo_align > align) align = cp->min_ubo_align;
    const uint64_t align_w = (uint64_t)align >> 2;

    const uint32_t maxv = mp->expanded_verts ? mp->max_primitives * 3u : mp->max_vertices;
    const uint32_t pw = has_task ? ((mp->task_payload_bytes + 3u) / 4u ? (mp->task_payload_bytes + 3u) / 4u : 1u) : 0u;
    const uint64_t sw = mp->has_relay ? (uint64_t)mp->relay_slice_words : 0ull;
    const uint64_t per_mesh_bytes = (uint64_t)maxv * mp->vertex_stride + (uint64_t)mp->max_primitives * 12ull + sw * 4ull;
    if (!per_mesh_bytes || per_mesh_bytes > budget) {
        mxe_red("mesh_exchange_budget_exceeded", "单个网格工作组所需交换空间超过预算上限", rec);
        return;
    }

    uint32_t capT = 0;
    if (has_task) {
        if (dr->ind) capT = 64u;
        else capT = (uint32_t)dr->count > RP_MESH_DIRECT_CAP_MAX ? RP_MESH_DIRECT_CAP_MAX : (uint32_t)dr->count;
        if (capT > RP_MESH_TASK_CAP_MAX && dr->ind) capT = RP_MESH_TASK_CAP_MAX;
    }
    uint32_t capM = 0;
    if (has_task) {
        uint64_t want = (uint64_t)capT * 32ull;
        if (!want) want = 1ull;
        if (want > RP_MESH_TASK_CAP_MAX) want = RP_MESH_TASK_CAP_MAX;
        capM = (uint32_t)want;
    } else if (dr->ind) {
        capM = 64u;
    } else {
        capM = (uint32_t)dr->count > RP_MESH_DIRECT_CAP_MAX ? RP_MESH_DIRECT_CAP_MAX : (uint32_t)dr->count;
        if (!capM) capM = 1u;
    }

    const uint64_t counts_w = NVN_GPU_MESH_EMU_HEAD;
    const uint64_t payload_w = counts_w + (uint64_t)capT;
    const uint64_t table_w = payload_w + (uint64_t)capT * (uint64_t)pw;
    const uint64_t fixed_head_bytes = (table_w + align_w * 2ull + 16ull) * 4ull;
    if (fixed_head_bytes >= budget) {
        mxe_red("mesh_exchange_budget_exceeded", "任务计数 / 载荷区超过交换缓冲预算", rec);
        return;
    }
    const uint64_t max_capM = (budget - fixed_head_bytes) / (per_mesh_bytes + 4ull);
    if (!max_capM) {
        mxe_red("mesh_exchange_budget_exceeded", "剩余交换缓冲不足以容纳 1 个网格工作组", rec);
        return;
    }
    if ((uint64_t)capM > max_capM) capM = (uint32_t)max_capM;

    const uint64_t first_w = mxe_align_up(table_w + (uint64_t)capM, align_w);
    const uint64_t vbo_w = mxe_align_up(first_w + 4ull, align_w);
    const uint64_t vbo_words = (uint64_t)capM * (uint64_t)maxv * ((uint64_t)mp->vertex_stride >> 2);
    const uint64_t ibo_w = (vbo_w + vbo_words + 3ull) & ~3ull;
    const uint64_t ibo_words = (uint64_t)capM * (uint64_t)mp->max_primitives * 3ull;
    const uint64_t sl_w = (ibo_w + ibo_words + 3ull) & ~3ull;
    const uint64_t total_words = sl_w + (uint64_t)capM * sw;
    const uint64_t total_bytes = total_words * 4ull;
    if (total_words > UINT32_MAX || total_bytes > budget || (cp->max_ssbo_range && total_bytes > cp->max_ssbo_range)) {
        mxe_red("mesh_exchange_budget_exceeded", "交换缓冲总长超出预算或 maxStorageBufferRange", rec);
        return;
    }

    // 结束当前渲染通道再解析间接缓冲与运行计算阶段
    rp_pass_flush(c);

    nvn_gpu_buf ind_buf = NULL;
    uint64_t ind_off = 0;
    if (dr->ind) {
        if (!rp_buf_resolve(c, dr->ind, 8u, &ind_buf, &ind_off) || !ind_buf) {
            mxe_red("mesh_indirect_unresolved", "间接网格参数地址不在已登记内存池中", rec);
            return;
        }
        if (ind_off & 3u) {
            mxe_red("mesh_indirect_unaligned", "间接网格参数镜像偏移未 4 字节对齐", rec);
            return;
        }
    }

    nvn_gpu_pipe cp_task = NULL;
    if (has_task) {
        cp_task = mxe_cpipe_get(mp->task_cs.sh[0], "mesh_task_pipeline_failed", rec);
        if (!cp_task) return;
    }
    nvn_gpu_pipe cp_mesh = mxe_cpipe_get(mp->mesh_cs.sh[0], "mesh_compute_pipeline_failed", rec);
    if (!cp_mesh) return;

    nvn_gpu_slice xsl = {0};
    nvn_gpu_err aerr = {0};
    nvn_gpu_rc arc = nvn_gpu_arena_alloc(c, total_bytes, align, &xsl, &aerr);
    if (!nvn_gpu_ok(arc) || !xsl.cpu) {
        arena_fail_detail(arc, &aerr, total_bytes);
        mxe_red("mesh_exchange_alloc_failed", aerr.why[0] ? aerr.why : "交换缓冲分配失败", rec);
        return;
    }

    // 初始化交换缓冲头（32 字，与 iOS nvn_geometry.inc 及 backend_vulkan_mesh_emu_spv.h 逐字对齐）
    uint32_t *h = (uint32_t *)(void *)xsl.cpu;
    memset(h, 0, NVN_GPU_MESH_EMU_HEAD * 4u);
    h[0] = (uint32_t)counts_w;
    h[1] = (uint32_t)payload_w;
    h[2] = (uint32_t)table_w;
    h[3] = capT;
    h[4] = capM;
    h[5] = pw;
    h[6] = dr->ind ? 0u : (uint32_t)dr->count;
    h[7] = mp->max_primitives * 3u;
    h[11] = (uint32_t)vbo_w;
    h[12] = (uint32_t)ibo_w;
    h[23] = sw ? (uint32_t)sl_w : 0u;
    h[29] = dr->ind ? 0u : (uint32_t)dr->first;
    h[30] = (uint32_t)first_w;
    h[first_w + 0] = h[29];
    h[first_w + 1] = 0u;
    h[first_w + 2] = 0u;
    h[first_w + 3] = 0u;
    if (capT) memset(h + counts_w, 0, (size_t)capT * 4u);
    if (!has_task && !dr->ind) {
        uint32_t mcnt = (uint32_t)dr->count < capM ? (uint32_t)dr->count : capM;
        h[8] = mcnt; h[9] = 1u; h[10] = 1u;
        h[13] = 0u; h[14] = 1u; h[15] = 0u; h[16] = 0u; h[17] = 0u;
        h[18] = (uint32_t)dr->count; h[19] = 0u;
    }

    // 挂接交换缓冲上下文，依次执行任务 / 排表 / 网格计算阶段
    g_mxe_bind.active = 1;
    g_mxe_bind.xbuf = xsl.buf;
    g_mxe_bind.xoff = xsl.off;
    g_mxe_bind.xbytes = total_bytes;
    g_mxe_bind.vbo_off = vbo_w * 4ull;
    g_mxe_bind.vbo_bytes = vbo_words * 4ull;
    g_mxe_bind.first_off = first_w * 4ull;

    g_pipe = NULL; g_pipe_cmd = NULL; rp_dirty_all();
    uint32_t bar = rp_pass_take_barrier();
    if (bar) { nvn_gpu_barrier(c, bar); g_compute_access.n = 0; }

    int cs_ok = 0;
    if (has_task) {
        if (dr->ind) {
            nvn_gpu_mesh_emu_op(c, NVN_GPU_MESH_EMU_TCLAMP, xsl.buf, xsl.off, total_bytes, ind_buf, ind_off);
            if (!mxe_run_cs(c, &mp->task_cs, 3, cp_task, 0, 0, 0, xsl.buf, xsl.off + 20ull * 4ull, rec)) goto mesh_fail;
            nvn_gpu_mesh_emu_op(c, NVN_GPU_MESH_EMU_SCAN, xsl.buf, xsl.off, total_bytes, ind_buf, ind_off);
        } else {
            if (!mxe_run_cs(c, &mp->task_cs, 3, cp_task, capT, 1u, 1u, NULL, 0, rec)) goto mesh_fail;
            nvn_gpu_mesh_emu_op(c, NVN_GPU_MESH_EMU_SCAN, xsl.buf, xsl.off, total_bytes, NULL, 0);
        }
        if (!mxe_run_cs(c, &mp->mesh_cs, 4, cp_mesh, 0, 0, 0, xsl.buf, xsl.off + 8ull * 4ull, rec)) goto mesh_fail;
    } else {
        if (dr->ind) {
            nvn_gpu_mesh_emu_op(c, NVN_GPU_MESH_EMU_MONLY, xsl.buf, xsl.off, total_bytes, ind_buf, ind_off);
            if (!mxe_run_cs(c, &mp->mesh_cs, 4, cp_mesh, 0, 0, 0, xsl.buf, xsl.off + 8ull * 4ull, rec)) goto mesh_fail;
        } else {
            if (!mxe_run_cs(c, &mp->mesh_cs, 4, cp_mesh, h[8], 1u, 1u, NULL, 0, rec)) goto mesh_fail;
        }
    }
    cs_ok = 1;

    // 计算写出的顶点 / 索引 / 间接绘制命令对图形光栅阶段可见
    nvn_gpu_compute_to_graphics(c);

    // 准备光栅化阶段（VS = main0_mv，FS = 游戏片元）的贴图 / 图像 / 存储缓冲绑定
    const struct rp_prog *saved_prog = g_prog;
    g_prog = &mp->raster;
    memset(g_sb_ok, 0, sizeof g_sb_ok);
    memset(g_ub_ok, 0, sizeof g_ub_ok);
    for (int st = 0; st < 2; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
        const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
        if (e->src_kind == NVN_RB_SBUFP || e->src_kind == NVN_RB_UBOP) {
            const p_bind_buf *sb = e->src_reg < 16 ? (e->src_kind == NVN_RB_UBOP ? &rp_S.ubo[st][e->src_reg] : &rp_S.ssbo[st][e->src_reg]) : NULL;
            const int sbufp = e->src_kind == NVN_RB_SBUFP;
            if (!sb || !sb_len(sb, sbufp)) continue;
            uint64_t wa, wl; (void)sb_window(sb, sbufp, &wa, &wl);
            if (rp_buf_resolve(c, wa, wl, &g_sb_buf[st][i], &g_sb_off[st][i])) g_sb_ok[st][i] = 1;
        } else if (e->src_kind == NVN_RB_CBUF && e->src_reg < 16) {
            const p_bind_buf *ub = &rp_S.ubo[st][e->src_reg];
            if (ub->addr && ub->size) {
                const uint64_t ulen = cbuf_len(ub);
                if (rp_pool_find(ub->addr, ulen, NULL, NULL) && rp_buf_gpu_pending(ub->addr, ulen) &&
                    rp_buf_resolve(c, ub->addr, ulen, &g_ub_buf[st][i], &g_ub_off[st][i]))
                    g_ub_ok[st][i] = 1;
            }
        }
    }

    static nvn_gpu_view tv[2][RP_PROG_ENT_MAX];
    static nvn_gpu_samp ts[2][RP_PROG_ENT_MAX];
    memset(tv, 0, sizeof tv);
    memset(ts, 0, sizeof ts);
    memset(g_gimg, 0, sizeof g_gimg);
    int narr[2] = {0, 0};
    rp_view_pin_begin();
    for (int st = 0; st < 2; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
        const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
        if (e->src_kind == NVN_RB_TBUF) {
            if (!tbuf_prepare(c, e, st, g_prog->dtt[st][i], g_prog->dcls[st][i], &tv[st][i], rec)) goto raster_done;
            continue;
        }
        if (e->src_kind == NVN_RB_TEXU) {
            if (e->count != 1 || e->src_reg2 >= g_prog->nusrc) { mxe_red("uniform_handle_shape", "网格光栅阶段常量句柄非法", rec); goto raster_done; }
            int tid = 0, sid = 0;
            const char *bad = usrc_ids(st, &g_prog->usrc[e->src_reg2], &tid, &sid);
            if (bad) { mxe_red(bad, "网格光栅阶段常量句柄未解析", rec); goto raster_done; }
            tv[st][i] = rp_view_by_ubo_handle_decl(tid, g_prog->dtt[st][i], g_prog->dcls[st][i], c, NULL);
            const int const_samp = g_prog->usrc[e->src_reg2].samp_pool;
            ts[st][i] = const_samp ? rp_samp_for_const_handle(sid, tid, rp_S.samp_base, rp_S.samp_pool_n) : rp_samp_for_tex(sid, tid);
            if (!ts[st][i] && const_samp) { mxe_red("const_sampler_unresolved", "网格光栅阶段常量采样器未解析", rec); goto raster_done; }
            continue;
        }
        if (e->src_kind == NVN_RB_TEXP || e->src_kind == NVN_RB_SAMPP) {
            const char *why = pool_table_resolve(c, g_prog, st, i, pool_row(st));
            if (why) { mxe_red(why, "网格光栅阶段整池表未解析", rec); goto raster_done; }
            continue;
        }
        if (e->src_kind == NVN_RB_IMG && e->cls == NVN_GPU_RES_IMAGE_TBUF) {
            const char *why = image_tbuf_resolve(c, g_prog, st, i, st, &tv[st][i]);
            if (why) { mxe_red(why, "网格光栅阶段存储纹素缓冲未解析", rec); goto raster_done; }
            continue;
        }
        if (e->src_kind == NVN_RB_IMG) {
            const char *why = image_resolve(c, g_prog, st, i, st, &tv[st][i], &g_gimg_snap[st][i]);
            if (why) { mxe_red(why, "网格光栅阶段存储图像未解析", rec); goto raster_done; }
            g_gimg[st][i] = &g_gimg_snap[st][i];
            continue;
        }
        if (e->src_kind != NVN_RB_TEX) continue;
        if (e->src_reg >= RP_NTEX || (e->src_reg2 != UINT16_MAX && e->count != 1) || !samp2_shape_ok(e->src_reg2)) {
            mxe_red("separate_texture_shape", "网格光栅阶段分离贴图形状非法", rec); goto raster_done;
        }
        if (e->count > 1) {
            if (e->count > RP_TEXARR_MAX || narr[st]++) { mxe_red("texture_array_unsupported_shape", "网格光栅阶段贴图数组不支持", rec); goto raster_done; }
            for (uint32_t k = 0; k < e->count; k++) {
                int slot = (int)(e->src_reg + k) - RP_HANDLE_TABLE_FIRST_SLOT;
                uint64_t hk = slot >= 0 && slot < 32 ? rp_S.tex[st][slot] : 0;
                g_av[st][k] = rp_view_by_id_decl(hk ? (int)(hk & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 1, c, NULL);
                g_as[st][k] = rp_samp_for_tex(hk ? (int)(hk >> 20) : rp_S.samp_base, hk ? (int)(hk & 0xFFFFF) : rp_S.tex_base);
                if (hk && !g_as[st][k]) { mxe_red("sampler_slot_unresolved", "网格光栅阶段采样器未解析", rec); goto raster_done; }
            }
            continue;
        }
        uint64_t htex = graphics_tex_handle(st, e->src_reg);
        tv[st][i] = rp_view_by_id_decl(htex ? (int)(htex & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 0, c, NULL);
        if (e->src_reg2 != UINT16_MAX && (e->src_reg2 & NVN_GPU_SAMP2_CONST)) {
            ts[st][i] = rp_samp_for_const_handle(const_samp_id((uint32_t)htex, e->src_reg2 & NVN_GPU_SAMP2_ID_MASK) + rp_S.samp_base,
                                                 htex ? (int)(htex & 0xFFFFF) : rp_S.tex_base, rp_S.samp_base, rp_S.samp_pool_n);
            if (!ts[st][i]) { mxe_red("const_sampler_unresolved", "网格光栅阶段常量采样器未解析", rec); goto raster_done; }
        } else if (e->src_reg2 != UINT16_MAX) {
            if (!rp_S.samp_bound[st][e->src_reg2]) { mxe_red("separate_sampler_unbound", "网格光栅阶段分离采样器未绑定", rec); goto raster_done; }
            ts[st][i] = rp_samp_for_tex((int)(rp_S.samp[st][e->src_reg2] >> 20), htex ? (int)(htex & 0xFFFFF) : rp_S.tex_base);
            if (!ts[st][i]) { mxe_red("separate_sampler_unresolved", "网格光栅阶段分离采样器未解析", rec); goto raster_done; }
        } else {
            ts[st][i] = rp_samp_for_tex(htex ? (int)(htex >> 20) : rp_S.samp_base, htex ? (int)(htex & 0xFFFFF) : rp_S.tex_base);
        }
    }

    if (rp_pass_parked()) {
        static rp_pass_read rd[2 * (RP_PROG_ENT_MAX + RP_TEXARR_MAX)];
        int nr = 0;
        for (int st = 0; st < 2; st++)
            for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
                const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
                if (e->src_kind == NVN_RB_TEXP) { rd[nr++] = (rp_pass_read){.pool = g_ptd[st][i]}; continue; }
                if (e->src_kind == NVN_RB_TEX && e->count > 1) {
                    for (uint32_t k = 0; k < e->count; k++) {
                        nvn_gpu_tex tex = nvn_gpu_view_texture(g_av[st][k]);
                        if (tex) rd[nr++] = (rp_pass_read){.tex = tex};
                    }
                } else if (tv[st][i]) {
                    nvn_gpu_tex tex = nvn_gpu_view_texture(tv[st][i]);
                    if (tex) rd[nr++] = (rp_pass_read){.tex = tex};
                }
            }
        rp_pass_land_reads(c, rd, nr);
    }

    uint32_t color_write_mask = rp_S.chmask.mask & (g_prog->color_out_mask ? g_prog->color_out_mask : 0xffffffffu);
    rp_pass_want_layered(c, g_prog->layer_out);
    rp_pass_prepare_draw(c, color_write_mask);
    if (!rp_pass_begin(c)) { mxe_red("mesh_pass_begin_failed", "网格光栅通道开启失败", rec); goto raster_done; }

    const int clip_neg1 = !rp_depth_zero() && (cp->caps & NVN_GPU_CAP_DEPTH_CLIP_CTL);
    const struct rp_vertex_decode decoded = {0};
    nvn_gpu_pipe_desc pd = replay_pipe_desc(rt, 4u /* TRIANGLES */, 1 /* no prim_restart */, 0, 0,
                                           &decoded, clip_neg1, color_write_mask, 0, rec);
    // 光栅顶点着色器 main0_mv 从 NVN_RB_MESH_VBO 拉取记录，不读取游戏顶点属性流
    memset(pd.attr, 0, sizeof pd.attr);
    memset(pd.stream, 0, sizeof pd.stream);
    pd.st[2].sh = NULL;

    nvn_gpu_pipe gp = NULL;
    nvn_gpu_err perr = {0};
    nvn_gpu_rc prc = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &gp, &perr);
    if (!nvn_gpu_ok(prc) || !gp) {
        mxe_red("mesh_raster_pipe_failed", perr.why[0] ? perr.why : "网格光栅管线创建失败", rec);
        goto raster_done;
    }
    nvn_gpu_set_pipe(c, gp);

    uint32_t tw = 1u, th = 1u;
    {
        int fi = -1;
        for (int i = 0; i < rt->n && i < 8 && fi < 0; i++) if (rt->color[i]) fi = i;
        if (fi >= 0) rp_pass_slot_size(fi, &tw, &th);
        else if (rt->depth) {
            uint32_t lv = rt->has_dview && rt->dview.set_levels ? rt->dview.base_level : 0;
            tw = rt->depth->d.width >> lv; th = rt->depth->d.height >> lv;
        }
        rp_pass_noatt_size(&tw, &th);
        if (!tw) tw = 1u;
        if (!th) th = 1u;
    }
    nvn_gpu_dyn dyn;
    if (!fill_dyn(&dyn, tw, th)) goto raster_done;
    nvn_gpu_set_dyn(c, &dyn);

    for (int st = 0; st < 2; st++)
        if (!bind_stage(c, g_prog, st, 0, tv[st], ts[st], rec)) goto raster_done;
    // 清空几何阶段绑定槽，避免残留绑定干扰布局
    nvn_gpu_bind(c, NVN_GPU_SH_GS, NULL, 0);

    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4u; /* NVN_DRAW_PRIMITIVE_TRIANGLES */
    da.ibuf = xsl.buf;
    da.ioff = xsl.off + ibo_w * 4ull;
    da.itype = 2u; /* uint32 索引 */
    da.ind = xsl.buf;
    da.ind_off = xsl.off + 13ull * 4ull;   // h[13..17] = VkDrawIndexedIndirectCommand
    da.max_draws = 1u;
    da.stride = 20u;
    rp_occl_draw(c);
    nvn_gpu_draw(c, &da);

    for (int st = 0; st < 2; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++)
        if (g_gimg[st][i] && (g_prog->ent[st][i].access & NVN_GPU_ACC_W)) rp_tex_mark_gpu_written(g_gimg[st][i]);
    rp_pass_note_draw((rp_S.ds.dt || rp_S.ds.dw || rp_S.ds.st ? RP_NOTE_DS : 0) | (prog_shader_writes(g_prog) ? RP_NOTE_SHADER_WRITE : 0));
    g_draws++;

raster_done:
    for (int st = 0; st < 2; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++)
        if (texel_tmp(&g_prog->ent[st][i]) && tv[st][i]) nvn_gpu_view_release(tv[st][i]);
    rp_view_pin_end();
    g_prog = saved_prog;
mesh_fail:
    (void)cs_ok;
    memset(&g_mxe_bind, 0, sizeof g_mxe_bind);
    g_pipe = NULL; g_pipe_cmd = NULL; rp_dirty_all();
    for (int st = 0; st < RP_PROG_STAGES; st++) g_lb[st].ok = 0;
}
