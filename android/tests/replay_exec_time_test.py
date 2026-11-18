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


def main():
    if not CC:
        print('【红】找不到 C 编译器（cc / clang）；不跳过', file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix='replay-exec-time-') as td:
        binary = Path(td) / 'replay_exec_time_test'
        cmd = [CC, '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror']
        for directory in INCLUDES:
            cmd += ['-I', str(directory)]
        # 同 native-test-nvn_replay：Darwin 的 ld64 不接受未定义弱引用，按源码登记（弱链接[replay]）逐个放行，强符号漏了照样红
        weak = subprocess.check_output([sys.executable, str(REPO / 'tools/weak_link_decl.py'), 'flags', 'replay'],
                                       text=True, cwd=REPO).split() if sys.platform == 'darwin' else []
        cmd += [str(HERE / 'replay_exec_time_test.c'), *weak, '-lm', '-o', str(binary)]
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
