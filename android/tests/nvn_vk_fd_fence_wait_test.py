#!/usr/bin/env python3
"""Compile and exercise fd_fence_wait ledger behavior on a host fixture."""
import shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
def includes():
    return [HERE/'vkhost', HERE/'hostvk', HERE.parent/'native', HERE.parent/'native/direct',
            HERE.parent/'native/nvn', ROOT/'runtime/include', ROOT/'graphics/nvn/gfx/nvn',
            ROOT/'graphics/nvn/gfx/nvn/include', ROOT/'graphics/nvn/gfx/tex',
            ROOT/'graphics/nvn/third_party/vulkan-headers/include']
def build(cc, td, name, extra):
    binary=Path(td)/name
    cmd=[cc,'-std=gnu11','-ffunction-sections','-fdata-sections'] + [f'-I{x}' for x in includes()] + \
        [str(HERE/'nvn_vk_fd_fence_wait_test.c'),('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'),'-o',str(binary)] + extra
    subprocess.run(cmd,check=True)
    return binary
class FdFenceWaitTest(unittest.TestCase):
    def test_ledger(self):
        cc=shutil.which('cc')
        self.assertIsNotNone(cc, 'C compiler required for fd_fence_wait regression')
        with tempfile.TemporaryDirectory() as td:
            binary=build(cc,td,'fdwait-test',[])
            for kase in (0,1,2,4):
                with self.subTest(case=kase):
                    result=subprocess.run([str(binary),str(kase)],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_no_hooks(self):
        # 负例：弱钩子缺席时等待照常（生产上 frame_log 模块可以不在链里）。
        # 只跑 Linux：ELF 的 extern weak 声明允许符号缺席解析为 0；Mach-O 需要
        # weak_import 才是可空引用，macOS 上无法复现「模块缺席」，那里由 case1 覆盖门关语义。
        if sys.platform == 'darwin':
            self.skipTest('weak-import NULL resolution is ELF-only')
        cc=shutil.which('cc')
        self.assertIsNotNone(cc, 'C compiler required for fd_fence_wait regression')
        with tempfile.TemporaryDirectory() as td:
            binary=build(cc,td,'fdwait-nohook',['-DNO_HOOK_STUBS'])
            result=subprocess.run([str(binary),'3'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_negative_mutations(self):
        # 有效负控：对源码做语义变异（非编译失败），断言必须红在对应语义点。
        #   drop_ledger：门开也不记账（case0 应红在 log_n==1）
        #   success_only：只记成功等待（case2 超时断言应红在 log_n==1）
        src=(HERE/'nvn_vk_fd_fence_wait_test.c').read_text()
        prod=(HERE.parent/'native/nvn/nvn_vk_first_draw.c').read_text()
        mutations={
            'drop_ledger': ('if (fl) nvn_vk_frame_log_fence_site(wt_ns, 10000u + line);',
                            'if (0) nvn_vk_frame_log_fence_site(wt_ns, 10000u + line);'),
            'success_only': ('if (fl || ft) {',
                             'if ((fl || ft) && r == VK_SUCCESS) {'),
        }
        cc=shutil.which('cc')
        self.assertIsNotNone(cc, 'C compiler required for fd_fence_wait regression')
        for name,(old,new) in mutations.items():
            with self.subTest(mutation=name):
                self.assertIn(old, prod, 'mutation anchor missing in production source')
                with tempfile.TemporaryDirectory() as td:
                    work=Path(td)/'nvn_vk_first_draw.c'
                    work.write_text(prod.replace(old,new,1))
                    test_src=(HERE/'nvn_vk_fd_fence_wait_test.c').read_text().replace(
                        '../native/nvn/nvn_vk_first_draw.c', str(work))
                    tsrc=Path(td)/'nvn_vk_fd_fence_wait_test.c'
                    tsrc.write_text(test_src)
                    binary=Path(td)/'mut'
                    cmd=[cc,'-std=gnu11','-ffunction-sections','-fdata-sections'] + [f'-I{x}' for x in includes()] + \
                        [str(tsrc),('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'),'-o',str(binary)]
                    subprocess.run(cmd,check=True)
                    kase='0' if name=='drop_ledger' else '2'
                    result=subprocess.run([str(binary),kase],capture_output=True,text=True)
                    self.assertNotEqual(result.returncode,0,f'mutation {name} unexpectedly passed')
if __name__=='__main__': unittest.main()
