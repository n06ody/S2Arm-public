// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Debug-only direct trace: bounded in-memory queue, immutable independently checked segments.
// A background writer persists every five seconds, including empty windows; Stop is not required.
#define _POSIX_C_SOURCE 200809L
#include "direct_diag.h"
#include "direct/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include "nvn_plat.h"
#include <sys/system_properties.h>
#include <android/log.h>
#endif

#ifndef S2A_DIRECT_DIAG_INTERVAL_NS
#define S2A_DIRECT_DIAG_INTERVAL_NS 5000000000ull
#endif
#define DIAG_CAP 4096u
struct diag_record {
    uint64_t seq, token, mono_ns, boot_us;
    uint32_t kind, stages;
    int submit, warm, success;
    uint8_t identity[32];
};
static struct diag_record records[DIAG_CAP];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int enabled;
static uint64_t session_id, record_seq, token_seq, drops, segment_seq, previous_boot;
static unsigned count;
static char dirpath[1024], session_uuid[65];

static void diag_red(const char *message) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "【红】direct trace: %s", message);
#else
    fprintf(stderr, "【红】direct trace: %s\n", message);
#endif
}
static uint64_t ns(clockid_t clock) {
    struct timespec t; if (clock_gettime(clock, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static uint64_t boot_us(void) {
#if defined(CLOCK_BOOTTIME)
    return ns(CLOCK_BOOTTIME) / 1000;
#else
    return ns(CLOCK_MONOTONIC) / 1000;
#endif
}
static int gate(void) {
#if defined(__ANDROID__)
    return nvn_plat_flag("direct_trace", 0) == 1;
#elif defined(S2A_DIRECT_DIAG_TEST)
    const char *e = getenv("S2A_DIRECT_DIAG_TEST_ENABLE"); return e && !strcmp(e, "1");
#else
    return 0;
#endif
}
int s2a_direct_diag_enabled(void) { return atomic_load_explicit(&enabled, memory_order_acquire); }
// Timestamps and queue sequence are captured while holding the same short lock.
static struct diag_record *reserve_locked(unsigned kind) {
    if (count == DIAG_CAP || record_seq == UINT64_MAX) { if (drops != UINT64_MAX) drops++; return NULL; }
    struct diag_record *r = &records[count++]; memset(r, 0, sizeof *r);
    r->seq = ++record_seq; r->kind = kind; r->mono_ns = ns(CLOCK_MONOTONIC); r->boot_us = boot_us();
    if (!r->mono_ns || !r->boot_us) { count--; drops++; return NULL; }
    return r;
}
void s2a_direct_diag_frame(void) {
    if (!s2a_direct_diag_enabled()) return;
    pthread_mutex_lock(&mu); (void)reserve_locked(1); pthread_mutex_unlock(&mu);
}
uint64_t s2a_direct_diag_compile_begin(const uint8_t id[32], unsigned stages, int submission, int warm_hit) {
    if (!s2a_direct_diag_enabled() || !id) return 0;
    pthread_mutex_lock(&mu);
    struct diag_record *r = reserve_locked(2);
    uint64_t token = 0;
    if (r && token_seq != UINT64_MAX) {
        token = ++token_seq; r->token = token; r->stages = stages; r->submit = !!submission; r->warm = !!warm_hit; memcpy(r->identity, id, 32);
    } else if (r) { count--; drops++; }
    pthread_mutex_unlock(&mu); return token;
}
void s2a_direct_diag_compile_end(uint64_t token, int success) {
    if (!token || !s2a_direct_diag_enabled()) return;
    pthread_mutex_lock(&mu);
    struct diag_record *r = reserve_locked(3);
    if (r) { r->token = token; r->success = !!success; }
    pthread_mutex_unlock(&mu);
}
static int line(FILE *f, s2a_sha256 *hash, const char *s) {
    size_t n = strlen(s); if (fwrite(s, 1, n, f) != n) return -1;
    s2a_sha256_update(hash, s, n); return 0;
}
// Called by the single writer; output is published only after complete write + fsync.
static int flush_segment(struct diag_record *batch, unsigned n, uint64_t start, uint64_t end, uint64_t lost, uint64_t seg) {
    char path[1280], tmp[1300];
    int a = snprintf(path, sizeof path, "%s/direct-trace-%ld-%llu-%06llu.jsonl", dirpath, (long)getpid(), (unsigned long long)session_id, (unsigned long long)seg);
    int b = snprintf(tmp, sizeof tmp, "%s.partial", path);
    if (a <= 0 || (size_t)a >= sizeof path || b <= 0 || (size_t)b >= sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600); if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w"); if (!f) { close(fd); unlink(tmp); return -1; }
    s2a_sha256 hash; s2a_sha256_init(&hash); char buf[512]; int bad = 0;
    snprintf(buf, sizeof buf, "{\"schema\":\"s2a-direct-trace-v1\",\"clock\":\"%s\",\"pid\":%ld,\"session\":%llu,\"uuid\":\"%s\",\"segment\":%llu,\"start_us\":%llu,\"end_us\":%llu,\"dropped\":%llu,\"count\":%u}\n",
#if defined(CLOCK_BOOTTIME)
             "CLOCK_BOOTTIME",
#else
             "CLOCK_MONOTONIC",
#endif
             (long)getpid(), (unsigned long long)session_id, session_uuid, (unsigned long long)seg, (unsigned long long)start, (unsigned long long)end, (unsigned long long)lost, n);
    if (line(f, &hash, buf)) bad = 1;
    for (unsigned i = 0; i < n && !bad; i++) {
        struct diag_record *r = &batch[i];
        if (r->kind == 1) snprintf(buf, sizeof buf, "{\"type\":\"frame\",\"seq\":%llu,\"mono_ns\":%llu,\"boot_us\":%llu}\n", (unsigned long long)r->seq, (unsigned long long)r->mono_ns, (unsigned long long)r->boot_us);
        else if (r->kind == 2) {
            char hex[65]; s2a_sha256_hex(r->identity, hex);
            snprintf(buf, sizeof buf, "{\"type\":\"compile_begin\",\"seq\":%llu,\"token\":%llu,\"identity\":\"%s\",\"stages\":%u,\"submit\":%d,\"warm_hit\":%d,\"mono_ns\":%llu,\"boot_us\":%llu}\n", (unsigned long long)r->seq, (unsigned long long)r->token, hex, r->stages, r->submit, r->warm, (unsigned long long)r->mono_ns, (unsigned long long)r->boot_us);
        } else snprintf(buf, sizeof buf, "{\"type\":\"compile_end\",\"seq\":%llu,\"token\":%llu,\"success\":%d,\"mono_ns\":%llu,\"boot_us\":%llu}\n", (unsigned long long)r->seq, (unsigned long long)r->token, r->success, (unsigned long long)r->mono_ns, (unsigned long long)r->boot_us);
        if (line(f, &hash, buf)) bad = 1;
    }
    uint8_t digest[32]; char hex[65]; s2a_sha256_final(&hash, digest); s2a_sha256_hex(digest, hex);
    if (!bad && fprintf(f, "{\"status\":\"complete\",\"sha256\":\"%s\"}\n", hex) < 0) bad = 1;
    if (fflush(f) || fsync(fd)) bad = 1;
    if (fclose(f)) bad = 1;
    if (!bad && link(tmp, path)) bad = 1;
    if (!bad && unlink(tmp)) bad = 1;
    if (bad) { unlink(tmp); return -1; }
    return 0;
}
static void *writer(void *unused) {
    (void)unused;
    struct diag_record *batch = malloc(sizeof records);
    if (!batch) { atomic_store(&enabled, 0); diag_red("分段缓冲分配失败，采集终止"); return NULL; }
    const struct timespec pause = {(time_t)(S2A_DIRECT_DIAG_INTERVAL_NS / 1000000000ull), (long)(S2A_DIRECT_DIAG_INTERVAL_NS % 1000000000ull)};
    for (;;) {
        struct timespec remain = pause; int rc;
        do { rc = nanosleep(&remain, &remain); } while (rc && errno == EINTR);
        if (rc) { atomic_store(&enabled, 0); diag_red("分段计时失败，后续采集不可用"); free(batch); return NULL; }
        pthread_mutex_lock(&mu);
        unsigned n = count; memcpy(batch, records, n * sizeof *batch); count = 0;
        uint64_t start = previous_boot, end = boot_us(), lost = drops, seg = segment_seq++;
        previous_boot = end;
        pthread_mutex_unlock(&mu);
        if (!end || end < start || flush_segment(batch, n, start, end, lost, seg)) {
            atomic_store(&enabled, 0); diag_red("分段写入失败，后续采集不可用"); free(batch); return NULL;
        }
    }
}
int s2a_direct_diag_start(const char *directory, uint64_t session) {
    if (!gate()) return 0;
    if (!directory || directory[0] != '/' || !session || strlen(directory) >= sizeof dirpath) return -1;
    pthread_mutex_lock(&mu);
    if (atomic_load(&enabled)) { pthread_mutex_unlock(&mu); return -1; }
    snprintf(dirpath, sizeof dirpath, "%s", directory); session_id = session;
    // 运行器生成的本局随机锚；缺失或非法时写空串，消费方不得把它当成本局证据。
    const char *uuid = NULL;
#if defined(__ANDROID__)
    char property[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.s2a.session_uuid", property) > 0) uuid = property;
#elif defined(S2A_DIRECT_DIAG_TEST)
    uuid = getenv("S2A_DIRECT_DIAG_TEST_UUID");
#endif
    session_uuid[0] = 0;
    if (uuid && strlen(uuid) == 32 && strspn(uuid, "0123456789abcdef") == 32)
        memcpy(session_uuid, uuid, 33);
    count = 0; drops = record_seq = token_seq = segment_seq = 0; previous_boot = boot_us();
    if (!previous_boot) { pthread_mutex_unlock(&mu); return -1; }
    pthread_attr_t attr; pthread_t th;
    if (pthread_attr_init(&attr)) { pthread_mutex_unlock(&mu); return -1; }
    int err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!err) { atomic_store_explicit(&enabled, 1, memory_order_release); err = pthread_create(&th, &attr, writer, NULL); }
    pthread_attr_destroy(&attr);
    if (err) atomic_store(&enabled, 0);
    pthread_mutex_unlock(&mu);
    if (err) { diag_red("后台线程启动失败"); return -1; }
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "direct trace enabled pid=%ld session=%llu uuid=%s interval_ns=%llu",
                        (long)getpid(), (unsigned long long)session_id, session_uuid[0] ? session_uuid : "-",
                        (unsigned long long)S2A_DIRECT_DIAG_INTERVAL_NS);
#endif
    return 0;
}
