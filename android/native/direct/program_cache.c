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
#include "program_cache.h"

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

int s2a_program_cache_load(const char *dir, const uint8_t identity[32],
                           uint8_t **bytes, size_t *len,
                           uint8_t result_sha[32], char *why, size_t why_size) {
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (result_sha) memset(result_sha, 0, 32);
    if (!dir || !*dir || !identity || !bytes || !len) { setwhy(why, why_size, "参数缺失"); return -1; }
    char path[PCACHE_PATH];
    if (entry_path(dir, identity, path, sizeof path)) { setwhy(why, why_size, "缓存路径过长"); return -1; }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
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

int s2a_program_cache_probe(const char *dir, const uint8_t identity[32],
                            uint8_t result_sha[32], uint64_t *len, char *why, size_t why_size) {
    if (result_sha) memset(result_sha, 0, 32);
    if (len) *len = 0;
    if (!dir || !*dir || !identity || !result_sha || !len) { setwhy(why, why_size, "参数缺失"); return -1; }
    char path[PCACHE_PATH];
    if (entry_path(dir, identity, path, sizeof path)) { setwhy(why, why_size, "缓存路径过长"); return -1; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
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

// ---- 异步写：一次性分离线程，当前绘制不等磁盘（方案 §5.5.2 第 5 条）----

struct pcache_job {
    char *dir;
    uint8_t identity[32];
    uint8_t *bytes;
    size_t len;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_idle = PTHREAD_COND_INITIALIZER;
static unsigned g_pending;

static void *pcache_writer(void *arg) {
    struct pcache_job *j = arg;
    char why[192] = "";
    if (s2a_program_cache_store(j->dir, j->identity, j->bytes, j->len, why, sizeof why) != 0) {
        // 写盘失败只记缓存问题，不影响已经可用的内存结果。
        fprintf(stderr, "[s2a] program cache: store failed: %s\n", why);
        atomic_fetch_add(&g_failures, 1);
    }
    free(j->bytes);
    free(j->dir);
    free(j);
    pthread_mutex_lock(&g_lock);
    if (g_pending) g_pending--;
    if (!g_pending) pthread_cond_broadcast(&g_idle);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int s2a_program_cache_store_async(const char *dir, const uint8_t identity[32],
                                  const uint8_t *bytes, size_t len) {
    if (!dir || !*dir || !identity || !bytes || !len || len > PCACHE_MAX_PAYLOAD) return -1;
    struct pcache_job *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->dir = strdup(dir);
    j->bytes = malloc(len);
    if (!j->dir || !j->bytes) { free(j->dir); free(j->bytes); free(j); return -1; }
    memcpy(j->identity, identity, 32);
    memcpy(j->bytes, bytes, len);
    j->len = len;
    pthread_mutex_lock(&g_lock);
    g_pending++;
    pthread_mutex_unlock(&g_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, pcache_writer, j) != 0) {
        pthread_mutex_lock(&g_lock);
        if (g_pending) g_pending--;
        if (!g_pending) pthread_cond_broadcast(&g_idle);
        pthread_mutex_unlock(&g_lock);
        free(j->bytes); free(j->dir); free(j);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

int s2a_program_cache_drain(uint32_t timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000u;
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_lock);
    int rc = 0;
    while (g_pending) {
        if (pthread_cond_timedwait(&g_idle, &g_lock, &deadline) == ETIMEDOUT) { rc = g_pending ? -1 : 0; break; }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}
