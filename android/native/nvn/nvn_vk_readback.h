// NVN ReadTexels（纹理读回）后端入口。
//
// 两个事实来源，按“当前内容归谁”选：
//   · 内容归 CPU（guest 内存里的纹理存储就是事实来源）：按 NVN 块线性布局 deswizzle 后拷贝，
//     不解析/不为此上传图像。刚被 CPU 填写、还没被任何 draw 用过的纹理正是这一类（0100F2C0115B6000
//     启动期把 1024x1024 BC4 材质贴图逐块填好再 ReadTexels 拼 atlas）。
//   · 内容归 GPU（图像是事实来源）：先拿帧队列的完成证明（pass_break(1)），再把图像按区域
//     VkBufferImageCopy 到 host-visible staging，fence 等到完成后打包进 dst。
// 内容归属由 nvn_vk_texcache 的 gpu_written 判定；录制层的 B2T/T2T 拷贝与 clear 都会登记，
// 不会出现“GPU 写过但被判成 CPU 内容”的静默错读。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nvn_obj.h"               // tex_t / NVNcopyRegion
#include "nvn_vk_readback_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nvn_vk_session;
struct nvn_vk_tex;
struct s2a_vk_device;

// 读回入口（调用方持 adapter 执行包 + g_mu）。成功返回 0；失败返回 -1 并把可定位原因
// （serial / 尺寸 / 格式 / level / layer / 区域 / 哪一步）写进 why。
int nvn_vk_session_read_texels(struct nvn_vk_session *s, const tex_t *t, unsigned level, unsigned layer0,
                               const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img,
                               char *why, size_t why_size);

// 会话销毁前释放读回用的 staging 缓存（设备空闲证明之后调用）。
void nvn_vk_session_read_texels_release(struct nvn_vk_session *s);

// GPU 图像 → host-visible staging → dst 的打包（内容归 GPU 时使用）。调用方必须已经拿到
// 帧队列的完成证明；本函数只负责“提交一次拷贝并等 fence + 按 plan 打包”。
// 成功返回 0；失败返回 -1 并写 why（包括“图像存储格式与 NVN 格式不同，无法还原字节布局”）。
int nvn_vk_readback_gpu(struct s2a_vk_device *d, const struct nvn_vk_tex *tex,
                        const struct nvn_vk_rb_plan *plan, uint8_t *dst,
                        char *why, size_t why_size);
// 会话销毁前释放读回 staging 缓存（设备空闲证明之后；等不到完成证明的缓冲会保留）。
void nvn_vk_readback_gpu_release(struct s2a_vk_device *d);

// 读回路径 fence 等待的不清零累计 ns（逐帧 TSV 的 rb_us 源；只在 frame_tsv 开关开时计时）。
uint64_t nvn_vk_readback_wait_ns_total(void);

#ifdef __cplusplus
}
#endif
