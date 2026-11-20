// 缓冲建删的增长率回归（11-07 时之笛 Switch 2 室外 30 → 5–6 帧）：经公开 NVN 接口（hle_nvn.c）跑 nvnBufferInitialize / Finalize，
//   N 个与 4N 个缓冲各跑一遍（先全部建好、活缓冲数随之涨到 N / 4N，再全部注销），比较两次耗时。
//   期望不依赖被测实现，只由复杂度推出：每次建删 O(log 活缓冲数) → 总耗时近线性，4N / N ≈ 4（加 log 因子约 4.6）；
//   每次建删 O(活缓冲数)（810f70a2b 的无序数组线性扫描）→ 总耗时平方级，比值 ≈ 16。比值 > 8 判红。
//   ① 单线程；② 4 线程（各建删自己那 1/4，建完等齐再一起删）抓锁争用：锁内做 O(n) 活时等锁时间同样平方级涨。
//   只比比值、不设绝对耗时上限：机器快慢 / 负载高低同时作用于两次测量，比值不受影响。每档取 5 次里最快的一次压掉偶发抖动。
//   负对照：同一单测放在 810f70a2b 的 hle_nvn.c 上跑，① ② 比值都约 16、判红（见坑记录 11-07）。
// 建删计时与登记功能分开验证：
//   缺省 = 安卓路线：S2A_NVN_BUF_REG=1 强制登记（安卓后端 nvn_be_buf_extent_consumer 报 1 时同一条路径）；③ 功能：每个活缓冲内的地址
//     nvn_buf_extent 都命中 {该缓冲起点, 长度}，注销后不再命中；再跑 ① ②。
//   metal = Metal 路线：不设开关，后端（nvn_mtl.m）报没有查询方 → ④ 建了 4N 个活缓冲，nvn_buf_extent 一个都不命中（一次登记都没发生）；① ② 只报耗时不判。
//   nvk / nvk-forced = 只测登记：前端接真实 rp_entry.c 的能力查询，同一夹具验证缺省不登记、强制打开能登记。
#include "nvn_protos.h"
#include "nvn_obj.h"
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern void *h_nvnBootstrapLoader(const char *name);
enum { N = 10000, NMAX = 4 * N, BUF = 256, THREADS = 4, REPS = 5 };
static const double LIMIT = 8.0;
static NVNdevice device;
static NVNmemoryPool pool;
static NVNbuffer *bufs;
static NVNT_nvnBufferBuilderSetDefaults p_defaults; static NVNT_nvnBufferBuilderSetDevice p_device;
static NVNT_nvnBufferBuilderSetStorage p_storage; static NVNT_nvnBufferInitialize p_init; static NVNT_nvnBufferFinalize p_fini;
static int fails, metal;
#define REQUIRE(x) do { if (!(x)) { printf("✗ t_nvn_bufreg_rate：准备失败 %s（%s:%d）\n", #x, __FILE__, __LINE__); exit(1); } } while (0)
static void *load(const char *name) { void *e = h_nvnBootstrapLoader(name); REQUIRE(e != NULL); return e; }
#define API(name) ((NVNT_##name)load(#name))
static double now(void) { struct timespec s; clock_gettime(CLOCK_MONOTONIC, &s); return s.tv_sec + s.tv_nsec * 1e-9; }

static void setup(void) {
    char dir[256], p[512]; snprintf(dir, sizeof dir, "%s/t_nvn_bufreg_rate.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(dir, 0700); snprintf(p, sizeof p, "%s/shaders", dir); mkdir(p, 0700); snprintf(p, sizeof p, "%s/shaders/direct", dir); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/shaders/direct/game.tsv", dir); FILE *f = fopen(p, "w"); REQUIRE(f != NULL); fputs("tex_dirty_upload\toff\n", f); fclose(f);
    setenv("S2A_GAME_DATA", dir, 1);
    NVNdeviceBuilder db; API(nvnDeviceBuilderSetDefaults)(&db); REQUIRE(API(nvnDeviceInitialize)(&device, &db));
    void *storage; size_t size = (size_t)NMAX * BUF; REQUIRE(posix_memalign(&storage, 65536, size) == 0);
    NVNmemoryPoolBuilder pb; API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, storage, size); API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    REQUIRE(API(nvnMemoryPoolInitialize)(&pool, &pb));
    bufs = calloc(NMAX, sizeof *bufs); REQUIRE(bufs != NULL);
    p_defaults = API(nvnBufferBuilderSetDefaults); p_device = API(nvnBufferBuilderSetDevice); p_storage = API(nvnBufferBuilderSetStorage);
    p_init = API(nvnBufferInitialize); p_fini = API(nvnBufferFinalize);   // 先取好入口：计时只含建删本身
}

// 线程 t 负责 [lo, hi)：建好全部 → 等所有线程建完（活缓冲数到顶）→ 按建的顺序全部注销
typedef struct { int lo, hi, nthreads; } span_t;
static _Atomic int built;
static void *work(void *arg) {
    const span_t *s = arg; NVNbufferBuilder b;
    for (int i = s->lo; i < s->hi; i++) {
        p_defaults(&b); p_device(&b, &device); p_storage(&b, &pool, (ptrdiff_t)i * BUF, BUF);
        if (!p_init(&bufs[i], &b)) { printf("✗ t_nvn_bufreg_rate：第 %d 个缓冲建失败\n", i); exit(1); }
    }
    atomic_fetch_add(&built, 1);
    while (atomic_load(&built) < s->nthreads) sched_yield();
    for (int i = s->lo; i < s->hi; i++) p_fini(&bufs[i]);
    return NULL;
}
static double run_once(int n, int nthreads) {
    pthread_t th[THREADS]; span_t sp[THREADS]; atomic_store(&built, 0);
    double t0 = now();
    for (int t = 0; t < nthreads; t++) { sp[t] = (span_t){n * t / nthreads, n * (t + 1) / nthreads, nthreads}; pthread_create(&th[t], NULL, work, &sp[t]); }
    for (int t = 0; t < nthreads; t++) pthread_join(th[t], NULL);
    return now() - t0;
}
static double best(int n, int nthreads) { double m = 1e30; for (int r = 0; r < REPS; r++) { double d = run_once(n, nthreads); if (d < m) m = d; } return m; }
static void judge(const char *what, int nthreads) {
    double a = best(N, nthreads), b = best(NMAX, nthreads), ratio = b / a;
    printf("  %s：N=%d %.1f ms，4N=%d %.1f ms，比值 %.2f（线性约 4、平方级约 16，> %.0f 判红）\n", what, N, a * 1e3, NMAX, b * 1e3, ratio, LIMIT);
    if (metal) return;   // Metal 路线不登记，建删只剩百纳秒级的对象写入：总耗时亚毫秒、比值被起线程等固定开销淹没，只报不判（零登记由 ④ 证）
    if (!(ratio <= LIMIT)) { fails++; printf("✗ t_nvn_bufreg_rate：%s 建删耗时随活缓冲数超线性增长（比值 %.2f > %.0f）\n", what, ratio, LIMIT); }
}

// ③ / ④：建 n 个活缓冲，逐个查缓冲中间的地址；want = 1 应命中自身 {起点, 长度}，0 应全不命中；全部注销后再查一遍应全不命中
static void extent_check(int n, int want, const char *backend) {
    NVNbufferBuilder b; unsigned bad = 0, stale = 0;
    for (int i = 0; i < n; i++) { p_defaults(&b); p_device(&b, &device); p_storage(&b, &pool, (ptrdiff_t)i * BUF, BUF); REQUIRE(p_init(&bufs[i], &b)); }
    uint8_t *mem = (uint8_t *)(uintptr_t)API(nvnBufferMap)(&bufs[0]);
    for (int i = 0; i < n; i++) {
        uint64_t base = 0, size = 0, a = (uint64_t)(uintptr_t)(mem + (size_t)i * BUF + BUF / 2);
        int hit = nvn_buf_extent(a, &base, &size);
        if (want ? !(hit && base == a - BUF / 2 && size == BUF) : hit) bad++;
    }
    for (int i = 0; i < n; i++) p_fini(&bufs[i]);
    for (int i = 0; i < n; i++) { uint64_t base, size; stale += nvn_buf_extent((uint64_t)(uintptr_t)(mem + (size_t)i * BUF + BUF / 2), &base, &size); }
    const char *what = want ? "活缓冲按地址查长度全命中" : "活缓冲一个都没登记";
    printf("  %s：%s，%d 个活缓冲，不符 %u、注销后仍命中 %u\n", backend, what, n, bad, stale);
    if (bad || stale) { fails++; printf("✗ t_nvn_bufreg_rate：%s %s 不成立（不符 %u、注销后仍命中 %u）\n", backend, what, bad, stale); }
}

int main(int argc, char **argv) {
    metal = argc > 1 && !strcmp(argv[1], "metal");
    int nvk = argc > 1 && (!strcmp(argv[1], "nvk") || !strcmp(argv[1], "nvk-forced"));
    int want = nvk ? !strcmp(argv[1], "nvk-forced") : !metal;
    const char *backend = nvk ? argv[1] : metal ? "Metal" : "强制登记";
    if (want) REQUIRE(setenv("S2A_NVN_BUF_REG", "1", 1) == 0);
    else REQUIRE(unsetenv("S2A_NVN_BUF_REG") == 0);
    @autoreleasepool {
        setup();
        extent_check(NMAX, want, backend);
        if (nvk) return fails != 0;
        run_once(NMAX, 1);   // 预热：表的结点池 / 页面先长到 4N，不算进计时
        judge("① 单线程", 1);
        judge("② 4 线程", THREADS);
    }
    if (fails) return 1;
    printf("✓ t_nvn_bufreg_rate（%s）：%s\n", metal ? "Metal 路线" : "安卓路线",
           metal ? "活缓冲零登记（耗时只报不判）" : "按地址查长度全对；单线程与 4 线程的缓冲建删耗时均随活缓冲数近线性增长");
    return 0;
}
