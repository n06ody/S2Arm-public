// 程序预热清单：本机玩这款游戏真实用过的「程序内容身份」（跨会话、设备侧增长）。下次开局由预热线程
// 提前完成读盘 / 离线库取 → 解析 → 建后端着色器与绑定布局，发布到来源缓存（g_progs），真实 draw 直接
// 命中；取不到的照旧走同步路径，绘制语义不变（不跳过任何绘制）。
//
// 蓝本：iOS graphics/nvn/gfx/nvn/nvn_mtl.m 的 warm_start / warm_prepare（:9097 / :8973，清单预热在后台、
// 首帧不挡）与 pso_user.tsv（设备侧增长）。差别：iOS 一行 = 一条 Metal PSO；这里程序与管线分开记，
// 因为 Vulkan 的程序对象（着色器模块 + 绑定布局）与管线对象分别由两边 owner 持有（见 nvn_vk_pipe_warm.h）。
//
// 身份：S2AI1 v3 规范输入的 SHA-256，也就是来源缓存（g_progs）的键，等于 struct rp_prog.identity。
// session / generation / request_id 是传输身份，换一局就变，不进清单（方案 §5.5.2 第 8 条：拿它们当
// 盘上键会让预热永远不命中）。
// 文件：<nvk_session_cache_dir()>/programs-v<版本>.bin（该目录已经是 <state>/direct-cache/nvn-vk，不要再套一层）。magic + 版本 + 条目大小 + 条数 + FNV-1a 校验；
// 损坏 / 超限 / 短读一律整份拒绝，绝不喂给运行时。写：临时文件 + fsync + rename + 目录 fsync。
// 线程：note 在提交线程（真实 resolve 成功后）；load/take/build/pending/save 由预热线程（Lead 的
// nvn_warm.c）调用；stats 任意线程。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVK_PROG_REC_STAGES 3
#define NVK_PROG_REC_C1_BYTES 0x100   // 与 source_store 的 c1 判据同源（views[st].c1_bytes >= 0x100 才算完整）

// 一条记录：身份 + 阶段掩码 + 各阶段 8 字节源键 + 各阶段 c1 常量段。
//   · 源键：sha256(control‖code) 的前 8 字节，只作日志/定位；
//   · c1：解析该程序时随源快照捕获的常量段（nvk_shader.c build() 逐字节拷进 rp_prog.c1）。必须随记录落盘：
//     开局预热时游戏还没 ProgramSetShaders（source_store 是空的），拿不到 views；而 rp_draw.c 对带 C1
//     绑定的程序在 c1_ok 位不齐时直接跳过绘制（"c1_segment_not_captured"），所以预热产品必须与真实
//     解析的产品逐位一致——照抄当时的 c1，不重新解释控制段。
//   · uses：本机真实使用次数（note 命中同一身份时 +1）。清单落盘按 uses 降序、同 uses 按最后一次使用
//     时间降序，预热带照这个顺序做：热点先准备好（真机 45 秒里出现了 4430 个不同身份，全量全量预热
//     不现实，热点优先才让常见路线先安全）。
struct nvk_prog_rec {
    uint8_t identity[32];
    uint32_t stage_mask;                    // 图形阶段掩码，或 32 = 计算阶段
    uint8_t keys[NVK_PROG_REC_STAGES][8];
    uint8_t c1_ok;                          // 位 i = 阶段 i 的 c1 完整（口径同 views[st].c1_bytes >= 0x100）
    uint8_t lp_ok;                          // 记录时的状态与出包预编库的翻译状态相符（nvk_shader.c libpack_state_ok）；旧清单读作 0 = 走现场编
    uint8_t c1[NVK_PROG_REC_STAGES][NVK_PROG_REC_C1_BYTES];
    uint32_t uses;                          // 只用于排序/淘汰，不进身份
};

// 读清单并用本局有效性过滤：profile_sha / direct_abi / translator closure 摘要必须与当前一致。
// 不检查 source_store（开局它本来就是空的——c1 已经随记录落盘，预热不依赖游戏先装载源）。
// 返回可用条数；<0 = 关闭或读失败（已记日志）。只由预热线程调用一次。
int nvk_prog_warm_load(void);

// 预热带游标：1 = 取到一条（游标推进）；0 = 没有更多。
int nvk_prog_warm_take(struct nvk_prog_rec *out);
int nvk_prog_warm_pending(void);

// 准备一条并发布（返回前结果已对真实 draw 可见，要么已发布要么失败）：
//   0 = 已发布到 g_progs（同身份早已发布也算 0）
//   1 = 需要现场编译且已交给编译服务，稍后重试（本条目没发布）
//  -1 = 失败（已计数；本局不再重试该条）
// 线程：[后台]（预热线程）。不得编码命令、不得碰 rp_S。
int nvk_prog_warm_build(const struct nvk_prog_rec *rec);

// 真实 resolve 成功后记录一条（提交线程）：带上当次源快照的 c1 与 uses=1。去重（identity 已有则
// uses++、刷新记录）与有界（上限按 uses 淘汰最冷的一条；`debug.s2a.nvk_warm_max` 可改，缺省 8192、硬顶 16384）。
void nvk_prog_warm_note(const struct nvk_prog_rec *rec);

// 同步落盘（Stop / 会话收尾）。失败只计数，不影响收尾。
void nvk_prog_warm_save(void);

// 预热线程退出后收拢后台在途工作（后台构建/发布一律不得越过这一步进会话收尾）。
// 返回 0 = 已静；1 = 预算内没静（已记红，调用方按保留会话处理）。线程：收尾线程。
int nvk_shader_warm_quiesce(uint32_t budget_ms);

struct nvk_prog_warm_stats {
    uint64_t loaded;      // 读清单时可用的条数（未按本局有效性过滤前的可用条数）
    uint64_t rejected;    // 读清单时因本局有效性被过滤的条数
    uint64_t recorded;    // 本局 note 收下的条数
    uint64_t built;       // 预热带发布成功的条数
    uint64_t failed;      // 预热带失败的条数
    uint64_t retried;     // 返回 1 等待编译的条数
    uint64_t pending;     // 还没有处理完的条数
    uint64_t draw_hits;   // 真实 draw 命中「预建」程序对象的次数（验收用）
    uint64_t draw_miss;   // 真实 draw 没命中、自己同步准备的次数（同一身份预热过却仍 miss 的证据）
};
void nvk_prog_warm_stats(struct nvk_prog_warm_stats *out);

// ---- 实现补充（nvk_shader.c 与宿主回归用；不改上面的签名）----

// stats.draw_hits 口径（实现约定）：每份「预建程序」第一次被真实 draw 采用时 +1，**不是每笔绘制**；
// 快路径命中不计数。所以它就是「本局有多少份预建程序真的被用上」，不会随绘制笔数放大。
// 本局清单里是否有这个身份（已载入的可用条目 + 本局 note 收下的）。1 = 有。
// 真实 draw 用它区分「清单里有、本局却没被预热发布」（draw_miss）与「清单里本来没有的新程序」
// ——后者是新程序，不是预热失败，不能算 miss。线程：[任意]。
int nvk_prog_warm_contains(const uint8_t identity[32]);

// 预热带一条的结果记账（nvk_prog_warm_build 自己调，调度器不用调）：
// rc = 0 → built++，1 → retried++，-1 → failed++。
void nvk_prog_warm_stat_build(int rc);
// 真实 draw 的验收计数：返回的程序是预热发布的 / 清单里有却没命中、本笔自己同步准备。线程：[提交]。
void nvk_prog_warm_stat_draw_hit(void);
void nvk_prog_warm_stat_draw_miss(void);

// 清掉本局清单状态（已载入条目、游标、本局记录与统计）。rp_shader_reset 调用；盘上文件不动。
// 同进程再开一局时 load() 也会先做同样的清场，所以不调也不会串局。
void nvk_prog_warm_reset(void);


#ifdef __cplusplus
}
#endif