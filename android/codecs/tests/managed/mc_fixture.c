/* managed_test.py 的 s2arm_android 替身：按 S2A_MC_FAKE_MODE 演出 AMediaCodec shim
 * 的各条路径。不是解码器——只验证 C# 后端的调用/回退行为。
 * 模式：ok（默认，出图写定值）/ noprobe / createfail / decodefail / latefail（第二帧起失败）
 *      / pending（decode 不出图、wait 出图）/ starve（decode 与 wait 都不出图）。 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *data; uint64_t capacity; uint32_t stride, reserved; } mc_dst;
typedef struct {
    uint32_t abi_version, struct_size, stage;
    int32_t code, media_status;
    uint32_t consumed, decoded, output, input_token, output_token;
    uint32_t width, height, generation;
    int32_t color_format;
    uint32_t out_stride, out_slice_h;
} mc_diag;

static int mode_is(const char *want)
{
    const char *m = getenv("S2A_MC_FAKE_MODE");
    return m && strcmp(m, want) == 0;
}

int32_t s2a_mc_probe(uint32_t abi, uint32_t codec, uint32_t w, uint32_t h, mc_diag *d)
{
    (void)abi; (void)codec; (void)w; (void)h;
    if (mode_is("noprobe")) { if (d) { d->stage = 1; d->code = -3; } return -3; }
    return 0;
}

int32_t s2a_mc_create(const void *cfg, void **out, mc_diag *d)
{
    (void)cfg;
    if (mode_is("createfail")) { if (d) { d->stage = 2; d->code = -8; } return -8; }
    *out = calloc(1, 16);
    return *out ? 0 : -2;
}

int32_t s2a_mc_decode(void *h, const uint8_t *au, uint32_t len, uint32_t w, uint32_t h2,
                      const mc_dst *dst, uint32_t token, mc_diag *d)
{
    (void)au; (void)len;
    static uint32_t calls;
    calls++;
    if (mode_is("decodefail") || (mode_is("latefail") && calls > 1)) {
        if (d) { d->stage = 6; d->code = -8; d->input_token = token; }
        return -8;
    }
    if (mode_is("starve") || (mode_is("pending") && calls == 1)) {
        /* 输入已收、帧在管线：rc=0 但 output=0，等调用方 wait 排空；
         * starve 每次调用都不出图（codec 活着但一直欠账）。 */
        if (d) { d->consumed = len; d->decoded = 1; d->output = 0; d->input_token = token; }
        return 0;
    }
    for (uint32_t r = 0; r < h2; r++)
        memset(dst[0].data + (uint64_t)r * dst[0].stride, 0x55, w);
    for (uint32_t r = 0; r < h2 / 2; r++) {
        memset(dst[1].data + (uint64_t)r * dst[1].stride, 0x66, w / 2);
        memset(dst[2].data + (uint64_t)r * dst[2].stride, 0x77, w / 2);
    }
    if (d) { d->decoded = 1; d->output = 1; d->output_token = token; d->width = w; d->height = h2; }
    return 0;
}

int32_t s2a_mc_wait(void *h, const mc_dst *dst, uint32_t budget_us, mc_diag *d)
{
    (void)h; (void)budget_us;
    if (mode_is("pending")) {
        /* 管线欠账在 wait 里补齐：写与 decode 相同的定值出图 */
        for (uint32_t r = 0; r < 16; r++)
            memset(dst[0].data + (uint64_t)r * dst[0].stride, 0x55, 32);
        for (uint32_t r = 0; r < 8; r++) {
            memset(dst[1].data + (uint64_t)r * dst[1].stride, 0x66, 16);
            memset(dst[2].data + (uint64_t)r * dst[2].stride, 0x77, 16);
        }
        if (d) { d->output = 1; }
        return 0;
    }
    /* starve 与其余模式：队列里没有可出的图 */
    if (d) { d->output = 0; }
    return 0;
}

int32_t s2a_mc_destroy(void *h, mc_diag *d) { (void)d; free(h); return 0; }
const char *s2a_mc_provenance(void) { return "S2ARM_AMEDIACODEC"; }
