// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 只读整图索引。格式和键与 pack/astc_pack.py 的 bc7-build 一致。
#include "direct/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { BC7_PACK_HEADER = 64, BC7_PACK_ENTRY = 80, BC7_PACK_MAX_ENTRIES = 1 << 18,
       BC7_PACK_MAX_IMAGE = 64 << 20 };

struct bc7_astc_pack {
    int fd;
    uint32_t count;
    uint8_t *index, *rejected;
    uint32_t rejected_count;
    uint8_t header[BC7_PACK_HEADER];
};

static uint64_t bc7_pack_le64(const uint8_t *p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; i++) n |= (uint64_t)p[i] << (8 * i);
    return n;
}

static int bc7_pack_read(int fd, void *dst, size_t len, uint64_t at) {
    uint8_t *p = dst;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        at += (uint64_t)n;
        len -= (size_t)n;
    }
    return 1;
}

static void bc7_pack_close(struct bc7_astc_pack *p) {
    if (!p) return;
    close(p->fd);
    free(p->index);
    free(p->rejected);
    free(p);
}

static struct bc7_astc_pack *bc7_pack_open(const char *path) {
    static const uint8_t protocol[16] = {'S','2','A','B','C','A','Q','2',2,0,0,0,BC7_PACK_ENTRY,0,0,0};
    static const uint8_t block[8] = {4,4,0,0,0,0,0,0};
    if (!path || !*path) return NULL;
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0) return NULL;
    struct bc7_astc_pack *p = calloc(1, sizeof *p);
    if (!p) { close(fd); return NULL; }
    p->fd = fd;
    struct stat st;
    uint8_t header[BC7_PACK_HEADER];
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < BC7_PACK_HEADER ||
        !bc7_pack_read(fd, header, sizeof header, 0) ||
        memcmp(header, protocol, sizeof protocol) || memcmp(header + 40, block, sizeof block)) goto bad;
    const uint64_t count = bc7_pack_le64(header + 16);
    const uint64_t ioff = bc7_pack_le64(header + 24);
    const uint64_t size = (uint64_t)st.st_size;
    if (count > BC7_PACK_MAX_ENTRIES || bc7_pack_le64(header + 32) != size ||
        ioff < BC7_PACK_HEADER || ioff > size || size - ioff != count * BC7_PACK_ENTRY) goto bad;
    if (count) {
        const size_t bytes = (size_t)count * BC7_PACK_ENTRY;
        p->index = malloc(bytes);
        if (!p->index || !bc7_pack_read(fd, p->index, bytes, ioff)) goto bad;
    }
    uint64_t next = BC7_PACK_HEADER;
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *e = p->index + i * BC7_PACK_ENTRY;
        const uint64_t n = bc7_pack_le64(e + 40);
        if ((i && memcmp(e - BC7_PACK_ENTRY, e, 32) >= 0) || bc7_pack_le64(e + 32) != next ||
            !n || (n & 15) || n > BC7_PACK_MAX_IMAGE || n > ioff - next) goto bad;
        next += n;
    }
    if (next != ioff) goto bad;
    p->count = (uint32_t)count;
    memcpy(p->header, header, sizeof header);
    return p;
bad:
    bc7_pack_close(p);
    return NULL;
}

static void bc7_pack_key(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers,
                         const void *bc, size_t len, uint8_t out[32]) {
    static const char tag[] = "s2a-bc7-astc-image-v1";
    const uint32_t words[] = {w, h, levels, layers};
    uint8_t dims[16];
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 4; j++)
        dims[4 * i + j] = (uint8_t)(words[i] >> (8 * j));
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    s2a_sha256_update(&hash, tag, sizeof tag - 1);
    s2a_sha256_update(&hash, dims, sizeof dims);
    s2a_sha256_update(&hash, bc, len);
    s2a_sha256_final(&hash, out);
}

static const uint8_t *bc7_pack_find(const struct bc7_astc_pack *p, const uint8_t key[32], size_t len) {
    if (!p) return NULL;
    uint32_t lo = 0, hi = p->count;
    while (lo < hi) {
        const uint32_t i = lo + (hi - lo) / 2;
        const uint8_t *e = p->index + (size_t)i * BC7_PACK_ENTRY;
        const int cmp = memcmp(e, key, 32);
        if (cmp < 0) lo = i + 1;
        else if (cmp > 0) hi = i;
        else return bc7_pack_le64(e + 40) == len ? e : NULL;
    }
    return NULL;
}

static int bc7_pack_lookup(const struct bc7_astc_pack *p, const uint8_t key[32], void *dst, size_t len) {
    const uint8_t *e = bc7_pack_find(p, key, len);
    if (!e || !bc7_pack_read(p->fd, dst, len, bc7_pack_le64(e + 32))) return 0;
    s2a_sha256 hash;
    uint8_t digest[32];
    s2a_sha256_init(&hash);
    s2a_sha256_update(&hash, dst, len);
    s2a_sha256_final(&hash, digest);
    return !memcmp(digest, e + 48, sizeof digest);
}

// 诊断回执绑定已读的包索引。旧包、截断回执或键无序时，未命中仍归未知键。
static void bc7_pack_load_rejects(struct bc7_astc_pack *p, const char *path) {
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0) return;
    struct stat st;
    uint8_t h[48], hash[32];
    uint8_t *keys = NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof h ||
        !bc7_pack_read(fd, h, sizeof h, 0) || memcmp(h, "S2ABCQR1", 8)) goto done;
    const uint64_t count = bc7_pack_le64(h + 8); // 低四字节是项数，高四字节必须为零。
    if (count > BC7_PACK_MAX_ENTRIES || (uint64_t)st.st_size != sizeof h + count * 32 || !count) goto done;
    s2a_sha256 digest;
    s2a_sha256_init(&digest);
    s2a_sha256_update(&digest, p->header, sizeof p->header);
    if (p->count) s2a_sha256_update(&digest, p->index, (size_t)p->count * BC7_PACK_ENTRY);
    s2a_sha256_final(&digest, hash);
    if (memcmp(hash, h + 16, 32)) goto done;
    keys = malloc((size_t)count * 32);
    if (!keys || !bc7_pack_read(fd, keys, (size_t)count * 32, sizeof h)) goto done;
    for (uint64_t i = 1; i < count; i++) if (memcmp(keys + (i-1)*32, keys + i*32, 32) >= 0) goto done;
    p->rejected = keys;
    p->rejected_count = (uint32_t)count;
    keys = NULL;
done:
    free(keys);
    close(fd);
}

static int bc7_pack_rejected(const struct bc7_astc_pack *p, const uint8_t key[32]) {
    uint32_t lo = 0, hi = p->rejected_count;
    while (lo < hi) {
        const uint32_t i = lo + (hi - lo) / 2;
        const int cmp = memcmp(p->rejected + (size_t)i * 32, key, 32);
        if (cmp < 0) lo = i + 1;
        else if (cmp > 0) hi = i;
        else return 1;
    }
    return 0;
}
