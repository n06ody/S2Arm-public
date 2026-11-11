// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_vk_compute_lookup.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "nvn_vk_program_result.h"
#include "direct/offline_library.h"
#include "direct/program_cache.h"
#include "direct/compile_worker.h"
#include "direct/libpack_vk.h"
#include "direct/json.h"
#include "direct/diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The direct core is linked in engine=OFF targets too. Provider availability
 * is optional at link time, never optional for accepting a compute result. */
extern int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *) __attribute__((weak));
#ifndef __ANDROID__
/* 宿主弱缝（与 nvn_vk_compute_bridge.c 同口径）：测试桩提供强符号，无桩静默；
 * Android 侧直接链 liblog。结果封套文件名截断只丢该次落盘并记一行，不影响结果判定。 */
extern int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((weak));
#endif

struct expected {
    uint64_t session, generation;
    uint32_t abi;
    int unaligned;
    char closure[257], profile[72], request_id[160];
};
static int error(char *why, size_t cap, const char *text) {
    if (why && cap) snprintf(why, cap, "%s", text);
    return -1;
}
static int same_context(const struct expected *e) {
    struct s2a_direct_compile_context live = {0};
    return s2a_direct_provider_compile_context && !s2a_direct_provider_compile_context(&live) && live.session == e->session &&
        live.generation == e->generation && live.direct_abi == e->abi &&
        live.translator_closure && !strcmp(live.translator_closure, e->closure) &&
        live.profile_hash && !strcmp(live.profile_hash, e->profile);
}
static int string_is(const s2a_json_object *o, const char *key, const char *value) {
    const s2a_json_field *f = s2a_json_get(o, key);
    return f && f->kind == S2A_JSON_STRING && !strcmp(f->text, value);
}
static int number_is(const s2a_json_object *o, const char *key, uint64_t value) {
    const s2a_json_field *f = s2a_json_get(o, key);
    return f && f->kind == S2A_JSON_NUMBER && f->is_unsigned && f->unsigned_value == value;
}
/* Container SHA proves bytes, not their producer profile or semantic shape.
 * Old session IDs in persistent results are allowed; freshly compiled results
 * must instead echo this transport generation plus the REQUEST's own
 * requestId. The producer envelope nests the echo in "transport"
 * (DirectShaderCompiler.WriteEnvelope: {"transport":{session,generation,
 * requestId}}); a missing object or any field drift rejects here, before
 * content is trusted. requestId 是“这份结果是不是回答这份 REQUEST”的唯一
 * 关联字段——只比 session/generation 会在同会话多笔请求时把 A 结果当 B 用。 */
static int accept(const uint8_t *bytes, size_t length, const struct expected *e, int fresh,
                  char *why, size_t cap) {
    s2a_json_object *j = malloc(sizeof *j);
    if (!j) return error(why, cap, "compute identity allocation failed");
    int rc = -1;
    if (s2a_json_parse_object(bytes, length, j, why, cap)) goto done;
    // 结果的 transport（session/generation/requestId）在嵌套对象 transport 里（编译器
    // DirectShaderCompiler.cs:536 写的是 {"transport":{"session":…,"generation":…,"requestId":…}}），
    // 不在顶层；顶层只有 schema/contentHash/content/identity。先把 content 的裸视图取出来，
    // 再解析 transport（j 会被覆盖），最后解析 content 与 content.identity。
    const s2a_json_field *fc = s2a_json_get(j, "content");
    if (!fc || fc->kind != S2A_JSON_OBJECT) { error(why, cap, "compute content missing"); goto done; }
    const uint8_t *content_raw = fc->raw; size_t content_len = fc->raw_bytes;
    if (fresh) {
        // 结果的 transport（session/generation/requestId）在嵌套对象 transport 里（编译器
        // DirectShaderCompiler.cs:536 写的是 {"transport":{"session":…,"generation":…,"requestId":…}}），
        // 不在顶层。fresh 结果必须三项全等价回显；持久化结果只校验内容身份（下面 fresh=0 路径不变）。
        const s2a_json_field *ft = s2a_json_get(j, "transport");
        if (!ft || ft->kind != S2A_JSON_OBJECT) { error(why, cap, "compute result transport missing"); goto done; }
        if (s2a_json_parse_object(ft->raw, ft->raw_bytes, j, why, cap)) goto done;
        if (!number_is(j, "session", e->session) || !number_is(j, "generation", e->generation) ||
            !string_is(j, "requestId", e->request_id)) {
            // 诊断要能直接定位是哪一项漂移（got/want），不能只剩一句 mismatch——
            // 真机上拿不到结果 JSON，只能看这一行 why。
            const s2a_json_field *sf = s2a_json_get(j, "session");
            const s2a_json_field *gf = s2a_json_get(j, "generation");
            const s2a_json_field *rf = s2a_json_get(j, "requestId");
            char got_s[25] = "missing", got_g[25] = "missing", got_r[48] = "missing";
            if (sf && sf->raw_bytes < sizeof got_s) {
                memcpy(got_s, sf->raw, sf->raw_bytes); got_s[sf->raw_bytes] = 0;
            }
            if (gf && gf->raw_bytes < sizeof got_g) {
                memcpy(got_g, gf->raw, gf->raw_bytes); got_g[gf->raw_bytes] = 0;
            }
            if (rf && rf->kind == S2A_JSON_STRING) {
                size_t rn = rf->text_bytes < sizeof got_r - 1 ? rf->text_bytes : sizeof got_r - 1;
                memcpy(got_r, rf->text, rn); got_r[rn] = 0;
            }
            char message[320];
            snprintf(message, sizeof message,
                "compute result transport mismatch (session got=%s want=%llu generation got=%s want=%llu requestId got=%s want=%.48s)",
                got_s, (unsigned long long)e->session, got_g, (unsigned long long)e->generation,
                got_r, e->request_id);
            error(why, cap, message); goto done;
        }
    }
    if (s2a_json_parse_object(content_raw, content_len, j, why, cap)) goto done;
    const s2a_json_field *fi = s2a_json_get(j, "identity");
    if (!fi || fi->kind != S2A_JSON_OBJECT) { error(why, cap, "compute identity missing"); goto done; }
    if (s2a_json_parse_object(fi->raw, fi->raw_bytes, j, why, cap)) goto done;
    if (!number_is(j, "directABI", e->abi) || !string_is(j, "translatorClosure", e->closure) ||
        !string_is(j, "profileHash", e->profile) || !string_is(j, "runtimeCompilerABI", "s2a-direct-compile-v1")) {
        error(why, cap, "compute result identity mismatch"); goto done;
    }
    struct nvn_vk_compute_program p = {0};
    rc = nvn_vk_program_parse_compute(bytes, length, &p, why, cap);
    if (!rc && p.has_unaligned_storage_buffer != e->unaligned)
        rc = error(why, cap, "compute reflection/request alignment mismatch");
    nvn_vk_program_free_compute(&p);
done:
    free(j); return rc;
}
/* Read the actual REQUEST facts, not a default or a guessed cache candidate.
 * requestId is the same root field the producer echoes under
 * result.transport; absent request text leaves it empty and fresh results then
 * reject on the echo (持久化结果 fresh=0 不经过这里，不受影响). */
static int request_facts(const struct nvk_compute_request *r, int *out,
                         char *request_id, size_t id_cap, char *why, size_t cap) {
    s2a_json_object *j = malloc(sizeof *j);
    if (!j) return error(why, cap, "compute request allocation failed");
    request_id[0] = 0;
    const uint8_t *raw = (const uint8_t *)r->json; size_t bytes = r->json_bytes;
    const char *path[] = {"program", "computeState"};
    int rc = -1;
    for (unsigned i = 0; i < 2; i++) {
        if (s2a_json_parse_object(raw, bytes, j, why, cap)) goto done;
        if (!i) {
            const s2a_json_field *idf = s2a_json_get(j, "requestId");
            if (idf && idf->kind == S2A_JSON_STRING)
                snprintf(request_id, id_cap, "%s", idf->text);
        }
        const s2a_json_field *f = s2a_json_get(j, path[i]);
        if (!f || f->kind != S2A_JSON_OBJECT) goto missing;
        raw = f->raw; bytes = f->raw_bytes;
    }
    if (s2a_json_parse_object(raw, bytes, j, why, cap)) goto done;
    const s2a_json_field *f = s2a_json_get(j, "hasUnalignedStorageBuffer");
    if (!f || (f->kind != S2A_JSON_TRUE && f->kind != S2A_JSON_FALSE)) goto missing;
    *out = f->kind == S2A_JSON_TRUE; rc = 0; goto done;
missing:
    error(why, cap, "compute request alignment fact missing or invalid");
done:
    free(j); return rc;
}
/* #1434 第二笔：live profile 的 supportLayout.requiredSize —— LibPack compute 结果
 * 里该字段的唯一合法真值源（_b.json 不带 profile 级量，不写死常量）。 */
static int profile_support_required(const struct s2a_direct_compile_context *c, uint32_t *out) {
    if (!c->profile_json || !c->profile_bytes) return -1;
    s2a_json_object *j = malloc(sizeof *j);
    if (!j) return -1;
    const s2a_json_field *f; int rc = -1;
    if (!s2a_json_parse_object((const uint8_t *)c->profile_json, c->profile_bytes, j, NULL, 0)) {
        f = s2a_json_get(j, "supportLayout");
        if (f && f->kind == S2A_JSON_OBJECT &&
            !s2a_json_parse_object(f->raw, f->raw_bytes, j, NULL, 0)) {
            f = s2a_json_get(j, "requiredSize");
            if (f && f->kind == S2A_JSON_NUMBER && f->number >= 1 && f->number <= 0xffffffff) {
                *out = (uint32_t)f->number; rc = 0;
            }
        }
    }
    free(j); return rc;
}
int nvk_compute_lookup(const struct nvk_compute_request *r,
    const struct s2a_direct_compile_context *c, const struct nvk_compute_lookup_options *options,
    struct nvk_compute_lookup_result *out, char *why, size_t cap) {
    if (!out) return error(why, cap, "compute lookup output missing");
    memset(out, 0, sizeof *out);
    if (why && cap) why[0] = 0;
    if (!r || !c || !options || !r->json || !r->json_bytes || !r->canonical_bytes ||
        r->session != c->session || r->generation != c->generation || !c->session || !c->generation ||
        !c->direct_abi || !c->translator_closure || strlen(c->translator_closure) > 256 ||
        !c->profile_hash || strlen(c->profile_hash) != 71 || strncmp(c->profile_hash, "sha256-", 7) ||
        (!!options->pause != !!options->resume))
        return error(why, cap, "compute lookup input or context invalid");
    if (!r->profile.storage_alignment || r->profile.storage_alignment > 65536 ||
        (r->profile.storage_alignment & (r->profile.storage_alignment - 1)) ||
        strncmp(r->profile.hash, c->profile_hash, sizeof r->profile.hash))
        return error(why, cap, "compute prepared profile differs from lookup context");
    if (!s2a_direct_provider_compile_context)
        return error(why, cap, "compute provider context unavailable");
    struct expected e = {.session=c->session, .generation=c->generation, .abi=c->direct_abi};
    if (request_facts(r, &e.unaligned, e.request_id, sizeof e.request_id, why, cap)) return -1;
    snprintf(e.closure, sizeof e.closure, "%s", c->translator_closure);
    snprintf(e.profile, sizeof e.profile, "%s", c->profile_hash);
    if (!same_context(&e)) return error(why, cap, "compute provider is stale or stopped");
    uint8_t *bytes = NULL; size_t length = 0;
    if (options->revision_root) {
        struct s2a_offline_request q = {.direct_abi=e.abi, .profile_hash=e.profile,
            .profile_declared=c->offline_profile_available};
        memcpy(q.identity, r->identity, 32);
        struct s2a_offline_program p = {0};
        int rc = s2a_offline_library_fetch(options->revision_root, &q, &p, out->offline_reason, sizeof out->offline_reason);
        if (rc == S2A_OFFLINE_HIT && !accept(p.bytes, p.len, &e, 0, out->offline_reason, sizeof out->offline_reason)) {
            bytes=p.bytes; length=p.len; out->origin=NVK_COMPUTE_OFFLINE;
        } else free(p.bytes);
    }
    /* #1434 第二笔：离线库 miss 后、私有缓存前插 LibPack compute 查询（draw 侧同序）。
     * 8 字节 packed CS 键域独立于 32 字节 identity 键域——两条键域不互查。
     * 命中按已合成 v1 走同一 accept 复核，归 origin=OFFLINE，不写私有缓存；
     * miss/reject 如实落 offline_reason（与离线库原因并存取先写者）。 */
    if (!bytes && options->libpack && options->libpack_key) {
        uint32_t sreq = 0;
        char lwhy[160] = "";
        int lrc = profile_support_required(c, &sreq)
            ? (snprintf(lwhy, sizeof lwhy, "profile supportLayout.requiredSize unavailable"),
               S2A_LIBPACK_REJECT)
            : s2a_libpack_compute_v1(options->libpack, options->libpack_key,
                e.unaligned, sreq, e.abi, e.profile, e.closure, &bytes, &length, lwhy, sizeof lwhy);
        if (lrc == S2A_LIBPACK_HIT &&
            !accept(bytes, length, &e, 0, out->offline_reason, sizeof out->offline_reason)) {
            out->origin = NVK_COMPUTE_OFFLINE;
        } else {
            free(bytes); bytes = NULL; length = 0;
            if (lwhy[0]) {
                size_t used = strlen(out->offline_reason);
                snprintf(out->offline_reason + used, sizeof out->offline_reason - used,
                         "%slibpack: %s", used ? "; " : "", lwhy);
            }
        }
    }
    if (!bytes && options->private_cache_root) {
        uint8_t digest[32];
        if (!s2a_program_cache_load(options->private_cache_root, r->identity, &bytes, &length, digest,
                out->private_reason, sizeof out->private_reason)) {
            if (accept(bytes, length, &e, 0, out->private_reason, sizeof out->private_reason)) {
                free(bytes); bytes=NULL; length=0;
            } else out->origin=NVK_COMPUTE_PRIVATE;
        }
    }
    if (!same_context(&e)) { free(bytes); return error(why, cap, "compute provider changed during cache lookup"); }
    if (!bytes && options->no_live_compile)
        return error(why, cap, "compute miss: live compilation disabled");
    if (!bytes) {
        int rc = s2a_compile_worker_run(e.session, e.generation, r->json, r->json_bytes, r->identity, 10000,
            options->pause, options->resume, options->pause_context, &bytes, &length, why, cap);
        if (rc) { free(bytes); return -1; } /* cancellation/timeout is terminal, no sync retry */
        /* F4.1：现编结果封套与请求同名配对写本局私有诊断目录，且在 accept 之前——
         * transport/反射/校验拒掉的封套正是现场实据。文件名合同 <requestId>-<身份
         * 全 64hex>.compile-result.json 与请求侧（bridge.c 的 diag_save）同源；落盘
         * 失败只丢诊断（目录不可用/名截断），不影响结果判定、不回落写别处。 */
        {
            const char *diag = getenv("S2A_DIRECT_DIAG_DIR");
            if (options->diag_open && diag && diag[0] && bytes) {
                static const char hex[] = "0123456789abcdef";
                char hid[65];
                for (unsigned i = 0; i < 32; ++i) { hid[i * 2] = hex[r->identity[i] >> 4]; hid[i * 2 + 1] = hex[r->identity[i] & 15]; }
                hid[64] = 0;
                char dname[256], dwhy[160] = "";
                int n = snprintf(dname, sizeof dname, "%s-%s.compile-result.json", e.request_id, hid);
                if (n < 0 || (size_t)n >= sizeof dname) {
#ifndef __ANDROID__
                    if (__android_log_print)
#endif
                        __android_log_print(4 /* ANDROID_LOG_INFO */, "S2Arm",
                            "[s2a-direct-diag] stage=cs diag filename truncated, compile-result.json skipped");
                } else
                    (void)s2a_direct_diag_write(diag, dname, bytes, length, dwhy, sizeof dwhy);
            }
        }
        if (!same_context(&e)) { free(bytes); return error(why, cap, "compute provider changed during compilation"); }
        if (accept(bytes, length, &e, 1, why, cap)) { free(bytes); return -1; }
        out->origin=NVK_COMPUTE_COMPILED;
        if (options->private_cache_root)
            out->cache_store_failed = s2a_program_cache_store_async(options->private_cache_root, r->identity, bytes, length) != 0;
    }
    out->profile=r->profile;
    out->bytes=bytes; out->length=length;
    // 对齐 main 的 lookup 合同（header 声明了 c1/c1_bytes）：消费点快照里的 c1 指针/长度原样带回。
    // 本线 compute build 目前直接从 source->c1 取，两条路径同源，不产生第二份拷贝。
    out->c1 = r->source.c1;
    out->c1_bytes = r->source.c1_bytes;
    return 0;
}
