// CPU 不可访问的池映射给 NULL（hle_nvn.c nvnMemoryPoolMap / nvnBufferMap，11-02 FC 26）：公开 NVN 接口、真实池与缓冲对象。期望按 NVN 语义手写，不由被测实现推出：
//   池标志带 CPU_NO_ACCESS（0x1）→ 两个 Map 都是 NULL；CPU 可访问（CPU_CACHED / CPU_UNCACHED）→ 池 Map = 存储指针，缓冲 Map = 存储 + 偏移。
//   FC 26 引擎的算式「拷贝源 = 缓冲 GPU 地址 + (CPU 指针 − nvnBufferMap)」在 CPU 不可访问的池上把 CPU 指针记 0，算回 GPU 地址的前提就是 Map 也给 NULL（反证一并核：GPU 地址照常给出、与存储无关）
#include "nvn_protos.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *h_nvnBootstrapLoader(const char *name);
static unsigned checks;
#define CHECK(expr, ...) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d：%s —— ", __FILE__, __LINE__, #expr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)
static void *load(const char *name) { void *entry = h_nvnBootstrapLoader(name); if (!entry) { fprintf(stderr, "FAIL 取不到 %s\n", name); exit(1); } return entry; }
#define API(name) ((NVNT_##name)load(#name))
enum { POOL = 1 << 20, OFF = 0x10000, LEN = 4096 };

// Observe what the real HLE initializer passes to the backend.
extern void nvn_be_pool_add(const void *, uint64_t);
static const void *registered_mem;
static int64_t registered_flags;
static uint64_t registered_size;
void nvn_be_pool_add_flags(const void *mem, uint64_t size, int64_t flags) {
    registered_mem = mem; registered_flags = flags; registered_size = size;
    nvn_be_pool_add(mem, size);
}

static void one(NVNdevice *dev, int flags, int none, const char *what) {
    void *mem = NULL; CHECK(posix_memalign(&mem, 65536, POOL) == 0, "分配存储"); memset(mem, 0, POOL);
    NVNmemoryPool pool; NVNmemoryPoolBuilder pb; memset(&pool, 0, sizeof pool); memset(&pb, 0, sizeof pb);
    API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, dev);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, mem, POOL); API(nvnMemoryPoolBuilderSetFlags)(&pb, flags);
    registered_mem = NULL; registered_flags = -1; registered_size = 0;
    CHECK(API(nvnMemoryPoolInitialize)(&pool, &pb), "%s：池初始化", what);
    CHECK(registered_mem == mem && registered_size == POOL && registered_flags == flags, "%s：后端收到真实建池标志和存储", what);
    NVNbuffer buf; NVNbufferBuilder bb; memset(&buf, 0, sizeof buf); memset(&bb, 0, sizeof bb);
    API(nvnBufferBuilderSetDefaults)(&bb); API(nvnBufferBuilderSetDevice)(&bb, dev); API(nvnBufferBuilderSetStorage)(&bb, &pool, OFF, LEN);
    CHECK(API(nvnBufferInitialize)(&buf, &bb), "%s：缓冲初始化", what);
    void *pm = API(nvnMemoryPoolMap)(&pool), *bm = API(nvnBufferMap)(&buf);
    NVNbufferAddress pa = API(nvnMemoryPoolGetBufferAddress)(&pool), ba = API(nvnBufferGetAddress)(&buf);
    if (none) { CHECK(pm == NULL, "%s：池 Map 应 NULL，得 %p", what, pm); CHECK(bm == NULL, "%s：缓冲 Map 应 NULL，得 %p", what, bm); }
    else { CHECK(pm == mem, "%s：池 Map 应 %p，得 %p", what, mem, pm); CHECK(bm == (uint8_t *)mem + OFF, "%s：缓冲 Map 应 %p，得 %p", what, (void *)((uint8_t *)mem + OFF), bm); }
    CHECK(pa != 0 && ba == pa + OFF, "%s：GPU 地址照常给出（池 %#llx、缓冲 %#llx，应池 + %#x）", what, (unsigned long long)pa, (unsigned long long)ba, OFF);
    // FC 26 的算式：GPU 地址 + (引擎记的 CPU 指针 − BufferMap)；CPU 不可访问时引擎记 0。结果须等于缓冲 GPU 地址
    uint64_t engine_cpu = none ? 0 : (uint64_t)(uintptr_t)bm, src = (uint64_t)ba + (engine_cpu - (uint64_t)(uintptr_t)bm);
    CHECK(src == (uint64_t)ba, "%s：引擎算式得 %#llx，应等于缓冲 GPU 地址 %#llx", what, (unsigned long long)src, (unsigned long long)ba);
    API(nvnBufferFinalize)(&buf); API(nvnMemoryPoolFinalize)(&pool); free(mem);
    printf("✓ %s\n", what);
}
// 空池（D-B3PR）：游戏拿空池调取值入口，宿主不解引用、回 0 / NULL；空池建缓冲回失败。期望按「宿主不能先崩」手写
static void null_pool(NVNdevice *dev) {
    CHECK(API(nvnMemoryPoolGetFlags)(NULL) == 0, "空池 GetFlags 应回 0");
    CHECK(API(nvnMemoryPoolGetSize)(NULL) == 0, "空池 GetSize 应回 0");
    CHECK(API(nvnMemoryPoolGetBufferAddress)(NULL) == 0, "空池 GetBufferAddress 应回 0");
    CHECK(API(nvnMemoryPoolMap)(NULL) == NULL, "空池 Map 应回 NULL");
    NVNbuffer buf; NVNbufferBuilder bb; memset(&buf, 0, sizeof buf); memset(&bb, 0, sizeof bb);
    API(nvnBufferBuilderSetDefaults)(&bb); API(nvnBufferBuilderSetDevice)(&bb, dev); API(nvnBufferBuilderSetStorage)(&bb, NULL, 0, 0x170);
    CHECK(!API(nvnBufferInitialize)(&buf, &bb), "空池建缓冲应回失败");
    CHECK(API(nvnBufferGetAddress)(&buf) == 0 && API(nvnBufferMap)(&buf) == NULL, "没建成的缓冲取地址 / 映射回空");
    printf("✓ 空池：取值入口回 0 / NULL，宿主不崩\n");
}
int main(void) {
    NVNdevice dev; NVNdeviceBuilder db; memset(&dev, 0, sizeof dev); memset(&db, 0, sizeof db);
    API(nvnDeviceBuilderSetDefaults)(&db); if (!API(nvnDeviceInitialize)(&dev, &db)) { fprintf(stderr, "FAIL 设备初始化\n"); return 1; }
    one(&dev, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, 1, "CPU 不可访问（0x21，同建池缺省）：两个 Map 都是 NULL");
    one(&dev, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, 0, "CPU 带缓存（0x24）：池 Map = 存储、缓冲 Map = 存储 + 偏移");
    one(&dev, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, 0, "CPU 不经缓存（0x22）：同上");
    null_pool(&dev);
    // 池没有存储（建池时没给 Storage）→ BufferInitialize 回 0；游戏不看返回值照样 Map / 取地址（11-14 D-MDVK）。期望：Map 回 NULL、地址回 0、宿主不崩。
    //   反证：去掉 hle_nvn.c 的 buf_dead → 这里解引用空池，进程段错误
    {
        NVNmemoryPool pool; NVNmemoryPoolBuilder pb; memset(&pool, 0, sizeof pool); memset(&pb, 0, sizeof pb);
        API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
        API(nvnMemoryPoolBuilderSetStorage)(&pb, NULL, POOL); API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        API(nvnMemoryPoolInitialize)(&pool, &pb);
        NVNbuffer buf; NVNbufferBuilder bb; memset(&buf, 0, sizeof buf); memset(&bb, 0, sizeof bb);
        API(nvnBufferBuilderSetDefaults)(&bb); API(nvnBufferBuilderSetDevice)(&bb, &dev); API(nvnBufferBuilderSetStorage)(&bb, &pool, OFF, LEN);
        CHECK(!API(nvnBufferInitialize)(&buf, &bb), "没存储的池上建缓冲应失败");
        void *bm = API(nvnBufferMap)(&buf); NVNbufferAddress ba = API(nvnBufferGetAddress)(&buf);
        CHECK(bm == NULL && ba == 0, "没初始化成功的缓冲：Map 应 NULL（得 %p）、地址应 0（得 %#llx）", bm, (unsigned long long)ba);
        printf("✓ 池没有存储、缓冲初始化失败后 Map / 取地址：回空，不崩\n");
    }
    // 审计 P1-190（同 #25）：从没 Initialize 过的对象（内容是垃圾）上调刷新 / 作废 / 收尾。期望：宿主不崩、不解引用里面的指针；收尾两次也不崩。
    //   反证：去掉 hle_nvn.c 里池收尾的魔数核对 / 纹理收尾只看 serial / 缓冲刷新不过 buf_dead → 这里解引用 0xa5a5… 段错误
    {
        NVNbuffer buf; NVNmemoryPool pool; NVNtexture tex;
        memset(&buf, 0xa5, sizeof buf); memset(&pool, 0xa5, sizeof pool); memset(&tex, 0xa5, sizeof tex);
        API(nvnBufferFlushMappedRange)(&buf, 0, 64); API(nvnBufferInvalidateMappedRange)(&buf, 0, 64);
        API(nvnMemoryPoolFlushMappedRange)(&pool, 0, 64);   // P1-190 余项：池刷新同样先核魔数（0xa5 填的 mem 非空，原先照样 ft_flush 去读）
        API(nvnMemoryPoolFinalize)(&pool); API(nvnMemoryPoolFinalize)(&pool);
        API(nvnTextureFinalize)(&tex); API(nvnTextureFinalize)(&tex);
        API(nvnBufferFinalize)(&buf);
        checks++; printf("✓ 没初始化过的缓冲刷新 / 作废、池刷新、池收尾两次、纹理收尾两次：不崩\n");
    }
    // 11-16 D-XDQS：缓冲 Finalize 之后游戏仍取地址（真驱动 Finalize 不清对象里存的地址，照样回原值）。期望按真驱动语义：池还活着时 Finalize 后
    //   GetAddress = 原地址、Map = 原映射；池也 Finalize 了 → 回 0 / NULL（不解引用）；Finalize 两次不崩。反证：原先 Finalize 把魔数写 0 → 这里回 0，游戏把 0 + 偏移当地址
    {
        void *mem = NULL; CHECK(posix_memalign(&mem, 65536, POOL) == 0, "分配存储"); memset(mem, 0, POOL);
        NVNmemoryPool pool; NVNmemoryPoolBuilder pb; memset(&pool, 0, sizeof pool); memset(&pb, 0, sizeof pb);
        API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
        API(nvnMemoryPoolBuilderSetStorage)(&pb, mem, POOL); API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        CHECK(API(nvnMemoryPoolInitialize)(&pool, &pb), "池初始化");
        NVNbuffer buf; NVNbufferBuilder bb; memset(&buf, 0, sizeof buf); memset(&bb, 0, sizeof bb);
        API(nvnBufferBuilderSetDefaults)(&bb); API(nvnBufferBuilderSetDevice)(&bb, &dev); API(nvnBufferBuilderSetStorage)(&bb, &pool, OFF, LEN);
        CHECK(API(nvnBufferInitialize)(&buf, &bb), "缓冲初始化");
        NVNbufferAddress ba0 = API(nvnBufferGetAddress)(&buf); void *bm0 = API(nvnBufferMap)(&buf);
        API(nvnBufferFinalize)(&buf);
        NVNbufferAddress ba1 = API(nvnBufferGetAddress)(&buf); void *bm1 = API(nvnBufferMap)(&buf);
        CHECK(ba0 && ba1 == ba0 && bm1 == bm0, "Finalize 后池还活着：地址应 %#llx（得 %#llx）、Map 应 %p（得 %p）", (unsigned long long)ba0, (unsigned long long)ba1, bm0, bm1);
        API(nvnBufferFinalize)(&buf);
        API(nvnMemoryPoolFinalize)(&pool);
        NVNbufferAddress ba2 = API(nvnBufferGetAddress)(&buf); void *bm2 = API(nvnBufferMap)(&buf);
        CHECK(ba2 == 0 && bm2 == NULL, "池也 Finalize 后：地址应 0（得 %#llx）、Map 应 NULL（得 %p）", (unsigned long long)ba2, bm2);
        NVNbuffer zero; memset(&zero, 0, sizeof zero);
        CHECK(API(nvnBufferGetAddress)(&zero) == 0, "从没初始化过（全零）的缓冲：地址应 0");
        free(mem); printf("✓ 缓冲 Finalize 后池还活着取地址 / Map 回原值；池也收尾后回空；全零对象回空\n");
    }
    printf("✓ t_nvn_map_noaccess：%u 项全过\n", checks);
    return 0;
}
