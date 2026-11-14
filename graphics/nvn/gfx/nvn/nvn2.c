// NVN2（Switch 2）宿主侧：平台判别、nvnDeviceGetInteger 的 NVN2 答复、着色器键（nvn2.h）
// 设备值表与驱动常量的读法移植自 参考实现（MIT）hle_nvn.c g_devinfo2 / loader.c dm_sdk_nvn2_item（c4b0618c、163a9905）
#include <dlfcn.h>
#include "nvn2.h"
#include "../../host/dm.h"
#include "../tex/nvn_deswizzle.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <pthread.h>
#include <strings.h>

__attribute__((weak)) int s2a_platform_ounce(void) { return 0; }   // 原生层 svc_mem.c 有强定义；单测没有原生层
// host_adapt.m：游戏 sdk 里 NVN2 驱动 nvnDeviceGetInteger 跳表中直接返回立即数的那一项（API / GPU 代码版本随 SDK 走）；0 = 不是常量项或没认出跳表
__attribute__((weak)) int dm_nvn2_driver_item(unsigned item, int *out) { (void)item; (void)out; return 0; }

int nvn2_on(void) { return s2a_platform_ounce() != 0; }   // 不缓存：装载器重定位期（s2a_nv_entry）也问，原生层 NPDM 那时已读（svc_mem.c layout_prepare）

int nvn2_key(const void *control, const void *code, uint8_t out[8]) {
    uint8_t ctl[NVN2_KEY_CTL]; memcpy(ctl, control, NVN2_KEY_CTL);
    memset(ctl + 0x20, 0, 8); memset(ctl + 0x30, 0, 4);
    uint32_t n = nvn2_code_len(control);
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX hc; CC_SHA256_Init(&hc);
    CC_SHA256_Update(&hc, ctl, NVN2_KEY_CTL);
    if (code && n && n < (1u << 24)) CC_SHA256_Update(&hc, code, n);
    CC_SHA256_Final(md, &hc); memcpy(out, md, 8);
    return 1;
}

static uint32_t w32(const uint8_t *img, size_t o) { uint32_t w; memcpy(&w, img + o, 4); return w; }
static int64_t sx21(uint32_t w) { int64_t v = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); return v & 0x100000 ? v - 0x200000 : v; }   // adr / adrp 的 21 位立即数
int nvn2_drv_scan(const uint8_t *img, size_t text_end, size_t ro_lo, size_t ro_hi, nvn2_drv *d) {
    for (size_t p = 8 * 4; p + 10 * 4 <= text_end; p += 4) if (w32(img, p) == 0x52801CE0) {   // mov w0, #231
        int64_t tb = -1, adr = -1; int lim = -1, ldrb = 0, br = 0;
        for (size_t q = p - 8 * 4; q <= p + 8 * 4; q += 4) {
            uint32_t w = w32(img, q);
            if ((w & 0xFFC0001F) == 0x7100001F) lim = (w >> 10) & 0xFFF;   // cmp wN, #imm
            else if ((w & 0x9F000000) == 0x90000000 && (w32(img, q + 4) & 0xFFC00000) == 0x91000000)   // adrp + add
                tb = (int64_t)(q & ~(size_t)0xFFF) + sx21(w) * 4096 + ((w32(img, q + 4) >> 10) & 0xFFF);
            else if ((w & 0x9F000000) == 0x10000000) adr = (int64_t)q + sx21(w);   // adr
            else if ((w & 0xFFE0FC00) == 0x38606800) ldrb = 1;   // ldrb wK, [xT, xI]
            else if ((w & 0xFFFFFC1F) == 0xD61F0000) br = 1;     // br
        }
        if (tb < 0 || adr < 0 || lim <= 1 || !ldrb || !br) continue;
        if ((size_t)tb < ro_lo || (size_t)tb + (size_t)lim >= ro_hi || (size_t)adr >= text_end) continue;
        *d = (nvn2_drv){p, (size_t)tb, (size_t)adr, (unsigned)lim, img, text_end};
        return 1;
    }
    return 0;
}
int nvn2_drv_item(const nvn2_drv *d, unsigned item, int *out) {
    if (item > d->last) return 0;
    size_t c = d->cases + 4 * (size_t)d->img[d->table + item];
    if (c + 8 > d->text_end) return 0;
    uint32_t c0 = w32(d->img, c), c1 = w32(d->img, c + 4);
    if ((c0 & 0xFFE0001F) != 0x52800000 || (c1 & 0xFC000000) != 0x14000000) return 0;   // mov w0, #imm16 ; b
    *out = (int)((c0 >> 5) & 0xFFFF);
    return 1;
}

// Switch 2 的设备值：NVN2 编进游戏自己的 sdk，nvnDeviceGetInteger 是一个按项的 switch，常量项直接返回立即数（dm_nvn2_driver_item 运行时从本游戏 sdk 读，
//   API 与 GPU 代码版本随 SDK 变：20.5.19 是 231.30 / GPU 代码 201.17，21.4.4 是 231.39 / 201.20）。下表是 参考实现 见过的最新（21.4.4），读不到时用。
//   和 X1 不同处：32 个顶点绑定 / 属性（宿主表只有 16 个，照 16 报）、计算组 1024、派发宽度不限、16 级 mip、16 KB 暂存粒度、2 KB 代码填充、32 个独立采样器绑定、项到 0xA9。
//   NVN2_RT：NVN2 运行时从 GPU 描述读的项（暂存倍数、L2 大小、层数、贴图尺寸、线性步长对齐、池页大小…），二进制里读不到：0x60 以下照 X1 表，以上报红回 0
enum { NVN2_RT = -1 };
static const int g_devinfo2[] = {
    [0x00] = 231, [0x01] = 39, [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16 /* NVN2 32 */, [0x07] = 4, [0x08] = 16, [0x09] = 32, [0x0A] = 16, [0x0B] = 4,
    [0x0C] = 4, [0x0D] = 4, [0x0E] = 16 /* NVN2 32 */, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = NVN2_RT,
    [0x18] = NVN2_RT, [0x19] = 4096, [0x1A] = 16384, [0x1B] = 16, [0x1C] = 1024, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1024, [0x20] = 2147483647, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = NVN2_RT, [0x2C] = 16, [0x2D] = NVN2_RT,
    [0x2E] = 201, [0x2F] = 201, [0x30] = 20, [0x31] = 0, [0x32] = 1, [0x33] = 8, [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = NVN2_RT, [0x38] = NVN2_RT, [0x39] = 128,
    [0x3A] = NVN2_RT, [0x3B] = 1, [0x3C] = 4, [0x3D] = NVN2_RT, [0x3E] = NVN2_RT, [0x3F] = NVN2_RT, [0x40] = NVN2_RT, [0x41] = NVN2_RT, [0x42] = 1, [0x43] = 1,
    [0x44] = 1, [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16, [0x4E] = 2048, [0x4F] = 32, [0x50] = 4096,
    [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384, [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 32, [0x60] = 65535, [0x61] = 1, [0x62] = NVN2_RT, [0x63] = 256, [0x64] = 16, [0x65] = 65535, [0x66] = 65535, [0x67] = 256,
    [0x68] = 1, [0x69] = 16, [0x6A] = 16, [0x6B] = 16, [0x6C] = 1, [0x6D] = 1, [0x6E] = 1, [0x6F] = 256, [0x70] = 0, [0x71] = 16, [0x72] = 8, [0x73] = 16, [0x74] = 32,
    [0x75] = 31, [0x76] = 536870912, [0x77] = 16777216, [0x78] = 16777216, [0x79] = 1073741824, [0x7A] = 32, [0x7B] = 1, [0x7C] = 64, [0x7D] = NVN2_RT, [0x7E] = 1,
    [0x7F] = 2147483647, [0x80] = 32, [0x81] = 32, [0x82] = 4194303, [0x83] = 512, [0x84] = 256, [0x85] = 32, [0x86] = 16383, [0x87] = NVN2_RT, [0x88] = 8, [0x89] = 0,
    [0x8A] = 0, [0x8B] = 4, [0x8C] = 256, [0x8D] = 16, [0x8E] = 0, [0x8F] = 1, [0x90] = 31, [0x91] = 0, [0x92] = 64, [0x93] = 1, [0x94] = 58368, [0x95] = 24576, [0x96] = 0,
    [0x97] = 256, [0x98] = 262144, [0x99] = NVN2_RT, [0x9A] = NVN2_RT, [0x9B] = 0, [0x9C] = 201, [0x9D] = 14, [0x9E] = 201, [0x9F] = 20, [0xA0] = 163840, [0xA1] = 8,
    [0xA2] = 4, [0xA3] = 256, [0xA4] = 256, [0xA5] = 4, [0xA6] = 65536, [0xA7] = 201, [0xA8] = 5, [0xA9] = 1,
};
enum { NVN2_X1_ITEMS = 0x60 };   // Switch 1 表（hle_nvn.c g_devinfo）的项数

int nvn2_device_integer(unsigned w, int *out) {
    if (w >= sizeof g_devinfo2 / sizeof *g_devinfo2) return 0;
    if (g_devinfo2[w] == NVN2_RT) {
        if (w < NVN2_X1_ITEMS) return 0;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) LOG("【红】 nvnDeviceGetInteger 项 %#x：NVN2 运行时从 GPU 读，宿主不知道，回 0", w);
        *out = 0; return 1;
    }
    *out = g_devinfo2[w];
    // 0 号是主版本，跳表前就装好（不在常量项里）；其余按本游戏 sdk 里驱动的常量（宿主 16 个顶点槽的两项不跟驱动的 32）
    if (w != 0 && w != 0x06 && w != 0x0E) dm_nvn2_driver_item(w, out);
    if (w <= 1) { static _Atomic int logged; if (!atomic_exchange(&logged, 1)) { int mn = g_devinfo2[1]; int got = dm_nvn2_driver_item(1, &mn);
        LOG("[nvn] 设备 API 231.%d（Switch 2 NVN2；%s）", mn, got ? "游戏 sdk 里驱动的常量" : "sdk 里没认出驱动跳表，按已知最新"); } }
    return 1;
}

// ---------- 红桩：宿主做不到的 Switch 2 SDK 函数（11-04 接口面全量）----------
// 原路径进 sdk 必中止 / 跳空（要 nn::gs / gsv 已启动或 NVN2 驱动在跑），宿主又给不出真实语义的函数：装载期解析到一个红桩，装载时打一行红、
//   被调到就中止并点名函数与原因——和 sdk 原路径同样的结局，但一眼看出是哪个函数、为什么。不回成功、不回假数据。
//   桩池 256 个，按名字分槽（同名复用）；槽满回 NULL（照原路径，装载红行照打）
enum { NS2_RED_SLOTS = 256 };
static struct { const char *name, *why; } g_red[NS2_RED_SLOTS];
static _Atomic int g_red_n;
static pthread_mutex_t g_red_mu = PTHREAD_MUTEX_INITIALIZER;
static void *const g_red_fn[NS2_RED_SLOTS];
__attribute__((noreturn)) static void red_hit(const void *self) {   // 桩按自己的地址找槽
    int k = 0; while (k < NS2_RED_SLOTS && g_red_fn[k] != self) k++;
    die("【红】Switch 2 直连：游戏调了 %s——%s；宿主没有这条路，原路径进 sdk 同样中止", k < atomic_load(&g_red_n) ? g_red[k].name : "?", k < atomic_load(&g_red_n) ? g_red[k].why : "?");
}
#define RED1(k) static uint64_t red_##k(void) { red_hit((const void *)red_##k); }
#define RED4(k) RED1(k##0) RED1(k##1) RED1(k##2) RED1(k##3)
#define RED16(k) RED4(k##0) RED4(k##1) RED4(k##2) RED4(k##3)
#define RED64(k) RED16(k##0) RED16(k##1) RED16(k##2) RED16(k##3)
RED64(0) RED64(1) RED64(2) RED64(3)   // 256 个桩（名字按四进制位拼，只为生成不同的函数名）
#define REF1(k) (void *)red_##k,
#define REF4(k) REF1(k##0) REF1(k##1) REF1(k##2) REF1(k##3)
#define REF16(k) REF4(k##0) REF4(k##1) REF4(k##2) REF4(k##3)
#define REF64(k) REF16(k##0) REF16(k##1) REF16(k##2) REF16(k##3)
static void *const g_red_fn[NS2_RED_SLOTS] = { REF64(0) REF64(1) REF64(2) REF64(3) };
void *nvn2_red(const char *name, const char *why) {
    void *f = NULL;
    pthread_mutex_lock(&g_red_mu);
    int n = atomic_load(&g_red_n), k = 0;
    while (k < n && strcmp(g_red[k].name, name)) k++;
    if (k == n && n < NS2_RED_SLOTS) { g_red[k].name = strdup(name); g_red[k].why = why; atomic_store(&g_red_n, n + 1); }
    if (k < NS2_RED_SLOTS && k <= n) f = g_red_fn[k];
    pthread_mutex_unlock(&g_red_mu);
    LOG("【红】直连：Switch 2 游戏导入 %s——%s：解析到红桩（调用即中止并点名）", name, why);
    return f;
}
const char *nvn2_red_of(const void *f) {
    int n = atomic_load(&g_red_n);
    for (int k = 0; k < n; k++) if (g_red_fn[k] == f) return g_red[k].why;
    return NULL;
}

// ---------- nv::（NVN 下面的 GPU 驱动启动层）----------
// Switch 2 的 nv::InitializeGraphics 经 nn::gs::sdk::BootGraphicsSystem → nn::am::GetGraphicsSupervisor 启动游戏自带的 NVN2 驱动（gsv / nvsrv / vi2 一整套协议）；
//   直连档 NVN 整个换成宿主实现（hle_nvn.c），驱动不跑，这层由宿主记账：装载器把游戏导入的名字解析到这里（runtime/src/gfxmode.c s2a_direct_lookup → s2a_nv_entry）。
//   导出表 16 个全接（4 款 sdk：金刚 20.5 / raiders 21.4 / 火纹 22 / Pokopia 同一套名字）：分配器（含调试工具分配器）记下、Get 读回同一份；
//   InitCheck / IsCapableNoVsyncForDebug 的 sdk 实现读驱动状态（直连档没起，中止）；GetGraphicsMemoryInfo 报游戏给的池。
//   移植自 参考实现（MIT）dm/gfx/hle_nv.c（8dd0c98a、fdb3710d、777a5f6c）
typedef void *(*nv_alloc_fn)(size_t size, size_t align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, size_t size, void *user);
typedef struct { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; } nv_allocator;
static nv_allocator g_nv_alloc, g_nv_devtools_alloc;   // 游戏的分配器（宿主图形层自己分配，眼下不用）
static struct { void *mem; size_t size; _Atomic int inited; } g_nv_pool;
static void nv_set_alloc(nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (nv_allocator){a, f, r, u}; }
static void nv_get_alloc(nv_alloc_fn *a, nv_free_fn *f, nv_realloc_fn *r, void **u) {
    if (a) *a = g_nv_alloc.alloc; if (f) *f = g_nv_alloc.free; if (r) *r = g_nv_alloc.realloc; if (u) *u = g_nv_alloc.user; }
static void nv_set_dev_alloc(nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (nv_allocator){a, f, r, u}; }
static void nv_get_dev_alloc(nv_alloc_fn *a, nv_free_fn *f, nv_realloc_fn *r, void **u) {
    if (a) *a = g_nv_devtools_alloc.alloc; if (f) *f = g_nv_devtools_alloc.free; if (r) *r = g_nv_devtools_alloc.realloc; if (u) *u = g_nv_devtools_alloc.user; }
// nn::gs::sdk 的图形分配器（nv::SetGraphicsAllocator 在 sdk 里就是设它）：与 nv:: 同一份，nvn2_codec.c 的 gs 接管读写这里
void nvn2_gs_allocator(void *out[4]) { out[0] = (void *)g_nv_alloc.alloc; out[1] = (void *)g_nv_alloc.free; out[2] = (void *)g_nv_alloc.realloc; out[3] = g_nv_alloc.user; }
void nvn2_gs_set_allocator(void *a, void *f, void *r, void *u) { nv_set_alloc((nv_alloc_fn)a, (nv_free_fn)f, (nv_realloc_fn)r, u); }
void nvn2_gs_boot(void *mem, size_t size, const char *who) {
    g_nv_pool.mem = mem; g_nv_pool.size = size; atomic_store(&g_nv_pool.inited, 1);
    LOG("%s：图形池 %.2f MB 记下（Switch 2 直连：不启动游戏的 NVN2 驱动、不进 nn::gs，宿主图形层自己分配）", who, size / 1048576.0);
}
void nvn2_gs_shutdown(void) { atomic_store(&g_nv_pool.inited, 0); g_nv_pool.mem = NULL; g_nv_pool.size = 0; }
int nvn2_gs_booted(void) { return atomic_load(&g_nv_pool.inited); }
static void nv_init(void *mem, size_t size) { nvn2_gs_boot(mem, size, "nv::InitializeGraphics"); }
static void nv_init0(void) { nvn2_gs_boot(NULL, 0, "nv::InitializeGraphics()"); }   // 无参版：sdk 自己从分配器要池；宿主不要池
static void nv_fini(void) { nvn2_gs_shutdown(); }
static void nv_init_check(void) { if (!nvn2_gs_booted()) die("nv::InitCheck：图形没初始化（游戏没调 nv::InitializeGraphics）"); }
// nv::MemoryInfo：SDK 11 起 0x40 字节（8 个 u64 计数，sdk 未初始化时清满 0x40）；[0] 总、[1] 已用、[2] 空闲。宿主图形层不从游戏的池分配：已用 0
static void nv_mem_info(uint64_t *out) { memset(out, 0, 0x40); if (nvn2_gs_booted()) { out[0] = g_nv_pool.size; out[2] = g_nv_pool.size; } }
static void nv_dump_mem(void) { LOG("nv::DumpGraphicsMemoryInfo：池 %.2f MB，已用 0（宿主图形层自己分配）", g_nv_pool.size / 1048576.0); }
static void nv_devtools(void *mem, size_t size) { (void)mem; LOG("nv::InitializeGraphicsDevtools %.2f MB：宿主没有 NVN 调试工具（Nsight / 图形调试器），池不用", size / 1048576.0); }
static void nv_service_name(const char *n) { LOG("nv::SetGraphicsServiceName(%s)：宿主不连 nvdrv 服务，不用", n ? n : "(null)"); }
static uint32_t nv_no_vsync_debug(void) { return 0; }   // 「能不能关垂直同步（调试）」：宿主上屏按 NVN 交换间隔，不提供
static void nv_knob(void) {}   // GFXP 抢占 / 显存追踪 / GPU 信号量等待超时：驱动的调试开关，驱动不跑，无处可设
static uint32_t nv_stpl_displays(int n) { static _Atomic int said; if (!atomic_exchange(&said, 1)) LOG("streamplay::CreateIndividualDisplays(%d)：宿主不串流，不建客机画面", n); return 0; }
static void nv_stpl_destroy(void) {}
static void *nv_stpl_window(int guest) { (void)guest; return NULL; }
// 其他命名空间里依赖 gs / gsv / NVN2 驱动启动的函数：只收有实证的——4 款 sdk 里至少一款「gs 未启动」模拟执行进了 gs Check / gsv 会话 / nv::InitCheck 且没有正常返回
//   （原路径必中止 / 拿空会话）。只是调用图可达、模拟执行正常返回的照原路径（03 报告风险表）。宿主没有，红桩
#define W_NVRM "NVIDIA 驱动 nvgpu / host1x 接口：直连档不启动驱动，宿主没有"
#define W_REC "录像（MovieMaker）要 gs 已启动：宿主不录屏"
#define W_CAM "摄像头输出缓冲经 gsv：宿主没有摄像头"
#define W_CHAT "GameChat（chat:u 服务、摄像头视频经 gsv）：宿主没有"
#define W_GPU "GPU 内核接口 / NVIDIA 服务启动（gpuk / gsv）：直连档不启动驱动"
#define W_VI2 "vi2 显示协议经 gsv：直连上屏归宿主（nn::vi 已接管）"
#define W_WSI "NVN2 驱动的窗口插件：直连档 NVN 换成宿主实现"
#define W_GSV "gsv 会话（NVN2 驱动启动时建）：直连档不启动驱动"
static const struct { const char *n; const char *why; } g_ns2_red[] = {
    {"NvOsDrvOpen", W_NVRM},
    {"NvOsTelemetrySubmitData", W_NVRM},
    {"NvRmChannelGetHost1xHandlePRIVATE", W_NVRM},
    {"NvRmChannelNumSyncPoints", W_NVRM},
    {"NvRmChannelOpen", W_NVRM},
    {"NvRmChannelSyncPointIncr", W_NVRM},
    {"NvRmChannelSyncPointRead", W_NVRM},
    {"NvRmChannelSyncPointWait", W_NVRM},
    {"NvRmChannelSyncPointWaitTimeout", W_NVRM},
    {"NvRmChannelSyncPointWaitexTimeout", W_NVRM},
    {"NvRmFencePutToFile", W_NVRM},
    {"NvRmFenceWait", W_NVRM},
    {"NvRmHost1xOpen", W_NVRM},
    {"NvRmHostModuleCheck", W_NVRM},
    {"_ZN2nn5album28InitializeMovieMakerInternalEONS_2sf13SharedPointerINS_6grcsrv11IMovieMakerEEEm", W_REC},
    // nn::chat::gsv（GameChat 画面合成）与 detail 内部入口：游戏直接导入就点名（nn::chat 公开接口由下面 chat_entry 整组接管）
    {"_ZN2nn4chat3gsv10InitializeEv", W_CHAT},
    {"_ZN2nn4chat35CreateAlternativeScreenSharingLayerEPPNS_2vi5LayerEPNS1_7DisplayEPKNS1_21LayerCreationSettingsE", W_CHAT},   // 出参 vi::Layer*：回空图层游戏会接着用，要真聊天才有意义
    {"_ZN2nn4chat3gsv16DestroyImagePoolEPNS1_13ImagePoolTypeE", W_CHAT},
    {"_ZN2nn4chat3gsv29CalculateRequiredBufferMemoryEPmS2_RKNS1_15ImagePreferenceEm", W_CHAT},
    {"_ZN2nn4chat6detail21InitializeUserServiceEv", W_CHAT},
    {"_ZN2nn3gpu3sdk20BootWithRemoteMemoryENS_3gsv12MemoryRegionE", W_GPU},
    {"_ZN2nn3gpu3sdk22BootWithTransferMemoryEPvmNS_3gsv12MemoryRegionE", W_GPU},
    {"_ZN2nn3gpu3sdk30ReserveVprQueueProtectedMemoryEPKNS1_18GraphicsDeviceTypeEm", W_GPU},
    {"_ZN2nn3gpu3sdk34QueryNativeMemoryNvRmMemIdForDebugEPjNS_2gs18NativeMemoryHandleE", W_GPU},
    {"_ZN2nn3gpu3sdk34TryQueryDonatedTransferMemoryUsageEPmS2_S2_", W_GPU},
    {"_ZN2nn3grc24InitializeForApplicationEONS_2sf13SharedPointerINS_6grcsrv11IMovieMakerEEE", W_REC},
    {"_ZN2nn5nvsrv22BootWithTransferMemoryEPvmNS_3gsv12MemoryRegionE", W_GPU},
    {"_ZN2nn3vi210InitializeEv", W_VI2},
    {"_ZN2nn3vi229CalculateRequiredBufferMemoryEPmS1_RKNS0_16BufferPreferenceEm", W_VI2},
    {"_ZN2nn3vi23sdk10InitializeEv", W_VI2},
    {"_ZN2nn3wsi2nv27CalculateTextureRequirementERKNS1_23TexturePreferenceInfoV2E", W_WSI},
    {"_ZN2nn3wsi5ounce6native12NativeWindow10InitializeEP24NVNdisplayPluginWindowV2PKNS0_2nv13TextureInfoV2EjPKc", W_WSI},
    {"_ZN2nn3wsi5ounce6native12NativeWindow20CalculateRequirementERKNS0_2nv23TexturePreferenceInfoV2E", W_WSI},
    {"_ZN2nn3gsv4shim21CalculateRequirementsEPmS2_RKNS1_30InterModuleUserImagePreferenceEj", W_GSV},
    {"_ZN2nn3gsv4shim6detail33InitializeInterModuleImageContextEv", W_GSV},
};
// ---------- nn::chat（GameChat）「没在聊」实现：公开接口整组接管 ----------
// 为什么整组：sdk 的 nn::chat::Initialize 经 detail::InitializeUserService 建 chat:u 服务对象存进 sdk 全局（g_UserService），
//   之后每个公开接口都「锁 → 取全局对象 → 读虚表发命令」；InitializeUserService 末尾还要 nn::chat::gsv::Initialize + 5 次 OpenVirtualDisplay（GPU 服务，直连档没有，失败即 Abort），
//   这些是 sdk 内部互调、不经导入表，宿主接不住——所以 Initialize 只能在 main 导入层接管、不建对象，那么 main 导入的其余公开接口也必须全在这一层接管，
//   漏一个就是读空对象的虚表（11-09 #299 塞尔达无双封印战记：Initialize 后第一个 chat 接口 SIGSEGV 地址 0；审计 P1-145 只接 Initialize 不够）。
// 语义 = 真机「没有 GameChat 会话」：没在聊、没有房间、事件永不触发、设置照收。收发格式取自 Splatoon Raiders（0400380020056000）sdk 的 nn::chat 客户端代理反汇编
//   （公开接口 → IUserService / 用户对象代理虚表 → 命令号 → 共用代理模板的消息布局）：RoomInfo 固定 0xF0 字节（GetChattingUid 读其 +0x18 的 Uid），
//   取模式类出参 1 字节，缩放倍率 f32。返回类型 void / nn::Result / bool / 计数都按 w0 = 0 读（成功 / false / 0）
static uint64_t chat_ret0(void) { return 0; }
typedef struct { uint64_t lo, hi; } chat_uid;
static chat_uid chat_uid0(void) { chat_uid u = {0, 0}; return u; }                                     // GetChattingUid：Uid 全 0 = 没在聊
static uint64_t chat_room0(void *room) { if (room) memset(room, 0, 0xF0); return 0; }                 // GetRoomInfo(ForTest)：没有房间
static uint64_t chat_mode0(void *mode) { if (mode) *(uint8_t *)mode = 0; return 0; }                  // GetLocalCameraImageProcessingMode(_Deprecated)：没有摄像头
static float chat_zoom1(void) { return 1.0f; }                                                         // GetLocalCameraZoomMagnification：不缩放
static uint64_t chat_read0(uint64_t *got, void *buf, uint64_t size, const void *nsa) { (void)buf; (void)size; (void)nsa; if (got) *got = 0; return 0; }   // ReadAudioStream：没有音频流
static uint64_t chat_sync0(void *sync) { if (sync) memset(sync, 0, 16); return 0; }                   // RequestNext*CameraImage：同步句柄全 0（视为已达到），没有图像
// Attach*ChangedEvent(SystemEvent*)：挂一个永不触发的宿主事件（游戏照常 Wait / TryWait / 多重等待）。事件工具在 nvn2_codec.c（依赖宿主 guest_sym / s2a_event_*），
//   由那里的构造函数登记（同 g_vi_vsync：nvn2.c 不对它有链接依赖）；没登记的（图形单测、安卓单库）这一族不接管
static uint64_t (*g_sev_attach)(void *sev, int autoclear);
void nvn2_sev_attach_register(uint64_t (*f)(void *, int)) { g_sev_attach = f; }
static void chat_attach(void *sev) { if (sev && g_sev_attach) g_sev_attach(sev, 0); }
// 摄像头视频存储（#116）：nvn2_codec.c 构造函数登记它的实现（那边有原生图像池）；没登记（安卓、不链 codec 的单测）照「没在聊」回 0
static void *g_chat_video[4];   // 0 Calculate / 1 Initialize / 2 Finalize / 3 Get*ImageHandle
void nvn2_chat_video_register(void *size, void *init, void *fini, void *handle) { g_chat_video[0] = size; g_chat_video[1] = init; g_chat_video[2] = fini; g_chat_video[3] = handle; }
static void *chat_entry(const char *name) {
    static const char P[] = "_ZN2nn4chat";
    if (strncmp(name, P, sizeof P - 1)) return NULL;
    const char *n = name + sizeof P - 1;
    static const struct { const char *n; void *f; } t[] = {
        {"14GetChattingUidEv", (void *)chat_uid0},
        {"11GetRoomInfoEPNS0_8RoomInfoE", (void *)chat_room0},
        {"18GetRoomInfoForTestEPNS0_8RoomInfoE", (void *)chat_room0},
        {"33GetLocalCameraImageProcessingModeEPNS0_25CameraImageProcessingModeE", (void *)chat_mode0},
        {"54GetLocalCameraImageProcessingMode_Deprecated_OAAA51548EPNS0_25CameraImageProcessingModeE", (void *)chat_mode0},
        {"31GetLocalCameraZoomMagnificationEv", (void *)chat_zoom1},
        {"15ReadAudioStreamEPmPvmRKNS_7account23NetworkServiceAccountIdE", (void *)chat_read0},
        {"27RequestNextLocalCameraImageEPNS_2gs16NativeSyncHandleEPNS0_20LocalCameraVideoTypeEiS2_", (void *)chat_sync0},
        {"28RequestNextRemoteCameraImageEPNS_2gs16NativeSyncHandleEPNS0_21RemoteCameraVideoTypeEiS2_RKNS_7account23NetworkServiceAccountIdE", (void *)chat_sync0},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(n, t[i].n)) return t[i].f;
    // 摄像头视频存储：真建原生图像（游戏开机就建、按序号取句柄建纹理；没人聊天时从不写入，#116），见 nvn2_codec.c nvn2_chat_video_*
    { static const struct { const char *n; int k; } cv[] = {
        {"35CalculateLocalCameraVideoBufferSizeEiii", 0}, {"36CalculateRemoteCameraVideoBufferSizeEiii", 0},
        {"33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm", 1}, {"34InitializeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeEiiiPvm", 1},
        {"31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE", 2}, {"32FinalizeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeE", 2},
        {"25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi", 3}, {"26GetRemoteCameraImageHandleEPKNS0_21RemoteCameraVideoTypeEi", 3} };
      for (unsigned i = 0; i < sizeof cv / sizeof *cv; i++) if (!strcmp(n, cv[i].n)) return g_chat_video[cv[i].k] ? g_chat_video[cv[i].k] : (void *)chat_ret0; }
    // 8 个 Attach*ChangedEvent（参数都是 nn::os::SystemEvent*）
    if (strstr(n, "AttachTo") && strstr(n, "EventEPNS_2os11SystemEventE")) return g_sev_attach ? (void *)chat_attach : NULL;
    // 语义不明、要真聊天才有意义的：屏幕共享替代图层（出参 vi::Layer*，回空图层游戏会接着用）——不在这里接管，交给下面红桩表点名
    if (!strncmp(n, "35CreateAlternativeScreenSharingLayer", 37)) return NULL;
    // 内部实现与全局变量：不是游戏该导入的
    { const char *q = n; while (*q >= '0' && *q <= '9') q++; if (!strncmp(n, "6detail", 7) || !strncmp(n, "3gsv", 4) || !strncmp(q, "g_", 2)) return NULL; }
    // 其余公开接口：初始化 / 结束、设置 / 动作、查询计数与尺寸、视频存储——都按「没有会话」回 0（无出参，或出参由调用方预置）
    static const char *const ret0[] = {
        "10InitializeEv", "8FinalizeEv", "30InitializeWithoutVirtualCameraEv", "28FinalizeWithoutVirtualCameraEv", "10IsChattingEv",
        "20SetScreenSharingModeENS0_17ScreenSharingModeE", "28SetScreenSharingDisallowListEPKNS_7account23NetworkServiceAccountIdEi", "30ClearScreenSharingDisallowListEv",
        "22GetRealtimeAudioLevelsEPNS0_10AudioLevelEiPKNS_7account23NetworkServiceAccountIdEi", "42CalculateRequiredAudioStreamWorkBufferSizeEi",
        "27CreateAudioStreamWorkBufferEPvmi", "28DestroyAudioStreamWorkBufferEv", "32GetAudioStreamCapacityFrameCountEv",
        "25StartAudioStreamRecordingERKNS_7account23NetworkServiceAccountIdE", "24StopAudioStreamRecordingERKNS_7account23NetworkServiceAccountIdE",
        "28GetAudioStreamFreeFrameCountERKNS_7account23NetworkServiceAccountIdE", "30GetAudioStreamFilledFrameCountERKNS_7account23NetworkServiceAccountIdE",
        "31SetLocalCameraZoomMagnificationEf", "13SetSharingKeyERKNS0_10SharingKeyE", "15ClearSharingKeyEv", "13ShowChatStartERKNS_7account3UidE",
        "22ShowCameraSharingGuideEv", "28ShowLocalCameraConfigurationENS0_25CameraImageProcessingModeE", "36ShowLocalCameraConfigurationVersion0ENS0_25CameraImageProcessingModeE",
        "46BeginFullScreenWithLocalCameraAvailableSectionEbb", "46BeginFullScreenWithLocalCameraAvailableSectionEb", "44EndFullScreenWithLocalCameraAvailableSectionEv",
        "25StartLocalCameraStreamingEv", "24StopLocalCameraStreamingEv", 
        
        "26StartRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", "33StartRemoteCameraStreamingForTestERKNS_7account23NetworkServiceAccountIdE",
        "25StopRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", "32StopRemoteCameraStreamingForTestERKNS_7account23NetworkServiceAccountIdE",
         
        "39SetAlternativeScreenSharingLayerEnabledEb", "23InitializeVirtualCameraEPNS0_17VirtualCameraTypeE", "21FinalizeVirtualCameraEPNS0_17VirtualCameraTypeE",
        "34GetVirtualCameraNativeWindowHandleEPKNS0_17VirtualCameraTypeE", "19EnableVirtualCameraEPNS0_17VirtualCameraTypeE",
        "20DisableVirtualCameraEPNS0_17VirtualCameraTypeE", "43SetVirtualCameraContainPhotographedContentsEPNS0_17VirtualCameraTypeEb",
        "44GetCameraImageProcessingModeForVirtualCameraEv",
    };
    for (unsigned i = 0; i < sizeof ret0 / sizeof *ret0; i++) if (!strcmp(n, ret0[i])) return (void *)chat_ret0;
    // 认不得的 nn::chat 名字（更新的 sdk 新加的接口）：大声报出来，照原路径（会读空服务对象）——补进上表
    LOG("【红】直连：Switch 2 游戏导入 nn::chat 接口 %s，宿主「没在聊」实现表里没有：照原路径会读空服务对象，补进 nvn2.c chat_entry", name);
    return NULL;
}
// ---------- nn::camera::sdk「服务不可用」实现 ----------
// 查询 / 创建类：返回「不可用」结果（尺寸 0、句柄无效）；Destroy 类：空操作。真实返回类型无公开文档（void 或 nn::Result），一律回 0
static uint64_t cam_calc_size(void *out_size, void *out_align, const void *pref, uint32_t count) {
    (void)pref; (void)count;
    static _Atomic int said;
    if (!atomic_exchange(&said, 1)) LOG("nn::camera::sdk::CalculateImageArraySize：宿主没有摄像头，输出参数写 0");
    if (out_size) memset(out_size, 0, 8);
    if (out_align) memset(out_align, 0, 8);
    return 0;
}
static uint64_t cam_create_pool(void *out_handle, const void *pref, uint32_t count, void *mem_handle) {
    (void)pref; (void)count; (void)mem_handle;
    static _Atomic int said;
    if (!atomic_exchange(&said, 1)) LOG("nn::camera::sdk::CreateOutputBufferPool：宿主没有摄像头，输出参数写 0（无效句柄）");
    if (out_handle) memset(out_handle, 0, 2);
    return 0;
}
static uint64_t cam_destroy_pool(uint16_t handle) {
    (void)handle;
    static _Atomic int said;
    if (!atomic_exchange(&said, 1)) LOG("nn::camera::sdk::DestroyOutputBufferPool：宿主没有摄像头，空操作");
    return 0;
}
// CreateAliasOutputBufferPool(nn::gsv::UserImageHandle, unsigned)：两个参数都按值（修饰名里没有出参），同 DestroyOutputBufferPool。
//   原先第一参当出参地址写 2 字节，客人按值传句柄 1 就往地址 1 写、信号 11（审计 P1-145）。不写任何内存，回 0（返回值按句柄读即无效句柄 0）
static uint64_t cam_create_alias(uint16_t source, uint32_t count) {
    (void)source; (void)count;
    static _Atomic int said;
    if (!atomic_exchange(&said, 1)) LOG("nn::camera::sdk::CreateAliasOutputBufferPool：宿主没有摄像头，空操作、回 0");
    return 0;
}
static uint64_t cam_destroy_alias(uint16_t handle) {
    (void)handle;
    static _Atomic int said;
    if (!atomic_exchange(&said, 1)) LOG("nn::camera::sdk::DestroyAliasOutputBufferPool：宿主没有摄像头，空操作");
    return 0;
}
// CreateCameraContext(UniqueNativeHandle*)：输出参数是 32 位会话句柄（同 nxnative hle_sf.c：SDK 22.2.0 sdk+0x2c42c0 只写 4 字节）。游戏之后经它发 HIPC：
//   原先写 0 → 游戏对句柄 0 SendSyncRequest、会话不在会话表 → sdk 中止（诊断 #0148a6，0400C3F00006E000 在 iPad 上 5 局 Break 都在这一步后）。
//   现在向服务层要一个「没插摄像头的主机」会话（services/S2A.Services/CameraNoDevice.cs S2A_CameraSessionOpen）；服务层没链（原生单测）时照旧写 0
static uint64_t cam_create_ctx(uint32_t *out_handle) {
#if defined(__ANDROID__)
    // 安卓：服务层 NativeAOT 与本文件静态并入同一个 libs2arm_android.so，链接带 --exclude-libs,ALL，导出不进动态表，
    //   dlsym(RTLD_DEFAULT) 恒为 NULL（同 hle_nvn.c pool_nvmap）。改链接期弱引用：出包的单库里由服务层归档解析
    //   （core-link 审计：归档定义的导出在库里仍未解析、或按名 dlsym 取隐藏导出都失败）；不带服务层的门禁链接里为空，照旧写 0
    extern uint32_t S2A_CameraSessionOpen(void) __attribute__((weak));
    uint32_t h = S2A_CameraSessionOpen ? S2A_CameraSessionOpen() : 0;
#else
    uint32_t (*open)(void) = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "S2A_CameraSessionOpen");
    uint32_t h = open ? open() : 0;
#endif
    static _Atomic int said;
    if (!atomic_exchange(&said, 1)) LOG("nn::camera::sdk::CreateCameraContext：宿主没有摄像头，给「没插摄像头」会话 %#x（0 = 服务层没起）", h);
    if (out_handle) *out_handle = h;
    return 0;
}
static uint64_t cam_destroy_ctx(void) {
    static _Atomic int said;
    if (!atomic_exchange(&said, 1)) LOG("nn::camera::sdk::DestroyCameraContext：宿主没有摄像头，空操作");
    return 0;
}

void *s2a_nv_entry(const char *name) {
    if (!name || !nvn2_on()) return NULL;
    static const struct { const char *n; void *f; } t[] = {
        {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", (void *)nv_set_alloc},
        {"_ZN2nv20GetGraphicsAllocatorEPPFPvmmS0_EPPFvS0_S0_EPPFS0_S0_mS0_EPS0_", (void *)nv_get_alloc},
        {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", (void *)nv_set_dev_alloc},
        {"_ZN2nv28GetGraphicsDevtoolsAllocatorEPPFPvmmS0_EPPFvS0_S0_EPPFS0_S0_mS0_EPS0_", (void *)nv_get_dev_alloc},
        {"_ZN2nv18InitializeGraphicsEPvm", (void *)nv_init},
        {"_ZN2nv18InitializeGraphicsEv", (void *)nv_init0},
        {"_ZN2nv16FinalizeGraphicsEv", (void *)nv_fini},
        {"_ZN2nv9InitCheckEv", (void *)nv_init_check},
        {"_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE", (void *)nv_mem_info},
        {"_ZN2nv22DumpGraphicsMemoryInfoEv", (void *)nv_dump_mem},
        {"_ZN2nv26InitializeGraphicsDevtoolsEPvm", (void *)nv_devtools},
        {"_ZN2nv22SetGraphicsServiceNameEPKc", (void *)nv_service_name},
        {"_ZN2nv24IsCapableNoVsyncForDebugEv", (void *)nv_no_vsync_debug},
        {"_ZN2nv20EnablePreemptionGFXPEv", (void *)nv_knob},
        {"_ZN2nv27EnableGraphicsMemoryTracingEv", (void *)nv_knob},
        {"_ZN2nv28DisableGpuSemaAcquireTimeoutEv", (void *)nv_knob},
        // nn::streamplay 里碰显示层的三个：sdk 实现内部直调 nv::InitCheck（游戏自带驱动的状态，直连档没起）就中止（Pokopia m12：CreateIndividualDisplays → sdk+0x57600c）。
        //   宿主不做 GameShare 串流（服务层 stpl:u 回「无会话、无客机」）：不建客机画面、回成功；客机窗口没有、回空（同 pokopia-native，MIT）
        {"_ZN2nn10streamplay24CreateIndividualDisplaysEi", (void *)nv_stpl_displays},
        {"_ZN2nn10streamplay25DestroyIndividualDisplaysEv", (void *)nv_stpl_destroy},
        {"_ZN2nn10streamplay21GetNativeWindowHandleENS0_7GuestIdE", (void *)nv_stpl_window},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(name, t[i].n)) return t[i].f;
    // nn::chat 公开接口整组「没在聊」实现（见 chat_entry）；nn::camera::sdk「服务不可用」
    { void *c = chat_entry(name); if (c) { static _Atomic int said; if (!atomic_exchange(&said, 1)) LOG("nn::chat：宿主没有 GameChat，游戏导入的公开接口整组按「没在聊」接管（首个 %s）", name); return c; } }
    if (!strcmp(name, "_ZN2nn6camera3sdk23CalculateImageArraySizeEPNS_3gsv10MemorySizeES4_RKNS2_25CameraUserImagePreferenceEj")) return (void *)cam_calc_size;
    if (!strcmp(name, "_ZN2nn6camera3sdk23CalculateImageArraySizeEPNS_3gsv10MemorySizeES4_RKNS2_7imgdesc4impl4user5ounce25CameraUserImagePreferenceEj")) return (void *)cam_calc_size;
    if (!strcmp(name, "_ZN2nn6camera3sdk22CreateOutputBufferPoolEPNS_3gsv15UserImageHandleERKNS2_25CameraUserImagePreferenceEjNS2_16UserMemoryHandleE")) return (void *)cam_create_pool;
    if (!strcmp(name, "_ZN2nn6camera3sdk22CreateOutputBufferPoolEPNS_3gsv15UserImageHandleERKNS2_7imgdesc4impl4user5ounce25CameraUserImagePreferenceEjNS2_16UserMemoryHandleE")) return (void *)cam_create_pool;
    if (!strcmp(name, "_ZN2nn6camera3sdk23DestroyOutputBufferPoolENS_3gsv15UserImageHandleE")) return (void *)cam_destroy_pool;
    if (!strcmp(name, "_ZN2nn6camera3sdk27CreateAliasOutputBufferPoolENS_3gsv15UserImageHandleEj")) return (void *)cam_create_alias;
    if (!strcmp(name, "_ZN2nn6camera3sdk28DestroyAliasOutputBufferPoolENS_3gsv15UserImageHandleE")) return (void *)cam_destroy_alias;
    if (!strcmp(name, "_ZN2nn6camera3sdk19CreateCameraContextEPNS_5gsdrv18UniqueNativeHandleE")) return (void *)cam_create_ctx;
    if (!strcmp(name, "_ZN2nn6camera3sdk20DestroyCameraContextEv")) return (void *)cam_destroy_ctx;
    for (unsigned i = 0; i < sizeof g_ns2_red / sizeof *g_ns2_red; i++) if (!strcmp(name, g_ns2_red[i].n)) return nvn2_red(name, g_ns2_red[i].why);
    // streamplay 其余走服务层（stpl:u）、gsv 其余（IsInitialized / GetUserSession…）无会话时 sdk 如实回空：照原路径
    if (!strncmp(name, "_ZN2nv", 6)) LOG("【红】直连：Switch 2 游戏导入 %s，宿主没接管：照原路径进 sdk（会启动游戏自带的 NVN2 驱动）", name);
    return NULL;
}

// ---------- nn::vi（显示层）----------
// Switch 2 的 nn::vi::Initialize 等在 sdk 里先调 nn::gs::sdk::CheckGraphicsSystemInitialized（sdk+0x3f84d0），gs 没启动（上面的 nv:: 接管）就中止。直连档上屏归宿主
//   （hle_nvn.c 的 NVN 窗口 → Metal 图层），显示 / 图层 / 原生窗口都只是交给 nvnWindowBuilderSetNativeWindow 的不透明句柄：回宿主静态对象。
//   句柄族：显示 / 图层是宿主的静态对象，sdk 拿到它们会和自己的 DefaultDisplay 比（火纹 sdk+0x5e02a0：不是就回 0x272）、或把图层 +8 当 vi2 图层转发（跳空）——
//   所以 nn::vi 收显示 / 图层的公有函数全部由宿主答（4 款 sdk 导出表的全部公有函数，见下表）；类成员 / detail 是 sdk 内部实现，游戏导入即红桩。
//   宿主出 SDR、固定 60 Hz：HDR 元数据无处可用，VRR / 120 Hz 能力不开。移植自 参考实现（MIT）dm/gfx/hle_vi.c（8dd0c98a、0a759de7、777a5f6c）
static struct { uint32_t magic; } g_vi_display = {0x50534944}, g_vi_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_vi_native = {0x4e49574e, &g_vi_layer};
static void vi_nop(void) {}
static uint32_t vi_open_display(void **out) { *out = &g_vi_display; return 0; }
// 按名打开：sdk 认「Default」（大小写不敏感，另认一个内部名「rtc」，火纹 sdk+0x5dfa30）；宿主只有一个显示，「rtc」等别的名字回 sdk 自己的「找不到」0xe72
static uint32_t vi_open_named(void **out, const char *name) {
    if (out) *out = NULL;
    if (name && !strcasecmp(name, "Default")) return vi_open_display(out);
    LOG("【红】vi::OpenDisplay(%s)：宿主只有默认显示，回 0xe72（sdk 对不认识的名字同一结果）", name ? name : "(null)"); return 0xe72;
}
static uint32_t vi_create_layer(void **out, void *disp) { (void)disp; *out = &g_vi_layer; return 0; }
static uint32_t vi_create_layer_wh(void **out, void *disp, int w, int h) { (void)disp; LOG("vi::CreateLayer %dx%d", w, h); *out = &g_vi_layer; return 0; }
// SDK 22 起带 LayerCreationSettings（构造函数 (w, h) 填 {w, h, 2}，sdk 自己的构造照跑）：按宽高建
static uint32_t vi_create_layer_cs(void **out, void *disp, const int32_t *cs) { return vi_create_layer_wh(out, disp, cs ? cs[0] : 0, cs ? cs[1] : 0); }
// 共享 / 录像 / MovieMaker 图层：真机给 GameShare 串流与录屏合成用；宿主不串流、不录屏，建不出——回失败（sdk 的 0x272 无效参数同族），不给假图层
static uint32_t vi_special_layer(void **out, void *disp, const void *cs) { (void)disp; (void)cs; if (out) *out = NULL; LOG("【红】vi::Create{Sharing,Recording,MovieMaker}Layer：宿主不串流、不录屏，没有这种图层，回失败"); return 0x2a2; }
static uint32_t vi_native_window(void **out, void *layer) { (void)layer; *out = &g_vi_native; return 0; }
static uint32_t vi_ok_vrr(void *disp, unsigned on) { (void)disp; static _Atomic int n; if (!atomic_exchange(&n, 1)) LOG("vi::SetDisplayVrrCapability %u：宿主固定刷新率，不开 VRR", on & 0xff); return 0; }
static uint32_t vi_ok_cap(void *disp, unsigned on) { (void)disp; (void)on; return 0; }   // HDR / 120 Hz 能力：只是「允许」，宿主出 SDR 60 Hz，查询照实报
static uint32_t vi_ok_hdr(void *layer, unsigned lo, unsigned hi) { (void)layer; (void)lo; (void)hi; return 0; }
// 火纹 Fortune's Weave（11-03，main 导入，不接管就进 sdk 要 nn::gs 而中止）：HDR 状态 / 纸白 / 显示最大缓冲 / VRR / 扩展交换间隔 / HDR 色域元数据。
//   宿主出 SDR、固定刷新率；BufferMaxResolution 是 u32 档位（0 = 720p、1 = 1080p、2 = 1440p、3 = 4K）：回 0 时火纹按 720p 出图（DLSS 输入 640x360），
//   照 Switch 2：主机模式 4K、掌机模式屏幕 1080p。事实取自 参考实现（MIT）dm/gfx/hle_vi.c（0a759de7），代码自写
int dm_opmode(void);
static uint32_t vi_hdr_status(uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) { (void)disp; if (on) *on = 0; if (fmt) *fmt = 0; if (peak) *peak = 0; return 0; }
static uint32_t vi_paper_white(uint16_t *nits, const void *disp) { (void)disp; if (nits) *nits = 203; return 0; }   // BT.2408 参考白；HDR 关着时游戏不读
static uint32_t vi_max_res(uint32_t *res, const void *disp) { (void)disp; if (res) *res = dm_opmode() == 1 ? 3 : 1; return 0; }
static uint32_t vi_vrr_supported(uint8_t *on, const void *disp) { (void)disp; if (on) *on = 0; return 0; }
// 当前刷新率：RefreshRate 是 u32 赫兹（sdk 只认 60 / 120，火纹 sdk+0x5dd5a4），带 bool 的 SDK 21 起版本另出「VRR 在用」：宿主 60、没有 VRR
static uint32_t vi_refresh(uint32_t *hz, const void *disp) { (void)disp; if (hz) *hz = 60; return 0; }
static uint32_t vi_refresh2(uint32_t *hz, uint8_t *vrr, const void *disp) { (void)disp; if (hz) *hz = 60; if (vrr) *vrr = 0; return 0; }
// 可用刷新率表：AvailableRefreshRate 0x38 字节、首字是赫兹（火纹 sdk+0x5ddc28 逐字拷 14 个 u32），其余 13 个字的含义没核过——不编，回失败
static uint32_t vi_avail_refresh(int *n, void *out, int max, void *disp) { (void)out; (void)max; (void)disp; if (n) *n = 0; LOG("【红】vi::QueryDisplayAvailableRefreshRates：表项后 13 个字含义未核，宿主不编，回失败"); return 0x2a2; }
static uint32_t vi_ext_swap(void *layer, int a, int b) { (void)layer; static _Atomic int n; if (!atomic_exchange(&n, 1)) LOG("vi::SetLayerExtendedSwapInterval %d %d：宿主没有 VRR，NVN 的交换间隔照旧", a, b); return 0; }
static uint32_t vi_ext_swap_clear(void *layer) { (void)layer; return 0; }
static uint32_t vi_hdr_gamut(void *layer, int g) { (void)layer; (void)g; return 0; }
static uint32_t vi_hdr_bezier(void *layer, float a, float b, float c) { (void)layer; (void)a; (void)b; (void)c; return 0; }
// 金刚 / raiders（11-04，main 导入，不接管就进 sdk 要 nn::gs：金刚开局跳空地址崩在 main+0x203704）：
//   HDR → SDR 色调映射元数据（宿主出 SDR，无处可用，回成功）；显示 vsync 事件（宿主线程按 60 Hz 触发，自动清除）。
//   事实取自 参考实现（MIT）dm/gfx/hle_vi.c、dm/host/hle_sdk16.c h_vi_VsyncEvent（777a5f6c），代码自写
static uint32_t vi_hdr_tonemap(void *layer, int m) { (void)layer; (void)m; return 0; }
static uint32_t vi_layer_clear(void *layer) { (void)layer; return 0; }
// 图层的帧号：Unreal 按它定节奏（参考实现 记八方旅人 0）；宿主的帧号 = 游戏已上屏的帧数（hle_nvn.c；单测不链它，弱定义回 0）。帧历史宿主不留：0 项
__attribute__((weak)) uint64_t dm_nvn_frame_count(void) { return 0; }
static uint32_t vi_frame_no(uint64_t *out, const void *layer) { (void)layer; if (out) *out = dm_nvn_frame_count(); return 0; }
static int vi_frame_hist(void *out, int n, const void *layer) { (void)out; (void)layer; static _Atomic int s; if (!atomic_exchange(&s, 1)) LOG("vi::ListFrameHistory（要 %d 项）：宿主不留帧历史，回 0 项", n); return 0; }
// 图层用户数据（SDK 22，随下一帧交给系统合成器）：宿主合成不读它
static uint32_t vi_user_data(void *layer, uint64_t v) { (void)layer; (void)v; return 0; }
// 图层的 vi2 内部图层：sdk 回 图层 + 8（vi2::LayerType）；宿主图层没有 vi2 对象——回空，谁拿去用谁当场空指针
static void *vi_internal_layer(const void *layer) { (void)layer; LOG("【红】vi::GetInternalLayer：宿主图层没有 vi2 内部对象，回空"); return NULL; }
// 版权图层（录屏 / 截图时盖在画面上的版权图）：真机只在录制合成时用；宿主不录屏、不截图——无处可合成，记下不用
static uint32_t vi_copyright_init(void *mem, uint64_t size) { (void)mem; LOG("vi::InitializeCopyrightFramebuffer %.1f KB：宿主不录屏，版权图层不合成", size / 1024.0); return 0; }
static uint32_t vi_copyright_write(const void *p, uint64_t n, int x, int y, int w, int h, int origin) { (void)p; (void)n; (void)x; (void)y; (void)w; (void)h; (void)origin; return 0; }
static void vi_copyright_enable(unsigned on) { (void)on; }
// 显示 vsync 事件的实现在 nvn2_codec.c（与视频解码共用宿主事件工具；那里依赖宿主的 guest_sym / s2a_event_*）。nvn2.c 被几乎所有图形单测与安卓单库链入，
//   不能对它有链接依赖（11-04：直接调 / 弱引用都会把 nvn2_codec.o 从 libnvn.a 拉进只链图形库的单测，缺宿主符号）：由 nvn2_codec.c 的构造函数登记进来，
//   宿主 / iOS 链着视频解码就有；没链的（图形单测、安卓单库）为空 → 这个名字回落红行
static void *g_vi_vsync;
void nvn2_vi_vsync_register(void *f) { g_vi_vsync = f; }
// nn::vi 的类成员 / detail（sdk 内部实现，经公有函数调到）：收的对象是 sdk 自己的 Display / Layer，宿主给的是静态句柄——游戏若直接导入，红桩
static int vi_internal(const char *n) {
    static const char *const pre[] = { "_ZN2nn2vi5Layer", "_ZNK2nn2vi5Layer", "_ZN2nn2vi7Display", "_ZNK2nn2vi7Display", "_ZN2nn2vi6detail", "_ZNK2nn2vi6detail",
                                       "_ZN2nn2vi16LayerWithHistory", "_ZNK2nn2vi16LayerWithHistory", "_ZN2nn2vi20CopyrightFramebuffer" };
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
    return 0;
}
// 不必接管：sdk 原路径可跑（ListDisplays 纯写一项「Default」，火纹 sdk+0x5df940；LayerCreationSettings 纯值对象；Set*LayerEnabled 转 nn::am 服务）
static int vi_pure(const char *n) {
    return !strcmp(n, "_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi") || !strncmp(n, "_ZN2nn2vi21LayerCreationSettings", 32) || !strncmp(n, "_ZNK2nn2vi21LayerCreationSettings", 33)
        || !strcmp(n, "_ZN2nn2vi22SetSharingLayerEnabledEb") || !strcmp(n, "_ZN2nn2vi24SetRecordingLayerEnabledEb") || !strcmp(n, "_ZN2nn2vi25SetMovieMakerLayerEnabledEb");
}
void *nvn2_vi_entry(const char *name) {
    if (!name || !nvn2_on()) return NULL;
    static const struct { const char *n; void *f; } t[] = {
        {"_ZN2nn2vi10InitializeEv", (void *)vi_nop},
        {"_ZN2nn2vi27InitializeWithVrrLfcEnabledEv", (void *)vi_nop},
        {"_ZN2nn2vi8FinalizeEv", (void *)vi_nop},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", (void *)vi_open_display},
        {"_ZN2nn2vi11OpenDisplayEPPNS0_7DisplayEPKc", (void *)vi_open_named},
        {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", (void *)vi_nop},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", (void *)vi_create_layer},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", (void *)vi_create_layer_wh},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", (void *)vi_create_layer_cs},
        {"_ZN2nn2vi18CreateSharingLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", (void *)vi_special_layer},
        {"_ZN2nn2vi20CreateRecordingLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", (void *)vi_special_layer},
        {"_ZN2nn2vi21CreateMovieMakerLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", (void *)vi_special_layer},
        {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", (void *)vi_nop},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", (void *)vi_native_window},
        {"_ZN2nn2vi16GetInternalLayerEPKNS0_5LayerE", (void *)vi_internal_layer},
        {"_ZN2nn2vi16SetLayerUserDataEPNS0_5LayerEm", (void *)vi_user_data},
        {"_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", (void *)vi_frame_no},
        {"_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", (void *)vi_frame_hist},
        {"_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", (void *)vi_ok_vrr},
        {"_ZN2nn2vi23SetDisplayHdrCapabilityEPNS0_7DisplayEb", (void *)vi_ok_cap},
        {"_ZN2nn2vi30SetDisplayHdrCapabilityPrivateEPNS0_7DisplayEb", (void *)vi_ok_cap},
        {"_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb", (void *)vi_ok_cap},
        {"_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", (void *)vi_ok_hdr},
        {"_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", (void *)vi_ok_hdr},
        {"_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", (void *)vi_hdr_status},
        {"_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", (void *)vi_paper_white},
        {"_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE", (void *)vi_max_res},
        {"_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", (void *)vi_vrr_supported},
        {"_ZN2nn2vi30QueryDisplayCurrentRefreshRateEPNS0_11RefreshRateEPKNS0_7DisplayE", (void *)vi_refresh},
        {"_ZN2nn2vi30QueryDisplayCurrentRefreshRateEPNS0_11RefreshRateEPbPKNS0_7DisplayE", (void *)vi_refresh2},
        {"_ZN2nn2vi33QueryDisplayAvailableRefreshRatesEPiPNS0_20AvailableRefreshRateEiPNS0_7DisplayE", (void *)vi_avail_refresh},
        {"_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", (void *)vi_ext_swap},
        {"_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", (void *)vi_ext_swap_clear},
        {"_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", (void *)vi_hdr_gamut},
        {"_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", (void *)vi_layer_clear},
        {"_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", (void *)vi_hdr_tonemap},
        {"_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", (void *)vi_layer_clear},
        {"_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", (void *)vi_hdr_bezier},
        {"_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", (void *)vi_layer_clear},
        {"_ZN2nn2vi30InitializeCopyrightFramebufferEPvm", (void *)vi_copyright_init},
        {"_ZN2nn2vi28FinalizeCopyrightFramebufferEv", (void *)vi_nop},
        {"_ZN2nn2vi27WriteToCopyrightFramebufferEPKvmiiiiNS0_10OriginModeE", (void *)vi_copyright_write},
        {"_ZN2nn2vi30SetCopyrightCompositionEnabledEb", (void *)vi_copyright_enable},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(name, t[i].n)) return t[i].f;
    if (!strcmp(name, "_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE") && g_vi_vsync) return g_vi_vsync;
    if (vi_pure(name)) return NULL;
    // 类成员 / detail：没有「原路径必中止」的实证（模拟执行用的是 sdk 自己的空对象），不做红桩，照原路径、打红行（4 款都没导入）
    if (vi_internal(name)) { LOG("【红】直连：Switch 2 游戏导入 nn::vi 内部实现 %s：收 sdk 自己的显示 / 图层对象，直连档的是宿主静态句柄；照原路径", name); return NULL; }
    LOG("【红】直连：Switch 2 游戏导入 %s，宿主没接管：照原路径进 sdk（要 nn::gs 已启动，会中止）", name);
    return NULL;
}

void nvn2_device_init(void) {
    if (!nvn2_on()) return;
    nvn_gob_switch2 = getenv("DM_NVN_GOB_X1") ? 0 : 1;   // DM_NVN_GOB_X1=1 (diagnostic): the Tegra X1's order, to compare against on a Switch 2 game
    LOG("[nvn] Switch 2 进程：NVN2 设备（贴图按 Switch 2 的 GOB 内次序解交错，着色器按 NVN2 控制段取键）");
}
