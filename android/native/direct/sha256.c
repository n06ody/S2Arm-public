// SHA-256 实现（FIPS 180-4），见 sha256.h。
#include "sha256.h"

#include <string.h>

#if defined(__aarch64__) && !defined(S2A_SHA256_PORTABLE_ONLY) && (defined(__linux__) || defined(__ARM_FEATURE_SHA2))
#define S2A_SHA256_ARM 1
#include <arm_neon.h>
#ifdef __linux__
#include <asm/hwcap.h>
#include <stdatomic.h>
#include <sys/auxv.h>
#endif
#endif

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void compress_portable(s2a_sha256 *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

#ifdef S2A_SHA256_ARM
#if defined(__clang__)
__attribute__((target("sha2")))
#else
__attribute__((target("arch=armv8-a+crypto")))
#endif
static void compress_sha2(s2a_sha256 *ctx, const uint8_t block[64]) {
    uint32x4_t abcd = vld1q_u32(ctx->h), efgh = vld1q_u32(ctx->h + 4);
    const uint32x4_t first = abcd, second = efgh;
    uint32x4_t words[4];
    for (unsigned i = 0; i < 4; i++)
        words[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16 * i)));
    for (unsigned group = 0; group < 16; group++) {
        unsigned i = group & 3;
        uint32x4_t wk = vaddq_u32(words[i], vld1q_u32(K + 4 * group));
        uint32x4_t previous = abcd;
        abcd = vsha256hq_u32(abcd, efgh, wk);
        efgh = vsha256h2q_u32(efgh, previous, wk);
        if (group < 12)
            words[i] = vsha256su1q_u32(vsha256su0q_u32(words[i], words[(i + 1) & 3]),
                                       words[(i + 2) & 3], words[(i + 3) & 3]);
    }
    vst1q_u32(ctx->h, vaddq_u32(first, abcd));
    vst1q_u32(ctx->h + 4, vaddq_u32(second, efgh));
}
#endif

static void compress_block(s2a_sha256 *ctx, const uint8_t block[64]) {
#ifdef S2A_SHA256_ARM
#ifdef __linux__
    // SHA2 is optional for the Android arm64 ABI. Only the specialized function uses it.
    static _Atomic int supported;
    int mode = atomic_load_explicit(&supported, memory_order_relaxed);
    if (!mode) {
        mode = (getauxval(AT_HWCAP) & HWCAP_SHA2) ? 2 : 1;
        atomic_store_explicit(&supported, mode, memory_order_relaxed);
    }
    if (mode == 2)
#endif
    {
        compress_sha2(ctx, block);
        return;
    }
#endif
    compress_portable(ctx, block);
}

void s2a_sha256_init(s2a_sha256 *ctx) {
    ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85; ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c; ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
    ctx->bytes = 0;
    ctx->used = 0;
}

void s2a_sha256_update(s2a_sha256 *ctx, const void *data, size_t bytes) {
    const uint8_t *p = data;
    ctx->bytes += bytes;
    while (bytes) {
        if (ctx->used == 0 && bytes >= 64) {
            compress_block(ctx, p);
            p += 64; bytes -= 64;
            continue;
        }
        size_t take = 64 - ctx->used;
        if (take > bytes) take = bytes;
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += (uint32_t)take;
        p += take; bytes -= take;
        if (ctx->used == 64) { compress_block(ctx, ctx->block); ctx->used = 0; }
    }
}

void s2a_sha256_final(s2a_sha256 *ctx, uint8_t out[32]) {
    uint64_t bits = ctx->bytes * 8;
    uint8_t pad = 0x80;
    s2a_sha256_update(ctx, &pad, 1);
    uint8_t zero = 0;
    while (ctx->used != 56) s2a_sha256_update(ctx, &zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - i * 8));
    // 直接写长度，避免把长度也计入 bytes 后再次触发分块计数。
    memcpy(ctx->block + 56, length, 8);
    compress_block(ctx, ctx->block);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
}

void s2a_sha256_hex(const uint8_t digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
}
