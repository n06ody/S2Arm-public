// 驱动管线缓存文件层，见 nvn_vk_driver_cache.h。纯文件/SHA-256 计算，宿主与设备同一份。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nvn_vk_driver_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"

#define HDR_MAGIC 4
#define HDR_VERSION 4
#define HDR_SIZE 8
#define HDR_SHA 32
#define HDR_BYTES (HDR_MAGIC + HDR_VERSION + HDR_SIZE + HDR_SHA)

static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static void hex16(const uint8_t uuid[16], char out[33]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i * 2] = h[uuid[i] >> 4]; out[i * 2 + 1] = h[uuid[i] & 15]; }
    out[32] = 0;
}

int nvk_driver_cache_name(const uint8_t uuid[16], char out[NVK_DRIVER_CACHE_NAME]) {
    if (!uuid || !out) return -1;
    char h[33];
    hex16(uuid, h);
    int n = snprintf(out, NVK_DRIVER_CACHE_NAME, "%s-v%u.bin", h, NVK_DRIVER_CACHE_VERSION);
    return n > 0 && n < NVK_DRIVER_CACHE_NAME ? 0 : -1;
}

static int make_path(const char *dir, const uint8_t uuid[16], char *out, size_t out_size) {
    if (!dir || !dir[0]) return -1;
    char name[NVK_DRIVER_CACHE_NAME];
    if (nvk_driver_cache_name(uuid, name)) return -1;
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}

static int read_full(int fd, uint8_t *buf, size_t bytes) {
    size_t got = 0;
    while (got < bytes) {
        ssize_t n = read(fd, buf + got, bytes - got);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

int nvk_driver_cache_read_stamp(const char *dir, const uint8_t uuid[16], void **data, size_t *size,
                          struct nvk_driver_cache_stamp *stamp, char *why, size_t why_size) {
    if (data) *data = NULL;
    if (size) *size = 0;
    if (!data || !size || !uuid) { if (why && why_size) snprintf(why, why_size, "bad arguments"); return -1; }
    char path[512];
    if (make_path(dir, uuid, path, sizeof path)) { if (why && why_size) snprintf(why, why_size, "bad cache path"); return -1; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 1;
        if (why && why_size) snprintf(why, why_size, "open: errno=%d", errno);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        if (why && why_size) snprintf(why, why_size, "not a regular file");
        return -1;
    }
    if (st.st_size < HDR_BYTES || (uint64_t)st.st_size > (uint64_t)HDR_BYTES + NVK_DRIVER_CACHE_MAX) {
        close(fd);
        if (why && why_size) snprintf(why, why_size, "size %lld outside limits", (long long)st.st_size);
        return -1;
    }
    uint8_t hdr[HDR_BYTES];
    if (read_full(fd, hdr, sizeof hdr)) {
        close(fd);
        if (why && why_size) snprintf(why, why_size, "short header");
        return -1;
    }
    if (get_u32(hdr) != NVK_DRIVER_CACHE_MAGIC || get_u32(hdr + 4) != NVK_DRIVER_CACHE_VERSION) {
        close(fd);
        if (why && why_size) snprintf(why, why_size, "magic/version mismatch (magic=%08x version=%u)",
                                      get_u32(hdr), get_u32(hdr + 4));
        return -1;
    }
    uint64_t payload = get_u64(hdr + 8);
    if (payload == 0 || payload > NVK_DRIVER_CACHE_MAX || payload != (uint64_t)st.st_size - HDR_BYTES) {
        close(fd);
        if (why && why_size) snprintf(why, why_size, "payload %llu does not match file %lld",
                                      (unsigned long long)payload, (long long)st.st_size - HDR_BYTES);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)payload);
    if (!buf) {
        close(fd);
        if (why && why_size) snprintf(why, why_size, "out of memory (%llu bytes)", (unsigned long long)payload);
        return -1;
    }
    if (read_full(fd, buf, (size_t)payload)) {
        free(buf); close(fd);
        if (why && why_size) snprintf(why, why_size, "short payload");
        return -1;
    }
    close(fd);
    uint8_t digest[32];
    {
        s2a_sha256 ctx;
        s2a_sha256_init(&ctx);
        s2a_sha256_update(&ctx, buf, (size_t)payload);
        s2a_sha256_final(&ctx, digest);
    }
    if (memcmp(digest, hdr + 16, 32) != 0) {
        free(buf);
        if (why && why_size) snprintf(why, why_size, "sha256 mismatch");
        return -1;
    }
    if (stamp) {
        stamp->size = (size_t)payload;
        memcpy(stamp->digest, digest, sizeof digest);
    }
    *data = buf;
    *size = (size_t)payload;
    return 0;
}

int nvk_driver_cache_read(const char *dir, const uint8_t uuid[16], void **data, size_t *size,
                          char *why, size_t why_size) {
    return nvk_driver_cache_read_stamp(dir, uuid, data, size, NULL, why, why_size);
}

int nvk_driver_cache_store(const char *dir, const uint8_t uuid[16], const void *data, size_t size,
                           struct nvk_driver_cache_stamp *stamp, char *why, size_t why_size) {
    if (!data || !uuid || size == 0) { if (why && why_size) snprintf(why, why_size, "empty cache"); return -1; }
    if (size > NVK_DRIVER_CACHE_MAX) { if (why && why_size) snprintf(why, why_size, "cache too large (%zu)", size); return -1; }
    char path[512];
    if (make_path(dir, uuid, path, sizeof path)) { if (why && why_size) snprintf(why, why_size, "bad cache path"); return -1; }
    char tmp[560];
    int tn = snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof tmp) { if (why && why_size) snprintf(why, why_size, "temp path too long"); return -1; }
    uint8_t hdr[HDR_BYTES];
    put_u32(hdr, NVK_DRIVER_CACHE_MAGIC);
    put_u32(hdr + 4, NVK_DRIVER_CACHE_VERSION);
    put_u64(hdr + 8, (uint64_t)size);
    {
        s2a_sha256 ctx;
        s2a_sha256_init(&ctx);
        s2a_sha256_update(&ctx, data, size);
        s2a_sha256_final(&ctx, hdr + 16);
    }
    if (stamp && stamp->size == size && !memcmp(stamp->digest, hdr + 16, sizeof stamp->digest)) return 1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { if (why && why_size) snprintf(why, why_size, "temp open: errno=%d", errno); return -1; }
    const uint8_t *parts[2] = { hdr, (const uint8_t *)data };
    const size_t lens[2] = { sizeof hdr, size };
    int failed = 0;
    for (int i = 0; i < 2 && !failed; i++) {
        size_t off = 0;
        while (off < lens[i]) {
            ssize_t n = write(fd, parts[i] + off, lens[i] - off);
            if (n < 0) { if (errno == EINTR) continue; failed = 1; break; }
            off += (size_t)n;
        }
    }
    if (!failed && fsync(fd)) failed = 1;
    if (close(fd)) failed = 1;
    if (failed) {
        unlink(tmp);
        if (why && why_size) snprintf(why, why_size, "write/fsync failed: errno=%d", errno);
        return -1;
    }
    if (rename(tmp, path)) {
        unlink(tmp);
        if (why && why_size) snprintf(why, why_size, "rename: errno=%d", errno);
        return -1;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { (void)fsync(dfd); close(dfd); }   // 目录项也落盘；失败不回滚已成功的 rename
    if (stamp) {
        stamp->size = size;
        memcpy(stamp->digest, hdr + 16, sizeof stamp->digest);
    }
    return 0;
}

int nvk_driver_cache_write(const char *dir, const uint8_t uuid[16], const void *data, size_t size,
                           char *why, size_t why_size) {
    return nvk_driver_cache_store(dir, uuid, data, size, NULL, why, why_size);
}
