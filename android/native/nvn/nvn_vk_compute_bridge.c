// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Default record hook. Submission/retirement remain owned by record/session;
// integration requires the independently reviewed conservative device-retirement fix
// AND worker_run checking pause failure before publishing or waiting for a job.
#include "nvn_vk_compute.h"
#include "direct/w2_observation.h"
#include "direct/diag.h"
#include "direct/compile_export.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <android/log.h>
#include <stdbool.h>
#include "debug_gate.h"
#else
/* 宿主弱缝（与主线同口径）：测试桩提供强符号，无桩时静默不丢（Android 直接链 liblog）。
 * 诊断通道只记事实，不参与结果判定。 */
extern int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((weak));
#endif
extern int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *) __attribute__((weak));
extern int s2a_vk_adapter_exec_pause(void) __attribute__((weak));
extern void s2a_vk_adapter_exec_resume(void) __attribute__((weak));
extern int s2a_vk_adapter_stop_requested(void) __attribute__((weak));
static int pause_exec(void *opaque) { (void)opaque; return s2a_vk_adapter_exec_pause(); }
static void resume_exec(void *opaque) { (void)opaque; s2a_vk_adapter_exec_resume(); }
/* 诊断按事实记录、不改判定：这里唯一要“红”的是唯一请求集发布失败——门开了却
 * 没落盘，说明请求集/身份证据链断了，必须在 logcat 可检索。 */
static void diag_note(const char *text) {
#ifndef __ANDROID__
    if (!__android_log_print) return;
#endif
    __android_log_print(4 /* ANDROID_LOG_INFO */, "S2Arm", "[s2a-direct-diag] stage=cs %s", text);
}
// F4.1：与图形 compile-request 同口径——请求体写本局私有诊断目录，文件名 =
// <requestId>-<32 字节身份全 64hex>.compile-request.json，与批编导入合同
// （direct_batch.py 的 (.+)-([0-9a-f]{64})\.compile-request\.json、前缀 = 正文
// requestId）一致；同一 requestId 的不同事实变体不互盖。文件名放不下一律不写、
// 记一行；目录不可用时只丢诊断，不拒编、不回落写别处。S2A_DIRECT_DIAG_DIR 每局
// 由外部按需设置，落盘与否还由共享调试门 diag_open 决定（#968 定案：正式包缺省不导出）。
static void diag_save(int diag_open, const char *request_id, const uint8_t identity[32],
                      const char *suffix, const void *data, size_t len) {
    const char *diag = getenv("S2A_DIRECT_DIAG_DIR");
    if (!diag_open || !diag || !diag[0]) return;
    static const char hex[] = "0123456789abcdef";
    char hid[65];
    for (unsigned i = 0; i < 32; ++i) { hid[i * 2] = hex[identity[i] >> 4]; hid[i * 2 + 1] = hex[identity[i] & 15]; }
    hid[64] = 0;
    char dname[224], dwhy[160] = "";
    int n = snprintf(dname, sizeof dname, "%s-%s.%s", request_id, hid, suffix);
    if (n < 0 || (size_t)n >= sizeof dname) {
        diag_note("diag filename truncated, request skipped");
        return;
    }
    (void)s2a_direct_diag_write(diag, dname, data, len, dwhy, sizeof dwhy);
}
static int rejected(struct s2a_record_result *result, const char *why) {
#ifdef __ANDROID__
    // result->why 只有 192 字节（对外契约不改）；完整原因（含编译器 error JSON 摘要）先进 logcat，
    // 否则“编译被哪条规则拦下”只能看到半截。
    __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "compute red: %s", why ? why : "");
#endif
    if (result) { result->code = S2A_RECORD_UNSUPPORTED; result->opcode = OP_DISPATCH; snprintf(result->why, sizeof result->why, "%s", why); }
    return S2A_RECORD_UNSUPPORTED;
}
static int nvn_vk_compute_build_impl(struct s2a_vk_device *device,
    const struct s2a_record_dispatch_state *state, const struct s2a_record_memory *memory,
    struct s2a_record_dispatch_output *out, struct s2a_record_result *result, void *opaque) {
    (void)opaque;
#ifdef __ANDROID__
    const int debug_open = s2a_debug_entries_open();
    char trace_flag[PROP_VALUE_MAX] = {0};
    int trace = debug_open && __system_property_get("debug.s2a.compute_trace", trace_flag) == 1 && trace_flag[0] == '1';
    /* Close logs before any early rejection; lifecycle activation is separate. */
    (void)s2a_record_compute_trace(trace);
#endif
    /* 请求/结果诊断共用一道门（与图形 compile-request 同合同，#968 定案）：
     * 正式包缺省关闭，调试包由 debug.s2a.compile_export 或调试门打开。 */
    const int diag_open = s2a_compile_export_open();
    if (out) memset(out, 0, sizeof *out);
    if (!device || !state || !memory || !out) return rejected(result, "compute bridge argument missing");
    if (!s2a_vk_adapter_exec_pause || !s2a_vk_adapter_exec_resume || !s2a_vk_adapter_stop_requested)
        return rejected(result, "compute execution suspension unavailable");
    if (s2a_vk_adapter_stop_requested()) return rejected(result, "compute execution stopped");
    struct s2a_direct_compile_context context = {0};
    if (!s2a_direct_provider_compile_context || s2a_direct_provider_compile_context(&context))
        return rejected(result, "compute provider context unavailable");
    out->diagnostic.session = context.session;
    out->diagnostic.generation = context.generation;
    char why[512] = {0};
    struct nvk_compute_profile_facts profile;
    if (nvk_compute_profile_parse(&context, &profile, why, sizeof why)) return rejected(result, why);
    // Owner contract: all observed valid direct CS slots, including shader-unused
    // slots, define the single candidate. Unknown/indirect referenced sources are
    // still rejected by the selected result's reflection/pipeline, never guessed.
    int unaligned = 0;
    for (unsigned slot = 0; slot < 16; ++slot)
        if ((state->ssbo_seen & (1u << slot)) && state->ssbo[slot].addr && state->ssbo[slot].size &&
            state->ssbo[slot].addr % profile.storage_alignment != 0) unaligned = 1;
    // 观察到的 CS 纹理绑定 → **消费点快照**（对象+视图+采样器+事实一次拿全，并登记保活）。
    // complete=1 只在「观察到的绑定表已被逐条描述」时成立：空表就是真的没有绑定；有一条描述不出来
    // 就整笔拒绝，绝不用空/残缺表冒充完整。编译等待期间即使发生 Finalize/同槽换对象，运行期绑定
    // 用的仍是这份快照里的同一个对象（不按槽号重新解析）。
    // 独立采样器/存储图像没有事实通道：它们存在本身不等于 shader 查询它们，真正的使用者由编译器
    // 反射与 nvn_vk_compute_pipeline_build 的 descriptor 分支明确失败（不在这里猜）。
    struct nvk_compute_tex_snapshot snapshot;
    if (nvk_compute_texture_snapshot_acquire(device, state, &snapshot, why, sizeof why))
        return rejected(result, why);
    struct nvk_compute_texture_set texture_set;
    nvk_compute_texture_facts(&snapshot, &texture_set);
    const struct nvk_compute_texture_facts textures = {
        .items = texture_set.items, .count = texture_set.count, .complete = 1 };
    struct nvk_compute_request request = {0};
    char request_id[96];
    snprintf(request_id, sizeof request_id, "cs-%llu-%llu-%llu",
        (unsigned long long)context.session, (unsigned long long)context.generation,
        (unsigned long long)state->cmd_seq);
    if (nvk_compute_request_prepare(state, &context, &textures, 1, unaligned,
            request_id, &request, why, sizeof why)) {
        nvk_compute_texture_snapshot_release(&snapshot);
        return rejected(result, why);
    }
    memcpy(out->diagnostic.identity, request.identity, 32);
    out->diagnostic.identity_valid = 1;
    // F4.1：请求体落本局私有诊断目录，并走共用唯一请求集入口发布一份——批编/导入按
    // <requestId>-<身份全64hex>.compile-request.json 同名配对；请求侧仍用本线的消费点
    // 纹理快照与 request_id 生成方式，只把主线这两段接线补回。发布失败只记红一行，
    // 不改变编译结果判定（证据链断可见，但不能因此拒掉一份合法程序）。
    diag_save(diag_open, request_id, request.identity, "compile-request.json",
              request.json, request.json_bytes);
    if (diag_open) {
        const char *dir = getenv("S2A_DIRECT_DIAG_DIR");
        if (s2a_compile_export_request(dir, context.session, context.generation, context.direct_abi, 1,
                context.translator_closure, context.profile_hash, request_id, request.identity,
                request.json, request.json_bytes) != 0)
            diag_note("【红】唯一请求集发布失败");
    }
    /* 程序级缓存（debug.s2a.compute_prog_cache，默认关）：identity 此刻已产出，命中即
     * 短路 lookup/parse/对象创建——同一张有界表的 by_identity 条目（见 nvn_vk_compute.c）。
     * 每笔状态（c0/c1/绑定快照/guest）不进键，命中路径仍按本次 dispatch 活状态逐笔填。 */
    struct cpso *prog_hit = nvn_vk_compute_prog_lookup(device, request.identity);
    if (prog_hit) {
        s2a_w2_shader_lookup(&context, request.identity, S2A_W2_MEMO, 1);
        out->diagnostic.source = S2A_COMPUTE_CACHED;
        struct s2a_compute_diagnostic diagnostic = out->diagnostic;
        int rc = 0;
        if (s2a_vk_adapter_stop_requested()) {
            nvn_vk_compute_prog_release(prog_hit);
            rc = -1; snprintf(why, sizeof why, "compute execution stopped");
        } else rc = nvn_vk_compute_pipeline_build_cached(device, state, memory, &snapshot,
                    &request.source, prog_hit, out, why, sizeof why);
        out->diagnostic = diagnostic;
        nvk_compute_request_release(&request);
        // 成功时 cached build 已接管快照（入参清零），这里是 no-op；失败/未走到时解除全部保活。
        nvk_compute_texture_snapshot_release(&snapshot);
        if (rc == S2A_RECORD_DISPATCH_RETRY_SYNC) return rc;
        if (rc) return rejected(result, why[0] ? why : "compute build failed");
        return 0;
    }
    struct nvk_compute_lookup_options options = {0};
    const char *revision = getenv("S2A_SHADER_REVISION_ROOT");
    const char *cache = getenv("S2A_DIRECT_CACHE_DIR");
#ifdef __ANDROID__
    char root[PROP_VALUE_MAX] = {0}, flag[PROP_VALUE_MAX] = {0};
    if (debug_open) {
        if (__system_property_get("debug.s2a.offline_root", root) > 0 && root[0] == '/') revision = root;
        if (__system_property_get("debug.s2a.no_offline", flag) == 1 && flag[0] == '1') revision = NULL;
        memset(flag, 0, sizeof flag);
        if (__system_property_get("debug.s2a.no_disk_cache", flag) == 1 && flag[0] == '1') cache = NULL;
        memset(flag, 0, sizeof flag);
        options.no_live_compile = __system_property_get("debug.s2a.no_live_compile", flag) == 1 && flag[0] == '1';
    }
#endif
    options.revision_root = revision && revision[0] == '/' ? revision : NULL;
    options.private_cache_root = cache && cache[0] == '/' ? cache : NULL;
    // 结果封套与请求侧同一道门、同一个目录：两侧文件名只有后缀不同（请求 .compile-request.json，
    // 结果 .compile-result.json），现场按 requestId + 身份后缀直接配对。
    options.diag_open = diag_open;
    options.pause = pause_exec; options.resume = resume_exec;
    struct nvk_compute_lookup_result program = {0};
    // Exactly one immutable REQUEST/identity and one lookup; no alternate bool.
    int rc = nvk_compute_lookup(&request, &context, &options, &program, why, sizeof why);
    s2a_w2_shader_lookup(&context, request.identity,
        program.origin == NVK_COMPUTE_OFFLINE ? S2A_W2_OFFLINE :
        program.origin == NVK_COMPUTE_PRIVATE ? S2A_W2_PRIVATE :
        program.origin == NVK_COMPUTE_COMPILED ? S2A_W2_LIVE : S2A_W2_UNKNOWN, rc == 0);
    if (!rc) out->diagnostic.source = program.origin == NVK_COMPUTE_OFFLINE ? S2A_COMPUTE_OFFLINE :
        program.origin == NVK_COMPUTE_PRIVATE ? S2A_COMPUTE_PRIVATE :
        program.origin == NVK_COMPUTE_COMPILED ? S2A_COMPUTE_LIVE : S2A_COMPUTE_UNKNOWN;
    struct s2a_compute_diagnostic diagnostic = out->diagnostic;
    if (s2a_vk_adapter_stop_requested()) { rc = -1; snprintf(why, sizeof why, "compute execution stopped"); }
    if (!rc) rc = nvn_vk_compute_pipeline_build_prog(device, state, memory, &snapshot, &request.source,
                                                &program, request.identity, out, why, sizeof why);
    out->diagnostic = diagnostic; /* pipeline may initialize its output; preserve facts on failure too. */
    free(program.bytes);
    nvk_compute_request_release(&request);
    // 成功时 pipeline 已接管快照（入参清零），这里是 no-op；失败/未走到 pipeline 时解除全部保活。
    nvk_compute_texture_snapshot_release(&snapshot);
    if (rc == S2A_RECORD_DISPATCH_RETRY_SYNC) return rc;
    if (rc) return rejected(result, why[0] ? why : "compute build failed");
    return 0;
}

// Observation wraps the unchanged build result, never dispatch/submission proof.
int nvn_vk_compute_build(struct s2a_vk_device *device,
    const struct s2a_record_dispatch_state *state, const struct s2a_record_memory *memory,
    struct s2a_record_dispatch_output *out, struct s2a_record_result *result, void *opaque) {
    struct s2a_w2_shader observation = {0};
    s2a_w2_shader_begin(&observation, S2A_W2_COMPUTE);
    int rc = nvn_vk_compute_build_impl(device, state, memory, out, result, opaque);
    s2a_w2_shader_end(&observation, rc == S2A_RECORD_DISPATCH_RETRY_SYNC ? -1 : rc == 0);
    return rc;
}
