// Vulkan 后端（nvn_gpu_*）D8 第一段验收：直接调 D0 原语，不经过任何 OP 解码器。
// 覆盖设计文档 §8：C01（32 位填充）、C02/C03（失败终止、失败段不回写）、C04（发布顺序）、
// C05/C06（录制未提交保活）、C08（cmd_abort）、C23 的粘滞错误部分，以及 close 义务。
// 期望值全部是 CPU 手算 / 契约条文，不取自被测实现。
// RTLD_NEXT（dlsym，见下方 vkGetPhysicalDeviceSurfaceCapabilitiesKHR）在 glibc 上是 GNU 扩展，
// -std=c11 会把它关掉，故在包含任何头文件之前开 _GNU_SOURCE。
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nvn_backend.h"
#include "nvn_unimpl.h"
#include "nvn_vk_format.h"
#include "nvn_vk_bcdec.h"
#include "nvn_fmt_info.h"
#include "vk_device.h"
#include "spirv_storage.h"   // s2a_spirv_declares_cap（C1340）
#include "nvn_plat_vk.h"
#include "nvn_vk_pipe_warm.h"
#include "nvn_vk_pipe_demand.h"

// 平台件替身：无头表面（lavapipe 没有窗口系统）。交换链、取图、呈现段、信号量与布局都是真的，校验层照查。
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) {
    static const char *const exts[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    *n = 2; return exts;
}
VkResult nvn_plat_vk_surface_create(VkInstance instance, void *native, VkSurfaceKHR *out) {
    (void)native;
    VkHeadlessSurfaceCreateInfoEXT ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    return vkCreateHeadlessSurfaceEXT(instance, &ci, NULL, out);
}
int nvn_plat_window_size(void *native, uint32_t *w, uint32_t *h) { (void)native; *w = 320; *h = 180; return 1; }
// S2A_TEST_NVK_DQ=1：整套用例在「两级回放开」下再跑一遍（run.sh 第二遍）
// S2A_TEST_NVK_DQ=1：整套用例在「两级回放开」下再跑一遍；S2A_TEST_DESC_MEMO=0：在「描述符组备忘关」下再跑一遍（生产缺省是开，整套默认就在开下跑；
// 开着时内容相同的组不再分配、UBO 走动态偏移，按「分配了几组」写的那几条期望在默认这遍里不比，像素期望照比；关的那遍保留原来的分配数期望）。
static int g_memo_on = 1;        // 整套用例这一遍 nvk_desc_memo 的取值：main 开头按环境变量 S2A_TEST_DESC_MEMO 定（没设 = 1，生产缺省）
static int g_dq_dflt_seen = -1;     // 后端传给 nvn_plat_flag("nvk_dq", dflt) 的缺省值（C-DQ-DFLT 核它 = 1：两级回放缺省开）
static int g_memo_dflt_seen = -1;   // 没设环境变量时后端传给 nvn_plat_flag("nvk_desc_memo", dflt) 的缺省值（C208 核它 = 1：缺省开）
static int g_shown_timing_test;
static int g_land_test_on;
void nvn_plat_safepoint(void) {}
static int g_upscale_flag = -1;   // -1 = 后端缺省（关），>= 0 = 读成这个值
static int g_compute_warm = -1;
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
static int g_creation_feedback_off;
#endif
static int g_plc_flag_off;   // C88：1 = 调试开关 nvk_pipe_cache 读成 0（管线缓存关）
// C207：诊断读数互不清零。g_c207_gputime = 1 时调试开关 nvk_gputime 读成 1（开会话之后才置，免得会话开着 GPU 时间戳）；
// g_c207_cap = 1 时 nvn_plat_log 把「近 300 帧」读数里的描述符行存下来（g_c207_win），并核每一行「每笔读数」的每笔描述符条数 / 组数（g_c207_nlog、g_c207_badlog）。
static atomic_int g_c207_gputime, g_c207_cap;   // 别的线程（回放 / 完成 / 录制）也会调 nvn_plat_flag / nvn_plat_log：这两个用原子读写
static char g_c207_win[12][512]; static int g_c207_nwin;   // 下面几个只由绘制 / 呈现所在的（测试）线程写
static int g_c207_nlog, g_c207_badlog; static char g_c207_firstbad[512];
// C89（局中补写）：-1 = 用后端缺省；>= 0 = 把对应的调试开关读成这个值（后端在开会话时读一次，所以要在 open 之前设、open 之后撤）。
static int g_plc_thr_new = -1, g_plc_thr_first_ms = -1, g_plc_thr_every_ms = -1;
static int g_stg_std_flag = -1;   // C202：-1 = 后端缺省；>= 0 = 把调试开关 nvk_stg_std 读成这个值（后端在开会话时读一次，所以要在 open 之前设）
static int g_ulist_flag = -1, g_fbdrop_flag = -1;   // C201：nvk_ulist：-1 = 生产缺省，>= 0 = 读成这个值；nvk_fbdrop：-1 = 自检档（整套用例的默认），-2 = 生产缺省，>= 0 = 读成这个值（改完要让后端的开关缓存作废：nvn_vk_dbg_view_stat(5)）
static int g_dsetrec_flag = -1;   // C204：nvk_dset_rec：-1 = 整套用例的默认（环境变量 S2A_TEST_DSET_REC=0 / 1 指定；没设就读生产缺省），-2 = 生产缺省，>= 0 = 读成这个值（后端在开会话时读一次）
static int g_pool_rec_flag = -1;    // 整池组回收 nvk_pool_rec：-1 = 后端缺省（开），>= 0 = 读成这个值（t_pool_table.c 的回收用例用；改完调 nvn_gpu_vk_test_pool_memo_refresh）
static int g_pool_memo_flag = -1;   // 整池数组组备忘 nvk_pool_memo：-1 = 后端缺省（开），>= 0 = 读成这个值（t_pool_table.c 的备忘用例用；后端每 256 次绑定重读，改完调 nvn_gpu_vk_test_pool_memo_refresh 立即作废缓存）
static int g_roi_fixture;
int nvn_plat_flag(const char *name, int dflt) {
    if (g_roi_fixture) {
        if (!strcmp(name, "nvk_roi_probe")) return g_roi_fixture == 2;
        if (!strcmp(name, "nvk_roi_frame")) return 2;
        if (!strcmp(name, "nvk_roi_x")) return 3;
        if (!strcmp(name, "nvk_roi_y")) return 5;
    }
    if (!strcmp(name, "nvk_land_readback") && g_land_test_on) return g_land_test_on;
    if (!strcmp(name, "nvk_pool_memo") && g_pool_memo_flag >= 0) return g_pool_memo_flag;
    if (!strcmp(name, "nvk_pool_rec") && g_pool_rec_flag >= 0) return g_pool_rec_flag;
    if (!strcmp(name, "nvk_compute_warm")) return g_compute_warm < 0 ? dflt : g_compute_warm;
    if (!strcmp(name, "display_timing") && g_shown_timing_test) return 1;
    if (!strcmp(name, "nvk_dset_rec")) {
        if (g_dsetrec_flag >= 0) return g_dsetrec_flag;
        if (g_dsetrec_flag == -1 && getenv("S2A_TEST_DSET_REC")) return atoi(getenv("S2A_TEST_DSET_REC")) != 0;
        return dflt;
    }
    if (!strcmp(name, "nvk_ulist") && g_ulist_flag >= 0) return g_ulist_flag;
    // 整套用例在 nvk_fbdrop=2（自检档）下跑：视图销毁走快路之后再全表扫一遍，扫到残留（反查表没记到的帧缓冲）就报红，main 末尾核「残留 0」。
    if (!strcmp(name, "nvk_fbdrop") && g_fbdrop_flag != -2) return g_fbdrop_flag >= 0 ? g_fbdrop_flag : 2;   // -2 = 读成生产缺省（C201 核缺省档的行为）
    if (!strcmp(name, "nvk_stg_std") && g_stg_std_flag >= 0) return g_stg_std_flag;
    if (!strcmp(name, "nvk_pipe_cache") && g_plc_flag_off) return 0;
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
    if (!strcmp(name, "nvk_pipe_creation_feedback") && g_creation_feedback_off) return 0;
#endif
    if (!strcmp(name, "nvk_gputime") && atomic_load_explicit(&g_c207_gputime, memory_order_relaxed)) return 1;
    if (!strcmp(name, "nvk_upscale") && g_upscale_flag >= 0) return g_upscale_flag;
    if (!strcmp(name, "nvk_plc_new") && g_plc_thr_new >= 0) return g_plc_thr_new;
    if (!strcmp(name, "nvk_plc_first_ms") && g_plc_thr_first_ms >= 0) return g_plc_thr_first_ms;
    if (!strcmp(name, "nvk_plc_every_ms") && g_plc_thr_every_ms >= 0) return g_plc_thr_every_ms;
    if (!strcmp(name, "nvk_gpu_deswizzle") && getenv("S2A_TEST_GPU_DESWIZZLE_OFF")) return 0;
    if (!strcmp(name, "nvk_dq")) { g_dq_dflt_seen = dflt; return getenv("S2A_TEST_NVK_DQ") ? 1 : 0; }   // 两档都由用例显式定（生产缺省开，见 C-DQ-DFLT），不跟缺省走
    if (!strcmp(name, "nvk_desc_memo")) {
        if (getenv("S2A_TEST_DESC_MEMO")) return g_memo_on;
        g_memo_dflt_seen = dflt;   // 没指定：读后端自己的缺省，核它是开（C208）
        return dflt;
    }
    return dflt;
}
static int g_shown_empty_logged;
static unsigned g_msaa_log_2, g_msaa_log_8;
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level;
    if (text && strstr(text, "msaa_fallback:")) {
        if (strstr(text, "请求 2 重采样")) g_msaa_log_2++;
        if (strstr(text, "请求 8 重采样")) g_msaa_log_8++;
    }
    if (text && strstr(text, "分时（实际上屏）") && strstr(text, "帧率= ") && strstr(text, "扩展未启用或不支持")) g_shown_empty_logged++;
    if (atomic_load_explicit(&g_c207_cap, memory_order_relaxed) && text) {   // C207：下面两种行只由绘制 / 呈现所在的（测试）线程打，不加锁
        if (strstr(text, "描述符细分（每帧毫秒，近 300 帧）")) {
            if (g_c207_nwin < 12) snprintf(g_c207_win[g_c207_nwin], sizeof g_c207_win[0], "%s", text);
            g_c207_nwin++;
        } else if (strstr(text, "后端逐笔（微秒/笔，共 ")) {
            const char *d = strstr(text, "描述符更新 "); double per = -1, sets = -1;
            if (d) sscanf(d, "描述符更新 %lf 笔/笔（%lf 套）", &per, &sets);
            g_c207_nlog++;
            if (!(per > 1.995 && per < 2.005 && sets > 0.95 && sets < 1.05) && !g_c207_badlog++) snprintf(g_c207_firstbad, sizeof g_c207_firstbad, "%s", text);
        }
    }
    printf("[%s] %s\n", cls ? cls : "", text ? text : "");
}
static int g_tri_origin_ul;   // 1 = 按 NVN 窗口原点在左上画（视口翻转）：期望上下对调。缺省 0 = 原点在左下（Vulkan 的自然方向）

// C66 only: model a strict driver enforcing maxSets=64; all other calls
// reach the real lavapipe entry. This is injected behaviour, not a claim about llvmpipe.
static int g_c66_inject_pool_limit;
static unsigned g_c66_sets_requested, g_c66_rejects;
static VkDescriptorPool g_c66_pool;
static unsigned g_desc_sets_total;
// 描述符组按布局回收（#1096）的读数钩子（后端导出，见 backend_vulkan.c）：0 新分 / 1 复用 / 2 放回 / 3 放弃的段还给池 / 4 交给 alloc_sets 的总数 /
// 5 空闲串里的组 / 6 表项数 / 7 增长池个数 / 8 dead 的表项数 / 9 回收是否开着（开会话时读的开关）/ 10 到总量上限退回老路径的次数 / 13 沿登记表遍历数出来的表项数（应恒等于 6）/ 26 当前的总量上限 /
// 布局表：19 布局结构数 / 20 建管线取布局的次数 / 21 其中命中 / 22 散列桶数 / 23 最长桶链 / 24 进程内累计向驱动建过的布局数 / 25 累计销毁的布局数
uint64_t nvn_vk_dbg_dset_stat(int which);
uint64_t nvn_vk_dbg_dyn_stat(int which);   // C208：0 = 选了动态 UBO 的管线条数、1 = 建过的带 UBO 的管线条数（进程内累计）、2 = 当前的动态 UBO 上限（设备值，或被下面压低后的值）、3 = 绑定表里「只换偏移」直接放过的项数（进程内累计）
void nvn_vk_dbg_dyn_limit(uint32_t v);     // C208：把上限压低到 v（只降不升），0 = 还原设备值
void nvn_vk_dbg_dyn_split(uint32_t bits);  // C208：把动态偏移的拆分点（低位走动态偏移、高位放描述符）从 32 位压到 bits 位，0 = 还原 32 位
uint64_t nvn_vk_dbg_dlay_get(const uint32_t *k4, uint32_t nb);   // 直接按结构取一个布局（不经管线）：nb 条 {binding, 类型, 数量, 阶段}，返回布局句柄的数值，0 = 失败
void nvn_vk_dbg_dset_hold(int on);     // 1 = 测试线程自己持着回收表的锁（取组碰锁退回老路径）；0 = 放开。持着期间只能读 nvn_vk_dbg_dset_stat(9 / 11)
void nvn_vk_dbg_dset_inject(int mode);   // 0 不注入；1 一批（> 1 组）向驱动要组时回「池耗尽」；2 任何一次向增长池要组都回「池耗尽」
void nvn_vk_dbg_dset_cap(uint32_t n);   // 把总量上限改成 n 组（本会话内有效，下次开会话复位）
void nvn_vk_dbg_dset_failreg(int n);   // 接下来 n 次登记表项当作内存不够（0 = 取消）：被注入的取组退回老路径、不记结果，下一次取组再登记；每个臂收尾时取消
// 后端「要了几组」：驱动里分出去的组数，扣掉回收表整批多分的、加上回收表交出去的（回收开着时一次向驱动分一批，驱动计数不等于后端要了几组）。
// 只用差值：回收表的计数每个会话从 0 起，驱动计数是整个进程的。
static unsigned desc_demand(void) { return g_desc_sets_total - (unsigned)nvn_vk_dbg_dset_stat(0) + (unsigned)nvn_vk_dbg_dset_stat(4); }
VKAPI_ATTR VkResult VKAPI_CALL __real_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *sets) {
    if (g_c66_inject_pool_limit) {
        if (g_c66_pool != info->descriptorPool) { g_c66_pool = info->descriptorPool; g_c66_sets_requested = 0; }
        g_c66_sets_requested += info->descriptorSetCount;
        if (g_c66_sets_requested > 64) { g_c66_rejects++; return VK_ERROR_OUT_OF_POOL_MEMORY; }
    }
    VkResult rc = __real_vkAllocateDescriptorSets(device, info, sets);
    if (rc == VK_SUCCESS) g_desc_sets_total += info->descriptorSetCount;
    return rc;
}

#ifdef S2A_CMDPOOL_TEST
// 跟踪真实驱动分配；只在本用例会话启用，按池统计存活命令缓冲。
static pthread_mutex_t g_cmdpool_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_cmdpool_track;
static struct { VkCommandPool pool; unsigned live; } g_cmdpool[256];
static unsigned g_cmdpool_peak, g_cmdpool_resets, g_cmdpool_overflow;
static atomic_int g_cmdpool_fail_begin, g_cmdpool_failed_begin;
VKAPI_ATTR VkResult VKAPI_CALL __real_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    if (atomic_exchange(&g_cmdpool_fail_begin, 0)) { atomic_fetch_add(&g_cmdpool_failed_begin, 1); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    return __real_vkBeginCommandBuffer(cb, bi);
}
VKAPI_ATTR VkResult VKAPI_CALL __real_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
VKAPI_ATTR void VKAPI_CALL __real_vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);
VKAPI_ATTR void VKAPI_CALL __real_vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL __real_vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags);
static unsigned cmdpool_index(VkCommandPool p) {
    unsigned empty = 256;
    for (unsigned i = 0; i < 256; i++) {
        if (g_cmdpool[i].pool == p) return i;
        if (!g_cmdpool[i].pool && empty == 256) empty = i;
    }
    if (empty == 256) { g_cmdpool_overflow++; return empty; }
    g_cmdpool[empty].pool = p;
    return empty;
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *a, VkCommandBuffer *b) {
    VkResult r = __real_vkAllocateCommandBuffers(d, a, b);
    pthread_mutex_lock(&g_cmdpool_mu);
    if (g_cmdpool_track && r == VK_SUCCESS) {
        unsigned i = cmdpool_index(a->commandPool);
        if (i < 256) { g_cmdpool[i].live += a->commandBufferCount; if (g_cmdpool[i].live > g_cmdpool_peak) g_cmdpool_peak = g_cmdpool[i].live; }
    }
    pthread_mutex_unlock(&g_cmdpool_mu);
    return r;
}
VKAPI_ATTR void VKAPI_CALL __wrap_vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *b) {
    __real_vkFreeCommandBuffers(d, p, n, b);
    pthread_mutex_lock(&g_cmdpool_mu);
    if (g_cmdpool_track) { unsigned i = cmdpool_index(p); if (i < 256) { if (g_cmdpool[i].live < n) g_cmdpool_overflow++; else g_cmdpool[i].live -= n; } }
    pthread_mutex_unlock(&g_cmdpool_mu);
}
VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) {
    __real_vkDestroyCommandPool(d, p, a);
    pthread_mutex_lock(&g_cmdpool_mu);
    if (g_cmdpool_track) { unsigned i = cmdpool_index(p); if (i < 256) { g_cmdpool[i].pool = VK_NULL_HANDLE; g_cmdpool[i].live = 0; } }
    pthread_mutex_unlock(&g_cmdpool_mu);
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    VkResult r = __real_vkResetCommandPool(d, p, f);
    pthread_mutex_lock(&g_cmdpool_mu);
    if (g_cmdpool_track && r == VK_SUCCESS) { unsigned i = cmdpool_index(p); if (i < 256 && g_cmdpool[i].live >= 2) g_cmdpool_resets++; }
    pthread_mutex_unlock(&g_cmdpool_mu);
    return r;
}

#endif

// C88（#822 驱动管线缓存）：包住建缓存 / 取缓存数据 / 建图形管线 / 建计算管线四个入口，只记录并转给真函数。
// 记下：每次建缓存时交给驱动的初始数据字节数、建出的句柄、取数据拿到的字节数，以及每次建管线调用传的是不是那个缓存。
// reject_initial 是注入：让「带初始数据」的建缓存调用失败，模拟驱动不收旧数据（其余调用走真实 lavapipe）。
struct plc_rec {
    unsigned cache_creates; size_t last_initial; VkPipelineCache last_cache; unsigned reject_initial;
    size_t last_data;
    unsigned gp, gp_cached, cp, cp_cached;
};
static struct plc_rec g_plc_rec;
// C89（局中补写）：lavapipe 的缓存数据恒为 32 字节文件头，不随建管线变大，「数据变没变」这类判断在它上面看不出区别。
// 所以把「驱动的缓存数据有多大」交给用例定：取数据时在真数据后面补 g_plc_extra + g_plc_per_pipe × 自 g_plc_pipe_base 起建的管线数 个填充字节
// （模拟真驱动的缓存随管线变大）；下一局读回时（带着填充）建缓存前把填充去掉再交给 lavapipe（g_plc_strip_tail）。
// 三个值缺省全 0 = 原样转给真函数，C88 等其余用例不受影响。
static size_t g_plc_extra, g_plc_per_pipe;
static unsigned g_plc_pipe_base;
static int g_plc_strip_tail;
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo *, const VkAllocationCallbacks *, VkPipelineCache *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    g_plc_rec.cache_creates++;
    g_plc_rec.last_initial = ci->initialDataSize;
    if (g_plc_rec.reject_initial && ci->initialDataSize) { g_plc_rec.reject_initial--; return VK_ERROR_INITIALIZATION_FAILED; }
    VkPipelineCacheCreateInfo ci2;
    if (g_plc_strip_tail && ci->pInitialData && ci->initialDataSize > 4) {   // 文件头第一个 u32 = 头长度（VkPipelineCacheHeaderVersionOne = 32）
        uint32_t hs; memcpy(&hs, ci->pInitialData, sizeof hs);
        if (hs >= 16 && hs < ci->initialDataSize) { ci2 = *ci; ci2.initialDataSize = hs; ci = &ci2; }
    }
    VkResult r = __real_vkCreatePipelineCache(device, ci, a, out);
    if (r == VK_SUCCESS) g_plc_rec.last_cache = *out;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL __real_vkGetPipelineCacheData(VkDevice, VkPipelineCache, size_t *, void *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache, size_t *size, void *data) {
    size_t extra = g_plc_extra;
    if (g_plc_per_pipe)
        extra += g_plc_per_pipe * (size_t)(__atomic_load_n(&g_plc_rec.gp, __ATOMIC_RELAXED) + __atomic_load_n(&g_plc_rec.cp, __ATOMIC_RELAXED) - g_plc_pipe_base);
    VkResult r;
    if (!extra) {
        r = __real_vkGetPipelineCacheData(device, cache, size, data);
        if (data && r == VK_SUCCESS) __atomic_store_n(&g_plc_rec.last_data, *size, __ATOMIC_RELAXED);   // C89：局中补写在建管线的线程里取数据，两个线程同时建管线时这里会并发
        return r;
    }
    size_t rs = 0;   // 真数据大小
    r = __real_vkGetPipelineCacheData(device, cache, &rs, NULL);
    if (r != VK_SUCCESS) return r;
    if (!data) { *size = rs + extra; return VK_SUCCESS; }
    size_t cap = *size, w = cap < rs ? cap : rs;
    r = __real_vkGetPipelineCacheData(device, cache, &w, data);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
    size_t fill = 0;
    if (w == rs) { fill = cap - w < extra ? cap - w : extra; memset((char *)data + rs, 0xA5, fill); }
    *size = w + fill;
    if (w == rs && fill == extra) { __atomic_store_n(&g_plc_rec.last_data, *size, __ATOMIC_RELAXED); return VK_SUCCESS; }
    return VK_INCOMPLETE;   // 容量不够（取大小之后又有新管线）：按规范只写放得下的部分
}
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
// Only the isolated no-extension arm changes what the application sees. All devices and
// pipelines are still created by lavapipe through the active validation layer.
static int g_hide_creation_feedback;
VKAPI_ATTR VkResult VKAPI_CALL __real_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (!g_hide_creation_feedback || layer)
        return __real_vkEnumerateDeviceExtensionProperties(physical, layer, count, props);
    uint32_t total = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(physical, NULL, &total, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(total ? total : 1, sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = __real_vkEnumerateDeviceExtensionProperties(physical, NULL, &total, all);
    if (r == VK_SUCCESS) {
        uint32_t found = 0, cap = props ? *count : 0;
        for (uint32_t i = 0; i < total; i++) {
            if (!strcmp(all[i].extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME)) continue;
            if (props && found < cap) props[found] = all[i];
            found++;
        }
        *count = props && found > cap ? cap : found;
        if (props && found > cap) r = VK_INCOMPLETE;
    }
    free(all);
    return r;
}

static int g_inspect_creation_feedback;
static unsigned g_creation_feedback_chained[2];
static unsigned g_creation_feedback_real_valid[2];
static VkPipelineCreationFeedback *find_creation_feedback(const void *next) {
    for (const VkBaseInStructure *p = next; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO)
            return ((const VkPipelineCreationFeedbackCreateInfo *)p)->pPipelineCreationFeedback;
    }
    return NULL;
}
// Keep the real Vulkan result and object. Replace only returned feedback for deterministic
// accounting assertions; an invalid sample's other flag bits and duration are undefined.
static void inspect_creation_feedback(unsigned compute, const void *next, VkPipelineCache cache) {
    VkPipelineCreationFeedback *feedback = find_creation_feedback(next);
    if (!feedback) return;
    g_creation_feedback_real_valid[compute] += !!(feedback->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT);
    const VkPipelineCreationFeedbackFlags valid = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
    const VkPipelineCreationFeedbackFlags hit = cache ? VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT : 0;
    const VkPipelineCreationFeedback gfx[] = {{valid | hit, 100}, {hit, 1000000}, {valid, 20}, {valid | hit, 40}};
    const VkPipelineCreationFeedback cs[] = {{valid, 80}, {0, 2000000}};
    const unsigned idx = g_creation_feedback_chained[compute]++;
    if (!compute && idx < sizeof gfx / sizeof gfx[0]) *feedback = gfx[idx];
    if (compute && idx < sizeof cs / sizeof cs[0]) *feedback = cs[idx];
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    __atomic_fetch_add(&g_plc_rec.gp, n, __ATOMIC_RELAXED);   // C89 ⑩：两个线程同时建管线
    if (cache != VK_NULL_HANDLE && cache == g_plc_rec.last_cache) __atomic_fetch_add(&g_plc_rec.gp_cached, n, __ATOMIC_RELAXED);
    const VkResult r = __real_vkCreateGraphicsPipelines(device, cache, n, ci, a, out);
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
    if (g_inspect_creation_feedback && r == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) inspect_creation_feedback(0, ci[i].pNext, cache);
#endif
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
static pthread_mutex_t g_cs_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cs_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_cs_gate_on, g_cs_gate_entered, g_cs_gate_release;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    pthread_mutex_lock(&g_cs_gate_mu);
    if (g_cs_gate_on) {
        g_cs_gate_entered = 1; pthread_cond_broadcast(&g_cs_gate_cv);
        while (!g_cs_gate_release) pthread_cond_wait(&g_cs_gate_cv, &g_cs_gate_mu);
    }
    pthread_mutex_unlock(&g_cs_gate_mu);
    __atomic_fetch_add(&g_plc_rec.cp, n, __ATOMIC_RELAXED);
    if (cache != VK_NULL_HANDLE && cache == g_plc_rec.last_cache) __atomic_fetch_add(&g_plc_rec.cp_cached, n, __ATOMIC_RELAXED);
    const VkResult r = __real_vkCreateComputePipelines(device, cache, n, ci, a, out);
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
    if (g_inspect_creation_feedback && r == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) inspect_creation_feedback(1, ci[i].pNext, cache);
#endif
    return r;
}

// 限定注入：真实 R32_UINT 支持不变，仅模拟驱动只报告单一 texel 能力。
static VkFormatFeatureFlags g_texel_feature_mask;
VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice device, VkFormat format, VkFormatProperties *props) {
    __real_vkGetPhysicalDeviceFormatProperties(device, format, props);
    if (g_texel_feature_mask && format == VK_FORMAT_R32_UINT)
        props->bufferFeatures &= ~g_texel_feature_mask;
}

extern uint32_t nvn_gpu_vk_test_push_capacity(nvn_gpu_cmd c, nvn_gpu_stage st);
extern uint32_t nvn_gpu_vk_test_buf_refs(nvn_gpu_buf b);    // #1194：缓冲现在被几段持有（句柄必须还活着）
extern uint32_t nvn_gpu_vk_test_cmd_nrefs(nvn_gpu_cmd c);   // #1194：一段命令的引用表现有几条
extern uint32_t nvn_gpu_vk_test_cmd_refseen_hits(nvn_gpu_cmd c);   // #1194：这一段里备忘命中（直接返回）了几次
extern void nvn_gpu_vk_test_ref_seen(int on);               // #1194：对照档：0 = 备忘关（每次都走原路径），1 = 产品行为
extern void nvn_gpu_vk_test_emit_counts(nvn_gpu_cmd c, uint32_t out[9]);   // #708：管线、视口、裁剪、混合常量、模板、深度偏移、顶点缓冲、索引缓冲、push 各录了几次
extern void nvn_gpu_vk_test_set_fault(int mode);
extern void nvn_gpu_vk_test_diag_timing(int on, uint64_t draw_log_ns);   // C207：直接开 / 关诊断计时，并改逐笔读数的周期（0 = 还原 5 秒）
extern void nvn_gpu_vk_test_fail_fence(uint64_t signal);
extern void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms);

static int g_fail;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            g_fail++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)
#define OK(r) ((r).code == NVN_GPU_OK)

struct done_rec { uint64_t signal; uint32_t status; int code; };
static struct done_rec g_done[64];
static atomic_int g_ndone;
static atomic_int g_order_bad;
static volatile int g_event;   // 普通变量：C04 靠 fence_wait 的同步边，TSan 才能真的检查
static atomic_int g_retired;
static atomic_int g_ndone_at_retire;
static atomic_int g_probe_order, g_order_code;

static void on_done(void *u, const nvn_gpu_done *d) {
    (void)u;
    int i = atomic_load_explicit(&g_ndone, memory_order_relaxed);   // done 回调由 worker 串行调用
    if (i < 64) { g_done[i].signal = d->signal; g_done[i].status = d->status; g_done[i].code = d->rc.code; }
    atomic_store_explicit(&g_ndone, i + 1, memory_order_release);
    // 发布顺序：成功段的 done 里，已完成值必须还没推进到本段 signal。
    if (d->status == NVN_GPU_DONE_OK && nvn_gpu_fence_done() >= d->signal) atomic_fetch_add(&g_order_bad, 1);
    if (d->status == NVN_GPU_DONE_OK) g_event = 9;
    if (atomic_load(&g_probe_order)) {
        nvn_gpu_err e;
        atomic_store(&g_order_code, nvn_gpu_fence_wait(d->signal, 0, &e).code);
    }
}

static void on_retire(void *u) {
    (void)u;
    atomic_store(&g_ndone_at_retire, atomic_load(&g_ndone));
    atomic_fetch_add(&g_retired, 1);
}

static void sleep_ms(int ms) { struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&ts, NULL); }

static int wait_until(atomic_int *v, int target, int ms) {
    for (int i = 0; i < ms; i++) {
        if (atomic_load(v) >= target) return 1;
        sleep_ms(1);
    }
    return atomic_load(v) >= target;
}

static void wait_done(int n) { CHECK(wait_until(&g_ndone, n, 5000), "等 done 回调超时：期望 %d，实到 %d", n, atomic_load(&g_ndone)); }

// 只认 base 之后的新记录：重开会话后 signal 会从头开始，旧记录不能冒充。
static const struct done_rec *wait_signal(uint64_t signal, int base, int ms) {
    for (int t = 0; t < ms; t++) {
        int n = atomic_load_explicit(&g_ndone, memory_order_acquire);
        if (n > 64) n = 64;
        for (int i = n - 1; i >= base; i--)
            if (g_done[i].signal == signal) return &g_done[i];
        sleep_ms(1);
    }
    return NULL;
}

static uint8_t *buf_cpu(nvn_gpu_buf b) { return nvn_gpu_buf_cpu(b); }

static struct s2a_vk_device g_dev;
// Inject only the optional extension: lavapipe still executes real presents,
// and never sees the simulated extension structure on its unsupported device.
static unsigned g_timing_tags, g_timing_bad;
static uint32_t g_timing_previous;
static VkSwapchainKHR g_timing_previous_swapchain;
#ifdef S2A_C883_TEST
static VKAPI_ATTR VkResult VKAPI_CALL fake_past_timing(VkDevice device, VkSwapchainKHR swapchain,
        uint32_t *count, VkPastPresentationTimingGOOGLE *timings) {
    (void)device; (void)swapchain; (void)timings; *count = 0; return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __real_vkGetDeviceProcAddr(VkDevice, const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    if (g_shown_timing_test == 1 && !strcmp(name, "vkGetPastPresentationTimingGOOGLE"))
        return (PFN_vkVoidFunction)fake_past_timing;
    return __real_vkGetDeviceProcAddr(device, name);
}
#endif
static nvn_gpu_open_desc g_desc;
extern uint64_t s2a_vk_easu_hits(void);   // backend_vulkan_easu.inc

// 保守故障注入：不等 fence 就返回 TIMEOUT，记录完成证明前的视图销毁；其余调用走真实 Vulkan。
static int g_bc_inject_wait, g_bc_unproven, g_bc_early_destroy;
static VkFence g_bc_injected_fence;
static int g_bc_early_n, g_bc_early_seen;
// 派生夹具（t_pool_table.c）用：返回 1 表示这个围栏的提交还扣在主机侧，按真超时处理、不进驱动——
// 否则后端等待线程的 vkWaitForFences 会和主线程放行时的 vkQueueSubmit 同时用这个围栏（线程校验报 MultipleThreads）。
static int (*g_fence_held)(VkFence);
// C197：现解贴图的 CPU 上传期间，在调用线程上数提交与等栅栏（后端工作线程自己的提交 / 等待不算）。
static _Thread_local int t_bc_watch;
static _Thread_local unsigned t_bc_submits, t_bc_waits;
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    PFN_vkQueueSubmit real = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(g_dev.device, "vkQueueSubmit");
    if (t_bc_watch) t_bc_submits++;
    return real(q, n, si, f);
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    PFN_vkWaitForFences real = (PFN_vkWaitForFences)vkGetDeviceProcAddr(d, "vkWaitForFences");
    if (t_bc_watch) t_bc_waits++;
    if (g_fence_held && n == 1 && g_fence_held(f[0])) {
        uint64_t ns = timeout < 1000000ull ? timeout : 1000000ull;   // 最多睡 1 毫秒，调用方会接着等
        struct timespec ts = {0, (long)ns}; nanosleep(&ts, NULL);
        return VK_TIMEOUT;
    }
    if (g_bc_inject_wait && n == 1) {   // 真超时：睡满调用方给的时限再报超时（等栅栏的包装按墙钟判，不睡满它会接着等）
        g_bc_inject_wait = 0; g_bc_unproven = 1; g_bc_injected_fence = f[0];
        struct timespec ts = {(time_t)(timeout / 1000000000ull), (long)(timeout % 1000000000ull)}; nanosleep(&ts, NULL);
        return VK_TIMEOUT;
    }
    if (g_bc_early_n > 0 && n == 1) { g_bc_early_n--; g_bc_early_seen++; return VK_TIMEOUT; }   // 提前返回（#697）：不等、立刻报超时
    return real(d, n, f, all, timeout);
}
// C196 / C198：段内解码的临时资源账（只记武装期间建的）：临时图（STORAGE | TRANSFER_SRC）、解码输入图（RG32 / RGBA32 UINT，
// TRANSFER_DST | SAMPLED）、暂存缓冲（带 UNIFORM 用途的共享缓冲，即段的 arena 块）。g_bc_fail_in = k：第 k 个解码输入图建失败。
#define BC_TRACK_MAX 512
static int g_bc_track, g_bc_fail_in, g_bc_in_seen;
static VkImage g_bc_imgs[BC_TRACK_MAX]; static int g_bc_img_live, g_bc_img_peak;
static VkBuffer g_bc_bufs[BC_TRACK_MAX]; static VkDeviceSize g_bc_buf_size[BC_TRACK_MAX]; static VkDeviceSize g_bc_stage_live, g_bc_stage_peak;
static pthread_mutex_t g_bc_track_mu = PTHREAD_MUTEX_INITIALIZER;
static int bc_track_is_transient(const VkImageCreateInfo *ci) {
    if (ci->usage == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) return 1;
    if (ci->usage == (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) &&
        (ci->format == VK_FORMAT_R32G32_UINT || ci->format == VK_FORMAT_R32G32B32A32_UINT)) return 2;
    return 0;
}
static void bc_track_reset(void) {
    pthread_mutex_lock(&g_bc_track_mu);
    memset(g_bc_imgs, 0, sizeof g_bc_imgs); memset(g_bc_bufs, 0, sizeof g_bc_bufs);
    g_bc_img_live = g_bc_img_peak = 0; g_bc_stage_live = g_bc_stage_peak = 0; g_bc_in_seen = 0;
    pthread_mutex_unlock(&g_bc_track_mu);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *a) {
    PFN_vkDestroyImage real = (PFN_vkDestroyImage)vkGetDeviceProcAddr(d, "vkDestroyImage");
    if (image) { pthread_mutex_lock(&g_bc_track_mu);
        for (int i = 0; i < BC_TRACK_MAX; i++) if (g_bc_imgs[i] == image) { g_bc_imgs[i] = VK_NULL_HANDLE; g_bc_img_live--; break; }
        pthread_mutex_unlock(&g_bc_track_mu); }
    real(d, image, a);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *buf) {
    PFN_vkCreateBuffer real = (PFN_vkCreateBuffer)vkGetDeviceProcAddr(d, "vkCreateBuffer");
    VkResult r = real(d, ci, a, buf);
    if (r == VK_SUCCESS && g_bc_track && (ci->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        pthread_mutex_lock(&g_bc_track_mu);
        for (int i = 0; i < BC_TRACK_MAX; i++) if (!g_bc_bufs[i]) { g_bc_bufs[i] = *buf; g_bc_buf_size[i] = ci->size;
            g_bc_stage_live += ci->size; if (g_bc_stage_live > g_bc_stage_peak) g_bc_stage_peak = g_bc_stage_live; break; }
        pthread_mutex_unlock(&g_bc_track_mu);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer buf, const VkAllocationCallbacks *a) {
    PFN_vkDestroyBuffer real = (PFN_vkDestroyBuffer)vkGetDeviceProcAddr(d, "vkDestroyBuffer");
    if (buf) { pthread_mutex_lock(&g_bc_track_mu);
        for (int i = 0; i < BC_TRACK_MAX; i++) if (g_bc_bufs[i] == buf) { g_bc_bufs[i] = VK_NULL_HANDLE; g_bc_stage_live -= g_bc_buf_size[i]; break; }
        pthread_mutex_unlock(&g_bc_track_mu); }
    real(d, buf, a);
}
// 只在 BC4 体积上传用例内武装，精确匹配解码输出临时图；其余创建走真实驱动。
static int g_bc4_fail_temp, g_bc4_temp_seen;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d, const VkImageCreateInfo *ci,
                                            const VkAllocationCallbacks *a, VkImage *image) {
    PFN_vkCreateImage real = (PFN_vkCreateImage)vkGetDeviceProcAddr(d, "vkCreateImage");
    if (g_bc4_fail_temp && ci->imageType == VK_IMAGE_TYPE_2D && ci->format == VK_FORMAT_R8_UNORM &&
        ci->usage == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        ++g_bc4_temp_seen == g_bc4_fail_temp) { *image = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    int kind = g_bc_track ? bc_track_is_transient(ci) : 0;
    if (kind == 2 && g_bc_fail_in && ++g_bc_in_seen == g_bc_fail_in) { *image = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    VkResult r = real(d, ci, a, image);
    if (r == VK_SUCCESS && kind) {
        pthread_mutex_lock(&g_bc_track_mu);
        for (int i = 0; i < BC_TRACK_MAX; i++) if (!g_bc_imgs[i]) { g_bc_imgs[i] = *image; if (++g_bc_img_live > g_bc_img_peak) g_bc_img_peak = g_bc_img_live; break; }
        pthread_mutex_unlock(&g_bc_track_mu);
    }
    return r;
}
// #697: synthetic driver enumeration, keeping real headless images/semaphores and VVL.
// Virtual indices 8+ map to the driver's real indices; this is NOT a device result.
static uint32_t g_w697_count, g_w697_real_count, g_w697_base = 8;
static int g_w697_fail_sem;
static int g_w697_incomplete, g_w697_invalid;
static unsigned g_w697_queries, g_w697_fetches, g_w697_created, g_w697_destroyed, g_w697_sc_destroyed;
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *images) {
    PFN_vkGetSwapchainImagesKHR real = (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(d, "vkGetSwapchainImagesKHR");
    if (!g_w697_count) return real(d, sc, n, images);
    if (!images) { g_w697_queries++; *n = g_w697_count; return VK_SUCCESS; }
    g_w697_fetches++;
    VkImage actual[64]; uint32_t nr = 64;
    VkResult r = real(d, sc, &nr, actual);
    if (r != VK_SUCCESS || !nr || nr > 6) return VK_ERROR_INITIALIZATION_FAILED;
    g_w697_real_count = nr;
    uint32_t cap = *n, written = cap < g_w697_count ? cap : g_w697_count;
    for (uint32_t i = 0; i < written; i++) images[i] = actual[(i >= g_w697_base ? i - g_w697_base : i) % nr];
    *n = written;
    if (g_w697_incomplete) { if (g_w697_incomplete > 0) g_w697_incomplete--; return VK_INCOMPLETE; }
    return cap < g_w697_count ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *idx) {
    PFN_vkAcquireNextImageKHR real = (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(d, "vkAcquireNextImageKHR");
    VkResult r = real(d, sc, timeout, sem, fence, idx);
    if (g_w697_count && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) *idx = g_w697_invalid ? g_w697_count : *idx + g_w697_base;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *sem) {
    PFN_vkCreateSemaphore real = (PFN_vkCreateSemaphore)vkGetDeviceProcAddr(d, "vkCreateSemaphore");
    if (g_w697_count && g_w697_fail_sem > 0 && !--g_w697_fail_sem) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r = real(d, ci, a, sem);
    if (g_w697_count && r == VK_SUCCESS) g_w697_created++;
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice d, VkSemaphore sem, const VkAllocationCallbacks *a) {
    PFN_vkDestroySemaphore real = (PFN_vkDestroySemaphore)vkGetDeviceProcAddr(d, "vkDestroySemaphore");
    if (g_w697_count && sem) g_w697_destroyed++;
    real(d, sem, a);
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    PFN_vkDestroySwapchainKHR real = (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(d, "vkDestroySwapchainKHR");
    if (g_w697_count && sc) g_w697_sc_destroyed++;
    real(d, sc, a);
}

// 呈现「次优」注入（C199）：呈现照常做，结果改报 VK_SUBOPTIMAL_KHR；g_wsi_rotated 让表面的当前方向报成转了 90 度。
static int g_wsi_subopt, g_wsi_rotated;
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    PFN_vkQueuePresentKHR real = (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(g_dev.device, "vkQueuePresentKHR");
    VkPresentInfoKHR mapped = *pi; uint32_t idx;
    if (g_shown_timing_test && mapped.pNext) {
        const VkPresentTimesInfoGOOGLE *times = mapped.pNext;
        if (times->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) {
            if (times->swapchainCount != 1 || !times->pTimes || !times->pTimes[0].presentID ||
                times->pTimes[0].desiredPresentTime != 0 ||
                (g_timing_previous_swapchain == pi->pSwapchains[0] &&
                 times->pTimes[0].presentID == g_timing_previous)) g_timing_bad++;
            else { g_timing_previous = times->pTimes[0].presentID; g_timing_previous_swapchain = pi->pSwapchains[0]; }
            g_timing_tags++;
            mapped.pNext = times->pNext;
        }
    }
    if (g_w697_count && pi->swapchainCount == 1) {
        idx = pi->pImageIndices[0];
        CHECK(idx >= g_w697_base && idx < g_w697_count, "C697 present virtual index %u outside range", idx);
        idx = (idx >= g_w697_base ? idx - g_w697_base : idx) % g_w697_real_count;
        mapped.pImageIndices = &idx;
    }
    VkResult r = real(q, &mapped);
    return r == VK_SUCCESS && g_wsi_subopt ? VK_SUBOPTIMAL_KHR : r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR sf, VkSurfaceCapabilitiesKHR *caps) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR real = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)dlsym(RTLD_NEXT, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkResult r = real(pd, sf, caps);
    if (r == VK_SUCCESS && g_wsi_rotated) caps->currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView view, const VkAllocationCallbacks *a) {
    PFN_vkDestroyImageView real = (PFN_vkDestroyImageView)vkGetDeviceProcAddr(d, "vkDestroyImageView");
    if (g_bc_unproven && view) g_bc_early_destroy++;
    real(d, view, a);
}

static void bc_timeout_lifetime(void) {
    VkDevice d = g_dev.device; VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
    struct nvn_vk_bcdec *bc = NULL; char why[192] = {0};
    VkImageCreateInfo ii = {0}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = (VkExtent3D){4,4,1};
    ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult vr = vkCreateImage(d, &ii, NULL, &image); CHECK(vr == VK_SUCCESS, "BC timeout 建图：%d", vr); if (vr != VK_SUCCESS) goto done;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(d, image, &mr);
    VkMemoryAllocateInfo ma = {0}; ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize = mr.size;
    ma.memoryTypeIndex = s2a_vk_memory_type(&g_dev, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (ma.memoryTypeIndex == UINT32_MAX) { CHECK(0, "BC timeout 无显存类型"); goto done; }
    vr = vkAllocateMemory(d, &ma, NULL, &memory); CHECK(vr == VK_SUCCESS, "BC timeout 分配：%d", vr); if (vr != VK_SUCCESS) goto done;
    vr = vkBindImageMemory(d, image, memory, 0); CHECK(vr == VK_SUCCESS, "BC timeout 绑定：%d", vr); if (vr != VK_SUCCESS) goto done;
    int br = nvn_vk_bcdec_create(&g_dev, &bc, why, sizeof why); CHECK(br == 0, "BC timeout 解码器：%s", why); if (br) goto done;
    { static const uint8_t block[8] = {0,0xf8,0,0xf8,0,0,0,0};
      // #697：驱动的等待被信号打断、提前报超时（这里连着 3 次立刻返回 VK_TIMEOUT）→ 没到墙钟时限必须接着等，解码照常成功
      g_bc_early_n = 3; g_bc_early_seen = 0;
      br = nvn_vk_bcdec_decode(bc, &g_dev, image, NVN_FORMAT_RGBA_DXT1, 4, 4, 0, 0, block, sizeof block, why, sizeof why);
      CHECK(br == 0 && g_bc_early_seen == 3 && g_bc_early_n == 0, "BC 等待提前返回 3 次：期望接着等、解码成功（0）且 3 次都被消耗 → 返回 %d、消耗 %d 次 %s", br, g_bc_early_seen, why);
      g_bc_early_n = 0;
      g_bc_inject_wait = 1; g_bc_unproven = g_bc_early_destroy = 0; g_bc_injected_fence = VK_NULL_HANDLE;
      br = nvn_vk_bcdec_decode(bc, &g_dev, image, NVN_FORMAT_RGBA_DXT1, 4, 4, 0, 0, block, sizeof block, why, sizeof why);
      CHECK(br == -2 && g_bc_injected_fence, "BC timeout 应报告未证明完成，实得 %d %s", br, why);
      CHECK(g_bc_early_destroy == 0, "BC timeout 完成证明前销毁视图：%d", g_bc_early_destroy);
      g_bc_inject_wait = 1;
      nvn_vk_bcdec_destroy(&g_dev, bc);  // 第二次等待仍失败：解码器和视图也必须继续保活
      CHECK(g_bc_early_destroy == 0, "BC destroy 等待失败时提前销毁视图：%d", g_bc_early_destroy);
      if (g_bc_injected_fence) {
          vr = vkWaitForFences(d, 1, &g_bc_injected_fence, VK_TRUE, 5000000000ull);
          CHECK(vr == VK_SUCCESS, "BC timeout 后真实 fence 完成：%d", vr);
      }
      g_bc_unproven = 0;
      printf("BC timeout 注入用例已执行：rc=%d，提前销毁=%d\n", br, g_bc_early_destroy); }
done:
    g_bc_inject_wait = g_bc_unproven = 0;
    if (bc) nvn_vk_bcdec_destroy(&g_dev, bc);
    if (image) vkDestroyImage(d, image, NULL);
    if (memory) vkFreeMemory(d, memory, NULL);
}

static int open_backend(void) {
    nvn_gpu_err e;
    memset(&e, 0, sizeof e);
    nvn_gpu_rc r = nvn_gpu_open(&g_desc, &e);
    if (!OK(r)) printf("open 失败：%d %s\n", r.code, e.why);
    return OK(r);
}

#ifdef S2A_BACKEND_PIPE_CALL_TEST
extern void nvn_vk_pipe_call_test_hold(int hold);
extern int nvn_vk_pipe_call_test_wait(uint64_t timeout_ns);
struct pipe_call_test_arg { int compute; nvn_gpu_rc result; nvn_gpu_err err; };
static void *pipe_call_test_main(void *opaque) {
    struct pipe_call_test_arg *a = opaque;
    nvn_gpu_pipe out = NULL;
    a->result = a->compute ? nvn_gpu_cpipe_get(NULL, 0, &out, &a->err) : nvn_gpu_pipe_get(NULL, 0, &out, &a->err);
    return NULL;
}

static void pipe_call_lifetime_one(int compute) {
    if (!open_backend()) { CHECK(0, "C1581 pipe-call open compute=%d", compute); return; }
    struct pipe_call_test_arg a; memset(&a, 0, sizeof a); a.compute = compute;
    pthread_t thread;
    nvn_vk_pipe_call_test_hold(1);
    if (pthread_create(&thread, NULL, pipe_call_test_main, &a) != 0) {
        nvn_vk_pipe_call_test_hold(0);
        nvn_gpu_err close_err = {0}; nvn_gpu_close(5000000000ull, &close_err);
        CHECK(0, "C1581 pipe-call thread create compute=%d", compute);
        return;
    }
    if (!nvn_vk_pipe_call_test_wait(2000000000ull)) {
        nvn_vk_pipe_call_test_hold(0); pthread_join(thread, NULL);
        nvn_gpu_err close_err = {0}; nvn_gpu_close(5000000000ull, &close_err);
        CHECK(0, "C1581 pipe-call enters admitted gate compute=%d", compute);
        return;
    }
    nvn_gpu_err close_err = {0};
    nvn_gpu_rc close_rc = nvn_gpu_close(0, &close_err);
    CHECK(close_rc.code == NVN_GPU_E_TIMEOUT, "C1581 close waits for active pipe call compute=%d rc=%d %s", compute, close_rc.code, close_err.why);
    nvn_vk_pipe_call_test_hold(0);
    pthread_join(thread, NULL);
    CHECK(a.result.code == NVN_GPU_E_ARG, "C1581 admitted pipe error return compute=%d rc=%d", compute, a.result.code);
    close_err = (nvn_gpu_err){0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &close_err)), "C1581 pipe-call close retry compute=%d: %s", compute, close_err.why);
}

static void pipe_call_lifetime_test(void) {
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "pipe-call-lifetime-test";
    pipe_call_lifetime_one(0);
    pipe_call_lifetime_one(1);
    printf("C1581 pipe-call lifetime failures=%d\n", g_fail);
}
#endif

static nvn_gpu_buf make_buf(uint64_t n, int mem, const void *init) {
    nvn_gpu_buf b = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_buf_create(n, mem, init, &b, &e);
    CHECK(OK(r), "buf_create(%llu) 失败：%d %s", (unsigned long long)n, r.code, e.why);
    return b;
}

static nvn_gpu_tex make_tex(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td;
    memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = (int)fmt;
    td.d.width = w;
    td.d.height = h;
    td.d.depth = 1;
    td.d.levels = 1;
    td.d.samples = 0;
    td.usage = usage;
    nvn_gpu_tex t = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "tex_create 失败：%d %s", r.code, e.why);
    return t;
}

static nvn_gpu_tex make_tex_target(uint32_t w, uint32_t h, uint32_t depth, uint32_t target, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td;
    memset(&td, 0, sizeof td);
    td.d.target = (int)target; td.d.format = (int)fmt; td.d.width = w; td.d.height = h;
    td.d.depth = (int)depth; td.d.levels = 1; td.d.samples = 0; td.usage = usage;
    nvn_gpu_tex t = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "tex_create target %u 失败：%d %s", target, r.code, e.why);
    return t;
}

static nvn_gpu_view make_view(nvn_gpu_tex t) {
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create(t, NULL, &v, &e);
    CHECK(OK(r), "view_create 失败：%d %s", r.code, e.why);
    return v;
}

static nvn_gpu_cmd begin_cmd(void) {
    nvn_gpu_cmd c = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_begin(NULL, &c, &e);
    CHECK(OK(r), "cmd_begin 失败：%d %s", r.code, e.why);
    return c;
}

#ifdef S2A_CMDPOOL_TEST
// 每段造新图，使提交必需前置转布局命令；同一池重复使用后不得积累缓冲。
static void c1581_cmdpool(void) {
    nvn_gpu_err e = {0};
    if (!open_backend()) { CHECK(0, "C1581 open"); return; }
    pthread_mutex_lock(&g_cmdpool_mu);
    memset(g_cmdpool, 0, sizeof g_cmdpool); g_cmdpool_peak = g_cmdpool_resets = g_cmdpool_overflow = 0; g_cmdpool_track = 1;
    pthread_mutex_unlock(&g_cmdpool_mu);
    for (uint64_t i = 1; i <= 96; i++) {
        nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
        nvn_gpu_cmd c = begin_cmd();
        if (!t || !c) { CHECK(0, "C1581 create %llu", (unsigned long long)i); break; }
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, i, on_done, NULL, &e);
        CHECK(OK(r), "C1581 commit %llu: %s", (unsigned long long)i, e.why);
        if (OK(r)) CHECK(OK(nvn_gpu_fence_wait(i, 5000000000ull, &e)), "C1581 wait %llu: %s", (unsigned long long)i, e.why);
        nvn_gpu_tex_release(t);
        if (!OK(r)) break;
        nvn_gpu_cmd abandoned = begin_cmd();
        if (abandoned) nvn_gpu_cmd_abort(abandoned);
    }
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1581 close: %s", e.why);
    pthread_mutex_lock(&g_cmdpool_mu);
    unsigned peak = g_cmdpool_peak, resets = g_cmdpool_resets, overflow = g_cmdpool_overflow; g_cmdpool_track = 0;
    pthread_mutex_unlock(&g_cmdpool_mu);
    printf("C1581 96 段：池内最高命令缓冲数=%u，带前置缓冲的池重置=%u，跟踪异常=%u\n", peak, resets, overflow);
    CHECK(peak == 2, "C1581 池内最高命令缓冲数期望 2 → %u", peak);
    CHECK(resets > 0, "C1581 必须复用带前置缓冲的池 → %u", resets);
    CHECK(overflow == 0, "C1581 跟踪异常期望 0 → %u", overflow);

    // 前置缓冲已分配但 begin 失败：真实失败回调与 close 仍须销毁整池。
    if (!open_backend()) { CHECK(0, "C1581 failure open"); return; }
    pthread_mutex_lock(&g_cmdpool_mu);
    memset(g_cmdpool, 0, sizeof g_cmdpool); g_cmdpool_peak = g_cmdpool_resets = g_cmdpool_overflow = 0; g_cmdpool_track = 1;
    pthread_mutex_unlock(&g_cmdpool_mu);
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_cmd c = begin_cmd();
    if (t && c) {
        atomic_store(&g_cmdpool_failed_begin, 0); atomic_store(&g_cmdpool_fail_begin, 1);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        atomic_store(&g_cmdpool_fail_begin, 0);
        CHECK(!OK(r) && atomic_load(&g_cmdpool_failed_begin) == 1, "C1581 前置 begin 注入应失败：rc=%d 命中=%d", r.code, atomic_load(&g_cmdpool_failed_begin));
        nvn_gpu_tex_release(t);
    } else { CHECK(0, "C1581 failure create"); if (c) nvn_gpu_cmd_abort(c); if (t) nvn_gpu_tex_release(t); }
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1581 failure close: %s", e.why);
    pthread_mutex_lock(&g_cmdpool_mu);
    unsigned live = 0; for (unsigned i = 0; i < 256; i++) live += g_cmdpool[i].live;
    peak = g_cmdpool_peak; overflow = g_cmdpool_overflow; g_cmdpool_track = 0;
    pthread_mutex_unlock(&g_cmdpool_mu);
    printf("C1581 失败段：最高=%u，close 后存活=%u，跟踪异常=%u\n", peak, live, overflow);
    CHECK(peak == 2 && live == 0 && overflow == 0, "C1581 失败段期望分配 2、close 后 0、跟踪异常 0 → %u/%u/%u", peak, live, overflow);
}

#endif

// C01：fill 是 32 位样式，不是逐字节 memset；范围外不动。
static void t01_fill32(void) {
    uint8_t zeros[64];
    memset(zeros, 0, sizeof zeros);
    nvn_gpu_buf b = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 16, 16, 0x11223344u);
    nvn_gpu_err e;
    atomic_store(&g_probe_order, 1);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 2000000000ull, &e);
    CHECK(OK(r), "wait 1：%d %s", r.code, e.why);
    uint8_t *p = buf_cpu(b);
    CHECK(p != NULL, "SHARED 缓冲没有 CPU 地址");
    static const uint8_t want[4] = {0x44, 0x33, 0x22, 0x11};
    for (int i = 0; i < 16; i++) CHECK(p[16 + i] == want[i % 4], "fill 字节 %d = %02x，期望 %02x", 16 + i, p[16 + i], want[i % 4]);
    for (int i = 0; i < 16; i++) CHECK(p[i] == 0, "fill 前越界改了字节 %d", i);
    for (int i = 32; i < 64; i++) CHECK(p[i] == 0, "fill 后越界改了字节 %d", i);
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(1);
}

// 写入 + 拷贝：dst 与 src 全等；CPU 真值不来自 GPU。
static void t02_write_copy(void) {
    uint8_t init[256], zero[256];
    for (int i = 0; i < 256; i++) init[i] = (uint8_t)i;
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(256, NVN_GPU_MEM_SHARED, init);
    nvn_gpu_buf b = make_buf(256, NVN_GPU_MEM_SHARED, zero);
    uint8_t w[16];
    for (int i = 0; i < 16; i++) w[i] = (uint8_t)(0x80 + i);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf_write(c, a, 32, w, sizeof w);
    nvn_gpu_copy_buf(c, a, 0, b, 0, 256);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(2, 2000000000ull, &e);
    CHECK(OK(r), "wait 2：%d %s", r.code, e.why);
    CHECK(memcmp(buf_cpu(a), buf_cpu(b), 256) == 0, "copy 后 dst 与 src 不同");
    CHECK(memcmp(buf_cpu(a) + 32, w, sizeof w) == 0, "buf_write 内容不对");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(2);
}

// C05：录制进未提交 cmd 后立刻 release，真正销毁必须等提交段 done 之后。
static void t03_keepalive_unsubmitted(void) {
    uint8_t pat[64], zero[64];
    memset(pat, 0xAB, sizeof pat);
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf src = make_buf(64, NVN_GPU_MEM_SHARED, pat);
    nvn_gpu_buf dst = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    int retired_before = atomic_load(&g_retired), done_base = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_copy_buf(c, src, 0, dst, 0, 64);
    nvn_gpu_buf_release(src, on_retire, NULL);
    sleep_ms(20);
    CHECK(atomic_load(&g_retired) == retired_before, "录制未提交的 src 被提前销毁");
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(3, 2000000000ull, &e);
    CHECK(OK(r), "wait 3：%d %s", r.code, e.why);
    for (int i = 0; i < 64; i++) CHECK(buf_cpu(dst)[i] == 0xAB, "保活期间内容被改，字节 %d", i);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "commit 完成后 src 没有退休");
    CHECK(atomic_load(&g_ndone_at_retire) >= done_base + 1, "retired 在本段 done 之前就调了");
    nvn_gpu_buf_release(dst, NULL, NULL);
    wait_done(3);
}

// C06：经 GPU 地址间接引用（buf_use）也保活；retired 在该段 done 之后才调。
static void t03b_buf_use(void) {
    uint8_t zero[32];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(32, NVN_GPU_MEM_SHARED, zero);
    int retired_before = atomic_load(&g_retired), done_before = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf_use(c, b);
    nvn_gpu_buf_release(b, on_retire, NULL);
    sleep_ms(20);
    CHECK(atomic_load(&g_retired) == retired_before, "buf_use 录制后仍提前销毁");
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 4, on_done, NULL, &e);
    CHECK(OK(r), "commit 4：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(4, 2000000000ull, &e);
    CHECK(OK(r), "wait 4：%d %s", r.code, e.why);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "buf_use 段完成后没有退休");
    CHECK(atomic_load(&g_retired) == retired_before + 1, "retired 调了不止一次");
    CHECK(atomic_load(&g_ndone_at_retire) >= done_before + 1, "retired 在本段 done 之前就调了");
    wait_done(4);
}

// C08：abort 不执行、不回调、不推进时间线，引用随即解除。
static void t04_abort(void) {
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf keep = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf rel = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    int done_before = atomic_load(&g_ndone), retired_before = atomic_load(&g_retired);
    uint64_t fence_before = nvn_gpu_fence_done();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, keep, 0, 64, 0xFFFFFFFFu);
    nvn_gpu_copy_buf(c, keep, 0, rel, 0, 64);
    nvn_gpu_buf_release(rel, on_retire, NULL);
    nvn_gpu_cmd_abort(c);
    sleep_ms(50);
    for (int i = 0; i < 64; i++) CHECK(buf_cpu(keep)[i] == 0, "abort 后仍执行了 fill（字节 %d）", i);
    CHECK(atomic_load(&g_ndone) == done_before, "abort 后仍调了 done");
    CHECK(nvn_gpu_fence_done() == fence_before, "abort 推进了时间线");
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "abort 后引用没有解除 / 没有退休");
    nvn_gpu_buf_release(keep, NULL, NULL);
}

// C02/C03：失败段终止后续；失败段与后续段都不执行、各回调一次 FAILED。
static void t05_failure_terminal(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf c = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_err e;

    // 三段都先录好再依次提交：失败终止后 begin 会返回 E_FAILED，C02 的「段 3」属于已录制的段。
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, a, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, c, 0, 16, 0x33333333u);

    nvn_gpu_rc r = nvn_gpu_cmd_commit(c1, 5, on_done, NULL, &e);
    CHECK(OK(r), "段 1 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(5, 2000000000ull, &e);
    CHECK(OK(r), "wait(5)：%d %s", r.code, e.why);

    nvn_gpu_vk_test_set_fault(1);
    r = nvn_gpu_cmd_commit(c2, 6, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_DEVICE, "段 2 注入失败应返回 E_DEVICE，实为 %d", r.code);

    r = nvn_gpu_cmd_commit(c3, 7, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "段 3 应返回 E_FAILED，实为 %d", r.code);

    CHECK(nvn_gpu_fence_done() == 5, "fence_done 应为 5，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_failed() == 6, "fence_failed 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    r = nvn_gpu_fence_wait(6, 2000000000ull, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "wait(6) 应 FAILED，实为 %d", r.code);
    r = nvn_gpu_fence_wait(7, 2000000000ull, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "wait(7) 应 FAILED，实为 %d", r.code);

    const struct done_rec *d2 = wait_signal(6, 0, 5000), *d3 = wait_signal(7, 0, 5000);
    CHECK(d2 && d2->status == NVN_GPU_DONE_FAILED, "段 2 回调不是 FAILED");
    CHECK(d3 && d3->status == NVN_GPU_DONE_FAILED, "段 3 回调不是 FAILED");
    for (int i = 0; i < 16; i++) {
        CHECK(((uint32_t *)buf_cpu(b))[i / 4] == 0, "失败段写了 B（偏移 %d）", i);
        CHECK(((uint32_t *)buf_cpu(c))[i / 4] == 0, "失败段之后仍写了 C（偏移 %d）", i);
    }
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_FAILED, "失败终态后 cmd_begin 应 E_FAILED");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    nvn_gpu_buf_release(c, NULL, NULL);
}

// C04：done 里写的值，等 fence 的线程返回后一定读得到。
struct waiter_arg { uint64_t v; int ok; };
static void *waiter_main(void *p) {
    struct waiter_arg *a = p;
    nvn_gpu_err e;
    a->ok = OK(nvn_gpu_fence_wait(a->v, 5000000000ull, &e));
    return NULL;
}

static void t06_publish_order(void) {
    g_event = 0;
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x00000009u);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    struct waiter_arg a = {1, 0};
    pthread_t t;
    pthread_create(&t, NULL, waiter_main, &a);
    pthread_join(t, NULL);
    CHECK(a.ok, "wait(1) 失败");
    CHECK(atomic_load(&g_order_code) == NVN_GPU_E_ORDER, "done 里 fence_wait 应 E_ORDER，实为 %d", atomic_load(&g_order_code));
    atomic_store(&g_probe_order, 0);
    CHECK(g_event == 9, "done 的写没有在 fence 返回前可见（读到 %d）", g_event);
    CHECK(atomic_load(&g_order_bad) == 0, "done 被调时已完成值已推进（发布顺序错）");
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(4);
}

// 等待语义：从没提交过的值 E_ARG；纯查询不产生副作用。
static void t07_wait_args(void) {
    nvn_gpu_err e;
    CHECK(nvn_gpu_fence_wait(1, 0, &e).code == NVN_GPU_OK, "wait(1, 0) 应 OK");
    CHECK(nvn_gpu_fence_wait(999, 0, &e).code == NVN_GPU_E_ARG, "wait(999) 应 E_ARG");
    CHECK(nvn_gpu_fence_wait(0, 0, &e).code == NVN_GPU_E_ARG, "wait(0) 应 E_ARG");
}

// 粘滞错误：整段不提交，按失败段处理（F = 该段 signal）。
static void t08_sticky(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    int base = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 6, 0xDEADBEEFu);   // 非 4 字节长度
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "粘滞段 commit 应返回 E_ARG，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    const struct done_rec *d5 = wait_signal(2, base, 5000);
    CHECK(d5 && d5->status == NVN_GPU_DONE_FAILED, "粘滞段回调不是 FAILED");
    for (int i = 0; i < 16; i++) CHECK(buf_cpu(b)[i] == 0, "粘滞段仍执行了");
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 没有引用时 release 立即退休。
static void t09_release_only(void) {
    int retired_before = atomic_load(&g_retired);
    nvn_gpu_buf b = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf_release(b, on_retire, NULL);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "无引用 release 没有退休");
}

// 清屏：CPU 手算像素；先清屏再读回，布局屏障也一起走通。
static void t11_clear_color(void) {
    nvn_gpu_tex t = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[8 * 8 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    CHECK(nvn_gpu_buf_create(0, NVN_GPU_MEM_SHARED, NULL, &(nvn_gpu_buf){0}, NULL).code != 0, "空缓冲应被拒");
    nvn_gpu_cmd c = begin_cmd();
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    nvn_gpu_clear_view(c, v, red, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 32, 256);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(2, 2000000000ull, &e);
    CHECK(OK(r), "wait 2：%d %s", r.code, e.why);
    for (int i = 0; i < 64; i++) {
        const uint8_t want[4] = {0xFF, 0x00, 0x00, 0xFF};
        CHECK(buf_cpu(b)[i * 4 + 0] == want[0] && buf_cpu(b)[i * 4 + 1] == want[1] &&
              buf_cpu(b)[i * 4 + 2] == want[2] && buf_cpu(b)[i * 4 + 3] == want[3],
              "清屏像素 %d = %02x%02x%02x%02x", i, buf_cpu(b)[i * 4], buf_cpu(b)[i * 4 + 1], buf_cpu(b)[i * 4 + 2], buf_cpu(b)[i * 4 + 3]);
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 上传 + 贴图到贴图复制：读回字节与 CPU 真值逐字节相同。
static void t12_upload_copy_tex(void) {
    uint8_t pat[4 * 4 * 4];
    for (int i = 0; i < (int)sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + 3);
    nvn_gpu_tex src = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex dst = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, src, 0, 0, NULL, pat, 16, 64);
    nvn_gpu_copy_tex(c, src, 0, 0, NULL, dst, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(3, 2000000000ull, &e);
    CHECK(OK(r), "wait 3：%d %s", r.code, e.why);
    CHECK(memcmp(buf_cpu(b), pat, sizeof pat) == 0, "上传 + 复制后的像素与 CPU 真值不同");
    nvn_gpu_tex_release(src);
    nvn_gpu_tex_release(dst);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 深度清屏：0.5 的 D32_SFLOAT 是 0x3F000000，直接对字节。
static void t13_clear_depth(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    const float half[4] = {0.5f, 0.0f, 0.0f, 0.0f};
    nvn_gpu_clear_view(c, v, half, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 4, on_done, NULL, &e);
    CHECK(OK(r), "commit 4：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(4, 2000000000ull, &e);
    CHECK(OK(r), "wait 4：%d %s", r.code, e.why);
    for (int i = 0; i < 16; i++) {
        uint32_t word;
        memcpy(&word, buf_cpu(b) + i * 4, 4);
        CHECK(word == 0x3F000000u, "深度像素 %d = 0x%08x，期望 0x3f000000", i, word);
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}


static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    void *p = malloc((size_t)sz);
    if (p && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); p = NULL; }
    fclose(f);
    if (p) *n = (size_t)sz;
    return p;
}

// 原创纹理三角形：全屏三角形 + 2x2 纹理 + nearest 采样；逐像素用 CPU 手算真值。
// 通道映射用例的旋钮（缺省全 0 = 不设）：存储自带映射、视图映射（NVN 取值 0 零 / 1 一 / 2 R / 3 G / 4 B / 5 A）
static uint16_t g_tri_swz; static int g_tri_vsw_on; static uint8_t g_tri_vsw[4];
static uint32_t g_tri_view_fmt;   // 非 0：建一个设了格式的视图（视图格式覆盖存储格式），用来验「按视图的有效格式判 alpha」
// 没有 alpha 的 NVN 格式（硬件把 a 供给常数 1）：RGBX / BGRX / RGB5 / BGR5 / RGB_DXT1 及其变体
static int tri_alpha_reads_one(uint32_t f) {
    return f == NVN_FORMAT_RGBX8 || f == NVN_FORMAT_RGBX8_SRGB || f == NVN_FORMAT_BGRX8 || f == NVN_FORMAT_BGRX8_SRGB ||
           f == NVN_FORMAT_RGB5 || f == NVN_FORMAT_BGR5 || f == NVN_FORMAT_RGB_DXT1 || f == NVN_FORMAT_RGB_DXT1_SRGB ||
           (f >= NVN_FORMAT_RGBX8SN && f <= NVN_FORMAT_RGBX8I) || (f >= NVN_FORMAT_RGBX16F && f <= NVN_FORMAT_RGBX16I) ||
           (f >= NVN_FORMAT_RGBX32F && f <= NVN_FORMAT_RGBX32I);
}
static const uint8_t *g_tri_expect;   // 非空：整张图期望都是这 4 个字节（压缩格式用，纹素不是按像素摆的）
static const uint8_t *g_tri_px;       // 非空：2x2 各纹素的期望 RGBA8（16 字节，按纹素顺序；打包格式用，存储字节不是 RGBA8）
static int g_tri_tol;
static uint64_t g_tri_signal = 5;
static void tri_impl(const char *shader_dir, uint32_t tex_fmt, const uint8_t *texels, int tbpp, int trow, int want_a) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", shader_dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri.frag.spv", shader_dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fsz, "读不到 SPIR-V（%s）", shader_dir);
    if (!vs || !fs) { free(vs); free(fs); return; }

    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fs_layout;
    memset(&fs_layout, 0, sizeof fs_layout);
    fs_layout.ent = ents;
    fs_layout.nent = 2;
    nvn_gpu_layout vs_layout;
    memset(&vs_layout, 0, sizeof vs_layout);

    nvn_gpu_shader vs_sh = NULL, fs_sh = NULL;
    nvn_gpu_err e;
    nvn_gpu_shader_src ssrc;
    memset(&ssrc, 0, sizeof ssrc);
    ssrc.stage = NVN_GPU_SH_VS; ssrc.kind = NVN_GPU_BLOB_SPIRV; ssrc.entry = "main";
    ssrc.blob = vs; ssrc.bytes = vsz; ssrc.layout = &vs_layout;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &vs_sh, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    ssrc.stage = NVN_GPU_SH_FS; ssrc.blob = fs; ssrc.bytes = fsz; ssrc.layout = &fs_layout;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &fs_sh, &e)), "FS 装载：%d %s", e.rc.code, e.why);

    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs_sh;
    pd.st[NVN_GPU_SH_FS].sh = fs_sh;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;   // NVN_DRAW_PRIMITIVE_TRIANGLES
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "pipe_get：%d %s", r.code, e.why);
    if (!OK(r)) { nvn_gpu_shader_release(vs_sh); nvn_gpu_shader_release(fs_sh); free(vs); free(fs); return; }

    const float verts[3][4] = {{-1.0f, -1.0f, 0.0f, 0.0f},
                               { 3.0f, -1.0f, 2.0f, 0.0f},
                               {-1.0f,  3.0f, 0.0f, 2.0f}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex tex = NULL;
    {
        nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = (int)tex_fmt; td.d.width = 2; td.d.height = 2; td.d.depth = 1; td.d.levels = 1;
        td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | (g_tri_view_fmt ? NVN_GPU_TEX_VIEW_FMT : 0); td.swz = g_tri_swz;
        r = nvn_gpu_tex_create(&td, &tex, &e);
        CHECK(OK(r), "tex_create（swz=%#x）：%d %s", g_tri_swz, r.code, e.why);
    }
    nvn_gpu_view tview = NULL;
    if (g_tri_vsw_on || g_tri_view_fmt) {
        view_t vw; memset(&vw, 0, sizeof vw);
        if (g_tri_vsw_on) { vw.set_swizzle = 1; memcpy(vw.swz, g_tri_vsw, 4); }
        if (g_tri_view_fmt) { vw.set_format = 1; vw.format = (int)g_tri_view_fmt; }
        r = nvn_gpu_view_create(tex, &vw, &tview, &e);
        CHECK(OK(r), "view_create（带通道映射 / 视图格式）：%d %s", r.code, e.why);
    } else tview = make_view(tex);
    samp_par sp;
    memset(&sp, 0, sizeof sp);   // nearest / clamp
    nvn_gpu_samp samp = NULL;
    r = nvn_gpu_samp_create(&sp, &samp, &e);
    CHECK(OK(r), "samp_create：%d %s", r.code, e.why);

    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view oview = make_view(out);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);

    nvn_gpu_cmd c = begin_cmd();
    // r=NULL：整级上传，展开时按该级宽高取区域（C30 覆盖这条路径）
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, texels, (uint64_t)trow, g_tri_expect ? (uint64_t)trow : (uint64_t)trow * 2);   // 压缩：2x2 只有一排块

    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = 0.0f; pass.color[0].clear.f[1] = 0.0f;
    pass.color[0].clear.f[2] = 0.0f; pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[0] = 0; dyn.sc[1] = 0; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2];
    memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = tview;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    // 保活：录制完成后、提交前就 release 视图 / 贴图 / 采样器，段完成前不能真销毁。
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_samp_release(samp);
    r = nvn_gpu_cmd_commit(c, g_tri_signal, on_done, NULL, &e);
    CHECK(OK(r), "commit 5：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(g_tri_signal, 5000000000ull, &e);
    CHECK(OK(r), "wait 5：%d %s", r.code, e.why);

    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float u = (x + 0.5f) / 64.0f, v = (y + 0.5f) / 64.0f;
            int tx = (int)(u * 2.0f); if (tx > 1) tx = 1;
            int ty = (int)(v * 2.0f); if (ty > 1) ty = 1;
            if (g_tri_origin_ul) ty = 1 - ty;   // 原点在左上：同一个裁剪空间 y 落到相反的行
            const uint8_t *src = g_tri_expect ? g_tri_expect : g_tri_px ? g_tri_px + (ty * 2 + tx) * 4 : texels + (ty * 2 + tx) * tbpp;
            // 期望：先取纹素（没有 alpha 的格式按 want_a），再按生效的通道映射手算（视图设了用视图的，否则用存储的）
            // 没有 alpha 的 NVN 格式（RGBX / BGRX / RGB5 / BGR5 / RGB_DXT1）：硬件把 a 读作 1，与存储的第 4 个字节无关；
            // 判定按「视图的有效格式」——视图换格式时以视图格式为准（与硬件一致）
            int noalpha = tri_alpha_reads_one(g_tri_view_fmt ? g_tri_view_fmt : tex_fmt);
            uint8_t px[4] = {src[0], src[1], src[2], (uint8_t)((tbpp == 4 && !noalpha) || g_tri_px ? src[3] : want_a)}, mapped[4], sw[4] = {2, 3, 4, 5};
            if (g_tri_vsw_on) memcpy(sw, g_tri_vsw, 4);
            else if (g_tri_swz) for (int ch = 0; ch < 4; ch++) sw[ch] = (uint8_t)(g_tri_swz >> (3 * ch) & 7);
            for (int ch = 0; ch < 4; ch++) mapped[ch] = sw[ch] == 0 ? 0 : sw[ch] == 1 ? 255 : px[sw[ch] - 2];
            const uint8_t *want = mapped;
            const uint8_t *got = buf_cpu(rb) + (y * 64 + x) * 4;
            if (abs(got[0] - want[0]) > g_tri_tol || abs(got[1] - want[1]) > g_tri_tol || abs(got[2] - want[2]) > g_tri_tol || abs(got[3] - (g_tri_vsw_on || g_tri_swz || g_tri_px ? want[3] : want_a)) > g_tri_tol) {
                g_fail++;
                printf("FAIL %s:%d: 三角像素 (%d,%d) = %02x%02x%02x%02x，期望 %02x%02x%02x%02x\n",
                       __FILE__, __LINE__, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                y = 64;
                break;
            }
        }
    }
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs_sh);
    nvn_gpu_shader_release(fs_sh);
    free(vs);
    free(fs);
}


static void t14_textured_triangle(const char *dir) {
    static const uint8_t texels[16] = {255, 0, 0, 255,   0, 255, 0, 255,
                                       0, 0, 255, 255,   255, 255, 255, 255};
    tri_impl(dir, NVN_FORMAT_RGBA8, texels, 4, 8, 255);
}

// C70：通道映射。存储自带映射 B G R 一（红蓝对调、alpha 恒 1）；视图映射 零 R A G 取代存储的（不是叠加）。
// 纹素的四个通道互不相同，哪一路映射错了都读得出来。期望在 tri_impl 里按 NVN 取值手算。
// C200：没有 alpha 的 NVN 格式，采样到的 a 必须是 1（RGBX8 的 X 字节摆 0 也一样），有 alpha 的格式照读。
// NVN/Maxwell 的输出合成都按 alpha 参与混合，读错这一位就是深色场景光照逐层变暗的直接原因。
// 每个变体自己一个会话：tri_impl 的提交序号固定（见 C70 / C195 的同一处理）。
static void c200_alpha_reads_one(const char *dir, int k) {
    static const uint8_t t4[16] = {255, 0, 0, 0,   0, 255, 0, 0,   0, 0, 255, 0,   255, 255, 255, 0};   // 四个纹素的第 4 字节全 0
    switch (k) {
    case 0: tri_impl(dir, NVN_FORMAT_RGBX8, t4, 4, 8, 255); break;                        // 宿主 RGBA8：X 字节不能当 alpha
    case 1: tri_impl(dir, NVN_FORMAT_RGBA8, t4, 4, 8, 0); break;                         // 负控：有 alpha 的格式照读 0
    case 2: g_tri_view_fmt = NVN_FORMAT_RGBA8; tri_impl(dir, NVN_FORMAT_RGBX8, t4, 4, 8, 0); g_tri_view_fmt = 0; break;   // 视图有 alpha：按视图格式照读存储的第 4 字节
    default: g_tri_view_fmt = NVN_FORMAT_RGBX8; tri_impl(dir, NVN_FORMAT_RGBA8, t4, 4, 8, 255); g_tri_view_fmt = 0; break; // 视图没有 alpha：a 读作 1
    }
}

// C975（#975）：16 位打包格式按 NVN 的位序采样、写入。NVN 从最低位起给分量命名：
//   RGBA4 = r 0–3、g 4–7、b 8–11、a 12–15；RGB5A1 = r 0–4、g 5–9、b 10–14、a 15（RGB5 的 bit15 是填充位，a 读作 1）；
//   RGB565 = r 0–4、g 5–10、b 11–15；BGR565 = b 0–4、g 5–10、r 11–15；BGR5A1 = b 0–4、g 5–9、r 10–14、a 15（BGR5 同 RGB5）；
//   A1BGR5 = a 0、b 1–5、g 6–10、r 11–15。565 没有 alpha，读作 1。
//   依据：iOS 11-03 真机实测（graphics/nvn/gfx/tex/nvn_fmt_mtl.c:54–60）；Ryujinx Vulkan 同口径（FormatTable.cs：R4G4B4A4Unorm → A4B4G4R4UnormPack16；
//   TextureView.cs：R5G6B5Unorm 配 R5G6B5UnormPack16 时把 r / b 对调，即 r 在低位）。
// 采样：纹素由各分量的字段值按上面的位移拼成 16 位、小端存；期望 = round(字段值 × 255 / 字段最大值)，字段值只取
//   转 8 位离 .5 远的（4 位全是 17 的倍数；5 位 0/6/9/17/31；6 位 0/21/63），完全按规则手算，不经被测的格式映射。
//   四个纹素各分量互不对称：哪一处位序反了都读得出来。
// 写入：同一组字段值（第 4 个纹素）当清屏色，① 当渲染目标 LOAD_CLEAR、② clear_view（vkCmdClearColorImage），读回原始 16 位，
//   比对手拼的位型（RGB5 / BGR5 的填充位不比）。本设备走退回格式（通道补偿）的格式不能当渲染目标：核 tex_create 与开通道都具名拒绝。
// forced = 1：开完后端把能退的格式全切到退回格式，在 lavapipe 上也把通道补偿这一路走一遍（结束恢复设备的路由）。
static const struct c975_fmt { uint32_t fmt; const char *name; int lo[4], bits[4]; int fld[4][4]; } c975_cs[8] = {
    {NVN_FORMAT_RGBA4,  "RGBA4",  {0, 4, 8, 12},  {4, 4, 4, 4}, {{15, 0, 0, 5}, {0, 15, 0, 10}, {0, 0, 15, 15}, {3, 6, 9, 12}}},
    {NVN_FORMAT_RGB5,   "RGB5",   {0, 5, 10, 0},  {5, 5, 5, 0}, {{31, 0, 0, 0}, {0, 31, 0, 0}, {0, 0, 31, 0}, {9, 17, 6, 0}}},
    {NVN_FORMAT_RGB5A1, "RGB5A1", {0, 5, 10, 15}, {5, 5, 5, 1}, {{31, 0, 0, 1}, {0, 31, 0, 0}, {0, 0, 31, 1}, {9, 17, 6, 1}}},
    {NVN_FORMAT_RGB565, "RGB565", {0, 5, 11, 0},  {5, 6, 5, 0}, {{31, 0, 0, 0}, {0, 63, 0, 0}, {0, 0, 31, 0}, {9, 21, 6, 0}}},
    {NVN_FORMAT_BGR565, "BGR565", {11, 5, 0, 0},  {5, 6, 5, 0}, {{31, 0, 0, 0}, {0, 63, 0, 0}, {0, 0, 31, 0}, {9, 21, 6, 0}}},
    {NVN_FORMAT_BGR5,   "BGR5",   {10, 5, 0, 0},  {5, 5, 5, 0}, {{31, 0, 0, 0}, {0, 31, 0, 0}, {0, 0, 31, 0}, {9, 17, 6, 0}}},
    {NVN_FORMAT_BGR5A1, "BGR5A1", {10, 5, 0, 15}, {5, 5, 5, 1}, {{31, 0, 0, 1}, {0, 31, 0, 0}, {0, 0, 31, 1}, {9, 17, 6, 1}}},
    {NVN_FORMAT_A1BGR5, "A1BGR5", {11, 6, 1, 0},  {5, 5, 5, 1}, {{31, 0, 0, 1}, {0, 31, 0, 0}, {0, 0, 31, 1}, {9, 17, 6, 1}}},
};
static uint16_t c975_word(const struct c975_fmt *f, const int fld[4], uint16_t *mask) {
    uint32_t v = 0, m = 0;
    for (int ch = 0; ch < 4; ch++) {
        if (!f->bits[ch]) continue;
        uint32_t max = (1u << f->bits[ch]) - 1u;
        v |= (uint32_t)fld[ch] << f->lo[ch];
        m |= max << f->lo[ch];
    }
    if (mask) *mask = (uint16_t)m;
    return (uint16_t)v;
}
// 清成第 4 个纹素的颜色，读回 2x2 原始字节；sig 是这次提交的序号。返回读回的第一个纹素（失败返回 -1）。
static int c975_clear_raw(nvn_gpu_tex t, nvn_gpu_view v, int as_rt, const float col[4], uint64_t sig, char *why, size_t why_size) {
    nvn_gpu_buf rb = make_buf(8, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    if (as_rt) {
        nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
        pass.ncolor = 1; pass.color[0].view = v; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
        for (int i = 0; i < 4; i++) pass.color[0].clear.f[i] = col[i];
        pass.w = 2; pass.h = 2; pass.layers = 1;
        nvn_gpu_pass_begin(c, &pass);
        nvn_gpu_pass_end(c);
    } else {
        nvn_gpu_clear_view(c, v, col, NULL, 0);
    }
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 4, 8);
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    int got = -1;
    if (OK(r)) {
        r = nvn_gpu_fence_wait(sig, 5000000000ull, &e);
        CHECK(OK(r), "C975 wait %llu：%d %s", (unsigned long long)sig, r.code, e.why);
        uint16_t w; memcpy(&w, buf_cpu(rb), 2); got = w;
        for (int i = 1; i < 4; i++) { uint16_t x; memcpy(&x, buf_cpu(rb) + i * 2, 2); if (x != w) got = -2; }
    }
    snprintf(why, why_size, "%d %s", r.code, e.why);
    nvn_gpu_buf_release(rb, NULL, NULL);
    return got;
}
static void c975_fmt16_order(const char *dir, int k, int forced) {
    const struct c975_fmt *f = &c975_cs[k];
    uint32_t dev_mask = nvn_vk_format_p16_fallback();
    if (forced) nvn_vk_format_set_p16_fallback(0xffu);
    uint8_t map[4];
    int remap = nvn_vk_format_chanmap(f->fmt, map);
    uint32_t caps = nvn_gpu_format_caps(f->fmt);
    printf("C975 %s：%s 路由 vk=%d 通道补偿=%d（%u%u%u%u）caps=0x%x\n", f->name, forced ? "强制退回" : "设备", (int)nvn_vk_format(f->fmt),
           remap, map[0], map[1], map[2], map[3], caps);
    if (forced && !remap) { nvn_vk_format_set_p16_fallback(dev_mask); return; }   // 没有退回格式的项：设备路由那一轮已核
    CHECK(caps & NVN_GPU_FMT_SAMPLE, "C975 %s：%s 路由不能采样（caps=0x%x）", f->name, forced ? "退回" : "设备", caps);
    if (!(caps & NVN_GPU_FMT_SAMPLE)) { nvn_vk_format_set_p16_fallback(dev_mask); return; }
    uint8_t texels[8], want[16];
    for (int t = 0; t < 4; t++) {
        uint16_t v = c975_word(f, f->fld[t], NULL);
        for (int ch = 0; ch < 4; ch++) {
            int max = f->bits[ch] ? (1 << f->bits[ch]) - 1 : 0;
            want[t * 4 + ch] = (uint8_t)(max ? (f->fld[t][ch] * 255 + max / 2) / max : ch == 3 ? 255 : 0);
        }
        texels[t * 2] = (uint8_t)(v & 0xff); texels[t * 2 + 1] = (uint8_t)(v >> 8);
    }
    printf("C975 执行：%s 采样（纹素 %02x%02x %02x%02x %02x%02x %02x%02x，小端）\n", f->name,
           texels[1], texels[0], texels[3], texels[2], texels[5], texels[4], texels[7], texels[6]);
    g_tri_px = want;
    tri_impl(dir, f->fmt, texels, 2, 4, 255);
    g_tri_px = NULL;

    float col[4];
    for (int ch = 0; ch < 4; ch++) col[ch] = f->bits[ch] ? (float)f->fld[3][ch] / (float)((1 << f->bits[ch]) - 1) : 1.0f;
    uint16_t mask, wword = c975_word(f, f->fld[3], &mask);
    char why[300];
    // ② clear_view：两条路由都得对（退回格式时清屏值按通道补偿换位）
    {
        nvn_gpu_tex t = make_tex(2, 2, f->fmt, NVN_GPU_TEX_SAMPLED);
        nvn_gpu_view v = make_view(t);
        int got = c975_clear_raw(t, v, 0, col, 6, why, sizeof why);
        CHECK(got >= 0 && ((uint16_t)got & mask) == wword, "C975 %s clear_view：读回 %04x（掩码 %04x），期望 %04x（%s）",
              f->name, got < 0 ? 0xffffu : (unsigned)got, mask, wword, why);
        if (got >= 0 && ((uint16_t)got & mask) == wword) printf("C975 %s clear_view：读回 %04x = 期望 %04x\n", f->name, (unsigned)got & mask, wword);
        nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    }
    // ① 渲染目标
    if (remap) {
        CHECK(!(caps & (NVN_GPU_FMT_RENDER | NVN_GPU_FMT_STORAGE | NVN_GPU_FMT_VERTEX)), "C975 %s：退回格式不该报渲染 / 存储 / 顶点能力（caps=0x%x）", f->name, caps);
        nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = (int)f->fmt; td.d.width = 2; td.d.height = 2; td.d.depth = 1; td.d.levels = 1;
        td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER;
        nvn_gpu_tex bad = NULL; nvn_gpu_err e; memset(&e, 0, sizeof e);
        nvn_gpu_rc r = nvn_gpu_tex_create(&td, &bad, &e);
        CHECK(!OK(r) && !bad && strstr(e.why, "不能当渲染目标"), "C975 %s：退回格式建渲染目标应具名拒绝：%d %s", f->name, r.code, e.why);
        if (bad) nvn_gpu_tex_release(bad);
        nvn_gpu_tex t = make_tex(2, 2, f->fmt, NVN_GPU_TEX_SAMPLED);
        nvn_gpu_view v = make_view(t);
        int got = c975_clear_raw(t, v, 1, col, 7, why, sizeof why);
        CHECK(got == -1 && strstr(why, "不能当渲染目标"), "C975 %s：退回格式当颜色附件应具名拒绝：读回 %d，%s", f->name, got, why);
        printf("C975 %s 渲染目标：退回格式具名拒绝（%s）\n", f->name, why);
        nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    } else if (caps & NVN_GPU_FMT_RENDER) {
        nvn_gpu_tex t = make_tex(2, 2, f->fmt, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER);
        nvn_gpu_view v = make_view(t);
        int got = c975_clear_raw(t, v, 1, col, 7, why, sizeof why);
        CHECK(got >= 0 && ((uint16_t)got & mask) == wword, "C975 %s 渲染目标清屏：读回 %04x（掩码 %04x），期望 %04x（%s）",
              f->name, got < 0 ? 0xffffu : (unsigned)got, mask, wword, why);
        if (got >= 0 && ((uint16_t)got & mask) == wword) printf("C975 %s 渲染目标清屏：读回 %04x = 期望 %04x\n", f->name, (unsigned)got & mask, wword);
        nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    } else {
        printf("C975 %s：本设备精确格式没有颜色附件能力（caps=0x%x），渲染目标一项不适用\n", f->name, caps);
    }
    nvn_vk_format_set_p16_fallback(dev_mask);
}

static void c70_swizzle(const char *dir, int view_overrides) {
    static const uint8_t texels[16] = {10, 60, 110, 200,   20, 70, 120, 210,
                                       30, 80, 130, 220,   40, 90, 140, 230};
    g_tri_swz = 4 | 3 << 3 | 2 << 6 | 1 << 9;
    if (view_overrides) { g_tri_vsw_on = 1; g_tri_vsw[0] = 0; g_tri_vsw[1] = 2; g_tri_vsw[2] = 5; g_tri_vsw[3] = 3; }
    tri_impl(dir, NVN_FORMAT_RGBA8, texels, 4, 8, 255);
    g_tri_swz = 0; g_tri_vsw_on = 0;
}

// C71：上屏。接面 → 取图 → 把刚提交（还没等完成）的贴图贴上去呈现 → 取了不呈现再取 → 越界矩形 → 重建换代 → 撤面。
// 无头表面读不回屏上像素：本用例核的是所有权与换代语义、每个 ticket 恰好回调一次，以及校验层 0 条（布局、信号量、交换链用法）。
static int g_shown_n; static uint64_t g_shown_ticket; static uint32_t g_shown_status;
static void on_shown(void *user, const nvn_gpu_shown *s) { (void)user; g_shown_n++; g_shown_ticket = s->ticket; g_shown_status = s->status; }
static void c71_present(void) {
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_surface sf = NULL, sf2 = NULL;
    nvn_gpu_rc r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(OK(r) && sf, "C71 attach：%d %s", r.code, e.why);
    if (!sf) return;
    CHECK(nvn_gpu_surface_gen(sf) == 1, "C71 首个 generation 应为 1，得 %llu", (unsigned long long)nvn_gpu_surface_gen(sf));
    r = nvn_gpu_surface_attach((void *)1, &sf2, &e);
    CHECK(r.code == NVN_GPU_E_ORDER && !sf2, "C71 已接着一个面时再 attach 应 E_ORDER，得 %d", r.code);

    nvn_gpu_tex tex = make_tex(64, 36, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view view = make_view(tex);
    static uint8_t px[64 * 36 * 4]; memset(px, 0x80, sizeof px);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, px, 64 * 4, sizeof px);
    r = nvn_gpu_cmd_commit(c, 7, on_done, NULL, &e);
    CHECK(OK(r), "C71 commit：%d %s", r.code, e.why);

    nvn_gpu_drawable dr; memset(&dr, 0, sizeof dr);
    r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
    CHECK(OK(r) && dr.w == 320 && dr.h == 180 && dr.gen == 1, "C71 acquire：%d %s（%ux%u gen %llu）", r.code, e.why, dr.w, dr.h, (unsigned long long)dr.gen);
    nvn_gpu_present_desc pd; memset(&pd, 0, sizeof pd); pd.src = view; pd.filter = 1;
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &dr, &pd, 42, on_shown, NULL, &e);   // 上传那一段还没等完成：呈现段排在它后面
    CHECK(OK(r), "C71 present：%d %s", r.code, e.why);
    CHECK(g_shown_n == 1 && g_shown_ticket == 42 && g_shown_status == NVN_GPU_SHOWN_UNKNOWN, "C71 presented 应恰好一次、UNKNOWN（没有上屏时刻），得 n=%d status=%u", g_shown_n, g_shown_status);
    CHECK(OK(nvn_gpu_fence_wait(7, 5000000000ull, &e)), "C71 wait 7：%d %s", e.rc.code, e.why);
    if (g_shown_timing_test == 2) {
        struct timespec pause = {5, 100000000};
        while (nanosleep(&pause, &pause) && errno == EINTR) {}
    }

    // 取了不呈现：交回后下一次取到的是同一张
    nvn_gpu_drawable a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &a, &e)), "C71 acquire 2：%d %s", e.rc.code, e.why);
    nvn_gpu_drawable_release(sf, &a);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &b, &e)) && b.token == a.token, "C71 交回的 drawable 应作为下次取图的结果");
    // 目标矩形越界：拒绝、按丢帧回调；这张图仍留给下一次
    pd.dst[0] = 300; pd.dst[1] = 0; pd.dst[2] = 100; pd.dst[3] = 50;
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &b, &pd, 43, on_shown, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG && g_shown_n == 1 && g_shown_status == NVN_GPU_SHOWN_DROPPED, "C71 越界矩形应 E_ARG + DROPPED，得 %d n=%d status=%u", r.code, g_shown_n, g_shown_status);
    nvn_gpu_drawable d3; memset(&d3, 0, sizeof d3);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &d3, &e)) && d3.token == b.token, "C71 被拒的那张应留给下一次取图");
    memset(pd.dst, 0, sizeof pd.dst); pd.dst[0] = 40; pd.dst[1] = 0; pd.dst[2] = 240; pd.dst[3] = 180;   // 两侧留黑边
    pd.crop[0] = 0; pd.crop[1] = 0; pd.crop[2] = 32; pd.crop[3] = 18;
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &d3, &pd, 44, on_shown, NULL, &e);
    CHECK(OK(r) && g_shown_n == 1 && g_shown_ticket == 44, "C71 present（裁剪 + 黑边）：%d %s", r.code, e.why);

    // 重建换代：旧 generation 的 drawable 作废
    nvn_gpu_drawable old; memset(&old, 0, sizeof old);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &old, &e)), "C71 acquire 4：%d %s", e.rc.code, e.why);
    r = nvn_gpu_surface_rebuild(sf, &e);
    CHECK(OK(r) && nvn_gpu_surface_gen(sf) == 2, "C71 rebuild：%d %s gen=%llu", r.code, e.why, (unsigned long long)nvn_gpu_surface_gen(sf));
    memset(pd.dst, 0, sizeof pd.dst); memset(pd.crop, 0, sizeof pd.crop);
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &old, &pd, 45, on_shown, NULL, &e);
    CHECK(r.code == NVN_GPU_E_OUTDATED && g_shown_n == 1 && g_shown_status == NVN_GPU_SHOWN_DROPPED, "C71 旧 generation 的 drawable 应 E_OUTDATED + DROPPED，得 %d", r.code);
    nvn_gpu_drawable nd; memset(&nd, 0, sizeof nd);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &nd, &e)) && nd.gen == 2, "C71 重建后 acquire：%d %s", e.rc.code, e.why);
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &nd, &pd, 46, on_shown, NULL, &e);
    CHECK(OK(r) && g_shown_n == 1, "C71 重建后 present：%d %s", r.code, e.why);

    // C199：呈现回「次优」但表面的方向和尺寸没变（安卓横屏的常态）→ 不重建，下一帧照常取图；表面真转了 → 30 次以内标过期
    { uint64_t gen0 = nvn_gpu_surface_gen(sf); int bad = 0;
      g_wsi_subopt = 1;
      for (int i = 0; i < 40; i++) {
          nvn_gpu_drawable q; memset(&q, 0, sizeof q);
          nvn_gpu_rc ra = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &q, &e);
          if (!OK(ra)) { bad++; break; }
          if (!OK(nvn_gpu_present(sf, &q, &pd, 100 + (uint64_t)i, on_shown, NULL, &e))) bad++;
      }
      CHECK(!bad && nvn_gpu_surface_gen(sf) == gen0, "C199 表面没变、连着 40 帧「次优」：期望每帧取图和呈现都成、不换代 → 失败 %d 次、代 %llu（原 %llu）：%s", bad, (unsigned long long)nvn_gpu_surface_gen(sf), (unsigned long long)gen0, e.why);
      g_wsi_rotated = 1;
      int outdated_at = -1;
      for (int i = 0; i < 31 && outdated_at < 0; i++) {
          nvn_gpu_drawable q; memset(&q, 0, sizeof q);
          nvn_gpu_rc ra = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &q, &e);
          if (ra.code == NVN_GPU_E_OUTDATED) { outdated_at = i; break; }
          if (!OK(ra)) break;
          nvn_gpu_present(sf, &q, &pd, 200 + (uint64_t)i, on_shown, NULL, &e);
      }
      CHECK(outdated_at >= 0, "C199 表面转了 90 度：期望 31 帧以内取图报过期（要重建）→ 没报");
      g_wsi_subopt = g_wsi_rotated = 0;
      r = nvn_gpu_surface_rebuild(sf, &e);
      CHECK(OK(r), "C199 rebuild：%d %s", r.code, e.why);
    }

    // C71w：取图信号量轮着用 nimages + 1 把；worker 落后（这里让它在一个游戏段上先睡 400 ms，呈现段都排在它后面没人等栅栏）时，
    //   连着呈现 20 帧（比任何一档的 nimages + 1 都多）会绕回第一把信号量。取图必须先等到上次等这把信号量的呈现段被确认完成，
    //   否则把还挂着等待的信号量交给驱动 = VUID-vkAcquireNextImageKHR-semaphore-01779（门禁上 CPU 忙时偶发，这里稳定复现）。
    //   判据：每帧取图和呈现都成（等待有界、不超 1 s 的取图时限），且 run.sh 的 validation 0 条。
    { nvn_gpu_cmd cw = begin_cmd();
      nvn_gpu_tex_upload(cw, tex, 0, 0, NULL, px, 64 * 4, sizeof px);
      nvn_gpu_vk_test_delay_segment(8, 400);
      r = nvn_gpu_cmd_commit(cw, 8, on_done, NULL, &e);
      CHECK(OK(r), "C71w commit：%d %s", r.code, e.why);
      int bad = 0;
      memset(pd.dst, 0, sizeof pd.dst); memset(pd.crop, 0, sizeof pd.crop);
      for (int i = 0; i < 20 && !bad; i++) {
          nvn_gpu_drawable q; memset(&q, 0, sizeof q);
          nvn_gpu_rc ra = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &q, &e);
          if (!OK(ra)) { bad = 1 + i; break; }
          if (!OK(nvn_gpu_present(sf, &q, &pd, 300 + (uint64_t)i, on_shown, NULL, &e))) bad = 1 + i;
      }
      CHECK(!bad, "C71w worker 落后时连呈现 20 帧：第 %d 帧取图 / 呈现失败：%s", bad, e.why);
      CHECK(OK(nvn_gpu_fence_wait(8, 5000000000ull, &e)), "C71w wait 8：%d %s", e.rc.code, e.why);
      nvn_gpu_vk_test_delay_segment(0, 0);
    }

    nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    r = nvn_gpu_surface_detach(sf, 5000000000ull, &e);
    CHECK(OK(r), "C71 detach：%d %s", r.code, e.why);
    r = nvn_gpu_surface_attach((void *)1, &sf, &e);          // 撤掉以后可以再接
    CHECK(OK(r) && nvn_gpu_surface_gen(sf) == 1, "C71 再 attach：%d %s", r.code, e.why);
    // 不 detach 直接 close：close 负责收掉还接着的面（由 main 里紧跟的 close 验）
}

// C697: the fixed-eight implementation must fail before we submit an out-of-range image.
static void c697_swapchain_images(void) {
    nvn_gpu_err e = {0}; nvn_gpu_surface sf = NULL;
    g_w697_count = 14; g_w697_queries = g_w697_fetches = 0;
    g_w697_created = g_w697_destroyed = g_w697_sc_destroyed = 0;
    g_w697_incomplete = 1; // valid count query followed by incomplete fill: must re-enumerate.
    nvn_gpu_rc r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(OK(r) && sf, "C697 attach 14 images/retry: %d %s", r.code, e.why);
    CHECK(g_w697_queries >= 2 && g_w697_fetches >= 2, "C697 complete enumeration: queries=%u fetches=%u", g_w697_queries, g_w697_fetches);
    CHECK(g_w697_created == 29, "C697 14 rendered + 15 acquire semaphores: got %u", g_w697_created);
    // Do not let the negative control feed an out-of-bounds handle to the real driver.
    if (!sf || g_w697_created != 29 || g_w697_queries < 2 || g_w697_fetches < 2) goto finish;
    nvn_gpu_tex tex = make_tex(64, 36, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex);
    nvn_gpu_present_desc pd = {0}; pd.src = view;
    for (int k = 0; k < 16; k++) {
        nvn_gpu_drawable dr = {0};
        r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
        CHECK(OK(r), "C697 acquire virtual index >=8 frame %d: %d %s", k, r.code, e.why);
        if (!OK(r)) break;
        CHECK(OK(nvn_gpu_present(sf, &dr, &pd, 697 + (uint64_t)k, NULL, NULL, &e)), "C697 present frame %d: %s", k, e.why);
    }
    // Exercise arrays/pending state beyond the old uint32_t bit mask.
    g_w697_count = 40;
    r = nvn_gpu_surface_rebuild(sf, &e);
    CHECK(OK(r), "C697 rebuild with 40 images: %d %s", r.code, e.why);
    if (OK(r)) {
        for (int k = 0; k < 35; k++) {
            nvn_gpu_drawable dr = {0};
            r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
            CHECK(OK(r), "C697 >32 acquire %d: %s", k, e.why);
            if (!OK(r)) break;
            CHECK(OK(nvn_gpu_present(sf, &dr, &pd, 797 + (uint64_t)k, NULL, NULL, &e)), "C697 >32 present %d: %s", k, e.why);
        }
        // Driver has signalled acquire slot 35, but returns the first invalid image index.
        g_w697_invalid = 1;
        nvn_gpu_drawable bad = {0};
        r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &bad, &e);
        CHECK(!OK(r) && bad.token == 0, "C697 invalid index must fail with empty drawable: rc=%d token=%llu", r.code, (unsigned long long)bad.token);
        g_w697_invalid = 0;
    }
    // Image and semaphore slots beyond 255 must not alias the old 8-bit token fields.
    g_w697_count = 264; g_w697_base = 256;
    r = nvn_gpu_surface_rebuild(sf, &e);
    CHECK(OK(r), "C697 rebuild 264 images: %s", e.why);
    if (OK(r)) {
        for (int k = 0; k < 258; k++) {
            nvn_gpu_drawable dr = {0}, held = {0};
            r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
            CHECK(OK(r), "C697 32-bit token acquire %d: %s", k, e.why);
            if (!OK(r)) break;
            nvn_gpu_drawable_release(sf, &dr);
            CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &held, &e)) && held.token == dr.token,
                  "C697 32-bit token release/reacquire %d", k);
            CHECK(OK(nvn_gpu_present(sf, &held, &pd, 897 + (uint64_t)k, NULL, NULL, &e)), "C697 32-bit token present %d: %s", k, e.why);
        }
    }
    nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
finish:
    if (sf) CHECK(OK(nvn_gpu_surface_detach(sf, 5000000000ull, &e)), "C697 detach: %s", e.why);
    CHECK(g_w697_created == g_w697_destroyed, "C697 semaphore cleanup: %u created / %u destroyed", g_w697_created, g_w697_destroyed);
    // Permanently incomplete enumeration must fail cleanly, never publish a partial swapchain.
    g_w697_count = 14; g_w697_incomplete = -1; sf = NULL;
    unsigned destroyed_before = g_w697_sc_destroyed;
    r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(!OK(r) && !sf, "C697 perpetual VK_INCOMPLETE must fail: %d", r.code);
    if (sf) nvn_gpu_surface_detach(sf, 5000000000ull, &e);
    CHECK(g_w697_sc_destroyed > destroyed_before, "C697 failed enumeration destroyed swapchain");
    CHECK(g_w697_created == g_w697_destroyed, "C697 failure semaphore cleanup");
    // Failure after some semaphores exist must free every successfully-created handle.
    g_w697_incomplete = 0; g_w697_fail_sem = 5; g_w697_base = 8; sf = NULL;
    r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(!OK(r) && !sf, "C697 partial semaphore creation must fail");
    if (sf) nvn_gpu_surface_detach(sf, 5000000000ull, &e);
    CHECK(g_w697_created == g_w697_destroyed, "C697 partial semaphore cleanup: %u / %u", g_w697_created, g_w697_destroyed);
    g_w697_fail_sem = 0;
    g_w697_count = 0; g_w697_incomplete = 0;
    printf("C697 synthetic swapchain enumeration/index/cleanup complete; failures=%d\n", g_fail);
}

// C73：NVN 窗口原点在左上时视口翻转。同一张 2x2 贴图、同一个三角形，读回的图应当上下对调（期望在 tri_impl 里按行号手算）。
static void c73_origin_upper_left(const char *dir) {
    static const uint8_t texels[16] = {255, 0, 0, 255,   0, 255, 0, 255,
                                       0, 0, 255, 255,   255, 255, 255, 255};
    g_tri_origin_ul = 1;
    tri_impl(dir, NVN_FORMAT_RGBA8, texels, 4, 8, 255);
    g_tri_origin_ul = 0;
}

// C72：块压缩贴图上传（一行是一排块）。BC1 一个 4x4 块：两个端点色都是纯红（565 的 0xF800）、索引全 0 → 解出来每个纹素都是不透明纯红。
// 2x2 的贴图只占这一个块的左上角。设备不支持 BC 直接采样时这条由 compute 解码路线负责，本用例只在原生支持时跑。
static void c72_bc1_upload(const char *dir, int required) {
    if (required) {
        nvn_gpu_tex_desc td = {0}; nvn_gpu_tex probe = NULL; nvn_gpu_err e = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA_DXT1;
        td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
        td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
        nvn_gpu_rc r = nvn_gpu_tex_create(&td, &probe, &e);
        CHECK(OK(r) && probe, "C72 compute：BC1 建图应成功，得到 %d %s", r.code, e.why);
        if (probe) nvn_gpu_tex_release(probe);
    }
    if (!(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE)) {
        CHECK(!required, "C72 compute：BC1 解码路线应可采样，实际未报告能力");
        if (!required) printf("C72 跳过：本设备档 BC1 不可采样\n");
        return;
    }
    printf("C72 执行：%s\n", required ? "compute" : "native");
    static const uint8_t block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    static const uint8_t red[4] = {255, 0, 0, 255};
    g_tri_expect = red;
    tri_impl(dir, NVN_FORMAT_RGBA_DXT1, block, 4, 8, 255);
    g_tri_expect = NULL;
}

// C192：压缩块行带 8 字节填充；两次上传之间抓一次快照，再检查 abort 不改变结果。
static void c192_bc_order(void) {
    static const uint8_t blocks[48] = {
        0x00,0xf8,0x00,0xf8,0,0,0,0, 0xe0,0x07,0xe0,0x07,0,0,0,0, 0,0,0,0,0,0,0,0,
        0x1f,0x00,0x1f,0x00,0,0,0,0, 0xff,0xff,0xff,0xff,0,0,0,0, 0,0,0,0,0,0,0,0
    };
    static const uint8_t blue[8] = {0x1f,0,0x1f,0,0,0,0,0};
    static const uint8_t colors[4][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
    nvn_gpu_tex bc = make_tex(8, 8, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex a = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex b = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(8 * 8 * 4 * 3, NVN_GPU_MEM_SHARED, NULL);
    if (!bc || !a || !b || !rb) return;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blocks, 24, sizeof blocks);
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, a, 0, 0, NULL);
    NVNcopyRegion region = {4,0,0,4,4,1};
    nvn_gpu_tex_upload(c, bc, 0, 0, &region, blue, 8, sizeof blue);
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, b, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, a, 0, 0, NULL, rb, 0, 32, 256);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 256, 32, 256);
    nvn_gpu_err e; nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(OK(rc), "C192 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(6, 5000000000ull, &e)), "C192 wait：%s", e.why);
    c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, &region, blocks, 8, 8);
    nvn_gpu_cmd_abort(c);
    c = begin_cmd();
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, b, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 512, 32, 256);
    rc = nvn_gpu_cmd_commit(c, 7, on_done, NULL, &e);
    CHECK(OK(rc), "C192 abort 后 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(7, 5000000000ull, &e)), "C192 abort 后 wait：%s", e.why);
    for (int image = 0; image < 3; image++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        int k = (y / 4) * 2 + x / 4; if (image && k == 1) k = 2;
        CHECK(memcmp(buf_cpu(rb) + image * 256 + (y * 8 + x) * 4, colors[k], 4) == 0,
              "C192 快照 %d 坐标 %d,%d 应为颜色 %d", image, x, y, k);
    }
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(a); nvn_gpu_tex_release(b); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C193：现解贴图按压缩块回读：给上传时的原字节（不是解码后的颜色）；没上传过的区域给零。
static void c193_bc_readback_rejected(void) {
    nvn_gpu_tex bc = make_tex(8, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    uint8_t init[64]; memset(init, 0x5a, sizeof init);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, init);
    if (!bc || !rb) return;
    static const uint8_t blk[8] = {0x11, 0xf8, 0x22, 0x07, 0x33, 0x44, 0x55, 0x66};
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e;
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, NULL, rb, 0, 16, 16);              // ① 一次都没上传：两块都是零
    NVNcopyRegion right = {4, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(c, bc, 0, 0, &right, blk, 8, 8);                  // 只传右边那块
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, NULL, rb, 32, 16, 16);             // ② 左块零、右块原字节
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "C193 解码贴图按压缩块回读 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C193 wait：%s", e.why);
    uint8_t want[64]; memcpy(want, init, sizeof want);
    memset(want, 0, 16); memset(want + 32, 0, 8); memcpy(want + 40, blk, 8);
    CHECK(memcmp(buf_cpu(rb), want, sizeof want) == 0, "C193 解码贴图回读应给原压缩字节（没上传过的为零）");
    nvn_gpu_tex_release(bc); nvn_gpu_buf_release(rb, NULL, NULL);
}

// 等段释放（工作线程在完成后回收段的临时资源）：最多 2 秒，看解码临时图归零。
static int bc_track_wait_drained(void) {
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&g_bc_track_mu); int live = g_bc_img_live; pthread_mutex_unlock(&g_bc_track_mu);
        if (!live) return 1;
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
    return 0;
}

// C196：CPU 字节 → BC 的解码录在段里，放弃 / 失败的段连同解码资源一起回收；没有「解码已单独提交、段却放弃」的悬空资源，close 不用另等。
//   ① 上传后 abort：临时图与解码输入图立即回收，close 立即成功。
//   ② 第 2 片的解码输入图建失败（BC4 3D 两片）：段粘滞 DEVICE（BC 解码失败），commit 报错；第 1 片已建的资源随段回收。
static void c196_bc_abort_reclaim(void) {
    static const uint8_t red[8] = {0,0xf8,0,0xf8,0,0,0,0};
    nvn_gpu_tex bc = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    if (!bc) return;
    bc_track_reset(); g_bc_track = 1;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, red, 8, 8);
    CHECK(g_bc_img_live == 2, "C196 ① 上传后段内持有临时图 + 解码输入图 2 张，实得 %d", g_bc_img_live);
    nvn_gpu_cmd_abort(c);
    CHECK(g_bc_img_live == 0, "C196 ① abort 后解码临时资源应全部回收，剩 %d", g_bc_img_live);
    nvn_gpu_tex_release(bc);
    nvn_gpu_err e; nvn_gpu_rc r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "C196 ① abort 后 close 应立即成功：%d %s", r.code, e.why);
    if (!open_backend()) { g_bc_track = 0; return; }
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = NVN_FORMAT_RGTC1_UNORM;   // 现解只收 BC4 的 3D
    td.d.width = td.d.height = 4; td.d.depth = 2; td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex t3 = NULL; r = nvn_gpu_tex_create(&td, &t3, &e);
    CHECK(OK(r) && t3, "C196 ② 建 3D BC4：%d %s", r.code, e.why);
    if (t3) {
        uint8_t two[16]; memcpy(two, red, 8); memcpy(two + 8, red, 8);
        bc_track_reset(); g_bc_fail_in = 2;
        c = begin_cmd();
        nvn_gpu_tex_upload(c, t3, 0, 0, NULL, two, 8, 16);
        g_bc_fail_in = 0;
        CHECK(g_bc_in_seen == 2, "C196 ② 第 2 片解码输入图注入未命中（%d）", g_bc_in_seen);
        r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        CHECK(r.code == NVN_GPU_E_DEVICE && strstr(e.why, "BC 解码失败"), "C196 ② 解码资源建失败应 commit 报 DEVICE：%d %s", r.code, e.why);
        if (OK(r)) nvn_gpu_fence_wait(1, 5000000000ull, &e);
        CHECK(bc_track_wait_drained(), "C196 ② 失败段的解码临时资源应随段回收，剩 %d", g_bc_img_live);
        nvn_gpu_tex_release(t3);
    }
    g_bc_track = 0;
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "C196 ② 失败段之后 close：%d %s", r.code, e.why);
    printf("C196 段内解码：abort / 失败段回收已执行\n");
}

// C197（修前红）：CPU 字节 → BC 现解贴图上传不单独提交、不在调用线程上等栅栏；解码与之后的采样同在一段、按 GPU 顺序生效。
//   修前：bc_upload 在 g_submit_mu 里单独 vkQueueSubmit 解码再等 fence（每切片 1 次提交 + 1 次等待）。
//   同一段里：上传 A（红）→ 拷走 → 上传 B（蓝，同一贴图）→ 拷走 → 回读两份，再按压缩块回读影子（应是 B 的原字节）。
static void c197_bc_upload_in_segment(void) {
    static const uint8_t red[8] = {0x00,0xf8,0x00,0xf8,0,0,0,0}, blue[8] = {0x1f,0,0x1f,0,0,0,0,0};
    static const uint8_t want_red[4] = {255,0,0,255}, want_blue[4] = {0,0,255,255};
    nvn_gpu_tex bc = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex a = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED), b = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(160, NVN_GPU_MEM_SHARED, NULL);
    if (!bc || !a || !b || !rb) return;
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e;
    t_bc_submits = t_bc_waits = 0; t_bc_watch = 1;
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, red, 8, 8);
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, a, 0, 0, NULL);
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blue, 8, 8);
    t_bc_watch = 0;
    unsigned submits = t_bc_submits, waits = t_bc_waits;
    CHECK(submits == 0 && waits == 0, "C197 CPU BC 上传在调用线程上不应提交 / 等栅栏：提交 %u 次、等待 %u 次", submits, waits);
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, b, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, a, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 64, 16, 64);
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, NULL, rb, 128, 8, 8);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "C197 commit：%d %s", r.code, e.why);
    if (OK(r)) {
        CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C197 wait：%s", e.why);
        for (int p = 0; p < 16; p++) {
            CHECK(!memcmp(buf_cpu(rb) + p * 4, want_red, 4), "C197 第一次上传后拷走的应是红，像素 %d", p);
            CHECK(!memcmp(buf_cpu(rb) + 64 + p * 4, want_blue, 4), "C197 第二次上传后拷走的应是蓝，像素 %d", p);
        }
        CHECK(!memcmp(buf_cpu(rb) + 128, blue, 8), "C197 压缩影子回读应是最后一次上传的原字节");
    }
    printf("C197 CPU BC 上传：调用线程提交 %u 次、等待 %u 次\n", submits, waits);
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(a); nvn_gpu_tex_release(b); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C198：同一段里大量 / 大尺寸 CPU BC 上传（三张 1024x1024 BC1 全 mip 链 + 一张 128x128x8 BC4 3D，共 41 片）。
//   arena 不够时就地新建暂存块（不等、不提交）：超过标准块（256 KB）的切片各占一块恰好的大块，小切片挤标准块（可能取自复用池）；
//   新建的暂存不超过「压缩总量 + 每片对齐 + 两块标准块」，不翻倍；解码临时图（每片临时图 + 输入图）在段完成前全留着、完成后归零，
//   大块暂存随段释放。内容抽查：每张 2D 的第 0 级 4x4 与最末级 1x1、3D 全部 8 片的 4x4。
static nvn_gpu_tex make_tex_3d_levels(uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t fmt, uint32_t usage);
static void c198_bc_bulk_upload(void) {
    enum { N2 = 3, W2 = 1024, L2 = 11, W3 = 128, D3 = 8, STD = 256 * 1024 };
    static const uint16_t col565[N2] = {0xf800, 0x07e0, 0x001f};
    static const uint8_t want[N2][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255}};
    nvn_gpu_tex tex[N2 + 1] = {0}; nvn_gpu_err e = {0}; nvn_gpu_rc r;
    for (int i = 0; i <= N2; i++) {
        nvn_gpu_tex_desc td = {0}; td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
        if (i < N2) { td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA_DXT1; td.d.width = td.d.height = W2; td.d.depth = 1; td.d.levels = L2; }
        else { td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = NVN_FORMAT_RGTC1_UNORM; td.d.width = td.d.height = W3; td.d.depth = D3; td.d.levels = 1; }   // 现解只收 BC4 的 3D
        r = nvn_gpu_tex_create(&td, &tex[i], &e);
        CHECK(OK(r) && tex[i], "C198 建图 %d：%d %s", i, r.code, e.why);
        if (!tex[i]) return;
    }
    size_t big = (size_t)(W2 / 4) * (W2 / 4) * 8;
    uint8_t *src = malloc(big);
    nvn_gpu_tex out = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex out3 = make_tex_3d_levels(4, 4, D3, 1, NVN_FORMAT_R8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(128 * N2 + 16 * D3, NVN_GPU_MEM_SHARED, NULL);
    if (!src || !out || !out3 || !rb) { free(src); return; }
    bc_track_reset(); g_bc_track = 1;
    nvn_gpu_cmd c = begin_cmd();
    uint64_t total = 0, dedicated = 0; int slices = 0;
    t_bc_submits = t_bc_waits = 0; t_bc_watch = 1;
    for (int i = 0; i <= N2; i++) {
        if (i == N2) {   // BC4 块：两端点都是 200、索引全 0 → 每个纹素 200
            for (size_t k = 0; k < big; k += 8) { uint8_t *b = src + k; b[0] = b[1] = 200; memset(b + 2, 0, 6); }
            uint64_t row = (uint64_t)(W3 / 4) * 8, slice = row * (W3 / 4);
            nvn_gpu_tex_upload(c, tex[i], 0, 0, NULL, src, row, slice * D3);
            total += slice * D3; slices += D3; if (slice + 16 > STD) dedicated += slice * D3;
            break;
        }
        for (size_t k = 0; k < big; k += 8) { uint8_t *b = src + k; b[0] = b[2] = (uint8_t)col565[i]; b[1] = b[3] = (uint8_t)(col565[i] >> 8); memset(b + 4, 0, 4); }
        for (uint32_t l = 0; l < L2; l++) {
            uint32_t w = W2 >> l, bw = (w + 3) / 4; uint64_t row = (uint64_t)bw * 8, bytes = row * bw;
            nvn_gpu_tex_upload(c, tex[i], l, 0, NULL, src, row, bytes);
            total += bytes; slices++; if (bytes + 16 > STD) dedicated += bytes;
        }
    }
    t_bc_watch = 0;
    CHECK(t_bc_submits == 0 && t_bc_waits == 0, "C198 大批上传期间调用线程提交 %u 次、等待 %u 次（应 0）", t_bc_submits, t_bc_waits);
    pthread_mutex_lock(&g_bc_track_mu); int img_peak = g_bc_img_peak; VkDeviceSize stage_peak = g_bc_stage_peak; pthread_mutex_unlock(&g_bc_track_mu);
    CHECK(img_peak == 2 * slices, "C198 段内应持有每片的临时图 + 输入图（%d 片 → %d 张），实得峰值 %d", slices, 2 * slices, img_peak);
    CHECK(stage_peak >= dedicated && stage_peak <= total + (VkDeviceSize)slices * 16 + 2u * STD,
          "C198 新建暂存峰值 %llu 字节：应 ≥ 大切片合计 %llu、≤ 压缩总量 %llu + 对齐 + 两块标准块（每片一份，不翻倍）",
          (unsigned long long)stage_peak, (unsigned long long)dedicated, (unsigned long long)total);
    for (int i = 0; i < N2; i++) {
        NVNcopyRegion r4 = {0, 0, 0, 4, 4, 1}, r1 = {0, 0, 0, 1, 1, 1};
        nvn_gpu_copy_tex(c, tex[i], 0, 0, &r4, out, 0, 0, &r4); nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, (uint64_t)128 * i, 16, 64);
        nvn_gpu_copy_tex(c, tex[i], L2 - 1, 0, &r1, out, 0, 0, &r1); nvn_gpu_copy_tex_buf(c, out, 0, 0, &r1, rb, (uint64_t)128 * i + 64, 4, 4);
    }
    { NVNcopyRegion r3 = {0, 0, 0, 4, 4, D3};
      nvn_gpu_copy_tex(c, tex[N2], 0, 0, &r3, out3, 0, 0, &r3); nvn_gpu_copy_tex_buf(c, out3, 0, 0, &r3, rb, (uint64_t)128 * N2, 4, 16 * D3); }
    r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "C198 commit：%d %s", r.code, e.why);
    if (OK(r)) {
        CHECK(OK(nvn_gpu_fence_wait(1, 10000000000ull, &e)), "C198 wait：%s", e.why);
        const uint8_t *o = buf_cpu(rb);
        for (int i = 0; i < N2; i++) {
            for (int p = 0; p < 16; p++) CHECK(!memcmp(o + 128 * i + p * 4, want[i], 4), "C198 2D %d 第 0 级像素 %d：%u,%u,%u", i, p, o[128 * i + p * 4], o[128 * i + p * 4 + 1], o[128 * i + p * 4 + 2]);
            CHECK(!memcmp(o + 128 * i + 64, want[i], 4), "C198 2D %d 最末级 1x1：%u,%u,%u", i, o[128 * i + 64], o[128 * i + 65], o[128 * i + 66]);
        }
        for (int p = 0; p < 16 * D3; p++) CHECK(o[128 * N2 + p] == 200, "C198 BC4 3D 纹素 %d：%u，期望 200", p, o[128 * N2 + p]);
    }
    CHECK(bc_track_wait_drained(), "C198 段完成后解码临时图应归零，剩 %d", g_bc_img_live);
    pthread_mutex_lock(&g_bc_track_mu); VkDeviceSize stage_left = 0;
    for (int k = 0; k < BC_TRACK_MAX; k++) if (g_bc_bufs[k] && g_bc_buf_size[k] != STD) stage_left += g_bc_buf_size[k];
    pthread_mutex_unlock(&g_bc_track_mu);
    CHECK(stage_left == 0, "C198 段完成后大块暂存应释放（标准块留给复用池），剩 %llu 字节", (unsigned long long)stage_left);
    g_bc_track = 0;
    printf("C198 同段大批 CPU BC 上传：%d 片、压缩 %llu 字节（大切片 %llu）、新建暂存峰值 %llu 字节、解码临时图峰值 %d 张，完成后归零\n",
           slices, (unsigned long long)total, (unsigned long long)dedicated, (unsigned long long)stage_peak, img_peak);
    for (int i = 0; i <= N2; i++) nvn_gpu_tex_release(tex[i]);
    nvn_gpu_tex_release(out); nvn_gpu_tex_release(out3); nvn_gpu_buf_release(rb, NULL, NULL); free(src);
}

// C697e：小于一个压缩块的 mip（8×8 BC1 的第 2 级是 2×2）按整块区域 4×4 上传（NVN 的写法），应裁到边照常写入、读回纯蓝；
//   区域超过边缘所在的整块（8×8）仍非法（commit 报 E_ARG）。#697 Mali 上「BC 上传子资源或区域非法」的一种来源。
// 两处调用都排在会话最后、紧接 close：signal 取 100 / 101，高于同会话前面各测试用过的值。
static void c697e_bc_small_mip_block_region(int decoded) {   // decoded=0：原生 BC 不能拷到 RGBA8，只核上传接受 / 超界拒
    static const uint8_t blue[8] = {0x1f,0,0x1f,0,0,0,0,0};
    static const uint8_t expect[4] = {0,0,255,255};
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA_DXT1;
    td.d.width = td.d.height = 8; td.d.depth = 1; td.d.levels = 3; td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex bc = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &bc, &e);
    CHECK(OK(rc) && bc, "C697e 建图：%d %s", rc.code, e.why); if (!bc) return;
    nvn_gpu_tex out = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    if (!out || !rb) return;
    NVNcopyRegion whole_block = {0, 0, 0, 4, 4, 1};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 2, 0, &whole_block, blue, 8, 8);
    if (decoded) {
        nvn_gpu_copy_tex(c, bc, 2, 0, NULL, out, 0, 0, NULL);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 8, 16);
    }
    rc = nvn_gpu_cmd_commit(c, 100, on_done, NULL, &e); CHECK(OK(rc), "C697e 整块区域写 2×2 级应接受：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(100, 5000000000ull, &e)), "C697e wait：%s", e.why);
    if (decoded) for (int i = 0; i < 4; i++) CHECK(!memcmp(buf_cpu(rb) + i * 4, expect, 4), "C697e 像素 %d 应为纯蓝", i);
    NVNcopyRegion too_big = {0, 0, 0, 8, 8, 1};
    c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 2, 0, &too_big, blue, 16, 64);
    rc = nvn_gpu_cmd_commit(c, 101, on_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_ARG || rc.code == NVN_GPU_E_FAILED, "C697e 超过整块的区域仍应非法：%d %s", rc.code, e.why);
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(out); nvn_gpu_buf_release(rb, NULL, NULL);
    printf("C697e 小 mip 整块区域：已核对\n");
}

// C697f：缓冲 → 3D BC 现解贴图，调用方按 NVN 的写法把切片号放在 layer 里（#697 Mali：32×32×32、layers 1 的 BC 贴图按 layer 1 进来）。
//   与直拷同一口径：layer 当 z 切片，应接受、提交成功；修前现解分支按数组层判「层越界」，commit 报 E_ARG。排在 C697e 前（它最后一笔故意失败）。
static void c697f_bc_3d_slice_from_buf(void) {
    static const uint8_t blue[32] = {0xff,0,0,0,0,0,0,0, 0xff,0,0,0,0,0,0,0, 0xff,0,0,0,0,0,0,0, 0xff,0,0,0,0,0,0,0};   // 4 个 BC4 块（每块 8 字节）
    // 现解路线只给 BC4 开 3D（backend_vulkan.c tex_create；报告人那张正是 0x49 BC4）：本用例临时把 BC4 加进现解名单
    uint32_t saved_mask = g_dev.bc_decode_mask; g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_RGTC1_UNORM - 0x41);
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = NVN_FORMAT_RGTC1_UNORM;
    td.d.width = td.d.height = 8; td.d.depth = 2; td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex bc = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &bc, &e);
    CHECK(OK(rc) && bc, "C697f 建 3D 图：%d %s", rc.code, e.why); if (!bc) { g_dev.bc_decode_mask = saved_mask; return; }
    nvn_gpu_buf src = make_buf(sizeof blue, NVN_GPU_MEM_SHARED, blue);
    if (!src) { nvn_gpu_tex_release(bc); g_dev.bc_decode_mask = saved_mask; return; }
    NVNcopyRegion whole = {0, 0, 0, 8, 8, 1};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_copy_buf_tex(c, src, 0, 16, 32, bc, 0, 1, &whole);
    rc = nvn_gpu_cmd_commit(c, 90, on_done, NULL, &e); CHECK(OK(rc), "C697f 缓冲 → 3D BC 第 1 切片应接受：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(90, 5000000000ull, &e)), "C697f wait：%s", e.why);
    nvn_gpu_tex_release(bc); nvn_gpu_buf_release(src, NULL, NULL); g_dev.bc_decode_mask = saved_mask;
    printf("C697f 3D BC 按切片号上传：已核对\n");
}

// C194：8×8 立方体的一级 mip 是 4×4，两个面分别为纯蓝、纯绿。
static void c194_bc_cube_mips(void) {
    static const uint8_t blue[8] = {0x1f,0,0x1f,0,0,0,0,0};
    static const uint8_t green[8] = {0xe0,7,0xe0,7,0,0,0,0};
    static const uint8_t expected[2][4] = {{0,0,255,255}, {0,255,0,255}};
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_CUBEMAP; td.d.format = NVN_FORMAT_RGBA_DXT1;
    td.d.width = td.d.height = 8; td.d.depth = 1; td.d.levels = 2; td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_VIEW_FMT;
    nvn_gpu_tex bc = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &bc, &e);
    CHECK(OK(rc) && bc, "C194 立方体建图：%d %s", rc.code, e.why); if (!bc) return;
    view_t same = {0}; same.set_format = 1; same.format = NVN_FORMAT_RGBA_DXT1;
    nvn_gpu_view v = NULL; rc = nvn_gpu_view_create(bc, &same, &v, &e);
    CHECK(OK(rc) && v, "C194 同格式显式视图：%d %s", rc.code, e.why);
    nvn_gpu_tex out = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(128, NVN_GPU_MEM_SHARED, NULL);
    if (!out || !rb) return;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 1, 2, NULL, blue, 8, 8);
    nvn_gpu_tex_upload(c, bc, 1, 4, NULL, green, 8, 8);
    nvn_gpu_copy_tex(c, bc, 1, 2, NULL, out, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_copy_tex(c, bc, 1, 4, NULL, out, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 64, 16, 64);
    rc = nvn_gpu_cmd_commit(c, 8, on_done, NULL, &e); CHECK(OK(rc), "C194 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(8, 5000000000ull, &e)), "C194 wait：%s", e.why);
    for (int i = 0; i < 32; i++) CHECK(!memcmp(buf_cpu(rb) + i * 4, expected[i / 16], 4), "C194 面 %d 像素 %d", i / 16, i % 16);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(bc); nvn_gpu_tex_release(out); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C30：RGB8 上传展开成 RGBA8 后经三角形采样读回；期望按 2x2 最近邻 + a=0xFF 手算。
static void t30_rgb8_sampled(const char *dir) {
    static const uint8_t texels[12] = {255, 0, 0,   0, 255, 0,
                                       0, 0, 255,   255, 255, 255};
    tri_impl(dir, NVN_FORMAT_RGB8, texels, 3, 6, 255);
}

// close 义务：在途段先 done 再返回；返回后零回调、新工作 STOPPED。
static void t10_close_inflight(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x77777777u);
    nvn_gpu_err e;
    int done_before = atomic_load(&g_ndone);   // 必须在 commit 前取：worker 可能先完成
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "close：%d %s", r.code, e.why);
    CHECK(atomic_load(&g_ndone) == done_before + 1, "close 返回前在途段没有恰好 done 一次");
    sleep_ms(50);
    CHECK(atomic_load(&g_ndone) == done_before + 1, "close 返回后仍有回调");
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_STOPPED, "close 后 cmd_begin 应 STOPPED");
}


// 真实 fence 失败路径：F 取最小、后续段 FAILED、完成值停在 F 之前。
static void t16_fence_failure(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf c = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, a, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, c, 0, 16, 0x33333333u);
    int base = atomic_load(&g_ndone);
    nvn_gpu_vk_test_fail_fence(2);
    // 先让后台在段 1 上停 200 ms：三段都提交进去之后段 2 才会失败。不压住的话，CPU 被抢时后台可能在段 3 提交之前
    // 就处理完段 2 的失败，段 3 提交直接回 E_FAILED（时间线已失败终态），这一例就测不到「失败之后已提交的段也判失败」。
    nvn_gpu_vk_test_delay_segment(1, 200);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c1, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_cmd_commit(c2, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_cmd_commit(c3, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
    CHECK(OK(r), "wait(1)：%d %s", r.code, e.why);
    CHECK(nvn_gpu_fence_wait(2, 5000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(2) 应 FAILED");
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    CHECK(nvn_gpu_fence_done() == 1, "完成值应停在 1，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_wait(3, 2000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(3) 应 FAILED");
    const struct done_rec *d2 = wait_signal(2, base, 5000), *d3 = wait_signal(3, base, 5000);
    CHECK(d2 && d2->status == NVN_GPU_DONE_FAILED, "段 2 回调不是 FAILED");
    CHECK(d3 && d3->status == NVN_GPU_DONE_FAILED, "段 3 回调不是 FAILED");
    nvn_gpu_vk_test_delay_segment(0, 0);   // 钩子不留给后面的用例
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    nvn_gpu_buf_release(c, NULL, NULL);
}

// C13 TIMEOUT：close(0) 在途段没完成 → TIMEOUT 且资源保留；再 close 完成，done 恰好一次。
static void t17_close_timeout(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x55555555u);
    int base = atomic_load(&g_ndone);
    nvn_gpu_vk_test_delay_segment(1, 300);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_close(0, &e);
    CHECK(r.code == NVN_GPU_E_TIMEOUT, "close(0) 应 TIMEOUT，实为 %d", r.code);
    CHECK(atomic_load(&g_ndone) == base, "TIMEOUT 前不应有 done");
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "第二次 close：%d %s", r.code, e.why);
    CHECK(atomic_load(&g_ndone) == base + 1, "close 完成后 done 应恰好 +1");
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_STOPPED, "close 后 cmd_begin 应 STOPPED");
}


// 组合采样管线的公共搭建（C25）：FS 用 sampler2D，布局里 cls = TEX_SAMP。
static nvn_gpu_pipe make_combined_pipe(const char *dir, nvn_gpu_shader *vs_out, nvn_gpu_shader *fs_out) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri_combined.frag.spv", dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fs, "读不到组合采样 SPIR-V");
    if (!vs || !fs) { free(vs); free(fs); return NULL; }
    nvn_gpu_layout_ent ent;
    memset(&ent, 0, sizeof ent);
    ent.set = 0; ent.cls = NVN_GPU_RES_TEX_SAMP; ent.access = NVN_GPU_ACC_R;
    ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = &ent; fl.nent = 1;
    nvn_gpu_layout vl;
    memset(&vl, 0, sizeof vl);
    nvn_gpu_err e;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.stage = NVN_GPU_SH_VS; src.blob = vs; src.bytes = vsz; src.layout = &vl;
    CHECK(OK(nvn_gpu_shader_load(&src, vs_out, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    src.stage = NVN_GPU_SH_FS; src.blob = fs; src.bytes = fsz; src.layout = &fl;
    CHECK(OK(nvn_gpu_shader_load(&src, fs_out, &e)), "FS 装载：%d %s", e.rc.code, e.why);
    free(vs);
    free(fs);
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = *vs_out;
    pd.st[NVN_GPU_SH_FS].sh = *fs_out;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "pipe_get：%d %s", e.rc.code, e.why);
    return pipe;
}

static void check_triangle_pixels(nvn_gpu_buf rb, const uint8_t texels[16]) {
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float u = (x + 0.5f) / 64.0f, v = (y + 0.5f) / 64.0f;
            int tx = (int)(u * 2.0f); if (tx > 1) tx = 1;
            int ty = (int)(v * 2.0f); if (ty > 1) ty = 1;
            const uint8_t *want = texels + (ty * 2 + tx) * 4;
            const uint8_t *got = buf_cpu(rb) + (y * 64 + x) * 4;
            if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2] || got[3] != want[3]) {
                g_fail++;
                printf("FAIL %s:%d: 三角像素 (%d,%d) = %02x%02x%02x%02x，期望 %02x%02x%02x%02x\n",
                       __FILE__, __LINE__, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                return;
            }
        }
    }
}

// C25 正例：cls = TEX_SAMP，u.ts 同时给 view 与 samp，采样结果与分开绑定一致。
static void t18_combined_sampler(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_combined_pipe(dir, &vs, &fs);
    if (!pipe) return;
    const float verts[3][4] = {{-1.0f, -1.0f, 0.0f, 0.0f}, {3.0f, -1.0f, 2.0f, 0.0f}, {-1.0f, 3.0f, 0.0f, 2.0f}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    nvn_gpu_tex tex = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view tview = make_view(tex);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "samp_create：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view oview = make_view(out);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, texels, 8, 16);
    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = tview; b.u.ts.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_samp_release(samp);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "wait 1：%d %s", e.rc.code, e.why);
    check_triangle_pixels(rb, texels);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
}

// C26：signal 违约（重复 5 / 0）→ E_ARG、F=6、各回调一次 FAILED；之后 7 也 FAILED。
static void t19_signal_contract(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_err e;
    nvn_gpu_cmd c0 = begin_cmd();
    nvn_gpu_fill_buf(c0, b, 0, 16, 0xAABBCCDDu);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c0, 5, on_done, NULL, &e);
    CHECK(OK(r), "合法 commit 5：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(5, 5000000000ull, &e)), "wait 5：%d %s", e.rc.code, e.why);
    int base = atomic_load(&g_ndone);
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, b, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, b, 0, 16, 0x33333333u);
    r = nvn_gpu_cmd_commit(c1, 5, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "重复 signal 5 应 E_ARG，实为 %d", r.code);
    r = nvn_gpu_cmd_commit(c2, 0, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "signal 0 应 E_ARG，实为 %d", r.code);
    r = nvn_gpu_cmd_commit(c3, 7, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "失败终态后 signal 7 应 E_FAILED，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 6, "F 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    CHECK(nvn_gpu_fence_done() == 5, "完成值应停在 5，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_wait(6, 2000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(6) 应 FAILED");
    CHECK(wait_until(&g_ndone, base + 3, 5000), "应有 3 次 done（三次被拒段）");
    int nfail = 0, zero_seen = 0;
    for (int i = base; i < base + 3 && i < 64; i++) {
        if (g_done[i].status == NVN_GPU_DONE_FAILED) nfail++;
        if (g_done[i].signal == 0 && g_done[i].status == NVN_GPU_DONE_FAILED) zero_seen = 1;
    }
    CHECK(nfail == 3, "FAILED 回调应有 3 次，实为 %d", nfail);
    CHECK(zero_seen, "signal 0 的被拒段没有 FAILED 回调");
    uint32_t word;
    memcpy(&word, buf_cpu(b), 4);
    CHECK(word == 0xAABBCCDDu, "被拒段改写了缓冲：0x%08x", word);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// C25 反例：TEX_SAMP 缺 samp → 粘滞 E_LAYOUT，提交返回 E_LAYOUT 并进入失败终态。
static void t20_combined_missing_samp(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_combined_pipe(dir, &vs, &fs);
    if (!pipe) return;
    nvn_gpu_tex out = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view oview = make_view(out);
    nvn_gpu_tex tex = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view tview = make_view(tex);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 8; pass.h = 8; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = tview; b.u.ts.samp = NULL;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_LAYOUT, "缺 samp 的 TEX_SAMP 应 E_LAYOUT，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
}



// C27：clear_view 的 is_int 必须与贴图格式一致（中立格式表驱动）。
static void t21_clear_int(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGBA8UI, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    const uint32_t vals[4] = {1, 2, 3, 4};
    nvn_gpu_clear_view(c, v, NULL, vals, 1);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "整数 clear commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 2000000000ull, &e)), "整数 clear wait：%d %s", e.rc.code, e.why);
    for (int i = 0; i < 16; i++) {
        uint32_t word;
        memcpy(&word, buf_cpu(b) + i * 4, 4);
        CHECK(word == 0x04030201u, "整数像素 %d = 0x%08x，期望 0x04030201", i, word);
    }
    // 反例：浮点颜色贴图却用 is_int=1，必须 E_ARG 且不执行
    nvn_gpu_tex ft = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view fv = make_view(ft);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_clear_view(c2, fv, NULL, vals, 1);
    r = nvn_gpu_cmd_commit(c2, 2, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "浮点贴图用 is_int 应 E_ARG，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(fv);
    nvn_gpu_tex_release(ft);
    nvn_gpu_buf_release(b, NULL, NULL);
}


// C27b：视图覆盖格式的 clear 明确不支持（Vulkan 按 image 格式解释 clear 值）。
static void t22_clear_view_fmt(void) {
    // 带存储用途：自由可改格式（跨类原始位视图只在存储图上保留；普通颜色图走格式列表保 UBWC，见 t22b）
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_VIEW_FMT);
    view_t vd;
    memset(&vd, 0, sizeof vd);
    vd.set_format = 1;
    vd.format = NVN_FORMAT_RGBA8UI;
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create(t, &vd, &v, &e);
    CHECK(OK(r), "覆盖格式视图创建：%d %s", r.code, e.why);
    nvn_gpu_cmd c = begin_cmd();
    const uint32_t vals[4] = {1, 2, 3, 4};
    nvn_gpu_clear_view(c, v, NULL, vals, 1);
    r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "覆盖格式 clear 应 E_UNSUPPORTED，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
}


// 保 Adreno UBWC（格式列表代替自由 MUTABLE_FORMAT）：普通颜色图按 sRGB 孪生看照常建视图、不退回；
//   跨类格式（RGBA8 → RGBA8UI）不在列表，退回图本身格式建视图并计数（不失败、不跳笔）；深度图不开 MUTABLE，按本格式看照常。
//   期望（手算）：孪生视图 0 次退回；BGRA 视图 0 次（通道互换等效）；跨类视图 1 次；深度本格式视图 0 次。全程校验层 0 报错由 run.sh 检测器判。
uint64_t nvn_vk_tex_view_fallbacks(void);
int nvn_vk_tex_fmt_list_active(void);
static void t22b_ubwc_format_list(void) {
    nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
    if (!nvn_vk_tex_fmt_list_active()) { printf("【未证】t22b：本设备没启用 VK_KHR_image_format_list，格式列表模式不生效，跳过\n"); return; }
    uint64_t f0 = nvn_vk_tex_view_fallbacks();
    nvn_gpu_tex t = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT);
    view_t vd; memset(&vd, 0, sizeof vd); vd.set_format = 1; vd.format = NVN_FORMAT_RGBA8_SRGB;
    nvn_gpu_view v = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_view_create(t, &vd, &v, &e);
    CHECK(OK(r), "列表模式：sRGB 孪生视图创建 %d %s", r.code, e.why);
    CHECK(nvn_vk_tex_view_fallbacks() == f0, "列表模式：孪生视图不应退回，实为 %llu 次", (unsigned long long)(nvn_vk_tex_view_fallbacks() - f0));
    if (v) nvn_gpu_view_release(v);
    // RGBA8 ↔ BGRA8：列表里不放 BGRA，按本格式 + 互换 R/B 的通道映射等效（像素读回由 t_feedback 第 8 种钉住），也不算退回
    vd.format = NVN_FORMAT_BGRA8; v = NULL;
    r = nvn_gpu_view_create(t, &vd, &v, &e);
    CHECK(OK(r), "列表模式：BGRA 视图创建 %d %s", r.code, e.why);
    CHECK(nvn_vk_tex_view_fallbacks() == f0, "列表模式：BGRA 视图应由通道互换等效、不退回，实为 %llu 次", (unsigned long long)(nvn_vk_tex_view_fallbacks() - f0));
    if (v) nvn_gpu_view_release(v);
    vd.format = NVN_FORMAT_RGBA8UI; v = NULL;
    r = nvn_gpu_view_create(t, &vd, &v, &e);
    CHECK(OK(r), "列表模式：跨类视图应退回本格式而不是失败 %d %s", r.code, e.why);
    CHECK(nvn_vk_tex_view_fallbacks() == f0 + 1, "列表模式：跨类视图应退回 1 次，实为 %llu 次", (unsigned long long)(nvn_vk_tex_view_fallbacks() - f0));
    if (v) nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_tex dt = make_tex(8, 8, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT);
    memset(&vd, 0, sizeof vd); vd.set_format = 1; vd.format = NVN_FORMAT_DEPTH32F; v = NULL;
    r = nvn_gpu_view_create(dt, &vd, &v, &e);
    CHECK(OK(r), "深度图（不开 MUTABLE）：本格式视图创建 %d %s", r.code, e.why);
    CHECK(nvn_vk_tex_view_fallbacks() == f0 + 1, "深度图本格式视图不应退回");
    if (v) nvn_gpu_view_release(v);
    nvn_gpu_tex_release(dt);
    (void)cp;
}

// C76：单次分配上限必须来自 maxMemoryAllocationSize，不能与 maxStorageBufferRange 共用同一个值。
// 以前两者相等（都是 128 MiB）：回放核心把内存池整池镜像成一块 CPU 可见缓冲，任何大于 SSBO 范围上限的池
// 都建不出镜像，池里的顶点 / 索引取不到，真机大地图进游戏后几何大面积不画（只剩 UI 与少数小池物件）。
// 这里用一个「超过 SSBO 范围上限、仍在分配上限内」的缓冲做判据：修前 E_ARG，修后成功。
static void t76_buf_create_alloc_cap(void) {
    nvn_gpu_caps cp;
    nvn_gpu_caps_get(&cp);
    // 直接对设备真值：单次分配上限必须是 maxMemoryAllocationSize（没有该项时才退回 SSBO 范围上限）。
    // 这一条不依赖设备上两个上限是否相等，能直接抓住「拿 maxStorageBufferRange 当分配上限」的回归。
    VkPhysicalDeviceMaintenance3Properties m3; memset(&m3, 0, sizeof m3);
    m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
    VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof p2);
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &m3;
    vkGetPhysicalDeviceProperties2(g_dev.physical, &p2);
    uint64_t expect = m3.maxMemoryAllocationSize ? (uint64_t)m3.maxMemoryAllocationSize
                                                 : (uint64_t)g_dev.properties.limits.maxStorageBufferRange;
    CHECK(cp.max_buf_bytes == expect, "C76: caps.max_buf_bytes=%llu 应等于设备 maxMemoryAllocationSize %llu（SSBO 范围上限 %llu 是另一项）",
          (unsigned long long)cp.max_buf_bytes, (unsigned long long)expect, (unsigned long long)cp.max_ssbo_range);
    if (cp.max_buf_bytes <= cp.max_ssbo_range) {
        printf("C76 跳过：本设备 max_buf_bytes(%llu) <= max_ssbo_range(%llu)，没有可判别的区间\n",
               (unsigned long long)cp.max_buf_bytes, (unsigned long long)cp.max_ssbo_range);
        return;
    }
    uint64_t n = cp.max_ssbo_range + (1u << 20);
    if (n > cp.max_buf_bytes) n = cp.max_buf_bytes;
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_buf b = NULL;
    nvn_gpu_rc r = nvn_gpu_buf_create(n, NVN_GPU_MEM_SHARED, NULL, &b, &e);
    CHECK(nvn_gpu_ok(r) && b, "C76: 建 %llu 字节缓冲（> max_ssbo_range %llu，<= 分配上限 %llu）失败：%d %s",
          (unsigned long long)n, (unsigned long long)cp.max_ssbo_range, (unsigned long long)cp.max_buf_bytes, r.code, e.why);
    if (nvn_gpu_ok(r)) printf("C76: 分配上限 %llu > SSBO 范围上限 %llu，建 %llu 字节缓冲成功\n",
                              (unsigned long long)cp.max_buf_bytes, (unsigned long long)cp.max_ssbo_range, (unsigned long long)n);
    if (b) nvn_gpu_buf_release(b, NULL, NULL);
}

// C29：Vulkan 映射按中立表重定向：展开格式必须映射到目标格式的 VkFormat（纯格式检查，不用 GPU）。
static void t29_format_expand(void) {
    for (uint32_t f = 0; f <= 0xFF; f++) {
        nvn_fmt_info i;
        if (!nvn_fmt_info_of(f, &i) || !i.expand) continue;
        VkFormat a = nvn_vk_tex_format(f), b = nvn_vk_tex_format(i.expand);
        CHECK(a == b, "0x%02X 贴图映射 %d != 目标 0x%02X 的 %d", f, (int)a, i.expand, (int)b);
    }
    // 顶点 / 存储映射不能被展开改掉
    CHECK(nvn_vk_format(0x22) == VK_FORMAT_R32G32B32_SFLOAT, "RGB32F 顶点映射被改：%d", (int)nvn_vk_format(0x22));
    CHECK(nvn_vk_format(0x19) == VK_FORMAT_R8G8B8_UNORM, "RGB8 存储映射被改：%d", (int)nvn_vk_format(0x19));
    CHECK(nvn_vk_format(0x51) == VK_FORMAT_R8_USCALED, "R8_UI2F 顶点取样映射不是 USCALED：%d", (int)nvn_vk_format(0x51));
    CHECK(nvn_vk_tex_format(0x51) == VK_FORMAT_R16_SFLOAT, "R8_UI2F 贴图映射不是 R16F：%d", (int)nvn_vk_tex_format(0x51));
    CHECK(nvn_vk_tex_format(0x19) == VK_FORMAT_R8G8B8A8_UNORM, "RGB8 贴图映射不是 RGBA8：%d", (int)nvn_vk_tex_format(0x19));
}

// C28：展开格式的回读明确不支持（不做隐式收缩）。
static void t28_rgb8_upload(void) {
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGB8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    const uint8_t px[12] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    NVNcopyRegion r = {0, 0, 0, 2, 2, 1};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, &r, px, 6, 12);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 8, 16);
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_UNSUPPORTED, "展开格式回读应 E_UNSUPPORTED，实为 %d", rc.code);
    CHECK(nvn_gpu_fence_failed() == 6, "F 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}


// C31：顶点路径证据——RGB32F 位置属性不能被展开成 RGBA32F（w 会读成下一个属性的字节），
// RGBX8 也不能被展开成 RGBA8（多读 1 字节）。两者都画一个实色三角形并回读。
static void t31_vertex_formats(const char *dir) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri3.vert.spv", dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri_solid.frag.spv", dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fs, "读不到顶点格式用例 SPIR-V（%s）", dir);
    if (!vs || !fs) { free(vs); free(fs); return; }

    nvn_gpu_layout lay;
    memset(&lay, 0, sizeof lay);
    nvn_gpu_shader vs_sh = NULL, fs_sh = NULL;
    nvn_gpu_err e;
    nvn_gpu_shader_src ssrc;
    memset(&ssrc, 0, sizeof ssrc);
    ssrc.stage = NVN_GPU_SH_VS; ssrc.kind = NVN_GPU_BLOB_SPIRV; ssrc.entry = "main";
    ssrc.blob = vs; ssrc.bytes = vsz; ssrc.layout = &lay;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &vs_sh, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    ssrc.stage = NVN_GPU_SH_FS; ssrc.blob = fs; ssrc.bytes = fsz;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &fs_sh, &e)), "FS 装载：%d %s", e.rc.code, e.why);

    const float verts32[3][4] = {{-1.0f, -1.0f, 0.0f, 99.0f},
                                 { 3.0f, -1.0f, 0.0f, 99.0f},
                                 {-1.0f,  3.0f, 0.0f, 99.0f}};
    const uint8_t vertsx[3][4] = {{0, 0, 0, 0}, {255, 0, 0, 0}, {0, 255, 0, 0}};
    nvn_gpu_buf vb32 = make_buf(sizeof verts32, NVN_GPU_MEM_SHARED, verts32);
    nvn_gpu_buf vbx = make_buf(sizeof vertsx, NVN_GPU_MEM_SHARED, vertsx);
    uint8_t zero[8 * 8 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_tex out = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);

    for (int which = 0; which < 2; which++) {
        nvn_gpu_pipe_desc pd;
        memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
        pd.st[NVN_GPU_SH_VS].sh = vs_sh;
        pd.st[NVN_GPU_SH_FS].sh = fs_sh;
        pd.cfmt[0] = NVN_FORMAT_RGBA8;
        pd.samples = 1;
        pd.prim = 4;
        pd.chmask.mask = 0xF;
        pd.attr[0].fmt = which ? NVN_FORMAT_RGBX8 : NVN_FORMAT_RGB32F;
        pd.attr[0].offset = 0;
        pd.attr[0].stream = 0;
        pd.stream[0].stride = which ? 4 : 16;
        nvn_gpu_pipe pipe = NULL;
        nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
        CHECK(OK(r), "顶点格式 pipe_get：%d %s", r.code, e.why);
        if (!OK(r)) continue;
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc pass;
        memset(&pass, 0, sizeof pass);
        pass.ncolor = 1;
        pass.color[0].view = ov;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[3] = 1.0f;
        pass.w = 8; pass.h = 8; pass.layers = 1;
        nvn_gpu_pass_begin(c, &pass);
        nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_dyn dyn;
        memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
        dyn.nvp = 1;
        dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 8; dyn.vp[0][3] = 8; dyn.vp[0][5] = 1;
        dyn.has_sc = 1; dyn.sc[2] = 8; dyn.sc[3] = 8;
        nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, which ? vbx : vb32, 0);
        nvn_gpu_draw_args da;
        memset(&da, 0, sizeof da);
        da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 8 * 4, 8 * 8 * 4);
        r = nvn_gpu_cmd_commit(c, which + 1, on_done, NULL, &e);
        CHECK(OK(r), "顶点格式 commit %d：%d %s", which + 1, r.code, e.why);
        CHECK(OK(nvn_gpu_fence_wait(which + 1, 2000000000ull, &e)), "顶点格式 wait %d：%d %s", which + 1, e.rc.code, e.why);
        int red = 0;
        for (int i = 0; i < 8 * 8; i++) {
            const uint8_t *p = buf_cpu(rb) + i * 4;
            if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) red++;
        }
        if (which == 0) CHECK(red == 64, "RGB32F 顶点全屏红像素 %d / 64（w 被读成 pad）", red);
        else CHECK(red > 0, "RGBX8 顶点没有红像素（多读字节 / w 错）");
    }
    nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb32, NULL, NULL);
    nvn_gpu_buf_release(vbx, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs_sh);
    nvn_gpu_shader_release(fs_sh);
    free(vs);
    free(fs);
}


// C32：整级展开的源越界必须 E_ARG（RGB8 4×4 只给 6 字节行宽 / 12 字节总量）。
static void t32_expand_bounds(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGB8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    const uint8_t px[12] = {0};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, px, 6, 12);
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_ARG, "展开源越界应 E_ARG，实为 %d", rc.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_tex_release(t);
}


// ---- C33–C35 公共绘制助手：只在已开始的通道里画，不管理 pass ----
static int load_shader_pair(const char *dir, const char *vs_name, const char *fs_name,
                            const nvn_gpu_layout *vlay, const nvn_gpu_layout *flay,
                            nvn_gpu_shader *vs, nvn_gpu_shader *fs, void **vsmem, void **fsmem) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/%s.spv", dir, vs_name);
    *vsmem = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/%s.spv", dir, fs_name);
    *fsmem = read_file(path, &fsz);
    if (!*vsmem || !*fsmem) return 0;
    nvn_gpu_layout empty;
    memset(&empty, 0, sizeof empty);
    nvn_gpu_err e;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.stage = NVN_GPU_SH_VS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = *vsmem; src.bytes = vsz; src.layout = vlay ? vlay : &empty;
    if (!OK(nvn_gpu_shader_load(&src, vs, &e))) return 0;
    src.stage = NVN_GPU_SH_FS; src.blob = *fsmem; src.bytes = fsz; src.layout = flay ? flay : &empty;
    if (!OK(nvn_gpu_shader_load(&src, fs, &e))) return 0;
    return 1;
}

// GS 专属：同一图形管线的三个阶段各读自己的 UBO；输出两张 4x4 手算图。
static void t_gs_pixels(const char *dir) {
    static const char *names[3] = {"gs_probe.vert", "gs_probe.frag", "gs_probe.geom"};
    nvn_gpu_shader sh[3] = {0}; void *mem[3] = {0};
    nvn_gpu_layout_ent ent[3]; nvn_gpu_layout layout[3]; nvn_gpu_err e = {0};
    memset(ent, 0, sizeof ent); memset(layout, 0, sizeof layout);
    for (int st = 0; st < 3; st++) {
        char path[512]; size_t len = 0; snprintf(path, sizeof path, "%s/%s.spv", dir, names[st]); mem[st] = read_file(path, &len);
        CHECK(mem[st] != NULL, "GS fixture shader %d exists", st); if (!mem[st]) goto cleanup;
        ent[st].set = 0; ent[st].binding = st == 0 ? 0 : st == 1 ? 2 : 1;
        ent[st].cls = NVN_GPU_RES_UBO; ent[st].access = NVN_GPU_ACC_R; ent[st].count = 1; ent[st].stages = 1u << st;
        layout[st].ent = &ent[st]; layout[st].nent = 1;
        nvn_gpu_shader_src src = {0}; src.stage = (nvn_gpu_stage)st; src.kind = NVN_GPU_BLOB_SPIRV;
        src.blob = mem[st]; src.bytes = len; src.entry = "main"; src.layout = &layout[st];
        nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh[st], &e);
        CHECK(OK(r), "GS fixture stage %d load: %d %s", st, r.code, e.why); if (!OK(r)) goto cleanup;
    }
    nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3;
    for (int st = 0; st < 3; st++) pd.st[st].sh = sh[st];
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 0; pd.chmask.mask = 15;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "GS point to triangle pipeline: %d %s", r.code, e.why); if (!OK(r)) goto cleanup;
    const float values[4][4] = {{0,.25f,0,0}, {0,.5f,0,1}, {0,.25f,0,0}, {0,0,.25f,0}};
    nvn_gpu_buf ub[4] = {0}; for (int i = 0; i < 4; i++) ub[i] = make_buf(16, NVN_GPU_MEM_SHARED, values[i]);
    nvn_gpu_tex tex = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex); nvn_gpu_buf rb = make_buf(128, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd();
    for (int passno = 0; passno < 2; passno++) {
        nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = view;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.w = 4; pass.h = 4; pass.layers = 1; nvn_gpu_pass_begin(cmd, &pass);
        nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1;
        dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4; nvn_gpu_set_dyn(cmd, &dyn);
        for (int st = 0; st < 3; st++) { nvn_gpu_binding b = {0}; b.cls = NVN_GPU_RES_UBO; b.u.b.buf = ub[st]; b.u.b.len = 16; nvn_gpu_bind(cmd, (nvn_gpu_stage)st, &b, 1); }
        nvn_gpu_draw_args draw = {0}; draw.prim = 0; draw.count = draw.instances = 1; nvn_gpu_draw(cmd, &draw);
        if (passno) {
            nvn_gpu_binding b = {0}; b.cls = NVN_GPU_RES_UBO; b.u.b.buf = ub[3]; b.u.b.len = 16;
            nvn_gpu_bind(cmd, NVN_GPU_SH_GS, &b, 1); dyn.sc[0] = 2; dyn.sc[2] = 2; nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_draw(cmd, &draw);
        }
        nvn_gpu_pass_end(cmd); nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, passno * 64, 16, 64);
    }
    r = nvn_gpu_cmd_commit(cmd, 1, on_done, NULL, &e); CHECK(OK(r), "GS commit %d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(r), "GS wait %d %s", r.code, e.why);
    for (int passno = 0; passno < 2; passno++) for (int i = 0; i < 16; i++) {
        const uint8_t *v = buf_cpu(rb) + passno * 64 + i * 4;
        const uint8_t green[4] = {0,255,0,255}, changed[4] = {0,191,64,255};
        const uint8_t *want = passno && i % 4 >= 2 ? changed : green;
        CHECK(!memcmp(v, want, 4), "GS pass %d pixel %d got %u,%u,%u,%u want %u,%u,%u,%u", passno, i, v[0],v[1],v[2],v[3],want[0],want[1],want[2],want[3]);
    }
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    for (int i = 0; i < 4; i++) nvn_gpu_buf_release(ub[i], NULL, NULL);
cleanup:
    for (int st = 0; st < 3; st++) { if (sh[st]) nvn_gpu_shader_release(sh[st]); free(mem[st]); }
}

// #696 分段验证（后端 + 真 GS）：回放层把「GS + 不带索引的三角带」按 GL 序展开成三角形列表交给后端。
// 后端契约本来就不收 GS 配三角带（建管线回 UNSUPPORTED），这里先核这一条；再在 lavapipe 上用同一个 GS 画两份列表，base vertex 7、5 个顶点：
//   ① GL 序索引 {0,1,2, 2,1,3, 2,3,4}（回放层对 5 个顶点展开的结果，期望同 t_nvn_replay）；
//   ② Vulkan 原生三角带的顶点序 {i, i+(1+i%2), i+(2-i%2)} 写成的索引 {0,1,2, 1,3,2, 2,3,4}，即原生画时 GS 会拿到的顺序。
// GS 把 gl_in[0..2] 的顶点号与 gl_PrimitiveIDIn 写进 4x1 目标的第 gl_PrimitiveIDIn 列。期望按规范手算：
//   GL（§10.1.6）偶 i (i, i+1, i+2)、奇 i (i+1, i, i+2) → 列 0..2 = (7,8,9,0) (9,8,10,1) (9,10,11,2)；Vulkan 序第 1 列是 (8,10,9,1)。第 3 列没画，清成 0。
static void t_gs_tstrip_pixels(const char *dir) {
    static const char *names[3] = {"gs_tstrip.vert", "gs_tstrip.frag", "gs_tstrip.geom"};
    nvn_gpu_shader sh[3] = {0}; void *mem[3] = {0}; nvn_gpu_err e = {0};
    nvn_gpu_layout empty; memset(&empty, 0, sizeof empty);
    nvn_gpu_pipe pipe = NULL;
    for (int st = 0; st < 3; st++) {
        char path[512]; size_t len = 0; snprintf(path, sizeof path, "%s/%s.spv", dir, names[st]); mem[st] = read_file(path, &len);
        CHECK(mem[st] != NULL, "GS 三角带着色器 %d 存在", st); if (!mem[st]) goto cleanup;
        nvn_gpu_shader_src src = {0}; src.stage = (nvn_gpu_stage)st; src.kind = NVN_GPU_BLOB_SPIRV;
        src.blob = mem[st]; src.bytes = len; src.entry = "main"; src.layout = &empty;
        nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh[st], &e);
        CHECK(OK(r), "GS 三角带着色器 %d 载入：%d %s", st, r.code, e.why); if (!OK(r)) goto cleanup;
    }
    {
        nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3;
        for (int st = 0; st < 3; st++) pd.st[st].sh = sh[st];
        pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 5; pd.chmask.mask = 15;
        nvn_gpu_pipe strip = NULL; memset(&e, 0, sizeof e);
        nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &strip, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !strip, "GS 三角带 原生三角带配 GS：期望后端拒建 UNSUPPORTED → rc=%d why=%s", r.code, e.why);
        printf("GS 三角带 原生三角带配 GS 建管线 rc=%d（%s）\n", r.code, e.why);
        pd.prim = 4;
        r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
        CHECK(OK(r), "GS 三角带 列表管线：%d %s", r.code, e.why); if (!OK(r)) goto cleanup;
    }
    {
        static const uint32_t idx[2][9] = {{0, 1, 2, 2, 1, 3, 2, 3, 4}, {0, 1, 2, 1, 3, 2, 2, 3, 4}};
        nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
        nvn_gpu_tex tex = make_tex(4, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
        nvn_gpu_view view = make_view(tex); nvn_gpu_buf rb = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_cmd cmd = begin_cmd();
        for (int k = 0; k < 2; k++) {
            nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = view;
            pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
            pass.w = 4; pass.h = 1; pass.layers = 1; nvn_gpu_pass_begin(cmd, &pass);
            nvn_gpu_set_pipe(cmd, pipe);
            nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = 4; dyn.vp[0][3] = 1; dyn.vp[0][5] = 1;
            dyn.has_sc = 1; dyn.sc[2] = 4; dyn.sc[3] = 1; nvn_gpu_set_dyn(cmd, &dyn);
            nvn_gpu_draw_args draw = {0}; draw.instances = 1;
            draw.prim = 4; draw.count = 9; draw.ibuf = ib; draw.ioff = (uint64_t)k * sizeof idx[0]; draw.itype = 2; draw.base_vertex = 7;
            nvn_gpu_draw(cmd, &draw);
            nvn_gpu_pass_end(cmd); nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, (uint64_t)k * 16, 16, 16);
        }
        nvn_gpu_rc r = nvn_gpu_cmd_commit(cmd, 2, on_done, NULL, &e); CHECK(OK(r), "GS 三角带 commit %d %s", r.code, e.why);   // 同一会话里 t_gs_pixels 用了围栏 1
        r = nvn_gpu_fence_wait(2, 5000000000ull, &e); CHECK(OK(r), "GS 三角带 wait %d %s", r.code, e.why);
        static const uint8_t want[2][4][4] = {{{7, 8, 9, 0}, {9, 8, 10, 1}, {9, 10, 11, 2}, {0, 0, 0, 0}},
                                              {{7, 8, 9, 0}, {8, 10, 9, 1}, {9, 10, 11, 2}, {0, 0, 0, 0}}};
        static const char *what[2] = {"列表（GL 序展开）", "列表（Vulkan 原生三角带序）"};
        for (int k = 0; k < 2; k++) {
            const uint8_t *v = buf_cpu(rb) + k * 16;
            for (int i = 0; i < 4; i++)
                CHECK(!memcmp(v + i * 4, want[k][i], 4), "GS 三角带 %s 第 %d 列：期望 (%u,%u,%u,%u) → 读到 (%u,%u,%u,%u)", what[k], i,
                      want[k][i][0], want[k][i][1], want[k][i][2], want[k][i][3], v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]);
            printf("GS 三角带 %s：列 0..2 读到 (%u,%u,%u,%u) (%u,%u,%u,%u) (%u,%u,%u,%u)\n", what[k], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
        }
        nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex); nvn_gpu_buf_release(ib, NULL, NULL);
    }
cleanup:
    for (int st = 0; st < 3; st++) { if (sh[st]) nvn_gpu_shader_release(sh[st]); free(mem[st]); }
}

// 使用生产请求经真实编译器和生产 parser 导出的同一组 Maxwell 模块。
static void t_gs_compiler_pixels(const char *dir) {
    const char *names[3] = {"vs", "fs", "gs"}; nvn_gpu_shader sh[3] = {0}; void *mem[3] = {0};
    nvn_gpu_layout_ent ent = {0}; ent.set = 0; ent.binding = 0; ent.cls = NVN_GPU_RES_UBO;
    ent.access = NVN_GPU_ACC_R; ent.count = 1; ent.stages = 7; ent.src_kind = NVN_RB_SUPPORT;
    nvn_gpu_layout layout = {0}; layout.ent = &ent; layout.nent = 1; layout.support_bytes = 2608;
    nvn_gpu_err e = {0};
    for (int st = 0; st < 3; st++) {
        char path[512]; size_t len = 0; snprintf(path, sizeof path, "%s/%s.spv", dir, names[st]); mem[st] = read_file(path, &len);
        CHECK(mem[st] != NULL, "compiled Maxwell %s module exists", names[st]); if (!mem[st]) goto cleanup;
        nvn_gpu_shader_src src = {0}; src.stage = (nvn_gpu_stage)st; src.kind = NVN_GPU_BLOB_SPIRV;
        src.blob = mem[st]; src.bytes = len; src.entry = "main"; src.layout = &layout;
        nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh[st], &e);
        CHECK(OK(r), "compiled Maxwell %s load: %d %s", names[st], r.code, e.why); if (!OK(r)) goto cleanup;
    }
    nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3;
    for (int st = 0; st < 3; st++) pd.st[st].sh = sh[st];
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 0; pd.chmask.mask = 15;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "compiled Maxwell GS pipeline: %d %s", r.code, e.why); if (!OK(r)) goto cleanup;
    uint8_t support[2608] = {0}; uint32_t flags = 3; memcpy(support + 1392, &flags, 4);
    float one = 1; memcpy(support + 1408, &one, 4);
    for (unsigned i = 0; i < 73; i++) { memcpy(support + 192 + i * 16, &one, 4); memcpy(support + 1440 + i * 16, &one, 4); }
    nvn_gpu_buf ub = make_buf(sizeof support, NVN_GPU_MEM_SHARED, support);
    nvn_gpu_tex tex = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex); nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd(); nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE; pass.w = 4; pass.h = 4; pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass); nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4; nvn_gpu_set_dyn(cmd, &dyn);
    for (int st = 0; st < 3; st++) { nvn_gpu_binding b = {0}; b.cls = NVN_GPU_RES_UBO; b.u.b.buf = ub; b.u.b.len = sizeof support; nvn_gpu_bind(cmd, (nvn_gpu_stage)st, &b, 1); }
    nvn_gpu_draw_args draw = {0}; draw.prim = 0; draw.count = draw.instances = 1; nvn_gpu_draw(cmd, &draw);
    nvn_gpu_pass_end(cmd); nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16, 64);
    r = nvn_gpu_cmd_commit(cmd, 1, on_done, NULL, &e); CHECK(OK(r), "Maxwell GS commit %d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(r), "Maxwell GS wait %d %s", r.code, e.why);
    for (int i = 0; i < 16; i++) { const uint8_t green[4] = {0,255,0,255}; const uint8_t *v = buf_cpu(rb) + i * 4;
        CHECK(!memcmp(v, green, 4), "Maxwell GS pixel %d got %u,%u,%u,%u expected 0,255,0,255", i, v[0],v[1],v[2],v[3]); }
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex); nvn_gpu_buf_release(ub, NULL, NULL);
cleanup:
    for (int st = 0; st < 3; st++) { if (sh[st]) nvn_gpu_shader_release(sh[st]); free(mem[st]); }
}

static void fs_draw_tex_frag_depth(nvn_gpu_cmd c, nvn_gpu_view view, nvn_gpu_samp samp, const char *dir, const char *frag, int test_depth) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl;
    memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "tri.vert", frag, &vl, &fl, &vs, &fs, &vsm, &fsm)) { CHECK(0, "读不到 %s 着色器", frag); return; }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    if (test_depth) {
        pd.dfmt=NVN_FORMAT_DEPTH32F; pd.ds.dt=1; pd.ds.df=5;
        pd.depth_clamp=1;
    }
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C33 pipe：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1, -1, 0, 0}, {3, -1, 2, 0}, {-1, 3, 0, 2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.depth_clamp=(uint8_t)test_depth;
    dyn.nvp = 1;
    dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2];
    memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = view;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vsm); free(fsm);
}

static void fs_draw_tex_frag(nvn_gpu_cmd c, nvn_gpu_view v, nvn_gpu_samp sampler, const char *dir, const char *frag) {
    fs_draw_tex_frag_depth(c,v,sampler,dir,frag,0);
}

static void fs_draw_solid(nvn_gpu_cmd c, const char *dir, const char *frag, const int sc[4]) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "tri3.vert", frag, NULL, NULL, &vs, &fs, &vsm, &fsm)) { CHECK(0, "读不到 %s 着色器", frag); return; }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C34 pipe：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1;
    memcpy(dyn.sc, sc, sizeof dyn.sc);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vsm); free(fsm);
}

static void expect_rect(const uint8_t *rb, int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b, const char *what) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            const uint8_t *p = rb + (y * 64 + x) * 4;
            if (p[0] != r || p[1] != g || p[2] != b || p[3] != 255) {
                CHECK(0, "%s 像素 (%d,%d)=%02x%02x%02x%02x，期望 %02x%02x%02xff", what, x, y, p[0], p[1], p[2], p[3], r, g, b);
                return;
            }
        }
    }
}


// 非恒等采样映射的图仍能做附件：先按原始颜色清两次（第二次 LOAD），
// 再从同一采样视图读进另一图；原始读回不换通道，采样读回按映射换通道。
// 各变体独立会话：修前 sticky 会令该会话的时间线失败，不能污染下一用例。
static atomic_int g_attach_done;
static char g_attach_why[192];
static void attach_swizzle_done(void *user, const nvn_gpu_done *done) {
    snprintf(g_attach_why, sizeof g_attach_why, "%s", done->why ? done->why : "");
    on_done(user, done);
    atomic_store_explicit(&g_attach_done, 1, memory_order_release);
}
static void attach_swizzle_case(const char *dir, int explicit_view) {
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 64; td.d.height = 64; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    td.swz = 4 | 3 << 3 | 2 << 6 | 1 << 9; // B,G,R,1
    nvn_gpu_tex src = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &src, &e);
    CHECK(OK(r), "attachment swizzle tex_create: %d %s", r.code, e.why);
    if (!OK(r)) return;
    view_t vd; memset(&vd, 0, sizeof vd);
    if (explicit_view) { vd.set_swizzle = 1; vd.swz[0] = 0; vd.swz[1] = 2; vd.swz[2] = 5; vd.swz[3] = 3; }
    nvn_gpu_view src_view = NULL;
    r = nvn_gpu_view_create(src, explicit_view ? &vd : NULL, &src_view, &e);
    CHECK(OK(r), "attachment swizzle view_create: %d %s", r.code, e.why);
    if (!OK(r)) { nvn_gpu_tex_release(src); return; }
    nvn_gpu_tex dst = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view dst_view = make_view(dst);
    uint8_t zero[64 * 64 * 4] = {0};
    nvn_gpu_buf raw = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf sampled = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_gpu_samp sampler = NULL;
    r = nvn_gpu_samp_create(&sp, &sampler, &e);
    CHECK(OK(r), "attachment swizzle sampler: %d %s", r.code, e.why);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = src_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = 0.2f; pass.color[0].clear.f[1] = 0.4f;
    pass.color[0].clear.f[2] = 0.6f; pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass); nvn_gpu_pass_end(c);
    pass.color[0].load = NVN_GPU_LOAD_LOAD;  // 第二次消费同一附件
    nvn_gpu_pass_begin(c, &pass); nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, src, 0, 0, NULL, raw, 0, 64 * 4, 64 * 64 * 4);
    memset(&pass, 0, sizeof pass); pass.ncolor = 1; pass.color[0].view = dst_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    fs_draw_tex_frag(c, src_view, sampler, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, sampled, 0, 64 * 4, 64 * 64 * 4);
    // 录制后立即 release：段完成前不得销毁附件或采样视图。
    nvn_gpu_view_release(src_view); nvn_gpu_tex_release(src);
    nvn_gpu_view_release(dst_view); nvn_gpu_tex_release(dst); nvn_gpu_samp_release(sampler);
    atomic_store(&g_attach_done, 0); g_attach_why[0] = 0;
    r = nvn_gpu_cmd_commit(c, 1, attach_swizzle_done, NULL, &e);
    if (!OK(r)) CHECK(wait_until(&g_attach_done, 1, 5000), "attachment swizzle failure callback missing");
    CHECK(OK(r), "attachment swizzle %s commit: %d %s", explicit_view ? "explicit" : "inherited", r.code, g_attach_why);
    if (OK(r)) {
        r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
        CHECK(OK(r), "attachment swizzle wait: %d %s", r.code, e.why);
        const uint8_t want_raw[4] = {51, 102, 153, 255};
        const uint8_t want_inherited[4] = {153, 102, 51, 255};
        const uint8_t want_explicit[4] = {0, 51, 255, 102};
        const uint8_t *want_sample = explicit_view ? want_explicit : want_inherited;
        const uint8_t *rawp = buf_cpu(raw), *samplep = buf_cpu(sampled);
        for (int i = 0; i < 64 * 64; i++) {
            if (memcmp(rawp + i * 4, want_raw, 4)) { CHECK(0, "attachment raw pixel %d differs", i); break; }
            if (memcmp(samplep + i * 4, want_sample, 4)) { CHECK(0, "attachment sampled pixel %d differs", i); break; }
        }
    }
    nvn_gpu_buf_release(raw, NULL, NULL); nvn_gpu_buf_release(sampled, NULL, NULL);
}

// 9000 的建视图崩溃参数：12 层 cube-array 图、只取第 0 层和第 0 级。
// 修前 vkCreateImageView 收到 CUBE_ARRAY + 1 层，VVL 报 viewType-02961。
static void cube_array_one_face(void) {
    CHECK(g_dev.core_features.imageCubeArray, "cube-array probe requires imageCubeArray");
    if (!g_dev.core_features.imageCubeArray) return;
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    td.d.format = NVN_FORMAT_R11G11B10F; td.d.width = 128; td.d.height = 128;
    td.d.depth = 12; td.d.levels = 8;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT;
    td.swz = 2 | 3 << 3 | 4 << 6 | 1 << 9; // R,G,B,ONE
    nvn_gpu_tex t = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "cube-array tex: %d %s", r.code, e.why);
    if (!OK(r)) return;
    const int layers[] = {1, 6, 12};
    for (size_t i = 0; i < sizeof layers / sizeof layers[0]; i++) {
        view_t vd = {0}; vd.set_levels = 1; vd.base_level = 0; vd.nlevels = 1;
        vd.set_layers = 1; vd.min_layer = 0; vd.nlayers = (uint16_t)layers[i];
        nvn_gpu_view v = NULL;
        r = nvn_gpu_view_create(t, &vd, &v, &e);
        CHECK(OK(r), "cube-array view %d layers: %d %s", layers[i], r.code, e.why);
        if (v) nvn_gpu_view_release(v);
    }
    nvn_gpu_tex_release(t);
}

// 数组第 1 层、多 mip 视图与 sRGB 重解释均保留精确的附件子资源与格式。
static void attach_swizzle_subresource_case(const char *dir, int srgb) {
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 128; td.d.height = 128; td.d.depth = 2; td.d.levels = 2;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT;
    td.swz = 4 | 3 << 3 | 2 << 6 | 1 << 9;
    nvn_gpu_tex src = NULL; nvn_gpu_view view = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &src, &e);
    CHECK(OK(r), "attachment subresource tex: %d %s", r.code, e.why);
    if (!OK(r)) return;
    view_t vd; memset(&vd, 0, sizeof vd);
    vd.set_layers = 1; vd.min_layer = 1; vd.nlayers = 1;
    vd.set_target = 1; vd.target = NVN_TEXTURE_TARGET_2D;
    if (srgb) { vd.set_levels = 1; vd.base_level = 1; vd.nlevels = 1;
                vd.set_format = 1; vd.format = NVN_FORMAT_RGBA8_SRGB; }
    r = nvn_gpu_view_create(src, &vd, &view, &e);
    CHECK(OK(r), "attachment subresource view: %d %s", r.code, e.why);
    if (!OK(r)) { nvn_gpu_tex_release(src); return; }
    uint8_t zero[64 * 64 * 4] = {0};
    nvn_gpu_buf raw = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf sampled = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_tex dst = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view dst_view = make_view(dst);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_gpu_samp sampler = NULL;
    r = nvn_gpu_samp_create(&sp, &sampler, &e);
    CHECK(OK(r), "attachment subresource sampler: %d %s", r.code, e.why);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = view; pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = srgb ? 0.25f : 0.2f;
    pass.color[0].clear.f[1] = srgb ? 0.5f : 0.4f;
    pass.color[0].clear.f[2] = srgb ? 0.75f : 0.6f;
    pass.color[0].clear.f[3] = 1.0f; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass); nvn_gpu_pass_end(c);
    NVNcopyRegion region = {0, 0, 0, 64, 64, 1};
    nvn_gpu_copy_tex_buf(c, src, srgb ? 1 : 0, 1, &region, raw, 0, 64 * 4, 64 * 64 * 4);
    if (srgb) {
        memset(&pass, 0, sizeof pass); pass.ncolor = 1; pass.color[0].view = dst_view;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.w = 64; pass.h = 64; pass.layers = 1;
        nvn_gpu_pass_begin(c, &pass); fs_draw_tex_frag(c, view, sampler, dir, "tri.frag"); nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, sampled, 0, 64 * 4, 64 * 64 * 4);
    }
    nvn_gpu_view_release(view); nvn_gpu_tex_release(src);
    nvn_gpu_view_release(dst_view); nvn_gpu_tex_release(dst); nvn_gpu_samp_release(sampler);
    atomic_store(&g_attach_done, 0); g_attach_why[0] = 0;
    r = nvn_gpu_cmd_commit(c, 1, attach_swizzle_done, NULL, &e);
    if (!OK(r)) CHECK(wait_until(&g_attach_done, 1, 5000), "attachment subresource callback missing");
    CHECK(OK(r), "attachment subresource %s commit: %d %s", srgb ? "sRGB" : "multi-mip", r.code, g_attach_why);
    if (OK(r)) {
        r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
        CHECK(OK(r), "attachment subresource wait: %d %s", r.code, e.why);
        const uint8_t rgba_linear[4] = {51, 102, 153, 255};
        const uint8_t rgba_srgb[4] = {137, 188, 225, 255};
        const uint8_t sample_srgb[4] = {191, 128, 64, 255};
        const uint8_t *got = buf_cpu(raw), *tex = buf_cpu(sampled);
        for (int i = 0; i < 64 * 64; i++) {
            for (int ch = 0; ch < 4; ch++) {
                int want = srgb ? rgba_srgb[ch] : rgba_linear[ch];
                if (abs((int)got[i * 4 + ch] - want) > (srgb ? 1 : 0)) { CHECK(0, "attachment subresource raw pixel %d channel %d: %u vs %d", i, ch, got[i * 4 + ch], want); goto pixels_done; }
                if (srgb && abs((int)tex[i * 4 + ch] - sample_srgb[ch]) > 1) { CHECK(0, "attachment subresource sample pixel %d channel %d: %u vs %u", i, ch, tex[i * 4 + ch], sample_srgb[ch]); goto pixels_done; }
            }
        }
    pixels_done: ;
    }
    nvn_gpu_buf_release(raw, NULL, NULL); nvn_gpu_buf_release(sampled, NULL, NULL);
}

// C33：一个 cmd 两个通道；通道 1 清 T 红，通道 2 采样 T 画进 U，U 读回全红。
static void t33_two_passes(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex U = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view vT = make_view(T), vU = make_view(U);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C33 samp：%d %s", e.rc.code, e.why);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vU; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vT, samp, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, U, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "C33 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C33 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 255, 0, 0, "C33 U");
    nvn_gpu_view_release(vT); nvn_gpu_view_release(vU);
    nvn_gpu_tex_release(T); nvn_gpu_tex_release(U);
    nvn_gpu_samp_release(samp); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C34：同一目标两通道；通道 1 左半绿，通道 2 Load 右半蓝。
static void t34_two_pass_load(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view vT = make_view(T);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    const int left[4] = {0, 0, 32, 64};
    fs_draw_solid(c, dir, "solid_green.frag", left);
    nvn_gpu_pass_end(c);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    const int right[4] = {32, 0, 32, 64};
    fs_draw_solid(c, dir, "solid_blue.frag", right);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "C34 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(2, 5000000000ull, &e)), "C34 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C34 左半");
    expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C34 右半");
    nvn_gpu_view_release(vT); nvn_gpu_tex_release(T); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C35：三通道链；每通道后各自回读：T 红 → U 采样 T 红 → T 采样 U 反色成青。
static void t35_three_passes(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex U = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view vT = make_view(T), vU = make_view(U);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C35 samp：%d %s", e.rc.code, e.why);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf r1 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf r2 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf r3 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, r1, 0, 64 * 4, 64 * 64 * 4);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vU; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vT, samp, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, U, 0, 0, NULL, r2, 0, 64 * 4, 64 * 64 * 4);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vU, samp, dir, "tri_invert.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, r3, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "C35 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(3, 5000000000ull, &e)), "C35 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(r1), 0, 0, 64, 64, 255, 0, 0, "C35 通道1 T");
    expect_rect(buf_cpu(r2), 0, 0, 64, 64, 255, 0, 0, "C35 通道2 U");
    expect_rect(buf_cpu(r3), 0, 0, 64, 64, 0, 255, 255, "C35 通道3 T反色");
    nvn_gpu_view_release(vT); nvn_gpu_view_release(vU);
    nvn_gpu_tex_release(T); nvn_gpu_tex_release(U);
    nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(r1, NULL, NULL); nvn_gpu_buf_release(r2, NULL, NULL); nvn_gpu_buf_release(r3, NULL, NULL);
}


// ---- C36–C40：索引 / 实例化 / 间接绘制 ----
static nvn_gpu_pipe color_pipe(const char *dir, int inst) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, inst ? "inst.vert" : "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return NULL;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C36 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return pipe;
}

static void run_quad(const char *dir, int inst, const void *vb, size_t vbytes,
                     nvn_gpu_buf ib, uint64_t ioff, uint32_t itype, uint32_t count, uint32_t first,
                     int32_t base_vertex, uint32_t instances, uint32_t base_instance,
                     nvn_gpu_buf ind, uint64_t ind_off, uint32_t max_draws, uint32_t stride,
                     uint8_t *rb, uint64_t sig) {
    nvn_gpu_pipe pipe = color_pipe(dir, inst);
    if (!pipe) return;
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf vbuf = make_buf(vbytes, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rbuf = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, rb);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vbuf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = count; a.first = first; a.instances = instances; a.base_instance = base_instance;
    a.ibuf = ib; a.ioff = ioff; a.itype = itype; a.base_vertex = base_vertex;
    a.ind = ind; a.ind_off = ind_off; a.max_draws = max_draws; a.stride = stride;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rbuf, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(r), "C36 commit %llu：%d %s", (unsigned long long)sig, r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "C36 wait %llu：%d %s", (unsigned long long)sig, e.rc.code, e.why);
    memcpy(rb, buf_cpu(rbuf), 64 * 64 * 4);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vbuf, NULL, NULL); nvn_gpu_buf_release(rbuf, NULL, NULL);
}

static int same_img(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 64 * 64 * 4) == 0; }
static const float quad4[4][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1},
    { 1, -1, 0, 1, 0, 1, 0, 1}, { 1, 1, 0, 1, 0, 1, 0, 1}};
static const float blue4[4][8] = {
    {-1, -1, 0, 1, 0, 0, 1, 1}, {-1, 1, 0, 1, 0, 0, 1, 1},
    { 1, -1, 0, 1, 0, 0, 1, 1}, { 1, 1, 0, 1, 0, 0, 1, 1}};
static const float left2[4][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1},
    { 0.2f, -1, 0, 1, 1, 0, 0, 1}, { 0.2f, 1, 0, 1, 1, 0, 0, 1}};
static const float six6[6][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 1, 0, 1},
    { 1, -1, 0, 1, 0, 1, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1}, {1, 1, 0, 1, 0, 1, 0, 1}};

static void c36_40(const char *dir) {
    static const uint16_t idx16[6] = {0, 1, 2, 2, 1, 3};
    static const uint16_t idxneg[10] = {0, 1, 2, 2, 1, 3, 0, 0, 0, 0};
    static const uint8_t idx8[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t ind16[5] = {6, 1, 0, 0, 0};
    static const uint32_t ind6[4] = {6, 1, 0, 0};
    nvn_gpu_buf ib16 = make_buf(sizeof idx16, NVN_GPU_MEM_SHARED, idx16);
    nvn_gpu_buf ibneg = make_buf(sizeof idxneg, NVN_GPU_MEM_SHARED, idxneg);
    nvn_gpu_buf ib8 = make_buf(sizeof idx8, NVN_GPU_MEM_SHARED, idx8);
    nvn_gpu_buf ibi = make_buf(sizeof ind16, NVN_GPU_MEM_SHARED, ind16);
    nvn_gpu_buf ini = make_buf(sizeof ind6, NVN_GPU_MEM_SHARED, ind6);
    uint8_t rb[64 * 64 * 4], ref[64 * 64 * 4], other[64 * 64 * 4];
    memset(rb, 0, sizeof rb);
    run_quad(dir, 0, quad4, sizeof quad4, ib16, 0, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, ref, 1);
    expect_rect(ref, 0, 0, 32, 64, 255, 0, 0, "C36 左半");
    expect_rect(ref, 32, 0, 32, 64, 0, 255, 0, "C36 右半");
    // C36 负控：ioff 偏一个 u16
    run_quad(dir, 0, quad4, sizeof quad4, ibneg, 2, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, other, 2);
    CHECK(!same_img(ref, other), "C36 负控：ioff=2 仍与期望相同（用例没在看索引）");
    // C37 base_vertex
    float vb8[8][8];
    memcpy(vb8, blue4, sizeof blue4);
    memcpy(vb8 + 4, quad4, sizeof quad4);
    run_quad(dir, 0, vb8, sizeof vb8, ib16, 0, 1, 6, 0, 4, 1, 0, NULL, 0, 0, 0, rb, 3);
    CHECK(same_img(ref, rb), "C37 base_vertex=4 与 C36 不同");
    run_quad(dir, 0, vb8, sizeof vb8, ib16, 0, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, other, 4);
    expect_rect(other, 0, 0, 64, 64, 0, 0, 255, "C37 base_vertex=0");
    // C37 负控：用 first=4 代替 base_vertex
    run_quad(dir, 0, vb8, sizeof vb8, ibneg, 0, 1, 6, 4, 0, 1, 0, NULL, 0, 0, 0, other, 5);
    CHECK(!same_img(ref, other), "C37 负控：first=4 仍与 base_vertex=4 相同（语义没区分）");
    // C38 u8：native 与强制展开
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    run_quad(dir, 0, quad4, sizeof quad4, ib8, 0, 0, 6, 0, 0, 1, 0, NULL, 0, 0, 0, rb, 6);
    CHECK(same_img(ref, rb), "C38 u8 原生与 C36 不同");
    setenv("S2A_VK_FORCE_INDEX_U8_EXPAND", "1", 1);
    run_quad(dir, 0, quad4, sizeof quad4, ib8, 0, 0, 6, 0, 0, 1, 0, NULL, 0, 0, 0, rb, 7);
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    CHECK(same_img(ref, rb), "C38 u8 展开与 C36 不同");
    // C39 实例化
    run_quad(dir, 1, left2, sizeof left2, ib16, 0, 1, 6, 0, 0, 2, 0, NULL, 0, 0, 0, rb, 8);
    expect_rect(rb, 0, 0, 64, 64, 255, 0, 0, "C39 两实例");
    run_quad(dir, 1, left2, sizeof left2, ib16, 0, 1, 6, 0, 0, 1, 1, NULL, 0, 0, 0, rb, 9);
    expect_rect(rb, 0, 0, 32, 64, 0, 0, 0, "C39 base_instance=1 左半清屏");
    expect_rect(rb, 32, 0, 32, 64, 255, 0, 0, "C39 base_instance=1 右半");
    // C40 间接
    run_quad(dir, 0, quad4, sizeof quad4, ib16, 0, 1, 6, 0, 0, 1, 0, ibi, 0, 1, 20, rb, 10);
    CHECK(same_img(ref, rb), "C40 索引间接与 C36 不同");
    run_quad(dir, 0, six6, sizeof six6, NULL, 0, 0, 6, 0, 0, 1, 0, ini, 0, 1, 16, rb, 11);
    CHECK(same_img(ref, rb), "C40 非索引间接与 C36 不同");
    nvn_gpu_buf_release(ib16, NULL, NULL); nvn_gpu_buf_release(ibneg, NULL, NULL); nvn_gpu_buf_release(ib8, NULL, NULL);
    nvn_gpu_buf_release(ibi, NULL, NULL); nvn_gpu_buf_release(ini, NULL, NULL);
}


// C41/C42：索引越界与 u8 间接的负例（各自独立会话，失败即终止）。
static void c41_bounds_neg(const char *dir) {
    static const uint8_t idx[6] = {0, 1, 2, 2, 1, 3};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, color_pipe(dir, 0));
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 6; a.ibuf = ib; a.ioff = 6; a.itype = 0;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C41 越界 u8 应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out); nvn_gpu_buf_release(ib, NULL, NULL);
}

static void c42_u8_indirect_neg(const char *dir) {
    static const uint8_t idx[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t cmd[5] = {6, 1, 0, 0, 0};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_buf ind = make_buf(sizeof cmd, NVN_GPU_MEM_SHARED, cmd);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    setenv("S2A_VK_FORCE_INDEX_U8_EXPAND", "1", 1);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, color_pipe(dir, 0));
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 6; a.ibuf = ib; a.itype = 0; a.ind = ind; a.max_draws = 1; a.stride = 20;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C42 u8 间接应 UNSUPPORTED，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(ind, NULL, NULL);
}


static nvn_gpu_pipe blend_pipe(const char *dir, const blend_st *b, int en, uint32_t mask) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return NULL;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = mask;
    pd.color.blend_en = en ? 1 : 0;
    if (b) pd.blend[0] = *b;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "blend pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return pipe;
}

static void blend_draw(nvn_gpu_pipe pipe, const float clear[4], const float vb[4][8], const float *bcolor, uint8_t *out, uint64_t sig) {
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf buf = make_buf(sizeof *vb * 4, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    for (int i = 0; i < 4; i++) p.color[0].clear.f[i] = clear[i];
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    if (bcolor) memcpy(dyn.blend_color, bcolor, sizeof dyn.blend_color);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, buf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 4; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(r), "blend commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "blend wait：%d %s", e.rc.code, e.why);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C69：双源混合因子（NVN 0x10–0x13 → SRC1_*）。缺 dualSrcBlend 的机型（多数 Mali / Xclipse 之外的低端档）
// 用到它的管线须按规定报 UNSUPPORTED、不崩、不建 Vulkan 管线；有特性时须正常建成。
// 先测缺特性（把已启用的位临时清掉），再测设备实况：成功的管线进后端缓存，顺序反了会命中缓存。
static nvn_gpu_rc c69_dual_src_get(const char *dir, nvn_gpu_pipe *pipe, nvn_gpu_err *e) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    *pipe = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) {
        nvn_gpu_rc bad = {NVN_GPU_E_ARG};
        return bad;
    }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.color.blend_en = 1;
    pd.blend[0].src_rgb = 0x10; pd.blend[0].dst_rgb = 0x11;   // SRC1_COLOR / ONE_MINUS_SRC1_COLOR
    pd.blend[0].src_a = 0x2; pd.blend[0].dst_a = 0x1; pd.blend[0].eq_rgb = 1; pd.blend[0].eq_a = 1;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, pipe, e);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return r;
}

static void c69_dual_src_blend(const char *dir) {
    VkBool32 has = g_dev.core_features.dualSrcBlend;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    memset(&e, 0, sizeof e);
    g_dev.core_features.dualSrcBlend = VK_FALSE;
    nvn_gpu_rc r = c69_dual_src_get(dir, &pipe, &e);
    g_dev.core_features.dualSrcBlend = has;
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !pipe, "C69 缺 dualSrcBlend 时双源混合管线应 UNSUPPORTED，实 %d", r.code);
    pipe = NULL;
    memset(&e, 0, sizeof e);
    r = c69_dual_src_get(dir, &pipe, &e);
    if (has) {
        CHECK(OK(r) && pipe, "C69 有 dualSrcBlend 时双源混合管线应建成：%d %s", r.code, e.why);
    } else {
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !pipe, "C69 设备无 dualSrcBlend，双源混合管线应 UNSUPPORTED，实 %d", r.code);
    }
    printf("C69 双源混合：设备 dualSrcBlend=%u，缺特性时 UNSUPPORTED%s\n", (unsigned)has,
           has ? "，有特性时建成" : "（本机实况即缺特性）");
}

static void c43_blend(const char *dir) {
    float red_half[4][8] = {{-1,-1,0,1, 1,0,0,0.5f},{-1,1,0,1, 1,0,0,0.5f},{1,-1,0,1, 1,0,0,0.5f},{1,1,0,1, 1,0,0,0.5f}};
    float red[4][8] = {{-1,-1,0,1, 1,0,0,1},{-1,1,0,1, 1,0,0,1},{1,-1,0,1, 1,0,0,1},{1,1,0,1, 1,0,0,1}};
    const float blue[4] = {0, 0, 1, 1};
    blend_st b;
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x5; b.dst_rgb = 0x6; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    uint8_t out[64 * 64 * 4];
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red_half, NULL, out, 1);
    const uint8_t *p = out;
    CHECK((p[0] == 127 || p[0] == 128) && p[1] == 0 && (p[2] == 127 || p[2] == 128) && p[3] == 255,
          "C43 alpha 混合 = %02x%02x%02x%02x，期望 7f/80 0 7f/80 ff", p[0], p[1], p[2], p[3]);
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x2; b.dst_rgb = 0x2; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red, NULL, out, 2);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "C43 加法 = %02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
    float yellow[4][8] = {{-1,-1,0,1, 1,1,0,1},{-1,1,0,1, 1,1,0,1},{1,-1,0,1, 1,1,0,1},{1,1,0,1, 1,1,0,1}};
    blend_draw(blend_pipe(dir, NULL, 0, 0x1), blue, yellow, NULL, out, 3);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "C43 写掩码 = %02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
    // 负控：把 alpha 混合换成加法，像素必须不同
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x2; b.dst_rgb = 0x2; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red_half, NULL, out, 4);
    CHECK(!((out[0] == 127 || out[0] == 128) && (out[2] == 127 || out[2] == 128)), "C43 负控：加法仍等于 alpha 期望（因子没生效）");
}


static nvn_gpu_rc prim_pipe(const char *dir, uint32_t prim, nvn_gpu_pipe *out) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, prim == 0 ? "point.vert" : "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return (nvn_gpu_rc){NVN_GPU_E_COMPILE};
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = prim; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, out, &e);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return rc;
}

static int prim_draw(const char *dir, uint32_t prim, uint32_t count, const float *vb, uint8_t *out, uint64_t sig) {
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc r = prim_pipe(dir, prim, &pipe);
    if (!OK(r)) return (int)r.code;
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf buf = make_buf((size_t)count * 32, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, buf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = prim; a.count = count; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    if (OK(r)) (void)nvn_gpu_fence_wait(sig, 5000000000ull, &e);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    return OK(r) ? 0 : (int)r.code;
}

static int count_px(const uint8_t *img, uint8_t r, uint8_t g, uint8_t b) {
    int n = 0;
    for (int i = 0; i < 64 * 64; i++) if (img[i*4] == r && img[i*4+1] == g && img[i*4+2] == b) n++;
    return n;
}

static void c44_prims(const char *dir) {
    uint8_t img[64 * 64 * 4];
    float pts[3][8] = {{-0.75f,-0.75f,0,1,1,0,0,1},{0,0,0,1,1,0,0,1},{0.75f,0.75f,0,1,1,0,0,1}};
    CHECK(prim_draw(dir, 0, 3, pts[0], img, 1) == 0, "C44 点绘制失败");
    CHECK(count_px(img, 255, 0, 0) >= 3, "C44 点像素不足：%d", count_px(img, 255, 0, 0));
    float line[2][8] = {{-1,0,0,1,1,0,0,1},{1,0,0,1,1,0,0,1}};
    CHECK(prim_draw(dir, 1, 2, line[0], img, 2) == 0, "C44 线绘制失败");
    CHECK(count_px(img, 255, 0, 0) > 10, "C44 线像素不足：%d", count_px(img, 255, 0, 0));
    float strip[3][8] = {{-0.75f,-0.75f,0,1,1,0,0,1},{0,0.5f,0,1,1,0,0,1},{0.75f,-0.75f,0,1,1,0,0,1}};
    uint8_t listimg[64 * 64 * 4];
    CHECK(prim_draw(dir, 1, 3, strip[0], listimg, 3) == 0, "C44 线列表失败");
    CHECK(prim_draw(dir, 3, 3, strip[0], img, 4) == 0, "C44 线带失败");
    CHECK(memcmp(img, listimg, sizeof img) != 0, "C44 线带与线列表结果相同（拓扑没生效）");
    int strc = prim_draw(dir, 5, 4, quad4[0], img, 5); CHECK(strc == 0, "C44 三角带失败：%d", strc);
    CHECK(count_px(img, 255, 0, 0) > 500 && count_px(img, 0, 255, 0) > 500, "C44 三角带两色不足");
    nvn_gpu_pipe dummy = NULL;
    nvn_gpu_rc frc = prim_pipe(dir, 6, &dummy);
    CHECK(OK(frc), "C44 三角扇在 lavapipe 上应可用，实为 %d", frc.code);
    CHECK(prim_pipe(dir, 2, &dummy).code == NVN_GPU_E_UNSUPPORTED, "C44 线环应 UNSUPPORTED");
}


static void blend2_draw(const char *dir, const blend_st b[2], uint8_t *out0, uint8_t *out1, uint64_t sig) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad2.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C45 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.cfmt[1] = NVN_FORMAT_RGBA8;
    pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xFF;
    pd.color.blend_en = 0x3;
    pd.blend[0] = b[0]; pd.blend[1] = b[1];
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C45 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex t0 = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_tex t1 = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v0 = make_view(t0), v1 = make_view(t1);
    float red_half[4][8] = {{-1,-1,0,1, 1,0,0,0.5f},{-1,1,0,1, 1,0,0,0.5f},{1,-1,0,1, 1,0,0,0.5f},{1,1,0,1, 1,0,0,0.5f}};
    nvn_gpu_buf vb = make_buf(sizeof red_half, NVN_GPU_MEM_SHARED, red_half);
    uint8_t z[64 * 64 * 4]; memset(z, 0, sizeof z);
    nvn_gpu_buf r0 = make_buf(sizeof z, NVN_GPU_MEM_SHARED, z), r1 = make_buf(sizeof z, NVN_GPU_MEM_SHARED, z);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 2; p.color[0].view = v0; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[1].view = v1; p.color[1].load = NVN_GPU_LOAD_CLEAR; p.color[1].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;
    p.color[1].clear.f[2] = 1; p.color[1].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 4; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t0, 0, 0, NULL, r0, 0, 256, 16384);
    nvn_gpu_copy_tex_buf(c, t1, 0, 0, NULL, r1, 0, 256, 16384);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(rc), "C45 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "C45 wait：%d %s", e.rc.code, e.why);
    memcpy(out0, buf_cpu(r0), 64 * 64 * 4); memcpy(out1, buf_cpu(r1), 64 * 64 * 4);
    nvn_gpu_view_release(v0); nvn_gpu_view_release(v1);
    nvn_gpu_tex_release(t0); nvn_gpu_tex_release(t1);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(r0, NULL, NULL); nvn_gpu_buf_release(r1, NULL, NULL);
}

static void c45_multi_blend(const char *dir) {
    blend_st b[2];
    memset(b, 0, sizeof b);
    for (int i = 0; i < 2; i++) { b[i].src_rgb = 0x5; b[i].dst_rgb = 0x6; b[i].src_a = 0x2; b[i].dst_a = 0x2; b[i].eq_rgb = 1; b[i].eq_a = 1; }
    b[1].src_rgb = 0x2; b[1].dst_rgb = 0x2; b[1].src_a = 0x2; b[1].dst_a = 0x2;
    uint8_t o0[64 * 64 * 4], o1[64 * 64 * 4];
    blend2_draw(dir, b, o0, o1, 1);
    CHECK((o0[0] == 127 || o0[0] == 128) && o0[1] == 0 && (o0[2] == 127 || o0[2] == 128),
          "C45 target0 alpha = %02x%02x%02x", o0[0], o0[1], o0[2]);
    CHECK(o1[0] == 0 && o1[1] == 255 && o1[2] == 255, "C45 target1 加法 = %02x%02x%02x", o1[0], o1[1], o1[2]);
    blend_st sw[2]; sw[0] = b[1]; sw[1] = b[0];
    uint8_t n0[64 * 64 * 4], n1[64 * 64 * 4];
    blend2_draw(dir, sw, n0, n1, 2);
    CHECK(n0[0] == 255 && n0[2] == 255, "C45 负控：交换后 target0 应为加法：%02x%02x%02x", n0[0], n0[1], n0[2]);
}


static void c46_blend_const(const char *dir) {
    blend_st b;
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x61; b.dst_rgb = 0x1; b.src_a = 0x61; b.dst_a = 0x1; b.eq_rgb = 1; b.eq_a = 1;
    float cb[4] = {0.25f, 0.5f, 0.75f, 0.25f};
    float clear[4] = {0, 0, 0, 1};
    float vb[4][8] = {{-1,-1,0,1,1,1,0,1},{-1,1,0,1,1,1,0,1},{1,-1,0,1,1,1,0,1},{1,1,0,1,1,1,0,1}};
    uint8_t out[64 * 64 * 4];
    blend_draw(blend_pipe(dir, &b, 1, 0xF), clear, vb, cb, out, 6);
    CHECK((out[0] == 63 || out[0] == 64) && out[1] == 0 && out[2] == 0 && (out[3] == 63 || out[3] == 64),
          "C46 混合常量 = %02x%02x%02x%02x，期望 3f/40 0 0 3f/40", out[0], out[1], out[2], out[3]);
}


// #863: expected bytes are a hand-calculated truth table for S=0x96, D=0x3c.
// Vulkan framebuffer-logicop excludes floating-point and sRGB attachments.
static void c47_case(const char *dir, unsigned op, unsigned fmt, unsigned mask,
                     int mixed, uint8_t expected, uint64_t serial) {
    void *vm=NULL,*fm=NULL; nvn_gpu_shader vs=NULL,fs=NULL;
    const int integer = fmt == NVN_FORMAT_RGBA8UI;
    CHECK(load_shader_pair(dir,"quad.vert",integer ? (mixed ? "logic_uint_mrt.frag" : "logic_uint.frag") : (mixed ? "logic_float_mrt.frag" : "logic_float.frag"),
                           NULL,NULL,&vs,&fs,&vm,&fm), "C47 shader");
    nvn_gpu_pipe_desc pd={0}; pd.poly.mode=NVN_POLYGON_MODE_FILL;
    pd.origin_ll=(uint8_t)!g_tri_origin_ul; pd.color.logic_op=op;
    pd.st[NVN_GPU_SH_VS].sh=vs; pd.st[NVN_GPU_SH_FS].sh=fs;
    pd.cfmt[0]=fmt; if(mixed) pd.cfmt[1]=NVN_FORMAT_RGBA32F;
    pd.samples=1; pd.prim=5; pd.chmask.mask=mask | (mixed ? 0xf0 : 0);
    // Deliberately enabled: non-COPY logic must override blending, even on float targets.
    pd.color.blend_en=integer ? (mixed ? 2 : 0) : (mixed ? 3 : 1);
    for(int j=0;j<2;j++) { pd.blend[j].src_rgb=2;pd.blend[j].dst_rgb=2;
        pd.blend[j].src_a=2;pd.blend[j].dst_a=2;pd.blend[j].eq_rgb=1;pd.blend[j].eq_a=1; }
    pd.attr[0].fmt=NVN_FORMAT_RGBA32F;pd.attr[1].fmt=NVN_FORMAT_RGBA32F;
    pd.attr[1].offset=16;pd.stream[0].stride=32;
    // Reuse the exact shader objects and descriptor; change only logic_op XOR -> OR -> XOR.
    // This detects pipeline cache aliasing, rather than giving each op fresh shader keys.
    const unsigned original_op=op;
    const uint8_t original_expected=expected;
    const unsigned rounds=(op==6 && fmt==NVN_FORMAT_RGBA8 && mask==15 && !mixed)?3:1;
    for(unsigned round=0;round<rounds;round++) {
    op=round==1?7:original_op; expected=round==1?0xbe:original_expected;
    pd.color.logic_op=op;
    nvn_gpu_pipe pipe=NULL;nvn_gpu_err e={0};nvn_gpu_rc rc=nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e);
    CHECK(OK(rc),"C47 op=%u fmt=%u pipeline: %d %s",op,fmt,rc.code,e.why);
    if(!OK(rc)) goto done;
    nvn_gpu_tex t=make_tex(64,64,fmt,NVN_GPU_TEX_RENDER), ft=NULL;
    nvn_gpu_view v=make_view(t),fv=NULL;
    if(mixed){ft=make_tex(64,64,NVN_FORMAT_RGBA32F,NVN_GPU_TEX_RENDER);fv=make_view(ft);}
    uint8_t zero[64*64*16]={0};
    nvn_gpu_buf rb=make_buf(64*64*4,NVN_GPU_MEM_SHARED,zero),fr=NULL;
    if(mixed)fr=make_buf(sizeof zero,NVN_GPU_MEM_SHARED,zero);
    float verts[4][8]={{-1,-1,0,1,0,0,0,1},{-1,1,0,1,0,0,0,1},{1,-1,0,1,0,0,0,1},{1,1,0,1,0,0,0,1}};
    nvn_gpu_buf vb=make_buf(sizeof verts,NVN_GPU_MEM_SHARED,verts);
    nvn_gpu_cmd c=begin_cmd();nvn_gpu_pass_desc pass={0};pass.ncolor=mixed?2:1;
    pass.color[0].view=v;pass.color[0].load=NVN_GPU_LOAD_CLEAR;pass.color[0].store=NVN_GPU_STORE_STORE;
    for(int j=0;j<4;j++){if(integer)pass.color[0].clear.u[j]=0x3c;else pass.color[0].clear.f[j]=60.0f/255.0f;}
    if(mixed){pass.color[1].view=fv;pass.color[1].load=NVN_GPU_LOAD_CLEAR;pass.color[1].store=NVN_GPU_STORE_STORE;
        for(int j=0;j<4;j++)pass.color[1].clear.f[j]=0.25f;}
    pass.w=64;pass.h=64;pass.layers=1;nvn_gpu_pass_begin(c,&pass);nvn_gpu_set_pipe(c,pipe);
    nvn_gpu_dyn dyn={0};dyn.origin_ll=(uint8_t)!g_tri_origin_ul;dyn.nvp=1;dyn.vp[0][2]=64;dyn.vp[0][3]=64;dyn.vp[0][5]=1;
    nvn_gpu_set_dyn(c,&dyn);nvn_gpu_set_vbuf(c,0,vb,0);
    nvn_gpu_draw_args draw={0};draw.prim=5;draw.count=4;draw.instances=1;nvn_gpu_draw(c,&draw);nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c,t,0,0,NULL,rb,0,256,16384);
    if(mixed)nvn_gpu_copy_tex_buf(c,ft,0,0,NULL,fr,0,1024,65536);
    rc=nvn_gpu_cmd_commit(c,serial+round,on_done,NULL,&e);CHECK(OK(rc),"C47 commit: %d %s",rc.code,e.why);
    if(OK(rc)){rc=nvn_gpu_fence_wait(serial+round,5000000000ull,&e);CHECK(OK(rc),"C47 wait");}
    if(OK(rc)){const uint8_t *out=buf_cpu(rb);for(int i=0;i<64*64*4;i++){
        uint8_t want=(mask & (1u<<(i%4)))?expected:0x3c;
        if(fmt==NVN_FORMAT_RGBA8_SRGB && i%4==3) want=150; // alpha is not sRGB encoded
        if(out[i]!=want){CHECK(0,"C47 op=%u fmt=%u mask=%x byte%d=%02x expected=%02x",op,fmt,mask,i,out[i],want);break;}}
        if(mixed){const float *f=(const float *)buf_cpu(fr);for(int i=0;i<64*64*4;i++)if(f[i]!=0.5f){CHECK(0,"C47 float attachment logic/blend must not apply: %g",(double)f[i]);break;}}}
    nvn_gpu_view_release(v);nvn_gpu_tex_release(t);nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_buf_release(vb,NULL,NULL);
    if(mixed){nvn_gpu_view_release(fv);nvn_gpu_tex_release(ft);nvn_gpu_buf_release(fr,NULL,NULL);}
    }
    if(rounds==3) printf("C47 same-shader XOR/OR/XOR cache sequence verified\n");
done:
    nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);free(vm);free(fm);
}
static void c47_logic(const char *dir) {
    // CLEAR, AND, AND_REVERSE, COPY, AND_INVERTED, NOOP, XOR, OR,
    // NOR, EQUIVALENT, INVERT, OR_REVERSE, COPY_INVERTED, OR_INVERTED, NAND, SET.
    static const uint8_t truth[16]={0x00,0x14,0x82,0x96,0x28,0x3c,0xaa,0xbe,0x41,0x55,0xc3,0xd7,0x69,0x7d,0xeb,0xff};
    const int enabled = g_dev.core_features.logicOp != VK_FALSE;
    printf("C47 logicOp feature=%d; integer/UNORM 16-op truth tables%s\n",enabled,enabled?"":" unavailable on this device");
    uint64_t seq=400;
    if(enabled) {
    for(unsigned op=0;op<16;op++){
        c47_case(dir,op,NVN_FORMAT_RGBA8UI,15,0,truth[op],(seq+=3));
        // COPY deliberately preserves additive blend, so S+D = 0xd2 instead of raw COPY.
        c47_case(dir,op,NVN_FORMAT_RGBA8,15,0,op==3?0xd2:truth[op],(seq+=3));
    }
    c47_case(dir,6,NVN_FORMAT_RGBA8,5,0,0xaa,(seq+=3));
    c47_case(dir,6,NVN_FORMAT_RGBA8UI,10,1,0xaa,(seq+=3));
    c47_case(dir,6,NVN_FORMAT_RGBA8,15,1,0xaa,(seq+=3));
    // sRGB excludes logic ops, and non-COPY disables blend: linear 150/255 -> sRGB 202.
    c47_case(dir,6,NVN_FORMAT_RGBA8_SRGB,15,0,202,(seq+=3));
    }
    nvn_gpu_err reset={0};
    CHECK(OK(nvn_gpu_close(5000000000ull,&reset)),"C47 close before capability negative");
    if(!open_backend())return; // empty pipeline cache: negative must not hit a previously compiled pipeline
    void *vm=NULL,*fm=NULL;nvn_gpu_shader vs=NULL,fs=NULL;
    CHECK(load_shader_pair(dir,"quad.vert","logic_float.frag",NULL,NULL,&vs,&fs,&vm,&fm),"C47 negative shader");
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.origin_ll=(uint8_t)!g_tri_origin_ul;
    pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=fs;pd.cfmt[0]=NVN_FORMAT_RGBA8;
    pd.samples=1;pd.prim=5;pd.chmask.mask=15;pd.attr[0].fmt=pd.attr[1].fmt=NVN_FORMAT_RGBA32F;pd.attr[1].offset=16;pd.stream[0].stride=32;
    nvn_gpu_pipe pipe=NULL;nvn_gpu_err e={0};
    // logic_op is a 4-bit public field: no invalid enumerant is representable.
    VkBool32 saved=g_dev.core_features.logicOp;g_dev.core_features.logicOp=VK_FALSE;
    pd.color.logic_op=6;CHECK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e).code==NVN_GPU_E_UNSUPPORTED,"C47 disabled feature must UNSUPPORTED");
    pd.color.logic_op=3;CHECK(OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e)),"C47 COPY without logicOp remains supported");
    g_dev.core_features.logicOp=saved;
    nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);free(vm);free(fm);
    CHECK(OK(nvn_gpu_close(5000000000ull,&reset)),"C47 close after capability negative");
    CHECK(open_backend(),"C47 reopen for next test fence sequence");
}


static void c48_fan(const char *dir) {
    uint8_t fan[64 * 64 * 4], strip[64 * 64 * 4];
    CHECK(prim_draw(dir, 6, 4, quad4[0], fan, 7) == 0, "C48 三角扇绘制失败");
    CHECK(prim_draw(dir, 5, 4, quad4[0], strip, 8) == 0, "C48 三角带绘制失败");
    CHECK(memcmp(fan, strip, sizeof fan) != 0, "C48 三角扇与三角带结果相同（拓扑映射没生效）");
    CHECK(count_px(fan, 255, 0, 0) > 500 && count_px(fan, 0, 255, 0) > 500, "C48 三角扇两色不足");
}

static void c48_mismatch(const char *dir) {
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(prim_pipe(dir, 6, &pipe)), "C48 扇管线创建失败");
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf vb = make_buf(sizeof quad4, NVN_GPU_MEM_SHARED, quad4);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 5; a.count = 4; a.instances = 1;   // 管线是扇，draw 写带 → 必须 E_ARG
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C48 prim 与管线不一致应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t); nvn_gpu_buf_release(vb, NULL, NULL);
}


// vs_in / fs_in 给出时用调用方的着色器（不释放），used 回报这次取到的管线对象；都给 NULL 即 C49 原样。
static int restart_draw_with(const char *dir, int on, uint8_t *out, uint64_t sig,
                             nvn_gpu_shader vs_in, nvn_gpu_shader fs_in, nvn_gpu_pipe *used) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = vs_in, fs = fs_in;
    const int own = !vs_in;
    if (own && !load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return -1;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 5; pd.prim_restart = on ? 1 : 0; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    if (!OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e))) { if (own) { nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm); } return -1; }
    if (used) *used = pipe;
    // 0x10000 个顶点，0xFFFF 是合法顶点；关重启时它会产生一条连接两段的三角形
    float *vb = calloc(0x10000 * 8, sizeof(float));
    if (!vb) { if (own) { nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm); } return -1; }
    const float V[7][8] = {
        {-1,-1,0,1, 1,0,0,1}, {-1,1,0,1, 1,0,0,1}, {-0.95f,-1,0,1, 1,0,0,1},
        {0.8f,1,0,1, 0,1,0,1}, {1,-1,0,1, 0,1,0,1}, {1,1,0,1, 0,1,0,1},
        {0,0.5f,0,1, 1,0,0,1}};
    for (int i = 0; i < 6; i++) memcpy(vb + i * 8, V[i], sizeof V[i]);
    memcpy(vb + 0xFFFF * 8, V[6], sizeof V[6]);
    static const uint16_t idx[7] = {0,1,2,0xFFFF,3,4,5};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_buf vbuf = make_buf(0x10000 * 32, NVN_GPU_MEM_SHARED, vb);
    free(vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vbuf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 5; a.count = 7; a.instances = 1; a.ibuf = ib; a.itype = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 256, 16384);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    if (OK(r)) (void)nvn_gpu_fence_wait(sig, 5000000000ull, &e);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t); nvn_gpu_buf_release(vbuf, NULL, NULL);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    if (own) { nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm); }
    return OK(r) ? 0 : (int)r.code;
}

static int restart_draw(const char *dir, int on, uint8_t *out, uint64_t sig) {
    return restart_draw_with(dir, on, out, sig, NULL, NULL, NULL);
}

static void c49_restart(const char *dir) {
    uint8_t on[64 * 64 * 4], off[64 * 64 * 4];
    CHECK(restart_draw(dir, 1, on, 9) == 0, "C49 重启开启绘制失败");
    CHECK(restart_draw(dir, 0, off, 10) == 0, "C49 重启关闭绘制失败");
    const int px = (1 * 64 + 2) * 4;   // 关重启时 0xFFFF 顶点多出的连接三角形覆盖 (2,1)
    CHECK(on[px] == 0 && on[px + 1] == 0 && on[px + 2] == 0, "C49 开重启时指定像素应保持清屏色");
    CHECK(off[px] != 0 || off[px + 1] != 0 || off[px + 2] != 0, "C49 关重启时 0xFFFF 顶点应多画出连接三角形（覆盖指定像素）");
    CHECK(memcmp(on, off, sizeof on) != 0, "C49 重启关闭与开启结果相同（重启没生效）");
}

// C49b：管线缓存按「实际交给 Vulkan 的重启」分（独占一个会话，围栏从 1 起）。同一对着色器、其余状态全同：
//  * 条带开 / 关重启必须是两条管线，且各自画出 C49 的结果（共用一条时有一边画错）；
//  * 列表图元上重启不生效，开 / 关必须共用同一条管线（不能多编一条一模一样的）。
static void c49b_restart_pipe_cache(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C49b shader");
    if (!vs || !fs) return;
    uint8_t on[64 * 64 * 4], off[64 * 64 * 4];
    nvn_gpu_pipe p_on = NULL, p_off = NULL;
    CHECK(restart_draw_with(dir, 1, on, 1, vs, fs, &p_on) == 0, "C49b 条带开重启绘制失败");
    CHECK(restart_draw_with(dir, 0, off, 2, vs, fs, &p_off) == 0, "C49b 条带关重启绘制失败");
    CHECK(p_on && p_off && p_on != p_off, "C49b 条带开 / 关重启取到了同一条管线（缓存键漏了重启）");
    const int px = (1 * 64 + 2) * 4;
    CHECK(on[px] == 0 && on[px + 1] == 0 && on[px + 2] == 0, "C49b 开重启时指定像素应保持清屏色");
    CHECK(off[px] != 0 || off[px + 1] != 0 || off[px + 2] != 0, "C49b 关重启时应多画出连接三角形（同着色器复用了开重启的管线）");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;   // 三角形列表
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe l_on = NULL, l_off = NULL; nvn_gpu_err e;
    pd.prim_restart = 1;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &l_on, &e)), "C49b 列表开重启取管线：%d %s", e.rc.code, e.why);
    pd.prim_restart = 0;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &l_off, &e)), "C49b 列表关重启取管线：%d %s", e.rc.code, e.why);
    CHECK(l_on && l_on == l_off, "C49b 列表图元开 / 关重启应共用一条管线（重启在列表上不生效）");
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}

static void c50_list_restart(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C50 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.prim_restart = 1; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "C50 列表图元带 prim_restart 应可建管线（忽略重启）：%d %s", r.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}


static void sample_tex(const char *dir, const char *frag, uint32_t target, uint32_t depth,
                       const uint8_t *face_colors, uint32_t ncolors, uint32_t tw, uint32_t th,
                       const view_t *vv, uint8_t *img, uint64_t sig);

static void c51_array(const char *dir) {
    nvn_gpu_tex src = make_tex_target(4, 4, 5, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8,
                                      NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    static const uint8_t colors[5][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255},{255,255,0,255}};
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_view sv = make_view(src);
    view_t vd; memset(&vd, 0, sizeof vd);
    vd.set_layers = 1; vd.min_layer = 1; vd.nlayers = 4;
    nvn_gpu_err e;
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl; memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "tri.vert", "arr.frag", &vl, &fl, &vs, &fs, &vsm, &fsm), "C51 读着色器失败");
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul; pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C51 pipe：%d %s", e.rc.code, e.why);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C51 samp：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    uint8_t zero[64*64*4]; memset(zero,0,sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    for (uint32_t l = 0; l < 5; l++) {
        uint8_t px[4*4*4];
        for (int i = 0; i < 16; i++) memcpy(px + i*4, colors[l], 4);
        nvn_gpu_tex_upload(c, src, 0, l, NULL, px, 16, 64);
    }
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2]; memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = sv;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 256, 16384);
    CHECK(OK(nvn_gpu_cmd_commit(c, 11, on_done, NULL, &e)), "C51 commit：%d %s", e.rc.code, e.why);
    (void)0;
    CHECK(OK(nvn_gpu_fence_wait(11, 5000000000ull, &e)), "C51 wait：%d %s", e.rc.code, e.why);
    uint8_t img[64*64*4]; memcpy(img, buf_cpu(rb), sizeof img);
    const int pts[4][2] = {{16,16},{48,16},{16,48},{48,48}};
    const uint8_t *exp[4] = {colors[0], colors[1], colors[2], colors[3]};
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = img + (pts[i][1]*64 + pts[i][0])*4;
        CHECK(p[0]==exp[i][0] && p[1]==exp[i][1] && p[2]==exp[i][2], "C51 层 %d = %02x%02x%02x", i, p[0],p[1],p[2]);
    }
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    uint8_t img2[64*64*4];
    sample_tex(dir, "arr.frag", NVN_TEXTURE_TARGET_2D_ARRAY, 5, colors[0], 5, 4, 4, &vd, img2, 12);
    const uint8_t *q = img2 + (16*64 + 16)*4;
    CHECK(q[0]==colors[1][0] && q[1]==colors[1][1] && q[2]==colors[1][2],
          "C51 负控 起始层偏 1 后第一象限应为第二层颜色，实为 %02x%02x%02x", q[0],q[1],q[2]);
}


static void sample_tex(const char *dir, const char *frag, uint32_t target, uint32_t depth,
                       const uint8_t *face_colors, uint32_t ncolors, uint32_t tw, uint32_t th,
                       const view_t *vv, uint8_t *img, uint64_t sig) {
    nvn_gpu_tex src = make_tex_target(tw, th, depth, target, NVN_FORMAT_RGBA8,
                                      NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_view sv = NULL;
    if (vv) { nvn_gpu_err ve; CHECK(OK(nvn_gpu_view_create(src, vv, &sv, &ve)), "%s 视图：%d %s", frag, ve.rc.code, ve.why); }
    else sv = make_view(src);
    nvn_gpu_layout_ent ents[2]; memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl; memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "tri.vert", frag, &vl, &fl, &vs, &fs, &vsm, &fsm), "%s 读着色器失败", frag);
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul; pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "%s pipe：%d %s", frag, e.rc.code, e.why);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "%s samp：%d %s", frag, e.rc.code, e.why);
    const float verts[3][4] = {{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    uint8_t zero[64*64*4]; memset(zero,0,sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    for (uint32_t f = 0; f < ncolors; f++) {
        uint8_t px[64*4];
        for (uint32_t i = 0; i < tw * th; i++) memcpy(px + i*4, face_colors + f*4, 4);
        nvn_gpu_tex_upload(c, src, 0, f, NULL, px, tw*4, tw*4*th);
    }
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2]; memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = sv;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 256, 16384);
    CHECK(OK(nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e)), "%s commit：%d %s", frag, e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "%s wait：%d %s", frag, e.rc.code, e.why);
    memcpy(img, buf_cpu(rb), 64*64*4);
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}

static void check_cell(const char *tag, const uint8_t *img, int x, int y, const uint8_t *exp) {
    const uint8_t *p = img + (y*64 + x)*4;
    CHECK(p[0]==exp[0] && p[1]==exp[1] && p[2]==exp[2], "%s (%d,%d)=%02x%02x%02x 期望 %02x%02x%02x",
          tag, x, y, p[0],p[1],p[2], exp[0],exp[1],exp[2]);
}

static void c52_cube(const char *dir) {
    static const uint8_t c[6][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255},{255,0,255,255},{0,255,255,255}};
    uint8_t img[64*64*4];
    sample_tex(dir, "cube.frag", NVN_TEXTURE_TARGET_CUBEMAP, 6, c[0], 6, 1, 1, NULL, img, 13);
    for (int f = 0; f < 6; f++) check_cell("C52", img, 10 + (f%3)*22, 16 + (f/3)*32, c[f]);
    uint8_t sw[6][4];
    memcpy(sw, c, sizeof sw);
    memcpy(sw[0], c[1], 4); memcpy(sw[1], c[0], 4);
    uint8_t img2[64*64*4];
    sample_tex(dir, "cube.frag", NVN_TEXTURE_TARGET_CUBEMAP, 6, sw[0], 6, 1, 1, NULL, img2, 14);
    check_cell("C52 负控", img2, 10, 16, c[1]);
    CHECK(img2[0] != img[0] || img2[1] != img[1] || img2[2] != img[2], "C52 换面顺序后 (+X) 应不同");
}

static void c53_cube_array(const char *dir) {
    uint8_t c[12][4];
    memset(c, 0, sizeof c);
    for (int f = 0; f < 6; f++) { c[f][0] = 255; }          // 第 0 个立方体 +X 红、其余面随 f 变化
    for (int f = 0; f < 6; f++) { c[6+f][1] = 255; c[6+f][2] = 255; }   // 第 1 个立方体全青
    c[0][0] = 255; c[0][1] = 0; c[0][2] = 0;
    uint8_t img[64*64*4];
    sample_tex(dir, "cubearr.frag", NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, c[0], 12, 1, 1, NULL, img, 15);
    const uint8_t red[4] = {255,0,0,255}, cyan[4] = {0,255,255,255};
    check_cell("C53", img, 16, 32, red);
    check_cell("C53", img, 48, 32, cyan);
}

static void c54_3d(const char *dir) {
    static const uint8_t c[4][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
    uint8_t img[64*64*4];
    sample_tex(dir, "tex3d.frag", NVN_TEXTURE_TARGET_3D, 4, c[0], 4, 2, 2, NULL, img, 16);
    check_cell("C54", img, 16, 16, c[0]);
    check_cell("C54", img, 48, 16, c[1]);
    check_cell("C54", img, 16, 48, c[2]);
    check_cell("C54", img, 48, 48, c[3]);
}


extern uint64_t nvn_vk_dbg_cpipe_waits(void);
struct cs_worker { nvn_gpu_stage_desc cs; nvn_gpu_pipe pipe; nvn_gpu_rc result; nvn_gpu_err err; int warm; };
static void *cs_worker_run(void *arg) {
    struct cs_worker *w = arg;
    if (w->warm) nvn_gpu_pipe_warm_scope(1);
    w->result = nvn_gpu_cpipe_get(&w->cs, NVN_GPU_PIPE_SYNC, &w->pipe, &w->err);
    if (w->warm) nvn_gpu_pipe_warm_scope(0);
    return NULL;
}
static void c55_compute(const char *dir) {
    char path[512]; size_t csz = 0;
    snprintf(path, sizeof path, "%s/comp.comp.spv", dir);
    void *cm = read_file(path, &csz);
    CHECK(cm != NULL, "C55 读不到 comp.comp.spv");
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1; ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl; memset(&cl, 0, sizeof cl); cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader_src src; memset(&src, 0, sizeof src);
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main"; src.blob = cm; src.bytes = csz; src.layout = &cl;
    nvn_gpu_shader sh = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_shader_load(&src, &sh, &e)), "C55 shader：%d %s", e.rc.code, e.why);
    nvn_gpu_stage_desc cs; memset(&cs, 0, sizeof cs); cs.sh = sh;
    nvn_gpu_stage_desc cs_spec = cs; cs_spec.nspec = 1; nvn_gpu_pipe unsupported = NULL;
    CHECK(nvn_gpu_cpipe_get(&cs_spec, NVN_GPU_PIPE_SYNC, &unsupported, &e).code == NVN_GPU_E_UNSUPPORTED && !unsupported,
          "C55 未实现的计算特化必须报错");
    nvn_gpu_pipe cp = NULL, cold1 = NULL, cold2 = NULL;
    g_compute_warm = -1;
    CHECK(OK(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cold1, &e)) &&
          OK(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cold2, &e)) && cold1 && cold2 && cold1 != cold2,
          "C55 开关缺省关闭时沿用原来的不缓存路径");
    g_compute_warm = 1;
    uint64_t warm_hits_before = nvn_gpu_pipe_warm_hits(), waits_before = nvn_vk_dbg_cpipe_waits();
    unsigned creates_before = __atomic_load_n(&g_plc_rec.cp, __ATOMIC_RELAXED);
    struct cs_worker wa = {.cs=cs, .warm=1}, wb = {.cs=cs};
    pthread_t ta, tb;
    pthread_mutex_lock(&g_cs_gate_mu); g_cs_gate_on = 1; g_cs_gate_entered = g_cs_gate_release = 0; pthread_mutex_unlock(&g_cs_gate_mu);
    int ar = pthread_create(&ta, NULL, cs_worker_run, &wa);
    CHECK(ar == 0, "C55 预热线程启动");
    int entered = 0;
    if (!ar) {
        struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 5;
        pthread_mutex_lock(&g_cs_gate_mu);
        while (!g_cs_gate_entered) if (pthread_cond_timedwait(&g_cs_gate_cv, &g_cs_gate_mu, &deadline) == ETIMEDOUT) break;
        entered = g_cs_gate_entered; pthread_mutex_unlock(&g_cs_gate_mu);
    }
    CHECK(entered, "C55 预建到达真实驱动入口");
    int br = entered ? pthread_create(&tb, NULL, cs_worker_run, &wb) : -1;
    CHECK(br == 0, "C55 提交线程启动");
    if (!br) {
        struct timespec pause = {0, 1000000};
        for (int i = 0; i < 2000 && nvn_vk_dbg_cpipe_waits() == waits_before; i++) nanosleep(&pause, NULL);
        CHECK(nvn_vk_dbg_cpipe_waits() > waits_before, "C55 同一 CS 必须等待在途预建");
    }
    pthread_mutex_lock(&g_cs_gate_mu); g_cs_gate_release = 1; g_cs_gate_on = 0; pthread_cond_broadcast(&g_cs_gate_cv); pthread_mutex_unlock(&g_cs_gate_mu);
    if (!ar) pthread_join(ta, NULL);
    if (!br) pthread_join(tb, NULL);
    CHECK(!ar && !br && OK(wa.result) && OK(wb.result), "C55 并发取计算管线成功");
    CHECK(__atomic_load_n(&g_plc_rec.cp, __ATOMIC_RELAXED) == creates_before + 1, "C55 同配方只调用一次 Vulkan 计算管线创建");
    cp = wa.pipe; nvn_gpu_pipe cp_used = wb.pipe;
    CHECK(cp && cp_used == cp, "C55 预建的计算管线必须由真实派发复用");
    CHECK(nvn_gpu_pipe_warm_hits() == warm_hits_before + 1, "C55 首次使用计一次预热命中");
    cp = cp_used;
    g_compute_warm = -1;
    uint32_t data[64]; memset(data, 0, sizeof data);
    nvn_gpu_buf buf = make_buf(sizeof data, NVN_GPU_MEM_SHARED, data);
    nvn_gpu_binding b; memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO;
    b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = sizeof data;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c, cp, 4, 1, 1, NULL, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c, 17, on_done, NULL, &e)), "C55 commit：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(17, 5000000000ull, &e)), "C55 wait：%d %s", e.rc.code, e.why);
    uint32_t rd[64]; memcpy(rd, buf_cpu(buf), sizeof rd);
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != i + 1) bad++;
    CHECK(bad == 0, "C55 dispatch 结果错 %d 项（首项 %u）", bad, rd[0]);
    // 间接 dispatch：先把 GPU 缓冲清 0，否则即使什么都不做也会读到上一次的 1..64
    memset(buf_cpu(buf), 0, sizeof rd);
    uint32_t indv[3] = {4, 1, 1};
    nvn_gpu_buf ind = make_buf(sizeof indv, NVN_GPU_MEM_SHARED, indv);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_set_pipe(c2, cp);
    nvn_gpu_bind(c2, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c2, cp, 0, 0, 0, ind, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c2, 18, on_done, NULL, &e)), "C55 间接 commit：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(18, 5000000000ull, &e)), "C55 间接 wait：%d %s", e.rc.code, e.why);
    memcpy(rd, buf_cpu(buf), sizeof rd);
    bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != i + 1) bad++;
    CHECK(bad == 0, "C55 间接 dispatch 结果错 %d 项", bad);
    // #659: GPU writes parameters, then native indirect dispatch consumes them
    // in the same submission. No CPU read/patch of generated parameters.
    memset(buf_cpu(buf), 0, sizeof rd);
    uint32_t zeros[64] = {0};
    uint32_t stale[4] = {99, 0, 0, 0};
    nvn_gpu_buf generated = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_buf gpu_ind = make_buf(sizeof stale, NVN_GPU_MEM_SHARED, stale);
    nvn_gpu_buf drv = make_buf(sizeof stale, NVN_GPU_MEM_SHARED, stale);
    nvn_gpu_binding producer = b; producer.u.b.buf = generated;
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_set_pipe(c3, cp);
    nvn_gpu_bind(c3, NVN_GPU_SH_CS, &producer, 1);
    nvn_gpu_dispatch(c3, cp, 1, 1, 1, NULL, 0);
    // Existing comp.comp produces 1..16. Assemble {2,1,1} on GPU at offset 4.
    nvn_gpu_copy_buf(c3, generated, 4, gpu_ind, 4, 4);
    nvn_gpu_copy_buf(c3, generated, 0, gpu_ind, 8, 4);
    nvn_gpu_copy_buf(c3, generated, 0, gpu_ind, 12, 4);
    // Model DRV0's group-count copy from the exact same authoritative buffer.
    nvn_gpu_copy_buf(c3, gpu_ind, 4, drv, 4, 12);
    nvn_gpu_barrier(c3, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);
    nvn_gpu_bind(c3, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c3, cp, 0, 0, 0, gpu_ind, 4);
    CHECK(OK(nvn_gpu_cmd_commit(c3, 19, on_done, NULL, &e)), "C55 GPU indirect commit: %d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(19, 5000000000ull, &e)), "C55 GPU indirect wait: %d %s", e.rc.code, e.why);
    memcpy(rd, buf_cpu(buf), sizeof rd);
    bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != (i < 32 ? i + 1 : 0)) bad++;
    CHECK(bad == 0, "C55 GPU generated indirect: %d wrong outputs", bad);
    uint32_t counts[4]; memcpy(counts, buf_cpu(drv), sizeof counts);
    CHECK(counts[0] == 99 && counts[1] == 2 && counts[2] == 1 && counts[3] == 1,
          "C55 GPU DRV0 copy: %u %u %u %u", counts[0], counts[1], counts[2], counts[3]);
    nvn_gpu_buf_release(drv, NULL, NULL);
    nvn_gpu_buf_release(gpu_ind, NULL, NULL);
    nvn_gpu_buf_release(generated, NULL, NULL);
    // 0 组 dispatch：空操作，命令缓冲仍应可提交
    nvn_gpu_cmd c4 = begin_cmd();
    nvn_gpu_set_pipe(c4, cp);
    nvn_gpu_bind(c4, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c4, cp, 0, 1, 1, NULL, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c4, 20, on_done, NULL, &e)), "C55 0 组 dispatch 后 commit 失败：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(20, 5000000000ull, &e)), "C55 0 组 wait：%d %s", e.rc.code, e.why);
    nvn_gpu_buf_release(ind, NULL, NULL);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh); free(cm);
}

// ---------- C56–C68：D8 补口（资源类别 + push 常量）契约用例 + D9 静默错复现 ----------
// 期望值全部按契约与 GLSL/SPIR-V 语义手算，不经过被测实现的路径。

static nvn_gpu_shader load_one_shader(const char *dir, const char *name, nvn_gpu_stage stage,
                                      const nvn_gpu_layout *lay) {
    char path[512];
    size_t sz = 0;
    snprintf(path, sizeof path, "%s/%s.spv", dir, name);
    void *mem = read_file(path, &sz);
    CHECK(mem != NULL, "读不到 %s.spv", name);
    if (!mem) return NULL;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.stage = stage; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = mem; src.bytes = sz; src.layout = lay;
    nvn_gpu_err e;
    nvn_gpu_shader sh = NULL;
    CHECK(OK(nvn_gpu_shader_load(&src, &sh, &e)), "%s shader：%d %s", name, e.rc.code, e.why);
    free(mem);
    return sh;
}

static nvn_gpu_pipe mk_cpipe(const char *dir, const char *name, const nvn_gpu_layout *lay, nvn_gpu_shader *sh_out) {
    *sh_out = NULL;
    nvn_gpu_shader sh = load_one_shader(dir, name, NVN_GPU_SH_CS, lay);
    if (!sh) return NULL;
    nvn_gpu_stage_desc cs;
    memset(&cs, 0, sizeof cs);
    cs.sh = sh;
    nvn_gpu_err e;
    nvn_gpu_pipe cp = NULL;
    CHECK(OK(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e)), "%s cpipe：%d %s", name, e.rc.code, e.why);
    *sh_out = sh;
    return cp;
}

static void fill_gpipe_desc(nvn_gpu_pipe_desc *pd, nvn_gpu_shader vs, nvn_gpu_shader fs, uint32_t dfmt) {
    memset(pd, 0, sizeof *pd); pd->poly.mode=NVN_POLYGON_MODE_FILL;
    pd->color.logic_op = 0x3;
    pd->st[NVN_GPU_SH_VS].sh = vs;
    pd->st[NVN_GPU_SH_FS].sh = fs;
    pd->cfmt[0] = NVN_FORMAT_RGBA8;
    pd->dfmt = dfmt;
    pd->samples = 1;
    pd->prim = 4;
    pd->chmask.mask = 0xF;
    pd->attr[0].fmt = NVN_FORMAT_RGBA32F; pd->attr[0].offset = 0; pd->attr[0].stream = 0;
    pd->stream[0].stride = 16;
    pd->origin_ll = 1;   // 与 full_dyn 的窗口原点一致（契约 v6：两处必须相同）
}

// 真实 Vulkan 管线缓存：同一语义的位域对象即便填充位不同也应命中。
static void poly_key_padding(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) {
        CHECK(0, "poly key shaders"); return;
    }
    nvn_gpu_pipe_desc a, b; fill_gpipe_desc(&a, vs, fs, 0); memcpy(&b, &a, sizeof b);
    memset(&a.poly, 0, sizeof a.poly); memset(&b.poly, 0xff, sizeof b.poly);
    a.poly.cull = b.poly.cull = 0; a.poly.front = b.poly.front = 1;
    a.poly.mode = b.poly.mode = NVN_POLYGON_MODE_FILL; a.poly.offset_en = b.poly.offset_en = 0;
    CHECK(memcmp(&a.poly, &b.poly, sizeof a.poly) != 0, "poly 夹具须保留不同填充位");
    nvn_gpu_err e = {0}; nvn_gpu_pipe p = NULL, q = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&a, NVN_GPU_PIPE_SYNC, &p, &e)) && p, "poly 首次取管线 %s", e.why);
    CHECK(OK(nvn_gpu_pipe_get(&b, NVN_GPU_PIPE_SYNC, &q, &e)) && q, "poly 再次取管线 %s", e.why);
    CHECK(p == q, "poly：四成员相同、填充位不同时应命中同一条管线");
    for (int i = 0; i < 4; i++) {
        nvn_gpu_pipe_desc c; memcpy(&c, &a, sizeof c);
        if (i == 0) c.poly.cull = 2;
        if (i == 1) c.poly.front = 0;
        if (i == 2) c.poly.mode = NVN_POLYGON_MODE_LINE;
        if (i == 3) c.poly.offset_en = 4;
        if (i == 2 && !g_dev.core_features.fillModeNonSolid) continue;
        nvn_gpu_pipe changed = NULL;
        CHECK(OK(nvn_gpu_pipe_get(&c, NVN_GPU_PIPE_SYNC, &changed, &e)) && changed, "poly 字段 %d 管线 %s", i, e.why);
        CHECK(changed != p, "poly 字段 %d 不同须保留不同管线", i);
    }
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

static void full_dyn(nvn_gpu_dyn *d) {
    memset(d, 0, sizeof *d); d->origin_ll = 1;   /* 原点在左下 = Vulkan 的自然方向 */
    d->nvp = 1; d->vp[0][2] = 64; d->vp[0][3] = 64; d->vp[0][5] = 1;
    d->has_sc = 1; d->sc[2] = 64; d->sc[3] = 64;
}

static nvn_gpu_buf full_vb(void) {
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    return make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
}

// C56：FS push 常量。两次 push 两个颜色，两笔各画一半屏。
// 手算：RGBA8 的 UNORM 量化 0.25*255=63.75→64、0.5*255=127.5→128、0.75*255=191.25→191。
static void c56_push_fs(const char *dir) {
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.push_off = 0; fl.push_bytes = 16;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 pushc 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C56 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.sc[3] = 32;   // 上半屏第一笔
    const float ca[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, ca, sizeof ca);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    const float cb[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, cb, sizeof cb);
    dyn.sc[1] = 32;   // 下半屏第二笔
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 100, on_done, NULL, &e);
    CHECK(OK(r), "C56 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(100, 5000000000ull, &e);
    CHECK(OK(r), "C56 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 64, 128, 191, "C56 上半");
    expect_rect(buf_cpu(rb), 0, 32, 64, 32, 255, 0, 0, "C56 下半");
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C57：CS push 常量 + SSBO 绑定共存。d[i] = 7 + i（i = 0..63 → 7..70）。
static void c57_push_cs(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1; cl.push_off = 0; cl.push_bytes = 4;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "pushc.comp", &cl, &sh);
    CHECK(cp != NULL, "C57 cpipe 为空");
    if (!cp) return;
    nvn_gpu_buf buf = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = 64 * 4;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    uint32_t v = 7;
    nvn_gpu_push(c, NVN_GPU_SH_CS, 0, &v, sizeof v);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 101, on_done, NULL, &e);
    CHECK(OK(r), "C57 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(101, 5000000000ull, &e);
    CHECK(OK(r), "C57 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(buf);
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != 7 + i) bad++;
    CHECK(bad == 0, "C57 push 结果错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C58d：同一个 cmd 切到更大 push 范围时，旧阶段值必须保留且写入不越界。
static void c58d_push_pipe_resize(const char *dir) {
    nvn_gpu_layout_ent ent = {0};
    ent.set = 0; ent.cls = NVN_GPU_RES_SSBO; ent.access = NVN_GPU_ACC_W;
    ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout small = {0}, large = {0};
    small.ent = large.ent = &ent; small.nent = large.nent = 1;
    small.push_bytes = 4; large.push_bytes = 16;
    nvn_gpu_shader a = NULL, b = NULL;
    nvn_gpu_pipe pa = mk_cpipe(dir, "pushc.comp", &small, &a);
    nvn_gpu_pipe pb = mk_cpipe(dir, "pushc.comp", &large, &b);
    CHECK(pa && pb, "C58d 两个计算管线必须可创建");
    if (!pa || !pb) { if (a) nvn_gpu_shader_release(a); if (b) nvn_gpu_shader_release(b); return; }
    nvn_gpu_buf out = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd();
    nvn_gpu_set_pipe(cmd, pa);
    uint32_t value = 7, last = 42;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &value, sizeof value);
    nvn_gpu_set_pipe(cmd, pb);
    uint32_t capacity = nvn_gpu_vk_test_push_capacity(cmd, NVN_GPU_SH_CS);
    CHECK(capacity >= large.push_bytes, "C58d 切换管线后的容量 %u，小于新范围 %u", capacity, large.push_bytes);
    if (capacity < large.push_bytes) {
        nvn_gpu_cmd_abort(cmd); nvn_gpu_buf_release(out, NULL, NULL);
        nvn_gpu_shader_release(a); nvn_gpu_shader_release(b); return;
    }
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 12, &last, sizeof last);
    nvn_gpu_binding binding = {0}; binding.ent = 0; binding.cls = NVN_GPU_RES_SSBO;
    binding.u.b.buf = out; binding.u.b.len = 64 * 4;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &binding, 1);
    nvn_gpu_dispatch(cmd, pb, 1, 1, 1, NULL, 0);
    nvn_gpu_err error; nvn_gpu_rc result = nvn_gpu_cmd_commit(cmd, 102, on_done, NULL, &error);
    CHECK(OK(result), "C58d commit %d %s", result.code, error.why);
    result = nvn_gpu_fence_wait(102, 5000000000ull, &error);
    CHECK(OK(result), "C58d wait %d %s", result.code, error.why);
    const uint32_t *values = (const uint32_t *)buf_cpu(out);
    int bad = 0; for (uint32_t i = 0; i < 64; i++) if (values[i] != 7 + i) bad++;
    CHECK(!bad, "C58d 切管线后 push 值错误：%d 项", bad);
    nvn_gpu_buf_release(out, NULL, NULL); nvn_gpu_shader_release(a); nvn_gpu_shader_release(b);
}

// C58a–c：push 越界 / 未声明阶段 → 粘滞 E_ARG（契约：只许写进该阶段声明范围）。
// 失败段让时间线进终态、恢复只能关会话重开，故三个子例各占独立会话（main 里关/开）。
static nvn_gpu_pipe c58_cpipe(const char *dir, nvn_gpu_shader *sh_out) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1; cl.push_off = 0; cl.push_bytes = 4;
    return mk_cpipe(dir, "pushc.comp", &cl, sh_out);
}

static void c58a_push_oob_cs(const char *dir) {
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = c58_cpipe(dir, &sh);
    CHECK(cp != NULL, "C58a cpipe 为空");
    if (!cp) { nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    uint32_t v = 1;
    nvn_gpu_push(c, NVN_GPU_SH_CS, 2, &v, 4);   // 声明 [0,4)：写 [2,4) → off+4=6 > 4
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 102, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58a 越界 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_shader_release(sh);
}

static void c58b_push_bad_stage(const char *dir) {
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = c58_cpipe(dir, &sh);
    CHECK(cp != NULL, "C58b cpipe 为空");
    if (!cp) { nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    uint32_t v = 1;
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, &v, 4);   // 计算管线无 FS 阶段 → 未声明范围
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 103, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58b 未声明阶段 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_shader_release(sh);
}

static void c58c_push_oob_fs(const char *dir) {
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.push_off = 0; fl.push_bytes = 16;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 pushc 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C58c pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_DONTCARE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    const float part[2] = {0.1f, 0.2f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 4, part, sizeof part);   // [4,8) ⊆ [0,16)：合法，不应置错
    nvn_gpu_push(c, NVN_GPU_SH_FS, 12, part, sizeof part);  // [12,8)：12+8=20 > 16 → E_ARG
    nvn_gpu_pass_end(c);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 104, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58c 越界 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C59a：缓冲视图显式携带格式，后端必须能创建并释放它。
static void c59a_buffer_view(void) {
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create_buf(b, 0, 16, NVN_FORMAT_R32UI, &v, &e);
    CHECK(OK(r) && v != NULL, "C59a R32UI 缓冲视图应创建成功：%d %s", r.code, e.why);
    if (v) nvn_gpu_view_release(v);
    for (int i = 0; i < 2; i++) {
        g_texel_feature_mask = i ? VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT
                                 : VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        v = NULL;
        r = nvn_gpu_view_create_buf(b, 0, 16, NVN_FORMAT_R32UI, &v, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !v,
              "C59a 模拟缺一种 texel 特性时不可创建双 usage 视图（%d）：%d", i, r.code);
        if (v) nvn_gpu_view_release(v);
    }
    g_texel_feature_mask = 0;
    const uint64_t offsets[] = {0, 0, 16};
    const uint64_t lengths[] = {0, 15, 4};
    for (int i = 0; i < 3; i++) {
        v = NULL;
        r = nvn_gpu_view_create_buf(b, offsets[i], lengths[i], NVN_FORMAT_R32UI, &v, &e);
        CHECK(r.code == NVN_GPU_E_ARG && !v, "C59a 非法范围 %d 应返回 ARG：%d", i, r.code);
        if (v) nvn_gpu_view_release(v);
    }
    nvn_gpu_buf_release(b, NULL, NULL);
}

// C59：Unknown 采样纹素缓冲的格式来自显式 view，读取结果须与输入一致。
static void c59_tbuf(const char *dir) {
    nvn_gpu_layout_ent ents[2] = {0};
    ents[0].cls = NVN_GPU_RES_TBUF; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_W;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ents; lay.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tbuf.comp", &lay, &sh);
    CHECK(cp != NULL, "C59 Unknown 采样 TBUF 管线必须创建成功");
    if (!cp) { if (sh) nvn_gpu_shader_release(sh); return; }
    const uint32_t src[4] = {11, 22, 33, 44};
    nvn_gpu_buf input = make_buf(sizeof src, NVN_GPU_MEM_SHARED, src);
    nvn_gpu_buf output = make_buf(sizeof src, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_view view = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create_buf(input, 0, sizeof src, NVN_FORMAT_R32UI, &view, &e);
    CHECK(OK(r) && view, "C59 视图创建失败：%d %s", r.code, e.why);
    if (!view) { nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL); nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b[2] = {0};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TBUF; b[0].u.view = view;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = output; b[1].u.b.len = sizeof src;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2);
    int retired_before = atomic_load(&g_retired);
    nvn_gpu_buf_release(input, on_retire, NULL);
    nvn_gpu_view_release(view);
    CHECK(atomic_load(&g_retired) == retired_before, "C59 录制中的视图未保活底层缓冲");
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    r = nvn_gpu_cmd_commit(c, 105, on_done, NULL, &e);
    CHECK(OK(r), "C59 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(105, 5000000000ull, &e);
    CHECK(OK(r), "C59 wait：%d %s", r.code, e.why);
    CHECK(!memcmp(buf_cpu(output), src, sizeof src), "C59 采样读回与输入不同");
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "C59 完成后底层缓冲未退休");
    CHECK(atomic_load(&g_retired) == retired_before + 1, "C59 底层缓冲退休次数不为一");
    nvn_gpu_buf_release(output, NULL, NULL);

    // 同一 Unknown 着色器用 RGBA8UI 时，读取每个 texel 的首分量。
    const uint8_t packed[16] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    const uint32_t expected[4] = {1,5,9,13};
    input = make_buf(sizeof packed, NVN_GPU_MEM_SHARED, packed);
    output = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
    view = NULL;
    r = nvn_gpu_view_create_buf(input, 0, sizeof packed, NVN_FORMAT_RGBA8UI, &view, &e);
    CHECK(OK(r) && view, "C59 RGBA8UI 视图创建失败：%d %s", r.code, e.why);
    if (view) {
        c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
        b[0].u.view = view; b[1].u.b.buf = output;
        nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2);
        nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
        r = nvn_gpu_cmd_commit(c, 106, on_done, NULL, &e);
        CHECK(OK(r), "C59 RGBA8UI commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(106, 5000000000ull, &e);
        CHECK(OK(r), "C59 RGBA8UI wait：%d %s", r.code, e.why);
        CHECK(!memcmp(buf_cpu(output), expected, sizeof expected), "C59 RGBA8UI 采样结果不符");
        nvn_gpu_view_release(view);
    }
    nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// 失败提交令时间线终止，旧绑定负测必须独占会话。
static void c59b_legacy_binding(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {0};
    ent[0].cls = NVN_GPU_RES_TBUF; ent[0].access = NVN_GPU_ACC_R;
    ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W;
    ent[1].binding = 1; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ent; lay.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tbuf.comp", &lay, &sh);
    CHECK(cp != NULL, "C59b 管线创建失败");
    if (!cp) { if (sh) nvn_gpu_shader_release(sh); return; }
    nvn_gpu_buf input = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding legacy = {0}; legacy.cls = NVN_GPU_RES_TBUF;
    legacy.u.b.buf = input; legacy.u.b.len = 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &legacy, 1);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 108, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C59b 旧 u.b 应返回 UNSUPPORTED：%d %s", r.code, e.why);
    nvn_gpu_buf_release(input, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C60：IMAGE_TBUF（storage texel buffer）。imageStore 写 src[i]+100 → {101,102,103,104}。
static void c60_image_tbuf(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_IMAGE_TBUF; ents[0].access = NVN_GPU_ACC_W;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = ents; cl.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "itbuf.comp", &cl, &sh);
    CHECK(cp != NULL, "C60 cpipe 为空");
    if (!cp) return;
    const uint32_t src[4] = {1, 2, 3, 4};
    nvn_gpu_buf ib = make_buf(sizeof src, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf sb = make_buf(sizeof src, NVN_GPU_MEM_SHARED, src);
    nvn_gpu_view iv = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc vr = nvn_gpu_view_create_buf(ib, 0, sizeof src, NVN_FORMAT_R32UI, &iv, &e);
    CHECK(OK(vr) && iv, "C60 视图创建失败：%d %s", vr.code, e.why);
    if (!iv) { nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(sb, NULL, NULL); nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[2];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_IMAGE_TBUF; bs[0].u.view = iv;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = sb; bs[1].u.b.off = 0; bs[1].u.b.len = sizeof src;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 2);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 107, on_done, NULL, &e);
    CHECK(OK(r), "C60 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(107, 5000000000ull, &e);
    CHECK(OK(r), "C60 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(ib);
    int bad = 0;
    for (int i = 0; i < 4; i++) if (rd[i] != src[i] + 100u) bad++;
    CHECK(bad == 0, "C60 imageStore 读回错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_view_release(iv);
    nvn_gpu_buf_release(ib, NULL, NULL);
    nvn_gpu_buf_release(sb, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C1072a：顶点阶段写存储 / 写裁剪距离 / 写剔除距离按已启用的设备特性守门（#1072 第 1 项）。
// 缺特性时 shader 照样装载（不向驱动交不合法 SPIR-V），建管线返回 UNSUPPORTED，原因里点名缺的特性；
// 负控：只读存储缓冲（NonWritable）不要 vertexPipelineStoresAndAtomics，缺它时管线照建。
// 设备真有这项特性：先把「已启用记录」清掉验拒绝，再恢复验正控；设备真没有（模拟机型档）：只验拒绝，且不该有 validation 报错。
static nvn_gpu_rc c1072a_get(const char *dir, const char *vs_name, int storage, nvn_gpu_pipe *out, nvn_gpu_err *e) {
    *out = NULL;
    nvn_gpu_layout_ent ent = {0};
    ent.cls = NVN_GPU_RES_SSBO; ent.access = storage == 2 ? NVN_GPU_ACC_R : NVN_GPU_ACC_W; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_VS;
    nvn_gpu_layout lay = {0}; lay.ent = &ent; lay.nent = 1;
    nvn_gpu_shader vs = load_one_shader(dir, vs_name, NVN_GPU_SH_VS, storage ? &lay : NULL);
    nvn_gpu_shader fs = load_one_shader(dir, "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_rc r = {NVN_GPU_E_ARG};
    if (vs && fs) { nvn_gpu_pipe_desc pd; fill_gpipe_desc(&pd, vs, fs, 0); memset(e, 0, sizeof *e); r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, out, e); }
    if (vs) nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    return r;
}
static void c1072a_case(const char *dir, const char *vs_name, int storage, VkBool32 *feature, const char *fname, int need) {
    const VkBool32 has = *feature;
    nvn_gpu_err e; nvn_gpu_pipe p = NULL;
    *feature = VK_FALSE;
    nvn_gpu_rc r = c1072a_get(dir, vs_name, storage, &p, &e);
    *feature = has;
    if (need) CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !p && strstr(e.why, fname), "C1072a %s 缺 %s 时管线应 UNSUPPORTED 且点名，实 %d「%s」", vs_name, fname, r.code, e.why);
    else CHECK(OK(r) && p, "C1072a 负控 %s 缺 %s 时管线照建：%d %s", vs_name, fname, r.code, e.why);
    if (!has) return;
    r = c1072a_get(dir, vs_name, storage, &p, &e);
    CHECK(OK(r) && p, "C1072a %s 有 %s 时管线应建成：%d %s", vs_name, fname, r.code, e.why);
}
static void c1072a_vertex_caps(const char *dir) {
    VkPhysicalDeviceFeatures *f = &g_dev.core_features;
    printf("C1072a 顶点阶段写存储 / 裁剪距离：设备 vertexPipelineStoresAndAtomics=%u shaderClipDistance=%u shaderCullDistance=%u\n",
           (unsigned)f->vertexPipelineStoresAndAtomics, (unsigned)f->shaderClipDistance, (unsigned)f->shaderCullDistance);
    c1072a_case(dir, "vs_store.vert", 1, &f->vertexPipelineStoresAndAtomics, "vertexPipelineStoresAndAtomics", 1);
    c1072a_case(dir, "vs_load.vert", 2, &f->vertexPipelineStoresAndAtomics, "vertexPipelineStoresAndAtomics", 0);
    c1072a_case(dir, "vs_clip.vert", 0, &f->shaderClipDistance, "shaderClipDistance", 1);
    c1072a_case(dir, "vs_cull.vert", 0, &f->shaderCullDistance, "shaderCullDistance", 1);
    printf("C1072a 缺特性时 UNSUPPORTED 并点名特性，只读存储负控照建\n");
}

// C1340：shaderInt16 按程序要求（#1340）。手造两份最小计算模块，只差一行 OpCapability Int16（外加一个 16 位整数类型）。
// 设备没有 shaderInt16（清掉「已启用记录」或模拟机型档真藏掉）：带 Int16 的模块装载即 UNSUPPORTED、why 点名 shaderInt16、
// 不交给驱动；不带的照常装、照常建计算管线。设备真有：恢复后带 Int16 的也照常建（validation 0 条）。
// 负对照：撤掉 nvn_gpu_shader_load 里的 Int16 检查，第一条断言变红。
static const uint32_t *c1340_module(int int16, size_t *bytes) {
    static uint32_t w[2][64];
    uint32_t *o = w[int16 ? 1 : 0];
    size_t k = 0;
    o[k++] = 0x07230203u; o[k++] = 0x00010000u; o[k++] = 0; o[k++] = 6; o[k++] = 0;   // 头：魔数 / 1.0 / 生成器 / bound / 0
    o[k++] = 2u << 16 | 17; o[k++] = 1;                                  // OpCapability Shader
    if (int16) { o[k++] = 2u << 16 | 17; o[k++] = 22; }                  // OpCapability Int16
    o[k++] = 3u << 16 | 14; o[k++] = 0; o[k++] = 1;                      // OpMemoryModel Logical GLSL450
    o[k++] = 5u << 16 | 15; o[k++] = 5; o[k++] = 1; o[k++] = 0x6e69616du; o[k++] = 0;   // OpEntryPoint GLCompute %1 "main"
    o[k++] = 6u << 16 | 16; o[k++] = 1; o[k++] = 17; o[k++] = 1; o[k++] = 1; o[k++] = 1;   // OpExecutionMode %1 LocalSize 1 1 1
    o[k++] = 2u << 16 | 19; o[k++] = 2;                                  // %2 = OpTypeVoid
    o[k++] = 3u << 16 | 33; o[k++] = 3; o[k++] = 2;                      // %3 = OpTypeFunction %2
    if (int16) { o[k++] = 4u << 16 | 21; o[k++] = 5; o[k++] = 16; o[k++] = 1; }   // %5 = OpTypeInt 16 1
    o[k++] = 5u << 16 | 54; o[k++] = 2; o[k++] = 1; o[k++] = 0; o[k++] = 3;   // %1 = OpFunction %2 None %3
    o[k++] = 2u << 16 | 248; o[k++] = 4;                                 // OpLabel
    o[k++] = 1u << 16 | 253;                                             // OpReturn
    o[k++] = 1u << 16 | 56;                                              // OpFunctionEnd
    *bytes = k * 4;
    return o;
}
static nvn_gpu_rc c1340_build(int int16, nvn_gpu_shader *sh, nvn_gpu_pipe *cp, nvn_gpu_err *e) {
    size_t bytes = 0;
    const uint32_t *m = c1340_module(int16, &bytes);
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main"; src.blob = m; src.bytes = bytes;
    *sh = NULL; *cp = NULL;
    memset(e, 0, sizeof *e);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, sh, e);
    if (!OK(r) || !*sh) return r;
    nvn_gpu_stage_desc cs;
    memset(&cs, 0, sizeof cs);
    cs.sh = *sh;
    return nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, cp, e);
}
static void c1340_int16_per_program(void) {
    size_t b16 = 0, b0 = 0;
    const uint32_t *m16 = c1340_module(1, &b16), *m0 = c1340_module(0, &b0);
    CHECK(s2a_spirv_declares_cap(m16, b16, 22) == 1, "C1340 扫描器没认出 OpCapability Int16");
    CHECK(s2a_spirv_declares_cap(m0, b0, 22) == 0, "C1340 扫描器把不带 Int16 的模块认成带");
    CHECK(s2a_spirv_declares_cap(m0, 16, 22) == -1, "C1340 扫描器对不完整的头应返回 -1");
    VkBool32 *f = &g_dev.core_features.shaderInt16;
    const VkBool32 has = *f;
    printf("C1340 16 位整数按程序要求：设备 shaderInt16=%u\n", (unsigned)has);
    nvn_gpu_shader sh; nvn_gpu_pipe cp; nvn_gpu_err e;
    *f = VK_FALSE;
    nvn_gpu_rc r = c1340_build(1, &sh, &cp, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !sh && !cp && strstr(e.why, "shaderInt16"),
          "C1340 缺 shaderInt16 时声明 Int16 的模块应 UNSUPPORTED 且点名，实 %d「%s」", r.code, e.why);
    if (sh) nvn_gpu_shader_release(sh);
    r = c1340_build(0, &sh, &cp, &e);
    CHECK(OK(r) && sh && cp, "C1340 缺 shaderInt16 时不声明 Int16 的模块应照常建：%d %s", r.code, e.why);
    if (sh) nvn_gpu_shader_release(sh);
    *f = has;
    if (has) {
        r = c1340_build(1, &sh, &cp, &e);
        CHECK(OK(r) && sh && cp, "C1340 有 shaderInt16 时声明 Int16 的模块应照常建：%d %s", r.code, e.why);
        if (sh) nvn_gpu_shader_release(sh);
    }
    printf("C1340 缺 shaderInt16 时声明 Int16 的模块具名拒收，不声明的照常建\n");
}

// C60b：Unknown 存储视图须按已启用的 without-format 写特性建管线。
static void c60b_unknown_image_tbuf(const char *dir) {
    VkBool32 enabled = g_dev.core_features.shaderStorageImageWriteWithoutFormat;
    CHECK(enabled, "C60b 宿主未启用 shaderStorageImageWriteWithoutFormat，无法验证正控");
    if (!enabled) return;
    nvn_gpu_layout_ent ent = {0};
    ent.cls = NVN_GPU_RES_IMAGE_TBUF; ent.access = NVN_GPU_ACC_W;
    ent.count = 1; ent.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = &ent; lay.nent = 1;
    char path[512]; size_t bytes = 0;
    snprintf(path, sizeof path, "%s/itbuf_unknown.comp.spv", dir);
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "C60b shader 文件缺失");
    if (!blob) return;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = blob; src.bytes = bytes; src.layout = &lay;
    nvn_gpu_err e; nvn_gpu_shader sh = NULL;
    g_dev.core_features.shaderStorageImageWriteWithoutFormat = VK_FALSE;
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60b 缺特性时 shader 仍须装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh;
        nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !cp, "C60b 缺写特性管线应 UNSUPPORTED，实 %d", r.code);
        nvn_gpu_shader_release(sh);
    }
    g_dev.core_features.shaderStorageImageWriteWithoutFormat = enabled;
    sh = NULL; r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60b 有特性时 shader 应装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(OK(r) && cp, "C60b 有写特性管线应成功：%d %s", r.code, e.why);
        if (cp) {
            const uint32_t expected[4] = {5, 6, 7, 8};
            nvn_gpu_buf b = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
            nvn_gpu_view v = NULL;
            r = nvn_gpu_view_create_buf(b, 0, sizeof expected, NVN_FORMAT_R32UI, &v, &e);
            CHECK(OK(r) && v, "C60b 视图创建失败：%d %s", r.code, e.why);
            if (v) {
                nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
                nvn_gpu_binding bind = {0}; bind.cls = NVN_GPU_RES_IMAGE_TBUF; bind.u.view = v;
                nvn_gpu_bind(c, NVN_GPU_SH_CS, &bind, 1);
                nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
                r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
                CHECK(OK(r), "C60b commit：%d %s", r.code, e.why);
                r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
                CHECK(OK(r), "C60b wait：%d %s", r.code, e.why);
                CHECK(!memcmp(buf_cpu(b), expected, sizeof expected), "C60b 写回不符");
                nvn_gpu_view_release(v);
            }
            nvn_gpu_buf_release(b, NULL, NULL);
        }
        nvn_gpu_shader_release(sh);
    }
    free(blob);
}

// C60c：Unknown 存储读取独立检查 read-without-format 特性并执行读回。
static void c60c_unknown_image_tbuf_read(const char *dir) {
    VkBool32 enabled = g_dev.core_features.shaderStorageImageReadWithoutFormat;
    CHECK(enabled, "C60c 宿主未启用 read-without-format，无法验证正控");
    if (!enabled) return;
    nvn_gpu_layout_ent ent[2] = {0};
    ent[0].cls = NVN_GPU_RES_IMAGE_TBUF; ent[0].access = NVN_GPU_ACC_R;
    ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W;
    ent[1].binding = 1; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ent; lay.nent = 2;
    char path[512]; size_t bytes = 0;
    snprintf(path, sizeof path, "%s/itbuf_unknown_read.comp.spv", dir);
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "C60c shader 文件缺失");
    if (!blob) return;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = blob; src.bytes = bytes; src.layout = &lay;
    nvn_gpu_err e; nvn_gpu_shader sh = NULL;
    g_dev.core_features.shaderStorageImageReadWithoutFormat = VK_FALSE;
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60c 缺特性时 shader 装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !cp, "C60c 缺读特性应 UNSUPPORTED：%d", r.code);
        nvn_gpu_shader_release(sh);
    }
    g_dev.core_features.shaderStorageImageReadWithoutFormat = enabled;
    sh = NULL; r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60c 有特性时 shader 装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(OK(r) && cp, "C60c 管线：%d %s", r.code, e.why);
        if (cp) {
            const uint32_t expected[4] = {19, 23, 29, 31};
            nvn_gpu_buf input = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, expected);
            nvn_gpu_buf output = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
            nvn_gpu_view v = NULL;
            r = nvn_gpu_view_create_buf(input, 0, sizeof expected, NVN_FORMAT_R32UI, &v, &e);
            CHECK(OK(r) && v, "C60c 视图：%d %s", r.code, e.why);
            if (v) {
                nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
                nvn_gpu_binding b[2] = {0};
                b[0].cls = NVN_GPU_RES_IMAGE_TBUF; b[0].u.view = v;
                b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = output; b[1].u.b.len = sizeof expected;
                nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2); nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
                r = nvn_gpu_cmd_commit(c, 110, on_done, NULL, &e);
                CHECK(OK(r), "C60c commit：%d %s", r.code, e.why);
                r = nvn_gpu_fence_wait(110, 5000000000ull, &e);
                CHECK(OK(r), "C60c wait：%d %s", r.code, e.why);
                CHECK(!memcmp(buf_cpu(output), expected, sizeof expected), "C60c 读回不符");
                nvn_gpu_view_release(v);
            }
            nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL);
        }
        nvn_gpu_shader_release(sh);
    }
    free(blob);
}

// C61：IMAGE（storage image）。uimage2D r32ui 4x4 imageStore(x,y)=x+4y+1 → 1..16。
static void c61_storage_image(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_IMAGE; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "img.comp", &cl, &sh);
    CHECK(cp != NULL, "C61 cpipe 为空");
    if (!cp) return;
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_R32UI, NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf rb = make_buf(16 * 4, NVN_GPU_MEM_SHARED, NULL);
    const uint32_t zeros[16] = {0};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, zeros, 16, 64);   // 上传后贴图落到 GENERAL，可进存储用途
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_IMAGE; b.u.view = v;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 108, on_done, NULL, &e);
    CHECK(OK(r), "C61 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(108, 5000000000ull, &e);
    CHECK(OK(r), "C61 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(rb);
    int bad = 0;
    for (int i = 0; i < 16; i++) if (rd[i] != (uint32_t)i + 1) bad++;
    CHECK(bad == 0, "C61 imageStore 读回错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C61s：sRGB 贴图按同位宽 UNORM 视图当存储图像写（NVN_GPU_CAP_STORAGE_SRGB_ALIAS）。RGBA8_SRGB 本身不能当存储：建图带存储用途时后端加
//   EXTENDED_USAGE，sRGB 默认视图剥掉存储用途（剥不掉校验层会报，run.sh 要求 0 条）；RGBA8 视图写 k/255 精确字节，读回逐字节相同
//   （被做了 sRGB 编码就不同：128/255 → 188）。设备不到 1.1 不置能力位，本用例按「没宣告」核：建图带存储用途要被拒。
static void c61s_storage_srgb_alias(const char *dir) {
    nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
    const int have = (cp.caps & NVN_GPU_CAP_STORAGE_SRGB_ALIAS) != 0;
    CHECK(!(nvn_gpu_format_caps(NVN_FORMAT_RGBA8_SRGB) & NVN_GPU_FMT_STORAGE) || !have, "C61s 前提：RGBA8_SRGB 本身不该报可存储（否则不需要别名）");
    if (!have) { printf("C61s 设备没宣告 STORAGE_SRGB_ALIAS（Vulkan < 1.1），sRGB 存储写不测\n"); return; }
    CHECK(nvn_gpu_format_caps(NVN_FORMAT_RGBA8) & NVN_GPU_FMT_STORAGE, "C61s 前提：RGBA8 能当存储");
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_IMAGE; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1; ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp2 = mk_cpipe(dir, "img_rgba8.comp", &cl, &sh);
    CHECK(cp2 != NULL, "C61s cpipe 为空");
    if (!cp2) return;
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGBA8_SRGB, NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT);
    if (t) {
        nvn_gpu_view vs = make_view(t);   // sRGB 默认视图（采样用）：带着存储用途就会被校验层拒
        view_t vd; memset(&vd, 0, sizeof vd); vd.set_format = 1; vd.format = NVN_FORMAT_RGBA8;
        nvn_gpu_view vu = NULL; nvn_gpu_err e;
        nvn_gpu_rc r = nvn_gpu_view_create(t, &vd, &vu, &e);
        CHECK(OK(r) && vu, "C61s RGBA8 视图：%d %s", r.code, e.why);
        nvn_gpu_buf rb = make_buf(16 * 4, NVN_GPU_MEM_SHARED, NULL);
        if (vu && rb) {
            const uint32_t zeros[16] = {0};
            nvn_gpu_cmd c = begin_cmd();
            nvn_gpu_tex_upload(c, t, 0, 0, NULL, zeros, 16, 64);
            nvn_gpu_set_pipe(c, cp2);
            nvn_gpu_binding b; memset(&b, 0, sizeof b);
            b.ent = 0; b.cls = NVN_GPU_RES_IMAGE; b.u.view = vu;
            nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
            nvn_gpu_dispatch(c, cp2, 1, 1, 1, NULL, 0);
            nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 16, 64);
            r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);   // 自己一个会话（见 main），围栏号从 1 起
            CHECK(OK(r), "C61s commit：%d %s", r.code, e.why);
            r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
            CHECK(OK(r), "C61s wait：%d %s", r.code, e.why);
            const uint8_t *rd = (const uint8_t *)buf_cpu(rb);
            int bad = 0;
            for (int k = 0; k < 16; k++) {
                const uint8_t want[4] = {(uint8_t)(k * 16), 128, (uint8_t)(255 - k), 255};
                if (memcmp(rd + 4 * k, want, 4)) { if (!bad) printf("C61s 像素 %d：期望 %u,%u,%u,%u → %u,%u,%u,%u\n", k, want[0], want[1], want[2], want[3], rd[4*k], rd[4*k+1], rd[4*k+2], rd[4*k+3]); bad++; }
            }
            CHECK(bad == 0, "C61s RGBA8 视图写进 sRGB 贴图：16 个像素期望逐字节相同（不做 sRGB 编码）→ %d 个不符", bad);
            if (!bad) printf("C61s sRGB 贴图按 UNORM 视图存储写：16 像素逐字节相同，已核对\n");
        }
        if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
        if (vu) nvn_gpu_view_release(vu);
        if (vs) nvn_gpu_view_release(vs);
        nvn_gpu_tex_release(t);
    }
    nvn_gpu_shader_release(sh);
}

// C62：TEX_SAMP 组合采样项走计算路径。texelFetch(1,0) = 红 {1,0,0,1} → 位模式 {3F800000,0,0,3F800000}。
static void c62_tsamp_cs(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX_SAMP; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_W;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = ents; cl.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tsamp.comp", &cl, &sh);
    CHECK(cp != NULL, "C62 cpipe 为空");
    if (!cp) return;
    static const uint8_t texels[16] = {0, 0, 0, 255,  255, 0, 0, 255,
                                       0, 0, 0, 255,  0, 0, 0, 255};
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v = make_view(t);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_err e;
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C62 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_buf out = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 8, 16);
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[2];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX_SAMP; bs[0].u.ts.view = v; bs[0].u.ts.samp = samp;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = out; bs[1].u.b.off = 0; bs[1].u.b.len = 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 2);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 109, on_done, NULL, &e);
    CHECK(OK(r), "C62 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(109, 5000000000ull, &e);
    CHECK(OK(r), "C62 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(out);
    CHECK(rd[0] == 0x3F800000u && rd[1] == 0 && rd[2] == 0 && rd[3] == 0x3F800000u,
          "C62 texelFetch = %08x %08x %08x %08x，期望 3f800000 0 0 3f800000", rd[0], rd[1], rd[2], rd[3]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(out, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// P0-6（差距表）：≥6 层的方形二维数组被着色器当立方体读。6 层 4x4 R32F，第 k 层每个纹素 = k；立方体视图（6 层），
// 按 +X -X +Y -Y +Z -Z 六个方向在面中心采样，期望读到面号 0 1 2 3 4 5（Vulkan 面序 = 层序）。
// 修前后端只给 CUBEMAP / CUBEMAP_ARRAY 目标建立方体兼容的图，这种视图被拒（「cube view requested for a non-cube-compatible image」）。
// 负对照：非方形（4x2）6 层二维数组不能当立方体，视图仍须被拒、不出句柄。
static void cp06_cube_of_array(const char *dir) {
    nvn_gpu_err e;
    nvn_gpu_tex t = make_tex_target(4, 4, 6, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_R32F, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex nsq = make_tex_target(4, 2, 6, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_R32F, NVN_GPU_TEX_SAMPLED);
    if (!t || !nsq) { if (t) nvn_gpu_tex_release(t); if (nsq) nvn_gpu_tex_release(nsq); return; }
    nvn_gpu_cmd c = begin_cmd();
    for (uint32_t k = 0; k < 6; k++) { float px[16]; for (int i = 0; i < 16; i++) px[i] = (float)k; nvn_gpu_tex_upload(c, t, 0, k, NULL, px, 16, 64); }
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r) && OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "P0-6 上传：%s", e.why);
    view_t vv; memset(&vv, 0, sizeof vv);
    vv.set_target = 1; vv.target = NVN_TEXTURE_TARGET_CUBEMAP; vv.set_layers = 1; vv.min_layer = 0; vv.nlayers = 6;
    nvn_gpu_view bad = NULL; memset(&e, 0, sizeof e);
    r = nvn_gpu_view_create(nsq, &vv, &bad, &e);
    CHECK(!OK(r) && !bad, "P0-6 非方形二维数组的立方体视图：期望被拒 → rc=%d 句柄=%p", r.code, (void *)bad);
    if (bad) nvn_gpu_view_release(bad);
    nvn_gpu_view v = NULL; memset(&e, 0, sizeof e);
    r = nvn_gpu_view_create(t, &vv, &v, &e);
    CHECK(OK(r) && v, "P0-6 6 层方形二维数组的立方体视图：期望建成 → rc=%d %s", r.code, e.why);
    if (v) {
        samp_par sp; memset(&sp, 0, sizeof sp); sp.ws = sp.wt = sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
        nvn_gpu_samp samp = NULL; CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "P0-6 采样器：%s", e.why);
        nvn_gpu_layout_ent ents[3]; memset(ents, 0, sizeof ents);
        ents[0].cls = NVN_GPU_RES_TEX_SAMP; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
        ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
        ents[2].cls = NVN_GPU_RES_SSBO; ents[2].access = NVN_GPU_ACC_W; ents[2].binding = 2; ents[2].count = 1; ents[2].stages = 1u << NVN_GPU_SH_CS;
        nvn_gpu_layout cl; memset(&cl, 0, sizeof cl); cl.ent = ents; cl.nent = 3;
        nvn_gpu_shader sh = NULL; nvn_gpu_pipe cp = mk_cpipe(dir, "cube_at.comp", &cl, &sh);
        static const float dirs[6][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}, {0, 1, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
        nvn_gpu_buf in = make_buf(sizeof dirs, NVN_GPU_MEM_SHARED, dirs), out = make_buf(6 * 16, NVN_GPU_MEM_SHARED, NULL);
        if (cp && samp) {
            c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
            nvn_gpu_binding bs[3]; memset(bs, 0, sizeof bs);
            bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX_SAMP; bs[0].u.ts.view = v; bs[0].u.ts.samp = samp;
            bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = in; bs[1].u.b.len = sizeof dirs;
            bs[2].ent = 2; bs[2].cls = NVN_GPU_RES_SSBO; bs[2].u.b.buf = out; bs[2].u.b.len = 6 * 16;
            nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 3); nvn_gpu_dispatch(c, cp, 6, 1, 1, NULL, 0);
            r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
            CHECK(OK(r) && OK(nvn_gpu_fence_wait(2, 5000000000ull, &e)), "P0-6 采样：%s", e.why);
            if (OK(r)) {
                const float (*got)[4] = (const float (*)[4])(const void *)buf_cpu(out); int badf = 0;
                for (int f = 0; f < 6; f++) if (got[f][0] != (float)f) badf++;
                CHECK(!badf, "P0-6 立方体视图 ±X ±Y ±Z：期望面号 0 1 2 3 4 5 → 读到 %g %g %g %g %g %g", got[0][0], got[1][0], got[2][0], got[3][0], got[4][0], got[5][0]);
                printf("P0-6 二维数组当立方体读：±X ±Y ±Z 期望 0 1 2 3 4 5 → 读到 %g %g %g %g %g %g\n", got[0][0], got[1][0], got[2][0], got[3][0], got[4][0], got[5][0]);
            }
        }
        nvn_gpu_buf_release(in, NULL, NULL); nvn_gpu_buf_release(out, NULL, NULL);
        if (sh) nvn_gpu_shader_release(sh);
        if (samp) nvn_gpu_samp_release(samp);
        nvn_gpu_view_release(v);
    }
    nvn_gpu_tex_release(t); nvn_gpu_tex_release(nsq);
}

// #976 采样器用例的公共部分：shader（samp_at*.comp）按 uv[i] 在第 0 级采样，读回 n 个 16 字节结果（vec4 / ivec4 / uvec4）。成功返回 1。
static int samp_at_sh(const char *dir, const char *shader, nvn_gpu_tex t, nvn_gpu_samp samp, const float (*uv)[2], int n, void *got, uint64_t signal) {
    nvn_gpu_layout_ent ents[3];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX_SAMP; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    ents[2].set = 0; ents[2].cls = NVN_GPU_RES_SSBO; ents[2].access = NVN_GPU_ACC_W; ents[2].binding = 2; ents[2].count = 1; ents[2].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl; memset(&cl, 0, sizeof cl); cl.ent = ents; cl.nent = 3;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, shader, &cl, &sh);
    CHECK(cp != NULL, "samp_at cpipe 为空");
    if (!cp) return 0;
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf in = make_buf((uint64_t)n * 8, NVN_GPU_MEM_SHARED, uv);
    nvn_gpu_buf out = make_buf((uint64_t)n * 16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[3];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX_SAMP; bs[0].u.ts.view = v; bs[0].u.ts.samp = samp;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = in; bs[1].u.b.len = (uint64_t)n * 8;
    bs[2].ent = 2; bs[2].cls = NVN_GPU_RES_SSBO; bs[2].u.b.buf = out; bs[2].u.b.len = (uint64_t)n * 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 3);
    nvn_gpu_dispatch(c, cp, (uint32_t)n, 1, 1, NULL, 0);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, signal, on_done, NULL, &e);
    CHECK(OK(r), "samp_at commit：%d %s", r.code, e.why);
    int ok = OK(r) && OK(nvn_gpu_fence_wait(signal, 5000000000ull, &e));
    CHECK(ok, "samp_at wait：%s", e.why);
    if (ok) memcpy(got, buf_cpu(out), (size_t)n * 16);
    nvn_gpu_view_release(v);
    nvn_gpu_buf_release(in, NULL, NULL); nvn_gpu_buf_release(out, NULL, NULL);
    nvn_gpu_shader_release(sh);
    return ok;
}
static int samp_at(const char *dir, nvn_gpu_tex t, nvn_gpu_samp samp, const float (*uv)[2], int n, float (*got)[4], uint64_t signal) {
    return samp_at_sh(dir, "samp_at.comp", t, samp, uv, n, got, signal);
}

// C976m：镜像夹取环绕（#976）。4x1 R32F 纹素 {0,1,2,3}、最近点，读到的 r 就是纹素下标。
// 期望按 Vulkan 规范的纹素选取手算（i = floor(u*4)；镜像 mirror(a) = a >= 0 ? a : -(1+a)）：
//   MIRROR_CLAMP_TO_EDGE：i' = clamp(mirror(i), 0, 3)。u = 0.625 / -0.375 / -1.5 / 1.25 / -2.25 → 2 1 3 3 3
//   （后三个落在纹素边界上，但两侧都夹到 3，期望不受舍入影响）。
//   修前映成 MIRRORED_REPEAT：lavapipe 实测 u=-1.5 读到 2、u=-2.25 读到 1，红在这两处。
// Vulkan 没有的 MIRROR_CLAMP / MIRROR_CLAMP_TO_BORDER：不论设备都回具名 UNSUPPORTED、不出句柄（近似由核心做、核心记账）。
// 设备没有 VK_KHR_sampler_mirror_clamp_to_edge（模拟档 no_mirror_clamp；有扩展时本用例自己把启用位清零再问一次）：MIRROR_CLAMP_TO_EDGE 也回具名 UNSUPPORTED。
#define C976_MCE_ENABLED g_dev.sampler_mirror_clamp_enabled
static void c976m_mirror_clamp(const char *dir) {
    printf("C976m 镜像夹取：设备 mirror_clamp_to_edge=%d\n", C976_MCE_ENABLED);
    static const struct { uint8_t axis, wrap; const char *why; } no_vk[2] = {
        {0, NVN_WRAP_MODE_MIRROR_CLAMP, "sampler_wrap_mirror_clamp："}, {1, NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER, "sampler_wrap_mirror_clamp_to_border："}};
    nvn_gpu_err e;
    for (int k = 0; k < 2; k++) {
        samp_par sp; memset(&sp, 0, sizeof sp);
        if (no_vk[k].axis == 0) sp.ws = no_vk[k].wrap; else sp.wt = no_vk[k].wrap;
        nvn_gpu_samp bad = NULL; memset(&e, 0, sizeof e);
        nvn_gpu_rc r = nvn_gpu_samp_create(&sp, &bad, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !bad && !strncmp(e.why, no_vk[k].why, strlen(no_vk[k].why)),
              "C976m 环绕 %u：期望 UNSUPPORTED「%s…」→ rc=%d 句柄=%p why=%s", no_vk[k].wrap, no_vk[k].why, r.code, (void *)bad, e.why);
        if (bad) nvn_gpu_samp_release(bad);
    }
    samp_par sp; memset(&sp, 0, sizeof sp);   // 最近点
    sp.ws = NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE; sp.wt = NVN_WRAP_MODE_CLAMP_TO_EDGE; sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
    const int had = C976_MCE_ENABLED;
    for (int pass = had ? 0 : 1; pass < 2; pass++) {
        if (pass == 1) C976_MCE_ENABLED = 0;
        nvn_gpu_samp samp = NULL; memset(&e, 0, sizeof e);
        nvn_gpu_rc r = nvn_gpu_samp_create(&sp, &samp, &e);
        if (pass == 1) {
            C976_MCE_ENABLED = had;
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !samp && !strncmp(e.why, "sampler_mirror_clamp_to_edge：", strlen("sampler_mirror_clamp_to_edge：")),
                  "C976m 没有扩展：期望 UNSUPPORTED「sampler_mirror_clamp_to_edge：…」→ rc=%d why=%s", r.code, e.why);
            if (samp) nvn_gpu_samp_release(samp);
            printf("C976m 没有扩展：MIRROR_CLAMP_TO_EDGE 建采样器 rc=%d（%s）\n", r.code, e.why);
            break;
        }
        CHECK(OK(r) && samp, "C976m MIRROR_CLAMP_TO_EDGE 建采样器：%d %s", r.code, e.why);
        if (!samp) continue;
        static const float texels[4] = {0.0f, 1.0f, 2.0f, 3.0f};
        static const float uv[5][2] = {{0.625f, 0.5f}, {-0.375f, 0.5f}, {-1.5f, 0.5f}, {1.25f, 0.5f}, {-2.25f, 0.5f}};
        static const float want[5] = {2, 1, 3, 3, 3};
        nvn_gpu_tex t = make_tex(4, 1, NVN_FORMAT_R32F, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 16, 16);
        r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        CHECK(OK(r) && OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C976m 上传：%s", e.why);
        float got[5][4] = {{0}};
        if (samp_at(dir, t, samp, uv, 5, got, 2))
            for (int i = 0; i < 5; i++)
                CHECK(got[i][0] == want[i], "C976m MIRROR_CLAMP_TO_EDGE u=%.3f：期望下标 %.0f → 读到 %g", uv[i][0], want[i], got[i][0]);
        printf("C976m MIRROR_CLAMP_TO_EDGE：u=0.625/-0.375/-1.5/1.25/-2.25 期望 2 1 3 3 3 → 读到 %g %g %g %g %g\n", got[0][0], got[1][0], got[2][0], got[3][0], got[4][0]);
        nvn_gpu_tex_release(t);
        nvn_gpu_samp_release(samp);
    }
}

// C976b：自定义边框色（#976）。最近点、U 向 CLAMP_TO_BORDER，在 u = -0.5 / 1.5 处读到的必须正好是边框色，u = 0.125 读纹素 0。
// 不是内置三档的边框色要 VK_EXT_custom_border_color（且 customBorderColorWithoutFormat：建采样器时不知道视图格式）；
// 没有扩展时按 RGBA 距离就近取内置三档建成（nvn_vk_border.h，不丢绘制）；有扩展缺 WithoutFormat 回具名 UNSUPPORTED、不出句柄。修前一律映成不透明黑 (0,0,0,1)，红在读值上。
// 整数视图走另一个采样器（int_sampler），边框色按数值取整成 INT_CUSTOM；无符号的大值按位存。
// 内置三档、没有一向用到边框的采样器不需要扩展、不占配额。配额：sampler 与 int_sampler 各占一个，到上限回具名 UNSUPPORTED，释放后归还。
// 先跑不绑定的部分（配额计数不受在途命令影响），再跑读值。围栏号 1..6。
#define C976_CB_ENABLED g_dev.custom_border_enabled
#define C976_CB_NOFMT g_dev.custom_border_without_format
#define C976_CB_MAX g_dev.custom_border_max
static nvn_gpu_samp c976b_samp(const float border[4], uint8_t ws, nvn_gpu_err *e, nvn_gpu_rc *r) {
    samp_par sp; memset(&sp, 0, sizeof sp);   // 最近点
    sp.ws = ws; sp.wt = NVN_WRAP_MODE_CLAMP_TO_EDGE; sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
    memcpy(sp.border, border, sizeof sp.border);
    nvn_gpu_samp s = NULL; memset(e, 0, sizeof *e);
    *r = nvn_gpu_samp_create(&sp, &s, e);
    return s;
}
static int c976b_why(const nvn_gpu_err *e, const char *want) { return !strncmp(e->why, want, strlen(want)); }
static void c976b_nearest_cb(void *user, const char *name, uint64_t v) { if (!strcmp(name, "sampler_border_color_nearest")) *(uint64_t *)user = v; }
static uint64_t c976b_nearest(void) { uint64_t v = 0; nvn_gpu_stats(c976b_nearest_cb, &v, 0); return v; }
static void c976b_border_color(const char *dir) {
    printf("C976b 自定义边框色：设备 custom_border=%d without_format=%d max=%u\n", C976_CB_ENABLED, C976_CB_NOFMT, C976_CB_MAX);
    static const float odd[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    nvn_gpu_err e; nvn_gpu_rc r;
    const int had = C976_CB_ENABLED, had_nofmt = C976_CB_NOFMT;
    const uint32_t had_max = C976_CB_MAX;
    // 没有一向用到边框：不论有没有扩展都能建
    C976_CB_ENABLED = 0; C976_CB_NOFMT = 0;
    nvn_gpu_samp s = c976b_samp(odd, NVN_WRAP_MODE_CLAMP_TO_EDGE, &e, &r);
    CHECK(OK(r) && s, "C976b 不用边框的采样器（清掉扩展位）：期望建成 → rc=%d why=%s", r.code, e.why);
    if (s) nvn_gpu_samp_release(s);
    // 内置三档：不要扩展
    static const float fixed[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    for (int k = 0; k < 3; k++) {
        s = c976b_samp(fixed[k], NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
        CHECK(OK(r) && s, "C976b 内置边框色 %g %g %g %g（清掉扩展位）：期望建成 → rc=%d why=%s",
              fixed[k][0], fixed[k][1], fixed[k][2], fixed[k][3], r.code, e.why);
        if (s) nvn_gpu_samp_release(s);
    }
    // 没有扩展：就近建成，具名计数 sampler_border_color_nearest 按建成次数 +1（上面内置三档、不用边框的不计）
    const uint64_t nb0 = c976b_nearest();
    s = c976b_samp(odd, NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
    CHECK(OK(r) && s, "C976b 没有扩展：期望就近取内置档建成（不丢绘制）→ rc=%d 句柄=%p why=%s", r.code, (void *)s, e.why);
    CHECK(c976b_nearest() == nb0 + 1, "C976b 没有扩展：sampler_border_color_nearest 期望 +1 → %llu → %llu",
          (unsigned long long)nb0, (unsigned long long)c976b_nearest());
    printf("C976b 没有扩展：自定义边框色就近建采样器 rc=%d\n", r.code);
    if (s) nvn_gpu_samp_release(s);
    C976_CB_ENABLED = had; C976_CB_NOFMT = had_nofmt;
    if (!had) { printf("C976b 设备没有 VK_EXT_custom_border_color：其余部分跳过\n"); return; }
    // 有扩展、没有 customBorderColorWithoutFormat
    C976_CB_NOFMT = 0;
    s = c976b_samp(odd, NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
    C976_CB_NOFMT = had_nofmt;
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !s && c976b_why(&e, "sampler_border_color_custom_format："),
          "C976b 没有 WithoutFormat：期望 UNSUPPORTED「sampler_border_color_custom_format：…」→ rc=%d 句柄=%p why=%s", r.code, (void *)s, e.why);
    if (s) nvn_gpu_samp_release(s);
    if (!had_nofmt) { printf("C976b 设备没有 customBorderColorWithoutFormat：其余部分跳过\n"); return; }
    // 配额：上限设成 4 = 两个自定义边框色采样器
    C976_CB_MAX = 4;
    nvn_gpu_samp q[3] = {NULL, NULL, NULL};
    nvn_gpu_rc qr[3];
    nvn_gpu_err qe[3];
    for (int k = 0; k < 3; k++) q[k] = c976b_samp(odd, NVN_WRAP_MODE_CLAMP_TO_BORDER, &qe[k], &qr[k]);
    CHECK(OK(qr[0]) && q[0] && OK(qr[1]) && q[1], "C976b 配额 4：前两个期望建成 → rc=%d/%d why=%s / %s", qr[0].code, qr[1].code, qe[0].why, qe[1].why);
    CHECK(qr[2].code == NVN_GPU_E_UNSUPPORTED && !q[2] && c976b_why(&qe[2], "sampler_border_color_custom_quota："),
          "C976b 配额 4：第三个期望 UNSUPPORTED「sampler_border_color_custom_quota：…」→ rc=%d why=%s", qr[2].code, qe[2].why);
    s = c976b_samp(fixed[2], NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
    CHECK(OK(r) && s, "C976b 配额满时内置边框色：期望建成（不占配额）→ rc=%d why=%s", r.code, e.why);
    if (s) nvn_gpu_samp_release(s);
    if (q[0]) nvn_gpu_samp_release(q[0]);
    q[0] = c976b_samp(odd, NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
    CHECK(OK(r) && q[0], "C976b 配额：释放一个后期望能再建 → rc=%d why=%s", r.code, e.why);
    for (int k = 0; k < 3; k++) if (q[k]) nvn_gpu_samp_release(q[k]);
    C976_CB_MAX = had_max;
    // 读值
    static const float uv[3][2] = {{-0.5f, 0.5f}, {0.125f, 0.5f}, {1.5f, 0.5f}};
    {   // 浮点：RGBA32F 纹素 i = (i, i+10, i+20, i+30)
        static const float texels[4][4] = {{0, 10, 20, 30}, {1, 11, 21, 31}, {2, 12, 22, 32}, {3, 13, 23, 33}};
        s = c976b_samp(odd, NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
        CHECK(OK(r) && s, "C976b 浮点自定义边框色建采样器：%d %s", r.code, e.why);
        nvn_gpu_tex t = make_tex(4, 1, NVN_FORMAT_RGBA32F, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 64, 64);
        r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        CHECK(OK(r) && OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C976b 浮点上传：%s", e.why);
        float got[3][4] = {{0}};
        if (s && samp_at_sh(dir, "samp_at.comp", t, s, uv, 3, got, 2)) {
            for (int i = 0; i < 3; i += 2)
                CHECK(!memcmp(got[i], odd, 16), "C976b 浮点 u=%.3f：期望边框色 0.2 0.4 0.6 0.8 → 读到 %g %g %g %g",
                      uv[i][0], got[i][0], got[i][1], got[i][2], got[i][3]);
            CHECK(!memcmp(got[1], texels[0], 16), "C976b 浮点 u=0.125：期望纹素 0 → 读到 %g %g %g %g", got[1][0], got[1][1], got[1][2], got[1][3]);
        }
        printf("C976b 浮点 RGBA32F 边框色 (0.2,0.4,0.6,0.8)：u=-0.5 读到 (%g,%g,%g,%g)，u=1.5 读到 (%g,%g,%g,%g)\n",
               got[0][0], got[0][1], got[0][2], got[0][3], got[2][0], got[2][1], got[2][2], got[2][3]);
        nvn_gpu_tex_release(t);
        if (s) nvn_gpu_samp_release(s);
    }
    {   // 有符号整数：RGBA32I，边框色 (-3, 7, 100000, 1)
        static const float ib[4] = {-3.0f, 7.0f, 100000.0f, 1.0f};
        static const int32_t want[4] = {-3, 7, 100000, 1};
        static const int32_t texels[4][4] = {{0, -1, 2, 5}, {1, -2, 3, 5}, {2, -3, 4, 5}, {3, -4, 5, 5}};
        s = c976b_samp(ib, NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
        CHECK(OK(r) && s, "C976b 整数自定义边框色建采样器：%d %s", r.code, e.why);
        nvn_gpu_tex t = make_tex(4, 1, NVN_FORMAT_RGBA32I, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 64, 64);
        r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
        CHECK(OK(r) && OK(nvn_gpu_fence_wait(3, 5000000000ull, &e)), "C976b 整数上传：%s", e.why);
        int32_t got[3][4] = {{0}};
        if (s && samp_at_sh(dir, "samp_at_i.comp", t, s, uv, 3, got, 4)) {
            for (int i = 0; i < 3; i += 2)
                CHECK(!memcmp(got[i], want, 16), "C976b RGBA32I u=%.3f：期望边框色 -3 7 100000 1 → 读到 %d %d %d %d",
                      uv[i][0], got[i][0], got[i][1], got[i][2], got[i][3]);
            CHECK(!memcmp(got[1], texels[0], 16), "C976b RGBA32I u=0.125：期望纹素 0 → 读到 %d %d %d %d", got[1][0], got[1][1], got[1][2], got[1][3]);
        }
        printf("C976b RGBA32I 边框色 (-3,7,100000,1)：u=-0.5 读到 (%d,%d,%d,%d)\n", got[0][0], got[0][1], got[0][2], got[0][3]);
        nvn_gpu_tex_release(t);
        if (s) nvn_gpu_samp_release(s);
    }
    {   // 无符号整数：RGBA32UI，边框色 (3000000000, 2, 0, 9)（3e9 在 float 里正好可表示，超过 INT32_MAX）
        static const float ub[4] = {3000000000.0f, 2.0f, 0.0f, 9.0f};
        static const uint32_t want[4] = {3000000000u, 2, 0, 9};
        static const uint32_t texels[4][4] = {{7, 0, 0, 1}, {8, 0, 0, 1}, {9, 0, 0, 1}, {10, 0, 0, 1}};
        s = c976b_samp(ub, NVN_WRAP_MODE_CLAMP_TO_BORDER, &e, &r);
        CHECK(OK(r) && s, "C976b 无符号自定义边框色建采样器：%d %s", r.code, e.why);
        nvn_gpu_tex t = make_tex(4, 1, NVN_FORMAT_RGBA32UI, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 64, 64);
        r = nvn_gpu_cmd_commit(c, 5, on_done, NULL, &e);
        CHECK(OK(r) && OK(nvn_gpu_fence_wait(5, 5000000000ull, &e)), "C976b 无符号上传：%s", e.why);
        uint32_t got[3][4] = {{0}};
        if (s && samp_at_sh(dir, "samp_at_u.comp", t, s, uv, 3, got, 6)) {
            for (int i = 0; i < 3; i += 2)
                CHECK(!memcmp(got[i], want, 16), "C976b RGBA32UI u=%.3f：期望边框色 3000000000 2 0 9 → 读到 %u %u %u %u",
                      uv[i][0], got[i][0], got[i][1], got[i][2], got[i][3]);
            CHECK(!memcmp(got[1], texels[0], 16), "C976b RGBA32UI u=0.125：期望纹素 0 → 读到 %u %u %u %u", got[1][0], got[1][1], got[1][2], got[1][3]);
        }
        printf("C976b RGBA32UI 边框色 (3000000000,2,0,9)：u=-0.5 读到 (%u,%u,%u,%u)\n", got[0][0], got[0][1], got[0][2], got[0][3]);
        nvn_gpu_tex_release(t);
        if (s) nvn_gpu_samp_release(s);
    }
}

// C976r：min / max 归约（#976）。2x1 R32F 纹素 {0.25, 0.75}，线性过滤、夹到边，在两纹素中心的中点 u = 0.5 采样：
// 两个纹素权重各一半。期望：平均 0.5、min 0.25、max 0.75。修前后端不读 reduction，三种都读到 0.5。
static void c976r_reduction(const char *dir) {
    printf("C976r 归约：设备 sampler_filter_minmax=%d image_mapping=%d\n", g_dev.sampler_minmax_enabled, g_dev.minmax_image_mapping);
    static const float texels[2] = {0.25f, 0.75f};
    static const float uv[1][2] = {{0.5f, 0.5f}};
    static const struct { uint32_t red; const char *name; float want; } k[3] = {
        {NVN_SAMPLER_REDUCTION_AVERAGE, "平均", 0.5f}, {NVN_SAMPLER_REDUCTION_MIN, "min", 0.25f}, {NVN_SAMPLER_REDUCTION_MAX, "max", 0.75f}};
    nvn_gpu_err e;
    nvn_gpu_tex t = make_tex(2, 1, NVN_FORMAT_R32F, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 8, 8);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r) && OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C976r 上传：%s", e.why);
    uint64_t signal = 1;
    for (int i = 0; i < 3; i++) {
        samp_par sp; memset(&sp, 0, sizeof sp);
        sp.minf = NVN_MIN_FILTER_LINEAR; sp.magf = NVN_MAG_FILTER_LINEAR;
        sp.ws = sp.wt = sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE; sp.reduction = k[i].red;
        nvn_gpu_samp samp = NULL; memset(&e, 0, sizeof e);
        if (i) {   // 没有扩展（模拟档 no_minmax；有扩展时本用例把启用位清零再问一次）：min / max 必须具名 UNSUPPORTED，不能静默退回平均
            const int had = g_dev.sampler_minmax_enabled;
            char want[48]; snprintf(want, sizeof want, "sampler_reduction_%s：", k[i].name);
            g_dev.sampler_minmax_enabled = 0;
            r = nvn_gpu_samp_create(&sp, &samp, &e);
            g_dev.sampler_minmax_enabled = had;
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !samp && !strncmp(e.why, want, strlen(want)),
                  "C976r 没有扩展：%s 期望 UNSUPPORTED「%s…」→ rc=%d why=%s", k[i].name, want, r.code, e.why);
            printf("C976r 没有扩展：%s 归约建采样器 rc=%d（%s）\n", k[i].name, r.code, e.why);
            if (samp) { nvn_gpu_samp_release(samp); samp = NULL; }
            if (!had) continue;
            // 缺完整映射时，退回平均并具名记一次；实际读取必须等于平均值。
            const int mapping = g_dev.minmax_image_mapping;
            uint64_t before = nvn_unimpl_count("gpu", "sampler_reduction_average_no_component_mapping");
            g_dev.minmax_image_mapping = 0;
            memset(&e, 0, sizeof e);
            r = nvn_gpu_samp_create(&sp, &samp, &e);
            g_dev.minmax_image_mapping = mapping;
            CHECK(OK(r) && samp, "C976r %s 缺多分量映射：期望平均采样器建成 → rc=%d why=%s", k[i].name, r.code, e.why);
            CHECK(nvn_unimpl_count("gpu", "sampler_reduction_average_no_component_mapping") == before + 1,
                  "C976r 缺多分量映射：期望具名降级记录一次");
            if (samp) {
                float avg[1][4] = {{-1, -1, -1, -1}};
                if (samp_at(dir, t, samp, uv, 1, avg, ++signal)) {
                    CHECK(fabsf(avg[0][0] - 0.5f) < 1e-3f, "C976r 缺多分量映射：期望平均 0.5 → %g", avg[0][0]);
                    printf("C976r 缺多分量映射 %s：平均 0.5 → %g\n", k[i].name, avg[0][0]);
                }
                nvn_gpu_samp_release(samp); samp = NULL;
            }
            if (!mapping) continue;
            memset(&e, 0, sizeof e);
        }
        r = nvn_gpu_samp_create(&sp, &samp, &e);
        CHECK(OK(r) && samp, "C976r %s 建采样器：%d %s", k[i].name, r.code, e.why);
        if (!samp) continue;
        float got[1][4] = {{-1, -1, -1, -1}};
        if (samp_at(dir, t, samp, uv, 1, got, ++signal)) {
            CHECK(got[0][0] > k[i].want - 1e-3f && got[0][0] < k[i].want + 1e-3f, "C976r %s：中点线性采样 期望 %.2f → 读到 %g", k[i].name, k[i].want, got[0][0]);
            printf("C976r 归约 %s：期望 %.2f → 读到 %g\n", k[i].name, k[i].want, got[0][0]);
        }
        nvn_gpu_samp_release(samp);
    }
    nvn_gpu_tex_release(t);
    // 即使本机没有支持归约的四分量格式，也可让校验层检查两个扩展结构同时建采样器。
    if (g_dev.sampler_minmax_enabled && g_dev.minmax_image_mapping && g_dev.custom_border_enabled &&
        g_dev.custom_border_without_format && g_dev.custom_border_max >= 2) {
        samp_par both = {0}; both.minf = NVN_MIN_FILTER_LINEAR; both.magf = NVN_MAG_FILTER_LINEAR;
        both.ws = NVN_WRAP_MODE_CLAMP_TO_BORDER; both.wt = both.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
        both.reduction = NVN_SAMPLER_REDUCTION_MIN;
        both.border[0] = 0.2f; both.border[1] = 0.4f; both.border[2] = 0.6f; both.border[3] = 0.8f;
        nvn_gpu_samp sampler = NULL; memset(&e, 0, sizeof e);
        r = nvn_gpu_samp_create(&both, &sampler, &e);
        CHECK(OK(r) && sampler, "C976r 双扩展采样器：期望建成 → rc=%d why=%s", r.code, e.why);
        printf("C976r 双扩展采样器：rc=%d\n", r.code);
        if (sampler) nvn_gpu_samp_release(sampler);
    }
    // 两个扩展同时使用：浮点采样器必须同时携带自定义边框和 min 归约；整数采样器保留自定义边框。
    VkFormatProperties fp = {0}; vkGetPhysicalDeviceFormatProperties(g_dev.physical, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
    if (g_dev.sampler_minmax_enabled && g_dev.minmax_image_mapping && g_dev.custom_border_enabled &&
        g_dev.custom_border_without_format && g_dev.custom_border_max >= 2 &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT_EXT)) {
        const float pixels[2][4] = {{0.25f, 0.25f, 0.25f, 0.25f}, {0.75f, 0.75f, 0.75f, 0.75f}};
        const float coords[2][2] = {{-0.5f, 0.5f}, {0.5f, 0.5f}};
        const float border[4] = {0.2f, 0.4f, 0.6f, 0.8f};
        samp_par sp; memset(&sp, 0, sizeof sp);
        sp.minf = NVN_MIN_FILTER_LINEAR; sp.magf = NVN_MAG_FILTER_LINEAR;
        sp.ws = NVN_WRAP_MODE_CLAMP_TO_BORDER; sp.wt = sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
        sp.reduction = NVN_SAMPLER_REDUCTION_MIN; memcpy(sp.border, border, sizeof border);
        nvn_gpu_samp samp = NULL; memset(&e, 0, sizeof e);
        r = nvn_gpu_samp_create(&sp, &samp, &e);
        CHECK(OK(r) && samp, "C976r 自定义边框 + min：建采样器 %d %s", r.code, e.why);
        nvn_gpu_tex ct = make_tex(2, 1, NVN_FORMAT_RGBA32F, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        c = begin_cmd(); nvn_gpu_tex_upload(c, ct, 0, 0, NULL, pixels, 32, 32);
        r = nvn_gpu_cmd_commit(c, ++signal, on_done, NULL, &e);
        CHECK(OK(r) && OK(nvn_gpu_fence_wait(signal, 5000000000ull, &e)), "C976r 组合上传：%s", e.why);
        float got[2][4] = {{0}};
        if (samp && samp_at(dir, ct, samp, coords, 2, got, ++signal)) {
            for (int j = 0; j < 4; j++) {
                CHECK(fabsf(got[0][j] - border[j]) < 1e-3f, "C976r 边框 + min：边框分量 %d 期望 %g → %g", j, border[j], got[0][j]);
                CHECK(fabsf(got[1][j] - 0.25f) < 1e-3f, "C976r 边框 + min：中点分量 %d 期望 0.25 → %g", j, got[1][j]);
            }
            printf("C976r 边框 + min：边框 R=%g，中点 R=%g\n", got[0][0], got[1][0]);
        }
        if (samp) nvn_gpu_samp_release(samp);
        nvn_gpu_tex_release(ct);
    } else printf("C976r 边框 + min：设备缺所需能力，组合像素未测\n");
    // 分离图像 / 采样器：配对格式未知，绘制必须实际走平均采样并只记一次降级。
    if (g_dev.sampler_minmax_enabled && g_dev.minmax_image_mapping) {
        nvn_gpu_layout_ent ents[2] = {{0}};
        ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 2; ents[0].stages = 1u << NVN_GPU_SH_FS;
        ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
        nvn_gpu_layout fl = {0}; fl.ent = ents; fl.nent = 2;
        nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
        if (load_shader_pair(dir, "tri3.vert", "arr2.frag", NULL, &fl, &vs, &fs, &vm, &fm)) {
            nvn_gpu_pipe_desc pd; fill_gpipe_desc(&pd, vs, fs, 0);
            nvn_gpu_pipe pipe = NULL; memset(&e, 0, sizeof e);
            r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
            CHECK(OK(r) && pipe, "C976r 分离采样：管线 %d %s", r.code, e.why);
            samp_par sp = {0}; sp.minf = NVN_MIN_FILTER_LINEAR; sp.magf = NVN_MAG_FILTER_LINEAR;
            sp.ws = sp.wt = sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE; sp.reduction = NVN_SAMPLER_REDUCTION_MIN;
            nvn_gpu_samp samp = NULL; r = nvn_gpu_samp_create(&sp, &samp, &e);
            CHECK(OK(r) && samp, "C976r 分离采样：建采样器 %d %s", r.code, e.why);
            const uint8_t sep_texels[16] = {64,0,0,255, 192,0,0,255, 64,0,0,255, 192,0,0,255};
            nvn_gpu_tex tex = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
            nvn_gpu_view view = make_view(tex);
            nvn_gpu_tex tex0 = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
            nvn_gpu_view view0 = make_view(tex0);
            nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
            nvn_gpu_view ov = make_view(out);
            nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL), vb = full_vb();
            if (pipe && samp && view && view0 && ov && rb && vb) {
                uint64_t before = nvn_unimpl_count("gpu", "sampler_reduce_average_separate_unknown_format");
                c = begin_cmd(); nvn_gpu_tex_upload(c, tex, 0, 0, NULL, sep_texels, 8, 16);
                nvn_gpu_tex_upload(c, tex0, 0, 0, NULL, sep_texels, 8, 16);
                nvn_gpu_pass_desc pass = {0};
                pass.ncolor = 1; pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
                pass.w = 64; pass.h = 64; pass.layers = 1;
                nvn_gpu_pass_begin(c, &pass); nvn_gpu_set_pipe(c, pipe);
                nvn_gpu_dyn dyn; full_dyn(&dyn); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
                nvn_gpu_binding bs[3] = {{0}};
                bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX; bs[0].u.view = view0;
                bs[1].ent = 0; bs[1].elem = 1; bs[1].cls = NVN_GPU_RES_TEX; bs[1].u.view = view;
                bs[2].ent = 1; bs[2].cls = NVN_GPU_RES_SAMP; bs[2].u.samp = samp;
                nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 3);
                nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1; nvn_gpu_draw(c, &da);
                nvn_gpu_pass_end(c); nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
                memset(&e, 0, sizeof e); r = nvn_gpu_cmd_commit(c, ++signal, on_done, NULL, &e);
                CHECK(OK(r), "C976r 分离采样：期望绘制成功 → rc=%d why=%s", r.code, e.why);
                if (OK(r)) {
                    r = nvn_gpu_fence_wait(signal, 5000000000ull, &e);
                    CHECK(OK(r), "C976r 分离采样 wait：%s", e.why);
                    if (OK(r)) {
                        const uint8_t *pixel = (const uint8_t *)buf_cpu(rb) + (32 * 64 + 32) * 4;
                        CHECK(pixel[0] >= 127 && pixel[0] <= 128, "C976r 分离采样：期望平均 0.5（127/128）→ %u", pixel[0]);
                        printf("C976r 分离采样：期望平均 0.5（127/128）→ %u\n", pixel[0]);
                    }
                }
                CHECK(nvn_unimpl_count("gpu", "sampler_reduce_average_separate_unknown_format") == before + 1,
                      "C976r 分离采样：期望具名降级记录一次");
            }
            if (vb) nvn_gpu_buf_release(vb, NULL, NULL);
            if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
            if (ov) nvn_gpu_view_release(ov);
            if (out) nvn_gpu_tex_release(out);
            if (view) nvn_gpu_view_release(view);
            if (view0) nvn_gpu_view_release(view0);
            if (tex) nvn_gpu_tex_release(tex);
            if (tex0) nvn_gpu_tex_release(tex0);
            if (samp) nvn_gpu_samp_release(samp);
            if (vs) nvn_gpu_shader_release(vs);
            if (fs) nvn_gpu_shader_release(fs);
            free(vm); free(fm);
        } else CHECK(0, "C976r 分离采样：着色器读取失败");
    }

}

// C976r 格式负控：RGBA32F 不支持 FILTER_MINMAX 时，在真实的组合描述符路径具名拒绝。
static void c976r_bad_format(const char *dir) {
    VkFormatProperties fp = {0}; vkGetPhysicalDeviceFormatProperties(g_dev.physical, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
    if (!g_dev.sampler_minmax_enabled || !g_dev.minmax_image_mapping ||
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT_EXT)) {
        printf("C976r 不支持格式负控：设备条件不符，未测\n"); return;
    }
    nvn_gpu_err e = {0};
    samp_par sp = {0}; sp.minf = NVN_MIN_FILTER_LINEAR; sp.magf = NVN_MAG_FILTER_LINEAR;
    sp.ws = sp.wt = sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE; sp.reduction = NVN_SAMPLER_REDUCTION_MIN;
    nvn_gpu_samp samp = NULL; nvn_gpu_rc r = nvn_gpu_samp_create(&sp, &samp, &e);
    CHECK(OK(r) && samp, "C976r 格式负控建采样器：%d %s", r.code, e.why);
    nvn_gpu_tex tex = make_tex(2, 1, NVN_FORMAT_RGBA32F, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view view = make_view(tex);
    const float uv[2] = {0.5f, 0.5f};
    nvn_gpu_buf in = make_buf(sizeof uv, NVN_GPU_MEM_SHARED, uv), out = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_layout_ent ents[3] = {{0}};
    ents[0].cls = NVN_GPU_RES_TEX_SAMP; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    ents[2].cls = NVN_GPU_RES_SSBO; ents[2].access = NVN_GPU_ACC_W; ents[2].binding = 2; ents[2].count = 1; ents[2].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout layout = {0}; layout.ent = ents; layout.nent = 3;
    nvn_gpu_shader sh = NULL; nvn_gpu_pipe pipe = mk_cpipe(dir, "samp_at.comp", &layout, &sh);
    CHECK(pipe != NULL, "C976r 格式负控管线为空");
    if (pipe && samp && view && in && out) {
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_binding b[3] = {{0}};
        b[0].ent = 0; b[0].cls = NVN_GPU_RES_TEX_SAMP; b[0].u.ts.view = view; b[0].u.ts.samp = samp;
        b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = in; b[1].u.b.len = sizeof uv;
        b[2].ent = 2; b[2].cls = NVN_GPU_RES_SSBO; b[2].u.b.buf = out; b[2].u.b.len = 16;
        nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 3); nvn_gpu_dispatch(c, pipe, 1, 1, 1, NULL, 0);
        memset(&e, 0, sizeof e); r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "sampler_reduction_view_format"),
              "C976r 不支持格式：期望具名 UNSUPPORTED → rc=%d why=%s", r.code, e.why);
        printf("C976r 不支持格式：rc=%d（%s）\n", r.code, e.why);
    }
    if (view) nvn_gpu_view_release(view);
    if (tex) nvn_gpu_tex_release(tex);
    if (samp) nvn_gpu_samp_release(samp);
    if (in) nvn_gpu_buf_release(in, NULL, NULL);
    if (out) nvn_gpu_buf_release(out, NULL, NULL);
    if (sh) nvn_gpu_shader_release(sh);
}

// C63：描述符数组元素位。t[2] 绑定 elem0=红、elem1=绿，着色器取 t[1] → 全绿。
// 若后端把 elem 当 0 写，elem1 会盖到元素 0、t[1] 留空——本例必红。
static void c63_desc_array_elem(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 2; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "arr2.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 arr2 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C63 pipe：%d %s", e.rc.code, e.why);
    const uint8_t red[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const uint8_t grn[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    nvn_gpu_tex tr = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex tg = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view vr = make_view(tr), vg = make_view(tg);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C63 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tr, 0, 0, NULL, red, 8, 16);
    nvn_gpu_tex_upload(c, tg, 0, 0, NULL, grn, 8, 16);
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding bs[3];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX; bs[0].elem = 0; bs[0].u.view = vr;
    bs[1].ent = 0; bs[1].cls = NVN_GPU_RES_TEX; bs[1].elem = 1; bs[1].u.view = vg;
    bs[2].ent = 1; bs[2].cls = NVN_GPU_RES_SAMP; bs[2].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 3);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 110, on_done, NULL, &e);
    CHECK(OK(r), "C63 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(110, 5000000000ull, &e);
    CHECK(OK(r), "C63 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, "C63");
    nvn_gpu_view_release(vr); nvn_gpu_view_release(vg);
    nvn_gpu_tex_release(tr); nvn_gpu_tex_release(tg);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C63c：同一个 view 写进同一描述符数组的两个元素（Vulkan 合法用法；VkWriteDescriptorSet 只约束每个元素
// 自身，没有数组内元素必须互不相同的要求）。着色器取 t[1]，纹理填均匀 128：元素 1 必须真被写进去。
// 后端若把 dstArrayElement 从写入影子的键里丢掉，元素 1 会留在组对象的零初始状态——采样时驱动按
// lp_descriptor.functions == NULL 解引用（或从复用组里静默采到上一笔的旧 view），本例必红。
static void c63c_desc_array_same_view(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 2; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "arr2.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "C63c 读不到 arr2 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C63c pipe：%d %s", e.rc.code, e.why);
    const uint8_t gray[16] = {128, 0, 0, 255, 128, 0, 0, 255, 128, 0, 0, 255, 128, 0, 0, 255};
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v = make_view(t);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C63c samp：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, gray, 8, 16);
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding bs[3];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX; bs[0].elem = 0; bs[0].u.view = v;
    bs[1].ent = 0; bs[1].cls = NVN_GPU_RES_TEX; bs[1].elem = 1; bs[1].u.view = v;   // 与元素 0 是同一个 view
    bs[2].ent = 1; bs[2].cls = NVN_GPU_RES_SAMP; bs[2].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 3);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 111, on_done, NULL, &e);
    CHECK(OK(r), "C63c commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(111, 5000000000ull, &e);
    CHECK(OK(r), "C63c wait：%d %s", r.code, e.why);
    if (OK(r)) {
        const uint8_t *px = (const uint8_t *)buf_cpu(rb) + (32 * 64 + 32) * 4;
        printf("C63c 同 view 双元素（取元素 1）：期望 128 → %u\n", px[0]);
        expect_rect(buf_cpu(rb), 0, 0, 64, 64, 128, 0, 0, "C63c");
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}


// C63b（独占一个会话，围栏从 1 起）：描述符数组里相邻元素相同。t[2] 的 elem0、elem1 都绑绿色视图（采样器共用），着色器取 t[1] → 全绿、校验层 0 条。
// 写入跳过（nvk_desc_skip，缺省开）的影子若不比元素号，elem1 会被当成「和 elem0 一样」跳过，新组里 t[1] 从没写过：
// 校验层报描述符未更新，画面不是绿色。
static void c63b_desc_array_same(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 2; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "arr2.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 arr2 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C63b pipe：%d %s", e.rc.code, e.why);
    const uint8_t red[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const uint8_t grn[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    nvn_gpu_tex tr = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex tg = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view vr = make_view(tr), vg = make_view(tg);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C63b samp：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tr, 0, 0, NULL, red, 8, 16);
    nvn_gpu_tex_upload(c, tg, 0, 0, NULL, grn, 8, 16);
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding bs[3];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX; bs[0].elem = 0; bs[0].u.view = vg;
    bs[1].ent = 0; bs[1].cls = NVN_GPU_RES_TEX; bs[1].elem = 1; bs[1].u.view = vg;
    bs[2].ent = 1; bs[2].cls = NVN_GPU_RES_SAMP; bs[2].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 3);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "C63b commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
    CHECK(OK(r), "C63b wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, "C63b");
    nvn_gpu_view_release(vr); nvn_gpu_view_release(vg);
    nvn_gpu_tex_release(tr); nvn_gpu_tex_release(tg);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C64：VA 类别显式报红——契约未定非 BDA 消费方式，管线创建期必须 E_UNSUPPORTED，不许静默成功。
static void c64_va_red(const char *dir) {
    nvn_gpu_err e;
    // 计算管线
    {
        nvn_gpu_layout_ent ce;
        memset(&ce, 0, sizeof ce);
        ce.set = 0; ce.cls = NVN_GPU_RES_VA; ce.access = NVN_GPU_ACC_R; ce.binding = 0; ce.count = 1;
        ce.stages = 1u << NVN_GPU_SH_CS;
        nvn_gpu_layout cl;
        memset(&cl, 0, sizeof cl);
        cl.ent = &ce; cl.nent = 1;
        nvn_gpu_shader sh = load_one_shader(dir, "comp.comp", NVN_GPU_SH_CS, &cl);
        if (sh) {
            nvn_gpu_stage_desc cs;
            memset(&cs, 0, sizeof cs);
            cs.sh = sh;
            nvn_gpu_pipe cp = NULL;
            nvn_gpu_rc r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C64 计算 VA 应 E_UNSUPPORTED，实为 %d", r.code);
            nvn_gpu_shader_release(sh);
        }
    }
    // 图形管线
    {
        nvn_gpu_layout_ent fe;
        memset(&fe, 0, sizeof fe);
        fe.set = 0; fe.cls = NVN_GPU_RES_VA; fe.access = NVN_GPU_ACC_R; fe.binding = 0; fe.count = 1;
        fe.stages = 1u << NVN_GPU_SH_FS;
        nvn_gpu_layout fl;
        memset(&fl, 0, sizeof fl);
        fl.ent = &fe; fl.nent = 1;
        nvn_gpu_shader vs = NULL, fs = NULL;
        void *vm = NULL, *fm = NULL;
        if (load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, &fl, &vs, &fs, &vm, &fm)) {
            nvn_gpu_pipe_desc pd;
            fill_gpipe_desc(&pd, vs, fs, 0);
            nvn_gpu_pipe pipe = NULL;
            nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C64 图形 VA 应 E_UNSUPPORTED，实为 %d", r.code);
            nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
            free(vm); free(fm);
        }
    }
}

// ---- D9 静默错复现（只复现不修）：断言的是当前实际行为；修好对应项后这些用例要翻期望 ----

// C65：只有深度附件、没有颜色附件的通道合法（阴影图、深度预通道）。清成 0.5 后读回。
// 期望（手算）：提交成功；64×64 个 32 位浮点全是 0.5。
static void c65_depth_only_pass(void) {
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.depth.view = v; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 0.5f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 111, on_done, NULL, &e);
    CHECK(OK(r), "C65 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(111, 5000000000ull, &e);
    CHECK(OK(r), "C65 wait：%d %s", r.code, e.why);
    { const float *f = (const float *)buf_cpu(rb); int bad = -1; for (int i = 0; i < 64 * 64; i++) if (f[i] != 0.5f) { bad = i; break; }
      CHECK(bad < 0, "C65 深度读回第 %d 个 = %g，期望 0.5", bad, bad < 0 ? 0.0 : (double)f[bad]); }
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
}

// C66：一段命令里的描述符组数不设上限。测试的链接包装器模拟严格执行池上限的驱动：每个池第 65 组起返回池耗尽。
// 一段里连发 70 次派发（每次 1 组）：第 65 次在第一个池上被拒 → 后端换新池重分 → 新池上分第 65–70 次共 6 组。
// 期望（手算）：被拒 1 次；最后一个池上累计请求 6 组；提交成功；派发的结果正确（comp.comp 把 v[i] 写成 i + 1）。
// 回收开着时（#1096）向驱动要组按 1、2、4、8 翻倍，每批多出的组留给后面的派发，前面的用例也可能在回收表里留下同布局的空闲组，这一段先取它们、少向驱动要；
// 所以派发数 = 80 + 开段时回收表里全部空闲组数（上界），向驱动要的组数 ≥ 80、仍越过每池 64 组的上限；回收关着时派发数仍是 70，严格数字不变。
static void c66_pool_sets(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "comp.comp", &cl, &sh);
    CHECK(cp != NULL, "C66 cpipe 为空");
    if (!cp) return;
    uint64_t stride = g_dev.properties.limits.minStorageBufferOffsetAlignment; if (stride < 64) stride = 64;
    const int n66 = nvn_vk_dbg_dset_stat(9) ? 80 + (int)nvn_vk_dbg_dset_stat(5) : 70;
    nvn_gpu_buf buf = make_buf(stride * (uint64_t)n66, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = 64;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    // 计算派发也复用；真实重复派发不得多分配，后面的压力段仍让每笔offset变化。
    b.u.b.off = 69 * stride; nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    unsigned before = desc_demand(); nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    CHECK(desc_demand() - before == 1, "C66 首派发分配1组");
    before = desc_demand(); nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1); nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    CHECK(desc_demand() == before, "C66 相同派发不分配");
    g_c66_sets_requested = g_c66_rejects = 0;
    g_c66_pool = VK_NULL_HANDLE;
    g_c66_inject_pool_limit = 1;
    for (int i = 0; i < n66; i++) {
        b.u.b.off = (uint64_t)i * stride; nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
        nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    }
    g_c66_inject_pool_limit = 0;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 112, on_done, NULL, &e);
    // 回收开着时一次向驱动要一批（1、2、4、8 组），被拒的次数与新池上的请求数随批大小变，这条严格数字只对老路径成立；回收开着时的池上限见 C204 臂 C。
    CHECK(g_memo_on || (nvn_vk_dbg_dset_stat(9) ? g_c66_rejects >= 1 : (g_c66_rejects == 1 && g_c66_sets_requested == 6)), "C66 应被拒 1 次、新池上请求 6 组（回收开着：被拒 ≥ 1 次），实为 被拒 %u 次、%u 组", g_c66_rejects, g_c66_sets_requested);
    CHECK(OK(r), "C66 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(112, 5000000000ull, &e);
    CHECK(OK(r), "C66 wait：%d %s", r.code, e.why);
    for (int k = 0; k < n66; k++) { const uint32_t *v = (const uint32_t *)(buf_cpu(buf) + k * stride); int bad = -1;
        for (int i = 0; i < 16; i++) if (v[i] != (uint32_t)i + 1) { bad = i; break; }
        CHECK(bad < 0, "C66 第%d笔输出项%d不符", k, bad); }
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C204（#1096）：描述符组按布局回收。真机世界场景每帧 2263 次 vkAllocateDescriptorSets（一组一次，约 1.25 毫秒，驱动里每次还有一轮堆分配 / 释放）；
// 后端改成按布局留一串空闲的组：段完成把取过的组整批放回、下一段直接取；放弃的段不放回、一律还给池；向驱动要组一次 1、2、4、8 组地翻倍（封顶 8），池分满换新池；
// 组一直留在表里复用、不清扫（驱动里还了再分会让池涨到约 4 倍，见 #1096）；在用加空闲的组数到总量上限后，向驱动要组被拒、这一笔退回老路径。
// 用计算管线（C66 同法）：一段里连发 N 次派发、每次换 SSBO 偏移，所以每次派发都要一个新组；comp.comp 把 v[i] 写成 i + 1。
// 每一轮开始前把整块缓冲清零、跑完逐槽核对：两次派发共用了同一个组、或取到了别人的组，都会让某个槽留 0。每个臂自己开 / 关一个会话（开关在开会话时读）。
// 期望值（手算；N = 70；向驱动要组的批大小依次 1、2、4、8、8、…，累计 1、3、7、15、23、31、39、47、55、63、71）：
//   臂 A（回收开）：第 1 轮回收表是空的，向驱动要 11 批（1 + 2 + 4 + 8 × 8）= 71 组（每批第 1 组直接交出、其余留进空闲串给后面的派发：复用 70 - 11 = 59），段完成放回 70 → 空闲 71；
//                   第 2…6 轮：向驱动新分 0 组（驱动里的分配计数也不动）、复用 70、放回 70，空闲恒为 71；
//                   第 7 轮放弃（记录完不提交）：复用 70、放回 0、还给池 70，空闲 1；第 8 轮补分 ≥ 68 组（批大小已是 8：先取空闲的 1 组，再要 9 批 72 组），第 9…10 轮又回到新分 0；
//                   全部段结束后两条恒等式：交出 == 放回 + 还给池；向驱动要来的 == 空闲 + 还给池。回放线程取组碰锁退回老路径 0 次。
//   臂 B（回收关，对照）：每轮驱动里的分配计数 +70（老路径一组一次），回收表的计数全程为 0——证明「新分 0」这个读数分得出两种做法。
//   臂 C（回收开 + 严格池上限，每池 64 组，C66 的包装器）：一段 150 次派发，被拒 ≥ 1 次、增长池 ≥ 2 个、150 组全从回收表交出、输出全对。
//   臂 D（回收开 + 一批放不进空池）：改成一组一组分（输出全对、第 2 轮起新分 0、不标 dead）。
//   臂 E（回收开 + 一组也放不进空池）：布局标 dead、回到老路径（输出全对、驱动里每轮 +70），增长池始终 1 个（不会每次都建新池）。
//   臂 F（生产缺省）：回收期望开着；稳态新分 0。缺省改成关的时候这里会红，改动要在 PR 里讲清。
//   臂 G（回收的组里留着上一次的描述符 → 写法不同的取用不用回收来的组）：布局有两项（binding 0 / 1），着色器只用 binding 0。
//                   段 1 写法 {binding 0, binding 1}（槽 1 指向缓冲 Y）：新分 1 组（批大小从 1 起）、表项的写法定为它；释放 Y（完成线程在不确定的时刻销毁它）；
//                   段 2 写法 {binding 0}（和表项的不同）：不从表项取（复用 0、交出 0、被拒 1），走老路径（驱动里分配计数 +1），输出全对；
//                   段 3 写法又是 {0, 1}（槽 1 指向新缓冲）：照常复用 1、被拒 0、新分 0；
//                   竞态 40 轮：每轮「写法 {0, 1} 的段 → 立刻释放它的缓冲 → 写法 {0} 的段」：复用 40、被拒 40、新分 0、老路径 +40、输出全对，两条恒等式成立。
//                   旧做法（表项里的组留着没重写的槽）下，被销毁的 Y 还被已绑进下一段的组指着：validation 层报 VUID-vkDestroyBuffer-buffer-00922
//                   或命令缓冲因「绑定的缓冲已销毁」失效，整套 vk 用例里偶发（本臂 40 轮就是为了让它每遍都有很多次机会撞上）。
//   臂 M（写法按「binding + 数组项号」认）：binding 0 一项（着色器用）、binding 1 有 3 项、binding 2 有 2 项（着色器都不用），每段在 binding 0 之外再多写一条：
//                   {b0e0, b1e1} 定写法并新分 1 组；{b0e0, b1e2}（条数、binding 相同，数组项号不同）、{b0e0, b2e1}（条数、数组项号的集合相同，binding 不同）、
//                   {b0e0}（条数不同）各被拒 1 次、复用 0；回到 {b0e0, b1e1} 照常复用 1。两条恒等式成立。
//   臂 H（总量上限，nvn_vk_dbg_dset_cap 改成 15 组；N = 70）：第 1 轮：向驱动要 1 + 2 + 4 + 8 = 15 组，到 15 就拒——复用 11、交出 15、放回 15、其余 55 次派发退回老路径
//                   （到上限退回 55 次，驱动里分配计数 +70 = 新分 15 + 老路径 55）、输出全对、空闲 15；第 2 轮：新分 0、复用 15、退回 55、驱动计数 +55；
//                   放弃一轮（取走的 15 组还给池、空闲 0，在用加空闲的组数回落，上限的余量回来）；再跑 1 轮：向驱动要 8 + 8 = 16 组（第 2 批取完是 16、比上限多 1 组：上限是软的，
//                   最多多 DSET_BATCH - 1 组），之后拒，退回 54 次；上限放开成 1000：新分 56（16 组复用，其余 54 次派发要 7 批 8 组）、不再退回、空闲 72；
//                   然后 300 轮稳态（300 个段）：新分 0、还给池 0（没有清扫）、不退回、空闲恒为 72。两条恒等式成立。
//   臂 I（取组碰锁）：测试线程自己持着回收表的锁，一段 10 次派发的取组全部退回老路径：碰锁 10 次、回收表交出 0、驱动里分配计数 +10、输出全对。
//   臂 J（表项登记在布局表项里，生产缺省）：同一条管线连跑 4 轮（N = 70）：表项只在第 1 轮第一次取组时登记 1 项，第 2…4 轮 0 项；放弃的段也是 0 项；
//                   换一条布局结构不同的管线（两项布局）：再登记 1 项，之后 0 项；输出全对、两条恒等式成立；会话代际比上一个臂的大（每次开会话 +1）。
//   臂 K（登记表随布局结构数涨，没有上限）：40 条布局结构各不相同的管线（第二项的 binding 号 1…40，布局表翻倍重排两次），每条各跑 5 轮、每轮 3 次派发：表项数 = 结构数 40、没有重复登记（第 2…5 轮不涨）、
//                   每条管线只向驱动要过 1 + 2 = 3 组（批大小 1、2，之后 3 组刚好够用）、沿登记表遍历数出来的表项数 == 登记的计数（链表没丢项）、输出全对、
//                   两条恒等式成立（空闲数是遍历出来的：丢了项恒等式就对不上）。
//   臂 L（登记时内存不够）：注入 70 次登记失败：第 1 轮取组全部退回老路径（驱动里每轮分配计数 +70、回收表交出 0、表项数 0、输出全对）；
//                   失败不记：第 2 轮登记成功、走回收表（交出 70、表项数 1、新分 71），第 3 轮起稳态新分 0，两条恒等式成立。
//   臂 N（布局按结构共用）：① 布局表本身（不经管线，直接按结构取）：同一结构取两次 = 同一个句柄（命中 +1、表项 +1）、binding 顺序打乱仍是同一个句柄；
//                   binding 号 / 类型 / 数量 / 阶段任何一项不同 = 不同的句柄（各一个新表项）；零项的布局也共用一个；300 种结构全建出来后桶数 ≥ 项数、最长链 ≤ 8，
//                   再按同样的结构全取一遍：句柄逐个相同、全部命中。② 两条管线用同一份布局（同一着色器、同一布局声明）：共用一个布局句柄和一个表项——
//                   第 1 轮 70 次派发新分 71、登记 1 项，第二条管线跑 70 次派发新分 0、复用 70、不再登记，布局表命中 1 次；再来一条结构不同的管线：登记 1 项，布局表项 +1；
//                   输出全对、两条恒等式成立。会话收尾后布局全部销毁（布局表 0 项、累计建的 == 累计销毁的，每个臂收尾都核对）。
#define C204_SLOTS 320
#define C204_EQ(what, got, want) CHECK((got) == (want), "C204 臂 %c " what "：期望 %llu → %llu", 'A' + arm, (unsigned long long)(want), (unsigned long long)(got))
struct c204_snap { uint64_t nw, reuse, put, freed, capped, taken, idle, pools, dead, busy, sigmiss; unsigned drv; };
static struct c204_snap c204_now(void) {
    struct c204_snap s;
    s.nw = nvn_vk_dbg_dset_stat(0); s.reuse = nvn_vk_dbg_dset_stat(1); s.put = nvn_vk_dbg_dset_stat(2); s.freed = nvn_vk_dbg_dset_stat(3);
    s.taken = nvn_vk_dbg_dset_stat(4); s.idle = nvn_vk_dbg_dset_stat(5); s.pools = nvn_vk_dbg_dset_stat(7); s.dead = nvn_vk_dbg_dset_stat(8);
    s.capped = nvn_vk_dbg_dset_stat(10); s.busy = nvn_vk_dbg_dset_stat(11); s.sigmiss = nvn_vk_dbg_dset_stat(14);
    s.drv = g_desc_sets_total;
    return s;
}
// 段完成的收尾（把组放回）在完成线程里、晚于 fence_wait 返回：等到交出去的组都有了着落（放回或还给池）再进下一轮（最多 5 秒）
static void c204_settle(void) {
    for (int i = 0; i < 5000; i++) {
        if (nvn_vk_dbg_dset_stat(4) == nvn_vk_dbg_dset_stat(2) + nvn_vk_dbg_dset_stat(3)) return;
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
}
// 一轮：缓冲清零 → 一段里连发 n 次派发（每次换偏移 → 每次一个新组）→ 提交并等完成（abandon 时改为放弃这一段）→ 逐槽核对输出。返回不符的槽数。
// extra != NULL：这一段在 binding 0 的第 0 项之外再多写一条（布局项 xent 的数组项 xelem，指向 extra），这一笔的「写法」就多一条。
static int c204_round_x(nvn_gpu_pipe cp, nvn_gpu_buf buf, nvn_gpu_buf extra, uint32_t xent, uint32_t xelem, uint64_t stride, int n, int abandon, uint64_t *sig, const char *tag) {
    memset(buf_cpu(buf), 0, (size_t)(stride * C204_SLOTS));
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[2];   // nvn_gpu_bind 一次给齐本阶段的全部绑定（每次调用整表替换），所以多出的一条要和第一条放在同一张表里
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_SSBO; bs[0].u.b.buf = buf; bs[0].u.b.len = 64;
    bs[1] = bs[0]; bs[1].ent = xent; bs[1].elem = xelem; bs[1].u.b.buf = extra; bs[1].u.b.off = 0; bs[1].u.b.len = 64;
    for (int i = 0; i < n; i++) {
        bs[0].u.b.off = (uint64_t)i * stride; nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, extra ? 2 : 1);
        nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    }
    if (abandon) { nvn_gpu_cmd_abort(c); c204_settle(); return 0; }
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, ++*sig, on_done, NULL, &e);
    CHECK(OK(r), "C204 %s commit：%d %s", tag, r.code, e.why);
    r = nvn_gpu_fence_wait(*sig, 5000000000ull, &e);
    CHECK(OK(r), "C204 %s wait：%d %s", tag, r.code, e.why);
    c204_settle();
    int bad = 0;
    for (int k = 0; k < n; k++) {
        const uint32_t *v = (const uint32_t *)(buf_cpu(buf) + k * stride);
        for (int i = 0; i < 16; i++) if (v[i] != (uint32_t)i + 1) { bad++; break; }
    }
    return bad;
}
static int c204_round(nvn_gpu_pipe cp, nvn_gpu_buf buf, uint64_t stride, int n, int abandon, uint64_t *sig, const char *tag) {
    return c204_round_x(cp, buf, NULL, 0, 0, stride, n, abandon, sig, tag);
}
static uint64_t g_c204_gen_prev;   // 上一个臂的会话代际（回收表代际每次开会话 +1，这里核对它确实在涨）
static uint64_t g_c204_cap0;       // 第一个开会话的臂看到的总量上限（缺省值）：之后每个臂开会话时都应是它（臂 H 改过上限，开会话要复位）
static void c204_arm(const char *dir, int arm) {
    g_dsetrec_flag = arm == 1 ? 0 : arm == 5 ? -2 : 1;
    if (!open_backend()) { CHECK(0, "C204 臂 %c 开会话失败", 'A' + arm); g_dsetrec_flag = -1; return; }
    { const uint64_t gen = nvn_vk_dbg_dset_stat(15);
      CHECK(gen > g_c204_gen_prev, "C204 臂 %c 回收表代际应随每次开会话增大：上一个臂 %llu → 这个臂 %llu", 'A' + arm, (unsigned long long)g_c204_gen_prev, (unsigned long long)gen);
      g_c204_gen_prev = gen; }
    { const uint64_t cap = nvn_vk_dbg_dset_stat(26);
      if (!g_c204_cap0) g_c204_cap0 = cap;
      CHECK(cap > 0 && cap == g_c204_cap0, "C204 臂 %c 总量上限应在每次开会话时复位成缺省：第一个臂 %llu → 这个臂 %llu", 'A' + arm, (unsigned long long)g_c204_cap0, (unsigned long long)cap); }
    nvn_vk_dbg_dset_inject(arm == 3 ? 1 : arm == 4 ? 2 : 0);
    if (arm == 11) nvn_vk_dbg_dset_failreg(70);   // 臂 L：接下来 70 次登记表项当作内存不够（第 1 轮 70 次取组各撞一次）
    nvn_gpu_layout_ent ce[3];
    memset(ce, 0, sizeof ce);
    ce[0].set = 0; ce[0].cls = NVN_GPU_RES_SSBO; ce[0].access = NVN_GPU_ACC_W; ce[0].binding = 0; ce[0].count = 1; ce[0].stages = 1u << NVN_GPU_SH_CS;
    ce[1] = ce[0]; ce[1].binding = 1;   // 臂 G / M：第二项，着色器不用
    // 臂 M：数组放在着色器不用的 binding 1（3 项）/ binding 2（2 项）上；binding 0 保持 1 项、每段都写。
    //   着色器用到的 binding 若是数组、只写第 0 项，新版校验层（1.4.341）把没写的项算作派发时访问到、报 VUID-vkCmdDispatch-None-08114。
    if (arm == 12) { ce[1].count = 3; ce[2] = ce[0]; ce[2].binding = 2; ce[2].count = 2; }
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = ce; cl.nent = arm == 12 ? 3 : arm == 6 ? 2 : 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "comp.comp", &cl, &sh);
    CHECK(cp != NULL, "C204 臂 %c cpipe 为空", 'A' + arm);
    uint64_t stride = g_dev.properties.limits.minStorageBufferOffsetAlignment; if (stride < 64) stride = 64;
    nvn_gpu_buf buf = cp ? make_buf(stride * C204_SLOTS, NVN_GPU_MEM_SHARED, NULL) : NULL;
    uint64_t sig = 0;
    struct c204_snap s0, s1;
    int bad;
    if (cp && buf) {
        if (arm == 0) {
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "r1"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 A 第 1 轮输出不符 %d 槽", bad);
            C204_EQ("第 1 轮向驱动新分的组（批大小 1、2、4、8 × 8 共 11 批）", s1.nw - s0.nw, 71); C204_EQ("第 1 轮复用（每批多出的组在本轮后面的派发里取：70 - 11）", s1.reuse - s0.reuse, 59);
            C204_EQ("第 1 轮交出", s1.taken - s0.taken, 70); C204_EQ("第 1 轮放回", s1.put - s0.put, 70); C204_EQ("第 1 轮后空闲串", s1.idle, 71);
            for (int r = 2; r <= 6; r++) {
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "steady"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 A 第 %d 轮输出不符 %d 槽", r, bad);
                CHECK(s1.nw == s0.nw && s1.drv == s0.drv, "C204 臂 A 第 %d 轮：向驱动新分的组期望 0（驱动计数 +0）→ %llu（驱动计数 +%u）", r, (unsigned long long)(s1.nw - s0.nw), s1.drv - s0.drv);
                CHECK(s1.reuse - s0.reuse == 70 && s1.put - s0.put == 70 && s1.idle == 71, "C204 臂 A 第 %d 轮：复用 70、放回 70、空闲 71，实为 %llu / %llu / %llu", r,
                      (unsigned long long)(s1.reuse - s0.reuse), (unsigned long long)(s1.put - s0.put), (unsigned long long)s1.idle);
            }
            s0 = c204_now(); (void)c204_round(cp, buf, stride, 70, 1, &sig, "abandon"); s1 = c204_now();
            C204_EQ("放弃的段：复用", s1.reuse - s0.reuse, 70); C204_EQ("放弃的段：放回", s1.put - s0.put, 0);
            C204_EQ("放弃的段：还给池", s1.freed - s0.freed, 70); C204_EQ("放弃之后的空闲串", s1.idle, 1);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "refill"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 A 放弃之后第 1 轮输出不符 %d 槽", bad);
            C204_EQ("放弃之后补分（取走空闲的 1 组，再要 9 批 8 组）", s1.nw - s0.nw, 72);
            for (int r = 0; r < 2; r++) {
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "steady2"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 A 放弃之后稳态输出不符 %d 槽", bad);
                CHECK(s1.nw == s0.nw && s1.drv == s0.drv, "C204 臂 A 放弃之后稳态：向驱动新分的组期望 0 → %llu", (unsigned long long)(s1.nw - s0.nw));
            }
            s1 = c204_now();
            CHECK(s1.taken == s1.put + s1.freed, "C204 臂 A 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
            CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 A 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
            C204_EQ("取组碰锁退回老路径的次数", s1.busy, 0);
        } else if (arm == 1) {
            for (int r = 1; r <= 3; r++) {
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "off"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 B 第 %d 轮输出不符 %d 槽", r, bad);
                C204_EQ("回收关：每轮驱动里的分配计数增量", (uint64_t)(s1.drv - s0.drv), 70);
                CHECK(s1.nw == 0 && s1.taken == 0 && s1.reuse == 0 && s1.idle == 0 && s1.pools == 0, "C204 臂 B 回收表的计数应全程为 0：新分 %llu 交出 %llu 复用 %llu 空闲 %llu 池 %llu",
                      (unsigned long long)s1.nw, (unsigned long long)s1.taken, (unsigned long long)s1.reuse, (unsigned long long)s1.idle, (unsigned long long)s1.pools);
            }
        } else if (arm == 2) {
            g_c66_sets_requested = g_c66_rejects = 0; g_c66_pool = VK_NULL_HANDLE; g_c66_inject_pool_limit = 1;
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 150, 0, &sig, "limit"); s1 = c204_now();
            g_c66_inject_pool_limit = 0;
            CHECK(bad == 0, "C204 臂 C 输出不符 %d 槽", bad);
            CHECK(g_c66_rejects >= 1, "C204 臂 C 严格池上限：被拒期望 ≥ 1 次 → %u 次", g_c66_rejects);
            CHECK(s1.pools - s0.pools >= 2, "C204 臂 C 增长池期望 ≥ 2 个 → %llu 个", (unsigned long long)(s1.pools - s0.pools));
            C204_EQ("150 组全从回收表交出", s1.taken - s0.taken, 150);
        } else if (arm == 3) {
            for (int r = 1; r <= 3; r++) {
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "solo"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 D 第 %d 轮输出不符 %d 槽", r, bad);
                if (r == 1) C204_EQ("一批放不进空池：第 1 轮改一组一组分，新分", s1.nw - s0.nw, 70);
                else C204_EQ("一批放不进空池：稳态新分", s1.nw - s0.nw, 0);
                C204_EQ("一批放不进空池：不标 dead", s1.dead, 0);
            }
        } else if (arm == 4) {
            for (int r = 1; r <= 3; r++) {
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "dead"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 E 第 %d 轮输出不符 %d 槽", r, bad);
                C204_EQ("一组也放不进空池：回到老路径，驱动里每轮分配计数增量", (uint64_t)(s1.drv - s0.drv), 70);
                C204_EQ("一组也放不进空池：标 dead 的布局数", s1.dead, 1);
                C204_EQ("一组也放不进空池：增长池始终", s1.pools, 1);
                C204_EQ("一组也放不进空池：回收表交出", s1.taken, 0);
            }
        } else if (arm == 5) {
            C204_EQ("生产缺省：回收是否开着", nvn_vk_dbg_dset_stat(9), 1);
            for (int r = 1; r <= 3; r++) {
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "default"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 F 第 %d 轮输出不符 %d 槽", r, bad);
                if (r == 3) C204_EQ("生产缺省：稳态新分", s1.nw - s0.nw, 0);
            }
        } else if (arm == 6) {
            nvn_gpu_buf y = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
            if (y) {
                s0 = c204_now(); bad = c204_round_x(cp, buf, y, 1, 0, stride, 1, 0, &sig, "pat-ab"); s1 = c204_now();   // 写法 {binding 0, binding 1}：表项的写法定为它；槽 1 指向缓冲 Y
                CHECK(bad == 0, "C204 臂 G 段 1 输出不符 %d 槽", bad);
                C204_EQ("段 1 向驱动新分的组（批大小从 1 起）", s1.nw - s0.nw, 1); C204_EQ("段 1 写法被拒的次数（第一次定写法，不拒）", s1.sigmiss - s0.sigmiss, 0);
                nvn_gpu_buf_release(y, NULL, NULL);   // Y 由完成线程在不确定的时刻销毁；表项里的组都写过槽 1，那里还指着 Y
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 1, 0, &sig, "pat-a"); s1 = c204_now();   // 写法 {binding 0}：和表项的不同
                CHECK(bad == 0, "C204 臂 G 段 2 输出不符 %d 槽", bad);
                C204_EQ("段 2 写法不同：不从表项取（复用）", s1.reuse - s0.reuse, 0); C204_EQ("段 2 写法不同：交出", s1.taken - s0.taken, 0);
                C204_EQ("段 2 写法不同：被拒", s1.sigmiss - s0.sigmiss, 1);
                C204_EQ("段 2 写法不同：走老路径，驱动里分配计数增量", (uint64_t)(s1.drv - s0.drv), 1);
                nvn_gpu_buf y2 = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
                if (y2) {
                    s0 = c204_now(); bad = c204_round_x(cp, buf, y2, 1, 0, stride, 1, 0, &sig, "pat-ab2"); s1 = c204_now();   // 写法又是 {0, 1}：照常复用（所有槽都重写）
                    CHECK(bad == 0, "C204 臂 G 段 3 输出不符 %d 槽", bad);
                    C204_EQ("段 3 写法相同：复用", s1.reuse - s0.reuse, 1); C204_EQ("段 3 写法相同：被拒", s1.sigmiss - s0.sigmiss, 0); C204_EQ("段 3 写法相同：新分", s1.nw - s0.nw, 0);
                    nvn_gpu_buf_release(y2, NULL, NULL);
                }
                // 竞态的读数：每一轮「写法 {0, 1} 的段（槽 1 指向新缓冲）→ 立刻释放那个缓冲 → 写法 {0} 的段」。缓冲由完成线程异步销毁，
                // 销毁可能落在下一段录制 / 执行的任何时刻；旧做法（回收的组里留着槽 1）下 validation 层会按 VUID-vkDestroyBuffer-buffer-00922 或命令缓冲失效报错，
                // 现在写法不同的段不碰表项里的组，一次都不该报（validation 报错由 run.sh 逐遍统计）。
                s0 = c204_now();
                for (int r = 0; r < 40; r++) {
                    nvn_gpu_buf yr = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
                    if (!yr) { CHECK(0, "C204 臂 G 竞态第 %d 轮建缓冲失败", r); break; }
                    bad = c204_round_x(cp, buf, yr, 1, 0, stride, 1, 0, &sig, "race-ab");
                    nvn_gpu_buf_release(yr, NULL, NULL);
                    bad += c204_round(cp, buf, stride, 1, 0, &sig, "race-a");
                    if (bad) { CHECK(0, "C204 臂 G 竞态第 %d 轮输出不符 %d 槽", r, bad); break; }
                }
                s1 = c204_now();
                C204_EQ("竞态 40 轮：写法 {0, 1} 的段复用", s1.reuse - s0.reuse, 40); C204_EQ("竞态 40 轮：写法 {0} 的段被拒", s1.sigmiss - s0.sigmiss, 40);
                C204_EQ("竞态 40 轮：向驱动新分的组", s1.nw - s0.nw, 0); C204_EQ("竞态 40 轮：老路径的驱动分配计数增量", (uint64_t)(s1.drv - s0.drv), 40);
                CHECK(s1.taken == s1.put + s1.freed, "C204 臂 G 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
                CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 G 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
            }
        } else if (arm == 12) {
            // 臂 M：写法按「binding + 数组项号」认，数一样也算不同。binding 1 有 3 项、binding 2 有 2 项（着色器不用）；每段在 binding 0 之外再多写一条。
            nvn_gpu_buf y = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
            if (y) {
                s0 = c204_now(); bad = c204_round_x(cp, buf, y, 1, 1, stride, 1, 0, &sig, "m-b1e1"); s1 = c204_now();   // 写法 {b0e0, b1e1}：表项的写法定为它
                CHECK(bad == 0, "C204 臂 M 段 1 输出不符 %d 槽", bad);
                C204_EQ("段 1 向驱动新分的组（批大小从 1 起）", s1.nw - s0.nw, 1); C204_EQ("段 1 被拒", s1.sigmiss - s0.sigmiss, 0);
                s0 = c204_now(); bad = c204_round_x(cp, buf, y, 1, 2, stride, 1, 0, &sig, "m-b1e2"); s1 = c204_now();   // {b0e0, b1e2}：条数相同、binding 相同，数组项号不同
                CHECK(bad == 0, "C204 臂 M 段 2 输出不符 %d 槽", bad);
                C204_EQ("段 2 数组项号不同：复用", s1.reuse - s0.reuse, 0); C204_EQ("段 2 数组项号不同：被拒", s1.sigmiss - s0.sigmiss, 1);
                s0 = c204_now(); bad = c204_round_x(cp, buf, y, 2, 1, stride, 1, 0, &sig, "m-b2e1"); s1 = c204_now();   // {b0e0, b2e1}：条数相同、数组项号的集合相同，binding 不同
                CHECK(bad == 0, "C204 臂 M 段 3 输出不符 %d 槽", bad);
                C204_EQ("段 3 binding 不同：复用", s1.reuse - s0.reuse, 0); C204_EQ("段 3 binding 不同：被拒", s1.sigmiss - s0.sigmiss, 1);
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 1, 0, &sig, "m-b0e0"); s1 = c204_now();   // {b0e0}：条数不同
                CHECK(bad == 0, "C204 臂 M 段 4 输出不符 %d 槽", bad);
                C204_EQ("段 4 条数不同：复用", s1.reuse - s0.reuse, 0); C204_EQ("段 4 条数不同：被拒", s1.sigmiss - s0.sigmiss, 1);
                s0 = c204_now(); bad = c204_round_x(cp, buf, y, 1, 1, stride, 1, 0, &sig, "m-b1e1-again"); s1 = c204_now();   // 回到 {b0e0, b1e1}：照常复用
                CHECK(bad == 0, "C204 臂 M 段 5 输出不符 %d 槽", bad);
                C204_EQ("段 5 写法相同：复用", s1.reuse - s0.reuse, 1); C204_EQ("段 5 写法相同：被拒", s1.sigmiss - s0.sigmiss, 0);
                nvn_gpu_buf_release(y, NULL, NULL);
                s1 = c204_now();
                CHECK(s1.taken == s1.put + s1.freed, "C204 臂 M 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
                CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 M 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
            }
        } else if (arm == 13) {
            // 臂 O：布局按结构共用（见上面的说明）。② 先做：两条管线用同一份布局，共用一个布局句柄和一个表项；再来一条结构不同的管线：多一项。
            const uint64_t n0 = nvn_vk_dbg_dset_stat(19), a0 = nvn_vk_dbg_dset_stat(20), h0 = nvn_vk_dbg_dset_stat(21);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "o-a1"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 N 第一条管线输出不符 %d 槽", bad);
            C204_EQ("第一条管线第 1 轮向驱动新分的组", s1.nw - s0.nw, 71); C204_EQ("第一条管线登记的表项", nvn_vk_dbg_dset_stat(6), 1);
            nvn_gpu_shader shb = NULL;
            nvn_gpu_pipe cpb = mk_cpipe(dir, "comp.comp", &cl, &shb);   // 同一着色器、同一份布局声明
            CHECK(cpb != NULL, "C204 臂 N 第二条管线为空");
            if (cpb) {
                C204_EQ("第二条管线取布局：布局表项数不变", nvn_vk_dbg_dset_stat(19) - n0, 0);
                C204_EQ("第二条管线取布局：取了 1 次", nvn_vk_dbg_dset_stat(20) - a0, 1); C204_EQ("第二条管线取布局：命中 1 次", nvn_vk_dbg_dset_stat(21) - h0, 1);
                s0 = c204_now(); bad = c204_round(cpb, buf, stride, 70, 0, &sig, "o-b1"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 N 第二条管线输出不符 %d 槽", bad);
                C204_EQ("第二条管线（同一布局）：向驱动新分的组（取第一条管线放回的）", s1.nw - s0.nw, 0); C204_EQ("第二条管线：复用", s1.reuse - s0.reuse, 70);
                C204_EQ("两条管线共用一个表项", nvn_vk_dbg_dset_stat(6), 1);
                // 结构不同：多一个着色器不用的 binding 7。不把着色器用到的 binding 0 改成 2 项：那样每段只写第 0 项，
                //   新版校验层（1.4.341）把没写的第 1 项算作派发时访问到，报 VUID-vkCmdDispatch-None-08114。
                //   用 7 是避开下面 ① 直接取的那些结构（binding 1 / 2 / 100–399），否则 ① 的「表项 +1」「命中次数」会被这里先登记的那项带偏。
                nvn_gpu_layout_ent cec[2]; cec[0] = ce[0]; cec[1] = ce[0]; cec[1].binding = 7;
                nvn_gpu_layout clc = cl; clc.ent = cec; clc.nent = 2;
                nvn_gpu_shader shc = NULL;
                nvn_gpu_pipe cpc = mk_cpipe(dir, "comp.comp", &clc, &shc);
                CHECK(cpc != NULL, "C204 臂 N 第三条管线为空");
                if (cpc) {
                    C204_EQ("结构不同的第三条管线：布局表项 +1", nvn_vk_dbg_dset_stat(19) - n0, 1);
                    s0 = c204_now(); bad = c204_round(cpc, buf, stride, 70, 0, &sig, "o-c1"); s1 = c204_now();
                    CHECK(bad == 0, "C204 臂 N 第三条管线输出不符 %d 槽", bad);
                    C204_EQ("结构不同的第三条管线有自己的表项：向驱动新分的组", s1.nw - s0.nw, 71); C204_EQ("三条管线 = 两个表项", nvn_vk_dbg_dset_stat(6), 2);
                }
                if (shc) nvn_gpu_shader_release(shc);
                s1 = c204_now();
                CHECK(s1.taken == s1.put + s1.freed, "C204 臂 N 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
                CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 N 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
            }
            if (shb) nvn_gpu_shader_release(shb);
            // ① 布局表本身（不经管线，直接按结构取）：类型 6 = UNIFORM_BUFFER、7 = STORAGE_BUFFER；阶段 0x1 = VERTEX、0x10 = FRAGMENT、0x20 = COMPUTE
            {
                enum { T_UBO = 6, T_SSBO = 7, S_VS = 0x1, S_FS = 0x10, S_CS = 0x20 };
                const uint32_t ka[8] = {0, T_SSBO, 1, S_CS, 1, T_SSBO, 1, S_CS};
                const uint32_t kp[8] = {1, T_SSBO, 1, S_CS, 0, T_SSBO, 1, S_CS};   // 同一个结构，binding 顺序倒过来
                const uint32_t kv[4][8] = {{0, T_SSBO, 1, S_CS, 2, T_SSBO, 1, S_CS},          // binding 号不同
                                           {0, T_SSBO, 1, S_CS, 1, T_UBO, 1, S_CS},           // 类型不同
                                           {0, T_SSBO, 1, S_CS, 1, T_SSBO, 2, S_CS},          // 数量不同
                                           {0, T_SSBO, 1, S_CS, 1, T_SSBO, 1, S_VS | S_FS}};  // 阶段不同
                const uint64_t n1 = nvn_vk_dbg_dset_stat(19), a1 = nvn_vk_dbg_dset_stat(20), hh1 = nvn_vk_dbg_dset_stat(21);
                const uint64_t ha = nvn_vk_dbg_dlay_get(ka, 2), hb = nvn_vk_dbg_dlay_get(ka, 2), hp = nvn_vk_dbg_dlay_get(kp, 2);
                CHECK(ha != 0 && ha == hb && ha == hp, "C204 臂 N ① 同一结构（含 binding 顺序打乱）应是同一个句柄：%llx / %llx / %llx", (unsigned long long)ha, (unsigned long long)hb, (unsigned long long)hp);
                C204_EQ("① 同一结构取 3 次：布局表项 +1", nvn_vk_dbg_dset_stat(19) - n1, 1); C204_EQ("① 取了 3 次", nvn_vk_dbg_dset_stat(20) - a1, 3); C204_EQ("① 命中 2 次", nvn_vk_dbg_dset_stat(21) - hh1, 2);
                uint64_t hv[4]; int distinct = 1;
                for (int i = 0; i < 4; i++) {
                    hv[i] = nvn_vk_dbg_dlay_get(kv[i], 2);
                    if (!hv[i] || hv[i] == ha) distinct = 0;
                    for (int j = 0; j < i; j++) if (hv[j] == hv[i]) distinct = 0;
                }
                CHECK(distinct, "C204 臂 N ① binding 号 / 类型 / 数量 / 阶段任何一项不同都应是不同的句柄：%llx %llx %llx %llx（原结构 %llx）",
                      (unsigned long long)hv[0], (unsigned long long)hv[1], (unsigned long long)hv[2], (unsigned long long)hv[3], (unsigned long long)ha);
                C204_EQ("① 原结构 + 四种变体：布局表项 +5", nvn_vk_dbg_dset_stat(19) - n1, 5);
                const uint64_t he1 = nvn_vk_dbg_dlay_get(NULL, 0), he2 = nvn_vk_dbg_dlay_get(NULL, 0);
                CHECK(he1 != 0 && he1 == he2 && he1 != ha, "C204 臂 N ① 零项的布局也共用一个句柄：%llx / %llx", (unsigned long long)he1, (unsigned long long)he2);
                C204_EQ("① 零项布局：布局表项 +1", nvn_vk_dbg_dset_stat(19) - n1, 6);
                enum { MANY = 300 };
                uint64_t hm[MANY]; int manyok = 1;
                for (int i = 0; i < MANY; i++) { const uint32_t k1[4] = {100u + (uint32_t)i, T_SSBO, 1, S_CS}; hm[i] = nvn_vk_dbg_dlay_get(k1, 1); if (!hm[i]) manyok = 0; }
                CHECK(manyok, "C204 臂 N ① 300 种结构取布局失败");
                C204_EQ("① 300 种结构：布局表项 +306", nvn_vk_dbg_dset_stat(19) - n1, 6 + MANY);
                CHECK(nvn_vk_dbg_dset_stat(22) >= nvn_vk_dbg_dset_stat(19) && nvn_vk_dbg_dset_stat(23) <= 8, "C204 臂 N ① 布局表随条数翻倍：期望桶数 ≥ 项数、最长链 ≤ 8 → 桶 %llu / 项 %llu、最长链 %llu",
                      (unsigned long long)nvn_vk_dbg_dset_stat(22), (unsigned long long)nvn_vk_dbg_dset_stat(19), (unsigned long long)nvn_vk_dbg_dset_stat(23));
                const uint64_t hit_before = nvn_vk_dbg_dset_stat(21); int same = 1;
                for (int i = 0; i < MANY; i++) { const uint32_t k1[4] = {100u + (uint32_t)i, T_SSBO, 1, S_CS}; if (nvn_vk_dbg_dlay_get(k1, 1) != hm[i]) same = 0; }
                CHECK(same, "C204 臂 N ① 翻倍重排之后按同样的结构再取：句柄应逐个相同");
                C204_EQ("① 再取 300 次：全部命中", nvn_vk_dbg_dset_stat(21) - hit_before, MANY); C204_EQ("① 再取 300 次：布局表项不再增加", nvn_vk_dbg_dset_stat(19) - n1, 6 + MANY);
            }
        } else if (arm == 7) {
            nvn_vk_dbg_dset_cap(15);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "h1"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 H 第 1 轮输出不符 %d 槽", bad);
            C204_EQ("第 1 轮向驱动新分的组（1 + 2 + 4 + 8 = 15，到 15 就拒）", s1.nw - s0.nw, 15); C204_EQ("第 1 轮复用", s1.reuse - s0.reuse, 11);
            C204_EQ("第 1 轮交出", s1.taken - s0.taken, 15); C204_EQ("第 1 轮放回", s1.put - s0.put, 15);
            C204_EQ("第 1 轮到上限退回老路径的次数（70 - 15）", s1.capped - s0.capped, 55);
            C204_EQ("第 1 轮驱动里分配计数增量（新分 15 + 老路径 55）", (uint64_t)(s1.drv - s0.drv), 70); C204_EQ("第 1 轮后空闲串", s1.idle, 15);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "h2"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 H 第 2 轮输出不符 %d 槽", bad);
            C204_EQ("第 2 轮向驱动新分的组", s1.nw - s0.nw, 0); C204_EQ("第 2 轮复用", s1.reuse - s0.reuse, 15);
            C204_EQ("第 2 轮到上限退回老路径的次数", s1.capped - s0.capped, 55); C204_EQ("第 2 轮驱动里分配计数增量（全是老路径）", (uint64_t)(s1.drv - s0.drv), 55);
            // 放弃的段把取走的组还给池：在用加空闲的组数回落，上限的余量回来
            s0 = c204_now(); (void)c204_round(cp, buf, stride, 70, 1, &sig, "h3abandon"); s1 = c204_now();
            C204_EQ("放弃的段：还给池", s1.freed - s0.freed, 15); C204_EQ("放弃之后的空闲串", s1.idle, 0);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "h4"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 H 放弃之后第 1 轮输出不符 %d 槽", bad);
            C204_EQ("放弃之后向驱动新分的组（8 + 8，第 2 批取完是 16，比上限多 1 组）", s1.nw - s0.nw, 16);
            C204_EQ("放弃之后到上限退回老路径的次数（70 - 16）", s1.capped - s0.capped, 54); C204_EQ("放弃之后驱动里分配计数增量（新分 16 + 老路径 54）", (uint64_t)(s1.drv - s0.drv), 70);
            // 上限放开：回到全走回收表
            nvn_vk_dbg_dset_cap(1000);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "h5"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 H 上限放开后第 1 轮输出不符 %d 槽", bad);
            C204_EQ("上限放开：向驱动新分的组（16 组复用，其余 54 次派发要 7 批 8 组）", s1.nw - s0.nw, 56); C204_EQ("上限放开：到上限退回老路径的次数", s1.capped - s0.capped, 0);
            C204_EQ("上限放开：驱动里分配计数增量（只有新分的）", (uint64_t)(s1.drv - s0.drv), 56); C204_EQ("上限放开：空闲串", s1.idle, 72);
            // 稳态 300 轮（300 个段，比任何按段数滚的窗口都长）：不新分、一组也不还给池、不退回
            s0 = c204_now();
            for (int r = 0; r < 300; r++) {
                bad = c204_round(cp, buf, stride, 70, 0, &sig, "h6");
                if (bad) { CHECK(0, "C204 臂 H 稳态第 %d 轮输出不符 %d 槽", r, bad); break; }
            }
            s1 = c204_now();
            C204_EQ("稳态 300 轮向驱动新分的组", s1.nw - s0.nw, 0); C204_EQ("稳态 300 轮还给池的组（没有清扫）", s1.freed - s0.freed, 0);
            C204_EQ("稳态 300 轮到上限退回老路径的次数", s1.capped - s0.capped, 0); C204_EQ("稳态 300 轮后空闲串", s1.idle, 72);
            CHECK(s1.taken == s1.put + s1.freed, "C204 臂 H 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
            CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 H 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
        } else if (arm == 9) {
            uint64_t k0 = nvn_vk_dbg_dset_stat(6);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "j1"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 J 第 1 轮输出不符 %d 槽", bad);
            C204_EQ("第 1 轮登记的表项（只有第一次取组要登记）", nvn_vk_dbg_dset_stat(6) - k0, 1);
            C204_EQ("第 1 轮交出", s1.taken - s0.taken, 70); C204_EQ("第 1 轮放回", s1.put - s0.put, 70);
            for (int r = 2; r <= 4; r++) {
                k0 = nvn_vk_dbg_dset_stat(6); s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "j-steady"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 J 第 %d 轮输出不符 %d 槽", r, bad);
                C204_EQ("稳态每轮登记的表项", nvn_vk_dbg_dset_stat(6) - k0, 0);
                CHECK(s1.reuse - s0.reuse == 70 && s1.put - s0.put == 70 && s1.nw == s0.nw, "C204 臂 J 第 %d 轮：复用 70、放回 70、新分 0，实为 %llu / %llu / %llu", r,
                      (unsigned long long)(s1.reuse - s0.reuse), (unsigned long long)(s1.put - s0.put), (unsigned long long)(s1.nw - s0.nw));
            }
            k0 = nvn_vk_dbg_dset_stat(6); (void)c204_round(cp, buf, stride, 70, 1, &sig, "j-abandon");
            C204_EQ("放弃的段登记的表项", nvn_vk_dbg_dset_stat(6) - k0, 0);
            bad = c204_round(cp, buf, stride, 70, 0, &sig, "j-after-abandon");
            CHECK(bad == 0, "C204 臂 J 放弃之后输出不符 %d 槽", bad);
            nvn_gpu_layout_ent ce2[2]; memset(ce2, 0, sizeof ce2);   // 另一条布局不同（两项）的管线：自己的布局句柄、自己的表项
            ce2[0].set = 0; ce2[0].cls = NVN_GPU_RES_SSBO; ce2[0].access = NVN_GPU_ACC_W; ce2[0].binding = 0; ce2[0].count = 1; ce2[0].stages = 1u << NVN_GPU_SH_CS;
            ce2[1] = ce2[0]; ce2[1].binding = 1;
            nvn_gpu_layout cl2; memset(&cl2, 0, sizeof cl2); cl2.ent = ce2; cl2.nent = 2;
            nvn_gpu_shader sh2 = NULL; nvn_gpu_pipe cp2 = mk_cpipe(dir, "comp.comp", &cl2, &sh2);
            CHECK(cp2 != NULL, "C204 臂 J 第二条管线为空");
            if (cp2) {
                k0 = nvn_vk_dbg_dset_stat(6); bad = c204_round(cp2, buf, stride, 70, 0, &sig, "j2-1");
                CHECK(bad == 0, "C204 臂 J 第二条管线第 1 轮输出不符 %d 槽", bad);
                C204_EQ("第二条管线第 1 轮登记的表项", nvn_vk_dbg_dset_stat(6) - k0, 1);
                k0 = nvn_vk_dbg_dset_stat(6); bad = c204_round(cp2, buf, stride, 70, 0, &sig, "j2-2");
                CHECK(bad == 0, "C204 臂 J 第二条管线第 2 轮输出不符 %d 槽", bad);
                C204_EQ("第二条管线稳态登记的表项", nvn_vk_dbg_dset_stat(6) - k0, 0);
                C204_EQ("两条管线 = 两个表项", nvn_vk_dbg_dset_stat(6), 2);
            }
            if (sh2) nvn_gpu_shader_release(sh2);
            s1 = c204_now();
            CHECK(s1.taken == s1.put + s1.freed, "C204 臂 J 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
            CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 J 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
        } else if (arm == 10) {
            enum { NP = 40 };   // 管线条数：比 DSET 的任何常数都大，链表头插 40 次，丢项会让遍历数少于登记数
            nvn_gpu_shader shs[NP]; nvn_gpu_pipe cps[NP]; nvn_gpu_layout cls[NP]; nvn_gpu_layout_ent ces[NP][2];
            memset(shs, 0, sizeof shs); memset(cps, 0, sizeof cps); memset(cls, 0, sizeof cls); memset(ces, 0, sizeof ces);
            int made = 0;
            for (int i = 0; i < NP; i++) {
                ces[i][0] = ce[0]; ces[i][1] = ce[1]; ces[i][1].binding = 1 + (uint32_t)i;
                cls[i].ent = ces[i]; cls[i].nent = 2;   // 第二项的 binding 号各不相同：40 种布局结构（结构相同的布局共用一个，见臂 O），每条管线各占一个表项；40 种也让布局表翻倍重排两次（16 → 32 → 64 桶）
                cps[i] = mk_cpipe(dir, "comp.comp", &cls[i], &shs[i]);
                if (cps[i]) made++;
            }
            C204_EQ("40 条管线都建出来了", (uint64_t)made, NP);
            C204_EQ("40 种布局结构 + 开头那条管线（cp）的 1 种 = 布局表 41 项", nvn_vk_dbg_dset_stat(19), NP + 1);
            CHECK(nvn_vk_dbg_dset_stat(22) >= NP + 1 && nvn_vk_dbg_dset_stat(23) <= 8, "C204 臂 K 布局表随条数翻倍：41 项期望桶数 ≥ 41、最长链 ≤ 8 → 桶 %llu、最长链 %llu",
                  (unsigned long long)nvn_vk_dbg_dset_stat(22), (unsigned long long)nvn_vk_dbg_dset_stat(23));
            for (int r = 1; r <= 5; r++) {   // 5 轮、每轮取 3 组：第 1 轮每条管线要 1 + 2 = 3 组，之后刚好够用；放回错表项的毛病到第 2 轮就会让自己的空闲串见底、被迫再向驱动要组
                const uint64_t t0 = nvn_vk_dbg_dset_stat(6);
                int badn = 0;
                for (int i = 0; i < NP; i++) if (cps[i]) badn += c204_round(cps[i], buf, stride, 3, 0, &sig, "k") != 0;
                CHECK(badn == 0, "C204 臂 K 第 %d 轮：%d 条管线输出不符", r, badn);
                if (r == 1) C204_EQ("第 1 轮登记的表项（每条管线 1 项）", nvn_vk_dbg_dset_stat(6) - t0, NP);
                else C204_EQ("稳态每轮登记的表项（不重复登记）", nvn_vk_dbg_dset_stat(6) - t0, 0);
            }
            C204_EQ("表项数 = 管线数", nvn_vk_dbg_dset_stat(6), NP);
            C204_EQ("沿登记表遍历数出的表项数 == 登记的计数（链表没丢项）", nvn_vk_dbg_dset_stat(13), nvn_vk_dbg_dset_stat(6));
            s1 = c204_now();
            C204_EQ("五轮、每条管线每轮 3 组：交出", s1.taken, 5 * 3 * NP); C204_EQ("五轮放回", s1.put, 5 * 3 * NP);
            C204_EQ("每条管线只向驱动要过 3 组（批大小 1、2）", s1.nw, 3 * NP);
            C204_EQ("遍历数出的空闲组 = 每条管线 3 组（丢项会少）", s1.idle, 3 * NP);
            CHECK(s1.taken == s1.put + s1.freed, "C204 臂 K 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
            CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 K 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
            for (int i = 0; i < NP; i++) if (shs[i]) nvn_gpu_shader_release(shs[i]);
        } else if (arm == 11) {
            // 臂 L 开会话后注入了 70 次登记失败（c204_arm）：第 1 轮 70 次取组各撞上一次，全部退回老路径
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "l1"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 L 第 1 轮输出不符 %d 槽", bad);
            C204_EQ("登记失败：回收表交出", s1.taken - s0.taken, 0);
            C204_EQ("登记失败：驱动里每轮分配计数增量（老路径一组一次）", (uint64_t)(s1.drv - s0.drv), 70);
            C204_EQ("登记失败：表项数", nvn_vk_dbg_dset_stat(6), 0);
            C204_EQ("登记失败：新分", s1.nw - s0.nw, 0);
            s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "l2"); s1 = c204_now();
            CHECK(bad == 0, "C204 臂 L 第 2 轮输出不符 %d 槽", bad);
            C204_EQ("失败不记：第 2 轮登记成功、走回收表，交出", s1.taken - s0.taken, 70); C204_EQ("失败不记：表项数", nvn_vk_dbg_dset_stat(6), 1);
            C204_EQ("失败不记：第 2 轮向驱动要 11 批 71 组", s1.nw - s0.nw, 71);
            for (int r = 3; r <= 4; r++) {
                s0 = c204_now(); bad = c204_round(cp, buf, stride, 70, 0, &sig, "l-steady"); s1 = c204_now();
                CHECK(bad == 0, "C204 臂 L 第 %d 轮输出不符 %d 槽", r, bad);
                CHECK(s1.nw == s0.nw && s1.reuse - s0.reuse == 70 && s1.put - s0.put == 70, "C204 臂 L 第 %d 轮：新分 0、复用 70、放回 70，实为 %llu / %llu / %llu", r,
                      (unsigned long long)(s1.nw - s0.nw), (unsigned long long)(s1.reuse - s0.reuse), (unsigned long long)(s1.put - s0.put));
            }
            s1 = c204_now();
            CHECK(s1.taken == s1.put + s1.freed, "C204 臂 L 恒等式 交出 == 放回 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.taken, (unsigned long long)s1.put, (unsigned long long)s1.freed);
            CHECK(s1.nw == s1.idle + s1.freed, "C204 臂 L 恒等式 向驱动要来的 == 空闲 + 还给池：%llu == %llu + %llu", (unsigned long long)s1.nw, (unsigned long long)s1.idle, (unsigned long long)s1.freed);
        } else {
            s0 = c204_now(); nvn_vk_dbg_dset_hold(1);
            nvn_gpu_cmd c = begin_cmd();   // 不走 c204_round：持锁期间不能调用要锁的读数函数
            nvn_gpu_set_pipe(c, cp);
            nvn_gpu_binding b;
            memset(&b, 0, sizeof b);
            b.ent = 0; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.len = 64;
            memset(buf_cpu(buf), 0, (size_t)(stride * C204_SLOTS));
            for (int i = 0; i < 10; i++) { b.u.b.off = (uint64_t)i * stride; nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1); nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0); }
            for (int w = 0; w < 5000 && nvn_vk_dbg_dset_stat(11) - s0.busy < 10; w++) { struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL); }   // nvk_dq 开着时取组在录制线程里异步发生：等 10 次碰锁都记上再放锁
            const uint64_t busy_held = nvn_vk_dbg_dset_stat(11);
            nvn_vk_dbg_dset_hold(0);
            nvn_gpu_err e;
            nvn_gpu_rc r = nvn_gpu_cmd_commit(c, ++sig, on_done, NULL, &e);
            CHECK(OK(r), "C204 臂 I commit：%d %s", r.code, e.why);
            r = nvn_gpu_fence_wait(sig, 5000000000ull, &e);
            CHECK(OK(r), "C204 臂 I wait：%d %s", r.code, e.why);
            c204_settle(); s1 = c204_now();
            bad = 0;
            for (int k = 0; k < 10; k++) { const uint32_t *v = (const uint32_t *)(buf_cpu(buf) + k * stride); for (int i = 0; i < 16; i++) if (v[i] != (uint32_t)i + 1) { bad++; break; } }
            CHECK(bad == 0, "C204 臂 I 输出不符 %d 槽", bad);
            C204_EQ("取组碰锁退回老路径的次数", busy_held - s0.busy, 10); C204_EQ("回收表交出", s1.taken - s0.taken, 0);
            C204_EQ("碰锁退回老路径：驱动里分配计数增量", (uint64_t)(s1.drv - s0.drv), 10);
        }
    }
    nvn_vk_dbg_dset_inject(0);
    nvn_vk_dbg_dset_failreg(0);
    if (buf) nvn_gpu_buf_release(buf, NULL, NULL);
    if (sh) nvn_gpu_shader_release(sh);
    nvn_gpu_err ce_close; memset(&ce_close, 0, sizeof ce_close);
    CHECK(OK(nvn_gpu_close(5000000000ull, &ce_close)), "C204 臂 %c close：%d %s", 'A' + arm, ce_close.rc.code, ce_close.why);
    CHECK(nvn_vk_dbg_dset_stat(7) == 0 && nvn_vk_dbg_dset_stat(5) == 0 && nvn_vk_dbg_dset_stat(6) == 0, "C204 臂 %c 收尾后增长池 / 空闲 / 布局表项期望全 0 → %llu / %llu / %llu", 'A' + arm,
          (unsigned long long)nvn_vk_dbg_dset_stat(7), (unsigned long long)nvn_vk_dbg_dset_stat(5), (unsigned long long)nvn_vk_dbg_dset_stat(6));
    CHECK(nvn_vk_dbg_dset_stat(19) == 0 && nvn_vk_dbg_dset_stat(24) == nvn_vk_dbg_dset_stat(25), "C204 臂 %c 收尾后布局表期望 0 项、进程内累计建的布局 == 累计销毁的 → %llu 项、建 %llu / 销毁 %llu", 'A' + arm,
          (unsigned long long)nvn_vk_dbg_dset_stat(19), (unsigned long long)nvn_vk_dbg_dset_stat(24), (unsigned long long)nvn_vk_dbg_dset_stat(25));
    g_dsetrec_flag = -1;
}
static void c204_dset_recycle(const char *dir) {
    const char *only = getenv("S2A_TEST_C204_ARM");   // 调试：只跑某一个臂（A…N）
    for (int arm = 0; arm < 14; arm++) if (!only || only[0] - 'A' == arm) c204_arm(dir, arm);
    if (!only) printf("C204 描述符组按布局回收：A…N 十四个臂已核对\n");
}

// C208（#1233）：动态 UBO 偏移。备忘开着（nvk_desc_memo，缺省开）时，管线布局里的 UBO 个数不超过设备的动态 UBO 上限就全部走动态偏移，超了保持静态；备忘关着一律静态。
// 两条管线：P2 = 片元着色器读两个 UBO（set 0 binding 0、set 1 binding 3，故意不连号）；P9 = 一个组里 9 个 UBO（binding 0..8，只有 0 和 8 被着色器读、逐笔变，其余 7 个恒定）。
// 每个臂 (lim2, lim9) 开一个会话，建 P2 前把「设备动态 UBO 上限」压到 lim2、建 P9 前压到 lim9（宿主设备 lavapipe 的上限比真机大，要核「刚好在上限」「超一个」只能这样压）：
//   (1, 8) 两条都超 1 个 → 全静态；(2, 9) 两条都刚好 → 备忘开时全动态；(2, 8) P2 刚好 / P9 超一个 → 一动一静。
// 期望动态与否：dyn = 备忘开 && UBO 个数 ≤ 上限；用 nvn_vk_dbg_dyn_stat 的前后差核，不靠像素猜。
// P2 每笔绘制缩到一个像素（scissor），逐笔换（A 缓冲, 偏移, 范围）/（B 缓冲, 偏移），像素 i 的 R = 51 × A 项号、G = 51 × B 项号（0.2 × 255 = 51，没有舍入歧义；bc 的项号是反的：第 k 项 = 0.2(4-k)）：
//   0 首笔，两组都新；1 只换 A 的偏移；2 只换 B 的偏移；3 两个偏移都换；4 一模一样；5 A 的范围 16 → 32（内容变）；6 A 换缓冲（内容变）；7 两个偏移都换；
//   8 A 回到「ba、范围 16」；9 A 回到「bc、范围 32」。
// 向回收表取的组数（动态 + 备忘）：0 笔取 2、5 笔取 1、6 笔取 1，8 / 9 笔内容和本段此前写过的组一样 → 备忘命中、不取 = 共 4；静态：每笔换了偏移或内容的组都要重新取 = 2+1+1+2+0+1+1+2+1+2 = 13（偏移在键里，8 / 9 笔不命中）。
// P9 四笔 A 偏移 {0,1,1,3} / B 偏移 {0,0,2,4}：动态取 1（首笔）、静态取 4。两轮各一个命令段（第 2 轮 = 新段，首笔所有组重新取）。
struct c208_env { nvn_gpu_buf ba, bb, bc, vb, rb; nvn_gpu_tex out; nvn_gpu_view ov; };
static void c208_begin(nvn_gpu_cmd *cp, struct c208_env *E, nvn_gpu_pipe pipe) {
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = E->ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 10; p.h = 1; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_set_vbuf(c, 0, E->vb, 0);
    *cp = c;
}
static void c208_pixel_draw(nvn_gpu_cmd c, int i) {
    nvn_gpu_dyn dd;
    memset(&dd, 0, sizeof dd); dd.origin_ll = 1; dd.nvp = 1; dd.vp[0][2] = 10; dd.vp[0][3] = 1; dd.vp[0][5] = 1;
    dd.has_sc = 1; dd.sc[0] = i; dd.sc[1] = 0; dd.sc[2] = 1; dd.sc[3] = 1;
    nvn_gpu_set_dyn(c, &dd);
}
static void c208_draw(nvn_gpu_cmd c) {
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
}
static int c208_finish(nvn_gpu_cmd c, struct c208_env *E, uint64_t id, int npix, const uint8_t (*want)[4], const char *tag) {
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, E->out, 0, 0, NULL, E->rb, 0, 10 * 4, 40);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, id, on_done, NULL, &e);
    CHECK(OK(r), "C208 %s commit：%d %s", tag, r.code, e.why);
    r = nvn_gpu_fence_wait(id, 5000000000ull, &e);
    CHECK(OK(r), "C208 %s wait：%d %s", tag, r.code, e.why);
    c204_settle();
    const uint8_t *v = buf_cpu(E->rb);
    int bad = 0;
    for (int i = 0; i < npix; i++)
        if (memcmp(v + i * 4, want[i], 4)) {
            bad++;
            printf("C208 %s 像素 %d：期望 %u,%u,%u,%u → %u,%u,%u,%u\n", tag, i, want[i][0], want[i][1], want[i][2], want[i][3], v[i*4], v[i*4+1], v[i*4+2], v[i*4+3]);
        }
    CHECK(bad == 0, "C208 %s：%d 个像素期望全对 → %d 个不符", tag, npix, bad);
    return bad == 0;
}
static void c208_p2(struct c208_env *E, nvn_gpu_pipe pipe, int dyn, uint32_t lim, uint64_t sig0) {
    // 每笔：A 的（缓冲, 项号, 范围）、B 的（缓冲, 项号）；项号 × 256 = 偏移；B 范围恒为 16
    const struct { nvn_gpu_buf *a; int ka; uint64_t la; nvn_gpu_buf *b; int kb; } seq[10] = {
        {&E->ba, 0, 16, &E->bb, 0}, {&E->ba, 1, 16, &E->bb, 0}, {&E->ba, 1, 16, &E->bb, 2}, {&E->ba, 3, 16, &E->bb, 4}, {&E->ba, 3, 16, &E->bb, 4},
        {&E->ba, 3, 32, &E->bb, 4}, {&E->bc, 1, 32, &E->bb, 4}, {&E->bc, 2, 32, &E->bb, 1}, {&E->ba, 4, 16, &E->bb, 1}, {&E->bc, 0, 32, &E->bb, 3}};
    const unsigned want_taken = dyn ? 4u : 13u;
    for (int round = 0; round < 2; round++) {
        char tag[96]; snprintf(tag, sizeof tag, "P2 上限 %u 动态 %d 第 %d 轮", lim, dyn, round);
        const uint64_t t0 = nvn_vk_dbg_dset_stat(4), f0 = nvn_vk_dbg_dyn_stat(3);
        nvn_gpu_cmd c; c208_begin(&c, E, pipe);
        uint8_t want[10][4];
        for (int i = 0; i < 10; i++) {
            c208_pixel_draw(c, i);
            nvn_gpu_binding bs[2];
            memset(bs, 0, sizeof bs);
            bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_UBO; bs[0].u.b.buf = *seq[i].a; bs[0].u.b.off = (uint64_t)seq[i].ka * 256; bs[0].u.b.len = seq[i].la;
            bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_UBO; bs[1].u.b.buf = *seq[i].b; bs[1].u.b.off = (uint64_t)seq[i].kb * 256; bs[1].u.b.len = 16;
            nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 2);
            c208_draw(c);
            const int ra = seq[i].a == &E->bc ? 4 - seq[i].ka : seq[i].ka;
            want[i][0] = (uint8_t)(51 * ra); want[i][1] = (uint8_t)(51 * seq[i].kb); want[i][2] = 0; want[i][3] = 255;
        }
        c208_finish(c, E, sig0 + (uint64_t)round, 10, (const uint8_t (*)[4])want, tag);   // 提交号严格递增：每次调用错开
        const uint64_t taken = nvn_vk_dbg_dset_stat(4) - t0;
        if (nvn_vk_dbg_dset_stat(9))   // 回收关着（run.sh 的回收关档）时没有「向回收表取」这回事，只核像素
            CHECK(taken == want_taken, "C208 %s：这一段向回收表取的组数期望 %u → %llu", tag, want_taken, (unsigned long long)taken);
        // 「只换偏移」的近路：第 1、2 笔各 1 项，第 3 笔 2 项，第 7 笔 2 项，第 9 笔 1 项（共 7）；静态档一项也不走。
        // 备忘开着时这条近路换成「标脏 → 查备忘 → 命中」结果一样（取组数、像素都不变），只有这条读数能钉住它没被去掉。
        const uint64_t fast = nvn_vk_dbg_dyn_stat(3) - f0;
        CHECK(fast == (dyn ? 7u : 0u), "C208 %s：「只换偏移」直接放过的绑定项数期望 %u → %llu", tag, dyn ? 7u : 0u, (unsigned long long)fast);
    }
}
static void c208_p9(struct c208_env *E, nvn_gpu_pipe pipe, int dyn, uint32_t lim) {
    const int ka9[4] = {0, 1, 1, 3}, kb9[4] = {0, 0, 2, 4};
    char tag[96]; snprintf(tag, sizeof tag, "P9 上限 %u 动态 %d", lim, dyn);
    const uint64_t t0 = nvn_vk_dbg_dset_stat(4), f0 = nvn_vk_dbg_dyn_stat(3);
    nvn_gpu_cmd c; c208_begin(&c, E, pipe);
    uint8_t want[4][4];
    for (int i = 0; i < 4; i++) {
        c208_pixel_draw(c, i);
        nvn_gpu_binding bs[9];
        memset(bs, 0, sizeof bs);
        for (int k = 0; k < 9; k++) { bs[k].ent = (uint32_t)k; bs[k].cls = NVN_GPU_RES_UBO; bs[k].u.b.buf = E->ba; bs[k].u.b.len = 16; }
        bs[0].u.b.off = (uint64_t)ka9[i] * 256; bs[8].u.b.buf = E->bb; bs[8].u.b.off = (uint64_t)kb9[i] * 256;
        nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 9);
        c208_draw(c);
        want[i][0] = (uint8_t)(51 * ka9[i]); want[i][1] = (uint8_t)(51 * kb9[i]); want[i][2] = 0; want[i][3] = 255;
    }
    c208_finish(c, E, 220, 4, (const uint8_t (*)[4])want, tag);
    const uint64_t taken = nvn_vk_dbg_dset_stat(4) - t0;
    if (nvn_vk_dbg_dset_stat(9))
        CHECK(taken == (dyn ? 1u : 4u), "C208 %s：这一段向回收表取的组数期望 %u → %llu", tag, dyn ? 1u : 4u, (unsigned long long)taken);
    const uint64_t fast = nvn_vk_dbg_dyn_stat(3) - f0;   // 第 1 笔 A 换偏移、第 2 笔 B 换偏移、第 3 笔两个都换：共 4 项
    CHECK(fast == (dyn ? 4u : 0u), "C208 %s：「只换偏移」直接放过的绑定项数期望 %u → %llu", tag, dyn ? 4u : 0u, (unsigned long long)fast);
}
static void c208_p34(struct c208_env *E, nvn_gpu_pipe pipe) {
    // 34 个 UBO（两个组各 17 个）：超过 32 条的封顶，设备上限再大也保持静态（动态偏移串固定 32 项，不能溢出）；偏移在描述符里，像素照对
    const int ka[4] = {0, 1, 1, 3}, kb[4] = {0, 0, 2, 4};
    const uint64_t f0 = nvn_vk_dbg_dyn_stat(3);
    nvn_gpu_cmd c; c208_begin(&c, E, pipe);
    uint8_t want[4][4];
    for (int i = 0; i < 4; i++) {
        c208_pixel_draw(c, i);
        nvn_gpu_binding bs[34];
        memset(bs, 0, sizeof bs);
        for (int k = 0; k < 34; k++) { bs[k].ent = (uint32_t)k; bs[k].cls = NVN_GPU_RES_UBO; bs[k].u.b.buf = E->ba; bs[k].u.b.len = 16; }
        bs[0].u.b.off = (uint64_t)ka[i] * 256; bs[33].u.b.buf = E->bb; bs[33].u.b.off = (uint64_t)kb[i] * 256;
        nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 34);
        c208_draw(c);
        want[i][0] = (uint8_t)(51 * ka[i]); want[i][1] = (uint8_t)(51 * kb[i]); want[i][2] = 0; want[i][3] = 255;
    }
    c208_finish(c, E, 240, 4, (const uint8_t (*)[4])want, "P34");
    CHECK(nvn_vk_dbg_dyn_stat(3) == f0, "C208 P34（静态，34 个 UBO）：「只换偏移」直接放过的绑定项数期望 0 → %llu", (unsigned long long)(nvn_vk_dbg_dyn_stat(3) - f0));
}
// C208 拆分点用例（#1292 评审补两条：偏移高位非 0、对齐边界上的偏移拼回逐字节对）。宿主设备（lavapipe）单次分配上限 2 GiB、maxBufferSize 4 GiB − 1，
// 造不出 ≥ 4 GiB 的缓冲，偏移的高 32 位在宿主上恒为 0；这里用测试钩子 nvn_vk_dbg_dyn_split 把拆分点从 32 位降到 20 位（偏移低 20 位走动态偏移、其余放描述符），
// 同一段代码、同一套判据，4 MiB 缓冲里的偏移就有了非 0 的「高位」：缓冲整个填毒值（1.0 → 像素 255），只在九个偏移处放标记（项号 j：x = y = 0.2 j → 像素 51 j）；
// 九个偏移取在高位边界（1 MiB 的整倍数）两侧、都按 256 对齐（≥ minUniformBufferOffsetAlignment，和静态同一条）：低位最大（…FFF00）与低位为 0、为 0x100 各有。
// 拼错（高位丢了、低位多加一遍、两段拼反）读到的不是毒值就是别的标记，或者越过缓冲尾被校验层报错。
static const uint64_t c208_mo[9] = {0x000000, 0x0FFF00, 0x100000, 0x100100, 0x1FFF00, 0x200000, 0x2FFF00, 0x3FFF00, 0x300100};
static const int c208_mj[9] = {0, 1, 2, 3, 4, 1, 2, 3, 4};
static void c208_split(struct c208_env *E, nvn_gpu_buf bh, nvn_gpu_pipe pipe, int dyn, uint64_t sig0) {
    // 十笔的（A 偏移项, B 偏移项）；「只换偏移」直接放过的项：同缓冲、同范围、高位相同、偏移不同 = 第 1 笔 A、第 4 笔 B、第 5 笔 A、第 7 笔 A + B、第 9 笔 A + B，共 7 项；
    // 高位变的（第 2、3、6、8 笔）走脏组重写，不算。两轮各一个命令段（第 2 轮首笔所有组重新取）。
    static const int seq[10][2] = {{0, 1}, {1, 1}, {1, 2}, {3, 2}, {3, 4}, {4, 4}, {5, 6}, {6, 5}, {7, 8}, {8, 7}};
    for (int round = 0; round < 2; round++) {
        char tag[96]; snprintf(tag, sizeof tag, "拆分点 20 位 动态 %d 第 %d 轮", dyn, round);
        const uint64_t f0 = nvn_vk_dbg_dyn_stat(3);
        nvn_gpu_cmd c; c208_begin(&c, E, pipe);
        uint8_t want[10][4];
        for (int i = 0; i < 10; i++) {
            c208_pixel_draw(c, i);
            nvn_gpu_binding bs[2];
            memset(bs, 0, sizeof bs);
            bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_UBO; bs[0].u.b.buf = bh; bs[0].u.b.off = c208_mo[seq[i][0]]; bs[0].u.b.len = 16;
            bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_UBO; bs[1].u.b.buf = bh; bs[1].u.b.off = c208_mo[seq[i][1]]; bs[1].u.b.len = 16;
            nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 2);
            c208_draw(c);
            want[i][0] = (uint8_t)(51 * c208_mj[seq[i][0]]); want[i][1] = (uint8_t)(51 * c208_mj[seq[i][1]]); want[i][2] = 0; want[i][3] = 255;
        }
        c208_finish(c, E, sig0 + (uint64_t)round, 10, (const uint8_t (*)[4])want, tag);
        const uint64_t fast = nvn_vk_dbg_dyn_stat(3) - f0;
        CHECK(fast == (dyn ? 7u : 0u), "C208 %s：「只换偏移」直接放过的绑定项数期望 %u → %llu", tag, dyn ? 7u : 0u, (unsigned long long)fast);
    }
}
static void c208_arm(const char *dir, uint32_t lim2, uint32_t lim9) {
    if (!open_backend()) { CHECK(0, "C208 (%u,%u) 开会话失败", lim2, lim9); return; }
    struct c208_env E; memset(&E, 0, sizeof E);
    nvn_gpu_layout_ent fe[2], f9[9], f34[34];
    memset(fe, 0, sizeof fe); memset(f9, 0, sizeof f9); memset(f34, 0, sizeof f34);
    fe[0].set = 0; fe[0].binding = 0; fe[0].cls = NVN_GPU_RES_UBO; fe[0].access = NVN_GPU_ACC_R; fe[0].count = 1; fe[0].stages = 1u << NVN_GPU_SH_FS;
    fe[1] = fe[0]; fe[1].set = 1; fe[1].binding = 3;
    for (int k = 0; k < 9; k++) { f9[k].set = 0; f9[k].binding = (uint32_t)k; f9[k].cls = NVN_GPU_RES_UBO; f9[k].access = NVN_GPU_ACC_R; f9[k].count = 1; f9[k].stages = 1u << NVN_GPU_SH_FS; }
    for (int k = 0; k < 34; k++) { f34[k].set = k < 17 ? 0u : 1u; f34[k].binding = (uint32_t)(k < 17 ? k : k - 17); f34[k].cls = NVN_GPU_RES_UBO; f34[k].access = NVN_GPU_ACC_R; f34[k].count = 1; f34[k].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout fl, fl9, fl34;
    memset(&fl, 0, sizeof fl); memset(&fl9, 0, sizeof fl9); memset(&fl34, 0, sizeof fl34);
    fl.ent = fe; fl.nent = 2; fl9.ent = f9; fl9.nent = 9; fl34.ent = f34; fl34.nent = 34;
    nvn_gpu_shader vs = NULL, fs = NULL, vs9 = NULL, fs9 = NULL, vs34 = NULL, fs34 = NULL;
    void *vm = NULL, *fm = NULL, *vm9 = NULL, *fm9 = NULL, *vm34 = NULL, *fm34 = NULL;
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_pipe pipe2 = NULL, pipe9 = NULL, pipe34 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "ubo_dyn.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "C208 读不到 ubo_dyn"); goto done; }
    if (!load_shader_pair(dir, "tri3.vert", "ubo_dyn9.frag", NULL, &fl9, &vs9, &fs9, &vm9, &fm9)) { CHECK(0, "C208 读不到 ubo_dyn9"); goto done; }
    if (!load_shader_pair(dir, "tri3.vert", "ubo_dyn34.frag", NULL, &fl34, &vs34, &fs34, &vm34, &fm34)) { CHECK(0, "C208 读不到 ubo_dyn34"); goto done; }
    {
        float tab[8 * 64], tabc[8 * 64];   // 每项 256 字节（64 个 float）；ba / bb 的第 k 项：x = 0.2k、y = 0.2k；bc 的第 k 项：x = 0.2(4-k)（和 ba 反着来，换缓冲不换偏移也能从像素看出来）；其余 0
        memset(tab, 0, sizeof tab); memset(tabc, 0, sizeof tabc);
        for (int k = 0; k < 5; k++) { tab[k * 64 + 0] = 0.2f * (float)k; tab[k * 64 + 1] = 0.2f * (float)k; tabc[k * 64 + 0] = 0.2f * (float)(4 - k); tabc[k * 64 + 1] = 0.2f * (float)(4 - k); }
        E.ba = make_buf(sizeof tab, NVN_GPU_MEM_SHARED, tab); E.bb = make_buf(sizeof tab, NVN_GPU_MEM_SHARED, tab); E.bc = make_buf(sizeof tabc, NVN_GPU_MEM_SHARED, tabc);
    }
    E.vb = full_vb();
    E.out = make_tex(10, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); E.ov = make_view(E.out);
    E.rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    {
        nvn_gpu_pipe_desc pd, pd9, pd34;
        fill_gpipe_desc(&pd, vs, fs, 0); fill_gpipe_desc(&pd9, vs9, fs9, 0); fill_gpipe_desc(&pd34, vs34, fs34, 0);
        nvn_vk_dbg_dyn_limit(0);
        const uint64_t devlim = nvn_vk_dbg_dyn_stat(2);   // 宿主设备真值；压低只降不升，宿主上限不够 9 时「刚好在上限」的 P9 臂退化成超限臂（下面按实际生效的上限算期望）
        const uint32_t eff2 = lim2 < devlim ? lim2 : (uint32_t)devlim, eff9 = lim9 < devlim ? lim9 : (uint32_t)devlim;
        const int dyn2 = g_memo_on && eff2 >= 2, dyn9 = g_memo_on && eff9 >= 9;   // P2 有 2 个 UBO、P9 有 9 个
        nvn_vk_dbg_dyn_limit(lim2);
        const uint64_t dp0 = nvn_vk_dbg_dyn_stat(0), up0 = nvn_vk_dbg_dyn_stat(1), l2 = nvn_vk_dbg_dyn_stat(2);
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C208 (%u,%u) 建 P2：%d %s", lim2, lim9, e.rc.code, e.why);
        if (!pipe2) goto done;
        CHECK(nvn_vk_dbg_dyn_stat(1) - up0 == 1 && l2 == eff2 && nvn_vk_dbg_dyn_stat(0) - dp0 == (dyn2 ? 1u : 0u),
              "C208 (%u,%u) P2（2 个 UBO、上限 %llu、备忘 %d）：带 UBO 的管线增量期望 1 → %llu，选动态的管线增量期望 %u → %llu", lim2, lim9, (unsigned long long)l2, g_memo_on,
              (unsigned long long)(nvn_vk_dbg_dyn_stat(1) - up0), dyn2 ? 1u : 0u, (unsigned long long)(nvn_vk_dbg_dyn_stat(0) - dp0));
        nvn_vk_dbg_dyn_limit(lim9);
        const uint64_t dq0 = nvn_vk_dbg_dyn_stat(0), uq0 = nvn_vk_dbg_dyn_stat(1), l9 = nvn_vk_dbg_dyn_stat(2);
        CHECK(OK(nvn_gpu_pipe_get(&pd9, NVN_GPU_PIPE_SYNC, &pipe9, &e)), "C208 (%u,%u) 建 P9：%d %s", lim2, lim9, e.rc.code, e.why);
        if (!pipe9) goto done;
        CHECK(nvn_vk_dbg_dyn_stat(1) - uq0 == 1 && l9 == eff9 && nvn_vk_dbg_dyn_stat(0) - dq0 == (dyn9 ? 1u : 0u),
              "C208 (%u,%u) P9（9 个 UBO、上限 %llu、备忘 %d）：带 UBO 的管线增量期望 1 → %llu，选动态的管线增量期望 %u → %llu", lim2, lim9, (unsigned long long)l9, g_memo_on,
              (unsigned long long)(nvn_vk_dbg_dyn_stat(1) - uq0), dyn9 ? 1u : 0u, (unsigned long long)(nvn_vk_dbg_dyn_stat(0) - dq0));
        nvn_vk_dbg_dyn_limit(0);   // P34 不压上限：超过 32 条的封顶要在设备上限很大时才看得出
        const uint64_t dr0 = nvn_vk_dbg_dyn_stat(0), ur0 = nvn_vk_dbg_dyn_stat(1);
        CHECK(OK(nvn_gpu_pipe_get(&pd34, NVN_GPU_PIPE_SYNC, &pipe34, &e)), "C208 (%u,%u) 建 P34：%d %s", lim2, lim9, e.rc.code, e.why);
        if (!pipe34) goto done;
        CHECK(nvn_vk_dbg_dyn_stat(1) - ur0 == 1 && nvn_vk_dbg_dyn_stat(0) - dr0 == 0,
              "C208 (%u,%u) P34（34 个 UBO、设备上限 %llu）：带 UBO 的管线增量期望 1 → %llu，选动态的管线增量期望 0（超过 32 条封顶）→ %llu", lim2, lim9, (unsigned long long)devlim,
              (unsigned long long)(nvn_vk_dbg_dyn_stat(1) - ur0), (unsigned long long)(nvn_vk_dbg_dyn_stat(0) - dr0));
        c208_p2(&E, pipe2, dyn2, eff2, 208);
        c208_p9(&E, pipe9, dyn9, eff9);
        c208_p2(&E, pipe2, dyn2, eff2, 230);   // 回到 P2：动态 ↔ 静态管线切换后偏移不串
        c208_p34(&E, pipe34);
        {   // 拆分点用例：4 MiB 缓冲，毒值 1.0 铺满，九个标记偏移处放 0.2 j（x、y 两个分量；z、w 清 0）
            enum { HS = 4 << 20 };
            float *th = malloc(HS);
            if (!th) { CHECK(0, "C208 (%u,%u) 拆分点用例：4 MiB 分配失败", lim2, lim9); goto done; }
            for (size_t k = 0; k < HS / sizeof(float); k++) th[k] = 1.0f;
            for (int k = 0; k < 9; k++) { float *q = th + c208_mo[k] / sizeof(float); q[0] = q[1] = 0.2f * (float)c208_mj[k]; q[2] = q[3] = 0.0f; }
            nvn_gpu_buf bh = make_buf(HS, NVN_GPU_MEM_SHARED, th);
            free(th);
            if (!bh) { CHECK(0, "C208 (%u,%u) 拆分点用例：建 4 MiB 缓冲失败", lim2, lim9); goto done; }
            nvn_vk_dbg_dyn_split(20);
            c208_split(&E, bh, pipe2, dyn2, 250);
            nvn_vk_dbg_dyn_split(0);
            nvn_gpu_buf_release(bh, NULL, NULL);
        }
        printf("C208 (%u,%u) 臂：设备动态 UBO 上限 %llu，压到 %u / %u 后 P2 动态 %d、P9 动态 %d\n", lim2, lim9, (unsigned long long)devlim, eff2, eff9, dyn2, dyn9);
        if (lim9 >= 9 && eff9 < 9) printf("C208 注意：宿主动态 UBO 上限 %llu < 9，(%u,%u) 臂里「刚好在上限」的 P9 退化成超限臂\n", (unsigned long long)devlim, lim2, lim9);
    }
done:
    nvn_vk_dbg_dyn_limit(0);   // 还原设备真值
    nvn_vk_dbg_dyn_split(0);   // 还原 32 位拆分点
    if (E.rb) nvn_gpu_buf_release(E.rb, NULL, NULL);
    if (E.ov) nvn_gpu_view_release(E.ov);
    if (E.out) nvn_gpu_tex_release(E.out);
    if (E.vb) nvn_gpu_buf_release(E.vb, NULL, NULL);
    if (E.ba) nvn_gpu_buf_release(E.ba, NULL, NULL);
    if (E.bb) nvn_gpu_buf_release(E.bb, NULL, NULL);
    if (E.bc) nvn_gpu_buf_release(E.bc, NULL, NULL);
    if (vs) nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    if (vs9) nvn_gpu_shader_release(vs9);
    if (fs9) nvn_gpu_shader_release(fs9);
    if (vs34) nvn_gpu_shader_release(vs34);
    if (fs34) nvn_gpu_shader_release(fs34);
    free(vm); free(fm); free(vm9); free(fm9); free(vm34); free(fm34);
    nvn_gpu_err ce; memset(&ce, 0, sizeof ce);
    CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "C208 (%u,%u) close：%d %s", lim2, lim9, ce.rc.code, ce.why);
}
// 绑定号 ≥ 256：布局合并表（backend_vulkan.c struct merged）原先把 binding 存成 uint8_t，262 变成 6。库程序保留全部 arg_id，绑定号可到 262+，
//   Adreno 链接时报「Bind group info mismatches the shader source」、管线建失败、整类绘制被失败表挡下（8E 全黑）。夹具里 262 与 6 同时存在（低 8 位相同，
//   截断会把两项并成一项）：管线必须建成，两个 UBO 各自取到自己的值。
static void t_high_binding(const char *dir) {
    if (!open_backend()) { CHECK(0, "高绑定号 开会话失败"); return; }
    nvn_gpu_layout_ent fe[2];
    memset(fe, 0, sizeof fe);
    fe[0].set = 0; fe[0].binding = 262; fe[0].cls = NVN_GPU_RES_UBO; fe[0].access = NVN_GPU_ACC_R; fe[0].count = 1; fe[0].stages = 1u << NVN_GPU_SH_FS;
    fe[1] = fe[0]; fe[1].binding = 6;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = fe; fl.nent = 2;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_pipe pipe = NULL;
    struct c208_env E; memset(&E, 0, sizeof E);
    if (!load_shader_pair(dir, "tri3.vert", "ubo_high.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "高绑定号 读不到 ubo_high"); goto done; }
    {
        nvn_gpu_pipe_desc pd;
        fill_gpipe_desc(&pd, vs, fs, 0);
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "高绑定号 建管线：%d %s", e.rc.code, e.why);
        if (!pipe) goto done;
    }
    {
        const float va[4] = {0.2f, 0, 0, 0}, vb[4] = {0, 0.6f, 0, 0};
        E.ba = make_buf(sizeof va, NVN_GPU_MEM_SHARED, va); E.bb = make_buf(sizeof vb, NVN_GPU_MEM_SHARED, vb);
        E.vb = full_vb();
        E.out = make_tex(10, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); E.ov = make_view(E.out);
        E.rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_cmd c; c208_begin(&c, &E, pipe);
        c208_pixel_draw(c, 0);
        nvn_gpu_binding bs[2];
        memset(bs, 0, sizeof bs);
        bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_UBO; bs[0].u.b.buf = E.ba; bs[0].u.b.len = 16;
        bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_UBO; bs[1].u.b.buf = E.bb; bs[1].u.b.len = 16;
        nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 2);
        c208_draw(c);
        const uint8_t want[1][4] = {{51, 153, 0, 255}};   // 0.2 × 255、0.6 × 255
        c208_finish(c, &E, 400, 1, want, "高绑定号 262 / 6");
    }
done:
    if (E.rb) nvn_gpu_buf_release(E.rb, NULL, NULL);
    if (E.ov) nvn_gpu_view_release(E.ov);
    if (E.out) nvn_gpu_tex_release(E.out);
    if (E.vb) nvn_gpu_buf_release(E.vb, NULL, NULL);
    if (E.ba) nvn_gpu_buf_release(E.ba, NULL, NULL);
    if (E.bb) nvn_gpu_buf_release(E.bb, NULL, NULL);
    if (vs) nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    free(vm); free(fm);
    nvn_gpu_err ce; memset(&ce, 0, sizeof ce);
    CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "高绑定号 close：%d %s", ce.rc.code, ce.why);
}
static void c208_dyn_ubo(const char *dir) {
    c208_arm(dir, 1, 8);   // 两条管线都超上限 1 个：全静态
    if (!getenv("S2A_TEST_DESC_MEMO"))   // 没指定环境变量：后端读 nvk_desc_memo 时传的缺省值必须是开（备忘 + 动态 UBO 缺省开，#1233；建管线时读过了）
        CHECK(g_memo_dflt_seen == 1, "C208 nvk_desc_memo 的缺省值期望 1（缺省开）→ %d（-1 = 后端没读过）", g_memo_dflt_seen);
    CHECK(g_dq_dflt_seen == 1, "C-DQ-DFLT nvk_dq 的缺省值期望 1（两级回放缺省开）→ %d（-1 = 后端没读过）", g_dq_dflt_seen);
    c208_arm(dir, 2, 9);   // 两条都刚好在上限上：备忘开时全动态
    c208_arm(dir, 2, 8);   // P2 刚好 / P9 超 1 个：一动一静
    c208_arm(dir, 8, 8);   // 设备只报规范最小值 8（不少 Mali 如此，#1292 评审补）：2 个 UBO 的 P2 动态、9 个 UBO 的 P9 超 1 个保持静态；像素与全静态臂的期望相同、校验层 0 条
    t_high_binding(dir);   // 绑定号 ≥ 256（库程序保留全部 arg_id）：布局合并表不许把 binding 截成 8 位
    printf("C208 动态 UBO 偏移（备忘%s）：四个臂（全静态 / 全动态 / 一动一静 / 上限 8）与拆分点 20 位的高位偏移已核对\n", g_memo_on ? "开" : "关，一律静态");
}

// C67：origin_ll=1（NVN 窗口原点在左下）时后端不翻视口：这种约定下图是自下而上存的，行号 0 就是「下」，
// 视口 {0,0,64,32} 落在行 0–31；呈现时由 present_desc.flip_y 整张翻过来。原点在左上的翻转见 C73。
static void c67_viewport_yflip(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C67 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;   // 蓝底衬绿
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 32; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.origin_ll = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 113, on_done, NULL, &e);
    CHECK(OK(r), "C67 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(113, 5000000000ull, &e);
    CHECK(OK(r), "C67 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 0, 255, 0, "C67 行 0–31（视口内，绿）");
    expect_rect(buf_cpu(rb), 0, 32, 64, 32, 0, 0, 255, "C67 行 32–63（视口外，保持蓝底）");
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C68：模板的比较函数 / 三种操作进管线，读写掩码与参考值走动态状态。两笔：
//   ① 只在左半屏（裁剪）画绿：函数 ALWAYS、通过时 REPLACE、参考值 1 → 左半屏模板变 1；
//   ② 整屏画蓝：函数 NOTEQUAL、参考值 1、操作全 KEEP → 只有模板还是 0 的右半屏画得上。
// 期望（手算）：左半绿、右半蓝。函数没进管线（全 0 = NEVER）时两笔都画不上，整屏是黑的清屏色。
static void c68_stencil_ops(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.sfmt = NVN_FORMAT_DEPTH24_STENCIL8; pd.ds.st = 1;
    pd.ds.sf_f = 8; pd.ds.sf_b = 8;            // NVN StencilFunc：8 = ALWAYS
    pd.ds.fail_f = 2; pd.ds.zfail_f = 2; pd.ds.zpass_f = 3;   // NVN op 值 = MTLStencilOperation+1：2=ZERO、3=REPLACE
    pd.ds.fail_b = 2; pd.ds.zfail_b = 2; pd.ds.zpass_b = 3;
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C68 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader vs2 = NULL, fs2 = NULL; void *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd2;
    fill_gpipe_desc(&pd2, vs2, fs2, NVN_FORMAT_DEPTH24_STENCIL8);
    pd2.sfmt = NVN_FORMAT_DEPTH24_STENCIL8; pd2.ds.st = 1;
    pd2.ds.sf_f = 6; pd2.ds.sf_b = 6;          // 6 = NOTEQUAL
    pd2.ds.fail_f = pd2.ds.zfail_f = pd2.ds.zpass_f = 1;   // 1 = KEEP
    pd2.ds.fail_b = pd2.ds.zfail_b = pd2.ds.zpass_b = 1;
    nvn_gpu_pipe pipe2 = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C68 pipe2：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f; p.clear_stencil = 0;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF;
    dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    dyn.st_ref[0] = dyn.st_ref[1] = 1;
    dyn.sc[2] = 32;   // ① 只画左半屏
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_set_pipe(c, pipe2);   // ② 整屏、NOTEQUAL 1
    dyn.sc[2] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 114, on_done, NULL, &e);
    CHECK(OK(r), "C68 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(114, 5000000000ull, &e);
    CHECK(OK(r), "C68 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C68 左半（①写了模板 1，②被 NOTEQUAL 挡住：绿）");
    expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C68 右半（模板还是 0，②画上：蓝）");
    nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2); free(vm2); free(fm2);
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C1581K：深度模板贴图的 CPU 上传 / 回读（#1581 K，VUID-VkBufferImageCopy-aspectMask-09103：一个拷贝区域只能有一个面）。
//   上传 64×64 的 D24S8：每纹素一个 32 位字，低 24 位深度 = 0x400000 + x，高 8 位模板 = 左半 1、右半 0。
//   ① 回读深度面：每纹素低 24 位 = 0x400000 + x（高 8 位按规范未定义，不比）；
//   ② 模板载入（LOAD）画整屏蓝：函数 NOTEQUAL、参考值 1、操作全 KEEP、深度测试关 → 只有模板是 0 的右半画上。
// 期望（手算）：左半黑（清屏色）、右半蓝。模板面没传上去（全 0）时整屏蓝；aspectMask 同时带深度与模板时 validation 报 09103。
// subst = 1：#398 D24 换 D32 存储（三星 Xclipse 那类设备）。深度面走换算，按字节回读本就不支持（E_UNSUPPORTED），这一臂只核模板面。
//   设备自己的换存储掩码（vk_device 按格式能力定的）每臂结束原样还回去；设备本就没有 D24S8 附件（模拟档 no_d24）时原生臂不跑。
extern void nvn_vk_format_set_depth_subst(uint32_t mask);
extern uint32_t nvn_vk_format_depth_subst(void);
static void c1581k_ds_upload_arm(const char *dir, int subst) {
    const uint32_t dev_mask = nvn_vk_format_depth_subst();
    if (!subst && (dev_mask & 1u /* NVN_VK_DEPTH_SUBST_D24S8 */)) { printf("C1581K 原生 D24S8 臂：设备没有 D24S8 附件、按 D32 存储，跳过（换存储臂照跑）\n"); return; }
    nvn_vk_format_set_depth_subst(subst ? (dev_mask | 1u) : dev_mask);
    if (!(nvn_gpu_format_caps(NVN_FORMAT_DEPTH24_STENCIL8) & NVN_GPU_FMT_DEPTH)) { CHECK(0, "C1581K 设备没有 D24S8 深度格式"); nvn_vk_format_set_depth_subst(dev_mask); return; }
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C1581K 读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.sfmt = NVN_FORMAT_DEPTH24_STENCIL8; pd.ds.st = 1;
    pd.ds.sf_f = pd.ds.sf_b = 6;                                   // 6 = NOTEQUAL
    pd.ds.fail_f = pd.ds.zfail_f = pd.ds.zpass_f = 1;              // 1 = KEEP
    pd.ds.fail_b = pd.ds.zfail_b = pd.ds.zpass_b = 1;
    if (subst) pd.ds.zfail_f = 2;   // 深度测试关、深度失败不会发生：只为换一个管线键，不沿用上一臂按 D24 附件建的管线
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C1581K pipe：%d %s", e.rc.code, e.why);
    static uint32_t packed[64 * 64];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) packed[y * 64 + x] = (0x400000u + (uint32_t)x) | ((x < 32 ? 1u : 0u) << 24);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rbd = make_buf(sizeof packed, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, ds, 0, 0, NULL, packed, 64 * 4, sizeof packed);
    if (!subst) nvn_gpu_copy_tex_buf(c, ds, 0, 0, NULL, rbd, 0, 64 * 4, sizeof packed);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_LOAD; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_LOAD; p.stencil.store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF; dyn.st_ref[0] = dyn.st_ref[1] = 1;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1u + (uint64_t)subst, on_done, NULL, &e);   // 自己一个会话（见 main）：两臂围栏号 1、2
    CHECK(OK(r), "C1581K commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1u + (uint64_t)subst, 5000000000ull, &e);
    CHECK(OK(r), "C1581K wait：%d %s", r.code, e.why);
    if (OK(r) && !subst) {
        const uint32_t *d = (const uint32_t *)(const void *)buf_cpu(rbd); int bad = -1;
        for (int i = 0; i < 64 * 64 && bad < 0; i++) if ((d[i] & 0xFFFFFFu) != (packed[i] & 0xFFFFFFu)) bad = i;
        CHECK(bad < 0, "C1581K 深度面回读 [%d]=%06x，期望 %06x", bad, bad < 0 ? 0 : d[bad] & 0xFFFFFFu, bad < 0 ? 0 : packed[bad] & 0xFFFFFFu);
    }
    if (OK(r)) {
        expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 0, 0, "C1581K 左半（上传的模板 1，NOTEQUAL 挡住：黑）");
        expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C1581K 右半（上传的模板 0，画上：蓝）");
    }
    printf("C1581K D24S8 上传 / 回读（%s）：已核对\n", subst ? "D24 换 D32 存储" : "原生 D24S8");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_buf_release(rbd, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
    nvn_vk_format_set_depth_subst(dev_mask);
}
static void c1581k_ds_upload(const char *dir) { c1581k_ds_upload_arm(dir, 0); c1581k_ds_upload_arm(dir, 1); }

// C1581O：纯模板格式 STENCIL8（VK_FORMAT_S8_UINT）当模板附件（#1581 O）。S8 只有模板面：图像的面、默认视图的面都得是 STENCIL；
//   管线的兼容渲染通道也要有这个附件（管线描述 dfmt = 0、sfmt = STENCIL8，附件格式不能只看 dfmt）。两笔同 C68：
//   ① 只在左半屏（裁剪）画绿：函数 ALWAYS、通过时 REPLACE、参考值 1；② 整屏画蓝：函数 NOTEQUAL、参考值 1、操作全 KEEP。
// 期望（手算）：左半绿、右半蓝，validation 0 条。面判成 DEPTH 时建视图报 VUID；管线通道没有模板附件时绘制报通道不兼容。
static void c1581o_stencil8(const char *dir) {
    if (!(nvn_gpu_format_caps(NVN_FORMAT_STENCIL8) & NVN_GPU_FMT_RENDER)) { CHECK(0, "C1581O 设备不能把 STENCIL8 当附件"); return; }
    nvn_gpu_shader vs = NULL, fs = NULL, vs2 = NULL, fs2 = NULL; void *vm = NULL, *fm = NULL, *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C1581O 读不到 solid_green"); return; }
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "C1581O 读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd, pd2;
    fill_gpipe_desc(&pd, vs, fs, 0); pd.sfmt = NVN_FORMAT_STENCIL8; pd.ds.st = 1;
    pd.ds.sf_f = pd.ds.sf_b = 8;                                                     // 8 = ALWAYS
    pd.ds.fail_f = pd.ds.zfail_f = 2; pd.ds.zpass_f = 3; pd.ds.fail_b = pd.ds.zfail_b = 2; pd.ds.zpass_b = 3;   // 2 = ZERO、3 = REPLACE
    fill_gpipe_desc(&pd2, vs2, fs2, 0); pd2.sfmt = NVN_FORMAT_STENCIL8; pd2.ds.st = 1;
    pd2.ds.sf_f = pd2.ds.sf_b = 6;                                                   // 6 = NOTEQUAL
    pd2.ds.fail_f = pd2.ds.zfail_f = pd2.ds.zpass_f = 1; pd2.ds.fail_b = pd2.ds.zfail_b = pd2.ds.zpass_b = 1;   // 1 = KEEP
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_pipe pipe = NULL, pipe2 = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C1581O pipe：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C1581O pipe2：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex st = make_tex(64, 64, NVN_FORMAT_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), sv = make_view(st);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.stencil.view = sv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE; p.clear_stencil = 0;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF; dyn.st_ref[0] = dyn.st_ref[1] = 1;
    dyn.sc[2] = 32;   // ① 只画左半屏
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_set_pipe(c, pipe2);   // ② 整屏、NOTEQUAL 1
    dyn.sc[2] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);   // 自己一个会话（见 main）：围栏号 1
    CHECK(OK(r), "C1581O commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
    CHECK(OK(r), "C1581O wait：%d %s", r.code, e.why);
    if (OK(r)) {
        expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C1581O 左半（①写了模板 1，②被 NOTEQUAL 挡住：绿）");
        expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C1581O 右半（模板还是 0，②画上：蓝）");
    }
    printf("C1581O 纯 STENCIL8 模板附件：已核对\n");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(sv);
    nvn_gpu_tex_release(out); nvn_gpu_tex_release(st);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
    nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2); free(vm2); free(fm2);
}

// 平铺在 z = 0.5 的整屏三角形（C74 / C75 的深度测试用）
static nvn_gpu_buf half_depth_vb(void) {
    const float verts[3][4] = {{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}};
    return make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
}

// C76：只有顶点阶段的管线（没有片元着色器）在只有深度附件的通道里写深度。深度清成 1，整屏三角形平铺在 z = 0.5，深度函数 ALWAYS、写深度。
// 期望（手算）：提交成功；读回的 64×64 个 32 位浮点全是 0.5。管线没建成或没写深度则仍是 1。
static void c76_vs_only_depth(const char *dir) {
    nvn_gpu_shader vs = load_one_shader(dir, "tri3.vert", NVN_GPU_SH_VS, NULL);
    if (!vs) return;
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, NULL, NVN_FORMAT_DEPTH32F);
    pd.cfmt[0] = 0; pd.chmask.mask = 0;
    pd.ds.dt = 1; pd.ds.dw = 1; pd.ds.df = 8;   // 深度测试开、写深度、ALWAYS
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C76 pipe：%d %s", e.rc.code, e.why);
    if (!pipe) { nvn_gpu_shader_release(vs); return; }
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = half_depth_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.depth.view = v; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE; p.clear_depth = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn); nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 118, on_done, NULL, &e);
    CHECK(OK(r), "C76 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(118, 5000000000ull, &e);
    CHECK(OK(r), "C76 wait：%d %s", r.code, e.why);
    { const float *f = (const float *)buf_cpu(rb); int bad = -1; for (int i = 0; i < 64 * 64; i++) if (f[i] != 0.5f) { bad = i; break; }
      CHECK(bad < 0, "C76 深度读回第 %d 个 = %g，期望 0.5", bad, bad < 0 ? 0.0 : (double)f[bad]); }
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs);
}

// C77：颜色槽里有空位的通道（槽 0 没有附件、槽 1 有）。槽 1 清成蓝，片元着色器只写输出 1（绿），整屏画一笔。
// 期望（手算）：槽 1 的图整屏绿。空位没处理对的话：通道 / 管线建不出来（提交失败），或者写到别处、槽 1 仍是蓝。
// C78：颜色写掩码为 0 的绘制一个通道都不写。同一张图清成蓝后，用写掩码 0 的管线整屏画绿。期望：仍然整屏蓝。
// C197：通道对象缓存。同一组目标反复开通道，渲染通道对象和帧缓冲只建一次；视图销毁后帧缓冲作废、换新视图再开要重建一个，
// 渲染通道对象（只看附件描述）不重建。期望的次数按「开了几次、目标变没变」数出来，不看实现。
uint64_t nvn_vk_dbg_created(int which);
static void c197_pass_cache(void) {
    nvn_gpu_err e; nvn_gpu_rc r;
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1; p.color[0].clear.f[3] = 1; p.w = 64; p.h = 64; p.layers = 1;
    uint64_t rp0 = nvn_vk_dbg_created(0), fb0 = nvn_vk_dbg_created(1);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    r = nvn_gpu_cmd_commit(c, 197, on_done, NULL, &e); CHECK(OK(r), "C197 commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(197, 5000000000ull, &e); CHECK(OK(r), "C197 wait 1：%d %s", r.code, e.why);
    CHECK(nvn_vk_dbg_created(0) - rp0 <= 1 && nvn_vk_dbg_created(1) - fb0 == 1, "C197 同一段里同一目标开两次：期望新建渲染通道至多 1（前面的用例建过同样的附件描述就是 0）、帧缓冲 1 → %llu、%llu",
          (unsigned long long)(nvn_vk_dbg_created(0) - rp0), (unsigned long long)(nvn_vk_dbg_created(1) - fb0));
    uint64_t rp1 = nvn_vk_dbg_created(0);
    c = begin_cmd();
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    r = nvn_gpu_cmd_commit(c, 198, on_done, NULL, &e); CHECK(OK(r), "C197 commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(198, 5000000000ull, &e); CHECK(OK(r), "C197 wait 2：%d %s", r.code, e.why);
    CHECK(nvn_vk_dbg_created(0) == rp1 && nvn_vk_dbg_created(1) - fb0 == 1, "C197 下一段再开同一目标：期望渲染通道不再新建、帧缓冲仍是 1 → %llu、%llu",
          (unsigned long long)(nvn_vk_dbg_created(0) - rp0), (unsigned long long)(nvn_vk_dbg_created(1) - fb0));
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 255, 0, 0, "C197 沿用缓存的帧缓冲清成红");
    nvn_gpu_view_release(ov);
    nvn_gpu_view ov2 = make_view(out);
    p.color[0].view = ov2; p.color[0].clear.f[0] = 0; p.color[0].clear.f[2] = 1;
    c = begin_cmd();
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    r = nvn_gpu_cmd_commit(c, 199, on_done, NULL, &e); CHECK(OK(r), "C197 commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(199, 5000000000ull, &e); CHECK(OK(r), "C197 wait 3：%d %s", r.code, e.why);
    CHECK(nvn_vk_dbg_created(0) == rp1 && nvn_vk_dbg_created(1) - fb0 == 2, "C197 旧视图销毁、换新视图再开：期望渲染通道不再新建、帧缓冲 2 → %llu、%llu",
          (unsigned long long)(nvn_vk_dbg_created(0) - rp0), (unsigned long long)(nvn_vk_dbg_created(1) - fb0));
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 0, 255, "C197 新视图的帧缓冲清成蓝");
    nvn_gpu_view_release(ov2); nvn_gpu_tex_release(out);
}

// C201：视图销毁的两处线性代价（真机世界场景：现存视图 2.3 万个、每帧销毁 56 个，这两处合起来占去掉托底线程后全部 CPU 样本的 23.9%，全程持后端大锁）。
// ① 全局视图链：创建时头插、销毁时摘。原先摘链从链头线性找自己；现在按反向指针 O(1) 摘。
//    期望（手算）：同一张贴图上建 N=1500 个视图，按「最旧先拆」逐个销毁（最旧的在链尾，旧做法的最坏顺序）——
//      旧做法（nvk_ulist=0，保留作对照）摘链走过的节点数 = (N-1)+(N-2)+…+1+0 = N(N-1)/2 = 1124250；缺省做法 = 0；
//      两种做法每拆一个现存视图数减 1，链结构（前驱指针指回自己、后继的反向指针指回本节点）零违例。
//    再按 k×773 mod N 的次序（773 与 1500 互质，每个下标恰好一次）拆一轮：链头 / 链中 / 链尾都摘到，链结构仍零违例。
// ② 帧缓冲缓存的反查表：视图记着自己当过哪些帧缓冲的附件，销毁时只拆那几个。视图从没当过附件时，原先也退回扫整张 8192 项的表。
//    期望（手算）：缺省档销毁「从没当过附件」的视图，全表扫次数 +0（旧做法 nvk_fbdrop=0：至少 +1）；
//    销毁「当过附件」的视图，缓存里恰好少 1 个帧缓冲（只有一个帧缓冲含它），全表扫仍 +0；
//    颜色 + 深度两个附件的通道：先销毁深度视图，帧缓冲少 1；再用新深度视图开同一个颜色视图（槽可能复用、代数不同）后销毁颜色视图，新帧缓冲也要拆掉。
uint64_t nvn_vk_dbg_view_stat(int which);
static void c201_set_flags(int ulist, int fbdrop) { g_ulist_flag = ulist; g_fbdrop_flag = fbdrop; (void)nvn_vk_dbg_view_stat(5); }
static void c201_check_chain(const char *what, int upto) {
    uint64_t bad = nvn_vk_dbg_view_stat(4);
    CHECK(bad == 0, "C201 %s（已拆 %d 个）：视图链结构违例期望 0 → %llu", what, upto, (unsigned long long)bad);
}
static void c201_view_list(void) {
    enum { N = 1500, STEP = 773 };
    static nvn_gpu_view vs[N];
    nvn_gpu_tex t = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    for (int arm = 0; arm < 2; arm++) {   // 0 = 旧做法（对照）、1 = 缺省
        c201_set_flags(arm == 0 ? 0 : -1, -1);   // 臂 1 读生产缺省（不显式设值）：缺省被改掉时这里会红
        const uint64_t live0 = nvn_vk_dbg_view_stat(0), steps0 = nvn_vk_dbg_view_stat(1);
        for (int i = 0; i < N; i++) vs[i] = make_view(t);
        CHECK(nvn_vk_dbg_view_stat(0) - live0 == N, "C201 臂 %d 建 %d 个视图：期望现存视图 +%d → +%llu", arm, N, N, (unsigned long long)(nvn_vk_dbg_view_stat(0) - live0));
        c201_check_chain("建完", 0);
        for (int i = 0; i < N; i++) {   // 最旧先拆
            nvn_gpu_view_release(vs[i]);
            if (i < 64 || i % 50 == 0) { c201_check_chain("最旧先拆", i + 1); if (nvn_vk_dbg_view_stat(4)) break; }
        }
        const uint64_t steps = nvn_vk_dbg_view_stat(1) - steps0, want = arm == 0 ? (uint64_t)N * (N - 1) / 2 : 0;
        printf("C201 视图摘链（%s）：最旧先拆 %d 个，摘链走过 %llu 个节点（期望 %llu）\n", arm == 0 ? "旧做法 nvk_ulist=0" : "缺省", N, (unsigned long long)steps, (unsigned long long)want);
        CHECK(steps == want, "C201 臂 %d 最旧先拆 %d 个：摘链走过的节点数期望 %llu → %llu", arm, N, (unsigned long long)want, (unsigned long long)steps);
        CHECK(nvn_vk_dbg_view_stat(0) == live0, "C201 臂 %d 拆完：现存视图数期望回到 %llu → %llu", arm, (unsigned long long)live0, (unsigned long long)nvn_vk_dbg_view_stat(0));
        for (int i = 0; i < N; i++) vs[i] = make_view(t);
        for (int k = 0; k < N; k++) {   // 乱序拆
            nvn_gpu_view_release(vs[(int)(((long)k * STEP) % N)]);
            if (k < 64 || k % 50 == 0) { c201_check_chain("乱序拆", k + 1); if (nvn_vk_dbg_view_stat(4)) break; }
            if (k == N / 2) CHECK(nvn_vk_dbg_view_stat(0) == live0 + (uint64_t)(N - (N / 2 + 1)), "C201 臂 %d 乱序拆一半：现存视图数期望 %llu → %llu", arm, (unsigned long long)(live0 + (uint64_t)(N - (N / 2 + 1))), (unsigned long long)nvn_vk_dbg_view_stat(0));
        }
        CHECK(nvn_vk_dbg_view_stat(0) == live0, "C201 臂 %d 乱序拆完：现存视图数期望回到 %llu → %llu", arm, (unsigned long long)live0, (unsigned long long)nvn_vk_dbg_view_stat(0));
    }
    c201_set_flags(-1, -1);
    nvn_gpu_tex_release(t);
}
static void c201_commit_pass(const nvn_gpu_pass_desc *p) {
    static uint64_t sig = 2000;   // 同一会话里先后跑两档：围栏号只增不减
    const uint64_t signal = ++sig;
    nvn_gpu_err e;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_begin(c, p); nvn_gpu_pass_end(c);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, signal, on_done, NULL, &e); CHECK(OK(r), "C201 commit %llu：%d %s", (unsigned long long)signal, r.code, e.why);
    r = nvn_gpu_fence_wait(signal, 5000000000ull, &e); CHECK(OK(r), "C201 wait %llu：%d %s", (unsigned long long)signal, r.code, e.why);
}
static void c201_wait_fb(uint64_t want) {   // 视图销毁可能要等完成线程摘掉段里的引用，轮询到帧缓冲数到位（最多 2 秒）
    for (int i = 0; i < 2000 && nvn_vk_dbg_view_stat(2) != want; i++) nanosleep(&(struct timespec){0, 1000000}, NULL);
}
// selfcheck = 0：生产缺省档（核全表扫次数）；selfcheck = 1：自检档 nvk_fbdrop=2（核「视图销毁走快路之后全表扫不到残留」，不核扫描次数）。
#define C201_SCAN_CHECK(cond, ...) do { if (!selfcheck) CHECK(cond, __VA_ARGS__); } while (0)
static void c201_fb_scan(int selfcheck) {
    const int fl = selfcheck ? 2 : -2;
    const uint64_t resid0 = nvn_vk_dbg_view_stat(3);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex dep = make_tex(64, 64, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex idle = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view cv = make_view(out), dv = make_view(dep);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = cv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE; p.clear_depth = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    c201_set_flags(-1, fl);   // 生产缺省档（不自检）
    const uint64_t fb0 = nvn_vk_dbg_view_stat(2);
    c201_commit_pass(&p);
    CHECK(nvn_vk_dbg_view_stat(2) - fb0 == 1, "C201 开一个颜色 + 深度通道：缓存里帧缓冲期望 +1 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(2) - fb0));
    // ② 销毁从没当过附件的视图：缺省档不扫表；旧做法（nvk_fbdrop=0）要扫
    nvn_gpu_view iv = make_view(idle);
    uint64_t s0 = nvn_vk_dbg_view_stat(6);
    nvn_gpu_view_release(iv);
    C201_SCAN_CHECK(nvn_vk_dbg_view_stat(6) - s0 == 0, "C201 缺省档销毁从没当过附件的视图：全表扫次数期望 +0 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(6) - s0));
    CHECK(nvn_vk_dbg_view_stat(2) - fb0 == 1, "C201 销毁无关视图后：帧缓冲数期望仍 +1 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(2) - fb0));
    c201_set_flags(-1, 0);
    iv = make_view(idle);
    s0 = nvn_vk_dbg_view_stat(6);
    nvn_gpu_view_release(iv);
    CHECK(nvn_vk_dbg_view_stat(6) - s0 >= 1, "C201 旧做法（nvk_fbdrop=0）销毁从没当过附件的视图：全表扫次数期望 ≥ +1 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(6) - s0));
    c201_set_flags(-1, fl);
    // 销毁当过附件的视图（深度）：帧缓冲 -1，不扫表
    s0 = nvn_vk_dbg_view_stat(6);
    nvn_gpu_view_release(dv);
    c201_wait_fb(fb0);
    CHECK(nvn_vk_dbg_view_stat(2) == fb0, "C201 销毁深度视图：帧缓冲数期望回到 %llu → %llu", (unsigned long long)fb0, (unsigned long long)nvn_vk_dbg_view_stat(2));
    C201_SCAN_CHECK(nvn_vk_dbg_view_stat(6) - s0 == 0, "C201 销毁当过附件的视图：全表扫次数期望 +0 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(6) - s0));
    // 同一个颜色视图 + 新深度视图再开：新帧缓冲（槽可能复用、代数不同）；销毁颜色视图要把它拆掉，不能被颜色视图里残留的旧记录误导
    nvn_gpu_view dv2 = make_view(dep);
    p.depth.view = dv2;
    c201_commit_pass(&p);
    CHECK(nvn_vk_dbg_view_stat(2) - fb0 == 1, "C201 换新深度视图再开：帧缓冲期望 +1 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(2) - fb0));
    s0 = nvn_vk_dbg_view_stat(6);
    nvn_gpu_view_release(cv);
    c201_wait_fb(fb0);
    CHECK(nvn_vk_dbg_view_stat(2) == fb0, "C201 销毁颜色视图：新帧缓冲也要拆，帧缓冲数期望回到 %llu → %llu", (unsigned long long)fb0, (unsigned long long)nvn_vk_dbg_view_stat(2));
    C201_SCAN_CHECK(nvn_vk_dbg_view_stat(6) - s0 == 0, "C201 销毁颜色视图：全表扫次数期望 +0 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(6) - s0));
    // 同一个颜色视图用在 8 个不同的帧缓冲里（超过反查表容量 6，记录溢出）：销毁它要退回全表扫，8 个帧缓冲一个不剩
    nvn_gpu_view cv2 = make_view(out), dvs[8];
    p.color[0].view = cv2;
    for (int i = 0; i < 8; i++) { dvs[i] = make_view(dep); p.depth.view = dvs[i]; c201_commit_pass(&p); }
    CHECK(nvn_vk_dbg_view_stat(2) - fb0 == 8, "C201 同一个颜色视图配 8 个深度视图各开一次：帧缓冲期望 +8 → +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(2) - fb0));
    s0 = nvn_vk_dbg_view_stat(6);
    nvn_gpu_view_release(cv2);
    c201_wait_fb(fb0);
    CHECK(nvn_vk_dbg_view_stat(2) == fb0, "C201 销毁用在 8 个帧缓冲里的颜色视图（记录溢出）：帧缓冲数期望回到 %llu → %llu", (unsigned long long)fb0, (unsigned long long)nvn_vk_dbg_view_stat(2));
    CHECK(nvn_vk_dbg_view_stat(6) - s0 >= 1, "C201 记录溢出时销毁视图：期望退回全表扫（≥ +1）→ +%llu", (unsigned long long)(nvn_vk_dbg_view_stat(6) - s0));
    for (int i = 0; i < 8; i++) nvn_gpu_view_release(dvs[i]);
    nvn_gpu_view_release(dv2);
    nvn_gpu_tex_release(out); nvn_gpu_tex_release(dep); nvn_gpu_tex_release(idle);
    c201_set_flags(-1, -1);
    CHECK(nvn_vk_dbg_view_stat(3) == resid0, "C201 %s：视图销毁走快路之后全表扫到的残留期望 +0 → +%llu", selfcheck ? "自检档" : "缺省档", (unsigned long long)(nvn_vk_dbg_view_stat(3) - resid0));
    printf("C201 帧缓冲缓存（%s）：起点 %llu 个帧缓冲，用例结束时 %llu 个；全表扫累计 %llu 次，残留 %llu（各步期望见上，不符会有 FAIL 行）\n", selfcheck ? "自检档" : "缺省档",
           (unsigned long long)fb0, (unsigned long long)nvn_vk_dbg_view_stat(2), (unsigned long long)nvn_vk_dbg_view_stat(6), (unsigned long long)nvn_vk_dbg_view_stat(3));
}

static void c77_null_color_slot(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "out1_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 out1_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    pd.cfmt[0] = 0; pd.cfmt[1] = NVN_FORMAT_RGBA8; pd.chmask.mask = 0xF0;   // 槽 1 四个通道都写
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C77 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    if (pipe) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 2; p.color[1].view = ov; p.color[1].load = NVN_GPU_LOAD_CLEAR; p.color[1].store = NVN_GPU_STORE_STORE;
        p.color[1].clear.f[2] = 1; p.color[1].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_dyn dyn; full_dyn(&dyn); nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 119, on_done, NULL, &e);
        CHECK(OK(r), "C77 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(119, 5000000000ull, &e);
        CHECK(OK(r), "C77 wait：%d %s", r.code, e.why);
        expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, "C77 槽 1（空位在槽 0）整屏绿");
    }
    // C78
    nvn_gpu_shader vs2 = NULL, fs2 = NULL; void *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd2;
    fill_gpipe_desc(&pd2, vs2, fs2, 0);
    pd2.chmask.mask = 0;
    nvn_gpu_pipe pipe2 = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C78 pipe：%d %s", e.rc.code, e.why);
    if (pipe2) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_set_pipe(c, pipe2);
        nvn_gpu_dyn dyn; full_dyn(&dyn); nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 120, on_done, NULL, &e);
        CHECK(OK(r), "C78 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(120, 5000000000ull, &e);
        CHECK(OK(r), "C78 wait：%d %s", r.code, e.why);
        expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 0, 255, "C78 写掩码 0：仍是清屏的蓝");
    }
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2);
    free(vm); free(fm); free(vm2); free(fm2);
}

// C74：通道内按矩形清（契约 v4 nvn_gpu_clear_rect）。通道开头整屏清成黑 / 深度 1 / 模板 0，然后三次矩形清：
//   ① 颜色：x 8–31、y 16–47 清成红；② 只清模板：左半屏 x 0–31 写 1；③ 只清深度：右上 x 32–63、y 0–31 写 0。
//   再整屏画一笔蓝（z = 0.5）：深度 LESS、不写深度；模板 NOTEQUAL 1、操作全 KEEP。
// 期望（手算）：左半屏模板是 1，蓝画不上 → ① 的矩形是红，其余是黑；右上深度是 0，0.5 < 0 不成立 → 黑；右下深度 1、模板 0 → 蓝。
// ②③ 各只动一个面：② 若连深度一起清（清成 0）不影响左半结论，但 ③ 若连模板一起动、或 ② 没清到，右上 / 左半会出蓝。
static void c74_clear_rect(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.sfmt = NVN_FORMAT_DEPTH24_STENCIL8;
    pd.ds.dt = 1; pd.ds.dw = 0; pd.ds.df = 2;   // 深度 LESS，不写
    pd.ds.st = 1; pd.ds.sf_f = 6; pd.ds.sf_b = 6;   // 模板 NOTEQUAL
    pd.ds.fail_f = pd.ds.zfail_f = pd.ds.zpass_f = 1; pd.ds.fail_b = pd.ds.zfail_b = pd.ds.zpass_b = 1;   // KEEP
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C74 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = half_depth_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f; p.clear_stencil = 0; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff;
    cl.color = 0; cl.value.f[0] = 1; cl.value.f[3] = 1; cl.x = 8; cl.y = 16; cl.w = 24; cl.h = 32;
    nvn_gpu_clear_rect(c, &cl);
    memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.color = -1; cl.stencil = 1; cl.stencil_value = 1; cl.depth_value = 0.25f /* 不该被用到 */; cl.x = 0; cl.y = 0; cl.w = 32; cl.h = 64;
    nvn_gpu_clear_rect(c, &cl);
    memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.color = -1; cl.depth = 1; cl.depth_value = 0; cl.stencil_value = 1 /* 不该被用到 */; cl.x = 32; cl.y = 0; cl.w = 32; cl.h = 32;
    nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF; dyn.st_ref[0] = dyn.st_ref[1] = 1;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 115, on_done, NULL, &e);
    CHECK(OK(r), "C74 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(115, 5000000000ull, &e);
    CHECK(OK(r), "C74 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 8, 16, 24, 32, 255, 0, 0, "C74 颜色矩形（红）");
    expect_rect(buf_cpu(rb), 0, 0, 8, 64, 0, 0, 0, "C74 左半、矩形左侧（模板 1 挡住蓝：黑）");
    expect_rect(buf_cpu(rb), 8, 0, 24, 16, 0, 0, 0, "C74 左半、矩形上方（黑）");
    expect_rect(buf_cpu(rb), 8, 48, 24, 16, 0, 0, 0, "C74 左半、矩形下方（黑）");
    expect_rect(buf_cpu(rb), 32, 0, 32, 32, 0, 0, 0, "C74 右上（深度清成 0，LESS 不过：黑）");
    expect_rect(buf_cpu(rb), 32, 32, 32, 32, 0, 0, 255, "C74 右下（深度 1、模板 0：蓝）");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv); nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C79：带写掩码的清除（契约 v5）。通道开头颜色清成 (0.25, 0.5, 0.75, 1) = 64,128,191,255，模板清成 0x0F。
//   ① 颜色：左半屏只写绿通道，值 (1,1,1,0) → 左半 64,255,191,255（红蓝 alpha 不动）；
//   ② 模板：上半屏只写 0x30 两位，值 0xF0 → 上半模板 = 0x0F 保留位 | 0x30 = 0x3F，下半仍是 0x0F；
//   再整屏画一笔蓝，模板 EQUAL 0x3F、操作全 KEEP → 只有上半画得上。
// 期望（手算）：上半蓝；左下 64,255,191；右下 64,128,191。
//   掩码没生效的反例：① 四通道全写则左下是 255,255,255,0；② 八位全写则上半模板是 0xF0，蓝一笔都画不上。
static void c79_masked_clear(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.sfmt = NVN_FORMAT_DEPTH24_STENCIL8;
    pd.ds.st = 1; pd.ds.sf_f = 3; pd.ds.sf_b = 3;   // 模板 EQUAL
    pd.ds.fail_f = pd.ds.zfail_f = pd.ds.zpass_f = 1; pd.ds.fail_b = pd.ds.zfail_b = pd.ds.zpass_b = 1;   // KEEP
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C79 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 0.25f; p.color[0].clear.f[1] = 0.5f; p.color[0].clear.f[2] = 0.75f; p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f; p.clear_stencil = 0x0F; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl);
    cl.color = 0; cl.color_mask = 0x2; cl.stencil_mask = 0xff; cl.value.f[0] = 1; cl.value.f[1] = 1; cl.value.f[2] = 1; cl.value.f[3] = 0; cl.x = 0; cl.y = 0; cl.w = 32; cl.h = 64;
    nvn_gpu_clear_rect(c, &cl);
    memset(&cl, 0, sizeof cl);
    cl.color = -1; cl.color_mask = 0xf; cl.stencil = 1; cl.stencil_mask = 0x30; cl.stencil_value = 0xF0; cl.x = 0; cl.y = 0; cl.w = 64; cl.h = 32;
    nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF; dyn.st_ref[0] = dyn.st_ref[1] = 0x3F;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 121, on_done, NULL, &e);
    CHECK(OK(r), "C79 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(121, 5000000000ull, &e);
    CHECK(OK(r), "C79 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 0, 0, 255, "C79 上半（模板 0x3F：蓝画上）");
    expect_rect(buf_cpu(rb), 0, 32, 32, 32, 64, 255, 191, "C79 左下（只写了绿通道）");
    expect_rect(buf_cpu(rb), 32, 32, 32, 32, 64, 128, 191, "C79 右下（没动）");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv); nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C80：正面绕序（契约 v6）。管线描述里的 poly.front 是 y 朝上的规范化设备坐标里的绕序；整屏三角形 (-1,-1) (3,-1) (-1,3) 在那个坐标里是逆时针。
//   剔除背面时：正面 = 逆时针的管线画得上，正面 = 顺时针的画不上——窗口原点在左下（视口不翻）和在左上（视口翻）两种情况都一样。
// 期望（手算）：每种原点下，先用「正面逆时针」画绿 → 整屏绿；再用「正面顺时针」画蓝 → 被剔掉，仍然整屏绿。
static void c80_front_face(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL, vs2 = NULL, fs2 = NULL; void *vm = NULL, *fm = NULL, *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    for (int ll = 1; ll >= 0; ll--) {
        nvn_gpu_pipe_desc pd, pd2; nvn_gpu_err e; nvn_gpu_pipe pipe = NULL, pipe2 = NULL;
        fill_gpipe_desc(&pd, vs, fs, 0); pd.poly.cull = 2; pd.poly.front = 1; pd.origin_ll = (uint8_t)ll;      // 剔背面，正面逆时针
        fill_gpipe_desc(&pd2, vs2, fs2, 0); pd2.poly.cull = 2; pd2.poly.front = 0; pd2.origin_ll = (uint8_t)ll;  // 剔背面，正面顺时针
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C80 pipe：%d %s", e.rc.code, e.why);
        CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C80 pipe2：%d %s", e.rc.code, e.why);
        if (!pipe || !pipe2) break;
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.origin_ll = (uint8_t)ll;
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
        nvn_gpu_set_pipe(c, pipe2); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
        uint64_t sig = 122 + (uint64_t)(1 - ll);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
        CHECK(OK(r), "C80 commit（原点左下 = %d）：%d %s", ll, r.code, e.why);
        r = nvn_gpu_fence_wait(sig, 5000000000ull, &e);
        CHECK(OK(r), "C80 wait：%d %s", r.code, e.why);
        expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, ll ? "C80 原点左下：逆时针画上、顺时针被剔（整屏绿）" : "C80 原点左上：逆时针画上、顺时针被剔（整屏绿）");
    }
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2);
    free(vm); free(fm); free(vm2); free(fm2);
}

// C81：整数格式的贴图绑进采样槽。游戏的采样器是线性过滤也照绑（整数格式不能线性过滤：后端给它换最近点的那一个）。
// 期望（手算）：2x2 的 R32UI 贴图存四个位模式（含浮点里是非数、次正规数的那两种），画到 2x2 的 R32UI 目标，逐像素读回来与存进去的位相同；校验层 0 条。
static void c81_int_sampled(const char *dir) {
    static const uint32_t bits[4] = {0x12345678u, 0xFFFFFFFFu, 0x00000123u, 0x7FC00001u};
    nvn_gpu_layout_ent ent; memset(&ent, 0, sizeof ent);
    ent.set = 0; ent.cls = NVN_GPU_RES_TEX_SAMP; ent.access = NVN_GPU_ACC_R; ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = &ent; fl.nent = 1;
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "tex_u.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 tex_u"); return; }
    nvn_gpu_tex src = make_tex(2, 2, NVN_FORMAT_R32UI, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex out = make_tex(2, 2, NVN_FORMAT_R32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view sv = make_view(src), ov = make_view(out);
    nvn_gpu_buf rb = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_err e; samp_par sp; memset(&sp, 0, sizeof sp);
    sp.magf = NVN_MAG_FILTER_LINEAR; sp.minf = NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR; sp.lod_max = 1000.0f;
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C81 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_pipe_desc pd; nvn_gpu_pipe pipe = NULL;
    fill_gpipe_desc(&pd, vs, fs, 0); pd.cfmt[0] = NVN_FORMAT_R32UI;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C81 pipe：%d %s", e.rc.code, e.why);
    if (pipe && samp) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_tex_upload(c, src, 0, 0, NULL, bits, 8, 16);
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.w = 2; p.h = 2; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.vp[0][2] = 2; dyn.vp[0][3] = 2; dyn.sc[2] = 2; dyn.sc[3] = 2;
        nvn_gpu_binding b; memset(&b, 0, sizeof b); b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = sv; b.u.ts.samp = samp;
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1); nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 8, 16);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 124, on_done, NULL, &e);
        CHECK(OK(r), "C81 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(124, 5000000000ull, &e);
        CHECK(OK(r), "C81 wait：%d %s", r.code, e.why);
        uint32_t got[4]; memcpy(got, buf_cpu(rb), 16);
        CHECK(!memcmp(got, bits, 16), "C81 整数贴图读回的位：%#x %#x %#x %#x（期望 %#x %#x %#x %#x）", got[0], got[1], got[2], got[3], bits[0], bits[1], bits[2], bits[3]);
    }
    if (samp) nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov); nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C82：遮挡查询。一段声明 4 个槽：槽 0 整屏三角形（64x64 目标）、槽 1 同一个三角形裁剪到 16x16、槽 2 开了不画、槽 3 不用。
// 期望（手算）：完成回调里各槽样本数 = 4096、256、0、0。
static uint64_t g_c82_occ[4]; static uint32_t g_c82_n;
static void c82_done(void *user, const nvn_gpu_done *d) { (void)user; g_c82_n = d->nocclusion; for (uint32_t i = 0; i < 4 && i < d->nocclusion && d->occlusion; i++) g_c82_occ[i] = d->occlusion[i]; on_done(NULL, d); }
static void c82_occlusion(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_pipe_desc pd; nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    fill_gpipe_desc(&pd, vs, fs, 0);
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C82 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_cmd c = NULL; nvn_gpu_cmd_desc cd = {4, "C82"};
    CHECK(OK(nvn_gpu_cmd_begin(&cd, &c, &e)), "C82 cmd_begin：%d %s", e.rc.code, e.why);
    if (pipe && c) {
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1; p.occlusion = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_dyn dyn; full_dyn(&dyn);
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_occlusion(c, 0); nvn_gpu_draw(c, &da);
        dyn.sc[0] = 8; dyn.sc[1] = 8; dyn.sc[2] = 16; dyn.sc[3] = 16; nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_occlusion(c, 1); nvn_gpu_draw(c, &da);
        nvn_gpu_occlusion(c, 2);
        nvn_gpu_occlusion(c, -1); nvn_gpu_draw(c, &da);   // 停了以后画的不计
        nvn_gpu_pass_end(c);
        g_c82_n = 0; memset(g_c82_occ, 0xff, sizeof g_c82_occ);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 125, c82_done, NULL, &e);
        CHECK(OK(r), "C82 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(125, 5000000000ull, &e);
        CHECK(OK(r), "C82 wait：%d %s", r.code, e.why);
        CHECK(g_c82_n == 4 && g_c82_occ[0] == 4096 && g_c82_occ[1] == 256 && g_c82_occ[2] == 0 && g_c82_occ[3] == 0, "C82 各槽样本数：%u 槽 %llu %llu %llu %llu（期望 4 槽 4096 256 0 0）", g_c82_n,
              (unsigned long long)g_c82_occ[0], (unsigned long long)g_c82_occ[1], (unsigned long long)g_c82_occ[2], (unsigned long long)g_c82_occ[3]);
    }
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out); nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C90：无附件渲染通道（NVN 不挂任何目标照样绘制：片元只写存储 / 只为遮挡计数；真机一款大作每帧约 4 笔）。
// 通道 0 个颜色槽、无深度，帧缓冲 32x24；管线不挂任何颜色格式；整屏三角形开遮挡计数。
// 期望（手算）：后端**不**对核心声明 NVN_GPU_CAP_NO_ATTACH_PASS（#697 撤回，核心照旧跳过这些笔）；直接开零附件通道仍可用：槽 0 样本数 = 32 × 24 = 768。
static uint64_t g_c90_occ; static uint32_t g_c90_n;
static void c90_done(void *user, const nvn_gpu_done *d) { (void)user; g_c90_n = d->nocclusion; if (d->nocclusion && d->occlusion) g_c90_occ = d->occlusion[0]; on_done(NULL, d); }
static void c90_no_attachment(const char *dir) {
    nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
    CHECK(!(caps.caps & NVN_GPU_CAP_NO_ATTACH_PASS), "C90 后端不声明无附件通道（#697 撤回）");
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C90 读不到 solid_green"); return; }
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_pipe_desc pd; nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    fill_gpipe_desc(&pd, vs, fs, 0); pd.cfmt[0] = 0; pd.chmask.mask = 0;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C90 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_cmd c = NULL; nvn_gpu_cmd_desc cd = {1, "C90"};
    CHECK(OK(nvn_gpu_cmd_begin(&cd, &c, &e)), "C90 cmd_begin：%d %s", e.rc.code, e.why);
    if (pipe && c) {
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 0; p.w = 32; p.h = 24; p.layers = 1; p.occlusion = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.vp[0][2] = 32; dyn.vp[0][3] = 24; dyn.sc[2] = 32; dyn.sc[3] = 24;
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_occlusion(c, 0); nvn_gpu_draw(c, &da); nvn_gpu_occlusion(c, -1);
        nvn_gpu_pass_end(c);
        g_c90_n = 0; g_c90_occ = ~0ull;
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 200, c90_done, NULL, &e);
        CHECK(OK(r), "C90 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(200, 5000000000ull, &e);
        CHECK(OK(r), "C90 wait：%d %s", r.code, e.why);
        CHECK(g_c90_n == 1 && g_c90_occ == 768, "C90 无附件通道样本数：%u 槽 %llu（期望 1 槽 768）", g_c90_n, (unsigned long long)g_c90_occ);
    }
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C83：带缩放 / 换格式的贴图拷贝。
// 期望（手算）：① 8x8 的 RGBA8 左半红右半蓝，最近点缩到 4x4 → 左两列红、右两列蓝；
//   ② 2x2 的 R11G11B10F 全是 (0.5, 0.25, 1.0)（位模式 R 0x380、G 0x340、B 0x1E0），线性拷到 2x2 的 RGBA8 → (127 或 128, 64 ± 1, 255, 255)。
static void c83_blit(void) {
    nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
    CHECK(caps.caps & NVN_GPU_CAP_BLIT_SCALED, "C83 后端声明原生缩放拷贝");
    nvn_gpu_tex a = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED), b = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex f = make_tex(2, 2, NVN_FORMAT_R11G11B10F, NVN_GPU_TEX_SAMPLED), g2 = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view av = make_view(a), bv = make_view(b), fv = make_view(f), gv = make_view(g2);
    nvn_gpu_buf rb = make_buf(4 * 4 * 4 + 2 * 2 * 4, NVN_GPU_MEM_SHARED, NULL);
    uint8_t px[8 * 8 * 4]; for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t *p = px + (y * 8 + x) * 4; p[0] = x < 4 ? 255 : 0; p[1] = 0; p[2] = x < 4 ? 0 : 255; p[3] = 255; }
    uint32_t fw[4]; for (int i = 0; i < 4; i++) fw[i] = 0x380u | 0x340u << 11 | 0x1E0u << 22;
    nvn_gpu_err e; nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, a, 0, 0, NULL, px, 32, sizeof px);
    nvn_gpu_tex_upload(c, f, 0, 0, NULL, fw, 8, 16);
    NVNcopyRegion r8 = {0, 0, 0, 8, 8, 1}, r4 = {0, 0, 0, 4, 4, 1}, r2 = {0, 0, 0, 2, 2, 1};
    nvn_gpu_blit(c, av, &r8, bv, &r4, 0);
    nvn_gpu_blit(c, fv, &r2, gv, &r2, 1);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_copy_tex_buf(c, g2, 0, 0, NULL, rb, 64, 8, 16);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 126, on_done, NULL, &e);
    CHECK(OK(r), "C83 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(126, 5000000000ull, &e);
    CHECK(OK(r), "C83 wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb); int bad = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = o + (y * 4 + x) * 4; if (p[0] != (x < 2 ? 255 : 0) || p[1] != 0 || p[2] != (x < 2 ? 0 : 255) || p[3] != 255) bad++; }
    CHECK(!bad, "C83 缩到 4x4：%d 个像素不符（期望左两列红、右两列蓝）", bad);
    bad = 0; for (int i = 0; i < 4; i++) { const uint8_t *p = o + 64 + i * 4; if (p[0] < 127 || p[0] > 128 || p[1] < 63 || p[1] > 65 || p[2] != 255 || p[3] != 255) bad++; }
    CHECK(!bad, "C83 R11G11B10F → RGBA8：%d 个像素不符，首像素 %u %u %u %u（期望 127–128, 63–65, 255, 255）", bad, o[64], o[65], o[66], o[67]);
    nvn_gpu_view_release(av); nvn_gpu_view_release(bv); nvn_gpu_view_release(fv); nvn_gpu_view_release(gv);
    nvn_gpu_tex_release(a); nvn_gpu_tex_release(b); nvn_gpu_tex_release(f); nvn_gpu_tex_release(g2); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C84：带校验层在手机上报过的几类用法各走一遍（整套用例在校验层下跑，报一条就是红），内容按手算的期望读回。
//   ① 两级贴图的默认视图（取全部级）当颜色附件：按首级首层挂。两级先传成红，通道清成绿 → 第 0 级全绿、第 1 级还是红。
//   ② 深度模板贴图的默认视图当深度模板附件（采样形态只取深度面，附件形态两个面都在）。
//   ③ 暂存偏移是 4 不是 8 的时候上传 BC1（一块 8 字节）：偏移要对齐到块。
//   ④ D16 → 缓冲 → R16 逐字节拷（核心的影子就是这么拷的）：0x8000 原样落到 R16。
static void c84_validation_classes(void) {
    nvn_gpu_err e; nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = 8; td.d.height = 8; td.d.depth = 1; td.d.levels = 2; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex m = NULL; nvn_gpu_rc r = nvn_gpu_tex_create(&td, &m, &e);
    CHECK(OK(r), "C84 两级贴图：%d %s", r.code, e.why);
    nvn_gpu_view mv = make_view(m);
    int has_ds = (nvn_gpu_format_caps(NVN_FORMAT_DEPTH24_STENCIL8) & NVN_GPU_FMT_DEPTH) != 0, has_bc = (nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE) != 0;
    nvn_gpu_tex ds = has_ds ? make_tex(8, 8, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED) : NULL; nvn_gpu_view dv = ds ? make_view(ds) : NULL;
    nvn_gpu_tex one = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED), bc = has_bc ? make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED) : NULL;
    nvn_gpu_tex d16 = make_tex(4, 4, NVN_FORMAT_DEPTH16, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED), r16 = make_tex(4, 4, NVN_FORMAT_R16, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(256 + 64 + 32, NVN_GPU_MEM_SHARED, NULL);
    uint8_t red[8 * 8 * 4]; for (int i = 0; i < 64; i++) { red[i * 4] = 255; red[i * 4 + 1] = 0; red[i * 4 + 2] = 0; red[i * 4 + 3] = 255; }
    uint8_t px1[4] = {1, 2, 3, 4}, blk[8] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0}; uint16_t half[16]; for (int i = 0; i < 16; i++) half[i] = 0x8000;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, one, 0, 0, NULL, px1, 4, 4);                       // 暂存用掉 4 字节
    if (bc) nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blk, 8, 8);               // ③
    nvn_gpu_tex_upload(c, m, 0, 0, NULL, red, 32, 256);
    nvn_gpu_tex_upload(c, m, 1, 0, NULL, red, 16, 64);
    nvn_gpu_tex_upload(c, d16, 0, 0, NULL, half, 8, 32);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = mv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[1] = 1; p.color[0].clear.f[3] = 1;
    if (dv) { p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE; p.stencil = p.depth; p.clear_depth = 0.5f; }
    p.w = 8; p.h = 8; p.layers = 1;
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);                          // ① ②
    nvn_gpu_slice sl; r = nvn_gpu_arena_alloc(c, 32, 16, &sl, &e);
    CHECK(OK(r), "C84 暂存：%d %s", r.code, e.why);
    NVNcopyRegion r4 = {0, 0, 0, 4, 4, 1};
    nvn_gpu_copy_tex_buf(c, d16, 0, 0, &r4, sl.buf, sl.off, 8, 32);          // ④
    nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, 8, 32, r16, 0, 0, &r4);
    nvn_gpu_copy_tex_buf(c, m, 0, 0, NULL, rb, 0, 32, 256);
    nvn_gpu_copy_tex_buf(c, m, 1, 0, NULL, rb, 256, 16, 64);
    nvn_gpu_copy_tex_buf(c, r16, 0, 0, NULL, rb, 320, 8, 32);
    r = nvn_gpu_cmd_commit(c, 127, on_done, NULL, &e);
    CHECK(OK(r), "C84 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(127, 5000000000ull, &e);
    CHECK(OK(r), "C84 wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb); int bad = 0;
    for (int i = 0; i < 64; i++) if (o[i * 4] != 0 || o[i * 4 + 1] != 255 || o[i * 4 + 2] != 0 || o[i * 4 + 3] != 255) bad++;
    CHECK(!bad, "C84 ① 第 0 级：%d 个像素不是绿（首像素 %u %u %u %u）", bad, o[0], o[1], o[2], o[3]);
    bad = 0; for (int i = 0; i < 16; i++) if (o[256 + i * 4] != 255 || o[256 + i * 4 + 1] != 0) bad++;
    CHECK(!bad, "C84 ① 第 1 级：%d 个像素不是红（通道只该写首级）", bad);
    bad = 0; for (int i = 0; i < 16; i++) { uint16_t v; memcpy(&v, o + 320 + i * 2, 2); if (v != 0x8000) bad++; }
    CHECK(!bad, "C84 ④ D16 → R16：%d 个像素不是 0x8000", bad);
    nvn_gpu_view_release(mv); if (dv) nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(m); if (ds) nvn_gpu_tex_release(ds); nvn_gpu_tex_release(one); if (bc) nvn_gpu_tex_release(bc); nvn_gpu_tex_release(d16); nvn_gpu_tex_release(r16); nvn_gpu_buf_release(rb, NULL, NULL);
}

// #690：原生 BC1 sRGB 的整级跨 mip 图像拷贝，按输入原始块字节核对结果。
static void c690_native_bc_image_copy(uint32_t fmt, uint32_t block_bytes, uint64_t signal) {
    if ((g_dev.bc_decode_mask & (1u << (fmt - 0x41))) ||
        !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_SAMPLE)) {
        printf("C690：格式 %#x 非原生存储，跳过原块图像拷贝\n", fmt);
        return;
    }
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = (int)fmt;
    td.d.width = 64; td.d.height = 64; td.d.depth = 1; td.d.levels = 7;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex src = NULL, dst = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &src, &e);
    CHECK(OK(r), "C690 源图创建：%d %s", r.code, e.why);
    td.d.width = 1024; td.d.height = 1024; td.d.levels = 11;
    r = nvn_gpu_tex_create(&td, &dst, &e);
    CHECK(OK(r), "C690 目标图创建：%d %s", r.code, e.why);
    uint32_t bytes = 256 * block_bytes, row = 16 * block_bytes;
    nvn_gpu_buf rb = make_buf(bytes, NVN_GPU_MEM_SHARED, NULL);
    if (src && dst && rb) {
        uint8_t blocks[4096];
        for (unsigned i = 0; i < bytes; i++) blocks[i] = (uint8_t)(i * 73u + i / block_bytes + 19u);
        NVNcopyRegion region = {0, 0, 0, 64, 64, 1};
        nvn_gpu_cmd c = begin_cmd();
        if (c) {
            nvn_gpu_tex_upload(c, src, 0, 0, &region, blocks, row, bytes);
            nvn_gpu_copy_tex(c, src, 0, 0, &region, dst, 4, 0, &region);
            nvn_gpu_copy_tex_buf(c, dst, 4, 0, &region, rb, 0, row, bytes);
            r = nvn_gpu_cmd_commit(c, signal, on_done, NULL, &e);
            CHECK(OK(r), "C690 原生 BC 格式 %#x 跨 mip 图像拷贝应成功：%d %s", fmt, r.code, e.why);
            if (OK(r)) {
                r = nvn_gpu_fence_wait(signal, 5000000000ull, &e);
                CHECK(OK(r), "C690 等待：%d %s", r.code, e.why);
                if (OK(r)) {
                    int same = !memcmp(buf_cpu(rb), blocks, bytes);
                    CHECK(same, "C690 格式 %#x 的 %u 字节原始 BC 块应逐字节相同", fmt, bytes);
                    if (same) printf("C690 跨 mip 原块一致：格式 %#x，%u 字节\n", fmt, bytes);
                }
            }
        }
    }
    if (src) nvn_gpu_tex_release(src);
    if (dst) nvn_gpu_tex_release(dst);
    if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
}

// 不满块的区域同时落在两张图的右下边缘，合法且只应替换最后一个块。
static void c690_edge_copy(void) {
    const uint32_t fmt = NVN_FORMAT_RGB_DXT1_SRGB;
    if ((g_dev.bc_decode_mask & (1u << (fmt - 0x41))) || !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_SAMPLE)) return;
    nvn_gpu_tex src = make_tex(6, 6, fmt, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex dst = make_tex(6, 6, fmt, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_buf rb = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    uint8_t a[32], b[32], want[32];
    for (unsigned i = 0; i < 32; i++) { a[i] = (uint8_t)(i + 1); b[i] = (uint8_t)(i + 101); }
    memcpy(want, b, 32); memcpy(want + 24, a + 24, 8);
    if (src && dst && rb) {
        NVNcopyRegion edge = {4, 4, 0, 2, 2, 1};
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e = {0};
        if (c) {
            nvn_gpu_tex_upload(c, src, 0, 0, NULL, a, 16, 32);
            nvn_gpu_tex_upload(c, dst, 0, 0, NULL, b, 16, 32);
            nvn_gpu_copy_tex(c, src, 0, 0, &edge, dst, 0, 0, &edge);
            nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, rb, 0, 16, 32);
            nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
            CHECK(OK(r), "C690 不满块的双图边缘应允许：%d %s", r.code, e.why);
            if (OK(r)) {
                r = nvn_gpu_fence_wait(3, 5000000000ull, &e);
                CHECK(OK(r), "C690 边缘等待：%d %s", r.code, e.why);
                if (OK(r)) {
                    int same = !memcmp(buf_cpu(rb), want, 32);
                    CHECK(same, "C690 边缘仅替换最后 8 字节块");
                    if (same) printf("C690 双图边缘：仅替换末块，32 字节一致\n");
                }
            }
        }
    }
    if (src) nvn_gpu_tex_release(src);
    if (dst) nvn_gpu_tex_release(dst);
    if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
}

// 每个非法命令都使时间线进入失败终态，调用方为每例另开会话。
static void c690_bad_region(int k) {
    const uint32_t fmt = NVN_FORMAT_RGB_DXT1_SRGB;
    if ((g_dev.bc_decode_mask & (1u << (fmt - 0x41))) || !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_SAMPLE)) return;
    uint32_t sw = k == 6 ? 6 : 8, dw = k == 7 ? 6 : 8;
    nvn_gpu_tex src = make_tex(sw, 8, fmt, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex dst = make_tex(dw, 8, fmt, NVN_GPU_TEX_SAMPLED);
    if (src && dst) {
        NVNcopyRegion sr = {0, 0, 0, 4, 4, 1}, dr = sr;
        switch (k) {
        case 0: sr.xoffset = 1; break;
        case 1: sr.yoffset = 1; break;
        case 2: dr.xoffset = 1; break;
        case 3: dr.yoffset = 1; break;
        case 4: sr.width = dr.width = 2; break;
        case 5: sr.height = dr.height = 2; break;
        case 6: case 7: sr.width = dr.width = 6; break; // 只到其中一张图的边缘
        }
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e = {0};
        if (c) {
            nvn_gpu_copy_tex(c, src, 0, 0, &sr, dst, 0, 0, &dr);
            nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
            CHECK(r.code == NVN_GPU_E_ARG, "C690 非法块区域 %d 应 E_ARG，实为 %d", k, r.code);
            if (r.code == NVN_GPU_E_ARG) printf("C690 非法块区域 %d：E_ARG\n", k);
        }
    }
    if (src) nvn_gpu_tex_release(src);
    if (dst) nvn_gpu_tex_release(dst);
}

// C84b（#709）：块压缩贴图与非压缩贴图之间经缓冲逐字节拷（核心的「一块当一个像素」影子就是这么拷的）。
//   BC1 4x4（一块 8 字节）传进去 → 区域按像素 4x4 拷到缓冲（一行块 8 字节）→ 拷进 1x1 的 RG32UI（一个像素 8 字节）→ 读回。
//   期望（手算）：读回的 8 个字节就是传进去的那一块。不能直接采样 BC 的设备上建不了这张贴图，跳过。
static void c84b_bc_block_copy(void) {
    if ((g_dev.bc_decode_mask & (1u << (NVN_FORMAT_RGBA_DXT1 - 0x41))) || !(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE)) { fprintf(stderr, "C84b：BC1 非原生压缩存储，跳过原块回读\n"); return; }
    nvn_gpu_tex bc = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED), px = make_tex(1, 1, NVN_FORMAT_RG32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    const uint8_t blk[8] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    nvn_gpu_err e; nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blk, 8, 8);
    nvn_gpu_slice sl; nvn_gpu_rc r = nvn_gpu_arena_alloc(c, 8, 16, &sl, &e);
    CHECK(OK(r), "C84b 暂存：%d %s", r.code, e.why);
    NVNcopyRegion rbc = {0, 0, 0, 4, 4, 1}, rpx = {0, 0, 0, 1, 1, 1};
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, &rbc, sl.buf, sl.off, 8, 8);
    nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, 8, 8, px, 0, 0, &rpx);
    nvn_gpu_copy_tex_buf(c, px, 0, 0, NULL, rb, 0, 8, 8);
    r = nvn_gpu_cmd_commit(c, 128, on_done, NULL, &e);
    CHECK(OK(r), "C84b commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(128, 5000000000ull, &e);
    CHECK(OK(r), "C84b wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb);
    CHECK(!memcmp(o, blk, 8), "C84b BC1 → 缓冲 → RG32UI：读回 %02x%02x%02x%02x%02x%02x%02x%02x，期望 123456789abcdef0", o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(px); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C85：两个实际使用的组；三笔分别留下红/红/蓝，分配增量 2/0/1。
static void c85_descriptor_reuse(const char *dir) {
    char path[1024]; size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", dir); void *vm = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri.frag.spv", dir); uint32_t *fm = read_file(path, &fsz);
    CHECK(vm && fm, "C85 shader"); if (!vm || !fm) { free(vm); free(fm); return; }
    // 原创仓内夹具的 sampler binding=1：只把 DescriptorSet decoration 改到组1。
    uint32_t sampler_id = 0; int changed = 0;
    for (size_t i = 5; i < fsz / 4; i += fm[i] >> 16) {
        if (!(fm[i] >> 16)) break;
        if ((fm[i] & 0xffff) == 71 && (fm[i] >> 16) == 4 && fm[i+2] == 33 && fm[i+3] == 1) sampler_id = fm[i+1];
    }
    for (size_t i = 5; i < fsz / 4; i += fm[i] >> 16) {
        if (!(fm[i] >> 16)) break;
        if ((fm[i] & 0xffff) == 71 && (fm[i] >> 16) == 4 && fm[i+1] == sampler_id && fm[i+2] == 34) { fm[i+3] = 1; changed++; }
    }
    CHECK(sampler_id && changed == 1, "C85 sampler装饰唯一");
    nvn_gpu_layout_ent ents[2] = {0};
    ents[0].set = 0; ents[0].binding = 0; ents[0].cls = NVN_GPU_RES_TEX;
    ents[1].set = 1; ents[1].binding = 1; ents[1].cls = NVN_GPU_RES_SAMP;
    for (int i=0;i<2;i++) { ents[i].count=1; ents[i].access=NVN_GPU_ACC_R; ents[i].stages=1u<<NVN_GPU_SH_FS; }
    nvn_gpu_layout fl = {0}, vl = {0}; fl.ent=ents; fl.nent=2;
    nvn_gpu_shader vs=NULL, fs=NULL, plain=NULL; nvn_gpu_err e;
    nvn_gpu_shader_src ss = {0}; ss.stage=NVN_GPU_SH_VS; ss.kind=NVN_GPU_BLOB_SPIRV; ss.entry="main"; ss.blob=vm; ss.bytes=vsz; ss.layout=&vl;
    CHECK(OK(nvn_gpu_shader_load(&ss,&vs,&e)), "C85 VS %s", e.why);
    ss.stage=NVN_GPU_SH_FS; ss.blob=fm; ss.bytes=fsz; ss.layout=&fl;
    CHECK(OK(nvn_gpu_shader_load(&ss,&fs,&e)), "C85 FS %s", e.why);
    size_t psz=0; snprintf(path,sizeof path,"%s/solid_green.frag.spv",dir); void *pm=read_file(path,&psz);
    CHECK(pm != NULL,"C85 no-set shader");
    ss.blob=pm; ss.bytes=psz; ss.layout=&vl;
    CHECK(OK(nvn_gpu_shader_load(&ss,&plain,&e)),"C85 plain FS %s",e.why); free(pm);
    nvn_gpu_pipe_desc pd={0}; pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll=(uint8_t)!g_tri_origin_ul; pd.color.logic_op=3;
    pd.st[NVN_GPU_SH_VS].sh=vs; pd.st[NVN_GPU_SH_FS].sh=fs; pd.cfmt[0]=NVN_FORMAT_RGBA8; pd.samples=1; pd.prim=4; pd.chmask.mask=0xf;
    pd.attr[0].fmt=pd.attr[1].fmt=NVN_FORMAT_RG32F; pd.attr[1].offset=8; pd.stream[0].stride=16;
    nvn_gpu_pipe pipe=NULL, pipe2=NULL, no_sets=NULL; CHECK(OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e)),"C85 pipe %s",e.why);
    pd.prim=5; CHECK(OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe2,&e)),"C85 pipe2 %s",e.why);
    pd.st[NVN_GPU_SH_FS].sh=plain;
    CHECK(OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&no_sets,&e)),"C85 no-set pipe %s",e.why);
    const float verts[3][4]={{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb=make_buf(sizeof verts,NVN_GPU_MEM_SHARED,verts), rb=make_buf(12*4*4,NVN_GPU_MEM_SHARED,NULL);
    nvn_gpu_tex red=make_tex(1,1,NVN_FORMAT_RGBA8,NVN_GPU_TEX_SAMPLED), blue=make_tex(1,1,NVN_FORMAT_RGBA8,NVN_GPU_TEX_SAMPLED), out=make_tex(12,4,NVN_FORMAT_RGBA8,NVN_GPU_TEX_RENDER);
    nvn_gpu_view rv=make_view(red), bv=make_view(blue), ov=make_view(out);
    samp_par sp={0}; nvn_gpu_samp samp=NULL; CHECK(OK(nvn_gpu_samp_create(&sp,&samp,&e)),"C85 sampler %s",e.why);
    nvn_gpu_binding binds[2]={0}; binds[0].cls=NVN_GPU_RES_TEX; binds[0].u.view=rv; binds[1].ent=1; binds[1].cls=NVN_GPU_RES_SAMP; binds[1].u.samp=samp;
    for (int seg=0;seg<2;seg++) {
        nvn_gpu_cmd c=begin_cmd(); uint8_t r[4]={255,0,0,255}, b[4]={0,0,255,255};
        if (!seg) { nvn_gpu_tex_upload(c,red,0,0,NULL,r,4,4); nvn_gpu_tex_upload(c,blue,0,0,NULL,b,4,4); }
        nvn_gpu_pass_desc pass={0}; pass.ncolor=1; pass.w=12; pass.h=4; pass.layers=1; pass.color[0].view=ov; pass.color[0].load=NVN_GPU_LOAD_CLEAR; pass.color[0].store=NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(c,&pass); nvn_gpu_set_pipe(c,pipe); nvn_gpu_set_vbuf(c,0,vb,0);
        nvn_gpu_dyn dyn={0}; dyn.origin_ll=pd.origin_ll; dyn.nvp=1; dyn.vp[0][2]=12; dyn.vp[0][3]=4; dyn.vp[0][5]=1; dyn.has_sc=1; dyn.sc[2]=4; dyn.sc[3]=4;
        nvn_gpu_draw_args da={0}; da.prim=4; da.count=3; da.instances=1;
        for (int draw=0;draw<3;draw++) {
            dyn.sc[0]=draw*4; nvn_gpu_set_dyn(c,&dyn); binds[0].u.view=draw==2?bv:rv;
            // 无效union字节不同不应影响同一种绑定身份。
            binds[0]._rsv=(uint8_t)draw;
            nvn_gpu_bind(c,NVN_GPU_SH_FS,binds,2); unsigned before=desc_demand(); nvn_gpu_draw(c,&da);
            CHECK(desc_demand()-before==(draw==0?2u:draw==1?0u:1u),"C85 段%d笔%d分配%u",seg,draw,desc_demand()-before);
        }
        // 内部清除后所有组失效；重新发同绑定也必须重建两组。
        nvn_gpu_clear cl={0}; cl.color=0; cl.color_mask=0xf; cl.stencil_mask=0xff; cl.x=8; cl.w=4; cl.h=4; cl.value.f[1]=1; cl.value.f[3]=1;
        nvn_gpu_clear_rect(c,&cl); nvn_gpu_set_pipe(c,pipe); nvn_gpu_set_dyn(c,&dyn); nvn_gpu_set_vbuf(c,0,vb,0); nvn_gpu_bind(c,NVN_GPU_SH_FS,binds,2);
        unsigned before=desc_demand(); nvn_gpu_draw(c,&da); CHECK(g_memo_on || desc_demand()-before==2,"C85 clear后重建两组");
        nvn_gpu_set_pipe(c,pipe2); nvn_gpu_bind(c,NVN_GPU_SH_FS,binds,2); da.prim=5; before=desc_demand(); nvn_gpu_draw(c,&da);
        // 换管线（管线自己的 VkPipelineLayout 换了）后两组都要重新取：备忘关着，每组一次取组，共 2；
        // 备忘开着：pipe 与 pipe2 的描述符布局结构相同、按结构共用同一个 VkDescriptorSetLayout（#1096），备忘的键（布局句柄 + 每条描述符的
        // binding / 类型 / 资源）相同 → 两组都命中、0 次取组；组仍照常重绑（下面的 48 像素核对）。
        CHECK(desc_demand()-before==(g_memo_on ? 0u : 2u),"C85 layout换后：备忘%s，期望取组 %u 次，实为 %u 次",g_memo_on?"开":"关",g_memo_on?0u:2u,desc_demand()-before);
        // 旧 stage 绑定仍保留：无组管线必须忽略它们，回来后重建全部组。
        nvn_gpu_set_pipe(c,no_sets); before=desc_demand(); nvn_gpu_draw(c,&da);
        CHECK(desc_demand()==before,"C85 无组管线不分配");
        nvn_gpu_set_pipe(c,pipe2); before=desc_demand(); nvn_gpu_draw(c,&da);
        CHECK(g_memo_on || desc_demand()-before==2,"C85 有组→无组→有组重建两组");
        nvn_gpu_pass_end(c); nvn_gpu_copy_tex_buf(c,out,0,0,NULL,rb,0,48,192);
        CHECK(OK(nvn_gpu_cmd_commit(c,130+seg,on_done,NULL,&e)),"C85 commit %s",e.why); CHECK(OK(nvn_gpu_fence_wait(130+seg,5000000000ull,&e)),"C85 wait %s",e.why);
        const uint8_t *px=buf_cpu(rb); int bad=0; for(int y=0;y<4;y++)for(int x=0;x<12;x++){const uint8_t *v=px+(y*12+x)*4;if(v[0]!=(x<8?255:0)||v[1]||v[2]!=(x<8?0:255)||v[3]!=255)bad++;}
        CHECK(!bad,"C85 段%d 48像素红/红/蓝，不符%d",seg,bad);
    }
    nvn_gpu_view_release(rv); nvn_gpu_view_release(bv); nvn_gpu_view_release(ov); nvn_gpu_tex_release(red); nvn_gpu_tex_release(blue); nvn_gpu_tex_release(out); nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(vb,NULL,NULL); nvn_gpu_buf_release(rb,NULL,NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(plain); free(vm);free(fm);
}

// C87：4×标准样本 x=1/8、3/8、5/8、7/8；半像素红矩形覆盖其中2个，另2个保留蓝。
// 解析后首列应为 (128,0,128,255)，其它列为蓝；不是只清同一种颜色。
static void c87_resolve(const char *dir) {
    CHECK(g_dev.properties.limits.standardSampleLocations, "C87 手算覆盖需要标准采样位置");
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
    td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.d.samples = 4; td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex a = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_tex_create(&td, &a, &e)), "C87 target5 MSAA 创建：%s", e.why);
    if (!a) return;
    // target6 的 depth 是数组层数，不是 VkImage 的第三维。
    nvn_gpu_tex arr = NULL; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY; td.d.depth = 3;
    CHECK(OK(nvn_gpu_tex_create(&td, &arr, &e)), "C87 target6 创建：%s", e.why);
    if (arr) { nvn_gpu_tex_info ti = {0}; nvn_gpu_tex_info_get(arr, &ti); CHECK(ti.layers == 3 && ti.samples == 4, "C87 target6 层数/样本 %u/%u", ti.layers, ti.samples); nvn_gpu_tex_release(arr); }
    td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; td.d.depth = 1; td.d.samples = 3;
    nvn_gpu_tex bad = NULL;
    CHECK(nvn_gpu_tex_create(&td, &bad, &e).code == NVN_GPU_E_UNSUPPORTED && !bad, "C87 非法采样数应 UNSUPPORTED");
    td.d.samples = 64;
    VkImageFormatProperties ip = {0};
    VkResult ir = vkGetPhysicalDeviceImageFormatProperties(g_dev.physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ip);
    if (ir == VK_SUCCESS && !(ip.sampleCounts & VK_SAMPLE_COUNT_64_BIT))
        CHECK(nvn_gpu_tex_create(&td, &bad, &e).code == NVN_GPU_E_UNSUPPORTED && !bad, "C87 设备不支持64采样应 UNSUPPORTED");
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C87 shader");
    nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 4; pd.prim = 5; pd.chmask.mask = 0xf; pd.color.logic_op = 3;
    pd.attr[0].fmt = pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C87 MSAA管线：%s", e.why);
    // 覆盖由几何边定，不靠小数视口：视口边界只保 viewportSubPixelBits 位（规范最小 0，Mali-G715 报 0，宽 0.5 的视口可能一个样本都不盖），
    // 顶点位置至少 4 位子像素。NDC x=-0.75 落在帧缓冲 x=0.5，标准 4× 位置下首列盖住 x=0.375 / 0.125 两个样本。
    float verts[4][8] = {{-1,-1,0,1,1,0,0,1},{-1,1,0,1,1,0,0,1},{-0.75f,-1,0,1,1,0,0,1},{-0.75f,1,0,1,1,0,0,1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex b = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view av = make_view(a), bv = make_view(b); nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc p = {0}; p.ncolor = 1; p.w = p.h = 4; p.layers = 1;
    p.color[0].view = av; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = p.color[0].clear.f[3] = 1;
    nvn_gpu_pass_begin(c, &p); nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn = {0}; dyn.origin_ll = pd.origin_ll; dyn.nvp = 1; dyn.vp[0][2] = 4; dyn.vp[0][3] = 4; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da = {0}; da.prim = 5; da.count = 4; da.instances = 1; nvn_gpu_draw(c, &da); nvn_gpu_pass_end(c);
    NVNcopyRegion rr = {0, 0, 0, 4, 4, 1}; nvn_gpu_blit(c, av, &rr, bv, &rr, 0);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 160, on_done, NULL, &e); CHECK(OK(rc), "C87 resolve commit：%d %s", rc.code, e.why);
    if (OK(rc)) {
        CHECK(OK(nvn_gpu_fence_wait(160, 5000000000ull, &e)), "C87 wait：%s", e.why);
        const uint8_t *px = buf_cpu(rb); int badpx = 0;
        // vkCmdResolveImage 对 normalized 格式按实现决定的方式合成样本，128 不是规范值：
        // 样本一致的通道判精确值；首列红、蓝各在 [96,159]（2/4 平均 127.5，1/4、3/4 落在外面），且红 + 蓝在 255±2。
        for (int i = 0; i < 16; i++) {
            const uint8_t *q = px + i * 4; int mix = i % 4 == 0, sum = q[0] + q[2];
            int ok = !q[1] && q[3] == 255 && (mix ? q[0] >= 96 && q[0] <= 159 && q[2] >= 96 && q[2] <= 159 && sum >= 253 && sum <= 257
                                                   : !q[0] && q[2] == 255);
            if (!ok) badpx++; }
        CHECK(!badpx, "C87 混合解析16像素不符 %d，首像素 %u,%u,%u,%u", badpx, px[0], px[1], px[2], px[3]);
    }
    nvn_gpu_view_release(av); nvn_gpu_view_release(bv); nvn_gpu_tex_release(a); nvn_gpu_tex_release(b);
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}

// C977b（#977）：实例除数。4 个实例各占 16 像素一列，颜色来自除数 2 的第 1 条流（每实例颜色 红 绿 蓝 红）：
// 期望列色 {红, 红, 绿, 绿}；除数被丢成每实例一次时是 {红, 绿, 蓝, 红}。
// 设备没有 VK_EXT_vertex_attribute_divisor（sim_tiers no_vertex_divisor 档）时，除数 2 的管线必须具名 UNSUPPORTED，不能静默退回。
static int c977_dev_has_ext(const char *name) {
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(g_dev.physical, NULL, &n, NULL);
    VkExtensionProperties *x = calloc(n ? n : 1, sizeof *x); if (!x) return 0;
    vkEnumerateDeviceExtensionProperties(g_dev.physical, NULL, &n, x);
    int found = 0; for (uint32_t i = 0; i < n; i++) if (!strcmp(x[i].extensionName, name)) found = 1;
    free(x); return found;
}
static void c977_divisor(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL; nvn_gpu_err e;
    CHECK(load_shader_pair(dir, "div.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C977b shader");
    if (!vs || !fs) { free(vsm); free(fsm); return; }
    nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 3; pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 5; pd.chmask.mask = 0xf;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].stream = 1;
    pd.stream[0].stride = 16; pd.stream[1].stride = 16; pd.stream[1].divisor = 2;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc pr = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    if (!c977_dev_has_ext("VK_EXT_vertex_attribute_divisor")) {
        CHECK(pr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "实例除数 2"), "C977b 无除数扩展：期望具名 UNSUPPORTED，真结果 %d %s", pr.code, OK(pr) ? "" : e.why);
        if (pr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "实例除数 2"))
            printf("C977b 实例除数：本设备没有 VK_EXT_vertex_attribute_divisor，除数 2 具名 UNSUPPORTED（%s）\n", e.why);
        goto out;
    }
    CHECK(OK(pr), "C977b 除数 2 管线：%d %s", pr.code, e.why);
    if (!pipe) goto out;
    {
        static const float pos[4][4] = {{-1, -1, 0, 1}, {-1, 1, 0, 1}, {-0.5f, -1, 0, 1}, {-0.5f, 1, 0, 1}};
        static const float col[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 0, 0, 1}};
        nvn_gpu_buf pb = make_buf(sizeof pos, NVN_GPU_MEM_SHARED, pos), cb = make_buf(sizeof col, NVN_GPU_MEM_SHARED, col);
        nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
        nvn_gpu_view ov = make_view(out);
        nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc p = {0};
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.color[0].clear.f[3] = 1.0f; p.w = 64; p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(c, &p); nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_dyn dyn = {0}; dyn.origin_ll = pd.origin_ll; dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
        dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64; nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, pb, 0); nvn_gpu_set_vbuf(c, 1, cb, 0);
        nvn_gpu_draw_args da = {0}; da.prim = 5; da.count = 4; da.instances = 4; nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
        nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        CHECK(OK(rc), "C977b commit：%d %s", rc.code, e.why);
        if (OK(rc)) {
            CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C977b wait：%s", e.why);
            const uint8_t *px = buf_cpu(rb);
            static const char *nm[4] = {"红", "红", "绿", "绿"};
            static const uint8_t want[4][3] = {{255, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 255, 0}};
            char got[128] = {0}; int bad = 0;
            for (int k = 0; k < 4; k++) {
                const uint8_t *q = px + (32 * 64 + 16 * k + 8) * 4;
                size_t l = strlen(got); snprintf(got + l, sizeof got - l, "%s%02x%02x%02x", k ? " " : "", q[0], q[1], q[2]);
                if (q[0] != want[k][0] || q[1] != want[k][1] || q[2] != want[k][2]) bad++;
            }
            CHECK(!bad, "C977b 除数 2、实例 4：期望列色 {%s,%s,%s,%s}=ff0000 ff0000 00ff00 00ff00，真结果 %s", nm[0], nm[1], nm[2], nm[3], got);
            if (!bad) printf("C977b 实例除数 2、实例 4：列色 %s = 期望 {c0,c0,c1,c1}\n", got);
        }
        nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
        nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_buf_release(pb, NULL, NULL); nvn_gpu_buf_release(cb, NULL, NULL);
    }
out:
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}

// C977a（#977）：透明度转覆盖率。4 倍采样目标清成黑，整屏画 (1,0,0,0.5)，解析后读红分量：
// 关 a2c 时 4 个样本全覆盖，红 = 255；开 a2c 时 alpha 0.5 只覆盖部分样本，16 个像素的红分量均值必须在 (0,255) 之间。
static void c977_a2c(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL; nvn_gpu_err e;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C977a shader");
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
    td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.d.samples = 4; td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex a = NULL;
    CHECK(OK(nvn_gpu_tex_create(&td, &a, &e)), "C977a 4 倍采样目标：%s", e.why);
    float verts[4][8] = {{-1,-1,0,1,1,0,0,0.5f},{-1,1,0,1,1,0,0,0.5f},{1,-1,0,1,1,0,0,0.5f},{1,1,0,1,1,0,0,0.5f}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex b = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    if (!a || !vs || !fs || !vb || !b || !rb) goto out;
    nvn_gpu_view av = make_view(a), bv = make_view(b);
    double mean[2] = {0, 0};
    for (int on = 0; on < 2; on++) {
        nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
        pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
        pd.samples = 4; pd.prim = 5; pd.chmask.mask = 0xf; pd.color.logic_op = 3;
        pd.ms.enable = 1; pd.ms.samples = 4; pd.ms.a2c = (uint8_t)on;
        pd.attr[0].fmt = pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.stream[0].stride = 32;
        nvn_gpu_pipe pipe = NULL;
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C977a 管线 a2c=%d：%s", on, e.why);
        if (!pipe) continue;
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc p = {0}; p.ncolor = 1; p.w = p.h = 4; p.layers = 1;
        p.color[0].view = av; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.color[0].clear.f[3] = 1;
        nvn_gpu_pass_begin(c, &p); nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_dyn dyn = {0}; dyn.origin_ll = pd.origin_ll; dyn.nvp = 1; dyn.vp[0][2] = 4; dyn.vp[0][3] = 4; dyn.vp[0][5] = 1;
        dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_draw_args da = {0}; da.prim = 5; da.count = 4; da.instances = 1; nvn_gpu_draw(c, &da); nvn_gpu_pass_end(c);
        NVNcopyRegion rr = {0, 0, 0, 4, 4, 1}; nvn_gpu_blit(c, av, &rr, bv, &rr, 0);
        nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 0, 16, 64);
        nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, (uint64_t)(1 + on), on_done, NULL, &e);
        CHECK(OK(rc), "C977a commit a2c=%d：%d %s", on, rc.code, e.why);
        if (!OK(rc)) continue;
        CHECK(OK(nvn_gpu_fence_wait((uint64_t)(1 + on), 5000000000ull, &e)), "C977a wait：%s", e.why);
        const uint8_t *px = buf_cpu(rb); int sum = 0;
        for (int i = 0; i < 16; i++) sum += px[i * 4];
        mean[on] = sum / 16.0;
        printf("C977a a2c=%d：解析后红分量均值 %.1f（首像素 %u,%u,%u,%u）\n", on, mean[on], px[0], px[1], px[2], px[3]);
    }
    CHECK(mean[0] == 255.0, "C977a 关 a2c：期望红均值 255，真结果 %.1f", mean[0]);
    CHECK(mean[1] > 0.0 && mean[1] < 255.0, "C977a 开 a2c：期望红均值在 (0,255)，真结果 %.1f（覆盖率没随 alpha 变）", mean[1]);
    nvn_gpu_view_release(av); nvn_gpu_view_release(bv);
out:
    if (a) nvn_gpu_tex_release(a);
    if (b) nvn_gpu_tex_release(b);
    if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
    if (vb) nvn_gpu_buf_release(vb, NULL, NULL);
    if (vs) nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    free(vsm); free(fsm);
}

// 每个拒绝用例独占会话；必须在发 Vulkan 命令前拒绝。
static void c87_resolve_reject(int k) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = k == 2 ? NVN_FORMAT_R32UI : k == 3 ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
    td.d.samples = 4; td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex a = NULL, b = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_tex_create(&td, &a, &e)), "C87 拒绝源创建：%s", e.why);
    td.d.samples = k == 4 ? 4 : 1;
    if (k == 1) td.d.format = NVN_FORMAT_BGRA8;
    if (k == 7) td.d.width = 8;
    CHECK(OK(nvn_gpu_tex_create(&td, &b, &e)), "C87 拒绝目标创建：%s", e.why);
    nvn_gpu_view av = make_view(a), bv = make_view(b); nvn_gpu_cmd c = begin_cmd();
    NVNcopyRegion sr = {0, 0, 0, 4, 4, 1}, dr = sr; if (k == 0) dr.width = 2; if (k == 5) dr.width = 0;
    nvn_gpu_blit(c, av, &sr, bv, &dr, k == 6);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(rc.code == ((k == 0 || k == 5) ? NVN_GPU_E_ARG : NVN_GPU_E_UNSUPPORTED), "C87 拒绝 %d：%d %s", k, rc.code, e.why);
    nvn_gpu_view_release(av); nvn_gpu_view_release(bv); nvn_gpu_tex_release(a); nvn_gpu_tex_release(b);
}

// C74b：矩形超出通道范围 → 粘滞 E_ARG，整段不提交（原生的矩形清越界是未定义行为，不能放过去）。独占会话。
static void c74b_clear_rect_oob(void) {
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.color = 0; cl.x = 60; cl.y = 0; cl.w = 8; cl.h = 8;   // x 60–67 超出 64
    nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 116, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C74b 越界矩形应 E_ARG，实为 %d %s", r.code, e.why);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
}

// C937：release 之后再录制使用 → 粘滞 E_ARG，报错要带句柄类型和录制位置（真机长局里只有这一行能定位，#937）。
//   同一段里先 use 一次让段持有引用（缓冲 release 后不会被回收，指针仍有效），release，再 use：第二次必须报。独占会话。
// 粘滞错误的原因不进 commit 的 err，随段交给完成回调（真机日志「提交段失败…：原因」就是这里来的）：从回调里取。
static char g_c937_why[256];
static int g_c937_ready;   // 回调写完原因后再置位（release），主线程见位后再读（acquire）
static void c937_done(void *u, const nvn_gpu_done *d) {
    (void)u; snprintf(g_c937_why, sizeof g_c937_why, "%s", d->why ? d->why : "");
    __atomic_store_n(&g_c937_ready, 1, __ATOMIC_RELEASE);
}
static void c937_use_after_release(void) {
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf_use(c, b);
    nvn_gpu_buf_release(b, NULL, NULL);
    nvn_gpu_buf_use(c, b);
    nvn_gpu_err e; g_c937_why[0] = 0; __atomic_store_n(&g_c937_ready, 0, __ATOMIC_RELEASE);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 117, c937_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C937 release 后再用应 E_ARG，实为 %d", r.code);
    nvn_gpu_fence_wait(117, 5000000000ull, &e);   // 失败段也走完成回调；回调可能在别的线程上稍后才到，最多再等 2 秒
    for (int i = 0; i < 200 && !__atomic_load_n(&g_c937_ready, __ATOMIC_ACQUIRE); i++) { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }
    CHECK(__atomic_load_n(&g_c937_ready, __ATOMIC_ACQUIRE), "C937 失败段 2 秒内没走完成回调");
    CHECK(strstr(g_c937_why, "release 之后仍在使用句柄（buf ") && strstr(g_c937_why, "录制于 nvn_gpu_buf_use:"),
          "C937 报错应带类型与录制位置：%s", g_c937_why);
}

// C75：深度偏移。开关（NVN 偏移开关的填充位 = 4）进管线，factor / units / clamp 走动态状态。
//   深度整屏清成 0.5；两笔都画在 z = 0.5、深度 LESS，动态状态里都带 units = −4、factor = 1000：
//   ① 绿，左半屏，管线没开偏移 → 0.5 < 0.5 不成立，画不上；
//   ② 蓝，右半屏，管线开了偏移 → 平铺三角形斜率为 0，factor 那一项是 0，深度 = 0.5 − 4 个最小可分辨差 < 0.5，画上。
// 期望（手算）：左半黑、右半蓝。偏移没进管线则右半也黑；units 与 factor 接反则偏移是 +1000 个可分辨差，右半也黑；
// 没开偏移的管线若吃了动态量则左半出绿。
static void c75_depth_bias(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL, vs2 = NULL, fs2 = NULL; void *vm = NULL, *fm = NULL, *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd, pd2;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8); pd.ds.dt = 1; pd.ds.dw = 0; pd.ds.df = 2;
    fill_gpipe_desc(&pd2, vs2, fs2, NVN_FORMAT_DEPTH24_STENCIL8); pd2.ds.dt = 1; pd2.ds.dw = 0; pd2.ds.df = 2; pd2.poly.offset_en = 4;
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL, pipe2 = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C75 pipe：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C75 pipe2：%d %s", e.rc.code, e.why);
    CHECK(pipe != pipe2, "C75 偏移开关不同的两份描述拿到了同一条管线");
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = half_depth_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 0.5f; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    dyn.po.units = -4; dyn.po.factor = 1000; dyn.po.clamp = 0;
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_set_pipe(c, pipe); dyn.sc[2] = 32; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    nvn_gpu_set_pipe(c, pipe2); dyn.sc[0] = 32; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 117, on_done, NULL, &e);
    CHECK(OK(r), "C75 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(117, 5000000000ull, &e);
    CHECK(OK(r), "C75 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 0, 0, "C75 左半（没开偏移，0.5 < 0.5 不过：黑）");
    expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C75 右半（偏移 −4 个可分辨差：蓝）");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv); nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2);
    free(vm); free(fm); free(vm2); free(fm2);
}

// C85（#708）：冗余状态消除——管线、动态状态、顶点 / 索引缓冲、push 只在和上次真正录进去的不同时才录。
//   计数（期望值手算，下标：管线、视口、裁剪、混合常量、模板、深度偏移、顶点缓冲、索引缓冲、push）：
//   ① 同一通道里两笔完全相同的带索引绘制 → 各录 1 次；② 第三笔只改裁剪 → 只有裁剪多 1 次；
//   ③ 第四笔只换顶点缓冲的偏移、索引偏移 → 只有顶点缓冲、索引缓冲各多 1 次；④ 矩形清除之后同样的一笔 → 全部再录一遍；
//   ⑤ 新通道里同样的一笔 → 全部再录一遍；⑥ push 常量：同样的字节再 push 不重发，内容变了重发。
//   读回（维护方在 #708 要求的）：通道清黑；第一笔绿、裁剪左半（x 0–31）；第二笔只改裁剪到 x 48–63。
//   期望：x 0–31 绿、32–47 黑、48–63 绿。第二笔的裁剪若被当成「没变」漏发，就会再画在左半，右边一条留黑。
static void c85_redundant_state(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd; fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C85 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    const float verts[4][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}, {-1, -1, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    const uint16_t idx[8] = {0, 1, 2, 0, 1, 2, 0, 0};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1; da.ibuf = ib; da.itype = 1;
    uint32_t n[9];
#define C85_EXPECT(tag, a0, a1, a2, a3, a4, a5, a6, a7, a8) do { nvn_gpu_vk_test_emit_counts(c, n); const uint32_t x_[9] = {a0, a1, a2, a3, a4, a5, a6, a7, a8}; \
        CHECK(!memcmp(n, x_, sizeof x_), "C85 %s：录了 管线%u 视口%u 裁剪%u 混合%u 模板%u 偏移%u 顶点%u 索引%u push%u，期望 %u %u %u %u %u %u %u %u %u", tag, \
              n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], a0, a1, a2, a3, a4, a5, a6, a7, a8); } while (0)
    // 计数用的一段（不读回）
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    C85_EXPECT("① 两笔相同", 1, 1, 1, 1, 1, 1, 1, 1, 0);
    dyn.sc[0] = 16; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_draw(c, &da);
    C85_EXPECT("② 只改裁剪", 1, 1, 2, 1, 1, 1, 1, 1, 0);
    nvn_gpu_set_vbuf(c, 0, vb, 16); da.ioff = 6; nvn_gpu_draw(c, &da); da.ioff = 0;
    C85_EXPECT("③ 只换顶点 / 索引偏移", 1, 1, 2, 1, 1, 1, 2, 2, 0);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color = 0; cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.x = 0; cl.y = 0; cl.w = 8; cl.h = 8;
    nvn_gpu_clear_rect(c, &cl); nvn_gpu_set_pipe(c, pipe); nvn_gpu_draw(c, &da);
    C85_EXPECT("④ 矩形清除之后", 2, 2, 3, 2, 2, 2, 3, 3, 0);
    nvn_gpu_pass_end(c);
    p.color[0].load = NVN_GPU_LOAD_LOAD; nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_draw(c, &da);
    C85_EXPECT("⑤ 新通道", 3, 3, 4, 3, 3, 3, 4, 4, 0);
    nvn_gpu_pass_end(c);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 140, on_done, NULL, &e);
    CHECK(OK(r), "C85 commit（计数段）：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(140, 5000000000ull, &e);
    CHECK(OK(r), "C85 wait（计数段）：%d %s", r.code, e.why);
    // 读回用的一段：两笔只差裁剪
    p.color[0].load = NVN_GPU_LOAD_CLEAR; full_dyn(&dyn); dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    c = begin_cmd();
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe); dyn.sc[2] = 32; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    dyn.sc[0] = 48; dyn.sc[2] = 16; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_draw(c, &da);
    C85_EXPECT("读回段", 1, 1, 2, 1, 1, 1, 1, 1, 0);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    r = nvn_gpu_cmd_commit(c, 141, on_done, NULL, &e);
    CHECK(OK(r), "C85 commit（读回段）：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(141, 5000000000ull, &e);
    CHECK(OK(r), "C85 wait（读回段）：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C85 第一笔（裁剪 x 0–31：绿）");
    expect_rect(buf_cpu(rb), 32, 0, 16, 64, 0, 0, 0, "C85 两笔都没画到的 x 32–47（黑）");
    expect_rect(buf_cpu(rb), 48, 0, 16, 64, 0, 255, 0, "C85 第二笔只改了裁剪（x 48–63：绿）");
    // ⑥ push 常量（FS push 的管线，同 C56）：同样的字节再 push 一遍 → 不重发；内容变了 → 重发
    { nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.push_off = 0; fl.push_bytes = 16;
      nvn_gpu_shader pvs = NULL, pfs = NULL; void *pvm = NULL, *pfm = NULL;
      if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &pvs, &pfs, &pvm, &pfm)) { CHECK(0, "读不到 pushc 着色器"); }
      else {
        nvn_gpu_pipe_desc ppd; fill_gpipe_desc(&ppd, pvs, pfs, 0); nvn_gpu_pipe ppipe = NULL;
        CHECK(OK(nvn_gpu_pipe_get(&ppd, NVN_GPU_PIPE_SYNC, &ppipe, &e)), "C85 push pipe：%d %s", e.rc.code, e.why);
        const float ca[4] = {0.25f, 0.5f, 0.75f, 1.0f}, cb[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        nvn_gpu_draw_args pa = da; pa.ibuf = NULL; pa.itype = 0;
        c = begin_cmd(); nvn_gpu_pass_begin(c, &p);
        nvn_gpu_set_pipe(c, ppipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_push(c, NVN_GPU_SH_FS, 0, ca, sizeof ca); nvn_gpu_draw(c, &pa);
        nvn_gpu_push(c, NVN_GPU_SH_FS, 0, ca, sizeof ca); nvn_gpu_draw(c, &pa);
        C85_EXPECT("⑥ 同样的 push", 1, 1, 1, 1, 1, 1, 1, 0, 1);
        nvn_gpu_push(c, NVN_GPU_SH_FS, 0, cb, sizeof cb); nvn_gpu_draw(c, &pa);
        C85_EXPECT("⑥ push 内容变了", 1, 1, 1, 1, 1, 1, 1, 0, 2);
        nvn_gpu_pass_end(c);
        r = nvn_gpu_cmd_commit(c, 142, on_done, NULL, &e);
        CHECK(OK(r), "C85 commit（push 段）：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(142, 5000000000ull, &e);
        CHECK(OK(r), "C85 wait（push 段）：%d %s", r.code, e.why);
        nvn_gpu_shader_release(pvs); nvn_gpu_shader_release(pfs); free(pvm); free(pfm);
      } }
#undef C85_EXPECT
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C86：同一三角形的填充 / 线 / 点；期望来自顶点窗口坐标，不取后端输出当金样。
static void c86_polygon_modes(const char *dir) {
    nvn_gpu_shader vs=NULL,fs=NULL; void *vm=NULL,*fm=NULL;
    if (!load_shader_pair(dir,"point.vert","solid_green.frag",NULL,NULL,&vs,&fs,&vm,&fm)) { CHECK(0,"C86 shaders"); return; }
    nvn_gpu_pipe_desc pd; fill_gpipe_desc(&pd,vs,fs,NVN_FORMAT_DEPTH24_STENCIL8);
    pd.ds.df=2; pd.attr[1].fmt=NVN_FORMAT_RGBA32F; pd.attr[1].offset=16; pd.stream[0].stride=32;
    const float verts[3][8]={{-.625f,-.625f,.5f,1,0,1,0,1},{.625f,-.625f,.5f,1,0,1,0,1},{-.625f,.625f,.5f,1,0,1,0,1}};
    nvn_gpu_buf vb=make_buf(sizeof verts,NVN_GPU_MEM_SHARED,verts),rb=make_buf(8*8*4,NVN_GPU_MEM_SHARED,NULL);
    nvn_gpu_tex out=make_tex(8,8,NVN_FORMAT_RGBA8,NVN_GPU_TEX_RENDER),ds=make_tex(8,8,NVN_FORMAT_DEPTH24_STENCIL8,NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov=make_view(out),dv=make_view(ds);
    nvn_gpu_pipe modes[3]={0}; nvn_gpu_err e;
    // 这是拒绝路径注入：只覆盖后端能力检查，不冒充真实无能力GPU。
    VkBool32 supported=g_dev.core_features.fillModeNonSolid;
    g_dev.core_features.fillModeNonSolid=VK_FALSE;
    for(int mode=0;mode<2;mode++) { pd.poly.mode=(uint8_t)mode; nvn_gpu_pipe denied=NULL; nvn_gpu_rc r=nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&denied,&e); CHECK(r.code==NVN_GPU_E_UNSUPPORTED&&!denied,"C86 无能力mode%d应拒绝，rc%d",mode,r.code); }
    g_dev.core_features.fillModeNonSolid=supported;
    CHECK(supported,"C86 本机需支持非填充正控");
    if(supported) for(int mode=0;mode<3;mode++) { pd.poly.mode=(uint8_t)mode; nvn_gpu_rc r=nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&modes[mode],&e); CHECK(OK(r),"C86 mode%d pipe %s",mode,e.why); }
    CHECK(modes[0]!=modes[1]&&modes[1]!=modes[2]&&modes[0]!=modes[2],"C86 模式必须进管线键");
    // 4次形状（fill,line,point,fill回切）+ 3模式x3偏移位（关/别的模式/自身）。
    for(int test=0;supported&&test<13;test++) {
        int mode=test<4?(test==0||test==3?2:test==1?1:0):(test-4)/3;
        int bias=test<4?-1:(test-4)%3;
        pd.poly.mode=(uint8_t)mode; pd.ds.dt=bias>=0; pd.ds.dw=0; pd.ds.df=2;
        pd.poly.offset_en=bias<1?0:bias==1?(uint8_t)(1u<<((mode+1)%3)):(uint8_t)(1u<<mode);
        nvn_gpu_pipe pipe=NULL; nvn_gpu_rc r=nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e); CHECK(OK(r),"C86 test%d pipe %s",test,e.why);
        if(test<4) CHECK(pipe==modes[mode],"C86 mode%d cache身份",mode);
        nvn_gpu_cmd c=begin_cmd(); nvn_gpu_pass_desc p={0}; p.ncolor=1;p.w=p.h=8;p.layers=1;
        p.color[0].view=ov;p.color[0].load=NVN_GPU_LOAD_CLEAR;p.color[0].store=NVN_GPU_STORE_STORE;p.color[0].clear.f[3]=1;
        p.depth.view=p.stencil.view=dv;p.depth.load=p.stencil.load=NVN_GPU_LOAD_CLEAR;p.depth.store=p.stencil.store=NVN_GPU_STORE_STORE;p.clear_depth=.5f;
        nvn_gpu_pass_begin(c,&p); nvn_gpu_set_pipe(c,pipe); nvn_gpu_set_vbuf(c,0,vb,0);
        nvn_gpu_dyn dyn;full_dyn(&dyn);dyn.vp[0][2]=dyn.vp[0][3]=8;dyn.sc[2]=dyn.sc[3]=8;dyn.po.units=-4;dyn.po.factor=0;
        nvn_gpu_set_dyn(c,&dyn);nvn_gpu_draw_args da={0};da.prim=4;da.count=3;da.instances=1;nvn_gpu_draw(c,&da);nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c,out,0,0,NULL,rb,0,32,256);r=nvn_gpu_cmd_commit(c,140+test,on_done,NULL,&e);CHECK(OK(r),"C86 test%d commit %s",test,e.why);r=nvn_gpu_fence_wait(140+test,5000000000ull,&e);CHECK(OK(r),"C87 wait %s",e.why);
        const uint8_t *px=buf_cpu(rb); int x=mode==2?2:mode==1?3:1,y=mode==2?2:1; int lit=bias<0||bias==2;
        const uint8_t *v=px+(y*8+x)*4;CHECK(v[0]==0&&v[1]==(lit?255:0)&&v[2]==0&&v[3]==255,"C86 test%d mode%d bias%d anchor(%d,%d)=(%u,%u,%u,%u)",test,mode,bias,x,y,v[0],v[1],v[2],v[3]);
        CHECK(px[(7*8+7)*4+1]==0,"C86 外部应黑");
        if(mode!=2) CHECK(px[(2*8+2)*4+1]==0,"C86 非填充内部应黑");
        if(mode==0) CHECK(px[(1*8+3)*4+1]==0,"C86 点模式边中应黑");
    }
    nvn_gpu_view_release(ov);nvn_gpu_view_release(dv);nvn_gpu_tex_release(out);nvn_gpu_tex_release(ds);nvn_gpu_buf_release(vb,NULL,NULL);nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);free(vm);free(fm);
}

// C86b：3D 贴图的深度切片当渲染目标。核心为 3D 附件请求 2D 阵列目标（graphics/nvn/replay/rp_pass.c att_view）；后端的 3D 图像带
//   2D_ARRAY_COMPATIBLE，通用视图按「2D 阵列、base_layer = 切片号」建，开通道时另建附件视图按 att_layer 取那一片。
// 期望（手算）：16x16x4 的 RGBA8，第 k 片初值全是 (0x40+k, 0x80, 0xC0-k, 0xFF)；
//   ① 只把第 2 片清成绿 (0,255,0,255) → 读回：片 2 = 绿，片 0、1、3 还是各自的初值；
//   ② 再把第 0 片清成蓝 (0,0,255,255) → 读回：片 0 = 蓝，片 2 仍是绿，片 1、3 不动。
//   反证：同一片不请求 2D 阵列目标（视图没设 set_target）→ view_create 被拒，原因里有「depth slice 2 of 4」。
enum { C86b_W = 16, C86b_H = 16, C86b_D = 4, C86b_SLICE = C86b_W * C86b_H * 4 };
static void c86_expect(const uint8_t *px, const uint8_t want[4][4], const char *tag) {
    for (int k = 0; k < C86b_D; k++) {
        int nbad = 0; const uint8_t *o = px + k * C86b_SLICE;
        for (int i = 0; i < C86b_W * C86b_H; i++) if (memcmp(o + i * 4, want[k], 4)) nbad++;
        CHECK(!nbad, "C86b %s 第 %d 片：%d 个像素不符（首像素 %02x%02x%02x%02x，期望 %02x%02x%02x%02x）", tag, k, nbad, o[0], o[1], o[2], o[3], want[k][0], want[k][1], want[k][2], want[k][3]);
    }
}
static void c86b_3d_slice_attach(void) {
    nvn_gpu_tex t = make_tex_target(C86b_W, C86b_H, C86b_D, NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER);
    static uint8_t init[C86b_D * C86b_SLICE];
    for (int k = 0; k < C86b_D; k++) for (int i = 0; i < C86b_W * C86b_H; i++) { uint8_t *p = init + k * C86b_SLICE + i * 4; p[0] = (uint8_t)(0x40 + k); p[1] = 0x80; p[2] = (uint8_t)(0xC0 - k); p[3] = 0xFF; }
    nvn_gpu_buf ib = make_buf(sizeof init, NVN_GPU_MEM_SHARED, init), rb = make_buf(sizeof init, NVN_GPU_MEM_SHARED, NULL);
    view_t vs; memset(&vs, 0, sizeof vs); vs.set_layers = 1; vs.min_layer = 2; vs.nlayers = 1;
    nvn_gpu_view bad = NULL, v2 = NULL, v0 = NULL; nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_rc r = nvn_gpu_view_create(t, &vs, &bad, &e);
    CHECK(!OK(r) && !bad && strstr(e.why, "depth slice 2 of 4"), "C86b 不请求 2D 阵列目标的第 2 片视图应被拒：%d %s", r.code, e.why);
    vs.set_target = 1; vs.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    r = nvn_gpu_view_create(t, &vs, &v2, &e);
    CHECK(OK(r) && v2, "C86b 第 2 片视图（2D 阵列目标）：%d %s", r.code, e.why);
    vs.min_layer = 0;
    r = nvn_gpu_view_create(t, &vs, &v0, &e);
    CHECK(OK(r) && v0, "C86b 第 0 片视图（2D 阵列目标）：%d %s", r.code, e.why);
    if (v2 && v0) {
        const float green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
        const uint8_t gr[4] = {0, 255, 0, 255}, bl[4] = {0, 0, 255, 255};
        uint8_t want[4][4] = {{0x40, 0x80, 0xC0, 0xFF}, {0x41, 0x80, 0xBF, 0xFF}, {0x42, 0x80, 0xBE, 0xFF}, {0x43, 0x80, 0xBD, 0xFF}};
        for (int round = 0; round < 2; round++) {
            nvn_gpu_cmd c = begin_cmd();
            if (round == 0) for (uint32_t k = 0; k < C86b_D; k++) nvn_gpu_copy_buf_tex(c, ib, k * C86b_SLICE, C86b_W * 4, C86b_SLICE, t, 0, k, NULL);
            nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
            p.ncolor = 1; p.color[0].view = round == 0 ? v2 : v0; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            memcpy(p.color[0].clear.f, round == 0 ? green : blue, sizeof green);
            p.w = C86b_W; p.h = C86b_H; p.layers = 1;
            nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
            for (uint32_t k = 0; k < C86b_D; k++) nvn_gpu_copy_tex_buf(c, t, 0, k, NULL, rb, k * C86b_SLICE, C86b_W * 4, C86b_SLICE);
            r = nvn_gpu_cmd_commit(c, 170 + round, on_done, NULL, &e);
            CHECK(OK(r), "C86b commit %d：%d %s", round, r.code, e.why);
            r = nvn_gpu_fence_wait(170 + round, 5000000000ull, &e);
            CHECK(OK(r), "C86b wait %d：%d %s", round, r.code, e.why);
            memcpy(want[round == 0 ? 2 : 0], round == 0 ? gr : bl, 4);
            c86_expect(buf_cpu(rb), want, round == 0 ? "只清第 2 片后" : "再清第 0 片后");
        }
    }
    if (v2) nvn_gpu_view_release(v2);
    if (v0) nvn_gpu_view_release(v0);
    nvn_gpu_tex_release(t); nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C86c（#773）：三维块压缩贴图的深度切片经缓冲与非压缩贴图之间逐字节拷。核心把三维块压缩目标的一个深度切片当影子拷就是这么拷的
//   （graphics/nvn/replay/rp_pass.c shadow_copy：通道前基底的切片 → 缓冲 → 二维影子，通道后影子 → 缓冲 → 基底的同一个切片；
//   层号在三维贴图上是深度切片，后端转成 zoffset、depth 1）。影子是 RG32UI，一个像素 8 字节 = BC1 一块。
// 期望（手算）：
//   ① BC1 8x4x2（两块宽、一块高、两个切片）：切片 0 = A0 A1、切片 1 = B0 B1（四块互不相同，A = 0x10..0x1f、B = 0x20..0x2f、C = 0x30..0x3f）。
//      切片 1 经 缓冲 → 2x1 的影子 → 缓冲 读回 = B0 B1（取错切片会读到 A）；
//   ② 往影子写 C0 C1，经缓冲拷回切片 1，再读两个切片：切片 1 = C0 C1，切片 0 仍是 A0 A1（写错切片会盖掉 A）；
//   ③ 三级的 BC1 8x8x2：第 2 级是 2x2 个纹素（一块，不是块高 4 的整数倍）。写 D（0x40..0x47）、经 1x1 的影子拷到切片 0 再读回 = D
//      （核心按「基底那一级的像素尺寸」给拷贝区域，不按块取整，深一级的区域就是这样的）。
//   不能直接采样 BC 的设备（后端走计算解码）建不了原生压缩的三维贴图，跳过。
static nvn_gpu_tex make_tex_3d_levels(uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = (int)fmt; td.d.width = w; td.d.height = h;
    td.d.depth = (int)depth; td.d.levels = (int)levels; td.d.samples = 0; td.usage = usage;
    nvn_gpu_tex t = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r) && t, "tex_create 三维 %ux%ux%u 级 %u 失败：%d %s", w, h, depth, levels, r.code, e.why);
    return t;
}
static void c86c_bc_3d_slice_copy(void) {
    if ((g_dev.bc_decode_mask & (1u << (NVN_FORMAT_RGBA_DXT1 - 0x41))) || !(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE)) { fprintf(stderr, "C86c：BC1 非原生压缩存储，跳过三维切片原块拷贝\n"); return; }
    nvn_gpu_tex bc = make_tex_3d_levels(8, 4, 2, 1, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex bm = make_tex_3d_levels(8, 8, 2, 3, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex px = make_tex(2, 1, NVN_FORMAT_RG32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED), p1 = make_tex(1, 1, NVN_FORMAT_RG32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    uint8_t init[64]; for (int i = 0; i < 64; i++) init[i] = (uint8_t)(0x10 + i);   // A0 A1 = 0x10..0x1f、B = 0x20..0x2f、C = 0x30..0x3f、D（第 2 级的一块）= 0x40..0x47
    nvn_gpu_buf ib = make_buf(sizeof init, NVN_GPU_MEM_SHARED, init), rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    if (!bc || !bm || !px || !p1 || !ib || !rb) return;
    nvn_gpu_err e; nvn_gpu_cmd c = begin_cmd();
    NVNcopyRegion rbc = {0, 0, 0, 8, 4, 1}, rpx = {0, 0, 0, 2, 1, 1};   // 基底按像素（整级 8x4），影子按块（2x1）
    NVNcopyRegion rm = {0, 0, 0, 2, 2, 1}, r1 = {0, 0, 0, 1, 1, 1};     // 第 2 级：2x2 个纹素，影子 1x1
    nvn_gpu_slice s1, s2, s3, s4;
    nvn_gpu_rc r = nvn_gpu_arena_alloc(c, 16, 16, &s1, &e); CHECK(OK(r), "C86c 暂存 1：%d %s", r.code, e.why);
    r = nvn_gpu_arena_alloc(c, 16, 16, &s2, &e); CHECK(OK(r), "C86c 暂存 2：%d %s", r.code, e.why);
    r = nvn_gpu_arena_alloc(c, 8, 16, &s3, &e); CHECK(OK(r), "C86c 暂存 3：%d %s", r.code, e.why);
    r = nvn_gpu_arena_alloc(c, 8, 16, &s4, &e); CHECK(OK(r), "C86c 暂存 4：%d %s", r.code, e.why);
    nvn_gpu_copy_buf_tex(c, ib, 0, 16, 16, bc, 0, 0, &rbc);                 // 切片 0 ← A0 A1
    nvn_gpu_copy_buf_tex(c, ib, 16, 16, 16, bc, 0, 1, &rbc);                // 切片 1 ← B0 B1
    nvn_gpu_copy_tex_buf(c, bc, 0, 1, &rbc, s1.buf, s1.off, 16, 16);        // ① 通道前：基底的切片 1 → 缓冲
    nvn_gpu_copy_buf_tex(c, s1.buf, s1.off, 16, 16, px, 0, 0, &rpx);        //            缓冲 → 影子
    nvn_gpu_copy_tex_buf(c, px, 0, 0, &rpx, rb, 0, 16, 16);                 //            影子读回到 rb[0..16)
    nvn_gpu_copy_buf_tex(c, ib, 32, 16, 16, px, 0, 0, &rpx);                // ② 「通道」往影子写 C0 C1
    nvn_gpu_copy_tex_buf(c, px, 0, 0, &rpx, s2.buf, s2.off, 16, 16);        //    通道后：影子 → 缓冲
    nvn_gpu_copy_buf_tex(c, s2.buf, s2.off, 16, 16, bc, 0, 1, &rbc);        //            缓冲 → 基底的同一个切片 1
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, &rbc, rb, 16, 16, 16);                //            读回切片 0 → rb[16..32)
    nvn_gpu_copy_tex_buf(c, bc, 0, 1, &rbc, rb, 32, 16, 16);                //            读回切片 1 → rb[32..48)
    nvn_gpu_copy_buf_tex(c, ib, 48, 8, 8, bm, 2, 0, &rm);                   // ③ 第 2 级切片 0 ← D（2x2 个纹素、一块）
    nvn_gpu_copy_tex_buf(c, bm, 2, 0, &rm, s3.buf, s3.off, 8, 8);           //    基底第 2 级 → 缓冲
    nvn_gpu_copy_buf_tex(c, s3.buf, s3.off, 8, 8, p1, 0, 0, &r1);           //    缓冲 → 1x1 的影子
    nvn_gpu_copy_tex_buf(c, p1, 0, 0, &r1, s4.buf, s4.off, 8, 8);           //    影子 → 缓冲
    nvn_gpu_copy_buf_tex(c, s4.buf, s4.off, 8, 8, bm, 2, 0, &rm);           //    缓冲 → 基底第 2 级（同一个切片）
    nvn_gpu_copy_tex_buf(c, bm, 2, 0, &rm, rb, 48, 8, 8);                   //    读回 → rb[48..56)
    r = nvn_gpu_cmd_commit(c, 191, on_done, NULL, &e);   // 围栏号同一会话里严格递增：取合入时的下一个空号（比前面所有用例的都大）
    CHECK(OK(r), "C86c commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(191, 5000000000ull, &e);
    CHECK(OK(r), "C86c wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb);
    CHECK(!memcmp(o, init + 16, 16), "C86c ① 三维切片 1 经影子读回 %02x%02x%02x%02x…，期望 B0 B1 = 20212223…（读到 10111213 = 取成了切片 0）", o[0], o[1], o[2], o[3]);
    CHECK(!memcmp(o + 16, init, 16), "C86c ② 切片 0 %02x%02x%02x%02x…，期望仍是 A0 A1 = 10111213…（被盖掉 = 写回写错了切片）", o[16], o[17], o[18], o[19]);
    CHECK(!memcmp(o + 32, init + 32, 16), "C86c ② 切片 1 %02x%02x%02x%02x…，期望 C0 C1 = 30313233…（还是 B = 没写回）", o[32], o[33], o[34], o[35]);
    CHECK(!memcmp(o + 48, init + 48, 8), "C86c ③ 第 2 级 %02x%02x%02x%02x…，期望 D = 40414243…", o[48], o[49], o[50], o[51]);
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(bm); nvn_gpu_tex_release(px); nvn_gpu_tex_release(p1);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
}

// ---- C202（#1087）：命令段暂存缓冲的回收表只留标准大小（VK_ARENA_MIN = 256 KB）的块 ----
// 背景：回收表按「大小完全相同」取用，容量 256 个位置（SEGPOOL_STG_BIG_N；#1087 时是 32 个，#1938 放大）。加载阶段留下的非标准大小块（每块大小各不相同）之后再没人要，会把位置占满，
// 之后每帧要的标准块一部分没命中：每帧各新建、销毁（vkAllocateMemory / vkFreeMemory + 映射的缺页）。
// 计数从 nvn_gpu_stats 读（stg_pool_hit / miss / kept / rejected / full，开会话清零）。一个命令段第一次 arena_alloc 占一块暂存缓冲，
// 同时开着 n 个段就同时占 n 块；段做完放回一块。arena 大小 = max(bytes + 16, 256 KB)：1000 字节 → 标准块，270000 + 64 k 字节 → 各不相同的非标准块。
// 期望（手算，不取自被测实现；每个会话开始时表空、计数全 0；表容量按 256 算）：
//  缺省档（只留标准块）：
//   ① 6 个段各要 1000 字节：没命中 6、命中 0；放回 6 块全留（留 6）；
//   ② 再来一轮同样的 6 个段：命中 6、没命中仍 6；放回再留 6（累计留 12）；
//   ③ 256 个段各要不同的非标准大小（k = 0..255，分 32 批，每批 8 个同时开）：没命中 256（累计 262）；放回 256 块全因非标准没留（非标准没留 256、留仍 12、表满没留 0）；表里还是 6 块标准块；
//   ④ 连续 3 轮、每轮 12 个段各要 1000 字节：第 1 轮命中 6、没命中 6（表里只有 6 块）、放回 12 块全留；第 2、3 轮命中 12、没命中 0、放回 12 块全留；表满没留 始终 0。
//  对照档（nvk_stg_std = 0，旧行为：什么大小都留）：
//   ①② 同上；③ 256 块非标准块没命中 256，放回时表里只剩 250 个空位（256 − 6）：留 250（累计 262）、表满没留 6，非标准没留 0——表被 250 块谁也不会再要的块占着，连同 6 块标准块 = 256 满；
//   ④ 每轮 12 个段：表里只有 6 块标准块 → 命中 6、没命中 6；放回 12 块只有 6 个空位 → 留 6、表满没留 6。每轮都新建 6 次、销毁 6 次，永远这样（真机世界场景每帧 11.8 次就是这个）。
struct stg_stats { uint64_t hit, miss, kept, rej, full; int seen; };
static void stg_stat_cb(void *u, const char *name, uint64_t v) {
    struct stg_stats *o = (struct stg_stats *)u;
    if (!strcmp(name, "stg_pool_hit")) { o->hit = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_miss")) { o->miss = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_kept")) { o->kept = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_rejected")) { o->rej = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_full")) { o->full = v; o->seen++; }
}
static struct stg_stats stg_read(void) { struct stg_stats s; memset(&s, 0, sizeof s); nvn_gpu_stats(stg_stat_cb, &s, 0); return s; }
// 一轮：同时开 n 个段（n <= 16），第 i 个要 bytes0 + i * step 字节的暂存，各做一次 fill，按序提交（围栏号从 *fence 起递增），再等这 n 块都放回。
static void stg_round(nvn_gpu_buf scratch, int n, uint64_t bytes0, uint64_t step, uint32_t *fence, const char *what) {
    nvn_gpu_cmd cs[16]; nvn_gpu_err e; struct stg_stats s0 = stg_read();
    for (int i = 0; i < n; i++) {
        cs[i] = begin_cmd(); if (!cs[i]) return;
        nvn_gpu_slice sl; nvn_gpu_rc r = nvn_gpu_arena_alloc(cs[i], bytes0 + (uint64_t)i * step, 16, &sl, &e);
        CHECK(OK(r), "C202 %s 暂存 %d：%d %s", what, i, r.code, e.why);
        nvn_gpu_fill_buf(cs[i], scratch, 0, 16, 0xA5A5A5A5u);
    }
    for (int i = 0; i < n; i++) {
        nvn_gpu_rc r = nvn_gpu_cmd_commit(cs[i], (*fence)++, on_done, NULL, &e);
        CHECK(OK(r), "C202 %s commit %d：%d %s", what, i, r.code, e.why);
    }
    CHECK(OK(nvn_gpu_fence_wait(*fence - 1, 5000000000ull, &e)), "C202 %s wait：%s", what, e.why);
    for (int t = 0; t < 500; t++) {   // 放回在完成线程上、可能比围栏晚一点：等这 n 块都放回（留 / 没留 都算）
        struct stg_stats s = stg_read();
        if (s.kept + s.rej + s.full >= s0.kept + s0.rej + s0.full + (uint64_t)n) return;
        sleep_ms(10);
    }
    CHECK(0, "C202 %s：5 秒内 %d 个段的暂存块没都放回", what, n);
}
static void c202_arm(int std_flag, const char *tag) {
    g_stg_std_flag = std_flag;
    if (!open_backend()) { g_stg_std_flag = -1; return; }
    nvn_gpu_buf scratch = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    uint32_t fence = 1; struct stg_stats s, p;
    int std_on = std_flag != 0;   // 缺省档（-1）与显式 1 都是「只留标准块」
    s = stg_read();
    CHECK(s.seen == 5, "C202 %s：后端没报全暂存池读数（stg_pool_*），期望 5 项 → %d 项", tag, s.seen);
    CHECK(s.hit == 0 && s.miss == 0 && s.kept == 0 && s.rej == 0 && s.full == 0, "C202 %s：开会话时计数期望全 0 → %llu/%llu/%llu/%llu/%llu", tag,
          (unsigned long long)s.hit, (unsigned long long)s.miss, (unsigned long long)s.kept, (unsigned long long)s.rej, (unsigned long long)s.full);
    stg_round(scratch, 6, 1000, 0, &fence, "① 6 个标准块");
    s = stg_read();
    CHECK(s.hit == 0 && s.miss == 6 && s.kept == 6 && s.rej == 0 && s.full == 0, "C202 %s ①：命中 0 / 没命中 6 / 留 6 / 非标准 0 / 表满 0 → %llu/%llu/%llu/%llu/%llu", tag,
          (unsigned long long)s.hit, (unsigned long long)s.miss, (unsigned long long)s.kept, (unsigned long long)s.rej, (unsigned long long)s.full);
    stg_round(scratch, 6, 1000, 0, &fence, "② 再来 6 个标准块");
    s = stg_read();
    CHECK(s.hit == 6 && s.miss == 6 && s.kept == 12 && s.rej == 0 && s.full == 0, "C202 %s ②：命中 6 / 没命中 6 / 留 12 / 非标准 0 / 表满 0 → %llu/%llu/%llu/%llu/%llu", tag,
          (unsigned long long)s.hit, (unsigned long long)s.miss, (unsigned long long)s.kept, (unsigned long long)s.rej, (unsigned long long)s.full);
    for (int b = 0; b < 32; b++) stg_round(scratch, 8, 270000 + (uint64_t)b * 8 * 64, 64, &fence, "③ 非标准块");   // 256 个大小各不相同（270000 + 64 k）
    s = stg_read();
    if (std_on) CHECK(s.hit == 6 && s.miss == 262 && s.kept == 12 && s.rej == 256 && s.full == 0, "C202 %s ③：命中 6 / 没命中 262 / 留 12 / 非标准没留 256 / 表满 0 → %llu/%llu/%llu/%llu/%llu", tag,
                      (unsigned long long)s.hit, (unsigned long long)s.miss, (unsigned long long)s.kept, (unsigned long long)s.rej, (unsigned long long)s.full);
    else CHECK(s.hit == 6 && s.miss == 262 && s.kept == 262 && s.rej == 0 && s.full == 6, "C202 %s ③：命中 6 / 没命中 262 / 留 262 / 非标准没留 0 / 表满 6 → %llu/%llu/%llu/%llu/%llu", tag,
               (unsigned long long)s.hit, (unsigned long long)s.miss, (unsigned long long)s.kept, (unsigned long long)s.rej, (unsigned long long)s.full);
    for (int r = 0; r < 3; r++) {
        p = stg_read();
        stg_round(scratch, 12, 1000, 0, &fence, "④ 12 个标准块");
        s = stg_read();
        uint64_t dh = s.hit - p.hit, dm = s.miss - p.miss, dk = s.kept - p.kept, df = s.full - p.full;
        uint64_t wh = std_on ? (r == 0 ? 6 : 12) : 6, wm = std_on ? (r == 0 ? 6 : 0) : 6, wk = std_on ? 12 : 6, wf = std_on ? 0 : 6;
        CHECK(dh == wh && dm == wm && dk == wk && df == wf, "C202 %s ④ 第 %d 轮：命中 %llu / 没命中 %llu / 留 %llu / 表满 %llu → %llu/%llu/%llu/%llu", tag, r + 1,
              (unsigned long long)wh, (unsigned long long)wm, (unsigned long long)wk, (unsigned long long)wf,
              (unsigned long long)dh, (unsigned long long)dm, (unsigned long long)dk, (unsigned long long)df);
    }
    nvn_gpu_buf_release(scratch, NULL, NULL);
    nvn_gpu_err ce; CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "C202 %s close：%d %s", tag, ce.rc.code, ce.why);
    g_stg_std_flag = -1;
}
static void c202_stg_pool(void) {
    c202_arm(-1, "缺省档");
    c202_arm(0, "对照档 nvk_stg_std=0");
}

// ---- C210：池镜像同步的暂存写按标准块切开（nvn_gpu_buf_write，stg_chunk 一节）----
// 背景：一笔写要的暂存超过一个标准块（256 KB）时，旧做法新建一个 bytes + 4 字节的非标准块，回收表不收，段完成就销毁，下一次又是一段新映射的内存（memcpy 逐页缺页）。
// 真机（Adreno 8 系，一款 3D 游戏的世界场景，R29 调试包）：每帧池镜像同步约 90 MB，其中约 34 MB 走这条暂存路径，主线程每秒约 20.8 万次缺页。
// 现在：一笔超过一个标准块就按 STG_CHUNK = 262140（= 标准块 262144 - 4：arena_alloc 要 bytes + 4 字节的块，整块用满时恰好是标准块）切开；
// 当前块还剩 4096 字节以上先用满它；回收表放大到 256 个位置。
// 期望（手算，不取自被测实现；会话开始时表空、计数全 0；每轮开 A、B 两个段各一次提交，等它们的暂存块都放回，共 3 轮）：
//  段 A：对 24 MB 目标缓冲，先写 6292456 字节（= 24 × 262140 + 1096）到偏移 12，再写 100 字节到偏移 6300000。
//        分块：前 24 块各 262140 字节（每块只剩 4 字节，不到 4096，下一块另开）+ 1 块 1096 字节；第二笔 100 字节放进最后一块的余量。→ 暂存块 25 个、拷贝命令 26 条；
//  段 B：先写 100000 字节到偏移 7000000，再写 300000 字节到偏移 7200000，最后把第一笔的区间用新内容再写一遍（100000 字节，与第一笔重叠：两次写同一段，之间必须有屏障）。
//        第一笔占一个新块（余 162144）；第二笔 300000 超过一块：当前块余量 162144 在 [4096, 262140) 之内，先写 162144，余下 137856 放进新的标准块（余 124288）；
//        第三笔 100000 放进这块的余量。→ 暂存块 2 个、拷贝命令 1 + 2 + 1 = 4 条。
//  每轮：写缓冲 5 笔、其中超过一个标准块 2 笔（6292456 与 300000）、拷贝命令 30 条；暂存块 27 个：
//        第 1 轮 命中 0 / 没命中 27 / 新建 27 个共 27 × 262144 字节，放回 27 块全留（留 27、非标准没留 0、表满没留 0）；
//        第 2、3 轮 命中 27 / 没命中 0 / 新建 0，放回再留 27、累计 54、81。
//  内容：每轮写完核对目标缓冲里写入区逐字节等于源（段 B 第一笔的区间要是第三笔的新内容；源每轮换种子，旧内容蒙混不过去），空隙保持开局的哨兵 0xEE（切错偏移 / 越界写会改到它）。
//  同步校验：整个单跑过程不许有 Validation Error / SYNC-HAZARD（一笔写之间缺屏障、或把屏障漏掉，段 B 的重叠两笔会报写后写冒险）。
//  表的上限：一笔写 264 × 262140 = 69204960 字节（264 个整块），段完成放回：表里留 256（SEGPOOL_STG_BIG_N）、表满没留 8；
//        紧接着再写一次同样的：命中 256 / 没命中 8（表被放大的上限封住，内存有界）。
struct stg_wr_stats { uint64_t hit, miss, kept, rej, full, n, bytes, copies, big, nblk, nbytes; int seen; };
static void stg_wr_cb(void *u, const char *name, uint64_t v) {
    struct stg_wr_stats *o = (struct stg_wr_stats *)u;
    if (!strcmp(name, "stg_pool_hit")) { o->hit = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_miss")) { o->miss = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_kept")) { o->kept = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_rejected")) { o->rej = v; o->seen++; }
    else if (!strcmp(name, "stg_pool_full")) { o->full = v; o->seen++; }
    else if (!strcmp(name, "stg_write_n")) { o->n = v; o->seen++; }
    else if (!strcmp(name, "stg_write_bytes")) { o->bytes = v; o->seen++; }
    else if (!strcmp(name, "stg_write_copies")) { o->copies = v; o->seen++; }
    else if (!strcmp(name, "stg_write_big")) { o->big = v; o->seen++; }
    else if (!strcmp(name, "stg_new_blocks")) { o->nblk = v; o->seen++; }
    else if (!strcmp(name, "stg_new_bytes")) { o->nbytes = v; o->seen++; }
}
static struct stg_wr_stats stg_wr_read(void) { struct stg_wr_stats s; memset(&s, 0, sizeof s); nvn_gpu_stats(stg_wr_cb, &s, 0); return s; }
static void c210_fill(uint8_t *p, uint64_t n, uint32_t seed) { for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 131u) ^ (i >> 9) ^ (seed * 29u + 7u)); }
// 目标缓冲里 [a, b) 全是哨兵 0xEE 吗；返回第一个不是的位置（都是 = UINT64_MAX）
static uint64_t c210_sentinel(const uint8_t *p, uint64_t a, uint64_t b) { for (uint64_t i = a; i < b; i++) if (p[i] != 0xEE) return i; return UINT64_MAX; }
// 等「放回」：留 + 非标准没留 + 表满没留 的总数到 target
static void c210_wait_returned(uint64_t target, const char *what) {
    for (int t = 0; t < 1000; t++) {
        struct stg_wr_stats s = stg_wr_read();
        if (s.kept + s.rej + s.full >= target) return;
        sleep_ms(10);
    }
    CHECK(0, "C210 %s：10 秒内暂存块没都放回（要 %llu 块）", what, (unsigned long long)target);
}
static void c210_stg_chunk(void) {
    enum { DST = 24 << 20, A1N = 6292456, A1O = 12, A2N = 100, A2O = 6300000, B1N = 100000, B1O = 7000000, B2N = 300000, B2O = 7200000 };
    if (!open_backend()) return;
    nvn_gpu_err e;
    nvn_gpu_buf dst = make_buf(DST, NVN_GPU_MEM_SHARED, NULL);
    uint8_t *a1 = malloc(A1N), *a2 = malloc(A2N), *b1 = malloc(B1N), *b2 = malloc(B2N), *b3 = malloc(B1N);
    CHECK(dst && a1 && a2 && b1 && b2 && b3, "C210：测试缓冲分配");
    if (!(dst && a1 && a2 && b1 && b2 && b3)) goto out;
    memset(buf_cpu(dst), 0xEE, DST);
    struct stg_wr_stats s0 = stg_wr_read();
    CHECK(s0.seen == 11 && s0.n == 0 && s0.copies == 0 && s0.nblk == 0 && s0.hit == 0 && s0.miss == 0, "C210：开会话时 11 项读数期望全 0 → %d 项 n=%llu 拷贝=%llu 新建=%llu",
          s0.seen, (unsigned long long)s0.n, (unsigned long long)s0.copies, (unsigned long long)s0.nblk);
    uint32_t fence = 1;
    for (int r = 1; r <= 3; r++) {
        struct stg_wr_stats p = stg_wr_read();
        c210_fill(a1, A1N, (uint32_t)r); c210_fill(a2, A2N, (uint32_t)r + 10); c210_fill(b1, B1N, (uint32_t)r + 20); c210_fill(b2, B2N, (uint32_t)r + 30); c210_fill(b3, B1N, (uint32_t)r + 40);
        nvn_gpu_cmd ca = begin_cmd(), cb = begin_cmd();
        if (!ca || !cb) goto out;
        nvn_gpu_buf_write(ca, dst, A1O, a1, A1N); nvn_gpu_buf_write(ca, dst, A2O, a2, A2N);
        nvn_gpu_buf_write(cb, dst, B1O, b1, B1N); nvn_gpu_buf_write(cb, dst, B2O, b2, B2N); nvn_gpu_buf_write(cb, dst, B1O, b3, B1N);
        nvn_gpu_rc rca = nvn_gpu_cmd_commit(ca, fence++, on_done, NULL, &e);
        CHECK(OK(rca), "C210 第 %d 轮 commit A：%d %s", r, rca.code, e.why);
        nvn_gpu_rc rcb = nvn_gpu_cmd_commit(cb, fence++, on_done, NULL, &e);
        CHECK(OK(rcb), "C210 第 %d 轮 commit B：%d %s", r, rcb.code, e.why);
        CHECK(OK(nvn_gpu_fence_wait(fence - 1, 10000000000ull, &e)), "C210 第 %d 轮 wait：%s", r, e.why);
        const uint64_t blocks = 27;   // 这一轮用到的暂存块（见上面的手算），段完成各放回一次
        c210_wait_returned(p.kept + p.rej + p.full + blocks, "每轮");
        struct stg_wr_stats s = stg_wr_read();
        const uint64_t wcopies = 30, wnew = r == 1 ? 27 : 0;
        const uint64_t whit = r == 1 ? 0 : 27, wmiss = r == 1 ? 27 : 0;
        const uint64_t wkept = 27, wrej = 0, wbytes = (uint64_t)A1N + A2N + B1N + B2N + B1N;
        CHECK(s.n - p.n == 5 && s.big - p.big == 2 && s.copies - p.copies == wcopies && s.bytes - p.bytes == wbytes,
              "C210 第 %d 轮：写缓冲 5 笔 / 超一块 2 笔 / 拷贝命令 %llu 条 / %llu 字节 → %llu / %llu / %llu / %llu", r, (unsigned long long)wcopies,
              (unsigned long long)wbytes, (unsigned long long)(s.n - p.n), (unsigned long long)(s.big - p.big),
              (unsigned long long)(s.copies - p.copies), (unsigned long long)(s.bytes - p.bytes));
        CHECK(s.hit - p.hit == whit && s.miss - p.miss == wmiss && s.nblk - p.nblk == wnew && s.kept - p.kept == wkept && s.rej - p.rej == wrej && s.full - p.full == 0,
              "C210 第 %d 轮：命中 %llu / 没命中 %llu / 新建 %llu / 留 %llu / 非标准没留 %llu / 表满 0 → %llu/%llu/%llu/%llu/%llu/%llu", r,
              (unsigned long long)whit, (unsigned long long)wmiss, (unsigned long long)wnew, (unsigned long long)wkept, (unsigned long long)wrej,
              (unsigned long long)(s.hit - p.hit), (unsigned long long)(s.miss - p.miss), (unsigned long long)(s.nblk - p.nblk), (unsigned long long)(s.kept - p.kept),
              (unsigned long long)(s.rej - p.rej), (unsigned long long)(s.full - p.full));
        if (r == 1) CHECK(s.nbytes == 27ull * 262144ull, "C210 第 1 轮：新建的 27 块应都是 262144 字节标准块，共 %llu 字节 → %llu", 27ull * 262144ull, (unsigned long long)s.nbytes);
        const uint8_t *m = buf_cpu(dst);
        CHECK(!memcmp(m + A1O, a1, A1N), "C210 第 %d 轮：段 A 第一笔（偏移 %d，%d 字节）内容与源不符", r, A1O, A1N);
        CHECK(!memcmp(m + A2O, a2, A2N), "C210 第 %d 轮：段 A 第二笔（偏移 %d，%d 字节）内容与源不符", r, A2O, A2N);
        CHECK(!memcmp(m + B1O, b3, B1N), "C210 第 %d 轮：段 B 第一笔的区间（偏移 %d，%d 字节）应是第三笔的新内容（读到第一笔的内容 = 重叠两笔的顺序错了）", r, B1O, B1N);
        CHECK(!memcmp(m + B2O, b2, B2N), "C210 第 %d 轮：段 B 第二笔（偏移 %d，%d 字节）内容与源不符（切块边界处 162144 / 137856）", r, B2O, B2N);
        const uint64_t gaps[5][2] = {{0, A1O}, {A1O + A1N, A2O}, {A2O + A2N, B1O}, {B1O + B1N, B2O}, {B2O + B2N, DST}};
        for (int g = 0; g < 5; g++) { uint64_t bad = c210_sentinel(m, gaps[g][0], gaps[g][1]); CHECK(bad == UINT64_MAX, "C210 第 %d 轮：空隙 [%llu, %llu) 的哨兵在 %llu 处被改写", r, (unsigned long long)gaps[g][0], (unsigned long long)gaps[g][1], (unsigned long long)bad); }
    }
    {   // 表的上限
        enum { CH = 262140, NB = 264 };
        const uint64_t total = (uint64_t)NB * CH;
        nvn_gpu_buf big_dst = make_buf(total, NVN_GPU_MEM_SHARED, NULL);
        uint8_t *src = malloc(total);
        CHECK(big_dst && src, "C210 表的上限：大缓冲分配");
        if (big_dst && src) {
            c210_fill(src, total, 99);
            for (int pass = 0; pass < 2; pass++) {
                struct stg_wr_stats p = stg_wr_read();
                nvn_gpu_cmd c = begin_cmd(); if (!c) break;
                nvn_gpu_buf_write(c, big_dst, 0, src, total);
                nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, fence++, on_done, NULL, &e);
                CHECK(OK(rc), "C210 表的上限 第 %d 遍 commit：%d %s", pass + 1, rc.code, e.why);
                CHECK(OK(nvn_gpu_fence_wait(fence - 1, 20000000000ull, &e)), "C210 表的上限 第 %d 遍 wait：%s", pass + 1, e.why);
                c210_wait_returned(p.kept + p.rej + p.full + NB, "表的上限");
                struct stg_wr_stats s = stg_wr_read();
                // 进第 1 遍时表里已有 27 块（前三轮留下的标准块）：要 264 块，27 块命中、237 块新建；放回 264 块，表容量 256 → 留 256、表满没留 8。
                // 第 2 遍进来时表里 256 块：命中 256、新建 8；放回 264 块，同样留 256、表满没留 8。
                const uint64_t whit = pass == 0 ? 27 : 256, wmiss = pass == 0 ? 237 : 8;
                CHECK(s.hit - p.hit == whit && s.miss - p.miss == wmiss && s.kept - p.kept == 256 && s.full - p.full == 8 && s.rej - p.rej == 0,
                      "C210 表的上限 第 %d 遍：命中 %llu / 没命中 %llu / 留 256 / 表满没留 8 / 非标准 0 → %llu/%llu/%llu/%llu/%llu", pass + 1, (unsigned long long)whit, (unsigned long long)wmiss,
                      (unsigned long long)(s.hit - p.hit), (unsigned long long)(s.miss - p.miss), (unsigned long long)(s.kept - p.kept), (unsigned long long)(s.full - p.full), (unsigned long long)(s.rej - p.rej));
                CHECK(!memcmp(buf_cpu(big_dst), src, total), "C210 表的上限 第 %d 遍：69204960 字节内容与源不符", pass + 1);
            }
        }
        free(src); nvn_gpu_buf_release(big_dst, NULL, NULL);
    }
out:
    free(a1); free(a2); free(b1); free(b2); free(b3);
    if (dst) nvn_gpu_buf_release(dst, NULL, NULL);
    nvn_gpu_err ce; CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "C210 close：%d %s", ce.rc.code, ce.why);
}

// ---- C206（#1194）：段内「已引用句柄」备忘 ----
// 背景：回放核心每笔绘制都把整张绑定表交给 nvn_gpu_bind，后端对表里每一项 cmd_ref 一次（取 g_mu、查 released、计数 +1、追加引用表），
// 真机世界场景每帧约 1.7 万次，而同一段里反复绑的是同一批句柄。同一段里引用过的句柄，段一直持有它的一份引用（引用表在段完成时才归还），
// 再引用只是白付代价；备忘（产品里恒开）把第二次起的引用直接放过去，条件是「本段已成功引用过它、且那之后没有任何句柄被 release」。
// 用 buf_use（走同一个 cmd_ref）驱动缓冲句柄；对照档靠测试钩子 nvn_gpu_vk_test_ref_seen(0) 把备忘关掉（每次都走原路径），臂间只差这一个变量。
// 每次引用要么命中备忘（段里的 refseen_hits 加一）、要么追加一条引用表项（报错的除外），所以「命中数 + 引用表条数 = 引用次数」是恒等式。
// 期望（手算，不取自被测实现；失败段会让时间线进失败终态，所以 ③ 单独一个会话）：
//  会话一：
//   ① 同一段对同一个缓冲 buf_use 1000 次：引用表条数 = 缓冲被持有的份数；缺省档 1 / 1、命中 999，对照档 1000 / 1000、命中 0；
//   ② 再把两个缓冲交替各用 500 次（累计 2000 次引用）：引用表每一条都对应缓冲上的一份持有，所以「引用表条数 = 两个缓冲持有数之和」两档都成立，
//      「命中数 + 引用表条数 = 2000」两档都成立；对照档恰好 2000 条；缺省档至少 2 条（a、b 各一）、至多 1001 条（a、b 恰好撞槽时每次交替都重记）；
//   ③ release 其中一个 a（段还持着它，指针有效），先 use 另一个 b（让备忘见到新代号：代号变了必须整表清空，只改代号不清表就会留着 a 的旧项），
//      再 use a：两档都必须 E_ARG——备忘不能把「release 之后仍在使用」放过去，
//      报错带「release 之后仍在使用句柄（buf 」与「录制于 nvn_gpu_buf_use:」；失败段完成后：被 release 的缓冲退休恰好 1 次，
//      另一个缓冲的持有数回到 0（归还次数与引用次数对得上：多记、少记都会让它不为 0 或者让退休次数不对）；
//      报错的那次不许记进备忘（成功引用之后才写）：报错之后再 use 同一个 a 一次（粘滞错误只留首错，外面看不出差别，所以读命中数），
//      release 之后这三次引用（b、a、a）的命中数一次都不许增加（两档都是 0：代号变了备忘已清空、报错的又不写）；
//  会话二：
//   ④ 同一段里同一个缓冲先用 10 次，release 一个无关的缓冲，再用 10 次：对照档 20 条；缺省档 ≤ 2 条（无关的 release 也让备忘作废、重记一次）；
//      然后提交成功，段完成后持有数回到 0；
//   ⑥ 段 A 完成归还之后，同一个缓冲在新段 B 里再引用：段 B 的备忘从空开始、不继承段 A 的：用 5 次 → 缺省档引用表恰好 1 条、持有数恰好 1、命中 4，
//      对照档 5 条、持有数 5、命中 0；提交、完成后持有数回到 0（段 B 要是继承了段 A 的备忘，第一次引用就直接放过去，持有数是 0，缓冲没人护着）；
//  会话三：
//   ⑤ 并发：另一个线程反复建缓冲再 release（每次 release 都让代号加一，回放线程同时在读），本线程录 300 段、每段对同一个缓冲 buf_use 50 次、逐段提交：
//      全部 OK，段都完成后持有数回到 0（TSan 档下这里也是无竞争的核对）。
static void c206_wait_refs0(nvn_gpu_buf b, const char *tag, const char *what) {
    for (int t = 0; t < 500; t++) { if (nvn_gpu_vk_test_buf_refs(b) == 0) return; sleep_ms(10); }
    CHECK(0, "C206 %s %s：5 秒后缓冲仍被持有 %u 份（期望 0）", tag, what, nvn_gpu_vk_test_buf_refs(b));
}
struct c206_thr { atomic_int stop, made; };
static void *c206_churn(void *p) {
    struct c206_thr *t = p;
    for (int n = 0; n < 4000 && !atomic_load(&t->stop); n++) {
        nvn_gpu_buf b = NULL; nvn_gpu_err e;
        if (OK(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, NULL, &b, &e)) && b) { nvn_gpu_buf_release(b, NULL, NULL); atomic_fetch_add(&t->made, 1); }
    }
    return NULL;
}
static void c206_arm(int on, const char *tag) {   // on = 1：产品行为（缺省档）；0：对照档（测试钩子把备忘关掉）
    nvn_gpu_err e, ce;
    if (!on) nvn_gpu_vk_test_ref_seen(0);   // 缺省档不碰钩子：核的就是产品缺省值本身（后端静态初值 = 开）
    // ---- 会话一：① ② ③ ----
    if (!open_backend()) { nvn_gpu_vk_test_ref_seen(1); return; }
    {
        const int retired0 = atomic_load(&g_retired);
        nvn_gpu_buf a = make_buf(64, NVN_GPU_MEM_SHARED, NULL), b = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_cmd c = begin_cmd();
        for (int i = 0; i < 1000; i++) nvn_gpu_buf_use(c, a);
        const uint32_t n1 = nvn_gpu_vk_test_cmd_nrefs(c), ra1 = nvn_gpu_vk_test_buf_refs(a), want1 = on ? 1u : 1000u;
        const uint32_t h1 = nvn_gpu_vk_test_cmd_refseen_hits(c);
        CHECK(n1 == want1 && ra1 == want1, "C206 %s ①：同一缓冲 buf_use 1000 次，引用表条数 / 持有数期望 %u / %u → %u / %u", tag, want1, want1, n1, ra1);
        CHECK(h1 == (on ? 999u : 0u), "C206 %s ①：同一缓冲 buf_use 1000 次，备忘命中数期望 %u → %u", tag, on ? 999u : 0u, h1);
        for (int i = 0; i < 1000; i++) nvn_gpu_buf_use(c, (i & 1) ? a : b);
        const uint32_t n2 = nvn_gpu_vk_test_cmd_nrefs(c), ra2 = nvn_gpu_vk_test_buf_refs(a), rb2 = nvn_gpu_vk_test_buf_refs(b);
        const uint32_t h2 = nvn_gpu_vk_test_cmd_refseen_hits(c);
        CHECK(n2 == ra2 + rb2, "C206 %s ②：引用表条数应等于两个缓冲持有数之和：%u vs %u + %u", tag, n2, ra2, rb2);
        CHECK(on ? (n2 >= 2 && n2 <= 1001) : n2 == 2000, "C206 %s ②：交替各用 500 次后引用表条数期望 %s → %u", tag, on ? "2 ~ 1001" : "2000", n2);
        CHECK(h2 + n2 == 2000u, "C206 %s ②：每次引用要么命中要么追加一条，命中数 + 引用表条数应为 2000：%u + %u", tag, h2, n2);
        printf("C206 备忘（%s）：同一缓冲 1000 次 → 引用表 %u 条、命中 %u 次；两缓冲交替各 500 次后共 %u 条、命中 %u 次\n", tag, n1, h1, n2, h2);
        nvn_gpu_buf_release(a, on_retire, NULL);   // 段还持着 a：指针有效、不会销毁
        const uint32_t h3 = nvn_gpu_vk_test_cmd_refseen_hits(c);
        nvn_gpu_buf_use(c, b);                     // 先用别的句柄让备忘「见到」新代号（代号变了必须整表清空，只改代号不清表的话 a 的旧项还在）
        nvn_gpu_buf_use(c, a);                     // release 之后再用：必须被抓出来，备忘不能放过去
        nvn_gpu_buf_use(c, a);                     // 报错之后再引用同一个句柄：仍走原路径（报错的那次没有记进备忘；粘滞错误只留首错，外面只能靠命中数看出来）
        CHECK(nvn_gpu_vk_test_cmd_refseen_hits(c) == h3, "C206 %s ③：release 之后三次引用（b、a、a）的命中数不许增加：%u → %u", tag, h3, nvn_gpu_vk_test_cmd_refseen_hits(c));
        g_c937_why[0] = 0; __atomic_store_n(&g_c937_ready, 0, __ATOMIC_RELEASE);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, c937_done, NULL, &e);
        CHECK(r.code == NVN_GPU_E_ARG, "C206 %s ③：release 之后再 use 应 E_ARG，实为 %d", tag, r.code);
        nvn_gpu_fence_wait(1, 5000000000ull, &e);   // 失败段也走完成回调，回调可能稍后才到
        for (int i = 0; i < 200 && !__atomic_load_n(&g_c937_ready, __ATOMIC_ACQUIRE); i++) sleep_ms(10);
        CHECK(__atomic_load_n(&g_c937_ready, __ATOMIC_ACQUIRE), "C206 %s ③：失败段 2 秒内没走完成回调", tag);
        CHECK(strstr(g_c937_why, "release 之后仍在使用句柄（buf ") && strstr(g_c937_why, "录制于 nvn_gpu_buf_use:"), "C206 %s ③：报错应带类型与录制位置：%s", tag, g_c937_why);
        CHECK(wait_until(&g_retired, retired0 + 1, 2000), "C206 %s ③：release 过的缓冲在失败段完成后没有退休", tag);
        sleep_ms(50);
        CHECK(atomic_load(&g_retired) - retired0 == 1, "C206 %s ③：release 过的缓冲退休次数期望恰好 1 → %d", tag, atomic_load(&g_retired) - retired0);
        c206_wait_refs0(b, tag, "③ 失败段完成后");
        nvn_gpu_buf_release(b, NULL, NULL);
    }
    CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "C206 %s 会话一 close：%d %s", tag, ce.rc.code, ce.why);
    // ---- 会话二：④ ⑥ ----
    if (!open_backend()) { nvn_gpu_vk_test_ref_seen(1); return; }
    {
        nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, NULL), d = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_cmd c = begin_cmd();
        for (int i = 0; i < 10; i++) nvn_gpu_buf_use(c, b);
        nvn_gpu_buf_release(d, NULL, NULL);   // 无关的缓冲（没有任何段用过它，release 即销毁）
        for (int i = 0; i < 10; i++) nvn_gpu_buf_use(c, b);
        const uint32_t n = nvn_gpu_vk_test_cmd_nrefs(c), rb = nvn_gpu_vk_test_buf_refs(b);
        CHECK(n == rb, "C206 %s ④：引用表条数应等于缓冲持有数：%u vs %u", tag, n, rb);
        CHECK(on ? (n >= 1 && n <= 2) : n == 20, "C206 %s ④：无关 release 前后各用 10 次，引用表条数期望 %s → %u", tag, on ? "1 ~ 2" : "20", n);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        CHECK(OK(r), "C206 %s ④ commit：%d %s", tag, r.code, e.why);
        CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C206 %s ④ wait：%s", tag, e.why);
        c206_wait_refs0(b, tag, "④ 段完成后");
        // ⑥ 上面那一段（段 A）已完成归还；同一个缓冲在新段 B 里再引用：段 B 的备忘从空开始，不继承段 A 的
        nvn_gpu_cmd c2 = begin_cmd();
        for (int i = 0; i < 5; i++) nvn_gpu_buf_use(c2, b);
        const uint32_t n6 = nvn_gpu_vk_test_cmd_nrefs(c2), r6 = nvn_gpu_vk_test_buf_refs(b), h6 = nvn_gpu_vk_test_cmd_refseen_hits(c2);
        CHECK(n6 == (on ? 1u : 5u) && r6 == n6, "C206 %s ⑥：新段里同一缓冲 buf_use 5 次，引用表条数 / 持有数期望 %u / %u → %u / %u", tag, on ? 1u : 5u, on ? 1u : 5u, n6, r6);
        CHECK(h6 == (on ? 4u : 0u), "C206 %s ⑥：新段里备忘命中数期望 %u → %u", tag, on ? 4u : 0u, h6);
        nvn_gpu_rc r2 = nvn_gpu_cmd_commit(c2, 2, on_done, NULL, &e);
        CHECK(OK(r2), "C206 %s ⑥ commit：%d %s", tag, r2.code, e.why);
        CHECK(OK(nvn_gpu_fence_wait(2, 5000000000ull, &e)), "C206 %s ⑥ wait：%s", tag, e.why);
        c206_wait_refs0(b, tag, "⑥ 新段完成后");
        nvn_gpu_buf_release(b, NULL, NULL);
    }
    CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "C206 %s 会话二 close：%d %s", tag, ce.rc.code, ce.why);
    // ---- 会话三：⑤ ----
    if (!open_backend()) { nvn_gpu_vk_test_ref_seen(1); return; }
    {
        nvn_gpu_buf a = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
        struct c206_thr th; atomic_init(&th.stop, 0); atomic_init(&th.made, 0);
        pthread_t t; const int started = pthread_create(&t, NULL, c206_churn, &th) == 0;
        CHECK(started, "C206 %s ⑤：建线程失败", tag);
        for (int i = 0; i < 2000 && atomic_load(&th.made) < 20; i++) sleep_ms(1);   // 等对面真的转起来，再开始录
        int bad = 0; uint64_t fence = 1;
        for (int k = 0; k < 300; k++) {
            nvn_gpu_cmd c = begin_cmd();
            if (!c) { bad++; break; }
            for (int i = 0; i < 50; i++) nvn_gpu_buf_use(c, a);
            if (!OK(nvn_gpu_cmd_commit(c, fence++, on_done, NULL, &e))) bad++;
        }
        atomic_store(&th.stop, 1);
        if (started) pthread_join(t, NULL);
        CHECK(bad == 0, "C206 %s ⑤：300 段里 %d 段录 / 提交失败（期望 0）", tag, bad);
        CHECK(OK(nvn_gpu_fence_wait(fence - 1, 20000000000ull, &e)), "C206 %s ⑤ wait：%s", tag, e.why);
        c206_wait_refs0(a, tag, "⑤ 全部段完成后");
        printf("C206 并发（%s）：录制期间对面建 / release 了 %d 个缓冲\n", tag, atomic_load(&th.made));
        nvn_gpu_buf_release(a, NULL, NULL);
    }
    CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "C206 %s 会话三 close：%d %s", tag, ce.rc.code, ce.why);
    nvn_gpu_vk_test_ref_seen(1);
}
static void c206_ref_seen(void) {
    c206_arm(1, "缺省档");
    c206_arm(0, "对照档（测试钩子关备忘）");
    printf("C206 段内已引用句柄备忘：重复引用只记一次 / release 之后再用照样报错且报错的不进备忘 / 无关 release 作废备忘 / 归还次数对得上 / 新段不继承旧段的备忘 / 并发无竞争，缺省与对照两档已核对\n");
}

// ---- C88：驱动管线缓存落盘与读回（#822）----
// 契约（nvn_backend.h open_desc.cache_dir）：给了目录，后端收尾时把驱动的管线缓存写进去，下一局开会话读回来喂给驱动；
// NULL = 不落盘。缓存只是加速不是前提：文件坏了 / 驱动不收旧数据 / 目录写不了，会话照常开、管线照常建、close 照常返回 OK。
// 期望（手算，不取自被测实现）：
//  ① 空目录、会话 1 建 3 条管线（2 图形 + 1 计算）：建缓存时没有初始数据；3 次建管线调用全带着那个缓存句柄（不是空句柄）；
//     close 后目录里恰好 1 个文件，名字 <32 位十六进制>-v<版本>.bin；
//  ② 会话 2（同目录）：交给驱动的初始数据字节数 = 会话 1 收尾时从驱动取出的字节数；统计里 pipe_cache_loaded_bytes 同值、rejected = 0；
//     建同样 3 条管线，同样全带缓存句柄；
//  ③ 文件中间改一个字节：会话 3 照开，读入 0 字节、rejected = 1，管线照建；收尾换成好文件（会话 4 又能读到，数同会话 1 取出的）；
//  ④ 驱动不收带初始数据的缓存（注入）：会话照开，退回空缓存重建（句柄仍有、管线照带）、rejected = 1；
//  ⑤ 缓存目录其实是个普通文件（写不了）：会话照开、管线照建、close 返回 OK，那个文件内容不变；
//  ⑥ cache_dir = NULL：会话照开、管线照建且仍带缓存句柄（本局内复用）、读入 0；
//  ⑦ 开关 nvk_pipe_cache=0：不建缓存（建缓存调用 0 次）、3 次建管线调用全是空句柄、目录里的文件原样不动；
//  ⑧ 缓存目录和它的上一层都还不存在（真机头一局的常态：<状态目录>/direct-cache/nvn-vk 谁都没建过）：收尾补出目录再写，目录里恰好 1 个文件、名字合规。
struct c88_run { int opened, closed; unsigned caches, gp, gp_cached, cp, cp_cached; size_t initial, data; uint64_t st_loaded, st_rejected, st_made; int st_seen; };

static void c88_stat_cb(void *u, const char *name, uint64_t v) {
    struct c88_run *o = (struct c88_run *)u;
    if (!strcmp(name, "pipe_cache_loaded_bytes")) { o->st_loaded = v; o->st_seen++; }
    else if (!strcmp(name, "pipe_cache_rejected")) { o->st_rejected = v; o->st_seen++; }
    else if (!strcmp(name, "pipes_created")) { o->st_made = v; o->st_seen++; }
}

// 建 3 条管线：两条图形（片元着色器不同 → 两条）+ 一条计算。管线归后端缓存持有，着色器对象用完即放（管线持有引用）。
static void c88_make_pipes(const char *sdir) {
    nvn_gpu_shader vs = NULL, fs = NULL, vs2 = NULL, fs2 = NULL, cs = NULL;
    void *vm = NULL, *fm = NULL, *vm2 = NULL, *fm2 = NULL;
    nvn_gpu_err e;
    if (!load_shader_pair(sdir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C88 读不到 solid_green"); return; }
    if (!load_shader_pair(sdir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "C88 读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd, pd2; nvn_gpu_pipe p1 = NULL, p2 = NULL, cp = NULL;
    fill_gpipe_desc(&pd, vs, fs, 0); fill_gpipe_desc(&pd2, vs2, fs2, 0);
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p1, &e)), "C88 图形管线 1：%d %s", e.rc.code, e.why);
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
    if (g_inspect_creation_feedback) {
        nvn_gpu_pipe cached = NULL;
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &cached, &e)) && cached == p1, "same graphics pipeline reused");
    }
#endif
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &p2, &e)), "C88 图形管线 2：%d %s", e.rc.code, e.why);
    char path[512]; size_t csz = 0;
    snprintf(path, sizeof path, "%s/comp.comp.spv", sdir);
    void *cm = read_file(path, &csz);
    CHECK(cm != NULL, "C88 读不到 comp.comp.spv");
    if (cm) {
        nvn_gpu_layout_ent ce; memset(&ce, 0, sizeof ce);
        ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1; ce.stages = 1u << NVN_GPU_SH_CS;
        nvn_gpu_layout cl; memset(&cl, 0, sizeof cl); cl.ent = &ce; cl.nent = 1;
        nvn_gpu_shader_src src; memset(&src, 0, sizeof src);
        src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main"; src.blob = cm; src.bytes = csz; src.layout = &cl;
        CHECK(OK(nvn_gpu_shader_load(&src, &cs, &e)), "C88 计算着色器：%d %s", e.rc.code, e.why);
        nvn_gpu_stage_desc sd; memset(&sd, 0, sizeof sd); sd.sh = cs;
        CHECK(OK(nvn_gpu_cpipe_get(&sd, NVN_GPU_PIPE_SYNC, &cp, &e)), "C88 计算管线：%d %s", e.rc.code, e.why);
        free(cm);
    }
    CHECK(p1 && p2 && p1 != p2, "C88 两条图形管线应当是两个不同的对象");
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2);
    if (cs) nvn_gpu_shader_release(cs);
    free(vm); free(fm); free(vm2); free(fm2);
}

#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
static void *create_demand_pipelines(void *sdir) {
    nvk_pipe_demand_worker_enter();
    c88_make_pipes(sdir);
    return NULL;
}

static void creation_feedback_arm(const char *sdir, struct s2a_vk_device *dev, enum nvk_pipe_feedback_mode mode) {
    const int expected = mode == NVK_PIPE_FEEDBACK_ON;
    g_desc.host_device = dev;
    g_creation_feedback_off = mode == NVK_PIPE_FEEDBACK_OFF;
    if (!open_backend()) {
        CHECK(0, "PSO feedback open");
    } else {
        memset(g_creation_feedback_chained, 0, sizeof g_creation_feedback_chained);
        memset(g_creation_feedback_real_valid, 0, sizeof g_creation_feedback_real_valid);
        g_inspect_creation_feedback = 1;
        const uint64_t before = rp_sync_pipelines_created();
        nvn_gpu_pipe_warm_scope(1);
        c88_make_pipes(sdir);
        nvn_gpu_pipe_warm_scope(0);
        CHECK(rp_sync_pipelines_created() == before, "warm pipelines excluded from long-frame counts");
        c88_make_pipes(sdir);
        CHECK(rp_sync_pipelines_created() - before == 3, "long frame counts two foreground graphics pipelines and one compute");
        g_inspect_creation_feedback = 0;
        const char *arm = expected ? "supported" : g_creation_feedback_off ? "switch-off" : "unavailable";
        printf("PSO feedback arm=%s chained gfx=%u compute=%u (expected %u/%u), real valid=%u/%u\n",
               arm, g_creation_feedback_chained[0], g_creation_feedback_chained[1],
               expected ? 4u : 0u, expected ? 2u : 0u, g_creation_feedback_real_valid[0], g_creation_feedback_real_valid[1]);
        CHECK(g_creation_feedback_chained[0] == (expected ? 4u : 0u) &&
              g_creation_feedback_chained[1] == (expected ? 2u : 0u), "PSO feedback pNext count");
        struct nvk_pipe_creation_stats stats[NVK_PIPE_CREATION_BUCKETS];
        CHECK(nvn_vk_pipe_creation_stats(stats) == mode, "PSO feedback latched mode (%s)", arm);
        static const struct nvk_pipe_creation_stats wanted[NVK_PIPE_CREATION_BUCKETS] = {
            [NVK_PIPE_GFX_WARM] = {.created = 2, .valid = 1, .app_hit = 1, .driver_ns = 100, .driver_p95_le_ns = 103},
            [NVK_PIPE_COMPUTE_WARM] = {.created = 1, .valid = 1, .driver_ns = 80, .driver_p95_le_ns = 87},
            [NVK_PIPE_GFX_DRAW] = {.created = 2, .valid = 2, .app_hit = 1, .driver_ns = 60, .driver_p95_le_ns = 43},
            [NVK_PIPE_COMPUTE_DRAW] = {.created = 1},
        };
        for (unsigned i = 0; i < NVK_PIPE_CREATION_BUCKETS; i++) {
            const struct nvk_pipe_creation_stats got = stats[i], want = wanted[i];
            const uint64_t valid = expected ? want.valid : 0;
            CHECK(got.created == want.created && got.no_feedback == want.created - valid &&
                  got.valid == valid && got.app_hit == (expected ? want.app_hit : 0) &&
                  got.driver_ns == (expected ? want.driver_ns : 0) &&
                  got.driver_p95_le_ns == (expected ? want.driver_p95_le_ns : 0) && got.wall_ns > 0,
                  "PSO feedback bucket %u (%s): created=%llu valid=%llu no_feedback=%llu app_hit=%llu driver_ns=%llu p95_le_ns=%llu wall_ns=%llu",
                  i, arm, (unsigned long long)got.created, (unsigned long long)got.valid, (unsigned long long)got.no_feedback,
                  (unsigned long long)got.app_hit, (unsigned long long)got.driver_ns,
                  (unsigned long long)got.driver_p95_le_ns, (unsigned long long)got.wall_ns);
        }
        nvn_gpu_err e = {0};
        pthread_t worker;
        const int started = pthread_create(&worker, NULL, create_demand_pipelines, (void *)sdir) == 0;
        CHECK(started, "start demand build thread");
        if (started) pthread_join(worker, NULL);
        CHECK(rp_sync_pipelines_created() - before == 3, "background demand builds excluded from long-frame counts");
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "PSO feedback close: %s", e.why);
    }
    g_creation_feedback_off = 0;
    g_desc.host_device = &g_dev;
}

static void creation_feedback_test(const char *sdir) {
    creation_feedback_arm(sdir, &g_dev, NVK_PIPE_FEEDBACK_ON);
    creation_feedback_arm(sdir, &g_dev, NVK_PIPE_FEEDBACK_OFF);
    struct s2a_vk_device without = {0};
    char why[256] = {0};
    g_hide_creation_feedback = 1;
    const int rc = s2a_vk_device_create(&without, why, sizeof why);
    g_hide_creation_feedback = 0;
    CHECK(rc == 0, "PSO feedback device without extension: %s", why);
    if (rc == 0) creation_feedback_arm(sdir, &without, NVK_PIPE_FEEDBACK_UNAVAILABLE);
    s2a_vk_device_destroy(&without, 1);
}

#endif
// 开一个会话（cache_dir 可为 NULL）→ 建 3 条管线 → 读统计 → close。记下这一段里包装函数看到的调用。
static struct c88_run c88_session(const char *sdir, const char *cache_dir) {
    struct c88_run o; memset(&o, 0, sizeof o);
    struct plc_rec b = g_plc_rec;
    g_desc.cache_dir = cache_dir;
    o.opened = open_backend();
    g_desc.cache_dir = NULL;
    if (!o.opened) return o;
    // 会话准备可能创建内部计算管线（如 GPU 解排）；C88 验的是下列三条游戏管线。
    struct plc_rec ready = g_plc_rec;
    c88_make_pipes(sdir);
    nvn_gpu_stats(c88_stat_cb, &o, 0);
    nvn_gpu_err e;
    o.closed = OK(nvn_gpu_close(5000000000ull, &e));
    if (!o.closed) printf("C88 close 失败：%d %s\n", e.rc.code, e.why);
    o.caches = g_plc_rec.cache_creates - b.cache_creates; o.initial = g_plc_rec.last_initial; o.data = g_plc_rec.last_data;
    o.gp = g_plc_rec.gp - ready.gp; o.gp_cached = g_plc_rec.gp_cached - ready.gp_cached;
    o.cp = g_plc_rec.cp - ready.cp; o.cp_cached = g_plc_rec.cp_cached - ready.cp_cached;
    return o;
}

// 数目录里的普通文件（不含 . / ..），记下最后一个的名字和大小。
static int c88_ls(const char *dir, char *name, size_t nn, long *size) {
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[600]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) { n++; if (name) snprintf(name, nn, "%s", de->d_name); if (size) *size = (long)st.st_size; }
        else n += 100;   // 不是普通文件（残留的临时目录等）也算多出来的东西
    }
    closedir(d);
    return n;
}

static int c88_name_ok(const char *n) {   // <32 位小写十六进制>-v<数字>.bin
    size_t l = strlen(n);
    if (l < 32 + 3 + 4 || strcmp(n + l - 4, ".bin")) return 0;
    for (int i = 0; i < 32; i++) if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return 0;
    if (n[32] != '-' || n[33] != 'v') return 0;
    for (size_t i = 34; i < l - 4; i++) if (n[i] < '0' || n[i] > '9') return 0;
    return l > 34 + 4;
}

static void c88_flip_middle_byte(const char *path) {
    FILE *f = fopen(path, "r+b");
    if (!f) { CHECK(0, "C88 打不开 %s 去改一个字节", path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, sz / 2, SEEK_SET);
    int c = fgetc(f);
    fseek(f, sz / 2, SEEK_SET);
    fputc(c ^ 0xFF, f);
    fclose(f);
}

static void c88_pipe_cache(const char *sdir) {
    char tmpl[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/s2a-plc.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    char *root = mkdtemp(tmpl);
    CHECK(root != NULL, "C88 建不出临时目录");
    if (!root) return;
    char dir[600], fname[256] = "", file[900], notdir[700];
    long fsize = 0;
    snprintf(dir, sizeof dir, "%s/cache", root);
    CHECK(mkdir(dir, 0700) == 0, "C88 建不出缓存目录");
    // ① 空目录 → 会话 1
    CHECK(c88_ls(dir, NULL, 0, NULL) == 0, "C88 ① 起始目录应为空");
    struct c88_run r1 = c88_session(sdir, dir);
    CHECK(r1.opened && r1.closed, "C88 ① 会话 1 open=%d close=%d", r1.opened, r1.closed);
    CHECK(r1.caches == 1 && r1.initial == 0, "C88 ① 建缓存应 1 次且没有初始数据 → %u 次、初始 %zu 字节", r1.caches, r1.initial);
    CHECK(r1.gp == 2 && r1.gp_cached == 2, "C88 ① 图形管线创建 2 次、全带缓存句柄 → %u 次、带缓存 %u 次（带空句柄 = 缓存没传进去）", r1.gp, r1.gp_cached);
    CHECK(r1.cp == 1 && r1.cp_cached == 1, "C88 ① 计算管线创建 1 次、带缓存句柄 → %u 次、带缓存 %u 次", r1.cp, r1.cp_cached);
    CHECK(r1.st_seen == 3 && r1.st_loaded == 0 && r1.st_rejected == 0 && r1.st_made == 3, "C88 ① 统计：loaded=%llu rejected=%llu created=%llu（期望 0 / 0 / 3）",
          (unsigned long long)r1.st_loaded, (unsigned long long)r1.st_rejected, (unsigned long long)r1.st_made);
    int nfiles = c88_ls(dir, fname, sizeof fname, &fsize);
    CHECK(nfiles == 1, "C88 ① close 后目录里应恰好 1 个文件（没有残留临时文件）→ %d", nfiles);
    CHECK(nfiles == 1 && c88_name_ok(fname), "C88 ① 文件名应是 <32 位十六进制>-v<版本>.bin → %s", fname);
    CHECK(r1.data > 0, "C88 ① 收尾应从驱动取出缓存数据 → %zu 字节", r1.data);
    snprintf(file, sizeof file, "%s/%s", dir, fname);
    // ② 会话 2：读回
    struct c88_run r2 = c88_session(sdir, dir);
    CHECK(r2.opened && r2.closed, "C88 ② 会话 2 open=%d close=%d", r2.opened, r2.closed);
    CHECK(r2.caches == 1 && r2.initial == r1.data, "C88 ② 交给驱动的初始数据应等于会话 1 收尾取出的 %zu 字节 → %u 次建缓存、%zu 字节", r1.data, r2.caches, r2.initial);
    CHECK(r2.st_loaded == r1.data && r2.st_rejected == 0, "C88 ② 统计 loaded=%llu（期望 %zu）rejected=%llu（期望 0）", (unsigned long long)r2.st_loaded, r1.data, (unsigned long long)r2.st_rejected);
    CHECK(r2.gp == 2 && r2.gp_cached == 2 && r2.cp == 1 && r2.cp_cached == 1, "C88 ② 管线创建应全带缓存句柄 → 图形 %u/%u 计算 %u/%u", r2.gp_cached, r2.gp, r2.cp_cached, r2.cp);
    // ③ 改坏文件
    c88_flip_middle_byte(file);
    struct c88_run r3 = c88_session(sdir, dir);
    CHECK(r3.opened && r3.closed, "C88 ③ 坏文件下会话照开 open=%d close=%d", r3.opened, r3.closed);
    CHECK(r3.st_loaded == 0 && r3.st_rejected == 1 && r3.initial == 0, "C88 ③ 坏文件：读入应为 0、rejected=1、不交初始数据 → loaded=%llu rejected=%llu 初始 %zu 字节", (unsigned long long)r3.st_loaded, (unsigned long long)r3.st_rejected, r3.initial);
    CHECK(r3.gp == 2 && r3.gp_cached == 2 && r3.cp == 1 && r3.cp_cached == 1, "C88 ③ 坏文件下管线照建且带缓存句柄");
    struct c88_run r4 = c88_session(sdir, dir);
    CHECK(r4.st_loaded == r1.data && r4.st_rejected == 0, "C88 ③ 收尾应已把坏文件换成好的：下一局读入 %llu（期望 %zu）、rejected=%llu（期望 0）", (unsigned long long)r4.st_loaded, r1.data, (unsigned long long)r4.st_rejected);
    // ④ 驱动不收旧数据
    g_plc_rec.reject_initial = 1;
    struct c88_run r5 = c88_session(sdir, dir);
    CHECK(r5.opened && r5.closed, "C88 ④ 驱动不收旧数据时会话照开 open=%d close=%d", r5.opened, r5.closed);
    CHECK(r5.caches == 2 && r5.initial == 0, "C88 ④ 应先带旧数据建、被拒后用空缓存重建（共 2 次，最后一次没有初始数据）→ %u 次、最后初始 %zu 字节", r5.caches, r5.initial);
    CHECK(r5.st_loaded == 0 && r5.st_rejected == 1, "C88 ④ 统计 loaded=%llu（期望 0）rejected=%llu（期望 1）", (unsigned long long)r5.st_loaded, (unsigned long long)r5.st_rejected);
    CHECK(r5.gp == 2 && r5.gp_cached == 2 && r5.cp == 1 && r5.cp_cached == 1, "C88 ④ 退回空缓存后管线照建且带缓存句柄 → 图形 %u/%u 计算 %u/%u", r5.gp_cached, r5.gp, r5.cp_cached, r5.cp);
    // ⑤ 目录写不了：路径是个普通文件
    snprintf(notdir, sizeof notdir, "%s/notadir", root);
    { FILE *f = fopen(notdir, "wb"); CHECK(f != NULL, "C88 ⑤ 建不出占位文件"); if (f) { fputs("x", f); fclose(f); } }
    struct c88_run r6 = c88_session(sdir, notdir);
    CHECK(r6.opened && r6.closed, "C88 ⑤ 目录写不了时会话照开、close 返回 OK → open=%d close=%d", r6.opened, r6.closed);
    CHECK(r6.gp == 2 && r6.gp_cached == 2 && r6.cp == 1 && r6.cp_cached == 1, "C88 ⑤ 目录写不了时管线照建");
    { struct stat st; CHECK(stat(notdir, &st) == 0 && st.st_size == 1, "C88 ⑤ 占位文件内容不应被动"); }
    // ⑥ 不给目录
    struct c88_run r7 = c88_session(sdir, NULL);
    CHECK(r7.opened && r7.closed && r7.st_loaded == 0 && r7.st_rejected == 0, "C88 ⑥ cache_dir=NULL：会话照开照关、读入 0、rejected 0");
    CHECK(r7.gp == 2 && r7.gp_cached == 2 && r7.cp == 1 && r7.cp_cached == 1, "C88 ⑥ cache_dir=NULL 时管线仍带缓存句柄（本局内复用）→ 图形 %u/%u 计算 %u/%u", r7.gp_cached, r7.gp, r7.cp_cached, r7.cp);
    // ⑦ 开关关掉：不读、不建缓存、不碰文件
    long before_size = 0; char before_name[256] = "";
    c88_ls(dir, before_name, sizeof before_name, &before_size);
    struct stat st0; snprintf(file, sizeof file, "%s/%s", dir, before_name); stat(file, &st0);
    g_plc_flag_off = 1;
    struct c88_run r8 = c88_session(sdir, dir);
    g_plc_flag_off = 0;
    CHECK(r8.opened && r8.closed, "C88 ⑦ 开关关掉时会话照开照关");
    CHECK(r8.caches == 0, "C88 ⑦ 开关关掉：不应建缓存 → %u 次", r8.caches);
    CHECK(r8.gp == 2 && r8.gp_cached == 0 && r8.cp == 1 && r8.cp_cached == 0, "C88 ⑦ 开关关掉：管线创建都不带缓存句柄 → 图形 %u/%u 带缓存、计算 %u/%u 带缓存", r8.gp_cached, r8.gp, r8.cp_cached, r8.cp);
    CHECK(r8.st_loaded == 0 && r8.st_made == 0, "C88 ⑦ 开关关掉：读入 0、经缓存建 0 条 → loaded=%llu created=%llu", (unsigned long long)r8.st_loaded, (unsigned long long)r8.st_made);
    { struct stat st1; int ok = stat(file, &st1) == 0 && st1.st_size == st0.st_size && st1.st_mtime == st0.st_mtime && c88_ls(dir, NULL, 0, NULL) == 1;
      CHECK(ok, "C88 ⑦ 开关关掉：目录里的文件应原样不动（大小 / 修改时间 / 个数）"); }
    // ⑧ 目录和它的上一层都不存在
    char fresh[700], fresh_dir[760], fresh_name[256] = "", fresh_file[1100];
    long fresh_size = 0;
    snprintf(fresh, sizeof fresh, "%s/fresh", root);
    snprintf(fresh_dir, sizeof fresh_dir, "%s/cache", fresh);
    struct c88_run r9 = c88_session(sdir, fresh_dir);
    CHECK(r9.opened && r9.closed, "C88 ⑧ 目录都不存在时会话照开照关 open=%d close=%d", r9.opened, r9.closed);
    CHECK(r9.gp == 2 && r9.gp_cached == 2 && r9.cp == 1 && r9.cp_cached == 1, "C88 ⑧ 管线照建且带缓存句柄");
    int fresh_n = c88_ls(fresh_dir, fresh_name, sizeof fresh_name, &fresh_size);
    CHECK(fresh_n == 1 && c88_name_ok(fresh_name) && fresh_size > 0,
          "C88 ⑧ 目录原先不存在：收尾应补出目录并写出恰好 1 个合规文件 → 文件数 %d（-1 = 目录没补出来）、名字 %s、%ld 字节", fresh_n, fresh_name, fresh_size);
    struct c88_run r10 = c88_session(sdir, fresh_dir);
    CHECK(r10.st_loaded == r9.data && r10.st_rejected == 0, "C88 ⑧ 补出来的目录里的文件下一局应能读回：loaded=%llu（期望 %zu）rejected=%llu（期望 0）",
          (unsigned long long)r10.st_loaded, r9.data, (unsigned long long)r10.st_rejected);
    printf("C88 管线缓存：文件 %ld 字节（驱动取出 %zu 字节）；重开读回 %zu 字节；坏文件 / 驱动不收 / 目录写不了 / 目录不存在 / 无目录 / 开关关 均照常建管线\n", fsize, r1.data, r2.st_loaded);
    // 清理
    if (fresh_n >= 0 && fresh_name[0]) { snprintf(fresh_file, sizeof fresh_file, "%s/%s", fresh_dir, fresh_name); unlink(fresh_file); }
    rmdir(fresh_dir); rmdir(fresh);
    DIR *d = opendir(dir);
    if (d) { struct dirent *de; while ((de = readdir(d))) { if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { char p[900]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name); unlink(p); } } closedir(d); }
    rmdir(dir); unlink(notdir); rmdir(root);
}

// ---- C89：驱动管线缓存的局中补写（#868）----
// 契约（backend_vulkan.c「局中补写」一节）：给了缓存目录，建管线累计「新增 ≥ N 条」，且头一次距会话开始 ≥ F、之后距上一次保存 ≥ E 时，
// 后端取一份驱动缓存数据交给后台线程落盘（沿用 nvk_driver_cache_write：临时文件 + fsync + 原子替换），被系统杀掉的局也给下一局留下缓存。
// 缺省 N=64 条 / F=10 秒 / E=30 秒；宿主用例用调试开关 nvk_plc_new / nvk_plc_first_ms / nvk_plc_every_ms 改成 3 条 / 0 / 0。
// 驱动缓存数据的大小由用例定（见 g_plc_extra 一段）：lavapipe 的数据恒为文件头，不随管线变，看不出「变没变」。下面「数据 X 字节」= lavapipe 真数据 + 用例补的填充。
// 期望（手算，不取自被测实现；C88 那组 3 条管线 = 2 图形 + 1 计算，阈值 3 条时第 3 条建完的那一刻触发）：
//  ① 空目录、数据 S1：建完 3 条（不 close）→ 交给后台写 1 次（统计 pipe_cache_saves +1，建完即可见）；后台写成功 1 次后，目录里恰好 1 个合规文件、
//     没有临时文件，大小 = 文件头 48 字节 + S1；close 不重写（inode、修改时间都不变：数据大小与上次保存一致就不写）；下一局读回 S1 字节、rejected = 0；
//  ② 负对照（各只改一处、其余同①）：条数没到（阈值 4）/ 头一次的等待没到（1000000 ms）/ 开关 nvk_pipe_cache=0 / 不给目录 → 不触发，close 前目录里没有文件；
//     头一次的等待 1500 ms 的对照：开会话后立刻建够 3 条 → 不触发；光是时间到了也不触发；到点之后再建管线 → 触发、写成；
//  ③ 目录里已有①写完的文件（数据 S1），同样 3 条管线、驱动数据仍是 S1 → 触发了，但与上次一致：不写（saves_same +1、saves 不变），文件原样；
//     检查过之后再建 3 条 → 再检查 1 次（共 2 次，不是每条都查）；
//     对照：驱动数据变大（S2 > S1，只改这一处）→ 写 1 次、文件变成 48 + S2；
//  ④ 第二次保存：头一次之后驱动数据变大、再建 3 条新管线——间隔没到（1000000 ms）→ 不再触发（saves、saves_same 都不动）；间隔 0 → 再触发 1 次、文件变大；
//  ⑤ 写失败只记日志：目录在会话中途被换成普通文件 → 后台写失败 1 次，会话照开、管线照建、close 返回 OK；
//     失败只是这一次：临时文件路径被一个目录占着 → 局中写失败 1 次、目录里没有正式文件；放开后 close，数据大小虽与「上次保存」一致也要再写一次 → 目录里有了恰好 1 个合规文件；
//  ⑥ 后台写途中收局：把临时文件路径占成一个没人读的管道，后台写卡在开文件上；close(200 ms) 返回 OK，用时 ≥ 150 ms（等了后台写，算进截止时间）
//     且 < 3 s（到期放弃，不挂住）；旧文件原样；写还卡着时数据又变大、再建 3 条新管线 → 不再起新的写（同一时刻最多一次、不排队）；
//     放开管道后后台写自己失败收尾、把临时文件清掉；下一局读回旧文件、rejected = 0；
//  ⑦ 被杀（不经关局）：子进程建完管线、等局中写完，自己 SIGKILL；目录里恰好 1 个合规文件，下一局读回、rejected = 0；
//  ⑧ 被杀在写盘途中：子进程的后台写卡在管道上时 SIGKILL；旧文件逐字节不变，残留的临时文件不碍事：下一局照常开、读回旧文件、rejected = 0（读了管道会卡死）；
//  ⑨ 临时文件残留：<文件名>.tmp.<pid> 里是垃圾——没有正式文件时下一局 loaded = 0 且 rejected = 0（残留不当坏文件）、收尾照常写出正式文件；有正式文件时读回正式文件；残留原样不动；
//     负对照：同样的垃圾放在正式文件名上 → rejected = 1（证明上面的 rejected = 0 不是计数坏了）；
//  ⑩ 两个线程同时建管线、驱动数据随管线数变大 → 局中写盘与建管线并行（校验层 0 条由 run.sh 统一核）：每次触发的写都成功、目录里恰好 1 个合规文件，下一局读回、rejected = 0；
//  ⑪ 缓存目录和它的上一层都还不存在（真机头一局的常态）→ 局中写盘由后台线程补出目录，写完（还没 close）目录里就有 1 个合规文件。
#define C89_HDR 48   // 文件头：魔数 4 + 版本 4 + 长度 8 + SHA-256 32（nvn_vk_driver_cache.c）
#define C89_X1 64    // 驱动数据的填充（字节）：数据 S1 = lavapipe 真数据 + 64
#define C89_X2 128   // 驱动数据「变大」后的填充：S2 = lavapipe 真数据 + 128
#define C89_X3 192   // 再变大一次：S3

struct c89_st { uint64_t saves, ok, failed, same, loaded, rejected, made; int seen; };

static void c89_stat_cb(void *u, const char *name, uint64_t v) {
    struct c89_st *o = (struct c89_st *)u;
    if (!strcmp(name, "pipe_cache_saves")) { o->saves = v; o->seen++; }
    else if (!strcmp(name, "pipe_cache_saves_ok")) { o->ok = v; o->seen++; }
    else if (!strcmp(name, "pipe_cache_saves_failed")) { o->failed = v; o->seen++; }
    else if (!strcmp(name, "pipe_cache_saves_same")) { o->same = v; o->seen++; }
    else if (!strcmp(name, "pipe_cache_loaded_bytes")) { o->loaded = v; o->seen++; }
    else if (!strcmp(name, "pipe_cache_rejected")) { o->rejected = v; o->seen++; }
    else if (!strcmp(name, "pipes_created")) { o->made = v; o->seen++; }
}

static struct c89_st c89_stats(void) {
    struct c89_st o; memset(&o, 0, sizeof o);
    nvn_gpu_stats(c89_stat_cb, &o, 0);
    return o;
}

static void c89_sleep_ms(int ms) { struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

static double c89_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6; }

// 开会话：cache_dir 与三个阈值（-1 = 缺省）只在这一次 open 里生效。
static int c89_open(const char *dir, int nnew, int first_ms, int every_ms) {
    g_plc_thr_new = nnew; g_plc_thr_first_ms = first_ms; g_plc_thr_every_ms = every_ms;
    g_desc.cache_dir = dir;
    int ok = open_backend();
    g_desc.cache_dir = NULL;
    g_plc_thr_new = g_plc_thr_first_ms = g_plc_thr_every_ms = -1;
    return ok;
}

// 等后台写收尾（成功或失败都算）：比基线多 want 次，最多等 timeout_ms。
static struct c89_st c89_wait_done(const struct c89_st *base, unsigned want, int timeout_ms) {
    struct c89_st s = c89_stats();
    for (int i = 0; i < timeout_ms / 2 && (s.ok + s.failed) - (base->ok + base->failed) < want; i++) { c89_sleep_ms(2); s = c89_stats(); }
    return s;
}

// 「没被重写」按名字 + 大小 + 修改时间 + 内容判，不比 inode：Mac 上 colima 的 virtiofs 在虚拟机丢页缓存（每 16 秒一次 drop_caches）后，
// 同一个没动过的文件会换 inode 号（实测 7237 → 7238），按 inode 判会把「原样」误报成「重写」
struct c89_dir { int nreg, nother; char name[256]; long size; uint64_t sum; struct timespec mtim; };
static uint64_t c89_file_sum(const char *path) {   // FNV-1a；读不出算 0（与任何真内容的和都不相等的概率可忽略）
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint64_t h = 1469598103934665603ull; int c;
    while ((c = fgetc(f)) != EOF) { h ^= (uint64_t)(unsigned char)c; h *= 1099511628211ull; }
    fclose(f); return h;
}

// 数目录：正式文件（普通文件、名字里没有 .tmp.）记名字 / 大小 / inode / 修改时间；其余（临时文件、管道、子目录）都算 nother。
static struct c89_dir c89_scan(const char *dir) {
    struct c89_dir o; memset(&o, 0, sizeof o);
    DIR *d = opendir(dir);
    if (!d) { o.nreg = -1; return o; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[900]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (!strstr(de->d_name, ".tmp.") && stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            o.nreg++; snprintf(o.name, sizeof o.name, "%s", de->d_name);
            o.size = (long)st.st_size; o.sum = c89_file_sum(p); o.mtim = st.st_mtim;
        } else o.nother++;
    }
    closedir(d);
    return o;
}

static int c89_same_file(const struct c89_dir *a, const struct c89_dir *b) {
    return a->nreg == 1 && b->nreg == 1 && a->sum == b->sum && a->size == b->size &&
           a->mtim.tv_sec == b->mtim.tv_sec && a->mtim.tv_nsec == b->mtim.tv_nsec && !strcmp(a->name, b->name);
}

// 再建 n 条新的图形管线：颜色写掩码 first_mask.. 依次不同（1..14）、图元 prim 给定 → 每条都是新管线；每条之后歇 gap_ms 毫秒（0 = 不歇）。
static void c89_more_pipes_with(nvn_gpu_shader vs, nvn_gpu_shader fs, unsigned first_mask, unsigned n, unsigned prim, unsigned gap_ms) {
    for (unsigned i = 0; i < n; i++) {
        nvn_gpu_pipe_desc pd; nvn_gpu_pipe p = NULL; nvn_gpu_err e;
        fill_gpipe_desc(&pd, vs, fs, 0);
        pd.chmask.mask = (first_mask + i) & 0xF;
        pd.prim = prim;
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &e)) && p, "C89 追加管线 mask=%u prim=%u：%d %s", first_mask + i, prim, e.rc.code, e.why);
        if (gap_ms) c89_sleep_ms((int)gap_ms);
    }
}

static void c89_more_pipes(const char *sdir, unsigned n) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(sdir, "tri3.vert", "out1_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C89 读不到 out1_green"); return; }
    c89_more_pipes_with(vs, fs, 1, n, 4, 0);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// 只建 1 条图形管线（solid_green）的会话：给后面几例铺一个「上一局写完的旧文件」。默认阈值，收尾写盘。
static int c89_seed(const char *sdir, const char *dir) {
    if (!c89_open(dir, -1, -1, -1)) return 0;
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    int ok = load_shader_pair(sdir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm);
    if (ok) {
        nvn_gpu_pipe_desc pd; nvn_gpu_pipe p = NULL; nvn_gpu_err e;
        fill_gpipe_desc(&pd, vs, fs, 0);
        ok = OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &e)) && p;
        nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
    }
    nvn_gpu_err e2;
    int closed = OK(nvn_gpu_close(5000000000ull, &e2));
    return ok && closed;
}

// 下一局：默认阈值，不建管线，读统计后关。返回读到的字节数与 rejected。
static struct c89_st c89_readback(const char *dir) {
    struct c89_st z; memset(&z, 0, sizeof z);
    if (!c89_open(dir, -1, -1, -1)) { CHECK(0, "C89 读回会话 open 失败"); return z; }
    struct c89_st s = c89_stats();
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 读回会话 close：%d %s", e.rc.code, e.why);
    return s;
}

// 放开被占成管道的临时文件路径：读端打开让后台写的 open 返回，再把它写来的字节读掉，直到它自己失败收尾（管道上 fsync 不成，临时文件被它自己清掉）。
// 读端必须一直开着到路径消失：后台写在本进程里，读端早关了它再 write 就是 SIGPIPE。read 返回 0 只表示「此刻没有写端」，不是收尾。
struct c89_unblock { const char *fifo; atomic_int go; };
static void *c89_unblock_main(void *arg) {
    struct c89_unblock *u = (struct c89_unblock *)arg;
    for (int i = 0; i < 350 && !atomic_load(&u->go); i++) c89_sleep_ms(10);   // 最多等 3.5 s：close 要是挂住了，到点也放开，免得整个用例卡死
    int fd = open(u->fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        static char buf[65536];
        for (int i = 0; i < 2000; i++) {   // 最多 10 s
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) continue;
            struct stat st;
            if (stat(u->fifo, &st) != 0) break;   // 路径已被后台写清掉
            c89_sleep_ms(5);
        }
        close(fd);
    }
    return NULL;
}

static int c89_wait_gone(const char *path, int timeout_ms) {
    struct stat st;
    for (int i = 0; i < timeout_ms / 5; i++) { if (stat(path, &st) != 0) return 1; c89_sleep_ms(5); }
    return stat(path, &st) != 0;
}

// 子进程：被 SIGKILL 的那一局。argv = <exe> <着色器目录> --c89-child <缓存目录> <开工信号文件> <kill-after-ok | kill-after-start>
// 等 parent 摆好现场（开工信号文件出现）→ 开会话（阈值 3 / 0 / 0）→ 建 3 条管线 → 等到局中写完（ok）或后台写已交出去（start）→ 自己 SIGKILL，不 close。
static int c89_child_main(char **argv) {
    const char *sdir = argv[1], *dir = argv[3], *gate = argv[4], *mode = argv[5];
    setvbuf(stdout, NULL, _IONBF, 0);   // 被杀时不丢日志
    for (int i = 0; i < 3000 && access(gate, F_OK) != 0; i++) c89_sleep_ms(10);
    if (access(gate, F_OK) != 0) { printf("C89 子进程等不到开工信号\n"); return 3; }
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "c89-child";
    g_plc_strip_tail = 1;
    g_plc_extra = !strcmp(mode, "kill-after-start") ? C89_X2 : C89_X1;   // 写盘途中被杀那局：驱动数据比旧文件里的大，后端才会真去写
    if (!c89_open(dir, 3, 0, 0)) return 3;
    struct c89_st b = c89_stats();
    c88_make_pipes(sdir);
    if (g_fail) return 4;
    int want_ok = !strcmp(mode, "kill-after-ok");
    for (int i = 0; i < 5000; i++) {   // 最多 10 s
        struct c89_st s = c89_stats();
        if (want_ok ? s.ok > b.ok : s.saves > b.saves) { fflush(stdout); kill(getpid(), SIGKILL); }
        c89_sleep_ms(2);
    }
    printf("C89 子进程等不到局中写（%s）\n", mode);
    return 5;
}

extern char **environ;
// 起子进程（还是这个可执行文件，Linux 下用 /proc/self/exe，不依赖 argv[0] 怎么写的）；返回 pid（失败 -1）。
static pid_t c89_spawn_child(const char *sdir, const char *dir, const char *gate, const char *mode) {
    char *args[] = {(char *)"t_backend_vk", (char *)sdir, (char *)"--c89-child", (char *)dir, (char *)gate, (char *)mode, NULL};
    pid_t pid = -1;
    fflush(stdout);
    if (posix_spawn(&pid, "/proc/self/exe", NULL, NULL, args, environ) != 0) return -1;
    return pid;
}

// 等子进程收尾（最多 60 s，到点补一刀）；返回 waitpid 的状态，-1 = 没等到。
static int c89_reap(pid_t pid) {
    int st = 0;
    for (int i = 0; i < 6000; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return st;
        if (r < 0) return -1;
        c89_sleep_ms(10);
    }
    kill(pid, SIGKILL); waitpid(pid, &st, 0);
    return -1;
}

static int c89_touch(const char *path, const void *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (n) fwrite(bytes, 1, n, f);
    fclose(f);
    return 1;
}

static void c89_rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[900]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

// 一个建管线的线程：三种图元（三角形 / 条带 / 扇）× 颜色写掩码 1..14 = 42 条新管线，每条之后歇 1 ms（让局中写盘在建管线的同时一直在发生）。
struct c89_thr { nvn_gpu_shader vs, fs; };
static void *c89_thr_main(void *arg) {
    struct c89_thr *t = (struct c89_thr *)arg;
    for (unsigned prim = 4; prim <= 6; prim++) c89_more_pipes_with(t->vs, t->fs, 1, 14, prim, 1);
    return NULL;
}

// C907：停局但会话保留到进程退出（#860 / #907 Stop 不关后端，走不到 close 的收尾写）时，nvn_gpu_vk_pipe_cache_flush 把驱动管线缓存落盘。
//  ① 阈值调到不会触发局中补写、建 3 条管线：调用前目录为空（负对照：没有这次调用就没有文件）；
//  ② 调用后后台写完成 1 次、目录里恰好 1 个正式文件、无残留临时文件；会话仍开着（之后还能建管线）；
//  ③ 再调一次、没有新管线：记「与上次一致没写」，文件原样。
void nvn_gpu_vk_pipe_cache_flush(void);
static void c907_retained_flush(const char *sdir) {
    char tmpl[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/s2a-plc907.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL, "C907 建不出临时目录");
    if (!dir) return;
    size_t keep_extra = g_plc_extra, keep_per = g_plc_per_pipe; int keep_strip = g_plc_strip_tail;
    g_plc_extra = 0; g_plc_per_pipe = 0; g_plc_strip_tail = 0;
    nvn_gpu_err e;
    CHECK(c89_open(dir, 1000000, 3600000, 3600000), "C907 open");
    c88_make_pipes(sdir);
    struct c89_dir before = c89_scan(dir);
    CHECK(before.nreg == 0 && before.nother == 0, "C907 ① 调用前目录应为空（局中补写不该触发）→ 正式 %d 个、其他 %d 个", before.nreg, before.nother);
    struct c89_st b = c89_stats();
    nvn_gpu_vk_pipe_cache_flush();
    struct c89_st a = c89_wait_done(&b, 1, 5000);
    struct c89_dir d1 = c89_scan(dir);
    CHECK(a.saves == b.saves + 1 && a.ok == b.ok + 1 && a.failed == b.failed, "C907 ② 应交后台写 1 次且成功 → saves +%llu ok +%llu failed +%llu",
          (unsigned long long)(a.saves - b.saves), (unsigned long long)(a.ok - b.ok), (unsigned long long)(a.failed - b.failed));
    CHECK(d1.nreg == 1 && d1.nother == 0 && d1.size > 0, "C907 ② 目录里应恰好 1 个正式文件、无临时文件 → 正式 %d 个、其他 %d 个、%ld 字节", d1.nreg, d1.nother, d1.size);
    c89_sleep_ms(20);   // 后台写线程先记成功、再清「在飞」：留一点时间让它清完，第二次调用不因「在飞」被跳过
    nvn_gpu_vk_pipe_cache_flush();
    struct c89_st a2 = c89_stats();
    struct c89_dir d2 = c89_scan(dir);
    CHECK(a2.saves == a.saves && a2.same == a.same + 1, "C907 ③ 没有新管线：不该再写、应记一次「一致没写」→ saves +%llu same +%llu",
          (unsigned long long)(a2.saves - a.saves), (unsigned long long)(a2.same - a.same));
    CHECK(c89_same_file(&d1, &d2), "C907 ③ 文件应原样不动");
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C907 close：%d %s", e.rc.code, e.why);
    g_plc_extra = keep_extra; g_plc_per_pipe = keep_per; g_plc_strip_tail = keep_strip;
    c89_rm_tree(dir);
    printf("C907 保留路径管线缓存落盘：已核对（%ld 字节）\n", d1.size);
}

static void c89_midrun_save(const char *sdir) {
    char tmpl[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/s2a-plc89.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    char *root = mkdtemp(tmpl);
    CHECK(root != NULL, "C89 建不出临时目录");
    if (!root) return;
    nvn_gpu_err e;
    // 先确认后端有局中补写的统计项：老后端没有它，整个 C89 就地报红（不往下跑，免得老实现上的管道用例卡死在收尾写盘上）。
    CHECK(c89_open(NULL, -1, -1, -1), "C89 open");
    struct c89_st probe = c89_stats();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 probe close：%d %s", e.rc.code, e.why);
    CHECK(probe.seen == 7, "C89 后端应报出局中补写的统计项（pipe_cache_saves / _ok / _failed / _same）→ 读到 %d 项（期望 7 项）", probe.seen);
    if (probe.seen != 7) { rmdir(root); return; }
    g_plc_strip_tail = 1; g_plc_extra = C89_X1;   // 驱动数据 S1 = lavapipe 真数据 + 64 字节填充（见 g_plc_extra 一段）

    // ① 空目录：局中写盘、close 不重写、下一局读回
    char d1[600]; snprintf(d1, sizeof d1, "%s/c1", root); CHECK(mkdir(d1, 0700) == 0, "C89 建不出 c1");
    CHECK(c89_open(d1, 3, 0, 0), "C89 ① open");
    struct c89_st b1 = c89_stats();
    c88_make_pipes(sdir);
    struct c89_st s1 = c89_stats();
    size_t mid1 = g_plc_rec.last_data;
    CHECK(s1.saves == b1.saves + 1, "C89 ① 建完 3 条应触发 1 次局中保存 → 触发 %llu 次", (unsigned long long)(s1.saves - b1.saves));
    struct c89_st w1 = c89_wait_done(&b1, 1, 5000);
    CHECK(w1.ok == b1.ok + 1 && w1.failed == b1.failed, "C89 ① 后台写应成功 1 次 → 成功 +%llu、失败 +%llu", (unsigned long long)(w1.ok - b1.ok), (unsigned long long)(w1.failed - b1.failed));
    struct c89_dir a1 = c89_scan(d1);
    CHECK(a1.nreg == 1 && c88_name_ok(a1.name) && a1.nother == 0, "C89 ① 局中写完（还没 close）目录里应恰好 1 个合规文件、没有临时文件 → 文件 %d、其它 %d、名字 %s", a1.nreg, a1.nother, a1.name);
    CHECK(mid1 > C89_X1 && a1.size == (long)mid1 + C89_HDR, "C89 ① 文件大小应 = 文件头 %d + 局中取出的 %zu 字节（> 填充 %d）→ %ld", C89_HDR, mid1, C89_X1, a1.size);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ① close：%d %s", e.rc.code, e.why);
    struct c89_dir a1b = c89_scan(d1);
    CHECK(c89_same_file(&a1, &a1b) && a1b.nother == 0, "C89 ① close 不应重写：数据大小与局中保存时一致 → 大小 / 修改时间 / 内容不变");
    struct c89_st r1 = c89_readback(d1);
    CHECK(r1.loaded == mid1 && r1.rejected == 0, "C89 ① 下一局读回 %llu 字节（期望 %zu）、rejected=%llu（期望 0）", (unsigned long long)r1.loaded, mid1, (unsigned long long)r1.rejected);

    // ② 负对照：各只改一处
    static const struct { const char *what; int nnew, first, every, off, nodir; int want_final; } neg[] = {
        {"条数没到（阈值 4 条，只建 3 条）", 4, 0, 0, 0, 0, 1},
        {"头一次的等待没到（1000000 ms）", 3, 1000000, 0, 0, 0, 1},
        {"开关 nvk_pipe_cache=0", 3, 0, 0, 1, 0, 0},
        {"不给缓存目录", 3, 0, 0, 0, 1, 0},
    };
    for (int k = 0; k < 4; k++) {
        char dn[600]; snprintf(dn, sizeof dn, "%s/n%d", root, k); CHECK(mkdir(dn, 0700) == 0, "C89 建不出 n%d", k);
        g_plc_flag_off = neg[k].off;
        int op = c89_open(neg[k].nodir ? NULL : dn, neg[k].nnew, neg[k].first, neg[k].every);
        g_plc_flag_off = 0;
        CHECK(op, "C89 ② %s：open", neg[k].what);
        if (!op) continue;
        struct c89_st nb = c89_stats();
        c88_make_pipes(sdir);
        struct c89_st ns = c89_stats();
        CHECK(ns.saves == nb.saves && ns.same == nb.same, "C89 ② %s：不应触发 → 触发 %llu 次、「没变」%llu 次", neg[k].what, (unsigned long long)(ns.saves - nb.saves), (unsigned long long)(ns.same - nb.same));
        struct c89_dir an = c89_scan(dn);
        CHECK(an.nreg == 0 && an.nother == 0, "C89 ② %s：close 前目录里不应有文件 → 文件 %d、其它 %d", neg[k].what, an.nreg, an.nother);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ② %s：close：%d %s", neg[k].what, e.rc.code, e.why);
        struct c89_dir af = c89_scan(dn);
        CHECK(af.nreg == neg[k].want_final && af.nother == 0, "C89 ② %s：close 后目录里应有 %d 个正式文件（收尾写盘照旧）→ %d 个、其它 %d", neg[k].what, neg[k].want_final, af.nreg, af.nother);
        c89_rm_tree(dn);
    }
    {   // ② 对照：「头一次的等待」只差时间这一个变量——开会话后没到点不触发；到点之后再建管线才触发（只在建管线时检查，光是时间到了不触发）
        char dt[600]; snprintf(dt, sizeof dt, "%s/c2t", root); CHECK(mkdir(dt, 0700) == 0, "C89 建不出 c2t");
        double ta = c89_now_ms();
        int opt = c89_open(dt, 3, 1500, 0);
        double tb = c89_now_ms();   // 后端的「开会话时刻」落在 [ta, tb] 之间
        CHECK(opt, "C89 ②t open");
        if (opt) {
            struct c89_st bt = c89_stats();
            c88_make_pipes(sdir);
            double tc = c89_now_ms();
            struct c89_st st = c89_stats();
            if (tc - ta < 1400) CHECK(st.saves == bt.saves && st.same == bt.same, "C89 ②t 开会话 %.0f ms 时已建够 3 条、等待 1500 ms 还没到：不应触发 → 触发 %llu 次、「没变」%llu 次", tc - ta, (unsigned long long)(st.saves - bt.saves), (unsigned long long)(st.same - bt.same));
            else printf("C89 ②t 本机太慢（开会话到建完 3 条用了 %.0f ms），「没到点不触发」这半条跳过\n", tc - ta);
            while (c89_now_ms() - tb < 1600) c89_sleep_ms(20);   // 从 open 返回算起也已过 1.5 s：后端那边一定到点了
            struct c89_st mt = c89_stats();
            CHECK(mt.saves == st.saves, "C89 ②t 光是时间到了不应触发（触发只在建管线时检查）→ 多触发 %llu 次", (unsigned long long)(mt.saves - st.saves));
            c89_more_pipes(sdir, 3);
            struct c89_st wt = c89_wait_done(&bt, 1, 5000);
            CHECK(wt.saves == bt.saves + 1 && wt.ok == bt.ok + 1, "C89 ②t 到点之后再建管线应触发并写成 1 次 → 触发 +%llu、成功 +%llu", (unsigned long long)(wt.saves - bt.saves), (unsigned long long)(wt.ok - bt.ok));
            CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ②t close：%d %s", e.rc.code, e.why);
        }
        c89_rm_tree(dt);
    }

    // ③ 旧文件是完整的（①留下的，数据 S1），同样 3 条管线、驱动数据仍是 S1 → 触发了但与上次一致：不写
    CHECK(c89_open(d1, 3, 0, 0), "C89 ③ open");
    struct c89_st b3 = c89_stats();
    c88_make_pipes(sdir);
    struct c89_st s3 = c89_stats();
    CHECK(s3.saves == b3.saves && s3.same == b3.same + 1, "C89 ③ 数据与上次一致：应触发检查 1 次但不写 → 写 +%llu 次、「没变」+%llu 次（期望 0 / 1）", (unsigned long long)(s3.saves - b3.saves), (unsigned long long)(s3.same - b3.same));
    c89_more_pipes(sdir, 3);   // 检查过一次（没变）之后基线就推进了：要再多 3 条才再查一次，不是每建一条就查（每次查都是一回取数据的调用）
    struct c89_st s3c = c89_stats();
    CHECK(s3c.saves == b3.saves && s3c.same == b3.same + 2, "C89 ③ 检查过之后再建 3 条：应再检查 1 次（共 2 次）而不是每条都查 → 写 +%llu 次、「没变」+%llu 次（期望 0 / 2）", (unsigned long long)(s3c.saves - b3.saves), (unsigned long long)(s3c.same - b3.same));
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ③ close：%d %s", e.rc.code, e.why);
    struct c89_dir a3 = c89_scan(d1);
    CHECK(c89_same_file(&a1, &a3) && a3.nother == 0, "C89 ③ 文件应原样（大小 / 修改时间 / 内容不变）");
    // ③ 对照：只把驱动数据变大（S2），其余同上 → 这回要写
    g_plc_extra = C89_X2;
    CHECK(c89_open(d1, 3, 0, 0), "C89 ③ 对照 open");
    struct c89_st b3b = c89_stats();
    c88_make_pipes(sdir);
    struct c89_st w3b = c89_wait_done(&b3b, 1, 5000);
    CHECK(w3b.saves == b3b.saves + 1 && w3b.ok == b3b.ok + 1 && w3b.same == b3b.same, "C89 ③ 对照（驱动数据变大）：应写 1 次 → 写 +%llu、成功 +%llu、「没变」+%llu", (unsigned long long)(w3b.saves - b3b.saves), (unsigned long long)(w3b.ok - b3b.ok), (unsigned long long)(w3b.same - b3b.same));
    struct c89_dir a3b = c89_scan(d1);
    CHECK(a3b.nreg == 1 && a3b.nother == 0 && a3b.size == a1.size + (C89_X2 - C89_X1), "C89 ③ 对照：文件应变成 48 + S2（比原来大 %d 字节）→ %ld → %ld 字节", C89_X2 - C89_X1, a1.size, a3b.size);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ③ 对照 close：%d %s", e.rc.code, e.why);
    g_plc_extra = C89_X1;

    // ④ 第二次保存：驱动数据变大后，间隔没到不再触发；间隔 0 再触发、文件变大
    char d4[600], d4b[600]; snprintf(d4, sizeof d4, "%s/c4a", root); snprintf(d4b, sizeof d4b, "%s/c4b", root);
    CHECK(mkdir(d4, 0700) == 0 && mkdir(d4b, 0700) == 0, "C89 建不出 c4");
    CHECK(c89_open(d4, 3, 0, 1000000), "C89 ④ 间隔没到 open");
    struct c89_st b4 = c89_stats();
    c88_make_pipes(sdir);
    struct c89_st w4 = c89_wait_done(&b4, 1, 5000);
    CHECK(w4.saves == b4.saves + 1 && w4.ok == b4.ok + 1, "C89 ④ 头一次应写成 1 次 → 触发 +%llu、成功 +%llu", (unsigned long long)(w4.saves - b4.saves), (unsigned long long)(w4.ok - b4.ok));
    g_plc_extra = C89_X2;   // 驱动数据变大了：只剩「间隔没到」一个理由不写
    c89_more_pipes(sdir, 3);
    struct c89_st s4 = c89_stats();
    CHECK(s4.saves == w4.saves && s4.same == w4.same, "C89 ④ 再建 3 条新管线、数据也变大了，但距上次保存的间隔没到 → 不应再触发（多触发 %llu 次、「没变」+%llu）", (unsigned long long)(s4.saves - w4.saves), (unsigned long long)(s4.same - w4.same));
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ④ 间隔没到 close：%d %s", e.rc.code, e.why);
    g_plc_extra = C89_X1;
    CHECK(c89_open(d4b, 3, 0, 0), "C89 ④ 间隔 0 open");
    struct c89_st b4b = c89_stats();
    c88_make_pipes(sdir);
    struct c89_st w4b = c89_wait_done(&b4b, 1, 5000);
    struct c89_dir a4 = c89_scan(d4b);
    g_plc_extra = C89_X2;
    c89_more_pipes(sdir, 3);
    struct c89_st s4b = c89_stats();
    CHECK(s4b.saves == w4b.saves + 1, "C89 ④ 间隔 0：再建 3 条新管线应再触发 1 次 → +%llu 次", (unsigned long long)(s4b.saves - w4b.saves));
    struct c89_st x4b = c89_wait_done(&w4b, 1, 5000);
    struct c89_dir a4b = c89_scan(d4b);
    CHECK(x4b.ok == w4b.ok + 1 && a4b.nreg == 1 && a4b.size > a4.size, "C89 ④ 第二次保存应成功且文件变大 → 成功 +%llu、%ld → %ld 字节", (unsigned long long)(x4b.ok - w4b.ok), a4.size, a4b.size);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ④ 间隔 0 close：%d %s", e.rc.code, e.why);
    g_plc_extra = C89_X1;
    c89_rm_tree(d4); c89_rm_tree(d4b);

    // ⑤ 写失败只记日志：目录在会话中途被换成普通文件
    char d5[600]; snprintf(d5, sizeof d5, "%s/c5", root); CHECK(mkdir(d5, 0700) == 0, "C89 建不出 c5");
    CHECK(c89_open(d5, 3, 0, 0), "C89 ⑤ open");
    struct c89_st b5 = c89_stats();
    CHECK(rmdir(d5) == 0 && c89_touch(d5, "x", 1), "C89 ⑤ 换不掉目录");
    c88_make_pipes(sdir);
    struct c89_st w5 = c89_wait_done(&b5, 1, 5000);
    CHECK(w5.saves == b5.saves + 1 && w5.failed == b5.failed + 1 && w5.ok == b5.ok, "C89 ⑤ 后台写应失败 1 次 → 触发 +%llu、失败 +%llu、成功 +%llu", (unsigned long long)(w5.saves - b5.saves), (unsigned long long)(w5.failed - b5.failed), (unsigned long long)(w5.ok - b5.ok));
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ⑤ 写失败后会话照常 close 返回 OK：%d %s", e.rc.code, e.why);
    unlink(d5);

    // ⑤ 续：失败只是这一次。临时文件路径被一个目录占着 → 局中写失败；放开后 close 要把没写成的补上。
    char d5b[600], d5s[600]; snprintf(d5b, sizeof d5b, "%s/c5b", root); snprintf(d5s, sizeof d5s, "%s/c5s", root);
    CHECK(mkdir(d5b, 0700) == 0 && mkdir(d5s, 0700) == 0, "C89 建不出 c5b");
    CHECK(c89_seed(sdir, d5s), "C89 ⑤ 续 铺旧文件（只为拿到合规的文件名）");
    struct c89_dir s5 = c89_scan(d5s);
    char blk5[1000]; snprintf(blk5, sizeof blk5, "%s/%s.tmp.%ld", d5b, s5.name, (long)getpid());
    CHECK(mkdir(blk5, 0700) == 0, "C89 ⑤ 续 占不了临时文件路径");
    CHECK(c89_open(d5b, 3, 0, 0), "C89 ⑤ 续 open");
    struct c89_st b5b = c89_stats();
    c88_make_pipes(sdir);
    struct c89_st w5b = c89_wait_done(&b5b, 1, 5000);
    CHECK(w5b.saves == b5b.saves + 1 && w5b.failed == b5b.failed + 1 && w5b.ok == b5b.ok, "C89 ⑤ 续 路径被占：局中写应失败 1 次 → 触发 +%llu、失败 +%llu、成功 +%llu", (unsigned long long)(w5b.saves - b5b.saves), (unsigned long long)(w5b.failed - b5b.failed), (unsigned long long)(w5b.ok - b5b.ok));
    struct c89_dir m5 = c89_scan(d5b);
    CHECK(m5.nreg == 0, "C89 ⑤ 续 局中写失败后目录里不应有正式文件 → %d 个", m5.nreg);
    CHECK(rmdir(blk5) == 0, "C89 ⑤ 续 放不开临时文件路径");
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ⑤ 续 close：%d %s", e.rc.code, e.why);
    struct c89_dir f5 = c89_scan(d5b);
    CHECK(f5.nreg == 1 && c88_name_ok(f5.name) && f5.nother == 0 && f5.size == (long)mid1 + C89_HDR,
          "C89 ⑤ 续 局中没写成，close 应补上（数据大小与「上次保存」一致也要再写）→ 文件 %d、其它 %d、%ld 字节（期望 %zu）", f5.nreg, f5.nother, f5.size, mid1 + C89_HDR);
    c89_rm_tree(d5b); c89_rm_tree(d5s);

    // ⑪ 局中写盘也要补目录（真机头一局：<状态目录>/direct-cache/nvn-vk 谁都没建过）：目录和它的上一层都还不存在，写完（还没 close）目录里就该有 1 个合规文件
    char d11p[600], d11[700]; snprintf(d11p, sizeof d11p, "%s/c11", root); snprintf(d11, sizeof d11, "%s/nvn-vk", d11p);
    CHECK(c89_open(d11, 3, 0, 0), "C89 ⑪ open");
    struct c89_st b11 = c89_stats();
    c88_make_pipes(sdir);
    struct c89_st w11 = c89_wait_done(&b11, 1, 5000);
    CHECK(w11.ok == b11.ok + 1 && w11.failed == b11.failed, "C89 ⑪ 目录还不存在时局中写应成功 1 次（后台线程补目录）→ 成功 +%llu、失败 +%llu", (unsigned long long)(w11.ok - b11.ok), (unsigned long long)(w11.failed - b11.failed));
    struct c89_dir a11 = c89_scan(d11);
    CHECK(a11.nreg == 1 && c88_name_ok(a11.name) && a11.nother == 0, "C89 ⑪ 局中写完（还没 close）补出的目录里应有 1 个合规文件 → 文件 %d、其它 %d", a11.nreg, a11.nother);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ⑪ close：%d %s", e.rc.code, e.why);
    c89_rm_tree(d11); rmdir(d11p);

    // ⑥ 后台写途中收局：临时文件路径被占成管道，后台写卡住；close(200 ms) 到期放弃、不挂住
    char d6[600]; snprintf(d6, sizeof d6, "%s/c6", root); CHECK(mkdir(d6, 0700) == 0, "C89 建不出 c6");
    CHECK(c89_seed(sdir, d6), "C89 ⑥ 铺旧文件");
    struct c89_dir a6 = c89_scan(d6);
    CHECK(a6.nreg == 1 && a6.nother == 0 && c88_name_ok(a6.name), "C89 ⑥ 旧文件应恰好 1 个合规文件 → %d 个、其它 %d", a6.nreg, a6.nother);
    char f6[900], pre6[900]; snprintf(f6, sizeof f6, "%s/%s", d6, a6.name);
    size_t n6a = 0; void *old6 = read_file(f6, &n6a);
    snprintf(pre6, sizeof pre6, "%s/%s.tmp.%ld", d6, a6.name, (long)getpid());
    CHECK(mkfifo(pre6, 0600) == 0, "C89 ⑥ 建不出管道");
    g_plc_extra = C89_X2;   // 这一局驱动数据比旧文件里的大 → 后端才会真去写
    CHECK(c89_open(d6, 3, 0, 0), "C89 ⑥ open");
    struct c89_st b6 = c89_stats();
    CHECK(b6.loaded == (uint64_t)(a6.size - C89_HDR) && b6.rejected == 0, "C89 ⑥ 开局应读到旧文件 %ld 字节、rejected=0 → %llu / %llu", a6.size - C89_HDR, (unsigned long long)b6.loaded, (unsigned long long)b6.rejected);
    c88_make_pipes(sdir);   // blue + compute 是新的：数据变大，第 3 条建完触发，后台写卡在管道上
    struct c89_st s6 = c89_stats();
    CHECK(s6.saves == b6.saves + 1, "C89 ⑥ 新管线让数据变大：应触发 1 次后台写 → +%llu", (unsigned long long)(s6.saves - b6.saves));
    c89_sleep_ms(100);
    struct c89_st m6 = c89_stats();
    CHECK(m6.ok + m6.failed == b6.ok + b6.failed, "C89 ⑥ 管道没人读时后台写应还卡着（没成功也没失败）→ 成功 +%llu、失败 +%llu", (unsigned long long)(m6.ok - b6.ok), (unsigned long long)(m6.failed - b6.failed));
    g_plc_extra = C89_X3;   // 写还卡着时数据又变大了：再建 3 条新管线也不该再起新的写（同一时刻最多一次、不排队）
    c89_more_pipes(sdir, 3);
    struct c89_st t6 = c89_stats();
    CHECK(t6.saves == s6.saves && t6.same == s6.same, "C89 ⑥ 上一次写还没收尾时不应再起新的写 → 又触发 %llu 次、「没变」+%llu", (unsigned long long)(t6.saves - s6.saves), (unsigned long long)(t6.same - s6.same));
    g_plc_extra = C89_X2;
    struct c89_unblock ub; ub.fifo = pre6; atomic_init(&ub.go, 0);
    pthread_t ubt; int ubt_ok = pthread_create(&ubt, NULL, c89_unblock_main, &ub) == 0;
    CHECK(ubt_ok, "C89 ⑥ 起不了放管道的线程");
    double t0 = c89_now_ms();
    nvn_gpu_rc rc6 = nvn_gpu_close(200000000ull, &e);
    double ms6 = c89_now_ms() - t0;
    CHECK(OK(rc6), "C89 ⑥ 后台写卡着时 close(200 ms) 应返回 OK（到期放弃这次写，不报超时）：%d %s", rc6.code, e.why);
    CHECK(ms6 >= 150.0 && ms6 < 3000.0, "C89 ⑥ close 应等了后台写（算进 200 ms 截止时间）又没挂住 → 用了 %.0f ms（期望 150–3000）", ms6);
    atomic_store(&ub.go, 1);
    struct c89_dir x6 = c89_scan(d6);
    CHECK(c89_same_file(&a6, &x6), "C89 ⑥ 放弃这次写后旧文件应原样（大小 / 修改时间 / 内容不变）");
    if (ubt_ok) pthread_join(ubt, NULL);
    CHECK(c89_wait_gone(pre6, 5000), "C89 ⑥ 放开管道后后台写应自己失败收尾、清掉临时文件");
    size_t n6b = 0; void *now6 = read_file(f6, &n6b);
    CHECK(old6 && now6 && n6a == n6b && !memcmp(old6, now6, n6a), "C89 ⑥ 旧文件应逐字节不变");
    free(old6); free(now6);
    g_plc_extra = C89_X1;
    CHECK(c89_open(d6, -1, -1, -1), "C89 ⑥ 下一局 open");
    struct c89_st r6 = c89_wait_done(&b6, 1, 5000);   // 读数是进程累计：顺带等放弃的那次写自己收尾
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ⑥ 下一局 close：%d %s", e.rc.code, e.why);
    CHECK(r6.loaded == (uint64_t)(a6.size - C89_HDR) && r6.rejected == 0, "C89 ⑥ 下一局读回旧文件 %ld 字节、rejected=0 → %llu / %llu", a6.size - C89_HDR, (unsigned long long)r6.loaded, (unsigned long long)r6.rejected);
    CHECK(r6.failed == b6.failed + 1 && r6.ok == b6.ok, "C89 ⑥ 放弃的那次写自己收尾后应记 1 次失败、0 次成功（管道上 fsync 不成）→ 失败 +%llu、成功 +%llu", (unsigned long long)(r6.failed - b6.failed), (unsigned long long)(r6.ok - b6.ok));
    c89_rm_tree(d6);

    // ⑦ 被杀（不经关局）：子进程局中写完后自己 SIGKILL
    char d7[600], g7[700]; snprintf(d7, sizeof d7, "%s/c7", root); snprintf(g7, sizeof g7, "%s/go7", root);
    CHECK(mkdir(d7, 0700) == 0, "C89 建不出 c7");
    pid_t p7 = c89_spawn_child(sdir, d7, g7, "kill-after-ok");
    CHECK(p7 > 0, "C89 ⑦ 起不了子进程");
    if (p7 > 0) {
        CHECK(c89_touch(g7, NULL, 0), "C89 ⑦ 摆不出开工信号");
        int st7 = c89_reap(p7);
        CHECK(st7 != -1 && WIFSIGNALED(st7) && WTERMSIG(st7) == SIGKILL, "C89 ⑦ 子进程应是被 SIGKILL 杀的（没有走 close）→ 状态 %#x", st7);
        struct c89_dir a7 = c89_scan(d7);
        CHECK(a7.nreg == 1 && a7.nother == 0 && c88_name_ok(a7.name) && a7.size > C89_HDR, "C89 ⑦ 被杀后目录里应已有恰好 1 个合规文件、没有临时文件 → 文件 %d、其它 %d、%ld 字节", a7.nreg, a7.nother, a7.size);
        struct c89_st r7 = c89_readback(d7);
        CHECK(a7.nreg == 1 && r7.loaded == (uint64_t)(a7.size - C89_HDR) && r7.rejected == 0, "C89 ⑦ 下一局应读回被杀那局写的 %ld 字节、rejected=0 → %llu / %llu", a7.size - C89_HDR, (unsigned long long)r7.loaded, (unsigned long long)r7.rejected);
    }
    c89_rm_tree(d7); unlink(g7);

    // ⑧ 被杀在写盘途中：后台写卡在管道上时子进程被 SIGKILL
    char d8[600], g8[700]; snprintf(d8, sizeof d8, "%s/c8", root); snprintf(g8, sizeof g8, "%s/go8", root);
    CHECK(mkdir(d8, 0700) == 0, "C89 建不出 c8");
    CHECK(c89_seed(sdir, d8), "C89 ⑧ 铺旧文件");
    struct c89_dir a8 = c89_scan(d8);
    char f8[900], pre8[1000]; snprintf(f8, sizeof f8, "%s/%s", d8, a8.name);
    size_t n8a = 0; void *old8 = read_file(f8, &n8a);
    pid_t p8 = c89_spawn_child(sdir, d8, g8, "kill-after-start");
    CHECK(p8 > 0, "C89 ⑧ 起不了子进程");
    if (p8 > 0) {
        snprintf(pre8, sizeof pre8, "%s/%s.tmp.%ld", d8, a8.name, (long)p8);
        CHECK(mkfifo(pre8, 0600) == 0, "C89 ⑧ 建不出管道");
        CHECK(c89_touch(g8, NULL, 0), "C89 ⑧ 摆不出开工信号");
        int st8 = c89_reap(p8);
        CHECK(st8 != -1 && WIFSIGNALED(st8) && WTERMSIG(st8) == SIGKILL, "C89 ⑧ 子进程应是在后台写途中被 SIGKILL 杀的 → 状态 %#x", st8);
        struct c89_dir x8 = c89_scan(d8);
        CHECK(c89_same_file(&a8, &x8) && x8.nother == 1, "C89 ⑧ 被杀后旧文件应原样、只多一个残留的临时文件 → 其它 %d 个", x8.nother);
        size_t n8b = 0; void *now8 = read_file(f8, &n8b);
        CHECK(old8 && now8 && n8a == n8b && !memcmp(old8, now8, n8a), "C89 ⑧ 旧文件应逐字节不变");
        free(now8);
        struct c89_st r8 = c89_readback(d8);
        CHECK(r8.loaded == (uint64_t)(a8.size - C89_HDR) && r8.rejected == 0, "C89 ⑧ 下一局照常开、读回旧文件 %ld 字节、rejected=0 → %llu / %llu", a8.size - C89_HDR, (unsigned long long)r8.loaded, (unsigned long long)r8.rejected);
        unlink(pre8);
    }
    free(old8);
    c89_rm_tree(d8); unlink(g8);

    // ⑨ 临时文件残留：垃圾在 <名>.tmp.<pid> 里，下一局不读它
    char d9[600], d9b[600], d9c[600]; snprintf(d9, sizeof d9, "%s/c9a", root); snprintf(d9b, sizeof d9b, "%s/c9b", root); snprintf(d9c, sizeof d9c, "%s/c9c", root);
    CHECK(mkdir(d9, 0700) == 0 && mkdir(d9b, 0700) == 0 && mkdir(d9c, 0700) == 0, "C89 建不出 c9");
    CHECK(c89_seed(sdir, d9b), "C89 ⑨ 铺旧文件");
    struct c89_dir a9b = c89_scan(d9b);
    static unsigned char junk[4096]; memset(junk, 0xA5, sizeof junk);
    char t9[1000], t9b[1000], t9c[1000];
    snprintf(t9, sizeof t9, "%s/%s.tmp.99999", d9, a9b.name);
    snprintf(t9b, sizeof t9b, "%s/%s.tmp.99999", d9b, a9b.name);
    snprintf(t9c, sizeof t9c, "%s/%s", d9c, a9b.name);
    CHECK(c89_touch(t9, junk, sizeof junk) && c89_touch(t9b, junk, sizeof junk) && c89_touch(t9c, junk, sizeof junk), "C89 ⑨ 摆不出残留文件");
    CHECK(c89_open(d9, -1, -1, -1), "C89 ⑨ 无正式文件 open");   // 只有残留
    struct c89_st s9 = c89_stats();
    CHECK(s9.loaded == 0 && s9.rejected == 0, "C89 ⑨ 只有残留的临时文件：读入应为 0、rejected=0（残留不当坏文件）→ %llu / %llu", (unsigned long long)s9.loaded, (unsigned long long)s9.rejected);
    c88_make_pipes(sdir);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ⑨ close：%d %s", e.rc.code, e.why);
    struct c89_dir x9 = c89_scan(d9);
    CHECK(x9.nreg == 1 && c88_name_ok(x9.name) && x9.nother == 1, "C89 ⑨ 收尾应照常写出正式文件、残留原样 → 正式 %d 个、其它 %d 个", x9.nreg, x9.nother);
    { size_t nj = 0; void *j = read_file(t9, &nj); CHECK(j && nj == sizeof junk && !memcmp(j, junk, nj), "C89 ⑨ 残留文件内容不应被动"); free(j); }
    struct c89_st s9b = c89_readback(d9b);   // 有正式文件、旁边有残留
    CHECK(s9b.loaded == (uint64_t)(a9b.size - C89_HDR) && s9b.rejected == 0, "C89 ⑨ 正式文件旁有残留：应读回正式文件 %ld 字节、rejected=0 → %llu / %llu", a9b.size - C89_HDR, (unsigned long long)s9b.loaded, (unsigned long long)s9b.rejected);
    struct c89_st s9c = c89_readback(d9c);   // 负对照：同样的垃圾放在正式文件名上
    CHECK(s9c.loaded == 0 && s9c.rejected == 1, "C89 ⑨ 负对照：垃圾放在正式文件名上应被拒绝 → 读入 %llu、rejected=%llu（期望 0 / 1）", (unsigned long long)s9c.loaded, (unsigned long long)s9c.rejected);
    c89_rm_tree(d9); c89_rm_tree(d9b); c89_rm_tree(d9c);

    // ⑩ 两个线程同时建管线，局中写盘与建管线并行
    char d10[600]; snprintf(d10, sizeof d10, "%s/c10", root); CHECK(mkdir(d10, 0700) == 0, "C89 建不出 c10");
    g_plc_extra = 0; g_plc_per_pipe = 16;   // 驱动数据随管线数变大（每条 +16 字节）→ 每次触发都比上次大，写盘一直在发生
    CHECK(c89_open(d10, 2, 0, 0), "C89 ⑩ open");
    // 人工大小只随这两线程要建的 84 条游戏管线变化，不计 open 期间的内部管线。
    g_plc_pipe_base = g_plc_rec.gp + g_plc_rec.cp;
    struct c89_st b10 = c89_stats();
    nvn_gpu_shader vsa = NULL, fsa = NULL, vsb = NULL, fsb = NULL; void *ma[4] = {0};
    int l10 = load_shader_pair(sdir, "tri3.vert", "out1_green.frag", NULL, NULL, &vsa, &fsa, &ma[0], &ma[1]) &&
              load_shader_pair(sdir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vsb, &fsb, &ma[2], &ma[3]);
    CHECK(l10, "C89 ⑩ 读着色器");
    if (l10) {
        struct c89_thr ta = {vsa, fsa}, tb = {vsb, fsb};   // 两个线程用不同的片元着色器 → 84 条互不相同的新管线
        pthread_t th[2];
        int cr = pthread_create(&th[0], NULL, c89_thr_main, &ta) == 0 && pthread_create(&th[1], NULL, c89_thr_main, &tb) == 0;
        CHECK(cr, "C89 ⑩ 起不了线程");
        if (cr) { pthread_join(th[0], NULL); pthread_join(th[1], NULL); }
        struct c89_st e10 = c89_wait_done(&b10, 1, 5000);
        for (int i = 0; i < 2500; i++) {   // 等在飞的写收尾：成功 + 失败 = 触发
            struct c89_st z = c89_stats();
            if (z.ok + z.failed - (b10.ok + b10.failed) >= z.saves - b10.saves) { e10 = z; break; }
            c89_sleep_ms(2);
        }
        CHECK(e10.made - b10.made == 84, "C89 ⑩ 两个线程应经缓存建出 84 条管线 → %llu 条", (unsigned long long)(e10.made - b10.made));
        // 触发次数只断言 ≥ 1：后台写在飞时新的触发直接跳过、不排队（plc_save_midrun_impl），次数取决于写盘耗时与两线程建管线的快慢，
        //   慢的 CI 机上只有 1–2 次（main 与 #1561 / #1559 都偶发红过 ≥ 3）。≥ 1 是确定的：阈值每 2 条、首次不等时间，第 2 条建好就开写，
        //   这时两个线程还要建满 84 条——要证的「并行建管线时局中写盘、全部写成、收尾文件合规」都还在。
        CHECK(e10.saves - b10.saves >= 1 && e10.failed == b10.failed && e10.ok - b10.ok == e10.saves - b10.saves,
              "C89 ⑩ 并行建管线时局中写盘应发生且全部成功 → 触发 %llu 次（期望 ≥ 1）、成功 +%llu、失败 +%llu", (unsigned long long)(e10.saves - b10.saves), (unsigned long long)(e10.ok - b10.ok), (unsigned long long)(e10.failed - b10.failed));
        nvn_gpu_shader_release(vsa); nvn_gpu_shader_release(fsa); nvn_gpu_shader_release(vsb); nvn_gpu_shader_release(fsb);
        for (int i = 0; i < 4; i++) free(ma[i]);
    }
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C89 ⑩ close：%d %s", e.rc.code, e.why);
    g_plc_per_pipe = 0; g_plc_extra = C89_X1;
    struct c89_dir a10 = c89_scan(d10);
    CHECK(a10.nreg == 1 && a10.nother == 0 && c88_name_ok(a10.name), "C89 ⑩ 目录里应恰好 1 个合规文件、没有残留 → 文件 %d、其它 %d", a10.nreg, a10.nother);
    // 收尾那份是建完 84 条之后的数据：= 文件头 + lavapipe 真数据（①里测到的 mid1 去掉填充）+ 84 × 16；中途哪次写成功都不该把它盖回旧的
    long want10 = C89_HDR + (long)(mid1 - C89_X1) + 84 * 16;
    CHECK(a10.size == want10, "C89 ⑩ 收尾应把建完 84 条之后的数据落盘 → %ld 字节（期望 %ld）", a10.size, want10);
    struct c89_st r10 = c89_readback(d10);
    CHECK(a10.nreg == 1 && r10.loaded == (uint64_t)(a10.size - C89_HDR) && r10.rejected == 0, "C89 ⑩ 下一局应读回、rejected=0 → 读入 %llu、rejected=%llu", (unsigned long long)r10.loaded, (unsigned long long)r10.rejected);
    printf("C89 局中补写：局中写出 %zu 字节；close 不重写；条数 / 等待 / 开关 / 无目录四个负对照不触发；数据没变不写；第二次保存受间隔约束；写失败只记日志、close 补写；"
           "目录不存在时局中写补目录；后台写途中 close(200 ms) 用 %.0f ms 到期放弃；SIGKILL（写完后 / 写途中）都不坏下一局；临时文件残留不读；两线程并行建管线写盘全成功\n", mid1, ms6);
    c89_rm_tree(d1); c89_rm_tree(d10);
    rmdir(root);
    g_plc_strip_tail = 0; g_plc_extra = 0; g_plc_per_pipe = 0;
}

// #697：强制 BC4 compute；端点与索引全由合成输入给出，不用解码器计算期望。
// 7x5x4 的每个块/切片不同，24 字节行距包含填充；最后一行不要求尾填充。
static void c697_bc4_done(void *u, const nvn_gpu_done *d) { (void)u; (void)d; }
static void c697_bc4_block(uint8_t *p, uint8_t value) {
    memset(p, 0, 8); p[0] = p[1] = value;
}
static void c697_bc4_snapshot(nvn_gpu_cmd c, nvn_gpu_tex bc, nvn_gpu_tex plain,
                             nvn_gpu_buf rb, uint32_t level, uint64_t offset,
                             uint32_t w, uint32_t h, uint32_t d) {
    NVNcopyRegion region = {0, 0, 0, (int)w, (int)h, (int)d};
    nvn_gpu_copy_tex(c, bc, level, 0, &region, plain, level, 0, &region);
    nvn_gpu_copy_tex_buf(c, plain, level, 0, &region, rb, offset, w, (uint64_t)w * h * d);
}
static void c697_bc4_3d(void) {
    nvn_gpu_tex bc = make_tex_3d_levels(7, 5, 4, 3, NVN_FORMAT_RGTC1_UNORM,
                                      NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex plain = NULL; nvn_gpu_buf rb = NULL;
    if (!bc) return; // 基线必须红在真实 tex_create，不继续使用空句柄。
    plain = make_tex_3d_levels(7, 5, 4, 3, NVN_FORMAT_R8, NVN_GPU_TEX_SAMPLED);
    rb = make_buf(436, NVN_GPU_MEM_SHARED, NULL);
    if (!plain || !rb) goto done;
    uint8_t blocks[192], patch[64], mip[24], last[8], mip_patch[8];
    memset(blocks, 0xee, sizeof blocks); memset(patch, 0xcc, sizeof patch); memset(mip, 0xdd, sizeof mip);
    for (int z = 0; z < 4; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++)
        c697_bc4_block(blocks + z * 48 + y * 24 + x * 8, (uint8_t)(20 + z * 40 + y * 10 + x * 5));
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++)
        c697_bc4_block(patch + z * 32 + y * 16, (uint8_t)(211 + z * 10 + y * 3));
    c697_bc4_block(mip, 171); c697_bc4_block(mip + 16, 181); c697_bc4_block(last, 251); c697_bc4_block(mip_patch, 191);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e;
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blocks, 24, 184);
    c697_bc4_snapshot(c, bc, plain, rb, 0, 0, 7, 5, 4);
    NVNcopyRegion region = {4, 0, 1, 3, 5, 2};
    nvn_gpu_tex_upload(c, bc, 0, 0, &region, patch, 16, 56);
    c697_bc4_snapshot(c, bc, plain, rb, 0, 140, 7, 5, 4);
    nvn_gpu_tex_upload(c, bc, 1, 0, NULL, mip, 16, sizeof mip);
    NVNcopyRegion mip_region = {0, 0, 1, 3, 2, 1};
    nvn_gpu_tex_upload(c, bc, 1, 0, &mip_region, mip_patch, 8, sizeof mip_patch);
    nvn_gpu_tex_upload(c, bc, 2, 0, NULL, last, 8, sizeof last);
    c697_bc4_snapshot(c, bc, plain, rb, 1, 420, 3, 2, 2);
    c697_bc4_snapshot(c, bc, plain, rb, 2, 432, 1, 1, 1);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, c697_bc4_done, NULL, &e);
    CHECK(OK(rc), "C697 BC4 3D commit：%d %s", rc.code, e.why);
    if (!OK(rc)) goto done;
    rc = nvn_gpu_fence_wait(1, 5000000000ull, &e);
    CHECK(OK(rc), "C697 BC4 3D wait：%s", e.why); if (!OK(rc)) goto done;
    // 后片分配失败：前面切片的解码与目标图拷贝都还在待提交段中，abort 不得改旧内容。
    for (int fail_at = 2; fail_at <= 3; fail_at++) {
        c = begin_cmd(); g_bc4_temp_seen = 0; g_bc4_fail_temp = fail_at;
        nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blocks, 24, 184);
        g_bc4_fail_temp = 0;
        CHECK(g_bc4_temp_seen == fail_at, "C697 BC4 第%d片 OOM 未命中（%d）", fail_at, g_bc4_temp_seen);
        nvn_gpu_cmd_abort(c);
    }
    c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blocks, 24, 184);
    nvn_gpu_cmd_abort(c);
    c = begin_cmd();
    c697_bc4_snapshot(c, bc, plain, rb, 0, 280, 7, 5, 4);
    rc = nvn_gpu_cmd_commit(c, 2, c697_bc4_done, NULL, &e);
    CHECK(OK(rc), "C697 BC4 3D abort 后 commit：%d %s", rc.code, e.why);
    if (!OK(rc)) goto done;
    rc = nvn_gpu_fence_wait(2, 5000000000ull, &e);
    CHECK(OK(rc), "C697 BC4 3D abort 后 wait：%s", e.why); if (!OK(rc)) goto done;
    const uint8_t *out = buf_cpu(rb);
    for (int image = 0; image < 3; image++) for (int z = 0; z < 4; z++) for (int y = 0; y < 5; y++) for (int x = 0; x < 7; x++) {
        uint8_t expected = (uint8_t)(20 + z * 40 + (y / 4) * 10 + (x / 4) * 5);
        if (image && z >= 1 && z <= 2 && x >= 4) expected = (uint8_t)(211 + (z - 1) * 10 + (y / 4) * 3);
        uint8_t actual = out[image * 140 + (z * 5 + y) * 7 + x];
        CHECK(actual == expected, "C697 BC4 3D 快照%d (%d,%d,%d)：%u，期望%u", image, x, y, z, actual, expected);
    }
    for (int i = 0; i < 12; i++) CHECK(out[420 + i] == (i < 6 ? 171 : 191), "C697 BC4 mip1 像素%d：%u", i, out[420 + i]);
    CHECK(out[432] == 251, "C697 BC4 mip2 1x1x1：%u", out[432]);
    printf("C697 BC4 3D：三份全深度快照420纹素、mip1 12纹素、mip2 1纹素已核对\n");
    {   // 按压缩字节回读（贴图 → 缓冲）：Mali 等非原生 BC 设备上游戏会做这种拷贝，显存里是解码后的 R8，回读要给上传时的原字节。
        // 期望值按上传的原块手工拼（c697_bc4_block：前两字节是端点、其余 0）；当前内容 = 第 1 次提交的结果（之后的整片重传都被放弃）。
        uint8_t init[160]; memset(init, 0x5a, sizeof init);
        nvn_gpu_buf rb2 = make_buf(sizeof init, NVN_GPU_MEM_SHARED, init);
        if (!rb2) goto done;
        c = begin_cmd();
        nvn_gpu_copy_tex_buf(c, bc, 0, 1, NULL, rb2, 0, 16, 32);                 // ① 0 级切片 1 整片：列 0 原块，列 1 是补丁
        nvn_gpu_copy_tex_buf(c, bc, 0, 3, NULL, rb2, 64, 24, 40);                // ② 0 级切片 3，行距 24：每行后 8 字节不许动
        nvn_gpu_copy_tex_buf(c, bc, 1, 1, NULL, rb2, 112, 8, 8);                 // ③ 1 级切片 1：局部补丁盖过的那块
        NVNcopyRegion col1 = {4, 0, 0, 3, 5, 1};
        nvn_gpu_copy_tex_buf(c, bc, 0, 2, &col1, rb2, 128, 8, 16);               // ④ 0 级切片 2 只读块列 1
        rc = nvn_gpu_cmd_commit(c, 3, c697_bc4_done, NULL, &e);
        CHECK(OK(rc), "C697 BC4 回读 commit：%d %s", rc.code, e.why);
        if (OK(rc)) rc = nvn_gpu_fence_wait(3, 5000000000ull, &e);
        CHECK(OK(rc), "C697 BC4 回读 wait：%s", e.why);
        const uint8_t *rd = buf_cpu(rb2);
        uint8_t want[160]; memcpy(want, init, sizeof want);
        for (int y = 0; y < 2; y++) { c697_bc4_block(want + y * 16, (uint8_t)(20 + 40 + y * 10)); c697_bc4_block(want + y * 16 + 8, (uint8_t)(211 + y * 3)); }
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) c697_bc4_block(want + 64 + y * 24 + x * 8, (uint8_t)(20 + 3 * 40 + y * 10 + x * 5));
        c697_bc4_block(want + 112, 191);
        for (int y = 0; y < 2; y++) c697_bc4_block(want + 128 + y * 8, (uint8_t)(211 + 10 + y * 3));
        int bad = 0; for (int i = 0; i < 160; i++) if (rd[i] != want[i]) { if (bad++ < 4) CHECK(0, "C697 BC4 回读字节 %d：%u，期望 %u", i, rd[i], want[i]); }
        CHECK(!bad, "C697 BC4 回读：%d 字节不符", bad);
        if (!bad) printf("C697 BC4 回读：整片 / 带行距 / 下一级局部补丁 / 局部区域 四种按原压缩字节读回，160 字节已核对\n");
        nvn_gpu_buf_release(rb2, NULL, NULL);
    }
    {   // 缓冲 → BC 现解贴图（#697 第二步，Mali 机上回读之后撞到的下一处）：同一段里先把 bc 的 0 级切片 1 回读进缓冲，
        // 紧接着把这块缓冲拷进另一张 16x8 的 BC4 贴图左半；右半的源是 CPU 填好的块、行距 24（每行后 8 字节是填充）。
        // 期望全部手算：两端点相同的 BC4 块解出来整块就是端点值（c697_bc4_block）。左半 = bc 切片 1 的块值，见上面回读期望。
        nvn_gpu_tex bc2 = make_tex(16, 8, NVN_FORMAT_RGTC1_UNORM, NVN_GPU_TEX_SAMPLED);
        nvn_gpu_tex pl2 = make_tex(16, 8, NVN_FORMAT_R8, NVN_GPU_TEX_SAMPLED);
        uint8_t src[96]; memset(src, 0xee, sizeof src);
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) c697_bc4_block(src + 48 + y * 24 + x * 8, (uint8_t)(100 + (y * 2 + x) * 7));
        nvn_gpu_buf sb = make_buf(sizeof src, NVN_GPU_MEM_SHARED, src);
        nvn_gpu_buf pix = make_buf(128, NVN_GPU_MEM_SHARED, NULL), cmp = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
        if (!bc2 || !pl2 || !sb || !pix || !cmp) { CHECK(0, "C697 缓冲→BC 建资源失败"); goto c697b_done; }
        c = begin_cmd();
        nvn_gpu_copy_tex_buf(c, bc, 0, 1, NULL, sb, 0, 16, 32);                   // 回读：sb[0,32) 被 GPU 写成 bc 切片 1 的原块
        NVNcopyRegion left = {0, 0, 0, 8, 8, 1}, right = {8, 0, 0, 8, 8, 1}, all = {0, 0, 0, 16, 8, 1};
        nvn_gpu_copy_buf_tex(c, sb, 0, 16, 32, bc2, 0, 0, &left);                 // 同一段里刚写的字节 → BC 贴图
        nvn_gpu_copy_buf_tex(c, sb, 48, 24, 40, bc2, 0, 0, &right);               // CPU 填的块，带行距
        nvn_gpu_copy_tex(c, bc2, 0, 0, &all, pl2, 0, 0, &all);                    // 解出来的像素
        nvn_gpu_copy_tex_buf(c, pl2, 0, 0, &all, pix, 0, 16, 128);
        nvn_gpu_copy_tex_buf(c, bc2, 0, 0, NULL, cmp, 0, 32, 64);                 // 再按压缩字节读回
        rc = nvn_gpu_cmd_commit(c, 4, c697_bc4_done, NULL, &e);
        CHECK(OK(rc), "C697 缓冲→BC commit：%d %s", rc.code, e.why);
        if (OK(rc)) rc = nvn_gpu_fence_wait(4, 5000000000ull, &e);
        CHECK(OK(rc), "C697 缓冲→BC wait：%s", e.why);
        if (OK(rc)) {
            const uint8_t *px = buf_cpu(pix), *cb = buf_cpu(cmp);
            int bad = 0;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) {
                int by = y / 4;
                uint8_t want = x < 4 ? (uint8_t)(60 + by * 10) : x < 8 ? (uint8_t)(211 + by * 3) : (uint8_t)(100 + (by * 2 + (x - 8) / 4) * 7);
                if (px[y * 16 + x] != want && bad++ < 4) CHECK(0, "C697 缓冲→BC 像素 (%d,%d)：%u，期望 %u", x, y, px[y * 16 + x], want);
            }
            uint8_t wantc[64];
            for (int by = 0; by < 2; by++) {
                c697_bc4_block(wantc + by * 32 + 0, (uint8_t)(60 + by * 10)); c697_bc4_block(wantc + by * 32 + 8, (uint8_t)(211 + by * 3));
                c697_bc4_block(wantc + by * 32 + 16, (uint8_t)(100 + by * 14)); c697_bc4_block(wantc + by * 32 + 24, (uint8_t)(107 + by * 14));
            }
            for (int i = 0; i < 64; i++) if (cb[i] != wantc[i] && bad++ < 8) CHECK(0, "C697 缓冲→BC 压缩回读字节 %d：%u，期望 %u", i, cb[i], wantc[i]);
            CHECK(!bad, "C697 缓冲→BC：%d 处不符", bad);
            if (!bad) printf("C697 缓冲→BC：同段回读字节与带行距 CPU 块拷入现解贴图，128 像素与 64 压缩字节已核对\n");
        }
    c697b_done:
        if (bc2) nvn_gpu_tex_release(bc2);
        if (pl2) nvn_gpu_tex_release(pl2);
        if (sb) nvn_gpu_buf_release(sb, NULL, NULL);
        if (pix) nvn_gpu_buf_release(pix, NULL, NULL);
        if (cmp) nvn_gpu_buf_release(cmp, NULL, NULL);
    }
done:
    if (bc) nvn_gpu_tex_release(bc);
    if (plain) nvn_gpu_tex_release(plain);
    if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
}
// 每个拒绝用例独立会话，不让上一例的 sticky 错误冒充本例命中。
static void c697_bc4_3d_bad(int which) {
    nvn_gpu_tex bc = make_tex_3d_levels(7, 5, 4, 3, NVN_FORMAT_RGTC1_UNORM, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    if (!bc) return;
    uint8_t blocks[192] = {0}; nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e;
    NVNcopyRegion region = {0, 0, 0, 7, 5, 4};
    uint32_t level = 0, layer = 0; uint64_t row = 24, bytes = 184;
    if (which == 0) bytes--;                         // 最后切片最后有效字节不足
    if (which == 1) region.zoffset = 1;             // z + depth 越过 mip 深度
    if (which == 2) { level = 1; region = (NVNcopyRegion){0,0,1,3,2,2}; }
    if (which == 3) layer = 1;                      // 3D 只有数组层 0；z 在区域里
    if (which == 4) row = 8;                        // 两块的行宽不足
    if (which == 5) region.width = 3;               // 非块整数宽且不在 mip 右边缘
    if (which == 6) row = UINT64_MAX;                // 行/片步距溢出先拒绝
    if (which == 7) { region.zoffset = -1; }         // 负 z 不可转成无符号通过
    if (which == 8) { region.height = 0; row = 16; } // 非空宽、零高不能成为零块行
    if (which == 9) region.width = 0;               // 非空高、零宽同样拒绝
    nvn_gpu_tex_upload(c, bc, level, layer, &region, blocks, row, bytes);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, c697_bc4_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_ARG, "C697 BC4 3D 非法输入%d 应ARG，实际%d %s", which, rc.code, e.why);
    nvn_gpu_tex_release(bc);
}
static int c697_bc4_3d_run(void) {
    uint32_t saved = g_dev.bc_decode_mask; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C697 BC4 初始close：%s", e.why);
    g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_RGTC1_UNORM - 0x41);
    if (!open_backend()) { g_dev.bc_decode_mask = saved; return 0; }
    c697_bc4_3d();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C697 BC4 close：%s", e.why);
    for (int i = 0; i < 10; i++) {
        if (!open_backend()) { g_dev.bc_decode_mask = saved; return 0; }
        c697_bc4_3d_bad(i);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C697 BC4 非法输入%d close：%s", i, e.why);
    }
    g_dev.bc_decode_mask = saved;
    return open_backend();
}

// #658: isolated native GPU-count truth tests; no NVN command expansion.
static nvn_gpu_pipe c658_producer(const char *dir) {
    char path[512]; size_t len=0;
    snprintf(path,sizeof path,"%s/count_args.comp.spv",dir); void *blob=read_file(path,&len);
    CHECK(blob != NULL,"C658 producer SPIR-V"); if(!blob)return NULL;
    nvn_gpu_layout_ent ent={0}; ent.cls=NVN_GPU_RES_SSBO;ent.access=NVN_GPU_ACC_RW;
    ent.binding=0;ent.count=1;ent.stages=1u<<NVN_GPU_SH_CS;
    nvn_gpu_layout layout={0};layout.ent=&ent;layout.nent=1;
    nvn_gpu_shader_src src={0};src.stage=NVN_GPU_SH_CS;src.kind=NVN_GPU_BLOB_SPIRV;
    src.entry="main";src.blob=blob;src.bytes=len;src.layout=&layout;
    nvn_gpu_shader sh=NULL;nvn_gpu_pipe pipe=NULL;nvn_gpu_err err={0};
    nvn_gpu_rc r=nvn_gpu_shader_load(&src,&sh,&err);CHECK(OK(r),"C658 producer load %d %s",r.code,err.why);
    if(OK(r)){nvn_gpu_stage_desc sd={0};sd.sh=sh;r=nvn_gpu_cpipe_get(&sd,NVN_GPU_PIPE_SYNC,&pipe,&err);CHECK(OK(r),"C658 producer pipe %d %s",r.code,err.why);}
    if(sh) nvn_gpu_shader_release(sh);
    free(blob);return pipe;
}
// Separate from mode8's g_retired oracle; static storage remains valid on timeout.
static atomic_int c658_cleanup_retired;
static int c658_cleanup_submitted; // Only the test thread changes this target.
static void c658_cleanup_done(void *u) {
    (void)u;
    atomic_fetch_add(&c658_cleanup_retired,1);
}
static void c658_case(const char *dir,unsigned count,int indexed,int mode) {
    int saved_enabled=g_dev.draw_indirect_count_enabled;
    if(mode==7)g_dev.draw_indirect_count_enabled=0;
    memset(&g_desc,0,sizeof g_desc);g_desc.host_device=&g_dev;g_desc.app_label="count-only";
    if(!open_backend()){g_dev.draw_indirect_count_enabled=saved_enabled;return;}
    nvn_gpu_caps caps;nvn_gpu_caps_get(&caps);
    CHECK((!!(caps.caps & NVN_GPU_CAP_INDIRECT_COUNT))==(mode!=7),"C658 actual enabled count capability mode=%d",mode);
    nvn_gpu_pipe gp=color_pipe(dir,0),cp=c658_producer(dir);
    if(!gp||!cp){nvn_gpu_err err={0};nvn_gpu_close(5000000000ull,&err);g_dev.draw_indirect_count_enabled=saved_enabled;return;}
    float verts[24][8]; const unsigned corners[6]={0,1,2,2,1,3};
    for(unsigned region=0;region<4;region++)for(unsigned j=0;j<6;j++){
        unsigned k=corners[j];float *v=verts[region*6+j];memset(v,0,8*sizeof *v);
        v[0]=-1.0f+region*0.5f+((k&2)?0.5f:0.0f);v[1]=(k&1)?1.0f:-1.0f;v[3]=v[7]=1;
        v[4+(region<3?region:0)]=1;
    }
    uint32_t initial[48];for(unsigned i=0;i<48;i++)initial[i]=0xA5A5A5A5u;
    initial[0]=count;initial[4]=count?0:3; // Opposite CPU poison count, overwritten on GPU.
    for(unsigned i=0;i<5;i++){unsigned b=8+i*8;initial[b]=6;initial[b+1]=1;initial[b+2]=18;initial[b+3]=initial[b+4]=0;}
    uint32_t indices[24];for(unsigned i=0;i<24;i++)indices[i]=i;
    nvn_gpu_buf args=make_buf(sizeof initial,NVN_GPU_MEM_SHARED,initial);
    uint32_t count_initial[8]={0};count_initial[4]=count?0:3;
    nvn_gpu_buf separate_count=mode==8?make_buf(sizeof count_initial,NVN_GPU_MEM_SHARED,count_initial):NULL;
    nvn_gpu_buf vb=make_buf(sizeof verts,NVN_GPU_MEM_SHARED,verts);
    nvn_gpu_buf ib=indexed?make_buf(sizeof indices,NVN_GPU_MEM_SHARED,indices):NULL;
    nvn_gpu_buf rb=make_buf(64*64*4,NVN_GPU_MEM_SHARED,NULL);
    nvn_gpu_tex tex=make_tex(64,64,NVN_FORMAT_RGBA8,NVN_GPU_TEX_RENDER);nvn_gpu_view view=make_view(tex);
    uint64_t signal=1;int retire_base=atomic_load(&g_retired);
    nvn_gpu_err err={0};
    nvn_gpu_cmd cmd=begin_cmd();nvn_gpu_binding binding={0};binding.cls=NVN_GPU_RES_SSBO;
    binding.u.b.buf=args;binding.u.b.len=sizeof initial;
    nvn_gpu_set_pipe(cmd,cp);nvn_gpu_bind(cmd,NVN_GPU_SH_CS,&binding,1);nvn_gpu_dispatch(cmd,cp,1,1,1,NULL,0);
    if(separate_count)nvn_gpu_copy_buf(cmd,args,16,separate_count,16,4);
    nvn_gpu_barrier(cmd,NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);
    if(mode==8){
        nvn_gpu_rc prep=nvn_gpu_cmd_commit(cmd,1,on_done,NULL,&err);
        CHECK(OK(prep),"C658 producer commit %d %s",prep.code,err.why);
        if(!OK(prep))goto c658_cleanup;
        // Waiting for a following empty segment proves the producer's retained
        // references were retired, not merely that its fence was published.
        nvn_gpu_cmd drain=begin_cmd();prep=nvn_gpu_cmd_commit(drain,2,on_done,NULL,&err);
        CHECK(OK(prep),"C658 producer drain commit %d %s",prep.code,err.why);
        if(!OK(prep))goto c658_cleanup;
        prep=nvn_gpu_fence_wait(2,5000000000ull,&err);
        CHECK(OK(prep),"C658 producer drain wait %d %s",prep.code,err.why);
        if(!OK(prep))goto c658_cleanup;
        cmd=begin_cmd();signal=3;
    }

    nvn_gpu_pass_desc pass={0};pass.ncolor=1;pass.color[0].view=view;
    pass.color[0].load=NVN_GPU_LOAD_CLEAR;pass.color[0].store=NVN_GPU_STORE_STORE;pass.color[0].clear.f[3]=1;
    pass.w=pass.h=64;pass.layers=1;nvn_gpu_pass_begin(cmd,&pass);nvn_gpu_set_pipe(cmd,gp);
    nvn_gpu_dyn dyn={0};dyn.origin_ll=1;dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=64;dyn.vp[0][5]=1;
    dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=64;nvn_gpu_set_dyn(cmd,&dyn);nvn_gpu_set_vbuf(cmd,0,vb,0);
    nvn_gpu_draw_args draw={0};draw.prim=4;draw.instances=1;draw.ind=args;draw.ind_off=32;
    draw.first=draw.count=UINT32_MAX; // CPU direct-draw fields are stale, not GPU indirect authority.
    draw.cnt=separate_count?separate_count:args;draw.cnt_off=16;draw.max_draws=mode==1?0:3;draw.stride=32;draw.ibuf=ib;draw.itype=2;
    if(mode==2)draw.cnt_off=18;
    if(mode==3)draw.ind_off=34;
    if(mode==4)draw.stride=18;
    if(mode==5)draw.ind_off=160; // max span exceeds 192-byte buffer despite valid actual count
    if(mode==6)draw.cnt_off=192;
    nvn_gpu_draw(cmd,&draw);
    if(separate_count){nvn_gpu_buf_release(separate_count,on_retire,NULL);separate_count=NULL;
        CHECK(atomic_load(&g_retired)==retire_base,"C658 distinct count retired before draw submission");
        if(atomic_load(&g_retired)!=retire_base){nvn_gpu_cmd_abort(cmd);goto c658_cleanup;}
    }
    nvn_gpu_pass_end(cmd);nvn_gpu_copy_tex_buf(cmd,tex,0,0,NULL,rb,0,256,16384);
    nvn_gpu_rc r=nvn_gpu_cmd_commit(cmd,signal,on_done,NULL,&err);
    int expected_rc=mode==7?NVN_GPU_E_UNSUPPORTED:(mode>=2&&mode<=6?NVN_GPU_E_ARG:NVN_GPU_OK);
    CHECK(r.code==expected_rc,"C658 count=%u indexed=%d mode=%d commit %d expected=%d %s",count,indexed,mode,r.code,expected_rc,err.why);
    if(OK(r)){
        r=nvn_gpu_fence_wait(signal,5000000000ull,&err);CHECK(OK(r),"C658 wait %d %s",r.code,err.why);
        if(OK(r)){
            const uint8_t *pixels=buf_cpu(rb);unsigned expected=mode==1?0:(count<3?count:3);
            for(unsigned region=0;region<4;region++){
                const uint8_t *p=pixels+(32*64+region*16+8)*4;
                unsigned red=region<expected && region==0?255:0,green=region<expected && region==1?255:0,blue=region<expected && region==2?255:0;
                CHECK(p[0]==red&&p[1]==green&&p[2]==blue&&p[3]==255,"C658 count=%u indexed=%d region=%u got %u/%u/%u/%u expected %u/%u/%u/255",count,indexed,region,p[0],p[1],p[2],p[3],red,green,blue);
            }
            const uint32_t *audit=(const uint32_t*)buf_cpu(args);
            CHECK(audit[4]==count,"C658 GPU count audit %u expected %u",audit[4],count);
            for(unsigned i=0;i<5;i++){unsigned b=8+i*8;
                CHECK(audit[b]==6&&audit[b+1]==1&&audit[b+2]==6*(i<3?i:3)&&audit[b+3]==0&&audit[b+4]==0,"C658 GPU args audit draw %u",i);
                CHECK(audit[b+5]==0xA5A5A5A5u&&audit[b+6]==0xA5A5A5A5u&&audit[b+7]==0xA5A5A5A5u,"C658 padded stride guard draw %u",i);
            }
        }
    }
    if(mode==8)CHECK(wait_until(&g_retired,retire_base+1,2000),"C658 distinct count did not retire after consumption");
c658_cleanup:
    nvn_gpu_view_release(view);nvn_gpu_tex_release(tex);
    nvn_gpu_buf cleanup_bufs[]={separate_count,args,vb,ib,rb};
    for(unsigned i=0;i<sizeof cleanup_bufs/sizeof cleanup_bufs[0];i++)if(cleanup_bufs[i]){
        ++c658_cleanup_submitted;
        nvn_gpu_buf_release(cleanup_bufs[i],c658_cleanup_done,NULL);
    }
    CHECK(wait_until(&c658_cleanup_retired,c658_cleanup_submitted,2000),"C658 cleanup buffers did not retire");
    CHECK(OK(nvn_gpu_close(5000000000ull,&err)),"C658 close %s",err.why);
    g_dev.draw_indirect_count_enabled=saved_enabled;
}
static void c658_suite(const char *dir,int strict) {
    memset(&g_desc,0,sizeof g_desc);g_desc.host_device=&g_dev;g_desc.app_label="count-cap";
    if(!open_backend())return;
    nvn_gpu_caps caps;nvn_gpu_caps_get(&caps);nvn_gpu_err err={0};
    CHECK(OK(nvn_gpu_close(5000000000ull,&err)),"C658 capability close");
    int can_run=(caps.caps & NVN_GPU_CAP_INDIRECT_COUNT) && g_dev.properties.limits.maxDrawIndirectCount>=5;
    if(strict)CHECK(can_run,"C658 isolated positive matrix requires enabled native count and device max >=5");
    if(can_run || strict){
        const unsigned values[]={0,1,3,5};
        for(int indexed=0;indexed<2;indexed++)for(unsigned i=0;i<4;i++)c658_case(dir,values[i],indexed,0);
        for(int indexed=0;indexed<2;indexed++)for(unsigned i=0;i<4;i++)c658_case(dir,values[i],indexed,8);
        for(int indexed=0;indexed<2;indexed++)for(int mode=1;mode<=6;mode++)c658_case(dir,3,indexed,mode);
    } else printf("C658 positive matrix unavailable on this device; checking unsupported path only\n");
    for(int indexed=0;indexed<2;indexed++)c658_case(dir,3,indexed,7);
}
static int c658_main(int argc,char **argv) {
    if(argc!=2){fprintf(stderr,"count-only: shader directory required\n");return 2;}
    char why[256]={0};if(s2a_vk_device_create(&g_dev,why,sizeof why)!=0){fprintf(stderr,"count-only device: %s\n",why);return 2;}
    printf("count-only device=%s maxDrawIndirectCount=%u\n",g_dev.properties.deviceName,g_dev.properties.limits.maxDrawIndirectCount);
    if(g_dev.properties.limits.maxDrawIndirectCount<5){fprintf(stderr,"count-only needs device limit >=5\n");s2a_vk_device_destroy(&g_dev,1);return 2;}
    c658_suite(argv[1],1);
    {extern uint64_t nvn_vk_dbg_dq_recs(void);uint64_t n=nvn_vk_dbg_dq_recs();
     CHECK(getenv("S2A_TEST_NVK_DQ")?n>0:n==0,"C658 DQ recording count=%llu",(unsigned long long)n);}
    s2a_vk_device_destroy(&g_dev,1);printf("count-only failures=%d\n",g_fail);return g_fail?1:0;
}

// #697 A: no shaders or unrelated tests; copy callback-owned text before publishing completion.
struct error697_done { atomic_int calls; uint64_t signal; int code; uint32_t status; char why[256]; };
static void error697_callback(void *u, const nvn_gpu_done *d) {
    struct error697_done *o=u;
    o->signal=d->signal; o->code=d->rc.code; o->status=d->status;
    snprintf(o->why,sizeof o->why,"%s",d->why?d->why:"");
    atomic_fetch_add_explicit(&o->calls,1,memory_order_release);
}
static void error697_expect(struct error697_done *o, uint64_t signal, nvn_gpu_rc rc,
                            const nvn_gpu_err *err, int code, const char *why) {
    CHECK(rc.code==code && err->rc.code==code,"E697 synchronous rc=%d err=%d expected=%d",rc.code,err->rc.code,code);
    CHECK(strstr(err->why,why)!=NULL,"E697 first reason missing: %s",err->why);
    if(!wait_until(&o->calls,1,5000)){CHECK(0,"E697 callback timeout");return;}
    CHECK(atomic_load_explicit(&o->calls,memory_order_acquire)==1,"E697 exactly one callback");
    CHECK(o->signal==signal && o->code==code && o->status==NVN_GPU_DONE_FAILED,"E697 asynchronous result mismatch");
    CHECK(!strcmp(o->why,err->why),"E697 synchronous/asynchronous reason mismatch: %s / %s",err->why,o->why);
}
static int error697_only(void) {
    char why[256]={0};
    if(s2a_vk_device_create(&g_dev,why,sizeof why)!=0){fprintf(stderr,"E697 device: %s\n",why);return 2;}
    memset(&g_desc,0,sizeof g_desc);g_desc.host_device=&g_dev;g_desc.app_label="error697-only";
    for(int mode=0;mode<3;mode++) {
        if(!open_backend())return 1;
        uint8_t zeros[16]={0};nvn_gpu_buf b=make_buf(16,NVN_GPU_MEM_SHARED,zeros);
        nvn_gpu_cmd first=begin_cmd(),second=begin_cmd();
        nvn_gpu_err e; memset(&e,0,sizeof e);e.rc.code=123;strcpy(e.why,"stale sentinel");
        struct error697_done d1={0},d2={0};nvn_gpu_rc rc;
        if(mode==0) {
            nvn_gpu_fill_buf(first,b,0,6,0xdeadbeef); // first ARG: misaligned size
            nvn_gpu_fill_buf(first,b,99,4,0xffffffff); // later bounds error must not overwrite it
        } else nvn_gpu_fill_buf(first,b,0,16,0xdeadbeef);
        nvn_gpu_fill_buf(second,b,0,16,0xffffffff);
        if(mode==1)nvn_gpu_vk_test_set_fault(1);
        uint64_t signal=mode==2?0:1;
        rc=nvn_gpu_cmd_commit(first,signal,error697_callback,&d1,&e);
        error697_expect(&d1,signal,rc,&e,mode==1?NVN_GPU_E_DEVICE:NVN_GPU_E_ARG,
                        mode==0?"bytes=6":mode==1?"测试注入":"signal 0");
        memset(&e,0,sizeof e);e.rc.code=123;strcpy(e.why,"second stale sentinel");
        rc=nvn_gpu_cmd_commit(second,2,error697_callback,&d2,&e);
        error697_expect(&d2,2,rc,&e,NVN_GPU_E_FAILED,"时间线已失败");
        CHECK(nvn_gpu_fence_done()==0 && nvn_gpu_fence_failed()==1,"E697 timeline changed");
        CHECK(nvn_gpu_fence_wait(2,0,&e).code==NVN_GPU_E_FAILED,"E697 failed wait");
        for(int i=0;i<16;i++)CHECK(buf_cpu(b)[i]==0,"E697 failed command wrote buffer");
        nvn_gpu_cmd blocked=NULL;CHECK(nvn_gpu_cmd_begin(NULL,&blocked,&e).code==NVN_GPU_E_FAILED,"E697 terminal begin");
        nvn_gpu_buf_release(b,NULL,NULL);
        CHECK(OK(nvn_gpu_close(5000000000ull,&e)),"E697 close");
        CHECK(atomic_load(&d1.calls)==1 && atomic_load(&d2.calls)==1,"E697 duplicate callback after close");
    }
    s2a_vk_device_destroy(&g_dev,1);printf("E697 isolated failures=%d\n",g_fail);return g_fail?1:0;
}

// C205 图形管线散列表（#1130）：桶数随管线数增长（装填因子 <= 1，含起始 16 桶的多次翻倍）、增长重排后每条管线仍取得到、散列看得见 same_pipe_desc 比的每个字节 / 字段、
// 「same_pipe_desc 相等 ⇒ 散列相等」、查表先比存下的散列（只有散列相同的才逐字段比）、整表逐条比与「表建不出来」两条退路的答案和散列表路相同。
uint64_t nvn_vk_dbg_pipe_stat(int which);
uint32_t nvn_vk_dbg_pipe_hash(const nvn_gpu_pipe_desc *d);
int nvn_vk_dbg_pipe_same(const nvn_gpu_pipe_desc *a, const nvn_gpu_pipe_desc *b);
void nvn_vk_dbg_pipe_test_mode(int m);
static uint32_t c205_rng(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
// C205_DESC_SIZE：nvn_gpu_pipe_desc 的大小。描述里加了字段，要同时改 same_pipe_desc、pipe_hash（backend_vulkan.c）和下面 c205_fields 的逐字段表，再改这个数
enum { C205_NCH = 2100, C205_NST = 400, C205_N = C205_NCH + C205_NST, C205_NG = 700, C205_DESC_SIZE = 2360 };
// 第 i 条：前 C205_NCH 条只差颜色写掩码，后 C205_NST 条只差顶点流步长（改动前的散列没算顶点流：这 400 条全落进一个桶）
static void c205_desc(const nvn_gpu_pipe_desc *base, int i, nvn_gpu_pipe_desc *o) {
    memcpy(o, base, sizeof *o);
    if (i < C205_NCH) o->chmask.mask = 0xFu | ((uint32_t)(i + 1) << 4);
    else o->stream[0].stride = 16 + 4 * (uint32_t)(i - C205_NCH + 1);
}
static int c205_ptr_cmp(const void *a, const void *b) { const uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b; return x < y ? -1 : x > y; }
// 随机一份描述（只保证 same_pipe_desc / 散列能安全读：特化常量条数 <= 上限）
static void c205_rand_desc(nvn_gpu_pipe_desc *a, uint32_t *rs) {
    for (size_t k = 0; k < sizeof *a; k++) ((uint8_t *)a)[k] = (uint8_t)c205_rng(rs);
    for (int i = 0; i < 5; i++) a->st[i].nspec = c205_rng(rs) % (NVN_GPU_SPEC_MAX + 1);
}
static void c205_note(char *buf, size_t cap, const char *nm) {
    const size_t n = strlen(buf), l = strlen(nm);
    if (n + l + 2 < cap) { memcpy(buf + n, nm, l); buf[n + l] = ' '; buf[n + l + 1] = 0; }
}
// 散列看得见 same_pipe_desc 比的每个字节 / 字段：逐个改一处，same_pipe_desc 必须判不同（夹具：证明这一改确实落在被比的东西上），散列也必须变
static void c205_fields(const nvn_gpu_pipe_desc *base) {
    int blind = 0, fixture = 0, tried = 0; char names[400], fnames[200]; names[0] = fnames[0] = 0;
    // pre 先把基准改成 b_，stmt 再改 m_（= b_ 的副本）
#define C205_MUTB(pre, stmt, nm) do { nvn_gpu_pipe_desc b_ = *base, m_; pre; m_ = b_; stmt; tried++; \
        if (nvn_vk_dbg_pipe_same(&m_, &b_)) { fixture++; c205_note(fnames, sizeof fnames, nm); } \
        else if (nvn_vk_dbg_pipe_hash(&m_) == nvn_vk_dbg_pipe_hash(&b_)) { blind++; c205_note(names, sizeof names, nm); } } while (0)
#define C205_MUT(stmt, nm) C205_MUTB((void)0, stmt, nm)
    for (int i = 0; i < 5; i++) { char nm[40]; snprintf(nm, sizeof nm, "st%d句柄", i); C205_MUT(m_.st[i].sh = (nvn_gpu_shader)((uintptr_t)m_.st[i].sh ^ 0x100), nm); }
    // 特化常量：条数本身（0 → 1 → … → 上限，常量内容全零，只差条数）与前 nspec 条里的每个字节
    for (int i = 0; i < 5; i++)
        for (uint32_t k = 0; k < NVN_GPU_SPEC_MAX; k++) { char nm[40]; snprintf(nm, sizeof nm, "st%d条数%u", i, (unsigned)k); C205_MUTB(b_.st[i].nspec = k, m_.st[i].nspec = k + 1, nm); }
    for (int i = 0; i < 5; i++)
        for (size_t k = 0; k < NVN_GPU_SPEC_MAX * sizeof(nvn_gpu_spec); k++) {
            char nm[40]; snprintf(nm, sizeof nm, "st%d常量+%zu", i, k);
            C205_MUTB(b_.st[i].nspec = NVN_GPU_SPEC_MAX, ((uint8_t *)m_.st[i].spec)[k] ^= 1, nm);
        }
    C205_MUT(m_.prim ^= 1, "prim");
    C205_MUT(m_.patch_size ^= 1, "patch_size");
    C205_MUT(m_.dfmt ^= 1, "dfmt");
    C205_MUT(m_.sfmt ^= 1, "sfmt");
    C205_MUT(m_.samples ^= 1, "samples");
    C205_MUT(m_.rast_discard ^= 1, "rast_discard");
    C205_MUT(m_.clip_neg1 ^= 1, "clip_neg1");
    C205_MUT(m_.origin_ll ^= 1, "origin_ll");
    C205_MUT(m_.depth_clamp ^= 1, "depth_clamp");
    C205_MUT(m_.poly.cull ^= 1, "poly.cull");
    C205_MUT(m_.poly.front ^= 1, "poly.front");
    C205_MUT(m_.poly.mode ^= 1, "poly.mode");
    C205_MUT(m_.poly.offset_en ^= 1, "poly.offset_en");
    // 按字节 memcmp 比的几块 + 颜色格式数组：每个字节翻一位
    const struct { const char *nm; size_t off, len; } reg[] = {
        {"cfmt", offsetof(nvn_gpu_pipe_desc, cfmt), sizeof base->cfmt}, {"lmap", offsetof(nvn_gpu_pipe_desc, lmap), sizeof base->lmap},
        {"blend", offsetof(nvn_gpu_pipe_desc, blend), sizeof base->blend}, {"color", offsetof(nvn_gpu_pipe_desc, color), sizeof base->color},
        {"chmask", offsetof(nvn_gpu_pipe_desc, chmask), sizeof base->chmask}, {"ms", offsetof(nvn_gpu_pipe_desc, ms), sizeof base->ms},
        {"attr", offsetof(nvn_gpu_pipe_desc, attr), sizeof base->attr}, {"stream", offsetof(nvn_gpu_pipe_desc, stream), sizeof base->stream},
        {"ds", offsetof(nvn_gpu_pipe_desc, ds), sizeof base->ds},
    };
    for (size_t r = 0; r < sizeof reg / sizeof *reg; r++)
        for (size_t k = 0; k < reg[r].len; k++) { char nm[40]; snprintf(nm, sizeof nm, "%s+%zu", reg[r].nm, k); C205_MUT(((uint8_t *)&m_)[reg[r].off + k] ^= 1, nm); }
#undef C205_MUT
#undef C205_MUTB
    CHECK(fixture == 0, "C205 夹具：下列改动没被 same_pipe_desc 判成不同（用例的改法不对，或 same_pipe_desc 不比这一项）：%s", fnames);
    CHECK(blind == 0, "C205 散列看不见 same_pipe_desc 比的 %d / %d 处改动：%s", blind, tried, names);
    printf("C205 逐字节 / 逐字段：试了 %d 处改动，散列看不见 %d 处\n", tried, blind);
}
static void c205_pipe_ht(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C205 读着色器"); return; }
    nvn_gpu_pipe_desc base; fill_gpipe_desc(&base, vs, fs, 0);
    nvn_gpu_pipe_desc *d = malloc(sizeof *d);
    nvn_gpu_pipe *pp = calloc(C205_N, sizeof *pp), *srt = calloc(C205_N, sizeof *srt);
    nvn_gpu_err e = {0};
    CHECK(d && pp && srt, "C205 分配");
    if (!(d && pp && srt)) goto done;
    CHECK(sizeof(nvn_gpu_pipe_desc) == C205_DESC_SIZE, "C205 描述大小期望 %d（描述里加字段要同时改 same_pipe_desc、pipe_hash 和 c205_fields 的逐字段表）→ %zu", (int)C205_DESC_SIZE, sizeof(nvn_gpu_pipe_desc));
    // 1 建 C205_N 条互不相同的图形管线
    int bad = 0;
    for (int i = 0; i < C205_N; i++) { c205_desc(&base, i, d); if (!OK(nvn_gpu_pipe_get(d, NVN_GPU_PIPE_SYNC, &pp[i], &e)) || !pp[i]) bad++; }
    CHECK(bad == 0, "C205：建 %d 条管线期望全部成功 → 失败 %d（%s）", C205_N, bad, e.why);
    memcpy(srt, pp, C205_N * sizeof *pp);
    qsort(srt, C205_N, sizeof *srt, c205_ptr_cmp);
    int dup = 0; for (int i = 1; i < C205_N; i++) if (srt[i] == srt[i - 1]) dup++;
    CHECK(dup == 0, "C205：描述互不相同的管线期望指针互不相同 → 重复 %d 对", dup);
    const uint64_t nb = nvn_vk_dbg_pipe_stat(0), cnt = nvn_vk_dbg_pipe_stat(1);
    CHECK(cnt == C205_N, "C205：登记的图形管线数期望 %d → %llu", C205_N, (unsigned long long)cnt);
    CHECK(nb >= cnt && !(nb & (nb - 1)) && nb >= 4096, "C205：桶数期望 >= 管线数、2 的幂、>= 4096（两次翻倍）→ 桶 %llu、管线 %llu", (unsigned long long)nb, (unsigned long long)cnt);
    // 2 再取一遍：每条都命中原来那一条（重排没丢节点），并读每次走过的条数与逐字段比的条数
    const uint64_t n0 = nvn_vk_dbg_pipe_stat(3), s0 = nvn_vk_dbg_pipe_stat(4), c0 = nvn_vk_dbg_pipe_stat(2);
    int miss = 0;
    for (int i = 0; i < C205_N; i++) { c205_desc(&base, i, d); nvn_gpu_pipe q = NULL; if (!OK(nvn_gpu_pipe_get(d, NVN_GPU_PIPE_SYNC, &q, &e)) || q != pp[i]) miss++; }
    const uint64_t dn = nvn_vk_dbg_pipe_stat(3) - n0, ds_ = nvn_vk_dbg_pipe_stat(4) - s0, dc = nvn_vk_dbg_pipe_stat(2) - c0;
    const double avg = dn ? (double)ds_ / (double)dn : 0, cavg = dn ? (double)dc / (double)dn : 0;
    CHECK(miss == 0 && dn == C205_N, "C205：再取一遍期望全部命中原管线、命中 %d 次 → 没命中 %d、命中 %llu 次", C205_N, miss, (unsigned long long)dn);
    printf("C205：桶 %llu、管线 %llu；再取一遍每次走 %.2f 条、逐字段比 %.3f 条\n", (unsigned long long)nb, (unsigned long long)cnt, avg, cavg);
    CHECK(avg <= 2.5, "C205：每次走过的条数期望 <= 2.5（装填因子 <= 1 + 散列看得见各字段）→ %.2f", avg);
    CHECK(cavg <= 1.01, "C205：每次逐字段比的条数期望 <= 1.01（先比存下的散列，散列相同的才碰 key）→ %.3f", cavg);
    // 3 退路一：整表逐条比（nvk_pipe_scan 的路）——每 7 条取一回，应当取到同一条；链很长，但先比存下的散列，逐字段比的条数仍约等于取的次数
    nvn_vk_dbg_pipe_test_mode(1);
    const uint64_t cw0 = nvn_vk_dbg_pipe_stat(2), sw0 = nvn_vk_dbg_pipe_stat(4), nw0 = nvn_vk_dbg_pipe_stat(3);
    int miss1 = 0, tried = 0;
    for (int i = 0; i < C205_N; i += 7) { c205_desc(&base, i, d); nvn_gpu_pipe q = NULL; tried++; if (!OK(nvn_gpu_pipe_get(d, NVN_GPU_PIPE_SYNC, &q, &e)) || q != pp[i]) miss1++; }
    const uint64_t cw = nvn_vk_dbg_pipe_stat(2) - cw0, sw = nvn_vk_dbg_pipe_stat(4) - sw0, nw = nvn_vk_dbg_pipe_stat(3) - nw0;
    nvn_vk_dbg_pipe_test_mode(0);
    CHECK(miss1 == 0 && nw == (uint64_t)tried, "C205 退路一：整表逐条比的退路期望每条都取到同一条管线、命中 %d 次 → %d / %d 条不符、命中 %llu 次", tried, miss1, tried, (unsigned long long)nw);
    CHECK(sw >= (uint64_t)tried * 100, "C205 退路一：整表逐条比期望每次走过很多条（>= 100）→ 平均 %.1f", tried ? (double)sw / tried : 0.0);
    CHECK(cw <= (uint64_t)tried + (uint64_t)tried / 100 + 2, "C205 退路一：整表逐条比时也先比存下的散列，逐字段比的条数期望约等于取的次数（%d）→ %llu", tried, (unsigned long long)cw);
    // 4 散列看得见每个字节 / 字段
    c205_fields(&base);
    {   // 4b #1102：same_pipe_desc 按实际生效的图元重启比（条带 / 扇才认 prim_restart），散列也必须看得见它
        nvn_gpu_pipe_desc ra = base, rb; ra.prim = 5; ra.prim_restart = 0; rb = ra; rb.prim_restart = 1;   // NVN 5 = 三角形条带
        CHECK(!nvn_vk_dbg_pipe_same(&ra, &rb), "C205 条带图元上重启开 / 关期望是两条不同的管线 → same_pipe_desc 判相同");
        CHECK(nvn_vk_dbg_pipe_hash(&ra) != nvn_vk_dbg_pipe_hash(&rb), "C205 条带图元上重启开 / 关期望散列不同（散列要混实际生效的重启）→ 相同");
    }
    // 5 性质：same_pipe_desc 相等 ⇒ 散列相等。随机描述，另一份只改 same_pipe_desc 不比的字节（prim_restart 的非零取值、保留字节、poly 位域填充位、nspec 以外的特化常量）
    {
        uint32_t rs = 0x2051u; int viol = 0, fixture = 0;
        for (int it = 0; it < 4000; it++) {
            nvn_gpu_pipe_desc a, b; c205_rand_desc(&a, &rs); b = a;
            b.prim_restart = a.prim_restart ? (uint8_t)(1 + c205_rng(&rs) % 255) : 0;   /* 只变原始字节值、不翻开关：same_pipe_desc 按实际生效的重启比（#1102）*/ for (int k = 0; k < 3; k++) b._rsv_prim[k] = (uint8_t)c205_rng(&rs);
            { uint32_t w; memcpy(&w, &b.poly, sizeof w); w ^= 0xFFFFFF00u & (uint32_t)c205_rng(&rs); memcpy(&b.poly, &w, sizeof w); }
            for (int i = 0; i < 5; i++) for (uint32_t k = b.st[i].nspec; k < NVN_GPU_SPEC_MAX; k++) { uint8_t *sp = (uint8_t *)&b.st[i].spec[k]; for (size_t j = 0; j < sizeof b.st[i].spec[k]; j++) sp[j] = (uint8_t)c205_rng(&rs); }
            if (!nvn_vk_dbg_pipe_same(&a, &b)) { fixture++; continue; }
            if (nvn_vk_dbg_pipe_hash(&a) != nvn_vk_dbg_pipe_hash(&b)) viol++;
        }
        CHECK(fixture == 0, "C205 性质夹具：只改不比的字节后 same_pipe_desc 应仍相等 → %d 次判不同", fixture);
        CHECK(viol == 0, "C205：same_pipe_desc 相等的两份描述（只差不比的字节）散列期望相同 → %d / 4000 次不同", viol);
    }
done:
    free(d); free(pp); free(srt);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}
// 起始 16 桶（用例开关 4）、建 C205_NG = 700 条：每建一条核「桶数 >= 管线数、2 的幂」（16 → 1024 共翻倍 6 次，每次重排一遍所有桶）；建完再取一遍全部命中原管线
static void c205_pipe_grow(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C205 增长 读着色器"); return; }
    nvn_gpu_pipe_desc base, d; fill_gpipe_desc(&base, vs, fs, 0);
    nvn_gpu_pipe *pp = calloc(C205_NG, sizeof *pp); nvn_gpu_err e = {0};
    CHECK(pp != NULL, "C205 增长 分配");
    if (!pp) goto done;
    nvn_vk_dbg_pipe_test_mode(4);
    int bad = 0, inv = 0, miss = 0;
    for (int i = 0; i < C205_NG; i++) {
        c205_desc(&base, i, &d);
        if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &pp[i], &e)) || !pp[i]) bad++;
        const uint64_t nb = nvn_vk_dbg_pipe_stat(0), cnt = nvn_vk_dbg_pipe_stat(1);
        if (cnt != (uint64_t)i + 1 || nb < cnt || (nb & (nb - 1)) || nb < 16) inv++;
    }
    nvn_vk_dbg_pipe_test_mode(0);
    CHECK(bad == 0 && inv == 0, "C205 增长：建 %d 条、每建一条桶数期望 >= 管线数且为 2 的幂 → 建失败 %d、不符 %d 次", C205_NG, bad, inv);
    CHECK(nvn_vk_dbg_pipe_stat(0) == 1024, "C205 增长：16 起翻倍到装得下 %d 条，期望 1024 桶 → %llu", C205_NG, (unsigned long long)nvn_vk_dbg_pipe_stat(0));
    for (int i = 0; i < C205_NG; i++) { c205_desc(&base, i, &d); nvn_gpu_pipe q = NULL; if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &q, &e)) || q != pp[i]) miss++; }
    CHECK(miss == 0, "C205 增长：翻倍 6 次重排后再取一遍期望全部命中原管线 → 没命中 %d / %d", miss, C205_NG);
done:
    free(pp);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}
// 增长时分配失败（用例开关 4 | 8：起始 16 桶，第一张表照常建，之后每次增长的分配都失败）：沿用旧表、链变长，答案不变，桶数一直是 16
static void c205_pipe_growfail(const char *dir) {
    enum { NF = 100 };
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C205 增长失败 读着色器"); return; }
    nvn_gpu_pipe_desc base, d; fill_gpipe_desc(&base, vs, fs, 0);
    nvn_gpu_pipe pp[NF] = {0}; nvn_gpu_err e = {0};
    nvn_vk_dbg_pipe_test_mode(12);
    int bad = 0, miss = 0;
    for (int i = 0; i < NF; i++) { c205_desc(&base, i, &d); if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &pp[i], &e)) || !pp[i]) bad++; }
    for (int i = 0; i < NF; i++) { c205_desc(&base, i, &d); nvn_gpu_pipe q = NULL; if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &q, &e)) || q != pp[i]) miss++; }
    nvn_vk_dbg_pipe_test_mode(0);
    CHECK(bad == 0 && miss == 0, "C205 增长失败：建 %d 条、再取 %d 条期望全部成功且命中原管线 → 建失败 %d、没命中 %d", (int)NF, (int)NF, bad, miss);
    CHECK(nvn_vk_dbg_pipe_stat(0) == 16 && nvn_vk_dbg_pipe_stat(1) == NF, "C205 增长失败：增长分配失败时沿用旧表，桶数期望一直是 16、管线数 %d → 桶 %llu、管线 %llu", (int)NF, (unsigned long long)nvn_vk_dbg_pipe_stat(0), (unsigned long long)nvn_vk_dbg_pipe_stat(1));
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}
// 退路二：散列表建不出来（分配失败）——查找退回整表逐条比，答案不变；桶数一直是 0
static void c205_pipe_noht(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C205 退路二 读着色器"); return; }
    nvn_gpu_pipe_desc base, d; fill_gpipe_desc(&base, vs, fs, 0);
    nvn_gpu_pipe pp[60] = {0}; nvn_gpu_err e = {0};
    nvn_vk_dbg_pipe_test_mode(2);
    int bad = 0, miss = 0;
    for (int i = 0; i < 60; i++) { c205_desc(&base, i, &d); if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &pp[i], &e)) || !pp[i]) bad++; }
    for (int i = 0; i < 60; i++) { c205_desc(&base, i, &d); nvn_gpu_pipe q = NULL; if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &q, &e)) || q != pp[i]) miss++; }
    nvn_vk_dbg_pipe_test_mode(0);
    CHECK(bad == 0 && miss == 0, "C205 退路二（散列表建不出来）：建 60 条、再取 60 条期望全部成功且命中原管线 → 建失败 %d、没命中 %d", bad, miss);
    CHECK(nvn_vk_dbg_pipe_stat(0) == 0 && nvn_vk_dbg_pipe_stat(1) == 60, "C205 退路二：桶数期望 0、管线数 60 → 桶 %llu、管线 %llu", (unsigned long long)nvn_vk_dbg_pipe_stat(0), (unsigned long long)nvn_vk_dbg_pipe_stat(1));
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}
// 四个会话：缺省（建 2500 条）、起始 16 桶多次翻倍、增长时分配失败、散列表建不出来；各自一个会话（管线表是会话级的）
static void c205_all(const char *dir) {
    nvn_gpu_err e = {0};
    if (!open_backend()) { CHECK(0, "C205 open"); return; }
    c205_pipe_ht(dir);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C205 close：%d %s", e.rc.code, e.why);
    if (!open_backend()) { CHECK(0, "C205 增长 open"); return; }
    c205_pipe_grow(dir);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C205 增长 close：%d %s", e.rc.code, e.why);
    if (!open_backend()) { CHECK(0, "C205 增长失败 open"); return; }
    c205_pipe_growfail(dir);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C205 增长失败 close：%d %s", e.rc.code, e.why);
    if (!open_backend()) { CHECK(0, "C205 退路二 open"); return; }
    c205_pipe_noht(dir);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C205 退路二 close：%d %s", e.rc.code, e.why);
    printf("C205 图形管线散列表：增长 / 多次翻倍 / 增长失败沿用旧表 / 逐字节逐字段 / 先比散列 / 两条退路已核对\n");
}

// C207：诊断读数（nvk_gputime 开着才有）互不清零。「近 300 帧」读数（nvn_gpu_present 里）与「每 5 秒一次的逐笔读数」（nvn_gpu_draw 末尾）
// 读同一批累加器（g_desc_* / g_draw_ns）。原先两处各自 exchange 清零，各自只看到对方上次清零之后的那一段：
// 真机同一局里「每帧重写 867 组」对回收读数的「每帧取用 1750 组」差一倍（#1096 之前的 b28 包，世界场景），每帧毫秒、每笔微秒、条数都偏小。
// 一帧 = 一段、K 笔绘制，每笔换一次纹理视图（该组变脏 1 个，写 2 条描述符：纹理 + 采样器），所以每帧恒为脏组 K 个、描述符 2K 条，
// 每笔恒为 2 条、1 组。逐笔读数的周期用测试钩子缩到 20 毫秒，每帧睡 1 毫秒，保证一个 300 帧窗口里它至少触发十几次。
// 期望（互不清零）：①头一个读数窗口可能不满（计时中途才开），之后每个窗口读数「每帧重写 K 组、写 2K 条描述符」整数相等；
// ②逐笔读数的每一行都是「2.00 笔/笔（1.0 套）」（分子分母同窗口）。互相清零时 ① 只剩最后一次清零之后的一小段，② 里每个 300 帧窗口的整点那一行偏小。
// 备忘开着（S2A_TEST_DESC_MEMO）时内容相同的组不再写，条数 / 组数不再是定值，只跑不核。
#define C207_K 40
static void c207_diag_readers(const char *dir) {
    nvn_gpu_layout_ent ents[2]; memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl, vl; memset(&fl, 0, sizeof fl); memset(&vl, 0, sizeof vl); fl.ent = ents; fl.nent = 2;
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "tri.vert", "tri.frag", &vl, &fl, &vs, &fs, &vsm, &fsm)) { CHECK(0, "C207 读不到着色器"); return; }
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul; pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0; pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_err e; memset(&e, 0, sizeof e); nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C207 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_surface sf = NULL;
    CHECK(OK(nvn_gpu_surface_attach((void *)1, &sf, &e)) && sf, "C207 attach：%s", e.why);
    if (!pipe || !sf) goto done;
    const float verts[3][4] = {{-1, -1, 0, 0}, {3, -1, 2, 0}, {-1, 3, 0, 2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex red = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED), blue = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex out = make_tex(16, 16, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_tex shown = make_tex(64, 36, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view rv = make_view(red), bv = make_view(blue), ov = make_view(out), sv = make_view(shown);
    samp_par sp = {0}; nvn_gpu_samp samp = NULL; CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C207 sampler：%s", e.why);
    nvn_gpu_binding binds[2]; memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_dyn dyn; memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul; dyn.nvp = 1; dyn.vp[0][2] = 16; dyn.vp[0][3] = 16; dyn.vp[0][5] = 1; dyn.has_sc = 1; dyn.sc[2] = 16; dyn.sc[3] = 16;
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_present_desc pdsc; memset(&pdsc, 0, sizeof pdsc); pdsc.src = sv; pdsc.filter = 1;
    uint64_t sig = 20700;
    {   // 首段：上传两张 1×1 采样图和上屏用的图（计时还没开；GPU 时间戳的开关在进程里第一次提交时读一次，这里先让它读成 0）
        nvn_gpu_cmd c = begin_cmd(); const uint8_t r4[4] = {255, 0, 0, 255}, b4[4] = {0, 0, 255, 255}; static uint8_t px[64 * 36 * 4]; memset(px, 0x80, sizeof px);
        nvn_gpu_tex_upload(c, red, 0, 0, NULL, r4, 4, 4); nvn_gpu_tex_upload(c, blue, 0, 0, NULL, b4, 4, 4); nvn_gpu_tex_upload(c, shown, 0, 0, NULL, px, 64 * 4, sizeof px);
        CHECK(OK(nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e)), "C207 首段 commit：%s", e.why);
        CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "C207 首段 wait：%s", e.why); sig++;
    }
    g_c207_nwin = g_c207_nlog = g_c207_badlog = 0; g_c207_firstbad[0] = 0;
    atomic_store(&g_c207_cap, 1); atomic_store(&g_c207_gputime, 1);
    nvn_gpu_vk_test_diag_timing(1, 20000000ull);
    int frames = 0;
    for (; frames < 1500 && g_c207_nwin < 3; frames++) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass); pass.ncolor = 1; pass.w = 16; pass.h = 16; pass.layers = 1;
        pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(c, &pass); nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_set_dyn(c, &dyn);
        for (int d = 0; d < C207_K; d++) { binds[0].u.view = (d & 1) ? bv : rv; nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2); nvn_gpu_draw(c, &da); }
        nvn_gpu_pass_end(c);
        if (!OK(nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e)) || !OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e))) { CHECK(0, "C207 帧 %d commit / wait：%s", frames, e.why); break; }
        sig++;
        nvn_gpu_drawable dr; memset(&dr, 0, sizeof dr);
        if (!OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e)) || !OK(nvn_gpu_present(sf, &dr, &pdsc, 30000 + (uint64_t)frames, NULL, NULL, &e))) { CHECK(0, "C207 帧 %d 呈现：%s", frames, e.why); break; }
        struct timespec nap = {0, 1000000}; nanosleep(&nap, NULL);
    }
    nvn_gpu_vk_test_diag_timing(0, 0);
    atomic_store(&g_c207_gputime, 0); atomic_store(&g_c207_cap, 0);
    CHECK(g_c207_nwin >= 3, "C207 近 300 帧读数期望至少打出 3 次（头一次可能不满）→ %d 次（跑了 %d 帧）", g_c207_nwin, frames);
    CHECK(g_c207_nlog >= 15, "C207 逐笔读数期望至少 15 行（周期 20 毫秒，跑了 %d 帧、每帧睡 1 毫秒）→ %d 行", frames, g_c207_nlog);
    if (!g_memo_on) {
        int bad_win = 0; double sets_seen = 0, desc_seen = 0;
        for (int i = 1; i < g_c207_nwin && i < 12; i++) {
            const char *a = strstr(g_c207_win[i], "每帧重写 "), *b = strstr(g_c207_win[i], "、写 ");
            double s = -1, w = -1; if (a) s = strtod(a + strlen("每帧重写 "), NULL); if (b) w = strtod(b + strlen("、写 "), NULL);
            sets_seen = s; desc_seen = w;
            if (s != (double)C207_K || w != (double)(2 * C207_K)) { bad_win++; printf("C207 窗口 %d 读数不符：%s\n", i, g_c207_win[i]); }
        }
        CHECK(bad_win == 0, "C207 完整的 300 帧窗口读数期望「每帧重写 %d 组、写 %d 条描述符」→ 最后一个 %.0f 组 / %.0f 条，%d 个窗口不符", C207_K, 2 * C207_K, sets_seen, desc_seen, bad_win);
        CHECK(g_c207_badlog == 0, "C207 逐笔读数每一行期望「描述符更新 2.00 笔/笔（1.0 套）」→ %d / %d 行不符，第一条：%s", g_c207_badlog, g_c207_nlog, g_c207_firstbad);
    }
    nvn_gpu_view_release(rv); nvn_gpu_view_release(bv); nvn_gpu_view_release(ov); nvn_gpu_view_release(sv);
    nvn_gpu_tex_release(red); nvn_gpu_tex_release(blue); nvn_gpu_tex_release(out); nvn_gpu_tex_release(shown);
    nvn_gpu_samp_release(samp); nvn_gpu_buf_release(vb, NULL, NULL);
done:
    if (sf) CHECK(OK(nvn_gpu_surface_detach(sf, 5000000000ull, &e)), "C207 detach：%s", e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}
// C208：close 不该等满时限。缓冲先 release、它的缓冲视图还活着：close 销毁视图 → 缓冲进退休队列 → worker 退休它。
// 退休完 worker 要唤醒 close；漏了唤醒时 close 睡到截止时间才醒（给 5 秒就白等 5 秒，整套 vk 用例每次关会话都这样）。
static void c208_close_wakeup(void) {
    nvn_gpu_err e = {0};
    if (!open_backend()) { CHECK(0, "C208 open"); return; }
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_view v = NULL;
    nvn_gpu_rc r = b ? nvn_gpu_view_create_buf(b, 0, 64, NVN_FORMAT_R32UI, &v, &e) : (nvn_gpu_rc){0};
    CHECK(b && OK(r) && v, "C208 缓冲视图：%d %s", r.code, e.why);
    if (b) nvn_gpu_buf_release(b, NULL, NULL);   // 视图还拿着引用：缓冲要等 close 销毁视图之后才退休
    const double t0 = c89_now_ms();
    r = nvn_gpu_close(5000000000ull, &e);
    const double ms = c89_now_ms() - t0;
    CHECK(OK(r), "C208 close：%d %s", r.code, e.why);
    CHECK(ms < 2000.0, "C208 close 等满了时限：%.0f ms（时限 5000 ms；退休队列清空后 worker 没唤醒 close）", ms);
    printf("C208 close 不等满时限：带着引用已释放缓冲的视图关会话 %.0f ms，已核对\n", ms);
}
static void c207_all(const char *dir) {
    nvn_gpu_err e = {0};
    if (!open_backend()) { CHECK(0, "C207 open"); return; }
    c207_diag_readers(dir);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C207 close：%d %s", e.rc.code, e.why);
    printf("C207 诊断读数互不清零：近 300 帧读数与逐笔读数各自读增量、不清零共享累加器，已核对\n");
}

// C209：管线标签随对象（#1274）。管线预热清单把（槽号, 槽代号）标签存在后端管线对象上（nvn_gpu_pipe_warm_tag / _tag_set），
// 热路径上换到同一条管线时只读标签、不再序列化配方和算散列。这个做法靠两条前提：
//   ① 一局内管线对象与（配方 + 程序）一一对应：缓存键含着色器句柄指针，句柄不按内容合并——同一份 SPIR-V 载入两次是两个句柄，
//      配同一份描述得到两条管线。将来若有人让句柄按内容合并，这里变红：请回头看清单标签的前提（nvn_vk_pipe_warm.h）；
//   ② 一局内管线不淘汰、指针不复用：销毁只在会话收尾整批做；哈希表翻倍重排只搬节点，不换对象。
// 起始 16 桶（用例开关 4）建 C209_N = 700 条互不相同的管线（翻倍 6 次）。建每条时标签必须是 0（新对象不继承旧的），
// 建完立刻写一个互不相同的标签；全部建完再按描述逐条取一遍：取回的是原对象、标签原样读回。
extern uint64_t nvn_gpu_pipe_warm_tag(nvn_gpu_pipe p);
extern void nvn_gpu_pipe_warm_tag_set(nvn_gpu_pipe p, uint64_t tag);
enum { C209_N = 700 };
static uint64_t c209_tag_of(int i) { return ((uint64_t)(unsigned)(i + 1) << 32) | (uint64_t)(unsigned)(i + 1); }
static void c209_pipe_tag(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL, vs2 = NULL, fs2 = NULL; void *vm = NULL, *fm = NULL, *vm2 = NULL, *fm2 = NULL;
    nvn_gpu_pipe_desc base, base2, d, d2; nvn_gpu_pipe p2 = NULL; nvn_gpu_err e = {0};
    nvn_gpu_pipe *pp = calloc(C209_N, sizeof *pp), *srt = calloc(C209_N, sizeof *srt);
    int bad = 0, fresh = 0, dup = 0, miss = 0, lost = 0;
    CHECK(pp && srt, "C209 分配");
    if (!(pp && srt)) goto done;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "C209 读着色器"); goto done; }
    fill_gpipe_desc(&base, vs, fs, 0);
    nvn_vk_dbg_pipe_test_mode(4);
    for (int i = 0; i < C209_N; i++) {
        c205_desc(&base, i, &d);
        if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &pp[i], &e)) || !pp[i]) { bad++; continue; }
        if (nvn_gpu_pipe_warm_tag(pp[i]) != 0) fresh++;
        nvn_gpu_pipe_warm_tag_set(pp[i], c209_tag_of(i));
    }
    nvn_vk_dbg_pipe_test_mode(0);
    CHECK(bad == 0, "C209：建 %d 条管线期望全部成功 → 失败 %d（%s）", C209_N, bad, e.why);
    CHECK(fresh == 0, "C209：新建的管线对象标签期望是 0（不继承别的对象的）→ %d 条不是", fresh);
    memcpy(srt, pp, C209_N * sizeof *pp);
    qsort(srt, C209_N, sizeof *srt, c205_ptr_cmp);
    for (int i = 1; i < C209_N; i++) if (srt[i] == srt[i - 1]) dup++;
    CHECK(dup == 0, "C209：一局内描述不同的管线指针期望互不相同（指针不复用）→ 重复 %d 对", dup);
    for (int i = 0; i < C209_N; i++) {
        nvn_gpu_pipe q = NULL; c205_desc(&base, i, &d);
        if (!OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &q, &e)) || q != pp[i]) miss++;
        else if (nvn_gpu_pipe_warm_tag(q) != c209_tag_of(i)) lost++;
    }
    CHECK(miss == 0 && lost == 0, "C209：翻倍重排后按描述再取一遍期望取回原对象、标签原样读回 → 没取回 %d、标签变了 %d / %d", miss, lost, C209_N);
    // 前提 ①：同一份 SPIR-V 再载入一次得到新句柄，配同一份描述（只换句柄）是另一条管线，标签 0，原来那条的标签不动
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "C209 再读着色器"); goto done; }
    CHECK(vs2 != vs && fs2 != fs, "C209：同一份 SPIR-V 载入两次期望是两个句柄（句柄不按内容合并）→ 顶点 %s、片元 %s", vs2 != vs ? "不同" : "相同", fs2 != fs ? "不同" : "相同");
    fill_gpipe_desc(&base2, vs2, fs2, 0);
    c205_desc(&base2, 0, &d2);
    CHECK(OK(nvn_gpu_pipe_get(&d2, NVN_GPU_PIPE_SYNC, &p2, &e)) && p2 && p2 != pp[0], "C209：只换着色器句柄的同一份描述期望是另一条管线对象（管线按句柄分，不按内容合并）→ %s", e.why);
    CHECK(p2 && nvn_gpu_pipe_warm_tag(p2) == 0, "C209：另一条管线对象的标签期望是 0");
    CHECK(pp[0] && nvn_gpu_pipe_warm_tag(pp[0]) == c209_tag_of(0), "C209：原来那条管线的标签期望不动");
done:
    free(pp); free(srt);
    if (vs) nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    if (vs2) nvn_gpu_shader_release(vs2);
    if (fs2) nvn_gpu_shader_release(fs2);
    free(vm); free(fm); free(vm2); free(fm2);
}
static void c209_all(const char *dir) {
    nvn_gpu_err e = {0};
    if (!open_backend()) { CHECK(0, "C209 open"); return; }
    c209_pipe_tag(dir);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C209 close：%d %s", e.rc.code, e.why);
    printf("C209 管线标签随对象：新对象标签为 0 / 翻倍重排后对象与标签不丢 / 指针不复用 / 同一份 SPIR-V 载入两次不合并句柄，已核对\n");
}

// 上屏 EASU 开关夹具：单独进程跑一次，先关后开各呈现一次。
// 为什么单独：nvn_gpu_surface 是进程级单例，且提交信号序列由 C71 占着；
// 插在大套件里会把 C71 的时间线打成失败终态（实测 signal 7 < 91）。
static int present_upscale_only(void) {
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why) != 0) { fprintf(stderr, "PRESENT_UPSCALE device: %s\n", why); return 2; }
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "present-upscale-only";
    if (!open_backend()) return 1;
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_surface sf = NULL;
    CHECK(OK(nvn_gpu_surface_attach((void *)1, &sf, &e)) && sf, "PRESENT_UPSCALE attach：%s", e.why);
    nvn_gpu_tex tex = make_tex(192, 108, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view view = make_view(tex);
    static uint8_t px[192 * 108 * 4];
    for (int y = 0; y < 108; y++) for (int x = 0; x < 192; x++) {
        uint8_t v = (((x / 12) + (y / 12)) & 1) ? 240 : 10;   // 高对比棋盘：亮边处可专门核
        uint8_t *p = px + (y * 192 + x) * 4; p[0] = p[1] = p[2] = v; p[3] = 255;
    }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, px, 192 * 4, sizeof px);
    CHECK(OK(nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e)), "PRESENT_UPSCALE commit：%s", e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "PRESENT_UPSCALE wait：%s", e.why);
    for (int on = 0; on <= 1; on++) {
        g_upscale_flag = on;
        uint64_t hits0 = s2a_vk_easu_hits();
        nvn_gpu_drawable dr; memset(&dr, 0, sizeof dr);
        nvn_gpu_rc r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
        CHECK(OK(r), "PRESENT_UPSCALE 关=%d acquire：%d %s", on, r.code, e.why);
        if (!OK(r)) break;
        nvn_gpu_present_desc pd; memset(&pd, 0, sizeof pd);
        pd.src = view; pd.filter = 1;   // 192x108 -> 320x180：非整数倍
        r = nvn_gpu_present(sf, &dr, &pd, 100 + (uint64_t)on, on_shown, NULL, &e);
        CHECK(OK(r), "PRESENT_UPSCALE 关=%d present：%d %s", on, r.code, e.why);
        uint64_t delta = s2a_vk_easu_hits() - hits0;
        CHECK(delta == (uint64_t)on, "PRESENT_UPSCALE 关=%d：EASU 次数应为 %d，得 %llu（格式不匹配会回落 blit）", on, on, (unsigned long long)delta);
        printf("PRESENT_UPSCALE 关=%d：present 成功、EASU 次数 %llu\n", on, (unsigned long long)delta);
    }
    // 跨字节序：源 BGRA8、目标是交换链（RGBA8）。EASU 应照样走，写出时换回目标字节序。
    {
        nvn_gpu_tex btex = make_tex(192, 108, NVN_FORMAT_BGRA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_view bview = make_view(btex);
        nvn_gpu_cmd bc = begin_cmd();
        nvn_gpu_tex_upload(bc, btex, 0, 0, NULL, px, 192 * 4, sizeof px);
        CHECK(OK(nvn_gpu_cmd_commit(bc, 2, on_done, NULL, &e)), "PRESENT_UPSCALE BGRA commit：%s", e.why);
        CHECK(OK(nvn_gpu_fence_wait(2, 5000000000ull, &e)), "PRESENT_UPSCALE BGRA wait：%s", e.why);
        g_upscale_flag = 1;
        uint64_t h0 = s2a_vk_easu_hits();
        nvn_gpu_drawable bd; memset(&bd, 0, sizeof bd);
        nvn_gpu_rc ba = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &bd, &e);
        CHECK(OK(ba), "PRESENT_UPSCALE BGRA acquire：%d %s", ba.code, e.why);
        if (OK(ba)) {
            nvn_gpu_present_desc bpd; memset(&bpd, 0, sizeof bpd);
            bpd.src = bview; bpd.filter = 1;
            nvn_gpu_rc br = nvn_gpu_present(sf, &bd, &bpd, 200, on_shown, NULL, &e);
            CHECK(OK(br), "PRESENT_UPSCALE BGRA present：%d %s", br.code, e.why);
            uint64_t dh = s2a_vk_easu_hits() - h0;
            CHECK(dh == 1, "PRESENT_UPSCALE BGRA：跨字节序应走 EASU，得 %llu", (unsigned long long)dh);
            printf("PRESENT_UPSCALE BGRA 源：present 成功、EASU 次数 %llu\n", (unsigned long long)dh);
        }
    // sRGB 源 / UNORM 目标（真机实际组合）：采样线性化、写出编码回 sRGB，应走 EASU。
    {
        nvn_gpu_tex stex = make_tex(192, 108, NVN_FORMAT_RGBA8_SRGB, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_view sview = make_view(stex);
        nvn_gpu_cmd sc = begin_cmd();
        nvn_gpu_tex_upload(sc, stex, 0, 0, NULL, px, 192 * 4, sizeof px);
        CHECK(OK(nvn_gpu_cmd_commit(sc, 3, on_done, NULL, &e)), "PRESENT_UPSCALE SRGB commit：%s", e.why);
        CHECK(OK(nvn_gpu_fence_wait(3, 5000000000ull, &e)), "PRESENT_UPSCALE SRGB wait：%s", e.why);
        g_upscale_flag = 1;
        uint64_t h0 = s2a_vk_easu_hits();
        nvn_gpu_drawable sd; memset(&sd, 0, sizeof sd);
        nvn_gpu_rc sa = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &sd, &e);
        CHECK(OK(sa), "PRESENT_UPSCALE SRGB acquire：%d %s", sa.code, e.why);
        if (OK(sa)) {
            nvn_gpu_present_desc spd; memset(&spd, 0, sizeof spd);
            spd.src = sview; spd.filter = 1;
            nvn_gpu_rc sr = nvn_gpu_present(sf, &sd, &spd, 300, on_shown, NULL, &e);
            CHECK(OK(sr), "PRESENT_UPSCALE SRGB present：%d %s", sr.code, e.why);
            uint64_t dh = s2a_vk_easu_hits() - h0;
            CHECK(dh == 1, "PRESENT_UPSCALE SRGB：sRGB 源应走 EASU，得 %llu", (unsigned long long)dh);
            printf("PRESENT_UPSCALE SRGB 源：present 成功、EASU 次数 %llu\n", (unsigned long long)dh);
        }
        nvn_gpu_view_release(sview); nvn_gpu_tex_release(stex);
    }
        nvn_gpu_view_release(bview); nvn_gpu_tex_release(btex);
    }
    g_upscale_flag = -1;
    nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    CHECK(OK(nvn_gpu_surface_detach(sf, 5000000000ull, &e)), "PRESENT_UPSCALE detach：%s", e.why);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "PRESENT_UPSCALE close：%s", e.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("PRESENT_UPSCALE isolated failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}

// 设备真实支持集合提供独立期望；只检查缺失的档，不能把支持原数当降级证据。
static void c1072_msaa_red(const char *dir) {
    VkImageFormatProperties ip = {0};
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult qr = vkGetPhysicalDeviceImageFormatProperties(g_dev.physical, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &ip);
    CHECK(qr == VK_SUCCESS, "C1072 format query rc=%d", (int)qr);
    if (qr != VK_SUCCESS) return;
    unsigned ran = 0;
    const uint32_t wants[] = {2, 8};
    for (unsigned i = 0; i < 2; i++) {
        uint32_t want = wants[i], expected = want;
        if (ip.sampleCounts & want) continue;
        while (expected > 1 && !(ip.sampleCounts & expected)) expected >>= 1;
        if (!(ip.sampleCounts & expected)) continue;
        nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
        td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
        td.d.depth = td.d.levels = 1; td.d.samples = (int)want; td.usage = NVN_GPU_TEX_RENDER;
        nvn_gpu_tex tex = NULL; nvn_gpu_err err = {0};
        nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &tex, &err);
        CHECK(OK(rc) && tex, "C1072 %ux requested, expect %ux image: %s", want, expected, err.why);
        if (tex) { nvn_gpu_tex_info info = {0}; nvn_gpu_tex_info_get(tex, &info);
            CHECK(info.samples == expected, "C1072 %ux actual=%u expect=%u", want, info.samples, expected);
            nvn_gpu_tex duplicate = NULL; nvn_gpu_rc dup = nvn_gpu_tex_create(&td, &duplicate, &err);
            CHECK(OK(dup) && duplicate, "C1072 repeated %ux creation", want);
            if (duplicate) nvn_gpu_tex_release(duplicate);
            CHECK((want == 2 ? g_msaa_log_2 : g_msaa_log_8) == 1, "C1072 %ux fallback is logged once per format/request", want);
            void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
            int shaders = load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm);
            CHECK(shaders, "C1072 shader pair");
            if (shaders) {
                nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
                pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
                pd.samples = want; pd.prim = 5; pd.chmask.mask = 0xf; pd.color.logic_op = 3;
                pd.attr[0].fmt = pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.stream[0].stride = 32;
                nvn_gpu_pipe pipe = NULL; CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err)) && pipe,
                                                "C1072 %ux pipe follows image: %s", want, err.why);
                if (pipe) {
                    nvn_gpu_pipe cached = NULL;
                    CHECK(nvn_gpu_pipe_lookup(&pd, &cached) && cached == pipe, "C1072 %ux lookup preserves requested identity", want);
                    float vertices[4][8] = {{-1,-1,0,1,1,0,0,1},{-1,1,0,1,1,0,0,1},{1,-1,0,1,1,0,0,1},{1,1,0,1,1,0,0,1}};
                    nvn_gpu_buf vb = make_buf(sizeof vertices, NVN_GPU_MEM_SHARED, vertices);
                    nvn_gpu_tex dest = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
                    nvn_gpu_view av = make_view(tex), bv = make_view(dest); nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
                    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.w = pass.h = 4; pass.layers = 1;
                    pass.color[0].view = av; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
                    nvn_gpu_pass_begin(c, &pass); nvn_gpu_set_pipe(c, pipe);
                    nvn_gpu_dyn dyn = {0}; dyn.origin_ll = pd.origin_ll; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1;
                    dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
                    nvn_gpu_draw_args da = {0}; da.prim = 5; da.count = 4; da.instances = 1; nvn_gpu_draw(c, &da); nvn_gpu_pass_end(c);
                    NVNcopyRegion rr = {0, 0, 0, 4, 4, 1}; nvn_gpu_blit(c, av, &rr, bv, &rr, 0);
                    nvn_gpu_copy_tex_buf(c, dest, 0, 0, NULL, rb, 0, 16, 64);
                    uint64_t fence = i + 1; nvn_gpu_rc cr = nvn_gpu_cmd_commit(c, fence, on_done, NULL, &err);
                    CHECK(OK(cr), "C1072 %ux draw and resolve/copy commit: %s", want, err.why);
                    if (OK(cr)) { CHECK(OK(nvn_gpu_fence_wait(fence, 5000000000ull, &err)), "C1072 fence: %s", err.why);
                        const uint8_t *px = buf_cpu(rb); int badpx = 0;
                        for (int j = 0; j < 16; j++) if (px[4*j] != 255 || px[4*j+1] || px[4*j+2] || px[4*j+3] != 255) badpx++;
                        CHECK(!badpx, "C1072 %ux resolved pixels expect 16 red, bad=%d", want, badpx); }
                    nvn_gpu_view_release(av); nvn_gpu_view_release(bv); nvn_gpu_tex_release(dest);
                    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_buf_release(vb, NULL, NULL);
                }
                nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
            }
            free(vsm); free(fsm);
            nvn_gpu_tex_release(tex); }
        printf("C1072 request=%u expect=%u support=%#x\n", want, expected, (unsigned)ip.sampleCounts);
        ran++;
    }
    // 宿主缺哪档就练哪档：有的 Mesa 版本 lavapipe 缺 2x 与 8x（练两档），CI 镜像的 lavapipe 报 1/4/8x（只缺 2x、练一档）。
    // 两档都支持的宿主练不到降档：照样报红，不静默跳过（换缺档的宿主或模拟档）。
    CHECK(ran >= 1, "C1072 requires host missing 2x or 8x; exercised=%u", ran);
    VkImageFormatProperties r8 = {0};
    VkResult r8rc = vkGetPhysicalDeviceImageFormatProperties(g_dev.physical, VK_FORMAT_R8_UNORM,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &r8);
    if (r8rc == VK_SUCCESS && (r8.sampleCounts & VK_SAMPLE_COUNT_1_BIT) && !(r8.sampleCounts & VK_SAMPLE_COUNT_2_BIT)) {
        nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
        td.d.format = NVN_FORMAT_R8; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
        td.d.samples = 2; td.usage = NVN_GPU_TEX_RENDER;
        nvn_gpu_tex first = NULL, second = NULL; nvn_gpu_err err = {0};
        CHECK(OK(nvn_gpu_tex_create(&td, &first, &err)) && first, "C1072 R8 2x fallback: %s", err.why);
        CHECK(OK(nvn_gpu_tex_create(&td, &second, &err)) && second, "C1072 repeated R8 2x fallback: %s", err.why);
        CHECK(g_msaa_log_2 == 2, "C1072 distinct formats each log once for 2x, count=%u", g_msaa_log_2);
        if (first) nvn_gpu_tex_release(first);
        if (second) nvn_gpu_tex_release(second);
        printf("C1072 diagnostic distinct formats: count=%u\n", g_msaa_log_2);
    } else printf("C1072 diagnostic R8 case unavailable, mask=%#x rc=%d\n", (unsigned)r8.sampleCounts, (int)r8rc);
}

// 真实 SPIR-V 声明 sampler2DMS；降为单采样后必须在绑定前拒绝，不能向驱动发错类型的图。
static void c1072_sample_guard(const char *dir) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
    td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1; td.d.samples = 2;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_tex tex = NULL;
    CHECK(OK(nvn_gpu_tex_create(&td, &tex, &e)) && tex, "C1072 sampled target: %s", e.why);
    if (!tex) return;
    nvn_gpu_tex_info info = {0}; nvn_gpu_tex_info_get(tex, &info);
    CHECK(info.samples == 1, "C1072 sampled target must be downgraded to 1x");
    nvn_gpu_layout_ent ent = {0}; ent.cls = NVN_GPU_RES_TEX_SAMP; ent.count = 1;
    ent.access = NVN_GPU_ACC_R; ent.stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl = {0}; fl.ent = &ent; fl.nent = 1;
    nvn_gpu_shader vs = NULL, fs = NULL; void *vsm = NULL, *fsm = NULL;
    int ok = load_shader_pair(dir, "quad.vert", "msaa_read.frag", NULL, &fl, &vs, &fs, &vsm, &fsm);
    CHECK(ok, "C1072 real multisample shader loads");
    if (ok) {
        nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL;
        pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
        pd.samples = 1; pd.prim = 5; pd.chmask.mask = 0xf; pd.color.logic_op = 3;
        pd.attr[0].fmt = pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.stream[0].stride = 32;
        nvn_gpu_pipe pipe = NULL; CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)) && pipe, "C1072 sample pipe: %s", e.why);
        if (pipe) {
            nvn_gpu_tex out = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
            nvn_gpu_view ov = make_view(out), tv = make_view(tex); samp_par sp = {0}; nvn_gpu_samp samp = NULL;
            CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)) && samp, "C1072 sampler: %s", e.why);
            nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.w = pass.h = 4; pass.layers = 1;
            pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(c, &pass); nvn_gpu_set_pipe(c, pipe);
            nvn_gpu_binding b = {0}; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = tv; b.u.ts.samp = samp;
            nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
            nvn_gpu_rc cr = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
            CHECK(cr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "msaa_sampled_single"),
                  "C1072 single sample must reject MS shader before draw: rc=%d %s", cr.code, e.why);
            if (cr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "msaa_sampled_single"))
                printf("C1072 guard sampled: rejected before draw\n");
            nvn_gpu_samp_release(samp); nvn_gpu_view_release(tv); nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
        }
        nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    }
    free(vsm); free(fsm); nvn_gpu_tex_release(tex);
}

static void c1072_attachment_guard(void) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
    td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1; td.d.samples = 2; td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex a = NULL, b = NULL; nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_tex_create(&td, &a, &e)) && a, "C1072 1x attachment: %s", e.why);
    td.d.samples = 4; CHECK(OK(nvn_gpu_tex_create(&td, &b, &e)) && b, "C1072 4x attachment: %s", e.why);
    if (a && b) {
        nvn_gpu_view av = make_view(a), bv = make_view(b); nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc pass = {0}; pass.ncolor = 2; pass.w = pass.h = 4; pass.layers = 1;
        pass.color[0].view = av; pass.color[1].view = bv;
        nvn_gpu_pass_begin(c, &pass);
        nvn_gpu_rc cr = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
        CHECK(cr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "msaa_attachment_mismatch"),
              "C1072 inconsistent attachments rejected before renderpass: rc=%d %s", cr.code, e.why);
        if (cr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "msaa_attachment_mismatch"))
            printf("C1072 guard attachments: rejected before renderpass\n");
        nvn_gpu_view_release(av); nvn_gpu_view_release(bv);
    }
    if (a) nvn_gpu_tex_release(a);
    if (b) nvn_gpu_tex_release(b);
}

static void c1072_pipe_guard(const char *dir) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
    td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1; td.d.samples = 4; td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex = NULL; nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_tex_create(&td, &tex, &e)) && tex, "C1072 4x pipe-guard attachment: %s", e.why);
    if (!tex) return;
    nvn_gpu_shader vs = NULL, fs = NULL; void *vsm = NULL, *fsm = NULL;
    int ok = load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm);
    CHECK(ok, "C1072 pipe-guard shaders");
    if (ok) {
        nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL;
        pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
        pd.samples = 1; pd.prim = 5; pd.chmask.mask = 0xf; pd.color.logic_op = 3;
        pd.attr[0].fmt = pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.stream[0].stride = 32;
        nvn_gpu_pipe pipe = NULL; CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)) && pipe, "C1072 pipe-guard pipe: %s", e.why);
        if (pipe) {
            nvn_gpu_view view = make_view(tex); nvn_gpu_cmd c = begin_cmd();
            nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.w = pass.h = 4; pass.layers = 1; pass.color[0].view = view;
            nvn_gpu_pass_begin(c, &pass); nvn_gpu_set_pipe(c, pipe);
            nvn_gpu_rc cr = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
            CHECK(cr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "msaa_pipeline_mismatch"),
                  "C1072 pipeline mismatch rejected before draw: rc=%d %s", cr.code, e.why);
            if (cr.code == NVN_GPU_E_UNSUPPORTED && strstr(e.why, "msaa_pipeline_mismatch"))
                printf("C1072 guard pipeline: rejected before draw\n");
            nvn_gpu_view_release(view);
        }
        nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    }
    free(vsm); free(fsm); nvn_gpu_tex_release(tex);
}

uint64_t nvn_gpu_vk_test_deswizzle_count(void);
#ifdef S2A_GPU_DESWIZZLE_TEST
#include "../../../graphics/nvn/gfx/tex/nvn_deswizzle.h"
#endif
// 手算 8x8 RGBA8 的 GOB 内地址，真实 Vulkan 贴图读回按原像素字节核对。
static void gpu_deswizzle_basic(void) {
    nvn_gpu_tex_desc d = {0}; d.d.target = NVN_TEXTURE_TARGET_2D; d.d.format = NVN_FORMAT_RGBA8;
    d.d.width = d.d.height = 8; d.d.depth = d.d.levels = 1; d.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_tex t = NULL; nvn_gpu_rc r = nvn_gpu_tex_create(&d, &t, &e);
    CHECK(OK(r) && t, "GPU deswizzle texture create: %d %s", r.code, e.why); if (!t) return;
    uint8_t raw[512] = {0}, expected[256];
    for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 32; x++) {
        uint8_t value = (uint8_t)(1 + x * 7 + y * 31);
        unsigned offset = (y / 2) * 64 + (x / 16) * 32 + (y % 2) * 16 + x % 16;
        raw[offset] = value; expected[y * 32 + x] = value;
    }
    nvn_texlayout layout = { .size = 512, .layer_size = 512, .align = 512, .layers = 1, .bpb = 4 };
    nvn_gpu_buf rb = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    uint64_t before = nvn_gpu_vk_test_deswizzle_count();
    nvn_gpu_tex_upload_blocklinear(c, t, 0, 0, &layout, raw, sizeof raw);
    CHECK(nvn_gpu_vk_test_deswizzle_count() == before + 1, "GPU deswizzle used the compute path");
    memset(raw, 0, sizeof raw);   // API returns only after its own raw snapshot is safe.
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 32, sizeof expected);
    r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "GPU deswizzle upload commit: %d %s", r.code, e.why);
    if (OK(r)) { r = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(r), "GPU deswizzle fence: %d %s", r.code, e.why);
        if (OK(r)) CHECK(!memcmp(buf_cpu(rb), expected, sizeof expected), "GPU deswizzle RGBA8 GOB readback"); }
    nvn_gpu_tex_release(t); nvn_gpu_buf_release(rb, NULL, NULL);
}

#ifdef S2A_GPU_DESWIZZLE_TEST
uint64_t nvn_gpu_vk_test_deswizzle_fallback_count(void);
static void gpu_deswizzle_depth_fallback(void) {
    nvn_gpu_tex_desc desc = {0}; desc.d.target = NVN_TEXTURE_TARGET_2D; desc.d.format = NVN_FORMAT_DEPTH16;
    desc.d.width = desc.d.height = 8; desc.d.depth = desc.d.levels = 1; desc.usage = NVN_GPU_TEX_SAMPLED;
    nvn_texlayout L; nvn_tex_layout(&desc.d, &L);
    uint8_t *raw = malloc(L.size), expected[128]; CHECK(raw != NULL, "depth fallback source"); if (!raw) return;
    for (uint64_t i = 0; i < L.size; i++) raw[i] = (uint8_t)(i * 29 + (i >> 8) * 23);
    CHECK(!nvn_deswizzle(&desc.d, 0, 0, raw, expected, 16), "depth fallback CPU reference");
    nvn_gpu_tex t = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&desc, &t, &e);
    CHECK(OK(rc) && t, "depth fallback texture: %s", e.why);
    if (t) {
        nvn_gpu_buf rb = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL); nvn_gpu_cmd c = begin_cmd();
        uint64_t before = nvn_gpu_vk_test_deswizzle_fallback_count();
        nvn_gpu_tex_upload_blocklinear(c, t, 0, 0, &L, raw, L.size);
        CHECK(nvn_gpu_vk_test_deswizzle_fallback_count() == before + 1, "depth fallback was selected");
        nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 16, sizeof expected);
        rc = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e); CHECK(OK(rc), "depth fallback commit: %d %s", rc.code, e.why);
        if (OK(rc)) { rc = nvn_gpu_fence_wait(2, 5000000000ull, &e); CHECK(OK(rc), "depth fallback fence");
            if (OK(rc)) CHECK(!memcmp(buf_cpu(rb), expected, sizeof expected), "depth fallback byte readback"); }
        nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_tex_release(t);
    }
    free(raw);
}
// 内部解排要恢复调用者的计算管线、描述符和 push 数据；第二次派发前不再重绑。
static void gpu_deswizzle_compute_state(const char *dir) {
    nvn_gpu_layout_ent ent = {0}; ent.set = 0; ent.cls = NVN_GPU_RES_SSBO; ent.access = NVN_GPU_ACC_W;
    ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout layout = {0}; layout.ent = &ent; layout.nent = 1; layout.push_bytes = 4;
    nvn_gpu_shader sh = NULL; nvn_gpu_pipe pipe = mk_cpipe(dir, "pushc.comp", &layout, &sh);
    CHECK(pipe != NULL, "GPU deswizzle compute-state pipeline"); if (!pipe) return;
    nvn_gpu_buf result = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_tex_desc desc = {0}; desc.d.target = NVN_TEXTURE_TARGET_2D; desc.d.format = NVN_FORMAT_RGBA8;
    desc.d.width = desc.d.height = 8; desc.d.depth = desc.d.levels = 1; desc.usage = NVN_GPU_TEX_SAMPLED;
    nvn_texlayout L; nvn_tex_layout(&desc.d, &L); uint8_t raw[512] = {0};
    nvn_gpu_tex t = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&desc, &t, &e);
    CHECK(OK(rc) && t, "GPU deswizzle compute-state texture %s", e.why);
    if (t) {
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_binding b = {0}; b.ent = 0; b.cls = NVN_GPU_RES_SSBO;
        b.u.b.buf = result; b.u.b.len = 64 * 4;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
        uint32_t value = 7; nvn_gpu_push(c, NVN_GPU_SH_CS, 0, &value, sizeof value);
        nvn_gpu_dispatch(c, pipe, 1, 1, 1, NULL, 0);
        nvn_gpu_fill_buf(c, result, 0, 64 * 4, 0); nvn_gpu_barrier(c, NVN_BARRIER_INVALIDATE_SHADER_BIT);
        uint64_t before = nvn_gpu_vk_test_deswizzle_count();
        nvn_gpu_tex_upload_blocklinear(c, t, 0, 0, &L, raw, sizeof raw);
        CHECK(nvn_gpu_vk_test_deswizzle_count() == before + 1, "GPU deswizzle compute-state used internal pipeline");
        nvn_gpu_dispatch(c, pipe, 1, 1, 1, NULL, 0);
        rc = nvn_gpu_cmd_commit(c, 1000, on_done, NULL, &e); CHECK(OK(rc), "GPU deswizzle compute-state commit %d %s", rc.code, e.why);
        if (OK(rc)) { rc = nvn_gpu_fence_wait(1000, 5000000000ull, &e); CHECK(OK(rc), "GPU deswizzle compute-state wait");
            if (OK(rc)) { const uint32_t *v = (const uint32_t *)buf_cpu(result); unsigned bad = 0; for (unsigned i = 0; i < 64; i++) bad += v[i] != 7 + i;
                CHECK(!bad, "GPU deswizzle compute-state restored: bad=%u", bad); } }
        nvn_gpu_tex_release(t);
    }
    nvn_gpu_buf_release(result, NULL, NULL); nvn_gpu_shader_release(sh);
}
// Raw guest bytes are independent input; the CPU reference computes linear bytes
// for each mip/layer, while the actual Vulkan image is copied back after dispatch.
static void gpu_deswizzle_matrix(void) {
    const struct { int format, target; unsigned w, h, depth, levels; int packaged; } cases[] = {
        {NVN_FORMAT_R8, NVN_TEXTURE_TARGET_2D, 13, 11, 1, 3, 0},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_2D, 37, 21, 1, 4, 0x204},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_2D, 37, 25, 1, 2, 1},  // 多 GOB 列、块高 1、多个块行：行步长负控可见
        {NVN_FORMAT_RG8, NVN_TEXTURE_TARGET_2D_ARRAY, 17, 9, 3, 3, 0},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_3D, 9, 7, 4, 3, 0x23},
        {NVN_FORMAT_RGBA_DXT1, NVN_TEXTURE_TARGET_2D, 20, 12, 1, 3, 0},
    };
    uint64_t signal = 2; unsigned count = 0;
    for (unsigned gob2 = 0; gob2 < 2; gob2++) {
        nvn_gob_switch2 = (int)gob2;
        for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            nvn_gpu_tex_desc desc = {0}; desc.d.format = cases[ci].format; desc.d.target = cases[ci].target;
            desc.d.width = cases[ci].w; desc.d.height = cases[ci].h; desc.d.depth = cases[ci].depth;
            desc.d.levels = cases[ci].levels; desc.d.pkg_layout = cases[ci].packaged; desc.usage = NVN_GPU_TEX_SAMPLED;
            nvn_texlayout L; nvn_tex_layout(&desc.d, &L);
            uint8_t *raw = malloc((size_t)L.size); CHECK(raw != NULL, "GPU matrix raw allocate"); if (!raw) continue;
            // 不按 256 / 512 字节重复：错 GOB、高度或行宽必须改变可见值。
            for (uint64_t i = 0; i < L.size; i++) raw[i] = (uint8_t)(i * 31u + (i >> 8) * 37u + (i >> 16) * 101u + ci * 13u + gob2 * 7u + 1u);
            nvn_gpu_tex t = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&desc, &t, &e);
            CHECK(OK(rc) && t, "GPU matrix texture %u gob2=%u: %d %s", ci, gob2, rc.code, e.why);
            if (!t) { free(raw); continue; }
            for (unsigned l = 0; l < cases[ci].levels; l++) for (unsigned y = 0; y < L.layers; y++) {
                uint32_t row, rows, slices, bpb; uint64_t base; unsigned h, d;
                int geom = nvn_deswizzle_geom(&desc.d, l, y, &row, &rows, &slices, &base, &h, &d, &bpb);
                CHECK(!geom, "GPU matrix CPU geometry %u/%u/%u", ci, l, y); if (geom) continue;
                size_t n = (size_t)row * rows * slices; uint8_t *expected = malloc(n);
                CHECK(expected && !nvn_deswizzle(&desc.d, l, y, raw, expected, row), "GPU matrix CPU reference");
                if (!expected) continue;
                nvn_gpu_buf rb = make_buf(n, NVN_GPU_MEM_SHARED, NULL); nvn_gpu_cmd c = begin_cmd();
                uint64_t before = nvn_gpu_vk_test_deswizzle_count();
                nvn_gpu_tex_upload_blocklinear(c, t, l, y, &L, raw, L.size);
                CHECK(nvn_gpu_vk_test_deswizzle_count() == before + 1, "GPU matrix actual compute %u/%u/%u gob2=%u", ci, l, y, gob2);
                unsigned w = desc.d.width >> l, hh = desc.d.height >> l; if (!w) w = 1; if (!hh) hh = 1;
                NVNcopyRegion region = {0, 0, 0, (int)w, (int)hh, (int)slices};
                nvn_gpu_copy_tex_buf(c, t, l, y, &region, rb, 0, row, n);
                rc = nvn_gpu_cmd_commit(c, ++signal, on_done, NULL, &e);
                CHECK(OK(rc), "GPU matrix commit %u/%u/%u gob2=%u rc=%d %s", ci, l, y, gob2, rc.code, e.why);
                if (OK(rc)) { rc = nvn_gpu_fence_wait(signal, 5000000000ull, &e); CHECK(OK(rc), "GPU matrix fence");
                    if (OK(rc)) CHECK(!memcmp(buf_cpu(rb), expected, n), "GPU matrix byte mismatch %u/%u/%u gob2=%u", ci, l, y, gob2); }
                nvn_gpu_buf_release(rb, NULL, NULL); free(expected); count++;
            }
            nvn_gpu_tex_release(t); free(raw);
        }
    }
    nvn_gob_switch2 = 0;
    printf("GPU_DESWIZZLE matrix-subresources=%u\n", count);

    // 逐格式走真实 Vulkan 图片；设备没提供的格式、需展开或深度模板分别计数，不能冒充对拍通过。
    unsigned formats = 0, unavailable = 0, expanded = 0, depth = 0, decoded_bc = 0;
    for (int f = 1; f <= NVN_FMT_MAX; f++) {
        nvn_fmt_info fi;
        if (!nvn_fmt_info_of(f, &fi)) continue;
        if (fi.depth || fi.stencil) { depth++; continue; }
        if (fi.expand) { expanded++; continue; }
        if (!(nvn_gpu_format_caps(f) & NVN_GPU_FMT_SAMPLE)) { unavailable++; continue; }
        if (f >= 0x41 && f <= 0x50 && (g_dev.bc_decode_mask & (1u << (f - 0x41)))) { decoded_bc++; continue; }
        nvn_gpu_tex_desc desc = {0}; desc.d.format = f; desc.d.target = NVN_TEXTURE_TARGET_2D;
        desc.d.width = desc.d.height = 24; desc.d.depth = desc.d.levels = 1; desc.usage = NVN_GPU_TEX_SAMPLED;
        nvn_texlayout L; nvn_tex_layout(&desc.d, &L);
        uint32_t row, rows, slices, bpb; uint64_t base; unsigned h, d;
        int geom = nvn_deswizzle_geom(&desc.d, 0, 0, &row, &rows, &slices, &base, &h, &d, &bpb);
        CHECK(!geom && L.size, "GPU format geometry 0x%x", f); if (geom || !L.size) continue;
        size_t n = (size_t)row * rows * slices;
        uint8_t *raw = malloc((size_t)L.size), *expected = malloc(n);
        CHECK(raw && expected, "GPU format allocation 0x%x", f);
        if (!raw || !expected) { free(raw); free(expected); continue; }
        for (uint64_t i = 0; i < L.size; i++) raw[i] = (uint8_t)(i * 31u + (i >> 8) * 37u + (i >> 16) * 101u + f * 13u);
        CHECK(!nvn_deswizzle(&desc.d, 0, 0, raw, expected, row), "GPU format CPU reference 0x%x", f);
        nvn_gpu_tex t = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&desc, &t, &e);
        CHECK(OK(rc) && t, "GPU format advertised but cannot create 0x%x: %d %s", f, rc.code, e.why);
        if (t) {
            nvn_gpu_buf rb = make_buf(n, NVN_GPU_MEM_SHARED, NULL); nvn_gpu_cmd c = begin_cmd();
            uint64_t before = nvn_gpu_vk_test_deswizzle_count();
            nvn_gpu_tex_upload_blocklinear(c, t, 0, 0, &L, raw, L.size);
            CHECK(nvn_gpu_vk_test_deswizzle_count() == before + 1, "GPU format compute 0x%x", f);
            nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, row, n);
            rc = nvn_gpu_cmd_commit(c, ++signal, on_done, NULL, &e);
            CHECK(OK(rc), "GPU format commit 0x%x: %d %s", f, rc.code, e.why);
            if (OK(rc)) { rc = nvn_gpu_fence_wait(signal, 5000000000ull, &e); CHECK(OK(rc), "GPU format fence 0x%x", f);
                if (OK(rc)) CHECK(!memcmp(buf_cpu(rb), expected, n), "GPU format byte mismatch 0x%x", f); }
            nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_tex_release(t); formats++;
        }
        free(raw); free(expected);
    }
    printf("GPU_DESWIZZLE formats=%u unavailable=%u expanded=%u depth_stencil=%u decoded_bc=%u\n", formats, unavailable, expanded, depth, decoded_bc);
}
#endif

#include "t_clear_capacity.inc"

#ifdef S2A_LAND_READBACK_TEST
void nvn_gpu_vk_test_land_mode(int on);
uint64_t nvn_gpu_vk_test_land_eligible_bytes(nvn_gpu_cmd c);
void nvn_gpu_vk_test_land_gate(VkResult (*gate)(uint64_t));
static atomic_int land_gate_release;
static int land_force_failure;

static VkResult land_wait_for_later_gpu(uint64_t signal) {
    if (signal != 2) return VK_SUCCESS;
    for (int i = 0; i < 500 && !atomic_load(&land_gate_release); i++) sleep_ms(10);
    if (!atomic_load(&land_gate_release)) return VK_ERROR_UNKNOWN;
    return land_force_failure ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}

struct land_result {
    atomic_int called;
    uint32_t status;
    uint64_t bytes;
    uint8_t data[56];
};
static void land_done(void *user, const nvn_gpu_done *done) {
    struct land_result *result = user;
    result->status = done->status;
    result->bytes = done->readback_bytes;
    if (done->readback && done->readback_bytes == sizeof result->data)
        memcpy(result->data, done->readback, sizeof result->data);
    atomic_store(&result->called, 1);
}

static void test_land_readback(int fail_invalidate, int automatic) {
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "land-readback";
    g_land_test_on = automatic ? 2 : 1;
    if (!open_backend()) {
        g_land_test_on = 0;
        return;
    }
    nvn_gpu_err err = {0};
    nvn_gpu_buf mirror = make_buf(128, NVN_GPU_MEM_SHARED, NULL);
    struct land_result a_result = {0}, b_result = {0};
    int enabled = nvn_gpu_readback_enabled();
    if (automatic) {
        CHECK(!enabled, "auto starts with forced path disabled");
        // Force an eligible candidate even on a software ICD with only cached mappings.
        nvn_gpu_vk_test_land_gate(land_wait_for_later_gpu);
        nvn_gpu_cmd r0 = begin_cmd();
        nvn_gpu_readback_span probe = {mirror, 5, 19, UINT64_MAX};
        CHECK(nvn_gpu_readback_cmd_enabled(r0), "auto R0 must collect candidate ranges");
        CHECK(nvn_gpu_readback_record(r0, &probe, 1) == 0 &&
              nvn_gpu_vk_test_land_eligible_bytes(r0) == 19,
              "auto R0 must observe 19 eligible bytes without recording a copy");
        nvn_gpu_vk_test_land_mode(1);
        probe.bytes = 37;
        CHECK(nvn_gpu_readback_record(r0, &probe, 1) == 0 &&
              nvn_gpu_vk_test_land_eligible_bytes(r0) == 37,
              "open R0 must still observe 37 eligible bytes without staging after a switch to R1");
        nvn_gpu_cmd_abort(r0);
        nvn_gpu_vk_test_land_gate(NULL);
        enabled=1;
    }
    CHECK(enabled, "land: no cached readback capability on test ICD");
    if (mirror && enabled) {
        CHECK(OK(nvn_gpu_cmd_commit(begin_cmd(), 1, on_done, NULL, &err)), "land: F-1 submit %s", err.why);
        CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &err)), "land: F-1 initially completed");

        nvn_gpu_cmd a = begin_cmd();
        nvn_gpu_fill_buf(a, mirror, 0, 128, 0x11111111u);
        // 两个同一源的不相邻字节范围，第二个目标起点不对齐缓存粒度。
        nvn_gpu_readback_span spans[] = {{mirror, 5, 37, UINT64_MAX}, {mirror, 73, 19, UINT64_MAX}};
        land_force_failure = fail_invalidate;
        atomic_store(&land_gate_release, 0);
        nvn_gpu_vk_test_land_gate(land_wait_for_later_gpu);
        if (automatic) nvn_gpu_vk_test_land_mode(0);
        CHECK(nvn_gpu_readback_record(a, spans, 2) == 56 &&
              spans[0].readback_offset == 0 && spans[1].readback_offset == 37,
              "land: packed copy plan must preserve both source ranges");
        CHECK(OK(nvn_gpu_cmd_commit(a, 2, land_done, &a_result, &err)), "land: A submit %s", err.why);

        nvn_gpu_cmd b = begin_cmd();
        if (automatic) CHECK(!nvn_gpu_readback_record(b, spans, 2), "new R0 segment must use mirror");
        nvn_gpu_fill_buf(b, mirror, 0, 128, 0x22222222u);
        CHECK(OK(nvn_gpu_cmd_commit(b, 3, land_done, &b_result, &err)), "land: B submit %s", err.why);
        // A 的 CPU 完成线程仍堵在 gate。直接排队一个 Vulkan 栅栏，证明 B 的真实 GPU 工作已经结束。
        VkFence fence = VK_NULL_HANDLE;
        const VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkResult status = vkCreateFence(g_dev.device, &info, NULL, &fence);
        if (status == VK_SUCCESS) status = vkQueueSubmit(g_dev.queue, 1, &submit, fence);
        if (status == VK_SUCCESS) status = vkWaitForFences(g_dev.device, 1, &fence, VK_TRUE, 4000000000ull);
        CHECK(status == VK_SUCCESS, "land: native fence behind B: %d", status);
        CHECK(!atomic_load(&a_result.called) && !atomic_load(&b_result.called), "land: software completion ran before release");
        if (status == VK_SUCCESS) {
            const uint8_t *mapped = nvn_gpu_buf_cpu(mirror);
            CHECK(mapped && mapped[5] == 0x22 && mapped[73] == 0x22, "land: B did not overwrite the source mirror");
        }
        atomic_store(&land_gate_release, 1);
        const int wait_rc = nvn_gpu_fence_wait(2, 5000000000ull, &err).code;
        CHECK(wait_rc == (fail_invalidate ? NVN_GPU_E_FAILED : NVN_GPU_OK), "land: waiting A got %d", wait_rc);
        const int b_rc = nvn_gpu_fence_wait(3, 5000000000ull, &err).code;
        CHECK(b_rc == (fail_invalidate ? NVN_GPU_E_FAILED : NVN_GPU_OK), "land: waiting B got %d", b_rc);
        CHECK(nvn_gpu_fence_wait(1, 0, &err).code == NVN_GPU_OK, "land: failed A hid the prior success");
        CHECK(nvn_gpu_fence_wait(2, 0, &err).code == wait_rc, "land: query A after completion disagrees");
        CHECK(nvn_gpu_fence_failed() == (fail_invalidate ? 2u : 0u), "land: wrong first failure value");
        if (fence && status == VK_SUCCESS) vkDestroyFence(g_dev.device, fence, NULL);
    }
    if (mirror) nvn_gpu_buf_release(mirror, NULL, NULL);
    const nvn_gpu_rc closed = nvn_gpu_close(5000000000ull, &err);
    CHECK(OK(closed), "land: close %s", err.why);
    if (mirror && enabled && OK(closed)) {
        const uint32_t want = fail_invalidate ? NVN_GPU_DONE_FAILED : NVN_GPU_DONE_OK;
        CHECK(atomic_load(&a_result.called) && a_result.status == want, "land: wrong A callback result");
        CHECK(atomic_load(&b_result.called) && b_result.status == want, "land: wrong B callback result");
        CHECK(a_result.bytes == (fail_invalidate ? 0u : 56u), "land: wrong A borrowed length");
        if (!fail_invalidate) {
            uint8_t expected[sizeof a_result.data];
            memset(expected, 0x11, sizeof expected);
            CHECK(!memcmp(expected, a_result.data, sizeof expected), "land: retained data must be A's writes, never B's mirror");
        }
    }
    nvn_gpu_vk_test_land_gate(NULL);
    g_land_test_on = 0;
}
#endif

#ifdef S2A_ROI_FIXTURE
#include "t_roi_probe.inc"
#endif

int main(int argc, char **argv) {
#ifdef S2A_ROI_FIXTURE
    if (argc == 4 && !strcmp(argv[1], "--roi-probe")) return roi_probe_only(atoi(argv[2]),argv[3]);
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);   // 经 tee 的管道默认整块缓冲：超时被杀时最后一行不是卡点；按行刷新才看得出卡在哪
    g_memo_on = getenv("S2A_TEST_DESC_MEMO") ? atoi(getenv("S2A_TEST_DESC_MEMO")) != 0 : 1;   // 备忘这一遍开 / 关：没设 = 生产缺省（开）
    if (getenv("S2A_TEST_COUNT_ONLY")) return c658_main(argc, argv);
    if (getenv("S2A_TEST_ERROR697_ONLY")) return error697_only();
    if (getenv("S2A_TEST_PRESENT_UPSCALE_ONLY")) return present_upscale_only();
    if (!getenv("S2A_TEST_LOGICOP_ONLY")) t29_format_expand();
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why) != 0) {
        printf("SKIP? 建 Vulkan 设备失败：%s\n", why);
        return 2;   // 不静默跳过：run.sh 把 2 当红
    }
    if (argc > 5 && !strcmp(argv[2], "--c89-child")) return c89_child_main(argv);   // C89 里被 SIGKILL 的那一局：由 c89_midrun_save 起
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
#ifdef S2A_BACKEND_PIPE_CALL_TEST
    if (argc > 2 && !strcmp(argv[2], "--pipe-call-lifetime")) {
        pipe_call_lifetime_test();
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
#endif
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
    if (argc > 2 && !strcmp(argv[2], "--pipe-creation-feedback")) {
        g_desc.host_device = &g_dev;
        g_desc.app_label = "pipe-creation-feedback-test";
        creation_feedback_test(argv[1]);
        s2a_vk_device_destroy(&g_dev, 1);
        printf("PSO feedback failures=%d\n", g_fail);
        return g_fail ? 1 : 0;
    }
#endif
    if (argc > 2 && !strcmp(argv[2], "--high-binding")) {   // 只跑绑定号 ≥ 256 的布局用例（默认档里也跑）；给红 / 绿取证用
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "high-binding-test";
        t_high_binding(argv[1]);
        s2a_vk_device_destroy(&g_dev, 1);
        printf("HIGH_BINDING failures=%d\n", g_fail);
        return g_fail ? 1 : 0;
    }
#ifdef S2A_LAND_READBACK_TEST
    if (argc > 2 && !strcmp(argv[2], "--land-readback")) {
        test_land_readback(0,0);
        test_land_readback(1,0);
        test_land_readback(0,1);
        test_land_readback(1,1);
        printf("LAND_READBACK valid and invalidated paths: failures=%d\n", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
#endif
    if (argc > 2 && !strcmp(argv[2], "--clear-capacity")) {
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "clear-capacity-test";
        c1501_clear_capacity(argv[1]);
        s2a_vk_device_destroy(&g_dev, 1);
        printf("C1501 clear capacity failures=%d\n", g_fail);
        return g_fail ? 1 : 0;
    }
#ifdef S2A_CMDPOOL_TEST
    if (argc > 2 && !strcmp(argv[2], "--cmdpool-only")) {
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c1581_cmdpool();
        printf("C1581 isolated failures=%d\n", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
#endif
    if (argc > 2 && !strcmp(argv[2], "--msaa-only")) {
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        if (!open_backend()) return 1;
        c1072_msaa_red(argv[1]);
        nvn_gpu_err e = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1072 close %s", e.why);
        if (open_backend()) { c1072_sample_guard(argv[1]); CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1072 sampled close %s", e.why); }
        else CHECK(0, "C1072 reopen for sample guard");
        if (open_backend()) { c1072_attachment_guard(); CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1072 attachment close %s", e.why); }
        else CHECK(0, "C1072 reopen for attachment guard");
        if (open_backend()) { c1072_pipe_guard(argv[1]); CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1072 pipe close %s", e.why); }
        else CHECK(0, "C1072 reopen for pipe guard");
        printf("C1072 isolated failures=%d\n", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--gpu-deswizzle")) {
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "gpu-deswizzle-test";
        if (!open_backend()) return 1;
        nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
        int disabled = getenv("S2A_TEST_GPU_DESWIZZLE_OFF") != NULL;
        CHECK(!!(caps.caps & NVN_GPU_CAP_GPU_DESWIZZLE) == !disabled, "GPU deswizzle capability matches switch");
        if (!disabled) {
            gpu_deswizzle_basic();
#ifdef S2A_GPU_DESWIZZLE_TEST
            gpu_deswizzle_depth_fallback();
            gpu_deswizzle_matrix();
            gpu_deswizzle_compute_state(argv[1]);
#endif
        }
        { extern uint64_t nvn_vk_dbg_dq_recs(void); uint64_t n = nvn_vk_dbg_dq_recs();
          CHECK(getenv("S2A_TEST_NVK_DQ") && !disabled ? n > 0 : n == 0, "GPU deswizzle recorder mode records=%llu", (unsigned long long)n); }
        nvn_gpu_err e = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "GPU deswizzle close %s", e.why);
        printf("GPU_DESWIZZLE failures=%d\n", g_fail);
        s2a_vk_device_destroy(&g_dev, 1); return g_fail ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--ds-upload-only")) {   // 只跑 C1581K（默认档里也跑）；给红 / 绿 / 负控取证用
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        if (!open_backend()) return 1;
        c1581k_ds_upload(argv[1]);
        nvn_gpu_err e = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1581K close %s", e.why);
        printf("C1581K failures=%d\n", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--stencil8-only")) {   // 只跑 C1581O（默认档里也跑）；给红 / 绿 / 负控取证用
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        if (!open_backend()) return 1;
        c1581o_stencil8(argv[1]);
        nvn_gpu_err e = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C1581O close %s", e.why);
        printf("C1581O failures=%d\n", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--c63-same-view")) {   // 只跑 C63c（默认档里也跑）；给「红在被修的那一处」取证用
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        if (!open_backend()) return 1;
        c63c_desc_array_same_view(argv[1]);
        nvn_gpu_err e = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C63c close %s", e.why);
        printf("C63SAMEVIEW failures=%d\n", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_PLC_ONLY") && argc > 1) {   // 开发时只跑 C89（run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c89_midrun_save(argv[1]);
        printf("nvn_gpu Vulkan 后端 C89 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_C204_ONLY") && argc > 1) {   // 开发时只跑 C204（描述符组按布局回收；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c204_dset_recycle(argv[1]);
        printf("nvn_gpu Vulkan 后端 C204 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_C208_ONLY") && argc > 1) {   // 开发时只跑 C208（动态 UBO 偏移；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c208_dyn_ubo(argv[1]);
        printf("nvn_gpu Vulkan 后端 C208 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_C201_ONLY")) {   // 开发时只跑 C201（视图销毁的两处线性代价；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        if (!open_backend()) return 1;
        c201_view_list();
        c201_fb_scan(0);
        c201_fb_scan(1);
        nvn_gpu_err e201 = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e201)), "C201 close：%d %s", e201.rc.code, e201.why);
        printf("nvn_gpu Vulkan 后端 C201 单跑：%s（%d 处不符，自检残留 %llu）\n", g_fail ? "红" : "绿", g_fail, (unsigned long long)nvn_vk_dbg_view_stat(3));
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_STG_ONLY")) {   // 开发时只跑 C202 / C210（暂存缓冲回收表、暂存写分块；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c202_stg_pool();
        c210_stg_chunk();
        printf("nvn_gpu Vulkan 后端 C202 / C210 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_C206_ONLY")) {   // 开发时只跑 C206（段内已引用句柄备忘；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c206_ref_seen();
        printf("nvn_gpu Vulkan 后端 C206 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_C205_ONLY") && argc > 1) {   // 开发时只跑 C205（图形管线散列表；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c205_all(argv[1]);
        printf("nvn_gpu Vulkan 后端 C205 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_C207_ONLY") && argc > 1) {   // 开发时只跑 C207（诊断读数；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c207_all(argv[1]);
        printf("nvn_gpu Vulkan 后端 C207 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_C209_ONLY") && argc > 1) {   // 开发时只跑 C209（管线标签随对象；run.sh 不设这个变量）
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-backend-vk-test";
        c209_all(argv[1]);
        printf("nvn_gpu Vulkan 后端 C209 单跑：%s（%d 处不符）\n", g_fail ? "红" : "绿", g_fail);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_LOGICOP_ONLY")) {
        if(argc < 2) return 2;
        memset(&g_desc,0,sizeof g_desc);g_desc.host_device=&g_dev;g_desc.app_label="logic-only";
        if(!open_backend())return 1;
        c47_logic(argv[1]);
        nvn_gpu_err le={0}; CHECK(OK(nvn_gpu_close(5000000000ull,&le)),"logic close"); s2a_vk_device_destroy(&g_dev, 1);
        printf("logicOp-only failures=%d\n",g_fail); return g_fail ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--gs-no-cap")) {
        memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "gs-no-cap";
        if (!open_backend()) return 1;
        nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
        CHECK(g_dev.core_features.geometryShader == VK_FALSE && !(cp.caps & NVN_GPU_CAP_GEOMETRY), "no probe leaves GS disabled");
        char path[512]; size_t len = 0; snprintf(path, sizeof path, "%s/gs_probe.geom.spv", argv[1]); void *blob = read_file(path, &len);
        CHECK(blob != NULL, "GS no-cap shader exists");
        nvn_gpu_shader_src ss = {0}; ss.stage = NVN_GPU_SH_GS; ss.kind = NVN_GPU_BLOB_SPIRV; ss.blob = blob; ss.bytes = len; ss.entry = "main";
        nvn_gpu_shader sh = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc sr = nvn_gpu_shader_load(&ss, &sh, &e);
        CHECK(sr.code == NVN_GPU_E_UNSUPPORTED && !sh, "no capability refuses GS shader before Vulkan");
        if (sh) nvn_gpu_shader_release(sh);
        free(blob);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "GS no-cap close"); s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (!getenv("S2A_TEST_LOGICOP_ONLY")) bc_timeout_lifetime();
    // 机型模拟档（android/tests/vk/sim_tiers.sh）按这行核对模拟层确实藏掉了对应能力。
    printf("sim caps: vertexPipelineStoresAndAtomics=%u dualSrcBlend=%u calibrated_timestamps=%d（0 = 无校准扩展，走完成点兜底）\n",
           (unsigned)g_dev.core_features.vertexPipelineStoresAndAtomics, (unsigned)g_dev.core_features.dualSrcBlend,
           g_dev.calibrated_timestamps);
    if (argc < 2) {
        printf("用法：%s <shader-dir>（run.sh 会编译 tri.vert/frag 到该目录）\n", argv[0]);
        return 2;
    }
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "nvn-backend-vk-test";
    if (argc > 2 && (!strcmp(argv[2], "--shown-only") || !strcmp(argv[2], "--shown-unsupported"))) {
        g_shown_timing_test = !strcmp(argv[2], "--shown-only") ? 1 : 2;
        if (g_shown_timing_test == 2) {
            uint32_t count = 0;
            VkResult er = vkEnumerateDeviceExtensionProperties(g_dev.physical, NULL, &count, NULL);
            VkExtensionProperties *exts = calloc(count ? count : 1, sizeof *exts);
            CHECK(er == VK_SUCCESS && exts, "C883 enumerate extension count");
            if (exts && er == VK_SUCCESS) {
                er = vkEnumerateDeviceExtensionProperties(g_dev.physical, NULL, &count, exts);
                CHECK(er == VK_SUCCESS, "C883 enumerate extension list");
                int supported = 0;
                for (uint32_t i = 0; i < count; i++) if (!strcmp(exts[i].extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) supported = 1;
                CHECK(!supported, "C883 unsupported arm requires actual lavapipe without display timing");
            }
            free(exts);
        }
        g_dev.has_display_timing = g_shown_timing_test == 1; // simulated capability, not a lavapipe claim
        if (!open_backend()) return 1;
        c71_present();
        if (g_shown_timing_test == 1)
            CHECK(g_timing_tags >= 3 && g_timing_bad == 0,
                  "C883 supported opt-in presents carry distinct IDs: tags=%u invalid=%u", g_timing_tags, g_timing_bad);
        else CHECK(!g_timing_tags && g_shown_empty_logged,
                   "C883 unsupported has no ID and logs empty metrics with reason: tags=%u logs=%d", g_timing_tags, g_shown_empty_logged);
        nvn_gpu_err ce = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &ce)), "C883 close: %s", ce.why);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--sampler-minmax-only")) {
        if (!open_backend()) return 1;
        c976r_reduction(argv[1]);
        nvn_gpu_err e = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C976r close：%s", e.why);
        if (!open_backend()) return 1;
        c976r_bad_format(argv[1]);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C976r bad format close：%s", e.why);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (!open_backend()) return 1;
    // #1072：核心按格式能力退最近点 / 关混合；模拟档 no_fmt_filter_blend 按这行核对后端如实报了「不能过滤 / 不能混合」。
    printf("sim fmt caps: RGBA16 filter=%d R32F blend=%d\n", !!(nvn_gpu_format_caps(NVN_FORMAT_RGBA16) & NVN_GPU_FMT_FILTER), !!(nvn_gpu_format_caps(NVN_FORMAT_R32F) & NVN_GPU_FMT_BLEND));
    if (!getenv("S2A_TEST_WSI697_ONLY") && !c697_bc4_3d_run()) return 1;
    if (getenv("S2A_TEST_BC4_3D_ONLY")) {
        nvn_gpu_err e = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "C697 BC4 final close：%s", e.why);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (getenv("S2A_TEST_WSI697_ONLY")) {
        c697_swapchain_images();
        c71_present();
        nvn_gpu_err close_err = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &close_err)), "C697 close: %s", close_err.why);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    poly_key_padding(argv[1]);
    c697_swapchain_images();
    c690_native_bc_image_copy(NVN_FORMAT_RGB_DXT1_SRGB, 8, 1);
    c690_native_bc_image_copy(NVN_FORMAT_RGBA_DXT5, 16, 2);
    c690_edge_copy();
    { nvn_gpu_err e690; CHECK(OK(nvn_gpu_close(5000000000ull, &e690)), "C690 close：%d %s", e690.rc.code, e690.why); }
    for (int k = 0; k < 8; k++) {
        if (!open_backend()) return 1;
        c690_bad_region(k);
        nvn_gpu_err e690; CHECK(OK(nvn_gpu_close(5000000000ull, &e690)), "C690 负例 %d close：%d %s", k, e690.rc.code, e690.why);
    }
    if (!open_backend()) return 1;

    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    if (getenv("S2A_CAPPROBE_RESULT")) {
        CHECK(g_dev.core_features.geometryShader == VK_TRUE, "探测通过后设备须真实启用 geometryShader");
        CHECK((caps.caps & NVN_GPU_CAP_GEOMETRY) != 0, "已启用 GS 应报告后端能力");
    } else {
        CHECK(g_dev.core_features.geometryShader == VK_FALSE && !(caps.caps & NVN_GPU_CAP_GEOMETRY),
              "无探测时设备和后端都不启用 GS");
    }
    CHECK(caps.api == NVN_GPU_API_VULKAN, "caps.api 应为 VULKAN");
    CHECK(caps.max_sets > 0 && caps.max_buf_bytes > 0, "caps 限值没有填");
    CHECK(caps.caps_id[0] | caps.caps_id[1] | caps.caps_id[15], "caps_id 全 0");
    CHECK(nvn_gpu_format_caps(0x1) != 0, "format_caps 对 R8 无能力位");   // NVN_FORMAT_R8
    if (argc > 2 && !strcmp(argv[2], "--gs-compiler")) t_gs_compiler_pixels(argv[1]);
    else { t_gs_pixels(argv[1]); t_gs_tstrip_pixels(argv[1]); }
    { nvn_gpu_err eg; CHECK(OK(nvn_gpu_close(5000000000ull, &eg)), "GS fixture close: %d %s", eg.rc.code, eg.why); }
    if (argc > 2 && (!strcmp(argv[2], "--gs-only") || !strcmp(argv[2], "--gs-compiler"))) {
        s2a_vk_device_destroy(&g_dev, 1);
        printf("GS 专属像素与资源：%d 处不符\n", g_fail);
        return g_fail ? 1 : 0;
    }
    if (!open_backend()) return 1;
    t76_buf_create_alloc_cap();

    t01_fill32();
    t02_write_copy();
    t03_keepalive_unsubmitted();
    t03b_buf_use();
    t04_abort();
    t05_failure_terminal();

    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "第一次 close 失败：%d %s", e.rc.code, e.why);
    if (!open_backend()) return 1;
    t06_publish_order();
    t07_wait_args();
    t08_sticky();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "第二次 close 失败：%d %s", e.rc.code, e.why);
    if (!open_backend()) return 1;
    t09_release_only();
    t11_clear_color();
    t12_upload_copy_tex();
    t13_clear_depth();
    t14_textured_triangle(argv[1]);
    t10_close_inflight();
    if (!open_backend()) return 1;
    t16_fence_failure();
    nvn_gpu_err e2;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e2)), "t16 close：%d %s", e2.rc.code, e2.why);
    if (!open_backend()) return 1;
    t17_close_timeout();
    if (!open_backend()) return 1;
    t18_combined_sampler(argv[1]);
    nvn_gpu_err e3;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e3)), "t18 close：%d %s", e3.rc.code, e3.why);
    if (!open_backend()) return 1;
    t19_signal_contract();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e3)), "t19 close：%d %s", e3.rc.code, e3.why);
    if (!open_backend()) return 1;
    t20_combined_missing_samp(argv[1]);
    nvn_gpu_err e4;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e4)), "t20 close：%d %s", e4.rc.code, e4.why);
    if (!open_backend()) return 1;
    t21_clear_int();
    nvn_gpu_err e5;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e5)), "t21 close：%d %s", e5.rc.code, e5.why);
    if (!open_backend()) return 1;
    t22_clear_view_fmt();
    t22b_ubwc_format_list();
    nvn_gpu_err e6;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e6)), "t22 close：%d %s", e6.rc.code, e6.why);
    if (!open_backend()) return 1;
    t30_rgb8_sampled(argv[1]);
    t28_rgb8_upload();
    nvn_gpu_err e7;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e7)), "t30/t28 close：%d %s", e7.rc.code, e7.why);
    if (!open_backend()) return 1;
    c71_present();
    { nvn_gpu_err e71; CHECK(OK(nvn_gpu_close(5000000000ull, &e71)), "C71 close（面还接着）：%d %s", e71.rc.code, e71.why); }
    if (!open_backend()) return 1;
    c73_origin_upper_left(argv[1]);
    { nvn_gpu_err e73; CHECK(OK(nvn_gpu_close(5000000000ull, &e73)), "C73 close：%d %s", e73.rc.code, e73.why); }
    if (!open_backend()) return 1;
    c72_bc1_upload(argv[1], (g_dev.bc_decode_mask & (1u << (NVN_FORMAT_RGBA_DXT1 - 0x41))) != 0);
    c697e_bc_small_mip_block_region(0);   // 原生 BC 路径（同一个 copy_region）
    { nvn_gpu_err e72; CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C72 close：%d %s", e72.rc.code, e72.why); }
    { uint32_t saved_mask = g_dev.bc_decode_mask;
      g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_RGBA_DXT1 - 0x41);
      if (!open_backend()) return 1;
      c72_bc1_upload(argv[1], 1);
      c192_bc_order();
      c194_bc_cube_mips();
      c697f_bc_3d_slice_from_buf();        // 现解路径：3D 贴图 layer 当切片
      c697e_bc_small_mip_block_region(1);   // 现解路径（Mali 走这条）
      nvn_gpu_err e72; CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C72 compute close：%d %s", e72.rc.code, e72.why);
      if (!open_backend()) return 1;
      c193_bc_readback_rejected();
      CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C87 close：%d %s", e72.rc.code, e72.why);
      if (!open_backend()) return 1;
      CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C196 前 close：%d %s", e72.rc.code, e72.why);
      { uint32_t m = g_dev.bc_decode_mask; g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_RGTC1_UNORM - 0x41);   // C196 ② / C198 的 3D 要 BC4 现解
        if (!open_backend()) return 1;
        c197_bc_upload_in_segment();
        CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C197 close：%d %s", e72.rc.code, e72.why);
        if (!open_backend()) return 1;
        c198_bc_bulk_upload();
        CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C198 close：%d %s", e72.rc.code, e72.why);
        if (!open_backend()) return 1;
        c196_bc_abort_reclaim();   // 以 close 结束
        g_dev.bc_decode_mask = m; }
      {   // #682 的六个格式：每块按端点直接构造纯红，采样后都是 (255,0,0,255)。
          static const uint32_t fmt[6] = {0x41,0x44,0x45,0x48,0x49,0x4b};
          static const uint8_t bc1[8] = {0x00,0xf8,0x00,0xf8,0,0,0,0};
          static const uint8_t bc3[16] = {255,255,0,0,0,0,0,0, 0x00,0xf8,0x00,0xf8,0,0,0,0};
          static const uint8_t bc4[8] = {255,255,0,0,0,0,0,0};
          static const uint8_t bc5[16] = {255,255,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};
          static const uint8_t red[4] = {255,0,0,255};
          for (int i = 0; i < 6; i++) g_dev.bc_decode_mask |= 1u << (fmt[i] - 0x41);
          if (!open_backend()) return 1;
          g_tri_expect = red;
          for (int i = 0; i < 6; i++) {
              const uint8_t *block = i == 1 || i == 3 ? bc3 : i == 4 ? bc4 : i == 5 ? bc5 : bc1;
              int row = i == 1 || i == 3 || i == 5 ? 16 : 8;
              g_tri_signal = (uint64_t)i + 1;
              printf("C195 执行：BC 格式 %#x\n", fmt[i]);
              tri_impl(argv[1], fmt[i], block, 4, row, 255);
          }
          g_tri_expect = NULL; g_tri_signal = 5;
          CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C195 close：%d %s", e72.rc.code, e72.why);
      }
      g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_RGBA_DXT1_SRGB - 0x41);
      if (!open_backend()) return 1;
      {   // 565 红端点 16 → 字节 132，sRGB 采样变线性 ≈ 0.2307 → 59/255。
          static const uint8_t block[8] = {0x00,0x80,0x00,0x80,0,0,0,0};
          static const uint8_t linear[4] = {59,0,0,255};
          g_tri_expect = linear; g_tri_tol = 1;
          tri_impl(argv[1], NVN_FORMAT_RGBA_DXT1_SRGB, block, 4, 8, 255);
          g_tri_expect = NULL; g_tri_tol = 0;
      }
      CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "BC SRGB close：%d %s", e72.rc.code, e72.why);
      g_dev.bc_decode_mask = saved_mask; }
    for (int k = 0; k < 2; k++) {   // tri_impl 的提交序号固定：每个变体一个会话
        if (!open_backend()) return 1;
        c70_swizzle(argv[1], k);
        nvn_gpu_err e70;
        CHECK(OK(nvn_gpu_close(5000000000ull, &e70)), "C70 close：%d %s", e70.rc.code, e70.why);
    }
    for (int k = 0; k < 4; k++) {   // C200：四张各一个会话（tri_impl 的提交序号固定）
        if (!open_backend()) return 1;
        c200_alpha_reads_one(argv[1], k);
        nvn_gpu_err e200;
        CHECK(OK(nvn_gpu_close(5000000000ull, &e200)), "C200 close：%d %s", e200.rc.code, e200.why);
    }
    for (int k = 0; k < 16; k++) {   // C975：16 位打包格式 8 种 × {设备路由, 强制退回}，各一个会话（tri_impl 的提交序号固定）
        if (!open_backend()) return 1;
        c975_fmt16_order(argv[1], k % 8, k / 8);
        nvn_gpu_err e975;
        CHECK(OK(nvn_gpu_close(5000000000ull, &e975)), "C975 close：%d %s", e975.rc.code, e975.why);
    }
    if (!open_backend()) return 1;
    t31_vertex_formats(argv[1]);
    nvn_gpu_err e8;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e8)), "t31 close：%d %s", e8.rc.code, e8.why);
    if (!open_backend()) return 1;
    t32_expand_bounds();
    nvn_gpu_err e9;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e9)), "t32 close：%d %s", e9.rc.code, e9.why);
    if (!open_backend()) return 1;
    t33_two_passes(argv[1]);
    t34_two_pass_load(argv[1]);
    t35_three_passes(argv[1]);
    nvn_gpu_err e10;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e10)), "C33–C35 close：%d %s", e10.rc.code, e10.why);
    if (!open_backend()) return 1;
    c36_40(argv[1]);
    nvn_gpu_err e11;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e11)), "C36–C40 close：%d %s", e11.rc.code, e11.why);
    if (!open_backend()) return 1;
    c41_bounds_neg(argv[1]);
    nvn_gpu_err e12;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e12)), "C41 close：%d %s", e12.rc.code, e12.why);
    if (!open_backend()) return 1;
    c42_u8_indirect_neg(argv[1]);
    nvn_gpu_err e13;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e13)), "C42 close：%d %s", e13.rc.code, e13.why);
    if (!open_backend()) return 1;
    c43_blend(argv[1]);
    nvn_gpu_err e14;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e14)), "C43 close：%d %s", e14.rc.code, e14.why);
    if (!open_backend()) return 1;
    c44_prims(argv[1]);
    c46_blend_const(argv[1]);
    c47_logic(argv[1]);
    c48_fan(argv[1]);
    c49_restart(argv[1]);
    c50_list_restart(argv[1]);
    c51_array(argv[1]);
    c52_cube(argv[1]);
    c53_cube_array(argv[1]);
    c54_3d(argv[1]);
    c55_compute(argv[1]);
    nvn_gpu_err e15;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e15)), "C44 close：%d %s", e15.rc.code, e15.why);
    if (!open_backend()) return 1;
    c45_multi_blend(argv[1]);
    nvn_gpu_err e16;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e16)), "C45 close：%d %s", e16.rc.code, e16.why);
    if (!open_backend()) return 1;
    c48_mismatch(argv[1]);
    nvn_gpu_err e17;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e17)), "C48 mismatch close：%d %s", e17.rc.code, e17.why);
    if (!open_backend()) return 1;
    // 成功路径同会话；C58a–c 预期失败提交，每个独占会话（失败段进终态，关会话重开恢复）。
    c56_push_fs(argv[1]);
    c57_push_cs(argv[1]);
    c58d_push_pipe_resize(argv[1]);
    c59a_buffer_view();
    c59_tbuf(argv[1]);
    c60_image_tbuf(argv[1]);
    c61_storage_image(argv[1]);
    c62_tsamp_cs(argv[1]);
    c63_desc_array_elem(argv[1]);
    c63c_desc_array_same_view(argv[1]);
    c64_va_red(argv[1]);
    c67_viewport_yflip(argv[1]);
    c68_stencil_ops(argv[1]);
    c74_clear_rect(argv[1]);
    c75_depth_bias(argv[1]);
    c76_vs_only_depth(argv[1]);
    c77_null_color_slot(argv[1]);
    c79_masked_clear(argv[1]);
    c80_front_face(argv[1]);
    c81_int_sampled(argv[1]);
    c82_occlusion(argv[1]);
    c83_blit();
    c84_validation_classes(); c84b_bc_block_copy();
    c85_descriptor_reuse(argv[1]);
    c86_polygon_modes(argv[1]);
    c87_resolve(argv[1]);
    c86b_3d_slice_attach();
    c86c_bc_3d_slice_copy();
    c90_no_attachment(argv[1]);   // 篱笆值 200：排在 c86c（191）之后
    nvn_gpu_err e18;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C56–C68 / C74 / C75 成功路径 close：%d %s", e18.rc.code, e18.why);
    for (int k = 0; k < 8; k++) {
        if (!open_backend()) return 1;
        c87_resolve_reject(k);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C87 拒绝 close：%s", e18.why);
    }
    if (!open_backend()) return 1;   // 自己一个会话：围栏号 1
    c1581o_stencil8(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C1581O close：%s", e18.why);
    if (!open_backend()) return 1;   // 自己一个会话：围栏号 1、2
    c1581k_ds_upload(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C1581K close：%s", e18.why);
    if (!open_backend()) return 1;   // 自己一个会话：围栏号 1、2
    cp06_cube_of_array(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "P0-6 close：%s", e18.why);
    if (!open_backend()) return 1;
    c977_divisor(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C977b close：%s", e18.why);
    if (!open_backend()) return 1;
    c977_a2c(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C977a close：%s", e18.why);
    if (!open_backend()) return 1;
    c74b_clear_rect_oob();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C74b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c937_use_after_release();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C937 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c85_redundant_state(argv[1]);   // 独占会话：时间线信号不受前后用例的排列影响
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C85 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58a_push_oob_cs(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58a close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58b_push_bad_stage(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58c_push_oob_fs(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58c close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c65_depth_only_pass();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C65 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c66_pool_sets(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C66 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c60b_unknown_image_tbuf(argv[1]);
    c1072a_vertex_caps(argv[1]);
    c1340_int16_per_program();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C60b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c60c_unknown_image_tbuf_read(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C60c close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c59b_legacy_binding(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C59b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c69_dual_src_blend(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C69 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;   // 自己一个会话：围栏号 1、2
    c976m_mirror_clamp(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C976m close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;   // 自己一个会话：围栏号 1..6，配额计数从 0 起
    c976b_border_color(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C976b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;   // 自己一个会话：围栏号 1–4
    c976r_reduction(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C976r close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c976r_bad_format(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C976r 格式负控 close：%d %s", e18.rc.code, e18.why);

    if (!open_backend()) return 1;
    cube_array_one_face();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "cube-array view close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;   // C197 自己一个会话：围栏号从这里重新算，和前后用例不相干
    c197_pass_cache();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C197 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;   // C61s 自己一个会话：围栏号从 1 起，不穿插在主会话 C61（108）/ C62（109）之间
    c61s_storage_srgb_alias(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C61s close：%d %s", e18.rc.code, e18.why);
    c204_dset_recycle(argv[1]);   // 描述符组按布局回收（#1096）：每个臂自己开 / 关会话
    c208_dyn_ubo(argv[1]);   // 动态 UBO 偏移（#1233）：关 / 开两个臂各开一次会话
    if (!open_backend()) return 1;   // C201 同样自己一个会话
    c201_view_list();
    c201_fb_scan(0);
    c201_fb_scan(1);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C201 close：%d %s", e18.rc.code, e18.why);
#ifdef S2A_CMDPOOL_TEST
    c1581_cmdpool();   // 独立会话：额外命令缓冲随命令池复用
#endif
    c202_stg_pool();   // 缺省档与对照档各自开 / 关会话（围栏号从各自会话重新算）
    c210_stg_chunk();  // C210 暂存写分块：一个会话
    c206_ref_seen();   // C206 段内已引用句柄备忘：缺省档与对照档各三个会话

    c205_all(argv[1]);   // C205 图形管线散列表：每个臂自己一个会话
    c207_all(argv[1]);   // C207 诊断读数互不清零：自己一个会话（计时开关、逐笔读数周期都在用例里开 / 还原）
    c208_close_wakeup();   // C208 close 不等满时限：自己一个会话
    c209_all(argv[1]);   // C209 管线标签随对象（#1274）：自己一个会话（起始 16 桶的用例开关要在会话里第一条管线之前设）

    for (int sw = 0; sw < 2; sw++) {
        if (!open_backend()) return 1;
        attach_swizzle_case(argv[1], sw);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "attachment swizzle close：%d %s", e18.rc.code, e18.why);
    }

    for (int srgb = 0; srgb < 2; srgb++) {
        if (!open_backend()) return 1;
        attach_swizzle_subresource_case(argv[1], srgb);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "attachment subresource close：%d %s", e18.rc.code, e18.why);
    }

    if (!open_backend()) return 1;
    c63b_desc_array_same(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C63b close：%d %s", e18.rc.code, e18.why);

    if (!open_backend()) return 1;
    c49b_restart_pipe_cache(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C49b close：%d %s", e18.rc.code, e18.why);

    c1501_clear_capacity(argv[1]);
    c88_pipe_cache(argv[1]);   // 每个会话自己开 / 关（带不同的 cache_dir）
#ifdef S2A_PIPE_CREATION_FEEDBACK_TEST
    creation_feedback_test(argv[1]);   // 三形态各用自己的后端会话
#endif
    c89_midrun_save(argv[1]);  // 同上；还会起子进程（被 SIGKILL 的那一局）
    c658_suite(argv[1],0);   // Independently opens/closes; absent count capability is a negative control.
    c907_retained_flush(argv[1]);   // 停局保留会话时的管线缓存落盘（#907 合入补）
#ifdef S2A_LAND_READBACK_TEST
    test_land_readback(0,0);
    test_land_readback(1,0);
    test_land_readback(0,1);
    test_land_readback(1,1);
#endif
    // C201：整套用例都在 nvk_fbdrop=2（自检档）下跑，视图销毁走快路之后每次再全表扫一遍；反查表漏记过的帧缓冲会在这里留下残留计数。
    CHECK(nvn_vk_dbg_view_stat(3) == 0, "C201 整套用例在自检档下跑完：视图销毁后帧缓冲缓存里扫到的残留期望 0 → %llu", (unsigned long long)nvn_vk_dbg_view_stat(3));

    printf("nvn_gpu Vulkan 后端 C01–C08/C23/C25/C26/C27–C55/C56–C69/C74–C75/C88/C89 + 清屏/复制/深度/纹理三角形用例：%s（%d 处不符，done %d 次，retired %d 次）\n",
           g_fail ? "红" : "绿", g_fail, atomic_load(&g_ndone), atomic_load(&g_retired));
    s2a_vk_device_destroy(&g_dev, 1);   // 最后一个会话已在 t20 close 里证明空闲
    { extern uint64_t nvn_vk_dbg_dq_recs(void); uint64_t n = nvn_vk_dbg_dq_recs();
      if (getenv("S2A_TEST_NVK_DQ")) { CHECK(n > 1000, "两级回放开着跑完整套：期望交给录制线程的调用远多于 1000 条 → %llu 条", (unsigned long long)n); printf("两级回放：交给录制线程 %llu 条调用\n", (unsigned long long)n); }
      else CHECK(n == 0, "两级回放关着：期望没有调用进队列 → %llu 条", (unsigned long long)n); }
    return g_fail ? 1 : 0;
}
