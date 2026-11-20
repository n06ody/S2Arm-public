// 热点诊断计数（Astra 8E 分析第 2、3 项的「廉价实验」）：只数、不改行为，开关 debug.s2a.nvk_hot_cnt（缺省 0 = 关）。
// 后端每 300 帧读一次开关、打一组「热点计数」行（backend_vulkan_wsi.inc），并把开关传给另两个模块：
//   ① 顶点缓冲绑定（backend_vulkan.c 内）；② CFDictionary 兼容表查找（portable.c）；③ 贴图事实摘要重算原因（nvk_shader.c）。
// ② ③ 经弱引用调用：只链后端的测试 / 诊断目标里没有它们，那两行就不打。
#ifndef NVN_HOT_CNT_H
#define NVN_HOT_CNT_H
#include <stdint.h>

// ② 指针表查找：自上次 take 起的增量（max_cmp 是窗口内单次最多比较次数）；tables / keys / keys_max 取这一刻。
struct s2a_cfdict_hot {
    uint64_t lookups, compares, max_cmp, hits, same_key;
    uint32_t tables, keys, keys_max;
};
void s2a_cfdict_hot_enable(int on);
void s2a_cfdict_hot_take(struct s2a_cfdict_hot *out);

// ③ 贴图事实摘要（rp_tex_fact_sig）：按调用点 × 结果计数，外加每个调用点重算的墙钟纳秒。
enum { NVK_SIG_SEP, NVK_SIG_COMB, NVK_SIG_HIGH, NVK_SIG_GS, NVK_SIG_CS_IMG, NVK_SIG_SITES };
// 结果：命中缓存；重算原因按先后取第一条成立的——首次（缓存空）、登记表代号变、池起点 / 大小变、槽内容变（分离槽为槽代号变）；
// GS 组合槽与计算图像槽不缓存，每次都算，记「不缓存」。
enum { NVK_SIG_HIT, NVK_SIG_FIRST, NVK_SIG_REG, NVK_SIG_POOL, NVK_SIG_SLOTS, NVK_SIG_UNCACHED, NVK_SIG_WHYS };
struct nvk_sig_hot { uint64_t n[NVK_SIG_SITES][NVK_SIG_WHYS], ns[NVK_SIG_SITES]; };
void nvk_shader_sig_hot_enable(int on);
void nvk_shader_sig_hot_take(struct nvk_sig_hot *out);

// ① 一笔绘制里「变了的顶点缓冲槽」掩码 → 连续区间数（不跨空槽、不跨没变的槽）。合并相邻槽可省的调用数 = 槽数 − 区间数。
static inline uint32_t nvn_hot_runs16(uint32_t m) { m &= 0xFFFFu; return (uint32_t)__builtin_popcount(m & ~(m << 1)); }
#endif
