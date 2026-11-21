#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""回放计时宿主单测的驱动（C 侧见 replay_exec_time_test.c）。

C 文件把 graphics/nvn/replay/rp_exec.c 整个编进自己的 TU（被测的就是生产源；测试在 include 前定义
S2A_RP_EXEC_TEST_CLOCK 把 now_ns 换成虚拟钟），所以编译命令里不能再列 rp_exec.c，否则符号重复。

跑两遍，任一遍非 0 即失败：
  · 缺省：nvk_exec_tm 关（生产缺省），验帧级「回放耗时」、绘制笔数与长帧归因分项（分项各记一次、
    分项和 ≤ 帧回放、三层 OP_CALL 嵌套不重复、取走即清零、长帧行格式）；
  · tm：再打开逐命令计时，另验 g_op_ns[OP_CALL] 只记递归自身开销、不含子列表。
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
CC = shutil.which('cc') or shutil.which('clang')
INCLUDES = [REPO, REPO / 'graphics/nvn/gfx/nvn', REPO / 'graphics/nvn/gfx/nvn/include',
            REPO / 'graphics/nvn/gfx/tex', REPO / 'graphics/nvn/replay']


WRAPPER = r"""#define S2A_RP_EXEC_TEST_COUNT_EXIT 1
#define main regression_main
#include "SOURCE_C"
#undef main
static unsigned count_exit_calls, count_exit_pending;
void s2a_rp_test_count_exit(uint32_t pending) { count_exit_calls++; count_exit_pending = pending; CHECK(pending == 0); }
// #1266 private validation prototype: missing batch symbol, same timing draw stub.
static uint32_t batch_n, batch_calls;
void rp_draw_count_batch(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di,
                         const void *rec, uint32_t n, uint64_t stride) {
    (void)stride; batch_n = n; batch_calls++;
    rp_draw(c, dr, di, rec);
}

static void put_count(uint8_t *b, int seq) {
    p_draw d = {0}; d.prim=3; d.ind=0x1000u+(uint64_t)seq*16u;
    d.mdi_cnt=0x9000; d.mdi_i=seq; d.count=seq+1;
    put_hdr(b, OP_DRAW, 8+sizeof d); memcpy(b+8,&d,sizeof d);
}
static void check_count1266(void) {
    enum { N=8+sizeof(p_draw) };
    uint8_t a[N*2+16]={0}, b[N*2+8]={0};
    put_count(a,0);put_count(a+N,1);put_hdr(a+2*N,OP_JUMP,16);
    const uint8_t *target=b;memcpy(a+2*N+8,&target,8);
    put_count(b,2);put_count(b+N,3);put_end(b+2*N);
    batch_calls=0;batch_n=0;exec_list(a);
    CHECK(batch_calls==1);CHECK(batch_n==4);
    uint8_t endcase[N+8]={0};put_count(endcase,0);put_end(endcase+N);
    count_exit_calls=0;batch_calls=0;exec_list(endcase);CHECK(batch_calls==1);CHECK(batch_n==1);CHECK(count_exit_calls==1);CHECK(count_exit_pending==0);
    put_hdr(endcase+N,OP_END,7);count_exit_calls=0;batch_calls=0;exec_list(endcase);CHECK(count_exit_calls==1);CHECK(count_exit_pending==0);
    CHECK(batch_calls==1);CHECK(batch_n==1);CHECK(!strcmp(g_unimpl_name,"bad_record"));
    count_exit_calls=0;batch_calls=0;exec_list(NULL);CHECK(batch_calls==0);CHECK(count_exit_calls==1);CHECK(count_exit_pending==0);
    // Prefix is submitted before a state command; later nonzero records are not re-grouped.
    uint8_t statecase[N*3+16]={0};put_count(statecase,0);put_count(statecase+N,1);
    put_work(statecase+2*N,OP_UPDATE_UB);put_count(statecase+2*N+8,2);put_end(statecase+3*N+8);
    batch_calls=0;exec_list(statecase);CHECK(batch_calls==1);CHECK(batch_n==2);
    // 1024 is the immediate submission boundary; the following nonzero record does not open a group.
    uint8_t *limit=calloc(1025*N+8,1);CHECK(limit!=NULL);
    if(limit) { for(int i=0;i<1025;i++)put_count(limit+i*N,i);put_end(limit+1025*N);
        batch_calls=0;exec_list(limit);CHECK(batch_calls==1);CHECK(batch_n==1024);free(limit); }
    // Real render-enable records: submit prefix, skip a draw, resume with a new zero sequence.
    enum { R=8+sizeof(p_render_cond) };
    uint8_t cond[N*3+R*2+8]={0};p_render_cond off={3,0,0},on={2,0,0};
    put_count(cond,0);put_hdr(cond+N,OP_RENDER_COND,R);memcpy(cond+N+8,&off,sizeof off);
    put_count(cond+N+R,0);put_hdr(cond+2*N+R,OP_RENDER_COND,R);memcpy(cond+2*N+R+8,&on,sizeof on);
    put_count(cond+2*N+2*R,0);put_end(cond+3*N+2*R);
    batch_calls=0;exec_list(cond);CHECK(batch_calls==2);CHECK(batch_n==1);CHECK(g_rc_skip==0);
    // Direct state of production grouping helper, separate from exec_list exit observations.
    struct rp_count_group group={0};p_draw d={0};d.prim=3;d.ind=0x1000;d.mdi_cnt=0x9000;
    count_feed(&group,&d,NULL,NULL,0);CHECK(group.n==1);
    count_flush(&group,0);CHECK(group.n==0);
}

// 段失败行的「本段末笔程序键」（rp_exec.c seg_pk）：键随段快照、完成回调在另一线程读不到活表。
// 覆盖：mid 段 done 失败、seg_cut 里前置段提交失败、末段提交失败 + sub_done、空段的「无可定位」分支。
static void check_segkeys(void) {
    memset(&rp_S, 0, sizeof rp_S);
    struct rp_prog gp = {0}, cp = {0};
    gp.sh[0] = (nvn_gpu_shader)(uintptr_t)1; gp.sh[1] = (nvn_gpu_shader)(uintptr_t)2;
    cp.is_compute = 1; cp.sh[0] = (nvn_gpu_shader)(uintptr_t)3;
    rp_S.skey_ok = 3 | 32;
    for (int b = 0; b < 8; b++) { rp_S.skey[0][b] = (uint8_t)(0xa0 + b); rp_S.skey[1][b] = (uint8_t)(0xb0 + b); rp_S.skey[5][b] = (uint8_t)(0xc0 + b); }
    nvn_gpu_done fd = {0}; fd.status = NVN_GPU_DONE_FAILED; fd.why = "设备丢失";

    // ① mid 段 done 失败：快照里两笔（图形 0/1 阶段键 + 计算 5 键）
    g_ncommits = 0; g_red = 0; g_commit_fail = 0; g_seg_pk.n = 0;
    g_cmd = (nvn_gpu_cmd)(uintptr_t)0x100; g_pre = NULL;
    g_sub = calloc(1, sizeof *g_sub); g_sub->refs = 1;
    rp_seg_prog_note(&gp); rp_seg_prog_note(&cp);
    CHECK(g_seg_pk.n == 2);
    seg_cut(0);
    CHECK(g_seg_pk.n == 0);
    CHECK(g_ncommits == 1);
    g_commits[0].fn(g_commits[0].user, &fd);
    CHECK(g_red == 1);
    CHECK(strstr(g_last_red, "中途交的段失败：设备丢失") != NULL);
    CHECK(strstr(g_last_red, "本段末 2 笔") != NULL);
    CHECK(strstr(g_last_red, "0=a0a1a2a3a4a5a6a7") != NULL);
    CHECK(strstr(g_last_red, "1=b0b1b2b3b4b5b6b7") != NULL);
    CHECK(strstr(g_last_red, "5=c0c1c2c3c4c5c6c7") != NULL);
    sub_release(g_sub);   // mid_done 已放掉 counted 引用；这里放掉录制者引用

    // ② seg_cut 里 g_pre 提交失败：活环（本段已录的键）随行打出
    g_ncommits = 0; g_red = 0; g_commit_fail = 1; g_seg_pk.n = 0;
    g_cmd = (nvn_gpu_cmd)(uintptr_t)0x200; g_pre = (nvn_gpu_cmd)(uintptr_t)0x210;
    g_sub = calloc(1, sizeof *g_sub); g_sub->refs = 1;
    rp_seg_prog_note(&gp);
    seg_cut(0);
    CHECK(g_red == 1);
    CHECK(strstr(g_last_red, "前置段提交失败：注入提交失败") != NULL);
    CHECK(strstr(g_last_red, "0=a0a1a2a3a4a5a6a7") != NULL);
    CHECK(g_ncommits == 1);   // pre 失败后 mid 照常提交
    g_commits[0].fn(g_commits[0].user, &(nvn_gpu_done){0});
    sub_release(g_sub);

    // ③ 末段提交失败 + sub_done FAILED：两条行都带 s->pk 快照
    g_ncommits = 0; g_red = 0; g_commit_fail = 1; g_seg_pk.n = 0;
    g_cmd = (nvn_gpu_cmd)(uintptr_t)0x300; g_pre = NULL;
    g_sub = calloc(1, sizeof *g_sub); g_sub->refs = 1;
    struct sub *sv = g_sub;   // sub_commit_body 会把全局 g_sub 清掉：留住对象指针给手动 done 用
    rp_seg_prog_note(&cp);
    nvn_be_sub_commit();
    CHECK(g_red == 1);
    CHECK(strstr(g_last_red, "提交失败：注入提交失败") != NULL);
    CHECK(strstr(g_last_red, "5=c0c1c2c3c4c5c6c7") != NULL);
    CHECK(strstr(g_last_red, "本段末 1 笔") != NULL);
    g_red = 0;
    sub_done(sv, &fd);   // 契约：失败段 done 也恰好调一次（本桩不自动调）
    CHECK(strstr(g_last_red, "提交段失败，本段的事件 / 计数器不回写：设备丢失") != NULL);
    CHECK(strstr(g_last_red, "5=c0c1c2c3c4c5c6c7") != NULL);

    // ④ 空段：无可定位键的显式分支（不是缺尾巴）
    g_ncommits = 0; g_red = 0; g_seg_pk.n = 0;
    g_cmd = (nvn_gpu_cmd)(uintptr_t)0x400; g_pre = NULL;
    g_sub = calloc(1, sizeof *g_sub); g_sub->refs = 1;
    seg_cut(0);
    g_commits[0].fn(g_commits[0].user, &fd);
    CHECK(strstr(g_last_red, "本段无可定位的程序键") != NULL);
    sub_release(g_sub);

    // ⑤ 滚动窗口：5 笔后只剩末 4 笔（最早一笔被挤出）
    g_seg_pk.n = 0;
    g_cmd = (nvn_gpu_cmd)(uintptr_t)0x500; g_pre = NULL;
    g_sub = calloc(1, sizeof *g_sub); g_sub->refs = 1;
    for (int i = 0; i < 5; i++) rp_seg_prog_note(&cp);
    CHECK(g_seg_pk.n == 4);
    CHECK(g_seg_pk.bm[0] == 32);
    sub_release(g_sub); g_sub = NULL; g_cmd = NULL;
}


int main(int argc, char **argv) {
    int rc=regression_main(argc,argv); if(rc) return rc;
    check_count1266();
    check_segkeys();
    printf("count-group + seg-keys checks: %d checks, %d failures\n", g_checks,g_fail);
    return g_fail ? 1 : 0;
}
"""


def main():
    if not CC:
        print('【红】找不到 C 编译器（cc / clang）；不跳过', file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix='replay-exec-time-') as td:
        binary = Path(td) / 'replay_exec_time_test'
        wrapper = Path(td) / 'count_group_wrapper.c'
        wrapper.write_text(WRAPPER.replace('SOURCE_C', str(HERE / 'replay_exec_time_test.c')))
        cmd = [CC, '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror']
        for directory in INCLUDES:
            cmd += ['-I', str(directory)]
        # 同 native-test-nvn_replay：Darwin 的 ld64 不接受未定义弱引用，按源码登记（弱链接[replay]）逐个放行，强符号漏了照样红
        weak = subprocess.check_output([sys.executable, str(REPO / 'tools/weak_link_decl.py'), 'flags', 'replay'],
                                       text=True, cwd=REPO).split() if sys.platform == 'darwin' else []
        cmd += [str(wrapper), *weak, '-lm', '-o', str(binary)]
        subprocess.run(cmd, check=True, cwd=REPO)
        for args in ([], ['tm']):
            r = subprocess.run([str(binary), *args], text=True, capture_output=True, timeout=60)
            sys.stdout.write(r.stdout)
            if r.returncode:
                print(f'【红】replay_exec_time_test {" ".join(args) or "(缺省)"} 返回 {r.returncode}', file=sys.stderr)
                return r.returncode
    print('✓ replay_exec_time：三层 OP_CALL 嵌套帧级账只记一次 / 不随层数放大 / 坏记录不漏计 / OP_CALL 只记本层开销；'
          '长帧归因分项各记一次、分项和 ≤ 回放、三层嵌套不重复、取走即清零、长帧行格式')
    return 0


if __name__ == '__main__':
    sys.exit(main())
