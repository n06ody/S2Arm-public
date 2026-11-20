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
// 有界：上限 debug.s2a.nvk_warm_max（缺省 8192 条，一条 844 字节，约 6.9 MB）。满了淘汰 uses 最低
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
#include <time.h>
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
#define NVK_PROG_WARM_VERSION 1u
#define NVK_PROG_WARM_HDR_FIELDS 60u        // magic4 版本4 条目大小4 条数4 profile32 ABI4 闭包摘要8
#define NVK_PROG_WARM_SUM 8u                // 末尾 FNV-1a 64
#define NVK_PROG_WARM_ENTRY 844u            // identity32 mask4 keys24 c1_ok1 pad3 c1 768 uses4 last_use8
#define NVK_PROG_WARM_ABS_MAX 16384u        // 上限的硬顶（flag 调再大也不超过它）
#define NVK_PROG_WARM_DEFAULT_MAX 8192      // 缺省上限（真机走一圈 6533 个身份；一条 844 字节，约 6.9 MB）
#define NVK_PROG_WARM_MAX_PATH 640

struct slot {
    struct nvk_prog_rec rec;
    uint64_t last_use_ms;
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
    uint64_t max_ts;
    struct nvk_prog_warm_stats st;
} g = { .mu = PTHREAD_MUTEX_INITIALIZER, .limit = NVK_PROG_WARM_DEFAULT_MAX };
// 落盘串行：预热线程的 30 秒定期写与收尾写不并发（否则两个临时文件抢同一个 rename 目标）。
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t warm_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
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
    return r && stage_count(r->stage_mask) && !(r->c1_ok & ~7u) && !identity_zero(r->identity);
}
static void stats_reset(void) { memset(&g.st, 0, sizeof g.st); }
static void reset_locked(void) {
    free(g.v); g.v = NULL;
    free(g.plan); g.plan = NULL;
    g.n = g.cap = 0;
    g.plan_n = g.plan_i = 0;
    g.limit = 0;   // 下一局（或下一个 load）按当时的 flag 重读上限
    g.dirty = 0; g.have_facts = 0; g.max_ts = 0;
    memset(g.profile_sha, 0, sizeof g.profile_sha); memset(g.closure_sha, 0, sizeof g.closure_sha);
    g.direct_abi = 0;
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
// uses 降序；同 uses 用 last_use 降序（谁最近用过谁先）；再同按 identity 升序，保证同一份清单两次落盘逐字节一致。
// 落盘排序只排一份下标表，不动 g.v：本局实时表可能正被 note / 淘汰改（虽然都在锁里，就地 qsort 会让
// 「这一份快照」和「take 看到的东西」分家，外部复核就是这个 repro 的根因之一）。
static const struct slot *g_cmp_base;
static int slot_cmp_idx(const void *pa, const void *pb) {
    const struct slot *a = &g_cmp_base[*(const uint32_t *)pa], *b = &g_cmp_base[*(const uint32_t *)pb];
    if (a->rec.uses != b->rec.uses) return a->rec.uses < b->rec.uses ? 1 : -1;
    if (a->last_use_ms != b->last_use_ms) return a->last_use_ms < b->last_use_ms ? 1 : -1;
    return memcmp(a->rec.identity, b->rec.identity, 32);
}
static int slot_cmp(const void *pa, const void *pb) {
    const struct slot *a = (const struct slot *)pa, *b = (const struct slot *)pb;
    if (a->rec.uses != b->rec.uses) return a->rec.uses < b->rec.uses ? 1 : -1;
    if (a->last_use_ms != b->last_use_ms) return a->last_use_ms < b->last_use_ms ? 1 : -1;
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
    size_t cap = (size_t)NVK_PROG_WARM_HDR_FIELDS + (size_t)NVK_PROG_WARM_ABS_MAX * NVK_PROG_WARM_ENTRY + NVK_PROG_WARM_SUM;
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
    // 本局有效性事实：拿不到（provider 还没激活）就返回 -1 让调度器重试，不拿“没有清单”糊弄过去。
    const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE);
    if (!dir || !dir[0]) return -1;
    uint8_t profile_sha[32], closure_sha[32];
    int32_t direct_abi = 0;
    if (fetch_facts(profile_sha, closure_sha, &direct_abi)) return -1;
    const int32_t pc_abi = direct_abi;

    char path[NVK_PROG_WARM_MAX_PATH];
    if (warm_path(path, sizeof path, dir, closure_sha)) return 0;
    uint8_t *buf = NULL; size_t len = 0;
    int rf = read_file(path, &buf, &len), e = rf ? errno : 0;
    if (rf && e == ENOENT) {   // 本闭包还没有清单：读旧名那份（升级后第一局；闭包对得上才收，见下面的有效性检查）
        char legacy[NVK_PROG_WARM_MAX_PATH];
        if (snprintf(legacy, sizeof legacy, "%s/%s", dir, NVK_PROG_WARM_FILE) < (int)sizeof legacy) { rf = read_file(legacy, &buf, &len); e = rf ? errno : 0; }
    }
    if (rf) {
        pthread_mutex_lock(&g.mu);
        reset_locked();
        g.have_facts = 1; memcpy(g.profile_sha, profile_sha, 32); g.direct_abi = pc_abi; memcpy(g.closure_sha, closure_sha, 32);
        g.limit = warm_limit();
        pthread_mutex_unlock(&g.mu);
        if (e == ENOENT) {
            const char *nm = strrchr(path, '/');
            char m[200]; snprintf(m, sizeof m, "程序清单：目录里还没有这一版翻译器的清单 %.48s（本局从零记录）", nm ? nm + 1 : path);
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
        uint64_t want = (uint64_t)NVK_PROG_WARM_HDR_FIELDS + (uint64_t)count * NVK_PROG_WARM_ENTRY + NVK_PROG_WARM_SUM;
        if (count > NVK_PROG_WARM_ABS_MAX) bad = "条数超过硬顶";
        else if ((uint64_t)len != want) bad = "长度与条数不符";
        else if (get_u64(buf + len - NVK_PROG_WARM_SUM) != fnv1a64(buf, len - NVK_PROG_WARM_SUM)) bad = "FNV 校验不符";
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
    reset_locked();
    g.have_facts = 1; memcpy(g.profile_sha, profile_sha, 32); g.direct_abi = pc_abi; memcpy(g.closure_sha, closure_sha, 32);
    g.limit = warm_limit();
    if (bad) {
        g.st.rejected = count;
        pthread_mutex_unlock(&g.mu);
        free(buf);
        { char m[288]; snprintf(m, sizeof m, "%s程序清单整份拒绝：%s（%u 条）", mismatch ? "【黄】" : "【红】", bad, (unsigned)count);
          nvn_plat_log(mismatch ? NVN_PLAT_LOG_WARN : NVN_PLAT_LOG_RED, "warm", m); }
        return 0;
    }
    // 逐条：结构合法性；本局上限内按 uses 热点保留。超过本局上限的（上次上限更大）按 rejected 记。
    uint32_t usable = 0, filtered = 0;
    uint32_t keep_cap = count < g.limit ? count : g.limit;
    if (keep_cap && ensure_cap_locked(keep_cap)) { pthread_mutex_unlock(&g.mu); free(buf); return 0; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = buf + NVK_PROG_WARM_HDR_FIELDS + (size_t)i * NVK_PROG_WARM_ENTRY;
        struct slot s;
        memset(&s, 0, sizeof s);
        memcpy(s.rec.identity, e, 32);
        s.rec.stage_mask = get_u32(e + 32);
        memcpy(s.rec.keys, e + 36, sizeof s.rec.keys);
        s.rec.c1_ok = e[60];
        s.rec.lp_ok = e[61];   // 2 = 按含「存储缓冲不对齐」的规则判可用；旧清单里的 1 当不可用
        memcpy(s.rec.c1, e + 64, sizeof s.rec.c1);
        s.rec.uses = get_u32(e + 60 + 4 + 768);
        s.last_use_ms = get_u64(e + 60 + 4 + 768 + 4);
        if (!rec_well_formed(&s.rec)) { filtered++; continue; }
        if (usable == keep_cap) { filtered++; continue; }   // 本局上限比文件小：更冷的丢掉
        if (usable < keep_cap) g.v[usable] = s;
        if (s.last_use_ms > g.max_ts) g.max_ts = s.last_use_ms;
        usable++;
    }
    g.n = usable;
    qsort(g.v, g.n, sizeof g.v[0], slot_cmp);
    // 冻结本局预热集：拿文件顺序（热点降序）的一份值拷贝。之后 g.v 怎么 note / 淘汰 / 重排都影响不到它。
    free(g.plan); g.plan = NULL; g.plan_n = g.plan_i = 0;
    if (g.n) {
        g.plan = (struct nvk_prog_rec *)malloc((size_t)g.n * sizeof *g.plan);
        if (g.plan) { int cs = nvn_plat_flag("nvk_compute_warm", 0); for (uint32_t i = 0; i < g.n; i++) if (cs || g.v[i].rec.stage_mask != 32u) g.plan[g.plan_n++] = g.v[i].rec; }
        else nvn_plat_log(NVN_PLAT_LOG_RED, "warm", "【红】程序清单：本局预热集快照分配失败（本局不预热，清单照常记录）");
    }
    g.st.loaded = g.n;
    g.st.rejected = filtered;
    g.dirty = 0;
    pthread_mutex_unlock(&g.mu);
    free(buf);
    {
        char m[224];
        snprintf(m, sizeof m, "程序清单：可用 %u 条（上限 %u，过滤 %u 条），热点优先", (unsigned)g.st.loaded, (unsigned)g.limit, (unsigned)filtered);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "warm", m);
    }
    return (int)g.st.loaded;
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

int nvk_prog_warm_contains(const uint8_t identity[32]) {
    if (!identity) return 0;
    pthread_mutex_lock(&g.mu);
    int found = find_locked(identity, NULL);
    pthread_mutex_unlock(&g.mu);
    return found;
}

// ---------------- 记 ----------------
void nvk_prog_warm_note(const struct nvk_prog_rec *rec) {
    if (!rec || !rec_well_formed(rec)) return;
    pthread_mutex_lock(&g.mu);
    if (!g.cap && ensure_cap_locked(64)) { pthread_mutex_unlock(&g.mu); return; }
    if (!g.limit) g.limit = warm_limit();
    uint64_t ts = warm_now_ms();
    if (ts <= g.max_ts) ts = g.max_ts + 1;   // 时钟回拨/同一毫秒：保持文件内单调，跨会话排序才有意义
    g.max_ts = ts;
    uint32_t at = 0;
    if (find_locked(rec->identity, &at)) {
        struct slot *s = &g.v[at];
        if (s->rec.uses != 0xFFFFFFFFu) s->rec.uses++;
        s->last_use_ms = ts;
        s->rec.stage_mask = rec->stage_mask; s->rec.lp_ok = rec->lp_ok;   // 同身份即同状态：旧清单读来的 0 在这里刷新
        memcpy(s->rec.keys, rec->keys, sizeof s->rec.keys);
        // c1 只在这次快照确实带回来时才覆盖（真实 resolve 有视图；清单加载来的记录没有新视图）。
        for (int st = 0; st < NVK_PROG_REC_STAGES; st++)
            if (rec->c1_ok & (1u << st)) {
                memcpy(s->rec.c1[st], rec->c1[st], NVK_PROG_REC_C1_BYTES);
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
                (g.v[i].rec.uses == g.v[cold].rec.uses && g.v[i].last_use_ms < g.v[cold].last_use_ms))
                cold = i;
        }
        g.v[cold] = (struct slot){ .rec = *rec, .last_use_ms = ts };
        g.v[cold].rec.uses = rec->uses ? rec->uses : 1;
        g.st.recorded++;
        g.dirty = 1;
        pthread_mutex_unlock(&g.mu);
        return;
    }
    if (ensure_cap_locked(g.n + 1)) { pthread_mutex_unlock(&g.mu); return; }
    g.v[g.n] = (struct slot){ .rec = *rec, .last_use_ms = ts };
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
        len = (size_t)NVK_PROG_WARM_HDR_FIELDS + (size_t)g.n * NVK_PROG_WARM_ENTRY + NVK_PROG_WARM_SUM;
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
            put_u8(&p, s->rec.c1_ok); put_u8(&p, s->rec.lp_ok); p += 2;   // 保留
            memcpy(p, s->rec.c1, sizeof s->rec.c1); p += sizeof s->rec.c1;
            put_u32(&p, s->rec.uses);
            put_u64(&p, s->last_use_ms);
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