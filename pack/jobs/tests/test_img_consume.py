# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""#746 写单一 IMG 边写边腾解包（ipa.to_img → pack/img.py convert --consume）：
  ① 同一款不接着出别的平台：带 --consume；写失败 → 出包工作目录除 build/ios/logs 外清空（解包已删了一部分，重试从解包重做）
  ② 接着还要出别的平台（followUp）：不带 --consume；写失败也不清（解包留给下一个平台）
  ③ 取消 / 进程被杀（玩家报「romfs.s2aidx 里的 X 不在 game.img 的 RomFS 段」）：取消同样清空；被杀来不及清的，靠出包工作目录里的
     .img_consume_inflight 标记，下次出包开头（drop_half_consumed）清空；写成功标记删掉"""
import os
import unittest
from pathlib import Path
from unittest.mock import patch
from jobcase import JobCase
from core import Cancelled, Context, Failure
import ipa, layout

GID = "0100F43008C44000"


class ImgConsume(JobCase):
    def run_to_img(self, **extra):
        self.sequence += 1
        c = Context(self.request | dict(taskId=f"img-{self.sequence}", operation="ipa", gameId=GID) | extra)
        out = layout.ios_work(c, GID); build = layout.build_dir(c, GID)
        for rel in ("roots/kuan@v1/romfs/a.bin", "pack/index.tsv", "logs/ipa.log"):
            (out / rel).parent.mkdir(parents=True, exist_ok=True); (out / rel).write_bytes(b"x")
        (build / "extract/romfs").mkdir(parents=True, exist_ok=True); (build / "extract/romfs/a.bin").write_bytes(b"x")   # 共用解包在 build/extract（build.shared_dir），不在 build/ios
        seen = []
        def fail(args, *a, **kw):
            seen.append([str(x) for x in args]); raise Failure("写到一半盘满了", code="tool_exit")
        with patch.object(c, "run", side_effect=fail), self.assertRaises(Failure) as e:
            ipa.to_img(c, GID, out / "roots/kuan@v1", out / "pack")
        self.assertEqual(e.exception.code, "package_failed")
        return seen[0], sorted(p.name for p in out.iterdir()) + sorted("build/" + p.name for p in build.iterdir() if p != out)

    def test_consume_unless_follow_up_and_failure_clears_work(self):
        args, left = self.run_to_img()
        self.assertIn("--consume", args)
        self.assertEqual(args[args.index("--work") + 1], str(layout.build_dir(Context(self.request | dict(operation="ipa", gameId=GID)), GID)))   # 这款出包缓存 build/（含共用解包，#2164）
        self.assertEqual(left, ["logs"])
        args, left = self.run_to_img(followUp=True)
        self.assertNotIn("--consume", args)
        self.assertEqual(left, ["logs", "pack", "roots", "build/extract"])

    def test_consume_frees_extract_hardlink(self):
        # #2164（Windows 0.6.9，时之笛）：共用解包 build/extract/romfs.std 与产物根 romfs.std 是 NTFS 硬链接；原先 --work 给 build/ios，
        #   free_clones 不删工作目录外的 build/extract 那份，镜像硬链接数 2，img.py copy_std 退回从头整份拷，峰值多 25 GB、写满盘。
        #   期望（手写）：按 to_img 实际传给 img.py 的 --work 跑 free_clones，产物根这份的硬链接数降到 1、解包那份删掉（copy_std 走边写边腾）
        import importlib.util
        spec = importlib.util.spec_from_file_location("s2a_test_img", Path(__file__).resolve().parents[3] / "pack/img.py")
        img = importlib.util.module_from_spec(spec); spec.loader.exec_module(img)
        c, out = self._work()
        build = layout.build_dir(c, GID); root = out / "roots/kuan@v1"
        (build / "extract").mkdir(exist_ok=True); (build / "extract/romfs.std").write_bytes(b"R" * 4096)
        (root / "romfs.std").unlink(missing_ok=True); os.link(build / "extract/romfs.std", root / "romfs.std")
        (root / "seed.txt").write_text(f"src={build / 'extract'}\n", encoding="utf-8")
        def run(args, *a, **kw):
            args = [str(x) for x in args]
            img.free_clones(str(root), str(root / "romfs.std"), str(out / "pack"), args[args.index("--work") + 1])
        with patch.object(c, "run", side_effect=run):
            ipa.to_img(c, GID, root, out / "pack")
        self.assertEqual(os.stat(root / "romfs.std").st_nlink, 1)
        self.assertFalse((build / "extract/romfs.std").exists())


    def _work(self, **extra):
        self.sequence += 1
        c = Context(self.request | dict(taskId=f"img-{self.sequence}", operation="ipa", gameId=GID) | extra)
        out = layout.ios_work(c, GID)
        for rel in ("roots/kuan@v1/romfs/a.bin", "pack/index.tsv", "logs/ipa.log"):
            (out / rel).parent.mkdir(parents=True, exist_ok=True); (out / rel).write_bytes(b"x")
        return c, out

    def test_cancel_also_clears_work(self):
        c, out = self._work()
        with patch.object(c, "run", side_effect=Cancelled()), self.assertRaises(Cancelled):
            ipa.to_img(c, GID, out / "roots/kuan@v1", out / "pack")
        self.assertEqual(sorted(q.name for q in out.iterdir()), ["logs"])

    def test_killed_mid_consume_cleared_next_time_and_success_drops_mark(self):
        c, out = self._work()
        ipa.drop_half_consumed(c, GID)   # 没标记：什么都不删
        self.assertEqual(sorted(q.name for q in out.iterdir()), ["logs", "pack", "roots"])
        (out / ipa.CONSUME_MARK).write_text("上一个任务")   # 进程被杀：标记留着
        ipa.drop_half_consumed(c, GID)
        self.assertEqual(sorted(q.name for q in out.iterdir()), ["logs"])
        c, out = self._work()
        with patch.object(c, "run", return_value=None):   # 写成功：标记删掉、别的不动
            ipa.to_img(c, GID, out / "roots/kuan@v1", out / "pack")
        self.assertFalse((out / ipa.CONSUME_MARK).exists()); self.assertIn("roots", [q.name for q in out.iterdir()])

    def test_failure_code_follows_log_cause(self):
        # D-6DEQ：Windows 写 game.img 写到 20 GB 报「[Errno 28] No space left on device」，原先一律 package_failed，玩家看到英文原句。
        #   期望：img.log 里有确凿盘满字样 → disk_full、上屏是盘满话术、原文进 internal；别的报错 → 仍 package_failed、原文照旧在上屏句里
        for line, code, shown in (("【红】[Errno 28] No space left on device", "disk_full", "磁盘空间不够"),
                                  ("【红】[Errno 13] Permission denied", "permission_denied", "出包未完成"),   # D-X5JB：原因句同其他判出码的；下一步按码给（Windows：关掉占着的程序、等杀毒扫完）
                                  ("【红】写标准 RomFS 段失败：坏了", "package_failed", "写单一 IMG（game.img）失败：【红】写标准 RomFS 段失败：坏了")):
            with self.subTest(code=code):
                c, out = self._work()
                def fail(args, log, *a, **kw):
                    Path(log).parent.mkdir(parents=True, exist_ok=True); Path(log).write_text(line + "\n", encoding="utf-8")
                    raise Failure("退出码 1", code="tool_exit")
                with patch.object(c, "run", side_effect=fail), self.assertRaises(Failure) as e:
                    ipa.to_img(c, GID, out / "roots/kuan@v1", out / "pack")
                self.assertEqual(e.exception.code, code)
                self.assertIn(shown, str(e.exception))
                self.assertIn(line, str(e.exception) + getattr(e.exception, "internal", ""))

    def test_other_jobs_img_output_not_mixed_in(self):
        # #1880：一款游戏的失败摘要里出现另一款的 IMG 路径。原先 logs/img.log 不分作业、追加写：前一款的输出（它的路径、它的盘满）
        #   串进这一款的失败原因与归因。期望：这次的报错只按这次作业自己的日志归类，别的作业 / 游戏的输出一行都不进
        c, out = self._work()
        other = "【红】[Errno 28] No space left on device: '/lib/games/另一款/game.img'"
        (Path(c.logs) / "img.log").parent.mkdir(parents=True, exist_ok=True)
        (Path(c.logs) / "img.log").write_text(other + "\n", encoding="utf-8")   # 老版本留下的共用日志
        (Path(c.logs) / "earlier-task-img.log").write_text(other + "\n", encoding="utf-8")   # 前一个作业自己的那份
        mine = "【红】写标准 RomFS 段失败：坏了"
        def fail(args, log, *a, **kw):
            with open(log, "a", encoding="utf-8") as f:
                f.write(mine + "\n")   # 同 c.run：追加写
            raise Failure("退出码 1", code="tool_exit")
        with patch.object(c, "run", side_effect=fail), self.assertRaises(Failure) as e:
            ipa.to_img(c, GID, out / "roots/kuan@v1", out / "pack")
        text = str(e.exception) + getattr(e.exception, "internal", "")
        self.assertEqual(e.exception.code, "package_failed")
        self.assertIn(mine, text)
        self.assertNotIn("另一款", text)


if __name__ == "__main__":
    unittest.main()
