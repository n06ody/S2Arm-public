// 预热调度：把「首次使用才做」的准备工作（程序读盘/解析/建设备对象、管线创建）从回放线程搬到
// 一个有界后台线程，按上一局留下的清单在开局陆续准备；真实绘制在自己的 owner 缓存里直接命中。
//
// 蓝本：iOS graphics/nvn/gfx/nvn/nvn_mtl.m 的 warm_start / warm_prepare（:9097 / :8973，清单预热在
// 后台、首帧不等；DM_PSO_BG_THREADS 缺省 2 路）+ pso_pend（异步建管线）。这里一路线程，理由：安卓目标机
// 已有游戏 3 个多核在用（真机 5 秒窗口进程 CPU 15.4 s），预热只做「本来就会在关键路径上做的事情」的下沉，
// 两路以上收益不抵抢核风险；要更多路时加 worker 数即可，合同不变。
//
// 本模块只做时序：每条工作由 nvk_prog_warm_build / nvk_pipe_warm_build 在自己的 owner 里完成，
// 代际、去重、停局拒绝都在那两个模块里。预热线程不发命令、不碰回放状态（rp_S）、不提交。
//
// 生命周期：会话 open 成功后 start（幂等）；Stop / 收尾前 stop（请停 + 有界 join + 清单落盘 +
// 后台静默确认）。启动清单处理完后保留线程，用于 SetShaders 预取和周期落盘。
// 开关：debug.s2a.nvk_warm = 0 关（缺省 1）。进度日志走 nvn_plat_log，前缀 "warm"。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nvk_warm_start(void);     // 幂等；会话 open 之后调用
// 幂等；Stop / 收尾之前调用。**有界**：交互调用方只在统一截止（≤500 ms）内等「静默 + 清单落盘完成」，
// 落盘由预热线程自己执行，这里绝不做文件 IO、绝不等文件锁或慢盘。
//   返回 0 = 已静默且落盘完成；1 = 预算内没完成（后台线程继续收尾，旧文件保留，迟到结果由各 owner 丢弃）。
int  nvk_warm_stop(void);
int  nvk_warm_active(void);    // 1 = 预热线程还活着

// LIFO cache-only work on the same low-priority thread. The caller supplies
// stage keys only after retaining the sources. Admission never waits.
void nvk_warm_prefetch_queue(const uint8_t (*keys)[8], int n, uint64_t session, uint64_t generation);

// 验收读数：预热线程这一局做了多少、为什么退出。
struct nvk_warm_totals {
    uint64_t prog_built, prog_failed;   // 程序：发布成功 / 失败
    uint64_t pipe_built, pipe_failed;   // 管线：建好 / 失败
    uint64_t prefetch_queued, prefetch_dedup, prefetch_dropped;
    uint64_t prefetch_ready, prefetch_missed, prefetch_cancelled;
    uint64_t ms;                        // 预热线程存活毫秒数
    int why;                            // 0 = 清单跑完 / 1 = Stop / 2 = 关闭（debug.s2a.nvk_warm=0）/ 3 = 无清单
};
void nvk_warm_totals(struct nvk_warm_totals *out);

#ifdef __cplusplus
}
#endif
