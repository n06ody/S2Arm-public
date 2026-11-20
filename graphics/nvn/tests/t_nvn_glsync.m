// NVN ⇄ GL 同步对象与两个调试接口（11-01 全库接口清点 NVN 缺口 4 个）：生产引导 / 登记 / 跳板，真 NVN 设备与队列、真 GL 上下文（同一个 Metal 设备、两条队列）。
// 跨队列等待用一个 CPU 控制的共享事件当「闸」：闸关着时，被等的一方在 GPU 上停住；期望值不跑被测实现就能定——闸开之前对方的栅栏一定没到，开之后一定到：
// ① GL → NVN（火影忍者 终极风暴羁绊的用法）：GL 命令缓冲先在 GPU 上等闸，再 glFenceSync / glFlush；nvnSyncInitializeFromFencedGLSync 转成 NVN 同步对象（status = TIMEOUT_EXPIRED：
//    闸关着、查询时 GL 栅栏没到），nvnQueueWaitSync 之后提交的 NVN 工作在闸开之前不完成（它的栅栏 nvnSyncWait 超时 0 → TIMEOUT_EXPIRED，300 ms 后仍是），开闸后完成（兜底 30 秒）；
// ② NVN → GL：NVN 队列先等闸再提交、QueueFenceSync；nvnSyncCreateGLSync 转成 GL 同步对象：glClientWaitSync 超时 0 → TIMEOUT_EXPIRED；glWaitSync 之后的 GL 提交在闸开之前不完成；开闸后都完成；
// ③ 边界：没围栏的 NVN 同步对象 CreateGLSync 回假、不写；无效 GL 句柄 InitializeFromFencedGLSync 回假、status = FAILED；
// ④ nvnDeviceWaitForError：没有 GPU 错误时阻塞到超时（≥ 给的 80 ms）、回 0、错误信息不写；
// ⑤ nvnInstallGlobalDebugCallback：按名可查（零售驱动这个函数就是一条 ret），调用后回调不被调用。
// ⓪ nvnSyncCreateGLSync 单独一例（只靠 NVN）：围在一次提交上的同步对象转出的 GL 同步对象等得到（兜底 30 秒）。
// ⑥ 命令缓冲里的 nvnCommandBufferWaitSync（缺口族 40）：同 ①，但等待录在命令缓冲里提交——闸开之前其后提交的 NVN 工作不完成，开闸后完成（兜底 30 秒；原先是空函数，闸没开就完成）。
// ⑦ 没围栏的 nvnSyncWait：给多长超时都立刻回 ALREADY_SIGNALED（怒之铁拳4：不能睡到超时；11-06 DOOM Eternal：不能回超时，游戏按超时 0 轮询会死循环）。
// ⑧ nvnTextureBuilderSetGLTextureName 接 GL 纹理（11-03 火影羁绊：Scaleform 往 GL 纹理画界面、游戏按名字建 NVN 纹理来回拷）：GL 建 64×32 RGBA8、3 级不可变存储，
//    NVN 构建器只设设备与名字（格式 / 尺寸留缺省）建纹理 → 宽 64、高 32、格式 RGBA8（0x25）、级数 3（取 GL 的）；建好之后 GL 再往第 0 级写 (x, y, 7, 255)，
//    经 NVN 纹理读回逐像素相同（同一份存储，不是建时的副本）；NVN 命令缓冲清成 (0, 1, 0, 1) 后 GL glGetTexImage 读回每像素 0 255 0 255（反向也通）；
//    名字不是 GL 纹理（999）→ 格式仍是构建器上的 0、宽 0（没有可接的存储，报红不伪造）。
// 反证：补齐前 CreateGLSync / InitializeFromFencedGLSync 是报红桩（回 0、不写输出），后两个按名查不到（load 的 CHECK 红）。参数给例号（0–8）只跑那一例，供逐接口反证；
//   「闸」用 GL 层的 gl_sync_from_event（nvnSyncCreateGLSync 内部同一条路）经 dlsym 取：补齐前的库没有它，那几例在取不到处红
#import <Metal/Metal.h>
#include "nvn_protos.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface; typedef int32_t EGLint; typedef uint32_t EGLBoolean, GLenum, GLbitfield; typedef uint64_t GLuint64; typedef struct __GLsync *GLsync;   // GL / EGL 的 C 类型（同 gfx/gl/gl_int.h）
extern void *h_nvnBootstrapLoader(const char *name);
extern void *gl_lookup(const char *name);
#include <dlfcn.h>
extern void *mtl_device(void);
static const char *phase = "准备";
static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL [%s] %s:%d：%s\n", phase, __FILE__, __LINE__, #expr); exit(1); \
} } while (0)
static void *load(const char *name) { void *entry = h_nvnBootstrapLoader(name); CHECK(entry != NULL); return entry; }
static void *glf(const char *name) { void *f = gl_lookup(name); CHECK(f != NULL); return f; }
#define API(name) ((NVNT_##name)load(#name))
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
// 开闸 / 提交之后「该等到」的等待上限 30 秒（原 2 秒：负载高时 GPU 完成晚到就假红）；闸关着时「不该完成」的检查仍用 300 ms 短窗（负对照：负载高时只会漏检，不会假红）
#define W30 30000000000ull
// hle_nvn.c 只读计数：nvnSyncWait 进入带超时的阻塞等待的次数（⑦ 不得涨）
uint64_t s2a_nvn_syncwait_blocks(void);
typedef NVNboolean (*create_gl_fn)(const NVNsync *, uint64_t *);
typedef NVNboolean (*init_gl_fn)(NVNsync *, NVNdevice *, const void *, int *);
typedef int (*wait_err_fn)(NVNdevice *, uint64_t, void *);
typedef void (*install_fn)(const void *, void *);

static NVNdevice device; static NVNqueue queue; static NVNcommandBuffer cmd; static NVNmemoryPool cmd_pool; static void *cmd_mem;
// 提交一次空命令缓冲（只有提交本身：栅栏号前进），再把 s 围在这次提交上
static void submit_and_fence(NVNsync *s) {
    API(nvnCommandBufferBeginRecording)(&cmd);
    NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&cmd);
    API(nvnQueueSubmitCommands)(&queue, 1, &h);
    API(nvnQueueFenceSync)(&queue, s, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    API(nvnQueueFlush)(&queue);
}
static int cb_called;
static void debug_cb(void) { cb_called++; }

static uint64_t (*gl_sync_from_event)(void *ev, uint64_t val);
#include <pthread.h>
static int want(int argc, char **argv, int k) { return argc < 2 || atoi(argv[1]) == k; }
int main(int argc, char **argv) { @autoreleasepool {
    alarm(60);
    create_gl_fn create_gl = (create_gl_fn)load("nvnSyncCreateGLSync");
    init_gl_fn init_gl = (init_gl_fn)load("nvnSyncInitializeFromFencedGLSync");
    NVNdeviceBuilder db = {{0}}; API(nvnDeviceBuilderSetDefaults)(&db); CHECK(API(nvnDeviceInitialize)(&device, &db));
    NVNqueueBuilder qb = {{0}}; API(nvnQueueBuilderSetDefaults)(&qb); API(nvnQueueBuilderSetDevice)(&qb, &device); CHECK(API(nvnQueueInitialize)(&queue, &qb));
    size_t page = (size_t)getpagesize(), size = 65536; CHECK(posix_memalign(&cmd_mem, page, size) == 0); memset(cmd_mem, 0, size);
    NVNmemoryPoolBuilder pb = {{0}}; API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED); API(nvnMemoryPoolBuilderSetStorage)(&pb, cmd_mem, size);
    CHECK(API(nvnMemoryPoolInitialize)(&cmd_pool, &pb));
    CHECK(API(nvnCommandBufferInitialize)(&cmd, &device)); API(nvnCommandBufferAddCommandMemory)(&cmd, &cmd_pool, 0, (ptrdiff_t)size);
    // GL 上下文（不建表面：只用同步对象与提交）
    EGLDisplay (*eglGetDisplay)(void *) = glf("eglGetDisplay"); EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = glf("eglInitialize");
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = glf("eglCreateContext");
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = glf("eglMakeCurrent");
    GLsync (*glFenceSync)(GLenum, GLbitfield) = glf("glFenceSync"); void (*glFlush)(void) = glf("glFlush"); GLenum (*glGetError)(void) = glf("glGetError");
    GLenum (*glClientWaitSync)(GLsync, GLbitfield, GLuint64) = glf("glClientWaitSync"); void (*glWaitSync)(GLsync, GLbitfield, GLuint64) = glf("glWaitSync");
    EGLDisplay dpy = eglGetDisplay(NULL); EGLint ma, mi; eglInitialize(dpy, &ma, &mi);
    CHECK(eglMakeCurrent(dpy, NULL, NULL, eglCreateContext(dpy, (EGLConfig)1, NULL, NULL)));
    id<MTLDevice> dev = (__bridge id<MTLDevice>)mtl_device();

    if (want(argc, argv, 0)) { phase = "⓪ nvnSyncCreateGLSync：NVN 栅栏转成的 GL 同步对象等得到";
        NVNsync s; API(nvnSyncInitialize)(&s, &device); submit_and_fence(&s); uint64_t h = 0;
        CHECK(create_gl(&s, &h)); CHECK(h != 0); GLenum r = glClientWaitSync((GLsync)(uintptr_t)h, 0, W30); CHECK(r == 0x911C || r == 0x911A);
        printf("%s：通过\n", phase); }
    if (want(argc, argv, 1) || want(argc, argv, 2) || want(argc, argv, 6)) { gl_sync_from_event = (uint64_t (*)(void *, uint64_t))dlsym(RTLD_DEFAULT, "gl_sync_from_event"); CHECK(gl_sync_from_event != NULL); }

    if (want(argc, argv, 1)) { phase = "① GL → NVN：NVN 队列在 GPU 上等 GL 的栅栏";
    {   id<MTLSharedEvent> gate = [dev newSharedEvent];
        GLsync g_gate = (GLsync)(uintptr_t)gl_sync_from_event((__bridge void *)gate, 1);
        glWaitSync(g_gate, 0, 0xFFFFFFFFFFFFFFFFull);   // GL 命令缓冲先等闸
        GLsync f = glFenceSync(0x9117, 0); glFlush(); CHECK(glGetError() == 0);
        NVNsync s, after; memset(&s, 0xcc, sizeof s); int status = -1;
        CHECK(init_gl(&s, &device, f, &status));
        CHECK(status == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);   // 闸关着：GL 栅栏还没到
        CHECK(API(nvnSyncWait)(&s, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        CHECK(API(nvnQueueWaitSync)(&queue, &s));
        API(nvnSyncInitialize)(&after, &device); submit_and_fence(&after);
        usleep(300000);
        CHECK(API(nvnSyncWait)(&after, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);   // 300 ms 后 NVN 的提交仍被 GL（闸）挡着
        uint64_t t0 = now_ns(); gate.signaledValue = 1;
        NVNsyncWaitResult r = API(nvnSyncWait)(&after, W30);
        CHECK(r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED || r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        CHECK(API(nvnSyncWait)(&s, 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        printf("%s：通过（开闸后 NVN 提交 %.1f ms 完成）\n", phase, (now_ns() - t0) / 1e6); } }

    if (want(argc, argv, 2)) { phase = "② NVN → GL：GL 在 CPU / GPU 上等 NVN 的栅栏";
    {   id<MTLSharedEvent> gate = [dev newSharedEvent];
        GLsync g_gate = (GLsync)(uintptr_t)gl_sync_from_event((__bridge void *)gate, 1);
        NVNsync wait_gate, n; int st; CHECK(init_gl(&wait_gate, &device, g_gate, &st)); CHECK(st == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        CHECK(API(nvnQueueWaitSync)(&queue, &wait_gate));   // NVN 队列先等闸
        API(nvnSyncInitialize)(&n, &device); submit_and_fence(&n);
        uint64_t h = 0; CHECK(create_gl(&n, &h)); CHECK(h != 0);
        CHECK(glClientWaitSync((GLsync)(uintptr_t)h, 0, 0) == 0x911B);   // TIMEOUT_EXPIRED
        glWaitSync((GLsync)(uintptr_t)h, 0, 0xFFFFFFFFFFFFFFFFull); GLsync g = glFenceSync(0x9117, 0); glFlush(); CHECK(glGetError() == 0);
        usleep(300000);
        CHECK(glClientWaitSync(g, 0, 0) == 0x911B);   // 300 ms 后 GL 的提交仍被 NVN（闸）挡着
        CHECK(glClientWaitSync((GLsync)(uintptr_t)h, 0, 0) == 0x911B);
        gate.signaledValue = 1;
        GLenum r1 = glClientWaitSync((GLsync)(uintptr_t)h, 0, W30), r2 = glClientWaitSync(g, 1, W30);
        CHECK((r1 == 0x911C || r1 == 0x911A) && (r2 == 0x911C || r2 == 0x911A));
        // 来自 GL 的 NVN 同步对象再转回 GL：就是原来那个 GL 栅栏
        uint64_t back = 0; CHECK(create_gl(&wait_gate, &back)); CHECK(glClientWaitSync((GLsync)(uintptr_t)back, 0, 0) == 0x911A);
        printf("%s：通过\n", phase); } }

    if (want(argc, argv, 3)) { phase = "③ 边界：没围栏 / 无效句柄";
    {   NVNsync blank; API(nvnSyncInitialize)(&blank, &device); uint64_t h = 0xA5A5A5A5A5A5A5A5ull;
        CHECK(!create_gl(&blank, &h)); CHECK(h == 0xA5A5A5A5A5A5A5A5ull);
        NVNsync s; int status = -1; CHECK(!init_gl(&s, &device, (const void *)(uintptr_t)0x7777777, &status)); CHECK(status == NVN_SYNC_WAIT_RESULT_FAILED);
        printf("%s：通过\n", phase); } }

    if (want(argc, argv, 4)) { phase = "④ nvnDeviceWaitForError：没有错误时阻塞到超时";
    {   wait_err_fn wait_err = (wait_err_fn)load("nvnDeviceWaitForError");
        uint8_t info[16]; memset(info, 0x5a, sizeof info); uint64_t t0 = now_ns();
        int r = wait_err(&device, 80000000ull, info); uint64_t dt = now_ns() - t0;
        CHECK(r == 0); CHECK(dt >= 80000000ull); for (int i = 0; i < 16; i++) CHECK(info[i] == 0x5a);
        printf("%s：通过（阻塞 %.1f ms）\n", phase, dt / 1e6); } }

    if (want(argc, argv, 6)) { phase = "⑥ 命令缓冲里的 WaitSync：其后的 NVN 工作在 GPU 上等 GL 的栅栏";
    {   id<MTLSharedEvent> gate = [dev newSharedEvent];
        GLsync g_gate = (GLsync)(uintptr_t)gl_sync_from_event((__bridge void *)gate, 1);
        glWaitSync(g_gate, 0, 0xFFFFFFFFFFFFFFFFull);
        GLsync f = glFenceSync(0x9117, 0); glFlush(); CHECK(glGetError() == 0);
        NVNsync s, after; int status = -1;
        CHECK(init_gl(&s, &device, f, &status)); CHECK(status == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        API(nvnCommandBufferBeginRecording)(&cmd); API(nvnCommandBufferWaitSync)(&cmd, &s);
        NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&cmd);
        API(nvnQueueSubmitCommands)(&queue, 1, &h);
        API(nvnSyncInitialize)(&after, &device); submit_and_fence(&after);
        usleep(300000);
        CHECK(API(nvnSyncWait)(&after, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);   // 300 ms 后仍被闸挡着
        uint64_t t0 = now_ns(); gate.signaledValue = 1;
        NVNsyncWaitResult r = API(nvnSyncWait)(&after, W30);
        CHECK(r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED || r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        printf("%s：通过（开闸后 %.1f ms 完成）\n", phase, (now_ns() - t0) / 1e6); } }
    if (want(argc, argv, 7)) { phase = "⑦ 没围栏的 SyncWait：给多长超时都立刻回 FAILED（同真机驱动）";
    {   // 11-01 原生崩溃线：怒之铁拳4 开局在没围栏的同步对象上带长超时等，原先睡到超时（封顶 1 小时）主线程卡死无首帧；10-29 普查时立刻返回、直连通过
        // 期望值 3 取自真机驱动（一款 UE4 游戏随包的 NVN：nvnSyncInitialize 置类型 0，nvnSyncWait 类型 0 返回预置的 3），不由被测实现反推；
        //   回 0 会让以超时 0 轮询「数据就绪」的游戏提前去读（诊断 #343862）
        NVNsync s; API(nvnSyncInitialize)(&s, &device);
        uint64_t t0 = now_ns(), b0 = s2a_nvn_syncwait_blocks(); CHECK(API(nvnSyncWait)(&s, 2000000000ull) == NVN_SYNC_WAIT_RESULT_FAILED);
        CHECK(API(nvnSyncWait)(&s, 0) == NVN_SYNC_WAIT_RESULT_FAILED);   // DOOM Eternal：超时 0 轮询也不回超时（它只在 TIMEOUT 时睡下再查）
        // 原判用时 < 50 ms：负载高时调用本身被抢占就假红。改判「没进入阻塞等待」（计数不涨），睡到超时的旧实现必进阻塞等待
        double ms = (now_ns() - t0) / 1e6; uint64_t blocks = s2a_nvn_syncwait_blocks() - b0; CHECK(blocks == 0);
        printf("%s：通过（2 秒超时：进入阻塞等待 %llu 次，用时 %.1f ms 仅供参考）\n", phase, (unsigned long long)blocks, ms); } }

    if (want(argc, argv, 5)) { phase = "⑤ nvnInstallGlobalDebugCallback：按名可查、什么都不做";
    {   install_fn install = (install_fn)load("nvnInstallGlobalDebugCallback"); int token = 0;
        install((const void *)debug_cb, &token); NVNsync s; API(nvnSyncInitialize)(&s, &device); submit_and_fence(&s); API(nvnQueueFinish)(&queue);
        CHECK(cb_called == 0 && token == 0);
        printf("%s：通过\n", phase); } }

    if (want(argc, argv, 8)) { phase = "⑧ nvnTextureBuilderSetGLTextureName：NVN 纹理就是那张 GL 纹理";
    {   void (*glGenTextures)(int, uint32_t *) = glf("glGenTextures"); void (*glBindTexture)(GLenum, uint32_t) = glf("glBindTexture");
        void (*glTexStorage2D)(GLenum, int, GLenum, int, int) = glf("glTexStorage2D");
        void (*glTexSubImage2D)(GLenum, int, int, int, int, int, GLenum, GLenum, const void *) = glf("glTexSubImage2D");
        void (*glGetTexImage)(GLenum, int, GLenum, GLenum, void *) = glf("glGetTexImage"); void (*glFinish)(void) = glf("glFinish");
        int (*storage_read)(const void *, uint8_t *, size_t) = (int (*)(const void *, uint8_t *, size_t))dlsym(RTLD_DEFAULT, "nvn_be_tex_storage_read"); CHECK(storage_read != NULL);
        enum { W = 64, H = 32 }; uint32_t tex = 0; glGenTextures(1, &tex); glBindTexture(0x0DE1, tex); glTexStorage2D(0x0DE1, 3, 0x8058, W, H); CHECK(glGetError() == 0);
        NVNtextureBuilder tb; API(nvnTextureBuilderSetDevice)(&tb, &device); API(nvnTextureBuilderSetDefaults)(&tb);
        API(nvnTextureBuilderSetGLTextureName)(&tb, tex); CHECK(API(nvnTextureBuilderGetGLTextureName)(&tb) == tex);
        NVNtexture nt; CHECK(API(nvnTextureInitialize)(&nt, &tb));
        CHECK(API(nvnTextureGetWidth)(&nt) == W && API(nvnTextureGetHeight)(&nt) == H && API(nvnTextureGetFormat)(&nt) == NVN_FORMAT_RGBA8 && API(nvnTextureGetLevels)(&nt) == 3);
        static uint8_t px[W * H * 4], back[W * H * 4];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *p = &px[(y * W + x) * 4]; p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 7; p[3] = 255; }
        glTexSubImage2D(0x0DE1, 0, 0, 0, W, H, 0x1908, 0x1401, px); glFinish(); CHECK(glGetError() == 0);
        CHECK(storage_read(&nt, back, W * 4) == 0); CHECK(memcmp(px, back, sizeof px) == 0);   // GL 建之后写的内容经 NVN 纹理读到：同一份存储
        const float green[4] = {0, 1, 0, 1}; NVNcopyRegion rg = {0, 0, 0, W, H, 1};
        API(nvnCommandBufferBeginRecording)(&cmd); API(nvnCommandBufferClearTexture)(&cmd, &nt, NULL, &rg, green, 0xF);
        NVNcommandHandle hc = API(nvnCommandBufferEndRecording)(&cmd); API(nvnQueueSubmitCommands)(&queue, 1, &hc); API(nvnQueueFinish)(&queue);
        memset(back, 0, sizeof back); glGetTexImage(0x0DE1, 0, 0x1908, 0x1401, back); CHECK(glGetError() == 0);
        int ok = 1; for (int i = 0; i < W * H; i++) ok &= back[i * 4] == 0 && back[i * 4 + 1] == 255 && back[i * 4 + 2] == 0 && back[i * 4 + 3] == 255;
        CHECK(ok);   // NVN 清的颜色 GL 读到：反向也通
        API(nvnTextureFinalize)(&nt);
        NVNtextureBuilder tb2; API(nvnTextureBuilderSetDevice)(&tb2, &device); API(nvnTextureBuilderSetDefaults)(&tb2); API(nvnTextureBuilderSetGLTextureName)(&tb2, 999);
        NVNtexture bad; API(nvnTextureInitialize)(&bad, &tb2); CHECK(API(nvnTextureGetFormat)(&bad) == 0 && API(nvnTextureGetWidth)(&bad) == 0); API(nvnTextureFinalize)(&bad);
        printf("%s：通过\n", phase); } }

    printf("✓ t_nvn_glsync：%u 条断言（%s）\n", checks, argc < 2 ? "9 例" : argv[1]);
    return 0;
}}
