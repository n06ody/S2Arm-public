// #1434 安卓离线 Vulkan 直连库的 POSIX 段读器 + _b.json → s2a-direct-program-v1 适配。
//   契约与计数口径见 libpack_vk.h；容器格式/共用核在 graphics/nvn/lib_pack_core.inc。
// 绑定换算的真值（graphics/tools/direct/dsh/DshVk.cs + 宿主夹具 t_dsh_vk 实测核对）：
//   成员绑定号 = arg_id × 6 + StageNo（vs 0 / hs 1 / ds 2 / gs 3 / ps 4 / cs 5）；
//   v1 texture slot 是 cb2 句柄表的字偏移（slot×4 = 字节偏移：0x20 区 = 纹理槽 0..31、
//   0x168 区 = 128+），_b.json 的 tex reg 是已解好的 NVN 纹理号 → slot = 8 + reg×2（reg<32）
//   或 0x5A + (reg-128)×2；img 句柄在 0x120 区 → slot = 0x48 + reg×2。
#include "libpack_vk.h"
#include "libpack_stage_wire.h"
#include "jv.h"   // 跨度式 JSON：_b.json 有长串字段（pos_slice 表 base64），s2a_json 短串上限吃不下
#include "spirv_workgroup.h"   // s2a_spv_workgroup_bytes：计算出库的设备共享内存上限闸
#include "../nvn/nvn_vk_program_result.h"   // SUPPORT_DEFAULT_BYTES：计算结果的 supportLayout.requiredSize

#include <errno.h>
#include <limits.h>
#include <pthread.h>
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
static void s2a_lp_red(const char *fmt, ...) {   // 具名红行：错误级、文本以「【红】」开头（同各模块口径）
    char line[512]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "【红】%s", line);
}
#else
static void s2a_lp_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
static void s2a_lp_red(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fputs("【红】", stderr); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
#endif
#define LP_LOG(...) s2a_lp_log(__VA_ARGS__)
#define LP_RED(...) s2a_lp_red(__VA_ARGS__)

#include "../../../graphics/nvn/lib_pack_core.inc"
#include "../../../graphics/nvn/gfx/nvn/dmsh_chain.h"

// Every slot owns its descriptor. Hold the slot lock through pread so another reader
// cannot close or reuse the descriptor mid-read. Bound fd use even for large indexes.
enum { LP_FD_SLOTS = 16 };
struct lp_fd_slot { pthread_mutex_t lock; unsigned shard; int fd; };
struct s2a_libpack {
    lp_pack p;
    struct lp_fd_slot *slots;
    unsigned nslots;
    _Atomic int caps_said, ilk_said;
};


static _Atomic uint64_t g_hits, g_misses, g_rejects, g_caps_stripped, g_interlock_dropped;
uint64_t s2a_libpack_hits(void) { return atomic_load(&g_hits); }
uint64_t s2a_libpack_caps_stripped(void) { return atomic_load(&g_caps_stripped); }
uint64_t s2a_libpack_interlock_dropped(void) { return atomic_load(&g_interlock_dropped); }
uint64_t s2a_libpack_misses(void) { return atomic_load(&g_misses); }
uint64_t s2a_libpack_rejects(void) { return atomic_load(&g_rejects); }

int s2a_libpack_open(struct s2a_libpack **out, const char *dir) {
    if (!out || !dir || !*dir) return -1;
    struct s2a_libpack *lp = calloc(1, sizeof *lp);
    if (!lp) return -1;
    int rc = lp_open(&lp->p, dir, DMSH_CHAIN);
    if (rc == 1) {
        unsigned n = lp->p.nshard < LP_FD_SLOTS ? lp->p.nshard : LP_FD_SLOTS;
        if (n && !(lp->slots = calloc(n, sizeof *lp->slots))) goto broken;
        for (; lp->nslots < n; lp->nslots++) {
            struct lp_fd_slot *slot = &lp->slots[lp->nslots];
            if (pthread_mutex_init(&slot->lock, NULL)) goto broken;
            slot->fd = -1;
        }
        *out = lp;
        return 1;
    }
    free(lp);
    return rc;   // 0 none / -1 broken（lp_open 已报红）
broken:
    LP_RED("direct library container: cannot initialize shard reader");
    s2a_libpack_close(lp);
    return -1;
}

void s2a_libpack_close(struct s2a_libpack *lp) {
    if (!lp) return;
    for (unsigned i = 0; i < lp->nslots; i++) {
        if (lp->slots[i].fd >= 0) close(lp->slots[i].fd);
        pthread_mutex_destroy(&lp->slots[i].lock);
    }
    free(lp->slots);
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
    if (kind >= 128 && !memcmp(lp->p.idx + 44, "S2SB", 4) &&
        r->len > NVK_PROGRAM_RESULT_MAX_BYTES + SB_HEADER + SB_SECTIONS * SB_SECTION + SB_META_WINDOW_SIZE +
                     2 * (SB_CHOICE + FD_MAX_BINDINGS * SB_BINDING + 4)) {
        if (why && why_size) snprintf(why, why_size, "binary stage exceeds maximum record size");
        return S2A_LIBPACK_REJECT;
    }
    if (lp_shard(r) >= lp->p.nshard || r->off > lp->p.ssize[2 * lp_shard(r)] ||
        r->len > lp->p.ssize[2 * lp_shard(r)] - r->off) {
        if (why && why_size) snprintf(why, why_size, "record points outside shard table (%u/%u off=%llu len=%u)",
                                      lp_shard(r), lp->p.nshard, (unsigned long long)r->off, r->len);
        return S2A_LIBPACK_REJECT;
    }
    unsigned shard = lp_shard(r);
    struct lp_fd_slot *slot = &lp->slots[shard % lp->nslots];
    pthread_mutex_lock(&slot->lock);
    if (slot->fd < 0 || slot->shard != shard) {
        if (slot->fd >= 0) close(slot->fd);
        char path[1100];
        snprintf(path, sizeof path, "%s/lib.%03u.pak", lp->p.dir, shard);
        slot->fd = open(path, O_RDONLY | O_CLOEXEC);
        slot->shard = shard;
    }
    struct stat st;
    uint8_t *out = NULL;
    if (slot->fd < 0 || fstat(slot->fd, &st) || (uint64_t)st.st_size != lp->p.ssize[2 * shard]) {
        if (why && why_size) snprintf(why, why_size, "shard %u is %s (index says %llu bytes)", shard,
            slot->fd < 0 ? "unavailable" : "the wrong size", (unsigned long long)lp->p.ssize[2 * shard]);
        goto fail_read;
    }
    out = malloc(r->len ? r->len : 1);
    if (!out) {
        if (why && why_size) snprintf(why, why_size, "out of memory for %u bytes", r->len);
        goto fail_read;
    }
    size_t done = 0;
    while (done < r->len) {
        size_t left = r->len - done;
        ssize_t got = pread(slot->fd, out + done, left > SSIZE_MAX ? (size_t)SSIZE_MAX : left,
                            (off_t)(r->off + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (why && why_size) snprintf(why, why_size, "shard %u read %zu of %u bytes (%s)",
                                         shard, done, r->len, got < 0 ? strerror(errno) : "unexpected EOF");
            goto fail_read;
        }
        done += (size_t)got;
    }
    pthread_mutex_unlock(&slot->lock);
    *bytes = out; *len = r->len;
    return S2A_LIBPACK_HIT;
fail_read:
    if (slot->fd >= 0) close(slot->fd);
    slot->fd = -1;
    pthread_mutex_unlock(&slot->lock);
    free(out);
    return S2A_LIBPACK_REJECT;
}

// ---------------- _b.json → s2a-direct-program-v1 ----------------

// 阶段名 ↔ StageNo（DshVk.cs StageNo 表同值）/ NVN 位序 ↔ v1 阶段名
static int lp_stage_no(const char *s) {
    if (!strcmp(s, "vs")) return 0;
    if (!strcmp(s, "hs")) return 1;
    if (!strcmp(s, "ds")) return 2;
    if (!strcmp(s, "gs")) return 3;
    if (!strcmp(s, "ps")) return 4;
    if (!strcmp(s, "cs")) return 5;
    return -1;
}
// NVN 位 → 库内阶段名（vk 库出 vs/ps/gs/cs；tcs/tes 给出 NULL → 程序 MISS）
static const char *lp_nvn_stage_name(unsigned nvn) {
    switch (nvn) { case 0: return "vs"; case 1: return "ps"; case 2: return "gs"; case 5: return "cs"; default: return NULL; }
}
static const char *lp_v1_stage_name(const char *lib_stage) {
    return !strcmp(lib_stage, "ps") ? "fs" : lib_stage;   // v1 用 NVN 阶段名 fs；vs/gs/cs 同名
}
// 纹理号 → cb2 句柄表字偏移（v1 texture slot 的约定）：主表 0..31（字 8+2r）、与图像表同址的 32..39
//   （字 72+2(r−32)，iOS nvn_mtl.m C2_IMG / tex_slot_h 同一区间）、分离贴图表 128..255（字 0x5A+2(r−128)）。
//   其余号段（40..127、≥256）在 c2 里没有句柄字：返回 0 = 无映射（真实槽号从 8 起，0 不会被当成槽 0 写出）。
static uint32_t lp_tex_slot_word(uint32_t reg) {
    if (reg < 32) return 8 + reg * 2;
    if (reg < 40) return 72 + (reg - 32) * 2;
    if (reg >= 128 && reg < 256) return 0x5A + (reg - 128) * 2;
    return 0;
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

// 句柄放在游戏用户常量缓冲里的绑定（_b.json tex_ubo / tbuf_ubo / samp_ubo，iOS nvn_refl.h NVN_RB_TEXU / TBUFU / SAMPU）：
//   句柄字 = 用户常量槽 reg 的字节 off 处的 32 位字；sid = 这个字是采样器号（SAMPU，Ryujinx SeparateSamplerId，读出后左移 20）；
//   pair = 同一个 Ryujinx 句柄的另一半（nvn_refl.m pair_from_name 原样移植：从绑定名解出，自己这半的 reg/off 要与名字对上才配）。
struct lp_uword { uint32_t reg, off, pair_reg, pair_off; int sid, pair, pair_sid; };
static int lp_isword(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
// 名字形如 <stage>_<t|i>…_cb<slots>_<HEX>（正则 ^[a-z]+_[ti]\w*?_cb(\d+)_([0-9A-F]+)）：slots = 纹理缓冲 | (采样器缓冲 + 1) << 16，
//   HEX = 纹理字 | 采样器字 << 14 | 类型 << 28（字偏移；缓冲 c3..c16 = 用户槽 0..13）。只有类型 1 / 2 有第二个字
static void lp_pair_from_name(const char *name, int is_samp, struct lp_uword *w) {
    char nm[64]; size_t n = strlen(name);
    if (n >= sizeof nm) return;
    memcpy(nm, name, n + 1);
    if (n > 5 && !strcmp(nm + n - 5, "Smplr")) nm[n - 5] = 0;
    const char *p = nm;
    if (!(*p >= 'a' && *p <= 'z')) return;
    while (*p >= 'a' && *p <= 'z') p++;
    if (*p++ != '_' || (*p != 't' && *p != 'i')) return;
    p++;
    unsigned long long slots = 0, h = 0; int ok = 0;
    for (const char *q = p; *q && !ok; q++) {   // \w*? 懒匹配：第一个能接上 _cb<数字>_<大写十六进制> 的位置
        if (q[0] == '_' && q[1] == 'c' && q[2] == 'b' && q[3] >= '0' && q[3] <= '9') {
            const char *r = q + 3; unsigned long long sl = 0;
            while (*r >= '0' && *r <= '9') sl = sl * 10 + (unsigned)(*r++ - '0');
            if (r[0] == '_' && ((r[1] >= '0' && r[1] <= '9') || (r[1] >= 'A' && r[1] <= 'F'))) {
                unsigned long long hv = 0;
                for (r++; (*r >= '0' && *r <= '9') || (*r >= 'A' && *r <= 'F'); r++) hv = hv * 16 + (unsigned)(*r <= '9' ? *r - '0' : *r - 'A' + 10);
                slots = sl; h = hv; ok = 1; break;
            }
        }
        if (!lp_isword(*q)) return;
    }
    if (!ok) return;
    long long tb = (long long)(slots & 0xffff), sb = (slots >> 16) ? (long long)(slots >> 16) - 1 : tb;
    long long o0 = (long long)(h & 0x3fff) * 4, o1 = (long long)((h >> 14) & 0x3fff) * 4, ty = (long long)(h >> 28);
    if ((ty != 1 && ty != 2) || tb < 3 || tb > 16 || sb < 3 || sb > 16) return;
    if (is_samp) { if ((long long)w->reg != sb - 3 || (long long)w->off != o1) return; w->pair_reg = (uint32_t)(tb - 3); w->pair_off = (uint32_t)o0; w->pair_sid = 0; }
    else { if ((long long)w->reg != tb - 3 || (long long)w->off != o0) return; w->pair_reg = (uint32_t)(sb - 3); w->pair_off = (uint32_t)o1; w->pair_sid = ty == 2; }
    w->pair = 1;
}
// 读一条 *_ubo 成员的句柄字来源（reg / off / sid）并按名字配另一半
static int lp_uword_read(struct s2a_jv b, const char *msl, int is_samp, struct lp_uword *w) {
    memset(w, 0, sizeof *w);
    uint32_t sid = 0;
    if (jf_u32(b, "reg", &w->reg) || jf_u32(b, "off", &w->off) || w->reg >= 16 || w->off > 0xffff) return -1;
    if (is_samp && !jf_bool(b, "sid", &sid)) w->sid = sid != 0;
    lp_pair_from_name(msl, is_samp, w);
    return 0;
}
// v1：{"slot":r,"offset":o[,"sid":true][,"pair":{"slot":r2,"offset":o2,"sid":bool}]}
static void lp_uword_emit(struct jw *j, const struct lp_uword *w) {
    jw_fmt(j, "{\"slot\":%u,\"offset\":%u", w->reg, w->off);
    if (w->sid) jw_str(j, ",\"sid\":true");
    if (w->pair) jw_fmt(j, ",\"pair\":{\"slot\":%u,\"offset\":%u,\"sid\":%s}", w->pair_reg, w->pair_off, w->pair_sid ? "true" : "false");
    jw_str(j, "}");
}
// 同一个 bindings[] 里按 msl_name 找成员（tex_ubo 的采样器半边 = <名>Smplr）
static int lp_find_member(struct s2a_jv arr, const char *name, struct s2a_jv *out, int unique) {
    int n = s2a_jv_arr_count(arr);
    int found = 0;
    for (int i = 0; i < n; i++) {
        struct s2a_jv member; char msl[72] = "";
        if (s2a_jv_arr_item(arr, i, &member) || jf_str(member, "msl_name", msl, sizeof msl) || strcmp(msl, name)) continue;
        if (found) return 1;
        *out = member;
        if (!unique) return 0;
        found = 1;
    }
    return found ? 0 : -1;
}

// Dsh.cs DecodeTex 把类型和 cbuf/字偏移编码在反射名里；kind=samp 本身分不清类型 1 和 2。
static uint32_t lp_texture_handle(const char *name) {
    const char *field = strrchr(name, '_');
    if (!field) return 0;
    const char *hex = field + 1;
    size_t digits = strspn(hex, "0123456789ABCDEF");
    return digits && digits <= 8 && !hex[digits] ? (uint32_t)strtoul(hex, NULL, 16) : 0;
}

// 与 DirectGpuAccessor.TrySeparateC2 同口径：两个操作数字可以互换，来源必须各落在 c2 的两张表。
static int lp_separate_c2(const char *name, uint32_t handle, uint32_t tex_word, uint32_t sampler_slot) {
    if (handle >> 28 != 1 || sampler_slot >= 128) return 0;
    const char *end = strrchr(name, '_');
    if (!end) return 0;
    const char *start = end;
    while (start > name && start[-1] != '_') start--;
    if (end - start != 3 || memcmp(start, "tcb", 3)) {
        if (end - start < 3 || memcmp(start, "cb", 2) || start + 2 + strspn(start + 2, "0123456789") != end) return 0;
        unsigned long slots = strtoul(start + 2, NULL, 10);
        if (slots != 2 && slots != ((3u << 16) | 2u)) return 0;
    }
    const uint32_t a = handle & 0x3fff, b = (handle >> 14) & 0x3fff;
    const uint32_t sampler_word = 0x568 / 4 + 2 * sampler_slot;
    return (a == tex_word && b == sampler_word) || (b == tex_word && a == sampler_word);
}

// 只查已定位的描述符。类型声明在函数体之前，遇到 OpFunction 即可停。
static const uint32_t *lp_spv_decl(const uint32_t *w, size_t nw, unsigned opcode, unsigned id_word, uint32_t id) {
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || wc > nw - i || op == 54) break;
        if (op == opcode && wc > id_word && w[i + id_word] == id) return w + i;
        i += wc;
    }
    return NULL;
}

static int lp_spv_single_combined(const uint8_t *spv, size_t n, uint32_t binding) {
    const uint32_t *w = (const uint32_t *)spv;
    size_t nw = n / 4;
    uint32_t id = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || wc > nw - i) return 0;
        if (op == 54) break;   // OpFunction
        if (op == 71 && wc == 4 && w[i + 2] == 33 && w[i + 3] == binding) {   // OpDecorate Binding
            if (id || !w[i + 1]) return 0;
            id = w[i + 1];
        }
        i += wc;
    }
    if (!id) return 0;
    unsigned sets = 0, bindings = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || wc > nw - i) return 0;
        if (op == 54) break;
        if (op == 71 && wc >= 3 && w[i + 1] == id) {
            if (w[i + 2] == 34) {   // DescriptorSet
                if (wc != 4 || w[i + 3] != 0 || ++sets != 1) return 0;
            } else if (w[i + 2] == 33 && (wc != 4 || ++bindings != 1)) return 0;
        }
        i += wc;
    }
    const uint32_t *var = lp_spv_decl(w, nw, 59, 2, id);   // OpVariable UniformConstant
    if (sets != 1 || bindings != 1 || !var || (var[0] >> 16) != 4 || var[3] != 0) return 0;
    const uint32_t *ptr = lp_spv_decl(w, nw, 32, 1, var[1]);   // OpTypePointer UniformConstant
    if (!ptr || (ptr[0] >> 16) != 4 || ptr[2] != 0) return 0;
    const uint32_t *sampled = lp_spv_decl(w, nw, 27, 1, ptr[3]);   // 直接指向 OpTypeSampledImage，不是数组
    if (!sampled || (sampled[0] >> 16) != 3) return 0;
    const uint32_t *image = lp_spv_decl(w, nw, 25, 1, sampled[2]);   // OpTypeImage：取样的 1D/2D/3D/Cube
    return image && (image[0] >> 16) >= 9 && image[3] <= 3 && image[7] == 1;
}

enum { LP_STAGE_BINDINGS = 64 };
enum lp_binding_owner { LP_BINDING_ABSENT, LP_BINDING_GUEST, LP_BINDING_SUPPORT };
struct lp_emitted_binding { uint32_t number; enum lp_binding_owner owner; };
struct lp_emitted_bindings {
    struct lp_emitted_binding entry[LP_STAGE_BINDINGS];
    int count, uses_v2;
};
struct lp_spv_bindings {
    uint32_t number[LP_STAGE_BINDINGS];
    int count, overflow, support, support_is_ubo, uses_nonzero_set;
};
enum lp_window_mode { LP_WINDOW_LEGACY, LP_WINDOW_UNUSED, LP_WINDOW_FIXED, LP_WINDOW_MASK };
#define LP_WINDOW_UBO 0x100u   // mode 高位：含用户常量缓冲来源存储的计算程序（见 lp_has_ubo_storage）；低 8 位是 lp_window_mode
#define LP_WINDOW_KIND(mode) ((mode) & 0xffu)
struct lp_window { uint32_t mode, id; };
struct lp_stage_contract {
    struct lp_window window;
    struct lp_emitted_bindings emitted;
    struct lp_spv_bindings declared;
    char key[17], json_name[12], spv_name[12];
};

#define LP_NVN_STAGES 6   // NVN 阶段号 vs..cs = 0..5；s2a_libpack_program_v1 的 keys[6][8] 与逐阶段合同都按它定长
_Static_assert(LP_NVN_STAGES == 6, "keys[6][8] in s2a_libpack_program_v1 is indexed by NVN stage count");

static void lp_record_binding(struct lp_emitted_bindings *bindings, uint32_t number, enum lp_binding_owner owner) {
    if (bindings->count < LP_STAGE_BINDINGS)
        bindings->entry[bindings->count] = (struct lp_emitted_binding){number, owner};
    bindings->count++;
}

static void lp_emit_support(struct jw *binds, int *sep, uint32_t number, int compute) {
    jw_fmt(binds, "%s{\"kind\":\"support\",\"stage\":\"program\",\"set\":0,\"binding\":%u%s}",
           *sep ? "," : "", number, compute ? ",\"count\":1" : "");
    *sep = 1;
}

static int lp_spv_binding_is_set0_ubo(const uint8_t *spv, size_t n, uint32_t binding);

// 一个键的 _b.json bindings[] → content.bindings[]（追加进 jw；sep 管逗号）。
// stage_no = 本段 StageNo（绑定号换算用它），v1_stage = v1 阶段名标签
// compute = 1：计算结果合同（nvn_vk_program_parse_compute）比图形严——每条带 count、storage 带
//   cbSlot/cbOffset 出处、c0 驱动常量段（drvcb bank 0）挂 slot 0；图形另映射 bank 2（c2 句柄表，slot 2）。
static int lp_emit_bindings(struct jw *j, struct s2a_jv top, const char *lib_stage,
                            int stage_no, const char *v1_stage, int compute, const uint8_t *spv, size_t spv_size,
                            int *sep, struct lp_emitted_bindings *emitted, char *why, size_t why_size) {
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
        // 绑定号 = arg_id × 6 + 阶段号，要装进布局项 / 线格式的 16 位（nvn_gpu_layout_ent.binding、S2SB 行、结果解析都按 16 位）。arg_id 是翻译器输出里
        //   SPIRV-Cross 参数块的 [[id(N)]]，上限由着色器自己的资源数定（没有固定上限），所以超界要具名拒，不能回绕（32 位乘法）、也不能截成 16 位。
        if (arg_id > (UINT16_MAX - (uint32_t)stage_no) / 6)
            return lp_fail(why, why_size, "%s bindings[%u] arg_id %u: binding number exceeds 16 bits", lib_stage, i, arg_id);
        uint32_t binding = arg_id * 6 + (uint32_t)stage_no;
        // 采样器半边：组合纹理在 .spv 只留一个合并变量、吃 texture 成员的号（dsh_vk ②）——v1 不出行
        size_t ml = strlen(msl);
        int sampler_half = (ml > 5 && !strcmp(msl + ml - 5, "Smplr"));
        const char *v1kind = NULL; uint32_t slot = 0; int has_slot = 0;
        uint32_t sampler_slot = 0; int has_sampler_slot = 0;
        char provenance[320] = "";   // storage 三元出处（nvnSlot / uboSlot+uboOffset / uboPtr）；texture 的 uboHandle / sampler
        if (sampler_half) continue;
        if (!strcmp(kind, "tex") || !strcmp(kind, "tbuf") || !strcmp(kind, "tex_pool")) {
            uint32_t reg = 0; int has_reg = !jf_u32(b, "reg", &reg);
            v1kind = "texture";
            if (has_reg && strcmp(kind, "tex_pool")) { slot = lp_tex_slot_word(reg); has_slot = slot != 0; }
            // 只有经 c2 两张表读到类型 1 的来源才写 v2 samp_slot。仍是一条组合图像描述符，采样器半边不另出行。
            if (has_slot && reg >= 128 && !strcmp(kind, "tex")) {
                char sn[72]; struct s2a_jv sb; char sk[24] = "";
                uint32_t handle = lp_texture_handle(msl);
                snprintf(sn, sizeof sn, "%sSmplr", msl);
                int half = lp_find_member(arr, sn, &sb, 1);
                if (half > 0) return lp_fail(why, why_size, "%s %s: duplicate sampler half", lib_stage, msl);
                if (!half && !jf_str(sb, "kind", sk, sizeof sk) && !strcmp(sk, "samp") &&
                    !jf_u32(sb, "reg", &sampler_slot) && lp_separate_c2(msl, handle, slot, sampler_slot)) {
                    if (arg_id > (UINT32_MAX - (uint32_t)stage_no) / 6 || !lp_spv_single_combined(spv, spv_size, binding))
                        return lp_fail(why, why_size, "%s %s: expected a single combined image sampler at set 0 binding %u", lib_stage, msl, binding);
                    emitted->uses_v2 = has_sampler_slot = 1;
                } else if (handle >> 28 || (!half && strcmp(sk, "samp_tex"))) has_slot = 0;
            }
        } else if (!strcmp(kind, "tex_ubo") || !strcmp(kind, "tbuf_ubo")) {
            v1kind = "texture";
            // 句柄在游戏用户常量缓冲里（iOS NVN_RB_TEXU / TBUFU）：v1 带 uboHandle（句柄字来源 + 另一半），
            //   tex_ubo 再带组合采样的采样器来源 sampler（取自 <名>Smplr 半边：samp_ubo / samp_id / samp，同 Dsh.cs DecodeUboTex 的三种）。
            //   数组（array<…>，按 8k 逐项取句柄）还没接：照旧不挂来源，消费端如实拒
            char ty[96] = "";
            if (jf_str(b, "msl_type", ty, sizeof ty) || strncmp(ty, "array<", 6)) {
                struct lp_uword w;
                if (lp_uword_read(b, msl, 0, &w)) return lp_fail(why, why_size, "%s bindings[%u] %s: %s without reg 0..15 / off", lib_stage, i, msl, kind);
                struct jw u = {0};
                jw_str(&u, ",\"uboHandle\":"); lp_uword_emit(&u, &w);
                char sn[72]; struct s2a_jv sb;
                snprintf(sn, sizeof sn, "%sSmplr", msl);
                if (!strcmp(kind, "tex_ubo") && !lp_find_member(arr, sn, &sb, 0)) {
                    char sk[24] = ""; uint32_t sreg = 0;
                    if (jf_str(sb, "kind", sk, sizeof sk)) { free(u.b); return lp_fail(why, why_size, "%s bindings[%u] %s: sampler half without kind", lib_stage, i, msl); }
                    if (!strcmp(sk, "samp_ubo")) {
                        struct lp_uword sw;
                        if (lp_uword_read(sb, sn, 1, &sw)) { free(u.b); return lp_fail(why, why_size, "%s %s: samp_ubo without reg 0..15 / off", lib_stage, sn); }
                        jw_str(&u, ",\"sampler\":{\"source\":\"ubo\",\"handle\":"); lp_uword_emit(&u, &sw); jw_str(&u, "}");
                    } else if (!strcmp(sk, "samp_id") && !jf_u32(sb, "reg", &sreg) && sreg <= 0xffff) {
                        jw_fmt(&u, ",\"sampler\":{\"source\":\"id\",\"id\":%u}", sreg);
                    } else if (!strcmp(sk, "samp") && !jf_u32(sb, "reg", &sreg) && sreg < 128) {
                        jw_fmt(&u, ",\"sampler\":{\"source\":\"slot\",\"slot\":%u}", sreg);
                    } else { free(u.b); return lp_fail(why, why_size, "%s %s kind '%s' not mappable as a uniform-handle sampler", lib_stage, sn, sk); }
                }
                if (u.oom || !u.b || u.n >= sizeof provenance) { free(u.b); return lp_fail(why, why_size, "%s %s: handle source too long", lib_stage, msl); }
                memcpy(provenance, u.b, u.n + 1); free(u.b);
            }
        } else if (!strcmp(kind, "tex_c2a")) {
            v1kind = "texture";   // 驱动表句柄数组：v1 无对应字段，出绑定不挂槽（按缺席槽位如实）
        } else if (!strcmp(kind, "samp") || !strcmp(kind, "samp_id") || !strcmp(kind, "samp_ubo")
                   || !strcmp(kind, "samp_c2a") || !strcmp(kind, "samp_pool") || !strcmp(kind, "samp_tex")) {
            continue;   // 采样器半边/独立采样器句柄：v1 是合并纹理模型，不出行
        } else if (!strcmp(kind, "img") || !strcmp(kind, "img_ubo")) {
            uint32_t reg = 0;
            v1kind = "image";
            if (!strcmp(kind, "img") && !jf_u32(b, "reg", &reg) && reg < 8) { slot = lp_img_slot_word(reg); has_slot = 1; }   // NVN 图像槽只有 0..7（c2 图像表字 72..86）
        } else if (!strcmp(kind, "cbuf")) {
            uint32_t reg = 0;
            if (jf_u32(b, "reg", &reg)) return lp_fail(why, why_size, "%s bindings[%u] cbuf without reg", lib_stage, i);
            v1kind = "uniform";
            if (reg < 16) { slot = reg + 3; has_slot = 1; }   // v1：驱动槽号，消费端 ubo_slot = slot-3；reg≥16 出 NVN 槽域
        } else if (!strcmp(kind, "c1")) {
            v1kind = "uniform"; slot = 1; has_slot = 1;   // 着色器自带常量段 c[1]：v1 槽 1（消费端 NVN_RB_C1）
        } else if (!strcmp(kind, "ubolen") || !strcmp(kind, "uvx") || !strcmp(kind, "lb") || !strcmp(kind, "sbsize")) {
            continue;   // Metal 专用的宿主表（越界长度表 / 宽屏采样换算 / LOD 偏移 / 存储长度表）：Vulkan 版 .spv 不声明它们
                        //   （11-13 一款 Unity 游戏全库 11052 个着色器逐个核：这四种在 _b.json 里 21599 处、.spv 里 0 处），v1 不出行
        } else if (!strcmp(kind, "drvcb")) {
            v1kind = "uniform";   // 宿主常量：真实 UBO 描述符、无 guest 槽（消费端还没接，按无槽如实拒）
            // 计算：bank 是 NVN cbuf 号本身（Dsh.cs Reflect：fp_c<n>，n∉{1,3..16} 记 drvcb+bank）。c0 = 驱动常量段，
            //   v1 槽 0（现编结果同号，消费端 NVN_RB_DRV0）；c2 = 句柄表，槽 2 交给解析器如实拒；其余 bank 不挂槽
            uint32_t bank = 0;
            int has_bank = !jf_u32(b, "bank", &bank);
            if (compute && has_bank && (bank == 0 || bank == 2)) { slot = bank; has_slot = 1; }
            // 图形：映射 bank 0 与 bank 2。bank 0 = c0 驱动常量段（消费端 layout_ent slot 0 → NVN_RB_DRV0，rp_draw 按完整 0xA00 驱动段供数：
            //   各阶段存储缓冲表 + 0x610 / 0x644 等宿主字）；bank 2 = c2 驱动句柄表（着色器自己从表里读句柄，slot 2 → NVN_RB_DRV2，
            //   rp_draw 按完整 0xA00 句柄表供数：组合槽 / 图像 / 分离贴图 / 分离采样器，同现编结果同号）。出处（_b.json drvcb bank 0 / 2）之外
            //   还要 .spv 证实同一绑定号是 set 0 的 Uniform Block，对不上就具名拒、不出死绑定；其余 bank（3…）照旧无槽、落到下面的「无来源」拒。
            else if (!compute && has_bank && (bank == 0 || bank == 2)) {
                if (!lp_spv_binding_is_set0_ubo(spv, spv_size, binding))
                    return lp_fail(why, why_size, "%s bindings[%u] %s drvcb bank %u binding %u is not a set 0 Uniform Block in the SPIR-V",
                                   lib_stage, i, msl, bank, binding);
                slot = bank; has_slot = 1;
            }
        } else if (!strcmp(kind, "geometry") || !strcmp(kind, "sbuf") || !strcmp(kind, "sbuf_ubo")
                   || !strcmp(kind, "ubo_ptr") || !strcmp(kind, "gmem_map") || !strcmp(kind, "gmem_log") || !strcmp(kind, "sbuf_push")) {
            v1kind = "storage";
            uint32_t v = 0;
            // 计算另带 cbSlot/cbOffset（地址字在哪个 cbuf 的哪个字节）：都由 _b.json 的同一事实换出，不另造——
            //   sbuf reg = (cb_offset×4 − 0x310)/16（Dsh.cs SSBO("cs") = 0x310）→ c0 + 0x310 + reg×16；
            //   ubo_ptr reg = (off − 0x210)/16 → c0 + 0x210 + reg×16；sbuf_ubo reg = cb_slot − 3、off = cb_offset×4 → c(reg+3) + off
            uint32_t cb_slot = 0, cb_off = 0; int has_cb = 0;
            if (!strcmp(kind, "sbuf")) {
                if (!jf_u32(b, "reg", &v) && v < 16) { snprintf(provenance, sizeof provenance, ",\"nvnSlot\":%u", v); cb_off = 0x310 + v * 16; has_cb = 1; }
            } else if (!strcmp(kind, "sbuf_ubo")) {
                uint32_t off = 0;
                if (!jf_u32(b, "reg", &v) && !jf_u32(b, "off", &off) && v < 16) {
                    snprintf(provenance, sizeof provenance, ",\"uboSlot\":%u,\"uboOffset\":%u", v, off); cb_slot = v + 3; cb_off = off; has_cb = 1; }
            } else if (!strcmp(kind, "ubo_ptr")) {
                if (!jf_u32(b, "reg", &v) && v < 16) { snprintf(provenance, sizeof provenance, ",\"uboPtr\":%u", v); cb_off = 0x210 + v * 16; has_cb = 1; }
            }
            if (compute && has_cb) {
                char pv[sizeof provenance]; memcpy(pv, provenance, sizeof pv);
                int pn = snprintf(provenance, sizeof provenance, ",\"cbSlot\":%u,\"cbOffset\":%u%s", cb_slot, cb_off, pv);
                if (pn < 0 || (size_t)pn >= sizeof provenance) return lp_fail(why, why_size, "%s bindings[%u] provenance too long", lib_stage, i);
            }
            // gmem_map/gmem_log/geometry：宿主哨兵槽，无 guest 映射——storage_kind 留 0，消费端如实报不支持；
            //   sbuf_push（地址在 NVN2 推常量槽里，iOS SBUFQ）同样不挂来源（v1 没有推常量来源），与它出现前「sbuf 无槽号」的结果相同
        } else if (!strcmp(kind, "support")) {
            v1kind = "support";   // 程序级支撑缓冲，绑定号由本阶段换算
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
        // 消费端映射不了来源的绑定不出库：没有 guest 槽、又没有消费端认的出处字段时，解析后 slot = -1，
        //   图形按 binding_kind_*_slot_-1、计算按 guest slot 类如实拒收——拒收挪到出库这一步，语义相同、
        //   原因写进 why，不把死绑定写进结果。认得的来源：guest 槽；uboHandle 句柄（TEXU/TBUFU）；
        //   存储出处——图形只接 nvnSlot（layout_ent 只认 storage_kind 1），计算另接 uboPtr / uboSlot+uboOffset
        //   （compute_storage 三种映射；图形消费端还没接，出的照样是死绑定）。
        int is_support = !strcmp(v1kind, "support");
        int mappable = is_support || has_slot || strstr(provenance, "\"uboHandle\"")
            || strstr(provenance, "\"nvnSlot\"")
            || (compute && (strstr(provenance, "\"uboPtr\"") || strstr(provenance, "\"uboSlot\"")));
        if (!mappable)
            return lp_fail(why, why_size, "%s bindings[%u] %s kind '%s' has no consumable guest source", lib_stage, i, msl, kind);
        lp_record_binding(emitted, binding, is_support ? LP_BINDING_SUPPORT : LP_BINDING_GUEST);
        jw_fmt(j, "%s{\"kind\":\"%s\",\"stage\":\"%s\",\"set\":0,\"binding\":%u", *sep ? "," : "", v1kind, is_support ? "program" : v1_stage, binding);
        *sep = 1;
        if (compute) jw_str(j, ",\"count\":1");   // 计算合同必填；.spv 里每个成员是单个描述符（数组形的 tex_c2a 等无槽，解析器另拒）
        if (has_slot) jw_fmt(j, ",\"slot\":%u", slot);
        if (has_sampler_slot) jw_fmt(j, ",\"samp_slot\":%u", sampler_slot);
        if (provenance[0]) jw_str(j, provenance);
        if (!strcmp(v1kind, "storage")) jw_fmt(j, ",\"rw\":%s", rw ? "true" : "false");
        jw_str(j, "}");
    }
    return 0;
}

// 变量 id 是 set 0 的 Uniform Block（Uniform 存储类、直接指向 Block 结构体、恰一个 DescriptorSet 0）。
//   支撑缓冲（Dsh.cs 以原名 support_buffer 标记，v2 要求它是这种变量）与图形 c0 驱动常量段共用这一判据。
static int lp_spv_var_is_set0_ubo(const uint32_t *w, size_t nw, uint32_t id) {
    const uint32_t *var = lp_spv_decl(w, nw, 59, 2, id);   // OpVariable，StorageClass Uniform = 2
    if (!var || (var[0] >> 16) != 4 || var[3] != 2) return 0;
    const uint32_t *ptr = lp_spv_decl(w, nw, 32, 1, var[1]);   // OpTypePointer Uniform
    if (!ptr || (ptr[0] >> 16) != 4 || ptr[2] != 2) return 0;
    const uint32_t *block = lp_spv_decl(w, nw, 30, 1, ptr[3]);   // OpTypeStruct（直接指向块，不是数组）
    if (!block) return 0;
    unsigned blocks = 0, sets = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || wc > nw - i) return 0;
        if (op == 54) break;   // OpFunction
        if (op == 71 && wc >= 3) {   // OpDecorate
            if (w[i + 1] == block[1] && w[i + 2] == 2 && wc == 3) blocks++;   // Block
            if (w[i + 1] == id && w[i + 2] == 34) {   // DescriptorSet
                if (wc != 4 || w[i + 3] != 0) return 0;
                sets++;
            }
        }
        i += wc;
    }
    return blocks == 1 && sets == 1;
}

// 绑定号 binding 在 .spv 里恰有一个变量带 Binding 装饰，且它是 set 0 的 Uniform Block（lp_spv_var_is_set0_ubo）。
//   0 = 没有 / 不止一个 / 类型不对（StorageBuffer、无 Block、数组、非 set 0 都在内）。
static int lp_spv_binding_is_set0_ubo(const uint8_t *spv, size_t n, uint32_t binding) {
    const uint32_t *w = (const uint32_t *)spv; size_t nw = n / 4;
    uint32_t id = 0; unsigned found = 0;
    if (nw < 5) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || wc > nw - i) return 0;
        if (op == 54) break;   // OpFunction：装饰都在函数体之前
        if (op == 71 && wc >= 4 && w[i + 2] == 33 && w[i + 3] == binding) { id = w[i + 1]; found++; }   // OpDecorate <id> Binding
        i += wc;
    }
    return found == 1 && lp_spv_var_is_set0_ubo(w, nw, id);
}

// 一个阶段：取键的 _b.json + _b.spv → 校验 → 往 jw 写 stage 项与 bindings 项。
// 返回 0 成功 / 1 miss / -1 reject
// .spv 实际声明的描述符绑定（OpDecorate Binding）与名为 support_buffer 的那一个。_b.json 是 Metal 反射，
//   支撑缓冲在 Metal 里另有传法、_b.json 常常不列它；而 Vulkan 版 .spv 照样声明（参数号 8 处，绑定 = 8×6 + 阶段号）。
//   只按 _b.json 出 v1 的话描述符布局里没有它，着色器读到的倍率 / 标志位是未定义值，整屏画黑（11-14 真机：一款大型
//   开放世界游戏只放行图形预编即黑、只放行计算即正常；该款库里一对 vs / fs 的 .spv 各多一个 support_buffer 绑定 48 / 52）。
//   返回 0 = 解析成功。超出收集容量仍记下 overflow，留给整份程序选择 v2 时拒收。
static int lp_spv_read_bindings(const uint8_t *spv, size_t n, struct lp_spv_bindings *out) {
    const uint32_t *w = (const uint32_t *)spv; size_t nw = n / 4;
    uint32_t sid = 0;
    int ambiguous_support = 0;
    out->support = -1;
    if (nw < 5) return -1;
    for (size_t pass = 0; pass < 2; pass++)   // 第一遍找名字（OpName 在 OpDecorate 之前，但不依赖顺序）
        for (size_t i = 5; i < nw; ) {
            uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
            if (!wc || i + wc > nw) return -1;
            if (pass == 0 && op == 5 && wc >= 6 &&
                !memcmp(&w[i + 2], "support_buffer", sizeof "support_buffer")) {   // OpName
                if (sid && sid != w[i + 1]) ambiguous_support = 1;
                sid = w[i + 1];
            }
            if (pass == 1 && op == 71 && wc >= 4 && w[i + 2] == 33) {   // OpDecorate <id> Binding <n>
                if (out->count < LP_STAGE_BINDINGS) out->number[out->count++] = w[i + 3];
                else out->overflow = 1;
                if (sid && w[i + 1] == sid) out->support = (int)w[i + 3];
            }
            if (pass == 1 && op == 71 && wc >= 4 && w[i + 2] == 34 && w[i + 3])   // DescriptorSet
                out->uses_nonzero_set = 1;
            i += wc;
        }
    if (out->support >= 0 && !ambiguous_support)
        out->support_is_ubo = lp_spv_var_is_set0_ubo(w, nw, sid);
    return 0;
}
// 片元输出映射（同现编结果 info.fragmentOutputMap：每个颜色目标 4 位，第 c 分量 = 位 4×location + c）：按 .spv 的 Output 变量推。
//   缺了这一项消费端按全 1（不屏蔽）处理，着色器没写的颜色目标也会被写入未定义值（延迟渲染的 G 缓冲被冲掉，王国之泪整屏发暗）。
//   0 = 推出；-1 = 读不懂（数组输出、location ≥ 8、没 Location 的非内建输出等），调用方不出这一项
int s2a_libpack_spv_frag_outputs(const uint8_t *spv, size_t n, uint32_t *map) {
    const uint32_t *w = (const uint32_t *)spv; size_t nw = n / 4;
    if (nw < 5 || !w[3] || w[3] > (1u << 22)) return -1;
    uint32_t bound = w[3];
    // 每个 id：低 8 位 location+1（0 = 没有）、8..15 位 component、位 16 = 内建；类型表：分量数（0 = 不是可用的标量 / 向量）
    uint32_t *dec = calloc(bound, sizeof *dec), *ncomp = calloc(bound, sizeof *ncomp), *ptr_to = calloc(bound, sizeof *ptr_to);
    int rc = -1; uint32_t m = 0;
    if (!dec || !ncomp || !ptr_to) goto out;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) goto out;
        if (op == 71 && wc >= 3 && w[i + 1] < bound) {                       // OpDecorate
            uint32_t id = w[i + 1], d = w[i + 2];
            if (d == 30 && wc >= 4) { if (w[i + 3] >= 255) goto out; dec[id] = (dec[id] & ~0xffu) | (w[i + 3] + 1); }   // Location
            else if (d == 31 && wc >= 4) { if (w[i + 3] > 3) goto out; dec[id] = (dec[id] & ~0xff00u) | (w[i + 3] << 8); }   // Component
            else if (d == 11) dec[id] |= 1u << 16;                                // BuiltIn
        } else if ((op == 21 || op == 22) && wc >= 2 && w[i + 1] < bound) ncomp[w[i + 1]] = 1;   // OpTypeInt / OpTypeFloat
        else if (op == 23 && wc >= 4 && w[i + 1] < bound && w[i + 2] < bound && ncomp[w[i + 2]] == 1 && w[i + 3] <= 4) ncomp[w[i + 1]] = w[i + 3];   // OpTypeVector
        else if (op == 32 && wc >= 4 && w[i + 1] < bound) ptr_to[w[i + 1]] = w[i + 2] == 3 ? w[i + 3] + 1 : 0;   // OpTypePointer Output → 指向类型 +1
        else if (op == 59 && wc >= 4 && w[i + 3] == 3) {                     // OpVariable … Output
            uint32_t ty = w[i + 1], id = w[i + 2];
            if (ty >= bound || id >= bound || !ptr_to[ty]) goto out;
            if (dec[id] >> 16 & 1) { i += wc; continue; }                        // 内建（FragDepth / SampleMask）不占颜色目标
            uint32_t loc = dec[id] & 0xff, comp = dec[id] >> 8 & 0xff, val = ptr_to[ty] - 1, k = val < bound ? ncomp[val] : 0;
            if (!loc || loc - 1 >= 8 || !k || comp + k > 4) goto out;
            m |= ((1u << k) - 1u) << (4 * (loc - 1) + comp);
        }
        i += wc;
    }
    *map = m; rc = 0;
out:
    free(dec); free(ncomp); free(ptr_to);
    return rc;
}
// 能力核：离线库的 .spv 是批翻链（translator/dmsh_xlate）按它自己的宿主口径翻的，不是按本机 profile——那边
//   QueryHostSupportsShaderFloat64 缺省答「有」（Metal 只在模块真用到 double 时重翻），所以几乎每个模块都带 OpCapability Float64。
//   本后端建设备时从不启用 shaderFloat64 / shaderInt64（direct/vk_device.c enabled_features），模块声明这两项能力就违反
//   VUID-VkShaderModuleCreateInfo-pCode-08740；Maleoon 920 的转换器直接拒（「SPV01: Unsupported capability (10)」，
//   vkCreateComputePipelines 回 VK_ERROR_OUT_OF_HOST_MEMORY，11-20 真机 13 个计算程序全是出包预编命中的那 13 个）。
//   模块里没有对应宽度的类型（OpTypeFloat 64 / OpTypeInt 64）时这条能力声明用不上，原地删掉（只删那一条 OpCapability，
//   id 上界不变，语义不变）；真用到了就拒收，由调用方退回现编（现编按本机 profile 翻，Float64 关时 double 降成 float）。
//   Int64Atomics 必带 64 位整型，一律拒收。返回 0 = 可用（*n 可能变小）；-1 = 拒收（why 以具名原因开头：
//   fp64_type_used / int64_type_used / int64_atomics_used，调用方照常走库拒收计数 s2a_libpack_rejects）。
//   判据以类型声明为准：OpTypeFloat 64 只被 OpConstant 引用、没有运算也算用到（模块仍按 Float64 校验）。
//   真删了能力声明的模块按条计数（libpack_caps_stripped，s2a_libpack_caps_stripped）；同一模块再核一遍不再计。
// 片元像素锁（SPV_EXT_fragment_shader_interlock）：批翻链的翻译器对「宿主支持片元互锁」缺省答「有」，于是几乎每个模块都声明
//   FragmentShader{Pixel,Sample,ShadingRate}InterlockEXT 能力和扩展，却没有一条 OpBegin/EndInvocationInterlockEXT（Mali-G710 上 94.6% 的库
//   因此建模块失败、整库退回现编）。本后端建设备时从不启用这个扩展，所以三样声明（OpCapability / OpExtension / 互锁执行模式）一律删掉。
//   模块里真有互锁临界区（Begin / End 指令）时：照常画、不带互锁（片元顺序可能不保），两条指令删掉，不拒收也不静默——按模块计数
//   （libpack_interlock_dropped，s2a_libpack_interlock_dropped），调用方每局首次打一条具名红行。这两条指令没有结果 id，删掉不影响别的指令。
static int lp_is_interlock_ext(const uint32_t *w, uint32_t wc) {   // OpExtension "SPV_EXT_fragment_shader_interlock"
    static const char name[] = "SPV_EXT_fragment_shader_interlock";
    const size_t bytes = (size_t)(wc - 1) * 4;
    return bytes > sizeof name - 1 && !memcmp(&w[1], name, sizeof name);   // 含结尾 0
}
int s2a_libpack_spv_admit_caps(uint8_t *spv, size_t *n, char *why, size_t why_size) {
    uint32_t *w = (uint32_t *)spv; size_t nw = *n / 4;
    int f64 = 0, i64 = 0, ilk = 0;
    if (nw < 5 || w[0] != 0x07230203u) { lp_fail(why, why_size, "not a SPIR-V module"); return -1; }
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) { lp_fail(why, why_size, "truncated instruction at word %zu", i); return -1; }
        if (op == 22 && wc >= 3 && w[i + 2] == 64) f64 = 1;          // OpTypeFloat 64
        if (op == 21 && wc >= 4 && w[i + 2] == 64) i64 = 1;          // OpTypeInt 64
        if ((op == 5364 || op == 5365) && wc == 1) ilk = 1;           // OpBegin / OpEndInvocationInterlockEXT
        i += wc;
    }
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (op == 17 && wc == 2) {                                    // OpCapability
            const uint32_t cap = w[i + 1];
            if (cap == 12 || (cap == 10 && f64) || (cap == 11 && i64)) {
                lp_fail(why, why_size, "%s: uses %s, which the Vulkan device does not enable",
                        cap == 10 ? "fp64_type_used" : cap == 11 ? "int64_type_used" : "int64_atomics_used",
                        cap == 10 ? "Float64" : cap == 11 ? "Int64" : "Int64Atomics");
                return -1;
            }
            if (cap == 10 || cap == 11 || cap == 5363 || cap == 5372 || cap == 5378) {   // 5363 / 5372 / 5378：Sample / ShadingRate / Pixel 互锁
                memmove(&w[i], &w[i + 2], (nw - i - 2) * sizeof *w);
                nw -= 2;
                continue;
            }
        }
        const int drop = (op == 10 && wc >= 2 && lp_is_interlock_ext(&w[i], wc))                     // OpExtension
                      || (op == 16 && wc >= 3 && w[i + 2] >= 5366 && w[i + 2] <= 5371)            // OpExecutionMode 互锁顺序 5366..5371
                      || ((op == 5364 || op == 5365) && wc == 1);                                 // 互锁临界区的两条指令
        if (drop) { memmove(&w[i], &w[i + wc], (nw - i - wc) * sizeof *w); nw -= wc; continue; }
        i += wc;
    }
    if (ilk) atomic_fetch_add(&g_interlock_dropped, 1);
    if (nw * 4 != *n) atomic_fetch_add(&g_caps_stripped, 1);
    *n = nw * 4;
    return 0;
}
// _b.json 里某个参数号的绑定是不是 v1 有意不出行的种类（Metal 专用宿主表 / 采样器半边）
static int lp_json_has_binding(struct s2a_jv top, int stage_no, uint32_t binding, int *skipped_kind) {
    struct s2a_jv arr; *skipped_kind = 0;
    if (s2a_jv_obj_find(top, "bindings", &arr) || !s2a_jv_is_array(arr)) return 0;
    int n = s2a_jv_arr_count(arr);
    for (int i = 0; i < n; i++) {
        struct s2a_jv b; uint32_t arg = 0; char kind[24] = "";
        if (s2a_jv_arr_item(arr, i, &b) || jf_u32(b, "arg_id", &arg) || arg * 6 + (uint32_t)stage_no != binding) continue;
        if (!jf_str(b, "kind", kind, sizeof kind) && (!strcmp(kind, "ubolen") || !strcmp(kind, "uvx") || !strcmp(kind, "lb") || !strcmp(kind, "sbsize")))
            { *skipped_kind = 1; continue; }
        return 1;
    }
    return 0;
}

// 任一段使用 v2 字段时，整个程序的每段都要满足描述符合同。反射里未发出的行不能充当布局条目。
static int lp_check_v2_bindings(const struct lp_stage_contract *stage, int *implicit_support,
                                char *why, size_t why_size) {
    const struct lp_emitted_bindings *emitted = &stage->emitted;
    const struct lp_spv_bindings *declared = &stage->declared;
    if (emitted->count > LP_STAGE_BINDINGS || declared->overflow)
        return lp_fail(why, why_size, "%s %s has too many descriptor bindings", stage->key, stage->spv_name);
    if (declared->uses_nonzero_set)
        return lp_fail(why, why_size, "%s %s declares a descriptor outside set 0", stage->key, stage->spv_name);
    if (declared->support >= 0 && !declared->support_is_ubo)
        return lp_fail(why, why_size, "%s %s support_buffer is not a set 0 uniform block",
                       stage->key, stage->spv_name);
    // 仅有 UBO 类型无法证明用途。反射的显式支撑缓冲须匹配 Dsh 命名的 SPIR-V 变量。
    for (int i = 0; i < emitted->count; i++) {
        const struct lp_emitted_binding *entry = &emitted->entry[i];
        if (entry->owner == LP_BINDING_SUPPORT &&
            (declared->support < 0 || entry->number != (uint32_t)declared->support))
            return lp_fail(why, why_size, "%s %s binding %u is not declared as support_buffer",
                           stage->key, stage->spv_name, entry->number);
    }
    int add_support = 0, support_declarations = 0;
    for (int i = 0; i < declared->count; i++) {
        uint32_t binding = declared->number[i];
        enum lp_binding_owner owner = LP_BINDING_ABSENT;
        for (int j = 0; j < emitted->count; j++) {
            if (emitted->entry[j].number != binding) continue;
            if (owner != LP_BINDING_ABSENT)
                return lp_fail(why, why_size, "%s %s binding %u has multiple emitted descriptors",
                               stage->key, stage->spv_name, binding);
            owner = emitted->entry[j].owner;
        }
        if (declared->support >= 0 && binding == (uint32_t)declared->support) {
            if (++support_declarations > 1)
                return lp_fail(why, why_size, "%s %s binding %u has multiple SPIR-V declarations for support_buffer",
                               stage->key, stage->spv_name, binding);
            if (owner == LP_BINDING_GUEST)
                return lp_fail(why, why_size, "%s %s binding %u is owned by support_buffer",
                               stage->key, stage->spv_name, binding);
            add_support = owner == LP_BINDING_ABSENT;
        } else if (owner == LP_BINDING_ABSENT) {
            return lp_fail(why, why_size, "%s %s binding %u has no mappable %s binding",
                           stage->key, stage->spv_name, binding, stage->json_name);
        }
    }
    *implicit_support = add_support ? declared->support : -1;
    return 0;
}

static int lp_finish_v2_bindings(const struct lp_stage_contract *stage, struct jw *binds, int *sep, int compute,
                                 char *why, size_t why_size) {
    int implicit_support = -1;
    int rc = lp_check_v2_bindings(stage, &implicit_support, why, why_size);
    if (!rc && implicit_support >= 0)
        lp_emit_support(binds, sep, (uint32_t)implicit_support, compute);
    return rc;
}
// 几何程序里任一阶段写视口号 / 视口掩码（OpDecorate / OpMemberDecorate BuiltIn ViewportIndex 10、ViewportMaskNV 5253、
//   SecondaryViewportMaskNV 5258、ViewportMaskPerViewNV 5262）：回放层没有视口数组，现编（DirectShaderCompiler ValidateSpirv
//   rejectViewportArray）对带几何的程序整支拒，这里同口径。返回 1 = 有、0 = 没有、-1 = 读不懂
static int lp_spv_viewport_builtin(const uint8_t *spv, size_t n) {
    const uint32_t *w = (const uint32_t *)spv; size_t nw = n / 4;
    if (nw < 5) return -1;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) return -1;
        uint32_t b = op == 71 && wc >= 4 && w[i + 2] == 11 ? w[i + 3] : op == 72 && wc >= 5 && w[i + 3] == 11 ? w[i + 4] : 0;
        if (b == 10 || b == 5253 || b == 5258 || b == 5262) return 1;
        i += wc;
    }
    return 0;
}
// 支撑缓冲长度：离线库的翻译器回答「宿主有贴图尺寸表」（ABI 2，translator/dmsh_xlate Accessor），程序的支撑块声明到 3776 字节，
//   偏移 2608 起是 73 项 × 16 字节的尺寸表。安卓宿主不填这张表：按 3776 分配、回放核心整块清零，表项全 0 时着色器走旧路径
//   （倍率恒 1，与机上现编同口径）。只按 2608 绑的话越界读到的是未定义值（设备没开 robustBufferAccess）
#define LP_SUPPORT_ABI2_BYTES 3776u

// 只固化翻译器单独分配、并在阶段合同中指出的掩码常量。
static int lp_fix_ssbo_window_mask(uint8_t *spv, size_t n, uint32_t id, uint32_t alignment) {
    static const char name[] = "s2a_storage_window_mask";
    uint32_t *w = (uint32_t *)spv;
    const size_t nw = n / sizeof(*w);
    if (nw < 5 || !id || id >= w[3]) return -1;
    size_t constant = 0;
    unsigned names = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || wc > nw - i) return -1;
        if (op == 5 && wc >= 3 && w[i + 1] == id &&
            (wc - 2) * sizeof(*w) >= sizeof(name) && !memcmp(w + i + 2, name, sizeof(name)))
            names++;
        if (op == 50 && wc == 4 && w[i + 2] == id) {   // OpSpecConstant
            if (constant || w[i + 3] != UINT32_MAX) return -1;
            constant = i;
        }
        if (op == 71 && wc >= 3 && w[i + 1] == id && w[i + 2] == 1) return -1;  // SpecId
        i += wc;
    }
    if (!constant || names != 1) return -1;
    for (size_t i = 5; i < constant; i += w[i] >> 16) {
        if (w[i] != ((4u << 16) | 21u) || w[i + 1] != w[constant + 1]) continue;   // OpTypeInt
        if (w[i + 2] != 32 || w[i + 3] != 0) return -1;
        w[constant] = (4u << 16) | 43u;   // OpConstant
        w[constant + 3] = 0u - alignment;
        return 0;
    }
    return -1;
}

// 带用户常量缓冲来源存储（sbuf_ubo / ubo_ptr）的计算程序：基址在运行时从常量缓冲读，对齐与否编译期和取库时都不知道，回放对这类来源不挪窗口。
//   非对齐时沿用旧库的对齐门、不放行：这一位随窗口 mode 存进 S2SB 记录（高位，低 8 位仍是 lp_window_mode）。
static int lp_has_ubo_storage(struct s2a_jv top) {
    struct s2a_jv bindings;
    if (s2a_jv_obj_find(top, "bindings", &bindings)) return 0;
    const int count = s2a_jv_arr_count(bindings);
    for (int i = 0; i < count; i++) {
        struct s2a_jv binding;
        char kind[24];
        if (!s2a_jv_arr_item(bindings, i, &binding) && !jf_str(binding, "kind", kind, sizeof kind) &&
            (!strcmp(kind, "sbuf_ubo") || !strcmp(kind, "ubo_ptr")))
            return 1;
    }
    return 0;
}

static int lp_window_parse(struct s2a_jv top, struct lp_stage_contract *contract, int compute, char *why, size_t cap) {
    struct s2a_jv field;
    int has_mask = !s2a_jv_obj_find(top, "ssbo_window_mask", &field);
    if (s2a_jv_obj_find(top, "ssbo_window_abi", &field)) {
        if (has_mask) return lp_fail(why, cap, "%s mask without SSBO window ABI", contract->key);
        return 0;
    }
    uint32_t abi;
    if (jf_u32(top, "ssbo_window_abi", &abi) || abi != 1)
        return lp_fail(why, cap, "%s unknown SSBO window ABI", contract->key);
    const uint32_t ubo = compute && lp_has_ubo_storage(top) ? LP_WINDOW_UBO : 0;
    if (has_mask) {
        contract->window.mode = LP_WINDOW_MASK | ubo;
        if (jf_u32(top, "ssbo_window_mask", &contract->window.id) || !contract->window.id)
            return lp_fail(why, cap, "%s invalid SSBO window mask", contract->key);
        return 0;
    }
    contract->window.mode = LP_WINDOW_UNUSED | ubo;
    struct s2a_jv bindings;
    if (!s2a_jv_obj_find(top, "bindings", &bindings)) {
        int count = s2a_jv_arr_count(bindings);
        for (int i = 0; i < count; i++) {
            struct s2a_jv binding;
            char kind[24];
            if (!s2a_jv_arr_item(bindings, i, &binding) &&
                !jf_str(binding, "kind", kind, sizeof kind) && !strcmp(kind, "sbuf")) {
                contract->window.mode = LP_WINDOW_FIXED | ubo;
                break;
            }
        }
    }
    return 0;
}

// 片元模块的贴图声明类别（见 s2a_libpack_gfx.window_tex_classes）：1 阴影（深度 + 数组的 OpTypeImage，或任一带 Dref 的采样 / 取样）、2 = 3D、4 = Cube、8 = 都不是；
//   不是片元入口的模块返回 0（不受限）。
static uint32_t lp_spv_tex_classes(const uint8_t *spv, size_t n) {
    const uint32_t *w = (const uint32_t *)spv;
    const size_t nw = n / sizeof(*w);
    uint32_t classes = 0;
    int fragment = 0;
    for (size_t i = 5; i < nw; ) {
        const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || wc > nw - i) break;
        if (op == 15 && wc >= 2 && w[i + 1] == 4) fragment = 1;   // OpEntryPoint Fragment
        else if (op == 25 && wc >= 9) {   // OpTypeImage：dim = w[3]（0 1D / 1 2D / 2 3D / 3 Cube）、depth = w[4]、arrayed = w[5]
            if (w[i + 4] == 1 && w[i + 5] == 1) classes |= 1u;
            if (w[i + 3] == 2) classes |= 2u;
            if (w[i + 3] == 3) classes |= 4u;
        } else if ((op >= 89 && op <= 94) || op == 97) classes |= 1u;   // OpImageSampleDref* / OpImageSampleProjDref* / OpImageDrefGather
        i += wc;
    }
    return fragment ? (classes ? classes : 8u) : 0u;
}

static int lp_window_apply(struct lp_window window, const struct s2a_libpack_gfx *gfx,
                           uint8_t *spv, size_t n, char *why, size_t cap) {
    const uint32_t kind = LP_WINDOW_KIND(window.mode);
    if ((window.mode & ~(0xffu | LP_WINDOW_UBO)) || kind > LP_WINDOW_MASK || (kind == LP_WINDOW_MASK) != (window.id != 0))
        return lp_fail(why, cap, "invalid SSBO window mode or mask");
    if (gfx && gfx->require_ssbo_window && gfx->window_tex_classes) {
        const uint32_t classes = lp_spv_tex_classes(spv, n);
        if (classes && !(classes & gfx->window_tex_classes)) {
            if (why && cap) snprintf(why, cap, "fragment texture classes outside the diagnostic window class set");
            return S2A_LIBPACK_MISS;
        }
    }
    if ((window.mode & LP_WINDOW_UBO) && gfx && gfx->require_ssbo_window)
        return lp_fail(why, cap, "UBO-sourced storage keeps the aligned gate");
    if (kind != LP_WINDOW_MASK) {
        if (!gfx || !gfx->require_ssbo_window) return 0;
        if (kind == LP_WINDOW_FIXED) return lp_fail(why, cap, "SBUFP uses a fixed SSBO window");
        if (kind == LP_WINDOW_LEGACY) {
            if (why && cap) snprintf(why, cap, "stage has no SSBO window variant");
            return S2A_LIBPACK_MISS;
        }
        return 0;
    }
    uint32_t alignment = gfx ? gfx->storage_alignment : 0, profile_alignment;
    struct s2a_jv caps;
    if (!alignment || (alignment & (alignment - 1)) || !gfx->profile_json ||
        s2a_jv_obj_find((struct s2a_jv){gfx->profile_json, gfx->profile_bytes}, "capabilities", &caps) ||
        jf_u32(caps, "storageBufferOffsetAlignment", &profile_alignment) || profile_alignment != alignment)
        return lp_fail(why, cap, "device/profile SSBO alignments differ");
    if (lp_fix_ssbo_window_mask(spv, n, window.id, alignment))
        return lp_fail(why, cap, "invalid SSBO window mask");
    if (gfx && gfx->window_used && gfx->require_ssbo_window) *gfx->window_used = 1;
    return 0;
}

// gs_topology：几何程序的输入图元（InputTopology 编号，见 lp_input_topology），几何段按它取 _vg<t>；不是几何程序给 -1。
// 图形阶段的 c0 读集：_b.json 的 drv_reads（按 MSL 文本的 vec4 粒度，库收录的程序读的都落在宿主能填的范围里）推出 c0Reads，
//   回放据此判断「这个阶段不读 c0 + 0x644（base instance）」，间接绘制才可以省掉每笔的 GPU 拷贝 + 屏障。
//   证得出才发：drv_reads 缺 / 不是对象 / bank 0 的 dynamic 非 false / vec4_offsets 不是 u32 数组 → 什么都不发（字段缺 = 未知，回放照旧拷）；
//   没有 bank 0 = 一处 c0 都不读 → 空数组；有静态下标 → 每个 vec4 一对 [偏移, 16]（0x640 的 vec4 盖住 0x644，读就是读）。
//   c0ReadsStatic 同值（不依赖动态下标定界的口径：这里本来就只在没有动态下标时才发）。
static void lp_emit_c0_reads(struct jw *stages, struct s2a_jv top) {
    struct s2a_jv drv, bank0, offsets, dyn;
    if (s2a_jv_obj_find(top, "drv_reads", &drv) || !s2a_jv_is_object(drv)) return;
    if (s2a_jv_obj_find(drv, "0", &bank0)) { jw_str(stages, ",\"c0Reads\":[],\"c0ReadsStatic\":[]"); return; }
    int dynamic = 1;
    if (!s2a_jv_is_object(bank0) || s2a_jv_obj_find(bank0, "dynamic", &dyn) || s2a_jv_bool(dyn, &dynamic) || dynamic) return;
    if (s2a_jv_obj_find(bank0, "vec4_offsets", &offsets) || !s2a_jv_is_array(offsets)) return;
    int n = s2a_jv_arr_count(offsets);
    if (n < 0) return;
    for (int i = 0; i < n; i++) {   // 先整体核：只要有一项不是 u32 就整段不发
        struct s2a_jv item; uint32_t o = 0;
        if (s2a_jv_arr_item(offsets, i, &item) || !item.n || item.p[0] < '0' || item.p[0] > '9' || s2a_jv_u32(item, &o)) return;   // strtoul 收 -16 / +16 / 前导空白：只认纯十进制数字开头
    }
    for (int k = 0; k < 2; k++) {
        jw_str(stages, k ? ",\"c0ReadsStatic\":[" : ",\"c0Reads\":[");
        for (int i = 0; i < n; i++) {
            struct s2a_jv item; uint32_t o = 0;
            (void)s2a_jv_arr_item(offsets, i, &item); (void)s2a_jv_u32(item, &o);
            jw_fmt(stages, "%s[%u,16]", i ? "," : "", o);
        }
        jw_str(stages, "]");
    }
}

static int lp_stage_from_bytes(struct s2a_libpack *lp, const uint8_t key[8], unsigned nvn, int compute, int gs_topology,
                         uint32_t shared_limit, const struct s2a_libpack_gfx *gfx, int packing, const uint8_t *jb, size_t jn, uint8_t *spv, size_t sn,
                         struct jw *stages, struct jw *binds, int *bind_sep, uint32_t *stage_mask,
                         uint32_t *support_bytes, int *uses_v2, struct lp_stage_contract *contract,
                         char *why, size_t why_size) {
    const char *lib_stage = lp_nvn_stage_name(nvn);
    if (!lib_stage) return 1;   // tcs/tes：vk 库不出此阶段，整个程序按 miss
    int gs = !strcmp(lib_stage, "gs");
    if (gs && (gs_topology < 0 || gs_topology > 4)) return 1;
    char *khex = contract->key;
    for (int i = 0; i < 8; i++) snprintf(khex + 2 * i, 3, "%02x", key[i]);
    // 几何段：每种输入图元一份（DshVk.CompileGeometry），_vg<t>.json 种类 112+t / _vg<t>.spv 种类 120+t
    const char *jname = contract->json_name, *sname = contract->spv_name;
    snprintf(contract->json_name, sizeof contract->json_name, gs ? "_vg%d.json" : "_b.json", gs_topology);
    snprintf(contract->spv_name, sizeof contract->spv_name, gs ? "_vg%d.spv" : "_b.spv", gs_topology);
    struct s2a_jv top = { (const char *)jb, jn };
    int out = -1;
    {   // 全文档语法核 + 顶层必须恰是一个对象（jv 不具现字符串，长串字段只吃跨度）
        const char *jend = NULL;
        if (!s2a_jv_is_object(top) || s2a_jv_scan((const char *)jb, (const char *)jb + jn, &jend)
            || jend != (const char *)jb + jn) {
            lp_fail(why, why_size, "%s %s: not a complete JSON object", khex, jname); goto done;
        }
    }
    {   // 命中判据：dm_ctl == 键 hex、stage 相符、_b.spv 是合法 SPIR-V
        char ctl[20] = "", st[8] = "";
        if (jf_str(top, "dm_ctl", ctl, sizeof ctl) || strcmp(ctl, khex))
            { lp_fail(why, why_size, "%s %s dm_ctl mismatch", khex, jname); goto done; }
        if (jf_str(top, "stage", st, sizeof st) || strcmp(st, lib_stage))
            { lp_fail(why, why_size, "%s %s stage is '%s' not '%s'", khex, jname, st, lib_stage); goto done; }
        uint32_t gt = 0;
        if (gs && (jf_u32(top, "gs_input_topology", &gt) || gt != (uint32_t)gs_topology))
            { lp_fail(why, why_size, "%s %s gs_input_topology is not %d", khex, jname, gs_topology); goto done; }
        if (sn < 20 || (sn & 3) || *(const uint32_t *)spv != 0x07230203u)
            { lp_fail(why, why_size, "%s %s is not a SPIR-V module", khex, sname); goto done; }
        {
            int window = lp_window_parse(top, contract, compute, why, why_size);
            if (!window && !packing) window = lp_window_apply(contract->window, gfx, spv, sn, why, why_size);
            if (window != 0) { out = window; goto done; }
        }
        {   char cw[128] = ""; const size_t sn0 = sn; const uint64_t il0 = s2a_libpack_interlock_dropped();
            if (s2a_libpack_spv_admit_caps(spv, &sn, cw, sizeof cw)) { lp_fail(why, why_size, "%s %s %s", khex, sname, cw); goto done; }
            if (s2a_libpack_interlock_dropped() != il0 && !atomic_exchange(&lp->ilk_said, 1))   // 每局首次一行具名红，之后只计数
                LP_RED("libpack_interlock_dropped: program %s %s uses a fragment interlock critical section; the device has no "
                       "VK_EXT_fragment_shader_interlock, drawing without it (fragment ordering not guaranteed); counted per module, first one this session",
                       khex, sname);
            if (sn != sn0 && !atomic_exchange(&lp->caps_said, 1))   // 每局首次一行（信息级），之后只计数
                LP_LOG("libpack_caps_stripped: program %s %s dropped %zu unused 64-bit capability declaration(s) (Float64/Int64 without a 64-bit type); counted per module, first one this session",
                       khex, sname, (sn0 - sn) / 8); }
        // 带几何的程序：每一段都不许写视口号 / 视口掩码（lp_spv_viewport_builtin，同现编具名拒）
        if (gs_topology >= 0) {
            int vp = lp_spv_viewport_builtin(spv, sn);
            if (vp) { lp_fail(why, why_size, vp > 0 ? "%s %s writes a viewport index/mask in a geometry program (not implemented)"
                                                   : "%s %s instructions unreadable", khex, sname); goto done; }
        }
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
        {   // 尺寸表 ABI：2 = 读支撑缓冲 2608 起的尺寸表，要 3776 字节（见 LP_SUPPORT_ABI2_BYTES）；没有这一项 = 不读表
            uint32_t abi = 0;
            if (!jf_u32(top, "resolution_scale_abi", &abi) && abi == 2) { if (*support_bytes < LP_SUPPORT_ABI2_BYTES) *support_bytes = LP_SUPPORT_ABI2_BYTES; }
            else if (abi && abi != 2) { lp_fail(why, why_size, "%s resolution_scale_abi %u unknown", khex, abi); goto done; }
        }
        {   // 计算共享内存闸：_b.json 的 spill 是批翻链按 32K 阈值在 MSL 侧做的溢出标记（Dsh.cs SPILL_SLOT），
            //   本库 _b.spv 里 Workgroup 声明原样保留——装不装得下一律按本设备 maxComputeSharedMemorySize 真值判，
            //   装不下的结果建不出计算管线（vkCreateComputePipelines 必败），出库前如实拒、退回现编
            //   （现编路径 DirectSharedSpill.Rewrite 会把溢出数组改写进宿主绑定的设备缓冲）。
            struct s2a_jv spf;
            if (compute && !s2a_jv_obj_find(top, "spill", &spf)) {
                uint32_t sps = 0; uint64_t wg = 0;
                if (jf_u32(top, "spill", &sps) || !sps) { lp_fail(why, why_size, "%s %s spill field invalid", khex, jname); goto done; }
                const char *werr = s2a_spv_workgroup_bytes(spv, sn, &wg);
                if (werr) { lp_fail(why, why_size, "%s %s: %s", khex, sname, werr); goto done; }
                if (wg > shared_limit) {
                    lp_fail(why, why_size, "%s %s compute shared memory %llu exceeds device limit %u",
                            khex, sname, (unsigned long long)wg, shared_limit);
                    goto done;
                }
            }
        }
        // tex_scales（缩放访问的贴图）不再拒收：ABI 2 下换算在着色器里经支撑缓冲的尺寸表做，表项全 0 走倍率 1 的旧路径，与现编同口径
        const char *v1_stage = lp_v1_stage_name(st);
        struct lp_emitted_bindings *emitted = &contract->emitted;
        struct lp_spv_bindings *declared = &contract->declared;
        if (lp_emit_bindings(binds, top, st, stage_no, v1_stage, compute, spv, sn, bind_sep, emitted, why, why_size)) goto done;
        if (lp_spv_read_bindings(spv, sn, declared)) {
            lp_fail(why, why_size, "%s %s decorations unreadable", khex, sname);
            goto done;
        }
        if (!emitted->uses_v2) {
            // v1 沿用按反射核对和补写。若另一段选了 v2，整份合成时会重新核对实际发出记录。
            for (int i = 0; i < declared->count; i++) {
                uint32_t binding = declared->number[i];
                int skipped = 0;
                if (lp_json_has_binding(top, stage_no, binding, &skipped)) continue;
                if ((int)binding == declared->support) {
                    lp_emit_support(binds, bind_sep, binding, compute);
                    lp_record_binding(emitted, binding, LP_BINDING_SUPPORT);
                    continue;
                }
                lp_fail(why, why_size, "%s %s binding %u has no %s%s binding", khex, sname, binding, skipped ? "mappable " : "", jname);
                goto done;
            }
        }
        jw_fmt(stages, "%s{\"stage\":\"%s\",\"spirv\":\"", *stage_mask ? "," : "", v1_stage);
        jw_b64(stages, spv, sn);
        jw_str(stages, "\"");
        if (mask) jw_fmt(stages, ",\"attributeReadMask\":%u", mask);
        if (!compute) lp_emit_c0_reads(stages, top);
        uint32_t fom = 0;
        if (!strcmp(v1_stage, "fs") && !s2a_libpack_spv_frag_outputs(spv, sn, &fom)) jw_fmt(stages, ",\"info\":{\"fragmentOutputMap\":%u}", fom);
        jw_str(stages, "}");
        *stage_mask |= 1u << nvn;
        *uses_v2 |= emitted->uses_v2;
        out = 0;
    }
done:
    return out;
}

static int lp_stage_emit(struct s2a_libpack *lp, const uint8_t key[8], unsigned nvn, int compute, int gs_topology,
                         uint32_t shared_limit, const struct s2a_libpack_gfx *gfx,
                         struct jw *stages, struct jw *binds, int *bind_sep, uint32_t *stage_mask,
                         uint32_t *support_bytes, int *uses_v2, struct lp_stage_contract *contract,
                         char *why, size_t why_size) {
    const char *lib_stage = lp_nvn_stage_name(nvn);
    if (!lib_stage) return S2A_LIBPACK_MISS;
    int gs = nvn == 2;
    if (gs && (gs_topology < 0 || gs_topology > 4)) return S2A_LIBPACK_MISS;
    char khex[17], jname[16], sname[16];
    for (int i = 0; i < 8; i++) snprintf(khex + 2*i, 3, "%02x", key[i]);
    snprintf(jname, sizeof jname, gs ? "_vg%d.json" : "_b.json", gs_topology);
    snprintf(sname, sizeof sname, gs ? "_vg%d.spv" : "_b.spv", gs_topology);
    unsigned kjson = gs ? LP_K_VGJSON + (unsigned)gs_topology : LP_K_BJSON;
    unsigned kspv = gs ? LP_K_VGSPV + (unsigned)gs_topology : LP_K_BSPV;
    uint8_t *jb = NULL, *spv = NULL; size_t jn = 0, sn = 0;
    char gw[192] = "";
    int rc = s2a_libpack_get(lp, key, kjson, &jb, &jn, gw, sizeof gw);
    if (rc != S2A_LIBPACK_HIT) { if (why && why_size) snprintf(why, why_size, "%s %s: %s", khex, jname, gw[0] ? gw : "absent"); free(jb); return rc; }
    rc = s2a_libpack_get(lp, key, kspv, &spv, &sn, gw, sizeof gw);
    if (rc != S2A_LIBPACK_HIT) { if (why && why_size) snprintf(why, why_size, "%s %s: %s", khex, sname, gw[0] ? gw : "absent"); free(jb); free(spv); return rc; }
    rc = lp_stage_from_bytes(lp, key, nvn, compute, gs_topology, shared_limit, gfx, 0, jb, jn, spv, sn,
                             stages, binds, bind_sep, stage_mask, support_bytes, uses_v2, contract, why, why_size);
    free(jb);
    free(spv);
    return rc;
}

// 绘制图元名（现编请求 graphicsState.topology 同名：nvn_vk_compile_facts.c nvk_input_topology）→ InputTopology 编号；
//   离线库只出点 / 线 / 三角三种（DshVk.GeometryTopologies），邻接与没有来源给 -1（几何程序按 miss）
static int lp_input_topology(const char *t) {
    if (!t) return -1;
    if (!strcmp(t, "Points")) return 0;
    if (!strcmp(t, "Lines")) return 1;
    if (!strcmp(t, "Triangles")) return 3;
    return -1;
}
// 本局 effective profile 的 capabilities.supportsGeometryShader（现编 profile.Caps.SupportsGeometryShader 同一字段）：1 开 / 0 没开或读不出
int s2a_libpack_profile_geometry_shader(const char *profile_json, size_t profile_bytes) {
    if (!profile_json || !profile_bytes) return 0;
    struct s2a_jv top = { profile_json, profile_bytes }, caps, f;
    int b = 0;
    if (!s2a_jv_is_object(top) || s2a_jv_obj_find(top, "capabilities", &caps) || !s2a_jv_is_object(caps) ||
        s2a_jv_obj_find(caps, "supportsGeometryShader", &f) || s2a_jv_bool(f, &b)) return 0;
    return b != 0;
}

static int lp_program_v1(const struct s2a_libpack *lp_c, const uint8_t keys[6][8], uint32_t stages,
                           const struct s2a_libpack_gfx *gfx,
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size, int account) {
    struct s2a_libpack *lp = (struct s2a_libpack *)lp_c;
    if (!lp || !lp->p.idx || !keys || !json || !json_len || !stages) return S2A_LIBPACK_INVALID;
    *json = NULL; *json_len = 0;
    // 图形接受口径同现编（DirectProgram.Parse）：vs+fs，或 vs+gs+fs（几何走原生几何阶段），或只有 vs（只写深度的绘制：
    //   深度预通道、阴影图；解析侧 allow_vs_only、后端 C76 管线都已支持，不造 fs）；别的形状（只有 fs / 带 tcs|tes / cs 走
    //   compute 线）本函数产不出可采纳结果，按 miss 如实回。
    if (stages != 0x1 && stages != 0x3 && stages != 0x7) {
        if (why && why_size) snprintf(why, why_size, "stages=0x%x is not a vs, vs+fs or vs+gs+fs program", stages);
        if (account) atomic_fetch_add(&g_misses, 1);
        return S2A_LIBPACK_MISS;
    }
    // 几何程序：设备没开 geometryShader 照旧 miss（现编同判据：profile 的 supportsGeometryShader）；
    //   输入图元挑同号那份 _vg<t>，邻接 / 没有来源 miss（现编也还没接邻接）
    int gs_topology = -1;
    if (stages == 0x7) {
        if (!gfx || !s2a_libpack_profile_geometry_shader(gfx->profile_json, gfx->profile_bytes)) {
            if (why && why_size) snprintf(why, why_size, "geometryShader is not enabled in the profile");
            if (account) atomic_fetch_add(&g_misses, 1);
            return S2A_LIBPACK_MISS;
        }
        gs_topology = lp_input_topology(gfx->topology);
        if (gs_topology < 0) {
            if (why && why_size) snprintf(why, why_size, "geometry input topology %s has no library variant", gfx->topology ? gfx->topology : "(none)");
            if (account) atomic_fetch_add(&g_misses, 1);
            return S2A_LIBPACK_MISS;
        }
    }
    int worst = S2A_LIBPACK_HIT;
    struct jw st = {0}, bd = {0};
    int sep = 0, uses_v2 = 0; uint32_t got = 0, support = 0;
    // 按 NVN 阶段号（vs=0 fs=1 gs=2 tcs=3 tes=4 cs=5）定长取下标：keys[] 与 contracts[] 同为全部阶段号一份，
    //   循环上界取自数组本身，阶段号永远在界内（原 contracts[3] 遇 tcs/tes 位会传出越界指针）
    struct lp_stage_contract contracts[LP_NVN_STAGES] = {0};
    for (unsigned nvn = 0, idx = 0; nvn < sizeof contracts / sizeof contracts[0] && idx < LP_NVN_STAGES; nvn++) {
        if (!(stages & (1u << nvn))) continue;
        char sw[192] = "";
        int rc = lp_stage_emit(lp, keys[idx], nvn, 0, gs_topology, 0, gfx, &st, &bd, &sep, &got, &support, &uses_v2,
                               &contracts[nvn], sw, sizeof sw);
        idx++;
        if (rc == 0) continue;
        // 半程序不命中：任一所需阶段拿不到，整个程序按该段的 miss/reject 计
        //   （reject 优先于 miss：有一个坏段就说明库有问题，不是“库没有”）
        if (why && why_size) snprintf(why, why_size, "%s", sw);
        if (rc < 0 || worst == S2A_LIBPACK_HIT) worst = rc;
    }
    if (uses_v2) {
        for (unsigned nvn = 0; nvn < sizeof contracts / sizeof contracts[0]; nvn++) {
            if (!(got & (1u << nvn))) continue;
            if (lp_finish_v2_bindings(&contracts[nvn], &bd, &sep, 0, why, why_size)) {
                worst = S2A_LIBPACK_REJECT;
                goto out;
            }
        }
    }
    if (worst != S2A_LIBPACK_HIT || got != stages) {
        if (worst == S2A_LIBPACK_HIT) worst = S2A_LIBPACK_MISS;   // 阶段位图对不上 = 库没有，miss
        goto out;
    }
    {
        struct jw j = {0};
        int version = uses_v2 ? 2 : 1;
        jw_fmt(&j, "{\"schema\":\"s2a-direct-program-v%d\",\"content\":{\"contract\":\"s2a-direct-program-content-v%d\",\"identity\":{\"directABI\":%u,\"translatorClosure\":\"%s\",\"profileHash\":\"%s\"},\"kind\":\"graphics\",\"stages\":[",
               version, version, direct_abi, translator_closure ? translator_closure : "", profile_hash ? profile_hash : "");
        jw_put(&j, st.b ? (char *)st.b : "", st.n);
        jw_str(&j, "],\"bindings\":[");
        jw_put(&j, bd.b ? (char *)bd.b : "", bd.n);
        jw_str(&j, "]");
        if (support) jw_fmt(&j, ",\"supportLayout\":{\"requiredSize\":%u}", support);
        jw_str(&j, "}}");
        if (st.oom || bd.oom || j.oom) { free(j.b); lp_fail(why, why_size, "program synthesis out of memory"); worst = S2A_LIBPACK_REJECT; goto out; }
        *json = j.b; *json_len = j.n;
        worst = S2A_LIBPACK_HIT;
    }
out:
    free(st.b); free(bd.b);
    if (account) {
        if (worst == S2A_LIBPACK_HIT) atomic_fetch_add(&g_hits, 1);
        else if (worst == S2A_LIBPACK_MISS) atomic_fetch_add(&g_misses, 1);
        else atomic_fetch_add(&g_rejects, 1);
    }
    return worst;
}
int s2a_libpack_program_v1(const struct s2a_libpack *lp_c, const uint8_t keys[6][8], uint32_t stages,
                           const struct s2a_libpack_gfx *gfx,
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size) {
    return lp_program_v1(lp_c, keys, stages, gfx, direct_abi, profile_hash, translator_closure, json, json_len, why, why_size, 1);
}

// 计算程序：单键（rp_S.skey[5]）的 _b.json（stage "cs"）+ _b.spv → kind "compute" 的 v1/v2 结果，
//   接受口径 = nvn_vk_program_parse_compute（与现编结果同一个 build_compute）。计算合同多出的四个顶层事实：
//   hasUnalignedStorageBuffer：库段带窗口掩码（取库时按本机对齐固化）时写 true（这份 .spv 按「基址可能不对齐」翻译），
//     旧库（无合同）写 false（批翻链按缺省 false 翻，照实）；非对齐基址只放行带窗口合同的库、且不含用户常量缓冲来源存储（lp_window_apply）；
//   c0Reads：null——_b.json 只有 drv_reads（按 MSL 文本的 vec4 粒度 + dynamic 标），给不出合同要的
//     字节精度读集；null 是解析器认可的「未知」，运行时按完整驱动 c0 布局填；
//   supportReads：null——_b.json 不带 support 读集，同上按「未知」写，运行时整块按恒等值填；
//   supportLayout.requiredSize：SUPPORT_DEFAULT_BYTES（2608）——dmsh_xlate 编进的是同一份 compat
//     Ryujinx.Graphics.Shader，SupportBuffer.RequiredSize 就是它；图形库路径不写 supportLayout，解析器也按这个数。
//   绑定号 = arg_id×6 + 5。计数口径同 program_v1（hit / miss / reject 三个计数器共用）。
static int lp_compute_v1(const struct s2a_libpack *lp_c, const uint8_t key[8],
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint32_t shared_limit, const struct s2a_libpack_gfx *gfx,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size, int account) {
    struct s2a_libpack *lp = (struct s2a_libpack *)lp_c;
    if (!lp || !lp->p.idx || !key || !json || !json_len) return S2A_LIBPACK_INVALID;
    *json = NULL; *json_len = 0;
    struct jw st = {0}, bd = {0};
    int sep = 0, uses_v2 = 0; uint32_t got = 0;
    uint32_t csupport = 0;
    struct lp_stage_contract contract = {0};
    int rc = lp_stage_emit(lp, key, 5, 1, -1, shared_limit, gfx, &st, &bd, &sep, &got, &csupport, &uses_v2, &contract, why, why_size);
    if (rc == 0 && got != (1u << 5)) rc = S2A_LIBPACK_MISS;
    if (rc == 0 && uses_v2) rc = lp_finish_v2_bindings(&contract, &bd, &sep, 1, why, why_size);
    if (rc == 0) {
        struct jw j = {0};
        int version = uses_v2 ? 2 : 1;
        jw_fmt(&j, "{\"schema\":\"s2a-direct-program-v%d\",\"content\":{\"contract\":\"s2a-direct-program-content-v%d\",\"identity\":{\"directABI\":%u,\"translatorClosure\":\"%s\",\"profileHash\":\"%s\"},\"kind\":\"compute\","
                   "\"hasUnalignedStorageBuffer\":%s,\"c0Reads\":null,\"supportReads\":null,\"stages\":[",
               version, version, direct_abi, translator_closure ? translator_closure : "", profile_hash ? profile_hash : "", LP_WINDOW_KIND(contract.window.mode) == LP_WINDOW_MASK ? "true" : "false");
        jw_put(&j, st.b ? (char *)st.b : "", st.n);
        jw_str(&j, "],\"bindings\":[");
        jw_put(&j, bd.b ? (char *)bd.b : "", bd.n);
        jw_fmt(&j, "],\"supportLayout\":{\"requiredSize\":%u}}}", csupport > SUPPORT_DEFAULT_BYTES ? csupport : SUPPORT_DEFAULT_BYTES);
        if (st.oom || bd.oom || j.oom) { free(j.b); lp_fail(why, why_size, "program synthesis out of memory"); rc = S2A_LIBPACK_REJECT; }
        else { *json = j.b; *json_len = j.n; }
    } else if (rc > 0) rc = S2A_LIBPACK_MISS;
    else rc = S2A_LIBPACK_REJECT;
    free(st.b); free(bd.b);
    if (account) {
        if (rc == S2A_LIBPACK_HIT) atomic_fetch_add(&g_hits, 1);
        else if (rc == S2A_LIBPACK_MISS) atomic_fetch_add(&g_misses, 1);
        else atomic_fetch_add(&g_rejects, 1);
    }
    return rc;
}
int s2a_libpack_compute_v1(const struct s2a_libpack *lp_c, const uint8_t key[8],
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint32_t shared_limit, const struct s2a_libpack_gfx *gfx,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size) {
    return lp_compute_v1(lp_c, key, direct_abi, profile_hash, translator_closure, shared_limit, gfx, json, json_len, why, why_size, 1);
}

int s2a_libpack_shadow_json(const struct s2a_libpack *lp, const uint8_t keys[6][8], uint32_t stages,
                            const struct s2a_libpack_gfx *gfx, uint32_t abi, const char *profile,
                            const char *closure, uint32_t shared, uint8_t **out, size_t *len, char *why, size_t cap) {
    if (stages == 32) return lp_compute_v1(lp, keys[0], abi, profile, closure, shared, gfx, out, len, why, cap, 0);
    return lp_program_v1(lp, keys, stages, gfx, abi, profile, closure, out, len, why, cap, 0);
}

#include "libpack_stage_read.inc"

#ifdef S2A_LIBPACK_PACKER
#include "libpack_stage_export.inc"
#endif
