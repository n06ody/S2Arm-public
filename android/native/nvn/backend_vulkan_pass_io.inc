// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// Count the attachment operations actually recorded by vkCmdBeginRenderPass.
// Bytes are logical texel payload (pixels × layers × samples × component
// bytes), independent of tile residency, compression, padding and traffic.
enum pass_io_reason {
    PASS_IO_DRAW, PASS_IO_CLEAR, PASS_IO_XFER, PASS_IO_AREA_REOPEN,
    PASS_IO_LAYOUT_REOPEN, PASS_IO_FEEDBACK_COPY, PASS_IO_FEEDBACK_READ, PASS_IO_IDLE_DS,
    PASS_IO_EASU, PASS_IO_CLEAR_RECT, PASS_IO_DEPTH_BLIT, PASS_IO_TERMINAL, PASS_IO_REASON_N
};
enum pass_io_aspect { PASS_IO_COLOR, PASS_IO_DEPTH, PASS_IO_STENCIL, PASS_IO_ASPECT_N };
enum pass_io_op { PASS_IO_LOAD, PASS_IO_CLEAR_LOAD, PASS_IO_DISCARD_LOAD, PASS_IO_NONE_LOAD,
                  PASS_IO_STORE, PASS_IO_DISCARD_STORE, PASS_IO_NONE_STORE, PASS_IO_OP_N };

static struct {
    unsigned passes;
    struct { uint64_t count[PASS_IO_OP_N], bytes[PASS_IO_OP_N], unknown_formats; } cell[PASS_IO_REASON_N][PASS_IO_ASPECT_N];
} pass_io;
static pthread_mutex_t pass_io_mu = PTHREAD_MUTEX_INITIALIZER;

static void pass_io_report_locked(void) {
    static const char *const reasons[] = {
        "draw", "clear-only", "replay-xfer", "area-reopen", "layout-reopen",
        "feedback-copy", "feedback-read", "idle-ds", "easu", "clear-rect", "depth-blit", "terminal-discard"
    };
    static const char *const aspects[] = {"color", "depth", "stencil"};
    if (!pass_io.passes) return;
    for (unsigned r = 0; r < PASS_IO_REASON_N; r++) for (unsigned a = 0; a < PASS_IO_ASPECT_N; a++) {
        const uint64_t *n = pass_io.cell[r][a].count, *b = pass_io.cell[r][a].bytes;
        if (!(n[PASS_IO_LOAD] || n[PASS_IO_CLEAR_LOAD] || n[PASS_IO_DISCARD_LOAD] || n[PASS_IO_NONE_LOAD])) continue;
        char line[520];
        snprintf(line, sizeof line, "vk-pass-io passes=%u reason=%s aspect=%s unit=count/logical_bytes"
                 " load=%llu/%llu clear=%llu/%llu load_discard=%llu/%llu load_none=%llu/%llu"
                 " store=%llu/%llu store_discard=%llu/%llu store_none=%llu/%llu unknown_formats=%llu",
                 pass_io.passes, reasons[r], aspects[a],
                 (unsigned long long)n[PASS_IO_LOAD], (unsigned long long)b[PASS_IO_LOAD],
                 (unsigned long long)n[PASS_IO_CLEAR_LOAD], (unsigned long long)b[PASS_IO_CLEAR_LOAD],
                 (unsigned long long)n[PASS_IO_DISCARD_LOAD], (unsigned long long)b[PASS_IO_DISCARD_LOAD],
                 (unsigned long long)n[PASS_IO_NONE_LOAD], (unsigned long long)b[PASS_IO_NONE_LOAD],
                 (unsigned long long)n[PASS_IO_STORE], (unsigned long long)b[PASS_IO_STORE],
                 (unsigned long long)n[PASS_IO_DISCARD_STORE], (unsigned long long)b[PASS_IO_DISCARD_STORE],
                 (unsigned long long)n[PASS_IO_NONE_STORE], (unsigned long long)b[PASS_IO_NONE_STORE],
                 (unsigned long long)pass_io.cell[r][a].unknown_formats);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass", line);
    }
    memset(&pass_io, 0, sizeof pass_io);
}

static uint32_t pass_io_texel_bytes(VkFormat format, uint32_t nvn, enum pass_io_aspect aspect) {
    if (aspect == PASS_IO_DEPTH) {
        switch (format) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: return 2;
        case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: return 3;
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
        default: return 0;
        }
    }
    nvn_fmt_info info;
    if (nvn_vk_format(nvn) == format && nvn_fmt_info_of(nvn, &info) && info.bw == 1 && info.bh == 1)
        return info.bytes;
    uint32_t expanded;
    int expand_bytes;
    if (nvn_fmt_expand_target(nvn, &expanded, &expand_bytes) && nvn_vk_format(expanded) == format)
        return (uint32_t)expand_bytes;
    uint32_t bytes;
    int cls;
    if (nvn_vk_format_override_class(format, &bytes, &cls)) return bytes;
    if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) return 4;
    return 0;
}

static void pass_io_add(enum pass_io_reason reason, enum pass_io_aspect aspect, uint64_t samples,
                        uint32_t bytes, VkAttachmentLoadOp load, VkAttachmentStoreOp store) {
    const enum pass_io_op l = load == VK_ATTACHMENT_LOAD_OP_LOAD ? PASS_IO_LOAD :
        load == VK_ATTACHMENT_LOAD_OP_CLEAR ? PASS_IO_CLEAR_LOAD :
        load == VK_ATTACHMENT_LOAD_OP_NONE_EXT ? PASS_IO_NONE_LOAD : PASS_IO_DISCARD_LOAD;
    const enum pass_io_op s = store == VK_ATTACHMENT_STORE_OP_STORE ? PASS_IO_STORE :
        store == VK_ATTACHMENT_STORE_OP_NONE_EXT ? PASS_IO_NONE_STORE : PASS_IO_DISCARD_STORE;
    pass_io.cell[reason][aspect].count[l]++;
    pass_io.cell[reason][aspect].count[s]++;
    pass_io.cell[reason][aspect].bytes[l] += samples * bytes;
    pass_io.cell[reason][aspect].bytes[s] += samples * bytes;
    if (!bytes) pass_io.cell[reason][aspect].unknown_formats++;
}

static void pass_io_record(enum pass_io_reason reason, VkExtent2D extent, uint32_t layers,
                           const VkAttachmentDescription *att, struct nvn_gpu_tex_s *const *textures,
                           uint32_t count, int has_ds) {
    if (!g.pass_io_on) return;
    const uint64_t pixels = (uint64_t)extent.width * extent.height * (layers ? layers : 1);
    pthread_mutex_lock(&pass_io_mu);
    for (uint32_t i = 0; i < count; i++) {
        const VkAttachmentDescription *a = &att[i];
        const uint64_t samples = pixels * a->samples;
        if (has_ds && i == count - 1) {
            if (nvn_vk_format_has_depth(a->format)) pass_io_add(reason, PASS_IO_DEPTH, samples,
                pass_io_texel_bytes(a->format, 0, PASS_IO_DEPTH), a->loadOp, a->storeOp);
            if (nvn_vk_format_has_stencil(a->format)) pass_io_add(reason, PASS_IO_STENCIL, samples,
                1, a->stencilLoadOp, a->stencilStoreOp);
        } else {
            const uint32_t nvn = textures ? textures[i]->nvn_format : 0;
            pass_io_add(reason, PASS_IO_COLOR, samples, pass_io_texel_bytes(a->format, nvn, PASS_IO_COLOR),
                        a->loadOp, a->storeOp);
        }
    }
    if (++pass_io.passes >= 8192 && !g.pass_discard_on) pass_io_report_locked();
    pthread_mutex_unlock(&pass_io_mu);
}

static void pass_io_finish(void) {
    if (!g.pass_io_on) return;
    pthread_mutex_lock(&pass_io_mu);
    pass_io_report_locked();
    pthread_mutex_unlock(&pass_io_mu);
}

void nvn_gpu_pass_discard_window(void) {
    if (g.pass_discard_on) pass_io_finish();
}

static enum pass_io_reason pass_io_origin(const nvn_gpu_pass_desc *d) {
    if (d->terminal_discard) return PASS_IO_TERMINAL;
    switch (d->trace_origin) {
    case NVN_GPU_PASS_TRACE_CLEAR: return PASS_IO_CLEAR;
    case NVN_GPU_PASS_TRACE_XFER: return PASS_IO_XFER;
    case NVN_GPU_PASS_TRACE_IDLE_DS: return PASS_IO_IDLE_DS;
    case NVN_GPU_PASS_TRACE_FEEDBACK_COPY: return PASS_IO_FEEDBACK_COPY;
    case NVN_GPU_PASS_TRACE_FEEDBACK_READ: return PASS_IO_FEEDBACK_READ;
    case NVN_GPU_PASS_TRACE_CLEAR_RECT: return PASS_IO_CLEAR_RECT;
    case NVN_GPU_PASS_TRACE_AREA_REOPEN: return PASS_IO_AREA_REOPEN;
    case NVN_GPU_PASS_TRACE_DEPTH_BLIT: return PASS_IO_DEPTH_BLIT;
    default: return PASS_IO_DRAW;
    }
}
