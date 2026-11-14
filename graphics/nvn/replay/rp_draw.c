// 绘制（对齐 nvn_mtl.m draw / draw_emit @12900 一带与 encode_args_ @9640 的职责）：
// 状态类命令早已落在 rp_S 里，这里在绘制时把它们变成后端的管线、动态状态、顶点流和绑定，再发绘制。
// 没改过的不重发：管线看 rp_pso_dirty，各阶段绑定看 rp_args_dirty[阶段]。
// 四边形图元（NVN 7）没有原生对应：在这里展开成三角形列表的 32 位索引（每个四边形 0,1,2 / 0,2,3，同 nvn_mtl.m fanquad_expand），按带索引的三角形画。
// 另外三种没有原生对应的图元（#710）：四边形带（8）当三角带画（顶点序不变；零头 1 个丢掉；带索引又开着重启时按段展开成三角形列表），
// 多边形（9）当三角扇画，线环（2）展开成线段列表的 32 位索引（每段末尾补一条回到段首的线）。
// 还没接：间接计数多绘、变换反馈、邻接图元与面片、图元重启以外的小状态、存储缓冲与图像绑定、空槽假贴图。
#include "rp_exec.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_hmap.h"
#include "nvn_plat.h"
#include "nvn_refl.h"
#include "nvn_fmt_info.h"
#include "nvn_dirty.h"   // 写跟踪：暂存片的逐页写代号（nvk_slc_gen 用）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// SupportBuffer 里现在要填的字段（翻译器产物的布局；其余保持 0）
enum { SB_RENDER_SCALE = 192, SB_S2A_FLAGS = 1392, SB_S2A_POINT = 1408, SB_RENDER_SCALE_Y = 1440, SB_RENDER_SCALE_COUNT = 73 };
// 驱动常量段 c0 的合成布局（着色器从它读存储缓冲描述符表、最大点径、base instance）
enum { DRV0_BYTES = 0xA00 };
static const int k_drv0_base[5] = {0x110, 0x510, 0x410, 0x210, 0x310};   // 各图形阶段的存储缓冲表起点

int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c
extern int s2a_nvn_rast_flip;   // hle_nvn.c：当前提交队列的正面光栅翻转位（建队列时按窗口原点定）

static const struct rp_prog *g_prog; static nvn_gpu_pipe g_pipe; static nvn_gpu_cmd g_pipe_cmd;
static uint64_t g_draws, g_skips, g_dispatches; static uint32_t g_prim = ~0u; static int g_quads = -1, g_face = -1;   // g_quads：这笔的索引展开方式（EXP_*）
static int g_ms_off;   // #1015：当前管线是「多重采样目标上关了多重采样」（游戏 ms.enable = 0、目标样本数 > 1）

// 四边形 → 三角形索引。src NULL = 不带索引（顶点自己，0 起；first 由调用方放进 base vertex）；否则是 isz 字节一个的索引。
// rv >= 0：图元重启值，每遇到一个就另起一串四边形，前一串不足一个四边形的零头丢掉。返回写出的索引个数（d 至少要 count / 4 * 6 个）
static uint32_t quad_expand(const uint8_t *src, uint32_t isz, uint32_t count, int64_t rv, uint32_t *d) {
#define IX(i) (!src ? (uint32_t)(i) : isz == 1 ? src[i] : isz == 2 ? (uint32_t)(src[(size_t)(i) * 2] | src[(size_t)(i) * 2 + 1] << 8) : \
               ((uint32_t)src[(size_t)(i) * 4] | (uint32_t)src[(size_t)(i) * 4 + 1] << 8 | (uint32_t)src[(size_t)(i) * 4 + 2] << 16 | (uint32_t)src[(size_t)(i) * 4 + 3] << 24))
    uint32_t o = 0;
    for (uint32_t s = 0; s < count;) {
        uint32_t e = s; if (src && rv >= 0) { while (e < count && IX(e) != (uint32_t)rv) e++; } else e = count;
        for (uint32_t q = s; q + 4 <= e; q += 4) {
            d[o++] = IX(q); d[o++] = IX(q + 1); d[o++] = IX(q + 2);
            d[o++] = IX(q); d[o++] = IX(q + 2); d[o++] = IX(q + 3);
        }
        s = e + 1;
    }
#undef IX
    return o;
}

// 索引展开方式：0 = 不展开；其余见下面三个展开函数
enum { EXP_NONE = 0, EXP_QUADS = 7, EXP_LOOP = 2, EXP_QSTRIP = 8 };
#define IXR(i) (!src ? (uint32_t)(i) : isz == 1 ? src[i] : isz == 2 ? (uint32_t)(src[(size_t)(i) * 2] | src[(size_t)(i) * 2 + 1] << 8) : \
                ((uint32_t)src[(size_t)(i) * 4] | (uint32_t)src[(size_t)(i) * 4 + 1] << 8 | (uint32_t)src[(size_t)(i) * 4 + 2] << 16 | (uint32_t)src[(size_t)(i) * 4 + 3] << 24))
// 线环 → 线段列表。每段（重启值之间）n 个点：n ≥ 2 时出 n 条线（i,i+1）与收口的（n-1,0）；不足 2 个点的段什么都不画。
// 用线段列表而不是「线带 + 补首点」，是为了不依赖后端的线带重启：各段之间不用插重启值。每条线的首点就是线带里那条线的首点，激发顶点不变。
// d 至少要 count * 2 个
static uint32_t loop_expand(const uint8_t *src, uint32_t isz, uint32_t count, int64_t rv, uint32_t *d) {
    uint32_t o = 0;
    for (uint32_t s = 0; s < count;) {
        uint32_t e = s; if (src && rv >= 0) { while (e < count && IXR(e) != (uint32_t)rv) e++; } else e = count;
        if (e - s >= 2) {
            for (uint32_t i = s; i + 1 < e; i++) { d[o++] = IXR(i); d[o++] = IXR(i + 1); }
            d[o++] = IXR(e - 1); d[o++] = IXR(s);
        }
        s = e + 1;
    }
    return o;
}
// 四边形带 → 三角形列表，只在带索引又开着重启时用（其余情况直接当三角带画）。每段按三角带的绕序：
// 第 k 个四边形（段内第 2k..2k+3 个点）出 (2k,2k+1,2k+2) 与 (2k+1,2k+3,2k+2)：就是 Vulkan 三角带第 2k、2k+1 个三角形的顶点序
// （规范：第 i 个三角形是 {i, i+(1+i%2), i+(2-i%2)}），绕序与激发顶点都和直接当三角带画一致；段内零头丢掉。
// d 至少要 count * 3 个
static uint32_t qstrip_expand(const uint8_t *src, uint32_t isz, uint32_t count, int64_t rv, uint32_t *d) {
    uint32_t o = 0;
    for (uint32_t s = 0; s < count;) {
        uint32_t e = s; while (e < count && IXR(e) != (uint32_t)rv) e++;
        for (uint32_t q = s; q + 4 <= e; q += 2) {
            d[o++] = IXR(q); d[o++] = IXR(q + 1); d[o++] = IXR(q + 2);
            d[o++] = IXR(q + 1); d[o++] = IXR(q + 3); d[o++] = IXR(q + 2);
        }
        s = e + 1;
    }
    return o;
}
#undef IXR

// 贴图数组（着色器按运行时下标取句柄）的各项：每个阶段至多一个数组，至多 RP_TEXARR_MAX 项。
// 数组第 k 项 = 驱动句柄表的第 k 个 8 字节项；贴图槽 s 的句柄在第 4 + s 项，前 4 项与没绑的槽读作池里的第 0 项（同单个贴图槽没绑时的口径）
enum { RP_TEXARR_MAX = 64, RP_HANDLE_TABLE_FIRST_SLOT = 4 };
static nvn_gpu_view g_av[RP_PROG_STAGES][RP_TEXARR_MAX]; static nvn_gpu_samp g_as[RP_PROG_STAGES][RP_TEXARR_MAX];

// 后端能力表在设备生命周期里是不变的，但取一次要拿后端全局锁并整份拷贝：真机上常量缓冲暂存一帧约 3700 次，
// 每次取一遍能力表就是 3700 次加锁（和提交完成线程抢同一把锁）。这里取一次就记住，会话重开（rp_draw_reset）再取。
static nvn_gpu_caps g_caps_c; static int g_caps_ok;
static const nvn_gpu_caps *caps_cached(void) { if (!g_caps_ok) { nvn_gpu_caps_get(&g_caps_c); g_caps_ok = 1; } return &g_caps_c; }
// Bounded per-session first-cause detail; bytes do not split one cause into repeated messages.
enum { ARENA_REASON_MAX = 8 };
static struct { int rc; char why[sizeof ((nvn_gpu_err *)0)->why]; } g_arena_reasons[ARENA_REASON_MAX];
static unsigned g_arena_reason_n;
static int g_arena_reason_full;
static void arena_fail_detail(nvn_gpu_rc rc, const nvn_gpu_err *err, uint64_t bytes) {
    const char *why = err->why[0] ? err->why : nvn_gpu_ok(rc) ? "CPU mapping unavailable" : "missing backend detail";
    for (unsigned i = 0; i < g_arena_reason_n; i++)
        if (g_arena_reasons[i].rc == rc.code && !strcmp(g_arena_reasons[i].why, why)) return;
    if (g_arena_reason_n == ARENA_REASON_MAX) {
        if (!g_arena_reason_full) {
            g_arena_reason_full = 1;
            nvn_plat_log(NVN_PLAT_LOG_RED, "draw", "arena_alloc_failed：后续不同原因详情省略（最多8）");
        }
        return;
    }
    g_arena_reasons[g_arena_reason_n].rc = rc.code;
    snprintf(g_arena_reasons[g_arena_reason_n++].why, sizeof g_arena_reasons[0].why, "%s", why);
    char line[256];
    snprintf(line, sizeof line, "arena_alloc_failed rc=%d bytes=%llu why=%.160s", rc.code,
             (unsigned long long)bytes, why);
    nvn_plat_log(NVN_PLAT_LOG_RED, "draw", line);
}

static uint64_t dt_now(void);   // 计时器（定义在文件后部）
static int g_btm;   // 暂存分段计时开关（跟 nvk_bind_tm 一起，见 rp_bind_tm 那边）
static uint64_t g_bt[8];   // 分段计时累加（正式定义在后面，这里先给暂存路径用）
static int alloc_copy(nvn_gpu_cmd c, uint64_t n, const void *src, nvn_gpu_slice *sl) {
    const uint32_t align = caps_cached()->min_ubo_align ? caps_cached()->min_ubo_align : 256;
    nvn_gpu_err err = {0};
    const uint64_t a0 = g_btm ? dt_now() : 0;
    nvn_gpu_rc rc = nvn_gpu_arena_alloc(c, n, align, sl, &err);
    const uint64_t a1 = g_btm ? dt_now() : 0;
    if (!nvn_gpu_ok(rc) || !sl->cpu) { arena_fail_detail(rc, &err, n); return 0; }
    if (src) memcpy(sl->cpu, src, (size_t)n); else memset(sl->cpu, 0, (size_t)n);
    if (a0) { const uint64_t a2 = dt_now(); g_bt[6] += a1 - a0; g_bt[7] += a2 - a1; }
    return 1;
}

// 按脏位少发绑定（对照 compat/tree/src/Ryujinx.Graphics.Vulkan/DescriptorSetUpdater.cs：只有变过的那一组才重建）。
// 后端按「缓冲 + 偏移 + 长度」判一组描述符变没变，所以核心要做到：内容没变的块，给后端的就是同一片暂存。两层：
//   ① 段内切片表：同一段里同一个游戏地址区间的常量缓冲、同一个程序的自带常量段，只在暂存区落一份。
//      一次提交里游戏缓冲的内容是定的（真机上 GPU 在提交之后才读，游戏不能在这期间改），例外只有回放时在 CPU 上
//      改缓冲的三个命令，它们改 rp_cpu_write_gen，表按它失效。
//   ② 上一笔的绑定表：这个阶段的绑定状态没变（rp_args_dirty 没立）、程序和 base instance 也没变，非贴图项直接沿用上一笔的。
extern uint64_t rp_cpu_write_gen;   // rp_buf.c
enum { SLC_N = 64 };
static struct {
    nvn_gpu_cmd c; uint64_t seg, gen, k0, k1; nvn_gpu_slice sl;
    // 写入记号（开关 nvk_slc_gen，缺省开）：这份片拷好之后，来源那一段再没被这三个改缓冲的命令写过 → 内容没变，可以直接用回这一片。
    // 全局写代号一被任何一处写入推进，原先这张表就整体作废；真机上常量缓冲一帧暂存 3700 次、共 9.2 MB，
    // 其中大部分（每帧固定的那几块）内容其实没变，靠逐区间判定把它们认回来。
    uint64_t wmark;
} g_slc[SLC_N];
// stable=1：内容与游戏内存无关的片（程序自带的常量段 C1）：同一段内一直有效，不看全局写代号。
// 不看写代号是关键：回放里每改一次游戏缓冲内容全局写代号就加一，原先 C1 片也跟着作废（一帧约 2200 次、
// 每笔每阶段各一份 256 字节）——内容根本不会变，白分配白拷贝。
static int slc_get_x(nvn_gpu_cmd c, uint64_t k0, uint64_t k1, const void *src, nvn_gpu_slice *sl, int stable) {
    uint64_t h = k0 * 0x9E3779B97F4A7C15ull ^ k1; uint32_t i = (uint32_t)(h >> 40) & (SLC_N - 1);
    uint64_t sg = rp_seg_serial();
    static int genv = -1; static unsigned gk;
    if (genv < 0 || !(++gk & 255)) genv = nvn_plat_flag("nvk_slc_gen", 1) != 0;
    const int same = g_slc[i].c == c && g_slc[i].seg == sg && g_slc[i].k0 == k0 && g_slc[i].k1 == k1;
    if (same && (stable ? g_slc[i].gen == UINT64_MAX : g_slc[i].gen == rp_cpu_write_gen)) { *sl = g_slc[i].sl; rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1); return 1; }
    // 写代号被别处推进了不一定真变：按「这一段在拷好之后有没有被改缓冲的命令写过」再判一次
    if (same && !stable && genv && g_slc[i].wmark && src && !rp_write_touched(g_slc[i].wmark, (uint64_t)(uintptr_t)src, (uint32_t)k1)) {
        g_slc[i].gen = rp_cpu_write_gen;   // 内容确认没变：把这份片重新挂到当前代号上
        *sl = g_slc[i].sl; rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1); return 1;
    }
    if (!alloc_copy(c, k1 & 0xffffffffu, src, sl)) return 0;
    g_slc[i].c = c; g_slc[i].seg = sg; g_slc[i].gen = stable ? UINT64_MAX : rp_cpu_write_gen; g_slc[i].k0 = k0; g_slc[i].k1 = k1; g_slc[i].sl = *sl;
    g_slc[i].wmark = (!stable && src) ? rp_write_mark() : 0;
    return 1;
}
static int slc_get(nvn_gpu_cmd c, uint64_t k0, uint64_t k1, const void *src, nvn_gpu_slice *sl) { return slc_get_x(c, k0, k1, src, sl, 0); }
static struct { nvn_gpu_cmd c; uint64_t seg, gen, reg_gen; const struct rp_prog *pg; uint32_t base_instance, n; int ok; uint16_t bof[RP_PROG_ENT_MAX]; nvn_gpu_binding b[RP_PROG_ENT_MAX]; } g_lb[RP_PROG_STAGES];

// B3：驱动段暂存的按内容缓存。DRV0（2.5 KB）/ DRV2（0.29 KB）的整段内容只由下面这些输入决定，
// 同一段里内容相同就复用同一片 slice，省掉每笔每阶段的 memset + 拷贝。段号（rp_seg_serial）进键：段内暂存随段回收，跨段不能复用。
// 键仍按内容整份存下来逐字节比（碰撞不会错内容）；上面那层 64 位摘要只是先筛一遍，省掉线性扫多格时的大块比较。
typedef struct { nvn_gpu_cmd c; uint64_t seg; uint32_t st, base_instance; uint64_t sb[16][2], ub[16][2]; uint32_t dim[3]; uint8_t has_ub; } drv0_key;
typedef struct { nvn_gpu_cmd c; uint64_t seg; uint32_t st; uint64_t h[32]; } drv2_key;
// 表按 64 格两路探测；键里再放一个按字段算的 64 位摘要，先比摘要再比整键。
// 真机上每帧 2292 次 DRV0 查询，原先线性扫 8 格 × 552 字节整键比较 ≈ 4.4 KB/次（一帧 10 MB 比较）。
enum { DRV_CACHE_N = 64, DRV_CACHE_WAYS = 2 };
static uint64_t drv_mix(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001B3ull; return h ^ (h >> 29); }
static uint64_t drv0_hash(const drv0_key *k) {   // 按字段算（不整块读键）：~80 次乘加
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)k->c;
    h = drv_mix(h, k->seg); h = drv_mix(h, (uint64_t)k->st << 32 | k->base_instance); h = drv_mix(h, k->has_ub);
    for (int r = 0; r < 16; r++) { h = drv_mix(h, k->sb[r][0]); h = drv_mix(h, k->sb[r][1]); }
    for (int r = 0; r < 16; r++) { h = drv_mix(h, k->ub[r][0]); h = drv_mix(h, k->ub[r][1]); }
    for (int r = 0; r < 3; r++) h = drv_mix(h, k->dim[r]);
    return h;
}
static uint64_t drv2_hash(const drv2_key *k) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)k->c;
    h = drv_mix(h, k->seg); h = drv_mix(h, k->st);
    for (int r = 0; r < 32; r++) h = drv_mix(h, k->h[r]);
    return h;
}
static struct { drv0_key k; uint64_t h; uint8_t ok; nvn_gpu_slice sl; } g_drv0c[DRV_CACHE_N]; static int g_drv0c_next;
static struct { drv2_key k; uint64_t h; uint8_t ok; nvn_gpu_slice sl; } g_drv2c[DRV_CACHE_N]; static int g_drv2c_next;
static int drv0_lookup(const drv0_key *k, nvn_gpu_slice *sl) {
    uint64_t h = drv0_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) {
        const uint32_t i = base + (uint32_t)w;
        if (g_drv0c[i].ok && g_drv0c[i].h == h && !memcmp(&g_drv0c[i].k, k, sizeof *k)) { *sl = g_drv0c[i].sl; return 1; }
    }
    return 0;
}
static void drv0_store(const drv0_key *k, const nvn_gpu_slice *sl) {
    uint64_t h = drv0_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) if (!g_drv0c[base + w].ok || !memcmp(&g_drv0c[base + w].k, k, sizeof *k)) {
        g_drv0c[base + w].k = *k; g_drv0c[base + w].h = h; g_drv0c[base + w].ok = 1; g_drv0c[base + w].sl = *sl; return; }
    uint32_t i = (uint32_t)(g_drv0c_next++ % DRV_CACHE_N);   // 这一组都占了：轮转替换
    g_drv0c[i].k = *k; g_drv0c[i].h = h; g_drv0c[i].ok = 1; g_drv0c[i].sl = *sl;
}
static int drv2_lookup(const drv2_key *k, nvn_gpu_slice *sl) {
    uint64_t h = drv2_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) {
        const uint32_t i = base + (uint32_t)w;
        if (g_drv2c[i].ok && g_drv2c[i].h == h && !memcmp(&g_drv2c[i].k, k, sizeof *k)) { *sl = g_drv2c[i].sl; return 1; }
    }
    return 0;
}
static void drv2_store(const drv2_key *k, const nvn_gpu_slice *sl) {
    uint64_t h = drv2_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) if (!g_drv2c[base + w].ok || !memcmp(&g_drv2c[base + w].k, k, sizeof *k)) {
        g_drv2c[base + w].k = *k; g_drv2c[base + w].h = h; g_drv2c[base + w].ok = 1; g_drv2c[base + w].sl = *sl; return; }
    uint32_t i = (uint32_t)(g_drv2c_next++ % DRV_CACHE_N);
    g_drv2c[i].k = *k; g_drv2c[i].h = h; g_drv2c[i].ok = 1; g_drv2c[i].sl = *sl;
}
static int g_bind_keep[RP_PROG_STAGES], g_pipe_set;   // 这一笔：阶段的绑定状态没变（rp_draw 在换管线之前取）；这一笔重设过管线
// 读数（随 nvk_gputime）：整组重建分两块——[0] 含暂存与构造的总时间、[1] 其中交给后端 nvn_gpu_bind 的时间、[2] 重建次数
static uint64_t dt_now(void);   // 计时器（定义在文件后部）
static uint64_t g_bt[8];   // [0] 总 [1] 交后端 [2] 次数 [3] 驱动段 DRV0/DRV2 [4] 常量缓冲 [5] 其它 [6] 暂存申请 [7] 内容拷贝
void rp_bind_tm_take(uint64_t *tot, uint64_t *backend, uint64_t *n) { *tot = g_bt[0]; *backend = g_bt[1]; *n = g_bt[2]; g_bt[0] = g_bt[1] = g_bt[2] = 0; }
void rp_bind_tm_take2(uint64_t *drv, uint64_t *buf, uint64_t *rest) { *drv = g_bt[3]; *buf = g_bt[4]; *rest = g_bt[5]; g_bt[3] = g_bt[4] = g_bt[5] = 0; }
void rp_bind_tm_take3(uint64_t *alloc, uint64_t *copy) { *alloc = g_bt[6]; *copy = g_bt[7]; g_bt[6] = g_bt[7] = 0; }   // 暂存申请 / 内容拷贝（见 alloc_copy）


// 调试读回用的绘制历史（见文件后部 dump_chain）
enum { HIST_N = 4096, HIST_TEX = 12, DUMP_MAX = 48 };
static struct { uint64_t target, tex[HIST_TEX]; } g_hist[HIST_N]; static uint32_t g_hist_n;
static void hist_note(uint64_t target, const uint64_t *tex, int ntex) {
    uint32_t i = g_hist_n++ % HIST_N; g_hist[i].target = target; memset(g_hist[i].tex, 0, sizeof g_hist[i].tex);
    for (int k = 0; k < ntex && k < HIST_TEX; k++) g_hist[i].tex[k] = tex[k];
}
static int g_log_left;   // 逐笔日志还剩几行（调试开关 nvk_drawlog：上屏每 600 帧给下一帧开一次，见 rp_draw_log_arm）
static int skip(const char *why, const void *rec) {
    nvn_unimpl("draw", why, rec); g_skips++;
    if (g_log_left) { g_log_left--; char b[96]; snprintf(b, sizeof b, "跳过一笔：%s", why); nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); }
    return 0;
}
void rp_draw_log_arm(int lines) { g_log_left = lines; }

// 一个阶段的全部绑定。0 = 有取不到的资源（已记账），这笔不画
// pi：程序里的阶段下标（0 顶点 / 计算，1 片元）；st：NVN 阶段号（回放状态按它取：0 顶点、1 片元、5 计算）
static nvn_gpu_buf g_disp_ind; static uint64_t g_disp_ind_off;
// 本笔绘制在开通道之前解析好的图形阶段存储缓冲（[阶段][布局项]）；只在 rp_draw 里填、在同一笔的发绑定里用
static nvn_gpu_buf g_sb_buf[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static uint64_t g_sb_off[RP_PROG_STAGES][RP_PROG_ENT_MAX];
static uint8_t g_sb_ok[RP_PROG_STAGES][RP_PROG_ENT_MAX];
// Per-draw GPU-populated driver slices. Never retained by the binding cache.
static nvn_gpu_slice g_ind_drv[RP_PROG_STAGES];
static uint8_t g_ind_drv_ok[RP_PROG_STAGES];
static void fill_graphics_drv(nvn_gpu_slice *sl, int st, uint32_t base_instance) {
    for (int r = 0; r < 16; r++) { const p_bind_buf *sb = &rp_S.ssbo[st][r]; if (!sb->addr) continue;
        uint64_t a = sb->addr; uint32_t sz = (uint32_t)sb->size;
        memcpy(sl->cpu + k_drv0_base[st] + 16 * r, &a, 8);
        memcpy(sl->cpu + k_drv0_base[st] + 16 * r + 8, &sz, 4);
    }
    float max_point = 2047.0f;
    memcpy(sl->cpu + 0x610, &max_point, 4); memcpy(sl->cpu + 0x644, &base_instance, 4);
}
static uint32_t g_disp_dim[3];   // 这次派发的组数（计算阶段的驱动段 c0 里要有）

// 两个省重开关（缺省开；正式包里 nvn_plat_flag 只返回缺省值）：
//   nvk_bind_fast：同一段里同程序 / 同 base_instance / 参数没变时整组沿用上一笔的绑定，不重建；
//   nvk_drv_memo：驱动段（DRV0 / DRV2）暂存按内容命中，不重复落 2.5 KB / 0.29 KB。
// 每 256 次重读一次，照后端 nvk_desc_memo 的读法。
static int flag_read(const char *name) {
    static struct { const char *n; int v; } f[4]; static int nf, k;
    if (!(++k & 255)) { for (int i = 0; i < nf; i++) f[i].v = -1; }
    for (int i = 0; i < nf; i++) if (!strcmp(f[i].n, name)) { if (f[i].v < 0) f[i].v = nvn_plat_flag(name, 1) != 0; return f[i].v; }
    if (nf < 4) { f[nf].n = name; f[nf].v = nvn_plat_flag(name, 1) != 0; return f[nf++].v; }
    return nvn_plat_flag(name, 1) != 0;
}

// B1 快路的前置检查：这个阶段的布局里有没有「每笔都必须在本笔重做」的项。
//   · 可写存储缓冲：每笔要 rp_post_gpu_write 登记 GPU 写回，跳过就不回写了；
//   · 间接寻址存储缓冲（UBOA）：每笔要读 UBO 里的指针、解析目标范围，内容可能刚变；
//   · 取不到的资源：完整构造会按原来的名字记账并跳过这一笔，快路不能把这一笔画出去。
static int fast_items_ok(const struct rp_prog *pg, int pi, int st, const nvn_gpu_view *tv, const nvn_gpu_samp *ts) {
    for (uint32_t i = 0; i < pg->nent[pi]; i++) {
        const nvn_gpu_layout_ent *e = &pg->ent[pi][i];
        switch (e->src_kind) {
        case NVN_RB_SBUFP: case NVN_RB_UBOP:
            if (e->access & NVN_GPU_ACC_W) return 0;
            if (i >= RP_PROG_ENT_MAX || !g_sb_ok[st][i]) return 0;
            break;
        case NVN_RB_UBOA: return 0;
        case NVN_RB_TEX: {
            // 这一笔刚解析出来的视图 / 采样器必须与上一组里的逐项相同：视图被轮转重建过就交给完整构造重发。
            const uint32_t cnt = e->count > 1 ? e->count : 1;
            if (e->count > RP_TEXARR_MAX || i >= RP_PROG_ENT_MAX) return 0;
            const uint32_t at = g_lb[pi].bof[i];
            if ((uint64_t)at + cnt > g_lb[pi].n || at >= RP_PROG_ENT_MAX) return 0;
            const nvn_gpu_binding *pb = &g_lb[pi].b[at];
            if (e->count > 1) { for (uint32_t k = 0; k < cnt; k++) if (!g_av[pi][k] || !g_as[pi][k] || pb[k].u.ts.view != g_av[pi][k] || pb[k].u.ts.samp != g_as[pi][k]) return 0; }
            else if (!tv[i] || !ts[i] || pb->u.ts.view != tv[i] || pb->u.ts.samp != ts[i]) return 0;
            break; }
        case NVN_RB_IMG: {
            if (!tv[i] || i >= RP_PROG_ENT_MAX) return 0;
            const uint32_t at = g_lb[pi].bof[i];
            if ((uint64_t)at + 1 > g_lb[pi].n || at >= RP_PROG_ENT_MAX || g_lb[pi].b[at].u.view != tv[i]) return 0;
            break; }
        default: break;
        }
    }
    return 1;
}

// B2：两个程序在某个图形阶段的绑定布局是否等同（后端按各阶段布局的 set / binding / 类别 / 数量合并管线布局）。
// 等同 = 换管线不用把这个阶段的绑定重立脏；程序本身变了由 g_lb[pi].pg 让 keep 失效，照样完整重建。
static int prog_layout_same(const struct rp_prog *a, const struct rp_prog *b, int st) {
    if (!a || !b || a->nent[st] != b->nent[st]) return 0;
    for (uint32_t i = 0; i < a->nent[st]; i++) {
        const nvn_gpu_layout_ent *x = &a->ent[st][i], *y = &b->ent[st][i];
        if (x->set != y->set || x->binding != y->binding || x->cls != y->cls || x->count != y->count) return 0;
    }
    return 1;
}
static int bind_stage_at(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, int st, uint32_t base_instance, const nvn_gpu_view *tv, const nvn_gpu_samp *ts, const void *rec);
static int bind_stage(nvn_gpu_cmd c, const struct rp_prog *pg, int st, uint32_t base_instance, const nvn_gpu_view *tv, const nvn_gpu_samp *ts, const void *rec) { return bind_stage_at(c, pg, st, st, base_instance, tv, ts, rec); }
static int bind_stage_at(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, int st, uint32_t base_instance, const nvn_gpu_view *tv, const nvn_gpu_samp *ts, const void *rec) {
    nvn_gpu_binding b[RP_PROG_ENT_MAX]; uint32_t n = 0;
    // 沿用上一笔：只限图形两个阶段；存储缓冲（要同步镜像、登记 GPU 写）和贴图（视图会轮转）每笔照取
    int keep = 0;
    {   // 逐条判「整组沿用」能不能成立，并记下失败原因（读数：真机上这一条几乎从不成立，要先看清是被哪一项否掉的）
        int why = -1;
        if (st < RP_PROG_STAGES && !g_ind_drv_ok[st] && g_bind_keep[pi] && g_lb[pi].ok) {   // #941：本笔用 GPU 写出的 DRV0 的阶段不能沿用
            if (g_lb[pi].c != c) why = RP_PERF_KEEP_F_CMD;
            else if (g_lb[pi].seg != rp_seg_serial()) why = RP_PERF_KEEP_F_SEG;
            else if (g_lb[pi].gen != rp_cpu_write_gen) why = RP_PERF_KEEP_F_GEN;
            else if (g_lb[pi].reg_gen != rp_tex_reg_gen()) why = RP_PERF_KEEP_F_REG;
            else if (g_lb[pi].pg != pg) why = RP_PERF_KEEP_F_PG;
            else if (g_lb[pi].base_instance != base_instance) why = RP_PERF_KEEP_F_BI;
            else keep = 1;
        }
        if (keep) rp_perf_add(RP_PERF_KEEP_OK, 1); else if (why >= 0) rp_perf_add(why, 1);
    }
    // B1 快路：上一笔交给后端的这一组就是这一笔要的，不重建 b[]（省掉每笔每阶段的驱动段暂存拷贝与整轮贴图视图取用）。
    // 不能沿用的：可写存储缓冲（每笔要登记 GPU 写回）、间接寻址存储缓冲（每笔要读指针重新解析）、取不到的资源（交给完整构造去记账跳过）。
    // 贴图视图由 fast_items_ok 逐项与本笔刚解析的结果比对：视图被轮转重建过就不走快路。
    // 换管线不再挡沿用：同一程序 ⇒ 同一套绑定布局（后面换管线的分支只对「布局真的变了」的阶段立脏），
    // 描述符组在 Vulkan 里也不随管线对象失效，只有布局不兼容才必须重发。真机上一帧 383 次换管线、程序却没变，
    // 全被这条挡掉了（读数见「分时（绑定）」行）。
    if (keep && flag_read("nvk_bind_fast")) {
        if (fast_items_ok(pg, pi, st, tv, ts)) { if (g_pipe_set) rp_perf_add(RP_PERF_KEEP_F_PIPESET, 1); rp_perf_add(RP_PERF_BIND_FAST, 1); return 1; }
        rp_perf_add(RP_PERF_KEEP_F_ITEMS, 1);
    }
    static int btm = -1; static unsigned btk; if (btm < 0 || !(++btk & 255)) btm = nvn_plat_flag("nvk_bind_tm", 0) != 0;
    g_btm = btm;
    const uint64_t btm0 = btm ? dt_now() : 0;
    if (st < RP_PROG_STAGES) g_lb[pi].ok = 0;
    for (uint32_t i = 0; i < pg->nent[pi]; i++) {
        const nvn_gpu_layout_ent *e = &pg->ent[pi][i];
        if (st < RP_PROG_STAGES) g_lb[pi].bof[i] = (uint16_t)n;   // 上一组的布局项 → b[] 起始下标（B1 快路逐项比对贴图视图用）
        nvn_gpu_binding *o = &b[n]; memset(o, 0, sizeof *o); o->ent = (uint16_t)i; o->cls = e->cls;
        nvn_gpu_slice sl;
        if (keep && e->src_kind != NVN_RB_SBUFP && e->src_kind != NVN_RB_TEX && e->src_kind != NVN_RB_IMG) { *o = g_lb[pi].b[n]; n++; continue; }
        switch (e->src_kind) {
            case NVN_RB_SBUFP: case NVN_RB_UBOP: {   // 存储缓冲：取地址、绑池镜像里的那一段；可写的登记一次 GPU 写（完成后回写游戏内存）。两种来源只差在哪张表给地址：
                // SBUFP 按 NVN 存储缓冲槽（rp_S.ssbo）；UBOP 按计算阶段的用户 UBO 槽（rp_S.ubo；反射映射 NVK_STORAGE_UBO_PTR），段的范围就是这个 UBO 绑定的地址与大小
                const p_bind_buf *sb = e->src_reg < 16 ? (e->src_kind == NVN_RB_UBOP ? &rp_S.ubo[st][e->src_reg] : &rp_S.ssbo[st][e->src_reg]) : NULL;
                if (!sb || !sb->addr || !sb->size) return skip("storage_buffer_unbound", rec);
                nvn_gpu_buf gb; uint64_t go;
                if (st < RP_PROG_STAGES && i < RP_PROG_ENT_MAX && g_sb_ok[st][i]) { gb = g_sb_buf[st][i]; go = g_sb_off[st][i]; }   // 开通道前已同步（见 rp_draw 开头）
                else if (!rp_buf_resolve(c, sb->addr, sb->size, &gb, &go)) return skip("storage_buffer_unresolved", rec);
                if (e->access & NVN_GPU_ACC_W) rp_post_gpu_write(sb->addr, sb->size);
                o->u.b.buf = gb; o->u.b.off = go; o->u.b.len = sb->size; break; }
            case NVN_RB_UBOA: {   // 计算存储缓冲来自「用户 UBO 槽 r 内 src_off 处的 8 字节 guest 裸指针」（map 3，NVK_STORAGE_UBO_ADDRESS）：
                // 指针字节必须当前：先按写的来源解析那 8 字节（池镜像里 GPU 写过的部分不会被旧 guest 字节盖掉），
                // 再从镜像的 CPU 映射读数；目标长度按所在池的余量，并夹到 SSBO 描述符范围上限。
                const p_bind_buf *ub = e->src_reg < 16 ? &rp_S.ubo[st][e->src_reg] : NULL;
                if (!ub || !ub->addr || !ub->size) return skip("uniform_buffer_unbound", rec);
                if ((uint64_t)e->src_off + 8 > ub->size) return skip("uniform_buffer_indirect_offset_outside", rec);
                if (e->access & NVN_GPU_ACC_W) return skip("compute_storage_indirect_writable_unknown", rec);   // 长度未知的可写绑定会把回写扩到相邻分配：准确拒绝
                uint64_t paddr = ub->addr + e->src_off;
                nvn_gpu_buf gb; uint64_t go;
                if (!rp_buf_resolve(c, paddr, 8, &gb, &go)) return skip("uniform_buffer_unresolved", rec);
                uint8_t *cpu = nvn_gpu_buf_cpu(gb);
                if (!cpu) return skip("uniform_buffer_not_host_readable", rec);
                uint64_t target = 0; memcpy(&target, cpu + go, 8);
                target = nvn_ga2ca(target);
                if (!target) return skip("compute_storage_indirect_null", rec);
                const uint8_t *pmem = NULL; uint64_t psz = 0;
                if (!rp_pool_find(target, 1, &pmem, &psz)) return skip("storage_buffer_outside_pools", rec);
                uint64_t extent = psz - (target - (uint64_t)(uintptr_t)pmem);
                { nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);   // 本函数作用域里没有外层 caps：这里自己取一次
                  if (cp.max_ssbo_range && extent > cp.max_ssbo_range) extent = cp.max_ssbo_range; }
                if (!extent) return skip("storage_buffer_indirect_empty", rec);
                if (!rp_buf_resolve(c, target, extent, &gb, &go)) return skip("storage_buffer_unresolved", rec);
                o->u.b.buf = gb; o->u.b.off = go; o->u.b.len = extent; break; }
            case NVN_RB_SUPPORT: {
                uint32_t flags = (rp_depth_zero() ? 0u : 1u) | 2u;   // 位 0：深度 −1..1；位 1：base vertex / instance 取内建（后端基线要求 shaderDrawParameters）
                // 这块内容只取决于长度与这两位：同一次提交的同一段里填过一份就接着用（每笔每阶段新建一份 2.6 KB 是回放里的固定开销）
                static struct { nvn_gpu_cmd c; uint64_t sub; uint32_t bytes, flags; nvn_gpu_slice sl; } sup[4]; static int sup_next;
                { int hit = -1; uint64_t sub = rp_seg_serial();
                  for (int k = 0; k < 4; k++) if (sup[k].c == c && sup[k].sub == sub && sup[k].bytes == pg->support_bytes && sup[k].flags == flags) hit = k;
                  if (hit >= 0) { o->u.b.buf = sup[hit].sl.buf; o->u.b.off = sup[hit].sl.off; o->u.b.len = pg->support_bytes; break; } }
                if (!alloc_copy(c, pg->support_bytes, NULL, &sl)) return skip("arena_alloc_failed", rec);
                if (SB_S2A_FLAGS + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_S2A_FLAGS, &flags, 4);
                float one = 1.0f;
                if (SB_S2A_POINT + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_S2A_POINT, &one, 4);
                for (uint32_t k = 0; k < SB_RENDER_SCALE_COUNT; k++) {
                    if (SB_RENDER_SCALE + k * 16 + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_RENDER_SCALE + k * 16, &one, 4);
                    if (SB_RENDER_SCALE_Y + k * 16 + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_RENDER_SCALE_Y + k * 16, &one, 4);
                }
                sup[sup_next].c = c; sup[sup_next].sub = rp_seg_serial(); sup[sup_next].bytes = pg->support_bytes; sup[sup_next].flags = flags; sup[sup_next].sl = sl; sup_next = (sup_next + 1) & 3;
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = pg->support_bytes; break; }
            case NVN_RB_DRV0: {
                if (st < RP_PROG_STAGES && g_ind_drv_ok[st]) {   // #941：间接绘制的 DRV0 由 GPU 写出，直接用那一片，不走内容缓存
                    o->u.b.buf = g_ind_drv[st].buf; o->u.b.off = g_ind_drv[st].off; o->u.b.len = DRV0_BYTES; break;
                }
                const uint64_t drv0_t0 = btm ? dt_now() : 0;
                drv0_key k; memset(&k, 0, sizeof k);
                k.c = c; k.seg = rp_seg_serial(); k.st = (uint32_t)st; k.base_instance = base_instance;
                for (int r = 0; r < 16; r++) { k.sb[r][0] = rp_S.ssbo[st][r].addr; k.sb[r][1] = rp_S.ssbo[st][r].size; }
                int cacheable = 1;
                if (st == 5) {   // 计算阶段的驱动段（nvn_mtl.m @9897）：0x200 派发组数，0x210 + 16r 常量缓冲表，0x310 + 16r 存储缓冲表（各项：地址 64 位、长度 32 位）
                    if (g_disp_ind) cacheable = 0;   // 间接派发：0x200 是从缓冲里拷过来的，不能复用旧内容
                    else { k.has_ub = 1;
                        for (int r = 0; r < 16; r++) { k.ub[r][0] = rp_S.ubo[5][r].addr; k.ub[r][1] = rp_S.ubo[5][r].size; }
                        memcpy(k.dim, g_disp_dim, sizeof k.dim); }
                }
                if (cacheable && flag_read("nvk_drv_memo") && drv0_lookup(&k, &sl)) {   // 同一段里内容相同：复用上次那片
                    rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1);
                    o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV0_BYTES; break;
                }
                if (!alloc_copy(c, DRV0_BYTES, NULL, &sl)) return skip("arena_alloc_failed", rec);
                if (st == 5) {
                    memcpy(sl.cpu + 0x200, g_disp_dim, 12);
                    if (g_disp_ind) nvn_gpu_copy_buf(c, g_disp_ind, g_disp_ind_off, sl.buf, sl.off + 0x200, 12);
                    for (int r = 0; r < 16; r++) { uint64_t a = rp_S.ubo[5][r].addr; uint32_t sz = (uint32_t)rp_S.ubo[5][r].size; memcpy(sl.cpu + 0x210 + 16 * r, &a, 8); memcpy(sl.cpu + 0x210 + 16 * r + 8, &sz, 4); }
                    for (int r = 0; r < 16; r++) { uint64_t a = rp_S.ssbo[5][r].addr; uint32_t sz = (uint32_t)rp_S.ssbo[5][r].size; memcpy(sl.cpu + 0x310 + 16 * r, &a, 8); memcpy(sl.cpu + 0x310 + 16 * r + 8, &sz, 4); }
                } else {
                    for (int r = 0; r < 16; r++) { const p_bind_buf *sb = &rp_S.ssbo[st][r]; if (!sb->addr) continue;
                        uint64_t a = sb->addr; uint32_t sz = (uint32_t)sb->size; memcpy(sl.cpu + k_drv0_base[st] + 16 * r, &a, 8); memcpy(sl.cpu + k_drv0_base[st] + 16 * r + 8, &sz, 4); }
                    float max_point = 2047.0f; memcpy(sl.cpu + 0x610, &max_point, 4); memcpy(sl.cpu + 0x644, &base_instance, 4);
                }
                if (cacheable && flag_read("nvk_drv_memo")) drv0_store(&k, &sl);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV0_BYTES; if (btm) g_bt[3] += dt_now() - drv0_t0; break; }
            case NVN_RB_DRV2: {   // 驱动的贴图句柄表：贴图槽 s 的句柄（贴图号 | 采样器号 << 20）在第 4 + s 个 8 字节项
                enum { DRV2_BYTES = (RP_HANDLE_TABLE_FIRST_SLOT + 32) * 8 };
                drv2_key k; memset(&k, 0, sizeof k); k.c = c; k.seg = rp_seg_serial(); k.st = (uint32_t)st;
                for (int s = 0; s < 32; s++) k.h[s] = rp_S.tex[st][s];
                if (flag_read("nvk_drv_memo") && drv2_lookup(&k, &sl)) {
                    rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1);
                    o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV2_BYTES; break;
                }
                if (!alloc_copy(c, DRV2_BYTES, NULL, &sl)) return skip("arena_alloc_failed", rec);
                for (int s = 0; s < 32; s++) { uint64_t h = rp_S.tex[st][s]; memcpy(sl.cpu + (RP_HANDLE_TABLE_FIRST_SLOT + s) * 8, &h, 8); }
                if (flag_read("nvk_drv_memo")) drv2_store(&k, &sl);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV2_BYTES; break; }
            case NVN_RB_C1: {
                if (!(pg->c1_ok >> pi & 1)) return skip("c1_segment_not_captured", rec);
                rp_perf_add(RP_PERF_UBO_C1_N, 1);
                if (!slc_get_x(c, (uint64_t)(uintptr_t)pg ^ (uint64_t)(pi + 1) << 60, 0x100, pg->c1[pi], &sl, 1)) return skip("arena_alloc_failed", rec);   // 程序自带常量段：内容恒定，段内一直有效
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = 0x100; break; }
            case NVN_RB_CBUF: {
                const uint64_t cbuf_t0 = btm ? dt_now() : 0;
                const p_bind_buf *ub = &rp_S.ubo[st][e->src_reg];
                if (!ub->addr || !ub->size) return skip("uniform_buffer_unbound", rec);
                // 常量缓冲一律拷进本段的暂存区再绑：就是编码这一刻的内容（按序的 OP_UPDATE_UB 之后再用自然是新值），
                // 不经池镜像，也就没有通道内的传输。地址必须在登记过的池里。
                if (!rp_pool_find(ub->addr, ub->size, NULL, NULL)) return skip("uniform_buffer_outside_pools", rec);
                if (ub->size > 0xffffffffull) return skip("uniform_buffer_too_large", rec);
                rp_perf_add(RP_PERF_UBO_N, 1); rp_perf_add(RP_PERF_UBO_KB, ub->size >> 10);
                if (!slc_get(c, ub->addr, ub->size, (const void *)(uintptr_t)ub->addr, &sl)) return skip("arena_alloc_failed", rec);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = ub->size; if (btm) g_bt[4] += dt_now() - cbuf_t0; break; }
            case NVN_RB_IMG: {
                if (st != 5 || e->cls != NVN_GPU_RES_IMAGE || e->count > 1) return skip("compute_image_shape", rec);
                if (!tv[i]) return skip("compute_image_unresolved", rec);
                o->u.view = tv[i]; break; }
            case NVN_RB_TEX: {
                if (e->count > 1) {   // 贴图数组：逐项绑（视图与采样器已在开通道前取好，放在 g_av / g_as）
                    if (n + e->count > RP_PROG_ENT_MAX) return skip("too_many_bindings", rec);
                    for (uint32_t k = 0; k < e->count; k++) {
                        if (!g_av[pi][k]) return skip("texture_slot_unresolved", rec);
                        if (!g_as[pi][k]) return skip("sampler_slot_unresolved", rec);
                        nvn_gpu_binding *ok = &b[n + k]; memset(ok, 0, sizeof *ok); ok->ent = (uint16_t)i; ok->cls = e->cls; ok->elem = k;
                        ok->u.ts.view = g_av[pi][k]; ok->u.ts.samp = g_as[pi][k];
                    }
                    n += e->count - 1;   // 循环末尾再加 1
                    break;
                }
                if (!tv[i]) return skip("texture_slot_unresolved", rec);
                if (!ts[i]) return skip("sampler_slot_unresolved", rec);
                o->u.ts.view = tv[i]; o->u.ts.samp = ts[i]; break; }
            default: return skip("binding_source_unknown", rec);
        }
        n++;
    }
    if (st < RP_PROG_STAGES) {
        // 和上一笔逐项相同、管线也没重设：后端手里的就是这一组，不用再发
        const int same = keep && g_lb[pi].n == n && !memcmp(g_lb[pi].b, b, (size_t)n * sizeof *b);
        g_lb[pi].c = c; g_lb[pi].seg = rp_seg_serial(); g_lb[pi].gen = rp_cpu_write_gen; g_lb[pi].reg_gen = rp_tex_reg_gen(); g_lb[pi].pg = pg; g_lb[pi].base_instance = base_instance; g_lb[pi].n = n; g_lb[pi].ok = !g_ind_drv_ok[st];   // #941：用 GPU 写出 DRV0 的这一组不留作下一笔沿用
        if (same) { rp_perf_add(RP_PERF_BIND_SKIP, 1); return 1; }
        memcpy(g_lb[pi].b, b, (size_t)n * sizeof *b);
    }
    { const uint64_t bb = btm ? dt_now() : 0;
      nvn_gpu_bind(c, st == 5 ? NVN_GPU_SH_CS : (nvn_gpu_stage)st, b, n);
      if (btm) { const uint64_t be = dt_now(); g_bt[1] += be - bb; g_bt[2]++; g_bt[0] += be - btm0; } }
    return 1;
}

// 绘制时的动态状态（对照 nvn_mtl.m @12740–12776）。返回 0 = 裁剪矩形与目标没有交集：这笔什么都画不出，不发（iOS 同样跳过，不算未接）
static int fill_dyn(nvn_gpu_dyn *d, uint32_t tw, uint32_t th, const void *rec) {
    memset(d, 0, sizeof *d);
    d->nvp = 1;
    if (rp_S.has_vp) { memcpy(d->vp[0], rp_S.vp[0], 16); } else { d->vp[0][2] = (float)tw; d->vp[0][3] = (float)th; }
    d->vp[0][4] = rp_S.dr[0]; d->vp[0][5] = rp_S.dr[1];
    // 视口重排：只认「y 取反」（等于视口上下翻，正面判定在它之后做，绕序状态不用动）；其余重排照没重排画，记一笔
    { uint32_t sw; memcpy(&sw, rp_S.vp_sw[0], 4);
      if (sw == 0x100) { d->vp[0][1] += d->vp[0][3]; d->vp[0][3] = -d->vp[0][3]; }
      else if (sw) nvn_unimpl("gpu", "viewport_swizzle", rec); }
    // 裁剪矩形夹到目标范围内（原生接口不收负的起点；宽高是游戏给的任意数）
    if (rp_R.has_sc) {
        int64_t x0 = rp_R.sc.x < 0 ? 0 : rp_R.sc.x, y0 = rp_R.sc.y < 0 ? 0 : rp_R.sc.y, x1 = (int64_t)rp_R.sc.x + rp_R.sc.w, y1 = (int64_t)rp_R.sc.y + rp_R.sc.h;
        if (x1 > (int64_t)tw) x1 = tw;
        if (y1 > (int64_t)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) return 0;
        d->has_sc = 1; d->sc[0] = (int32_t)x0; d->sc[1] = (int32_t)y0; d->sc[2] = (int32_t)(x1 - x0); d->sc[3] = (int32_t)(y1 - y0);
    }
    d->origin_ll = (uint8_t)rp_origin_lower_left(); d->depth_clamp = (uint8_t)(rp_S.depth_clamp != 0);
    d->po = rp_S.po;
    memcpy(d->st_vmask, rp_S.st_vmask, sizeof d->st_vmask); memcpy(d->st_wmask, rp_S.st_wmask, sizeof d->st_wmask); memcpy(d->st_ref, rp_S.st_ref, sizeof d->st_ref);
    memcpy(d->blend_color, rp_S.blend_color, sizeof d->blend_color);
    d->ds = rp_S.ds; d->poly = rp_S.poly;
    return 1;
}

// 间接计数多绘（MultiDraw*IndirectCount）：前端把一次调用录成 max 条间接绘制，每条带计数地址 mdi_cnt 和序号 mdi_i
//   （hle_nvn.c；iOS 没有 GPU 读计数的绘制，回放时在 GPU 上把序号 ≥ 计数的记录清零，nvn_mtl.m mdx_run）。
//   Vulkan 有 vkCmdDraw*IndirectCount：在第 0 条处顺着命令流往后数出同一组（同操作码、同计数地址、序号连续、记录地址等步长，
//   跨块跟 OP_JUMP），整组一次交给后端由 GPU 读计数；后面同组的记录认出来直接跳过。数不出整组（中间夹了别的记录）的不画半组。
static uint64_t g_mdi_cnt; static int32_t g_mdi_next, g_mdi_end;
// OP_MULTI_DRAW（超过 1,024 条的一次调用，前端整段录一条）：组大小与步长由记录直接给，不往后数
static uint32_t g_mdi_force_n; static uint64_t g_mdi_force_stride;
void rp_multi_draw(nvn_gpu_cmd c, const p_multi_draw *m, const void *rec) {
    if (!c || !m || m->max <= 0 || m->stride <= 0) { skip("multi_draw_invalid", rec); return; }
    g_mdi_force_n = (uint32_t)m->max; g_mdi_force_stride = (uint64_t)m->stride;
    if (m->itype >= 0) {
        p_draw_idx d; memset(&d, 0, sizeof d);
        d.prim = m->prim; d.itype = m->itype; d.instances = 1; d.ibuf = m->ibuf; d.ind = m->ind; d.mdi_cnt = m->cnt; d.mdi_i = 0;
        rp_draw(c, NULL, &d, rec);
    } else {
        p_draw d; memset(&d, 0, sizeof d);
        d.prim = m->prim; d.instances = 1; d.ind = m->ind; d.mdi_cnt = m->cnt; d.mdi_i = 0;
        rp_draw(c, &d, NULL, rec);
    }
    g_mdi_force_n = 0;
}
static uint32_t mdi_group(const uint8_t *rec, uint64_t cnt, uint64_t ind0, uint64_t *stride) {
    op_hdr h0; memcpy(&h0, rec, sizeof h0);
    const uint8_t *p = rec + h0.size; uint32_t k = 1; *stride = 0;
    for (int guard = 0; guard < 8192 && k < 4096; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7)) break;
        if (h.op == OP_JUMP) { const uint8_t *t; memcpy(&t, p + 8, 8); if (!t) break; p = t; continue; }
        if (h.op != h0.op) break;
        uint64_t c2, ind; int32_t i2;
        if (h.op == OP_DRAW_IDX) { p_draw_idx t; const p_draw_idx *d = draw_idx_rec(p + 8, h.size, &t); c2 = d->mdi_cnt; i2 = d->mdi_i; ind = d->ind; }
        else { p_draw t; const p_draw *d = draw_rec(p + 8, h.size, &t); c2 = d->mdi_cnt; i2 = d->mdi_i; ind = d->ind; }
        if (c2 != cnt || i2 != (int32_t)k) break;
        if (k == 1) { if (ind <= ind0) break; *stride = ind - ind0; }
        else if (ind != ind0 + (uint64_t)k * *stride) break;
        k++; p += h.size;
    }
    return k;
}

// OP_DRAW / OP_DRAW_IDX 共用。di 非空 = 带索引
// 分时：一笔绘制里各步各花多久（累计到 rp_perf，上屏那边每 5 秒报一次）
static uint64_t dt_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#define DT(step) do { uint64_t n_ = dt_now(); rp_perf_add(step, n_ - dt_); dt_ = n_; } while (0)
void rp_draw(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di, const void *rec) {
    if (!c) return;
    memset(g_ind_drv_ok, 0, sizeof g_ind_drv_ok);
    uint64_t dt_ = dt_now();
    uint32_t prim = (uint32_t)(di ? di->prim : dr->prim);
    const uint64_t ind = di ? di->ind : dr->ind;
    const uint64_t mcnt = di ? di->mdi_cnt : dr->mdi_cnt;
    uint32_t mdi_n = 0; uint64_t mdi_stride = 0;
    if (mcnt) {
        const int32_t mi = di ? di->mdi_i : dr->mdi_i;
        if (mi != 0) {   // 同组后面的记录：第 0 条已整组交出
            if (mcnt == g_mdi_cnt && mi == g_mdi_next && mi < g_mdi_end) { g_mdi_next++; return; }
            skip("indirect_count_group_unavailable", rec); return;
        }
        if (g_mdi_force_n) { mdi_n = g_mdi_force_n; mdi_stride = g_mdi_force_stride; }
        else mdi_n = mdi_group(rec, mcnt, ind, &mdi_stride);
        g_mdi_cnt = mcnt; g_mdi_next = 1; g_mdi_end = (int32_t)mdi_n;
        if (mdi_n == 1) mdi_stride = di ? 20 : 16;
        if (!ind || mdi_stride < (uint64_t)(di ? 20 : 16) || (mdi_stride & 3) || mdi_stride > 0xFFFFFFFFu) { skip("indirect_count_group_unavailable", rec); return; }
        nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);   // 设备没开 drawIndirectCount 就不交（交了后端整段判失败），照旧报名跳过
        if (!(caps.caps & NVN_GPU_CAP_INDIRECT_COUNT)) { skip("indirect_count_no_device_support", rec); return; }
    }
    if (ind && (prim == 2 || prim == 7 || prim == 8 || prim == 9 || (di && di->itype == 0))) {
        skip("indirect_topology_or_index_conversion", rec); return;
    }
    if (dr && dr->tfc) { skip("transform_feedback_count", rec); return; }
    if (rp_S.tf_on) { skip("transform_feedback_active", rec); return; }
    if (!(prim <= 9)) { char nm[24]; snprintf(nm, sizeof nm, "prim_%u", prim); skip(nm, rec); return; }
    // 必须在图元展开前判原始输入，避免 GS 看到不同数量或顺序的图元。
    if ((rp_shader_graphics_stages() & 4u) && prim != 0 && prim != 1 && prim != 4) {
        skip("geometry_topology_unimplemented", rec); return;
    }
    uint32_t count = (uint32_t)(di ? di->count : dr->count);
    if (!count && !ind) return;
    if (ind) count = 0;   // GPU parameters, not stale direct fields, own the draw count.
    // 没有原生对应的图元：换成能画的图元号，要展开索引的记下展开方式（quads），索引在取资源那一步展开
    int restart = di && rp_S.restart.on;
    int quads = EXP_NONE;
    if (prim == 7) { quads = EXP_QUADS; prim = 4; }
    else if (prim == 2) { quads = EXP_LOOP; prim = 1; }
    else if (prim == 9) prim = 6;   // 多边形 = 三角扇（首点为扇心，绕序同扇）
    else if (prim == 8) {           // 四边形带 = 三角带；零头 1 个点在三角带里会多画一个三角形，丢掉
        if (restart) { quads = EXP_QSTRIP; prim = 4; }
        else { count &= ~1u; if (count < 4) return; prim = 5; }
    }
    const p_set_rt *rt = rp_pass_target();
    if (!rt) { skip("draw_without_target", rec); return; }
    // 程序：管线状态或任一阶段的绑定变过才重新取（贴图槽里换了不同类型的贴图会换变体）
    if (prim != g_prim || quads != g_quads) { g_prim = prim; g_quads = quads; rp_pso_dirty = 1; }
    { int face = (rp_origin_lower_left() != 0) | (s2a_nvn_rast_flip != 0) << 1;   // 窗口原点与光栅翻转位不在状态块里，但进管线描述（正面绕序）：变了就是换管线
      if (face != g_face) { g_face = face; rp_pso_dirty = 1; } }   // 图元在绘制记录里，不在状态块里：换了图元就是换管线
    int args_dirty = rp_args_dirty[0] || rp_args_dirty[1] || rp_args_dirty[2];
    if (!g_prog || rp_pso_dirty || args_dirty) {
        struct rp_shader_req q; memset(&q, 0, sizeof q); q.prim = prim; q.rt_count = rt->n;
        for (int i = 0; i < rt->n && i < 8; i++) if (rt->color[i]) q.rt_format[i] = (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format);
        const struct rp_prog *np = NULL;
        int sr = rp_shader_resolve(&q, &np);
        if (sr) { skip(sr > 0 ? "program_not_ready" : "program_unresolved", rec); g_prog = NULL; return; }
        if (np != g_prog) { g_prog = np; rp_pso_dirty = 1; }
    }
    DT(RP_PERF_D_PROG);
    // 先把这笔要用的资源都取好：池镜像同步、贴图上传是传输，不能出现在通道里，必须在开通道之前做完
    nvn_gpu_buf vb_buf[16]; uint64_t vb_off[16]; uint32_t streams = 0;
    for (int i = 0; i < rp_S.nattr && i < 16; i++) if (g_prog->attr_mask >> i & 1) streams |= 1u << rp_S.attr[i].stream;
    {   // 一次抢锁解析全部顶点流（逐段各抢一次时这把锁和完成线程互抢，重叠档能吃到 13.7–16.6 毫秒/帧）
        uint64_t vaddr[16], vlen[16], voff[16]; unsigned char vok[16]; int nv = 0;
        for (int i = 0; i < 16; i++) if (streams >> i & 1) {
            uint64_t va = nvn_ga2ca(rp_S.vbuf[i].address), vn = rp_S.vbuf[i].size;   // 记录里的顶点缓冲地址是 GPU 地址
            if (!va || !vn) { skip("vertex_stream_unbound", rec); return; }
            vaddr[nv] = va; vlen[nv] = vn; nv++;
        }
        if (nv) {
            nvn_gpu_buf vbuf[16];
            rp_buf_resolve_multi(nv, vaddr, vlen, vbuf, voff, vok);
            int k = 0;
            for (int i = 0; i < 16; i++) if (streams >> i & 1) {
                if (!vok[k]) { skip("vertex_buffer_unresolved", rec); return; }
                vb_buf[i] = vbuf[k]; vb_off[i] = voff[k]; k++;
            }
        }
    }
    // 存储缓冲同样要在开通道之前同步：同一次提交里第二次同步这块池页时要录成传输、结束通道（rp_xfer_cmd），
    // 放到发绑定时才做，通道会在设了管线之后被结束，这一笔的绑定与绘制落在通道外，后端置粘滞、整段作废
    // （#697 Mali 真机：「bind 在通道外（非计算管线）」）。这里先同步一遍，发绑定时同一段已同步过，走快路径、不再结束通道。
    // 解析失败不在这里报：发绑定时同样的解析会按原来的名字记未接。
    // 结果留给发绑定直接用（g_sb_pre），发绑定时不再解析：即使这块页在开通道之后又被别的游戏线程写脏，也不会在通道里再同步一次。
    memset(g_sb_ok, 0, sizeof g_sb_ok);
    {   // 一次抢锁把这一笔的存储缓冲全解析掉（同顶点流：逐段各抢一次锁会和完成线程互抢）
        enum { SB_BATCH = 64 };
        uint64_t ba[SB_BATCH], bl[SB_BATCH], bo[SB_BATCH]; nvn_gpu_buf bb[SB_BATCH]; unsigned char bok[SB_BATCH];
        int bs[SB_BATCH], bi[SB_BATCH], nb = 0;
        for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st] && i < RP_PROG_ENT_MAX; i++) {
            const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
            if (e->src_kind != NVN_RB_SBUFP && e->src_kind != NVN_RB_UBOP) continue;
            const p_bind_buf *sb = e->src_reg < 16 ? (e->src_kind == NVN_RB_UBOP ? &rp_S.ubo[st][e->src_reg] : &rp_S.ssbo[st][e->src_reg]) : NULL;
            if (!sb || !sb->addr || !sb->size) continue;
            if (nb < SB_BATCH) { bs[nb] = st; bi[nb] = (int)i; ba[nb] = sb->addr; bl[nb] = sb->size; nb++; }
            else if (rp_buf_resolve(c, sb->addr, sb->size, &g_sb_buf[st][i], &g_sb_off[st][i])) g_sb_ok[st][i] = 1;   // 超出一批的照旧逐段解析
        }
        if (nb) {
            rp_buf_resolve_multi(nb, ba, bl, bb, bo, bok);
            for (int k = 0; k < nb; k++) if (bok[k]) { g_sb_buf[bs[k]][bi[k]] = bb[k]; g_sb_off[bs[k]][bi[k]] = bo[k]; g_sb_ok[bs[k]][bi[k]] = 1; }
        }
    }
    nvn_gpu_draw_args a; memset(&a, 0, sizeof a);
    if (di && (di->itype < 0 || di->itype > 2)) { skip("index_type_invalid", rec); return; }
    if (ind) {
        const uint64_t isize = di ? 20 : 16, ispan = mdi_n ? (uint64_t)(mdi_n - 1) * mdi_stride + isize : isize;
        if (!rp_buf_resolve(c, ind, ispan, &a.ind, &a.ind_off) || (a.ind_off & 3)) {
            skip("indirect_buffer_unresolved", rec); return;
        }
        a.max_draws = 1; a.stride = (uint32_t)isize;
        if (mdi_n) {   // 整组：GPU 读计数，画 min(计数, mdi_n) 条
            if (!rp_buf_resolve(c, mcnt, 4, &a.cnt, &a.cnt_off) || (a.cnt_off & 3)) { skip("indirect_count_unresolved", rec); return; }
            a.max_draws = mdi_n; a.stride = (uint32_t)mdi_stride;
        }
        if (di) {
            const uint8_t *mem = NULL; uint64_t bytes = 0;
            const uint64_t isz = di->itype == 1 ? 2 : 4;
            // The GPU owns firstIndex/indexCount. Resolve the registered binding tail,
            // not a CPU-derived count, without reading index or argument bytes.
            if (!rp_pool_find(di->ibuf, isz, &mem, &bytes)) { skip("index_buffer_unresolved", rec); return; }
            uint64_t off = di->ibuf - (uint64_t)(uintptr_t)mem;
            if (off > bytes || !rp_buf_resolve(c, di->ibuf, bytes - off, &a.ibuf, &a.ioff) || (a.ioff & (isz - 1))) {
                skip("index_buffer_unresolved", rec); return;
            }
            a.itype = (uint32_t)di->itype;
        }
    } else if (quads) {
        uint32_t isz = di ? (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) : 0;
        if (quads == EXP_QUADS && count / 4 == 0) return;   // 不足一个四边形：什么都不画
        // 带索引：游戏的索引在登记过的池里，编码这一刻读出来展开（同常量缓冲：就是此刻的内容）
        if (di && !rp_pool_find(di->ibuf, (uint64_t)count * isz, NULL, NULL)) { skip("index_buffer_unresolved", rec); return; }
        uint64_t cap = quads == EXP_QUADS ? (uint64_t)(count / 4) * 6 : quads == EXP_LOOP ? (uint64_t)count * 2 : (uint64_t)count * 3;   // 展开后最多几个索引
        nvn_gpu_slice sl; nvn_gpu_err err = {0};
        nvn_gpu_rc rc = nvn_gpu_arena_alloc(c, cap * 4, 4, &sl, &err);
        if (!nvn_gpu_ok(rc) || !sl.cpu) { arena_fail_detail(rc, &err, cap * 4); skip("arena_alloc_failed", rec); return; }
        const uint8_t *src = di ? (const uint8_t *)(uintptr_t)di->ibuf : NULL; int64_t rv = restart ? (int64_t)rp_S.restart.index : -1; uint32_t *dst = (uint32_t *)(void *)sl.cpu;
        uint32_t n = quads == EXP_QUADS ? quad_expand(src, isz, count, rv, dst) : quads == EXP_LOOP ? loop_expand(src, isz, count, rv, dst) : qstrip_expand(src, isz, count, rv, dst);
        if (!n) return;
        a.ibuf = sl.buf; a.ioff = sl.off; a.itype = 2; a.base_vertex = di ? di->base_vertex : dr->first; count = n;
    } else if (di) {
        uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (!rp_buf_resolve(c, di->ibuf, (uint64_t)count * isz, &a.ibuf, &a.ioff)) { skip("index_buffer_unresolved", rec); return; }
        a.itype = (uint32_t)di->itype; a.base_vertex = di->base_vertex;
    } else a.first = (uint32_t)dr->first;
    if (ind) {
        int copied = 0;
        for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
            if (g_prog->ent[st][i].src_kind != NVN_RB_DRV0 || g_ind_drv_ok[st]) continue;
            if (!alloc_copy(c, DRV0_BYTES, NULL, &g_ind_drv[st])) { skip("arena_alloc_failed", rec); return; }
            fill_graphics_drv(&g_ind_drv[st], st, 0);
            // Stay in the main command stream after any GPU producer; a pre-submit
            // transfer command would move this read before that producer.
            if (!copied) rp_pass_flush(c);
            nvn_gpu_copy_buf(c, a.ind, a.ind_off + (di ? 16 : 12),
                             g_ind_drv[st].buf, g_ind_drv[st].off + 0x644, 4);
            g_ind_drv_ok[st] = 1; g_lb[st].ok = 0; copied = 1;
        }
        if (copied) nvn_gpu_barrier(c, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);   // transfer writes visible to driver uniform reads
    }
    static nvn_gpu_view tv[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static nvn_gpu_samp ts[RP_PROG_STAGES][RP_PROG_ENT_MAX];
    char tl[520]; size_t tn = 0; tl[0] = 0;   // 逐笔日志用：这笔采样的贴图
    int narr[RP_PROG_STAGES] = {0};
    DT(RP_PERF_D_PREP);
    rp_view_pin_begin();
    for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
        const nvn_gpu_layout_ent *e = &g_prog->ent[st][i]; tv[st][i] = NULL; ts[st][i] = NULL;
        if (e->src_kind != NVN_RB_TEX) continue;
        if (e->src_reg >= RP_NTEX || (e->src_reg2 != UINT16_MAX && (e->count != 1 || e->src_reg2 >= 128))) {
            skip("separate_texture_shape", rec); goto draw_done;
        }
        if (e->count > 1) {
            if (e->count > RP_TEXARR_MAX || narr[st]++) { skip("texture_array_unsupported_shape", rec); goto draw_done; }
            for (uint32_t k = 0; k < e->count; k++) {
                int slot = (int)(e->src_reg + k) - RP_HANDLE_TABLE_FIRST_SLOT;
                uint64_t hk = slot >= 0 && slot < 32 ? rp_S.tex[st][slot] : 0;
                g_av[st][k] = rp_view_by_id_decl(hk ? (int)(hk & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 1, c, NULL);
                g_as[st][k] = rp_samp_by_id(hk ? (int)(hk >> 20) : rp_S.samp_base);
            }
            // 已知声明的空元素直接取同类型零贴图；未知声明沿用首个有效视图。采样器仍共用首个有效项。
            { nvn_gpu_view fv = NULL; nvn_gpu_samp fs = NULL;
              for (uint32_t k = 0; k < e->count && !(fv && fs); k++) { if (!fv) fv = g_av[st][k]; if (!fs) fs = g_as[st][k]; }
              for (uint32_t k = 0; k < e->count; k++) { if (!g_av[st][k] && !g_prog->dtt[st][i]) g_av[st][k] = fv; if (!g_as[st][k]) g_as[st][k] = fs; } }
            continue;
        }
        uint64_t h = rp_S.tex[st][e->src_reg];
        const tex_t *tt = NULL;
        tv[st][i] = rp_view_by_id_decl(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 0, c, &tt);
        if (g_log_left && tn < sizeof tl - 56) {
            int id = h ? (int)(h & 0xFFFFF) : rp_S.tex_base;
            if (tt) tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " t%d.%u=#%llu(%#x %dx%d 态%x 存%08x%08x 标%#x 池%#x 包%d)", st, (unsigned)e->src_reg, (unsigned long long)tt->serial, (unsigned)tt->d.format, tt->d.width, tt->d.height, (unsigned)rp_tex_debug_state(tt->serial),
                                         tt->mem && tt->L.size >= 8 ? (unsigned)((const uint32_t *)(const void *)tt->mem)[0] : 0u, tt->mem && tt->L.size >= 8 ? (unsigned)((const uint32_t *)(const void *)tt->mem)[1] : 0u, (unsigned)tt->d.flags, tt->pool ? (unsigned)tt->pool->flags : 0xffffu, (int)tt->has_layout);
            else tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " t%d.%u=#%d(无)", st, (unsigned)e->src_reg, id);
        }
        if (e->src_reg2 != UINT16_MAX) {
            if (!rp_S.samp_bound[st][e->src_reg2]) { skip("separate_sampler_unbound", rec); goto draw_done; }
            uint64_t sh = rp_S.samp[st][e->src_reg2];
            ts[st][i] = rp_samp_by_id((int)(sh >> 20));
            if (!ts[st][i]) { skip("separate_sampler_unresolved", rec); goto draw_done; }
        } else ts[st][i] = rp_samp_by_id(h ? (int)(h >> 20) : rp_S.samp_base);
    }
    DT(RP_PERF_D_TEX);
    // 这一笔要采样的贴图里，有还挂着待折清除的：先让它落地（rp_pass_land_reads），否则采到的是没清过的旧内容。
    // 稳态下挂起表是空的（rp_pass_parked() == 0），这条近路不收集、不加锁；只有这一笔真遇到挂起项才走一次。
    if (rp_pass_parked()) {
        static tex_t rdsnap[2 * RP_TEXARR_MAX]; static const tex_t *rd[2 * RP_TEXARR_MAX];
        int nr = 0;
        for (int st = 0; st < RP_PROG_STAGES; st++)
            for (uint32_t i = 0; i < g_prog->nent[st] && nr < (int)(sizeof rd / sizeof rd[0]); i++) {
                const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
                if (e->src_kind != NVN_RB_TEX) continue;
                uint32_t cnt = e->count > 1 ? e->count : 1;
                for (uint32_t k = 0; k < cnt && nr < (int)(sizeof rd / sizeof rd[0]); k++) {
                    int slot = (int)e->src_reg + (e->count > 1 ? (int)k - RP_HANDLE_TABLE_FIRST_SLOT : 0);
                    if (slot < 0 || slot >= 32) continue;
                    uint64_t h = rp_S.tex[st][slot];
                    if (!rp_tex_snap_by_id(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, &rdsnap[nr], NULL, NULL)) continue;
                    rd[nr] = &rdsnap[nr]; nr++;
                }
            }
        if (nr) rp_pass_land_reads(c, rd, nr);
    }
    rp_pass_prepare_draw(c);   // 把这一笔的像素区域（裁剪矩形）交给开通道：小区域的目标只在区域里载入 / 保存
    if (!rp_pass_begin(c)) { skip("pass_begin_failed", rec); goto draw_done; }
    DT(RP_PERF_D_PASS);   // 开通道单独记；打印时与取管线合成原来那一项，另出一行细分
    for (int st = 0; st < RP_PROG_STAGES; st++) g_bind_keep[st] = !rp_args_dirty[st];
    g_pipe_set = 0;   // 换管线会把两个阶段都立脏，绑定状态变没变要在那之前取
    if (rp_pso_dirty || !g_pipe || g_pipe_cmd != c) {
        nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd);
        for (int st = 0; st < RP_PROG_STAGES; st++) pd.st[st].sh = g_prog->sh[st];
        for (int i = 0; i < rt->n && i < 8; i++) { pd.cfmt[i] = rt->color[i] && rp_pass_slot_attached(i) ? (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format) : 0; pd.lmap[i] = (uint8_t)i; }
        if (rt->depth) { nvn_fmt_info fi; uint32_t f = (uint32_t)rt->depth->d.format; int ok = nvn_fmt_info_of(f, &fi); if (!ok || fi.depth) pd.dfmt = f; if (ok && fi.stencil) pd.sfmt = f; }
        const tex_t *c0 = NULL; for (int i = 0; i < rt->n && i < 8 && !c0; i++) c0 = rt->color[i];   // 第一个有图的颜色槽
        pd.samples = c0 ? (uint32_t)c0->d.samples : rt->depth ? (uint32_t)rt->depth->d.samples : 0;
        pd.prim = prim; pd.patch_size = (uint32_t)rp_S.patch_size; pd.prim_restart = (uint8_t)(rp_S.restart.on != 0 && !quads);   // 要展开的图元，重启在展开时已经处理掉
        memcpy(pd.blend, rp_S.blend, sizeof pd.blend); pd.color = rp_S.color; pd.chmask = rp_S.chmask; pd.ms = rp_S.ms;
        g_ms_off = pd.samples > 1 && !pd.ms.enable;
        // 归一化（对照 nvn_mtl.m pso_key_norm / pso_desc，docs/对照_iOS回放层语义.md 第 1 段）：
        //   · 片元着色器没写的输出分量不落帧缓冲：写掩码与着色器的输出映射相与（硬件只写着色器输出表里的；宿主上没写的输出是未定义值）；
        //   · 一个分量都不写的目标、没有图的槽：不混合；高级混合位只为查询保真，不进管线。
        pd.chmask.mask &= g_prog->color_out_mask;
        for (int i = 0; i < 8; i++) {
            if (!pd.cfmt[i] || !(pd.chmask.mask >> (4 * i) & 0xf)) { pd.chmask.mask &= ~(0xfu << (4 * i)); pd.color.blend_en &= (uint8_t)~(1u << i); memset(&pd.blend[i], 0, sizeof pd.blend[i]); continue; }
            pd.blend[i].adv_mode = 0; pd.blend[i].adv_overlap = 0; pd.blend[i].adv_premul = 0; pd.blend[i].adv_normdst = 0;
        }
        { static int fm = -1; if (fm < 0) fm = nvn_plat_flag("nvk_fullmask", 0); if (fm) pd.chmask.mask = 0xffffffffu; }   // 临时实验：颜色写掩码一律全开
        for (int i = 0; i < rp_S.nattr && i < 16; i++) pd.attr[i] = rp_S.attr[i];
        for (int i = 0; i < rp_S.nstream && i < 16; i++) pd.stream[i] = rp_S.stream[i];
        pd.ds = rp_S.ds; pd.poly = rp_S.poly; pd.rast_discard = (uint8_t)(rp_S.rast_discard != 0);
        // 正面绕序（对照 nvn_mtl.m @12766）：游戏自带驱动只在建队列时按当时的窗口原点写一次「光栅翻转」位（原点不是左上 = 开），
        // 之后改原点不再动它。y 朝上的规范化设备坐标里：翻转开着时正面是接口值的反面，关着时就是接口值。
        // 后端再按它翻不翻视口换成自己的帧缓冲绕序，所以窗口原点一并给它。
        pd.poly.front = ((rp_S.poly.front == NVN_FRONT_FACE_CCW) != (s2a_nvn_rast_flip != 0)) ? NVN_FRONT_FACE_CCW : NVN_FRONT_FACE_CW;
        pd.origin_ll = (uint8_t)(rp_origin_lower_left() != 0);
        pd.depth_clamp = (uint8_t)(rp_S.depth_clamp != 0);
        nvn_gpu_pipe np = NULL; nvn_gpu_err err = {0};
        const uint64_t pg0_ = dt_now();
        nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &np, &err);
        if (r.code == NVN_GPU_E_UNSUPPORTED && rp_S.poly.mode != NVN_POLYGON_MODE_FILL) {   // 设备没有点 / 线填充模式：记一笔，照旧按填充画（不让这笔绘制消失）
            nvn_unimpl("gpu", "polygon_mode_not_fill", rec);
            pd.poly.mode = NVN_POLYGON_MODE_FILL; memset(&err, 0, sizeof err);
            r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &np, &err);
        }
        if (!nvn_gpu_ok(r)) {
            char logic_name[32];
            const char *name = "pipe_get";
            if (r.code == NVN_GPU_E_UNSUPPORTED && rp_S.color.logic_op != NVN_LOGIC_OP_COPY) {
                snprintf(logic_name, sizeof logic_name, "logic_op_%u", (unsigned)rp_S.color.logic_op);
                name = logic_name;
            }
            if (nvn_unimpl("gpu", name, rec) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);
            g_skips++; g_pipe = NULL; goto draw_done;
        }
        g_pipe = np; g_pipe_cmd = c;
        nvn_gpu_set_pipe(c, g_pipe); g_pipe_set = 1;
        rp_perf_add(RP_PERF_D_PGET, dt_now() - pg0_);   // 后端取管线 + 设管线（含在取管线那一项里）
        // 换管线：只有绑定布局（set / binding / 类别 / 数量）变了的阶段才立脏重发；布局没变时后端按管线布局自己判描述符组归属。
        for (int st = 0; st < RP_PROG_STAGES; st++) if (!prog_layout_same(g_lb[st].ok ? g_lb[st].pg : NULL, g_prog, st)) rp_args_dirty[st] = 1;
    }
    // #1015：多重采样目标上游戏关了多重采样。Vulkan 没有「目标多重采样、光栅按单样本」的开关（iOS 写满 sample_mask 的做法在 Vulkan 里
    // 等于没写：片元输出的样本掩码与光栅覆盖按位与）；照多重采样画，但具名记账，不静默。
    if (g_ms_off) nvn_unimpl("draw", "multisample_off_on_ms_target", rec);
    uint32_t tw = 1, th = 1;
    { int fi = -1; for (int i = 0; i < rt->n && i < 8 && fi < 0; i++) if (rt->color[i]) fi = i;   // 缺省视口的尺寸：第一个有图的颜色槽，没有就取深度目标
      if (fi >= 0) rp_pass_slot_size(fi, &tw, &th);   // 块压缩基底配一块当一个像素的视图时是块数（#709）
      else if (rt->depth) { uint32_t l = rt->has_dview && rt->dview.set_levels ? rt->dview.base_level : 0; tw = rt->depth->d.width >> l; th = rt->depth->d.height >> l; }
      if (!tw) tw = 1;
      if (!th) th = 1; }
    DT(RP_PERF_D_PIPE);
    nvn_gpu_dyn dyn; if (!fill_dyn(&dyn, tw, th, rec)) goto draw_done;
    nvn_gpu_set_dyn(c, &dyn);
    for (int i = 0; i < 16; i++) if (streams >> i & 1) nvn_gpu_set_vbuf(c, (uint32_t)i, vb_buf[i], vb_off[i]);
    uint32_t base_instance = ind ? 0 : (uint32_t)(di ? di->base_instance : dr->base_instance);
    DT(RP_PERF_D_DYN);
    // 每个阶段都要发，程序里没有的阶段发零条：后端按阶段留着上一笔的绑定，不清掉的话「上一笔有片元、这一笔只有顶点」
    // 会带着旧的片元绑定去配没有片元布局的管线，后端报布局错、整段作废
    for (int st = 0; st < RP_PROG_STAGES; st++) if (!bind_stage(c, g_prog, st, base_instance, tv[st], ts[st], rec)) goto draw_done;
    DT(RP_PERF_D_BIND);   // 首版每笔都整组重发（support / c0 每笔在暂存区里新建）；按脏位省重发是后面的事
    a.prim = prim; a.count = count; a.instances = (uint32_t)(di ? di->instances : dr->instances); if (!a.instances) a.instances = 1; a.base_instance = base_instance;
    rp_occl_draw(c);
    nvn_gpu_draw(c, &a);
    rp_pass_note_draw(rp_S.ds.dt || rp_S.ds.dw || rp_S.ds.st);
    DT(RP_PERF_D_DRAW);
    if (g_log_left) {
        g_log_left--;
        char b[1100]; const blend_st *bl = &rp_S.blend[0];
        snprintf(b, sizeof b, "第 %llu 笔：图元 %u 数 %u 实例 %u%s 目标 #%llu %ux%u 格式 %#x 视图格式 %#x 颜色槽 %d 程序 %02x%02x/%02x%02x 混合 开%#x 源%u 目%u 掩码 %#x 视口 %.0f,%.0f %.0fx%.0f 裁剪 %d(%d,%d %dx%d) 深度测 %d 模板 %d 剔除 %u 正面 %u%s",
                 (unsigned long long)g_draws, prim, count, a.instances, di ? " 带索引" : "", (unsigned long long)(rt->n && rt->color[0] ? rt->color[0]->serial : rt->depth ? rt->depth->serial : 0), tw, th, (unsigned)(rt->n && rt->color[0] ? rt->color[0]->d.format : 0), (unsigned)(rt->n && rt->color[0] && rt->has_cview[0] && rt->cview[0].set_format ? rt->cview[0].format : 0), rt->n,
                 rp_S.skey[0][0], rp_S.skey[0][1], rp_S.skey[1][0], rp_S.skey[1][1], (unsigned)rp_S.color.blend_en, (unsigned)bl->src_rgb, (unsigned)bl->dst_rgb, (unsigned)rp_S.chmask.mask,
                 dyn.vp[0][0], dyn.vp[0][1], dyn.vp[0][2], dyn.vp[0][3], dyn.has_sc, dyn.sc[0], dyn.sc[1], dyn.sc[2], dyn.sc[3], (int)rp_S.ds.dt, (int)rp_S.ds.st, (unsigned)rp_S.poly.cull, (unsigned)rp_S.poly.front, tl);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b);
        if (rt->n > 1) { int o = snprintf(b, sizeof b, "  其余颜色目标："); for (int k = 1; k < rt->n; k++) o += snprintf(b + o, sizeof b - (size_t)o, " [%d]=#%llu(%#x)", k, (unsigned long long)(rt->color[k] ? rt->color[k]->serial : 0), (unsigned)(rt->color[k] ? rt->color[k]->d.format : 0)); nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); }
        // 片元阶段第 0 个常量缓冲的前 20 个四元组与着色器自带常量段的前 2 个（调试取证：对照着色器里读的位置看内容对不对）
        { const p_bind_buf *ub = &rp_S.ubo[1][0];
          if (ub->addr && ub->size >= 320 && rp_pool_find(ub->addr, ub->size, NULL, NULL)) { const float *f = (const float *)(uintptr_t)ub->addr;
            for (int q = 0; q < 20; q += 5) { int o = snprintf(b, sizeof b, "  片元常量 0（长 %llu）第 %d 项起：", (unsigned long long)ub->size, q);
                for (int k = q; k < q + 5; k++) o += snprintf(b + o, sizeof b - (size_t)o, "[%.3g %.3g %.3g %.3g]", (double)f[k * 4], (double)f[k * 4 + 1], (double)f[k * 4 + 2], (double)f[k * 4 + 3]);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); } }
          if (g_prog->c1_ok >> 1 & 1) { float f[8]; memcpy(f, g_prog->c1[1], sizeof f);
            snprintf(b, sizeof b, "  片元自带常量：[%.3g %.3g %.3g %.3g][%.3g %.3g %.3g %.3g]", (double)f[0], (double)f[1], (double)f[2], (double)f[3], (double)f[4], (double)f[5], (double)f[6], (double)f[7]);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); } }
    }
    { uint64_t ts4[HIST_TEX]; int nt = 0;
      for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st] && nt < HIST_TEX; i++) { const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
          if (e->src_kind != NVN_RB_TEX || e->count > 1) continue;
          uint64_t h = rp_S.tex[st][e->src_reg]; tex_t snap; view_t sv; int hv;
          if (rp_tex_snap_by_id(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, &snap, &sv, &hv)) ts4[nt++] = snap.serial; }
      int noted = 0;
      for (int k = 0; k < rt->n; k++) if (rt->color[k]) { hist_note(rt->color[k]->serial, ts4, nt); noted = 1; }   // 每个颜色目标各记一条：多目标的绘制，第二个以后的目标也要能往上追
      if (!noted) hist_note(rt->depth ? rt->depth->serial : 0, ts4, nt); }
    rp_pso_dirty = rp_dss_dirty = 0; memset(rp_args_dirty, 0, sizeof rp_args_dirty);
    g_draws++;
    DT(RP_PERF_D_TAIL);
draw_done:
    rp_view_pin_end();
}

// ---------- 调试：把「上屏那张图」和它上游几层的贴图读回落盘（开关 nvk_dump；rp_present 每 600 帧调一次） ----------
// 每笔绘制记下目标序号和它采样的头几张贴图序号；从上屏的那张图往回走：画到它的那些笔采样了谁，再往上，至多 3 层、12 张。
// 每张读第 0 级第 0 层的原始字节，写到缓存目录（没有就写诊断目录）的 nvkdump_<帧>_<层>_<序号>_<宽>x<高>_<格式>.raw。
struct dump_job { nvn_gpu_buf buf; uint64_t bytes; char path[300]; };
static void dump_done(void *user, const nvn_gpu_done *d) {
    struct dump_job *j = user;
    if (d->status != NVN_GPU_DONE_FAILED) { FILE *f = fopen(j->path, "wb"); if (f) { fwrite(nvn_gpu_buf_cpu(j->buf), 1, (size_t)j->bytes, f); fclose(f); nvn_plat_log(NVN_PLAT_LOG_INFO, "dump", j->path); } }
    nvn_gpu_buf_release(j->buf, NULL, NULL); free(j);
}
// 上屏线程只登记「要读回」；真正的读回在提交线程上、一次提交收尾时做（时间线的值只有提交线程能发）
static uint64_t g_dump_serial, g_dump_frame;
void rp_debug_dump_request(uint64_t present_serial, uint64_t frame) { __atomic_store_n(&g_dump_frame, frame, __ATOMIC_RELEASE); __atomic_store_n(&g_dump_serial, present_serial, __ATOMIC_RELEASE); }
static void dump_chain(uint64_t present_serial, uint64_t frame, uint64_t *fence);
void rp_debug_dump_poll(uint64_t *fence) {
    uint64_t s = __atomic_exchange_n(&g_dump_serial, 0, __ATOMIC_ACQ_REL);
    if (s) dump_chain(s, __atomic_load_n(&g_dump_frame, __ATOMIC_ACQUIRE), fence);
}
static void dump_chain(uint64_t present_serial, uint64_t frame, uint64_t *fence) {
    // 先用缓存目录：安卓上它在应用私有区，调试包可以用 run-as 取出来；诊断目录在外部存储的应用目录里，新系统上 adb 读不到
    const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE); if (!dir) dir = nvn_plat_dir(NVN_PLAT_DIR_DIAG);
    if (!dir) { nvn_plat_log(NVN_PLAT_LOG_WARN, "dump", "没有可写目录，不落盘"); return; }
    uint64_t list[DUMP_MAX]; int depth[DUMP_MAX], n = 0; list[n] = present_serial; depth[n++] = 0;
    // 点名要读的贴图序号（调试开关 nvk_dump_s0 … s5）：不靠往上追，直接读；层号记 9
    for (int k = 0; k < 6; k++) { char nm[16]; snprintf(nm, sizeof nm, "nvk_dump_s%d", k); int sv = nvn_plat_flag(nm, 0); if (sv > 0) { list[n] = (uint64_t)sv; depth[n++] = 9; } }
    uint32_t have = g_hist_n < HIST_N ? g_hist_n : HIST_N;
    for (int i = 0; i < n && n < DUMP_MAX; i++) {
        if (depth[i] >= 6) continue;
        for (uint32_t h = 0; h < have && n < DUMP_MAX; h++) { if (g_hist[h].target != list[i]) continue;
            for (int k = 0; k < HIST_TEX && n < DUMP_MAX; k++) { uint64_t s = g_hist[h].tex[k]; if (!s) continue;
                int seen = 0; for (int q = 0; q < n; q++) if (list[q] == s) seen = 1;
                if (!seen) { list[n] = s; depth[n++] = depth[i] + 1; } } }
    }
    for (int i = 0; i < n; i++) {
        nvn_gpu_tex g = rp_tex_backend_by_serial(list[i]); if (!g) continue;
        nvn_gpu_tex_info ti; nvn_gpu_tex_info_get(g, &ti);
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb((int)ti.nvn_format, &bw, &bh);
        if (!bpb || bw != 1 || bh != 1 || !ti.w || !ti.h || ti.samples > 1 || ti.depth > 1) continue;   // 只读回不压缩的二维贴图：读回失败的段会让时间线进失败终态
        uint64_t row = (uint64_t)((ti.w + (uint32_t)bw - 1) / (uint32_t)bw) * (uint64_t)bpb, bytes = row * ((ti.h + (uint32_t)bh - 1) / (uint32_t)bh);
        struct dump_job *j = calloc(1, sizeof *j); nvn_gpu_err err = {0}; nvn_gpu_cmd c = NULL; nvn_gpu_cmd_desc cd = {0, "nvk dump"};
        if (!j || !nvn_gpu_ok(nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, NULL, &j->buf, &err)) || !nvn_gpu_ok(nvn_gpu_cmd_begin(&cd, &c, &err))) { if (j && j->buf) nvn_gpu_buf_release(j->buf, NULL, NULL); free(j); continue; }
        j->bytes = bytes; snprintf(j->path, sizeof j->path, "%s/nvkdump_%llu_%d_%llu_%ux%u_%#x.raw", dir, (unsigned long long)frame, depth[i], (unsigned long long)list[i], ti.w, ti.h, ti.nvn_format);
        nvn_gpu_copy_tex_buf(c, g, 0, 0, NULL, j->buf, 0, row, bytes);
        *fence += 1;
        nvn_gpu_rc cr = nvn_gpu_cmd_commit(c, *fence, dump_done, j, &err);
        if (!nvn_gpu_ok(cr)) { char m[260]; snprintf(m, sizeof m, "读回 #%llu（%ux%u 格式 %#x）提交失败 %d：%s", (unsigned long long)list[i], ti.w, ti.h, ti.nvn_format, cr.code, err.why); nvn_plat_log(NVN_PLAT_LOG_RED, "dump", m); }
    }
}

// ---------- 计算派发（nvn_mtl.m dispatch @13556） ----------
// 取计算程序 → 落地待折清除并结束通道（派发在通道外，且可能读写那些目标）→ 绑定 NVN 阶段 5 的资源 → 派发。
// 范围：直接与池镜像参数的原生间接派发；常量缓冲、驱动段、按槽绑定的存储缓冲、单张组合贴图与图像。贴图数组：记账跳过。
static nvn_hmap g_cpipes = {.klen = 8};   // 计算程序 → 计算管线
void rp_dispatch(nvn_gpu_cmd c, const p_dispatch *dp, const void *rec) {
    if (!c) return;
    rp_perf_add(RP_PERF_DISP_N, 1);   // 派发次数（其余命令里最贵的一项，看它到底多少次）
    struct timespec tsa; clock_gettime(CLOCK_MONOTONIC, &tsa); uint64_t t_disp0 = (uint64_t)tsa.tv_sec * 1000000000ull + (uint64_t)tsa.tv_nsec;
    if (!dp->ind && (dp->x <= 0 || dp->y <= 0 || dp->z <= 0)) return;   // 0 组：空操作
    const struct rp_prog *pg = NULL;
    { struct timespec tsh; clock_gettime(CLOCK_MONOTONIC, &tsh); uint64_t sh0 = (uint64_t)tsh.tv_sec * 1000000000ull + (uint64_t)tsh.tv_nsec;
      int srx = rp_shader_resolve_compute(&pg);
      clock_gettime(CLOCK_MONOTONIC, &tsh); rp_perf_add(RP_PERF_DISP_SHADER_NS, (uint64_t)tsh.tv_sec * 1000000000ull + (uint64_t)tsh.tv_nsec - sh0);
      if (srx) { skip(srx > 0 ? "compute_program_not_ready" : "compute_program_unresolved", rec); return; } }
    static nvn_gpu_view tv[RP_PROG_ENT_MAX]; static nvn_gpu_samp ts[RP_PROG_ENT_MAX];
    const tex_t *images[RP_PROG_ENT_MAX] = {0};
    struct timespec tr0; clock_gettime(CLOCK_MONOTONIC, &tr0); uint64_t r0 = (uint64_t)tr0.tv_sec * 1000000000ull + (uint64_t)tr0.tv_nsec;
    rp_perf_add(RP_PERF_DISP_ENTS, pg->nent[0]);
    rp_view_pin_begin();
    for (uint32_t i = 0; i < pg->nent[0]; i++) { const nvn_gpu_layout_ent *e = &pg->ent[0][i]; tv[i] = NULL; ts[i] = NULL;
        if (e->src_kind == NVN_RB_IMG) {
            if (e->cls != NVN_GPU_RES_IMAGE || e->count > 1 || e->src_reg >= 32) { skip("compute_image_shape", rec); goto done; }
            uint64_t h = rp_S.img[5][e->src_reg];
            tex_t snap; view_t view; int has_view;
            if (!rp_tex_snap_by_id((int)(h & 0xFFFFF), &snap, &view, &has_view)) { skip("compute_image_unresolved", rec); goto done; }
            uint32_t format = has_view && view.set_format ? (uint32_t)view.format : (uint32_t)snap.d.format;
            if (!(snap.d.flags & NVN_TEXTURE_FLAGS_IMAGE) || !(nvn_gpu_format_caps(format) & NVN_GPU_FMT_STORAGE)) { skip("compute_image_storage_unsupported", rec); goto done; }
            uint32_t levels = has_view && view.set_levels ? (view.nlevels ? view.nlevels : 1) : (snap.d.levels ? snap.d.levels : 1);
            int swizzled = 0;
            for (int ch = 0; ch < 4; ch++) {
                uint32_t sw = has_view && view.set_swizzle ? view.swz[ch] : snap.set_swz && snap.swz ? (snap.swz >> (3 * ch) & 7) : (uint32_t)(2 + ch);
                if (sw <= 5 && sw != (uint32_t)(2 + ch)) swizzled = 1;
            }
            if (levels != 1 || swizzled) { skip("compute_image_view_unsupported", rec); goto done; }
            tv[i] = rp_view_by_id((int)(h & 0xFFFFF), c, &images[i]);
            if (!tv[i] || !images[i]) { skip("compute_image_unresolved", rec); goto done; }
            continue;
        }
        if (e->src_kind != NVN_RB_TEX) continue;
        if (e->count > 1 || e->src_reg >= RP_NTEX || (e->src_reg2 != UINT16_MAX && (e->count != 1 || e->src_reg2 >= 128))) { skip("compute_texture_array", rec); goto done; }
        uint64_t h = rp_S.tex[5][e->src_reg];
        tv[i] = rp_view_by_id_decl(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, pg->dtt[0][i], pg->dcls[0][i], 0, c, NULL);
        if (e->src_reg2 != UINT16_MAX) {
            if (!rp_S.samp_bound[5][e->src_reg2]) { skip("separate_sampler_unbound", rec); goto done; }
            ts[i] = rp_samp_by_id((int)(rp_S.samp[5][e->src_reg2] >> 20));
            if (!ts[i]) { skip("separate_sampler_unresolved", rec); goto done; }
        } else ts[i] = rp_samp_by_id(h ? (int)(h >> 20) : rp_S.samp_base);
    }
    { struct timespec tr1; clock_gettime(CLOCK_MONOTONIC, &tr1); rp_perf_add(RP_PERF_DISP_RESOLVE_NS, (uint64_t)tr1.tv_sec * 1000000000ull + (uint64_t)tr1.tv_nsec - r0); }
    { struct timespec tsb; clock_gettime(CLOCK_MONOTONIC, &tsb); uint64_t f0 = (uint64_t)tsb.tv_sec * 1000000000ull + (uint64_t)tsb.tv_nsec;
      rp_pass_flush(c);
      clock_gettime(CLOCK_MONOTONIC, &tsb); rp_perf_add(RP_PERF_DISP_FLUSH_NS, (uint64_t)tsb.tv_sec * 1000000000ull + (uint64_t)tsb.tv_nsec - f0); }
    g_disp_ind = NULL; g_disp_ind_off = 0;
    { struct timespec tin; clock_gettime(CLOCK_MONOTONIC, &tin); uint64_t in0 = (uint64_t)tin.tv_sec * 1000000000ull + (uint64_t)tin.tv_nsec;
      int bad = dp->ind && ((dp->ind & 3) || !rp_buf_resolve(c, dp->ind, 12, &g_disp_ind, &g_disp_ind_off));
      clock_gettime(CLOCK_MONOTONIC, &tin); rp_perf_add(RP_PERF_DISP_IND_NS, (uint64_t)tin.tv_sec * 1000000000ull + (uint64_t)tin.tv_nsec - in0);
      if (bad) { skip("dispatch_indirect_unresolved", rec); goto done; } }
    uint64_t key = (uint64_t)(uintptr_t)pg; nvn_gpu_pipe cp = nvn_hm_get(&g_cpipes, &key);
    if (!cp) {
        nvn_gpu_stage_desc cs; memset(&cs, 0, sizeof cs); cs.sh = pg->sh[0]; nvn_gpu_err err = {0};
        if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &err)) || !cp) { if (nvn_unimpl("gpu", "compute_pipeline", rec) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why); g_skips++; goto done; }
        nvn_hm_put(&g_cpipes, &key, cp);
    }
    struct timespec tsd; clock_gettime(CLOCK_MONOTONIC, &tsd); uint64_t b0 = (uint64_t)tsd.tv_sec * 1000000000ull + (uint64_t)tsd.tv_nsec;
    nvn_gpu_set_pipe(c, cp);
    g_pipe = NULL; g_pipe_cmd = NULL; rp_dirty_all();   // 后端的当前管线换成了计算管线：下一笔绘制全部重发
    g_disp_dim[0] = dp->ind ? 0 : (uint32_t)dp->x; g_disp_dim[1] = dp->ind ? 0 : (uint32_t)dp->y; g_disp_dim[2] = dp->ind ? 0 : (uint32_t)dp->z;
    if (!bind_stage_at(c, pg, 0, 5, 0, tv, ts, rec)) goto done;
    for (uint32_t i = 0; i < pg->nent[0]; i++) if (images[i] && (pg->ent[0][i].access & NVN_GPU_ACC_W)) rp_tex_mark_gpu_written(images[i]);
    if (g_disp_ind) nvn_gpu_barrier(c, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);
    nvn_gpu_dispatch(c, cp, g_disp_dim[0], g_disp_dim[1], g_disp_dim[2], g_disp_ind, g_disp_ind_off);
    { struct timespec tse; clock_gettime(CLOCK_MONOTONIC, &tse); rp_perf_add(RP_PERF_DISP_BIND_NS, (uint64_t)tse.tv_sec * 1000000000ull + (uint64_t)tse.tv_nsec - b0); }
    g_dispatches++;
done:
    rp_view_pin_end();
    { struct timespec tsc; clock_gettime(CLOCK_MONOTONIC, &tsc); rp_perf_add(RP_PERF_DISP_TOT_NS, (uint64_t)tsc.tv_sec * 1000000000ull + (uint64_t)tsc.tv_nsec - t_disp0); }
}

void rp_draw_stats(uint64_t *draws, uint64_t *skips) { if (draws) *draws = g_draws; if (skips) *skips = g_skips; }
void rp_draw_reset(void) { g_caps_ok = 0; g_arena_reason_n = 0; g_arena_reason_full = 0;   // 会话重开：能力表重新取
    memset(g_ind_drv_ok, 0, sizeof g_ind_drv_ok); memset(g_ind_drv, 0, sizeof g_ind_drv); memset(g_lb, 0, sizeof g_lb);   // #941
    free(g_cpipes.keys); free(g_cpipes.vals); nvn_hm_init(&g_cpipes, 8); g_prog = NULL; g_pipe = NULL; g_pipe_cmd = NULL; g_draws = g_skips = 0; g_prim = ~0u; g_quads = -1; g_face = -1; g_ms_off = 0; }
