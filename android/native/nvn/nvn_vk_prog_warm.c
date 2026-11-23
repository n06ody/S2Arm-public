#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // open/fsync/mkdir/rename 在严格 -std=c11 下也要可见（同 compile_worker.c）
#endif
// 程序预热清单（nvn_vk_prog_warm.h 的实现）：跨会话保存「本机真实用过的程序内容身份」，
// 开局由预热线程按 uses 热点顺序把程序提前读盘 / 解析 / 建设备对象并发布到 g_progs。
//
// 本文件只做清单本身（纯逻辑 + 文件层，无 GPU、无 Vulkan 类型）：读/写/去重/有界/排序/有效性过滤。
// 真正「按记录发布程序」的 nvk_prog_warm_build 与收尾静默 nvk_shader_warm_quiesce 在
// android/native/gfx_plat/nvk_shader.c（它们要拿 g_progs、build() 与后端着色器装载）。
//
// 线程：note 在提交线程（真实 resolve 成功后）；load/take/pending/save 由预热线程调用（Lead 的
// nvn_warm.c，先程序后管线）；stats/contains 任意线程。内部一把清单锁保护全部可变状态；落盘另有一把
// 串行锁（30 秒定期落盘与收尾落盘可能相撞），写盘用临时文件 + fsync + rename + 目录 fsync。
//
// 有效性：文件头带 profile_sha / direct_abi / translator closure 摘要（闭包只存 SHA-256 前 8 字节，
// 隐私上不多留原文）。任一与当前会话不一致就整份拒绝——那是另一台设备 / 另一版翻译器的清单，拿来做
// 预热只会白读盘。条目的阶段键只作日志/定位：开局 source_store 还是空的，预热不依赖它（c1 随记录落盘）。
//
// 有界：上限 debug.s2a.nvk_warm_max（缺省 8192 条，一条 88 字节头 + 各阶段 c1 整段，常见约 0.9 KB，约 7 MB）。满了淘汰 uses 最低
// （同 uses 淘汰最久没用）的一条，不拒绝新条目：热点优先才让常见路线先安全。
// 缺省为什么是 8192：真机同进程 45 秒里出现 4430 个不同身份；世界场景一条路线（站着、前走、折返、转视角，约 6 分钟）
// 一局出现 6533 个。缺省 4096 时只有走动才会用到的程序排在 uses 低位被挤掉，每次走进新区域都要在回放线程上
// 读盘 / 现场编；读数见 docs/坑记录.md「预热清单上限 4096 → 8192」一条。
#include "nvn_vk_prog_warm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvn_plat.h"
#include "direct/compile_request.h"
#include "direct/sha256.h"

#define NVK_PROG_WARM_FILE "programs-v1.bin"   // 旧名（一款一份）：只读作过渡，落盘后删
// 按翻译器闭包分文件：programs-v1-<闭包 SHA-256 前 8 字节 hex>.bin。A/B 两臂 closure 不同时各用各的，交替跑不再互相覆盖；
// 同目录最多留 NVK_PROG_WARM_KEEP 份（本局这份 + 最近改过的另一份），多的落盘时删（offline-shader-miss 调查：ABAB 每局都冷启动）。
#define NVK_PROG_WARM_PREFIX "programs-v1-"
#define NVK_PROG_WARM_KEEP 2
#define NVK_PROG_WARM_MAGIC 0x31575053u     // "SPW1" little-endian：S P W 1
#define NVK_PROG_WARM_VERSION 2u              // 2：条目变长，c1 按整段记（版本 1 每阶段定长 0x100：长 c1 被截，预热以同一身份发布后段全 0 的程序 → 八方旅人场景黑）。
                                            //   版本 1 的文件整份按「版本不符」拒收（不按新格式误读），本局照常记、落盘写成版本 2
#define NVK_PROG_WARM_HDR_FIELDS 60u        // magic4 版本4 条目头大小4 条数4 profile32 ABI4 闭包摘要8
#define NVK_PROG_WARM_SUM 8u                // 末尾 FNV-1a 64
#define NVK_PROG_WARM_ENTRY 88u             // 条目头：identity32 mask4 keys24 c1_ok1 lp_ok1 lp_ssbo_window1 pad1 c1_len 3×4 uses4 last_use8；其后按阶段 0..2 依次接有效阶段的 c1 整段（各补齐到 4 字节）
#define NVK_PROG_WARM_FILE_MAX (64u << 20)  // 读文件的体积硬顶（8192 条 × 平均 c1 约 1 KB ≈ 9 MB；硬顶留余量，超了整份拒）
#define NVK_PROG_WARM_ABS_MAX 16384u        // 上限的硬顶（flag 调再大也不超过它）
#define NVK_PROG_WARM_DEFAULT_MAX 8192      // 缺省上限（真机走一圈 6533 个身份；一条约 0.9 KB，约 7 MB）
#define NVK_PROG_WARM_MAX_PATH 640

// c1 只读副本：只追加、按内容去重、进程内不释放。记录按值拷给预热线程 / 冻结快照 / 落盘排序，指针一直有效，
//   不随会话复位作废（复位后预热线程手上残留的记录也不会读到释放过的内存）。量级 = 本进程见过的不同 c1 的总字节（几 MB）。
static pthread_mutex_t g_c1_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t h; uint32_t len; uint8_t *p; } *g_c1v;
static uint32_t g_c1n, g_c1cap, *g_c1tab, g_c1tab_n;   // g_c1tab：开放寻址表，值 = 下标 + 1（0 = 空）
static uint64_t c1_hash(const uint8_t *p, uint32_t len) {
    uint64_t h = 1469598103934665603ull ^ len;
    for (uint32_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}
static int c1_tab_put(uint32_t idx) {   // 调用方持 g_c1_mu；表至少半空
    uint32_t m = g_c1tab_n - 1, i = (uint32_t)g_c1v[idx].h & m;
    while (g_c1tab[i]) i = (i + 1) & m;
    g_c1tab[i] = idx + 1;
    return 0;
}
static const uint8_t *c1_intern(const uint8_t *p, uint32_t len) {
    if (!p || len < NVK_PROG_REC_C1_MIN || len > NVK_PROG_REC_C1_MAX) return NULL;
    const uint64_t h = c1_hash(p, len);
    pthread_mutex_lock(&g_c1_mu);
    if (g_c1tab_n) {
        for (uint32_t m = g_c1tab_n - 1, i = (uint32_t)h & m; g_c1tab[i]; i = (i + 1) & m) {
            uint32_t k = g_c1tab[i] - 1;
            if (g_c1v[k].h == h && g_c1v[k].len == len && !memcmp(g_c1v[k].p, p, len)) { const uint8_t *r = g_c1v[k].p; pthread_mutex_unlock(&g_c1_mu); return r; }
        }
    }
    if (g_c1n == g_c1cap) {
        uint32_t cap = g_c1cap ? g_c1cap * 2 : 256;
        void *v = realloc(g_c1v, (size_t)cap * sizeof *g_c1v);
        if (!v) { pthread_mutex_unlock(&g_c1_mu); return NULL; }
        g_c1v = v; g_c1cap = cap;
    }
    if ((uint64_t)(g_c1n + 1) * 2 > g_c1tab_n) {   // 装载率 ≤ 1/2：翻倍重建
        uint32_t n = g_c1tab_n ? g_c1tab_n * 2 : 512;
        uint32_t *t = calloc(n, sizeof *t);
        if (!t) { pthread_mutex_unlock(&g_c1_mu); return NULL; }
        free(g_c1tab); g_c1tab = t; g_c1tab_n = n;
        for (uint32_t k = 0; k < g_c1n; k++) c1_tab_put(k);
    }
    uint8_t *q = malloc(len);
    if (!q) { pthread_mutex_unlock(&g_c1_mu); return NULL; }
    memcpy(q, p, len);
    g_c1v[g_c1n].h = h; g_c1v[g_c1n].len = len; g_c1v[g_c1n].p = q;
    c1_tab_put(g_c1n++);
    pthread_mutex_unlock(&g_c1_mu);
    return q;
}
// 把记录里有效阶段的 c1 换成本模块的只读副本；长度越界 / 指针空 / 内存不足的阶段清掉有效位（那一段按没捕获处理，同 build 的判据）。
static void rec_intern_c1(struct nvk_prog_rec *r) {
    for (int st = 0; st < NVK_PROG_REC_STAGES; st++) {
        const uint8_t *q = (r->c1_ok & (1u << st)) ? c1_intern(r->c1[st], r->c1_len[st]) : NULL;
        if (!q) { r->c1_ok &= (uint8_t)~(1u << st); r->c1[st] = NULL; r->c1_len[st] = 0; }
        else r->c1[st] = q;
    }
}
static uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }

struct slot {
    struct nvk_prog_rec rec;
    uint64_t last_use_rank;
};

static struct {
    pthread_mutex_t mu;
    struct slot *v;
    uint32_t n, cap;
    // 本局预热集：load 时冻结的一份不可变快照（身份 + 阶段键 + C1）。运行期的 note / 淘汰 / 热点排序只写 g.v、
    // 只影响下一局清单，**绝不改这份快照**——否则 take 的游标会走在「正在被 qsort / 淘汰的数组」上，同一身份
    // 被取两次、另一个身份永远漏掉（外部复核 prog_order_probe / pipe_order_probe 复现的 A→A）。
    struct nvk_prog_rec *plan;
    uint32_t plan_n, plan_i;
    uint32_t limit;
    int dirty;                    // 上次落盘以来有变化（save 的快路径）
    int have_facts;               // 载入过本局有效性事实（save 的头需要它）
    uint8_t profile_sha[32];
    int32_t direct_abi;
    uint8_t closure_sha[32];
    uint64_t max_rank;
    uint64_t reset_seq;           // 只由会话 reset 推进；锁外读盘不能跨会话提交
    // 随包种子（首局预热，nvn_vk_prog_warm.h「随包种子」）：-1 = 本局还没定（load 没走到能定的那一步）；
    // 0 = 本局没用随包种子（有本机清单 / 包里没带 / 坏了）；1 = 用了，seed_dir 是那一对种子所在目录（管线种子同目录）
    int seed_state;
    char seed_dir[NVK_PROG_WARM_MAX_PATH];
    struct nvk_prog_warm_stats st;
} g = { .mu = PTHREAD_MUTEX_INITIALIZER, .limit = NVK_PROG_WARM_DEFAULT_MAX, .seed_state = -1 };
// 落盘串行：预热线程的 30 秒定期写与收尾写不并发（否则两个临时文件抢同一个 rename 目标）。
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;

static void warm_sha256(const void *p, size_t n, uint8_t out[32]) {
    s2a_sha256 c; s2a_sha256_init(&c); s2a_sha256_update(&c, p, n); s2a_sha256_final(&c, out);
}
static uint64_t fnv1a64(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static int identity_zero(const uint8_t id[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= id[i];
    return acc == 0;
}
// 新层只产生这三种图形阶段掩码（rp_shader_graphics_stages：VS / VS+FS / VS+FS+GS）。
static int stage_count(uint32_t mask) {
    if (mask == 32u) return 1;   // CS（清单中独占掩码，不与 VS 1 混）
    if (mask == 1u) return 1;
    if (mask == 3u) return 2;
    if (mask == 7u) return 3;
    return 0;
}
// 清单条目的结构合法性（不是本局有效性：那是 profile/ABI/闭包那条，在 load 里整份判）。
static int rec_well_formed(const struct nvk_prog_rec *r) {
    if (!r || !stage_count(r->stage_mask) || (r->c1_ok & ~7u) || identity_zero(r->identity)) return 0;
    if (r->lp_ssbo_window && (r->lp_ssbo_window != 1 || r->lp_ok != 2)) return 0;
    for (int st = 0; st < NVK_PROG_REC_STAGES; st++)
        if ((r->c1_ok & (1u << st)) && (!r->c1[st] || r->c1_len[st] < NVK_PROG_REC_C1_MIN || r->c1_len[st] > NVK_PROG_REC_C1_MAX)) return 0;
    return 1;
}
static void stats_reset(void) { memset(&g.st, 0, sizeof g.st); }
static void reset_locked(void) {
    g.reset_seq++;
    free(g.v); g.v = NULL;
    free(g.plan); g.plan = NULL;
    g.n = g.cap = 0;
    g.plan_n = g.plan_i = 0;
    g.limit = 0;   // 下一局（或下一个 load）按当时的 flag 重读上限
    g.dirty = 0; g.have_facts = 0; g.max_rank = 0;
    memset(g.profile_sha, 0, sizeof g.profile_sha); memset(g.closure_sha, 0, sizeof g.closure_sha);
    g.direct_abi = 0;
    g.seed_state = -1; g.seed_dir[0] = 0;
    stats_reset();
}
static int ensure_cap_locked(uint32_t need) {
    if (need <= g.cap) return 0;
    uint32_t cap = g.cap ? g.cap : 64;
    while (cap < need) cap *= 2;
    struct slot *v = (struct slot *)realloc(g.v, (size_t)cap * sizeof *v);
    if (!v) return -1;
    g.v = v; g.cap = cap;
    return 0;
}
// 本局有效性事实：provider 还没就绪（activate 之前）返回 -1，由调用方决定重试还是先不写。
static int fetch_facts(uint8_t profile_sha[32], uint8_t closure_sha[32], int32_t *abi) {
    struct s2a_direct_compile_context pc;
    memset(&pc, 0, sizeof pc);
    if (s2a_direct_provider_compile_context(&pc) || !pc.profile_json || !pc.profile_bytes ||
        !pc.translator_closure || !pc.translator_closure[0]) return -1;
    warm_sha256(pc.profile_json, pc.profile_bytes, profile_sha);
    warm_sha256(pc.translator_closure, strlen(pc.translator_closure), closure_sha);
    *abi = pc.direct_abi;
    return 0;
}
// 本局清单路径（按闭包摘要命名）。0 = 好；-1 = 路径过长
static int warm_path(char *out, size_t n, const char *dir, const uint8_t closure_sha[32]) {
    char hx[17]; for (int i = 0; i < 8; i++) snprintf(hx + 2 * i, 3, "%02x", closure_sha[i]);
    return snprintf(out, n, "%s/" NVK_PROG_WARM_PREFIX "%s.bin", dir, hx) >= (int)n ? -1 : 0;
}
// 随包种子目录：<安装 revision 根>/shaders/direct/warm-vk/<闭包摘要前 8 字节 hex>-<profile 摘要前 8 字节 hex>
//   （revision 根与出包预编库同一个来源：环境变量 S2A_SHADER_REVISION_ROOT，nvk_shader.c libpack_fetch）。目录名带闭包与 profile：
//   一份种子只给记下它的那一类设备 + 那一版翻译器用（文件头还会再整份核一遍）；管线种子没有自己的有效性头，靠同目录配对。
//   0 = 拼好；-1 = 没有 revision 根 / 路径过长（= 没有随包种子）
static int seed_dir_of(char *out, size_t n, const uint8_t closure_sha[32], const uint8_t profile_sha[32]) {
    const char *root = getenv("S2A_SHADER_REVISION_ROOT");
    if (!root || !root[0]) return -1;
    char c[17], p[17];
    for (int i = 0; i < 8; i++) { snprintf(c + 2 * i, 3, "%02x", closure_sha[i]); snprintf(p + 2 * i, 3, "%02x", profile_sha[i]); }
    return snprintf(out, n, "%s/shaders/direct/warm-vk/%s-%s", root, c, p) >= (int)n ? -1 : 0;
}
static int warm_name_is_ours(const char *name) {   // programs-v1-<16 位小写 hex>.bin
    const size_t pl = sizeof NVK_PROG_WARM_PREFIX - 1;
    if (strncmp(name, NVK_PROG_WARM_PREFIX, pl) || strlen(name) != pl + 16 + 4 || strcmp(name + pl + 16, ".bin")) return 0;
    for (size_t i = pl; i < pl + 16; i++) if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return 1;
}
// 落盘后修剪：本局这份留着；别的闭包的清单按修改时间只留最新的 KEEP-1 份；旧名文件删掉（它的内容已并进本局这份或已作废）。
static void warm_prune(const char *dir, const char *keep_path) {
    const char *keep = strrchr(keep_path, '/'); keep = keep ? keep + 1 : keep_path;
    DIR *d = opendir(dir); if (!d) return;
    char names[32][64]; time_t mt[32]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!warm_name_is_ours(e->d_name) || !strcmp(e->d_name, keep) || n >= 32) continue;
        char pth[NVK_PROG_WARM_MAX_PATH]; struct stat st;
        if (snprintf(pth, sizeof pth, "%s/%s", dir, e->d_name) >= (int)sizeof pth || stat(pth, &st)) continue;
        memcpy(names[n], e->d_name, sizeof NVK_PROG_WARM_PREFIX - 1 + 16 + 4 + 1); mt[n] = st.st_mtime; n++;   // 名字长度已由 warm_name_is_ours 定死（32 + NUL）
    }
    closedir(d);
    for (int k = 0; k < NVK_PROG_WARM_KEEP - 1 && k < n; k++) {   // 最新的几份挪到前面（n 很小，选择排序）
        int best = k; for (int i = k + 1; i < n; i++) if (mt[i] > mt[best]) best = i;
        if (best != k) { char t[64]; memcpy(t, names[k], 64); memcpy(names[k], names[best], 64); memcpy(names[best], t, 64); time_t tm = mt[k]; mt[k] = mt[best]; mt[best] = tm; }
    }
    for (int i = NVK_PROG_WARM_KEEP - 1; i < n; i++) { char pth[NVK_PROG_WARM_MAX_PATH]; if (snprintf(pth, sizeof pth, "%s/%s", dir, names[i]) < (int)sizeof pth) unlink(pth); }
    { char pth[NVK_PROG_WARM_MAX_PATH]; if (snprintf(pth, sizeof pth, "%s/%s", dir, NVK_PROG_WARM_FILE) < (int)sizeof pth) unlink(pth); }
}
static uint32_t warm_limit(void) {
    int v = nvn_plat_flag("nvk_warm_max", NVK_PROG_WARM_DEFAULT_MAX);
    if (v < 1) return 1;
    if ((unsigned)v > NVK_PROG_WARM_ABS_MAX) return NVK_PROG_WARM_ABS_MAX;
    return (uint32_t)v;
}
static int rank_ref_cmp(const void *pa, const void *pb) {
    const struct slot *a = *(struct slot * const *)pa;
    const struct slot *b = *(struct slot * const *)pb;
    return (a->last_use_rank > b->last_use_rank) - (a->last_use_rank < b->last_use_rank);
}
// Rebase only the ranks, keeping slot order and preserving equal ranks. Old v1
// wall-clock values are ordered, not interpreted as time. `refs` has n slots.
static uint64_t rebase_ranks(struct slot *v, uint32_t n, struct slot **refs, uint64_t start) {
    if (!n) return start;
    for (uint32_t i = 0; i < n; i++) refs[i] = &v[i];
    qsort(refs, n, sizeof *refs, rank_ref_cmp);
    uint64_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t old = refs[i]->last_use_rank;
        if (i == 0 || old != prev) start++;
        refs[i]->last_use_rank = start;
        prev = old;
    }
    return start;
}
// uses 降序；同 uses 用 last_use 降序（谁最近用过谁先）；再同按 identity 升序，保证同一份清单两次落盘逐字节一致。
// 落盘排序只排一份下标表，不动 g.v：本局实时表可能正被 note / 淘汰改（虽然都在锁里，就地 qsort 会让
// 「这一份快照」和「take 看到的东西」分家，外部复核就是这个 repro 的根因之一）。
static const struct slot *g_cmp_base;
static int slot_cmp_idx(const void *pa, const void *pb) {
    const struct slot *a = &g_cmp_base[*(const uint32_t *)pa], *b = &g_cmp_base[*(const uint32_t *)pb];
    if (a->rec.uses != b->rec.uses) return a->rec.uses < b->rec.uses ? 1 : -1;
    if (a->last_use_rank != b->last_use_rank) return a->last_use_rank < b->last_use_rank ? 1 : -1;
    return memcmp(a->rec.identity, b->rec.identity, 32);
}
static int slot_cmp(const void *pa, const void *pb) {
    const struct slot *a = (const struct slot *)pa, *b = (const struct slot *)pb;
    if (a->rec.uses != b->rec.uses) return a->rec.uses < b->rec.uses ? 1 : -1;
    if (a->last_use_rank != b->last_use_rank) return a->last_use_rank < b->last_use_rank ? 1 : -1;
    return memcmp(a->rec.identity, b->rec.identity, 32);
}
static int find_locked(const uint8_t identity[32], uint32_t *at) {
    for (uint32_t i = 0; i < g.n; i++)
        if (!memcmp(g.v[i].rec.identity, identity, 32)) { if (at) *at = i; return 1; }
    return 0;
}

// ---------------- 读 ----------------
static int read_file(const char *path, uint8_t **out, size_t *out_len) {
    *out = NULL; *out_len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    // 上限按硬顶算：上一次会话的 nvk_warm_max 可能比本局大，文件更大也要能读，读后按本局上限截。
    size_t cap = NVK_PROG_WARM_FILE_MAX;
    if (st.st_size < (off_t)NVK_PROG_WARM_HDR_FIELDS || (uint64_t)st.st_size > (uint64_t)cap) { close(fd); errno = EFBIG; return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size);
    if (!buf) { close(fd); errno = ENOMEM; return -1; }
    size_t off = 0, want = (size_t)st.st_size;
    while (off < want) {
        ssize_t n = read(fd, buf + off, want - off);
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -1; }
        if (n == 0) { free(buf); close(fd); errno = EIO; return -1; }   // 短读：文件在脚下被改
        off += (size_t)n;
    }
    close(fd);
    *out = buf; *out_len = want;
    return 0;
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int nvk_prog_warm_load(void) {
    pthread_mutex_lock(&g.mu);
    uint64_t reset_seq = g.reset_seq;
    pthread_mutex_unlock(&g.mu);
    // 本局有效性事实：拿不到（provider 还没激活）就返回 -1 让调度器重试，不拿“没有清单”糊弄过去。
    const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE);
    if (!dir || !dir[0]) return -1;
    uint8_t profile_sha[32], closure_sha[32];
    int32_t direct_abi = 0;
    if (fetch_facts(profile_sha, closure_sha, &direct_abi)) return -1;
    const int32_t pc_abi = direct_abi;

    char path[NVK_PROG_WARM_MAX_PATH];
    if (warm_path(path, sizeof path, dir, closure_sha)) {   // 路径过长：本局没有清单，种子也不用（定下来，管线那边不用再等）
        pthread_mutex_lock(&g.mu); if (g.reset_seq == reset_seq) g.seed_state = 0; pthread_mutex_unlock(&g.mu);
        return 0;
    }
    uint8_t *buf = NULL; size_t len = 0;
    int rf = read_file(path, &buf, &len), e = rf ? errno : 0;
    if (rf && e == ENOENT) {   // 本闭包还没有清单：读旧名那份（升级后第一局；闭包对得上才收，见下面的有效性检查）
        char legacy[NVK_PROG_WARM_MAX_PATH];
        if (snprintf(legacy, sizeof legacy, "%s/%s", dir, NVK_PROG_WARM_FILE) < (int)sizeof legacy) { rf = read_file(legacy, &buf, &len); e = rf ? errno : 0; }
    }
    // 首局（本机这一版翻译器连旧名清单都没有）：读随包种子。有本机清单一律用本机的，种子不读。
    // 种子是锦上添花：不在 / 读不动 / 坏 / 不属于本局，都只打一行、按「本局没有清单」走，不拦起局。
    int from_seed = 0;
    char seed_dir[NVK_PROG_WARM_MAX_PATH] = "", seed_file[NVK_PROG_WARM_MAX_PATH] = "";
    if (rf && e == ENOENT && !seed_dir_of(seed_dir, sizeof seed_dir, closure_sha, profile_sha)) {
        const char *nm = strrchr(path, '/');
        if (snprintf(seed_file, sizeof seed_file, "%s/%s", seed_dir, nm ? nm + 1 : path) < (int)sizeof seed_file) {
            uint8_t *sb = NULL; size_t sl = 0;
            int sr = read_file(seed_file, &sb, &sl), se = sr ? errno : 0;
            if (!sr) { buf = sb; len = sl; rf = 0; e = 0; from_seed = 1; }
            else if (se != ENOENT) {
                char m[200]; snprintf(m, sizeof m, "【黄】程序清单随包种子读不动（%d），本局不用种子", se);
                nvn_plat_log(NVN_PLAT_LOG_WARN, "warm", m);
            }
        }
    }
    if (rf) {
        pthread_mutex_lock(&g.mu);
        if (g.reset_seq == reset_seq && !g.have_facts) {
            g.have_facts = 1; memcpy(g.profile_sha, profile_sha, 32); g.direct_abi = pc_abi; memcpy(g.closure_sha, closure_sha, 32);
        }
        if (g.reset_seq == reset_seq) g.seed_state = 0;
        pthread_mutex_unlock(&g.mu);
        if (e == ENOENT) {
            const char *nm = strrchr(path, '/');
            char m[200]; snprintf(m, sizeof m, "程序清单：目录里还没有这一版翻译器的清单 %.48s（保留本局记录）", nm ? nm + 1 : path);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "warm", m);
            return 0;
        }
        { char m[192]; snprintf(m, sizeof m, "【红】程序清单读取失败（%d），整份拒绝", e); nvn_plat_log(NVN_PLAT_LOG_RED, "warm", m); }
        return 0;
    }
    // 整份校验：magic / 版本 / 条目大小 / 条数 / 长度 / FNV，任一不符就不喂给运行时。
    const char *bad = NULL;
    uint32_t count = 0;
    if (len < (size_t)NVK_PROG_WARM_HDR_FIELDS + NVK_PROG_WARM_SUM) bad = "文件太短";
    else if (get_u32(buf) != NVK_PROG_WARM_MAGIC) bad = "magic 不符";
    else if (get_u32(buf + 4) != NVK_PROG_WARM_VERSION) bad = "版本不符";
    else if (get_u32(buf + 8) != NVK_PROG_WARM_ENTRY) bad = "条目大小不符";
    else {
        count = get_u32(buf + 12);
        if (count > NVK_PROG_WARM_ABS_MAX) bad = "条数超过硬顶";
        else {   // 变长条目：逐条走一遍，每条的 c1 长度在 [MIN, MAX]、合计正好落到校验和之前
            uint64_t off = NVK_PROG_WARM_HDR_FIELDS, end = (uint64_t)len - NVK_PROG_WARM_SUM;
            for (uint32_t i = 0; i < count && !bad; i++) {
                if (off + NVK_PROG_WARM_ENTRY > end) { bad = "长度与条数不符"; break; }
                const uint8_t *e = buf + off; off += NVK_PROG_WARM_ENTRY;
                for (int st = 0; st < NVK_PROG_REC_STAGES && !bad; st++) {
                    if (!(e[60] & (1u << st))) { if (get_u32(e + 64 + 4 * st)) bad = "条目 c1 长度与有效位不符"; continue; }
                    uint32_t cl = get_u32(e + 64 + 4 * st);
                    if (cl < NVK_PROG_REC_C1_MIN || cl > NVK_PROG_REC_C1_MAX) bad = "条目 c1 长度越界";
                    else if (off + pad4(cl) > end) bad = "长度与条数不符";
                    else off += pad4(cl);
                }
            }
            if (!bad && off != end) bad = "长度与条数不符";
            if (!bad && get_u64(buf + len - NVK_PROG_WARM_SUM) != fnv1a64(buf, len - NVK_PROG_WARM_SUM)) bad = "FNV 校验不符";
        }
    }
    const uint8_t *hdr_profile = buf + 16, *hdr_closure = buf + 52;
    int mismatch = 0;   // 1 = 文件本身好、只是不属于本局（换版本 / 换设备是正常情形，打黄不打红）
    int32_t hdr_abi = (int32_t)get_u32(buf + 48);
    if (!bad) {
        // 本局有效性：另一台设备 / 另一版翻译器的清单整份不要（预热上去也只会编出对不上的东西）。
        // 先比闭包、再比接口版本、最后比 profile：profile 原文里就有 translatorClosure，先比 profile 的话
        // 「只换了 APK」也会被报成「换设备 / 驱动」（offline-shader-miss 调查：12 次整份拒绝全是换 APK）。
        if (memcmp(hdr_closure, closure_sha, 8)) { bad = "翻译器闭包不同（换了 APK / 编译器版本，清单是另一版翻译器记的）"; mismatch = 1; }
        else if (hdr_abi != pc_abi) { bad = "directABI 不同（换了直连接口版本）"; mismatch = 1; }
        else if (memcmp(hdr_profile, profile_sha, 32)) { bad = "设备 profile 不同（同一版翻译器；设备能力 / 驱动 / 画质开关变了）"; mismatch = 1; }
    }
    pthread_mutex_lock(&g.mu);
    if (g.reset_seq != reset_seq || (g.have_facts &&
        (memcmp(g.profile_sha, profile_sha, 32) || g.direct_abi != pc_abi ||
         memcmp(g.closure_sha, closure_sha, 32)))) {
        pthread_mutex_unlock(&g.mu); free(buf); return 0;
    }
    if (!g.have_facts) {
        g.have_facts = 1; memcpy(g.profile_sha, profile_sha, 32); g.direct_abi = pc_abi; memcpy(g.closure_sha, closure_sha, 32);
    }
    g.seed_state = 0;   // 走到这里本局就定了：下面只有「种子整份收下」才改成 1
    if (bad) {
        pthread_mutex_unlock(&g.mu);
        free(buf);
        if (from_seed) {   // 随包种子坏 / 不属于本局：锦上添花，打黄一行、不用，不算本机清单坏
            char m[288]; snprintf(m, sizeof m, "【黄】程序清单随包种子不用：%s（%u 条）；本局按没有清单走，首遇照常准备", bad, (unsigned)count);
            nvn_plat_log(NVN_PLAT_LOG_WARN, "warm", m);
            return 0;
        }
        { char m[288]; snprintf(m, sizeof m, "%s程序清单整份拒绝：%s（%u 条）", mismatch ? "【黄】" : "【红】", bad, (unsigned)count);
          nvn_plat_log(mismatch ? NVN_PLAT_LOG_WARN : NVN_PLAT_LOG_RED, "warm", m); }
        return 0;
    }
    // 逐条：结构合法性；本局上限内按 uses 热点保留。超过本局上限的（上次上限更大）按 rejected 记。
    uint32_t usable = 0, filtered = 0;
    uint32_t limit = warm_limit();
    uint32_t keep_cap = count < limit ? count : limit;
    struct slot *disk = keep_cap ? (struct slot *)malloc((size_t)keep_cap * sizeof *disk) : NULL;
    if (keep_cap && !disk) { pthread_mutex_unlock(&g.mu); free(buf); return 0; }
    size_t eoff = NVK_PROG_WARM_HDR_FIELDS;   // 整份校验已走过一遍，这里按同样的步长取
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = buf + eoff; eoff += NVK_PROG_WARM_ENTRY;
        struct slot s;
        memset(&s, 0, sizeof s);
        memcpy(s.rec.identity, e, 32);
        s.rec.stage_mask = get_u32(e + 32);
        memcpy(s.rec.keys, e + 36, sizeof s.rec.keys);
        s.rec.c1_ok = e[60];
        s.rec.lp_ok = e[61];
        s.rec.lp_ssbo_window = e[62];   // 旧 v2 清单的保留字节为 0，只可尝试对齐的库合同
        for (int st = 0; st < NVK_PROG_REC_STAGES; st++) {
            if (!(s.rec.c1_ok & (1u << st))) continue;
            s.rec.c1_len[st] = get_u32(e + 64 + 4 * st); s.rec.c1[st] = buf + eoff; eoff += pad4(s.rec.c1_len[st]);
        }
        s.rec.uses = get_u32(e + 76);
        s.last_use_rank = get_u64(e + 80);
        if (!rec_well_formed(&s.rec)) { filtered++; continue; }
        const uint8_t c1_had = s.rec.c1_ok;
        rec_intern_c1(&s.rec);   // 指针换成本模块的副本（buf 马上释放）
        if (s.rec.c1_ok != c1_had) { filtered++; continue; }   // 内存不足拷不下：这条不收，不发布缺 c1 的程序
        if (usable == keep_cap) { filtered++; continue; }   // 本局上限比文件小：更冷的丢掉
        if (usable < keep_cap) disk[usable] = s;
        usable++;
    }
    if (usable) qsort(disk, usable, sizeof *disk, slot_cmp);
    // 事务提交：当前局记录优先，磁盘按热点填空位。重合身份保留当前元数据，
    // uses 取较大值，不把可能已保存过的同一段历史再相加。
    uint32_t cap = g.n + usable;
    if (cap > limit) cap = limit;
    if (cap < g.n) cap = g.n;
    struct slot *merged = cap ? (struct slot *)malloc((size_t)cap * sizeof *merged) : NULL;
    struct nvk_prog_rec *plan = usable ? (struct nvk_prog_rec *)malloc((size_t)usable * sizeof *plan) : NULL;
    struct slot **refs = cap ? (struct slot **)malloc((size_t)cap * sizeof *refs) : NULL;
    if ((cap && (!merged || !refs)) || (usable && !plan)) {
        free(merged); free(plan); free(disk); free(refs);
        pthread_mutex_unlock(&g.mu); free(buf); return 0;
    }
    uint32_t n = g.n, plan_n = 0, loaded = 0;
    if (n) memcpy(merged, g.v, (size_t)n * sizeof *merged);
    int cs = nvn_plat_flag("nvk_compute_warm", 0);
    for (uint32_t i = 0; i < usable; i++) {
        uint32_t at;
        // writer 的磁盘条目身份唯一；只需与 load 前的本局记录查重，
        // 不再扫描刚合入的磁盘项，空本局的常见启动路径保持线性。
        if (!find_locked(disk[i].rec.identity, &at)) {
            if (n >= limit) { filtered++; continue; }
            at = n;
            merged[n++] = disk[i];
        } else if (merged[at].rec.uses < disk[i].rec.uses) {
            merged[at].rec.uses = disk[i].rec.uses;
        }
        // 冻结集仅来自本次接纳的磁盘条目，不把本局新 note 送去额外编译。
        if (cs || merged[at].rec.stage_mask != 32u) plan[plan_n++] = merged[at].rec;
        loaded++;
    }
    // Loaded history and pre-load live notes are distinct domains. Normalize
    // each in order, placing live notes after disk without affecting the plan.
    uint64_t max_rank = n > g.n ? rebase_ranks(merged + g.n, n - g.n, refs, 0) : 0;
    max_rank = rebase_ranks(merged, g.n, refs, max_rank);
    free(refs);
    free(disk);
    free(g.v); g.v = merged; g.n = n; g.cap = cap;
    free(g.plan); g.plan = plan; g.plan_n = plan_n; g.plan_i = 0;
    g.limit = limit; g.max_rank = max_rank;
    g.st.loaded = loaded;
    g.st.rejected = filtered;
    if (from_seed) { g.seed_state = 1; snprintf(g.seed_dir, sizeof g.seed_dir, "%s", seed_dir); }
    // recorded / dirty / 构建与命中统计属于本局，不因读取成功或失败清零。
    pthread_mutex_unlock(&g.mu);
    free(buf);
    {
        char m[224];
        snprintf(m, sizeof m, "程序清单%s：可用 %u 条（上限 %u，过滤 %u 条），热点优先", from_seed ? "（随包种子，本机还没有清单）" : "",
                 (unsigned)loaded, (unsigned)limit, (unsigned)filtered);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "warm", m);
    }
    return (int)loaded;
}

// 只走开局冻结的快照：与 note / 淘汰 / 落盘排序完全解耦（快照不可变，所以这里不需要持锁读内容，
// 只用锁保护游标）。返回 1 = 取到；0 = 本局快照取完。
int nvk_prog_warm_take(struct nvk_prog_rec *out) {
    if (!out) return 0;
    pthread_mutex_lock(&g.mu);
    int have = g.plan_i < g.plan_n;
    if (have) { *out = g.plan[g.plan_i]; g.plan_i++; }
    pthread_mutex_unlock(&g.mu);
    return have;
}

int nvk_prog_warm_pending(void) {
    pthread_mutex_lock(&g.mu);
    uint32_t pending = g.plan_i < g.plan_n ? g.plan_n - g.plan_i : 0;
    pthread_mutex_unlock(&g.mu);
    return (int)pending;
}

int nvk_prog_warm_seed_dir(char *out, size_t n) {
    pthread_mutex_lock(&g.mu);
    int s = g.seed_state;
    if (out && n) { if (s == 1) snprintf(out, n, "%s", g.seed_dir); else out[0] = 0; }
    pthread_mutex_unlock(&g.mu);
    return s;
}

int nvk_prog_warm_contains(const uint8_t identity[32]) {
    if (!identity) return 0;
    pthread_mutex_lock(&g.mu);
    int found = find_locked(identity, NULL);
    pthread_mutex_unlock(&g.mu);
    return found;
}

// ---------------- 记 ----------------
void nvk_prog_warm_note(const struct nvk_prog_rec *rec_in) {
    if (!rec_in || !rec_well_formed(rec_in)) return;
    struct nvk_prog_rec copy = *rec_in; const struct nvk_prog_rec *rec = &copy;
    rec_intern_c1(&copy);   // 调用方的 c1 只在本次调用期间有效：换成本模块的只读副本（锁外做，拷贝不占清单锁）
    pthread_mutex_lock(&g.mu);
    if (!g.cap && ensure_cap_locked(64)) { pthread_mutex_unlock(&g.mu); return; }
    if (!g.limit) g.limit = warm_limit();
    if (g.max_rank == UINT64_MAX) {
        struct slot **refs = g.n ? (struct slot **)malloc((size_t)g.n * sizeof *refs) : NULL;
        if (g.n && !refs) { pthread_mutex_unlock(&g.mu); return; }
        g.max_rank = rebase_ranks(g.v, g.n, refs, 0);
        free(refs);
    }
    uint64_t rank = ++g.max_rank;
    uint32_t at = 0;
    if (find_locked(rec->identity, &at)) {
        struct slot *s = &g.v[at];
        if (s->rec.uses != 0xFFFFFFFFu) s->rec.uses++;
        s->last_use_rank = rank;
        s->rec.stage_mask = rec->stage_mask; s->rec.lp_ok = rec->lp_ok;   // 同身份即同状态：旧清单读来的 0 在这里刷新
        s->rec.lp_ssbo_window = rec->lp_ssbo_window;
        memcpy(s->rec.keys, rec->keys, sizeof s->rec.keys);
        // c1 只在这次快照确实带回来时才覆盖（真实 resolve 有视图；清单加载来的记录没有新视图）。
        for (int st = 0; st < NVK_PROG_REC_STAGES; st++)
            if (rec->c1_ok & (1u << st)) {
                s->rec.c1[st] = rec->c1[st]; s->rec.c1_len[st] = rec->c1_len[st];
                s->rec.c1_ok |= (uint8_t)(1u << st);
            }
        g.st.recorded++;
        g.dirty = 1;
        pthread_mutex_unlock(&g.mu);
        return;
    }
    uint32_t limit = g.limit;
    if (limit < 1) limit = 1;
    if (g.n >= limit) {
        // 满了淘汰最冷的一条（uses 最低；同 uses 最久没用），不拒绝新条目：热点优先。
        uint32_t cold = 0;
        for (uint32_t i = 1; i < g.n; i++) {
            if (g.v[i].rec.uses < g.v[cold].rec.uses ||
                (g.v[i].rec.uses == g.v[cold].rec.uses && g.v[i].last_use_rank < g.v[cold].last_use_rank))
                cold = i;
        }
        g.v[cold] = (struct slot){ .rec = *rec, .last_use_rank = rank };
        g.v[cold].rec.uses = rec->uses ? rec->uses : 1;
        g.st.recorded++;
        g.dirty = 1;
        pthread_mutex_unlock(&g.mu);
        return;
    }
    if (ensure_cap_locked(g.n + 1)) { pthread_mutex_unlock(&g.mu); return; }
    g.v[g.n] = (struct slot){ .rec = *rec, .last_use_rank = rank };
    if (!g.v[g.n].rec.uses) g.v[g.n].rec.uses = 1;
    g.n++;
    g.st.recorded++;
    g.dirty = 1;
    pthread_mutex_unlock(&g.mu);
}

void nvk_prog_warm_stat_build(int rc) {
    pthread_mutex_lock(&g.mu);
    if (rc == 0) g.st.built++;
    else if (rc == 1) g.st.retried++;
    else g.st.failed++;
    pthread_mutex_unlock(&g.mu);
}
void nvk_prog_warm_stat_draw_hit(void) { pthread_mutex_lock(&g.mu); g.st.draw_hits++; pthread_mutex_unlock(&g.mu); }
void nvk_prog_warm_stat_draw_miss(void) { pthread_mutex_lock(&g.mu); g.st.draw_miss++; pthread_mutex_unlock(&g.mu); }

void nvk_prog_warm_stats(struct nvk_prog_warm_stats *out) {
    if (!out) return;
    pthread_mutex_lock(&g.mu);
    *out = g.st;
    out->pending = g.plan_i < g.plan_n ? g.plan_n - g.plan_i : 0;
    pthread_mutex_unlock(&g.mu);
}

void nvk_prog_warm_reset(void) {
    pthread_mutex_lock(&g.mu);
    reset_locked();
    pthread_mutex_unlock(&g.mu);
}

// ---------------- 写 ----------------
static void put_u8(uint8_t **p, uint8_t v) { (*p)[0] = v; *p += 1; }
static void put_u32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); (*p)[2] = (uint8_t)(v >> 16); (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}
static void put_u64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) (*p)[i] = (uint8_t)(v >> (8 * i));
    *p += 8;
}
// 临时文件 → 文件 fsync → rename → 目录 fsync（两次 fsync，最多）。失败返回 -1，目标文件保持旧内容。
static int write_atomic(const char *dir, const char *path, const uint8_t *buf, size_t len) {
    char tmp[NVK_PROG_WARM_MAX_PATH + 32];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof tmp) return -1;
    {   // 目录由自己建（同 nvn_vk_pipe_warm.c pw_write_atomic：程序全从出包预编库取时没有现场编替它建目录）
        char d[NVK_PROG_WARM_MAX_PATH + 32]; snprintf(d, sizeof d, "%s", dir);
        if (mkdir(d, 0700) != 0 && errno == ENOENT) { char *up = strrchr(d, '/'); if (up && up != d) { *up = 0; (void)mkdir(d, 0700); *up = '/'; } (void)mkdir(d, 0700); }
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        if (n == 0) { close(fd); unlink(tmp); errno = EIO; return -1; }
        off += (size_t)n;
    }
    if (fsync(fd)) { close(fd); unlink(tmp); return -1; }
    if (close(fd)) { unlink(tmp); return -1; }
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return -1;              // 目标已可见：如实报失败，不回滚（同 program_cache 的约定）
    int rc = fsync(dfd);
    close(dfd);
    return rc ? -1 : 0;
}

void nvk_prog_warm_save(void) {
    pthread_mutex_lock(&g_save_mu);
    uint8_t *buf = NULL;
    size_t len = 0;
    char dir[NVK_PROG_WARM_MAX_PATH] = "", path[NVK_PROG_WARM_MAX_PATH] = "";
    {
        pthread_mutex_lock(&g.mu);
        if (!g.have_facts) {   // note 可能早于 load（提交线程先记一笔）：落盘时再取一次会话事实
            uint8_t ps[32], cs[32];
            int32_t abi = 0;
            if (!fetch_facts(ps, cs, &abi)) {
                memcpy(g.profile_sha, ps, 32); memcpy(g.closure_sha, cs, 32); g.direct_abi = abi; g.have_facts = 1;
            }
        }
        if (!g.have_facts || !g.dirty || !g.n) { pthread_mutex_unlock(&g.mu); pthread_mutex_unlock(&g_save_mu); return; }
        const char *d = nvn_plat_dir(NVN_PLAT_DIR_CACHE);
        if (!d || !d[0] || snprintf(dir, sizeof dir, "%s", d) >= (int)sizeof dir ||
            warm_path(path, sizeof path, dir, g.closure_sha)) {
            pthread_mutex_unlock(&g.mu);
            pthread_mutex_unlock(&g_save_mu);
            return;
        }
        len = (size_t)NVK_PROG_WARM_HDR_FIELDS + NVK_PROG_WARM_SUM;
        for (uint32_t i = 0; i < g.n; i++) {
            len += NVK_PROG_WARM_ENTRY;
            for (int st = 0; st < NVK_PROG_REC_STAGES; st++) if (g.v[i].rec.c1_ok & (1u << st)) len += pad4(g.v[i].rec.c1_len[st]);
        }
        buf = (uint8_t *)malloc(len);
        if (!buf) { pthread_mutex_unlock(&g.mu); pthread_mutex_unlock(&g_save_mu); nvn_plat_log(NVN_PLAT_LOG_RED, "warm", "【红】程序清单落盘：内存不足，本局不写"); return; }
        // 落盘顺序 = 下局预热带顺序：uses 降序、同 uses 最近用过的先。排的是下标表（副本），不改 g.v。
        uint32_t *idx = (uint32_t *)malloc((size_t)g.n * sizeof *idx);
        if (!idx) {
            free(buf);
            pthread_mutex_unlock(&g.mu);
            pthread_mutex_unlock(&g_save_mu);
            nvn_plat_log(NVN_PLAT_LOG_RED, "warm", "【红】程序清单落盘：排序下标分配失败（保留旧文件，下次再写）");
            return;
        }
        for (uint32_t i = 0; i < g.n; i++) idx[i] = i;
        g_cmp_base = g.v;
        qsort(idx, g.n, sizeof *idx, slot_cmp_idx);
        g_cmp_base = NULL;
        uint8_t *p = buf;
        put_u32(&p, NVK_PROG_WARM_MAGIC);
        put_u32(&p, NVK_PROG_WARM_VERSION);
        put_u32(&p, NVK_PROG_WARM_ENTRY);
        put_u32(&p, g.n);
        memcpy(p, g.profile_sha, 32); p += 32;
        put_u32(&p, (uint32_t)g.direct_abi);
        memcpy(p, g.closure_sha, 8); p += 8;
        for (uint32_t i = 0; i < g.n; i++) {
            const struct slot *s = &g.v[idx[i]];
            memcpy(p, s->rec.identity, 32); p += 32;
            put_u32(&p, s->rec.stage_mask);
            memcpy(p, s->rec.keys, sizeof s->rec.keys); p += sizeof s->rec.keys;
            put_u8(&p, s->rec.c1_ok); put_u8(&p, s->rec.lp_ok); put_u8(&p, s->rec.lp_ssbo_window); put_u8(&p, 0);
            for (int st = 0; st < NVK_PROG_REC_STAGES; st++) put_u32(&p, (s->rec.c1_ok & (1u << st)) ? s->rec.c1_len[st] : 0);
            put_u32(&p, s->rec.uses);
            put_u64(&p, s->last_use_rank);
            for (int st = 0; st < NVK_PROG_REC_STAGES; st++) {
                if (!(s->rec.c1_ok & (1u << st))) continue;
                const uint32_t cl = s->rec.c1_len[st];
                memcpy(p, s->rec.c1[st], cl); memset(p + cl, 0, pad4(cl) - cl); p += pad4(cl);
            }
        }
        free(idx);
        put_u64(&p, fnv1a64(buf, len - NVK_PROG_WARM_SUM));
        g.dirty = 0;   // 先清：写盘期间新的 note 会重新置 1，下一轮再写
        pthread_mutex_unlock(&g.mu);
    }
    int rc = write_atomic(dir, path, buf, len);
    free(buf);
    if (!rc) warm_prune(dir, path);
    pthread_mutex_unlock(&g_save_mu);
    if (rc) {
        pthread_mutex_lock(&g.mu);
        g.dirty = 1;   // 失败：下一轮定期落盘要重试
        pthread_mutex_unlock(&g.mu);
        nvn_plat_log(NVN_PLAT_LOG_RED, "warm", "【红】程序清单落盘失败（保留旧文件，下次再写）");
    }
}
