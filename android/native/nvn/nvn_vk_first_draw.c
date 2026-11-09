// 首笔真实 GPU 绘制：把统一编译结果（内存里的 s2a-direct-program-v1）直接消费成真实 index draw。
// 只用真实快照与真实反射：资源从 Draw 快照的逐槽位绑定解析，缺一项就明确失败，不用占位/写死资源。
#include <stdio.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>

#include "nvn_vk_first_draw.h"
#include "nvn_obj.h"
#pragma weak nvn_ga2ca
#include "nvn_vk_program_result.h"
#include "nvn_vk_format.h"
#include "nvn_vk_mrt.h"
#include "nvn_vk_texview.h"
#include "nvn_vk_sampler.h"
#include "nvn_vk_sampler_cache.h"
#include "nvn_vk_texcache.h"
#include "nvn_vk_poolahb.h"
#include "nvn_vk_query.h"
#include "nvn_vk_desc_write.h"
#include "nvn_vk_snap_pool.h"
#include "nvn_vk_query_retire.h"
#include "nvn_vk_pipe_recipe.h"
#include "nvn_vk_pipe_worker.h"
#include "nvn_first_use.h"
#include "nvn_vk_driver_cache.h"
#include "nvn_vk_draw_diag.h"
#include "nvn_vk_pipe_manifest.h"
#include "direct/program_cache.h"
#include "mtl_vk_adapter.h"
#include "direct/sha256.h"
#include "direct/spirv_storage_vk.h"

// SupportBuffer 布局（与统一编译器 SupportBuffer.cs 一致；只为本笔需要的字段写值）：
#define SB_ALPHA_TEST        0u
#define SB_VIEWPORT_INVERSE  144u
#define SB_RENDER_SCALE      192u
#define SB_S2A_FLAGS         1392u
#define SB_S2A_POINT         1408u
#define SB_RENDER_SCALE_Y    1440u
#define SB_RENDER_SCALE_COUNT 73u

#define FD_LOG(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", "first draw: " __VA_ARGS__)
static int fd_tex_diag_level(void);
static void fd_timing_frame_boundary(void);
// 采样输入诊断（tex_diag=2）：每 (request, 纹理 id) 一条，含注册视图与采样器参数；有界，不逐帧刷。
static void fd_tex_diag_once(const char *request_id, uint32_t tid, const struct nvn_vk_tex *tex,
                             const samp_par *par, int set, int binding, int slot) {
    static struct { char rid[24]; uint32_t id; } seen[4096];
    static int n;
    const char *rid = request_id ? request_id : "?";
    for (int i = 0; i < n; i++)
        if (seen[i].id == tid && strncmp(seen[i].rid, rid, sizeof seen[i].rid - 1) == 0) return;
    extern int s2a_vk_adapter_tex_view_by_id(int id, view_t *out) __attribute__((weak));
    view_t v;
    memset(&v, 0, sizeof v);
    int hv = s2a_vk_adapter_tex_view_by_id ? s2a_vk_adapter_tex_view_by_id((int)tid, &v) : -1;
    // 级别 3：只关心“视图有覆盖字段”或非 2D 目标的采样（正常 2D 全等视图不刷屏）。
    if (fd_tex_diag_level() == 3 && tex && tex->nvn_target == 1 &&
        !(hv == 1 && (v.set_format || v.set_levels || v.set_layers || v.set_swizzle || v.set_target)))
        return;
    if (n >= (int)(sizeof seen / sizeof seen[0])) return;
    snprintf(seen[n].rid, sizeof seen[n].rid, "%s", rid);
    seen[n].id = tid;
    n++;
    // 纹理目标/格式/尺寸来自 nvn_vk_tex（视图是这一层真正绑定的东西）。
    uint32_t target = 0, fmt = 0, w = 0, h = 0, layers = 0, levels = 0;
    if (tex) { target = tex->nvn_target; fmt = tex->nvn_format; w = tex->width; h = tex->height; layers = tex->layers; levels = tex->levels; }
    char view_txt[160];
    if (hv == 1)
        snprintf(view_txt, sizeof view_txt,
                 "present=1 set_fmt=%d fmt=0x%x set_lv=%d lv=%u+%u set_ly=%d ly=%u+%u set_swz=%d swz=%u,%u,%u,%u set_tgt=%d tgt=%u",
                 v.set_format, v.format, v.set_levels, v.base_level, v.nlevels,
                 v.set_layers, v.min_layer, v.nlayers,
                 v.set_swizzle, v.swz[0], v.swz[1], v.swz[2], v.swz[3], v.set_target, v.target);
    else
        snprintf(view_txt, sizeof view_txt, "present=%d", hv == 1 ? 1 : 0);
    FD_LOG("tex-diag: request=%.16s id=%u set=%d bind=%d slot=%d target=%u fmt=0x%x %ux%u L=%u layers=%u "
           "tex=(flags=0x%x stride=%u blk_h=%d set_swz=%d swz=0x%x) "
           "view=(%s) samp=(min=%u mag=%u wrap=%u/%u/%u cmp=%u/%u lod=%.2f..%.2f/%.2f aniso=%.1f)",
           rid, tid, set, binding, slot, target, fmt, w, h, levels, layers,
           tex ? tex->nvn_flags : 0u, tex ? tex->nvn_stride : 0u, tex ? tex->nvn_pkg_block_h : 0,
           tex ? tex->set_swz : 0, tex ? tex->swz : 0u, view_txt,
           par ? par->minf : 0, par ? par->magf : 0, par ? par->ws : 0, par ? par->wt : 0, par ? par->wr : 0,
           par ? par->cmp_mode : 0, par ? par->cmp_func : 0,
           par ? par->lod_min : 0.0, par ? par->lod_max : 0.0, par ? par->lod_bias : 0.0,
           par ? par->aniso : 0.0);
}
// 诊断开关（进程启动前设置，进程内缓存一次）：
//   debug.s2a.draw_log=1  每笔绘制的进度日志（默认关；正常性能模式不写）
//   debug.s2a.draw_probe=1 每笔 draw 的 5 点 before/after 像素探针与读回（默认关）
// 两者只影响诊断输出/额外诊断拷贝，取舍不影响绘制本身与游戏需要的屏障。
// 逐帧快照的调试开关：__system_property_get 每次约 1-3 µs，而 trace/no_depth/fail_desc_alloc
// 原来是在每一笔 draw 里各读一次。开关是调试旋钮，按帧刷新足够；语义不变（帧内取值一致）。
static char g_prop_trace[PROP_VALUE_MAX] = {0};
static char g_prop_no_depth[PROP_VALUE_MAX] = {0};
static int g_prop_fail_desc_alloc;
static void fd_props_refresh(void) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.trace", g_prop_trace);
    __system_property_get("debug.s2a.no_depth", g_prop_no_depth);
    __system_property_get("debug.s2a.fail_desc_alloc", v);
    g_prop_fail_desc_alloc = v[0] ? atoi(v) : 0;
}

static int fd_prop_is_on(const char *name) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get(name, v);
    return v[0] == '1' && v[1] == 0;
}
// 生产通道的 draw_log 覆盖值：user 版设备上 untrusted_app 读不到 debug_prop，
// adb shell 设了 debug.s2a.draw_log 进程内也拿不到，因此 Java 侧 intent extra
// s2arm.draw_log 经 NativeBridge.setDrawLog 落到这里；-1 = 未覆盖，仍按属性。
static _Atomic int g_draw_log_override = -1;
void s2a_fd_set_draw_log(int on) { if (on >= 0) atomic_store(&g_draw_log_override, on); }
static int fd_verbose(void) {
    int ov = atomic_load(&g_draw_log_override);
    if (ov >= 0) return ov;
    static int v = -1; if (v < 0) v = fd_prop_is_on("debug.s2a.draw_log"); return v;
}
static int fd_probe_enabled(void) { static int v = -1; if (v < 0) v = fd_prop_is_on("debug.s2a.draw_probe"); return v; }
// 诊断：debug.s2a.pass_probe=1 在每次 render pass 收尾时把该 pass 颜色目标上的 5 点像素拷进
// 本帧的 host buffer，flush 时逐 pass 打印。用于定位“世界首帧偏色出自哪个 pass”，不逐笔、
// 不新建每笔资源；代价是每个 pass 5 次 1x1 copy，默认关。
static int fd_pass_probe_enabled(void) { static int v = -1; if (v < 0) v = fd_prop_is_on("debug.s2a.pass_probe"); return v; }
// 诊断：debug.s2a.tex_diag=1 只记“非 2D 采样目标”（立方体贴图/3D 等）；=2 时每个 (request, 纹理 id)
// 都记一条真实采样输入事实（目标/格式/尺寸/注册视图/采样器）；=3 只记“视图有覆盖字段（格式/mip/层/
// swizzle/target）或非 2D 目标”的采样。只用于把画面异常关联到被采样的 target/view，而不是按截图猜。
static int fd_tex_diag_level(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.tex_diag", b);
        v = b[0] ? atoi(b) : 0;
    }
    return v;
}
#define FD_VLOG(...) do { if (fd_verbose()) FD_LOG(__VA_ARGS__); } while (0)
// 首 8 笔诊断的日志出口：保留 FD_LOG 的 "first draw: " 前缀，落到同一条 logcat。
static void fd_diag_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[S2A_FIRST_DRAW_DIAG_MSG_CAP];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    FD_LOG("%s", msg);
}

static int fail(char *why, size_t n, const char *text) {
    if (why && n) snprintf(why, n, "%s", text);
    return -1;
}
// 附件视图选中 mip 时，framebuffer 的宽高必须是该 mip 的尺寸，不是底图尺寸。
static uint32_t fd_mipdim(uint32_t s, uint32_t level) { s >>= level; return s ? s : 1; }

// 颜色附件格式能力：按 physical device 的 optimalTilingFeatures 查（按设备+格式缓存）。
// 没有 physical device（宿主假设备/纯逻辑夹具）时不伪造查询结果，返回全 1 由调用方跳过设备能力关。
struct fd_fmt_cap { VkPhysicalDevice phys; VkFormat fmt; VkFormatFeatureFlags flags; };
#define FD_FMT_CAP 64
static struct fd_fmt_cap g_fmt_cap[FD_FMT_CAP];
static int g_fmt_cap_n;
static VkFormatFeatureFlags fd_format_features(struct s2a_vk_device *d, VkFormat fmt) {
    if (!d || !d->physical) return (VkFormatFeatureFlags)~0u;
    for (int i = 0; i < g_fmt_cap_n; i++)
        if (g_fmt_cap[i].phys == d->physical && g_fmt_cap[i].fmt == fmt) return g_fmt_cap[i].flags;
    VkFormatProperties props;
    memset(&props, 0, sizeof props);
    vkGetPhysicalDeviceFormatProperties(d->physical, fmt, &props);
    if (g_fmt_cap_n < FD_FMT_CAP) {
        g_fmt_cap[g_fmt_cap_n].phys = d->physical;
        g_fmt_cap[g_fmt_cap_n].fmt = fmt;
        g_fmt_cap[g_fmt_cap_n].flags = props.optimalTilingFeatures;
        g_fmt_cap_n++;
    }
    return props.optimalTilingFeatures;
}

// 离线库条目在"采纳"之前必须过同一套解析：容器摘要正确只证明字节没坏，
// 不证明 schema/阶段/SPIR-V/SupportBuffer 布局可消费（见 offline_library.h 的命中判据）。
// 返回 1 = 可解析；0 = 不可解析（why 里是 parse_program 的原因）。
int nvn_vk_first_draw_program_valid(const uint8_t *bytes, size_t length, char *why, size_t why_size) {
    struct fd_program p;
    memset(&p, 0, sizeof p);
    int rc = nvn_vk_program_parse_graphics(bytes, length, &p, why, why_size);
    nvn_vk_program_free(&p);
    return rc == 0;
}

// 程序内容身份：SHA-256(program bytes)。requestId 只拼阶段短键，不能当内容身份。
// 这里缓存“已解析 program”；模块/四 set 布局/pipeline layout 由下面的 ProgramRef 对象表按
// 同一内容身份只创建一次。
// 解析缓存的容量不是语义上限：初始 96，按需翻倍到 512（LRU 仍在，但不再因为“城里程序多”
// 不断重解析）。命中的 ProgramRef/管线对象仍按内容 SHA 复用，解析只是入口段的快速路径。
#define FD_PROG_CACHE 96
#define FD_PROG_CACHE_MAX 512
struct fd_prog_slot { int used; uint8_t sha[32]; struct fd_program prog; uint64_t last_use; };
static struct fd_prog_slot *g_prog_cache;
static int g_prog_cap, g_prog_n;
static uint64_t g_prog_clock;
static _Atomic uint64_t g_parse_hits, g_parse_misses;
// 诊断：debug.s2a.dump_prog_dir=<绝对目录> 时，把每个实际使用的已编译程序字节按其内容
// SHA-256 前 8 字节落一次盘（<16hex>.program）。用途：把 pass probe 打印的 prog= 前缀映射回
// 具体程序，再离线看 SPIR-V/反射；默认关，只有诊断局才写。
static void fd_prog_dump(const uint8_t *bytes, size_t n, const uint8_t sha[32]) {
    static char dir[PROP_VALUE_MAX] = {0};
    static int init = 0;
    if (!init) { init = 1; __system_property_get("debug.s2a.dump_prog_dir", dir); if (dir[0]) mkdir(dir, 0777); }
    if (!dir[0] || !bytes || !n || !sha) return;
    static uint8_t seen[512][4]; static int nseen;
    for (int i = 0; i < nseen; i++) if (!memcmp(seen[i], sha, 4)) return;
    if (nseen >= 512) return;
    memcpy(seen[nseen++], sha, 4);
    char path[PROP_VALUE_MAX + 96];
    snprintf(path, sizeof path, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.program", dir,
             sha[0], sha[1], sha[2], sha[3], sha[4], sha[5], sha[6], sha[7]);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    size_t w = fwrite(bytes, 1, n, f);
    fclose(f);
    FD_LOG("program dump: %s (%zu/%zu B)", path, w, n);
}
// 程序查找加速：按 SHA 首字节的直接索引。世界一帧数百笔，每笔都对 96 项做 32 字节 memcmp
// 是白白的热路径开销；绝大多数命中会落在索引指向的槽位，未命中再回退到线性扫描（不改变语义）。
static int16_t g_prog_idx[256];   // -1 = 空；只做加速，命中失败一律回退
static int g_prog_idx_ready;
static void fd_prog_idx_init(void) {
    if (g_prog_idx_ready) return;
    for (int i = 0; i < 256; i++) g_prog_idx[i] = -1;
    g_prog_idx_ready = 1;
}
// 0 = 已增长；-1 = 到达预算上限/分配失败（调用方退回 LRU 淘汰）。
static int fd_prog_cache_grow(void) {
    int ncap = g_prog_cap ? g_prog_cap * 2 : FD_PROG_CACHE;
    if (ncap > FD_PROG_CACHE_MAX) ncap = FD_PROG_CACHE_MAX;
    if (ncap <= g_prog_cap) return -1;
    struct fd_prog_slot *ns = (struct fd_prog_slot *)calloc((size_t)ncap, sizeof *ns);
    if (!ns) return -1;
    if (g_prog_cache) {
        memcpy(ns, g_prog_cache, (size_t)g_prog_n * sizeof *ns);
        free(g_prog_cache);
    }
    g_prog_cache = ns; g_prog_cap = ncap;
    for (int i = 0; i < 256; i++) g_prog_idx[i] = -1;
    for (int i = 0; i < g_prog_n; i++)
        if (g_prog_cache[i].used) g_prog_idx[g_prog_cache[i].sha[0]] = (int16_t)i;
    return 0;
}
// ---- 崩溃取证：最近 N 次关键 Vulkan 调用 -----------------------------------------------
// 菜单/暂停界面这类新路径会让 Adreno 驱动 SIGSEGV，符号化没用（APK 里的 so 被 strip）。
// 这里把最近 64 次关键调用记成文本，崩溃处理器写 exit.log 时一起落盘。
// 只在 debug.s2a.callring=1 时工作，默认零开销。
#define FD_CALLRING_N 64
static char g_callring[FD_CALLRING_N][176];
static unsigned g_callring_head;
static int g_callring_on = -1;
int nvn_vk_callring_enabled(void) {
    if (g_callring_on < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.callring", b);
        g_callring_on = (b[0] && atoi(b)) ? 1 : 0;
    }
    return g_callring_on;
}
void nvn_vk_callring_add(const char *fmt, ...) {
    if (!nvn_vk_callring_enabled()) return;
    unsigned i = __atomic_fetch_add(&g_callring_head, 1, __ATOMIC_RELAXED);
    char *dst = g_callring[i % FD_CALLRING_N];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, 176, fmt, ap);
    va_end(ap);
}
// 按时间顺序把环形缓冲写进 fd（崩溃处理器用）。
void nvn_vk_callring_dump(int fd) {
    unsigned h = __atomic_load_n(&g_callring_head, __ATOMIC_RELAXED);
    unsigned n = h < FD_CALLRING_N ? h : FD_CALLRING_N;
    if (!n) return;
    (void)!dprintf(fd, "--- last %u vulkan calls (oldest first) ---\n", n);
    for (unsigned k = 0; k < n; k++) {
        unsigned idx = (h - n + k) % FD_CALLRING_N;
        (void)!dprintf(fd, "[%u] %s\n", k, g_callring[idx]);
    }
}
static struct fd_program *fd_prog_get(const uint8_t *bytes, size_t n, const uint8_t *known_sha,
                                      uint8_t sha[32], char *why, size_t why_size) {
    if (known_sha) memcpy(sha, known_sha, 32);
    else { s2a_sha256 ctx; s2a_sha256_init(&ctx); s2a_sha256_update(&ctx, bytes, n); s2a_sha256_final(&ctx, sha); }
    fd_prog_dump(bytes, n, sha);
    fd_prog_idx_init();
    if (!g_prog_cap && fd_prog_cache_grow() != 0) {
        if (why && why_size) snprintf(why, why_size, "parse cache allocation failed");
        return NULL;
    }
    {
        int16_t s0 = g_prog_idx[sha[0]];
        if (s0 >= 0 && s0 < g_prog_n && g_prog_cache[s0].used && !memcmp(g_prog_cache[s0].sha, sha, 32)) {
            g_prog_cache[s0].last_use = ++g_prog_clock;
            atomic_fetch_add_explicit(&g_parse_hits, 1, memory_order_relaxed);
            return &g_prog_cache[s0].prog;
        }
    }
    for (int i = 0; i < g_prog_n; i++)
        if (g_prog_cache[i].used && !memcmp(g_prog_cache[i].sha, sha, 32)) {
            g_prog_cache[i].last_use = ++g_prog_clock;
            atomic_fetch_add_explicit(&g_parse_hits, 1, memory_order_relaxed);
            return &g_prog_cache[i].prog;
        }
    int slot = -1;
    if (g_prog_n < g_prog_cap) slot = g_prog_n++;
    else if (fd_prog_cache_grow() == 0) slot = g_prog_n++;
    if (slot < 0) {
        uint64_t oldest = ~0ull;
        for (int i = 0; i < g_prog_n; i++)
            if (g_prog_cache[i].last_use < oldest) { oldest = g_prog_cache[i].last_use; slot = i; }
        if (slot < 0) slot = 0;
        nvn_vk_program_free(&g_prog_cache[slot].prog);
        if (g_prog_idx[g_prog_cache[slot].sha[0]] == (int16_t)slot) g_prog_idx[g_prog_cache[slot].sha[0]] = -1;
        memset(&g_prog_cache[slot], 0, sizeof g_prog_cache[slot]);
    }
    struct fd_program p;
    memset(&p, 0, sizeof p);
    if (nvn_vk_program_parse_graphics(bytes, n, &p, why, why_size)) { nvn_vk_program_free(&p); return NULL; }
    atomic_fetch_add_explicit(&g_parse_misses, 1, memory_order_relaxed);
    g_prog_cache[slot].used = 1; memcpy(g_prog_cache[slot].sha, sha, 32); g_prog_cache[slot].prog = p;
    g_prog_idx[sha[0]] = (int16_t)slot;   // 加速索引；被淘汰时由下一次插入覆盖
    g_prog_cache[slot].last_use = ++g_prog_clock;
    return &g_prog_cache[slot].prog;
}

// ---- 程序级 Vulkan 对象（A2）：模块/四 set 布局/pipeline layout 每个内容只创建一次 ----
// 身份是完整结果字节的 SHA-256；requestId 只拼阶段短键，不能当对象身份。
// 归属：本表是这些对象的唯一 owner。管线缓存/本笔绘制只借引用，不再销毁它们。同帧内用过的
// 对象不回收（已录制未提交的命令仍引用其描述符集）；被管线引用（users>0）时也不回收。
#define FD_PROG_OBJ 512
struct fd_prog_obj {
    int used;
    uint8_t sha[32];
    uint32_t attr_mask;              // 顶点属性掩码（管线顶点输入按它取 location）
    VkDevice device;                 // 对象属于哪个设备；不同设备绝不混用
    VkShaderModule vs, fs;
    VkDescriptorSetLayout sets[4];
    VkPipelineLayout playout;
    uint32_t users;                  // 管线缓存条目引用数
    uint64_t last_use;               // 最近使用的帧序号
    int failed;                      // 创建失败：同内容不反复重试
    struct fd_program prog;          // 已解析程序（反射/SPIR-V）：对象命中直接复用，不再经过解析缓存
    int has_prog;
};
static struct fd_prog_obj g_prog_obj[FD_PROG_OBJ];
static _Atomic uint64_t g_prog_owned_bytes;   // 对象持有的解析程序字节数（诊断/预算口径）
// 程序对象按 sha[0] 的直接索引：世界一帧 500 笔，逐笔线性扫 512 槽 × memcmp(32) 是热路径固定开销。
// 索引只做预筛，命中仍逐字节比较完整 sha 与 device；索引过期就回退线性扫描。
static int16_t g_prog_obj_idx[256];
static int g_prog_obj_idx_ready;
static _Atomic uint64_t g_prog_obj_created, g_prog_obj_hits, g_prog_obj_evicts;
static _Atomic uint64_t g_storage_feature_rejects;
static uint64_t g_frame_seq;         // 每次帧 flush 递增

static uint64_t g_arena_bytes; static int g_arena_chunk_n;   // 定义在后面：报告要提前用
// 反馈快照池：同一 (格式, 尺寸) 的快照图跨帧复用。
// 依据：帧边界（fd_fq 退休）已经等到 fence，更早帧用过的图此刻 GPU 不再引用，可以安全重用；
// 每次使用前都会重新做 pre-copy，所以内容不会被上一帧残留污染。
// 目的：把“每笔 draw 新建 ~4.6 MB 的 image+memory”的分配抖动降下来——实测这部份每秒 200+ 次
// 分配，驱动侧 mmap 逐渐失败（errno 12），最后连 5.6 MB 都申请不到而 abort。
// 槽数要盖住世界里出现的快照规格数：实测 32 槽时命中不了，分配一路涨到撞 4096 上限。
#define FD_SNAP_POOL 256
struct fd_snap_slot {
    VkImage img; VkDeviceMemory mem; VkImageView view;
    uint32_t w, h; VkFormat fmt;
    // 所属帧号：只有“这一帧的提交已经拿到完成证明”（延迟槽退休 / 同步 flush 等完 fence）之后才能复用。
    // 不能用全局周期计数代替完成证明：帧 N 提交后退休更早的槽也会让周期 +1，帧 N 仍在途时就会被误判可复用。
    uint64_t owner_frame;
    uint64_t bytes;        // 本槽占用的设备内存字节（预算记账）
    int used;
};
static struct fd_snap_slot g_snap_pool[FD_SNAP_POOL];
static int g_snap_pool_n;
// 最后已退休帧号：延迟槽退休时取该槽的 frame_id，同步 flush 等完 fence 后取当前帧号。
// 快照槽 owner_frame <= g_snap_retired_frame 才可复用/淘汰——保证那张图所属的提交已完成、GPU 不再引用。
static uint64_t g_snap_retired_frame;
static uint64_t g_snap_evicted;   // 池满/超预算淘汰次数：证明上限真的生效
static uint64_t g_snap_pool_bytes; // 池内快照占用的设备内存字节（硬上限的记账口径）
// 快照池字节预算：默认 256 MB。超出就先淘汰“所属提交已完成”的最旧槽；无槽可淘汰时退回帧队列
// （随帧退休），绝不无限增长——之前正是这条路径把整机内存吃满。
static uint64_t fd_snap_budget_bytes(void) {
    static uint64_t v;
    if (!v) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.snap_mb", b);
        v = (b[0] && atoi(b) > 0) ? (uint64_t)atoi(b) << 20 : (256ull << 20);
    }
    return v;
}

// 设备内存账（按类别）：当前/峰值字节、存活分配数、分配与释放次数。
// 只统计 Vulkan 分配对象（vkAllocateMemory），不冒充 Linux VMA：两者数量不等，VMA 另行采。
enum { FD_MEM_SNAP = 0, FD_MEM_TEX, FD_MEM_ARENA, FD_MEM_BUF, FD_MEM_OTHER, FD_MEM_CAT_N };
static const char *const fd_mem_cat_name[FD_MEM_CAT_N] = { "快照", "纹理", "arena", "buffer", "其它" };
static struct {
    _Atomic uint64_t cur, peak, allocs, frees;
} g_memcat[FD_MEM_CAT_N];
static _Atomic uint64_t g_devmem_bytes;       // 所有类别当前占用之和
static _Atomic uint64_t g_devmem_peak_bytes;
static _Atomic uint64_t g_devmem_allocs;
static _Atomic uint64_t g_devmem_frees;
static _Atomic uint64_t g_devmem_fails;
static _Atomic uint64_t g_devmem_snap_bytes;  // 兼容旧读数：快照字节
static _Atomic uint64_t g_devmem_snap_live;

// 句柄 → (大小, 类别) 登记表：释放时才知道该扣哪一类。开放寻址，满表时按“其它”计。
#define FD_DEVMEM_REG 8192
static struct { VkDeviceMemory mem; uint64_t size; uint8_t cat; } g_devmem_reg[FD_DEVMEM_REG];
static _Atomic int g_devmem_reg_n;
// 设备的 maxMemoryAllocationCount：区分“撞分配对象上限”和“物理内存不足”。
static uint32_t g_max_mem_allocs;
static void fd_devmem_register(VkDeviceMemory mem, uint64_t size, int cat) {
    if (!mem) return;
    uint64_t h = ((uint64_t)(uintptr_t)mem >> 4) & (FD_DEVMEM_REG - 1);
    for (int i = 0; i < FD_DEVMEM_REG; i++) {
        int idx = (int)((h + (uint64_t)i) & (FD_DEVMEM_REG - 1));
        if (!g_devmem_reg[idx].mem) {
            g_devmem_reg[idx].mem = mem; g_devmem_reg[idx].size = size; g_devmem_reg[idx].cat = (uint8_t)cat;
            atomic_fetch_add(&g_devmem_reg_n, 1);
            return;
        }
    }
}
static int fd_devmem_unregister(VkDeviceMemory mem, uint64_t *size) {
    if (!mem) return -1;
    uint64_t h = ((uint64_t)(uintptr_t)mem >> 4) & (FD_DEVMEM_REG - 1);
    for (int i = 0; i < FD_DEVMEM_REG; i++) {
        int idx = (int)((h + (uint64_t)i) & (FD_DEVMEM_REG - 1));
        if (g_devmem_reg[idx].mem == mem) {
            if (size) *size = g_devmem_reg[idx].size;
            int cat = g_devmem_reg[idx].cat;
            g_devmem_reg[idx].mem = VK_NULL_HANDLE;
            atomic_fetch_sub(&g_devmem_reg_n, 1);
            return cat;
        }
        if (!g_devmem_reg[idx].mem) return -1;
    }
    return -1;
}
static void fd_memcat_add(int cat, uint64_t size) {
    atomic_fetch_add(&g_memcat[cat].cur, size);
    atomic_fetch_add(&g_memcat[cat].allocs, 1);
    uint64_t cur = atomic_load(&g_memcat[cat].cur), peak = atomic_load(&g_memcat[cat].peak);
    while (cur > peak && !atomic_compare_exchange_weak(&g_memcat[cat].peak, &peak, cur)) {}
    atomic_fetch_add(&g_devmem_bytes, size);
    atomic_fetch_add(&g_devmem_allocs, 1);
    uint64_t all = atomic_load(&g_devmem_bytes), pk = atomic_load(&g_devmem_peak_bytes);
    while (all > pk && !atomic_compare_exchange_weak(&g_devmem_peak_bytes, &pk, all)) {}
}
// 统一分配入口：成功即按类别记账并登记句柄，失败计入失败次数。
// 所有释放必须走 fd_devmem_release，否则账面 live 会与驱动真实存活不一致（延迟退休路径曾绕过它）。
static VkResult fd_devmem_alloc(VkDevice dev, const VkMemoryAllocateInfo *ai, VkDeviceMemory *mem, int cat) {
    VkResult r = vkAllocateMemory(dev, ai, NULL, mem);
    if (r == VK_SUCCESS && *mem) {
        fd_memcat_add(cat, (uint64_t)ai->allocationSize);
        fd_devmem_register(*mem, (uint64_t)ai->allocationSize, cat);
    } else {
        atomic_fetch_add(&g_devmem_fails, 1);
    }
    return r;
}
static void fd_memcat_sub(int cat, uint64_t size);   // 定义在后面（释放路径要先用）
// 唯一释放入口（设备句柄版）：先按登记表扣减类别与存活数，再真正释放。
// 任何直接 vkFreeMemory 都会让账面 live 偏大——延迟退休路径就是这样漏掉的。
static void fd_devmem_release_dev(VkDevice dev, VkDeviceMemory mem) {
    if (!mem) return;
    uint64_t size = 0;
    int cat = fd_devmem_unregister(mem, &size);
    if (cat >= 0 && cat < FD_MEM_CAT_N) fd_memcat_sub(cat, size);
    if (dev) vkFreeMemory(dev, mem, NULL);
}
static void fd_devmem_release(struct s2a_vk_device *d, VkDeviceMemory mem) {
    fd_devmem_release_dev(d ? d->device : VK_NULL_HANDLE, mem);
}

static void fd_memcat_sub(int cat, uint64_t size) {
    atomic_fetch_sub(&g_memcat[cat].cur, size);
    atomic_fetch_add(&g_memcat[cat].frees, 1);
    atomic_fetch_sub(&g_devmem_bytes, size);
    atomic_fetch_add(&g_devmem_frees, 1);
}

// 写跟踪是否已不可靠（弱符号：宿主单测链接时可能没有该模块）。
extern int nvn_dirty_tracking_unreliable(void) __attribute__((weak));
extern void nvn_dirty_set_chunk_pages(uint32_t) __attribute__((weak));
// 保护分块大小（实验参数）：debug.s2a.chunk_kb，默认 1024 KiB。必须与 poolahb 的 VMA 预算同一口径。
static void fd_apply_chunk_kb(void) {
    if (!nvn_dirty_set_chunk_pages) return;
    char b[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.chunk_kb", b);
    uint32_t kb = (b[0] && atoi(b) > 0) ? (uint32_t)atoi(b) : 1024u;
    const uint32_t pg = (uint32_t)(sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096);
    uint32_t pages = (kb * 1024u) / pg;
    nvn_dirty_set_chunk_pages(pages ? pages : 1);
}
// VkResult 定性：对象数上限与内存类型耗尽要分开报，否则“撞上限”只是猜测。
static const char *fd_vkresult_name(VkResult r) {
    switch ((int)r) {
        case (int)VK_SUCCESS: return "SUCCESS";
        case (int)VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
        case (int)VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
        case (int)VK_ERROR_TOO_MANY_OBJECTS: return "TOO_MANY_OBJECTS";
        case (int)VK_ERROR_MEMORY_MAP_FAILED: return "MEMORY_MAP_FAILED";
        case (int)VK_ERROR_FRAGMENTED_POOL: return "FRAGMENTED_POOL";
        case (int)VK_ERROR_OUT_OF_POOL_MEMORY: return "OUT_OF_POOL_MEMORY";
        default: return "其他";
    }
}
static void fd_mem_report(const char *where) {
    // 进程视角：地址空间与常驻是判断“映射耗尽 / 物理内存压力”的第一手数据。
    uint64_t vm_size_kb = 0, vm_rss_kb = 0, vm_peak_kb = 0, vm_swap_kb = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned long long v = 0;
            if (sscanf(line, "VmSize: %llu", &v) == 1) vm_size_kb = v;
            else if (sscanf(line, "VmRSS: %llu", &v) == 1) vm_rss_kb = v;
            else if (sscanf(line, "VmPeak: %llu", &v) == 1) vm_peak_kb = v;
            else if (sscanf(line, "VmSwap: %llu", &v) == 1) vm_swap_kb = v;
        }
        fclose(f);
    }
    uint64_t tex_calls = 0, tex_bytes = 0, tex_live = 0, tex_n = 0;
    uint64_t tex_devmem_calls = 0, tex_devmem_live = 0, tex_dedicated = 0;
    uint64_t tex_reup_calls = 0, tex_reup_full = 0, tex_reup_pages = 0;
    {
        extern void nvn_vk_texcache_mem(uint64_t *, uint64_t *, uint64_t *, uint64_t *) __attribute__((weak));
        if (nvn_vk_texcache_mem) nvn_vk_texcache_mem(&tex_calls, &tex_bytes, &tex_live, &tex_n);
        extern void nvn_vk_texcache_devmem(uint64_t *, uint64_t *, uint64_t *) __attribute__((weak));
        if (nvn_vk_texcache_devmem) nvn_vk_texcache_devmem(&tex_devmem_calls, &tex_devmem_live, &tex_dedicated);
        extern void nvn_vk_texcache_reupload(uint64_t *, uint64_t *, uint64_t *) __attribute__((weak));
        if (nvn_vk_texcache_reupload) nvn_vk_texcache_reupload(&tex_reup_calls, &tex_reup_full, &tex_reup_pages);
    }
    // Linux 侧：真实 VMA 数 + 按类别拆开（进程自己读 /proc/self/maps 是允许的）。
    // 只数总数无法定位「谁在涨」；映射耗尽是 mmap 失败的直接原因，必须看类别构成。
    uint64_t vma = 0;
    uint64_t cat_n[10] = {0}, cat_b[10] = {0};
    uint64_t exec_n = 0, anon_exec_n = 0;
    static const char *const cat_name[10] = {
        "匿名", "anon名", "heap/stack", "/dev", "memfd", "/system", "/data", "/apex", "/vendor", "其他"
    };
    {
        int sample_other = 0, sample_dev = 0, sample_memfd = 0;
        FILE *m = fopen("/proc/self/maps", "r");
        if (m) {
            char buf[512];
            while (fgets(buf, sizeof buf, m)) {
                unsigned long long lo = 0, hi = 0;
                char perms[8] = {0};
                if (sscanf(buf, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
                vma++;
                const char *path = strchr(buf, '/');
                const char *anon = strstr(buf, "[anon:");
                int c;
                if (anon) c = 1;
                else if (!path) c = strstr(buf, "[heap]") || strstr(buf, "[stack") ? 2 : 0;
                else if (!strncmp(path, "/dev/", 5)) c = 3;
                else if (!strncmp(path, "/memfd:", 7)) c = 4;
                else if (!strncmp(path, "/system/", 8)) c = 5;
                else if (!strncmp(path, "/data/", 6)) c = 6;
                else if (!strncmp(path, "/apex/", 6)) c = 7;
                else if (!strncmp(path, "/vendor/", 8)) c = 8;
                else c = 9;
                cat_n[c]++;
                cat_b[c] += (hi > lo ? hi - lo : 0);
                const int is_exec = perms[2] == 'x';
                if (is_exec) { exec_n++; if (c == 0 || c == 1) anon_exec_n++; }
                // 样本：最大的类别（其他）和 /dev、memfd 各留几行，用来判断这堆小映射到底是什么。
                if (sample_other < 3 && c == 9) { FD_LOG("mem@%s: VMA样-其他: %s", where, buf); sample_other++; }
                else if (sample_dev < 2 && c == 3) { FD_LOG("mem@%s: VMA样-dev: %s", where, buf); sample_dev++; }
                else if (sample_memfd < 2 && c == 4) { FD_LOG("mem@%s: VMA样-memfd: %s", where, buf); sample_memfd++; }
            }
            fclose(m);
        }
    }
    // 存活分配对象总数 = 登记表（快照/arena/其它）+ 纹理侧（自己记）——两者合起来才和 4096 可比。
    FD_LOG("mem@%s: VMA=%llu 个 | Vulkan 分配 live=%llu(登记=%llu+纹理=%llu) 累计=%llu 释放=%llu 失败=%llu | 上限=%u",
           where, (unsigned long long)vma,
           (unsigned long long)(atomic_load(&g_devmem_reg_n) + tex_devmem_live),
           (unsigned long long)atomic_load(&g_devmem_reg_n), (unsigned long long)tex_devmem_live,
           (unsigned long long)atomic_load(&g_devmem_allocs),
           (unsigned long long)atomic_load(&g_devmem_frees),
           (unsigned long long)atomic_load(&g_devmem_fails), (unsigned)g_max_mem_allocs);
    for (int c = 0; c < FD_MEM_CAT_N; c++) {
        uint64_t cur = atomic_load(&g_memcat[c].cur), pk = atomic_load(&g_memcat[c].peak);
        if (!cur && !pk && !atomic_load(&g_memcat[c].allocs)) continue;
        FD_LOG("mem@%s: [%s] 当前=%lluMB 峰值=%lluMB 分配=%llu 释放=%llu",
               where, fd_mem_cat_name[c], (unsigned long long)(cur >> 20), (unsigned long long)(pk >> 20),
               (unsigned long long)atomic_load(&g_memcat[c].allocs),
               (unsigned long long)atomic_load(&g_memcat[c].frees));
    }
    for (int c = 0; c < 10; c++) {
        if (!cat_n[c]) continue;
        FD_LOG("mem@%s: VMA[%s]=%llu 个 / %lluMB", where, cat_name[c],
               (unsigned long long)cat_n[c], (unsigned long long)(cat_b[c] >> 20));
    }
    FD_LOG("mem@%s: 可执行 VMA=%llu 个（其中匿名 %llu 个，JIT/mprotect 拆分的典型来源）", where,
           (unsigned long long)exec_n, (unsigned long long)anon_exec_n);
    FD_LOG("mem@%s: VmSize=%lluMB VmPeak=%lluMB VmRSS=%lluMB VmSwap=%lluMB | 纹理=%llu张/%lluMB | 快照=%lluMB",
           where, (unsigned long long)(vm_size_kb >> 10), (unsigned long long)(vm_peak_kb >> 10),
           (unsigned long long)(vm_rss_kb >> 10), (unsigned long long)(vm_swap_kb >> 10),
           (unsigned long long)tex_n, (unsigned long long)(tex_live >> 20),
           (unsigned long long)(atomic_load(&g_memcat[FD_MEM_SNAP].cur) >> 20));
    FD_LOG("mem@%s: 纹理重传=%llu 次 | 整张=%lluMB 按脏页=%lluMB | 跟踪不可靠=%d",
           where, (unsigned long long)tex_reup_calls, (unsigned long long)(tex_reup_full >> 20),
           (unsigned long long)(tex_reup_pages >> 20),
           (int)(nvn_dirty_tracking_unreliable ? nvn_dirty_tracking_unreliable() : 0));
    FD_LOG("mem@%s: 快照池 %d/%d 槽 占=%lluMB 预算=%lluMB 淘汰=%llu 次", where, g_snap_pool_n, (int)FD_SNAP_POOL,
           (unsigned long long)(g_snap_pool_bytes >> 20), (unsigned long long)(fd_snap_budget_bytes() >> 20),
           (unsigned long long)g_snap_evicted);

}

// 窗口计数（每次取数由 take_open_window 清零并打印，用来验证调用数是否真的减少）
static uint64_t g_desc_alloc_calls, g_desc_update_calls, g_desc_writes;
// 累计计数（永不清零）：周期读数用它，窗口快照另有自己那份，两者不互相踩出下溢。
static uint64_t g_desc_tot_alloc, g_desc_tot_update, g_desc_tot_writes;
static _Atomic uint64_t g_desc_tot_alloc_ns, g_desc_tot_write_ns;
// 描述符/内存周期读数开关（默认关：读 /proc 是同步 IO，会污染帧时间）。
static int fd_desc_stats_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.desc_stats", b);
        v = (b[0] && atoi(b) != 0) ? 1 : 0;
    }
    return v;
}
static uint64_t g_frame_id = 1;      // 当前录制帧序号：时间戳待写项按它归属到帧
// 采样器缓存：同一组 NVN sampler 参数复用同一个 VkSampler，避免逐笔 vkCreateSampler。
static struct nvn_vk_sampler_cache g_samp_cache;
static int g_samp_cache_ready;
// 逐帧 id→对象缓存：世界一帧 480+ 笔 draw，同一批纹理/采样器反复绑定。注册表锁 + 哈希查找
// 每笔 ~60 µs；缓存只在本帧有效（纹理 pin 到 fence，unpin 后槽可能被淘汰/复用）。
#define FD_TEX_LOOKUP 2048
#define FD_SAMP_LOOKUP 512
static struct { int id; struct nvn_vk_tex *tex; } g_tex_lookup[FD_TEX_LOOKUP];
static struct { int id; samp_par par; } g_samp_lookup[FD_SAMP_LOOKUP];
static _Atomic uint64_t g_lookup_gen;
static void fd_lookup_clear(void) {
    memset(g_tex_lookup, 0, sizeof g_tex_lookup);
    memset(g_samp_lookup, 0, sizeof g_samp_lookup);
    atomic_store(&g_lookup_gen, s2a_vk_adapter_registry_generation());
}
static void fd_lookup_ensure(void) {
    uint64_t g = s2a_vk_adapter_registry_generation();
    if (atomic_load(&g_lookup_gen) != g) fd_lookup_clear();
}
static void fd_samp_cache_ensure(struct s2a_vk_device *d) {
    if (g_samp_cache_ready && g_samp_cache.d == d) return;
    if (g_samp_cache_ready) nvn_vk_sampler_cache_destroy(&g_samp_cache);
    nvn_vk_sampler_cache_init(&g_samp_cache, d);
    g_samp_cache_ready = 1;
}

static void fd_prog_obj_destroy(struct s2a_vk_device *d, struct fd_prog_obj *o) {
    if (!o->used) return;
    if (d && o->device == d->device) {
        if (o->vs) vkDestroyShaderModule(d->device, o->vs, NULL);
        if (o->fs) vkDestroyShaderModule(d->device, o->fs, NULL);
        for (int i = 0; i < 4; i++) if (o->sets[i]) vkDestroyDescriptorSetLayout(d->device, o->sets[i], NULL);
        if (o->playout) vkDestroyPipelineLayout(d->device, o->playout, NULL);
    }
    if (o->has_prog) {
        uint64_t b = 0;
        for (int i = 0; i < FD_MAX_STAGES; i++) b += o->prog.stage[i].spirv_bytes;
        atomic_fetch_sub(&g_prog_owned_bytes, b);
        nvn_vk_program_free(&o->prog);
    }
    memset(o, 0, sizeof *o);
}

// 只查不建：draw 先按完整内容 SHA 找对象；命中且持有解析结果就直接复用，跳过解析缓存。
static struct fd_prog_obj *fd_prog_obj_find(struct s2a_vk_device *d, const uint8_t sha[32]) {
    if (!g_prog_obj_idx_ready) { for (int i = 0; i < 256; i++) g_prog_obj_idx[i] = -1; g_prog_obj_idx_ready = 1; }
    int16_t s = g_prog_obj_idx[sha[0]];
    if (s >= 0 && s < FD_PROG_OBJ) {
        struct fd_prog_obj *o = &g_prog_obj[s];
        if (o->used && o->device == d->device && !memcmp(o->sha, sha, 32)) { o->last_use = g_frame_seq; return o; }
    }
    for (int i = 0; i < FD_PROG_OBJ; i++) {
        struct fd_prog_obj *o = &g_prog_obj[i];
        if (o->used && o->device == d->device && !memcmp(o->sha, sha, 32)) {
            g_prog_obj_idx[sha[0]] = (int16_t)i;
            o->last_use = g_frame_seq;
            return o;
        }
    }
    return NULL;
}
// 把解析结果交给对象（所有权转移）；对象已有一份时调用方负责释放新的一份。
static void fd_prog_obj_attach(struct fd_prog_obj *o, const struct fd_program *p) {
    if (!o || !p || o->has_prog) return;
    o->prog = *p;
    o->has_prog = 1;
    uint64_t b = 0;
    for (int i = 0; i < FD_MAX_STAGES; i++) b += p->stage[i].spirv_bytes;
    uint64_t total = atomic_fetch_add(&g_prog_owned_bytes, b) + b;
    if (total > (64ull << 20)) {
        static int noted;
        if (!noted) { noted = 1; FD_LOG("program objects hold %llu MB of parsed programs (budget watch)", (unsigned long long)(total >> 20)); }
    }
}
// 从解析缓存取走一份（调用方拿到所有权，缓存槽清零不再释放它）。
static int fd_prog_cache_take(const uint8_t sha[32], struct fd_program *out) {
    for (int i = 0; i < g_prog_n; i++) {
        if (!g_prog_cache[i].used || memcmp(g_prog_cache[i].sha, sha, 32)) continue;
        *out = g_prog_cache[i].prog;
        if (g_prog_idx[sha[0]] == (int16_t)i) g_prog_idx[sha[0]] = -1;
        memset(&g_prog_cache[i], 0, sizeof g_prog_cache[i]);
        return 1;
    }
    return 0;
}
static struct fd_prog_obj *fd_prog_obj_get(struct s2a_vk_device *d, const struct fd_program *prog,
                                           const uint8_t sha[32], char *why, size_t why_size) {
    if (!g_prog_obj_idx_ready) { for (int i = 0; i < 256; i++) g_prog_obj_idx[i] = -1; g_prog_obj_idx_ready = 1; }
    {
        int16_t s = g_prog_obj_idx[sha[0]];
        if (s >= 0 && s < FD_PROG_OBJ) {
            struct fd_prog_obj *o = &g_prog_obj[s];
            if (o->used && o->device == d->device && !memcmp(o->sha, sha, 32)) {
                o->last_use = g_frame_seq;
                atomic_fetch_add_explicit(&g_prog_obj_hits, 1, memory_order_relaxed);
                if (o->failed) { fail(why, why_size, "program object creation failed earlier for this content"); return NULL; }
                return o;
            }
        }
    }
    for (int i = 0; i < FD_PROG_OBJ; i++) {
        struct fd_prog_obj *o = &g_prog_obj[i];
        if (o->used && o->device == d->device && !memcmp(o->sha, sha, 32)) {
            g_prog_obj_idx[sha[0]] = (int16_t)i;
            o->last_use = g_frame_seq;
            atomic_fetch_add_explicit(&g_prog_obj_hits, 1, memory_order_relaxed);
            if (o->failed) { fail(why, why_size, "program object creation failed earlier for this content"); return NULL; }
            return o;
        }
    }
    int slot = -1;
    for (int i = 0; i < FD_PROG_OBJ; i++) if (!g_prog_obj[i].used) { slot = i; break; }
    if (slot < 0) {
        uint64_t oldest = ~0ull;
        for (int i = 0; i < FD_PROG_OBJ; i++) {
            struct fd_prog_obj *o = &g_prog_obj[i];
            if (o->users || o->last_use == g_frame_seq) continue;   // 命令/管线仍可能引用
            if (o->last_use < oldest) { oldest = o->last_use; slot = i; }
        }
        if (slot < 0) { fail(why, why_size, "program object cache is full (all entries in use this frame)"); return NULL; }
        fd_prog_obj_destroy(d, &g_prog_obj[slot]);
        atomic_fetch_add_explicit(&g_prog_obj_evicts, 1, memory_order_relaxed);
    }
    struct fd_prog_obj *o = &g_prog_obj[slot];
    memset(o, 0, sizeof *o);
    o->device = d->device;
    memcpy(o->sha, sha, 32);
    o->attr_mask = prog->stage[0].attr_mask;
    o->last_use = g_frame_seq;
    // 所有对象一次建齐；任一失败就完整回滚，不留下半套对象。
    VkShaderModuleCreateInfo vci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vci.codeSize = prog->stage[0].spirv_bytes; vci.pCode = (const uint32_t *)prog->stage[0].spirv;
    VkShaderModuleCreateInfo fci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fci.codeSize = prog->stage[1].spirv_bytes; fci.pCode = (const uint32_t *)prog->stage[1].spirv;
    int feature_rejected = 0;
    VkResult vertex_rc = s2a_vk_create_checked_vertex(d->device,
        d->core_features.vertexPipelineStoresAndAtomics, &vci, &o->vs, &feature_rejected);
    if (feature_rejected) {
        uint64_t count = atomic_fetch_add(&g_storage_feature_rejects, 1) + 1;
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
            "【红】program %02x%02x%02x%02x missing vertexPipelineStoresAndAtomics (rejected #%llu)",
            sha[0], sha[1], sha[2], sha[3], (unsigned long long)count);
        o->used = 1; o->failed = 1;
        fail(why, why_size, "program requires vertexPipelineStoresAndAtomics");
        return NULL;
    }
    if (vertex_rc != VK_SUCCESS ||
        vkCreateShaderModule(d->device, &fci, NULL, &o->fs) != VK_SUCCESS) {
        o->used = 1; o->failed = 1;
        fail(why, why_size, "vkCreateShaderModule failed for program content");
        return NULL;
    }
    for (int set = 0; set < 4; set++) {
        VkDescriptorSetLayoutBinding list[FD_MAX_BINDINGS];
        uint32_t n = 0;
        for (int i = 0; i < prog->binding_count; i++) {
            if (prog->bindings[i].set != set) continue;
            VkDescriptorSetLayoutBinding *b = &list[n++];
            memset(b, 0, sizeof *b);
            b->binding = (uint32_t)prog->bindings[i].binding;
            b->descriptorCount = 1;
            b->stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            b->descriptorType = prog->bindings[i].kind == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                                prog->bindings[i].kind == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                                prog->bindings[i].kind == 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = n; li.pBindings = list;
        if (vkCreateDescriptorSetLayout(d->device, &li, NULL, &o->sets[set]) != VK_SUCCESS) {
            o->used = 1; o->failed = 1;
            fail(why, why_size, "vkCreateDescriptorSetLayout failed for program content");
            return NULL;
        }
    }
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 4; pli.pSetLayouts = o->sets;
    if (vkCreatePipelineLayout(d->device, &pli, NULL, &o->playout) != VK_SUCCESS) {
        o->used = 1; o->failed = 1;
        fail(why, why_size, "vkCreatePipelineLayout failed for program content");
        return NULL;
    }
    o->used = 1;
    g_prog_obj_idx[sha[0]] = (int16_t)slot;
    atomic_fetch_add_explicit(&g_prog_obj_created, 1, memory_order_relaxed);
    return o;
}

// NVN 混合因子/方程 → Vulkan（枚举值同 nvn_mtl.m 的 bfac/bop）。
// NVN 深度比较函数（1 Never … 8 Always，与 alpha func 同序）→ Vulkan。
static VkCompareOp nvk_compare_op(uint32_t f) {
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
// NVN 模板操作 → Vulkan（1 Keep … 8 DecrWrap）。
static VkStencilOp nvk_stencil_op(uint32_t o) {
    switch (o) {
        case 1: return VK_STENCIL_OP_KEEP;
        case 2: return VK_STENCIL_OP_ZERO;
        case 3: return VK_STENCIL_OP_REPLACE;
        case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 6: return VK_STENCIL_OP_INVERT;
        case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}
// ---- 诊断：固定一笔 draw 的完整输入/输出转储（debug.s2a.trace=<requestId 前缀>） ----
// 只读真实快照与真实 GPU 内容，不改变绘制语义；一次进程只转储一笔。
struct fd_trace {
    int on;
    char dir[PROP_VALUE_MAX];
    char tag[40];
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize cap;
    uint32_t bpp;
    int ord;
    int tex_dumped;
    uint32_t tex_ids[8];
};

static _Atomic uint64_t g_draw_us_sum;
static _Atomic uint32_t g_draw_n;
static _Atomic uint64_t g_us_modules, g_us_layouts, g_us_pass, g_us_pipe, g_us_record, g_us_wait;
static _Atomic uint64_t g_us_sup, g_us_res, g_us_vb, g_us_desc, g_us_cmd;
// 精确账（ns 累加，打印时除以 n 换成 0.1µs）：旧的 g_us_* 是逐笔整数微秒，10 项各自向下取整，
// 真机实测把「单笔 15.7µs」的账压成 8µs，看起来像有 7µs 残差，实际只是量化误差。
static _Atomic uint64_t g_ns_entry, g_ns_modules, g_ns_layouts, g_ns_pass, g_ns_pipe;
static _Atomic uint64_t g_ns_n;   // 精确账累计了多少笔
static _Atomic uint64_t g_ns_pass_plan, g_ns_pass_fb, g_ns_pass_open;
static _Atomic uint64_t g_ns_pass_plan_n, g_ns_pass_fb_n, g_ns_pass_open_n;
static _Atomic uint64_t g_ns_open_call, g_ns_open_call_n;   // fd_pass_begin 调用本身的实测
// pass 段（body 里 2.29 us/笔 那一段）的逐笔细分。上面 g_ns_pass_plan/fb/open 只在**开新 pass 时**
// 结算（分母 = 新开 pass 次数），不能拿来解释每笔平均；这两个计数器才是逐笔都累加的：
//   fb   = fd_feedback_tex 的反馈快照检查（每笔都做）
//   cont = fd_pass_continues 附件延续判定（每笔都做）
//   open = 非延续时走开 pass 路径（close + rp 查找 + framebuffer + clear + begin，只有 35 次/帧）
static _Atomic uint64_t g_ns_pd_fb, g_ns_pd_cont, g_ns_pd_open, g_ns_pd_open_n, g_ns_pd_n;
// rest 段细分：与父项同一批调用（分母 = 本窗新开 pass 次数），另给累计 ns 与次数，便于核对覆盖。
static _Atomic uint64_t g_rest_gpuarm_ns, g_rest_query_ns, g_rest_precopy_ns;
static _Atomic uint64_t g_rest_barrier_ns, g_rest_beginrp_ns;
static _Atomic uint64_t g_rest_sub_n;        // 本窗 rest 子项的覆盖次数（= 新开 pass 数）
static _Atomic uint64_t g_win_draws;         // 本窗经过 fd_note_draw_stats 的单笔数
static _Atomic uint64_t g_res_ub_c, g_res_tex_c;                       // 子项自己的次数
static _Atomic uint64_t g_desc_alloc_c, g_desc_loop_c, g_desc_write_c, g_desc_pool_c;
static _Atomic uint64_t g_ns_sup, g_ns_res, g_ns_vb, g_ns_desc, g_ns_cmd, g_ns_total;
// “pass” 段再拆两刀：反馈采样判定（逐 draw、按 program 的纹理绑定扫一遍）与 pass 对象配置
// （收尾旧 pass / 取缓存 pass / 建 framebuffer / begin）。24us/draw 里是哪一半，决定先改哪里。
static _Atomic uint64_t g_us_fbtex, g_us_pcfg, g_fbtex_n, g_pcfg_n;
// 本窗内 blend_en=1 但因子等价于不混合的 draw 数：关掉 blendEnable 是纯省，不改像素。
static _Atomic uint64_t g_blend_noop;
// 描述符内部再拆一刀：分配集 vs 写集。没有这两个读数就只能猜，D1/D2 会改错地方。
static _Atomic uint64_t g_desc_alloc_ns, g_desc_write_ns, g_desc_loop_ns, g_desc_pool_ns;
// res 阶段内部再拆：uniform/storage 解析 vs 纹理/采样器解析。res 是当前单笔最大项，
// 不分清这两半就只能猜该优化哪边。
static _Atomic uint64_t g_res_ub_ns, g_res_tex_ns, g_res_ssbo_ns;
// 纹理绑定这一支再拆三刀（诊断口径，debug.s2a.perf_timing=1 才累加）：
//   注册表视图查询（外层互斥 + 开放寻址）/ 采样器缓存查询（曾是 O(槽数) 线性扫描）/ pin 保活。
// 不拆就只能看到“resT 很大”，改错地方。绑定数（UBO/纹理/合计）也一起数，用来把时间换成“每绑定”。
static _Atomic uint64_t g_res_view_ns, g_res_samp_ns, g_res_pin_ns;
// 绑定数只在绘制线程上增减、也只在绘制线程上读，普通计数即可（原子 RMW 是每笔 3 次的固定开销）。
static uint64_t g_bind_ub_n, g_bind_tex_n, g_bind_ssbo_n, g_bind_n;
// 归因（照 xbg 的 key_diff 思路）：连续两笔之间纹理句柄数组是否相同、绑定数是否变化。
// 不猜“为什么 resT 高”，先数清楚重解析到底有没有必要。
static _Atomic uint64_t g_key_same, g_key_diff, g_key_draws;
static uint32_t g_prev_tex_sig[2][32];
static uint8_t g_prev_tex_present[2][32];
static int g_prev_tex_valid;


static VkPipelineCache g_pipeline_cache;
// 驱动缓存落盘：记录创建时的设备/UUID/目录，release_device（设备销毁前）取数据写回。
static struct s2a_vk_device *g_driver_dev;
static uint8_t g_driver_uuid[16];
static int g_driver_uuid_ok;
static char g_driver_dir[480];

static VkPipelineCache fd_pipeline_cache(struct s2a_vk_device *d) {
    if (!g_pipeline_cache) {
        void *initial = NULL;
        size_t initial_bytes = 0;
        const char *dir = getenv("S2A_DIRECT_CACHE_DIR");
        if (d && dir && dir[0]) {
            snprintf(g_driver_dir, sizeof g_driver_dir, "%s", dir);
            memcpy(g_driver_uuid, d->properties.pipelineCacheUUID, sizeof g_driver_uuid);
            g_driver_uuid_ok = 1;
            nvm_manifest_configure(dir, g_driver_uuid);   // 设备侧配方清单：读上次的 PSO 集合
            char why[160] = "";
            int rc = nvk_driver_cache_read(dir, g_driver_uuid, &initial, &initial_bytes, why, sizeof why);
            if (rc == 0)
                FD_LOG("driver cache: loaded %zu bytes for UUID %02x%02x%02x%02x…",
                       initial_bytes, g_driver_uuid[0], g_driver_uuid[1], g_driver_uuid[2], g_driver_uuid[3]);
            else if (rc < 0)
                FD_LOG("driver cache: rejected (%s); starting with an empty cache", why[0] ? why : "corrupt");
        }
        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        ci.initialDataSize = initial_bytes;
        ci.pInitialData = initial;
        VkResult vr = vkCreatePipelineCache(d->device, &ci, NULL, &g_pipeline_cache);
        if (vr != VK_SUCCESS && initial_bytes) {
            // 驱动不接受这份数据：按规范退回空缓存，不能把失败留成“没有 pipeline cache”。
            FD_LOG("driver cache: driver rejected %zu bytes (VkResult %d); retrying empty", initial_bytes, (int)vr);
            ci.initialDataSize = 0; ci.pInitialData = NULL;
            vr = vkCreatePipelineCache(d->device, &ci, NULL, &g_pipeline_cache);
        }
        free(initial);
        if (vr != VK_SUCCESS) g_pipeline_cache = VK_NULL_HANDLE;
        fd_apply_chunk_kb();   // 保护分块（必须在第一次纹理上传前设置）
        {
            // 设备分配对象上限：撞上限与“物理内存不足”是两种完全不同的失败，读数必须能区分。
            VkPhysicalDeviceProperties props;
            memset(&props, 0, sizeof props);
            if (d->physical) {
                vkGetPhysicalDeviceProperties(d->physical, &props);
                g_max_mem_allocs = props.limits.maxMemoryAllocationCount;
            }
        }
        g_driver_dev = d;
    }
    return g_pipeline_cache;
}

// 管线失败键（有界）：同一 program sha 已经失败过就不再重试，避免每帧撞同一面墙。
// 记录失败原因；上层据此走“受控收局”，而不是无限重试或静默跳过绘制。
#define FD_PIPE_FAIL_KEYS 256
static struct { uint8_t sha[32]; int used; int bindings; } g_pipe_fail[FD_PIPE_FAIL_KEYS];
static int g_pipe_fail_n;
static _Atomic int g_pipe_fail_hits;
static void fd_pipe_note_failure(const uint8_t sha[32], int bindings) {
    for (int i = 0; i < g_pipe_fail_n; i++)
        if (!memcmp(g_pipe_fail[i].sha, sha, 32)) return;
    if (g_pipe_fail_n < FD_PIPE_FAIL_KEYS) {
        memcpy(g_pipe_fail[g_pipe_fail_n].sha, sha, 32);
        g_pipe_fail[g_pipe_fail_n].bindings = bindings;
        g_pipe_fail[g_pipe_fail_n].used = 1;
        g_pipe_fail_n++;
    }
}
static int fd_pipe_known_failure(const uint8_t sha[32]) {
    for (int i = 0; i < g_pipe_fail_n; i++)
        if (g_pipe_fail[i].used && !memcmp(g_pipe_fail[i].sha, sha, 32)) { atomic_fetch_add(&g_pipe_fail_hits, 1); return 1; }
    return 0;
}

#define FD_PIPE_CACHE 1024
struct fd_pipe_slot {
    int used;
    uint64_t key;                     // 64 位 hash：只作预筛
    struct nvk_pipe_recipe recipe;    // 完整规范配方：命中判据（逐字节相等）
    VkPipeline pipe;
    struct fd_prog_obj *obj;       // 程序对象（模块/布局/layout）由对象表拥有；本槽只借引用
    uint64_t last_use;
};
static struct fd_pipe_slot g_pipe_cache[FD_PIPE_CACHE];
// 直接映射索引：pipe_key 只作预筛，命中仍由 nvk_pipe_recipe_hit 逐字节判。
// 逐笔线性扫 1024 槽是管线段的主要来源（真机 A/B：管线段 3.79 → 1.37 us/笔，exec −1.9 ms）；
// 索引未命中或已过期（同槽被淘汰、不同 key 落到同一桶）一律回退线性扫，所以索引错只损失速度、不改判据。
#define FD_PIPE_INDEX 8192
static int32_t g_pipe_index[FD_PIPE_INDEX];
static void fd_pipe_index_reset(void) { for (int i = 0; i < FD_PIPE_INDEX; i++) g_pipe_index[i] = -1; }
static inline int32_t *fd_pipe_index_slot(uint64_t key) { return &g_pipe_index[(uint32_t)(key ^ (key >> 29)) & (FD_PIPE_INDEX - 1)]; }
static uint64_t g_pipe_clock;
static _Atomic uint64_t g_pipe_hits, g_pipe_misses, g_pipe_evicts;
// 绘制路径上真实 vkCreateGraphicsPipelines 的次数/墙钟（与 worker 的 longframe 计数分开：
// 预热创建也走同一函数，但按调用方分别记账）。
static _Atomic uint64_t g_draw_pipe_n, g_draw_pipe_ns;

// 管线对象缓存键：见 nvn_vk_pipe_recipe.h。64 位 hash 只作预筛，命中还要比较完整配方。
static uint64_t fd_pipe_key(const struct nvk_pipe_recipe *r) { return nvk_pipe_recipe_hash(r); }

// ---- 帧提交队列：draw 只录制并挂起，present 前一次 submit + 一次 fence ----
#define FD_FRAME_CMDS 1024
#define FD_FRAME_BUFS 8192
#define FD_FRAME_SAMPS 2048
#define FD_FRAME_POOLS 2048
#define FD_FRAME_FBS 2048
#define FD_FRAME_PASSES 2048
#define FD_FRAME_PEND 2048
// 已提交前缀的完成证明表：每批一个 fence。加载期（barrier/查询报告把一帧切出成百上千个批次，
// present 又被延迟）一帧内会积累远多于 136 个前缀；真机上 TOTK 加载期在 OP_BARRIER 处撞到
// “frame queue batch-fence table is full”（2026-11-06 22:25）。表项只是一个 VkFence 句柄（8 B），
// 放大到 512 后仍满才是真资源上限，那时明确失败。
#define FD_FRAME_BATCH_FENCES 512
static int64_t fd_now_ns(void);
// 命令缓冲复用（定义在 pass begin 前）：pass 收尾/错误路径更早用到。
static void fd_cmd_put(struct s2a_vk_device *d, VkCommandBuffer cmd);
static void fd_unpin_prefix(int n);
static void fd_pass_dbg_flush(void);
// pass 关闭原因窗口（诊断，debug.s2a.pass_close_reason=1）：帧完成点比定义点更早用到。
static void fd_pass_reason_tick(void);
// 批量提交 fence ring（debug.s2a.fence_ring=1，默认关）：槽位释放路径比定义点更早用到。
static VkFence fd_fence_acquire(VkDevice dev);
static void fd_fence_put_back(VkDevice dev, VkFence fence);
// GPU 逐 pass 计时（诊断，debug.s2a.gpu_time=1）：定义在 pass 缓存之后，pass 收尾更早用到。
static int64_t fd_stamp(void);   // 关诊断时返回 0（定义在后）
static void fd_gpu_pass_begin(struct s2a_vk_device *d, VkCommandBuffer cmd, uint32_t w, uint32_t h,
                              VkFormat cfmt, VkFormat dfmt, int depth_only);
static void fd_gpu_pass_end(VkCommandBuffer cmd);
// 可写 graphics storage buffer 的帧内影子：同帧复用、帧退休回写。Vulkan 没有“把 GPU 写直接
// 映射回 guest 内存”的借用语义，所以影子在 host-visible coherent 的 arena 上，fence 信号后
// （Vulkan 的 host 可用性保证）再按字节拷回 guest。
struct fd_ssbo_wb {
    VkBuffer buf; void *map; uint64_t guest; uint32_t off, size, writable;
    // 可写子区间：同一 guest 缓冲可能先被大范围只读绑定、再按子范围可写绑定（反之亦然）。
    // 回写只拷真正被可写绑定的区间，不把只读影子覆盖回 guest。
    uint32_t nwb;
    struct { uint32_t off, size; } wb[4];
};
struct fd_frame_q {
    struct s2a_vk_device *dev;
    VkCommandBuffer cmds[FD_FRAME_CMDS]; int ncmd;
    VkBuffer bufs[FD_FRAME_BUFS]; VkDeviceMemory mems[FD_FRAME_BUFS]; int nbuf;
    VkSampler samps[FD_FRAME_SAMPS]; int nsamp;
    VkDescriptorPool pools[FD_FRAME_POOLS]; int npool;
    VkFramebuffer fbs[FD_FRAME_FBS]; int nfb;
    // feedback 采样快照：本帧内“附件同时当采样源”的 draw 用；等这批命令有完成证明后再销毁。
    VkImage fbimg[FD_FRAME_FBS]; VkDeviceMemory fbmem[FD_FRAME_FBS]; VkImageView fbview[FD_FRAME_FBS];
    int nfbimg;
    VkRenderPass passes[FD_FRAME_PASSES]; int npass;
    // 被 LRU 淘汰但仍可能被本帧已录制命令引用的管线：等 fence 完成后才销毁。
    VkPipeline pend[FD_FRAME_PEND]; int npend;
    VkBuffer pbuf[FD_FRAME_CMDS]; VkDeviceMemory pmem[FD_FRAME_CMDS]; void *pmap[FD_FRAME_CMDS];
    uint32_t pw[FD_FRAME_CMDS], ph[FD_FRAME_CMDS], pbpp[FD_FRAME_CMDS], pcount[FD_FRAME_CMDS];
    char pid[FD_FRAME_CMDS][80];
    // 可写 graphics storage buffer：本帧的影子（arena 里 host-visible 一块）与退休回写账本。
    // 同一 guest 区间在本帧再次绑定必须复用同一影子，后一笔才能看到前一笔的 GPU 写。
    struct fd_ssbo_wb ssbo_wb[16];
    int n_ssbo_wb;
    // pass_probe：每个 pass 一份 5 点像素（每点最多 16 B，与 draw probe 同口径），
    // 本帧所有 pass 共用一个 host buffer；flush 时打印后释放。
    VkBuffer pp_buf; VkDeviceMemory pp_mem; void *pp_map; int pp_n;
    uint32_t pp_w[FD_FRAME_CMDS], pp_h[FD_FRAME_CMDS], pp_fmt[FD_FRAME_CMDS], pp_bpp[FD_FRAME_CMDS];
    char pp_req[FD_FRAME_CMDS][64];
    char pp_prog[FD_FRAME_CMDS][16];
    // 诊断：按程序前缀把某个 pass 的整幅颜色目标落盘（merged 路径安全：copy 在 end render pass 之后）。
    struct { VkBuffer buf; VkDeviceMemory mem; void *map; uint32_t w, h; int slot; } pd[8];
    int pd_n;
    VkFence fence;                       // 本次 flush 后缀的 fence；没有后缀时为 NULL
    int submitted_n;                     // 已经“提交成功”的 cmd 前缀（cmds[0..submitted_n)）；失败不推进
    // 早批次（每 8 个 pass 一次）各自的 fence：按提交顺序等待才是完成证明。已提交但未完成的前缀
    // 与“还没提交”的前缀必须分开，不能因为 ncmd==submitted_n 就当作已完成。
    VkFence batch_fence[FD_FRAME_BATCH_FENCES];
    uint64_t batch_seq[FD_FRAME_BATCH_FENCES];   // 对应批次的 arena 提交编号（0 = 无块）
    uint64_t suffix_seq;                          // 后缀提交的 arena 提交编号
    // 查询批次号：由查询模块分配，绝不复用 arena 提交编号（arena 无切片时 seal 返回 0，
    // 会把不同提交混成一个批次，页的归属就错了）。0 = 这批没有录制查询页。
    uint64_t batch_qid[FD_FRAME_BATCH_FENCES];
    uint64_t suffix_qid;                          // 后缀提交的查询批次号
    int nbatch_fence;
    VkDevice dev_handle;                 // 这条队列属于哪个 VkDevice：新设备不能释放/等待旧设备的对象
    // 录制/提交/完成状态：失败或超时后队列进入终态，资源保留（不释放、不再复用），
    // 由上层看到失败并结束本会话；下一次启动是新的队列/设备，不做“半恢复”。
    int state;
    char fail[200];      // 首个失败原因（不覆盖）
};
// 状态：空闲 → 已录制 → 已提交 → 已完成；失败/超时为终态。
enum { FD_FQ_IDLE = 0, FD_FQ_RECORDED, FD_FQ_SUBMITTED, FD_FQ_COMPLETED, FD_FQ_FAILED, FD_FQ_TIMEOUT };
static void fd_ssbo_wb_writeback(struct fd_frame_q *q);   // 帧退休回写可写 storage buffer 影子

static struct fd_frame_q g_fq;
// 查询批次号：由这里统一分配，绝不用 arena 提交编号（arena 无切片时 seal 返回 0，
// 会把不同提交混成一个批次，页的归属就错了）。
static uint64_t g_query_batch_id;
static uint64_t fd_query_next_batch(void) { if (!g_query_batch_id) g_query_batch_id = 1; return ++g_query_batch_id; }
// 一个提交批次拿到完成证明后，把它的查询页结果取走并回收页。同步 flush 与延迟退休都走这里：
// 只调 nvn_vk_query_resolve() 只结算报告、不读页也不回收页，页会一直攒到池满。
static void fd_fq_fail(int state, const char *why);   // 定义在后：批次退休失败要进终态
static int fd_query_retire_cb(uint64_t qid, void *ctx) {
    (void)ctx;
    char qwhy[200] = "";
    if (nvn_vk_query_batch_completed(qid, qwhy, sizeof qwhy) != 0) {
        FD_LOG("query retire: batch %llu results unavailable: %s",
               (unsigned long long)qid, qwhy[0] ? qwhy : "no reason");
        fd_fq_fail(FD_FQ_FAILED, qwhy[0] ? qwhy : "query results unavailable for a completed batch");
        return -1;
    }
    return 0;
}

// pass 探针输出：正常路径在 flush 尾部调；延迟 present 下队列在退休点才完成，这里也要能打印。
static void fd_pp_print_q(struct fd_frame_q *q) {
    if (!q || q->pp_n <= 0 || !q->pp_map) return;
    const uint8_t *p = (const uint8_t *)q->pp_map;
    for (int i = 0; i < q->pp_n; i++) {
        const uint8_t *px = p + (size_t)i * 5 * 16;
        uint32_t bpp = q->pp_bpp[i];
        FD_LOG("pass probe frame=%llu #%d rt=%ux%u fmt=0x%x bpp=%u req=%s prog=%s px=[%08x %08x %08x %08x %08x]",
               (unsigned long long)g_frame_seq, i, q->pp_w[i], q->pp_h[i], q->pp_fmt[i], bpp,
               q->pp_req[i][0] ? q->pp_req[i] : "-",
               q->pp_prog[i][0] ? q->pp_prog[i] : "-",
               bpp == 4 ? *(const uint32_t *)(px + 0 * 16) : 0,
               bpp == 4 ? *(const uint32_t *)(px + 1 * 16) : 0,
               bpp == 4 ? *(const uint32_t *)(px + 2 * 16) : 0,
               bpp == 4 ? *(const uint32_t *)(px + 3 * 16) : 0,
               bpp == 4 ? *(const uint32_t *)(px + 4 * 16) : 0);
    }
}

// ---- 延迟 present（debug.s2a.present_defer=1）------------------------------------------
// 目的：present 不再同步等本帧 GPU 完成，把这一等留给下一帧——CPU 立刻去录下一帧，
// GPU 的尾部在下一帧录制期间收尾。资源（命令缓冲/framebuffer/pool/arena/pin）不能提前释放：
// 帧结束时把整个队列快照进 slot，下一帧 present 开始前（或任何同步 flush 前）再等 fence、
// 释放、解 pin。只有一份在途帧，所以 2 个槽足够。
// arena / 描述符池按帧奇偶分两套（定义在下方；退休点更早用到，先声明）。
static void fd_arena_reset_parity(int p);
static uint64_t fd_arena_seal_submit(void);
static void fd_arena_complete(uint64_t seq);
static int g_arena_parity;   // 帧奇偶：arena/描述符池/GPU 帧查询池共用
static VkDescriptorPool g_frame_pool[2];
static uint32_t g_frame_pool_left[2];
struct fd_slot {
    struct fd_frame_q q;
    VkFence blit_fence;      // present blit 的渲染 fence（swapchain 交过来）
    VkCommandBuffer blit_cmd;
    int pin_n;               // 本帧新增的 pin 数（位于 pin 数组前缀）
    int parity;              // 本帧用的 arena/描述符池编号
    uint64_t frame_id;        // 本帧的时间戳归属序号（退休时只结算 <= 它的待写项）
    uint64_t ref_seq;         // 本帧在池后备账本里的提交序号（退休时只退休它）
    int used;
};
static struct fd_slot g_slot[2];
static int g_slot_cur = -1;   // 最新的延迟槽；-1 = 没有
static int g_slot_fail;
// 每窗上传账：把“每笔 draw 到底往 arena 里拷了多少字节”量出来（VB/IB/UBO 分开），
// 与零拷贝命中字节对照，判断零拷贝值不值得继续投入（tick 单位：/16 换算成字节）。
static uint64_t g_cp_vb, g_cp_ib, g_cp_ubo, g_cp_vb_n, g_cp_ib_n, g_cp_ubo_n;
void nvn_vk_first_draw_take_copy_stats(uint64_t *vb, uint64_t *ib, uint64_t *ubo,
                                       uint64_t *vbn, uint64_t *ibn, uint64_t *ubon) {
    if (vb) *vb = g_cp_vb;
    if (ib) *ib = g_cp_ib;
    if (ubo) *ubo = g_cp_ubo;
    if (vbn) *vbn = g_cp_vb_n;
    if (ibn) *ibn = g_cp_ib_n;
    if (ubon) *ubon = g_cp_ubo_n;
    g_cp_vb = g_cp_ib = g_cp_ubo = g_cp_vb_n = g_cp_ib_n = g_cp_ubo_n = 0;
}
static _Atomic uint64_t g_slot_retired;   // 延迟槽拿到完成证明并回收的帧数（W6 全账）
static int fd_present_defer(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        // 默认开：2026-10-28 真机 A/B（handheld 1280x720、同一存档、交替两局）37.9/39.1 → 60.0/60.0 fps，
        // 2 分钟 soak（414 个窗口）0 错误，BACK 收尾 session destroy 干净、重启正常。
        // 延迟一帧收口，最长 1 帧在途；关闭用 debug.s2a.present_defer=0。
        __system_property_get("debug.s2a.present_defer", b);
        v = (b[0] && atoi(b) == 0) ? 0 : 1;
    }
    return v;
}
int nvn_vk_first_draw_present_deferred(void) { return fd_present_defer(); }
// 释放快照队列持有的 Vulkan 对象（不碰 arena/pool/pin：那些归帧边界处理）。
static void fd_slot_release(struct s2a_vk_device *d, struct fd_frame_q *q) {
    if (!d || !d->device) return;
    // 快照池的完成证明由调用方按该槽的 frame_id 推进（fd_slot_retire），这里只释放本帧对象。
    for (int i = 0; i < q->pd_n; i++) {
        if (q->pd[i].map) vkUnmapMemory(d->device, q->pd[i].mem);
        if (q->pd[i].buf) vkDestroyBuffer(d->device, q->pd[i].buf, NULL);
        if (q->pd[i].mem) fd_devmem_release_dev(d->device, q->pd[i].mem);
    }
    q->pd_n = 0;
    if (q->pp_buf) { if (q->pp_map) vkUnmapMemory(d->device, q->pp_mem); vkDestroyBuffer(d->device, q->pp_buf, NULL); }
    if (q->pp_mem) fd_devmem_release_dev(d->device, q->pp_mem);
    q->pp_buf = VK_NULL_HANDLE; q->pp_mem = VK_NULL_HANDLE; q->pp_map = NULL;
    for (int i = 0; i < q->nbuf; i++) { if (q->bufs[i]) vkDestroyBuffer(d->device, q->bufs[i], NULL); if (q->mems[i]) fd_devmem_release_dev(d->device, q->mems[i]); }
    for (int i = 0; i < q->nsamp; i++) vkDestroySampler(d->device, q->samps[i], NULL);
    for (int i = 0; i < q->npool; i++) vkDestroyDescriptorPool(d->device, q->pools[i], NULL);
    for (int i = 0; i < q->nfb; i++) vkDestroyFramebuffer(d->device, q->fbs[i], NULL);
    for (int i = 0; i < q->npass; i++) vkDestroyRenderPass(d->device, q->passes[i], NULL);
    for (int i = 0; i < q->nfbimg; i++) {
        if (q->fbview[i]) vkDestroyImageView(d->device, q->fbview[i], NULL);
        if (q->fbimg[i]) vkDestroyImage(d->device, q->fbimg[i], NULL);
        if (q->fbmem[i]) fd_devmem_release_dev(d->device, q->fbmem[i]);
    }
    for (int i = 0; i < q->npend; i++) if (q->pend[i]) vkDestroyPipeline(d->device, q->pend[i], NULL);
    for (int i = 0; i < q->ncmd; i++) fd_cmd_put(d, q->cmds[i]);
    // 走到这里 = fd_slot_retire 已在本设备上等到该槽全部 fence（完成证明在手）：批量 fence 回池，
    // 后缀 fence 不是 ring 创建的（put_back 会按原行为销毁）。
    for (int i = 0; i < q->nbatch_fence; i++)
        if (q->batch_fence[i]) fd_fence_put_back(d->device, q->batch_fence[i]);
    q->nbatch_fence = 0;
    if (q->fence) { fd_fence_put_back(d->device, q->fence); q->fence = VK_NULL_HANDLE; }
    memset(q, 0, sizeof *q);
}
// 等并释放一个延迟槽。返回 0 = 已回收；-1 = 完成证明失败（资源保留，会话按失败处理）。
static int fd_slot_retire(struct s2a_vk_device *d, struct fd_slot *s) {
    if (!s || !s->used) return 0;
    struct fd_frame_q *q = &s->q;
    int bad = 0;
    for (int i = 0; i < q->nbatch_fence && !bad; i++)
        if (q->batch_fence[i] && vkWaitForFences(d->device, 1, &q->batch_fence[i], VK_TRUE, 5000000000ull) != VK_SUCCESS) bad = 1;
    if (!bad && q->fence && vkWaitForFences(d->device, 1, &q->fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) bad = 1;
    if (!bad && s->blit_fence && vkWaitForFences(d->device, 1, &s->blit_fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) bad = 1;
    if (!bad) {
        // 完成证明已在手：先把本帧每个提交批次的查询结果取走并回收页（页归属必须与 fence 同批），
        // 再释放提交资源。取不到就是准确失败（红），不写 0。
        if (s2a_qret_prefix(q->batch_qid, q->nbatch_fence, fd_query_retire_cb, NULL) != 0 ||
            s2a_qret_slot(&q->suffix_qid, fd_query_retire_cb, NULL) != 0) {
            g_slot_fail = 1;
            return -1;
        }
        fd_ssbo_wb_writeback(q);   // 完成证明在手：可写 storage buffer 的影子拷回 guest
        for (int i = 0; i < q->nbatch_fence; i++) fd_arena_complete(q->batch_seq[i]);
        fd_arena_complete(q->suffix_seq);
    }
    if (bad) {
        FD_LOG("present defer: the previous frame has no completion proof; resources retained");
        g_slot_fail = 1;
        return -1;
    }
    // 完成证明已在手：此刻读该帧奇偶的 GPU 起止时间戳，不能等下一帧 arm reset 覆盖。
    (void)nvn_vk_first_draw_gpu_read_frame(d, s->parity, "present retire");
    if (s->blit_cmd) vkFreeCommandBuffers(d->device, d->command_pool, 1, &s->blit_cmd);
    if (s->blit_fence) vkDestroyFence(d->device, s->blit_fence, NULL);
    s->blit_cmd = VK_NULL_HANDLE; s->blit_fence = VK_NULL_HANDLE;
    fd_pp_print_q(q);
    char qwhy[200] = "";
    if (nvn_vk_query_resolve(d, qwhy, sizeof qwhy) != 0)
        FD_LOG("present defer: query writeback at retirement failed: %s", qwhy);
    {
        // 时间戳按帧序号结算：这个 slot 的 fence 已等到，只结算序号 <= 本槽帧序号的待写项；
        // 本帧刚提交、还没执行的项序号更大，不会被提前读回。
        extern int nvn_vk_ts_top_resolve(struct s2a_vk_device *d, uint64_t completed_frame_id,
                                         int blocking, char *why, size_t why_size) __attribute__((weak));
        if (nvn_vk_ts_top_resolve) {
            char twhy[200] = "";
            if (nvn_vk_ts_top_resolve(d, s->frame_id, 0, twhy, sizeof twhy) != 0) {
                FD_LOG("present defer: timestamp-top writeback at retirement failed: %s", twhy);
                g_slot_fail = 1;
                return -1;
            }
        }
    }
    fd_pass_dbg_flush();
    // 该槽的 fence 已全部等到：这是“所属帧已完成”的完成证明，快照池只认它（不是全局周期计数）。
    if (s->frame_id > g_snap_retired_frame) g_snap_retired_frame = s->frame_id;
    fd_slot_release(d, q);
    fd_unpin_prefix(s->pin_n);
    s->pin_n = 0;
    // 这一帧的 arena / 描述符池现在才安全重置（命令已全部完成）。
    fd_arena_reset_parity(s->parity);
    {
        int pc = s->parity & 1;
        if (g_frame_pool[pc] && vkResetDescriptorPool(d->device, g_frame_pool[pc], 0) == VK_SUCCESS) {
            g_frame_pool_left[pc] = 2048;
        }
    }
    s->used = 0;
    g_frame_seq++;
    fd_pass_reason_tick();   // 延迟 present 的帧完成点：与同步 flush 用同一个 16 帧窗口
    atomic_fetch_add_explicit(&g_slot_retired, 1, memory_order_relaxed);
    // 该槽的 fence 已等到：只退休这一帧（序号）登记的池后备在途引用。
    // 用单调序号而不是帧槽奇偶：延迟上屏时 N-1 与 N+1 同槽，同号会把另一帧仍在读的引用一起清掉。
    s2a_poolahb_complete_seq(s->ref_seq);
    s->ref_seq = 0;
    return 0;
}
// 同步边界（guest 等待/收尾）必须把在途的延迟帧先收口，查询结果与资源都不能晚到。
int nvn_vk_first_draw_retire_all(void) {
    int rc = 0;
    for (int i = 0; i < 2; i++) {
        int idx = (g_slot_cur - i + 2) % 2;
        if (g_slot[idx].used) {
            struct s2a_vk_device *d = g_slot[idx].q.dev ? g_slot[idx].q.dev : g_fq.dev;
            if (!d || fd_slot_retire(d, &g_slot[idx]) != 0) { rc = -1; g_slot_fail = 1; }
        }
    }
    g_slot_cur = -1;
    return rc;
}

// 诊断/测试：早批次阈值（默认 8 个 pass 一次）。设为 1 可以让“已提交但未完成的前缀”成为常态，
// 从而真实触达 ncmd==submitted_n>0 时仍必须等完成的分支；不改变正常语义。
// 每积攒多少个 pass 提交一批。8 → 2 是 2026-10-28 真机 A/B 的结论（Redmi 22081212C/Adreno 730，
// handheld 1280x720，同一存档、三对交替局）：8 是 39.3/39.6/38.8 fps，2 是 42.9/42.0/42.0。
// 批更小 → GPU 更早拿到命令、帧首空闲更少；这台设备上多出来的 submit 开销小于重叠收益。
// 更小的 1（逐 pass）在早先的扫描里是 42.0，和 2 同量级但不更好，16 则掉到 35.3。
// 其它设备/游戏可用 debug.s2a.batch_passes 覆盖，默认值按这份读数定。
static int fd_batch_passes(void) {
    static int n = -1;
    if (n < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.batch_passes", v);
        int got = v[0] ? atoi(v) : 0;
        n = (got > 0 && got <= FD_FRAME_CMDS) ? got : 2;
    }
    return n;
}
// 诊断/测试：关掉“入队前先 flush 腾地方”的优化（只影响容量腾挪，不影响容量检查本身），
// 用于把容量缩小时触达真实的“入队满”分支。
static int fd_fq_no_make_room(void) {
    static int n = -1;
    if (n < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.fq_no_make_room", v);
        n = v[0] && atoi(v) ? 1 : 0;
    }
    return n;
}
// 诊断/测试：允许把命令缓冲容量调小（只允许 <= 编译期上限），触达真实“容量满”分支。
static int fd_fq_cmd_capacity(void) {
    static int cap = -1;
    if (cap < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.fq_cmds", v);
        int n = v[0] ? atoi(v) : 0;
        cap = (n > 0 && n < FD_FRAME_CMDS) ? n : FD_FRAME_CMDS;
    }
    return cap;
}
// 帧围栏属于哪个设备：会话被销毁后句柄数值可能仍在，但对象已经无效。新设备绝不能再
// vkResetFences/vkWaitForFences 旧设备的 fence（曾在这里 SIGSEGV，nvn_vk_first_draw.c:600）。
static VkDevice g_fq_fence_dev;

static void fd_fq_reset(void) {
    g_fq.ncmd = 0; g_fq.nbuf = 0; g_fq.nsamp = 0; g_fq.npool = 0; g_fq.nfb = 0; g_fq.npass = 0; g_fq.npend = 0;
    g_fq.nfbimg = 0;
    g_fq.submitted_n = 0; g_fq.nbatch_fence = 0; g_fq.fence = VK_NULL_HANDLE;
    memset(g_fq.batch_qid, 0, sizeof g_fq.batch_qid); g_fq.suffix_qid = 0;
    g_fq.state = FD_FQ_IDLE;
    g_fq.fail[0] = 0;
}
// 队列是否处于终态（失败/超时）：此状态下资源已保留，任何新的提交/释放都禁止。
static int fd_fq_poisoned(void) { return g_fq.state == FD_FQ_FAILED || g_fq.state == FD_FQ_TIMEOUT; }
// 还有没有“已录制/已提交但没有完成证明”的工作。带预算的 flush 会返回“未完成”，此时
// ncmd==submitted_n 且 nbatch_fence 可能为 0，但 g_fq.fence 仍 in-flight——所有需要完成
// 证明的同步边界都必须把它算上，否则会把在途工作当成已完成。
static int fd_fq_has_inflight(void) {
    return g_fq.ncmd > g_fq.submitted_n || g_fq.nbatch_fence > 0 || g_fq.fence != VK_NULL_HANDLE;
}
static void fd_fq_fail(int state, const char *why) {
    if (fd_fq_poisoned()) return;
    // 本条队列进终态 = 已录制的命令缓冲不会再提交：把本批的查询页整页作废（不变式见 nvn_vk_query.c
    // 的 qp_page 注释）。不这么做，页里那些 begin 过的查询会在下次复用时被带 WAIT 读——等一个永不完成的查询。
    extern void nvn_vk_query_batch_abandoned(void) __attribute__((weak));
    if (nvn_vk_query_batch_abandoned) nvn_vk_query_batch_abandoned();
    g_fq.state = state;
    snprintf(g_fq.fail, sizeof g_fq.fail, "%s", why ? why : "unknown");
    FD_LOG("frame queue: %s; %d command buffer(s) and their resources retained "
           "(submitted=%d of %d, early batches=%d; no retry, no reuse)",
           why ? why : "failed", g_fq.ncmd, g_fq.submitted_n, g_fq.ncmd, g_fq.nbatch_fence);
}
// 诊断负例（只用于验收失败路径，不参与正常语义）：1 = 假装 vkQueueSubmit 失败；
// 2 = 提交后假装 fence 超时（命令仍在 GPU 上）。
static int fd_fail_submit_mode(void) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.fail_submit", v);
    return v[0] ? atoi(v) : 0;
}
// 容量满时不能静默丢掉对象（那会让 flush 漏销毁或提前销毁在途对象）：明确失败并让队列进入
// 终态持有既有工作；新建但还没被任何已提交命令引用的对象由调用方自行回收。
static int fd_fq_keep_fb(VkFramebuffer f) {
    if (!f) return 0;
    if (g_fq.nfb >= FD_FRAME_FBS) { fd_fq_fail(FD_FQ_FAILED, "frame queue framebuffer table is full"); return -1; }
    g_fq.fbs[g_fq.nfb++] = f;
    return 0;
}
static int fd_fq_keep_pass(VkRenderPass p) {
    if (!p) return 0;
    if (g_fq.npass >= FD_FRAME_PASSES) { fd_fq_fail(FD_FQ_FAILED, "frame queue render-pass table is full"); return -1; }
    g_fq.passes[g_fq.npass++] = p;
    return 0;
}
static int fd_fq_keep_fbimg(VkImage img, VkDeviceMemory mem, VkImageView view) {
    if (g_fq.nfbimg >= FD_FRAME_FBS) {
        fd_fq_fail(FD_FQ_FAILED, "frame queue feedback-snapshot table is full");
        return -1;
    }
    g_fq.fbimg[g_fq.nfbimg] = img; g_fq.fbmem[g_fq.nfbimg] = mem; g_fq.fbview[g_fq.nfbimg] = view;
    g_fq.nfbimg++;
    return 0;
}
static int fd_fq_keep_pool(VkDescriptorPool p) {
    if (!p) return 0;
    if (g_fq.npool >= FD_FRAME_POOLS) { fd_fq_fail(FD_FQ_FAILED, "frame queue descriptor-pool table is full"); return -1; }
    g_fq.pools[g_fq.npool++] = p;
    return 0;
}
// pass 关闭 / 批量提交 / fence 创建的耗时（诊断，开关 debug.s2a.pass_close，默认关）。
// 报告 §3.3/§4.1：46 pass/帧、batch_passes=2 时每帧约 23 次提交，每次 vkCreateFence+vkQueueSubmit，
// 代码里完全没有计时，而且这些时间被算在触发它的非 draw opcode 名下。这三项只加测、不改任何
// 顺序/语义；关闭时不取钟、不累加、不打印。每 pass ≤2 次 clock -> 46 pass/帧，远低于报告 §7 上限。
// 口径：close 是 pass 关闭整体；close 内触发的 submit 是 close 的子区间，但 clear 入队/开 pass 也会
// 调 submit，所以窗口里的 submit/fence 总量不是 close 的子集，两个读数不能相加。
static uint64_t g_pclose_ns, g_pclose_n;
static uint64_t g_psubmit_ns, g_psubmit_n;
static uint64_t g_pfence_ns, g_pfence_n;
static int fd_pass_close_stats_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.pass_close", b);
        v = (b[0] && atoi(b)) ? 1 : 0;
    }
    return v;
}

// pass 关闭原因计数（诊断，开关 debug.s2a.pass_close_reason=1，默认关）。
// 目的：回答「20 次 pass/帧、43–47 µs/次，其中多少是语义上真的必须切」。每个 close 只进一个
// 触发桶（由调用点传进来：调用点就是唯一的触发来源），n = 各触发桶之和。failed 是 n 的子集，
// 只作旁证（close 计入了触发桶，但命令缓冲没能入队）。关闭时不取钟、不累加、不打印（只多传一个枚举）。
enum {
    FD_PCR_ATTACH_SAME = 0,   // 同一附件继续：附件没变，但反馈采样快照要求先收尾旧 pass
    FD_PCR_ATTACH_SWITCH,     // 附件切换：新 draw 的附件/清除状态不能并入当前 pass
    FD_PCR_DEPTH_ONLY,        // 深度-only 结束：换 pass 时被关闭的是 depth-only pass
    FD_PCR_CLEAR,             // 清屏：整幅 queued clear 入队前收尾
    FD_PCR_STATE_BREAK,       // 非绘制 opcode 边界（sync=0：只改 CPU 记录的 SetRT/Barrier/…）
    FD_PCR_GPU_OP,            // 非绘制 opcode 边界（sync=1：会立即提交 GPU 工作的 clear/copy/dispatch/fence）
    FD_PCR_QUERY,             // 时间戳/查询命令要排在已录制 draw 之后
    FD_PCR_FRAME_FLUSH,       // 帧末 flush / finish
    FD_PCR_FAIL_CLEANUP,      // 失败收尾：本笔 draw 失败后结束半开的 pass
    FD_PCR_COUNT
};
static uint64_t g_pr_n[FD_PCR_COUNT];
static uint64_t g_pr_failed;
static int fd_pass_close_reason_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.pass_close_reason", b);
        v = (b[0] && atoi(b)) ? 1 : 0;
    }
    return v;
}
static void fd_pass_reason_note(int reason) {
    if (reason < 0 || reason >= FD_PCR_COUNT) reason = FD_PCR_ATTACH_SWITCH;
    __atomic_fetch_add(&g_pr_n[reason], 1, __ATOMIC_RELAXED);
}
// 每 16 个「已完成的帧」打一行（与已有 submit window 同为 16 帧窗口，但窗口边界独立：
// 那边按 successful present 计数，这里按帧完成点计数）。读取即清零，不取钟、不读 /proc。
static void fd_pass_reason_tick(void) {
    if (!fd_pass_close_reason_on()) return;
    static uint32_t win;
    if (++win % 16) return;
    uint64_t n[FD_PCR_COUNT];
    uint64_t total = 0;
    for (int i = 0; i < FD_PCR_COUNT; i++) {
        n[i] = __atomic_exchange_n(&g_pr_n[i], 0, __ATOMIC_RELAXED);
        total += n[i];
    }
    uint64_t failed = __atomic_exchange_n(&g_pr_failed, 0, __ATOMIC_RELAXED);
    if (!total) return;
    FD_LOG("pass reason window(开关 debug.s2a.pass_close_reason=1, 16帧窗口): n=%llu (%.1f/帧) "
           "attach_same=%llu attach_switch=%llu depth_only=%llu clear=%llu state_break=%llu gpu_op=%llu "
           "query=%llu frame_flush=%llu fail_cleanup=%llu | 子集: failed=%llu（子集不与触发桶相加）",
           (unsigned long long)total, (double)total / 16.0,
           (unsigned long long)n[FD_PCR_ATTACH_SAME], (unsigned long long)n[FD_PCR_ATTACH_SWITCH],
           (unsigned long long)n[FD_PCR_DEPTH_ONLY], (unsigned long long)n[FD_PCR_CLEAR],
           (unsigned long long)n[FD_PCR_STATE_BREAK], (unsigned long long)n[FD_PCR_GPU_OP],
           (unsigned long long)n[FD_PCR_QUERY], (unsigned long long)n[FD_PCR_FRAME_FLUSH],
           (unsigned long long)n[FD_PCR_FAIL_CLEANUP], (unsigned long long)failed);
}

// ---- 批量提交 fence ring（debug.s2a.fence_ring=1 打开，默认关）-------------------------------
// 现状：fd_submit_batch 每批 vkCreateFence、退休点 vkDestroyFence；营地场景 16 批/帧，fence 创建
// 实测只 0.21–0.24 ms/16 帧（≈14 µs/帧）。这里把「已经拿到完成证明」的 fence 按槽位留下来复用，
// 不改变提交顺序、不改变完成证明、不改变任何命令内容。默认关：关闭时与改前逐字节一致。
// 复用前提（唯一）：put_back 的每个调用点都刚在同一个 VkDevice 上拿到完成证明——
//   vkWaitForFences 成功（或 vkGetFenceStatus=VK_SUCCESS），即这个 fence 对应的那次 vkQueueSubmit
//   已经完成；对象已 signal、不再关联任何未完成的队列命令，所以下一次 acquire 时 vkResetFences 合法
//   （Vulkan：不得 reset 关联未完成命令的 fence）。
// 提交失败路径不复用、按旧行为立即销毁（fd_fence_discard）：不对「驱动失败后 fence 处于什么状态」
//   做任何假设，保持「没有成功提交 = 这批命令从未执行 → fence 可以立即销毁」的语义逐字不变。
// 没有完成证明的一律不回池：失败/超时/终态（fd_fq_fail）路径保留原状（资源保留、不释放）。
// 槽位连 VkDevice 一起记：设备代际变化时旧槽直接丢弃（对象随旧设备回收，不能在新设备上 reset/destroy）。
#define FD_FENCE_RING 64
struct fd_fence_ring_slot { VkFence fence; VkDevice dev; int state; };
enum { FD_FR_EMPTY = 0, FD_FR_READY, FD_FR_OUT };
static struct fd_fence_ring_slot g_fence_ring[FD_FENCE_RING];
static uint64_t g_fr_reused, g_fr_created, g_fr_destroyed;
static int fd_fence_ring_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.fence_ring", b);
        v = (b[0] && atoi(b)) ? 1 : 0;   // 默认关；=1 打开复用，=0/未设置都走每批 create/destroy
    }
    return v;
}
// 取一个可用于本次提交的 fence：优先复用已 proven 的池内槽（reset 后使用）；没有/开关关/设备不符
// 时按原行为新建。新建的也登记成 OUT，退休点才能放回池（池满时新建的不登记，退休点直接销毁）。
static VkFence fd_fence_acquire(VkDevice dev) {
    if (fd_fence_ring_on()) {
        for (int i = 0; i < FD_FENCE_RING; i++) {
            struct fd_fence_ring_slot *s = &g_fence_ring[i];
            if (s->state != FD_FR_READY) continue;
            if (s->dev != dev) {
                // 旧设备的槽：对象随旧设备回收，这里只丢记账（不能在新设备上 destroy）。
                s->state = FD_FR_EMPTY; s->fence = VK_NULL_HANDLE; s->dev = VK_NULL_HANDLE;
                continue;
            }
            // 只有 proven（已 signal）的槽才会是 READY：reset 回 unsignaled 给本次提交用。
            if (vkResetFences(dev, 1, &s->fence) != VK_SUCCESS) {
                vkDestroyFence(dev, s->fence, NULL);
                s->state = FD_FR_EMPTY; s->fence = VK_NULL_HANDLE; s->dev = VK_NULL_HANDLE;
                __atomic_fetch_add(&g_fr_destroyed, 1, __ATOMIC_RELAXED);
                continue;
            }
            s->state = FD_FR_OUT;
            __atomic_fetch_add(&g_fr_reused, 1, __ATOMIC_RELAXED);
            return s->fence;
        }
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS) return VK_NULL_HANDLE;
    __atomic_fetch_add(&g_fr_created, 1, __ATOMIC_RELAXED);
    if (fd_fence_ring_on()) {
        for (int i = 0; i < FD_FENCE_RING; i++) {
            struct fd_fence_ring_slot *s = &g_fence_ring[i];
            if (s->state != FD_FR_EMPTY) continue;
            s->fence = fence; s->dev = dev; s->state = FD_FR_OUT;
            break;
        }
    }
    return fence;
}
// 回收一个「已经拿到完成证明」的 fence（调用点前提见上面的论证）。
// 池里找得到（句柄 + 设备都匹配）就放回 READY 等下次 acquire reset；找不到（开关关、池满时新建的、
// 非本路径创建的，例如后缀 fence）就按原行为立即销毁。
static void fd_fence_put_back(VkDevice dev, VkFence fence) {
    if (!fence) return;
    if (dev && fd_fence_ring_on()) {
        for (int i = 0; i < FD_FENCE_RING; i++) {
            struct fd_fence_ring_slot *s = &g_fence_ring[i];
            if (s->state == FD_FR_OUT && s->fence == fence && s->dev == dev) { s->state = FD_FR_READY; return; }
        }
    }
    if (dev) vkDestroyFence(dev, fence, NULL);
    __atomic_fetch_add(&g_fr_destroyed, 1, __ATOMIC_RELAXED);
}
// 没有完成证明/提交失败时按旧行为立即销毁：先清掉池内记账（不能留一个已销毁的句柄），再销毁。
static void fd_fence_discard(VkDevice dev, VkFence fence) {
    if (!fence) return;
    for (int i = 0; i < FD_FENCE_RING; i++) {
        struct fd_fence_ring_slot *s = &g_fence_ring[i];
        if (s->state == FD_FR_OUT && s->fence == fence && (!dev || s->dev == dev)) {
            s->state = FD_FR_EMPTY; s->fence = VK_NULL_HANDLE; s->dev = VK_NULL_HANDLE;
            break;
        }
    }
    if (dev) vkDestroyFence(dev, fence, NULL);
    __atomic_fetch_add(&g_fr_destroyed, 1, __ATOMIC_RELAXED);
}

// 每积攒 8 个 pass 提交一批（带自己的 fence）：GPU 与后续 CPU 录制重叠，present 只需等这些 fence。
// 逐 pass submit 的驱动开销世界一帧 ~16 ms；批量后只付 ~7 次 submit。
// 游标只在 vkQueueSubmit 成功后推进；每个批次保留自己的完成证据，flush 时按提交顺序全部等到。
static int fd_submit_batch(void) {
    if (!g_fq.dev) return -1;
    if (fd_fq_poisoned()) return -1;
    int n = g_fq.ncmd - g_fq.submitted_n;
    if (n <= 0) return 0;
    if (g_fq.nbatch_fence >= FD_FRAME_BATCH_FENCES) {
        fd_fq_fail(FD_FQ_FAILED, "frame queue batch-fence table is full; cannot prove completion of earlier batches");
        return -1;
    }
    if (fd_fail_submit_mode() == 1) { fd_fq_fail(FD_FQ_FAILED, "injected batch submit failure"); return -1; }
    // 诊断开关（默认关）：只对成功提交的批次记账；失败批次是终态，不计入窗口读数。
    const int st = fd_pass_close_stats_on();
    const int64_t t_sub0 = st ? fd_now_ns() : 0;
    // fence ring（debug.s2a.fence_ring=1，默认关）：复用已 proven 的 fence，否则按原行为新建。
    // fence 子项计时因此从“vkCreateFence”变成“取 fence（reset 复用或新建）”，口径不变。
    VkFence fence = fd_fence_acquire(g_fq.dev->device);
    if (!fence) {
        fd_fq_fail(FD_FQ_FAILED, "vkCreateFence failed for a recorded pass batch");
        return -1;
    }
    const int64_t t_fence1 = st ? fd_now_ns() : 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = (uint32_t)n; si.pCommandBuffers = &g_fq.cmds[g_fq.submitted_n];
    nvn_vk_callring_add("fq submit n=%d first=%p fence=%p", (int)n, (void *)si.pCommandBuffers[0], (void *)fence);
    VkResult rc = vkQueueSubmit(g_fq.dev->queue, 1, &si, fence);
    const int64_t t_sub1 = st ? fd_now_ns() : 0;
    if (rc != VK_SUCCESS) {
        // 没有成功提交 = 这批命令从未执行：fence 可以立即销毁（旧语义逐字不变；这里不复用，
        // 不对驱动失败后的 fence 状态做任何假设）；命令缓冲按终态保留（不重试）。
        fd_fence_discard(g_fq.dev->device, fence);
        fd_fq_fail(FD_FQ_FAILED, "vkQueueSubmit failed for a recorded pass batch");
        return -1;
    }
    if (st) {
        // submit = 本批整体（fence 创建 + vkQueueSubmit）；fence 是其中子项，不能与 submit 相加。
        __atomic_fetch_add(&g_psubmit_ns, (uint64_t)(t_sub1 - t_sub0), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_psubmit_n, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_pfence_ns, (uint64_t)(t_fence1 - t_sub0), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_pfence_n, 1, __ATOMIC_RELAXED);
    }
    int bf = g_fq.nbatch_fence;
    g_fq.batch_fence[bf] = fence;
    g_fq.batch_seq[bf] = fd_arena_seal_submit();   // 提交成功 → 本批录制的块封存到这个提交编号
    g_fq.batch_qid[bf] = fd_query_next_batch();    // 查询页归属这个提交批次，完成后再取结果
    nvn_vk_query_batch_submitted(g_fq.batch_qid[bf]);
    g_fq.nbatch_fence = bf + 1;
    g_fq.submitted_n = g_fq.ncmd;   // 提交成功后才推进；这批现在是“已提交但未完成”
    if (g_fq.state < FD_FQ_SUBMITTED) g_fq.state = FD_FQ_SUBMITTED;
    return 0;
}

// 管线槽释放：对象引用减一，管线对象等帧完成后销毁（已录制命令可能仍引用它）。
static void fd_pipe_slot_release(struct s2a_vk_device *d, struct fd_pipe_slot *s) {
    if (!s->used) return;
    if (s->pipe) {
        if (g_fq.npend < FD_FRAME_PEND) {
            g_fq.pend[g_fq.npend++] = s->pipe;
        } else if (d) {
            // 极端情况：待销毁队列满。先等一次完成再销毁，保证没有在途引用。
            if (nvn_vk_first_draw_flush() != 0) { FD_LOG("pipeline retire: queue did not complete; pipeline retained"); return; }
            if (d->device) vkDestroyPipeline(d->device, s->pipe, NULL);
        }
    }
    if (s->obj && s->obj->users) s->obj->users--;
    memset(s, 0, sizeof *s);
}

/* ---------------- C4b：管线创建入后台（debug.s2a.pipe_async，默认 0 = 旧同步创建） ----------------
   开关开：管线配方 miss 时把完整创建输入投进 nvn_vk_pipe_worker 后台线程，本笔绘制按
   C4a 同一预算/判定表决定等还是跳；worker 产出的管线复核代际+配方后装入本缓存。
   开关关：路径与旧版逐字节一致（同步 vkCreateGraphicsPipelines）。

   与 debug.s2a.fu_wait 分开立开关的理由：fu_wait 测的是着色器现编等待链（compile_worker），
   pipe_async 测的是管线对象创建链（pipe_worker）——两个成本中心、两套新机制，真机 A/B
   归因要能单独开/关；判定点（能等多久）共用 C4a 的预算与 skip_safe，行为口径一致。

   生命周期硬约束（违反就是 UAF）：
   - job 引用 pobj->playout 与 render_pass，直到结果被 take/release。render_pass 只接受
     全局 rp 缓存的（rp_queued==0，设备寿命）；open pass/帧队列 pass 不进 worker（帧末销毁）。
   - pobj 在飞期间会被 LRU 淘汰（users==0 且非本帧）：pend 表项持有 users++ 保活到
     结果取走或放弃，释放时归还给对象表。 */

/* 弱链接[androiddraw]：nvn_vk_pipe_worker_start（宿主单测不链 pipe_worker.c 时整套
   缺席 → fd_pipe_async_ensure 返回 0，路径与开关关闭同义。头文件是强声明，
   按本文件 nvn_ga2ca 同款用 #pragma weak 覆盖本 TU 的引用强度。） */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_active */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_submit */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_take */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_wait */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_release */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_stop */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_cache_lock */
/* 弱链接[androiddraw]：nvn_vk_pipe_worker_cache_unlock */
#pragma weak nvn_vk_pipe_worker_start
#pragma weak nvn_vk_pipe_worker_active
#pragma weak nvn_vk_pipe_worker_submit
#pragma weak nvn_vk_pipe_worker_take
#pragma weak nvn_vk_pipe_worker_wait
#pragma weak nvn_vk_pipe_worker_release
#pragma weak nvn_vk_pipe_worker_stop
#pragma weak nvn_vk_pipe_worker_cache_lock
#pragma weak nvn_vk_pipe_worker_cache_unlock
/* 弱链接[androiddraw]：nvk_fu_item_ms（draw.c 的 C4a 预算/判定件；缺实现时按 iOS 默认上限走） */
/* 弱链接[androiddraw]：nvk_fu_frame_ms */
/* 弱链接[androiddraw]：nvk_fu_unsafe_ms */
/* 弱链接[androiddraw]：nvk_fu_skip_safe */
/* 弱链接[androiddraw]：nvk_fu_budget_state */
extern uint32_t nvk_fu_item_ms(void);
extern uint32_t nvk_fu_frame_ms(void);
extern uint32_t nvk_fu_unsafe_ms(void);
extern int nvk_fu_skip_safe(const struct s2a_record_draw_state *, uint64_t);
extern struct s2a_fu_budget *nvk_fu_budget_state(void);
#pragma weak nvk_fu_item_ms
#pragma weak nvk_fu_frame_ms
#pragma weak nvk_fu_unsafe_ms
#pragma weak nvk_fu_skip_safe
#pragma weak nvk_fu_budget_state
/* 弱链接[androiddraw]：s2a_fu_wait_decide（共用件缺席时判定环整体不开） */
/* 弱链接[androiddraw]：s2a_fu_budget_left_ms */
/* 弱链接[androiddraw]：s2a_fu_budget_spend */
#pragma weak s2a_fu_wait_decide
#pragma weak s2a_fu_budget_left_ms
#pragma weak s2a_fu_budget_spend

static int fd_pipe_async_on(void) {
    static int v = -1;
    if (v < 0) v = fd_prop_is_on("debug.s2a.pipe_async");
    return v;
}

/* 在飞管线配方：late/stuck 终局标记 + pobj 保活 pin。判据用 nvk_pipe_recipe_match
   （与 worker 去重、缓存命中同口径，request_id 不参与）。 */
#define FD_PIPE_PEND 32
static struct {
    struct nvk_pipe_recipe recipe;
    struct fd_prog_obj *pobj;
    uint8_t used, late, stuck;
    uint64_t seq;
} g_pipe_pend[FD_PIPE_PEND];
static uint64_t g_pipe_pend_seq;

static int fd_pipe_pend_find(const struct nvk_pipe_recipe *r) {
    for (int i = 0; i < FD_PIPE_PEND; i++)
        if (g_pipe_pend[i].used && nvk_pipe_recipe_match(&g_pipe_pend[i].recipe, r)) return i;
    return -1;
}
static void fd_pipe_pend_drop(int i) {
    if (i < 0 || !g_pipe_pend[i].used) return;
    if (g_pipe_pend[i].pobj && g_pipe_pend[i].pobj->users) g_pipe_pend[i].pobj->users--;
    memset(&g_pipe_pend[i], 0, sizeof g_pipe_pend[i]);
}
/* 新登记：占用即对 pobj 计一次 users（布局对象的保活 pin）；满返回 -1（调用方回落同步）。 */
static int fd_pipe_pend_add(const struct nvk_pipe_recipe *r, struct fd_prog_obj *pobj) {
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < FD_PIPE_PEND; i++) {
        if (!g_pipe_pend[i].used) { slot = i; break; }
        if (g_pipe_pend[i].seq < oldest) { oldest = g_pipe_pend[i].seq; slot = i; }
    }
    if (slot < 0) return -1;
    if (g_pipe_pend[slot].used) fd_pipe_pend_drop(slot);   // 挤掉最旧的：它的 worker 结果到无人认领会被 drain 丢弃
    g_pipe_pend[slot].recipe = *r;
    g_pipe_pend[slot].pobj = pobj;
    g_pipe_pend[slot].late = g_pipe_pend[slot].stuck = 0;
    g_pipe_pend[slot].seq = ++g_pipe_pend_seq;
    g_pipe_pend[slot].used = 1;
    if (pobj) pobj->users++;
    return slot;
}
static void fd_pipe_pend_mark(int i, int timeout) {
    if (i < 0 || !g_pipe_pend[i].used) return;
    if (timeout == S2A_FU_T_LATE) g_pipe_pend[i].late = 1;
    else if (timeout == S2A_FU_T_STUCK) g_pipe_pend[i].stuck = 1;
    g_pipe_pend[i].seq = ++g_pipe_pend_seq;
}

/* 跳过/等待与 unsupported 分账：跳过是有意的、可恢复的，与 C4a 同一口径。 */
static _Atomic uint64_t g_pfu_submit, g_pfu_ready_n, g_pfu_wait_n, g_pfu_wait_ns;
static _Atomic uint64_t g_pfu_skip_budget, g_pfu_skip_late, g_pfu_skip_stuck, g_pfu_fallback;
static _Atomic uint64_t g_pfu_drain_n, g_pfu_drain_drop;
static void fd_pipe_fu_stats_log(void) {
    static _Atomic uint64_t n;
    uint64_t c = atomic_fetch_add(&n, 1) + 1;
    if (c != 1 && c % 256) return;
    FD_LOG("pipe first-use: submit=%llu ready=%llu waits=%llu wait=%.1fms "
           "skip{budget=%llu late=%llu stuck=%llu} sync_fallback=%llu drain=%llu drain_drop=%llu",
           (unsigned long long)atomic_load(&g_pfu_submit),
           (unsigned long long)atomic_load(&g_pfu_ready_n),
           (unsigned long long)atomic_load(&g_pfu_wait_n),
           (double)atomic_load(&g_pfu_wait_ns) / 1e6,
           (unsigned long long)atomic_load(&g_pfu_skip_budget),
           (unsigned long long)atomic_load(&g_pfu_skip_late),
           (unsigned long long)atomic_load(&g_pfu_skip_stuck),
           (unsigned long long)atomic_load(&g_pfu_fallback),
           (unsigned long long)atomic_load(&g_pfu_drain_n),
           (unsigned long long)atomic_load(&g_pfu_drain_drop));
}
static void fd_pipe_fu_skip_count(int timeout) {
    if (timeout == S2A_FU_T_BUDGET) atomic_fetch_add(&g_pfu_skip_budget, 1);
    else if (timeout == S2A_FU_T_LATE) atomic_fetch_add(&g_pfu_skip_late, 1);
    else if (timeout == S2A_FU_T_STUCK) atomic_fetch_add(&g_pfu_skip_stuck, 1);
    fd_pipe_fu_stats_log();
}
static const char *fd_pipe_fu_class(int t) {
    return t == S2A_FU_T_LATE ? "late" : t == S2A_FU_T_BUDGET ? "budget" :
           t == S2A_FU_T_STUCK ? "stuck" : "?";
}

static int fd_pipe_async_ensure(struct s2a_vk_device *d, char *why, size_t why_size) {
    if (!fd_pipe_async_on()) return 0;
    if (!nvn_vk_pipe_worker_start || !nvn_vk_pipe_worker_active || !nvn_vk_pipe_worker_submit ||
        !nvn_vk_pipe_worker_wait || !nvn_vk_pipe_worker_release || !nvn_vk_pipe_worker_take ||
        !s2a_fu_wait_decide || !s2a_fu_budget_left_ms || !s2a_fu_budget_spend) return 0;
    if (nvn_vk_pipe_worker_active()) return 1;
    /* 代际 = 设备句柄（与 recipe.device 同源）：设备重建后 gen 自然换，worker 对旧代际
       的结果一律不转交；start 幂等，旧线程未退出会如实拒绝（本笔回落同步）。 */
    if (nvn_vk_pipe_worker_start(d->device, fd_pipeline_cache(d),
                                 (uint64_t)(uintptr_t)d->device, why, why_size) != 0) {
        static _Atomic int nf;
        if (atomic_fetch_add(&nf, 1) < 4)
            FD_LOG("pipe async: worker start failed (%s); this draw uses sync creation",
                   why && why[0] ? why : "unknown");
        return 0;
    }
    FD_LOG("pipe async: worker started (debug.s2a.pipe_async=1)");
    return 1;
}

/* 装入管线缓存：与同步创建同一条插入路径（含 LRU 逐出与索引回填）。调用方持有
   pipeline 所有权；装入后归缓存所有。pobj 必须由 pend pin 或调用方保活。 */
static void fd_pipe_adopt(struct s2a_vk_device *d, const struct nvk_pipe_recipe *recipe,
                          uint64_t pipe_key, VkPipeline pipeline, struct fd_prog_obj *pobj,
                          const uint8_t program_input_sha[32]) {
    if (program_input_sha) nvm_manifest_note(recipe, program_input_sha);
    int slot = -1;
    for (int i = 0; i < FD_PIPE_CACHE; i++) if (!g_pipe_cache[i].used) { slot = i; break; }
    if (slot < 0) {
        uint64_t oldest = ~0ull;
        for (int i = 0; i < FD_PIPE_CACHE; i++)
            if (g_pipe_cache[i].last_use < oldest) { oldest = g_pipe_cache[i].last_use; slot = i; }
        fd_pipe_slot_release(d, &g_pipe_cache[slot]);   // 只释放本槽；共享对象不在这里销毁
        atomic_fetch_add(&g_pipe_evicts, 1);
    }
    g_pipe_cache[slot].used = 1; g_pipe_cache[slot].key = pipe_key;
    *fd_pipe_index_slot(pipe_key) = slot;
    g_pipe_cache[slot].recipe = *recipe;
    g_pipe_cache[slot].pipe = pipeline; g_pipe_cache[slot].obj = pobj;
    pobj->users++;
    g_pipe_cache[slot].last_use = ++g_pipe_clock;
}

/* 把 worker 已产出的结果取回装缓存。返回取到的条数。input_sha 只在本条配方属于
   当前程序时传给 manifest_note（别的程序的结果不知道它的输入键，不硬配）。 */
static int fd_pipe_drain(struct s2a_vk_device *d, const uint8_t prog_sha[32],
                         const uint8_t program_input_sha[32]) {
    if (!nvn_vk_pipe_worker_take || !nvn_vk_pipe_worker_release) return 0;
    int got = 0;
    struct nvk_pipe_job_result res;
    while (nvn_vk_pipe_worker_take(&res) == 1) {
        int pi = fd_pipe_pend_find(&res.recipe);
        int usable = res.ok && res.pipeline &&
                     res.device_generation == (uint64_t)(uintptr_t)d->device && pi >= 0;
        uint64_t key = fd_pipe_key(&res.recipe);
        if (usable) {
            /* 同配方可能已被同步创建/先前 adopt 装上：命中则丢弃这份，不重复装。 */
            int32_t s = *fd_pipe_index_slot(key);
            int dup = 0;
            if (s >= 0 && (unsigned)s < FD_PIPE_CACHE && g_pipe_cache[s].used &&
                nvk_pipe_recipe_hit(g_pipe_cache[s].key, &g_pipe_cache[s].recipe, key, &res.recipe)) dup = 1;
            if (!dup)
                for (int i = 0; i < FD_PIPE_CACHE && !dup; i++)
                    if (g_pipe_cache[i].used &&
                        nvk_pipe_recipe_hit(g_pipe_cache[i].key, &g_pipe_cache[i].recipe, key, &res.recipe)) dup = 1;
            if (dup) {
                usable = 0;
            } else {
                const uint8_t *in_sha = (prog_sha && program_input_sha &&
                                         !memcmp(res.recipe.prog_sha, prog_sha, 32)) ? program_input_sha : NULL;
                fd_pipe_adopt(d, &res.recipe, key, res.pipeline, g_pipe_pend[pi].pobj, in_sha);
                atomic_fetch_add(&g_pfu_drain_n, 1);
            }
        }
        if (!usable) {
            if (res.ok && res.pipeline) {
                atomic_fetch_add(&g_pfu_drain_drop, 1);
                static _Atomic int nf;
                if (atomic_fetch_add(&nf, 1) < 8)
                    FD_LOG("pipe async: drained pipeline dropped (stale/dup/no pend)");
            }
            nvn_vk_pipe_worker_release(&res);
        }
        if (pi >= 0) fd_pipe_pend_drop(pi);
        got++;
    }
    if (got) fd_pipe_fu_stats_log();
    return got;
}

/* 提交 + 等待环（C4a 判定表语义）。
   返回 0 = 管线已装入缓存并写到 *out_pipe；2 = 按判定有意跳过本笔；
   1 = 回落同步创建（队满/worker 未起/证据丢失/pend 表满）；
   -1 = 真实失败（why 已写）。 */
static int fd_pipe_async_get(struct s2a_vk_device *d, const struct s2a_record_draw_state *state,
                             const struct nvk_pipe_job_spec *spec, struct fd_prog_obj *pobj,
                             uint64_t pipe_key, const uint8_t program_input_sha[32],
                             VkPipeline *out_pipe, char *why, size_t why_size) {
    uint64_t job_id = 0;
    int src = nvn_vk_pipe_worker_submit(spec, &job_id, why, why_size);
    if (src == -2 || src == -3) {   // 队列满 / worker 未起：等价旧同步路径
        atomic_fetch_add(&g_pfu_fallback, 1);
        static _Atomic int nf;
        if (atomic_fetch_add(&nf, 1) < 8)
            FD_LOG("pipe async: %s, sync create", src == -2 ? "queue full" : "worker not running");
        return 1;
    }
    if (src != 0 && src != 1) {   // -1 参数/容量；-4 代际不匹配：如实失败，不静默回落
        if (!why[0]) fail(why, why_size, "pipeline worker submit failed");
        return -1;   // why 已由 submit 写清
    }
    atomic_fetch_add(&g_pfu_submit, 1);
    int pi = fd_pipe_pend_find(&spec->recipe);
    if (pi < 0) pi = fd_pipe_pend_add(&spec->recipe, pobj);
    if (pi < 0) {
        /* 表满：job 仍在飞（结果到无人认领会被 drain 丢弃），本笔按旧路径同步建。 */
        atomic_fetch_add(&g_pfu_fallback, 1);
        return 1;
    }
    const uint64_t frame = nvn_vk_first_draw_frame_id();
    const uint32_t item_ms = nvk_fu_item_ms ? nvk_fu_item_ms() : 2000;
    const uint32_t frame_ms = nvk_fu_frame_ms ? nvk_fu_frame_ms() : 100;
    const uint32_t unsafe_ms = nvk_fu_unsafe_ms ? nvk_fu_unsafe_ms() : 10000;
    const int may_skip = (nvk_fu_skip_safe && state) ? nvk_fu_skip_safe(state, frame) : 0;
    static struct s2a_fu_budget local_budget;
    struct s2a_fu_budget *budget = nvk_fu_budget_state ? nvk_fu_budget_state() : &local_budget;
    int late = g_pipe_pend[pi].late, stuck = g_pipe_pend[pi].stuck;
    for (;;) {
        struct nvk_pipe_job_result res;
        int wrc = nvn_vk_pipe_worker_wait(&spec->recipe, spec->device_generation, 0, &res);
        if (wrc == 1) {
            if (!res.ok || !res.pipeline) {
                snprintf(why, why_size, "pipeline worker: %s", res.why[0] ? res.why : "creation failed");
                nvn_vk_pipe_worker_release(&res);
                fd_pipe_pend_drop(pi);
                return -1;
            }
            fd_pipe_adopt(d, &res.recipe, pipe_key, res.pipeline, pobj, program_input_sha);
            *out_pipe = res.pipeline;
            memset(&res, 0, sizeof res);   // 所有权已转交缓存
            fd_pipe_pend_drop(pi);
            atomic_fetch_add(&g_pfu_ready_n, 1);
            return 0;
        }
        if (wrc == -1) {   // 证据丢失（挤出/stop）：回同步创建，不空等
            fd_pipe_pend_drop(pi);
            atomic_fetch_add(&g_pfu_fallback, 1);
            return 1;
        }
        int64_t left = s2a_fu_budget_left_ms(budget, frame, frame_ms);
        struct s2a_fu_verdict v = s2a_fu_wait_decide(0, late, stuck, may_skip,
                                                   item_ms, frame_ms, left, unsafe_ms);
        if (v.action == S2A_FU_SKIP) {
            fd_pipe_pend_mark(pi, v.timeout);
            fd_pipe_fu_skip_count(v.timeout);
            snprintf(why, why_size, "pipeline first-use skipped (%s)", fd_pipe_fu_class(v.timeout));
            return 2;
        }
        const uint32_t slice = v.wait_ms ? v.wait_ms : 250;
        const uint64_t t0 = fd_now_ns();
        wrc = nvn_vk_pipe_worker_wait(&spec->recipe, spec->device_generation, slice, &res);
        const uint64_t waited = fd_now_ns() - t0;
        if (may_skip) s2a_fu_budget_spend(budget, frame, waited);
        atomic_fetch_add(&g_pfu_wait_n, 1);
        atomic_fetch_add(&g_pfu_wait_ns, waited);
        if (wrc == 1) {
            if (!res.ok || !res.pipeline) {
                snprintf(why, why_size, "pipeline worker: %s", res.why[0] ? res.why : "creation failed");
                nvn_vk_pipe_worker_release(&res);
                fd_pipe_pend_drop(pi);
                return -1;
            }
            fd_pipe_adopt(d, &res.recipe, pipe_key, res.pipeline, pobj, program_input_sha);
            *out_pipe = res.pipeline;
            memset(&res, 0, sizeof res);
            fd_pipe_pend_drop(pi);
            atomic_fetch_add(&g_pfu_ready_n, 1);
            return 0;
        }
        if (wrc == -1) {
            fd_pipe_pend_drop(pi);
            atomic_fetch_add(&g_pfu_fallback, 1);
            return 1;
        }
        /* 本片超时：按判定表给的终局定性，打项标并跳过本笔。 */
        fd_pipe_pend_mark(pi, v.timeout);
        fd_pipe_fu_skip_count(v.timeout);
        snprintf(why, why_size, "pipeline first-use skipped (%s)", fd_pipe_fu_class(v.timeout));
        return 2;
    }
}

// ---- 连续的同一附件绘制合并为一个 render pass / command buffer ----
// NVN 命令流通常是一个 SetRenderTargets 后跟多笔 draw。若逐笔 begin/end render pass，
// Adreno 会对同一张 1600x900 附件反复 load/store（362 笔/帧 → 数 GB/帧带宽）。
// 这里把“同一组附件、同一清除状态”的连续 draw 录进同一个 command buffer，只在附件变化、
// 非绘制 GPU 操作或 present 时结束 pass。诊断 trace/probe 仍走逐笔路径。
struct fd_open_pass {
    int open;
    struct s2a_vk_device *dev;
    VkCommandBuffer cmd;
    VkRenderPass pass;
    VkFramebuffer fb;
    VkImage color, depth;              // 诊断/探针用的首参与颜色附件（槽号可能不是 0）
    VkImageView color_view, depth_view;
    VkFormat color_fmt, depth_fmt;
    uint32_t w, h;
    int depth_only;
    uint32_t clear_mask;   // bit0 = 本 pass 开始时清深度，bit1 = 清模板；两平面独立
    // MRT：逐槽真实附件。color_count 是 subpass colorAttachmentCount（含中间空洞），
    // 空洞槽的 image/view 为 VK_NULL_HANDLE；pass 连续性按整套附件比较。
    uint32_t color_count;
    VkImage color_img[NVN_VK_MRT_MAX_COLOR];
    VkImageView color_views[NVN_VK_MRT_MAX_COLOR];
    VkFormat color_fmts[NVN_VK_MRT_MAX_COLOR];
    char last_req[64];     // 诊断：并入本 pass 的最近一笔 draw 的 request id（pass probe 用）
    char last_prog[16];    // 诊断：同上，程序内容 SHA-256 前 4 字节（pass probe 用）
};
static struct fd_open_pass g_op;
// 本帧已录制的 draw 数（诊断用）：载入屏 ~几十/帧，世界帧数千/帧；trace 用它区分
// “载入屏反复使用同一合成程序”和“世界首帧”。
static _Atomic uint32_t g_draws_this_frame;
static uint64_t g_frame_ref_seq;   // 本帧的池后备提交序号（帧首笔 draw 时分配）
static int g_fb_logged;   // feedback 快照日志只打一次（逐帧重复没有新信息）
static _Atomic uint64_t g_fb_leave_n;   // 反馈采样“是反馈但单层 2D 快照表达不了”的绑定数（诊断）
static int g_pass_merges, g_pass_flushes;
// 本帧仍在 GPU 命令/描述符里的纹理：flush 拿到 fence 后才能 unpin，之后缓存才可淘汰/销毁。
#define FD_PIN_MAX 16384
static struct nvn_vk_tex *g_pin[FD_PIN_MAX];
static VkImageView g_pin_view[FD_PIN_MAX];   // 与 g_pin 同下标；NULL = 这笔只保底图（附件）
static int g_pin_n;
// 诊断/测试：允许把保活容量调小（只允许 <= 编译期上限），用于触达真实“登记满”分支。
static int fd_pin_capacity(void) {
    static int cap = -1;
    if (cap < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.pin_max", v);
        int n = v[0] ? atoi(v) : 0;
        cap = (n > 0 && n < FD_PIN_MAX) ? n : FD_PIN_MAX;
    }
    return cap;
}
// 帧内纹理保活：登记满时必须停止这一笔 draw 并准确失败——既不能“只 pin 不记账”（永久不可淘汰），
// 也不能让未登记的资源继续进入 descriptor/命令（完成后无法证明它没被淘汰）。
static int fd_pin_tex(struct nvn_vk_tex *t) {
    if (!t) return 0;
    if (g_pin_n >= fd_pin_capacity()) {
        FD_LOG("texture pin list full (%d of %d); refusing this draw (no unregistered resource may be used)",
               g_pin_n, fd_pin_capacity());
        return -1;
    }
    nvn_vk_texcache_pin(t);
    g_pin_view[g_pin_n] = VK_NULL_HANDLE;
    g_pin[g_pin_n++] = t;
    return 0;
}
// 视图单独保活：底图 pin 不保 view，LRU 仍可能在没有 view 引用时回收并销毁它。
// 先由 fd_pin_tex 登记底图；这里找到同一底图的未占位记录，把 view 一起记下。
static int fd_pin_view(struct nvn_vk_tex *t, VkImageView view) {
    if (!t || !view) return -1;
    // 默认整幅/附件视图跟随底图生命周期（release 只在 pins==0 时销毁），不需要也找不到视图缓存条目。
    if (view == t->view || view == t->view_att) return 0;
    for (int i = g_pin_n - 1; i >= 0; i--)
        if (g_pin[i] == t && !g_pin_view[i]) {
            if (nvn_vk_texcache_pin_view(t, view) != 0) return -1;
            g_pin_view[i] = view;
            return 0;
        }
    return -1;
}
static void fd_unpin_all(void) {
    for (int i = 0; i < g_pin_n; i++) {
        if (g_pin_view[i]) nvn_vk_texcache_unpin_view(g_pin[i], g_pin_view[i]);
        nvn_vk_texcache_unpin(g_pin[i]);
    }
    g_pin_n = 0;
    fd_lookup_clear();
}
// 只解最老一帧的 pin（延迟 present 时，前一帧的 pin 要留到它真正完成）。
// 前半段属于最老帧、后半段属于当前帧：解完把后半段搬回开头。
static void fd_unpin_prefix(int n) {
    if (n > g_pin_n) n = g_pin_n;
    for (int i = 0; i < n; i++) {
        if (g_pin_view[i]) nvn_vk_texcache_unpin_view(g_pin[i], g_pin_view[i]);
        nvn_vk_texcache_unpin(g_pin[i]);
    }
    int left = g_pin_n - n;
    if (left > 0) {
        memmove(g_pin, g_pin + n, (size_t)left * sizeof g_pin[0]);
        memmove(g_pin_view, g_pin_view + n, (size_t)left * sizeof g_pin_view[0]);
    }
    g_pin_n = left;
    fd_lookup_clear();
}

// compute dispatch 的纹理绑定入口（nvn_vk_compute.c 消费）：把 NVN 组合句柄（低 20 位纹理 id、
// 20 位起采样器 id）解析成真实 view + sampler，并按图形路径同一套语义登记保活。
// 与上面 kind==2 图形分支逐条对齐，但不涉及反馈环/附件快照：compute 的 dispatch 在自己的命令
// 缓冲里、不在 render pass 内，采样源不可能是本笔的附件。
// 成功返回 0 并 pin 底图（及非默认视图）；失败返回 -1，why 带纹理 id/采样器 id/注册代际，绝不绑占位。
int s2a_vk_nvn_texture_bind_for_compute(struct s2a_vk_device *d, uint64_t handle,
    struct nvn_vk_tex **tex_out, VkImageView *view_out, VkSampler *sampler_out,
    char *why, size_t why_size) {
    if (!d || !tex_out || !view_out || !sampler_out) return -1;
    *tex_out = NULL; *view_out = VK_NULL_HANDLE; *sampler_out = VK_NULL_HANDLE;
    uint32_t tid = (uint32_t)(handle & 0xFFFFFu), sid = (uint32_t)(handle >> 20);
    struct nvn_vk_tex *tex = NULL;
    char inner[128] = "";
    if (s2a_vk_adapter_tex_by_id((int)tid, &tex, inner, sizeof inner) || !tex || !tex->view) {
        if (why && why_size)
            snprintf(why, why_size, "texture id %u did not resolve: %s (handle=0x%llx reggen=%llu)",
                tid, inner[0] ? inner : "no view", (unsigned long long)handle,
                (unsigned long long)s2a_vk_adapter_registry_generation());
        return -1;
    }
    // 注册的 NVNtextureView 必须被真实消费（mip/层/格式覆盖）；没有覆盖字段时走默认整幅视图。
    extern int s2a_vk_adapter_tex_view_by_id(int id, view_t *out) __attribute__((weak));
    view_t rv;
    memset(&rv, 0, sizeof rv);
    int hv = s2a_vk_adapter_tex_view_by_id ? s2a_vk_adapter_tex_view_by_id((int)tid, &rv) : -1;
    const int view_override = hv == 1 && (rv.set_format || rv.set_levels || rv.set_layers ||
                                          rv.set_target || rv.set_dsmode);
    const int view_cube = view_override && rv.set_target && (rv.target == 8 || rv.target == 9);
    const int cube_view = view_cube || tex->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                          tex->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    samp_par par;
    memset(&par, 0, sizeof par);
    if (s2a_vk_adapter_samp_by_id((int)sid, &par)) {
        if (why && why_size)
            snprintf(why, why_size, "sampler id %u did not resolve (texture id %u, handle=0x%llx)",
                sid, tid, (unsigned long long)handle);
        return -1;
    }
    // 采样器取会话级缓存，不用绘制侧的 g_samp_cache：compute 的提交由 record 的保留账本单独管，
    // 完成未知时命令可能比帧队列活得更久；会话缓存在 nvn_vk_session_destroy 里、拿到设备空闲证明
    // 之后才销毁，所以这里建出的 VkSampler 不可能先于仍在途的 compute 命令被销毁。
    struct nvn_vk_session *sess = s2a_vk_adapter_session();
    if (!sess || !sess->ready) {
        if (why && why_size) snprintf(why, why_size, "compute texture binding has no live session");
        return -1;
    }
    VkSampler sampler = nvn_vk_sampler_cache_get_for_view(&sess->samplers, &par, cube_view, why, why_size);
    if (!sampler) return -1;
    VkImageView view = tex->view;
    if (view_override) {
        VkImageView vv = s2a_vk_adapter_tex_view(tex, &rv, why, why_size);
        if (!vv) return -1;
        view = vv;
    }
    // 保活：底图必 pin；缓存里的精确视图单独 pin（默认整幅/附件视图跟随底图生命周期）。
    nvn_vk_texcache_pin(tex);
    if (view != tex->view && view != tex->view_att) {
        if (nvn_vk_texcache_pin_view(tex, view) != 0) {
            nvn_vk_texcache_unpin(tex);
            if (why && why_size)
                snprintf(why, why_size, "texture id %u view is not registered for keeping", tid);
            return -1;
        }
    }
    *tex_out = tex;
    *view_out = view;
    *sampler_out = sampler;
    return 0;
}

// 解除 s2a_vk_nvn_texture_bind_for_compute 登记的保活（view 必须与解析时同一句柄）。
// 只在 compute 的 finish 拿到完成证明（或证明性未提交）后调用；完成未知时保持不动。
void s2a_vk_nvn_texture_release_for_compute(struct nvn_vk_tex *tex, VkImageView view) {
    if (!tex) return;
    if (view && view != tex->view && view != tex->view_att)
        nvn_vk_texcache_unpin_view(tex, view);
    nvn_vk_texcache_unpin(tex);
}

// pass_probe 的 host buffer：每帧首次用到时才建，flush 打印后销毁（诊断路径，不占产品常态）。
static VkBuffer fd_pp_ensure(struct s2a_vk_device *d) {
    if (g_fq.pp_buf) return g_fq.pp_buf;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)FD_FRAME_CMDS * 5 * 16;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(d->device, &bi, NULL, &g_fq.pp_buf) != VK_SUCCESS) { g_fq.pp_buf = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, g_fq.pp_buf, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (fd_devmem_alloc(d->device, &ai, &g_fq.pp_mem, FD_MEM_OTHER) != VK_SUCCESS ||
        vkBindBufferMemory(d->device, g_fq.pp_buf, g_fq.pp_mem, 0) != VK_SUCCESS ||
        vkMapMemory(d->device, g_fq.pp_mem, 0, VK_WHOLE_SIZE, 0, &g_fq.pp_map) != VK_SUCCESS) {
        if (g_fq.pp_buf) { vkDestroyBuffer(d->device, g_fq.pp_buf, NULL); g_fq.pp_buf = VK_NULL_HANDLE; }
        if (g_fq.pp_mem) { fd_devmem_release_dev(d->device, g_fq.pp_mem); g_fq.pp_mem = VK_NULL_HANDLE; }
        return VK_NULL_HANDLE;
    }
    g_fq.pp_n = 0;
    return g_fq.pp_buf;
}
static void fd_pp_release(struct s2a_vk_device *d) {
    if (g_fq.pp_map) { vkUnmapMemory(d->device, g_fq.pp_mem); g_fq.pp_map = NULL; }
    if (g_fq.pp_buf) { vkDestroyBuffer(d->device, g_fq.pp_buf, NULL); g_fq.pp_buf = VK_NULL_HANDLE; }
    if (g_fq.pp_mem) { fd_devmem_release_dev(d->device, g_fq.pp_mem); g_fq.pp_mem = VK_NULL_HANDLE; }
    g_fq.pp_n = 0;
}
// 在 end render pass 之后、颜色目标屏障之后调用；只记录 copy，不等待。
static void fd_pp_record(struct s2a_vk_device *d, VkCommandBuffer cmd, const struct fd_open_pass *op) {
    if (!op || !op->color || op->w == 0 || op->h == 0) return;
    if (g_fq.pp_n >= FD_FRAME_CMDS) return;
    if (fd_pp_ensure(d) == VK_NULL_HANDLE) return;
    uint32_t bpp = 4;
    switch (op->color_fmt) {
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: bpp = 16; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: bpp = 8; break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: bpp = 2; break;
        case VK_FORMAT_R8_UNORM: bpp = 1; break;
        case VK_FORMAT_R16_UNORM: bpp = 2; break;
        default: break;
    }
    int slot = g_fq.pp_n++;
    g_fq.pp_w[slot] = op->w; g_fq.pp_h[slot] = op->h; g_fq.pp_fmt[slot] = (uint32_t)op->color_fmt; g_fq.pp_bpp[slot] = bpp;
    snprintf(g_fq.pp_req[slot], sizeof g_fq.pp_req[slot], "%s", op->last_req);
    snprintf(g_fq.pp_prog[slot], sizeof g_fq.pp_prog[slot], "%s", op->last_prog);
    const int32_t x[5] = { (int32_t)(op->w / 2), (int32_t)(op->w / 4), (int32_t)(op->w * 3 / 4), (int32_t)(op->w / 4), (int32_t)(op->w * 3 / 4) };
    const int32_t y[5] = { (int32_t)(op->h / 2), (int32_t)(op->h / 4), (int32_t)(op->h / 4), (int32_t)(op->h * 3 / 4), (int32_t)(op->h * 3 / 4) };
    for (int i = 0; i < 5; i++) {
        VkBufferImageCopy c = {0};
        c.bufferOffset = (VkDeviceSize)(slot * 5 + i) * 16;
        c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageOffset = (VkOffset3D){ x[i], y[i], 0 };
        c.imageExtent = (VkExtent3D){ 1, 1, 1 };
        vkCmdCopyImageToBuffer(cmd, op->color, VK_IMAGE_LAYOUT_GENERAL, g_fq.pp_buf, 1, &c);
    }
}

// 诊断：debug.s2a.dump_pass_prog=<prog 前缀> 时，把匹配 pass 的整幅颜色目标拷进 host buffer，
// flush 后写到 debug.s2a.dump_fail 目录（passdump-<frame>-<n>.raw）。默认关，只用于定位偏色层。
static void fd_pass_dump_record(struct s2a_vk_device *d, VkCommandBuffer cmd, const struct fd_open_pass *op) {
    if (!op || !op->color || !op->last_prog[0] || op->w == 0 || op->h == 0) return;
    if (atomic_load(&g_draws_this_frame) < 100) return;   // 只抓世界帧，不抓载入屏
    char want[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.dump_pass_prog", want);
    if (!want[0] || strncmp(want, "prog:", 5) || strncmp(op->last_prog, want + 5, strlen(want + 5))) return;
    if (op->color_fmt != VK_FORMAT_R8G8B8A8_UNORM && op->color_fmt != VK_FORMAT_R8G8B8A8_SRGB) return;
    static _Atomic int pd_total;
    if (g_fq.pd_n >= 8) return;
    if (atomic_fetch_add(&pd_total, 1) >= 16) return;   // 全局上限，避免长时间运行写满存储
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)op->w * op->h * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void *map = NULL;
    if (vkCreateBuffer(d->device, &bi, NULL, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, buf, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (fd_devmem_alloc(d->device, &ai, &mem, FD_MEM_OTHER) != VK_SUCCESS ||
        vkBindBufferMemory(d->device, buf, mem, 0) != VK_SUCCESS ||
        vkMapMemory(d->device, mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        if (buf) vkDestroyBuffer(d->device, buf, NULL);
        if (mem) fd_devmem_release_dev(d->device, mem);
        return;
    }
    VkBufferImageCopy c = {0};
    c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = (VkExtent3D){ op->w, op->h, 1 };
    vkCmdCopyImageToBuffer(cmd, op->color, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    int slot = g_fq.pd_n++;
    g_fq.pd[slot].buf = buf; g_fq.pd[slot].mem = mem; g_fq.pd[slot].map = map;
    g_fq.pd[slot].w = op->w; g_fq.pd[slot].h = op->h; g_fq.pd[slot].slot = slot;
}

// 关闭 open pass 并交给帧队列。返回 0 = 已入队；-1 = 容量满/队列入不了队（队列进入终态，
// 命令缓冲从未提交、可安全释放；既有工作全部保留）。不能静默丢 pass（等于少画还报成功）。
// 诊断：指定序号 pass 的颜色目标整幅回读（定义见下方；pass close 里更早用到）。
static VkBuffer g_pass_dbg_buf; static VkDeviceMemory g_pass_dbg_mem; static void *g_pass_dbg_map;
static uint32_t g_pass_dbg_w, g_pass_dbg_h; static int g_pass_dbg_pending; static uint32_t g_pass_dbg_no;
static int fd_dump_pass_want(uint32_t *want_w, uint32_t *want_h);
static int fd_dump_pass_ensure(struct s2a_vk_device *d);
static void fd_pass_dbg_flush(void) {
    if (g_pass_dbg_pending && g_pass_dbg_map && g_pass_dbg_w && g_pass_dbg_h) {
        g_pass_dbg_pending = 0;
        char dir[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dump_fail", dir);
        if (dir[0] == '/') {
            char path[1200];
            snprintf(path, sizeof path, "%s/pass-%u-%ux%u.raw", dir, g_pass_dbg_no, g_pass_dbg_w, g_pass_dbg_h);
            FILE *f = fopen(path, "wb");
            if (f) {
                size_t bytes = (size_t)g_pass_dbg_w * g_pass_dbg_h * 4;
                size_t wr = fwrite(g_pass_dbg_map, 1, bytes, f);
                fclose(f);
                FD_LOG("pass rt dump: %s %ux%u bytes=%zu written=%zu", path, g_pass_dbg_w, g_pass_dbg_h, bytes, wr);
            }
        }
    }
    g_pass_dbg_no = 0;
}
static int g_close_probe;   // 诊断：失败收尾时逐步打点，定位驱动 abort 的具体调用
// reason：本次关闭的触发来源（FD_PCR_*，见上）。诊断关闭时不累加、不取钟，只多传一个枚举。
// 开关 debug.s2a.batch_cb（默认 0）：1 = 批次内共用命令缓冲；0 = 每个 pass 一个命令缓冲（旧行为）。
// 关诊断后的 A/B：exec 12.10→11.60 ms（us/命令 2.938→2.818），commands 4140/4142 可比；
// gap/fps 不变（那一窗受 GPU/帧间隔限制），所以收益体现在 CPU 侧而不是帧率。
static int fd_batch_cb_on(void) {
    static int v = -1;
    if (v < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.batch_cb", b); v = (!b[0] || atoi(b)) ? 1 : 0; }   // 默认开：关诊断后 exec 12.10→11.60ms 可重复
    return v;
}
static VkCommandBuffer g_batch_cb;    // 本批仍在录制的命令缓冲；VK_NULL_HANDLE = 无
static int g_batch_pass_n;            // 本批已收录的 pass 数

// 封口当前批次：结束命令缓冲 → 入帧队列 → 立刻提交（与原来的「攒够 batch_passes 个缓冲再提交」等价）。
// 只在这里 vkEndCommandBuffer；pass 结束不再结束命令缓冲。
static int fd_batch_seal(struct s2a_vk_device *d) {
    VkCommandBuffer cmd = g_batch_cb;
    if (!cmd) return 0;
    g_batch_cb = VK_NULL_HANDLE;
    g_batch_pass_n = 0;
    if (!d) d = g_fq.dev;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        if (d) fd_cmd_put(d, cmd);
        fd_fq_fail(FD_FQ_FAILED, "batch command buffer could not be ended");
        return -1;
    }
    if (g_fq.ncmd < fd_fq_cmd_capacity()) {
        g_fq.cmds[g_fq.ncmd] = cmd;
        g_fq.pmap[g_fq.ncmd] = NULL;
        g_fq.pid[g_fq.ncmd][0] = 0;
        g_fq.ncmd++;
        g_pass_flushes++;
        const int need = fd_batch_cb_on() ? 1 : fd_batch_passes();
        if (g_fq.ncmd - g_fq.submitted_n >= need && fd_submit_batch() != 0) {
            FD_LOG("batch seal: submit failed; the frame queue is poisoned");
            return -1;
        }
        return 0;
    }
    if (d) fd_cmd_put(d, cmd);
    fd_fq_fail(FD_FQ_FAILED, "frame queue command-buffer table is full; batch not submitted");
    return -1;
}

static int fd_pass_close(struct s2a_vk_device *d, int reason) {
    (void)d;
    if (!g_op.open) return 0;
    // 诊断（默认关）：记录本次关闭的触发来源（每个 close 只进一个触发桶）。
    if (fd_pass_close_reason_on()) fd_pass_reason_note(reason);
    // 诊断（默认关）：pass close 整体墙钟，含其中可能触发的批量提交（子项另计）。
    const int st = fd_pass_close_stats_on();
    const int64_t t_close0 = st ? fd_now_ns() : 0;
    VkCommandBuffer cmd = g_op.cmd;
    if (cmd) {
        // 一个 query 只覆盖一个 render pass 内的绘制：结束段必须发生在 vkCmdEndRenderPass 之前。
        if (g_close_probe) FD_LOG("close probe: step1 begin rp=%p fb=%p", (void *)g_op.pass, (void *)g_op.fb);
        nvn_vk_query_pass_end();
        if (g_close_probe) FD_LOG("close probe: step2 query ended");
        fd_gpu_pass_end(cmd);   // 诊断：本 pass 的 GPU 结束时间戳（EndRenderPass 之前）
        if (g_close_probe) FD_LOG("close probe: step3 gpu timestamp written");
        nvn_vk_callring_add("end rp=%p fb=%p %ux%u", (void *)g_op.pass, (void *)g_op.fb, g_op.w, g_op.h);
        vkCmdEndRenderPass(cmd);
        if (g_close_probe) FD_LOG("close probe: step4 render pass ended");
        {
            uint32_t want_w = 0, want_h = 0;
            int want = fd_dump_pass_want(&want_w, &want_h);
            int hit = 0;
            if (want >= 0) {
                if (want_w && want_h) hit = (g_op.w == want_w && g_op.h == want_h && !g_pass_dbg_pending);
                else hit = ((int)g_pass_dbg_no == want);
            }
            if (hit && g_op.color && g_op.w <= 1600 && g_op.h <= 900 && fd_dump_pass_ensure(g_op.dev) == 0) {
                VkBufferImageCopy bc = {0};
                bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                bc.imageExtent = (VkExtent3D){ g_op.w, g_op.h, 1 };
                vkCmdCopyImageToBuffer(cmd, g_op.color, VK_IMAGE_LAYOUT_GENERAL, g_pass_dbg_buf, 1, &bc);
                g_pass_dbg_w = g_op.w; g_pass_dbg_h = g_op.h; g_pass_dbg_pending = 1;
            }
            g_pass_dbg_no++;
        }
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        for (uint32_t s = 0; s < g_op.color_count && s < NVN_VK_MRT_MAX_COLOR; s++) {
            if (!g_op.color_img[s]) continue;
            b.image = g_op.color_img[s];
            b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
        }
        if (g_op.depth) {
            b.image = g_op.depth;
            b.subresourceRange = (VkImageSubresourceRange){ nvn_vk_format_has_stencil(g_op.depth_fmt)
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
        }
        if (fd_pass_probe_enabled()) fd_pp_record(d, cmd, &g_op);
        fd_pass_dump_record(d, cmd, &g_op);
        // 批次内共用命令缓冲：pass 收尾只结束 render pass（上面已经做过）与查询段，
        // 不 vkEndCommandBuffer；攒够 fd_batch_passes() 个 pass 才由 fd_batch_seal 封口入队提交。
        g_batch_pass_n++;
        if ((g_batch_pass_n >= fd_batch_passes() || !fd_batch_cb_on()) && fd_batch_seal(d) != 0) {
            FD_LOG("open pass: batch seal failed; the following work must fail");
            if (st) {
                __atomic_fetch_add(&g_pclose_ns, (uint64_t)(fd_now_ns() - t_close0), __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_pclose_n, 1, __ATOMIC_RELAXED);
            }
            memset(&g_op, 0, sizeof g_op);
            return -1;
        }
    }
    if (st) {
        __atomic_fetch_add(&g_pclose_ns, (uint64_t)(fd_now_ns() - t_close0), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_pclose_n, 1, __ATOMIC_RELAXED);
    }
    memset(&g_op, 0, sizeof g_op);
    return fd_fq_poisoned() ? -1 : 0;
}

// C1：清屏走帧队列。vk_clear.c 录好一段“只含这笔清除”的命令缓冲后调这里，
// 与 pass 一样并入队列一起提交，完成/失败后由队列统一释放；不 submit、不等 fence。
// 返回 0 = 已接手（命令缓冲所有权归帧队列）；非 0 = 没接手，调用方自己回收。
// 顺序要求：先把当前 open pass 收尾，清除才能排在前面那些 draw 之后。
int nvn_vk_first_draw_clear_enqueue(struct s2a_vk_device *d, VkCommandBuffer cmd) {
    if (!d || !cmd) return -1;
    if (!g_fq.dev || fd_fq_poisoned()) return -1;
    if (g_op.open && fd_pass_close(g_op.dev ? g_op.dev : g_fq.dev, FD_PCR_CLEAR) != 0) return -1;
    // 顺序：当前批次里可能还有已录制未封口的 pass，必须先封口入队，
    // 否则这笔 clear 的命令缓冲会排到那些 draw 前面（画面重排）。
    if (fd_batch_seal(d) != 0) return -1;
    if (g_fq.ncmd >= fd_fq_cmd_capacity()) return -1;
    g_fq.cmds[g_fq.ncmd] = cmd;
    g_fq.pmap[g_fq.ncmd] = NULL;
    g_fq.pid[g_fq.ncmd][0] = 0;
    g_fq.ncmd++;
    if (g_fq.ncmd - g_fq.submitted_n >= fd_batch_passes() && fd_submit_batch() != 0) return -1;
    return 0;
}

// 返回 1 = 这个 draw 已并入现有 pass；0 = 需要新 pass。
// 诊断：debug.s2a.skip_prog=<8 位十六进制前缀> 时跳过匹配的程序（定位“哪一笔把画面画坏”）。
// 跳过 = 不录进队列（draw 没发生），用来做逐程序的二分。
// 诊断：debug.s2a.log_rt=1 时打印每笔 draw 的渲染目标 serial（有界），用来查“某个纹理到底有没有被 GPU 写过”。
static int fd_log_rt(void) {
    static int v = -1;
    if (v < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.log_rt", b); v = (b[0] && atoi(b)) ? 1 : 0; }
    return v;
}
static int fd_skip_prog(const uint8_t sha[32]) {
    static char v[PROP_VALUE_MAX];
    static int init;
    if (!init) { init = 1; __system_property_get("debug.s2a.skip_prog", v); }
    if (!v[0] || !sha) return 0;
    char id[16];
    snprintf(id, sizeof id, "%02x%02x%02x%02x", sha[0], sha[1], sha[2], sha[3]);
    return strncmp(id, v, strlen(v)) == 0;
}
// 诊断：debug.s2a.no_pass_reuse=1 时每笔 draw 都新开 pass（仍走异步批量路径，不像 draw_probe 那样逐笔同步），
// 用来单独隔离“同附件合并 pass”这一个变量。
static int fd_no_pass_reuse(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.no_pass_reuse", b);
        v = (b[0] && atoi(b) != 0) ? 1 : 0;
    }
    return v;
}
static int fd_pass_continues(const VkImage *color_img, const VkImageView *color_views,
                             const VkFormat *color_fmts, uint32_t color_count,
                             VkImage depth, VkImageView depth_view, VkFormat dfmt,
                             uint32_t w, uint32_t h, int depth_only, uint32_t clear_mask) {
    if (!g_op.open || fd_no_pass_reuse()) return 0;
    if (g_op.color_count != color_count) return 0;
    for (uint32_t i = 0; i < color_count && i < NVN_VK_MRT_MAX_COLOR; i++) {
        if (g_op.color_img[i] != color_img[i] || g_op.color_views[i] != color_views[i] ||
            g_op.color_fmts[i] != color_fmts[i])
            return 0;
    }
    return g_op.depth == depth && g_op.depth_view == depth_view && g_op.depth_fmt == dfmt &&
           g_op.w == w && g_op.h == h && g_op.depth_only == depth_only && g_op.clear_mask == clear_mask;
}

// ---- VkRenderPass 缓存 -----------------------------------------------------
// 附件组合（逐槽颜色格式/空洞、深度格式、depth-only、深度/模板清除掩码）相同的 pass 只建一次。
// VkRenderPass 与具体图像无关；Adreno 上 vkCreateRenderPass 要编译附件状态，世界场景每帧几十次
// pass 切换，每次按 draw 新建是纯 CPU 浪费。pass 对象活到设备销毁（release_device 统一清），
// 不再交给帧队列按帧销毁；framebuffer 仍每次新建，因为它引用具体 image view，生命周期跟帧队列走。
// 缓存满时退回“本次新建、交给帧队列销毁”，不因为缓存容量少画。
struct fd_rp_key {
    uint32_t color_count;              // subpass colorAttachmentCount（含空洞）
    uint32_t bound_mask;               // 真正参与的颜色槽（bit i）
    VkFormat color_fmt[NVN_VK_MRT_MAX_COLOR];
    VkFormat depth_fmt;
    uint32_t clear_mask;               // bit0 清深度 / bit1 清模板
    uint8_t has_depth;
};
struct fd_rp_slot {
    struct fd_rp_key key;
    VkRenderPass pass;
};
#define FD_RP_CACHE 256
static struct fd_rp_slot g_rp_cache[FD_RP_CACHE];
static int g_rp_cache_n;
static uint64_t g_rp_hits, g_rp_miss, g_rp_create_ns, g_fb_create_ns, g_fb_create_n;
// 跨帧 framebuffer 缓存：同一 (render pass, 逐槽颜色视图, 深度视图, 尺寸) 只建一次。
// render pass 缓存里的 pass 活到设备销毁；framebuffer 也活到设备销毁，不能交给帧队列。
// 纹理 serial 一起入键：视图句柄被驱动复用但 serial 变化时不会误中。
#define FD_FB_CACHE 1024
struct fd_fb_slot {
    VkFramebuffer fb; VkRenderPass pass;
    uint32_t color_count;
    VkImageView color[NVN_VK_MRT_MAX_COLOR];
    uint64_t color_serial[NVN_VK_MRT_MAX_COLOR];
    VkImageView depth; uint64_t depth_serial;
    uint64_t last_use; uint32_t w, h; int used;
};
static struct fd_fb_slot g_fb_cache[FD_FB_CACHE];
static int g_fb_cache_n; static uint64_t g_fb_clock, g_fb_hits;
static VkFramebuffer fd_fb_acquire(VkDevice dev, VkRenderPass pass,
                                   const VkImageView *color, const uint64_t *color_serial,
                                   uint32_t color_count, VkImageView depth, uint64_t depth_serial,
                                   uint32_t w, uint32_t h, int *cached, char *why, size_t why_size) {
    *cached = 0;
    for (int i = 0; i < g_fb_cache_n; i++) {
        struct fd_fb_slot *e = &g_fb_cache[i];
        if (e->pass != pass || e->color_count != color_count || e->depth != depth ||
            e->depth_serial != depth_serial || e->w != w || e->h != h)
            continue;
        int same = 1;
        for (uint32_t s = 0; s < color_count; s++)
            if (e->color[s] != color[s] || e->color_serial[s] != color_serial[s]) { same = 0; break; }
        if (!same) continue;
        e->last_use = ++g_fb_clock; __atomic_fetch_add(&g_fb_hits, 1, __ATOMIC_RELAXED); *cached = 1;
        return e->fb;
    }
    VkImageView views[NVN_VK_MRT_MAX_COLOR + 1]; uint32_t n = 0;
    for (uint32_t s = 0; s < color_count; s++) if (color[s]) views[n++] = color[s];
    if (depth) views[n++] = depth;
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = pass; fbi.attachmentCount = n; fbi.pAttachments = views;
    fbi.width = w; fbi.height = h; fbi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    int64_t t0 = fd_now_ns();
    VkResult vr = vkCreateFramebuffer(dev, &fbi, NULL, &fb);
    __atomic_fetch_add(&g_fb_create_ns, (uint64_t)(fd_now_ns() - t0), __ATOMIC_RELAXED);
    if (vr != VK_SUCCESS) { snprintf(why, why_size, "vkCreateFramebuffer failed (%d)", (int)vr); return VK_NULL_HANDLE; }
    __atomic_fetch_add(&g_fb_create_n, 1, __ATOMIC_RELAXED);
    if (g_fb_cache_n < FD_FB_CACHE) {
        struct fd_fb_slot *e = &g_fb_cache[g_fb_cache_n++];
        e->fb = fb; e->pass = pass; e->color_count = color_count;
        for (uint32_t s = 0; s < color_count; s++) { e->color[s] = color[s]; e->color_serial[s] = color_serial[s]; }
        e->depth = depth; e->depth_serial = depth_serial;
        e->w = w; e->h = h; e->last_use = ++g_fb_clock; e->used = 1; *cached = 1;
    }
    return fb;
}
// 纹理视图销毁前必须把引用它的持久 framebuffer 一起销毁；否则跨帧缓存会拿到
// 已释放的 view，真机表现为进世界后闪烁/花屏（DroidDeck 的 persistent_framebuffers
// 也是靠 Image 析构时 forget_framebuffers 保活的）。调用点保证该纹理已无在途 GPU 引用。
void nvn_vk_first_draw_forget_view(VkImageView view) {
    if (!view || !g_driver_dev || !g_driver_dev->device) return;
    for (int i = 0; i < g_fb_cache_n; i++) {
        struct fd_fb_slot *e = &g_fb_cache[i];
        int hit = e->depth == view;
        for (uint32_t s = 0; !hit && s < e->color_count; s++) if (e->color[s] == view) hit = 1;
        if (!hit) continue;
        if (e->fb) vkDestroyFramebuffer(g_driver_dev->device, e->fb, NULL);
        g_fb_cache[i] = g_fb_cache[--g_fb_cache_n];
        i--;
    }
}
static void fd_fb_cache_clear(VkDevice dev) {
    if (!dev) return;
    for (int i = 0; i < g_fb_cache_n; i++) if (g_fb_cache[i].fb) vkDestroyFramebuffer(dev, g_fb_cache[i].fb, NULL);
    g_fb_cache_n = 0; g_fb_clock = 0;
}
// pass 打开成本拆三段（每帧 46 个 pass、单 pass ~120 us）：取命令缓冲 / vkBeginCommandBuffer /
// 其余（barrier + fb + begin render pass）。不拆开就不知道 5.5 ms/帧 花在哪一步。
static uint64_t g_rp_t_cget, g_rp_t_bcb, g_rp_t_rest, g_rp_t_npass;
static _Atomic uint64_t g_rp_n_cget, g_rp_n_bcb;   // 实际 reset/begin 次数（批次复用后每次开 pass 不一定发生）

// 返回 pass；*queued=0 表示缓存所有（设备销毁时清），1 表示本次新建、调用方要交给帧队列销毁。
// 失败返回 VK_NULL_HANDLE 并写 why。
static VkRenderPass fd_rp_get(VkDevice dev, const struct fd_rp_key *key, int *queued,
                              char *why, size_t why_size) {
    *queued = 0;
    for (int i = 0; i < g_rp_cache_n; i++) {
        struct fd_rp_slot *s = &g_rp_cache[i];
        if (memcmp(&s->key, key, sizeof *key) == 0) {
            __atomic_fetch_add(&g_rp_hits, 1, __ATOMIC_RELAXED);
            return s->pass;
        }
    }
    VkAttachmentDescription atts[NVN_VK_MRT_MAX_COLOR + 1];
    memset(atts, 0, sizeof atts);
    // 渲染期间用附件最优布局（GENERAL 会关闭 Adreno UBWC/压缩，整帧几十个 pass 的
    // load/store 带宽会翻倍）；initial/final 仍是 GENERAL，和采样侧保持一致。
    VkAttachmentReference color_refs[NVN_VK_MRT_MAX_COLOR];
    memset(color_refs, 0, sizeof color_refs);
    VkAttachmentReference depth_ref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    uint32_t natt = 0;
    for (uint32_t i = 0; i < key->color_count && i < NVN_VK_MRT_MAX_COLOR; i++) {
        color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (!(key->bound_mask & (1u << i))) {
            // 合法空洞：subpass 位置保留，附件引用 UNUSED；不凭空补格式。
            color_refs[i].attachment = VK_ATTACHMENT_UNUSED;
            continue;
        }
        if (key->color_fmt[i] == VK_FORMAT_UNDEFINED) {
            snprintf(why, why_size, "colour attachment %u format is undefined", i);
            return VK_NULL_HANDLE;
        }
        color_refs[i].attachment = natt;
        atts[natt].format = key->color_fmt[i];
        atts[natt].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[natt].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[natt].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[natt].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        atts[natt].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        natt++;
    }
    if (key->has_depth) {
        if (key->depth_fmt == VK_FORMAT_UNDEFINED) { snprintf(why, why_size, "depth target format is undefined"); return VK_NULL_HANDLE; }
        depth_ref.attachment = natt;
        atts[natt].format = key->depth_fmt;
        atts[natt].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[natt].loadOp = (key->clear_mask & 1u) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[natt].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[natt].stencilLoadOp = (key->clear_mask & 2u) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[natt].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        atts[natt].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        natt++;
    }
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = key->color_count;
    sub.pColorAttachments = key->color_count ? color_refs : NULL;
    // 现路径单采样（attachment samples=1），没有 resolve 附件；多采样仍未接，不为它伪造
    // resolve 目标。若以后接 MSAA，这里按颜色槽逐槽填 pResolveAttachments。
    sub.pResolveAttachments = NULL;
    sub.pDepthStencilAttachment = key->has_depth ? &depth_ref : NULL;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = natt; rpi.pAttachments = atts;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    VkRenderPass pass = VK_NULL_HANDLE;
    int64_t t0 = fd_now_ns();
    VkResult vr = vkCreateRenderPass(dev, &rpi, NULL, &pass);
    __atomic_fetch_add(&g_rp_create_ns, (uint64_t)(fd_now_ns() - t0), __ATOMIC_RELAXED);
    if (vr != VK_SUCCESS) { snprintf(why, why_size, "vkCreateRenderPass failed (%d)", (int)vr); return VK_NULL_HANDLE; }
    __atomic_fetch_add(&g_rp_miss, 1, __ATOMIC_RELAXED);
    if (g_rp_cache_n < FD_RP_CACHE) {
        struct fd_rp_slot *s = &g_rp_cache[g_rp_cache_n++];
        s->key = *key; s->pass = pass;
    } else {
        *queued = 1;   // 缓存满：本次新建的 pass 归帧队列，按旧路径回收
    }
    return pass;
}

// ---- GPU 帧时长 -------------------------------------------------------------
// 首笔 pass 开头写 t0，present blit 末尾写 t1，present fence 等完读结果。用来区分
// “present 等到 13 ms”是 GPU 真的忙，还是呈现路径有固定等待；两者该优化的地方不同。
static VkQueryPool g_gpu_pool[2];       // 逐 pass 池：按帧奇偶各一份，退休后再读，不被下一帧 reset 覆盖
static int g_gpu_on = -1, g_gpu_armed;
static int g_gpu_arm_parity = -1;       // 本帧录制用的是哪一份（-1 = 还没 arm）
// 延迟 present 下 present 返回时还拿不到 GPU 完成证明：逐 pass 池会被下一帧 arm 时的 reset
// 覆盖。这里按帧奇偶各留一个“帧起止”查询池，等对应 slot 的 render fence signal 后再读；
// 两槽最多一帧在途，同奇偶的上一帧必已退休。只在 debug.s2a.gpu_time=1 下建池。
static VkQueryPool g_gpu_frame_pool[2];
static int g_gpu_frame_pool_bad[2];
static int g_gpu_frame_parity = -1;
static int g_gpu_frame_passes[2];
static uint64_t g_gpu_frame_seq;
static double g_gpu_last_ms;
static int g_gpu_last_passes;
static int g_gpu_last_valid;
// 逐 pass 计时：每帧固定槽位（2 个时间戳/pass），present 等到完成证明后一次性读回。
// 只记 top（按耗时）与总量，不在这里做任何调度决策。
#define FD_GPU_PASSES 96
struct fd_gpu_pass { uint32_t w, h; int32_t cfmt, dfmt; uint32_t draws, blended, zdepth_off, blend_noop; uint64_t verts; uint8_t depth_only; char req[20]; };
static struct fd_gpu_pass g_gpu_meta[2][FD_GPU_PASSES];
// 当前录制中 pass 的元数据（按 arm 奇偶取对应那一份）
#define GPU_META_CUR (g_gpu_meta[(g_gpu_arm_parity >= 0 && g_gpu_arm_parity <= 1) ? g_gpu_arm_parity : 0][g_gpu_cur])
static int g_gpu_npass[2], g_gpu_cur = -1;
static int fd_gpu_enabled(void) {
    if (g_gpu_on < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.gpu_time", b);
        g_gpu_on = (b[0] && atoi(b) != 0) ? 1 : 0;
    }
    return g_gpu_on;
}
static void fd_gpu_arm(struct s2a_vk_device *d, VkCommandBuffer cmd) {
    if (!fd_gpu_enabled() || g_gpu_armed || !d || !d->device || !cmd) return;
    int p = g_arena_parity & 1;
    if (!g_gpu_pool[p]) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 * FD_GPU_PASSES;
        if (vkCreateQueryPool(d->device, &qi, NULL, &g_gpu_pool[p]) != VK_SUCCESS) { g_gpu_on = 0; return; }
    }
    vkCmdResetQueryPool(cmd, g_gpu_pool[p], 0, 2 * FD_GPU_PASSES);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_gpu_pool[p], 0);
    // 逐 pass 池与帧池都按奇偶各一份：延迟 present 下要等该帧 fence 退休才能读，不能被下一帧 reset 掉。
    if (!g_gpu_frame_pool[p] && !g_gpu_frame_pool_bad[p]) {
        VkQueryPoolCreateInfo fqi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        fqi.queryType = VK_QUERY_TYPE_TIMESTAMP; fqi.queryCount = 2;
        if (vkCreateQueryPool(d->device, &fqi, NULL, &g_gpu_frame_pool[p]) != VK_SUCCESS)
            g_gpu_frame_pool_bad[p] = 1;
    }
    if (g_gpu_frame_pool[p]) {
        vkCmdResetQueryPool(cmd, g_gpu_frame_pool[p], 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_gpu_frame_pool[p], 0);
        g_gpu_frame_parity = p;
        g_gpu_frame_passes[p] = 1;
    }
    g_gpu_arm_parity = p;
    g_gpu_npass[p] = 1; g_gpu_cur = -1;   // 槽 0 = 整帧起点，pass 从槽 1 起
    g_gpu_armed = 1;
}
// pass 开始：在 begin 之前写 t0（槽 g_gpu_npass*2-1 已用于帧起点，pass 用 2..）。
static void fd_gpu_pass_begin(struct s2a_vk_device *d, VkCommandBuffer cmd,
                              uint32_t w, uint32_t h, VkFormat cfmt, VkFormat dfmt,
                              int depth_only) {
    (void)d;
    int p = g_gpu_arm_parity;
    if (!fd_gpu_enabled() || !g_gpu_armed || !cmd || p < 0 || p > 1 || !g_gpu_pool[p]) return;
    if (g_gpu_npass[p] >= FD_GPU_PASSES) { g_gpu_cur = -1; return; }
    int idx = g_gpu_npass[p]++;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_gpu_pool[p], (uint32_t)(2 * idx));
    if (g_gpu_frame_parity >= 0) g_gpu_frame_passes[g_gpu_frame_parity]++;
    g_gpu_meta[p][idx].w = w; g_gpu_meta[p][idx].h = h;
    g_gpu_meta[p][idx].cfmt = (int32_t)cfmt; g_gpu_meta[p][idx].dfmt = (int32_t)dfmt;
    g_gpu_meta[p][idx].depth_only = (uint8_t)depth_only; g_gpu_meta[p][idx].draws = 0;
    g_gpu_meta[p][idx].blended = 0; g_gpu_meta[p][idx].zdepth_off = 0; g_gpu_meta[p][idx].blend_noop = 0;
    g_gpu_meta[p][idx].verts = 0; g_gpu_meta[p][idx].req[0] = 0;
    g_gpu_cur = idx;
}
// pass 结束：EndRenderPass 之前写 t1。
static void fd_gpu_pass_end(VkCommandBuffer cmd) {
    int p = g_gpu_arm_parity;
    if (!g_gpu_armed || g_gpu_cur < 0 || !cmd || p < 0 || p > 1 || !g_gpu_pool[p]) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gpu_pool[p], (uint32_t)(2 * g_gpu_cur + 1));
    g_gpu_cur = -1;
}
// present blit 的末尾（EndCommandBuffer 之前）写 t1。
void nvn_vk_first_draw_gpu_end(VkCommandBuffer cmd) {
    int p = g_gpu_arm_parity;
    if (!fd_gpu_enabled() || !g_gpu_armed || !cmd || p < 0 || p > 1 || !g_gpu_pool[p]) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gpu_pool[p], 1);
    if (g_gpu_frame_parity >= 0 && g_gpu_frame_pool[g_gpu_frame_parity])
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            g_gpu_frame_pool[g_gpu_frame_parity], 1);
}
// 延迟 slot 的 render fence 已 signal 后调用：读该帧奇偶的帧起止时间戳。
// 成功才更新 last；未完成/不支持保留上次值并如实返回 0，不把 0 ms 当成有效时间。
int nvn_vk_first_draw_gpu_read_frame(struct s2a_vk_device *d, int parity, const char *where) {
    if (parity < 0 || parity > 1 || !d || !d->device || !g_gpu_frame_pool[parity]) return 0;
    uint64_t r[2] = { 0, 0 };
    VkResult vr = vkGetQueryPoolResults(d->device, g_gpu_frame_pool[parity], 0, 2,
                                        sizeof r, r, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (vr != VK_SUCCESS) {
        static int once;
        if (!once++) FD_LOG("gpu frame(retire): timestamp unavailable (%d); 不把未知写成 0 ms", (int)vr);
        return 0;
    }
    double period = d->properties.limits.timestampPeriod > 0 ? d->properties.limits.timestampPeriod : 1.0;
    double ms = (double)(r[1] - r[0]) * period / 1e6;
    g_gpu_last_ms = ms;
    g_gpu_last_passes = g_gpu_frame_passes[parity];
    g_gpu_last_valid = 1;
    g_gpu_frame_seq++;
    if (where)
        FD_LOG("gpu frame(retire): %.2f ms (frame=%llu, %d passes, parity=%d)",
               ms, (unsigned long long)g_gpu_frame_seq, g_gpu_last_passes, parity);
    // 逐 pass：该帧奇偶的池在 fence 退休后仍然有效（不再被下一帧 reset 覆盖），这里读总量与前 6。
    if (g_gpu_pool[parity] && g_gpu_npass[parity] > 1) {
        int np = g_gpu_npass[parity];
        uint64_t q[2 * FD_GPU_PASSES];
        if (vkGetQueryPoolResults(d->device, g_gpu_pool[parity], 0, (uint32_t)(2 * np), sizeof q, q,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double dur[FD_GPU_PASSES];
            double busy = 0;
            for (int i = 1; i < np; i++) {
                dur[i] = (double)(q[2 * i + 1] - q[2 * i]) * period / 1e6;
                busy += dur[i];
            }
            double span_ms = ms;
            FD_LOG("gpu busy(retire): %.2f ms of %.2f ms span (%.0f%%), %d passes; 逐 pass 空闲=%s",
                   busy, span_ms, span_ms > 0 ? 100.0 * busy / span_ms : 0.0, np,
                   busy >= span_ms ? "0（含重叠）" : "见差值");
            for (int k = 0; k < 6; k++) {
                int best = -1; double bv = 0;
                for (int i = 1; i < np; i++) if (dur[i] > bv) { bv = dur[i]; best = i; }
                if (best < 0 || bv < 0.05) break;
                FD_LOG("gpu pass #%d %.2f ms %ux%u cfmt=%d dfmt=%d depth_only=%d draws=%u blended=%u zdepth_off=%u blend_noop=%u verts=%llu req=%.12s",
                       best, bv, g_gpu_meta[parity][best].w, g_gpu_meta[parity][best].h,
                       (int)g_gpu_meta[parity][best].cfmt, (int)g_gpu_meta[parity][best].dfmt,
                       (int)g_gpu_meta[parity][best].depth_only, g_gpu_meta[parity][best].draws,
                       g_gpu_meta[parity][best].blended, g_gpu_meta[parity][best].zdepth_off,
                       g_gpu_meta[parity][best].blend_noop,
                       (unsigned long long)g_gpu_meta[parity][best].verts, g_gpu_meta[parity][best].req);
                dur[best] = 0;
            }
        }
    }
    return 1;
}

uint64_t nvn_vk_first_draw_deferred_retired(void) { return atomic_load(&g_slot_retired); }

int nvn_vk_first_draw_gpu_last(double *ms, int *passes, uint64_t *frame) {
    if (!g_gpu_last_valid) return 0;
    if (ms) *ms = g_gpu_last_ms;
    if (passes) *passes = g_gpu_last_passes;
    if (frame) *frame = g_gpu_frame_seq;
    return 1;
}

// present 的渲染 fence signal 之后调用：读 t0/t1 并打一行；失败/未就绪只记一次。
void nvn_vk_first_draw_gpu_read(struct s2a_vk_device *d, const char *where) {
    int pp = g_gpu_arm_parity;
    if (!g_gpu_armed || pp < 0 || pp > 1 || !g_gpu_pool[pp] || !d || !d->device) return;
    uint64_t r[2] = { 0, 0 };
    VkResult vr = vkGetQueryPoolResults(d->device, g_gpu_pool[pp], 0, 2, sizeof r, r, sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT);
    if (vr == VK_SUCCESS) {
        (void)nvn_vk_first_draw_gpu_read_frame(d, g_gpu_frame_parity >= 0 ? g_gpu_frame_parity : 0, NULL);
        double period = d->properties.limits.timestampPeriod > 0 ? d->properties.limits.timestampPeriod : 1.0;
        FD_LOG("gpu frame: %.2f ms (%s, %d passes)", (double)(r[1] - r[0]) * period / 1e6,
               where ? where : "?", g_gpu_npass[pp]);
        // 逐 pass：读回整段，按耗时排前 6；只报 (size, 格式, draws)，不带 request 身份。
        uint64_t q[2 * FD_GPU_PASSES];
        if (vkGetQueryPoolResults(d->device, g_gpu_pool[pp], 0, (uint32_t)(2 * g_gpu_npass[pp]), sizeof q, q,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double dur[FD_GPU_PASSES];
            double busy = 0;
            for (int i = 1; i < g_gpu_npass[pp]; i++) {
                dur[i] = (double)(q[2 * i + 1] - q[2 * i]) * period / 1e6;
                busy += dur[i];
            }
            FD_LOG("gpu busy: %.2f ms of %.2f ms frame (%.0f%%); 逐 pass 之间的空闲=%s",
                   busy, (double)(r[1] - r[0]) * period / 1e6,
                   (double)(r[1] - r[0]) > 0 ? 100.0 * busy / ((double)(r[1] - r[0]) * period / 1e6) : 0.0,
                   busy >= (double)(r[1] - r[0]) * period / 1e6 ? "0（含重叠）" : "见差值");
            for (int k = 0; k < 6; k++) {
                int best = -1; double bv = 0;
                for (int i = 1; i < g_gpu_npass[pp]; i++) if (dur[i] > bv) { bv = dur[i]; best = i; }
                if (best < 0 || bv < 0.05) break;
                FD_LOG("gpu pass #%d %.2f ms %ux%u cfmt=%d dfmt=%d depth_only=%d draws=%u blended=%u zdepth_off=%u blend_noop=%u verts=%llu req=%.12s", best, bv,
                       g_gpu_meta[pp][best].w, g_gpu_meta[pp][best].h, (int)g_gpu_meta[pp][best].cfmt,
                       (int)g_gpu_meta[pp][best].dfmt, (int)g_gpu_meta[pp][best].depth_only, g_gpu_meta[pp][best].draws,
                       g_gpu_meta[pp][best].blended, g_gpu_meta[pp][best].zdepth_off, g_gpu_meta[pp][best].blend_noop,
                       (unsigned long long)g_gpu_meta[pp][best].verts, g_gpu_meta[pp][best].req);
                dur[best] = 0;
            }
        }
    } else {
        static int once;
        if (!once++) FD_LOG("gpu frame: timestamp unavailable (%d)", (int)vr);
    }
    g_gpu_armed = 0;
    g_gpu_arm_parity = -1;
}
void nvn_vk_first_draw_gpu_cancel(void) { g_gpu_armed = 0; }

// 反馈采样（附件同时当采样源）：NVN 允许这么写，读的是本笔之前附件里的内容；Vulkan 不允许
// 同一个 image 同时是附件和采样源（未定义）。做法是在 vkCmdBeginRenderPass 之前把附件内容
// 拷进一张快照，本笔采样快照。拷贝必须录在 begin 之前，所以由 fd_pass_begin 消费这两个静态。
// 同一条命令缓冲内重复绑定去重：世界一帧 500 笔 draw，同一 pipeline/viewport/scissor 常常连续出现。
// Vulkan 的动态状态在同一命令缓冲内跨 draw 保留，重复设置可以直接跳过；每个 pass 用新的命令缓冲
// 录制，所以 fd_pass_begin 里必须把影子状态清掉。
static struct {
    VkCommandBuffer cmd;      // 影子状态只对同一条命令缓冲有效（trace/probe 路径会用另一条）
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D sc;
    int vp_valid, sc_valid;
} g_cmd_state;
static VkImage g_precopy_src, g_precopy_dst;
// 快照按被采样纹理的尺寸建；拷贝区域必须取两侧交集，否则 1600x900 的 pass 去拷 1280x720 的快照
// 会越界（= 采样到未初始化内容：进世界淡入的整屏彩虹/竖条纹）。这里记下快照尺寸。
// 源子资源：附件可以是多 mip / 多层的纹理，但 SetRenderTargets 的 cview 选中的是其中一个
// mip + 一层（2D 切片）。反馈快照必须从该子资源拷，不能假设 level=0/layer=0。
static uint32_t g_precopy_w, g_precopy_h;
// 反馈快照源的完整子资源描述（3D 底图用 slice，拷贝走 srcOffset.z；见 nvn_vk_mrt.h）。
static struct nvn_vk_feedback_src g_precopy_src_desc;
// 诊断：debug.s2a.fb_mark=1 时把快照整幅染红。若画面里背景/淡入层变红，就证明它采样的正是这张
// 快照（用于把“花屏那一笔的输入”定位到反馈快照本身）。
// 诊断：debug.s2a.dump_pass=<帧内序号> 时，把该序号 pass 的颜色目标整幅回读落盘
// （回答“条纹是场景 RT 里就有，还是合成时才出现”）。
// 取值两种：纯数字 = 帧内第 N 个 pass；`WxH` = 该帧第一个尺寸匹配的 pass（更可靠：序号每帧会漂）。
static int fd_dump_pass_want(uint32_t *want_w, uint32_t *want_h) {
    static int mode = -2, w = 0, h = 0;
    if (mode == -2) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dump_pass", b);
        if (b[0] && (b[0] == '-' || b[0] == 'o' || b[0] == 'O')) { mode = -1; }   // "-1"/"off" = 关闭
        else if (b[0] && strchr(b, 'x')) {
            unsigned a = 0, c = 0;
            if (sscanf(b, "%ux%u", &a, &c) == 2 && a && c) { mode = 1; w = (int)a; h = (int)c; }
            else mode = -1;
        } else {
            mode = b[0] ? atoi(b) : -1;
        }
    }
    if (want_w) *want_w = (uint32_t)w;
    if (want_h) *want_h = (uint32_t)h;
    return mode;
}
static int fd_dump_pass_ensure(struct s2a_vk_device *d) {
    if (g_pass_dbg_buf) return 0;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)1600 * 900 * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(d->device, &bi, NULL, &g_pass_dbg_buf) != VK_SUCCESS) { g_pass_dbg_buf = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, g_pass_dbg_buf, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (vkAllocateMemory(d->device, &ai, NULL, &g_pass_dbg_mem) != VK_SUCCESS) return -1;
    if (vkBindBufferMemory(d->device, g_pass_dbg_buf, g_pass_dbg_mem, 0) != VK_SUCCESS) return -1;
    if (vkMapMemory(d->device, g_pass_dbg_mem, 0, VK_WHOLE_SIZE, 0, &g_pass_dbg_map) != VK_SUCCESS) return -1;
    return 0;
}
// 诊断：debug.s2a.dump_snapshot=1 时，把反馈快照图回读落盘（挂在 pre-copy 之后、同一个命令缓冲里），
// flush 等完 fence 后写文件。用来判定“淡入背景 = 快照内容”时，拷贝本身是否正确。
static VkBuffer g_snap_dbg_buf; static VkDeviceMemory g_snap_dbg_mem; static void *g_snap_dbg_map;
static uint32_t g_snap_dbg_w, g_snap_dbg_h; static int g_snap_dbg_pending;
static int fd_dump_snapshot(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dump_snapshot", b);
        v = (b[0] && atoi(b) != 0) ? 1 : 0;
    }
    return v;
}
static int fd_snap_dbg_ensure(struct s2a_vk_device *d, uint32_t w, uint32_t h) {
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (g_snap_dbg_buf) return need <= (VkDeviceSize)g_snap_dbg_w * g_snap_dbg_h * 4 ? 0 : -1;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)1600 * 900 * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(d->device, &bi, NULL, &g_snap_dbg_buf) != VK_SUCCESS) { g_snap_dbg_buf = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, g_snap_dbg_buf, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (vkAllocateMemory(d->device, &ai, NULL, &g_snap_dbg_mem) != VK_SUCCESS) return -1;
    if (vkBindBufferMemory(d->device, g_snap_dbg_buf, g_snap_dbg_mem, 0) != VK_SUCCESS) return -1;
    if (vkMapMemory(d->device, g_snap_dbg_mem, 0, VK_WHOLE_SIZE, 0, &g_snap_dbg_map) != VK_SUCCESS) return -1;
    return 0;
}
static int fd_fb_mark(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.fb_mark", b);
        v = (b[0] && atoi(b) != 0) ? 1 : 0;
    }
    return v;
}
static VkImageView fd_feedback_snapshot(struct s2a_vk_device *d, struct nvn_vk_tex *rt,
                                        VkFormat format, uint32_t w, uint32_t h,
                                        uint32_t level, uint32_t layer, uint32_t layers,
                                        char *why, size_t why_size) {
    if (!rt || !rt->image || format == VK_FORMAT_UNDEFINED || !w || !h) {
        snprintf(why, why_size, "反馈快照参数不完整");
        return VK_NULL_HANDLE;
    }
    if (nvn_vk_format(rt->nvn_format) != format) {
        snprintf(why, why_size, "采样纹理格式 %d 与附件格式 %d 不一致，不能用整幅快照代替采样视图",
                 (int)nvn_vk_format(rt->nvn_format), (int)format);
        return VK_NULL_HANDLE;
    }
    struct nvn_vk_feedback_src src;
    // 3D 底图的 cview 层号是深度切片：把 target/depth 一起交给计划，别按 arrayLayers 判越界。
    if (nvn_vk_feedback_src_plan(rt->nvn_target, rt->depth, rt->levels, rt->layers, level, layer, layers,
                                 w, h, &src, why, why_size) != 0)
        return VK_NULL_HANDLE;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format;
    ii.extent = (VkExtent3D){ w, h, 1 };
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    // 先看池里有没有上一帧用过、内容已可覆盖的同规格快照；有就直接复用，不再分配。
    struct fd_snap_slot *pooled = NULL;
    for (int i = 0; i < g_snap_pool_n; i++) {
        struct fd_snap_slot *sl = &g_snap_pool[i];
        if (sl->w == w && sl->h == h && sl->fmt == format && nvn_vk_snap_slot_reusable(sl->used, sl->owner_frame, g_snap_retired_frame)) {
            pooled = sl;
            break;
        }
    }
    if (pooled) {
        pooled->owner_frame = g_frame_id;
        img = pooled->img;
        g_precopy_src = rt->image; g_precopy_dst = img; g_precopy_w = w; g_precopy_h = h;
        g_precopy_src_desc = src;
        return pooled->view;
    }
    if (vkCreateImage(d->device, &ii, NULL, &img) != VK_SUCCESS) { snprintf(why, why_size, "反馈快照 vkCreateImage 失败"); return VK_NULL_HANDLE; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(d->device, img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    {
        VkResult arc = fd_devmem_alloc(d->device, &ai, &mem, FD_MEM_SNAP);
        if (arc != VK_SUCCESS) {
            atomic_fetch_add(&g_devmem_fails, 1);
            FD_LOG("反馈快照分配失败：vkAllocateMemory rc=%d(%s) size=%llu type=%u", (int)arc,
                   fd_vkresult_name(arc), (unsigned long long)ai.allocationSize, (unsigned)ai.memoryTypeIndex);
            fd_mem_report("snapshot-alloc-fail");
            snprintf(why, why_size, "反馈快照内存分配失败(vkAllocateMemory rc=%d size=%llu)",
                     (int)arc, (unsigned long long)ai.allocationSize);
            goto fail;
        }
        VkResult brc = vkBindImageMemory(d->device, img, mem, 0);
        if (brc != VK_SUCCESS) {
            FD_LOG("反馈快照绑定失败：vkBindImageMemory rc=%d size=%llu", (int)brc,
                   (unsigned long long)ai.allocationSize);
            fd_mem_report("snapshot-bind-fail");
            snprintf(why, why_size, "反馈快照绑定失败(vkBindImageMemory rc=%d)", (int)brc);
            goto fail;
        }
        // 记账已由 fd_devmem_alloc(FD_MEM_SNAP) 完成；这里不再重复加，否则同一句柄登记两次、
        // 释放只摘掉一条，账面 live 会永远偏大（正是维护者指出过的错误记账）。
    }
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(d->device, &vi, NULL, &view) != VK_SUCCESS) {
            snprintf(why, why_size, "反馈快照 vkCreateImageView 失败");
            goto fail;
        }
    }
    // 池满且没有可复用的同规格槽：淘汰一个「所属提交已完成」的最旧槽（销毁后重建为本次规格）。
    // 这是快照内存的硬上限来源：池容量 × 单张大小，不再随 draw 数无限增长。
    if (!pooled && g_snap_pool_n >= FD_SNAP_POOL) {
        int victim = -1;
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < g_snap_pool_n; i++) {
            struct fd_snap_slot *sl = &g_snap_pool[i];
            if (!nvn_vk_snap_slot_reusable(sl->used, sl->owner_frame, g_snap_retired_frame)) continue;   // 在途帧的槽不动
            if (sl->owner_frame < oldest) { oldest = sl->owner_frame; victim = i; }
        }
        if (victim >= 0) {
            struct fd_snap_slot *sl = &g_snap_pool[victim];
            if (sl->view) vkDestroyImageView(d->device, sl->view, NULL);
            if (sl->img) vkDestroyImage(d->device, sl->img, NULL);
            if (sl->mem) fd_devmem_release(d, sl->mem);
            g_snap_evicted++;
            g_snap_pool_bytes -= sl->bytes;
            memset(sl, 0, sizeof *sl);
            g_snap_pool_n--;   // 末尾槽挪到空位，保持前 n 项连续
            if (victim != g_snap_pool_n) g_snap_pool[victim] = g_snap_pool[g_snap_pool_n];
            memset(&g_snap_pool[g_snap_pool_n], 0, sizeof g_snap_pool[g_snap_pool_n]);
        }
    }
    // 字节预算：池内占用 + 本次 > 预算时，先淘汰已完成的旧槽。
    while (g_snap_pool_bytes + (uint64_t)mr.size > fd_snap_budget_bytes()) {
        int victim = -1;
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < g_snap_pool_n; i++) {
            struct fd_snap_slot *sl = &g_snap_pool[i];
            if (!nvn_vk_snap_slot_reusable(sl->used, sl->owner_frame, g_snap_retired_frame)) continue;
            if (sl->owner_frame < oldest) { oldest = sl->owner_frame; victim = i; }
        }
        if (victim < 0) break;
        struct fd_snap_slot *sl = &g_snap_pool[victim];
        if (sl->view) vkDestroyImageView(d->device, sl->view, NULL);
        if (sl->img) vkDestroyImage(d->device, sl->img, NULL);
        if (sl->mem) fd_devmem_release(d, sl->mem);
        g_snap_evicted++;
        g_snap_pool_bytes -= sl->bytes;
        g_snap_pool_n--;
        if (victim != g_snap_pool_n) g_snap_pool[victim] = g_snap_pool[g_snap_pool_n];
        memset(&g_snap_pool[g_snap_pool_n], 0, sizeof g_snap_pool[g_snap_pool_n]);
    }
    const int over_budget = g_snap_pool_bytes + (uint64_t)mr.size > fd_snap_budget_bytes();
    if (!over_budget && g_snap_pool_n < FD_SNAP_POOL) {
        g_snap_pool_bytes += (uint64_t)mr.size;
        struct fd_snap_slot *sl = &g_snap_pool[g_snap_pool_n++];
        sl->img = img; sl->mem = mem; sl->view = view;
        sl->w = w; sl->h = h; sl->fmt = format;
        sl->owner_frame = g_frame_id; sl->used = 1; sl->bytes = (uint64_t)mr.size;
    } else if (fd_fq_keep_fbimg(img, mem, view) != 0) {
        snprintf(why, why_size, "反馈快照登记失败");
        goto fail;
    }
    g_precopy_src = rt->image; g_precopy_dst = img; g_precopy_w = w; g_precopy_h = h;
    g_precopy_src_desc = src;
    if (fd_verbose())
        FD_LOG("feedback snapshot: src serial=%llu level=%u layer=%u %ux%u -> snapshot %ux%u",
               (unsigned long long)rt->serial, level, layer, w, h, w, h);
    return view;
fail:
    if (view) vkDestroyImageView(d->device, view, NULL);
    if (img) vkDestroyImage(d->device, img, NULL);
    if (mem) fd_devmem_release_dev(d->device, mem);
    return VK_NULL_HANDLE;
}

// 命令缓冲复用：每个 pass 一次 vkAllocateCommandBuffers/vkFreeCommandBuffers，世界一帧 24 次。
// 命令池归设备；只有已经拿到完成证明（flush 等完 fence）或从未提交的缓冲才回到空闲表，
// 下一帧 reset 后继续用。设备销毁时池一起回收，空闲表只清指针、不再按句柄释放。
// 批次内共用命令缓冲：一个提交批次（fd_batch_passes() 个 pass）只取一次命令缓冲、begin 一次，
// 各 pass 在同一个 primary 缓冲里 vkCmdBeginRenderPass/vkCmdEndRenderPass。
// 依据：真机实测「每次开 pass 28.6–29.7 us，其中 vkResetCommandBuffer 16.1 + vkBeginCommandBuffer 4.6」，
// 30 次/帧 → 约 0.63 ms/帧；共用后这两项按批次数（约 15 次/帧）计。
// 提交节奏不变：仍是每 fd_batch_passes() 个 pass 提交一次（原来每 2 个 pass 提交 2 个命令缓冲，
// 现在每 2 个 pass 封口 1 个命令缓冲后立刻提交，GPU 看到的工作量与提交点完全一致）。
// 顺序约束：任何「外来命令缓冲」（清屏/拷贝/逐笔 probe 命令缓冲）入队前必须先封口当前批次，
// 否则它会排到尚未入队的批次前面。
#define FD_CMD_CACHE 512
static VkCommandBuffer g_cmd_free[FD_CMD_CACHE];
static int g_cmd_free_n;
// cget=15 µs 的拆分：复用 reset 与新建 allocate 各占多少（诊断窗口用，帧结束清零）。
static uint64_t g_cmd_reset_ns, g_cmd_alloc_ns;

static VkCommandBuffer fd_cmd_get(struct s2a_vk_device *d) {
    while (g_cmd_free_n > 0) {
        VkCommandBuffer cmd = g_cmd_free[--g_cmd_free_n];
        int64_t tr0 = fd_now_ns();
        VkResult rr = vkResetCommandBuffer(cmd, 0);
        __atomic_fetch_add(&g_cmd_reset_ns, (uint64_t)(fd_now_ns() - tr0), __ATOMIC_RELAXED);
        if (rr == VK_SUCCESS) {
            // 复用命令缓冲 reset 后 pipeline/viewport/scissor 全部失效；影子状态只按句柄
            // 比较，句柄相同会误以为还绑着，下一笔 draw 跳过绑定 → VUID 08606/07831/07832，
            // Adreno 在 vkCmdDrawIndexed 内 SIGSEGV（8E 合包 VVL 实测）。
            if (g_cmd_state.cmd == cmd) {
                g_cmd_state.cmd = VK_NULL_HANDLE;
                g_cmd_state.pipe = VK_NULL_HANDLE;
                g_cmd_state.vp_valid = 0;
                g_cmd_state.sc_valid = 0;
            }
            return cmd;
        }
        // reset 失败：句柄不再可用，按“直接丢弃、重新分配”处理（不能重复入表）。
    }
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = d->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int64_t ta0 = fd_now_ns();
    VkResult ar = vkAllocateCommandBuffers(d->device, &cai, &cmd);
    __atomic_fetch_add(&g_cmd_alloc_ns, (uint64_t)(fd_now_ns() - ta0), __ATOMIC_RELAXED);
    if (ar != VK_SUCCESS) return VK_NULL_HANDLE;
    return cmd;
}

static void fd_cmd_put(struct s2a_vk_device *d, VkCommandBuffer cmd) {
    if (!cmd) return;
    if (g_cmd_free_n < FD_CMD_CACHE) { g_cmd_free[g_cmd_free_n++] = cmd; return; }
    if (d && d->device) vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
}

// 当前录制帧序号：时间戳待写项按它归属到帧，退休只结算已拿到完成证明的帧。
uint64_t nvn_vk_first_draw_frame_id(void) { return g_frame_id; }

// OP_REPORT_COUNTER(TIMESTAMP_TOP)：录一段只含 reset+时间戳的命令缓冲，按顺序并入当前帧队列。
// 不 submit、不等 fence；完成证明由帧队列统一拿，写回由查询模块在帧完成点做。
int nvn_vk_first_draw_timestamp_top(struct s2a_vk_device *d, VkQueryPool pool, uint32_t slot,
                                    char *why, size_t why_size) {
    if (!d || !d->device || !pool) {
        snprintf(why, why_size, "timestamp-top: no device/query pool");
        return -1;
    }
    // #1350：首个 TIMESTAMP_TOP 可能先于第一笔 draw 到达（#1250 初始化时序）。帧队列此前只在
    // nvn_vk_first_draw 里绑定设备，这里按需绑定到当前设备再记，后面的 draw/上屏沿用同一条队列；
    // 不能因为“还没有 draw”把整条回放打成 abort。属于旧设备的队列仍是真错误，照旧报红。
    if (!g_fq.dev) {
        if (g_fq.dev_handle && g_fq.dev_handle != d->device) {
            snprintf(why, why_size, "timestamp-top: the frame queue belongs to a previous device");
            return -1;
        }
        g_fq.dev = d;
        g_fq.dev_handle = d->device;
        FD_LOG("timestamp-top: frame queue bound on demand before the first draw");
    }
    if (fd_fq_poisoned()) {
        snprintf(why, why_size, "timestamp-top: the frame queue is not usable: %s",
                 g_fq.fail[0] ? g_fq.fail : "frame queue failed");
        return -1;
    }
    // 前面若还有 open pass，先按原顺序收进队列；时间戳不能越过已录制但未提交的绘制。
    if (g_op.open && fd_pass_close(g_op.dev ? g_op.dev : g_fq.dev, FD_PCR_QUERY) != 0) {
        snprintf(why, why_size, "timestamp-top: closing the open pass failed: %s",
                 g_fq.fail[0] ? g_fq.fail : "frame queue failed");
        return -1;
    }
    // 旁路命令缓冲（时间戳）入队前必须把已录制的前缀封口，否则它会排到那些 draw 前面。
    if (fd_batch_seal(g_fq.dev) != 0) {
        snprintf(why, why_size, "timestamp-top: sealing the open batch failed: %s",
                 g_fq.fail[0] ? g_fq.fail : "frame queue failed");
        return -1;
    }
    if (g_fq.ncmd >= fd_fq_cmd_capacity()) {
        snprintf(why, why_size, "timestamp-top: frame queue command-buffer table is full");
        return -1;
    }
    VkCommandBuffer cmd = fd_cmd_get(d);
    if (!cmd) {
        snprintf(why, why_size, "timestamp-top: command buffer allocation failed");
        return -1;
    }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        fd_cmd_put(d, cmd);
        snprintf(why, why_size, "timestamp-top: vkBeginCommandBuffer failed");
        return -1;
    }
    vkCmdResetQueryPool(cmd, pool, slot, 1);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, slot);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        fd_cmd_put(d, cmd);
        snprintf(why, why_size, "timestamp-top: vkEndCommandBuffer failed");
        return -1;
    }
    g_fq.cmds[g_fq.ncmd] = cmd;
    g_fq.pmap[g_fq.ncmd] = NULL;
    g_fq.pid[g_fq.ncmd][0] = 0;
    g_fq.ncmd++;
    if (g_fq.ncmd - g_fq.submitted_n >= fd_batch_passes() && fd_submit_batch() != 0) {
        snprintf(why, why_size, "timestamp-top: early frame-queue submit failed: %s",
                 g_fq.fail[0] ? g_fq.fail : "queue submit failed");
        return -1;
    }
    return 0;
}

static int fd_pass_begin(struct s2a_vk_device *d, VkRenderPass pass, VkFramebuffer fb,
                         const VkImage *color_img, const VkImageView *color_views,
                         const VkFormat *color_fmts, uint32_t color_count, uint32_t color_attachment_count,
                         VkImage depth, VkImageView depth_view, VkFormat dfmt,
                         uint32_t w, uint32_t h, int depth_only, uint32_t clear_mask,
                         float depth_clear_value, float stencil_clear_value) {
    // 查询页选择/回收会 flush 帧队列（封口并提交当前批次命令缓冲）：必须在取命令缓冲之前做，
    // 否则会把本 pass 正要录制的那条缓冲提交掉，再往上录命令 = 驱动崩溃。
    {
        char qwhy[200] = "";
        int qrc = nvn_vk_query_prepare(d, qwhy, sizeof qwhy);
        if (qrc < 0) {
            FD_LOG("open pass: occlusion query page prepare failed: %s", qwhy);
            return -1;
        }
    }
    // 口径：cget 只算「取句柄」（含 vkResetCommandBuffer / vkAllocateCommandBuffers），
    // bcb 只算 vkBeginCommandBuffer；两者都只在真正新建批次时发生，另记次数便于核对。
    int64_t tp0 = fd_now_ns();
    VkCommandBuffer cmd = g_batch_cb;
    const int acquired = (cmd == VK_NULL_HANDLE);
    if (acquired) {
        cmd = fd_cmd_get(d);
        if (!cmd) return -1;
    }
    int64_t tp1 = fd_now_ns();
    if (acquired) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { fd_cmd_put(d, cmd); return -1; }
        g_batch_cb = cmd;
        g_batch_pass_n = 0;
    }
    int64_t tp2 = fd_now_ns();
    if (acquired) {
        __atomic_fetch_add(&g_rp_t_cget, (uint64_t)(tp1 - tp0), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_rp_t_bcb, (uint64_t)(tp2 - tp1), __ATOMIC_RELAXED);
        atomic_fetch_add(&g_rp_n_cget, 1);
        atomic_fetch_add(&g_rp_n_bcb, 1);
    }
    fd_gpu_arm(d, cmd);   // GPU 计时：本帧第一条命令缓冲的开头写 t0（render pass 之外）
    int64_t tr_gpuarm = fd_stamp();   // 关诊断时返回 0 → 下面整段不执行，不留任何额外开销
    {
        // 计数中：整池 reset 必须在 render pass 之外、开查询之前（同一条命令缓冲内）。
        char qwhy[200] = "";
        int qrc = nvn_vk_query_cb_begin(d, cmd, qwhy, sizeof qwhy);
        if (qrc < 0) {
            FD_LOG("open pass: occlusion query setup failed: %s", qwhy);
            // 统一放弃路径：这条命令缓冲就是批次缓冲（g_batch_cb），不能直接 put 回去——
            // 那会留下一个悬空句柄，后续 pass 继续往一个已归还的缓冲里录。
            // 此刻还没有 vkCmdBeginRenderPass，所以封口是合法的；已录制的前缀如实入队。
            if (fd_batch_seal(d) != 0) return -1;
            return -1;
        }
    }
    int64_t tr_query = fd_stamp();
    if (g_precopy_src && g_precopy_dst) {
        VkImageMemoryBarrier pb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        pb.srcQueueFamilyIndex = pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pb.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pb.image = g_precopy_dst;
        pb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; pb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        pb.srcAccessMask = 0; pb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &pb);
        {
            VkImageMemoryBarrier rb = pb;
            rb.image = g_precopy_src;
            rb.oldLayout = rb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            rb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            rb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &rb);
        }
        {
            uint32_t cw = w < g_precopy_w ? w : g_precopy_w;
            uint32_t ch = h < g_precopy_h ? h : g_precopy_h;
            if (cw && ch) {
                static int noted;
                if (!noted++ && (cw != w || ch != h))
                    FD_LOG("feedback snapshot: pass=%ux%u snapshot=%ux%u; copying the %ux%u intersection "
                           "(the rest of the snapshot stays uninitialised)", w, h, g_precopy_w, g_precopy_h, cw, ch);
                VkImageCopy rc = {0};
                char cwhy[192] = "";
                if (nvn_vk_feedback_copy_make(&g_precopy_src_desc, w, h, g_precopy_w, g_precopy_h,
                                              &rc, cwhy, sizeof cwhy) != 0) {
                    static int noted_mk;
                    if (!noted_mk++) FD_LOG("feedback snapshot: copy region refused: %s", cwhy);
                } else {
                    vkCmdCopyImage(cmd, g_precopy_src, VK_IMAGE_LAYOUT_GENERAL,
                                   g_precopy_dst, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
                }
                // 只抓 1600x900 的画布快照：一帧里可能有 1280x720 与 1600x900 两种反馈快照，
                // 共用一个回读缓冲时后写的会覆盖前面的；画布那种才是“淡入/背景”的输入。
                if (fd_dump_snapshot() && cw >= 1600 && fd_snap_dbg_ensure(d, cw, ch) == 0) {
                    VkBufferImageCopy bc = {0};
                    bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    bc.imageExtent = (VkExtent3D){ cw, ch, 1 };
                    vkCmdCopyImageToBuffer(cmd, g_precopy_dst, VK_IMAGE_LAYOUT_GENERAL, g_snap_dbg_buf, 1, &bc);
                    g_snap_dbg_w = cw; g_snap_dbg_h = ch; g_snap_dbg_pending = 1;
                }
                if (fd_fb_mark()) {
                    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    VkClearColorValue red; memset(&red, 0, sizeof red);
                    red.float32[0] = 1.0f; red.float32[3] = 1.0f;
                    vkCmdClearColorImage(cmd, g_precopy_dst, VK_IMAGE_LAYOUT_GENERAL, &red, 1, &rr);
                }
            }
        }
        {
            VkImageMemoryBarrier sb = pb;
            sb.image = g_precopy_dst;
            sb.oldLayout = sb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &sb);
        }
        g_precopy_src = g_precopy_dst = VK_NULL_HANDLE; g_precopy_w = g_precopy_h = 0;
        memset(&g_precopy_src_desc, 0, sizeof g_precopy_src_desc);
    }
    int64_t tr_precopy = fd_stamp();
    VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    bar.oldLayout = bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    for (uint32_t s = 0; s < color_count && s < NVN_VK_MRT_MAX_COLOR; s++) {
        if (!color_img[s]) continue;
        bar.image = color_img[s];
        bar.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    }
    if (depth) {
        VkImageMemoryBarrier dbar = bar;
        dbar.image = depth;
        dbar.subresourceRange = (VkImageSubresourceRange){ nvn_vk_format_has_stencil(dfmt)
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        dbar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        dbar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &dbar);
    }
    int64_t tr_barrier = fd_stamp();
    VkClearValue clears[NVN_VK_MRT_MAX_COLOR + 1];
    memset(clears, 0, sizeof clears);
    if (depth) {
        // 附件声明顺序：参与颜色槽（compact）在前，深度最后；颜色全 LOAD，只有深度用 clear 值。
        uint32_t di = color_attachment_count < NVN_VK_MRT_MAX_COLOR + 1 ? color_attachment_count : 0u;
        clears[di].depthStencil.depth = depth_clear_value;
        clears[di].depthStencil.stencil = (uint32_t)stencil_clear_value;
    }
    VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass = pass; rpbi.framebuffer = fb;
    rpbi.renderArea = (VkRect2D){ {0,0}, {w, h} };
    rpbi.clearValueCount = color_attachment_count + (depth ? 1u : 0u);
    rpbi.pClearValues = clears;
    VkImageView first_view = VK_NULL_HANDLE;
    VkFormat first_fmt = VK_FORMAT_UNDEFINED;
    for (uint32_t s = 0; s < color_count && s < NVN_VK_MRT_MAX_COLOR; s++)
        if (color_views[s]) { first_view = color_views[s]; first_fmt = color_fmts[s]; break; }
    nvn_vk_callring_add("begin rp=%p fb=%p cv=%p dv=%p %ux%u clear=%u do=%d",
                        (void *)pass, (void *)fb, (void *)first_view, (void *)depth_view,
                        w, h, rpbi.clearValueCount, depth_only);
    fd_gpu_pass_begin(d, cmd, w, h, first_fmt, dfmt, depth_only);   // 诊断：本 pass 的 GPU 起始时间戳
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    int64_t tp3 = fd_now_ns();
    __atomic_fetch_add(&g_rp_t_rest, (uint64_t)(tp3 - tp2), __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_rp_t_npass, 1, __ATOMIC_RELAXED);
    // rest 细分：父项是 tp2..tp3，下面六段首尾相接、同属这批调用（分母一致）
    if (tr_gpuarm && tr_query && tr_precopy && tr_barrier && tp3) {   // 只在诊断开启时累加
        atomic_fetch_add(&g_rest_gpuarm_ns, (uint64_t)(tr_gpuarm - tp2));
        atomic_fetch_add(&g_rest_query_ns, (uint64_t)(tr_query - tr_gpuarm));
        atomic_fetch_add(&g_rest_precopy_ns, (uint64_t)(tr_precopy - tr_query));
        atomic_fetch_add(&g_rest_barrier_ns, (uint64_t)(tr_barrier - tr_precopy));
        atomic_fetch_add(&g_rest_beginrp_ns, (uint64_t)(tp3 - tr_barrier));
        atomic_fetch_add(&g_rest_sub_n, 1);
    }
    g_op.open = 1; g_op.dev = d; g_op.cmd = cmd; g_op.pass = pass; g_op.fb = fb;
    g_op.color = VK_NULL_HANDLE; g_op.color_view = VK_NULL_HANDLE; g_op.color_fmt = VK_FORMAT_UNDEFINED;
    g_op.color_count = color_count;
    for (uint32_t s = 0; s < color_count && s < NVN_VK_MRT_MAX_COLOR; s++) {
        g_op.color_img[s] = color_img[s];
        g_op.color_views[s] = color_views[s];
        g_op.color_fmts[s] = color_fmts[s];
        if (!g_op.color && color_img[s]) {
            g_op.color = color_img[s]; g_op.color_view = color_views[s]; g_op.color_fmt = color_fmts[s];
        }
    }
    for (uint32_t s = color_count; s < NVN_VK_MRT_MAX_COLOR; s++) {
        g_op.color_img[s] = VK_NULL_HANDLE;
        g_op.color_views[s] = VK_NULL_HANDLE;
        g_op.color_fmts[s] = VK_FORMAT_UNDEFINED;
    }
    g_op.depth = depth; g_op.depth_view = depth_view; g_op.depth_fmt = dfmt;
    g_op.w = w; g_op.h = h; g_op.depth_only = depth_only; g_op.clear_mask = clear_mask;
    g_pass_merges++;
    return 0;
}

// 非绘制 GPU 操作（clear/copy/dispatch/barrier/fence/render-target 变化）之前必须结束 open pass，
// 保证命令顺序与附件使用合法。只做 CPU 状态更新的 opcode 不调用。
// sync=1：后面的 opcode 会立即提交自己的 GPU 工作（clear/copy/dispatch/barrier/fence），
// 必须先把已录制的 draw 提交并等完成，否则它会在这些 draw 之前执行、打乱命令顺序。
// sync=0：只是附件/清除状态变化（SetRenderTargets/ClearDepthStencil 的 CPU 记录），
// 结束 pass 后让下一笔 draw 新开一个 pass 即可，队列顺序仍然正确。
// 诊断用：当前是否有已打开、尚未收尾的 pass。vk_record 用它在「确实关掉了一个 pass」时
// 才记一次触发源，避免把「本来就没开 pass」的 opcode 边界也算进关闭次数。
int nvn_vk_first_draw_pass_is_open(void) { return g_op.open; }

int nvn_vk_first_draw_pass_break(int sync) {
    if (g_op.open && fd_pass_close(g_op.dev ? g_op.dev : g_fq.dev,
                                   sync ? FD_PCR_GPU_OP : FD_PCR_STATE_BREAK) != 0) {
        FD_LOG("pass break: closing the open pass failed (%s); following GPU operation must fail",
               g_fq.fail[0] ? g_fq.fail : "unknown");
        return -1;
    }
    // 批次内共用命令缓冲：未封口的批次里可能还有已录制的 draw 没有进队列。
    // sync=1 的 opcode 要求"它排在那些 draw 之后"，所以必须先封口入队；否则下面的
    // fd_fq_has_inflight() 会看不到这批工作、直接跳过等待。
    if (sync && fd_batch_seal(g_op.dev ? g_op.dev : g_fq.dev) != 0) {
        FD_LOG("pass break: sealing the open batch failed (%s)", g_fq.fail[0] ? g_fq.fail : "unknown");
        return -1;
    }
    // 已关闭的 pass 还在帧队列里（cmds[submitted_n..ncmd) 未提交）。sync=1 的后续 opcode 会立即
    // 提交自己的 GPU 工作并按队列顺序执行，所以必须先按队列顺序交出这些 draw 并等它完成；
    // 是否 flush 由“有没有未提交的积压工作”决定，不能只看 g_op.open（旧实现会漏掉这一半）。
    // 需要等完成的边界：还有没提交的前缀，或者有早批次已提交但未完成（ncmd==submitted_n>0）。
    if (sync && g_fq.nbatch_fence > 0 && g_fq.ncmd == g_fq.submitted_n) {
        // 旧实现只在 ncmd>submitted_n 时 flush：早批次提交后这个条件为假，CPU 可见的 clear/copy
        // 会以为“顺序已保证”而继续。现在记录一次，证明收紧后的分支在真实帧里确实被走到。
        static int noted;
        if (!noted++)
            FD_LOG("pass break: waiting for %d early batch(es) with no unsubmitted prefix "
                   "(needs CPU-visible completion)", g_fq.nbatch_fence);
    }
    if (sync && fd_fq_has_inflight()) {
        // 带预算的 guest 等待可能留下“已提交但未完成”的后缀 fence（ncmd==submitted_n、
        // nbatch_fence==0）；这里必须把它也算作在途，否则会把在途 GPU 工作当成已完成。
        if (nvn_vk_first_draw_flush() != 0) {
            FD_LOG("pass break: pending draws did not complete (%s); the following GPU operation must fail",
                   g_fq.fail[0] ? g_fq.fail : "unknown");
            return -1;
        }
    }
    return 0;
}
const char *nvn_vk_first_draw_first_error(void) { return g_fq.fail; }
// ---- 帧内 arena：UBO/support/VB/IB 共用 host-visible 大 buffer，不再逐笔建 buffer/内存 ----
// 每套 parity 的 chunk 上限：按需创建（对齐/尺寸不满足才开新的），不做固定 8 块的死限制。
// 参考 iOS NvArena 的“按提交 fence 回收 + 不够就长”：我们的回收点是帧退休（fd_arena_reset_parity），
// 增长点就是这里；典型世界帧只用 1-3 块，成就/对话这类瞬时大帧才用得多。
// 每套 parity 的 chunk 上限：传送/读图这类“单帧上万笔”的加载帧会真的需要几百 MB 瞬时 arena，
// 24 块（384 MB）不够。参考 iOS NvArena 的按需增长，这里放到 64 块（1 GB/parity 上限，按需创建）。
// 帧临时内存（iOS NvArena 模型）：按提交 fence 回收、按需增长、小块（4 MiB）、失败可降级。
// 旧模型是“2 套 parity × 固定块数 + 帧退休重置”：单帧几千笔的加载/传送帧会把固定上限打穿，
// 块尾对齐缝隙还会反复新开块。这里改成：
//   * 每块只在被某次提交用过之后打上那次提交的 fence；
//   * 下次分配只复用 fence 已完成（或从未提交）的块，慢帧在途只会让池子暂时变大；
//   * 用完就新建（4 MiB），硬顶 FD_ARENA_CHUNKS；失败由调用方跳过该笔，不再 abort 整局。
// 帧临时内存（对照 iOS NvArena 的所有权账本）：
//   空闲池 / 当前录制批次 / 在途提交（提交编号由本文件递增分配）。
// 状态：空闲 → 录制中（rec 列表） → 已提交（submit_seq） → 完成证明 → 空闲。
// 录制中没有 fence 也不等于空闲：rec 列表里的块永远不参与复用。
struct fd_arena_chunk {
    VkBuffer buf; VkDeviceMemory mem; void *map;
    uint64_t size, used;
    uint64_t submit_seq;   // 0 = 空闲；否则归这个提交编号所有
};
#define FD_ARENA_BYTES (4u << 20)
#define FD_ARENA_CHUNKS 512
#define FD_ARENA_SOFT_BYTES (1ull << 30)
#define FD_ARENA_HARD_BYTES (2ull << 30)
static struct fd_arena_chunk g_arena_chunks[FD_ARENA_CHUNKS];
static int g_arena_chunk_n;
static uint64_t g_arena_bytes;
static uint64_t g_arena_submit_seq;
static struct fd_arena_chunk *g_arena_rec[FD_ARENA_CHUNKS];
static int g_arena_rec_n;
static struct fd_arena_chunk *g_arena_cur;
static uint64_t g_arena_align;

static int fd_arena_in_rec(struct fd_arena_chunk *c) {
    for (int i = 0; i < g_arena_rec_n; i++) if (g_arena_rec[i] == c) return 1;
    return 0;
}
static void fd_arena_reset_parity(int p) { (void)p; }   // 兼容旧调用点：回收由提交账本驱动
static void fd_arena_reset(void) { g_arena_rec_n = 0; g_arena_cur = NULL; }

static int fd_arena_new_chunk(struct s2a_vk_device *d, uint64_t want) {
    uint64_t size = FD_ARENA_BYTES > want ? FD_ARENA_BYTES : (want + 0xFFFF) & ~0xFFFFull;
    if (g_arena_bytes + size > FD_ARENA_HARD_BYTES || g_arena_chunk_n >= FD_ARENA_CHUNKS) {
        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
            "arena budget: chunks=%d bytes=%llu MB want=%llu -> fail (hard=%llu MB)",
            g_arena_chunk_n, (unsigned long long)(g_arena_bytes >> 20), (unsigned long long)want,
            (unsigned long long)(FD_ARENA_HARD_BYTES >> 20));
        return -1;
    }
    struct fd_arena_chunk *c = &g_arena_chunks[g_arena_chunk_n];
    memset(c, 0, sizeof *c);
    c->size = size;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = c->size; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkResult ar = vkCreateBuffer(d->device, &bi, NULL, &c->buf);
    if (ar != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_INFO, "S2Arm", "arena chunk create failed: vkCreateBuffer rc=%d size=%llu chunk=%d",
                            (int)ar, (unsigned long long)c->size, g_arena_chunk_n);
        return -1;
    }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, c->buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    ar = fd_devmem_alloc(d->device, &mai, &c->mem, FD_MEM_ARENA);   // arena 大块：按类别记账
    if (ar != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_INFO, "S2Arm", "arena chunk create failed: vkAllocateMemory rc=%d size=%llu mr=%llu chunk=%d",
                            (int)ar, (unsigned long long)c->size, (unsigned long long)mr.size, g_arena_chunk_n);
        vkDestroyBuffer(d->device, c->buf, NULL); c->buf = VK_NULL_HANDLE; return -1;
    }
    if (vkBindBufferMemory(d->device, c->buf, c->mem, 0) != VK_SUCCESS) {
        fd_devmem_release_dev(d->device, c->mem); vkDestroyBuffer(d->device, c->buf, NULL);
        c->buf = VK_NULL_HANDLE; c->mem = VK_NULL_HANDLE; return -1;
    }
    ar = vkMapMemory(d->device, c->mem, 0, VK_WHOLE_SIZE, 0, &c->map);
    if (ar != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_INFO, "S2Arm", "arena chunk create failed: vkMapMemory rc=%d size=%llu chunk=%d",
                            (int)ar, (unsigned long long)c->size, g_arena_chunk_n);
        fd_devmem_release_dev(d->device, c->mem); vkDestroyBuffer(d->device, c->buf, NULL);
        c->buf = VK_NULL_HANDLE; c->mem = VK_NULL_HANDLE; return -1;
    }
    g_arena_chunk_n++;
    g_arena_bytes += size;
    if (g_arena_bytes > FD_ARENA_SOFT_BYTES) {
        static int warned59 = 0;
        if (!warned59) { warned59 = 1;
            __android_log_print(ANDROID_LOG_INFO, "S2Arm", "arena soft budget crossed: chunks=%d bytes=%llu MB",
                                g_arena_chunk_n, (unsigned long long)(g_arena_bytes >> 20)); }
    }
    return 0;
}

// owner 拿到完成证明后调用：只归还这个提交编号的块。
static void fd_arena_complete(uint64_t seq) {
    if (!seq) return;
    for (int i = 0; i < g_arena_chunk_n; i++)
        if (g_arena_chunks[i].submit_seq == seq) { g_arena_chunks[i].used = 0; g_arena_chunks[i].submit_seq = 0; }
}

// 预算压力下：对最早的在途批次做有界等待，完成就归还它的块。
static int fd_arena_reclaim_oldest(struct s2a_vk_device *d) {
    if (g_fq.nbatch_fence > 0) {
        VkResult r = vkWaitForFences(d->device, 1, &g_fq.batch_fence[0], VK_TRUE, 50000000ull);
        if (r == VK_SUCCESS) { fd_arena_complete(g_fq.batch_seq[0]); return 1; }
        return 0;
    }
    if (g_fq.fence) {
        VkResult r = vkWaitForFences(d->device, 1, &g_fq.fence, VK_TRUE, 50000000ull);
        if (r == VK_SUCCESS) { fd_arena_complete(g_fq.suffix_seq); return 1; }
    }
    return 0;
}

// UBO 描述符偏移必须满足 minUniformBufferOffsetAlignment（8E/Adreno 为 256）；
// 只按 64 对齐会让 support/guest UBO 触发 VUID-VkWriteDescriptorSet-descriptorType-00327。
static uint64_t fd_ubo_align(struct s2a_vk_device *d) {
    uint64_t a = d->properties.limits.minUniformBufferOffsetAlignment;
    return a > 64 ? a : 64;
}

// 未绑定的 NVN storage 槽：NVN 允许把槽留空/绑 NULL（游戏用 NULL 表示“本槽不给”）。
// 硬件/参考实现对空槽绑一块零缓冲：读得到 0，写被丢弃。不能当成缺资源报红，也不能把
// 上一槽的缓冲借过来（那会让 shader 读到别人的数据）。
#define FD_DUMMY_SSBO_BYTES (1u << 20)   // 1 MiB 零页：shader 索引远小于它；越界读取决于设备健壮性特性
static VkBuffer g_dummy_ssbo_buf;
static VkDeviceMemory g_dummy_ssbo_mem;
static void *g_dummy_ssbo_map;
static VkDevice g_dummy_ssbo_dev;
static int fd_dummy_ssbo(struct s2a_vk_device *d, VkBuffer *buf, uint64_t *size, char *why, size_t why_size) {
    if (g_dummy_ssbo_buf && g_dummy_ssbo_dev == d->device) {
        *buf = g_dummy_ssbo_buf; *size = FD_DUMMY_SSBO_BYTES;
        return 0;
    }
    if (g_dummy_ssbo_buf) {   // 旧设备的句柄：随旧设备回收，不跨设备销毁
        g_dummy_ssbo_buf = VK_NULL_HANDLE; g_dummy_ssbo_mem = VK_NULL_HANDLE;
        g_dummy_ssbo_map = NULL; g_dummy_ssbo_dev = NULL;
    }
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = FD_DUMMY_SSBO_BYTES;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(d->device, &bi, NULL, &g_dummy_ssbo_buf) != VK_SUCCESS) {
        snprintf(why, why_size, "dummy storage buffer create failed");
        return -1;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(d->device, g_dummy_ssbo_buf, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(d->device, &ai, NULL, &g_dummy_ssbo_mem) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, g_dummy_ssbo_buf, NULL); g_dummy_ssbo_buf = VK_NULL_HANDLE;
        snprintf(why, why_size, "dummy storage buffer memory allocate failed");
        return -1;
    }
    if (vkBindBufferMemory(d->device, g_dummy_ssbo_buf, g_dummy_ssbo_mem, 0) != VK_SUCCESS ||
        vkMapMemory(d->device, g_dummy_ssbo_mem, 0, VK_WHOLE_SIZE, 0, &g_dummy_ssbo_map) != VK_SUCCESS) {
        vkFreeMemory(d->device, g_dummy_ssbo_mem, NULL); g_dummy_ssbo_mem = VK_NULL_HANDLE;
        vkDestroyBuffer(d->device, g_dummy_ssbo_buf, NULL); g_dummy_ssbo_buf = VK_NULL_HANDLE;
        snprintf(why, why_size, "dummy storage buffer map failed");
        return -1;
    }
    memset(g_dummy_ssbo_map, 0, FD_DUMMY_SSBO_BYTES);
    g_dummy_ssbo_dev = d->device;
    *buf = g_dummy_ssbo_buf; *size = FD_DUMMY_SSBO_BYTES;
    return 0;
}

// 可写 graphics storage buffer 的帧内影子：查/加。同一 guest 区间精确匹配才复用；
// 与不同区间重叠属于同一存储的两套影子，GPU 写会互相覆盖——准确报红，不猜。
static struct fd_ssbo_wb *fd_ssbo_wb_find(uint64_t guest, uint32_t size, uint32_t *delta) {
    if (delta) *delta = 0;
    for (int i = 0; i < g_fq.n_ssbo_wb; i++) {
        struct fd_ssbo_wb *w = &g_fq.ssbo_wb[i];
        if (guest >= w->guest && guest + size <= w->guest + w->size) {
            // 包含（含完全相等）：同一块影子按 offset/range 绑不同子区间，GPU 写可见。
            if (delta) *delta = (uint32_t)(guest - w->guest);
            return w;
        }
        if (guest < w->guest + w->size && w->guest < guest + size)
            return (struct fd_ssbo_wb *)-1;   // 部分重叠：两套影子会互相覆盖
    }
    return NULL;
}
static int fd_ssbo_wb_add(VkBuffer buf, void *map, uint64_t guest, uint32_t off, uint32_t size, int writable) {
    if (g_fq.n_ssbo_wb >= 16) return -1;
    struct fd_ssbo_wb *w = &g_fq.ssbo_wb[g_fq.n_ssbo_wb++];
    w->buf = buf; w->map = map; w->guest = guest; w->off = off; w->size = size;
    w->writable = writable ? 1u : 0u;
    if (writable && w->nwb < 4) { w->wb[w->nwb].off = 0; w->wb[w->nwb].size = size; w->nwb++; }
    return 0;
}
static int fd_ssbo_wb_mark(uint32_t size, uint32_t delta, struct fd_ssbo_wb *w) {
    if (delta + size > w->size) return -1;
    for (uint32_t i = 0; i < w->nwb; i++)   // 已有完全相同的可写区间就不重复记
        if (w->wb[i].off == delta && w->wb[i].size == size) { w->writable = 1; return 0; }
    if (w->nwb >= 4) return -1;
    w->wb[w->nwb].off = delta; w->wb[w->nwb].size = size; w->nwb++;
    w->writable = 1;
    return 0;
}
// 帧退休（fence 全部等到）后把可写影子拷回 guest：host-visible coherent 内存在 fence 信号后
// 对 host 可见（Vulkan fence 的可用性保证），不需要额外的 host 屏障命令。
static void fd_ssbo_wb_writeback(struct fd_frame_q *q) {
    for (int i = 0; i < q->n_ssbo_wb; i++) {
        struct fd_ssbo_wb *w = &q->ssbo_wb[i];
        if (!w->writable || !w->guest || !w->map) continue;
        for (uint32_t j = 0; j < w->nwb; j++) {
            if (!w->wb[j].size) continue;
            memcpy((void *)(uintptr_t)(w->guest + w->wb[j].off), (const uint8_t *)w->map + w->wb[j].off,
                   w->wb[j].size);
        }
    }
    q->n_ssbo_wb = 0;
}
static int fd_arena_alloc(struct s2a_vk_device *d, uint64_t size, uint64_t align, uint64_t *off, VkBuffer *buf, void **map) {
    if (!g_arena_align) g_arena_align = 64;
    if (!align) align = g_arena_align;
    int reclaimed = 0;   // 回收只尝试一次：否则“fence 已完成但块不够大”会让循环空转
    for (;;) {
        if (g_arena_cur) {
            uint64_t o = g_arena_cur->used;
            if (o > ~0ull - (align - 1)) o = ~0ull; else o = (o + align - 1) & ~(align - 1);   // 溢出保护
            if (o >= g_arena_cur->used && o <= g_arena_cur->size && size <= g_arena_cur->size - o) {
                g_arena_cur->used = o + size;
                if (off) *off = o;
                if (buf) *buf = g_arena_cur->buf;
                if (map) *map = (uint8_t *)g_arena_cur->map + o;
                return 0;
            }
        }
        struct fd_arena_chunk *pick = NULL;
        for (int i = 0; i < g_arena_chunk_n; i++) {
            struct fd_arena_chunk *c = &g_arena_chunks[i];
            if (c->submit_seq || fd_arena_in_rec(c) || c->size < size) continue;   // 录制中/在途一律跳过
            pick = c;
            break;
        }
        if (!pick && !reclaimed) {                 // 先回收最早在途提交再试（一次）
            reclaimed = 1;
            if (fd_arena_reclaim_oldest(d)) continue;
        }
        if (!pick) {
            if (fd_arena_new_chunk(d, size)) return -1;   // 真正分配失败：准确失败，不跳过
            pick = &g_arena_chunks[g_arena_chunk_n - 1];
        }
        pick->used = 0;
        g_arena_cur = pick;
        if (g_arena_rec_n < FD_ARENA_CHUNKS) g_arena_rec[g_arena_rec_n++] = pick;
    }
}

// vkQueueSubmit 成功后：本批录制的块归这个提交编号所有，直到 owner 给出完成证明。
static uint64_t fd_arena_seal_submit(void) {
    if (!g_arena_rec_n) return 0;
    uint64_t seq = ++g_arena_submit_seq;
    for (int i = 0; i < g_arena_rec_n; i++) g_arena_rec[i]->submit_seq = seq;
    g_arena_rec_n = 0;
    g_arena_cur = NULL;
    return seq;
}

// ---- 每帧描述符池 ----
static _Atomic uint64_t g_pool_created;
// 池额度必须按“本次真正分配了几个 set”扣：旧实现固定扣 4，而描述符路径在程序只用 set0 时
// 只分配 1 个，于是池被 4 倍速耗光并重建。实测重建那一下平均摊到每笔 draw 是 18 us
// （desc=23 us 里 descP=18），比分配(2)+写(1) 合起来还大一个量级。
static VkDescriptorPool fd_frame_pool(struct s2a_vk_device *d, uint32_t need_sets) {
    if (!need_sets) need_sets = 1;
    int pc = g_arena_parity & 1;
    if (!g_frame_pool[pc] || g_frame_pool_left[pc] < need_sets) {
        VkDescriptorPoolSize sizes[4] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8192 },
        };
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = 2048; pi.poolSizeCount = 4; pi.pPoolSizes = sizes;
        // 旧池里的 set 仍被本帧已录制的命令引用，不能现在销毁；挂到帧队列，flush 后统一释放。
        if (g_frame_pool[pc] && fd_fq_keep_pool(g_frame_pool[pc]) != 0) { g_frame_pool[pc] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
        if (vkCreateDescriptorPool(d->device, &pi, NULL, &g_frame_pool[pc]) != VK_SUCCESS) { g_frame_pool[pc] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
        g_frame_pool_left[pc] = 2048;
        atomic_fetch_add(&g_pool_created, 1);
    }
    g_frame_pool_left[pc] -= need_sets;
    return g_frame_pool[pc];
}

// 池的类型计数（每类 8192）通常比 maxSets(2048) 先耗尽：一个带 16 张纹理的 set 就吃掉
// 16 个 COMBINED_IMAGE_SAMPLER，512 个这样的 set 就撞顶。撞顶不该判这笔 draw 失败，
// 换一个新池重试即可——旧池里的 set 仍被已录制的命令引用，挂到帧队列，flush 完成后释放。
// 只有“挂不住且不能销毁”时才准确失败，不为了让 draw 过而丢掉仍在用的池。
// 注入：1 = 让接下来 N 次 set 分配按“池类型耗尽”失败（只用于证明换池重试真的被走到）。
static int fd_fail_desc_alloc_mode(void) { return g_prop_fail_desc_alloc; }
static _Atomic uint64_t g_pool_rotations;
static VkDescriptorPool fd_pool_rotate(struct s2a_vk_device *d, VkDescriptorPool old) {
    if (!d) return VK_NULL_HANDLE;
    if (old && fd_fq_keep_pool(old) != 0) return VK_NULL_HANDLE;
    int pc = g_arena_parity & 1;
    if (g_frame_pool[pc] == old) { g_frame_pool[pc] = VK_NULL_HANDLE; g_frame_pool_left[pc] = 0; }
    VkDescriptorPool np = fd_frame_pool(d, 4);
    if (np) atomic_fetch_add(&g_pool_rotations, 1);
    return np;
}


// 完成证明的预算版本：
//   budget_ns < 0  = 旧语义：内部上限 5 s，等不到就是终态（提交失败/超时进保留账本）；
//   budget_ns >= 0 = 最多等这么久（0 = 只提交 + 非阻塞查状态）。返回 0 = 全部完成；
//                    1 = 已提交但仍有批次未完成（不判失败、资源/fence/命令全部保留）；-1 = 终态失败。
static int fd_fq_flush_impl(int64_t budget_ns, int terminal_timeout, int defer, int frame_end) {
    if (fd_fq_poisoned()) return -1;   // 终态：不再提交、不再释放（资源可能在 GPU 上）
    // 顺序必须是：结束查询段与 render pass（fd_pass_close）→ 封口批次（vkEndCommandBuffer）→ 提交/等待。
    // 反过来会在 render pass 仍打开时 vkEndCommandBuffer，违反有效使用条件。
    if (budget_ns != 0) {
        if (g_op.open && fd_pass_close(g_op.dev ? g_op.dev : g_fq.dev, FD_PCR_FRAME_FLUSH) != 0) return -1;
        if (fd_batch_seal(g_fq.dev) != 0) return -1;
    }
    // 零预算 = 纯状态探测：不关 pass、不提交新命令，只回答“已提交部分是否全部完成”。
    // 注意 g_op.open：命令先录到 open pass，fd_pass_close 才进入 g_fq；只看 g_fq 会把
    // “已有绘制但还没关闭的 pass”当成没有在途工作，从而错误发布完成。
    if (budget_ns == 0) {
        if (g_op.open) return 1;
        // 已录制但未封口的批次同样是「有绘制还没交出去」，不能当成没有在途工作。
        if (g_batch_cb) return 1;
        if (!fd_fq_has_inflight()) return 0;
        if (g_fq.ncmd > g_fq.submitted_n) return 1;   // 有未提交前缀：无法证明目标提交已完成
        struct s2a_vk_device *pd = g_fq.dev;
        if (!pd || !pd->device) return -1;
        for (int i = 0; i < g_fq.nbatch_fence; i++) {
            VkResult st = vkGetFenceStatus(pd->device, g_fq.batch_fence[i]);
            if (st == VK_NOT_READY) return 1;
            if (st != VK_SUCCESS) {
                fd_fq_fail(FD_FQ_FAILED, "batch fence status for the frame queue is not usable");
                return -1;
            }
        }
        if (g_fq.fence) {
            VkResult st = vkGetFenceStatus(pd->device, g_fq.fence);
            if (st == VK_NOT_READY) return 1;
            if (st != VK_SUCCESS) {
                fd_fq_fail(FD_FQ_FAILED, "fence status for the frame queue is not usable");
                return -1;
            }
        }
        return 0;
    }
    if (g_op.open && fd_pass_close(g_op.dev ? g_op.dev : g_fq.dev, FD_PCR_FRAME_FLUSH) != 0) return -1;
    if (!g_fq.ncmd && !g_fq.nbatch_fence && !g_fq.fence) { g_fq.state = FD_FQ_COMPLETED; return 0; }
    struct s2a_vk_device *d = g_fq.dev;
    if (!d) { fd_fq_reset(); return -1; }
    if (g_fq.dev_handle && g_fq.dev_handle != d->device) {
        // 队列里的命令缓冲/fence 属于旧设备：既不能在新设备上等待，也不能用新设备释放它们。
        // 这不代表“已完成”，按完成未知保留并准确失败；新会话不返回旧句柄。
        fd_fq_fail(FD_FQ_FAILED, "frame queue owns command buffers/fences from a previous device");
        return -1;
    }
    g_fq.dev_handle = d->device;
    int64_t flush_t0 = fd_now_ns();
    static uint32_t flush_n; static int64_t flush_last, flush_sum, flush_wait_sum; static uint64_t flush_draw_sum;
    int64_t flush_gap = flush_last ? (flush_t0 - flush_last) / 1000 : 0;
    flush_last = flush_t0;
    const int64_t deadline = budget_ns < 0 ? flush_t0 + 5000000000ll : flush_t0 + budget_ns;

    // 1) 把还没提交的后缀交出去（带 fence）。提交失败时游标不推进、资源全部保留。
    int remain = g_fq.ncmd - g_fq.submitted_n;
    if (remain > 0) {
        if (fd_fail_submit_mode() == 1) {
            // 负例：不调用 vkQueueSubmit；命令缓冲从未执行，但这里必须按“提交失败”进入终态并保留。
            fd_fq_fail(FD_FQ_FAILED, "injected vkQueueSubmit failure (debug.s2a.fail_submit=1)");
            return -1;
        }
        // 上一次提交的 fence 可能还没 signal（带预算的 guest 轮询/超时会返回“未完成”，但之后游戏
        // 继续录制新命令）。Vulkan 不允许 reset 一个可能仍在 in-flight 的 fence，所以把旧 fence
        // 降级为“早批次”按顺序等，本次提交用新 fence。队列因此保留每个已提交前缀的完成证据。
        if (g_fq.fence) {
            if (g_fq.nbatch_fence >= FD_FRAME_BATCH_FENCES) {
                fd_fq_fail(FD_FQ_FAILED, "frame queue batch-fence table is full; cannot prove completion of "
                                         "an earlier submitted prefix");
                return -1;
            }
            g_fq.batch_fence[g_fq.nbatch_fence++] = g_fq.fence;
            g_fq.fence = VK_NULL_HANDLE;
        }
        {
            VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(d->device, &fci, NULL, &g_fq.fence) != VK_SUCCESS) {
                fd_fq_fail(FD_FQ_FAILED, "vkCreateFence failed for the frame queue");
                return -1;
            }
            g_fq_fence_dev = d->device;
        }
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = (uint32_t)remain;
        si.pCommandBuffers = &g_fq.cmds[g_fq.submitted_n];
        VkResult sres = vkQueueSubmit(d->queue, 1, &si, g_fq.fence);
        if (sres != VK_SUCCESS) {
            fd_fq_fail(FD_FQ_FAILED, "vkQueueSubmit failed for the frame queue suffix");
            return -1;
        }
        // 只有真正提交成功才推进游标：这个后缀现在进入“已提交但未完成”，不是“已完成”。
        g_fq.submitted_n = g_fq.ncmd;
        if (g_fq.state < FD_FQ_SUBMITTED) g_fq.state = FD_FQ_SUBMITTED;
        g_fq.suffix_seq = fd_arena_seal_submit();   // 后缀提交成功 → 封存本帧剩余块
        g_fq.suffix_qid = fd_query_next_batch();    // 后缀这批的查询页同样要登记归属
        nvn_vk_query_batch_submitted(g_fq.suffix_qid);
    }
    if (fd_fail_submit_mode() == 2) {
        // 负例：命令已真实提交但假装等不到完成。Vulkan 禁止此时释放/复用，必须保留全部 fence/命令。
        fd_fq_fail(FD_FQ_TIMEOUT, "injected fence timeout (debug.s2a.fail_submit=2)");
        return -1;
    }
    if (defer) {
        // 延迟 present：本帧命令已全部提交，但现在不等完成。整条队列快照进槽位，
        // g_fq 立刻清空给下一帧录制；fence/framebuffer/命令缓冲/pin 都留到退休点。
        if (g_slot_fail) return -1;
        int idx = (g_slot_cur + 1) & 1;
        if (g_slot[idx].used && fd_slot_retire(d, &g_slot[idx]) != 0) return -1;
        memcpy(&g_slot[idx].q, &g_fq, sizeof g_fq);
        g_slot[idx].ref_seq = g_frame_ref_seq;   // 该槽只退休自己这一帧的引用
        g_slot[idx].pin_n = g_pin_n;
        g_slot[idx].parity = g_arena_parity & 1;
        g_slot[idx].frame_id = g_frame_id;
        g_slot[idx].used = 1;
        g_slot_cur = idx;
        g_arena_parity ^= 1;   // 下一帧用另一套；旧一套等退休时再重置
        g_frame_id++;          // 下一帧的命令归属新的序号
        // 诊断回读 buffer（pass_probe 的 pp_* 与 passdump 的 pd[]）随快照归槽位：
        // fd_fq_reset 不清这些字段，不清空的话槽退休销毁后 g_fq 仍持死句柄，
        // 下一帧 fd_pp_record 会往已销毁 buffer 录 copy（8E 实测 VUID + SEGV）。
        g_fq.pp_buf = VK_NULL_HANDLE; g_fq.pp_mem = VK_NULL_HANDLE;
        g_fq.pp_map = NULL; g_fq.pp_n = 0;
        g_fq.pd_n = 0;
        fd_fq_reset();
        fd_timing_frame_boundary();
        return 0;
    }
    // 2) 完成证明按提交顺序全部等到：先早批次，再本次后缀。ncmd==submitted_n>0 也不能跳过等待。
    //    预算用尽时分成两种情况：guest 的有限超时（非终态：保留全部在途资源，下次继续等）和
    //    内部分配用的完成点（终态失败，不假装完成）。零预算只用 vkGetFenceStatus 查，不阻塞。
    int incomplete = 0;
    for (int i = 0; i < g_fq.nbatch_fence && !incomplete; i++) {
        int64_t left = deadline - fd_now_ns();
        if (left <= 0) {
            VkResult st = vkGetFenceStatus(d->device, g_fq.batch_fence[i]);
            if (st == VK_NOT_READY) { incomplete = 1; break; }
            if (st != VK_SUCCESS) {
                FD_LOG("frame flush: batch fence %d/%d status=%d", i + 1, g_fq.nbatch_fence, (int)st);
                fd_fq_fail(FD_FQ_FAILED, "batch fence status for the frame queue is not usable");
                return -1;
            }
            fd_arena_complete(g_fq.batch_seq[i]);   // 有完成证明才归还
            // 同步完成路径同样要退休这批查询页：只调 resolve 只结算报告、不回收页，页会攒到池满。
            if (s2a_qret_slot(&g_fq.batch_qid[i], fd_query_retire_cb, NULL) != 0) return -1;
            continue;
        }
        VkResult wr = vkWaitForFences(d->device, 1, &g_fq.batch_fence[i], VK_TRUE, (uint64_t)left);
        if (wr == VK_TIMEOUT) { incomplete = 1; break; }
        if (wr != VK_SUCCESS) {
            FD_LOG("frame flush: batch fence %d/%d wait result=%d", i + 1, g_fq.nbatch_fence, (int)wr);
            fd_fq_fail(FD_FQ_FAILED, "batch fence wait for the frame queue did not complete");
            return -1;
        }
        fd_arena_complete(g_fq.batch_seq[i]);
        if (s2a_qret_slot(&g_fq.batch_qid[i], fd_query_retire_cb, NULL) != 0) return -1;
    }
    if (!incomplete && g_fq.fence) {
        int64_t left = deadline - fd_now_ns();
        if (left <= 0) {
            VkResult st = vkGetFenceStatus(d->device, g_fq.fence);
            if (st == VK_NOT_READY) incomplete = 1;
            else if (st == VK_SUCCESS) {
                fd_arena_complete(g_fq.suffix_seq);
                if (s2a_qret_slot(&g_fq.suffix_qid, fd_query_retire_cb, NULL) != 0) return -1;
            }
            else if (st != VK_SUCCESS) {
                FD_LOG("frame flush: fence status=%d", (int)st);
                fd_fq_fail(FD_FQ_FAILED, "fence status for the frame queue is not usable");
                return -1;
            }
        } else {
            VkResult wr = vkWaitForFences(d->device, 1, &g_fq.fence, VK_TRUE, (uint64_t)left);
            if (wr == VK_TIMEOUT) incomplete = 1;
            else if (wr == VK_SUCCESS) {
                fd_arena_complete(g_fq.suffix_seq);
                if (s2a_qret_slot(&g_fq.suffix_qid, fd_query_retire_cb, NULL) != 0) return -1;
            }
            else if (wr != VK_SUCCESS) {
                FD_LOG("frame flush: fence wait result=%d", (int)wr);
                fd_fq_fail(FD_FQ_FAILED, "fence wait for the frame queue did not complete");
                return -1;
            }
        }
    }
    if (incomplete) {
        if (terminal_timeout) {
            FD_LOG("frame flush: completion budget exhausted (%lld us)", (long long)budget_ns);
            fd_fq_fail(FD_FQ_TIMEOUT, "fence wait for the frame queue did not complete");
            return -1;
        }
        // guest 可见的“还没完成”：命令已提交、资源全部保留；下一次 flush/等待继续。
        return 1;
    }
    // 同步 flush 路径的完成证明：本帧所有 fence 已等到。快照池只按“最后已退休帧号”判定可复用/可淘汰。
    if (g_frame_id > g_snap_retired_frame) g_snap_retired_frame = g_frame_id;
    // 到这里队列里的每个前缀都有完成证明：先把已计数的 query 结果取走（Report 的回写依赖它），
    // 再销毁完成证据、释放命令缓冲。读不到就进入终态：不写假计数、不继续按“已完成”回收。
    {
        char qwhy[200] = "";
        int qrc = nvn_vk_query_resolve(d, qwhy, sizeof qwhy);
        if (qrc < 0) {
            fd_fq_fail(FD_FQ_FAILED, qwhy[0] ? qwhy : "samples-passed query results could not be read");
            return -1;
        }
    }
    {
        // 本帧队列的 fence 都等到了：只结算属于本帧及更早序号的待写项。
        extern int nvn_vk_ts_top_resolve(struct s2a_vk_device *d, uint64_t completed_frame_id,
                                         int blocking, char *why, size_t why_size) __attribute__((weak));
        if (nvn_vk_ts_top_resolve) {
            char twhy[200] = "";
            if (nvn_vk_ts_top_resolve(d, g_frame_id, 0, twhy, sizeof twhy) != 0) {
                fd_fq_fail(FD_FQ_FAILED, twhy[0] ? twhy : "timestamp-top results could not be read");
                return -1;
            }
        }
    }
    // 走到这里 = 本帧每个前缀的 fence 都已等到（完成证明在手）：批量 fence 回池，后缀 fence 销毁。
    for (int i = 0; i < g_fq.nbatch_fence; i++) {
        fd_fence_put_back(d->device, g_fq.batch_fence[i]);
        g_fq.batch_fence[i] = VK_NULL_HANDLE;
    }
    g_fq.nbatch_fence = 0;
    int64_t wait_us = (fd_now_ns() - flush_t0) / 1000;
    flush_wait_sum += wait_us; flush_draw_sum += (uint32_t)g_fq.ncmd;
    flush_sum += flush_gap;
    if (++flush_n % 16 == 0) {
        int64_t avg_gap_ms = flush_sum / 16 / 1000;
        FD_LOG("frame flush: avg draws=%llu wait=%lld us gap=%lld ms (~%lld fps)",
               (unsigned long long)(flush_draw_sum / 16), (long long)(flush_wait_sum / 16), (long long)avg_gap_ms,
               (long long)(avg_gap_ms > 0 ? 1000 / avg_gap_ms : 0));
        flush_sum = flush_wait_sum = 0; flush_draw_sum = 0;
    }
    // 读回挂起的 probe 并补日志（draw 时只录制，present 前统一完成）
    for (int i = 0; i < g_fq.ncmd; i++) {
        void *pm = g_fq.pmap[i];
        if (!pm) continue;
        const uint8_t *px = (const uint8_t *)pm;
        uint32_t bpp = g_fq.pbpp[i];
        int changed = 0;
        for (int k = 0; k < 5; k++)
            if (memcmp(px + k * 16, px + (5 + k) * 16, bpp) != 0) changed++;
        FD_LOG("deferred probe request=%s rt=%ux%u bpp=%u changed=%d/5 before[0]=%08x after[0]=%08x",
               g_fq.pid[i], g_fq.pw[i], g_fq.ph[i], bpp, changed,
               bpp >= 4 ? *(const uint32_t *)(px + 0) : 0, bpp >= 4 ? *(const uint32_t *)(px + 5 * 16) : 0);
        {
            const uint8_t *pa = px + 5 * 16;
            if (changed && pa[1] > 180 && pa[0] < 60 && pa[2] < 60)
                FD_LOG("green-detect: request=%s rt=%ux%u before=%02x%02x%02x%02x after=%02x%02x%02x%02x",
                       g_fq.pid[i], g_fq.pw[i], g_fq.ph[i], px[0], px[1], px[2], px[3], pa[0], pa[1], pa[2], pa[3]);
        }
        vkUnmapMemory(d->device, g_fq.pmem[i]);
        vkDestroyBuffer(d->device, g_fq.pbuf[i], NULL);
        fd_devmem_release_dev(d->device, g_fq.pmem[i]);
    }
    fd_pass_dbg_flush();
    if (g_snap_dbg_pending && g_snap_dbg_map && g_snap_dbg_w && g_snap_dbg_h) {
        g_snap_dbg_pending = 0;
        char dir[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dump_fail", dir);
        if (dir[0] == '/') {
            char path[1200];
            snprintf(path, sizeof path, "%s/fb-snapshot-%ux%u.raw", dir, g_snap_dbg_w, g_snap_dbg_h);
            FILE *f = fopen(path, "wb");
            if (f) {
                size_t bytes = (size_t)g_snap_dbg_w * g_snap_dbg_h * 4;
                size_t wr = fwrite(g_snap_dbg_map, 1, bytes, f);
                fclose(f);
                FD_LOG("fb snapshot dump: %s %ux%u bytes=%zu written=%zu", path, g_snap_dbg_w, g_snap_dbg_h, bytes, wr);
            }
        }
    }
    // pass_probe：逐 pass 打印颜色目标的 5 点像素（本帧所有 pass 共用一块 host buffer）。
    fd_pp_print_q(&g_fq);
    if (g_fq.pp_buf) fd_pp_release(d);
    if (g_fq.pd_n > 0) {
        char dir[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dump_fail", dir);
        for (int i = 0; i < g_fq.pd_n; i++) {
            if (dir[0] && g_fq.pd[i].map) {
                char path[PROP_VALUE_MAX + 96];
                snprintf(path, sizeof path, "%s/passdump-%llu-%d.raw", dir,
                         (unsigned long long)g_frame_seq, i);
                FILE *f = fopen(path, "wb");
                if (f) {
                    size_t bytes = (size_t)g_fq.pd[i].w * g_fq.pd[i].h * 4;
                    size_t wr = fwrite(g_fq.pd[i].map, 1, bytes, f);
                    fclose(f);
                    FD_LOG("pass dump: wrote %s %zu/%zu B", path, wr, bytes);
                }
            }
            vkUnmapMemory(d->device, g_fq.pd[i].mem);
            vkDestroyBuffer(d->device, g_fq.pd[i].buf, NULL);
            fd_devmem_release_dev(d->device, g_fq.pd[i].mem);
            g_fq.pd[i].map = NULL; g_fq.pd[i].buf = VK_NULL_HANDLE; g_fq.pd[i].mem = VK_NULL_HANDLE;
        }
    }
    g_fq.pd_n = 0;
    for (int i = 0; i < g_fq.nbuf; i++) { if (g_fq.bufs[i]) vkDestroyBuffer(d->device, g_fq.bufs[i], NULL); if (g_fq.mems[i]) fd_devmem_release_dev(d->device, g_fq.mems[i]); }
    for (int i = 0; i < g_fq.nsamp; i++) vkDestroySampler(d->device, g_fq.samps[i], NULL);
    for (int i = 0; i < g_fq.npool; i++) vkDestroyDescriptorPool(d->device, g_fq.pools[i], NULL);
    for (int i = 0; i < g_fq.nfb; i++) vkDestroyFramebuffer(d->device, g_fq.fbs[i], NULL);
    for (int i = 0; i < g_fq.npass; i++) vkDestroyRenderPass(d->device, g_fq.passes[i], NULL);
    // 到这里本周期所有提交都已有完成证明（fence 已等到）；快照池的“最后已退休帧号”已在上面的完成点推进。
    for (int i = 0; i < g_fq.nfbimg; i++) {
        if (g_fq.fbview[i]) vkDestroyImageView(d->device, g_fq.fbview[i], NULL);
        if (g_fq.fbimg[i]) vkDestroyImage(d->device, g_fq.fbimg[i], NULL);
        if (g_fq.fbmem[i]) fd_devmem_release(d, g_fq.fbmem[i]);   // 统一入口：按类别扣减
    }
    for (int i = 0; i < g_fq.npend; i++) if (g_fq.pend[i]) vkDestroyPipeline(d->device, g_fq.pend[i], NULL);
    // 这些命令缓冲已经等完 fence：回到空闲表供下一帧复用，不再每帧 alloc/free。
    for (int i = 0; i < g_fq.ncmd; i++) fd_cmd_put(d, g_fq.cmds[i]);
    if (g_fq.fence) { fd_fence_put_back(d->device, g_fq.fence); g_fq.fence = VK_NULL_HANDLE; }
    fd_arena_reset();
    fd_timing_frame_boundary();   // 帧边界决定下一帧要不要逐笔打点（debug.s2a.perf_sample）
    // 帧完成点：到这里 CPU 已拿到完成证明（fence 已等到、命令缓冲已释放），
    // 把延迟的遮挡查询报告一次性写回。此刻查询结果都已就绪，非阻塞路径直接取到值。
    {
        extern int nvn_vk_query_resolve(struct s2a_vk_device *d, char *why, size_t why_size) __attribute__((weak));
        if (nvn_vk_query_resolve && d) {
            char qwhy[200] = "";
            if (nvn_vk_query_resolve(d, qwhy, sizeof qwhy) != 0)
                FD_LOG("query report writeback at frame completion failed: %s", qwhy);
        }
    }
    // 到这里 CPU 已拿到完成证明（上面刚等完 fence、释放了命令缓冲），本池里的 set 不再被任何
    // 在途命令引用，可以整池复位复用。旧实现每帧 vkDestroyDescriptorPool + 下一帧再建一个
    // 4x8192 的池：实测 872 次重建、累计 3.5 s CPU（desc 里 descP=29 us/笔，其余分配+写只有 3 us）。
    // 复位与销毁的寿命前提完全相同，只是不再重复分配描述符内存。
    {
        int pc = g_arena_parity & 1;
        if (g_frame_pool[pc]) {
            if (vkResetDescriptorPool(d->device, g_frame_pool[pc], 0) == VK_SUCCESS) {
                g_frame_pool_left[pc] = 2048;
            } else {
                vkDestroyDescriptorPool(d->device, g_frame_pool[pc], NULL);
                g_frame_pool[pc] = VK_NULL_HANDLE;
                g_frame_pool_left[pc] = 0;
            }
        }
    }
    for (int i = 0; i < g_fq.nbatch_fence; i++) fd_arena_complete(g_fq.batch_seq[i]);
    fd_arena_complete(g_fq.suffix_seq);
    // 兜底：走到「整帧已完成」时把还没退休的查询批次一并退休（槽位退休后为 0，重复调用是 no-op）。
    if (s2a_qret_prefix(g_fq.batch_qid, g_fq.nbatch_fence, fd_query_retire_cb, NULL) != 0 ||
        s2a_qret_slot(&g_fq.suffix_qid, fd_query_retire_cb, NULL) != 0) return -1;
    g_frame_seq++;   // 本帧已全部完成；下一帧起，未被管线引用的程序对象才可回收
    fd_pass_reason_tick();   // 同步 flush 的帧完成点：与延迟 present 用同一个 16 帧窗口
    g_frame_id++;    // 下一批时间戳归属新帧；本帧的待写项已在上面的 resolve 结算
    if (frame_end) g_arena_parity ^= 1;   // 下一帧用另一套 arena/池（延迟 present 时旧一套还在 GPU 上）
    fd_fq_reset();
    g_fq.state = FD_FQ_COMPLETED;
    return 0;
}

// 同步边界：先把延迟帧收口（查询结果/资源不能晚到），再按原语义 flush 本帧。
int nvn_vk_first_draw_flush(void) {
    if (fd_present_defer() && nvn_vk_first_draw_retire_all() != 0) return -1;
    return fd_fq_flush_impl(-1, 1, 0, 0);
}

int nvn_vk_first_draw_flush_ready(int64_t budget_ns) {
    if (fd_present_defer() && nvn_vk_first_draw_retire_all() != 0) return -1;
    return fd_fq_flush_impl(budget_ns, 0, 0, 0);
}

// present 专用：提交本帧后缀但不等完成（debug.s2a.present_defer=1 时生效）。
int nvn_vk_first_draw_flush_defer(void) {
    if (!fd_present_defer()) return nvn_vk_first_draw_flush();
    if (nvn_vk_first_draw_retire_all() != 0) return -1;
    return fd_fq_flush_impl(-1, 1, 1, 1);
}

// swapchain 把 present blit 的渲染 fence/命令缓冲交给最近一个延迟槽，退休点统一等/释放。
void nvn_vk_first_draw_slot_blit(VkFence fence, VkCommandBuffer cmd) {
    if (g_slot_cur < 0 || !g_slot[g_slot_cur].used) {
        if (cmd && g_fq.dev) vkFreeCommandBuffers(g_fq.dev->device, g_fq.dev->command_pool, 1, &cmd);
        return;
    }
    g_slot[g_slot_cur].blit_fence = fence;
    g_slot[g_slot_cur].blit_cmd = cmd;
    // 延迟路径的 present blit 已写完 t1：下一帧录制必须重新 arm 并 reset 自己的奇偶池。
    // 若一直保留 g_gpu_armed，fd_gpu_arm 会在第一帧后全部提前返回，t0 停在首帧，
    // 之后每个 t1 都减到几十秒前的 t0（真机复现：25 s 的“GPU 帧”）。
    g_gpu_armed = 0;
    g_gpu_frame_parity = -1;
}

// 一次上屏完成（present 的 fence 已等到）后调用：解除本帧纹理 pin 并清逐帧查找缓存。
// 不能放进 flush：同一帧内 barrier/capacity 会多次 flush，提前 unpin 会让缓存槽被淘汰/复用。
void nvn_vk_first_draw_present_done(void) {
    fd_unpin_all();
}

// QueueFinish 的语义：把积压的命令提交并等完成，**不销毁会话/设备**。
// 返回 0 = 队列已完成（没有积压或已等到 fence）；非 0 = 提交/等待失败且资源仍保留。
int nvn_vk_first_draw_finish(void) {
    if (fd_fq_poisoned()) {
        FD_LOG("finish: queue is in a terminal state (%s); not retrying, %d command buffer(s) retained",
               g_fq.fail, g_fq.ncmd);
        return -2;
    }
    if (g_op.open && fd_pass_close(g_op.dev ? g_op.dev : g_fq.dev, FD_PCR_FRAME_FLUSH) != 0) return -2;
    if (!g_fq.dev) return 0;
    // 批次里可能还有已录制、未达封口阈值的 pass（例如只录了 1 笔就 Finish）：必须先封口，
    // 否则下面的 fd_fq_has_inflight() 看不到这批工作，会直接报“已完成”。
    if (g_batch_cb && fd_batch_seal(g_fq.dev) != 0) return -2;
    // 早批次已提交但未完成时 ncmd==submitted_n>0：QueueFinish 必须等这些 fence，不能直接报“已完成”。
    if (!fd_fq_has_inflight()) return 0;
    if (nvn_vk_first_draw_flush() != 0) {
        FD_LOG("finish: flush failed (%s); %d command buffer(s) retained", g_fq.fail, g_fq.ncmd);
        return -2;
    }
    if (g_fq.ncmd) {
        FD_LOG("finish: %u commands still pending after flush", g_fq.ncmd);
        return -1;
    }
    return 0;
}

// 设备即将销毁（真正的会话退出）：清掉属于旧设备的 fence/描述符池/管线缓存/arena 记账。
// session 完成终局结算后显式销毁设备对象，不能在新设备上继续使用。
// 宿主内存里的编译结果与 SPIR-V 不归设备所有，保留。
void nvn_vk_first_draw_release_device(struct s2a_vk_device *d) {
    // 只由 session 在设备空闲、保留账本结清且呈现退休之后调用。
    if (!d || !d->device) return;
    /* C4b：先停管线 worker 再动设备对象——在飞 job 引用 pobj->playout 与全局 rp 缓存
       的 render pass。stop 有界（最多等在途那一次 vkCreateGraphicsPipelines 出队）；
       仍未退出就如实记一行：迟到结果由 worker 内销毁，pend 的 pin 全部归还。 */
    if (nvn_vk_pipe_worker_stop) {
        char why[160] = "";
        int src = nvn_vk_pipe_worker_stop(2000, why, sizeof why);
        if (src == 1) {
            FD_LOG("pipe async: worker still creating at release (%s); second bounded stop",
                   why[0] ? why : "timeout");
            src = nvn_vk_pipe_worker_stop(4000, why, sizeof why);
            if (src == 1)
                FD_LOG("【红】pipe async: worker did not exit after second stop; in-flight create "
                       "outlives device teardown");
        }
    }
    for (int i = 0; i < FD_PIPE_PEND; i++) fd_pipe_pend_drop(i);
    for (int i = 0; i < g_fq.npend; i++)
        if (g_fq.pend[i]) vkDestroyPipeline(d->device, g_fq.pend[i], NULL);
    for (int i = 0; i < FD_PIPE_CACHE; i++) {
        if (g_pipe_cache[i].used && g_pipe_cache[i].pipe)
            vkDestroyPipeline(d->device, g_pipe_cache[i].pipe, NULL);
    }
    for (int i = 0; i < FD_PROG_OBJ; i++) fd_prog_obj_destroy(d, &g_prog_obj[i]);
    // render pass 缓存归本设备：必须在 vkDestroyDevice 之前销毁（release_device 的调用点满足）。
    // 设备销毁后再销毁会拿旧句柄；这里清空缓存，下一会话重新建。
    if (d && d->device) {
        for (int i = 0; i < g_rp_cache_n; i++)
            if (g_rp_cache[i].pass) vkDestroyRenderPass(d->device, g_rp_cache[i].pass, NULL);
    }
    g_rp_cache_n = 0;
    // 反馈快照池也可能早于驱动缓存初始化；使用 session 给出的真实设备。
    for (int i = 0; i < g_snap_pool_n; i++) {
        struct fd_snap_slot *sl = &g_snap_pool[i];
        if (sl->view) vkDestroyImageView(d->device, sl->view, NULL);
        if (sl->img) vkDestroyImage(d->device, sl->img, NULL);
        if (sl->mem) fd_devmem_release_dev(d->device, sl->mem);
    }
    memset(g_snap_pool, 0, sizeof g_snap_pool);
    g_snap_pool_n = 0;
    g_snap_pool_bytes = 0;
    g_snap_retired_frame = 0; // 新会话帧号从 1 开始，不能继承旧退休证明。
    g_snap_evicted = 0;
    atomic_store(&g_devmem_snap_bytes, (uint64_t)0);
    atomic_store(&g_devmem_snap_live, (uint64_t)0);
    g_fmt_cap_n = 0;   // 设备代际变了，格式能力缓存不能跨设备复用
    fd_fb_cache_clear(d->device);
    // 批量提交 fence ring：只销毁仍在本设备上、且已 proven（READY）的槽（它们没有在途命令）；
    // OUT 槽由原持有者（帧队列/延迟槽）随设备一起丢弃，这里不碰，避免重复销毁。
    for (int i = 0; i < FD_FENCE_RING; i++)
        if (g_fence_ring[i].state == FD_FR_READY && g_fence_ring[i].dev == d->device)
            vkDestroyFence(d->device, g_fence_ring[i].fence, NULL);
    memset(g_fence_ring, 0, sizeof g_fence_ring);
    {
        // 会话级读数：开关关时不打印（默认配置与改前日志一致）。reused>0 才说明复用真的发生。
        uint64_t created = __atomic_exchange_n(&g_fr_created, 0, __ATOMIC_RELAXED);
        uint64_t reused = __atomic_exchange_n(&g_fr_reused, 0, __ATOMIC_RELAXED);
        uint64_t destroyed = __atomic_exchange_n(&g_fr_destroyed, 0, __ATOMIC_RELAXED);
        if (fd_fence_ring_on() && (created || reused || destroyed))
            FD_LOG("fence ring session(开关 debug.s2a.fence_ring=1): created=%llu reused=%llu "
                   "destroyed=%llu slots=%d | reused>0 才说明复用真的发生；fence 创建实测 0.21–0.24 ms/16 帧",
                   (unsigned long long)created, (unsigned long long)reused,
                   (unsigned long long)destroyed, FD_FENCE_RING);
    }
    g_cmd_free_n = 0;   // 命令池随设备销毁，空闲表只清指针
    for (int gp = 0; gp < 2; gp++) {
        if (g_gpu_pool[gp])
            vkDestroyQueryPool(d->device, g_gpu_pool[gp], NULL);
        g_gpu_pool[gp] = VK_NULL_HANDLE;
        g_gpu_npass[gp] = 0;
    }
    g_gpu_armed = 0;
    g_gpu_arm_parity = -1;
    // pass probe 的 host buffer 归当前设备；设备销毁前收回（没走完 flush 时也不会把映射带过设备边界）。
    if (g_fq.pp_buf) fd_pp_release(d);
    // 驱动管线缓存落盘：必须在设备销毁前取（release_device 的调用点满足），失败只记一行，
    // 不影响会话收尾；比“下次所有管线重新让驱动编”更划算。
    nvm_manifest_save_sync();   // 收尾同步落盘（进程要退出，后台线程等不起）
    {
        uint32_t loaded, recorded, dups, refused;
        nvm_manifest_stats(&loaded, &recorded, &dups, &refused);
        FD_LOG("pipe manifest: loaded=%u recorded=%u dups=%u refused=%u entries=%d",
               loaded, recorded, dups, refused, nvm_manifest_count());
    }
    if (g_pipeline_cache && g_driver_dir[0] && g_driver_uuid_ok) {
        size_t bytes = 0;
        if (vkGetPipelineCacheData(d->device, g_pipeline_cache, &bytes, NULL) == VK_SUCCESS && bytes) {
            void *data = malloc(bytes);
            if (data && vkGetPipelineCacheData(d->device, g_pipeline_cache, &bytes, data) == VK_SUCCESS) {
                char why[160] = "";
                if (nvk_driver_cache_write(g_driver_dir, g_driver_uuid, data, bytes, why, sizeof why) == 0)
                    FD_LOG("driver cache: stored %zu bytes for UUID %02x%02x%02x%02x…",
                           bytes, g_driver_uuid[0], g_driver_uuid[1], g_driver_uuid[2], g_driver_uuid[3]);
                else
                    FD_LOG("driver cache: store failed (%s); cache not persisted", why[0] ? why : "unknown");
            }
            free(data);
        }
    }
    if (g_pipeline_cache) vkDestroyPipelineCache(d->device, g_pipeline_cache, NULL);
    g_pipeline_cache = VK_NULL_HANDLE;
    g_driver_dev = NULL;
    g_driver_uuid_ok = 0;
    g_driver_dir[0] = 0;
    // 会话级诊断：真实游戏是否用过 samples-passed、结果是否零/非零，都留在一行里。
    struct nvn_vk_query_stats qst;
    nvn_vk_query_stats(&qst);
    if (qst.resets || qst.reports)
        FD_LOG("samples-passed session summary: resets=%llu reports=%llu segments=%llu slots=%llu "
               "total_samples=%llu zero=%llu before_reset=%llu fails=%llu",
               (unsigned long long)qst.resets, (unsigned long long)qst.reports,
               (unsigned long long)qst.segments, (unsigned long long)qst.slots,
               (unsigned long long)qst.samples, (unsigned long long)qst.zero_reports,
               (unsigned long long)qst.before_reset_reports, (unsigned long long)qst.fails);
    nvn_vk_query_release_device();
    if (fd_fq_poisoned() || g_fq.ncmd)
        FD_LOG("release_device with %d command buffer(s) still owned by the queue (%s); "
               "they are dropped with the device (intentional leak, no fake completion)",
               g_fq.ncmd, g_fq.fail[0] ? g_fq.fail : "no completion proof");
    // 两套帧池与映射 arena 归 first_draw 所有；session 已证明设备空闲。
    for (int i = 0; i < 2; i++)
        if (g_frame_pool[i]) vkDestroyDescriptorPool(d->device, g_frame_pool[i], NULL);
    for (int i = 0; i < g_arena_chunk_n; i++) {
        struct fd_arena_chunk *c = &g_arena_chunks[i];
        if (c->map && c->mem) vkUnmapMemory(d->device, c->mem);
        if (c->buf) vkDestroyBuffer(d->device, c->buf, NULL);
        if (c->mem) fd_devmem_release_dev(d->device, c->mem);
    }
    memset(&g_op, 0, sizeof g_op);   // 旧设备的 pass/cmd 随设备回收，不再调用 vkDestroy*
    memset(&g_fq, 0, sizeof g_fq);
    g_fq_fence_dev = VK_NULL_HANDLE;
    g_frame_pool[0] = g_frame_pool[1] = VK_NULL_HANDLE;
    g_frame_pool_left[0] = g_frame_pool_left[1] = 0;
    g_arena_chunk_n = 0;
    g_arena_bytes = 0;
    g_arena_submit_seq = 0;
    g_arena_cur = NULL;
    g_arena_rec_n = 0;
    memset(g_arena_chunks, 0, sizeof g_arena_chunks);
    g_arena_parity = 0;
    g_slot[0].used = g_slot[1].used = 0;
    g_slot_cur = -1;
    g_slot_fail = 0;
    memset(g_pipe_cache, 0, sizeof g_pipe_cache);
    fd_pipe_index_reset();
    g_pipe_clock = 0;
    g_pin_n = 0;   // 旧设备的纹理随设备回收；不要再 unpin
    // 采样器与程序对象归旧设备；设备销毁前先释放采样器，再清账本。
    if (g_samp_cache_ready) { nvn_vk_sampler_cache_destroy(&g_samp_cache); g_samp_cache_ready = 0; }
    for (int i = 0; i < FD_PROG_OBJ; i++)
        if (g_prog_obj[i].has_prog) { nvn_vk_program_free(&g_prog_obj[i].prog); g_prog_obj[i].has_prog = 0; }
    memset(g_prog_obj, 0, sizeof g_prog_obj);
    atomic_store(&g_prog_owned_bytes, 0);
    g_frame_seq = 0;
    g_frame_id = 1;
    g_pipeline_cache = VK_NULL_HANDLE;
    g_arena_chunk_n = 0;
    g_arena_bytes = 0;
    g_arena_submit_seq = 0;
    g_arena_cur = NULL;
    g_arena_rec_n = 0;
    g_arena_align = 0;
    memset(g_arena_chunks, 0, sizeof g_arena_chunks);
}

static int64_t fd_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static uint32_t vk_bpp(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 0;
    }
}

static void tr_file(const struct fd_trace *t, const char *name, const void *data, size_t bytes) {
    char path[1200];
    snprintf(path, sizeof path, "%s/trace-%s-%s", t->dir, t->tag, name);
    FILE *f = fopen(path, "wb");
    if (!f) { FD_LOG("trace: open %s failed", path); return; }
    size_t wr = fwrite(data, 1, bytes, f);
    fclose(f);
    FD_LOG("trace: wrote %s %zu/%zu B", path, wr, bytes);
}

static void tr_sha(const char *what, const void *data, size_t bytes) {
    s2a_sha256 c; s2a_sha256_init(&c); s2a_sha256_update(&c, data, bytes);
    uint8_t dg[32]; char hex[65]; s2a_sha256_final(&c, dg); s2a_sha256_hex(dg, hex);
    FD_LOG("trace: %s bytes=%zu sha256=%s", what, bytes, hex);
}

static int tr_match_value(const char *want, const char *id) {
    if (!want || !want[0] || !id) return 0;
    if (want[0] == '*') return 1;
    return strncmp(id, want, strlen(want)) == 0;
}
// trace 目标既可以是 request id 前缀，也可以是 `prog:<程序内容 SHA-256 前 4 字节>`。
// 世界程序都从磁盘缓存采纳（request_id 全叫 disk-cache），只有按程序前缀才能在热路径里
// 单独命中一笔，不用把整帧的 merge 关掉。
// trace 配置只在换帧时读属性：以前每笔 draw 读 3 个系统属性 + snprintf + strncmp，
// 世界帧 500 笔就是每帧上千次属性读（op=26 里能看到 ~1-3 us/笔）。属性改动仍在一帧内生效。
struct tr_cfg {
    char v[PROP_VALUE_MAX], sk[PROP_VALUE_MAX], m[PROP_VALUE_MAX], dir[PROP_VALUE_MAX];
    int skip, max;
};
static struct tr_cfg g_tr_cfg;
static void tr_refresh_config(void) {
    memset(&g_tr_cfg, 0, sizeof g_tr_cfg);
    __system_property_get("debug.s2a.trace", g_tr_cfg.v);
    __system_property_get("debug.s2a.trace_skip", g_tr_cfg.sk);
    __system_property_get("debug.s2a.trace_max", g_tr_cfg.m);
    __system_property_get("debug.s2a.dump_fail", g_tr_cfg.dir);
    g_tr_cfg.skip = g_tr_cfg.sk[0] ? atoi(g_tr_cfg.sk) : 0;
    if (g_tr_cfg.skip < 0) g_tr_cfg.skip = 0;
    g_tr_cfg.max = g_tr_cfg.m[0] ? atoi(g_tr_cfg.m) : 1;
    if (g_tr_cfg.max < 1) g_tr_cfg.max = 1;
}
static void tr_cfg_tick(void);   // 定义在下方；tr_want/fd_trace_wanted 共用这个节流入口
// 是否需要 trace：带计数节流的配置刷新（每 32 笔一次 ≈ 每帧 1-2 次），不再每笔读属性。
// 注意不能依赖“帧内第一笔”刷新：首笔常因深度-only 等提前返回，永远到不了这里。
static int tr_want(void) {
    tr_cfg_tick();
    return g_tr_cfg.v[0] && g_tr_cfg.dir[0] == '/';
}
static int fd_trace_wanted(const char *request_id, const uint8_t prog_sha[32], char *id, size_t id_size) {
    tr_cfg_tick();   // 以前这里每笔都读 debug.s2a.trace 属性（~2 us/笔），改成与 tr_next 共用的节流缓存
    const char *want = g_tr_cfg.v;
    if (!want[0]) { if (id_size) id[0] = 0; return 0; }
    if (!strncmp(want, "prog:", 5))
        snprintf(id, id_size, "prog:%02x%02x%02x%02x", prog_sha[0], prog_sha[1], prog_sha[2], prog_sha[3]);
    else
        snprintf(id, id_size, "%s", request_id ? request_id : "?");
    return tr_match_value(want, id);
}

// 命中 request 的第 n 笔（debug.s2a.trace_skip 跳过前 n 笔、debug.s2a.trace_max 控制数量，默认 1）；
// 返回序号，超过上限返回 -1。换 trace 目标（属性值变化）时重新计数：同一进程内可以先后抓不同
// request/不同序号的一笔，不必重启游戏。
static void tr_cfg_tick(void) {
    static int calls;
    if ((calls++ & 31) == 0) tr_refresh_config();
}
static int tr_next(const char *id) {
    static int used;
    static char last[PROP_VALUE_MAX * 3];
    const char *v = g_tr_cfg.v;
    int skip = g_tr_cfg.skip, max = g_tr_cfg.max;
    char key[PROP_VALUE_MAX * 3];
    snprintf(key, sizeof key, "%s|%s|%s", v, g_tr_cfg.sk, g_tr_cfg.m);
    if (strncmp(key, last, sizeof last) != 0) {
        snprintf(last, sizeof last, "%s", key);
        used = 0;
        if (v[0]) FD_LOG("trace: target/offset changed to '%s' skip=%d max=%d; draw counter reset", v, skip, max);
    }
    if (!tr_match_value(v, id)) return -1;
    int ordinal = used++;
    if (ordinal < skip) return -1;
    if (ordinal - skip >= max) return -1;
    return ordinal - skip;
}
// 纹理转储开关：debug.s2a.trace_tex=0 时只记绑定/采样事实，不落纹理 raw（找目标是哪一笔时用）。
static int tr_dump_tex(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.trace_tex", b);
        v = (b[0] == '0') ? 0 : 1;
    }
    return v;
}
// RT 转储开关：debug.s2a.trace_rt=0 时只算差异/bbox 并写日志，不落 before/after 大文件（找目标是哪一笔时用）。
static int tr_dump_rt(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.trace_rt", b);
        v = (b[0] == '0') ? 0 : 1;
    }
    return v;
}

// 把真实图像（level 0 / layer 0）读回成紧凑 raw。只用于诊断，不改布局（GENERAL）。
static int tr_image_raw(struct s2a_vk_device *d, struct fd_trace *t, const char *name, VkImage image,
                        uint32_t nvn_fmt, VkFormat fmt, uint32_t w, uint32_t h, int use_depth_aspect) {
    uint32_t bpp = vk_bpp(fmt);
    VkDeviceSize bytes;
    uint64_t blocks = nvn_vk_format_compressed_image_bytes(nvn_fmt, w, h);
    if (blocks) bytes = (VkDeviceSize)blocks;
    else if (bpp) bytes = (VkDeviceSize)w * h * bpp;
    else { FD_LOG("trace: %s format %d has no known bpp; not dumped", name, (int)fmt); return -1; }
    if (bytes > t->cap) { FD_LOG("trace: %s needs %llu B > trace buffer %llu B", name, (unsigned long long)bytes, (unsigned long long)t->cap); return -1; }
    VkBufferImageCopy c = {0};
    c.imageSubresource = (VkImageSubresourceLayers){ use_depth_aspect ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = (VkExtent3D){ w, h, 1 };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){ c.imageSubresource.aspectMask, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = d->command_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(d->device, &cbi, &cmd) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, t->buf, 1, &c);
    VkImageMemoryBarrier b2 = b;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f; vkCreateFence(d->device, &fci, NULL, &f);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    int ok = vkQueueSubmit(d->queue, 1, &si, f) == VK_SUCCESS &&
             vkWaitForFences(d->device, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS;
    vkDestroyFence(d->device, f, NULL);
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
    if (!ok) { FD_LOG("trace: %s GPU readback failed", name); return -1; }
    tr_file(t, name, t->map, (size_t)bytes);
    return 0;
}

// 全幅 RT 前后差异：返回改动像素数并给出真实覆盖 bbox（5 个固定点不能代替它）。
static void tr_rt_diff(struct fd_trace *t, uint32_t w, uint32_t h, uint32_t bpp) {
    const uint8_t *a = (const uint8_t *)t->map;
    const uint8_t *b = a + (size_t)w * h * bpp;
    uint32_t changed = 0, minx = w, miny = h, maxx = 0, maxy = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t off = ((size_t)y * w + x) * bpp;
            if (memcmp(a + off, b + off, bpp)) {
                changed++;
                if (x < minx) minx = x; if (x > maxx) maxx = x;
                if (y < miny) miny = y; if (y > maxy) maxy = y;
            }
        }
    }
    if (!changed) {
        FD_LOG("trace: rt diff 0/%u pixels changed (draw wrote nothing on the real target)", w * h);
        return;
    }
    uint32_t cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    size_t off = ((size_t)cy * w + cx) * bpp;
    FD_LOG("trace: rt diff changed=%u/%u bbox=(%u,%u)-(%u,%u) center=(%u,%u) before=%02x%02x%02x%02x after=%02x%02x%02x%02x",
           changed, w * h, minx, miny, maxx, maxy, cx, cy,
           bpp > 0 ? a[off] : 0, bpp > 1 ? a[off+1] : 0, bpp > 2 ? a[off+2] : 0, bpp > 3 ? a[off+3] : 0,
           bpp > 0 ? b[off] : 0, bpp > 1 ? b[off+1] : 0, bpp > 2 ? b[off+2] : 0, bpp > 3 ? b[off+3] : 0);
}

// 批量/同步路径共用的逐笔耗时记账（正常模式只做原子累加，每 256 笔打一行）。
// 采样闸门：逐笔打点每次 clock_gettime 约 150ns，一笔十几到二十几个打点就是 2-4us，
// 与被测对象同量级。debug.s2a.perf_sample=N（N>0）时只对 1/N 帧计时，其余帧返回 0，
// 统计里按“采样到的笔数”归一。缺省 0 = 全开（不改变现有读数）。
static _Atomic int g_fd_timing_on = 1;
static _Atomic uint32_t g_fd_frame_n;
static int fd_perf_sample(void) {
    static int v = -2;
    if (v == -2) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.perf_sample", b);
        v = b[0] ? atoi(b) : 0;
        if (v < 0) v = 0;
    }
    return v;
}
// 每帧开始时决定这一帧要不要打点；只由 flush（帧边界）推进，不影响帧内一致性。
static int fd_perf_timing_enabled(void) {
    static int v = -1;
    if (v < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.perf_timing", b); v = b[0] ? atoi(b) : 0; }
    return v;
}
// 后段残差打点（诊断，开关 debug.s2a.draw_tail，默认关；报告 §3.1-N/§7 的 draw_tail）。
// 覆盖 draw timing 分项之外、原先没有名字的两段：first_draw 入口→t_start 的准备工作，以及
// t_submitted 之后到返回（含 fd_preheat_tick 与收尾清理）。与 draw timing 共用 256 笔窗口和
// 同一个 g_fd_timing_on 采样闸门；关闭时不取钟、不累加、不打印。first_draw 侧每笔 +2 次 clock，
// 另外 +2 次在调用方（nvn_vk_draw 的 c1 回填 / release），合计 4 次/笔，符合报告 §7 上限。
// 必须同时 perf_timing=1：c1/release 的起点/终点要复用 nvn_vk_draw 里只认 perf_timing 的
// pf_now() 时间戳；只开 perf_sample 时这些时间戳为 0，尾段采不到，所以不打开本开关。
static _Atomic int g_fd_tail_on;
static int fd_draw_tail_prop(void) {
    static int v = -1;
    if (v < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.draw_tail", b); v = (b[0] && atoi(b)) ? 1 : 0; }
    return v;
}
static inline int64_t fd_tail_stamp(void) {
    return atomic_load(&g_fd_tail_on) ? fd_now_ns() : 0;
}
static uint64_t g_tail_pre_ns, g_tail_pre_n, g_tail_post_ns, g_tail_post_n;
// 供 nvn_vk_draw 取同一采样闸门（调用方按弱符号判空，宿主探针没有 first_draw 时不受影响）。
int nvn_vk_first_draw_tail_on(void) { return atomic_load(&g_fd_tail_on); }
// 尾段窗口账：读取即清零，与 draw timing 的 256 笔窗口同一个打印点取数。
void nvn_vk_first_draw_take_draw_tail_stats(uint64_t *pre_ns, uint64_t *pre_n,
                                            uint64_t *post_ns, uint64_t *post_n) {
    if (pre_ns) *pre_ns = __atomic_exchange_n(&g_tail_pre_ns, 0, __ATOMIC_RELAXED);
    if (pre_n) *pre_n = __atomic_exchange_n(&g_tail_pre_n, 0, __ATOMIC_RELAXED);
    if (post_ns) *post_ns = __atomic_exchange_n(&g_tail_post_ns, 0, __ATOMIC_RELAXED);
    if (post_n) *post_n = __atomic_exchange_n(&g_tail_post_n, 0, __ATOMIC_RELAXED);
}
// 主体未归属时间拆分（诊断，开关 debug.s2a.body_gap=1，默认关；需 perf_timing=1，进同一个 256 笔窗口）。
// 背景：draw timing(batch) 的 avg 与「已打印分项之和」在营地场景差 ~5 us/笔，但那条行每个分项各自
// 先截断成整数 us（record 的 5 个子项最多丢 5 us），而且它的 entry 是「avg 减各分项」的残差、不是实测。
// 这里用原始 ns 逐段累加、不截断，量出真实落点：
//   prep    = 函数入口 → t_start（samp/lookup ensure、程序对象解析、tex 槽比较、帧队列腾位检查）
//   entry   = t_start → t_parsed（状态解析；旧行同名分项其实是残差）
//   modules/layouts/pass/pipe = 原有各段（layouts 恒 0，保留位置）
//   support/res/vbuf/desc/cmd = record 段的五个子项（原样，但原始 ns）
//   tail    = t_submitted → 返回（fd_note_draw_stats / preheat tick / 收尾清理；在返回点累加，
//             所以和 draw_tail 的 post 一样计入下一窗，1/256 的错位可忽略）
// 关闭时 fd_body_stamp 返回 0、不累加、不打印；打开时每笔 +2 次 clock（入口/返回各一次）。
static _Atomic int g_bg_on;
static int fd_body_gap_prop(void) {
    static int v = -1;
    if (v < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.body_gap", b); v = (b[0] && atoi(b)) ? 1 : 0; }
    return v;
}
static inline int64_t fd_body_stamp(void) {
    return atomic_load(&g_bg_on) ? fd_now_ns() : 0;
}
enum {
    FD_BG_PREP = 0, FD_BG_ENTRY, FD_BG_MODULES, FD_BG_LAYOUTS, FD_BG_PASS, FD_BG_PIPE,
    FD_BG_SUPPORT, FD_BG_RES, FD_BG_VBUF, FD_BG_DESC, FD_BG_CMD, FD_BG_RECORD,
    FD_BG_COUNT
};
static uint64_t g_bg_ns[FD_BG_COUNT], g_bg_n;
static uint64_t g_bg_tail_ns, g_bg_tail_n;

static void fd_timing_frame_boundary(void) {
    atomic_store(&g_draws_this_frame, 0);
    fd_props_refresh();
    int n = fd_perf_sample();
    // 逐笔打点（每笔约 10 次 clock_gettime + 原子累加）是诊断口径，正常游玩默认关；
    // 需要逐笔归因时设 debug.s2a.perf_timing=1（或 perf_sample=N 抽样）。
    if (n <= 0) {
        atomic_store(&g_fd_timing_on, fd_perf_timing_enabled() ? 1 : 0);
    } else {
        uint32_t f = atomic_fetch_add(&g_fd_frame_n, 1) + 1;
        atomic_store(&g_fd_timing_on, (f % (uint32_t)n) == 0);
    }
    // draw_tail 跟随逐笔打点的采样闸门，且要求 perf_timing=1（见上面注释：c1/release 要复用
    // pf_now() 的时间戳）。perf_sample 抽样时只对同一批帧补尾段，保证同一批样本。
    atomic_store(&g_fd_tail_on,
                 fd_perf_timing_enabled() && atomic_load(&g_fd_timing_on) && fd_draw_tail_prop());
    // body_gap 用同一批样本、同一个 256 笔窗口（复用已有的分项时间戳，只额外取入口/返回两个点）。
    atomic_store(&g_bg_on,
                 fd_perf_timing_enabled() && atomic_load(&g_fd_timing_on) && fd_body_gap_prop());
}
static inline int64_t fd_stamp(void) {
    return atomic_load(&g_fd_timing_on) ? fd_now_ns() : 0;
}

static void fd_note_draw_stats(int64_t t_body0, int64_t t_start, int64_t t_parsed, int64_t t_modules,
                               int64_t t_layouts, int64_t t_pass, int64_t t_pipeline, int64_t t_submitted,
                               int64_t t_support, int64_t t_res, int64_t t_vbuf, int64_t t_desc,
                               int64_t t_pass_a, int64_t t_pass_b) {
    if (!atomic_load(&g_fd_timing_on)) return;
    uint64_t us = (uint64_t)((t_submitted - t_start) / 1000);
    uint32_t n = atomic_fetch_add(&g_draw_n, 1) + 1;
    atomic_fetch_add(&g_draw_us_sum, us);
    atomic_fetch_add(&g_us_modules, (uint64_t)((t_modules - t_parsed) / 1000));
    atomic_fetch_add(&g_us_layouts, (uint64_t)((t_layouts - t_modules) / 1000));
    atomic_fetch_add(&g_us_pass, (uint64_t)((t_pass - t_layouts) / 1000));
    atomic_fetch_add(&g_us_pipe, (uint64_t)((t_pipeline - t_pass) / 1000));
    atomic_fetch_add(&g_us_record, (uint64_t)((t_submitted - t_pipeline) / 1000));
    atomic_fetch_add(&g_us_wait, 0);
    uint64_t sub_support = t_support ? (uint64_t)((t_support - t_pipeline) / 1000) : 0;
    uint64_t sub_res = t_res && t_support ? (uint64_t)((t_res - t_support) / 1000) : 0;
    uint64_t sub_vbuf = t_vbuf && t_res ? (uint64_t)((t_vbuf - t_res) / 1000) : 0;
    uint64_t sub_desc = t_desc && t_vbuf ? (uint64_t)((t_desc - t_vbuf) / 1000) : 0;
    uint64_t sub_cmd = t_desc ? (uint64_t)((t_submitted - t_desc) / 1000) : (uint64_t)((t_submitted - t_pipeline) / 1000);
    atomic_fetch_add(&g_us_sup, sub_support); atomic_fetch_add(&g_us_res, sub_res);
    atomic_fetch_add(&g_us_vb, sub_vbuf); atomic_fetch_add(&g_us_desc, sub_desc);
    atomic_fetch_add(&g_us_cmd, sub_cmd);
    // 精确账：同一批 ns 时间戳，不经过整数微秒截断
    {
        int64_t d_entry = t_parsed - t_start;
        int64_t d_mod = t_modules - t_parsed, d_lay = t_layouts - t_modules, d_pass = t_pass - t_layouts;
        int64_t d_pipe = t_pipeline - t_pass, d_sup = t_support ? (t_support - t_pipeline) : 0;
        int64_t d_res = (t_res && t_support) ? (t_res - t_support) : 0;
        int64_t d_vb = (t_vbuf && t_res) ? (t_vbuf - t_res) : 0;
        int64_t d_desc = (t_desc && t_vbuf) ? (t_desc - t_vbuf) : 0;
        int64_t d_cmd = t_desc ? (t_submitted - t_desc) : 0;
        int64_t d_total = t_submitted - t_start;
        if (d_entry > 0) atomic_fetch_add(&g_ns_entry, (uint64_t)d_entry);
        if (d_mod > 0) atomic_fetch_add(&g_ns_modules, (uint64_t)d_mod);
        if (d_lay > 0) atomic_fetch_add(&g_ns_layouts, (uint64_t)d_lay);
        if (d_pass > 0) atomic_fetch_add(&g_ns_pass, (uint64_t)d_pass);
        if (d_pipe > 0) atomic_fetch_add(&g_ns_pipe, (uint64_t)d_pipe);
        if (d_sup > 0) atomic_fetch_add(&g_ns_sup, (uint64_t)d_sup);
        if (d_res > 0) atomic_fetch_add(&g_ns_res, (uint64_t)d_res);
        if (d_vb > 0) atomic_fetch_add(&g_ns_vb, (uint64_t)d_vb);
        if (d_desc > 0) atomic_fetch_add(&g_ns_desc, (uint64_t)d_desc);
        if (d_cmd > 0) atomic_fetch_add(&g_ns_cmd, (uint64_t)d_cmd);
        if (d_total > 0) atomic_fetch_add(&g_ns_total, (uint64_t)d_total);
        if (t_pass_a && t_pass_b && t_pass) {
            int64_t a = t_pass_a - t_layouts, b = t_pass_b - t_pass_a, c = t_pass - t_pass_b;
            if (a > 0) { atomic_fetch_add(&g_ns_pass_plan, (uint64_t)a); atomic_fetch_add(&g_ns_pass_plan_n, 1); }
            if (b > 0) { atomic_fetch_add(&g_ns_pass_fb, (uint64_t)b); atomic_fetch_add(&g_ns_pass_fb_n, 1); }
            if (c > 0) { atomic_fetch_add(&g_ns_pass_open, (uint64_t)c); atomic_fetch_add(&g_ns_pass_open_n, 1); }
        }
        atomic_fetch_add(&g_ns_n, 1);
        atomic_fetch_add(&g_win_draws, 1);
    }
    // 主体未归属拆分（开关 debug.s2a.body_gap=1，默认关）：原始 ns 逐段累加，不经过整数 us 截断。
    // 各段以相邻时间戳为界（prep 从函数入口算），所以 prep..cmd 之和 = t_submitted − 函数入口；
    // FD_BG_RECORD 单独记 t_pipeline→t_submitted，用来核对五个子项是否真的覆盖了 record 段（残差应为 0）。
    // tail 在 nvn_vk_first_draw 的返回点累加（这一笔的返回发生在本次调用之后，所以计入下一窗）。
    if (atomic_load(&g_bg_on) && t_body0 && t_start && t_parsed && t_modules && t_layouts && t_pass &&
        t_pipeline && t_support && t_res && t_vbuf && t_desc && t_submitted) {
        int64_t sp[FD_BG_COUNT];
        sp[FD_BG_PREP] = t_start - t_body0;
        sp[FD_BG_ENTRY] = t_parsed - t_start;
        sp[FD_BG_MODULES] = t_modules - t_parsed;
        sp[FD_BG_LAYOUTS] = t_layouts - t_modules;
        sp[FD_BG_PASS] = t_pass - t_layouts;
        sp[FD_BG_PIPE] = t_pipeline - t_pass;
        sp[FD_BG_SUPPORT] = t_support - t_pipeline;
        sp[FD_BG_RES] = t_res - t_support;
        sp[FD_BG_VBUF] = t_vbuf - t_res;
        sp[FD_BG_DESC] = t_desc - t_vbuf;
        sp[FD_BG_CMD] = t_submitted - t_desc;
        sp[FD_BG_RECORD] = t_submitted - t_pipeline;
        for (int i = 0; i < FD_BG_COUNT; i++)
            if (sp[i] > 0) __atomic_fetch_add(&g_bg_ns[i], (uint64_t)sp[i], __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_bg_n, 1, __ATOMIC_RELAXED);
    }
    static uint32_t timing_batches;
    int timing_print = (n % 256 == 0) && (fd_verbose() || ++timing_batches % 32 == 0);
    if (timing_print)
        FD_LOG("draw timing(batch): avg=%llu us over %u draws [entry=%llu modules=%llu layouts=%llu pass=%llu pipe=%llu | support=%llu res=%llu vbuf=%llu desc=%llu cmd=%llu] texSame=%llu texDiff=%llu passes=%d/%d pools=%llu rot=%llu left=%u pipes hit=%llu miss=%llu evict=%llu pobj created=%llu hit=%llu fbtex=%llu pcfg=%llu",
               (unsigned long long)(atomic_load(&g_draw_us_sum) / n), n,
               (unsigned long long)((atomic_load(&g_draw_us_sum) - atomic_load(&g_us_modules) - atomic_load(&g_us_layouts) - atomic_load(&g_us_pass) - atomic_load(&g_us_pipe) - atomic_load(&g_us_record)) / n),
               (unsigned long long)(atomic_load(&g_us_modules) / n),
               (unsigned long long)(atomic_load(&g_us_layouts) / n),
               (unsigned long long)(atomic_load(&g_us_pass) / n),
               (unsigned long long)(atomic_load(&g_us_pipe) / n),
               (unsigned long long)(atomic_load(&g_us_sup) / n), (unsigned long long)(atomic_load(&g_us_res) / n),
               (unsigned long long)(atomic_load(&g_us_vb) / n), (unsigned long long)(atomic_load(&g_us_desc) / n),
               (unsigned long long)(atomic_load(&g_us_cmd) / n),
               (unsigned long long)atomic_load(&g_key_same),
               (unsigned long long)atomic_load(&g_key_diff),
               g_pass_merges, g_pass_flushes,

               (unsigned long long)atomic_load(&g_pool_created),
               (unsigned long long)atomic_load(&g_pool_rotations), g_frame_pool_left[g_arena_parity & 1],
               (unsigned long long)atomic_load(&g_pipe_hits), (unsigned long long)atomic_load(&g_pipe_misses),
               (unsigned long long)atomic_load(&g_pipe_evicts),
               (unsigned long long)atomic_load(&g_prog_obj_created), (unsigned long long)atomic_load(&g_prog_obj_hits),
               (unsigned long long)(atomic_load(&g_fbtex_n) ? atomic_load(&g_us_fbtex) / atomic_load(&g_fbtex_n) : 0),
               (unsigned long long)(atomic_load(&g_pcfg_n) ? atomic_load(&g_us_pcfg) / atomic_load(&g_pcfg_n) : 0));
    if (timing_print) {
        uint64_t view_ns = atomic_load(&g_res_view_ns), samp_ns = atomic_load(&g_res_samp_ns),
                 pin_ns = atomic_load(&g_res_pin_ns), tex_ns = atomic_load(&g_res_tex_ns);
        uint64_t other_ns = tex_ns > view_ns + samp_ns + pin_ns ? tex_ns - view_ns - samp_ns - pin_ns : 0;
        // 口径修正：view/sampler/pin/纹理合计 都是「每次纹理绑定」的量，分母必须是绑定次数
        // （g_res_tex_c），不是窗口里的 draw 数 n。用 n 除会让这一行恒为 0 或明显偏小，
        // 之前读到的「纹理合计 16–42 us/笔」就是这个错分母乘出来的假数。
        const uint64_t tex_c = atomic_load(&g_res_tex_c) ? atomic_load(&g_res_tex_c) : 1;
        FD_LOG("  res 细分（均/次纹理绑定, 次数=%llu）：view=%llu sampler=%llu pin=%llu 其余=%llu 合计=%llu us | "
               "绑定 ubo=%llu tex=%llu 合计=%llu（均/笔） 采样器缓存 n=%d created=%u reused=%u",
               (unsigned long long)atomic_load(&g_res_tex_c),
               (unsigned long long)(view_ns / tex_c / 1000), (unsigned long long)(samp_ns / tex_c / 1000),
               (unsigned long long)(pin_ns / tex_c / 1000), (unsigned long long)(other_ns / tex_c / 1000),
               (unsigned long long)(tex_ns / tex_c / 1000),
               (unsigned long long)(g_bind_ub_n / n), (unsigned long long)(g_bind_tex_n / n),
               (unsigned long long)(g_bind_n / n),
               g_samp_cache.n, g_samp_cache.created, g_samp_cache.reused);
    }
    if (n % 256 == 0)
        FD_LOG("parse cache: hits=%llu misses=%llu", (unsigned long long)atomic_load(&g_parse_hits),
               (unsigned long long)atomic_load(&g_parse_misses));
    if (n % 256 == 0) {
        // 后段残差（开关 debug.s2a.draw_tail=1，默认关）：draw timing 分项之外的两段。
        // 与上面 draw timing 同一个 256 笔窗口；只在有样本时出现。c1/release 由 nvn_vk_draw 记，
        // 这里按弱符号取数（宿主探针没有那边时不影响链接）。切换点这一笔的 post 在返回时才落账，
        // 所以它的 post 计入下一窗（1/256，可忽略）。
        extern void nvk_draw_take_tail_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *)
            __attribute__((weak));
        uint64_t pre_ns = 0, pre_n = 0, post_ns = 0, post_n = 0;
        uint64_t c1_ns = 0, c1_n = 0, rel_ns = 0, rel_n = 0;
        nvn_vk_first_draw_take_draw_tail_stats(&pre_ns, &pre_n, &post_ns, &post_n);
        if (nvk_draw_take_tail_stats) nvk_draw_take_tail_stats(&c1_ns, &c1_n, &rel_ns, &rel_n);
        if (pre_n || post_n || c1_n || rel_n)
            FD_LOG("draw 尾段(本窗 n=%u, 开关 debug.s2a.draw_tail=1 且需 perf_timing=1): first_draw 入口前=%.2f us/笔(n=%llu) "
                   "first_draw 返回后=%.2f us/笔(n=%llu) | c1 回填=%.2f us/笔(n=%llu) release=%.2f us/笔(n=%llu)",
                   n,
                   pre_n ? (double)pre_ns / (double)pre_n / 1000.0 : 0.0, (unsigned long long)pre_n,
                   post_n ? (double)post_ns / (double)post_n / 1000.0 : 0.0, (unsigned long long)post_n,
                   c1_n ? (double)c1_ns / (double)c1_n / 1000.0 : 0.0, (unsigned long long)c1_n,
                   rel_n ? (double)rel_ns / (double)rel_n / 1000.0 : 0.0, (unsigned long long)rel_n);
    }
    if (n % 256 == 0) {
        // 主体未归属（开关 debug.s2a.body_gap=1，默认关）：与 draw timing(batch) 同一个 256 笔窗口、
        // 同一批样本；原始 ns 累加不截断。旧行整数 us 口径的「缺口」用同一窗口、尚未清零的计数复算，
        // 和旧行完全同源，便于对照「缺口是截断还是真的没有归属」。
        uint64_t bg_n = __atomic_exchange_n(&g_bg_n, 0, __ATOMIC_RELAXED);
        uint64_t tail_ns = __atomic_exchange_n(&g_bg_tail_ns, 0, __ATOMIC_RELAXED);
        uint64_t tail_n = __atomic_exchange_n(&g_bg_tail_n, 0, __ATOMIC_RELAXED);
        if (bg_n) {
            double us[FD_BG_COUNT];
            for (int i = 0; i < FD_BG_COUNT; i++)
                us[i] = (double)__atomic_exchange_n(&g_bg_ns[i], 0, __ATOMIC_RELAXED) / (double)bg_n / 1000.0;
            uint64_t o_avg = atomic_load(&g_draw_us_sum) / n;
            uint64_t o_entry = (atomic_load(&g_draw_us_sum) - atomic_load(&g_us_modules) -
                                atomic_load(&g_us_layouts) - atomic_load(&g_us_pass) -
                                atomic_load(&g_us_pipe) - atomic_load(&g_us_record)) / n;
            uint64_t o_sum = o_entry + atomic_load(&g_us_modules) / n + atomic_load(&g_us_layouts) / n +
                             atomic_load(&g_us_pass) / n + atomic_load(&g_us_pipe) / n +
                             atomic_load(&g_us_sup) / n + atomic_load(&g_us_res) / n +
                             atomic_load(&g_us_vb) / n + atomic_load(&g_us_desc) / n +
                             atomic_load(&g_us_cmd) / n;
            double sub_sum = us[FD_BG_SUPPORT] + us[FD_BG_RES] + us[FD_BG_VBUF] + us[FD_BG_DESC] + us[FD_BG_CMD];
            double body = us[FD_BG_PREP] + us[FD_BG_ENTRY] + us[FD_BG_MODULES] + us[FD_BG_LAYOUTS] +
                          us[FD_BG_PASS] + us[FD_BG_PIPE] + us[FD_BG_RECORD];
            // 打印点跟 draw timing(batch) 完全同一处（同一窗口、同一降频），两条行成对出现；
            // 窗口计数仍在每个 256 笔边界清零，不打印的窗口直接丢弃。
            if (timing_print) {
                FD_LOG("draw body gap(本窗 n=%llu, 开关 debug.s2a.body_gap=1 且需 perf_timing=1, 原始ns不截断): "
                       "每笔 us: prep=%.2f entry=%.2f modules=%.2f layouts=%.2f pass=%.2f pipe=%.2f | "
                       "support=%.2f res=%.2f vbuf=%.2f desc=%.2f cmd=%.2f | tail=%.2f(n=%llu) | "
                       "body=%.2f(入口→t_submitted) record=%.2f 子项和=%.2f 残差=%.2f | "
                       "旧行整数us: avg=%llu 分项和=%llu 缺口=%lld",
                       (unsigned long long)bg_n,
                       us[FD_BG_PREP], us[FD_BG_ENTRY], us[FD_BG_MODULES], us[FD_BG_LAYOUTS],
                       us[FD_BG_PASS], us[FD_BG_PIPE],
                       us[FD_BG_SUPPORT], us[FD_BG_RES], us[FD_BG_VBUF], us[FD_BG_DESC], us[FD_BG_CMD],
                       tail_n ? (double)tail_ns / (double)tail_n / 1000.0 : 0.0, (unsigned long long)tail_n,
                       body, us[FD_BG_RECORD], sub_sum, us[FD_BG_RECORD] - sub_sum,
                       (unsigned long long)o_avg, (unsigned long long)o_sum,
                       (long long)o_avg - (long long)o_sum);
            if (bg_n) {
                // pass 段 = 逐笔检查(fb+cont) + 开 pass 路径(只有部分 draw 走)；三者相加应 ≈ body 里的 pass。
                uint64_t pfb = atomic_exchange(&g_ns_pd_fb, 0), pcont = atomic_exchange(&g_ns_pd_cont, 0);
                uint64_t popen = atomic_exchange(&g_ns_pd_open, 0), popen_n = atomic_exchange(&g_ns_pd_open_n, 0);
                uint64_t pl = atomic_exchange(&g_ns_pass_plan, 0), pl_n = atomic_exchange(&g_ns_pass_plan_n, 0);
                uint64_t pfb2 = atomic_exchange(&g_ns_pass_fb, 0), pfb2_n = atomic_exchange(&g_ns_pass_fb_n, 0);
                uint64_t pop2 = atomic_exchange(&g_ns_pass_open, 0), pop2_n = atomic_exchange(&g_ns_pass_open_n, 0);
                uint64_t pd_n = atomic_exchange(&g_ns_pd_n, 0);
                double d = pd_n ? (double)pd_n : (double)bg_n;   // 用真正的累计笔数，避免窗口错配造成放大
                FD_LOG("  pass 段细分(本窗 n=%llu)：每笔 fb 检查=%.2f + 延续判定=%.2f + 开pass 摊薄=%.2f "
                       "= %.2f us（本窗 n=%llu 实计, body 的 pass=%.2f）| 新开 pass %llu 次(%.1f/帧)：均 %.2f us = "
                       "rp查找 %.2f + fb取得 %.2f + clear+begin %.2f",
                       (unsigned long long)bg_n,
                       (double)pfb / d / 1000.0, (double)pcont / d / 1000.0, (double)popen / d / 1000.0,
                       ((double)pfb + (double)pcont + (double)popen) / d / 1000.0, (unsigned long long)pd_n,
                       us[FD_BG_PASS],
                       (unsigned long long)popen_n, (double)popen_n / 16.0,
                       popen_n ? (double)popen / (double)popen_n / 1000.0 : 0.0,
                       pl_n ? (double)pl / (double)pl_n / 1000.0 : 0.0,
                       pfb2_n ? (double)pfb2 / (double)pfb2_n / 1000.0 : 0.0,
                       pop2_n ? (double)pop2 / (double)pop2_n / 1000.0 : 0.0);
            }
            }
        }
    }
    // 窗口化：这些累计量只服务于这条日志，打完就归零，下一行就是“近 n 笔”而不是“整局累计”。
    // 整局累计会把早期的重负载永远混在里面——同一个 avg 既能是 60us 也能是 216us，
    // 中间并没有变慢，只是窗口不同。这样读数才对应“现在”。
    if (n % 256 == 0) {
        atomic_store(&g_draw_us_sum, 0); atomic_store(&g_us_modules, 0); atomic_store(&g_us_layouts, 0);
        atomic_store(&g_us_pass, 0); atomic_store(&g_us_pipe, 0); atomic_store(&g_us_record, 0);
        atomic_store(&g_us_sup, 0); atomic_store(&g_us_res, 0); atomic_store(&g_us_vb, 0);
        atomic_store(&g_us_desc, 0); atomic_store(&g_us_cmd, 0); atomic_store(&g_us_wait, 0);
        atomic_store(&g_us_fbtex, 0); atomic_store(&g_us_pcfg, 0);
        atomic_store(&g_fbtex_n, 0); atomic_store(&g_pcfg_n, 0);
        // 注意：g_desc_* 与 g_res_ub/tex（含各自次数）只由 nvn_vk_first_draw_take_open_window()
        // 取数+清零，这里再清一次会让「窗口时间总量」和「窗口次数」错配。
        // view/sampler/pin 三个子项只服务下面的「res 细分」这条日志，仍在这里按同批清零。
        atomic_store(&g_res_view_ns, 0); atomic_store(&g_res_samp_ns, 0); atomic_store(&g_res_pin_ns, 0);
        g_bind_ub_n = 0; g_bind_tex_n = 0; g_bind_n = 0;
        atomic_store(&g_key_same, 0); atomic_store(&g_key_diff, 0);
        g_pass_merges = 0; g_pass_flushes = 0;
        atomic_store(&g_draw_n, 0);
    }
}

// 低频统计：depth-only pass 的每笔明细只在 debug.s2a.draw_log 打开时打印；默认由 present 窗口
// 汇总一行（这是“本窗多少笔 draw / 其中多少笔壳体 pass”，不是 pass 关闭次数）。
static _Atomic uint64_t g_draws_since_report, g_depth_only_since_report;

void nvn_vk_first_draw_take_counts(uint64_t *draws, uint64_t *depth_only) {
    if (draws) *draws = atomic_exchange(&g_draws_since_report, 0);
    if (depth_only) *depth_only = atomic_exchange(&g_depth_only_since_report, 0);
}

// 观察窗口读数：render pass 缓存命中/新建，以及建 pass/framebuffer 的耗时。命中率和对象的
// 创建耗时必须分开看，不能把“缓存命中多”当成“pass 成本已经降下来”。读取即清零。
void nvn_vk_first_draw_pipe_create_stats(uint64_t *n, uint64_t *ns) {
    if (n) *n = atomic_load(&g_draw_pipe_n);
    if (ns) *ns = atomic_load(&g_draw_pipe_ns);
}

void nvn_vk_first_draw_take_cmd_stats(uint64_t *reset_ns, uint64_t *alloc_ns) {
    if (reset_ns) *reset_ns = __atomic_exchange_n(&g_cmd_reset_ns, 0, __ATOMIC_RELAXED);
    if (alloc_ns) *alloc_ns = __atomic_exchange_n(&g_cmd_alloc_ns, 0, __ATOMIC_RELAXED);
}
// 本窗开 pass / 单笔的统一账：每项都带自己的累计 ns 与次数，调用方按「每次新开 / 每笔 / 每帧」
// 三个分母分别换算，禁止把「每次新开」的数除以「所有 draw」的次数。
void nvn_vk_first_draw_take_open_window(struct nvn_vk_open_window *o) {
    if (!o) return;
    o->passes = __atomic_exchange_n(&g_rp_t_npass, 0, __ATOMIC_RELAXED);
    o->cget_ns = __atomic_exchange_n(&g_rp_t_cget, 0, __ATOMIC_RELAXED);
    o->bcb_ns = __atomic_exchange_n(&g_rp_t_bcb, 0, __ATOMIC_RELAXED);
    o->rest_ns = __atomic_exchange_n(&g_rp_t_rest, 0, __ATOMIC_RELAXED);
    o->rest_gpuarm_ns = atomic_exchange(&g_rest_gpuarm_ns, 0);
    o->rest_query_ns = atomic_exchange(&g_rest_query_ns, 0);
    o->rest_precopy_ns = atomic_exchange(&g_rest_precopy_ns, 0);
    o->rest_barrier_ns = atomic_exchange(&g_rest_barrier_ns, 0);
    o->rest_beginrp_ns = atomic_exchange(&g_rest_beginrp_ns, 0);
    o->rest_sub_n = atomic_exchange(&g_rest_sub_n, 0);
    o->open_call_ns = atomic_exchange(&g_ns_open_call, 0);
    o->open_call_n = atomic_exchange(&g_ns_open_call_n, 0);
    o->draws = atomic_exchange(&g_win_draws, 0);
    o->seg_ns[S2A_OW_SEG_ENTRY_MODULES] = atomic_exchange(&g_ns_entry, 0)
                                        + atomic_exchange(&g_ns_modules, 0);
    (void)atomic_exchange(&g_ns_layouts, 0);
    o->seg_ns[S2A_OW_SEG_PASS] = atomic_exchange(&g_ns_pass, 0);
    o->seg_ns[S2A_OW_SEG_PIPE] = atomic_exchange(&g_ns_pipe, 0);
    o->seg_ns[S2A_OW_SEG_RES] = atomic_exchange(&g_ns_res, 0)
                              + atomic_exchange(&g_ns_sup, 0);
    o->seg_ns[S2A_OW_SEG_VBUF] = atomic_exchange(&g_ns_vb, 0);
    o->seg_ns[S2A_OW_SEG_DESC_CMD] = atomic_exchange(&g_ns_desc, 0)
                                   + atomic_exchange(&g_ns_cmd, 0);
    o->seg_total_ns = atomic_exchange(&g_ns_total, 0);
    o->res_ub_ns = atomic_exchange(&g_res_ub_ns, 0);
    o->res_ub_n = atomic_exchange(&g_res_ub_c, 0);
    o->res_tex_ns = atomic_exchange(&g_res_tex_ns, 0);
    o->res_tex_n = atomic_exchange(&g_res_tex_c, 0);
    o->desc_alloc_ns = atomic_exchange(&g_desc_alloc_ns, 0);
    o->desc_alloc_n = atomic_exchange(&g_desc_alloc_c, 0);
    o->desc_loop_ns = atomic_exchange(&g_desc_loop_ns, 0);
    o->desc_loop_n = atomic_exchange(&g_desc_loop_c, 0);
    o->desc_write_ns = atomic_exchange(&g_desc_write_ns, 0);
    o->desc_write_n = atomic_exchange(&g_desc_write_c, 0);
    o->desc_pool_ns = atomic_exchange(&g_desc_pool_ns, 0);
    o->desc_pool_n = atomic_exchange(&g_desc_pool_c, 0);
    o->desc_alloc_calls = g_desc_alloc_calls; g_desc_alloc_calls = 0;
    o->desc_update_calls = g_desc_update_calls; g_desc_update_calls = 0;
    o->desc_writes = g_desc_writes; g_desc_writes = 0;
    o->frame_id = g_frame_id;
}

void nvn_vk_first_draw_take_pass_open_stats(uint64_t *cget_ns, uint64_t *bcb_ns, uint64_t *rest_ns, uint64_t *npass) {
    if (cget_ns) *cget_ns = __atomic_exchange_n(&g_rp_t_cget, 0, __ATOMIC_RELAXED);
    if (bcb_ns) *bcb_ns = __atomic_exchange_n(&g_rp_t_bcb, 0, __ATOMIC_RELAXED);
    if (rest_ns) *rest_ns = __atomic_exchange_n(&g_rp_t_rest, 0, __ATOMIC_RELAXED);
    if (npass) *npass = __atomic_exchange_n(&g_rp_t_npass, 0, __ATOMIC_RELAXED);
}
void nvn_vk_first_draw_take_pass_stats(uint64_t *hits, uint64_t *miss, uint64_t *create_ns,
                                       uint64_t *fb_ns, uint64_t *fb_n) {
    if (hits) *hits = __atomic_exchange_n(&g_rp_hits, 0, __ATOMIC_RELAXED);
    if (miss) *miss = __atomic_exchange_n(&g_rp_miss, 0, __ATOMIC_RELAXED);
    if (create_ns) *create_ns = __atomic_exchange_n(&g_rp_create_ns, 0, __ATOMIC_RELAXED);
    if (fb_ns) *fb_ns = __atomic_exchange_n(&g_fb_create_ns, 0, __ATOMIC_RELAXED);
    if (fb_n) *fb_n = __atomic_exchange_n(&g_fb_create_n, 0, __ATOMIC_RELAXED);
}

// pass close / 批量提交 / fence 的窗口账（开关 debug.s2a.pass_close，默认关；读取即清零）。
// 调用方按 present 的 16 帧窗口取数；close 是本函数整体，submit 与 fence 是其中子项，不能相加。
void nvn_vk_first_draw_take_pass_close_stats(uint64_t *close_ns, uint64_t *close_n,
                                             uint64_t *sub_ns, uint64_t *sub_n,
                                             uint64_t *fence_ns, uint64_t *fence_n) {
    if (close_ns) *close_ns = __atomic_exchange_n(&g_pclose_ns, 0, __ATOMIC_RELAXED);
    if (close_n) *close_n = __atomic_exchange_n(&g_pclose_n, 0, __ATOMIC_RELAXED);
    if (sub_ns) *sub_ns = __atomic_exchange_n(&g_psubmit_ns, 0, __ATOMIC_RELAXED);
    if (sub_n) *sub_n = __atomic_exchange_n(&g_psubmit_n, 0, __ATOMIC_RELAXED);
    if (fence_ns) *fence_ns = __atomic_exchange_n(&g_pfence_ns, 0, __ATOMIC_RELAXED);
    if (fence_n) *fence_n = __atomic_exchange_n(&g_pfence_n, 0, __ATOMIC_RELAXED);
}

// 定向诊断：只对进世界紫/绿的那个淡入程序（内容前缀 2e5b8c1f）打反馈采样的判定/替换过程。
static int fd_dbg_fade(const uint8_t sha[32]) {
    return sha && sha[0] == 0x2e && sha[1] == 0x5b && sha[2] == 0x8c && sha[3] == 0x1f;
}

// 诊断开关：debug.s2a.no_feedback_fix=1 退回旧行为（直接采样本笔正在写入的附件），
// 只用于 A/B 证明紫/绿闪与反馈采样同因；默认 0 = 用预录快照。
static int fd_feedback_fix(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.no_feedback_fix", b);
        v = (b[0] && atoi(b) != 0) ? 0 : 1;
    }
    return v;
}

// 这一笔是否把当前渲染目标当纹理采样（NVN 的反馈采样）；返回第一张命中的纹理。
static struct nvn_vk_tex *fd_feedback_tex(const struct fd_program *prog,
                                          const struct s2a_record_draw_state *state,
                                          struct nvn_vk_tex *rt) {
    if (!rt || !rt->image) return NULL;
    for (int i = 0; i < prog->binding_count; i++) {
        const struct fd_binding *b = &prog->bindings[i];
        if (b->kind != 2 || b->tex_slot < 0 || b->stage_idx < 0 || b->stage_idx > 1) continue;
        if (!state->tex_present[b->stage_idx][b->tex_slot]) continue;
        uint64_t handle = state->tex_handle[b->stage_idx][b->tex_slot];
        uint32_t tid = (uint32_t)(handle & 0xfffffu);
        struct nvn_vk_tex *tex = NULL;
        unsigned slot = (unsigned)((tid * 2654435761u) % FD_TEX_LOOKUP);
        if (g_tex_lookup[slot].tex && g_tex_lookup[slot].id == (int)tid) {
            tex = g_tex_lookup[slot].tex;
        } else {
            char inner[128] = "";
            if (s2a_vk_adapter_tex_by_id((int)tid, &tex, inner, sizeof inner)) continue;
            g_tex_lookup[slot].id = (int)tid;
            g_tex_lookup[slot].tex = tex;
        }
        if (tex->image == rt->image) return tex;
    }
    return NULL;
}

// ---- 管线预热（v2）：按清单把“已记录、但本局还没遇到”的管线在加载窗口提前建好 ----
// 与绘制路径共用同一套构造规则（配方 + 程序对象）；装入前逐字节复核配方（request_id 不参与）。
// 只在加载窗口跑：每帧最多一条、真实 draw 优先；任何一步失败都静默跳过该条。
static _Atomic uint64_t g_preheat_n, g_preheat_ns, g_preheat_skip;
static uint64_t g_preheat_frame = ~0ull;

static void fd_preheat_one(struct s2a_vk_device *d) {
    if (!d || !d->device || !nvm_manifest_enabled() || nvm_manifest_count() == 0) return;
    /* C4b：先把上一批投给 worker 的预热结果装回缓存（manifest 条目本就是账本记的，
       不回写 manifest）；取回的管线让下面 take 的同配方条目在查重时直接命中。 */
    if (fd_pipe_async_on()) fd_pipe_drain(d, NULL, NULL);
    const char *cdir = getenv("S2A_DIRECT_CACHE_DIR");
    if (!cdir || !cdir[0]) return;
    struct nvm_entry e;
    static _Atomic int dbg;
    if (!nvm_manifest_take_next(&e)) {
        if (atomic_fetch_add(&dbg, 1) < 4) FD_LOG("preheat: no more entries");
        return;
    }
    if (atomic_fetch_add(&dbg, 1) < 8)
        FD_LOG("preheat: entry prog=%02x%02x%02x%02x in=%02x%02x%02x%02x colours=%u bound=0x%x fmt0=%d dfsize=%u",
               e.recipe.prog_sha[0], e.recipe.prog_sha[1], e.recipe.prog_sha[2], e.recipe.prog_sha[3],
               e.input_sha[0], e.input_sha[1], e.input_sha[2], e.input_sha[3],
               e.recipe.rt_color_count, e.recipe.rt_bound_mask, (int)e.recipe.rt_fmt[0],
               (unsigned)e.recipe.depth_fmt);
    char why[192] = "";
    // 1) 取程序结果字节（结果 SHA 由缓存校验后给出）
    uint8_t *bytes = NULL; size_t len = 0; uint8_t rsha[32] = {0};
    if (s2a_program_cache_load(cdir, e.input_sha, &bytes, &len, rsha, why, sizeof why) != 0) {
        if (atomic_fetch_add(&dbg, 1) < 12) FD_LOG("preheat: skip (program cache) %s", why);
        atomic_fetch_add(&g_preheat_skip, 1);
        return;
    }
    // 2) 解析成程序对象（模块/布局/反射）
    // parse_program 不清零输出、只填解析到的阶段：先清零，否则下面 free_program 会对没填的阶段
    //   free 栈上残值（安卓带标签指针检查直接 abort「Pointer tag … truncated」），挂接分支也会把未初始化的反射交给对象（11-01 vivo 首帧后崩 / 黑屏）
    struct fd_program prog;
    memset(&prog, 0, sizeof prog);
    int prc = nvn_vk_program_parse_graphics(bytes, len, &prog, why, sizeof why);
    free(bytes);
    if (prc != 0) { nvn_vk_program_free(&prog); atomic_fetch_add(&g_preheat_skip, 1); return; }
    struct fd_prog_obj *pobj = fd_prog_obj_get(d, &prog, rsha, why, sizeof why);
    if (pobj && !pobj->has_prog) fd_prog_obj_attach(pobj, &prog);
    else nvn_vk_program_free(&prog);
    if (!pobj) { if (atomic_fetch_add(&dbg, 1) < 12) FD_LOG("preheat: skip (prog obj) %s", why); atomic_fetch_add(&g_preheat_skip, 1); return; }
    struct nvk_pipe_recipe r = e.recipe;
    r.device = (uint64_t)(uintptr_t)d->device;   // 本局设备：命中判据的一部分
    r.request_id[0] = 0;
    uint64_t key = nvk_pipe_recipe_hash(&r);
    for (int i = 0; i < FD_PIPE_CACHE; i++)
        if (g_pipe_cache[i].used && nvk_pipe_recipe_hit(g_pipe_cache[i].key, &g_pipe_cache[i].recipe, key, &r))
            return;   // 本局已有
    // 3) render pass：只接受缓存里的（queued=1 的 pass 生命周期归帧队列，管线不能引用）
    struct fd_rp_key rp_key;
    memset(&rp_key, 0, sizeof rp_key);
    rp_key.color_count = r.rt_color_count;
    rp_key.bound_mask = r.rt_bound_mask;
    for (uint32_t i = 0; i < r.rt_color_count && i < NVN_VK_MRT_MAX_COLOR; i++)
        rp_key.color_fmt[i] = (r.rt_bound_mask & (1u << i)) ? (VkFormat)r.rt_fmt[i] : VK_FORMAT_UNDEFINED;
    rp_key.depth_fmt = r.has_depth ? (VkFormat)r.depth_fmt : VK_FORMAT_UNDEFINED;
    rp_key.clear_mask = r.clear_mask;
    rp_key.has_depth = r.has_depth ? 1 : 0;
    int rp_queued = 0;
    VkRenderPass pass = fd_rp_get(d->device, &rp_key, &rp_queued, why, sizeof why);
    if (!pass || rp_queued) { if (atomic_fetch_add(&dbg, 1) < 12) FD_LOG("preheat: skip (pass not cached) %s", why); atomic_fetch_add(&g_preheat_skip, 1); return; }
    int64_t t0 = fd_now_ns();
    // 4) 从配方还原构造输入
    VkVertexInputBindingDescription vbind[16];
    VkVertexInputAttributeDescription vattr[16];
    uint32_t vbind_count = 0, vattr_count = 0;
    int stream_used[16] = {0};
    for (int loc = 0; loc < 32 && vattr_count < 16; loc++) {
        if (!(pobj->attr_mask & (1u << loc))) continue;
        VkFormat vf = nvn_vk_format(r.vattr[loc][0]);
        if (vf == VK_FORMAT_UNDEFINED) { atomic_fetch_add(&g_preheat_skip, 1); return; }
        uint32_t stream = r.vattr[loc][2];
        if (stream >= 16) { atomic_fetch_add(&g_preheat_skip, 1); return; }
        vattr[vattr_count].location = (uint32_t)loc;
        vattr[vattr_count].binding = stream;
        vattr[vattr_count].format = vf;
        vattr[vattr_count].offset = r.vattr[loc][1];
        vattr_count++;
        if (!stream_used[stream]) {
            stream_used[stream] = 1;
            vbind[vbind_count].binding = stream;
            vbind[vbind_count].stride = r.vstride[stream];
            vbind[vbind_count].inputRate = r.vdivisor[stream] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            vbind_count++;
        }
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = vbind_count; vi.pVertexBindingDescriptions = vbind;
    vi.vertexAttributeDescriptionCount = vattr_count; vi.pVertexAttributeDescriptions = vattr;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    switch (r.prim) {
        case 0x0: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 0x1: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case 0x3: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case 0x5: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case 0x6: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        default: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }
    VkViewport viewport = { 0.0f, 1.0f, 1.0f, -1.0f, 0.0f, 1.0f };   // 动态状态，静态值不参与绘制
    VkRect2D scissor = { {0, 0}, {1, 1} };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn_state = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn_state.dynamicStateCount = 2; dyn_state.pDynamicStates = dyn;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.pViewports = &viewport; vp.scissorCount = 1; vp.pScissors = &scissor;
    // 未绑状态按 NVN 新命令缓冲默认值（各 nvnXxxStateSetDefaults），不按零值：
    // poly.mode 零值是 POINT 不是 FILL，零初始化会光栅成点。
    poly_st poly = { .cull = NVN_FACE_NONE, .front = NVN_FRONT_FACE_CCW,
                     .mode = NVN_POLYGON_MODE_FILL, .offset_en = 0 };
    ms_st mst = { .enable = 1 };
    if (r.state_seen & (1u << 4)) memcpy(&poly, r.state_obj[4], sizeof poly);
    if (r.state_seen & (1u << 3)) memcpy(&mst, r.state_obj[3], sizeof mst);
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = poly.mode == 1 ? VK_POLYGON_MODE_LINE : poly.mode == 0 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    rs.cullMode = poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : poly.cull == 2 ? VK_CULL_MODE_BACK_BIT :
                  poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    rs.frontFace = poly.front == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = mst.samples == 2 ? VK_SAMPLE_COUNT_2_BIT : mst.samples == 4 ? VK_SAMPLE_COUNT_4_BIT :
                              mst.samples == 8 ? VK_SAMPLE_COUNT_8_BIT : VK_SAMPLE_COUNT_1_BIT;
    ms.alphaToCoverageEnable = mst.a2c ? VK_TRUE : VK_FALSE;
    struct nvn_vk_mrt_blend_input blend_in;
    memset(&blend_in, 0, sizeof blend_in);
    memcpy(blend_in.color_obj, r.state_obj[1], sizeof blend_in.color_obj);
    memcpy(blend_in.chmask_obj, r.state_obj[2], sizeof blend_in.chmask_obj);
    memcpy(blend_in.blend_obj, r.blend_obj, sizeof blend_in.blend_obj);
    blend_in.state_seen = r.state_seen;
    blend_in.blend_seen = r.blend_seen;
    VkPipelineColorBlendAttachmentState cba[NVN_VK_MRT_MAX_COLOR];
    memset(cba, 0, sizeof cba);
    for (uint32_t i = 0; i < r.rt_color_count && i < NVN_VK_MRT_MAX_COLOR; i++) {
        int bound = (r.rt_bound_mask >> i) & 1u;
        nvn_vk_mrt_color_blend(i, bound, (VkFormat)r.rt_fmt[i], &blend_in, &cba[i]);
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = r.depth_only ? 0u : r.rt_color_count;
    cb.pAttachments = r.depth_only ? NULL : cba;
    ds_st dss_nvn = { .dt = 0, .dw = 1, .df = NVN_DEPTH_FUNC_LESS, .st = 0,
                      .sf_f = NVN_STENCIL_FUNC_ALWAYS, .sf_b = NVN_STENCIL_FUNC_ALWAYS,
                      .fail_f = NVN_STENCIL_OP_KEEP, .zfail_f = NVN_STENCIL_OP_KEEP,
                      .zpass_f = NVN_STENCIL_OP_KEEP, .fail_b = NVN_STENCIL_OP_KEEP,
                      .zfail_b = NVN_STENCIL_OP_KEEP, .zpass_b = NVN_STENCIL_OP_KEEP };
    if (r.state_seen & (1u << 5)) memcpy(&dss_nvn, r.state_obj[5], sizeof dss_nvn);
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    if (r.has_depth) {
        dss.depthTestEnable = dss_nvn.dt ? VK_TRUE : VK_FALSE;
        dss.depthWriteEnable = dss_nvn.dw ? VK_TRUE : VK_FALSE;
        dss.depthCompareOp = nvk_compare_op(dss_nvn.df);
        dss.minDepthBounds = 0.0f; dss.maxDepthBounds = 1.0f;
        dss.stencilTestEnable = dss_nvn.st ? VK_TRUE : VK_FALSE;
        dss.front.failOp = nvk_stencil_op(dss_nvn.fail_f);
        dss.front.depthFailOp = nvk_stencil_op(dss_nvn.zfail_f);
        dss.front.passOp = nvk_stencil_op(dss_nvn.zpass_f);
        dss.front.compareOp = nvk_compare_op(dss_nvn.sf_f);
        dss.front.compareMask = 0xffu; dss.front.writeMask = 0xffu;
        dss.back.failOp = nvk_stencil_op(dss_nvn.fail_b);
        dss.back.depthFailOp = nvk_stencil_op(dss_nvn.zfail_b);
        dss.back.passOp = nvk_stencil_op(dss_nvn.zpass_b);
        dss.back.compareOp = nvk_compare_op(dss_nvn.sf_b);
        dss.back.compareMask = 0xffu; dss.back.writeMask = 0xffu;
    }
    /* C4b：预热投给 worker 后台建，不占提交线程（debug.s2a.pipe_async）。结果由下一个
       tick / 下一笔 draw 开头的 fd_pipe_drain 取回装缓存；pobj 由 pend 表 pin 保活。 */
    if (fd_pipe_async_ensure(d, why, sizeof why)) {
        struct nvk_pipe_job_spec spec;
        memset(&spec, 0, sizeof spec);
        spec.device_generation = (uint64_t)(uintptr_t)d->device;
        spec.device = d->device;
        spec.cache = fd_pipeline_cache(d);
        spec.layout = pobj->playout;
        spec.render_pass = pass;
        spec.subpass = 0;
        spec.recipe = r;
        spec.stage_count = 2;
        spec.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        spec.stages[0].spirv = (const uint32_t *)pobj->prog.stage[0].spirv;
        spec.stages[0].bytes = pobj->prog.stage[0].spirv_bytes;
        snprintf(spec.stages[0].entry, sizeof spec.stages[0].entry, "main");
        spec.stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        spec.stages[1].spirv = (const uint32_t *)pobj->prog.stage[1].spirv;
        spec.stages[1].bytes = pobj->prog.stage[1].spirv_bytes;
        snprintf(spec.stages[1].entry, sizeof spec.stages[1].entry, "main");
        spec.binding_count = vbind_count;
        memcpy(spec.bindings, vbind, vbind_count * sizeof vbind[0]);
        spec.attr_count = vattr_count;
        memcpy(spec.attrs, vattr, vattr_count * sizeof vattr[0]);
        spec.ia = ia;
        spec.vp = vp;
        spec.vp.pViewports = NULL; spec.vp.pScissors = NULL;
        spec.viewport = viewport;
        spec.scissor = scissor;
        spec.rs = rs;
        spec.ms = ms;
        spec.cb = cb;
        spec.blend_count = r.depth_only ? 0 : r.rt_color_count;
        if (spec.blend_count) memcpy(spec.blend, cba, spec.blend_count * sizeof cba[0]);
        spec.cb.pAttachments = NULL;
        spec.ds = dss;
        spec.has_depth = r.has_depth;
        spec.dynamic_count = 2;
        spec.dynamic[0] = VK_DYNAMIC_STATE_VIEWPORT;
        spec.dynamic[1] = VK_DYNAMIC_STATE_SCISSOR;
        int pi = fd_pipe_pend_find(&spec.recipe);
        int added = 0;
        if (pi < 0) { pi = fd_pipe_pend_add(&spec.recipe, pobj); added = pi >= 0; }
        if (pi >= 0) {
            uint64_t jid = 0;
            int src = nvn_vk_pipe_worker_submit(&spec, &jid, why, sizeof why);
            if (src == 0 || src == 1) {   // 受理或同配方已在途/待取：本条已处理完
                atomic_fetch_add(&g_preheat_ns, (uint64_t)(fd_now_ns() - t0));
                atomic_fetch_add(&g_preheat_n, 1);
                atomic_fetch_add(&g_pfu_submit, 1);
                return;
            }
            if (added) fd_pipe_pend_drop(pi);   // 只撤本次新加的 pin；旧 pend 归在途 job
            if (src != -2 && src != -3) {   // -1 参数/-4 代际：如实跳过本条
                atomic_fetch_add(&g_preheat_skip, 1);
                return;
            }
            /* -2 队列满 / -3 worker 未起：落回下面的同步创建（与关开关同路径）。 */
        }
        /* pend 表满同样落回同步创建。 */
    }
    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = pobj->vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = pobj->fs; stages[1].pName = "main";
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms; gpi.pColorBlendState = &cb;
    if (r.has_depth) gpi.pDepthStencilState = &dss;
    gpi.pDynamicState = &dyn_state;
    gpi.layout = pobj->playout; gpi.renderPass = pass; gpi.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const int pre_cache_lock = fd_pipe_async_on() &&
        nvn_vk_pipe_worker_cache_lock && nvn_vk_pipe_worker_cache_unlock;
    if (pre_cache_lock) nvn_vk_pipe_worker_cache_lock();
    VkResult pre_rc = vkCreateGraphicsPipelines(d->device, fd_pipeline_cache(d), 1,
                                                &gpi, NULL, &pipeline);
    if (pre_cache_lock) nvn_vk_pipe_worker_cache_unlock();
    if (pre_rc != VK_SUCCESS || !pipeline) {
        atomic_fetch_add(&g_preheat_skip, 1);
        return;
    }
    atomic_fetch_add(&g_preheat_ns, (uint64_t)(fd_now_ns() - t0));
    atomic_fetch_add(&g_preheat_n, 1);
    // 5) 装入管线缓存（不逐出在用的；满了就放弃这条）
    int slot = -1;
    for (int i = 0; i < FD_PIPE_CACHE; i++) if (!g_pipe_cache[i].used) { slot = i; break; }
    if (slot < 0) { vkDestroyPipeline(d->device, pipeline, NULL); return; }
    r.request_id[0] = 0;
    g_pipe_cache[slot].used = 1; g_pipe_cache[slot].key = key;
    *fd_pipe_index_slot(key) = slot;
    g_pipe_cache[slot].recipe = r;
    g_pipe_cache[slot].pipe = pipeline; g_pipe_cache[slot].obj = pobj;
    pobj->users++;
    g_pipe_cache[slot].last_use = ++g_pipe_clock;
}

void nvn_vk_first_draw_preheat_stats(uint64_t *n, uint64_t *ns, uint64_t *skip) {
    if (n) *n = atomic_load(&g_preheat_n);
    if (ns) *ns = atomic_load(&g_preheat_ns);
    if (skip) *skip = atomic_load(&g_preheat_skip);
}

// ---- 块编码渲染目标（压缩底图 + “块=像素”的无压缩视图）-----------------------------
// TOTK 把 32x32x32 的 RGTC1(BC4) 3D 纹理每个深度切片当渲染目标，附件视图用 RGBA16UI 之类
// 无压缩格式覆盖：一个像素 = 一个 8 字节压缩块，也就是在 GPU 上直接编码 BC4。Vulkan 既不能
// 把压缩格式当颜色附件，也不能建压缩↔无压缩视图，所以渲染到等尺寸影子（bw×bh 像素）+ 一个
// buffer，pass 前 底图→buffer→影子，pass 后 影子→buffer→底图，字节原样往返：压缩格式没有
// COLOR_ATTACHMENT 特性、不能建无压缩视图，image→image 拷贝又要求格式兼容，buffer 往返是
// Vulkan 上唯一可行的通路。影子按 (格式, bw, bh) 复用，等所属提交完成才换内容。
// 每个“底图+子资源”的渲染各要一个影子；同一形状的连续切片（TOTK 是 32 个深度切片）在所属帧
// 提交完成前不能复用，所以池按需增长。内存很小（32x32 BC4 的 8x8 影子约 1 KiB/片），但按字节
// 预算硬上限 fail-closed，不无限增长。
#define FD_BCRT_POOL 1024
struct fd_bcrt_slot {
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkBuffer buf; VkDeviceMemory bmem;
    VkFormat fmt; uint32_t bw, bh, block_bytes;
    VkDeviceSize img_bytes, buf_bytes;
    uint64_t owner_frame;
    int inited, in_use, used;
};
static struct fd_bcrt_slot g_bcrt_pool[FD_BCRT_POOL];
static int g_bcrt_pool_n;
static _Atomic uint64_t g_bcrt_shadows, g_bcrt_shadow_bytes, g_bcrt_precopies, g_bcrt_postcopies, g_bcrt_passes;
static uint64_t fd_bcrt_budget_bytes(void) {
    static uint64_t v;
    if (!v) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.bcrt_mb", b);
        v = (b[0] && atoi(b) > 0) ? (uint64_t)atoi(b) << 20 : (256ull << 20);
    }
    return v;
}
struct fd_bcrt_pend_slot {
    struct nvn_vk_bcrt_plan plan;
    struct fd_bcrt_slot *slot;
    const tex_t *tex;   // 底图 guest 对象：拷回后按子资源标 GPU 写
    VkImage base;
};
static struct fd_bcrt_pend_slot g_bcrt_pend[NVN_VK_MRT_MAX_COLOR];
static int g_bcrt_n;
// 块编码影子每笔独占 pass；启动期这类 pass 可能上百笔（TOTK 的 BC4 LUT 逐深度切片生成），
// 一个帧内全堆着会撞帧队列的完成证明批次表。每攒 32 笔做一次等待式 flush：拿到完成证明后
// 影子可以按帧完成语义安全复用，批次表也不会溢出。
static int g_bcrt_since_flush;
// 诊断开关（默认关）：debug.s2a.bcrt_skip_copy=1 跳过影子往返拷贝，用来把“拷贝”与“块空间 pass”
// 的失败分开；debug.s2a.bcrt_log=1 打分段日志。
static int fd_bcrt_prop(const char *name) {
    char b[PROP_VALUE_MAX] = {0};
    __system_property_get(name, b);
    return (b[0] && atoi(b)) ? 1 : 0;
}
static int fd_bcrt_log(void) { static int v = -1; if (v < 0) v = fd_bcrt_prop("debug.s2a.bcrt_log"); return v; }
static int fd_bcrt_skip_copy(void) { static int v = -1; if (v < 0) v = fd_bcrt_prop("debug.s2a.bcrt_skip_copy"); return v; }

static void fd_bcrt_reset(void) {
    g_bcrt_n = 0;
    for (int i = 0; i < g_bcrt_pool_n; i++) g_bcrt_pool[i].in_use = 0;
}

static struct fd_bcrt_slot *fd_bcrt_acquire(struct s2a_vk_device *d, VkFormat fmt, uint32_t bw, uint32_t bh,
                                            uint32_t block_bytes, char *why, size_t why_size) {
    struct fd_bcrt_slot *slot = NULL;
    for (int i = 0; i < g_bcrt_pool_n; i++) {
        struct fd_bcrt_slot *s = &g_bcrt_pool[i];
        if (!s->used || s->in_use || s->fmt != fmt || s->bw != bw || s->bh != bh) continue;
        // 只有所属帧的提交已经拿到完成证明，旧内容/旧视图才没人引用。
        if (s->owner_frame <= g_snap_retired_frame) { slot = s; break; }
    }
    if (!slot) {
        if (g_bcrt_pool_n >= FD_BCRT_POOL) {
            uint32_t same_shape = 0;
            for (int i = 0; i < g_bcrt_pool_n; i++)
                if (g_bcrt_pool[i].used && g_bcrt_pool[i].fmt == fmt &&
                    g_bcrt_pool[i].bw == bw && g_bcrt_pool[i].bh == bh) same_shape++;
            snprintf(why, why_size,
                     "block-encoding shadow pool is full (%d shadows, %u of %ux%u still in flight); "
                     "refusing to recycle an in-flight image", FD_BCRT_POOL, same_shape, bw, bh);
            return NULL;
        }
        struct fd_bcrt_slot *s = &g_bcrt_pool[g_bcrt_pool_n];
        memset(s, 0, sizeof *s);
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = (VkExtent3D){ bw, bh, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d->device, &ici, NULL, &s->img) != VK_SUCCESS) {
            snprintf(why, why_size, "block-encoding shadow image (%ux%u format %d) creation failed", bw, bh, (int)fmt);
            return NULL;
        }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(d->device, s->img, &mr);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = (uint32_t)s2a_vk_memory_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &ai, NULL, &s->mem) != VK_SUCCESS ||
            vkBindImageMemory(d->device, s->img, s->mem, 0) != VK_SUCCESS) {
            if (s->mem) vkFreeMemory(d->device, s->mem, NULL);
            vkDestroyImage(d->device, s->img, NULL);
            memset(s, 0, sizeof *s);
            snprintf(why, why_size, "block-encoding shadow image memory (%ux%u) allocation failed", bw, bh);
            return NULL;
        }
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = s->img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(d->device, &vci, NULL, &s->view) != VK_SUCCESS) {
            vkFreeMemory(d->device, s->mem, NULL);
            vkDestroyImage(d->device, s->img, NULL);
            memset(s, 0, sizeof *s);
            snprintf(why, why_size, "block-encoding shadow view creation failed");
            return NULL;
        }
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)bw * bh * block_bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d->device, &bci, NULL, &s->buf) != VK_SUCCESS) {
            vkDestroyImageView(d->device, s->view, NULL);
            vkFreeMemory(d->device, s->mem, NULL);
            vkDestroyImage(d->device, s->img, NULL);
            memset(s, 0, sizeof *s);
            snprintf(why, why_size, "block-encoding shadow staging buffer creation failed");
            return NULL;
        }
        VkMemoryRequirements br; vkGetBufferMemoryRequirements(d->device, s->buf, &br);
        VkMemoryAllocateInfo bai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        bai.allocationSize = br.size;
        bai.memoryTypeIndex = (uint32_t)s2a_vk_memory_type(d, br.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
        if (bai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &bai, NULL, &s->bmem) != VK_SUCCESS ||
            vkBindBufferMemory(d->device, s->buf, s->bmem, 0) != VK_SUCCESS) {
            if (s->bmem) vkFreeMemory(d->device, s->bmem, NULL);
            vkDestroyBuffer(d->device, s->buf, NULL);
            vkDestroyImageView(d->device, s->view, NULL);
            vkFreeMemory(d->device, s->mem, NULL);
            vkDestroyImage(d->device, s->img, NULL);
            memset(s, 0, sizeof *s);
            snprintf(why, why_size, "block-encoding shadow staging memory allocation failed");
            return NULL;
        }
        const uint64_t slot_bytes = (uint64_t)mr.size + (uint64_t)br.size;
        if (atomic_load(&g_bcrt_shadow_bytes) + slot_bytes > fd_bcrt_budget_bytes()) {
            vkFreeMemory(d->device, s->bmem, NULL);
            vkDestroyBuffer(d->device, s->buf, NULL);
            vkDestroyImageView(d->device, s->view, NULL);
            vkFreeMemory(d->device, s->mem, NULL);
            vkDestroyImage(d->device, s->img, NULL);
            memset(s, 0, sizeof *s);
            snprintf(why, why_size,
                     "block-encoding shadow budget exceeded (%llu + %llu bytes > %llu); raise debug.s2a.bcrt_mb",
                     (unsigned long long)atomic_load(&g_bcrt_shadow_bytes), (unsigned long long)slot_bytes,
                     (unsigned long long)fd_bcrt_budget_bytes());
            return NULL;
        }
        s->fmt = fmt; s->bw = bw; s->bh = bh; s->block_bytes = block_bytes;
        s->img_bytes = mr.size; s->buf_bytes = br.size;
        s->used = 1;
        g_bcrt_pool_n++;
        atomic_fetch_add(&g_bcrt_shadows, 1);
        atomic_fetch_add(&g_bcrt_shadow_bytes, slot_bytes);
        FD_LOG("block-encoding render target: %ux%u %d-byte-block shadow created (format %d, %llu+%llu bytes) "
               "for a compressed texture drawn through a block-sized view", bw, bh, (int)block_bytes,
               (int)fmt, (unsigned long long)mr.size, (unsigned long long)br.size);
        slot = s;
    }
    slot->in_use = 1;
    slot->owner_frame = g_frame_id;
    return slot;
}

static void fd_bcrt_img_barrier(VkCommandBuffer cmd, VkImage img, uint32_t level, uint32_t layer,
                                int is_3d, VkImageLayout from, VkImageLayout to,
                                VkAccessFlags src, VkAccessFlags dst,
                                VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    // 3D 子资源只能按 level 转（层=深度切片不能单独选），2D/数组才按层。
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, level, 1, is_3d ? 0u : layer, 1 };
    b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

// pass 开始前：底图（压缩，整 mip/切片）→ buffer → 影子。
static void fd_bcrt_precopy(VkCommandBuffer cmd) {
    if (fd_bcrt_skip_copy()) return;
    for (int i = 0; i < g_bcrt_n; i++) {
        struct fd_bcrt_pend_slot *p = &g_bcrt_pend[i];
        struct fd_bcrt_slot *s = p->slot;
        if (!s->inited) {
            fd_bcrt_img_barrier(cmd, s->img, 0, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
            s->inited = 1;
        }
        fd_bcrt_img_barrier(cmd, p->base, p->plan.level, p->plan.layer, p->plan.is_3d,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        static int log_n;
        if (fd_bcrt_log() && log_n++ < 8)
            FD_LOG("bcrt precopy[%d]: base=%p lv=%u slice=%u %ux%u -> buf=%p %ux%u blocks -> shadow=%p",
                   i, (void *)p->base, p->plan.level, p->plan.layer, p->plan.width, p->plan.height,
                   (void *)s->buf, s->bw, s->bh, (void *)s->img);
        VkBufferImageCopy c;
        nvn_vk_bcrt_img_to_buf(&p->plan, &c);
        if (fd_bcrt_log()) FD_LOG("bcrt precopy[%d]: img2buf z=%d extent=%ux%u", i, (int)c.imageOffset.z, c.imageExtent.width, c.imageExtent.height);
        vkCmdCopyImageToBuffer(cmd, p->base, VK_IMAGE_LAYOUT_GENERAL, s->buf, 1, &c);
        if (fd_bcrt_log()) FD_LOG("bcrt precopy[%d]: img2buf recorded", i);
        VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = s->buf; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 1, &bb, 0, NULL);
        nvn_vk_bcrt_buf_to_shadow(&p->plan, &c);
        if (fd_bcrt_log()) FD_LOG("bcrt precopy[%d]: buf2shadow extent=%ux%u", i, c.imageExtent.width, c.imageExtent.height);
        vkCmdCopyBufferToImage(cmd, s->buf, s->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        if (fd_bcrt_log()) FD_LOG("bcrt precopy[%d]: buf2shadow recorded", i);
        // 渲染前把影子写发布给颜色附件阶段。
        fd_bcrt_img_barrier(cmd, s->img, 0, 0, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        atomic_fetch_add(&g_bcrt_precopies, 1);
    }
}

// pass 结束后：影子 → buffer → 底图；拷回后该子资源归 GPU（CPU 镜像不可信）。
static void fd_bcrt_postcopy(VkCommandBuffer cmd) {
    if (fd_bcrt_skip_copy()) return;
    if (fd_bcrt_log()) FD_LOG("bcrt postcopy: %d attachment(s)", g_bcrt_n);
    for (int i = 0; i < g_bcrt_n; i++) {
        struct fd_bcrt_pend_slot *p = &g_bcrt_pend[i];
        struct fd_bcrt_slot *s = p->slot;
        fd_bcrt_img_barrier(cmd, s->img, 0, 0, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c;
        nvn_vk_bcrt_shadow_to_buf(&p->plan, &c);
        vkCmdCopyImageToBuffer(cmd, s->img, VK_IMAGE_LAYOUT_GENERAL, s->buf, 1, &c);
        VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = s->buf; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 1, &bb, 0, NULL);
        nvn_vk_bcrt_buf_to_img(&p->plan, &c);
        vkCmdCopyBufferToImage(cmd, s->buf, p->base, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        fd_bcrt_img_barrier(cmd, p->base, p->plan.level, p->plan.layer, p->plan.is_3d,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        if (p->tex) s2a_vk_adapter_tex_gpu_written_subres(p->tex, p->plan.level, p->plan.layer);
        atomic_fetch_add(&g_bcrt_postcopies, 1);
    }
}

// 每帧最多一条；只在加载窗口（前 ~900 帧）跑。真实 draw 已先完成，不会抢在需求之前。
static void fd_preheat_tick(struct s2a_vk_device *d) {
    if (g_frame_seq == g_preheat_frame) return;
    g_preheat_frame = g_frame_seq;
    if (g_frame_seq > 900) return;
    if (g_frame_seq & 1ull) return;   // 每 2 帧最多一条：加载窗口的每帧增量减半
    fd_preheat_one(d);
}

int nvn_vk_first_draw(struct s2a_vk_device *d, const struct s2a_record_draw_state *state,
                      const uint8_t *program, size_t program_bytes, const uint8_t program_sha[32],
                      const uint8_t program_input_sha[32],
                      uint64_t session, uint64_t generation, const char *request_id,
                      char *why, size_t why_size) {
    if (!d || !d->device || !state || !program || !program_bytes) return fail(why, why_size, "bad first-draw arguments");
    fd_bcrt_reset();   // 每笔从干净影子状态开始：上一笔失败/异常时不外溢
    // 主体未归属拆分（开关 debug.s2a.body_gap=1，默认关）：函数入口戳，用来量「入口→t_start」这段
    // 之前只有 draw_tail 才覆盖的工作（samp/lookup ensure、程序对象解析、tex 槽比较、队列腾位检查）。
    // 关闭时返回 0，不取钟。
    const int64_t t_body0 = fd_body_stamp();
    // 后段残差（默认关）：入口到 t_start 之间是 draw timing 分项之外的工作（samp/lookup ensure、
    // perf_timing 下的纹理槽比较、程序对象解析、帧队列腾位检查），原先没有归属。
    const int64_t t_tail_entry = fd_tail_stamp();
    atomic_fetch_add_explicit(&g_draws_since_report, 1, memory_order_relaxed);
    if (atomic_fetch_add_explicit(&g_draws_this_frame, 1, memory_order_relaxed) == 0)
        g_frame_ref_seq = s2a_poolahb_begin_seq();   // 本帧所有保活记在这个单调序号下
    fd_samp_cache_ensure(d);
    fd_lookup_ensure();
    if (fd_perf_timing_enabled()) {
        // 只统计，不影响任何行为：与上一笔比较纹理槽（present + handle）。诊断口径默认关。
        int diff = !g_prev_tex_valid;
        for (int st = 0; st < 2 && !diff; st++)
            for (int sl = 0; sl < 32; sl++) {
                if (g_prev_tex_present[st][sl] != (uint8_t)state->tex_present[st][sl]) { diff = 1; break; }
                if (state->tex_present[st][sl] &&
                    g_prev_tex_sig[st][sl] != (uint32_t)(state->tex_handle[st][sl] & 0xFFFFFFFFu)) { diff = 1; break; }
            }
        for (int st = 0; st < 2; st++)
            for (int sl = 0; sl < 32; sl++) {
                g_prev_tex_present[st][sl] = (uint8_t)state->tex_present[st][sl];
                g_prev_tex_sig[st][sl] = (uint32_t)(state->tex_handle[st][sl] & 0xFFFFFFFFu);
            }
        g_prev_tex_valid = 1;
        atomic_fetch_add(&g_key_draws, 1);
        if (diff) atomic_fetch_add(&g_key_diff, 1); else atomic_fetch_add(&g_key_same, 1);
    }
    int64_t t_start = fd_stamp(), t_parsed = 0, t_modules = 0, t_layouts = 0, t_pass = 0, t_pipeline = 0, t_submitted = 0;
    int64_t t_support = 0, t_res = 0, t_vbuf = 0, t_desc = 0;
    int64_t t_pass_a = 0, t_pass_b = 0;   // pass 段细分：rp 查找 / framebuffer 取得
    int64_t t_tail_post0 = 0;             // 后段残差：t_submitted 之后到返回的起点（默认 0 = 不采）
    if (t_tail_entry && t_start) {
        __atomic_fetch_add(&g_tail_pre_ns, (uint64_t)(t_start - t_tail_entry), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_tail_pre_n, 1, __ATOMIC_RELAXED);
    }
    struct fd_program prog;
    memset(&prog, 0, sizeof prog);
    uint8_t prog_sha[32];
    {
        struct fd_prog_obj *phit = program_sha ? fd_prog_obj_find(d, program_sha) : NULL;
        if (phit && phit->has_prog) {
            memcpy(prog_sha, program_sha, 32);
            prog = phit->prog;   // 对象持有；本次只读
        } else {
            struct fd_program *cp = fd_prog_get(program, program_bytes, program_sha, prog_sha, why, why_size);
            if (!cp) return -1;
            prog = *cp;
        }
    }
    if (fd_skip_prog(prog_sha)) return 0;   // 诊断：按程序前缀跳过这一笔
    char trace_id[80] = {0};
    int trace_on = fd_trace_wanted(request_id, prog_sha, trace_id, sizeof trace_id);
    // 连续同一附件的绘制合并进一个 pass；被 trace/probe 选中的那一笔才走逐笔完整命令。
    // 只按 request 前缀时旧行为不变（命中所有同前缀 draw）；世界热路径用 prog: 前缀只抓一笔。
    // trace 不再强制逐笔路径：逐笔同步提交在这类全屏合成 draw 上会踩到驱动的 SIGSEGV
    // （见坑记录），因此 trace 只做输入/UBO 转储；RT before/after 仍只在 probe 的逐笔路径。
    int merge_draw = !fd_probe_enabled();
    if (!merge_draw && nvn_vk_query_counting()) {
        if (!trace_on)
            return fail(why, why_size,
                        "samples-passed counting is active: the trace/probe per-draw path records no render pass, "
                        "so its samples would not be counted; refusing this draw instead of under-reporting");
        static int warned;
        if (!warned) {
            warned = 1;
            FD_LOG("trace: targeted draw records no render pass; samples for this draw are not counted "
                   "(diagnostic run only)");
        }
    }
    if (fd_fq_poisoned())
        return fail(why, why_size, "frame queue failed earlier in this session; refusing to record more draws");
    if (!merge_draw && g_op.open) {
        // 逐笔诊断路径：本笔之前录制的 pass 必须真正完成，后面的 draw 才能安全覆盖同一附件。
        if (nvn_vk_first_draw_pass_break(1) != 0)
            return fail(why, why_size, "pass break before this draw has no completion proof; draw not recorded");
    }
    g_fq.dev = d;
    g_fq.dev_handle = d->device;
    if (!fd_fq_no_make_room() &&
        (g_fq.ncmd >= fd_fq_cmd_capacity() - 1 || g_fq.nbuf >= FD_FRAME_BUFS - 64 ||
         g_fq.npass >= FD_FRAME_PASSES - 4)) {
        if (nvn_vk_first_draw_flush() != 0)
            return fail(why, why_size, "frame queue flush failed while making room for this draw");
    }
    if (state->count == 0) return fail(why, why_size, "first draw has zero vertices");
    // 零附件 draw（既没有颜色槽也没有深度）是合法的：TOTK 的“编码”pass 只往可写 storage buffer
    // 里写结果。Vulkan 允许零附件 render pass + 零附件 framebuffer，光栅化仍会执行片元着色器。
    // 注意：rt_count==0 但绑了深度是合法的 **depth-only pass**，不能当零附件把深度静默丢掉。
    int no_attach = 0;
    t_parsed = fd_stamp();

    int rc = -1;
    int pipe_from_cache = 0;
    int deferred = 0;
    // fb/pass 所有权：只有本笔创建、且还没交给帧队列或缓存的才能在失败清理里销毁。
    // 复用 open pass 时 fb/pass 是借用的；缓存里的 render pass 归缓存；merge 模式下创建即交给帧队列。
    int fb_owned = 0, pass_owned = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layouts[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkBuffer support_buf = VK_NULL_HANDLE, ib_buf = VK_NULL_HANDLE;
    uint64_t support_off = 0, support_size = 0, ib_off = 0;
    uint8_t *support_map = NULL;
    uint32_t tex_unnorm_bits[3] = {0};
    uint64_t res_off[FD_MAX_BINDINGS] = {0}, res_size[FD_MAX_BINDINGS] = {0};
    uint64_t vb_off[16] = {0};
    VkBuffer res_buf[FD_MAX_BINDINGS] = {0};
    VkImageView res_view[FD_MAX_BINDINGS] = {0};
    VkSampler res_sampler[FD_MAX_BINDINGS] = {0};
    int res_kind[FD_MAX_BINDINGS] = {0};   // 0 未解析 / 1 UBO / 2 组合采样 / 3 support / 4 storage
    int res_ssbo_wr[FD_MAX_BINDINGS] = {0};  // 该绑定是可写 storage buffer（draw 后要发布写）
    VkBuffer vb_buf[16] = {0};
    // 像素读回：draw 前后各读目标 (0,0)，证明 GPU 真的执行了这笔绘制而不是只建了对象。
    VkBuffer probe_buf = VK_NULL_HANDLE;
    VkDeviceMemory probe_mem = VK_NULL_HANDLE;
    void *probe_map = NULL;
    struct fd_trace tr = {0};   // done also handles attachment failures before trace allocation.
    struct nvn_vk_tex *rt = NULL;
    VkImageView fb_view = VK_NULL_HANDLE;   // 反馈采样：本笔绑定用的预录快照视图
    // 快照源子资源（绑定替换判定用）：3D 底图的 fb_layer 是深度切片，fb_is3d=1。
    VkFormat fb_fmt = VK_FORMAT_UNDEFINED;
    uint32_t fb_level = 0, fb_layer = 0;
    int fb_is3d = 0;

    // 附件解析：先解析深度（无颜色附件的 pass 用它定尺寸/格式），再解析颜色槽 0。
    // NVN 允许颜色槽为空（SetRenderTargets 给了 numTargets 但该槽没有纹理）：这是“无颜色附件”
    // 的合法 pass，不是缺失事实，也不是可以把格式补成 Float/RGBA8 的场景。
    struct nvn_vk_tex *depth_tex = NULL;
    VkImageView depth_att = VK_NULL_HANDLE;
    VkFormat rt_format = VK_FORMAT_UNDEFINED, depth_fmt = VK_FORMAT_UNDEFINED;
    int depth_only = 0;
    if (state->has_depth && g_prop_no_depth[0] != '1') {
        const tex_t *dt = (const tex_t *)state->depth_key;
        if (!dt || dt->magic != 0x00584554u) { fail(why, why_size, "depth target is not a live NVNtexure object"); goto done; }
        depth_tex = s2a_vk_adapter_tex(dt, why, why_size);
        if (!depth_tex || !depth_tex->view || !depth_tex->view_att) { fail(why, why_size, "depth target texture has no view"); goto done; }
        VkFormat dfmt_chk = nvn_vk_format(depth_tex->nvn_format);
        FD_VLOG("depth target: nvn_fmt=0x%x vk_fmt=%d %ux%u levels=%u layers=%u depth=%u",
               depth_tex->nvn_format, (int)dfmt_chk, depth_tex->width, depth_tex->height,
               depth_tex->levels, depth_tex->layers, depth_tex->depth);
        if (!nvn_vk_format_is_depth(dfmt_chk)) {
            char msg[160];
            snprintf(msg, sizeof msg, "depth target format %d is not a depth format", (int)dfmt_chk);
            fail(why, why_size, msg);
            goto done;
        }
        // 附件视图：SetRenderTargets 的 dview 选 mip/层；默认整幅多 mip/多层的图不能直接当附件。
        {
            const view_t *dv = state->has_dview ? &state->dview : NULL;
            depth_att = s2a_vk_adapter_attachment_view(depth_tex, dv, &depth_fmt, why, why_size);
            if (!depth_att) goto done;
            if (!nvn_vk_format_is_depth(depth_fmt)) {
                fail(why, why_size, "depth attachment view format is not a depth format");
                goto done;
            }
            // 附件视图必须在创建后立刻保活：后面解析颜色槽时还会请求/创建视图，
            // 未保活的旧视图会被同一底图的视图缓存当作 LRU 淘汰；等到本函数末尾再
            // pin 时，framebuffer 里已经是销毁过的句柄（8E 在 vkCmdDrawIndexed 内崩）。
            if (fd_pin_tex(depth_tex) != 0) {
                fail(why, why_size, "texture pin list is full; the draw is not recorded (no unregistered attachment)");
                goto done;
            }
            if (fd_pin_view(depth_tex, depth_att) != 0) {
                fail(why, why_size, "attachment view is not registered for keeping; refusing this draw");
                goto done;
            }
        }
    }
    // 深度解析完毕：rt_count==0 且没有深度才算零附件；绑了深度就走 depth-only 计划。
    no_attach = (state->rt_count == 0 && depth_tex == NULL);

    // 颜色附件逐槽解析：槽 0 不再是特例（槽 0 空、槽 1 有也合法）。
    // 每槽都按 SetRenderTargets 的真实 cview 取附件视图；格式/尺寸都来自真实对象，不补默认值。
    struct nvn_vk_tex *rt_texs[NVN_VK_MRT_MAX_COLOR] = {0};
    VkImage rt_imgs[NVN_VK_MRT_MAX_COLOR] = {0};
    VkImageView rt_atts[NVN_VK_MRT_MAX_COLOR] = {0};
    VkFormat rt_formats[NVN_VK_MRT_MAX_COLOR];
    uint64_t rt_serials[NVN_VK_MRT_MAX_COLOR] = {0};
    struct nvn_vk_mrt_slot mrt_slots[NVN_VK_MRT_MAX_COLOR];
    struct nvn_vk_mrt_plan mrt_plan;
    memset(rt_formats, 0, sizeof rt_formats);
    memset(mrt_slots, 0, sizeof mrt_slots);
    memset(&mrt_plan, 0, sizeof mrt_plan);
    for (uint32_t i = 0; i < NVN_VK_MRT_MAX_COLOR; i++) rt_formats[i] = VK_FORMAT_UNDEFINED;

    // 逐槽颜色状态：MRT 的 blend_en/chmask 都是每目标独立，blend 状态按 NVNblendState.target 分槽。
    // 未绑状态按 NVN 新命令缓冲默认值（SetDefaults），不按零值：chmask 零值会让
    // colorWriteMask=0 整槽不写（#1409 黑屏机理），poly.mode 零值是 POINT 不是 FILL。
    color_st color = { .blend_en = 0, .logic_op = NVN_LOGIC_OP_COPY,
                       .alpha_func = NVN_ALPHA_FUNC_ALWAYS };
    blend_st blend = { .src_rgb = NVN_BLEND_FUNC_ONE, .dst_rgb = NVN_BLEND_FUNC_ZERO,
                       .src_a = NVN_BLEND_FUNC_ONE, .dst_a = NVN_BLEND_FUNC_ZERO,
                       .eq_rgb = NVN_BLEND_EQUATION_ADD, .eq_a = NVN_BLEND_EQUATION_ADD,
                       .adv_premul = 1, .adv_normdst = 1 };
    chmask_st chmask = { .mask = 0xffffffffu };
    poly_st poly = { .cull = NVN_FACE_NONE, .front = NVN_FRONT_FACE_CCW,
                     .mode = NVN_POLYGON_MODE_FILL, .offset_en = 0 };
    ms_st mst = { .enable = 1 };
    if (state->state_seen & (1u << 1)) memcpy(&color, state->state_obj[1], sizeof color);
    if (state->state_seen & (1u << 0)) memcpy(&blend, state->state_obj[0], sizeof blend);   // 仅 trace 日志
    if (state->state_seen & (1u << 2)) memcpy(&chmask, state->state_obj[2], sizeof chmask);
    if (state->state_seen & (1u << 4)) memcpy(&poly, state->state_obj[4], sizeof poly);
    if (state->state_seen & (1u << 3)) memcpy(&mst, state->state_obj[3], sizeof mst);
    struct nvn_vk_mrt_blend_input mrt_blend;
    memset(&mrt_blend, 0, sizeof mrt_blend);
    memcpy(mrt_blend.color_obj, state->state_obj[1], sizeof mrt_blend.color_obj);
    memcpy(mrt_blend.chmask_obj, state->state_obj[2], sizeof mrt_blend.chmask_obj);
    memcpy(mrt_blend.blend_obj, state->blend_obj, sizeof mrt_blend.blend_obj);
    mrt_blend.state_seen = state->state_seen;
    mrt_blend.blend_seen = state->blend_seen;

    FD_VLOG("resolving render targets");
    for (uint32_t i = 0; i < state->rt_count && i < NVN_VK_MRT_MAX_COLOR; i++) {
        if (!state->rt[i].key) continue;
        const tex_t *rt_tex = (const tex_t *)state->rt[i].key;
        if (rt_tex->magic != 0x00584554u) {   // MAGIC('T','E','X',0)
            char msg[192];
            snprintf(msg, sizeof msg, "colour target slot %u is not a live NVNtexture object (magic 0x%x)",
                     i, rt_tex->magic);
            fail(why, why_size, msg);
            goto done;
        }
        char adapter_why[192] = "";
        struct nvn_vk_tex *t = s2a_vk_adapter_tex(rt_tex, adapter_why, sizeof adapter_why);
        if (!t || !t->view || !t->image) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "colour target slot %u texture has no view: tex=%p mem=%p target=%u samples=%d fmt=0x%x "
                     "rt=%p serial=%llu %ux%u valid=%d image=%p view=%p adapter=%s",
                     i, (void *)rt_tex, (void *)rt_tex->mem, rt_tex->d.target, rt_tex->d.samples,
                     rt_tex->d.format, (void *)t, t ? (unsigned long long)t->serial : 0ull,
                     t ? t->width : 0u, t ? t->height : 0u, t ? (int)t->valid : -1,
                     t ? (void *)t->image : NULL, t ? (void *)t->view : NULL,
                     adapter_why[0] ? adapter_why : "none");
            fail(why, why_size, msg);
            goto done;
        }
        const view_t *rv = state->rt[i].has_view ? &state->rt[i].view : NULL;
        VkFormat f = VK_FORMAT_UNDEFINED;
        VkImageView v = VK_NULL_HANDLE;
        int bcrt_slot = 0;
        uint32_t bcrt_level = (rv && rv->set_levels) ? rv->base_level : 0;
        uint32_t bcrt_layer = (rv && rv->set_layers) ? rv->min_layer : 0;
        if (t->vk_format && !nvn_vk_format_is_depth(t->vk_format) &&
            nvn_vk_format_is_compressed(t->vk_format) && rv && rv->set_format) {
            // 压缩纹理作为渲染目标：附件视图用“块=像素”的无压缩格式覆盖（TOTK 的 BC4 3D LUT）。
            struct nvn_vk_bcrt_plan bp;
            char bwhy[192] = "";
            const VkFormat vf = nvn_vk_format(rv->format);
            const int is3d = (t->nvn_target == NVN_TEXTURE_TARGET_3D && t->depth > 1);
            if (bcrt_level < (t->levels ? t->levels : 1u) &&
                nvn_vk_bcrt_plan_make(t->vk_format, vf, fd_mipdim(t->width, bcrt_level),
                                      fd_mipdim(t->height, bcrt_level), bcrt_level, bcrt_layer,
                                      (uint8_t)is3d, &bp, bwhy, sizeof bwhy) == 0) {
                if (d->physical) {
                    // 影子要能当颜色附件、能被 copy 两端用；底图要能拷出/拷回。缺一项就准确报红。
                    const VkFormatFeatureFlags bcaps = fd_format_features(d, t->vk_format);
                    const VkFormatFeatureFlags vcaps = fd_format_features(d, vf);
                    const VkFormatFeatureFlags need_view = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                           VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                                           VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                    const VkFormatFeatureFlags need_base = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                                           VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                    if ((vcaps & need_view) != need_view || (bcaps & need_base) != need_base) {
                        char msg[256];
                        snprintf(msg, sizeof msg,
                                 "block-encoding target %d/%d lacks required features: view 0x%x (need 0x%x), "
                                 "base 0x%x (need 0x%x)", (int)vf, (int)t->vk_format,
                                 (unsigned)vcaps, (unsigned)need_view, (unsigned)bcaps, (unsigned)need_base);
                        fail(why, why_size, msg);
                        goto done;
                    }
                }
                if (g_bcrt_n >= NVN_VK_MRT_MAX_COLOR) {
                    fail(why, why_size, "too many block-encoding render targets in one draw");
                    goto done;
                }
                struct fd_bcrt_slot *bs = fd_bcrt_acquire(d, vf, bp.bw, bp.bh, bp.block_bytes, why, why_size);
                if (!bs) goto done;
                f = vf;
                v = bs->view;
                bcrt_slot = 1;
                g_bcrt_pend[g_bcrt_n].plan = bp;
                g_bcrt_pend[g_bcrt_n].slot = bs;
                g_bcrt_pend[g_bcrt_n].tex = (const tex_t *)state->rt[i].key;
                g_bcrt_pend[g_bcrt_n].base = t->image;
                g_bcrt_n++;
            } else {
                static _Atomic int bcrt_why_log;
                if (atomic_fetch_add(&bcrt_why_log, 1) < 8)
                    FD_LOG("compressed colour target not encodable as blocks: %s", bwhy);
            }
        }
        if (!bcrt_slot) {
            v = s2a_vk_adapter_attachment_view(t, rv, &f, why, why_size);
            if (!v) goto done;
        }
        if (!nvn_vk_mrt_format_is_color(f)) {
            char msg[192];
            snprintf(msg, sizeof msg, "colour target slot %u format %d cannot be a colour attachment",
                     i, (int)f);
            fail(why, why_size, msg);
            goto done;
        }
        // 设备能力：格式必须在 optimal tiling 下支持颜色附件；不支持就准确报红，不换格式兜底。
        if (d->physical) {
            VkFormatFeatureFlags caps = fd_format_features(d, f);
            if (!(caps & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
                char msg[224];
                snprintf(msg, sizeof msg,
                         "colour target slot %u format %d is not supported as a colour attachment "
                         "(optimalTilingFeatures=0x%x)", i, (int)f, (unsigned)caps);
                fail(why, why_size, msg);
                goto done;
            }
        }
        // 同深度附件：视图一创建就保活，避免后续槽位的视图请求把它从同一底图的
        // 视图缓存里淘汰；末端的 pin 只覆盖“已经创建且还活着”的视图，来不及。
        if (fd_pin_tex(t) != 0) {
            fail(why, why_size, "texture pin list is full; the draw is not recorded (no unregistered attachment)");
            goto done;
        }
        if (!bcrt_slot && fd_pin_view(t, v) != 0) {
            fail(why, why_size, "attachment view is not registered for keeping; refusing this draw");
            goto done;
        }
        uint32_t level = (rv && rv->set_levels) ? rv->base_level : 0;
        uint32_t layer = (rv && rv->set_layers) ? rv->min_layer : 0;
        rt_texs[i] = t;
        rt_imgs[i] = bcrt_slot ? g_bcrt_pend[g_bcrt_n - 1].slot->img : t->image;
        rt_atts[i] = v;
        rt_formats[i] = f;
        rt_serials[i] = t->serial;
        mrt_slots[i].bound = 1;
        mrt_slots[i].format = f;
        // 块编码影子的”像素“是压缩块：mip 的纹素尺寸除以块边长 = 影子尺寸。NVN 的视口/scissor
        // 也在这个视图像素（块）空间里（TOTK：64x64 的 BC4 用 16x16 视口）。framebuffer/renderArea
        // 必须按影子尺寸建，按纹素尺寸建会让附件视图小于 framebuffer（非法 framebuffer）。
        mrt_slots[i].width = bcrt_slot ? g_bcrt_pend[g_bcrt_n - 1].plan.bw : fd_mipdim(t->width, level);
        mrt_slots[i].height = bcrt_slot ? g_bcrt_pend[g_bcrt_n - 1].plan.bh : fd_mipdim(t->height, level);
        mrt_slots[i].image_id = (uint64_t)(uintptr_t)rt_imgs[i];
        mrt_slots[i].level = level;
        mrt_slots[i].layer = layer;
        mrt_slots[i].write_mask = (chmask.mask >> (4u * i)) & 0xfu;
        if (g_prop_trace[0]) {
            static _Atomic int nrt_log;
            int n = atomic_fetch_add(&nrt_log, 1);
            if (n < 16384)
                FD_LOG("trace: pass-rt slot=%u serial=%llu %ux%u nvn_fmt=0x%x frame=%llu",
                       i, (unsigned long long)t->serial, mrt_slots[i].width, mrt_slots[i].height,
                       t->nvn_format, (unsigned long long)g_frame_seq);
        }
        FD_VLOG("colour slot %u: serial=%llu %ux%u nvn_fmt=0x%x vk_fmt=%d level=%u layer=%u",
                i, (unsigned long long)t->serial, mrt_slots[i].width, mrt_slots[i].height,
                t->nvn_format, (int)f, level, layer);
    }
    {
        char mrt_why[240] = "";
        uint32_t depth_level = (depth_tex && state->has_dview && state->dview.set_levels) ? state->dview.base_level : 0;
        uint32_t depth_w = depth_tex ? fd_mipdim(depth_tex->width, depth_level) : 0;
        uint32_t depth_h = depth_tex ? fd_mipdim(depth_tex->height, depth_level) : 0;
        if (no_attach) {
            // 零附件：尺寸取视口（renderArea 必须覆盖视口，光栅化才会产生片元给 FS 写 storage），
            // 并夹到设备 framebuffer 上限；附件表为空。
            uint32_t w = state->vp_seen && state->vp_w > 0.0f ? (uint32_t)(state->vp_w + 0.5f) : 1u;
            uint32_t h = state->vp_seen && state->vp_h > 0.0f ? (uint32_t)(state->vp_h + 0.5f) : 1u;
            if (w > d->properties.limits.maxFramebufferWidth) w = d->properties.limits.maxFramebufferWidth;
            if (h > d->properties.limits.maxFramebufferHeight) h = d->properties.limits.maxFramebufferHeight;
            memset(&mrt_plan, 0, sizeof mrt_plan);
            mrt_plan.color_count = 0;
            mrt_plan.bound_mask = 0;
            mrt_plan.attachment_count = 0;
            mrt_plan.depth_used = 0;
            mrt_plan.first_slot = UINT32_MAX;
            mrt_plan.width = w ? w : 1u;
            mrt_plan.height = h ? h : 1u;
            FD_VLOG("zero-attachment draw: framebuffer %ux%u (viewport=%s%.0fx%.0f scissor=%s%d,%d,%dx%d)", 
                    mrt_plan.width, mrt_plan.height, state->vp_seen ? "" : "unset->fallback ",
                    (double)state->vp_w, (double)state->vp_h, state->sc_seen ? "" : "unset->fallback ",
                    state->sc_x, state->sc_y, state->sc_w, state->sc_h);
            // 无附件 pass 的“画多少片元”只能从视口/裁剪推断（iOS 坑记录 10-21：按 1x1 会让
            // 只靠副作用的绘制只跑 1 个片元）。视口从未设置时这里只能按 1x1 兜底：记一次事实行，
            // 便于把“结果不对”对到具体 draw。
            if (!state->vp_seen) {
                static _Atomic int noted_vp;
                if (atomic_fetch_add(&noted_vp, 1) < 8)
                    FD_LOG("zero-attachment draw without a recorded viewport: framebuffer %ux%u is a fallback; "
                           "副作用范围无法从快照证明", mrt_plan.width, mrt_plan.height);
            }
        } else if (nvn_vk_mrt_plan_build(mrt_slots, state->rt_count, depth_tex != NULL, depth_w, depth_h,
                                         &mrt_plan, mrt_why, sizeof mrt_why) != 0) {
            char msg[320];
            snprintf(msg, sizeof msg, "multi-colour render target: %s", mrt_why);
            fail(why, why_size, msg);
            goto done;
        }
        depth_only = mrt_plan.color_count == 0;
        if (depth_only && depth_tex) {
            atomic_fetch_add(&g_depth_only_since_report, 1);
            if (fd_verbose() && depth_tex)
                FD_LOG("depth-only pass: rt_count=%u no colour slot bound; depth nvn_fmt=0x%x %ux%u (%s)",
                       state->rt_count, depth_tex->nvn_format, depth_tex->width, depth_tex->height,
                       request_id ? request_id : "?");
        }
        if (depth_tex && !mrt_plan.depth_used) {
            static _Atomic int n_mismatch;
            if (atomic_fetch_add(&n_mismatch, 1) < 32)
                FD_LOG("depth attachment %ux%u != colour %ux%u: dropping depth from this pass",
                       depth_w, depth_h, mrt_plan.width, mrt_plan.height);
            depth_tex = NULL;
            depth_att = VK_NULL_HANDLE;
        }
    }
    if (mrt_plan.first_slot != UINT32_MAX) {
        rt = rt_texs[mrt_plan.first_slot];
        rt_format = rt_formats[mrt_plan.first_slot];
    }
    // MRT 触发证据：真实路线到底有没有 >1 颜色槽、槽 1..n 是绑了纹理（MRT）还是空（合法空洞）。
    {
        static uint64_t hist1, hist2, hist3, hist4, bound_hi, hole, n;
        uint32_t rc = state->rt_count;
        int any_bound = 0, any_hole = 0;
        for (uint32_t i = 1; i < rc && i < 8; i++) {
            if (state->rt[i].key) any_bound = 1;
            else any_hole = 1;
        }
        if (rc <= 1) hist1++; else if (rc == 2) hist2++; else if (rc == 3) hist3++; else hist4++;
        if (any_bound) {
            if (!bound_hi)
                FD_LOG("first multi-colour render target draw: rt_count=%u bound slots above 0 (request=%s)",
                       rc, request_id ? request_id : "?");
            bound_hi++;
        }
        if (any_hole) hole++;
        if (++n % 16384 == 0)
            FD_LOG("draw rt histogram: count1=%llu count2=%llu count3=%llu count4+=%llu "
                   "slot>0-bound=%llu with-hole=%llu", (unsigned long long)hist1, (unsigned long long)hist2,
                   (unsigned long long)hist3, (unsigned long long)hist4, (unsigned long long)bound_hi,
                   (unsigned long long)hole);
    }
    if (rt && fd_log_rt()) {                 // 诊断：这颗 RT serial 被 GPU 写过
        static int n;
        if (n++ < 4000)
            FD_LOG("rt use serial=%llu %ux%u nvn_fmt=0x%x prog=%02x%02x%02x%02x depth_only=%d",
                   (unsigned long long)rt->serial, rt->width, rt->height, rt->nvn_format,
                   prog_sha[0], prog_sha[1], prog_sha[2], prog_sha[3], depth_only);
    }
    // 附件纹理/视图已在解析时逐个 pin（见上面的深度块和颜色槽循环）：这里不能再延迟到
    // 全部视图创建之后，否则同一底图的视图缓存会先把先前创建的附件视图当 LRU 淘汰掉。
    if (!depth_only && mrt_plan.first_slot == UINT32_MAX) {
        fail(why, why_size, "render target format is undefined");
        goto done;
    }
    if (!no_attach && depth_only && depth_fmt == VK_FORMAT_UNDEFINED) {
        fail(why, why_size, "depth target format is undefined");
        goto done;
    }
    const uint32_t fw = mrt_plan.width;
    const uint32_t fh = mrt_plan.height;
    const uint32_t color_attachment_count = mrt_plan.attachment_count - (mrt_plan.depth_used ? 1u : 0u);
    {
        char tdir[PROP_VALUE_MAX] = {0};   // 每帧第一笔时从缓存填充（见下），不再每笔读属性
        // 只抓“这个 traced draw 换了新渲染目标”的第一笔：载入屏会反复用同一个合成程序，
        // 世界首帧换到新的 RT/内容，这正是紫/绿要抓的那一笔。
        // 载入屏和世界首帧用的是同一个 RT serial（都是 1600x900 合成目标），只按 serial
        // 区分会漏掉世界首帧；本帧 draw 数能区分“几十笔的载入屏”和“数千笔的世界帧”。
        uint32_t frame_draws = atomic_load(&g_draws_this_frame);
        snprintf(tdir, sizeof tdir, "%s", g_tr_cfg.dir);   // 每 32 笔刷新一次的缓存（见 tr_want）
        int world_frame = frame_draws > 100;
        int ord = (rt && world_frame && tr_want()) ? tr_next(trace_id) : -1;
        if (ord >= 0) {
            uint32_t tbpp = vk_bpp(rt_format);
            VkDeviceSize want = tbpp ? (VkDeviceSize)rt->width * rt->height * tbpp * 2 : 0;
            if (!tbpp) FD_LOG("trace: rt format %d has no known bpp; not tracing", (int)rt_format);
            else if (want > (64ull << 20)) FD_LOG("trace: rt %ux%u needs %llu B; not tracing", rt->width, rt->height, (unsigned long long)want);
            else {
                VkBufferCreateInfo tbi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                tbi.size = want;
                tbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VkMemoryRequirements tmr;
                VkMemoryAllocateInfo tmi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                if (vkCreateBuffer(d->device, &tbi, NULL, &tr.buf) == VK_SUCCESS) {
                    vkGetBufferMemoryRequirements(d->device, tr.buf, &tmr);
                    tmi.allocationSize = tmr.size;
                    tmi.memoryTypeIndex = s2a_vk_memory_type(d, tmr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
                    if (vkAllocateMemory(d->device, &tmi, NULL, &tr.mem) == VK_SUCCESS &&
                        vkBindBufferMemory(d->device, tr.buf, tr.mem, 0) == VK_SUCCESS &&
                        vkMapMemory(d->device, tr.mem, 0, VK_WHOLE_SIZE, 0, &tr.map) == VK_SUCCESS) {
                        tr.on = 1; tr.cap = want; tr.bpp = tbpp; tr.ord = ord;
                        snprintf(tr.dir, sizeof tr.dir, "%s", tdir);
                        snprintf(tr.tag, sizeof tr.tag, "%s-%d", trace_id[0] ? trace_id : "?", ord);
                        FD_LOG("trace: on #%d request=%s rt=%ux%u rt_serial=%llu frame_draws=%u bpp=%u buf=%llu B", ord,
                               trace_id[0] ? trace_id : "?", rt->width, rt->height,
                               (unsigned long long)rt->serial, frame_draws, tbpp, (unsigned long long)want);
                    } else {
                        FD_LOG("trace: trace buffer allocation/map failed");
                    }
                }
            }
        }
    }
    // #1229 P0-9：首 8 笔诊断只在 draw_log 打开时执行（默认关）。客机内存读取
    //   边界由 nvn_vk_draw_diag.c 按记录范围收口；这里只做开关通道。
    if (fd_verbose()) s2a_first_draw_diag(state, prog.stage[0].attr_mask, request_id, fd_diag_log);
    // 程序级对象（模块/四 set 布局/pipeline layout）按内容 SHA 只创建一次；本次只借引用。
    struct fd_prog_obj *pobj = fd_prog_obj_get(d, &prog, prog_sha, why, why_size);
    if (!pobj) goto done;
    if (!pobj->has_prog) {
        struct fd_program owned;
        memset(&owned, 0, sizeof owned);
        if (fd_prog_cache_take(prog_sha, &owned)) fd_prog_obj_attach(pobj, &owned);
    }
    vs = pobj->vs;
    fs = pobj->fs;
    memcpy(set_layouts, pobj->sets, sizeof set_layouts);
    playout = pobj->playout;
    t_modules = fd_stamp();
    t_layouts = t_modules;

    FD_VLOG("program objects ready (created=%llu hits=%llu)",
            (unsigned long long)atomic_load(&g_prog_obj_created), (unsigned long long)atomic_load(&g_prog_obj_hits));
    // Render pass：颜色 + 可选深度；布局统一 GENERAL（与描述符声明、上传后布局一致，
    // 对齐参考实现 xbg_game 的 prepare_attachment/transition：附件 initial=final=GENERAL）。
    // 连续同一附件的 draw 复用 open pass；附件/清除状态变化才新建。
    // 深度/模板的清除掩码是两个独立平面：NVN 可以只清其中之一，不能用一个合并标志同时决定两个 loadOp。
    // ClearDepthStencil 现在按命令位置立即执行（vk_record OP_CLEAR_DS），所以这里通常都是 0（LOAD）。
    const uint32_t clear_mask = (state->depth_clear_mask ? 1u : 0u) | (state->stencil_clear_mask ? 2u : 0u);
    int pass_reuse = 0;
    int64_t t_fb0 = fd_stamp();
    // 块编码影子：一个 pass 只写一个子资源，影子内容每笔都要重拷 → 本笔必须独占 pass，不能与
    // 前/后一笔合并（否则影子会被多笔覆盖、拷回时机也错）。
    if (g_bcrt_n) {
        if (g_op.open && fd_pass_close(d, FD_PCR_ATTACH_SWITCH) != 0) {
            fail(why, why_size, "closing the previous pass for a block-encoding render target failed");
            goto done;
        }
        if (++g_bcrt_since_flush >= 32) {
            // 安全点：上一笔 pass 已收尾、本笔还没录任何命令。等单位完成后再开新 pass：
            // 影子按帧完成语义可复用，帧队列的批次完成证明表也不会被启动期几百笔撑爆。
            g_bcrt_since_flush = 0;
            if (nvn_vk_first_draw_flush() != 0) {
                fail(why, why_size, "flushing the frame queue for block-encoding render targets failed");
                goto done;
            }
        }
        merge_draw = 0;
        atomic_fetch_add(&g_bcrt_passes, 1);
    }
    if (no_attach) merge_draw = 0;   // 零附件 pass 没有可共享的附件，逐笔独占（也避免与前一 pass 混录）
    struct nvn_vk_tex *fb_tex = (merge_draw && fd_feedback_fix()) ? fd_feedback_tex(&prog, state, rt) : NULL;
    int64_t t_fb1 = fd_stamp();
    if (fd_dbg_fade(prog_sha)) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 8)
            FD_LOG("dbg fade: rt=%ux%u serial=%llu fb_tex=%p fix=%d", fw, fh,
                   (unsigned long long)(rt ? rt->serial : 0), (void *)fb_tex, fd_feedback_fix());
    }
    if (merge_draw && fd_pass_continues(rt_imgs, rt_atts, rt_formats, mrt_plan.color_count,
                                        depth_tex ? depth_tex->image : VK_NULL_HANDLE,
                                        depth_att, depth_fmt,
                                        fw, fh, depth_only, clear_mask)) {
        pass = g_op.pass; fb = g_op.fb; cmd = g_op.cmd; pass_reuse = 1;
    }
    const int64_t t_cont1 = fd_stamp();
    if (t_fb1 && t_cont1) {
        int64_t d_cont = t_cont1 - t_fb1;
        if (d_cont > 0) atomic_fetch_add(&g_ns_pd_cont, (uint64_t)d_cont);
    }
    if (t_fb0 && t_fb1) {
        int64_t d_fb = t_fb1 - t_fb0;
        if (d_fb > 0) atomic_fetch_add(&g_ns_pd_fb, (uint64_t)d_fb);
        atomic_fetch_add(&g_ns_pd_n, 1);   // 自己的分母：这些计数器可能在多次窗口之间都没被取走
    }
    if (fb_tex && pass_reuse) {
        // 同一附件连续 draw 的复用 pass 不能就地插拷贝：先把已录制的部分收尾，再开新 pass。
        if (g_op.open && fd_pass_close(d, FD_PCR_ATTACH_SAME) != 0) { fail(why, why_size, "反馈采样前关闭旧 pass 失败"); goto done; }
        pass_reuse = 0;
    }
    int64_t t_pc0 = fd_stamp();
    if (!pass_reuse) {
        if (g_op.open)   // 附件变化：先把旧 pass 收尾入队（被关闭的是 depth-only 单独计数）
            fd_pass_close(d, g_op.depth_only ? FD_PCR_DEPTH_ONLY : FD_PCR_ATTACH_SWITCH);
        int rp_queued = 0;
        struct fd_rp_key rp_key;
        memset(&rp_key, 0, sizeof rp_key);
        rp_key.color_count = mrt_plan.color_count;
        rp_key.bound_mask = mrt_plan.bound_mask;
        for (uint32_t i = 0; i < mrt_plan.color_count && i < NVN_VK_MRT_MAX_COLOR; i++)
            if (mrt_plan.bound_mask & (1u << i)) rp_key.color_fmt[i] = rt_formats[i];
        rp_key.depth_fmt = depth_tex ? depth_fmt : VK_FORMAT_UNDEFINED;
        rp_key.clear_mask = clear_mask;
        rp_key.has_depth = depth_tex != NULL;
        pass = fd_rp_get(d->device, &rp_key, &rp_queued, why, why_size);
        if (!pass) { goto done; }   // why 已由 fd_rp_get 写清
        pass_owned = rp_queued;     // 缓存命中的 pass 归缓存；只有缓存满时本次新建的那个归本笔
        t_pass_a = fd_stamp();      // pass 段细分：MRT 计划 + render pass 缓存查找到此为止
        // framebuffer 的附件表只放“参与”的槽（空槽/被丢弃的重复槽不声明），顺序与 pass 一致。
        VkImageView fb_color[NVN_VK_MRT_MAX_COLOR];
        uint64_t fb_serial[NVN_VK_MRT_MAX_COLOR];
        memset(fb_color, 0, sizeof fb_color);
        memset(fb_serial, 0, sizeof fb_serial);
        for (uint32_t i = 0; i < mrt_plan.color_count && i < NVN_VK_MRT_MAX_COLOR; i++) {
            if (!(mrt_plan.bound_mask & (1u << i))) continue;
            fb_color[i] = rt_atts[i];
            fb_serial[i] = rt_serials[i];
        }
        // 跨帧 framebuffer 缓存：同一 pass/逐槽视图/深度/尺寸组合不再每帧新建。
        // 只有全局 render pass 缓存里的 pass（rp_queued=0）才能进缓存；否则 pass 会先于 fb 被销毁。
        int fb_cached = 0;
        if (!rp_queued) {
            fb = fd_fb_acquire(d->device, pass, fb_color, fb_serial, mrt_plan.color_count,
                               depth_tex ? depth_att : VK_NULL_HANDLE, depth_tex ? depth_tex->serial : 0,
                               fw, fh, &fb_cached, why, why_size);
            if (!fb) goto done;
            fb_owned = !fb_cached;   // 缓存满时本次新建的 fb 归本笔/帧队列
            // 视图销毁时会由 nvn_vk_first_draw_forget_view 把缓存条目一起失效。
        } else {
            VkImageView fb_views[NVN_VK_MRT_MAX_COLOR + 1];
            uint32_t nviews = 0;
            for (uint32_t i = 0; i < mrt_plan.color_count && i < NVN_VK_MRT_MAX_COLOR; i++)
                if (fb_color[i]) fb_views[nviews++] = fb_color[i];
            if (depth_tex) fb_views[nviews++] = depth_att;
            VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fbi.renderPass = pass; fbi.attachmentCount = nviews; fbi.pAttachments = fb_views;
            fbi.width = fw; fbi.height = fh; fbi.layers = 1;
            int64_t t_fb = fd_now_ns();
            VkResult fbrc = vkCreateFramebuffer(d->device, &fbi, NULL, &fb);
            __atomic_fetch_add(&g_fb_create_ns, (uint64_t)(fd_now_ns() - t_fb), __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_fb_create_n, 1, __ATOMIC_RELAXED);
            if (fbrc != VK_SUCCESS) { fail(why, why_size, "vkCreateFramebuffer failed"); goto done; }
            fb_owned = 1;
        }
        if (merge_draw) {
            // framebuffer 归 open pass 与帧队列，flush 前不销毁；缓存里的 render pass 不归帧队列，
            // 只有缓存满时本次新建的那个才要按帧回收。容量满时刚建的对象还没被提交命令引用，可立即回收。
            if (rp_queued) {
                if (fd_fq_keep_pass(pass) != 0) {
                    vkDestroyRenderPass(d->device, pass, NULL);
                    pass = VK_NULL_HANDLE; pass_owned = 0;
                    fail(why, why_size, "frame queue capacity is full for this pass; draw not recorded");
                    goto done;
                }
                pass_owned = 0;
            }
            if (fb_owned) {
                if (fd_fq_keep_fb(fb) != 0) {
                    vkDestroyFramebuffer(d->device, fb, NULL);
                    fb = VK_NULL_HANDLE; fb_owned = 0;
                    fail(why, why_size, "frame queue capacity is full for this pass; draw not recorded");
                    goto done;
                }
                fb_owned = 0;
            }
            if (fb_tex) {
                const view_t *fv = (mrt_plan.first_slot != UINT32_MAX && state->rt[mrt_plan.first_slot].has_view)
                                       ? &state->rt[mrt_plan.first_slot].view : NULL;
                fb_level = mrt_slots[mrt_plan.first_slot].level;
                fb_layer = mrt_slots[mrt_plan.first_slot].layer;
                fb_is3d = (rt->nvn_target == NVN_TEXTURE_TARGET_3D && rt->depth > 1);
                fb_fmt = rt_format;
                fb_view = fd_feedback_snapshot(d, rt, rt_format, fw, fh,
                                               fb_level, fb_layer,
                                               (fv && fv->set_layers) ? fv->nlayers : 1,
                                               why, why_size);
                if (fd_dbg_fade(prog_sha)) {
                    static _Atomic int n;
                    if (atomic_fetch_add(&n, 1) < 8)
                        FD_LOG("dbg fade: snapshot view=%p (%s)", (void *)fb_view, fb_view ? "ok" : "failed");
                }
                if (!fb_view) {
                    // 保留 fd_feedback_snapshot 写回的具体原因：旧实现用通用文案直接覆盖，
                    // 日志只剩“快照创建失败”，无法区分格式/尺寸/分配/视图/登记哪一步。
                    char detail[192];
                    snprintf(detail, sizeof detail, "%s",
                             (why && why[0]) ? why : "snapshot returned no reason");
                    fail(why, why_size, "反馈采样快照创建失败");
                    size_t used = strlen(why);
                    if (used < why_size) snprintf(why + used, why_size - used, ": %s", detail);
                    goto done;
                }
                static _Atomic int fb_draw_log;
                if (atomic_fetch_add(&fb_draw_log, 1) < 32)
                    FD_LOG("feedback: request=%.16s samples its own %ux%u render target (tex serial=%llu); "
                           "binding a pre-pass snapshot instead of the attachment",
                           request_id ? request_id : "?", fw, fh, (unsigned long long)rt->serial);
            }
            t_pass_b = fd_stamp();  // framebuffer 取得（命中或新建）到此为止，剩下是真正开 pass
            int64_t t_open0 = fd_stamp();
            int fd_pb_rc = fd_pass_begin(d, pass, fb, rt_imgs, rt_atts, rt_formats,
                              mrt_plan.color_count, color_attachment_count,
                              depth_tex ? depth_tex->image : VK_NULL_HANDLE,
                              depth_att, depth_fmt, fw, fh,
                              depth_only, clear_mask, state->depth_clear, state->stencil_clear);
            { int64_t t_open1 = fd_stamp();
              if (t_open0 && t_open1) { atomic_fetch_add(&g_ns_open_call, (uint64_t)(t_open1 - t_open0)); atomic_fetch_add(&g_ns_open_call_n, 1); } }
            if (fd_pb_rc) {
                g_precopy_src = g_precopy_dst = VK_NULL_HANDLE;
                memset(&g_precopy_src_desc, 0, sizeof g_precopy_src_desc);
                fail(why, why_size, "open render pass begin failed");
                goto done;
            }
            g_precopy_src = g_precopy_dst = VK_NULL_HANDLE;   // fd_pass_begin 成功时已消费；失败/提前返回也不外溢
    memset(&g_precopy_src_desc, 0, sizeof g_precopy_src_desc);
            cmd = g_op.cmd;
        }
    }

    {
        int64_t t_pc1 = fd_stamp();
        if (t_fb1 && t_fb0) { atomic_fetch_add(&g_us_fbtex, (uint64_t)((t_fb1 - t_fb0) / 1000)); atomic_fetch_add(&g_fbtex_n, 1); }
        if (t_pc1 && t_pc0) { atomic_fetch_add(&g_us_pcfg, (uint64_t)((t_pc1 - t_pc0) / 1000)); atomic_fetch_add(&g_pcfg_n, 1); }
    }
    t_pass = fd_stamp();
    if (!pass_reuse && t_cont1 && t_pass) {
        int64_t d_open = t_pass - t_cont1;
        if (d_open > 0) { atomic_fetch_add(&g_ns_pd_open, (uint64_t)d_open); atomic_fetch_add(&g_ns_pd_open_n, 1); }
    }
    // pass 诊断：记录并入当前 open pass 的最近一笔 request，flush 时随 pass probe 打出，
    // 用来把“哪个 pass 开始偏色”映射回具体 request（不改变绘制）。
    if (merge_draw && g_op.open) {
        snprintf(g_op.last_req, sizeof g_op.last_req, "%s", request_id ? request_id : "?");
        if (g_gpu_cur >= 0) snprintf(GPU_META_CUR.req, sizeof GPU_META_CUR.req, "%s",
                                     request_id ? request_id : "?");
        snprintf(g_op.last_prog, sizeof g_op.last_prog, "%02x%02x%02x%02x",
                 prog_sha[0], prog_sha[1], prog_sha[2], prog_sha[3]);
    }
    FD_VLOG("render pass + framebuffer %s", pass_reuse ? "reused" : "created");
    // 逐笔身份（draw_log=1）：把 request/程序内容前缀/附件/绘制参数放到同一行，便于按画面阶段定位到
    // 具体 pass 和程序，再按需用 debug.s2a.trace=<前缀> 抓该笔的完整输入/输出。
    FD_VLOG("draw: request=%.16s prog=%02x%02x%02x%02x rt=%ux%u fmt=%d depth=%d rt_count=%u count=%u first=%u idx=%d "
            "vp=%.0f,%.0f %.0fx%.0f sc=%d,%d,%dx%d",
            request_id ? request_id : "?", prog_sha[0], prog_sha[1], prog_sha[2], prog_sha[3],
            fw, fh, (int)rt_format, depth_tex != NULL, state->rt_count, state->count, state->first, state->indexed,
            state->vp_x, state->vp_y, state->vp_w, state->vp_h,
            state->sc_x, state->sc_y, state->sc_w, state->sc_h);
    // 顶点输入：来自真实 vattr/stream（只取 attrMask 里用到的 location）。
    VkVertexInputBindingDescription vbind[16];
    VkVertexInputAttributeDescription vattr[16];
    uint32_t vbind_count = 0, vattr_count = 0;
    int stream_used[16] = {0};
    for (int loc = 0; loc < 32 && vattr_count < 16; loc++) {
        if (!(prog.stage[0].attr_mask & (1u << loc))) continue;
        uint32_t fmt = state->vattr[loc].format;
        VkFormat vf = nvn_vk_format(fmt);
        if (vf == VK_FORMAT_UNDEFINED) {
            char msg[192];
            snprintf(msg, sizeof msg,
                     "vertex attribute format 0x%x (location %d, stream %u, offset %u, stride %u) is not supported",
                     fmt, loc, state->vattr[loc].stream, state->vattr[loc].offset,
                     state->vattr[loc].stream < 16 ? state->vstream_stride[state->vattr[loc].stream] : 0u);
            fail(why, why_size, msg);
            goto done;
        }
        uint32_t stream = state->vattr[loc].stream;
        if (stream >= 16) { fail(why, why_size, "vertex attribute stream outside 0..15"); goto done; }
        vattr[vattr_count].location = (uint32_t)loc;
        vattr[vattr_count].binding = stream;
        vattr[vattr_count].format = vf;
        vattr[vattr_count].offset = state->vattr[loc].offset;
        vattr_count++;
        if (!stream_used[stream]) {
            stream_used[stream] = 1;
            vbind[vbind_count].binding = stream;
            vbind[vbind_count].stride = state->vstream_stride[stream];
            vbind[vbind_count].inputRate = state->vstream_divisor[stream] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            vbind_count++;
        }
    }
    // 顶点着色器可以不用任何顶点属性（全屏三角形等）：合法，管线就不带顶点输入。

    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = vbind_count; vi.pVertexBindingDescriptions = vbind;
    vi.vertexAttributeDescriptionCount = vattr_count; vi.pVertexAttributeDescriptions = vattr;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    // 真实绘制图元来自 Draw 快照（triangles/strip/fan 等）；本笔按 NVN prim 映射。
    switch (state->prim) {
        case 0x0: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 0x1: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case 0x3: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case 0x5: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case 0x6: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        default: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }
    // NVN/Metal 的裁剪空间 Y 向上，宿主框架缓冲 Y 向下：Metal 的视口是
    //   screenY = y + (1 - ndcY) * height / 2
    // Vulkan 用同号的 height 会得到 screenY = y + (ndcY + 1) * height / 2，整幅上下翻转，
    // 三角形绕序也跟着反，背面剔除会把 NVN 的正面全部剔掉。
    // 因此用 y + height、height 取负来对齐；不能只翻 frontFace，也不能关剔除。
    VkViewport viewport = { state->vp_x, state->vp_y + state->vp_h, state->vp_w, -state->vp_h,
                            state->vp_near, state->vp_far };
    // NVN 首绘前没有 SetViewport 时视口状态是未定义的；这里按“整张目标”处理（与 scissor 同一模型），
    // 但把真实记录值打出来，便于评审是否需要改成别的起点。
    FD_VLOG("viewport from snapshot: x=%.2f y=%.2f w=%.2f h=%.2f (Vk y=%.2f h=%.2f, NVN->Vulkan Y flip) near=%.3f far=%.3f scissor=%d,%d,%d,%d",
           state->vp_x, state->vp_y, state->vp_w, state->vp_h,
           viewport.y, viewport.height,
           state->vp_near, state->vp_far,
           state->sc_x, state->sc_y, state->sc_w, state->sc_h);
    if (!(viewport.width > 0.0f) || !(state->vp_h > 0.0f)) {
        viewport.x = 0.0f; viewport.y = (float)fh; viewport.width = (float)fw;
        viewport.height = -(float)fh;
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        FD_VLOG("viewport empty in the snapshot -> full render target (flipped Y)");
    }
    VkRect2D scissor = { { state->sc_x, state->sc_y },
                         { state->sc_w > 0 ? (uint32_t)state->sc_w : 0u,
                           state->sc_h > 0 ? (uint32_t)state->sc_h : 0u } };
    if (!state->sc_seen) {
        // 未设置 ≠ 游戏显式设置空裁剪：只有前者才回退到整张目标，并且日志必须标明这是替代值。
        scissor.offset = (VkOffset2D){0,0};
        scissor.extent = (VkExtent2D){fw, fh};
        FD_VLOG("scissor was never set in this command stream -> full target (fallback, not captured state)");
    } else if (scissor.extent.width == 0 || scissor.extent.height == 0) {
        FD_VLOG("scissor explicitly set to %dx%d -> empty (draw covers no pixels)", state->sc_w, state->sc_h);
    }
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn_state = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn_state.dynamicStateCount = 2; dyn_state.pDynamicStates = dyn;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.pViewports = &viewport; vp.scissorCount = 1; vp.pScissors = &scissor;
    // 真实 NVN 状态（未绑定的字段按 NVN SetDefaults 种子：混合关、掩码全开、无剔除、
    // FILL/CCW、深度写开 LESS——零值与默认值不同，见 nvn_vk_mrt.c 与 vk_record.c 的注释）。
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = poly.mode == 1 ? VK_POLYGON_MODE_LINE : poly.mode == 0 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    rs.cullMode = poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : poly.cull == 2 ? VK_CULL_MODE_BACK_BIT :
                  poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    rs.frontFace = poly.front == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = mst.samples == 2 ? VK_SAMPLE_COUNT_2_BIT : mst.samples == 4 ? VK_SAMPLE_COUNT_4_BIT :
                              mst.samples == 8 ? VK_SAMPLE_COUNT_8_BIT : VK_SAMPLE_COUNT_1_BIT;
    ms.alphaToCoverageEnable = mst.a2c ? VK_TRUE : VK_FALSE;
    VkPipelineColorBlendAttachmentState cba[NVN_VK_MRT_MAX_COLOR];
    memset(cba, 0, sizeof cba);
    for (uint32_t i = 0; i < mrt_plan.color_count && i < NVN_VK_MRT_MAX_COLOR; i++) {
        int bound = (mrt_plan.bound_mask >> i) & 1u;
        nvn_vk_mrt_color_blend(i, bound, rt_formats[i], &mrt_blend, &cba[i]);
        if (bound && (color.blend_en & (1u << i))) {
            if (g_gpu_cur >= 0 && cba[i].blendEnable) GPU_META_CUR.blended++;
            // 数学等价的不混合被关掉：与旧路径同一统计口径（写掩码全 0 的槽不计）。
            if (!cba[i].blendEnable && cba[i].colorWriteMask) {
                if (g_gpu_cur >= 0) GPU_META_CUR.blend_noop++;
                atomic_fetch_add(&g_blend_noop, 1);
            }
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = depth_only ? 0u : mrt_plan.color_count;
    cb.pAttachments = depth_only ? NULL : cba;
    ds_st dss_nvn = { .dt = 0, .dw = 1, .df = NVN_DEPTH_FUNC_LESS, .st = 0,
                      .sf_f = NVN_STENCIL_FUNC_ALWAYS, .sf_b = NVN_STENCIL_FUNC_ALWAYS,
                      .fail_f = NVN_STENCIL_OP_KEEP, .zfail_f = NVN_STENCIL_OP_KEEP,
                      .zpass_f = NVN_STENCIL_OP_KEEP, .fail_b = NVN_STENCIL_OP_KEEP,
                      .zfail_b = NVN_STENCIL_OP_KEEP, .zpass_b = NVN_STENCIL_OP_KEEP };
    if (state->state_seen & (1u << 5)) memcpy(&dss_nvn, state->state_obj[5], sizeof dss_nvn);
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    if (depth_tex) {
        dss.depthTestEnable = dss_nvn.dt ? VK_TRUE : VK_FALSE;
        dss.depthWriteEnable = dss_nvn.dw ? VK_TRUE : VK_FALSE;
        if (g_gpu_cur >= 0 && !dss_nvn.dt) GPU_META_CUR.zdepth_off++;
        dss.depthCompareOp = nvk_compare_op(dss_nvn.df);
        dss.minDepthBounds = 0.0f; dss.maxDepthBounds = 1.0f;
        dss.stencilTestEnable = dss_nvn.st ? VK_TRUE : VK_FALSE;
        dss.front.failOp = nvk_stencil_op(dss_nvn.fail_f);
        dss.front.depthFailOp = nvk_stencil_op(dss_nvn.zfail_f);
        dss.front.passOp = nvk_stencil_op(dss_nvn.zpass_f);
        dss.front.compareOp = nvk_compare_op(dss_nvn.sf_f);
        dss.front.compareMask = 0xffu; dss.front.writeMask = 0xffu; dss.front.reference = 0;
        dss.back.failOp = nvk_stencil_op(dss_nvn.fail_b);
        dss.back.depthFailOp = nvk_stencil_op(dss_nvn.zfail_b);
        dss.back.passOp = nvk_stencil_op(dss_nvn.zpass_b);
        dss.back.compareOp = nvk_compare_op(dss_nvn.sf_b);
        dss.back.compareMask = 0xffu; dss.back.writeMask = 0xffu; dss.back.reference = 0;
    }
    if (tr.on) {
        FD_LOG("trace: identity request=%s stages=0x%x vs_key=%02x%02x%02x%02x%02x%02x%02x%02x fs_key=%02x%02x%02x%02x%02x%02x%02x%02x",
               request_id ? request_id : "?", state->stages,
               state->keys[0][0],state->keys[0][1],state->keys[0][2],state->keys[0][3],
               state->keys[0][4],state->keys[0][5],state->keys[0][6],state->keys[0][7],
               state->keys[1][0],state->keys[1][1],state->keys[1][2],state->keys[1][3],
               state->keys[1][4],state->keys[1][5],state->keys[1][6],state->keys[1][7]);
        FD_LOG("trace: draw indexed=%d prim=%u count=%u first=%u baseVertex=%u instances=%u index_type=%u index_addr=%llx",
               state->indexed, state->prim, state->count, state->first, state->base_vertex,
               state->instances ? state->instances : 1, state->index_type,
               (unsigned long long)state->index_address);
        FD_LOG("trace: rt=%ux%u fmt=%d depth=%d depth_clear=%.3f/%d stencil_clear=%.3f/%d cleared=%d",
               fw, fh, (int)rt_format, state->has_depth, state->depth_clear,
               state->depth_clear_mask, state->stencil_clear, state->stencil_clear_mask, state->depth_cleared);
        FD_LOG("trace: viewport %s = (%.2f,%.2f %.2fx%.2f near=%.3f far=%.3f) scissor %s = (%d,%d %dx%d)",
               state->vp_seen ? "SET" : "UNSET->full target fallback",
               state->vp_x, state->vp_y, state->vp_w, state->vp_h, state->vp_near, state->vp_far,
               state->sc_seen ? "SET" : "UNSET->full target fallback",
               state->sc_x, state->sc_y, state->sc_w, state->sc_h);
        FD_LOG("trace: state_seen=0x%02x color(blend_en=0x%x) chmask=0x%x blend(src_rgb=%u dst_rgb=%u src_a=%u dst_a=%u eq_rgb=%u eq_a=%u) "
               "poly(cull=%u front=%u mode=%u) ms(samples=%u a2c=%u) ds(dt=%u dw=%u df=%u st=%u sf_f=%u sf_b=%u fail_f=%u zfail_f=%u zpass_f=%u fail_b=%u zfail_b=%u zpass_b=%u)",
               state->state_seen, color.blend_en, chmask.mask,
               blend.src_rgb, blend.dst_rgb, blend.src_a, blend.dst_a, blend.eq_rgb, blend.eq_a,
               poly.cull, poly.front, poly.mode, mst.samples, mst.a2c,
               dss_nvn.dt, dss_nvn.dw, dss_nvn.df, dss_nvn.st, dss_nvn.sf_f, dss_nvn.sf_b,
               dss_nvn.fail_f, dss_nvn.zfail_f, dss_nvn.zpass_f, dss_nvn.fail_b, dss_nvn.zfail_b, dss_nvn.zpass_b);
        for (int k = 0; k < 6; k++) {
            const uint8_t *o = state->state_obj[k];
            FD_LOG("trace: state_obj[%d] seen=%d %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                   k, (state->state_seen >> k) & 1,
                   o[0],o[1],o[2],o[3],o[4],o[5],o[6],o[7],o[8],o[9],o[10],o[11],
                   o[12],o[13],o[14],o[15],o[16],o[17],o[18],o[19],o[20],o[21],o[22],o[23]);
        }
        for (int loc = 0; loc < 32; loc++) {
            if (!(prog.stage[0].attr_mask & (1u << loc))) continue;
            uint32_t st = state->vattr[loc].stream;
            FD_LOG("trace: attr loc=%d nvn_fmt=%u offset=%u stream=%u stride=%u divisor=%u vbuf=%llx+%llu",
                   loc, state->vattr[loc].format, state->vattr[loc].offset, st,
                   state->vstream_stride[st], state->vstream_divisor[st],
                   (unsigned long long)state->vbuf_addr[st], (unsigned long long)state->vbuf_size[st]);
        }
        for (int st = 0; st < 2; st++)
            for (int u = 0; u < 16; u++)
                if (state->ubo[st][u].addr)
                    FD_LOG("trace: ubo_bound stage=%s nvn_slot=%d addr=%llx size=%llu last_update=%llu@%llx n=%u",
                           st ? "fs" : "vs", u, (unsigned long long)state->ubo[st][u].addr,
                           (unsigned long long)state->ubo[st][u].size,
                           (unsigned long long)state->last_ub_bytes[st][u],
                           (unsigned long long)state->last_ub_addr[st][u],
                           state->last_ub_count[st][u]);
        if (state->index_address && state->count) {
            uint32_t isz = state->index_type == 0 ? 1 : state->index_type == 1 ? 2 : 4;
            char line[512]; int off = 0;
            uint32_t n = state->count < 24 ? state->count : 24;
            for (uint32_t i = 0; i < n && off < (int)sizeof line - 16; i++) {
                const uint8_t *ip = (const uint8_t *)(uintptr_t)state->index_address + (size_t)i * isz;
                uint32_t v = 0;
                if (isz == 1) v = *ip; else if (isz == 2) { uint16_t vv; memcpy(&vv, ip, 2); v = vv; } else memcpy(&v, ip, 4);
                off += snprintf(line + off, sizeof line - (size_t)off, "%u,", v);
            }
            FD_LOG("trace: indices[0..%u] = %s", n, line);
        }
    }
    /* C4b：先把 worker 已产出的管线装回缓存——刚完成的结果能把下面的 miss 直接
       变成命中，也让被跳过的上一笔在结果就绪后无等待接上。 */
    if (fd_pipe_async_on()) fd_pipe_drain(d, prog_sha, program_input_sha);
    uint64_t pipe_key = 0;
    struct nvk_pipe_recipe recipe;
    memset(&recipe, 0, sizeof recipe);
    {
        uint32_t recipe_fmt[NVN_VK_MRT_MAX_COLOR];
        for (uint32_t i = 0; i < NVN_VK_MRT_MAX_COLOR; i++)
            recipe_fmt[i] = (i < mrt_plan.color_count && (mrt_plan.bound_mask & (1u << i)))
                                ? (uint32_t)rt_formats[i] : (uint32_t)VK_FORMAT_UNDEFINED;
        nvk_pipe_recipe_fill(&recipe, state, request_id, prog_sha, recipe_fmt,
                             mrt_plan.color_count, mrt_plan.bound_mask, (uint32_t)depth_fmt,
                             depth_tex != NULL, depth_only, (uint64_t)(uintptr_t)d->device);
        pipe_key = fd_pipe_key(&recipe);
        struct fd_pipe_slot *hit = NULL;
        {
            int32_t *ib = fd_pipe_index_slot(pipe_key);
            int32_t s = *ib;
            if (s >= 0 && (unsigned)s < FD_PIPE_CACHE && g_pipe_cache[s].used &&
                nvk_pipe_recipe_hit(g_pipe_cache[s].key, &g_pipe_cache[s].recipe, pipe_key, &recipe)) {
                hit = &g_pipe_cache[s];
            } else {
                // 索引未命中或已过期：回退线性扫（判据不变），找到后补索引。
                for (int i = 0; i < FD_PIPE_CACHE; i++)
                    if (g_pipe_cache[i].used &&
                        nvk_pipe_recipe_hit(g_pipe_cache[i].key, &g_pipe_cache[i].recipe, pipe_key, &recipe)) {
                        hit = &g_pipe_cache[i];
                        *ib = (int32_t)i;
                        break;
                    }
            }
        }
        if (hit) {
            // 完整配方含程序内容 SHA-256；命中即同一程序对象的管线，不销毁共享布局。
            if (hit->obj != pobj) { fail(why, why_size, "pipeline recipe hit a different program object"); goto done; }
            hit->last_use = ++g_pipe_clock;
            pipeline = hit->pipe;
            pipe_from_cache = 1;
            atomic_fetch_add(&g_pipe_hits, 1);
        }
    }
    if (!pipe_from_cache) {
        // 已知失败键：同一程序内容不重复建管线（避免每帧撞同一面墙），直接把确定原因传给上层。
        if (fd_pipe_known_failure(pobj->sha)) {
            fail(why, why_size, "pipeline interface mismatch (recorded for this program; not retried)");
            goto done;
        }
        /* C4b：异步创建（debug.s2a.pipe_async）。render_pass 必须活到 worker 产出被取走：
           只有全局 rp 缓存的 pass（pass_owned==0 即 rp_queued==0，且未走 open-pass 复用）
           满足；open pass/帧队列 pass 帧末销毁，不能进 worker——本笔直接回落同步。
           pobj->playout 由 pend 表 pin 保活。 */
        if (pass && !pass_owned && !pass_reuse && fd_pipe_async_ensure(d, why, why_size)) {
            struct nvk_pipe_job_spec spec;
            memset(&spec, 0, sizeof spec);
            spec.device_generation = (uint64_t)(uintptr_t)d->device;
            spec.device = d->device;
            spec.cache = fd_pipeline_cache(d);
            spec.layout = playout;
            spec.render_pass = pass;
            spec.subpass = 0;
            spec.recipe = recipe;
            spec.stage_count = 2;
            spec.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            spec.stages[0].spirv = (const uint32_t *)prog.stage[0].spirv;
            spec.stages[0].bytes = prog.stage[0].spirv_bytes;
            snprintf(spec.stages[0].entry, sizeof spec.stages[0].entry, "main");
            spec.stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            spec.stages[1].spirv = (const uint32_t *)prog.stage[1].spirv;
            spec.stages[1].bytes = prog.stage[1].spirv_bytes;
            snprintf(spec.stages[1].entry, sizeof spec.stages[1].entry, "main");
            spec.binding_count = vbind_count;
            memcpy(spec.bindings, vbind, vbind_count * sizeof vbind[0]);
            spec.attr_count = vattr_count;
            memcpy(spec.attrs, vattr, vattr_count * sizeof vattr[0]);
            spec.ia = ia;
            spec.vp = vp;
            spec.vp.pViewports = NULL; spec.vp.pScissors = NULL;   // 指针内容搬进定长字段
            spec.viewport = viewport;
            spec.scissor = scissor;
            spec.rs = rs;
            spec.ms = ms;
            spec.cb = cb;
            spec.blend_count = depth_only ? 0 : mrt_plan.color_count;
            if (spec.blend_count) memcpy(spec.blend, cba, spec.blend_count * sizeof cba[0]);
            spec.cb.pAttachments = NULL;
            spec.ds = dss;
            spec.has_depth = depth_tex != NULL;
            spec.dynamic_count = 2;
            spec.dynamic[0] = VK_DYNAMIC_STATE_VIEWPORT;
            spec.dynamic[1] = VK_DYNAMIC_STATE_SCISSOR;
            int arc = fd_pipe_async_get(d, state, &spec, pobj, pipe_key,
                                        program_input_sha, &pipeline, why, why_size);
            if (arc == 0) {
                pipe_from_cache = 1;   // worker 产出已装入缓存；模块/布局归程序对象表
                atomic_fetch_add(&g_pipe_misses, 1);
            } else if (arc == 2) {
                rc = 2;   // 有意跳过：按 C4a 口径记 OK（调用方分支），不标 rt_drawn
                goto done;
            } else if (arc < 0) {
                goto done;   // why 已由 fd_pipe_async_get 写清
            }
            /* arc == 1：回落下面的同步创建（队满/worker 未起/证据丢失/pend 表满）。 */
        }
    }
    if (!pipe_from_cache) {
        VkPipelineShaderStageCreateInfo stages[2];
        memset(stages, 0, sizeof stages);
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount = 2; gpi.pStages = stages;
        gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms; gpi.pColorBlendState = &cb;
        if (depth_tex) gpi.pDepthStencilState = &dss;
        gpi.pDynamicState = &dyn_state;
        gpi.layout = playout; gpi.renderPass = pass; gpi.subpass = 0;
        FD_VLOG("creating graphics pipeline (%u vb, %u attr)", vbind_count, vattr_count);
        int64_t t_pipe0 = fd_now_ns();
        /* async 开时 worker 与这里的创建共用同一 VkPipelineCache：两边都要在
           nvk_pipe_worker 的 g_cache_mu 下建（合同见 nvn_vk_pipe_worker.h）。 */
        const int pipe_cache_lock = fd_pipe_async_on() &&
            nvn_vk_pipe_worker_cache_lock && nvn_vk_pipe_worker_cache_unlock;
        if (pipe_cache_lock) nvn_vk_pipe_worker_cache_lock();
        VkResult pipe_rc = vkCreateGraphicsPipelines(d->device, fd_pipeline_cache(d), 1,
                                                   &gpi, NULL, &pipeline);
        if (pipe_cache_lock) nvn_vk_pipe_worker_cache_unlock();
        if (pipe_rc != VK_SUCCESS) {
            // 驱动只说“Bind group info mismatches the shader source for symbol fp_c1”，不告诉我们是哪一项。
            // 这里把我们实际交给驱动的接口原样打出来（set/binding/kind/stage），才能定位是类型、数量还是缺项。
            FD_LOG("【红】管线创建失败：program=%02x%02x%02x%02x… bindings=%d（layout 原样如下）",
                   pobj->sha[0], pobj->sha[1], pobj->sha[2], pobj->sha[3], prog.binding_count);
            for (int bi = 0; bi < prog.binding_count; bi++)
                FD_LOG("【红】  layout set=%d binding=%d kind=%d stage=%s", prog.bindings[bi].set,
                       prog.bindings[bi].binding, prog.bindings[bi].kind, prog.bindings[bi].stage);
            fd_pipe_note_failure(pobj->sha, prog.binding_count);
            fail(why, why_size, "vkCreateGraphicsPipelines failed"); goto done;
        }
        int64_t t_pipe1 = fd_now_ns();
        atomic_fetch_add(&g_draw_pipe_n, 1);
        atomic_fetch_add(&g_draw_pipe_ns, (uint64_t)(t_pipe1 - t_pipe0));
        nvm_manifest_note(&recipe, program_input_sha);   // 记录配方 + 程序缓存键（下次会话预热用）
        atomic_fetch_add(&g_pipe_misses, 1);
        int slot = -1;
        for (int i = 0; i < FD_PIPE_CACHE; i++) if (!g_pipe_cache[i].used) { slot = i; break; }
        if (slot < 0) {
            uint64_t oldest = ~0ull;
            for (int i = 0; i < FD_PIPE_CACHE; i++)
                if (g_pipe_cache[i].last_use < oldest) { oldest = g_pipe_cache[i].last_use; slot = i; }
            fd_pipe_slot_release(d, &g_pipe_cache[slot]);   // 只释放本槽；共享对象不在这里销毁
            atomic_fetch_add(&g_pipe_evicts, 1);
        }
        g_pipe_cache[slot].used = 1; g_pipe_cache[slot].key = pipe_key;
        *fd_pipe_index_slot(pipe_key) = slot;
        g_pipe_cache[slot].recipe = recipe;
        g_pipe_cache[slot].pipe = pipeline; g_pipe_cache[slot].obj = pobj;
        pobj->users++;
        g_pipe_cache[slot].last_use = ++g_pipe_clock;
        pipe_from_cache = 1;   // 管线归缓存所有；模块/布局归程序对象表
    }

    FD_VLOG("pipeline created");
    t_pipeline = fd_stamp();
    // SupportBuffer：只填本笔真实用到/已确定的字段；其余保持 0（未读取不是事实）。
    {
        void *map = NULL;
        if (fd_arena_alloc(d, prog.support_bytes, fd_ubo_align(d), &support_off, &support_buf, &map) != 0) { fail(why, why_size, "arena alloc failed for support buffer"); goto done; }
        support_size = prog.support_bytes;
        uint8_t *sb = (uint8_t *)map;
        support_map = sb;
        memset(sb, 0, prog.support_bytes);
        extern int s2a_nvn_depth_mode __attribute__((weak));
        uint32_t flags = 0;
        if (&s2a_nvn_depth_mode && s2a_nvn_depth_mode) flags |= 1u;                       // 位 0：深度 −1..1
        if (d->shader_draw_parameters_enabled) flags |= 2u;                                // 位 1：绘制参数取内建
        memcpy(sb + SB_S2A_FLAGS, &flags, sizeof flags);
        float point = 1.0f;
        memcpy(sb + SB_S2A_POINT, &point, sizeof point);
        for (uint32_t i = 0; i < SB_RENDER_SCALE_COUNT; i++) {
            float one = 1.0f;
            memcpy(sb + SB_RENDER_SCALE + i * 16, &one, sizeof one);        // 无分辨率倍率
            memcpy(sb + SB_RENDER_SCALE_Y + i * 16, &one, sizeof one);
        }
    }

    FD_VLOG("support buffer filled");
    t_support = fd_stamp();
    // 资源解析：逐条反射绑定对应真实 NVN 状态；缺一项就失败，不用占位资源。
    for (int i = 0; i < prog.binding_count; i++) {
        struct fd_binding *b = &prog.bindings[i];
        const int64_t r0 = fd_stamp();
        g_bind_n++;
        if (b->kind == 0) {
            if (b->is_support) { res_kind[i] = 3; continue; }   // SupportBuffer（加载时已判定）
            int stage = b->stage_idx;
            if (stage < 0) { fail(why, why_size, "uniform binding has no stage"); goto done; }
            int synth_c0 = 0;
            // shader cbuf：c0 驱动内建、c1 常量段、c2 纹理句柄表、c3 起是 NVN 用户 UBO（槽 = cbuf − 3）。
            uint64_t addr = 0, size = 0;
            if (b->slot == 1) {
                if (!state->c1_ok[stage]) {
                    char msg[160];
                    snprintf(msg, sizeof msg, "c1 constant segment for stage %s was not captured", b->stage);
                    fail(why, why_size, msg);
                    goto done;
                }
                addr = (uint64_t)(uintptr_t)state->c1[stage];
                size = 0x100;
                FD_VLOG("bind[%s] set=%d binding=%d reflected_slot=1 -> c1 constant segment (using %llu bytes)",
                       b->stage, b->set, b->binding, (unsigned long long)size);
            } else if (b->slot == 2) {
                fail(why, why_size, "c2 texture-handle bank used as a uniform buffer is not implemented yet");
                goto done;
            } else if (b->slot >= 3) {
                int nvn_index = b->slot - 3;
                if (nvn_index >= 16) { fail(why, why_size, "user UBO slot outside 0..15"); goto done; }
                addr = state->ubo[stage][nvn_index].addr;
                size = state->ubo[stage][nvn_index].size;
                if (!addr || !size) {
                    char msg[160];
                    snprintf(msg, sizeof msg, "guest UBO for cbuf %d (NVN slot %d) is not bound", b->slot, nvn_index);
                    fail(why, why_size, msg);
                    goto done;
                }
            } else if (b->slot == 0) {
                // NVN 驱动常量段 c0：shader 从它读存储缓冲描述符表（Maxwell 的 SB descriptor 在常量
                // 空间），以及最大点径 / base instance。内容按已测量布局合成，不来自 guest。
                if (stage > 4) {
                    char msg[160];
                    snprintf(msg, sizeof msg, "driver c0 for graphics stage %d is not supported", stage);
                    fail(why, why_size, msg);
                    goto done;
                }
                size = NVN_VK_DRIVER_C0_BYTES;
                synth_c0 = 1;
                FD_VLOG("bind[%s] set=%d binding=%d reflected_slot=0 -> synthesized driver c0 (%llu B)",
                       b->stage, b->set, b->binding, (unsigned long long)size);
            } else {
                char msg[128];
                snprintf(msg, sizeof msg, "uniform cbuf slot %d is not a user UBO", b->slot);
                fail(why, why_size, msg);
                goto done;
            }
            void *map = NULL;
            uint64_t roff = 0;
            g_cp_ubo += size; g_cp_ubo_n++;
            if (fd_arena_alloc(d, size, fd_ubo_align(d), &roff, &res_buf[i], &map) != 0) { fail(why, why_size, "arena alloc failed for guest UBO"); goto done; }
            res_off[i] = roff; res_size[i] = size;
            if (synth_c0) {
                uint64_t saddrs[16];
                uint32_t ssz[16];
                for (int r = 0; r < 16; r++) {
                    saddrs[r] = state->ssbo[stage][r].addr;
                    ssz[r] = (uint32_t)state->ssbo[stage][r].size;
                }
                if (nvn_vk_driver_c0_fill_graphics((uint8_t *)map, (size_t)size, (uint32_t)stage, saddrs, ssz,
                                                   (uint32_t)state->base_instance, why, why_size) != 0)
                    goto done;
            } else {
                memcpy(map, (const void *)(uintptr_t)addr, (size_t)size);
            }
            uint32_t first = 0, at1900 = 0;
            float m5[4] = {0};
            if (size >= 4) memcpy(&first, map, 4);
            if (size >= 118 * 16 + 16) memcpy(&at1900, (const uint8_t *)map + 118 * 16 + 12, 4);
            if (size >= 6 * 16) memcpy(m5, (const uint8_t *)map + 5 * 16, sizeof m5);
            res_kind[i] = 1;
            if (tr.on) {
                char nm[64], what[96];
                snprintf(nm, sizeof nm, "ubo-%s-c%d.bin", b->stage, b->slot);
                snprintf(what, sizeof what, "ubo stage=%s cbuf=%d nvn_slot=%d", b->stage, b->slot, b->slot - 3);
                tr_file(&tr, nm, (const void *)(uintptr_t)addr, (size_t)size);
                tr_sha(what, (const void *)(uintptr_t)addr, (size_t)size);
            }
            {
                int us = b->slot >= 3 ? b->slot - 3 : 0;
                uint64_t ua = state->last_ub_addr[stage][us], ub = state->last_ub_bytes[stage][us];
                // 无符号溢出安全：用减法比较区间端点，而不是 addr+size。
                int update_within = ua >= addr && ua - addr <= size && ub <= size - (ua - addr);
                int exact = ua == addr && ub == size;
                int binding_within_update = ua <= addr && addr - ua <= ub && size <= ub - (addr - ua);
                FD_VLOG("bind[%s] set=%d binding=%d reflected_slot=%d -> nvn_ubo_slot=%d addr=%llx size=%llu offset=0 "
                       "first4=%08x m5=(%.4f,%.4f,%.4f,%.4f) m118.w=%08x buf=%p range=%llu "
                       "last_ub=(%llu@%llx,n=%u) update_within_binding=%d exact_full_cover=%d binding_within_update=%d",
                       b->stage, b->set, b->binding, b->slot,
                       b->slot == 1 ? -1 : b->slot - 3,
                       (unsigned long long)addr, (unsigned long long)size, first,
                       m5[0], m5[1], m5[2], m5[3], at1900,
                       (void *)res_buf[i], (unsigned long long)size,
                       (unsigned long long)ub, (unsigned long long)ua,
                       state->last_ub_count[stage][us],
                       update_within, exact, binding_within_update);
            }
            g_bind_ub_n++;
            // 关诊断时 fd_stamp() 返回 0：连原子加法一起跳过（这是逐绑定的热路径，
            // 每笔 draw 有十几到几十个绑定，白加一遍原子是实打实的开销）。
            { int64_t t1 = fd_stamp(); if (r0 && t1) { atomic_fetch_add(&g_res_ub_ns, (uint64_t)(t1 - r0)); atomic_fetch_add(&g_res_ub_c, 1); } }
        } else if (b->kind == 1) {
            g_bind_ssbo_n++;
            int stage = b->stage_idx;
            if (stage < 0 || stage > 4) { fail(why, why_size, "storage binding has no NVN graphics stage"); goto done; }
            uint64_t addr = 0, size = 0;
            if (b->storage_kind == 1) {
                int slot = b->storage_slot;
                if (slot < 0 || slot >= 16) {
                    char msg[160];
                    snprintf(msg, sizeof msg, "NVN storage slot %d for stage %s is outside 0..15", slot, b->stage);
                    fail(why, why_size, msg);
                    goto done;
                }
                if (!(state->ssbo_bound[stage] & ((uint64_t)1 << slot))) {
                    // NVN 允许槽留空/绑 NULL（游戏用 NULL 表示“本槽不给”）。硬件与参考实现给空槽绑
                    // 零缓冲：读得 0、写被丢弃。不能当成缺资源报红，也不能借相邻槽的缓冲。
                    VkBuffer dbuf = VK_NULL_HANDLE;
                    uint64_t dsize = 0;
                    if (fd_dummy_ssbo(d, &dbuf, &dsize, why, why_size) != 0) goto done;
                    static _Atomic int noted_unbound;
                    if (atomic_fetch_add(&noted_unbound, 1) < 32)
                        FD_LOG("unbound NVN storage slot %d (stage %s) -> zero dummy buffer", slot, b->stage);
                    res_buf[i] = dbuf; res_off[i] = 0; res_size[i] = dsize; res_kind[i] = 4;
                    continue;
                }
                addr = state->ssbo[stage][slot].addr;
                size = state->ssbo[stage][slot].size;
                FD_VLOG("bind[%s] set=%d binding=%d storage -> nvn_ssbo_slot=%d addr=%llx size=%llu",
                       b->stage, b->set, b->binding, slot, (unsigned long long)addr, (unsigned long long)size);
            } else if (b->storage_kind == 2 || (b->storage_kind == 0 && b->storage_cb_slot == 1)) {
                // SbDescriptor: addressLow, addressHigh, size, padding (16 B).
                struct { uint32_t lo, hi; int32_t size; uint32_t pad; } desc;
                if (b->storage_kind == 2) {
                    int slot = b->storage_slot;
                    uint64_t base = (slot >= 0 && slot < 16) ? state->ubo[stage][slot].addr : 0;
                    uint64_t ubo_size = (slot >= 0 && slot < 16) ? state->ubo[stage][slot].size : 0;
                    if (!base || !ubo_size || b->storage_cb_offset < 0 ||
                        (uint64_t)b->storage_cb_offset > ubo_size ||
                        ubo_size - (uint64_t)b->storage_cb_offset < sizeof desc) {
                        char msg[192];
                        snprintf(msg, sizeof msg,
                                 "graphics storage SbDescriptor is outside the bound user UBO (slot=%d off=0x%x)",
                                 slot, b->storage_cb_offset);
                        fail(why, why_size, msg);
                        goto done;
                    }
                    memcpy(&desc, (const void *)(uintptr_t)(base + (uint64_t)b->storage_cb_offset), sizeof desc);
                } else {
                    if (!state->c1_ok[stage] || b->storage_cb_offset < 0 ||
                        (uint64_t)b->storage_cb_offset > sizeof state->c1[stage] ||
                        sizeof state->c1[stage] - (uint64_t)b->storage_cb_offset < sizeof desc) {
                        fail(why, why_size, "graphics storage SbDescriptor is outside the captured c1 segment");
                        goto done;
                    }
                    memcpy(&desc, state->c1[stage] + b->storage_cb_offset, sizeof desc);
                }
                addr = (uint64_t)desc.lo | ((uint64_t)desc.hi << 32);
                size = (uint32_t)desc.size;
                // 客体 SbDescriptor 里存的是 GPU VA；arena 拷贝要用池的 CPU 地址。
                if (nvn_ga2ca) addr = nvn_ga2ca(addr);
                FD_VLOG("bind[%s] set=%d binding=%d storage -> cb=%d+0x%x addr=%llx size=%llu",
                       b->stage, b->set, b->binding, b->storage_cb_slot, b->storage_cb_offset,
                       (unsigned long long)addr, (unsigned long long)size);
            } else if (b->storage_kind == 3) {
                fail(why, why_size,
                     "graphics storage buffer uses an indirect UBO pointer; extent is not available");
                goto done;
            } else {
                char msg[192];
                snprintf(msg, sizeof msg,
                         "graphics storage buffer has no NVN/UBO mapping (cbSlot=%d cbOffset=0x%x)",
                         b->storage_cb_slot, b->storage_cb_offset);
                fail(why, why_size, msg);
                goto done;
            }
            if (!addr) { fail(why, why_size, "graphics storage buffer address is 0"); goto done; }
            int unbounded_size = 0;
            if (!size) {
                // NVN 的 storage 绑定允许 size=0（未声明长度/无边界）。用 guest 池的可证明区间
                // （池尾 − 地址）并夹到 64 KiB；证明不了就准确失败。可写绑定需要整段回写 guest，
                // 窗口放大会把相邻分配一起写回（静默），这里先准确拒绝可写无界绑定。
                int exact = 0;
                size = nvn_vk_guest_buffer_extent(addr, 4u << 20, &exact);
                if (b->storage_writable && !exact) {
                    fail(why, why_size,
                         "writable graphics storage buffer has size 0 and only a pool-tail extent (not the buffer); "
                         "whole-buffer writeback would overwrite neighbouring guest memory");
                    goto done;
                }
                if (!exact && size > (64u << 10)) size = 64u << 10;
                unbounded_size = 1;
                if (!size) {
                    char msg[200];
                    snprintf(msg, sizeof msg,
                             "graphics storage buffer has size 0 and no provable guest pool extent (addr=0x%llx)",
                             (unsigned long long)addr);
                    fail(why, why_size, msg);
                    goto done;
                }
                static _Atomic int noted_zero;
                if (atomic_fetch_add(&noted_zero, 1) < 8)
                    FD_LOG("graphics storage buffer size=0: using %s extent %llu B",
                           exact ? "exact buffer" : "pool-tail (approximate)", (unsigned long long)size);
            }
            uint64_t align = d->properties.limits.minStorageBufferOffsetAlignment;
            if (!align || (align & (align - 1))) {
                fail(why, why_size, "device minStorageBufferOffsetAlignment is not a usable power of two");
                goto done;
            }
            if (size > d->properties.limits.maxStorageBufferRange) {
                fail(why, why_size, "graphics storage buffer exceeds maxStorageBufferRange");
                goto done;
            }
            uint64_t prefix = addr & (align - 1);
            if (prefix > ~0ull - size) { fail(why, why_size, "graphics storage buffer range overflows"); goto done; }
            uint64_t host_size = size + prefix;
            uint64_t roff = 0;
            uint64_t arena_align = align > 64 ? align : 64;
            if (b->storage_writable) {
                // Vulkan 要求 storage buffer 绑定偏移对齐；客体地址不对齐时无法既绑定又不偏移，
                // 且回写语义会含糊——准确报红，不做错误对齐。
                if (prefix) {
                    char msg[192];
                    snprintf(msg, sizeof msg,
                             "writable storage buffer base 0x%llx is not aligned to minStorageBufferOffsetAlignment %llu",
                             (unsigned long long)addr, (unsigned long long)align);
                    fail(why, why_size, msg);
                    goto done;
                }
                uint32_t hit_delta = 0;
                struct fd_ssbo_wb *hit = fd_ssbo_wb_find(addr, (uint32_t)size, &hit_delta);
                if (hit == (struct fd_ssbo_wb *)-1) {
                    char msg[220];
                    snprintf(msg, sizeof msg,
                             "writable storage buffer [0x%llx,+%llu) partially overlaps another frame shadow; "
                             "a second copy would fork GPU writes", (unsigned long long)addr,
                             (unsigned long long)size);
                    fail(why, why_size, msg);
                    goto done;
                }
                if (hit) {
                    if (fd_ssbo_wb_mark((uint32_t)size, hit_delta, hit) != 0) {
                        fail(why, why_size, "too many writable sub-ranges on one storage-buffer shadow in a frame");
                        goto done;
                    }
                    res_buf[i] = hit->buf;
                    res_off[i] = hit->off + hit_delta;
                    res_size[i] = size; res_kind[i] = 4;
                } else {
                    void *wmap = NULL;
                    if (fd_arena_alloc(d, size, arena_align, &roff, &res_buf[i], &wmap) != 0) {
                        fail(why, why_size, "arena alloc failed for writable guest storage buffer");
                        goto done;
                    }
                    memcpy(wmap, (const void *)(uintptr_t)addr, (size_t)size);
                    res_off[i] = roff; res_size[i] = size; res_kind[i] = 4;
                    if (fd_ssbo_wb_add(res_buf[i], wmap, addr, (uint32_t)roff, (uint32_t)size, 1) != 0) {
                        fail(why, why_size, "too many writable storage buffer shadows in one frame");
                        goto done;
                    }
                }
                {   // 诊断：可写 storage buffer 走影子（限量打，避免刷屏）
                    static _Atomic int n;
                    if (atomic_fetch_add(&n, 1) < 16)
                        FD_LOG("writable storage buffer: guest=0x%llx size=%llu -> frame shadow%s",
                               (unsigned long long)addr, (unsigned long long)size, hit ? " (contained)" : "");
                }
                res_ssbo_wr[i] = 1;
            } else {
                // 同帧已经有可写影子时，只读绑定也复用它（否则会拿 guest 旧字节新建一份）。
                uint32_t hit_delta = 0;
                struct fd_ssbo_wb *hit = (prefix == 0) ? fd_ssbo_wb_find(addr, (uint32_t)size, &hit_delta) : NULL;
                if (hit == (struct fd_ssbo_wb *)-1) {
                    char msg[220];
                    snprintf(msg, sizeof msg,
                             "storage buffer [0x%llx,+%llu) partially overlaps another frame shadow; "
                             "a second copy would not see GPU writes", (unsigned long long)addr,
                             (unsigned long long)size);
                    fail(why, why_size, msg);
                    goto done;
                }
                if (hit) {
                    res_buf[i] = hit->buf; res_off[i] = hit->off + hit_delta;
                    res_size[i] = size; res_kind[i] = 4;
                } else {
                    void *rmap = NULL;
                    if (fd_arena_alloc(d, host_size, arena_align, &roff, &res_buf[i], &rmap) != 0) {
                        fail(why, why_size, "arena alloc failed for guest storage buffer");
                        goto done;
                    }
                    memcpy(rmap, (const void *)(uintptr_t)(addr - prefix), (size_t)host_size);
                    res_off[i] = roff; res_size[i] = host_size; res_kind[i] = 4;
                    (void)unbounded_size;   // 无界只读：一次性影子，不进帧内复用账本
                }
            }
            if (tr.on) {
                char nm[64], what[96];
                snprintf(nm, sizeof nm, "ssbo-%s-b%d.bin", b->stage, b->binding);
                snprintf(what, sizeof what, "ssbo stage=%s binding=%d cbSlot=%d cbOffset=0x%x",
                         b->stage, b->binding, b->storage_cb_slot, b->storage_cb_offset);
                tr_file(&tr, nm, (const void *)(uintptr_t)(addr - prefix), (size_t)host_size);
                tr_sha(what, (const void *)(uintptr_t)(addr - prefix), (size_t)host_size);
            }
            { int64_t t1 = fd_stamp(); if (r0 && t1) atomic_fetch_add(&g_res_ssbo_ns, (uint64_t)(t1 - r0)); }
        } else if (b->kind == 2) {
            g_bind_tex_n++;

            int stage = nvn_vk_program_graphics_stage_index(b->stage);
            if (b->slot < 0) { fail(why, why_size, "texture binding has no reflected handle slot"); goto done; }
            // binding.slot = HandleIndex（c2 word offset）；组合纹理表 [0x20,0x168) 每 8 字节一槽。
            int byte_off = b->slot * 4;
            int slot = -1;
            if (byte_off >= 0x20 && byte_off < 0x120) slot = (byte_off - 0x20) / 8;
            else if (byte_off >= 0x168 && byte_off < 0x568) slot = 128 + (byte_off - 0x168) / 8;
            if (stage < 0 || slot < 0 || slot >= 32) {
                char msg[160];
                snprintf(msg, sizeof msg, "texture handleIndex %d (byte 0x%x) is not a supported combined slot",
                         b->slot, byte_off);
                fail(why, why_size, msg);
                goto done;
            }
            if (!state->tex_present[stage][slot]) {
                char msg[160];
                snprintf(msg, sizeof msg, "NVN texture slot %d (handleIndex %d) is not bound", slot, b->slot);
                fail(why, why_size, msg);
                goto done;
            }
            uint64_t handle = state->tex_handle[stage][slot];
            uint32_t tid = (uint32_t)(handle & 0xfffffu), sid = (uint32_t)(handle >> 20);
            struct nvn_vk_tex *tex = NULL;
            unsigned tex_slot = (unsigned)((tid * 2654435761u) % FD_TEX_LOOKUP);
            if (g_tex_lookup[tex_slot].tex && g_tex_lookup[tex_slot].id == (int)tid) {
                tex = g_tex_lookup[tex_slot].tex;
            } else {
                char inner[128] = "";
                if (s2a_vk_adapter_tex_by_id((int)tid, &tex, inner, sizeof inner)) {
                    char msg[224];
                    snprintf(msg, sizeof msg, "texture id %u (handle 0x%llx) did not resolve: %s",
                             tid, (unsigned long long)handle, inner);
                    fail(why, why_size, msg);
                    goto done;
                }
                g_tex_lookup[tex_slot].id = (int)tid;
                g_tex_lookup[tex_slot].tex = tex;
            }
            samp_par par;
            unsigned samp_slot = (unsigned)((sid * 2654435761u) % FD_SAMP_LOOKUP);
            if (g_samp_lookup[samp_slot].id == (int)sid) {
                par = g_samp_lookup[samp_slot].par;
            } else if (s2a_vk_adapter_samp_by_id((int)sid, &par)) {
                char msg[160];
                snprintf(msg, sizeof msg, "sampler id %u (handle 0x%llx) did not resolve", sid, (unsigned long long)handle);
                fail(why, why_size, msg);
                goto done;
            } else {
                g_samp_lookup[samp_slot].id = (int)sid;
                g_samp_lookup[samp_slot].par = par;
            }
            // 注册的 NVNtextureView 必须被真实消费：mip/层范围、格式覆盖、target 类型、通道映射、
            // 深度/模板面都按视图取精确 VkImageView；没有覆盖字段时走默认整幅视图。
            extern int s2a_vk_adapter_tex_view_by_id(int id, view_t *out) __attribute__((weak));
            extern VkImageView s2a_vk_adapter_tex_view(struct nvn_vk_tex *t, const view_t *v,
                                                      char *why, size_t why_size) __attribute__((weak));
            view_t rv;
            memset(&rv, 0, sizeof rv);
            const int64_t tv0 = fd_stamp();
            int hv = s2a_vk_adapter_tex_view_by_id ? s2a_vk_adapter_tex_view_by_id((int)tid, &rv) : -1;
            { int64_t d = fd_stamp() - tv0; if (d > 0) atomic_fetch_add(&g_res_view_ns, (uint64_t)d); }
            // S2ATexUnnormBit: binding groups of 128, first 32 in Y/Z/W.
            // The registered view target overrides the texture target. Buffer textures
            // have a different descriptor and do not participate in this contract.
            int group = b->binding / 128, bit = b->binding % 128;
            unsigned target = hv == 1 && rv.set_target ? rv.target : tex->nvn_target;
            if (support_map && b->binding >= 0 && group < 3 && bit < 32 &&
                tex->nvn_target != NVN_TEXTURE_TARGET_BUFFER && target != NVN_TEXTURE_TARGET_BUFFER) {
                uint32_t mask = 1u << bit;
                if (target == NVN_TEXTURE_TARGET_RECTANGLE) tex_unnorm_bits[group] |= mask;
                else tex_unnorm_bits[group] &= ~mask;
            }
            const int view_override = hv == 1 && (rv.set_format || rv.set_levels || rv.set_layers ||
                                                 rv.set_target || rv.set_dsmode);
            const int view_cube = view_override && rv.set_target && (rv.target == 8 || rv.target == 9);
            const int cube_view = view_cube || tex->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                                  tex->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            {
                const int64_t ts0 = fd_stamp();
                res_sampler[i] = nvn_vk_sampler_cache_get_for_view(&g_samp_cache, &par, cube_view, why, why_size);
                { int64_t d = fd_stamp() - ts0; if (d > 0) atomic_fetch_add(&g_res_samp_ns, (uint64_t)d); }
            }
            if (!res_sampler[i]) goto done;
            res_view[i] = tex->view;
            if (view_override) {
                VkImageView vv = s2a_vk_adapter_tex_view
                    ? s2a_vk_adapter_tex_view(tex, &rv, why, why_size) : VK_NULL_HANDLE;
                if (!vv) goto done;
                res_view[i] = vv;
            }
            if (fd_dbg_fade(prog_sha) && rt && tex->image == rt->image) {
                static _Atomic int n;
                if (atomic_fetch_add(&n, 1) < 8)
                    FD_LOG("dbg fade bind: i=%d tid=%u view_override=%d fb_view=%p", i, tid, view_override, (void *)fb_view);
            }
            if (fb_view && tex->image == rt->image) {
                // 单层 2D 快照只能代表“采样视图正好是附件那个子资源”；同图其它 mip/层不是反馈，
                // 默认 3D/多层视图表达不了（保持原绑定并记一次，与苹果路线 ④ 同口径）。
                struct nvn_vk_feedback_bind fbp;
                memset(&fbp, 0, sizeof fbp);
                fbp.fb_is_3d = (uint8_t)fb_is3d;
                fbp.fb_level = fb_level; fbp.fb_layer = fb_layer;
                fbp.tex_levels = tex->levels; fbp.tex_layers = tex->layers; fbp.tex_depth = tex->depth;
                fbp.view_override = (uint8_t)view_override;
                if (view_override) {
                    struct nvn_vk_texview_plan vp;
                    memset(&vp, 0, sizeof vp);
                    nvn_vk_texview_plan_make(tex->nvn_target, tex->depth, tex->layers, tex->levels, &rv,
                                             0, d->has_2d_array_compatible, &vp);
                    fbp.view_ok = (uint8_t)vp.ok;
                    fbp.view_type = (uint32_t)vp.type;
                    fbp.view_base_level = vp.base_level; fbp.view_nlevels = vp.nlevels;
                    fbp.view_base_layer = vp.base_layer; fbp.view_nlayers = vp.nlayers;
                    fbp.override_ok = (uint8_t)(!rv.set_format && !rv.set_swizzle && !rv.set_dsmode &&
                                                tex->views[0].format == fb_fmt);
                }
                char bwhy[192] = "";
                const int act = nvn_vk_feedback_bind_plan(&fbp, bwhy, sizeof bwhy);
                if (act == NVN_VK_FB_BIND_SUBSTITUTE) {
                    res_view[i] = fb_view;   // 采样本笔开始前的附件快照，而不是正在写入的附件
                    if (!g_fb_logged) {
                        g_fb_logged = 1;
                        FD_LOG("feedback: binding pre-pass snapshot for tex id=%u (serial=%llu) request=%.16s",
                               tid, (unsigned long long)tex->serial, request_id ? request_id : "?");
                    }
                } else if (act == NVN_VK_FB_BIND_LEAVE) {
                    static _Atomic int fb_leave_log;
                    int n = atomic_fetch_add(&fb_leave_log, 1);
                    if (n < 16)
                        FD_LOG("feedback: sampling tex id=%u (serial=%llu) keeps its live binding: %s",
                               tid, (unsigned long long)tex->serial, bwhy);
                    if (n == 0) atomic_fetch_add(&g_fb_leave_n, 1);
                }
            }
            res_kind[i] = 2;
            {
                const int64_t tp0 = fd_stamp();
                if (fd_pin_tex(tex) != 0) {
                    fail(why, why_size, "texture pin list is full; the sampled texture cannot be kept for this draw");
                    goto done;
                }
                if (res_view[i] != fb_view && fd_pin_view(tex, res_view[i]) != 0) {
                    fail(why, why_size,
                         "sampled texture view is not registered for keeping; refusing this draw");
                    goto done;
                }
                { int64_t d = fd_stamp() - tp0; if (d > 0) atomic_fetch_add(&g_res_pin_ns, (uint64_t)d); }
            }
            {
                int lvl = fd_tex_diag_level();
                if (lvl >= 2 || (lvl == 1 && tex->nvn_target != 1))
                    fd_tex_diag_once(request_id, tid, tex, &par, b->set, b->binding, slot);
            }
            FD_VLOG("bind[%s] set=%d binding=%d handleIndex=%d byte=0x%x -> nvn_tex_slot=%d tex_id=%u samp_id=%u image=%p view=%p fmt=%d %ux%u",
                   b->stage, b->set, b->binding, b->slot, byte_off, slot, tid, sid,
                   (void *)tex->image, (void *)tex->view, (int)tex->nvn_format, tex->width, tex->height);
            if (tr.on) {
                uint32_t swz = 0; int swz_set = 0;
                if (s2a_vk_adapter_tex_swz_by_id((int)tid, &swz, &swz_set) == 0)
                    FD_LOG("trace: tex id=%u feedback=%d serial=%llu %ux%u nvn_fmt=0x%x fmt=%d target=%u flags=0x%x gpu_written=%d "
                           "set_swz=%d swz=0x%04x (channels r=%u g=%u b=%u a=%u; NVN 0=zero 1=one 2=R 3=G 4=B 5=A)",
                           tid, (rt && tex->image == rt->image) ? 1 : 0,
                           (unsigned long long)tex->serial, tex->width, tex->height, tex->nvn_format,
                           (int)nvn_vk_format(tex->nvn_format), tex->nvn_target, tex->nvn_flags, tex->gpu_written,
                           swz_set, swz, swz & 7u, (swz >> 3) & 7u, (swz >> 6) & 7u, (swz >> 9) & 7u);
                int seen = 0;
                for (int q = 0; q < tr.tex_dumped; q++) if (tr.tex_ids[q] == tid) seen = 1;
                if (!seen && tr.tex_dumped < 8 && tr_dump_tex()) {
                    tr.tex_ids[tr.tex_dumped++] = tid;
                    char nm[64];
                    snprintf(nm, sizeof nm, "tex-%u.raw", tid);
                    tr_image_raw(d, &tr, nm, tex->image, tex->nvn_format, nvn_vk_format(tex->nvn_format), tex->width, tex->height, 0);
                }
            }
            { int64_t t1 = fd_stamp(); if (r0 && t1) { atomic_fetch_add(&g_res_tex_ns, (uint64_t)(t1 - r0)); atomic_fetch_add(&g_res_tex_c, 1); } }
        } else {
            char msg[128];
            snprintf(msg, sizeof msg, "descriptor kind %d is not implemented yet", b->kind);
            fail(why, why_size, msg);
            goto done;
        }
    }
    if (!prog.binding_count) { fail(why, why_size, "program has no bindings"); goto done; }
    // Publish only after every reflected resource has resolved. A missing later
    // binding leaves this draw's private support bitmap untouched.
    memcpy(support_map + SB_S2A_FLAGS + sizeof(uint32_t), tex_unnorm_bits, sizeof tex_unnorm_bits);

    // 顶点/索引缓冲：真实地址与范围，直接从 guest 内存拷（池直映射）。
    FD_VLOG("texture bound");
    t_res = fd_stamp();
    // 只上传这笔 draw 实际用到的顶点区间：索引 count 条 → [min(idx)+base, max(idx)+base]。
    // 全量上传 1 MiB（这笔只用到几十顶点）是每笔 record+submit 十几毫秒的主要来源。
    int vb_range_ok = 0;
    int64_t vb_first = 0, vb_last = 0;
    int32_t vertex_offset = (int32_t)state->base_vertex;
    if (state->index_address && state->count && state->index_type <= 2) {
        int all_vertex_rate = 1;
        for (int s = 0; s < 16; s++) if (stream_used[s] && state->vstream_divisor[s]) all_vertex_rate = 0;
        if (all_vertex_rate) {
            uint32_t isz = state->index_type == 0 ? 1 : state->index_type == 1 ? 2 : 4;
            const uint8_t *ip = (const uint8_t *)(uintptr_t)state->index_address;
            uint32_t mn = 0xffffffffu, mx = 0;
            for (uint32_t i = 0; i < state->count; i++) {
                uint32_t v;
                if (isz == 1) v = ip[i];
                else if (isz == 2) { uint16_t t; memcpy(&t, ip + (size_t)i * 2, 2); v = t; }
                else memcpy(&v, ip + (size_t)i * 4, 4);
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            int64_t first = (int64_t)state->base_vertex + mn, last = (int64_t)state->base_vertex + mx;
            if (first >= 0 && last >= first) {
                vb_range_ok = 1;
                for (int s = 0; s < 16 && vb_range_ok; s++) {
                    if (!stream_used[s]) continue;
                    uint32_t stride = state->vstream_stride[s];
                    if (!stride || (uint64_t)(last + 1) * stride > state->vbuf_size[s]) vb_range_ok = 0;
                }
                if (vb_range_ok) { vb_first = first; vb_last = last; vertex_offset = -(int32_t)mn; }
            }
        }
    }
    for (int s = 0; s < 16; s++) {
        if (!stream_used[s]) continue;
        if (!state->vbuf_addr[s] || !state->vbuf_size[s]) { fail(why, why_size, "vertex buffer for a used stream is not bound"); goto done; }
        uint32_t stride_s = state->vstream_stride[s];
        uint64_t src_off = vb_range_ok ? (uint64_t)vb_first * stride_s : 0;
        uint64_t src_len = vb_range_ok ? (uint64_t)(vb_last - vb_first + 1) * stride_s : state->vbuf_size[s];
        void *map = NULL; uint64_t voff = 0;
        VkBuffer pbuf = VK_NULL_HANDLE; uint64_t poff = 0;
        uint64_t gaddr = (uint64_t)(uintptr_t)(state->vbuf_addr[s] + src_off);
        if (s2a_poolahb_lookup(gaddr, src_len, &pbuf, &poff) == 0) {
            vb_buf[s] = pbuf; vb_off[s] = poff;      // 池已换 AHB 后备页：直接引用，不再拷贝
        } else {
            if (fd_arena_alloc(d, src_len, 4, &voff, &vb_buf[s], &map) != 0) { fail(why, why_size, "arena alloc failed for vertex stream"); goto done; }
            vb_off[s] = voff;
            g_cp_vb += src_len; g_cp_vb_n++;
            memcpy(map, (const void *)(uintptr_t)(state->vbuf_addr[s] + src_off), (size_t)src_len);
        }
        if (tr.on) {
            char nm[64], what[80];
            snprintf(nm, sizeof nm, "vb-stream%d.bin", s);
            snprintf(what, sizeof what, "vertex stream %d", s);
            tr_file(&tr, nm, (const void *)(uintptr_t)state->vbuf_addr[s], (size_t)state->vbuf_size[s]);
            tr_sha(what, (const void *)(uintptr_t)state->vbuf_addr[s], (size_t)state->vbuf_size[s]);
        }
    }
    VkIndexType itype = VK_INDEX_TYPE_UINT16;
    uint32_t index_size = 2;
    if (state->indexed) switch (state->index_type) {
        case 0: itype = VK_INDEX_TYPE_UINT8_EXT; index_size = 1; break;
        case 1: itype = VK_INDEX_TYPE_UINT16; index_size = 2; break;
        case 2: itype = VK_INDEX_TYPE_UINT32; index_size = 4; break;
        default: fail(why, why_size, "index type is not supported"); goto done;
    }
    if (state->indexed) {
        if (!state->index_address) { fail(why, why_size, "indexed draw has no index buffer address"); goto done; }
        VkDeviceSize bytes = (VkDeviceSize)state->count * index_size;
        void *map = NULL;
        uint64_t gaddr = (uint64_t)(uintptr_t)state->index_address;
        VkBuffer pbuf = VK_NULL_HANDLE; uint64_t poff = 0;
        if (s2a_poolahb_lookup(gaddr, (uint64_t)bytes, &pbuf, &poff) == 0) {
            ib_buf = pbuf; ib_off = poff;      // 池已换 AHB 后备页：直接引用
        } else {
            g_cp_ib += (uint64_t)bytes; g_cp_ib_n++;
            if (fd_arena_alloc(d, bytes, 4, &ib_off, &ib_buf, &map) != 0) { fail(why, why_size, "arena alloc failed for indices"); goto done; }
            memcpy(map, (const void *)(uintptr_t)state->index_address, (size_t)bytes);
        }
        if (tr.on) {
            tr_file(&tr, "indices.bin", (const void *)(uintptr_t)state->index_address, (size_t)bytes);
            tr_sha("indices", (const void *)(uintptr_t)state->index_address, (size_t)bytes);
        }
    }

    FD_VLOG("vertex + index buffers uploaded");
    t_vbuf = fd_stamp();
    // 描述符池/集：按反射实际用到的类型计数，逐条绑定写真实资源。
    {
        uint32_t n_ubo = 0, n_tex = 0, n_ssbo = 0;
        for (int i = 0; i < prog.binding_count; i++) {
            if (prog.bindings[i].kind == 0) n_ubo++;
            else if (prog.bindings[i].kind == 1) n_ssbo++;
            else if (prog.bindings[i].kind == 2) n_tex++;
        }
        // 只分配这次真的写到的 set：没有绑定的 set 不分配、也不绑定。
        // 一次调用仍然覆盖全部需要的 set（Adreno 上逐 set 多次调用是主要 CPU 成本），
        // 但不再无条件分配 4 个——程序只用 set0 时，另外 3 个纯属浪费。
        VkDescriptorSet sets[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        int used[4] = {0, 0, 0, 0};
        int used_n = 0;
        for (int i = 0; i < prog.binding_count; i++) {
            int st = prog.bindings[i].set;
            if (st < 0 || st > 3) continue;
            if (!used[st]) { used[st] = 1; used_n++; }
        }
        if (!used_n) { fail(why, why_size, "program declares no descriptor set"); goto done; }


        {
            const int64_t ta0 = fd_stamp();
            uint32_t alloc_n = (uint32_t)used_n;
            dpool = fd_frame_pool(d, alloc_n);
            { int64_t t1 = fd_stamp(); if (ta0 && t1) { atomic_fetch_add(&g_desc_pool_ns, (uint64_t)(t1 - ta0)); atomic_fetch_add(&g_desc_pool_c, 1); } }
            if (!dpool) { fail(why, why_size, "frame descriptor pool failed"); goto done; }
            {
                VkDescriptorSetLayout want[4];
                VkDescriptorSet got[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
                int map[4] = { -1, -1, -1, -1 };
                int n = 0;
                for (int st = 0; st < 4; st++) {
                    if (!used[st]) continue;
                    want[n] = set_layouts[st];
                    map[n] = st;
                    n++;
                }
                VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                ai.descriptorPool = dpool; ai.descriptorSetCount = (uint32_t)n; ai.pSetLayouts = want;
                VkResult ar;
                {
                    static _Atomic int injected;
                    int inject = fd_fail_desc_alloc_mode();
                    if (inject == 1 && atomic_exchange(&injected, 1) == 0) {
                        // 负例：不调用驱动，直接按“池类型计数耗尽”处理，验证换池重试分支。
                        ar = VK_ERROR_OUT_OF_POOL_MEMORY;
                        FD_LOG("descriptor set alloc: injected pool exhaustion (debug.s2a.fail_desc_alloc=1)");
                    } else {
                        ar = vkAllocateDescriptorSets(d->device, &ai, got);
                    }
                }
                if (ar != VK_SUCCESS) {
                    // 类型计数耗尽（不是真错误）：换池重试一次。旧池仍被在途命令引用，交给帧队列。
                    VkDescriptorPool np = fd_pool_rotate(d, dpool);
                    if (np) {
                        dpool = np;
                        ai.descriptorPool = np;
                        ar = vkAllocateDescriptorSets(d->device, &ai, got);
                    }
                }
                if (ar != VK_SUCCESS) {
                    char msg[160];
                    snprintf(msg, sizeof msg, "descriptor set alloc failed (ubo=%u ssbo=%u tex=%u sets=%d rc=%d)",
                             n_ubo, n_ssbo, n_tex, n, (int)ar);
                    fail(why, why_size, msg);
                    goto done;
                }
                for (int k = 0; k < n; k++) sets[map[k]] = got[k];
                g_desc_alloc_calls++;
                g_desc_tot_alloc++;
            }
            const int64_t ta1 = fd_stamp();
            VkWriteDescriptorSet writes[FD_MAX_BINDINGS];
            VkDescriptorBufferInfo bufs[FD_MAX_BINDINGS];
            VkDescriptorImageInfo imgs[FD_MAX_BINDINGS];
            uint32_t nw = 0;
            for (int i = 0; i < prog.binding_count; i++) {
                struct fd_binding *b = &prog.bindings[i];
                if (res_kind[i] == 0) continue;   // 这笔没有要写的描述符
                VkDescriptorType dt = res_kind[i] == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                    : res_kind[i] == 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                         : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                VkWriteDescriptorSet *w = &writes[nw];
                s2a_desc_write_init(w, sets[b->set], (uint32_t)b->binding, dt, 1);
                if (res_kind[i] == 2) {
                    imgs[nw].sampler = res_sampler[i];
                    imgs[nw].imageView = res_view[i];
                    imgs[nw].imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // 上传后/回放里纹理一直停在 GENERAL
                    w->pImageInfo = &imgs[nw];
                } else {
                    bufs[nw].buffer = res_kind[i] == 3 ? support_buf : res_buf[i];
                    bufs[nw].offset = res_kind[i] == 3 ? support_off : res_off[i];
                    bufs[nw].range = res_kind[i] == 3 ? support_size : res_size[i];
                    w->pBufferInfo = &bufs[nw];
                }
                FD_VLOG("desc write stage=%s set=%d binding=%d kind=%d buffer=%p view=%p range=%llu",
                       b->stage, b->set, b->binding, res_kind[i],
                       (void *)(res_kind[i] == 3 ? support_buf : res_buf[i]),
                       (void *)res_view[i], (unsigned long long)bufs[nw].range);
                nw++;
            }
            const int64_t tw0 = fd_stamp();
            if (ta1 && tw0) { atomic_fetch_add(&g_desc_loop_ns, (uint64_t)(tw0 - ta1)); atomic_fetch_add(&g_desc_loop_c, 1); }
            vkUpdateDescriptorSets(d->device, nw, writes, 0, NULL);
            {
                const int64_t ta2 = fd_stamp();
                if (ta0 && ta1) { atomic_fetch_add(&g_desc_alloc_ns, (uint64_t)(ta1 - ta0)); atomic_fetch_add(&g_desc_alloc_c, 1);
                                  atomic_fetch_add(&g_desc_tot_alloc_ns, (uint64_t)(ta1 - ta0)); }
                if (tw0 && ta2) { atomic_fetch_add(&g_desc_write_ns, (uint64_t)(ta2 - tw0)); atomic_fetch_add(&g_desc_write_c, 1);
                                  atomic_fetch_add(&g_desc_tot_write_ns, (uint64_t)(ta2 - tw0)); }
            }
            g_desc_update_calls++;
            g_desc_tot_update++;
            g_desc_writes += nw;
            g_desc_tot_writes += nw;
        }

        // 描述符/内存周期读数：默认**关**，debug.s2a.desc_stats=1 才开。
        // 默认关的原因：mem@ 报告要读 /proc/self/maps（世界里有 1.7 万行），是同步文件 IO、
        // 跑在渲染线程上，单次能顶出几毫秒；量的手段本身不能改变被量对象。
        if (fd_desc_stats_on()) {
            static _Atomic uint64_t print_n;
            const uint64_t tick = atomic_fetch_add(&print_n, 1) + 1;
            if (tick % 8192 == 0) fd_mem_report("periodic");   // 内存增长曲线：每 8192 笔对象一行
            if (tick % 512 == 0) {
                static uint64_t pa, pu, pw, pan, pwn;
                const uint64_t a = g_desc_tot_alloc, u = g_desc_tot_update, w = g_desc_tot_writes;
                const uint64_t an = atomic_load(&g_desc_tot_alloc_ns), wn = atomic_load(&g_desc_tot_write_ns);
                const uint64_t da = a - pa, du = u - pu, dw = w - pw;
                const uint64_t dan = an - pan, dwn = wn - pwn;
                FD_LOG("desc rise(每512笔对象): alloc=%llu update=%llu 写入=%llu | "
                       "每笔 alloc=%.2f update=%.2f 写入=%.2f | 每笔 alloc=%.2f us update+写入=%.2f us",
                       (unsigned long long)da, (unsigned long long)du, (unsigned long long)dw,
                       (double)da / 512.0, (double)du / 512.0, (double)dw / 512.0,
                       da ? (double)dan / 1e3 / (double)da : 0.0,
                       du ? (double)dwn / 1e3 / (double)du : 0.0);
                pa = a; pu = u; pw = w; pan = an; pwn = wn;
            }
        }
        // 命令缓冲：非合并模式逐笔；合并模式直接录进 open pass 的 cmd。
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        FD_VLOG("descriptors written; recording");
        t_desc = fd_stamp();
        if (!merge_draw) {
            VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cbi.commandPool = d->command_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(d->device, &cbi, &cmd) != VK_SUCCESS) { cmd = VK_NULL_HANDLE; fail(why, why_size, "command buffer alloc failed"); goto done; }
        } else {
            cmd = g_op.cmd;
            if (!cmd) { fail(why, why_size, "open pass has no command buffer"); goto done; }
        }
        if (rt && fd_probe_enabled()) {
            VkBufferCreateInfo pbi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            pbi.size = 5 * 16 * 2; pbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;   // 5 个采样点 × 前/后 × 16 B
            if (vkCreateBuffer(d->device, &pbi, NULL, &probe_buf) != VK_SUCCESS) { fail(why, why_size, "probe buffer failed"); goto done; }
            VkMemoryRequirements pmr; vkGetBufferMemoryRequirements(d->device, probe_buf, &pmr);
            VkMemoryAllocateInfo pmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            pmai.allocationSize = pmr.size; pmai.memoryTypeIndex = s2a_vk_memory_type(d, pmr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
            if (vkAllocateMemory(d->device, &pmai, NULL, &probe_mem) != VK_SUCCESS) { fail(why, why_size, "probe memory failed"); goto done; }
            vkBindBufferMemory(d->device, probe_buf, probe_mem, 0);
            if (vkMapMemory(d->device, probe_mem, 0, 5 * 16 * 2, 0, &probe_map) != VK_SUCCESS) { fail(why, why_size, "probe map failed"); goto done; }
        }
        if (!merge_draw) {
            vkBeginCommandBuffer(cmd, &bi);
            // 命令缓冲由池里取、句柄会被复用：begin 会把绑定状态清空，但句柄值可能和上一笔相同，
            // 只看句柄会误判“管线/视口还在这个 cmd 里”，于是跳过 BindPipeline 直接录 draw
            // （Adreno 在未绑管线的 cmd 上录 draw 会 SIGSEGV）。begin 之后一律作废绑定状态。
            g_cmd_state.cmd = VK_NULL_HANDLE;
            g_cmd_state.pipe = VK_NULL_HANDLE;
            g_cmd_state.vp_valid = 0;
            g_cmd_state.sc_valid = 0;
        }
        VkBufferImageCopy probe_copy = {0};
        probe_copy.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        probe_copy.imageExtent = (VkExtent3D){ 1, 1, 1 };
        // 5 个采样点（中心 + 四分之一位置）：只读中心会把“画在别处”的绘制误判成没画。
        uint32_t probe_bpp = 4;   // 按真实格式决定每像素字节数：RGBA8=4、RGBA32F=16、RGB565=2、R8=1
        switch (rt_format) {
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_SNORM:
            case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: probe_bpp = 4; break;
            case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: probe_bpp = 16; break;
            case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: probe_bpp = 8; break;
            case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16: probe_bpp = 2; break;
            case VK_FORMAT_R8_UNORM: probe_bpp = 1; break;
            default: break;
        }
        // depth-only pass 没有颜色目标：采样坐标按本笔真实目标尺寸（fw/fh）算，不能解引用空的 rt。
        const int32_t probe_x[5] = { (int32_t)(fw / 2), (int32_t)(fw / 4), (int32_t)(fw * 3 / 4),
                                     (int32_t)(fw / 4), (int32_t)(fw * 3 / 4) };
        const int32_t probe_y[5] = { (int32_t)(fh / 2), (int32_t)(fh / 4), (int32_t)(fh / 4),
                                     (int32_t)(fh * 3 / 4), (int32_t)(fh * 3 / 4) };
        if (probe_buf) for (int i = 0; i < 5; i++) {
            probe_copy.bufferOffset = (VkDeviceSize)i * 16;
            probe_copy.imageOffset = (VkOffset3D){ probe_x[i], probe_y[i], 0 };
            vkCmdCopyImageToBuffer(cmd, rt->image, VK_IMAGE_LAYOUT_GENERAL, probe_buf, 1, &probe_copy);
        }
        if (rt && tr.on && tr.map && !merge_draw) {
            VkImageMemoryBarrier tb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            tb.oldLayout = tb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            tb.image = rt->image;
            tb.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            tb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            tb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &tb);
            VkBufferImageCopy tc = {0};
            tc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            tc.imageExtent = (VkExtent3D){ fw, fh, 1 };
            vkCmdCopyImageToBuffer(cmd, rt->image, VK_IMAGE_LAYOUT_GENERAL, tr.buf, 1, &tc);   // offset 0 = before
        }
        VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        if (!merge_draw) {
            if (rt) {
                bar.image = rt->image;
                bar.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
                bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     0, 0, NULL, 0, NULL, 1, &bar);
            }
            if (depth_tex) {
                VkImageMemoryBarrier dbar = bar;
                dbar.image = depth_tex->image;
                dbar.subresourceRange = (VkImageSubresourceRange){ nvn_vk_format_has_stencil(nvn_vk_format(depth_tex->nvn_format))
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                dbar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                dbar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                     0, 0, NULL, 0, NULL, 1, &dbar);
            }
            if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: about to precopy (%d)", g_bcrt_n);
            if (g_bcrt_n) fd_bcrt_precopy(cmd);   // 底图块 → 影子；必须在 BeginRenderPass 之前
            if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: precopy recorded; begin render pass %ux%u pass=%p fb=%p", fw, fh, (void *)pass, (void *)fb);
            VkClearValue clears[NVN_VK_MRT_MAX_COLOR + 1];
            memset(clears, 0, sizeof clears);
            if (depth_tex) {
                uint32_t di = color_attachment_count < NVN_VK_MRT_MAX_COLOR + 1 ? color_attachment_count : 0u;
                clears[di].depthStencil = (VkClearDepthStencilValue){
                    state->depth_clear, (uint32_t)state->stencil_clear };
            }
            VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpbi.renderPass = pass; rpbi.framebuffer = fb;
            rpbi.renderArea = (VkRect2D){ {0,0}, {fw, fh} };
            rpbi.clearValueCount = color_attachment_count + (depth_tex ? 1u : 0u);
            rpbi.pClearValues = clears;
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: render pass begun");
        }
        if (g_cmd_state.cmd != cmd) {   // 换命令缓冲：影子状态作废，必须重新绑定
            g_cmd_state.cmd = cmd;
            g_cmd_state.pipe = VK_NULL_HANDLE;
            g_cmd_state.vp_valid = 0;
            g_cmd_state.sc_valid = 0;
        }
        if (g_cmd_state.pipe != pipeline) {
            if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: bind pipeline %p", (void *)pipeline);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: pipeline bound");
            g_cmd_state.pipe = pipeline;
        }
        if (!g_cmd_state.vp_valid || memcmp(&g_cmd_state.vp, &viewport, sizeof viewport) != 0) {
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            g_cmd_state.vp = viewport; g_cmd_state.vp_valid = 1;
        }
        if (!g_cmd_state.sc_valid || memcmp(&g_cmd_state.sc, &scissor, sizeof scissor) != 0) {
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            g_cmd_state.sc = scissor; g_cmd_state.sc_valid = 1;
        }
        {
            // 只绑这次分配过的 set（未分配的是 VK_NULL_HANDLE，绑它是非法的）。
            {
                // 连续 set 合并成一次绑定，无动态偏移。
                int first = -1, last = -1, count = 0, contiguous = 1;
                for (int set = 0; set < 4; set++) {
                    if (!used[set]) continue;
                    if (first < 0) first = set;
                    if (last >= 0 && set != last + 1) contiguous = 0;
                    last = set; count++;
                }
                if (first == 0 && contiguous)
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, (uint32_t)count,
                                            &sets[0], 0, NULL);
                else
                    for (int set = 0; set < 4; set++)
                        if (used[set])
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, (uint32_t)set, 1,
                                                    &sets[set], 0, NULL);
            }
        }
        if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: bind vertex streams (%u)", vbind_count);
        for (uint32_t s = 0; s < vbind_count; s++) {
            uint32_t stream = vbind[s].binding;
            if (!vb_buf[stream]) { fail(why, why_size, "vertex stream has no buffer at bind time"); goto done; }
            VkDeviceSize vboff = vb_off[stream];
            vkCmdBindVertexBuffers(cmd, stream, 1, &vb_buf[stream], &vboff);
        }
        if (state->indexed) {
            if (!ib_buf) { fail(why, why_size, "indexed draw has no index buffer at bind time"); goto done; }
            vkCmdBindIndexBuffer(cmd, ib_buf, ib_off, itype);
        }
        if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: vertex/index buffers bound (ib=%p idx=%d)", (void *)ib_buf, (int)state->indexed);
        if (merge_draw) {
            // 计数中：这一笔绘制属于某个 query 段。段按 render pass 拆，Report/Reset 处切段；
            // 开不了段（槽耗尽/池未 reset）就准确失败，不能“画了但没计数”。
            char qwhy[200] = "";
            int qrc = nvn_vk_query_draw_begin(d, cmd, qwhy, sizeof qwhy);
            if (qrc < 0) { fail(why, why_size, qwhy); goto done; }
        }
        nvn_vk_callring_add("%s cmd=%p ib=%p n=%u inst=%u vtx=%d binst=%u",
                            state->indexed ? "drawIndexed" : "draw",
                            (void *)cmd, (void *)ib_buf, (unsigned)state->count,
                            (unsigned)(state->instances ? state->instances : 1), (int)vertex_offset,
                            (unsigned)state->base_instance);
        if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: about to draw count=%u idx=%d", state->count, (int)state->indexed);
        if (state->indexed)
            vkCmdDrawIndexed(cmd, state->count, state->instances ? state->instances : 1, 0, vertex_offset,
                             state->base_instance);
        else
            vkCmdDraw(cmd, state->count, state->instances ? state->instances : 1,
                      (uint32_t)((int32_t)state->first + vertex_offset), state->base_instance);
        if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: draw recorded");
        {
            // 可写 storage buffer：把本笔的 shader 写发布给后续 GPU 读/传输。
            // 绑定在 render pass 内也允许 buffer 屏障（只有 image layout 转换受限）。
            VkBufferMemoryBarrier wb_bar[FD_MAX_BINDINGS];
            uint32_t nwb_bar = 0;
            for (int i = 0; i < FD_MAX_BINDINGS; i++) {
                if (!res_ssbo_wr[i] || res_kind[i] != 4 || !res_buf[i]) continue;
                VkBufferMemoryBarrier *wb = &wb_bar[nwb_bar];
                memset(wb, 0, sizeof *wb);
                wb->sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                wb->srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                wb->dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
                wb->srcQueueFamilyIndex = wb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                wb->buffer = res_buf[i];
                wb->offset = (VkDeviceSize)res_off[i];
                wb->size = (VkDeviceSize)(res_size[i] ? res_size[i] : VK_WHOLE_SIZE);
                nwb_bar++;
            }
            if (nwb_bar)
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, nwb_bar, wb_bar, 0, NULL);
        }
        if (g_gpu_cur >= 0) {
            GPU_META_CUR.draws++;
            GPU_META_CUR.verts += (uint64_t)state->count * (state->instances ? state->instances : 1);
        }
        if (!merge_draw) {
        vkCmdEndRenderPass(cmd);
        if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: render pass ended");
        if (g_bcrt_n) fd_bcrt_postcopy(cmd);
        if (fd_bcrt_log() && g_bcrt_n) FD_LOG("bcrt: postcopy recorded");   // 影子 → 底图块；必须在 EndRenderPass 之后
        if (rt) {
            bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, NULL, 0, NULL, 1, &bar);
        } else if (depth_tex) {
            // 无颜色附件：只把深度写完成发布给后续读取（绘制本身不需要颜色屏障）。
            VkImageMemoryBarrier dbar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            dbar.oldLayout = dbar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            dbar.srcQueueFamilyIndex = dbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            dbar.image = depth_tex->image;
            dbar.subresourceRange = (VkImageSubresourceRange){ nvn_vk_format_has_stencil(nvn_vk_format(depth_tex->nvn_format))
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            dbar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dbar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, NULL, 0, NULL, 1, &dbar);
        }
        if (rt && tr.on && tr.map && !merge_draw) {
            VkBufferImageCopy tc = {0};
            tc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            tc.imageExtent = (VkExtent3D){ fw, fh, 1 };
            tc.bufferOffset = (VkDeviceSize)fw * fh * tr.bpp;
            vkCmdCopyImageToBuffer(cmd, rt->image, VK_IMAGE_LAYOUT_GENERAL, tr.buf, 1, &tc);   // second half = after
        }
        if (probe_buf) for (int i = 0; i < 5; i++) {
            VkBufferImageCopy after = probe_copy;
            after.bufferOffset = (VkDeviceSize)(5 + i) * 16;
            after.imageOffset = (VkOffset3D){ probe_x[i], probe_y[i], 0 };
            vkCmdCopyImageToBuffer(cmd, rt->image, VK_IMAGE_LAYOUT_GENERAL, probe_buf, 1, &after);
            (void)after;
        }
        vkEndCommandBuffer(cmd);
        }   // !merge_draw：逐笔命令缓冲在这里收尾
        if (!merge_draw) {
            int qi = g_fq.ncmd;
            if (qi >= fd_fq_cmd_capacity()) {
                vkEndCommandBuffer(cmd);
                vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
                fd_fq_fail(FD_FQ_FAILED, "frame queue command-buffer table is full; draw not recorded");
                fail(why, why_size, "frame queue capacity is full; draw not recorded");
                goto done;
            }
            if (qi < FD_FRAME_CMDS) {
                g_fq.cmds[qi] = cmd;
                g_fq.pbuf[qi] = probe_buf; g_fq.pmem[qi] = probe_mem; g_fq.pmap[qi] = probe_map;
                g_fq.pw[qi] = fw; g_fq.ph[qi] = fh; g_fq.pbpp[qi] = probe_bpp; g_fq.pcount[qi] = state->count;
                snprintf(g_fq.pid[qi], sizeof g_fq.pid[qi], "%s", request_id ? request_id : "?");
                g_fq.ncmd++;
                probe_buf = VK_NULL_HANDLE; probe_mem = VK_NULL_HANDLE; probe_map = NULL;
                cmd = VK_NULL_HANDLE;
                deferred = 1;
                t_submitted = fd_stamp();
            }
        }
        if (merge_draw) { deferred = 1; t_submitted = fd_stamp(); }
        if (deferred)
            fd_note_draw_stats(t_body0, t_start, t_parsed, t_modules, t_layouts, t_pass, t_pipeline,
                               t_submitted, t_support, t_res, t_vbuf, t_desc, t_pass_a, t_pass_b);
        if (!deferred) {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence;
        if (vkCreateFence(d->device, &fci, NULL, &fence) != VK_SUCCESS) { fail(why, why_size, "fence create failed"); goto done; }
        FD_VLOG("command buffer recorded; submitting");
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        FD_VLOG("submitted; waiting fence");
        int64_t t_before_submit = fd_stamp();
        if (vkQueueSubmit(d->queue, 1, &si, fence) != VK_SUCCESS) {
            vkDestroyFence(d->device, fence, NULL);
            vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
            fail(why, why_size, "vkQueueSubmit failed for the draw");
            goto done;
        }
        if (vkWaitForFences(d->device, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
            // 提交成功但未拿到完成证明：命令缓冲和它引用的资源仍在 GPU 上，Vulkan 禁止此时释放。
            // 保留全部在途资源（有意泄漏），返回失败，让上层知道这笔没有完成证明。
            FD_LOG("fence timeout: draw resources retained (cmd=%p)", (void *)(uintptr_t)cmd);
            snprintf(why, why_size, "GPU draw submit succeeded but fence wait timed out; in-flight resources retained");
            return -1;
        }
        t_submitted = fd_stamp();
        {
            uint64_t us = (uint64_t)(t_submitted && t_start ? (uint64_t)((t_submitted - t_start) / 1000) : 0);
            uint32_t n = atomic_fetch_add(&g_draw_n, 1) + 1;
            atomic_fetch_add(&g_draw_us_sum, us);
            atomic_fetch_add(&g_us_modules, (uint64_t)((t_modules - t_parsed) / 1000));
            atomic_fetch_add(&g_us_layouts, (uint64_t)((t_layouts - t_modules) / 1000));
            atomic_fetch_add(&g_us_pass, (uint64_t)((t_pass - t_layouts) / 1000));
            atomic_fetch_add(&g_us_pipe, (uint64_t)((t_pipeline - t_pass) / 1000));
            atomic_fetch_add(&g_us_record, (uint64_t)((t_before_submit - t_pipeline) / 1000));
            atomic_fetch_add(&g_us_wait, (uint64_t)((t_submitted - t_before_submit) / 1000));
            if (n % 128 == 0)
                FD_LOG("draw timing: avg=%llu us over %u draws [modules=%llu layouts=%llu pass=%llu pipe=%llu record=%llu wait=%llu]",
                       (unsigned long long)(atomic_load(&g_draw_us_sum) / n), n,
                       (unsigned long long)(atomic_load(&g_us_modules) / n),
                       (unsigned long long)(atomic_load(&g_us_layouts) / n),
                       (unsigned long long)(atomic_load(&g_us_pass) / n),
                       (unsigned long long)(atomic_load(&g_us_pipe) / n),
                       (unsigned long long)(atomic_load(&g_us_record) / n),
                       (unsigned long long)(atomic_load(&g_us_wait) / n));
            int64_t total_us = (t_submitted - t_start) / 1000;
            if (total_us >= 30000) {   // 只打慢笔：定位 2.4 s 帧由哪些 draw/阶段构成
                FD_LOG("timing: request=%s count=%u total=%.1fms parse=%.1f modules=%.1f layouts=%.1f pass=%.1f pipeline=%.1f record+submit=%.1f wait=%.1f",
                       request_id ? request_id : "?", state->count,
                       (t_submitted - t_start) / 1e6,
                       (t_parsed ? (t_parsed - t_start) : 0) / 1e6,
                       (t_modules && t_parsed ? (t_modules - t_parsed) : 0) / 1e6,
                       (t_layouts && t_modules ? (t_layouts - t_modules) : 0) / 1e6,
                       (t_pass && t_layouts ? (t_pass - t_layouts) : 0) / 1e6,
                       (t_pipeline && t_pass ? (t_pipeline - t_pass) : 0) / 1e6,
                       (t_before_submit - t_pipeline) / 1e6,
                       (t_submitted - t_before_submit) / 1e6);
            }
        }
        // GPU 已经写过这些图像：标记后缓存淘汰不会拿旧 CPU 内容重建它们。
        if (rt) s2a_vk_adapter_mark_written(rt->image);
        if (depth_tex) s2a_vk_adapter_mark_written(depth_tex->image);
        vkDestroyFence(d->device, fence, NULL);
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        if (probe_map) {
            const uint8_t *px = (const uint8_t *)probe_map;
            int changed = 0;
            for (int i = 0; i < 5; i++)
                if (memcmp(px + i * 16, px + (5 + i) * 16, probe_bpp) != 0) changed++;
            FD_LOG("probe rt=%ux%u bpp=%u changed=%d/5 before[0]=%08x after[0]=%08x before[4]=%08x after[4]=%08x",
                   fw, fh, probe_bpp, changed,
                   probe_bpp >= 4 ? *(const uint32_t *)(px + 0) : 0, probe_bpp >= 4 ? *(const uint32_t *)(px + 5 * 16) : 0,
                   probe_bpp >= 4 ? *(const uint32_t *)(px + 4 * 16) : 0, probe_bpp >= 4 ? *(const uint32_t *)(px + 9 * 16) : 0);
            if (rt && tr.on && tr.map) {
                VkDeviceSize rb = (VkDeviceSize)fw * fh * tr.bpp;
                if (tr_dump_rt()) {
                    tr_file(&tr, "rt-before.raw", tr.map, (size_t)rb);
                    tr_file(&tr, "rt-after.raw", (const uint8_t *)tr.map + rb, (size_t)rb);
                }
                tr_rt_diff(&tr, fw, fh, tr.bpp);
            }
            {
                const uint8_t *pa = px + 5 * 16;   // after: 中心点
                if (changed && pa[1] > 180 && pa[0] < 60 && pa[2] < 60)
                    FD_LOG("green-detect: request=%s count=%u rt=%ux%u before=%02x%02x%02x%02x after=%02x%02x%02x%02x",
                           request_id ? request_id : "?", state->count, fw, fh,
                           px[0], px[1], px[2], px[3], pa[0], pa[1], pa[2], pa[3]);
            }
            if (1) snprintf(why, why_size,
                     "first indexed draw executed: rt=%ux%u count=%u baseVertex=%u instances=%u ubo=%s tex=%s "
                     "pixel_before=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x "
                     "pixel_after=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x request=%s",
                     fw, fh, state->count, state->base_vertex, state->instances ? state->instances : 1,
                     "bound", "bound",
                     px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7],
                     px[8], px[9], px[10], px[11], px[12], px[13], px[14], px[15],
                     px[5 * 16 + 0], px[5 * 16 + 1], px[5 * 16 + 2], px[5 * 16 + 3],
                     px[5 * 16 + 4], px[5 * 16 + 5], px[5 * 16 + 6], px[5 * 16 + 7],
                     px[5 * 16 + 8], px[5 * 16 + 9], px[5 * 16 + 10], px[5 * 16 + 11],
                     px[5 * 16 + 12], px[5 * 16 + 13], px[5 * 16 + 14], px[5 * 16 + 15],
                     request_id ? request_id : "?");
        }
        }
    }

    (void)session; (void)generation;
    rc = 0;
    // 尾段起点 = 最后一笔录制时间戳（合并模式在 fd_note_draw_stats 之前取，同步模式在等 fence 之后取）。
    // 只有 draw_tail 打开、且本笔拿到了时间戳时才采；goto done 的失败路径保持 0。
    if (t_tail_entry && t_submitted) t_tail_post0 = t_submitted;
    fd_preheat_tick(d);   // 加载窗口内每帧最多预热一条；真实 draw 已完成，需求优先

done:
    fd_bcrt_reset();
    if (deferred) {
        // 资源归帧队列，present 前统一释放。模块/布局/layout 归程序对象表，不在这里销毁。
        // 合并模式下 pass/fb 在创建时已挂到帧队列，不能重复挂（否则 flush 时二次销毁）。
        if (!merge_draw) {
            // 只交接本笔拥有的对象：缓存里的 render pass 归缓存，不能塞进帧队列（否则 flush 时
            // 会把还在缓存里的 pass 销毁，下一笔复用就是失效句柄）。
            int kept = 0;
            if (fb_owned && fb) { if (fd_fq_keep_fb(fb) != 0) kept = 1; else fb_owned = 0; }
            if (pass_owned && pass) { if (fd_fq_keep_pass(pass) != 0) kept = 1; else pass_owned = 0; }
            if (kept && rc == 0) {
                rc = -1;
                fail(why, why_size, "frame queue capacity is full; draw's framebuffer/pass retained but not tracked");
            }
        }
        // support/UBO/VB/IB 是帧 arena 的切片，descriptor pool 是帧池：都由帧 flush 统一释放。
        // 采样器归采样器缓存所有，不在帧 flush 销毁。
    } else {
        // 失败/同步路径：只销毁本笔拥有、且没交给帧队列或缓存的对象。
        // 合并模式下 fb/pass 创建即交给帧队列、probe 已录进 open pass 的命令缓冲；在这里销毁会让
        // 驱动在随后的 EndQuery/EndRenderPass 里访问已释放对象（菜单键崩溃的根因）。
        // 管线归缓存、模块/布局归程序对象表、support/UBO/VB/IB 归帧 arena、descriptor pool 归帧池、
        // 采样器归采样器缓存：都不在本笔销毁。
        if (fb_owned && fb) vkDestroyFramebuffer(d->device, fb, NULL);
        if (pass_owned && pass) vkDestroyRenderPass(d->device, pass, NULL);
        if (!merge_draw) {
            if (probe_map) vkUnmapMemory(d->device, probe_mem);
            if (probe_buf) vkDestroyBuffer(d->device, probe_buf, NULL);
            if (probe_mem) fd_devmem_release_dev(d->device, probe_mem);
            if (tr.map) vkUnmapMemory(d->device, tr.mem);
            if (tr.buf) vkDestroyBuffer(d->device, tr.buf, NULL);
            if (tr.mem) fd_devmem_release_dev(d->device, tr.mem);
        } else if (probe_buf || probe_map) {
            FD_LOG("draw failure: merge path retains probe objects (%s); they are referenced by the open pass command buffer",
                   why);
        }
    }
    // 本笔失败时不要把半开的 pass 留给后续命令：结束并交给帧队列，下一笔重新开 pass。
    if (merge_draw && rc != 0 && g_op.open) {
        char qd[256]; nvn_vk_query_debug(qd, sizeof qd);
        FD_LOG("draw failure: %s; closing pass cmd=%p %s", why, (void *)g_op.cmd, qd);
        g_close_probe = 1;
        int close_rc = fd_pass_close(d, FD_PCR_FAIL_CLEANUP);
        FD_LOG("close probe: pass close returned rc=%d", close_rc);
        g_close_probe = 0;
        if (close_rc != 0)
            FD_LOG("draw failure: closing the open pass also failed (%s)", g_fq.fail[0] ? g_fq.fail : "unknown");
    }
    // 后段残差（默认关）：t_submitted 之后的 fd_note_draw_stats / fd_preheat_tick / 收尾清理。
    // 起点复用已有时间戳，只多这一次取钟。失败路径不采（不把错误收尾混进稳态读数）。
    if (t_tail_post0) {
        __atomic_fetch_add(&g_tail_post_ns, (uint64_t)(fd_now_ns() - t_tail_post0), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_tail_post_n, 1, __ATOMIC_RELAXED);
    }
    // 主体未归属（开关 debug.s2a.body_gap=1，默认关）：t_submitted → 返回（含 fd_note_draw_stats、
    // fd_preheat_tick、收尾清理）。这一笔的返回发生在 fd_note_draw_stats 之后，所以 tail 计入下一窗
    // （与 draw_tail 的 post 同口径，1/256 的错位可忽略）。
    if (t_submitted && atomic_load(&g_bg_on)) {
        __atomic_fetch_add(&g_bg_tail_ns, (uint64_t)(fd_now_ns() - t_submitted), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_bg_tail_n, 1, __ATOMIC_RELAXED);
    }
    // prog 归程序缓存所有，不在本笔释放。
    return rc;
}
