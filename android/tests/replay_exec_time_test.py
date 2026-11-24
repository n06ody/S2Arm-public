#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""回放计时宿主单测的驱动（C 侧见 replay_exec_time_test.c）。

C 文件把 graphics/nvn/replay/rp_exec.c 整个编进自己的 TU（被测的就是生产源；测试在 include 前定义
S2A_RP_EXEC_TEST_CLOCK 把 now_ns 换成虚拟钟），所以编译命令里不能再列 rp_exec.c，否则符号重复。

跑三遍，任一遍非 0 即失败：
  · 缺省：nvk_exec_tm 关（生产缺省），验帧级「回放耗时」、绘制笔数与长帧归因分项（分项各记一次、
    分项和 ≤ 帧回放、三层 OP_CALL 嵌套不重复、取走即清零、长帧行格式）；
  · tm：再打开逐命令计时，另验 g_op_ns[OP_CALL] 只记递归自身开销、不含子列表；
  · hitch-off：确认长帧诊断关闭时只有原有帧级总账。
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


int main(int argc, char **argv) {
    int rc=regression_main(argc,argv); if(rc) return rc;
    check_count1266();
    printf("count-group exit checks: %d checks, %d failures\n", g_checks,g_fail);
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
        for args in ([], ['tm'], ['hitch-off']):
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
