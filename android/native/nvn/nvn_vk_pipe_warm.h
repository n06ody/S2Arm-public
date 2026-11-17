// 管线预热清单：本机真实建过的图形 / 计算管线配方（跨会话、设备侧增长）。计算预热以 debug.s2a.nvk_compute_warm 隔离，缺省关。下次开局由预热线程
// 用同一份配方提前 nvn_gpu_pipe_get，装进后端的管线缓存；真实 draw 拿同一份 nvn_gpu_pipe_desc 取时
// 命中缓存、直接拿到预建对象（验收看 backend 的 warm 命中计数 nvn_gpu_pipe_warm_hits）。
//
// 蓝本：iOS graphics/nvn/gfx/nvn/nvn_mtl.m 的 pso_manifest.tsv / pso_user.tsv（:9076 / warm_paths）
// 与旧安卓层的 nvn_vk_pipe_manifest.c（文件格式与原子落盘那一套）。身份换成了新层的正式描述：
// nvn_gpu_pipe_desc（backend_vulkan.c same_pipe_desc 的判据）去掉着色器句柄，附带程序内容身份。
//
// 记录不是 desc 的整份字节：desc 里 st[5] 各带 spec[48]（本层 nspec 恒 0，每阶段 384 字节全零），
// 整份 memcpy 一条 2.6 KB、4096 条 10 MB。这里**逐字段序列化**成紧凑小端字节（NVK_PW_DESC_BYTES，
// 352 字节/条），记录里带字节长度；反序列化逐字段还原成 memset 过的 nvn_gpu_pipe_desc。
//
// 配方判据语义：hash 只作预筛，命中要 identity + 紧凑字节**整份逐字节相等**（旧层 64 位 FNV 单键撞
// 一次就复用不兼容管线）。记录里的 pointer 字段（st[].sh）不存，由程序身份还原；nspec != 0 的配方
// （本层目前不产生）不记。
//
// 文件：<nvk_session_cache_dir()>/pipes-v<格式版本>-<描述版本>.bin（该目录已经是
// <state>/direct-cache/nvn-vk，不要再套一层）。头：magic + 格式版本 + 描述版本 + 条目大小 + 条数 +
// FNV-1a 整份校验；任一不符整份拒绝，绝不喂给运行时。写成「临时文件 + fsync + rename + 目录 fsync」。
// 条目顺序 = uses 降序、同 uses 按最后使用时间降序（热点优先，真机 45 秒里有大量只出现一次的管线）。
//
// 线程：note 在提交线程（真实创建成功后，约 130 次/帧 → 先算散列 + 最近 4 条 memo，只有新配方才整份
// 拷进清单）；load/take/build/pending 由预热线程调用；save 由预热线程每 30 秒与收尾各调一次；stats
// 任意线程。全部由本模块内部一把锁保护。
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "nvn_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rp_prog;   // 回放核心的程序解析结果（rp_shader.h）；note_prog 只用它的内容身份与阶段句柄

// 紧凑序列化后的配方长度（逐字段小端；见 .c 的 nvk_pw_desc_write / nvk_pw_desc_read）。
// 改布局必须同时改这个值：它进文件名，也与旧文件天然不命中。
#define NVK_PW_DESC_BYTES 352

// 一条记录：程序内容身份 + 紧凑配方 + 字节长度 + 使用次数（只用于排序/淘汰，不进配方判据）。
// 计算配方首字节 0x80、其余为零；旧图形配方首字节仅使用低 5 位，因此现有文件无需迁移。
struct nvk_pipe_rec {
    uint8_t prog_identity[32];                 // 程序内容身份（rp_prog.identity / g_progs 键）
    uint8_t desc[NVK_PW_DESC_BYTES];           // 紧凑 nvn_gpu_pipe_desc：st[].sh / nspec / spec 不存，其余逐字段
    uint32_t bytes;                            // desc 的有效字节数（当前恒为 NVK_PW_DESC_BYTES）
    uint32_t uses;                             // 真实绑定过多少次（跨局排序用）
};

// 两条诊断 / 用例入口（与 note 内部走同一条路径，语义不另立）：
//   rec_of：把一条描述规范成记录（st[].sh / nspec / spec 不存）；nspec 非 0 返回带 0 的 bytes。
//   hash：记录的 64 位散列（只作预筛；相等与否仍由 identity + 整份字节定）。用例用它构造
//         「散列相同、字节不同」的负例（只比散列的实现在这一例上会把两条配方并成一条）。
void nvk_pipe_warm_rec_of(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], struct nvk_pipe_rec *out);
uint64_t nvk_pipe_warm_hash(const struct nvk_pipe_rec *rec);

// 用例钩子：把散列固定成常数（1 = 有效），证明命中的判据是 identity + 整份字节而不是散列。
// 生产路径不调用它；缺省 0（真散列）。宿主单测 android/tests/nvn_vk_pipe_warm_test.c 用。
void nvk_pipe_warm_test_hash_const(int on);

int nvk_pipe_warm_load(void);        // 开局读清单 + 整份校验；返回可用条数（<0 = 关闭/失败）
int nvk_pipe_warm_take(struct nvk_pipe_rec *out);
int nvk_pipe_warm_pending(void);

// 按记录建一条管线并装进后端缓存（后台线程）：
//   0 = 已建 / 早已存在  1 = 程序还没发布（稍后重试）  -1 = 失败（已计数，本局不再重试）
// 线程：[后台]。只调 nvn_gpu_pipe_get（契约 v8 明确 [提交] 与 [后台] 都可调），不编码命令。
int nvk_pipe_warm_build(const struct nvk_pipe_rec *rec);

// 真实创建成功后记录一条（提交线程）。着色器句柄 / nspec / spec 不进记录、不参与判等；nspec 非 0 或
// 程序没有身份就直接跳过；去重按整份配方字节 + 程序身份（重复只 uses++）；有界：上限
// debug.s2a.nvk_warm_max（缺省 8192，也是硬顶），满了淘汰 uses 最小、同 uses 最久没用的那条。
void nvk_pipe_warm_note(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32]);

// rp_draw 用的入口：程序身份从 rp_prog 取（回放核心里不出现身份解析）。程序没有身份就不记。
// np = 刚取到的后端管线对象（可为 NULL）。一局内 np 与「配方 + 程序身份」一一对应（后端缓存的键含着色器句柄，
// 局内不淘汰、不复用指针），所以清单把这份配方的（槽号, 槽代号）标签存在 np 上（nvn_gpu_pipe_warm_tag_set）：
// 之后每次换到同一条管线，标签还对得上（槽仍占用、槽代号没变、槽里那份配方的程序身份就是这次传进来的程序）
// 就只 uses++ / 刷新最近序号——不序列化 352 字节、不散列、不查备忘和桶链；对不上（第一次见这条 np / 槽被淘汰
// 或重载后重用过 / 同一条 np 换了程序身份）才走完整路径，入清单（或并进已有条目）后把新标签写回。
// 槽代号：清单每往一个槽里写一份新配方（入清单、淘汰重用、重载）加一，旧标签随之失效，不会给新配方白加 uses。
// 清单内容与 uses 的口径与不带标签时逐项相同。
// 返回 1 = 收下或 uses++（nspec 非 0 照旧计入 rejected 并返回 1）；0 = 跳过（没身份 / 参数空）。
int nvk_pipe_warm_note_prog(const nvn_gpu_pipe_desc *d, const struct rp_prog *p, nvn_gpu_pipe np);

void nvk_pipe_warm_save(void);   // 内容没变直接返回；有变化才原子写；失败只计数（下次 save 再试）

struct nvk_pipe_warm_stats {
    uint64_t loaded;      // 读清单时可用条数
    uint64_t rejected;    // 被拒绝的条数（整份拒绝时 = 文件声明的条数；note 跳过 / 淘汰也计在这里）
    uint64_t recorded;    // 本局 note 收下（新配方）的条数
    uint64_t built;       // 预热带建好 / 命中的条数
    uint64_t failed;      // 预热带失败的条数
    uint64_t retried;     // 返回 1（等程序发布）的次数
    uint64_t pending;     // 还没有被 take 的条数
    uint64_t draw_hits;   // 转发 backend：真实 draw 命中「后台预建」对象的次数（验收读数）
    uint64_t tag_hits;    // 换管线时标签对得上、只加了计数的次数（没走序列化 / 散列 / 查表）
};
void nvk_pipe_warm_stats(struct nvk_pipe_warm_stats *out);

// ---- 清单颠簸 / 逐帧变化量诊断（#1372；debug.s2a.nvk_pw_diag，缺省 0）----
// 清单满了以后每新记一条要整表扫一遍挑淘汰目标；站着不动时这一项每秒都在发生。原因有两种：(a) 被淘汰的配方过一会儿又要
// 用、再插回来（颠簸）；(b) 配方里混进了逐帧变化的量，同一条管线每帧都被当成新配方。下面的计数和日志用来分辨它们。
// 只读计数 + 日志：不改清单的任何判定、顺序和落盘字节。玩家包的 nvn_plat_flag 恒取缺省 0 → 整块不运行（不分配、不读时钟、
// 不打日志）。开着时（调试包 setprop debug.s2a.nvk_pw_diag 1）：
//   · note 每次进锁读一次单调时钟，每 5 秒打一行窗口汇总（note 次数 / 新记 / 此前没见过 / 被淘汰过又插回 / 淘汰扫描耗时 /
//     淘汰集合清空次数 / 条数）；
//   · 淘汰时把被淘汰配方的散列记进集合（1 MiB，满 3/4 清空并计数）；新记时查它，分成「此前没见过」与「被淘汰过又插回」；
//   · 每个窗口每类各抽 1 条新记配方做样本（每类最多 24 条）：整表找最近的一条，打出身份是否相同、差几个字节、差在哪些字段。
//     持锁只做内存拷贝（这条新配方 + 一份清单快照，快照缓冲诊断开时才分配）；整表比对和所有日志都在锁外，
//     所以抽样不会让回放线程多等 1–3 毫秒把读数自己污染（样本行里带「快照拷贝 N 微秒（持锁）」「比对 N 微秒（锁外）」）。
struct nvk_pipe_warm_diag {
    int on;                  // 1 = 诊断开着；0 = 关着（其余字段全 0）
    uint64_t calls;          // 过了身份 / 特化筛、进到锁里的 note 次数
    uint64_t memo_hits;      // 最近 4 条 memo 命中（只 uses++）
    uint64_t chain_hits;     // 桶链命中（只 uses++）
    uint64_t news;           // 新记条数（与 stats.recorded 同口径，从诊断开着起算）
    uint64_t evicts;         // 新记时表已满、挤掉一条的次数（≠ stats.rejected：后者还混着 nspec 跳过和整份拒绝）
    uint64_t fresh;          // 新记里：此前没被淘汰过
    uint64_t reins;          // 新记里：此前被淘汰过又插回来（颠簸）
    uint64_t scans;          // 淘汰扫描次数（含表未满时找空位的那一遍）
    uint64_t scan_ns;        // 扫描总耗时（纳秒）
    uint64_t ev_clears;      // 淘汰散列集合装满清空的次数（清空之后的「插回」会漏数）
    uint64_t samples_fresh, samples_reins;   // 已输出的样本条数
};
void nvk_pipe_warm_diag_stats(struct nvk_pipe_warm_diag *out);
void nvk_pipe_warm_diag_flush(void);   // 立刻输出当前窗口一行并开新窗口（用例用；诊断关着时什么也不做）

// ---- 后端协作（实现在 backend_vulkan.c；宿主单测不链后端，本模块按弱符号调） ----
// 预热线程建管线的作用域：1 = 接下来的创建算「预建」，后端据此在对象上打 warm 标记并计 warm 创建数。
void nvn_gpu_pipe_warm_scope(int on);
uint64_t nvn_gpu_pipe_warm_hits(void);      // 真实 draw 命中预建对象的次数
uint64_t nvn_gpu_pipe_warm_created(void);   // 预建出来的对象数
// 清单标签：存在后端管线对象上的一个 64 位值（低 32 位 = 槽号 + 1，高 32 位 = 槽代号；0 = 没设过）。
// 原子读写、不加锁；标签随管线对象一起销毁（后端局内不淘汰管线），所以不会指到别的管线。
uint64_t nvn_gpu_pipe_warm_tag(nvn_gpu_pipe p);
void nvn_gpu_pipe_warm_tag_set(nvn_gpu_pipe p, uint64_t tag);

#ifdef __cplusplus
}
#endif
