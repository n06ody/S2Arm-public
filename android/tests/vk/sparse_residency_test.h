// SPDX-License-Identifier: Apache-2.0
// Guest page table fixture. The replay resource resolver, upload, VkImage,
// validation and pixel readback are real. The virtual address has no CPU alias.
#include "nvn_deswizzle.h"
#include <sys/mman.h>

enum { GUEST_PAGE = 65536, VPAGE = 4096, SPARSE_WIDTH = 512, SPARSE_HEIGHT = 128,
       BASE_PAGES = 4, SPARSE_BYTES = (BASE_PAGES + 1) * GUEST_PAGE };
static const pool_t *fixture_pool;
static const uint8_t *fixture_pages[SPARSE_BYTES / VPAGE];
static uint64_t fixture_page_gen[SPARSE_BYTES / VPAGE], fixture_gen;
static unsigned fixture_binds, fixture_image_binds, fixture_tail_binds, fixture_creates;
static int fixture_guard_dense;
static VkImageUsageFlags fixture_image_usage;
static VkImageCreateFlags fixture_image_flags;
static VkDeviceMemory fixture_last_memory;
static VkDeviceSize fixture_last_size, fixture_resident_bytes;

extern __typeof__(vkAllocateMemory) __real_vkAllocateMemory;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    if (fixture_guard_dense && info->allocationSize > (256u << 20)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const VkResult result = __real_vkAllocateMemory(device, info, allocator, memory);
    if (fixture_pool && result == VK_SUCCESS) {
        fixture_last_memory = *memory;
        fixture_last_size = info->allocationSize;
    }
    return result;
}

extern __typeof__(vkCreateImage) __real_vkCreateImage;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImage(
    VkDevice device, const VkImageCreateInfo *info, const VkAllocationCallbacks *allocator, VkImage *image) {
    if (fixture_pool) {
        fixture_creates++;
        fixture_image_usage = info->usage;
        fixture_image_flags = info->flags;
    }
    return __real_vkCreateImage(device, info, allocator, image);
}

extern __typeof__(vkQueueBindSparse) __real_vkQueueBindSparse;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueBindSparse(
    VkQueue queue, uint32_t count, const VkBindSparseInfo *info, VkFence fence) {
    const VkResult result = __real_vkQueueBindSparse(queue, count, info, fence);
    if (!fixture_pool || result != VK_SUCCESS) return result;
    fixture_binds += count;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < info[i].imageBindCount; j++) {
            const VkSparseImageMemoryBindInfo *tiles = &info[i].pImageBinds[j];
            fixture_image_binds += tiles->bindCount;
            for (uint32_t k = 0; k < tiles->bindCount; k++)
                CHECK(tiles->pBinds[k].memory == fixture_last_memory, "resident tile must use the single backing slab");
        }
        for (uint32_t j = 0; j < info[i].imageOpaqueBindCount; j++) {
            const VkSparseImageOpaqueMemoryBindInfo *tails = &info[i].pImageOpaqueBinds[j];
            fixture_tail_binds += tails->bindCount;
            for (uint32_t k = 0; k < tails->bindCount; k++)
                CHECK(tails->pBinds[k].memory == fixture_last_memory, "opaque tail must use the single backing slab");
        }
    }
    fixture_resident_bytes += fixture_last_size;
    return result;
}

static int sparse_fixture_range(const void *pool, uint64_t addr, uint64_t *off) {
    if (!fixture_pool || pool != fixture_pool || addr < (uintptr_t)fixture_pool->mem) return 0;
    *off = addr - (uintptr_t)fixture_pool->mem;
    return *off < fixture_pool->size;
}

int nvn_sparse_pool(const void *pool) { return fixture_pool && pool == fixture_pool; }
uint64_t nvn_sparse_gen(void) { return fixture_gen; }

uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t addr, uint64_t bytes) {
    unsigned char *out = dst;
    uint64_t missing = 0, off = 0;
    if (!sparse_fixture_range(pool, addr, &off)) {
        memset(out, 0, bytes);
        return bytes;
    }
    while (bytes) {
        const unsigned in_page = off % VPAGE;
        uint64_t n = VPAGE - in_page;
        if (n > bytes) n = bytes;
        const uint8_t *page = off < SPARSE_BYTES && off < fixture_pool->size ? fixture_pages[off / VPAGE] : NULL;
        if (page) memcpy(out, page + in_page, n);
        else { memset(out, 0, n); missing += n; }
        out += n;
        off += n;
        bytes -= n;
    }
    return missing;
}

uint64_t nvn_sparse_map_any_bits(const void *pool, uint64_t addr, uint64_t bytes, uint8_t *bits) {
    const uint64_t pages = (bytes + GUEST_PAGE - 1) / GUEST_PAGE;
    memset(bits, 0, (pages + 7) / 8);
    uint64_t off = 0, mapped = 0;
    if (!sparse_fixture_range(pool, addr, &off)) return 0;
    for (uint64_t p = 0; p < pages && off < SPARSE_BYTES; p++, off += GUEST_PAGE) {
        for (unsigned v = 0; v < GUEST_PAGE / VPAGE && off + v * VPAGE < fixture_pool->size; v++) {
            if (!fixture_pages[off / VPAGE + v]) continue;
            bits[p / 8] |= 1u << (p % 8);
            mapped++;
            break;
        }
    }
    return mapped;
}

int nvn_sparse_mapped(const void *pool, uint64_t addr, uint64_t bytes) {
    uint64_t off = 0;
    if (!sparse_fixture_range(pool, addr, &off) || bytes > fixture_pool->size - off ||
        off + bytes > SPARSE_BYTES) return 0;
    const uint64_t end = off + bytes;
    for (uint64_t i = off / VPAGE; i < (end + VPAGE - 1) / VPAGE; i++)
        if (!fixture_pages[i]) return 0;
    return 1;
}

int nvn_sparse_since(const void *pool, uint64_t addr, uint64_t bytes, uint64_t gen, uint64_t stamp) {
    (void)stamp;   // Backings in this fixture are immutable.
    uint64_t off = 0;
    if (!sparse_fixture_range(pool, addr, &off) || bytes > fixture_pool->size - off) return 1;
    const uint64_t end = off + bytes < SPARSE_BYTES ? off + bytes : SPARSE_BYTES;
    for (uint64_t v = off / VPAGE; v < (end + VPAGE - 1) / VPAGE; v++)
        if (fixture_page_gen[v] > gen) return 1;
    return 0;
}

void nvn_sparse_protect(const void *pool, uint64_t addr, uint64_t bytes) {
    (void)pool; (void)addr; (void)bytes;   // Physical page contents are immutable.
}
uint64_t nvn_sparse_log_seq(const void *pool) { return nvn_sparse_pool(pool) ? fixture_gen : 0; }
int nvn_sparse_log_hit(const void *pool, uint64_t *seq, uint64_t addr, uint64_t bytes) {
    const int hit = nvn_sparse_since(pool, addr, bytes, *seq, 0);
    *seq = fixture_gen;
    return hit;
}
int nvn_sparse_runs(const void *pool, uint64_t addr, uint64_t bytes, uint64_t (*out)[2], int max) {
    (void)pool; (void)addr; (void)bytes; (void)out; (void)max;
    return -1;   // Ask replay to use conservative per-page inspection.
}

static void sparse_fixture_map(unsigned logical, const uint8_t *physical, unsigned subpages) {
    const uint64_t generation = ++fixture_gen;
    for (unsigned i = 0; i < GUEST_PAGE / VPAGE; i++) {
        const unsigned v = logical * (GUEST_PAGE / VPAGE) + i;
        fixture_pages[v] = physical && i < subpages ? physical + i * VPAGE : NULL;
        fixture_page_gen[v] = generation;
    }
}

static int sparse_fixture_probe(void) {
    VkPhysicalDeviceFeatures f = {0};
    vkGetPhysicalDeviceFeatures(g_dev.physical, &f);
    uint32_t count = 0;
    vkGetPhysicalDeviceSparseImageFormatProperties(g_dev.physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_TILING_OPTIMAL, &count, NULL);
    const int strict = g_dev.properties.sparseProperties.residencyNonResidentStrict;
    printf("sparse Vulkan capability: physical binding=%u residency2D=%u aliased=%u strict=%d enabled_queue=%d RGBA8 layouts=%u space=%llu\n",
           f.sparseBinding, f.sparseResidencyImage2D, f.sparseResidencyAliased, strict, !!g_dev.sparse_queue, count,
           (unsigned long long)g_dev.properties.limits.sparseAddressSpaceSize);
    return f.sparseBinding && f.sparseResidencyImage2D && (f.sparseResidencyAliased || strict) && count;
}

static int sparse_fixture_begin(pool_t *pool, tex_t *t) {
    nvn_tex_layout(&t->d, &t->L);
    pool->flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL;
    pool->size = t->L.size;
    pool->mem = mmap(NULL, pool->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pool->mem != MAP_FAILED, "reserve %llu guest texture bytes", (unsigned long long)pool->size);
    if (pool->mem == MAP_FAILED) return 0;
    t->mem = pool->mem;
    t->pool = pool;
    fixture_pool = pool;
    fixture_binds = fixture_image_binds = fixture_tail_binds = fixture_creates = 0;
    fixture_resident_bytes = 0;
    fixture_image_usage = 0;
    fixture_image_flags = 0;
    memset(fixture_pages, 0, sizeof fixture_pages);
    return 1;
}
static void sparse_fixture_end(const pool_t *pool) {
    rp_res_reset();
    fixture_pool = NULL;
    munmap(pool->mem, pool->size);
}

static void check_sparse_residency(int native) {
    static uint8_t pages[3][GUEST_PAGE], partial[GUEST_PAGE], reference[GUEST_PAGE];
    enum { W = SPARSE_WIDTH, H = SPARSE_HEIGHT, NPIX = W * H * 5 / 4 };
    const uint8_t palette[4][4] = {{239, 41, 35, 255}, {25, 151, 70, 255}, {23, 59, 190, 255}, {0}};
    for (unsigned p = 0; p < 3; p++) for (unsigned i = 0; i < GUEST_PAGE; i += 4)
        memcpy(pages[p] + i, palette[p], 4);
    fixture_sparse_native = native;
    pool_t pool;
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_SPARSE,
                     .width = W, .height = H, .depth = 1, .levels = 2, .samples = 1}, .serial = 101};
    if (!sparse_fixture_begin(&pool, &t)) return;
    CHECK(t.L.size == SPARSE_BYTES && nvn_tex_level_offset(&t.d, 1) == BASE_PAGES * GUEST_PAGE,
          "fixture stores four base pages and one tail page");
    const nvn_texdesc one_page = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
        .width = 128, .height = 128, .depth = 1, .levels = 1, .pkg_layout = 5};
    memcpy(partial, pages[1], VPAGE);
    CHECK(!nvn_deswizzle(&one_page, 0, 0, partial, reference, 128 * 4), "partial-page pixel oracle");
    sparse_fixture_map(0, pages[0], 16);
    sparse_fixture_map(BASE_PAGES, pages[2], 16);
    nvn_gpu_tex src = NULL;
    for (unsigned round = 0; round < 3; round++) {
        if (round == 1) { sparse_fixture_map(0, NULL, 0); sparse_fixture_map(1, pages[1], 16); }
        if (round == 2) sparse_fixture_map(1, pages[1], 1);
        nvn_gpu_buf read = make_buf(SPARSE_BYTES, NVN_GPU_MEM_SHARED, NULL);
        active = begin_cmd();
        src = rp_tex_resolve(&t, active);
        CHECK(src, "sparse texture resolve native=%d vgen=%llu", native, (unsigned long long)fixture_gen);
        if (src) {
            nvn_gpu_copy_tex_buf(active, src, 0, 0, NULL, read, 0, W * 4, W * H * 4);
            nvn_gpu_copy_tex_buf(active, src, 1, 0, NULL, read, W * H * 4, W * 2, W * H);
        }
        finish(active);
        active = NULL;
        if (src) {
            const uint8_t *data = buf_cpu(read);
            unsigned wrong = 0;
            for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
                const unsigned p = x / 128;
                const uint8_t *expected = p == 1 && round == 2 ? reference + (y * 128 + x % 128) * 4
                    : palette[p == 0 && round == 0 ? 0 : p == 1 && round == 1 ? 1 : 3];
                wrong += memcmp(data + (y * W + x) * 4, expected, 4) != 0;
            }
            for (unsigned pix = W * H; pix < NPIX; pix++)
                wrong += memcmp(data + pix * 4, palette[2], 4) != 0;
            printf("sparse native=%d round=%u verified=%u/%u pixels vgen=%llu\n",
                   native, round, NPIX - wrong, NPIX, (unsigned long long)fixture_gen);
            CHECK(!wrong, "sparse pixels native=%d round=%u wrong=%u/%u", native, round, wrong, NPIX);
        }
        nvn_gpu_buf_release(read, NULL, NULL);
    }
    if (native) {
        printf("sparse small bindings=%u tiles=%u opaque=%u physical=%llu bytes\n", fixture_binds, fixture_image_binds,
               fixture_tail_binds, (unsigned long long)fixture_resident_bytes);
        CHECK(fixture_binds == 3 && fixture_image_binds >= 3,
              "expected three sparse snapshots (bindings=%u tiles=%u)", fixture_binds, fixture_image_binds);
        CHECK(fixture_resident_bytes < 3ull * SPARSE_BYTES, "holes cannot occupy unique backing blocks");
    } else CHECK(!fixture_binds, "small sparse texture should use bounded dense storage");
    sparse_fixture_end(&pool);
}

static void check_oversized_sparse(int native) {
    pool_t pool;
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_SPARSE,
                     .width = 8192, .height = 8192, .depth = 16, .levels = 12, .samples = 1}, .serial = 102};
    if (!sparse_fixture_begin(&pool, &t)) return;
    fixture_guard_dense = 1;
    if (native && g_dev.properties.limits.sparseAddressSpaceSize < t.L.size) {
        fixture_sparse_native = 0;
        const uint64_t oom = nvn_unimpl_count("oom", "tex_sparse_resident_oom");
        const uint64_t fallback = nvn_unimpl_count("oom", "tex_sparse_dense_limit");
        active = begin_cmd();
        CHECK(!rp_tex_resolve(&t, active), "oversized native resource must respect sparse address space");
        CHECK(!fixture_binds && !fixture_resident_bytes &&
              (nvn_unimpl_count("oom", "tex_sparse_resident_oom") > oom ||
               nvn_unimpl_count("oom", "tex_sparse_dense_limit") > fallback),
              "sparse address-space refusal must precede residency allocation and give a named OOM");
        finish(active);
        active = NULL;
    }

    fixture_sparse_native = -1;
    t.serial++;
    const unsigned create_before = fixture_creates;
    const uint64_t oom = nvn_unimpl_count("oom", "tex_sparse_dense_limit");
    active = begin_cmd();
    CHECK(!rp_tex_resolve(&t, active), "oversized compatibility must fail before image creation");
    CHECK(fixture_creates == create_before && nvn_unimpl_count("oom", "tex_sparse_dense_limit") == oom + 1,
          "oversized compatibility must report named OOM before any image creation");
    printf("sparse oversized compatibility nominal=%llu image-creates=%u\n",
           (unsigned long long)t.L.size, fixture_creates - create_before);
    finish(active);
    active = NULL;
    fixture_guard_dense = 0;
    sparse_fixture_end(&pool);
}

static void check_large_sparse(int native) {
    check_oversized_sparse(native);
    if (!native) return;
    static uint8_t pixel_page[GUEST_PAGE];
    memset(pixel_page, 0xA5, sizeof pixel_page);
    fixture_sparse_native = 0;   // Production 64 MiB minimum.
    const unsigned layers = g_dev.properties.limits.sparseAddressSpaceSize >= (6ull << 30) ? 16 : 4;
    pool_t pool;
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_SPARSE,
                     .width = 8192, .height = 8192, .depth = layers, .levels = 12, .samples = 1}, .serial = 104};
    if (!sparse_fixture_begin(&pool, &t)) return;
    fixture_guard_dense = 1;
    sparse_fixture_map(0, pixel_page, 16);
    active = begin_cmd();
    const nvn_gpu_tex src = rp_tex_resolve(&t, active);
    CHECK(src != NULL, "sparse array should fit by resident bytes within sparse address space");
    nvn_gpu_buf read = make_buf(2 * GUEST_PAGE, NVN_GPU_MEM_SHARED, NULL);
    const NVNcopyRegion mapped = {0, 0, 0, 128, 128, 1}, hole = {128, 0, 0, 128, 128, 1};
    if (src) {
        nvn_gpu_copy_tex_buf(active, src, 0, 0, &mapped, read, 0, 128 * 4, GUEST_PAGE);
        nvn_gpu_copy_tex_buf(active, src, 0, 0, &hole, read, GUEST_PAGE, 128 * 4, GUEST_PAGE);
    }
    finish(active);
    active = NULL;
    if (src) {
        const uint8_t *data = buf_cpu(read);
        unsigned wrong = 0;
        for (unsigned i = 0; i < GUEST_PAGE; i++) wrong += data[i] != 0xA5 || data[GUEST_PAGE + i] != 0;
        CHECK(!wrong, "large resident page and hole wrong=%u", wrong);
    }
    nvn_gpu_buf_release(read, NULL, NULL);
    printf("sparse large nominal=%llu physical=%llu bytes bindings=%u tiles=%u opaque=%u\n",
           (unsigned long long)t.L.size, (unsigned long long)fixture_resident_bytes,
           fixture_binds, fixture_image_binds, fixture_tail_binds);
    CHECK(fixture_binds == 1 && fixture_image_binds >= 1 && fixture_resident_bytes <= (8u << 20),
          "large sparse need resident pages and required tails only");

    // Writes to a completely unmapped tile are discarded; the shared zero tile stays intact.
    if (src) {
        nvn_gpu_buf b = make_buf(GUEST_PAGE, NVN_GPU_MEM_SHARED, pixel_page);
        active = begin_cmd();
        nvn_gpu_copy_buf_tex(active, b, 0, 128 * 4, GUEST_PAGE, src, 0, 0, &hole);
        nvn_gpu_copy_tex_buf(active, src, 0, 0, &hole, b, 0, 128 * 4, GUEST_PAGE);
        finish(active);
        const uint8_t *hole_data = buf_cpu(b);
        unsigned wrong = 0;
        for (unsigned i = 0; i < GUEST_PAGE; i++) wrong += hole_data[i] != 0;
        CHECK(!wrong, "unmapped write modified %u bytes", wrong);
        active = NULL;
        nvn_gpu_buf_release(b, NULL, NULL);
    }
    fixture_guard_dense = 0;
    sparse_fixture_end(&pool);
}

// Two resident tiles separated by a hole. Read the entire level, an untouched
// hole and the next mip. A scaled blit samples a gradient so clipping must
// preserve the original sampling coordinates on both sides of the hole.
static void check_sparse_writer(const char *op, int native) {
    enum { W = SPARSE_WIDTH, H = SPARSE_HEIGHT, START = 120, END = 392 };
    const int clear = !strcmp(op, "clear"), blit = !strcmp(op, "blit");
    static uint32_t backing[GUEST_PAGE / 4], tail[GUEST_PAGE / 4];
    for (unsigned i = 0; i < GUEST_PAGE / 4; i++) { backing[i] = 0xff706050; tail[i] = 0xff123456; }
    fixture_sparse_native = native ? 1 : -1;
    pool_t pool = {0};
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                     .width = W, .height = H, .depth = 1, .levels = 2, .samples = 1,
                     .flags = NVN_TEXTURE_FLAGS_SPARSE}, .serial = 105};
    if (!sparse_fixture_begin(&pool, &t)) return;
    static uint32_t third[GUEST_PAGE / 4];
    memcpy(third, backing, sizeof third);
    sparse_fixture_map(0, (const uint8_t *)backing, 16);
    sparse_fixture_map(2, (const uint8_t *)third, 16);
    sparse_fixture_map(BASE_PAGES, (const uint8_t *)tail, 16);
    active = begin_cmd();
    nvn_gpu_tex dst = rp_tex_resolve(&t, active);
    CHECK(dst != NULL && !!fixture_binds == native, "sparse writer allocation native=%d binds=%u", native, fixture_binds);
    const unsigned input_width = (END - START) / (blit ? 2 : 1);
    uint32_t gradient[END - START];
    for (unsigned x = 0; x < input_width; x++) gradient[x] = 0xff904000u + x;
    nvn_gpu_buf buffer = make_buf(input_width * 4, NVN_GPU_MEM_SHARED, gradient);
    nvn_gpu_tex src = make_tex(input_width, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    const NVNcopyRegion write = {START, 17, 0, END - START, 1, 1};
    const NVNcopyRegion from = {0, 0, 0, (int)input_width, 1, 1};
    nvn_gpu_tex_upload(active, src, 0, 0, &from, gradient, input_width * 4, input_width * 4);
    CHECK(nvn_gpu_cmd_record_ok(active), "source upload");
    nvn_gpu_view dv = NULL, sv = make_view(src);
    nvn_gpu_err err = {0};
    const view_t base = {.set_levels = 1, .base_level = 0, .nlevels = 1};
    if (dst) CHECK(OK(nvn_gpu_view_create(dst, &base, &dv, &err)), "sparse destination view: %s", err.why);
    finish(active);
    active = begin_cmd();
    if (dst && dv) {
        if (clear) {
            const float color[4] = {1, 0, 1, 1};
            nvn_gpu_clear_view(active, dv, color, NULL, 0);
        } else if (blit) nvn_gpu_blit(active, sv, &from, dv, &write, 0);
        else if (!strcmp(op, "image")) nvn_gpu_copy_tex(active, src, 0, 0, &from, dst, 0, 0, &write);
        else nvn_gpu_copy_buf_tex(active, buffer, 0, input_width * 4, input_width * 4, dst, 0, 0, &write);
    }
    rp_tex_mark_gpu_written(&t);
    nvn_gpu_buf read = make_buf(SPARSE_BYTES, NVN_GPU_MEM_SHARED, NULL);
    if (dst) {
        nvn_gpu_copy_tex_buf(active, dst, 0, 0, NULL, read, 0, W * 4, W * H * 4);
        nvn_gpu_copy_tex_buf(active, dst, 1, 0, NULL, read, W * H * 4, W * 2, W * H);
    }
    const uint64_t fence = ++fixture_signal;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(active, fence, on_done, NULL, &err);
    CHECK(OK(rc), "sparse %s native=%d commit: %d %s", op, native, rc.code, err.why);
    if (OK(rc)) {
        rc = nvn_gpu_fence_wait(fence, 5000000000ull, &err);
        CHECK(OK(rc), "sparse %s wait: %s", op, err.why);
    }
    if (OK(rc)) {
        const uint32_t *data = (const uint32_t *)buf_cpu(read);
        unsigned resident_wrong = 0, hole_wrong = 0, tail_wrong = 0;
        for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
            if ((x / 128) & 1) { hole_wrong += data[y * W + x] != 0; continue; }
            uint32_t expect = 0xff706050;
            if (clear) expect = 0xffff00ff;
            else if (y == 17 && x >= START && x < END) expect = gradient[(x - START) / (blit ? 2 : 1)];
            resident_wrong += data[y * W + x] != expect;
        }
        for (unsigned i = 0; i < W * H / 4; i++) tail_wrong += data[W * H + i] != 0xff123456;
        printf("sparse writer %s native=%d resident_errors=%u/32768 hole_errors=%u/32768 mip_errors=%u/16384\n",
               op, native, resident_wrong, hole_wrong, tail_wrong);
        CHECK(!resident_wrong && !hole_wrong && !tail_wrong, "sparse writer pixels differ");
    }
    if (OK(rc)) {
        // Unmap page zero and map a different backing on page one. Page two
        // keeps its physical identity and the GPU value written above.
        sparse_fixture_map(0, NULL, 0);
        sparse_fixture_map(1, (const uint8_t *)tail, 16);
        active = begin_cmd();
        nvn_gpu_tex next = rp_tex_resolve(&t, active);
        CHECK(next != NULL, "resolve remapped destination");
        if (next) nvn_gpu_copy_tex_buf(active, next, 0, 0, NULL, read, 0, W * 4, W * H * 4);
        finish(active);
        if (next) {
            const uint32_t *data = (const uint32_t *)buf_cpu(read);
            unsigned retained = 0, mapping = 0;
            for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
                const unsigned tile = x / 128;
                if (tile != 2) { mapping += data[y * W + x] != (tile == 1 ? 0xff123456u : 0u); continue; }
                uint32_t expect = clear ? 0xffff00ff : 0xff706050;
                if (!clear && y == 17) expect = gradient[(x - START) / (blit ? 2 : 1)];
                retained += data[y * W + x] != expect;
            }
            printf("sparse remap %s native=%d retained_errors=%u/16384 mapping_errors=%u/49152\n",
                   op, native, retained, mapping);
            CHECK(!retained && !mapping, "GPU contents across sparse remap");
        }
    }
    active = NULL;
    nvn_gpu_buf_release(read, NULL, NULL);
    nvn_gpu_buf_release(buffer, NULL, NULL);
    if (dv) nvn_gpu_view_release(dv);
    nvn_gpu_view_release(sv);
    nvn_gpu_tex_release(src);
    sparse_fixture_end(&pool);
}

static void sparse_pass_draw(nvn_gpu_cmd cmd, nvn_gpu_view view, nvn_gpu_pipe pipe,
                             nvn_gpu_buf verts, unsigned w, unsigned h) {
    nvn_gpu_pass_desc pass = {.ncolor = 1, .w = w, .h = h, .layers = 1};
    pass.color[0].view = view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(cmd, &pass);
    if (!nvn_gpu_cmd_record_ok(cmd)) return;
    const nvn_gpu_dyn dyn = {.origin_ll = 1, .nvp = 1, .vp = {{0, 0, w, h, 0, 1}},
                             .has_sc = 1, .sc = {0, 0, w, h}};
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_set_vbuf(cmd, 0, verts, 0);
    const nvn_gpu_draw_args draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    nvn_gpu_draw(cmd, &draw);
    nvn_gpu_pass_end(cmd);
}

static void sparse_pass_dispatch(nvn_gpu_cmd cmd, nvn_gpu_view view, nvn_gpu_pipe pipe, unsigned w, unsigned h) {
    const nvn_gpu_binding binding = {.ent = 0, .cls = NVN_GPU_RES_IMAGE, .u.view = view};
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &binding, 1);
    nvn_gpu_dispatch(cmd, pipe, w / 4, h / 4, 1, NULL, 0);
}

static unsigned sparse_pass_wrong(const uint32_t *pixels, unsigned w, unsigned h, int rt) {
    unsigned wrong = 0;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
        const uint32_t expect = rt ? (x < w / 2 ? 37u : 73u) : x + 4 * y + 1;
        wrong += pixels[y * w + x] != expect;
    }
    return wrong;
}

static void check_sparse_pass_writer(const char *dir, const char *op, int native, const char *case_name) {
    enum { W = SPARSE_WIDTH, H = SPARSE_HEIGHT, MW = W / 2, MH = H / 2 };
    const int rt = !strcmp(op, "attachment");
    const int whole_hole = !strcmp(case_name, "hole");
    const int partial_tail = !strcmp(case_name, "tail");
    const int base_control = !strcmp(case_name, "base");
    const int raw_attachment = !strcmp(case_name, "raw");
    const int expect_reject = raw_attachment || partial_tail || (whole_hole &&
        (rt || !native || !g_dev.properties.sparseProperties.residencyNonResidentStrict));
    const uint32_t level = whole_hole || base_control ? 0 : 1;
    const uint32_t width = W >> level, height = H >> level;
    const uint64_t size = (uint64_t)width * height * 4;
    fixture_sparse_native = native ? 1 : -1;
    pool_t pool = {0};
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32UI,
                     .width = W, .height = H, .depth = 1, .levels = 2, .samples = 1,
                     .flags = NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_IMAGE}, .serial = 106};
    static uint32_t backing[GUEST_PAGE / 4];
    for (unsigned i = 0; i < GUEST_PAGE / 4; i++) backing[i] = 0x12345678;
    if (!sparse_fixture_begin(&pool, &t)) return;
    sparse_fixture_map(0, (const uint8_t *)backing, 16);
    sparse_fixture_map(2, (const uint8_t *)backing, 16);
    if (base_control) {
        sparse_fixture_map(1, (const uint8_t *)backing, 16);
        sparse_fixture_map(3, (const uint8_t *)backing, 16);
    }
    sparse_fixture_map(BASE_PAGES, (const uint8_t *)backing, partial_tail ? 15 : 16);

    const view_t mip = {.set_levels = 1, .base_level = level, .nlevels = 1};
    active = begin_cmd();
    nvn_gpu_tex tex = rp_tex_resolve(&t, active);
    CHECK(tex && !!fixture_binds == native, "sparse pass allocation native=%d binds=%u", native, fixture_binds);
    const int promote = rt && native && !raw_attachment;
    const int check_retained = promote && !expect_reject;
    const NVNcopyRegion marker_region = {0, 0, 0, 4, 4, 1};
    uint32_t marker[16];
    nvn_gpu_buf retained = NULL;
    if (tex && check_retained) {
        for (unsigned i = 0; i < 16; i++) marker[i] = 0x5a172300u + i;
        nvn_gpu_tex_upload(active, tex, level, 0, &marker_region, marker, 16, sizeof marker);
        rp_tex_mark_gpu_written(&t);
        retained = make_buf(sizeof marker, NVN_GPU_MEM_SHARED, NULL);
    }
    if (promote) CHECK(rp_tex_prepare_sparse_attachment(&t, active), "prepare sparse attachment storage");
    nvn_gpu_view view = rt && !raw_attachment
        ? rp_view_for_attach(&t, &mip, active) : rp_view_for(&t, &mip, active);
    nvn_gpu_tex bound = rp_tex_resolve(&t, active);
    if (promote) CHECK(bound && bound != tex, "replace the sparse image before publishing attachment views");
    tex = bound;
    const VkImageUsageFlags required = rt ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
    const int has_usage = raw_attachment || (fixture_image_usage & required);
    CHECK(has_usage, "fully resident sparse mip missing image usage for %s", op);
    CHECK(!!(fixture_image_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) == (native && !promote),
          "sparse %s chose the wrong image storage", op);
    CHECK(view != NULL, "fully resident sparse mip should accept %s", op);
    if (retained && tex) nvn_gpu_copy_tex_buf(active, tex, level, 0, &marker_region, retained, 0, 16, sizeof marker);
    finish(active);
    if (retained) {
        const int keeps = tex && !memcmp(buf_cpu(retained), marker, sizeof marker);
        CHECK(keeps, "attachment migration must retain same-command GPU writes");
        printf("sparse attachment migrate %s retained=%u/16\n", case_name, keeps ? 16 : 0);
        nvn_gpu_buf_release(retained, NULL, NULL);
    }

    nvn_gpu_err err = {0};
    nvn_gpu_shader vs = NULL, sh = NULL;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_buf verts = NULL;
    void *vs_bytes = NULL, *fs_bytes = NULL;
    if (rt) {
        CHECK(load_shader_pair(dir, "tri.vert", "sparse_attachment.frag", NULL, NULL, &vs, &sh, &vs_bytes, &fs_bytes),
              "sparse attachment shaders");
        nvn_gpu_pipe_desc pd = {
            .poly.mode = NVN_POLYGON_MODE_FILL, .origin_ll = 1, .color.logic_op = NVN_LOGIC_OP_COPY,
            .cfmt[0] = NVN_FORMAT_R32UI, .samples = 1, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .chmask.mask = 15,
            .attr = {{.fmt = NVN_FORMAT_RG32F}, {.fmt = NVN_FORMAT_RG32F, .offset = 8}},
            .stream[0].stride = 16,
        };
        pd.st[NVN_GPU_SH_VS].sh = vs;
        pd.st[NVN_GPU_SH_FS].sh = sh;
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err)), "sparse attachment pipe: %s", err.why);
        const float tri[3][4] = {{-1, -1, 0, 0}, {3, -1, 2, 0}, {-1, 3, 0, 2}};
        verts = make_buf(sizeof tri, NVN_GPU_MEM_SHARED, tri);
    } else {
        const nvn_gpu_layout_ent entry = {.cls = NVN_GPU_RES_IMAGE, .access = NVN_GPU_ACC_W,
                                          .count = 1, .stages = 1u << NVN_GPU_SH_CS};
        const nvn_gpu_layout layout = {.ent = &entry, .nent = 1};
        pipe = mk_cpipe(dir, "img.comp", &layout, &sh);
        CHECK(pipe != NULL, "sparse storage pipe");
    }
    nvn_gpu_buf read = make_buf(size > 3 * GUEST_PAGE ? size : 3 * GUEST_PAGE, NVN_GPU_MEM_SHARED, NULL);
    if (tex && view && has_usage && pipe && read) {
        active = begin_cmd();
        if (rt) sparse_pass_draw(active, view, pipe, verts, width, height);
        else sparse_pass_dispatch(active, view, pipe, width, height);
        if (expect_reject) {
            const int refused = !nvn_gpu_cmd_record_ok(active);
            CHECK(refused, "sparse %s %s must reject before recording the write", op, case_name);
            finish_expected(active, NVN_GPU_E_UNSUPPORTED);
            if (refused) printf("sparse pass %s native=%d %s rejected\n", op, native, case_name);
        } else {
            CHECK(nvn_gpu_cmd_record_ok(active), "sparse %s must write level %u", op, level);
            rp_tex_mark_gpu_written(&t);
            CHECK(nvn_be_tex_gpu_owned(t.serial), "sparse ReadTexels must use GPU contents after %s", op);
            nvn_gpu_copy_tex_buf(active, tex, level, 0, NULL, read, 0, width * 4, size);
            finish(active);
            const uint32_t *p = (const uint32_t *)buf_cpu(read);
            unsigned write_wrong = 0;
            if (whole_hole) {
                for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width; x++) {
                    const uint32_t written = rt ? (x < width / 2 ? 37u : 73u) : x + 4 * y + 1;
                    const uint32_t expected = ((x / 128) & 1) ? 0 : written;
                    write_wrong += p[y * width + x] != expected;
                }
            } else write_wrong = sparse_pass_wrong(p, width, height, rt);
            if (write_wrong) {
                printf("sparse pass %s native=%d level=%u bad=%u/%u first row=%08x,%08x,%08x,%08x last row=%08x,%08x,%08x,%08x\n",
                       op, native, level, write_wrong, width * height,
                       p[0], p[width / 4], p[width / 2], p[width * 3 / 4],
                       p[(height - 1) * width], p[(height - 1) * width + width / 4],
                       p[(height - 1) * width + width / 2], p[(height - 1) * width + width * 3 / 4]);
            }
            CHECK(!write_wrong, "sparse %s write pixels differ: %u", op, write_wrong);
            if (base_control || whole_hole) {
                printf("sparse pass %s native=%d %s write_errors=%u/%u\n",
                       op, native, case_name, write_wrong, width * height);
            } else {
                sparse_fixture_map(0, NULL, 0);
                active = begin_cmd();
                nvn_gpu_tex next = rp_tex_resolve(&t, active);
                CHECK(next != NULL && next != tex, "resolve remap after sparse %s", op);
                unsigned remap_wrong = MW * MH, hole_wrong = MW * MH;
                if (next) {
                    const NVNcopyRegion hole = {0, 0, 0, 128, 128, 1};
                    nvn_gpu_copy_tex_buf(active, next, 1, 0, NULL, read, GUEST_PAGE, MW * 4, GUEST_PAGE);
                    nvn_gpu_copy_tex_buf(active, next, 0, 0, &hole, read, 2 * GUEST_PAGE, 128 * 4, GUEST_PAGE);
                    finish(active);
                    p = (const uint32_t *)buf_cpu(read);
                    remap_wrong = sparse_pass_wrong(p + GUEST_PAGE / 4, MW, MH, rt);
                    hole_wrong = 0;
                    for (unsigned i = 0; i < MW * MH; i++) hole_wrong += p[2 * GUEST_PAGE / 4 + i] != 0;
                } else finish(active);
                printf("sparse pass %s native=%d write_errors=%u/16384 remap_errors=%u/16384 hole_errors=%u/16384\n",
                       op, native, write_wrong, remap_wrong, hole_wrong);
                CHECK(!remap_wrong && !hole_wrong, "sparse %s remap changed retained or hole pixels", op);
            }
        }
    }
    active = NULL;
    if (read) nvn_gpu_buf_release(read, NULL, NULL);
    if (verts) nvn_gpu_buf_release(verts, NULL, NULL);
    if (vs) nvn_gpu_shader_release(vs);
    if (sh) nvn_gpu_shader_release(sh);
    free(vs_bytes);
    free(fs_bytes);
    sparse_fixture_end(&pool);
}
