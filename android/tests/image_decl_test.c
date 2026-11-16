// 存储图像的声明（nvk_shader.c sampled_decl）：数值类与 SPIR-V Image Format 操作数都要落到程序记录，回放按它们选视图格式。
// 手搭最小 SPIR-V：同一 set 下三个存储图像 —— uint R32ui（SULD.B 按位读的声明）、float Unknown（无格式读 / 写）、sint Rg32i。
#include "../native/gfx_plat/nvk_shader.c"

enum { ID_UINT = 1, ID_FLOAT, ID_SINT, ID_IMG0, ID_IMG1, ID_IMG2, ID_PTR0, ID_PTR1, ID_PTR2, ID_VAR0, ID_VAR1, ID_VAR2, ID_BOUND };
static uint32_t g_w[128]; static size_t g_n;
static void op(uint32_t opcode, int count, ...) {
    va_list ap; va_start(ap, count); g_w[g_n++] = (uint32_t)count << 16 | opcode;
    for (int i = 1; i < count; i++) g_w[g_n++] = va_arg(ap, uint32_t);
    va_end(ap);
}
int main(void) {
    g_w[0] = 0x07230203u; g_w[1] = 0x00010300u; g_w[2] = 0; g_w[3] = ID_BOUND; g_w[4] = 0; g_n = 5;
    const uint32_t vars[3] = {ID_VAR0, ID_VAR1, ID_VAR2};
    for (uint32_t i = 0; i < 3; i++) { op(71, 4, vars[i], 34u, 3u); op(71, 4, vars[i], 33u, i); }   // DescriptorSet 3 / Binding i
    op(21, 4, (uint32_t)ID_UINT, 32u, 0u); op(22, 3, (uint32_t)ID_FLOAT, 32u); op(21, 4, (uint32_t)ID_SINT, 32u, 1u);
    op(25, 9, (uint32_t)ID_IMG0, (uint32_t)ID_UINT, 1u, 0u, 0u, 0u, 2u, 33u);    // uint 2D 存储 R32ui
    op(25, 9, (uint32_t)ID_IMG1, (uint32_t)ID_FLOAT, 1u, 0u, 0u, 0u, 2u, 0u);    // float 2D 存储 Unknown
    op(25, 9, (uint32_t)ID_IMG2, (uint32_t)ID_SINT, 1u, 0u, 0u, 0u, 2u, 25u);    // sint 2D 存储 Rg32i
    op(32, 4, (uint32_t)ID_PTR0, 0u, (uint32_t)ID_IMG0); op(32, 4, (uint32_t)ID_PTR1, 0u, (uint32_t)ID_IMG1); op(32, 4, (uint32_t)ID_PTR2, 0u, (uint32_t)ID_IMG2);
    op(59, 4, (uint32_t)ID_PTR0, (uint32_t)ID_VAR0, 0u); op(59, 4, (uint32_t)ID_PTR1, (uint32_t)ID_VAR1, 0u); op(59, 4, (uint32_t)ID_PTR2, (uint32_t)ID_VAR2, 0u);
    static const struct { uint8_t cls, fmt; } want[3] = {{1, 33}, {0, 0}, {2, 25}};
    int bad = 0;
    for (uint32_t b = 0; b < 3; b++) {
        uint8_t dtt = 0, dcls = 9, ifmt = 99;
        sampled_decl(g_w, g_n * 4, 3, b, &dtt, &dcls, &ifmt);
        int ok = dtt == NVN_TEXTURE_TARGET_2D + 1 && dcls == want[b].cls && ifmt == want[b].fmt;
        fprintf(stderr, "%s: storage image binding %u -> target %u class %u format %u (want class %u format %u)\n", ok ? "PASS" : "FAIL", b, dtt, dcls, ifmt, want[b].cls, want[b].fmt);
        bad += !ok;
    }
    uint8_t dtt = 0, dcls = 9;
    sampled_decl(g_w, g_n * 4, 3, 0, &dtt, &dcls, NULL);   // 贴图路径不要格式：传 NULL 不得写
    if (dtt != NVN_TEXTURE_TARGET_2D + 1 || dcls != 1) { fprintf(stderr, "FAIL: NULL format output breaks the class answer\n"); bad++; }
    return bad ? 1 : 0;
}
