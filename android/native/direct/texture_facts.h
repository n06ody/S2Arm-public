// F4.2B：NVN 纹理描述 → 统一编译器 program.textures[] 事实。
// 只输出能证明语义的组合：颜色格式 + 非 buffer 目标（rect 按上游 Texture2DRect 出 2d/不归一化）；
// 现行运行时入口对 buffer、UI2F/I2F 与表外不可表达目标一律不输出（编译器按 MISSING_FACT 准确失败）。
// 单独的 buffer 事实入口用于分步开发；在结果与绑定路径接通前不交给运行时生产者。
// handle 坐标：NVN 组合纹理表从 cbuf word 8（byte 0x20）起，每槽 8 字节 → handle = 8 + 2 * slot。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2A_TEX_FORMAT_MAX 40
#define S2A_NVN_FORMAT_UNKNOWN 0xFFFFFFFFu   // 格式表里「Unknown」那一项的 NVN 值：不是任何真实 NVN 格式（表外格式按它出事实，见 texture_facts.c）
#define S2A_TEX_SAMPLER_MAX 24

// F4.2B：纹理注册表里的真实描述（由适配层 s2a_vk_adapter_tex_facts 提供，不建视图、不上传）。
// 放在这里而不是 mtl_vk_adapter.h：graphics 与 compute 的编译请求都消费它，且这个头必须能被
// 宿主测试编译（mtl_vk_adapter.h 依赖 android/native_window.h）。
struct s2a_vk_tex_facts {
    uint32_t nvn_format, nvn_target, layers, levels;
    int samples;
};

struct s2a_nvn_texture_fact {
    char format[S2A_TEX_FORMAT_MAX];    // Ryujinx Format 名（编译器 TextureFormat）
    char sampler_type[S2A_TEX_SAMPLER_MAX]; // "2d" / "2d-array" / "cube" ...
    int is_buffer;
    int normalized;
    uint32_t array_length;
};

// 未映射的格式/目标返回 -1 并写 why；调用方记录 gap，不写请求事实。
int s2a_nvn_texture_fact(uint32_t nvn_format, uint32_t nvn_target, uint32_t layers,
                         struct s2a_nvn_texture_fact *out, char *why, size_t why_size);

// BUFFER facts for the staged implementation. Existing runtime producers must
// retain s2a_nvn_texture_fact's rejection until texel views can be bound.
// array_length is descriptor count 1, not the texture's element count.
int s2a_nvn_texture_buffer_fact(uint32_t nvn_format, struct s2a_nvn_texture_fact *out,
                                char *why, size_t why_size);

// 供测试枚举：返回 0..n-1 的已映射 NVN 格式；-1 结束。
int s2a_nvn_texture_format_at(unsigned index, uint32_t *nvn_format, const char **name);

// 身份编码用：把上面产出的 Format/SamplerType 名字映回稳定小整数。
// 名字未知返回 -1；调用方必须明确失败（不能截断、不能退回只比 key）。
// 索引与 s2a_nvn_texture_format_at / g_targets 同源，新增名字只改一处。
int s2a_nvn_texture_format_index(const char *name, uint16_t *out);
int s2a_nvn_texture_target_index(const char *name, uint8_t *out);

#ifdef __cplusplus
}
#endif
