// 回放核心内部接口（核心各文件之间用；前端只认 nvn_mtl.h / nvn_obj.h 里的 nvn_be_*）。
#pragma once
#include <stdint.h>
#include "nvn_obj.h"
#include "nvn_backend.h"
#include "nvn2_cmd.h"   // p_multi_draw

const char *rp_op_name(uint32_t op);   // 录制命令名（"OP_DRAW" …）；范围外返回 "OP_?"
int rp_pool_count(void);                // 已登记的内存池个数
uint64_t rp_pool_fast_hits(void);       // 调用线程的快路命中累计（只读；测试与调优观测点）
void rp_pool_memo_stats(uint64_t *hits, uint64_t *lookups);   // 池查找的线程内备忘（rp_entry.c）：调用线程自己的累计命中数、查找数
int rp_tex_live_n(void);                // 活贴图对象个数（只是登记数，不等于已建图的个数）
int rp_depth_zero(void);                // NVN 深度模式：1 = 近平面为 0
int rp_origin_lower_left(void);         // NVN 窗口原点：1 = 左下
int rp_present_lower_left(void);        // 上屏翻转：被上屏窗口建时的原点（没有窗口说过 = 当前原点）
uint64_t rp_frame(void);                // 前端给的当前帧号
// 提交之外的旁路段（rp_exec.c）：调用方持提交锁、不在提交回放里。begin 失败返回 NULL（已记账）；commit 返回这一段的栅栏值，0 = 没交成（已报红）
int rp_in_submit(void);
nvn_gpu_cmd rp_side_begin(const char *label);
uint64_t rp_side_commit(nvn_gpu_cmd c);
void nvn_be_read_texels_gpu_stats(uint64_t out[4]);   // rp_entry.c：ReadTexels 读 GPU 内容的读数（次数 / 等之前的活共纳秒 / 最长纳秒 / 读回共纳秒）

// ---------- 内存池镜像（rp_buf.c）----------
// 合法但未绑定的 NVN 存储槽：内部 64 KiB 缓冲（初始全零），不登记 guest 池；会话收尾释放。
nvn_gpu_buf rp_buf_for_null_storage(void);
// 取 [addr, addr+len) 在后端的缓冲与偏移；自上次同步后写过的页在 c 这一段的 GPU 顺序里重传。0 = 地址不在任何池里（已记账）。线程：[提交]。
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t addr, uint64_t len, nvn_gpu_buf *buf, uint64_t *off);
int rp_buf_resolve_multi(int n, const uint64_t *addr, const uint64_t *len, nvn_gpu_buf *buf, uint64_t *off, unsigned char *ok);   // 一次抢锁解析多段
// 回放窗口 = 一次提交的回放（nvn_be_sub_begin → nvn_be_sub_commit，开窗口的线程是回放线程）。窗口开着时别的线程销毁池（MapVirtual 改 run、MemoryPoolFinalize），
// 镜像缓冲不当场放（回放线程手里可能已有 rp_buf_resolve 取到、后端还没 cmd_ref 的句柄），推迟到窗口收尾；窗口外或同线程销毁照旧当场放。
void rp_buf_hold_begin(void);
void rp_buf_hold_end(void);
// 写入区间日志（rp_draw 的暂存片复用判定用）：记号 + 「记号之后有没有人写过这一段」
uint64_t rp_write_mark(void);
int rp_write_touched(uint64_t mark, uint64_t addr, uint64_t len);
void rp_write_note(uint64_t addr, uint64_t len);   // 记一笔「回放自己在 CPU 上改了游戏内存这一段」：段内切片表按它作废；不受写跟踪的池里对应的镜像页也按它作废（rp_buf.c）
void rp_buf_stats(uint64_t *bytes, uint64_t *runs);   // 累计同步字节数 / 段数
int rp_buf_mirrors(void);                             // 现有镜像个数
// 缓冲类命令（rec = 命令记录地址，只进未接日志）
void rp_op_update_ub(const void *a, const void *rec);
void rp_op_copy_b2b(const void *a, const void *rec);
void rp_op_clear_buf(const p_clear_buf *cb, const void *rec);
void rp_op_copy_b2t(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec);
void rp_op_copy_t2b(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec);
void rp_op_copy_t2t(nvn_gpu_cmd c, const p_copy_t2t *o, const void *rec);
void rp_op_clear_tex(nvn_gpu_cmd c, const p_clear_tex *o, const void *rec);
void rp_op_draw_tex(nvn_gpu_cmd c, const p_draw_tex *o, const void *rec);   // OP_DRAW_TEX（rp_draw_tex.c，#1452）：后端有 NVN_GPU_CAP_DRAW_TEX 才画，否则记未接

// 会话四件套（前端按名字调，上沿头文件里没有声明）
void nvn_be_begin_session(void);
int nvn_be_request_stop(void);
int nvn_be_drain_session(void);
int nvn_be_shutdown_session(void);   // 放掉全部后端对象、状态回到 NVN 默认值

// ---------- 渲染目标与通道（rp_pass.c）----------
void rp_pass_set_rt(nvn_gpu_cmd c, const p_set_rt *r);
void rp_pass_clear_color(nvn_gpu_cmd c, const p_clear_color *cc, const void *rec);
void rp_pass_clear_ds(nvn_gpu_cmd c, const p_clear_ds *cd, const void *rec);
void rp_pass_barrier(uint32_t bits);   // 游戏的屏障：记下位，下一个通道前交给后端；带「顺序」位、且开着的通道里已有着色器写的，下一笔绘制前结束该通道（见 rp_pass.c）
uint32_t rp_pass_take_barrier(void);   // 通道外的计算派发前调：取走待发的游戏屏障位（取完清零），由调用方交给后端
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask);   // 有效颜色写掩码（含片元输出映射）与像素区域；附件集合变了或区域超出时重开
int rp_pass_begin(nvn_gpu_cmd c);     // 绘制前调：1 = 通道开着；0 = 开不了（已记账），跳过这笔
void rp_pass_want_layered(nvn_gpu_cmd c, int layered);   // 绘制前调（prepare_draw 之前）：程序的顶点 / 几何阶段写层号 = 1，按分层通道开；与开着的通道不同就收掉重开
// 开通道之前，落地实际绑定的贴图（含整池快照）上的待折清除。按宿主底图匹配，不限级 / 层。
typedef struct rp_pass_read { nvn_gpu_tex tex; nvn_gpu_pool_table pool; } rp_pass_read;
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *reads, int n);
int rp_pass_parked(void);   // 现在挂着几条待折清除（绘制路径的快路门：0 = 不必为读路径收集贴图）
void rp_pass_end(nvn_gpu_cmd c);
void rp_pass_flush(nvn_gpu_cmd c);    // 提交前 / 通道外操作前：落地待折清除并结束通道
void rp_pass_reset(void);
int rp_pass_is_open(void);
int rp_pass_draws(void);      // 当前通道已画的笔数（通道没开 = 0）
int rp_pass_split_ok(void);   // 当前通道能否在两笔之间切开续开（见 rp_pass.c）
const p_set_rt *rp_pass_target(void); // 当前目标（没设过 NULL）

// ---------- 上屏（rp_present.c）----------
void rp_present_set_surface(nvn_gpu_surface s, int display_hz);   // 平台件：窗口挂上（s 非空）/ 摘下（NULL）
void rp_present_stats(uint64_t *shown, uint64_t *dropped);
void rp_present_reset(void);
void rp_present_tc_note(uint64_t wall_ns, uint64_t cpu_ns);   // 上屏线程 CPU：记一次成功上屏（rp_present.c，宿主单测直接驱动）
void rp_present_tc_take(uint64_t out[4]);                      // 取走窗口累计：墙钟纳秒、CPU 纳秒、同线程样本、跨线程样本

// ---------- 绘制（rp_draw.c）----------
// 已正式解码的计数组：显式条数/参数步长，不前瞻命令流。
void rp_draw_count_batch(nvn_gpu_cmd, const p_draw *, const p_draw_idx *, const void *, uint32_t, uint64_t);
void rp_draw(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di, const void *rec);   // di 非空 = 带索引
// 通道级批量拷 base instance（rp_draw.c）：命令循环在把 OP_DRAW / OP_DRAW_IDX 交给 rp_draw 之前把这条记录的头指针放进 rp_draw_stream，
//   rp_draw 只在 rec == rp_draw_stream 时才从这里往后前瞻（别的调用方传进来的 rec 不一定在命令流里）。
// rp_gpu_wr_gen：主段里录了写镜像的 GPU 传输就加一（rp_xfer_cmd 落主段、rp_buf 的按 GPU 顺序改写）；前移拷贝读的间接记录可能被它改过，批量表随之作废。
extern const void *rp_draw_stream;
extern uint64_t rp_gpu_wr_gen;
void rp_multi_draw(nvn_gpu_cmd c, const p_multi_draw *m, const void *rec);   // OP_MULTI_DRAW：整段间接计数多绘一次交后端
void rp_draw_stats(uint64_t *draws, uint64_t *skips);
int rp_pipe_fail_summary(char *b, size_t n);   // 图形管线失败表汇总：上次以来有新失败 / 新挡下时写一行（前 5 类）返回 1，否则 0
int rp_pipe_fail_class(unsigned c, const char **reason, uint64_t *fails, uint64_t *cached);   // 第 c 类（0 起）的原因与计数；没有这类返回 0
// 分时读数：自上次取走以来的累计（纳秒 / 次数），取走即清零
enum { RP_PERF_SUBMIT_NS, RP_PERF_COMMIT_NS, RP_PERF_SUBS, RP_PERF_WAIT_NS, RP_PERF_WAIT_N, RP_PERF_DONE_NS, RP_PERF_DONE_N, RP_PERF_PASSES, RP_PERF_PRESENT_NS,
       RP_PERF_D_PROG, RP_PERF_D_PREP, RP_PERF_D_TEX, RP_PERF_D_PIPE, RP_PERF_D_DYN, RP_PERF_D_BIND, RP_PERF_D_DRAW, RP_PERF_D_TAIL, RP_PERF_D_PASS, RP_PERF_D_PGET, RP_PERF_PB_VIEW, RP_PERF_PB_BAR, RP_PERF_PB_BE, RP_PERF_PB_MARK, RP_PERF_RES_LOCK, RP_PERF_RES_NEW, RP_PERF_RES_INIT, RP_PERF_RES_VIEWMAKE, RP_PERF_PB_SHADOW, RP_PERF_RES_SCAN, RP_PERF_RES_UP, RP_PERF_RES_UPN, RP_PERF_RES_ZERO, RP_PERF_RES_ZERON, RP_PERF_RES_MAKE, RP_PERF_RES_ZDEFER, RP_PERF_BIND_SLICE_HIT, RP_PERF_BIND_SKIP,   // 绑定：段内切片表命中的块数、整组沿用没发的阶段数
      
       RP_PERF_P_DS, RP_PERF_P_DS_IDLE, RP_PERF_P_LOADC, RP_PERF_P_KPIX, RP_PERF_P_ONE, RP_PERF_P_CUT_XFER, RP_PERF_P_CUT_LONG, RP_PERF_P_CUT_CLEAR, RP_PERF_P_CUT_RT, RP_PERF_P_CUT_OP, RP_PERF_TEX_UP_N, RP_PERF_TEX_UP_KB, RP_PERF_TEX_UP_PART, RP_PERF_TEX_UP_ROWS, RP_PERF_BUF_SYNC_KB, RP_PERF_BUF_SYNC_DIRECT_KB, RP_PERF_FX_SUBS, RP_PERF_FX_EVENT, RP_PERF_FX_REPORT, RP_PERF_FX_OCC, RP_PERF_FX_GW, RP_PERF_FIN_SKIP,
       RP_PERF_P_CONT, RP_PERF_P_CLEARONLY, RP_PERF_P_PARK, RP_PERF_BIND_FAST, RP_PERF_BIND_CONT, RP_PERF_TEX_EPOCH, RP_PERF_PC_HIT, RP_PERF_PC_MISS, RP_PERF_WAIT_FIN_N, RP_PERF_WAIT_FIN_NS, RP_PERF_WAIT_SYNC_N, RP_PERF_WAIT_SYNC_NS,
       RP_PERF_P_TINY_DS, RP_PERF_P_TINY_NODS, RP_PERF_P_TINY_KPIX,   // 0–1 笔通道里：挂深度模板的、不挂的、它们的百万像素和（看通道碎片化构成）
       RP_PERF_P_AREA, RP_PERF_P_AREA_KPIX,   // 用了缩小渲染区域的通道数、这些区域的千像素和（面积优化到底有没有真的缩下来）
       RP_PERF_P_SHADOW,   // 影子附件的通道数（深度 / 块压缩图经颜色视图当渲染目标，拷贝通道是我们加的）
       RP_PERF_EXEC_DRAW_NS, RP_PERF_EXEC_OTHER_NS,   // 回放循环里绘制类命令 / 其余命令各花多久（开关 nvk_exec_tm）
       RP_PERF_DISP_N, RP_PERF_DISP_NS,               // 计算派发次数与耗时（其余命令里最贵的一项之一）
       RP_PERF_DISP_TOT_NS, RP_PERF_DISP_FLUSH_NS,    // 派发一次的总耗时、其中 rp_pass_flush（收通道 + 落地挂起清除）占多少
       RP_PERF_DISP_BIND_NS,                          // 派发里取管线 + 发绑定 + 交后端那一段（总耗时减去它和 flush 就是入口取资源循环）
       RP_PERF_DISP_RESOLVE_NS, RP_PERF_DISP_ENTS,    // 派发入口取资源循环的耗时与它扫过的布局项数（判每次 130 微秒花在哪）
       RP_PERF_DISP_IND_NS, RP_PERF_DISP_SHADER_NS,   // 派发里间接携程缓冲解析 / 取计算程序各占多少
       RP_PERF_SETRT_N, RP_PERF_SETRT_END_NS,         // 换目标次数、其中收通道（rp_pass_end）用了多久
       RP_PERF_PASSEND_N,                             // 收通道次数（含换目标、通道中途的清除、提交收尾）
       RP_PERF_BIND_NS, RP_PERF_BIND_REBUILD_N,       // 发绑定总耗时、整组重建的次数（分 B1 快路与重建两块看）
       RP_PERF_UBO_N, RP_PERF_UBO_KB, RP_PERF_UBO_C1_N,   // 常量缓冲暂存的次数 / 千字节 / 其中程序自带常量段（C1）的次数
       RP_PERF_BUF_GPU_ORDER_N, RP_PERF_UBO_MIRROR_N,   // 改 / 拷游戏内存的命令因还没落地的 GPU 写改在 GPU 上做的次数；常量缓冲因此直接绑镜像的次数（#1022）
       RP_PERF_UBO_FILL_N, RP_PERF_UBO_FILL_WAIT_N,   // 其中交给后端异步填的次数（nvn_gpu_arena_fill）/ 改游戏内存前因区间重叠等它填完的次数（rp_ub_fill_fence）
       RP_PERF_UBO_FILL_STEAL_N, RP_PERF_UBO_FILL_STEAL_B, RP_PERF_UBO_FILL_BUSY_N, RP_PERF_UBO_STAGED_B,   // 其中回放线程自己代拷的笔数 / 字节（nvn_gpu_arena_fill_take）/ 撞上录制线程正在拷、等它拷完的次数；段内切片表没认回、真落进暂存区的字节（含异步填、就地拷、程序自带常量段）
       RP_PERF_BUF_FAST_N, RP_PERF_BUF_SLOW_N, RP_PERF_BUF_NOSPAN_N, RP_PERF_BUF_PAGES_N,   // 缓冲解析：快路命中 / 走逐页慢路 / 不受写跟踪 / 慢路里一共查了多少页
       RP_PERF_BUF_NOSPAN_FRESH_N,   // 其中不受写跟踪的池里、这一段本次提交里都同步过而免传的次数（page_fresh）
       RP_PERF_BUF_LOG_EV, RP_PERF_BUF_LOG_WORDS, RP_PERF_BUF_LOG_REBUILD,   // 脏页追加日志（nvk_buf_dirtylog）：灌进镜像的事件数 / 求交时看的位图字数 / 日志不可用、按逐页代号重建的次数
       RP_PERF_RES_FIND_NS, RP_PERF_RES_FAST_NS, RP_PERF_RES_SLOW_NS, RP_PERF_RES_N,   // 缓冲解析三段耗时：找池与取镜像 / 快路逐页核对 / 慢路同步（开关 nvk_res_tm）
       RP_PERF_RES_LW_NS, RP_PERF_RES_LA_NS,   // 批量解析里：抢锁等待 / 持锁解析（开关 nvk_res_tm）
       RP_PERF_LAND_CACHED_BYTES, RP_PERF_LAND_CACHED_NS, RP_PERF_LAND_UNCACHED_BYTES, RP_PERF_LAND_UNCACHED_NS,
       RP_PERF_LAND_KB, RP_PERF_LAND_N, RP_PERF_MIRROR_N,   // 落地（GPU 写回游戏内存）的千字节 / 次数；新建镜像次数
       RP_PERF_KEEP_OK, RP_PERF_KEEP_F_CMD, RP_PERF_KEEP_F_SEG, RP_PERF_KEEP_F_GEN, RP_PERF_KEEP_F_REG,   // 「整组沿用」成立次数与失败原因（命令变 / 段变 / 全局写代号变 / 贴图登记代号变）
       RP_PERF_KEEP_F_PG, RP_PERF_KEEP_F_BI, RP_PERF_KEEP_F_PIPESET, RP_PERF_KEEP_F_ITEMS,               // 继续：程序变 / base_instance 变 / 换过管线 / 逐项校验不过
       RP_PERF_SUB_PRE_NS, RP_PERF_SUB_EXEC_NS, RP_PERF_SUB_POST_NS, RP_PERF_WAIT_ALREADY_N, RP_PERF_GPU_BUSY_NS,   // 提交三段耗时 / 进等待时已完成 / GPU 忙区间
       RP_PERF_WAIT_FAKED_N,   // 实验开关 nvk_sync_lag 下「没真等」的等待次数（量重叠上限用，画面可能错）
       RP_PERF_TEX_LOG_N, RP_PERF_TEX_LOG_EV, RP_PERF_TEX_LOG_FALLBACK,   // CPU 贴图内容按脏页日志核对的次数 / 读到的事件数 / 日志不可用、退回整张逐页比的次数
       RP_PERF_TEX_UNARMED,    // CPU 贴图上传时有页加保护没落地（被并发的解保护挡住）：这次上传不算同步，下一次必重传
       RP_PERF_TEX_EPOCH_MISS, RP_PERF_TEX_CHECK_PAGES,   // CPU 贴图内容检查：本可按纪元跳过却没跳成的次数（已传过、没待重传、有跟踪句柄，但全局写代号 / 段号 / 映射代际变了）；逐页比对写代号的页数（每次调 nvn_dirty_since 记整个范围的页数，是上限：遇到脏页会提前停）
       RP_PERF_BINST_HZ, RP_PERF_BINST_ONE, RP_PERF_BINST_CUT, RP_PERF_BINST_SCAN, RP_PERF_BINST_WASTE,   // 间接绘制 base instance 拷贝：批量前移供上的笔数 / 逐笔拷的笔数 / 其中为它收了开着的通道 / 前移扫描次数 / 前移了没用上的笔数（rp_draw.c 通道级批量拷）
       RP_PERF_TM_FRAMES,      // 分时抽样：窗口里逐步计时过的帧数（#1103）；绘制各步 / 开通道细分 / 贴图解析等锁持锁按它求每帧值
       RP_PERF_GW_REG_N, RP_PERF_GW_REG_KB, RP_PERF_GW_MERGED_N, RP_PERF_GW_MERGED_KB,   // 显卡写登记：收到的登记条数 / 千字节；同段合并掉的条数 / 因重叠省下的千字节（落地拷贝按合并后的条数算，见 rp_buf.c rp_buf_gpu_write_note2）
       RP_PERF_P_CUT_BAR,      // 通道里有着色器写、其后游戏发了带顺序位的屏障，因此在下一笔绘制前结束的通道数（见 rp_pass.c rp_pass_barrier）
       RP_PERF_BUF_SYNC_GW_KB,   // 已登记 GPU 写覆盖整页，在途免同步的 KB
       RP_PERF_N };   // P_CONT：同目标续接命中的 SET_RT 次数；P_CLEARONLY：一笔绘制都没有的通道数；P_PARK：记进挂起表的待折清除条数；BIND_FAST：整组绑定直接沿用没重建；TEX_EPOCH：贴图内容检查按纪元跳过的次数；PC_HIT/MISS：后端开通道缓存命中 / 新建   /* FX_*：做完时要写回游戏内存的提交数，和各类回写的个数（事件 / 计数器报告 / 遮挡事件 / 显卡写缓冲） */   // TEX_UP_*：贴图整张上传的次数与千字节；BUF_SYNC_KB：池镜像同步的千字节   // P_*：通道的构成（见 rp_pass.c rp_pass_end）   // D_*：一笔绘制里各步（见 rp_draw.c）
void rp_perf_add(int i, uint64_t v);
// 分时读数抽样（#1103）：一笔绘制取 10 次时钟 + 9 次原子加、一次贴图解析取 3 次、一个通道取 5 次，世界稳态每帧合计约 2.2 万次取时钟，占回放线程约 3%。
// 改成每 rp_tm_stride() 帧只完整计 1 帧（缺省 32）：采样帧里逐步计时照旧，其余帧一次时钟都不取；纯计数的项不动、每帧都记。
// 「分时」里这几项按采样帧数（RP_PERF_TM_FRAMES）求每帧值，读数行的格式和含义不变。
// 调试开关（仅调试包读；release 恒为缺省）：nvk_tm_stride = 抽样间隔（帧，夹到 1..1024）；nvk_tm_full=1 = 每帧都计（等于原行为，同包 A/B 用）。
// 开关在会话重开（rp_draw_reset）时读；rp_tm_frame_end 在每次上屏时调一次（帧边界），由它定下一帧要不要计时。
extern int rp_tm_on;   // 1 = 当前这一帧要逐步计时
static inline int rp_tm(void) { return __atomic_load_n(&rp_tm_on, __ATOMIC_RELAXED); }
unsigned rp_tm_stride(void);   // 当前抽样间隔（帧）；1 = 每帧都计
void rp_tm_frame_end(void);    // 一帧结束（上屏）：刚结束的是采样帧就记一帧，再定下一帧
void rp_tm_reset(void);        // 会话重开：重读开关（rp_draw_reset 里调）
void rp_exec_dump_ops(char *b, size_t n);   // 逐命令类型耗时前几名（开关 nvk_exec_tm；窗口末尾取一次即清零）
void rp_sync_lag_present(void);   // 上屏之后调：给「每帧只放行一次同步等待」补一次额度（开关 nvk_sync_lag=3）
void rp_bind_tm_take(uint64_t *tot, uint64_t *backend, uint64_t *n);   // 绑定重建：总时间 / 其中交后端 / 次数（纳秒，取即清零）
void rp_bind_tm_take2(uint64_t *drv, uint64_t *buf, uint64_t *rest);
void rp_bind_tm_take3(uint64_t *alloc, uint64_t *copy);   // 暂存：申请 / 内容拷贝  // 其中：驱动段 / 常量缓冲与支持块 / 其它
// 回放线程要在 CPU 上改游戏内存 [addr, addr+len) 之前调：还没填完、来源与它相交的常量缓冲暂存先填完（代拷或等）（rp_draw.c，见 nvn_gpu_arena_fill）
void rp_ub_fill_fence(uint64_t addr, uint64_t len);
int rp_buf_gpu_pending(uint64_t addr, uint64_t len);   // 这段与还没落地的 GPU 写相交（rp_buf.c）
// 改 / 拷游戏内存的三条命令遇到还没落地的 GPU 写时改在 GPU 上按提交顺序做（#1022）；返回 0 = 不需要 / 做不了，调用方照旧走 CPU 版
int rp_op_update_ub_gpu(nvn_gpu_cmd c, const void *a, const void *rec); int rp_op_copy_b2b_gpu(nvn_gpu_cmd c, const void *a, const void *rec); int rp_op_clear_buf_gpu(nvn_gpu_cmd c, const p_clear_buf *cb, const void *rec);
uint64_t rp_sub_fence(void);        // 正在回放的这次提交最后一段的栅栏值
uint64_t rp_fx_report_types(void);  // 诊断读数：上次调用以来计数器报告用到的类型（位图，取完清零）
int rp_res_new_sizes_take(void);     // 诊断读数：上次调用以来新建贴图的宽高有几种（最多数到 64，取完清零）
int rp_fence_done(uint64_t f);       // 栅栏值 f 及之前交出去的活显卡都做完了
extern int rp_split_draws;       // 中途交段：每段的笔数（见 rp_exec.c）
extern int rp_split_pass_draws, rp_split_pass_max;   // 超长通道中途先交：每段笔数、每次提交最多切几次（见 rp_exec.c pass_split）
uint64_t rp_seg_serial(void);   // 第几个后端段：段里的暂存内容（arena 切片）只在同一段里能复用
void rp_exec_totals(uint64_t *exec_ns, uint64_t *subs);   // 进程累计：最外层回放用时 / 提交次数（与帧级账同源，只读）
void rp_pool_stats_frame(void);   // [上屏] 帧边界：读一次开关；开着时每 1024 帧打一行 S2A_POOLSTATS
void rp_pool_stats_end(void);     // 会话收尾：开着就再打一行（why=end）并关掉
void rp_frame_take(uint64_t *exec_ns, uint64_t *draws, uint64_t *subs);   // 取走本帧的回放耗时 / 绘制笔数 / 提交次数（长帧日志用）
// 逐帧分项账（长帧归因，开关 nvk_hitch_log）：5 秒窗口的 rp_perf 混了多帧，回答不了「这一帧的时间花在哪」。
// 这一组只在帧边界由上屏线程取走一次（取即清零），累计点与 rp_perf 的 D_* 同源（见 rp_exec.c 的 fr_perf_project）。
// 口径：取程序 / 开通道 / 取管线 / 绑定 / 后端绘制 / 收尾各占一笔绘制里的一段；「后端取与设」「管线缓存写盘」
// 嵌套在取管线里，只细分显示、不再加进合计。
enum { RP_FR_PROG, RP_FR_PASS, RP_FR_PIPE, RP_FR_PGET, RP_FR_BIND, RP_FR_DRAW, RP_FR_TAIL, RP_FR_PLC, RP_FR_N };
int rp_fr_perf_on(void);   // 开关（nvk_hitch_log，进程内缓存一次，与长帧日志同一个值）：关着时帧级分项不做任何累计
void rp_fr_perf_take(uint64_t out[RP_FR_N]);   // [上屏] 取走本帧分项并清零；每帧只调一次（与 rp_frame_take 同一个帧边界）
int rp_hitch_line(char *b, size_t n, uint64_t gap_ns, uint64_t exec_ns, uint64_t draws, uint64_t subs, const uint64_t fr[RP_FR_N]);   // 长帧行（见 rp_exec.c）
uint64_t rp_sub_serial(void);   // 第几次提交（一次提交内可复用的暂存内容按它判）
void rp_perf_take(uint64_t out[RP_PERF_N]);

// 给可选后端策略的同窗读数。原生栅栏、热档和分配真值仍由后端提供。
struct rp_mirror_auto_sample {
    uint64_t elapsed_ns, frames, draws, submits, target_ns;
    uint64_t cached_bytes, uncached_bytes, cached_ns, uncached_ns;
    uint64_t sync_kib, direct_kib, acquire_ns, present_ns;
    double p99_ms;
    int compiling, target_changed;
};

enum { RP_MIRROR_SHADOW_SPANS = 2048 };
void rp_draw_reset(void);
void rp_draw_log_arm(int lines);   // 接下来的 lines 笔绘制（含被跳过的）各出一行日志；0 = 关

// 资源同步用的段（见 rp_exec.c）：通常是先于主段提交的前置段；同一资源本次提交里第二次同步时结束当前通道、回到主段。
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub);
// 遮挡计数（rp_exec.c）：通道与绘制那边的接入点
int rp_occl_want(void);                 // 开通道时：要不要声明遮挡计数
void rp_occl_pass_opened(int declared); void rp_occl_pass_closed(void);
void rp_occl_pause(nvn_gpu_cmd c);      // 通道里按矩形清之前
void rp_occl_draw(nvn_gpu_cmd c);       // 每笔绘制之前
void rp_dispatch(nvn_gpu_cmd c, const p_dispatch *dp, const void *rec);   // 计算派发（rp_draw.c）
int rp_post_gpu_write(uint64_t addr, uint64_t len);                       // 登记单段；1 = 登记成功，段完成后回写
int rp_post_gpu_write_rows(uint64_t addr, uint64_t row_bytes, uint64_t row_stride, uint64_t image_stride, uint32_t rows, uint32_t images);
// [addr, addr + len) 有没有还没落地的 GPU 写（登记中或正在锁外拷回）：1 有、0 没有、-1 地址不在登记过的池里。线程：[提交]。
int rp_buf_gpu_write_pending(uint64_t addr, uint64_t len);
int rp_buf_read_pointer(nvn_gpu_buf b, uint64_t off, uint64_t addr, uint64_t *value);
struct gw_ev { uint64_t pool_key, id; };   // 提交里 GPU 写池的一笔登记
void rp_buf_gpu_write_snapshot(const struct gw_ev *refs, uint32_t count, nvn_gpu_mirror_span *spans);
uint64_t rp_buf_gpu_write_note(uint64_t addr, uint64_t len, uint64_t *pool_key); void rp_buf_gpu_write_land(uint64_t pool_key, uint64_t id, int ok);
// 同段合并版登记（merged 非空才合并，开关 nvk_land_merge）：本次提交里相交 / 首尾相接的已有登记并成一条，*merged = 1、返回已有那条的登记号（不要再记新落地项）。
uint64_t rp_buf_gpu_write_note2(uint64_t addr, uint64_t len, uint64_t *pool_key, int *merged);
int rp_rc_skip(void);                   // 条件渲染判成不画的那一段里
enum { RP_NOTE_DS = 1, RP_NOTE_SHADER_WRITE = 2 };   // rp_pass_note_draw 的位
void rp_pass_note_draw(int note);   // 一笔绘制进了当前通道（RP_NOTE_DS：这笔开着深度测试 / 深度写 / 模板测试；RP_NOTE_SHADER_WRITE：这笔的着色器写存储缓冲 / 存储图像）
int rp_pass_slot_attached(int i);   // 当前目标的颜色槽 i 真的挂了附件（有图，且不是重复挂的同一张）：管线描述按它给格式
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h);   // 颜色槽 i 的视图尺寸（像素；块压缩基底配一块当一个像素的视图时按块数）
void rp_pass_noatt_size(uint32_t *w, uint32_t *h);   // 当前是无附件通道时给它的帧缓冲尺寸（缺省视口 / 裁剪夹取用）；不是就不动 *w *h
// 调试读回（rp_draw.c）：上屏线程登记，提交线程在一次提交收尾时执行；fence 是提交线程的时间线值，每读一张 +1
void rp_debug_dump_request(uint64_t present_serial, uint64_t frame);
void rp_debug_dump_poll(uint64_t *fence);

// #653：无能力 / 段超 64 槽时沿用 guest_tick 的报告数（OOM 不伪造读数），取走清零。
uint64_t rp_timestamp_fallback_take(void);
