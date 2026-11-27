// F5.1：编译结果的跨局私有缓存。键口径见 program_cache.h。
//
// 盘上布局（小端，固定 88 字节头）：
//   0  magic[8]      "S2APC1\n\0"
//   8  u32 version   2（身份编码 S2AI1 v3：纳入完整源摘要；v1 文件明确拒绝，不猜测兼容）
//  12  u32 header   88
//  16  identity[32] 内容身份（规范化输入的 SHA-256）
//  48  result[32]   结果是完整字节的 SHA-256（损坏检测，读时重算比对）
//  80  u64 len      结果字节数
//  88  payload
//
// 头里同时带 identity 和 result 两份摘要：identity 只证明“这是谁的结果”，result 才能发现
// 同一身份下的字节损坏。两个都要比，不能只比文件名。
#include "installed_seed.h"
#include "program_cache.h"
#include "compile_phase.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"

#define PCACHE_MAGIC "S2APC1\n\0"
#define PCACHE_VERSION 2u
#define PCACHE_HEADER 88u
#define PCACHE_MAX_PAYLOAD (64u * 1024u * 1024u)
#define PCACHE_PATH 640

static _Atomic uint64_t g_hits, g_misses, g_stores, g_failures;

#ifdef S2A_DIRECT_TEST_FAULTS
static _Atomic int g_fsync_fault;
static _Atomic unsigned g_fault_hits;
void s2a_program_cache_test_fault(int fault) {
    atomic_store(&g_fault_hits, 0);
    atomic_store(&g_fsync_fault, fault);
}
unsigned s2a_program_cache_test_fault_hits(void) { return atomic_load(&g_fault_hits); }
#endif

static int sync_fd(int fd, int directory) {
    int rc = fsync(fd);
#ifdef S2A_DIRECT_TEST_FAULTS
    int fault = atomic_load(&g_fsync_fault);
    if (fault >= 1 && fault <= 4 && (fault > 2) == directory &&
        atomic_compare_exchange_strong(&g_fsync_fault, &fault, 0)) {
        atomic_fetch_add(&g_fault_hits, 1);
        errno = (fault & 1) ? EIO : EINVAL;
        rc = -1;
    }
#else
    (void)directory;
#endif
    return rc;
}

uint64_t s2a_program_cache_hits(void) { return atomic_load(&g_hits); }
uint64_t s2a_program_cache_misses(void) { return atomic_load(&g_misses); }
uint64_t s2a_program_cache_stores(void) { return atomic_load(&g_stores); }
uint64_t s2a_program_cache_failures(void) { return atomic_load(&g_failures); }

static void setwhy(char *why, size_t n, const char *fmt, ...) {
    if (!why || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

int s2a_program_cache_dir(const char *state_dir, char *out, size_t out_size,
                          char *why, size_t why_size) {
    if (!out || out_size == 0) { setwhy(why, why_size, "cache: 没有输出缓冲"); return -1; }
    out[0] = 0;
    if (!state_dir || !state_dir[0]) { setwhy(why, why_size, "cache: 缺会话状态目录"); return -1; }
    if (state_dir[0] != '/') { setwhy(why, why_size, "cache: 会话状态目录不是绝对路径"); return -1; }
    size_t len = strlen(state_dir);
    if (len > out_size - 48) { setwhy(why, why_size, "cache: 会话状态目录过长"); return -1; }
    int n = snprintf(out, out_size, "%s%sdirect-cache/nvn-vk", state_dir,
                     state_dir[len - 1] == '/' ? "" : "/");
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = 0;
        setwhy(why, why_size, "cache: 缓存路径过长");
        return -1;
    }
    return 0;
}

static void hex64(const uint8_t sha[32], char out[65]) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = d[sha[i] >> 4];
        out[2 * i + 1] = d[sha[i] & 15];
    }
    out[64] = 0;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v); put_u32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

// 目录不存在就建一层；父目录由调用方保证（应用私有状态目录）。
static int ensure_dir(const char *dir, char *why, size_t why_size) {
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) { setwhy(why, why_size, "%s 存在但不是目录", dir); return -1; }
        return 0;
    }
    if (mkdir(dir, 0700) == 0) return 0;
    if (errno == ENOENT) {
        // 父目录缺：只补一层，避免在深层路径上递归创建出非预期的树。
        char parent[PCACHE_PATH];
        snprintf(parent, sizeof parent, "%s", dir);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = 0;
            if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
                setwhy(why, why_size, "建父目录 %s 失败：%s", parent, strerror(errno));
                return -1;
            }
            if (mkdir(dir, 0700) == 0) return 0;
        }
    }
    setwhy(why, why_size, "建目录 %s 失败：%s", dir, strerror(errno));
    return -1;
}

static int entry_path(const char *dir, const uint8_t identity[32], char *out, size_t n) {
    char hex[65];
    hex64(identity, hex);
    int w = snprintf(out, n, "%s/%s.program", dir, hex);
    return (w > 0 && (size_t)w < n) ? 0 : -1;
}

// ---- 目录索引（本机跨局缓存目录；随包种子目录不走这里）----
// 每个身份取程序时要查一次缓存（正式读取、只读头各一处）。没有文件的身份原先每次都 open 一个不存在的路径：出包预编库供的
//   程序从不写缓存文件，一款大型开放世界游戏每局约 2.7 万个身份全部落空（11-26 真机：回放线程 __openat 占 0.9–1.3%）。
// 改为：某目录第一次被查询时 readdir 一遍，把「<64 位十六进制身份>.program」登记进内存集合；之后集合里没有的身份直接判
//   未命中，不再 open。本进程写入成功（改名可见）后登记。缓存目录应用私有、只有本游戏进程写，外部同时写入不在设计内；
//   外部删除无妨（集合里有、文件没了，open 得 ENOENT 照旧按未命中报）。目录打不开（不存在除外）就不建索引，退回逐次 open。
static pthread_mutex_t g_idx_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char dir[PCACHE_PATH];
    int state;            // 0 = 没建；1 = 已建；-1 = 列不了目录，逐次 open
    uint8_t (*keys)[32];  // 开放寻址；used[i] 标占用
    uint8_t *used;
    size_t n, cap;
} g_idx;

static size_t idx_slot(const uint8_t id[32], size_t cap) {
    uint64_t h; memcpy(&h, id, 8);   // 身份是 SHA-256，前 8 字节已经均匀
    return (size_t)(h & (cap - 1));
}
static int idx_insert_locked(const uint8_t id[32]) {
    if ((g_idx.n + 1) * 2 > g_idx.cap) {
        size_t cap = g_idx.cap ? g_idx.cap * 2 : 1024;
        uint8_t (*keys)[32] = calloc(cap, 32); uint8_t *used = calloc(cap, 1);
        if (!keys || !used) { free(keys); free(used); return -1; }
        for (size_t i = 0; i < g_idx.cap; i++) if (g_idx.used[i]) {
            size_t j = idx_slot(g_idx.keys[i], cap);
            while (used[j]) j = (j + 1) & (cap - 1);
            memcpy(keys[j], g_idx.keys[i], 32); used[j] = 1;
        }
        free(g_idx.keys); free(g_idx.used);
        g_idx.keys = keys; g_idx.used = used; g_idx.cap = cap;
    }
    size_t j = idx_slot(id, g_idx.cap);
    while (g_idx.used[j]) { if (!memcmp(g_idx.keys[j], id, 32)) return 0; j = (j + 1) & (g_idx.cap - 1); }
    memcpy(g_idx.keys[j], id, 32); g_idx.used[j] = 1; g_idx.n++;
    return 0;
}
static int idx_has_locked(const uint8_t id[32]) {
    if (!g_idx.cap) return 0;
    for (size_t j = idx_slot(id, g_idx.cap); g_idx.used[j]; j = (j + 1) & (g_idx.cap - 1))
        if (!memcmp(g_idx.keys[j], id, 32)) return 1;
    return 0;
}
static int hexnib(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static void idx_build_locked(const char *dir) {
    free(g_idx.keys); free(g_idx.used);
    memset(&g_idx, 0, sizeof g_idx);
    if (strlen(dir) >= sizeof g_idx.dir) { g_idx.state = -1; return; }
    strcpy(g_idx.dir, dir);
    DIR *d = opendir(dir);
    if (!d) { g_idx.state = errno == ENOENT ? 1 : -1; return; }   // 目录还没有 = 一个文件都没有；写入时 ensure_dir 建
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (strlen(nm) != 64 + 8 || strcmp(nm + 64, ".program")) continue;
        uint8_t id[32]; int ok = 1;
        for (int i = 0; i < 32 && ok; i++) {
            int hi = hexnib(nm[2 * i]), lo = hexnib(nm[2 * i + 1]);
            if (hi < 0 || lo < 0) ok = 0; else id[i] = (uint8_t)(hi << 4 | lo);
        }
        if (ok && idx_insert_locked(id)) { closedir(d); g_idx.state = -1; return; }
    }
    closedir(d);
    g_idx.state = 1;
}
// 1 = 索引里有（照常 open）；0 = 确定没有这个文件；-1 = 没有可用索引（照常 open）
static int idx_lookup(const char *dir, const uint8_t id[32]) {
    pthread_mutex_lock(&g_idx_mu);
    if (g_idx.state == 0 || strcmp(g_idx.dir, dir)) idx_build_locked(dir);
    int r = g_idx.state == 1 ? idx_has_locked(id) : -1;
    pthread_mutex_unlock(&g_idx_mu);
    return r;
}
// 本机缓存目录里的条目只读打开：索引确定没有就不碰文件系统，按 ENOENT 返回（与真 open 落空走同一条报告路径）
static int idx_open(const char *dir, const uint8_t id[32], const char *path) {
    if (idx_lookup(dir, id) == 0) { errno = ENOENT; return -1; }
    return open(path, O_RDONLY | O_CLOEXEC);
}
static void idx_note_stored(const char *dir, const uint8_t id[32]) {
    pthread_mutex_lock(&g_idx_mu);
    if (g_idx.state == 1 && !strcmp(g_idx.dir, dir) && idx_insert_locked(id)) g_idx.state = -1;   // 记不下就整体退回逐次 open，不漏报
    pthread_mutex_unlock(&g_idx_mu);
}

// installed = 1：随包种子目录里的文件，按已冻结修订的叶子规则受控打开（常规文件，或恰一条指向本游戏 objects 库的规范相对链接，见 installed_seed.h）；
//   0 = 本机跨局缓存（可写目录），普通 open。
static int program_cache_load(const char *dir, const uint8_t identity[32],
                              uint8_t **bytes, size_t *len,
                              uint8_t result_sha[32], char *why, size_t why_size, int installed) {
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (result_sha) memset(result_sha, 0, 32);
    if (!dir || !*dir || !identity || !bytes || !len) { setwhy(why, why_size, "参数缺失"); return -1; }
    char path[PCACHE_PATH];
    if (entry_path(dir, identity, path, sizeof path)) { setwhy(why, why_size, "缓存路径过长"); return -1; }

    int fd = installed ? s2a_installed_seed_open(path) : idx_open(dir, identity, path);
    if (fd < 0) {
        atomic_fetch_add(&g_misses, 1);
        setwhy(why, why_size, errno == ENOENT ? "未命中" : "打开失败：%s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)PCACHE_HEADER) {
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "文件短于头部");
        return -1;
    }
    uint8_t head[PCACHE_HEADER];
    if (read(fd, head, sizeof head) != (ssize_t)sizeof head) {
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "头部读不全");
        return -1;
    }
    if (memcmp(head, PCACHE_MAGIC, 8) != 0) {
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "magic 不符（不是本缓存格式）");
        return -1;
    }
    if (get_u32(head + 8) != PCACHE_VERSION || get_u32(head + 12) != PCACHE_HEADER) {
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "格式版本不符（写入方 version=%u header=%u）",
               get_u32(head + 8), get_u32(head + 12));
        return -1;
    }
    if (memcmp(head + 16, identity, 32) != 0) {
        // 文件名与内部身份不一致：被改名/串了身份，按不可信处理。
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "内部身份与查询身份不符");
        return -1;
    }
    uint64_t n = get_u64(head + 80);
    if (n == 0 || n > PCACHE_MAX_PAYLOAD) {
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "负载长度越界：%llu", (unsigned long long)n);
        return -1;
    }
    if ((uint64_t)st.st_size != (uint64_t)PCACHE_HEADER + n) {
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "文件长度 %lld 与头部声明 %llu 不符（截断？）",
               (long long)st.st_size, (unsigned long long)(PCACHE_HEADER + n));
        return -1;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) {
        close(fd); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "负载分配失败：%llu B", (unsigned long long)n);
        return -1;
    }
    size_t got = 0;
    while (got < (size_t)n) {
        ssize_t r = read(fd, buf + got, (size_t)n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != (size_t)n) {
        free(buf); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "负载读不全：%zu/%llu", got, (unsigned long long)n);
        return -1;
    }
    uint8_t digest[32];
    s2a_sha256 ctx;
    s2a_sha256_init(&ctx);
    s2a_sha256_update(&ctx, buf, (size_t)n);
    s2a_sha256_final(&ctx, digest);
    if (memcmp(digest, head + 48, 32) != 0) {
        free(buf); atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "结果内容摘要不符（字节损坏）");
        return -1;
    }
    atomic_fetch_add(&g_hits, 1);
    if (result_sha) memcpy(result_sha, digest, 32);
    *bytes = buf;
    *len = (size_t)n;
    return 0;
}
int s2a_program_cache_load(const char *dir, const uint8_t identity[32],
                           uint8_t **bytes, size_t *len,
                           uint8_t result_sha[32], char *why, size_t why_size) {
    return program_cache_load(dir, identity, bytes, len, result_sha, why, why_size, 0);
}
int s2a_program_cache_load_installed(const char *dir, const uint8_t identity[32],
                                     uint8_t **bytes, size_t *len,
                                     uint8_t result_sha[32], char *why, size_t why_size) {
    return program_cache_load(dir, identity, bytes, len, result_sha, why, why_size, 1);
}

int s2a_program_cache_probe(const char *dir, const uint8_t identity[32],
                            uint8_t result_sha[32], uint64_t *len, char *why, size_t why_size) {
    if (result_sha) memset(result_sha, 0, 32);
    if (len) *len = 0;
    if (!dir || !*dir || !identity || !result_sha || !len) { setwhy(why, why_size, "参数缺失"); return -1; }
    char path[PCACHE_PATH];
    if (entry_path(dir, identity, path, sizeof path)) { setwhy(why, why_size, "缓存路径过长"); return -1; }
    int fd = idx_open(dir, identity, path);
    if (fd < 0) {
        if (errno == ENOENT) { setwhy(why, why_size, "未命中"); return 1; }   // 没有这个身份的文件：与「文件坏」分开返回（调用方分开计数）
        setwhy(why, why_size, "打开失败：%s", strerror(errno)); return -1;
    }
    struct stat st;
    uint8_t head[PCACHE_HEADER];
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)PCACHE_HEADER) { close(fd); setwhy(why, why_size, "文件短于头部"); return -1; }
    const ssize_t r = read(fd, head, sizeof head);
    close(fd);
    if (r != (ssize_t)sizeof head) { setwhy(why, why_size, "头部读不全"); return -1; }
    if (memcmp(head, PCACHE_MAGIC, 8) != 0) { setwhy(why, why_size, "magic 不符（不是本缓存格式）"); return -1; }
    if (get_u32(head + 8) != PCACHE_VERSION || get_u32(head + 12) != PCACHE_HEADER) {
        setwhy(why, why_size, "格式版本不符（写入方 version=%u header=%u）", get_u32(head + 8), get_u32(head + 12));
        return -1;
    }
    if (memcmp(head + 16, identity, 32) != 0) { setwhy(why, why_size, "内部身份与查询身份不符"); return -1; }
    const uint64_t n = get_u64(head + 80);
    if (n == 0 || n > PCACHE_MAX_PAYLOAD) { setwhy(why, why_size, "负载长度越界：%llu", (unsigned long long)n); return -1; }
    if ((uint64_t)st.st_size != (uint64_t)PCACHE_HEADER + n) {
        setwhy(why, why_size, "文件长度 %lld 与头部声明 %llu 不符（截断？）", (long long)st.st_size, (unsigned long long)(PCACHE_HEADER + n));
        return -1;
    }
    memcpy(result_sha, head + 48, 32);
    *len = n;
    return 0;
}

int s2a_program_cache_store(const char *dir, const uint8_t identity[32],
                            const uint8_t *bytes, size_t len, char *why, size_t why_size) {
    if (!dir || !*dir || !identity || !bytes || !len) { setwhy(why, why_size, "参数缺失"); return -1; }
    if (len > PCACHE_MAX_PAYLOAD) { setwhy(why, why_size, "结果过大：%zu B", len); return -1; }
    if (ensure_dir(dir, why, why_size)) return -1;

    char path[PCACHE_PATH], tmp[PCACHE_PATH];
    if (entry_path(dir, identity, path, sizeof path)) { setwhy(why, why_size, "缓存路径过长"); return -1; }
    // 临时名要在同目录（rename 才是原子的），并且带 pid + 计数避免并发撞名。
    static _Atomic unsigned seq;
    snprintf(tmp, sizeof tmp, "%s/.tmp-%ld-%u", dir, (long)getpid(), atomic_fetch_add(&seq, 1u));

    uint8_t head[PCACHE_HEADER];
    memcpy(head, PCACHE_MAGIC, 8);
    put_u32(head + 8, PCACHE_VERSION);
    put_u32(head + 12, PCACHE_HEADER);
    memcpy(head + 16, identity, 32);
    {
        uint8_t digest[32];
        s2a_sha256 ctx;
        s2a_sha256_init(&ctx);
        s2a_sha256_update(&ctx, bytes, len);
        s2a_sha256_final(&ctx, digest);
        memcpy(head + 48, digest, 32);
    }
    put_u64(head + 80, (uint64_t)len);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { setwhy(why, why_size, "建临时文件失败：%s", strerror(errno)); return -1; }
    int ok = 1;
    if (write(fd, head, sizeof head) != (ssize_t)sizeof head) ok = 0;
    if (ok) {
        size_t done = 0;
        while (done < len) {
            ssize_t w = write(fd, bytes + done, len - done);
            if (w <= 0) { ok = 0; break; }
            done += (size_t)w;
        }
        if (done != len) ok = 0;
    }
    // 文件同步失败不能发布；不重试，也不把“字节已写入”当作持久化成功。
    if (ok && sync_fd(fd, 0) != 0) {
        ok = 0;
        setwhy(why, why_size, "文件 fsync 失败（未改名）：%s", strerror(errno));
    }
    if (close(fd) != 0) ok = 0;
    if (!ok) {
        unlink(tmp);
        atomic_fetch_add(&g_failures, 1);
        if (!why || !why[0]) setwhy(why, why_size, "写临时文件失败：%s", strerror(errno));
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "rename 失败：%s", strerror(errno));
        return -1;
    }
    idx_note_stored(dir, identity);   // 改名后目标已可见（目录同步失败也不回滚），索引同步登记
    // 改名已可见后再同步目录项。失败如实报告，但不回滚或删除已发布的目标。
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int sync_error = dirfd < 0 ? errno : 0;
    if (dirfd >= 0) {
        if (sync_fd(dirfd, 1) != 0) sync_error = errno;
        if (close(dirfd) != 0 && !sync_error) sync_error = errno;
    }
    if (sync_error) {
        atomic_fetch_add(&g_failures, 1);
        setwhy(why, why_size, "缓存目录同步失败（文件已改名）：%s", strerror(sync_error));
        return -1;
    }
    atomic_fetch_add(&g_stores, 1);
    return 0;
}

// 本会话的程序写入队列。当前写入也占字节预算；磁盘操作不持队列锁。
enum { PCACHE_QUEUE_MAX = 32 };
struct pcache_job {
    char dir[PCACHE_PATH];
    uint8_t identity[32];
    uint8_t *bytes;
    size_t len;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_idle = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;
static struct pcache_job g_queue[PCACHE_QUEUE_MAX], g_current;
static unsigned g_head, g_queued;
static size_t g_write_bytes;
static int g_writer_alive, g_writer_stopping;

static int same_job(const struct pcache_job *job, const char *dir, const uint8_t identity[32]) {
    return job->len && !memcmp(job->identity, identity, sizeof job->identity) && !strcmp(job->dir, dir);
}

static void *pcache_writer(void *unused) {
    (void)unused;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_queued && !g_writer_stopping) pthread_cond_wait(&g_work, &g_lock);
        if (!g_queued) break;
        g_current = g_queue[g_head];
        g_head = (g_head + 1) % PCACHE_QUEUE_MAX;
        g_queued--;
        pthread_mutex_unlock(&g_lock);

        char why[192] = "";
        struct s2a_compile_stamp phase = s2a_compile_phase_begin();
        if (s2a_program_cache_store(g_current.dir, g_current.identity, g_current.bytes, g_current.len, why, sizeof why))
            fprintf(stderr, "[s2a] program cache: store failed: %s\n", why);
        s2a_compile_phase_end(phase, "cache-write", g_current.identity);
        free(g_current.bytes);

        pthread_mutex_lock(&g_lock);
        g_write_bytes -= g_current.len;
        memset(&g_current, 0, sizeof g_current);
        if (!g_queued) pthread_cond_broadcast(&g_idle);
    }
    g_writer_alive = 0;
    pthread_cond_broadcast(&g_idle);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int s2a_program_cache_start(void) {
    pthread_mutex_lock(&g_lock);
    int rc = g_writer_stopping && g_writer_alive ? -1 : 0;
    if (!g_writer_alive) {
        pthread_attr_t attr;
        rc = pthread_attr_init(&attr);
        if (!rc) {
            pthread_t thread;
            rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (!rc) rc = pthread_create(&thread, &attr, pcache_writer, NULL);
            pthread_attr_destroy(&attr);
        }
        if (!rc) {
            g_writer_stopping = 0;
            g_writer_alive = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc ? -1 : 0;
}

int s2a_program_cache_store_owned(const char *dir, const uint8_t identity[32],
                                  uint8_t *bytes, size_t len) {
    if (!dir || !*dir || strlen(dir) >= PCACHE_PATH || !identity || !bytes || !len || len > PCACHE_MAX_PAYLOAD)
        return -1;
    pthread_mutex_lock(&g_lock);
    if (!g_writer_alive || g_writer_stopping) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    int duplicate = same_job(&g_current, dir, identity);
    for (unsigned i = 0; !duplicate && i < g_queued; i++)
        duplicate = same_job(&g_queue[(g_head + i) % PCACHE_QUEUE_MAX], dir, identity);
    if (duplicate) {
        pthread_mutex_unlock(&g_lock);
        free(bytes);
        return 0;
    }
    if (g_queued == PCACHE_QUEUE_MAX || len > PCACHE_MAX_PAYLOAD - g_write_bytes) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    struct pcache_job *job = &g_queue[(g_head + g_queued) % PCACHE_QUEUE_MAX];
    strcpy(job->dir, dir);   // 路径长度在入队前核对。
    memcpy(job->identity, identity, sizeof job->identity);
    job->bytes = bytes;
    job->len = len;
    g_queued++;
    g_write_bytes += len;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int s2a_program_cache_store_async(const char *dir, const uint8_t identity[32],
                                  const uint8_t *bytes, size_t len) {
    if (!dir || !*dir || !identity || !bytes || !len || len > PCACHE_MAX_PAYLOAD) return -1;
    uint8_t *copy = malloc(len);
    if (!copy) return -1;
    memcpy(copy, bytes, len);
    int rc = s2a_program_cache_store_owned(dir, identity, copy, len);
    if (rc) free(copy);
    return rc;
}

static int wait_writer(uint32_t timeout_ms, int stop) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000u;
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_lock);
    if (stop && g_writer_alive) {
        g_writer_stopping = 1;
        pthread_cond_signal(&g_work);
    }
    while (stop ? g_writer_alive : g_write_bytes != 0)
        if (pthread_cond_timedwait(&g_idle, &g_lock, &deadline) == ETIMEDOUT) break;
    const int busy = stop ? g_writer_alive : g_write_bytes != 0;
    pthread_mutex_unlock(&g_lock);
    return busy ? -1 : 0;
}

int s2a_program_cache_drain(uint32_t timeout_ms) { return wait_writer(timeout_ms, 0); }
int s2a_program_cache_stop(uint32_t timeout_ms) { return wait_writer(timeout_ms, 1); }
