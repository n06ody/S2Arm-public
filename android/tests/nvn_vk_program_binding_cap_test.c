// 宿主回归：s2a-direct-program-v1 的 bindings 超出 FD_MAX_BINDINGS 时必须**准确失败**，绝不静默截断。
//
// 反例（F1）：旧写法把解析循环条件写成 `i < n && binding_count < FD_MAX_BINDINGS`，第 17 条起直接
// 不解析、不报错——同一份程序少绑一个资源，画面上只表现为错阴影/缺贴图，没有红行，最难查。
//
// 上限的由来：旧 cap=16 是数组随手定的，与合同无关——8E Wonder 一份合法程序就有 17 条（op=26
// 拒画 → SIGABRT）。新 cap=229 = producer 合同上限：DirectShaderCompiler 图形程序固定 vs+fs
// 两阶段，每阶段至多 18 uniform + 16 storage + 64 texture + 16 image，再加 1 条全程序共享的
// support binding：1 + 2×(18+16+64+16) = 229。越过它就是结果损坏，继续准确失败。
//
// 用法：nvn_vk_program_binding_cap_test <cap|truncate>；退出 0 = 该 case 通过。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn/nvn_vk_program_result.h"

// 最小可解析的 SPIR-V：magic + 4 个字（20 字节），解析器只校验 magic 与长度。
static const char kSpirvB64[] = "AwIjBwABAAAAAAAAAQAAAAAAAAA=";

static char *build_program(int bindings, size_t *out_len) {
    size_t cap = 4096 + (size_t)bindings * 128;
    char *s = (char *)malloc(cap);
    if (!s) return NULL;
    size_t off = (size_t)snprintf(s, cap,
        "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
        "\"stages\":["
        "{\"stage\":\"vs\",\"spirv\":\"%s\"},"
        "{\"stage\":\"fs\",\"spirv\":\"%s\"}],"
        "\"bindings\":[", kSpirvB64, kSpirvB64);
    for (int i = 0; i < bindings; i++)
        off += (size_t)snprintf(s + off, cap - off,
            "%s{\"kind\":\"uniform\",\"set\":0,\"binding\":%d,\"slot\":3,\"stage\":\"vs\"}",
            i ? "," : "", 100 + i);
    off += (size_t)snprintf(s + off, cap - off, "]}}");
    *out_len = off;
    return s;
}

static char *build_compute(int bindings, size_t *out_len) {
    size_t cap = 4096 + (size_t)bindings * 160;
    char *s = (char *)malloc(cap);
    if (!s) return NULL;
    size_t off = (size_t)snprintf(s, cap,
        "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
        "\"contract\":\"s2a-direct-program-content-v1\",\"kind\":\"compute\","
        "\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],"
        "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],"
        "\"bindings\":[", kSpirvB64);
    for (int i = 0; i < bindings; i++)
        off += (size_t)snprintf(s + off, cap - off,
            "%s{\"kind\":\"uniform\",\"set\":0,\"binding\":%d,\"slot\":3,\"count\":1,\"stage\":\"cs\"}",
            i ? "," : "", 100 + i);
    off += (size_t)snprintf(s + off, cap - off,
        "],\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":[]}}");
    *out_len = off;
    return s;
}

static int parse_graphics(int bindings, int *count, char *why, size_t why_size) {
    size_t len = 0;
    char *doc = build_program(bindings, &len);
    struct fd_program p;
    memset(&p, 0, sizeof p);
    why[0] = 0;
    int rc = nvn_vk_program_parse_graphics((const uint8_t *)doc, len, &p, why, why_size);
    *count = p.binding_count;
    nvn_vk_program_free(&p);
    free(doc);
    return rc;
}

static int parse_compute(int bindings, int *count, char *why, size_t why_size) {
    size_t len = 0;
    char *doc = build_compute(bindings, &len);
    struct nvn_vk_compute_program p;
    why[0] = 0;
    int rc = nvn_vk_program_parse_compute((const uint8_t *)doc, len, &p, why, why_size);
    *count = p.binding_count;
    nvn_vk_program_free_compute(&p);
    free(doc);
    return rc;
}

// 期望：17 条（8E Wonder 真实崩溃规模）通过；65 条（评审 moongrant 要求补的常驻边界，证明
// 「16→64」那个中间上限已经不是判据）通过；229 条（合同上限）通过；230 条准确失败（why 带 cap）。
// compute 路径共用同一上限：65 / 229 过、230 拒。
static int case_cap(void) {
    int bad = 0;
    char why[256] = "";
    int count = -1;
    int rc = parse_graphics(17, &count, why, sizeof why);
    printf("  graphics 17 bindings（旧 cap=16 会拒）: rc=%d count=%d why=%s\n", rc, count, rc ? why : "-");
    if (rc != 0 || count != 17) bad = 1;

    // 65 条：PR #1603 评审指出的常驻边界——常驻矩阵里原先只有 17 / 229 / 230，65 只在本支单测里
    // 验过；这里把它钉成常驻用例，避免「17/65/229 过」这句话没有常驻证据。
    rc = parse_graphics(65, &count, why, sizeof why);
    printf("  graphics 65 bindings（旧常量 64 已不是判据）: rc=%d count=%d why=%s\n", rc, count, rc ? why : "-");
    if (rc != 0 || count != 65) bad = 1;

    rc = parse_graphics(FD_MAX_BINDINGS, &count, why, sizeof why);
    printf("  graphics %d bindings（合同上限）: rc=%d count=%d why=%s\n", FD_MAX_BINDINGS, rc, count, rc ? why : "-");
    if (rc != 0 || count != FD_MAX_BINDINGS) bad = 1;

    rc = parse_graphics(FD_MAX_BINDINGS + 1, &count, why, sizeof why);
    printf("  graphics %d bindings（上限+1）: rc=%d count=%d why=%s\n", FD_MAX_BINDINGS + 1, rc, count, rc ? why : "-");
    if (rc == 0 || !strstr(why, "refusing to drop bindings") || !strstr(why, "cap is 343")) bad = 1;

    rc = parse_compute(65, &count, why, sizeof why);
    printf("  compute 65 bindings（旧常量 64 已不是判据）: rc=%d count=%d why=%s\n", rc, count, rc ? why : "-");
    if (rc != 0 || count != 65) bad = 1;

    rc = parse_compute(FD_MAX_BINDINGS, &count, why, sizeof why);
    printf("  compute %d bindings（合同上限）: rc=%d count=%d why=%s\n", FD_MAX_BINDINGS, rc, count, rc ? why : "-");
    if (rc != 0 || count != FD_MAX_BINDINGS) bad = 1;

    rc = parse_compute(FD_MAX_BINDINGS + 1, &count, why, sizeof why);
    printf("  compute %d bindings（上限+1）: rc=%d count=%d why=%s\n", FD_MAX_BINDINGS + 1, rc, count, rc ? why : "-");
    if (rc == 0 || !strstr(why, "binding count exceeds limit")) bad = 1;
    return bad;
}

// 反例说明：把守卫去掉后，cap+1 条会被**静默接收 cap 条**（少一条资源、rc=0）——这就是要修的危害。
// 本用例断言“危险成立”，用来锁住守卫不能删。
static int case_truncate(void) {
    char why[256];
    int count = -1;
    int rc = parse_graphics(FD_MAX_BINDINGS + 1, &count, why, sizeof why);
    printf("  mutated(无守卫) %d bindings: rc=%d count=%d\n", FD_MAX_BINDINGS + 1, rc, count);
    int hazard = (rc == 0 && count == FD_MAX_BINDINGS);   // 成功但少一条 = 静默截断
    return hazard ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "用法: %s <cap|truncate>\n", argv[0]); return 2; }
    if (!strcmp(argv[1], "cap")) return case_cap();
    if (!strcmp(argv[1], "truncate")) return case_truncate();
    fprintf(stderr, "未知 case: %s\n", argv[1]);
    return 2;
}
