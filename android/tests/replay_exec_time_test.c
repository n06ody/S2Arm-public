// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 回放计时与长帧归因（graphics/nvn/replay/rp_exec.c）的宿主单测：帧级「回放耗时」、逐命令耗时、
// 逐帧分项账各只许记一次（长帧行的格式由生产函数 rp_hitch_line 出，测试不重写）。
//
// 被测的就是生产源文件本身：rp_exec.c 整个 #include 进本 TU，时钟换成本文件的虚拟钟
// （rp_exec.c 里的 S2A_RP_EXEC_TEST_CLOCK 分支；生产构建不带这个宏，仍走 CLOCK_MONOTONIC）。
// 编译命令里不要再列 rp_exec.c，否则符号重复：
//   cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include
//      -Igraphics/nvn/gfx/tex -Igraphics/nvn/replay android/tests/replay_exec_time_test.c -lm -o /tmp/replay_exec_time_test
//   /tmp/replay_exec_time_test        # 生产缺省：nvk_exec_tm 关，验帧级账与绘制笔数
//   /tmp/replay_exec_time_test tm     # 打开 nvk_exec_tm，另外验逐命令账里 OP_CALL 只记本层
//
// 期望值全是手算的：命令流里的「干活」由替身把虚拟钟推进 1 毫秒，嵌套本身不推进时间。
//   三层 OP_CALL 嵌套、每层 1 毫秒自己的活 → 最外层一次调用累计 3 毫秒（一帧的墙钟就这么长）；
//   修前每层在 OP_END 各自把「含子层」的整段加进 g_fr_exec_ns：1 + 2 + 3 = 6 毫秒。
//   真机「回放 779 毫秒 > 帧间隔 227.5 毫秒」就是这一条（11-14 安卓新回放层 45 秒采集）。
//   非嵌套的 3 条命令直落 = 3 毫秒；同样这 3 条命令摊在三层里也必须还是 3 毫秒（不随层数变）。
//   坏记录（rec_bad）提前结束的列表：已经花掉的 1 毫秒要记上（修前这条路直接 return，漏计）。
//   逐命令账：三层子列表一共干了 3 毫秒，全部记在各自的命令（这里都是 OP_UPDATE_UB）上；g_op_ns[OP_CALL]
//   必须还是 0 —— 子列表里每条命令已经各自记过，把整段记进 OP_CALL 就是把同一批命令算两遍。
// 长帧归因（⑥–⑨）：一帧 3 笔绘制（第 1 笔里有 1 次开通道、1 次取管线（其中后端取与设 0.5 毫秒））
//   加 2 毫秒没归因的活 → 分项各按手算值只记一次、分项和 11 ≤ 回放 13；摊进三层 OP_CALL 后分项必须不变；
//   长帧行由生产函数 rp_hitch_line 生成（测试只比对期望字符串，不重写格式逻辑）；取走一次即清零。
#define S2A_RP_EXEC_TEST_CLOCK 1
#include "../../graphics/nvn/replay/rp_exec.c"

#include <stdio.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("【红】 %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define MS 1000000ull

// 回放状态（rp_state.c 的全局）：本用例只把 rp_exec.c 编进来，给它一份零值状态让链接通过；
// 状态类命令都走 rp_state_op 替身，内容不参与这里的计时判断。
struct rp_state rp_S;
struct rp_target rp_R;

static uint64_t g_vt;   // 虚拟钟（纳秒）：只有「干活」替身推进它，回放实现自己不许动
static int g_tm;        // 1 = 打开 debug.s2a.nvk_exec_tm（第二遍运行）；rp_exec.c 在第一次回放时读一次
static int g_hitch = 1; // 1 = 打开 debug.s2a.nvk_hitch_log（长帧日志 / 逐帧分项账）；rp_exec.c 在第一次累加时读一次
uint64_t s2a_rp_test_clock_ns(void) { return g_vt; }   // 供 rp_exec.c 的 now_ns（见文件头）

// ---------- 平台 / 未接 / 后端 / 回放核心其余文件的替身 ----------
// 本用例只走 nvn_be_exec 的循环：状态、通道、传输、绘制、资源都不在这里验，给出链接需要的空实现。
static int g_red; static char g_last_red[200];
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)cls; if (level != NVN_PLAT_LOG_RED) return;
    g_red++; snprintf(g_last_red, sizeof g_last_red, "%s", text);
}
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_exec_tm")) return g_tm;
    if (!strcmp(name, "nvk_hitch_log")) return g_hitch;   // 长帧日志 / 逐帧分项账的开关
    return dflt;
}
void nvn_plat_safepoint(void) {}
static const char *g_unimpl_name;
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) { (void)cls; (void)site; g_unimpl_name = name; return 1; }

static nvn_gpu_rc ok_rc(void) { return (nvn_gpu_rc){NVN_GPU_OK}; }
nvn_gpu_rc nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *d, nvn_gpu_cmd *out, nvn_gpu_err *err) { (void)d; (void)err; *out = NULL; return ok_rc(); }
void nvn_gpu_cmd_abort(nvn_gpu_cmd c) { (void)c; }
nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd c, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    (void)c; (void)signal; (void)done; (void)user; (void)err; return ok_rc();
}
nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t timeout_ns, nvn_gpu_err *err) { (void)v; (void)timeout_ns; (void)err; return ok_rc(); }
void nvn_gpu_occlusion(nvn_gpu_cmd c, int32_t slot) { (void)c; (void)slot; }
// 本用例不发 ReportCounter；无时间戳能力，误走采样 / 换钟路径必须报红。
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); }
void nvn_gpu_timestamp(nvn_gpu_cmd c, uint32_t slot, uint32_t type) {
    (void)c; (void)slot; (void)type; CHECK(0);
}

int s2a_nvn_render_cond(void) { return 0; }
uint64_t s2a_guest_tick(void) { return 0; }
int s2a_guest_tick_from_host_ns(uint64_t ns, uint64_t *out) { (void)ns; (void)out; CHECK(0); return 0; }
void nvn_hle_queue_enter(void *q) { (void)q; }
void nvn_hle_piece_begun(void) {}
int nvn_cmd_call_resolve(uint64_t h, uint64_t *to) { *to = h; return 0; }   // hle_nvn.c 的 CALL 目标解析（main 的调用目标安全阀）：本用例只量耗时，照原地址执行

const void *rp_draw_stream; uint64_t rp_gpu_wr_gen;   // rp_draw.c 的批量拷接口（本用例不链 rp_draw.c）
void rp_state_reset(void) {}
int rp_state_op(uint32_t op, const void *a, uint32_t size) { (void)op; (void)a; (void)size; return 0; }
void rp_dirty_all(void) {}
int rp_pass_is_open(void) { return 0; }
int rp_pass_draws(void) { return 0; }
int rp_pass_split_ok(void) { return 0; }
void rp_pass_end(nvn_gpu_cmd c) { (void)c; }
void rp_pass_flush(nvn_gpu_cmd c) { (void)c; }
void rp_pass_barrier(uint32_t bits) { (void)bits; }
void rp_pass_clear_color(nvn_gpu_cmd c, const p_clear_color *cc, const void *rec) { (void)c; (void)cc; (void)rec; }
void rp_pass_clear_ds(nvn_gpu_cmd c, const p_clear_ds *cd, const void *rec) { (void)c; (void)cd; (void)rec; }
void rp_op_copy_t2b(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec) { (void)c; (void)o; (void)rec; }
void rp_op_copy_b2t(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec) { (void)c; (void)o; (void)rec; }
void rp_op_copy_t2t(nvn_gpu_cmd c, const p_copy_t2t *o, const void *rec) { (void)c; (void)o; (void)rec; }
void rp_op_clear_tex(nvn_gpu_cmd c, const p_clear_tex *o, const void *rec) { (void)c; (void)o; (void)rec; }
void rp_op_draw_tex(nvn_gpu_cmd c, const p_draw_tex *o, const void *rec) { (void)c; (void)o; (void)rec; }
void rp_op_copy_b2b(const void *a, const void *rec) { (void)a; (void)rec; }
void rp_op_clear_buf(const p_clear_buf *cb, const void *rec) { (void)cb; (void)rec; }
int rp_op_update_ub_gpu(nvn_gpu_cmd c, const void *a, const void *rec) { (void)c; (void)a; (void)rec; return 0; }   // 没有还没落地的 GPU 写：照旧走 CPU 版（计时口径不变）
int rp_op_copy_b2b_gpu(nvn_gpu_cmd c, const void *a, const void *rec) { (void)c; (void)a; (void)rec; return 0; }
int rp_op_clear_buf_gpu(nvn_gpu_cmd c, const p_clear_buf *cb, const void *rec) { (void)c; (void)cb; (void)rec; return 0; }
void rp_draw_stats(uint64_t *draws, uint64_t *skips) { if (draws) *draws = 0; if (skips) *skips = 0; }
void rp_dispatch(nvn_gpu_cmd c, const p_dispatch *dp, const void *rec) { (void)c; (void)dp; (void)rec; }
// OP_MULTI_DRAW（main 新增的整段间接计数多绘）在 rp_draw.c：本测试不测它，给个空实现闭合链接。
void rp_multi_draw(nvn_gpu_cmd c, const p_multi_draw *m, const void *rec) { (void)c; (void)m; (void)rec; }
void rp_debug_dump_poll(uint64_t *fence) { (void)fence; }
uint64_t rp_buf_gpu_write_note(uint64_t addr, uint64_t len, uint64_t *pool_key) { (void)addr; (void)len; if (pool_key) *pool_key = 0; return 0; }
uint64_t rp_buf_gpu_write_note2(uint64_t addr, uint64_t len, uint64_t *pool_key, int *merged) { (void)addr; (void)len; if (pool_key) *pool_key = 0; if (merged) *merged = 0; return 0; }   // 同段合并的登记入口（rp_post_gpu_write_rows 用）：本测试只测计时，不登记
void rp_buf_gpu_write_land(uint64_t pool_key, uint64_t id, int ok) { (void)pool_key; (void)id; (void)ok; }
// 计时流不登记 GPU 写，也不制造提交失败。误入回写或传输撤销路径要让用例报红。
void rp_buf_gpu_write_snapshot(const struct rp_gpu_write_ref *refs, uint32_t count, nvn_gpu_readback_span *spans) {
    (void)refs; (void)count; (void)spans; CHECK(0);
}
void rp_buf_gpu_write_land_from(uint64_t pool_key, uint64_t id, int ok, const uint8_t *staged) {
    (void)pool_key; (void)id; (void)ok; (void)staged; CHECK(0);
}
void rp_buf_frame_flip(void) {}
void rp_buf_hold_begin(void) {}   // 本测试只测计时：回放窗口（池销毁的镜像缓冲延后放）在 t_nvn_replay / 隔离夹具里测
void rp_buf_hold_end(void) {}

// 「干活」的三条命令：每执行一条推进虚拟钟 1 毫秒。推进只发生在替身里：
//   回放实现无论嵌套几层都不许让时间自己走，所以「时间过了多少」就是「干了几件活」。
void rp_op_update_ub(const void *a, const void *rec) { (void)a; (void)rec; g_vt += MS; }
void rp_pass_set_rt(nvn_gpu_cmd c, const p_set_rt *r) { (void)c; (void)r; g_vt += MS; }
// 长帧归因用例的绘制替身：按生产 rp_draw 的先后顺序、用同一个 rp_perf_add 记各步，虚拟钟推进多少就记多少
// （所以「分项和 ≤ 帧回放」是手算得出的）。g_draw_plan：0 = 前面的计时用例（一笔 1 毫秒、不记分项）；
// 1 = 归因用例，3 笔：第 1 笔含 1 次开通道（1 毫秒）与 1 次取管线（1 毫秒，其中后端取与设 0.5 毫秒）。
static int g_draw_plan, g_draw_n;
void rp_draw(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di, const void *rec) {
    (void)c; (void)dr; (void)di; (void)rec;
    if (!g_draw_plan) { g_vt += MS; return; }
    int first = g_draw_n++ == 0;
    g_vt += MS / 2; rp_perf_add(RP_PERF_D_PROG, MS / 2);                                            // 取程序
    if (first) { g_vt += MS; rp_perf_add(RP_PERF_D_PASS, MS); }                                     // 开通道
    if (first) { g_vt += MS; rp_perf_add(RP_PERF_D_PIPE, MS); rp_perf_add(RP_PERF_D_PGET, MS / 2); } // 取管线（其中后端取与设是子项，不另走钟）
    g_vt += MS; rp_perf_add(RP_PERF_D_BIND, MS);                                                    // 绑定
    g_vt += MS; rp_perf_add(RP_PERF_D_DRAW, MS);                                                    // 后端绘制
    g_vt += MS / 2; rp_perf_add(RP_PERF_D_TAIL, MS / 2);                                            // 收尾
}

// ---------- 命令记录 ----------
// 记录 = 8 字节头（op, size）+ 载荷，size 含头且 8 字节对齐（nvn_obj.h）。
static void put_hdr(uint8_t *b, uint32_t op, uint32_t size) { op_hdr h = {op, size}; memcpy(b, &h, sizeof h); }
static void put_work(uint8_t *b, uint32_t op) { put_hdr(b, op, 8); }   // 不带载荷的「干活」记录：OP_UPDATE_UB / OP_DRAW / OP_SET_RT
static void put_call(uint8_t *b, const uint8_t *target) { put_hdr(b, OP_CALL, 16); uint64_t t = (uint64_t)(uintptr_t)target; memcpy(b + 8, &t, 8); }
static void put_end(uint8_t *b) { put_hdr(b, OP_END, 8); }

// 回放一次命令列表，取走本帧的帧级账（与 rp_present 取账同一个入口），返回回放耗时（纳秒）。
static uint64_t run_frame(const uint8_t *list, uint64_t *draws, uint64_t *subs) {
    nvn_be_exec(list);
    uint64_t exec_ns = 0, dr = 0, sb = 0;
    rp_frame_take(&exec_ns, &dr, &sb);
    if (draws) *draws = dr;
    if (subs) *subs = sb;
    return exec_ns;
}

// ---------- 长帧归因的手算期望（纳秒）----------
// 3 笔绘制：每笔都是「取程序 0.5 → 绑定 1 → 后端绘制 1 → 收尾 0.5」；
// 第 1 笔另有「开通道 1」「取管线 1（其中后端取与设 0.5，嵌套在里面，只细分不进合计）」。
// 整帧另有 2 毫秒没归因的活（两条 OP_UPDATE_UB）→ 帧回放 = 11 + 2 = 13 毫秒。
static const uint64_t k_attr[RP_FR_N] = {
    [RP_FR_PROG] = 1500000, [RP_FR_PASS] = 1000000, [RP_FR_PIPE] = 1000000, [RP_FR_PGET] = 500000,
    [RP_FR_BIND] = 3000000, [RP_FR_DRAW] = 3000000, [RP_FR_TAIL] = 1500000, [RP_FR_PLC] = 0,
};
static const char *const k_attr_name[RP_FR_N] = {
    "取程序", "开通道", "取管线", "后端取设", "绑定", "后端绘制", "收尾", "管线缓存写盘",
};
// 逐项核对（一项一条检查；红时打出期望 → 实测，负对照就红在这几条上）。
static void check_attr(const char *tag, const uint64_t fr[RP_FR_N]) {
    for (int i = 0; i < RP_FR_N; i++) {
        g_checks++;
        if (fr[i] != k_attr[i]) { g_fail++; printf("【红】 %s：本帧%s 期望 %llu 纳秒 → 实测 %llu 纳秒\n", tag, k_attr_name[i], (unsigned long long)k_attr[i], (unsigned long long)fr[i]); }
    }
}

int main(int argc, char **argv) {
    g_tm = argc > 1 && !strcmp(argv[1], "tm");   // 第二遍运行：打开逐命令计时（第一次回放时生效）

    // ① 三层 OP_CALL 嵌套：每层先干 1 毫秒自己的活，再调下一层。
    //    L2 = [干活][END]，L1 = [干活][CALL→L2][END]，L0 = [干活][CALL→L1][END]。
    //    手算：L2 这一层墙钟 1 毫秒、L1 2 毫秒、L0 3 毫秒；帧级「回放耗时」就应当是最外层的 3 毫秒。
    uint8_t l2[16], l1[32], l0[32];
    put_work(l2, OP_UPDATE_UB); put_end(l2 + 8);
    put_work(l1, OP_UPDATE_UB); put_call(l1 + 8, l2); put_end(l1 + 24);
    put_work(l0, OP_UPDATE_UB); put_call(l0 + 8, l1); put_end(l0 + 24);
    uint64_t draws = 0, subs = 0, op_update_before = g_op_ns[OP_UPDATE_UB], op_call_before = g_op_ns[OP_CALL];
    uint64_t ex = run_frame(l0, &draws, &subs);
    // 逐命令账只取 ① 这一段（后面的用例还会往 OP_UPDATE_UB 上加数）：子列表一共干了 3 毫秒，OP_CALL 自身应当是 0。
    uint64_t op_update_nested = g_op_ns[OP_UPDATE_UB] - op_update_before;
    uint64_t op_call_nested = g_op_ns[OP_CALL] - op_call_before;
    printf("[1] 三层 OP_CALL 嵌套（每层 1 毫秒）：期望 回放 3.000 毫秒 → 实测 回放 %.3f 毫秒（绘制 %llu 笔、提交 %llu 次）\n",
           (double)ex / 1e6, (unsigned long long)draws, (unsigned long long)subs);
    CHECK(ex == 3 * MS);   // 修前 = 6 毫秒：子层先记 1、父层再记含子层的 2 和 3
    CHECK(draws == 0);
    CHECK(subs == 0);

    // ② 非嵌套直落：3 条「干活」命令各 1 毫秒 → 帧级账 = 各命令时间和 = 3 毫秒，与层数无关。
    uint8_t flat[32];
    put_work(flat, OP_DRAW); put_work(flat + 8, OP_SET_RT); put_work(flat + 16, OP_DRAW); put_end(flat + 24);
    ex = run_frame(flat, &draws, &subs);
    printf("[2] 非嵌套（DRAW / SET_RT / DRAW 直落）：期望 回放 3.000 毫秒、绘制 2 笔 → 实测 回放 %.3f 毫秒、绘制 %llu 笔\n",
           (double)ex / 1e6, (unsigned long long)draws);
    CHECK(ex == 3 * MS);
    CHECK(draws == 2);

    // ③ 同样这 3 条命令摊进三层：帧级账仍是 3 毫秒（不随层数变），绘制笔数也不变。
    uint8_t n2[16], n1[32], n0[32];
    put_work(n2, OP_DRAW); put_end(n2 + 8);
    put_work(n1, OP_SET_RT); put_call(n1 + 8, n2); put_end(n1 + 24);
    put_work(n0, OP_DRAW); put_call(n0 + 8, n1); put_end(n0 + 24);
    ex = run_frame(n0, &draws, &subs);
    printf("[3] 同上 3 条命令摊到三层：期望 回放 3.000 毫秒、绘制 2 笔 → 实测 回放 %.3f 毫秒、绘制 %llu 笔\n",
           (double)ex / 1e6, (unsigned long long)draws);
    CHECK(ex == 3 * MS);   // 修前 = 1 + 2 + 3 = 6 毫秒
    CHECK(draws == 2);

    // ④ 坏记录提前结束：已经干掉的 1 毫秒要记上（这条列表按它的 END 处理；修前直接 return，帧级账漏掉这 1 毫秒）。
    uint8_t bad[16];
    put_work(bad, OP_UPDATE_UB); put_hdr(bad + 8, OP_NVN2_END_, 8);   // op 越界 = rec_bad
    ex = run_frame(bad, &draws, &subs);
    printf("[4] 坏记录提前结束（先干 1 毫秒）：期望 回放 1.000 毫秒 → 实测 回放 %.3f 毫秒（未接登记 \"%s\"）\n",
           (double)ex / 1e6, g_unimpl_name ? g_unimpl_name : "（无）");
    CHECK(ex == 1 * MS);
    CHECK(g_unimpl_name && !strcmp(g_unimpl_name, "bad_record"));

    // ⑤ 逐命令账（只在 nvk_exec_tm 打开时）：①里子层一共干了 3 毫秒，各记在 OP_UPDATE_UB 上；
    //    OP_CALL 只该记递归自身开销（虚拟钟没在递归里自己走，所以是 0）。
    //    把整段 OP_CALL 记进去的实现会让这里变成 2 毫秒（最外层那次调用的子列表耗时）。
    if (g_tm) {
        uint64_t op_update = op_update_nested, op_call = op_call_nested;
        printf("[5] 逐命令账（nvk_exec_tm 开）：期望 OP_UPDATE_UB 3.000 毫秒、OP_CALL 0.000 毫秒 → 实测 %.3f 毫秒、%.3f 毫秒\n",
               (double)op_update / 1e6, (double)op_call / 1e6);
        CHECK(op_update == 3 * MS);
        CHECK(op_call == 0);   // 子列表已在各自命令里记过，这里再记就是算两遍
    } else {
        printf("[5] 逐命令账：本轮 nvk_exec_tm 关（生产缺省），此项不验；带 tm 参数再跑一遍\n");
    }
    // ⑥ 长帧归因（平铺）：3 笔绘制 + 第 1 笔里的 1 次开通道 / 1 次取管线 + 2 条没归因的活。
    //    分项各按手算值只记一次；分项和 11 毫秒 ≤ 帧回放 13 毫秒（2 毫秒是还没归因的活）。
    g_draw_plan = 1;
    uint8_t attr[48];
    put_work(attr, OP_DRAW); put_work(attr + 8, OP_DRAW); put_work(attr + 16, OP_DRAW);
    put_work(attr + 24, OP_UPDATE_UB); put_work(attr + 32, OP_UPDATE_UB); put_end(attr + 40);
    uint64_t fr[RP_FR_N];
    rp_fr_perf_take(fr);   // 归因用例开始前先清一次（起点的账不属于这一帧；正常实现本来就是 0）
    g_draw_n = 0;
    uint64_t aex = run_frame(attr, &draws, &subs);
    rp_fr_perf_take(fr);   // 帧边界取一次（与 rp_present 同一个取法）
    {
        uint64_t sum = fr[RP_FR_PROG] + fr[RP_FR_PASS] + fr[RP_FR_PIPE] + fr[RP_FR_BIND] + fr[RP_FR_DRAW] + fr[RP_FR_TAIL];
        printf("[6] 长帧归因（平铺 3 笔 + 开通道 / 取管线各 1 次）：期望 分项和 11.000 ≤ 回放 13.000 毫秒 → 实测 分项和 %.3f ≤ 回放 %.3f 毫秒（绘制 %llu 笔）\n",
               (double)sum / 1e6, (double)aex / 1e6, (unsigned long long)draws);
        CHECK(aex == 13 * MS);   // 11 毫秒归因 + 2 毫秒没归因的活
        CHECK(draws == 3);
        CHECK(sum == 11 * MS);
        CHECK(sum <= aex);
    }
    check_attr("[6] 平铺", fr);

    // ⑦ 长帧行：生产函数 rp_hitch_line（rp_present 的长帧日志就调它）。既有格式一字不动，末尾追加「本帧」分项；
    //    管线缓存写盘由上屏从后端统计项填（这里注入 4 毫秒，等价于后端 pipe_cache_save_trigger_ns 的帧差分）。
    {
        uint64_t frp[RP_FR_N]; memcpy(frp, fr, sizeof frp);
        frp[RP_FR_PLC] = 4 * MS;
        char line[512];
        int need = rp_hitch_line(line, sizeof line, 227500000ull, aex, draws, 2, frp);
        const char *want = "长帧 227.5 毫秒（上一帧：回放 13.0 毫秒、绘制 3 笔、提交 2 次）；本帧 取程序 1.50、开通道 1.00、取管线 1.00（后端取设 0.50、管线缓存写盘 4.00）、绑定 3.00、后端绘制 3.00、收尾 1.50 毫秒；合计 11.00 毫秒（回放里其余 2.00 毫秒）";
        printf("[7] 长帧行：%s\n", line);
        CHECK(need > 0 && (size_t)need < sizeof line);
        CHECK(!strcmp(line, want));
    }

    // ⑧ 同样 3 笔摊进三层 OP_CALL：分项必须与平铺完全相同（嵌套不重复计），帧回放仍 13 毫秒。
    uint8_t d2[16], d1[40], d0[40];
    put_work(d2, OP_DRAW); put_end(d2 + 8);
    put_work(d1, OP_DRAW); put_call(d1 + 8, d2); put_work(d1 + 24, OP_UPDATE_UB); put_end(d1 + 32);
    put_work(d0, OP_DRAW); put_call(d0 + 8, d1); put_work(d0 + 24, OP_UPDATE_UB); put_end(d0 + 32);
    g_draw_n = 0;
    aex = run_frame(d0, &draws, &subs);
    rp_fr_perf_take(fr);
    {
        uint64_t sum = fr[RP_FR_PROG] + fr[RP_FR_PASS] + fr[RP_FR_PIPE] + fr[RP_FR_BIND] + fr[RP_FR_DRAW] + fr[RP_FR_TAIL];
        printf("[8] 同上 3 笔摊到三层 OP_CALL：期望 分项和 11.000（与平铺一致）、回放 13.000 毫秒 → 实测 分项和 %.3f、回放 %.3f 毫秒（绘制 %llu 笔）\n",
               (double)sum / 1e6, (double)aex / 1e6, (unsigned long long)draws);
        CHECK(aex == 13 * MS);
        CHECK(draws == 3);
        CHECK(sum == 11 * MS);
    }
    check_attr("[8] 三层嵌套", fr);

    // ⑨ 取走即清零（每帧只取一次）：再回放一帧没有归因的活（1 条没归因的命令），分项全 0、帧回放 1 毫秒。
    uint8_t quiet[16]; put_work(quiet, OP_UPDATE_UB); put_end(quiet + 8);
    aex = run_frame(quiet, &draws, &subs);
    rp_fr_perf_take(fr);
    {
        int zero = 1; for (int i = 0; i < RP_FR_N; i++) zero &= fr[i] == 0;
        printf("[9] 取走即清零：期望 分项全 0、回放 1.000 毫秒 → 实测 分项%s、回放 %.3f 毫秒\n", zero ? "全 0" : "有残留", (double)aex / 1e6);
        CHECK(aex == 1 * MS);
        CHECK(draws == 0);
        CHECK(zero);
    }

    CHECK(g_red == 0);   // 这些路径不该报红行
    if (g_red) printf("      最近一条红行：%s\n", g_last_red);

    printf("%s：%d 项检查，%d 红\n", g_tm ? "replay_exec_time(tm)" : "replay_exec_time", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
