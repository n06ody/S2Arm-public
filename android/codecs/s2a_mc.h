#ifndef S2A_MC_H
#define S2A_MC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* AMediaCodec 出图边界（#44 第二刀）：把 VP9 比特流交给硬件解码器，输出直接
 * 写进调用方给的 Y/U/V 三平面（caller 持有存储，不是借出视图）。半平面输出
 * 在这里解交织成 I420 形——ISurface 的 U/V 是独立平面，WriteChroma 按行读
 * 连续字节，表达不了交错 UV；字节序按 color-format 分（NV12 U 在前、QCOM
 * YVU420SemiPlanar V 在前）。
 *
 * 契约：owner 顺序调用，无并发 decode/destroy。一个 context 失败即保持失败，
 * 由 C# 侧决定回退托管（建/配/首帧失败）还是维持失败（已出帧后）。硬件给不出的
 * mvsOut / BackwardUpdateCounts 由 C# 侧清零，不在这里伪造。
 *
 * debug.s2a.video_hw 开关在 probe/create 里读：v[0]=='1' 才碰 MediaCodec，
 * 否则一律 UNSUPPORTED——调用方据此走托管，行为与接槽前一致。 */
#define S2A_MC_ABI 1u
#define S2A_MC_PROVENANCE "S2ARM_AMEDIACODEC"
typedef struct s2a_mc s2a_mc;
enum { S2A_MC_ARGUMENT=-1, S2A_MC_NOMEM=-2, S2A_MC_UNSUPPORTED=-3,
       S2A_MC_BITSTREAM=-4, S2A_MC_CONTRACT=-5, S2A_MC_RECONFIG=-6,
       S2A_MC_STATE=-7, S2A_MC_BACKEND=-8, S2A_MC_LIMIT=-9 };
enum { S2A_MC_STAGE_PROBE=1, S2A_MC_STAGE_CREATE=2, S2A_MC_STAGE_FORMAT=3,
       S2A_MC_STAGE_START=4, S2A_MC_STAGE_INPUT=5, S2A_MC_STAGE_DEQUEUE=6,
       S2A_MC_STAGE_OUTPUT=7, S2A_MC_STAGE_COPY=8, S2A_MC_STAGE_STOP=9 };
enum { S2A_MC_CODEC_VP9=1 };
typedef struct { uint32_t abi_version, struct_size, codec, width, height, reserved; } s2a_mc_config;
typedef struct {
    uint32_t abi_version, struct_size, stage;
    int32_t code, media_status;
    uint32_t consumed, decoded, output, input_token, output_token;
    uint32_t width, height, generation;
    /* 最近一次输出格式里厂商实际报的值：失败时随 diag 带出，供真机按机型对照。 */
    int32_t color_format;
    uint32_t out_stride, out_slice_h;
} s2a_mc_diag;
typedef struct {
    uint8_t *data;
    uint64_t capacity;
    uint32_t stride, reserved;
} s2a_mc_plane;
/* 探测 codec 可用性（开关 + 建得起来 + 配得成）。返回 0 可用；<0 见 enum。 */
int32_t s2a_mc_probe(uint32_t abi_version, uint32_t codec, uint32_t width, uint32_t height, s2a_mc_diag *);
int32_t s2a_mc_create(const s2a_mc_config *, s2a_mc **, s2a_mc_diag *);
/* 每次调用：排队一个 AU → 有界排空输出 → 最多出一帧写进 dst[3]（Y,U,V）。
 * 硬解有管线延迟：返回 0 且 diag->output==0 表示「输入已收、帧未到」，
 * 属可恢复态——下一调用继续排，早前输入的帧会在后续调用里按序交付；
 * show_frame=0 的隐藏帧天然走 output==0 路径，不判败。判死盯「连续零
 * 输出调用数」：任何一次出图清零，连续超过 S2A_MC_MAX_EMPTY 才回 LIMIT
 * （隐藏帧只可能抬高连续值，不可能累计误判）。真错误（<0）置粘性失败。
 * diag->output_token 记的是本次调用的输入 token，不是出图那帧的输入。
 * width/height 与配置不符回 RECONFIG：C ABI 只报告，由调用方（托管封装的
 * Ensure）销毁重建——经 C# 包装层调用时 RECONFIG 实际不可达。 */
int32_t s2a_mc_decode(s2a_mc *, const uint8_t *au, uint32_t au_len,
                      uint32_t width, uint32_t height, const s2a_mc_plane *dst,
                      uint32_t token, s2a_mc_diag *);
/* 不进新输入、只排空输出：budget_us=0 单次不阻塞轮询，>0 有界等到预算。
 * 拿到一帧 output=1（empty_streak 清零），没拿到 output=0 返回 0（不是错误，
 * 也不算连续零输出调用）。超帧一个 AU 多帧时连续 wait(0) 取到队空、用最后一帧，
 * 与 vpx 侧 frame_to_show=最后帧同语义。 */
int32_t s2a_mc_wait(s2a_mc *, const s2a_mc_plane *dst, uint32_t budget_us, s2a_mc_diag *);
int32_t s2a_mc_destroy(s2a_mc *, s2a_mc_diag *);
const char *s2a_mc_provenance(void);
#ifdef S2A_MC_TESTING
void s2a_mc_test_set_hw(int);
#endif
#ifdef __cplusplus
}
#endif
#endif
