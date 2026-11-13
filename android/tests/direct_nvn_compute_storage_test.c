// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// #task-7 L 级（device-free）：compute storage 绑定的「地址来源」解析回归。
//
// 只链生产 `nvn_vk_program_result.c` 的 `nvn_vk_program_parse_compute`（+ 通用 JSON 校验器
// `direct/json.c`），不建 Vulkan 设备、不链 compute 运行期。用最小 s2a-direct-program-v1
// compute 结果 JSON 驱动，钉住 `compute_storage()` 的映射规则：
//
//   A. 合法来源（字段逐项钉）：
//      NVN         cbSlot=0, cbOffset=0x310+16*r, nvnSlot=r
//                  → mapping=NVK_STORAGE_NVN，index=r，offset=0，cb_slot/cb_offset/rw 原样保留。
//      UBO_PTR     cbSlot=0, cbOffset=0x210+16*r, uboPtr=r
//                  → mapping=NVK_STORAGE_UBO_PTR，index=r（驱动 c0 的 UBO 表别名来源）。
//      UBO_ADDRESS cbSlot>=3, uboSlot=cbSlot-3, uboOffset=cbOffset
//                  → mapping=NVK_STORAGE_UBO_ADDRESS，index=uboSlot(=cbSlot-3)，offset=cbOffset。
//      同一程序内多条 storage 绑定各按自己的字段独立映射。
//   B. 反例（必须 rc!=0 且 why 可定位）：nvnSlot/uboPtr 与 cbOffset 不一致、三种来源同时出现、
//      nvnSlot 带多余 uboOffset、uboSlot 缺 uboOffset、cbOffset 非 4 字节对齐、NVN/UBO_PTR 表项
//      cbOffset 非 16 对齐、缺 rw、rw 非布尔、nvnSlot>=16、cbSlot>255、cbOffset 超出
//      65535*4、UBO_ADDRESS 的 uboSlot>=16 / uboOffset!=cbOffset、没有来源字段、storage 绑定
//      夹带 guest `slot` 字段。
//   C. supportReads / c0Reads 的 null 接受路径（JSON null = 编译器分析不出读集）：
//      接受、`*_reads_unknown=1`、count=0；空数组是“已证明 0 条读”而非 unknown；非数组非 null
//      与缺失字段仍然拒绝。null 不影响同一程序里 bindings 的 storage 来源解析。
//
// cbOffset 对齐口径（已与 Lead 确认，这是正确语义，不是 bug）：
//   全局先拒绝 `cbOffset & 3`（非 4 字节对齐）。NVN / UBO_PTR 是驱动 c0 里的**表项**：
//   0x310+16*r / 0x210+16*r，每项 16 字节，所以表项 cbOffset 必须 16 对齐，且
//   nvnSlot/uboPtr == (cbOffset-base)/16 是“表项索引一致性”判据。UBO_ADDRESS 是“用户 UBO
//   体内的 SbDescriptor 字节偏移”（不是表项），只是一个字节偏移，4 字节对齐即可——descriptor
//   的 lo/hi/size 都按 4 字节读取。所以 `cbSlot=3, cbOffset=68, uboSlot=0, uboOffset=68`
//   （4 对齐、非 16）被接受是正确行为（[A4]）；`cbOffset=66`（非 4 对齐）必须拒绝（[B21]）。
//
// 不证明（留给运行期/其它批）：nvn_vk_compute.c 对 mapping 的消费（NVN/UBO_PTR 的 guest 槽、
// overlap/对齐/尺寸上限；UBO_ADDRESS 当前运行期仍报 unsupported）、graphics 侧解析、c0/support
// 读数组的内容校验、设备行为。
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvn/nvn_vk_program_result.h"
#include "tbuf_spirv_fixtures.h"

// 最小合法 SPIR-V：5 个头部 word（magic=0x07230203、version 1.3、generator 0、bound 1、schema 0）。
// 生产 compute 解析只要求 base64 解码后 >=20 字节、4 字节对齐、magic 正确。
static const char *const k_spirv = "AwIjBwABAwAAAAAAAQAAAAAAAAA=";

static int fails;
static int checks;

// ---------- 断言：每条失败打 “✗ <定位>：期望 …，真结果 …” ----------

static void failf(const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("✗ %s：", label);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
    fails++;
}

static void ck_true(const char *label, int ok, const char *expect) {
    checks++;
    if (!ok) failf(label, "期望 %s，真结果 不成立", expect);
}

static void ck_u32(const char *label, uint32_t got, uint32_t want) {
    checks++;
    if (got != want) failf(label, "期望 %u，真结果 %u", (unsigned)want, (unsigned)got);
}

static void ck_i32(const char *label, int32_t got, int32_t want) {
    checks++;
    if (got != want) failf(label, "期望 %d，真结果 %d", (int)want, (int)got);
}

static void group(const char *name) {
    printf("… %s\n", name);
    fflush(stdout);
}

// ---------- 结果 JSON 骨架 ----------

static const char *mapping_name(enum nvn_vk_storage_mapping m) {
    switch (m) {
        case NVK_STORAGE_NVN: return "NVK_STORAGE_NVN";
        case NVK_STORAGE_UBO_PTR: return "NVK_STORAGE_UBO_PTR";
        case NVK_STORAGE_UBO_ADDRESS: return "NVK_STORAGE_UBO_ADDRESS";
        default: return "NVK_STORAGE_UNKNOWN";
    }
}

// 最小 compute 结果骨架：schema/contract/kind/hasUnalignedStorageBuffer/c0Reads/stages(1 个 cs)/
// bindings/supportLayout/supportReads。bindings、两个 reads 由用例给原文（可传 ""、null、数组）。
static size_t build_json(char *buf, size_t cap, const char *bindings,
                         const char *c0_reads, const char *support_reads) {
    int n = snprintf(buf, cap,
        "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
        "\"contract\":\"s2a-direct-program-content-v1\",\"kind\":\"compute\","
        "\"hasUnalignedStorageBuffer\":false,\"c0Reads\":%s,"
        "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],"
        "\"bindings\":[%s],"
        "\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":%s}}",
        c0_reads, k_spirv, bindings, support_reads);
    if (n <= 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

// 一个 compute storage binding：fields 是 "kind":"storage" 之后、结尾 '}' 之前的原文。
static void binding_json(char *out, size_t cap, unsigned set, unsigned binding, const char *fields) {
    snprintf(out, cap,
             "{\"stage\":\"cs\",\"kind\":\"storage\",\"set\":%u,\"binding\":%u,\"count\":1,%s}",
             set, binding, fields);
}

// 解析：返回生产 rc（0=接受）。p 由调用方释放（nvn_vk_program_free_compute）。
static int parse_case(const char *bindings, const char *c0_reads, const char *support_reads,
                      struct nvn_vk_compute_program *p, char *why, size_t why_size) {
    char json[8192];
    size_t n = build_json(json, sizeof json, bindings, c0_reads, support_reads);
    memset(p, 0, sizeof *p);
    if (!n) {
        snprintf(why, why_size, "test fixture JSON does not fit");
        return -1;
    }
    return nvn_vk_program_parse_compute((const uint8_t *)json, n, p, why, why_size);
}

// 期望接受；返回 0 表示 p 已填（调用方 free），-1 已打红。
static int accept_case(const char *label, const char *bindings, const char *c0_reads,
                       const char *support_reads, struct nvn_vk_compute_program *p) {
    char why[256] = "";
    int rc;
    checks++;
    rc = parse_case(bindings, c0_reads, support_reads, p, why, sizeof why);
    if (rc != 0) {
        failf(label, "期望 接受（rc=0），真结果 拒绝（rc=%d）why=\"%s\"", rc, why);
        return -1;
    }
    return 0;
}

// 期望拒绝：rc!=0、why 非空且含 keyword（可定位）。每次算两项检查。
static void reject_case(const char *label, const char *bindings, const char *c0_reads,
                        const char *support_reads, const char *keyword) {
    struct nvn_vk_compute_program p;
    char why[256] = "";
    int rc;

    checks++;
    rc = parse_case(bindings, c0_reads, support_reads, &p, why, sizeof why);
    if (rc == 0) {
        failf(label, "期望 拒绝（rc!=0），真结果 接受（rc=0）");
        nvn_vk_program_free_compute(&p);
        return;
    }
    checks++;
    if (!why[0])
        failf(label, "期望 why 写明拒绝原因（含 \"%s\"），真结果 空串", keyword);
    else if (!strstr(why, keyword))
        failf(label, "期望 why 含 \"%s\"，真结果 \"%s\"", keyword, why);
    nvn_vk_program_free_compute(&p);
}

// 钉住一条 storage 映射的字段：cb_slot/cb_offset/rw/mapping/index/offset + binding kind/stage。
static void ck_storage(const char *label, const struct nvn_vk_compute_program *p, int i,
                       uint32_t cb_slot, uint32_t cb_offset, int writable,
                       enum nvn_vk_storage_mapping map, uint32_t index, uint32_t offset) {
    char l[224];
    snprintf(l, sizeof l, "%s binding[%d].kind", label, i);
    ck_i32(l, p->bindings[i].kind, 1);
    snprintf(l, sizeof l, "%s binding[%d].stage", label, i);
    ck_true(l, !strcmp(p->bindings[i].stage, "cs"), "stage=\"cs\"");
    snprintf(l, sizeof l, "%s storage[%d].cb_slot", label, i);
    ck_u32(l, p->storage[i].cb_slot, cb_slot);
    snprintf(l, sizeof l, "%s storage[%d].cb_offset", label, i);
    ck_u32(l, p->storage[i].cb_offset, cb_offset);
    snprintf(l, sizeof l, "%s storage[%d].writable", label, i);
    ck_i32(l, p->storage[i].writable, writable);
    snprintf(l, sizeof l, "%s storage[%d].mapping", label, i);
    checks++;
    if (p->storage[i].mapping != map)
        failf(l, "期望 %s(%d)，真结果 %s(%d)", mapping_name(map), (int)map,
              mapping_name(p->storage[i].mapping), (int)p->storage[i].mapping);
    snprintf(l, sizeof l, "%s storage[%d].index", label, i);
    ck_u32(l, p->storage[i].index, index);
    snprintf(l, sizeof l, "%s storage[%d].offset", label, i);
    ck_u32(l, p->storage[i].offset, offset);
}

// ---------- A. 合法来源解析 ----------

static void case_accept_nvn(void) {
    struct nvn_vk_compute_program p;
    char b[512];
    group("[A1] NVN 来源：cbSlot=0, cbOffset=0x310+16*r, nvnSlot=r");

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0");
    if (accept_case("[A1] NVN r=0 @0x310", b, "[]", "[]", &p) == 0) {
        ck_storage("[A1] NVN r=0", &p, 0, 0, 0x310, 1, NVK_STORAGE_NVN, 0, 0);
        ck_i32("[A1] storage binding 不占用 guest slot 字段（slot 必须 -1）", p.bindings[0].slot, -1);
        nvn_vk_program_free_compute(&p);
    }
    binding_json(b, sizeof b, 2, 5, "\"rw\":false,\"cbSlot\":0,\"cbOffset\":1024,\"nvnSlot\":15");
    if (accept_case("[A1] NVN r=15 @0x400（区间上界内最后一个槽）", b, "[]", "[]", &p) == 0) {
        ck_storage("[A1] NVN r=15", &p, 0, 0, 0x400, 0, NVK_STORAGE_NVN, 15, 0);
        nvn_vk_program_free_compute(&p);
    }
}

static void case_accept_ubo_ptr(void) {
    struct nvn_vk_compute_program p;
    char b[512];
    group("[A2] UBO_PTR 来源：cbSlot=0, cbOffset=0x210+16*r, uboPtr=r（驱动 c0 UBO 表别名）");

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":528,\"uboPtr\":0");
    if (accept_case("[A2] UBO_PTR r=0 @0x210", b, "[]", "[]", &p) == 0) {
        ck_storage("[A2] UBO_PTR r=0", &p, 0, 0, 0x210, 1, NVK_STORAGE_UBO_PTR, 0, 0);
        nvn_vk_program_free_compute(&p);
    }
    binding_json(b, sizeof b, 1, 0, "\"rw\":false,\"cbSlot\":0,\"cbOffset\":768,\"uboPtr\":15");
    if (accept_case("[A2] UBO_PTR r=15 @0x300（区间上界内最后一个槽）", b, "[]", "[]", &p) == 0) {
        ck_storage("[A2] UBO_PTR r=15", &p, 0, 0, 0x300, 0, NVK_STORAGE_UBO_PTR, 15, 0);
        nvn_vk_program_free_compute(&p);
    }
}

static void case_accept_ubo_address(void) {
    struct nvn_vk_compute_program p;
    char b[512];
    group("[A3] UBO_ADDRESS 来源：cbSlot>=3, uboSlot=cbSlot-3, uboOffset=cbOffset");

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":3,\"cbOffset\":64,\"uboSlot\":0,\"uboOffset\":64");
    if (accept_case("[A3] UBO_ADDRESS cbSlot=3 uboSlot=0 off=64", b, "[]", "[]", &p) == 0) {
        ck_storage("[A3] UBO_ADDRESS cb3/ubo0", &p, 0, 3, 64, 1, NVK_STORAGE_UBO_ADDRESS, 0, 64);
        nvn_vk_program_free_compute(&p);
    }
    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":18,\"cbOffset\":40,\"uboSlot\":15,\"uboOffset\":40");
    if (accept_case("[A3] UBO_ADDRESS cbSlot=18 uboSlot=15（UBO 槽上界）", b, "[]", "[]", &p) == 0) {
        ck_storage("[A3] UBO_ADDRESS cb18/ubo15", &p, 0, 18, 40, 1, NVK_STORAGE_UBO_ADDRESS, 15, 40);
        nvn_vk_program_free_compute(&p);
    }
    // UBO_ADDRESS 是“用户 UBO 体内的 SbDescriptor 字节偏移”，不是 16 字节表项：只要求 4 字节
    // 对齐。cbOffset=68（4 对齐、非 16）被接受是正确行为；cbOffset=66 必须拒绝（见 [B21]）。
    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":3,\"cbOffset\":68,\"uboSlot\":0,\"uboOffset\":68");
    if (accept_case("[A4] UBO_ADDRESS cbOffset=68（UBO 内偏移只要求 4 字节对齐）接受", b, "[]", "[]",
                    &p) == 0) {
        ck_storage("[A4] UBO_ADDRESS 4 字节对齐", &p, 0, 3, 68, 1, NVK_STORAGE_UBO_ADDRESS, 0, 68);
        nvn_vk_program_free_compute(&p);
    }
}

static void case_accept_multiple(void) {
    struct nvn_vk_compute_program p;
    char b[2048];
    group("[A5] 同一程序内多条 storage 绑定按各自来源独立映射");

    snprintf(b, sizeof b,
        "{\"stage\":\"cs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,\"count\":1,"
        "\"rw\":true,\"cbSlot\":0,\"cbOffset\":896,\"nvnSlot\":7},"
        "{\"stage\":\"cs\",\"kind\":\"storage\",\"set\":1,\"binding\":1,\"count\":1,"
        "\"rw\":false,\"cbSlot\":0,\"cbOffset\":528,\"uboPtr\":0},"
        "{\"stage\":\"cs\",\"kind\":\"storage\",\"set\":1,\"binding\":2,\"count\":1,"
        "\"rw\":true,\"cbSlot\":3,\"cbOffset\":64,\"uboSlot\":0,\"uboOffset\":64}");
    if (accept_case("[A5] 三条 storage 来源", b, "[]", "[]", &p) == 0) {
        ck_u32("[A5] binding 数量", (uint32_t)p.binding_count, 3);
        ck_storage("[A5] 第 1 条 NVN", &p, 0, 0, 896, 1, NVK_STORAGE_NVN, 7, 0);
        ck_storage("[A5] 第 2 条 UBO_PTR", &p, 1, 0, 528, 0, NVK_STORAGE_UBO_PTR, 0, 0);
        ck_storage("[A5] 第 3 条 UBO_ADDRESS", &p, 2, 3, 64, 1, NVK_STORAGE_UBO_ADDRESS, 0, 64);
        nvn_vk_program_free_compute(&p);
    }
}

// ---------- B. 反例 ----------

static void case_reject_storage(void) {
    char b[640];
    // 所有映射失败都走 compute_storage() 的 -1，调用方统一写
    // “compute storage source mapping is unknown or inconsistent”；why 关键字取 "compute storage"。
    static const char *const keyword = "compute storage";
    group("[B] storage 来源反例（必须 rc!=0 且 why 可定位）");

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":1");
    reject_case("[B1] nvnSlot 与 cbOffset 不一致", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":528,\"uboPtr\":1");
    reject_case("[B2] uboPtr 与 cbOffset 不一致", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0,\"uboPtr\":0,"
                 "\"uboSlot\":0,\"uboOffset\":784");
    reject_case("[B3] nvnSlot+uboPtr+uboSlot 三种来源同时出现", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0,\"uboOffset\":784");
    reject_case("[B4] 带 nvnSlot 却夹带 uboOffset（uboSlot 缺失）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":3,\"cbOffset\":64,\"uboSlot\":0");
    reject_case("[B5] uboSlot 缺 uboOffset", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":3,\"cbOffset\":64,\"uboOffset\":64");
    reject_case("[B6] uboOffset 缺 uboSlot", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":788,\"nvnSlot\":0");
    reject_case("[B7] NVN 表项 cbOffset=0x314 未按 16 对齐", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":532,\"uboPtr\":0");
    reject_case("[B8] UBO_PTR 表项 cbOffset=0x214 未按 16 对齐", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":786,\"nvnSlot\":0");
    reject_case("[B9] cbOffset 非 4 字节对齐", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0");
    reject_case("[B10] 缺少 rw（不能静默当只读）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":1,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0");
    reject_case("[B11] rw 非布尔（数字 1）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":\"false\",\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0");
    reject_case("[B12] rw 非布尔（字符串 \"false\"）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":1040,\"nvnSlot\":16");
    reject_case("[B13] nvnSlot>=16（16 @0x410，超出 NVN 表区间）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":16");
    reject_case("[B14] nvnSlot>=16（16 @0x310，与 cbOffset 也不一致）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":256,\"cbOffset\":64,\"uboSlot\":253,\"uboOffset\":64");
    reject_case("[B15] cbSlot>255", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":3,\"cbOffset\":262144,\"uboSlot\":0,\"uboOffset\":262144");
    reject_case("[B16] cbOffset>65535*4（越界）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":19,\"cbOffset\":40,\"uboSlot\":16,\"uboOffset\":40");
    reject_case("[B17] UBO_ADDRESS uboSlot>=16（cbSlot=19）", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":3,\"cbOffset\":64,\"uboSlot\":0,\"uboOffset\":68");
    reject_case("[B18] UBO_ADDRESS uboOffset 与 cbOffset 不一致", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784");
    reject_case("[B19] 没有 nvnSlot/uboPtr/uboSlot 来源字段", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0,\"slot\":7");
    reject_case("[B20] storage 绑定夹带 guest slot 字段", b, "[]", "[]", keyword);

    binding_json(b, sizeof b, 1, 0,
                 "\"rw\":true,\"cbSlot\":3,\"cbOffset\":66,\"uboSlot\":0,\"uboOffset\":66");
    reject_case("[B21] UBO_ADDRESS cbOffset=66 非 4 字节对齐", b, "[]", "[]", keyword);
}

// ---------- C. supportReads / c0Reads 的 null 接受路径 ----------

static void case_reads_null(void) {
    struct nvn_vk_compute_program p;
    char b[512];
    group("[C1] supportReads/c0Reads=null：接受并把“读集未知”记进程序对象");

    if (accept_case("[C1] 基线：两个 reads 都是空数组", "", "[]", "[]", &p) == 0) {
        ck_i32("[C1] 空数组 c0_reads_unknown=0", p.c0_reads_unknown, 0);
        ck_u32("[C1] 空数组 c0_read_count=0", p.c0_read_count, 0);
        ck_i32("[C1] 空数组 support_reads_unknown=0", p.support_reads_unknown, 0);
        ck_u32("[C1] 空数组 support_read_count=0", p.support_read_count, 0);
        nvn_vk_program_free_compute(&p);
    }

    if (accept_case("[C1] c0Reads=null", "", "null", "[]", &p) == 0) {
        ck_i32("[C1] c0Reads=null 置 c0_reads_unknown", p.c0_reads_unknown, 1);
        ck_u32("[C1] c0Reads=null 时 c0_read_count=0", p.c0_read_count, 0);
        ck_i32("[C1] c0Reads=null 不影响 support_reads_unknown", p.support_reads_unknown, 0);
        nvn_vk_program_free_compute(&p);
    }

    if (accept_case("[C1] supportReads=null", "", "[]", "null", &p) == 0) {
        ck_i32("[C1] supportReads=null 置 support_reads_unknown", p.support_reads_unknown, 1);
        ck_u32("[C1] supportReads=null 时 support_read_count=0", p.support_read_count, 0);
        ck_i32("[C1] supportReads=null 不影响 c0_reads_unknown", p.c0_reads_unknown, 0);
        nvn_vk_program_free_compute(&p);
    }

    if (accept_case("[C1] 两个 reads 都=null", "", "null", "null", &p) == 0) {
        ck_i32("[C1] 两个 null：c0_reads_unknown", p.c0_reads_unknown, 1);
        ck_i32("[C1] 两个 null：support_reads_unknown", p.support_reads_unknown, 1);
        ck_u32("[C1] 两个 null：两个 count 都为 0", p.c0_read_count + p.support_read_count, 0);
        nvn_vk_program_free_compute(&p);
    }

    // null 只表示“读集未知”，不能顺带跳过 bindings 的来源解析。
    binding_json(b, sizeof b, 1, 0, "\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0");
    if (accept_case("[C1] c0Reads=null + 一条合法 NVN storage 绑定", b, "null", "null", &p) == 0) {
        ck_u32("[C1] null 读集下 binding 数量", (uint32_t)p.binding_count, 1);
        ck_storage("[C1] null 读集下 NVN 映射仍解析", &p, 0, 0, 0x310, 1, NVK_STORAGE_NVN, 0, 0);
        nvn_vk_program_free_compute(&p);
    }

    // supportReads=null 且带 support binding：null 在 support 读集校验之前被接受，
    // support binding 本身仍按 kind/stage 解析（运行期负责按恒等值填满 SupportBuffer）。
    if (accept_case("[C1] supportReads=null + support binding",
                    "{\"stage\":\"program\",\"kind\":\"support\",\"set\":0,\"binding\":0,\"count\":1}",
                    "[]", "null", &p) == 0) {
        ck_u32("[C1] support binding 数量", (uint32_t)p.binding_count, 1);
        ck_i32("[C1] support binding is_support", p.bindings[0].is_support, 1);
        ck_i32("[C1] supportReads=null 的 unknown 位", p.support_reads_unknown, 1);
        nvn_vk_program_free_compute(&p);
    }
}

static void case_reads_reject_non_array(void) {
    struct nvn_vk_compute_program p;
    char why[256] = "";

    group("[C2] 非数组非 null / 缺失的 reads 仍然拒绝");

    reject_case("[C2] c0Reads=7（数字，非数组非 null）", "", "7", "[]", "c0 reads");
    reject_case("[C2] c0Reads=\"x\"（字符串，非数组非 null）", "", "\"x\"", "[]", "c0 reads");
    reject_case("[C2] c0Reads=true（布尔，非数组非 null）", "", "true", "[]", "c0 reads");
    reject_case("[C2] supportReads=7（数字，非数组非 null）", "", "[]", "7", "support reads");
    reject_case("[C2] supportReads={}（对象，非数组非 null）", "", "[]", "{}", "support reads");

    // 缺失字段 ≠ null：必须准确拒绝，不能静默当成“读集未知”。
    checks++;
    memset(&p, 0, sizeof p);
    {
        char json[8192];
        int n = snprintf(json, sizeof json,
            "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
            "\"contract\":\"s2a-direct-program-content-v1\",\"kind\":\"compute\","
            "\"hasUnalignedStorageBuffer\":false,"
            "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],\"bindings\":[],"
            "\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":[]}}", k_spirv);
        int rc = nvn_vk_program_parse_compute((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
        if (rc == 0) failf("[C2] 缺失 c0Reads", "期望 拒绝（rc!=0），真结果 接受（rc=0）");
        else if (!strstr(why, "c0 reads")) failf("[C2] 缺失 c0Reads 的 why", "期望 含 \"c0 reads\"，真结果 \"%s\"", why);
        nvn_vk_program_free_compute(&p);
    }
    checks++;
    memset(&p, 0, sizeof p);
    {
        char json[8192];
        int n = snprintf(json, sizeof json,
            "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
            "\"contract\":\"s2a-direct-program-content-v1\",\"kind\":\"compute\","
            "\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],"
            "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],\"bindings\":[],"
            "\"supportLayout\":{\"requiredSize\":2608}}}", k_spirv);
        int rc = nvn_vk_program_parse_compute((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
        if (rc == 0) failf("[C2] 缺失 supportReads", "期望 拒绝（rc!=0），真结果 接受（rc=0）");
        else if (!strstr(why, "support reads")) failf("[C2] 缺失 supportReads 的 why", "期望 含 \"support reads\"，真结果 \"%s\"", why);
        nvn_vk_program_free_compute(&p);
    }
}

int main(void) {
    case_accept_nvn();
    case_accept_ubo_ptr();
    case_accept_ubo_address();
    case_accept_multiple();
    case_reject_storage();
    case_reads_null();
    case_reads_reject_non_array();
    {
        struct nvn_vk_compute_program p; char why[192] = "";
        int rc = parse_case("{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":0,\"count\":524288}", "[]", "[]", &p, why, sizeof why);
        ck_true("large compute array rejected with exact stage/count", rc == NVK_PROGRAM_ARRAY_UNSUPPORTED && strstr(why, "524288") && strstr(why, "cs"), why);
        nvn_vk_program_free_compute(&p);
    }

    {
        char json[4096], why[192] = ""; struct nvn_vk_compute_program p;
        int n = snprintf(json, sizeof json,
            "{\"schema\":\"s2a-direct-program-v2\",\"content\":{"
            "\"contract\":\"s2a-direct-program-content-v2\",\"kind\":\"compute\","
            "\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],"
            "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],"
            "\"bindings\":[{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":344,\"samp_slot\":127,\"count\":1}],"
            "\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":[]}}", S2A_TEST_COMP_1);
        int rc = nvn_vk_program_parse_compute((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
        ck_true("compute v2 separate maximum slots", rc == 0 && p.binding_count == 1 && p.bindings[0].tex_slot == 255 && p.bindings[0].samp_slot == 127, why);
        nvn_vk_program_free_compute(&p);
    }

    if (fails) {
        printf("✗ direct_nvn_compute_storage: %d 处不符（共 %d 项检查）\n", fails, checks);
        return 1;
    }
    printf("✓ direct_nvn_compute_storage: compute storage 三来源解析 / 21 条来源反例 / "
           "supportReads与c0Reads的null接受 %d 项检查全部通过（L 级）\n", checks);
    return 0;
}
