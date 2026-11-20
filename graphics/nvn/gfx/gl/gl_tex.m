// #272 纹理 / 采样器。GL 存储行 0 在底；CPU 上传不翻行、不提交、不切遍。
// 首批（#360）：2D、RGBA8 / RGB8 / R8 / RG8 / RGBA16F、Storage / SubImage / 参数 / 采样器对象 / GenerateMipmap。
// 第二批：立方体（6 面 × 各级）、亮度 / alpha（存 R8 / RG8，采样时换位视图）、16 位打包与调色板索引（转 RGBA8）、浮点 / 10 位 / 深度格式、
//   S3TC / RGTC / BPTC / ASTC 压缩上传、帧缓冲附件（gl_tex_attach）、glTexParameterfv / glTexEnvi / glColorTable / glPixelTransferi。
// #272 第 6 项：压缩纹理子图 glCompressedTexSubImage2D（逐块局部上传，块对齐校验）；压缩存储上走 glTexSubImage2D 属规范错误、不计未实现。
// 在途上传换共享存储，已编码命令持有旧对象。各级（各面）独立存储，采样时按完整性组成 mip 链 / 立方体，组好后各级改指组合贴图的视图（不留两份）。
// 采样组链在开渲染编码器之前用 GPU blit，不提交、不等完成；GenerateMipmap 仍等生成完成后发布各级。
// 附件写入 / 组链（GPU 写）：登记 gpu_written；之后 CPU 上传先提交并等写完（GPU 内容最新，不能提前按 CPU 内容复制）。
// 体纹理（glTexImage3D / glTexSubImage3D）：GL_TEXTURE_2D_ARRAY 存 Metal 2D 数组、GL_TEXTURE_3D 存 Metal 3D，每级一张（level[0][l]），采样组链同 2D。
// 读回（glGetTexImage / glGetCompressedTexImage）：等 GPU 写完后取存储原始字节，按要的格式 / 类型与 GL_PACK_ALIGNMENT 写给客户端。
#include "gl_int.h"
#include "../tex/nvn_bcdec.h"   // nvn_bc_unpacked / nvn_bc_decode（GPU 不支持 BC 时解码，#749）
const char gl_slice_gl_tex = 1;   // 分片锚（gl_core.m 引用：静态链接时整片进来，见 gl_int.h「分片锚」）
#include <math.h>

enum {
    GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_CUBE_MAP = 0x8513, GL_TEXTURE_2D_ARRAY = 0x8C1A, GL_TEXTURE_3D = 0x806F, GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515, GL_TEXTURE0 = 0x84C0,
    GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_RED = 0x1903, GL_RG = 0x8227, GL_BGRA = 0x80E1, GL_ALPHA = 0x1906, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A,
    GL_COLOR_INDEX = 0x1900, GL_DEPTH_COMPONENT = 0x1902, GL_DEPTH_STENCIL = 0x84F9,
    GL_RGBA8 = 0x8058, GL_RGB8 = 0x8051, GL_R8 = 0x8229, GL_RG8 = 0x822B, GL_RGBA16F = 0x881A,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_HALF_FLOAT = 0x140B, GL_FLOAT = 0x1406,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
    GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368, GL_UNSIGNED_INT_10F_11F_11F_REV = 0x8C3B, GL_UNSIGNED_INT_24_8 = 0x84FA,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700,
    GL_LINEAR_MIPMAP_NEAREST = 0x2701, GL_NEAREST_MIPMAP_LINEAR = 0x2702, GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_REPEAT = 0x2901, GL_CLAMP = 0x2900, GL_CLAMP_TO_EDGE = 0x812F, GL_CLAMP_TO_BORDER = 0x812D, GL_MIRRORED_REPEAT = 0x8370,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_TEXTURE_WRAP_R = 0x8072,
    GL_TEXTURE_BASE_LEVEL = 0x813C, GL_TEXTURE_MAX_LEVEL = 0x813D, GL_TEXTURE_MIN_LOD = 0x813A, GL_TEXTURE_MAX_LOD = 0x813B,
    GL_TEXTURE_BORDER_COLOR = 0x1004, GL_TEXTURE_MAX_ANISOTROPY = 0x84FE, GL_TEXTURE_LOD_BIAS = 0x8501,
    GL_TEXTURE_COMPARE_MODE = 0x884C, GL_TEXTURE_COMPARE_FUNC = 0x884D,
    GL_NONE = 0, GL_COMPARE_REF_TO_TEXTURE = 0x884E,
    GL_NEVER = 0x0200, GL_LESS = 0x0201, GL_EQUAL = 0x0202, GL_LEQUAL = 0x0203,
    GL_GREATER = 0x0204, GL_NOTEQUAL = 0x0205, GL_GEQUAL = 0x0206, GL_ALWAYS = 0x0207,
    GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_PACK_ALIGNMENT = 0x0D05,
    GL_COLOR_TABLE = 0x80D0, GL_INDEX_SHIFT = 0x0D12, GL_INDEX_OFFSET = 0x0D13,
    GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501, GL_INVALID_OPERATION = 0x0502, GL_OUT_OF_MEMORY = 0x0505,
    TEX_LEVELS = 15
};
#define CTX(ret) do { if (!gl_cur) { GLRED1("%s：没有当前上下文", __func__); return ret; } } while (0)

// ---------- 格式 ----------
// kind：颜色 / 深度 / 深度模板 / 块压缩。ch / ct：存储的通道数与类型（0 = unorm8、1 = half、2 = float、3 = 其它，只收同格式字节）。
// swz：Metal 缺省读法与 GL 不同的才换位（亮度 / alpha；Metal 缺的通道读 0、alpha 读 1，同 GL）；换位放在采样视图上，存储本身可当渲染目标
enum { K_COL, K_DEPTH, K_DS, K_BLOCK };
typedef struct { MTLPixelFormat pf; uint8_t bytes, bw, kind, ch, ct, swizzled; MTLTextureSwizzleChannels swz; } fmt_t;
#define SWZ(r, g, b, a) {MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a}
#define SW0 SWZ(Red, Green, Blue, Alpha)
enum { F_RGBA8 = 1, F_SRGBA8, F_R8, F_RG8, F_L8, F_A8, F_LA8, F_RGBA16F, F_RGBA32F, F_R16F, F_RG16F, F_R32F, F_RG32F, F_RG11B10F, F_RGB10A2, F_RGBA16,
       F_D16, F_D32F, F_D32FS8, F_S8, F_BC1_RGB, F_BC1, F_BC2, F_BC3, F_BC1_SRGB, F_BC2_SRGB, F_BC3_SRGB, F_BC4, F_BC4S, F_BC5, F_BC5S, F_BC7, F_BC7_SRGB,
       F_BC6S, F_BC6U, F_ASTC4, F_ASTC4_SRGB, F_N };
static const fmt_t k_fmt[F_N] = {
    [F_RGBA8] = {MTLPixelFormatRGBA8Unorm, 4, 1, K_COL, 4, 0, 0, SW0}, [F_SRGBA8] = {MTLPixelFormatRGBA8Unorm_sRGB, 4, 1, K_COL, 4, 0, 0, SW0},
    [F_R8] = {MTLPixelFormatR8Unorm, 1, 1, K_COL, 1, 0, 0, SW0}, [F_RG8] = {MTLPixelFormatRG8Unorm, 2, 1, K_COL, 2, 0, 0, SW0},
    [F_L8] = {MTLPixelFormatR8Unorm, 1, 1, K_COL, 1, 0, 1, SWZ(Red, Red, Red, One)}, [F_A8] = {MTLPixelFormatR8Unorm, 1, 1, K_COL, 1, 0, 1, SWZ(Zero, Zero, Zero, Red)},
    [F_LA8] = {MTLPixelFormatRG8Unorm, 2, 1, K_COL, 2, 0, 1, SWZ(Red, Red, Red, Green)},
    [F_RGBA16F] = {MTLPixelFormatRGBA16Float, 8, 1, K_COL, 4, 1, 0, SW0}, [F_RGBA32F] = {MTLPixelFormatRGBA32Float, 16, 1, K_COL, 4, 2, 0, SW0},
    [F_R16F] = {MTLPixelFormatR16Float, 2, 1, K_COL, 1, 1, 0, SW0}, [F_RG16F] = {MTLPixelFormatRG16Float, 4, 1, K_COL, 2, 1, 0, SW0},
    [F_R32F] = {MTLPixelFormatR32Float, 4, 1, K_COL, 1, 2, 0, SW0}, [F_RG32F] = {MTLPixelFormatRG32Float, 8, 1, K_COL, 2, 2, 0, SW0},
    [F_RG11B10F] = {MTLPixelFormatRG11B10Float, 4, 1, K_COL, 3, 3, 0, SW0}, [F_RGB10A2] = {MTLPixelFormatRGB10A2Unorm, 4, 1, K_COL, 4, 3, 0, SW0},
    [F_RGBA16] = {MTLPixelFormatRGBA16Unorm, 8, 1, K_COL, 4, 3, 0, SW0},
    [F_D16] = {MTLPixelFormatDepth16Unorm, 2, 1, K_DEPTH, 1, 3, 0, SW0}, [F_D32F] = {MTLPixelFormatDepth32Float, 4, 1, K_DEPTH, 1, 2, 0, SW0},
    [F_D32FS8] = {MTLPixelFormatDepth32Float_Stencil8, 8, 1, K_DS, 2, 3, 0, SW0}, [F_S8] = {MTLPixelFormatStencil8, 1, 1, K_DS, 1, 3, 0, SW0},
    [F_BC1_RGB] = {MTLPixelFormatBC1_RGBA, 8, 4, K_BLOCK, 4, 3, 1, SWZ(Red, Green, Blue, One)}, [F_BC1] = {MTLPixelFormatBC1_RGBA, 8, 4, K_BLOCK, 4, 3, 0, SW0},
    [F_BC2] = {MTLPixelFormatBC2_RGBA, 16, 4, K_BLOCK, 4, 3, 0, SW0}, [F_BC3] = {MTLPixelFormatBC3_RGBA, 16, 4, K_BLOCK, 4, 3, 0, SW0},
    [F_BC1_SRGB] = {MTLPixelFormatBC1_RGBA_sRGB, 8, 4, K_BLOCK, 4, 3, 0, SW0}, [F_BC2_SRGB] = {MTLPixelFormatBC2_RGBA_sRGB, 16, 4, K_BLOCK, 4, 3, 0, SW0},
    [F_BC3_SRGB] = {MTLPixelFormatBC3_RGBA_sRGB, 16, 4, K_BLOCK, 4, 3, 0, SW0},
    [F_BC4] = {MTLPixelFormatBC4_RUnorm, 8, 4, K_BLOCK, 1, 3, 0, SW0}, [F_BC4S] = {MTLPixelFormatBC4_RSnorm, 8, 4, K_BLOCK, 1, 3, 0, SW0},
    [F_BC5] = {MTLPixelFormatBC5_RGUnorm, 16, 4, K_BLOCK, 2, 3, 0, SW0}, [F_BC5S] = {MTLPixelFormatBC5_RGSnorm, 16, 4, K_BLOCK, 2, 3, 0, SW0},
    [F_BC7] = {MTLPixelFormatBC7_RGBAUnorm, 16, 4, K_BLOCK, 4, 3, 0, SW0}, [F_BC7_SRGB] = {MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, 4, K_BLOCK, 4, 3, 0, SW0},
    [F_BC6S] = {MTLPixelFormatBC6H_RGBFloat, 16, 4, K_BLOCK, 3, 3, 0, SW0}, [F_BC6U] = {MTLPixelFormatBC6H_RGBUfloat, 16, 4, K_BLOCK, 3, 3, 0, SW0},
    [F_ASTC4] = {MTLPixelFormatASTC_4x4_LDR, 16, 4, K_BLOCK, 4, 3, 0, SW0}, [F_ASTC4_SRGB] = {MTLPixelFormatASTC_4x4_sRGB, 16, 4, K_BLOCK, 4, 3, 0, SW0},
};
// 内部格式 → 存储格式（0 = 不认）。无尺寸格式（GL_RGBA 之类）按给的类型取（同 GL 驱动：浮点 / 半精度源给浮点存储）；BGRA 存 RGBA8（上传时换序）
static int fmt_of(GLint internal, GLenum type) {
    int half = type == GL_HALF_FLOAT, fl = type == GL_FLOAT;
    switch (internal) {
        case GL_RGBA: case 4: return fl ? F_RGBA32F : half ? F_RGBA16F : F_RGBA8;
        case GL_RGB: case 3: return fl || half ? F_RGBA16F : F_RGBA8;
        case GL_BGRA: case GL_RGBA8: case GL_RGB8: return F_RGBA8;
        case 0x8C43: case 0x8C41: return F_SRGBA8;                               // SRGB8_ALPHA8 / SRGB8
        case GL_RED: return fl ? F_R32F : half ? F_R16F : F_R8;
        case GL_RG: return fl ? F_RG32F : half ? F_RG16F : F_RG8;
        case GL_R8: return F_R8; case GL_RG8: return F_RG8;
        case GL_LUMINANCE: case 1: case 0x8040: return F_L8;                     // LUMINANCE8
        case GL_ALPHA: case 0x803C: return F_A8;                                 // ALPHA8
        case GL_LUMINANCE_ALPHA: case 2: case 0x8045: return F_LA8;              // LUMINANCE8_ALPHA8
        case GL_RGBA16F: case 0x881B: return F_RGBA16F;                          // RGBA16F / RGB16F（alpha 读 1）
        case 0x8814: case 0x8815: return F_RGBA32F;                              // RGBA32F / RGB32F
        case 0x822D: return F_R16F; case 0x822F: return F_RG16F; case 0x822E: return F_R32F; case 0x8230: return F_RG32F;
        case 0x8C3A: return F_RG11B10F; case 0x8059: return F_RGB10A2; case 0x805B: return F_RGBA16;
        case 0x81A5: return F_D16;                                               // DEPTH_COMPONENT16
        case GL_DEPTH_COMPONENT: case 0x81A6: case 0x81A7: case 0x8CAC: return F_D32F;   // 24 位深度 Apple GPU 没有，存 32 位浮点
        case GL_DEPTH_STENCIL: case 0x88F0: case 0x8CAD: return F_D32FS8;
        case 0x8D48: return F_S8;
        case 0x83F0: return F_BC1_RGB; case 0x83F1: return F_BC1; case 0x83F2: return F_BC2; case 0x83F3: return F_BC3;
        case 0x8C4C: case 0x8C4D: return F_BC1_SRGB; case 0x8C4E: return F_BC2_SRGB; case 0x8C4F: return F_BC3_SRGB;
        case 0x8DBB: return F_BC4; case 0x8DBC: return F_BC4S; case 0x8DBD: return F_BC5; case 0x8DBE: return F_BC5S;
        case 0x8E8C: return F_BC7; case 0x8E8D: return F_BC7_SRGB; case 0x8E8E: return F_BC6S; case 0x8E8F: return F_BC6U;
        case 0x93B0: return F_ASTC4; case 0x93D0: return F_ASTC4_SRGB;
    }
    return 0;
}
static int is_rgb_internal(GLint i) { return i == GL_RGB || i == GL_RGB8 || i == 3 || i == 0x8C41 || i == 0x881B || i == 0x8815; }   // alpha 恒读 1 的 RGB 内部格式
static size_t row_bytes(int f, NSUInteger w) { const fmt_t *m = &k_fmt[f]; return (size_t)((w + m->bw - 1) / m->bw) * m->bytes; }
static size_t row_count(int f, NSUInteger h) { const fmt_t *m = &k_fmt[f]; return (size_t)((h + m->bw - 1) / m->bw); }
// GPU 不支持 BC 压缩格式（A15 / A16 及更早，MTLDevice.supportsBCTextureCompression = NO）：BC 纹理按解压后的格式建存储、上传时在 CPU 上解码（#749：iPad15,7 上
//   一款 GL 游戏建 BC3 纹理，Metal 断言「invalid pixelFormat (134)」中止进程）。同 NVN 支 nvn_mtl.m no_bc / fmt_unpack，解码与格式表共用 tex/nvn_bcdec.c（按 NVN 格式号）。
//   DM_TEST_NO_BC=1 任何机器上强制走这条（Mac 核对用，同 NVN 支）
static uint32_t bc_nvn(int f) {   // GL 存储格式 → NVN 格式号（nvn_bcdec.c 的表）；0 = 不是 BC
    switch (f) {
    case F_BC1_RGB: return 0x41; case F_BC1: return 0x42; case F_BC2: return 0x43; case F_BC3: return 0x44;
    case F_BC1_SRGB: return 0x46; case F_BC2_SRGB: return 0x47; case F_BC3_SRGB: return 0x48;
    case F_BC4: return 0x49; case F_BC4S: return 0x4a; case F_BC5: return 0x4b; case F_BC5S: return 0x4c;
    case F_BC7: return 0x4d; case F_BC7_SRGB: return 0x4e; case F_BC6S: return 0x4f; case F_BC6U: return 0x50;
    default: return 0;
    }
}
static int gl_no_bc(void) {
    static int v = -1;
    if (v < 0) { gl_dev_init(); const char *e = getenv("DM_TEST_NO_BC"); int forced = e && *e && strcmp(e, "0");
        v = forced || ![gl_dev supportsBCTextureCompression];
        if (v) GLOG("GPU 不支持 BC 压缩格式%s：GL 的 BC 纹理按解压格式建存储、上传时 CPU 解码", forced ? "（测试开关 DM_TEST_NO_BC）" : ""); }
    return v;
}
static int bc_unpacked(int f, uint32_t *pf, int *bpp) { uint32_t n = bc_nvn(f); return n && gl_no_bc() && nvn_bc_unpacked(n, pf, bpp); }   // 这个格式在本机按解压存
static MTLPixelFormat store_pf(int f) { uint32_t pf; int bpp; return bc_unpacked(f, &pf, &bpp) ? (MTLPixelFormat)pf : k_fmt[f].pf; }
static size_t store_row(int f, NSUInteger w) { uint32_t pf; int bpp; return bc_unpacked(f, &pf, &bpp) ? (size_t)w * (size_t)bpp : row_bytes(f, w); }   // 存储里一行的字节
static size_t store_rows(int f, NSUInteger h) { uint32_t pf; int bpp; return bc_unpacked(f, &pf, &bpp) ? (size_t)h : row_count(f, h); }
// 往存储写一块压缩数据（src：块紧排、每行 pitch 字节，覆盖 (x, y, w, h)）：本机按解压存就先解码
static int put_blocks(id<MTLTexture> img, int f, NSUInteger x, NSUInteger y, NSUInteger w, NSUInteger h, const void *src, size_t pitch) {
    uint32_t pf; int bpp;
    if (!bc_unpacked(f, &pf, &bpp)) { [img replaceRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0 withBytes:src bytesPerRow:pitch]; return 1; }
    size_t dp = w * (size_t)bpp; uint8_t *out = malloc(dp * h ? dp * h : 1); if (!out) { gl_set_error(GL_OUT_OF_MEMORY); return 0; }
    if (nvn_bc_decode(bc_nvn(f), src, pitch, (uint32_t)w, (uint32_t)h, out, dp)) { free(out); GLRED1("BC 解码失败（格式 %d）", f); gl_bad_feat++; return 0; }
    [img replaceRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0 withBytes:out bytesPerRow:dp]; free(out); return 1;
}
static size_t f_bytes(int f) { return f == F_D32FS8 ? 4 : k_fmt[f].bytes; }   // 读回时一个像素的原始字节（深度模板只取深度平面：4 字节浮点）

// ---------- 对象 ----------
typedef struct { GLenum min, mag, wrap[3]; GLfloat min_lod, max_lod, aniso, border[4]; GLenum compare_mode, compare_func; } tex_params;
typedef struct { __strong id<MTLTexture> image; GLenum format; int fmt; } tex_level;   // format：内部格式（完整性比对用）；fmt：存储格式
struct gl_texture {
    GLenum target; tex_level level[6][TEX_LEVELS];
    __strong id<MTLTexture> sample, view, view_src; __strong id<MTLSamplerState> sampler; int sample_base, sample_count;   // view：view_src（某次的采样贴图）的换位视图（要换位的格式才有）
    GLint base, max; int immutable, gpu_written; uint64_t last_use; tex_params params;
    uint8_t uswz[4];   // GL_TEXTURE_SWIZZLE_R / G / B / A：0 = 缺省（该通道取自己），否则 MTLTextureSwizzle + 1（采样视图上叠在格式换位之后，见 swizzled）
    gl_wd_shadow wd;   // 宽屏附件影子（gl_widen.inc）
};
struct gl_sampler { tex_params params; __strong id<MTLSamplerState> state; };
static gl_names textures, samplers;
static id<MTLTexture> g_black[4];
static id<MTLSamplerState> g_fallback_sampler;
static uint32_t g_palette[256]; static int g_npalette, g_index_shift, g_index_offset;   // glColorTable / glPixelTransferi（调色板纹理在上传时查表）
static tex_params defaults(void) { return (tex_params){GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR, {GL_REPEAT, GL_REPEAT, GL_REPEAT}, -1000, 1000, 1, {0, 0, 0, 0}, GL_NONE, GL_LEQUAL}; }
static gl_texture *new_texture(void) { gl_texture *t = calloc(1, sizeof *t); if (t) { t->params = defaults(); t->max = 1000; } return t; }
static void sample_drop(gl_texture *t) { t->sample = nil; t->view = nil; t->view_src = nil; }
static void sample_set(gl_texture *t, id<MTLTexture> image, int base, int n) {
    sample_drop(t); t->sample = image; t->sample_base = base; t->sample_count = n;
}
static void free_texture(gl_texture *t) {
    if (!t) return;
    gl_wd_drop(&t->wd, 0);
    for (int f = 0; f < 6; f++) for (int l = 0; l < TEX_LEVELS; l++) t->level[f][l].image = nil;
    t->sample = nil; t->view = t->view_src = nil; t->sampler = nil; free(t);
}
void gl_tex_context_drop(gl_state *st) {
    for (int slot = 0; slot < 4; slot++) { free_texture(st->default_tex[slot]); st->default_tex[slot] = NULL; }
}
static void texture_destroy(void *object) { free_texture(object); }
static void sampler_destroy(void *object) { gl_sampler *s = object; s->state = nil; free(s); }
void gl_tex_session_reset(void) {
    gl_names_clear(&textures, texture_destroy); gl_names_clear(&samplers, sampler_destroy);
    for (int i = 0; i < 4; i++) g_black[i] = nil;
    g_fallback_sampler = nil;
    memset(g_palette, 0, sizeof g_palette); g_npalette = g_index_shift = g_index_offset = 0;
}
// 名字0只在“按单元/目标访问当前绑定”的接口中解析为默认对象；FBO等按名字访问的接口仍保留0的解绑含义。
static const GLenum k_slot_target[4] = {GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D};
static gl_texture *unit_texture(GLuint unit, int slot, int create) {
    if (!gl_cur || unit >= 32 || slot < 0 || slot >= 4) return NULL;
    GLuint name = gl_cur->tex_unit[unit][slot];
    if (name) return gl_name_get(&textures, name);
    gl_texture *t = gl_cur->default_tex[slot];
    if (!t && create) {
        t = new_texture();
        if (!t) { gl_set_error(GL_OUT_OF_MEMORY); return NULL; }
        t->target = k_slot_target[slot];
        gl_cur->default_tex[slot] = t;
    }
    return t;
}
static void unsupported(const char *where, GLenum value) { GLRED1("%s 参数 %#x 不在已实现的纹理范围", where, value); gl_set_error(GL_INVALID_ENUM); gl_bad_feat++; }
int gl_tex_slot(GLenum target) { return target == GL_TEXTURE_2D ? 0 : target == GL_TEXTURE_CUBE_MAP ? 1 : target == GL_TEXTURE_2D_ARRAY ? 2 : target == GL_TEXTURE_3D ? 3 : -1; }
int gl_tex_sampler_slot(const char *t) {
    return !strcmp(t, "sampler2D") || !strcmp(t, "sampler2DShadow") ? 0 : !strcmp(t, "samplerCube") || !strcmp(t, "samplerCubeShadow") ? 1 :
           !strcmp(t, "sampler2DArray") || !strcmp(t, "sampler2DArrayShadow") ? 2 : !strcmp(t, "sampler3D") ? 3 : -1;
}
static int nfaces(const gl_texture *t) { return t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1; }
static int is_vol(const gl_texture *t) { return t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_3D; }
static NSUInteger vol_depth(id<MTLTexture> i) { return i.textureType == MTLTextureType3D ? i.depth : i.textureType == MTLTextureType2DArray ? i.arrayLength : 1; }   // 3D 的深度 / 数组的层数
static NSUInteger view_slices(id<MTLTexture> i) { return i.textureType == MTLTextureTypeCube ? 6 : i.textureType == MTLTextureType2DArray ? i.arrayLength : 1; }
// 图像目标（2D 或立方体某面）→ 绑定点序号与面号
static int image_target(GLenum target, int *face) {
    if (target == GL_TEXTURE_2D) { *face = 0; return 0; }
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6) { *face = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X); return 1; }
    return -1;
}
static gl_texture *bound_slot(int slot) {
    gl_texture *t = unit_texture(gl_cur->active_unit, slot, 1);
    if (!t && gl_cur->tex_unit[gl_cur->active_unit][slot]) gl_set_error(GL_INVALID_OPERATION);
    return t;
}
static gl_texture *bound(GLenum target) {
    int slot = gl_tex_slot(target); if (slot < 0) { unsupported("纹理目标", target); return NULL; }
    return bound_slot(slot);
}
static gl_texture *bound_image(GLenum target, int *face) {
    int slot = image_target(target, face); if (slot < 0) { unsupported("纹理图像目标", target); return NULL; }
    return bound_slot(slot);
}
static int mip_count(NSUInteger w, NSUInteger h) { int n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }
static int base_level(const gl_texture *t) { return t->immutable ? MIN(t->base, t->immutable - 1) : t->base; }
static int max_level(const gl_texture *t) { return t->immutable ? MAX(base_level(t), MIN(t->max, t->immutable - 1)) : t->max; }
static GLenum sized_format(GLenum f) { return f == GL_RGBA ? GL_RGBA8 : f == GL_RGB ? GL_RGB8 : f == GL_RED ? GL_R8 : f == GL_RG ? GL_RG8 : f; }
_Atomic uint64_t gl_n_tex_alloc;   // 贴图存储新建次数（egl.m 每 300 帧拆账打增量；11-01 街霸 Spare-host 每帧漏一张 384x224 定位用）
static id<MTLTexture> allocate(int f, NSUInteger w, NSUInteger h, NSUInteger levels, int cube) {
    gl_dev_init(); gl_n_tex_alloc++; const fmt_t *m = &k_fmt[f];
    MTLTextureDescriptor *d = cube ? [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:store_pf(f) size:w mipmapped:NO]
                                   : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:store_pf(f) width:w height:h mipmapped:NO];
    d.mipmapLevelCount = levels;
    // 深度 / 模板只能私有存储；压缩与要换位的格式不能当渲染目标（GL 也不能往里画）
    int ds = m->kind == K_DEPTH || m->kind == K_DS;
    d.storageMode = ds ? MTLStorageModePrivate : MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | (m->kind != K_BLOCK && !m->swizzled ? MTLTextureUsageRenderTarget : 0);
    id<MTLTexture> image = [gl_dev newTextureWithDescriptor:d];
    if (!image) { GLOG("【红】 纹理存储分配失败（%lux%lu，格式 %lu，%lu 级）", (unsigned long)w, (unsigned long)h, (unsigned long)m->pf, (unsigned long)levels); gl_set_error(GL_OUT_OF_MEMORY); }
    return image;
}
// 体纹理存储：2D 数组（arrayLength = 层数）或 3D（depth），用途规则同 allocate
static id<MTLTexture> allocate_vol(int f, NSUInteger w, NSUInteger h, NSUInteger d, NSUInteger levels, int is3d) {
    gl_dev_init(); gl_n_tex_alloc++; const fmt_t *m = &k_fmt[f];
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:store_pf(f) width:w height:h mipmapped:NO];
    desc.textureType = is3d ? MTLTextureType3D : MTLTextureType2DArray; if (is3d) desc.depth = d; else desc.arrayLength = d;
    desc.mipmapLevelCount = levels;
    int ds = m->kind == K_DEPTH || m->kind == K_DS;
    desc.storageMode = ds ? MTLStorageModePrivate : MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | (m->kind != K_BLOCK && !m->swizzled ? MTLTextureUsageRenderTarget : 0);
    id<MTLTexture> image = [gl_dev newTextureWithDescriptor:desc];
    if (!image) { GLOG("【红】 体纹理存储分配失败（%lux%lux%lu，格式 %lu，%lu 级，%s）", (unsigned long)w, (unsigned long)h, (unsigned long)d, (unsigned long)m->pf, (unsigned long)levels, is3d ? "3D" : "2D 数组"); gl_set_error(GL_OUT_OF_MEMORY); }
    return image;
}
static int cpu_ok(id<MTLTexture> t) { return t.storageMode != MTLStorageModePrivate; }
// GPU 写过的纹理：CPU 读写前先让 GPU 写完；采样组链不走这里。超时不许继续读旧内容 / 覆盖在途写。
static int gpu_settle(gl_texture *t) {
    if (!t->gpu_written) return 1;
    if (gl_seq_busy(t->last_use)) {
        if (t->last_use >= gl_seq_ref()) gl_commit();
        gl_wait(t->last_use);
        if (gl_ev.signaledValue < t->last_use) { gl_set_error(GL_INVALID_OPERATION); return 0; }
    }
    t->gpu_written = 0; return 1;
}
// CPU 内容当前有效：上传只用 CPU 写；GenerateMipmap 在返回前等生成完成。无编码器 / 提交 / 等待。
static int copy_level(id<MTLTexture> src, int f, id<MTLTexture> dst, NSUInteger level, NSUInteger slice) {
    if (!cpu_ok(src) || !cpu_ok(dst)) { GLRED1("私有存储（深度 / 模板）纹理拼链没实现"); gl_bad_feat++; return 0; }
    size_t row = store_row(f, src.width), n = row * store_rows(f, src.height);
    void *bytes = malloc(n ? n : 1); if (!bytes) { gl_set_error(GL_OUT_OF_MEMORY); return 0; }
    [src getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, src.width, src.height) mipmapLevel:0 slice:0];
    [dst replaceRegion:MTLRegionMake2D(0, 0, src.width, src.height) mipmapLevel:level slice:slice withBytes:bytes bytesPerRow:row bytesPerImage:0]; free(bytes); return 1;
}
// 体纹理整级复制（在途换存储用，CPU 内容有效）：逐层 / 逐片
static int copy_vol(id<MTLTexture> src, int f, id<MTLTexture> dst) {
    if (!cpu_ok(src) || !cpu_ok(dst)) { GLRED1("私有存储（深度 / 模板）体纹理换存储没实现"); gl_bad_feat++; return 0; }
    size_t row = store_row(f, src.width), img = row * store_rows(f, src.height); int is3d = src.textureType == MTLTextureType3D;
    void *bytes = malloc(img ? img : 1); if (!bytes) { gl_set_error(GL_OUT_OF_MEMORY); return 0; }
    for (NSUInteger i = 0; i < vol_depth(src); i++) {
        MTLRegion r = MTLRegionMake3D(0, 0, is3d ? i : 0, src.width, src.height, 1);
        [src getBytes:bytes bytesPerRow:row bytesPerImage:img fromRegion:r mipmapLevel:0 slice:is3d ? 0 : i];
        [dst replaceRegion:r mipmapLevel:0 slice:is3d ? 0 : i withBytes:bytes bytesPerRow:row bytesPerImage:img];
    }
    free(bytes); return 1;
}

// ---------- 上传 ----------
static int pixel_layout(GLenum format, GLenum type, int *channels, int *bytes) {
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) { *channels = 1; *bytes = 2; return 1; }   // 一个 16 位打包值一像素
    if (type == GL_UNSIGNED_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_10F_11F_11F_REV || type == GL_UNSIGNED_INT_24_8) { *channels = 1; *bytes = 4; return 1; }
    *channels = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB ? 3 : format == GL_RG || format == GL_LUMINANCE_ALPHA ? 2 :
                format == GL_RED || format == GL_ALPHA || format == GL_LUMINANCE || format == GL_COLOR_INDEX || format == GL_DEPTH_COMPONENT ? 1 : format == GL_DEPTH_STENCIL ? 1 : 0;
    *bytes = type == GL_UNSIGNED_BYTE ? 1 : type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT ? 2 : type == GL_FLOAT || type == GL_UNSIGNED_INT ? 4 : 0;
    if (!*channels || !*bytes) { unsupported("像素格式 / 类型", !*channels ? format : type); return 0; } return 1;
}
// 源像素 → 存储字节。快路径：源每像素字节与存储一致且无需换算（同通道数的 unorm8 / half / float、同格式打包值）整行拷贝；
// 其余逐像素经浮点换算（首批的通用路径）：16 位打包、BGRA 换序、调色板索引、RGB 内部格式 alpha 置 1
// slice：数组层 / 立方体面；z：3D 纹理的片（其它为 0）
static int upload(id<MTLTexture> dst, int f, NSUInteger slice, NSUInteger z, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data, int rgb) {
    int nc, nb; if (!pixel_layout(format, type, &nc, &nb)) return 0;
    if (!data || !w || !h) return 1;
    const fmt_t *m = &k_fmt[f];
    if (!cpu_ok(dst)) { GLRED1("往私有存储（深度 / 模板）纹理上传数据没实现（丢弃）"); gl_bad_feat++; return 1; }
    size_t length = gl_cur->unpack_row ? (size_t)gl_cur->unpack_row : (size_t)w;
    size_t alignment = gl_cur->unpack_align ? (size_t)gl_cur->unpack_align : 4;
    if (length > (SIZE_MAX - alignment) / (size_t)(nc * nb)) { gl_set_error(GL_INVALID_VALUE); return 0; }
    size_t stride = (length * nc * nb + alignment - 1) & ~(alignment - 1), outrow = (size_t)w * m->bytes;
    if (h > 1 && stride > (SIZE_MAX - (size_t)w * nc * nb) / (size_t)(h - 1)) { gl_set_error(GL_INVALID_VALUE); return 0; }
    int packed16 = type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1;
    int same = !rgb && format != GL_BGRA && format != GL_COLOR_INDEX && !packed16 && (size_t)(nc * nb) == m->bytes &&
               ((m->ct == 0 && type == GL_UNSIGNED_BYTE) || (m->ct == 1 && type == GL_HALF_FLOAT) || (m->ct == 2 && type == GL_FLOAT) ||
                (f == F_RG11B10F && type == GL_UNSIGNED_INT_10F_11F_11F_REV) || (f == F_RGB10A2 && type == GL_UNSIGNED_INT_2_10_10_10_REV) ||
                (f == F_RGBA16 && type == GL_UNSIGNED_SHORT));
    MTLRegion region = MTLRegionMake3D((NSUInteger)x, (NSUInteger)y, z, (NSUInteger)w, (NSUInteger)h, 1); int is3d = dst.textureType == MTLTextureType3D;
    if (same) {   // 行步长直接给 Metal，不经中转缓冲
        [dst replaceRegion:region mipmapLevel:0 slice:slice withBytes:data bytesPerRow:stride bytesPerImage:is3d ? stride * (size_t)h : 0]; return 1;
    }
    if (m->ct == 3) { GLRED1("存储格式 %lu 收类型 %#x 的数据没实现（丢弃）", (unsigned long)m->pf, type); gl_bad_feat++; return 1; }
    if (format == GL_COLOR_INDEX && type != GL_UNSIGNED_BYTE) { unsupported("调色板索引类型", type); return 0; }
    uint8_t *out = malloc(outrow * (size_t)h); if (!out) { gl_set_error(GL_OUT_OF_MEMORY); return 0; }
    int dc = m->ch;
    for (GLsizei row = 0; row < h; row++) for (GLsizei col = 0; col < w; col++) {
        const uint8_t *in = (const uint8_t *)data + (size_t)row * stride + (size_t)col * nc * nb;
        float v[4] = {0,0,0,1};
        if (packed16) {
            uint16_t p; memcpy(&p, in, 2);
            if (type == GL_UNSIGNED_SHORT_5_6_5) { v[0] = (p >> 11) / 31.f; v[1] = (p >> 5 & 63) / 63.f; v[2] = (p & 31) / 31.f; }
            else if (type == GL_UNSIGNED_SHORT_4_4_4_4) { v[0] = (p >> 12) / 15.f; v[1] = (p >> 8 & 15) / 15.f; v[2] = (p >> 4 & 15) / 15.f; v[3] = (p & 15) / 15.f; }
            else { v[0] = (p >> 11) / 31.f; v[1] = (p >> 6 & 31) / 31.f; v[2] = (p >> 1 & 31) / 31.f; v[3] = p & 1; }
        } else if (format == GL_COLOR_INDEX) {
            // 索引 << shift + offset 查表（GL 兼容像素传送）；没设调色板按灰度
            int i = ((int)in[0] << g_index_shift) + g_index_offset; uint32_t c = g_npalette ? g_palette[i < 0 ? 0 : i >= g_npalette ? g_npalette - 1 : i] : 0xff000000u | in[0] * 0x10101u;
            for (int k = 0; k < 4; k++) v[k] = (c >> (8 * k) & 0xff) / 255.0f;
            if (!(gl_cur->enable & GLE_COLOR_TABLE)) GLRED1("调色板索引纹理上传时 GL_COLOR_TABLE 没开（照样查表）");
        } else {
            for (int k = 0; k < nc; k++) {
                if (type == GL_UNSIGNED_BYTE) v[k] = in[k] / 255.0f;
                else if (type == GL_HALF_FLOAT) { _Float16 a; memcpy(&a, in + 2 * k, 2); v[k] = a; }
                else if (type == GL_UNSIGNED_SHORT) { uint16_t a; memcpy(&a, in + 2 * k, 2); v[k] = a / 65535.0f; }
                else if (type == GL_UNSIGNED_INT) { uint32_t a; memcpy(&a, in + 4 * k, 4); v[k] = (float)(a / 4294967295.0); }
                else memcpy(&v[k], in + 4 * k, 4);
            }
            if (format == GL_BGRA) { float b = v[0]; v[0] = v[2]; v[2] = b; }
            if (format == GL_LUMINANCE_ALPHA && dc == 4) { v[3] = v[1]; v[1] = v[2] = v[0]; }   // 亮度 / alpha 源进 RGBA 存储（GL 的像素传送：L → RGB）
            else if (format == GL_LUMINANCE && dc >= 3) v[1] = v[2] = v[0];
            else if (format == GL_ALPHA && dc == 4) { v[3] = v[0]; v[0] = 0; }
        }
        // RGB 内部格式的 alpha 始终为 1（即使外部格式为 RGBA）。
        if (dc == 4 && rgb) v[3] = 1;
        uint8_t *o = out + (size_t)row * outrow + (size_t)col * m->bytes;
        for (int k = 0; k < dc; k++) {
            if (m->ct == 1) { _Float16 a = v[k]; memcpy(o + 2 * k, &a, 2); }
            else if (m->ct == 2) memcpy(o + 4 * k, &v[k], 4);
            else o[k] = (uint8_t)lrintf(fminf(1, fmaxf(0, v[k])) * 255);
        }
    }
    [dst replaceRegion:region mipmapLevel:0 slice:slice withBytes:out bytesPerRow:outrow bytesPerImage:is3d ? outrow * (size_t)h : 0]; free(out); return 1;
}
GL(glGenTextures, void, GLsizei n, GLuint *out) {
    CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) { gl_texture *t = new_texture(); if (!t) { gl_set_error(GL_OUT_OF_MEMORY); return; } out[i] = gl_name_new(&textures, t); }
}
GLB(glDeleteTextures, void, GLsizei n, const GLuint *names) { @autoreleasepool {
    CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) { gl_texture *t = gl_name_del(&textures, names[i]); if (!t) continue;
        for (int u = 0; u < 32; u++) for (int s = 0; s < 4; s++) if (gl_cur->tex_unit[u][s] == names[i]) gl_cur->tex_unit[u][s] = 0;
        free_texture(t);
    }
}}
GL(glActiveTexture, void, GLenum unit) { CTX(); if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + 32) { gl_set_error(GL_INVALID_ENUM); return; } gl_cur->active_unit = unit - GL_TEXTURE0; }
GL(glBindTexture, void, GLenum target, GLuint name) {
    CTX(); int slot = gl_tex_slot(target); if (slot < 0) { unsupported("glBindTexture", target); return; }
    gl_texture *t = gl_name_get(&textures, name);
    if (name && !t) { t = new_texture(); if (!t) { gl_set_error(GL_OUT_OF_MEMORY); return; } if (!gl_name_put(&textures, name, t)) { free(t); gl_set_error(GL_OUT_OF_MEMORY); return; } }
    if (t && !t->target) t->target = target;   // 第一次绑定定下目标（规范）
    else if (t && t->target != target) { gl_set_error(GL_INVALID_OPERATION); return; }
    gl_cur->tex_unit[gl_cur->active_unit][slot] = name;
}
// 像素解包缓冲（GL 4.5 §8.4.4.1 / ES 3.0 §3.7.1）：绑着它时上传入口的 data 是缓冲内偏移，像素从缓冲取（NULL = 偏移 0）。
//   缓冲没建存储、映射着（持久映射除外）、偏移 + need 超出大小 → INVALID_OPERATION（规范），不上传。没绑：data 原样是客户端内存
static size_t unpack_image(GLenum format, GLenum type, GLsizei w, GLsizei h);
static int unpack_src(const void **data, size_t need) {
    if (!gl_unpack_buffer_name()) return 1;
    struct gl_buffer *b = gl_unpack_buffer(); size_t off = (size_t)(uintptr_t)*data;
    if (!b || !b->buf || (b->map_access && !(b->map_bits & 0x40)) || off > b->size || need > b->size - off) { gl_set_error(GL_INVALID_OPERATION); return 0; }
    *data = (const uint8_t *)b->buf.contents + off; return 1;
}
static int define(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, int compressed, GLsizei size, GLenum format, GLenum type, const void *data) {
    int face; gl_texture *t = bound_image(target, &face); if (!t) return 0;
    if (t->immutable) { gl_set_error(GL_INVALID_OPERATION); return 0; }
    if (level < 0 || level >= TEX_LEVELS || w < 0 || h < 0 || w > (16384 >> level) || h > (16384 >> level) || border) { gl_set_error(GL_INVALID_VALUE); return 0; }
    if (t->target == GL_TEXTURE_CUBE_MAP && w != h) { gl_set_error(GL_INVALID_VALUE); return 0; }
    int f = fmt_of(internal, type), nc, nb;
    if (!f) { unsupported(compressed ? "glCompressedTexImage2D internalformat" : "glTexImage2D internalformat", internal); return 0; }
    if (compressed) {
        if (k_fmt[f].kind != K_BLOCK) { gl_set_error(GL_INVALID_ENUM); return 0; }
        if (size < 0 || (size_t)size != row_bytes(f, (NSUInteger)w) * row_count(f, (NSUInteger)h)) { gl_set_error(GL_INVALID_VALUE); return 0; }
    } else if (k_fmt[f].kind == K_BLOCK) { GLRED1("glTexImage2D 往压缩格式 %#x 传未压缩数据没实现", internal); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return 0; }
    else if (!pixel_layout(format, type, &nc, &nb)) return 0;
    if (!unpack_src(&data, compressed ? (size_t)size : unpack_image(format, type, w, h))) return 0;
    if (!gpu_settle(t)) return 0;
    if (!face && !level) { gl_wd_drop(&t->wd, 0); t->wd.cpu = data != NULL; }   // 宽屏：第 0 级换存储，影子作废
    if (!w || !h) { t->level[face][level].image = nil; sample_drop(t); return 1; }
    id<MTLTexture> image = allocate(f, (NSUInteger)w, (NSUInteger)h, 1, 0); if (!image) return 0;
    if (compressed) { if (data && !put_blocks(image, f, 0, 0, (NSUInteger)w, (NSUInteger)h, data, row_bytes(f, (NSUInteger)w))) return 0; }
    else if (!upload(image, f, 0, 0, 0, 0, w, h, format, type, data, is_rgb_internal(internal))) return 0;
    t->level[face][level].image = image; t->level[face][level].format = sized_format((GLenum)internal); t->level[face][level].fmt = f; sample_drop(t);
    return 1;
}
GLB(glTexImage2D, void, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *data) { @autoreleasepool {
    CTX(); define(target, level, internal, w, h, border, 0, 0, format, type, data);
}}
GLB(glCompressedTexImage2D, void, GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data) { @autoreleasepool {
    CTX(); define(target, level, (GLint)internal, w, h, border, 1, size, 0, 0, data);
}}
GLB(glTexStorage2D, void, GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h) { @autoreleasepool {
    CTX(); int slot = gl_tex_slot(target); if (slot < 0) { unsupported("纹理目标", target); return; }
    if (slot >= 2) { gl_set_error(GL_INVALID_ENUM); return; }   // 2D 数组 / 3D 不是 TexStorage2D 的目标（规范）
    // GL4.5 §8.19 / GLES3.2 §8.18：不可变存储不能用于默认对象，即使它已经定义了图像。
    if (!gl_cur->tex_unit[gl_cur->active_unit][slot]) { gl_set_error(GL_INVALID_OPERATION); return; }
    gl_texture *t = bound_slot(slot); if (!t) return;
    if (t->immutable) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (w < 1 || h < 1 || w > 16384 || h > 16384 || levels < 1 || levels > mip_count(w, h)) { gl_set_error(GL_INVALID_VALUE); return; }
    int f = fmt_of(internal, 0);
    if (internal == GL_RGBA || internal == GL_RGB || internal == GL_RED || internal == GL_RG || !f) { unsupported("glTexStorage2D internalformat", internal); return; }
    int cube = t->target == GL_TEXTURE_CUBE_MAP; if (cube && w != h) { gl_set_error(GL_INVALID_VALUE); return; }
    id<MTLTexture> chain = allocate(f, w, h, levels, cube); if (!chain) return;
    id<MTLTexture> views[6][TEX_LEVELS] = {{nil}};
    for (int s = 0; s < nfaces(t); s++) for (int l = 0; l < levels; l++) {
        views[s][l] = [chain newTextureViewWithPixelFormat:chain.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(l, 1) slices:NSMakeRange(s, 1)];
        if (!views[s][l]) { gl_set_error(GL_OUT_OF_MEMORY); return; }
    }
    gl_wd_drop(&t->wd, 0); t->wd.cpu = 0;
    for (int s = 0; s < nfaces(t); s++) for (int l = 0; l < TEX_LEVELS; l++) {
        t->level[s][l].image = views[s][l];
        t->level[s][l].format = internal; t->level[s][l].fmt = f;
    }
    sample_set(t, chain, 0, levels); t->immutable = levels; t->gpu_written = 0; t->last_use = 0;
}}
GLB(glTexSubImage2D, void, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data) { @autoreleasepool {
    CTX(); int face; gl_texture *t = bound_image(target, &face); if (!t) return;
    if (level < 0 || level >= TEX_LEVELS) { gl_set_error(GL_INVALID_VALUE); return; }
    tex_level *lv = &t->level[face][level]; id<MTLTexture> old = lv->image; if (!old) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (x < 0 || y < 0 || w < 0 || h < 0 || (NSUInteger)x > old.width || (NSUInteger)w > old.width - (NSUInteger)x || (NSUInteger)y > old.height || (NSUInteger)h > old.height - (NSUInteger)y) { gl_set_error(GL_INVALID_VALUE); return; }
    // 压缩存储不接受未压缩来源：这是规范规定的错（GL_INVALID_OPERATION），不是本层缺实现，所以不计未实现、不报红
    if (k_fmt[lv->fmt].kind == K_BLOCK) { gl_set_error(GL_INVALID_OPERATION); return; }
    int nc, nb; if (!pixel_layout(format, type, &nc, &nb)) return; if (!w || !h) return;
    if (!unpack_src(&data, unpack_image(format, type, w, h))) return;
    if (!data) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!gpu_settle(t)) return;
    id<MTLTexture> image = old;
    if (gl_seq_busy(t->last_use)) {   // 在途：换存储，已编码的绘制读旧的
        image = allocate(lv->fmt, old.width, old.height, 1, 0); if (!image || !copy_level(old, lv->fmt, image, 0, 0)) return;
        sample_drop(t);
    }
    if (!upload(image, lv->fmt, 0, 0, x, y, w, h, format, type, data, lv->format == GL_RGB || lv->format == GL_RGB8)) return;
    lv->image = image;
    if (!face && !level) { gl_wd_drop(&t->wd, 1); t->wd.cpu = 1; }   // 宽屏：CPU 改了第 0 级，影子作废（下次当附件重建时拷进原图内容）
    if (image != old) sample_drop(t);
    else if (!(t->sample && image.parentTexture == t->sample)) sample_drop(t);   // 子范围视图可从共同 parent 重取；改指组合贴图的视图原地写即生效
}}
// 压缩纹理子图（#272 第 6 项）。规范语义：区域必须按块对齐——xoffset / yoffset 是该级块边长的整数倍；
// width / height 也必须是整数倍，唯一例外是区域贴到该级的右 / 下边界（末块按整块覆盖，数据仍按整块给）。
// 数据长度 = 覆盖区域的块数 × 每块字节（压缩图像不看 UNPACK_ROW_LENGTH / UNPACK_ALIGNMENT，这两项只管未压缩来源）。
GLB(glCompressedTexSubImage2D, void, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei size, const void *data) { @autoreleasepool {
    CTX(); int face; gl_texture *t = bound_image(target, &face); if (!t) return;
    if (level < 0 || level >= TEX_LEVELS) { gl_set_error(GL_INVALID_VALUE); return; }
    tex_level *lv = &t->level[face][level]; id<MTLTexture> old = lv->image;
    if (!old) { gl_set_error(GL_INVALID_OPERATION); return; }                        // 这一级还没定义
    const fmt_t *m = &k_fmt[lv->fmt];
    if (m->kind != K_BLOCK) { gl_set_error(GL_INVALID_OPERATION); return; }           // 目标不是压缩存储（规范：INVALID_OPERATION）
    int f = fmt_of((GLint)format, 0);
    if (!f || k_fmt[f].kind != K_BLOCK) { gl_set_error(GL_INVALID_ENUM); return; }    // 来源格式不是压缩格式
    if (f != lv->fmt) { gl_set_error(GL_INVALID_OPERATION); return; }                 // 与该级存储的压缩格式不符
    if (x < 0 || y < 0 || w < 0 || h < 0 || (NSUInteger)x > old.width || (NSUInteger)w > old.width - (NSUInteger)x ||
        (NSUInteger)y > old.height || (NSUInteger)h > old.height - (NSUInteger)y) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!w || !h) { if (size) gl_set_error(GL_INVALID_VALUE); return; }                // 空区域：不传数据（长度必须为 0）
    int bw = m->bw ? m->bw : 1;                                                       // 本层块格式一律 4×4
    int edge_x = (NSUInteger)(x + w) == old.width, edge_y = (NSUInteger)(y + h) == old.height;
    if (x % bw || y % bw || (w % bw && !edge_x) || (h % bw && !edge_y)) { gl_set_error(GL_INVALID_VALUE); return; }
    size_t blocks_x = (size_t)((x + w + bw - 1) / bw - x / bw), blocks_y = (size_t)((y + h + bw - 1) / bw - y / bw), row = blocks_x * m->bytes;
    if (size >= 0 && !unpack_src(&data, (size_t)size)) return;
    if (!data || size < 0 || (size_t)size != row * blocks_y) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!gpu_settle(t)) return;
    id<MTLTexture> image = old;
    if (gl_seq_busy(t->last_use)) {   // 在途：换存储，已编码的绘制读旧的（/ 复制整级含全部块）
        image = allocate(lv->fmt, old.width, old.height, 1, 0); if (!image || !copy_level(old, lv->fmt, image, 0, 0)) return;
        sample_drop(t);
    }
    if (!put_blocks(image, lv->fmt, (NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h, data, row)) return;
    lv->image = image;
    if (!face && !level) { gl_wd_drop(&t->wd, 1); t->wd.cpu = 1; }
    if (image != old) sample_drop(t);
    else if (!(t->sample && image.parentTexture == t->sample)) sample_drop(t);
}}
static MTLSamplerAddressMode address(GLenum wrap) {
    return wrap == GL_REPEAT ? MTLSamplerAddressModeRepeat : wrap == GL_MIRRORED_REPEAT ? MTLSamplerAddressModeMirrorRepeat :
           wrap == GL_CLAMP_TO_BORDER ? MTLSamplerAddressModeClampToBorderColor : MTLSamplerAddressModeClampToEdge;
}
static int parameter(tex_params *p, GLenum what, double value, int sampler, gl_texture *t) {
    if (!isfinite(value)) { gl_set_error(GL_INVALID_VALUE); return 0; }
    if (what == GL_TEXTURE_COMPARE_MODE) {
        if (value != GL_NONE && value != GL_COMPARE_REF_TO_TEXTURE) { gl_set_error(GL_INVALID_ENUM); return 0; }
        p->compare_mode = (GLenum)value; return 1;
    }
    if (what == GL_TEXTURE_COMPARE_FUNC) {
        if (value < GL_NEVER || value > GL_ALWAYS || value != round(value)) { gl_set_error(GL_INVALID_ENUM); return 0; }
        p->compare_func = (GLenum)value; return 1;
    }
    if (what == GL_TEXTURE_MIN_LOD) { p->min_lod = (GLfloat)value; return 1; }
    if (what == GL_TEXTURE_MAX_LOD) { p->max_lod = (GLfloat)value; return 1; }
    if (what == GL_TEXTURE_MAX_ANISOTROPY) { if (value < 1) { gl_set_error(GL_INVALID_VALUE); return 0; } p->aniso = (GLfloat)fmin(value, 16); return 1; }
    if (what == GL_TEXTURE_LOD_BIAS) { if (value) GLRED1("GL_TEXTURE_LOD_BIAS %g：Metal 采样器没有偏移（忽略）", value); return 1; }
    if (value > INT32_MAX || value < INT32_MIN) { gl_set_error(GL_INVALID_VALUE); return 0; }
    GLint v = (GLint)round(value);
    switch (what) {
    case GL_TEXTURE_MIN_FILTER: if (value != v || (v != GL_NEAREST && v != GL_LINEAR && (v < GL_NEAREST_MIPMAP_NEAREST || v > GL_LINEAR_MIPMAP_LINEAR))) break; p->min = v; return 1;
    case GL_TEXTURE_MAG_FILTER: if (value != v || (v != GL_NEAREST && v != GL_LINEAR)) break; p->mag = v; return 1;
    case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: case GL_TEXTURE_WRAP_R:
        if (value != v || (v != GL_REPEAT && v != GL_CLAMP_TO_EDGE && v != GL_MIRRORED_REPEAT && v != GL_CLAMP_TO_BORDER && v != GL_CLAMP)) break;
        p->wrap[what == GL_TEXTURE_WRAP_S ? 0 : what == GL_TEXTURE_WRAP_T ? 1 : 2] = v == GL_CLAMP ? GL_CLAMP_TO_EDGE : v; return 1;   // 旧 GL_CLAMP 按夹到边
    case 0x8E42: case 0x8E43: case 0x8E44: case 0x8E45: {   // GL_TEXTURE_SWIZZLE_R / G / B / A（GL 3.3 / ES 3.0；纹理对象的状态，采样器对象没有）
        //   小丑牌 / LÖVE 11 建单 / 双通道格式时设它；以前报 INVALID_ENUM，LÖVE 建纹理后查 glGetError 抛异常、游戏退出
        if (sampler || value != v) break;
        int c = (int)what - 0x8E42, m = v == 0x1903 ? MTLTextureSwizzleRed : v == 0x1904 ? MTLTextureSwizzleGreen : v == 0x1905 ? MTLTextureSwizzleBlue :
                v == 0x1906 ? MTLTextureSwizzleAlpha : v == 0 ? MTLTextureSwizzleZero : v == 1 ? MTLTextureSwizzleOne : -1;
        if (m < 0) break;
        uint8_t u = m == MTLTextureSwizzleRed + c ? 0 : (uint8_t)(m + 1);   // 取自己 = 缺省，存 0：全缺省时不建换位视图
        if (t->uswz[c] != u) { t->uswz[c] = u; t->view = nil; t->view_src = nil; }
        return 1;
    }
    case GL_TEXTURE_BASE_LEVEL: case GL_TEXTURE_MAX_LEVEL:
        if (sampler) break; if (v < 0) { gl_set_error(GL_INVALID_VALUE); return 0; }
        if (what == GL_TEXTURE_BASE_LEVEL) { if (t->base != v) sample_drop(t); t->base = v; }
        else { if (t->max != v) sample_drop(t); t->max = v; } return 1;
    default: break;
    } unsupported("纹理 / 采样器参数", what); return 0;
}
// 贴图参数查询：过滤 / 环绕 / 比较、基础与最大级、不可变格式与级数；其它参数报红
GL(glGetTexParameteriv, void, GLenum target, GLenum what, GLint *v) {
    CTX(); gl_texture *t = bound(target); if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }
    const tex_params *q = &t->params;
    switch (what) {
        case 0x2801: *v = (GLint)q->min; return;  case 0x2800: *v = (GLint)q->mag; return;                    // MIN / MAG_FILTER
        case 0x8E42: case 0x8E43: case 0x8E44: case 0x8E45: { int c = (int)what - 0x8E42, u = t->uswz[c] ? t->uswz[c] - 1 : MTLTextureSwizzleRed + c;   // SWIZZLE_R / G / B / A
            *v = u == MTLTextureSwizzleZero ? 0 : u == MTLTextureSwizzleOne ? 1 : 0x1903 + (u - MTLTextureSwizzleRed); return; }
        case 0x2802: *v = (GLint)q->wrap[0]; return; case 0x2803: *v = (GLint)q->wrap[1]; return; case 0x8072: *v = (GLint)q->wrap[2]; return;   // WRAP_S / T / R
        case GL_TEXTURE_COMPARE_MODE: *v = (GLint)q->compare_mode; return; case GL_TEXTURE_COMPARE_FUNC: *v = (GLint)q->compare_func; return;
        case 0x813C: *v = t->base; return; case 0x813D: *v = t->max; return;                                  // BASE / MAX_LEVEL
        case 0x912F: *v = t->immutable ? 1 : 0; return; case 0x82DF: *v = t->immutable; return;               // IMMUTABLE_FORMAT / IMMUTABLE_LEVELS
    }
    GLRED1("glGetTexParameteriv 参数 %#x 没实现", what); gl_set_error(GL_INVALID_ENUM);
}
GLB(glTexParameteri, void, GLenum target, GLenum what, GLint value) { @autoreleasepool { CTX(); gl_texture *t = bound(target); if (t && parameter(&t->params, what, value, 0, t)) t->sampler = nil; }}
GLB(glTexParameterf, void, GLenum target, GLenum what, GLfloat value) { @autoreleasepool { CTX(); gl_texture *t = bound(target); if (t && parameter(&t->params, what, value, 0, t)) t->sampler = nil; }}
GLB(glTexParameterfv, void, GLenum target, GLenum what, const GLfloat *v) { @autoreleasepool {
    CTX(); gl_texture *t = bound(target); if (!t) return;
    if (what == GL_TEXTURE_BORDER_COLOR) { memcpy(t->params.border, v, sizeof t->params.border); t->sampler = nil; return; }
    if (parameter(&t->params, what, v[0], 0, t)) t->sampler = nil;
}}
GL(glTexEnvi, void, GLenum target, GLenum what, GLint value) { (void)target; (void)what; (void)value; }   // 固定管线纹理组合：着色器程序取代，无作用（规范）
GL(glPixelStorei, void, GLenum what, GLint value) {
    CTX(); if (what == GL_UNPACK_ALIGNMENT) { if (value != 1 && value != 2 && value != 4 && value != 8) { gl_set_error(GL_INVALID_VALUE); return; } gl_cur->unpack_align = value; }
    else if (what == GL_UNPACK_ROW_LENGTH) { if (value < 0) { gl_set_error(GL_INVALID_VALUE); return; } gl_cur->unpack_row = value; }
    else if (what == GL_PACK_ALIGNMENT) { if (value != 1 && value != 2 && value != 4 && value != 8) { gl_set_error(GL_INVALID_VALUE); return; } gl_cur->pack_align = value; }   // glGetTexImage 读回的行对齐
    else if (what >= 0x0CF0 && what <= 0x0CF4 && !value) {}   // UNPACK_SWAP_BYTES / LSB_FIRST / SKIP_ROWS / SKIP_PIXELS 取缺省 0
    else unsupported("glPixelStorei", what);
}
GL(glPixelTransferi, void, GLenum what, GLint value) {
    CTX(); if (what == GL_INDEX_SHIFT) g_index_shift = value; else if (what == GL_INDEX_OFFSET) g_index_offset = value; else unsupported("glPixelTransferi", what);
}
GL(glColorTable, void, GLenum target, GLenum internal, GLsizei w, GLenum format, GLenum type, const void *table) {
    CTX(); (void)internal;
    if (target != GL_COLOR_TABLE || type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_RGB) || w <= 0 || !table) { GLRED1("glColorTable(%#x, %d, %#x, %#x) 没实现", target, w, format, type); gl_bad_feat++; return; }
    g_npalette = w > 256 ? 256 : w; const uint8_t *s = table;
    for (int i = 0; i < g_npalette; i++) g_palette[i] = format == GL_RGBA ? (uint32_t)s[4 * i] | s[4 * i + 1] << 8 | s[4 * i + 2] << 16 | (uint32_t)s[4 * i + 3] << 24
                                                                       : (uint32_t)s[3 * i] | s[3 * i + 1] << 8 | s[3 * i + 2] << 16 | 0xff000000u;
}
GL(glGenSamplers, void, GLsizei n, GLuint *out) { CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; } for (int i = 0; i < n; i++) { gl_sampler *s = calloc(1, sizeof *s); if (!s) { gl_set_error(GL_OUT_OF_MEMORY); return; } s->params = defaults(); out[i] = gl_name_new(&samplers, s); } }
GL(glBindSampler, void, GLuint unit, GLuint name) { CTX(); if (unit >= 32) { gl_set_error(GL_INVALID_VALUE); return; } if (name && !gl_name_get(&samplers, name)) { gl_set_error(GL_INVALID_OPERATION); return; } gl_cur->smp_unit[unit] = name; }
GLB(glSamplerParameteri, void, GLuint name, GLenum what, GLint value) { @autoreleasepool { CTX(); gl_sampler *s = gl_name_get(&samplers, name); if (!s) { gl_set_error(GL_INVALID_OPERATION); return; } if (parameter(&s->params, what, value, 1, NULL)) s->state = nil; }}
GLB(glDeleteSamplers, void, GLsizei n, const GLuint *names) { @autoreleasepool { CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; } for (int i = 0; i < n; i++) { gl_sampler *s = gl_name_del(&samplers, names[i]); if (!s) continue; for (int u = 0; u < 32; u++) if (gl_cur->smp_unit[u] == names[i]) gl_cur->smp_unit[u] = 0; s->state = nil; free(s); } }}

// ---------- 采样 ----------
// 返回有效 mip 个数；不完整并非未实现，采样按黑色处理。max 可把链截断，immutable 的实际级数也限制链。立方体六面同尺寸同格式才完整
static int complete(gl_texture *t, const tex_params *p) {
    if (!t) return 0;
    int base = base_level(t), max = max_level(t);
    if (base < 0 || base >= TEX_LEVELS || max < base) return 0;
    id<MTLTexture> b = t->level[0][base].image; if (!b) return 0;
    int is3d = t->target == GL_TEXTURE_3D;   // 3D 的链按宽、高、深最大者折半；数组层数各级相同
    int n = p->min == GL_NEAREST || p->min == GL_LINEAR ? 1 : mip_count(is3d ? MAX(b.width, b.depth) : b.width, b.height);
    if ((int64_t)n > (int64_t)max - base + 1) n = (int)((int64_t)max - base + 1);   // max 可到 INT32_MAX（合法），按 64 位算跨度（wwmogu #386）
    if (t->immutable && n > t->immutable - base) n = t->immutable - base;
    if (n <= 0 || n > TEX_LEVELS - base) return 0;
    for (int s = 0; s < nfaces(t); s++) for (int l = 0; l < n; l++) { id<MTLTexture> i = t->level[s][base + l].image;
        if (!i || i.pixelFormat != b.pixelFormat || t->level[s][base + l].format != t->level[0][base].format || i.width != MAX(1, b.width >> l) || i.height != MAX(1, b.height >> l) ||
            vol_depth(i) != (is3d ? MAX(1, b.depth >> l) : vol_depth(b))) return 0; }
    return n;
}
// 只取现有存储 / 建子范围视图，不编码 GPU 命令。bind 只能走这里。
static MTLTextureType sample_type(const gl_texture *t) {
    return t->target == GL_TEXTURE_CUBE_MAP ? MTLTextureTypeCube : t->target == GL_TEXTURE_2D_ARRAY ? MTLTextureType2DArray : t->target == GL_TEXTURE_3D ? MTLTextureType3D : MTLTextureType2D;
}
static id<MTLTexture> sampling_view(gl_texture *t, int n) {
    int base = base_level(t), cube = t->target == GL_TEXTURE_CUBE_MAP;
    if (n == 1 && !cube) return t->level[0][base].image;   // 体纹理每级本身就是 2D 数组 / 3D 贴图
    if (t->sample && t->sample_base == base && t->sample_count == n) return t->sample;
    // 同一 parent 还须逐面逐级对应；仅对象相同不足以证明 mip / cube 的排列。
    id<MTLTexture> first = t->level[0][base].image, parent = first.parentTexture;
    NSUInteger start = first.parentRelativeLevel;
    MTLTextureType type = sample_type(t);
    if (parent && parent.textureType == type && first.parentRelativeSlice == 0 &&
        start <= parent.mipmapLevelCount && (NSUInteger)n <= parent.mipmapLevelCount - start) {
        int all = 1;
        for (int s = 0; s < nfaces(t) && all; s++) for (int l = 0; l < n && all; l++) {
            id<MTLTexture> i = t->level[s][base + l].image;
            all = i.parentTexture == parent && i.parentRelativeLevel == start + (NSUInteger)l && i.parentRelativeSlice == (NSUInteger)s;
        }
        if (all) {
            id<MTLTexture> v = start || parent.mipmapLevelCount != (NSUInteger)n ?
                [parent newTextureViewWithPixelFormat:parent.pixelFormat textureType:type levels:NSMakeRange(start, n) slices:NSMakeRange(0, view_slices(parent))] : parent;
            if (!v) { gl_set_error(GL_OUT_OF_MEMORY); return nil; }
            sample_set(t, v, base, n); return v;
        }
    }
    return nil;
}
// GPU 组链：保留全部已有格式（包括 private 深度 / 模板），无需 CPU 读回、提交或等待。
// 先建齐所有视图再编码 / 发布；命令缓冲持有源纹理，重定义不会提前释放在途旧引用。
static id<MTLTexture> sampling_image(gl_texture *t, int n) {
    id<MTLTexture> ready = sampling_view(t, n); if (ready) return ready;
    int base = base_level(t), cube = t->target == GL_TEXTURE_CUBE_MAP;
    id<MTLTexture> b = t->level[0][base].image; int f = t->level[0][base].fmt, vol = is_vol(t), is3d = t->target == GL_TEXTURE_3D;
    id<MTLTexture> image = vol ? allocate_vol(f, b.width, b.height, vol_depth(b), n, is3d) : allocate(f, b.width, b.height, n, cube); if (!image) return nil;
    id<MTLTexture> views[6][TEX_LEVELS] = {{nil}};
    for (int s = 0; s < nfaces(t); s++) for (int l = 0; l < n; l++) {
        views[s][l] = vol ? [image newTextureViewWithPixelFormat:image.pixelFormat textureType:image.textureType levels:NSMakeRange(l, 1) slices:NSMakeRange(0, is3d ? 1 : vol_depth(b))]
                          : [image newTextureViewWithPixelFormat:image.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(l, 1) slices:NSMakeRange(s, 1)];
        if (!views[s][l]) { gl_set_error(GL_OUT_OF_MEMORY); return nil; }
    }
    id<MTLBlitCommandEncoder> be = gl_blit_enc();
    if (!be) { gl_set_error(GL_OUT_OF_MEMORY); return nil; }
    for (int s = 0; s < nfaces(t); s++) for (int l = 0; l < n; l++) {
        id<MTLTexture> src = t->level[s][base + l].image;
        if (!vol) [be copyFromTexture:src sourceSlice:0 sourceLevel:0 toTexture:image destinationSlice:s destinationLevel:l sliceCount:1 levelCount:1];
        else if (is3d) [be copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(src.width, src.height, src.depth)
                                 toTexture:image destinationSlice:0 destinationLevel:l destinationOrigin:MTLOriginMake(0, 0, 0)];
        else [be copyFromTexture:src sourceSlice:0 sourceLevel:0 toTexture:image destinationSlice:0 destinationLevel:l sliceCount:src.arrayLength levelCount:1];
    }
    // 仅存储身份变化，宽屏影子仍对应同一份内容；mip / 换位采影子的既有限制由 gl_wd_sample 明确报红。
    int shadow_base = !cube && !base && t->wd.orig == b;
    for (int s = 0; s < nfaces(t); s++) for (int l = 0; l < n; l++) t->level[s][base + l].image = views[s][l];
    if (shadow_base) {
        if (t->wd.tex == b) t->wd.tex = t->level[0][0].image;
        t->wd.orig = t->level[0][0].image;
    }
    sample_set(t, image, base, n); t->gpu_written = 1; t->last_use = gl_seq_ref(); return image;
}
GLB(glGenerateMipmap, void, GLenum target) { @autoreleasepool {
    CTX(); gl_texture *t = bound(target); if (!t) return;
    int base = base_level(t), max = max_level(t), cube = t->target == GL_TEXTURE_CUBE_MAP;
    if (base >= TEX_LEVELS || max < base || !t->level[0][base].image) { gl_set_error(GL_INVALID_OPERATION); return; }
    id<MTLTexture> b = t->level[0][base].image; int f = t->level[0][base].fmt, n = mip_count(b.width, b.height);
    if ((int64_t)n > (int64_t)max - base + 1) n = (int)((int64_t)max - base + 1);   // max 可到 INT32_MAX（合法），按 64 位算跨度（wwmogu #386）
    if (t->immutable && n > t->immutable - base) n = t->immutable - base;
    if (n < 1 || n > TEX_LEVELS - base) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (n == 1 && !cube) return;
    if (k_fmt[f].kind != K_COL) { GLRED1("非颜色格式 GenerateMipmap 没实现"); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return; }
    if (is_vol(t)) { GLRED1("2D 数组 / 3D 纹理 GenerateMipmap 没实现"); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return; }
    if (!gpu_settle(t)) return;
    id<MTLTexture> image = allocate(f, b.width, b.height, n, cube); if (!image) return;
    for (int s = 0; s < nfaces(t); s++) { if (!t->level[s][base].image || !copy_level(t->level[s][base].image, f, image, 0, s)) { gl_set_error(GL_INVALID_OPERATION); return; } }
    [gl_blit_enc() generateMipmapsForTexture:image]; uint64_t seq = gl_commit();
    if (![gl_ev waitUntilSignaledValue:seq timeoutMS:5000]) { GLOG("【红】 GenerateMipmap 等 GPU 超过5秒，未发布新级"); gl_set_error(GL_INVALID_OPERATION); gl_bad_err++; return; }
    for (int s = 0; s < nfaces(t); s++) for (int l = 0; l < n; l++) {
        t->level[s][base + l].image = [image newTextureViewWithPixelFormat:image.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(l,1) slices:NSMakeRange(s,1)];
        t->level[s][base + l].format = t->level[0][base].format; t->level[s][base + l].fmt = f;
    }
    sample_set(t, image, base, n);
}}
static int texture_dims(const gl_texture *t, uint32_t *w, uint32_t *h) {
    if (!t) return 0;
    int b = base_level(t); id<MTLTexture> i = b >= 0 && b < TEX_LEVELS ? t->level[0][b].image : nil; if (!i) return 0;
    *w = (uint32_t)i.width; *h = (uint32_t)i.height; return 1;
}
int gl_tex_dims(GLuint name, uint32_t *w, uint32_t *h) { return texture_dims(gl_name_get(&textures, name), w, h); }
int gl_tex_unit_dims(GLuint unit, int slot, uint32_t *w, uint32_t *h) { return texture_dims(unit_texture(unit, slot, 0), w, h); }
gl_wd_shadow *gl_tex_wd(GLuint name) { gl_texture *t = gl_name_get(&textures, name); return t ? &t->wd : NULL; }
id<MTLTexture> gl_tex_mtl(GLuint name, GLint level) { gl_texture *t = gl_name_get(&textures, name); return t && level >= 0 && level < TEX_LEVELS ? t->level[0][level].image : nil; }
id<MTLTexture> gl_tex_attach(GLuint name, GLenum target, GLint level) {
    gl_texture *t = gl_name_get(&textures, name); int face;
    if (!t || level < 0 || level >= TEX_LEVELS || image_target(target, &face) < 0) return nil;
    id<MTLTexture> image = t->level[face][level].image; if (!image) return nil;
    if (!(image.usage & MTLTextureUsageRenderTarget)) { GLRED1("纹理格式 %lu 不能当渲染目标", (unsigned long)image.pixelFormat); return nil; }
    // 独立存储 / 子范围缓存作废后，下次采样从各级的当前 parent 重取或 GPU 重组。
    if (t->sample && image.parentTexture != t->sample && !(image == t->sample)) sample_drop(t);
    t->gpu_written = 1; t->last_use = gl_seq_ref();
    return image;
}
static id<MTLTexture> black_image(int slot) {   // 目标序号同 gl_tex_slot：2D / 立方体 / 2D 数组（1 层）/ 3D（1 片）
    if (g_black[slot]) return g_black[slot];
    int cube = slot == 1; id<MTLTexture> b = slot >= 2 ? allocate_vol(F_RGBA8, 1, 1, 1, 1, slot == 3) : allocate(F_RGBA8, 1, 1, 1, cube); const uint8_t px[] = {0,0,0,255};
    if (!b) return nil;
    for (int s = 0; s < (cube ? 6 : 1); s++) [b replaceRegion:MTLRegionMake3D(0,0,0,1,1,1) mipmapLevel:0 slice:s withBytes:px bytesPerRow:4 bytesPerImage:slot == 3 ? 4 : 0];
    return g_black[slot] = b;
}
// 要换位的格式（亮度 / alpha / 不带 alpha 的 DXT1）：按组好的采样贴图建一个换位视图，同一张采样贴图只建一次
// 游戏设的 GL_TEXTURE_SWIZZLE_*（uswz）叠在格式换位之后：GL 规定换位作用在格式展开后的 (R,G,B,A) 上，所以取「格式换位的第 k 个分量」
static id<MTLTexture> swizzled(gl_texture *t, id<MTLTexture> image) {
    const fmt_t *m = &k_fmt[t->level[0][base_level(t)].fmt];
    if (!m->swizzled && !(t->uswz[0] | t->uswz[1] | t->uswz[2] | t->uswz[3])) return image;
    if (t->view && t->view_src == image) return t->view;
    static const MTLTextureSwizzleChannels identity = SW0; const MTLTextureSwizzleChannels base = m->swizzled ? m->swz : identity;
    const MTLTextureSwizzle bc[4] = {base.red, base.green, base.blue, base.alpha}; MTLTextureSwizzle out[4];
    for (int c = 0; c < 4; c++) { int u = t->uswz[c] ? t->uswz[c] - 1 : MTLTextureSwizzleRed + c; out[c] = u <= MTLTextureSwizzleOne ? (MTLTextureSwizzle)u : bc[u - MTLTextureSwizzleRed]; }
    t->view_src = image; t->view = [image newTextureViewWithPixelFormat:image.pixelFormat textureType:image.textureType levels:NSMakeRange(0, image.mipmapLevelCount) slices:NSMakeRange(0, view_slices(image))
                                                                   swizzle:MTLTextureSwizzleChannelsMake(out[0], out[1], out[2], out[3])];
    return t->view;
}
int gl_tex_prepare(const struct gl_program *program) {
    for (int k = 0; k < program->nsmp; k++) {
        int unit = program->smp[k].unit, slot = gl_tex_sampler_slot(program->smp[k].type);
        if (unit < 0 || unit >= 32 || slot < 0) {
            GLRED1("采样 uniform 的类型或单元不受支持"); gl_set_error(GL_INVALID_OPERATION); return 0;
        }
        gl_texture *t = unit_texture((GLuint)unit, slot, 0);
        gl_sampler *s = gl_name_get(&samplers, gl_cur->smp_unit[unit]);
        tex_params p = s ? s->params : t ? t->params : defaults(); int n = complete(t, &p);
        if (n && !sampling_image(t, n)) return 0;
    }
    return 1;
}
int gl_tex_bind(id<MTLRenderCommandEncoder> re, const struct gl_program *program) {
    for (int k = 0; k < program->nsmp; k++) {
        int unit = program->smp[k].unit, slot = gl_tex_sampler_slot(program->smp[k].type);
        if (unit < 0 || unit >= 32 || slot < 0) { GLRED1("采样 uniform 的类型或单元不受支持"); gl_set_error(GL_INVALID_OPERATION); return 0; }
        gl_texture *t = unit_texture((GLuint)unit, slot, 0); gl_sampler *s = gl_name_get(&samplers, gl_cur->smp_unit[unit]);
        tex_params p = s ? s->params : t ? t->params : defaults(); int n = complete(t, &p);
        id<MTLTexture> image = n ? sampling_view(t, n) : nil;
        if (n && !image) { GLRED1("完整纹理未在渲染编码前准备好采样存储"); gl_set_error(GL_INVALID_OPERATION); return 0; }
        if (image) { image = swizzled(t, image); if (!image) { gl_set_error(GL_OUT_OF_MEMORY); return 0; } }
        if (image && !slot) image = gl_wd_sample(&t->wd, t->level[0][0].image, image);   // 宽屏：当过附件且放宽了的纹理采影子（gl_widen.inc）
        if (!image) { n = 0; image = black_image(slot); if (!image) return 0; }
        if (!n) { static int warned; if (!warned++) GLOG("【黄】 不完整 / 未绑定纹理采样为 (0,0,0,1)"); }
        id<MTLSamplerState> sampler = s ? s->state : t ? t->sampler : g_fallback_sampler;
        if (!sampler) {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.minFilter = p.min == GL_LINEAR || p.min == GL_LINEAR_MIPMAP_NEAREST || p.min == GL_LINEAR_MIPMAP_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.magFilter = p.mag == GL_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = p.min == GL_NEAREST || p.min == GL_LINEAR ? MTLSamplerMipFilterNotMipmapped : p.min == GL_NEAREST_MIPMAP_NEAREST || p.min == GL_LINEAR_MIPMAP_NEAREST ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        d.sAddressMode = address(p.wrap[0]); d.tAddressMode = address(p.wrap[1]); d.rAddressMode = address(p.wrap[2]);
        d.lodMinClamp = fmaxf(0, p.min_lod); d.lodMaxClamp = fmaxf(d.lodMinClamp, p.max_lod); d.maxAnisotropy = (NSUInteger)fmaxf(1, p.aniso);
        d.compareFunction = p.compare_mode == GL_COMPARE_REF_TO_TEXTURE ? (MTLCompareFunction)(p.compare_func - GL_NEVER) : MTLCompareFunctionAlways;
        // Metal 的边框色只有三种：按 GL 边框色取最近的（透明黑 / 不透明黑 / 不透明白）
        d.borderColor = p.border[3] < .5f ? MTLSamplerBorderColorTransparentBlack : p.border[0] + p.border[1] + p.border[2] > 1.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
        sampler = [gl_dev newSamplerStateWithDescriptor:d]; if (!sampler) { GLOG("【红】 建采样器失败"); return 0; }
        if (s) s->state = sampler; else if (t) t->sampler = sampler; else g_fallback_sampler = sampler;
        }
        for (int st = 0; st < 2; st++) { int slot = program->smp[k].slot[st]; if (slot < 0) continue; if (slot >= 16) { GLRED1("采样器槽超出 16"); return 0; }
            if (st) { [re setFragmentTexture:image atIndex:slot]; [re setFragmentSamplerState:sampler atIndex:slot]; }
            else { [re setVertexTexture:image atIndex:slot]; [re setVertexSamplerState:sampler atIndex:slot]; }
        }
        if (n) t->last_use = gl_seq_ref();
    } return 1;
}
GLenum gl_tex_level_format(GLuint name, GLenum target, GLint level) {
    gl_texture *t = gl_name_get(&textures, name); int face;
    if (!t || level < 0 || level >= TEX_LEVELS || image_target(target, &face) < 0 || !t->level[face][level].image) return 0;
    return t->level[face][level].format;
}
// NVN 互操作（nvnTextureBuilderSetGLTextureName，nvn_mtl.m nvn_be_tex_from_gl 调）：GL 二维纹理名 → 它的存储与描述。真机 GL 与 NVN 是同一个驱动、同一块显存，
//   NVN 纹理按 GL 纹理建出来就是同一份存储，所以这里交出的就是 GL 画进去、采样出来的那张 MTLTexture：不可变存储（glTexStorage2D）= 整条级链，
//   可变存储（glTexImage2D 逐级各一张）只有第 0 级能共享、级数报 1。没这个名字 / 不是二维纹理 / 第 0 级没定义存储 → nil（调用方报红）。
//   之后 GL 再重新指定第 0 级存储（glTexImage2D / 再建）不会跟到 NVN 纹理上：规范要求互操作期间 GL 纹理存储不变（不可变存储）
id<MTLTexture> gl_tex_export(GLuint name, GLenum *internal, uint32_t *levels) {
    gl_texture *t = gl_name_get(&textures, name);
    if (!t || t->target != 0x0DE1 || !t->level[0][0].image) return nil;   // GL_TEXTURE_2D
    id<MTLTexture> image = t->level[0][0].image;
    *internal = t->level[0][0].format;
    if (t->immutable && image.parentTexture) { *levels = (uint32_t)t->immutable; return image.parentTexture; }
    *levels = 1; return image;
}

// ---------- 体纹理（glTexImage3D / glTexSubImage3D，Slay the Spire 取到入口） ----------
// 客户端数据：层 / 片之间的跨度 = 行步长 × 高（UNPACK_IMAGE_HEIGHT 没实现、取缺省 0 = 高），行步长同 2D 上传（UNPACK_ROW_LENGTH / ALIGNMENT）
static size_t unpack_image(GLenum format, GLenum type, GLsizei w, GLsizei h) {
    int nc, nb; if (!pixel_layout(format, type, &nc, &nb)) return 0;
    size_t length = gl_cur->unpack_row ? (size_t)gl_cur->unpack_row : (size_t)w, alignment = gl_cur->unpack_align ? (size_t)gl_cur->unpack_align : 4;
    return ((length * (size_t)(nc * nb) + alignment - 1) & ~(alignment - 1)) * (size_t)h;
}
static gl_texture *bound_vol(GLenum target, const char *fn) {
    int slot = gl_tex_slot(target); if (slot != 2 && slot != 3) { unsupported(fn, target); return NULL; }
    return bound_slot(slot);
}
GLB(glTexImage3D, void, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, uint64_t type64, const void *data) { @autoreleasepool {   // type 在栈上：按游戏的 8 字节一格读（见下 glTexSubImage3D）
    GLenum type = (GLenum)type64;
    CTX(); gl_texture *t = bound_vol(target, "glTexImage3D 目标"); if (!t) return;
    if (t->immutable) { gl_set_error(GL_INVALID_OPERATION); return; }
    int is3d = target == GL_TEXTURE_3D, max_wh = is3d ? 2048 : 16384;   // MAX_3D_TEXTURE_SIZE 2048（Metal 3D 上限）、MAX_ARRAY_TEXTURE_LAYERS 2048
    if (level < 0 || level >= TEX_LEVELS || w < 0 || h < 0 || d < 0 || w > (max_wh >> level) || h > (max_wh >> level) || d > (is3d ? 2048 >> level : 2048) || border) { gl_set_error(GL_INVALID_VALUE); return; }
    int f = fmt_of(internal, type), nc, nb;
    if (!f) { unsupported("glTexImage3D internalformat", internal); return; }
    if (k_fmt[f].kind == K_BLOCK) { GLRED1("glTexImage3D 往压缩格式 %#x 传未压缩数据没实现", internal); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return; }
    if (is3d && k_fmt[f].kind != K_COL) { gl_set_error(GL_INVALID_OPERATION); return; }   // 深度 / 模板格式不能做 3D 纹理（规范）
    if (!pixel_layout(format, type, &nc, &nb)) return;
    if (!unpack_src(&data, unpack_image(format, type, w, h) * (size_t)d)) return;
    if (!gpu_settle(t)) return;
    if (!w || !h || !d) { t->level[0][level].image = nil; sample_drop(t); return; }
    id<MTLTexture> image = allocate_vol(f, (NSUInteger)w, (NSUInteger)h, (NSUInteger)d, 1, is3d); if (!image) return;
    size_t step = unpack_image(format, type, w, h);
    for (GLsizei z = 0; data && z < d; z++)
        if (!upload(image, f, is3d ? 0 : (NSUInteger)z, is3d ? (NSUInteger)z : 0, 0, 0, w, h, format, type, (const uint8_t *)data + (size_t)z * step, is_rgb_internal(internal))) return;
    t->level[0][level].image = image; t->level[0][level].format = sized_format((GLenum)internal); t->level[0][level].fmt = f; sample_drop(t);
}}
// 第 9–11 个参数在栈上（同 gl_fb.m glBlitFramebuffer）：Switch 标准 AAPCS64 每个栈参数占 8 字节一格（format sp+0、type sp+8、data sp+16），
//   Apple ARM64 把 32 位栈参数紧挨着排（format sp+0、type sp+4、data sp+8）。照 32 位声明，宿主把 format 那格的高 32 位当 type、把 type 那格当 data 指针读
//   （小丑牌 / LÖVE 11 起局给 3D、2D 数组默认纹理写白：type 读成垃圾 0x70 → GL_INVALID_ENUM → LÖVE 查到 GL 错误抛异常、游戏退出）。声明成 64 位按 8 字节一格读
GLB(glTexSubImage3D, void, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, uint64_t format64, uint64_t type64, const void *data) { @autoreleasepool {
    GLenum format = (GLenum)format64, type = (GLenum)type64;
    CTX(); gl_texture *t = bound_vol(target, "glTexSubImage3D 目标"); if (!t) return;
    if (level < 0 || level >= TEX_LEVELS) { gl_set_error(GL_INVALID_VALUE); return; }
    tex_level *lv = &t->level[0][level]; id<MTLTexture> old = lv->image; if (!old) { gl_set_error(GL_INVALID_OPERATION); return; }
    NSUInteger depth = vol_depth(old); int is3d = target == GL_TEXTURE_3D;
    if (x < 0 || y < 0 || z < 0 || w < 0 || h < 0 || d < 0 || (NSUInteger)x > old.width || (NSUInteger)w > old.width - (NSUInteger)x ||
        (NSUInteger)y > old.height || (NSUInteger)h > old.height - (NSUInteger)y || (NSUInteger)z > depth || (NSUInteger)d > depth - (NSUInteger)z) { gl_set_error(GL_INVALID_VALUE); return; }
    if (k_fmt[lv->fmt].kind == K_BLOCK) { gl_set_error(GL_INVALID_OPERATION); return; }   // 压缩存储不收未压缩来源（规范错误，同 2D）
    int nc, nb; if (!pixel_layout(format, type, &nc, &nb)) return; if (!w || !h || !d) return;
    if (!unpack_src(&data, unpack_image(format, type, w, h) * (size_t)d)) return;
    if (!data) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!gpu_settle(t)) return;
    id<MTLTexture> image = old;
    if (gl_seq_busy(t->last_use)) {   // 在途：换存储，已编码的绘制读旧的
        image = allocate_vol(lv->fmt, old.width, old.height, depth, 1, is3d); if (!image || !copy_vol(old, lv->fmt, image)) return;
    }
    size_t step = unpack_image(format, type, w, h); int rgb = is_rgb_internal((GLint)lv->format);
    for (GLsizei k = 0; k < d; k++)
        if (!upload(image, lv->fmt, is3d ? 0 : (NSUInteger)(z + k), is3d ? (NSUInteger)(z + k) : 0, x, y, w, h, format, type, (const uint8_t *)data + (size_t)k * step, rgb)) return;
    lv->image = image;
    if (image != old) sample_drop(t);
    else if (!(t->sample && image.parentTexture == t->sample)) sample_drop(t);
}}
// 体纹理的不可变存储（小丑牌 / LÖVE 11 起局给每种纹理类型建 1×1 默认纹理，3D 与 2D 数组走这里；以前查表回空，游戏跳到地址 0 被系统杀）。
//   存法同 glTexImage3D：每级一张（level[0][l]），2D 数组每级层数不变、3D 每级三边各减半；之后只能 glTexSubImage3D 改内容。
GLB(glTexStorage3D, void, GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d) { @autoreleasepool {
    CTX(); int slot = gl_tex_slot(target); if (slot != 2 && slot != 3) { gl_set_error(GL_INVALID_ENUM); return; }   // 只收 2D 数组 / 3D（规范）
    if (!gl_cur->tex_unit[gl_cur->active_unit][slot]) { gl_set_error(GL_INVALID_OPERATION); return; }   // 默认对象不能做不可变存储（同 glTexStorage2D）
    gl_texture *t = bound_slot(slot); if (!t) return;
    if (t->immutable) { gl_set_error(GL_INVALID_OPERATION); return; }
    int is3d = target == GL_TEXTURE_3D, max_wh = is3d ? 2048 : 16384;
    int maxl = is3d ? mip_count((NSUInteger)(w > d ? w : d), (NSUInteger)h) : mip_count((NSUInteger)w, (NSUInteger)h);
    if (w < 1 || h < 1 || d < 1 || w > max_wh || h > max_wh || d > 2048 || levels < 1 || levels > maxl || levels > TEX_LEVELS) { gl_set_error(GL_INVALID_VALUE); return; }
    int f = fmt_of((GLint)internal, 0);
    if (internal == GL_RGBA || internal == GL_RGB || internal == GL_RED || internal == GL_RG || !f) { unsupported("glTexStorage3D internalformat", internal); return; }
    if (is3d && k_fmt[f].kind != K_COL) { gl_set_error(GL_INVALID_OPERATION); return; }   // 深度 / 模板 / 压缩格式不能做 3D 纹理
    if (!gpu_settle(t)) return;
    id<MTLTexture> images[TEX_LEVELS] = {nil};
    for (int l = 0; l < levels; l++) {
        NSUInteger lw = (NSUInteger)w >> l, lh = (NSUInteger)h >> l, ld = is3d ? (NSUInteger)d >> l : (NSUInteger)d;
        images[l] = allocate_vol(f, lw ? lw : 1, lh ? lh : 1, ld ? ld : 1, 1, is3d); if (!images[l]) return;
    }
    for (int l = 0; l < TEX_LEVELS; l++) { t->level[0][l].image = images[l]; t->level[0][l].format = internal; t->level[0][l].fmt = images[l] ? f : 0; }
    sample_drop(t); t->immutable = levels; t->gpu_written = 0; t->last_use = 0;
}}

// ---------- 读回（glGetTexImage / glGetCompressedTexImage，Slay the Spire 取到入口） ----------
// 读取目标 → 纹理与面：2D、立方体某面、2D 数组 / 3D（全部层 / 片）；GL_TEXTURE_CUBE_MAP 本身不是合法目标
static gl_texture *read_target(GLenum target, int *face) {
    int slot = target == GL_TEXTURE_2D_ARRAY ? 2 : target == GL_TEXTURE_3D ? 3 : image_target(target, face);
    if (slot >= 2) *face = 0;
    if (slot < 0) { gl_set_error(GL_INVALID_ENUM); return NULL; }
    gl_texture *t = unit_texture(gl_cur->active_unit, slot, 1); if (!t) gl_set_error(GL_INVALID_OPERATION);
    return t;
}
// 某级原始字节（紧排：行 = 存储行字节、层 = 行 × 行数，n 层 / 片）：共享存储直接取；私有存储（深度）经 GPU 拷进共享缓冲、提交并等（深度模板取深度平面）
static uint8_t *raw_level(id<MTLTexture> img, int f, size_t *row, size_t *ib, NSUInteger *n) {
    NSUInteger w = img.width, h = img.height; int is3d = img.textureType == MTLTextureType3D; *n = vol_depth(img);
    *row = f == F_D32FS8 ? (size_t)w * 4 : store_row(f, w); *ib = *row * store_rows(f, h);
    uint8_t *out = malloc(*ib * *n ? *ib * *n : 1); if (!out) { gl_set_error(GL_OUT_OF_MEMORY); return NULL; }
    if (cpu_ok(img)) {
        for (NSUInteger i = 0; i < *n; i++)
            [img getBytes:out + i * *ib bytesPerRow:*row bytesPerImage:*ib fromRegion:MTLRegionMake3D(0, 0, is3d ? i : 0, w, h, 1) mipmapLevel:0 slice:is3d ? 0 : i];
        return out;
    }
    @autoreleasepool {
        id<MTLBuffer> b = [gl_dev newBufferWithLength:*ib * *n options:MTLResourceStorageModeShared]; if (!b) { free(out); gl_set_error(GL_OUT_OF_MEMORY); return NULL; }
        id<MTLBlitCommandEncoder> be = gl_blit_enc();
        for (NSUInteger i = 0; i < *n; i++)
            [be copyFromTexture:img sourceSlice:is3d ? 0 : i sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, is3d ? i : 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b
              destinationOffset:i * *ib destinationBytesPerRow:*row destinationBytesPerImage:*ib options:f == F_D32FS8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        uint64_t v = gl_commit(); gl_wait(v);
        if (gl_ev.signaledValue < v) { free(out); gl_set_error(GL_INVALID_OPERATION); return NULL; }
        memcpy(out, b.contents, *ib * *n);
    }
    return out;
}
static float ufloat_bits(uint32_t v, int mbits) {   // 无符号小浮点（11 / 10 位：5 位指数）→ float
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (!e) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}
// 存储的一个像素 → GL 纹理分量（表 8.? 纹理基本格式到 RGBA：亮度 → R、alpha → A，缺的颜色读 0、alpha 读 1；RGB 内部格式 alpha 读 1）；深度放 v[0]
static void decode_px(int f, const uint8_t *p, int rgb, double v[4]) {
    const fmt_t *m = &k_fmt[f]; v[0] = v[1] = v[2] = 0; v[3] = 1;
    if (f == F_RG11B10F) { uint32_t x; memcpy(&x, p, 4); v[0] = ufloat_bits(x & 0x7FF, 6); v[1] = ufloat_bits(x >> 11 & 0x7FF, 6); v[2] = ufloat_bits(x >> 22, 5); }
    else if (f == F_RGB10A2) { uint32_t x; memcpy(&x, p, 4); v[0] = (x & 1023) / 1023.0; v[1] = (x >> 10 & 1023) / 1023.0; v[2] = (x >> 20 & 1023) / 1023.0; v[3] = (x >> 30) / 3.0; }
    else if (f == F_RGBA16 || f == F_D16) { for (int k = 0; k < m->ch; k++) { uint16_t a; memcpy(&a, p + 2 * k, 2); v[k] = a / 65535.0; } }
    else if (f == F_D32F || f == F_D32FS8) { float a; memcpy(&a, p, 4); v[0] = a; }
    else for (int k = 0; k < m->ch; k++) {
        if (m->ct == 0) v[k] = p[k] / 255.0;
        else if (m->ct == 1) { _Float16 a; memcpy(&a, p + 2 * k, 2); v[k] = a; }
        else { float a; memcpy(&a, p + 4 * k, 4); v[k] = a; }
    }
    if (f == F_L8) { v[1] = v[2] = 0; } else if (f == F_A8) { v[3] = v[0]; v[0] = 0; } else if (f == F_LA8) { v[3] = v[1]; v[1] = 0; }
    if (rgb) v[3] = 1;
}
static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
// 输出一个像素：按 format 取分量（BGRA 换序、LUMINANCE 取 R、DEPTH 取深度），按 type 编码（规格化整数钳到 [0,1]）
static void encode_px(GLenum format, GLenum type, const double v[4], uint8_t *o) {
    double c[4]; int n;
    switch (format) {
        case GL_RGBA: n = 4; memcpy(c, v, sizeof c); break; case GL_BGRA: n = 4; c[0] = v[2]; c[1] = v[1]; c[2] = v[0]; c[3] = v[3]; break;
        case GL_RGB: n = 3; memcpy(c, v, 3 * sizeof *c); break; case GL_RG: n = 2; memcpy(c, v, 2 * sizeof *c); break;
        case GL_RED: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: n = 1; c[0] = v[0]; break;
        case GL_ALPHA: n = 1; c[0] = v[3]; break; case GL_LUMINANCE_ALPHA: n = 2; c[0] = v[0]; c[1] = v[3]; break;
        default: return;
    }
    if (type == GL_UNSIGNED_SHORT_5_6_5) { uint16_t x = (uint16_t)(lround(clamp01(c[0]) * 31) << 11 | lround(clamp01(c[1]) * 63) << 5 | lround(clamp01(c[2]) * 31)); memcpy(o, &x, 2); return; }
    if (type == GL_UNSIGNED_SHORT_4_4_4_4) { uint16_t x = (uint16_t)(lround(clamp01(c[0]) * 15) << 12 | lround(clamp01(c[1]) * 15) << 8 | lround(clamp01(c[2]) * 15) << 4 | lround(clamp01(c[3]) * 15)); memcpy(o, &x, 2); return; }
    if (type == GL_UNSIGNED_SHORT_5_5_5_1) { uint16_t x = (uint16_t)(lround(clamp01(c[0]) * 31) << 11 | lround(clamp01(c[1]) * 31) << 6 | lround(clamp01(c[2]) * 31) << 1 | lround(clamp01(c[3]))); memcpy(o, &x, 2); return; }
    if (type == GL_UNSIGNED_INT_2_10_10_10_REV) { uint32_t x = (uint32_t)lround(clamp01(c[0]) * 1023) | (uint32_t)lround(clamp01(c[1]) * 1023) << 10 | (uint32_t)lround(clamp01(c[2]) * 1023) << 20 | (uint32_t)lround(clamp01(c[3]) * 3) << 30; memcpy(o, &x, 4); return; }
    for (int k = 0; k < n; k++) {
        if (type == GL_UNSIGNED_BYTE) o[k] = (uint8_t)lround(clamp01(c[k]) * 255);
        else if (type == GL_UNSIGNED_SHORT) { uint16_t a = (uint16_t)lround(clamp01(c[k]) * 65535); memcpy(o + 2 * k, &a, 2); }
        else if (type == GL_UNSIGNED_INT) { uint32_t a = (uint32_t)llround(clamp01(c[k]) * 4294967295.0); memcpy(o + 4 * k, &a, 4); }
        else if (type == GL_HALF_FLOAT) { _Float16 a = (_Float16)c[k]; memcpy(o + 2 * k, &a, 2); }
        else { float a = (float)c[k]; memcpy(o + 4 * k, &a, 4); }
    }
}
// 存储原样就是要的格式 / 类型（逐字节一致）：浮点 / 半精度 / 打包格式不经换算，读回位精确
static int native_read(int f, GLenum format, GLenum type, int rgb) {
    const fmt_t *m = &k_fmt[f];
    if (m->swizzled || rgb) return 0;
    if (f == F_RG11B10F) return format == GL_RGB && type == GL_UNSIGNED_INT_10F_11F_11F_REV;
    if (f == F_RGB10A2) return format == GL_RGBA && type == GL_UNSIGNED_INT_2_10_10_10_REV;
    if (f == F_RGBA16) return format == GL_RGBA && type == GL_UNSIGNED_SHORT;
    if (f == F_D32F || f == F_D32FS8) return format == GL_DEPTH_COMPONENT && type == GL_FLOAT;
    if (f == F_D16) return format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT;
    int nc = format == GL_RGBA ? 4 : format == GL_RG ? 2 : format == GL_RED ? 1 : 0;
    return nc == m->ch && ((m->ct == 0 && type == GL_UNSIGNED_BYTE) || (m->ct == 1 && type == GL_HALF_FLOAT) || (m->ct == 2 && type == GL_FLOAT));
}
GLB(glGetTexImage, void, GLenum target, GLint level, GLenum format, GLenum type, void *pixels) { @autoreleasepool {
    CTX(); int face; gl_texture *t = read_target(target, &face); if (!t) return;
    if (level < 0 || level >= TEX_LEVELS) { gl_set_error(GL_INVALID_VALUE); return; }
    int nc, nb; if (!pixel_layout(format, type, &nc, &nb)) return;
    tex_level *lv = &t->level[face][level]; if (!lv->image || !pixels) return;   // 这一级没定义：什么都不写（规范）
    const fmt_t *m = &k_fmt[lv->fmt]; int depth = m->kind == K_DEPTH || m->kind == K_DS;
    if (m->kind == K_BLOCK) { GLRED1("glGetTexImage：压缩纹理按非压缩格式读回（要解压）没实现，用 glGetCompressedTexImage"); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return; }
    if (depth != (format == GL_DEPTH_COMPONENT) || format == GL_DEPTH_STENCIL) {   // 深度纹理只能按深度读、颜色纹理不能按深度读（规范）；模板平面读回没实现
        if (format == GL_DEPTH_STENCIL || lv->fmt == F_S8) { GLRED1("glGetTexImage：模板读回没实现"); gl_bad_feat++; }
        gl_set_error(GL_INVALID_OPERATION); return;
    }
    int special = format == GL_COLOR_INDEX || type == GL_UNSIGNED_INT_10F_11F_11F_REV || type == GL_UNSIGNED_INT_24_8;   // 这几种只支持存储原样读回
    if (special && !native_read(lv->fmt, format, type, 0)) {
        GLRED1("glGetTexImage：格式 %#x / 类型 %#x 的换算读回没实现", format, type); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return;
    }
    if (!gpu_settle(t)) return;
    id<MTLTexture> img = lv->image; NSUInteger w = img.width, h = img.height, n; size_t row, ib;
    uint8_t *raw = raw_level(img, lv->fmt, &row, &ib, &n); if (!raw) return;
    int rgb = is_rgb_internal((GLint)lv->format), same = native_read(lv->fmt, format, type, rgb);
    size_t px = type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1 ? 2 :
                type == GL_UNSIGNED_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_10F_11F_11F_REV ? 4 : (size_t)(nc * nb);
    size_t align = (size_t)gl_cur->pack_align, orow = (w * px + align - 1) & ~(align - 1), sbytes = f_bytes(lv->fmt);
    for (NSUInteger i = 0; i < n; i++) for (NSUInteger y = 0; y < h; y++) {
        const uint8_t *src = raw + i * ib + y * row; uint8_t *dst = (uint8_t *)pixels + (i * h + y) * orow;
        if (same) { memcpy(dst, src, w * px); continue; }
        for (NSUInteger x = 0; x < w; x++) { double v[4]; decode_px(lv->fmt, src + x * sbytes, rgb, v); encode_px(format, type, v, dst + x * px); }
    }
    free(raw);
}}
GLB(glGetCompressedTexImage, void, GLenum target, GLint level, void *pixels) { @autoreleasepool {
    CTX(); int face; gl_texture *t = read_target(target, &face); if (!t) return;
    if (level < 0 || level >= TEX_LEVELS) { gl_set_error(GL_INVALID_VALUE); return; }
    tex_level *lv = &t->level[face][level];
    if (!lv->image || k_fmt[lv->fmt].kind != K_BLOCK) { gl_set_error(GL_INVALID_OPERATION); return; }   // 没定义或不是压缩存储（规范）
    { uint32_t pf; int bpp; if (bc_unpacked(lv->fmt, &pf, &bpp)) { GLRED1("glGetCompressedTexImage：本机 GPU 不支持 BC，存储已解压，交不出原始压缩块"); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return; } }
    if (!pixels || !gpu_settle(t)) return;
    NSUInteger n; size_t row, ib; uint8_t *raw = raw_level(lv->image, lv->fmt, &row, &ib, &n); if (!raw) return;
    memcpy(pixels, raw, ib * n); free(raw);   // 压缩图像按块紧排（不受 PACK_ALIGNMENT 影响）
}}

// ---------- 即时存档：纹理 / 采样器 / 调色板 ----------
// 纹理的各级图像与采样贴图多是同一张存储的视图（TexStorage 整条级链、组链后的采样贴图）：按「根贴图（不是视图的那张）+ 视图参数」记，根贴图整张存一次（内容含全部级与层），
//   恢复时先建根、再按参数建视图，各级仍共用同一张存储。采样器状态、换位视图、宽屏影子都是缓存：不存，用到时照常重建
typedef struct { int32_t root; uint32_t pf, type, level, nlevels, slice, nslices; uint8_t sw[4]; } tx_view;   // root −1 = 没有
enum { GX_NAME = 1, GX_TARGET, GX_FORMAT, GX_FMT, GX_LVIEW, GX_SVIEW, GX_SBASE, GX_SCOUNT, GX_BASE, GX_MAX, GX_IMM, GX_GPUW, GX_PARAMS, GX_USWZ,
       GM_NAME = 1, GM_PARAMS, GP_PAL = 1, GP_NPAL, GP_ISHIFT, GP_IOFF };
static NSUInteger tx_slices(id<MTLTexture> t) { NSUInteger s = t.arrayLength ? t.arrayLength : 1; return t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? s * 6 : t.textureType == MTLTextureType3D ? 1 : s; }
static tx_view tx_view_of(id<MTLTexture> img, NSMutableArray *roots) {
    tx_view v = {-1}; if (!img) return v;
    id<MTLTexture> r = img; uint32_t lv = 0, sl = 0;
    while (r.parentTexture) { lv += (uint32_t)r.parentRelativeLevel; sl += (uint32_t)r.parentRelativeSlice; r = r.parentTexture; }
    NSUInteger k = [roots indexOfObjectIdenticalTo:r]; if (k == NSNotFound) { k = roots.count; [roots addObject:r]; }
    v.root = (int32_t)k; v.pf = (uint32_t)img.pixelFormat; v.type = (uint32_t)img.textureType; v.level = lv; v.nlevels = (uint32_t)img.mipmapLevelCount; v.slice = sl; v.nslices = (uint32_t)tx_slices(img);
    MTLTextureSwizzleChannels s = img.swizzle; v.sw[0] = s.red; v.sw[1] = s.green; v.sw[2] = s.blue; v.sw[3] = s.alpha;
    return v;
}
static id<MTLTexture> tx_view_make(const tx_view *v, NSArray *roots) {
    if (v->root < 0 || (NSUInteger)v->root >= roots.count || roots[(NSUInteger)v->root] == (id)[NSNull null]) return nil;
    id<MTLTexture> r = roots[(NSUInteger)v->root];
    MTLTextureSwizzleChannels s = MTLTextureSwizzleChannelsMake(v->sw[0], v->sw[1], v->sw[2], v->sw[3]), id_ = MTLTextureSwizzleChannelsDefault;
    if (!v->level && v->nlevels == r.mipmapLevelCount && !v->slice && v->nslices == tx_slices(r) && v->pf == r.pixelFormat && v->type == r.textureType && !memcmp(&s, &id_, sizeof s)) return r;   // 就是根本身
    if (!memcmp(&s, &id_, sizeof s)) return [r newTextureViewWithPixelFormat:(MTLPixelFormat)v->pf textureType:(MTLTextureType)v->type levels:NSMakeRange(v->level, v->nlevels) slices:NSMakeRange(v->slice, v->nslices)];
    return [r newTextureViewWithPixelFormat:(MTLPixelFormat)v->pf textureType:(MTLTextureType)v->type levels:NSMakeRange(v->level, v->nlevels) slices:NSMakeRange(v->slice, v->nslices) swizzle:s];
}
static void tx_rec(sfw *w, GLuint name, gl_texture *t, NSMutableArray *roots) {
    GLenum fmt_[6][TEX_LEVELS]; int fmi[6][TEX_LEVELS]; tx_view lv[6][TEX_LEVELS];
    for (int f = 0; f < 6; f++) for (int l = 0; l < TEX_LEVELS; l++) { fmt_[f][l] = t->level[f][l].format; fmi[f][l] = t->level[f][l].fmt; lv[f][l] = tx_view_of(t->level[f][l].image, roots); }
    tx_view sv = tx_view_of(t->sample, roots);
    sfw_rec(w); GLSF_W(w, GX_NAME, name); GLSF_W(w, GX_TARGET, t->target); GLSF_W(w, GX_FORMAT, fmt_); GLSF_W(w, GX_FMT, fmi); GLSF_W(w, GX_LVIEW, lv); GLSF_W(w, GX_SVIEW, sv);
    GLSF_W(w, GX_SBASE, t->sample_base); GLSF_W(w, GX_SCOUNT, t->sample_count); GLSF_W(w, GX_BASE, t->base); GLSF_W(w, GX_MAX, t->max); GLSF_W(w, GX_IMM, t->immutable);
    GLSF_W(w, GX_GPUW, t->gpu_written); GLSF_W(w, GX_PARAMS, t->params); GLSF_W(w, GX_USWZ, t->uswz); sfw_end_rec(w);
}
typedef struct { GLuint name; gl_texture *t; tx_view lv[6][TEX_LEVELS], sv; } tx_pend;
static gl_texture *tx_parse(const sfr *r, tx_pend *p) {
    gl_texture *t = new_texture(); if (!t) return NULL; memset(p, 0, sizeof *p);
    GLenum fmt_[6][TEX_LEVELS] = {{0}}; int fmi[6][TEX_LEVELS] = {{0}};
    for (int f = 0; f < 6; f++) for (int l = 0; l < TEX_LEVELS; l++) p->lv[f][l].root = -1;
    p->sv.root = -1;
    GLSF_R(r, GX_NAME, p->name); GLSF_R(r, GX_TARGET, t->target); GLSF_R(r, GX_FORMAT, fmt_); GLSF_R(r, GX_FMT, fmi); GLSF_RC(r, GX_LVIEW, p->lv, "gl.tx_view", tx_view); GLSF_RC(r, GX_SVIEW, p->sv, "gl.tx_view", tx_view);
    GLSF_R(r, GX_SBASE, t->sample_base); GLSF_R(r, GX_SCOUNT, t->sample_count); GLSF_R(r, GX_BASE, t->base); GLSF_R(r, GX_MAX, t->max); GLSF_R(r, GX_IMM, t->immutable);
    GLSF_R(r, GX_GPUW, t->gpu_written); GLSF_R(r, GX_PARAMS, t->params); GLSF_R(r, GX_USWZ, t->uswz);
    for (int f = 0; f < 6; f++) for (int l = 0; l < TEX_LEVELS; l++) { t->level[f][l].format = fmt_[f][l]; t->level[f][l].fmt = fmi[f][l]; }
    t->last_use = 0; p->t = t; return t;
}
static int tx_finish(tx_pend *p, NSArray *roots) {   // 建视图；该有却建不出的回 0
    int ok = 1; gl_texture *t = p->t;
    for (int f = 0; f < 6; f++) for (int l = 0; l < TEX_LEVELS; l++) if (p->lv[f][l].root >= 0) { t->level[f][l].image = tx_view_make(&p->lv[f][l], roots); ok &= t->level[f][l].image != nil; }
    if (p->sv.root >= 0) { t->sample = tx_view_make(&p->sv, roots); ok &= t->sample != nil; }
    return ok;
}
// 弱链接[nvntest]：nvn_snap_tex_save（直连层 graphics/nvn/gfx/nvn/nvn_snap.inc 提供；GL 单测只链 GL 片，链接行按这行放行未定义）
// 弱链接[nvntest]：nvn_snap_tex_load（同上）
static int roots_write(FILE *f, NSArray *roots, uint64_t *nc) {
    if (!nvn_snap_tex_save) return -1;
    uint32_t n = (uint32_t)roots.count; if (fwrite(&n, 4, 1, f) != 1) return -1;
    for (id<MTLTexture> r in roots) { int k = nvn_snap_tex_save(f, (__bridge void *)r); if (k < 0) return -1; if (!k && nc) (*nc)++; }
    return 0;
}
static NSMutableArray *roots_read(FILE *f) {
    if (!nvn_snap_tex_load) return nil;
    uint32_t n; if (fread(&n, 4, 1, f) != 1) return nil; NSMutableArray *a = [NSMutableArray arrayWithCapacity:n];
    for (uint32_t i = 0; i < n; i++) { void *p = nvn_snap_tex_load(f); if (p) [a addObject:(__bridge_transfer id<MTLTexture>)p]; else return nil; }
    return a;
}
int gl_tex_snap_one_save(FILE *f, gl_texture *t) {
    uint8_t has = t != NULL; if (fwrite(&has, 1, 1, f) != 1) return -1; if (!t) return 0;
    NSMutableArray *roots = [NSMutableArray array]; sfw w; sfw_sec(&w, f, "gl.tex1", 1); tx_rec(&w, 0, t, roots);
    return sfw_end(&w) || roots_write(f, roots, NULL) ? -1 : 0;
}
gl_texture *gl_tex_snap_one_load(FILE *f) {
    uint8_t has; if (fread(&has, 1, 1, f) != 1 || !has) return NULL;
    sfr r; tx_pend p = {0}; if (sfr_sec(&r, f, "gl.tex1", 1)) return NULL;
    while (sfr_rec(&r) == 1) if (!p.t) tx_parse(&r, &p);
    sfr_done(&r);
    NSArray *roots = roots_read(f); if (!roots || !p.t) { free_texture(p.t); return NULL; }
    tx_finish(&p, roots); return p.t;
}
int gl_tex_snap_save(FILE *f) {
    NSMutableArray *roots = [NSMutableArray array]; sfw w; sfw_sec(&w, f, "gl.tex", 1); uint32_t n = 0;
    for (GLuint i = 1; i < textures.n; i++) if (textures.v[i]) { tx_rec(&w, i, textures.v[i], roots); n++; }
    if (sfw_end(&w)) return -1;
    uint64_t nc = 0; if (roots_write(f, roots, &nc)) return -1;
    sfw_sec(&w, f, "gl.smp", 1);
    for (GLuint i = 1; i < samplers.n; i++) { gl_sampler *s = samplers.v[i]; if (!s) continue; sfw_rec(&w); GLSF_W(&w, GM_NAME, i); GLSF_W(&w, GM_PARAMS, s->params); sfw_end_rec(&w); }
    if (sfw_end(&w)) return -1;
    sfw_sec(&w, f, "gl.pal", 1); sfw_rec(&w); GLSF_W(&w, GP_PAL, g_palette); GLSF_W(&w, GP_NPAL, g_npalette); GLSF_W(&w, GP_ISHIFT, g_index_shift); GLSF_W(&w, GP_IOFF, g_index_offset); sfw_end_rec(&w);
    GLOG("即时存档：GL 纹理 %u 个、存储 %lu 张（其中只存描述、不存内容 %llu 张）", n, (unsigned long)roots.count, (unsigned long long)nc);
    return sfw_end(&w);
}
int gl_tex_snap_restore(FILE *f) {
    sfr r; if (sfr_sec(&r, f, "gl.tex", 1)) return -1;
    tx_pend *pend = NULL; uint32_t n = 0, cap = 0; int bad = 0;
    while (sfr_rec(&r) == 1) { if (n == cap) { cap = cap ? cap * 2 : 256; pend = realloc(pend, cap * sizeof *pend); } if (tx_parse(&r, &pend[n])) n++; else bad = 1; }
    sfr_done(&r);
    NSArray *roots = bad ? nil : roots_read(f); int nmiss = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!roots || !pend[i].name) { free_texture(pend[i].t); bad = 1; continue; }
        if (!tx_finish(&pend[i], roots)) nmiss++;
        if (!gl_name_put(&textures, pend[i].name, pend[i].t)) { free_texture(pend[i].t); bad = 1; }
    }
    free(pend);
    if (bad || sfr_sec(&r, f, "gl.smp", 1)) return -1;
    while (sfr_rec(&r) == 1) { GLuint nm = 0; GLSF_R(&r, GM_NAME, nm); gl_sampler *s = calloc(1, sizeof *s); if (!nm || !s) { free(s); continue; } s->params = defaults(); GLSF_R(&r, GM_PARAMS, s->params);
        if (!gl_name_put(&samplers, nm, s)) { free(s); bad = 1; } }
    sfr_done(&r);
    if (bad || sfr_sec(&r, f, "gl.pal", 1)) return -1;
    while (sfr_rec(&r) == 1) { GLSF_R(&r, GP_PAL, g_palette); GLSF_R(&r, GP_NPAL, g_npalette); GLSF_R(&r, GP_ISHIFT, g_index_shift); GLSF_R(&r, GP_IOFF, g_index_offset); }
    sfr_done(&r);
    GLOG("%s即时存档恢复：GL 纹理 %u 个、存储 %lu 张%s", nmiss ? "【黄】 " : "", n, (unsigned long)roots.count, nmiss ? [NSString stringWithFormat:@"（%d 个纹理的视图没建出来）", nmiss].UTF8String : "");
    return 0;
}
void gl_tex_snap_fps(gl_fp_cb cb) { cb("gl.tex_params", SNAP_FP_TYPE(tex_params)); cb("gl.tx_view", SNAP_FP_TYPE(tx_view)); }
