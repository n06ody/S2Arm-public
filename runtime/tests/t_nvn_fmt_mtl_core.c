// D7 B 续：Metal 映射（nvn_fmt_mtl.c）是纯 C 表，可在 Linux 上单独编译验证。
// 期望值按映射表手算：RGBA8→RGBA8Unorm(70)、RGBA8UI→RGBA8Uint(73)、sRGB→71、
// R16F→25、D32F→252、Stencil8→253、D32FS8→260、D24→替换 252、BC1→130、ASTC→204/186；上传时展开类（ret 3）与逐像素展开见各条注释。
#include "nvn_fmt_mtl.h"
#include "nvn_fmt_info.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;

static void want_map(const char *what, int f, int ret, uint32_t mtl) {
    uint32_t m = 0xdeadbeefu;
    uint8_t swz[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    int r = nvn_fmt_to_mtl(f, &m, swz);
    checks++;
    if (r != ret || (ret != 0 && m != mtl)) {
        fails++;
        printf("FAIL %s：0x%02X ret %d mtl %u，期望 ret %d mtl %u\n", what, f, r, m, ret, mtl);
    }
    checks++;
    if (ret != 0 && (swz[0] == 0xAA || swz[3] == 0xAA)) {
        fails++;
        printf("FAIL %s：0x%02X swz 未填\n", what, f);
    }
}

int main(void) {
    want_map("RGBA8", 0x25, 1, 70);
    want_map("RGBA8UI", 0x27, 1, 73);
    want_map("RGBA8_SRGB", 0x38, 1, 71);
    want_map("R16F", 0x05, 1, 25);
    want_map("DEPTH32F", 0x34, 1, 252);
    want_map("STENCIL8", 0x31, 1, 253);
    want_map("DEPTH32F_STENCIL8", 0x36, 1, 260);
    want_map("DEPTH24 替换", 0x33, 2, 252);
    want_map("RGB_DXT1", 0x41, 1, 130);
    want_map("RGB10A2", 0x3D, 1, 90);
    want_map("RGB10A2UI", 0x3E, 1, 91);
    want_map("ASTC_LDR 4x4", 0x79, 1, 204);
    want_map("ASTC_SRGB 4x4", 0x87, 1, 186);
    // 上传时展开（ret 3，缺口族 30）：三分量 → 同类四分量；*UI2F / *I2F → 浮点（8 位进半精度、16 / 32 位进单精度）；RGB10A2 有符号 → 16 位
    want_map("RGB8 展开", 0x19, 3, 70);
    want_map("RGB16F 展开", 0x1D, 3, 115);
    want_map("RGB32UI 展开", 0x23, 3, 123);
    want_map("R8_UI2F 转半精度", 0x51, 3, 25);
    want_map("R16_UI2F 转单精度", 0x53, 3, 55);
    want_map("RG8_I2F 转半精度", 0x58, 3, 65);
    want_map("RGB8_UI2F 四分量半精度", 0x5D, 3, 115);
    want_map("RGBA32_I2F 转单精度", 0x68, 3, 125);
    want_map("RGB10A2SN 转 RGBA16Snorm", 0x69, 3, 112);
    want_map("RGB10A2I 转 RGBA16Sint", 0x6A, 3, 114);
    want_map("RGB10A2_UI2F 转半精度", 0x6B, 3, 115);
    want_map("未知 0x9D", 0x9D, 0, 0);
    want_map("未知 0xFF", 0xFF, 0, 0);
    want_map("0 不是格式", 0x00, 0, 0);

    // RGBX8_SRGB 无 alpha：swz.a = ONE(1)
    uint8_t swz[4];
    uint32_t m = 0;
    int r = nvn_fmt_to_mtl(0x37, &m, swz);
    checks++;
    if (r != 1 || m != 71 || swz[3] != NVN_SWZ_ONE) { fails++; printf("FAIL RGBX8_SRGB noalpha：r %d mtl %u swz.a %u\n", r, m, swz[3]); }

    // 逐像素展开（nvn_fmt_expand）：期望按格式定义手算，不调被测函数推
    {
        struct { const char *what; uint32_t f; uint8_t src[16]; int sn; uint8_t want[16]; int wn; } X[] = {
            {"RGB8 补 a=0xFF", 0x19, {10, 20, 30}, 3, {10, 20, 30, 0xFF}, 4},
            {"RGB8SN 补 a=0x7F", 0x1A, {0x80, 1, 2}, 3, {0x80, 1, 2, 0x7F}, 4},
            {"RGB16F 补 a=1.0（0x3C00）", 0x1D, {1, 2, 3, 4, 5, 6}, 6, {1, 2, 3, 4, 5, 6, 0x00, 0x3C}, 8},
            {"RGB32UI 补 a=1", 0x23, {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0}, 12, {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0}, 16},
            {"R8_UI2F 200 → 200.0 半精度 0x5A40", 0x51, {200}, 1, {0x40, 0x5A}, 2},
            {"R8_I2F -1 → -1.0 半精度 0xBC00", 0x52, {0xFF}, 1, {0x00, 0xBC}, 2},
            {"R16_UI2F 65535 → 65535.0f（0x477FFF00）", 0x53, {0xFF, 0xFF}, 2, {0x00, 0xFF, 0x7F, 0x47}, 4},
            {"RGB8_UI2F (1,2,3) → (1,2,3,1) 半精度", 0x5D, {1, 2, 3}, 3, {0x00, 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x3C}, 8},
            // RGB10A2SN：r=511 → 32767，g=-512 → -32768（采样为 -1），b=0 → 0，a=-1（两位 0b11）→ -32767
            {"RGB10A2SN 端点", 0x69, {0xFF, 0x01, 0x08, 0xC0}, 4, {0xFF, 0x7F, 0x00, 0x80, 0x00, 0x00, 0x01, 0x80}, 8},
            // r=-511、g=-512，采样均为 -1，原码分别保留；a=-2 也取最小原码。
            {"RGB10A2SN 相邻负端原码", 0x69, {0x01, 0x02, 0x08, 0x80}, 4, {0x01, 0x80, 0x00, 0x80, 0, 0, 0, 0x80}, 8},
            // RGB10A2I：r=-5（0x3FB），g=7，b=0，a=1 → int16
            {"RGB10A2I 符号扩展", 0x6A, {0xFB, 0x1F, 0x00, 0x40}, 4, {0xFB, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00}, 8},
            // RGB10A2_UI2F：r=1023 → 1023.0（0x63FE），g=b=0，a=3 → 3.0（0x4200）
            {"RGB10A2_UI2F", 0x6B, {0xFF, 0x03, 0x00, 0xC0}, 4, {0xFE, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42}, 8},
        };
        for (unsigned i = 0; i < sizeof X / sizeof *X; i++) {
            uint8_t out[16]; memset(out, 0xAA, sizeof out);
            int rc = nvn_fmt_expand(X[i].f, X[i].src, (size_t)X[i].sn, 1, 1, out, (size_t)X[i].wn);
            checks++;
            if (rc || memcmp(out, X[i].want, (size_t)X[i].wn)) {
                fails++; printf("FAIL 展开 %s：rc %d，得", X[i].what, rc);
                for (int k = 0; k < X[i].wn; k++) printf(" %02X", out[k]);
                printf("\n");
            }
        }
        uint8_t o[4]; checks++;
        if (nvn_fmt_expand(0x25, (const uint8_t *)"abcd", 4, 1, 1, o, 4) != -1) { fails++; printf("FAIL 不展开的格式（RGBA8）应返回 -1\n"); }
    }

    // 全码：0x00–0xFF 不崩；中立表没有的格式必须返回 0
    int known = 0, unknown = 0;
    for (int f = 0; f <= 0xFF; f++) {
        nvn_fmt_info info;
        int in_table = nvn_fmt_info_of((uint32_t)f, &info);
        r = nvn_fmt_to_mtl(f, &m, swz);
        checks++;
        if (r < 0 || r > 3) { fails++; printf("FAIL 0x%02X 返回 %d\n", f, r); }
        if (!in_table && r != 0) { fails++; printf("FAIL 0x%02X 不在中立表却返回 %d\n", f, r); }
        if (in_table) known++; else unknown++;
    }
    checks++;
    if (known != NVN_FMT_COUNT) { fails++; printf("FAIL 中立表登记 %d，期望 %d\n", known, NVN_FMT_COUNT); }
    printf("（扫描 0x00–0xFF：中立表内 %d 个、表外 %d 个）\n", known, unknown);

    if (fails) printf("✗ t_nvn_fmt_mtl_core：%d / %d 项不符\n", fails, checks);
    else printf("✓ t_nvn_fmt_mtl_core：Metal 映射 %d 项全过（0 处不符）\n", checks);
    return fails ? 1 : 0;
}
