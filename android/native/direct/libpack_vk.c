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

// Enumerate descriptor variables from the Vulkan module, not the Metal reflection.
// The producer gives the two backends separate descriptor lists. A name without
// an OpVariable is not a Vulkan binding; a Vulkan variable without a usable name
// or supported type must be rejected rather than invented from a Metal entry.
#define LP_GFX_MAX_IDS 262144u
#define LP_GFX_MAX_DESCS 256u
struct lp_gfx_id {
    uint32_t set, binding, type, target, storage, aux, stride, pos, words;
    uint16_t type_op;
    uint8_t variable, named, name_long, block, constant, sampled, dim;
    char name[80];
};
struct lp_gfx_desc { uint32_t binding; int kind; char name[80]; };

// Compute the declared support block extent from explicit SPIR-V member offsets
// and array strides. Unsupported types/layouts fail closed; never guess a size.
static int lp_gfx_extent(const struct lp_gfx_id *ids, uint32_t bound, const uint32_t *w, size_t n,
                         uint32_t id, unsigned depth, uint32_t *extent) {
    if (id >= bound || depth > 32) return -1;
    const struct lp_gfx_id *t = &ids[id];
    uint32_t sub = 0;
    uint64_t size = 0;
    if (t->type_op == 21 || t->type_op == 22) { // integer / float
        if (!t->aux || t->aux > 64 || (t->aux & 7)) return -1;
        size = t->aux / 8;
    } else if (t->type_op == 23) { // vector
        if (!t->aux || t->aux > 4 || lp_gfx_extent(ids, bound, w, n, t->target, depth+1, &sub)) return -1;
        size = (uint64_t)sub * t->aux;
    } else if (t->type_op == 28) { // fixed array with explicit ArrayStride
        if (!t->stride || t->aux >= bound || !ids[t->aux].constant || !ids[t->aux].aux ||
            ids[t->aux].type >= bound || ids[ids[t->aux].type].type_op != 21 ||
            ids[ids[t->aux].type].aux != 32 ||
            lp_gfx_extent(ids, bound, w, n, t->target, depth+1, &sub) || sub > t->stride) return -1;
        size = (uint64_t)t->stride * ids[t->aux].aux;
    } else if (t->type_op == 30) { // struct: every member must have an explicit offset
        if (t->words < 2 || (size_t)t->pos + t->words > n) return -1;
        for (uint32_t member = 0; member < t->words-2; ++member) {
            uint32_t offset = UINT32_MAX;
            for (size_t at = 5; at < n;) {
                uint32_t op = w[at] & 0xffffu, wc = w[at] >> 16;
                if (!wc || wc > n-at) return -1;
                if (op == 72 && wc >= 5 && w[at+1] == id && w[at+2] == member && w[at+3] == 35) {
                    if (offset != UINT32_MAX) return -1;
                    offset = w[at+4];
                }
                at += wc;
            }
            if (offset == UINT32_MAX || lp_gfx_extent(ids, bound, w, n, w[t->pos+2+member], depth+1, &sub)) return -1;
            uint64_t end = (uint64_t)offset + sub;
            if (end > size) size = end;
        }
    } else return -1;
    if (!size || size > UINT32_MAX) return -1;
    *extent = (uint32_t)size;
    return 0;
}

static int lp_gfx_descriptors(const uint8_t *spv, size_t bytes, int stage_no,
                              struct lp_gfx_desc *out, size_t cap, uint32_t *support_max,
                              char *why, size_t why_size) {
    if (bytes < 20 || (bytes & 3)) return lp_fail(why, why_size, "SPIR-V size invalid");
    const uint32_t *w = (const uint32_t *)spv;
    const size_t n = bytes / 4;
    uint32_t bound = w[3];
    if (!bound || bound > LP_GFX_MAX_IDS) return lp_fail(why, why_size, "SPIR-V id bound invalid");
    struct lp_gfx_id *ids = calloc(bound, sizeof *ids);
    if (!ids) return lp_fail(why, why_size, "SPIR-V descriptor scan out of memory");
    for (uint32_t i = 0; i < bound; ++i) ids[i].set = ids[i].binding = UINT32_MAX;
    int rc = -1;
    for (size_t at = 5; at < n;) {
        uint32_t op = w[at] & 0xffffu, wc = w[at] >> 16;
        if (!wc || wc > n - at) { lp_fail(why, why_size, "invalid SPIR-V instruction length"); goto done; }
        if (op == 5 && wc >= 3) { // OpName
            uint32_t id = w[at+1];
            if (id >= bound) { lp_fail(why, why_size, "SPIR-V name id outside bound"); goto done; }
            size_t available = (wc-2u)*4u;
            const char *s = (const char *)&w[at+2], *end = memchr(s, 0, available);
            if (!end || ids[id].named) { lp_fail(why, why_size, "invalid SPIR-V descriptor name"); goto done; }
            size_t len = (size_t)(end-s); ids[id].named = 1;
            if (len >= sizeof ids[id].name) ids[id].name_long = 1;
            else memcpy(ids[id].name, s, len+1);
        } else if (op == 71 && wc >= 3) { // OpDecorate
            uint32_t id = w[at+1], decoration = w[at+2];
            if (id >= bound) { lp_fail(why, why_size, "SPIR-V decoration id outside bound"); goto done; }
            if (decoration == 2 || decoration == 3) ids[id].block = (uint8_t)decoration;
            if (decoration == 6) {
                if (wc < 4 || !w[at+3] || ids[id].stride) { lp_fail(why, why_size, "invalid SPIR-V array stride"); goto done; }
                ids[id].stride = w[at+3];
            }
            if (decoration == 33 || decoration == 34) {
                if (wc < 4) { lp_fail(why, why_size, "SPIR-V descriptor decoration missing value"); goto done; }
                uint32_t *field = decoration == 33 ? &ids[id].binding : &ids[id].set;
                if (*field != UINT32_MAX) { lp_fail(why, why_size, "duplicate SPIR-V descriptor decoration"); goto done; }
                *field = w[at+3];
            }
        } else if (op == 59 && wc >= 4) { // OpVariable
            uint32_t id = w[at+2];
            if (id >= bound || w[at+1] >= bound || ids[id].variable) { lp_fail(why, why_size, "invalid SPIR-V variable"); goto done; }
            ids[id].variable = 1; ids[id].type = w[at+1]; ids[id].storage = w[at+3];
        } else if (op == 43 && wc == 4) { // 32-bit OpConstant for array lengths
            uint32_t id = w[at+2];
            if (id >= bound || ids[id].constant) { lp_fail(why, why_size, "invalid SPIR-V constant"); goto done; }
            ids[id].constant = 1; ids[id].type = w[at+1]; ids[id].aux = w[at+3];
        } else if ((op == 32 && wc >= 4) || (op == 27 && wc >= 3) ||
                   (op == 25 && wc >= 9) || (op == 28 && wc >= 4) || (op == 29 && wc >= 3) ||
                   (op == 21 && wc >= 4) || (op == 22 && wc >= 3) ||
                   (op == 23 && wc >= 4) || (op == 30 && wc >= 2)) {
            uint32_t id = w[at+1];
            if (id >= bound || ids[id].type_op) { lp_fail(why, why_size, "invalid SPIR-V type id"); goto done; }
            ids[id].type_op = (uint16_t)op;
            if (op == 32) { ids[id].storage = w[at+2]; ids[id].target = w[at+3]; }
            else if (op == 25) {
                if (w[at+3] > UINT8_MAX || w[at+7] > UINT8_MAX) { lp_fail(why, why_size, "invalid SPIR-V image type"); goto done; }
                ids[id].dim = (uint8_t)w[at+3]; ids[id].sampled = (uint8_t)w[at+7];
            }
            else if (op == 21 || op == 22) ids[id].aux = w[at+2];
            else if (op == 23 || op == 28) { ids[id].target = w[at+2]; ids[id].aux = w[at+3]; }
            else if (op == 30) { ids[id].pos = (uint32_t)at; ids[id].words = wc; }
            else ids[id].target = w[at+2];
        }
        at += wc;
    }
    rc = 0;
    for (uint32_t id = 0; id < bound; ++id) {
        struct lp_gfx_id *v = &ids[id];
        if (!v->variable || (v->storage != 0 && v->storage != 2 && v->storage != 12)) continue;
        if (v->set == UINT32_MAX && v->binding == UINT32_MAX) {
            lp_fail(why, why_size, "SPIR-V descriptor lacks set and binding"); rc = -1; goto done;
        }
        if (v->set != 0 || v->binding == UINT32_MAX || v->binding % 6u != (unsigned)stage_no ||
            !v->named || v->name_long || !v->name[0] || v->type >= bound ||
            ids[v->type].type_op != 32 || ids[v->type].storage != v->storage || ids[v->type].target >= bound) {
            lp_fail(why, why_size, "unsupported or incomplete SPIR-V descriptor"); rc = -1; goto done;
        }
        uint32_t target = ids[v->type].target;
        int kind = -1; // 0 uniform, 1 storage, 2 combined texture, 3 image
        if (v->storage == 2) kind = ids[target].block == 3 ? 1 : ids[target].block == 2 ? 0 : -1;
        else if (v->storage == 12) kind = 1;
        else if (ids[target].type_op == 27 && ids[target].target < bound) {
            struct lp_gfx_id *image = &ids[ids[target].target];
            if (image->type_op == 25 && image->sampled == 1 && image->dim != 5) kind = 2;
        } else if (ids[target].type_op == 25 && ids[target].sampled == 2 && ids[target].dim != 5) kind = 3;
        if (kind < 0 || (size_t)rc >= cap) { lp_fail(why, why_size, "unsupported SPIR-V descriptor type/count"); rc = -1; goto done; }
        if (!strcmp(v->name, "support_buffer")) {
            uint32_t required = 0;
            if (kind != 0 || lp_gfx_extent(ids, bound, w, n, target, 0, &required)) {
                lp_fail(why, why_size, "SPIR-V support block extent unavailable"); rc = -1; goto done;
            }
            if (required > *support_max) *support_max = required;
        }
        for (int j = 0; j < rc; ++j) if (out[j].binding == v->binding) {
            lp_fail(why, why_size, "duplicate SPIR-V binding"); rc = -1; goto done;
        }
        out[rc].binding = v->binding; out[rc].kind = kind;
        memcpy(out[rc].name, v->name, sizeof out[rc].name); ++rc;
    }
done:
    free(ids); return rc;
}

static int lp_gfx_bank(const char *name) {
    const char *p = strrchr(name, '_');
    if (!p || p[1] != 'c' || p[2] < '0' || p[2] > '9') return -1;
    unsigned bank = 0;
    for (p += 2; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        bank = bank * 10u + (unsigned)(*p - '0');
        if (bank > 18) return -1;
    }
    return (int)bank;
}

// Emit exactly the Vulkan module's descriptors. JSON is used only to recover
// source facts for descriptors that actually exist; Metal-only members never
// become Vulkan bindings.
static int lp_emit_bindings(struct jw *j, struct s2a_jv top, const char *lib_stage,
                            int stage_no, const char *v1_stage, const uint8_t *spv, size_t spv_len,
                            int *sep, uint32_t *support_max, char *why, size_t why_size) {
    struct s2a_jv arr;
    if (s2a_jv_obj_find(top, "bindings", &arr) || !s2a_jv_is_array(arr))
        return lp_fail(why, why_size, "%s: no bindings array", lib_stage);
    int count = s2a_jv_arr_count(arr);
    if (count < 0 || count > 512) return lp_fail(why, why_size, "%s: invalid binding count", lib_stage);
    struct lp_gfx_desc desc[LP_GFX_MAX_DESCS];
    int nd = lp_gfx_descriptors(spv, spv_len, stage_no, desc, LP_GFX_MAX_DESCS, support_max, why, why_size);
    if (nd < 0) return -1;
    for (int di = 0; di < nd; ++di) {
        const struct lp_gfx_desc *d = &desc[di];
        struct s2a_jv found = {0};
        char kind[24] = "";
        uint32_t arg = 0;
        int have = 0;
        for (int i = 0; i < count; ++i) {
            struct s2a_jv b; char name[80] = "";
            if (s2a_jv_arr_item(arr, i, &b) || jf_str(b, "msl_name", name, sizeof name))
                return lp_fail(why, why_size, "%s bindings[%d] missing name", lib_stage, i);
            if (strcmp(name, d->name)) continue;
            if (have || jf_str(b, "kind", kind, sizeof kind) || jf_u32(b, "arg_id", &arg))
                return lp_fail(why, why_size, "%s descriptor %s has invalid metadata", lib_stage, d->name);
            found = b; have = 1;
        }
        if (have && (arg > UINT32_MAX / 6 || arg * 6u + (unsigned)stage_no != d->binding))
            return lp_fail(why, why_size, "%s descriptor %s binding mismatch", lib_stage, d->name);
        if (!strcmp(d->name, "support_buffer")) {
            if (d->kind != 0 || (have && strcmp(kind, "support")))
                return lp_fail(why, why_size, "%s support descriptor type mismatch", lib_stage);
            jw_fmt(j, "%s{\"kind\":\"support\",\"stage\":\"program\",\"set\":0,\"binding\":%u}", *sep ? "," : "", d->binding);
            *sep = 1; continue;
        }
        uint32_t slot = 0, rw = 0; char prov[160] = "";
        const char *out_kind = NULL;
        int bank = lp_gfx_bank(d->name);
        if (d->kind == 0 && bank >= 0) {
            if ((stage_no == 0 && strncmp(d->name, "vp_", 3)) ||
                (stage_no == 4 && strncmp(d->name, "fp_", 3)) || bank == 2)
                return lp_fail(why, why_size, "%s unsupported cbuf descriptor %s", lib_stage, d->name);
            if (have) {
                uint32_t v = 0;
                if (!strcmp(kind, "cbuf")) {
                    if (bank < 3 || jf_u32(found, "reg", &v) || v != (unsigned)bank-3u)
                        return lp_fail(why, why_size, "%s cbuf metadata mismatch", lib_stage);
                } else if (!strcmp(kind, "c1")) {
                    if (bank != 1) return lp_fail(why, why_size, "%s c1 metadata mismatch", lib_stage);
                } else if (!strcmp(kind, "drvcb")) {
                    if (jf_u32(found, "bank", &v) || v != (unsigned)bank)
                        return lp_fail(why, why_size, "%s driver bank metadata mismatch", lib_stage);
                } else return lp_fail(why, why_size, "%s uniform metadata kind mismatch", lib_stage);
            }
            out_kind = "uniform"; slot = (uint32_t)bank;
        } else if (d->kind == 1 && have && !strcmp(kind, "sbuf")) {
            uint32_t reg = 0;
            if (jf_u32(found, "reg", &reg) || reg >= 16 || jf_bool(found, "rw", &rw))
                return lp_fail(why, why_size, "%s storage descriptor facts missing", lib_stage);
            out_kind = "storage";
            snprintf(prov, sizeof prov, ",\"cbSlot\":0,\"cbOffset\":%u,\"nvnSlot\":%u,\"rw\":%s",
                     (stage_no == 0 ? 0x110u : 0x510u) + 16u*reg, reg, rw ? "true" : "false");
        } else if (d->kind == 2 && have && !strcmp(kind, "tex")) {
            uint32_t reg = 0;
            if (jf_u32(found, "reg", &reg) || reg >= 32)
                return lp_fail(why, why_size, "%s texture slot is unsupported", lib_stage);
            out_kind = "texture"; slot = lp_tex_slot_word(reg);
        } else {
            return lp_fail(why, why_size, "%s descriptor %s kind %d not mappable", lib_stage, d->name, d->kind);
        }
        jw_fmt(j, "%s{\"kind\":\"%s\",\"stage\":\"%s\",\"set\":0,\"binding\":%u",
               *sep ? "," : "", out_kind, v1_stage, d->binding);
        *sep = 1;
        if (d->kind == 0 || d->kind == 2) jw_fmt(j, ",\"slot\":%u", slot);
        if (prov[0]) jw_str(j, prov);
        jw_str(j, "}");
    }
    return 0;
}

// 一个阶段：取键的 _b.json + _b.spv → 校验 → 往 jw 写 stage 项与 bindings 项。
// 返回 0 成功 / 1 miss / -1 reject
static int lp_stage_emit(struct s2a_libpack *lp, const uint8_t key[8], unsigned nvn,
                         struct jw *stages, struct jw *binds, int *bind_sep, uint32_t *stage_mask,
                         uint32_t *support_max, char *why, size_t why_size) {
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
        if (lp_emit_bindings(binds, top, st, stage_no, v1_stage, spv, sn, bind_sep, support_max, why, why_size)) goto done;
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
    int sep = 0; uint32_t got = 0, support_max = 0;
    for (unsigned nvn = 0, idx = 0; nvn < 6 && idx < 6; nvn++) {
        if (!(stages & (1u << nvn))) continue;
        char sw[192] = "";
        int rc = lp_stage_emit(lp, keys[idx], nvn, &st, &bd, &sep, &got, &support_max, sw, sizeof sw);
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
        jw_str(&j, "]");
        if (support_max) jw_fmt(&j, ",\"supportLayout\":{\"requiredSize\":%u}", support_max);
        jw_str(&j, "}}");
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
