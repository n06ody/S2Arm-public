// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 大内存池按块用到才包（马车世界缺口 ②，nvn_mtl.m big_pool / big_lookup，照上游参考实现（MIT））。期望按块的规则手算（BIG_S 16 MB、不知道长度给 64 MB、块盖到要读的末尾向上取步长再多一个步长），不由被测实现推出：
//   ① 640 MB 的池登记后：没有任何缓冲条目（大池记录 1 个）
//   ② GPU 查 100 MB + 16 起 64 字节：建 [96, 128) MB 的块，缓冲长 32 MB，偏移 4 MB + 16
//   ③ 同一步长查 100 MB 起 40 MB：同一条目加长到 [96, 160) MB（缓冲 64 MB），被替下的旧缓冲 1 个
//   ④ 查 300 MB 起长度 1（不知道读多远）：[288, 384) MB，96 MB
//   ⑤ CPU 读 500 MB 处（addr_cpu）：给游戏地址本身、不建块（条目数不变）
//   ⑥ 内容：CPU 在 100 MB + 16 写的 16 字节，GPU 经块的缓冲拷出来一样
//   ⑦ 池注销：当时照旧可查（记录 1、块 2、别名 1）；过两次提交（提交序号 +2、pool_reap）后大池记录、块条目、指着旧块的别名都没了
//   ⑩ 池表下标：④ 之前登记一个小池，④ 的块排在池表末尾；删掉小池（末尾那条挪进空位）、再登记一个小池占住块原来的下标后，再查 300 MB 处仍是 ④ 那块缓冲（块表下标跟着改）
//   ⑨ 虚拟池地址别名到块（#1143 nvn_be_pool_alias）：登记 1 条；块被 ③ 加长替换、池注销后这条也没了（旧块缓冲要算进「被放掉的」）
//   ⑧ DM_NVN_POOL_CHUNKS=0（子进程，对照）：登记即建整池一个缓冲，长 640 MB
//   ⑪ ⑫ 块范围里有脏页跟踪设成只读的页时建块：GPU 经块读出 CPU 写的数据、写进的数据落到游戏内存（期望 = CPU 写的常量）
//   ⑬ 浮层「GPU 已分配」不计零拷贝块（#1827，s2a_gpu_alloc_bytes）：② ③ 建块前后，设备总量至少涨 32 + 64 = 96 MB（② 的块被 ③ 替下、仍活着），浮层读数涨不到 1 MB
//   ⑭ 零拷贝缓冲放掉后减回：单建一个 16 MB 的零拷贝缓冲，记账 +16 MB；放掉后回到建前
//   ⑮ Vulkan 前端的入口 mtl_nocopy_buf（hle_vk.m / vk_snap.inc 用）：同一段 32 MB 内存上包两个重叠缓冲 [0, 32) MB、[16, 32) MB——设备总量涨 32 + 16 = 48 MB，
//     浮层读数涨不到 1 MB（这段内存只计一次：在进程实占里，不进 GPU 已分配）；两个都放掉后记账回到建前
//   ⑯ 零拷贝缓冲只有一个建法：nvn_mtl.m 里 newBufferWithBytesNoCopy: 只出现 1 次（nocopy_buf 里），hle_vk.m、vk_snap.inc 里 0 次
//   ⑱ 状态表存取（子进程；快照 gfx_state.bin / 帧录 state.bin）：640 MB 池查出两块 → fr_dump_state → 会话销毁 → fr_restore_state →
//      大池记录 1（整池）、池表 0 条；存之前没查到过的 500 MB 处按长度 1 查：查到、往后可读 ≥ 64 MB、不走地址兜底。反证：nvn_frame_rec.inc 照块写 → 两项红
//   ㉒ ㉓ 一个大池的零拷贝缓冲合计有上限（子进程）：块互相重叠、加长替下的旧缓冲也活着，设备的 GPU 已分配对每个按整块长度记。再建一块就超过池长一半时改包整池一个缓冲。
//      ㉒ 896 MB 池 56 个步长跳着各查一次（长度 1）：4 块 + 整池 = 1280 MB；㉓ 640 MB 池同一步长读 16、32 … 480 MB：块 128 + 整池 640 + 旧缓冲 192 = 960 MB。
//      反证：去掉 big_lookup 里改整池那段 → ㉒ 56 个 5136 MB、㉓ 480 + 3360 MB，两项红
//   ㉔ 改整池后块与旧块按两次提交到期（D-3N8R）：过两次提交（g_sub_serial += 2、pool_reap）后 ㉒ ㉓ 两个池各只剩整池一个缓冲（896 / 640 MB）、旧缓冲 0，
//      被放掉的 4 块 + 1 块 + 3 个旧缓冲 = 8 个进延后释放队列；查找照常落到整池缓冲。到期前（刚改整池）照旧 5 个 / 2 个 + 旧缓冲（㉒ ㉓ 本身）。
//      反证：去掉 big_lookup 里改整池时标块到期、旧块 old_dead 那两行 → ㉔ 红（5 个 1280 MB、2 个 768 MB + 192 MB，延后释放 0）
//   反证：s2a_gpu_alloc_bytes 改回只读 currentAllocatedSize → ⑬ ⑮ 红（涨 96 / 48 MB）；nocopy_buf 的 deallocator 不减 → ⑭ ⑮ 红；hle_vk.m 改回直接建 → ⑯ 红
//   反证：去掉 nvn_be_pool_add_body 里登记大池那段 → ① ② ③ ④ 红（整池一个缓冲）；去掉注销时把旧块缓冲算进被放掉的那两行 → ⑨ 注销后 1 条红；去掉 big_new 建块前的 dm_guest_write_prepare → ⑫ 红
#include "../gfx/nvn/nvn_mtl.m"
#include <sys/wait.h>

static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"SETUP FAIL %s:%d %s\n",__FILE__,__LINE__,#c); exit(2); } } while (0)
#define CHECK(c, ...) do { checks++; int ok=!!(c); failures+=!ok; printf("%s ",ok?"✓":"✗"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
#define MB (1ull << 20)
static int aliases(void) { int n=0; pthread_mutex_lock(&g_mu); for (int i=0;i<g_npool;i++) n+=g_pools[i].alias; pthread_mutex_unlock(&g_mu); return n; }
static int entries_of(const void *mem) { int n=0; pthread_mutex_lock(&g_mu); for (int i=0;i<g_npool;i++) n+=g_pools[i].key==mem; pthread_mutex_unlock(&g_mu); return n; }

static void t_win(uint64_t *b, uint64_t *s) { *b = 0; *s = ~0ull >> 1; }   // ⑱ 地址兜底的客体窗口：全放开（照块写的旧行为下兜底能成，读数才对得上真机）
static int child(int chunks) {
    alarm(60); unsetenv("NXGAL_BE");
    if (chunks) unsetenv("DM_NVN_POOL_CHUNKS"); else setenv("DM_NVN_POOL_CHUNKS","0",1);
    mtl_init();
    const uint64_t SZ = 640 * MB; uint8_t *mem = NULL; REQUIRE(!posix_memalign((void **)&mem, 16*MB, SZ));   // 16 MB 对齐：块按绝对地址取步长，期望好手算
    nvn_be_pool_add(mem, SZ);
    if (chunks == 3) {   // ㉒ ㉓ 一个大池的零拷贝缓冲合计有上限（块互相重叠、加长替下的旧缓冲也活着，各自整块记进设备的 GPU 已分配）
        const uint64_t P = 896 * MB; uint8_t *m = NULL; REQUIRE(!posix_memalign((void **)&m, 16*MB, P)); memset(m, 0, 16384); nvn_be_pool_add(m, P);
        uint64_t dev0 = (uint64_t)g_dev.currentAllocatedSize; int wrong = 0, miss = 0; void *bp; uint64_t off, gpu;
        for (int j = 0; j < 56; j++) { int k = j * 23 % 56;   // 56 个步长各查一次步长中间、不知道读多远；每次跳开 368 MB（23 与 56 互质：每个步长正好一次），不落在上一次查到的那块里
            uint64_t a = (uint64_t)(uintptr_t)m + (uint64_t)k * 16*MB + 8*MB;
            if (!mtl_gpu_addr(a, 1, &bp, &off, &gpu)) { miss++; continue; }
            id<MTLBuffer> bb = (__bridge id<MTLBuffer>)bp; wrong += (uintptr_t)bb.contents + off != (uintptr_t)a || gpu != bb.gpuAddress + off || bb.length - off < (P - ((uint64_t)k * 16*MB + 8*MB) < 64*MB ? P - ((uint64_t)k * 16*MB + 8*MB) : 64*MB);
        }
        uint64_t sum = 0, olds = 0; int n = 0; pthread_mutex_lock(&g_mu); for (int i=0;i<g_npool;i++) if (g_pools[i].key==m) { n++; sum += g_pools[i].buf.length; }
        int full = 0; for (int q=0;q<g_nbig;q++) if (g_big[q].mem==m) { full = g_big[q].full; for (id<MTLBuffer> o in g_big[q].old) olds += o.length; } pthread_mutex_unlock(&g_mu);
        uint64_t dd = (uint64_t)g_dev.currentAllocatedSize - dev0;
        CHECK(!miss && !wrong,"㉒ 896 MB 池 56 个步长各查一次（长度 1）：查不到 %d、地址不对或往后读不到 64 MB 的 %d（期望 0、0）",miss,wrong);
        // 手算：前 4 次各建一块 96 MB（384 MB），第 5 块会到 480 > 448（池的一半）→ 改整池：4 块 + 整池 1 个 = 5 个、384 + 896 = 1280 MB。设备总量是全局量，只报不判
        CHECK(sum + olds == 1280*MB && n == 5 && full,"㉒ 查完：这个池的零拷贝缓冲 %d 个合计 %llu MB + 旧缓冲 %llu MB、整池缓冲 %s（期望 5 个 1280 MB、有；不设上限是 56 个 5136 MB）；设备总量涨 %llu MB",n,(unsigned long long)(sum/MB),(unsigned long long)(olds/MB),full?"有":"无",(unsigned long long)(dd/MB));
        const uint64_t P2 = 640 * MB; uint8_t *m2 = NULL; REQUIRE(!posix_memalign((void **)&m2, 16*MB, P2)); nvn_be_pool_add(m2, P2);   // ㉓ 同一步长越读越远：每次加长都替下一个旧缓冲
        dev0 = (uint64_t)g_dev.currentAllocatedSize; miss = wrong = 0;
        for (int j = 1; j <= 30; j++) { uint64_t a = (uint64_t)(uintptr_t)m2, len = (uint64_t)j * 16*MB;
            if (!mtl_gpu_addr(a, len, &bp, &off, &gpu)) { miss++; continue; }
            id<MTLBuffer> bb = (__bridge id<MTLBuffer>)bp; wrong += (uintptr_t)bb.contents + off != (uintptr_t)a || bb.length - off < len; }
        sum = olds = 0; n = 0; pthread_mutex_lock(&g_mu); for (int i=0;i<g_npool;i++) if (g_pools[i].key==m2) { n++; sum += g_pools[i].buf.length; }
        full = 0; for (int q=0;q<g_nbig;q++) if (g_big[q].mem==m2) { full = g_big[q].full; for (id<MTLBuffer> o in g_big[q].old) olds += o.length; } pthread_mutex_unlock(&g_mu);
        dd = (uint64_t)g_dev.currentAllocatedSize - dev0;
        // 手算（块长 = 要读的长度向上取步长再多一个步长；已有的块够长就用它）：读 16 MB 建 32 MB，读 32 MB 用它；读 48 MB 建 64 MB（32 被替下），读 64 MB 用它；读 80 MB 建 96 MB；
        //   读 112 MB 建 128 MB（合计 32 + 64 + 96 + 128 = 320，没超过一半 320，照建）；读 144 MB 要 160 MB、到 480 > 320 → 改整池。
        //   留在表里：128 MB 的块 + 整池 640 = 768 MB；旧缓冲 32 + 64 + 96 = 192 MB；合计 960 MB = 池的 1.5 倍（正好压在上限：块的取整规则一动这里就变）
        CHECK(!miss && !wrong && sum == 768*MB && olds == 192*MB && n == 2 && full,"㉓ 640 MB 池同一步长读 16、32 … 480 MB：查不到 %d、不对 %d；缓冲 %d 个合计 %llu MB + 旧缓冲 %llu MB、整池缓冲 %s（期望 2 个 768 MB + 192 MB、有；不设上限是 480 + 3360 MB）；设备总量涨 %llu MB",miss,wrong,n,(unsigned long long)(sum/MB),(unsigned long long)(olds/MB),full?"有":"无",(unsigned long long)(dd/MB));
        // ㉔ 过两次提交：改整池时到期的块与旧块走了，只剩整池缓冲
        pthread_mutex_lock(&g_deferred_mu); unsigned long d0 = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
        pthread_mutex_lock(&g_mu); g_sub_serial += 2; pool_reap(0, 0); pthread_mutex_unlock(&g_mu);
        pthread_mutex_lock(&g_deferred_mu); unsigned long d1 = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
        uint64_t s1 = 0, s2 = 0, o1 = 0, o2 = 0; int n1 = 0, n2 = 0; pthread_mutex_lock(&g_mu);
        for (int i=0;i<g_npool;i++) { if (g_pools[i].key==m) { n1++; s1 += g_pools[i].buf.length; } if (g_pools[i].key==m2) { n2++; s2 += g_pools[i].buf.length; } }
        for (int q=0;q<g_nbig;q++) { for (id<MTLBuffer> o in g_big[q].old) { if (g_big[q].mem==m) o1 += o.length; if (g_big[q].mem==m2) o2 += o.length; } } pthread_mutex_unlock(&g_mu);
        int hit1 = mtl_gpu_addr((uint64_t)(uintptr_t)m + 700*MB, 64, &bp, &off, &gpu) && ((__bridge id<MTLBuffer>)bp).length == P && off == 700*MB;
        int hit2 = mtl_gpu_addr((uint64_t)(uintptr_t)m2 + 16, 64, &bp, &off, &gpu) && ((__bridge id<MTLBuffer>)bp).length == P2 && off == 16;
        CHECK(n1 == 1 && s1 == P && o1 == 0 && n2 == 1 && s2 == P2 && o2 == 0 && d1 - d0 == 8 && hit1 && hit2,
              "㉔ 过两次提交：896 MB 池 %d 个 %llu MB + 旧 %llu MB、640 MB 池 %d 个 %llu MB + 旧 %llu MB、进延后释放 %lu 个、查找落到整池 %d %d（期望 1 个 896 + 0、1 个 640 + 0、8、1 1；不放是 5 个 1280、2 个 768 + 192、0）",
              n1,(unsigned long long)(s1/MB),(unsigned long long)(o1/MB),n2,(unsigned long long)(s2/MB),(unsigned long long)(o2/MB),d1 - d0,hit1,hit2);
        printf("%s t_nvn_big_pool 子进程（缓冲合计上限）：%u 项，%u 项失败\n",failures?"✗":"✓",checks,failures); return failures!=0;
    }
    if (chunks == 2) {   // ⑱ 状态表（快照 gfx_state.bin / 帧录 state.bin）存取大池：存整池一条、不存块
        g_adhoc_win = t_win;
        void *bp0; uint64_t off0, gpu0; REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)mem + 100*MB + 16, 64, &bp0, &off0, &gpu0));
        REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)mem + 300*MB, 1, &bp0, &off0, &gpu0));
        char path[256]; snprintf(path, sizeof path, "%s/t_nvn_big_pool.%d.bin", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
        REQUIRE(entries_of(mem)==2 && fr_dump_state(path));
        REQUIRE(!nvn_be_shutdown_session()); nvn_be_begin_session();
        int ntex = 0; REQUIRE(fr_restore_state(path, &ntex)); unlink(path);
        uint64_t ad0 = atomic_load(&g_adhoc_n);
        { pthread_mutex_lock(&g_mu); int nb = g_nbig, ne = g_npool; uintptr_t blo = nb ? g_big[0].lo : 0, bhi = nb ? g_big[0].hi : 0; pthread_mutex_unlock(&g_mu);
          CHECK(nb==1 && ne==0 && blo==(uintptr_t)mem && bhi==(uintptr_t)mem + SZ,"⑱ 放回后：大池记录 %d、池表 %d 条、记录范围 %s（期望 1、0、整池 640 MB；照块写则记录 0、池表 2 条）",nb,ne,blo==(uintptr_t)mem && bhi==(uintptr_t)mem + SZ?"整池":"不对"); }
        void *bp1; uint64_t off1, gpu1; int got = mtl_gpu_addr((uint64_t)(uintptr_t)mem + 500*MB + 0xbf00, 1, &bp1, &off1, &gpu1);   // 存之前没查到过的一段，不知道读多远
        uint64_t room = got ? ((__bridge id<MTLBuffer>)bp1).length - off1 : 0;
        CHECK(got && room >= 64*MB && atomic_load(&g_adhoc_n)==ad0,"⑱ 放回后查没碰过的 500 MB 处（长度 1）：查到 %d、往后可读 %llu MB、走地址兜底 %llu 次（期望 1、≥ 64、0；照块写则兜底 1 次、可读不到 64 KB）",got,(unsigned long long)(room/MB),(unsigned long long)(atomic_load(&g_adhoc_n)-ad0));
        printf("%s t_nvn_big_pool 子进程（状态表存取）：%u 项，%u 项失败\n",failures?"✗":"✓",checks,failures); return failures!=0;
    }
    if (!chunks) {
        pthread_mutex_lock(&g_mu); uint64_t len = 0; int n = 0; for (int i=0;i<g_npool;i++) if (g_pools[i].key==mem) { n++; len = g_pools[i].buf.length; } int nb = g_nbig; pthread_mutex_unlock(&g_mu);
        CHECK(nb==0 && n==1 && len==SZ,"⑧ DM_NVN_POOL_CHUNKS=0：大池记录 %d（期望 0）、整池缓冲 %d 个长 %llu MB（期望 1 个 640）",nb,n,(unsigned long long)(len/MB));
        printf("%s t_nvn_big_pool 子进程（DM_NVN_POOL_CHUNKS=0）：%u 项，%u 项失败\n",failures?"✗":"✓",checks,failures); return failures!=0;
    }
    { pthread_mutex_lock(&g_mu); int nb = g_nbig; pthread_mutex_unlock(&g_mu);
      CHECK(nb==1 && entries_of(mem)==0,"① 640 MB 池登记后：大池记录 %d（期望 1）、缓冲条目 %d（期望 0）",nb,entries_of(mem)); }
    void *bp; uint64_t off, gpu;
    uint64_t dev0 = (uint64_t)g_dev.currentAllocatedSize, hud0 = s2a_gpu_alloc_bytes();
    REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)mem + 100*MB + 16, 64, &bp, &off, &gpu));
    id<MTLBuffer> b1 = (__bridge id<MTLBuffer>)bp;
    CHECK(b1.length==32*MB && off==4*MB+16 && entries_of(mem)==1,"② 查 100 MB+16 起 64 字节：块缓冲 %llu MB（期望 32）、偏移 %#llx（期望 %#llx）、条目 %d（期望 1）",(unsigned long long)(b1.length/MB),(unsigned long long)off,(unsigned long long)(4*MB+16),entries_of(mem));
    const uint64_t VA = 0x10000100000ull; int aok = nvn_be_pool_alias(VA, 64, (uint64_t)(uintptr_t)mem + 100*MB + 16);   // ⑨ 虚拟池地址别名到 ② 的块（#1143）
    CHECK(aok && aliases()==1,"⑨ 虚拟池地址别名到 ② 的块：登记 %d、别名条目 %d（期望 1、1）",aok,aliases());
    REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)mem + 100*MB, 40*MB, &bp, &off, &gpu));
    id<MTLBuffer> b2 = (__bridge id<MTLBuffer>)bp; int nold; pthread_mutex_lock(&g_mu); nold = (int)g_big[0].old.count; pthread_mutex_unlock(&g_mu);
    CHECK(b2.length==64*MB && off==4*MB && entries_of(mem)==1 && nold==1,"③ 同步长查 100 MB 起 40 MB：缓冲 %llu MB（期望 64）、偏移 %#llx（期望 %#llx）、条目 %d（期望 1）、旧缓冲 %d（期望 1）",(unsigned long long)(b2.length/MB),(unsigned long long)off,(unsigned long long)(4*MB),entries_of(mem),nold);
    { uint64_t dd = (uint64_t)g_dev.currentAllocatedSize - dev0, dh = s2a_gpu_alloc_bytes() - hud0;
      CHECK(dd >= 96*MB && dh < MB,"⑬ ② ③ 建块：设备总量涨 %llu MB（期望 ≥ 96）、浮层 GPU 已分配涨 %llu KB（期望 < 1024，零拷贝块不计）",(unsigned long long)(dd/MB),(unsigned long long)(dh>>10)); }
    { uint64_t z0 = atomic_load(&g_nocopy_bytes), z1; uint8_t *m16 = NULL; REQUIRE(!posix_memalign((void **)&m16, 16384, 16*MB));
      @autoreleasepool { id<MTLBuffer> nb = nocopy_buf(m16, 16*MB, nil); REQUIRE(nb); z1 = atomic_load(&g_nocopy_bytes); nb = nil; }
      uint64_t z2 = atomic_load(&g_nocopy_bytes);
      CHECK(z1 - z0 == 16*MB && z2 == z0,"⑭ 单建 16 MB 零拷贝缓冲：记账 +%llu MB（期望 16）；放掉后与建前差 %lld 字节（期望 0）",(unsigned long long)((z1-z0)/MB),(long long)(z2-z0));
      free(m16); }
    { uint8_t *m32 = NULL; REQUIRE(!posix_memalign((void **)&m32, 16384, 32*MB)); uint64_t z0 = atomic_load(&g_nocopy_bytes), d0, d1, h0, h1;
      @autoreleasepool { d0 = (uint64_t)g_dev.currentAllocatedSize; h0 = s2a_gpu_alloc_bytes();
        id<MTLBuffer> x = (__bridge_transfer id<MTLBuffer>)mtl_nocopy_buf(m32, 32*MB), y = (__bridge_transfer id<MTLBuffer>)mtl_nocopy_buf(m32 + 16*MB, 16*MB); REQUIRE(x && y);
        d1 = (uint64_t)g_dev.currentAllocatedSize; h1 = s2a_gpu_alloc_bytes(); x = nil; y = nil; }
      uint64_t z2 = atomic_load(&g_nocopy_bytes);
      CHECK(d1 - d0 == 48*MB && h1 - h0 < MB && z2 == z0,"⑮ Vulkan 入口包两个重叠缓冲（32 + 16 MB）：设备总量涨 %llu MB（期望 48）、浮层涨 %llu KB（期望 < 1024）、放掉后记账差 %lld 字节（期望 0）",
            (unsigned long long)((d1-d0)/MB),(unsigned long long)((h1-h0)>>10),(long long)(z2-z0));
      free(m32); }
    { static const char *const fs[3] = {"nvn_mtl.m","../vk/hle_vk.m","../vk/vk_snap.inc"}; static const int want[3] = {1,0,0}; int got[3], same = 1;
      char dir[1024]; snprintf(dir, sizeof dir, "%s", __FILE__); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
      for (int i = 0; i < 3; i++) { char p[1200]; snprintf(p, sizeof p, "%s/../gfx/nvn/%s", dir, fs[i]); NSString *t = [NSString stringWithContentsOfFile:@(p) encoding:NSUTF8StringEncoding error:nil]; REQUIRE(t);
        got[i] = (int)[t componentsSeparatedByString:@"newBufferWithBytesNoCopy:"].count - 1; same &= got[i] == want[i]; }
      CHECK(same,"⑯ 零拷贝缓冲只有一个建法：nvn_mtl.m %d 处（期望 1）、hle_vk.m %d 处、vk_snap.inc %d 处（期望 0、0）",got[0],got[1],got[2]); }
    uint8_t *small = NULL; REQUIRE(!posix_memalign((void **)&small, 16384, 65536)); nvn_be_pool_add(small, 65536);   // ⑩：块建在它之后、排在末尾
    REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)mem + 300*MB, 1, &bp, &off, &gpu));
    id<MTLBuffer> b3 = (__bridge id<MTLBuffer>)bp;
    CHECK(b3.length==96*MB && off==12*MB && entries_of(mem)==2,"④ 查 300 MB 起长度 1：缓冲 %llu MB（期望 96）、偏移 %#llx（期望 %#llx）、条目 %d（期望 2）",(unsigned long long)(b3.length/MB),(unsigned long long)off,(unsigned long long)(12*MB),entries_of(mem));
    { pthread_mutex_lock(&g_mu); int is = -1, last_is_b3 = g_npool > 0 && g_pools[g_npool - 1].buf == b3; for (int i = 0; i < g_npool; i++) if (g_pools[i].key == small) is = i;
      REQUIRE(is >= 0 && last_is_b3); pool_remove_at(is); pthread_mutex_unlock(&g_mu);
      uint8_t *small2 = NULL; REQUIRE(!posix_memalign((void **)&small2, 16384, 65536)); nvn_be_pool_add(small2, 65536);   // 新池占住块原来的下标（末尾）
      REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)mem + 300*MB, 1, &bp, &off, &gpu));
      CHECK((__bridge id<MTLBuffer>)bp == b3 && off == 12*MB && entries_of(mem) == 2, "⑩ 删掉排在前面的小池（④ 的块挪进它的位置）后再查 300 MB 处：同一块缓冲 %d、偏移 %#llx（期望 1、%#llx）、条目 %d（期望 2，不另建）",
            (__bridge id<MTLBuffer>)bp == b3, (unsigned long long)off, (unsigned long long)(12*MB), entries_of(mem)); }
    const uint8_t *c = addr_cpu((uint64_t)(uintptr_t)mem + 500*MB, 4);
    CHECK(c==mem+500*MB && entries_of(mem)==2,"⑤ CPU 读 500 MB 处：给游戏地址本身 %d、条目 %d（期望 2，不建块）",c==mem+500*MB,entries_of(mem));
    { uint32_t pat[4]={0x11223344,0x55667788,0x99aabbcc,0xddeeff00}; memcpy(mem+100*MB+16,pat,16);
      REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)mem + 100*MB + 16, 16, &bp, &off, &gpu)); id<MTLBuffer> src=(__bridge id<MTLBuffer>)bp;
      id<MTLBuffer> dst=[g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; id<MTLCommandQueue> q=[g_dev newCommandQueue];
      id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
      [be copyFromBuffer:src sourceOffset:off toBuffer:dst destinationOffset:0 size:16]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
      CHECK(!memcmp(dst.contents,pat,16),"⑥ GPU 经块的缓冲拷出 CPU 写的 16 字节：一致 %d",!memcmp(dst.contents,pat,16)); }
    // ⑪ ⑫ 块范围里有被脏页跟踪设成只读的页（上传过的贴图存储）时建块：GPU 读、写这块缓冲都要落到游戏内存（11-19：建块时范围里有只读页，GPU 写整块无声丢失）
    { nvn_dirty_add_range(mem, SZ); uint8_t *ro = mem + 196*MB, *in = mem + 200*MB, *outp = mem + 204*MB;   // 同在 [192, 208) MB 这一步长
      const uint32_t pin[4]={0x0badf00d,0x12345678,0x9abcdef0,0x0f1e2d3c}, pout[4]={0xcafebabe,0x31415926,0x27182818,0x16180339}; memcpy(in,pin,16); memset(outp,0,16);
      nvn_dirty_protect(ro, nvn_dirty_page_size());
      CHECK(nvn_dirty_page_armed(nvn_dirty_span(ro, 1), ro),"⑪ 前提：196 MB 那页已设只读（脏页跟踪）");
      REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)in, 16, &bp, &off, &gpu)); id<MTLBuffer> ch=(__bridge id<MTLBuffer>)bp; uint64_t base=(uint64_t)((uintptr_t)in - off);
      CHECK(base <= (uint64_t)(uintptr_t)ro && (uint64_t)(uintptr_t)ro < base + ch.length,"⑪ 前提：新建的块盖住那页只读页（块 [%+lld, +%llu MB)）",(long long)(base - (uint64_t)(uintptr_t)mem)/(long long)MB,(unsigned long long)(ch.length/MB));
      id<MTLBuffer> dst=[g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared], src=[g_dev newBufferWithBytes:pout length:16 options:MTLResourceStorageModeShared];
      id<MTLCommandQueue> q=[g_dev newCommandQueue]; id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
      [be copyFromBuffer:ch sourceOffset:off toBuffer:dst destinationOffset:0 size:16];
      [be copyFromBuffer:src sourceOffset:0 toBuffer:ch destinationOffset:off + 4*MB size:16]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
      CHECK(cb.status==MTLCommandBufferStatusCompleted && !memcmp(dst.contents,pin,16),"⑪ GPU 经这块读 CPU 写的 16 字节：一致 %d（命令缓冲状态 %ld）",!memcmp(dst.contents,pin,16),(long)cb.status);
      CHECK(!memcmp(outp,pout,16),"⑫ GPU 经这块写的 16 字节落到游戏内存：一致 %d（读回 %08x…，期望 %08x…）",!memcmp(outp,pout,16),((uint32_t *)outp)[0],pout[0]);
      nvn_dirty_remove_range(mem); }
    nvn_be_pool_del(mem);
    { pthread_mutex_lock(&g_mu); int nb = g_nbig; pthread_mutex_unlock(&g_mu);   // 注销按回放顺序：游戏释放后再过两次提交才回收（pool_reap），期间照旧可查
      CHECK(nb==1 && entries_of(mem)==3 && aliases()==1,"⑦ 刚注销：大池记录 %d、块条目 %d、别名 %d（期望 1、3、1：[96, 160)、[288, 384)、⑪ 的 [192, 224) 三块，到期前照旧可查）",nb,entries_of(mem),aliases()); }
    pthread_mutex_lock(&g_mu); g_sub_serial += 2; pool_reap(0, 0); pthread_mutex_unlock(&g_mu);   // 过两次提交：nvn_be_sub_begin 里回收的那一步（它本身要配对的提交收尾，这里直接走回收）
    { pthread_mutex_lock(&g_mu); int nb = g_nbig; pthread_mutex_unlock(&g_mu);
      CHECK(nb==0 && entries_of(mem)==0,"⑦ 池注销：大池记录 %d、块条目 %d（期望都 0）",nb,entries_of(mem)); }
    CHECK(aliases()==0,"⑨ 池注销后：指着被 ③ 加长替下的旧块缓冲的别名条目 %d（期望 0；旧块不在池表里，原先留下指向已放缓冲的条目）",aliases());
    // ⑰ 进程内重开一局（会话关 → 开）后大池记录要清（D-Q349 等 8 份诊断「insertObject:atIndex: object cannot be nil」，都在重开后的那局）：期望手写——
    //   重开前登记一个 640 MB 大池、查 100 MB 处建块；重开后大池记录 0；同一块内存重新登记、查同一处建块、再按 40 MB 查（加长）都成功、条目 1、旧块缓冲 1。
    //   旧实现：重开后大池记录仍 1，块表指着旧池表下标，重新登记后查同一处直接用旧下标读新表，加长时把空缓冲塞进 old 抛异常
    { uint8_t *m2 = NULL; REQUIRE(!posix_memalign((void **)&m2, 16*MB, SZ)); nvn_be_pool_add(m2, SZ);
      REQUIRE(mtl_gpu_addr((uint64_t)(uintptr_t)m2 + 100*MB, 64, &bp, &off, &gpu));
      REQUIRE(!nvn_be_shutdown_session()); nvn_be_begin_session();
      pthread_mutex_lock(&g_mu); int nb = g_nbig; pthread_mutex_unlock(&g_mu);
      CHECK(nb==0,"⑰ 重开后大池记录 %d（期望 0）",nb);
      nvn_be_pool_add(m2, SZ);
      int a1 = mtl_gpu_addr((uint64_t)(uintptr_t)m2 + 100*MB, 64, &bp, &off, &gpu), a2 = mtl_gpu_addr((uint64_t)(uintptr_t)m2 + 100*MB, 40*MB, &bp, &off, &gpu);
      pthread_mutex_lock(&g_mu); int nold = g_nbig ? (int)g_big[0].old.count : -1; nb = g_nbig; pthread_mutex_unlock(&g_mu);
      CHECK(a1 && a2 && nb==1 && entries_of(m2)==1 && nold==1,"⑰ 重开后同一块内存重新登记：查 %d、加长 %d、大池记录 %d、条目 %d、旧块缓冲 %d（期望 1 1 1 1 1）",a1,a2,nb,entries_of(m2),nold); }
    printf("%s t_nvn_big_pool 子进程（缺省）：%u 项，%u 项失败\n",failures?"✗":"✓",checks,failures);
    return failures!=0;
}
int main(int argc, char **argv) { @autoreleasepool {
    setvbuf(stdout,NULL,_IOLBF,0);
    if (argc==3 && !strcmp(argv[1],"--child")) return child(atoi(argv[2]));
    unsigned bad=0; static const char *const modes[4]={"1","0","2","3"};   // "3" = ㉒ ㉓
    for (int m=0; m<4; m++) {
        fflush(NULL); pid_t pid=fork(); REQUIRE(pid>=0);
        if (!pid) { execl(argv[0],argv[0],"--child",modes[m],(char *)NULL); _exit(127); }
        int st=0; pid_t w; do { w=waitpid(pid,&st,0); } while (w<0 && errno==EINTR); REQUIRE(w==pid);
        bad+=!(WIFEXITED(st) && WEXITSTATUS(st)==0);
    }
    printf("%s t_nvn_big_pool：缺省 / DM_NVN_POOL_CHUNKS=0 / 状态表存取 / 缓冲合计上限四个子进程，%u 个不过（大内存池按块用到才包）\n",bad?"✗":"✓",bad);
    return bad!=0;
} }
