// SPDX-License-Identifier: Apache-2.0
// Native residency snapshots share one allocation for resident blocks and
// required tails. On non-strict devices, equally sized holes alias an
// immutable zero block; Vulkan does not define reads from unbound holes there.
// Mesa 26.0.8 reports non-strict, aliased residency and a 2 GiB sparse address
// space. Its 1,431,830,528-byte fixture uses 1,441,792 bytes of backing, while
// a 5,727,322,112-byte image exceeds the address limit before backing allocation.
enum { VK_SPARSE_TAIL_MAX = 256 << 20, VK_SPARSE_ZERO_MAX = 1 + 3 * 32 };

struct sparse_zero {
    nvn_gpu_sparse_region region;
    VkDeviceSize memory_offset;
};
struct sparse_image {
    nvn_gpu_cmd cmd;
    nvn_gpu_sparse_region *regions;
    uint32_t count, nzero;
    struct sparse_zero zeros[VK_SPARSE_ZERO_MAX];
};

static VkResult sparse_queue_bind(const VkBindSparseInfo *bind) {
    VkFence fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult vr = vkCreateFence(g.d, &fi, NULL, &fence);
    if (vr != VK_SUCCESS) return vr;
    pthread_mutex_lock(&g_submit_mu);
    vr = vkQueueBindSparse(g.dev->sparse_queue, 1, bind, fence);
    pthread_mutex_unlock(&g_submit_mu);
    if (vr == VK_SUCCESS) {
        vr = s2a_vk_wait_fences(g.d, 1, &fence, UINT64_MAX);
        if (vr != VK_SUCCESS && vr != VK_ERROR_DEVICE_LOST) {
            // The fence and the image binding cannot be retired while pending.
            pthread_mutex_lock(&g_submit_mu);
            vr = vkQueueWaitIdle(g.dev->sparse_queue);
            pthread_mutex_unlock(&g_submit_mu);
        }
    }
    vkDestroyFence(g.d, fence, NULL);
    return vr;
}

static int sparse_image_tile(const nvn_gpu_sparse_region *j, const struct nvn_gpu_tex_s *t, VkExtent3D block) {
    if (j->level >= t->levels || j->layer >= t->layers) return 0;
    VkOffset3D o;
    VkExtent3D e;
    if (copy_region(t, j->level, &j->region, &o, &e) || o.z || e.depth != 1) return 0;
    uint32_t w = t->w >> j->level, h = t->h >> j->level;
    if (!w) w = 1;
    if (!h) h = 1;
    return o.x % (int32_t)block.width == 0 && o.y % (int32_t)block.height == 0 &&
           (e.width % block.width == 0 || (uint32_t)o.x + e.width == w) &&
           (e.height % block.height == 0 || (uint32_t)o.y + e.height == h);
}

static nvn_gpu_rc sparse_image_bind(struct nvn_gpu_tex_s *t, const VkMemoryRequirements *mr,
                                    uint32_t memory_type, struct sparse_image *sp, int alias_holes, nvn_gpu_err *err) {
    uint32_t nreq = 0;
    vkGetImageSparseMemoryRequirements(g.d, t->image, &nreq, NULL);
    if (!nreq || nreq > 2 || !mr->alignment)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse image requirements");
    VkSparseImageMemoryRequirements req[2];
    vkGetImageSparseMemoryRequirements(g.d, t->image, &nreq, req);

    const VkSparseImageMemoryRequirements *color = NULL;
    uint32_t ntail = 0;
    VkDeviceSize budget = 0;
    for (uint32_t i = 0; i < nreq; i++) {
        const VkImageAspectFlags aspect = req[i].formatProperties.aspectMask;
        if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) color = &req[i];
        else if (aspect != VK_IMAGE_ASPECT_METADATA_BIT)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse image aspect %#x", aspect);
        if (!req[i].imageMipTailSize) {
            if (aspect == VK_IMAGE_ASPECT_METADATA_BIT || req[i].imageMipTailFirstLod < t->levels)
                return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse mip tail is missing from requirements");
            continue;
        }
        const uint32_t layers = (req[i].formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) ? 1 : t->layers;
        const VkDeviceSize size = req[i].imageMipTailSize;
        if (size % mr->alignment || size > (VK_SPARSE_TAIL_MAX - budget) / layers)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse mip tail exceeds resident budget");
        budget += size * layers;
        ntail += layers;
    }
    if (!color) return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse color requirement missing");
    const VkExtent3D block = color->formatProperties.imageGranularity;
    if (!block.width || !block.height || block.depth != 1)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse 2D block shape");
    const int strict = g.dev->properties.sparseProperties.residencyNonResidentStrict;
    uint64_t tile_count = 0;
    for (uint32_t i = 0; i < sp->count; i++) {
        nvn_gpu_sparse_region *j = &sp->regions[i];
        if (j->level >= color->imageMipTailFirstLod) {
            j->upload = 1;
            continue;
        }
        if (!sparse_image_tile(j, t, block))
            return fail(err, NVN_GPU_E_UNSUPPORTED, "guest sparse page does not subdivide into host blocks");
        if (!j->resident && strict) continue;
        if (!j->resident && !alias_holes)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse holes have no defined zero residency");
        tile_count += ((j->region.width - 1u) / block.width + 1ull) * ((j->region.height - 1u) / block.height + 1ull);
        if (tile_count > UINT32_MAX || tile_count > mr->size / mr->alignment)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse block count exceeds image size");
    }

    VkSparseImageMemoryBind *tiles = tile_count ? calloc((size_t)tile_count, sizeof *tiles) : NULL;
    VkSparseMemoryBind *tails = ntail ? calloc(ntail, sizeof *tails) : NULL;
    nvn_gpu_rc rc = {0};
    if ((tile_count && !tiles) || (ntail && !tails)) {
        rc = fail(err, NVN_GPU_E_OOM, "sparse residency table allocation");
        goto done;
    }

    VkDeviceSize bytes = 0;
    uint32_t a = 0, b = 0;
    for (uint32_t i = 0; i < nreq; i++) {
        const VkSparseImageMemoryRequirements *r = &req[i];
        if (!r->imageMipTailSize) continue;
        const uint32_t layers = (r->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) ? 1 : t->layers;
        for (uint32_t layer = 0; layer < layers; layer++) {
            tails[b++] = (VkSparseMemoryBind){
                .resourceOffset = r->imageMipTailOffset + layer * r->imageMipTailStride,
                .size = r->imageMipTailSize, .memoryOffset = bytes,
                .flags = r->formatProperties.aspectMask == VK_IMAGE_ASPECT_METADATA_BIT ? VK_SPARSE_MEMORY_BIND_METADATA_BIT : 0,
            };
            bytes += r->imageMipTailSize;
        }
    }
    for (uint32_t i = 0; i < sp->count; i++) {
        nvn_gpu_sparse_region *j = &sp->regions[i];
        if (j->level >= color->imageMipTailFirstLod || (!j->resident && strict)) continue;
        const uint32_t w = j->region.width, h = j->region.height;
        for (uint32_t y = 0; y < h; y += block.height) for (uint32_t x = 0; x < w; x += block.width) {
            const uint32_t cw = w - x < block.width ? w - x : block.width;
            const uint32_t ch = h - y < block.height ? h - y : block.height;
            const uint32_t ox = j->region.xoffset + x, oy = j->region.yoffset + y;
            VkDeviceSize offset = bytes;
            if (j->resident) bytes += mr->alignment;
            else {
                uint32_t k = 0;
                for (; k < sp->nzero; k++) {
                    const NVNcopyRegion *r = &sp->zeros[k].region.region;
                    if (r->width == (int)cw && r->height == (int)ch) break;
                }
                if (k == sp->nzero) {
                    if (k == VK_SPARSE_ZERO_MAX) {
                        rc = fail(err, NVN_GPU_E_UNSUPPORTED, "sparse zero block shape limit");
                        goto done;
                    }
                    sp->zeros[k] = (struct sparse_zero){.memory_offset = bytes,
                        .region = {.region = {ox, oy, 0, cw, ch, 1}, .level = j->level, .layer = j->layer}};
                    bytes += mr->alignment;
                    sp->nzero++;
                }
                offset = sp->zeros[k].memory_offset;
            }
            tiles[a++] = (VkSparseImageMemoryBind){.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, j->level, j->layer},
                .offset = {ox, oy, 0}, .extent = {cw, ch, 1}, .memoryOffset = offset};
        }
    }

    if (bytes) {
        const VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = bytes, .memoryTypeIndex = memory_type};
        VkResult vr = vkAllocateMemory(g.d, &alloc, NULL, &t->memory);
        if (vr != VK_SUCCESS) {
            rc = fail(err, pipe_vk_res_code(vr, NVN_GPU_E_OOM), "sparse %llu resident bytes (%s)",
                      (unsigned long long)bytes, pipe_vk_res_name(vr));
            goto done;
        }
        for (uint32_t i = 0; i < a; i++) tiles[i].memory = t->memory;
        for (uint32_t i = 0; i < b; i++) tails[i].memory = t->memory;
        const VkSparseImageMemoryBindInfo image = {.image = t->image, .bindCount = a, .pBinds = tiles};
        const VkSparseImageOpaqueMemoryBindInfo opaque = {.image = t->image, .bindCount = b, .pBinds = tails};
        const VkBindSparseInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
            .imageBindCount = a != 0, .pImageBinds = a ? &image : NULL,
            .imageOpaqueBindCount = b != 0, .pImageOpaqueBinds = b ? &opaque : NULL};
        vr = sparse_queue_bind(&bind);
        if (vr != VK_SUCCESS) {
            rc = fail(err, pipe_vk_res_code(vr, NVN_GPU_E_DEVICE), "vkQueueBindSparse (%s)", pipe_vk_res_name(vr));
            goto done;
        }
    }
    t->mem_size = bytes;
    t->mem_type = memory_type;
done:
    free(tiles);
    free(tails);
    return rc;
}

static nvn_gpu_rc sparse_image_zero(const struct sparse_image *sp, nvn_gpu_tex t, nvn_gpu_err *err) {
    if (!sp->nzero) return rc(NVN_GPU_OK);
    int bw, bh;
    const uint32_t bpb = nvn_fmt_bpb(t->nvn_format, &bw, &bh);
    if (!bpb) return fail(err, NVN_GPU_E_UNSUPPORTED, "sparse zero block format");
    uint64_t pitch[VK_SPARSE_ZERO_MAX], size[VK_SPARSE_ZERO_MAX], max_size = 0;
    for (uint32_t i = 0; i < sp->nzero; i++) {
        const NVNcopyRegion *r = &sp->zeros[i].region.region;
        pitch[i] = ((r->width + (uint64_t)bw - 1) / bw) * bpb;
        size[i] = pitch[i] * ((r->height + (uint64_t)bh - 1) / bh);
        if (size[i] > max_size) max_size = size[i];
    }
    uint8_t *zeros = max_size <= VK_SPARSE_TAIL_MAX ? calloc(1, (size_t)max_size) : NULL;
    if (!zeros) return fail(err, NVN_GPU_E_OOM, "sparse zero block upload allocation");
    nvn_gpu_rc result = rc(NVN_GPU_OK);
    for (uint32_t i = 0; i < sp->nzero && nvn_gpu_ok(result); i++)
        result = nvn_gpu_tex_sparse_upload(sp->cmd, t, &sp->zeros[i].region, zeros, pitch[i], size[i]);
    free(zeros);
    if (!nvn_gpu_ok(result)) return fail(err, result.code, "sparse zero block upload");
    return result;
}
