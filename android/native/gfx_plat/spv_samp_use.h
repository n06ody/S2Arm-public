// 一次性诊断（不进 PR）：组合采样绑定在 SPIR-V 里到底用不用采样器。
// 从 set / binding 对应的 UniformConstant 变量出发做 def-use 闭包：
//   OpLoad / 常量下标 OpAccessChain → 仍是组合采样值；OpImage → 拆出的图像值；
//   图像值只进 OpImageFetch / OpImageSparseFetch / OpImageQuerySize(Lod) / QueryLevels / QuerySamples → 不用采样器；
//   组合采样值进任何取样 / gather（87–94、96、97、105 QueryLod、305–312、314、315）→ 要采样器；
//   其余一切（OpPhi / OpSelect / OpCopyObject / OpFunctionCall / OpStore / 动态下标 / 未知指令 / 扩展指令 / 解析异常 / 缺变量 / 多个同号变量）→ 不明。
// 只有闭包完整、所有用途都是取值或查尺寸时才记「不用」；有不明就是不明（不明优先于要），否则有取样就是「要」。
// 判断只针对着色器语义，不证明 Vulkan 组合描述符可以不填采样器。
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SPV_SU_NOTRUN = 0, SPV_SU_UNKNOWN = 1, SPV_SU_NONE = 2, SPV_SU_NEED = 3, SPV_SU_UNUSED = 4 };
struct spv_su {
    uint8_t cls;          // SPV_SU_*
    uint16_t op;          // 定类的那条指令的操作码（不明 / 要）；缺变量等非指令原因为 0
    uint8_t why;          // 非指令原因：见 spv_su_why
    uint16_t fetch, query, sample;   // 各类用途条数（封顶 65535）
    uint16_t next;        // 扩展数（OpExtension 条数，只记录，见 spv_su_why 的说明）
};
enum { SPV_SU_WHY_NONE = 0, SPV_SU_WHY_HEADER, SPV_SU_WHY_PARSE, SPV_SU_WHY_NOVAR, SPV_SU_WHY_MULTIVAR, SPV_SU_WHY_NOTCOMBINED,
       SPV_SU_WHY_DYNIDX, SPV_SU_WHY_OOM, SPV_SU_WHY_PASSES };
static inline const char *spv_su_cls_name(int c) {
    static const char *const n[] = {"notrun", "unknown", "none", "need", "unused"};
    return c >= 0 && c < 5 ? n[c] : "?";
}
static inline const char *spv_su_why_name(int w) {
    static const char *const n[] = {"-", "header", "parse", "no_var", "multi_var", "not_combined", "dynamic_index", "oom", "passes"};
    return w >= 0 && w < 9 ? n[w] : "?";
}

static inline int spv_su_is_sample(uint32_t op) {
    return (op >= 87 && op <= 94) || op == 96 || op == 97 || op == 105 || (op >= 305 && op <= 312) || op == 314 || op == 315;
}
static inline int spv_su_is_fetch(uint32_t op) { return op == 95 || op == 313; }
static inline int spv_su_is_query(uint32_t op) { return op == 103 || op == 104 || op == 106 || op == 107; }
// 不带值操作数的指令（调试、注解、类型、带字面量的常量、模块头、控制流标签）：其中出现的字就算等于被追的号也不是用途
static inline int spv_su_skip(uint32_t op) {
    return (op >= 1 && op <= 8) || op == 317 || op == 330 || op == 10 || op == 11 || op == 14 || op == 15 || op == 16 || op == 17 ||
           op == 331 || (op >= 19 && op <= 39) || op == 322 || op == 43 || op == 50 || (op >= 71 && op <= 75) || op == 332 || op == 5632 || op == 5633 ||
           op == 248 || op == 249 || op == 247 || op == 246 || op == 250 || op == 251 || op == 54 || op == 56 || op == 253 || op == 252;
}

static inline void spv_su_classify(const void *blob, size_t bytes, uint32_t set, uint32_t binding, struct spv_su *r) {
    memset(r, 0, sizeof *r); r->cls = SPV_SU_UNKNOWN;
    const uint32_t *w = blob;
    if (!w || bytes < 20 || bytes % 4 || w[0] != 0x07230203u || !w[3] || w[3] > 0x400000u) { r->why = SPV_SU_WHY_HEADER; return; }
    const uint32_t bound = w[3]; const size_t words = bytes / 4;
    // 每个号：0 不追、1 组合采样值（含指向它的指针）、2 拆出的图像值；def[] 记定义它的指令位置（判常量下标用）
    uint8_t *tr = calloc(bound, 1); uint32_t *defop = calloc(bound, sizeof *defop);
    uint32_t *dset = malloc(bound * sizeof *dset), *dbind = malloc(bound * sizeof *dbind);
    size_t *defpos = calloc(bound, sizeof *defpos);
    if (!tr || !defop || !dset || !dbind || !defpos) { r->why = SPV_SU_WHY_OOM; goto out; }
    for (uint32_t i = 0; i < bound; i++) dset[i] = dbind[i] = UINT32_MAX;
    // 第一遍：结构合法性、装饰、定义位置
    for (size_t pos = 5; pos < words;) {
        uint32_t count = w[pos] >> 16, op = w[pos] & 0xffffu;
        if (!count || count > words - pos) { r->why = SPV_SU_WHY_PARSE; goto out; }
        const uint32_t *q = w + pos;
        if (op == 10 && r->next < 65535) r->next++;
        if (op == 71 && count >= 4 && q[1] < bound) { if (q[2] == 34) dset[q[1]] = q[3]; if (q[2] == 33) dbind[q[1]] = q[3]; }
        // 只记后面要查的定义：类型（结果号在 q[1]）、常量与变量（结果号在 q[2]）；其它指令不记，免得把无结果指令的操作数当成定义
        uint32_t res = 0;
        if ((op >= 19 && op <= 39) || op == 322) res = count >= 2 ? q[1] : 0;
        else if (((op >= 41 && op <= 46) || (op >= 48 && op <= 52) || op == 59) && count >= 3) res = q[2];
        if (res && res < bound) { defop[res] = op; defpos[res] = pos; }
        pos += count;
    }
    // 找变量：OpVariable(59)，存储类 UniformConstant(0)，set / binding 对上
    uint32_t var = 0; int nvar = 0;
    for (uint32_t id = 1; id < bound; id++) {
        if (defop[id] != 59 || dset[id] != set || dbind[id] != binding) continue;
        const uint32_t *q = w + defpos[id];
        if ((q[0] >> 16) < 4 || q[3] != 0) continue;
        var = id; nvar++;
    }
    if (!nvar) { r->why = SPV_SU_WHY_NOVAR; goto out; }
    if (nvar > 1) { r->why = SPV_SU_WHY_MULTIVAR; goto out; }
    {   // 类型：指针 → [数组 →] OpTypeSampledImage(27)
        const uint32_t *q = w + defpos[var]; uint32_t t = q[1];
        if (t >= bound || defop[t] != 32) { r->why = SPV_SU_WHY_NOTCOMBINED; goto out; }
        t = w[defpos[t] + 3];
        for (int step = 0; step < 4 && t < bound && (defop[t] == 28 || defop[t] == 29); step++) t = w[defpos[t] + 2];
        if (t >= bound || defop[t] != 27) { r->why = SPV_SU_WHY_NOTCOMBINED; goto out; }
    }
    tr[var] = 1;
    // 不动点：每遍按顺序扫全部指令，新追到的号下一遍再看用途；遍数封顶（正常 SPIR-V 定义在用途之前，两三遍收敛）
    int changed = 1, passes = 0; uint32_t f = 0, qn = 0, sm = 0;
    while (changed) {
        if (++passes > 16) { r->why = SPV_SU_WHY_PASSES; goto out; }
        changed = 0; f = qn = sm = 0;
        for (size_t pos = 5; pos < words;) {
            uint32_t count = w[pos] >> 16, op = w[pos] & 0xffffu;
            const uint32_t *q = w + pos; pos += count;
            if (spv_su_skip(op) || op == 59) continue;   // 变量的定义本身不是用途
            int hit = 0;
            for (uint32_t k = 1; k < count; k++) if (q[k] < bound && tr[q[k]]) { hit = 1; break; }
            if (!hit) continue;
            if (op == 61 && count >= 4 && q[3] < bound && tr[q[3]] == 1) { if (q[2] < bound && !tr[q[2]]) { tr[q[2]] = 1; changed = 1; } continue; }   // OpLoad
            if ((op == 65 || op == 66) && count >= 4 && q[3] < bound && tr[q[3]] == 1) {   // OpAccessChain / InBounds：下标都得是常量
                for (uint32_t k = 4; k < count; k++) if (q[k] >= bound || defop[q[k]] != 43) { r->why = SPV_SU_WHY_DYNIDX; r->op = (uint16_t)op; goto out; }
                if (q[2] < bound && !tr[q[2]]) { tr[q[2]] = 1; changed = 1; }
                continue;
            }
            if (op == 100 && count >= 4 && q[3] < bound && tr[q[3]] == 1) { if (q[2] < bound && !tr[q[2]]) { tr[q[2]] = 2; changed = 1; } continue; }   // OpImage
            if (count >= 4 && q[3] < bound) {
                if (spv_su_is_sample(op) && tr[q[3]] == 1) { sm++; if (!r->op) r->op = (uint16_t)op; continue; }
                if (spv_su_is_fetch(op) && tr[q[3]] == 2) { f++; continue; }
                if (spv_su_is_query(op) && tr[q[3]] == 2) { qn++; continue; }
            }
            r->op = (uint16_t)op; goto out;   // 其余一切用途：不明
        }
    }
    r->fetch = (uint16_t)(f > 65535 ? 65535 : f); r->query = (uint16_t)(qn > 65535 ? 65535 : qn); r->sample = (uint16_t)(sm > 65535 ? 65535 : sm);
    r->cls = sm ? SPV_SU_NEED : (f || qn) ? SPV_SU_NONE : SPV_SU_UNUSED;
    if (!sm) r->op = 0;
out:
    free(tr); free(defop); free(dset); free(dbind); free(defpos);
}
