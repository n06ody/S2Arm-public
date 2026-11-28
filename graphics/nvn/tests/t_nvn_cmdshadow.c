// 命令缓冲影子录制（hle_nvn.c 命令缓冲）：记录一律写宿主块，游戏给的命令内存只当身份和锚点；已用 / 剩余 / 大小与内存回调按设备字节。
// 完整生产 bootstrap、真实队列提交：回放用 UpdateUniformBuffer 的提交期 CPU 可见副作用（往数据池写 16 字节）核对每条记录都被回放、顺序与内容对。
// ⑨ 帧回放能放回影子块（7df29fdfc 后帧回放全红：块是 mmap(NULL) 落在内核挑的地址，回放进程的库 / 堆 / Metal 常占着）：本进程带 S2A_CMD_ARENA=1 当录的进程（构造期占命令记录区），
//   录完列出全部影子块 → 每块都在命令记录区；再起一个子进程当回放进程（S2A_FRAME_REPLAY，同样构造期占区、只占不切），每块都落在子进程的区里且是空的保留（PROT_NONE），回放能 MAP_FIXED 放回原地址。
//   不靠窗口尾部：窗口放不下标准布局的款没有尾部（11-05 哆啦A梦 1350 高，尾部 0 MB，块全退回 mmap(NULL)）
// 设备字节按 hle_nvn.c dev_bytes 的约定手写：UpdateUniformBuffer 16 字节 = 记录大小 8 + 8 + 8 + 16 = 40，DrawArrays 32，DrawElements 24（驱动真值），BindTexture 24，CallCommands 16，END 0（驱动的 EndRecording 只写控制内存）。
#include "nvn_protos.h"
#include "nvn_obj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <mach/mach.h>

extern void *h_nvnBootstrapLoader(const char *name);
extern int mtl_last_commit_error(void);
extern uint64_t s2a_hle_cmdshadow_live(void), s2a_hle_cmdshadow_live_bytes(void), s2a_hle_cmdshadow_shared(void);
extern void s2a_hle_cmdshadow_quarantine_flush(void);
extern int nvn_cmd_call_target(uint64_t h);
static const char *phase = "准备";
static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL [%s] %s:%d：%s\n", phase, __FILE__, __LINE__, #expr); exit(1); \
} } while (0)
static void *load(const char *name) { void *e = h_nvnBootstrapLoader(name); CHECK(e != NULL); return e; }
#define API(name) ((NVNT_##name)load(#name))

enum { CMD_BYTES = 1 << 20, DATA_BYTES = 1 << 20, UB = 40, DRAW = 32, DRAWIDX = 24, BTEX = 24, CALL = 16, END = 0, FILL = 0x5a };
static NVNdevice device;
static NVNqueue queue;
static NVNmemoryPool cmdpool, datapool;
static uint8_t *cmdmem, *datamem;
static NVNbufferAddress data_ga;

static void new_pool(NVNmemoryPool *p, uint8_t **mem, size_t n) {
    CHECK(posix_memalign((void **)mem, 65536, n) == 0); memset(*mem, 0, n);
    NVNmemoryPoolBuilder b = {{0}};
    API(nvnMemoryPoolBuilderSetDefaults)(&b); API(nvnMemoryPoolBuilderSetDevice)(&b, &device);
    API(nvnMemoryPoolBuilderSetFlags)(&b, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&b, *mem, n);
    CHECK(API(nvnMemoryPoolInitialize)(p, &b));
}
static void new_cmd(NVNcommandBuffer *c, size_t off, size_t n) {
    memset(c, 0, sizeof *c); CHECK(API(nvnCommandBufferInitialize)(c, &device));
    if (n) API(nvnCommandBufferAddCommandMemory)(c, &cmdpool, (ptrdiff_t)off, n);
}
// 一条 UpdateUniformBuffer：往数据池第 slot 个 16 字节写 {v, v+1, v+2, v+3}
static void ub(NVNcommandBuffer *c, unsigned slot, uint32_t v) {
    uint32_t d[4] = {v, v + 1, v + 2, v + 3};
    API(nvnCommandBufferUpdateUniformBuffer)(c, data_ga, DATA_BYTES, (ptrdiff_t)slot * 16, 16, d);
}
static void run(NVNcommandHandle h) {
    API(nvnQueueSubmitCommands)(&queue, 1, &h); API(nvnQueueFinish)(&queue);
    CHECK(mtl_last_commit_error() == 0);
}
static int slot_is(unsigned slot, uint32_t v) { const uint32_t *d = (const uint32_t *)(datamem + slot * 16); return d[0] == v && d[1] == v + 1 && d[2] == v + 2 && d[3] == v + 3; }
static size_t used(NVNcommandBuffer *c) { return API(nvnCommandBufferGetCommandMemoryUsed)(c); }
static size_t freeb(NVNcommandBuffer *c) { return API(nvnCommandBufferGetCommandMemoryFree)(c); }
static void untouched(size_t lo, size_t hi) { for (size_t i = lo; i < hi; i++) CHECK(cmdmem[i] == FILL); }
static void fill(void) { memset(cmdmem, FILL, CMD_BYTES); }
static int in_guest(NVNcommandHandle h) { return (uint8_t *)(uintptr_t)h >= cmdmem && (uint8_t *)(uintptr_t)h < cmdmem + CMD_BYTES; }

// 回调：从命令池 ring 起按 grant 字节一段段给（grant = 0：什么也不给），记下次数与每次要的量
static size_t g_ring, g_grant; static unsigned g_cbs; static size_t g_need[64];
static void ring_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent e, size_t need, void *d) {
    (void)d; CHECK(e == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY);
    if (g_cbs < 64) g_need[g_cbs] = need;
    g_cbs++;
    if (g_grant) { CHECK(g_ring + g_grant <= CMD_BYTES); API(nvnCommandBufferAddCommandMemory)(c, &cmdpool, (ptrdiff_t)g_ring, g_grant); g_ring += g_grant; }
}
static void ring(size_t at, size_t grant) { g_ring = at; g_grant = grant; g_cbs = 0; memset(g_need, 0, sizeof g_need); }

// ① 录制期间游戏的命令内存逐字节不动（含中途回调换段），句柄不指向游戏内存；回放照样写对
static void guest_untouched(void) {
    phase = "① 游戏命令内存逐字节不变";
    fill();
    NVNcommandBuffer c; new_cmd(&c, 0, 100);
    API(nvnCommandBufferSetMemoryCallback)(&c, ring_cb); ring(4096, 100);
    API(nvnCommandBufferBeginRecording)(&c);
    for (unsigned i = 0; i < 10; i++) ub(&c, i, 100 + 10 * i);
    API(nvnCommandBufferDrawArrays)(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);   // 不提交：只录不放
    NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&c);
    CHECK(h && !in_guest(h));
    CHECK(g_cbs > 0);   // 确实换过段
    untouched(0, CMD_BYTES);
    // 回放：DrawArrays 那条另录，提交的这份只有写数据
    NVNcommandBuffer d; new_cmd(&d, 8192, 4096); API(nvnCommandBufferBeginRecording)(&d);
    for (unsigned i = 0; i < 10; i++) ub(&d, i, 100 + 10 * i);
    NVNcommandHandle hd = API(nvnCommandBufferEndRecording)(&d);
    untouched(0, CMD_BYTES);
    memset(datamem, 0, 4096); run(hd);
    for (unsigned i = 0; i < 10; i++) CHECK(slot_is(i, 100 + 10 * i));
    untouched(0, CMD_BYTES);
    API(nvnCommandBufferFinalize)(&c); API(nvnCommandBufferFinalize)(&d);
    printf("%s：通过\n", phase); fflush(stdout);
}

// 通过公开入口连续设宽线和默认线，回放必须能按原顺序取到两次宽度。
static void recorded_line_width(void) {
    phase = "SetLineWidth 录制与恢复";
    NVNcommandBuffer c;
    new_cmd(&c, 0x10000, 4096);
    API(nvnCommandBufferBeginRecording)(&c);
    API(nvnCommandBufferSetLineWidth)(&c, 5.0f);
    API(nvnCommandBufferSetLineWidth)(&c, 1.0f);
    NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&c);
    CHECK(h != 0);
    const op_hdr *op = (const op_hdr *)(uintptr_t)h;
    const float expected[] = {5, 1};
    for (size_t i = 0; i < 2; i++) {
        CHECK(op->op == OP_BIND_STATE && op->size >= sizeof *op + sizeof(p_bind_state));
        const p_bind_state *state = (const p_bind_state *)(op + 1);
        CHECK(state->kind == NVN_BIND_LINE_WIDTH);
        float width;
        memcpy(&width, state->bytes, sizeof width);
        CHECK(width == expected[i]);
        op = (const op_hdr *)((const uint8_t *)op + op->size);
    }
    CHECK(op->op == OP_END);
    API(nvnCommandBufferFinalize)(&c);
}

// ② 已用按设备字节；同一张表再录一次已用相同（塞尔达按它定每个模型的预录尺寸）；已用从最近一次 AddCommandMemory 算起、跨录制累加
static void used_by_device(void) {
    phase = "② 已用按设备字节、同表两录相同";
    NVNcommandBuffer c; new_cmd(&c, 0x10000, 4096);
    const size_t want = 3 * UB + DRAW + DRAWIDX + END;   // 208；DrawElements 宿主记录 32、设备 48：两者不同才分得出口径
    for (int round = 0; round < 2; round++) {
        if (round) API(nvnCommandBufferAddCommandMemory)(&c, &cmdpool, 0x11000, 4096);
        API(nvnCommandBufferBeginRecording)(&c);
        for (unsigned i = 0; i < 3; i++) ub(&c, i, 7);
        API(nvnCommandBufferDrawArrays)(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        API(nvnCommandBufferDrawElements)(&c, NVN_DRAW_PRIMITIVE_POINTS, NVN_INDEX_TYPE_UNSIGNED_SHORT, 1, 0x100000000ull);
        CHECK(API(nvnCommandBufferEndRecording)(&c) != 0);
        CHECK(used(&c) == want);
        CHECK(freeb(&c) == 4096 - want);
        CHECK(API(nvnCommandBufferGetCommandMemorySize)(&c) == 4096);
    }
    API(nvnCommandBufferBeginRecording)(&c); ub(&c, 0, 1); CHECK(API(nvnCommandBufferEndRecording)(&c) != 0);
    CHECK(used(&c) == want + UB + END);   // 不重给内存：接着上一段累加（风花雪月拿段起点 + 已用当 GPU 位置）
    API(nvnCommandBufferFinalize)(&c);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ③ 设备口径会超出时才回调游戏，次数与需要量按设备字节；回调不给就调两次后宿主借窗口接着录
static void callback_timing(void) {
    phase = "③ 回调时机与需要量按设备字节";
    NVNcommandBuffer c; new_cmd(&c, 0x20000, 96);
    API(nvnCommandBufferSetMemoryCallback)(&c, ring_cb); ring(0x21000, 100);
    // BindTexture 宿主记录 40 字节、设备 24：4 条 = 96 设备字节正好装下（END 0；宿主 168 字节）——不回调
    API(nvnCommandBufferBeginRecording)(&c);
    for (int i = 0; i < 4; i++) API(nvnCommandBufferBindTexture)(&c, NVN_SHADER_STAGE_FRAGMENT, i, (NVNtextureHandle)(5 | 7u << 20));
    CHECK(API(nvnCommandBufferEndRecording)(&c) != 0);
    CHECK(g_cbs == 0); CHECK(used(&c) == 4 * BTEX + END); CHECK(freeb(&c) == 0);
    // 再给 100：第 5 条（96 + 24 > 100）回调一次、要 24；新段里第 5 条 24（END 0）
    API(nvnCommandBufferAddCommandMemory)(&c, &cmdpool, 0x20100, 100);
    API(nvnCommandBufferBeginRecording)(&c);
    for (int i = 0; i < 5; i++) API(nvnCommandBufferBindTexture)(&c, NVN_SHADER_STAGE_FRAGMENT, i, (NVNtextureHandle)(5 | 7u << 20));
    CHECK(API(nvnCommandBufferEndRecording)(&c) != 0);
    CHECK(g_cbs == 1); CHECK(g_need[0] == BTEX);
    CHECK(used(&c) == BTEX + END); CHECK(freeb(&c) == 100 - BTEX - END);
    // 回调什么也不给：同一条记录回调两次，然后宿主借窗口。已用只数游戏给的这段（审计 #1534 P1-99）：借的那一刻这段算用尽，已用 = 大小 50、剩余 0，
    //   借窗口里的记录不再加（旧实现接着累加成 3 × 40 + 8 = 128 > 大小 50）
    ring(0, 0);
    API(nvnCommandBufferAddCommandMemory)(&c, &cmdpool, 0x20200, 50);
    API(nvnCommandBufferBeginRecording)(&c);
    ub(&c, 0, 1);            // 40 ≤ 50
    ub(&c, 1, 2);            // 80 > 50：回调两次都不给 → 借窗口
    ub(&c, 2, 3);            // 借的窗口里，不再回调
    CHECK(API(nvnCommandBufferEndRecording)(&c) != 0);
    CHECK(g_cbs == 2); CHECK(g_need[0] == UB && g_need[1] == UB);
    CHECK(used(&c) == 50); CHECK(freeb(&c) == 0); CHECK(API(nvnCommandBufferGetCommandMemorySize)(&c) == 50);
    API(nvnCommandBufferFinalize)(&c);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ④ 王国之泪：按测得大小 S 排相邻槽（槽 k 在 base + S × k，给 max(S, 512) 盖到后面几槽），倒着录也各录各的，互不覆盖、回放各自正确
static void totk_slots(void) {
    phase = "④ 王国之泪按测得大小排槽";
    enum { N = 8 };
    NVNcommandBuffer m; new_cmd(&m, 0x30000, 4096);
    API(nvnCommandBufferBeginRecording)(&m); for (unsigned i = 0; i < 3; i++) ub(&m, i, 0);
    CHECK(API(nvnCommandBufferEndRecording)(&m) != 0);
    const size_t S = used(&m); CHECK(S == 3 * UB + END);   // 128 < 512
    const size_t base = 0x40000; NVNcommandHandle h[N];
    NVNcommandBuffer c; new_cmd(&c, 0, 0);
    uint64_t live0 = s2a_hle_cmdshadow_live();
    for (int k = N - 1; k >= 0; k--) {
        API(nvnCommandBufferAddCommandMemory)(&c, &cmdpool, (ptrdiff_t)(base + S * (size_t)k), S > 512 ? S : 512);
        API(nvnCommandBufferBeginRecording)(&c);
        for (unsigned i = 0; i < 3; i++) ub(&c, (unsigned)k * 3 + i, 1000 * (uint32_t)k + 10 * i);
        h[k] = API(nvnCommandBufferEndRecording)(&c);
        CHECK(used(&c) == S);
    }
    CHECK(s2a_hle_cmdshadow_live() == live0 + N);   // 只给内存不算覆盖：一个也没被释放
    memset(datamem, 0, 4096);
    for (int k = 0; k < N; k++) run(h[k]);
    for (int k = 0; k < N; k++) for (unsigned i = 0; i < 3; i++) CHECK(slot_is((unsigned)k * 3 + i, 1000 * (uint32_t)k + 10 * i));
    // 只重录槽 3：旧槽 3 释放，槽 4 照旧
    API(nvnCommandBufferAddCommandMemory)(&c, &cmdpool, (ptrdiff_t)(base + S * 3), 512);
    API(nvnCommandBufferBeginRecording)(&c); for (unsigned i = 0; i < 3; i++) ub(&c, 9 + i, 77777); h[3] = API(nvnCommandBufferEndRecording)(&c);
    CHECK(s2a_hle_cmdshadow_live() == live0 + N);
    memset(datamem, 0, 4096); run(h[3]); run(h[4]);
    for (unsigned i = 0; i < 3; i++) { CHECK(slot_is(9 + i, 77777)); CHECK(slot_is(12 + i, 4000 + 10 * i)); }
    API(nvnCommandBufferFinalize)(&c); API(nvnCommandBufferFinalize)(&m);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ⑤ 游戏重录覆盖到旧录制的设备区间时，旧录制的宿主块释放；没被覆盖的照样能 CALL。内存池销毁时其中的录制释放
static void release(void) {
    phase = "⑤ 覆盖即释放、未覆盖仍可 CALL";
    const size_t R = 0x50000;
    NVNcommandBuffer x; new_cmd(&x, R, 4096);
    uint64_t live0 = s2a_hle_cmdshadow_live();
    API(nvnCommandBufferBeginRecording)(&x); ub(&x, 0, 500); ub(&x, 1, 510); NVNcommandHandle a = API(nvnCommandBufferEndRecording)(&x);   // [R, R + 80)（END 0）
    API(nvnCommandBufferBeginRecording)(&x); ub(&x, 2, 520); ub(&x, 3, 530); NVNcommandHandle b = API(nvnCommandBufferEndRecording)(&x);   // [R + 80, R + 160)
    CHECK(a && b && s2a_hle_cmdshadow_live() == live0 + 2);
    NVNcommandBuffer y; new_cmd(&y, R, 4096);   // 同一块内存再给出去：只给不写，谁也不释放
    CHECK(s2a_hle_cmdshadow_live() == live0 + 2);
    API(nvnCommandBufferBeginRecording)(&y); ub(&y, 4, 540);   // 写 [R, R + 40)：盖到 a，没到 b
    CHECK(s2a_hle_cmdshadow_live() == live0 + 2);              // a 释放、新录制 +1
    NVNcommandHandle cy = API(nvnCommandBufferEndRecording)(&y);
    CHECK(cy && cy != a);
    NVNcommandBuffer z; new_cmd(&z, 0x58000, 4096);
    API(nvnCommandBufferBeginRecording)(&z); API(nvnCommandBufferCallCommands)(&z, 1, &b); ub(&z, 5, 550);
    NVNcommandHandle hz = API(nvnCommandBufferEndRecording)(&z);
    CHECK(used(&z) == CALL + UB + END);
    memset(datamem, 0, 4096); run(hz);
    CHECK(slot_is(2, 520)); CHECK(slot_is(3, 530)); CHECK(slot_is(5, 550));
    // 紧挨 b 前写满 [R + 40, R + 80)（y 的录制占 [R, R + 40)）：b 不释放、照样能 CALL；从 b 的最后一字节 R + 159 起写一条：b 释放（区间按设备字节记）
    NVNcommandBuffer v; new_cmd(&v, R + 40, 40);
    API(nvnCommandBufferBeginRecording)(&v); ub(&v, 6, 1); CHECK(API(nvnCommandBufferEndRecording)(&v) != 0);   // 40 正好装下（END 0）
    CHECK(s2a_hle_cmdshadow_live() == live0 + 4);   // b、cy、hz、v
    memset(datamem, 0, 4096); run(hz); CHECK(slot_is(2, 520)); CHECK(slot_is(3, 530));
    NVNcommandBuffer u; new_cmd(&u, R + 159, 40);
    API(nvnCommandBufferBeginRecording)(&u); ub(&u, 7, 1); CHECK(API(nvnCommandBufferEndRecording)(&u) != 0);   // 写 [R + 159, R + 199)：盖到 b 的最后一字节
    CHECK(s2a_hle_cmdshadow_live() == live0 + 4);   // b 释放、u +1（空录制不占字节、盖不到任何区间，所以这里写一条）
    // 另一个内存池里的录制：池销毁即释放
    NVNmemoryPool p2; uint8_t *m2; new_pool(&p2, &m2, 65536);
    NVNcommandBuffer w; memset(&w, 0, sizeof w); CHECK(API(nvnCommandBufferInitialize)(&w, &device));
    API(nvnCommandBufferAddCommandMemory)(&w, &p2, 0, 4096);
    API(nvnCommandBufferBeginRecording)(&w); ub(&w, 0, 9); CHECK(API(nvnCommandBufferEndRecording)(&w) != 0);
    uint64_t before = s2a_hle_cmdshadow_live();
    API(nvnMemoryPoolFinalize)(&p2); free(m2);
    CHECK(s2a_hle_cmdshadow_live() == before - 1);
    API(nvnCommandBufferFinalize)(&w); API(nvnCommandBufferFinalize)(&x); API(nvnCommandBufferFinalize)(&y); API(nvnCommandBufferFinalize)(&z);
    API(nvnCommandBufferFinalize)(&v); API(nvnCommandBufferFinalize)(&u);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ⑤b #768338 CALL 目标安全阀：长期列表调用的录制被覆盖、隔离期满块被放回后，旧句柄指向已释放的块。
//   期望手写：录 b（写槽 7）→ 核目标 0（活）；从别处起头的录制盖住 b → 1（隔离中，块完好）；录 z = CALL b + 写槽 8 → 清空隔离区 → -1（落在命令记录区、不是任何录制的句柄）；
//   回放 z：CALL 跳过、槽 7 不被写，槽 8 照写。反证：去掉回放里的 ck < 0 跳过，CALL 跳进已释放的块执行 b 的旧记录，槽 7 被写成 770
static void call_guard(void) {
    phase = "⑤b CALL 目标已被复用 / 释放：跳过这次调用（#768338 安全阀）";
    const size_t R = 0x60000;
    NVNcommandBuffer x; new_cmd(&x, R, 4096);
    API(nvnCommandBufferBeginRecording)(&x); ub(&x, 7, 770); NVNcommandHandle b = API(nvnCommandBufferEndRecording)(&x);
    CHECK(b && nvn_cmd_call_target(b) == 0);
    NVNcommandBuffer y; new_cmd(&y, R - 64, 4096);   // 从 R − 64 起头、三条 UB 盖到 R + 64：b 被盖住，但 R 上不再有录制的开头（同地址重录另见 ⑤c）
    API(nvnCommandBufferBeginRecording)(&y); ub(&y, 9, 990); ub(&y, 15, 1500); ub(&y, 16, 1600); NVNcommandHandle hy = API(nvnCommandBufferEndRecording)(&y);
    CHECK(hy && nvn_cmd_call_target(b) == 1);
    NVNcommandBuffer z; new_cmd(&z, 0x68000, 4096);
    API(nvnCommandBufferBeginRecording)(&z); API(nvnCommandBufferCallCommands)(&z, 1, &b); ub(&z, 8, 880);
    NVNcommandHandle hz = API(nvnCommandBufferEndRecording)(&z);
    s2a_hle_cmdshadow_quarantine_flush();
    int ck = nvn_cmd_call_target(b);
    if (ck != -1) printf("CALL 目标核对：隔离区清空后期望 -1，实 %d\n", ck);
    CHECK(ck == -1);
    CHECK(nvn_cmd_call_target(hz) == 0 && nvn_cmd_call_target((NVNcommandHandle)(uintptr_t)(cmdmem + 16)) == 0);   // 活录制 / 不在命令记录区（游戏内存）都不判
    memset(datamem, 0, 4096); run(hz);
    if (slot_is(7, 770) || !slot_is(8, 880)) printf("CALL 目标安全阀：槽 7 %s、槽 8 %s\n", slot_is(7, 770) ? "被写（跳进了已释放的块）" : "没写", slot_is(8, 880) ? "已写" : "没写");
    CHECK(!slot_is(7, 770)); CHECK(slot_is(8, 880));
    API(nvnCommandBufferFinalize)(&x); API(nvnCommandBufferFinalize)(&y); API(nvnCommandBufferFinalize)(&z);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ⑤c #768338 根修：CALL 按设备地址解析。长期列表 A 调用 B，B 每帧在同一块游戏命令内存里重录（真机上 B 的句柄 = 那块内存的地址、不变，A 不用重录）。
//   期望按真机语义手写：B 在同一地址重录为写槽 10 = 1000 / 2000 / 3000，A 每次回放读到的都是最新那份（第二次重录前把隔离区清空，块已被放回）；
//   B 被一条从别处起头的录制盖掉（同地址没有新录制）且隔离期满 → A 的这次 CALL 跳过，槽 10 不再被写，A 自己写的槽 11 照写。
//   反证：去掉解析里的同地址跳转（nvn_cmd_call_resolve 的 r = 2），第一次重录后 A 读到的仍是 1000（隔离中跑旧内容），本项红
static void call_redirect(void) {
    phase = "⑤c CALL 按设备地址解析：同地址重录的 B 被 A 读到最新内容（#768338 根修）";
    const size_t R = 0xD0000;   // 0xD0000 / 0xD8000：别的用例不碰的区段（⑦ 用 0x70000 / 0x80000）
    NVNcommandBuffer b1; new_cmd(&b1, R, 4096);
    API(nvnCommandBufferBeginRecording)(&b1); ub(&b1, 10, 1000); NVNcommandHandle hb = API(nvnCommandBufferEndRecording)(&b1);
    NVNcommandBuffer a; new_cmd(&a, 0xD8000, 4096);
    API(nvnCommandBufferBeginRecording)(&a); API(nvnCommandBufferCallCommands)(&a, 1, &hb); ub(&a, 11, 1100);
    NVNcommandHandle ha = API(nvnCommandBufferEndRecording)(&a);
    memset(datamem, 0, 4096); run(ha); CHECK(slot_is(10, 1000)); CHECK(slot_is(11, 1100));
    static const uint32_t v[3] = {2000, 3000, 4000};
    for (int i = 0; i < 3; i++) {
        if (i == 1) s2a_hle_cmdshadow_quarantine_flush();   // 旧录制的块放回块池：A 记的旧句柄不再是任何录制，仍要按设备地址找到新录制
        NVNcommandBuffer bn; new_cmd(&bn, R, 4096);
        API(nvnCommandBufferBeginRecording)(&bn); ub(&bn, 10, v[i]); CHECK(API(nvnCommandBufferEndRecording)(&bn) != 0);
        memset(datamem, 0, 4096); run(ha);
        if (!slot_is(10, v[i])) printf("CALL 按设备地址：第 %d 次重录后槽 10 = %u（期望 %u）\n", i + 1, *(uint32_t *)(datamem + 10 * 16), v[i]);
        CHECK(slot_is(10, v[i])); CHECK(slot_is(11, 1100));
        API(nvnCommandBufferFinalize)(&bn);
    }
    NVNcommandBuffer w; new_cmd(&w, R - 64, 4096);   // 从 R − 64 起头：盖住 R 上的 B，但 R 上不再有录制的开头
    API(nvnCommandBufferBeginRecording)(&w); ub(&w, 12, 1200); ub(&w, 13, 1300); ub(&w, 14, 1400); CHECK(API(nvnCommandBufferEndRecording)(&w) != 0);
    s2a_hle_cmdshadow_quarantine_flush();
    memset(datamem, 0, 4096); run(ha);
    if (slot_is(10, 4000) || !slot_is(11, 1100)) printf("CALL 按设备地址：B 被别处起头的录制盖掉后槽 10 %s、槽 11 %s\n", slot_is(10, 4000) ? "仍被写（跑了旧块）" : "没写", slot_is(11, 1100) ? "已写" : "没写");
    CHECK(!slot_is(10, 4000)); CHECK(*(uint32_t *)(datamem + 10 * 16) == 0); CHECK(slot_is(11, 1100));
    API(nvnCommandBufferFinalize)(&b1); API(nvnCommandBufferFinalize)(&a); API(nvnCommandBufferFinalize)(&w);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ⑥ CopyCommands 复制记录：之后源内存被游戏重用（重录 + 清零），副本照样回放拷贝时的内容；目标已用 = 源已用（END 不拷、目标自己的 END 补上）
// ⑫ 不占命令内存、只含池绑定 / END 的录制按内容共用（#783：某款每帧约 10 个这种录制，各挂 1 字节锚点、落在游戏从不写的段尾，永远不释放）。
//   期望按规则手写：连录 1000 个空录制 → 活录制数与宿主字节都不变、共用 +1000、句柄全相同；回放空表什么都不做，别的录制 CALL 它照常往下跑；
//   只绑池、不占命令内存的录制（没给内存也没回调的宿主借窗口里录）：同内容同句柄、不同内容不同句柄，连录 500 轮活录制数不变、共用 +1000
static void interned(void) {
    phase = "⑫ 不占命令内存的录制按内容共用";
    NVNcommandBuffer x; new_cmd(&x, 0xC0000, 0x10000);   // 前面各组没用过的一段：空录制的设备字节不盖到别的录制
    uint64_t n0 = s2a_hle_cmdshadow_live(), b0 = s2a_hle_cmdshadow_live_bytes(), e0 = s2a_hle_cmdshadow_shared();
    NVNcommandHandle first = 0; int same = 1;
    for (int i = 0; i < 1000; i++) { API(nvnCommandBufferBeginRecording)(&x); NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&x); if (!first) first = h; same &= h == first; }
    CHECK(first && same && s2a_hle_cmdshadow_live() == n0 && s2a_hle_cmdshadow_live_bytes() == b0 && s2a_hle_cmdshadow_shared() == e0 + 1000);
    memset(datamem, 0, 4096); run(first); CHECK(!slot_is(30, 900));
    API(nvnCommandBufferBeginRecording)(&x); API(nvnCommandBufferCallCommands)(&x, 1, &first); ub(&x, 30, 900); NVNcommandHandle c = API(nvnCommandBufferEndRecording)(&x);
    memset(datamem, 0, 4096); run(c); CHECK(slot_is(30, 900));
    NVNtexturePool tp1, tp2; memset(&tp1, 0, sizeof tp1); memset(&tp2, 0, sizeof tp2);
    NVNcommandBuffer y; new_cmd(&y, 0, 0);
    uint64_t n1 = s2a_hle_cmdshadow_live(), e1 = s2a_hle_cmdshadow_shared();
    NVNcommandHandle p1 = 0, p2 = 0; int same1 = 1, same2 = 1;
    for (int i = 0; i < 500; i++) {
        API(nvnCommandBufferBeginRecording)(&y); API(nvnCommandBufferSetTexturePool)(&y, &tp1); NVNcommandHandle a1 = API(nvnCommandBufferEndRecording)(&y);
        API(nvnCommandBufferBeginRecording)(&y); API(nvnCommandBufferSetTexturePool)(&y, &tp2); NVNcommandHandle a2 = API(nvnCommandBufferEndRecording)(&y);
        if (!p1) { p1 = a1; p2 = a2; } same1 &= a1 == p1; same2 &= a2 == p2;
    }
    CHECK(p1 && p2 && p1 != p2 && same1 && same2 && s2a_hle_cmdshadow_live() == n1 && s2a_hle_cmdshadow_shared() == e1 + 1000);
    API(nvnCommandBufferFinalize)(&x); API(nvnCommandBufferFinalize)(&y);
    printf("%s：通过\n", phase); fflush(stdout);
}
static void copy_then_reuse(void) {
    phase = "⑥ 复制后源内存被重用不影响副本";
    const size_t R2 = 0x60000;
    NVNcommandBuffer s; new_cmd(&s, R2, 4096);
    API(nvnCommandBufferBeginRecording)(&s); for (unsigned i = 0; i < 3; i++) ub(&s, i, 600 + 10 * i);
    NVNcommandHandle hs = API(nvnCommandBufferEndRecording)(&s);
    size_t su = used(&s); CHECK(su == 3 * UB + END);
    NVNcommandBuffer t; new_cmd(&t, 0x68000, su);   // 网球：按源的已用给目标
    API(nvnCommandBufferSetMemoryCallback)(&t, ring_cb); ring(0, 0);
    API(nvnCommandBufferBeginRecording)(&t); API(nvnCommandBufferCopyCommands)(&t, 1, &hs);
    NVNcommandHandle ht = API(nvnCommandBufferEndRecording)(&t);
    CHECK(g_cbs == 0); CHECK(used(&t) == su); CHECK(freeb(&t) == 0);
    API(nvnCommandBufferAddCommandMemory)(&s, &cmdpool, (ptrdiff_t)R2, 4096);
    API(nvnCommandBufferBeginRecording)(&s); for (unsigned i = 0; i < 3; i++) ub(&s, i, 9000); CHECK(API(nvnCommandBufferEndRecording)(&s) != 0);   // 盖掉源录制
    memset(cmdmem + R2, 0, 4096);
    memset(datamem, 0, 4096); run(ht);
    for (unsigned i = 0; i < 3; i++) CHECK(slot_is(i, 600 + 10 * i));
    API(nvnCommandBufferFinalize)(&s); API(nvnCommandBufferFinalize)(&t);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ⑦ 一个录制跨多个游戏段（中途回调给新段）、跨多个宿主块（JUMP 链，含大于首块的大记录），回放完整；写到它任一段都释放它
static void spanning(void) {
    phase = "⑦ 跨段 / 跨块录制回放完整";
    NVNcommandBuffer c; new_cmd(&c, 0x70000, 64);
    API(nvnCommandBufferSetMemoryCallback)(&c, ring_cb); ring(0x71000, 64);
    uint64_t live0 = s2a_hle_cmdshadow_live();
    API(nvnCommandBufferBeginRecording)(&c);
    for (unsigned i = 0; i < 6; i++) ub(&c, i, 700 + 10 * i);   // 每段 64 只放得下一条 40：第 2..6 条各回调一次
    NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&c);   // END 0：不占第 6 段剩下的 24
    CHECK(g_cbs == 5); for (unsigned i = 0; i < 5; i++) CHECK(g_need[i] == UB);
    CHECK(used(&c) == UB + END);
    memset(datamem, 0, 4096); run(h);
    for (unsigned i = 0; i < 6; i++) CHECK(slot_is(i, 700 + 10 * i));
    // 多块：300 条 + 一条 4 KB 的大更新（大于首块），宿主记录 > 16 KB
    NVNcommandBuffer b; new_cmd(&b, 0x80000, 0x10000);
    API(nvnCommandBufferBeginRecording)(&b);
    for (unsigned i = 0; i < 300; i++) ub(&b, i, 20000 + i * 8);
    static uint32_t big[1024]; for (unsigned i = 0; i < 1024; i++) big[i] = 0xb16b0000u + i;
    API(nvnCommandBufferUpdateUniformBuffer)(&b, data_ga, DATA_BYTES, 0x10000, sizeof big, big);
    NVNcommandHandle hb = API(nvnCommandBufferEndRecording)(&b);
    CHECK(used(&b) == 300 * UB + (24 + sizeof big) + END);
    memset(datamem, 0, 0x20000); run(hb);
    for (unsigned i = 0; i < 300; i++) CHECK(slot_is(i, 20000 + i * 8));
    CHECK(!memcmp(datamem + 0x10000, big, sizeof big));
    CHECK(s2a_hle_cmdshadow_live() == live0 + 2);
    // 只在第 3 段（0x71000 + 64 × 2 起，h 占其中 [0, 40)）里从 +16 写一条：跨段的录制 h 释放（空录制不占字节、盖不到区间，所以写一条）
    NVNcommandBuffer o; new_cmd(&o, 0x71000 + 64 * 2 + 16, 64);
    API(nvnCommandBufferBeginRecording)(&o); ub(&o, 9, 1); CHECK(API(nvnCommandBufferEndRecording)(&o) != 0);
    CHECK(s2a_hle_cmdshadow_live() == live0 + 2);   // h 释放、o +1（hb 仍活）
    API(nvnCommandBufferFinalize)(&c); API(nvnCommandBufferFinalize)(&b); API(nvnCommandBufferFinalize)(&o);
    printf("%s：通过\n", phase); fflush(stdout);
}

// ⑧ 路易吉洋馆3：BindTexture 的句柄字在游戏命令内存 段起点 + 已用 + 0x10（设备命令里的位置），游戏录完改写它，回放取改写后的；其余字节不动。拷贝取拷贝那一刻的句柄
static void lm3_patch(void) {
    phase = "⑧ 路易吉洋馆3 录后改写句柄字";
    fill();
    const size_t R4 = 0x90000;
    NVNcommandBuffer c; new_cmd(&c, R4, 4096);
    API(nvnCommandBufferBeginRecording)(&c); ub(&c, 0, 1);
    size_t at = used(&c); CHECK(at == UB);
    const uint64_t h1 = 5 | 7u << 20, h2 = 9 | 3u << 20;
    API(nvnCommandBufferBindTexture)(&c, NVN_SHADER_STAGE_FRAGMENT, 3, (NVNtextureHandle)h1);
    NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&c);
    uint64_t w; memcpy(&w, cmdmem + R4 + at + 0x10, 8); CHECK(w == h1);
    untouched(0, R4 + at + 0x10); untouched(R4 + at + 0x18, CMD_BYTES);
    // 找到那条记录
    const uint8_t *q = (const uint8_t *)(uintptr_t)h; op_hdr hd;
    for (;;) { memcpy(&hd, q, sizeof hd); CHECK(hd.op != OP_END); if (hd.op == OP_BIND_TEX1) break; if (hd.op == OP_JUMP) { memcpy(&q, q + 8, 8); continue; } q += hd.size; }
    p_bind_handles t; const p_bind_handles *r = bind_rec(OP_BIND_TEX1, q + 8, &t);
    CHECK(r->n == 1 && r->first == 3 && r->h[0] == h1);   // 没绑池：两类基址都是 0
    memcpy(cmdmem + R4 + at + 0x10, &h2, 8);                // 游戏录完改写
    r = bind_rec(OP_BIND_TEX1, q + 8, &t); CHECK(r->first == 3 && r->h[0] == h2);
    // 拷贝：取此刻的 h2；之后源字再改，副本不变
    NVNcommandBuffer d; new_cmd(&d, R4 + 0x1000, 4096);
    API(nvnCommandBufferBeginRecording)(&d); API(nvnCommandBufferCopyCommands)(&d, 1, &h); NVNcommandHandle hc = API(nvnCommandBufferEndRecording)(&d);
    memcpy(cmdmem + R4 + at + 0x10, &h1, 8);
    q = (const uint8_t *)(uintptr_t)hc;
    for (;;) { memcpy(&hd, q, sizeof hd); CHECK(hd.op != OP_END); if (hd.op == OP_BIND_TEX1) break; if (hd.op == OP_JUMP) { memcpy(&q, q + 8, 8); continue; } q += hd.size; }
    r = bind_rec(OP_BIND_TEX1, q + 8, &t); CHECK(r->first == 3 && r->h[0] == h2);
    memcpy(&w, cmdmem + R4 + 0x1000 + UB + 0x10, 8); CHECK(w == h2);   // 副本的句柄字在目标内存里它自己的位置
    API(nvnCommandBufferFinalize)(&c); API(nvnCommandBufferFinalize)(&d);
    printf("%s：通过\n", phase); fflush(stdout);
}

extern char **environ;
int s2a_cmd_arena_has(const void *p, uint64_t n);
extern int s2a_hle_cmdshadow_list(void (*cb)(void *ctx, const void *mem, size_t size), void *ctx);
static uint64_t g_seg[256][2]; static int g_nseg;
static void seg_cb(void *ctx, const void *mem, size_t n) { (void)ctx; if (g_nseg < 256) { g_seg[g_nseg][0] = (uint64_t)(uintptr_t)mem; g_seg[g_nseg][1] = n; g_nseg++; } }
// 回放进程一侧：argv = --replay-check <地址> <大小> …；每段在本进程的命令记录区里、整段是一块不可访问的空保留
static int replay_check(int argc, char **argv) {
    for (int i = 2; i + 1 < argc; i += 2) {
        uint64_t a = strtoull(argv[i], NULL, 0), n = strtoull(argv[i + 1], NULL, 0);
        vm_address_t r = (vm_address_t)a; vm_size_t rs = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        int in = s2a_cmd_arena_has((const void *)(uintptr_t)a, n), got = vm_region_64(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS;
        int empty = got && r <= a && a + n <= r + rs && info.protection == VM_PROT_NONE;
        if (!in || !empty) { fprintf(stderr, "影子块 %#llx+%#llx：在回放进程命令记录区里 %d、是空保留 %d\n", (unsigned long long)a, (unsigned long long)n, in, empty); return 1; }
    }
    return 0;
}
static void replayable(const char *self) {
    phase = "⑨ 帧回放能把影子块放回原地址";
    g_nseg = 0; int n = s2a_hle_cmdshadow_list(seg_cb, NULL); CHECK(n > 0 && n <= 256);
    for (int i = 0; i < g_nseg; i++) CHECK(s2a_cmd_arena_has((const void *)(uintptr_t)g_seg[i][0], g_seg[i][1]));
    static char buf[256 * 2][24]; char *av[2 + 256 * 2 + 1]; int k = 0;
    av[k++] = (char *)self; av[k++] = "--replay-check";
    for (int i = 0; i < g_nseg; i++) { snprintf(buf[2 * i], 24, "%#llx", (unsigned long long)g_seg[i][0]); snprintf(buf[2 * i + 1], 24, "%#llx", (unsigned long long)g_seg[i][1]); av[k++] = buf[2 * i]; av[k++] = buf[2 * i + 1]; }
    av[k] = NULL; pid_t pid; int st = -1;
    setenv("S2A_FRAME_REPLAY", "/nonexistent/t_nvn_cmdshadow", 1);   // 子进程当回放进程：构造期占区、只占不切
    CHECK(posix_spawn(&pid, self, NULL, NULL, av, environ) == 0); CHECK(waitpid(pid, &st, 0) == pid);
    unsetenv("S2A_FRAME_REPLAY");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("%s：通过（%d 块都在命令记录区，回放进程同址是空保留）\n", phase, g_nseg); fflush(stdout);
}

// ⑩ 绑纹理池 / 采样器池不占命令内存：真机驱动把它们写成控制内存里的 16 字节令牌（类型 0x13 / 0x14），不进命令内存、不发 OUT_OF_COMMAND_MEMORY。
//   期望取自游戏自带驱动的反汇编（SDK 4.4.1 的 sdk 模块与另一款更新 SDK 的 sdk 模块，两代同形），不跑被测实现：
//   Outlast 2（UE4）建 RHI 时 BeginRecording 前只给控制内存，绑完两个池才建命令内存环，环没建时回调读未初始化字段崩（main+0xf3a574）
static void pool_binds_no_cmd_memory(void) {
    phase = "⑩ 池绑定不占命令内存";
    NVNtexturePool tp; NVNsamplerPool sp; memset(&tp, 0, sizeof tp); memset(&sp, 0, sizeof sp);
    // 没给命令内存：两次池绑定不回调、已用 0；接着的第一条真命令才回调，要的量 = 它自己的设备字节
    NVNcommandBuffer c; new_cmd(&c, 0, 0);
    API(nvnCommandBufferSetMemoryCallback)(&c, ring_cb); ring(0xa0000, 4096);
    API(nvnCommandBufferBeginRecording)(&c);
    API(nvnCommandBufferSetTexturePool)(&c, &tp); API(nvnCommandBufferSetSamplerPool)(&c, &sp);
    CHECK(g_cbs == 0); CHECK(used(&c) == 0);
    ub(&c, 0, 1);
    CHECK(g_cbs == 1); CHECK(g_need[0] == UB);
    CHECK(API(nvnCommandBufferEndRecording)(&c) != 0);
    CHECK(used(&c) == UB + END);
    // 给了内存：池绑定前后已用不变
    NVNcommandBuffer d; new_cmd(&d, 0xb0000, 4096); API(nvnCommandBufferBeginRecording)(&d);
    for (int i = 0; i < 4; i++) { API(nvnCommandBufferSetTexturePool)(&d, &tp); API(nvnCommandBufferSetSamplerPool)(&d, &sp); }
    CHECK(used(&d) == 0);
    CHECK(API(nvnCommandBufferEndRecording)(&d) != 0);
    CHECK(used(&d) == END);
    API(nvnCommandBufferFinalize)(&c); API(nvnCommandBufferFinalize)(&d);
    printf("%s：通过\n", phase); fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--replay-check")) return replay_check(argc, argv);
    // 当录的进程：命令记录区在构造期按环境变量占，没设就带上重进自己。同时按单测窗口（S2A_WIN_NEED_MB）占窗：占到窗口才走 Mac 的 iPhone 地址围栏（2^39 之上几乎全占死），
    //   区要赶在它之前占（11-05 旷野之息录 / 放都报「2^39 之上没有 1024 MB 的空洞」，没窗口的单测进程碰不到这条）
    if (!getenv("S2A_CMD_ARENA")) { setenv("S2A_CMD_ARENA", "1", 1); setenv("S2A_WIN_NEED_MB", "4096", 1); execv(argv[0], argv); perror("execv"); return 1; }

    NVNdeviceBuilder db = {{0}}; API(nvnDeviceBuilderSetDefaults)(&db); CHECK(API(nvnDeviceInitialize)(&device, &db));
    NVNqueueBuilder qb = {{0}}; API(nvnQueueBuilderSetDefaults)(&qb); API(nvnQueueBuilderSetDevice)(&qb, &device); CHECK(API(nvnQueueInitialize)(&queue, &qb));
    new_pool(&cmdpool, &cmdmem, CMD_BYTES); new_pool(&datapool, &datamem, DATA_BYTES);
    data_ga = API(nvnMemoryPoolGetBufferAddress)(&datapool);
    guest_untouched(); recorded_line_width(); used_by_device(); callback_timing(); totk_slots(); release(); call_guard(); call_redirect(); copy_then_reuse(); spanning(); lm3_patch(); pool_binds_no_cmd_memory(); interned(); replayable(argv[0]);
    API(nvnQueueFinish)(&queue);
    API(nvnMemoryPoolFinalize)(&cmdpool); API(nvnMemoryPoolFinalize)(&datapool); API(nvnQueueFinalize)(&queue); API(nvnDeviceFinalize)(&device);
    printf("✓ t_nvn_cmdshadow：%u 条断言（10 组：游戏内存不动、设备口径已用、回调时机、王国之泪排槽、覆盖释放、复制后重用、跨段跨块、录后改写句柄、池绑定不占命令内存、回放放回影子块）\n", checks);
    return 0;
}
