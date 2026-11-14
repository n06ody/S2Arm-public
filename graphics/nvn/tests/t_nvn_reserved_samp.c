// 采样器池的保留号（认领 #969）：游戏自带的 NVN 驱动在 nvnSamplerPoolInitialize 里往池的第 0、1 项写两个默认采样器
//   （本机 6 款 sdk 静态反汇编一致：SetDefaults 那套参数，寻址 S / T / R 改成 CLAMP_TO_EDGE；第 0 项各向异性 1.0、第 1 项 2.0），
//   nvnTexturePoolInitialize 不写任何描述符。游戏直接拿采样器号 0 采样（11-14 安卓 8E 010045B018EC2000：句柄 贴图 2127 | 采样器 0，
//   OpImageSampleImplicitLod，一局 sampler_slot_unresolved 5315 次）。HLE 替换了驱动，就得在池初始化时照样登记这两项。
// 期望按驱动反汇编手写，不由被测实现推出；观测走公开接口 + 后端的 DM_NVN_SAMPINFO 诊断行（每次登记打一行完整参数，写 fd 2）。
// 用法：t_nvn_reserved_samp <临时目录>（fd 2 暂时转到该目录下的文件里再读回）
#include "nvn_protos.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *h_nvnBootstrapLoader(const char *name);
extern int nvn_dpool_base(const void *pool, int tex);
static unsigned checks, bad;
#define CHECK(expr, ...) do { checks++; if (!(expr)) { bad++; fprintf(stdout, "✗ %s:%d：%s —— ", __FILE__, __LINE__, #expr); fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); } } while (0)
static void *load(const char *name) { void *entry = h_nvnBootstrapLoader(name); if (!entry) { printf("✗ 取不到 %s\n", name); exit(1); } return entry; }
#define API(name) ((NVNT_##name)load(#name))

// 驱动写的两项（NVN 枚举值：LINEAR = 1、CLAMP_TO_EDGE = 7、COMPARE_MODE_NONE = 0、COMPARE_FUNC_LESS = 2、REDUCTION_AVERAGE = 0）
static void want_line(char *out, size_t n, int id, float aniso) {
    snprintf(out, n, "SAMPINFO id %d min 1 mag 1 wrap 7/7/7 lod 0.000..1000.000 bias 0.000 aniso %.1f compare 0/2 reduction 0", id, aniso);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setenv("DM_NVN_SAMPINFO", "64", 1);
    char path[1024]; snprintf(path, sizeof path, "%s/t_nvn_reserved_samp.log", argv[1]);
    fflush(stderr); int saved = dup(2), fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (saved < 0 || fd < 0) { printf("✗ 打不开 %s\n", path); return 2; }
    dup2(fd, 2); close(fd);

    NVNdevice dev; NVNdeviceBuilder db; memset(&dev, 0, sizeof dev); memset(&db, 0, sizeof db);
    API(nvnDeviceBuilderSetDefaults)(&db); int dev_ok = API(nvnDeviceInitialize)(&dev, &db);
    enum { POOL = 1 << 20 };
    void *mem = NULL; int mem_ok = posix_memalign(&mem, 65536, POOL) == 0; if (mem_ok) memset(mem, 0, POOL);
    NVNmemoryPool mp; NVNmemoryPoolBuilder pb; memset(&mp, 0, sizeof mp); memset(&pb, 0, sizeof pb);
    API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &dev); API(nvnMemoryPoolBuilderSetStorage)(&pb, mem, POOL);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    int mp_ok = API(nvnMemoryPoolInitialize)(&mp, &pb);
    // 两个同时在用的采样器池：第二个的编号段不从 0 起，保留号要落在各自的 base + 0 / base + 1
    NVNsamplerPool sa, sb; memset(&sa, 0, sizeof sa); memset(&sb, 0, sizeof sb);
    int a_ok = API(nvnSamplerPoolInitialize)(&sa, &mp, 0, 272), b_ok = API(nvnSamplerPoolInitialize)(&sb, &mp, 0x10000, 300);
    int ba = nvn_dpool_base(&sa, 0), bb = nvn_dpool_base(&sb, 0);
    // 游戏之后自己登记 0 号：照常覆盖（同真机上游戏改写池内存）
    NVNsamplerBuilder sbld; memset(&sbld, 0, sizeof sbld);
    API(nvnSamplerBuilderSetDevice)(&sbld, &dev); API(nvnSamplerBuilderSetDefaults)(&sbld);
    API(nvnSamplerBuilderSetMinMagFilter)(&sbld, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    API(nvnSamplerPoolRegisterSamplerBuilder)(&sa, 0, &sbld);

    fflush(stderr); dup2(saved, 2); close(saved);
    CHECK(dev_ok && mem_ok && mp_ok && a_ok && b_ok, "设备 / 内存池 / 采样器池初始化");
    CHECK(bb != ba, "两个在用的池编号段不同（A %d、B %d）", ba, bb);
    FILE *f = fopen(path, "r"); char line[512], log[64][200]; int n = 0;
    while (f && fgets(line, sizeof line, f)) { char *p = strstr(line, "SAMPINFO id "); if (p && n < 64) { p[strcspn(p, "\n")] = 0; snprintf(log[n++], sizeof log[0], "%s", p); } }
    if (f) fclose(f);
    char w[200];
    struct { int id; float aniso; const char *what; } want[] = {{ba, 1.0f, "池 A 第 0 项"}, {ba + 1, 2.0f, "池 A 第 1 项"}, {bb, 1.0f, "池 B 第 0 项"}, {bb + 1, 2.0f, "池 B 第 1 项"}};
    int last_a0 = -1;
    for (unsigned k = 0; k < sizeof want / sizeof *want; k++) {
        want_line(w, sizeof w, want[k].id, want[k].aniso); int hit = -1;
        for (int i = 0; i < n; i++) if (!strcmp(log[i], w)) { hit = i; break; }
        CHECK(hit >= 0, "%s：期望登记「%s」，没有（共 %d 行 SAMPINFO）", want[k].what, w, n);
        if (k == 0) last_a0 = hit;
    }
    char g[200]; snprintf(g, sizeof g, "SAMPINFO id %d min 0 mag 0 wrap 1/1/1 lod 0.000..1000.000 bias 0.000 aniso 1.0 compare 0/2 reduction 0", ba);
    int ghit = -1; for (int i = 0; i < n; i++) if (!strcmp(log[i], g)) ghit = i;
    CHECK(ghit >= 0 && ghit > last_a0, "游戏后登记的 0 号照常覆盖保留项（保留项第 %d 行，游戏登记第 %d 行）", last_a0, ghit);
    for (int i = 0; i < n; i++) printf("  %s\n", log[i]);
    if (bad) { printf("✗ t_nvn_reserved_samp：%u 项里 %u 项不符\n", checks, bad); return 1; }
    printf("✓ t_nvn_reserved_samp：%u 项全过（采样器池 A 基号 %d、B 基号 %d：第 0 / 1 项照驱动登记，游戏后登记照常覆盖）\n", checks, ba, bb);
    return 0;
}
