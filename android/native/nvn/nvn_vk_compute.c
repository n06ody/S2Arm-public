// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_vk_compute.h"
#include "nvn_vk_desc_write.h"
#include "nvn_vk_program_result.h"
#include "nvn_vk_mrt.h"
#include "nvn_vk_texcache.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
// bad: 事实行无条件调 __android_log_print。三种消费面各自成立：
//   Android：真头（__system_property_get 只在 __ANDROID__ 段用，system_properties.h 留门内）；
//   宿主自带缝 TU：w2 组件/整合测试前缀先 #define ANDROID_LOG_INFO 再给 __android_log_print
//     声明（w2_compute_pipeline_test.c 第 3-7 行）——已带常量则不引头，否则宏与桩头 enum 撞名；
//   宿主独立 TU：nvn_vk_real_teardown_test 链 hostvk 桩头——__has_include 命中才引。
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#elif !defined(ANDROID_LOG_INFO)
#ifdef __has_include
#if __has_include(<android/log.h>)
#include <android/log.h>
#endif
#endif
#endif
#include <stdio.h>
#include <inttypes.h>
#include <time.h>

#define COMPUTE_MAX_BYTES (64u * 1024u * 1024u)
/* 间接 storage 的两条宿主缝（来自 main 7afe4bfa1，合并时保留）：游戏写在 UBO 里的裸指针需要
 * GPU→CPU 地址映射与已注册池的长度查询；两者都是链接期可缺省，缺了对应用例准确拒、不换区间。 */
extern uint64_t nvn_ga2ca(uint64_t a) __attribute__((weak));
// 弱链接[androiddraw]：s2a_vk_adapter_pool_find（mtl_vk_adapter.c 提供；宿主单测不链适配器）
extern int s2a_vk_adapter_pool_find(uint64_t addr, uint64_t len, uint64_t *base, uint64_t *size) __attribute__((weak));
// dispatch-fq 收编的两条宿主缝（生产 nvn_vk_poolahb / nvn_vk_first_draw 提供）：
// lookup_usage 命中 = 该 guest 范围已迁进 AHB 后备，返回池 buffer + 池内偏移 + 后备 usage；
// submit_seq 返回本帧池保活序号（懒分配，dispatch 早于首笔 draw 也能拿到有效序号）。
// 任一缝缺席/序号为 0 都按 miss 处理，整笔退回 staging——不拿编造的句柄继续。
extern int s2a_poolahb_lookup_usage(uint64_t addr, uint64_t len, VkBuffer *buf,
                                    uint64_t *off, VkBufferUsageFlags *usage) __attribute__((weak));
extern uint64_t nvn_vk_first_draw_submit_seq(void) __attribute__((weak));

/* 上游 MaxUnknownStorageSize（Ryujinx Constants）：只能证明“UBO 里 8 字节裸指针”的 storage，
 * 绑已注册池的映射余量、封顶这个上界。截断不静默：每个 shader key 首次截断打一条结构化诊断。 */
#define NVK_COMPUTE_INDIRECT_MAX_BYTES 0x100000u
#define NVK_COMPUTE_INDIRECT_KEYS 64u
static struct { uint8_t key[8]; uint32_t hits; } g_indirect_trunc[NVK_COMPUTE_INDIRECT_KEYS];
static unsigned g_indirect_trunc_n;
static void indirect_trunc_diag(const struct s2a_record_dispatch_state *s,
    const struct nvn_vk_compute_storage *mapping, uint64_t extent) {
    for (unsigned i = 0; i < g_indirect_trunc_n; ++i) {
        if (!memcmp(g_indirect_trunc[i].key, s->keys[0], sizeof g_indirect_trunc[i].key)) {
            g_indirect_trunc[i].hits++;
            return;
        }
    }
    if (g_indirect_trunc_n < NVK_COMPUTE_INDIRECT_KEYS) {
        memcpy(g_indirect_trunc[g_indirect_trunc_n].key, s->keys[0],
            sizeof g_indirect_trunc[g_indirect_trunc_n].key);
        g_indirect_trunc[g_indirect_trunc_n].hits = 1;
        g_indirect_trunc_n++;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
        "[s2a-direct-diag] stage=cs why=indirect storage extent truncated "
        "key=%02x%02x%02x%02x%02x%02x%02x%02x mapping=%u extent=%llu bound=%u",
        s->keys[0][0], s->keys[0][1], s->keys[0][2], s->keys[0][3],
        s->keys[0][4], s->keys[0][5], s->keys[0][6], s->keys[0][7],
        (unsigned)mapping->mapping, (unsigned long long)extent, NVK_COMPUTE_INDIRECT_MAX_BYTES);
#else
    (void)s; (void)mapping; (void)extent;
#endif
}
// c1 常量段长度：图形侧同样按 0x100 字节绑定（nvn_vk_first_draw.c 的 c1 UBO）。
#define NVK_COMPUTE_C1_BYTES 0x100u
struct cpso;
struct compute_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    uint64_t guest, size;
    int writable, guest_backed, unbounded;
    int shared;              // 绑定到 dispatch 级“无界并集影子”而不是自己的影子
    int pooled;              // buffer 是借来的 AHB 池后备：不 allocate/上传/回写/销毁
    uint64_t window_off;     // shared 时在并集影子里的字节偏移；pooled 时是池后备内偏移
};
struct compute_owner {
    VkDevice device;
    VkShaderModule module;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout sets[4];
    /* 非 NULL = 上面四个对象归 PSO 缓存表所有（owner 持引用，destroy 只减引用不销毁）。
     * 空 = 按笔自有对象，destroy 逐个销毁（未缓存语义，与缓存前一致）。 */
    struct cpso *pso;
    VkDescriptorPool pool;
    struct compute_buffer buffers[FD_MAX_BINDINGS];
    // 这次 dispatch 在消费点拿全的纹理快照（含已 pin 的对象/视图）。成功构建时由 owner 接管，
    // 只在拿到完成证明（或证明性未提交）后释放；完成未知（-2）时保持不动。
    struct nvk_compute_tex_snapshot snapshot;
    // size=0（无界）storage 的 dispatch 级并集影子：重叠的无界绑定共用这一份字节，
    // GPU 写不会分叉、帧尾只回写一次（见 pipeline_build 里的说明）。
    struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; uint64_t base, size; int writable; } window;
    int count;
    struct s2a_record_memory guest;
};
static int error(char *why, size_t bytes, const char *message) {
    if (why && bytes) snprintf(why, bytes, "%s", message);
    return -1;
}
static VkDescriptorType binding_type(int kind) {
    switch (kind) {
        case 0: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case 1: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case 2: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        default: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}
/* ======== compute PSO / 程序级进程内缓存（各一道门，同一张表，不叠层；pso 默认开、prog 默认关） ========
 * 直连每笔 dispatch 都重建 shader module / set layouts / pipeline layout / pipeline，
 * 驱动对每次 vkCreateComputePipelines 重跑后端编译（8E 实测 nx-* 单核 58% 在 libllvm-qgl
 * + scudo 里）。
 * 两道门两种键，共用同一张有界表、同一套 refs/LRU/设备冲表：
 *   - debug.s2a.compute_pso_cache（PSO 级）：键 = SPIR-V 字节 + 描述符布局签名
 *     （set/binding/kind 三元组序列）+ 特化常量记号；命中比对是 hash 快筛 + 全字节
 *     memcmp——hash 不当身份。lookup/parse 仍逐笔跑，只省对象创建。
 *   - debug.s2a.compute_prog_cache（程序级，默认关）：键 = request.identity
 *     （canonical sha256——canonical 按构造覆盖翻译器全部输入：abi、closure、
 *     profile_hash、源 key/sha/字节、ubo_use、池长度、纹理事实，与 direct/program_cache.c
 *     磁盘缓存同信任域）。条目多存已解析的 nvn_vk_compute_program + profile，
 *     命中在桥上按 identity 直接短路：lookup（JSON 三遍+磁盘缓存读+封套+首遍反射）、
 *     parse（第二遍反射）、对象创建全省——真机 8E 实测 lookup 段 ~10-12ms/笔。
 *     同 identity ⇒ 同 REQUEST ⇒ 同输出程序；每笔数据（c0/c1/绑定快照/guest 字节）
 *     不进键、命中后仍按活状态逐笔填——与解析语义一致。
 *   prog 门开时不再叠 spirv 键层：spirv 键条目只在 pso 门独立开时产生/消费；
 *   by_identity 标志保证两种键域永不互配（门中途翻转只表现为逐出换血，不错配）。
 * 当前 stage 没有 pSpecializationInfo：将来给 stage 加 spec 常量必须并进键（canonical
 * 的 translator 输入面届时同样要扩）。
 * descriptor pool/set 与影子缓冲仍按笔 owner 管理：它们带每笔 dispatch 的数据。
 * 有界 LRU：表引用 + 在途 owner 引用计数（refs）。逐出只动 refs==1 的最旧项；满表且
 * 全部在途 → 该笔不入表、按 owner 自有对象收尾（未缓存语义），不超界、不装死。
 * 线程安全：表操作收在一把 test-and-set 自旋锁下；命中临界区是 hash+memcmp+句柄拷贝，
 * miss 的驱动对象创建放在锁外——不拿锁等编译。并发 miss 同键：先插者胜，后插者保留
 * 自己刚建的对象按笔收尾（内容等价，无正确性问题）。
 * 设备生命周期：nvn_vk_compute_pso_cache_release 由 session 在 device_destroy 前调
 * （幂等）；惰性守卫是 build/prog_lookup 发现设备句柄变了先冲表再入表。
 * 同一设备的 VkPipelineCache 只有一份（任一门开时懒建）：表内逐出的项，驱动 blob 仍可能复用。 */
#define CPSO_CAP 64u
struct cpso {
    uint8_t *spv, *sig;     /* pso 条目键：spirv 字节+布局签名；prog 条目：spv=spirv 副本、sig=NULL */
    size_t spv_len, sig_len;
    uint64_t h;
    int by_identity;                          /* 1 = 程序级条目（identity 键 + 已解析程序） */
    uint8_t identity[32];
    struct nvn_vk_compute_program *prog;      /* by_identity：malloc 深拷贝，stage.spirv 重指 e->spv */
    struct nvk_compute_profile_facts profile;
    VkDevice dev;
    VkShaderModule module;
    VkDescriptorSetLayout sets[4];
    uint32_t set_count;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    int refs;          /* 表引用 1 + 每个在途 owner 1；0 = 销毁 */
    int in_table;
    uint64_t tick;
};
static _Atomic int g_cpso_gate = -1;   // 宿主替身可直接复位（与 g_indirect_trunc_n 同缝口径）
/* vkCreateComputePipelines 调用次数/耗时的进程累计（与缓存门无关：A/B 两边都如实计，
 * 逐帧 TSV 的 cpso_n/cpso_us 差分取数；计时只发生在创建路径，命中路径零开销）。 */
static _Atomic uint64_t g_cpso_creates, g_cpso_create_ns;
void nvn_vk_compute_cpso_totals(uint64_t *creates, uint64_t *ns) {
    if (creates) *creates = atomic_load(&g_cpso_creates);
    if (ns) *ns = atomic_load(&g_cpso_create_ns);
}
/* 程序级命中/未中进程累计（门无关：=0 时永远 0；宿主断言与 A/B 读数用）。 */
static _Atomic uint64_t g_prog_hits, g_prog_miss;
void nvn_vk_compute_prog_totals(uint64_t *hits, uint64_t *misses) {
    if (hits) *hits = atomic_load(&g_prog_hits);
    if (misses) *misses = atomic_load(&g_prog_miss);
}
static _Atomic int g_cpso_lock;
static struct cpso g_cpso_tab[CPSO_CAP];
static unsigned g_cpso_n;
static uint64_t g_cpso_tick;
static VkDevice g_cpso_dev;
static VkPipelineCache g_cpso_pipe;
static int cpso_on(void) {
    int v = atomic_load(&g_cpso_gate);
    if (v < 0) {
#ifdef __ANDROID__
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.compute_pso_cache", b);
        v = (b[0] == '0') ? 0 : 1;   // 默认开（8E A/B 3.854→13.567 fps 中位，#473）；设 0 关
#else
        const char *e = getenv("S2A_COMPUTE_PSO_CACHE");
        v = (e && e[0] == '0') ? 0 : 1;
#endif
        atomic_store(&g_cpso_gate, v);
    }
    return v;
}
static _Atomic int g_prog_gate = -1;   // 宿主替身可直接复位（与 g_cpso_gate 同缝口径）
static int prog_cache_on(void) {
    int v = atomic_load(&g_prog_gate);
    if (v < 0) {
#ifdef __ANDROID__
        char b[PROP_VALUE_MAX] = {0};
        v = __system_property_get("debug.s2a.compute_prog_cache", b) > 0 && b[0] == '1';
#else
        const char *e = getenv("S2A_COMPUTE_PROG_CACHE");
        v = e && e[0] == '1';
#endif
        atomic_store(&g_prog_gate, v);
    }
    return v;
}
static void cpso_lock(void) { while (atomic_exchange_explicit(&g_cpso_lock, 1, memory_order_acquire)) {} }
static void cpso_unlock(void) { atomic_store_explicit(&g_cpso_lock, 0, memory_order_release); }
static uint64_t cpso_hash(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static void cpso_objects_destroy(struct cpso *e) {
    if (e->pipeline) vkDestroyPipeline(e->dev, e->pipeline, NULL);
    if (e->layout) vkDestroyPipelineLayout(e->dev, e->layout, NULL);
    for (uint32_t i = 0; i < e->set_count; ++i)
        if (e->sets[i]) vkDestroyDescriptorSetLayout(e->dev, e->sets[i], NULL);
    if (e->module) vkDestroyShaderModule(e->dev, e->module, NULL);
    e->pipeline = VK_NULL_HANDLE; e->layout = VK_NULL_HANDLE; e->module = VK_NULL_HANDLE;
    memset(e->sets, 0, sizeof e->sets);
}
/* 减一个引用（表/owner 通用），归零销毁对象并释放键副本。持锁调用。 */
static void cpso_release_locked(struct cpso *e) {
    if (--e->refs == 0) {
        cpso_objects_destroy(e);
        free(e->spv); free(e->sig); free(e->prog);
        e->spv = e->sig = NULL; e->prog = NULL;
    }
}
/* 锁外一处用：减一个引用（owner 归还/早退路径）。 */
static void cpso_unref(struct cpso *e) {
    if (!e) return;
    cpso_lock(); cpso_release_locked(e); cpso_unlock();
}
/* 命中：entry 持 owner 引用返回（refs 已 +1）；miss 返回 NULL。持锁调用。 */
static struct cpso *cpso_lookup_locked(uint64_t h, const uint8_t *spv, size_t spv_len,
                                       const uint8_t *sig, size_t sig_len) {
    for (unsigned i = 0; i < CPSO_CAP; ++i) {
        struct cpso *e = &g_cpso_tab[i];
        if (!e->in_table || e->by_identity || e->h != h || e->spv_len != spv_len || e->sig_len != sig_len) continue;
        if (memcmp(e->spv, spv, spv_len) || memcmp(e->sig, sig, sig_len)) continue;
        e->refs++; e->tick = ++g_cpso_tick;
        return e;
    }
    return NULL;
}
/* 程序级命中：identity 全字节相等才中（hash 不参政）。持锁调用；命中 refs 已 +1。 */
static struct cpso *cprog_lookup_locked(const uint8_t identity[32]) {
    for (unsigned i = 0; i < CPSO_CAP; ++i) {
        struct cpso *e = &g_cpso_tab[i];
        if (!e->in_table || !e->by_identity || memcmp(e->identity, identity, 32)) continue;
        e->refs++; e->tick = ++g_cpso_tick;
        return e;
    }
    return NULL;
}
static int cpso_slot_locked(void) {
    for (unsigned i = 0; i < CPSO_CAP; ++i)
        if (!g_cpso_tab[i].in_table && !g_cpso_tab[i].refs) return (int)i;
    return -1;
}
/* 逐出 refs==1 的最旧项（无在途 dispatch 引用）。返回 0 = 腾出空位。持锁调用。 */
static int cpso_evict_locked(void) {
    struct cpso *v = NULL;
    for (unsigned i = 0; i < CPSO_CAP; ++i) {
        struct cpso *e = &g_cpso_tab[i];
        if (e->in_table && e->refs == 1 && (!v || e->tick < v->tick)) v = e;
    }
    if (!v) return -1;
    v->in_table = 0; g_cpso_n--;
    cpso_release_locked(v);
    return 0;
}
/* 入表：键副本 + 对象句柄进表，refs=2（表引用 + 本笔 owner 引用）。同键已在表 /
 * 无空位且无可逐出项 / 内存失败 → NULL（调用方对象保持按笔自有）。持锁调用。 */
static struct cpso *cpso_insert_locked(VkDevice dev, uint64_t h,
    const uint8_t *spv, size_t spv_len, const uint8_t *sig, size_t sig_len,
    VkShaderModule module, const VkDescriptorSetLayout *sets, uint32_t set_count,
    VkPipelineLayout layout, VkPipeline pipeline) {
    for (unsigned i = 0; i < CPSO_CAP; ++i) {
        struct cpso *e = &g_cpso_tab[i];
        if (e->in_table && !e->by_identity && e->h == h && e->spv_len == spv_len && e->sig_len == sig_len &&
            !memcmp(e->spv, spv, spv_len) && !memcmp(e->sig, sig, sig_len)) return NULL;
    }
    int slot = cpso_slot_locked();
    if (slot < 0 && (cpso_evict_locked() || (slot = cpso_slot_locked()) < 0)) return NULL;
    struct cpso *e = &g_cpso_tab[slot];
    e->spv = malloc(spv_len); e->sig = malloc(sig_len);
    if (!e->spv || !e->sig) { free(e->spv); free(e->sig); e->spv = e->sig = NULL; return NULL; }
    memcpy(e->spv, spv, spv_len); memcpy(e->sig, sig, sig_len);
    e->spv_len = spv_len; e->sig_len = sig_len; e->h = h; e->dev = dev;
    e->module = module; memcpy(e->sets, sets, set_count * sizeof e->sets[0]); e->set_count = set_count;
    e->layout = layout; e->pipeline = pipeline;
    e->refs = 2; e->in_table = 1; e->tick = ++g_cpso_tick; g_cpso_n++;
    return e;
}
/* 程序级入表：键=identity，随存已解析程序深拷贝 + profile（spirv 副本放 e->spv）。
 * 同 identity 已在表 / 无空位且无可逐出项 / 内存失败 → NULL（调用方对象保持按笔自有）。
 * 持锁调用；入表成功 refs=2（表 + 本笔 owner）。 */
static struct cpso *cprog_insert_locked(VkDevice dev, const uint8_t identity[32],
    const struct nvn_vk_compute_program *prog, const struct nvk_compute_profile_facts *profile,
    VkShaderModule module, const VkDescriptorSetLayout *sets, uint32_t set_count,
    VkPipelineLayout layout, VkPipeline pipeline) {
    for (unsigned i = 0; i < CPSO_CAP; ++i) {
        struct cpso *e = &g_cpso_tab[i];
        if (e->in_table && e->by_identity && !memcmp(e->identity, identity, 32)) return NULL;
    }
    int slot = cpso_slot_locked();
    if (slot < 0 && (cpso_evict_locked() || (slot = cpso_slot_locked()) < 0)) return NULL;
    struct cpso *e = &g_cpso_tab[slot];
    e->spv = malloc(prog->stage.spirv_bytes);
    e->prog = malloc(sizeof *e->prog);
    if (!e->spv || !e->prog) { free(e->spv); free(e->prog); e->spv = NULL; e->prog = NULL; return NULL; }
    memcpy(e->spv, prog->stage.spirv, prog->stage.spirv_bytes);
    *e->prog = *prog;
    e->prog->stage.spirv = e->spv;
    e->spv_len = prog->stage.spirv_bytes; e->sig = NULL; e->sig_len = 0; e->h = 0;
    e->by_identity = 1; memcpy(e->identity, identity, 32); e->profile = *profile; e->dev = dev;
    e->module = module; memcpy(e->sets, sets, set_count * sizeof e->sets[0]); e->set_count = set_count;
    e->layout = layout; e->pipeline = pipeline;
    e->refs = 2; e->in_table = 1; e->tick = ++g_cpso_tick; g_cpso_n++;
    return e;
}
/* 设备句柄切换：整表冲掉 + pipeline cache 销毁（在途 owner 引用仍保活其对象）。
 * 持锁调用；new_dev 作为下一笔入表的设备。 */
static void cpso_flush_locked(VkDevice new_dev) {
    for (unsigned i = 0; i < CPSO_CAP; ++i)
        if (g_cpso_tab[i].in_table) {
            g_cpso_tab[i].in_table = 0;
            cpso_release_locked(&g_cpso_tab[i]);
        }
    g_cpso_n = 0;
    if (g_cpso_pipe) {
        vkDestroyPipelineCache(g_cpso_dev, g_cpso_pipe, NULL);
        g_cpso_pipe = VK_NULL_HANDLE;
    }
    g_cpso_dev = new_dev;
}
/* 持锁调用：设备级 pipeline cache 懒建（任一门开都要）；建不起来返回 NULL（不致命，
 * 本层表仍工作——与 pso 路径同口径）。顺带设备切换冲表。 */
static VkPipelineCache cpso_plc_locked(VkDevice dev) {
    if (g_cpso_dev != dev) cpso_flush_locked(dev);
    if (!g_cpso_pipe) {
        VkPipelineCacheCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        if (vkCreatePipelineCache(dev, &pc, NULL, &g_cpso_pipe) != VK_SUCCESS)
            g_cpso_pipe = VK_NULL_HANDLE;
    }
    return g_cpso_pipe;
}
/* 程序级查表出口（桥在 PREPARE 后调）：门开 + identity 命中 → 持 owner 引用的条目；
 * 门关/未中 → NULL。命中计入 g_prog_hits、未中计 g_prog_miss（门开时才计）。 */
struct cpso *nvn_vk_compute_prog_lookup(struct s2a_vk_device *d, const uint8_t identity[32]) {
    if (!prog_cache_on() || !d || !d->device || !identity) return NULL;
    cpso_lock();
    if (g_cpso_dev != d->device) cpso_flush_locked(d->device);
    struct cpso *e = cprog_lookup_locked(identity);
    if (e) atomic_fetch_add_explicit(&g_prog_hits, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&g_prog_miss, 1, memory_order_relaxed);
    cpso_unlock();
    return e;
}
void nvn_vk_compute_prog_release(struct cpso *entry) { cpso_unref(entry); }
/* session 在 device_destroy 前调（幂等）：释放该设备在表的全部对象与 pipeline cache。 */
void nvn_vk_compute_pso_cache_release(struct s2a_vk_device *d) {
    if (!d || !d->device) return;
    cpso_lock();
    for (unsigned i = 0; i < CPSO_CAP; ++i)
        if (g_cpso_tab[i].in_table && g_cpso_tab[i].dev == d->device) {
            g_cpso_tab[i].in_table = 0; g_cpso_n--;
            cpso_release_locked(&g_cpso_tab[i]);
        }
    if (g_cpso_pipe && g_cpso_dev == d->device) {
        vkDestroyPipelineCache(d->device, g_cpso_pipe, NULL);
        g_cpso_pipe = VK_NULL_HANDLE;
    }
    if (g_cpso_dev == d->device) g_cpso_dev = VK_NULL_HANDLE;
    cpso_unlock();
}
static void destroy(struct compute_owner *o) {
    if (!o) return;
    if (o->pso) {
        /* pipeline/layout/sets/module 归 PSO 缓存表：这里只减本笔 owner 引用。 */
        cpso_lock();
        cpso_release_locked(o->pso);
        cpso_unlock();
    } else {
        if (o->pipeline) vkDestroyPipeline(o->device, o->pipeline, NULL);
        if (o->layout) vkDestroyPipelineLayout(o->device, o->layout, NULL);
        for (int i = 0; i < 4; ++i)
            if (o->sets[i]) vkDestroyDescriptorSetLayout(o->device, o->sets[i], NULL);
        if (o->module) vkDestroyShaderModule(o->device, o->module, NULL);
    }
    if (o->pool) vkDestroyDescriptorPool(o->device, o->pool, NULL);
    // 纹理保活只在命令完成/证明性的未提交之后解除；完成未知（-2）时 destroy 不会被调用。
    nvk_compute_texture_snapshot_release(&o->snapshot);
    for (int i = 0; i < o->count; ++i) {
        struct compute_buffer *b = &o->buffers[i];
        // shared 句柄属于 o->window 统一释放；pooled 句柄是借来的 AHB 池后备，归池账本，不能碰。
        if (b->shared || b->pooled) continue;
        if (b->mapped) vkUnmapMemory(o->device, b->memory);
        if (b->buffer) vkDestroyBuffer(o->device, b->buffer, NULL);
        if (b->memory) vkFreeMemory(o->device, b->memory, NULL);
    }
    if (o->window.mapped) vkUnmapMemory(o->device, o->window.memory);
    if (o->window.buffer) vkDestroyBuffer(o->device, o->window.buffer, NULL);
    if (o->window.memory) vkFreeMemory(o->device, o->window.memory, NULL);
    free(o);
}
static int finish(struct s2a_vk_device *d, void *owner, int completion) {
    struct compute_owner *o = owner;
    if (!o || !d || d->device != o->device) return -1;
    if (completion == -2) return 0; /* No completion proof: preserve all handles. */
    if (completion != 0 && completion != -1) return -1;
    int rc = 0;
    if (completion == 0) {
        for (int i = 0; i < o->count; ++i) {
            struct compute_buffer *b = &o->buffers[i];
            // shared 归并集影子统一回写；pooled 目前只允许只读绑定，无需回写。
            if (b->shared || b->pooled || !b->writable) continue;
            if (o->guest.write(o->guest.opaque, b->guest, b->mapped, (size_t)b->size)) rc = -1;
        }
        {
            // 诊断 A/B 门：跳过无界并集影子的回写（默认写）。开着时 GPU 写结果不回 guest，
            // 只用于判断“崩溃是否由整段回写覆盖相邻 guest 字节引起”，不能当功能用。
            static _Atomic int no_wb = -1;
            if (no_wb < 0) {
                int v = 0;
#ifdef __ANDROID__
                char b[PROP_VALUE_MAX] = {0};
                v = __system_property_get("debug.s2a.compute_no_writeback", b) > 0 && b[0] == '1';
#endif
                no_wb = v;
            }
            if (o->window.writable && o->window.mapped && !no_wb &&
                o->guest.write(o->guest.opaque, o->window.base, o->window.mapped, (size_t)o->window.size)) rc = -1;
        }
    }
    destroy(o);
    return rc;
}
static int allocate(struct s2a_vk_device *d, struct compute_buffer *b, VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = b->size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(d->device, &ci, NULL, &b->buffer) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(d->device, b->buffer, &mr);
    uint32_t type = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (type == UINT32_MAX) return -1;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = type };
    if (vkAllocateMemory(d->device, &ai, NULL, &b->memory) != VK_SUCCESS ||
        vkBindBufferMemory(d->device, b->buffer, b->memory, 0) != VK_SUCCESS ||
        vkMapMemory(d->device, b->memory, 0, b->size, 0, &b->mapped) != VK_SUCCESS) return -1;
    return 0;
}
/* resolved != NULL = 程序级命中（接管条目的 owner 引用，任何早退经 destroy/o->pso 归还）；
 * identity != NULL = 本笔 request.identity（prog 门开时 miss 路径用于入表）。 */
static int pipeline_build_impl(struct s2a_vk_device *d,
    const struct s2a_record_dispatch_state *s, const struct s2a_record_memory *memory,
    struct nvk_compute_tex_snapshot *snapshot,
    const struct s2a_source_entry_view *source,
    const struct nvk_compute_lookup_result *program,
    struct cpso *resolved, const uint8_t identity[32],
    struct s2a_record_dispatch_output *out, char *why, size_t why_bytes) {
    if (!out) { cpso_unref(resolved); return error(why, why_bytes, "compute output is missing"); }
    memset(out, 0, sizeof *out);
    if (why && why_bytes) why[0] = 0;
    if (!d || !d->device || !d->physical || !s || s->stages != 32 || !memory || !memory->read || !memory->write) {
        cpso_unref(resolved);
        return error(why, why_bytes, "compute device/state/memory is missing");
    }
    if (resolved) {
        if (!resolved->by_identity || !resolved->prog || !resolved->prog->stage.spirv) {
            cpso_unref(resolved);
            return error(why, why_bytes, "compute prog cache entry malformed");
        }
    } else if (!program || !program->bytes || !program->length)
        return error(why, why_bytes, "compute accepted lookup result is missing");
    uint64_t profile_storage_alignment = resolved ? resolved->profile.storage_alignment
                                                  : program->profile.storage_alignment;
    struct nvn_vk_compute_program p;
    if (resolved) {
        p = *resolved->prog;   /* 内嵌数组全随结构拷贝；spirv 需每笔独立副本供 free_compute 释放 */
        p.stage.spirv = malloc(resolved->prog->stage.spirv_bytes);
        if (!p.stage.spirv) {
            cpso_unref(resolved);
            return error(why, why_bytes, "compute prog cache spirv copy failed");
        }
        memcpy(p.stage.spirv, resolved->prog->stage.spirv, resolved->prog->stage.spirv_bytes);
    } else if (nvn_vk_program_parse_compute(program->bytes, program->length, &p, why, why_bytes)) return -1;
    /* An unused declaration is not a runtime descriptor requirement. Remove it
     * from every subsequent allocation/layout/write path, preserving other indices. */
    if (!p.support_read_count && !p.support_reads_unknown) {
        for (int i = 0; i < p.binding_count;) {
            if (!p.bindings[i].is_support) { ++i; continue; }
            --p.binding_count;
            memmove(&p.bindings[i], &p.bindings[i + 1], (size_t)(p.binding_count - i) * sizeof p.bindings[0]);
            memmove(&p.storage[i], &p.storage[i + 1], (size_t)(p.binding_count - i) * sizeof p.storage[0]);
        }
    }
    struct compute_owner *o = calloc(1, sizeof *o);
    if (!o) {
        nvn_vk_program_free_compute(&p);
        cpso_unref(resolved);
        return error(why, why_bytes, "compute owner allocation failed");
    }
    o->device = d->device; o->guest = *memory;
    if (resolved) {
        /* 条目对象直接接管：module/layout/pipeline/sets 归表，o->pso 记引用——
         * 之后一切失败经 destroy() 归还引用（对象不在这层销毁）。 */
        o->module = resolved->module; o->layout = resolved->layout; o->pipeline = resolved->pipeline;
        for (uint32_t i = 0; i < resolved->set_count; ++i) o->sets[i] = resolved->sets[i];
        o->pso = resolved;
    }
    const char *failure = "compute Vulkan object creation failed";
    char detail[256];
    uint8_t c0[NVK_COMPUTE_C0_BYTES];
    /* Unread bytes are not facts: only reflected, validated ranges are filled. */
    memset(c0, 0, sizeof c0);
    if (p.c0_reads_unknown) {
        /* 编译器给不出 c0 读集：按已测量驱动布局填满。每个值都来自本次 dispatch 的真实状态：
         *   0x200 = workgroup 数 (x,y,z)；0x210+16r = 用户 UBO 表 {u64 addr,u32 size}；
         *   0x310+16r = storage buffer 表 {u64 addr,u32 size}（0x410 之前结束）。
         * 未绑定的槽保持 0（硬件对空槽的描述符也是 0）。 */
        for (unsigned k = 0; k < 3; ++k)
            for (unsigned byte = 0; byte < 4; ++byte)
                c0[0x200 + k * 4 + byte] = (uint8_t)(s->groups[k] >> (byte * 8));
        for (unsigned r = 0; r < 16; ++r) {
            uint64_t a = s->ubo[r].addr;
            uint32_t n = (uint32_t)s->ubo[r].size;
            memcpy(c0 + 0x210 + 16 * r, &a, 8);
            memcpy(c0 + 0x210 + 16 * r + 8, &n, 4);
            a = s->ssbo[r].addr;
            n = (uint32_t)s->ssbo[r].size;
            memcpy(c0 + 0x310 + 16 * r, &a, 8);
            memcpy(c0 + 0x310 + 16 * r + 8, &n, 4);
        }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(d->physical, &props);
    if (!profile_storage_alignment || profile_storage_alignment > 65536 ||
        (profile_storage_alignment & (profile_storage_alignment - 1)) ||
        profile_storage_alignment != props.limits.minStorageBufferOffsetAlignment) {
        snprintf(detail, sizeof detail, "compute profile alignment mismatch: profile=%" PRIu64 " device=%" PRIu64,
            profile_storage_alignment, (uint64_t)props.limits.minStorageBufferOffsetAlignment);
        failure = detail; goto bad;
    }
    for (uint32_t i = 0; i < (p.c0_reads_unknown ? 0u : p.c0_read_count); ++i) {
        const struct nvn_vk_c0_read *r = &p.c0_reads[i];
        uint32_t base = r->offset < 0x210 ? 0x200 : r->offset & ~15u;
        uint8_t value[12];
        if (base == 0x200) {
            for (unsigned k = 0; k < 3; ++k)
                for (unsigned byte = 0; byte < 4; ++byte)
                    value[k * 4 + byte] = (uint8_t)(s->groups[k] >> (byte * 8));
        } else {
            int ubo = base < 0x310;
            unsigned slot = (base - (ubo ? 0x210u : 0x310u)) / 16u;
            uint32_t seen = ubo ? s->ubo_seen : s->ssbo_seen;
            if (!(seen & (1u << slot))) {
                snprintf(detail, sizeof detail, "compute c0 snapshot unknown: offset=0x%x", r->offset);
                failure = detail; goto bad;
            }
            uint64_t addr = ubo ? s->ubo[slot].addr : s->ssbo[slot].addr;
            uint64_t size = ubo ? s->ubo[slot].size : s->ssbo[slot].size;
            if (size > UINT32_MAX) { failure = "compute c0 buffer size exceeds u32"; goto bad; }
            for (unsigned byte = 0; byte < 8; ++byte) value[byte] = (uint8_t)(addr >> (byte * 8));
            for (unsigned byte = 0; byte < 4; ++byte) value[8 + byte] = (uint8_t)(size >> (byte * 8));
        }
        memcpy(c0 + r->offset, value + r->offset - base, r->size);
    }

    for (int k = 0; k < 3; ++k) if (s->groups[k] > props.limits.maxComputeWorkGroupCount[k]) {
        failure = "compute dispatch exceeds device limits"; goto bad;
    }
    /* Validate every binding and alias before creating any Vulkan object. */
    uint32_t set_count = 0, total = 0, uniforms = 0, storage = 0, sampled = 0;
    for (int i = 0; i < p.binding_count; ++i) {
        const struct fd_binding *r = &p.bindings[i];
        struct compute_buffer *b = &o->buffers[i];
        o->count = i + 1;
        if (r->set < 0 || r->set >= 4 || (r->kind != 0 && r->kind != 1 && r->kind != 2)) {
            snprintf(detail, sizeof detail,
                "compute descriptor unsupported: set=%d binding=%d kind=%s handleIndex=%d", r->set, r->binding,
                r->kind == 2 ? "texture" : r->kind == 3 ? "image" : "unknown", r->slot);
            failure = detail; goto bad;
        }
        if (r->kind == 0) uniforms++;
        else if (r->kind == 1) storage++;
        else sampled++;
        if ((uint32_t)r->set + 1 > set_count) set_count = (uint32_t)r->set + 1;
        if (r->kind == 2) {
            // 纹理不占 guest 缓冲/大小/别名预算：反射来的 handleIndex 必须对应消费点快照里的条目。
            // 反射说有、快照没有（该槽此刻没绑定）→ 明确失败，不绑占位纹理、不跨槽借事实。
            if (r->tex_slot < 0 || r->tex_slot >= 32) {
                snprintf(detail, sizeof detail,
                    "compute texture handleIndex %d is outside the combined slots: set=%d binding=%d",
                    r->slot, r->set, r->binding);
                failure = detail; goto bad;
            }
            if (!nvk_compute_texture_snapshot_find(snapshot, (unsigned)r->tex_slot)) {
                snprintf(detail, sizeof detail,
                    "compute texture slot %d (handleIndex %d) is not in the dispatch snapshot: set=%d binding=%d",
                    r->tex_slot, r->slot, r->set, r->binding);
                failure = detail; goto bad;
            }
            continue;
        }
        if (r->is_support) {
            b->size = p.support_bytes;
        } else if (r->kind == 0 && r->slot == 0) {
            b->size = sizeof c0;
        } else if (r->kind == 0 && r->slot == 1) {
            // cbuf 1 = c1 常量段（与图形侧 nvn_vk_first_draw.c 同一语义）：内容取本次 dispatch
            // 消费点快照里的 source->c1。没捕获或大小不够就报红——不绑零缓冲、不回查 guest。
            if (!source || !source->c1 || source->c1_bytes < NVK_COMPUTE_C1_BYTES) {
                snprintf(detail, sizeof detail,
                    "compute c1 constant segment is not captured: stage=cs cbuf=1 set=%d binding=%d c1_bytes=%u",
                    r->set, r->binding, source ? source->c1_bytes : 0u);
                failure = detail; goto bad;
            }
            b->size = NVK_COMPUTE_C1_BYTES;
        } else if (r->kind == 0 && r->slot == 2) {
            snprintf(detail, sizeof detail,
                "compute c2 texture-handle bank used as a uniform buffer is not implemented: set=%d binding=%d",
                r->set, r->binding);
            failure = detail; goto bad;
        } else {
            int slot = r->ubo_slot;
            int use_ubo = r->kind == 0;
            int indirect = 0;
            const struct nvn_vk_compute_storage *mapping = r->kind == 1 ? &p.storage[i] : NULL;
            if (r->kind == 1) {
                if (mapping->mapping == NVK_STORAGE_NVN) {
                    slot = (int)mapping->index; use_ubo = 0;
                } else if (mapping->mapping == NVK_STORAGE_UBO_PTR) {
                    // 描述符来自驱动 c0 的 UBO 表（0x210 + 16*r 是 {u64 addr, u32 size}）：这张表
                    // 存的就是该用户 UBO 的描述符，因此这条 storage 绑定别名“槽 r 的 UBO 本体”。
                    // 运行时的 c0 合成与这里用同一份 ubo[slot] 事实，shader 自己读 c0 也不会分叉。
                    slot = (int)mapping->index; use_ubo = 1;
                } else if (mapping->mapping == NVK_STORAGE_UBO_ADDRESS) {
                    indirect = 1;   // 用户 UBO 内 8 字节裸指针：读指针再查已注册池余量
                } else {
                    failure = "compute storage source is unsupported"; goto bad;
                }
            }
            if (indirect && s->fq_pool) goto retry_sync;
            if (indirect) {
                /* UBO_ADDRESS：绑定的用户 UBO 在 mapping->offset 处只存一个 8 字节 guest 裸指针。
                 * 上游不信指针旁边那个长度字（Ryujinx ComputeClass 读指针后用
                 * MemoryManager.GetMappedSize 求长度，封顶 MaxUnknownStorageSize；Metal 前端同口径
                 * 走池表）。这里的等价事实是“已注册池的映射余量”，封顶 1 MiB 且截断必打诊断。 */
                if (mapping->index >= 16 || !(s->ubo_seen & (1u << mapping->index))) {
                    failure = "compute indirect storage source UBO is not bound"; goto bad;
                }
                const uint64_t ubo_addr = s->ubo[mapping->index].addr;
                const uint64_t ubo_size = s->ubo[mapping->index].size;
                if (!ubo_addr || (uint64_t)mapping->offset > ubo_size ||
                    ubo_size - (uint64_t)mapping->offset < 8) {
                    snprintf(detail, sizeof detail,
                        "compute indirect storage source is outside the bound user UBO (slot=%u off=0x%x)",
                        mapping->index, mapping->offset);
                    failure = detail; goto bad;
                }
                uint64_t target = 0;
                if (memory->read(memory->opaque, ubo_addr + mapping->offset, &target, 8)) {
                    failure = "compute indirect storage source is unreadable"; goto bad;
                }
                if (nvn_ga2ca) target = nvn_ga2ca(target);
                if (!s2a_vk_adapter_pool_find) {
                    failure = "compute indirect storage pool lookup is unavailable"; goto bad;
                }
                uint64_t pbase = 0, psize = 0;
                if (s2a_vk_adapter_pool_find(target, 1, &pbase, &psize)) {
                    failure = "compute indirect storage target is outside the registered pools"; goto bad;
                }
                uint64_t extent = psize - (target - pbase);
                if (extent > NVK_COMPUTE_INDIRECT_MAX_BYTES) {
                    indirect_trunc_diag(s, mapping, extent);
                    extent = NVK_COMPUTE_INDIRECT_MAX_BYTES;
                }
                b->guest = target;
                b->size = extent;
            } else {
            uint32_t seen = use_ubo ? s->ubo_seen : s->ssbo_seen;
            if (slot < 0 || slot >= 16 || !(seen & (1u << slot))) {
                snprintf(detail, sizeof detail,
                    "compute guest slot is unknown: set=%d binding=%d kind=%d slot=%d use_ubo=%d ubo_seen=0x%x ssbo_seen=0x%x",
                    r->set, r->binding, r->kind, slot, use_ubo, s->ubo_seen, s->ssbo_seen);
                failure = detail; goto bad;
            }
            b->guest = use_ubo ? s->ubo[slot].addr : s->ssbo[slot].addr;
            b->size = use_ubo ? s->ubo[slot].size : s->ssbo[slot].size;
            if (b->guest && !b->size) {
                b->unbounded = 1;
                // NVN storage 绑定 size=0（长度未声明）：先用“地址所在活 nvnBuffer 的精确长度”，
                // 查不到再退回池尾近似（只读绑定可接受；可写绑定必须精确，否则整段回写会覆盖相邻分配）。
                int exact = 0;
                uint64_t extent = nvn_vk_guest_buffer_extent(b->guest, 4u << 20, &exact);
                if (!extent) {
                    snprintf(detail, sizeof detail,
                        "compute storage size is 0 and no provable guest extent: set=%d binding=%d addr=0x%llx",
                        r->set, r->binding, (unsigned long long)b->guest);
                    failure = detail; goto bad;
                }
                if (!exact) {
                    if (r->kind == 1 && p.storage[i].writable) {
                        snprintf(detail, sizeof detail,
                            "writable compute storage has size 0 and only a pool-tail extent (not the buffer): "
                            "set=%d binding=%d addr=0x%llx", r->set, r->binding, (unsigned long long)b->guest);
                        failure = detail; goto bad;
                    }
                    if (extent > (64u << 10)) extent = 64u << 10;   // 近似窗口只在只读上使用，夹小
                }
                b->size = extent;
#ifdef __ANDROID__
                {
                    static _Atomic int noted_zero;
                    if (atomic_fetch_add(&noted_zero, 1) < 8)
                        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                            "compute storage size=0: using %s extent %llu B",
                            exact ? "exact buffer" : "pool-tail (approximate)", (unsigned long long)extent);
                }
#endif
            }
            }
            b->guest_backed = 1;
            b->writable = r->kind == 1 && p.storage[i].writable;
            if (!b->guest || !b->size || b->guest > UINT64_MAX - b->size) {
                snprintf(detail, sizeof detail,
                    "compute guest buffer range is invalid: set=%d binding=%d kind=%d slot=%d use_ubo=%d "
                    "guest=0x%llx size=%llu | ubo[%d]=0x%llx/%llu ssbo[%d]=0x%llx/%llu seen=0x%x/0x%x",
                    r->set, r->binding, r->kind, slot, use_ubo,
                    (unsigned long long)b->guest, (unsigned long long)b->size,
                    slot, (unsigned long long)s->ubo[slot].addr, (unsigned long long)s->ubo[slot].size,
                    slot, (unsigned long long)s->ssbo[slot].addr, (unsigned long long)s->ssbo[slot].size,
                    s->ubo_seen, s->ssbo_seen);
                failure = detail; goto bad;
            }
            if (r->kind == 1 && p.has_unaligned_storage_buffer) {
                uint64_t prefix = b->guest & (profile_storage_alignment - 1);
                b->guest -= prefix;
                b->size += prefix; /* end was already checked against UINT64_MAX */
            } else {
                uint64_t align = r->kind == 0 ? props.limits.minUniformBufferOffsetAlignment
                                            : profile_storage_alignment;
                if (!align || b->guest % align) {
                    failure = "unaligned compute guest buffers require offset mapping"; goto bad;
                }
            }
        }
        uint64_t limit = r->kind == 0 ? props.limits.maxUniformBufferRange : props.limits.maxStorageBufferRange;
        if (!b->size || b->size > limit || b->size > COMPUTE_MAX_BYTES || total > COMPUTE_MAX_BYTES - b->size) {
            failure = "compute buffer size exceeds limit"; goto bad;
        }
        total += (uint32_t)b->size;
        for (int j = 0; j < i; ++j) {
            const struct compute_buffer *a = &o->buffers[j];
            if (a->unbounded != b->unbounded && a->guest_backed && b->guest_backed &&
                (a->writable || b->writable) && a->guest < b->guest + b->size && b->guest < a->guest + a->size) {
                // 无界（size=0）与有明确 size 的可写绑定重叠：前者进并集影子、后者是独立影子，
                // GPU 写会分叉、回写互相覆盖。准确拒绝，等两条都走同一口径再放开。
                snprintf(detail, sizeof detail,
                    "overlapping writable storage windows with different provenance: "
                    "[0x%llx,+%llu) [0x%llx,+%llu)",
                    (unsigned long long)a->guest, (unsigned long long)a->size,
                    (unsigned long long)b->guest, (unsigned long long)b->size);
                failure = detail; goto bad;
            }
            if (a->guest_backed && b->guest_backed && (a->writable || b->writable) &&
                !a->unbounded && !b->unbounded &&
                a->guest < b->guest + b->size && b->guest < a->guest + a->size) {
                snprintf(detail, sizeof detail, "overlapping compute writable windows: [0x%" PRIx64 ",0x%" PRIx64 ") [0x%" PRIx64 ",0x%" PRIx64 ")",
                    a->guest, a->guest + a->size, b->guest, b->guest + b->size);
                failure = detail; goto bad;
            }
        }
    }
    if (set_count > props.limits.maxBoundDescriptorSets ||
        uniforms > props.limits.maxPerStageDescriptorUniformBuffers ||
        uniforms > props.limits.maxDescriptorSetUniformBuffers ||
        storage > props.limits.maxPerStageDescriptorStorageBuffers ||
        storage > props.limits.maxDescriptorSetStorageBuffers ||
        sampled > props.limits.maxPerStageDescriptorSampledImages ||
        sampled > props.limits.maxDescriptorSetSampledImages ||
        uniforms + storage + sampled > props.limits.maxPerStageResources) {
        failure = "compute descriptors exceed device limits"; goto bad;
    }
    // OP_DISPATCH 帧队列收编（s->fq_pool 门；门关以下整块不执行、不做任何池查找，逐字节不变）：
    // 每条 guest-backed 绑定试一次 AHB 池直绑——命中就不再建影子/上传/回写，描述符写池后备 +
    // 池内偏移。一条 miss、偏移不对齐或可写性未证时先返回重试信号；调用方同步后重建影子。
    // out.fq 只在所有 guest-backed 绑定均为只读且已安全命中时置 1。
    if (s->fq_pool) {
        int all_hit = s2a_poolahb_lookup_usage && nvn_vk_first_draw_submit_seq &&
                      nvn_vk_first_draw_submit_seq() != 0;
        for (int i = 0; all_hit && i < p.binding_count; ++i) {
            struct compute_buffer *b = &o->buffers[i];
            const struct fd_binding *r = &p.bindings[i];
            if (r->kind == 2 || !b->guest_backed) continue;
            // AHB alias 自检目前只证明 guest 写→GPU 读；未证明 GPU 写→guest 读。
            // 可写缓冲必须先同步、再用原 staging/回写路径，不推断缓存一致性。
            if (r->kind == 1 && b->writable) goto retry_sync;
            VkBuffer pb = VK_NULL_HANDLE; uint64_t po = 0; VkBufferUsageFlags pu = 0;
            const VkBufferUsageFlags need = r->kind == 0 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            uint64_t align = r->kind == 0 ? props.limits.minUniformBufferOffsetAlignment
                                           : profile_storage_alignment;
            if (s2a_poolahb_lookup_usage(b->guest, b->size, &pb, &po, &pu) != 0 ||
                !pb || !(pu & need) || !align || po % align) { all_hit = 0; break; }
            b->buffer = pb; b->window_off = po; b->pooled = 1;
        }
        if (!all_hit) goto retry_sync;
        out->fq = 1;
    }
    // size=0（长度未声明）的 storage 绑定：0100F2C0115B6000 相邻缓冲的精确长度可互相覆盖（实测 4 MiB 缓冲只差
    // 0x200），若各建一份影子，GPU 写会分叉、回写互相覆盖。这里按“本次 dispatch 所有无界 storage 的
    // 并集”建一份影子，所有无界绑定按偏移绑进同一份字节，帧尾只回写一次。
    {
        uint64_t win_base = UINT64_MAX, win_end = 0;
        int win_n = 0, win_writable = 0;
        for (int i = 0; i < p.binding_count; ++i) {
            struct compute_buffer *b = &o->buffers[i];
            if (p.bindings[i].kind != 1 || !b->unbounded || b->pooled) continue;   // pooled 直绑池页，不进影子
            if (b->guest < win_base) win_base = b->guest;
            if (b->guest + b->size > win_end) win_end = b->guest + b->size;
            if (b->writable) win_writable = 1;
            win_n++;
        }
        if (win_n) {
            // 描述符 offset 必须是 minStorageBufferOffsetAlignment 的倍数：把并集起点按 64 字节向下对齐，
            // 各绑定的 offset = guest − win_base 仍是“两个对齐地址之差”，对齐条件由上面的逐条校验保证。
            win_base &= ~(uint64_t)63u;
            const uint64_t win_size = win_end - win_base;
            if (!win_size || win_size > COMPUTE_MAX_BYTES) {
                failure = "compute unbounded storage window exceeds limits"; goto bad;
            }
            VkBufferCreateInfo wci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = win_size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            if (vkCreateBuffer(d->device, &wci, NULL, &o->window.buffer) != VK_SUCCESS) {
                failure = "compute unbounded window buffer creation failed"; goto bad;
            }
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(d->device, o->window.buffer, &mr);
            uint32_t type = s2a_vk_memory_type(d, mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
            if (type == UINT32_MAX) { failure = "compute unbounded window memory type unavailable"; goto bad; }
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = mr.size, .memoryTypeIndex = type };
            if (vkAllocateMemory(d->device, &mai, NULL, &o->window.memory) != VK_SUCCESS ||
                vkBindBufferMemory(d->device, o->window.buffer, o->window.memory, 0) != VK_SUCCESS ||
                vkMapMemory(d->device, o->window.memory, 0, win_size, 0, &o->window.mapped) != VK_SUCCESS ||
                memory->read(memory->opaque, win_base, o->window.mapped, (size_t)win_size)) {
                failure = "compute unbounded window upload failed"; goto bad;
            }
            o->window.base = win_base; o->window.size = win_size; o->window.writable = win_writable;
#ifdef __ANDROID__
            static _Atomic int noted_window;
            if (atomic_fetch_add(&noted_window, 1) < 8)
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                    "compute unbounded storage: %d binding(s) share one window [0x%llx,+%llu) writable=%d",
                    win_n, (unsigned long long)win_base, (unsigned long long)win_size, win_writable);
#endif
            for (int i = 0; i < p.binding_count; ++i) {
                struct compute_buffer *b = &o->buffers[i];
                if (p.bindings[i].kind != 1 || !b->unbounded || b->pooled) continue;
                b->shared = 1; b->window_off = b->guest - win_base;
                b->buffer = o->window.buffer; b->memory = o->window.memory; b->mapped = o->window.mapped;
            }
        }
    }
    for (int i = 0; i < p.binding_count; ++i) {
        struct compute_buffer *b = &o->buffers[i];
        if (p.bindings[i].kind == 2) continue;   // 纹理条目没有 guest 缓冲
        if (b->shared || b->pooled) continue;    // 已绑进并集影子或直绑池后备：不建影子、不分配、不读 guest
        if (allocate(d, b, p.bindings[i].kind == 0 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) goto bad;
        if (p.bindings[i].is_support) {
            static const uint8_t one[4] = {0, 0, 0x80, 0x3f}; /* IEEE754 float1.0, little endian. */
            if (p.support_reads_unknown) {
                /* 编译器分析不出读集：按“每个 4 字节字都是恒等值”填满整个 SupportBuffer。
                 * 恒等值在布局里的含义：render scale / render scale_y = 1.0、s2a_flags 位全 0、
                 * 其余浮点字段 = 1.0。非 scale 字段若被读到的确定性见矩阵限制（未验证的字段
                 * 不在这里假装已知）。 */
#ifdef __ANDROID__
                static _Atomic int logged;
                if (!atomic_fetch_add(&logged, 1))
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "compute support reads unknown (compiler analysis null): filling identity values");
#endif
                for (uint32_t off = 0; off + 4 <= p.support_bytes; off += 4)
                    memcpy((uint8_t *)b->mapped + off, one, 4);
            } else {
                /* No graphics initialization and no whole-buffer zero fill: padding
                 * and unobserved fields carry no promised value and cannot be read. */
                for (uint32_t j = 0; j < p.support_read_count; ++j) {
                    const struct nvn_vk_c0_read *r = &p.support_reads[j];
                    memcpy((uint8_t *)b->mapped + r->offset, one + (r->offset & 3u), r->size);
                }
            }
        }
        else if (p.bindings[i].kind == 0 && p.bindings[i].slot == 0) memcpy(b->mapped, c0, sizeof c0);
        else if (p.bindings[i].kind == 0 && p.bindings[i].slot == 1)
            memcpy(b->mapped, source->c1, NVK_COMPUTE_C1_BYTES);
        else if (memory->read(memory->opaque, b->guest, b->mapped, (size_t)b->size)) {
            failure = "compute guest buffer read failed"; goto bad;
        }
    }
    /* PSO 缓存键在绑定过滤完成后取最终 p.bindings 算签名；门关则完全不算（零开销）。
     * 程序级命中（resolved）：对象随条目接管完成，这里自然落 o->pipeline 短路，整段跳过；
     * 程序级门开（prog）：不叠 spirv 键层——identity 键已在桥上查过，这里直接建对象后按
     * identity 入表。 */
    uint8_t pso_sig[FD_MAX_BINDINGS * 12 + 4]; size_t pso_sig_len = 0;
    uint64_t pso_h = 0; VkPipelineCache pso_plc = VK_NULL_HANDLE;
    const int pso_cache = !resolved && !prog_cache_on() && cpso_on();
    if (pso_cache) {
        for (int i = 0; i < p.binding_count; ++i) {
            const uint32_t t[3] = { (uint32_t)p.bindings[i].set, (uint32_t)p.bindings[i].binding,
                                    (uint32_t)p.bindings[i].kind };
            memcpy(pso_sig + pso_sig_len, t, sizeof t); pso_sig_len += sizeof t;
        }
        pso_sig[pso_sig_len++] = 0;   // 特化常量记号位：当前无 spec；将来加 pSpecializationInfo 必须并进键
        pso_h = cpso_hash(p.stage.spirv, p.stage.spirv_bytes)
              ^ cpso_hash(pso_sig, pso_sig_len) * 0x9e3779b97f4a7c15ull;
        cpso_lock();
        pso_plc = cpso_plc_locked(d->device);
        struct cpso *hit = cpso_lookup_locked(pso_h, p.stage.spirv, p.stage.spirv_bytes,
                                              pso_sig, pso_sig_len);
        if (hit) {
            o->module = hit->module; o->layout = hit->layout; o->pipeline = hit->pipeline;
            for (uint32_t i = 0; i < hit->set_count; ++i) o->sets[i] = hit->sets[i];
            o->pso = hit;
        }
        cpso_unlock();
    }
    const int prog = !resolved && prog_cache_on();
    if (prog) {
        cpso_lock();
        pso_plc = cpso_plc_locked(d->device);   // 顺带设备切换冲表（与 pso 路径同一守卫）
        cpso_unlock();
    }
    if (o->pipeline) goto pso_ready;
    VkShaderModuleCreateInfo sm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = p.stage.spirv_bytes, .pCode = (const uint32_t *)p.stage.spirv };
    if (vkCreateShaderModule(d->device, &sm, NULL, &o->module) != VK_SUCCESS) goto bad;
    for (uint32_t set = 0; set < set_count; ++set) {
        VkDescriptorSetLayoutBinding bindings[FD_MAX_BINDINGS]; uint32_t n = 0;
        for (int i = 0; i < p.binding_count; ++i) if (p.bindings[i].set == (int)set) {
            bindings[n++] = (VkDescriptorSetLayoutBinding) { .binding = (uint32_t)p.bindings[i].binding,
                .descriptorType = binding_type(p.bindings[i].kind),
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
        }
        VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = n, .pBindings = bindings };
        if (vkCreateDescriptorSetLayout(d->device, &ci, NULL, &o->sets[set]) != VK_SUCCESS) goto bad;
    }
    VkPipelineLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = set_count, .pSetLayouts = o->sets };
    if (vkCreatePipelineLayout(d->device, &li, NULL, &o->layout) != VK_SUCCESS) goto bad;
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = o->module, .pName = "main" }, .layout = o->layout };
    {
        /* 创建耗时如实累计：缓存开 → 每帧应降到 0；关 → 与修前一致（逐帧 TSV cpso_us 的数）。 */
        struct timespec cpso_t0, cpso_t1;
        clock_gettime(CLOCK_MONOTONIC, &cpso_t0);
        VkResult pso_rc = vkCreateComputePipelines(d->device, pso_plc, 1, &ci, NULL, &o->pipeline);
        clock_gettime(CLOCK_MONOTONIC, &cpso_t1);
        atomic_fetch_add(&g_cpso_creates, 1);
        atomic_fetch_add(&g_cpso_create_ns,
            (uint64_t)(cpso_t1.tv_sec - cpso_t0.tv_sec) * 1000000000ull + (uint64_t)(cpso_t1.tv_nsec - cpso_t0.tv_nsec));
        if (pso_rc != VK_SUCCESS) goto bad;
    }
    if (pso_cache) {
        cpso_lock();
        o->pso = cpso_insert_locked(d->device, pso_h, p.stage.spirv, p.stage.spirv_bytes,
            pso_sig, pso_sig_len, o->module, o->sets, set_count, o->layout, o->pipeline);
        cpso_unlock();
        /* 入表失败（表满且在途/同键先至/内存失败）：o->pso 保持空，对象按笔自有收尾。 */
    } else if (prog && identity && o->pipeline) {
        cpso_lock();
        o->pso = cprog_insert_locked(d->device, identity, &p, &program->profile,
            o->module, o->sets, set_count, o->layout, o->pipeline);
        cpso_unlock();
        /* 同口径：入表失败 o->pso 保持空，对象按笔自有收尾。 */
    }
pso_ready:
    if (set_count) {
        // 池按实际出现的四类描述符给额度；kind 2 是 COMBINED_IMAGE_SAMPLER（纹理+采样器成对）。
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0}};
        for (int i = 0; i < p.binding_count; ++i)
            sizes[p.bindings[i].kind == 0 ? 0 : p.bindings[i].kind == 1 ? 1 : 2].descriptorCount++;
        VkDescriptorPoolSize used[3]; uint32_t n = 0;
        for (int i = 0; i < 3; ++i) if (sizes[i].descriptorCount) used[n++] = sizes[i];
        VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = set_count, .poolSizeCount = n, .pPoolSizes = used };
        if (vkCreateDescriptorPool(d->device, &pi, NULL, &o->pool) != VK_SUCCESS) goto bad;
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = o->pool, .descriptorSetCount = set_count, .pSetLayouts = o->sets };
        if (vkAllocateDescriptorSets(d->device, &ai, out->sets) != VK_SUCCESS) goto bad;
        VkDescriptorBufferInfo bi[FD_MAX_BINDINGS];
        VkDescriptorImageInfo ii[FD_MAX_BINDINGS];
        VkWriteDescriptorSet writes[FD_MAX_BINDINGS];
        for (int i = 0; i < p.binding_count; ++i) {
            s2a_desc_write_init(&writes[i], out->sets[p.bindings[i].set],
                (uint32_t)p.bindings[i].binding, binding_type(p.bindings[i].kind), 1);
            if (p.bindings[i].kind == 2) {
                // 直接用消费点快照里的对象/视图/采样器：不按槽号重新解析，编译等待期间的
                // Finalize/同槽换对象不会让这里绑到另一个对象。
                const struct nvk_compute_tex_binding *tb =
                    nvk_compute_texture_snapshot_find(snapshot, (unsigned)p.bindings[i].tex_slot);
                if (!tb) {
                    failure = "compute texture snapshot entry disappeared before descriptor write";
                    goto bad;
                }
                ii[i].sampler = tb->sampler;
                ii[i].imageView = tb->view;
                ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // 纹理上传后/回放里一直停在 GENERAL
                writes[i].pImageInfo = &ii[i];
            } else {
                bi[i] = (VkDescriptorBufferInfo) { .buffer = o->buffers[i].buffer,
                    .offset = o->buffers[i].window_off, .range = o->buffers[i].size };
                writes[i].pBufferInfo = &bi[i];
            }
        }
        vkUpdateDescriptorSets(d->device, (uint32_t)p.binding_count, writes, 0, NULL);
    }
    // 成功才转移保活所有权：owner 在 finish 里解除；失败路径快照原样留给调用方释放。
    if (snapshot) {
        o->snapshot = *snapshot;
        memset(snapshot, 0, sizeof *snapshot);
    }
    out->pipeline = o->pipeline; out->layout = o->layout; out->set_count = set_count;
    out->owner = o; out->finish = finish;
    nvn_vk_program_free_compute(&p);
    return 0;
retry_sync:
    destroy(o); nvn_vk_program_free_compute(&p); memset(out, 0, sizeof *out);
    return S2A_RECORD_DISPATCH_RETRY_SYNC;
bad:
    {
        // 一次性事实行：失败时把本次 dispatch 的绑定/槽原样打出，判定“size=0”是录制口径还是
        // 游戏真的这么绑（限量 8 次，避免刷屏）。
        static _Atomic int dumped;
        if (atomic_fetch_add(&dumped, 1) < 8) {
            for (int i = 0; i < p.binding_count; ++i) {
                const struct fd_binding *r = &p.bindings[i];
                const struct nvn_vk_compute_storage *m = r->kind == 1 ? &p.storage[i] : NULL;
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                    "compute fact: set=%d binding=%d kind=%d slot=%d support=%d storage_map=%d rw=%d",
                    r->set, r->binding, r->kind, r->slot, (int)r->is_support,
                    m ? (int)m->mapping : -1, m ? (int)m->writable : -1);
            }
            for (int st = 0; st < 6; ++st) {
                if (!(s->ssbo_seen & (1u << st)) && !(s->ubo_seen & (1u << st))) continue;
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                    "compute slot fact: stage=%d ssbo[%d]=0x%llx/%llu ubo[%d]=0x%llx/%llu groups=%u,%u,%u",
                    st, st, (unsigned long long)s->ssbo[st].addr, (unsigned long long)s->ssbo[st].size,
                    st, (unsigned long long)s->ubo[st].addr, (unsigned long long)s->ubo[st].size,
                    s->groups[0], s->groups[1], s->groups[2]);
            }
        }
    }
    destroy(o); nvn_vk_program_free_compute(&p); memset(out, 0, sizeof *out);
    return error(why, why_bytes, failure);
}
int nvn_vk_compute_pipeline_build(struct s2a_vk_device *d,
    const struct s2a_record_dispatch_state *s, const struct s2a_record_memory *memory,
    struct nvk_compute_tex_snapshot *snapshot,
    const struct s2a_source_entry_view *source,
    const struct nvk_compute_lookup_result *program,
    struct s2a_record_dispatch_output *out, char *why, size_t why_bytes) {
    return pipeline_build_impl(d, s, memory, snapshot, source, program, NULL, NULL, out, why, why_bytes);
}
/* miss 路径（桥带 identity）：prog 门开时成功构建后按 identity 入程序级表。 */
int nvn_vk_compute_pipeline_build_prog(struct s2a_vk_device *d,
    const struct s2a_record_dispatch_state *s, const struct s2a_record_memory *memory,
    struct nvk_compute_tex_snapshot *snapshot,
    const struct s2a_source_entry_view *source,
    const struct nvk_compute_lookup_result *program, const uint8_t identity[32],
    struct s2a_record_dispatch_output *out, char *why, size_t why_bytes) {
    return pipeline_build_impl(d, s, memory, snapshot, source, program, NULL, identity, out, why, why_bytes);
}
/* 程序级命中路径：lookup/parse/对象创建全省。接管 entry 的 owner 引用——
 * 成败都由本函数收尾（早退归还引用，成功归 owner，失败经 destroy 归还）。 */
int nvn_vk_compute_pipeline_build_cached(struct s2a_vk_device *d,
    const struct s2a_record_dispatch_state *s, const struct s2a_record_memory *memory,
    struct nvk_compute_tex_snapshot *snapshot,
    const struct s2a_source_entry_view *source, struct cpso *entry,
    struct s2a_record_dispatch_output *out, char *why, size_t why_bytes) {
    if (!entry) return error(why, why_bytes, "compute prog cache entry is missing");
    return pipeline_build_impl(d, s, memory, snapshot, source, NULL, entry, NULL, out, why, why_bytes);
}
