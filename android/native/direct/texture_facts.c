#include "texture_facts.h"

#include <stdio.h>
#include <string.h>

#include "nvn.h"

// NVN 颜色格式 → Ryujinx Format 名（编译器 TextureFormat 枚举）。表外的格式（BC / sRGB / 565 / 深度模板 …）照样出事实，格式写 "Unknown"：
//   格式事实只在图像读写（QueryTextureFormat）时有意义，采样器类型、坐标是否归一化与格式无关，Ryujinx 自己对这类贴图也给 Unknown。
//   编译器在图像读写查到 Unknown 时才报 MISSING_FACT（DirectGpuAccessor.QueryTextureFormat）。10-31 暗黑3 安卓：第一笔片元着色器采样 DXT1 / DXT5，
//   原先整条事实不出，QueryTextureCoordNormalized 缺事实、整份请求被拒、进程退出。UI2F / I2F 仍拒（整数格式按浮点读，归一化与否要另证）。
static const struct { uint32_t f; const char *name; } g_formats[] = {
    // 编译器 TextureFormat 是一个较小的枚举（Ryujinx.Graphics.Shader.TextureFormat）：只接受
    // 这里列出的名字。Bc*/sRGB/565/4444/3 通道/深度等格式不在枚举里，输出会被整份请求拒绝，
    // 所以它们不产出事实（Shaders 若真查询则 MISSING_FACT，和空 textures[] 时一致，不回归）。
    { NVN_FORMAT_R8, "R8Unorm" }, { NVN_FORMAT_R8SN, "R8Snorm" },
    { NVN_FORMAT_R8UI, "R8Uint" }, { NVN_FORMAT_R8I, "R8Sint" },
    { NVN_FORMAT_R16F, "R16Float" }, { NVN_FORMAT_R16, "R16Unorm" }, { NVN_FORMAT_R16SN, "R16Snorm" },
    { NVN_FORMAT_R16UI, "R16Uint" }, { NVN_FORMAT_R16I, "R16Sint" },
    { NVN_FORMAT_R32F, "R32Float" }, { NVN_FORMAT_R32UI, "R32Uint" }, { NVN_FORMAT_R32I, "R32Sint" },
    { NVN_FORMAT_RG8, "R8G8Unorm" }, { NVN_FORMAT_RG8SN, "R8G8Snorm" },
    { NVN_FORMAT_RG8UI, "R8G8Uint" }, { NVN_FORMAT_RG8I, "R8G8Sint" },
    { NVN_FORMAT_RG16F, "R16G16Float" }, { NVN_FORMAT_RG16, "R16G16Unorm" }, { NVN_FORMAT_RG16SN, "R16G16Snorm" },
    { NVN_FORMAT_RG16UI, "R16G16Uint" }, { NVN_FORMAT_RG16I, "R16G16Sint" },
    { NVN_FORMAT_RG32F, "R32G32Float" }, { NVN_FORMAT_RG32UI, "R32G32Uint" }, { NVN_FORMAT_RG32I, "R32G32Sint" },
    { NVN_FORMAT_RGBA8, "R8G8B8A8Unorm" }, { NVN_FORMAT_RGBA8SN, "R8G8B8A8Snorm" },
    { NVN_FORMAT_RGBA8UI, "R8G8B8A8Uint" }, { NVN_FORMAT_RGBA8I, "R8G8B8A8Sint" },
    { NVN_FORMAT_RGBA16F, "R16G16B16A16Float" }, { NVN_FORMAT_RGBA16, "R16G16B16A16Unorm" },
    { NVN_FORMAT_RGBA16SN, "R16G16B16A16Snorm" }, { NVN_FORMAT_RGBA16UI, "R16G16B16A16Uint" },
    { NVN_FORMAT_RGBA16I, "R16G16B16A16Sint" },
    { NVN_FORMAT_RGBA32F, "R32G32B32A32Float" }, { NVN_FORMAT_RGBA32UI, "R32G32B32A32Uint" },
    { NVN_FORMAT_RGBA32I, "R32G32B32A32Sint" },
    { NVN_FORMAT_RGB10A2, "R10G10B10A2Unorm" }, { NVN_FORMAT_RGB10A2UI, "R10G10B10A2Uint" },
    { NVN_FORMAT_R11G11B10F, "R11G11B10Float" },
    { S2A_NVN_FORMAT_UNKNOWN, "Unknown" },   // 表外格式（见上）；放表尾，前面各项的身份索引不变
};

// NVN 目标 → 编译器 SamplerType 名。生产者 s2a_nvn_texture_fact 与身份编码的反向查找
// 共用这一张表：名字只在这里定义一次，避免两处漂移。
// layers_kind 决定该目标的 array_length 语义。
enum { LAYERS_ONE = 0, LAYERS_COUNT = 1, LAYERS_CUBE = 2 };
static const struct { uint32_t target; const char *name; int layers_kind; } g_targets[] = {
    { NVN_TEXTURE_TARGET_1D, "1d", LAYERS_ONE },
    { NVN_TEXTURE_TARGET_2D, "2d", LAYERS_ONE },
    { NVN_TEXTURE_TARGET_3D, "3d", LAYERS_ONE },
    { NVN_TEXTURE_TARGET_1D_ARRAY, "1d-array", LAYERS_COUNT },
    { NVN_TEXTURE_TARGET_2D_ARRAY, "2d-array", LAYERS_COUNT },
    { NVN_TEXTURE_TARGET_2D_MULTISAMPLE, "2d-ms", LAYERS_ONE },
    { NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY, "2d-ms-array", LAYERS_COUNT },
    { NVN_TEXTURE_TARGET_CUBEMAP, "cube", LAYERS_ONE },
    { NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, "cube-array", LAYERS_CUBE },
    // RECTANGLE：上游 TextureTarget.Texture2DRect → ConvertSamplerType = Texture2D。
    // rect 的坐标空间就是纹素单位（不归一化），normalized 位按 0 出事实。
    { NVN_TEXTURE_TARGET_RECTANGLE, "2d", LAYERS_ONE },
    // Append only: the table index is part of the persisted S2AI1 identity.
    { NVN_TEXTURE_TARGET_BUFFER, "buffer", LAYERS_ONE },
};

// 身份编码用：名字 → 稳定小整数。未知名字返回 -1（编码方必须明确失败，不截断、不猜）。
int s2a_nvn_texture_target_index(const char *name, uint8_t *out) {
    if (!name || !out) return -1;
    for (size_t i = 0; i < sizeof g_targets / sizeof g_targets[0]; i++)
        if (!strcmp(g_targets[i].name, name)) { *out = (uint8_t)i; return 0; }
    return -1;
}

int s2a_nvn_texture_format_index(const char *name, uint16_t *out) {
    if (!name || !out) return -1;
    for (size_t i = 0; i < sizeof g_formats / sizeof g_formats[0]; i++)
        if (!strcmp(g_formats[i].name, name)) { *out = (uint16_t)i; return 0; }
    return -1;
}

static const char *format_name(uint32_t f) {
    for (size_t i = 0; i < sizeof g_formats / sizeof g_formats[0]; i++)
        if (g_formats[i].f == f) return g_formats[i].name;
    return NULL;
}

int s2a_nvn_texture_format_at(unsigned index, uint32_t *nvn_format, const char **name) {
    if (index >= sizeof g_formats / sizeof g_formats[0]) return -1;
    if (nvn_format) *nvn_format = g_formats[index].f;
    if (name) *name = g_formats[index].name;
    return 0;
}

static void setwhy(char *why, size_t n, const char *text) { if (why && n) snprintf(why, n, "%s", text); }
static int int_to_float_format(uint32_t f) {   // UI2F / I2F：整数存储按浮点读，归一化与否表里没有依据
    return f == NVN_FORMAT_R8_UI2F || f == NVN_FORMAT_R8_I2F || f == NVN_FORMAT_R16_UI2F || f == NVN_FORMAT_R16_I2F || f == NVN_FORMAT_R32_UI2F || f == NVN_FORMAT_R32_I2F
        || f == NVN_FORMAT_RG8_UI2F || f == NVN_FORMAT_RG8_I2F || f == NVN_FORMAT_RG16_UI2F || f == NVN_FORMAT_RG16_I2F || f == NVN_FORMAT_RG32_UI2F || f == NVN_FORMAT_RG32_I2F
        || f == NVN_FORMAT_RGB8_UI2F || f == NVN_FORMAT_RGB8_I2F || f == NVN_FORMAT_RGB16_UI2F || f == NVN_FORMAT_RGB16_I2F || f == NVN_FORMAT_RGB32_UI2F || f == NVN_FORMAT_RGB32_I2F
        || f == NVN_FORMAT_RGBA8_UI2F || f == NVN_FORMAT_RGBA8_I2F || f == NVN_FORMAT_RGBA16_UI2F || f == NVN_FORMAT_RGBA16_I2F || f == NVN_FORMAT_RGBA32_UI2F || f == NVN_FORMAT_RGBA32_I2F
        || f == NVN_FORMAT_RGB10A2_UI2F || f == NVN_FORMAT_RGB10A2_I2F;
}

// 缓存：本函数是 (nvn_format, nvn_target, layers) 的纯函数，而逐笔 draw 会对同一批纹理
// 反复调用它（实测 22 条/笔 × 274 笔/帧）。命中省掉格式表线性查找 + 两次 snprintf + strstr。
// 直接映射、容量小、只由渲染线程访问；失败结果也缓存（否则未映射格式会每笔重算）。
static int s2a_nvn_texture_fact_compute(uint32_t nvn_format, uint32_t nvn_target, uint32_t layers,
                                        struct s2a_nvn_texture_fact *out, char *why, size_t why_size);
#define FACT_MEMO 64
static struct {
    uint32_t fmt, target, layers;
    int ok;
    struct s2a_nvn_texture_fact v;
} g_fact_memo[FACT_MEMO];

int s2a_nvn_texture_fact(uint32_t nvn_format, uint32_t nvn_target, uint32_t layers,
                         struct s2a_nvn_texture_fact *out, char *why, size_t why_size) {
    if (!out) return -1;
    // Existing producers cannot bind a texel-buffer view yet. Preserve their
    // missing-fact rejection until the result and replay paths are connected.
    if (nvn_target == NVN_TEXTURE_TARGET_BUFFER) { setwhy(why, why_size, "buffer texture execution is not implemented"); return -1; }
    {
        uint32_t h = (nvn_format * 2654435761u) ^ (nvn_target * 40503u) ^ (layers * 2246822519u);
        unsigned k = h % FACT_MEMO;
        if (g_fact_memo[k].fmt == nvn_format && g_fact_memo[k].target == nvn_target &&
            g_fact_memo[k].layers == layers) {
            if (g_fact_memo[k].ok) { *out = g_fact_memo[k].v; return 0; }
            setwhy(why, why_size, "NVN 纹理格式/目标未映射（已缓存）");
            return -1;
        }
        int rc = s2a_nvn_texture_fact_compute(nvn_format, nvn_target, layers, out, why, why_size);
        g_fact_memo[k].fmt = nvn_format; g_fact_memo[k].target = nvn_target; g_fact_memo[k].layers = layers;
        g_fact_memo[k].ok = (rc == 0);
        if (rc == 0) g_fact_memo[k].v = *out;
        return rc;
    }
}

static int s2a_nvn_texture_fact_compute(uint32_t nvn_format, uint32_t nvn_target, uint32_t layers,
                                        struct s2a_nvn_texture_fact *out, char *why, size_t why_size) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (int_to_float_format(nvn_format)) { setwhy(why, why_size, "UI2F / I2F 格式不出事实（整数按浮点读）"); return -1; }
    const char *fmt = format_name(nvn_format);
    if (!fmt) fmt = "Unknown";

    const char *base = NULL;
    uint32_t array_length = 1;
    int found = 0;
    for (size_t i = 0; i < sizeof g_targets / sizeof g_targets[0]; i++) {
        if (g_targets[i].target != nvn_target) continue;
        found = 1;
        base = g_targets[i].name;
        if (g_targets[i].layers_kind == LAYERS_COUNT) array_length = layers ? layers : 1;
        else if (g_targets[i].layers_kind == LAYERS_CUBE) {
            if (!layers || layers % 6) { setwhy(why, why_size, "cubemap array 的层数不是 6 的倍数"); return -1; }
            array_length = layers / 6;
        }
        break;
    }
    if (!found) { setwhy(why, why_size, "NVN 纹理目标未映射（buffer/未知）"); return -1; }
    if (array_length > 65536) { setwhy(why, why_size, "数组层数超出编译器上限"); return -1; }

    snprintf(out->format, sizeof out->format, "%s", fmt);
    snprintf(out->sampler_type, sizeof out->sampler_type, "%s", base);
    out->array_length = array_length;
    out->is_buffer = nvn_target == NVN_TEXTURE_TARGET_BUFFER;
    // 坐标归一不归一只看有效目标（视图设了目标取视图的，否则取贴图的）：RECTANGLE 不归一，其余一律归一，与格式无关。
    // 依据同 graphics/nvn/gfx/nvn/nvn_unnorm.inc（从游戏自带的 NVN 驱动核出来的）。整数格式的二维贴图照样用归一化坐标采样。
    out->normalized = nvn_target != NVN_TEXTURE_TARGET_RECTANGLE;
    return 0;
}

int s2a_nvn_texture_buffer_fact(uint32_t nvn_format, struct s2a_nvn_texture_fact *out,
                                char *why, size_t why_size) {
    // This fact-only entry point is not used by production request producers:
    // a BUFFER resource must not reach execution until its binding is supported.
    if (nvn_format == S2A_NVN_FORMAT_UNKNOWN || !format_name(nvn_format)) {
        setwhy(why, why_size, "buffer texture format is not mapped");
        return -1;
    }
    return s2a_nvn_texture_fact_compute(nvn_format, NVN_TEXTURE_TARGET_BUFFER, 1, out, why, why_size);
}
