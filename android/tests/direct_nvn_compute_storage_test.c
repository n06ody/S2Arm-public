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
#include <stdlib.h>
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

// 共享内存溢出缓冲（kind = spill，编译器 DirectSharedSpill）：每线程组字节数进程序对象；没有客体槽；形状不对如实拒
static void case_spill(void) {
    group("[S] 共享内存溢出缓冲 spill 绑定");
    struct nvn_vk_compute_program p;
    if (accept_case("[S1] spill 40960 字节 / 组", "{\"stage\":\"cs\",\"kind\":\"spill\",\"set\":1,\"binding\":0,\"count\":1,\"bytesPerGroup\":40960}", "[]", "[]", &p) == 0) {
        ck_u32("[S1] spill_bytes", p.spill_bytes, 40960);
        ck_u32("[S1] binding kind 4", (uint32_t)p.bindings[0].kind, 4);
        nvn_vk_program_free_compute(&p);
    }
    if (accept_case("[S2] 没有 spill 绑定", "", "[]", "[]", &p) == 0) { ck_u32("[S2] spill_bytes 为 0", p.spill_bytes, 0); nvn_vk_program_free_compute(&p); }
    reject_case("[S3] spill 缺 bytesPerGroup", "{\"stage\":\"cs\",\"kind\":\"spill\",\"set\":1,\"binding\":0,\"count\":1}", "[]", "[]", "bytesPerGroup");
    reject_case("[S4] spill bytesPerGroup 不是 4 的倍数", "{\"stage\":\"cs\",\"kind\":\"spill\",\"set\":1,\"binding\":0,\"count\":1,\"bytesPerGroup\":40962}", "[]", "[]", "bytesPerGroup");
    reject_case("[S5] spill 超过 48 KiB", "{\"stage\":\"cs\",\"kind\":\"spill\",\"set\":1,\"binding\":0,\"count\":1,\"bytesPerGroup\":49156}", "[]", "[]", "bytesPerGroup");
    reject_case("[S6] 两个 spill", "{\"stage\":\"cs\",\"kind\":\"spill\",\"set\":1,\"binding\":0,\"count\":1,\"bytesPerGroup\":4},"
                "{\"stage\":\"cs\",\"kind\":\"spill\",\"set\":1,\"binding\":1,\"count\":1,\"bytesPerGroup\":4}", "[]", "[]", "more than one spill");
    reject_case("[S7] spill 带客体槽", "{\"stage\":\"cs\",\"kind\":\"spill\",\"set\":1,\"binding\":0,\"count\":1,\"slot\":0,\"bytesPerGroup\":4}", "[]", "[]", "guest slot");
    reject_case("[S8] 非 spill 绑定带 bytesPerGroup", "{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":8,\"count\":1,\"bytesPerGroup\":4}", "[]", "[]", "bytesPerGroup");
}

int main(void) {
    case_spill();
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

    // #1594：类型 3 句柄（常量采样器）计算侧没接。带 constSampler 的 cs 贴图绑定必须具名拒绝——不能被当成「没有 samp_slot = 采样器取同字高位」
    //   而绑成错的采样器（宿主的计算回放不认这个来源）。v1 / v2 结果都一样；没有这个字段的同槽绑定照旧接受（见上面直接读 c2 分离贴图表的字）。
    for (int v = 1; v <= 2; ++v) {
        static const char *const fields[] = {"\"slot\":90,\"constSampler\":257", "\"slot\":344,\"constSampler\":0", "\"slot\":90,\"constSampler\":4096", "\"constSampler\":3",
                                             "\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":260"};   // 片 3：游戏 UBO 里的贴图字（图形侧接了，计算侧不接）
        for (unsigned k = 0; k < sizeof fields / sizeof fields[0]; k++) {
            char json[4096], why[192] = ""; struct nvn_vk_compute_program p;
            int n = snprintf(json, sizeof json,
                "{\"schema\":\"s2a-direct-program-v%d\",\"content\":{"
                "\"contract\":\"s2a-direct-program-content-v%d\",\"kind\":\"compute\","
                "\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],"
                "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],"
                "\"bindings\":[{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,%s,\"count\":1}],"
                "\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":[]}}", v, v, S2A_TEST_COMP_1, fields[k]);
            memset(&p, 0, sizeof p);
            int rc = nvn_vk_program_parse_compute((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
            char what[96]; snprintf(what, sizeof what, "compute v%d constant sampler form %u is rejected by name", v, k);
            ck_true(what, rc != 0 && strstr(why, "constant sampler"), why);
            nvn_vk_program_free_compute(&p);
        }
    }

    // 计算结果是「程序」级文档：超过 1 MiB（描述文件上限）的合法结果要接；超过程序结果上限（64 MiB，同跨局缓存单条）的拒，原因带大小
    {
        static const uint8_t head[20] = {0x03, 0x02, 0x23, 0x07, 0x00, 0x01, 0x03, 0x00, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0};
        const size_t spv_bytes = (size_t)3 << 19;   // 1.5 MiB 的 SPIR-V（头 + 0 字；解析器只核头与对齐）
        uint8_t *spv = calloc(1, spv_bytes); memcpy(spv, head, sizeof head);
        static const char t64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t b64n = (spv_bytes + 2) / 3 * 4; char *b64 = malloc(b64n + 1), *q = b64;
        for (size_t i = 0; i < spv_bytes; i += 3) {
            uint32_t v = (uint32_t)spv[i] << 16 | (i + 1 < spv_bytes ? (uint32_t)spv[i + 1] << 8 : 0) | (i + 2 < spv_bytes ? spv[i + 2] : 0);
            *q++ = t64[v >> 18 & 63]; *q++ = t64[v >> 12 & 63]; *q++ = i + 1 < spv_bytes ? t64[v >> 6 & 63] : '='; *q++ = i + 2 < spv_bytes ? t64[v & 63] : '=';
        }
        *q = 0;
        size_t cap = b64n + 1024; char *json = malloc(cap), why[192] = ""; struct nvn_vk_compute_program p;
        int n = snprintf(json, cap,
            "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
            "\"contract\":\"s2a-direct-program-content-v1\",\"kind\":\"compute\","
            "\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],"
            "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],\"bindings\":[],"
            "\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":[]}}", b64);
        int rc = nvn_vk_program_parse_compute((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
        ck_true("compute result larger than 1 MiB is accepted", n > (1 << 20) && rc == 0 && p.stage.spirv_bytes == spv_bytes, why);
        if (rc == 0) nvn_vk_program_free_compute(&p);
        free(json); free(b64); free(spv);
        size_t over = (size_t)NVK_PROGRAM_RESULT_MAX_BYTES + 1;
        char *big = malloc(over); memset(big, ' ', over); big[0] = '{'; big[over - 1] = '}';
        why[0] = 0;
        rc = nvn_vk_program_parse_compute((const uint8_t *)big, over, &p, why, sizeof why);
        char want[64]; snprintf(want, sizeof want, "%zu bytes", over);
        ck_true("compute result over the program limit is rejected with its size", rc != 0 && strstr(why, want) && strstr(why, "larger than"), why);
        free(big);
    }

    // 立即句柄直接读 c2 分离贴图表的字（空轨 2nd 9000：cs 也有）：slot 90..344 偶、无 samp_slot → 分离贴图槽 128 + k；表外 / 奇字照旧拒
    {
        static const struct { int slot, ok, want; } sep[] = {{90, 1, 128}, {344, 1, 255}, {92, 1, 129}, {91, 0, 0}, {346, 0, 0}, {88, 0, 0}};
        for (unsigned k = 0; k < sizeof sep / sizeof sep[0]; k++) {
            char b[160], why[192] = ""; struct nvn_vk_compute_program p;
            snprintf(b, sizeof b, "{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":%d,\"count\":1}", sep[k].slot);
            int rc = parse_case(b, "[]", "[]", &p, why, sizeof why);
            checks++;
            if (sep[k].ok && !(rc == 0 && p.binding_count == 1 && p.bindings[0].tex_slot == sep[k].want && p.bindings[0].samp_slot == -1))
                failf("compute direct separate texture", "slot %d 期望 接受、分离贴图槽 %d，真结果 rc=%d why=%s", sep[k].slot, sep[k].want, rc, why);
            if (!sep[k].ok && !(rc != 0 && strstr(why, "compute texture slot outside recorded range")))
                failf("compute direct separate texture", "slot %d 期望 拒绝，真结果 rc=%d why=%s", sep[k].slot, rc, why);
            nvn_vk_program_free_compute(&p);
        }
    }

    // 按贴图采样 c2 图像句柄表的字（空轨 2nd compute_atmosphere_scatter：slot 72）：72..86 偶、无 samp_slot → 贴图槽 32 + k（同 iOS tex_slot_h）；奇字、表外照旧拒
    {
        static const struct { int slot, ok, want; } it[] = {{72, 1, 32}, {74, 1, 33}, {86, 1, 39}, {73, 0, 0}, {88, 0, 0}};
        for (unsigned k = 0; k < sizeof it / sizeof it[0]; k++) {
            char b[160], why[192] = ""; struct nvn_vk_compute_program p;
            snprintf(b, sizeof b, "{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":%d,\"count\":1}", it[k].slot);
            int rc = parse_case(b, "[]", "[]", &p, why, sizeof why);
            checks++;
            if (it[k].ok && !(rc == 0 && p.binding_count == 1 && p.bindings[0].tex_slot == it[k].want && p.bindings[0].samp_slot == -1))
                failf("compute texture from image table", "slot %d 期望 接受、贴图槽 %d，真结果 rc=%d why=%s tex_slot=%d", it[k].slot, it[k].want, rc, why, rc == 0 ? p.bindings[0].tex_slot : -99);
            if (!it[k].ok && !(rc != 0 && strstr(why, "compute texture slot outside recorded range")))
                failf("compute texture from image table", "slot %d 期望 拒绝，真结果 rc=%d why=%s", it[k].slot, rc, why);
            nvn_vk_program_free_compute(&p);
        }
    }

    // 计算阶段的图像：handleIndex 72..86 偶数 = 驱动 c2 图像句柄表第 0..7 槽（同 iOS C2_IMG），槽号 = (handleIndex−72)/2；表外拒绝并报 handleIndex
    {
        static const struct { int slot, ok, want; } img[] = {{72, 1, 0}, {86, 1, 7}, {71, 0, 0}, {88, 0, 0}, {74, 1, 1}};
        for (unsigned k = 0; k < sizeof img / sizeof img[0]; k++) {
            struct nvn_vk_compute_program p; char why[192] = "", b[160];
            snprintf(b, sizeof b, "{\"stage\":\"cs\",\"kind\":\"image\",\"set\":3,\"binding\":0,\"slot\":%d,\"count\":1}", img[k].slot);
            checks++;
            int rc = parse_case(b, "[]", "[]", &p, why, sizeof why);
            if (img[k].ok && !(rc == 0 && p.binding_count == 1 && p.bindings[0].kind == 3 && p.bindings[0].tex_slot == img[k].want))
                failf("compute image slot", "handleIndex %d 期望 接受、图像槽 %d，真结果 rc=%d why=%s", img[k].slot, img[k].want, rc, why);
            if (!img[k].ok && !(rc != 0 && strstr(why, "image handleIndex")))
                failf("compute image slot", "handleIndex %d 期望 拒绝，真结果 rc=%d why=%s", img[k].slot, rc, why);
            nvn_vk_program_free_compute(&p);
        }
    }

    if (fails) {
        printf("✗ direct_nvn_compute_storage: %d 处不符（共 %d 项检查）\n", fails, checks);
        return 1;
    }
    printf("✓ direct_nvn_compute_storage: compute storage 三来源解析 / 21 条来源反例 / "
           "supportReads与c0Reads的null接受 %d 项检查全部通过（L 级）\n", checks);
    return 0;
}
