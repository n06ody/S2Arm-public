// F5.1 宿主回归：跨局编译结果缓存。真机只用同一份实现，这里验数据面。
//
// 反例基线：把“未命中”当成功、只比文件名不比内部身份、只比 identity 不比 result 摘要、
// 读半截就返回，任一情况下调用方都会拿到不可信结果。逐项构造坏文件，必须全部被拒。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "direct/program_cache.h"
#include "direct/sha256.h"

static int fails, checks;
#define CHECK(cond, what) do { checks++; if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7u + (i >> 3));
}

static char g_dir[512];

static int count_entries(void) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "ls -1 '%s' | wc -l", g_dir);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    int n = -1;
    if (fscanf(f, "%d", &n) != 1) n = -1;
    pclose(f);
    return n;
}

static void path_for(const uint8_t id[32], char *path, size_t size) {
    char hex[65];
    static const char *digits = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = digits[id[i] >> 4];
        hex[i * 2 + 1] = digits[id[i] & 15];
    }
    hex[64] = 0;
    snprintf(path, size, "%s/%s.program", g_dir, hex);
}

static int temp_entries(void) {
    DIR *dir = opendir(g_dir);
    if (!dir) return -1;
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (!strncmp(entry->d_name, ".tmp-", 5)) n++;
    closedir(dir);
    return n;
}

static void check_payload(const uint8_t id[32], const uint8_t *expected, size_t len) {
    uint8_t *got = NULL;
    size_t size = 0;
    CHECK(s2a_program_cache_load(g_dir, id, &got, &size, NULL, NULL, 0) == 0,
          "failure-path target remains a complete readable entry");
    CHECK(got && size == len && !memcmp(got, expected, len),
          "target bytes match the independent expected payload");
    free(got);
}

static void sync_failure_tests(void) {
    uint8_t old[64], next[64], id[32];
    fill(old, sizeof old, 19);
    fill(next, sizeof next, 23);
    char path[640], why[256];
    for (int fault = 1; fault <= 4; fault++) {
        for (int existing = 0; existing <= 1; existing++) {
            fill(id, sizeof id, (uint8_t)(90 + fault * 2 + existing));
            path_for(id, path, sizeof path);
            s2a_program_cache_test_fault(0);
            if (existing) CHECK(s2a_program_cache_store(g_dir, id, old, sizeof old, NULL, 0) == 0,
                                "prepare an existing complete target");
            uint64_t failures = s2a_program_cache_failures(), stores = s2a_program_cache_stores();
            s2a_program_cache_test_fault(fault);
            why[0] = 0;
            int rc = s2a_program_cache_store(g_dir, id, next, sizeof next, why, sizeof why);
            printf("sync fault=%d existing=%d rc=%d hits=%u stores=%llu failures=%llu\n",
                   fault, existing, rc, s2a_program_cache_test_fault_hits(),
                   (unsigned long long)(s2a_program_cache_stores() - stores),
                   (unsigned long long)(s2a_program_cache_failures() - failures));
            CHECK(s2a_program_cache_test_fault_hits() == 1, "requested fsync failure is actually consumed");
            CHECK(rc == -1, "fsync failure is not reported as success");
            CHECK(why[0] != 0, "fsync failure has a diagnostic");
            CHECK(s2a_program_cache_stores() == stores, "failed persistence does not count as a store");
            CHECK(s2a_program_cache_failures() == failures + 1, "synchronous failure is counted once");
            CHECK(temp_entries() == 0, "failure leaves no temporary file");
            s2a_program_cache_test_fault(0);
            if (fault <= 2) {
                if (existing) check_payload(id, old, sizeof old);
                else CHECK(access(path, F_OK) != 0, "file fsync failure does not publish a new target");
            } else {
                // 目录同步在 rename 之后；返回失败不等于目标不可见，也不回滚。
                check_payload(id, next, sizeof next);
            }
            CHECK(s2a_program_cache_store(g_dir, id, next, sizeof next, NULL, 0) == 0,
                  "a later independent write succeeds with faults disabled");
        }
    }

    fill(id, sizeof id, 120);
    path_for(id, path, sizeof path);
    CHECK(mkdir(path, 0700) == 0, "prepare a directory that rejects rename");
    uint64_t failures = s2a_program_cache_failures(), stores = s2a_program_cache_stores();
    CHECK(s2a_program_cache_store(g_dir, id, next, sizeof next, NULL, 0) == -1, "rename failure is reported");
    CHECK(s2a_program_cache_failures() == failures + 1, "rename failure is counted once");
    CHECK(s2a_program_cache_stores() == stores, "rename failure is not a successful store");
    CHECK(temp_entries() == 0, "rename failure cleans its temporary file");
    CHECK(rmdir(path) == 0, "rename failure preserved the target directory");

    // 异步写失败仍须 drain 完成，不能阻断或改写调用方的内存结果。
    for (int fault = 1; fault <= 3; fault += 2) {
        fill(id, sizeof id, (uint8_t)(130 + fault));
        path_for(id, path, sizeof path);
        failures = s2a_program_cache_failures(); stores = s2a_program_cache_stores();
        s2a_program_cache_test_fault(fault);
        CHECK(s2a_program_cache_store_async(g_dir, id, next, sizeof next) == 0, "async work is accepted");
        CHECK(s2a_program_cache_drain(10000) == 0, "failed async write is drained");
        CHECK(s2a_program_cache_test_fault_hits() == 1, "async fault is consumed");
        CHECK(s2a_program_cache_failures() > failures, "async failure remains observable");
        CHECK(s2a_program_cache_stores() == stores, "async failure is not a successful store");
        CHECK(temp_entries() == 0, "async failure leaves no temporary file");
        for (size_t i = 0; i < sizeof next; i++)
            CHECK(next[i] == (uint8_t)(23 + i * 7u + (i >> 3)), "async write preserves caller bytes");
        s2a_program_cache_test_fault(0);
        if (fault == 1) CHECK(access(path, F_OK) != 0, "failed async file sync does not publish");
        else check_payload(id, next, sizeof next);
    }
}

// 只读头（s2a_program_cache_probe，结果合并 #1355 用）：给出的结果摘要 / 负载长度必须与正式读取一致；
// 不动命中 / 未命中 / 失败三个计数；坏头逐项拒绝；负载被改时头仍可读（探针不校验负载，是合同的一部分），正式读取照旧拒绝。
static void independent_sha(const uint8_t *p, size_t n, uint8_t out[32]) {
    s2a_sha256 c;
    s2a_sha256_init(&c);
    s2a_sha256_update(&c, p, n);
    s2a_sha256_final(&c, out);
}
static void probe_tests(void) {
    uint8_t id[32], other[32], zero[32] = {0}, sha[32], want[32], lsha[32];
    static uint8_t payload[777];
    fill(id, sizeof id, 201);      // 身份种子避开本文件其它用例用过的（1 / 2 / 77 / 92..99 / 131 / 133），免得串到它们的文件
    fill(other, sizeof other, 203);
    fill(payload, sizeof payload, 41);
    independent_sha(payload, sizeof payload, want);
    char path[640], why[256] = "";
    path_for(id, path, sizeof path);
    uint64_t plen = 99;

    // 未命中：1（与「文件坏」的 -1 分开），输出清零，原因「未命中」，不计 misses / failures
    {
        uint64_t m0 = s2a_program_cache_misses(), f0 = s2a_program_cache_failures();
        memset(sha, 0xAA, sizeof sha);
        CHECK(s2a_program_cache_probe(g_dir, id, sha, &plen, why, sizeof why) == 1, "probe of a missing entry returns 1 (no file), not -1 (bad file)");
        CHECK(!memcmp(sha, zero, 32) && plen == 0, "probe miss leaves outputs zeroed");
        CHECK(strstr(why, "未命中") != NULL, "probe miss is reported as a miss");
        CHECK(s2a_program_cache_misses() == m0 && s2a_program_cache_failures() == f0, "probe miss touches neither miss nor failure counters");
    }

    // 往返：摘要 = 独立算出的负载 SHA-256，长度 = 负载字节数，与正式读取给出的已校验摘要一致；计数不动
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "probe fixture stored");
    {
        uint64_t h0 = s2a_program_cache_hits(), m0 = s2a_program_cache_misses(), f0 = s2a_program_cache_failures();
        CHECK(s2a_program_cache_probe(g_dir, id, sha, &plen, why, sizeof why) == 0, "probe of a stored entry hits");
        CHECK(!memcmp(sha, want, 32), "probe digest equals the independently computed payload SHA-256");
        CHECK(plen == sizeof payload, "probe length equals the payload size");
        CHECK(s2a_program_cache_hits() == h0 && s2a_program_cache_misses() == m0 && s2a_program_cache_failures() == f0,
              "probe hit touches no counter");
        uint8_t *got = NULL; size_t glen = 0;
        CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, lsha, why, sizeof why) == 0, "formal load of the same entry hits");
        CHECK(!memcmp(lsha, sha, 32) && glen == plen, "probe and formal load agree on digest and length");
        free(got);
    }

    // 参数防御
    CHECK(s2a_program_cache_probe(NULL, id, sha, &plen, why, sizeof why) != 0, "probe null dir rejected");
    CHECK(s2a_program_cache_probe(g_dir, NULL, sha, &plen, why, sizeof why) != 0, "probe null identity rejected");
    CHECK(s2a_program_cache_probe(g_dir, id, NULL, &plen, why, sizeof why) != 0, "probe null digest out rejected");
    CHECK(s2a_program_cache_probe(g_dir, id, sha, NULL, why, sizeof why) != 0, "probe null length out rejected");
    CHECK(s2a_program_cache_probe(g_dir, id, sha, &plen, NULL, 0) == 0, "probe accepts a NULL why when it hits");

    // 坏头逐项拒绝（每项：改坏 → 探针必须拒、输出清零、原因对得上 → 复原）
    struct { const char *name; const char *needle; } bad[] = {
        {"short-header", "短于头部"}, {"magic", "magic"}, {"version", "版本"}, {"inner-identity", "身份"},
        {"zero-length", "越界"}, {"huge-length", "越界"}, {"truncated", "长度"}, {"extended", "长度"},
    };
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore probe fixture");
        FILE *f = NULL;
        if (!strcmp(bad[k].name, "short-header")) CHECK(truncate(path, 40) == 0, "shorten file below the header");
        else if (!strcmp(bad[k].name, "truncated")) CHECK(truncate(path, 88 + 10) == 0, "truncate payload");
        else if (!strcmp(bad[k].name, "extended")) CHECK(truncate(path, 88 + (off_t)sizeof payload + 5) == 0, "extend file past declared length");
        else if ((f = fopen(path, "r+b")) != NULL) {
            if (!strcmp(bad[k].name, "magic")) { fputc('X', f); }
            else if (!strcmp(bad[k].name, "version")) { uint8_t one = 1; fseek(f, 8, SEEK_SET); fwrite(&one, 1, 1, f); }
            else if (!strcmp(bad[k].name, "inner-identity")) { fseek(f, 16, SEEK_SET); fwrite(other, 1, 32, f); }
            else if (!strcmp(bad[k].name, "zero-length")) { uint8_t z[8] = {0}; fseek(f, 80, SEEK_SET); fwrite(z, 1, 8, f); }
            else if (!strcmp(bad[k].name, "huge-length")) { uint8_t h[8] = {0, 0, 0, 0, 0, 1, 0, 0}; fseek(f, 80, SEEK_SET); fwrite(h, 1, 8, f); }   // 2^40
            fclose(f);
        } else CHECK(0, "open entry for header damage");
        why[0] = 0; memset(sha, 0xAA, sizeof sha); plen = 99;
        char what[96];
        snprintf(what, sizeof what, "probe rejects a damaged header: %s", bad[k].name);
        CHECK(s2a_program_cache_probe(g_dir, id, sha, &plen, why, sizeof why) == -1, what);   // 坏文件是 -1，不是「没有文件」的 1
        snprintf(what, sizeof what, "probe damage leaves outputs zeroed: %s", bad[k].name);
        CHECK(!memcmp(sha, zero, 32) && plen == 0, what);
        snprintf(what, sizeof what, "probe damage is reported with its reason: %s", bad[k].name);
        CHECK(strstr(why, bad[k].needle) != NULL, what);
    }

    // 负载里一个字节坏了：探针只读头，照常给出头里记的摘要（合同：不校验负载）；正式读取必须拒绝并报摘要不符
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore probe fixture");
    {
        FILE *f = fopen(path, "r+b");
        CHECK(f != NULL, "open entry for payload corruption");
        if (f) {
            uint8_t bad_byte = (uint8_t)(payload[100] ^ 0xFF);
            fseek(f, 88 + 100, SEEK_SET);
            fwrite(&bad_byte, 1, 1, f);
            fclose(f);
        }
        CHECK(s2a_program_cache_probe(g_dir, id, sha, &plen, why, sizeof why) == 0 && !memcmp(sha, want, 32) && plen == sizeof payload,
              "probe still reports the header digest when only the payload is damaged (it does not read the payload)");
        uint8_t *got = NULL; size_t glen = 0;
        why[0] = 0;
        CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, lsha, why, sizeof why) != 0 && strstr(why, "摘要") != NULL,
              "formal load of the same damaged entry is rejected as a digest mismatch");
        free(got);
    }
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore probe fixture");
}

int main(void) {
    char tmpl[] = "/tmp/s2a-pcache-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { printf("FAIL: mkdtemp\n"); return 1; }
    snprintf(g_dir, sizeof g_dir, "%s/cache", dir);
    char state[600], derived[640], why[256] = "";
    snprintf(state, sizeof state, "%s/state", dir);
    CHECK(s2a_program_cache_dir(state, derived, sizeof derived, why, sizeof why) == 0,
          "cache dir derives from the session state dir");
    CHECK(strstr(derived, "direct-cache/nvn-vk") != NULL, "cache dir is separated from diagnostics");
    CHECK(s2a_program_cache_dir("relative/dir", derived, sizeof derived, why, sizeof why) != 0,
          "non-absolute state dir is rejected");

    uint8_t id[32], other[32];
    fill(id, sizeof id, 1);
    fill(other, sizeof other, 2);

    static uint8_t payload[4096];
    fill(payload, sizeof payload, 11);

    // --- 未命中 ---
    uint8_t *got = NULL;
    size_t glen = 0;
    uint8_t gsha[32];
    uint64_t misses0 = s2a_program_cache_misses();
    CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, gsha, why, sizeof why) != 0,
          "empty cache misses");
    CHECK(s2a_program_cache_misses() == misses0 + 1, "miss is counted");
    CHECK(got == NULL && glen == 0, "miss leaves outputs empty");

    // --- 存取往返 ---
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0,
          "store succeeds");
    CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, gsha, why, sizeof why) == 0,
          "store then load hits");
    CHECK(glen == sizeof payload && got && !memcmp(got, payload, glen), "payload round-trips");
    {
        static const uint8_t zero[32];
        CHECK(memcmp(gsha, zero, 32) != 0, "verified result digest is returned");
    }
    free(got); got = NULL;

    // --- 同一身份重复写：幂等 ---
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0,
          "store is idempotent");
    CHECK(count_entries() == 1, "no duplicate entry and no leftover temp file");

    // --- 另一身份不命中 ---
    CHECK(s2a_program_cache_load(g_dir, other, &got, &glen, gsha, why, sizeof why) != 0,
          "different identity misses");

    // --- 坏文件必须全部被拒 ---
    char path[640];
    {
        char hex[65];
        static const char *d = "0123456789abcdef";
        for (int i = 0; i < 32; i++) { hex[2*i] = d[id[i] >> 4]; hex[2*i+1] = d[id[i] & 15]; }
        hex[64] = 0;
        snprintf(path, sizeof path, "%s/%s.program", g_dir, hex);
    }

    // 1) 改一个负载字节：result 摘要必须发现
    {
        FILE *f = fopen(path, "r+b");
        CHECK(f != NULL, "open entry for corruption test");
        if (f) {
            fseek(f, 88 + 100, SEEK_SET);
            uint8_t bad = (uint8_t)(payload[100] ^ 0xFF);
            CHECK(fwrite(&bad, 1, 1, f) == 1, "corrupt one payload byte");
            fclose(f);
            why[0] = 0;
            CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, gsha, why, sizeof why) != 0,
                  "corrupted payload is rejected");
            CHECK(strstr(why, "摘要") != NULL, "corruption is reported as a digest mismatch");
        }
    }
    // 恢复
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore entry");

    // 2) 截断：长度和头部声明不符
    {
        CHECK(truncate(path, 88 + 10) == 0, "truncate entry");
        why[0] = 0;
        CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, gsha, why, sizeof why) != 0,
              "truncated file is rejected");
        CHECK(strstr(why, "长度") != NULL, "truncation is reported as a length mismatch");
    }
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore entry");

    // 3) magic 不符：不是本缓存格式
    {
        FILE *f = fopen(path, "r+b");
        if (f) { fputc('X', f); fclose(f); }
        why[0] = 0;
        CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, gsha, why, sizeof why) != 0,
              "bad magic is rejected");
        CHECK(strstr(why, "magic") != NULL, "bad magic is reported");
    }
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore entry");

    // 3b) 旧版本条目（身份编码 S2AI1 v3 之前的缓存）：明确拒绝，不猜测兼容
    {
        FILE *f = fopen(path, "r+b");
        if (f) {
            uint8_t one = 1;   // 头偏移 8 = u32 version；小端写入 1
            fseek(f, 8, SEEK_SET);
            fwrite(&one, 1, 1, f);
            fclose(f);
        }
        why[0] = 0;
        CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, gsha, why, sizeof why) != 0,
              "an older cache version is rejected instead of being guessed compatible");
        CHECK(strstr(why, "版本") != NULL || strstr(why, "version") != NULL, "version mismatch is reported");
    }
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore entry");

    // 4) 内部身份被改（文件被改名/串身份）
    {
        FILE *f = fopen(path, "r+b");
        if (f) { fseek(f, 16, SEEK_SET); fwrite(other, 1, 32, f); fclose(f); }
        why[0] = 0;
        CHECK(s2a_program_cache_load(g_dir, id, &got, &glen, gsha, why, sizeof why) != 0,
              "inner identity mismatch is rejected");
    }
    CHECK(s2a_program_cache_store(g_dir, id, payload, sizeof payload, why, sizeof why) == 0, "restore entry");

    // --- 异步写：不阻塞，drain 之后可见 ---
    // 注意：上面故意构造的坏文件会让 failures 增加，这是被拒绝的证据，不能当成功路径的失败。
    uint64_t failures_before = s2a_program_cache_failures();
    {
        uint8_t id3[32];
        fill(id3, sizeof id3, 77);
        uint64_t stores0 = s2a_program_cache_stores();
        CHECK(s2a_program_cache_store_async(g_dir, id3, payload, sizeof payload) == 0,
              "async store is accepted");
        CHECK(s2a_program_cache_drain(10000) == 0, "drain completes");
        CHECK(s2a_program_cache_stores() == stores0 + 1, "async store lands on disk");
        got = NULL; glen = 0;
        CHECK(s2a_program_cache_load(g_dir, id3, &got, &glen, gsha, why, sizeof why) == 0,
              "async store is loadable");
        free(got);
    }
    CHECK(s2a_program_cache_failures() == failures_before,
          "async success path adds no failure (rejected corrupt files above are counted)");

    // --- 参数防御 ---
    CHECK(s2a_program_cache_load(NULL, id, &got, &glen, gsha, why, sizeof why) != 0, "null dir rejected");
    CHECK(s2a_program_cache_store(g_dir, id, NULL, 10, why, sizeof why) != 0, "null bytes rejected");
    CHECK(s2a_program_cache_store(g_dir, id, payload, 0, why, sizeof why) != 0, "zero length rejected");
    CHECK(s2a_program_cache_store_async(NULL, id, payload, 10) != 0, "async null dir rejected");

    probe_tests();
    sync_failure_tests();

    // 清场
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("note: cleanup failed\n");

    if (!fails) printf("direct program cache: ok (hits=%llu misses=%llu stores=%llu failures=%llu)\n",
                       (unsigned long long)s2a_program_cache_hits(),
                       (unsigned long long)s2a_program_cache_misses(),
                       (unsigned long long)s2a_program_cache_stores(),
                       (unsigned long long)s2a_program_cache_failures());
    else printf("direct program cache: %d FAILURES\n", fails);
    printf("direct program cache assertions: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
