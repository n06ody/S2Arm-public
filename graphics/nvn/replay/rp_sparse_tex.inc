// SPDX-License-Identifier: Apache-2.0
// 虚拟池采样源的驻留快照。几何和页表沿用 NVN，分配与稀疏块绑定由后端处理。
enum { SPARSE_GUEST_PAGE = 65536, SPARSE_UPLOAD_MAX = 256 << 20 };
static uint32_t sparse_mip(uint32_t n, uint32_t level) { n >>= level; return n ? n : 1; }

static int sparse_snapshot_candidate(const tex_t *t, int paged) {
    if (!paged || !(t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) || !nvn_sparse_map_any_bits ||
        !nvn_gpu_tex_sparse_create || !nvn_gpu_tex_sparse_upload ||
        (t->d.target != NVN_TEXTURE_TARGET_2D && t->d.target != NVN_TEXTURE_TARGET_2D_ARRAY)) return 0;
    return 1;
}

static int sparse_native_candidate(const tex_t *t, int paged) {
    if (!sparse_snapshot_candidate(t, paged)) return 0;
    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    if (!(caps.caps & NVN_GPU_CAP_TEX_SPARSE)) return 0;
    const int min_mb = nvn_plat_flag("nvk_sparse_native_mb", 64);
    return min_mb >= 0 && t->L.size >= (uint64_t)min_mb << 20;
}

// 返回正值表示快照已录，0 表示能力或块几何不符（仅允许有界兼容），负值表示构建或录制失败。
static int sparse_snapshot(const tex_t *t, struct rp_tex *x, nvn_gpu_cmd c, const nvn_gpu_tex_desc *d,
                           nvn_gpu_err *err) {
    uint32_t tile[4];
    nvn_tex_sparse_layout(&t->d, tile);
    const uint32_t levels = t->d.levels ? t->d.levels : 1, layers = t->L.layers;
    if (!tile[0] || !tile[1] || !tile[2] || !layers || levels > 32) return 0;
    const uint32_t tiled = tile[0] < levels ? tile[0] : levels;
    int bw = 1, bh = 1;
    const uint32_t bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || tile[1] % bw || tile[2] % bh) return 0;
    uint64_t offs[33] = {0}, jobs_per_layer = 0, max_pages = 0;
    uint32_t cols[32], rows[32];
    for (uint32_t l = 0; l < levels; l++) {
        offs[l] = nvn_tex_level_offset(&t->d, l);
        cols[l] = l < tiled ? (sparse_mip(t->d.width, l) - 1) / tile[1] + 1 : 1;
        rows[l] = l < tiled ? (sparse_mip(t->d.height, l) - 1) / tile[2] + 1 : 1;
        const uint64_t n = (uint64_t)cols[l] * rows[l];
        if (l < tiled && n > max_pages) max_pages = n;
        jobs_per_layer += n;
    }
    offs[levels] = t->L.layer_size;
    for (uint32_t l = 0; l < tiled; l++) {
        unsigned th = 0, td = 0;
        nvn_tex_level_tiles(&t->d, l, &th, &td);
        const uint64_t pages = (uint64_t)cols[l] * rows[l];
        if (td || tile[2] / bh != (8u << th) ||
            (uint64_t)nvn_tex_level_gobs(&t->d, l) * 64 != (uint64_t)cols[l] * (tile[1] / bw) * bpb ||
            offs[l + 1] - offs[l] != pages * SPARSE_GUEST_PAGE) return 0;
    }
    if (!jobs_per_layer || jobs_per_layer > UINT32_MAX / layers) return 0;
    const uint32_t njobs = (uint32_t)jobs_per_layer * layers;
    nvn_gpu_sparse_region *jobs = calloc(njobs, sizeof *jobs);
    uint8_t *bits = malloc((size_t)((max_pages + 7) / 8));
    if (!jobs || !bits) { free(jobs); free(bits); err->rc.code = NVN_GPU_E_OOM; return -1; }

    const uint64_t base = (uint64_t)(uintptr_t)t->mem, vg = nvn_sparse_gen();
    nvn_sparse_protect(t->pool, base, t->L.size);
    const uint64_t stamp = nvn_dirty_now();
    uint32_t i = 0;
    for (uint32_t y = 0; y < layers; y++) for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = sparse_mip(t->d.width, l), h = sparse_mip(t->d.height, l);
        const uint64_t start = base + (uint64_t)y * t->L.layer_size + offs[l];
        if (l < tiled)
            nvn_sparse_map_any_bits(t->pool, start, (uint64_t)cols[l] * rows[l] * SPARSE_GUEST_PAGE, bits);
        for (uint32_t py = 0; py < rows[l]; py++) for (uint32_t px = 0; px < cols[l]; px++) {
            const uint64_t p = (uint64_t)py * cols[l] + px;
            const uint32_t x0 = px * tile[1], y0 = py * tile[2];
            const uint32_t cw = l >= tiled || w - x0 < tile[1] ? w - x0 : tile[1];
            const uint32_t ch = l >= tiled || h - y0 < tile[2] ? h - y0 : tile[2];
            const int mapped = l >= tiled || ((bits[p / 8] >> (p % 8)) & 1);
            jobs[i++] = (nvn_gpu_sparse_region){
                .level = l, .layer = y, .region = {x0, y0, 0, cw, ch, 1},
                .resident = mapped, .upload = mapped,
            };
        }
    }
    free(bits);

    nvn_gpu_tex fresh = NULL;
    nvn_gpu_rc rc = nvn_gpu_tex_sparse_create(c, d, jobs, njobs, x->sparse_native, &fresh, err);
    if (!nvn_gpu_ok(rc)) { free(jobs); return rc.code == NVN_GPU_E_UNSUPPORTED ? 0 : -1; }

    uint8_t *raw = NULL, *linear = NULL;
    size_t raw_cap = 0, linear_cap = 0;
    int ok = 1;
    const int retain = x->uploaded && x->g && nvn_sparse_since && !nvn_dirty_tracking_unreliable() &&
        x->sparse_upload_epoch == tex_bytes_failure_epoch();
    for (i = 0; i < njobs; i++) {
        const nvn_gpu_sparse_region *j = &jobs[i];
        if (!j->upload) continue;
        const uint32_t l = j->level;
        const int page = l < tiled;
        const uint32_t w = page ? tile[1] : (uint32_t)j->region.width;
        const uint32_t h = page ? tile[2] : (uint32_t)j->region.height;
        const uint64_t pitch = ((uint64_t)w + bw - 1) / bw * bpb;
        const uint64_t size = pitch * (((uint64_t)h + bh - 1) / bh);
        const uint64_t n = page ? SPARSE_GUEST_PAGE : offs[l + 1] - offs[l];
        if (!n || n > SPARSE_UPLOAD_MAX || size > SPARSE_UPLOAD_MAX) { err->rc.code = NVN_GPU_E_OOM; ok = 0; break; }
        const uint64_t page_id = (uint64_t)(j->region.yoffset / tile[2]) * cols[l] + j->region.xoffset / tile[1];
        const uint64_t addr = base + (uint64_t)j->layer * t->L.layer_size + offs[l] + page_id * SPARSE_GUEST_PAGE;
        // 映射和物理内容均未变的区域可能含有 GPU 的新值，从原图保留。
        if (retain && j->resident && !nvn_sparse_since(t->pool, addr, n, x->vgen, x->stamp)) {
            nvn_gpu_copy_tex(c, x->g, l, j->layer, &j->region, fresh, l, j->layer, &j->region);
            if (nvn_gpu_cmd_record_ok && !nvn_gpu_cmd_record_ok(c)) { err->rc.code = NVN_GPU_E_DEVICE; ok = 0; break; }
            continue;
        }
        if (n > raw_cap) {
            uint8_t *p = realloc(raw, (size_t)n);
            if (!p) { err->rc.code = NVN_GPU_E_OOM; ok = 0; break; }
            raw = p; raw_cap = (size_t)n;
        }
        if (size > linear_cap) {
            uint8_t *p = realloc(linear, (size_t)size);
            if (!p) { err->rc.code = NVN_GPU_E_OOM; ok = 0; break; }
            linear = p; linear_cap = (size_t)size;
        }
        if (j->resident) {
            sparse_fill(t, addr, raw, n);
            nvn_texdesc pd = t->d;
            if (page) {
                unsigned th, td;
                nvn_tex_level_tiles(&t->d, l, &th, &td);
                pd = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = t->d.format, .width = w, .height = h,
                    .depth = 1, .levels = 1, .pkg_layout = (int)(th | td << 4) + 1};
            }
            if (nvn_deswizzle_level(&pd, page ? 0 : l, raw, linear, (size_t)pitch)) {
                err->rc.code = NVN_GPU_E_ARG; ok = 0; break;
            }
        } else memset(linear, 0, (size_t)size);

        rc = nvn_gpu_tex_sparse_upload(c, fresh, j, linear, pitch, size);
        if (!nvn_gpu_ok(rc)) { err->rc = rc; ok = 0; break; }
    }
    free(raw); free(linear); free(jobs);
    if (!ok) { nvn_gpu_tex_release(fresh); return -1; }

    tex_replace_storage(x, fresh);
    x->uploaded = 1;
    x->vgen = vg;
    x->stamp = stamp;
    x->nruns = 0;
    x->sparse_upload_epoch = tex_bytes_failure_epoch();
    g_uploads++;
    note_tex_upload(x);
    return 1;
}
