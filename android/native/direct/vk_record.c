#include "vk_record.h"
#include "nvn_obj.h"
#include "nvn_vk_texcache.h"   // A3：对象身份判据 / serial 状态（墓碑）查询；弱符号，宿主合同可替身
#include "t2t_plan.h"
#include "buffer_texture_plan.h"
#include "nvn2_cmd.h"   // DM_PRIM_MESH_TASKS（NVN2 任务 / 网格绘制）
#pragma weak nvn_ga2ca
// A3：texcache 是资源状态的唯一归属；宿主单测单独编本 TU 时这两个符号为空（不分类，保持原失败路径）。
// 弱链接[androiddraw]：nvn_vk_texcache_tex_identity
// 弱链接[androiddraw]：nvn_vk_texcache_serial_state
// （宿主单测编本 TU 不链 texcache.c；macOS ld64 不接受可达的未定义弱引用，按 tools/weak_link_decl.py 登记放行）
extern int nvn_vk_texcache_tex_identity(const tex_t *t, char *why, size_t why_size) __attribute__((weak));
extern int nvn_vk_texcache_serial_state(uint64_t serial, struct nvn_tex_tombstone *tomb) __attribute__((weak));
#include "nvn_vk_format.h"
#include "nvn_vk_query.h"
#include "nvn_vk_first_draw.h"
#include "vk_clear.h"

#include <android/log.h>
#include <sys/system_properties.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEPS 1000000u
#define MAX_COPY (64u * 1024u * 1024u)

static int gpu_clear_image(struct s2a_vk_device *d, VkImage image, VkClearColorValue color);

// 诊断（不改变回放行为）：统计 SetRenderTarget 与当前已开 pass 目标是否完全相同。
// same 高说明很多 SET_RT 是冗余的 → 可去重合并 pass，同时降 GPU(tile load/store) 与 CPU(SET_RT/pass open)。
static _Atomic uint64_t g_rt_same, g_rt_diff;
void vk_record_take_rt_stats(uint64_t *same, uint64_t *diff) {
    if (same) *same = atomic_exchange(&g_rt_same, 0);
    if (diff) *diff = atomic_exchange(&g_rt_diff, 0);
}

// ---- 诊断（不改回放行为）：① 每次「确实关掉了一个 pass」是被哪个 opcode 触发的；
// ② SetRT 的**语义级**重复率（只比附件身份/代际/尺寸/mip/layer/view 覆盖，不整结构 memcmp）。
// 上面那对 g_rt_same/g_rt_diff 是「与上一次 SetRT 的整 tex_t memcmp」，tex_t 里还有
// serial 之外的易变字段，会把「同一目标」误判成 diff；这里的指纹只取与附件身份相关的字段。
static _Atomic uint64_t g_opbrk[64];
static _Atomic uint64_t g_rt_sem_same, g_rt_sem_diff;
static uint64_t fd_fp_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 1099511628211ull;
}
static uint64_t fd_att_fp(const tex_t *t, const view_t *v, int hasv) {
    if (!t) return 0;
    uint64_t h = 1469598103934665603ull;
    h = fd_fp_mix(h, (uint64_t)(uintptr_t)t);                 // 附件对象身份
    h = fd_fp_mix(h, t->serial);                              // 代际（重新 Initialize 会变）
    h = fd_fp_mix(h, ((uint64_t)(uint32_t)t->d.target << 32) | (uint32_t)t->d.format);
    h = fd_fp_mix(h, ((uint64_t)t->d.width << 32) | t->d.height);
    h = fd_fp_mix(h, ((uint64_t)t->d.depth << 32) | t->d.levels);
    h = fd_fp_mix(h, ((uint64_t)(uint32_t)t->d.samples << 32) | (uint32_t)t->d.stride);
    h = fd_fp_mix(h, ((uint64_t)t->set_swz << 40) | ((uint64_t)t->stencil << 41) | (uint64_t)t->swz);
    if (hasv && v) {
        h = fd_fp_mix(h, ((uint64_t)v->set_format << 40) | ((uint64_t)v->set_levels << 41) |
                         ((uint64_t)v->set_layers << 42) | ((uint64_t)v->set_target << 43) |
                         ((uint64_t)v->set_swizzle << 44) | ((uint64_t)v->set_dsmode << 45) | (uint64_t)v->dsmode);
        h = fd_fp_mix(h, ((uint64_t)v->target << 32) | ((uint64_t)v->base_level << 16) | v->nlevels);
        h = fd_fp_mix(h, ((uint64_t)v->format << 32) | ((uint64_t)v->min_layer << 16) | v->nlayers);
    }
    return h;
}
static struct { int valid, n, has_d; uint64_t c[8], d; } g_rt_sem;
void vk_record_take_break_stats(uint64_t *out64) {
    if (!out64) return;
    for (int i = 0; i < 64; i++) out64[i] = atomic_exchange(&g_opbrk[i], 0);
}
void vk_record_take_rt_sem_stats(uint64_t *same, uint64_t *diff) {
    if (same) *same = atomic_exchange(&g_rt_sem_same, 0);
    if (diff) *diff = atomic_exchange(&g_rt_sem_diff, 0);
}

// 新后端的 open render pass：下列 opcode 会改变附件或插入非绘制 GPU 工作，必须先结束它。
// 只更新 CPU 状态的绑定/状态/UBO 写入不在此列，连续 draw 才能真正合并。
// 返回 0 = 边界已落地；非 0 = 帧队列没有完成证明（资源已保留），后续 opcode 必须停止执行，
// 不能按“之前的 GPU 工作已经完成”继续消费。
extern int nvn_vk_first_draw_pass_break(int sync) __attribute__((weak));
// 帧队列时间戳入口（弱绑定）：生产链接时把时间戳录进当前帧命令缓冲；微例/同步路径没有它。
extern int nvn_vk_first_draw_timestamp_top(struct s2a_vk_device *d, VkQueryPool pool, uint32_t slot,
                                           char *why, size_t why_size) __attribute__((weak));
extern uint64_t nvn_vk_first_draw_frame_id(void) __attribute__((weak));
extern uint64_t nvn_vk_first_draw_detach_submit_seq(void) __attribute__((weak));
extern void s2a_poolahb_complete_seq(uint64_t seq) __attribute__((weak));
// 通用帧队列入队三态：只有 NOT_TAKEN 允许调用方自提或回收。
extern int nvn_vk_first_draw_enqueue(struct s2a_vk_device *d, VkCommandBuffer cmd) __attribute__((weak));
// 帧队列首错（弱绑定）：失败传播时带上真实原因，不覆盖首错。
extern const char *nvn_vk_first_draw_first_error(void) __attribute__((weak));
// ReportCounter 只在 pass 内切查询段（vkCmdEndQuery/BeginQuery 在 pass 内合法），不再关整个 render pass。
// query 模块本就支持同 pass 内多段。默认开：真机 A/B 证 45→35 pass/帧、exec 13.3→12.3ms、fps 49.0→50.1、
// 查询结果正确、red=0。回退旧行为（每次 Report 关 pass）用 debug.s2a.qseg_off=1。
static int qseg_inpass(void) {
    static int v = -1;
    if (v < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.qseg_off", b); v = (b[0] == '1') ? 0 : 1; }
    return v;
}
static int op_breaks_pass(uint16_t op) {
    switch (op) {
    case OP_SET_RT:
    case OP_CLEAR_COLOR:
    case OP_CLEAR_DS:
    case OP_CLEAR_TEX:
    case OP_CLEAR_BUF:
    case OP_COPY_B2B:
    case OP_COPY_T2B:
    case OP_COPY_B2T:
    case OP_COPY_T2T:
    case OP_DISPATCH:
    case OP_BARRIER:
    case OP_FENCE_SYNC:
    case OP_SIGNAL_EVENT:
        return 1;
    case OP_REPORT_COUNTER:
        return qseg_inpass() ? 0 : 1;   // 默认 pass 内切段，减少每帧 pass 数（debug.s2a.qseg_off=1 回退每次关 pass）
    default:
        return 0;
    }
}


// 回放的 GPU 状态：必须在 mtl_exec 的多次调用之间保留（前端每条命令一个包，绑定是跨包的）。
// 一次提交内要保留同一份状态；这里放到模块级，由 mtl_sub_begin 复位。
struct replay_state {
    const void *rt_color[8];
    uint8_t rt_has_view[8];
    tex_t rt_snap[8];             // SetRenderTarget 时的真实 tex_t 副本（对象可能随后被复用/释放）
    uint8_t rt_snap_ok[8];
    const void *rt_depth;
    tex_t rt_depth_snap;
    uint8_t rt_depth_snap_ok;
    view_t rt_dview;              // SetRenderTargets 的深度视图（mip/层子集，清除必须按它）
    uint8_t rt_has_dview;
    view_t rt_view[8];
    int rt_count;
    uint64_t seq, rt_seq;         // 记录序号：本笔 draw 的位置与颜色目标最后一次 SetRenderTargets 的位置
    const void *bound_program;
    const void *cs_program;
    uint64_t cs_serial;
    uint32_t cs_ubo_seen, cs_ssbo_seen; // observed bind/unbind, distinct from current bound bits
    uint8_t cs_key[8], cs_present;
    uint64_t cs_image_handle[32];
    uint8_t cs_image_present[32];
    uint8_t bound_keys[6][8];     // 按 NVN 阶段序号（0 vs / 1 fs / 2 gs / 3 tcs / 4 tes）合并后的 key
    uint32_t bound_stages;        // = bound_stage_present
    uint8_t bound_stage_present;
    struct {
        int32_t vp_first, vp_n; float vp[16][6];
        uint8_t vp_seen, sc_seen;     // 该状态是否被真实命令设置过（0 = 仍未设置，不等于“显式设为空”）
int32_t sc_x, sc_y, sc_w, sc_h;
        float dr_n, dr_f;
int32_t tex_pool, samp_pool;
int32_t ubo_stage, ubo_index; uint64_t ubo_addr, ubo_size;
int32_t tex_stage, tex_first, tex_n;
int32_t vb_first, vb_n;
        float blend[4];
        float poly_factor, poly_units, poly_clamp;
int32_t stencil_face, stencil_vmask, stencil_mask, stencil_ref, stencil_which;
int32_t prim_restart_on; uint32_t prim_restart_index;
int32_t depth_clamp_on, render_cond_mode; uint64_t render_cond_addr;
        uint32_t tex_pool_n, samp_pool_n;
        int tf_enabled;
        uint64_t ssbo_bound[6];     // NVN stages 0..5, bit per storage slot
        struct { uint64_t addr, size; } ssbo[6][16];
        uint64_t ubo_bound[6];      // 每个 NVN 阶段的逐槽绑定状态（bit i = cbuf i 已绑定）
        // 不再存“曾经溢出”的粘性标志：它由 ubo_bound 在 draw 时现算，
        // 否则一次绑过槽≥2 之后整局每一笔都被标成溢出，身份和缺口报告都不再对应真实状态。
        uint32_t vattr_fmt[32];
        uint8_t state_obj[6][0x18];
        uint8_t state_seen;
        uint8_t blend_obj[8][0x18];   // 按 NVNblendState.target 分槽（MRT 逐槽混合）
        uint8_t blend_seen;
        struct { uint32_t format, offset, stream; } vattr[32];
        uint32_t vstream_stride[16], vstream_divisor[16];
        uint64_t vbuf_addr[16], vbuf_size[16];
        struct { uint64_t addr, size; } ubo[6][16];
        uint64_t last_ub_addr[6][16], last_ub_bytes[6][16];
        uint32_t last_ub_count[6][16];
        uint64_t tex_handle[6][32];
        uint8_t tex_present[6][32];
        uint64_t samp_handle[6][32];
        uint8_t samp_present[6][32];
        float ds_depth; int32_t ds_depth_mask, ds_stencil, ds_stencil_mask;
    } st;
    int rc_skip;                                    // 当前 SetRenderEnable 块求值为“不绘制”
    struct { uint64_t addr; int kind; } rc_pend[64];  // 本次提交里已回放的报告：kind 1 = 时间戳（counter 0）、2 = 其它（未知）
    int rc_npend;
};
static struct replay_state g_state;

// ---- 条件渲染（NVN SetRenderEnable / SetRenderEnableConditional）----
// 语义（与 Apple 直连同一口径，见 nvn_mtl.m 的 rc_*）：条件在回放时按内存里的报告求值；
// 本次提交里已经回放过的报告还没被 GPU 写回，时间戳类报告的 counter 域已知是 0，
// samples-passed 类未知 → 条件未知时照常绘制（遮挡剔除只会省工作量，不会画错）。
static uint64_t g_rc_skipped_draws, g_rc_skipped_clears;
static void rc_note(struct replay_state *S, uint64_t addr, int kind) {
    if (!addr) return;
    for (int i = 0; i < S->rc_npend; i++) if (S->rc_pend[i].addr == addr) { S->rc_pend[i].kind = kind; return; }
    if (S->rc_npend < (int)(sizeof S->rc_pend / sizeof S->rc_pend[0])) {
        S->rc_pend[S->rc_npend].addr = addr; S->rc_pend[S->rc_npend].kind = kind; S->rc_npend++;
    }
}
static int rc_value(struct replay_state *S, const struct s2a_record_memory *m, uint64_t addr, uint64_t *v) {
    for (int i = S->rc_npend - 1; i >= 0; i--) if (S->rc_pend[i].addr == addr) {
        if (S->rc_pend[i].kind != 1) return 0;   // 非时间戳报告：编码时值未知
        *v = 0; return 1;                         // 时间戳报告：counter 域是 0
    }
    if (!addr || !m || m->read(m->opaque, addr, v, 8)) return 0;
    return 1;
}
// 诊断：每种状态对象历史上被真实绑定过多少次（跨提交累计，不受 reset 影响）。
static uint32_t g_state_binds[6];
unsigned s2a_record_state_bind_count(int kind) { return kind >= 0 && kind < 6 ? g_state_binds[kind] : 0; }

void s2a_record_reset_state(void) {
    memset(&g_state, 0, sizeof g_state);
    g_rc_skipped_draws = g_rc_skipped_clears = 0;
    g_state.st.tex_pool = g_state.st.samp_pool = -1;
    // 深度范围无 seen 位，未设时按 NVN 新命令缓冲默认 {0,1}——全 0 会让视口
    // maxDepth=0 把一切 z 裁掉（#1409 同族：未绑状态零值≠默认值）。
    g_state.st.dr_f = 1.0f;
}

/* Compute accounting is independent of per-command replay state. No asset bytes.
 * Execution is owner-serialized, but Stop/snapshot may run on another thread. */
static pthread_mutex_t g_compute_mu = PTHREAD_MUTEX_INITIALIZER;
static struct s2a_record_compute_stats g_compute;
int s2a_record_compute_session_begin(uint64_t session, uint64_t generation) {
    if (!session || !generation) return -1;
    pthread_mutex_lock(&g_compute_mu);
    int rc = 0;
    if (g_compute.active) {
        rc = g_compute.session == session && g_compute.generation == generation ? 0 : -1;
    } else {
        memset(&g_compute, 0, sizeof g_compute);
        g_compute.session = session; g_compute.generation = generation; g_compute.active = 1;
    }
    pthread_mutex_unlock(&g_compute_mu);
    return rc;
}
int s2a_record_compute_trace(int enabled) {
    pthread_mutex_lock(&g_compute_mu);
    int rc = g_compute.active ? 0 : -1;
    if (!rc) g_compute.trace = enabled == 1;
    pthread_mutex_unlock(&g_compute_mu);
    return rc;
}
void s2a_record_compute_snapshot(struct s2a_record_compute_stats *out) {
    if (!out) return;
    pthread_mutex_lock(&g_compute_mu); *out = g_compute; pthread_mutex_unlock(&g_compute_mu);
}
/* Arbitrary compiler/cache errors may contain paths or asset content. Only
 * exact, fixed vocabulary enters exported counters/logs. Internal errors stay
 * unchanged; unknown codes intentionally group unclassified failures. */
static const char *compute_reason_code(const char *why) {
    if (!why || !*why) return "";
    static const struct { const char *text, *code; } known[] = {
        {"dispatch payload too small", "payload_invalid"},
        {"indirect dispatch needs GPU-written group counts (not wired yet)", "indirect_unsupported"},
        {"compute retention ledger full", "retention_full"},
        {"dispatch without CS/group counts", "dispatch_incomplete"},
        {"invalid compute builder output", "builder_invalid"},
        {"missing compute descriptor set", "descriptor_missing"},
        {"compute command allocation failed", "command_alloc_failed"},
        {"compute completion unknown; resources retained", "completion_unknown"},
        {"compute never submitted", "never_submitted"},
        {"compute completion copyback failed", "copyback_failed"},
        {"compute miss: live compilation disabled", "live_disabled"},
        {"compute provider context unavailable", "provider_unavailable"},
        {"compute provider is stale or stopped", "provider_stale"},
        {"compute provider changed during cache lookup", "provider_changed_cache"},
        {"compute provider changed during compilation", "provider_changed_compile"},
    };
    for (unsigned i=0;i<sizeof known/sizeof known[0];i++)
        if (!strcmp(why,known[i].text)) return known[i].code;
    return "unknown_failure";
}
void s2a_record_compute_note(const struct s2a_compute_diagnostic *d,
    enum s2a_compute_outcome outcome, const char *why) {
    const char *reason = compute_reason_code(why);
    struct s2a_compute_diagnostic unknown = {0};
    if (!d) d = &unknown;
    int trace = 0;
    pthread_mutex_lock(&g_compute_mu);
    if (!g_compute.active || (d->session && (d->session != g_compute.session || d->generation != g_compute.generation))) {
        g_compute.stale_events++; pthread_mutex_unlock(&g_compute_mu); return;
    }
    g_compute.dispatches++;
    unsigned source = (unsigned)d->source < S2A_COMPUTE_SOURCE_COUNT ? (unsigned)d->source : S2A_COMPUTE_UNKNOWN;
    g_compute.sources[source]++;
    switch (outcome) {
    case S2A_COMPUTE_COMPLETED: g_compute.submitted++; g_compute.completed++; break;
    case S2A_COMPUTE_COMPLETION_UNKNOWN: g_compute.submitted++; g_compute.unknown_completion++; break;
    case S2A_COMPUTE_COPYBACK_FAILED: g_compute.submitted++; g_compute.copyback_failed++; break;
    case S2A_COMPUTE_UNSUBMITTED: g_compute.unsubmitted++; break;
    default: g_compute.build_failed++; break;
    }
    if (*reason) {
        size_t n = strlen(reason); int slot = -1;
        for (int i=0;i<S2A_COMPUTE_REASON_SLOTS;i++) {
            if (g_compute.reasons[i].count && !strcmp(g_compute.reasons[i].why,reason)) { slot=i; break; }
            if (slot<0 && !g_compute.reasons[i].count) slot=i;
        }
        if (slot<0) g_compute.reason_overflow++;
        else { if (!g_compute.reasons[slot].count) memcpy(g_compute.reasons[slot].why,reason,n+1); g_compute.reasons[slot].count++; }
    }
    if (g_compute.trace && g_compute.trace_lines < 64) { g_compute.trace_lines++; trace=1; }
    pthread_mutex_unlock(&g_compute_mu);
    if (trace) {
        static const char *source_names[]={"unknown","offline","private","live","miss","cached"};
        static const char *outcome_names[]={"build_failed","unsubmitted","completed","completion_unknown","copyback_failed"};
        const char *outcome_name = (unsigned)outcome < sizeof outcome_names/sizeof outcome_names[0] ? outcome_names[outcome] : "build_failed";
        char identity[65] = "unavailable";
        if (d->identity_valid) for(unsigned i=0;i<32;i++) snprintf(identity+2*i,3,"%02x",d->identity[i]);
        __android_log_print(ANDROID_LOG_INFO,"S2Arm","compute dispatch: session=%llu generation=%llu identity=%s source=%s outcome=%s reason=%s",
            (unsigned long long)d->session,(unsigned long long)d->generation,identity,source_names[source],outcome_name,reason);
    }
}
int s2a_record_compute_session_end(void) {
    struct s2a_record_compute_stats stats;
    pthread_mutex_lock(&g_compute_mu);
    if (!g_compute.active) { pthread_mutex_unlock(&g_compute_mu); return 0; }
    g_compute.active=0; stats=g_compute;
    pthread_mutex_unlock(&g_compute_mu);
    char reasons[3000]; size_t used=0; unsigned omitted=0; reasons[0]=0;
    for(int i=0;i<S2A_COMPUTE_REASON_SLOTS;i++) if(stats.reasons[i].count) {
        char entry[224];
        int n=snprintf(entry,sizeof entry," [%s]=%llu",stats.reasons[i].why,(unsigned long long)stats.reasons[i].count);
        if(n<0 || (size_t)n>=sizeof entry || (size_t)n>=sizeof reasons-used) { omitted++; continue; }
        for(int j=0;j<n;j++) if(entry[j]=='\n' || entry[j]=='\r') entry[j]=' ';
        memcpy(reasons+used,entry,(size_t)n+1); used+=(size_t)n;
    }
    __android_log_print(ANDROID_LOG_INFO,"S2Arm","compute session: session=%llu generation=%llu dispatches=%llu offline=%llu private=%llu live=%llu miss=%llu cached=%llu unknown_source=%llu submitted=%llu completed=%llu completion_unknown=%llu unsubmitted=%llu build_failed=%llu copyback_failed=%llu reason_overflow=%llu stale=%llu reason_log_omitted=%u%s",
        (unsigned long long)stats.session,(unsigned long long)stats.generation,(unsigned long long)stats.dispatches,
        (unsigned long long)stats.sources[S2A_COMPUTE_OFFLINE],(unsigned long long)stats.sources[S2A_COMPUTE_PRIVATE],
        (unsigned long long)stats.sources[S2A_COMPUTE_LIVE],(unsigned long long)stats.sources[S2A_COMPUTE_MISS],
        (unsigned long long)stats.sources[S2A_COMPUTE_CACHED],
        (unsigned long long)stats.sources[S2A_COMPUTE_UNKNOWN],(unsigned long long)stats.submitted,(unsigned long long)stats.completed,
        (unsigned long long)stats.unknown_completion,(unsigned long long)stats.unsubmitted,(unsigned long long)stats.build_failed,
        (unsigned long long)stats.copyback_failed,(unsigned long long)stats.reason_overflow,(unsigned long long)stats.stale_events,omitted,reasons);
    return 1;
}

static int fail(struct s2a_record_result *r, int code, uint32_t opcode, const char *why) {
    r->code = code; r->opcode = opcode;
    snprintf(r->why, sizeof r->why, "%s", why);
    return code;
}

// ---- 对象/提交记账：每次创建/销毁都计数，测试可核“正常完成后恰好回收一次”。 ----
static uint64_t g_cmd_created, g_cmd_freed;
static uint64_t g_fence_created, g_fence_freed;
static uint64_t g_buffer_created, g_buffer_freed;
static uint64_t g_memory_created, g_memory_freed;
static uint64_t g_image_created, g_image_freed;
static uint64_t g_submit_completed, g_submit_unsubmitted, g_submit_incomplete;
static uint64_t g_retained_overflow;   // 账本条目数直接取 g_retained_n

static int create_visible(struct s2a_vk_device *d, VkDeviceSize size, VkBuffer *buffer,
                          VkDeviceMemory *memory, void **mapped) {
    if (size == 0) size = 4;
    VkBufferCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->device, &info, NULL, buffer) != VK_SUCCESS) return -1;
    g_buffer_created++;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, *buffer, &req);
    VkMemoryAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = s2a_vk_memory_type(d, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(d->device, &alloc, NULL, memory) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, *buffer, NULL); g_buffer_freed++;
        *buffer = VK_NULL_HANDLE;
        return -1;
    }
    g_memory_created++;
    if (vkBindBufferMemory(d->device, *buffer, *memory, 0) != VK_SUCCESS ||
        vkMapMemory(d->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, *buffer, NULL); g_buffer_freed++;
        vkFreeMemory(d->device, *memory, NULL); g_memory_freed++;
        *buffer = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

static void destroy_visible(struct s2a_vk_device *d, VkBuffer buffer, VkDeviceMemory memory, void *mapped) {
    if (mapped) vkUnmapMemory(d->device, memory);
    if (buffer) { vkDestroyBuffer(d->device, buffer, NULL); g_buffer_freed++; }
    if (memory) { vkFreeMemory(d->device, memory, NULL); g_memory_freed++; }
}

// 首个 GPU 错误（保留现场：失败调用、VkResult、提交序号、图像身份）。后续错误不覆盖首错。
static char g_gpu_first_error[512];
static uint64_t g_submit_seq;

// 诊断：op=26 的墙钟里，“打包这条记录”与“回放入口→draw builder”各占多少。没有这两个数，
// 程序查找与 first_draw 之外那段时间只能靠相减猜。
#include <time.h>
static _Atomic uint64_t g_dbg_pkt_ns, g_dbg_pkt_n;      // 打包 = malloc + 复制记录 + 补 END
static _Atomic uint64_t g_dbg_free_ns;                   // free 单列（同属打包，但可分辨分配器成本）
static _Atomic uint64_t g_dbg_replay_ns;                  // replay 本体（逐包执行）
static _Atomic uint64_t g_dbg_tobuilder_ns, g_dbg_tobuilder_n;
static inline uint64_t rec_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
// 逐笔 draw 落点计时（打包/free/replay/入口→builder）的开关：沿用 debug.s2a.perf_timing，
// 默认关。报告 §6.4/§7：这笔成本（每笔 draw 最多 6 次 clock_gettime + 若干 atomic）原先在默认
// 路径上无条件付；只有打开才采，关闭时不取钟、不累加、也不打「draw 落点」行。
// 属性首读定值，改属性要重启进程（与其它诊断属性一致）。
static int rec_perf_timing_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.perf_timing", b);
        v = b[0] ? atoi(b) : 0;
    }
    return v;
}

// 整幅 queued clear 的耗时窗口（诊断，开关 debug.s2a.clear_timing，默认关）。
// 覆盖 OP_CLEAR_COLOR 记录本身：附件/pass 取用 + 命令缓冲分配 + 录制 + enqueue（都在 vk_clear.c
// 的 s2a_vk_clear_color_queued 里），并按记录分开数「整幅入队」与「退回同步」。
// 报告 §3.3/§4.4：这条路径原先没有任何打点；每 clear 记录 ≤2 次 clock（8-12 笔/帧）。
static _Atomic uint64_t g_clear_ns, g_clear_n, g_clear_queued, g_clear_fallback;
static int rec_clear_stats_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.clear_timing", b);
        v = (b[0] && atoi(b)) ? 1 : 0;
    }
    return v;
}
// 常量更新诊断（默认关，属性 debug.s2a.ub_diag=1）：每笔 UpdateUniformBuffer 都要扫 6×16 个
// UBO 槽找“这次更新落在哪个已绑定槽里”，并按笔把整张表复制进 draw 状态。两者都只被
// nvn_vk_first_draw 的 trace 行读取（诊断），却默认走在热路径上：营地场景 946 笔/帧 × 96 次比较，
// 另有 560 笔/帧 × 3 次 memcpy（约 1.9 KB/笔）。关闭时不扫、不复制；draw 状态 memset 后的零值
// 就是“没见过”的原语义。
static int rec_ub_diag_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.ub_diag", b);
        v = (b[0] && atoi(b)) ? 1 : 0;
    }
    return v;
}

// 读取即清零；窗口由调用方（present 的 16 帧块）决定。
void vk_record_take_clear_stats(uint64_t *ns, uint64_t *n, uint64_t *queued, uint64_t *fallback) {
    if (ns) *ns = atomic_exchange(&g_clear_ns, 0);
    if (n) *n = atomic_exchange(&g_clear_n, 0);
    if (queued) *queued = atomic_exchange(&g_clear_queued, 0);
    if (fallback) *fallback = atomic_exchange(&g_clear_fallback, 0);
}

static void note_gpu_error(const char *call, VkResult rc, const char *detail) {
    if (g_gpu_first_error[0]) return;
    snprintf(g_gpu_first_error, sizeof g_gpu_first_error, "%s rc=%d submit=#%llu %s",
             call, (int)rc, (unsigned long long)g_submit_seq, detail ? detail : "");
}
const char *s2a_record_first_gpu_error(void) { return g_gpu_first_error; }

// 一次提交的三种结果：调用方必须分开收尾。UNSUBMITTED = 命令从未执行；INCOMPLETE = 已执行但
// 没有完成证明（Vulkan 禁止释放/复用），对象所有权移入保留账本。
#define REC_SUBMIT_COMPLETED 0
#define REC_SUBMIT_UNSUBMITTED (-1)
#define REC_SUBMIT_INCOMPLETE (-2)

// 保留账本：已提交但没拿到完成证明的对象。这些对象由本次执行器创建，回收所有权也在这里；
// 不能用“后来某次提交完成了”推断它们已经完成，也不能交给设备销毁去冒充显式回收。
// 账本按设备归属：设备句柄变化时旧对象已随旧设备消失，只清账、不再 destroy。
#define REC_RETAINED_MAX 64
struct rec_keep {
    VkBuffer buf[2]; VkDeviceMemory mem[2]; int nbuf;
    VkImage image; VkDeviceMemory image_mem;
    // Compute-only ownership; every graphics keep remains zero in these fields.
    void *compute_owner;
    int (*compute_finish)(struct s2a_vk_device *, void *, int);
    uint64_t pool_seq;  // 未入队后自提：保活直到这次提交有完成证明
};
struct rec_retained_entry {
    VkFence fence;
    VkCommandBuffer cmd;
    struct rec_keep keep;
};
static struct rec_retained_entry g_retained[REC_RETAINED_MAX];
static int g_retained_n;
static VkDevice g_retained_dev;

static void recount_device(struct s2a_vk_device *d) {
    if (!d) return;
    if (g_retained_dev && g_retained_dev != d->device) {
        fprintf(stderr, "[vk-record] retained %d object set(s) belong to a previous device; "
                        "they are gone with it (no explicit destroy, no fake completion)\n", g_retained_n);
        memset(g_retained, 0, sizeof g_retained);
        g_retained_n = 0;
    }
    g_retained_dev = d->device;
}

static int g_submit_fault;   // 诊断注入：1 = 提交后按完成未知；2 = 假装提交失败
void s2a_record_set_submit_fault(int mode) { g_submit_fault = mode; }

// ---- OP_DISPATCH 帧队列收编（issue #48；默认关：debug.s2a.dispatch_fq / 宿主 S2A_DISPATCH_FQ）----
// 门开且全部 guest-backed 绑定命中 AHB 池后备（builder 置 out.fq=1）时，把 dispatch 录进一段
// 独立命令缓冲交帧队列统一提交/退休，省掉“每笔 dispatch 一次独立 submit + 立刻等 fence”的
// 同步开销。门关时 dispatch 仍按原有顺序同步提交。
static _Atomic int g_dispatch_fq_gate = -1;
static int dispatch_fq_on(void) {
    int v = atomic_load(&g_dispatch_fq_gate);
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dispatch_fq", b);
        v = b[0] == '1';
        if (!v) {
            const char *e = getenv("S2A_DISPATCH_FQ");
            v = e && e[0] == '1';
        }
        atomic_store(&g_dispatch_fq_gate, v);
    }
    return v;
}
int s2a_record_dispatch_fq_enabled(void) { return dispatch_fq_on(); }
static _Atomic uint64_t g_dispatch_fq_enq, g_dispatch_fq_fallback;

// pending_fq：帧队列收编的在途 dispatch owner 表。与 g_retained 的区别：命令缓冲所有权在
// 帧队列（不在本执行器），所以这里不记 fence/cmd，只记“等完成证明再 finish”的 owner。
// 完成通知携带实际完成的帧号，只释放归属于该帧的 owner。
#define REC_FQ_PENDING_MAX 64
struct rec_fq_pending {
    struct s2a_vk_device *d;
    void *owner;
    int (*finish)(struct s2a_vk_device *, void *, int);
    uint64_t frame_id;
    struct s2a_compute_diagnostic diag;
};
static struct rec_fq_pending g_fq_pend[REC_FQ_PENDING_MAX];
static int g_fq_pend_n;
static VkDevice g_fq_pend_dev;
void s2a_record_dispatch_fq_stats(uint64_t *enqueued, uint64_t *fallback, uint64_t *pending) {
    if (enqueued) *enqueued = atomic_load(&g_dispatch_fq_enq);
    if (fallback) *fallback = atomic_load(&g_dispatch_fq_fallback);
    if (pending) *pending = (uint64_t)g_fq_pend_n;
}
// 与 g_retained 同口径：旧设备上的在途项随设备消失，只清账、不在新设备上 finish。
static void fq_pend_check_device(struct s2a_vk_device *d) {
    if (g_fq_pend_dev && d && g_fq_pend_dev != d->device && g_fq_pend_n) {
        fprintf(stderr, "[vk-record] %d queued dispatch owner(s) belong to a previous device; "
                        "gone with it (no explicit finish)\n", g_fq_pend_n);
        memset(g_fq_pend, 0, sizeof g_fq_pend);
        g_fq_pend_n = 0;
    }
    if (d) g_fq_pend_dev = d->device;
}
// 只结算明确获得完成证明的帧；其他延迟槽可能仍在飞行。
static void fq_pend_drain(uint64_t completed_frame) {
    int left = 0;
    for (int i = 0; i < g_fq_pend_n; ++i) {
        struct rec_fq_pending e = g_fq_pend[i];
        if (e.frame_id != completed_frame || !e.d || !e.d->device) { g_fq_pend[left++] = e; continue; }
        int rc = e.finish ? e.finish(e.d, e.owner, 0) : 0;
        s2a_record_compute_note(&e.diag,
            rc ? S2A_COMPUTE_COPYBACK_FAILED : S2A_COMPUTE_COMPLETED,
            rc ? "compute completion copyback failed" : "");
    }
    memset(g_fq_pend + left, 0, (size_t)(g_fq_pend_n - left) * sizeof g_fq_pend[0]);
    g_fq_pend_n = left;
}
// 完成未知（槽退休失败 / pass_break 返回非 0）：逐项 finish(-2)——按契约一个对象都不释放，
// 全部留在表里等后面的完成证明（或随设备销毁）。
static void fq_pend_unknown(void) {
    for (int i = 0; i < g_fq_pend_n; ++i)
        if (g_fq_pend[i].d && g_fq_pend[i].d->device && g_fq_pend[i].finish)
            (void)g_fq_pend[i].finish(g_fq_pend[i].d, g_fq_pend[i].owner, -2);
}
void s2a_record_fq_drain(int ok) {
    if (!ok) fq_pend_unknown();
}
void s2a_record_fq_retire(uint64_t completed_frame) {
    if (completed_frame) fq_pend_drain(completed_frame);
}

void s2a_record_gpu_stats(struct s2a_record_gpu_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->cmd_created = g_cmd_created; out->cmd_freed = g_cmd_freed;
    out->fence_created = g_fence_created; out->fence_freed = g_fence_freed;
    out->buffer_created = g_buffer_created; out->buffer_freed = g_buffer_freed;
    out->memory_created = g_memory_created; out->memory_freed = g_memory_freed;
    out->image_created = g_image_created; out->image_freed = g_image_freed;
    out->submit_completed = g_submit_completed;
    out->submit_unsubmitted = g_submit_unsubmitted;
    out->submit_incomplete = g_submit_incomplete;
    out->retained_entries = (uint64_t)g_retained_n;
    out->retained_overflow = g_retained_overflow;
}

static void rec_free_command(struct s2a_vk_device *d, VkCommandBuffer cmd) {
    if (!cmd) return;
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
    g_cmd_freed++;
}

int s2a_record_release_retained(struct s2a_vk_device *d, uint64_t timeout_ns, uint32_t *left_out) {
    if (left_out) *left_out = 0;
    if (!d || !d->device || !g_retained_n) return 0;
    if (g_retained_dev && g_retained_dev != d->device) {
        // 旧设备的对象已随设备消失：不能在新设备上等待/销毁。
        fprintf(stderr, "[vk-record] %d retained object set(s) belong to a previous device; cleared without destroy\n",
                g_retained_n);
        memset(g_retained, 0, sizeof g_retained);
        g_retained_n = 0;
        return 0;
    }
    int64_t deadline = (int64_t)(timeout_ns ? timeout_ns : 0);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline += (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    int reclaimed = 0, left = 0;
    for (int i = 0; i < g_retained_n; i++) {
        struct rec_retained_entry e_copy = g_retained[i];
        struct rec_retained_entry *e = &e_copy;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
        int64_t left_ns = deadline - now;
        VkResult wr = VK_TIMEOUT;
        if (left_ns > 0) wr = vkWaitForFences(d->device, 1, &e->fence, VK_TRUE, (uint64_t)left_ns);
        if (wr != VK_SUCCESS) {
            // 没有完成证明：原样保留（压缩到数组前部），随设备销毁。
            g_retained[left++] = e_copy;
            continue;
        }
        // 到这里有完成证明：可以显式回收。
        vkDestroyFence(d->device, e->fence, NULL); g_fence_freed++;
        if (e->cmd) { rec_free_command(d, e->cmd); }
        // A fence success and command retirement precede copyback/owner release.
        if (e->keep.compute_finish &&
            e->keep.compute_finish(d, e->keep.compute_owner, REC_SUBMIT_COMPLETED))
            fprintf(stderr, "[vk-record] retired compute completion copyback failed\n");
        if (e->keep.pool_seq && s2a_poolahb_complete_seq)
            s2a_poolahb_complete_seq(e->keep.pool_seq);
        for (int b = 0; b < e->keep.nbuf; b++) {
            if (e->keep.buf[b]) { vkDestroyBuffer(d->device, e->keep.buf[b], NULL); g_buffer_freed++; }
            if (e->keep.mem[b]) { vkFreeMemory(d->device, e->keep.mem[b], NULL); g_memory_freed++; }
        }
        if (e->keep.image) { vkDestroyImage(d->device, e->keep.image, NULL); g_image_freed++; }
        if (e->keep.image_mem) { vkFreeMemory(d->device, e->keep.image_mem, NULL); g_memory_freed++; }
        reclaimed++;
    }
    // Compaction moved live owners to the prefix; erase only the retired tail.
    memset(g_retained + left, 0, (size_t)(g_retained_n - left) * sizeof g_retained[0]);
    g_retained_n = left;
    if (left) {
        // 没有完成证明：条目留在账本里（设备即将销毁，它们随设备回收）。不能 memset 掉再宣称回收。
        fprintf(stderr, "[vk-record] %d retained object set(s) have no completion proof before device destroy; "
                        "kept and reclaimed by device destruction (no fake completion)\n", left);
    }
    if (left_out) *left_out = (uint32_t)left;
    return reclaimed;
}

static int rec_retain(struct s2a_vk_device *d, VkFence fence, VkCommandBuffer cmd, const struct rec_keep *keep) {
    recount_device(d);
    if (g_retained_n >= REC_RETAINED_MAX) {
        // 账本满：仍然不能销毁仍在 GPU 上的对象（Vulkan 禁止），只记录失去集中记账，不伪造回收。
        static int noted;
        if (!noted++)
            fprintf(stderr, "[vk-record] retained-object table full (%d); in-flight objects are kept but "
                            "no longer tracked centrally\n", REC_RETAINED_MAX);
        g_retained_overflow++;
        return -1;
    }
    struct rec_retained_entry *e = &g_retained[g_retained_n++];
    memset(e, 0, sizeof *e);
    e->fence = fence;
    e->cmd = cmd;
    if (keep) e->keep = *keep;
    return 0;
}

// 一次性提交并等完成（命令缓冲已结束录制——供 dispatch 帧队列收编的“入队被拒后自提”路径）。
// 返回 REC_SUBMIT_*：
//   COMPLETED   —— 已提交且有完成证明；fence 已销毁，命令缓冲已回收（辅助资源仍归调用方）。
//   UNSUBMITTED —— 没有提交成功，命令从未执行；fence/命令缓冲已回收（辅助资源仍归调用方）。
//   INCOMPLETE  —— 已提交但没有完成证明；fence/命令缓冲/keep 里的资源全部转入保留账本，调用方不得释放/复用。
static int submit_recorded(struct s2a_vk_device *d, VkCommandBuffer cmd, const struct rec_keep *keep) {
    VkResult rc;
    VkFenceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    rc = vkCreateFence(d->device, &info, NULL, &fence);
    if (rc != VK_SUCCESS) {
        note_gpu_error("vkCreateFence", rc, NULL);
        rec_free_command(d, cmd);
        g_submit_unsubmitted++;
        return REC_SUBMIT_UNSUBMITTED;
    }
    g_fence_created++;
    if (g_submit_fault == 2) {
        // 负例：不提交。命令从未执行，fence/命令缓冲可以回收。
        vkDestroyFence(d->device, fence, NULL);
        g_fence_freed++;
        rec_free_command(d, cmd);
        g_submit_unsubmitted++;
        return REC_SUBMIT_UNSUBMITTED;
    }
    VkSubmitInfo submit_info = {0};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    rc = vkQueueSubmit(d->queue, 1, &submit_info, fence);
    if (rc != VK_SUCCESS) {
        note_gpu_error("vkQueueSubmit", rc, NULL);
        vkDestroyFence(d->device, fence, NULL);
        g_fence_freed++;
        rec_free_command(d, cmd);
        g_submit_unsubmitted++;
        return REC_SUBMIT_UNSUBMITTED;
    }
    if (g_submit_fault == 1) {
        rc = VK_TIMEOUT;   // 注入超时不是真实等待，不计入逐帧 fence 账。
    } else {
        // t-direct-frametimes：本次 fence 等待计入逐帧帧时间记录。先判 on()——
        // 模块不在或开关关都跳过一次分支判断，不计时也不记账（量的成本不默认付）。
        extern void nvn_vk_frame_log_fence_wait(uint64_t) __attribute__((weak));
        extern int nvn_vk_frame_log_on(void) __attribute__((weak));
        extern void nvn_vk_frame_tsv_fence_wait(uint64_t) __attribute__((weak));
        extern int nvn_vk_frame_tsv_on(void) __attribute__((weak));
        const int fl = nvn_vk_frame_log_on && nvn_vk_frame_log_on();
        const int ft = nvn_vk_frame_tsv_on && nvn_vk_frame_tsv_on();
        uint64_t wt0 = (fl || ft) ? rec_now_ns() : 0;
        rc = vkWaitForFences(d->device, 1, &fence, VK_TRUE, 5000000000ull);
        if (fl) nvn_vk_frame_log_fence_wait(rec_now_ns() - wt0);
        if (ft) nvn_vk_frame_tsv_fence_wait(rec_now_ns() - wt0);
    }
    if (rc != VK_SUCCESS) {
        // 提交成功但没拿到完成证明：fence/命令缓冲/辅助资源都不能释放或复用。
        note_gpu_error("vkWaitForFences(5s)", rc, NULL);
        g_submit_incomplete++;
        rec_retain(d, fence, cmd, keep);
        return REC_SUBMIT_INCOMPLETE;
    }
    vkDestroyFence(d->device, fence, NULL);
    g_fence_freed++;
    rec_free_command(d, cmd);
    g_submit_completed++;
    return REC_SUBMIT_COMPLETED;
}

// 常规一次性提交：先结束录制，再走 submit_recorded 的提交/等待/退休流程。
static int submit(struct s2a_vk_device *d, VkCommandBuffer cmd, const struct rec_keep *keep) {
    ++g_submit_seq;
    VkResult rc = vkEndCommandBuffer(cmd);
    if (rc != VK_SUCCESS) {
        note_gpu_error("vkEndCommandBuffer", rc, NULL);
        rec_free_command(d, cmd);
        g_submit_unsubmitted++;
        return REC_SUBMIT_UNSUBMITTED;
    }
    return submit_recorded(d, cmd, keep);
}

static VkCommandBuffer begin_command(struct s2a_vk_device *d) {
    VkCommandBufferAllocateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = d->command_pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d->device, &info, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_cmd_created++;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        rec_free_command(d, cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

static int gpu_fill(struct s2a_vk_device *d, uint64_t addr, uint64_t bytes, uint32_t value,
                    const struct s2a_record_memory *mem) {
    if (bytes % 4 || bytes > MAX_COPY) return -1;
    if (bytes == 0) return 0;
    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void *mapped = NULL;
    if (create_visible(d, bytes, &buffer, &memory, &mapped)) return -1;
    VkCommandBuffer cmd = begin_command(d);
    if (!cmd) { destroy_visible(d, buffer, memory, mapped); return -1; }
    vkCmdFillBuffer(cmd, buffer, 0, bytes, value);
    VkBufferMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    struct rec_keep keep;
    memset(&keep, 0, sizeof keep);
    keep.buf[0] = buffer; keep.mem[0] = memory; keep.nbuf = 1;
    int rc = submit(d, cmd, &keep);
    if (rc == REC_SUBMIT_INCOMPLETE) {
        // 命令已提交但没有完成证明：buffer/memory/命令都在保留账本里，不能写回 guest（CPU 读回不可信）。
        return -1;
    }
    if (rc == REC_SUBMIT_COMPLETED && mem->write(mem->opaque, addr, mapped, (size_t)bytes) != 0) rc = -1;
    destroy_visible(d, buffer, memory, mapped);
    return rc;
}

static int gpu_clear_image_range(struct s2a_vk_device *d, VkImage image, VkClearColorValue color,
                                 uint32_t base_level, uint32_t level_count,
                                 uint32_t base_layer, uint32_t layer_count) {
    if (!image) return -1;
    if (!level_count) level_count = 1;
    if (!layer_count) layer_count = 1;
    VkCommandBuffer cmd = begin_command(d);
    if (!cmd) return -1;
    VkImageMemoryBarrier to_general = {0};
    to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = image;
    to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_general.subresourceRange.baseMipLevel = base_level;
    to_general.subresourceRange.levelCount = level_count;
    to_general.subresourceRange.baseArrayLayer = base_layer;
    to_general.subresourceRange.layerCount = layer_count;
    to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_general);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, base_level, level_count, base_layer, layer_count};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    VkImageMemoryBarrier done = to_general;
    done.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    done.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 0, NULL, 1, &done);
    return submit(d, cmd, NULL) == REC_SUBMIT_COMPLETED ? 0 : -1;
}

static int gpu_clear_image(struct s2a_vk_device *d, VkImage image, VkClearColorValue color) {
    return gpu_clear_image_range(d, image, color, 0, 1, 0, 1);
}

// 全通道、局部区域的清除：临时图像 GPU 清色 + vkCmdCopyImage 区域拷贝，保留区域外像素。
static int clear_image_region_copy(struct s2a_vk_device *d, const struct s2a_replay_texture *tex,
                                          const p_clear_tex *record) {
    VkImageCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = tex->format;
    ci.extent = (VkExtent3D){tex->width, tex->height, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage temp = VK_NULL_HANDLE;
    if (vkCreateImage(d->device, &ci, NULL, &temp) != VK_SUCCESS) return -1;
    g_image_created++;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d->device, temp, &req);
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, req.memoryTypeBits, 0, NULL);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &ai, NULL, &memory) != VK_SUCCESS ||
        vkBindImageMemory(d->device, temp, memory, 0) != VK_SUCCESS) {
        vkDestroyImage(d->device, temp, NULL); g_image_freed++;
        if (memory) { vkFreeMemory(d->device, memory, NULL); g_memory_freed++; }
        return -1;
    }
    g_memory_created++;
    VkCommandBuffer cmd = begin_command(d);
    if (!cmd) {
        vkDestroyImage(d->device, temp, NULL); g_image_freed++;
        vkFreeMemory(d->device, memory, NULL); g_memory_freed++;
        return -1;
    }
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = temp;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue color = {0};
    switch (record->value_type) {
    case CLEAR_TEX_FLOAT: memcpy(color.float32, record->c.f, sizeof color.float32); break;
    case CLEAR_TEX_UINT: memcpy(color.uint32, record->c.u, sizeof color.uint32); break;
    case CLEAR_TEX_INT: memcpy(color.int32, record->c.i, sizeof color.int32); break;
    default: return -1;
    }
    VkImageSubresourceRange full = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, temp, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &full);
    VkImageMemoryBarrier dst_barrier = b;
    dst_barrier.image = tex->image;
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; dst_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst_barrier.srcAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &dst_barrier);
    VkImageMemoryBarrier src_barrier = b;
    src_barrier.image = temp;
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &src_barrier);
    VkImageCopy copy = {0};
    copy.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.srcOffset = (VkOffset3D){record->r.xoffset, record->r.yoffset, 0};
    copy.dstOffset = copy.srcOffset;
    copy.extent = (VkExtent3D){(uint32_t)record->r.width, (uint32_t)record->r.height, 1};
    vkCmdCopyImage(cmd, temp, VK_IMAGE_LAYOUT_GENERAL, tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    dst_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 0, NULL, 1, &dst_barrier);
    struct rec_keep keep;
    memset(&keep, 0, sizeof keep);
    keep.image = temp; keep.image_mem = memory;
    if (submit(d, cmd, &keep) != REC_SUBMIT_COMPLETED) {
        // 完成未知：临时图像/内存/命令归保留账本（不能销毁仍在 GPU 上的对象）。
        return -1;
    }
    vkDestroyImage(d->device, temp, NULL); g_image_freed++;
    vkFreeMemory(d->device, memory, NULL); g_memory_freed++;
    return 0;
}

static int clear_format_bpp(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT:
        return 4;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_UINT:
        return 16;
    default:
        return 0;
    }
}

// 局部/通道清除：先用真实 GPU 拷贝读回，按 mask/region 更新，再拷回。
// 这是 Vulkan 传输 + fence 完成，不是 CPU 伪造；性能不如 vkCmdClearColorImage，首版优先正确性。
// 两次提交都按 REC_SUBMIT_* 分类：读回没有完成证明时不能改 CPU 副本、也不能继续写回。
static int clear_image_partial_cpu(struct s2a_vk_device *d, const struct s2a_replay_texture *tex,
                                   const p_clear_tex *record) {
    if (!d || !tex || !tex->image || !record) return -1;
    int bpp = clear_format_bpp(tex->format);
    if (!bpp || record->r.depth != 1 || record->r.xoffset < 0 || record->r.yoffset < 0 ||
        record->r.width <= 0 || record->r.height <= 0 ||
        (uint32_t)(record->r.xoffset + record->r.width) > tex->width ||
        (uint32_t)(record->r.yoffset + record->r.height) > tex->height ||
        (record->mask & ~0xF) || record->mask == 0) return -1;
    size_t image_bytes = (size_t)tex->width * tex->height * (size_t)bpp;
    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void *mapped = NULL;
    if (create_visible(d, image_bytes, &buffer, &memory, &mapped)) return -1;
    struct rec_keep keep;
    memset(&keep, 0, sizeof keep);
    keep.buf[0] = buffer; keep.mem[0] = memory; keep.nbuf = 1;
    VkCommandBuffer cmd = begin_command(d);
    if (!cmd) { destroy_visible(d, buffer, memory, mapped); return -1; }
    VkImageMemoryBarrier to_transfer = {0};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = tex->image;
    to_transfer.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_transfer.srcAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = (VkExtent3D){tex->width, tex->height, 1};
    vkCmdCopyImageToBuffer(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    VkBufferMemoryBarrier to_host = {0};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = buffer; to_host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &to_host, 0, NULL);
    int sr = submit(d, cmd, &keep);
    if (sr != REC_SUBMIT_COMPLETED) {
        // 未提交：命令从未执行，buffer 可释放；完成未知：buffer 已归保留账本，不能释放/复用。
        if (sr == REC_SUBMIT_UNSUBMITTED) destroy_visible(d, buffer, memory, mapped);
        return -1;
    }

    for (int32_t y = 0; y < record->r.height; y++) {
        for (int32_t x = 0; x < record->r.width; x++) {
            uint8_t *pixel = (uint8_t *)mapped +
                ((size_t)(record->r.yoffset + y) * tex->width + (uint32_t)(record->r.xoffset + x)) * (size_t)bpp;
            for (int c = 0; c < 4; c++) {
                if (!(record->mask & (1 << c))) continue;
                if (bpp == 4) {
                    if (tex->format == VK_FORMAT_R8G8B8A8_SINT) {
                        pixel[c] = (uint8_t)(int8_t)record->c.i[c];
                    } else if (tex->format == VK_FORMAT_R8G8B8A8_UINT) {
                        pixel[c] = (uint8_t)record->c.u[c];
                    } else if (tex->format == VK_FORMAT_R8G8B8A8_SRGB ||
                               tex->format == VK_FORMAT_R8G8B8A8_UNORM) {
                        float v = record->c.f[c];
                        if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
                        pixel[c] = (uint8_t)lrintf(v * 255.0f);
                    } else { destroy_visible(d, buffer, memory, mapped); return -1; }
                } else {
                    uint8_t *channel = pixel + (size_t)c * 4;
                    if (tex->format == VK_FORMAT_R32G32B32A32_SFLOAT) {
                        float v = record->c.f[c]; memcpy(channel, &v, 4);
                    } else if (tex->format == VK_FORMAT_R32G32B32A32_SINT) {
                        int32_t v = record->c.i[c]; memcpy(channel, &v, 4);
                    } else if (tex->format == VK_FORMAT_R32G32B32A32_UINT) {
                        uint32_t v = record->c.u[c]; memcpy(channel, &v, 4);
                    } else { destroy_visible(d, buffer, memory, mapped); return -1; }
                }
            }
        }
    }

    VkMappedMemoryRange flush = {0};
    flush.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    flush.memory = memory; flush.offset = 0; flush.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(d->device, 1, &flush);
    cmd = begin_command(d);
    if (!cmd) { destroy_visible(d, buffer, memory, mapped); return -1; }
    VkImageMemoryBarrier to_write = to_transfer;
    to_write.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_write.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_write);
    VkBufferMemoryBarrier from_host = to_host;
    from_host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    from_host.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &from_host, 0, NULL);
    vkCmdCopyBufferToImage(cmd, buffer, tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    VkImageMemoryBarrier done = to_transfer;
    done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 0, NULL, 1, &done);
    sr = submit(d, cmd, &keep);
    if (sr != REC_SUBMIT_COMPLETED) {
        if (sr == REC_SUBMIT_UNSUBMITTED) destroy_visible(d, buffer, memory, mapped);
        return -1;
    }
    destroy_visible(d, buffer, memory, mapped);
    return 0;
}

// 适配层注册的动态解析器：静态表未命中时按 key 现解析一张纹理（线程局部，避免污染测试）。
static _Thread_local s2a_record_tex_resolve g_tex_resolve;
static _Thread_local void *g_tex_resolve_opaque;
static _Thread_local struct s2a_replay_texture g_tex_scratch;

void s2a_record_set_tex_resolver(s2a_record_tex_resolve resolve, void *opaque) {
    g_tex_resolve = resolve;
    g_tex_resolve_opaque = opaque;
}

static _Thread_local int g_keep_state;
static _Thread_local s2a_record_draw_fn g_draw_builder;
static _Thread_local void *g_draw_builder_opaque;

void s2a_record_set_draw_builder(s2a_record_draw_fn draw, void *opaque) {
    g_draw_builder = draw;
    g_draw_builder_opaque = opaque;
}

static _Thread_local s2a_record_dispatch_fn g_dispatch_builder;
static _Thread_local void *g_dispatch_builder_opaque;
// Only the production exec_one path uses the independent compute unit by default.
extern int nvn_vk_compute_build(struct s2a_vk_device *, const struct s2a_record_dispatch_state *,
    const struct s2a_record_memory *, struct s2a_record_dispatch_output *,
    struct s2a_record_result *, void *) __attribute__((weak));
void s2a_record_set_dispatch_builder(s2a_record_dispatch_fn dispatch, void *opaque) {
    g_dispatch_builder = dispatch;
    g_dispatch_builder_opaque = opaque;
}

// NVN 的清除遵循当前 scissor（设备像素，左上原点）。从未设置过 scissor 时按整目标处理（返回 0）。
// 显式设成 0 宽/高的 scissor 表示“什么也不清”，这里返回 1 且矩形为空。
static int rec_scissor(const struct replay_state *S, VkRect2D *out) {
    if (!S->st.sc_seen) return 0;
    out->offset = (VkOffset2D){ S->st.sc_x, S->st.sc_y };
    out->extent = (VkExtent2D){ S->st.sc_w > 0 ? (uint32_t)S->st.sc_w : 0u,
                                S->st.sc_h > 0 ? (uint32_t)S->st.sc_h : 0u };
    return 1;
}
static uint32_t rec_mipdim(uint32_t s, uint32_t level) { s >>= level; return s ? s : 1; }

// ---- A3：附件解析失败的分类（对象身份 + 墓碑；texcache 是唯一归属，弱符号可宿主替身） ----
// 纯判据：输入是已经查到的状态，输出类型化失败（code + why）；返回 0 = 保留原有的“解析不到”。
// 不建资源、不 no-op；不把“活表未命中”推断成“从未创建”，也不把“零字段”推断成 guest 正在重建。
static int tex_attachment_failure(int identity, const char *detail, int serial_state,
                                  const struct nvn_tex_tombstone *tomb, int *code, char *why, size_t why_size) {
    if (serial_state == NVN_TEX_SERIAL_RETIRED && tomb) {
        if (why && why_size)
            snprintf(why, why_size,
                     "attachment texture serial=%llu was retired (Finalize) at generation %llu (%ux%u fmt=0x%x); "
                     "refusing to resurrect it",
                     (unsigned long long)tomb->serial, (unsigned long long)tomb->generation,
                     tomb->width, tomb->height, tomb->nvn_format);
        if (code) *code = S2A_RECORD_INVALID;
        return 1;
    }
    // 描述不自洽 + 活表没有同 serial 条目 → “已绑定但描述无效”。活表命中时保留原有失败：
    // 解析层能给出更具体的原因，不把“重建中间态”当成确定结论。
    if (identity == NVN_TEX_ID_DESC_INVALID && serial_state != NVN_TEX_SERIAL_LIVE) {
        if (why && why_size)
            snprintf(why, why_size,
                     "bound attachment texture is invalid: %s; refusing to build a resource from an "
                     "incomplete snapshot",
                     detail && detail[0] ? detail : "descriptor is not self-consistent");
        if (code) *code = S2A_RECORD_INVALID;
        return 1;
    }
    return 0;
}

// 查询包装（弱符号）：宿主合同单独编本 TU 时符号为空，此时不分类、保持原有失败路径。
// 生产直连后端链接 texcache.c，判据与墓碑查询都可用。
static int tex_attachment_unresolved(const void *key, int *code, char *why, size_t why_size) {
    const tex_t *t = (const tex_t *)key;
    if (!t) return 0;   // 未绑定：保持既有“没有目标 / 解析不到”的语义
    if (!nvn_vk_texcache_tex_identity || !nvn_vk_texcache_serial_state) return 0;
    char detail[160] = "";
    const int identity = nvn_vk_texcache_tex_identity(t, detail, sizeof detail);
    struct nvn_tex_tombstone tomb;
    memset(&tomb, 0, sizeof tomb);
    const int state = nvn_vk_texcache_serial_state(t->serial, &tomb);
    return tex_attachment_failure(identity, detail, state, &tomb, code, why, why_size);
}

static const struct s2a_replay_texture *find_texture(const struct s2a_replay_texture *textures,
                                                      size_t texture_count, const void *key) {
    // 身份优先 serial（沿 7a74818b8 语义）：记录里是产者写的 tex_t 快照，同一 serial 重新 Initialize
    // 后副本可能不带尺寸/已被清零，此时按 serial 命中活条目，不能拿副本描述去重建资源。
    // 先整体扫 serial，再退回 key 匹配：同一批表里“key 恰好等于旧指针、serial 是新的”不能让旧条目抢先。
    if (key) {
        const uint64_t serial = ((const tex_t *)key)->serial;
        if (serial) {
            for (size_t i = 0; i < texture_count; i++)
                if (textures[i].serial == serial) return &textures[i];
        }
    }
    for (size_t i = 0; i < texture_count; i++)
        if (textures[i].key == key) return &textures[i];   // 原语义：key 为 NULL 时只按 key 匹配（无身份调用）
    if (g_tex_resolve && g_tex_resolve(g_tex_resolve_opaque, key, &g_tex_scratch)) {
        g_tex_scratch.key = key;
        return &g_tex_scratch;
    }
    return NULL;
}


static int gpu_clear_texture(struct s2a_vk_device *d, const p_clear_tex *record,
                             const struct s2a_replay_texture *textures, size_t texture_count) {
    if (!record || record->mask == 0) return -1;
    if (record->r.xoffset < 0 || record->r.yoffset < 0 || record->r.zoffset ||
        record->r.width <= 0 || record->r.height <= 0 || record->r.depth <= 0) return -1;
    const struct s2a_replay_texture *texture = find_texture(textures, texture_count, record->tex);
    if (!texture) return -1;
    if (record->mask != 0xF)
        return clear_image_partial_cpu(d, texture, record);
    if (record->r.xoffset || record->r.yoffset ||
        (uint32_t)record->r.width != texture->width || (uint32_t)record->r.height != texture->height ||
        (uint32_t)record->r.depth != texture->depth)
        return clear_image_region_copy(d, texture, record);
    VkClearColorValue color = {0};
    switch (record->value_type) {
    case CLEAR_TEX_FLOAT: memcpy(color.float32, record->c.f, sizeof color.float32); break;
    case CLEAR_TEX_UINT: memcpy(color.uint32, record->c.u, sizeof color.uint32); break;
    case CLEAR_TEX_INT: memcpy(color.int32, record->c.i, sizeof color.int32); break;
    default: return -1;
    }
    return gpu_clear_image(d, texture->image, color);
}

static VkImageAspectFlags t2t_aspect(uint32_t a) {
    VkImageAspectFlags m = 0;
    if (a & S2A_T2T_ASPECT_COLOR) m |= VK_IMAGE_ASPECT_COLOR_BIT;
    if (a & S2A_T2T_ASPECT_DEPTH) m |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (a & S2A_T2T_ASPECT_STENCIL) m |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return m ? m : VK_IMAGE_ASPECT_COLOR_BIT;
}

// 把记录里的 NVNtextureView 折成计划器的视图输入（与 view_t 字段一一对应）。
static void t2t_fill_view(const view_t *v, int has, struct s2a_t2t_view *o) {
    memset(o, 0, sizeof *o);
    if (!has || !v) return;
    o->has = 1;
    o->base_level = v->set_levels ? v->base_level : 0;
    o->nlevels = v->set_levels ? v->nlevels : 0;
    o->min_layer = v->set_layers ? v->min_layer : 0;
    o->has_format = v->set_format;
    o->nvn_format = v->set_format ? v->format : 0;
    o->has_dsmode = v->set_dsmode;
    o->dsmode_stencil = v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
    o->has_target = v->set_target;
    o->target = v->set_target ? v->target : 0;
}

// 已解析纹理 + 记录内嵌快照 → 计划器的纹理事实。快照是 REC_SNAP 内嵌的 tex_t（magic 校验过）；
// 没有快照时只剩 VkImage 事实，nvn_format / target 未知（计划器按未知处理）。
static void t2t_fill_tex(const struct s2a_replay_texture *t, const tex_t *snap, struct s2a_t2t_tex *o) {
    memset(o, 0, sizeof *o);
    o->vk_format = (uint32_t)t->format;
    o->width = t->width; o->height = t->height;
    o->depth = t->depth ? t->depth : 1;
    o->layers = t->layers; o->levels = t->levels;
    o->target = -1;
    if (snap) {
        o->nvn_format = (uint32_t)snap->d.format;
        o->target = snap->d.target;
        o->is_3d = snap->d.target == NVN_TEXTURE_TARGET_3D;
        o->ms = snap->d.samples > 1 ? snap->d.samples : 0;
        o->stencil_mode = snap->stencil;
        if (!o->depth) o->depth = snap->d.depth ? snap->d.depth : 1;
        if (!o->layers) o->layers = 1;
    } else {
        o->is_3d = t->depth > 1;
    }
}

// 按 t2t_plan 发一次 vkCmdCopyImage / vkCmdBlitImage / vkCmdResolveImage：只迁移本次用到的
// 子资源，沿用旧 gpu_copy_image 的布局约定（GENERAL）与提交约定（dst 收尾转 HOST）。
static int gpu_copy_texture_plan(struct s2a_vk_device *d, const struct s2a_replay_texture *src,
                                 const struct s2a_replay_texture *dst, const struct s2a_t2t_plan *p,
                                 char *why, size_t why_size) {
    if (!p || !src->image || !dst->image) return -1;
    if (p->op == S2A_T2T_BLIT_NEAREST || p->op == S2A_T2T_BLIT_LINEAR) {
        // blit 的能力门槛按真实设备查：BLIT_SRC/DST 必须，linear 还要 FILTER_LINEAR。
        VkFormatProperties sf, df;
        vkGetPhysicalDeviceFormatProperties(d->physical, src->format, &sf);
        vkGetPhysicalDeviceFormatProperties(d->physical, dst->format, &df);
        if (!(sf.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(df.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            if (why && why_size)
                snprintf(why, why_size, "driver cannot blit format %u -> %u (features 0x%x/0x%x)",
                         (unsigned)src->format, (unsigned)dst->format,
                         (unsigned)sf.optimalTilingFeatures, (unsigned)df.optimalTilingFeatures);
            return -2;
        }
        if (p->op == S2A_T2T_BLIT_LINEAR &&
            !(sf.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            if (why && why_size)
                snprintf(why, why_size, "driver has no linear filter for blit source format %u",
                         (unsigned)src->format);
            return -2;
        }
    }
    VkCommandBuffer cmd = begin_command(d);
    if (!cmd) return -1;
    // 图像按本层约定常驻 GENERAL（texcache 上传收尾即停在 GENERAL）；GENERAL→GENERAL 保留内容，
    // 部分区域拷贝不会丢掉目标里没写的像素（UNDEFINED 允许驱动丢内容，旧整图拷贝只是凑巧没踩到）。
    VkImageMemoryBarrier barriers[2] = {{0}};
    for (int i = 0; i < 2; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = i ? dst->image : src->image;
        barriers[i].subresourceRange.aspectMask = i ? t2t_aspect(p->dst_aspect) : t2t_aspect(p->src_aspect);
        barriers[i].subresourceRange.baseMipLevel = i ? p->dst_level : p->src_level;
        barriers[i].subresourceRange.levelCount = 1;
        barriers[i].subresourceRange.baseArrayLayer = i ? p->dst_base_layer : p->src_base_layer;
        barriers[i].subresourceRange.layerCount = p->layer_count;
        barriers[i].dstAccessMask = i ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, barriers);
    if (p->op == S2A_T2T_COPY) {
        VkImageCopy r = {0};
        r.srcSubresource.aspectMask = t2t_aspect(p->src_aspect);
        r.srcSubresource.mipLevel = p->src_level;
        r.srcSubresource.baseArrayLayer = p->src_base_layer;
        r.srcSubresource.layerCount = p->layer_count;
        r.srcOffset = (VkOffset3D){p->src_offset[0], p->src_offset[1], p->src_offset[2]};
        r.dstSubresource.aspectMask = t2t_aspect(p->dst_aspect);
        r.dstSubresource.mipLevel = p->dst_level;
        r.dstSubresource.baseArrayLayer = p->dst_base_layer;
        r.dstSubresource.layerCount = p->layer_count;
        r.dstOffset = (VkOffset3D){p->dst_offset[0], p->dst_offset[1], p->dst_offset[2]};
        r.extent = (VkExtent3D){p->extent[0], p->extent[1], p->extent[2]};
        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else if (p->op == S2A_T2T_RESOLVE) {
        VkImageResolve r = {0};
        r.srcSubresource.aspectMask = t2t_aspect(p->src_aspect);
        r.srcSubresource.mipLevel = p->src_level;
        r.srcSubresource.baseArrayLayer = p->src_base_layer;
        r.srcSubresource.layerCount = p->layer_count;
        r.srcOffset = (VkOffset3D){p->src_offset[0], p->src_offset[1], p->src_offset[2]};
        r.dstSubresource.aspectMask = t2t_aspect(p->dst_aspect);
        r.dstSubresource.mipLevel = p->dst_level;
        r.dstSubresource.baseArrayLayer = p->dst_base_layer;
        r.dstSubresource.layerCount = p->layer_count;
        r.dstOffset = (VkOffset3D){p->dst_offset[0], p->dst_offset[1], p->dst_offset[2]};
        r.extent = (VkExtent3D){p->extent[0], p->extent[1], p->extent[2]};
        vkCmdResolveImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        VkImageBlit r = {0};
        r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.mipLevel = p->src_level;
        r.srcSubresource.baseArrayLayer = p->src_base_layer;
        r.srcSubresource.layerCount = p->layer_count;
        r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.dstSubresource.mipLevel = p->dst_level;
        r.dstSubresource.baseArrayLayer = p->dst_base_layer;
        r.dstSubresource.layerCount = p->layer_count;
        for (int i = 0; i < 2; i++) {
            r.srcOffsets[i] = (VkOffset3D){p->src_box[i][0], p->src_box[i][1], p->src_box[i][2]};
            r.dstOffsets[i] = (VkOffset3D){p->dst_box[i][0], p->dst_box[i][1], p->dst_box[i][2]};
        }
        vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r,
                       p->op == S2A_T2T_BLIT_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    VkImageMemoryBarrier done = barriers[1];
    done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 0, NULL, 1, &done);
    return submit(d, cmd, NULL) == REC_SUBMIT_COMPLETED ? 0 : -1;
}

static VkImageMemoryBarrier bt_image_barrier(VkImage image, const struct s2a_bt_dependency *dep) {
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = dep->src_access; b.dstAccessMask = dep->dst_access;
    b.oldLayout = (VkImageLayout)dep->old_layout; b.newLayout = (VkImageLayout)dep->new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){dep->aspect, dep->mip, 1, dep->base_layer, dep->layer_count};
    return b;
}

// OP_COPY_B2T / OP_COPY_T2B 按 buffer_texture_plan 执行：guest 只读/写计划给出的块行（行尾填充与
// 层尾不碰），staging 紧排，一条 VkBufferImageCopy 覆盖全部层/深度片；前后各一条图像依赖。
static int copy_buffer_texture(struct s2a_vk_device *d, VkImage image, const struct s2a_bt_plan *p,
                               const struct s2a_record_memory *mem) {
    if (!image || !p || !p->staging_bytes) return -1;
    const int to_guest = p->direction == S2A_BT_TO_BUFFER;
    const uint32_t nrows = p->rows * p->slices;
    const int tight = p->guest_row_pitch == p->row_bytes && p->image_stride == p->row_bytes * p->rows;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    if (create_visible(d, p->staging_bytes, &buffer, &memory, &mapped)) return -1;
    if (!to_guest) {
        if (tight) {
            if (mem->read(mem->opaque, p->guest_address, mapped, (size_t)p->staging_bytes) != 0) goto fail_staging;
        } else {
            for (uint32_t row = 0; row < nrows; row++) {
                struct s2a_bt_span span;
                if (s2a_buffer_texture_row(p, row, &span) ||
                    mem->read(mem->opaque, span.guest_address, (uint8_t *)mapped + span.staging_offset,
                              (size_t)span.bytes) != 0) goto fail_staging;
            }
        }
    }
    VkCommandBuffer cmd = begin_command(d);
    if (!cmd) goto fail_staging;
    VkImageMemoryBarrier before = bt_image_barrier(image, &p->image_before);
    vkCmdPipelineBarrier(cmd, p->image_before.src_stage, p->image_before.dst_stage, 0, 0, NULL, 0, NULL, 1, &before);
    VkBufferImageCopy region = {0};
    region.imageSubresource = (VkImageSubresourceLayers){p->aspect, p->mip, p->base_layer, p->layer_count};
    region.imageOffset = (VkOffset3D){(int32_t)p->x, (int32_t)p->y, (int32_t)p->z};
    region.imageExtent = (VkExtent3D){p->width, p->height, p->depth};
    if (to_guest)
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    else
        vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    VkImageMemoryBarrier after = bt_image_barrier(image, &p->image_after);
    vkCmdPipelineBarrier(cmd, p->image_after.src_stage, p->image_after.dst_stage, 0, 0, NULL, 0, NULL, 1, &after);
    if (to_guest) {
        VkBufferMemoryBarrier host = {0};
        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = buffer; host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
    }
    struct rec_keep keep;
    memset(&keep, 0, sizeof keep);
    keep.buf[0] = buffer; keep.mem[0] = memory; keep.nbuf = 1;
    int sr = submit(d, cmd, &keep);
    if (sr == REC_SUBMIT_INCOMPLETE) return -1;   // 完成未知：staging 已归保留账本，不能读回/释放
    int rc = sr == REC_SUBMIT_COMPLETED ? 0 : -1;
    if (rc == 0 && to_guest) {
        if (tight) {
            if (mem->write(mem->opaque, p->guest_address, mapped, (size_t)p->staging_bytes) != 0) rc = -1;
        } else {
            for (uint32_t row = 0; rc == 0 && row < nrows; row++) {
                struct s2a_bt_span span;
                if (s2a_buffer_texture_row(p, row, &span) ||
                    mem->write(mem->opaque, span.guest_address, (const uint8_t *)mapped + span.staging_offset,
                               (size_t)span.bytes) != 0) rc = -1;
            }
        }
    }
    destroy_visible(d, buffer, memory, mapped);
    return rc;
fail_staging:
    destroy_visible(d, buffer, memory, mapped);
    return -1;
}

static int gpu_copy(struct s2a_vk_device *d, uint64_t src, uint64_t dst, uint64_t bytes,
                    const struct s2a_record_memory *mem) {
    if (bytes == 0) return 0;
    if (bytes > MAX_COPY) return -1;
    VkBuffer a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    VkDeviceMemory am = VK_NULL_HANDLE, bm = VK_NULL_HANDLE;
    void *amap = NULL, *bmap = NULL;
    if (create_visible(d, bytes, &a, &am, &amap)) return -1;
    if (create_visible(d, bytes, &b, &bm, &bmap)) { destroy_visible(d, a, am, amap); return -1; }
    if (mem->read(mem->opaque, src, amap, (size_t)bytes) != 0) {
        destroy_visible(d, a, am, amap); destroy_visible(d, b, bm, bmap);
        return -1;
    }
    VkCommandBuffer cmd = begin_command(d);
    if (!cmd) { destroy_visible(d, a, am, amap); destroy_visible(d, b, bm, bmap); return -1; }
    VkBufferCopy region = {0, 0, bytes};
    vkCmdCopyBuffer(cmd, a, b, 1, &region);
    VkBufferMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = b; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    struct rec_keep keep;
    memset(&keep, 0, sizeof keep);
    keep.buf[0] = a; keep.mem[0] = am; keep.buf[1] = b; keep.mem[1] = bm; keep.nbuf = 2;
    int sr = submit(d, cmd, &keep);
    if (sr == REC_SUBMIT_INCOMPLETE) return -1;   // 完成未知：两块 staging 都在保留账本里
    int rc = sr == REC_SUBMIT_COMPLETED ? 0 : -1;
    if (rc == 0 && mem->write(mem->opaque, dst, bmap, (size_t)bytes) != 0) rc = -1;
    destroy_visible(d, a, am, amap);
    destroy_visible(d, b, bm, bmap);
    return rc;
}

int s2a_record_exec_one(struct s2a_vk_device *device, const uint8_t *packet, size_t len,
                        const struct s2a_record_memory *memory, struct s2a_record_result *result) {
    if (!result) return S2A_RECORD_ARGUMENT;
    if (!packet || len < sizeof(op_hdr)) return fail(result, S2A_RECORD_ARGUMENT, 0, "packet too small");
    op_hdr h; memcpy(&h, packet, sizeof h);
    if (h.size < sizeof h || h.size > len) return fail(result, S2A_RECORD_INVALID, h.op, "packet size outside buffer");
    // 只有打开 debug.s2a.perf_timing 才采逐笔落点（默认关，见 rec_perf_timing_on）。
    const int dbg_draw = rec_perf_timing_on() && (h.op == OP_DRAW || h.op == OP_DRAW_IDX);
    // 口径（P3 复核修正）：打包 = malloc + 复制记录 + 补 END，free 单列；replay 本体单列。
    // 8880778d 曾把终点放到 replay 之后，读数实际是“整笔 draw 执行（含 builder）”，
    // 不是打包；那会把 draw builder 的 30+ us 误记成分配成本，据此改分配器会改错地方。
    const uint64_t dbg_pkt0 = dbg_draw ? rec_now_ns() : 0;
    uint8_t *buf = malloc(h.size + sizeof h);
    if (!buf) return fail(result, S2A_RECORD_LIMIT, h.op, "out of memory");
    memcpy(buf, packet, h.size);
    op_hdr end = { OP_END, sizeof h };
    memcpy(buf + h.size, &end, sizeof end);
    const uint64_t dbg_pkt1 = dbg_draw ? rec_now_ns() : 0;
    g_keep_state = 1;   // 适配层逐包执行：绑定状态必须跨包保留
    int rc = s2a_record_replay(device, buf, buf + h.size + sizeof end, memory, NULL, 0, NULL, 0, NULL, 0, result);
    g_keep_state = 0;
    const uint64_t dbg_rep1 = dbg_draw ? rec_now_ns() : 0;
    free(buf);
    const uint64_t dbg_end = dbg_draw ? rec_now_ns() : 0;
    if (dbg_pkt0) {
        atomic_fetch_add(&g_dbg_pkt_ns, dbg_pkt1 - dbg_pkt0);
        atomic_fetch_add(&g_dbg_replay_ns, dbg_rep1 - dbg_pkt1);
        atomic_fetch_add(&g_dbg_free_ns, dbg_end - dbg_rep1);
        atomic_fetch_add(&g_dbg_pkt_n, 1);
    }
    return rc;
}

int s2a_record_replay(struct s2a_vk_device *device, const uint8_t *begin, const uint8_t *end,
                      const struct s2a_record_memory *memory,
                      const struct s2a_replay_texture *textures, size_t texture_count,
                      const struct s2a_replay_draw *draws, size_t draw_count,
                      const struct s2a_replay_dispatch *dispatches, size_t dispatch_count,
                      struct s2a_record_result *result) {
    if (!device || !device->device || !begin || !end || begin >= end || !memory ||
        !memory->read || !memory->write || !result || (texture_count && !textures) ||
        (draw_count && !draws) || (dispatch_count && !dispatches))
        return fail(result ? result : &(struct s2a_record_result){0}, S2A_RECORD_ARGUMENT, 0, "bad arguments");
    memset(result, 0, sizeof *result);
    if (!g_keep_state) s2a_record_reset_state();   // 单条流的调用方：每次从干净状态开始
    const uint8_t *pc = begin;
    const uint8_t *stack[8];
    int depth = 0;
    struct replay_state *S = &g_state;
    // 逐笔落点开关只读一次：关闭时不取钟，也不往 g_dbg_tobuilder_* 累加（默认关）。
    const int dbg_timing = rec_perf_timing_on();
    while (result->executed++ < MAX_STEPS) {
        if (pc < begin || (size_t)(end - pc) < sizeof(op_hdr))
            return fail(result, S2A_RECORD_INVALID, 0, "record header outside command memory");
        op_hdr header;
        memcpy(&header, pc, sizeof header);
        if (header.size < sizeof header || header.size % 8 || (size_t)(end - pc) < header.size)
            return fail(result, S2A_RECORD_INVALID, header.op, "record size is not aligned/in range");
        const uint8_t *payload = pc + sizeof header;
        S->seq++;
        const uint64_t dbg_draw_t0 =
            (dbg_timing && (header.op == OP_DRAW || header.op == OP_DRAW_IDX)) ? rec_now_ns() : 0;
        int dispatch_fq_try = header.op == OP_DISPATCH && dispatch_fq_on() &&
            nvn_vk_first_draw_pass_break && nvn_vk_first_draw_enqueue &&
            nvn_vk_first_draw_frame_id && nvn_vk_first_draw_detach_submit_seq &&
            s2a_poolahb_complete_seq && g_fq_pend_n < REC_FQ_PENDING_MAX &&
            (g_dispatch_builder || (g_keep_state && nvn_vk_compute_build));
        if (nvn_vk_first_draw_pass_break && op_breaks_pass(header.op)) {
            {
                extern int nvn_vk_first_draw_pass_is_open(void) __attribute__((weak));
                if (nvn_vk_first_draw_pass_is_open && nvn_vk_first_draw_pass_is_open() && header.op < 64)
                    atomic_fetch_add(&g_opbrk[header.op], 1);
            }
            // 只有真正会立即提交 GPU 工作、且后续 CPU 会读取结果的 op 才需要先等完成。
            // SetRT/Barrier/SignalEvent/ReportCounter 只改 CPU 记录或写 CPU 时间，结束 pass 保证队列顺序即可；
            // 每帧 18 条 Barrier 若各自等一次 GPU，会白掉 ~20 ms。ClearDepthStencil 现在按命令位置立即清，
            // 属于“立即 GPU 工作”，必须先结束并提交前面已录制的 draw，否则它会跑到那些 draw 之前。
            // ClearColor/ClearDepthStencil 不再计入这里：C1 之后它们录进同一个帧队列，顺序由队列保证，
            // 不需要先 flush+等完成。这笔清除若表达不了（部分通道掩码、逐位模板掩码等）会退回同步
            // 路径，退回点在下面自己补一次 sync pass_break，顺序仍然成立。
            // 实测（Adreno 730）：旧逻辑每笔 clear 前等一次，8 笔颜色 + 4 笔深度合计 8.8–28 ms/帧。
            // dispatch_fq 门开时 OP_DISPATCH 只关 pass（sync=0）：命令由帧队列统一提交，
            // 不需要先排空；门关照旧 sync=1 排空后独立提交，逐字节不变。
            int sync = header.op == OP_CLEAR_TEX || header.op == OP_CLEAR_BUF ||
                       header.op == OP_COPY_B2B || header.op == OP_COPY_T2B || header.op == OP_COPY_B2T ||
                       header.op == OP_COPY_T2T || header.op == OP_FENCE_SYNC ||
                       (header.op == OP_DISPATCH && !dispatch_fq_try);
            if (nvn_vk_first_draw_pass_break(sync) != 0) {
                // 帧队列没有拿到完成证明：命令顺序不再成立，这条 opcode 及其后的 clear/copy/fence/event
                // 都不能执行。首错留在帧队列里，这里只转述，不覆盖。
                fq_pend_unknown();   // 在途 dispatch owner 完成未知：finish(-2) 全保留在表
                const char *first = nvn_vk_first_draw_first_error ? nvn_vk_first_draw_first_error() : NULL;
                char msg[420];
                snprintf(msg, sizeof msg,
                         "pass break before opcode %u has no completion proof; this and following GPU "
                         "operations are not executed: %s", (unsigned)header.op,
                         first && first[0] ? first : "frame queue failed (no first error recorded)");
                return fail(result, S2A_RECORD_GPU, header.op, msg);
            }
        }
        switch (header.op) {
        case OP_END:
            if (depth) { pc = stack[--depth]; continue; }
            return S2A_RECORD_OK;
        case OP_JUMP: {
            uint64_t target = 0;
            if (header.size < 16) return fail(result, S2A_RECORD_INVALID, header.op, "jump payload too small");
            memcpy(&target, payload, 8);
            if (target < (uint64_t)(uintptr_t)begin || target >= (uint64_t)(uintptr_t)end)
                return fail(result, S2A_RECORD_INVALID, header.op, "jump target outside command memory");
            pc = (const uint8_t *)(uintptr_t)target;
            continue;
        }
        case OP_CALL: {
            uint64_t target = 0;
            if (header.size < 16) return fail(result, S2A_RECORD_INVALID, header.op, "call payload too small");
            memcpy(&target, payload, 8);
            if (target < (uint64_t)(uintptr_t)begin || target >= (uint64_t)(uintptr_t)end)
                return fail(result, S2A_RECORD_INVALID, header.op, "call target outside command memory");
            if (depth >= 8) return fail(result, S2A_RECORD_DEPTH, header.op, "call depth exceeds 8");
            stack[depth++] = pc + header.size;
            pc = (const uint8_t *)(uintptr_t)target;
            continue;
        }
        case OP_CLEAR_BUF: {
            p_clear_buf clear;
            if (header.size < sizeof(op_hdr) + sizeof clear)
                return fail(result, S2A_RECORD_INVALID, header.op, "clear-buffer payload too small");
            memcpy(&clear, payload, sizeof clear);
            if (gpu_fill(device, clear.addr, clear.size, clear.value, memory))
                return fail(result, S2A_RECORD_GPU, header.op, "GPU fill/readback failed");
            break;
        }
        case OP_COPY_B2B: {
            uint64_t src = 0, dst = 0, bytes = 0;
            if (header.size < 32) return fail(result, S2A_RECORD_INVALID, header.op, "copy payload too small");
            memcpy(&src, payload, 8); memcpy(&dst, payload + 8, 8); memcpy(&bytes, payload + 16, 8);
            if (gpu_copy(device, src, dst, bytes, memory))
                return fail(result, S2A_RECORD_GPU, header.op, "GPU copy/readback failed");
            break;
        }
        case OP_SET_RT: {
            p_set_rt_c compact;   // 紧凑记录（nvn_obj.h），经 set_rt_rec 还原
            if (header.size < sizeof(op_hdr) + sizeof compact)
                return fail(result, S2A_RECORD_INVALID, header.op, "set-render-target payload too small");
            memcpy(&compact, payload, sizeof compact);
            if (header.size - sizeof(op_hdr) < set_rt_payload(&compact))
                return fail(result, S2A_RECORD_INVALID, header.op, "set-render-target payload too small");
            p_set_rt rt;
            set_rt_rec(payload, header.size, &rt);
            if (rt.n < 0 || rt.n > 8)
                return fail(result, S2A_RECORD_INVALID, header.op, "render target count outside 0..8");
            // 诊断：真实游戏是否用 NVNtextureView 覆盖附件的 mip/层/格式/通道。只记首条，
            // 用于选择 F1.3 的真实缺口；不改变回放语义。
            {
                static int noted_color, noted_depth;
                for (int i = 0; i < 8 && i < rt.n; i++)
                    if (rt.has_cview[i] && (rt.cview[i].set_format || rt.cview[i].set_levels ||
                                            rt.cview[i].set_layers || rt.cview[i].set_target ||
                                            rt.cview[i].set_swizzle || rt.cview[i].set_dsmode) && !noted_color++) {
                        fprintf(stderr, "[vk-record] first real colour attachment view: slot=%d set_fmt=%d fmt=0x%x "
                                        "set_lv=%d lv=%u+%u set_ly=%d ly=%u+%u set_swz=%d swz=%u,%u,%u,%u set_tgt=%d tgt=%u\n",
                                i, rt.cview[i].set_format, rt.cview[i].format, rt.cview[i].set_levels,
                                rt.cview[i].base_level, rt.cview[i].nlevels, rt.cview[i].set_layers,
                                rt.cview[i].min_layer, rt.cview[i].nlayers, rt.cview[i].set_swizzle,
                                rt.cview[i].swz[0], rt.cview[i].swz[1], rt.cview[i].swz[2], rt.cview[i].swz[3],
                                rt.cview[i].set_target, rt.cview[i].target);
                    }
                if (rt.has_dview && (rt.dview.set_format || rt.dview.set_levels || rt.dview.set_layers ||
                                     rt.dview.set_target || rt.dview.set_swizzle || rt.dview.set_dsmode) && !noted_depth++) {
                    fprintf(stderr, "[vk-record] first real depth attachment view: set_fmt=%d fmt=0x%x set_lv=%d lv=%u+%u "
                                    "set_ly=%d ly=%u+%u set_swz=%d swz=%u,%u,%u,%u set_tgt=%d tgt=%u set_ds=%d ds=%u\n",
                            rt.dview.set_format, rt.dview.format, rt.dview.set_levels,
                            rt.dview.base_level, rt.dview.nlevels, rt.dview.set_layers,
                            rt.dview.min_layer, rt.dview.nlayers, rt.dview.set_swizzle,
                            rt.dview.swz[0], rt.dview.swz[1], rt.dview.swz[2], rt.dview.swz[3],
                            rt.dview.set_target, rt.dview.target, rt.dview.set_dsmode, rt.dview.dsmode);
                }
            }
            {
                // 诊断（不改行为）：与“上一次 SetRT 时记录的目标”比较，量冗余 SET_RT 的比例。
                // 必须在 S->rt_count = rt.n 之前算：放在后面会让“数量相同”恒为真（2 个目标换 1 个、
                // 第一个没变也会被算成 same），而这个计数正是用来判断能否去重合并 pass 的。
                int rtsame = (S->rt_count == rt.n) && (S->rt_depth == (const void *)rt.depth) &&
                             (S->rt_has_dview == (rt.has_dview ? 1 : 0));
                for (int i2 = 0; rtsame && i2 < rt.n && i2 < 8; i2++) {
                    const void *p = (const void *)rt.color[i2];
                    if (S->rt_color[i2] != p) rtsame = 0;
                    else if (S->rt_snap_ok[i2] && p && memcmp(&S->rt_snap[i2], p, sizeof(tex_t))) rtsame = 0;
                }
                if (rtsame) atomic_fetch_add(&g_rt_same, 1); else atomic_fetch_add(&g_rt_diff, 1);
                // 语义级比较：只比「附件身份 + 代际 + 尺寸/格式/目标 + 视图覆盖(mip/layer)」。
                int sn = rt.n;
                if (sn < 0) sn = 0;
                uint64_t cfp[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                for (int i2 = 0; i2 < sn && i2 < 8; i2++)
                    cfp[i2] = fd_att_fp((const tex_t *)rt.color[i2], &rt.cview[i2],
                                        rt.has_cview[i2] ? 1 : 0);
                uint64_t dfp = rt.depth ? fd_att_fp((const tex_t *)rt.depth, &rt.dview,
                                                    rt.has_dview ? 1 : 0) : 0;
                int sem_same = g_rt_sem.valid && g_rt_sem.n == sn &&
                               g_rt_sem.has_d == (rt.depth != NULL) &&
                               (!rt.depth || g_rt_sem.d == dfp);
                for (int i2 = 0; sem_same && i2 < sn && i2 < 8; i2++)
                    if (g_rt_sem.c[i2] != cfp[i2]) sem_same = 0;
                if (sem_same) atomic_fetch_add(&g_rt_sem_same, 1);
                else atomic_fetch_add(&g_rt_sem_diff, 1);
                g_rt_sem.valid = 1; g_rt_sem.n = sn; g_rt_sem.has_d = rt.depth != NULL; g_rt_sem.d = dfp;
                for (int i2 = 0; i2 < 8; i2++) g_rt_sem.c[i2] = cfp[i2];
            }
            S->rt_count = rt.n;
            S->rt_seq = S->seq;
            S->rt_depth = (const void *)rt.depth;
            S->rt_depth_snap_ok = 0;
            S->rt_has_dview = rt.has_dview ? 1 : 0;
            if (S->rt_has_dview) memcpy(&S->rt_dview, &rt.dview, sizeof S->rt_dview);
            if (S->rt_depth) {
                memcpy(&S->rt_depth_snap, S->rt_depth, sizeof(tex_t));
                S->rt_depth_snap_ok = 1;
            }
            for (int i = 0; i < 8; i++) {
                S->rt_color[i] = i < S->rt_count ? (const void *)rt.color[i] : NULL;
                S->rt_snap_ok[i] = 0;
                if (S->rt_color[i]) {
                    // 记录里的 tex_t* 只在当时有效：按 Metal 回放的 tex_snap 同样做法复制对象头。
                    memcpy(&S->rt_snap[i], S->rt_color[i], sizeof(tex_t));
                    S->rt_snap_ok[i] = 1;
                }
                S->rt_has_view[i] = i < S->rt_count ? rt.has_cview[i] : 0;
                if (i < S->rt_count && rt.has_cview[i]) memcpy(&S->rt_view[i], &rt.cview[i], sizeof S->rt_view[i]);
            }
            break;
        }
        case OP_CLEAR_COLOR: {
            if (S->rc_skip) {
                if (g_rc_skipped_clears++ < 8)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "render_cond: drop clear (block evaluated false); skipped_clears=%llu",
                        (unsigned long long)g_rc_skipped_clears);
                break;
            }
            p_clear_color clear;
            if (header.size < sizeof(op_hdr) + sizeof clear)
                return fail(result, S2A_RECORD_INVALID, header.op, "clear-color payload too small");
            memcpy(&clear, payload, sizeof clear);
            if (S->rt_count == 0) {
                // NVN 允许在没绑颜色目标时记录 ClearColor：没有目标可清，等价于空操作（不影响画面）。
                static int noted;
                if (!noted++) fprintf(stderr, "[vk-record] clear-color with no bound render target: no-op\n");
                break;
            }
            if (clear.index < 0 || clear.index >= S->rt_count || !S->rt_color[clear.index])
                return fail(result, S2A_RECORD_INVALID, header.op,
                            "clear-color index outside the bound render targets");
            const struct s2a_replay_texture *texture = find_texture(textures, texture_count, S->rt_color[clear.index]);
            if (!texture) {
                // A3：解析不到时先按对象身份/墓碑给出类型化失败；完全不确定才保留原“解析不到”。
                int tcode = 0;
                char twhy[256] = "";
                if (tex_attachment_unresolved(S->rt_color[clear.index], &tcode, twhy, sizeof twhy))
                    return fail(result, tcode, header.op, twhy);
                return fail(result, S2A_RECORD_MEMORY, header.op,
                    "clear-color render target is not resolvable (registration/lifetime gap; refusing to fake success)");
            }
            {
                // 诊断：清屏颜色前后差异是偏色类问题的第一手证据（见坑记录 world-tint）。
                static _Atomic int ncl, on = -1;
                if (on < 0) {
                    char b[PROP_VALUE_MAX] = {0};
                    __system_property_get("debug.s2a.clear_log", b);
                    on = (b[0] && atoi(b) != 0);
                }
                if (on && atomic_fetch_add(&ncl, 1) < 48)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "clear color: rgba=%.4f,%.4f,%.4f,%.4f target=%ux%u fmt=%d mask=0x%x",
                        clear.rgba[0], clear.rgba[1], clear.rgba[2], clear.rgba[3],
                        texture->width, texture->height, (int)texture->format, (unsigned)clear.mask);
            }
            // 附件格式覆盖：使用真实视图格式建 render pass/view；不合法（不同兼容类）明确拒绝，
            // 不退回 base 格式假装清过。
            VkFormat clear_format = texture->format;
            if (S->rt_has_view[clear.index] && S->rt_view[clear.index].set_format) {
                char owhy[192] = "";
                if (nvn_vk_format_override_ok(texture->format, S->rt_view[clear.index].format, &clear_format,
                                              owhy, sizeof owhy)) {
                    char msg[320];
                    snprintf(msg, sizeof msg, "clear-color attachment view format override rejected: %s", owhy);
                    return fail(result, S2A_RECORD_UNSUPPORTED, header.op, msg);
                }
            }
            uint32_t base_level = 0, level_count = 1, base_layer = 0, layer_count = 1;
            if (S->rt_has_view[clear.index]) {
                const view_t *v = &S->rt_view[clear.index];
                if (v->set_levels) { base_level = v->base_level; level_count = v->nlevels ? v->nlevels : 1; }
                if (v->set_layers) { base_layer = v->min_layer; layer_count = v->nlayers ? v->nlayers : 1; }
            }
            VkClearColorValue color = {0};
            switch (clear_format) {
            case VK_FORMAT_R8G8B8A8_SINT:
                for (int i = 0; i < 4; i++) color.int32[i] = (int32_t)lrintf(clear.rgba[i]);
                break;
            case VK_FORMAT_R8G8B8A8_UINT:
                for (int i = 0; i < 4; i++) color.uint32[i] = (uint32_t)llrintf(clear.rgba[i] < 0 ? 0 : clear.rgba[i]);
                break;
            default:
                memcpy(color.float32, clear.rgba, sizeof color.float32);
                break;
            }
            if (texture->levels && base_level + level_count > texture->levels)
                return fail(result, S2A_RECORD_INVALID, header.op, "clear-color view exceeds the image mip count");
            if (texture->layers && base_layer + layer_count > texture->layers)
                return fail(result, S2A_RECORD_INVALID, header.op, "clear-color view exceeds the image layer count");
            VkRect2D rect;
            int have_rect = rec_scissor(S, &rect);
            // 诊断（默认关）：本条 clear 记录的整条路径耗时（含 fb/view/pass 取用、命令缓冲分配、
            // 录制与 enqueue）。不改变任何分支；退回同步单独计数。
            const int clear_st = rec_clear_stats_on();
            const uint64_t clear_t0 = clear_st ? rec_now_ns() : 0;
            int clear_fallback = 0;
            for (uint32_t lv = 0; lv < level_count; lv++) {
                for (uint32_t ly = 0; ly < layer_count; ly++) {
                    s2a_clear_target t = {
                        texture->image, clear_format, VK_IMAGE_ASPECT_COLOR_BIT,
                        base_level + lv, base_layer + ly,
                        rec_mipdim(texture->width, base_level + lv), rec_mipdim(texture->height, base_level + lv),
                    };
                    char cwhy[192] = "";
                    // C1：整幅、无掩码的清除录进帧队列，不 submit 不等 fence；不能表达才走同步路径。
                    int crc = s2a_vk_clear_color_queued(device, &t, &color, (uint32_t)clear.mask & 0xfu,
                                                        have_rect ? &rect : NULL, cwhy, sizeof cwhy);
                    if (crc == S2A_VK_CLEAR_UNHANDLED) {
                        clear_fallback = 1;
                        // 退回“立即清除”前必须先把帧队列交出去并等完成，否则这笔清除会跑到前面
                        // 已录制但未提交的 draw 之前（旧实现靠 sync pass_break 保证这一点）。
                        if (nvn_vk_first_draw_pass_break && nvn_vk_first_draw_pass_break(1) != 0) {
                            const char *first = nvn_vk_first_draw_first_error ? nvn_vk_first_draw_first_error() : NULL;
                            char fmsg[420];
                            snprintf(fmsg, sizeof fmsg,
                                     "clear-color fallback has no completion proof for earlier draws: %s",
                                     first && first[0] ? first : "frame queue failed");
                            return fail(result, S2A_RECORD_GPU, header.op, fmsg);
                        }
                        crc = s2a_vk_clear_color(device, &t, &color, (uint32_t)clear.mask & 0xfu,
                                                 have_rect ? &rect : NULL, cwhy, sizeof cwhy);
                    }
                    if (crc == S2A_VK_CLEAR_QUEUED) {
                        // 已录进帧队列但还没提交/完成：这张图已经是“GPU 写过”。
                        // 不登记的话，纹理缓存在这次清除真正执行前可能用旧 CPU 镜像把它重建回去。
                        extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
                        if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(texture->image);
                    }
                    if (crc < 0) {
                        // 已提交但完成未知：GPU 可能已经写过这张图，必须登记为 GPU 生产者，
                        // 防止缓存淘汰时用旧 CPU 镜像重建；这与“清除已完成”是两件事。
                        if (crc == S2A_VK_CLEAR_UNCOMPLETED) {
                            extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
                            if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(texture->image);
                        }
                        char msg[420];
                        snprintf(msg, sizeof msg,
                                 "clear-color failed: %s (%s) | image=%p serial=%llu fmt=%d %ux%u mip=%u layer=%u "
                                 "rect=%d,%d %ux%d color=(%.2f,%.2f,%.2f,%.2f)",
                                 cwhy, s2a_vk_clear_first_error(), (void *)texture->image,
                                 (unsigned long long)texture->serial, (int)texture->format,
                                 texture->width, texture->height, base_level + lv, base_layer + ly,
                                 have_rect ? rect.offset.x : 0, have_rect ? rect.offset.y : 0,
                                 have_rect ? (int)rect.extent.width : (int)texture->width,
                                 have_rect ? (int)rect.extent.height : (int)texture->height,
                                 clear.rgba[0], clear.rgba[1], clear.rgba[2], clear.rgba[3]);
                        return fail(result, S2A_RECORD_GPU, header.op, msg);
                    }
                }
            }
            extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
            if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(texture->image);
            {
                /* C4a 首用证据：这个颜色目标的内容在本帧重新起过（iOS rt_fresh）。
                   弱绑定：后端实现挂在 nvn_vk_draw.c，无实现时啥都不发生。 */
                extern void nvn_vk_fu_rt_fresh(uint64_t serial) __attribute__((weak));
                if (nvn_vk_fu_rt_fresh && S->rt_snap_ok[clear.index])
                    nvn_vk_fu_rt_fresh(S->rt_snap[clear.index].serial);
            }
            if (clear_st) {
                atomic_fetch_add(&g_clear_ns, rec_now_ns() - clear_t0);
                atomic_fetch_add(&g_clear_n, 1);
                if (clear_fallback) atomic_fetch_add(&g_clear_fallback, 1);
                else atomic_fetch_add(&g_clear_queued, 1);
            }
            break;
        }
        case OP_BIND_PROGRAM: {
            if (header.size != sizeof(op_hdr) + sizeof(p_bind_program_id))
                return fail(result, S2A_RECORD_INVALID, header.op, "bind-program payload size");
            p_bind_program_id id; memcpy(&id, payload, sizeof id);   // 载荷可能不按 8 对齐：先拷出再按序号还原
            p_bind_program tmp; const p_bind_program *bp = bind_program_rec(&id, header.size, &tmp);
            const p_bind_program program = *bp;
            // NVN 的 BindProgram 按阶段绑定：本次调用的掩码只覆盖要改的阶段，未置位的阶段
            // 保留先前绑定。这里按阶段合并：每个被本次掩码覆盖的阶段，取本程序该阶段 sd[]
            // 的 key；空程序则解绑该阶段。不能只记本次调用的掩码，否则会丢掉更早的绑定。
            for (int st = 0; st < 5; st++) {
                if (!((program.stages >> st) & 1)) continue;
                int found = -1;
                if (program.prog) {
                    for (int j = 0; j < 6; j++)
                        if (program.stage_of[j] == st && ((program.kok >> j) & 1)) { found = j; break; }
                }
                if (found < 0) {
                    S->bound_stage_present &= (uint8_t)~(1u << st);
                    memset(S->bound_keys[st], 0, 8);
                } else {
                    memcpy(S->bound_keys[st], program.keys[found], 8);
                    S->bound_stage_present |= (uint8_t)(1u << st);
                }
            }
            // 图形程序身份同样按阶段归属（Metal state_op：stages & 0x1f 才换 S.prog）：只绑 CS 的调用
            // 不动它；空程序只解绑本次掩码里的阶段，还有图形阶段在绑时保留原身份（各阶段键已在上面合并）。
            // 旧写法每次都用本次 prog 覆盖，游戏先绑 VS/FS 再绑 CS（或空程序解绑别的阶段）后，
            // 下一笔 draw 拿到 CS 程序或空指针（真机：「draw: no device/program」后 abort）。
            if (!(program.stages & (1u << 5)) || (program.stages & 0x1f)) {
                if (program.prog || !S->bound_stage_present) S->bound_program = (const void *)program.prog;
            }
            // CS has independent ownership: a later graphics bind must not replace it.
            if (program.stages & (1u << 5)) {
                int found = -1;
                if (program.prog) {
                    for (int j = 0; j < 6; j++)
                        if (program.stage_of[j] == 5 && ((program.kok >> j) & 1)) {
                            if (found >= 0) return fail(result, S2A_RECORD_INVALID, header.op,
                                                       "ambiguous compute shader binding");
                            found = j;
                        }
                }
                S->cs_present = found >= 0;
                S->cs_program = found >= 0 ? program.prog : NULL;
                S->cs_serial = found >= 0 ? program.snap.serial : 0;
                memset(S->cs_key, 0, sizeof S->cs_key);
                if (found >= 0) memcpy(S->cs_key, program.keys[found], sizeof S->cs_key);
            }
            S->bound_stages = S->bound_stage_present;
            break;
        }
        case OP_DRAW_TEX: {
            // P0-0b：NVN DrawTexture（把一个纹理矩形画进当前颜色目标 0）。前端 hle_nvn.c 已按 p_draw_tex
            // 记录：handle = 纹理 id | 采样器 id << 20，dst = 窗口坐标 x0 y0 x1 y1，src = 纹素坐标。
            // Android Vulkan 后端没有这类内置程序设施（Metal 的 nvn_mtl.m draw_tex 自建 VS/FS 与
            // 管线；本后端没有对应实现），所以在这里准确拒绝：报 opcode、句柄（纹理/采样器 id）和两个
            // 矩形，不静默丢掉这条命令，也不假装画了。真支持要新增内置 quad 管线（只写 color(0)、保留
            // 混合与通道掩码、关深度/模板、viewport=目的矩形、整数/深度源格式拒绝），属独立特性。
            p_draw_tex t;
            if (header.size < sizeof(op_hdr) + sizeof t)
                return fail(result, S2A_RECORD_INVALID, header.op, "draw-texture payload too small");
            memcpy(&t, payload, sizeof t);
            char msg[320];
            snprintf(msg, sizeof msg,
                     "op=OP_DRAW_TEX(%u) handle=0x%llx tex=%u samp=%u dst=(%g,%g)-(%g,%g) src=(%g,%g)-(%g,%g): "
                     "DrawTexture unimplemented (needs built-in quad pipeline)",
                     (unsigned)header.op, (unsigned long long)t.handle,
                     (unsigned)(t.handle & 0xfffff), (unsigned)(t.handle >> 20),
                     (double)t.dst[0], (double)t.dst[1], (double)t.dst[2], (double)t.dst[3],
                     (double)t.src[0], (double)t.src[1], (double)t.src[2], (double)t.src[3]);
            return fail(result, S2A_RECORD_UNSUPPORTED, header.op, msg);
        }
        case OP_DRAW:
        case OP_DRAW_IDX: {
            if (S->rc_skip) {
                if (g_rc_skipped_draws++ < 8)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "render_cond: drop draw (block evaluated false); skipped_draws=%llu",
                        (unsigned long long)g_rc_skipped_draws);
                break;
            }
            // 生产入口闸（P0-0a）：adapter 注册了 draw builder（mtl_vk_adapter.c），所以生产路径永远走
            // 下面的 builder 分支，**不会**经过 replay 回退里的 ind/mdi 检查。间接参数由 GPU 写、multi-draw
            // 的计数也在 GPU 侧，两者都还没接；若在这里放行，记录里补的 count/first=0 会继续往下变成
            // “返回成功但什么都没画”。先把表达不了的绘制准确拒掉，并把记录字段原样报出（opcode/ind/mdi/tfc）。
            {
                uint64_t ind = 0, mdi_cnt = 0, tfc = 0;
                int32_t mdi_i = 0;
                if (header.op == OP_DRAW_IDX) {
                    p_draw_idx dim;
                    const p_draw_idx *d = draw_idx_rec(payload, header.size, &dim);
                    ind = d->ind; mdi_cnt = d->mdi_cnt; mdi_i = d->mdi_i;
                } else {
                    p_draw dm;
                    const p_draw *d = draw_rec(payload, header.size, &dm);
                    ind = d->ind; mdi_cnt = d->mdi_cnt; mdi_i = d->mdi_i; tfc = d->tfc;
                }
                if (ind || mdi_cnt || mdi_i || tfc) {
                    char gate_why[192];
                    snprintf(gate_why, sizeof gate_why,
                             "draw cannot be expressed: op=%s(%u) ind=0x%llx mdi_cnt=0x%llx mdi_i=%d tfc=0x%llx: "
                             "GPU-written draw params not wired yet",
                             header.op == OP_DRAW_IDX ? "OP_DRAW_IDX" : "OP_DRAW", (unsigned)header.op,
                             (unsigned long long)ind, (unsigned long long)mdi_cnt, (int)mdi_i,
                             (unsigned long long)tfc);
                    return fail(result, S2A_RECORD_UNSUPPORTED, header.op, gate_why);
                }
            }
            // NVN2 任务 / 网格绘制（nvn2_cmd.h DM_PRIM_MESH_TASKS，11-05 只接了 Metal）：本后端没有任务 /
            // 网格管线，明确报不支持，不当普通绘制画。与上面的间接参数闸并列（main 侧新增，合并保留）。
            if (header.op == OP_DRAW) {
                p_draw mt; const p_draw *md = draw_rec(payload, header.size, &mt);
                if (md->prim == DM_PRIM_MESH_TASKS)
                    return fail(result, S2A_RECORD_UNSUPPORTED, header.op, "mesh tasks draw: the Vulkan backend has no task / mesh pipeline");
            }
            if (g_draw_builder) {
                if (!S->bound_stages) return fail(result, S2A_RECORD_INVALID, header.op, "draw without a bound program");
                struct s2a_record_draw_state ds;
                memset(&ds, 0, sizeof ds);
                ds.program = S->bound_program;
                memcpy(ds.keys, S->bound_keys, sizeof ds.keys);
                ds.stages = S->bound_stages;
                ds.indexed = header.op == OP_DRAW_IDX;
                ds.rt_count = (uint32_t)S->rt_count;
                ds.cmd_seq = S->seq;
                ds.rt_seq = S->rt_seq;
                for (int i = 0; i < S->rt_count && i < 8; i++) {
                    // 用快照 tex_t 解析格式：原始 tex_t* 可能已在 draw 前被复用/释放，且 ds.rt[i].key
                    // 暴露的本来就是快照；两者必须用同一身份，否则 format=0 会让编译请求缺 fragmentOutputTypes。
                    const void *rt_key = S->rt_snap_ok[i] ? (const void *)&S->rt_snap[i] : S->rt_color[i];
                    const struct s2a_replay_texture *t = find_texture(textures, texture_count, rt_key);
                    ds.rt[i].key = (uint64_t)(uintptr_t)rt_key;
                    ds.rt[i].has_view = S->rt_has_view[i];
                    if (S->rt_has_view[i]) ds.rt[i].view = S->rt_view[i];
                    // 记录下来的对象本身是否有效（快照丢失时 magic 不对，格式/尺寸都不可信）。
                    if (S->rt_snap_ok[i]) {
                        ds.rt[i].obj_magic = S->rt_snap[i].magic;
                        ds.rt[i].obj_serial = S->rt_snap[i].serial;
                        ds.rt[i].storage = (uint64_t)(uintptr_t)S->rt_snap[i].mem;
                    }
                    if (S->rt_has_view[i] && S->rt_view[i].set_format) ds.rt[i].view_format = S->rt_view[i].format;
                    // 编译请求只缺 VkFormat 时还有权威的 NVN 格式：优先视图覆盖格式，否则对象格式。
                    if (S->rt_has_view[i] && S->rt_view[i].set_format) ds.rt[i].nvn_format = S->rt_view[i].format;
                    else if (S->rt_snap_ok[i]) ds.rt[i].nvn_format = S->rt_snap[i].d.format;
                    if (t) { ds.rt[i].format = t->format; ds.rt[i].width = t->width; ds.rt[i].height = t->height; }
                    else if (S->rt_snap_ok[i]) {
                        const tex_t *sn = &S->rt_snap[i];
                        fprintf(stderr, "[vk-record] draw rt[%d] unresolved: snap_fmt=0x%x %ux%u has_view=%d view_set_fmt=%d view_fmt=0x%x serial=%llu\n",
                                i, sn->d.format, sn->d.width, sn->d.height, S->rt_has_view[i],
                                S->rt_has_view[i] ? S->rt_view[i].set_format : 0,
                                S->rt_has_view[i] ? S->rt_view[i].format : 0,
                                (unsigned long long)sn->serial);
                    }
                }
                if (S->rt_depth_snap_ok) {
                    ds.depth_key = (uint64_t)(uintptr_t)&S->rt_depth_snap;
                    ds.depth_storage = (uint64_t)(uintptr_t)S->rt_depth_snap.mem;
                    ds.has_depth = 1;
                    ds.has_dview = S->rt_has_dview;
                    if (S->rt_has_dview) ds.dview = S->rt_dview;
                    ds.depth_magic = S->rt_depth_snap.magic;
                    ds.depth_nvn_format = S->rt_depth_snap.d.format;
                    ds.depth_serial = S->rt_depth_snap.serial;
                    // ClearDepthStencil 已在命令位置立即执行（见 OP_CLEAR_DS），所以这里没有待处理的
                    // 清除状态：附件一律 LOAD 已有内容，depth/stencil 掩码分别保持 0。
                    ds.depth_clear = 1.0f;
                    ds.stencil_clear = 0.0f;
                    ds.depth_clear_mask = 0;
                    ds.stencil_clear_mask = 0;
                    ds.depth_cleared = 0;
                }
                ds.vp_first = S->st.vp_first; ds.vp_n = S->st.vp_n;
                ds.vp_seen = S->st.vp_seen; ds.sc_seen = S->st.sc_seen;
                ds.vp_x = S->st.vp[0][0]; ds.vp_y = S->st.vp[0][1];
                ds.vp_w = S->st.vp[0][2]; ds.vp_h = S->st.vp[0][3];
                ds.vp_near = S->st.dr_n; ds.vp_far = S->st.dr_f;
                if (rec_ub_diag_on()) {   // 诊断表：与 UpdateUniformBuffer 的扫描同一个开关（默认关）
                    memcpy(ds.last_ub_addr, S->st.last_ub_addr, sizeof ds.last_ub_addr);
                    memcpy(ds.last_ub_bytes, S->st.last_ub_bytes, sizeof ds.last_ub_bytes);
                    memcpy(ds.last_ub_count, S->st.last_ub_count, sizeof ds.last_ub_count);
                }
                ds.sc_x = S->st.sc_x; ds.sc_y = S->st.sc_y; ds.sc_w = S->st.sc_w; ds.sc_h = S->st.sc_h;
                ds.tex_stage = S->st.tex_stage; ds.tex_first = S->st.tex_first; ds.tex_n = S->st.tex_n;
                ds.vb_first = S->st.vb_first; ds.vb_n = S->st.vb_n;
                ds.ubo_addr = S->st.ubo_addr; ds.ubo_size = S->st.ubo_size;
                ds.ubo_stage = S->st.ubo_stage; ds.ubo_index = S->st.ubo_index;
                memcpy(ds.ssbo, S->st.ssbo, sizeof ds.ssbo);
                memcpy(ds.ssbo_bound, S->st.ssbo_bound, sizeof ds.ssbo_bound);
                ds.tex_pool_n = S->st.tex_pool_n; ds.samp_pool_n = S->st.samp_pool_n;
                ds.tf_enabled = S->st.tf_enabled;
                // 翻译器 cbuf = NVN 用户槽 + 3（c0 驱动、c1 常量段、c2 纹理句柄）。掩码是「cbuf 号掩码」：
                // 用户槽 i → 位 (i+3)，16 个用户槽 = cbuf 3..18 全部可表达（与 compute 侧同一口径，
                // compile_request.c 已把契约扩到完整 32 位）。只有超出可表达集合（记录层只收 index 0..15，
                // 实际不可达）才置 overflow，且不再把截断掩码交给编译器。
                ds.ubo_use_overflow = 0;
                for (int st = 0; st < 6; st++) {
                    uint64_t bound = S->st.ubo_bound[st];
                    ds.ubo_use[st] = (uint32_t)(bound << 3);
                    if (bound & ~(uint64_t)0xFFFFu) ds.ubo_use_overflow = 1;
                }
                memcpy(ds.vattr_fmt, S->st.vattr_fmt, sizeof ds.vattr_fmt);
                memcpy(ds.state_obj, S->st.state_obj, sizeof ds.state_obj);
                ds.state_seen = S->st.state_seen;
                memcpy(ds.blend_obj, S->st.blend_obj, sizeof ds.blend_obj);
                ds.blend_seen = S->st.blend_seen;
                memcpy(ds.vattr, S->st.vattr, sizeof ds.vattr);
                memcpy(ds.vstream_stride, S->st.vstream_stride, sizeof ds.vstream_stride);
                memcpy(ds.vstream_divisor, S->st.vstream_divisor, sizeof ds.vstream_divisor);
                memcpy(ds.vbuf_addr, S->st.vbuf_addr, sizeof ds.vbuf_addr);
                memcpy(ds.vbuf_size, S->st.vbuf_size, sizeof ds.vbuf_size);
                memcpy(ds.ubo, S->st.ubo, sizeof ds.ubo);
                memcpy(ds.tex_handle, S->st.tex_handle, sizeof ds.tex_handle);
                memcpy(ds.tex_present, S->st.tex_present, sizeof ds.tex_present);
                memcpy(ds.samp_handle, S->st.samp_handle, sizeof ds.samp_handle);
                memcpy(ds.samp_present, S->st.samp_present, sizeof ds.samp_present);
                if (header.op == OP_DRAW_IDX) {
                    // main 的紧凑记录省掉隐含的 instances=1 / base_instance=0：必须经 draw_idx_rec 重建，
                    // 直接 memcpy 会让 instances 读成 0（Vulkan 下等于不绘制）。
                    p_draw_idx dim;
                    const p_draw_idx *di = draw_idx_rec(payload, header.size, &dim);
                    ds.prim = (uint32_t)di->prim;
                    ds.count = (uint32_t)di->count; ds.first = (uint32_t)di->base_vertex;
                    ds.instances = (uint32_t)di->instances; ds.base_vertex = (uint32_t)di->base_vertex;
                    ds.base_instance = (uint32_t)di->base_instance;
                    ds.index_address = di->ibuf; ds.index_type = (uint32_t)di->itype;
                } else {
                    // 普通 draw 可能停在 ind 之前（短记录）：draw_rec 把缺的间接字段补 0。
                    p_draw dm;
                    const p_draw *d = draw_rec(payload, header.size, &dm);
                    ds.prim = (uint32_t)d->prim;
                    ds.count = (uint32_t)d->count; ds.first = (uint32_t)d->first;
                    ds.instances = (uint32_t)d->instances;
                    ds.base_instance = (uint32_t)d->base_instance;
                }
                if (dbg_draw_t0) {
                    atomic_fetch_add(&g_dbg_tobuilder_ns, rec_now_ns() - dbg_draw_t0);
                    uint64_t dn = atomic_fetch_add(&g_dbg_tobuilder_n, 1) + 1;
                    if ((dn & 8191u) == 0) {
                        uint64_t pn = atomic_load(&g_dbg_pkt_n);
                        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                            "draw 落点(开关 debug.s2a.perf_timing=1，默认关)：打包(malloc+copy) %.2f + free %.2f us/笔（n=%llu），replay %.2f us/笔，replay 内入口→builder %.2f us/笔（n=%llu，不含 adapter 锁）",
                                            pn ? (double)atomic_load(&g_dbg_pkt_ns) / (double)pn / 1000.0 : 0.0,
                                            pn ? (double)atomic_load(&g_dbg_free_ns) / (double)pn / 1000.0 : 0.0,
                                            (unsigned long long)pn,
                                            pn ? (double)atomic_load(&g_dbg_replay_ns) / (double)pn / 1000.0 : 0.0,
                                            (double)atomic_load(&g_dbg_tobuilder_ns) / (double)dn / 1000.0,
                                            (unsigned long long)dn);
                        atomic_store(&g_dbg_tobuilder_ns, 0); atomic_store(&g_dbg_tobuilder_n, 0);
                        atomic_store(&g_dbg_pkt_ns, 0); atomic_store(&g_dbg_pkt_n, 0);
                        atomic_store(&g_dbg_free_ns, 0); atomic_store(&g_dbg_replay_ns, 0);
                        atomic_store(&g_dbg_pkt_ns, 0); atomic_store(&g_dbg_pkt_n, 0);
                    }
                }
                int rc = g_draw_builder(device, &ds, result, g_draw_builder_opaque);
                if (rc) return rc;
                break;
            }
            if (header.op == OP_DRAW_IDX)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op, "indexed draw needs the backend draw builder");
            if (header.size < sizeof(op_hdr) + offsetof(p_draw, ind))
                return fail(result, S2A_RECORD_INVALID, header.op, "draw payload too small");
            p_draw draw;
            const p_draw *drawp = draw_rec(payload, header.size, &draw);
            if (drawp->ind || drawp->mdi_cnt || drawp->mdi_i)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "indirect/multi-draw needs GPU-written parameters (not wired yet)");
            if (drawp->count < 0 || drawp->first < 0 || !S->bound_program || !S->rt_count || !S->rt_color[0])
                return fail(result, S2A_RECORD_INVALID, header.op, "draw without program/render target");
            const struct s2a_replay_draw *entry = NULL;
            for (size_t i = 0; i < draw_count; i++)
                if (draws[i].program_key == S->bound_program && draws[i].rt_key == S->rt_color[0]) { entry = &draws[i]; break; }
            if (!entry || !entry->pipeline || !entry->render_pass || !entry->framebuffer)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "draw has no registered Vulkan pipeline for this program/render target");
            // 查询页选择/回收必须在取得并开始录制命令缓冲之前完成（与直连 first_draw.c:2924 → :2958
            // 同一顺序）：prepare 的页耗尽回收可能 flush 帧队列，若命令缓冲已经 vkBeginCommandBuffer，
            // 提交后再往上录命令会崩（真机 SIGSEGV fault 0x74）。replay 的每条 draw 各建一条命令缓冲，
            // 所以这里每条都调 prepare；Q.counting==0 时它是 no-op，计数中同一提交批次共用一页，
            // 只有页满（余量不足以容纳下一段）时才在这里换页并整池 reset。
            // 与 OP_RESET_COUNTER 的先后：前一包 OP_RESET_COUNTER 的 nvn_vk_query_reset_run 先开计数
            // （还不建页，first 时返回 NEED_PASS），本笔 draw 的 prepare 才建页，随后 cb_begin 在缓冲
            // 开头录整池 reset——与直连路径“ResetCounter op → 下一 pass 的 prepare → cb_begin”同序。
            // 去掉这步的后果（宿主回归 tools/vk_record_deferred_bind_test.sh 实测）：页从未建立，
            // draw_begin 命中 p->used >= p->cap，报 "query page full inside one render pass"。
            {
                char qwhy[200] = "";
                int qrc = nvn_vk_query_prepare(device, qwhy, sizeof qwhy);
                if (qrc < 0)
                    return fail(result, qrc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                header.op, qwhy);
            }
            VkCommandBuffer cmd = begin_command(device);
            if (!cmd) return fail(result, S2A_RECORD_GPU, header.op, "command buffer allocation failed");
            // 计数中：命令缓冲开头先做整池 reset（render pass 之外），之后的 draw 段才有真实 query。
            {
                char qwhy[200] = "";
                int qrc = nvn_vk_query_cb_begin(device, cmd, qwhy, sizeof qwhy);
                if (qrc < 0) {
                    vkEndCommandBuffer(cmd);
                    rec_free_command(device, cmd);
                    return fail(result, qrc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                header.op, qwhy);
                }
            }
            VkClearValue clear = {0};
            clear.color = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}};
            VkRenderPassBeginInfo begin = {0};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = entry->render_pass;
            begin.framebuffer = entry->framebuffer;
            begin.renderArea.extent = entry->extent;
            begin.clearValueCount = 1;
            begin.pClearValues = &clear;
            vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->pipeline);
            if (entry->vertex_buffer)
                vkCmdBindVertexBuffers(cmd, 0, 1, &entry->vertex_buffer, &entry->vertex_offset);
            if (entry->descriptor)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->layout, 0, 1,
                                        &entry->descriptor, 0, NULL);
            {
                char qwhy[200] = "";
                int qrc = nvn_vk_query_draw_begin(device, cmd, qwhy, sizeof qwhy);
                if (qrc < 0) {
                    // 已经进了 render pass：先合法结束它再结束命令缓冲（VUID-00060），然后释放这条
                    // 从未提交的命令缓冲。清屏/绘制都还没执行，不会伪造计数。
                    vkCmdEndRenderPass(cmd);
                    vkEndCommandBuffer(cmd);
                    rec_free_command(device, cmd);
                    return fail(result, qrc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                header.op, qwhy);
                }
            }
            vkCmdDraw(cmd, (uint32_t)drawp->count, drawp->instances ? (uint32_t)drawp->instances : 1u,
                      (uint32_t)drawp->first, 0);
            nvn_vk_query_pass_end();
            vkCmdEndRenderPass(cmd);
            {
                int sr = submit(device, cmd, NULL);
                if (sr != REC_SUBMIT_COMPLETED)
                    return fail(result, S2A_RECORD_GPU, header.op,
                                sr == REC_SUBMIT_INCOMPLETE
                                    ? "GPU draw was submitted but its completion is unknown; resources retained"
                                    : "GPU draw submit failed (never executed)");
            }
            break;
        }
        case OP_COPY_T2B:
        case OP_COPY_B2T: {
            p_copy_b2t copy;
            if (header.size < sizeof(op_hdr) + sizeof copy)
                return fail(result, S2A_RECORD_INVALID, header.op, "buffer/texture copy payload too small");
            memcpy(&copy, payload, sizeof copy);
            const struct s2a_replay_texture *tex = find_texture(textures, texture_count, copy.tex);
            if (!tex) return fail(result, S2A_RECORD_MEMORY, header.op,
                "buffer/texture copy target is not resolvable (refusing to fake success)");
            // REC_SNAP 把 tex_t 内嵌在记录里（payload + SNAP_OFF(p_copy_b2t)）：NVN 格式 / 目标 / 层数
            // 只信快照，不解引用 guest 对象；magic 不对就明确报不支持。
            const size_t b2t_snap = (sizeof(p_copy_b2t) + 7) & ~(size_t)7;
            const tex_t *snap = NULL;
            if (header.size >= sizeof(op_hdr) + b2t_snap + sizeof(tex_t)) {
                const tex_t *t = (const tex_t *)(payload + b2t_snap);
                if (t->magic == MAGIC('T', 'E', 'X', 0)) snap = t;
            }
            if (!snap) return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                "buffer/texture copy record carries no texture snapshot");
            struct s2a_bt_input in;
            memset(&in, 0, sizeof in);
            const int layered = snap->d.target == NVN_TEXTURE_TARGET_2D_ARRAY ||
                                snap->d.target == NVN_TEXTURE_TARGET_CUBEMAP ||
                                snap->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
            const int is_3d = snap->d.target == NVN_TEXTURE_TARGET_3D;
            in.texture.nvn_format = (uint32_t)snap->d.format;
            in.texture.vk_format = (uint32_t)tex->format;
            in.texture.width = tex->width; in.texture.height = tex->height;
            in.texture.depth = is_3d ? (tex->depth ? tex->depth : snap->d.depth) : 1;
            in.texture.levels = tex->levels ? tex->levels : (snap->d.levels ? snap->d.levels : 1);
            in.texture.layers = layered ? (tex->layers ? tex->layers : snap->d.depth) : 1;
            in.texture.samples = (uint32_t)(snap->d.samples > 0 ? snap->d.samples : 0);
            in.texture.target = snap->d.target;
            if (copy.has_view) {
                in.view.has = 1;
                in.view.set_format = copy.view.set_format; in.view.format = copy.view.format;
                in.view.set_levels = copy.view.set_levels;
                in.view.base_level = copy.view.base_level; in.view.nlevels = copy.view.nlevels;
                in.view.set_layers = copy.view.set_layers;
                in.view.min_layer = copy.view.min_layer; in.view.nlayers = copy.view.nlayers;
                in.view.set_target = copy.view.set_target; in.view.target = copy.view.target;
                in.view.set_swizzle = copy.view.set_swizzle; in.view.set_dsmode = copy.view.set_dsmode;
            }
            in.region = (struct s2a_bt_region){copy.r.xoffset, copy.r.yoffset, copy.r.zoffset,
                                               copy.r.width, copy.r.height, copy.r.depth};
            in.direction = header.op == OP_COPY_T2B ? S2A_BT_TO_BUFFER : S2A_BT_TO_TEXTURE;
            in.flags = (uint32_t)copy.flags;
            in.row_stride = copy.row_stride; in.image_stride = copy.image_stride;
            in.guest_address = copy.src;
            in.max_staging_bytes = MAX_COPY; in.max_guest_span = (uint64_t)MAX_COPY * 4;
            struct s2a_bt_plan plan;
            char why[160];
            int prc = s2a_buffer_texture_plan(&in, &plan, why, sizeof why);
            if (prc) {
                static char msg[224];
                snprintf(msg, sizeof msg, "buffer/texture copy: %s", why);
                return fail(result, prc == S2A_BT_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED :
                                    prc == S2A_BT_LIMIT ? S2A_RECORD_LIMIT : S2A_RECORD_INVALID, header.op, msg);
            }
            if (copy_buffer_texture(device, tex->image, &plan, memory))
                return fail(result, S2A_RECORD_GPU, header.op, "GPU buffer/texture copy failed");
            // B2T（guest 缓冲 → 纹理）是 GPU 写图像：登记 gpu_written，否则 ReadTexels /
            // 缓存淘汰会把 guest 内存里的旧字节当成这张纹理的当前内容（T2B 只读图像，不登记）。
            if (header.op == OP_COPY_B2T) {
                extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
                if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(tex->image);
            }
            // 诊断（最多 64 条）：谁在什么时候把图拷进/拷出 guest 内存（截图/流式路径定位）。
            {
                static int n;
                if (n++ < 64)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "tex copy %s tex=%ux%u fmt=0x%x mip=%u layer=%u+%u region=%u,%u,%u %ux%ux%u row_stride=%u image_stride=%u",
                        header.op == OP_COPY_T2B ? "t2b" : "b2t", tex->width, tex->height, (unsigned)tex->format,
                        plan.mip, plan.base_layer, plan.layer_count, plan.x, plan.y, plan.z, plan.width, plan.height,
                        plan.depth, (unsigned)copy.row_stride, (unsigned)copy.image_stride);
            }
            break;
        }
        case OP_COPY_T2T: {
            p_copy_t2t copy;
            if (header.size < sizeof(op_hdr) + sizeof copy)
                return fail(result, S2A_RECORD_INVALID, header.op, "texture-copy payload too small");
            memcpy(&copy, payload, sizeof copy);
            const struct s2a_replay_texture *src = find_texture(textures, texture_count, copy.src);
            const struct s2a_replay_texture *dst = find_texture(textures, texture_count, copy.dst);
            if (!src || !dst) return fail(result, S2A_RECORD_MEMORY, header.op,
                "texture copy source/destination is not resolvable (refusing to fake success)");
            // REC_SNAP 把双方 tex_t 内嵌在记录里（payload + SNAP_OFF(p_copy_t2t) 起 2 个槽）：
            // magic 合法才信快照里的 target/format/samples/stencil。
            const tex_t *ssnap = NULL, *dsnap = NULL;
            const size_t t2t_snap = (sizeof(p_copy_t2t) + 7) & ~(size_t)7;
            if (header.size >= sizeof(op_hdr) + t2t_snap + 2 * sizeof(tex_t)) {
                const tex_t *snaps = (const tex_t *)(payload + t2t_snap);
                if (snaps[0].magic == MAGIC('T', 'E', 'X', 0)) ssnap = &snaps[0];
                if (snaps[1].magic == MAGIC('T', 'E', 'X', 0)) dsnap = &snaps[1];
            }
            struct s2a_t2t_tex st, dt;
            t2t_fill_tex(src, ssnap, &st);
            t2t_fill_tex(dst, dsnap, &dt);
            struct s2a_t2t_view svw, dvw;
            t2t_fill_view(&copy.sv, copy.has_sv, &svw);
            t2t_fill_view(&copy.dv, copy.has_dv, &dvw);
            const struct s2a_t2t_region srg = {copy.sr.xoffset, copy.sr.yoffset, copy.sr.zoffset,
                                               copy.sr.width, copy.sr.height, copy.sr.depth};
            const struct s2a_t2t_region drg = {copy.dr.xoffset, copy.dr.yoffset, copy.dr.zoffset,
                                               copy.dr.width, copy.dr.height, copy.dr.depth};
            struct s2a_t2t_plan plan;
            char pwhy[192] = "";
            if (s2a_t2t_plan(&st, &svw, &srg, &dt, &dvw, &drg, (uint32_t)copy.flags, &plan,
                             pwhy, sizeof pwhy)) {
                char msg[384];
                snprintf(msg, sizeof msg,
                    "texture copy unsupported: %s [flags=0x%x sv=%d(lv%d,ly%d,f0x%x) "
                    "dv=%d(lv%d,ly%d,f0x%x) sr=%d,%d,%d+%dx%dx%d dr=%d,%d,%d+%dx%dx%d "
                    "tex=0x%x %ux%u -> 0x%x %ux%u]",
                    pwhy, (unsigned)copy.flags, copy.has_sv, svw.base_level, svw.min_layer,
                    svw.nvn_format, copy.has_dv, dvw.base_level, dvw.min_layer, dvw.nvn_format,
                    copy.sr.xoffset, copy.sr.yoffset, copy.sr.zoffset,
                    copy.sr.width, copy.sr.height, copy.sr.depth,
                    copy.dr.xoffset, copy.dr.yoffset, copy.dr.zoffset,
                    copy.dr.width, copy.dr.height, copy.dr.depth,
                    st.nvn_format, src->width, src->height, dt.nvn_format, dst->width, dst->height);
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op, msg);
            }
            char gwhy[160] = "";
            int grc = gpu_copy_texture_plan(device, src, dst, &plan, gwhy, sizeof gwhy);
            if (grc == -2)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op, gwhy);
            if (grc)
                return fail(result, S2A_RECORD_GPU, header.op, "GPU texture copy failed");
            // Preserve #1603: after a completed T2T write, mark the destination
            // GPU-owned so readback/eviction cannot resurrect stale guest bytes.
            {
                extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
                if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(dst->image);
            }
            // 诊断（最多 64 条）：解码后的拷贝事实 + 走的路径——复审/QA 靠这行看清本局组合。
            {
                static int n;
                if (n++ < 64)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "tex copy t2t %s flags=0x%x sv=%d(lv%d,ly%d,f0x%x) dv=%d(lv%d,ly%d,f0x%x) "
                        "src=%ux%u f0x%x dst=%ux%u f0x%x sr=%d,%d,%d+%dx%dx%d dr=%d,%d,%d+%dx%dx%d",
                        plan.op == S2A_T2T_COPY ? "copy" : (plan.op == S2A_T2T_BLIT_LINEAR ? "blitL" :
                            (plan.op == S2A_T2T_RESOLVE ? "resolve" : "blitN")),
                        (unsigned)copy.flags, copy.has_sv, svw.base_level, svw.min_layer, svw.nvn_format,
                        copy.has_dv, dvw.base_level, dvw.min_layer, dvw.nvn_format,
                        src->width, src->height, (unsigned)src->format, dst->width, dst->height,
                        (unsigned)dst->format,
                        copy.sr.xoffset, copy.sr.yoffset, copy.sr.zoffset,
                        copy.sr.width, copy.sr.height, copy.sr.depth,
                        copy.dr.xoffset, copy.dr.yoffset, copy.dr.zoffset,
                        copy.dr.width, copy.dr.height, copy.dr.depth);
            }
            break;
        }
        case OP_DISPATCH: {
            p_dispatch dispatch;
            if (header.size < sizeof(op_hdr) + sizeof dispatch) {
                s2a_record_compute_note(NULL, S2A_COMPUTE_BUILD_FAILED, "dispatch payload too small");
                return fail(result, S2A_RECORD_INVALID, header.op, "dispatch payload too small");
            }
            memcpy(&dispatch, payload, sizeof dispatch);
            if (dispatch.ind) {
                s2a_record_compute_note(NULL, S2A_COMPUTE_BUILD_FAILED, "indirect dispatch needs GPU-written group counts (not wired yet)");
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op, "indirect dispatch needs GPU-written group counts (not wired yet)");
            }
            {
                // 诊断 A/B 门：跳过整笔 compute dispatch（不做 GPU 工作、不建资源）。只用于定位
                // “崩溃是否由 dispatch 执行引起”，默认关；开着时 compute 结果必错，不能当功能用。
                static _Atomic int skip = -1;
                if (skip < 0) {
                    char v[PROP_VALUE_MAX] = {0};
                    skip = __system_property_get("debug.s2a.compute_skip", v) > 0 && v[0] == '1';
                    if (skip) fprintf(stderr, "[vk-record] debug.s2a.compute_skip=1: dispatches are skipped (diagnostic only)\n");
                }
                if (skip) break;
            }
            s2a_record_dispatch_fn builder = g_dispatch_builder;
            if (!builder && g_keep_state && nvn_vk_compute_build) builder = nvn_vk_compute_build;
            if (builder) {
                // Reserve room for the worst case before creating/submitting compute resources.
                // Unlike legacy work, compute must never overflow the completion ownership ledger.
                if (g_retained_n >= REC_RETAINED_MAX) {
                    s2a_record_compute_note(NULL, S2A_COMPUTE_BUILD_FAILED, "compute retention ledger full");
                    return fail(result, S2A_RECORD_LIMIT, header.op, "compute retention ledger full");
                }
                if (dispatch.x < 0 || dispatch.y < 0 || dispatch.z < 0 || !S->cs_present) {
                    s2a_record_compute_note(NULL, S2A_COMPUTE_BUILD_FAILED, "dispatch without CS/group counts");
                    return fail(result, S2A_RECORD_INVALID, header.op, "dispatch without CS/group counts");
                }
                struct s2a_record_dispatch_state ds;
                memset(&ds, 0, sizeof ds);
                ds.program = S->cs_program;
                ds.program_serial = S->cs_serial;
                ds.cmd_seq = S->seq;
                memcpy(ds.keys[0], S->cs_key, 8);
                ds.stages = 1u << 5;
                ds.groups[0] = (uint32_t)dispatch.x;
                ds.groups[1] = (uint32_t)dispatch.y;
                ds.groups[2] = (uint32_t)dispatch.z;
                ds.tex_pool_n = S->st.tex_pool_n;
                ds.samp_pool_n = S->st.samp_pool_n;
                ds.tex_pool_seen = S->st.tex_pool != -1;
                ds.samp_pool_seen = S->st.samp_pool != -1;
                // 用户 uniform 槽 i 对应 NVN cbuf (i+3)：掩码位 = cbuf 号。NVN 有 16 个用户槽
                // （cbuf 3..18），全部都能在 u32 里表达；只有记录层收过 index>=16 的绑定（它只存
                // 0..15）才可能是不可表达，这里仍按“超出可表达集合”精确置 overflow。
                ds.ubo_use[5] = (uint32_t)(S->st.ubo_bound[5] << 3);
                ds.ubo_use_overflow = (S->st.ubo_bound[5] & ~(uint64_t)0xFFFFu) != 0;
                ds.ubo_seen = S->cs_ubo_seen;
                ds.ssbo_seen = S->cs_ssbo_seen;
                memcpy(ds.ubo, S->st.ubo[5], sizeof ds.ubo);
                memcpy(ds.ssbo, S->st.ssbo[5], sizeof ds.ssbo);
                memcpy(ds.tex_handle, S->st.tex_handle[5], sizeof ds.tex_handle);
                memcpy(ds.tex_present, S->st.tex_present[5], sizeof ds.tex_present);
                memcpy(ds.samp_handle, S->st.samp_handle[5], sizeof ds.samp_handle);
                memcpy(ds.samp_present, S->st.samp_present[5], sizeof ds.samp_present);
                memcpy(ds.image_handle, S->cs_image_handle, sizeof ds.image_handle);
                memcpy(ds.image_present, S->cs_image_present, sizeof ds.image_present);
                // 帧队列收编门：门开才允许 builder 试 AHB 池直绑；门关 builder 不做任何池查找。
                ds.fq_pool = dispatch_fq_try ? 1 : 0;
                // local memory is not a binding opcode fact; the compute source parser must prove it.
                struct s2a_record_dispatch_output out;
                memset(&out, 0, sizeof out);
                int rc = builder(device, &ds, memory, &out, result, g_dispatch_builder_opaque);
                int fallback_synced = !dispatch_fq_try;
                if (dispatch_fq_try && (rc == S2A_RECORD_DISPATCH_RETRY_SYNC || (rc == 0 && !out.fq))) {
                    if (out.finish) out.finish(device, out.owner, -1);
                    if (nvn_vk_first_draw_pass_break(1) != 0) {
                        fq_pend_unknown();
                        return fail(result, S2A_RECORD_GPU, header.op, "dispatch fallback sync failed");
                    }
                    fallback_synced = 1;
                    ds.fq_pool = 0;
                    memset(&out, 0, sizeof out);
                    rc = builder(device, &ds, memory, &out, result, g_dispatch_builder_opaque);
                }
                if (rc != 0) {
                    s2a_record_compute_note(&out.diagnostic, S2A_COMPUTE_BUILD_FAILED, result->why);
                    if (out.finish) out.finish(device, out.owner, -1);
                    return rc;
                }
                if (!out.pipeline || !out.layout || out.set_count > S2A_RECORD_DISPATCH_MAX_SETS) {
                    if (out.finish) out.finish(device, out.owner, -1);
                    s2a_record_compute_note(&out.diagnostic, S2A_COMPUTE_UNSUBMITTED, "invalid compute builder output");
                    return fail(result, S2A_RECORD_INVALID, header.op, "invalid compute builder output");
                }
                for (uint32_t i = 0; i < out.set_count; i++) {
                    if (!out.sets[i]) {
                        if (out.finish) out.finish(device, out.owner, -1);
                        s2a_record_compute_note(&out.diagnostic, S2A_COMPUTE_UNSUBMITTED, "missing compute descriptor set");
                        return fail(result, S2A_RECORD_INVALID, header.op, "missing compute descriptor set");
                    }
                }
                VkCommandBuffer cmd = begin_command(device);
                if (!cmd) {
                    if (out.finish) out.finish(device, out.owner, -1);
                    s2a_record_compute_note(&out.diagnostic, S2A_COMPUTE_UNSUBMITTED, "compute command allocation failed");
                    return fail(result, S2A_RECORD_GPU, header.op, "compute command allocation failed");
                }
                // 帧队列收编：门开 + builder 全命中池后备(out.fq) + 入队缝在 + pending 表有位
                // → 录独立命令缓冲交帧队列（不 submit 不等），owner 进 pending_fq 等完成证明。
                // 其余情况照旧同步提交，但先按旧语义排空——门开时上面的 pass_break 只关 pass
                // （sync=0），队列可能还有已录制工作，直接单独提交会排到它们前面、打乱顺序。
                int queued = 0;
                uint64_t detached_pool_seq = 0;
                if (dispatch_fq_try && out.fq) {
                    // 头屏障：此前所有 GPU 写（draw/copy/upload）对 CS 读写可见；
                    // 尾屏障：CS 写对后续 GPU 操作及完成后的宿主读取可见。
                    // 外部 AHB 缓存一致性仍需单独证明，不能仅凭屏障判断。
                    VkMemoryBarrier head = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    };
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &head, 0, NULL, 0, NULL);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, out.pipeline);
                    if (out.set_count) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        out.layout, 0, out.set_count, out.sets, 0, NULL);
                    vkCmdDispatch(cmd, ds.groups[0], ds.groups[1], ds.groups[2]);
                    VkMemoryBarrier tail = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT,
                    };
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &tail, 0, NULL, 0, NULL);
                    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
                        rec_free_command(device, cmd);
                        if (out.finish) out.finish(device, out.owner, -1);
                        s2a_record_compute_note(&out.diagnostic, S2A_COMPUTE_UNSUBMITTED,
                                                "compute queued command end failed");
                        return fail(result, S2A_RECORD_GPU, header.op,
                                    "compute queued command end failed");
                    }
                    int enqueue_rc = nvn_vk_first_draw_enqueue(device, cmd);
                    if (enqueue_rc != S2A_FQ_ENQUEUE_NOT_TAKEN) {
                        fq_pend_check_device(device);
                        struct rec_fq_pending *e = &g_fq_pend[g_fq_pend_n++];
                        e->d = device;
                        e->owner = out.owner;
                        e->finish = out.finish;
                        e->frame_id = nvn_vk_first_draw_frame_id ? nvn_vk_first_draw_frame_id() : 0;
                        e->diag = out.diagnostic;
                        atomic_fetch_add(&g_dispatch_fq_enq, 1);
                        queued = 1;
                        if (enqueue_rc != S2A_FQ_ENQUEUE_ACCEPTED) {
                            fq_pend_unknown();
                            s2a_record_compute_note(&out.diagnostic, S2A_COMPUTE_COMPLETION_UNKNOWN,
                                                    "frame queue retained dispatch after submit failure");
                            return fail(result, S2A_RECORD_GPU, header.op,
                                        "frame queue retained dispatch after submit failure");
                        }
                    } else {
                        // 帧队列没接手：命令已录完未提交，所有权仍在自己手里，下面自提走旧路径。
                        detached_pool_seq = nvn_vk_first_draw_detach_submit_seq();
                        queued = -1;
                    }
                }
                if (queued != 1) {
                    if (dispatch_fq_on()) {
                        atomic_fetch_add(&g_dispatch_fq_fallback, 1);
                        // 门开时 pass_break(0) 没排空：先按旧 sync=1 语义排空再单独提交。
                        if (!fallback_synced && nvn_vk_first_draw_pass_break && nvn_vk_first_draw_pass_break(1) != 0) {
                            fq_pend_unknown();
                            rec_free_command(device, cmd);
                            if (out.finish) out.finish(device, out.owner, -1);
                            const char *first = nvn_vk_first_draw_first_error
                                ? nvn_vk_first_draw_first_error() : NULL;
                            char msg[420];
                            snprintf(msg, sizeof msg,
                                     "pass break before queued dispatch fallback has no completion proof: %s",
                                     first && first[0] ? first
                                                       : "frame queue failed (no first error recorded)");
                            s2a_record_compute_note(&out.diagnostic, S2A_COMPUTE_UNSUBMITTED,
                                                    "compute fallback pass break failed");
                            return fail(result, S2A_RECORD_GPU, header.op, msg);
                        }
                    }
                    if (queued == 0) {
                        // 旧路径录制（或回退）：bind + dispatch + shader→host 屏障。
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, out.pipeline);
                        if (out.set_count) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            out.layout, 0, out.set_count, out.sets, 0, NULL);
                        vkCmdDispatch(cmd, ds.groups[0], ds.groups[1], ds.groups[2]);
                        // Fence completion alone does not make shader writes visible to the host.
                        // Compute owners may copy coherent mapped SSBOs back after submit completes.
                        VkMemoryBarrier host_read = {
                            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                        };
                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, NULL, 0, NULL);
                    }
                    // (queued==-1：命令已录完帧队列版屏障，直接自提提交)
                    struct rec_keep keep;
                    memset(&keep, 0, sizeof keep);
                    keep.compute_owner = out.owner;
                    keep.compute_finish = out.finish;
                    keep.pool_seq = detached_pool_seq;
                    if (queued == -1) ++g_submit_seq;
                    int sr = queued == -1 ? submit_recorded(device, cmd, &keep)
                                          : submit(device, cmd, &keep);
                    int cleanup = out.finish ? out.finish(device, out.owner, sr) : 0;
                    if (sr != REC_SUBMIT_INCOMPLETE && detached_pool_seq && s2a_poolahb_complete_seq)
                        s2a_poolahb_complete_seq(detached_pool_seq);
                    const char *compute_why = sr == REC_SUBMIT_INCOMPLETE ? "compute completion unknown; resources retained" :
                        sr == REC_SUBMIT_UNSUBMITTED ? "compute never submitted" : cleanup ? "compute completion copyback failed" : "";
                    s2a_record_compute_note(&out.diagnostic,
                        sr == REC_SUBMIT_INCOMPLETE ? S2A_COMPUTE_COMPLETION_UNKNOWN :
                        sr == REC_SUBMIT_UNSUBMITTED ? S2A_COMPUTE_UNSUBMITTED :
                        cleanup ? S2A_COMPUTE_COPYBACK_FAILED : S2A_COMPUTE_COMPLETED, compute_why);
                    if (sr != REC_SUBMIT_COMPLETED || cleanup)
                        return fail(result, S2A_RECORD_GPU, header.op, compute_why);
                }
                break;
            }
            if (dispatch.x < 0 || dispatch.y < 0 || dispatch.z < 0 || !S->bound_program)
                return fail(result, S2A_RECORD_INVALID, header.op, "dispatch without program/group counts");
            const struct s2a_replay_dispatch *entry = NULL;
            for (size_t i = 0; i < dispatch_count; i++)
                if (dispatches[i].program_key == S->bound_program) { entry = &dispatches[i]; break; }
            if (!entry || !entry->pipeline || !entry->layout)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "dispatch has no registered Vulkan compute pipeline");
            VkCommandBuffer cmd = begin_command(device);
            if (!cmd) return fail(result, S2A_RECORD_GPU, header.op, "command buffer allocation failed");
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry->pipeline);
            if (entry->descriptor)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry->layout, 0, 1,
                                        &entry->descriptor, 0, NULL);
            vkCmdDispatch(cmd, (uint32_t)dispatch.x, (uint32_t)dispatch.y, (uint32_t)dispatch.z);
            {
                int sr = submit(device, cmd, NULL);
                if (sr != REC_SUBMIT_COMPLETED)
                    return fail(result, S2A_RECORD_GPU, header.op,
                                sr == REC_SUBMIT_INCOMPLETE
                                    ? "GPU dispatch was submitted but its completion is unknown; resources retained"
                                    : "GPU dispatch submit failed (never executed)");
            }
            break;
        }
        case OP_CLEAR_TEX: {
            p_clear_tex clear;
            if (header.size < sizeof(op_hdr) + sizeof clear)
                return fail(result, S2A_RECORD_INVALID, header.op, "clear-texture payload too small");
            memcpy(&clear, payload, sizeof clear);
            if (gpu_clear_texture(device, &clear, textures, texture_count))
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "clear-texture region/mask/format not supported");
            break;
        }
        case OP_UPDATE_UB: {
            uint64_t dst = 0, bytes = 0;
            if (header.size < 24) return fail(result, S2A_RECORD_INVALID, header.op, "update payload too small");
            memcpy(&dst, payload, 8); memcpy(&bytes, payload + 8, 8);
            if (bytes > header.size - 24 || bytes > MAX_COPY)
                return fail(result, S2A_RECORD_INVALID, header.op, "uniform update length outside record");
            if (memory->write(memory->opaque, dst, payload + 16, (size_t)bytes) != 0)
                return fail(result, S2A_RECORD_MEMORY, header.op, "uniform update write failed");
            // 记录最近一次常量更新（诊断用，默认关）：draw 时看它是否覆盖实际绑定的 UBO 范围。
            for (int st = 0; rec_ub_diag_on() && st < 6; st++)
                for (int u = 0; u < 16; u++)
                    if (S->st.ubo[st][u].addr && dst >= S->st.ubo[st][u].addr && dst < S->st.ubo[st][u].addr + S->st.ubo[st][u].size) {
                        S->st.last_ub_addr[st][u] = dst;
                        S->st.last_ub_bytes[st][u] = bytes;
                        S->st.last_ub_count[st][u]++;
                    }
            break;
        }
        // ---------- 只承载状态的记录（供 draw 接线后消费；本身不产生 GPU 工作） ----------
        case OP_VIEWPORTS: {
            p_viewports v;
            if (header.size < sizeof(op_hdr) + 8) return fail(result, S2A_RECORD_INVALID, header.op, "viewports payload too small");
            int32_t count = 0;
            memcpy(&count, payload + 4, 4);
            if (count < 0 || count > 16 || header.size < sizeof(op_hdr) + 8 + (uint32_t)count * 24u)
                return fail(result, S2A_RECORD_INVALID, header.op, "viewports count outside the record");
            memset(&v, 0, sizeof v);
            memcpy(&v, payload, 8 + (size_t)count * 24u);
            v.n = count;
            S->st.vp_first = v.first; S->st.vp_n = v.n; memcpy(S->st.vp, v.v, sizeof S->st.vp);
            S->st.vp_seen = 1;
            break;
        }
        case OP_SCISSOR: {
            p_scissor v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "scissor payload too small");
            memcpy(&v, payload, sizeof v);
            S->st.sc_x = v.x; S->st.sc_y = v.y; S->st.sc_w = v.w; S->st.sc_h = v.h;
            S->st.sc_seen = 1;
            break;
        }
        case OP_DEPTH_RANGE: {
            p_depth_range v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "depth-range payload too small");
            memcpy(&v, payload, sizeof v);
            S->st.dr_n = v.n; S->st.dr_f = v.f;
            break;
        }
        case OP_BIND_STATE: {
            p_bind_state v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "bind-state payload too small");
            memcpy(&v, payload, sizeof v);
            // 编译请求要的 alpha test / alpha-to-coverage / 双源混合都在这些真实状态对象里。
            if (v.kind >= 0 && v.kind < 6) {
                memcpy(S->st.state_obj[v.kind], v.bytes, sizeof S->st.state_obj[v.kind]);
                S->st.state_seen |= (uint8_t)(1u << v.kind);
                // 混合状态按 NVNblendState.target 分槽保存：MRT 的每个颜色槽可能有不同的
                // 因子/方程，只留最后一次绑定会让其它槽拿错状态。
                if (v.kind == 0) {
                    blend_st bs;
                    memcpy(&bs, v.bytes, sizeof bs);
                    unsigned target = (unsigned)bs.target;
                    if (target < 8) {
                        memcpy(S->st.blend_obj[target], v.bytes, sizeof S->st.blend_obj[target]);
                        S->st.blend_seen |= (uint8_t)(1u << target);
                    }
                }
                g_state_binds[v.kind]++;
            }
            break;
        }
        case OP_BIND_VATTR: {
            p_bind_vattr v;
            if (header.size < sizeof(op_hdr) + 8) return fail(result, S2A_RECORD_INVALID, header.op, "bind-vertex-attrib payload too small");
            memset(&v, 0, sizeof v); memcpy(&v, payload, 8);
            if (v.n < 0 || v.n > 16 || header.size < sizeof(op_hdr) + 8 + (uint32_t)v.n * 4u)
                return fail(result, S2A_RECORD_INVALID, header.op, "vertex-attrib count outside the record");
            memcpy(&v, payload, 8 + (size_t)v.n * 4u);
            if (v.first < 0 || v.first > 32 || v.n > 32 - v.first)
                return fail(result, S2A_RECORD_INVALID, header.op, "vertex-attrib range outside 32 slots");
            for (int i = 0; i < v.n; i++) {
                uint32_t raw = v.st[i], fmt = nvn_vattr_fmt_canon(raw & 0xffu);   // RGBX → 同宽 RGB（X 不参与），同 Metal 回放
                S->st.vattr_fmt[v.first + i] = fmt;
                S->st.vattr[v.first + i].format = fmt;
                S->st.vattr[v.first + i].offset = (raw >> 8) & 0x7ffffu;
                S->st.vattr[v.first + i].stream = (raw >> 27) & 0x1fu;
            }
            break;
        }
        case OP_BIND_VSTREAM: {
            p_bind_vstream v;
            if (header.size < sizeof(op_hdr) + 8) return fail(result, S2A_RECORD_INVALID, header.op, "bind-vertex-stream payload too small");
            memset(&v, 0, sizeof v); memcpy(&v, payload, 8);
            if (v.n < 0 || v.n > 16 || header.size < sizeof(op_hdr) + 8 + (uint32_t)v.n * 8u)
                return fail(result, S2A_RECORD_INVALID, header.op, "vertex-stream count outside the record");
            memcpy(&v, payload, 8 + (size_t)v.n * 8u);
            for (int i = 0; i < v.n; i++) {
                int index = v.first + i;
                if (index >= 0 && index < 16) {
                    S->st.vstream_stride[index] = v.st[i].stride;
                    S->st.vstream_divisor[index] = v.st[i].divisor;
                }
            }
            break;
        }
        case OP_BIND_VBUF: {
            // main 的紧凑记录：单条、32 位字节数时只写 {first, size, addr}（16 B 载荷）。
            // 一律经 bind_vbuf_rec 解码，不能把前 8 字节当成 {first, n}——那样 n 会是缓冲区字节数。
            p_bind_vbuf v;
            if (header.size < sizeof(op_hdr) + 8) return fail(result, S2A_RECORD_INVALID, header.op, "bind-vertex-buffer payload too small");
            const p_bind_vbuf *b = bind_vbuf_rec(payload, header.size, &v);
            if (b->n < 0 || b->n > 16)
                return fail(result, S2A_RECORD_INVALID, header.op, "vertex-buffer count outside the record");
            if (b != &v && header.size < sizeof(op_hdr) + 8 + (uint32_t)b->n * 16u)
                return fail(result, S2A_RECORD_INVALID, header.op, "vertex-buffer count outside the record");
            S->st.vb_first = b->first; S->st.vb_n = b->n;
            for (int i = 0; i < b->n; i++) {
                int index = b->first + i;
                if (index >= 0 && index < 16) {
                    S->st.vbuf_addr[index] = (uint64_t)(uintptr_t)b->r[i].address;
                    S->st.vbuf_size[index] = b->r[i].size;
                }
            }
            break;
        }
        case OP_BIND_UBO:
        case OP_BIND_SSBO: {
            p_bind_buf v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "bind-buffer payload too small");
            memcpy(&v, payload, sizeof v);
            int stage = v.stage & ~BIND_BUF_DEFER;
            if (stage == 5 && (v.index < 0 || v.index >= 16))
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "compute buffer binding exceeds snapshot slots");
            if (header.op == OP_BIND_SSBO &&
                (stage < 0 || stage >= 6 || v.index < 0 || v.index >= 16))
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "storage buffer binding exceeds per-stage snapshot slots");
            uint64_t addr = v.addr, size = v.size;
            if (v.stage & BIND_BUF_DEFER) {
                // 延迟绑定：记录里存的是游戏 NVNbufferRange 的地址，回放时再读一次。
                NVNbufferRange range;
                if (!v.addr || memory->read(memory->opaque, v.addr, &range, sizeof range))
                    return fail(result, S2A_RECORD_MEMORY, header.op, "deferred buffer range unreadable");
                addr = (uint64_t)(uintptr_t)range.address; size = range.size;
                if (nvn_ga2ca) addr = nvn_ga2ca(addr);   // 游戏内存里的是 GPU 地址，换回 CPU（hle_nvn.c GPU 地址空间；单独编本文件的探针没有它）
            }
            if (header.op == OP_BIND_UBO) {
                if (stage == 5) S->cs_ubo_seen |= 1u << v.index;
                S->st.ubo_stage = stage; S->st.ubo_index = v.index; S->st.ubo_addr = addr; S->st.ubo_size = size;
                if (stage >= 0 && stage < 6 && v.index >= 0 && v.index < 16) {
                    S->st.ubo[stage][v.index].addr = addr;
                    S->st.ubo[stage][v.index].size = size;
                }
                if (stage >= 0 && stage < 6 && v.index >= 0 && v.index < 64) {
                    // addr=0 是 NVN 的解绑；逐槽置/清位，不能只 |= 让旧绑定一直留着。
                    if (addr) S->st.ubo_bound[stage] |= (uint64_t)1 << v.index;
                    else S->st.ubo_bound[stage] &= ~((uint64_t)1 << v.index);
                    // 溢出与否不在这里置位：解绑同样要能清掉，否则状态永远停在“曾经溢出”。
                    // draw 时按当前 ubo_bound 现算（见 OP_DRAW 分支）。
                }
            } else {
                if (stage == 5) S->cs_ssbo_seen |= 1u << v.index;
                S->st.ssbo[stage][v.index].addr = addr;
                S->st.ssbo[stage][v.index].size = addr ? size : 0;
                if (addr) S->st.ssbo_bound[stage] |= (uint64_t)1 << v.index;
                else S->st.ssbo_bound[stage] &= ~((uint64_t)1 << v.index);
            }
            break;
        }
        case OP_BIND_TEX:
        case OP_BIND_SAMP:
        case OP_BIND_IMG:
        case OP_BIND_TEX1: {
            p_bind_handles rec;
            memset(&rec, 0, sizeof rec);
            if (header.op == OP_BIND_TEX1) {
                // 单条组合纹理绑定：固定 24 字节载荷，经 bind_rec 折成 p_bind_handles。
                if (header.size < sizeof(op_hdr) + sizeof(p_bind_tex1))
                    return fail(result, S2A_RECORD_INVALID, header.op, "bind-texture record too small");
                bind_rec(header.op, payload, &rec);
            } else {
                if (header.size < sizeof(op_hdr) + 16) return fail(result, S2A_RECORD_INVALID, header.op, "bind-handles payload too small");
                memcpy(&rec, payload, header.size - sizeof(op_hdr) > sizeof rec ? sizeof rec : header.size - sizeof(op_hdr));
                if (rec.first < 0 || rec.n < 0 || rec.n > (int32_t)(sizeof rec.h / sizeof rec.h[0]) ||
                    header.size < sizeof(op_hdr) + 16 + 8 + (uint32_t)rec.n * 8u)
                    return fail(result, S2A_RECORD_INVALID, header.op, "bind-handles count outside the record");
            }
            // P0-0b'：Deferred 绑定（BindTexturesDeferred / Bind*Deferred）。记录里存的是游戏句柄数组的
            // 地址（hle_nvn.c bind_handles_deferred：defer=1、src=指针，h[n] 之后追加录制时的池基址）。
            // 回放必须在**执行到这条命令时**再读一次游戏内存，并像录制侧的 bind_handles 那样把 id 平移
            // 到池基址；否则 h[] 全是 0，纹理/采样器会静默绑到 id 0（Metal 侧对应 nvn_mtl.m 的 bh_fresh）。
            if (rec.defer) {
                if (rec.n < 0 || rec.n > (int32_t)(sizeof rec.h / sizeof rec.h[0]))
                    return fail(result, S2A_RECORD_INVALID, header.op, "deferred bind count outside the record");
                if (rec.n > 0) {
                    if (!rec.src)
                        return fail(result, S2A_RECORD_INVALID, header.op, "deferred bind has no source address");
                    // 只有 Deferred 记录在 h[n] 之后追加池基址；缺了就是一条不完整记录，不能按 0 基址放行。
                    if (header.size < sizeof(op_hdr) + offsetof(p_bind_handles, h) +
                                            (uint32_t)rec.n * 8u + sizeof(p_bind_pool_bases))
                        return fail(result, S2A_RECORD_INVALID, header.op,
                                    "deferred bind record is missing its pool bases");
                    p_bind_pool_bases bases;
                    memcpy(&bases, payload + offsetof(p_bind_handles, h) + (size_t)rec.n * 8u, sizeof bases);
                    if (memory->read(memory->opaque, rec.src, rec.h, (size_t)rec.n * 8u))
                        return fail(result, S2A_RECORD_MEMORY, header.op,
                                    "deferred texture/sampler handles unreadable from game memory");
                    for (int i = 0; i < rec.n; i++) {
                        uint64_t tid = rec.h[i] & 0xfffff, sid = rec.h[i] >> 20;
                        // 与 hle_nvn.c bind_handles / nvn_mtl.m bh_fresh 同一编码：纹理 id 加纹理池基址，
                        // 采样器 id 仅在非 0 时加采样器池基址（0 = 该句柄不带采样器）。
                        rec.h[i] = (tid + (uint64_t)bases.tbase) |
                                   ((sid ? sid + (uint64_t)bases.sbase : 0) << 20);
                    }
                }
            }
            const p_bind_handles *src = &rec;
            if (src->stage == 5 && (src->first < 0 || src->n < 0 || src->first > 32 || src->n > 32 - src->first))
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "compute handle binding exceeds snapshot slots");
            if (src->stage >= 0 && src->stage < 6) {
                for (int i = 0; i < src->n; i++) {
                    int slot = src->first + i;
                    if (slot < 0 || slot >= 32) continue;
                    if (header.op == OP_BIND_IMG && src->stage == 5) {
                        S->cs_image_handle[slot] = src->h[i];
                        S->cs_image_present[slot] = 1;
                    }
                    if (header.op == OP_BIND_SAMP) {
                        S->st.samp_handle[src->stage][slot] = src->h[i];
                        S->st.samp_present[src->stage][slot] = 1;
                    } else if (header.op == OP_BIND_TEX || header.op == OP_BIND_TEX1) {
                        // 组合句柄：低 20 位纹理 id，高 20 位采样器 id（与 hle_nvn 的编码一致）。
                        S->st.tex_handle[src->stage][slot] = src->h[i];
                        S->st.tex_present[src->stage][slot] = 1;
                        S->st.tex_stage = src->stage; S->st.tex_first = src->first; S->st.tex_n = src->n;
                    }
                }
            }
            break;
        }
        case OP_SET_TEX_POOL:
        case OP_SET_SAMP_POOL: {
            p_set_pool v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "set-pool payload too small");
            memcpy(&v, payload, sizeof v);
            if (header.op == OP_SET_TEX_POOL) S->st.tex_pool = 1; else S->st.samp_pool = 1;
            // 池容量是真实描述符数（dpool_t.n），编译请求的数组长度直接用它；越界不采信。
            uint32_t capacity = 0;
            // 记录里的 pool 是前端写的宿主 dpool_t；与 Metal 回放同一信任边界。指针可疑时不采信容量。
            if (v.pool && ((uintptr_t)v.pool & 7u) == 0 && (uintptr_t)v.pool >= 0x1000) {
                int32_t n = v.pool->n;
                if (n > 0 && n <= 65536) capacity = (uint32_t)n;
            }
            if (header.op == OP_SET_TEX_POOL) S->st.tex_pool_n = capacity;
            else S->st.samp_pool_n = capacity;
            break;
        }
        case OP_BLEND_COLOR: {
            p_blend_color v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "blend-color payload too small");
            memcpy(&v, payload, sizeof v); memcpy(S->st.blend, v.rgba, sizeof S->st.blend);
            break;
        }
        case OP_POLY_OFFSET: {
            p_poly_offset v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "poly-offset payload too small");
            memcpy(&v, payload, sizeof v);
            S->st.poly_factor = v.factor; S->st.poly_units = v.units; S->st.poly_clamp = v.clamp;
            break;
        }
        case OP_STENCIL: {
            p_stencil v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "stencil payload too small");
            memcpy(&v, payload, sizeof v);
            S->st.stencil_face = v.face; S->st.stencil_vmask = v.value_mask; S->st.stencil_mask = v.mask;
            S->st.stencil_ref = v.ref; S->st.stencil_which = v.which;
            break;
        }
        case OP_VP_SWIZZLE: {
            p_vp_swizzle v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "viewport-swizzle payload too small");
            memcpy(&v, payload, sizeof v);
            break;
        }
        case OP_PRIM_RESTART: {
            p_prim_restart v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "primitive-restart payload too small");
            memcpy(&v, payload, sizeof v);
            S->st.prim_restart_on = v.on ? 1 : 0; S->st.prim_restart_index = v.index;
            break;
        }
        case OP_DEPTH_CLAMP: {
            p_depth_clamp v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "depth-clamp payload too small");
            memcpy(&v, payload, sizeof v); S->st.depth_clamp_on = v.on;
            break;
        }
        case OP_RENDER_COND: {
            p_render_cond v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "render-cond payload too small");
            memcpy(&v, payload, sizeof v);
            S->st.render_cond_mode = v.mode; S->st.render_cond_addr = v.addr;
            {
                static _Atomic int rc_seen;
                if (atomic_fetch_add(&rc_seen, 1) < 8)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "render_cond: op mode=%d addr=0x%llx", v.mode, (unsigned long long)v.addr);
            }
            if (v.mode >= 2) {
                S->rc_skip = (v.mode == 3);            // 2 = SetRenderEnable(true)，3 = SetRenderEnable(false)
            } else {
                uint64_t x = 0, y = 0;
                int kx = rc_value(S, memory, v.addr, &x);
                int ky = rc_value(S, memory, v.addr + 16, &y);
                S->rc_skip = (kx && ky) ? (v.mode ? (x == y) : (x != y)) : 0;
                static _Atomic int rc_log;
                if (!(kx && ky) && atomic_fetch_add(&rc_log, 1) < 8)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "render_cond: report value unknown (mode=%d addr=0x%llx) -> drawing (fail-open)",
                        v.mode, (unsigned long long)v.addr);
            }
            break;
        }
        case OP_CLEAR_DS: {
            if (S->rc_skip) { g_rc_skipped_clears++; break; }
            p_clear_ds v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "clear-depth-stencil payload too small");
            memcpy(&v, payload, sizeof v);
            // 清除发生在“这条命令时的当前深度目标”上，按命令位置立即执行；不押后成下一笔 draw 的 loadOp
            // （清后没有 draw 会丢、清 A 后再绑 B 会清错目标，两者都是真实可见的错误）。
            if (!S->rt_depth_snap_ok) {
                // NVN 允许在没有绑定深度目标时记录清除：没有目标可清，等价空操作。
                static int noted;
                if (!noted++) fprintf(stderr, "[vk-record] clear-depth-stencil with no bound depth target: no-op\n");
                break;
            }
            if (S->rt_depth_snap.magic != 0x00584554u)
                return fail(result, S2A_RECORD_MEMORY, header.op, "depth target snapshot is not a live NVNtexture object");
            const int dmask = v.depth_mask != 0;
            // NVN 的 stencil 掩码是 8 位平面掩码：0x01 只清 bit0，0xFF 才是整平面。
            // 位掩码用受 VkStencilWriteMask 约束的清除绘制表达，绝不把部分掩码降成整平面清除。
            const uint32_t smask = (uint32_t)v.stencil_mask & 0xffu;
            if (!dmask && !smask) break;
            char why[192];
            VkFormat fmt = nvn_vk_format(S->rt_depth_snap.d.format);
            if (!nvn_vk_format_is_depth(fmt)) {
                snprintf(why, sizeof why, "depth target format 0x%x is not a depth format", S->rt_depth_snap.d.format);
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op, why);
            }
            if (S->rt_has_dview && S->rt_dview.set_format)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "clear-depth-stencil through a format-overriding attachment view has no mutable depth path");
            const int has_stencil = nvn_vk_format_has_stencil(fmt);
            if (smask && !has_stencil)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "clear requests stencil on a depth-only target format");
            VkImageAspectFlags aspect = (dmask || smask) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
            if (smask) aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            const struct s2a_replay_texture *texture = find_texture(textures, texture_count, &S->rt_depth_snap);
            if (!texture || !texture->image) {
                // A3：0×0/不自洽的快照 + 活表未命中 = “已绑定但描述无效”；墓碑命中 = “已退休”。
                // 两者都返回类型化失败，不再用 no-op 掩盖（no-op 不是修复：目标可能有像素没被清）。
                int tcode = 0;
                char twhy[256] = "";
                if (tex_attachment_unresolved(&S->rt_depth_snap, &tcode, twhy, sizeof twhy))
                    return fail(result, tcode, header.op, twhy);
                // 完全不确定（描述有效但活表/墓碑都查不到、或宿主合同里没有查询）：保留原失败并留现场。
                if (S->rt_depth_snap.d.width == 0 || S->rt_depth_snap.d.height == 0) {
                    static unsigned noted;
                    unsigned n = noted++;
                    if (n < 8 || (n % 64) == 8) {
                        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "clear-depth-stencil on a zero-sized depth target (serial=%llu fmt=0x%x %ux%u %u layers): unresolvable, reporting instead of no-op",
                            (unsigned long long)S->rt_depth_snap.serial, (unsigned)S->rt_depth_snap.d.format,
                            S->rt_depth_snap.d.width, S->rt_depth_snap.d.height, S->rt_depth_snap.d.depth);
                        // 同一批 RT 的颜色目标是否也“不可解析”：区分“只有深度目标失效”与“整个 SetRT 绑定是旧的”。
                        for (int ci = 0; ci < S->rt_count && ci < 8; ci++) {
                            if (!S->rt_snap_ok[ci]) { __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "  rt color[%d]: no texture", ci); continue; }
                            const struct s2a_replay_texture *ct = find_texture(textures, texture_count, &S->rt_snap[ci]);
                            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "  rt color[%d]: serial=%llu %ux%u fmt=0x%x resolve=%d image=%p", ci,
                                (unsigned long long)S->rt_snap[ci].serial, S->rt_snap[ci].d.width,
                                S->rt_snap[ci].d.height, (unsigned)S->rt_snap[ci].d.format,
                                ct && ct->image ? 1 : 0, ct ? (void *)ct->image : NULL);
                        }
                    }
                }
                return fail(result, S2A_RECORD_MEMORY, header.op,
                    "bound depth target is not resolvable (refusing to fake success)");
            }
            uint32_t base_level = 0, level_count = 1, base_layer = 0, layer_count = 1;
            if (S->rt_has_dview) {
                const view_t *dv = &S->rt_dview;
                if (dv->set_levels) { base_level = dv->base_level; level_count = dv->nlevels ? dv->nlevels : 1; }
                if (dv->set_layers) { base_layer = dv->min_layer; layer_count = dv->nlayers ? dv->nlayers : 1; }
            }
            if (texture->levels && base_level + level_count > texture->levels)
                return fail(result, S2A_RECORD_INVALID, header.op, "depth clear view exceeds the image mip count");
            if (texture->layers && base_layer + layer_count > texture->layers)
                return fail(result, S2A_RECORD_INVALID, header.op, "depth clear view exceeds the image layer count");
            VkRect2D rect;
            int have_rect = rec_scissor(S, &rect);
            for (uint32_t lv = 0; lv < level_count; lv++) {
                for (uint32_t ly = 0; ly < layer_count; ly++) {
                    s2a_clear_target t = {
                        texture->image, texture->format, aspect,
                        base_level + lv, base_layer + ly,
                        rec_mipdim(texture->width, base_level + lv), rec_mipdim(texture->height, base_level + lv),
                    };
                    char cwhy[192] = "";
                    // C1：同 ClearColor。逐位模板掩码、部分 rect 等表达不了的仍走同步路径。
                    int crc = s2a_vk_clear_ds_queued(device, &t, v.depth, (uint32_t)v.stencil & 0xffu, dmask, smask,
                                                     have_rect ? &rect : NULL, cwhy, sizeof cwhy);
                    if (crc == S2A_VK_CLEAR_UNHANDLED) {
                        if (nvn_vk_first_draw_pass_break && nvn_vk_first_draw_pass_break(1) != 0) {
                            const char *first = nvn_vk_first_draw_first_error ? nvn_vk_first_draw_first_error() : NULL;
                            char fmsg[420];
                            snprintf(fmsg, sizeof fmsg,
                                     "clear-depth-stencil fallback has no completion proof for earlier draws: %s",
                                     first && first[0] ? first : "frame queue failed");
                            return fail(result, S2A_RECORD_GPU, header.op, fmsg);
                        }
                        crc = s2a_vk_clear_depth_stencil(device, &t, v.depth, (uint32_t)v.stencil & 0xffu,
                                                         dmask, smask, have_rect ? &rect : NULL,
                                                         cwhy, sizeof cwhy);
                    }
                    if (crc == S2A_VK_CLEAR_QUEUED) {
                        // 同 ClearColor：入队即“GPU 写过”，防止旧 CPU 镜像把清除结果覆盖回去。
                        extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
                        if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(texture->image);
                    }
                    if (crc < 0) {
                        // 同 ClearColor：完成未知也要登记 GPU 生产者，避免旧 CPU 镜像覆盖真实 GPU 内容。
                        if (crc == S2A_VK_CLEAR_UNCOMPLETED) {
                            extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
                            if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(texture->image);
                        }
                        char msg[460];
                        snprintf(msg, sizeof msg,
                                 "clear-depth-stencil failed: %s (%s) | image=%p serial=%llu fmt=0x%x mip=%u layer=%u "
                                 "depth=%.4f dmask=%d stencil=%d smask=0x%02x rect=%d,%d %ux%d",
                                 cwhy, s2a_vk_clear_first_error(), (void *)texture->image,
                                 (unsigned long long)S->rt_depth_snap.serial, S->rt_depth_snap.d.format,
                                 base_level + lv, base_layer + ly, v.depth, dmask, v.stencil, smask,
                                 have_rect ? rect.offset.x : 0, have_rect ? rect.offset.y : 0,
                                 have_rect ? (int)rect.extent.width : (int)texture->width,
                                 have_rect ? (int)rect.extent.height : (int)texture->height);
                        return fail(result, S2A_RECORD_GPU, header.op, msg);
                    }
                }
            }
            extern void s2a_vk_adapter_mark_written(VkImage image) __attribute__((weak));
            if (s2a_vk_adapter_mark_written) s2a_vk_adapter_mark_written(texture->image);
            {
                /* C4a 首用证据：深度目标的内容在本帧重新起过（同 OP_CLEAR_COLOR 的 fresh 标记）。 */
                extern void nvn_vk_fu_rt_fresh(uint64_t serial) __attribute__((weak));
                if (nvn_vk_fu_rt_fresh) nvn_vk_fu_rt_fresh(S->rt_depth_snap.serial);
            }
            break;
        }
        case OP_TF_BIND: {
            p_tf_bind v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "tf-bind payload too small");
            memcpy(&v, payload, sizeof v);
            break;
        }
        case OP_TF_STATE: {
            p_tf_state v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "tf-state payload too small");
            memcpy(&v, payload, sizeof v);
            switch (v.what) {
                case 0: case 3: S->st.tf_enabled = 1; break;   // Begin / Resume
                case 1: case 2: S->st.tf_enabled = 0; break;   // End / Pause
                default: return fail(result, S2A_RECORD_INVALID, header.op, "unknown transform-feedback state");
            }
            break;
        }
        case OP_RAST_DISCARD: {
            p_rast_discard v;
            if (header.size < sizeof(op_hdr) + sizeof v) return fail(result, S2A_RECORD_INVALID, header.op, "raster-discard payload too small");
            memcpy(&v, payload, sizeof v);
            break;
        }
        case OP_BARRIER:
            // 同步后端：每条记录执行时已经等过 GPU 完成，屏障语义天然满足。
            break;
        case OP_FENCE_SYNC: {
            // 记录里是 NVNsync 地址；镜像 sync_t{magic, fenced, fence}。同步后端里“到此为止的
            // GPU 工作已完成”，所以 fence 记 0（<= 任何已提交栅栏值），fenced=1 让 SyncWait 立即成立。
            uint64_t addr = 0;
            if (header.size < 16) return fail(result, S2A_RECORD_INVALID, header.op, "fence-sync payload too small");
            memcpy(&addr, payload, 8);
            if (!addr) return fail(result, S2A_RECORD_INVALID, header.op, "fence-sync with a null sync object");
            uint32_t fenced = 1; uint64_t fence = 0;
            if (memory->write(memory->opaque, addr + 4, &fenced, 4) ||
                memory->write(memory->opaque, addr + 8, &fence, 8))
                return fail(result, S2A_RECORD_MEMORY, header.op, "fence-sync write failed");
            break;
        }
        case OP_SIGNAL_EVENT: {
            uint64_t addr = 0; uint32_t mode = 0, value = 0, cur = 0;
            if (header.size < 24) return fail(result, S2A_RECORD_INVALID, header.op, "signal-event payload too small");
            memcpy(&addr, payload, 8); memcpy(&mode, payload + 8, 4); memcpy(&value, payload + 12, 4);
            if (mode & 0x80000000u) break;   // nvnCommandBufferWaitEvent 也录成这条（方式高位 NVN_EV_WAIT，graphics/nvn nvn_mtl.h）：Vulkan 后端还没实现 GPU 等事件，照旧不等，不当信号写值
            if (!addr || memory->read(memory->opaque, addr, &cur, 4))
                return fail(result, S2A_RECORD_MEMORY, header.op, "event value unreadable");
            cur = mode ? cur + value : value;
            if (memory->write(memory->opaque, addr, &cur, 4))
                return fail(result, S2A_RECORD_MEMORY, header.op, "event value write failed");
            break;
        }
        case OP_REPORT_COUNTER: {
            uint64_t type = 0, addr = 0;
            if (header.size < 24) return fail(result, S2A_RECORD_INVALID, header.op, "report-counter payload too small");
            memcpy(&type, payload, 8); memcpy(&addr, payload + 8, 8);
            if (!addr) return fail(result, S2A_RECORD_INVALID, header.op, "report-counter with a null output");
            rc_note(S, addr, (type == 0x0 || type == 0x10) ? 1 : 2);   // 时间戳类 counter 域已知 0，其余未知

            if (type == 0x1 /* NVN_COUNTER_TYPE_SAMPLES_PASSED */) {
                // F3.2：真实遮挡查询。Report 只把“当时”的累计值快照成待写回；结果必须独立于
                // 下一次绘制，所以这里自己造完成证明（提交 + 等 fence），再读真实 query 结果写回。
                // 读不到就准确失败：不写“全部可见”上界，也不把未完成当 0。
                char qwhy[200] = "";
                int qrc = nvn_vk_query_report(device, memory, addr, qwhy, sizeof qwhy);
                if (qrc < 0)
                    return fail(result, qrc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                header.op, qwhy);
                // 默认不在这里等：报告只入队，写回挪到帧完成点（非阻塞前缀写回）。
                // 依据是 d2r-mobile 在同一台 Redmi K50 Ultra 上的实测：游戏从不在帧完成前读计数器。
                // 需要旧行为时用 debug.s2a.query_wait=1 做 A/B。
                if (nvn_vk_query_immediate_mode()) {
                    if (nvn_vk_first_draw_pass_break && nvn_vk_first_draw_pass_break(1) != 0) {
                        const char *first = nvn_vk_first_draw_first_error ? nvn_vk_first_draw_first_error() : NULL;
                        char msg[420];
                        snprintf(msg, sizeof msg,
                                 "samples-passed report: recorded GPU work has no completion proof; "
                                 "no counter value was written: %s", first && first[0] ? first : "frame queue failed");
                        return fail(result, S2A_RECORD_GPU, header.op, msg);
                    }
                    qrc = nvn_vk_query_resolve_blocking(device, qwhy, sizeof qwhy);
                    if (qrc < 0)
                        return fail(result, qrc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                    header.op, qwhy);
                } else {
                    static int noted;
                    if (noted++ < 4)
                        fprintf(stderr, "[vk-record] report-counter 延迟写回：报告入队即返回，帧完成点统一写回\n");
                }
                break;
            }
            if (type == 0x10 /* NVN_COUNTER_TYPE_TIMESTAMP_TOP */) {
                // 段顶时刻（与 Metal 端同语义）：counter 域（+0）写 0，条件渲染靠它与
                // 未写过的邻槽比对；timestamp 域（+8）写游戏时钟的 19.2 MHz 刻度。
                // 默认把 reset+写时间戳录进当前帧命令缓冲、地址进待写队列，帧完成点统一读回；
                // 只有显式 A/B（debug.s2a.query_wait=1）或没有帧队列的微例才同步提交并等完成。
                char twhy[200] = "";
                int trc;
                if (nvn_vk_first_draw_timestamp_top && !nvn_vk_query_immediate_mode()) {
                    trc = nvn_vk_ts_top_enqueue(device, memory, addr, twhy, sizeof twhy);
                } else {
                    // 同步路径：先让帧队列里已录制但未提交的绘制拿到完成证明，时间戳才会排在它们后面。
                    if (nvn_vk_first_draw_pass_break && nvn_vk_first_draw_pass_break(1) != 0) {
                        const char *first = nvn_vk_first_draw_first_error ? nvn_vk_first_draw_first_error() : NULL;
                        char msg[420];
                        snprintf(msg, sizeof msg,
                                 "timestamp-top: recorded GPU work has no completion proof; "
                                 "nothing was written: %s", first && first[0] ? first : "frame queue failed");
                        return fail(result, S2A_RECORD_GPU, header.op, msg);
                    }
                    VkCommandBuffer tcmd = begin_command(device);
                    if (!tcmd)
                        return fail(result, S2A_RECORD_GPU, header.op,
                                    "timestamp-top: command buffer allocation failed");
                    trc = nvn_vk_ts_top_record(device, tcmd, memory, addr, twhy, sizeof twhy);
                    if (trc < 0) {
                        vkEndCommandBuffer(tcmd);
                        rec_free_command(device, tcmd);
                        return fail(result, trc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                    header.op, twhy);
                    }
                    int sr = submit(device, tcmd, NULL);
                    if (sr != REC_SUBMIT_COMPLETED)
                        return fail(result, S2A_RECORD_GPU, header.op,
                                    sr == REC_SUBMIT_INCOMPLETE
                                        ? "timestamp-top: no completion proof; nothing was written"
                                        : "timestamp-top: command buffer was never submitted");
                    uint64_t ts_frame = nvn_vk_first_draw_frame_id ? nvn_vk_first_draw_frame_id() : 1;
                    trc = nvn_vk_ts_top_resolve(device, ts_frame, 1, twhy, sizeof twhy);
                }
                if (trc < 0)
                    return fail(result, trc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                header.op, twhy);
                break;
            }
            if (type != 0x0 /* NVN_COUNTER_TYPE_TIMESTAMP */)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "report-counter: GPU counter type has no Android implementation");
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (memory->write(memory->opaque, addr, &ns, 8))
                return fail(result, S2A_RECORD_MEMORY, header.op, "report-counter write failed");
            break;
        }
        case OP_RESET_COUNTER: {
            uint64_t type = 0;
            if (header.size < 16) return fail(result, S2A_RECORD_INVALID, header.op, "reset-counter payload too small");
            memcpy(&type, payload, 8);
            if (type == 0x1 /* NVN_COUNTER_TYPE_SAMPLES_PASSED */) {
                // F3.2：真实查询状态清零。返回 1 = 当前 open pass 的命令缓冲是在计数开启前开的
                // （还没有整池 reset），必须先关掉它，下一笔绘制在新命令缓冲里 reset 后再开查询。
                char qwhy[200] = "";
                int qrc = nvn_vk_query_reset_run(device, qwhy, sizeof qwhy);
                if (qrc < 0)
                    return fail(result, qrc == NVN_VK_QUERY_UNSUPPORTED ? S2A_RECORD_UNSUPPORTED : S2A_RECORD_GPU,
                                header.op, qwhy);
                if (qrc == NVN_VK_QUERY_NEED_PASS && nvn_vk_first_draw_pass_break &&
                    nvn_vk_first_draw_pass_break(0) != 0) {
                    const char *first = nvn_vk_first_draw_first_error ? nvn_vk_first_draw_first_error() : NULL;
                    char msg[420];
                    snprintf(msg, sizeof msg,
                             "samples-passed reset: closing the pass that predates counting failed: %s",
                             first && first[0] ? first : "frame queue failed");
                    return fail(result, S2A_RECORD_GPU, header.op, msg);
                }
                break;
            }
            if (type != 0x0)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op, "reset-counter: counter type has no Android implementation");
            break;
        }
        case OP_WAIT_SYNC: {
            // 命令缓冲里的 GPU 等待（nvnCommandBufferWaitSync）。主线从 34a54ae83 起记录它（缺口族 40），
            // 回放端此前只有 Metal 有实现：Android 直连碰到就按“未实现”报红、进程 abort，任何走到
            // WaitSync 的局都进不去。
            // 本后端每笔记录提交后都等到完成，本队列的栅栏在回放到这里时已经成立（OP_FENCE_SYNC 把镜像
            // 的 fenced 置 1）。若镜像仍没围栏，说明生产者不在本队列/本线程；本后端没有跨队列事件等待，
            // 有界等一小段给别的线程围栏的机会，仍不成立就明确报红——不假装等到，也不静默跳过。
            uint64_t addr = 0;
            if (header.size < 16) return fail(result, S2A_RECORD_INVALID, header.op, "wait-sync payload too small");
            memcpy(&addr, payload, 8);
            if (!addr) return fail(result, S2A_RECORD_INVALID, header.op, "wait-sync with a null sync object");
            uint32_t fenced = 0;
            if (memory->read(memory->opaque, addr + 4, &fenced, 4))
                return fail(result, S2A_RECORD_MEMORY, header.op, "wait-sync sync object unreadable");
            for (int i = 0; i < 1000 && !fenced; i++) {
                struct timespec ts = {0, 1000000};   // 1 ms
                nanosleep(&ts, NULL);
                if (memory->read(memory->opaque, addr + 4, &fenced, 4)) break;
            }
            if (!fenced)
                return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                            "wait-sync: sync object still not fenced after 1000 ms (no cross-queue event wait on this backend)");
            break;
        }
        default:
            return fail(result, S2A_RECORD_UNSUPPORTED, header.op,
                        "opcode has no Android Vulkan decoder yet (not skipped, not faked)");
        }
        pc += header.size;
    }
    return fail(result, S2A_RECORD_LIMIT, 0, "record step limit exceeded");
}
