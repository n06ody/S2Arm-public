// nvn_gpu_* 的 Vulkan 后端（W1 D8 第一段：接管已有设备 → 缓冲 → 写 / 填 / 拷 → 提交 / 等 → 读回 → release / abort / close）。
//
// 范围（本段只做契约用例 C01–C08 需要的原语；其余函数按契约明确返回 E_UNSUPPORTED，不静默空实现）：
//   · 设备：nvn_gpu_open 接管平台已建好的 struct s2a_vk_device（不另建、不拥有其寿命）；
//   · 缓冲：create / cpu / va / write / fill / copy / arena / use / release(retired)；
//   · 提交：cmd_begin / commit / abort / fence_done / fence_failed / fence_wait；
//   · 失败终止（A4）：第一个失败段的 signal = F，先发布 F 再以 FAILED 回调；F 之后不再执行；
//   · 发布顺序：GPU 完成 → done（核心回写）→ release 语义推进已完成值；fence_wait 以 acquire 语义读。
// 本段不做：贴图 / 视图 / 采样器 / 着色器 / 管线 / 通道 / 上屏（清屏、三角形、深度用例随后接，届时按同一契约补）。
//
// 线程模型：[提交] 线程录制并提交；后端自己的 worker 线程等栅栏、调 done / retired（[回调]）。
// 对象状态（refs / released）都由 g_mu 保护；回调一律在锁外调用。
//
// 已知与契约 v2 的接点（消费审查，见 docs/设计_回放核心共用.md / 工单 #784 评论）：
//   1. nvn_gpu_binding 的 union 只能放 view 或 samp，表达不了 TEX_SAMP 组合（安卓现有 COMBINED_IMAGE_SAMPLER）；
//   2. done.signal 的「0 = 不推进」与 commit 的「signal 严格递增」冲突；本实现按后者：signal = 0 直接 E_ARG，
//      因为失败终止用 F = 0 表示「没有失败」，无信号段一旦失败无法表达。等契约属主定稿后同批改。

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvn_backend.h"
#include "nvn_vk_format.h"
#include "nvn_vk_texview.h"
#include "nvn_vk_bcdec.h"
#include "nvn_fmt_info.h"
#include "nvn_fmt_expand.h"
#include "vk_device.h"
#include "../capprobe/capprobe.h"
S2A_CAPPROBE_RUNTIME_LOST_WEAK_REF;   // 宿主链接单元无 bridge.cpp 时解析为 NULL

#define VK_SEG_QUEUE_INIT 16
#define VK_ARENA_MIN (256u * 1024u)
#define VK_WAIT_SLICE_NS (50ull * 1000ull * 1000ull)

enum { STAT_BUFS = 0, STAT_SUBMITS, STAT_DONE_OK, STAT_DONE_FAILED, STAT_RETIRED, STAT_WRITES, STAT_FILLS, STAT_COPIES, STAT_N };
static const char *const stat_names[STAT_N] = {
    "bufs_created", "submits", "segments_done", "segments_failed", "bufs_retired", "buf_writes", "buf_fills", "buf_copies",
};

struct nvn_gpu_buf_s {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    uint64_t bytes;
    uint64_t va;
    int mem_kind;
    int released;
    int refs;
    nvn_gpu_retired_fn retired;
    void *retired_user;
    struct nvn_gpu_buf_s *all_next;
    struct nvn_gpu_buf_s *retire_next;   // 退休队列就地链接，队列本身不再分配
};

struct nvn_gpu_tex_s {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
    uint32_t w, h, depth, levels, layers, samples, nvn_format, target;
    int array_compatible;
    int view_fmt, released, refs;
    struct nvn_gpu_tex_s *all_next;
};

struct nvn_gpu_view_s {
    VkImageView view;
    struct nvn_gpu_tex_s *tex;
    VkBufferView buffer_view;
    struct nvn_gpu_buf_s *buf;
    VkFormat buffer_format;
    VkFormatFeatureFlags buffer_features;
    uint32_t buffer_caps;
    uint32_t nvn_format;   // 视图的有效 NVN 格式（可从贴图覆盖）；clear 的 is_int 按它判
    uint32_t base_level, levels, base_layer, layers;
    int released, refs;
    struct nvn_gpu_view_s *all_next;
};

struct nvn_gpu_shader_s {
    VkShaderModule mod;
    int missing_storage_format_feature;
    nvn_gpu_stage stage;
    nvn_gpu_layout_ent *ents;
    VkFormat *tfmt;              // 与 ents 平行：TBUF / IMAGE_TBUF 项的 texel 视图格式（自 blob 反射）；非 texel 项 UNDEFINED
    uint32_t nent;
    uint32_t push_off, push_bytes;
    char entry[32];
    int released, refs;
    struct nvn_gpu_shader_s *all_next;
};

struct nvn_gpu_pipe_s {
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkDescriptorSetLayout dsl[4];
    uint32_t nset;
    nvn_gpu_layout_ent *stage_ents[6];
    VkFormat *stage_tfmt[6];
    uint32_t stage_nent[6];
    uint32_t stage_push_off[6], stage_push_bytes[6];
    nvn_gpu_pipe_desc key;
    struct nvn_gpu_pipe_s *all_next;
};

struct nvn_gpu_samp_s {
    VkSampler sampler;
    int released, refs;
    struct nvn_gpu_samp_s *all_next;
};

struct vk_ref { void *obj; int type; };
enum { REF_BUF = 1, REF_TEX, REF_VIEW, REF_SAMP };

struct vk_staging { struct nvn_gpu_buf_s *buf; };

struct pass_rec { VkRenderPass rp; VkFramebuffer fb; };

struct nvn_gpu_cmd_s {
    VkCommandBuffer cb;
    int recording;
    int in_pass;
    nvn_gpu_rc sticky;
    char why[192];
    struct vk_ref *refs;
    uint32_t nrefs, refcap;
    struct vk_staging *staging;
    uint32_t nstaging, scap;
    struct nvn_gpu_buf_s *arena;
    uint64_t arena_used, arena_cap;
    VkCommandPool pool;
    VkDescriptorPool dpool;
    struct nvn_gpu_pipe_s *pipe;
    struct pass_rec *passes;
    uint32_t npasses, passcap;
    uint32_t pass_w, pass_h;
    struct nvn_gpu_tex_s *pass_tex[8];
    uint32_t pass_ntex;
    struct { struct nvn_gpu_tex_s *t; VkImageLayout old; } lay[64];
    uint32_t nlay;
    nvn_gpu_dyn dyn;
    int has_dyn;
    struct { nvn_gpu_buf buf; uint64_t off; } vbuf[16];
    struct { nvn_gpu_binding b; } binds[6][64];   // 每阶段一张表，bind 只替换该阶段
    uint32_t nbinds[6];
    uint8_t *push_data[6];        // 各阶段 push 暂存；vkCmdPushConstants 录制时拷贝入命令缓冲，段完成即放
    uint32_t push_capacity[6]; // 分配字节数，测试与管线切换共用
    uint8_t push_have[6];
    struct nvn_gpu_cmd_s *live_next;
};

struct vk_segment {
    struct vk_segment *next;
    struct nvn_gpu_cmd_s *cmd;
    uint64_t signal;
    nvn_gpu_done_fn done;
    void *user;
    VkFence fence;
    VkCommandBuffer cb;
    int failed;
    int submitted;
    int executed;
    nvn_gpu_rc rc;
    char why[192];
};

struct nvn_gpu_vk {
    struct s2a_vk_device *dev;
    VkDevice d;
    VkPhysicalDevice phys;
    VkQueue queue;
    nvn_gpu_caps caps;
    pthread_t worker;
    int worker_started;
    int closing;
    int closed;
    _Atomic int stop;
    int busy;
    int commit_active;   // 正在 commit 的线程数：close 要等它归零才拆资源
    int close_active;    // 已有一个线程在跑 close，防止重复 join
    _Atomic int device_lost;
    uint64_t completed;
    uint64_t failed;
    uint64_t last_signal;
    uint64_t *submitted;
    uint32_t nsub, capsub;
    struct vk_segment *qhead, *qtail;
    struct nvn_gpu_buf_s *rhead, *rtail;
    struct nvn_gpu_cmd_s *live;
    struct nvn_gpu_buf_s *bufs;
    struct nvn_gpu_tex_s *texs;
    struct nvn_gpu_view_s *views;
    struct nvn_gpu_samp_s *samps;
    struct nvn_gpu_shader_s *shaders;
    struct nvn_gpu_pipe_s *pipes;
    uint64_t stats[STAT_N];
    int fault_inject;
    uint64_t test_fail_signal;    // 测试钩子：让指定 signal 的段表现为 fence 失败
    uint64_t test_delay_signal;   // 测试钩子：让指定 signal 的段先睡 ms
    uint32_t test_delay_ms;
};

static struct nvn_gpu_vk g;
static _Atomic int g_open;   // 跨线程：close 置 0 后其他线程据此拒收
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_submit_mu = PTHREAD_MUTEX_INITIALIZER; // 提交串行化：校验 signal、vkQueueSubmit、入队
static pthread_cond_t g_cv;
static int g_cv_ready;
static _Thread_local int g_in_callback;   // [回调] 线程：只许调 [任意] 函数

// ---------------------------------------------------------------- 小工具

static nvn_gpu_rc rc(int code) { nvn_gpu_rc r; r.code = code; return r; }
static int rc_is(nvn_gpu_rc r, int code) { return r.code == code; }

static nvn_gpu_rc fail(nvn_gpu_err *e, int code, const char *fmt, ...) {
    if (e) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->why, sizeof e->why, fmt, ap);
        va_end(ap);
        e->rc = rc(code);
    }
    return rc(code);
}

static void set_sticky(struct nvn_gpu_cmd_s *c, int code, const char *fmt, ...) {
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;   // 首错原文
    c->sticky = rc(code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->why, sizeof c->why, fmt, ap);
    va_end(ap);
}

static uint64_t add_sat(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void deadline_after(struct timespec *ts, uint64_t ns) {
    uint64_t t = add_sat(now_ns(), ns);
    ts->tv_sec = (time_t)(t / 1000000000ull);
    ts->tv_nsec = (long)(t % 1000000000ull);
}

// ---------------------------------------------------------------- 提交集合

static int submitted_has(struct nvn_gpu_vk *vk, uint64_t v) {
    uint32_t lo = 0, hi = vk->nsub;   // signal 严格递增，数组天然有序
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (vk->submitted[mid] == v) return 1;
        if (vk->submitted[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static void submitted_compact_locked(struct nvn_gpu_vk *vk) {
    uint32_t drop = 0;
    while (drop < vk->nsub && vk->submitted[drop] <= vk->completed) drop++;
    if (!drop) return;
    memmove(vk->submitted, vk->submitted + drop, (size_t)(vk->nsub - drop) * sizeof *vk->submitted);
    vk->nsub -= drop;
}

static int submitted_add(struct nvn_gpu_vk *vk, uint64_t v) {
    if (submitted_has(vk, v)) return 1;
    if (vk->nsub == vk->capsub) {
        uint32_t cap = vk->capsub ? vk->capsub * 2 : VK_SEG_QUEUE_INIT;
        uint64_t *p = realloc(vk->submitted, (size_t)cap * sizeof *p);
        if (!p) return 0;
        vk->submitted = p;
        vk->capsub = cap;
    }
    vk->submitted[vk->nsub++] = v;
    return 1;
}

// ---------------------------------------------------------------- 引用与退休

// 调用方持 g_mu。对象本身就是队列节点：分配失败不可能吞掉 retired。
static void queue_retire_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    b->retire_next = NULL;
    if (vk->rtail) vk->rtail->retire_next = b;
    else vk->rhead = b;
    vk->rtail = b;
    pthread_cond_broadcast(&g_cv);
}

static void destroy_pipe_objects(struct nvn_gpu_pipe_s *p);
static void destroy_tex_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t);
static void destroy_view_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v);
static void destroy_samp_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_samp_s *s);
static void unref_locked(struct nvn_gpu_vk *vk, void *obj, int type);

// 调用方持 g_mu。贴图 / 视图没有 retired 回调，最后一次引用结束即可销毁。
static void destroy_tex_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t) {
    struct nvn_gpu_tex_s **pp = &vk->texs;
    while (*pp && *pp != t) pp = &(*pp)->all_next;
    if (*pp) *pp = t->all_next;
    if (t->view) vkDestroyImageView(vk->d, t->view, NULL);
    if (t->image) vkDestroyImage(vk->d, t->image, NULL);
    if (t->memory) vkFreeMemory(vk->d, t->memory, NULL);
    free(t);
}

static void destroy_view_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v) {
    struct nvn_gpu_view_s **pp = &vk->views;
    while (*pp && *pp != v) pp = &(*pp)->all_next;
    if (*pp) *pp = v->all_next;
    if (v->view) vkDestroyImageView(vk->d, v->view, NULL);
    if (v->buffer_view) vkDestroyBufferView(vk->d, v->buffer_view, NULL);
    struct nvn_gpu_tex_s *t = v->tex;
    struct nvn_gpu_buf_s *b = v->buf;
    free(v);
    if (b) unref_locked(vk, b, REF_BUF);
    if (t) {
        if (t->refs > 0) t->refs--;
        if (t->released && t->refs == 0) destroy_tex_locked(vk, t);
    }
}

static void destroy_samp_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_samp_s *s) {
    struct nvn_gpu_samp_s **pp = &vk->samps;
    while (*pp && *pp != s) pp = &(*pp)->all_next;
    if (*pp) *pp = s->all_next;
    if (s->sampler) vkDestroySampler(vk->d, s->sampler, NULL);
    free(s);
}

static int ref_released(void *obj, int type) {
    switch (type) {
    case REF_BUF: return ((struct nvn_gpu_buf_s *)obj)->released;
    case REF_TEX: return ((struct nvn_gpu_tex_s *)obj)->released;
    case REF_VIEW: return ((struct nvn_gpu_view_s *)obj)->released;
    case REF_SAMP: return ((struct nvn_gpu_samp_s *)obj)->released;
    default: return 1;
    }
}

static void ref_bump(void *obj, int type) {
    switch (type) {
    case REF_BUF: ((struct nvn_gpu_buf_s *)obj)->refs++; break;
    case REF_TEX: ((struct nvn_gpu_tex_s *)obj)->refs++; break;
    case REF_VIEW: ((struct nvn_gpu_view_s *)obj)->refs++; break;
    case REF_SAMP: ((struct nvn_gpu_samp_s *)obj)->refs++; break;
    }
}

static void ref_drop(void *obj, int type) {
    switch (type) {
    case REF_BUF: if (((struct nvn_gpu_buf_s *)obj)->refs > 0) ((struct nvn_gpu_buf_s *)obj)->refs--; break;
    case REF_TEX: if (((struct nvn_gpu_tex_s *)obj)->refs > 0) ((struct nvn_gpu_tex_s *)obj)->refs--; break;
    case REF_VIEW: if (((struct nvn_gpu_view_s *)obj)->refs > 0) ((struct nvn_gpu_view_s *)obj)->refs--; break;
    case REF_SAMP: if (((struct nvn_gpu_samp_s *)obj)->refs > 0) ((struct nvn_gpu_samp_s *)obj)->refs--; break;
    }
}

static void cmd_ref(struct nvn_gpu_cmd_s *c, void *obj, int type) {
    if (!obj) return;
    pthread_mutex_lock(&g_mu);
    if (ref_released(obj, type)) {   // release 之后再录制使用 = 契约违规（粘滞 E_ARG）
        pthread_mutex_unlock(&g_mu);
        set_sticky(c, NVN_GPU_E_ARG, "release 之后仍在使用句柄");
        return;
    }
    ref_bump(obj, type);
    pthread_mutex_unlock(&g_mu);
    if (c->nrefs == c->refcap) {
        uint32_t cap = c->refcap ? c->refcap * 2 : 16;
        struct vk_ref *p = realloc(c->refs, (size_t)cap * sizeof *p);
        if (!p) {   // 记录失败视为 OOM：整段不提交；刚加的引用要当场归还
            pthread_mutex_lock(&g_mu);
            ref_drop(obj, type);
            pthread_mutex_unlock(&g_mu);
            set_sticky(c, NVN_GPU_E_OOM, "引用表分配失败");
            return;
        }
        c->refs = p;
        c->refcap = cap;
    }
    c->refs[c->nrefs].obj = obj;
    c->refs[c->nrefs].type = type;
    c->nrefs++;
}

// 调用方持 g_mu。
static void unref_locked(struct nvn_gpu_vk *vk, void *obj, int type) {
    if (!obj) return;
    if (type == REF_BUF) {
        struct nvn_gpu_buf_s *b = obj;
        if (b->refs > 0) b->refs--;
        if (b->released && b->refs == 0) queue_retire_locked(vk, b);
        return;
    }
    if (type == REF_TEX) {
        struct nvn_gpu_tex_s *t = obj;
        if (t->refs > 0) t->refs--;
        if (t->released && t->refs == 0) destroy_tex_locked(vk, t);
        return;
    }
    if (type == REF_VIEW) {
        struct nvn_gpu_view_s *v = obj;
        if (v->refs > 0) v->refs--;
        if (v->released && v->refs == 0) destroy_view_locked(vk, v);
        return;
    }
    if (type == REF_SAMP) {
        struct nvn_gpu_samp_s *s = obj;
        if (s->refs > 0) s->refs--;
        if (s->released && s->refs == 0) destroy_samp_locked(vk, s);
    }
}

static void unlink_buf_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    struct nvn_gpu_buf_s **pp = &vk->bufs;
    while (*pp && *pp != b) pp = &(*pp)->all_next;
    if (*pp) *pp = b->all_next;
    b->all_next = NULL;
}

static void retire_one(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    nvn_gpu_retired_fn cb = b->retired;
    void *user = b->retired_user;
    pthread_mutex_lock(&g_mu);
    unlink_buf_locked(vk, b);
    pthread_mutex_unlock(&g_mu);
    if (b->mapped && vk->d) vkUnmapMemory(vk->d, b->memory);
    if (b->buffer) vkDestroyBuffer(vk->d, b->buffer, NULL);
    if (b->memory) vkFreeMemory(vk->d, b->memory, NULL);
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->mapped = NULL;
    free(b);
    pthread_mutex_lock(&g_mu);
    vk->stats[STAT_RETIRED]++;
    pthread_mutex_unlock(&g_mu);
    if (cb) {
        g_in_callback++;
        cb(user);
        g_in_callback--;
    }
}

// ---------------------------------------------------------------- 设备 / 能力

static uint32_t sample_bits_max(VkSampleCountFlags f) {
    uint32_t n = 0;
    for (uint32_t s = 1; s <= 64; s <<= 1)
        if (f & s) n = s;
    return n;
}

static void caps_digest(const void *p, size_t n, uint8_t out[16]) {
    uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull;
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h1 = (h1 ^ b[i]) * 1099511628211ull;
        h2 = (h2 + b[i]) * 1469598103934665603ull;
    }
    memcpy(out, &h1, 8);
    memcpy(out + 8, &h2, 8);
}

static void fill_caps(struct nvn_gpu_vk *vk, const nvn_gpu_open_desc *d) {
    const VkPhysicalDeviceProperties *p = &vk->dev->properties;
    const VkPhysicalDeviceLimits *l = &p->limits;
    nvn_gpu_caps *c = &vk->caps;
    memset(c, 0, sizeof *c);
    c->api = NVN_GPU_API_VULKAN;
    c->api_version = p->apiVersion;
    c->family = 0;
    c->vendor_id = p->vendorID;
    c->device_id = p->deviceID;
    snprintf(c->name, sizeof c->name, "%.63s", p->deviceName);
    c->max_color_att = l->maxColorAttachments;
    c->max_vertex_streams = l->maxVertexInputBindings;
    c->max_tex_dim = l->maxImageDimension2D;
    c->max_samples = sample_bits_max(l->framebufferColorSampleCounts & l->framebufferDepthSampleCounts);
    c->max_viewports = l->maxViewports;
    c->max_push_bytes = l->maxPushConstantsSize;
    c->min_ubo_align = (uint32_t)l->minUniformBufferOffsetAlignment;
    c->min_ssbo_align = (uint32_t)l->minStorageBufferOffsetAlignment;
    c->max_ubo_range = l->maxUniformBufferRange;
    c->max_ssbo_range = l->maxStorageBufferRange;
    c->max_buf_bytes = l->maxStorageBufferRange;
    c->max_stage_res[NVN_GPU_RES_UBO] = l->maxPerStageDescriptorUniformBuffers;
    c->max_stage_res[NVN_GPU_RES_SSBO] = l->maxPerStageDescriptorStorageBuffers;
    c->max_stage_res[NVN_GPU_RES_TEX] = l->maxPerStageDescriptorSampledImages;
    c->max_stage_res[NVN_GPU_RES_SAMP] = l->maxPerStageDescriptorSamplers;
    uint32_t comb = l->maxPerStageDescriptorSampledImages < l->maxPerStageDescriptorSamplers
                        ? l->maxPerStageDescriptorSampledImages : l->maxPerStageDescriptorSamplers;
    c->max_stage_res[NVN_GPU_RES_TEX_SAMP] = comb;
    c->max_stage_res[NVN_GPU_RES_IMAGE] = l->maxPerStageDescriptorStorageImages;
    c->max_sets = l->maxBoundDescriptorSets;

    VkPhysicalDeviceFeatures f = vk->dev->core_features;   // 已启用记录，不是「设备支持」
    if (f.vertexPipelineStoresAndAtomics) c->storage_stages |= 1u << NVN_GPU_SH_VS;
    if (f.fragmentStoresAndAtomics) c->storage_stages |= 1u << NVN_GPU_SH_FS;
    c->storage_stages |= 1u << NVN_GPU_SH_CS;
    if (f.vertexPipelineStoresAndAtomics) c->atomic_stages |= 1u << NVN_GPU_SH_VS;
    if (f.fragmentStoresAndAtomics) c->atomic_stages |= 1u << NVN_GPU_SH_FS;
    c->atomic_stages |= 1u << NVN_GPU_SH_CS;

    // #1199：BC 不再按 textureCompressionBC 一刀切——原生全有或缺的格式能走 compute 解码就算支持。
    if (f.textureCompressionBC || vk->dev->bc_decode_mask) c->caps |= NVN_GPU_CAP_TEX_BC;
    if (f.textureCompressionASTC_LDR) c->caps |= NVN_GPU_CAP_TEX_ASTC;
    // B2：设备 advertise VK_GOOGLE_display_timing 才能报每帧实际上屏时刻；debug.s2a.display_timing=0
    // 是计量门，不报「没有能力」——能力位如实反映设备，时刻取不到时按契约报 UNKNOWN。
    if (vk->dev->has_display_timing) c->caps |= NVN_GPU_CAP_SHOWN_TIME;
    // HOST_IMPORT / GPU_VA / TILE_MEMORY / SAMP_MINMAX / LOD_BIAS / DEPTH_CLIP_CTL / GEOMETRY / TESSELLATION /
    // XFB / INDIRECT_COUNT / LINKED_PIPE / GPU_DURATION / GPU_TIME_HOST / DYN_* / DESWIZZLE：
    // 本段没有实现或没实测生效，一律不置位（契约：后端不做近似）。

    if (d->caps_record && d->caps_record_bytes) {
        caps_digest(d->caps_record, d->caps_record_bytes, c->caps_id);
    } else {
        nvn_gpu_caps tmp = *c;   // 摘要覆盖整份能力记录（含限值与能力位），caps_id 自己清零
        memset(tmp.caps_id, 0, sizeof tmp.caps_id);
        caps_digest(&tmp, sizeof tmp, c->caps_id);
    }
}

// ---------------------------------------------------------------- worker

static void segment_done(struct vk_segment *s, int status, int code, const char *why) {
    if (!s->done) return;
    nvn_gpu_done d;
    memset(&d, 0, sizeof d);
    d.signal = s->signal;
    d.status = (uint32_t)status;
    d.rc = rc(code);
    d.why = why;
    d.time_kind = NVN_GPU_TIME_NONE;
    d.occlusion = NULL;
    d.nocclusion = 0;
    g_in_callback++;
    s->done(s->user, &d);
    g_in_callback--;
}

static void detach_live(struct nvn_gpu_cmd_s *c) {
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (*pp) *pp = c->live_next;
    c->live_next = NULL;
    pthread_mutex_unlock(&g_mu);
}

static void finish_cmd(struct nvn_gpu_cmd_s *c, int executed) {
    if (!c) return;
    if (!executed) {
        pthread_mutex_lock(&g_mu);
        for (uint32_t i = c->nlay; i > 0; i--) c->lay[i - 1].t->layout = c->lay[i - 1].old;
        pthread_mutex_unlock(&g_mu);
    }
    detach_live(c);   // 已提交的 cmd 在 commit 时已摘下，这里兜底
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < c->nrefs; i++) unref_locked(&g, c->refs[i].obj, c->refs[i].type);
    pthread_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < c->nstaging; i++) {
        struct nvn_gpu_buf_s *b = c->staging[i].buf;
        if (!b) continue;
        pthread_mutex_lock(&g_mu);
        unlink_buf_locked(&g, b);
        pthread_mutex_unlock(&g_mu);
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        if (b->buffer) vkDestroyBuffer(g.d, b->buffer, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
    }
    for (uint32_t i = 0; i < c->npasses; i++) {
        if (c->passes[i].fb) vkDestroyFramebuffer(g.d, c->passes[i].fb, NULL);
        if (c->passes[i].rp) vkDestroyRenderPass(g.d, c->passes[i].rp, NULL);
    }
    free(c->passes);
    for (int i = 0; i < 6; i++) free(c->push_data[i]);
    if (c->dpool) vkDestroyDescriptorPool(g.d, c->dpool, NULL);
    if (c->pool) vkDestroyCommandPool(g.d, c->pool, NULL);   // pool 同时回收 command buffer
    free(c->refs);
    free(c->staging);
    free(c);
}

static void process_segment(struct nvn_gpu_vk *vk, struct vk_segment *s) {
    int failed = s->failed;
    nvn_gpu_rc code = s->rc;
    const char *why = s->why[0] ? s->why : "段未执行";

    if (!failed) {
        pthread_mutex_lock(&g_mu);
        uint64_t fail_sig = g.test_fail_signal, delay_sig = g.test_delay_signal;
        uint32_t delay_ms = g.test_delay_ms;
        pthread_mutex_unlock(&g_mu);
        if (delay_ms && delay_sig == s->signal) {
            struct timespec ts = {(time_t)(delay_ms / 1000), (long)(delay_ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        VkResult r = VK_TIMEOUT;
        while (!vk->stop && !vk->device_lost) {
            r = vkWaitForFences(vk->d, 1, &s->fence, VK_TRUE, 50ull * 1000ull * 1000ull);
            if (r != VK_TIMEOUT) break;
        }
        // 测试钩子：真实栅栏必须先完成（否则 fence / command buffer 还在途，不能销毁），
        // 完成后再把它当成失败，走真实的失败终止路径。
        if (r == VK_SUCCESS && fail_sig && fail_sig == s->signal) r = (VkResult)VK_ERROR_UNKNOWN;
        if (r != VK_SUCCESS) {
            pthread_mutex_lock(&g_mu);
            if (r == VK_ERROR_DEVICE_LOST) vk->device_lost = 1;
            if (!vk->failed || s->signal < vk->failed) vk->failed = s->signal;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
            // #1456：真实 DEVICE_LOST 回写当前指纹 capprobe 结果（source=runtime-lost，
            // 只记指纹不记游戏），下次启动直接走规避不再探；JNI 回呼不持 g_mu。
            if (r == VK_ERROR_DEVICE_LOST && s2a_capprobe_runtime_lost) s2a_capprobe_runtime_lost();
            failed = 1;
            code = rc(r == VK_ERROR_DEVICE_LOST ? NVN_GPU_E_DEVICE : NVN_GPU_E_FAILED);
            why = r == VK_ERROR_DEVICE_LOST ? "设备丢失" : "等待栅栏失败";
        } else {
            // 前面的段已经把时间线打进失败终态时，本段即使执行完也不许发布成功（A4）。
            pthread_mutex_lock(&g_mu);
            int terminal = vk->failed && s->signal >= vk->failed;
            pthread_mutex_unlock(&g_mu);
            if (terminal) {
                failed = 1;
                code = rc(NVN_GPU_E_FAILED);
                why = "时间线已失败，本段不发布成功";
            }
        }
    }

    if (failed) {
        segment_done(s, NVN_GPU_DONE_FAILED, code.code, why);
        pthread_mutex_lock(&g_mu);
        vk->stats[STAT_DONE_FAILED]++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    } else {
        // 发布顺序：先 done（核心回写），done 返回后才推进已完成值。
        segment_done(s, NVN_GPU_DONE_OK, NVN_GPU_OK, NULL);
        pthread_mutex_lock(&g_mu);
        if (s->signal > 0 && s->signal > vk->completed) vk->completed = s->signal;
        submitted_compact_locked(vk);
        vk->stats[STAT_DONE_OK]++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    if (s->fence) vkDestroyFence(vk->d, s->fence, NULL);
    finish_cmd(s->cmd, s->executed);
    free(s);
}

static void *worker_main(void *arg) {
    struct nvn_gpu_vk *vk = arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (vk->rhead) {
            struct nvn_gpu_buf_s *b = vk->rhead;
            vk->rhead = b->retire_next;
            if (!vk->rhead) vk->rtail = NULL;
            b->retire_next = NULL;
            pthread_mutex_unlock(&g_mu);
            retire_one(vk, b);
            pthread_mutex_lock(&g_mu);
            continue;
        }
        if (vk->qhead) {
            struct vk_segment *s = vk->qhead;
            vk->qhead = s->next;
            if (!vk->qhead) vk->qtail = NULL;
            vk->busy = 1;
            pthread_mutex_unlock(&g_mu);
            process_segment(vk, s);
            pthread_mutex_lock(&g_mu);
            vk->busy = 0;
            pthread_cond_broadcast(&g_cv);
            continue;
        }
        if (vk->stop) break;
        struct timespec ts;
        deadline_after(&ts, VK_WAIT_SLICE_NS);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

// ---------------------------------------------------------------- 打开 / 关闭 / 能力

nvn_gpu_rc nvn_gpu_open(const nvn_gpu_open_desc *d, nvn_gpu_err *err) {
    if (g_open) return fail(err, NVN_GPU_E_ORDER, "已有打开的后端");
    if (!d || !d->host_device) return fail(err, NVN_GPU_E_ARG, "host_device 为空");
    struct s2a_vk_device *dev = d->host_device;
    if (!dev->instance || !dev->physical || !dev->device || !dev->queue)
        return fail(err, NVN_GPU_E_ARG, "host_device 不是已建好的 Vulkan 设备上下文");

    memset(&g, 0, sizeof g);
    g.dev = dev;
    g.d = dev->device;
    g.phys = dev->physical;
    g.queue = dev->queue;
    if (!g_cv_ready) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   // 与 deadline_after 同一时钟
        if (pthread_cond_init(&g_cv, &ca) != 0) {
            pthread_condattr_destroy(&ca);
            return fail(err, NVN_GPU_E_DEVICE, "cond 初始化失败");
        }
        pthread_condattr_destroy(&ca);
        g_cv_ready = 1;
    }
    fill_caps(&g, d);
    if (pthread_create(&g.worker, NULL, worker_main, &g) != 0)
        return fail(err, NVN_GPU_E_DEVICE, "worker 线程创建失败");
    g.worker_started = 1;
    g_open = 1;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_caps_get(nvn_gpu_caps *out) {
    if (!out) return;
    if (!g_open) {
        memset(out, 0, sizeof *out);
        return;
    }
    pthread_mutex_lock(&g_mu);
    *out = g.caps;
    pthread_mutex_unlock(&g_mu);
}

uint32_t nvn_gpu_format_caps(uint32_t nvn_format) {
    if (!g_open) return 0;
    nvn_fmt_info info;
    int expand = nvn_fmt_info_of(nvn_format, &info) && info.expand;
    VkFormat f = nvn_vk_tex_format(nvn_format);
    if (f == VK_FORMAT_UNDEFINED) return 0;
    // #1199：走 compute 解码的 BC 格式按解码后格式报能力（图像实际就是该格式）。
    if (nvn_format >= 0x41 && nvn_format <= 0x50 && (g.dev->bc_decode_mask >> (nvn_format - 0x41)) & 1u) {
        VkFormat dfmt = VK_FORMAT_UNDEFINED;
        if (!nvn_vk_bcdec_route(nvn_format, NULL, &dfmt, NULL, NULL, NULL) || dfmt == VK_FORMAT_UNDEFINED) return 0;
        f = dfmt;
    }
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &p);
    VkFormatFeatureFlags o = p.optimalTilingFeatures;
    uint32_t out = 0;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) out |= NVN_GPU_FMT_SAMPLE;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) out |= NVN_GPU_FMT_FILTER;
    if (o & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) out |= NVN_GPU_FMT_RENDER;
    if (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) out |= NVN_GPU_FMT_BLEND;
    if (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) out |= NVN_GPU_FMT_STORAGE;
    if (nvn_vk_format_is_depth(f)) out |= NVN_GPU_FMT_DEPTH;
    if (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) out |= NVN_GPU_FMT_ATOMIC;
    if (o & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) out |= NVN_GPU_FMT_VERTEX;
    if (expand) {
        out |= NVN_GPU_FMT_SUBST;   // 存储被换成展开目标，逐字节拷贝要核心换算
        VkFormat sf = nvn_vk_format(nvn_format);
        VkFormatProperties sp;
        memset(&sp, 0, sizeof sp);
        if (sf != VK_FORMAT_UNDEFINED) vkGetPhysicalDeviceFormatProperties(g.phys, sf, &sp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) out &= ~NVN_GPU_FMT_VERTEX;
    }
    return out;
}

nvn_gpu_rc nvn_gpu_close(uint64_t timeout_ns, nvn_gpu_err *err) {
    if (!g_open) return rc(NVN_GPU_OK);
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许 close");
    pthread_mutex_lock(&g_mu);
    if (g.close_active) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ORDER, "另一个线程正在 close");
    }
    g.close_active = 1;
    g.closing = 1;
    pthread_mutex_unlock(&g_mu);
    uint64_t deadline = add_sat(now_ns(), timeout_ns);

    // ① 丢弃未提交 cmd：原子摘链后由本线程独占销毁，避免与并发 abort 重复释放。
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s *list = g.live;
    g.live = NULL;
    pthread_mutex_unlock(&g_mu);
    while (list) {
        struct nvn_gpu_cmd_s *next = list->live_next;
        list->live_next = NULL;
        finish_cmd(list, 0);
        list = next;
    }

    // ②③④ 等在途段 / 退休队列 / 并发 commit 全部结束。
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (!g.qhead && !g.busy && !g.rhead && g.commit_active == 0) {
            // 先销毁视图、放下其持有的底层缓冲引用，再由 worker 完成退休回调。
            while (g.views) destroy_view_locked(&g, g.views);
            if (!g.rhead) break;
        }
        uint64_t now = now_ns();
        if (now >= deadline) {
            g.close_active = 0;
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_TIMEOUT, "close 时限内未完成在途段 / 退休 / 提交（保留资源，可再调一次）");
        }
        struct timespec ts;
        deadline_after(&ts, deadline - now);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    g.stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g.worker, NULL);
    g.worker_started = 0;

    // ⑤ 销毁所有仍活着的后端对象；已 release 的 retired 已由 worker 调完。
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_buf_s *b = g.bufs;
    g.bufs = NULL;
    g.live = NULL;
    pthread_mutex_unlock(&g_mu);
    while (b) {
        struct nvn_gpu_buf_s *next = b->all_next;
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        if (b->buffer) vkDestroyBuffer(g.d, b->buffer, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
        b = next;
    }
    struct nvn_gpu_pipe_s *p = g.pipes;
    g.pipes = NULL;
    while (p) {
        struct nvn_gpu_pipe_s *pn = p->all_next;
        destroy_pipe_objects(p);
        p = pn;
    }
    struct nvn_gpu_shader_s *sh = g.shaders;
    g.shaders = NULL;
    while (sh) {
        struct nvn_gpu_shader_s *sn = sh->all_next;
        if (sh->mod) vkDestroyShaderModule(g.d, sh->mod, NULL);
        free(sh->ents);
        free(sh->tfmt);
        free(sh);
        sh = sn;
    }
    struct nvn_gpu_samp_s *sp = g.samps;
    g.samps = NULL;
    while (sp) {
        struct nvn_gpu_samp_s *sn = sp->all_next;
        if (sp->sampler) vkDestroySampler(g.d, sp->sampler, NULL);
        free(sp);
        sp = sn;
    }
    struct nvn_gpu_view_s *vw = g.views;
    g.views = NULL;
    while (vw) {
        struct nvn_gpu_view_s *vn = vw->all_next;
        if (vw->view) vkDestroyImageView(g.d, vw->view, NULL);
        free(vw);
        vw = vn;
    }
    struct nvn_gpu_tex_s *tx = g.texs;
    g.texs = NULL;
    while (tx) {
        struct nvn_gpu_tex_s *tn = tx->all_next;
        if (tx->view) vkDestroyImageView(g.d, tx->view, NULL);
        if (tx->image) vkDestroyImage(g.d, tx->image, NULL);
        if (tx->memory) vkFreeMemory(g.d, tx->memory, NULL);
        free(tx);
        tx = tn;
    }
    free(g.submitted);
    g.submitted = NULL;
    g.nsub = g.capsub = 0;
    g.closed = 1;
    g.close_active = 0;
    g_open = 0;
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 缓冲

static int is_shared(int mem) { return mem == NVN_GPU_MEM_SHARED; }

static int session_accepting(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = !g.closing;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

nvn_gpu_rc nvn_gpu_buf_create(uint64_t bytes, int mem, const void *init, nvn_gpu_buf *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    pthread_mutex_lock(&g_mu);
    int closing = g.closing;
    pthread_mutex_unlock(&g_mu);
    if (closing) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    if (bytes == 0 || bytes > g.caps.max_buf_bytes) return fail(err, NVN_GPU_E_ARG, "bytes 非法：%llu", (unsigned long long)bytes);
    if (mem != NVN_GPU_MEM_SHARED && mem != NVN_GPU_MEM_DEVICE) return fail(err, NVN_GPU_E_ARG, "mem 非法");
    if (mem == NVN_GPU_MEM_DEVICE && init) return fail(err, NVN_GPU_E_ARG, "DEVICE 内存不能带 init");

    struct nvn_gpu_buf_s *b = calloc(1, sizeof *b);
    if (!b) return fail(err, NVN_GPU_E_OOM, "缓冲结构分配失败");
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.d, &bi, NULL, &b->buffer) != VK_SUCCESS) {
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkCreateBuffer 失败（%llu 字节）", (unsigned long long)bytes);
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.d, b->buffer, &req);
    VkMemoryPropertyFlags want = is_shared(mem) ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, want, NULL);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "找不到满足要求的显存类型");
    }
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g.d, &ai, NULL, &b->memory) != VK_SUCCESS) {
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkAllocateMemory 失败");
    }
    if (vkBindBufferMemory(g.d, b->buffer, b->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(g.d, b->memory, NULL);
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkBindBufferMemory 失败");
    }
    b->bytes = bytes;
    b->mem_kind = mem;
    if (is_shared(mem)) {
        if (vkMapMemory(g.d, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->mapped) != VK_SUCCESS) {
            vkFreeMemory(g.d, b->memory, NULL);
            vkDestroyBuffer(g.d, b->buffer, NULL);
            free(b);
            return fail(err, NVN_GPU_E_OOM, "vkMapMemory 失败");
        }
        if (init) memcpy(b->mapped, init, (size_t)bytes);
    }
    pthread_mutex_lock(&g_mu);
    if (g.closing || g.closed) {   // 与 close 的快照串行：close 开始后新建的对象不留在账上
        pthread_mutex_unlock(&g_mu);
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        vkDestroyBuffer(g.d, b->buffer, NULL);
        vkFreeMemory(g.d, b->memory, NULL);
        free(b);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    b->all_next = g.bufs;
    g.bufs = b;
    g.stats[STAT_BUFS]++;
    pthread_mutex_unlock(&g_mu);
    *out = b;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_buf_wrap(void *host, uint64_t bytes, nvn_gpu_buf *out, nvn_gpu_err *err) {
    (void)host;
    (void)bytes;
    if (out) *out = NULL;
    return fail(err, NVN_GPU_E_UNSUPPORTED, "本设备 / 本段未启用 HOST_IMPORT（VK_EXT_external_memory_host 未实测）");
}

uint64_t nvn_gpu_buf_va(nvn_gpu_buf b) { return b ? b->va : 0; }   // 未开 CAP_GPU_VA：恒 0
uint8_t *nvn_gpu_buf_cpu(nvn_gpu_buf b) { return b ? b->mapped : NULL; }

void nvn_gpu_buf_release(nvn_gpu_buf bh, nvn_gpu_retired_fn retired, void *user) {
    if (!bh) return;
    struct nvn_gpu_buf_s *b = bh;
    pthread_mutex_lock(&g_mu);
    if (!b->released) {
        b->released = 1;
        b->retired = retired;
        b->retired_user = user;
        if (b->refs == 0) queue_retire_locked(&g, b);
    }
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_buf_use(nvn_gpu_cmd ch, nvn_gpu_buf bh) {
    if (!ch || !bh) return;
    cmd_ref((struct nvn_gpu_cmd_s *)ch, bh, REF_BUF);
}

// ---------------------------------------------------------------- arena

static struct nvn_gpu_buf_s *new_staging(struct nvn_gpu_cmd_s *c, uint64_t cap) {
    nvn_gpu_err e;
    nvn_gpu_buf b = NULL;
    if (!rc_is(nvn_gpu_buf_create(cap, NVN_GPU_MEM_SHARED, NULL, &b, &e), NVN_GPU_OK)) return NULL;
    if (c->nstaging == c->scap) {
        uint32_t n = c->scap ? c->scap * 2 : 4;
        struct vk_staging *p = realloc(c->staging, (size_t)n * sizeof *p);
        if (!p) {
            nvn_gpu_buf_release(b, NULL, NULL);
            return NULL;
        }
        c->staging = p;
        c->scap = n;
    }
    c->staging[c->nstaging++].buf = b;
    return b;
}

nvn_gpu_rc nvn_gpu_arena_alloc(nvn_gpu_cmd ch, uint64_t bytes, uint32_t align, nvn_gpu_slice *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    memset(out, 0, sizeof *out);
    if (!ch) return fail(err, NVN_GPU_E_ARG, "cmd 为空");
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return fail(err, c->sticky.code, "%s", c->why);
    if (!g_open || g.closing) return fail(err, NVN_GPU_E_STOPPED, "会话已开始关闭");
    if (bytes == 0) return fail(err, NVN_GPU_E_ARG, "bytes 为 0");
    if (align == 0) align = 4;
    if (align & (align - 1)) return fail(err, NVN_GPU_E_ARG, "align 不是 2 的幂：%u", align);
    uint64_t off = (c->arena_used + align - 1) & ~(uint64_t)(align - 1);
    if (!c->arena || off + bytes > c->arena_cap) {
        uint64_t cap = bytes + align;
        if (cap < VK_ARENA_MIN) cap = VK_ARENA_MIN;
        struct nvn_gpu_buf_s *b = new_staging(c, cap);
        if (!b) return fail(err, NVN_GPU_E_OOM, "arena %llu 字节分配失败", (unsigned long long)cap);
        c->arena = b;
        c->arena_cap = cap;
        c->arena_used = 0;
        off = 0;
    }
    out->buf = c->arena;
    out->off = off;
    out->cpu = c->arena->mapped + off;
    out->va = 0;
    c->arena_used = off + bytes;
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 录制原语

// 保守的全局内存屏障：传输操作前后与段末各来一次（先保证正确，后面再按访问类型收窄）。
static void mem_barrier(struct nvn_gpu_cmd_s *c, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                        VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(c->cb, src_stage, dst_stage, 0, 1, &b, 0, NULL, 0, NULL);
}
static void full_barrier(struct nvn_gpu_cmd_s *c) {
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

static int buf_range(const struct nvn_gpu_buf_s *b, uint64_t off, uint64_t bytes) {
    return b && off <= b->bytes && bytes <= b->bytes - off;
}

void nvn_gpu_buf_write(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, const void *src, uint64_t bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *b = bh;
    if (!b || (!src && bytes)) { set_sticky(c, NVN_GPU_E_ARG, "buf_write 句柄 / src 为空"); return; }
    if (!buf_range(b, off, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "buf_write 越界 off=%llu bytes=%llu size=%llu",
                                                   (unsigned long long)off, (unsigned long long)bytes, (unsigned long long)b->bytes); return; }
    if (bytes == 0) return;
    nvn_gpu_slice sl;
    nvn_gpu_err e;
    if (!rc_is(nvn_gpu_arena_alloc(ch, bytes, 4, &sl, &e), NVN_GPU_OK)) {
        set_sticky(c, e.rc.code, "buf_write 暂存：%s", e.why);
        return;
    }
    memcpy(sl.cpu, src, (size_t)bytes);
    full_barrier(c);
    VkBufferCopy bc = {sl.off, off, bytes};
    vkCmdCopyBuffer(c->cb, sl.buf->buffer, b->buffer, 1, &bc);
    cmd_ref(c, b, REF_BUF);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_WRITES]++;
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_fill_buf(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t bytes, uint32_t value) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *b = bh;
    if (!b || !buf_range(b, off, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "fill_buf 越界"); return; }
    if ((off & 3) || (bytes & 3)) { set_sticky(c, NVN_GPU_E_ARG, "fill_buf 需 4 字节对齐 off=%llu bytes=%llu",
                                                  (unsigned long long)off, (unsigned long long)bytes); return; }
    if (bytes == 0) return;
    full_barrier(c);
    vkCmdFillBuffer(c->cb, b->buffer, off, bytes, value);
    cmd_ref(c, b, REF_BUF);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_FILLS]++;
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_copy_buf(nvn_gpu_cmd ch, nvn_gpu_buf src, uint64_t soff, nvn_gpu_buf dst, uint64_t doff, uint64_t bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *s = src, *d = dst;
    if (!s || !d || !buf_range(s, soff, bytes) || !buf_range(d, doff, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "copy_buf 越界"); return; }
    if (bytes & 3) { set_sticky(c, NVN_GPU_E_ARG, "copy_buf 需 4 字节长度（本段实现限制）bytes=%llu", (unsigned long long)bytes); return; }
    if (bytes == 0) return;
    full_barrier(c);
    VkBufferCopy bc = {soff, doff, bytes};
    vkCmdCopyBuffer(c->cb, s->buffer, d->buffer, 1, &bc);
    cmd_ref(c, s, REF_BUF);
    cmd_ref(c, d, REF_BUF);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_COPIES]++;
    pthread_mutex_unlock(&g_mu);
}


// ---------------------------------------------------------------- 着色器 / 管线 / 通道 / 绘制（纹理三角形）

static VkShaderStageFlagBits vk_shader_stage(nvn_gpu_stage st) {
    switch (st) {
    case NVN_GPU_SH_VS: return VK_SHADER_STAGE_VERTEX_BIT;
    case NVN_GPU_SH_FS: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case NVN_GPU_SH_CS: return VK_SHADER_STAGE_COMPUTE_BIT;
    default: return 0;
    }
}

static VkPrimitiveTopology vk_topology(uint32_t prim) {
    switch (prim) {
    case 0x0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 0x1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 0x3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 0x4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 0x5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 0x6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // 2 线环 / 7+：未实现
    }
}

static VkBlendFactor vk_blend_factor(uint32_t f) {
    switch (f) {
    case 0x1: return VK_BLEND_FACTOR_ZERO;
    case 0x2: return VK_BLEND_FACTOR_ONE;
    case 0x3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x9: return VK_BLEND_FACTOR_DST_COLOR;
    case 0xA: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_MAX_ENUM;
    }
}
static VkBlendOp vk_blend_op(uint32_t e) {
    switch (e) {
    case 0x1: return VK_BLEND_OP_ADD;
    case 0x2: return VK_BLEND_OP_SUBTRACT;
    case 0x3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x4: return VK_BLEND_OP_MIN;
    case 0x5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_MAX_ENUM;
    }
}
static VkCompareOp vk_depth_func(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkDescriptorType vk_desc_type(uint8_t cls) {
    switch (cls) {
    case NVN_GPU_RES_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case NVN_GPU_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case NVN_GPU_RES_TEX: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case NVN_GPU_RES_SAMP: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case NVN_GPU_RES_TEX_SAMP: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case NVN_GPU_RES_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case NVN_GPU_RES_TBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case NVN_GPU_RES_IMAGE_TBUF: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

// ---------------- SPIR-V texel 反射（只读 blob；未知格式由绑定的 view 提供） ----------------
// 只解析 TBUF / IMAGE_TBUF 需要的最小指令子集：
//   OpDecorate(Binding/DescriptorSet) → OpVariable(UniformConstant) → OpTypePointer → (OpTypeArray) → OpTypeImage。
// 约定（v2 未定，报 PR 请契约属主复核）：
//   · OpTypeImage.Dim 必须为 Buffer（一维缓冲纹理只能映射成 texel buffer）；
//   · TBUF 收 Sampled=0/1（采样读），IMAGE_TBUF 收 Sampled=0/2（存储写）；
//   · ImageFormat 非 Unknown 按 SPIR-V 值映射；Unknown 留给绑定视图，不猜格式；
//   · 项与 blob 对不上 → E_LAYOUT（逐项核对的 A1 要求）；数值类 / 位宽推不出 → E_UNSUPPORTED。
enum { SPV_OP_CAPABILITY = 17, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_IMAGE = 25,
       SPV_OP_TYPE_SAMPLED_IMAGE = 27, SPV_OP_TYPE_ARRAY = 28, SPV_OP_TYPE_RUNTIME_ARRAY = 29,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_VARIABLE = 59, SPV_OP_DECORATE = 71 };
enum { SPV_DEC_BINDING = 33, SPV_DEC_SET = 34 };
enum { SPV_DIM_BUFFER = 5, SPV_SC_UNIFORM_CONST = 0 };   // SPIR-V Dim：0-3 维度、4=Rect、5=Buffer

struct spv_id {
    uint8_t kind;          // 1 int 2 float 3 vector 4 image 5 array 7 pointer 8 variable 9 sampled_image
    uint8_t sgn, sampled, dim;
    uint32_t a;            // int/float: 位宽；vector: 分量类型 id；image: sampled 类型 id；array: elem id；ptr: pointee id；var: 指针类型 id
    uint32_t fmt;          // image: ImageFormat 操作数
};

// SPIR-V ImageFormat 操作数（非 Unknown）→ VkFormat
static VkFormat spv_img_fmt(uint32_t f) {
    switch (f) {
    case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 3: return VK_FORMAT_R32_SFLOAT;
    case 4: return VK_FORMAT_R8G8B8A8_UNORM;
    case 5: return VK_FORMAT_R8G8B8A8_SNORM;
    case 6: return VK_FORMAT_R32G32_SFLOAT;
    case 7: return VK_FORMAT_R16G16_SFLOAT;
    case 8: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 9: return VK_FORMAT_R16_SFLOAT;
    case 10: return VK_FORMAT_R16G16B16A16_UNORM;
    case 11: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 12: return VK_FORMAT_R16G16_UNORM;
    case 13: return VK_FORMAT_R8G8_UNORM;
    case 14: return VK_FORMAT_R16_UNORM;
    case 15: return VK_FORMAT_R8_UNORM;
    case 16: return VK_FORMAT_R16G16B16A16_SNORM;
    case 17: return VK_FORMAT_R16G16_SNORM;
    case 18: return VK_FORMAT_R8G8_SNORM;
    case 19: return VK_FORMAT_R16_SNORM;
    case 20: return VK_FORMAT_R8_SNORM;
    case 21: return VK_FORMAT_R32G32B32A32_SINT;
    case 22: return VK_FORMAT_R16G16B16A16_SINT;
    case 23: return VK_FORMAT_R8G8B8A8_SINT;
    case 24: return VK_FORMAT_R32_SINT;
    case 25: return VK_FORMAT_R32G32_SINT;
    case 26: return VK_FORMAT_R16G16_SINT;
    case 27: return VK_FORMAT_R8G8_SINT;
    case 28: return VK_FORMAT_R16_SINT;
    case 29: return VK_FORMAT_R8_SINT;
    case 30: return VK_FORMAT_R32G32B32A32_UINT;
    case 31: return VK_FORMAT_R16G16B16A16_UINT;
    case 32: return VK_FORMAT_R8G8B8A8_UINT;
    case 33: return VK_FORMAT_R32_UINT;
    case 34: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 35: return VK_FORMAT_R32G32_UINT;
    case 36: return VK_FORMAT_R16G16_UINT;
    case 37: return VK_FORMAT_R8G8_UINT;
    case 38: return VK_FORMAT_R16_UINT;
    case 39: return VK_FORMAT_R8_UINT;
    case 40: return VK_FORMAT_R64_UINT;
    case 41: return VK_FORMAT_R64_SINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// Vulkan 1.1 设备级 without-format 特性适用的格式；1..39 与规范列表对应。
static int texel_unknown_storage_supported(VkFormat f) {
    for (uint32_t i = 1; i <= 39; i++)
        if (spv_img_fmt(i) == f) return 1;
    return 0;
}

static nvn_gpu_rc spv_texel_formats(const void *blob, size_t bytes,
                                    const nvn_gpu_layout_ent *ents, uint32_t nent, VkFormat *tfmt,
                                    uint32_t *required, nvn_gpu_err *err) {
    const uint32_t *w = blob;
    if (bytes < 20 || w[0] != 0x07230203u) return fail(err, NVN_GPU_E_ARG, "SPIR-V 头非法");
    uint32_t bound = w[3];
    if (!bound || bound > 0x100000) return fail(err, NVN_GPU_E_ARG, "SPIR-V id 上界非法：%u", bound);
    struct spv_id *ids = calloc(bound, sizeof *ids);
    uint32_t *vset = malloc(bound * sizeof *vset), *vbind = malloc(bound * sizeof *vbind);
    if (!ids || !vset || !vbind) {
        free(ids); free(vset); free(vbind);
        return fail(err, NVN_GPU_E_OOM, "SPIR-V 反射暂存分配失败");
    }
    for (uint32_t i = 0; i < bound; i++) vset[i] = vbind[i] = UINT32_MAX;
    size_t pos = 5, n = bytes / 4;
    while (pos < n) {
        uint32_t w0 = w[pos], op = w0 & 0xFFFFu, wc = w0 >> 16;
        if (!wc || pos + wc > n) break;
        const uint32_t *o = w + pos + 1;
        switch (op) {
        case SPV_OP_CAPABILITY:
            if (wc >= 2 && o[0] == 55) *required |= NVN_GPU_ACC_R;
            if (wc >= 2 && o[0] == 56) *required |= NVN_GPU_ACC_W;
            break;
        case SPV_OP_TYPE_INT:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 1; t->a = o[1]; t->sgn = (uint8_t)o[2]; }
            break;
        case SPV_OP_TYPE_FLOAT:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 2; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_VECTOR:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 3; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_IMAGE:
            if (wc >= 9 && o[0] < bound) {
                struct spv_id *t = &ids[o[0]];
                t->kind = 4; t->a = o[1]; t->dim = (uint8_t)o[2]; t->sampled = (uint8_t)o[6]; t->fmt = o[7];
            }
            break;
        case SPV_OP_TYPE_SAMPLED_IMAGE:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 9; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_ARRAY:
        case SPV_OP_TYPE_RUNTIME_ARRAY:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 5; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_POINTER:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 7; t->a = o[2]; }
            break;
        case SPV_OP_VARIABLE:
            if (wc >= 4 && o[1] < bound && o[2] == SPV_SC_UNIFORM_CONST) {
                struct spv_id *t = &ids[o[1]];
                t->kind = 8; t->a = o[0];
            }
            break;
        case SPV_OP_DECORATE:
            if (wc >= 4 && o[0] < bound) {
                if (o[1] == SPV_DEC_BINDING) vbind[o[0]] = o[2];
                else if (o[1] == SPV_DEC_SET) vset[o[0]] = o[2];
            }
            break;
        default: break;
        }
        pos += wc;
    }
    nvn_gpu_rc r = rc(NVN_GPU_OK);
    for (uint32_t i = 0; i < nent; i++) {
        const nvn_gpu_layout_ent *e = &ents[i];
        if (e->cls != NVN_GPU_RES_TBUF && e->cls != NVN_GPU_RES_IMAGE_TBUF) continue;
        uint32_t v = UINT32_MAX;
        for (uint32_t j = 0; j < bound; j++)
            if (ids[j].kind == 8 && vset[j] == e->set && vbind[j] == e->binding) { v = j; break; }
        if (v == UINT32_MAX) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) 在 blob 里无对应 OpVariable", e->set, e->binding);
            break;
        }
        uint32_t t = ids[v].a, guard = 0;
        while (t < bound && (ids[t].kind == 7 || ids[t].kind == 5 || ids[t].kind == 9) && ++guard <= 16) t = ids[t].a;
        if (t >= bound || ids[t].kind != 4) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) 的类型链解不到 OpTypeImage", e->set, e->binding);
            break;
        }
        if (ids[t].dim != SPV_DIM_BUFFER) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Dim=%u 不是 Buffer", e->set, e->binding, ids[t].dim);
            break;
        }
        if (e->cls == NVN_GPU_RES_TBUF && ids[t].sampled > 1) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Sampled=%u 与 TBUF（采样读）不符", e->set, e->binding, ids[t].sampled);
            break;
        }
        if (e->cls == NVN_GPU_RES_IMAGE_TBUF && ids[t].sampled == 1) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Sampled=1 与 IMAGE_TBUF（存储写）不符", e->set, e->binding);
            break;
        }
        if (e->cls == NVN_GPU_RES_IMAGE_TBUF && !ids[t].fmt)
            *required |= e->access ? e->access & NVN_GPU_ACC_RW : NVN_GPU_ACC_RW;
        VkFormat f = ids[t].fmt ? spv_img_fmt(ids[t].fmt) : VK_FORMAT_UNDEFINED;
        if (ids[t].fmt && f == VK_FORMAT_UNDEFINED) {
            r = fail(err, NVN_GPU_E_UNSUPPORTED, "texel 布局项 (set %u, binding %u) 格式推不出（ImageFormat=%u）", e->set, e->binding, ids[t].fmt);
            break;
        }
        tfmt[i] = f;
    }
    free(ids);
    free(vset);
    free(vbind);
    return r;
}

static void destroy_shader_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_shader_s *sh) {
    struct nvn_gpu_shader_s **pp = &vk->shaders;
    while (*pp && *pp != sh) pp = &(*pp)->all_next;
    if (*pp) *pp = sh->all_next;
    if (sh->mod) vkDestroyShaderModule(vk->d, sh->mod, NULL);
    free(sh->ents);
    free(sh->tfmt);
    free(sh);
}

nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!s || !s->blob || !s->bytes) return fail(err, NVN_GPU_E_ARG, "着色器 blob 为空");
    if (s->kind != NVN_GPU_BLOB_SPIRV) return fail(err, NVN_GPU_E_UNSUPPORTED, "只收 SPIR-V blob（kind=%d）", s->kind);
    if (s->bytes & 3) return fail(err, NVN_GPU_E_ARG, "SPIR-V 字节数不是 4 的倍数");
    struct nvn_gpu_shader_s *sh = calloc(1, sizeof *sh);
    if (!sh) return fail(err, NVN_GPU_E_OOM, "着色器结构分配失败");
    sh->stage = s->stage;
    snprintf(sh->entry, sizeof sh->entry, "%s", s->entry && s->entry[0] ? s->entry : "main");
    if (s->layout) {
        sh->nent = s->layout->nent;
        sh->push_off = s->layout->push_off;
        sh->push_bytes = s->layout->push_bytes;
        if (sh->nent) {
            sh->ents = calloc(sh->nent, sizeof *sh->ents);
            if (!sh->ents) {
                free(sh);
                return fail(err, NVN_GPU_E_OOM, "布局拷贝分配失败");
            }
            memcpy(sh->ents, s->layout->ent, (size_t)sh->nent * sizeof *sh->ents);
        }
        int need_tfmt = 0;
        for (uint32_t i = 0; i < sh->nent; i++)
            if (sh->ents[i].cls == NVN_GPU_RES_TBUF || sh->ents[i].cls == NVN_GPU_RES_IMAGE_TBUF) need_tfmt = 1;
        if (need_tfmt) {
            sh->tfmt = calloc(sh->nent, sizeof *sh->tfmt);
            if (!sh->tfmt) {
                free(sh->ents);
                free(sh);
                return fail(err, NVN_GPU_E_OOM, "texel 格式表分配失败");
            }
            uint32_t required = 0;
            nvn_gpu_rc tr = spv_texel_formats(s->blob, s->bytes, sh->ents, sh->nent, sh->tfmt, &required, err);
            if (!rc_is(tr, NVN_GPU_OK)) {
                free(sh->tfmt);
                free(sh->ents);
                free(sh);
                return tr;
            }
            sh->missing_storage_format_feature =
                ((required & NVN_GPU_ACC_R) && !g.dev->core_features.shaderStorageImageReadWithoutFormat) ||
                ((required & NVN_GPU_ACC_W) && !g.dev->core_features.shaderStorageImageWriteWithoutFormat);
        }
    }
    // 没有启用能力时保留可查询的 shader 句柄，管线入口返回 UNSUPPORTED；
    // 不向驱动提交不合法 SPIR-V，以免 vkCreateShaderModule 先触发 validation。
    if (!sh->missing_storage_format_feature) {
        VkShaderModuleCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = s->bytes; ci.pCode = s->blob;
        if (vkCreateShaderModule(g.d, &ci, NULL, &sh->mod) != VK_SUCCESS) {
            free(sh->tfmt); free(sh->ents); free(sh);
            return fail(err, NVN_GPU_E_COMPILE, "vkCreateShaderModule 失败（stage %d）", s->stage);
        }
    }
    sh->refs = 1;   // 核心持有
    pthread_mutex_lock(&g_mu);
    sh->all_next = g.shaders;
    g.shaders = sh;
    pthread_mutex_unlock(&g_mu);
    *out = sh;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_shader_release(nvn_gpu_shader sh) {
    struct nvn_gpu_shader_s *s = sh;
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    if (!s->released) {
        s->released = 1;
        if (s->refs > 0) s->refs--;
        if (s->refs == 0) destroy_shader_locked(&g, s);
    }
    pthread_mutex_unlock(&g_mu);
}

static VkRenderPass make_pipeline_pass(const nvn_gpu_pipe_desc *d) {
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    uint32_t natt = 0, ncol = 0;
    VkSampleCountFlagBits samples = (VkSampleCountFlagBits)(d->samples > 1 ? d->samples : 1);
    for (uint32_t i = 0; i < 8; i++) {
        if (!d->cfmt[i]) break;
        VkFormat f = nvn_vk_tex_format(d->cfmt[i]);
        if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = f;
        att[natt].samples = samples;
        att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col[ncol].attachment = natt;
        col[ncol].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ncol++;
        natt++;
    }
    int has_depth = 0;
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (d->dfmt) {
        VkFormat f = nvn_vk_tex_format(d->dfmt);
        if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = f;
        att[natt].samples = samples;
        att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref.attachment = natt;
        has_depth = 1;
        natt++;
    }
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = ncol;
    sp.pColorAttachments = ncol ? col : NULL;
    sp.pDepthStencilAttachment = has_depth ? &dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rp;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = natt;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(g.d, &rp, NULL, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

static int same_spec(const nvn_gpu_stage_desc *a, const nvn_gpu_stage_desc *b) {
    if (a->sh != b->sh || a->nspec != b->nspec) return 0;
    return a->nspec == 0 || memcmp(a->spec, b->spec, (size_t)a->nspec * sizeof *a->spec) == 0;
}

static int same_pipe_desc(const nvn_gpu_pipe_desc *a, const nvn_gpu_pipe_desc *b) {
    if (a->prim != b->prim || a->patch_size != b->patch_size || a->dfmt != b->dfmt || a->sfmt != b->sfmt ||
        a->samples != b->samples || a->rast_discard != b->rast_discard || a->clip_neg1 != b->clip_neg1)
        return 0;
    for (int i = 0; i < 5; i++)
        if (!same_spec(&a->st[i], &b->st[i])) return 0;
    for (int i = 0; i < 8; i++)
        if (a->cfmt[i] != b->cfmt[i]) return 0;
    return memcmp(a->lmap, b->lmap, sizeof a->lmap) == 0 &&
           memcmp(a->blend, b->blend, sizeof a->blend) == 0 &&
           memcmp(&a->color, &b->color, sizeof a->color) == 0 &&
           memcmp(&a->chmask, &b->chmask, sizeof a->chmask) == 0 &&
           memcmp(&a->ms, &b->ms, sizeof a->ms) == 0 &&
           memcmp(a->attr, b->attr, sizeof a->attr) == 0 &&
           memcmp(a->stream, b->stream, sizeof a->stream) == 0 &&
           memcmp(&a->ds, &b->ds, sizeof a->ds) == 0 &&
           memcmp(&a->poly, &b->poly, sizeof a->poly) == 0;
}

static void destroy_pipe_objects(struct nvn_gpu_pipe_s *p) {
    if (!p) return;
    if (p->pipe) vkDestroyPipeline(g.d, p->pipe, NULL);
    if (p->pass) vkDestroyRenderPass(g.d, p->pass, NULL);
    if (p->layout) vkDestroyPipelineLayout(g.d, p->layout, NULL);
    for (uint32_t i = 0; i < p->nset; i++)
        if (p->dsl[i]) vkDestroyDescriptorSetLayout(g.d, p->dsl[i], NULL);
    for (int i = 0; i < 6; i++) { free(p->stage_ents[i]); free(p->stage_tfmt[i]); }
    free(p);
}

nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    (void)mode;
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!d) return fail(err, NVN_GPU_E_ARG, "管线描述为空");
    if (!d->st[NVN_GPU_SH_VS].sh || !d->st[NVN_GPU_SH_FS].sh)
        return fail(err, NVN_GPU_E_ARG, "图形管线至少需要 VS 与 FS");
    for (int st = 0; st < 5; st++)
        if (d->st[st].sh && d->st[st].sh->missing_storage_format_feature)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "设备未启用无格式存储图像所需特性");
    for (int i = 2; i < 5; i++)
        if (d->st[i].sh) return fail(err, NVN_GPU_E_UNSUPPORTED, "几何 / 细分阶段：本段未实现");

    pthread_mutex_lock(&g_mu);
    for (struct nvn_gpu_pipe_s *q = g.pipes; q; q = q->all_next) {
        if (same_pipe_desc(&q->key, d)) {
            pthread_mutex_unlock(&g_mu);
            *out = q;   // 后端缓存持有，核心不 release
            return rc(NVN_GPU_OK);
        }
    }
    pthread_mutex_unlock(&g_mu);

    struct merged { uint8_t set, binding, cls; uint16_t count; uint32_t stages; } m[64];
    uint32_t nm = 0, maxset = 0;
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh) continue;
        for (uint32_t i = 0; i < sh->nent; i++) {
            const nvn_gpu_layout_ent *e = &sh->ents[i];
            if (e->cls == NVN_GPU_RES_VA)
                return fail(err, NVN_GPU_E_UNSUPPORTED,
                            "VA（设备地址）类别：契约未定非 bufferDeviceAddress 的消费方式，不报绿（请契约属主定夺）");
            if (vk_desc_type(e->cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM)
                return fail(err, NVN_GPU_E_UNSUPPORTED, "资源类别 %u：本段未实现", e->cls);
            uint32_t j = 0;
            for (; j < nm; j++)
                if (m[j].set == e->set && m[j].binding == e->binding) break;
            uint32_t stagebit = 1u << st;
            if (j == nm) {
                if (nm == 64) return fail(err, NVN_GPU_E_UNSUPPORTED, "描述符项超过 64");
                m[nm++] = (struct merged){e->set, e->binding, e->cls, e->count ? e->count : 1, stagebit};
            } else {
                if (m[j].cls != e->cls || m[j].count != (e->count ? e->count : 1))
                    return fail(err, NVN_GPU_E_LAYOUT, "set %u binding %u 的阶段布局冲突", e->set, e->binding);
                m[j].stages |= stagebit;
            }
            if (e->set > maxset) maxset = e->set;
        }
    }
    if (maxset >= 4) return fail(err, NVN_GPU_E_LAYOUT, "set %u 超过四 set 上限", maxset);

    struct nvn_gpu_pipe_s *p = calloc(1, sizeof *p);
    if (!p) return fail(err, NVN_GPU_E_OOM, "管线结构分配失败");
    p->nset = nm ? maxset + 1 : 0;
    for (uint32_t set = 0; set < p->nset; set++) {
        VkDescriptorSetLayoutBinding b[32];
        uint32_t nb = 0;
        for (uint32_t i = 0; i < nm; i++) {
            if (m[i].set != set) continue;
            if (nb == 32) { free(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "单 set 描述符项超过 32"); }
            memset(&b[nb], 0, sizeof b[nb]);
            b[nb].binding = m[i].binding;
            b[nb].descriptorType = vk_desc_type(m[i].cls);
            b[nb].descriptorCount = m[i].count;
            b[nb].stageFlags = 0;
            for (int st = 0; st < 5; st++)
                if (m[i].stages & (1u << st)) b[nb].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);
            nb++;
        }
        VkDescriptorSetLayoutCreateInfo li;
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = nb;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(g.d, &li, NULL, &p->dsl[set]) != VK_SUCCESS) {
            free(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateDescriptorSetLayout 失败（set %u）", set);
        }
    }
    VkPushConstantRange pr[6];
    uint32_t npr = 0;
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh || !sh->push_bytes) continue;
        if ((sh->push_off | sh->push_bytes) & 3) {
            free(p);
            return fail(err, NVN_GPU_E_LAYOUT, "阶段 %d push 范围 [%u,+%u) 未 4 字节对齐", st, sh->push_off, sh->push_bytes);
        }
        if (sh->push_off > g.caps.max_push_bytes || sh->push_bytes > g.caps.max_push_bytes - sh->push_off) {
            free(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "阶段 %d push 范围 [%u,+%u) 超出设备上限 %u",
                        st, sh->push_off, sh->push_bytes, g.caps.max_push_bytes);
        }
        uint32_t k;
        for (k = 0; k < npr; k++)
            if (pr[k].offset == sh->push_off && pr[k].size == sh->push_bytes) {
                pr[k].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);
                break;
            }
        if (k == npr)
            pr[npr++] = (VkPushConstantRange){vk_shader_stage((nvn_gpu_stage)st), sh->push_off, sh->push_bytes};
    }
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = p->nset;
    pli.pSetLayouts = p->nset ? p->dsl : NULL;
    pli.pushConstantRangeCount = npr;
    pli.pPushConstantRanges = npr ? pr : NULL;
    if (vkCreatePipelineLayout(g.d, &pli, NULL, &p->layout) != VK_SUCCESS) {
        free(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreatePipelineLayout 失败");
    }
    p->pass = make_pipeline_pass(d);
    if (!p->pass) {
        vkDestroyPipelineLayout(g.d, p->layout, NULL);
        free(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "管线 render pass 创建失败（附件格式 / 采样数）");
    }

    VkPipelineShaderStageCreateInfo stages[5];
    uint32_t ns = 0;
    for (int st = 0; st < 5; st++) {
        if (!d->st[st].sh) continue;
        memset(&stages[ns], 0, sizeof stages[ns]);
        stages[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[ns].stage = vk_shader_stage((nvn_gpu_stage)st);
        stages[ns].module = d->st[st].sh->mod;
        stages[ns].pName = d->st[st].sh->entry;
        ns++;
    }

    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t nvb = 0, nva = 0;
    for (int i = 0; i < 16; i++) {
        if (!d->attr[i].fmt) continue;
        VkFormat f = nvn_vk_format(nvn_vattr_fmt_canon(d->attr[i].fmt));   // RGBX → 同宽 RGB（X 不参与）
        if (f == VK_FORMAT_UNDEFINED) {
            nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "顶点属性格式 0x%x 无映射", d->attr[i].fmt);
            destroy_pipe_objects(p);
            return bad;
        }
        // 必须 CPU 解码的格式（32 位 UI2F/I2F）：本后端没有解码合成 binding，按映射出的整数
        // 格式绑定会把整数位模式当 float 静默读错值——明确报红，不返回成功兜底。
        if (nvn_vattr_must_cpu_decode(d->attr[i].fmt)) {
            nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED,
                                  "顶点属性格式 0x%x 需要 CPU 顶点解码（本后端未实现）", d->attr[i].fmt);
            destroy_pipe_objects(p);
            return bad;
        }
        uint32_t stream = d->attr[i].stream;
        int found = 0;
        for (uint32_t k = 0; k < nvb; k++) if (vb[k].binding == stream) found = 1;
        if (!found) {
            vb[nvb].binding = stream;
            vb[nvb].stride = d->stream[stream].stride;
            vb[nvb].inputRate = d->stream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            nvb++;
        }
        va[nva].location = (uint32_t)i;
        va[nva].binding = stream;
        va[nva].format = f;
        va[nva].offset = d->attr[i].offset;
        nva++;
    }

    VkPipelineVertexInputStateCreateInfo vin;
    memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = nvb;
    vin.pVertexBindingDescriptions = vb;
    vin.vertexAttributeDescriptionCount = nva;
    vin.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = vk_topology(d->prim);
    // 重启只在条带 / 扇上有意义；列表图元打开需要 primitiveTopologyListRestart，这里忽略并注释
    int restart_topology = ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                           ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                           ia.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    ia.primitiveRestartEnable = (d->prim_restart && restart_topology) ? VK_TRUE : VK_FALSE;
    if (ia.topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "图元 %u：本段只实现三角形", d->prim);
        destroy_pipe_objects(p);
        return bad;
    }

    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d->poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : d->poly.cull == 2 ? VK_CULL_MODE_BACK_BIT
                 : d->poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    rs.frontFace = d->poly.front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.rasterizerDiscardEnable = d->rast_discard ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(d->samples > 1 ? d->samples : 1);

    VkPipelineDepthStencilStateCreateInfo ds;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (d->dfmt) {
        ds.depthTestEnable = d->ds.dt ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = d->ds.dw ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = vk_depth_func(d->ds.df);
        ds.depthBoundsTestEnable = VK_FALSE;
        ds.stencilTestEnable = d->ds.st ? VK_TRUE : VK_FALSE;
    }

    if (d->color.logic_op != 0x3) {   // NVN 缺省 COPY=0x3；其余逻辑操作未实现
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "逻辑操作 %u：本段未实现（只支持缺省 COPY）", d->color.logic_op);
        destroy_pipe_objects(p);
        return bad;
    }
    VkPipelineColorBlendAttachmentState cb[8];
    uint32_t ncb = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (!d->cfmt[i]) break;
        memset(&cb[ncb], 0, sizeof cb[ncb]);
        if ((d->color.blend_en >> i) & 1u) {
            const blend_st *b = &d->blend[i];
            if (b->adv_mode) {
                nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "高级混合（adv_mode）：本段未实现（target %u）", i);
                destroy_pipe_objects(p);
                return bad;
            }
            uint32_t fs[4] = {b->src_rgb, b->dst_rgb, b->src_a, b->dst_a};
            for (int k = 0; k < 4; k++)
                if (fs[k] >= 0x10 && fs[k] <= 0x13 && !g.dev->core_features.dualSrcBlend) {
                    nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "双源混合因子：设备未启用 dualSrcBlend（target %u）", i);
                    destroy_pipe_objects(p);
                    return bad;
                }
            VkBlendFactor sf = vk_blend_factor(b->src_rgb), df = vk_blend_factor(b->dst_rgb);
            VkBlendFactor sa = vk_blend_factor(b->src_a), da = vk_blend_factor(b->dst_a);
            VkBlendOp er = vk_blend_op(b->eq_rgb), ea = vk_blend_op(b->eq_a);
            if (sf == VK_BLEND_FACTOR_MAX_ENUM || df == VK_BLEND_FACTOR_MAX_ENUM || sa == VK_BLEND_FACTOR_MAX_ENUM ||
                da == VK_BLEND_FACTOR_MAX_ENUM || er == VK_BLEND_OP_MAX_ENUM || ea == VK_BLEND_OP_MAX_ENUM) {
                nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "混合因子 / 运算：target %u 未实现", i);
                destroy_pipe_objects(p);
                return bad;
            }
            cb[ncb].blendEnable = VK_TRUE;
            cb[ncb].srcColorBlendFactor = sf; cb[ncb].dstColorBlendFactor = df; cb[ncb].colorBlendOp = er;
            cb[ncb].srcAlphaBlendFactor = sa; cb[ncb].dstAlphaBlendFactor = da; cb[ncb].alphaBlendOp = ea;
        }
        uint32_t mask = d->chmask.mask ? (d->chmask.mask >> (i * 4)) & 0xF : 0xF;
        cb[ncb].colorWriteMask = ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                 ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
        ncb++;
    }
    VkPipelineColorBlendStateCreateInfo cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = ncb;
    cbs.pAttachments = cb;

    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyns;
    memset(&dyns, 0, sizeof dyns);
    dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 3;
    dyns.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp;
    memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = ns;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dyns;
    gp.layout = p->layout;
    gp.renderPass = p->pass;
    gp.subpass = 0;
    if (vkCreateGraphicsPipelines(g.d, VK_NULL_HANDLE, 1, &gp, NULL, &p->pipe) != VK_SUCCESS) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_COMPILE, "vkCreateGraphicsPipelines 失败");
        destroy_pipe_objects(p);
        return bad;
    }
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh) continue;
        p->stage_nent[st] = sh->nent;
        p->stage_push_off[st] = sh->push_off;
        p->stage_push_bytes[st] = sh->push_bytes;
        if (sh->nent) {
            p->stage_ents[st] = calloc(sh->nent, sizeof *p->stage_ents[st]);
            if (!p->stage_ents[st]) {
                destroy_pipe_objects(p);
                return fail(err, NVN_GPU_E_OOM, "图形管线阶段布局分配失败");
            }
            memcpy(p->stage_ents[st], sh->ents, (size_t)sh->nent * sizeof *p->stage_ents[st]);
            if (sh->tfmt) {
                p->stage_tfmt[st] = calloc(sh->nent, sizeof *p->stage_tfmt[st]);
                if (!p->stage_tfmt[st]) {
                    destroy_pipe_objects(p);
                    return fail(err, NVN_GPU_E_OOM, "图形管线 texel 格式表分配失败");
                }
                memcpy(p->stage_tfmt[st], sh->tfmt, (size_t)sh->nent * sizeof *p->stage_tfmt[st]);
            }
        }
    }
    pthread_mutex_lock(&g_mu);
    for (int st = 0; st < 5; st++)
        if (d->st[st].sh) d->st[st].sh->refs++; // 管线持有
    pthread_mutex_unlock(&g_mu);
    p->key = *d;
    pthread_mutex_lock(&g_mu);
    p->all_next = g.pipes;
    g.pipes = p;
    pthread_mutex_unlock(&g_mu);
    *out = p;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    (void)mode;
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!cs || !cs->sh) return fail(err, NVN_GPU_E_ARG, "计算阶段为空");
    struct nvn_gpu_shader_s *sh = cs->sh;
    if (sh->stage != NVN_GPU_SH_CS) return fail(err, NVN_GPU_E_ARG, "阶段不是 CS");
    if (sh->missing_storage_format_feature)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "设备未启用无格式存储图像所需特性");
    struct nvn_gpu_pipe_s *p = calloc(1, sizeof *p);
    if (!p) return fail(err, NVN_GPU_E_OOM, "计算管线结构分配失败");
    uint32_t maxset = 0;
    for (uint32_t i = 0; i < sh->nent; i++) {
        if (sh->ents[i].cls == NVN_GPU_RES_VA) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED,
                        "VA（设备地址）类别：契约未定非 bufferDeviceAddress 的消费方式，不报绿（请契约属主定夺）");
        }
        if (vk_desc_type(sh->ents[i].cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "资源类别 %u：本段未实现", sh->ents[i].cls);
        }
        if (sh->ents[i].set > maxset) maxset = sh->ents[i].set;
    }
    if (sh->nent && maxset >= 4) { destroy_pipe_objects(p); return fail(err, NVN_GPU_E_LAYOUT, "set %u 超过四 set 上限", maxset); }
    p->nset = sh->nent ? maxset + 1 : 0;
    for (uint32_t set = 0; set < p->nset; set++) {
        VkDescriptorSetLayoutBinding b[32];
        uint32_t nb = 0;
        for (uint32_t i = 0; i < sh->nent; i++) {
            if (sh->ents[i].set != set) continue;
            if (nb == 32) { destroy_pipe_objects(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "单 set 描述符项超过 32"); }
            memset(&b[nb], 0, sizeof b[nb]);
            b[nb].binding = sh->ents[i].binding;
            b[nb].descriptorType = vk_desc_type(sh->ents[i].cls);
            b[nb].descriptorCount = sh->ents[i].count ? sh->ents[i].count : 1;
            b[nb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            nb++;
        }
        VkDescriptorSetLayoutCreateInfo li;
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = nb;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(g.d, &li, NULL, &p->dsl[set]) != VK_SUCCESS) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateDescriptorSetLayout 失败（set %u）", set);
        }
    }
    VkPushConstantRange cpr;
    uint32_t ncpr = 0;
    if (sh->push_bytes) {
        if ((sh->push_off | sh->push_bytes) & 3) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_LAYOUT, "CS push 范围 [%u,+%u) 未 4 字节对齐", sh->push_off, sh->push_bytes);
        }
        if (sh->push_off > g.caps.max_push_bytes || sh->push_bytes > g.caps.max_push_bytes - sh->push_off) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "CS push 范围 [%u,+%u) 超出设备上限 %u",
                        sh->push_off, sh->push_bytes, g.caps.max_push_bytes);
        }
        cpr = (VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT, sh->push_off, sh->push_bytes};
        ncpr = 1;
    }
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = p->nset;
    pli.pSetLayouts = p->nset ? p->dsl : NULL;
    pli.pushConstantRangeCount = ncpr;
    pli.pPushConstantRanges = ncpr ? &cpr : NULL;
    if (vkCreatePipelineLayout(g.d, &pli, NULL, &p->layout) != VK_SUCCESS) {
        destroy_pipe_objects(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreatePipelineLayout 失败");
    }
    VkComputePipelineCreateInfo cp;
    memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = sh->mod;
    cp.stage.pName = sh->entry;
    cp.layout = p->layout;
    if (vkCreateComputePipelines(g.d, VK_NULL_HANDLE, 1, &cp, NULL, &p->pipe) != VK_SUCCESS) {
        destroy_pipe_objects(p);
        return fail(err, NVN_GPU_E_COMPILE, "vkCreateComputePipelines 失败");
    }
    p->stage_nent[NVN_GPU_SH_CS] = sh->nent;
    p->stage_push_off[NVN_GPU_SH_CS] = sh->push_off;
    p->stage_push_bytes[NVN_GPU_SH_CS] = sh->push_bytes;
    if (sh->nent) {
        p->stage_ents[NVN_GPU_SH_CS] = calloc(sh->nent, sizeof *p->stage_ents[NVN_GPU_SH_CS]);
        if (!p->stage_ents[NVN_GPU_SH_CS]) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_OOM, "计算管线阶段布局分配失败");
        }
        memcpy(p->stage_ents[NVN_GPU_SH_CS], sh->ents, (size_t)sh->nent * sizeof *sh->ents);
        if (sh->tfmt) {
            p->stage_tfmt[NVN_GPU_SH_CS] = calloc(sh->nent, sizeof *p->stage_tfmt[NVN_GPU_SH_CS]);
            if (!p->stage_tfmt[NVN_GPU_SH_CS]) {
                destroy_pipe_objects(p);
                return fail(err, NVN_GPU_E_OOM, "计算管线 texel 格式表分配失败");
            }
            memcpy(p->stage_tfmt[NVN_GPU_SH_CS], sh->tfmt, (size_t)sh->nent * sizeof *sh->tfmt);
        }
    }
    pthread_mutex_lock(&g_mu);
    sh->refs++;
    p->all_next = g.pipes;
    g.pipes = p;
    pthread_mutex_unlock(&g_mu);
    *out = p;
    return rc(NVN_GPU_OK);
}

static VkRenderPass make_pass(const nvn_gpu_pass_desc *d, VkImageView fb_views[9], VkAttachmentDescription att[9],
                              VkAttachmentReference col[8], VkAttachmentReference *dref, uint32_t *natt_out, int *has_depth) {
    VkAttachmentReference stencil_ref;
    uint32_t natt = 0, ncol = 0;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        struct nvn_gpu_view_s *v = d->color[i].view;
        if (!v) return VK_NULL_HANDLE;
        struct nvn_gpu_tex_s *t = v->tex;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = t->format;
        att[natt].samples = (VkSampleCountFlagBits)t->samples;
        switch (d->color[i].load) {
        case NVN_GPU_LOAD_CLEAR: att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[natt].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; break;
        case NVN_GPU_LOAD_DONTCARE: att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[natt].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; break;
        default: att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[natt].initialLayout = t->layout; break;
        }
        att[natt].storeOp = d->color[i].store == NVN_GPU_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col[ncol].attachment = natt;
        col[ncol].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fb_views[natt] = v->view;
        ncol++;
        natt++;
    }
    *has_depth = 0;
    if (d->depth.view) {
        struct nvn_gpu_view_s *v = d->depth.view;
        struct nvn_gpu_tex_s *t = v->tex;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = t->format;
        att[natt].samples = (VkSampleCountFlagBits)t->samples;
        if (d->depth.load == NVN_GPU_LOAD_LOAD) { att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[natt].initialLayout = t->layout; }
        else if (d->depth.load == NVN_GPU_LOAD_DONTCARE) { att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[natt].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; }
        else { att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[natt].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; }
        att[natt].storeOp = d->depth.store == NVN_GPU_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref->attachment = natt;
        dref->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        fb_views[natt] = v->view;
        *has_depth = 1;
        natt++;
    }
    (void)stencil_ref;
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = ncol;
    sp.pColorAttachments = ncol ? col : NULL;
    sp.pDepthStencilAttachment = *has_depth ? dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rp;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = natt;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    *natt_out = natt;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(g.d, &rp, NULL, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

static void image_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout to);

void nvn_gpu_pass_begin(nvn_gpu_cmd ch, const nvn_gpu_pass_desc *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "pass_begin 时已在通道里"); return; }
    if (!d || !d->ncolor || !d->w || !d->h) { set_sticky(c, NVN_GPU_E_ARG, "pass_begin 参数非法"); return; }
    VkImageView fb_views[9];
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    uint32_t natt = 0;
    int has_depth = 0;
    VkRenderPass pass = make_pass(d, fb_views, att, col, &dref, &natt, &has_depth);
    if (!pass) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "渲染通道创建失败（附件格式 / 采样数）"); return; }
    VkFramebufferCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = pass;
    fi.attachmentCount = natt;
    fi.pAttachments = fb_views;
    fi.width = d->w;
    fi.height = d->h;
    fi.layers = d->layers ? d->layers : 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(g.d, &fi, NULL, &fb) != VK_SUCCESS) {
        vkDestroyRenderPass(g.d, pass, NULL);
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "vkCreateFramebuffer 失败");
        return;
    }
    VkClearValue clear[9];
    memset(clear, 0, sizeof clear);
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        clear[i].color.float32[0] = d->color[i].clear.f[0];
        clear[i].color.float32[1] = d->color[i].clear.f[1];
        clear[i].color.float32[2] = d->color[i].clear.f[2];
        clear[i].color.float32[3] = d->color[i].clear.f[3];
    }
    if (has_depth) { clear[natt - 1].depthStencil.depth = d->clear_depth; clear[natt - 1].depthStencil.stencil = d->clear_stencil; }
    VkRenderPassBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = pass;
    bi.framebuffer = fb;
    bi.renderArea.extent = (VkExtent2D){d->w, d->h};
    bi.clearValueCount = natt;
    bi.pClearValues = clear;
    vkCmdBeginRenderPass(c->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c->in_pass = 1;
    c->pass_w = d->w;
    c->pass_h = d->h;
    c->pass_ntex = 0;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        if (!d->color[i].view) continue;
        cmd_ref(c, d->color[i].view, REF_VIEW);   // 附件 view（连带贴图）保活到本段完成
        struct nvn_gpu_tex_s *t = d->color[i].view->tex;
        t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (c->pass_ntex < 8) c->pass_tex[c->pass_ntex++] = t;
    }
    if (d->depth.view) {
        cmd_ref(c, d->depth.view, REF_VIEW);
        struct nvn_gpu_tex_s *t = d->depth.view->tex;
        t->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (c->pass_ntex < 8) c->pass_tex[c->pass_ntex++] = t;
    }
    if (c->npasses == c->passcap) {
        uint32_t cap = c->passcap ? c->passcap * 2 : 8;
        struct pass_rec *np = realloc(c->passes, cap * sizeof *np);
        if (!np) { vkDestroyFramebuffer(g.d, fb, NULL); vkDestroyRenderPass(g.d, pass, NULL); set_sticky(c, NVN_GPU_E_OOM, "通道列表分配失败"); return; }
        c->passes = np;
        c->passcap = cap;
    }
    c->passes[c->npasses++] = (struct pass_rec){pass, fb};
}

void nvn_gpu_pass_end(nvn_gpu_cmd ch) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "pass_end 时不在通道里"); return; }
    vkCmdEndRenderPass(c->cb);
    c->in_pass = 0;
    // 通道结束：附件从 COLOR/DEPTH 显式转回 GENERAL，写对后续采样 / 拷贝 / 再当附件可见
    for (uint32_t i = 0; i < 8 && c->pass_tex[i]; i++) image_barrier(c, c->pass_tex[i], VK_IMAGE_LAYOUT_GENERAL);
    // render pass / framebuffer 被已录制的命令引用，等 cmd 真正完成后再销毁（finish_cmd）。
}

static int ensure_push_capacity(struct nvn_gpu_cmd_s *c, int st, uint32_t required) {
    if (required <= c->push_capacity[st]) return 1;
    if (required > g.caps.max_push_bytes) { set_sticky(c, NVN_GPU_E_ARG, "push 暂存超出设备上限"); return 0; }
    uint8_t *data = realloc(c->push_data[st], required);
    if (!data) { set_sticky(c, NVN_GPU_E_OOM, "push 暂存分配失败"); return 0; }
    memset(data + c->push_capacity[st], 0, required - c->push_capacity[st]);
    c->push_data[st] = data;
    c->push_capacity[st] = required;
    return 1;
}

void nvn_gpu_set_pipe(nvn_gpu_cmd ch, nvn_gpu_pipe ph) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_pipe_s *p = ph;
    if (!p) { set_sticky(c, NVN_GPU_E_ARG, "set_pipe 传了空管线"); return; }
    if (!c->in_pass && p->pass) { set_sticky(c, NVN_GPU_E_ORDER, "图形管线在通道外 set_pipe"); return; }
    // Already allocated stage data survives pipe changes. Expand before the next push/dispatch.
    for (int st = 0; st <= NVN_GPU_SH_CS; st++) {
        uint64_t end = (uint64_t)p->stage_push_off[st] + p->stage_push_bytes[st];
        if (c->push_data[st] && end > UINT32_MAX) { set_sticky(c, NVN_GPU_E_ARG, "push 范围溢出"); return; }
        if (c->push_data[st] && !ensure_push_capacity(c, st, (uint32_t)end)) return;
    }
    c->pipe = ph;
}

void nvn_gpu_set_dyn(nvn_gpu_cmd ch, const nvn_gpu_dyn *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "set_dyn 在通道外"); return; }
    if (!d) { set_sticky(c, NVN_GPU_E_ARG, "set_dyn 为空"); return; }
    c->dyn = *d;
    c->has_dyn = 1;
}

void nvn_gpu_set_vbuf(nvn_gpu_cmd ch, uint32_t stream, nvn_gpu_buf bh, uint64_t off) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (stream >= 16) { set_sticky(c, NVN_GPU_E_ARG, "顶点流 %u 越界", stream); return; }
    c->vbuf[stream].buf = bh;
    c->vbuf[stream].off = off;
    if (bh) cmd_ref(c, bh, REF_BUF);
}

// 先按地址查活视图表，再保活，绝不把旧 u.b 的 buffer 指针当作 view 解引用。
static int cmd_ref_texel_view(struct nvn_gpu_cmd_s *c, nvn_gpu_view handle, uint32_t cls) {
    VkFormatFeatureFlags need = cls == NVN_GPU_RES_TBUF ? VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT
                                                         : VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
    uint32_t cap = cls == NVN_GPU_RES_TBUF ? NVN_GPU_FMT_SAMPLE : NVN_GPU_FMT_STORAGE;
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_view_s *v = g.views;
    while (v && v != handle) v = v->all_next;
    if (!v || v->released || !v->buffer_view || !(v->buffer_features & need) || !(v->buffer_caps & cap)) {
        pthread_mutex_unlock(&g_mu);
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "纹素缓冲绑定不是支持该用途的有效缓冲视图");
        return 0;
    }
    v->refs++;
    pthread_mutex_unlock(&g_mu);
    if (c->nrefs == c->refcap) {
        uint32_t cap = c->refcap ? c->refcap * 2 : 16;
        struct vk_ref *p = realloc(c->refs, (size_t)cap * sizeof *p);
        if (!p) {
            pthread_mutex_lock(&g_mu); unref_locked(&g, v, REF_VIEW); pthread_mutex_unlock(&g_mu);
            set_sticky(c, NVN_GPU_E_OOM, "引用表分配失败");
            return 0;
        }
        c->refs = p; c->refcap = cap;
    }
    c->refs[c->nrefs++] = (struct vk_ref){v, REF_VIEW};
    return 1;
}

void nvn_gpu_bind(nvn_gpu_cmd ch, nvn_gpu_stage st, const nvn_gpu_binding *b, uint32_t n) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass && (!c->pipe || c->pipe->pass)) { set_sticky(c, NVN_GPU_E_ORDER, "bind 在通道外（非计算管线）"); return; }
    if (!c->pipe) { set_sticky(c, NVN_GPU_E_ORDER, "bind 前没有 set_pipe"); return; }
    if (!b && n) { set_sticky(c, NVN_GPU_E_ARG, "bind 表为空"); return; }
    uint32_t count = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        if (c->pipe->stage_nent[st] && b[i].ent >= c->pipe->stage_nent[st]) {
            set_sticky(c, NVN_GPU_E_LAYOUT, "绑定项 %u 的 ent %u 越界", i, b[i].ent);
            return;
        }
        const nvn_gpu_layout_ent *e = c->pipe->stage_nent[st] ? &c->pipe->stage_ents[st][b[i].ent] : NULL;
        if (e && e->cls != b[i].cls) { set_sticky(c, NVN_GPU_E_LAYOUT, "绑定项 %u 类别 %u != 布局 %u", i, b[i].cls, e->cls); return; }
        c->binds[st][count].b = b[i];
        if (b[i].cls == NVN_GPU_RES_TEX_SAMP) {
            if (!b[i].u.ts.view || !b[i].u.ts.samp) {
                set_sticky(c, NVN_GPU_E_LAYOUT, "TEX_SAMP 绑定缺 view 或 samp");
                return;
            }
            cmd_ref(c, b[i].u.ts.view, REF_VIEW);
            cmd_ref(c, b[i].u.ts.samp, REF_SAMP);
        } else if (b[i].cls == NVN_GPU_RES_TEX && b[i].u.view) cmd_ref(c, b[i].u.view, REF_VIEW);
        else if (b[i].cls == NVN_GPU_RES_SAMP && b[i].u.samp) cmd_ref(c, b[i].u.samp, REF_SAMP);
        else if (b[i].cls == NVN_GPU_RES_IMAGE && b[i].u.view) cmd_ref(c, b[i].u.view, REF_VIEW);
        else if (b[i].cls == NVN_GPU_RES_TBUF || b[i].cls == NVN_GPU_RES_IMAGE_TBUF) {
            if (!cmd_ref_texel_view(c, b[i].u.view, b[i].cls)) return;
        }
        else if ((b[i].cls == NVN_GPU_RES_UBO || b[i].cls == NVN_GPU_RES_SSBO) && b[i].u.b.buf) cmd_ref(c, b[i].u.b.buf, REF_BUF);
        count++;
    }
    c->nbinds[st] = count;
}

// Test-only observation of the real allocation; never reads or writes push bytes.
uint32_t nvn_gpu_vk_test_push_capacity(nvn_gpu_cmd ch, nvn_gpu_stage st) {
    struct nvn_gpu_cmd_s *c = ch;
    return c && (uint32_t)st <= (uint32_t)NVN_GPU_SH_CS ? c->push_capacity[st] : 0;
}

void nvn_gpu_push(nvn_gpu_cmd ch, nvn_gpu_stage st, uint32_t off, const void *data, uint32_t bytes) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if ((uint32_t)st > (uint32_t)NVN_GPU_SH_CS) { set_sticky(c, NVN_GPU_E_ARG, "push 阶段 %d 非法", (int)st); return; }
    if (!c->pipe) { set_sticky(c, NVN_GPU_E_ORDER, "push 前没有 set_pipe"); return; }
    if (!c->in_pass && c->pipe->pass) { set_sticky(c, NVN_GPU_E_ORDER, "push 在通道外（非计算管线）"); return; }
    if (!data && bytes) { set_sticky(c, NVN_GPU_E_ARG, "push data 为空"); return; }
    uint32_t po = c->pipe->stage_push_off[st], pb = c->pipe->stage_push_bytes[st];
    if (!pb) { set_sticky(c, NVN_GPU_E_ARG, "阶段 %d 未声明 push 范围", (int)st); return; }
    if (off < po || (uint64_t)(off - po) + bytes > pb) {
        set_sticky(c, NVN_GPU_E_ARG, "push [%u,+%u) 越出阶段 %d 声明范围 [%u,+%u)", off, bytes, (int)st, po, pb);
        return;
    }
    if (!bytes) return;
    uint64_t end = (uint64_t)po + pb;
    if (end > UINT32_MAX) { set_sticky(c, NVN_GPU_E_ARG, "push 范围溢出"); return; }
    if (!ensure_push_capacity(c, st, (uint32_t)end)) return;
    memcpy(c->push_data[st] + off, data, bytes);
    c->push_have[st] = 1;
}

// ---------------------------------------------------------------- 描述符池 / 绑定写入 / texel 视图

// 每条 cmd 一个池：64 set（缺扩容是已知静默错 C66 的复现对象，本段不修）。
static int ensure_dpool(struct nvn_gpu_cmd_s *c) {
    if (c->dpool) return 1;
    VkDescriptorPoolSize ps[8] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64}, {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 64}};
    VkDescriptorPoolCreateInfo dpi;
    memset(&dpi, 0, sizeof dpi);
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 64;
    dpi.poolSizeCount = 8;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(g.d, &dpi, NULL, &c->dpool) != VK_SUCCESS) {
        set_sticky(c, NVN_GPU_E_OOM, "描述符池创建失败");
        return 0;
    }
    return 1;
}

// 把某阶段本笔绑定写进 wr/ii/bi；返回写入条数，出错置粘滞返回 -1。
// 缺成员的新类别（IMAGE / TBUF / IMAGE_TBUF）报 E_LAYOUT；老类别保持 NULL 句柄空资源写法不变。
static int write_binds(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int st,
                       const VkDescriptorSet *sets, VkWriteDescriptorSet *wr,
                       VkDescriptorImageInfo *ii, VkDescriptorBufferInfo *bi) {
    int nw = 0;
    for (uint32_t i = 0; i < c->nbinds[st]; i++) {
        const nvn_gpu_binding *b = &c->binds[st][i].b;
        if (!p->stage_nent[st]) { set_sticky(c, NVN_GPU_E_LAYOUT, "阶段 %d 没有布局项却给了绑定", st); return -1; }
        const nvn_gpu_layout_ent *e = &p->stage_ents[st][b->ent];
        VkWriteDescriptorSet *w = &wr[nw];
        memset(w, 0, sizeof *w);
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w->dstSet = sets[e->set];
        w->dstBinding = e->binding;
        w->dstArrayElement = b->elem;
        w->descriptorCount = 1;
        w->descriptorType = vk_desc_type(e->cls);
        VkDescriptorImageInfo *im = &ii[nw];
        VkDescriptorBufferInfo *bf = &bi[nw];
        switch (e->cls) {
        case NVN_GPU_RES_TEX:
            memset(im, 0, sizeof *im);
            im->imageView = b->u.view ? b->u.view->view : VK_NULL_HANDLE;
            im->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_SAMP:
            memset(im, 0, sizeof *im);
            im->sampler = b->u.samp ? b->u.samp->sampler : VK_NULL_HANDLE;
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_TEX_SAMP:
            if (!b->u.ts.view || !b->u.ts.samp) { set_sticky(c, NVN_GPU_E_LAYOUT, "TEX_SAMP 绑定缺 view 或 samp"); return -1; }
            memset(im, 0, sizeof *im);
            im->imageView = b->u.ts.view->view;
            im->sampler = b->u.ts.samp->sampler;
            im->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_IMAGE:
            if (!b->u.view) { set_sticky(c, NVN_GPU_E_LAYOUT, "IMAGE 绑定缺 view"); return -1; }
            memset(im, 0, sizeof *im);
            im->imageView = b->u.view->view;
            im->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_UBO:
        case NVN_GPU_RES_SSBO:
            memset(bf, 0, sizeof *bf);
            bf->buffer = b->u.b.buf ? b->u.b.buf->buffer : VK_NULL_HANDLE;
            bf->offset = b->u.b.off;
            bf->range = b->u.b.len;
            w->pBufferInfo = bf;
            break;
        case NVN_GPU_RES_TBUF:
        case NVN_GPU_RES_IMAGE_TBUF: {
            struct nvn_gpu_view_s *v = b->u.view; // 已在 bind 时核验并保活
            VkFormat f = p->stage_tfmt[st] ? p->stage_tfmt[st][b->ent] : VK_FORMAT_UNDEFINED;
            if (f != VK_FORMAT_UNDEFINED && f != v->buffer_format) {
                set_sticky(c, NVN_GPU_E_LAYOUT, "texel 绑定视图格式与着色器声明不符");
                return -1;
            }
            if (e->cls == NVN_GPU_RES_IMAGE_TBUF && f == VK_FORMAT_UNDEFINED &&
                !texel_unknown_storage_supported(v->buffer_format)) {
                set_sticky(c, NVN_GPU_E_UNSUPPORTED, "纹素视图格式不支持无格式存储访问");
                return -1;
            }
            w->pTexelBufferView = &v->buffer_view;
            break;
        }
        default:
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "绑定类别 %u：本段未实现", e->cls);
            return -1;
        }
        nw++;
    }
    return nw;
}

void nvn_gpu_draw(nvn_gpu_cmd ch, const nvn_gpu_draw_args *a) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass || !c->pipe || !a) { set_sticky(c, NVN_GPU_E_ORDER, "draw 缺通道 / 管线 / 参数"); return; }
    if (a->cnt) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "带计数缓冲的间接多绘（cnt）：本段未实现"); return; }
    if (a->prim != 0x0 && a->prim != 0x1 && a->prim != 0x3 && a->prim != 0x4 && a->prim != 0x5 && a->prim != 0x6) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "图元 %u 未实现（线环 / quads 等）", a->prim); return;
    }
    struct nvn_gpu_pipe_s *p = c->pipe;
    if (a->prim != p->key.prim) {
        set_sticky(c, NVN_GPU_E_ARG, "draw prim %u 与管线拓扑 %u 不一致", a->prim, p->key.prim);
        return;
    }
    if (p->nset) {
        if (!ensure_dpool(c)) return;
        VkDescriptorSetAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = c->dpool;
        ai.descriptorSetCount = p->nset;
        ai.pSetLayouts = p->dsl;
        VkDescriptorSet sets[4] = {0};
        if (vkAllocateDescriptorSets(g.d, &ai, sets) != VK_SUCCESS) {
            set_sticky(c, NVN_GPU_E_OOM, "vkAllocateDescriptorSets 失败");
            return;
        }
        VkWriteDescriptorSet wr[6 * 64];
        VkDescriptorImageInfo ii[6 * 64];
        VkDescriptorBufferInfo bi[6 * 64];
        uint32_t nw = 0;
        for (int bst = 0; bst < 5; bst++) {
            int n = write_binds(c, p, bst, sets, wr + nw, ii + nw, bi + nw);
            if (n < 0) return;
            nw += (uint32_t)n;
        }
        if (nw) vkUpdateDescriptorSets(g.d, nw, wr, 0, NULL);
        vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, p->nset, sets, 0, NULL);
    }
    for (int st = 0; st < 5; st++)
        if (c->push_have[st] && p->stage_push_bytes[st])
            vkCmdPushConstants(c->cb, p->layout, vk_shader_stage((nvn_gpu_stage)st),
                               p->stage_push_off[st], p->stage_push_bytes[st], c->push_data[st] + p->stage_push_off[st]);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    VkViewport vp;
    if (c->has_dyn && c->dyn.nvp) {
        vp = (VkViewport){c->dyn.vp[0][0], c->dyn.vp[0][1], c->dyn.vp[0][2], c->dyn.vp[0][3], c->dyn.vp[0][4], c->dyn.vp[0][5]};
    } else {
        vp = (VkViewport){0, 0, (float)c->pass_w, (float)c->pass_h, 0.0f, 1.0f};
    }
    VkRect2D sc = {{0, 0}, {c->pass_w, c->pass_h}};
    if (c->has_dyn && c->dyn.has_sc)
        sc = (VkRect2D){{c->dyn.sc[0], c->dyn.sc[1]}, {(uint32_t)c->dyn.sc[2], (uint32_t)c->dyn.sc[3]}};
    vkCmdSetViewport(c->cb, 0, 1, &vp);
    vkCmdSetScissor(c->cb, 0, 1, &sc);
    const float bc[4] = {c->dyn.blend_color[0], c->dyn.blend_color[1], c->dyn.blend_color[2], c->dyn.blend_color[3]};
    vkCmdSetBlendConstants(c->cb, bc);
    for (uint32_t s = 0; s < 16; s++) {
        if (!c->vbuf[s].buf) continue;
        VkBuffer vb = c->vbuf[s].buf->buffer;
        VkDeviceSize off = c->vbuf[s].off;
        vkCmdBindVertexBuffers(c->cb, s, 1, &vb, &off);
    }
    uint32_t inst = a->instances ? a->instances : 1;
    uint32_t draws = a->max_draws ? a->max_draws : 1;
    if (a->ibuf) {
        if (a->itype > 2) { set_sticky(c, NVN_GPU_E_ARG, "索引类型 %u 非法", a->itype); return; }
        VkIndexType it = a->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : a->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        VkBuffer ib = a->ibuf->buffer;
        VkDeviceSize ioff = a->ioff;
        uint32_t first = a->first;
        int force_expand = getenv("S2A_VK_FORCE_INDEX_U8_EXPAND") != NULL;
        if (a->itype == 0 && (!g.dev->index_type_uint8_enabled || force_expand)) {
            if (a->ind) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "u8 间接索引无 CPU 展开回退"); return; }
            if (!a->ibuf->mapped || !a->count) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "u8 索引展开需要可映射索引缓冲"); return; }
            if (ioff > a->ibuf->bytes || (uint64_t)first + a->count > (a->ibuf->bytes - ioff)) {
                set_sticky(c, NVN_GPU_E_ARG, "u8 索引展开范围越界（ioff=%llu first=%u count=%u bytes=%llu）",
                           (unsigned long long)ioff, first, a->count, (unsigned long long)a->ibuf->bytes);
                return;
            }
            const uint8_t *src = a->ibuf->mapped + ioff + first;
            uint64_t need = 2ull * a->count;
            nvn_gpu_slice sl;
            nvn_gpu_err ae;
            if (!rc_is(nvn_gpu_arena_alloc(ch, need, 4, &sl, &ae), NVN_GPU_OK)) { set_sticky(c, ae.rc.code, "u8 索引展开暂存：%s", ae.why); return; }
            for (uint32_t i = 0; i < a->count; i++) ((uint16_t *)sl.cpu)[i] = src[i];
            ib = sl.buf->buffer;
            ioff = sl.off;
            it = VK_INDEX_TYPE_UINT16;
            first = 0;
        }
        {
            uint64_t isz = a->itype == 0 ? 1 : a->itype == 1 ? 2 : 4;
            if (a->itype != 0 || (g.dev->index_type_uint8_enabled && !force_expand)) {
                if (ioff > a->ibuf->bytes || (uint64_t)first + a->count > (a->ibuf->bytes - ioff) / isz) {
                    set_sticky(c, NVN_GPU_E_ARG, "索引范围越界（ioff=%llu first=%u count=%u bytes=%llu）",
                               (unsigned long long)ioff, first, a->count, (unsigned long long)a->ibuf->bytes);
                    return;
                }
            }
        }
        cmd_ref(c, a->ibuf, REF_BUF);
        vkCmdBindIndexBuffer(c->cb, ib, ioff, it);
        if (a->ind) {
            cmd_ref(c, a->ind, REF_BUF);
            uint32_t stride = a->stride ? a->stride : (uint32_t)sizeof(VkDrawIndexedIndirectCommand);
            if (draws > 1) {
                for (uint32_t i = 0; i < draws; i++)
                    vkCmdDrawIndexedIndirect(c->cb, a->ind->buffer, a->ind_off + (uint64_t)i * stride, 1, stride);
            } else {
                vkCmdDrawIndexedIndirect(c->cb, a->ind->buffer, a->ind_off, 1, stride);
            }
        } else {
            vkCmdDrawIndexed(c->cb, a->count, inst, first, a->base_vertex, a->base_instance);
        }
    } else if (a->ind) {
        cmd_ref(c, a->ind, REF_BUF);
        uint32_t stride = a->stride ? a->stride : (uint32_t)sizeof(VkDrawIndirectCommand);
        if (draws > 1) {
            for (uint32_t i = 0; i < draws; i++)
                vkCmdDrawIndirect(c->cb, a->ind->buffer, a->ind_off + (uint64_t)i * stride, 1, stride);
        } else {
            vkCmdDrawIndirect(c->cb, a->ind->buffer, a->ind_off, 1, stride);
        }
    } else {
        vkCmdDraw(c->cb, a->count, inst, a->first, a->base_instance);
    }
}

// 本段未实现的区域：一律粘滞 UNSUPPORTED / 返回 UNSUPPORTED，不空实现。
#define VK_NOT_YET(c, name) set_sticky((c), NVN_GPU_E_UNSUPPORTED, name "：D8 第一段未实现（随后接通道 / 管线）")

void nvn_gpu_dispatch(nvn_gpu_cmd ch, nvn_gpu_pipe ph, uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf ind, uint64_t off) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!ind && (!x || !y || !z)) return;   // 0 组：空操作，不录、不置错（对齐 Ryujinx）
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "dispatch 在通道内"); return; }
    if (ph && c->pipe && ph != c->pipe) { set_sticky(c, NVN_GPU_E_ARG, "dispatch 管线与 bind 时的管线不一致"); return; }
    struct nvn_gpu_pipe_s *p = c->pipe;
    if (!p || p->pass) { set_sticky(c, NVN_GPU_E_ORDER, "dispatch 缺计算管线"); return; }
    if (ind && x | y | z) { set_sticky(c, NVN_GPU_E_ARG, "间接 dispatch 与计数不能同时给"); return; }
    if (ind) { if (!buf_range(ind, off, 12)) { set_sticky(c, NVN_GPU_E_ARG, "间接 dispatch 参数越界"); return; } }
    c->pipe = p;
    if (p->nset) {
        if (!ensure_dpool(c)) return;
        VkDescriptorSetAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = c->dpool;
        ai.descriptorSetCount = p->nset;
        ai.pSetLayouts = p->dsl;
        VkDescriptorSet sets[4] = {0};
        if (vkAllocateDescriptorSets(g.d, &ai, sets) != VK_SUCCESS) { set_sticky(c, NVN_GPU_E_OOM, "vkAllocateDescriptorSets 失败"); return; }
        VkWriteDescriptorSet wr[64];
        VkDescriptorImageInfo ii[64];
        VkDescriptorBufferInfo bi[64];
        int nw = write_binds(c, p, NVN_GPU_SH_CS, sets, wr, ii, bi);
        if (nw < 0) return;
        if (nw) vkUpdateDescriptorSets(g.d, (uint32_t)nw, wr, 0, NULL);
        vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, p->nset, sets, 0, NULL);
    }
    if (c->push_have[NVN_GPU_SH_CS] && p->stage_push_bytes[NVN_GPU_SH_CS])
        vkCmdPushConstants(c->cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           p->stage_push_off[NVN_GPU_SH_CS], p->stage_push_bytes[NVN_GPU_SH_CS],
                           c->push_data[NVN_GPU_SH_CS] + p->stage_push_off[NVN_GPU_SH_CS]);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    if (ind) {
        vkCmdDispatchIndirect(c->cb, ind->buffer, off);
        cmd_ref(c, ind, REF_BUF);
    } else {
        vkCmdDispatch(c->cb, x, y, z);
    }
}
// ---------------------------------------------------------------- 贴图 / 视图 / 采样器

static uint32_t fmt_bpp(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB: return 1;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_D16_UNORM: return 2;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;   // 压缩 / 打包格式先不算行宽：调用方按 E_UNSUPPORTED 处理
    }
}

static VkImageAspectFlags fmt_aspect(VkFormat f) {
    if (!nvn_vk_format_is_depth(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (nvn_vk_format_has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static VkPipelineStageFlags layout_dst_stage(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}
static VkAccessFlags layout_dst_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}

// 记录线程单线程编码（契约 [提交]）；布局按贴图整体跟踪，进出操作各转一次。
static void image_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout to) {
    if (t->layout == to) return;
    if (c->nlay < 64) {   // abort / 提交前失败时按逆序回滚，避免记录态布局和实际不一致
        c->lay[c->nlay].t = t;
        c->lay[c->nlay].old = t->layout;
        c->nlay++;
    }
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags src_access = 0;
    switch (t->layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; src_access = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; src_access = VK_ACCESS_TRANSFER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        src_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        src_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_GENERAL:
        src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        src_access = VK_ACCESS_MEMORY_WRITE_BIT;
        break;
    default: break;
    }
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = t->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = (VkImageSubresourceRange){t->aspect, 0, t->levels, 0, t->layers};
    b.srcAccessMask = src_access;
    b.dstAccessMask = layout_dst_access(to);
    vkCmdPipelineBarrier(c->cb, src_stage, layout_dst_stage(to), 0, 0, NULL, 0, NULL, 1, &b);
    t->layout = to;
}

static int copy_region(const struct nvn_gpu_tex_s *t, uint32_t level, const NVNcopyRegion *r, VkOffset3D *off, VkExtent3D *ext) {
    uint32_t w = t->w >> level, h = t->h >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    if (t->target == NVN_TEXTURE_TARGET_3D) {
        if (r && (r->width > 0 || r->height > 0)) {
            uint32_t rz = (uint32_t)(r->depth > 0 ? r->depth : 1);
            if (r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
                (uint32_t)r->xoffset + (uint32_t)r->width > w ||
                (uint32_t)r->yoffset + (uint32_t)r->height > h ||
                (uint32_t)r->zoffset + rz > d) return -1;
            *off = (VkOffset3D){r->xoffset, r->yoffset, r->zoffset};
            *ext = (VkExtent3D){(uint32_t)r->width, (uint32_t)r->height, rz};
        } else {
            *off = (VkOffset3D){0, 0, r ? r->zoffset : 0};
            *ext = (VkExtent3D){w, h, (uint32_t)(r && r->depth > 0 ? (uint32_t)r->depth : d)};
        }
        return 0;
    }
    if (r && r->width > 0 && r->height > 0) {
        if (r->xoffset < 0 || r->yoffset < 0 || (uint32_t)r->xoffset + (uint32_t)r->width > w ||
            (uint32_t)r->yoffset + (uint32_t)r->height > h || r->zoffset != 0 || r->depth > 1)
            return -1;
        *off = (VkOffset3D){r->xoffset, r->yoffset, 0};
        *ext = (VkExtent3D){(uint32_t)r->width, (uint32_t)r->height, 1};
    } else {
        *off = (VkOffset3D){0, 0, 0};
        *ext = (VkExtent3D){w, h, 1};
    }
    return 0;
}

static int check_copy_params(const struct nvn_gpu_tex_s *t, uint32_t level, const NVNcopyRegion *r,
                             uint64_t row_bytes, uint64_t image_bytes, uint32_t bpp,
                             VkOffset3D *off, VkExtent3D *ext) {
    if (!t || level >= t->levels) return -1;
    if (copy_region(t, level, r, off, ext)) return -1;
    if (!row_bytes || row_bytes < (uint64_t)ext->width * bpp) return -1;
    if (!image_bytes || image_bytes < row_bytes * ext->height) return -1;
    return 0;
}

nvn_gpu_rc nvn_gpu_tex_create(const nvn_gpu_tex_desc *d, nvn_gpu_tex *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (!d) return fail(err, NVN_GPU_E_ARG, "描述为空");
    if (d->host_w || d->host_h) return fail(err, NVN_GPU_E_UNSUPPORTED, "宽屏宿主尺寸（host_w/h）：本段未实现");
    if (d->swz) return fail(err, NVN_GPU_E_UNSUPPORTED, "存储自带通道映射（swz）：本段未实现");
    if (d->stencil_view) return fail(err, NVN_GPU_E_UNSUPPORTED, "模板视图：本段未实现");
    uint32_t target = (uint32_t)d->d.target;
    uint32_t depth = d->d.depth ? (uint32_t)d->d.depth : 1;
    uint32_t layers = 1;
    VkImageType itype = VK_IMAGE_TYPE_2D;
    VkImageViewType vtype = VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateFlags iflags = 0;
    int array_compat = 0;
    switch (target) {
    case NVN_TEXTURE_TARGET_2D: break;
    case NVN_TEXTURE_TARGET_2D_ARRAY:
        layers = depth; vtype = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case NVN_TEXTURE_TARGET_3D:
        itype = VK_IMAGE_TYPE_3D; vtype = VK_IMAGE_VIEW_TYPE_3D;
        array_compat = 1;
        iflags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        break;
    case NVN_TEXTURE_TARGET_CUBEMAP:
        layers = 6; vtype = VK_IMAGE_VIEW_TYPE_CUBE;
        iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
        if (!g.dev->core_features.imageCubeArray)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "立方体数组需要 imageCubeArray 设备特性（未启用）");
        if (!depth || depth % 6) return fail(err, NVN_GPU_E_ARG, "立方体数组 depth=%u 必须是 6 的倍数", depth);
        layers = depth; vtype = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    default:
        return fail(err, NVN_GPU_E_UNSUPPORTED, "target %u：本段只实现 2D / 2D 数组 / 立方体 / 立方体数组 / 3D", target);
    }
    VkFormat fmt = nvn_vk_tex_format((uint32_t)d->d.format);
    if (fmt == VK_FORMAT_UNDEFINED) return fail(err, NVN_GPU_E_UNSUPPORTED, "NVN 格式 0x%x 无 Vulkan 映射", d->d.format);
    // #1199：需要 compute 解码的 BC 格式走 guest 纹理缓存路径；nvn_gpu_tex 侧不静默支持，报红。
    if (d->d.format >= 0x41 && d->d.format <= 0x50 && (g.dev->bc_decode_mask >> ((uint32_t)d->d.format - 0x41)) & 1u)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "BC format 0x%x needs compute decode: only guest texture-cache path is bridged", d->d.format);
    if (!d->d.width || !d->d.height) return fail(err, NVN_GPU_E_ARG, "尺寸为 0");
    uint32_t levels = d->d.levels ? (uint32_t)d->d.levels : 1;
    uint32_t samples = d->d.samples > 1 ? (uint32_t)d->d.samples : 1;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d->usage & NVN_GPU_TEX_SAMPLED) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (d->usage & NVN_GPU_TEX_RENDER) {
        usage |= nvn_vk_format_is_depth(fmt) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (d->usage & NVN_GPU_TEX_STORAGE) usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    struct nvn_gpu_tex_s *t = calloc(1, sizeof *t);
    if (!t) return fail(err, NVN_GPU_E_OOM, "贴图结构分配失败");
    t->format = fmt;
    t->aspect = fmt_aspect(fmt);
    t->w = d->d.width;
    t->h = d->d.height;
    t->levels = levels;
    t->layers = layers;
    t->depth = depth;
    t->target = target;
    t->array_compatible = array_compat;
    t->samples = samples;
    t->nvn_format = (uint32_t)d->d.format;
    t->view_fmt = (d->usage & NVN_GPU_TEX_VIEW_FMT) != 0;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = itype;
    ci.format = fmt;
    ci.extent = (VkExtent3D){d->d.width, d->d.height, itype == VK_IMAGE_TYPE_3D ? depth : 1};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.flags = iflags;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (t->view_fmt) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (vkCreateImage(g.d, &ci, NULL, &t->image) != VK_SUCCESS) {
        free(t);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateImage 拒绝（格式 0x%x / usage 0x%x / %ux%u）", d->d.format, usage, d->d.width, d->d.height);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.d, t->image, &req);
    uint32_t type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, 0, NULL);
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(g.d, &ai, NULL, &t->memory) != VK_SUCCESS ||
        vkBindImageMemory(g.d, t->image, t->memory, 0) != VK_SUCCESS) {
        if (t->memory) vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "贴图显存分配失败（%ux%u，%u 级）", d->d.width, d->d.height, levels);
    }
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.viewType = vtype;
    vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){t->aspect, 0, levels, 0, itype == VK_IMAGE_TYPE_3D ? 1 : layers};
    if (vkCreateImageView(g.d, &vi, NULL, &t->view) != VK_SUCCESS) {
        vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "默认贴图视图创建失败");
    }
    pthread_mutex_lock(&g_mu);
    t->all_next = g.texs;
    g.texs = t;
    pthread_mutex_unlock(&g_mu);
    *out = t;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_tex_info_get(nvn_gpu_tex th, nvn_gpu_tex_info *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    struct nvn_gpu_tex_s *t = th;
    if (!t) return;
    out->w = t->w;
    out->h = t->h;
    out->depth = t->depth;
    out->levels = t->levels;
    out->layers = t->layers;
    out->samples = t->samples;
    out->nvn_format = t->nvn_format;
    out->fmt_flags = nvn_gpu_format_caps(t->nvn_format);
}

void nvn_gpu_tex_release(nvn_gpu_tex th) {
    struct nvn_gpu_tex_s *t = th;
    if (!t) return;
    pthread_mutex_lock(&g_mu);
    if (!t->released) {
        t->released = 1;
        if (t->refs == 0) destroy_tex_locked(&g, t);
    }
    pthread_mutex_unlock(&g_mu);
}

nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex th, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    struct nvn_gpu_tex_s *t = th;
    if (!t) return fail(err, NVN_GPU_E_ARG, "贴图为空");
    uint32_t base_level = 0, levels = t->levels, base_layer = 0, layers = t->layers;
    VkFormat fmt = t->format;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    if (v) {
        if (v->set_levels) { base_level = v->base_level; levels = v->nlevels ? v->nlevels : 1; }
        if (v->set_layers) { base_layer = v->min_layer; layers = v->nlayers ? v->nlayers : 1; }
        if (v->set_format && (uint32_t)v->format != t->nvn_format) {
            VkFormat of = nvn_vk_tex_format((uint32_t)v->format);
            if (of == VK_FORMAT_UNDEFINED) return fail(err, NVN_GPU_E_ARG, "视图格式 0x%x 无映射", v->format);
            fmt = of;
        }
    }
    struct nvn_vk_texview_plan plan;
    nvn_vk_texview_plan_make(t->target, t->depth, t->layers, t->levels, v, 0, t->array_compatible, &plan);
    if (!plan.ok) return fail(err, NVN_GPU_E_ARG, "%s", plan.why);
    base_level = plan.base_level; levels = plan.nlevels;
    base_layer = plan.base_layer; layers = plan.nlayers; type = plan.type;
    if (fmt != t->format && !t->view_fmt)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "不同格式的视图需要创建时声明 NVN_GPU_TEX_VIEW_FMT");

    struct nvn_gpu_view_s *o = calloc(1, sizeof *o);
    if (!o) return fail(err, NVN_GPU_E_OOM, "视图结构分配失败");
    o->nvn_format = v && v->set_format ? (uint32_t)v->format : t->nvn_format;
    VkImageViewCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = t->image;
    ci.viewType = type;
    ci.format = fmt;
    ci.subresourceRange = (VkImageSubresourceRange){t->aspect, base_level, levels, base_layer, layers};
    if (vkCreateImageView(g.d, &ci, NULL, &o->view) != VK_SUCCESS) {
        free(o);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateImageView 拒绝（格式 / 范围）");
    }
    o->tex = t;
    o->base_level = base_level;
    o->levels = levels;
    o->base_layer = base_layer;
    o->layers = layers;
    pthread_mutex_lock(&g_mu);
    t->refs++;   // 视图让贴图活到视图 release
    o->all_next = g.views;
    g.views = o;
    pthread_mutex_unlock(&g_mu);
    *out = o;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_view_create_buf(nvn_gpu_buf b, uint64_t off, uint64_t bytes, uint32_t fmt, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    nvn_fmt_info info;
    VkFormat f = nvn_vk_format(fmt);
    if (f == VK_FORMAT_UNDEFINED || !nvn_fmt_info_of(fmt, &info) ||
        info.bw != 1 || info.bh != 1 || !info.bytes)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "缓冲视图格式 0x%x 不支持", fmt);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &fp);
    VkFormatFeatureFlags features = fp.bufferFeatures &
        (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT);
    uint32_t caps = nvn_gpu_format_caps(fmt);
    // 底层 VkBuffer 同时声明两种 texel 用途；视图必须满足两项格式特性。
    if (features != (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "格式 0x%x 不支持缓冲声明的全部 texel 用途", fmt);
    if (!((features & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) && (caps & NVN_GPU_FMT_SAMPLE)) &&
        !((features & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) && (caps & NVN_GPU_FMT_STORAGE)))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "格式 0x%x 不支持纹素缓冲", fmt);
    uint64_t align = g.dev->properties.limits.minTexelBufferOffsetAlignment;
    if (!bytes || bytes % info.bytes || (align && off % align) ||
        bytes / info.bytes > g.dev->properties.limits.maxTexelBufferElements)
        return fail(err, NVN_GPU_E_ARG, "缓冲视图范围 / 对齐非法");
    struct nvn_gpu_view_s *v = calloc(1, sizeof *v);
    if (!v) return fail(err, NVN_GPU_E_OOM, "缓冲视图结构分配失败");
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_buf_s *found = g.bufs;
    while (found && found != b) found = found->all_next;
    if (g.closing || !found || found->released || !buf_range(found, off, bytes)) {
        int closing = g.closing;
        pthread_mutex_unlock(&g_mu);
        free(v);
        return fail(err, closing ? NVN_GPU_E_STOPPED : NVN_GPU_E_ARG, "缓冲无效或视图范围越界");
    }
    VkBufferViewCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    ci.buffer = found->buffer; ci.format = f; ci.offset = off; ci.range = bytes;
    VkResult result = vkCreateBufferView(g.d, &ci, NULL, &v->buffer_view);
    if (result != VK_SUCCESS) {
        pthread_mutex_unlock(&g_mu);
        free(v);
        return fail(err, result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                         ? NVN_GPU_E_OOM : NVN_GPU_E_UNSUPPORTED, "vkCreateBufferView 失败（%d）", result);
    }
    found->refs++;
    v->buf = found; v->nvn_format = fmt; v->buffer_format = f; v->buffer_features = features; v->buffer_caps = caps;
    v->all_next = g.views; g.views = v;
    pthread_mutex_unlock(&g_mu);
    *out = v;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_view_release(nvn_gpu_view vh) {
    struct nvn_gpu_view_s *v = vh;
    if (!v) return;
    pthread_mutex_lock(&g_mu);
    if (!v->released) {
        v->released = 1;
        if (v->refs == 0) destroy_view_locked(&g, v);
    }
    pthread_mutex_unlock(&g_mu);
}

static VkSamplerAddressMode samp_wrap(uint8_t w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRROR_CLAMP:
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER:
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkCompareOp samp_compare(uint8_t m) {
    switch (m) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

nvn_gpu_rc nvn_gpu_samp_create(const samp_par *p, nvn_gpu_samp *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!p) return fail(err, NVN_GPU_E_ARG, "samp_par 为空");
    struct nvn_gpu_samp_s *s = calloc(1, sizeof *s);
    if (!s) return fail(err, NVN_GPU_E_OOM, "采样器结构分配失败");
    VkSamplerCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    switch (p->minf) {
    case NVN_MIN_FILTER_NEAREST: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_LINEAR: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    default: free(s); return fail(err, NVN_GPU_E_ARG, "NVN 最小过滤 %u 无映射", p->minf);
    }
    ci.addressModeU = samp_wrap(p->ws);
    ci.addressModeV = samp_wrap(p->wt);
    ci.addressModeW = samp_wrap(p->wr);
    ci.mipLodBias = p->lod_bias;
    ci.minLod = p->lod_min;
    ci.maxLod = p->lod_max;
    if (p->aniso > 1.0f) {
        if (!g.dev->core_features.samplerAnisotropy) {
            free(s);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "请求各向异性 %.2f，但设备未启用 samplerAnisotropy", p->aniso);
        }
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = p->aniso;
    } else ci.maxAnisotropy = 1.0f;
    if (p->cmp_mode != NVN_COMPARE_MODE_NONE) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = samp_compare(p->cmp_func);
    }
    int white = p->border[0] == 1 && p->border[1] == 1 && p->border[2] == 1 && p->border[3] == 1;
    int transparent = p->border[0] == 0 && p->border[1] == 0 && p->border[2] == 0 && p->border[3] == 0;
    ci.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                           : transparent ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(g.d, &ci, NULL, &s->sampler) != VK_SUCCESS) {
        free(s);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateSampler 拒绝（可能缺少 samplerAnisotropy）");
    }
    pthread_mutex_lock(&g_mu);
    s->all_next = g.samps;
    g.samps = s;
    pthread_mutex_unlock(&g_mu);
    *out = s;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_samp_release(nvn_gpu_samp sh) {
    struct nvn_gpu_samp_s *s = sh;
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    if (!s->released) {
        s->released = 1;
        if (s->refs == 0) destroy_samp_locked(&g, s);
    }
    pthread_mutex_unlock(&g_mu);
}

// 线性内容上传 / 缓冲 ↔ 贴图拷贝。全部在通道外，按 GPU 顺序执行。
void nvn_gpu_copy_buf_tex(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes,
                          nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *t = th;
    struct nvn_gpu_buf_s *b = bh;
    uint32_t bpp = t ? fmt_bpp(t->format) : 0;
    VkOffset3D o;
    VkExtent3D e;
    NVNcopyRegion rr;
    if (t && t->target == NVN_TEXTURE_TARGET_3D) {
        if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
        memset(&rr, 0, sizeof rr);
        if (r) rr = *r;
        if (rr.depth <= 0) rr.depth = 1;
        rr.zoffset = (int)layer;
        r = &rr;
    }
    if (!t || !b || !bpp || (t->target != NVN_TEXTURE_TARGET_3D && layer >= t->layers) || (off & 3) ||
        !buf_range(b, off, image_bytes) ||
        check_copy_params(t, level, r, row_bytes, image_bytes, bpp, &o, &e)) {
        set_sticky(c, NVN_GPU_E_ARG, "copy_buf_tex 参数非法（越界 / 行宽 / 偏移未 4 对齐）");
        return;
    }
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bc;
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.bufferRowLength = (uint32_t)(row_bytes / bpp);
    bc.bufferImageHeight = t->target == NVN_TEXTURE_TARGET_3D ? e.height : (uint32_t)(image_bytes / row_bytes);
    bc.imageSubresource = (VkImageSubresourceLayers){t->aspect, level, t->target == NVN_TEXTURE_TARGET_3D ? 0 : layer, 1};
    bc.imageOffset = o;
    bc.imageExtent = e;
    vkCmdCopyBufferToImage(c->cb, b->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);   // 之后按采样 / 存储使用；本段统一用 GENERAL
    cmd_ref(c, b, REF_BUF);
    cmd_ref(c, t, REF_TEX);
}

void nvn_gpu_copy_tex_buf(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                          nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *t = th;
    struct nvn_gpu_buf_s *b = bh;
    nvn_fmt_info fi;
    if (t && nvn_fmt_info_of(t->nvn_format, &fi) && fi.expand) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "展开格式回读未实现（0x%x）", t->nvn_format);
        return;
    }
    uint32_t bpp = t ? fmt_bpp(t->format) : 0;
    VkOffset3D o;
    VkExtent3D e;
    NVNcopyRegion rr;
    if (t && t->target == NVN_TEXTURE_TARGET_3D) {
        if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
        memset(&rr, 0, sizeof rr);
        if (r) rr = *r;
        if (rr.depth <= 0) rr.depth = 1;
        rr.zoffset = (int)layer;
        r = &rr;
    }
    if (!t || !b || !bpp || (t->target != NVN_TEXTURE_TARGET_3D && layer >= t->layers) || (off & 3) ||
        !buf_range(b, off, image_bytes) ||
        check_copy_params(t, level, r, row_bytes, image_bytes, bpp, &o, &e)) {
        set_sticky(c, NVN_GPU_E_ARG, "copy_tex_buf 参数非法（越界 / 行宽 / 偏移未 4 对齐）");
        return;
    }
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bc;
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.bufferRowLength = (uint32_t)(row_bytes / bpp);
    bc.bufferImageHeight = t->target == NVN_TEXTURE_TARGET_3D ? e.height : (uint32_t)(image_bytes / row_bytes);
    bc.imageSubresource = (VkImageSubresourceLayers){t->aspect, level, t->target == NVN_TEXTURE_TARGET_3D ? 0 : layer, 1};
    bc.imageOffset = o;
    bc.imageExtent = e;
    vkCmdCopyImageToBuffer(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buffer, 1, &bc);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, t, REF_TEX);
    cmd_ref(c, b, REF_BUF);
}

void nvn_gpu_copy_tex(nvn_gpu_cmd ch, nvn_gpu_tex sh, uint32_t slevel, uint32_t slayer, const NVNcopyRegion *sr,
                      nvn_gpu_tex dh, uint32_t dlevel, uint32_t dlayer, const NVNcopyRegion *dr) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *s = sh, *d = dh;
    uint32_t bpp = s ? fmt_bpp(s->format) : 0;
    VkOffset3D so, dof;
    VkExtent3D se, de;
    if (!s || !d || !bpp || bpp != fmt_bpp(d->format) || s->format != d->format || slayer >= s->layers ||
        dlayer >= d->layers || copy_region(s, slevel, sr, &so, &se) || copy_region(d, dlevel, dr, &dof, &de) ||
        se.width != de.width || se.height != de.height || se.depth != de.depth) {
        set_sticky(c, NVN_GPU_E_ARG, "copy_tex 参数非法（格式 / 尺寸 / 范围）");
        return;
    }
    full_barrier(c);
    image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic;
    memset(&ic, 0, sizeof ic);
    ic.srcSubresource = (VkImageSubresourceLayers){s->aspect, slevel, slayer, 1};
    ic.srcOffset = so;
    ic.dstSubresource = (VkImageSubresourceLayers){d->aspect, dlevel, dlayer, 1};
    ic.dstOffset = dof;
    ic.extent = se;
    vkCmdCopyImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, s, REF_TEX);
    cmd_ref(c, d, REF_TEX);
}

void nvn_gpu_clear_view(nvn_gpu_cmd ch, nvn_gpu_view vh, const float rgba[4], const uint32_t u4[4], int is_int) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_view_s *v = vh;
    if (!v) { set_sticky(c, NVN_GPU_E_ARG, "clear_view 视图为空"); return; }
    struct nvn_gpu_tex_s *t = v->tex;
    if (v->nvn_format != t->nvn_format) {
        // vkCmdClearColorImage 按 image 格式解释 clear 值，无法按 view 的覆盖格式生效
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "clear_view 不支持视图覆盖格式");
        return;
    }
    if (!(t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        nvn_fmt_info fi;
        if (nvn_fmt_info_of(v->nvn_format, &fi)) {
            int fmt_int = fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT;
            if (fmt_int != (is_int != 0)) {
                set_sticky(c, NVN_GPU_E_ARG, "clear_view 的 is_int 与贴图格式不符");
                return;
            }
        }
    }
    VkImageSubresourceRange range = {t->aspect, v->base_level, v->levels, v->base_layer, v->layers};
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        VkClearDepthStencilValue ds;
        ds.depth = rgba ? rgba[0] : 1.0f;
        ds.stencil = 0;
        vkCmdClearDepthStencilImage(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ds, 1, &range);
    } else {
        VkClearColorValue col;
        memset(&col, 0, sizeof col);
        for (int i = 0; i < 4; i++) {
            if (is_int) col.uint32[i] = u4 ? u4[i] : 0;
            else col.float32[i] = rgba ? rgba[i] : 0.0f;
        }
        vkCmdClearColorImage(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
    }
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, t, REF_TEX);
}

void nvn_gpu_tex_upload(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!src) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload src 为空"); return; }
    struct nvn_gpu_tex_s *t = th;
    if (!t) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 贴图为空"); return; }
    int ebpp = 0;
    int expand = nvn_fmt_expand_target(t->nvn_format, NULL, &ebpp);
    uint32_t ew = r ? (uint32_t)r->width : (t->w >> level);
    uint32_t eh = r ? (uint32_t)r->height : (t->h >> level);
    if (!ew) ew = 1;
    if (!eh) eh = 1;
    if (expand && (r && (r->width <= 0 || r->height <= 0))) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开区域非法"); return; }
    uint64_t out_row = expand ? (uint64_t)ebpp * ew : 0;
    uint64_t out_bytes = expand ? out_row * eh : 0;
    if (expand) {
        nvn_fmt_info in;
        if (!nvn_fmt_info_of(t->nvn_format, &in)) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 源格式未知"); return; }
        uint64_t need_row = (uint64_t)in.bytes * ew;
        uint64_t need_img = (uint64_t)row_bytes * (eh - 1) + need_row;
        if (row_bytes < need_row || image_bytes < need_img) {
            set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开源越界（行 %llu < %llu 或总 %llu < %llu）",
                       (unsigned long long)row_bytes, (unsigned long long)need_row,
                       (unsigned long long)image_bytes, (unsigned long long)need_img);
            return;
        }
    }
    uint64_t need = expand ? out_bytes : image_bytes;
    nvn_gpu_slice sl;
    nvn_gpu_err e;
    if (!rc_is(nvn_gpu_arena_alloc(ch, need, 4, &sl, &e), NVN_GPU_OK)) {
        set_sticky(c, e.rc.code, "tex_upload 暂存：%s", e.why);
        return;
    }
    if (expand) {
        if (nvn_fmt_expand_upload(t->nvn_format, src, (size_t)row_bytes, ew, eh,
                                  sl.cpu, (size_t)out_row) != 0) {
            set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开失败（格式 0x%x）", t->nvn_format);
            return;
        }
        nvn_gpu_copy_buf_tex(ch, sl.buf, sl.off, out_row, out_bytes, th, level, layer, r);
    } else {
        memcpy(sl.cpu, src, (size_t)image_bytes);
        nvn_gpu_copy_buf_tex(ch, sl.buf, sl.off, row_bytes, image_bytes, th, level, layer, r);
    }
}

void nvn_gpu_barrier(nvn_gpu_cmd ch, uint32_t nvn_bits) {
    (void)nvn_bits;
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    // 保守实现：先把所有已写内存对后续所有阶段可见；按 NVN 位收窄留给后续。
    full_barrier(c);
}

void nvn_gpu_blit(nvn_gpu_cmd c, nvn_gpu_view s, const NVNcopyRegion *sr, nvn_gpu_view d, const NVNcopyRegion *dr, int linear) {
    (void)s; (void)sr; (void)d; (void)dr; (void)linear;
    set_sticky((struct nvn_gpu_cmd_s *)c, NVN_GPU_E_UNSUPPORTED, "blit：未实现（不置 CAP_BLIT_SCALED）");
}

void nvn_gpu_occlusion(nvn_gpu_cmd c, int32_t slot) {
    (void)slot;
    set_sticky((struct nvn_gpu_cmd_s *)c, NVN_GPU_E_UNSUPPORTED, "遮挡查询：未实现");
}

void nvn_gpu_label(nvn_gpu_cmd c, const char *text) { (void)c; (void)text; }

void nvn_gpu_tex_upload_blocklinear(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t l, uint32_t y, const nvn_texlayout *L, const void *src, uint64_t bytes) {
    (void)t; (void)l; (void)y; (void)L; (void)src; (void)bytes; VK_NOT_YET(c, "nvn_gpu_tex_upload_blocklinear");
}
nvn_gpu_rc nvn_gpu_surface_attach(void *native, nvn_gpu_surface *out, nvn_gpu_err *err) {
    (void)native; if (out) *out = NULL;
    return fail(err, NVN_GPU_E_UNSUPPORTED, "上屏：D8 第一段未实现");
}
nvn_gpu_rc nvn_gpu_surface_rebuild(nvn_gpu_surface s, nvn_gpu_err *err) {
    (void)s; return fail(err, NVN_GPU_E_UNSUPPORTED, "上屏：D8 第一段未实现");
}
uint64_t nvn_gpu_surface_gen(nvn_gpu_surface s) { (void)s; return 0; }
nvn_gpu_rc nvn_gpu_surface_detach(nvn_gpu_surface s, uint64_t timeout_ns, nvn_gpu_err *err) {
    (void)s; (void)timeout_ns; return fail(err, NVN_GPU_E_UNSUPPORTED, "上屏：D8 第一段未实现");
}
nvn_gpu_rc nvn_gpu_drawable_acquire(nvn_gpu_surface s, uint64_t timeout_ns, void *plat_frame, nvn_gpu_drawable *out, nvn_gpu_err *err) {
    (void)s; (void)timeout_ns; (void)plat_frame; if (out) memset(out, 0, sizeof *out);
    return fail(err, NVN_GPU_E_UNSUPPORTED, "上屏：D8 第一段未实现");
}
void nvn_gpu_drawable_release(nvn_gpu_surface s, const nvn_gpu_drawable *dr) { (void)s; (void)dr; }
nvn_gpu_rc nvn_gpu_present(nvn_gpu_surface s, const nvn_gpu_drawable *dr, const nvn_gpu_present_desc *d, uint64_t ticket,
                           nvn_gpu_presented_fn presented, void *user, nvn_gpu_err *err) {
    (void)s; (void)dr; (void)d; (void)ticket;
    if (presented) {
        nvn_gpu_shown shown = {ticket, NVN_GPU_SHOWN_DROPPED, 0.0};
        presented(user, &shown);
    }
    return fail(err, NVN_GPU_E_UNSUPPORTED, "上屏：D8 第一段未实现");
}

// ---------------------------------------------------------------- 命令

nvn_gpu_rc nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *d, nvn_gpu_cmd *out, nvn_gpu_err *err) {
    (void)d;
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许开命令段");
    pthread_mutex_lock(&g_mu);
    int closing = g.closing, lost = g.device_lost, failed = g.failed != 0;
    pthread_mutex_unlock(&g_mu);
    if (closing) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    if (failed) return fail(err, NVN_GPU_E_FAILED, "时间线已进入失败终态（恢复只能关会话重开）");
    if (lost) return fail(err, NVN_GPU_E_DEVICE, "设备已丢失");

    struct nvn_gpu_cmd_s *c = calloc(1, sizeof *c);
    if (!c) return fail(err, NVN_GPU_E_OOM, "cmd 分配失败");
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = g.dev->queue_family;
    if (vkCreateCommandPool(g.d, &pci, NULL, &c->pool) != VK_SUCCESS) {
        free(c);
        return fail(err, NVN_GPU_E_OOM, "vkCreateCommandPool 失败");
    }
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = c->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.d, &ai, &c->cb) != VK_SUCCESS) {
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_OOM, "vkAllocateCommandBuffers 失败");
    }
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(c->cb, &bi) != VK_SUCCESS) {
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_DEVICE, "vkBeginCommandBuffer 失败");
    }
    c->recording = 1;
    c->sticky = rc(NVN_GPU_OK);
    pthread_mutex_lock(&g_mu);
    if (g.closing) {
        pthread_mutex_unlock(&g_mu);
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    c->live_next = g.live;
    g.live = c;
    pthread_mutex_unlock(&g_mu);
    *out = c;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_cmd_abort(nvn_gpu_cmd ch) {
    if (!ch || !g_open) return;
    struct nvn_gpu_cmd_s *c = ch;
    int owned = 0;
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (*pp) {
        *pp = c->live_next;
        c->live_next = NULL;
        owned = 1;   // 摘链与销毁所有权原子完成；并发 abort / close 不会重复释放
    }
    pthread_mutex_unlock(&g_mu);
    if (owned) finish_cmd(c, 0);   // 不执行、不回调、不推进时间线；引用随即解除
}

static void enqueue_segment(struct nvn_gpu_vk *vk, struct vk_segment *s) {
    pthread_mutex_lock(&g_mu);
    if (vk->qtail) vk->qtail->next = s;
    else vk->qhead = s;
    vk->qtail = s;
    if (!submitted_add(vk, s->signal) && (!vk->failed || s->signal < vk->failed)) vk->failed = s->signal;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void publish_signal(uint64_t signal, int failed) {
    pthread_mutex_lock(&g_mu);
    g.last_signal = signal;
    if (failed) {
        if (!g.failed || signal < g.failed) g.failed = signal;
    } else {
        g.stats[STAT_SUBMITS]++;
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// 提交串行化：signal 校验、vkEndCommandBuffer、vkQueueSubmit、入队必须在同一临界区，
// 否则两个提交线程可能让更大的 signal 先入队，或同时通过 last_signal 检查。
static nvn_gpu_rc commit_impl(nvn_gpu_cmd ch, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    struct nvn_gpu_cmd_s *c = ch;

    struct vk_segment *s = calloc(1, sizeof *s);
    if (!s) {
        finish_cmd(c, 0);
        return fail(err, NVN_GPU_E_OOM, "段记录分配失败（本段未提交）");
    }
    s->cmd = c;
    s->signal = signal;
    s->done = done;
    s->user = user;
    s->cb = c->cb;

    pthread_mutex_lock(&g_submit_mu);

    pthread_mutex_lock(&g_mu);
    int closing = g.closing, lost = g.device_lost, inject = g.fault_inject, failed_state = g.failed != 0;
    if (inject) g.fault_inject = 0;
    uint64_t last = g.last_signal;
    pthread_mutex_unlock(&g_mu);

    if (closing) {
        pthread_mutex_unlock(&g_submit_mu);
        finish_cmd(c, 0);
        free(s);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    // 契约 v2 的 commit：signal 必须严格大于上一次；本实现同时拒绝 0（F = 0 表示「没有失败」，
    // 无信号段失败后无法表达；见文件头消费审查第 2 条）。
    if (signal == 0 || signal <= last) {
        // 违约被拒：不提交、不推进 last_signal，但按失败处理——发布 F = 上一次合法值 + 1，
        // 再以 FAILED（rc = E_ARG）回调本段一次，signal 用调用方给的原值。
        uint64_t f = add_sat(last, 1);
        pthread_mutex_lock(&g_mu);
        g.failed = g.failed ? (f < g.failed ? f : g.failed) : f;
        pthread_mutex_unlock(&g_mu);
        s->failed = 1;
        s->rc = rc(NVN_GPU_E_ARG);
        snprintf(s->why, sizeof s->why, "signal %llu 未严格大于上一次合法提交 %llu（F=%llu）",
                 (unsigned long long)signal, (unsigned long long)last, (unsigned long long)f);
        enqueue_segment(&g, s);
        pthread_mutex_unlock(&g_submit_mu);
        return fail(err, NVN_GPU_E_ARG, "%s", s->why);
    }

    if (c->in_pass) set_sticky(c, NVN_GPU_E_ORDER, "commit 时通道还开着");
    nvn_gpu_rc sticky = c->sticky;
    if (!rc_is(sticky, NVN_GPU_OK) || lost || inject || failed_state) {
        nvn_gpu_rc code = !rc_is(sticky, NVN_GPU_OK) ? sticky
                        : rc(lost ? NVN_GPU_E_DEVICE : (failed_state ? NVN_GPU_E_FAILED : NVN_GPU_E_DEVICE));
        const char *why = !rc_is(sticky, NVN_GPU_OK) ? c->why
                        : (lost ? "设备已丢失" : (failed_state ? "时间线已失败，本段不执行" : "测试注入：本段提交失败"));
        s->failed = 1;
        s->rc = code;
        snprintf(s->why, sizeof s->why, "%s", why);
        publish_signal(signal, 1);
        enqueue_segment(&g, s);
        pthread_mutex_unlock(&g_submit_mu);
        return code;
    }

    // 段末：让本段的所有 GPU 写对主机可见（fence 只保证执行完，不保证主机可见）。
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT);
    // 结束录制并提交。
    VkResult r = vkEndCommandBuffer(c->cb);
    if (r == VK_SUCCESS) {
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        r = vkCreateFence(g.d, &fi, NULL, &fence);
        if (r == VK_SUCCESS) {
            VkSubmitInfo si;
            memset(&si, 0, sizeof si);
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &c->cb;
            r = vkQueueSubmit(g.queue, 1, &si, fence);
            if (r == VK_SUCCESS) {
                s->fence = fence;
                s->submitted = 1;
                s->executed = 1;
                publish_signal(signal, 0);
                enqueue_segment(&g, s);   // 此后 cmd 归 worker（finish_cmd 负责摘链）
                pthread_mutex_unlock(&g_submit_mu);
                return rc(NVN_GPU_OK);
            }
            vkDestroyFence(g.d, fence, NULL);
            err = err ? err : &(nvn_gpu_err){0};
            fail(err, NVN_GPU_E_DEVICE, "vkQueueSubmit 失败");
        } else {
            err = err ? err : &(nvn_gpu_err){0};
            fail(err, NVN_GPU_E_OOM, "vkCreateFence 失败");
        }
    } else {
        err = err ? err : &(nvn_gpu_err){0};
        fail(err, NVN_GPU_E_DEVICE, "vkEndCommandBuffer 失败");
    }

    // 提交前失败：段不执行，按失败终止发布 F，并给本段恰好一次 FAILED 回调。
    s->failed = 1;
    s->rc = err->rc;
    snprintf(s->why, sizeof s->why, "%s", err->why);
    publish_signal(signal, 1);
    enqueue_segment(&g, s);
    pthread_mutex_unlock(&g_submit_mu);
    return err->rc;
}

nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd ch, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    if (!ch) return fail(err, NVN_GPU_E_ARG, "cmd 为空");
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许提交");
    struct nvn_gpu_cmd_s *c = ch;
    pthread_mutex_lock(&g_mu);
    if (g.closing) {   // 交给 close 的第①步统一丢弃，避免和它在同一 cmd 上重复销毁
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (!*pp) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ORDER, "cmd 已失效（abort / close 已接管）");
    }
    *pp = c->live_next;
    c->live_next = NULL;
    g.commit_active++;
    pthread_mutex_unlock(&g_mu);
    nvn_gpu_rc r = commit_impl(ch, signal, done, user, err);
    pthread_mutex_lock(&g_mu);
    g.commit_active--;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return r;
}

uint64_t nvn_gpu_fence_done(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g.completed;
    pthread_mutex_unlock(&g_mu);
    return v;
}

uint64_t nvn_gpu_fence_failed(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g.failed;
    pthread_mutex_unlock(&g_mu);
    return v;
}

nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t timeout_ns, nvn_gpu_err *err) {
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许等栅栏");
    if (v == 0) return fail(err, NVN_GPU_E_ARG, "v = 0 不是提交过的值");
    pthread_mutex_lock(&g_mu);
    if (g.completed >= v) {   // 已完成的旧值不再保留在提交集合里
        pthread_mutex_unlock(&g_mu);
        return rc(NVN_GPU_OK);
    }
    if (g.failed && v >= g.failed) {
        uint64_t f = g.failed;
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_FAILED, "时间线首个失败段 F=%llu", (unsigned long long)f);
    }
    if (!submitted_has(&g, v)) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ARG, "v=%llu 从未提交过", (unsigned long long)v);
    }
    if (timeout_ns == 0) {
        pthread_mutex_unlock(&g_mu);
        return rc(NVN_GPU_E_TIMEOUT);
    }
    struct timespec ts;
    deadline_after(&ts, timeout_ns);
    while (g.completed < v) {
        if (g.failed && v >= g.failed) {
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_FAILED, "时间线首个失败段 F=%llu", (unsigned long long)g.failed);
        }
        if (g.device_lost) {
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_DEVICE, "设备已丢失");
        }
        if (pthread_cond_timedwait(&g_cv, &g_mu, &ts) != 0) {
            if (g.completed < v) {
                pthread_mutex_unlock(&g_mu);
                return rc(NVN_GPU_E_TIMEOUT);
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 读数 / 测试钩子

void nvn_gpu_stats(nvn_gpu_stat_fn fn, void *user, int take) {
    if (!fn || !g_open) return;
    uint64_t v[STAT_N];
    pthread_mutex_lock(&g_mu);
    memcpy(v, g.stats, sizeof v);
    if (take) memset(g.stats, 0, sizeof g.stats);
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < STAT_N; i++) fn(user, stat_names[i], v[i]);
}

// 测试钩子：让某个 signal 的段在 fence 等待时报失败（走真实的失败终止路径）。
void nvn_gpu_vk_test_fail_fence(uint64_t signal) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.test_fail_signal = signal;
    pthread_mutex_unlock(&g_mu);
}

// 测试钩子：让某个 signal 的段先睡 ms，用来测 close 的 TIMEOUT 路径。
void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.test_delay_signal = signal;
    g.test_delay_ms = ms;
    pthread_mutex_unlock(&g_mu);
}

// 测试钩子（与 vk_clear 的 s2a_vk_clear_set_submit_fault 同一口径）：下一段 commit 按提交失败处理。
void nvn_gpu_vk_test_set_fault(int mode) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.fault_inject = mode;
    pthread_mutex_unlock(&g_mu);
}
