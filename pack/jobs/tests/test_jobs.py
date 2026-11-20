import contextlib
import collections
import io
import json
import os, stat
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock
import uuid
import zipfile
from unittest.mock import patch

JOBS = Path(__file__).resolve().parents[1]
ROOT = JOBS.parents[1]
sys.path.insert(0, str(JOBS))
os.environ.setdefault("S2A_PATCH_BASE", "file:" + os.path.join(tempfile.gettempdir(), "s2a-jobtests-no-patch-site"))   # 出包用例不连补丁下载站，同 jobcase.py
from core import Context, Failure, Cancelled, atomic, contained, digest
import build
import layout
import library


def oneclick_done(args, ipa, gid="0100000000000000", pack_id="pack-" + "0" * 8, **fields):
    """假一键出包的交付：照 pack/oneclick/oneclick.sh 的输出布局在 --out 下留现行产物根 roots/t<程序号>@现行（含 main.npdm）与 <款>资源包（清单带 index_sha256），
    再照 --events 写最后一条 result 事件（ipa.py 从这里取交付物，收尾 deliver 搬进这款的 image/ios）"""
    ipa = Path(ipa)
    root = ipa.parent / "roots" / ("t" + gid.lower() + "@现行"); root.mkdir(parents=True, exist_ok=True); (root / "main.npdm").write_bytes(b"META")
    pack = ipa.parent / (ipa.stem + "资源包"); pack.mkdir(exist_ok=True); (pack / "manifest.json").write_text(json.dumps(dict(version=2, index_sha256=pack_id)))
    (pack / "exefs").mkdir(exist_ok=True); (pack / "exefs" / "main.npdm").write_bytes(pack_id.encode())   # 收尾转单一 IMG（pack/img.py convert）读清单；内容随资源包号变
    (pack / "index.tsv").write_text(f"exefs/main.npdm\t{len(pack_id)}\t{pack_id}\n")
    ev = Path(str(args[args.index("--events") + 1]))
    with ev.open("a", encoding="utf-8") as f:
        f.write(json.dumps(dict(ev="result", t=0, ok=True, ipa=str(ipa), **fields), ensure_ascii=False) + "\n")


def shared_extract(f):
    """打 iPhone 包的单测：共用解包目录（build.ensure_extract，真解由 Mac 打包单测覆盖）桩成夹具，兄弟根起步（build.seed_from_sibling）关掉"""
    import functools
    @functools.wraps(f)
    def wrapped(self, *args, **kwargs):
        import unittest.mock as um, build
        x = self.root / "shared-extract"; (x / "exefs").mkdir(parents=True, exist_ok=True)
        (x / "exefs/main").write_bytes(b"m"); (x / "unpack.tsv").write_text("title_id\t0100000000000000\n")
        with um.patch.object(build, "ensure_extract", return_value=x) as ex, um.patch.object(build, "seed_from_sibling", return_value=False):
            self.shared_extract = (x, ex)
            return f(self, *args, **kwargs)
    return wrapped



def img_step(args, cwd=None, env=None):
    """假 c.run 遇到出包收尾转单一 IMG（ipa.to_img 经 c.run 调 pack/img.py convert，好让出包页有字节进度）就真跑它；→ 是不是这一步"""
    if "pack/img.py" not in [str(a) for a in args]: return False
    r = subprocess.run([str(a) for a in args], cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode: raise AssertionError(r.stdout + r.stderr)
    return True

class JobsTests(unittest.TestCase):
    def setUp(self):
        import ipa
        p = patch.object(ipa, "current_shell_id", return_value="0" * 16)   # 单测环境 PATH 没有 git，壳编号另有 test_install 覆盖
        p.start(); self.addCleanup(p.stop)
        self.tmp = tempfile.TemporaryDirectory(prefix="s2arm job test ")
        self.root = Path(self.tmp.name)
        # 诊断导出收本机崩溃报告（library.MAC_CRASH_DIR）：指到空的临时目录，同 jobcase.JobCase。不隔离就读跑测试这台 Mac 真实的
        #   ~/Library/Logs/DiagnosticReports——那里有近 7 天的 s2arm-host 崩溃报告时，整包预算先被它们占满，诊断包用例按机器忽红忽绿（11-16）
        crash_dir = patch.object(library, "MAC_CRASH_DIR", self.root / "no-crash-reports"); crash_dir.start(); self.addCleanup(crash_dir.stop)
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()
        self.request = dict(schema=1, taskId=str(uuid.uuid4()), operation="setup",
                            engineDir=str(ROOT), privateRoot=str(self.root / "private"),
                            library=str(self.root / "library"), libraryId="")   # 不传 developerDir：走缺省（装着的最新 Xcode），不写死 Xcode.app
        self.c = Context(self.request)
        result = library.setup(self.c)
        self.request.update(libraryId=result["libraryId"])
        self.c.r.update(self.request)

    def tearDown(self):
        self.redirect.__exit__(None, None, None)
        self.tmp.cleanup()

    def test_other_library_in_place_asks_instead_of_rejecting(self):
        # 11-17 维护者：游戏库标识和记下的库号不同，不直接拒、也不悄悄改用：说清是哪个（文件夹名 + 路径）、有几款，前端问玩家。
        #   期望手写：库里 2 款、其中 1 款隐藏 → 「有 1 款游戏」，码 library_id_mismatch，facts 带它的库号；标识文件没了 → 照旧「未连接或标识不符」
        lib = Path(self.request["library"])
        marker = json.loads((lib / ".s2arm-library.json").read_text(encoding="utf-8"))
        (lib / "catalog.json").write_text(json.dumps({"games": [{"id": "a"}, {"id": "b", "hidden": True}]}), encoding="utf-8")
        c = Context(self.request | dict(taskId="other-lib", libraryId="remembered-elsewhere"))
        with self.assertRaises(Failure) as e:
            c.require_library()
        self.assertEqual(e.exception.code, "library_id_mismatch")
        self.assertEqual(e.exception.facts, {"libraryId": marker["id"]})
        self.assertIn(f"「{lib.resolve().name}」（{lib.resolve()}）里是另一个游戏库，有 1 款游戏", str(e.exception))
        (lib / ".s2arm-library.json").unlink()
        with self.assertRaises(Failure) as e:
            c.require_library()
        self.assertEqual((e.exception.code, str(e.exception)), ("job_failed", "游戏库未连接或标识不符，请重新选择游戏库。"))

    def test_game_required_when_request_has_no_game(self):
        """客户端不再按名单拦「要不要游戏号」（自己人之间不设允许清单，#1149）：要游戏的作业请求里没带游戏号，
        作业层说清缺的是游戏号，不是「游戏未登记」；带了但库里没有照旧是未登记"""
        c = Context(dict(self.request, operation="ipa"))
        with self.assertRaises(Failure) as e:
            c.game()
        self.assertIn("没有游戏号", str(e.exception))
        c = Context(dict(self.request, operation="ipa", gameId="0100000000ABCDEF"))
        with self.assertRaises(Failure) as e:
            c.game()
        self.assertIn("未登记", str(e.exception))

    def test_context_accepts_readable_windows_volume_when_final_path_query_is_unsupported(self):
        import core
        # 用真实路径起步：Mac 临时目录在 /var（指向 /private/var 的符号链接）下，退回分支逐级查到它会按「路上有链接」照样报错
        mounted = Path(os.path.realpath(self.root)) / "mounted-library"
        child = mounted / "games" / "one"
        child.mkdir(parents=True)
        (mounted / ".s2arm-library.json").write_text(json.dumps({"schema": 1, "id": "test-library"}), encoding="utf-8")
        (child / "payload.bin").write_bytes(b"DATA")
        request = dict(self.request, taskId=str(uuid.uuid4()), privateRoot=str(self.root / "private-mounted"), library=str(mounted), libraryId="test-library")
        resolve = Path.resolve

        def resolve_with_volume_error(path, *args, **kwargs):
            if path == mounted or mounted in path.parents:
                error = OSError(22, "filesystem final-path query unsupported")
                error.winerror = 1005
                raise error
            return resolve(path, *args, **kwargs)

        with patch.object(core, "_windows", lambda: True), patch.object(Path, "resolve", resolve_with_volume_error):
            context = Context(request)
            context.require_library()
            contained = core.contained(mounted, child)
            self.assertEqual(core.default_home(context.private, context.library), Path(os.path.abspath(mounted)))
            self.assertEqual(layout.require_in_location(context, child), Path(os.path.abspath(child)))
            build.validate_files(context, mounted, {"games/one/payload.bin": [4, ""]})
        self.assertEqual(context.library, Path(os.path.abspath(mounted)))
        self.assertEqual(contained, Path(os.path.abspath(child)))
        self.assertTrue((context.logs / "jobs" / (context.task + ".json")).is_file())

    def test_context_does_not_hide_other_library_path_resolution_errors(self):
        import core
        mounted = self.root / "denied-library"
        mounted.mkdir()
        request = dict(self.request, taskId=str(uuid.uuid4()), privateRoot=str(self.root / "private-denied"), library=str(mounted))
        resolve = Path.resolve

        def resolve_with_access_error(path, *args, **kwargs):
            if path == mounted:
                error = OSError(5, "access denied")
                error.winerror = 5
                raise error
            return resolve(path, *args, **kwargs)

        with patch.object(core, "_windows", lambda: True), patch.object(Path, "resolve", resolve_with_access_error):
            with self.assertRaises(OSError) as caught:
                Context(request)
        self.assertEqual(caught.exception.winerror, 5)

    def test_resolve_path_does_not_fall_back_across_reparse_points(self):
        import core
        mounted = Path(os.path.realpath(self.root)) / "reparse-library"   # 同上：只让被测的这一级是重解析点
        mounted.mkdir()
        resolve = Path.resolve
        lstat = Path.lstat

        def resolve_with_volume_error(path, *args, **kwargs):
            if path == mounted:
                error = OSError(22, "filesystem final-path query unsupported")
                error.winerror = 1005
                raise error
            return resolve(path, *args, **kwargs)

        def lstat_with_reparse(path, *args, **kwargs):
            if path == mounted:
                return type("Stat", (), {"st_mode": stat.S_IFDIR, "st_file_attributes": 0x400})()
            return lstat(path, *args, **kwargs)

        with patch.object(core, "_windows", lambda: True), patch.object(Path, "resolve", resolve_with_volume_error), patch.object(Path, "lstat", lstat_with_reparse):
            with self.assertRaises(OSError) as caught:
                core.resolve_path(mounted)
        self.assertEqual(caught.exception.winerror, 1005)

    def test_resolve_path_does_not_fall_back_off_windows(self):
        import core
        error = OSError(22, "filesystem final-path query unsupported")
        error.winerror = 1005
        with patch.object(core, "_windows", lambda: False), patch.object(Path, "resolve", side_effect=error):
            with self.assertRaises(OSError) as caught:
                core.resolve_path(self.root / "library")
        self.assertIs(caught.exception, error)

    @unittest.skipUnless(os.name == "nt", "盘符 / UNC 只有 Windows 有（Windows 实机上跑）")
    def test_library_keeps_mapped_drive_letter(self):
        # 映射网络驱动器（Z: → \\NAS\share）：Path.resolve 换成内网地址。库根保留玩家选的盘符写法（存、显示、拼路径），
        #   判越界照样按解析结果比；路上有 junction 时不保留（没法确认它指哪）
        import core
        real = Path(os.path.realpath(self.root)) / "nas-library"
        (real / "games").mkdir(parents=True)
        unc = Path(r"\\nas\share\nas-library")
        resolve = Path.resolve

        def as_unc(path, *args, **kwargs):
            path = Path(os.path.abspath(path))
            if path == real or real in path.parents:
                return unc / path.relative_to(real)
            return resolve(path, *args, **kwargs)

        with patch.object(Path, "resolve", as_unc):
            self.assertEqual(core.resolve_library_path(real), real)
            self.assertEqual(core.resolve_path(real), unc)
            self.assertEqual(core.contained(real, real / "games"), unc / "games")
            context = Context(dict(self.request, taskId=str(uuid.uuid4()), privateRoot=str(self.root / "private-nas"), library=str(real)))
            self.assertEqual(context.library, real)
            target = real / "elsewhere"
            target.mkdir()
            subprocess.run(["cmd", "/c", "mklink", "/J", str(real / "junction"), str(target)], check=True, capture_output=True)
            self.assertEqual(core.resolve_library_path(real / "junction"), unc / "junction")

    def catalogue(self, games):
        atomic(self.c.library / "catalog.json", dict(schema=1, games=games))

    def test_player_jobs_keep_shader_compilation_local(self):
        # App jobs must not inherit developer remote dispatch, or fall back to
        # dl_gen.py's automatic remote default when the variable is absent.
        with patch.dict(os.environ, {"S2A_DL_REMOTE": "auto", "S2A_PSO_REMOTE": "auto"}):
            env = self.c.env()
        self.assertEqual(env.get("S2A_DL_REMOTE"), "off")
        self.assertEqual(env.get("S2A_PSO_REMOTE"), "off")

    def test_bundled_patch_library_is_for_packing_not_for_play(self):
        # 补丁包部署方案 v2（11-04）：<资源>/Patches/INDEX.tsv 在 → 出包环境 S2A_PATCH_DIR 指它；没有就不设；起局环境去掉（宿主会往补丁根写 .applied，App 包内不能写）
        res = self.root / "res"; (res / "Patches").mkdir(parents=True)
        self.c.r["resources"] = str(res)
        self.assertNotIn("S2A_PATCH_DIR", self.c.env())
        (res / "Patches/INDEX.tsv").write_text("程序号\t已验构建号\tsha256\n")
        self.assertEqual(self.c.env().get("S2A_PATCH_DIR"), str(res / "Patches"))
        self.assertNotIn("S2A_PATCH_DIR", build.runtime_env(self.c, self.game(), self.root / "runtime"))

    def player_keys(self):
        """玩家已导入 prod.keys（加密原件出包要它，library.require_game 开头核）"""
        self.c.keys.parent.mkdir(parents=True, exist_ok=True); self.c.keys.write_text("header_key = " + "0" * 64 + "\n")

    def game(self, tid="0100000000000000", packages=None):
        return dict(id=tid, name="Test title", display="1.0", hidden=False, packages=packages or [])

    def package(self, kind="base", version=0, tid="0100000000000000", sha="a" * 64):
        p = self.c.library / "sources" / (sha + ".nsp")
        p.parent.mkdir(exist_ok=True)
        p.write_bytes(b"unit-test content; not a Switch container")
        return dict(titleId=tid, baseId="0100000000000000", type=kind, version=version, path=str(p.relative_to(self.c.library)),
                    sha256=sha, display="1.0", guestIsa="aarch64")

    def events_of(self, kind):
        return [json.loads(l) for l in self.c.events.read_text().splitlines() if json.loads(l)["type"] == kind]

    def test_shared_extract_reused_until_selection_changes(self):
        # 11-02 多平台共用（维护者）：Mac 打包与打 iPhone 包共用 游戏库/cache/<程序号>/extract；所选包没变 → 复用（打复用行、不解）；加了 DLC → 重解
        import unittest.mock as um, build, toolchain
        base = self.package("base"); game = self.game(packages=[base])
        calls = []
        def run(args, log=None, env=None, cwd=None, on_line=None, timeout=None, ok_codes=(0,)):
            args = [str(a) for a in args]
            self.c.last_rc = 0
            if "--npdm-isa" in args:
                Path(log).write_text("aarch64\n")
                return
            calls.append(args)
            x = Path(args[2])  # 解包目标在 args[2]（读取器直接执行，不再带 dotnet 前缀）
            for f, b in (
                ("exefs/main.npdm", b"npdm"),
                ("ctl/control.nacp", b"nacp"),
                ("romfs/a.bin", b"a"),
            ):
                (x / f).parent.mkdir(parents=True, exist_ok=True)
                (x / f).write_bytes(b)

        logs = self.root / "logs"
        logs.mkdir()
        import milestones as ms

        ms.plan(self.c, 1, "build_dual")
        with (
            um.patch.object(toolchain, "reader", return_value="reader.dll"),
            um.patch.object(self.c, "run", run),
        ):
            x1 = build.ensure_extract(self.c, game, [base], {}, logs)
            x2 = build.ensure_extract(self.c, game, [base], {}, logs)
            self.assertEqual((x1, x2, len(calls)), (layout.build_dir(self.c, "0100000000000000") / "extract",) * 2 + (1,))
            self.assertTrue(any(e.get("message") == "已验证并复用解包产物" for e in self.events_of("cache")))
            dlc = dict(self.package("dlc", tid="0100000000001001", sha="d" * 64))
            build.ensure_extract(self.c, self.game(packages=[base, dlc]), [base, dlc], {}, logs)
            self.assertEqual(len(calls), 2, "所选包变了（加 DLC）要重解，不能认旧解包")

    def test_nup_extract_goes_through_ns2_nup(self):
        # ③ Switch 2 NUP（导入记 format=nup）：共用解包走 pack/ns2_nup.py --to-extract（真跑：展开 + ns2_dump 整理），不调读取器解密、不要 prod.keys；
        #   产物与 nx_unpack 解包同形（exefs/main.npdm、ctl/control.nacp、unpack.tsv 程序号、aoc/），中间导出目录不留。只桩程序位数判定（nx_unpack --npdm-isa，要 .NET）
        import unittest.mock as um, toolchain, milestones as ms
        from jobcase import synthetic_nacp, synthetic_npdm, synthetic_nup
        app = 0x0400486024DEC000
        src = self.root / "drive" / "Zelda T [0400486024dec000][v131072].nup"; src.parent.mkdir()
        synthetic_nup(src, app, [(0, app, 0, {"main": b"\x01" * 300, "main.npdm": synthetic_npdm(app)}, {"a.bin": b"base"}, {}),
                                 (1, app | 0x800, 131072, {"main": b"\x02" * 500, "main.npdm": synthetic_npdm(app)}, {"a.bin": b"upd"},
                                  {2: synthetic_nacp("Ocarina T", "1.2.0")}),
                                 (2, app + 0x1001, 0, None, {"d.bin": b"DLC"}, {})])
        common = dict(baseId="0400486024DEC000", path=str(src), sha256="e" * 64, display="1.2.0", guestIsa="unknown", format="nup")
        packages = [dict(common, titleId="0400486024DEC000", type="base", version=0),
                    dict(common, titleId="0400486024DEC800", type="update", version=131072),
                    dict(common, titleId="0400486024DED001", type="dlc", version=0)]
        game = self.game("0400486024DEC000", packages); game["name"] = "Ocarina T"
        calls, real = [], self.c.run
        def run(args, log=None, env=None, cwd=None, on_line=None, timeout=None, ok_codes=(0,)):
            calls.append([str(a) for a in args])
            if "--npdm-isa" in calls[-1]:
                self.c.last_rc = 0; Path(log).write_text("aarch64\n"); return
            return real(args, log, env, cwd, on_line, timeout, ok_codes)
        logs = self.root / "logs"; logs.mkdir()
        ms.plan(self.c, 1, "build_dual")
        self.assertFalse(self.c.keys.exists())
        # 读取器是桩（reader.dll 不在）：RomFS 镜像形态（缺省）要真 nx_unpack 建 romfs.std，这里按目录形态（S2A_ROMFS_IMAGE=0）核分流与产物形状
        with um.patch.object(toolchain, "reader", return_value=self.root / "reader.dll"), um.patch.object(self.c, "run", run):
            x = build.ensure_extract(self.c, game, build.selected_packages(game), {"S2A_ROMFS_IMAGE": "0"}, logs)
        self.assertEqual([c[4:6] for c in calls if "--npdm-isa" not in c], [["--to-extract", str(src)]])
        self.assertTrue(calls[0][3].endswith("pack/ns2_nup.py") and calls[0][6] == str(x), calls[0])
        self.assertFalse(any(str(self.c.keys) in c for c in calls), "NUP 不调读取器解密")
        self.assertEqual((x / "exefs/main.npdm").read_bytes(), synthetic_npdm(app))
        self.assertEqual((x / "exefs/main").read_bytes(), b"\x02" * 500)
        self.assertEqual((x / "ctl/control.nacp").read_bytes(), synthetic_nacp("Ocarina T", "1.2.0"))
        self.assertIn("title_id\t0400486024dec000", (x / "unpack.tsv").read_text())
        self.assertEqual((x / "aoc/1/d.bin").read_bytes(), b"DLC")
        self.assertEqual(sorted(p.name for p in x.parent.iterdir()), ["extract", "extract-checkpoint.json"])

    def test_two_nups_use_the_newest_update_container(self):
        # 同一款放过两个 NUP（旧的带 DLC、新的带更新）：展开选中最新更新所在的那个；旧 NUP 里的 DLC 不带、发黄字 aoc_missing，不静默
        import unittest.mock as um, toolchain, milestones as ms
        old, new = self.root / "old.nup", self.root / "new.nup"
        common = dict(baseId="0400486024DEC000", display="", guestIsa="unknown", format="nup")
        packages = [dict(common, titleId="0400486024DEC000", type="base", version=0, path=str(old), sha256="1" * 64),
                    dict(common, titleId="0400486024DED001", type="dlc", version=0, path=str(old), sha256="1" * 64),
                    dict(common, titleId="0400486024DEC000", type="base", version=0, path=str(new), sha256="2" * 64),
                    dict(common, titleId="0400486024DEC800", type="update", version=131072, path=str(new), sha256="2" * 64)]
        game = self.game("0400486024DEC000", packages)
        calls = []
        def run(args, log=None, env=None, cwd=None, on_line=None, timeout=None, ok_codes=(0,)):
            args = [str(a) for a in args]; self.c.last_rc = 0; calls.append(args)
            if "--npdm-isa" in args: Path(log).write_text("aarch64\n"); return
            x = Path(args[6])
            for f in ("exefs/main.npdm", "ctl/control.nacp"):
                (x / f).parent.mkdir(parents=True, exist_ok=True); (x / f).write_bytes(b"x")
        logs = self.root / "logs"; logs.mkdir()
        ms.plan(self.c, 1, "build_dual")
        with um.patch.object(toolchain, "reader", return_value=self.root / "reader.dll"), um.patch.object(self.c, "run", run):
            build.ensure_extract(self.c, game, build.selected_packages(game), {}, logs)
        self.assertEqual(calls[0][4:6], ["--to-extract", str(new)])
        d = [y for y in self.c.job["diagnostics"] if y["code"] == "aoc_missing"]
        self.assertEqual((len(d), d[0]["severity"], "0400486024DED001" in d[0]["summary"]), (1, "warning", True))

    def test_license_only_dlc_warns_not_fails(self):
        # 审计 P1-89：所选 DLC 一行文件数 -1（只有授权 / 数据解不开）→ 不整单失败、解包照常记成完整准备，发一条黄字 aoc_missing 带那份 DLC 的标题号；
        #   全部 DLC 有数据 → 没有 aoc_missing
        import unittest.mock as um, build, toolchain
        base = self.package("base"); dlc = dict(self.package("dlc", tid="0100000000001001", sha="d" * 64)); game = self.game(packages=[base, dlc])
        rows = {"v": "1\t0100000000001001\t0\t-1\t0\n"}
        def run(args, log=None, env=None, cwd=None, on_line=None, timeout=None, ok_codes=(0,)):
            args = [str(a) for a in args]
            self.c.last_rc = 0
            if "--npdm-isa" in args:
                Path(log).write_text("aarch64\n")
                return
            x = Path(args[2])  # 解包目标在 args[2]（读取器直接执行，不再带 dotnet 前缀）
            for f, b in (
                ("exefs/main.npdm", b"npdm"),
                ("ctl/control.nacp", b"nacp"),
                ("aoc.tsv", ("idx\ttid\tver\tfiles\tbytes\n" + rows["v"]).encode()),
            ):
                (x / f).parent.mkdir(parents=True, exist_ok=True)
                (x / f).write_bytes(b)

        logs = self.root / "logs"
        logs.mkdir()
        import milestones as ms

        ms.plan(self.c, 1, "build_dual")
        with (
            um.patch.object(toolchain, "reader", return_value="reader.dll"),
            um.patch.object(self.c, "run", run),
        ):
            x = build.ensure_extract(self.c, game, [base, dlc], {}, logs)
            self.assertTrue((x.parent / "extract-checkpoint.json").is_file(), "仅授权行不该挡住本体的完整准备")
            d = [y for y in self.c.job["diagnostics"] if y["code"] == "aoc_missing"]
            self.assertEqual((len(d), d[0]["severity"] if d else None, "0100000000001001" in (d[0]["summary"] if d else "")), (1, "warning", True))
            shutil.rmtree(x); rows["v"] = "1\t0100000000001001\t0\t3\t30\n"; self.c.job["diagnostics"].clear()
            build.ensure_extract(self.c, game, [base, dlc], {}, logs)
            self.assertEqual([y for y in self.c.job["diagnostics"] if y["code"] == "aoc_missing"], [])

    def test_seed_from_sibling_clones_current_root_and_names_commit(self):
        # 11-02：本平台还没现行根、别的平台有 → 克隆它的现行根起步（不从解包目录重新播种），复用行带被复用根的提交号；没有兄弟根 → False；
        #   克隆失败（盘满 / 权限等）→ 黄字 sibling_clone_failed、False（调用方从解包目录播种）；exFAT 上 clonedir 走普通拷贝应成功
        import unittest.mock as um, build
        game = self.game()
        sib = layout.ios_work(self.c, "0100000000000000") / "roots"; v = sib / "party@pkg-abc"   # iPhone 出包工作目录 build/ios 的现行根
        (v / "romfs").mkdir(parents=True); (v / ".stage_rewrite").write_text("K-rewrite\n"); (v / ".root_src").write_text("abcdef1234567890\n")
        (sib / "party@现行").symlink_to("party@pkg-abc")
        mac = layout.build_dir(self.c, "0100000000000000") / "key1/roots"
        self.assertFalse(build.seed_from_sibling(self.c, self.game(tid="0100000000000001"), mac, "0100000000000001"), "别的款的根不算兄弟")
        self.assertTrue(build.seed_from_sibling(self.c, game, mac, "0100000000000000"))
        cur = (mac / "0100000000000000@现行").resolve()
        self.assertEqual((cur.name, (cur / ".stage_rewrite").read_text()), ("0100000000000000@sib-pkg-abc", "K-rewrite\n"))
        ev = [e for e in self.events_of("cache") if "复用兄弟平台" in e.get("message", "")]
        self.assertTrue(ev and "提交 abcdef123" in ev[-1]["message"] and "克隆路线：APFS 克隆" in ev[-1]["message"], ev)
        with um.patch.object(self.c, "run", side_effect=Failure("克隆失败：x：[Errno 45] Operation not supported")):
            self.assertFalse(build.seed_from_sibling(self.c, game, layout.build_dir(self.c, "0100000000000000") / "key2/roots", "0100000000000000"))
        d = [x for x in self.c.job["diagnostics"] if x["code"] == "sibling_clone_failed"]
        self.assertTrue(d and d[0]["severity"] == "warning", d)

    def test_developer_dir_default_is_newest_installed_not_hardcoded(self):
        # 10-25 维护者：请求没带 developerDir 时不写死 /Applications/Xcode.app；11-07 Duo 定因：缺省取装着的最新 Xcode（toolchain.detect_xcode，
        #   同启动器与 tools/toolchain.sh），不再取 xcode-select（本机指 26.5 → 旧 SDK 壳在 iOS 27 新机型上缩小显示）；找不到就不设（由工具链检查说清没装）
        import core, toolchain
        newest = self.root / "apps/Xcode-99.app/Contents/Developer"; newest.mkdir(parents=True)
        with patch.object(toolchain, "detect_xcode", return_value=str(newest)):
            env = Context(dict(self.request)).env()
        self.assertEqual(env.get("DEVELOPER_DIR"), str(newest))
        self.assertEqual(env.get("S2A_DEVELOPER_DIR"), env.get("DEVELOPER_DIR"))
        chosen = str(self.root / "picked/Xcode.app/Contents/Developer")
        self.assertEqual(Context(dict(self.request, developerDir=chosen)).env()["DEVELOPER_DIR"], chosen)   # 启动器传的优先
        with patch.object(core, "default_developer_dir", return_value=None), patch.dict(os.environ, {"DEVELOPER_DIR": "/stale/Xcode.app/Contents/Developer"}):
            env = Context(dict(self.request)).env()
        self.assertNotIn("DEVELOPER_DIR", env); self.assertNotIn("S2A_DEVELOPER_DIR", env)   # 外层环境里的旧值也不漏进来

    def test_preflight_offers_one_click_fix_for_missing_xcode(self):
        # 没有 Xcode：Xcode 项不过、给「去 App Store」；Metal 项不过、不给按钮（没 Xcode 下载不了组件）；不再有「共享字体」检测项
        import toolchain
        self.c.r.update(developerDir=str(self.root / "NoXcode.app/Contents/Developer"))
        checks = {k["name"]: k for k in toolchain.preflight(self.c)["checks"]}
        self.assertFalse(checks["Xcode"]["ok"]); self.assertEqual(checks["Xcode"].get("action"), "appStoreXcode")
        self.assertFalse(checks["Metal 编译组件"]["ok"]); self.assertIsNone(checks["Metal 编译组件"].get("action"))
        self.assertNotIn("共享字体", checks)

    def test_preflight_timeout_probe_runs_a_bounded_command(self):
        # #1133：timeout 按「限时跑一次命令退出 0」判可用：认不认 --version 不算数；缺 / 跑不起来 / 命令失败照旧判缺
        import toolchain, unittest.mock as um
        def fake(rc_of):
            def probe(c, args, timeout=15):
                return rc_of(list(map(str, args))) == 0, ""
            return probe
        def bundled(a):   # 内附替身的行为：不认任何选项（返回 125），限时跑命令原样返回命令的码
            if a[0] != "timeout": return 0
            return 125 if a[1].startswith("-") else 0 if a[2:] == ["/usr/bin/true"] else 1
        for rc_of, want in ((bundled, True), (lambda a: 127 if a[0] == "timeout" else 0, False), (lambda a: 1 if a[0] == "timeout" else 0, False)):
            with um.patch.object(toolchain, "probe", fake(rc_of)):
                checks = {k["name"]: k for k in toolchain.preflight(self.c)["checks"]}
            self.assertEqual(checks["timeout"]["ok"], want)

    def test_inspect_turns_decrypt_failure_into_keys_advice(self):
        # 读取器报「could not be decrypted… Check keys」退出 → 提示换新密钥；别的失败原样往上抛
        import unittest.mock as um
        keys = self.root / "prod.keys"; keys.write_text("header_key = " + "0" * 64 + "\n")
        def fake(msg):
            def run(args, log=None, env=None, cwd=None, on_line=None):
                on_line(msg); raise Failure("S2Arm 工具退出，当前阶段未完成。")
            return run
        with um.patch.object(library.toolchain, "reader", return_value="nx_unpack.dll"):
            with um.patch.object(self.c, "run", fake("Container metadata could not be decrypted or verified. Check keys, tickets and file integrity.")):
                with self.assertRaises(Failure) as e: library.inspect(self.c, self.root / "a.xci", self.root, keys)
                self.assertIs(str(e.exception), library.KEYS_CANNOT_DECRYPT)
            # #1008：读取器给了确定原因（哈希校验不过）→ 照原话报，不落三选一猜测。期望手写：原话前加「无法读取游戏内容：」、码 content_unreadable
            hash_line = "【原因 hash_mismatch】源文件数据哈希校验不过（ResultFsNonRealDataVerificationFailed (2002-4604): Hash error!）：文件被改过（如合进改版资源的整合版）或损坏，换一份原版完整的文件"
            def fake2(*msgs):
                def run(args, log=None, env=None, cwd=None, on_line=None):
                    for m in msgs: on_line(m)
                    raise Failure("S2Arm 工具退出，当前阶段未完成。")
                return run
            with um.patch.object(self.c, "run", fake2("Container metadata could not be decrypted or verified. Check keys, tickets and file integrity.", hash_line)):
                with self.assertRaises(Failure) as e: library.inspect(self.c, self.root / "a.xci", self.root, keys)
                self.assertEqual((str(e.exception), e.exception.code), ("无法读取游戏内容：" + hash_line.split("】", 1)[1], "content_unreadable"))
            with um.patch.object(self.c, "run", fake("Unhandled exception: disk full")):
                with self.assertRaises(Failure) as e: library.inspect(self.c, self.root / "a.xci", self.root, keys)
                self.assertEqual(str(e.exception), "S2Arm 工具退出，当前阶段未完成。")

    def test_inspect_success_after_integrity_skip_warns_not_fails(self):
        # 维护者 11-17：校验不过不再整单拒收（#1467）。读取器哈希不符后跳过完整性校验照常读出、退出 0，输出里带「🟡 原件有数据和原版对不上…」→
        #   期望（手写）：照常返回包信息，并发一条 warning 级诊断 source_integrity_skipped（句子 = library.SOURCE_INTEGRITY_SKIPPED）；没有这行就不发
        import unittest.mock as um
        keys = self.root / "prod.keys"; keys.write_text("header_key = " + "0" * 64 + "\n")
        pkg = '{"schema": 1, "packages": [{"titleId": "0100000000010000", "baseId": "0100000000010000", "type": "base", "version": 0}]}'
        def fake(*msgs):
            def run(args, log=None, env=None, cwd=None, on_line=None):
                for m in msgs: on_line(m)
            return run
        for skipped in (True, False):
            lines = (["🟡 原件有数据和原版对不上（导入检查：控制数据（内容 aabbccdd），内容 aabbccdd）：…", "【校验跳过】导入检查：控制数据（内容 aabbccdd）\taabbccdd00\t文件 /control.nacp"] if skipped else []) + [pkg]
            with um.patch.object(library.toolchain, "reader", return_value="nx_unpack.dll"), um.patch.object(self.c, "run", fake(*lines)), \
                 um.patch.object(self.c, "diagnostic", create=True) as diag:
                got = library.inspect(self.c, self.root / "a.xci", self.root, keys)
            self.assertEqual(got[0]["titleId"], "0100000000010000")
            want = [dict(stage="导入检查：控制数据（内容 aabbccdd）", contents="aabbccdd00", place="文件 /control.nacp")]
            if skipped:
                diag.assert_called_once_with(library.SOURCE_INTEGRITY_SKIPPED, code="source_integrity_skipped", severity="warning",
                                             internal="导入检查：控制数据（内容 aabbccdd） aabbccdd00 文件 /control.nacp")
                self.assertEqual(got[0]["integritySkipped"], want)   # 记进包记录：游戏库标「数据校验不过」、之后失败补说明都按它
                self.assertTrue(library.integrity_skipped(dict(packages=got)))
            else:
                diag.assert_not_called(); self.assertNotIn("integritySkipped", got[0]); self.assertFalse(library.integrity_skipped(dict(packages=got)))

    def test_failure_of_integrity_skipped_game_adds_note(self):
        # 维护者 11-17：导入时数据校验不过的款，之后出包 / 起局失败要补一句。期望（手写）：游戏有包带 integritySkipped → 补一条
        #   warning 级 source_integrity_skipped（句子 = SOURCE_INTEGRITY_FAILED_NOTE）；没有、或请求没带游戏号 → 不补；读游戏库出错 → 不补、不抛
        import unittest.mock as um, main
        games = {"games": [{"id": "A", "packages": [{"integritySkipped": [{"stage": "x", "contents": "", "place": ""}]}]}, {"id": "B", "packages": [{}]}]}
        for gid, want in (("A", True), ("B", False), (None, False)):
            with um.patch.object(self.c, "r", {"gameId": gid} if gid else {}), um.patch.object(self.c, "catalog", return_value=games), \
                 um.patch.object(self.c, "diagnostic", create=True) as diag:
                main.integrity_note(self.c)
            if want: diag.assert_called_once_with(library.SOURCE_INTEGRITY_FAILED_NOTE, code="source_integrity_skipped", severity="warning")
            else: diag.assert_not_called()
        with um.patch.object(self.c, "r", {"gameId": "A"}), um.patch.object(self.c, "catalog", side_effect=OSError("坏")), \
             um.patch.object(self.c, "diagnostic", create=True) as diag:
            main.integrity_note(self.c)
        diag.assert_not_called()

    def test_failure_after_pack_verify_mismatch_adds_note(self):
        # 维护者 11-17：出包时资源包核对没对上（pack/verify.py 改黄照常出包，原因落 <包目录>.verify_warn.txt），之后这款失败补一句。
        #   期望（手写）：现行产物根 pkg/ 下有 *.verify_warn.txt → 补一条 warning 级 source_integrity_skipped（句子 = PACK_VERIFY_NOTE）；
        #   只有非现行版本里有、或没有 → 不补
        import unittest.mock as um, main, layout
        games = {"games": [{"id": "A", "packages": [{}]}]}
        for where, want in (("ios/roots/ta@现行/pkg", True), ("ios/roots/ta@v1/pkg", False), (None, False)):
            with tempfile.TemporaryDirectory() as d:
                if where:
                    (Path(d) / where).mkdir(parents=True); (Path(d) / where / "pack.files.verify_warn.txt").write_text("x\n")
                with um.patch.object(self.c, "r", {"gameId": "A"}), um.patch.object(self.c, "catalog", return_value=games), \
                     um.patch.object(layout, "build_dir", return_value=Path(d)), um.patch.object(self.c, "diagnostic", create=True) as diag:
                    main.integrity_note(self.c)
            if want: diag.assert_called_once_with(main.PACK_VERIFY_NOTE, code="source_integrity_skipped", severity="warning")
            else: diag.assert_not_called()

    def test_connected_counts_wired_paired_with_idle_tunnel(self):
        # 10-26 测试设备2：数据线连着、已配对，隧道空闲 disconnected → 算连着。11-13 起（维护者：插着手机别说识别不了）：
        #   无线且隧道没通的（N）、插着线还没配对的（U）也列出来给玩家选——在不在线、配没配对由各行连接状态显示，装的那一步会去配对
        import unittest.mock as um, diag, target
        devices = {"result": {"devices": [
            {"identifier": "W", "hardwareProperties": {"reality": "physical", "platform": "iOS", "productType": "iPhone18,2"}, "deviceProperties": {"name": "测试设备2", "osVersionNumber": "27.1"},
             "connectionProperties": {"tunnelState": "disconnected", "transportType": "wired", "pairingState": "paired"}},
            {"identifier": "N", "hardwareProperties": {"reality": "physical", "platform": "iOS"}, "connectionProperties": {"tunnelState": "unavailable", "transportType": None, "pairingState": "paired"}},
            {"identifier": "U", "hardwareProperties": {"reality": "physical", "platform": "iOS"}, "connectionProperties": {"tunnelState": "disconnected", "transportType": "wired", "pairingState": "unpaired"}}]}}
        with um.patch.object(diag, "devicectl", lambda c, *a, timeout=120: (0, devices, "")), um.patch.object(target, "gpu_arch", lambda *a: "g18"):
            self.assertEqual([d["id"] for d in target.connected(self.c)], ["W", "N", "U"])

    def test_emit_survives_frontend_gone(self):
        # 10-26：启动器（读作业标准输出的一端）没了，作业不能因写断掉的管道而死；事件照写记录，之后起的子进程标准输出指向 /dev/null
        code = (f"import os, sys; sys.path.insert(0, {str(ROOT / 'pack/jobs')!r}); from core import Context\n"
                f"r, w = os.pipe(); os.dup2(w, 1); os.close(w); os.close(r)\n"   # 读端已关 = 启动器没了
                f"c = Context({dict(self.request, taskId='gone')!r})\n"
                "c.begin('甲', 1, 2); c.emit(done=1, total=2); c.begin('乙', 2, 2)\n"
                "import subprocess; rc = subprocess.run(['/bin/sh', '-c', 'yes | head -c 200000']).returncode\n"
                "open(os.environ['OUT'], 'w').write(f'{c.frontend_gone} {c.job[\"stage\"]} {c.seq} {rc}')\n")
        out = self.root / "gone.out"
        p = subprocess.run([sys.executable, "-c", code], env=dict(os.environ, OUT=str(out)), capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(out.read_text(), "True 乙 3 0")   # 发现断管、没抛；三条事件都进了记录；子进程照常跑完
        jobs = self.root / "private/logs/jobs"   # 任务记录固定在 <私有目录>/logs/jobs（剃刀 H6）
        events = (jobs / "gone.events.jsonl").read_text().splitlines()
        self.assertEqual([json.loads(l)["stage"] for l in events], ["甲", "甲", "乙"])

    def test_pull_diagnostics_only_s2arm_apps_on_connected_ios(self):
        # Same-model devices and same-second batches retain independent snapshots.
        import unittest.mock as um, diag
        devices = {"result": {"devices": [
            {"identifier": "D1", "hardwareProperties": {"platform": "iOS", "udid": "U1", "marketingName": "iPhone 17 Pro Max"}, "connectionProperties": {"tunnelState": "connected"}},
            {"identifier": "D3", "hardwareProperties": {"platform": "iOS", "udid": "U3", "marketingName": "iPhone 17 Pro Max"}, "connectionProperties": {"tunnelState": "connected"}},
            {"identifier": "D2", "hardwareProperties": {"platform": "iOS", "udid": "U2"}, "connectionProperties": {"tunnelState": "disconnected"}},
            {"identifier": "M1", "hardwareProperties": {"platform": "macOS"}, "connectionProperties": {"tunnelState": "connected"}}]}}
        apps = {"result": {"apps": [{"bundleIdentifier": "io.s2arm.d2r.ABCDE12345", "name": "D2R S2A"}, {"bundleIdentifier": "com.other.app", "name": "Other"}]}}
        calls, batch = [], 1
        def fake(c, *args, timeout=120):
            calls.append(args)
            if args[:2] == ("list", "devices"): return 0, devices, ""
            if args[:3] == ("device", "info", "apps"): return 0, apps, ""
            source = args[args.index("--source") + 1]
            device = args[args.index("--device") + 1]
            target = Path(args[args.index("--destination") + 1])
            if target.name in ("diag", "metrickit"):
                target.mkdir(); (target / "partial.json").write_text("failed partial")
            else:
                target.write_text(f"batch {batch} {device} {source}")
            return (0 if batch < 3 and (source.endswith("host.log") or batch == 1 and source.endswith("frames.tsv")) else 1), {}, ""
        with um.patch.object(diag, "devicectl", fake), um.patch.object(diag.time, "strftime", return_value="same-second"):
            first = diag.pull(self.c)
            self.assertEqual([a["bundle"] for a in first["apps"]], ["io.s2arm.d2r.ABCDE12345"] * 2)
            self.assertEqual(len({a["path"] for a in first["apps"]}), 2)
            self.assertEqual({(Path(a["path"]) / "host.log").read_text() for a in first["apps"]}, {"batch 1 D1 Documents/host.log", "batch 1 D3 Documents/host.log"})
            copies = [a for a in calls if a[:3] == ("device", "copy", "from")]
            self.assertEqual({a[a.index("--device") + 1] for a in copies}, {"D1", "D3"})
            self.assertEqual(sorted(a[a.index("--source") + 1] for a in copies), sorted(diag.ITEMS * 2))
            self.assertFalse(any("save" in a[a.index("--source") + 1] for a in copies))
            snapshot = {p: p.read_bytes() for p in Path(first["outputPath"]).rglob("*") if p.is_file()}
            batch = 2
            second = diag.pull(self.c)
            self.assertNotEqual(first["outputPath"], second["outputPath"])
            for app in second["apps"]:
                self.assertEqual(app["files"], 1)
                self.assertEqual([p.name for p in Path(app["path"]).iterdir()], ["host.log"])
                self.assertNotIn("U1", app["path"]); self.assertNotIn("U3", app["path"])
            self.assertEqual({p: p.read_bytes() for p in snapshot}, snapshot)
            calls.clear(); self.c.r.update(device="U1")
            selected = diag.pull(self.c)
            self.assertEqual(len(selected["apps"]), 1)
            self.assertTrue(all(a[a.index("--device") + 1] == "D1" for a in calls if "--device" in a))
            self.c.r.update(device="U2")
            with self.assertRaises(Failure): diag.pull(self.c)
            self.c.r.update(device="U1"); batch = 3
            before = set((self.c.private / "diagnostics").iterdir())
            with self.assertRaises(Failure): diag.pull(self.c)
            failed = set((self.c.private / "diagnostics").iterdir()) - before
            self.assertEqual(len(failed), 1)
            self.assertFalse(any(p.is_file() for root in failed for p in root.rglob("*")))
            self.assertEqual({p: p.read_bytes() for p in snapshot}, snapshot)

    def test_pull_diagnostics_brings_each_games_unpack_log(self):
        # #457：多款容器每款的解包日志在 Documents/games/<游戏号>/unpack.log（手机 host.log 写「见 unpack.log」），原先没拉回。
        #   期望（手写）：游戏库里两款，机上只有 A 有这份日志 → 拉回 games/A/unpack.log，B 不留空目录；不碰资源包、存档
        import unittest.mock as um, diag
        A, B = "01008DB008C2C000", "0100000000010000"
        atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id=A, name="A"), dict(id=B, name="B")]))
        devices = {"result": {"devices": [{"identifier": "D1", "hardwareProperties": {"platform": "iOS", "udid": "U1"}, "connectionProperties": {"tunnelState": "connected"}}]}}
        apps = {"result": {"apps": [{"bundleIdentifier": "io.s2arm.games.abcde12345", "name": "S2Arm"}]}}
        sources = []
        def fake(c, *args, timeout=120):
            if args[:2] == ("list", "devices"): return 0, devices, ""
            if args[:3] == ("device", "info", "apps"): return 0, apps, ""
            source = args[args.index("--source") + 1]; sources.append(source)
            if source in ("Documents/host.log", f"Documents/games/{A}/unpack.log"):
                Path(args[args.index("--destination") + 1]).write_text("资源包收推送：失败 " + source)
                return 0, {}, ""
            return 1, {}, "not found"
        with um.patch.object(diag, "devicectl", fake):
            out = diag.pull(self.c)
        app = Path(out["apps"][0]["path"])
        self.assertEqual((app / "games" / A / "unpack.log").read_text(), f"资源包收推送：失败 Documents/games/{A}/unpack.log")
        self.assertFalse((app / "games" / B).exists())
        self.assertEqual(out["apps"][0]["files"], 2)
        self.assertIn(f"Documents/games/{B}/unpack.log", sources)
        self.assertFalse(any("资源包" in s or "save" in s for s in sources))

    def test_pull_diagnostics_hints_resign_when_probe_says_iml_lost(self):
        # 拉回的 diag/iml_probe.json（App 首启复核写）has_iml_ent=false → 一行黄字提示重签；true / null / 没文件 / 坏文件 → 不提示、不报错
        import unittest.mock as um, diag
        devices = {"result": {"devices": [{"identifier": "D1", "hardwareProperties": {"platform": "iOS", "udid": "U1"}, "connectionProperties": {"tunnelState": "connected"}}]}}
        apps = {"result": {"apps": [{"bundleIdentifier": "io.s2arm.acnh", "name": "ACNH S2A"}]}}
        probe = {"v": None}
        def fake(c, *args, timeout=120):
            if args[:2] == ("list", "devices"): return 0, devices, ""
            if args[:3] == ("device", "info", "apps"): return 0, apps, ""
            target = Path(args[args.index("--destination") + 1])
            if target.name != "diag": return 1, {}, ""
            target.mkdir()
            if probe["v"] is not None: (target / "iml_probe.json").write_text(probe["v"])
            return 0, {}, ""
        with um.patch.object(diag, "devicectl", fake):
            for v, hint in (('{"has_iml_ent": false, "window_gb": 5.4}', True), ('{"has_iml_ent": true}', False), ('{"has_iml_ent": null}', False), (None, False), ("{坏", False)):
                self.c.job["diagnostics"].clear(); probe["v"] = v
                diag.pull(self.c)
                got = [x for x in self.c.job["diagnostics"] if x["code"] == "iml_missing"]
                self.assertEqual(len(got), 1 if hint else 0, v)
                if hint:
                    self.assertEqual(got[0]["severity"], "warning")
                    self.assertIn("ACNH S2A：这台机上装的包签名丢了提高内存上限权限", got[0]["summary"])
                    self.assertIn("重签", got[0]["summary"])

    @shared_extract
    def test_ipa_calls_oneclick_with_player_keys_fonts_and_ios(self):
        # 打 iPhone 包：没 SDK → 提示一键安装；有 SDK → 调引擎里的 oneclick.sh，带应用字体、iOS 平台、出到这款文件夹的 build/ios；交付件搬进 image/ios，单款 ipa 不留
        self.player_keys()   # 加密原件出包开头要 prod.keys（library.require_game，v0.3.1）
        import unittest.mock as um, ipa
        base = self.package("base")
        self.catalogue([self.game(packages=[base])])
        res = self.root / "res"; (res / "Fonts").mkdir(parents=True); (res / "Fonts" / "a.bfttf").write_bytes(b"x")
        self.c.r.update(resources=str(res), gameId="0100000000000000")
        import target as tg
        um.patch.object(tg, "os_choices", return_value=[27, 26]).start(); self.addCleanup(um.patch.stopall)
        # 没给目标、也没连设备：一包通用（10-30）→ 不拦，接着走到装 SDK 那一步
        with um.patch.object(tg, "connected", return_value=[]):
            with um.patch.object(ipa, "dotnet_sdk", return_value=(None, None)), self.assertRaises(Failure) as e: ipa.build(self.c)
            self.assertIn("一键安装", str(e.exception))
        self.c.r["target"] = dict(model="iPhone 16 Pro", productType="iPhone17,1", gpuArch="applegpu_g17p", os=26)
        with um.patch.object(ipa, "dotnet_sdk", return_value=(None, None)):
            with self.assertRaises(Failure) as e: ipa.build(self.c)
            self.assertIn("一键安装", str(e.exception))
        seen = {}
        def run(args, log=None, env=None, cwd=None, on_line=None):
            if img_step(args, cwd, env): return
            seen.update(args=[str(a) for a in args], env=env)
            out = Path(args[args.index("--out") + 1]); (out / "x.ipa").write_bytes(b"ipa")
            oneclick_done(args, out / "x.ipa", build_id="abc-123", graphics_mode="direct")
        home = self.root / "dn"; (home / "sdk/10.0.403").mkdir(parents=True); (home / "dotnet").write_text("")
        def build():
            with um.patch.object(ipa, "dotnet_sdk", return_value=(home, home / "dotnet")), um.patch.object(self.c, "run", run), \
                 um.patch.object(type(self.c), "engine", new_callable=um.PropertyMock, return_value=ROOT) if isinstance(getattr(type(self.c), "engine", None), property) else um.patch.object(self.c, "engine", ROOT):
                return ipa.build(self.c)
        r = build()
        a = seen["args"]
        self.assertTrue(a[1].endswith("pack/oneclick/oneclick.sh"))
        # 11-02 多平台共用：从共用解包目录往下（--from-extract），不再暂存原件、一键出包不再自己解（解包、按程序号挑款由 build.ensure_extract 做：--title-id 同 #456）
        self.assertEqual(a[2:4], ["--from-extract", str(self.shared_extract[0])])
        self.assertNotIn("--keys", a)
        self.assertEqual(self.shared_extract[1].call_args.args[1]["id"], "0100000000000000")
        self.assertEqual(a[a.index("--fonts") + 1], str((res / "Fonts").resolve()))
        gdir = layout.find_game(self.c, "0100000000000000")
        self.assertEqual(Path(a[a.index("--out") + 1]), gdir / "build" / "ios")
        self.assertEqual((seen["env"]["S2A_PACK_PLAT"], seen["env"]["DOTNET_ROOT"]), ("ios", str(home)))
        # 直连包本身仍与机型无关（一包通用 10-30）；随包管线归档按架构出，目标设备认得出 GPU 架构就交给出包（ipa.pso_env），
        #   别的机型装这个包照常能玩，只是首遇现编（11-20 维护者定）
        self.assertEqual((seen["env"]["S2A_PSO_ARCHS"], seen["env"]["S2A_PSO_OS"]), ("applegpu_g17p", "26"))
        self.assertEqual(r["buildId"], "abc-123")
        image = gdir / "image" / "ios"
        self.assertEqual((r["ipa"], r["outputPath"]), ("", str(image)))   # 不再交付单款 ipa：装机时现组容器
        self.assertTrue((layout.ios_kit(self.c, "0100000000000000") / "main.npdm").is_file()); self.assertTrue((image / ".pack" / "manifest.json").is_file())
        self.assertFalse((gdir / "build" / "ios" / "x.ipa").exists())
        man = json.loads((image / ".pack" / "manifest.json").read_text())   # 交付转成单一 IMG 形态：game.json 记的资源包号 = 新清单的
        self.assertEqual((man["format"], man["imgFile"], (image / ".pack" / "game.img").is_file()), ("img", "game.img", True))
        self.assertEqual(json.loads((gdir / "game.json").read_text())["ios"]["pack"], man["index_sha256"])
        # 存档游戏身份（#298 同批）：--from-extract 时一键出包不自己生成，作业层写好经 S2A_SAVE_IDENTITY 交过去；
        #   内容 = Mac 存档作业认的同一份（saves.identity），并能过出包侧 save_identity.py --check（iPhone 存档管理 / iCloud 同步靠它）
        import saves
        si = Path(seen["env"]["S2A_SAVE_IDENTITY"])
        game = json.loads((self.c.library / "catalog.json").read_text())["games"][0]
        self.assertEqual(json.loads(si.read_text()), saves.identity(game))
        self.assertEqual(json.loads(si.read_text())["id"], "0100000000000000")
        chk = subprocess.run([sys.executable, str(ROOT / "pack/save_identity.py"), "--check", str(si)], capture_output=True, text=True)
        self.assertEqual(chk.returncode, 0, chk.stderr)

    def test_ipa_follow_up_keeps_unpack_for_next_platform(self):
        # 剃刀 M21：同一次出包后面还有平台（followUp）→ 收尾不调 prune_build；缺省 / false 照调（期望按 pack/contracts/README.md 手写）。
        #   只测收尾判断本身（ipa.prune_unless_follow_up），不跑整份 ipa.build（会留下解包状态影响别的用例）
        import unittest.mock as um, ipa
        for follow, want in ((True, 0), (False, 1), (None, 1)):
            with self.subTest(followUp=follow):
                self.c.r.pop("followUp", None)
                if follow is not None:
                    self.c.r["followUp"] = follow
                with um.patch.object(library, "prune_build") as prune:
                    pruned = ipa.prune_unless_follow_up(self.c, "0100000000000000")
                self.assertEqual((prune.call_count, pruned), (want, bool(want)))
        self.c.r.pop("followUp", None)

    def test_ipa_prune_failure_after_success_is_only_a_warning(self):
        # #793 / #796：包已经出好，收尾清中间产物删不掉（游戏库在 Xbox App 管的文件夹，PermissionError）→ 只记一条警告、不抛，出包照算成功
        import unittest.mock as um, ipa
        notes = []
        with um.patch.object(library, "prune_build", side_effect=PermissionError(13, "Access is denied", "x.bin")), \
             um.patch.object(self.c, "diagnostic", lambda msg, **k: notes.append((msg, k.get("severity")))):
            pruned = ipa.prune_unless_follow_up(self.c, "0100000000000000")
        self.assertFalse(pruned)
        self.assertEqual(len(notes), 1); self.assertIn("清理中间产物没有完成", notes[0][0]); self.assertEqual(notes[0][1], "warning")

    @shared_extract
    def test_ipa_result_is_the_delivered_file_not_the_newest_in_output(self):
        # #67 qidian99 反证：输出目录里有修改时间更「新」（2100 年）的旧 IPA 和资源包，本轮交付的是 current-title.ipa → 搬进 image/ios 的必须是本轮的资源包，不按 mtime 挑
        self.player_keys()   # 加密原件出包开头要 prod.keys（library.require_game，v0.3.1）
        import unittest.mock as um, ipa
        base = self.package("base"); self.catalogue([self.game(packages=[base])])
        self.c.r.update(gameId="0100000000000000", target={"gpuArch": "applegpu_g18p", "os": 27})
        res = self.root / "res"; (res / "Fonts").mkdir(parents=True); (res / "Fonts/a.bfttf").write_bytes(b"f")
        out = layout.ios_work(self.c, "0100000000000000"); out.mkdir(parents=True)
        old = out / "old-title.ipa"; old.write_bytes(b"old"); os.utime(old, (4102444800, 4102444800))
        old_pack = out / "old-title资源包"; old_pack.mkdir(); (old_pack / "manifest.json").write_text('{"index_sha256": "old-pack"}'); os.utime(old_pack, (4102444800, 4102444800))
        # Q15: real synthetic cache payload, mocked external packaging only.
        cache = self.c.work_root / 'cache/air/ios/aa' / ('a' * 64 + '.air')
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b'synthetic reusable AIR')
        direct = self.c.work_root / 'run/s2arm/cache/direct/0100000000000000'
        direct_lib = direct / 'store/aa.pack'   # S2Arm 10-31: the direct cache is the store (shard files); the old ios/ / mac/ trees are cleared as transients
        direct_work = direct / 'work/log'
        for path in (direct_lib, direct_work):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic direct')
        import cache_retention
        lines = {}
        def run(args, log=None, env=None, cwd=None, on_line=None):
            if img_step(args, cwd, env): return
            o = Path(args[args.index("--out") + 1]); (o / "current-title.ipa").write_bytes(b"new")
            if lines.get("done", True):
                oneclick_done(args, o / "current-title.ipa", pack_id="current-pack", build_id="current-123", graphics_mode="direct")
        home = self.root / "dn"; (home / "sdk/10.0.403").mkdir(parents=True); (home / "dotnet").write_text("")
        with um.patch.object(ipa, "dotnet_sdk", return_value=(home, home / "dotnet")), um.patch.object(self.c, "run", run), \
             um.patch.object(ipa, "resolve_target", return_value={"gpuArch": "applegpu_g18p", "os": 27}), \
             um.patch.object(self.c, "fonts_dir", return_value=res / "Fonts"), \
             um.patch.object(library, "check_pack_space", return_value={"compilerMinFreeBytes": 48 << 30}) as preflight, \
             um.patch.object(cache_retention, "trim", wraps=cache_retention.trim) as trim, \
             um.patch.object(cache_retention.shutil, "disk_usage", return_value=um.Mock(free=100 * 1024**3)) as disk:
            r = ipa.build(self.c)
            image = layout.image_ios(self.c, "0100000000000000")
            self.assertEqual(r["buildId"], "current-123")
            man = json.loads((image / ".pack" / "manifest.json").read_text())   # 本轮交付的那份（转成 IMG 后清单号按新 index.tsv 算）：exefs/main.npdm 内容是本轮的资源包号
            self.assertEqual(man["format"], "img")
            self.assertEqual(layout.game_info(self.c, "0100000000000000")["ios"]["pack"], man["index_sha256"])
            import importlib.util; spec = importlib.util.spec_from_file_location("img", ROOT / "pack/img.py"); img = importlib.util.module_from_spec(spec); spec.loader.exec_module(img)
            g = str(image / ".pack" / "game.img"); off, n = img.find(g, "exefs/main.npdm"); fh = open(g, "rb"); fh.seek(off)
            self.assertEqual(fh.read(n), b"current-pack")
            self.assertEqual(cache.read_bytes(), b'synthetic reusable AIR')
            self.assertEqual(direct_lib.read_bytes(), b'synthetic direct')
            self.assertFalse(direct_work.exists())
            self.assertEqual(r['compilerCache']['removedEntries'], 0)
            self.assertEqual(trim.call_count, 1)
            self.assertEqual(trim.call_args.kwargs['min_free_bytes'], 48 << 30)
            self.assertIn('compiler_policy', preflight.call_args.kwargs)
            # Optional eviction refusal must warn, not hide the validated deliverable.
            os.utime(cache, (1, 1))
            real_unlink = Path.unlink
            def refuse_cache_unlink(path, *args, **kwargs):
                if path == cache:
                    raise PermissionError(13, 'synthetic read-only cache', str(path))
                return real_unlink(path, *args, **kwargs)
            disk.return_value.free = 0  # controlled disk-pressure oracle, no real disk filled
            with um.patch.dict(os.environ, {'S2A_COMPILER_CACHE_MIN_FREE_BYTES': str(10**15)}), \
                 um.patch.object(Path, 'unlink', refuse_cache_unlink):
                warned = ipa.build(self.c)
            self.assertEqual(warned['buildId'], 'current-123')
            self.assertEqual(warned['compilerCache']['evictionErrors'], 1)
            warnings = [d for d in self.c.job['diagnostics'] if d['code'] == 'compiler_cache_incomplete']
            self.assertEqual(len(warnings), 1)
            self.assertEqual(warnings[0]['severity'], 'warning')
            self.assertEqual(cache.read_bytes(), b'synthetic reusable AIR')
            # 事件流里没有 result（一键出包中途退出等）→ 报错，不退回按时间猜
            lines["done"] = False
            with self.assertRaises(Failure):
                ipa.build(self.c)
            self.assertEqual(trim.call_count, 2, 'failed packaging must not trim caches')
            self.assertEqual(cache.read_bytes(), b'synthetic reusable AIR')

    def test_run_reaps_leftover_session_processes(self):
        # 10-26 0100C2500FC20000 Spare-host：工具退出后，留在它会话里的孙进程（dl_gen 超时只杀 metal-tt，air-tt 过继给 launchd 继续编）要收掉；
        #   有意活得比任务久、自立会话的（宽屏自动出表，perl setsid）不收
        import shlex
        d = self.root / "reap"; d.mkdir(); q = shlex.quote(str(d))
        #   两个后台都等到进程名变成 sleep 再退出（exec 完成；自立会话那个 setsid 在 exec 之前）：机器忙时 fork 后还没 exec，收残留扫到的是「bash」，日志对不上（10-26 gate-host 高负载偶发红）
        script = (f'sleep 60 & echo $! > {q}/orphan; '
                  f"(exec perl -MPOSIX -e 'POSIX::setsid(); exec @ARGV' -- sleep 60) > /dev/null 2>&1 < /dev/null & echo $! > {q}/detached; "
                  f'for p in $(cat {q}/orphan {q}/detached); do for i in $(seq 1 600); do [ "$(ps -o comm= -p $p)" = sleep ] && break; sleep 0.05; done; done; exit 0')
        self.c.run(["bash", "-c", script], log=d / "run.log")
        orphan, detached = (int((d / n).read_text()) for n in ("orphan", "detached"))
        def alive(pid):
            try: os.kill(pid, 0); return True
            except OSError: return False
        try:
            self.assertFalse(alive(orphan), "留在任务会话里的孙进程应被收掉")
            self.assertTrue(alive(detached), "自立会话的后台不该被收")
            self.assertIn("收掉本阶段工具会话里的残留进程 1 个：sleep", (d / "run.log").read_text())
        finally:
            for pid in (orphan, detached):
                if alive(pid): os.kill(pid, 9)

    @shared_extract
    def test_ipa_aoc_missing_becomes_warning(self):
        # 审计 P1-74：追加内容没解出，本体照常出（不挡），但结果事件的 aoc_missing 要成一条黄字 aoc_missing，任务记「完成（有警告）」、完成页不画完成章
        self.player_keys()   # 加密原件出包开头要 prod.keys（library.require_game，v0.3.1）
        import unittest.mock as um, ipa
        base = self.package("base"); self.catalogue([self.game(packages=[base])])
        self.c.r.update(gameId="0100000000000000", target={"gpuArch": "applegpu_g18p", "os": 27})
        res = self.root / "res"; (res / "Fonts").mkdir(parents=True); (res / "Fonts/a.bfttf").write_bytes(b"f")
        miss = {"v": ["DLC 套装 A.nsp", "dlc_b.nsp"]}
        def run(args, log=None, env=None, cwd=None, on_line=None):
            if img_step(args, cwd, env): return
            o = Path(args[args.index("--out") + 1]); (o / "g.ipa").write_bytes(b"new")
            oneclick_done(args, o / "g.ipa", graphics_mode="direct", aoc_missing=miss["v"])
        home = self.root / "dn"; (home / "sdk/10.0.403").mkdir(parents=True); (home / "dotnet").write_text("")
        with um.patch.object(ipa, "dotnet_sdk", return_value=(home, home / "dotnet")), um.patch.object(self.c, "run", run), \
             um.patch.object(ipa, "resolve_target", return_value={"gpuArch": "applegpu_g18p", "os": 27}), \
             um.patch.object(self.c, "fonts_dir", return_value=res / "Fonts"):
            ipa.build(self.c)
            d = [x for x in self.c.job["diagnostics"] if x["code"] == "aoc_missing"]
            self.assertEqual([(x["severity"], x["summary"]) for x in d], [("warning", "追加内容 2 份没解出，包里不含（DLC 套装 A.nsp、dlc_b.nsp）")])
            # 都解出了（空名单）→ 不加
            miss["v"] = []; self.c.job["diagnostics"].clear()
            ipa.build(self.c)
            self.assertEqual([x for x in self.c.job["diagnostics"] if x["code"] == "aoc_missing"], [])

    def test_dotnet_sdk_user_directory(self):
        import unittest.mock as um, ipa, shlex
        home = self.root / "user home"
        old = home / ".dotnet/sdk/10.0.100"; old.mkdir(parents=True)
        host = home / ".dotnet/dotnet"
        def lists(*versions):
            host.write_text('#!/bin/sh\n[ "$1" = --list-sdks ] || exit 2\n' + "".join(f'printf "%s\\n" {shlex.quote(f"{v} [{old.parent}]")}\n' for v in versions))
            host.chmod(0o755)
        env = {"DOTNET": "", "DOTNET_ROOT": "", "DOTNET_ROOT_ARM64": "", "DOTNET_ROOT_X64": "", "PATH": ""}
        # 候选只留本测试临时目录里的：实现还会探测 /usr/local/share/dotnet、Homebrew 等固定位置，
        #   装了系统级 .NET 的机器上会探到真 SDK，「只有 10.0.100 时认不出」就随机器而变（10-24 #76 作者 Intel 机上实撞）
        real = ipa._dotnet_candidates
        only_tmp = lambda: [p for p in real() if str(p).startswith(str(self.root))]
        with um.patch.object(Path, "home", return_value=home), um.patch.dict(os.environ, env), \
             um.patch.object(ipa, "_dotnet_candidates", side_effect=only_tmp):
            # compat/tree/global.json 要 10.0.403 + latestFeature：只有 10.0.100 不算有 SDK
            lists("10.0.100")
            self.assertEqual(ipa.dotnet_sdk(), (None, None))
            (old.parent / "10.0.403").mkdir()
            lists("10.0.100", "10.0.403")
            self.assertEqual(ipa.dotnet_sdk(), (home / ".dotnet", host))

    def test_install_dotnet_reports_download_progress(self):
        # 10-24 #152：官方安装脚本下载时零输出、23 分钟看着像卡死。期望：按 --dry-run 给的包地址和 HEAD 大小，下载期间报「已下载 N / 总数」
        import unittest.mock as um, toolchain, ipa, subprocess as sp, time
        url = "https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.402/dotnet-sdk-10.0.402-osx-x64.tar.gz"
        def fake_sub(args, *a, **k):
            if "--dry-run" in args:
                return sp.CompletedProcess(args, 0, "dotnet-install: URL #0 - aka.ms: " + url + "\n", "")
            if args[:2] == ["curl", "-fsIL"]:
                return sp.CompletedProcess(args, 0, "HTTP/2 200\r\ncontent-length: 3000\r\n", "")
            raise AssertionError(args)
        seen = []
        def fake_run(args, log=None, env=None, cwd=None, on_line=None):
            if args[0] == "curl":
                return
            zp = Path(env["zip_path"])
            for i in range(3):
                with zp.open("ab") as f: f.write(b"x" * 1000)
                time.sleep(0.15)
        with um.patch.object(toolchain, "DOTNET_PROGRESS_S", 0.05), um.patch.object(toolchain.subprocess, "run", side_effect=fake_sub), \
             um.patch.object(self.c, "run", side_effect=fake_run), um.patch.object(self.c, "emit", side_effect=lambda *a, **k: seen.append(k)), \
             um.patch.object(ipa, "dotnet_sdk", return_value=(Path("/x"), Path("/x/dotnet"))):
            toolchain.install_dotnet(self.c)
        progress = [k for k in seen if k.get("unit") == "B"]
        self.assertTrue(progress, "下载期间没有进度")
        self.assertEqual(progress[0]["total"], 3000.0)
        self.assertTrue(all(0 < k["done"] <= 3000 for k in progress))

    def test_dotnet_sdk_version_rule_matches_global_json(self):
        # 期望按 .NET 的 rollForward 规则手写（与 tools/doctor.sh 同一判法），不跑被测代码推
        import ipa
        req = ("10.0.403", "latestFeature")
        for version, ok in [("10.0.403", True), ("10.0.404", True), ("10.0.500", True), ("10.0.402", False), ("10.0.400", False),
                            ("10.0.301", False), ("10.0.100", False), ("10.1.100", False), ("11.0.100", False), ("9.0.300", False),
                            ("10.0.404-rc.1.25", True)]:
            self.assertEqual(ipa._sdk_ok(version, req), ok, version)
        self.assertTrue(ipa._sdk_ok("10.0.100", None))
        self.assertFalse(ipa._sdk_ok("9.0.100", None))
        self.assertEqual(ipa._sdk_requirement(), req)

    def test_host_crash_quiet_for_helpers_not_for_player_launch(self):
        # 出包类作业起的 s2arm-host 是辅助运行：S2A_CRASH_QUIET=1（runtime/src/native.h，出错只写日志、不弹 macOS「意外退出」框）；
        #   玩家起局（launch）不静默，只有启动器是 --control 测试实例（S2ARM_CONTROL=1）时也静默（期望按 core.env 注释手写）
        import unittest.mock as um
        for op, control, want in (("ipa", None, "1"), ("build", None, "1"), ("launch", None, None), ("launch", "1", "1")):
            with self.subTest(op=op, control=control):
                c = Context(dict(self.request, operation=op))
                with um.patch.dict(os.environ, {"S2ARM_CONTROL": control} if control else {}, clear=False):
                    if not control: os.environ.pop("S2ARM_CONTROL", None)
                    self.assertEqual(c.env().get("S2A_CRASH_QUIET"), want)

    @shared_extract
    def test_dotnet_wrapper_fallback_and_ipa_environment(self):
        self.player_keys()   # 加密原件出包开头要 prod.keys（library.require_game，v0.3.1）
        import unittest.mock as um, ipa, shlex
        home = self.root / "user home"; home.mkdir()
        sdk_root = self.root / "actual SDK root"
        (sdk_root / "sdk/10.0.403").mkdir(parents=True)
        bins = [self.root / name for name in ("runtime bin", "failed bin", "timeout bin", "wrapper bin")]
        scripts = ['exit 0', 'exit 7', 'exec /bin/sleep 10',
                   '[ -z "${DOTNET_ROOT:-}${DOTNET_ROOT_ARM64:-}${DOTNET_ROOT_X64:-}" ] || exit 9\nprintf "%s\\n" ' + shlex.quote(f"10.0.403 [{sdk_root / 'sdk'}]")]
        for directory, script in zip(bins, scripts):
            directory.mkdir()
            host = directory / "dotnet"
            host.write_text('#!/bin/sh\n[ "$1" = --list-sdks ] || exit 2\n' + script + '\n')
            host.chmod(0o755)
        selected = bins[-1] / "dotnet"
        real_probe = subprocess.run
        def probe(args, **kwargs):
            if args[0] == str(bins[2] / "dotnet"):
                self.assertEqual(kwargs["timeout"], 3)
                raise subprocess.TimeoutExpired(args, kwargs["timeout"])
            # 真跑的桩（成功 / 失败退出 / 包装脚本）不按产品的 3 秒探测上限：这条用例测的是回退顺序与环境剥离，不是快慢；
            #   11-02 合入线：满负载时 sh 桩起进程超 3 秒，包装脚本那支被当超时跳过，返回 (None, None) 偶发红
            return real_probe(args, **{**kwargs, "timeout": 120})
        base = self.package("base"); self.catalogue([self.game(packages=[base])])
        self.c.r.update(gameId="0100000000000000", target={"gpuArch": "applegpu_g18p", "os": 27})
        fonts = self.root / "fonts"; fonts.mkdir()
        seen = {}
        def run(args, log=None, env=None, cwd=None, on_line=None):
            if img_step(args, cwd, env): return
            seen.update(env=env)
            out = Path(args[args.index("--out") + 1]); (out / "synthetic.ipa").write_bytes(b"synthetic IPA fixture")
            oneclick_done(args, out / "synthetic.ipa", graphics_mode="direct")
        env = {"DOTNET": str(bins[0] / "dotnet"), "DOTNET_ROOT": str(bins[0]),
               "DOTNET_ROOT_ARM64": str(bins[0]), "DOTNET_ROOT_X64": str(bins[0]), "PATH": os.pathsep.join(str(p) for p in bins)}
        with um.patch.object(Path, "home", return_value=home), um.patch.dict(os.environ, env), um.patch.object(ipa.subprocess, "run", side_effect=probe):
            self.assertEqual(ipa.dotnet_sdk(), (sdk_root, selected))
            with um.patch.object(ipa, "resolve_target", return_value={"gpuArch": "applegpu_g18p", "os": 27}), \
                 um.patch.object(self.c, "fonts_dir", return_value=fonts), um.patch.object(self.c, "env", return_value=dict(env)), \
                 um.patch.object(self.c, "run", side_effect=run):
                ipa.build(self.c)
            self.assertEqual(seen["env"]["DOTNET"], str(selected))
            self.assertEqual(seen["env"]["DOTNET_ROOT"], str(sdk_root))
            self.assertEqual(seen["env"]["PATH"].split(os.pathsep)[:2], [str(selected.parent), str(sdk_root)])
            self.assertNotIn("DOTNET_ROOT_ARM64", seen["env"])
            self.assertNotIn("DOTNET_ROOT_X64", seen["env"])
            # Runtime-only, failed and timed-out candidates do not count as an SDK.
            with um.patch.object(ipa, "_dotnet_candidates", return_value=[p / "dotnet" for p in bins[:-1]]):
                self.assertEqual(ipa.dotnet_sdk(), (None, None))

    def test_target_arch_via_deployment_variant(self):
        # 机型号 → GPU 架构：Xcode 27.1 的库里 Devices.DeviceTraitSet 是占位（指向 g14g），真值在 DeploymentVariant（老系统行是 air64、要跳过）；
        # iPad 机型号带 -A / -B 后缀按前缀认；没有 applegpu 记录 → 报「认不出」；可选机型只列 g13（A14）起、新的在前、同名去重
        import sqlite3, target as tg
        dev = self.root / "Xcode.app/Contents/Developer"; db = dev / "Platforms/iPhoneOS.platform/usr/standalone/device_traits.db"; db.parent.mkdir(parents=True)
        con = sqlite3.connect(db)
        con.executescript("""create table DeviceTraits (DeviceTraitSetID integer primary key, GPUArchName text);
            create table Devices (Target text, ProductType text, ProductDescription text, DeviceTraitSet integer);
            create table DeploymentVariant (DeploymentTarget text, DeviceTarget text, DeviceTraitSet integer);
            insert into DeviceTraits values (1,'applegpu_g14g'),(2,'air64'),(3,'applegpu_g18p'),(4,'applegpu_g17g'),(5,'applegpu_g12p'),(6,'applegpu_g13p');
            insert into Devices values ('v54ap','iPhone18,2','iPhone 17 Pro Max',1),('j717ap','iPad17,1-A','iPad Pro 11-inch (M5)',1),('j717bap','iPad17,1-B','iPad Pro 11-inch (M5)',1),
                ('n104ap','iPhone12,1','iPhone 11',1),('k93ap','iPad2,1','iPad 2',1),('d53gap','iPhone13,2','iPhone 12',1);
            insert into DeploymentVariant values ('17.0','v54ap',2),('26.0','v54ap',3),('26.0','j717ap',4),('26.0','j717bap',4),('26.0','n104ap',5),('9.0','k93ap',2),('26.0','d53gap',6);""")
        con.commit(); con.close()
        self.assertEqual(tg.gpu_arch("iPhone18,2", dev), "applegpu_g18p")
        self.assertEqual(tg.gpu_arch("iPad17,1", dev), "applegpu_g17g")
        with self.assertRaises(tg.TargetError): tg.gpu_arch("iPad2,1", dev)
        self.assertEqual([(m["name"], m["productType"]) for m in tg.models(dev)],
                         [("iPhone 17 Pro Max", "iPhone18,2"), ("iPhone 12", "iPhone13,2"), ("iPad Pro 11-inch (M5)", "iPad17,1")])
        self.assertEqual(tg.check(dict(model="m", gpuArch="applegpu_g13p", os=26)), dict(gpuArch="applegpu_g13p", os=26, model="m"))
        for bad in (dict(gpuArch="applegpu_g12p", os=27), dict(gpuArch="applegpu_g18p", os=17), dict(gpuArch="", os=27)):
            with self.assertRaises(tg.TargetError): tg.check(bad)
        self.assertEqual(tg.check(dict(gpuArch="applegpu_g18p", os=27), 27)["os"], 27)
        with self.assertRaises(tg.TargetError) as e: tg.check(dict(gpuArch="applegpu_g18p", os=27), 26)
        self.assertIn("更新到最新版", str(e.exception))


    def test_pack_space_needs_unpack_plus_direct(self):
        # 预检门槛 = 原包 × 1.1 × 2（解包 + 单文件镜像）+ 直连上限：恰好够放行，差 1 字节拦下，提示里写需要与剩余
        import unittest.mock as um, collections
        base = dict(self.package("base"), size=6 << 30)
        game = self.game(packages=[base]); need = (6 << 30) * 11 // 10 + 15_000_000_000
        du = collections.namedtuple("du", "total used free")
        with um.patch("shutil.disk_usage", return_value=du(0, 0, need)):
            library.check_pack_space(self.c, game)
        with um.patch("shutil.disk_usage", return_value=du(0, 0, need - 1)), um.patch.object(self.c, "diagnostic") as diag:
            library.check_pack_space(self.c, game)   # 11-14 不许堵门：差 1 字节也不拦，只提示
        msg = diag.call_args.args[0]
        self.assertEqual((diag.call_args.kwargs.get("code"), diag.call_args.kwargs.get("severity")), ("disk_full", "warning"))
        self.assertIn(f"要 {need / 1e9:.0f} GB", msg); self.assertIn(f"只剩 {(need - 1) / 1e9:.0f} GB", msg)

    def test_stage_sources_links_originals_in_place(self):
        # 原件原地只读（11-02）：出包原件目录里是指向原件的软链（按内容哈希命名、整合版同一文件列两次只链一次），不复制、不改原件；
        #   旧版复制进游戏库的托管副本（相对路径）照样链到库里那份
        outside = self.root / "exfat-drive"; outside.mkdir()
        original = outside / "整合版.xci"; original.write_bytes(b"synthetic combined image")
        base = dict(self.package("base"), path=str(original), sha256="a" * 64)
        update = dict(self.package("update"), path=str(original), sha256="a" * 64)
        legacy = dict(self.package("dlc", sha="c" * 64))
        (self.c.library / legacy["path"]).write_bytes(b"legacy managed copy")
        stage = self.root / "stage"
        library.stage_sources(self.c, [base, update, legacy], stage)
        entries = sorted(stage.iterdir())
        self.assertEqual([p.name for p in entries], ["a" * 64 + ".xci", "c" * 64 + Path(legacy["path"]).suffix])
        self.assertTrue(all(p.is_symlink() for p in entries))
        self.assertEqual(entries[0].resolve(), original.resolve())
        self.assertEqual(entries[1].resolve(), (self.c.library / legacy["path"]).resolve())
        self.assertEqual(original.read_bytes(), b"synthetic combined image")

    def test_missing_original_says_where(self):
        # 原件在的盘没接上 / 被挪走：缺项直接写原件位置和怎么办，不是「托管文件缺失」
        gone = self.root / "unplugged" / "game.nsp"
        game = self.game(packages=[dict(self.package("base"), path=str(gone))])
        [missing] = library.requirements(self.c, game)
        self.assertIn("找不到游戏文件「" + gone.name, missing); self.assertNotIn(str(gone.parent), missing); self.assertIn("接上磁盘", missing)

    def space_events(self):
        return [json.loads(l) for l in self.c.events.read_text().splitlines() if json.loads(l)["type"] == "space"]

    def test_space_budget_adds_direct_library_for_every_game(self):
        # 直连优先、零配置（10-25）：每款都另加直连库预算（按01006F8002326000实测 × 体积换算），不看有没有手写配置档；只有直连（11-07），没有不加的分支
        import collections
        import unittest.mock as um
        du = collections.namedtuple("du", "total used free")
        for tid in ("01006F8002326000", "0100000000000000"):
            with self.subTest(tid=tid):
                game = self.game(tid=tid, packages=[dict(self.package("base", tid=tid), size=6 << 30)])
                with um.patch("shutil.disk_usage", return_value=du(0, 0, 50 << 30)):
                    lib = library.space_budget(self.c, game)["volumes"][0]
                self.assertEqual(lib["needBytes"], (6 << 30) * 11 // 10 + 15_000_000_000)
                self.assertIn("直连着色器上限", lib["note"])

    def test_catalog_says_whether_keys_are_needed(self):
        # 剃刀 W10：catalog 每款带 needsKeys，判据只有 library.KEYLESS_FORMATS 一份（前端不再各算一份格式名单）。
        #   期望手写：NSP 原件 → 要；NUP 容器 / 解包目录（extract）→ 不要；NUP 本体 + NSP 的 DLC → 要
        self.assertTrue(library.needs_keys([dict(format="nsp")]))
        self.assertTrue(library.needs_keys([dict()]))   # 旧记录没有 format：按加密原件算（宁可提示导入密钥，不放过）
        self.assertFalse(library.needs_keys([dict(format="nup"), dict(format="extract")]))
        self.assertTrue(library.needs_keys([dict(format="nup"), dict(format="nsp")]))
        game = self.game(packages=[self.package()]); self.catalogue([game])
        out = library.catalog(self.c)
        self.assertEqual(out["games"][0]["needsKeys"], library.needs_keys(library.selected_packages(game)))

    def test_space_budget_covers_measured_peaks(self):
        # 11-10 冷出包实测（Mac，S2Arm.app 起作业，临时游戏库）：峰值按 library.disk_bytes（APFS 克隆共享块只算一次、同存储页与实测账口径）只量出包作业
        #   自己的目录，扣掉硬链进来的原件，不用整盘空闲下降（混进别的会话写盘、同款两次差 1 GiB）。按原件的规则必须 ≥ 峰值；
        #   #265 按抽取量的那支已删：0100559011740000 抽取 0.48 GiB 估 17.30 GiB < 峰值 17.77 GiB
        import collections
        import unittest.mock as um
        du = collections.namedtuple("du", "total used free")
        GIB = 1 << 30
        cases = (("010024B00F9D0000", 433397760, 1.03), ("01008FC00D964000", 10776169984, 18.61), ("0100559011740000", 11299842048, 17.77))
        for tid, size, peak in cases:
            with self.subTest(tid=tid):
                game = self.game(tid=tid, packages=[dict(self.package("base", tid=tid), size=size)])
                with um.patch("shutil.disk_usage", return_value=du(0, 0, 500 * GIB)):
                    need = library.space_budget(self.c, game)["volumes"][0]["needBytes"]
                self.assertGreaterEqual(need, int(peak * GIB), f"{tid}：估算 {need / GIB:.2f} GiB < 实测峰值 {peak} GiB")
                self.assertIn("直连着色器上限", library.space_budget(self.c, game)["volumes"][0]["note"])

    def test_offline_empty_warns(self):
        # #470：直连库只能运行时现编（shaders/direct/runtime_only.txt，首行原因）→ 完成页一条黄字 direct_offline_empty、文案带原因；没有这个文件 → 不出诊断
        import build
        root = self.root / "r470"; (root / "shaders/direct").mkdir(parents=True)
        build.warn_offline_empty(self.c, root)
        self.assertEqual(self.c.job["diagnostics"], [])
        (root / "shaders/direct/runtime_only.txt").write_text("直连库失败：直连抽取失败（没有改写模块目录）\n第二行不要\n", encoding="utf-8")
        build.warn_offline_empty(self.c, root)
        d = self.c.job["diagnostics"]
        self.assertEqual([x["code"] for x in d], ["direct_offline_empty"])
        self.assertIn("离线预编 0 支", d[0]["summary"] + d[0].get("detail", "") + str(d[0]))
        self.assertIn("没有改写模块目录", str(d[0])); self.assertNotIn("第二行不要", str(d[0]))

    def test_degrade_notes_and_log(self):
        # v0.3.1 降级继续（开发丙线约定）：产物根 degrade.<阶段>.tsv，五列 阶段 / 类别 / 处数 / 玩家提示 / 说明，# 开头是表头。
        #   期望手写：两个阶段各一行 + 一行重复提示 → 完成卡提示两条（去重、按文件名序：direct 在 rewrite 前）；
        #   列数不对 / 处数不是数字的行跳过、发一条消息、不报错；整表存进 <任务>-degrade.log；没有表 → 空、不写日志
        import build
        root = self.root / "rdeg"; root.mkdir()
        self.assertEqual(build.collect_degrade(self.c, root), [])
        self.assertFalse((self.c.logs / f"{self.c.task}-degrade.log").exists())
        (root / "degrade.rewrite.tsv").write_text("# 阶段\t类别\t处数\t玩家提示\t说明\n"
                                                  "rewrite\tx18_compat\t27\t这款有 27 处用了兼容写法\t恢复 sp 后挑不出临时寄存器\n"
                                                  "rewrite\tbroken\n"
                                                  "rewrite\tx\tmany\t提示\t说明\n", encoding="utf-8")
        (root / "degrade.direct.tsv").write_text("direct\tlazy\t3\t3 支着色器首次遇到时现编\t抽不出\n"
                                                 "direct\tlazy2\t1\t3 支着色器首次遇到时现编\t同一句\n", encoding="utf-8")
        notes = build.collect_degrade(self.c, root)
        self.assertEqual(notes, ["3 支着色器首次遇到时现编", "这款有 27 处用了兼容写法"])
        log = (self.c.logs / f"{self.c.task}-degrade.log").read_text(encoding="utf-8").splitlines()
        self.assertEqual(log[0], build.DEGRADE_HEAD)
        self.assertEqual(len(log), 4)   # 表头 + 3 条有效行
        self.assertIn("rewrite\tx18_compat\t27\t", "\n".join(log))
        self.assertEqual(self.c.job["diagnostics"], [])   # 降级不当错误

    def test_diagnostics_degrade_summary(self):
        # 诊断包：任务日志目录的 <任务>-degrade.log 随 logs/ 收进包，manifest 加「降级汇总」degrade_summary：任务号 → {阶段/类别: 处数}（同类别相加）
        import zipfile
        (self.c.logs / "t1-degrade.log").write_text("# 阶段\t类别\t处数\t玩家提示\t说明\nrewrite\tx18\t27\t提示\t说明\nrewrite\tx18\t3\t提示\t说明\ndirect\tlazy\t5\t提示\t说明\n", encoding="utf-8")
        destination = self.root / "diag-degrade.zip"; self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            self.assertIn("logs/t1-degrade.log", archive.namelist())
            self.assertEqual(json.loads(archive.read("manifest.json"))["degrade_summary"], {"t1": {"rewrite/x18": 30, "direct/lazy": 5}})

    def test_diagnostics_carries_pipeline_records(self):
        # 本机管线记录（11-13 维护者：诊断收进众包种子）：游戏库里各款 temp/<号>/wire/<号>/pso_user.tsv 原样进 pso/<号>.tsv；没跑过（没这份）的不放；
        #   空文件、超过 4 MB 的、链接都不放。期望按规则手写
        import zipfile
        A, B, C, D = "0100AAAA00000000", "0100BBBB00000000", "0100CCCC00000000", "0100DDDD00000000"
        atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id=g, name=g) for g in (A, B, C, D)]))
        def rec(g, data):
            f = self.c.library / "temp" / g / "wire" / g / "pso_user.tsv"; f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(data); return f
        rec(A, b"S\t0123456789abcdef_b\t1\nC\t1111222233334444_b\n")
        rec(C, b"")
        rec(D, b"x" * ((4 << 20) + 1))
        destination = self.root / "diag-pso.zip"; destination.unlink(missing_ok=True); self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            names = sorted(n for n in archive.namelist() if n.startswith("pso/"))
            self.assertEqual(names, [f"pso/{A}.tsv"])
            self.assertEqual(archive.read(f"pso/{A}.tsv"), b"S\t0123456789abcdef_b\t1\nC\t1111222233334444_b\n")
            self.assertIn(f"pso/{A}.tsv", json.loads(archive.read("manifest.json"))["files"])

    def test_diagnostics_zip_budget_skips_optional_members_and_records_them(self):
        # 整包预算（DIAGNOSTIC_ZIP_BUDGET，收件 30 MB 上限）：日志先放；放不下的可选成员（这里是管线记录）跳过、记进 manifest budget.skipped，
        #   整包压缩后不超预算。预算临时压到 600 KB，三份各 400 KB 不可压缩的管线记录只放得下 1 份。期望按规则手写
        import zipfile
        from unittest import mock
        games = ["0100AAAA00000000", "0100BBBB00000000", "0100CCCC00000000"]
        atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id=g, name=g) for g in games]))
        for g in games:
            f = self.c.library / "temp" / g / "wire" / g / "pso_user.tsv"; f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(os.urandom(400 << 10))
        (self.c.logs / "budget-host.log").write_text("宿主日志 一行\n" * 50, encoding="utf-8")
        destination = self.root / "diag-budget.zip"; destination.unlink(missing_ok=True); self.c.r["path"] = str(destination)
        with mock.patch.object(library, "DIAGNOSTIC_ZIP_BUDGET", 600 << 10):
            library.diagnostics(self.c)
        self.assertLessEqual(destination.stat().st_size, (600 << 10) + (64 << 10))   # 预算 + manifest / zip 目录
        with zipfile.ZipFile(destination) as archive:
            pso = sorted(n for n in archive.namelist() if n.startswith("pso/"))
            self.assertEqual(pso, [f"pso/{games[0]}.tsv"])
            self.assertIn("logs/budget-host.log", archive.namelist())
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["budget"]["skipped"], [f"pso/{games[1]}.tsv", f"pso/{games[2]}.tsv"])
            self.assertEqual(manifest["budget"]["limit"], 600 << 10)

    def test_diagnostics_zip_budget_absent_when_everything_fits(self):
        # 预算放得下时 manifest 不出现 budget（现有 manifest 形状不变）
        import zipfile
        destination = self.root / "diag-budget-fits.zip"; destination.unlink(missing_ok=True); self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            self.assertNotIn("budget", json.loads(archive.read("manifest.json")))

    def test_diagnostics_carries_shader_salvage(self):
        # 着色器补料（dsh_salvage.inc）：各款 temp/<号>/wire/<号>/shader_miss/ 的 index.tsv 与成对的 <键>.ctl / .code 原样进 shader_miss/<号>/；
        #   不认的文件名、缺 .code 的半对、链接不放；每款超上限时索引先放、成对放不下整对不放。期望按规则手写
        import zipfile
        A, B = "0100AAAA00000000", "0100BBBB00000000"
        atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id=g, name=g) for g in (A, B)]))
        def put(g, name, data):
            f = self.c.library / "temp" / g / "wire" / g / "shader_miss" / name; f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(data); return f
        put(A, "index.tsv", b"# key\n1111111111111111\t2\t1\t1\t4\t3\t-\t-\n")
        put(A, "1111111111111111.ctl", b"CTL1"); put(A, "1111111111111111.code", b"COD")
        put(A, "2222222222222222.ctl", b"CTL2")   # 半对：缺 .code
        put(A, "notes.txt", b"x")                  # 不认的名字
        (self.c.library / "temp" / A / "wire" / A / "shader_miss" / "3333333333333333.ctl").symlink_to("/etc/hosts")
        put(A, "3333333333333333.code", b"C3")
        # B：超每款上限——索引放，4444（小）放，5555（大）整对不放
        put(B, "index.tsv", b"# key\n")
        put(B, "4444444444444444.ctl", b"a"); put(B, "4444444444444444.code", b"b")
        put(B, "5555555555555555.ctl", b"c"); put(B, "5555555555555555.code", b"d" * library.DIAG_SALVAGE_GAME_LIMIT)
        destination = self.root / "diag-salvage.zip"; destination.unlink(missing_ok=True); self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        import io, tarfile
        with zipfile.ZipFile(destination) as archive:
            names = sorted(n for n in archive.namelist() if n.startswith("shader_miss/"))
            self.assertEqual(names, [f"shader_miss/{A}.tar", f"shader_miss/{B}.tar"])   # 每款一个成员（诊断包成员数有上限）
            def members(g):
                with tarfile.open(fileobj=io.BytesIO(archive.read(f"shader_miss/{g}.tar"))) as t:
                    return {m.name: t.extractfile(m).read() for m in t.getmembers()}
            a = members(A)
            self.assertEqual(sorted(a), ["1111111111111111.code", "1111111111111111.ctl", "index.tsv"])
            self.assertEqual((a["1111111111111111.ctl"], a["1111111111111111.code"]), (b"CTL1", b"COD"))
            self.assertEqual(sorted(members(B)), ["4444444444444444.code", "4444444444444444.ctl", "index.tsv"])

    def test_diagnostics_player_note(self):
        # 故障报修的可选文本框（11-13 维护者）：请求带 note 就原样（去首尾空白）进 manifest.json 的 note，收件服务与 iOS 同字段；不填 / 空白不写
        import zipfile
        for given, expect in (("  进游戏第二关就黑屏 C:\\Users\\玩家  ", "进游戏第二关就黑屏 C:\\Users\\玩家"), ("   ", None), (None, None)):
            destination = self.root / "diag-note.zip"; destination.unlink(missing_ok=True); self.c.r["path"] = str(destination)
            self.c.r.pop("note", None)
            if given is not None:
                self.c.r["note"] = given
            library.diagnostics(self.c)
            with zipfile.ZipFile(destination) as archive:
                self.assertEqual(json.loads(archive.read("manifest.json")).get("note"), expect)

    def test_diagnostics_names_the_game(self):
        # 11-14 诊断#e68047：Mac / Windows 诊断包 manifest 没有 game.title_id，报告程序号成了「-」，不同游戏的停帧被并成一单。
        #   只认导出请求带的游戏号（启动器「故障报修」传玩家选中的那款）；没带就不写——任务记录里最近玩的那款也不拿来猜
        import zipfile
        jobs = self.c.jobpath.parent; jobs.mkdir(parents=True, exist_ok=True)
        (jobs / "mid.json").write_text(json.dumps({"id": "mid", "operation": "play", "state": "completed", "gameId": "0100F2C0115B6000"}), encoding="utf-8")
        def game_of():
            destination = self.root / "diag-game.zip"; destination.unlink(missing_ok=True); self.c.r["path"] = str(destination)
            library.diagnostics(self.c)
            with zipfile.ZipFile(destination) as archive:
                return json.loads(archive.read("manifest.json")).get("game")
        for given, expect in ((None, None), ("", None), ("not-a-title", None), ("0400a3a01ee4c000", {"title_id": "0400A3A01EE4C000"})):
            self.c.r.pop("gameId", None)
            if given is not None:
                self.c.r["gameId"] = given
            self.assertEqual(game_of(), expect, given)
        self.c.r.pop("gameId", None)

    def test_diagnostics_export_to_volume_whose_new_files_change_inode(self):
        # exFAT / FAT（macOS 驱动）：新文件写入数据、分到存储后 inode 号会变（本机 exFAT 镜像实测：mkstemp 后 18446744073709404714，写完 32987）。
        #   模拟这条规则：空文件的 fstat 给一个临时号，有数据后给真号。期望导出照常成功、诊断包在所选位置；
        #   修前身份在建文件时取（空文件，临时号），收尾比对必然不符，报「诊断临时文件在导出期间发生变化」
        import zipfile
        real = os.fstat
        def exfat_fstat(fd):
            st = real(fd)
            if not stat.S_ISREG(st.st_mode) or st.st_size:
                return st
            v = list(st); v[stat.ST_INO] = st.st_ino ^ (1 << 40)
            return os.stat_result(v)
        destination = self.root / "diag-exfat.zip"; self.c.r["path"] = str(destination)
        with unittest.mock.patch("os.fstat", side_effect=exfat_fstat):
            library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            self.assertIn("manifest.json", archive.namelist())

    def test_direct_check_cli_never_blocks(self):
        # 不许堵门（11-14，诊断 D-QDK9）：开工前空间预估不够只提示、不拦。期望手写：预估不够 → 退 0、打一行【黄】带原句与「照常开编」；
        #   够 → 退 0、不打字；复核自己抛异常 → 退 2（direct_lib.sh 记黄照编）
        import space_rule, contextlib, io
        for why, rc, want in (("磁盘空间不足：直连库预计还要 15.0 GB", 0, "【黄】磁盘空间不足：直连库预计还要 15.0 GB：这是预估，照常开编"), (None, 0, "")):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                got = space_rule.cli(["direct-check", "r", "c", "o"], check=lambda *a, w=why: w)
            self.assertEqual(got, rc); self.assertTrue(out.getvalue().startswith(want) if want else out.getvalue() == "", out.getvalue())
        def boom(*a): raise OSError("x")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(space_rule.cli(["direct-check", "r", "c", "o"], check=boom), 2)

    def test_direct_check_counts_existing_cache_and_volume(self):
        # 直连阶段开工前复核（pack/jobs/space_rule.py direct_check）与出包预检同一规则：游戏内容（romfs + exefs + aoc）≥ 3e9 → 15e9、小于 → 5e9；
        #   已有的同款直连缓存扣掉；期望按 library.py 的两档常量手算
        import collections
        import space_rule
        du = collections.namedtuple("du", "total used free")
        d = self.root / "dc"; root, cache, out = d / "root", d / "cache", d / "out"
        for x in (root / "romfs", root / "exefs", root / "aoc", cache, out): x.mkdir(parents=True)
        sizes = {str(root / "romfs"): int(2.5e9), str(root / "romfs.std"): 0, str(root / "exefs"): int(0.4e9), str(root / "aoc"): int(0.2e9), str(cache): 0}   # 内容 3.1e9 → 大档（romfs.std：镜像形态的 RomFS，这里是目录形态 = 0）
        with unittest.mock.patch("space_rule.tree_bytes", side_effect=lambda p: sizes[str(p)]):
            self.assertIsNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(15e9))))
            why = space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(15e9) - 1))
            self.assertIn("磁盘空间不足", why)   # pack/jobs/errors.py 据此归 disk_full
            self.assertIn("游戏内容 3.10 GB", why)
            sizes[str(cache)] = int(4e9)
            self.assertIsNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(11e9))))
            self.assertIsNotNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(11e9) - 1)))
            sizes[str(root / "aoc")] = 0; sizes[str(cache)] = 0   # 内容 2.9e9 → 小档 5e9
            self.assertIsNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(5e9))))
            self.assertIsNotNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(5e9) - 1)))
            # 单测注入余量 S2A_DISK_FREE_G（GB）：给了就不看本机实况（直接真跑 direct_lib.sh 的单测靠它，11-09 列车假红）
            with unittest.mock.patch.dict(os.environ, S2A_DISK_FREE_G="4.9"):
                self.assertIn("这块盘只剩 4.9 GB", space_rule.direct_check(root, cache, out))

            with unittest.mock.patch.dict(os.environ, S2A_DISK_FREE_G="999"):
                self.assertIsNone(space_rule.direct_check(root, cache, out))
        self.assertIs(library.pack_direct_bytes, space_rule.pack_direct_bytes)   # 预检与开工前复核是同一个函数（剃刀 H11：只在 space_rule 定义一份）
    def test_direct_check_shader_bound(self):
        # 开工前复核按着色器数取大（11-11：0400A3A01EE4C000 原件 6.7 GB、99084 个着色器，复核后直连段约 41 GiB，原件那一档只记 15 GB）。
        #   期望手算：99084 × 512 KiB = 51,948,552,192 字节 > 15e9 → 取它；1000 个 × 512 KiB = 524,288,000 < 5e9 → 取小档 5e9。
        #   着色器数 = 抽取表第 4 列唯一键：注释行、表头行不算，同一键出现两次算一个
        import collections
        import space_rule
        du = collections.namedtuple("du", "total used free")
        def make(name, keys, dup):
            d = self.root / name; root, cache, out = d / "root", d / "cache", d / "out"
            for x in (root / "romfs", root / "exefs", root / "aoc", cache, out, root / "extract" / "split"): x.mkdir(parents=True)
            rows = ["# kind: evidence", "package\tindex\tstage\tdm_ctl_sha16\tsource"] + [f"p.bin\t{i}\tpixel\t{i:016x}\t/s#{i}" for i in range(keys)] + [f"q.bin\t0\tvertex\t{0:016x}\t/dup"] * dup
            (root / "extract" / "split" / "shader_map.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
            return root, cache, out
        root, cache, out = make("sb_big", 99084, 3)
        sizes = {str(root / "romfs"): int(6.0e9), str(root / "romfs.std"): 0, str(root / "exefs"): int(0.5e9), str(root / "aoc"): int(0.2e9), str(cache): 0}
        self.assertEqual(space_rule.shader_count(root), 99084)
        with unittest.mock.patch("space_rule.tree_bytes", side_effect=lambda p: sizes[str(p)]):
            self.assertIsNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, 51948552192)))
            why = space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, 51948552191))
            self.assertIn("磁盘空间不足", why); self.assertIn("着色器 99084 个", why)
            self.assertIsNotNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(15e9))))   # 修前按原件那一档 15e9 就放行了
        root, cache, out = make("sb_small", 1000, 0)
        sizes = {str(root / "romfs"): int(0.3e9), str(root / "romfs.std"): 0, str(root / "exefs"): int(0.1e9), str(root / "aoc"): 0, str(cache): 0}
        with unittest.mock.patch("space_rule.tree_bytes", side_effect=lambda p: sizes[str(p)]):
            self.assertIsNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(5e9))))
            self.assertIsNotNone(space_rule.direct_check(root, cache, out, free=lambda v: du(0, 0, int(5e9) - 1)))
        self.assertEqual(space_rule.shader_count(self.root / "no_such_root"), 0)

    def test_space_budget_uses_unpacked_bytes_for_compressed_packages(self):
        # nsz / xcz（10-26）：文件比解出来小得多，按 inspect 的 contentBytes（清单里原始 NCA 大小之和）与文件字节的大值算；没有 contentBytes 的老记录照旧按文件字节
        import collections
        import unittest.mock as um
        du = collections.namedtuple("du", "total used free")
        for label, pkg, want in (("nsz", dict(size=1 << 30, contentBytes=6 << 30), 6 << 30), ("old record", dict(size=6 << 30), 6 << 30),
                                 ("nsp", dict(size=6 << 30, contentBytes=5 << 30), 6 << 30)):
            with self.subTest(label=label):
                game = self.game(packages=[dict(self.package("base"), **pkg)])
                with um.patch("shutil.disk_usage", return_value=du(0, 0, 50 << 30)):
                    budget = library.space_budget(self.c, game)
                self.assertEqual(budget["gameBytes"], want)
                self.assertEqual(budget["volumes"][0]["needBytes"], want * 11 // 10 + 15_000_000_000)

    def test_space_budget_library_volume_only(self):
        # 11-02 起打 iPhone 包不再把原件暂存到数据目录（共用 游戏库/cache/<程序号>/extract），预算只有游戏库一卷：原件跨卷与否都一样
        # 预算明细：游戏库一条（经验门槛）；未量化开销单列；事件里不出现绝对路径
        import collections
        import unittest.mock as um
        base = dict(self.package("base"), size=6 << 30)
        source = self.c.library / base["path"]; source.write_bytes(b"x" * 1000)
        game = self.game(packages=[base])
        du = collections.namedtuple("du", "total used free")
        real_stat = Path.stat
        for label, src_dev, expect_staging in (("same volume", 9, False), ("cross volume", 1, True)):
            with self.subTest(label=label):
                def fake_stat(path, *args, **kwargs):
                    info = real_stat(path, *args, **kwargs)
                    device = {source: src_dev, self.c.private: 9}.get(path)
                    if device is None:
                        return info
                    values = list(info); values[2] = device
                    return os.stat_result(values)
                def usage(path):
                    return du(0, 0, 5000 if Path(path) == self.c.private else 50 << 30)
                with um.patch.object(Path, "stat", fake_stat), um.patch("shutil.disk_usage", side_effect=usage):
                    budget = library.space_budget(self.c, game)
                    library.check_pack_space(self.c, game)
                lib = budget["volumes"][0]
                self.assertEqual((lib["role"], lib["basis"], lib["needBytes"], lib["freeBytes"], lib["enough"], lib["volume"]),
                                 ("library", "rule", (6 << 30) * 11 // 10 + 15_000_000_000, 50 << 30, True, "系统盘"))
                self.assertIn("直连着色器上限", lib["note"]); self.assertEqual(lib["unknown"], ["shader_cache"])
                self.assertEqual(budget["gameBytes"], 6 << 30)
                self.assertEqual([u["id"] for u in budget["unknown"]], ["shader_cache"])
                self.assertIn("未量化", budget["unknown"][0]["note"])
                self.assertEqual(len(budget["volumes"]), 1)

    def test_space_shortfall_emits_budget_warns_and_persists_in_job(self):
        # 游戏库不够：发 space 事件（enough=false）并记一条 disk_full 警告、不拦（11-14 不许堵门）；后续阶段事件不冲掉任务文件里的 space
        import collections
        import unittest.mock as um
        base = dict(self.package("base"), size=1 << 30)
        game = self.game(packages=[base])
        du = collections.namedtuple("du", "total used free")
        with um.patch("shutil.disk_usage", return_value=du(0, 0, 5 << 30)):
            library.check_pack_space(self.c, game)
        [d] = [d for d in self.c.job["diagnostics"] if d["code"] == "disk_full"]
        self.assertEqual(d["severity"], "warning"); self.assertIn("磁盘空间不够", d["summary"])
        event = self.space_events()[-1]
        self.assertFalse(event["space"]["volumes"][0]["enough"])
        self.assertEqual(event["space"]["volumes"][0]["needBytes"], (1 << 30) * 11 // 10 + 5_000_000_000)
        self.c.begin("下一阶段", 2, 3); self.c.emit(done=1, total=2)
        job = json.loads(self.c.jobpath.read_text())
        self.assertEqual(job["space"], event["space"]); self.assertEqual(job["message"], "下一阶段")

    def test_mac_prepare_space_estimate_does_not_stop(self):
        # 11-14 不许堵门：Mac 打包预估空间不够不再在开头拦，记一条盘满警告、照常往下走（这里让往下第一步抛个记号，证明走到了）
        self.player_keys()   # 加密原件出包开头要 prod.keys（library.require_game，v0.3.1）
        import collections
        import unittest.mock as um
        base = dict(self.package("base"), size=1 << 30)
        game = self.game(packages=[base])
        self.catalogue([game])
        image = layout.image_mac(self.c, game["id"])
        image.mkdir(parents=True); (image / "runtime").write_text("keep")
        self.c.r.update(gameId=game["id"])
        du = collections.namedtuple("du", "total used free")
        class Reached(Exception): pass
        with um.patch("shutil.disk_usage", return_value=du(0, 0, 1)), um.patch.object(self.c, "run"), \
             um.patch.object(self.c, "fonts_dir", side_effect=Reached):   # 空间预检之后的下一步就是取共享字体
            with self.assertRaises(Reached): build.prepare(self.c)   # 过了空间预检，走到了下一步
        warn = [d for d in self.c.job["diagnostics"] if d["code"] == "disk_full"]
        self.assertEqual([d["severity"] for d in warn], ["warning"]); self.assertIn("这是预估，照常开工", warn[0]["summary"])
        self.assertEqual((image / "runtime").read_text(), "keep")   # 现有镜像不动
        types = [json.loads(l)["type"] for l in self.c.events.read_text().splitlines()]
        self.assertIn("space", types)

    def test_library_on_unplugged_disk_says_plug_it_in(self):
        # 游戏库在没插的外接盘上：报「磁盘没连上」并写出盘名；本机磁盘上标识不符仍是原提示
        from pathlib import Path as P
        self.c.r["libraryId"] = "x"
        self.c.library = P("/Volumes/Synthetic/游戏库")
        with self.assertRaises(Failure) as e: self.c.require_library()
        self.assertIn("「Synthetic」没连上", str(e.exception))
        self.c.library = self.root / "lib-elsewhere"
        with self.assertRaises(Failure) as e: self.c.require_library()
        self.assertIn("标识不符", str(e.exception))

    def test_detect_xcode_picks_newest_complete_xcode(self):
        # 与前端 Store.detectXcode 同一规则：只认 com.apple.dt.Xcode 且有 usr/bin/xcodebuild 的；版本按数字比（27.1 > 27.0 > 26.6，10 > 9）
        import plistlib, toolchain, unittest.mock as um
        apps = self.root / "Applications"
        def mk(name, version, bid="com.apple.dt.Xcode", complete=True):
            app = apps / name; (app / "Contents/Developer/usr/bin").mkdir(parents=True)
            (app / "Contents/Info.plist").write_bytes(plistlib.dumps(dict(CFBundleIdentifier=bid, CFBundleShortVersionString=version)))
            if complete: (app / "Contents/Developer/usr/bin/xcodebuild").write_text("")
        mk("Xcode.app", "26.6"); mk("Xcode-27.app", "27.0"); mk("Xcode-27.1.app", "27.1")
        mk("Xcode-beta.app", "28.0", complete=False); mk("XcodeTool.app", "99", bid="com.example.other")
        with um.patch.object(toolchain.subprocess, "run", side_effect=OSError):
            self.assertEqual(toolchain.detect_xcode((str(apps),)), str(apps / "Xcode-27.1.app/Contents/Developer"))
            self.assertIsNone(toolchain.detect_xcode((str(self.root / "none"),)))

    def test_detect_xcode_finds_xcode_on_external_volume(self):
        # 玩家把 Xcode 装在扩展硬盘上（D-9WFW）：外接卷的 Applications 与卷根都看；/Volumes 里指回 / 的启动盘软链跳过
        import plistlib, toolchain, unittest.mock as um
        vols = self.root / "Volumes"
        def mk(app, version):
            (app / "Contents/Developer/usr/bin").mkdir(parents=True)
            (app / "Contents/Info.plist").write_bytes(plistlib.dumps(dict(CFBundleIdentifier="com.apple.dt.Xcode", CFBundleShortVersionString=version)))
            (app / "Contents/Developer/usr/bin/xcodebuild").write_text("")
        mk(vols / "Ext SSD" / "Applications" / "Xcode.app", "27.1")
        mk(vols / "Other" / "Xcode-26.app", "26.6")   # 直接放在卷根
        boot = self.root / "boot"; (boot / "Applications").mkdir(parents=True)
        (vols / "Macintosh HD").symlink_to(boot)
        dirs = toolchain.default_app_dirs(str(vols))
        self.assertEqual(dirs[2:], (str(vols / "Ext SSD" / "Applications"), str(vols / "Ext SSD"), str(vols / "Other" / "Applications"), str(vols / "Other")))
        with um.patch.object(toolchain.subprocess, "run", side_effect=OSError):
            ext = dirs[2:]   # 只看外接卷那几项：本机真 /Applications 里的 Xcode 不进期望
            self.assertEqual(toolchain.detect_xcode(ext), str(vols / "Ext SSD" / "Applications" / "Xcode.app/Contents/Developer"))
            self.assertEqual([x[1] for x in toolchain.installed_xcodes(ext)], ["27.1", "26.6"])

    def test_installed_xcodes_by_version_and_build_not_path(self):
        # 11-07 gate-host 现场：Xcode-27.app 被 App Store 原地升到 27.1（构建号 27A9275），Xcode-27.1.app 是指向它的软链 → 只算一套；
        #   改名拷贝（同版本同构建号）也只算一套；同版本不同构建号（beta 与正式）算两套；路径名写 27 不影响读出的 27.1
        import plistlib, os, toolchain, unittest.mock as um
        apps = self.root / "Apps"
        def mk(name, version, build):
            app = apps / name; (app / "Contents/Developer/usr/bin").mkdir(parents=True)
            (app / "Contents/Info.plist").write_bytes(plistlib.dumps(dict(CFBundleIdentifier="com.apple.dt.Xcode", CFBundleShortVersionString=version)))
            (app / "Contents/version.plist").write_bytes(plistlib.dumps(dict(ProductBuildVersion=build)))
            (app / "Contents/Developer/usr/bin/xcodebuild").write_text("")
        mk("Xcode-26.6.app", "26.6", "17F113"); mk("Xcode-27.app", "27.1", "27A9275")
        os.symlink(apps / "Xcode-27.app", apps / "Xcode-27.1.app")
        mk("Xcode-copy.app", "27.1", "27A9275"); mk("Xcode-27.1-rc.app", "27.1", "27A9270")
        with um.patch.object(toolchain.subprocess, "run", side_effect=OSError):
            got = [(vs, b) for _, vs, b, _ in toolchain.installed_xcodes((str(apps),))]
        self.assertEqual(sorted(got), sorted([("27.1", "27A9275"), ("27.1", "27A9270"), ("26.6", "17F113")]))
        self.assertEqual(got[-1], ("26.6", "17F113"))   # 版本新的在前

    def test_fonts_dir_prefers_player_then_bundled(self):
        # 共享字体：玩家选的目录（含 .bfttf）优先；没选或选的不含 .bfttf → 应用自带 Resources/Fonts；都没有 → None
        res = self.root / "res"; (res / "Fonts").mkdir(parents=True); (res / "Fonts" / "a.bfttf").write_bytes(b"x")
        mine = self.root / "mine"; mine.mkdir(); empty = self.root / "empty"; empty.mkdir()
        self.c.r.update(resources=str(res))
        self.assertEqual(self.c.fonts_dir(), (res / "Fonts").resolve())
        self.c.r.update(fontsDir=str(empty))
        self.assertEqual(self.c.fonts_dir(), (res / "Fonts").resolve())
        (mine / "b.bfttf").write_bytes(b"y"); self.c.r.update(fontsDir=str(mine))
        self.assertEqual(self.c.fonts_dir(), mine.resolve())
        self.c.r.update(fontsDir="", resources=str(self.root / "nores"))
        self.assertIsNone(self.c.fonts_dir())

    def test_library_identity_reopen_and_offline_drive(self):
        self.assertEqual(library.setup(self.c)["libraryId"], self.c.r["libraryId"])
        marker = self.c.library / ".s2arm-library.json"
        marker.unlink()
        with self.assertRaises(Failure):
            self.c.catalog()
        self.assertFalse(marker.exists())

    def test_caches_follow_a_library_outside_the_private_root(self):
        """外接盘上的游戏库：工具缓存和运行目录跟着库走（默认位置的 tool-cache/，游戏库结构 P3），不写系统盘；库在数据目录里也一样；
        库还没建好（没有库标识）时退回 <私有目录>/tool-cache。"""
        outside = self.root / "external" / "lib"
        c = Context(self.request | dict(taskId="outside", library=str(outside)))
        self.assertEqual(c.work_root, c.private / "tool-cache")
        atomic(outside / ".s2arm-library.json", dict(schema=1, id="x"))
        self.assertEqual(c.work_root, outside.resolve() / "tool-cache")
        env = c.env()
        self.assertEqual(env["S2A_RUN_ROOT"], str(outside.resolve() / "tool-cache/run"))
        self.assertTrue(env["GAL_PSO_CACHE"].startswith(str(outside.resolve())))
        lib = Path(self.request["privateRoot"]) / "lib"
        atomic(lib / ".s2arm-library.json", dict(schema=1, id="y"))
        inside = Context(self.request | dict(taskId="inside", library=str(lib)))
        self.assertEqual(inside.work_root, lib.resolve() / "tool-cache")
        self.assertEqual(inside.env()["S2A_RUN_ROOT"], str(lib.resolve() / "tool-cache" / "run"))

    def test_refuses_nonempty_or_legacy_library(self):
        other = self.root / "legacy"
        other.mkdir()
        (other / ".nsmac-library.json").write_text("original")
        c = Context(self.request | dict(taskId="legacy", library=str(other)))
        with self.assertRaises(Failure):
            library.setup(c)
        self.assertEqual((other / ".nsmac-library.json").read_text(), "original")

    def test_export_checks_keys_at_start_not_in_catalog(self):
        # v0.3.1：前端不再按 keysReady 置灰出包，出包开头（require_game）当场核：加密原件、没有 prod.keys → keys_missing 停；
        #   导入 keys 后同一款照常过这一关；游戏库读数里的「缺文件」（requirements）不含 keys 这一条
        game = self.game(packages=[self.package()])
        self.c.keys.unlink(missing_ok=True)
        self.assertEqual(library.requirements(self.c, game), [])
        with self.assertRaises(Failure) as got:
            library.require_game(self.c, game)
        self.assertEqual(got.exception.code, "keys_missing")
        self.c.keys.parent.mkdir(parents=True, exist_ok=True); self.c.keys.write_text("dummy = 00\n")
        library.require_game(self.c, game)

    def test_missing_base_and_aarch32_are_not_buildable(self):
        update = self.package("update", 2)
        self.assertTrue(any("缺游戏本体" in m for m in library.requirements(self.c, self.game(packages=[update]))))
        base = self.package()
        base["guestIsa"] = "aarch32"
        self.assertTrue(any("32 位" in x for x in library.requirements(self.c, self.game(packages=[base]))))
        # 本体 32 位、最新更新 64 位（0100152000022000 1.0 → 4.0.1）：按更新判 → 能打包；更新读不出指令集（unknown）→ 退回本体 → 不能打包
        update64 = dict(self.package("update", 2), guestIsa="aarch64")
        self.assertFalse(any("32 位" in x for x in library.requirements(self.c, self.game(packages=[base, update64]))))
        update_unknown = dict(self.package("update", 2), guestIsa="unknown")
        self.assertTrue(any("32 位" in x for x in library.requirements(self.c, self.game(packages=[base, update_unknown]))))

    def test_newest_update_and_each_dlc_are_frozen(self):
        packages = [self.package(), self.package("update", 1), self.package("update", 2),
                    self.package("dlc", 1, "0100000000001001"), self.package("dlc", 4, "0100000000001001")]
        game = self.game(packages=packages)
        self.assertEqual([p["version"] for p in library.selected_packages(game)], [0, 2, 4])
        identity = library.input_identity(game)
        self.assertEqual(identity, library.input_identity(self.game(packages=list(reversed(packages)))))
        packages[2]["sha256"] = "b" * 64
        self.assertNotEqual(identity, library.input_identity(game))

    def test_duplicate_same_version_prefers_source_that_still_exists(self):
        old_base = self.package(sha="a" * 64)
        old_dlc = self.package("dlc", tid="0100000000001001", sha="b" * 64)
        new_base = self.package(sha="c" * 64)
        new_dlc = self.package("dlc", tid="0100000000001001", sha="d" * 64)
        update = self.package("update", 1, sha="e" * 64)
        for stale in (old_base, old_dlc):
            stale["path"] = str(self.root / "moved-away.xci")

        selected = library.selected_packages(self.game(packages=[old_base, old_dlc, new_base, new_dlc, update]))
        self.assertEqual([p["sha256"] for p in selected], ["c" * 64, "e" * 64, "d" * 64])
        self.assertEqual(library.requirements(self.c, self.game(packages=[old_base, old_dlc, new_base, new_dlc, update])), [])

    def test_catalog_marks_games_recorded_on_a_phone(self):
        # 删除确认框据 onPhone 多写一句「下次往这台手机装游戏时，手机上的这款会打不开」：哪台设备记录里有这款就是 True，没有的 False
        on, off = self.game(tid="0100000000AA0000", packages=[self.package(tid="0100000000AA0000")]), self.game(tid="0100000000BB0000", packages=[self.package(tid="0100000000BB0000")])
        self.catalogue([on, off])
        layout.save_device(self.c, "dev1", dict(games={on["id"].lower(): dict(pack="p")}))   # 记录里的游戏号大小写不一也认
        got = {g["id"]: g["onPhone"] for g in library.catalog(self.c)["games"]}
        self.assertEqual(got, {on["id"]: True, off["id"]: False})

    def test_catalog_counts_phone_devices(self):
        # 启动器更新弹窗按 phoneDevices 写成「更新启动器和 iPhone App」：设备记录里有款的才算；没装过 → 0，空记录 → 不算
        g = self.game(tid="0100000000AA0000", packages=[self.package(tid="0100000000AA0000")])
        self.catalogue([g])
        self.assertEqual(library.catalog(self.c)["phoneDevices"], 0)
        layout.save_device(self.c, "dev1", dict(games={g["id"]: dict(pack="p")}))
        layout.save_device(self.c, "dev2", dict(games={}))
        self.assertEqual(library.catalog(self.c)["phoneDevices"], 1)

    def test_catalogue_keeps_unsupported_and_unverified_distinct(self):
        game = self.game(packages=[self.package()])
        self.catalogue([game])
        output = library.catalog(self.c)
        self.assertEqual(output["games"][0]["compatibility"], "experimental")
        self.assertFalse(output["games"][0]["ready"])
        self.assertTrue(output["games"][0]["needsPreparation"])

    def test_catalogue_lists_job_records_not_retained_requests(self):
        # 任务目录里同一任务有记录 <id>.json 和留存的请求 <id>.request.json（J10 resume）；catalog 只该返回记录。
        #   请求混进来时启动器把整个任务列表解码失败（10-25 main 上 BudgetTests 重开读回 space 为 nil）
        self.catalogue([self.game(packages=[self.package()])])
        jobs = self.c.jobpath.parent   # 这个作业自己的任务记录目录（core.logs_root）
        jobs.mkdir(parents=True, exist_ok=True)
        (jobs / "older.json").write_text(json.dumps({"id": "older", "operation": "build", "state": "failed"}), encoding="utf-8")
        (jobs / "older.request.json").write_text(json.dumps({"schema": 1, "taskId": "older", "operation": "build"}), encoding="utf-8")
        output = library.catalog(self.c)
        self.assertEqual([j.get("id") for j in output["jobs"]], ["older"])
        self.assertTrue(all("state" in j for j in output["jobs"]))

    def test_catalogue_skips_exfat_sidecars_and_unreadable_job_records(self):
        # #1010：数据目录在 exFAT 上搬过，每条记录旁多一份 ._<名>.json（AppleDouble，头 00 05 16 07，非 UTF-8）；
        #   原先 read() 抛 UnicodeDecodeError，整个游戏库刷新失败。附属文件跳过；正常名字但读不出的那一条也只跳过它自己
        self.catalogue([self.game(packages=[self.package()])])
        jobs = self.c.jobpath.parent
        jobs.mkdir(parents=True, exist_ok=True)
        (jobs / "older.json").write_text(json.dumps({"id": "older", "operation": "build", "state": "failed"}), encoding="utf-8")
        junk = b"\x00\x05\x16\x07\x00\x02\x00\x00Mac OS X        \x81\xb0\x8d\x90"
        (jobs / "._older.json").write_bytes(junk)
        (jobs / "broken.json").write_bytes(junk)
        output = library.catalog(self.c)
        self.assertEqual([j.get("id") for j in output["jobs"]], ["older"])

    def test_catalogue_returns_only_newest_job_records(self):
        # 11-07：任务目录 1.5 万条记录时 catalog 整份读 + 回传 7 MB；只回最新 JOB_LIST_LIMIT 条（按修改时间）
        self.catalogue([self.game(packages=[self.package()])])
        jobs = self.c.jobpath.parent   # 这个作业自己的任务记录目录（core.logs_root）
        jobs.mkdir(parents=True, exist_ok=True)
        now = time.time()
        for i in range(library.JOB_LIST_LIMIT + 5):
            f = jobs / f"t{i:04d}.json"
            f.write_text(json.dumps({"id": f"t{i:04d}", "operation": "catalog", "state": "completed", "ended": now}), encoding="utf-8")
            os.utime(f, (now - 1000 + i, now - 1000 + i))   # 编号越大越新
        output = library.catalog(self.c)
        ids = [j["id"] for j in output["jobs"]]
        self.assertEqual(len(ids), library.JOB_LIST_LIMIT)
        self.assertEqual(ids[0], f"t{library.JOB_LIST_LIMIT + 4:04d}")
        self.assertNotIn("t0000", ids)

    def test_game_data_cannot_escape_library(self):
        p = self.package()
        p["path"] = "../../outside.nsp"
        with self.assertRaises(Failure):
            library.requirements(self.c, self.game(packages=[p]))

    def test_runtime_environment_maps_language_and_never_reseeds_saves(self):
        self.c.r.update(gameLanguage="zh-Hant", docked=True)
        os.environ["S2A_SAVE_DIR"] = "/do-not-use"
        os.environ["S2A_AUTOPLAY"] = "1"
        os.environ["S2A_ROUTE"] = "/do-not-use"
        try:
            game = self.game()
            env = build.runtime_env(self.c, game, self.root / "runtime")
        finally:
            for name in ("S2A_SAVE_DIR", "S2A_AUTOPLAY", "S2A_ROUTE"):
                os.environ.pop(name)
        self.assertEqual(env["S2A_SYSTEM_LANGUAGE"], "TraditionalChinese")
        self.assertEqual(env["S2A_DOCKED"], "1")   # 请求明给 docked=True
        self.assertEqual(env["S2A_AUTOPLAY"], "0")
        self.assertNotIn("S2A_SAVE_DIR", env)
        self.assertNotIn("S2A_ROUTE", env)
        self.assertEqual(env["S2A_RYU_DATA"], str(layout.path(self.c, game["id"], "saves")))

    def test_runtime_environment_follows_system_language_and_docks_by_default(self):
        # 请求不带 gameLanguage 就不设 S2A_SYSTEM_LANGUAGE（继承来的也不带，c.env 去掉 S2A_*；宿主跟系统首选语言）；玩家在启动设置选的游戏语言走 community launch_env（11-14 维护者确认可选）；
        # 不带 docked 是主机模式，只有明说 false 才掌机
        os.environ["S2A_SYSTEM_LANGUAGE"] = "Japanese"
        try:
            env = build.runtime_env(self.c, self.game(), self.root / "runtime")
        finally:
            os.environ.pop("S2A_SYSTEM_LANGUAGE")
        self.assertNotIn("S2A_SYSTEM_LANGUAGE", env)
        self.assertEqual(env["S2A_DOCKED"], "0")   # 没选过：缺省掌机
        self.c.r["docked"] = False
        self.assertEqual(build.runtime_env(self.c, self.game(), self.root / "runtime")["S2A_DOCKED"], "0")
        self.c.r["docked"] = True   # 请求明给主机照给的
        self.assertEqual(build.runtime_env(self.c, self.game(), self.root / "runtime")["S2A_DOCKED"], "1")

    def test_invalid_language_is_rejected(self):
        self.c.r["gameLanguage"] = "zhHans; command"
        with self.assertRaises(Failure):
            build.runtime_env(self.c, self.game(), self.root)

    def test_launcher_decides_whether_the_game_may_auto_connect(self):
        self.c.r["switch2ProAutoConnect"] = True
        self.assertEqual(build.runtime_env(self.c, self.game(), self.root / "runtime")["S2A_NS2_AUTOCONNECT"], "1")
        self.c.r["switch2ProAutoConnect"] = False
        self.assertEqual(build.runtime_env(self.c, self.game(), self.root / "runtime")["S2A_NS2_AUTOCONNECT"], "0")
        self.c.r["switch2ProAutoConnect"] = "yes"
        with self.assertRaises(Failure):
            build.runtime_env(self.c, self.game(), self.root)
        self.c.r["switch2ProAutoConnect"] = True
        self.c.r["switch2ProRelaySocket"] = "/tmp/s2a-ns2-relay.sock"
        self.assertEqual(build.runtime_env(self.c, self.game(), self.root / "runtime")["S2A_NS2_SOCKET"],
                         "/tmp/s2a-ns2-relay.sock")
        self.c.r.pop("switch2ProRelaySocket")
        self.assertNotIn("S2A_NS2_SOCKET", build.runtime_env(self.c, self.game(), self.root / "runtime"))
        self.c.r["switch2ProRelaySocket"] = 1
        with self.assertRaises(Failure):
            build.runtime_env(self.c, self.game(), self.root)
        self.c.r.pop("switch2ProRelaySocket")
        self.c.r["xboxGipRelaySocket"] = "/tmp/s2a-xbox-gip.sock"
        self.assertEqual(build.runtime_env(self.c, self.game(), self.root / "runtime")["S2A_XBOX_GIP_SOCKET"],
                         "/tmp/s2a-xbox-gip.sock")
        self.c.r.pop("xboxGipRelaySocket")
        self.assertNotIn("S2A_XBOX_GIP_SOCKET", build.runtime_env(self.c, self.game(), self.root / "runtime"))
        self.c.r["xboxGipRelaySocket"] = 1
        with self.assertRaises(Failure):
            build.runtime_env(self.c, self.game(), self.root)
        self.c.r.pop("xboxGipRelaySocket")

    def test_launch_fullscreen_setting_gives_host_a_default_only(self):
        # #575：启动器「启动游戏时自动全屏」只给宿主全局缺省 S2A_FULLSCREEN_DEFAULT，不写 S2A_FULLSCREEN（那是单款显式设置，宿主优先认它）
        self.c.r["launchFullscreen"] = True
        env = build.runtime_env(self.c, self.game(), self.root / "runtime")
        self.assertEqual(env.get("S2A_FULLSCREEN_DEFAULT"), "1")
        self.assertNotIn("S2A_FULLSCREEN", env)
        for off in (False, None, "1"):
            self.c.r["launchFullscreen"] = off
            self.assertNotIn("S2A_FULLSCREEN_DEFAULT", build.runtime_env(self.c, self.game(), self.root / "runtime"))
        self.c.r.pop("launchFullscreen")
        self.assertNotIn("S2A_FULLSCREEN_DEFAULT", build.runtime_env(self.c, self.game(), self.root / "runtime"))

    def test_metal_hud_switch_and_explicit_environment_reach_real_child(self):
        names = ("MTL_HUD_ENABLED", "MTL_HUD_LOG_ENABLED", "MTL_HUD_LOG_SHADER_ENABLED")
        previous = {name: os.environ.get(name) for name in names}
        try:
            for name in names:
                os.environ.pop(name, None)
            for dev_hud, metal_hud, override, expected in (
                    (False, False, None, None), (True, False, None, None),
                    (False, True, None, "1"), (True, True, None, "1"),
                    (True, True, "0", "0"), (False, False, "1", "1")):
                self.c.r.update(devHud=dev_hud, metalHud=metal_hud)
                if override is None:
                    os.environ.pop("MTL_HUD_ENABLED", None)
                else:
                    os.environ["MTL_HUD_ENABLED"] = override
                env = build.runtime_env(self.c, self.game(), self.root / "runtime")
                lines = []
                self.c.run(["/usr/bin/env"], env=env, on_line=lines.append)
                child = dict(line.split("=", 1) for line in lines if "=" in line)
                self.assertEqual(child.get("MTL_HUD_ENABLED"), expected)
                self.assertEqual(child.get("S2A_HUD"), "1" if dev_hud else None)
                self.assertNotIn("MTL_HUD_LOG_ENABLED", child)
                self.assertNotIn("MTL_HUD_LOG_SHADER_ENABLED", child)
        finally:
            for name, value in previous.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value

    def test_mac_input_guide_freezes_the_confirmed_mapping_per_game_and_launch(self):
        def mapping(code):
            return dict(schema=1, keyboard={"A": {"code": code}}, mouse={"Y": 0}, gamepad={"A": "east"},
                        axes={}, deadzone=.15, mouseOptions=dict(mode="off", sensitivityX=1, sensitivityY=1))
        first = self.game(); second = self.game(tid="0100000000000001"); second["name"] = "Another game"
        defaults, saved, confirmed = mapping(14), mapping(36), mapping(3)
        atomic(self.c.private / "default-input.json", defaults)
        atomic(layout.path(self.c, first["id"], "input.json"), saved)
        before = build.runtime_env(self.c, first, self.root / "runtime")
        next_context = Context(dict(self.request, taskId=str(uuid.uuid4()), launchInputConfig=confirmed,
                                    inputGuideHandled=True, uiLanguage="en"))
        actual = build.runtime_env(next_context, first, self.root / "runtime")
        other_context = Context(dict(self.request, taskId=str(uuid.uuid4())))
        other = build.runtime_env(other_context, second, self.root / "runtime2")
        self.assertNotEqual(before["S2A_INPUT_CONFIG"], actual["S2A_INPUT_CONFIG"])
        self.assertNotEqual(actual["S2A_INPUT_CONFIG"], other["S2A_INPUT_CONFIG"])
        self.assertEqual(json.loads(Path(before["S2A_INPUT_CONFIG"]).read_text()), saved)
        self.assertEqual(json.loads(Path(actual["S2A_INPUT_CONFIG"]).read_text()), confirmed)
        self.assertEqual(json.loads(Path(other["S2A_INPUT_CONFIG"]).read_text()), defaults)
        self.assertEqual(actual["S2A_GAME_NAME"], first["name"])
        self.assertEqual(other["S2A_GAME_NAME"], second["name"])
        self.assertEqual(actual["S2A_GAME_ID"], first["id"])
        self.assertEqual(actual["S2A_INPUT_GUIDE_SHOWN"], "1")
        self.assertNotIn("S2A_INPUT_GUIDE_SHOWN", other)
        self.assertEqual(actual["S2A_UI_LANGUAGE"], "en")
        self.assertTrue(actual["S2A_INPUT_GUIDE_PREFS"].endswith(first["id"] + ".json"))
        self.assertNotEqual(actual["S2A_INPUT_GUIDE_PREFS"], other["S2A_INPUT_GUIDE_PREFS"])

    def test_mac_input_guide_malformed_confirmed_mapping_uses_default_keys(self):
        # v0.3.1：起局时带来的映射不合规 → 这局用默认键位，不挡起局
        for value in ({}, [], {"schema": 1}):
            self.c.r["launchInputConfig"] = value
            self.assertNotIn("S2A_INPUT_CONFIG", build.runtime_env(self.c, self.game(), self.root))

    def test_mac_runtime_env_passes_per_player_mappings(self):
        # 玩家 2–4 的键位按玩家保存（input-pN.json，键位设置里选玩家）：起局时照玩家 1 同一套校验后交给宿主
        #   （S2A_INPUT_CONFIG_P2..P4，front_mac.m 槽 i = 玩家 i+1 各照各的映射采样）；没存的那位不给，宿主照玩家 1 的键位
        def mapping(code):
            return dict(schema=1, keyboard={"A": {"code": code}}, mouse={"Y": 0}, gamepad={"A": "east"},
                        axes={}, deadzone=.15, mouseOptions=dict(mode="off", sensitivityX=1, sensitivityY=1))
        game = self.game()
        atomic(layout.path(self.c, game["id"], "input-p2.json"), mapping(36))
        atomic(layout.path(self.c, game["id"], "input-p4.json"), {"schema": 99})   # 存坏的映射：这位照玩家 1 的键位
        env = build.runtime_env(self.c, game, self.root / "runtime")
        self.assertEqual(json.loads(Path(env["S2A_INPUT_CONFIG_P2"]).read_text()), mapping(36))
        self.assertNotIn("S2A_INPUT_CONFIG_P3", env)
        self.assertNotIn("S2A_INPUT_CONFIG_P4", env)

    def test_launch_rejects_another_games_runtime_before_starting_process(self):
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        with patch.object(build, "runtime_manifest", return_value={"gameId": "0100000000000001"}), patch.object(self.c, "run") as run:
            with self.assertRaisesRegex(Failure, "不匹配"):
                build.launch(self.c)
            run.assert_not_called()

    def fake_host(self, gid):
        """公共宿主夹具：hosts/mac/<摘要>/S2ArmGame.app，game.json 记这款在用它（host_update.choose 开发模式下按 hostMac 取）"""
        exe = layout.hosts_mac(self.c) / "aaaaaaaaaaaa" / "S2ArmGame.app/Contents/MacOS/s2arm-host"
        exe.parent.mkdir(parents=True, exist_ok=True); exe.write_bytes(b"#!/bin/sh\n"); exe.chmod(0o755)
        layout.update_info(self.c, gid, hostMac="aaaaaaaaaaaa")
        return exe

    def test_launch_confirms_mapping_support_on_this_games_host(self):
        game = self.game(); self.catalogue([game]); self.c.r.update(gameId=game["id"], launchInputConfig={"schema": 1})
        host = self.fake_host(game["id"])
        with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), patch.object(build.toolchain, "input_interface", return_value=(False, "legacy")) as probe, patch.object(self.c, "run") as run:
            build.launch(self.c)   # v0.3.1：宿主不认映射 → 用默认键位照常起局（不再报「重新打包」挡住）
            probe.assert_called_once_with(self.c, host)   # 照旧查的是这款在用的宿主
            run.assert_called_once()
            self.assertNotIn("S2A_INPUT_CONFIG", run.call_args.args[2])

    def test_launch_returns_to_library_only_when_the_host_left_the_switch_game_marker(self):
        # 宿主菜单栏「切换游戏」写 S2A_RETURN_MARKER 再退出：结果带 returnToLibrary；正常退出不带；上一局残留的标记不算
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        self.fake_host(game["id"])
        stale = self.c.private / "runtime-return" / (self.c.task + ".flag")
        for switch in (True, False):
            stale.parent.mkdir(parents=True, exist_ok=True); stale.write_text("")
            seen = []
            def run(command, log, env, **kwargs):
                marker = Path(env["S2A_RETURN_MARKER"]); seen.append(marker)
                self.assertFalse(marker.exists())
                if switch:
                    marker.write_text("")
            with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
                 patch.object(self.c, "run", side_effect=run):
                result = build.launch(self.c)
            self.assertEqual(result.get("returnToLibrary", False), switch)
            self.assertEqual(seen, [stale])
            self.assertFalse(stale.exists())

    def test_launch_forgets_host_registrations_after_normal_exit_and_crash(self):
        # #1126：每局结束（正常退出、游戏崩溃都算）注销系统应用登记里的 S2ArmGame，启动台不再出灰色图标
        import host_update
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        host = self.fake_host(game["id"])
        for raised in (None, Failure("游戏运行中失败（退出码 139）：fixture", code="tool_exit")):
            with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
                 patch.object(self.c, "run", side_effect=raised), patch.object(host_update, "forget_registrations") as forget:
                if raised:
                    with self.assertRaises(Failure): build.launch(self.c)
                else:
                    build.launch(self.c)
            self.assertEqual(forget.call_count, 1)
            self.assertEqual(forget.call_args.kwargs["app"], host.parents[2], "交这局起的宿主包：不是真 App 包（没 Info.plist）就不碰系统登记")

    def test_game_quitting_mid_run_reports_game_crashed_with_exit_code(self):
        # 启动页说人话（11-11）：游戏进程自己非 0 退出（139 = 段错误）报 game_crashed、消息带退出码；别的失败照原样往上抛
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        self.fake_host(game["id"])
        for raised, want in ((Failure("游戏运行中失败（退出码 139）：fixture", code="tool_exit"), "game_crashed"),
                             (Failure("fixture other", code="permission_denied"), "permission_denied")):
            with self.subTest(code=raised.code):
                def run(command, log, env, **kwargs):
                    self.c.last_rc = 139
                    raise raised
                with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
                     patch.object(self.c, "run", side_effect=run):
                    with self.assertRaises(Failure) as e:
                        build.launch(self.c)
                self.assertEqual(e.exception.code, want)
                if want == "game_crashed":
                    self.assertIn("退出码 139", str(e.exception))
                    self.assertIn("fixture", e.exception.internal)   # 原文随错误摘要带走

    def test_gpu_safe_stop_exit_86_reports_game_gpu_stopped_not_done(self):
        # #1257：显卡挂死、运行时安全停（host_adapt.m s2a_gpu_safe_stop 设收局退出码 86）——报 game_gpu_stopped，不当玩完、也不说崩溃
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        self.fake_host(game["id"])
        def run(command, log, env, **kwargs):
            self.c.last_rc = 86
            raise Failure("游戏运行中失败（退出码 86）：fixture", code="tool_exit")
        with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
             patch.object(self.c, "run", side_effect=run):
            with self.assertRaises(Failure) as e:
                build.launch(self.c)
        self.assertEqual(e.exception.code, "game_gpu_stopped")
        self.assertIn("安全停下", str(e.exception))
        self.assertIn("fixture", e.exception.internal)

    def test_launch_checks_no_files_then_explains_broken_files_after_crash(self):
        # 11-19 维护者：起局前不核任何运行文件（原先核主程序 / 程序描述大小、给内存表算摘要）。期望手写：主程序缺、内存表与清单对不上，
        #   照样起到 c.run、不出黄字；游戏非 0 退出后再完整核一遍：有缺 / 坏的 → runtime_invalid、点名例子并说「重新出一次 Mac 版」；都好 → 照旧 game_crashed
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        self.fake_host(game["id"])
        runtime = layout.image_mac(self.c, game["id"], create=False)
        runtime.mkdir(parents=True, exist_ok=True); (runtime / "ok.bin").write_bytes(b"12")
        (runtime / "addr_need.tsv").write_text("x\n")
        atomic(runtime / "manifest.json", {"schema": 1, "engine": "S2Arm", "target": "macos-arm64", "gameId": game["id"], "memoryRequirementsVersion": 2,
                                           "memoryRequirementsSha256": "0" * 64, "files": {"main": [5, ""], "ok.bin": [2, ""], "romfs/gone.bin": [9, ""]}})
        ran = []
        def crash(command, log, env, **kwargs):
            ran.append(command); self.c.last_rc = 139; raise Failure("游戏运行中失败（退出码 139）：fixture", code="tool_exit")
        with patch.object(build, "validate_runtime", side_effect=AssertionError("起局去核运行文件了")), \
             patch.object(build, "digest", side_effect=AssertionError("起局去算摘要了")), patch.object(self.c, "run", side_effect=crash):
            with self.assertRaises(Failure) as e:
                build.launch(self.c)
        self.assertEqual(len(ran), 1)   # 主程序缺也照常起了
        self.assertEqual([d for d in self.c.job["diagnostics"] if d.get("code") == "runtime_invalid" and d.get("severity") == "warning"], [])
        self.assertEqual(e.exception.code, "runtime_invalid")
        self.assertIn("main", str(e.exception)); self.assertIn("重新出一次 Mac 版", str(e.exception))
        (runtime / "main").write_bytes(b"12345"); (runtime / "romfs").mkdir(exist_ok=True); (runtime / "romfs/gone.bin").write_bytes(b"123456789")
        with patch.object(self.c, "run", side_effect=crash):
            with self.assertRaises(Failure) as e:
                build.launch(self.c)
        self.assertEqual(e.exception.code, "game_crashed")   # 文件都好：是真崩溃

    def test_launch_warns_and_continues_with_legacy_request(self):
        # 请求带旧字段 vammLayout（旧启动器发的）→ 不抛错、照常起到 c.run，黄字一条（runtime_invalid / warning，「先照常启动」）；
        #   这局崩了（139、文件都好）→ 仍是 game_crashed，消息末尾带「起局前已知」这条
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]; self.c.r["vammLayout"] = "legacy"
        self.fake_host(game["id"])
        runtime = layout.image_mac(self.c, game["id"], create=False); runtime.mkdir(parents=True, exist_ok=True)
        manifest = {"gameId": game["id"], "memoryRequirementsVersion": 2, "files": {}}
        ran = []
        def crash(command, log, env, **kwargs):
            ran.append(command); self.c.last_rc = 139; raise Failure("游戏运行中失败（退出码 139）：fixture", code="tool_exit")
        with patch.object(build, "runtime_manifest", return_value=manifest), patch.object(self.c, "run", side_effect=crash):
            with self.assertRaises(Failure) as e:
                build.launch(self.c)
        self.assertEqual(len(ran), 1)   # 照常起了
        warned = [(d["code"], d["severity"]) for d in self.c.job["diagnostics"] if "先照常启动" in str(d)]
        self.assertEqual(warned, [("runtime_invalid", "warning")])
        self.assertEqual(e.exception.code, "game_crashed")
        self.assertTrue(str(e.exception).endswith("\n起局前已知：请求里带着已停用的旧内存布局设置（vammLayout）"))

    def test_bad_or_unsupported_input_mapping_falls_back_to_default_keys(self):
        # v0.3.1（认不出的直接做）：键位映射不合规 / 宿主不认映射 → 这局用默认键位照常起，不让玩家先去重新打包
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        atomic(layout.path(self.c, game["id"], "input.json"), {"schema": 99})   # 存的映射坏了
        env = build.runtime_env(self.c, game, self.root / "runtime")
        self.assertNotIn("S2A_INPUT_CONFIG", env)
        good = dict(schema=1, keyboard={"A": {"code": 14}}, mouse={}, gamepad={}, axes={}, deadzone=.15,
                    mouseOptions=dict(mode="off", sensitivityX=1, sensitivityY=1))
        self.c.r["launchInputConfig"] = good
        self.assertIn("S2A_INPUT_CONFIG", build.runtime_env(self.c, game, self.root / "runtime"))
        self.assertNotIn("S2A_INPUT_CONFIG", build.runtime_env(self.c, game, self.root / "runtime", input_ok=False))
        # 起局：宿主不认映射 → 黄字提醒、照常起局（原先报 runtime_invalid 不让玩）
        self.fake_host(game["id"]); notes, ran = [], []
        with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
             patch.object(build.toolchain, "input_interface", return_value=(False, "fixture")), \
             patch.object(self.c, "run", side_effect=lambda command, log, env, **k: ran.append(env)), \
             patch.object(self.c, "diagnostic", lambda m, **k: notes.append((m, k.get("severity")))):
            build.launch(self.c)
        self.assertEqual(len(ran), 1); self.assertNotIn("S2A_INPUT_CONFIG", ran[0])
        self.assertTrue(any(sev == "warning" and "默认键位" in m for m, sev in notes), notes)
    def test_launch_collects_mac_pipeline_records_for_the_next_package(self):
        # GUI 起局与 runtime/run_mac.sh 同口径：宿主写到本款 S2A_DOCS/wire/，收局后并进出包只读的
        # S2A_RUN_ROOT/s2arm/pso_seed/。按整行保留首见顺序、去重；下一局只追加新行。
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        self.fake_host(game["id"])
        sessions = ["R\tfirst\nR\tsecond\nR\tfirst\n", "R\tsecond\nC\tthird\n"]
        def run(command, log, env, **kwargs):
            source = Path(env["S2A_DOCS"]) / "wire" / game["id"] / "pso_user.tsv"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(sessions.pop(0))
        with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
             patch.object(self.c, "run", side_effect=run):
            build.launch(self.c)
            build.launch(self.c)
        seed_dir = self.c.work_root / "run" / "s2arm" / "pso_seed" / game["id"]
        sources = list(seed_dir.glob("mac_*.tsv"))
        self.assertEqual(len(sources), 1)
        self.assertEqual(sources[0].read_text(), "R\tfirst\nR\tsecond\nC\tthird\n")

    def test_launch_collects_mac_pipeline_records_after_host_failure_or_cancellation(self):
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        self.fake_host(game["id"])
        for row, failure in (("R\tfrom-failure\n", Failure("fixture host failed")),
                             ("R\tfrom-cancel\n", Cancelled())):
            def run(command, log, env, **kwargs):
                source = Path(env["S2A_DOCS"]) / "wire" / game["id"] / "pso_user.tsv"
                source.parent.mkdir(parents=True, exist_ok=True); source.write_text(row)
                raise failure
            with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
                 patch.object(self.c, "run", side_effect=run):
                with self.assertRaises(type(failure)):
                    build.launch(self.c)
        sources = list((self.c.work_root / "run" / "s2arm" / "pso_seed" / game["id"]).glob("mac_*.tsv"))
        self.assertEqual(len(sources), 1)
        self.assertEqual(sources[0].read_text(), "R\tfrom-failure\nR\tfrom-cancel\n")
    def test_launch_removes_only_its_snapshot_after_exit_failure_or_cancellation(self):
        game = self.game(); self.catalogue([game]); self.c.r.update(gameId=game["id"], inputGuideHandled=True,
            launchInputConfig=dict(schema=1, keyboard={"A": {"code": 14}}, mouse={}, gamepad={}, axes={},
                                   deadzone=.15, mouseOptions=dict(mode="off", sensitivityX=1, sensitivityY=1)))
        self.fake_host(game["id"])
        sibling = self.c.private / "runtime-input" / game["id"] / "another-run.json"
        atomic(sibling, {"keep": True})
        for failure in (None, Failure("fixture host failed"), Cancelled()):
            captured = []
            def run(command, log, env, **kwargs):
                path = Path(env["S2A_INPUT_CONFIG"]); captured.append(path)
                self.assertTrue(path.is_file())
                self.assertEqual(env["S2A_INPUT_GUIDE_SHOWN"], "1")
                if failure is not None:
                    raise failure
            with patch.object(build, "runtime_manifest", return_value={"gameId": game["id"], "memoryRequirementsVersion": 2}), \
                 patch.object(build.toolchain, "input_interface", return_value=(True, "fixture")), \
                 patch.object(self.c, "run", side_effect=run):
                if failure is None:
                    build.launch(self.c)
                else:
                    with self.assertRaises(type(failure)):
                        build.launch(self.c)
            self.assertEqual(len(captured), 1)
            self.assertFalse(captured[0].exists())
            self.assertTrue(sibling.is_file())

    def test_hash_validation_rejects_same_size_corruption_and_traversal(self):
        # v0.2.3 清理：缺省只核大小（同大小改写不再逐字节查，#304 被改的文件是靠大小查出来的）；调用方明确要 full=True 时照旧按摘要拒；越界照旧拒
        root = self.root / "products"
        root.mkdir()
        f = root / "module"
        f.write_bytes(b"valid")
        record = {"module": [5, digest(f)]}
        build.validate_files(self.c, root, record)
        f.write_bytes(b"wrong")
        build.validate_files(self.c, root, record)   # 缺省只核大小：同大小改写放行
        with self.assertRaises(Failure):
            build.validate_files(self.c, root, record, full=True)
        f.write_bytes(b"longer!")
        with self.assertRaises(Failure):
            build.validate_files(self.c, root, record)   # 大小变了照旧拒
        with self.assertRaises(Failure):
            build.validate_files(self.c, root, {"../outside": [0, ""]})

    def test_size_check_rejects_symlinks_and_escaping_directories(self):
        # validate_files 每个目录只解析一次（11-07 提速）后，判据仍同逐个 contained()：目录软链指到外面、文件本身是软链、大小不对都拒
        root = self.root / "products2"
        (root / "sub").mkdir(parents=True)
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "m").write_bytes(b"12345")
        (root / "sub" / "ok").write_bytes(b"12345")
        build.validate_files(self.c, root, {"sub/ok": [5, ""]}, full=False)
        with self.assertRaises(Failure):
            build.validate_files(self.c, root, {"sub/ok": [4, ""]}, full=False)
        (root / "link").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(Failure):
            build.validate_files(self.c, root, {"link/m": [5, ""]}, full=False)
        (root / "sub" / "filelink").symlink_to(outside / "m")
        with self.assertRaises(Failure):
            build.validate_files(self.c, root, {"sub/filelink": [5, ""]}, full=False)
        with self.assertRaises(Failure):
            build.validate_files(self.c, root, {"sub/missing": [5, ""]}, full=False)

    def test_cleanup_preserves_releases_saves_and_other_games_sources(self):
        package = self.package()
        game = self.game(packages=[package])
        other = self.game("0100000000000001", [package.copy()])
        self.catalogue([game, other])
        save = layout.path(self.c, game["id"], "saves") / "progress"
        release = layout.image_mac(self.c, game["id"]) / "runtime"
        for f in (save, release):
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text("keep")
        cache = layout.build_dir(self.c, game["id"])
        cache.mkdir(parents=True)
        (cache / "scratch").write_text("discard")
        self.c.r.update(gameId=game["id"], kind="cache")
        library.clean(self.c)
        self.assertFalse(cache.exists())
        self.c.r["kind"] = "sources"
        library.clean(self.c)
        self.assertTrue((self.c.library / package["path"]).exists())
        self.assertEqual(save.read_text(), "keep")
        self.assertEqual(release.read_text(), "keep")

    def test_remove_only_hides_catalogue_entry(self):
        game = self.game(packages=[self.package()])
        self.catalogue([game])
        self.c.r["gameId"] = game["id"]
        library.remove(self.c)
        self.assertTrue(self.c.catalog()["games"][0]["hidden"])
        self.assertTrue((self.c.library / game["packages"][0]["path"]).exists())

    def _build_leftovers(self, gid):
        """造一款出完 iPhone 包的现场（老玩家：有资源包、还没有容器件）：image/ios/pack + 出包工作目录 build/ios（日志 / 产物根 / 解包）
        + 私有目录里按程序号存的缓存 + 别的款的缓存 + 共享 metallib 缓存"""
        low, run = gid.lower(), self.c.work_root / "run" / "s2arm"
        out = layout.ios_work(self.c, gid)
        keep = [layout.image_ios(self.c, gid) / ".pack" / "manifest.json", out / "logs" / "a.log"]
        gone = [out / "roots" / f"t{low}@现行" / "x", out / "extract" / "romfs" / "f", run / "cache" / "direct" / gid.upper() / "work" / "cas",
                run / "cache" / "bake" / f"{low}-abc" / "ok", run / "cache" / "aoc" / f"t{low}-k" / "aoc.tsv", run / "cache" / "srcver" / f"t{low}-k" / "r",
                run / "widen" / f"t{low}_b" / "ui", run / "widen" / "auto" / f"t{low}_b" / "w", self.c.work_root / "cache" / "air" / "ios" / "x.air"]
        other = [run / "cache" / "direct" / "0100AAAAAAAAA000" / "cas", run / "cache" / "bake" / "0100aaaaaaaaa000-abc" / "ok", self.c.work_root / "cache" / "nxaot" / "host.o"]
        for f in keep + gone + other:
            f.parent.mkdir(parents=True, exist_ok=True); f.write_text("x")
        return keep, gone, other

    def test_prune_build_keeps_ipa_pack_logs_and_clears_this_games_intermediates(self):
        # Q15 政策：成功后保留共享编库缓存；清本款一次性中间产物，不改变显式清缓存/删除语义。
        game = self.game(packages=[self.package()]); self.catalogue([game])
        keep, gone, other = self._build_leftovers(game["id"])
        keep.append(gone.pop())  # shared AIR cache is retained only on automatic success cleanup
        # 多平台共用阶段：自动清理保留产物根现行版本（链接与它指向的版本），旧版本照删；显式清缓存 / 删除仍整个删（下两条）
        roots = layout.ios_work(self.c, game["id"]) / "roots"; low = game["id"].lower()
        keep.append(gone.pop(0))   # roots/t<程序号>@现行/x
        new = roots / f"t{low}x@gk-new" / "y"; new.parent.mkdir(); new.write_text("x"); keep.append(new)
        (roots / f"t{low}x@现行").symlink_to(new.parent.name)   # 现行是链接：它指向的版本保留
        old = roots / f"t{low}@gk-old" / "z"; old.parent.mkdir(); old.write_text("x"); gone.append(old)
        library.prune_build(self.c, game["id"])
        self.assertEqual([f for f in keep + other if not f.exists()], [])
        self.assertEqual([f for f in gone if f.exists()], [])
        # 有了容器件（程序缓存 cache/programs/<程序号>/kit，pack/jobs/programs.py）：现行产物根也不用留，build/ 只剩出包日志 build/ios/logs
        import programs
        kit = programs.cache(self.c, game["id"]) / "kit" / "main.npdm"; kit.parent.mkdir(parents=True); kit.write_bytes(b"META")
        (kit.parent / "main.fixups.tsv").write_text("")
        library.prune_build(self.c, game["id"])
        build = layout.build_dir(self.c, game["id"], create=False)
        self.assertEqual(sorted(str(q.relative_to(build)) for q in build.rglob("*")), ["ios", "ios/logs", "ios/logs/a.log"])
        self.assertTrue(kit.is_file()); self.assertTrue(keep[0].is_file())

    def test_prune_build_keeps_windows_work_root(self):
        # #32：Windows 产物根在 work/ 下。手算期望：现行链接及指向、.romfs_store、direct*、pkg/product/main.npdm 留下；旧版本、其他中间产物、product/pkg（交付件已拷走）删掉；链接只删自身
        game = self.game(packages=[self.package()]); self.catalogue([game])
        work = layout.ios_work(self.c, game["id"]) / "work"
        def f(rel):
            q = work / rel; q.parent.mkdir(parents=True, exist_ok=True); q.write_text("x"); return q
        keep = [f("roots/k@new/y"), f("roots/.romfs_store/b"), f("direct/lib"), f("direct_p1/lib"), f("pkg/product/main.npdm")]
        (work / "roots/k@现行").symlink_to("k@new")
        gone = [f("roots/k@old/z"), f("dcache/a"), f("pkg/tmp/b"), f("pkg/product/pkg/x.ipa")]
        outside = self.c.library / "outside" / "z"; outside.parent.mkdir(parents=True); outside.write_text("x")
        (work / "pkg/link").symlink_to(outside.parent)   # 指向外部的链接：只删链接，指向的文件在
        with patch.object(library, "_windows_layout", return_value=True):
            pack = layout.image_ios(self.c, game["id"]) / ".pack"; pack.mkdir(parents=True); (pack / "manifest.json").write_text("{}")   # 老玩家：有资源包没容器件
            library.prune_build(self.c, game["id"])
        self.assertEqual([q for q in keep if not q.exists()], [])
        self.assertEqual([q for q in gone if q.exists()], [])
        self.assertTrue((work / "roots/k@现行").is_symlink())
        self.assertFalse(os.path.lexists(work / "pkg/link"))
        self.assertTrue(outside.exists())

    def test_clean_cache_also_prunes_iphone_intermediates(self):
        game = self.game(packages=[self.package()]); self.catalogue([game])
        keep, gone, other = self._build_leftovers(game["id"])
        self.c.r.update(gameId=game["id"], kind="cache")
        library.clean(self.c)
        self.assertEqual([f for f in gone if f.exists()], [])
        self.assertTrue(all(f.exists() for f in keep + other))

    def test_delete_removes_outputs_caches_and_entry_keeps_saves_and_sources(self):
        # 期望：删除这款游戏 = IPA / 资源包 / 中间产物 / Mac 构建缓存 / 本款缓存全删、清单里没有这一条；存档与托管原始文件保留，别的款不动
        package = self.package(); game = self.game(packages=[package]); self.catalogue([game])
        keep, gone, other = self._build_leftovers(game["id"])
        save = layout.path(self.c, game["id"], "saves") / "s.dat"; save.parent.mkdir(parents=True); save.write_text("keep")
        mac = layout.build_dir(self.c, game["id"]) / "root"; mac.mkdir(parents=True)
        self.c.r["gameId"] = game["id"]
        library.delete(self.c)
        self.assertEqual([f for f in keep + gone if f.exists()], [])
        self.assertFalse(mac.exists())
        self.assertEqual([g["id"] for g in self.c.catalog()["games"]], [])
        self.assertEqual(save.read_text(), "keep")
        self.assertTrue((self.c.library / package["path"]).exists())
        self.assertTrue(all(f.exists() for f in other))

    def test_delete_keeps_every_kind_of_save(self):
        # 维护者 11-17：「删除游戏」原先只留 saves/、save-backups/，把 SaveData*（Mac 原生格式存档位置）和 save_backup/（装机前从 iPhone /
        #   Apple TV 拉回的存档备份）一起删了。期望（手写）：带齐四种存档（另加一个玩家的 SaveData-<玩家>）的款删完，五样都在、内容不变；
        #   金手指、mod、封面、Mac 运行版本这些照删；清单里没有这款。存档类以 layout.save_entries 为唯一来源
        package = self.package(); game = self.game(packages=[package]); self.catalogue([game])
        g = layout.game_dir(self.c, game["id"])
        saves = {"saves/a.sav": "s", "save-backups/20261117/a.sav": "b", "save_backup/20261117-080000/0000000000000001/0/x.bin": "p",
                 "SaveData/account/x.bin": "n", "SaveData-Player2/account/y.bin": "m"}
        others = ["cheats/c.txt", "mods/m/romfs/a.bin", "cover.jpg", "image/mac/main.patched.bin", "seen/dev.tsv"]
        for rel, text in list(saves.items()) + [(r, "x") for r in others]:
            (g / rel).parent.mkdir(parents=True, exist_ok=True); (g / rel).write_text(text)
        self.assertEqual(sorted(p.name for p in layout.save_entries(g)), ["SaveData", "SaveData-Player2", "save-backups", "save_backup", "saves"])
        self.c.r["gameId"] = game["id"]
        library.delete(self.c)
        self.assertEqual({rel: (g / rel).read_text() for rel in saves if (g / rel).is_file()}, saves)
        self.assertEqual([r for r in others if (g / r).exists()], [])
        self.assertEqual([x["id"] for x in self.c.catalog()["games"]], [])

    def test_delete_removes_the_program_store_too(self):
        # 期望（手写）：「删除游戏」= 从库里删、也从手机记录摘掉，这款在游戏程序专用文件夹 .programs/<程序号>/ 的 kit / 原始程序 / 兼容记录一并删；别款的不动
        package = self.package(); game = self.game(packages=[package]); self.catalogue([game])
        mine, other = layout.program_store(self.c, game["id"], create=True), layout.program_store(self.c, "0100AAAA00000000", create=True)
        for d in (mine, other):
            (d / "kit").mkdir(); (d / "kit" / "main.npdm").write_bytes(b"n")
        self.c.r["gameId"] = game["id"]
        library.delete(self.c)
        self.assertFalse(mine.exists())
        self.assertTrue((other / "kit" / "main.npdm").is_file())

    def test_delete_retry_after_half_done_delete_removes_the_leftover_folder(self):
        # #433：上次删到一半（目录删不空、Errno 66）时 game.json 已被删掉，文件夹和出包中止留下的临时目录还在、索引还指着它。
        #   期望（手写）：再点删除按索引找回同一个文件夹删干净、清单里去掉；原先认不出这个文件夹，只去掉条目、残留目录留在盘上
        package = self.package(); game = self.game(packages=[package]); self.catalogue([game])
        gdir = layout.game_dir(self.c, game["id"])
        tmp = gdir / "build" / "extract.ns2_dump.tmp"; tmp.mkdir(parents=True); (tmp / "part.bin").write_bytes(b"x" * 4096)
        (gdir / layout.GAME_JSON).unlink()
        self.assertIsNone(layout.find_game(self.c, game["id"]))   # 认不出（没有 game.json）
        self.c.r["gameId"] = game["id"]
        library.delete(self.c)
        self.assertFalse(gdir.exists())
        self.assertEqual([g["id"] for g in self.c.catalog()["games"]], [])

    def test_delete_removes_files_without_holding_library_locks(self):
        # 诊断#dab32d：删一款大游戏在慢盘上删了 53 分钟，整段占着执行锁，期间开别的款 6 次全 library_busy。
        #   期望（手写）：挪进 .s2a-trash、清单去掉这款在锁内做完；之后每一次真删（_rm）时执行锁和清单锁都拿得到；
        #   这款的缓存（直连库）和共用编库缓存照旧删掉（删除语义不变），工具缓存根的 .s2a-trash 删完是空的
        import filelock
        package = self.package(); game = self.game(packages=[package]); self.catalogue([game])
        keep, gone, other = self._build_leftovers(game["id"])
        free, real = [], library._rm
        def rm(c, root, p):
            for name in ("execution", "catalog"):
                try:
                    with filelock.held(self.c.library / f".{name}.lock", wait_s=0, holder={}):
                        free.append(True)
                except filelock.Busy:
                    free.append(False)
            return real(c, root, p)
        self.c.r["gameId"] = game["id"]
        with unittest.mock.patch.object(library, "_rm", rm):
            library.delete(self.c)
        self.assertTrue(free and all(free), free)
        self.assertEqual([f for f in keep + gone if f.exists()], [])
        self.assertEqual([g["id"] for g in self.c.catalog()["games"]], [])
        self.assertEqual(list((self.c.work_root / library.TRASH).iterdir()), [])
        self.assertTrue(all(f.exists() for f in other))

    def test_sweep_trash_also_clears_tool_cache_trash(self):
        # 删除时挪进工具缓存根 .s2a-trash 的缓存当时没删掉（被占用 / 进程被杀）：期望下次启动 sweep_trash 一并清掉
        left = self.c.work_root / library.TRASH / "0100AAAAAAAAA000-1-bake" / "ok"; left.parent.mkdir(parents=True); left.write_text("x")
        library.sweep_trash(self.c)
        self.assertEqual(list((self.c.work_root / library.TRASH).iterdir()), [])

    def test_delete_with_undeletable_file_still_removes_game_from_library(self):
        # 11-11 玩家反馈「删除有时生效有时不生效」（真 App 复现：游戏文件夹里一个文件删不掉 → 整个删除失败、目录删一半、游戏还挂在列表里成了「找不到游戏文件」）。
        #   期望（手写）：游戏立刻从清单里去掉、原文件夹不在原处；删不掉的那个文件留在存放位置的 .s2a-trash 里，记一条黄字；
        #   之后读游戏库（catalog → sweep_trash）文件能删了就清掉
        package = self.package(); game = self.game(packages=[package]); self.catalogue([game])
        gdir = layout.game_dir(self.c, game["id"])
        locked = gdir / "image" / "ios" / "kit" / "a.tsv"; locked.parent.mkdir(parents=True); locked.write_text("x")
        os.chflags(locked, stat.UF_IMMUTABLE)
        self.addCleanup(lambda: [os.chflags(p, 0) for p in (self.c.library / library.TRASH).rglob("a.tsv")] if (self.c.library / library.TRASH).exists() else None)
        self.c.r["gameId"] = game["id"]
        library.delete(self.c)
        self.assertFalse(gdir.exists())
        self.assertEqual([g["id"] for g in self.c.catalog()["games"]], [])
        left = list((self.c.library / library.TRASH).rglob("a.tsv"))
        self.assertEqual(len(left), 1)
        self.assertTrue(any(d["severity"] == "warning" and "空闲时会再删" in json.dumps(d, ensure_ascii=False) for d in self.c.job["diagnostics"]))
        os.chflags(left[0], 0)
        library.sweep_trash(self.c)
        self.assertEqual(list((self.c.library / library.TRASH).iterdir()), [])

    def test_delete_with_saves_on_exfat_skips_appledouble_that_left_with_its_file(self):
        # 诊断#3d03f4：exFAT 上有存档的款逐项挪进 .s2a-trash，挪走 banner.jpg 时系统连带挪走 ._banner.jpg，列表里还挂着它 →
        #   原先 rename 报 ENOENT、整单「删不掉 ._banner.jpg」job_failed。期望（手写）：删除完成、清单去掉这款、存档保留、游戏目录里只剩 saves
        package = self.package(); game = self.game(packages=[package]); self.catalogue([game])
        gdir = layout.game_dir(self.c, game["id"])
        (gdir / "banner.jpg").write_bytes(b"j"); (gdir / "._banner.jpg").write_bytes(b"a")
        save = gdir / "saves" / "s.dat"; save.parent.mkdir(parents=True); save.write_text("keep")
        real = os.rename
        def rename(src, dst):   # 照 exFAT：数据文件挪走，它的 AppleDouble 附属文件跟着走
            real(src, dst)
            side = Path(src).with_name("._" + Path(src).name)
            if side.exists():
                real(side, Path(dst).with_name("._" + Path(dst).name))
        self.c.r["gameId"] = game["id"]
        listdir = Path.iterdir
        def iterdir(d):   # 让 banner.jpg 先于 ._banner.jpg 挪（真盘上的列目录顺序不保证，这里固定成会出事的那种）
            return iter(sorted(listdir(d), key=lambda q: q.name.startswith("._")))
        with unittest.mock.patch.object(library.os, "rename", rename), unittest.mock.patch.object(Path, "iterdir", iterdir):
            library.delete(self.c)
        self.assertEqual([g["id"] for g in self.c.catalog()["games"]], [])
        self.assertEqual(save.read_text(), "keep")
        self.assertEqual(sorted(i.name for i in gdir.iterdir()), ["saves"])

    def test_delete_also_drops_game_from_library_container(self):
        # 期望（#227，游戏库结构 v3）：删 A → 各台手机记录（devices/<设备>.json）里摘掉 A，B 与其余字段原样；没装 A 的那台记录不动；
        #   不装机、不碰默认位置根的 S2Arm.ipa（下次给那台装机时现组的容器就不再带 A）
        a, b = "0100000000010000", "0100000000020000"
        layout.save_device(self.c, "phone1", dict(games={a: dict(pack="pa"), b: dict(pack="pb")}, shellId="S", bundleId="io.s2arm.games.x"))
        layout.save_device(self.c, "phone2", dict(games={b: dict(pack="pb")}, shellId="S"))
        untouched = layout.device_path(self.c, "phone2").read_bytes()
        layout.phone_ipa(self.c).write_bytes(b"old")
        self.catalogue([self.game(tid=a, packages=[self.package(tid=a)]), self.game(tid=b, packages=[self.package(tid=b, sha="b" * 64)])])
        self.c.r["gameId"] = a
        library.delete(self.c)
        rec = layout.load_device(self.c, "phone1")
        self.assertEqual((rec["games"], rec["shellId"], rec["bundleId"]), ({b: dict(pack="pb")}, "S", "io.s2arm.games.x"))
        self.assertEqual(layout.device_path(self.c, "phone2").read_bytes(), untouched)
        self.assertEqual(layout.phone_ipa(self.c).read_bytes(), b"old")
        self.c.r["gameId"] = b
        library.delete(self.c)   # 最后一款也摘（摘空）
        self.assertEqual([layout.load_device(self.c, k)["games"] for k in ("phone1", "phone2")], [{}, {}])

    def test_delete_waits_for_execution_lock_held_by_cancelled_task(self):
        # 真实持锁进程直到显式释放才退出；先观测生产删除的等待事件，
        # 确认没有提前删除，再释放锁并验证完成，不依赖父进程及时抢到 CPU。
        game = self.game(packages=[self.package()]); self.catalogue([game])
        self.c.require_library()
        lockf = self.c.library / ".execution.lock"
        holder = subprocess.Popen([sys.executable, "-c", "import sys; sys.path.insert(0,sys.argv[2]); import filelock; f=open(sys.argv[1],'a'); filelock.try_lock(f); print('ok',flush=True); sys.stdin.readline(); filelock.unlock(f)", str(lockf), str(JOBS)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        finished = threading.Event()
        errors = []
        def delete():
            try:
                library.delete(self.c)
            except BaseException as error:
                errors.append(error)
            finally:
                finished.set()
        worker = threading.Thread(target=delete)
        try:
            self.assertEqual(holder.stdout.readline().strip(), "ok")
            self.c.r["gameId"] = game["id"]
            worker.start()
            deadline = time.monotonic() + 10
            while '"phase": "waiting_lock"' not in self.output.getvalue():   # 锁排队进度（剃刀 H2：不再另套按报错原文重试的循环）
                self.assertFalse(finished.wait(0.01), "delete returned before reporting the held execution lock")
                self.assertLess(time.monotonic(), deadline, "delete did not report waiting for the held execution lock")
            self.assertFalse(finished.is_set(), "delete finished while another process still held the lock")
            self.assertEqual([g["id"] for g in self.c.catalog()["games"]], [game["id"]])
            holder.stdin.write("release\n"); holder.stdin.flush()
            self.assertEqual(holder.wait(timeout=5), 0)
            self.assertTrue(finished.wait(10), "delete did not finish after the execution lock was released")
            worker.join()
            self.assertEqual(errors, [])
            self.assertEqual(self.c.catalog()["games"], [])
        finally:
            if holder.poll() is None:
                holder.kill()
            holder.communicate(timeout=5)
            if worker.ident is not None:
                worker.join(5)
            self.assertFalse(worker.is_alive(), "delete worker remained after lock-holder cleanup")

    def test_lock_timeout_failure_event_is_library_busy_with_holder(self):
        # 出包任务 t7 占着游戏库锁不放；排队上限 0.3 秒 → 失败事件 code=library_busy、holder 原样带出、下一步取 RECOVERY["library_busy"]、
        #   上屏原句不变（旧代码：code=job_failed，占锁者只在 detail 文字里）
        self.c.require_library()
        lockf = self.c.library / ".catalog.lock"
        info = dict(taskId="t7", operation="build", gameId="0100000000010000", pid=1, startedAt=1.0)
        holder = subprocess.Popen([sys.executable, "-c", "import sys,json; sys.path.insert(0,sys.argv[2]); import filelock; f=open(sys.argv[1],'a'); filelock.try_lock(f); open(sys.argv[1]+'.holder','w').write(sys.argv[3]); print('ok',flush=True); sys.stdin.readline(); filelock.unlock(f)", str(lockf), str(JOBS), json.dumps(info)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), "ok")
            self.c.lock_wait = 0.3
            with self.assertRaises(Failure) as raised:
                with self.c.lock():
                    pass
            event = self.c.describe_failure(raised.exception)
            self.assertEqual((event["code"], event["holder"], event["summary"]), ("library_busy", info, "另一个任务正在使用游戏库，请稍后重试。"))
            self.assertEqual(event["suggestion"], __import__("errors").RECOVERY["library_busy"])
            self.assertNotIn("holder", self.c.describe_failure(Failure("别的失败")))
        finally:
            holder.stdin.write("release\n"); holder.stdin.flush()
            holder.communicate(timeout=5)

    def test_proven_dead_lock_holder_uses_stuck_code(self):
        import filelock
        self.c.require_library()
        self.c.lock_wait = 0
        lockf = self.c.library / ".catalog.lock"
        info = dict(taskId="old-task", operation="ipa", gameId="0100000000010000", pid=1,
                    startedAt=1.0, host="this-host")
        with filelock.held(lockf, holder=info), patch("filelock.holder_is_stale", return_value=True):
            with self.assertRaises(Failure) as raised:
                with self.c.lock():
                    pass
        self.assertEqual(raised.exception.code, "library_lock_stuck")
        self.assertEqual(raised.exception.holder, info)
        self.assertIn("不要手动删除锁文件", str(raised.exception))

    def test_direct_small_batch_progress_replaces_finished_translation(self):
        progress = self.root / "progress.tsv"
        progress.write_text("1\t直连库·翻译着色器\t30\t30\n2\t直连库·已生成库文件\t1\t3\n", encoding="utf-8")
        with patch.object(self.c, "emit") as emit:
            pos = self.c.stage_progress(progress, 0)
        event = emit.call_args.kwargs
        self.assertEqual((event["done"], event["total"]), (1, 3))
        self.assertEqual([c["name"] for c in event["counters"]], ["直连库·已生成库文件"])
        with progress.open("a", encoding="utf-8") as f:
            f.write("3\t直连库·已生成库文件\t3\t3\n4\t直连库·整份出库与收尾\t0\t2\n")
        with patch.object(self.c, "emit") as emit:
            self.c.stage_progress(progress, pos)
        event = emit.call_args.kwargs
        self.assertEqual((event["done"], event["total"]), (0, 2))
        self.assertEqual([c["name"] for c in event["counters"]], ["直连库·整份出库与收尾"])

    def test_direct_space_recheck_extrapolates_this_run(self):
        # 11-10（0400A3A01EE4C000 预检 22.3 GB，翻译 99084 个着色器期间直连缓存涨到约 44 GB 写满盘）：按本局实际消耗外推，不用常数。
        #   期望手算：开始时本款占用 10.5 GB；10 秒后完成 20000、占用 19.5 GB、卷剩 36 GB → 还需 9 GB ÷ 20000 × 79084 × 1.1 = 39.15 GB > 36 GB → 停
        import core
        from errors import Failure
        self.c.operation = "build"
        name = "直连库·翻译与收尾"
        DU = collections.namedtuple("DU", "total used free")
        def step(done, used_gb, free_gb, now):
            with patch.object(core, "job_footprint", return_value={1: (self.root, int(used_gb * 10**9), "library")}), \
                    patch.object(core.shutil, "disk_usage", return_value=DU(0, 0, int(free_gb * 10**9))):
                core.direct_space_recheck(self.c, {name: dict(done=done, total=99084)}, now=now)
        step(0, 10.5, 45, 100.0)                         # 记起点
        step(3000, 11.5, 44, 111.0)                      # 不到 5%：不判
        with patch.object(self.c, "diagnostic") as diag:   # 11-14 不许堵门：外推不够只提示、不停
            step(20000, 19.5, 36, 122.0)
        [call] = diag.call_args_list
        self.assertEqual((call.args[1], call.kwargs.get("severity")), ("disk_full", "warning"))
        self.assertIn("还要再占约 39 GB", call.args[0]); self.assertIn("只剩 36 GB", call.args[0])
        # 同卷别的作业写盘：卷从 45 GB 掉到 30 GB，但本款只多占 1 GB → 还需 1 ÷ 20000 × 79084 × 1.1 ≈ 4.3 GB < 30 GB，不误报
        self.c._direct_space_marks = {}; self.c._direct_space_warned = False
        with patch.object(self.c, "diagnostic") as diag:
            step(0, 10.5, 45, 200.0); step(20000, 11.5, 30, 211.0)
        diag.assert_not_called()
        self.c._direct_space_marks = {}; self.c._direct_space_warned = False
        with patch.object(self.c, "diagnostic") as diag:
            step(0, 10.5, 45, 300.0); step(20000, 19.5, 36, 311.0); step(30000, 24, 31, 322.0)   # 连着几次都不够：每单只打一次黄
        self.assertEqual([call.kwargs.get("severity") for call in diag.call_args_list], ["warning"])

    def test_direct_space_recheck_per_volume(self):
        # P1-165：存放位置（卷 1）与工具缓存（卷 2）是两块盘。翻译期间只有缓存那块在涨：卷 1 余量再大也救不了卷 2。
        #   期望手算：卷 2 开始 5 GB → 10 秒后完成 20000、14 GB，余量 30 GB → 还需 9 ÷ 20000 × 79084 × 1.1 = 39.15 GB > 30 GB → 停，点名工具缓存那块盘
        import core
        from errors import Failure
        self.c.operation = "build"
        name = "直连库·翻译与收尾"
        free = {"loc": 500 * 10**9, "cache": 30 * 10**9}
        DU = collections.namedtuple("DU", "total used free")
        def step(done, loc_gb, cache_gb, now):
            fp = {1: ("loc", int(loc_gb * 10**9), "library"), 2: ("cache", int(cache_gb * 10**9), "cache")}
            with patch.object(core, "job_footprint", return_value=fp), \
                    patch.object(core.shutil, "disk_usage", side_effect=lambda path: DU(0, 0, free[path])):
                core.direct_space_recheck(self.c, {name: dict(done=done, total=99084)}, now=now)
        step(0, 10.5, 5, 100.0)
        with patch.object(self.c, "diagnostic") as diag:   # 11-14 不许堵门：只提示、不停
            step(20000, 10.5, 14, 111.0)
        msg = diag.call_args.args[0]
        self.assertIn("工具缓存所在的盘只剩 30 GB", msg); self.assertIn("还要再占约 39 GB", msg)

    def test_stage_progress_feeds_direct_space_recheck(self):
        import core
        progress = self.root / "progress.tsv"
        progress.write_text("1\t直连库·翻译与收尾\t20000\t99084\n", encoding="utf-8")
        with patch.object(self.c, "emit"), patch.object(core, "direct_space_recheck") as recheck:
            self.c.stage_progress(progress, 0)
        counters = recheck.call_args.args[1]
        self.assertEqual((counters["直连库·翻译与收尾"]["done"], counters["直连库·翻译与收尾"]["total"]), (20000, 99084))

    def test_protocol_has_ordered_events_and_measured_counters(self):
        self.c.begin("phase", 2, 5)
        self.c.emit(done=5, total=10, unit="files")
        self.c.finish("completed", "done", {"outputPath": "/result"})
        events = [json.loads(line) for line in self.output.getvalue().splitlines()]
        self.assertEqual([e["seq"] for e in events], list(range(1, len(events) + 1)))
        self.assertIsNone(events[1]["totalUnits"])
        self.assertEqual(events[-2]["doneUnits"], 5)
        self.assertEqual(events[-1]["state"], "succeeded")
        self.assertEqual(events[-1]["artifactRef"], "/result")
        self.assertEqual(json.loads(self.c.jobpath.read_text())["state"], "completed")

    def test_finish_records_ipa_device_and_pushed_files_for_relaunch(self):
        # 启动器重开后完成卡从任务记录读 IPA（11-01：原先只在结果事件里，重开后画面模式「未知」）
        self.c.finish("completed", "done", {"outputPath": "/out", "ipa": "/out/x.ipa", "device": "测试设备1", "packFiles": 3})
        job = json.loads(self.c.jobpath.read_text())
        self.assertEqual((job["ipaPath"], job["device"], job["packFiles"]), ("/out/x.ipa", "测试设备1", 3))

    def test_subprocess_preserves_spaces_and_reports_real_failure_exit(self):
        log = self.root / "log with spaces.log"
        self.c.run([sys.executable, "-c", "import sys; print(sys.argv[1])", "a b;$(ignored)"], log)
        self.assertIn("a b;$(ignored)", log.read_text())
        with self.assertRaises(Failure):
            self.c.run([sys.executable, "-c", "raise SystemExit(17)"], log)
        self.assertEqual(self.c.job["diagnostics"][-1]["exitCode"], 17)

    @unittest.skipIf(os.name == "nt", "POSIX 信号")
    def test_signal_death_reports_shell_style_exit(self):
        # 宿主崩溃处理写完现场后按默认处置重抛（系统出 .ips），进程被信号结束、Python 给负数（SIGSEGV = -11）：摘要与诊断照旧报 128 + 信号（139）。
        #   这里用 SIGKILL（-9 → 137）验同一条折算：SIGSEGV 会让系统为测试进程生成崩溃报告
        log = self.root / "sig.log"
        with self.assertRaises(Failure) as caught:
            self.c.run([sys.executable, "-c", "import os, signal; os.kill(os.getpid(), signal.SIGKILL)"], log)
        self.assertIn("退出码 137", str(caught.exception))
        self.assertEqual((self.c.job["diagnostics"][-1]["exitCode"], self.c.last_rc), (137, 137))

    def test_cancel_stops_a_silent_process_group(self):
        timer = threading.Timer(.4, self.c.stop)
        timer.start()
        start = time.monotonic()
        try:
            with self.assertRaises(Cancelled):
                self.c.run([sys.executable, "-c", "import subprocess,sys,time; subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); time.sleep(30)"])
        finally:
            timer.cancel()
        self.assertLess(time.monotonic() - start, 8)
        self.assertFalse(self.c.children)

    def test_pull_diagnostics_accepts_oneclick_s2arm_bundle(self):
        import diag
        bundle = "s2arm.abcde12345.0100aabbccdd0000"
        devices = {"result": {"devices": [{"identifier": "synthetic-device",
                   "hardwareProperties": {"platform": "iOS", "productType": "iPhoneSynthetic"},
                   "connectionProperties": {"tunnelState": "connected"}}]}}
        def fake(context, *args, timeout=120):
            if args[:2] == ("list", "devices"):
                return 0, devices, ""
            if args[:3] == ("device", "info", "apps"):
                return 0, {"result": {"apps": [{"bundleIdentifier": bundle, "name": "Synthetic"}]}}, ""
            self.assertEqual(args[args.index("--domain-identifier") + 1], bundle)
            if args[args.index("--source") + 1] == "Documents/host.log":
                Path(args[args.index("--destination") + 1]).write_text("synthetic log")
                return 0, {}, ""
            return 1, {}, ""
        with patch.object(diag, "devicectl", side_effect=fake):
            result = diag.pull(self.c)
        self.assertEqual([app["bundle"] for app in result["apps"]], [bundle])
        self.assertEqual(result["apps"][0]["files"], 1)

    def test_context_redact_keeps_ui_limit_and_can_export_complete_text(self):
        text = str(self.c.library) + "/BEGIN " + "x" * 4000 + " contact=a=b@example.invalid END"
        complete = self.c.redact(text, limit=None)
        self.assertTrue(complete.startswith("[LIBRARY]/BEGIN "))
        self.assertTrue(complete.endswith("contact=[EMAIL] END"))
        self.assertEqual(self.c.redact(text), complete[-3000:])
        self.assertGreater(len(complete), 4000)

    def test_diagnostics_and_rotation_bounded_with_2000_jobs(self):
        """11-13 玩家反馈「文件数超过 1000 个」：任务记录和各任务日志原先整目录打包、从不清。造 2000 个已结束的任务（各带记录 / 请求 / 事件 + 两份日志）：
        期望包内任务记录 ≤ DIAG_JOBS + 1（本次导出）、包内文件 < 1000，没结束的那个照收；轮转后磁盘上任务 ≤ KEEP_TOTAL，被清任务的日志一起没了"""
        import zipfile, uuid, core, time as t
        jobs = self.c.jobpath.parent; jobs.mkdir(parents=True, exist_ok=True)
        now = t.time(); ids = []
        for i in range(2000):
            tid = str(uuid.UUID(int=i + 1)); ids.append(tid)
            state = "running" if i == 1999 else "completed"   # 最老的那个还没结束：照收
            (jobs / f"{tid}.json").write_text(json.dumps(dict(id=tid, operation="catalog", state=state, **({} if state == "running" else {"ended": now}))))
            (jobs / f"{tid}.request.json").write_text("{}"); (jobs / f"{tid}.events.jsonl").write_text("{}\n")
            for log in (f"{tid}.log", f"{tid}-install.log"):
                (self.c.logs / log).write_text("x\n")
            stamp = now - i * 60   # i 越大越旧
            for f in (jobs / f"{tid}.json", jobs / f"{tid}.request.json", jobs / f"{tid}.events.jsonl", self.c.logs / f"{tid}.log", self.c.logs / f"{tid}-install.log"):
                os.utime(f, (stamp, stamp))
        destination = self.root / "diag-2000.zip"; self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            names = archive.namelist(); manifest = json.loads(archive.read("manifest.json"))
        in_jobs = [n for n in names if n.startswith("jobs/")]
        self.assertLessEqual(len(in_jobs), library.DIAG_JOBS + 1)
        self.assertIn(f"jobs/{ids[1999]}.txt", names)          # 没结束的最老那个照收
        self.assertNotIn(f"jobs/{ids[1000]}.txt", names)       # 中间的旧任务不收
        self.assertIn(f"logs/{ids[0]}.log", names); self.assertNotIn(f"logs/{ids[1000]}.log", names)
        self.assertLess(len(names), 1000)
        self.assertEqual((manifest["jobs"]["total"], manifest["jobs"]["limit"]), (2000 + 1, library.DIAG_JOBS))   # + 本次导出自己的记录
        self.assertGreater(manifest["jobs"]["omittedLogs"], 3000)
        r = core.prune_jobs(self.c.private, jobs=jobs)
        left = {p.name.split(".")[0] for p in jobs.iterdir() if p.name.endswith(".json") and not p.name.endswith(".request.json")}
        self.assertLessEqual(len(left), core.KEEP_TOTAL + 2)   # + 没结束的那个 + 本次导出自己的记录（没结束的不动）
        self.assertNotIn(ids[1500], left)
        self.assertFalse((self.c.logs / f"{ids[1500]}.log").exists()); self.assertFalse((self.c.logs / f"{ids[1500]}-install.log").exists())
        self.assertTrue((self.c.logs / f"{ids[0]}.log").exists())
        self.assertTrue((self.c.logs / f"{ids[1999]}.log").exists())   # 没结束的任务的日志不动
        self.assertGreaterEqual(r["logsRemoved"], 2 * (2000 - core.KEEP_TOTAL - 1))

    def test_diagnostics_export_includes_latest_phone_pull_redacted(self):
        """先 diag pull 再 diag export：包里要有最近一次拉回的手机日志（原先导出只收 logs/ 和任务记录，玩家照说明交上来的包里没有手机日志），
        走和 logs/ 同一套脱敏，manifest 记拉回时刻与来源机型；更早那次拉回不收；不认识的类型跳过并写明"""
        import unittest.mock as um, diag, os, time as t
        old = self.c.private / "diagnostics" / "0101-000000-old"; old.mkdir(parents=True)
        (old / "stale.log").write_text("OLD PULL")
        os.utime(old, (t.time() - 3600, t.time() - 3600))
        devices = {"result": {"devices": [{"identifier": "D1", "hardwareProperties": {"platform": "iOS", "udid": "U1", "marketingName": "iPhone 17 Pro Max"},
                                           "deviceProperties": {"name": "合成甲的iPhone", "osVersionNumber": "27.3"}, "connectionProperties": {"tunnelState": "connected"}}]}}
        apps = {"result": {"apps": [{"bundleIdentifier": "io.s2arm.games.abcde12345", "name": "S2Arm"}]}}
        def fake(c, *args, timeout=120):
            if args[:2] == ("list", "devices"): return 0, devices, ""
            if args[:3] == ("device", "info", "apps"): return 0, apps, ""
            source, target = args[args.index("--source") + 1], Path(args[args.index("--destination") + 1])
            if source.endswith("host.log"): target.write_text("crash for synth@example.invalid DEVELOPMENT_TEAM = ABCDE12345\n"); return 0, {}, ""
            if source.endswith("Documents/logs"): target.mkdir(); (target / "host-1.log").write_text("previous run\n"); return 0, {}, ""
            if source.endswith("Documents/diag"): target.mkdir(); (target / "probe.json").write_text('{"ok": 1}'); (target / "blob.bin").write_bytes(b"\0\1"); return 0, {}, ""
            return 1, {}, ""
        with um.patch.object(diag, "devicectl", fake):
            diag.pull(self.c)
        destination = self.root / "diagnostics.zip"; self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            names = archive.namelist()
            phone = [n for n in names if n.startswith("phone/")]
            host = [n for n in phone if n.endswith("/host.log")]
            self.assertEqual(len(host), 1, names)
            text = archive.read(host[0]).decode()
            self.assertIn("crash for", text); self.assertNotIn("synth@example.invalid", text); self.assertNotIn("ABCDE12345", text)   # 同 logs/ 脱敏
            self.assertTrue(any(n.endswith("/logs/host-1.log") for n in phone)); self.assertTrue(any(n.endswith("/diag/probe.json") for n in phone))
            self.assertFalse(any("stale" in n for n in names))   # 只收最近一次拉回
            m = json.loads(archive.read("manifest.json"))["phone_diagnostics"]
            self.assertTrue(m["pulledAt"]); self.assertEqual(m["devices"][0]["model"], "iPhone 17 Pro Max"); self.assertEqual(m["devices"][0]["os"], "27.3")
            self.assertNotIn("合成甲", json.dumps(m, ensure_ascii=False)); self.assertNotIn("U1", json.dumps(m))   # 不记设备名、设备号
            self.assertEqual(m["files"], 3); self.assertTrue(any(x.endswith("diag/blob.bin") for x in m["skipped"]))

    def test_diagnostics_collect_recent_mac_crash_reports(self):
        """诊断#43f7d3：宿主起局 1 秒内 SIGTRAP、没走到写日志，包里没现场。导出时收本机 ~/Library/Logs/DiagnosticReports 里我们进程的 .ips。
        期望手写：收 s2arm-host / S2ArmApple 近 7 天的两份（脱敏）；8 天前的、别的 App 的不收；超 4 MB 的跳过并写名字；manifest 计数对"""
        import unittest.mock as um, os, sys, time as t
        rep = self.root / "DiagnosticReports"; rep.mkdir()
        (rep / "s2arm-host-2026-11-14-101500.ips").write_text('{"exception": "EXC_BREAKPOINT", "path": "/Users/Synthetic/x", "mail": "synth@example.invalid"}')
        (rep / "S2ArmApple-2026-11-14-101600.ips").write_text('{"app": "launcher"}')
        old = rep / "s2arm-host-2026-11-01-090000.ips"; old.write_text("old"); os.utime(old, (t.time() - 8 * 86400, t.time() - 8 * 86400))
        (rep / "Finder-2026-11-14-101700.ips").write_text("other app")
        big = rep / "s2arm-host-2026-11-14-101800.ips"; big.write_bytes(b"x" * (library.MAC_CRASH_FILE_LIMIT + 1))
        destination = self.root / "diagnostics.zip"; self.c.r["path"] = str(destination)
        with um.patch.object(library, "MAC_CRASH_DIR", rep), um.patch.object(sys, "platform", "darwin"):
            library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            crash = sorted(n for n in archive.namelist() if n.startswith("mac_crash/"))
            self.assertEqual(crash, ["mac_crash/S2ArmApple-2026-11-14-101600.ips", "mac_crash/s2arm-host-2026-11-14-101500.ips"])
            text = archive.read("mac_crash/s2arm-host-2026-11-14-101500.ips").decode()
            self.assertIn("EXC_BREAKPOINT", text); self.assertNotIn("synth@example.invalid", text); self.assertNotIn("/Users/Synthetic", text)
            m = json.loads(archive.read("manifest.json"))["mac_crash_reports"]
            self.assertEqual((m["found"], m["files"], m["skipped"]), (3, 2, ["s2arm-host-2026-11-14-101800.ips"]))

    def test_diagnostic_manifest_and_every_member_are_redacted_without_truncation(self):
        import platform
        logs = self.c.logs; logs.mkdir(exist_ok=True)
        sensitive = ('contact=synth@example.invalid; DEVELOPMENT_TEAM = ABCDE12345; '
                     's2arm.abcde12345.0100aabbccdd0000; 0000abcd-0123456789abcdef; '
                     'device 0123456789abcdef0123456789abcdef01234567; '
                     '"合成甲的iPhone"; /Users/Synthetic/account')
        # Invalid UTF-8 must not bypass text redaction, and long data must not
        # lose its prefix through Context.redact's UI-only 3000-character cap.
        (logs / "synth@example.invalid.log").write_bytes(("BEGIN " + "x" * 4000 + "\n" + sensitive + "\nEND").encode() + b"\xff")
        destination = self.root / "diagnostics.zip"; self.c.r["path"] = str(destination)
        def probe(args, **kwargs):
            value = "x" * 4000 + "\n" + sensitive if args[-1] == "hw.model" else "Xcode 27.1\nBuild version SYNTH"
            return subprocess.CompletedProcess(args, 0, value, "")
        with patch.object(platform, "mac_ver", return_value=("27.1", (), "")), patch.object(subprocess, "run", side_effect=probe):
            library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["files"], sorted(archive.namelist()))
            self.assertEqual(manifest["macos_version"], "27.1")
            self.assertGreater(len(manifest["mac_model"]), 4000)
            self.assertEqual(manifest["xcode_version"], "Xcode 27.1\nBuild version SYNTH")
            self.assertIn("error", manifest["launcher_version"])
            log = archive.read("logs/[EMAIL].log").decode()
            self.assertTrue(log.startswith("BEGIN ")); self.assertGreater(len(log), 4000)
            self.assertTrue(log.endswith("END�"))
            text = "\n".join(archive.namelist()) + "\n" + "\n".join(archive.read(n).decode() for n in archive.namelist())
            for secret in ("/Users/", "synth@example.invalid", "ABCDE12345", "abcde12345", "0000abcd-0123456789abcdef",
                           "0123456789abcdef0123456789abcdef01234567", "合成甲"):
                self.assertNotIn(secret, text)
            self.assertIn("0100aabbccdd0000", text)

    def test_diagnostic_uuid_task_members_are_unique_and_manifest_matches(self):
        tasks = ("11111111-2222-4333-8444-555555555555",
                 "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")
        logs = self.c.logs
        logs.mkdir(exist_ok=True)
        expected = set()
        for task in tasks:
            (self.c.logs / "jobs" / (task + ".json")).write_text('{"state":"done"}')
            (logs / (task + "-game.log")).write_text("serial=" + task)
            folder = logs / task
            folder.mkdir()
            (folder / "x.log").write_text("synthetic nested log")
            # Preserving the generated task prefix must not preserve a private suffix.
            (logs / (task + "-synth@example.invalid.log")).write_text("synthetic log")
            expected.update(("jobs/" + task + ".txt", "logs/" + task + "-game.log",
                             "logs/" + task + "/x.log", "logs/" + task + "[EMAIL].log"))
        destination = self.root / "diagnostics.zip"
        self.c.r["path"] = str(destination)
        with patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "synthetic", "")):
            library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as archive:
            members = archive.namelist()
            self.assertEqual(len(members), len(set(members)))
            self.assertTrue(expected.issubset(set(members)))
            for task in tasks:
                self.assertEqual(archive.read("logs/" + task + "-game.log").decode(), "serial=[REDACTED]")
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["files"], sorted(members))
            self.assertNotIn("synth@example.invalid", "\n".join(members) + archive.read("manifest.json").decode())

    def test_diagnostic_manifest_probe_errors_and_redacted_name_collision(self):
        import platform
        destination = self.root / "diagnostics.zip"; self.c.r["path"] = str(destination)
        for result in (subprocess.CompletedProcess([], 7, "", "synthetic error"),
                       subprocess.CompletedProcess([], 0, "", ""),
                       subprocess.TimeoutExpired([], 10), OSError("synthetic unavailable")):
            effect = {"side_effect": result} if isinstance(result, Exception) else {"return_value": result}
            with self.subTest(result=str(result)), patch.object(platform, "mac_ver", return_value=("", (), "")), patch.object(subprocess, "run", **effect):
                library.diagnostics(self.c)
            with zipfile.ZipFile(destination) as archive:
                manifest = json.loads(archive.read("manifest.json"))
                for key in ("macos_version", "xcode_version", "mac_model"):
                    self.assertIn("error", manifest[key])
        previous = destination.read_bytes()
        logs = self.c.logs; logs.mkdir(exist_ok=True)
        for name in ("one@example.invalid.log", "two@example.invalid.log"):
            (logs / name).write_text("synthetic log")
        # #457：两个文件脱敏后同名，原先整包不生成；现在后一个加序号照样收进去，内容都在
        library.diagnostics(self.c)
        self.assertNotEqual(destination.read_bytes(), previous)
        with zipfile.ZipFile(destination) as archive:
            same = sorted(n for n in archive.namelist() if n.startswith("logs/") and n.endswith(".log") and "example.invalid" not in n)
            self.assertEqual(len(same), 2, archive.namelist())
            self.assertTrue(same[0].endswith(" (2).log") or same[1].endswith(" (2).log"), same)
            self.assertEqual({archive.read(n).decode() for n in same}, {"synthetic log"})

    def test_diagnostic_export_failure_names_the_step_and_reason_but_no_path(self):
        # #427：游戏库在 SMB 共享盘上导不出，原先只有一句「无法安全读取或写入」，看不出卡在哪一步。期望手写：所选位置的上一级是个普通文件、建不了文件夹
        #   → 建临时文件那一步失败（#917 起上一级不存在会自动建，不再拿「文件夹不在」触发）
        blocker = self.root / "是个文件"; blocker.write_text("x")
        destination = blocker / "诊断.zip"; self.c.r["path"] = str(destination)
        with self.assertRaises(Failure) as caught:
            library.diagnostics(self.c)
        message = str(caught.exception)
        self.assertIn("无法安全读取或写入诊断文件，未更新诊断包（在所选位置建临时文件：", message)
        self.assertIn("错误码", message)
        self.assertNotIn(str(self.root), message)   # 不带路径（路径里可能有用户名，摘要会贴到公开帖子）

    def test_diagnostic_export_never_contains_keys_or_player_files(self):
        self.c.keys.parent.mkdir(parents=True)
        self.c.keys.write_text("header_key = " + "a" * 64)
        log = self.c.logs / "tool.log"
        log.parent.mkdir(exist_ok=True)
        log.write_text("token=verysecret " + str(self.c.private) + " " + "a" * 64)
        destination = self.root / "diagnostics.zip"
        self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as z:
            self.assertFalse(any("keys" in name or "sources" in name or "saves" in name for name in z.namelist()))
            text = "\n".join(z.read(name).decode() for name in z.namelist())
            self.assertNotIn("verysecret", text)
            self.assertNotIn("a" * 64, text)
            self.assertNotIn(str(self.c.private), text)

    def test_diagnostic_export_keeps_whole_game_log_and_reports_clipping(self):
        # #41：游戏日志整份进诊断包（原先只留头尾各 16 KB + 心跳行，存档前后的帧读数整段丢）；只有超过单份上限才截，截时尾多头少，manifest 写明截了多少
        import unittest.mock as um
        rows = [f"[帧] 第 {i} 帧 18.2 ms 热状态 0\n" for i in range(120000)]   # 约 4 MB，跨好几个 1 MiB 读块
        mid = len(rows) // 2
        rows[mid] = "存档：写入 slot0 完成 token=verysecret\n"
        log = self.c.logs / "0100-game.log"; log.parent.mkdir(exist_ok=True)
        body = "".join(rows).encode()
        # 一行敏感信息正好跨 1 MiB 读块边界：逐块脱敏不能把它拦腰切开
        at = library.DIAGNOSTIC_LOG_CHUNK - 10
        body = body[:at] + b"\ntoken=boundarysecret\n" + body[at:]
        log.write_bytes(body)
        destination = self.root / "diagnostics.zip"; self.c.r["path"] = str(destination)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as z:
            text = z.read("logs/0100-game.log").decode()
            manifest = json.loads(z.read("manifest.json"))
        for i in (0, mid - 1, mid + 1, len(rows) - 1):
            self.assertIn(f"[帧] 第 {i} 帧 ", text)
        self.assertIn("存档：写入 slot0 完成", text)
        self.assertNotIn("verysecret", text); self.assertNotIn("boundarysecret", text)
        self.assertEqual(manifest["truncated"], [])
        # 超过上限（这里调成 1 MB）：开头 1/8、结尾 7/8，manifest 记原长、留下、截掉
        with um.patch.object(library, "DIAGNOSTIC_LOG_HEAD", 128 << 10), um.patch.object(library, "DIAGNOSTIC_LOG_TAIL", 896 << 10):
            destination.unlink(); library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as z:
            text = z.read("logs/0100-game.log").decode()
            cut = json.loads(z.read("manifest.json"))["truncated"]
        self.assertEqual([x["file"] for x in cut], ["logs/0100-game.log"])
        self.assertEqual(cut[0]["bytes"], len(body)); self.assertEqual(cut[0]["kept"] + cut[0]["omitted"], len(body))
        self.assertTrue((1 << 20) - 200 <= cut[0]["kept"] <= 1 << 20, cut[0])
        self.assertIn("[帧] 第 0 帧 ", text); self.assertIn(f"[帧] 第 {len(rows) - 1} 帧 ", text)
        self.assertIn(f"中间截掉 {cut[0]['omitted']} 字节", text)
        tail_part = text[text.index("以下是日志结尾"):]
        self.assertGreater(len(tail_part.encode()), 6 * len(text[:text.index("中间截掉")].encode()))   # 尾部留得比头部多

    def test_diagnostic_export_brings_diag_run_pictures(self):
        # #438 诊断局：游戏文件夹 diagrun/ 里的 PNG 原样进诊断包 diagrun/<游戏号>_<原名>；单张超过上限的跳过、别的文件不带。期望（手写）
        import unittest.mock as um
        gid = "0100000000010000"
        atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id=gid, name="Alpha", packages=[])]))
        run = layout.game_dir(self.c, gid, english="Alpha") / "diagrun"; run.mkdir()
        (run / "shot_3600.png").write_bytes(b"\x89PNG small")
        (run / "td_100.png").write_bytes(b"\x89PNG" + b"x" * 2000)
        (run / "notes.txt").write_text("不带")
        destination = self.root / "diagnostics.zip"; self.c.r["path"] = str(destination)
        with um.patch.object(library, "DIAG_RUN_FILE_LIMIT", 1024):
            library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as z:
            names = [n for n in z.namelist() if n.startswith("diagrun/")]
            self.assertEqual(names, [f"diagrun/{gid}_shot_3600.png"])
            self.assertEqual(z.read(names[0]), b"\x89PNG small")

    def test_diagnostic_fd_boundaries_and_directory_replacement(self):
        import unittest.mock as um
        destination = self.root / "diagnostics.zip"; destination.write_bytes(b"old target")
        self.c.r["path"] = str(destination)
        external = self.root / "external"; external.mkdir()
        (external / "outside.log").write_text("OUTSIDE_BOUNDARY_SENTINEL")
        (external / "outside.json").write_text("OUTSIDE_BOUNDARY_SENTINEL")
        self.c.logs.mkdir(exist_ok=True)
        # Static root links are rejected before begin can persist a job through them.
        for root in (self.c.logs / "jobs", self.c.logs):
            saved = root.with_name(root.name + "-saved")
            root.rename(saved); root.symlink_to(external, target_is_directory=True)
            with um.patch.object(self.c, "begin") as begin, self.assertRaises(Failure):
                library.diagnostics(self.c)
            begin.assert_not_called()
            self.assertEqual(sorted(p.name for p in external.iterdir()), ["outside.json", "outside.log"])
            root.unlink(); saved.rename(root)
            self.assertEqual(destination.read_bytes(), b"old target")
        jobs, logs = self.c.logs / "jobs", self.c.logs
        (jobs / "original.json").write_text('{"detail":"ORIGINAL_JOB"}')
        (logs / "nested").mkdir(); (logs / "nested/original.log").write_text("ORIGINAL_LOG")
        (jobs / "link.json").symlink_to(external / "outside.json")
        (logs / "link.log").symlink_to(external / "outside.log")
        (logs / "linked-directory").symlink_to(external, target_is_directory=True)
        os.mkfifo(jobs / "pipe.json"); os.mkfifo(logs / "pipe.log")
        roots = {p.stat().st_ino: p for p in (jobs, logs)}
        listed = os.listdir; replaced = []
        def replace_after_listing(fd):
            names = listed(fd)
            root = roots.get(os.fstat(fd).st_ino) if isinstance(fd, int) else None
            if root and root not in replaced:
                root.rename(root.with_name(root.name + "-saved"))
                root.symlink_to(external, target_is_directory=True)
                replaced.append(root)
            return names
        with um.patch.object(os, "listdir", side_effect=replace_after_listing):
            library.diagnostics(self.c)
        self.assertEqual(len(replaced), 2)
        with zipfile.ZipFile(destination) as archive:
            self.assertIn("ORIGINAL_JOB", archive.read("jobs/original.txt").decode())
            self.assertEqual(archive.read("logs/nested/original.log"), b"ORIGINAL_LOG")
            self.assertFalse(any("link" in name or "pipe" in name or "outside" in name for name in archive.namelist()))
            self.assertNotIn("OUTSIDE_BOUNDARY_SENTINEL", "".join(archive.read(name).decode() for name in archive.namelist()))

    def test_diagnostic_log_stops_at_size_measured_when_opened(self):
        # D-4PE6（11-17）：游戏开着、日志边导边涨，原先读到文件尾，导出慢时一份 game.log 涨过收件单个成员 16 MB 上限、整包被拒。
        #   期望手写：打开时量的是 size，之后多写进来的不收——不截断（size ≤ 头 + 尾）时整份正好 size 字节；截断时结尾段止于 size
        grown = b"x" * 9 + b"\n"   # 打开后才写进来的
        short = b"line\n" * 100
        self.assertEqual(library.diagnostic_log_excerpt(io.BytesIO(short + grown * 1000), len(short), 16000, 16000), short)
        body = b"".join(f"{i:07d} payload\n".encode() for i in range(10000))   # 16 字节一行，160000 字节
        out = library.diagnostic_log_excerpt(io.BytesIO(body + grown * 100000), len(body), 16000, 16000)
        self.assertTrue(out.endswith(b"0009999 payload\n"), out[-40:])
        self.assertNotIn(grown, out)

    def test_diagnostic_log_keeps_head_heartbeats_and_tail(self):
        import io
        short = b"a" * 32000
        self.assertEqual(library.diagnostic_log_excerpt(io.BytesIO(short), len(short), 16000, 16000), short)
        beat = "[心跳] 帧 {} 热状态 0\n".format
        head = b"HEAD_MARK\n" + b"h" * 16000
        noise = b"NOISE_LINE padding padding\n" * 80000   # 约 2.2 MB，心跳行跨 1 MiB 分块
        body = head + noise + beat(1).encode() + noise + beat(2).encode() + noise + b"t" * 16000 + b"\nTAIL_MARK\n"
        out = library.diagnostic_log_excerpt(io.BytesIO(body), len(body), 16000, 16000)
        self.assertTrue(out.startswith(b"HEAD_MARK")); self.assertTrue(out.endswith(b"TAIL_MARK\n"))
        self.assertIn(beat(1).strip().encode(), out); self.assertIn(beat(2).strip().encode(), out)
        self.assertLess(out.count(b"NOISE_LINE"), 1200)   # 只有开头 / 结尾 16 KB 内可能带到
        # 心跳太多：只留最新的，总量有界
        many = b"".join(beat(i).encode() for i in range(20000))
        body = b"h" * 16000 + many + b"t" * 16000
        out = library.diagnostic_log_excerpt(io.BytesIO(body), len(body), 16000, 16000)
        self.assertIn(beat(19999).strip().encode(), out); self.assertNotIn(beat(0).strip().encode(), out)
        self.assertLessEqual(len(out), 16000 * 2 + 64000 + 200)

    def test_diagnostic_log_keeps_crash_scene_in_the_middle(self):
        # #141：28 万字节的 game.log，崩溃摘要与全线程现场在中段（只留头尾时整段丢，玩家交了诊断包也定不了因）。
        #   现场格式同 runtime/src/crash.c（【红】CRASH 摘要）+ tdump.c（「[现场] 」前缀，===== 全线程现场 起、===== 现场完 ===== 止）
        import io
        def thread(i, n, state, frames):
            rows = [f"[现场] 线程 {i}/{n} tid {9000 + i}「worker-{i}」{state} 优先级 31 QoS UI 用户态 1.00 s", f"[现场]     pc libS2A.dylib+{0x1000 + i:#x}", f"[现场]     lr libS2A.dylib+{0x2000 + i:#x}"]
            rows += [f"[现场]     #{k} {frames}+{0x100 * k + i:#x}" for k in range(12)]
            rows += [f"[现场]     x{r:<2d} " + " ".join(["%016x" % (r * 1000 + i)] * 4) for r in range(0, 29, 4)]
            return rows
        def scene(n, crash_at, waiting="等待"):
            rows = ["【红】CRASH 信号 11 地址 0x10 pc main+0x1234 lr main+0x1200 sp 0x16f000000", "  x0  0000000000000000 0000000000000001 0000000000000002 0000000000000003",
                    "崩溃时余量：低区空洞合计 512 MB、最大一块 256 MB；文件句柄已开 40 个，软上限 256、硬上限 10240",
                    f"[现场] ===== 全线程现场：崩溃 信号 11（{n} 条线程）====="]
            for i in range(1, n + 1):
                rows += thread(i, n, "在跑" if i == crash_at else waiting, "CRASHFRAME" if i == crash_at else "idle")
            return rows + ["[现场] ===== 现场完 ====="]
        noise = "".join(f"[log] 第 {i} 行普通日志 padding padding padding\n" for i in range(2200)).encode()
        body = b"HEAD_MARK\n" + noise + "\n".join(scene(30, 17)).encode() + b"\n" + noise + b"\nTAIL_MARK\n"
        self.assertTrue(250000 < len(body) < 330000, len(body))
        out = library.diagnostic_log_excerpt(io.BytesIO(body), len(body), 16000, 16000)
        for want in ("【红】CRASH 信号 11", "全线程现场：崩溃 信号 11", "线程 17/30 tid 9017「worker-17」在跑", "#11 CRASHFRAME+0xb11", "===== 现场完 ====="):
            self.assertIn(want.encode(), out, want)
        self.assertTrue(out.startswith(b"HEAD_MARK")); self.assertTrue(out.endswith(b"TAIL_MARK\n"))
        # 现场块超出保留上限：总量有界；崩溃摘要、第一个非等待线程（崩溃线程）的整段栈、其余线程头一行、现场完都在
        body = b"h" * 16000 + noise + "\n".join(scene(600, 333)).encode() + b"\n" + b"t" * 16000
        out = library.diagnostic_log_excerpt(io.BytesIO(body), len(body), 16000, 16000)
        self.assertLessEqual(len(out), 16000 * 2 + 64000 + library.DIAGNOSTIC_LOG_SCENE + 600)
        for want in ("【红】CRASH 信号 11", "线程 333/600 tid 9333「worker-333」在跑", f"#11 CRASHFRAME+{0x100 * 11 + 333:#x}", "线程 1/600 tid 9001", "===== 现场完 ====="):
            self.assertIn(want.encode(), out, want)
        self.assertNotIn(b"#11 idle+0xb01", out)   # 等待线程的栈让位给崩溃线程

    def customize_game(self, **fields):
        gid = "0100000000010000"
        cat = self.c.catalog()
        if not any(g["id"] == gid for g in cat["games"]):
            atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id=gid, name="原名", display="", packages=[], hidden=False)]))
        request = dict(self.request, taskId=str(uuid.uuid4()), operation="customize", gameId=gid, **fields)
        return library.customize(Context(request)), gid

    def test_customize_name_and_cover_are_display_only_and_restorable(self):
        png = self.root / "my.png"; png.write_bytes(b"\x89PNG\r\n\x1a\n" + b"x" * 100)
        result, gid = self.customize_game(name="  我的游戏  ", coverPath=str(png))
        self.assertEqual(result["name"], "我的游戏")
        folder = self.c.library / "games" / gid
        covers = list(folder.glob("cover.custom.*.jpg"))
        self.assertEqual(len(covers), 1); self.assertEqual(covers[0].read_bytes(), png.read_bytes())
        self.assertEqual(self.c.catalog()["games"][0]["name"], "原名")   # 只改显示，不改登记名
        rendered = library.game_custom(self.c, gid)
        self.assertEqual((rendered["name"], rendered["cover"]), ("我的游戏", covers[0]))
        # 只改名字：封面不动
        self.customize_game(name="第二个名字")
        self.assertEqual(list(folder.glob("cover.custom.*.jpg")), covers)
        # 换图：文件名变（两端按路径缓存），旧图删掉
        jpg = self.root / "my.jpg"; jpg.write_bytes(b"\xff\xd8\xff" + b"y" * 100)
        self.customize_game(coverPath=str(jpg))
        after = list(folder.glob("cover.custom.*.jpg"))
        self.assertEqual(len(after), 1); self.assertNotEqual(after, covers)
        self.assertEqual(library.game_custom(self.c, gid)["name"], "第二个名字")
        # 恢复默认
        self.customize_game(name="", coverPath="")
        self.assertEqual(list(folder.glob("cover.custom.*.jpg")), [])
        self.assertFalse((folder / "custom.json").exists())
        self.assertEqual(library.game_custom(self.c, gid), dict(name="", cover=None))

    def test_customize_rejects_bad_input_and_keeps_previous(self):
        self.customize_game(name="好名字")
        bad = self.root / "bad.txt"; bad.write_bytes(b"not an image")
        huge = self.root / "huge.png"; huge.write_bytes(b"\x89PNG\r\n\x1a\n" + b"x" * (16 << 20))
        for fields in (dict(name="x" * 65), dict(name="换\n行"), dict(name=3), dict(coverPath=str(bad)), dict(coverPath=str(huge)), dict(coverPath=str(self.root / "none.png"))):
            with self.assertRaises(Failure, msg=str(list(fields))):
                self.customize_game(**fields)
        custom = library.game_custom(self.c, "0100000000010000")
        self.assertEqual((custom["name"], custom["cover"]), ("好名字", None))

    def test_container_custom_lists_custom_and_default_entries(self):
        import ipa
        png = self.root / "my.png"; png.write_bytes(b"\x89PNG\r\n\x1a\n" + b"x")
        _, gid = self.customize_game(name="新名", coverPath=str(png))
        item = ipa.container_custom(self.c)[gid]
        self.assertEqual((item["custom"], item["name"]), (True, "新名"))
        self.assertIn("cover.custom.", item["cover"])
        self.customize_game(name="", coverPath="")
        self.assertEqual(ipa.container_custom(self.c)[gid], dict(custom=False, name="原名", cover=""))

    def test_ship_ext_applies_custom_and_restores_default(self):
        import importlib.util, json as j
        spec = importlib.util.spec_from_file_location("container_custom_under_test", ROOT / "pack/container_custom.py")
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        app = self.root / "S2Arm.app"; game = app / "Games" / "A"; game.mkdir(parents=True); (game / "icon.jpg").write_bytes(b"old")
        cover = self.root / "c.jpg"; cover.write_bytes(b"custom")
        spec_file = self.root / "custom.json"
        entries = [dict(title_id="A", name="原名"), dict(title_id="B", name="别的款")]
        spec_file.write_text(j.dumps(dict(A=dict(custom=True, name="新名", cover=str(cover)))), encoding="utf-8")
        module.apply_custom(app, entries, str(spec_file), "Games")
        self.assertEqual(entries, [dict(title_id="A", name="新名", icon="Games/A/icon.jpg", customized=True), dict(title_id="B", name="别的款")])
        self.assertEqual((game / "icon.jpg").read_bytes(), b"custom")
        # 恢复默认：带着旧自定义的条目换回原名原图、去掉标记；没自定义过的条目不碰
        original = self.root / "o.jpg"; original.write_bytes(b"original")
        spec_file.write_text(j.dumps(dict(A=dict(custom=False, name="原名", cover=str(original)), B=dict(custom=False, name="库里的名字", cover=""))), encoding="utf-8")
        module.apply_custom(app, entries, str(spec_file), "Games")
        self.assertEqual(entries, [dict(title_id="A", name="原名", icon="Games/A/icon.jpg"), dict(title_id="B", name="别的款")])
        self.assertEqual((game / "icon.jpg").read_bytes(), b"original")

    def stopped_game(self, kit=True, ipa=True):
        gid = "0100000000010000"
        atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id=gid, name="x", display="", packages=[], hidden=False)]))
        out = layout.ios_work(self.c, gid); root = out / "roots" / "k@pkg"; root.mkdir(parents=True)   # 出包工作目录 build/ios
        (root / "main.npdm").write_bytes(b"n"); (root / "stage.log").write_text("阶段日志")
        (out / "roots" / "k@现行").symlink_to("k@pkg"); (layout.build_dir(self.c, gid) / "x").mkdir(parents=True)
        (out / "logs").mkdir(); (out / "logs" / "oneclick.log").write_text("出包日志")
        image = layout.image_ios(self.c, gid)   # 上一次成功出包的交付件
        if ipa: (image / ".pack").mkdir(parents=True); (image / ".pack" / "manifest.json").write_text('{"index_sha256": "p"}')
        if kit: (image / "kit").mkdir(parents=True); (image / "kit" / "main.npdm").write_bytes(b"old"); (image / "kit" / "main.fixups.tsv").write_text("")
        image = layout.path(self.c, gid, "image"); (image / "mac").mkdir(parents=True); (image / "mac.partial").mkdir()   # 出了一半的新镜像
        save = layout.path(self.c, gid, "saves") / "s.bin"; save.parent.mkdir(parents=True); save.write_bytes(b"save")
        return gid, out, save

    def stop_context(self, gid):
        return Context(dict(self.request, taskId=str(uuid.uuid4()), operation="ipa", gameId=gid))

    def test_clean_after_stop_keeps_previous_delivery_and_logs(self):
        gid, out, save = self.stopped_game()
        c = self.stop_context(gid)
        library.clean_after_stop(c)
        image = layout.image_ios(self.c, gid)
        self.assertEqual(sorted(p.name for p in image.iterdir()), [".pack", "kit"])   # 旧位置的程序件：清理不碰，真用到时才搬进程序常驻（pack/jobs/programs.py adopt）
        self.assertEqual(sorted(p.name for p in layout.build_dir(self.c, gid).iterdir()), ["ios"])   # 只剩出包日志
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["logs"])
        self.assertEqual((image / "kit" / "main.npdm").read_bytes(), b"old")
        self.assertEqual(save.read_bytes(), b"save")
        self.assertEqual((c.logs / (c.task + "-stages") / "stage.log").read_text(), "阶段日志")

    def test_clean_after_stop_keeps_failed_stage_logs(self):   # #251：改写失败的 .failed/<款>_main.log 在游戏库缓存里，清理前要拷进私有日志目录，诊断包才带得上
        gid, out, _ = self.stopped_game()
        failed = layout.build_dir(self.c, gid) / "x" / "roots" / ".failed"; failed.mkdir(parents=True)
        (failed / f"{gid}_main.log").write_text("改写超时前最后一行")
        c = self.stop_context(gid)
        library.clean_after_stop(c)
        self.assertFalse(failed.exists())
        self.assertEqual((c.logs / (c.task + "-stages") / f"{gid}_main.log").read_text(), "改写超时前最后一行")

    def test_clean_after_stop_removes_first_failed_export(self):
        gid, out, _ = self.stopped_game(kit=False, ipa=False)
        library.clean_after_stop(self.stop_context(gid))
        self.assertEqual([p.name for p in out.iterdir()], ["logs"])   # 第一次出包就失败：中间产物全清，出包日志留着

    def test_clean_after_stop_keeps_only_source_of_old_install(self):
        gid, out, _ = self.stopped_game(kit=False)
        library.clean_after_stop(self.stop_context(gid))
        self.assertTrue((out / "roots" / "k@pkg" / "main.npdm").is_file())   # 老玩家：没有容器件时产物根是装机的唯一来源

    def test_sweep_skips_while_export_runs_and_cleans_when_idle(self):
        gid, out, _ = self.stopped_game()
        c = self.stop_context(gid)
        with c.lock("execution"):
            library.sweep_residue(c)
        self.assertTrue((out / "roots").exists())
        library.sweep_residue(c)
        self.assertFalse((out / "roots").exists())
        self.assertEqual(sorted(p.name for p in layout.path(self.c, gid, "image").iterdir()), ["ios", "mac"])   # 半成品 mac.partial 清掉，交付件 image/ios 不动

    def test_catalog_sweeps_and_prunes_only_on_startup_refresh(self):
        # 剃刀 H14：残留清扫与任务目录清理只在启动器打开后那次刷新（startup: true）做；之后的刷新（装机页、导入完、切回窗口）都不做。
        #   用真残留夹具：普通刷新后残留还在、清理没调；startup 刷新后残留清掉、清理调了一次
        import unittest.mock as um
        gid, out, _ = self.stopped_game()
        pruned = []
        with um.patch.object(library, "prune_jobs", lambda *a, **k: pruned.append(1)):
            library.catalog(self.c)
            self.assertTrue((out / "roots").exists())
            self.assertEqual(pruned, [])
            self.c.r["startup"] = True
            try:
                library.catalog(self.c)
            finally:
                self.c.r.pop("startup", None)
        self.assertFalse((out / "roots").exists())
        self.assertEqual(pruned, [1])

    def test_sweep_without_residue_never_takes_execution_lock(self):
        """读游戏库（catalog）每次都扫：没有残留时不碰执行锁，否则同时开始的任务当场撞锁（11-09 main 守望红 SaveManagementTests）"""
        import unittest.mock as um
        gid, out, _ = self.stopped_game()
        c = self.stop_context(gid)
        library.sweep_residue(c)            # 先把残留清干净
        with um.patch.object(type(c), "idle", side_effect=AssertionError("没有残留却去拿执行锁")):
            library.sweep_residue(c)
        self.assertEqual(sorted(p.name for p in layout.path(self.c, gid, "image").iterdir()), ["ios", "mac"])   # 半成品 mac.partial 清掉，交付件 image/ios 不动

    def test_diagnostic_bounded_reads_and_atomic_publication(self):
        import unittest.mock as um
        logs = self.c.logs; logs.mkdir(exist_ok=True)
        log = logs / "tail.log"; log.write_bytes(b"x" * 40000 + b"tail")
        destination = self.root / "diagnostics.zip"; destination.write_bytes(b"old target")
        self.c.r["path"] = str(destination)
        marker = self.root / "outside-marker"; marker.write_bytes(b"unchanged marker")
        predictable = destination.with_name("." + destination.name + "." + self.c.task + ".partial")
        predictable.symlink_to(marker)
        fdopen, make_zip = os.fdopen, zipfile.ZipFile
        reads, outputs = [], []
        class ReadProbe:
            def __init__(self, stream): self.stream = stream
            def __getattr__(self, name): return getattr(self.stream, name)
            def read(self, size=-1):
                reads.append((os.fstat(self.stream.fileno()).st_ino, self.stream.tell(), size))
                return self.stream.read(size)
        @contextlib.contextmanager
        def tracked_open(fd, mode, *args, **kwargs):
            with fdopen(fd, mode, *args, **kwargs) as stream:
                yield ReadProbe(stream) if mode == "rb" else stream
        def archive(output, *args, **kwargs):
            self.assertEqual(os.fstat(output.fileno()).st_mode & 0o777, 0o600)
            outputs.append(output)
            return make_zip(output, *args, **kwargs)
        with um.patch.object(os, "fdopen", tracked_open), um.patch.object(zipfile, "ZipFile", side_effect=archive):
            library.diagnostics(self.c)
        self.assertTrue(all(output.closed for output in outputs))
        # #41：日志整份读（按块），不再只读头尾；D-4PE6：只读到打开时的大小，一块不超过 DIAGNOSTIC_LOG_CHUNK
        self.assertIn((log.stat().st_ino, 0, min(log.stat().st_size, library.DIAGNOSTIC_LOG_CHUNK)), reads)
        self.assertTrue(all(size <= 1 << 20 or size == (4 << 20) + 1 for _, _, size in reads))
        self.assertEqual(marker.read_bytes(), b"unchanged marker")
        self.assertTrue(predictable.is_symlink()); self.assertFalse(destination.is_symlink())
        # #917：超过 4 MiB 的任务记录不再整包拒绝：留前 4 MiB、manifest truncated 记一笔（期望按造的大小手算：原长 4 MiB+1、留 4 MiB、截 1）
        oversized = self.c.logs / "jobs/oversized.json"
        with oversized.open("wb") as stream: stream.truncate((4 << 20) + 1)
        library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as z:
            cut = [t for t in json.loads(z.read("manifest.json"))["truncated"] if t["file"] == "jobs/oversized.txt"]
            self.assertEqual(cut, [dict(file="jobs/oversized.txt", bytes=(4 << 20) + 1, kept=4 << 20, omitted=1)])
            self.assertIn("jobs/oversized.txt", z.namelist())   # 收进来了（脱敏后的文本长度不等于原字节数，截多少看 manifest）
        oversized.unlink()
        # #917：导出期间一直被替换的文件（查名字与打开时 inode 总不同）：重试三次后跳过、manifest changed 记一笔，其余照收
        busy = self.c.logs / "jobs/busy.json"; busy.write_text("{}")
        real_stat = os.stat
        def flipping(path, *a, **k):
            r = real_stat(path, *a, **k)
            return os.stat_result((r[0], r[1] + 1, *tuple(r)[2:])) if path == "busy.json" else r
        with um.patch.object(library.os, "stat", flipping):
            library.diagnostics(self.c)
        with zipfile.ZipFile(destination) as z:
            m = json.loads(z.read("manifest.json"))
            self.assertEqual(m["changed"], ["jobs/busy.txt"]); self.assertNotIn("jobs/busy.txt", z.namelist())
        busy.unlink()
        # #917：指定路径的上一级还不存在：建好照常写出
        nested = destination.parent / "还没建的" / "子目录" / "包.zip"
        self.c.r["path"] = str(nested); library.diagnostics(self.c); self.assertTrue(zipfile.is_zipfile(nested))
        self.c.r["path"] = str(destination)
        previous = destination.read_bytes()
        with um.patch.object(zipfile.ZipFile, "writestr", side_effect=OSError("synthetic write failure")), self.assertRaises(Failure):
            library.diagnostics(self.c)
        with um.patch.object(self.c, "check", side_effect=Cancelled()), self.assertRaises(Cancelled):
            library.diagnostics(self.c)
        self.assertEqual(destination.read_bytes(), previous)
        self.assertEqual(marker.read_bytes(), b"unchanged marker")
        self.assertEqual(list(self.root.glob("." + destination.name + ".*.partial")), [predictable])

    def test_cli_setup_and_unknown_operation_use_real_worker_protocol(self):
        request = self.request | dict(taskId="cli-test", operation="not-an-operation")
        p = self.root / "request.json"
        atomic(p, request)
        result = subprocess.run([sys.executable, "-I", "-B", str(JOBS / "main.py"), "--request", str(p)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        event = json.loads(result.stdout.splitlines()[-1])
        self.assertEqual(event["jobId"], "cli-test")
        self.assertEqual(event["state"], "failed")
        p.chmod(0o644)   # 11-16 不再自设权限：系统缺省权限的请求照常收（原先拒收、stdout 为空）
        result = subprocess.run([sys.executable, "-I", "-B", str(JOBS / "main.py"), "--request", str(p)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout.splitlines()[-1])["state"], "failed")

    def test_invalid_keys_preserve_the_previous_player_key(self):
        self.c.keys.parent.mkdir(parents=True)
        self.c.keys.write_text("header_key = " + "a" * 64)
        invalid = self.root / "invalid.keys"
        invalid.write_text("header_key = broken")
        self.c.r["path"] = str(invalid)
        with self.assertRaises(Failure):
            library.keys(self.c)
        self.assertIn("a" * 64, self.c.keys.read_text())

    def test_game_file_picked_as_keys_is_named(self):
        # 诊断 D-HZF3：玩家在导入密钥处连选 4 次游戏原件（.nsp / .nsz），只看到「过大」「格式无效」。期望手写：
        #   原件扩展名（大小写不论）一律说「选的是游戏文件」、码 keys_invalid、原有密钥不动；真密钥照旧导入
        self.c.keys.parent.mkdir(parents=True)
        self.c.keys.write_text("header_key = " + "a" * 64)
        for name, body in (("Game [0100000000000000][v0].NSP", b"PFS0" + b"\0" * (2 << 20)), ("Game.nsz", b"PFS0")):
            with self.subTest(name=name):
                f = self.root / name; f.write_bytes(body); self.c.r["path"] = str(f)
                with self.assertRaises(Failure) as e:
                    library.keys(self.c)
                self.assertTrue(str(e.exception).startswith("选的是游戏文件"), str(e.exception)); self.assertEqual(e.exception.code, "keys_invalid")
                self.assertIn("a" * 64, self.c.keys.read_text())

    def test_mapping_rejects_malformed_native_values_and_duplicate_axes(self):
        config = dict(schema=1, keyboard={"A": {"code":14}}, mouse={}, gamepad={}, axes={}, deadzone=.15,
                      mouseOptions=dict(mode="camera", sensitivityX=1, sensitivityY=1, invertX=False, invertY=False, recenter=True))
        self.c.r["config"] = config
        library.save_input(self.c)
        config["keyboard"]["A"]["code"] = "fourteen"
        with self.assertRaises(Failure):
            library.save_input(self.c)
        config["keyboard"] = {}
        config["axes"] = {"LX": {"source":"leftX"}, "LY": {"source":"leftX", "invert":True}}
        with self.assertRaises(Failure):
            library.save_input(self.c)

    def test_save_input_keeps_player_one_file_and_writes_per_player_files(self):
        # 键位按玩家保存（键位设置里选玩家）：玩家 1 / 没给 player 照旧写这款的 input.json；玩家 2–4 写 input-pN.json（起局 build.py 读它交给宿主）；
        #   序号超出 1–4、没带 gameId 存玩家 2 起的键位都拒收
        def mapping(code):
            return dict(schema=1, keyboard={"A": {"code": code}}, mouse={}, gamepad={}, axes={}, deadzone=.15,
                        mouseOptions=dict(mode="camera", sensitivityX=1, sensitivityY=1, invertX=False, invertY=False, recenter=True))
        game = self.game(); self.catalogue([game]); self.c.r["gameId"] = game["id"]
        self.c.r["config"] = mapping(14)
        library.save_input(self.c)
        self.assertTrue(layout.path(self.c, game["id"], "input.json").exists())
        self.c.r["config"] = mapping(36)
        for player in (2, 3, 4):
            self.c.r["player"] = player
            library.save_input(self.c)
            self.assertTrue(layout.path(self.c, game["id"], f"input-p{player}.json").exists())
        self.assertEqual(json.loads(layout.path(self.c, game["id"], "input.json").read_text()), mapping(14))
        self.assertEqual(json.loads(layout.path(self.c, game["id"], "input-p2.json").read_text()), mapping(36))
        for bad in (5, 0, "2"):
            self.c.r["player"] = bad
            with self.assertRaises(Failure):
                library.save_input(self.c)
        self.c.r.pop("gameId")
        self.c.r["player"] = 2   # 默认键位只有玩家 1 的一份：没带 gameId 只能存玩家 1
        with self.assertRaises(Failure):
            library.save_input(self.c)

    def test_frontend_toolchain_is_child_only_and_logged(self):
        previous = os.environ.get("S2A_DEVELOPER_DIR")
        chosen = str(self.root / "chosen Xcode.app/Contents/Developer")
        self.c.r["developerDir"] = chosen
        log = self.root / "toolchain.log"
        self.c.run([sys.executable, "-c", "import os; print('child:' + os.environ['DEVELOPER_DIR'])"], log)
        text = log.read_text()
        self.assertIn("S2Arm frontend toolchain: DEVELOPER_DIR=" + chosen, text)
        self.assertIn("child:" + chosen, text)
        self.assertEqual(os.environ.get("S2A_DEVELOPER_DIR"), previous)

    def test_seed_stage_keeps_dlc_and_does_not_modify_input(self):
        source = self.root / "raw"
        for name in ("exefs/main", "romfs/data", "aoc/1/data", "ctl/control.nacp", "aoc.tsv"):
            p = source / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("seed-stage fixture only")
        env = self.c.env() | dict(S2A_ROOTS=str(self.root / "roots"), S2A_ROOTS_SHARED=str(self.root / "shared"))
        self.c.run(["bash", ROOT / "pack/seed_root.sh", "unit", "seed", source], env=env)
        current = (self.root / "roots/unit@现行").resolve()
        self.assertEqual((current / "aoc/1/data").read_text(), "seed-stage fixture only")
        self.assertEqual((current / "aoc.tsv").read_bytes(), (source / "aoc.tsv").read_bytes())
        (current / "aoc/1/data").write_text("changed clone")
        self.assertEqual((source / "aoc/1/data").read_text(), "seed-stage fixture only")



class WaitReadableTests(unittest.TestCase):
    """开工前读原件（library.wait_readable，10-31 出包管线深修 4.5）：读得到不打扰；系统拒绝说清去哪打开；读卡住时界面上说在等什么、等了多久"""
    def ctx(self):
        from types import SimpleNamespace
        msgs = []
        return SimpleNamespace(emit=lambda kind="progress", message="", **k: msgs.append(message), check=lambda: None), msgs

    def test_readable_passes_silently(self):
        import library
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "base.nsp"; p.write_bytes(b"x" * 5000)
            # poll 取大：读完 join 当场返回、不真等；原来 0.1 秒在门禁负载下起线程 + 读 4 KB 就可能超过，先红后绿（11-15）
            c, msgs = self.ctx(); library.wait_readable(c, [p], poll=30)
            self.assertEqual(msgs, [])

    def test_permission_denied_says_where_to_allow(self):
        import library
        from errors import Failure
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "base.nsp"; p.write_bytes(b"x"); p.chmod(0)
            try:
                c, _ = self.ctx()
                with self.assertRaises(Failure) as e: library.wait_readable(c, [p], poll=0.1)
                self.assertEqual(e.exception.code, "input_permission")
                self.assertIn("隐私与安全性", str(e.exception))
            finally: p.chmod(0o600)

    def test_blocked_read_tells_what_it_waits_for(self):
        import builtins, library, time as _t
        from unittest.mock import patch
        real_open = builtins.open
        def slow_open(*a, **k):   # 读像被授权框挡住一样慢 0.35 秒，之后照常
            _t.sleep(0.35); return real_open(*a, **k)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "base.nsp"; p.write_bytes(b"x")
            c, msgs = self.ctx()
            with patch.object(builtins, "open", slow_open): library.wait_readable(c, [p], poll=0.1)
            self.assertTrue(msgs and all("在等系统的文件访问授权：读 base.nsp" in m for m in msgs), msgs)


class WaitWritableTests(WaitReadableTests):
    """开工前往出包工作目录试写（library.wait_writable，11-09 拷字体失败 + 手测推包挂 33 分钟：外接盘第一次写也弹授权框）：
    写得到不打扰、不留探针；系统拒绝报 permission_denied 并说去哪打开；写卡住时界面上说在等什么"""
    def test_writable_passes_silently_and_leaves_nothing(self):
        import library
        with tempfile.TemporaryDirectory() as d:
            w = Path(d) / "lib" / ".s2arm-work"
            c, msgs = self.ctx(); library.wait_writable(c, w, poll=0.1)
            self.assertEqual((msgs, list(w.iterdir())), ([], []))

    def test_write_denied_says_where_to_allow(self):
        import library
        from errors import Failure
        with tempfile.TemporaryDirectory() as d:
            w = Path(d) / "work"; w.mkdir(); w.chmod(0o500)
            try:
                c, _ = self.ctx()
                with self.assertRaises(Failure) as e: library.wait_writable(c, w, poll=0.1)
                self.assertEqual(e.exception.code, "permission_denied")
                self.assertIn("可移除的宗卷", str(e.exception))
            finally: w.chmod(0o700)

    def test_blocked_write_tells_what_it_waits_for(self):
        import library, time as _t
        from pathlib import Path as P
        from unittest.mock import patch
        real = P.write_bytes
        def slow(self, *a, **k):   # 写像被授权框挡住一样慢 0.35 秒，之后照常
            _t.sleep(0.35); return real(self, *a, **k)
        with tempfile.TemporaryDirectory() as d:
            w = Path(d) / "work"
            c, msgs = self.ctx()
            with patch.object(P, "write_bytes", slow): library.wait_writable(c, w, poll=0.1)
            self.assertTrue(msgs and all("在等系统的文件访问授权：写 work" in m for m in msgs), msgs)

if __name__ == "__main__":
    unittest.main()
