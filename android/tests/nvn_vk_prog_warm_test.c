// 程序预热清单的宿主回归（nvn_vk_prog_warm.c）：纯逻辑 + 文件层，无 GPU。
// 覆盖：首次无文件 / 记录去重与 uses / 有界淘汰最冷（含缺省上限 8192）/ uses 热点排序 / 落盘→读回逐字段一致（含 c1）/
//       坏文件整份拒绝（magic·版本·条目大小·条数·长度·FNV·截断）/ 本局有效性整份拒绝（profile·ABI·闭包）/
//       合法条目容量过滤 / 落盘没变化不重写 / 统计记账 / reset。
// 期望值不来自被测实现：uses 与淘汰结果按测试自己构造的顺序手算，文件长度按格式手算（60 + Σ(112 + 各有效阶段 c1 补齐到 4) + 8；mkrec 掩码 3 的条目 = 112 + 2×0x100 = 624）。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // mkdtemp / st_mtim 在严格 -std=c11 下也要可见
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "nvn_vk_prog_warm.h"
#include "nvn_plat.h"
#include "direct/compile_request.h"
#include "direct/sha256.h"
#ifdef __APPLE__   // 文件修改时间字段：Linux / 安卓是 st_mtim，苹果是 st_mtimespec（宿主测试在 macOS 上也编）
#define S2A_ST_MTIM st_mtimespec
#else
#define S2A_ST_MTIM st_mtim
#endif

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---- 平台替身：缓存目录、调试开关、日志 ----
static char g_dir[512];
static int g_warm_max = -1;
static char g_profile[128] = "profile-A";
static char g_closure[64] = "closure-A";
static int32_t g_abi = 7;
static char g_last_red[512], g_last_warn[512], g_last_info[2048];
const char *nvn_plat_dir(int which) { return which == NVN_PLAT_DIR_CACHE && g_dir[0] ? g_dir : NULL; }
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_warm_max")) return g_warm_max >= 0 ? g_warm_max : dflt;
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)cls;
    if (level == NVN_PLAT_LOG_RED) snprintf(g_last_red, sizeof g_last_red, "%s", text);
    if (level == NVN_PLAT_LOG_WARN) snprintf(g_last_warn, sizeof g_last_warn, "%s", text);
    snprintf(g_last_info, sizeof g_last_info, "%s", text);
}
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) {
    if (!g_profile[0] || !g_closure[0]) return -1;
    memset(out, 0, sizeof *out);
    out->session = 1; out->generation = 1; out->profile_json = g_profile; out->profile_bytes = strlen(g_profile);
    out->direct_abi = (uint32_t)g_abi; out->translator_closure = g_closure;
    return 0;
}

// ---- 夹具 ----
static struct nvk_prog_rec mkrec(int n, uint32_t mask) {
    struct nvk_prog_rec r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 32; i++) r.identity[i] = (uint8_t)(n * 17 + i + 1);   // 非零
    r.stage_mask = mask;
    for (int i = 0; i < NVK_PROG_REC_STAGES; i++)
        for (int b = 0; b < 8; b++) r.keys[i][b] = (uint8_t)(n * 7 + i * 3 + b);
    r.c1_ok = (uint8_t)(mask == 32u ? 1u : mask);
    static uint8_t c1tab[256][NVK_PROG_REC_STAGES][NVK_PROG_REC_C1_MIN];   // c1 内容只随 (n*3+i) 的低 8 位变：按 n 的低 8 位共用一份
    for (int i = 0; i < NVK_PROG_REC_STAGES; i++) {
        uint8_t *c = c1tab[n & 255][i]; memset(c, (uint8_t)(n * 3 + i), NVK_PROG_REC_C1_MIN);
        if (r.c1_ok & (1u << i)) { r.c1[i] = c; r.c1_len[i] = NVK_PROG_REC_C1_MIN; }
    }
    return r;
}
static int rec_is(const struct nvk_prog_rec *r, int n) {
    struct nvk_prog_rec x = mkrec(n, r->stage_mask);
    return !memcmp(r->identity, x.identity, 32);
}
// 宽身份：mkrec 的身份只随 n 模 256 变；缺省上限用例要上万个互不相同的身份，把序号写进身份前 4 字节（小端），其余填固定非零字节。
static struct nvk_prog_rec mkwide(uint32_t n) {
    struct nvk_prog_rec r = mkrec(1, 1);
    memset(r.identity, 0x5a, sizeof r.identity);
    r.identity[0] = (uint8_t)n; r.identity[1] = (uint8_t)(n >> 8); r.identity[2] = (uint8_t)(n >> 16); r.identity[3] = (uint8_t)(n >> 24);
    return r;
}
static uint32_t wide_of(const struct nvk_prog_rec *r) {
    return (uint32_t)r->identity[0] | (uint32_t)r->identity[1] << 8 | (uint32_t)r->identity[2] << 16 | (uint32_t)r->identity[3] << 24;
}
static int read_file(const char *path, uint8_t **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -1; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return -1; }
    fclose(f); *out = b; *len = (size_t)n;
    return 0;
}
static int write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(buf, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}
static void dir_reset(void) { g_warm_max = -1; nvk_prog_warm_reset(); }
// 清单路径按闭包命名：programs-v1-<SHA-256(闭包原文) 前 8 字节 hex>.bin（期望值由测试自己算，不调被测实现）
static void closure_path(char *out, size_t n, const char *dir, const char *closure) {
    s2a_sha256 h; uint8_t d[32]; s2a_sha256_init(&h); s2a_sha256_update(&h, closure, strlen(closure)); s2a_sha256_final(&h, d);
    char hx[17]; for (int i = 0; i < 8; i++) snprintf(hx + 2 * i, 3, "%02x", d[i]);
    snprintf(out, n, "%s/programs-v1-%s.bin", dir, hx);
}
static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static void set_mtime(const char *path, time_t t) { struct utimbuf u = { t, t }; utime(path, &u); }
static int load_now(void) { return nvk_prog_warm_load(); }
static void stats_now(struct nvk_prog_warm_stats *s) { nvk_prog_warm_stats(s); }

int main(void) {
    char tmpl[] = "/tmp/nvk-prog-warm-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL, "temp dir");
    if (!dir) return 1;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    char file[600], legacy[600];
    closure_path(file, sizeof file, dir, g_closure);
    snprintf(legacy, sizeof legacy, "%s/programs-v1.bin", dir);

    // 1) 首次没有文件：readiness 事实齐 → 0 条（不是失败），pending 0。
    {
        nvk_prog_warm_reset();
        CHECK(load_now() == 0, "new session without a manifest loads 0 entries");
        struct nvk_prog_warm_stats s; stats_now(&s);
        CHECK(s.loaded == 0 && s.rejected == 0, "empty manifest counts nothing");
        CHECK(nvk_prog_warm_pending() == 0, "empty manifest has nothing pending");
        CHECK(!nvk_prog_warm_contains(mkrec(1, 3).identity), "empty manifest contains nothing");
    }
    // 没有缓存目录 / 上下文没就绪：返回 -1（调度器重试），不是「没有清单」。
    {
        char saved[512]; snprintf(saved, sizeof saved, "%s", g_dir);
        g_dir[0] = 0;
        CHECK(load_now() == -1, "no cache directory answers retry (-1)");
        snprintf(g_dir, sizeof g_dir, "%s", saved);
    }

    // 带 GS 空槽的细分记录：在量产序列化器里写读 TCS、TES 的键和完整 c1。
    {
        dir_reset();
        struct nvk_prog_rec a = mkrec(3, 0x1b);
        memset(a.keys[2], 0, sizeof a.keys[2]);
        nvk_prog_warm_note(&a);
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        struct nvk_prog_rec got = {0};
        CHECK(load_now() == 1 && nvk_prog_warm_take(&got) == 1, "tessellation record round-trips");
        CHECK(got.stage_mask == 0x1b && got.c1_ok == 0x1b && !memcmp(got.keys, a.keys, sizeof a.keys),
              "tessellation record preserves its five NVN slots");
        CHECK(!got.c1[2] && got.c1[3] && got.c1[4] && got.c1[3][0] == 12 && got.c1[4][255] == 13,
              "tessellation record keeps TCS/TES c1 and the GS hole");
    }

    // 2) 记录去重 + uses：同一身份 note 两次 uses=2，不产生两条。
    {
        dir_reset();
        struct nvk_prog_rec a = mkrec(1, 3);
        nvk_prog_warm_note(&a);
        nvk_prog_warm_note(&a);
        struct nvk_prog_warm_stats s; stats_now(&s);
        CHECK(s.recorded == 2, "duplicate note is accepted and counted (uses bump), got %llu", (unsigned long long)s.recorded);
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        CHECK(load_now() == 1, "one identity round-trips through the manifest file");
        struct nvk_prog_rec got;
        CHECK(nvk_prog_warm_take(&got) == 1 && rec_is(&got, 1) && got.uses == 2,
              "round-trip keeps identity and uses (2), got uses=%u", (unsigned)got.uses);
        CHECK(got.c1_ok == 3 && got.c1[0][0] == 3 && got.c1[1][255] == 4, "round-trip keeps c1 bytes per stage");
        CHECK(got.stage_mask == 3 && !memcmp(got.keys[1], a.keys[1], 8), "round-trip keeps stage mask and keys");
        CHECK(nvk_prog_warm_take(&got) == 0, "cursor is exhausted after one take");
    }


    // 2a) 预编库可用位 lp_ok：随清单落盘读回；旧清单（保留字节为 0）读作 0；同身份再 note 时按最新值刷新。
    {
        dir_reset();
        struct nvk_prog_rec a = mkrec(1, 3), b = mkrec(2, 3);
        a.lp_ok = 1; b.lp_ok = 0;
        nvk_prog_warm_note(&a); nvk_prog_warm_note(&b);
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        CHECK(load_now() == 2, "two records round-trip for lp_ok");
        struct nvk_prog_rec g1, g2;
        CHECK(nvk_prog_warm_take(&g1) == 1 && nvk_prog_warm_take(&g2) == 1, "both lp_ok records are taken");
        struct nvk_prog_rec *ga = rec_is(&g1, 1) ? &g1 : &g2, *gb = rec_is(&g1, 1) ? &g2 : &g1;
        CHECK(rec_is(ga, 1) && ga->lp_ok == 1 && !ga->lp_ssbo_window && rec_is(gb, 2) && gb->lp_ok == 0 && !gb->lp_ssbo_window, "lp_ok round-trips per record (%u, %u)", (unsigned)ga->lp_ok, (unsigned)gb->lp_ok);
        b.lp_ok = 2;
        b.lp_ssbo_window = 1;
        nvk_prog_warm_note(&b);   // 实际命中带补偿的库件后刷新。
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        CHECK(load_now() == 2, "records survive the lp_ok refresh");
        CHECK(nvk_prog_warm_take(&g1) == 1 && nvk_prog_warm_take(&g2) == 1, "refreshed records are taken");
        gb = rec_is(&g1, 2) ? &g1 : &g2;
        CHECK(rec_is(gb, 2) && gb->lp_ok == 2 && gb->lp_ssbo_window == 1, "a later note round-trips the required SSBO window");
    }


    // 2b) 无 c1 的 note：真实 draw 命中「已发布」的程序时没有源快照（只有身份与阶段键），这种 note
    //     不得抹掉记录里已有的 c1（预热产品必须与真实解析逐位一致，c1 缺了 rp_draw 会跳过绘制），
    //     uses 照常 ++、keys 照旧保留。
    {
        dir_reset();
        struct nvk_prog_rec a = mkrec(1, 3);            // c1_ok=3，c1[i] 全为 n*3+i
        CHECK(a.c1_ok == 3, "fixture carries c1 for stages 0 and 1");
        nvk_prog_warm_note(&a);
        struct nvk_prog_rec b = mkrec(1, 3);
        b.c1_ok = 0;                                     // 无源快照的 note
        static uint8_t ee[NVK_PROG_REC_C1_MIN]; memset(ee, 0xee, sizeof ee);
        for (int i = 0; i < NVK_PROG_REC_STAGES; i++) { b.c1[i] = ee; b.c1_len[i] = sizeof ee; }   // 有效位为 0：这些字节不该进清单
        nvk_prog_warm_note(&b);
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        CHECK(load_now() == 1, "c1-less note keeps exactly one record for the identity");
        struct nvk_prog_rec got;
        CHECK(nvk_prog_warm_take(&got) == 1 && rec_is(&got, 1), "c1-less note round-trips the identity");
        CHECK(got.uses == 2, "c1-less note still bumps uses (got %u)", (unsigned)got.uses);
        CHECK(got.c1_ok == 3 && got.c1[0][0] == 3 && got.c1[1][255] == 4,
              "c1-less note never erases the recorded c1 (c1_ok=%u, c1[0][0]=%u)", (unsigned)got.c1_ok, (unsigned)got.c1[0][0]);
        CHECK(!got.c1[2] && got.c1_len[2] == 0, "c1-less note does not invent c1 for an unrecorded stage");
        CHECK(!memcmp(got.keys[1], a.keys[1], 8) && got.stage_mask == 3, "c1-less note keeps the recorded keys / stage mask");
    }

    // 2c) 本局预热集冻结（外部复核 P1「冻结了数量，身份集合仍会变化」的 repro）：载入之后 take 的顺序与集合
    //     必须就是文件里那份；运行期的 note、热点重排、淘汰、周期 save 都不许影响它，否则同一身份被取两次、
    //     另一个身份永远漏掉（复核复现的 A→A）。
    {
        dir_reset();
        struct nvk_prog_rec a = mkrec(1, 3), b = mkrec(2, 3), c = mkrec(3, 3);
        for (int i = 0; i < 10; i++) nvk_prog_warm_note(&a);   // uses=10，最热
        for (int i = 0; i < 5; i++) nvk_prog_warm_note(&b);    // uses=5
        nvk_prog_warm_note(&c);                                // uses=1
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        CHECK(load_now() == 3, "frozen: three identities load");
        struct nvk_prog_rec got;
        CHECK(nvk_prog_warm_take(&got) == 1 && rec_is(&got, 1), "frozen: first take = hottest A");
        for (int i = 0; i < 20; i++) nvk_prog_warm_note(&b);   // 运行期：B 变最热
        struct nvk_prog_rec d = mkrec(4, 3);
        nvk_prog_warm_note(&d);                               // 运行期：插入新身份 D
        nvk_prog_warm_save();                                 // 落盘（排序现在只排下标副本，不动实时表）
        CHECK(nvk_prog_warm_take(&got) == 1 && rec_is(&got, 2),
              "frozen: second take must still be B from the loaded plan (got A repeat=%d, D=%d)",
              rec_is(&got, 1), rec_is(&got, 4));
        {   // 集合唯一性：剩下必须恰好是 C 一条，且全程没有重复
            int ntake = 0, dup = 0, ns = 0;
            struct nvk_prog_rec seen[4];
            while (nvk_prog_warm_take(&got)) {
                for (int i = 0; i < ns; i++) if (!memcmp(seen[i].identity, got.identity, 32)) dup = 1;
                if (ns < 4) seen[ns++] = got;
                ntake++;
            }
            CHECK(ntake == 1 && !dup && ns == 1 && rec_is(&seen[0], 3),
                  "frozen: exactly C left, no duplicates (ntake=%d dup=%d)", ntake, dup);
        }
        CHECK(nvk_prog_warm_contains(d.identity), "frozen: the run-time note still lands in the live table (next session)");
    }

    // 3) 有界：上限 3 时淘汰最冷的一条（uses 最低，同 uses 最久没用），不拒绝新条目。
    {
        dir_reset();
        g_warm_max = 3;
        for (int n = 1; n <= 2; n++) { struct nvk_prog_rec r = mkrec(1, 1); nvk_prog_warm_note(&r); }   // A uses=2
        for (int n = 2; n <= 5; n++) { struct nvk_prog_rec r = mkrec(n, 1); nvk_prog_warm_note(&r); }   // B..E
        struct nvk_prog_rec a = mkrec(1, 1), b = mkrec(2, 1), c = mkrec(3, 1), d = mkrec(4, 1), e = mkrec(5, 1);
        CHECK(nvk_prog_warm_contains(a.identity), "hottest entry (uses=2) survives eviction");
        CHECK(nvk_prog_warm_contains(e.identity) && nvk_prog_warm_contains(d.identity), "newest entries survive eviction");
        CHECK(!nvk_prog_warm_contains(b.identity) && !nvk_prog_warm_contains(c.identity), "coldest entries are evicted");
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        g_warm_max = -1;
        int nloaded = load_now();
        CHECK(nloaded == 3, "bounded manifest keeps exactly the limit, got %d", nloaded);
        struct nvk_prog_rec got; int order_ok = 1; int expect[3] = {1, 5, 4};   // uses 降序，再按最近使用降序
        for (int i = 0; i < 3; i++) { if (!nvk_prog_warm_take(&got) || !rec_is(&got, expect[i])) order_ok = 0; }
        CHECK(order_ok, "hot order: uses desc, then most-recently-used (expected 1,5,4)");
        CHECK(nvk_prog_warm_pending() == 0, "take cursor drives pending to 0");
    }

    // 3b) 缺省上限 8192（#1325）：没设 nvk_warm_max 时清单留 8192 条，挤掉的是最冷的。
    //     期望手算：热组 1..100 各记 3 次（uses 3），冷组 101..9000 各记 1 次（8900 条）。前 8192 条进表后表满
    //     （100 热 + 冷 101..8192），之后每进一条冷的就挤掉冷里最老的一条：8193..9000 共 808 条进来，挤掉 101..908；
    //     留下 100 热 + 冷 909..9000（8092 条）= 8192 条。落盘 → 读回逐条核对集合，热的排在最前。
    //     变异体：缺省回到 4096 → 读回 4096 条；缺省抬到硬顶 16384 → 读回 9000 条；两个都红。
    {
        dir_reset();   // g_warm_max = -1：nvn_plat_flag 返回代码里传进来的缺省值
        for (uint32_t n = 1; n <= 100; n++) { struct nvk_prog_rec r = mkwide(n); for (int k = 0; k < 3; k++) nvk_prog_warm_note(&r); }
        for (uint32_t n = 101; n <= 9000; n++) { struct nvk_prog_rec r = mkwide(n); nvk_prog_warm_note(&r); }
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        int nloaded = load_now();
        CHECK(nloaded == 8192, "default limit keeps exactly 8192 entries, got %d", nloaded);
        static unsigned char seen[9001];
        memset(seen, 0, sizeof seen);
        int dup = 0, hot_first = 1, taken = 0;
        struct nvk_prog_rec got;
        while (nvk_prog_warm_take(&got)) {
            uint32_t n = wide_of(&got);
            if (n < 1 || n > 9000) { dup = -1; break; }
            if (seen[n]) dup++;
            seen[n] = 1;
            if (taken < 100 && n > 100) hot_first = 0;   // uses 降序：前 100 条必须是热组
            taken++;
        }
        CHECK(dup == 0 && taken == 8192, "take hands out 8192 distinct identities (dup %d, taken %d)", dup, taken);
        int hot_ok = 1, evicted_ok = 1, kept_ok = 1;
        for (uint32_t n = 1; n <= 100; n++) if (!seen[n]) hot_ok = 0;
        for (uint32_t n = 101; n <= 908; n++) if (seen[n]) evicted_ok = 0;
        for (uint32_t n = 909; n <= 9000; n++) if (!seen[n]) kept_ok = 0;
        CHECK(hot_ok, "all 100 hot entries (uses 3) survive the default limit");
        CHECK(evicted_ok, "the 808 oldest cold entries (101..908) are the ones evicted");
        CHECK(kept_ok, "the newest 8092 cold entries (909..9000) are kept");
        CHECK(hot_first, "hot entries are handed out first (uses desc)");
    }

    // 4) 落盘格式与「没变化不重写」：文件长度按格式手算；dirty 清掉后 save 不碰文件。
    {
        nvk_prog_warm_reset();
        for (int n = 1; n <= 2; n++) { struct nvk_prog_rec r = mkrec(n, 3); nvk_prog_warm_note(&r); }
        nvk_prog_warm_save();
        struct stat st1;
        CHECK(stat(file, &st1) == 0, "manifest file exists after save");
        CHECK((uint64_t)st1.st_size == 60ull + 2ull * 624ull + 8ull, "file size = 60 + n*624 + 8, got %lld", (long long)st1.st_size);
        struct timespec nap = {0, 30 * 1000000}; nanosleep(&nap, NULL);
        nvk_prog_warm_save();   // 没有变化：直接返回，不动文件
        struct stat st2;
        CHECK(stat(file, &st2) == 0, "manifest file still there");
        CHECK(st1.st_ino == st2.st_ino && st1.S2A_ST_MTIM.tv_sec == st2.S2A_ST_MTIM.tv_sec &&
              st1.S2A_ST_MTIM.tv_nsec == st2.S2A_ST_MTIM.tv_nsec, "save without changes does not rewrite the file");
        struct nvk_prog_rec r = mkrec(3, 3);
        nvk_prog_warm_note(&r);
        nvk_prog_warm_save();
        struct stat st3;
        CHECK(stat(file, &st3) == 0 && (uint64_t)st3.st_size == 60ull + 3ull * 624ull + 8ull,
              "save after a note rewrites the manifest");
    }

    // 5) 坏文件整份拒绝：magic / 版本 / 条目大小 / 条数 / 长度 / FNV / 截断。
    {
        uint8_t *good = NULL; size_t glen = 0;
        CHECK(read_file(file, &good, &glen) == 0 && glen == 60 + 3 * 624 + 8, "good manifest is readable");
        if (!good || glen != 60 + 3 * 624 + 8) { printf("nvn_vk_prog_warm: cannot continue without a good manifest (%d checks, %d failed)\n", checks, failures); free(good); return 1; }
        struct { const char *name; size_t len; size_t at; uint8_t v; } cases[] = {
            { "magic", glen, 0, 'X' },
            { "version", glen, 4, 2 },
            { "entry size", glen, 8, 0x41 },
            { "count", glen, 12, 4 },
            { "fnv", glen, 0, 0 },   // 特殊：改最后一个字节（下面单独处理）
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            uint8_t *bad = (uint8_t *)malloc(glen);
            memcpy(bad, good, glen);
            if (!strcmp(cases[i].name, "fnv")) bad[glen - 1] ^= 0xFF;
            else bad[cases[i].at] = cases[i].v;
            CHECK(write_file(file, bad, glen) == 0, "%s corruption written", cases[i].name);
            free(bad);
            nvk_prog_warm_reset();
            CHECK(load_now() == 0, "corrupt manifest (%s) is rejected as a whole", cases[i].name);
            struct nvk_prog_warm_stats s; stats_now(&s);
            CHECK(s.loaded == 0, "corrupt manifest (%s) loads 0 entries", cases[i].name);
            CHECK(g_last_red[0] != 0, "corrupt manifest (%s) reports a red line", cases[i].name);
        }
        // 截断与超长
        {
            uint8_t *bad = (uint8_t *)malloc(glen);
            memcpy(bad, good, glen);
            CHECK(write_file(file, bad, glen - 1) == 0, "truncated manifest written");
            nvk_prog_warm_reset();
            CHECK(load_now() == 0, "truncated manifest is rejected");
            uint8_t *longer = (uint8_t *)calloc(glen + 624, 1);
            memcpy(longer, good, glen);
            CHECK(write_file(file, longer, glen + 624) == 0, "oversized manifest written");
            nvk_prog_warm_reset();
            CHECK(load_now() == 0, "manifest longer than its entry count claims is rejected");
            free(longer);
            free(bad);
        }
        // 旧格式（版本 1：每阶段 c1 定长 0x100、条目 844 字节）：整份按「版本不符」拒收，不按版本 3 的变长条目误读；
        //   本局照常记，落盘写成版本 3（文件头第 4 字节起 = 3），下一局按版本 3 读回。
        //   期望值手算：版本 1 文件 = 60 + 2×844 + 8 = 1756 字节，头 magic/版本 1/条目 844/条数 2，FNV 按宿主同一公式算对（只有版本不同）。
        {
            size_t olen = 60 + 2 * 844 + 8; uint8_t *old = (uint8_t *)calloc(olen, 1);
            memcpy(old, good, 60);
            old[4] = 1; old[5] = old[6] = old[7] = 0; old[8] = 844 & 0xff; old[9] = 844 >> 8; old[10] = old[11] = 0; old[12] = 2; old[13] = old[14] = old[15] = 0;
            for (int k = 0; k < 2; k++) { struct nvk_prog_rec r = mkrec(k + 1, 3); uint8_t *e = old + 60 + (size_t)k * 844;
                memcpy(e, r.identity, 32); e[32] = 3; memcpy(e + 36, r.keys, 24); e[60] = 3; e[61] = 2; memset(e + 64, 0x77, 768); e[832] = 5; }
            uint64_t h = 1469598103934665603ull; for (size_t k = 0; k < olen - 8; k++) { h ^= old[k]; h *= 1099511628211ull; }
            for (int k = 0; k < 8; k++) old[olen - 8 + (size_t)k] = (uint8_t)(h >> (8 * k));
            CHECK(write_file(file, old, olen) == 0, "version-1 manifest written");
            free(old);
            nvk_prog_warm_reset(); g_last_red[0] = 0;
            CHECK(load_now() == 0, "version-1 manifest is rejected as a whole (not misread as version 3)");
            struct nvk_prog_warm_stats s; stats_now(&s);
            CHECK(s.loaded == 0 && strstr(g_last_red, "版本不符"), "version-1 manifest: loaded=%llu red=%s", (unsigned long long)s.loaded, g_last_red);
            struct nvk_prog_rec r = mkrec(9, 3); nvk_prog_warm_note(&r); nvk_prog_warm_save();
            uint8_t *nb = NULL; size_t nl = 0;
            CHECK(read_file(file, &nb, &nl) == 0 && nl == 60 + 624 + 8 && nb[4] == 3 && nb[8] == 112, "after a version-1 file, save rewrites it as version 3 (len=%zu ver=%u)", nl, nb ? nb[4] : 0);
            free(nb);
            nvk_prog_warm_reset();
            CHECK(load_now() == 1, "the rewritten version-3 manifest loads back");
        }
        // c1 整段往返（八方旅人调色 LUT 的片元 c1 是 2304 字节）：期望 片元段长 2304、首尾字节原样，文件 = 60 + (112 + 0x100 + 2304) + 8。
        {
            static uint8_t big[2304]; for (int k = 0; k < 2304; k++) big[k] = (uint8_t)(k * 13 + 1);
            nvk_prog_warm_reset(); dir_reset();
            struct nvk_prog_rec r = mkrec(11, 3); r.c1[1] = big; r.c1_len[1] = sizeof big;
            nvk_prog_warm_note(&r);
            big[0] ^= 0xff;   // note 必须拷了自己的副本：调用方改自己的缓冲不影响清单
            nvk_prog_warm_save();
            struct stat stb; CHECK(stat(file, &stb) == 0 && (uint64_t)stb.st_size == 60ull + 112ull + 0x100ull + 2304ull + 8ull, "long c1 file size (got %lld)", (long long)stb.st_size);
            nvk_prog_warm_reset();
            CHECK(load_now() == 1, "long c1 manifest loads back");
            struct nvk_prog_rec got;
            big[0] ^= 0xff;
            CHECK(nvk_prog_warm_take(&got) == 1 && got.c1_len[1] == 2304 && got.c1[1] && !memcmp(got.c1[1], big, 2304) && got.c1_len[0] == 0x100,
                  "long c1 round-trips whole (len=%u)", (unsigned)got.c1_len[1]);
            nvk_prog_warm_reset(); dir_reset();
            for (int n = 1; n <= 3; n++) { struct nvk_prog_rec q = mkrec(n, 3); nvk_prog_warm_note(&q); }
            nvk_prog_warm_save();   // 后面的用例要那份 3 条的好文件
        }
        // 恢复好文件（后面的用例要用）
        CHECK(write_file(file, good, glen) == 0, "good manifest restored");
        free(good);
    }

    // 6) 本局有效性。profile / directABI 变：本闭包这份整份拒绝（打黄、不打红），原因各报各的。
    //    闭包变：清单按闭包分文件，新闭包没有自己的清单 → 从零记录、不拒绝、旧闭包那份不动。
    {
        for (int i = 0; i < 2; i++) {
            char saved_profile[128]; snprintf(saved_profile, sizeof saved_profile, "%s", g_profile);
            int32_t saved_abi = g_abi;
            if (i == 0) g_profile[0] = (char)(g_profile[0] ^ 1);        // profile 原文变一个字节（同一版翻译器）
            if (i == 1) g_abi = saved_abi + 1;
            nvk_prog_warm_reset(); g_last_red[0] = g_last_warn[0] = 0;
            CHECK(load_now() == 0, "manifest from another session (%s) is rejected whole", i == 0 ? "profile" : "ABI");
            struct nvk_prog_warm_stats s; stats_now(&s);
            CHECK(s.loaded == 0 && s.rejected == 0, "whole-file mismatch does not change entry counters (%s), loaded=%llu rejected=%llu",
                  i == 0 ? "profile" : "ABI", (unsigned long long)s.loaded, (unsigned long long)s.rejected);
            CHECK(strstr(g_last_warn, i == 0 ? "设备 profile 不同" : "directABI 不同") && !g_last_red[0],
                  "%s mismatch is a yellow line with its own reason: warn=%s red=%s", i == 0 ? "profile" : "ABI", g_last_warn, g_last_red);
            snprintf(g_profile, sizeof g_profile, "%s", saved_profile);
            g_abi = saved_abi;
        }
        {   // 换闭包：新闭包的文件名不同，没有就从零记录；旧闭包那份留着
            char saved_closure[64]; snprintf(saved_closure, sizeof saved_closure, "%s", g_closure);
            g_closure[0] = (char)(g_closure[0] ^ 1);
            char other[600]; closure_path(other, sizeof other, dir, g_closure);
            nvk_prog_warm_reset(); g_last_red[0] = g_last_warn[0] = 0;
            CHECK(load_now() == 0, "another closure has no manifest of its own yet");
            struct nvk_prog_warm_stats s; stats_now(&s);
            CHECK(s.loaded == 0 && s.rejected == 0 && !g_last_red[0] && !g_last_warn[0], "new closure starts empty without a rejection (rejected=%llu red=%s warn=%s)",
                  (unsigned long long)s.rejected, g_last_red, g_last_warn);
            CHECK(strstr(g_last_info, "还没有这一版翻译器的清单") && strstr(g_last_info, strrchr(other, '/') + 1), "missing manifest line names this closure's file: %s", g_last_info);
            CHECK(exists(file) && !exists(other), "the previous closure's manifest is left alone");
            snprintf(g_closure, sizeof g_closure, "%s", saved_closure);
        }
        {   // 旧名文件（一款一份，另一版闭包记的）碰上新闭包且 profile 也不同（真机上换 APK 时 profile 原文里的闭包也跟着变）：
            //   原因必须报闭包，不能报「设备 profile」
            uint8_t *good = NULL; size_t glen = 0;
            CHECK(read_file(file, &good, &glen) == 0 && write_file(legacy, good, glen) == 0, "legacy-named manifest from closure A written");
            free(good);
            char saved_profile[128], saved_closure[64];
            snprintf(saved_profile, sizeof saved_profile, "%s", g_profile); snprintf(saved_closure, sizeof saved_closure, "%s", g_closure);
            g_profile[0] = (char)(g_profile[0] ^ 1); g_closure[0] = (char)(g_closure[0] ^ 1);
            nvk_prog_warm_reset(); g_last_red[0] = g_last_warn[0] = 0;
            CHECK(load_now() == 0, "legacy manifest of another closure is rejected");
            struct nvk_prog_warm_stats s; stats_now(&s);
            CHECK(s.loaded == 0 && s.rejected == 0, "whole-file closure mismatch does not change entry counters");
            CHECK(strstr(g_last_warn, "翻译器闭包不同") && !strstr(g_last_warn, "设备 profile") && !g_last_red[0],
                  "closure is reported before profile: %s", g_last_warn);
            snprintf(g_profile, sizeof g_profile, "%s", saved_profile); snprintf(g_closure, sizeof g_closure, "%s", saved_closure);
            unlink(legacy);
        }
        nvk_prog_warm_reset();
        CHECK(load_now() == 3, "same session facts accept the manifest again");
    }

    // 7) 有效清单的容量过滤才计逐条 rejected。用生产 save 写三条，再用上限 2 加载。
    {
        dir_reset();
        struct nvk_prog_rec hot = mkrec(1, 3), warm = mkrec(2, 3), cold = mkrec(3, 3);
        hot.uses = 9; warm.uses = 5; cold.uses = 1;
        nvk_prog_warm_note(&cold); nvk_prog_warm_note(&warm); nvk_prog_warm_note(&hot);
        nvk_prog_warm_save();
        nvk_prog_warm_reset();
        g_warm_max = 2;
        CHECK(load_now() == 2, "valid manifest accepts two entries at the smaller limit");
        struct nvk_prog_warm_stats s; stats_now(&s);
        CHECK(s.loaded == 2 && s.rejected == 1, "capacity-dropped entry counts as rejected, loaded=%llu rejected=%llu",
              (unsigned long long)s.loaded, (unsigned long long)s.rejected);
        struct nvk_prog_rec got;
        CHECK(nvk_prog_warm_take(&got) == 1 && rec_is(&got, 1), "capacity keeps hottest entry first");
        CHECK(nvk_prog_warm_take(&got) == 1 && rec_is(&got, 2), "capacity keeps second-hottest entry");
        CHECK(!nvk_prog_warm_contains(cold.identity) && nvk_prog_warm_take(&got) == 0, "cold entry excluded from table and plan");
        g_warm_max = -1;
    }

    // 8) 统计记账 + reset。
    {
        nvk_prog_warm_reset();
        struct nvk_prog_warm_stats s; stats_now(&s);
        CHECK(s.loaded == 0 && s.recorded == 0 && s.built == 0 && s.failed == 0 && s.draw_hits == 0 && s.draw_miss == 0,
              "reset clears the session counters");
        nvk_prog_warm_stat_build(0);
        nvk_prog_warm_stat_build(1);
        nvk_prog_warm_stat_build(-1);
        nvk_prog_warm_stat_draw_hit();
        nvk_prog_warm_stat_draw_miss();
        stats_now(&s);
        CHECK(s.built == 1 && s.retried == 1 && s.failed == 1 && s.draw_hits == 1 && s.draw_miss == 1,
              "build / draw counters are recorded (built=%llu retried=%llu failed=%llu hits=%llu miss=%llu)",
              (unsigned long long)s.built, (unsigned long long)s.retried, (unsigned long long)s.failed,
              (unsigned long long)s.draw_hits, (unsigned long long)s.draw_miss);
        nvk_prog_warm_reset();
        stats_now(&s);
        CHECK(s.built == 0 && s.draw_hits == 0, "reset clears build / draw counters too");
    }

    // 9) 同局缺文件不清场；同进程第二局由会话 reset 清掉上一局条目。
    {
        dir_reset();
        struct nvk_prog_rec a = mkrec(1, 1);
        nvk_prog_warm_note(&a);
        CHECK(nvk_prog_warm_contains(a.identity), "note is visible in this session");
        unlink(file);
        CHECK(load_now() == 0 && nvk_prog_warm_contains(a.identity), "same session: missing file preserves current note");
        nvk_prog_warm_reset();
        CHECK(load_now() == 0, "next session: missing file loads 0");
        CHECK(!nvk_prog_warm_contains(a.identity), "next session: previous session's note is cleared");
    }

    // 10) A/B 交替（两臂闭包不同）：各自的清单各自留着，换回来还在；第三个闭包落盘后只留最近 2 份，旧名文件删掉。
    {
        char ca[64], cb[64], cc[64], pa[600], pb[600], pc[600];
        snprintf(ca, sizeof ca, "%s", g_closure); snprintf(cb, sizeof cb, "closure-B"); snprintf(cc, sizeof cc, "closure-C");
        closure_path(pa, sizeof pa, dir, ca); closure_path(pb, sizeof pb, dir, cb); closure_path(pc, sizeof pc, dir, cc);
        snprintf(g_closure, sizeof g_closure, "%s", ca); dir_reset();
        for (int n = 1; n <= 3; n++) { struct nvk_prog_rec r = mkrec(n, 3); nvk_prog_warm_note(&r); }
        nvk_prog_warm_save();
        snprintf(g_closure, sizeof g_closure, "%s", cb); nvk_prog_warm_reset();
        CHECK(load_now() == 0, "arm B starts without A's manifest");
        for (int n = 4; n <= 5; n++) { struct nvk_prog_rec r = mkrec(n, 3); nvk_prog_warm_note(&r); }
        nvk_prog_warm_save();
        CHECK(exists(pa) && exists(pb), "both arms keep their own manifest");
        snprintf(g_closure, sizeof g_closure, "%s", ca); nvk_prog_warm_reset();
        CHECK(load_now() == 3, "back on arm A: its 3 entries are still there");
        snprintf(g_closure, sizeof g_closure, "%s", cb); nvk_prog_warm_reset();
        CHECK(load_now() == 2, "back on arm B: its 2 entries are still there");
        set_mtime(pa, time(NULL) - 3600); set_mtime(pb, time(NULL) - 60);   // A 最旧、B 较新
        CHECK(write_file(legacy, (const uint8_t *)"x", 1) == 0, "stray legacy-named file");
        snprintf(g_closure, sizeof g_closure, "%s", cc); nvk_prog_warm_reset();
        CHECK(load_now() == 0, "arm C starts empty");
        { struct nvk_prog_rec r = mkrec(6, 3); nvk_prog_warm_note(&r); }
        nvk_prog_warm_save();
        CHECK(exists(pc) && exists(pb) && !exists(pa), "after C saves: C and the newest other (B) stay, the oldest (A) is pruned (a=%d b=%d c=%d)", exists(pa), exists(pb), exists(pc));
        CHECK(!exists(legacy), "the legacy-named file is removed after a per-closure save");
        unlink(pb); unlink(pc);
        snprintf(g_closure, sizeof g_closure, "%s", ca);
    }
    // 11) 升级过渡：只有旧名文件、闭包与本局相同 → 照收；落盘后换成按闭包命名的文件。
    {
        dir_reset();
        for (int n = 1; n <= 2; n++) { struct nvk_prog_rec r = mkrec(n, 3); nvk_prog_warm_note(&r); }
        nvk_prog_warm_save();
        CHECK(rename(file, legacy) == 0 && !exists(file), "manifest moved to the legacy name");
        nvk_prog_warm_reset();
        CHECK(load_now() == 2, "legacy-named manifest of the same closure is accepted");
        { struct nvk_prog_rec r = mkrec(3, 3); nvk_prog_warm_note(&r); }
        nvk_prog_warm_save();
        CHECK(exists(file) && !exists(legacy), "save writes the per-closure name and drops the legacy one");
        unlink(file);
    }
    printf("nvn_vk_prog_warm: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}