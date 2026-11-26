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
    if (fixture_pool) fixture_creates++;
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

    // A rejected committed segment terminates this session's submit timeline.
    if (src) {
        nvn_gpu_buf b = make_buf(GUEST_PAGE, NVN_GPU_MEM_SHARED, NULL);
        active = begin_cmd();
        nvn_gpu_copy_buf_tex(active, b, 0, 128 * 4, GUEST_PAGE, src, 0, 0, &hole);
        finish_expected(active, NVN_GPU_E_UNSUPPORTED);
        active = NULL;
        nvn_gpu_buf_release(b, NULL, NULL);
    }
    fixture_guard_dense = 0;
    sparse_fixture_end(&pool);
}
