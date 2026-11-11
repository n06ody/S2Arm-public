// #1434 安卓离线 Vulkan 直连库的 POSIX 段读器 + _b.json → s2a-direct-program-v1 适配。
//   契约与计数口径见 libpack_vk.h；容器格式/共用核在 graphics/nvn/lib_pack_core.inc。
// 绑定换算的真值（graphics/tools/direct/dsh/DshVk.cs + 宿主夹具 t_dsh_vk 实测核对）：
//   成员绑定号 = arg_id × 6 + StageNo（vs 0 / hs 1 / ds 2 / gs 3 / ps 4 / cs 5）；
//   v1 texture slot 是 cb2 句柄表的字偏移（slot×4 = 字节偏移：0x20 区 = 纹理槽 0..31、
//   0x168 区 = 128+），_b.json 的 tex reg 是已解好的 NVN 纹理号 → slot = 8 + reg×2（reg<32）
//   或 0x5A + (reg-128)×2；img 句柄在 0x120 区 → slot = 0x48 + reg×2。
#include "libpack_vk.h"
#include "jv.h"   // 跨度式 JSON：_b.json 有长串字段（pos_slice 表 base64），s2a_json 短串上限吃不下

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
static void s2a_lp_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "S2Arm", fmt, ap);
    va_end(ap);
}
#else
static void s2a_lp_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
#endif
#define LP_LOG(...) s2a_lp_log(__VA_ARGS__)

#include "../../../graphics/nvn/lib_pack_core.inc"
#include "../../../graphics/nvn/gfx/nvn/dmsh_chain.h"

struct s2a_libpack { lp_pack p; };

static _Atomic uint64_t g_hits, g_misses, g_rejects;
uint64_t s2a_libpack_hits(void) { return atomic_load(&g_hits); }
uint64_t s2a_libpack_misses(void) { return atomic_load(&g_misses); }
uint64_t s2a_libpack_rejects(void) { return atomic_load(&g_rejects); }

int s2a_libpack_open(struct s2a_libpack **out, const char *dir) {
    if (!out || !dir || !*dir) return -1;
    struct s2a_libpack *lp = calloc(1, sizeof *lp);
    if (!lp) return -1;
    int rc = lp_open(&lp->p, dir, DMSH_CHAIN);
    if (rc == 1) { *out = lp; return 1; }
    free(lp);
    return rc;   // 0 none / -1 broken（lp_open 已报红）
}

void s2a_libpack_close(struct s2a_libpack *lp) {
    if (!lp) return;
    if (lp->p.idx) munmap((void *)lp->p.idx, lp->p.idx_len);
    free(lp);
}

static uint64_t lp_key8(const uint8_t key[8]) {
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = (k << 8) | key[i];   // lp_key 的同值：文件名 16 位 hex = 8 字节大端
    return k;
}

int s2a_libpack_get(const struct s2a_libpack *lp, const uint8_t key[8], unsigned kind,
                    uint8_t **bytes, size_t *len, char *why, size_t why_size) {
    if (!lp || !lp->p.idx || !key || !bytes || !len) return S2A_LIBPACK_INVALID;
    *bytes = NULL; *len = 0;
    const lp_rec *r = lp_find(&lp->p, lp_key8(key), kind);
    if (!r) return S2A_LIBPACK_MISS;
    if (r->shard >= lp->p.nshard || r->off + r->len > lp->p.ssize[2 * r->shard]) {
        if (why && why_size) snprintf(why, why_size, "record points outside shard table (%u/%u off=%llu len=%u)",
                                      r->shard, lp->p.nshard, (unsigned long long)r->off, r->len);
        return S2A_LIBPACK_REJECT;
    }
    char path[1100]; snprintf(path, sizeof path, "%s/lib.%03u.pak", lp->p.dir, r->shard);
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || (uint64_t)st.st_size != lp->p.ssize[2 * r->shard]) {
        if (why && why_size) snprintf(why, why_size, "shard %s is %s (index says %llu bytes)", path,
                                      fd < 0 ? "missing" : "the wrong size", (unsigned long long)lp->p.ssize[2 * r->shard]);
        if (fd >= 0) close(fd);
        return S2A_LIBPACK_REJECT;
    }
    const long ps = sysconf(_SC_PAGESIZE);   // getpagesize 已于 POSIX.1-2008 撤稿，宿主严标编译环境不给声明
    const uint64_t pg = (uint64_t)(ps > 0 ? ps : 4096), at = r->off & ~(pg - 1), delta = r->off - at;
    void *m = mmap(NULL, (size_t)(delta + (r->len ? r->len : 1)), PROT_READ, MAP_PRIVATE, fd, (off_t)at);
    int e = errno;
    close(fd);
    if (m == MAP_FAILED) {
        if (why && why_size) snprintf(why, why_size, "cannot map %u bytes at %#llx from %s (errno %d)",
                                      r->len, (unsigned long long)r->off, path, e);
        return S2A_LIBPACK_REJECT;
    }
    uint8_t *out = malloc(r->len ? r->len : 1);
    if (out) memcpy(out, (const uint8_t *)m + delta, r->len);
    munmap(m, (size_t)(delta + (r->len ? r->len : 1)));
    if (!out) {
        if (why && why_size) snprintf(why, why_size, "out of memory for %u bytes", r->len);
        return S2A_LIBPACK_REJECT;
    }
    *bytes = out; *len = r->len;
    return S2A_LIBPACK_HIT;
}

// ---------------- _b.json → s2a-direct-program-v1 ----------------

// 阶段名 ↔ StageNo（DshVk.cs StageNo 表同值）/ NVN 位序 ↔ v1 阶段名
static int lp_stage_no(const char *s) {
    if (!strcmp(s, "vs")) return 0; if (!strcmp(s, "hs")) return 1; if (!strcmp(s, "ds")) return 2;
    if (!strcmp(s, "gs")) return 3; if (!strcmp(s, "ps")) return 4; if (!strcmp(s, "cs")) return 5;
    return -1;
}
// NVN 位 → 库内阶段名（vk 库只出 vs/ps/cs；gs/tcs/tes 给出 NULL → 程序 MISS）
static const char *lp_nvn_stage_name(unsigned nvn) {
    switch (nvn) { case 0: return "vs"; case 1: return "ps"; case 5: return "cs"; default: return NULL; }
}
static const char *lp_v1_stage_name(const char *lib_stage) {
    return !strcmp(lib_stage, "ps") ? "fs" : lib_stage;   // v1 用 NVN 阶段名 fs；vs/cs 同名
}
// 纹理号 → cb2 句柄表字偏移（v1 texture slot 的约定）：0x20 区 0..31、0x168 区 128+
static uint32_t lp_tex_slot_word(uint32_t reg) {
    return reg < 32 ? 8 + reg * 2 : 0x5A + (reg - 128) * 2;
}
static uint32_t lp_img_slot_word(uint32_t reg) {
    return 0x48 + reg * 2;   // IMG_BASE 0x120 / 4
}

// 增长缓冲（v1 字节是 snprintf 合成的 JSON；段都不长，失败即拒不算半份）
struct jw { uint8_t *b; size_t n, cap; int oom; };
static void jw_put(struct jw *j, const char *s, size_t n) {
    if (j->oom) return;
    if (j->n + n + 1 > j->cap) {
        size_t c = j->cap ? j->cap : 4096;
        while (c < j->n + n + 1) c *= 2;
        uint8_t *nb = realloc(j->b, c);
        if (!nb) { j->oom = 1; return; }
        j->b = nb; j->cap = c;
    }
    memcpy(j->b + j->n, s, n); j->n += n; j->b[j->n] = 0;
}
static void jw_str(struct jw *j, const char *s) { jw_put(j, s, strlen(s)); }
static void jw_fmt(struct jw *j, const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n > 0) jw_put(j, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void jw_b64(struct jw *j, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        char q[5] = { B64[v >> 18], B64[(v >> 12) & 63], i + 1 < n ? B64[(v >> 6) & 63] : '=', i + 2 < n ? B64[v & 63] : '=', 0 };
        jw_put(j, q, 4);
    }
}

// jv 跨度上的字段薄封装：_b.json 成员对象 / 顶层对象的取值都走这几个
static int jf_u32(struct s2a_jv o, const char *name, uint32_t *v) {
    struct s2a_jv f;
    if (s2a_jv_obj_find(o, name, &f)) return -1;
    return s2a_jv_u32(f, v);
}
static int jf_str(struct s2a_jv o, const char *name, char *out, size_t cap) {
    struct s2a_jv f;
    if (s2a_jv_obj_find(o, name, &f)) return -1;
    return s2a_jv_str(f, out, cap);
}
static int jf_bool(struct s2a_jv o, const char *name, uint32_t *v) {
    struct s2a_jv f;
    if (s2a_jv_obj_find(o, name, &f)) return -1;
    int b;
    if (s2a_jv_bool(f, &b)) return -1;
    *v = (uint32_t)b; return 0;
}
static int lp_fail(char *why, size_t why_size, const char *fmt, ...) {
    if (why && why_size) {
        va_list ap; va_start(ap, fmt); vsnprintf(why, why_size, fmt, ap); va_end(ap);
    }
    return -1;
}

// 一个键的 _b.json bindings[] → v1 content.bindings[]（追加进 jw；sep 管逗号）
// stage_no = 本段 StageNo（绑定号换算用它），v1_stage = v1 阶段名标签
static int lp_emit_bindings(struct jw *j, struct s2a_jv top, const char *lib_stage,
                            int stage_no, const char *v1_stage, int *sep, char *why, size_t why_size) {
    struct s2a_jv arr;
    if (s2a_jv_obj_find(top, "bindings", &arr) || !s2a_jv_is_array(arr))
        return lp_fail(why, why_size, "%s: no bindings array", lib_stage);
    int n = s2a_jv_arr_count(arr);
    if (n < 0) return lp_fail(why, why_size, "%s: bindings array syntax", lib_stage);
    for (int i = 0; i < n; i++) {
        struct s2a_jv b;
        if (s2a_jv_arr_item(arr, i, &b)) return lp_fail(why, why_size, "%s bindings[%u] span", lib_stage, i);
        char msl[64] = "", kind[24] = "";
        uint32_t arg_id = 0;
        if (jf_str(b, "msl_name", msl, sizeof msl) || jf_str(b, "kind", kind, sizeof kind) || jf_u32(b, "arg_id", &arg_id))
            return lp_fail(why, why_size, "%s bindings[%u] missing msl_name/kind/arg_id", lib_stage, i);
        uint32_t binding = arg_id * 6 + (uint32_t)stage_no;
        // 采样器半边：组合纹理在 .spv 只留一个合并变量、吃 texture 成员的号（dsh_vk ②）——v1 不出行
        size_t ml = strlen(msl);
        int sampler_half = (ml > 5 && !strcmp(msl + ml - 5, "Smplr"));
        const char *v1kind = NULL; uint32_t slot = 0; int has_slot = 0;
        char provenance[96] = "";   // storage 三元出处（nvnSlot / uboSlot+uboOffset / uboPtr）
        if (sampler_half) continue;
        if (!strcmp(kind, "tex") || !strcmp(kind, "tbuf") || !strcmp(kind, "tex_pool")) {
            uint32_t reg = 0; int has_reg = !jf_u32(b, "reg", &reg);
            v1kind = "texture";
            if (has_reg && strcmp(kind, "tex_pool")) { slot = lp_tex_slot_word(reg); has_slot = 1; }
        } else if (!strcmp(kind, "tex_ubo") || !strcmp(kind, "tbuf_ubo") || !strcmp(kind, "tex_c2a")) {
            v1kind = "texture";   // 句柄在游戏 UBO 里：v1 无对应字段，出绑定不挂槽（按缺席槽位如实）
        } else if (!strcmp(kind, "samp") || !strcmp(kind, "samp_id") || !strcmp(kind, "samp_ubo")
                   || !strcmp(kind, "samp_c2a") || !strcmp(kind, "samp_pool") || !strcmp(kind, "samp_tex")) {
            continue;   // 采样器半边/独立采样器句柄：v1 是合并纹理模型，不出行
        } else if (!strcmp(kind, "img") || !strcmp(kind, "img_ubo")) {
            uint32_t reg = 0;
            v1kind = "image";
            if (!strcmp(kind, "img") && !jf_u32(b, "reg", &reg)) { slot = lp_img_slot_word(reg); has_slot = 1; }
        } else if (!strcmp(kind, "cbuf")) {
            uint32_t reg = 0;
            if (jf_u32(b, "reg", &reg)) return lp_fail(why, why_size, "%s bindings[%u] cbuf without reg", lib_stage, i);
            v1kind = "uniform"; slot = reg + 3; has_slot = 1;   // v1：驱动槽号，消费端 ubo_slot = slot-3
        } else if (!strcmp(kind, "c1") || !strcmp(kind, "drvcb") || !strcmp(kind, "ubolen")
                   || !strcmp(kind, "sbsize") || !strcmp(kind, "uvx") || !strcmp(kind, "lb")) {
            v1kind = "uniform";   // 宿主常量/表：真实 UBO 描述符、无 guest 槽
        } else if (!strcmp(kind, "geometry") || !strcmp(kind, "sbuf") || !strcmp(kind, "sbuf_ubo")
                   || !strcmp(kind, "ubo_ptr") || !strcmp(kind, "gmem_map") || !strcmp(kind, "gmem_log")) {
            v1kind = "storage";
            uint32_t v = 0;
            if (!strcmp(kind, "sbuf")) {
                if (!jf_u32(b, "reg", &v)) snprintf(provenance, sizeof provenance, ",\"nvnSlot\":%u", v);
            } else if (!strcmp(kind, "sbuf_ubo")) {
                uint32_t off = 0;
                if (!jf_u32(b, "reg", &v) && !jf_u32(b, "off", &off))
                    snprintf(provenance, sizeof provenance, ",\"uboSlot\":%u,\"uboOffset\":%u", v, off);
            } else if (!strcmp(kind, "ubo_ptr")) {
                if (!jf_u32(b, "reg", &v)) snprintf(provenance, sizeof provenance, ",\"uboPtr\":%u", v);
            }
            // gmem_map/gmem_log/geometry：宿主哨兵槽，无 guest 映射——storage_kind 留 0，消费端如实报不支持
        } else if (!strcmp(kind, "support")) {
            // v1：程序级支撑缓冲。set/binding 仍是本段的 arg_id 换算号；stage 标签 "program"
            jw_fmt(j, "%s{\"kind\":\"support\",\"stage\":\"program\",\"set\":0,\"binding\":%u}", *sep ? "," : "", binding);
            *sep = 1;
            continue;
        } else {
            return lp_fail(why, why_size, "%s bindings[%u] %s kind '%s' not mappable", lib_stage, i, msl, kind);
        }
        uint32_t rw = 0; int has_rw = !jf_bool(b, "rw", &rw);
        // storage 的 rw 是 v1 必填事实：geometry 是着色器写目标的宿主辅助缓冲（vb/ib/trb/vb_info），
        //   可写是它的定义事实，_b.json 不落 rw 字段；其余 storage 类 _b.json 缺 rw 就如实拒绝，
        //   不能代造 false（把可写缓冲按只读绑是静默错绘，比拒收更糟）
        if (!strcmp(v1kind, "storage") && !has_rw) {
            if (!strcmp(kind, "geometry")) { rw = 1; has_rw = 1; }
            else return lp_fail(why, why_size, "%s bindings[%u] %s kind '%s' missing rw", lib_stage, i, msl, kind);
        }
        jw_fmt(j, "%s{\"kind\":\"%s\",\"stage\":\"%s\",\"set\":0,\"binding\":%u", *sep ? "," : "", v1kind, v1_stage, binding);
        *sep = 1;
        if (has_slot) jw_fmt(j, ",\"slot\":%u", slot);
        if (provenance[0]) jw_str(j, provenance);
        if (!strcmp(v1kind, "storage")) jw_fmt(j, ",\"rw\":%s", rw ? "true" : "false");
        jw_str(j, "}");
    }
    return 0;
}

// 一个阶段：取键的 _b.json + _b.spv → 校验 → 往 jw 写 stage 项与 bindings 项。
// 返回 0 成功 / 1 miss / -1 reject
static int lp_stage_emit(struct s2a_libpack *lp, const uint8_t key[8], unsigned nvn,
                         struct jw *stages, struct jw *binds, int *bind_sep, uint32_t *stage_mask,
                         char *why, size_t why_size) {
    const char *lib_stage = lp_nvn_stage_name(nvn);
    if (!lib_stage) return 1;   // gs/tcs/tes：vk 库不出此阶段，整个程序按 miss
    char khex[17];
    for (int i = 0; i < 8; i++) snprintf(khex + 2 * i, 3, "%02x", key[i]);
    uint8_t *jb = NULL, *spv = NULL; size_t jn = 0, sn = 0;
    char gw[192] = "";
    int rc = s2a_libpack_get(lp, key, LP_K_BJSON, &jb, &jn, gw, sizeof gw);
    if (rc != S2A_LIBPACK_HIT) { if (why && why_size) snprintf(why, why_size, "%s _b.json: %s", khex, gw[0] ? gw : "absent"); free(jb); return rc; }
    rc = s2a_libpack_get(lp, key, LP_K_BSPV, &spv, &sn, gw, sizeof gw);
    if (rc != S2A_LIBPACK_HIT) { if (why && why_size) snprintf(why, why_size, "%s _b.spv: %s", khex, gw[0] ? gw : "absent"); free(jb); free(spv); return rc; }
    struct s2a_jv top = { (const char *)jb, jn };
    int out = -1;
    {   // 全文档语法核 + 顶层必须恰是一个对象（jv 不具现字符串，长串字段只吃跨度）
        const char *jend = NULL;
        if (!s2a_jv_is_object(top) || s2a_jv_scan((const char *)jb, (const char *)jb + jn, &jend)
            || jend != (const char *)jb + jn) {
            lp_fail(why, why_size, "%s _b.json: not a complete JSON object", khex); goto done;
        }
    }
    {   // 命中判据：dm_ctl == 键 hex、stage 相符、_b.spv 是合法 SPIR-V
        char ctl[20] = "", st[8] = "";
        if (jf_str(top, "dm_ctl", ctl, sizeof ctl) || strcmp(ctl, khex))
            { lp_fail(why, why_size, "%s _b.json dm_ctl mismatch", khex); goto done; }
        if (jf_str(top, "stage", st, sizeof st) || strcmp(st, lib_stage))
            { lp_fail(why, why_size, "%s _b.json stage is '%s' not '%s'", khex, st, lib_stage); goto done; }
        if (sn < 20 || (sn & 3) || *(const uint32_t *)spv != 0x07230203u)
            { lp_fail(why, why_size, "%s _b.spv is not a SPIR-V module", khex); goto done; }
        int stage_no = lp_stage_no(st);
        // vertex_inputs[].location → attributeReadMask
        uint32_t mask = 0;
        struct s2a_jv vi;
        if (!s2a_jv_obj_find(top, "vertex_inputs", &vi) && s2a_jv_is_array(vi)) {
            int vn = s2a_jv_arr_count(vi);
            for (int i = 0; i < vn; i++) {
                struct s2a_jv ve;
                uint32_t loc = 0;
                if (!s2a_jv_arr_item(vi, i, &ve) && !jf_u32(ve, "location", &loc) && loc < 32) mask |= 1u << loc;
            }
        }
        // tex_scales 非空：scaled-access 绑定映射还没接——如实拒绝不静默丢
        struct s2a_jv ts;
        if (!s2a_jv_obj_find(top, "tex_scales", &ts) && s2a_jv_is_array(ts) && s2a_jv_arr_count(ts) > 0)
            { lp_fail(why, why_size, "%s has tex_scales entries (scaled-access bindings not mapped)", khex); goto done; }
        const char *v1_stage = lp_v1_stage_name(st);
        if (lp_emit_bindings(binds, top, st, stage_no, v1_stage, bind_sep, why, why_size)) goto done;
        jw_fmt(stages, "%s{\"stage\":\"%s\",\"spirv\":\"", *stage_mask ? "," : "", v1_stage);
        jw_b64(stages, spv, sn);
        if (mask) jw_fmt(stages, "\",\"attributeReadMask\":%u}", mask);
        else jw_str(stages, "\"}");
        *stage_mask |= 1u << nvn;
        out = 0;
    }
done:
    free(jb); free(spv);
    return out;
}

int s2a_libpack_program_v1(const struct s2a_libpack *lp_c, const uint8_t keys[6][8], uint32_t stages,
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size) {
    struct s2a_libpack *lp = (struct s2a_libpack *)lp_c;
    if (!lp || !lp->p.idx || !keys || !json || !json_len || !stages) return S2A_LIBPACK_INVALID;
    *json = NULL; *json_len = 0;
    // v1 图形接受口径就是 vs+fs 两阶段（nvn_vk_program_parse_graphics 判据）：
    //   别的形状（单阶段 / 带 gs|tcs|tes / cs 走 compute 线）本函数产不出可采纳结果，按 miss 如实回。
    if (stages != 0x3) {
        if (why && why_size) snprintf(why, why_size, "stages=0x%x is not a vs+fs program", stages);
        atomic_fetch_add(&g_misses, 1);
        return S2A_LIBPACK_MISS;
    }
    int worst = S2A_LIBPACK_HIT;
    struct jw st = {0}, bd = {0};
    int sep = 0; uint32_t got = 0;
    for (unsigned nvn = 0, idx = 0; nvn < 6 && idx < 6; nvn++) {
        if (!(stages & (1u << nvn))) continue;
        char sw[192] = "";
        int rc = lp_stage_emit(lp, keys[idx], nvn, &st, &bd, &sep, &got, sw, sizeof sw);
        idx++;
        if (rc == 0) continue;
        // 半程序不命中：任一所需阶段拿不到，整个程序按该段的 miss/reject 计
        //   （reject 优先于 miss：有一个坏段就说明库有问题，不是“库没有”）
        if (why && why_size) snprintf(why, why_size, "%s", sw);
        if (rc < 0 || worst == S2A_LIBPACK_HIT) worst = rc;
    }
    if (worst != S2A_LIBPACK_HIT || got != stages) {
        if (worst == S2A_LIBPACK_HIT) worst = S2A_LIBPACK_MISS;   // 阶段位图对不上 = 库没有，miss
        goto out;
    }
    {
        struct jw j = {0};
        jw_fmt(&j, "{\"schema\":\"s2a-direct-program-v1\",\"content\":{\"contract\":\"s2a-direct-program-content-v1\",\"identity\":{\"directABI\":%u,\"translatorClosure\":\"%s\",\"profileHash\":\"%s\"},\"kind\":\"graphics\",\"stages\":[",
               direct_abi, translator_closure ? translator_closure : "", profile_hash ? profile_hash : "");
        jw_put(&j, st.b ? (char *)st.b : "", st.n);
        jw_str(&j, "],\"bindings\":[");
        jw_put(&j, bd.b ? (char *)bd.b : "", bd.n);
        jw_str(&j, "]}}");
        if (j.oom) { free(j.b); lp_fail(why, why_size, "v1 synthesis out of memory"); worst = S2A_LIBPACK_REJECT; goto out; }
        *json = j.b; *json_len = j.n;
        worst = S2A_LIBPACK_HIT;
    }
out:
    free(st.b); free(bd.b);
    if (worst == S2A_LIBPACK_HIT) atomic_fetch_add(&g_hits, 1);
    else if (worst == S2A_LIBPACK_MISS) atomic_fetch_add(&g_misses, 1);
    else atomic_fetch_add(&g_rejects, 1);
    return worst;
}
