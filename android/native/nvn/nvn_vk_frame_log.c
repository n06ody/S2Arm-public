// 逐帧上屏间隔 + 每帧 fence 等待（诊断，默认关）：环形缓冲 + 局末 logcat 分片转储。
// 格式与宿主解析见头文件注释；分片先例 = provider_vk.c 的 `direct device caps[i/n]`。
#include "nvn_vk_frame_log.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#include <stdbool.h>
#include "debug_gate.h"
#endif

void s2a_fl_reset(struct s2a_frame_log *fl) {
    atomic_store(&fl->snapshot_lock, 0);
    fl->seq = 0;
    fl->dropped = 0;
    fl->w = 0;
    fl->kept = 0;
    atomic_store(&fl->fence_ns_acc, 0);
    atomic_store(&fl->fence_n_acc, 0);
}

void s2a_fl_fence(struct s2a_frame_log *fl, uint64_t wait_ns) {
    while (atomic_exchange_explicit(&fl->snapshot_lock, 1, memory_order_acquire)) {}
    atomic_fetch_add(&fl->fence_ns_acc, wait_ns);
    atomic_fetch_add(&fl->fence_n_acc, 1);
    atomic_store_explicit(&fl->snapshot_lock, 0, memory_order_release);
}

uint64_t s2a_fl_note_timed(struct s2a_frame_log *fl, uint64_t boot_us, uint64_t acquire_ns, uint64_t present_ns) {
    while (atomic_exchange_explicit(&fl->snapshot_lock, 1, memory_order_acquire)) {}
    // 先快照帧内 fence 账（present 之后到达的等待归下一帧），再落记录。
    // acc 存 ns 避免逐次截断；记录字段是 µs（封顶 UINT32_MAX ≈ 71 分钟，远超单帧）。
    uint64_t fus = atomic_exchange(&fl->fence_ns_acc, 0) / 1000;
    uint64_t fn = atomic_exchange(&fl->fence_n_acc, 0);
    struct s2a_fl_rec *r = &fl->rec[fl->w];
    r->ts = boot_us;
    r->fence_wait_us = fus > UINT32_MAX ? UINT32_MAX : (uint32_t)fus;
    r->fence_n = fn > UINT32_MAX ? UINT32_MAX : (uint32_t)fn;
    acquire_ns /= 1000; present_ns /= 1000;
    r->acquire_us = acquire_ns > UINT32_MAX ? UINT32_MAX : (uint32_t)acquire_ns;
    r->present_us = present_ns > UINT32_MAX ? UINT32_MAX : (uint32_t)present_ns;
    // gpu 字段：first_draw 在渲染 fence signal 后非阻塞读回的 TOP→BOTTOM 帧耗时
    // （debug.s2a.gpu_time，默认关）。同一结算帧只报一次（seq 去重）；没有新结算写
    // UINT32_MAX——不写 0，与 iOS host_adapt.m gpu_us 空口径一致。
    extern int nvn_vk_first_draw_gpu_last(double *, int *, uint64_t *) __attribute__((weak));
    r->gpu_us = UINT32_MAX; r->gpu_seq = UINT32_MAX;
    if (nvn_vk_first_draw_gpu_last) {
        double gms = 0.0; uint64_t gf = 0;
        if (nvn_vk_first_draw_gpu_last(&gms, NULL, &gf)) {
            static uint64_t fl_gpu_seen;
            if (gf != fl_gpu_seen) {
                fl_gpu_seen = gf;
                uint64_t u = (uint64_t)(gms * 1000.0 + 0.5);
                r->gpu_us = u > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)u;
                r->gpu_seq = gf > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)gf;
            }
        }
    }
    // disp 字段：VK_GOOGLE_display_timing 回报的「最新已确认相邻帧上屏间隔」
    // （actualPresentTime 差，不取 CPU present 节拍）。读数比本行 present 晚到——disp_seq
    // 标明间隔属于哪个较新帧，宿主按 seq 对号不按行序；同一对已确认对只报一次，
    // 没有新确认写 UINT32_MAX（不写 0、不写 CPU 间隔，与 iOS disp_us 空口径一致）。
    extern int nvn_vk_dt_last_gap(uint64_t *, uint64_t *) __attribute__((weak));
    r->disp_us = UINT32_MAX; r->disp_seq = UINT32_MAX;
    if (nvn_vk_dt_last_gap) {
        uint64_t df = 0, dns = 0;
        if (nvn_vk_dt_last_gap(&df, &dns)) {
            static uint64_t fl_disp_seen;
            if (df != fl_disp_seen) {
                fl_disp_seen = df;
                uint64_t u = (dns + 500) / 1000;
                r->disp_us = u > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)u;
                r->disp_seq = df > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)df;
            }
        }
    }
    fl->w = (fl->w + 1) % S2A_FL_CAP;
    if (fl->kept < S2A_FL_CAP) fl->kept++;
    else if (fl->dropped < UINT64_MAX) fl->dropped++;
    if (fl->seq < UINT64_MAX) fl->seq++;
    uint64_t dropped = fl->dropped;
    atomic_store_explicit(&fl->snapshot_lock, 0, memory_order_release);
    return dropped;
}
uint64_t s2a_fl_note(struct s2a_frame_log *fl, uint64_t boot_us) {
    return s2a_fl_note_timed(fl, boot_us, 0, 0);
}

static uint32_t s2a_fl_slot(const struct s2a_frame_log *fl, uint64_t i) {
    return (uint32_t)((fl->w + S2A_FL_CAP - fl->kept + i) % S2A_FL_CAP);
}

uint64_t s2a_fl_at(const struct s2a_frame_log *fl, uint64_t i) {
    if (i >= fl->kept) return 0;
    return fl->rec[s2a_fl_slot(fl, i)].ts;
}

uint32_t s2a_fl_fence_us(const struct s2a_frame_log *fl, uint64_t i) {
    if (i >= fl->kept) return 0;
    return fl->rec[s2a_fl_slot(fl, i)].fence_wait_us;
}

uint32_t s2a_fl_fence_n(const struct s2a_frame_log *fl, uint64_t i) {
    if (i >= fl->kept) return 0;
    return fl->rec[s2a_fl_slot(fl, i)].fence_n;
}

// 载荷第 j 字节（kept 条 32B LE 记录，最旧→最新），不在堆里重排整段。
static uint8_t s2a_fl_byte(const struct s2a_frame_log *fl, uint64_t j) {
    const struct s2a_fl_rec *r = &fl->rec[s2a_fl_slot(fl, j / S2A_FL_REC_BYTES)];
    unsigned o = (unsigned)(j % S2A_FL_REC_BYTES);
    if (o < 8) return (uint8_t)(r->ts >> (o * 8));
    if (o < 12) return (uint8_t)(r->fence_wait_us >> ((o - 8) * 8));
    if (o < 16) return (uint8_t)(r->fence_n >> ((o - 12) * 8));
    if (o < 20) return (uint8_t)(r->acquire_us >> ((o - 16) * 8));
    if (o < 24) return (uint8_t)(r->present_us >> ((o - 20) * 8));
    if (o < 28) return (uint8_t)(r->gpu_us >> ((o - 24) * 8));
    if (o < 32) return (uint8_t)(r->gpu_seq >> ((o - 28) * 8));
    if (o < 36) return (uint8_t)(r->disp_us >> ((o - 32) * 8));
    return (uint8_t)(r->disp_seq >> ((o - 36) * 8));
}

// logcat 单条约 1 KiB 会截断：分片载荷 700 字符（同 caps/profile 先例），前缀约 30 字符。
#define S2A_FL_CHUNK 700

static const char s2a_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void s2a_fl_dump(const struct s2a_frame_log *fl, void (*emit)(void *ctx, const char *line), void *ctx) {
    if (!fl || !emit) return;
    const uint64_t kept = fl->kept;
    const uint64_t bytes = kept * S2A_FL_REC_BYTES;
    const uint64_t b64len = ((bytes + 2) / 3) * 4;
    const unsigned chunks = (unsigned)((b64len + S2A_FL_CHUNK - 1) / S2A_FL_CHUNK);
    char line[800];
    // Android：本局身份同 host 首行——运行器 setprop debug.s2a.session_uuid，写进 meta
    // 行尾（v=5 向后兼容：解析器非锚定，尾部字段可缺）。没置上 → uuid=-（取证端 unproven）。
    char uuid_tail[128] = "";
#if defined(__ANDROID__)
    char uuid[PROP_VALUE_MAX];
    if (__system_property_get("debug.s2a.session_uuid", uuid) <= 0) strcpy(uuid, "-");
    snprintf(uuid_tail, sizeof uuid_tail, " uuid=%s", uuid);
#endif
    snprintf(line, sizeof line,
             "direct frame-log meta: v=5 clock=%s unit=us endian=le rec=%u cap=%u recorded=%llu "
             "presents=%llu dropped=%llu first=%llu bytes=%llu chunks=%u%s",
#if defined(CLOCK_BOOTTIME)
             "CLOCK_BOOTTIME",
#else
             "CLOCK_MONOTONIC",
#endif
             S2A_FL_REC_BYTES, S2A_FL_CAP, (unsigned long long)kept, (unsigned long long)fl->seq,
             (unsigned long long)fl->dropped, (unsigned long long)(fl->seq - kept),
             (unsigned long long)bytes, chunks, uuid_tail);
    emit(ctx, line);

    // 流式编码：逻辑载荷按 4 KB 分块取出（不堆整段），同一块喂 sha256 与 base64；
    // 每凑满 700 个 base64 字符出一片。base64 的 3 字节组可能跨块，rem 留尾。
    uint8_t stage[4096];
    uint8_t rem[3];
    unsigned rn = 0, cpos = 0, part = 0;
    char chunk[S2A_FL_CHUNK + 4];
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    uint64_t off = 0;
    while (off < bytes) {
        size_t n = (size_t)(bytes - off < sizeof stage ? bytes - off : sizeof stage);
        for (size_t i = 0; i < n; i++) stage[i] = s2a_fl_byte(fl, off + i);
        s2a_sha256_update(&hash, stage, n);
        off += n;
        size_t i = 0;
        while (i < n) {
            while (rn < 3 && i < n) rem[rn++] = stage[i++];
            if (rn < 3) break;
            chunk[cpos++] = s2a_b64[rem[0] >> 2];
            chunk[cpos++] = s2a_b64[((rem[0] & 3) << 4) | (rem[1] >> 4)];
            chunk[cpos++] = s2a_b64[((rem[1] & 15) << 2) | (rem[2] >> 6)];
            chunk[cpos++] = s2a_b64[rem[2] & 63];
            rn = 0;
            if (cpos == S2A_FL_CHUNK) {
                int m = snprintf(line, sizeof line, "direct frame-log[%u/%u]:%.*s",
                                 part + 1, chunks, (int)cpos, chunk);
                if (m > 0) emit(ctx, line);
                cpos = 0;
                part++;
            }
        }
    }
    if (rn) {
        // 尾组补零再编码，末两位（rn==1）或末一位（rn==2）写成 '='。
        for (unsigned k = rn; k < 3; k++) rem[k] = 0;
        chunk[cpos++] = s2a_b64[rem[0] >> 2];
        chunk[cpos++] = s2a_b64[((rem[0] & 3) << 4) | (rem[1] >> 4)];
        chunk[cpos++] = rn == 1 ? '=' : s2a_b64[((rem[1] & 15) << 2) | (rem[2] >> 6)];
        chunk[cpos++] = '=';   // 尾组 rn∈{1,2}：末位必为 '='
    }
    if (cpos) {
        int m = snprintf(line, sizeof line, "direct frame-log[%u/%u]:%.*s",
                         part + 1, chunks, (int)cpos, chunk);
        if (m > 0) emit(ctx, line);
    }
    uint8_t digest[32];
    char hex[65];
    s2a_sha256_final(&hash, digest);
    s2a_sha256_hex(digest, hex);
    snprintf(line, sizeof line, "direct frame-log end: frames=%llu dropped=%llu sha256=%s",
             (unsigned long long)kept, (unsigned long long)fl->dropped, hex);
    emit(ctx, line);
}

// ---------------- 生产钩子（单例 + 开关） ----------------

static struct s2a_frame_log g_fl;
static int g_fl_on = -1;   // 开关缓存：属性一次性读入，之后每帧/每等只比标志

// 诊断开关（默认关）：Android 走两道门（编译期 S2A_ANDROID_DEBUG_ENV + 运行期
// debug.s2a.debug_env=1，见 native/debug_gate.h）后还要看 debug.s2a.frame_log=1——
// 写法同 nvn_vk_compute_bridge.c 的 debug.s2a.compute_trace；正式构建门关着，
// 玩家包里本开关恒为缺省关。宿主探针走同名环境变量——与 vk_device.c prop_int 同一惯例。
static int s2a_fl_enabled(void) {
    if (g_fl_on < 0) {
#if defined(__ANDROID__)
        char b[PROP_VALUE_MAX] = {0};
        g_fl_on = s2a_debug_entries_open()
                  && __system_property_get("debug.s2a.frame_log", b) == 1 && b[0] == '1';
#else
        const char *e = getenv("debug.s2a.frame_log");
        g_fl_on = (e && atoi(e)) ? 1 : 0;
#endif
    }
    return g_fl_on;
}

// 开关缓存标志（弱符号调用点先判它再计时/记账）：0=关 1=开。
int nvn_vk_frame_log_on(void) { return s2a_fl_enabled(); }

void s2a_frame_log_present_timed(uint64_t acquire_ns, uint64_t present_ns) {
    if (!s2a_fl_enabled()) return;
    struct timespec ts;
#if defined(CLOCK_BOOTTIME)
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);   // 宿主无 CLOCK_BOOTTIME（如 macOS 探针）：同一单调族
#endif
    s2a_fl_note_timed(&g_fl, (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull, acquire_ns, present_ns);
}
void nvn_vk_frame_log_present(void) { s2a_frame_log_present_timed(UINT64_MAX, UINT64_MAX); }

void nvn_vk_frame_log_fence_wait(uint64_t wait_ns) {
    if (!s2a_fl_enabled()) return;
    s2a_fl_fence(&g_fl, wait_ns);
}

static void s2a_fl_platform_emit(void *ctx, const char *line) {
    FILE *file = (FILE *)ctx;
    if (file) (void)fprintf(file, "%s\n", line);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "%s", line);
#else
    fprintf(stderr, "%s\n", line);   // 宿主探针/单测：走与 shim_log 相同的宿主 stderr 通道
#endif
}

void nvn_vk_frame_log_dump(void) {
    if (!s2a_fl_enabled() || !g_fl.seq) return;
    FILE *file = NULL;
#if defined(__ANDROID__)
    // PID 文件名由 READY 进程身份定位；每个进程第一次转储截断旧文件，后续段追加。
    static int started;
    const char *dir = getenv("S2A_FRAME_LOG_DIR");
    char path[4096];
    if (dir && dir[0] == '/') {
        int n = snprintf(path, sizeof path, "%s/frame-log-%ld.log", dir, (long)getpid());
        if (n > 0 && (size_t)n < sizeof path) file = fopen(path, started ? "a" : "w");
    }
    if (!file) __android_log_print(ANDROID_LOG_WARN, "S2Arm", "direct frame-log file open failed errno=%d", errno);
    else started = 1;
#endif
    s2a_fl_dump(&g_fl, s2a_fl_platform_emit, file);
    if (file && fclose(file) != 0) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_WARN, "S2Arm", "direct frame-log file write failed errno=%d", errno);
#endif
    }
    // 进程内可能再起新会话（直连↔兜底退回）：转储过的数据清掉，下一份转储只属于下一段会话。
    s2a_fl_reset(&g_fl);
}

// 托管层入口：direct 与 generic 共用同一单例；generic 的 present 线程与 direct 不同，
// 记录顺序即到达顺序（两条路同一局不会同时跑）。
int s2a_frame_log_on(void) { return nvn_vk_frame_log_on(); }
void s2a_frame_log_fence_wait(uint64_t wait_ns) { nvn_vk_frame_log_fence_wait(wait_ns); }
void s2a_frame_log_present(void) { nvn_vk_frame_log_present(); }
void s2a_frame_log_dump(void) { nvn_vk_frame_log_dump(); }
