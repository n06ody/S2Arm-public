// 上传时展开的共享实现：目标格式来自 nvn_fmt_info.expand；像素位宽/类型转换规则
// （三分量补 a、UI2F/I2F、RGB10A2）仍按源格式语义写在这里，目标选择不重复。
#include "nvn_fmt_expand.h"
#include "nvn_fmt_info.h"
#include <string.h>

static int64_t rd(const uint8_t *p, int n, int sign) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    if (sign && n < 8 && (v >> (8 * n - 1) & 1)) v |= ~0ull << (8 * n);
    return (int64_t)v;
}

static void put(uint8_t *p, int n, uint64_t v) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int64_t sx(uint32_t v, int bits) {
    return v & (1u << (bits - 1)) ? (int64_t)v - ((int64_t)1 << bits) : (int64_t)v;
}

static uint16_t half_of(float x) {
    uint32_t u; memcpy(&u, &x, 4);
    uint32_t sgn = u >> 16 & 0x8000, e = u >> 23 & 0xff, m = u & 0x7fffff;
    if (e == 0xff) return (uint16_t)(sgn | 0x7c00 | (m ? 0x200 : 0));
    int ex = (int)e - 127 + 15;
    if (ex >= 31) return (uint16_t)(sgn | 0x7c00);
    if (ex <= 0) {
        if (ex < -10) return (uint16_t)sgn;
        m |= 0x800000; uint32_t sh = (uint32_t)(14 - ex), r = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (r & 1))) r++;
        return (uint16_t)(sgn | r);
    }
    uint32_t r = (uint32_t)ex << 10 | m >> 13, rem = m & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++;
    return (uint16_t)(sgn | r);
}

static void putf(uint8_t *p, int n, double x) {
    if (n == 2) put(p, 2, half_of((float)x));
    else { float f = (float)x; memcpy(p, &f, 4); }
}

int nvn_fmt_expand_target(uint32_t f, uint32_t *target, int *bytes_per_pixel) {
    nvn_fmt_info in, out;
    if (!nvn_fmt_info_of(f, &in) || !in.expand || !nvn_fmt_info_of(in.expand, &out)) return 0;
    if (target) *target = in.expand;
    if (bytes_per_pixel) *bytes_per_pixel = out.bytes;
    return 1;
}

int nvn_fmt_expand_upload(uint32_t f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h,
                          uint8_t *dst, size_t dpitch) {
    nvn_fmt_info in, out;
    if (!src || !dst || !nvn_fmt_info_of(f, &in) || !in.expand || !nvn_fmt_info_of(in.expand, &out)) return -1;
    int packed = f >= 0x69 && f <= 0x6C;
    int i2f = (f >= 0x51 && f <= 0x68) || f == 0x6B || f == 0x6C;
    int sign = in.kind == NVN_FMT_SINT || in.kind == NVN_FMT_SNORM;
    int nc = in.channels, cs = packed ? 0 : in.bytes / in.channels;
    int dc = out.channels, db = out.bytes, ds = db / dc;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * pitch;
        uint8_t *d = dst + y * dpitch;
        for (uint32_t x = 0; x < w; x++, s += in.bytes, d += db) {
            int64_t c[4] = {0, 0, 0, 0};
            if (packed) {
                uint32_t v = (uint32_t)rd(s, 4, 0);
                c[0] = v & 1023; c[1] = v >> 10 & 1023; c[2] = v >> 20 & 1023; c[3] = v >> 30;
                if (sign) { c[0] = sx((uint32_t)c[0], 10); c[1] = sx((uint32_t)c[1], 10); c[2] = sx((uint32_t)c[2], 10); c[3] = sx((uint32_t)c[3], 2); }
            } else {
                for (int k = 0; k < nc; k++) c[k] = rd(s + k * cs, cs, sign);
            }
            for (int k = 0; k < dc; k++) {
                uint8_t *o = d + k * ds;
                int one = k >= nc;
                if (i2f) putf(o, ds, one ? 1.0 : (double)c[k]);
                else if (f == 0x69) {
                    double x = (double)c[k] / (k == 3 ? 1.0 : 511.0);
                    // 两种负端编码在 SNORM16 的采样值都为 -1；保留各自位模式供原字节回读。
                    const int64_t sn = x < -1 ? -32768 : (int64_t)(x * 32767.0 + (x < 0 ? -0.5 : 0.5));
                    put(o, 2, (uint64_t)sn);
                } else if (f == 0x6A) {
                    put(o, 2, (uint64_t)c[k]);
                } else if (!one) {
                    memcpy(o, s + k * cs, (size_t)cs);
                } else {
                    switch (in.kind) {
                    case NVN_FMT_UNORM: put(o, ds, ~0ull); break;
                    case NVN_FMT_SNORM: put(o, ds, ~0ull >> (65 - 8 * ds)); break;
                    case NVN_FMT_FLOAT: if (ds == 2) put(o, 2, 0x3c00); else put(o, 4, 0x3f800000u); break;
                    default: put(o, ds, 1); break;
                    }
                }
            }
        }
    }
    return 0;
}
