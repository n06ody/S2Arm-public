// SPDX-License-Identifier: Apache-2.0
// A command owns its samples until its fence retires. Each copy transfers one
// native texel. Do not use blit conversion (it clamps HDR and loses NaNs).
enum { ROI_ROWS = 64, ROI_STRIDE = 16 };
struct roi_row {
    const char *site;
    VkFormat format;
    uint64_t image;
    uint32_t pass, slot, mip, layer, draws;
    int depth;
    int x, y;
    uint64_t view;
    unsigned order;
};
struct roi_page {
    struct roi_page *next;
    nvn_gpu_buf buffer;
    unsigned count;
    struct roi_row row[ROI_ROWS];
};
struct roi_fragment {
    struct roi_fragment *next;
    struct roi_module module;
    uint64_t view[ROI_TRACE_SITES];
    unsigned pass, draw, x, y;
    nvn_gpu_buf buffer;
    nvn_gpu_pipe variant;
    VkDescriptorPool pool;
};
struct roi_cmd {
    struct roi_fragment *fragments;
    struct roi_page *first, *last;
    unsigned pass, draws, order, id;
    VkRect2D area;
    VkAttachmentDescription physical[9]; // indexed by logical color slot, depth at 8
    int invalid;
};
static uint64_t access_id(uint64_t *slot);

static void roi_message(const char *fmt, ...) {
    va_list ap;
    char text[768];
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "roi", text);
}

static void roi_gap(nvn_gpu_cmd c, const char *reason) {
    roi_message("event=gap frame=%d pass=%u reason=%s", roi_target, c->roi->pass, reason);
}

// An attempted dispatch is not a resource write. Preserve indirect arguments
// as a buffer/offset pair rather than reporting their unknown values as zero.
static void roi_dispatch_skip(nvn_gpu_cmd c, const char *reason, uint32_t x, uint32_t y, uint32_t z,
                              nvn_gpu_buf indirect, uint64_t offset) {
    unsigned order = ++c->roi->order;
    roi_message("event=dispatch_skip frame=%d cmd=%u order=%u pass=%u reason=%s direct_args=%u,%u,%u "
                "indirect_buffer=%llu offset=%llu error=%d", roi_target, c->roi->id, order, c->roi->pass, reason, x, y, z,
                (unsigned long long)(indirect ? access_id(&indirect->access_id) : 0),
                (unsigned long long)offset, (int)c->sticky.code);
    const unsigned st = NVN_GPU_SH_CS;
    for (unsigned i = 0; i < c->nbinds[st]; ++i) {
        const nvn_gpu_binding *b = &c->binds[st][i].b;
        if (b->cls != NVN_GPU_RES_IMAGE || !b->u.view) continue;
        nvn_gpu_view v = b->u.view;
        if (v->tex->target != NVN_TEXTURE_TARGET_3D) continue;
        roi_message("event=dispatch_skip_binding frame=%d cmd=%u order=%u image=%llu mip=%u "
                    "view=%llu type=%d format=%d usage=%u",
                    roi_target, c->roi->id, order, (unsigned long long)access_id(&v->tex->access_id),
                    v->base_level, (unsigned long long)(uintptr_t)(v->swizzled ? v->img : v->view),
                    v->type, v->format, v->tex->usage);
    }
}

static double roi_ufloat(unsigned mant, unsigned exp, unsigned bits) {
    if (exp == 31)
        return mant ? NAN : INFINITY;
    return ldexp(exp ? (1u << bits) + mant : mant, (exp ? (int)exp : 1) - 15 - (int)bits);
}

static double roi_half(uint16_t u) {
    double value = roi_ufloat(u & 1023, (u >> 10) & 31, 10);
    return (u & 32768) ? -value : value;
}

// Decode storage bytes as the attachment's effective format (sRGB into linear RGB).
// Return zero for formats without a decoder so missing values never look like black.
static int roi_decode(VkFormat fmt, const uint8_t *bytes, double out[4]) {
    uint32_t u;
    uint16_t h[4];
    float f[4];
    memcpy(&u, bytes, sizeof u);
    memcpy(h, bytes, sizeof h);
    memcpy(f, bytes, sizeof f);
    switch (fmt) {
    case VK_FORMAT_R8_UNORM:
        out[0] = bytes[0] / 255.;
        break;
    case VK_FORMAT_R8G8_UNORM:
        out[0] = bytes[0] / 255.;
        out[1] = bytes[1] / 255.;
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB: {
        int bgra = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
        int srgb = fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB;
        for (int i = 0; i < 4; ++i) {
            double v = bytes[bgra && (i == 0 || i == 2) ? 2 - i : i] / 255.;
            out[i] = srgb && i < 3 ? (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)) : v;
        }
        break;
    }
    case VK_FORMAT_R16_SFLOAT:
        out[0] = roi_half(h[0]);
        break;
    case VK_FORMAT_R16G16_SFLOAT:
        out[0] = roi_half(h[0]);
        out[1] = roi_half(h[1]);
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        for (int i = 0; i < 4; ++i)
            out[i] = roi_half(h[i]);
        break;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        out[0] = f[0];
        break;
    case VK_FORMAT_R32G32_SFLOAT:
        out[0] = f[0];
        out[1] = f[1];
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        for (int i = 0; i < 4; ++i)
            out[i] = f[i];
        break;
    case VK_FORMAT_D16_UNORM:
        out[0] = h[0] / 65535.;
        break;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        out[0] = (u & 0xffffff) / 16777215.;
        break;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        out[0] = roi_ufloat(u & 63, (u >> 6) & 31, 6);
        out[1] = roi_ufloat((u >> 11) & 63, (u >> 17) & 31, 6);
        out[2] = roi_ufloat((u >> 22) & 31, u >> 27, 5);
        break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        out[0] = (u & 1023) / 1023.;
        out[1] = ((u >> 10) & 1023) / 1023.;
        out[2] = ((u >> 20) & 1023) / 1023.;
        out[3] = (u >> 30) / 3.;
        break;
    default:
        return 0;
    }
    return 1;
}

static void roi_capture(nvn_gpu_cmd c, nvn_gpu_view v, struct roi_row row) {
    nvn_gpu_tex t = v->tex;
    uint32_t mip = row.mip, layer = row.layer;
    uint32_t w = t->w >> mip, h = t->h >> mip;
    if ((uint32_t)row.x >= (w ? w : 1) || (uint32_t)row.y >= (h ? h : 1)) {
        roi_gap(c, "outside_mip");
        return;
    }
    double dummy[4] = {0};
    static const uint8_t zero[ROI_STRIDE] = {0};
    if (!roi_decode(v->format, zero, dummy) || t->samples != 1 || !(t->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        roi_message("event=gap frame=%d pass=%u site=%s slot=%u format=%d samples=%u usage=%u "
                    "reason=unreadable_attachment", roi_target, c->roi->pass, row.site, row.slot,
                    v->format, t->samples, t->usage);
        return;
    }
    struct roi_page *p = c->roi->last;
    if (!p || p->count == ROI_ROWS) {
        nvn_gpu_err e;
        p = calloc(1, sizeof *p);
        if (!p ||
            !nvn_gpu_ok(buf_create_impl(ROI_ROWS * ROI_STRIDE, NVN_GPU_MEM_SHARED, NULL, 0, &p->buffer, &e))) {
            free(p);
            roi_gap(c, "readback_allocation");
            return;
        }
        if (c->roi->last)
            c->roi->last->next = p;
        else
            c->roi->first = p;
        c->roi->last = p;
    }
    unsigned i = p->count++;
    row.format = v->format;
    row.image = access_id(&t->access_id);
    row.pass = c->roi->pass;
    p->row[i] = row;
    VkBufferImageCopy copy = {0};
    copy.bufferOffset = i * ROI_STRIDE;
    copy.imageSubresource =
        (VkImageSubresourceLayers){row.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
    copy.imageOffset = (VkOffset3D){row.x, row.y, 0};
    if (t->target == NVN_TEXTURE_TARGET_3D) {
        copy.imageOffset.z = (int32_t)layer;
        copy.imageSubresource.baseArrayLayer = 0;
    }
    copy.imageExtent = (VkExtent3D){1, 1, 1};
    VkImageLayout old = lay_cur(c, t);
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->buffer->buffer, 1, &copy);
    image_barrier(c, t, old);
    mem_barrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
}

static void roi_pixel(nvn_gpu_cmd c, const char *site, nvn_gpu_view v, unsigned slot, int depth, uint32_t mip,
                      uint32_t layer) {
    roi_capture(c, v, (struct roi_row){.site = site, .slot = slot, .depth = depth, .mip = mip,
                .layer = layer, .draws = c->roi->draws, .x = roi_x, .y = roi_y});
}

// Record only metadata for candidate producers. The target draw identifies its
// input views later. This avoids reading every volume written in the frame.
static VkExtent3D roi_mip_extent(nvn_gpu_tex t, unsigned mip) {
    unsigned w = t->w >> mip, h = t->h >> mip, d = t->depth >> mip;
    return (VkExtent3D){w ? w : 1, h ? h : 1, d ? d : 1};
}

static void roi_volume_write(nvn_gpu_cmd c, nvn_gpu_tex t, const char *kind, uint32_t mip,
                             VkOffset3D offset, VkExtent3D extent, nvn_gpu_view view,
                             uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf indirect, uint64_t arg_offset) {
    if (!roi_volume_pass || t->target != NVN_TEXTURE_TARGET_3D) return;
    roi_message("event=volume_write frame=%d cmd=%u order=%u image=%llu kind=%s mip=%u "
                "origin=%d,%d,%d extent=%u,%u,%u view=%llu type=%d format=%d usage=%u "
                "groups=%u,%u,%u indirect_buffer=%llu indirect_offset=%llu",
                roi_target, c->roi->id, ++c->roi->order, (unsigned long long)access_id(&t->access_id),
                kind, mip, offset.x, offset.y, offset.z, extent.width, extent.height, extent.depth,
                (unsigned long long)(uintptr_t)(view ? (view->swizzled ? view->img : view->view) : VK_NULL_HANDLE),
                view ? (int)view->type : -1, view ? view->format : t->format, t->usage, x,y,z,
                (unsigned long long)(indirect ? access_id(&indirect->access_id) : 0),
                (unsigned long long)arg_offset);
}

static void roi_fragment_report(nvn_gpu_cmd c, struct roi_fragment *f, uint64_t signal);
static void roi_fragment_free(struct roi_fragment *f);
static void roi_report(nvn_gpu_cmd c, int ok, uint64_t signal) {
    if (!ok || c->roi->invalid) {
        roi_gap(c, "submission");
        return;
    }
    for (struct roi_fragment *f = c->roi->fragments; f; f = f->next)
        roi_fragment_report(c, f, signal);
    if (roi_volume_pass)
        roi_message("event=volume_submit frame=%d cmd=%u signal=%llu", roi_target, c->roi->id,
                    (unsigned long long)signal);
    for (struct roi_page *p = c->roi->first; p; p = p->next)
        for (unsigned i = 0; i < p->count; i++) {
            const struct roi_row *r = &p->row[i];
            double v[4] = {0, 0, 0, 1};
            roi_decode(r->format, p->buffer->mapped + i * ROI_STRIDE, v);
            char result[140];
            if (r->depth)
                snprintf(result, sizeof result, "depth=%.9g", v[0]);
            else
                snprintf(result, sizeof result, "rgba=%.9g,%.9g,%.9g,%.9g", v[0], v[1], v[2], v[3]);
            if (r->view) {
                roi_message("event=volume_input frame=%d cmd=%u order=%u pass=%u draw=%u signal=%llu "
                            "image=%llu view=%llu format=%d mip=%u texel=%d,%d,%u %s",
                            roi_target, c->roi->id, r->order, r->pass, r->draws, (unsigned long long)signal,
                            (unsigned long long)r->image, (unsigned long long)r->view, r->format, r->mip,
                            r->x, r->y, r->layer, result);
                continue;
            }
            roi_message("event=sample frame=%d pass=%u signal=%llu site=%s slot=%u image=%llu mip=%u layer=%u "
                        "format=%d draws=%u x=%d y=%d %s",
                        roi_target, r->pass, (unsigned long long)signal, r->site, r->slot,
                        (unsigned long long)r->image, r->mip, r->layer, r->format, r->draws, roi_x, roi_y, result);
        }
}

static void roi_dispose(nvn_gpu_cmd c) {
    struct roi_page *p = c->roi->first;
    while (p) {
        struct roi_page *next = p->next;
        nvn_gpu_buf_release(p->buffer, NULL, NULL);
        free(p);
        p = next;
    }
    struct roi_fragment *f = c->roi->fragments;
    while (f) {
        struct roi_fragment *next = f->next;
        roi_fragment_free(f);
        f = next;
    }
    free(c->roi);
}

static void roi_physical(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d, const VkAttachmentDescription *att,
                         VkRect2D area) {
    unsigned index = 0;
    c->roi->area = area;
    for (unsigned slot = 0; slot < d->ncolor && slot < 8; slot++)
        if (d->color[slot].view)
            c->roi->physical[slot] = att[index++];
    if (d->depth.view || d->stencil.view)
        c->roi->physical[8] = att[index];
}

static void roi_attachments(nvn_gpu_cmd c, const char *site) {
    const nvn_gpu_pass_desc *d = &c->pass_desc;
    for (unsigned slot = 0; slot <= 8; slot++) {
        if (slot < 8 && slot >= d->ncolor)
            continue;
        nvn_gpu_view v = slot == 8 ? d->depth.view : d->color[slot].view;
        if (!v)
            continue;
        int load = slot == 8 ? d->depth.load : d->color[slot].load;
        int store = slot == 8 ? d->depth.store : d->color[slot].store;
        const VkAttachmentDescription *actual = &c->roi->physical[slot];
        roi_message("event=attachment frame=%d pass=%u site=%s slot=%u format=%d load=%d store=%d vkLoad=%d "
                    "vkStore=%d renderArea=%d,%d,%u,%u draws=%u",
                    roi_target, c->roi->pass, site, slot, v->format, load, store, actual->loadOp, actual->storeOp,
                    c->roi->area.offset.x, c->roi->area.offset.y, c->roi->area.extent.width,
                    c->roi->area.extent.height, c->roi->draws);
        if (actual->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) {
            roi_gap(c, "discard_store");
            continue;
        }
        for (unsigned l = 0; l < c->pass_layers; l++)
            roi_pixel(c, site, v, slot, slot == 8, v->base_level,
                      (v->tex->target == NVN_TEXTURE_TARGET_3D ? v->att_layer : v->base_layer) + l);
    }
}

static int roi_can_split(nvn_gpu_cmd c) {
    const nvn_gpu_pass_desc *p = &c->pass_desc;
    if (p->occlusion || c->q_active >= 0 || c->feedback_read_mask)
        return 0;
    for (unsigned i = 0; i < p->ncolor && i < 8; i++)
        if (p->color[i].view && p->color[i].store != NVN_GPU_STORE_STORE)
            return 0;
    return (!p->depth.view || p->depth.store == NVN_GPU_STORE_STORE) &&
           (!p->stencil.view || p->stencil.store == NVN_GPU_STORE_STORE);
}
