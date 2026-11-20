"""装到 iPhone（pack/jobs/install.py，10-26）：选团队、包名、报错分类、整条流程（签名 / 装机 / 拷资源包打桩，IPA 夹具用 ditto 真解）。期望按规则手写"""
import json, os, plistlib, shutil, subprocess, sys, tempfile, time, types, unittest, zipfile
import jobcase  # noqa: E402（作业上下文替身统一工厂 jobcase.fake_context）
import unittest.mock as um
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import install
from core import Failure, atomic
import tests.test_jobs as tj   # 只借 JobsTests 的夹具；不 from-import 类名，免得 unittest 把它整组再跑一遍
from tests.jobcase import ios_fixture
import layout


class ContainerVariantTest(unittest.TestCase):
    """S2A_CONTAINER_SUFFIX / S2A_CONTAINER_NAME：开发用的并存安装，不设时与玩家一致"""
    def test_suffix_changes_bundle_id_only_when_set(self):
        with um.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("S2A_CONTAINER_SUFFIX", None)
            self.assertEqual(install.container_bundle_id("TEAM1"), "io.s2arm.games.team1")
        with um.patch.dict(os.environ, {"S2A_CONTAINER_SUFFIX": "Feat/iCloud_Save Sync"}):
            self.assertEqual(install.container_bundle_id("TEAM1"), "io.s2arm.games.team1.featicloudsavesync")   # 只留小写字母 / 数字 / 连字符

    def test_display_name_rewrites_plist_strings_only_when_set(self):
        with tempfile.TemporaryDirectory() as d:
            app = Path(d) / "S2Arm.app"; (app / "zh-Hans.lproj").mkdir(parents=True)
            strings = app / "zh-Hans.lproj/InfoPlist.strings"
            strings.write_text('"CFBundleDisplayName" = "S2Arm";\n"Other" = "x";\n', encoding="utf-8")
            with um.patch.dict(os.environ, {}, clear=False):
                os.environ.pop("S2A_CONTAINER_NAME", None)
                self.assertIsNone(install.container_display_name(app))
                self.assertIn('"S2Arm"', strings.read_text(encoding="utf-8"))
            with um.patch.dict(os.environ, {"S2A_CONTAINER_NAME": "S2Arm·iCloud"}):
                self.assertEqual(install.container_display_name(app), "S2Arm·iCloud")
            text = strings.read_text(encoding="utf-8")
            self.assertIn('"CFBundleDisplayName" = "S2Arm·iCloud";', text)
            self.assertIn('"Other" = "x";', text)


class CableFirstTest(unittest.TestCase):
    """线连的优先：插线的一台 + 同网配对的另一台 = 一台（手算：只留 wired=True 的那行）；没有线连的就看网络的"""
    def test_cable_first(self):
        import target
        a, b = dict(id="USB", wired=True), dict(id="NET", wired=False)
        self.assertEqual(target.cable_first([b, a]), [a])
        self.assertEqual(target.cable_first([b]), [b])
        self.assertEqual(target.cable_first([]), [])

    def test_windows_usb_first(self):
        W = type("W", (), {"mux_devices": staticmethod(lambda: [("U1", "USB", 1), ("U2", "Network", 2), ("U1", "Network", 3)])})
        self.assertEqual(install.win_udids(W), ["U1"])
        W2 = type("W", (), {"mux_devices": staticmethod(lambda: [("U2", "Network", 2)])})
        self.assertEqual(install.win_udids(W2), ["U2"])

    def test_macos_usb_names_tolerate_invalid_ioreg_bytes(self):
        import target
        raw = b'"USB Product Name" = "iPad Pro"\n' + b'\xca' * 4 + b'"USB Product Name" = "iPhone 17"'
        def run_process(*args, **kwargs):
            output = raw.decode("utf-8") if kwargs.get("text") else raw
            return types.SimpleNamespace(stdout=output)
        with um.patch.object(target.subprocess, "run", side_effect=run_process) as run:
            self.assertEqual(target.usb_idevices(), ["iPad Pro", "iPhone 17"])
        self.assertTrue(run.call_args.kwargs["capture_output"])
        self.assertNotIn("text", run.call_args.kwargs)


class PickTests(unittest.TestCase):
    def test_team_choice_and_container_bundle_id(self):
        free = dict(id="TEAM1", name="个人", free=True); paid = dict(id="PAID1", name="付费", free=False)
        self.assertEqual(install.pick_team([free, paid])["id"], "PAID1")   # 有付费团队用付费的
        self.assertEqual(install.pick_team([free])["id"], "TEAM1")          # 只有免费的用免费的
        self.assertEqual(install.pick_team([free, paid], "TEAM1")["id"], "TEAM1")
        with um.patch.object(install, "xcode_account", return_value=(False, [])), self.assertRaises(Failure) as e: install.pick_team([])
        self.assertEqual(e.exception.code, "xcode_account_missing")   # 没登录
        with um.patch.object(install, "xcode_account", return_value=(True, [])), self.assertRaises(Failure) as e: install.pick_team([])
        self.assertEqual(e.exception.code, "xcode_team_pending")      # 登录了但团队没同步（11-08 Spare-host 实走）
        self.assertEqual(install.classify("error: No signing certificate \"iOS Development\" found"), "signing_certificate_missing")
        self.assertEqual(install.container_bundle_id("TEAM1"), "io.s2arm.games.team1")                  # 多款容器：团队专属固定包名，与款无关
        self.assertEqual(install.game_docs("0100c2500fc20000"), "Documents/games/0100C2500FC20000")            # 每款设备侧文档根（游戏号大写，同容器 Games/<游戏号>）
        self.assertEqual(install.game_pack("0100c2500fc20000"), "Library/Application Support/S2Arm/games/0100C2500FC20000/资源包")   # 资源包根挪出 Documents（11-11）
        self.assertEqual(install.game_pack_old("0100c2500fc20000"), "Documents/games/0100C2500FC20000/资源包")
        self.assertEqual(install.game_ctx("0100c2500fc20000"), "@bundle/Games/0100C2500FC20000/launch.ctx")

    def test_classify_signing_errors(self):
        # Apple 9120（7 天 10 个包名，删 App 不退）与设备同时 3 个自签 App 是两件事，各报各的码
        self.assertEqual(install.classify("error: Your maximum App ID limit has been reached. You may create up to 10 App IDs every 7 days."), "free_appid_limit")
        self.assertEqual(install.classify("The maximum number of apps for free development profiles has been reached."), "free_app_limit")   # 较早 iOS 的装机原文：说的是装上的 apps
        self.assertEqual(install.classify("This device has reached the maximum number of installed apps using a free developer profile"), "free_app_limit")   # 11-06 iOS 27 原文
        self.assertEqual(install.classify('error: No Account for Team "TEAM1".'), "xcode_account_missing")
        # #897 免费个人团队申请了付费团队才有的权限（Xcode 原文，第一条 error；后面跟的「No profiles … were found」不该盖过它）
        self.assertEqual(install.classify('error: Cannot create a iOS App Development provisioning profile for "io.s2arm.ds.x". Personal development teams, including "T", do not support the Extended Virtual Addressing capability.'), "free_team_capability")
        self.assertEqual(install.classify("error: something else"), "signing_failed")

    FAKE_TEAM = "ABCDE" + "12345"
    FAKE_OU = "TEAM12" + "3456"

    def test_manual_team_id_is_tried_even_when_not_detected(self):
        self.assertEqual(install.pick_team([], self.FAKE_TEAM), dict(id=self.FAKE_TEAM, name="", free=True))
        with self.assertRaises(Failure) as e: install.pick_team([], "bad id")
        self.assertIn("Team ID", str(e.exception))

    @unittest.skipUnless(shutil.which("openssl") and os.path.exists("/usr/bin/openssl"), "需要 /usr/bin/openssl")
    def test_keychain_certificate_gives_team_without_xcode_cache(self):
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN=Apple Development: X ({self.FAKE_TEAM})/OU={self.FAKE_OU}/O=X/C=US",
                            "-keyout", d + "/k", "-out", d + "/c"], check=True, capture_output=True)
            pem = open(d + "/c").read()
        good, other = "A" * 40, "B" * 40
        def security(args, env):
            if args[0] == "find-identity":
                return f'  1) {good} "Apple Development: X ({self.FAKE_TEAM})"\n  2) {other} "Developer ID Application: X ({self.FAKE_OU})"\n'
            return f"SHA-1 hash: {good}\n{pem}SHA-1 hash: {other}\n{pem}"
        with um.patch.object(install, "_security_text", security):
            self.assertEqual(install.keychain_teams(), [dict(id=self.FAKE_OU, name="", free=True)])   # 同一团队去重，Developer ID 不算
            with um.patch.object(install, "_xcode_prefs", return_value={}), um.patch.object(install, "profile_teams", return_value=[]):
                self.assertEqual(install.xcode_account()[1], [dict(id=self.FAKE_OU, name="", free=True)])
                self.assertEqual(install.pick_team(install.xcode_account()[1])["id"], self.FAKE_OU)
        with um.patch.object(install, "_security_text", return_value=""), um.patch.object(install, "_xcode_prefs", return_value={}):
            self.assertEqual(install.xcode_account(), (False, []))

    def test_signing_summary_has_counts_not_names(self):
        prefs = {"DVTDeveloperAccountManagerAppleIDLists": {"a": [{"identifier": "secret@example.com"}]}, "IDEProvisioningTeamByIdentifier": {"x": [{"teamID": "SECRET" + "1234", "teamName": "秘密团队"}]}}
        with um.patch.object(install, "_xcode_prefs", return_value=prefs), um.patch.object(install, "development_identities", return_value={"A"}), \
                um.patch.object(install.subprocess, "run", return_value=um.Mock(stdout="Xcode 27.1\nBuild")):
            text = install.signing_summary()
        self.assertEqual(text, "Xcode 27.1；账号 1 个；团队缓存键有；可用团队 1 个；开发证书 1 张")
        self.assertNotIn("secret", text); self.assertNotIn("秘密", text)

    def test_signing_project_is_stable_and_buildable_shape(self):
        import types, plistlib as _p
        with tempfile.TemporaryDirectory() as d:
            c = types.SimpleNamespace(private=Path(d))
            path = install.signing_project(c)["path"]
            text = (Path(path) / "project.pbxproj").read_text(encoding="utf-8")
            self.assertRegex(text, r'PRODUCT_BUNDLE_IDENTIFIER = "io\.s2arm\.signing\.h[0-9a-f]{10}"')
            self.assertNotIn("@BUNDLE@", text); self.assertNotIn("DEVELOPMENT_TEAM", text)
            self.assertEqual(install.signing_project(c)["path"], path)
            self.assertTrue((Path(d) / "signing-helper/App.swift").is_file())

    def test_prefs_with_control_characters_still_show_account(self):
        body = plistlib.dumps(dict(DVTDeveloperAccountManagerAppleIDLists=dict(a=[dict(identifier="x")]))).replace(b"<dict>", b"<dict><key>ext\x1bkey</key><string>v</string>", 1)
        with um.patch.object(install.subprocess, "run", return_value=um.Mock(returncode=0, stdout=body)):
            self.assertIn("DVTDeveloperAccountManagerAppleIDLists", install._xcode_prefs(None))
        with um.patch.object(install.subprocess, "run", return_value=um.Mock(returncode=0, stdout=b"not a plist")):
            self.assertEqual(install._xcode_prefs(None), {install.PREFS_UNREADABLE: True})

    def test_xcode_teams_reads_prefs(self):
        d = tempfile.mkdtemp(); fake = Path(d) / "defaults"
        prefs = plistlib.dumps({"IDEProvisioningTeamByIdentifier": {"acc": [{"teamID": "TEAM1", "teamName": "我", "isFreeProvisioningTeam": True}, {"teamID": "TEAM1"}]}})
        (Path(d) / "p.plist").write_bytes(prefs)
        fake.write_text(f'#!/bin/sh\ncat "{d}/p.plist"\n'); fake.chmod(0o755)
        self.assertEqual(install.xcode_teams(dict(os.environ, PATH=d + ":" + os.environ["PATH"])), [dict(id="TEAM1", name="我", free=True)])


class ShellSdkTests(unittest.TestCase):
    # 11-07 Duo 实测：旧 SDK 链接的壳在 iOS 新一代机型上按旧尺寸缩小居中 → 装机时比「壳链接 SDK 大版本」与「iPhone 系统大版本」
    def _app(self, sdk):
        import struct
        d = Path(tempfile.mkdtemp()) / "S2Arm.app"; d.mkdir()
        (d / "Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable": "S2Arm"}))
        seg = struct.pack("<II", 0x19, 72) + bytes(64)                                   # 先放一条无关的 LC_SEGMENT_64，确认按 cmdsize 往后走
        bv = struct.pack("<IIIIII", 0x32, 24, 2, 0x00120000, sdk, 0) if sdk is not None else b""   # LC_BUILD_VERSION：platform 2（iOS）、minos 18.0
        cmds = seg + bv
        hdr = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 2 if bv else 1, len(cmds), 0, 0)
        (d / "S2Arm").write_bytes(hdr + cmds); return d

    def test_linked_sdk_decodes_nibbles(self):
        self.assertEqual(install.linked_sdk(self._app(0x001A0500) / "S2Arm"), (26, 5, 0))   # 0x1A=26、0x05=5
        self.assertEqual(install.linked_sdk(self._app(0x001B0100) / "S2Arm"), (27, 1, 0))
        self.assertIsNone(install.linked_sdk(self._app(None) / "S2Arm"))                  # 没有 LC_BUILD_VERSION
        bad = Path(tempfile.mkdtemp()) / "x"; bad.write_bytes(b"#!/bin/sh\n"); self.assertIsNone(install.linked_sdk(bad))

    def test_warns_only_when_phone_newer_than_sdk(self):
        for sdk, os_ver, want in ((0x001A0500, "27.1", 1), (0x001B0100, "27.1", 0), (0x001A0500, "26.4", 0), (0x001A0500, "", 0)):
            c = um.Mock(); install.check_shell_sdk(c, self._app(sdk), {"os": os_ver})
            self.assertEqual(c.diagnostic.call_count, want, (hex(sdk), os_ver))
            if want:
                self.assertEqual(c.diagnostic.call_args.kwargs, dict(code="shell_sdk_old", severity="warning"))
                self.assertIn("iOS 26.5", c.diagnostic.call_args.args[0]); self.assertIn("iOS 27.1", c.diagnostic.call_args.args[0])


_phone_games = install.phone_games   # 真的那份：下面的夹具把 install.phone_games 换成「手机上没有别的游戏」，要走真读法的用例再换回来


def _jobs_setup_no_phone_games(self):
    """JobsTests 的夹具 + 手机上没有别的游戏（install.phone_games 替身：这些用例不测「读手机游戏清单」，那条另有用例）"""
    tj.JobsTests.setUp(self)
    p = um.patch.object(install, "phone_games", lambda c, ph: set()); p.start(); self.addCleanup(p.stop)


class FlowTests(unittest.TestCase):
    setUp = _jobs_setup_no_phone_games; tearDown = tj.JobsTests.tearDown   # 只借夹具（临时游戏库 / 私有目录 / 任务上下文），不继承它的用例

    def _fixture(self, gid):
        """这款出好的 iPhone 版（本壳 SHELL1 出的容器件 + 资源包 cafe01）→ (image/ios, 资源包)"""
        kit, pack = ios_fixture(self.c, gid, shell="SHELL1")
        return kit.parent, pack

    def _real_ipa(self, path):
        """写真 zip ipa（sign_and_install 用 ditto 真解，要 PKZip 签名）"""
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("Payload/S2Arm.app/Info.plist", plistlib.dumps({"CFBundleIdentifier": "io.s2arm.games", "CFBundleExecutable": "S2Arm"}))
            z.writestr("Payload/S2Arm.app/S2Arm", b"bin")

    def _install(self, gid, pushed, applied_before, mbps=0, unpack_ok=True, existing=(), cant_open=False, locked=False, install_errs=()):
        """跑 install.install（容器流程）：替身 assemble 出容器（含已有各款 + 本款）、签名装机、只推 fresh 的款。
        existing = 这台手机上已有的款（同壳、资源包号与设备记录一致），现组时带上、不再推。替身按 push_pack.apply 口径记起 App 解包"""
        if existing:
            self.c.catalog = lambda: {"games": [{"id": g, "name": g} for g in (gid, *existing)]}
            layout.save_device(self.c, "00000000-0000000000000002", dict(games={g: dict(pack="cafe01") for g in existing}))
        shell = self.c.private / "shell"; shell.mkdir(exist_ok=True); (shell / "manifest.json").write_text("{}")
        runs, seen, launches, state = [], {}, [], {"applied": applied_before, "unpack": "10-01 09:00:00 解开 3 个包 9 个文件 1 MB，.applied = cafe01（0.2 秒）\n"}
        all_games = list(existing) + [gid]
        def run(args, log=None, env=None, cwd=None, on_line=None, ok_codes=(0,)):
            args = [str(a) for a in args]; runs.append(args)
            # 本机描述文件（替身）：申请过的包名才有；没有时 dev_sign 退出码 3（install._sign_or_request 先签、3 才申请）
            if args[0] == "bash" and args[1].endswith("ensure_profile.sh"): state.setdefault("prof", set()).add(args[2])
            if args[1].endswith("dev_sign.py") and 3 in ok_codes and args[3] not in state.get("prof", set()):
                self.c.last_rc = 3; return
            if len(args) > 5 and args[4].endswith("ship_ext.py") and args[5] == "container":
                o = Path(args[args.index("--out") + 1]); o.mkdir(parents=True, exist_ok=True)
                (o / "s2a_catalogue.json").write_text(json.dumps({"schema": 1, "shell_id": "SHELL1", "games": [{"title_id": x} for x in all_games]}))
            if len(args) > 5 and args[4].endswith("app_bundle.py") and args[5] == "write_ipa":
                self._real_ipa(Path(args[7]))
            if args[1].endswith("dev_sign.py"): seen["bid"] = plistlib.loads((Path(args[2]) / "Info.plist").read_bytes())["CFBundleIdentifier"]
            if args[2].endswith("tools/push_pack.py"):
                seen["env"] = env; seen["progress_flag"] = "--progress-lines" in args
                if seen["progress_flag"]:   # 推包工具每推完一批打的机器读行（tools/push_pack.py --progress-lines）
                    for d, k in ((0, 0), (1 << 20, 1), (3 << 20, 2)): on_line(f"S2A_PUSH_PROGRESS {d} {3 << 20} {k} 2")
                if locked:   # #333：推之前写探针就写不进（探针在推送之前，和带不带 --apply 无关）
                    Path(log).parent.mkdir(parents=True, exist_ok=True)
                    Path(log).write_text("【红】" + ("iPhone 写不进资源包（Operation timed out; abandoned）。手机没有锁屏，是电脑和手机的连接出了问题：拔下数据线重新插好，再点一次。"
                                                    if locked == "conn" else "iPhone 写不进资源包：手机锁屏了。解锁手机、保持亮屏，再点一次。") + "\n", encoding="utf-8")
                    raise install.Failure("tool", code="tool_exit")
                on_line(f"[12:00:00] 资源包：推 {pushed} 个文件 3 MB，分 1 批 + 索引 + 清单（1 秒）")
                if "--apply" in args and not (pushed == 0 and state["applied"] == "cafe01"):
                    launches.append({"S2A_PACK_APPLY_ONLY": "1"})
                    on_line("[12:00:05] 机上解包 13/17")
                    if cant_open:
                        Path(log).parent.mkdir(parents=True, exist_ok=True)
                        Path(log).write_text("【红】资源包已传完，还差最后一步：在 iPhone 上打开 S2Arm（电脑这边打不开手机上的 App：起 App 失败：RSD 里没有 app_service）\n", encoding="utf-8")
                        raise install.Failure("tool", code="tool_exit")
                    if not unpack_ok:
                        Path(log).parent.mkdir(parents=True, exist_ok=True)
                        Path(log).write_text("【红】资源包推进去了，但机上解包 180 秒没有新进展（停在第 13 包）：打开这款游戏、保持在前台等它导入完\n", encoding="utf-8")
                        raise install.Failure("tool", code="tool_exit")
                    state["applied"] = "cafe01"
        errs = list(install_errs)   # devicectl device install app 依次回的错误原文（用完 = 成功）
        def devicectl(c, *a, timeout=0):
            if a[:3] == ("device", "install", "app"):
                seen["installs"] = seen.get("installs", 0) + 1
                return (1, {}, errs.pop(0)) if errs else (0, {}, "")
            if a[:3] == ("device", "process", "launch"):
                launches.append(a)
                if unpack_ok: state["applied"] = "cafe01"; state["unpack"] += "11-06 12:00:00 解开 1 个包 2 个文件 0 MB，.applied = cafe01（0.1 秒）\n"
                return 0, {}, ""
            if a[:3] == ("device", "copy", "from"):
                src = a[a.index("--source") + 1]
                Path(a[a.index("--destination") + 1]).write_text(state["unpack"] if src.endswith("unpack.log") else state["applied"] + "\n", encoding="utf-8"); return 0, {}, ""
            return 0, {}, ""
        import core, diag, itertools, ipa as packer
        tick = itertools.count(1000.0, 1.0)   # 作业层进度节流按单调时钟 0.2 秒：替身里各行紧挨着打，让每次取时钟走 1 秒（真实推送每批几十秒、解包每包十几秒）
        with um.patch.object(install, "xcode_teams", return_value=[dict(id="TEAM1", name="我", free=True)]), \
             um.patch.object(install, "phone", return_value=dict(id="C891", name="测试设备2")), um.patch.object(install, "udid_of", return_value="00000000-0000000000000002"), \
             um.patch.object(install, "usb_mbps", return_value=mbps), \
             um.patch.object(packer, "current_shell_id", return_value="SHELL1"), um.patch.object(packer, "container_shell", return_value=shell), \
             um.patch.object(self.c, "run", run), um.patch.object(diag, "devicectl", devicectl), um.patch.object(install.time, "sleep"), \
             um.patch.object(install, "pick_pack_root", lambda c, ph, g: f"Library/Application Support/S2Arm/games/{g}/资源包"), \
             um.patch.object(core.time, "monotonic", lambda: next(tick)):
            r = install.install(self.c)
        return r, runs, seen, launches

    E4016 = ("ERROR: The device is not able to fulfill the requested usage assertion requirements. (com.apple.dt.CoreDeviceError error 4016 (0xFB0))\n"
             "       RequestedDeviceStates = (\n           \"com.apple.coredevice.coreDeviceServicesLoaded\",\n       )\n       CurrentlyAssertableStates = (\n       )")

    def test_single_app_push_never_launches_the_app_to_unpack(self):
        """11-13 维护者：装上 App、把包传到位就算装好，玩家自己在手机上点信任、打开时 App 自己解开。原先不带上下文的单款 App（下游壳）推完带 --apply
        拉起 App 解包，免费账号第一次装还没信任开发者时拉不起，作业打了红却报完成。期望：旧形态资源包、不带上下文，同样不带 --apply"""
        gid = "0100535012974000"; out, pack = self._fixture(gid)
        runs = []
        def run(args, log=None, env=None, cwd=None, on_line=None, ok_codes=(0,)):
            runs.append([str(a) for a in args]); on_line("[12:00:00] 资源包：推 2 个文件 3 MB，分 1 批 + 索引 + 清单（1 秒）")
        with um.patch.object(self.c, "run", run):
            self.assertEqual(install.push_pack(self.c, "dev1", "io.s2arm.ds.x.team1", pack), (2, 3 << 20))
        push = next(a for a in runs if a[2].endswith("tools/push_pack.py"))
        self.assertIn("run", push); self.assertNotIn("--apply", push)

    def test_readiness_downstream_package_comes_from_its_own_record(self):
        """下游深度方案出的是独立 App：install-check 的「iPhone 版已做好」看它自己的出包记录（原先看容器件，下游款一律报没做好，实际装得上）"""
        import downstream
        gid = "0100535012974000"; self.c.r.update(gameId=gid)
        scheme = {"id": "x"}
        with um.patch.object(downstream, "requested", return_value=scheme), \
             um.patch.object(downstream, "last_record", return_value={"ipa": "/lib/downstream/x/a.ipa"}):
            r, it = self._ready([self._dev()])
        self.assertTrue(it["package"]["ok"]); self.assertEqual(r["ipaPath"], "/lib/downstream/x/a.ipa")
        with um.patch.object(downstream, "requested", return_value=scheme), um.patch.object(downstream, "last_record", return_value=None):
            self.assertFalse(self._ready([self._dev()])[1]["package"]["ok"])

    def test_container_missing_pack_refuses_before_install(self):
        gid = "0100000000010000"
        # 有容器件但没有资源包（image/ios/pack）：ios_pack 拒绝，不走到找设备 / 组装
        kit = layout.image_ios(self.c, gid) / "kit"; kit.mkdir(parents=True); (kit / "main.npdm").write_bytes(b"META"); (kit / "main.fixups.tsv").write_text("")
        self.c.r.update(gameId=gid)
        with um.patch.object(install, "phone_for", side_effect=AssertionError("不该走到找设备")):
            with self.assertRaises(Failure) as e: install.install(self.c)
        self.assertEqual(e.exception.code, "pack_missing")
        # 连容器件也没有（没出过 iPhone 版）：报「先打 iPhone 包」
        self.c.r.update(gameId="0100000000090000")
        with self.assertRaises(Failure) as e: install.install(self.c)
        self.assertEqual(e.exception.code, "job_failed"); self.assertIn("打 iPhone 包", str(e.exception))

    def test_container_assemble_uses_each_games_kit(self):
        """assemble：每款都从这款文件夹的 image/ios/kit 现组（--game），不带 --base 底包；--host-abi 必传；产物出到游戏库里的临时目录，只留 S2Arm.ipa"""
        import ipa as packer, host_update
        A, B = "0100000000010000", "0100000000020000"
        kits = {g: ios_fixture(self.c, g, shell="SID1")[0] for g in (A, B)}
        self.c.catalog = lambda: {"games": [{"id": A, "name": "甲"}, {"id": B, "name": "乙"}]}
        shell = self.c.private / "shell"; shell.mkdir(); (shell / "manifest.json").write_text("{}")
        runs = []
        def run(args, log=None, env=None, cwd=None, on_line=None, ok_codes=(0,)):
            args = [str(a) for a in args]; runs.append(args)
            if len(args) > 5 and args[4].endswith("ship_ext.py") and args[5] == "container":
                o = Path(args[args.index("--out") + 1]); o.mkdir(parents=True, exist_ok=True)
                gs = [A] if args.count("--game") == 1 else [A, B]
                (o / "s2a_catalogue.json").write_text(json.dumps({"schema": 1, "shell_id": "SID1", "games": [{"title_id": x} for x in gs]}))
            if len(args) > 5 and args[4].endswith("app_bundle.py") and args[5] == "write_ipa":
                Path(args[7]).write_bytes(b"ipa")
        with um.patch.object(packer, "current_shell_id", return_value="SID1"), \
             um.patch.object(packer, "container_shell", return_value=shell), \
             um.patch.object(packer, "release_version", return_value="9.8.7"), \
             um.patch.object(packer, "display_version", return_value="v9.8.7-dev+abc1234"), \
             um.patch.object(self.c, "run", run):
            ipa1, games1, sid1 = packer.assemble(self.c, A)
            ipa2, games2, sid2 = packer.assemble(self.c, A, [B])
        c1, c2 = [a for a in runs if len(a) > 5 and a[5] == "container"]
        for cmd in (c1, c2):
            self.assertNotIn("--base", cmd)
            self.assertEqual(cmd[cmd.index("--host-abi") + 1], str(host_update.HOST_ABI))
            self.assertEqual(cmd[cmd.index("--version") + 1], "9.8.7")   # 三端同号：电脑上 S2Arm 的发版号写进容器
            self.assertEqual(cmd[cmd.index("--display-version") + 1], "v9.8.7-dev+abc1234")   # 界面版本字同一来源
            # 组装时不签，写完容器摘要（#845，改 Info.plist）再统一 ad-hoc 签：签完再改会破坏签名（v0.3.6 候选 ⑥ codesign 不过）
            self.assertEqual((cmd[cmd.index("--sign") + 1], cmd[cmd.index("--shell") + 1]), ("none", str(shell)))
        signs = [i for i, a in enumerate(runs) if len(a) > 5 and a[5] == "sign"]
        conts = [i for i, a in enumerate(runs) if len(a) > 5 and a[5] == "container"]
        self.assertEqual(len(signs), 2)   # 每次组装签一次，排在各自的 container 之后
        for ci, si in zip(conts, signs):
            self.assertLess(ci, si); self.assertEqual(runs[si][runs[si].index("--sign") + 1], "adhoc")
        self.assertEqual([c1[i + 1] for i, x in enumerate(c1) if x == "--game"], [str(kits[A].resolve())])
        self.assertEqual([c2[i + 1] for i, x in enumerate(c2) if x == "--game"], [str(kits[A].resolve()), str(kits[B].resolve())])
        self.assertEqual((games1, games2, sid1, sid2), ([A], [A, B], "SID1", "SID1"))
        for ipa in (ipa1, ipa2):
            self.assertEqual(ipa.parent.parent, self.c.library); self.assertTrue(ipa.parent.name.startswith(".container-"))
            self.assertEqual(sorted(p.name for p in ipa.parent.iterdir()), ["S2Arm.ipa", "custom.json"])   # 组完删 S2Arm.app
        self.assertFalse(layout.phone_ipa(self.c).exists())   # 组装不碰默认位置根（装上之后才 commit_phone_ipa）
        # 没出过 iPhone 版的款报 container_stale（没有容器件）
        with um.patch.object(packer, "current_shell_id", return_value="SID1"), \
             um.patch.object(packer, "container_shell", return_value=shell), \
             um.patch.object(self.c, "run", run):
            with self.assertRaises(Failure) as e: packer.assemble(self.c, "0100000000090000")
        self.assertEqual(e.exception.code, "container_stale")

    def test_container_program_number_mismatch_routes_by_program_number(self):
        """组装结果里这款的程序号（main.npdm）和游戏库的号（导入时 CNMT 发行号）不一样（玩家实报：把别的游戏的程序换进了这款的包里）。
        维护者 11-17（按新规矩改：原先 game_files_mismatch 拒绝）：多出来的程序号只有一个 → 就是这款，照常组装、按程序号放、资源取回游戏包号那款；
        发 warning 级 game_files_mismatch、两个号都给出；多出来不止一个（认不出是哪个）→ 照旧报错。期望手写"""
        import ipa as packer
        A, OTHER = "0100DD2053220000", "010051F0207B2000"
        ios_fixture(self.c, A, shell="SID1")
        self.c.catalog = lambda: {"games": [{"id": A, "name": "甲"}]}
        shell = self.c.private / "shell"; shell.mkdir(); (shell / "manifest.json").write_text("{}")
        def run(args, log=None, env=None, cwd=None, on_line=None, ok_codes=(0,)):
            args = [str(a) for a in args]
            if len(args) > 5 and args[4].endswith("ship_ext.py") and args[5] == "container":
                o = Path(args[args.index("--out") + 1]); o.mkdir(parents=True, exist_ok=True)
                (o / "s2a_catalogue.json").write_text(json.dumps({"schema": 1, "shell_id": "SID1", "games": [{"title_id": OTHER}]}))
            if len(args) > 5 and args[4].endswith("app_bundle.py") and args[5] == "write_ipa":
                Path(args[7]).write_bytes(b"ipa")
        with um.patch.object(packer, "current_shell_id", return_value="SID1"), \
             um.patch.object(packer, "container_shell", return_value=shell), \
             um.patch.object(packer, "release_version", return_value="9.8.7"), \
             um.patch.object(self.c, "run", run):
            diags = []
            with um.patch.object(self.c, "diagnostic", create=True, side_effect=lambda m, **kw: diags.append(dict(kw, message=m))):
                _, games, _ = packer.assemble(self.c, A)
        self.assertEqual(games, [OTHER])
        self.assertEqual(packer.library_id(self.c, OTHER), A); self.assertEqual(packer.library_id(self.c, A), A)
        self.assertEqual([(d["code"], d["severity"]) for d in diags], [("game_files_mismatch", "warning")])
        self.assertIn(OTHER, diags[0]["message"]); self.assertIn(A, diags[0]["message"]); self.assertNotIn("产物根", diags[0]["message"])
        with self.assertRaises(Failure) as e:
            packer.title_mismatch(self.c, A, (), [OTHER, "0100000000000BBB"])
        self.assertEqual(e.exception.code, "game_files_mismatch"); self.assertIn("原版游戏文件重新导入", str(e.exception))

    def _ready(self, devices, teams=(dict(id="team-a", name="我", free=True),), rc=0, usb=(), raises=None, want="测试设备2"):
        """跑装机前清单：devicectl list devices 回 devices（手造的 devicectl 行），Xcode 团队 teams；want = 玩家选的那台（None = 没选）"""
        import diag
        if want is None: self.c.r.pop("device", None)
        else: self.c.r["device"] = want
        data = {"result": {"devices": list(devices)}}
        import target
        def devicectl(c, *a, timeout=0):
            if raises: raise raises
            if a[:3] == ("device", "info", "details"):
                return rc, {"result": next((d for d in devices if d["identifier"] == a[-1]), {})}, ""
            return rc, data, ""
        acct = dict(account="a@example.com", team=teams[0]["id"], teamName=teams[0]["name"]) if teams else None   # Apple ID 看随包 plumesign 当前账号（install.plume_account），不看 Xcode
        with um.patch.object(install, "mac_plume", lambda c: True), um.patch.object(install, "win_install_mod", lambda c: None), \
             um.patch.object(install, "plume_account", lambda c, W: acct), um.patch.object(diag, "devicectl", devicectl), \
             um.patch.object(target, "usb_idevices", return_value=list(usb)):
            r = install.readiness(self.c)
        return r, {i["id"]: i for i in r["items"]}

    @staticmethod
    def _dev(name="测试设备2", transport="wired", pairing="paired", devmode="enabled", tunnel="disconnected", platform="iOS", reality="physical"):
        return {"identifier": name, "hardwareProperties": {"reality": reality, "platform": platform},
                "connectionProperties": {"transportType": transport, "pairingState": pairing, "tunnelState": tunnel},
                "deviceProperties": {"name": name, "developerModeStatus": devmode}}

    def test_readiness_all_ready(self):
        gid = "0100535012974000"; self._fixture(gid); self.c.r.update(gameId=gid)
        r, it = self._ready([self._dev()])
        self.assertTrue(r["ready"]); self.assertEqual(r["device"], "测试设备2")
        self.assertEqual([i["id"] for i in r["items"]], ["package", "appleId", "connected", "trusted", "developerMode", "deviceReady"])
        self.assertEqual(r["ipaPath"], "")   # 「显示 IPA」只指真有的那份：还没装过、默认位置根没有 S2Arm.ipa → 空（原先指这款 image/ios，打开的是游戏目录）
        layout.phone_ipa(self.c).write_bytes(b"ipa")                                  # 装过一次：默认位置根有整库共用的那份
        self.assertEqual(self._ready([self._dev()])[0]["ipaPath"], str(layout.phone_ipa(self.c)))
        self.assertEqual((r["account"], it["appleId"]["title"]), ("我", "已登录 Apple ID"))
        self.assertTrue(all(i["detail"] == "" for i in r["items"]))   # 好了的项不带说明

    def test_readiness_network_only_says_so(self):
        """没插线、只在同一网络上配对着（transport localNetwork）：能装，但「已连接」那项如实写网络连接、带建议插线（原先照样打勾「已用数据线连上」，11-11 玩家反馈）"""
        gid = "0100535012974000"; self._fixture(gid); self.c.r.update(gameId=gid)
        r, it = self._ready([self._dev(transport="localNetwork")])
        self.assertTrue(it["connected"]["ok"])
        self.assertIn("同一 Wi-Fi", it["connected"]["title"]); self.assertEqual(it["connected"]["detail"], install.S_NETWORK)   # 标题写明走 Wi-Fi，说明提醒插线更稳
        r, it = self._ready([self._dev()])
        self.assertIn("（数据线）", it["connected"]["title"]); self.assertEqual(it["connected"]["detail"], "")   # 插着线：不提醒

    def test_probe_reports_why_device_list_unreadable(self):
        """出包页设备探测：读不到设备列表（超时）→ connected 空 + connectedError 带原因，界面据此说「读不到」而不是「把 iPhone 插上」；读得到 → connectedError 空"""
        import target
        with um.patch.object(target, "device_list", return_value=([], "读取设备列表超时：解锁手机，重新插一次数据线再试。", "timeout", "")), \
             um.patch.object(target, "models", return_value=[]), um.patch.object(target, "os_choices", return_value=[]), um.patch.dict(self.c.r, {}, clear=False):
            self.c.r.pop("quick", None)
            r = target.probe(self.c)
        self.assertEqual((r["connected"], r["connectedError"]), ([], "读取设备列表超时：解锁手机，重新插一次数据线再试。"))
        with um.patch.object(target, "device_list", return_value=([self._dev()], "", 0, "")), um.patch.object(target, "gpu_arch", return_value="g"), \
             um.patch.object(target, "models", return_value=[]), um.patch.object(target, "os_choices", return_value=[]):
            r = target.probe(self.c)
        self.assertEqual(([d["id"] for d in r["connected"]], r["connectedError"]), (["测试设备2"], ""))

    def test_readiness_steps_in_order(self):
        gid = "0100535012974000"; self.c.r.update(gameId=gid)   # 没出 iPhone 包
        r, it = self._ready([], teams=())                       # 没登录、没连
        self.assertFalse(r["ready"])
        self.assertEqual([i["id"] for i in r["items"] if i["blocking"]], ["package"])
        self.assertEqual(r["ipaPath"], "")
        self.assertEqual((it["package"]["ok"], it["package"]["who"]), (False, "app"))
        self.assertFalse(it["appleId"]["ok"]); self.assertEqual(it["appleId"]["detail"], install.APPLE_SIGN_IN)   # 没登录：在 S2Arm 里用 Apple ID 登录
        self.assertFalse(it["connected"]["ok"])
        self.assertTrue(it["trusted"]["waiting"]); self.assertTrue(it["developerMode"]["waiting"])   # 没连上：后两项是「等前一步」，不报没打开
        self.assertEqual((it["trusted"]["detail"], it["developerMode"]["detail"]), ("", ""))
        # 连上了但没点信任：信任这项要玩家做，开发者模式仍在等
        r, it = self._ready([self._dev(pairing="unpaired", devmode=None)])
        self.assertTrue(it["connected"]["ok"]); self.assertFalse(it["trusted"]["ok"]); self.assertFalse(it["trusted"]["waiting"])
        self.assertTrue(it["developerMode"]["waiting"])
        # 信任了、开发者模式没开
        r, it = self._ready([self._dev(devmode="disabled")])
        self.assertTrue(it["trusted"]["ok"]); self.assertFalse(it["developerMode"]["ok"]); self.assertFalse(it["developerMode"]["waiting"])
        self.assertIn("开发者模式", it["developerMode"]["detail"])

    def test_readiness_actions(self):
        """每项没好时带的启动器本地动作（清单按钮、控制口 /install-action 按它派发）：对照表写死；好了 / 等前一步的不带"""
        want = {"package": "openExport", "appleId": "appleLogin", "connected": "pickDevice", "trusted": "trustHelp",
                "developerMode": "developerModeHelp", "deviceReady": None}
        gid = "0100535012974000"; self.c.r.update(gameId=gid)
        r, it = self._ready([], teams=())   # 没出包、没登录、没连：信任 / 开发者模式在等前一步
        self.assertEqual({k: it[k]["action"] for k in ("package", "appleId", "connected", "trusted", "developerMode")},
                         dict(package="openExport", appleId="appleLogin", connected="pickDevice", trusted=None, developerMode=None))
        r, it = self._ready([self._dev(pairing="unpaired", devmode=None)])
        self.assertEqual(it["trusted"]["action"], want["trusted"])
        r, it = self._ready([self._dev(devmode="disabled")])
        self.assertEqual(it["developerMode"]["action"], want["developerMode"])
        self.assertIsNone(it["deviceReady"]["action"])
        self._fixture(gid)
        r, it = self._ready([self._dev()])
        self.assertTrue(all(i["action"] is None for i in r["items"]), "都好了：一个动作都不带")

    def test_readiness_only_package_blocks(self):
        gid = "0100535012974000"; self._fixture(gid); self.c.r.update(gameId=gid)   # 出好了包，但没登录、没连手机
        r, it = self._ready([], teams=())
        self.assertTrue(r["ready"])
        self.assertFalse(it["appleId"]["ok"]); self.assertFalse(it["connected"]["ok"])
        self.assertEqual([i["id"] for i in r["items"] if i["blocking"]], [])

    def test_readiness_device_filtering(self):
        gid = "0100535012974000"; self._fixture(gid); self.c.r.update(gameId=gid)
        # 模拟器、手表不算候选（物理上装不了）
        r, it = self._ready([self._dev(reality="simulated"), self._dev(platform="watchOS")])
        self.assertFalse(it["connected"]["ok"])
        # 列着、但列表说它离线的那台：算连上（候选不按在不在线筛，11-13），在线与否交给「设备就绪」那一项现查
        r, it = self._ready([self._dev(transport=None, tunnel="unavailable")])
        self.assertTrue(it["connected"]["ok"])
        # 两台都连着：只看玩家选的那台（不再要求只留一台）
        r, it = self._ready([self._dev(), self._dev(name="测试设备1")], want="测试设备1")
        self.assertTrue(it["connected"]["ok"]); self.assertEqual(r["device"], "测试设备1")
        # 没选设备：不替玩家挑一台来打勾（11-11 玩家实报：没插线却亮「已用数据线连上」）
        r, it = self._ready([self._dev()], want=None)
        self.assertFalse(it["connected"]["ok"]); self.assertIn("没说要装到哪台", it["connected"]["detail"]); self.assertEqual(r["device"], "")
        # 读不到设备列表（devicectl 失败）= 当没连
        r, it = self._ready([self._dev()], rc=1)
        self.assertFalse(it["connected"]["ok"]); self.assertIn("读不到设备列表", it["connected"]["detail"])

    def test_readiness_single_paired_network_device_counts(self):
        gid = "0100535012974000"; self._fixture(gid); self.c.r.update(gameId=gid)
        r, it = self._ready([self._dev(transport="localNetwork")])
        self.assertTrue(it["connected"]["ok"]); self.assertEqual(r["device"], "测试设备2")
        self.assertEqual(it["connected"]["title"], "iPhone「测试设备2」已连上（同一 Wi-Fi）")   # 写实际类型、名字和连接方式，不写死「数据线」
        _, it = self._ready([self._dev(transport="localNetwork"), self._dev(name="另一台", transport="localNetwork")])
        self.assertTrue(it["connected"]["ok"])   # 只看选的那台，同网另一台不相干
        r, it = self._ready([self._dev(transport=None, tunnel="unavailable"), self._dev(name="线连的")])
        self.assertTrue(it["connected"]["ok"]); self.assertEqual(r["device"], "测试设备2")   # 选的那台列着就是它，不换成插着的另一台

        self.assertEqual(self._ready([self._dev()])[1]["connected"]["title"], "iPhone「测试设备2」已连上（数据线）")

    def _link(self, states, want="", polling=False, usb=(), results=None, raises=None, details=None, install_=False, **kw):
        """states 是列表快照；details 独立给详情回包，缺省沿用下一张快照的同一设备；results 可给错误或异常"""
        import diag, target
        calls, seen, detail_seen = [], [0], [0]
        def devicectl(c, *a, timeout=0):
            if a[:2] == ("list", "devices"):
                if raises: raise raises
                state = states[min(seen[0], len(states) - 1)]; seen[0] += 1
                return state if isinstance(state, tuple) else (0, {"result": {"devices": state}}, "")
            calls.append(a[:3])
            result = (results or {}).get(a[1], (0, ""))
            if isinstance(result, Exception): raise result
            rc, raw = result
            if a[:3] == ("device", "info", "details"):
                if details is not None:
                    row = details[min(detail_seen[0], len(details) - 1)]
                else:
                    rows = states[min(detail_seen[0] + 1, len(states) - 1)]
                    row = next((d for d in rows if d["identifier"] == a[-1]), {})
                detail_seen[0] += 1
                return rc, {"result": row}, raw
            return rc, {}, raw
        with um.patch.object(diag, "devicectl", devicectl), um.patch.object(target, "usb_idevices", return_value=list(usb)), \
             um.patch.object(install, "LINK_RETRY_WAIT", 0):
            return install.device_link(self.c, want, polling=polling, install=kw.get("install", install_)), calls

    def _held(self, link, stage, *texts):
        """真装机：这一环没过也照装（stage 空），诊断轨迹记一条「继续装（不预先拦）」，rc 是那一环、raw 是原来要说的话（11-13 维护者：别预先拦）"""
        self.assertEqual(link["stage"], "")
        held = [t for t in link["trace"] if t["step"] == "继续装（不预先拦）" and t["rc"] == stage and all(x in t["raw"] for x in texts)]
        self.assertTrue(held, link["trace"])

    def _p(self, name="甲", transport="wired", pairing="paired", tunnel="disconnected", devmode="enabled", ddi=True, ident="D1", udid="SAMPLE-UDID-00112233"):
        d = self._dev(name=name, transport=transport, pairing=pairing, tunnel=tunnel, devmode=devmode)
        d["identifier"] = ident; d["hardwareProperties"]["udid"] = udid; d["deviceProperties"]["ddiServicesAvailable"] = ddi
        return d

    def test_link_reports_why_the_list_is_unreadable(self):
        self.assertIn("超时", self._link([], raises=subprocess.TimeoutExpired("devicectl", 60), usb=["iPad"])[0]["problem"])   # 读不到优先：不说成「没插」
        self.assertIn("CoreDevice", self._link([(127, {}, "x")])[0]["problem"])
        link, _ = self._link([(1, {}, "boom")])
        self.assertIn("读不到设备列表", link["problem"]); self.assertEqual(link["trace"][0]["rc"], 1); self.assertEqual(link["trace"][0]["raw"], "boom")

    def test_link_install_rereads_a_transient_miss(self):
        # 真装机：第一次列表是空的（瞬时 / 正在从同网切到数据线），第二次就在 → 照常连上，只多读一次；装机前清单轮询不重读；
        #   一直不在 → 读满 3 次才报，每次都进诊断轨迹（期望手写：读数 = trace 里 list devices 的条数）
        link, _ = self._link([[], [self._p()]], want="D1")
        self.assertEqual(link["stage"], ""); self.assertEqual([t["step"] for t in link["trace"]].count("list devices"), 2)
        link, _ = self._link([[], [self._p()]], want="D1", polling=True)
        self.assertEqual(link["stage"], "connected"); self.assertEqual([t["step"] for t in link["trace"]].count("list devices"), 1)
        link, _ = self._link([[]], want="D1")
        self.assertEqual(link["stage"], "connected"); self.assertEqual([t["step"] for t in link["trace"]].count("list devices"), 3)
        link, _ = self._link([(1, {}, "boom"), [self._p()]], want="D1")   # 读失败一次也再读
        self.assertEqual(link["stage"], "")

    def test_link_usb_branches(self):
        self.assertIn("USB 上没看到", self._link([[]])[0]["problem"])
        link, _ = self._link([[self._p(transport=None, tunnel="unavailable")]], usb=["iPad"])
        self.assertEqual(link["stage"], "connected"); self.assertIn("还没列出", link["problem"])

    def test_link_pairs_an_untrusted_phone(self):
        link, calls = self._link([[self._p(pairing="unpaired")], [self._p()]])
        self.assertEqual(link["stage"], ""); self.assertIn(("manage", "pair", "--device"), calls)
        link, _ = self._link([[self._p(pairing="unpaired")]], results={"pair": (1, "user denied")}, polling=True)
        self.assertEqual(link["stage"], "trusted"); self.assertIn("信任", link["problem"]); self.assertIn("user denied", link["problem"])   # 清单：提示
        link, _ = self._link([[self._p(pairing="unpaired")]], results={"pair": (1, "user denied")})
        self._held(link, "trusted", "信任", "user denied")   # 真装机：照装，装不上由装的那一步按原文报

    def test_link_polling_does_not_repeat_the_trust_prompt(self):
        states = [[self._p(pairing="unpaired")]]
        self.assertIn(("manage", "pair", "--device"), self._link(states, polling=True)[1])
        link, calls = self._link(states, polling=True)
        self.assertEqual(calls, []); self.assertEqual(link["stage"], "trusted")

    def test_link_rechecks_disabled_mode_from_details_not_stale_list(self):
        stale = self._p(devmode="disabled", ddi=False)
        fresh = self._p(tunnel="connected")
        other = self._p(ident="D2", transport="localNetwork")
        for polling in (False, True):
            with self.subTest(polling=polling):
                link, calls = self._link([[stale, other]], details=[fresh], polling=polling)
                self.assertEqual(link["stage"], "")
                self.assertEqual(calls, [("device", "info", "details")])
                self.assertEqual(link["device"], fresh)
                self.assertEqual(link["devices"], [fresh, other])

    def test_link_confirmed_disabled_mode_stops_before_ddi_update(self):
        link, calls = self._link([[self._p(devmode="disabled", ddi=False)]], polling=True)
        self.assertEqual(link["stage"], "developerMode")
        self.assertEqual(calls, [("device", "info", "details")]); self.assertIn("开发者模式", link["problem"])
        link, _ = self._link([[self._p(devmode="disabled", ddi=False)]])
        self._held(link, "developerMode", "开发者模式")

    def test_link_detail_query_failure_does_not_claim_mode_disabled(self):
        for result, rc in [((1, "device locked"), 1), (subprocess.TimeoutExpired("devicectl", 20), "timeout"), (OSError("disconnected"), "oserror")]:
            with self.subTest(result=result):
                link, calls = self._link([[self._p(devmode="disabled")]], results={"info": result}, polling=True)
                self.assertEqual(link["stage"], "prepare")
                self.assertIn("开发连接", link["problem"])
                self.assertEqual(link["trace"][-1]["rc"], rc)
                self.assertEqual(calls, [("device", "info", "details")])
                link, _ = self._link([[self._p(devmode="disabled")]], results={"info": result})
                self._held(link, "prepare", "开发连接")

    def test_link_details_must_identify_same_phone_and_mode(self):
        stale = self._p(devmode="disabled")
        for row in (None, [], {}, {"identifier": "D1", "deviceProperties": None}, self._p(ident="D2"), self._p(devmode=None), self._p(devmode="unknown")):
            with self.subTest(row=row):
                link, calls = self._link([[stale]], details=[row], polling=True)
                self.assertEqual(link["stage"], "prepare")
                self.assertEqual(calls, [("device", "info", "details")])
                self._held(self._link([[stale]], details=[row])[0], "prepare")

    def test_link_fresh_disabled_mode_overrides_enabled_list(self):
        link, calls = self._link([[self._p(ddi=False)]], details=[self._p(devmode="disabled", ddi=False)], polling=True)
        self.assertEqual(link["stage"], "developerMode")
        self.assertEqual(calls, [("device", "info", "details")])
        self._held(self._link([[self._p(ddi=False)]], details=[self._p(devmode="disabled", ddi=False)])[0], "developerMode")

    def test_link_rechecked_enabled_mode_can_prepare_ddi(self):
        stale = self._p(devmode="disabled", ddi=False)
        down = self._p(tunnel="connected", ddi=False)
        fresh = self._p(tunnel="connected")
        link, calls = self._link([[stale]], details=[down, fresh])
        self.assertEqual(link["stage"], "")
        self.assertEqual(link["device"], fresh)
        self.assertEqual(calls, [("device", "info", "details"), ("manage", "ddis", "update"), ("device", "info", "details")])

    def test_link_ddi_recheck_does_not_accept_unreadable_or_disabled_details(self):
        down = self._p(tunnel="connected", ddi=False)
        for row, stage in [({}, "prepare"), (self._p(devmode="disabled", ddi=False), "developerMode")]:
            with self.subTest(row=row):
                (self.c.private / "device-prepare.json").unlink(missing_ok=True)   # 清单轮询的冷却按设备记：每个子例从头算
                link, calls = self._link([[down]], details=[down, row], polling=True)
                self.assertEqual(link["stage"], stage)
                self.assertEqual(calls, [("device", "info", "details"), ("manage", "ddis", "update"), ("device", "info", "details")])
                self._held(self._link([[down]], details=[down, row])[0], stage)

    def test_readiness_failed_mode_query_waits_and_shows_connection_error(self):
        link, _ = self._link([[self._p(devmode="disabled")]], results={"info": (1, "device locked")}, polling=True)   # 清单拿的是轮询结果
        with um.patch.object(install, "device_link", return_value=link):
            _, items = self._ready([])
        self.assertFalse(items["developerMode"]["ok"])
        self.assertTrue(items["developerMode"]["waiting"])
        self.assertEqual(items["developerMode"]["detail"], "")
        self.assertFalse(items["deviceReady"]["ok"])
        self.assertFalse(items["deviceReady"]["waiting"])
        self.assertIn("device locked", items["deviceReady"]["detail"])

    def test_link_brings_up_an_idle_tunnel(self):
        link, calls = self._link([[self._p(ddi=False)], [self._p(tunnel="connected")]])
        self.assertEqual(link["stage"], ""); self.assertEqual(calls, [("device", "info", "details")])
        link, calls = self._link([[self._p(tunnel="connected")]], polling=True)
        self.assertEqual((link["stage"], calls), ("", []))   # 清单轮询：已经通了不折腾
        link, calls = self._link([[self._p(tunnel="connected")]])
        self.assertEqual((link["stage"], calls), ("", [("device", "info", "details")]))   # 真装机：选中的那台一律现查一次（列表里在不等于此刻连得上）
        link, _ = self._link([[self._p(transport="localNetwork", ddi=False)]], results={"info": (1, "device locked")}, polling=True)
        self.assertEqual(link["stage"], "prepare"); self.assertIn("不在线", link["problem"]); self.assertIn("iPhone「甲」", link["problem"])   # 清单：走 Wi-Fi 的那台现查不通 = 不在线，说是哪台
        link, _ = self._link([[self._p(ddi=False)]], results={"info": (1, "device locked")}, polling=True)
        self.assertIn("开发连接", link["problem"]); self.assertIn("device locked", link["problem"])   # 清单：插着线却建不起连接
        self._held(self._link([[self._p(ddi=False)]], results={"info": (1, "device locked")})[0], "prepare", "device locked")   # 真装机：照装

    def test_link_updates_disk_images_when_services_stay_down(self):
        down = [self._p(tunnel="connected", ddi=False)]
        link, calls = self._link([down, down, [self._p(tunnel="connected")]])
        self.assertEqual(link["stage"], ""); self.assertEqual(calls, [("device", "info", "details"), ("manage", "ddis", "update"), ("device", "info", "details")])
        link, _ = self._link([down], results={"info": (0, "still no ddi")}, polling=True)
        self.assertEqual(link["stage"], "prepare"); self.assertIn("开发者服务", link["problem"])
        self._held(self._link([down], results={"info": (0, "still no ddi")})[0], "prepare", "开发者服务")

    def test_link_only_the_chosen_device(self):
        """选机（11-13 维护者：插着手机就别说识别不了）：列着的、带 UDID 的都是候选，不看是否在线 / 配对 / 连接方式。期望手写：
        选的那台列着就用它（哪怕列表说它离线：照装，装不上由装的那一步说）；选的那台没列出、但只有一台候选 → 用那一台；
        多台候选都对不上 → 不装，列出看得到的让玩家选；一台都没有 → 按 USB 现状说"""
        ipad = self._p(name="小平板", ident="D2", transport="localNetwork", tunnel="connected", udid="PAD-UDID")
        ipad["hardwareProperties"]["platform"] = "iPadOS"
        offline_phone = self._p(name="我的手机", ident="D1", transport=None, tunnel="unavailable", udid="PHONE-UDID")
        link, _ = self._link([[offline_phone, ipad]], want="PHONE-UDID")
        self.assertEqual(link["device"]["identifier"], "D1")
        link, _ = self._link([[ipad]], want="旧手机")
        self.assertEqual(link["device"]["identifier"], "D2")
        link, _ = self._link([[offline_phone, ipad]], want="0000AAAA-0000-BBBB-0000BB")
        self.assertIsNone(link["device"]); self.assertEqual(link["stage"], "connected")
        for text in ("编号末尾 0000BB", "iPhone「我的手机」", "iPad「小平板」"): self.assertIn(text, link["problem"])
        self.assertNotIn("AAAA", link["problem"])   # 设备号前面的部分不上屏
        link, _ = self._link([[offline_phone, ipad]], want="PAD-UDID")
        self.assertEqual(link["device"]["identifier"], "D2")
        link, _ = self._link([[self._p(name="甲", tunnel="connected"), self._p(name="乙", ident="D2", tunnel="connected")]], want="乙")
        self.assertEqual(link["device"]["identifier"], "D2")
        watch = self._p(name="表", ident="W1", udid="WATCH-UDID"); watch["hardwareProperties"]["platform"] = "watchOS"
        sim = self._p(name="模拟", ident="S1", udid="SIM-UDID"); sim["hardwareProperties"]["reality"] = "simulated"
        link, _ = self._link([[watch, sim, offline_phone]], want="旧手机")   # 手表 / 模拟器不算候选：剩一台手机就用它
        self.assertEqual(link["device"]["identifier"], "D1")

    def test_phone_requires_a_chosen_device(self):
        """真装机（phone）不带设备号（11-13 改）：只连着一台就装它；多台才拒（device_required，要玩家选）。期望手写"""
        import diag, target
        self.c.r.pop("device", None)
        for devs, ok in (([self._p(name="唯一一台", tunnel="connected")], True), ([self._p(), self._p(name="乙", ident="D2")], False)):
            data = {"result": {"devices": devs}}
            with self.subTest(n=len(devs)), um.patch.object(diag, "devicectl", lambda c, *a, timeout=0, data=data: (0, data, "")), \
                    um.patch.object(target, "usb_idevices", return_value=[]), um.patch.object(install, "LINK_RETRY_WAIT", 0), \
                    um.patch.object(target, "gpu_arch", return_value="applegpu_g18p"):
                if ok:
                    self.assertEqual(install.phone(self.c)["name"], "唯一一台")
                else:
                    with self.assertRaises(Failure) as e:
                        install.phone(self.c)
                    self.assertEqual(e.exception.code, "device_required")

    def test_link_install_without_chosen_device(self):
        # 真装机没指定哪台（11-13）：只有一台候选就用它；两台要玩家选（device_required 那句）；期望手写
        link, _ = self._link([[self._p()]], install=True)
        self.assertEqual((link["stage"], link["device"]["identifier"]), ("", "D1"))
        link, _ = self._link([[self._p(), self._p(name="乙", ident="D2")]], install=True)
        self.assertIsNone(link["device"]); self.assertEqual(link["problem"], install.S_DEVICE_REQUIRED)

    def test_phone_failure_writes_a_redacted_report(self):
        # 真装机失败（选的那台没列出、列着的两台都不是）：报错不带整串设备号；诊断里的设备报告不带设备号、不带设备名（结论里的名字也打码），连接字段都在
        import diag, target
        udid = "00008140-" + "AB" * 8
        data = {"result": {"devices": [self._p(name="张三的手机", udid="00008140-" + "CD" * 8), self._p(name="李四的平板", ident="D2", udid="00008140-" + "EF" * 8)]}}
        def devicectl(c, *a, timeout=0):
            return (0, data, "") if a[:2] == ("list", "devices") else (1, {}, f"error on {udid}")
        self.c.r["device"] = udid
        with um.patch.object(diag, "devicectl", devicectl), um.patch.object(target, "usb_idevices", return_value=[]), \
             um.patch.object(install, "LINK_RETRY_WAIT", 0), self.assertRaises(Failure) as e:
            install.phone(self.c)
        self.assertEqual(e.exception.code, "device_not_connected"); self.assertNotIn(udid, str(e.exception))
        report = (self.c.logs / f"{self.c.task}-device.log").read_text(encoding="utf-8")
        for secret in (udid, "张三", "李四", "CDCDCDCD"): self.assertNotIn(secret, report)
        for field in ("rc=0", "transport=wired", "pairing=paired", "tunnel=disconnected", "developerMode=enabled", "ddi=True", "boot="): self.assertIn(field, report)

    def test_no_pack_dir_fails_before_install(self):
        # 审计 18:00 P1-58：没有「…资源包」目录不装、不报成功（原先照装，启动器写死「资源包也拷好了」）
        gid = "0100535012974000"; out, pack = self._fixture(gid); self.c.r.update(gameId=gid)
        import shutil; shutil.rmtree(pack)
        with self.assertRaises(install.Failure) as e:
            self._install(gid, pushed=0, applied_before="")
        self.assertEqual(e.exception.code, "pack_missing"); self.assertIn("资源包", str(e.exception))

    def test_usb2_link_no_hint_when_nothing_to_push(self):
        # 11-11 维护者截图：只换壳更新一个资源包都不推，也报「资源包 0.0 GB 第一次全拷约 1 分钟，换 USB 3 能快约 10 倍」→ 不推就不报
        from unittest import mock as um2
        ph = install.MacPhone.__new__(install.MacPhone); ph.c = self.c; ph.udid = "U"
        with um2.patch.object(install, "usb_mbps", return_value=480):
            ph.before_push({})
        self.assertEqual([x for x in self.c.job.get("diagnostics", []) if x["code"] == "usb_slow"], [])

    def test_usb_mbps_matches_serial(self):
        # system_profiler 的序列号 = 去掉横杠的 UDID；别的设备、没插的不算
        d = tempfile.mkdtemp(); fake = Path(d) / "system_profiler"
        data = {"SPUSBHostDataType": [{"_name": "hub", "_items": [
            {"_name": "iPhone", "USBDeviceKeySerialNumber": "000000000000000000000002", "USBDeviceKeyLinkSpeed": "480 Mb/s"},
            {"_name": "iPhone", "USBDeviceKeySerialNumber": "000000000000000000000001", "USBDeviceKeyLinkSpeed": "5 Gb/s"}]}]}
        (Path(d) / "o.json").write_text(json.dumps(data)); fake.write_text(f'#!/bin/sh\ncat "{d}/o.json"\n'); fake.chmod(0o755)
        env = dict(os.environ, PATH=d + ":" + os.environ["PATH"])
        self.assertEqual(install.usb_mbps("00000000-0000000000000002", env), 480)
        self.assertEqual(install.usb_mbps("00000000-0000000000000001", env), 5000)
        self.assertEqual(install.usb_mbps("00000000-0000000000000004", env), 0)
        self.assertEqual(install.usb_mbps("", env), 0)

    def test_locked_phone_maps_device_locked(self):
        # 10-26 测试设备2 锁屏：ensure_profile.sh 带出 xcodebuild 的目的设备报错，归成「iPhone 锁屏了」（原先是含糊的「签名没成」）
        line = "{ platform:iOS, arch:arm64, id:00000000-0000000000000002, name:测试设备2, error:The device needs to be unlocked to continue Unlock the device"
        self.assertEqual(install.classify(line), "device_locked")

    def test_unregistered_unseen_phone_maps_device_not_visible(self):
        # 11-16 D-44DP 兜底的盲区：Xcode 看不到这台、账号上也没登记过 → ensure_profile.sh 的红行 / Xcode「has no devices」原文都归 device_not_visible，建议是插线 / 信任 / 开发者模式
        out = subprocess.run(["bash", "-c", 'grep -o "^NV=\\"[^\\"]*" "$1" | cut -c5-', "_", str(Path(__file__).resolve().parents[3] / "pack/ensure_profile.sh")],
                             capture_output=True, text=True).stdout.strip()
        self.assertTrue(out.startswith("【红】Xcode 看不到这台 iPhone"), out)
        for text in ("【黄】xcodebuild 看不到这台 iPhone（ABCDEF），改为不指定设备再申请一次\n" + out,
                     "error: Your team has no devices from which to generate a provisioning profile. Connect a device to use"):
            self.assertEqual(install.classify(text), "device_not_visible", text)
        from errors import RECOVERY
        self.assertIn("开发者模式", RECOVERY["device_not_visible"])
        self.assertNotEqual(install.classify("【黄】xcodebuild 看不到这台 iPhone（ABCDEF），改为不指定设备再申请一次"), "device_not_visible", "只有兜底的提示行不算")

class KeptNroTests(unittest.TestCase):
    """运行时留下的 NRO（#8be819）：Mac 局收局后（build.keep_runtime_nros）/ 装 iPhone 前（install.pull_kept_nros）收进游戏库 games/<号>/nro_kept/。
    期望按约定手写：只收文件名 = 内容 SHA-256 的 <64 位十六进制>.nro；已收的不重复、不报；收到新的告诉玩家重新准备"""
    setUp = tj.JobsTests.setUp; tearDown = tj.JobsTests.tearDown
    G = "0100c2500fc20000"

    def _msgs(self):
        out = []
        self.c.emit = lambda kind, text=None, *a, **k: out.append(text)
        return out

    def test_mac_keep(self):
        import hashlib, build
        src = Path(self.c.private) / "docs-wire-nro"; src.mkdir(parents=True)
        good = b"NRO-A" * 7; sg = hashlib.sha256(good).hexdigest()
        (src / f"{sg}.nro").write_bytes(good)
        (src / f"{'0' * 64}.nro").write_bytes(b"name does not match")
        (src / "readme.txt").write_text("x")
        msgs = self._msgs()
        self.assertEqual(build.keep_runtime_nros(self.c, self.G, src), 1)
        kept = self.c.library / f"games/{self.G}/nro_kept"
        self.assertEqual(sorted(p.name for p in kept.iterdir()), [f"{sg}.nro"])
        self.assertEqual((kept / f"{sg}.nro").read_bytes(), good)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(build.keep_runtime_nros(self.c, self.G, src), 0)   # 已收：不重复、不再提示
        self.assertEqual(len(msgs), 1)
        self.assertEqual(build.keep_runtime_nros(self.c, self.G, src / "absent"), 0)   # 没有留下的：0、不报错

    def test_iphone_pull(self):
        import hashlib
        good = b"NRO-B" * 9; sg = hashlib.sha256(good).hexdigest(); bad = "1" * 64
        files = {f"{sg}.nro": good, f"{bad}.nro": b"content differs from its name", "notes.txt": b"x"}
        calls = []
        def devicectl(c, *a, timeout=0):
            calls.append(a[2] if a[1] == "copy" else "list")
            if a[1] == "info":
                self.assertEqual(a[a.index("--subdirectory") + 1], f"Documents/wire/{self.G.upper()}/nro")
                return 0, {"result": {"files": [{"relativePath": n} for n in files]}}, ""
            name = a[a.index("--source") + 1].rsplit("/", 1)[-1]
            Path(a[a.index("--destination") + 1]).write_bytes(files[name]); return 0, {}, ""
        import diag
        msgs = self._msgs()
        with um.patch.object(diag, "devicectl", devicectl):
            n1 = install.pull_kept_nros(self.c, "C891", "s2arm.z", self.G)
            n2 = install.pull_kept_nros(self.c, "C891", "s2arm.z", self.G)
        kept = self.c.library / f"games/{self.G}/nro_kept"
        self.assertEqual((n1, n2, sorted(p.name for p in kept.iterdir())), (1, 0, [f"{sg}.nro"]))
        self.assertEqual(len(msgs), 1)
        self.assertEqual(calls.count("from"), 3)   # 第一次拉两个 .nro（名不符的拉回后核内容丢掉）；第二次好的已在、只再拉名不符那个：共 3 次
        def missing(c, *a, timeout=0): return 1, {}, "no such directory"
        with um.patch.object(diag, "devicectl", missing):
            self.assertEqual(install.pull_kept_nros(self.c, "C891", "s2arm.z", self.G), 0)   # 机上没有：0、不报错


    def test_windows_does_not_pull(self):
        # 0.5.0 维护者定：从手机收回只保留 Mac 已发版的那条，Windows 不收。期望手写：WinPhone.pull_kept 回 0，碰都不碰手机（不起 s2a-devpush）
        ph = object.__new__(install.WinPhone); ph.c, ph.bid = self.c, "s2arm.z"
        def touched(work): raise AssertionError("Windows 不该连手机收 NRO")
        ph.client = touched
        self.assertEqual(ph.pull_kept([self.G]), 0)
        self.assertFalse((self.c.library / f"games/{self.G}/nro_kept").exists())

    def test_pull_skips_only_when_the_phone_is_in_use(self):
        # 只按真冲突互斥（候选验收：升级首启迁移几分钟，前端「有任何作业就整轮跳过」，拉回拖到第 360 秒）。期望手写：
        #   手机锁被装机 / 推包占着（ipa.phone_lock）→ 这一轮不碰手机、回空结果；锁空着 → 照常收（别的作业在跑不影响，前端不再拦）
        pulled = []
        ph = types.SimpleNamespace(key="dev1", pull_kept=lambda games: pulled.append(list(games)) or 0, app_running=lambda: False)
        install.layout.save_device(self.c, "dev1", dict(games={self.G: {}}))
        with self.c.lock("phone"):
            r = install._kept_pull_on(self.c, ph)
        self.assertEqual((r["device"], r["new"], pulled), ("", 0, []))
        r = install._kept_pull_on(self.c, ph)
        self.assertEqual((r["device"], pulled), ("dev1", [[self.G]]))

class PullSeenTests(unittest.TestCase):
    setUp = tj.JobsTests.setUp; tearDown = tj.JobsTests.tearDown

    def _pull(self, device_text):
        got = {}
        def devicectl(c, *a, timeout=0):
            got["src"] = a[a.index("--source") + 1]
            if device_text is None: return 1, {}, "no such file"
            Path(a[a.index("--destination") + 1]).write_text(device_text); return 0, {}, ""
        import diag
        with um.patch.object(diag, "devicectl", devicectl):
            r = install.pull_seen(self.c, "C891", "s2arm.z.0100c2500fc20000", "0100c2500fc20000", "00000000-0000000000000002")
        return r, got

    def test_append_overwrite_and_fork(self):
        seen = self.c.library / "games/0100c2500fc20000/seen"
        r, got = self._pull(None)
        self.assertEqual((r, got["src"], seen.exists()), (False, "Documents/wire/0100C2500FC20000/pso_user.tsv", False))   # 机上没有不算错
        self.assertTrue(self._pull("S\ta\n")[0]); self.assertEqual((seen / "00000000-0000000000000002.tsv").read_text(), "S\ta\n")
        self.assertFalse(self._pull("S\ta\n")[0])                                       # 没变
        self.assertTrue(self._pull("S\ta\nS\tb\n")[0])                                  # 续写 → 覆盖
        self.assertEqual((seen / "00000000-0000000000000002.tsv").read_text(), "S\ta\nS\tb\n")
        self.assertTrue(self._pull("S\tc\n")[0])                                        # 换包后重新记 → 另存，旧的保留
        self.assertEqual(sorted(p.read_text() for p in seen.glob("*.tsv")), ["S\ta\nS\tb\n", "S\tc\n"])


class SaveBackupTests(unittest.TestCase):
    setUp = tj.JobsTests.setUp; tearDown = tj.JobsTests.tearDown

    def _run(self, files, fail=None, list_rc=0, installed=False, apps_rc=0):
        got = []
        def devicectl(c, *a, timeout=0):
            if a[:3] == ("device", "info", "apps"):
                if apps_rc:
                    return apps_rc, {}, "devicectl 等了 60 秒没有返回，已停止等待"
                return 0, {"result": {"apps": [{"bundleIdentifier": "s2arm.z.0100c2500fc20000"}] if installed else []}}, ""
            if a[:3] == ("device", "info", "files") and list_rc:
                return list_rc, {}, "The operation couldn’t be completed"
            if a[:3] == ("device", "info", "files"):
                return 0, {"result": {"files": [dict(relativePath=r, resources=dict(isDirectory=d)) for r, d in files]}}, ""
            src = a[a.index("--source") + 1]; got.append(src)
            if fail and src.endswith(fail): return 1, {}, "Device locked"
            Path(a[a.index("--destination") + 1]).write_text("内容:" + src); return 0, {}, ""
        import diag
        with um.patch.object(diag, "devicectl", devicectl):
            return install.backup_saves(self.c, "C891", "s2arm.z.0100c2500fc20000", "0100C2500FC20000"), got

    def test_backs_up_every_file(self):
        d, got = self._run([("0000000000000001", True), ("0000000000000001/0", True), ("0000000000000001/0/save.dat", False),
                            ("0000000000000001/1/save.dat", False), ("0000000000000001/.lock", False)])
        self.assertEqual(got, [install.SAVE_DIR + "/0000000000000001/0/save.dat", install.SAVE_DIR + "/0000000000000001/1/save.dat"])   # 目录与 .lock 不拷
        self.assertEqual((d / "0000000000000001/0/save.dat").read_text(), "内容:" + got[0])
        self.assertEqual(d.parent, self.c.library / "games/0100C2500FC20000/save_backup")

    def test_list_entry_without_relative_path_asks_not_crash(self):
        # #908：Xcode 16.0 的 devicectl 有条目不带 relativePath，原先 KeyError 让整个装机作业崩成「本地任务失败」。
        #   path 字段照认、照备份；两种都没有 → 停在「备份不了存档」确认（save_backup_failed），玩家确认后照装
        import diag
        def run(entries):
            def devicectl(c, *a, timeout=0):
                if a[:3] == ("device", "info", "files"):
                    return 0, {"result": {"files": entries}}, ""
                Path(a[a.index("--destination") + 1]).write_text("x"); return 0, {}, ""
            with um.patch.object(diag, "devicectl", devicectl):
                return install.backup_saves(self.c, "C891", "s2arm.z.0100c2500fc20000", "0100C2500FC20000")
        d = run([dict(path="0000000000000001/0/save.dat", resources=dict(isDirectory=False))])
        self.assertTrue((d / "0000000000000001/0/save.dat").is_file())
        with self.assertRaises(Failure) as e:
            run([dict(name="save.dat", resources=dict(isDirectory=False))])
        self.assertEqual(e.exception.code, "save_backup_failed"); self.assertIn("读不懂 iPhone 返回的存档文件清单", e.exception.internal)
        self._accept()
        self.assertIsNone(run([dict(name="save.dat", resources=dict(isDirectory=False))]))

    def test_nothing_on_device(self):
        self.assertEqual(self._run([])[0], None)                       # 第一次装：机上没有存档，不备份不报错

    def _accept(self, on=True):   # 玩家在确认页点了「仍然继续」：同一请求带 acceptNoBackup 重发
        if on: self.c.r["acceptNoBackup"] = True
        else: self.c.r.pop("acceptNoBackup", None)

    def _decisions(self):
        f = self.c.logs / f"{self.c.task}-decisions.log"
        return f.read_text(encoding="utf-8") if f.is_file() else ""

    def test_list_failure(self):
        self.assertEqual(self._run([], list_rc=1, installed=False)[0], None)   # App 不在机上：第一次装，照装
        with self.assertRaises(Failure) as e:
            self._run([], list_rc=1, installed=True)                           # App 在却列不出：停在确认（装机原则 11-11：认得出的风险让用户确认）
        self.assertEqual(e.exception.code, "save_backup_failed")
        self.assertIn("可能丢档", str(e.exception))
        self._accept()
        self.assertEqual(self._run([], list_rc=1, installed=True)[0], None)    # 确认后带 acceptNoBackup：照装、不备份
        self.assertIn("玩家已确认继续", self._decisions())

    def test_unknown_install_state_asks_instead_of_skipping_backup(self):
        # #333：列存档超时（124）后连「装没装过」都问不出：不能悄悄当成第一次装（跳过备份直接覆盖），停在确认；玩家确认后才照装
        with self.assertRaises(Failure) as e:
            self._run([], list_rc=124, apps_rc=124)
        self.assertEqual(e.exception.code, "save_backup_failed")
        self.assertIn("确认不了 iPhone 上有没有装过这款", e.exception.internal)
        self._accept()
        self.assertEqual(self._run([], list_rc=124, apps_rc=124)[0], None)

    def _cloud(self, has_copy, placeholder=False):
        """玩家开了云存档：请求带云存档文件夹（Mac 启动器随请求带 cloudSavesFolder）；has_copy = 这款在 iCloud 里有快照"""
        folder = self.root / "iCloud Saves"; d = folder / "S2Arm Saves" / "0100C2500FC20000"; d.mkdir(parents=True, exist_ok=True)
        if has_copy: (d / (".head-iPhone.json.icloud" if placeholder else "head-iPhone.json")).write_text("{}")
        self.c.r["cloudSavesFolder"] = str(folder)

    def _messages(self):
        ev = [json.loads(l) for l in self.c.events.read_text(encoding="utf-8").splitlines() if l.strip()] if self.c.events.is_file() else []
        return [e.get("text") or e.get("message") or "" for e in ev if e.get("type") == "message" or e.get("event") == "message"]

    def test_saves_local_cloud_or_both(self):
        """维护者 11-17（玩家更新手机上的 S2Arm 卡在「确认不了 iPhone 上有…」）三种存档位置，期望手写：
        ① 只在本地（没开云存档）：照常拉回 1 个文件；② 只在 iCloud（手机上没有存档目录，或备份做不成但云端有这款快照）：不报 save_backup_failed、照装，
        做不成时提示「这款存档在 iCloud 云存档里有一份」；③ 两边都有：本地照常备份（云端有快照不跳过本地备份）"""
        local = [("0000000000000001/0/save.dat", False)]
        d, got = self._run(local)                                                     # ①
        self.assertEqual(len(got), 1); self.assertTrue((d / "0000000000000001/0/save.dat").is_file())
        self._cloud(True)
        self.assertEqual(self._run([], list_rc=1, installed=False)[0], None)          # ② 手机上没有：第一次装 / 没存档，照装
        r, _ = self._run([], list_rc=124, apps_rc=124)                                # ② 备份做不成（问不出装没装过），云端有快照 → 照装
        self.assertIsNone(r)
        self.assertTrue(any("这款存档在 iCloud 云存档里有一份" in m for m in self._messages()), self._messages())
        self.assertIn("云存档里有这款的快照，照装", self._decisions())
        d, got = self._run(local)                                                     # ③ 两边都有：本地照常备份
        self.assertEqual(len(got), 1); self.assertTrue((d / "0000000000000001/0/save.dat").is_file())

    def test_cloud_folder_without_this_game_still_asks(self):
        # 开了云存档但这款没有快照（或只有别的游戏的）：不能放行，照旧问「仍然继续 / 先不」；iCloud 没下载到本机的占位 head 也算有快照
        self._cloud(False)
        with self.assertRaises(Failure) as e:
            self._run([], list_rc=124, apps_rc=124)
        self.assertEqual(e.exception.code, "save_backup_failed")
        self._cloud(True, placeholder=True)
        self.assertIsNone(self._run([], list_rc=124, apps_rc=124)[0])

    def test_full_mac_disk_says_so(self):
        """玩家截图 [Errno 28]：Mac 磁盘满，devicectl 结果 / 拉回的存档写不下（diag.devicectl 回 28）。期望：不报笼统的「确认不了 iPhone 上有…」，
        而是 save_backup_failed 带「这台 Mac 空间不够…腾出 1 GB 再试，或者确认后继续」；确认后照装；云端有这款快照时照装不问"""
        import diag
        def run(stage):
            def devicectl(c, *a, timeout=0):
                if a[:3] == ("device", "info", "files"):
                    if stage == "list": return diag.DEVICECTL_NOSPACE, {}, "这台 Mac 磁盘满了，devicectl 的结果写不下"
                    return 0, {"result": {"files": [dict(relativePath="0000000000000001/0/save.dat", resources=dict(isDirectory=False))]}}, ""
                if a[:3] == ("device", "info", "apps"):
                    return 0, {"result": {"apps": [{"bundleIdentifier": "s2arm.z.0100c2500fc20000"}]}}, ""
                return diag.DEVICECTL_NOSPACE, {}, "ERROR: No space left on device"
            with um.patch.object(diag, "devicectl", devicectl):
                return install.backup_saves(self.c, "C891", "s2arm.z.0100c2500fc20000", "0100C2500FC20000")
        for stage in ("list", "copy"):
            with self.assertRaises(Failure) as e:
                run(stage)
            self.assertEqual(e.exception.code, "save_backup_failed", stage)
            self.assertIn("这台 Mac 空间不够，存档备份放不下", e.exception.internal); self.assertIn("腾出 1 GB 再试，或者确认后继续", e.exception.internal)
            self.assertNotIn("确认不了 iPhone 上有", e.exception.internal)
        self._accept()
        self.assertIsNone(run("list")); self._accept(False)
        self._cloud(True)
        self.assertIsNone(run("copy"))

    def test_locked_phone_waits_for_unlock_then_backs_up(self):
        """#a442de（维护者 11-17：玩家判断不了的我们替他判）：手机锁着（RemotePairingError 1016）→ 不先问「仍然继续」，提示解锁、自动重试；
        解锁后照常备份。期望手写：前两次列存档报锁屏、第三次列得出 → 提示「请解锁手机」一次、共列 3 次、备份照常（拉回 1 个文件）、不报 save_backup_failed；
        一直锁着到等满 → 走原来的确认（save_backup_failed）"""
        import diag
        LOCK = "ERROR: com.apple.dt.RemotePairingError error 1016 (0x3F8) The device has not been unlocked recently"
        def run(locked_times):
            calls = []
            def devicectl(c, *a, timeout=0):
                if a[:3] == ("device", "info", "files"):
                    calls.append(1)
                    if len(calls) <= locked_times:
                        return 1, {}, LOCK
                    return 0, {"result": {"files": [dict(relativePath="0000000000000001/0/a.bin")]}}, ""
                if a[:3] == ("device", "copy", "from"):
                    d = Path(a[a.index("--destination") + 1]); d.parent.mkdir(parents=True, exist_ok=True); d.write_bytes(b"s"); return 0, {}, ""
                if a[:3] == ("device", "info", "apps"):
                    return 0, {"result": {"apps": [{"bundleIdentifier": "s2arm.z.games"}]}}, ""
                raise AssertionError(a)
            said = []
            with um.patch.object(diag, "devicectl", devicectl), um.patch.object(install, "UNLOCK_POLL_S", 0), um.patch.object(install.time, "sleep", lambda s: None), \
                 um.patch.object(self.c, "emit", lambda kind, text="", **kw: said.append(text), create=True):
                return install.backup_saves(self.c, "C891", "s2arm.z.games", "0100C2500FC20000"), calls, said
        dest, calls, said = run(2)
        self.assertEqual(len(calls), 3)
        self.assertTrue((Path(dest) / "0000000000000001/0/a.bin").is_file())
        self.assertEqual(sum("请解锁手机" in t for t in said), 1)
        with um.patch.object(install, "UNLOCK_WAIT_S", 0), self.assertRaises(Failure) as e:
            run(10 ** 6)
        self.assertEqual(e.exception.code, "save_backup_failed")

    def _tree(self, tree, docs, locked=()):
        # 假设备文件系统（11-07 满屏线 Duo 实测）：列不存在的目录 devicectl 也报「failed to get a list of files」——和锁屏同一个错，只能靠父目录分辨
        def devicectl(c, *a, timeout=0):
            if a[:3] == ("device", "info", "apps"):
                return 0, {"result": {"apps": [{"bundleIdentifier": "s2arm.z.games"}]}}, ""   # App 在机上
            if a[:3] == ("device", "info", "files"):
                sub = a[a.index("--subdirectory") + 1]
                if sub in locked or not any(t == sub or t.startswith(sub + "/") for t in tree):
                    return 1, {}, "StreamingAction: The system failed to get a list of files on the remote device."
                return 0, {"result": {"files": [dict(relativePath=t[len(sub) + 1:], resources=dict(isDirectory=True)) for t in tree if t.startswith(sub + "/")]}}, ""
            raise AssertionError(a)
        import diag
        with um.patch.object(diag, "devicectl", devicectl):
            return install.backup_saves(self.c, "C891", "s2arm.z.games", "0100C2500FC20000", docs=docs)

    def test_missing_save_dir_under_listable_parent_installs(self):
        # 容器第一次装半途失败 / 从没进过游戏：机上 Documents/games 是空目录，存档目录不在 → 没有存档，照装（原先每次都拒装，玩家永久卡死）
        g = "Documents/games/0100C2500FC20000"
        self.assertIsNone(self._tree({"Documents", "Documents/games"}, g))
        self.assertIsNone(self._tree({"Documents", "Documents/games", g, g + "/资源包"}, g))       # 本款目录在、S2A 不在
        self.assertIsNone(self._tree({"Documents", "Documents/logs"}, "Documents"))                # 单款：Documents 在、S2A 不在

    def test_unlistable_parent_or_existing_save_dir_asks(self):
        g = "Documents/games/0100C2500FC20000"
        with self.assertRaises(Failure) as e:   # Documents 也列不出（锁屏 / 隧道断）：分不清有没有存档，停在确认
            self._tree({"Documents", "Documents/games"}, g, locked={"Documents", "Documents/games", g, g + "/" + install.SAVE_REL})
        self.assertEqual(e.exception.code, "save_backup_failed")
        with self.assertRaises(Failure) as e:   # 父目录里看得到存档目录、它本身却列不出：有档读不到，停在确认
            self._tree({"Documents", g + "/" + install.SAVE_REL + "/0000000000000001"}, g, locked={g + "/" + install.SAVE_REL})
        self.assertEqual(e.exception.code, "save_backup_failed")
        self._accept()
        self.assertIsNone(self._tree({"Documents", g + "/" + install.SAVE_REL + "/0000000000000001"}, g, locked={g + "/" + install.SAVE_REL}))

    def test_copy_failure_asks_then_continues(self):
        with self.assertRaises(Failure) as e:
            self._run([("0000000000000001/0/save.dat", False)], fail="save.dat")
        self.assertEqual(e.exception.code, "save_backup_failed")
        self._accept()
        self.assertIsNone(self._run([("0000000000000001/0/save.dat", False)], fail="save.dat")[0])




class AssembleTests(unittest.TestCase):
    """现组容器（assemble）/ put_game / 手机侧 S2Arm.ipa 与上一版（commit_phone_ipa、restore_previous）/ remove_from_phone 的细粒度测试（游戏库结构 v3）"""
    setUp = _jobs_setup_no_phone_games; tearDown = tj.JobsTests.tearDown
    A, B, C = "0100000000010000", "0100000000020000", "0100000000030000"
    COMPAT = dict(hostAbi=1, dmshChain="aaaa")   # 替身：本版引擎（新宿主）的兼容判据

    def _library(self, *games):
        """游戏库清单：(游戏号, 名字)…"""
        self.c.catalog = lambda: {"games": [{"id": g, "name": n} for g, n in games]}

    def _run(self, runs):
        """ship_ext container / app_bundle write_ipa 的替身：按 --game 容器件所在的程序专用文件夹认出各款，出真 zip ipa（带 s2a_catalogue.json）"""
        def run(args, log=None, env=None, cwd=None, on_line=None, ok_codes=(0,)):
            args = [str(a) for a in args]; runs.append(args)
            if any(a.endswith("ship_ext.py") for a in args) and "container" in args:
                o = Path(args[args.index("--out") + 1]); o.mkdir(parents=True, exist_ok=True)
                # 容器件在游戏程序专用文件夹 .programs/<程序号>/kit：上一级目录名就是游戏号
                gs = [Path(args[i + 1]).resolve().parent.name for i, x in enumerate(args) if x == "--game"]
                (o / "s2a_catalogue.json").write_text(json.dumps({"schema": 1, "shell_id": self.sid, "games": [{"title_id": g} for g in gs]}))
            if "write_ipa" in args:
                cat = json.loads((Path(args[-2]) / "s2a_catalogue.json").read_text())
                self._ipa(Path(args[-1]), cat["shell_id"], [g["title_id"] for g in cat["games"]])
        return run

    @staticmethod
    def _ipa(path, sid, games):
        """手机侧装机产物（真 zip，ipa_games 读 Payload/*.app/s2a_catalogue.json）"""
        path.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("Payload/S2Arm.app/Info.plist", plistlib.dumps({"CFBundleIdentifier": "io.s2arm.games", "CFBundleExecutable": "S2Arm"}))
            z.writestr("Payload/S2Arm.app/s2a_catalogue.json", json.dumps({"schema": 1, "shell_id": sid, "games": [{"title_id": g} for g in games]}))
        return path

    def _assemble(self, gid, others=(), failed=(), sid="SID1", runs=None, agreed=None):
        import ipa as packer, host_update
        self.sid = sid
        shell = self.c.private / "shell"; shell.mkdir(exist_ok=True); (shell / "manifest.json").write_text("{}")
        runs = [] if runs is None else runs
        with um.patch.object(packer, "current_shell_id", return_value=sid), um.patch.object(packer, "container_shell", return_value=shell), \
             um.patch.object(host_update, "engine_compat", return_value=dict(self.COMPAT)), um.patch.object(self.c, "run", self._run(runs)):
            return packer.assemble(self.c, gid, others, failed=failed, agreed=agreed or {})

    @staticmethod
    def _games_arg(runs):
        cmd = next(a for a in runs if any(x.endswith("ship_ext.py") for x in a))
        return [Path(cmd[i + 1]).resolve().parent.name for i, x in enumerate(cmd) if x == "--game"], cmd   # 容器件在 .programs/<游戏号>/kit

    def test_assemble_this_game_only(self):
        """第一次装：只组这一款，不带 --base；→ (ipa, [这款], 本壳号)"""
        self._library((self.A, "甲")); ios_fixture(self.c, self.A)
        runs = []
        ipa, games, sid = self._assemble(self.A, runs=runs)
        got, cmd = self._games_arg(runs)
        self.assertNotIn("--base", cmd)
        self.assertEqual((got, games, sid), ([self.A], [self.A], "SID1"))
        self.assertEqual(install_ipa_games(ipa), ([self.A], "SID1"))

    def test_assemble_passes_library_banner(self):
        """横图是游戏库的元数据（导入时抓好放在这款文件夹）：组装命令带 --banner 游戏号=那份；没有横图的款不带"""
        import layout
        self._library((self.A, "甲"), (self.B, "乙"))
        ios_fixture(self.c, self.A); ios_fixture(self.c, self.B)
        banner = layout.path(self.c, self.A, "banner.jpg"); banner.write_bytes(b"BANNER")
        runs = []
        self._assemble(self.A, others=(self.B,), runs=runs)
        _, cmd = self._games_arg(runs)
        self.assertEqual([cmd[i + 1] for i, x in enumerate(cmd) if x == "--banner"], [f"{self.A}={banner}"])

    def test_assemble_carries_phone_games_of_same_shell(self):
        """这台手机已有的款（同壳出的）现组时一起带上，容器件取各自的 image/ios/kit"""
        self._library((self.A, "甲"), (self.B, "乙"))
        ios_fixture(self.c, self.A); kb, _ = ios_fixture(self.c, self.B)
        runs = []
        ipa, games, sid = self._assemble(self.A, [self.B], runs=runs)
        got, cmd = self._games_arg(runs)
        self.assertEqual((got, games), ([self.A, self.B], [self.A, self.B]))
        self.assertIn(str(kb.resolve()), cmd)

    def test_assemble_other_shell_compatible_swaps_host_only(self):
        """乙是旧壳 SID1 出的、兼容判据与新宿主一致 → 原样带进新壳 SID2（只换宿主，不要求重出）；
        这台手机对 SID2 装回过上一版（failedShells 含 SID2）→ 不带、报 container_stale 点名；failedShells 里是别的壳不影响"""
        self._library((self.A, "甲"), (self.B, "乙款"))
        ios_fixture(self.c, self.A, shell="SID2"); kb, _ = ios_fixture(self.c, self.B, shell="SID1", compat=self.COMPAT)
        runs = []
        _, games, sid = self._assemble(self.A, [self.B], sid="SID2", runs=runs)
        self.assertEqual((games, sid), ([self.A, self.B], "SID2"))
        self.assertIn(str(kb.resolve()), self._games_arg(runs)[1])
        _, games, _ = self._assemble(self.A, [self.B], failed=["SID1"], sid="SID2")
        self.assertEqual(games, [self.A, self.B])
        with self.assertRaises(Failure) as e:
            self._assemble(self.A, [self.B], failed=["SID2"], sid="SID2")
        self.assertEqual(e.exception.code, "container_stale"); self.assertIn("乙款", str(e.exception))

    def test_assemble_other_shell_incompatible_is_stale(self):
        """乙是旧壳出的、新宿主认不得（直连链号不同 / 没记判据按旧版口径）→ container_stale 点名乙，不装半套"""
        self._library((self.A, "甲"), (self.B, "乙款"))
        ios_fixture(self.c, self.A, shell="SID2")
        for compat in (dict(hostAbi=1, dmshChain="bbbb"), dict(hostAbi=2, dmshChain="aaaa"), None):
            ios_fixture(self.c, self.B, shell="SID1", compat=compat)
            if compat is None:   # 没记判据的旧款：game.json 的 ios 里没有 hostAbi / dmshChain
                layout.update_info(self.c, self.B, ios=dict(shellId="SID1", pack="cafe01"))
            with self.assertRaises(Failure) as e:
                self._assemble(self.A, [self.B], sid="SID2")
            self.assertEqual(e.exception.code, "container_stale", compat); self.assertIn("乙款", str(e.exception))

    def test_assemble_stale_others_ask_then_leave_out_on_accept(self):
        """v0.3.1 装机原则：别的款（乙）新宿主认不得 → 没确认时停下报 container_stale、点名乙、说清确认后会怎样；
        玩家确认（acceptStale）→ 这次不带乙照装甲，黄字说明乙的存档和资源还在、重新出包再装就回来"""
        self._library((self.A, "甲"), (self.B, "乙款"))
        ios_fixture(self.c, self.A, shell="SID2"); ios_fixture(self.c, self.B, shell="SID1", compat=dict(hostAbi=1, dmshChain="bbbb"))
        with self.assertRaises(Failure) as e:
            self._assemble(self.A, [self.B], sid="SID2")
        self.assertEqual(e.exception.code, "container_stale"); self.assertIn("乙款", str(e.exception)); self.assertIn("确认继续", str(e.exception))
        self.assertEqual(e.exception.internal, self.B)
        self.c.r["acceptStale"] = True
        notes, runs = [], []
        with um.patch.object(self.c, "diagnostic", lambda m, **k: notes.append((m, k.get("severity")))):
            _, games, _ = self._assemble(self.A, [self.B], sid="SID2", runs=runs)
        self.assertEqual(games, [self.A]); self.assertEqual(self._games_arg(runs)[0], [self.A])
        self.assertEqual(len(notes), 1); self.assertIn("乙款", notes[0][0]); self.assertEqual(notes[0][1], "warning")

    def test_assemble_stale_two_reasons_and_agreed_not_asked_again(self):
        """#479：带不进的分两种说——乙有 iPhone 版但是旧壳出的（新版认不得）、丙这台电脑上根本没有 iPhone 版；
        这台手机在本版壳上已同意过不带乙、丙（agreed）→ 不再问，照不带它们装；agreed 只含乙、丙没同意过 → 照旧问"""
        C = "0100CCCC00000000"
        self._library((self.A, "甲"), (self.B, "乙款"), (C, "丙款"))
        ios_fixture(self.c, self.A, shell="SID2"); ios_fixture(self.c, self.B, shell="SID1", compat=dict(hostAbi=1, dmshChain="bbbb"))
        with self.assertRaises(Failure) as e:
            self._assemble(self.A, [self.B, C], sid="SID2")
        msg = str(e.exception)
        self.assertEqual(e.exception.code, "container_stale")
        self.assertIn("这台电脑上没有它们的 iPhone 版：「丙款」", msg); self.assertIn("新版认不得，要重新「打 iPhone 包」：「乙款」", msg)
        with self.assertRaises(Failure):
            self._assemble(self.A, [self.B, C], sid="SID2", agreed={"SID2": [self.B]})
        runs = []
        with um.patch.object(self.c, "diagnostic", lambda m, **k: None):
            _, games, _ = self._assemble(self.A, [self.B, C], sid="SID2", runs=runs, agreed={"SID2": [self.B, C.lower()]})
        self.assertEqual(games, [self.A])

    def test_assemble_skips_games_deleted_from_library(self):
        """#194：手机上的某款已从游戏库删除：不点名、不挡，新容器不带它，黄字说明"""
        self._library((self.A, "甲")); ios_fixture(self.c, self.A)
        notes, runs = [], []
        with um.patch.object(self.c, "diagnostic", lambda m, **k: notes.append((m, k.get("severity")))):
            _, games, _ = self._assemble(self.A, [self.B], runs=runs)
        self.assertEqual(games, [self.A]); self.assertEqual(self._games_arg(runs)[0], [self.A])
        self.assertEqual(len(notes), 1); self.assertIn(self.B, notes[0][0]); self.assertEqual(notes[0][1], "warning")

    def test_assemble_own_game_no_kit(self):
        """本款没有容器件 / 容器件不是本壳出的且不兼容 → container_stale"""
        self._library((self.A, "甲"))
        with self.assertRaises(Failure) as e: self._assemble(self.A)
        self.assertEqual(e.exception.code, "container_stale")
        ios_fixture(self.c, self.A, shell="SID1", compat=dict(hostAbi=1, dmshChain="bbbb"))
        with self.assertRaises(Failure) as e: self._assemble(self.A, sid="SID2")
        self.assertEqual(e.exception.code, "container_stale")

    def _phone(self, key="dev1"):
        pushed, signed = [], []
        ph = um.Mock()
        ph.key = key; ph.bid = install.container_bundle_id("TEAM1"); ph.info = dict(device="d", team="t", free=True)
        ph.install.side_effect = lambda ipa, games, steps=True: signed.append((list(games), install_ipa_games(ipa)))
        ph.push.side_effect = lambda g, pack: (pushed.append(g), (1, 0))[1]
        ph.before_push.return_value = None
        return ph, pushed, signed

    def _update(self, sid="SID2", **req):
        """「更新 iPhone 上的 S2Arm」夹具：甲兼容新宿主（只换壳）、乙新宿主认不得（要重新出包）；手机记录是旧壳 SID1、两款资源包都没变"""
        A, B = self.A, self.B
        self._library((A, "甲"), (B, "乙款"))
        ios_fixture(self.c, A, shell="SID1", pack="pa", compat=self.COMPAT); ios_fixture(self.c, B, shell="SID1", pack="pb", compat=dict(hostAbi=2, dmshChain="aaaa"))
        layout.save_device(self.c, "dev1", dict(games={A: dict(pack="pa"), B: dict(pack="pb")}, shellId="SID1"))
        ph, pushed, signed = self._phone()
        import ipa as packer, host_update
        self.sid = sid
        shell = self.c.private / "shell"; shell.mkdir(exist_ok=True); (shell / "manifest.json").write_text("{}")
        self.c.r = dict(self.c.r, **req)
        for p in (um.patch.object(packer, "current_shell_id", return_value=sid), um.patch.object(packer, "container_shell", return_value=shell),
                  um.patch.object(host_update, "engine_compat", return_value=dict(self.COMPAT)), um.patch.object(self.c, "run", self._run([])),
                  um.patch.object(install, "phone_for", lambda c: ph)):
            p.start(); self.addCleanup(p.stop)
        return ph, pushed, signed

    def test_update_phone_offline_target_but_connected_one_is_current(self):
        """#900：更新要找的那台（dev1，壳旧）不在线，连着的另一台（dev2）已是本版壳 → 说清是两台、连着的这台不用更新，码仍是 device_not_connected；
        连着的那台也是旧壳 / 没有别的在线设备 → 照原来的「不在线」原文"""
        self._update(device="dev1")
        layout.save_device(self.c, "dev2", dict(games={}, shellId="SID2"))
        offline = Failure("要装的那台设备（编号末尾 dev1）现在不在线：……", code="device_not_connected")
        with um.patch.object(install, "phone_for", side_effect=offline), um.patch.object(install, "online_devices", return_value={"dev2"}):
            with self.assertRaises(Failure) as e:
                install.update_phone(self.c)
        self.assertEqual(e.exception.code, "device_not_connected")
        self.assertIn("要更新的是另一台设备（编号末尾 dev1）", str(e.exception)); self.assertIn("（编号末尾 dev2）已经是新版 S2Arm，不用更新", str(e.exception))
        with um.patch.object(install, "phone_for", side_effect=offline), um.patch.object(install, "online_devices", return_value=set()):
            with self.assertRaises(Failure) as e:
                install.update_phone(self.c)
        self.assertEqual(str(e.exception), str(offline))

    def test_built_shell_reads_manifest_falls_back_to_tree(self):
        import json as _json
        import ipa as packer
        shell = self.c.private / "shellx"; shell.mkdir()
        with um.patch.object(packer, "current_shell_id", return_value="TREE"), um.patch.object(packer, "container_shell", return_value=shell):
            (shell / "manifest.json").write_text(_json.dumps(dict(details=dict(shellId="OLD"))))
            self.assertEqual(packer.built_shell(self.c), (shell, "OLD"))
            (shell / "manifest.json").write_text("{}")
            self.assertEqual(packer.built_shell(self.c), (shell, "TREE"), "manifest 没写壳号才退回树的")

    def test_phone_updates_from_device_records(self):
        """游戏库页检测：只读设备记录——壳旧了列出各款和要重新出包的乙；壳是本版 → 空"""
        A, B = self.A, self.B
        self._update()
        got = install.phone_updates(self.c)
        self.assertEqual([(u["device"], u["fromShell"], u["toShell"], [g["gameId"] for g in u["games"]], [g["gameId"] for g in u["stale"]]) for u in got],
                         [("dev1", "SID1", "SID2", [A, B], [B])])
        import ipa as packer
        with um.patch.object(packer, "current_shell_id", return_value="SID1"):
            self.assertEqual(install.phone_updates(self.c), [])

    def test_phone_updates_lists_kept_modules_on_current_shell(self):
        """壳是本版、但甲有运行时留下、已收回还没补进 iPhone 件的 NRO（#8be819）→ 照样列这台，kept = [甲]；没有留下的 → 空"""
        A = self.A
        self._update()
        import ipa as packer, rewrite_refresh
        with um.patch.object(packer, "current_shell_id", return_value="SID1"), \
             um.patch.object(rewrite_refresh, "kept_todo", side_effect=lambda c, g, base: ["x.nro"] if g == A else []):
            got = install.phone_updates(self.c)
        self.assertEqual([(u["device"], u["fromShell"], u["toShell"], [g["gameId"] for g in u["kept"]], u["stale"]) for u in got], [("dev1", "SID1", "SID1", [A], [])])

    def test_phone_kept_pull(self):
        """phoneKeptPull 作业：连着的手机上各款留下的 NRO 拉回 → new = 新收个数；没在玩就补推那几款（pushed），正在玩 / 查不清留在 kept；没连手机 → new 0、不报错"""
        A = self.A
        self._update()
        ph = types.SimpleNamespace(key="dev1", pull_kept=lambda games: 2 if A in games else 0, app_running=lambda: False)
        import rewrite_refresh
        pushed = []
        with um.patch.object(install, "phone_for", return_value=ph), um.patch.object(install, "push_game", lambda c, g, p, steps=True: pushed.append(g)), \
             um.patch.object(rewrite_refresh, "kept_todo", side_effect=lambda c, g, base: ["x.nro"] if g == A else []):
            r = install.phone_kept_pull(self.c)
        self.assertEqual((r["device"], r["new"], r["kept"], r["pushed"], r["running"]), ("dev1", 2, [], [A], False))   # 没在玩：收回的 NRO 当场补推这一款
        self.assertEqual(pushed, [A])
        for state, want in ((True, True), (None, True)):   # 正在玩 / 查不清：running 为真，启动器这一轮不推
            ph.app_running = lambda s=state: s
            with um.patch.object(install, "phone_for", return_value=ph), \
                 um.patch.object(rewrite_refresh, "kept_todo", side_effect=lambda c, g, base: ["x.nro"] if g == A else []):
                self.assertEqual(install.phone_kept_pull(self.c)["running"], want)
        def none(c): raise Failure("没连手机", code="device_not_connected")
        with um.patch.object(install, "phone_for", side_effect=none):
            self.assertEqual(install.phone_kept_pull(self.c), dict(device="", new=0, kept=[], shellUpdate=False, stale=[], running=False))

    def test_phone_kept_pull_shell_update(self):
        """启动器升级顺带更新 iPhone App（11-16）：连着的手机壳比本版旧、没有要重出包的款 → shellUpdate 真（没在玩时启动器自动更新）；
        有新壳认不得的款 → shellUpdate 照样真、stale 列出（装壳不碰各款，更新后在总控台补推）；壳是本版 → shellUpdate 假；正在玩 / 查不清 → running 真"""
        A = self.A
        self._update()
        ph = types.SimpleNamespace(key="dev1", pull_kept=lambda games: 0, app_running=lambda: False)
        import ipa as packer, rewrite_refresh
        def run(sid, usable=True, running=False):
            ph.app_running = lambda: running
            with um.patch.object(install, "phone_for", return_value=ph), um.patch.object(packer, "current_shell_id", return_value=sid), \
                 um.patch.object(packer, "usable_kit", return_value=("kit" if usable else None)), \
                 um.patch.object(rewrite_refresh, "kept_todo", return_value=[]):
                r = install.phone_kept_pull(self.c)
            return (r["shellUpdate"], r["stale"], r["running"])
        self.assertEqual(run("NEWSHELL"), (True, [], False))
        games = sorted(install.layout.load_device(self.c, "dev1")["games"])   # 夹具这台装了两款
        self.assertEqual(len(games), 2)
        self.assertEqual(run("NEWSHELL", usable=False), (True, games, False))   # 装壳不碰各款：新壳认不得的款也照样可以更新，更新后总控台标建议更新
        self.assertEqual(run("NEWSHELL", running=True), (True, [], True))
        self.assertEqual(run("NEWSHELL", running=None), (True, [], True))
        cur = install.layout.load_device(self.c, "dev1").get("shellId")
        self.assertEqual(run(cur), (False, [], False))

    def test_playing_by_heartbeat(self):
        """Windows 判正在玩（11-16）：读各款 Documents/games/<程序号>/playtime.json 的 lastPlayed；45 秒内 → 在玩；
        更早 / 没这个文件 → 没在玩；全都读不了 → 查不清（None）；没有款 → 没在玩"""
        import tempfile as tf
        work = Path(tf.mkdtemp()); self.addCleanup(lambda: shutil.rmtree(work, ignore_errors=True))
        class Fake:
            def __init__(self, files, boom=False): self.files, self.boom, self.asked = files, boom, []
            def get(self, rel, dst, t=None):
                self.asked.append(rel)
                if self.boom: raise OSError("device locked")
                if rel not in self.files: return False
                Path(dst).write_text(json.dumps(dict(schema=1, seconds=100, lastPlayed=self.files[rel]))); return True
        now = 1_000_000
        A, B = "0100000000010000", "0100000000020000"
        f = Fake({f"Documents/games/{A}/playtime.json": now - 10})
        self.assertIs(install.playing_by_heartbeat(f, [A, B], work, now), True)
        self.assertEqual(f.asked[0], f"Documents/games/{A}/playtime.json")
        self.assertIs(install.playing_by_heartbeat(Fake({f"Documents/games/{A}/playtime.json": now - 600}), [A, B], work, now), False)
        self.assertIs(install.playing_by_heartbeat(Fake({}), [A], work, now), False, "没玩过：文件不在")
        self.assertIsNone(install.playing_by_heartbeat(Fake({}, boom=True), [A, B], work, now), "进不了容器：查不清")
        self.assertIs(install.playing_by_heartbeat(Fake({}), [], work, now), False)

    def test_phone_kept_pull_uses_heartbeat_when_process_unknown(self):
        """查不了进程（app_running None，Windows）：改按 playing（心跳）判；playing 也查不清 → 当在玩（不装）"""
        self._update()
        ph = types.SimpleNamespace(key="dev1", pull_kept=lambda games: 0, app_running=lambda: None)
        import ipa as packer, rewrite_refresh
        for heartbeat, want in ((False, False), (True, True), (None, True)):
            ph.playing = lambda games, h=heartbeat: h
            with um.patch.object(install, "phone_for", return_value=ph), um.patch.object(packer, "current_shell_id", return_value="NEWSHELL"), \
                 um.patch.object(packer, "usable_kit", return_value="kit"), um.patch.object(rewrite_refresh, "kept_todo", return_value=[]):
                self.assertEqual(install.phone_kept_pull(self.c)["running"], want, heartbeat)

    def test_phone_kept_pull_many_devices(self):
        """多台手机连着、请求没说哪台（11-16 候选验收：Mac 插着两部 iPhone，2 分钟循环每次 device=""，自动更新永远不触发）：
        按设备记录逐台问——有事且没在玩的那台先回；有事的都在玩回第一台有事的；都没事回第一台；
        选机退到别的手机（这台没连着、只连着另一台）的记录跳过，不把那台的结论安在它头上；问完请求里的 device 还原"""
        self._update()
        A = self.A
        import ipa as packer, rewrite_refresh
        state = dict(online={"dev1", "dev2"}, running={})
        def pf(c):
            want = (c.r.get("device") or "").strip()
            if not want:
                raise Failure("多台", code="device_required")
            on = state["online"]
            if want not in on:
                if len(on) == 1:   # 同 _pick_device / win_udid：给的那台没列着、只连着一台 → 退到那一台
                    return types.SimpleNamespace(key=next(iter(on)), pull_kept=lambda g: 0, app_running=lambda: False)
                raise Failure("不在线", code="device_not_connected")
            return types.SimpleNamespace(key=want, pull_kept=lambda g: 0, app_running=lambda w=want: state["running"].get(w, False))
        def run(shell1, shell2, online=("dev1", "dev2"), running=None):
            layout.save_device(self.c, "dev1", dict(games={A: dict(pack="pa")}, shellId=shell1))
            layout.save_device(self.c, "dev2", dict(games={A: dict(pack="pa")}, shellId=shell2))
            state["online"], state["running"] = set(online), dict(running or {})
            with um.patch.object(install, "phone_for", pf), um.patch.object(packer, "current_shell_id", return_value="SID2"), \
                 um.patch.object(packer, "usable_kit", return_value="kit"), um.patch.object(rewrite_refresh, "kept_todo", return_value=[]):
                r = install.phone_kept_pull(self.c)
            self.assertNotIn("device", self.c.r, "问完还原请求（原来没有 device）")
            return (r["device"], r["shellUpdate"], r["running"])
        self.assertEqual(run("SID2", "SID1"), ("dev2", True, False), "第一台已是本版、第二台壳旧 → 回第二台")
        self.assertEqual(run("SID1", "SID1", running={"dev1": True}), ("dev2", True, False), "都壳旧、第一台在玩 → 先回没在玩的")
        self.assertEqual(run("SID1", "SID1", running={"dev1": True, "dev2": True}), ("dev1", True, True), "都在玩 → 回第一台有事的（启动器这轮不装）")
        self.assertEqual(run("SID2", "SID2"), ("dev1", False, False), "都没事 → 第一台")
        self.assertEqual(run("SID2", "SID1", online=("dev1",)), ("dev1", False, False), "dev2 没连着、选机退到 dev1：不把 dev2 的壳旧安到 dev1 头上")
        def none(c): raise Failure("没连手机", code="device_not_connected")
        with um.patch.object(install, "phone_for", side_effect=none):
            self.assertEqual(install.phone_kept_pull(self.c)["device"], "")
        self.c.r["device"] = "dev1"   # 请求给了哪台、那台多台不明（device_required 不该在给了 device 时出现）：照旧空结果、不逐台问
        def many(c): raise Failure("多台", code="device_required")
        with um.patch.object(install, "phone_for", side_effect=many):
            self.assertEqual(install.phone_kept_pull(self.c)["device"], "")
        self.assertEqual(self.c.r["device"], "dev1")

    def test_app_running(self):
        """自动推送前查手机上 S2Arm 是否在跑（#8be819）：进程在它安装目录下 = True；没有 = False；没装 = False；devicectl 查不了 = None"""
        url = "file:///private/var/containers/Bundle/Application/X/S2Arm.app/"
        def fake(procs, apps_rc=0, ps_rc=0, installed=True):
            def devicectl(c, *a, timeout=0):
                if a[2] == "apps": return apps_rc, {"result": {"apps": [{"bundleIdentifier": "b", "url": url}] if installed else []}}, ""
                return ps_rc, {"result": {"runningProcesses": [{"executable": e} for e in procs]}}, ""
            return devicectl
        import diag
        cases = [(fake([url + "S2Arm"]), True), (fake(["file:///System/Library/CoreServices/SpringBoard.app/SpringBoard"]), False),
                 (fake([url + "S2Arm"], installed=False), False), (fake([], apps_rc=1), None), (fake([], ps_rc=1), None),
                 (fake(["file:///private/var/containers/Bundle/Application/X/S2Arm.app2/S2Arm"]), False),
                 # 只有主屏小组件扩展进程在（App 已退出）：不算在玩（11-16 实测原先判 True，自动补件从不发起）
                 # 11-16 真机：App 已退出、只剩主屏小组件扩展进程 → 不算在玩；只有主程序 / 两个都在 → 在玩；安装目录下别的可执行文件也不算
                 (fake([url + "PlugIns/S2ArmWidget.appex/S2ArmWidget"]), False),
                 (fake([url + "S2Arm"]), True),
                 (fake([url + "PlugIns/S2ArmWidget.appex/S2ArmWidget", url + "S2Arm"]), True),
                 (fake([url + "Frameworks/Helper.framework/Helper"]), False),
                 (fake([url + "S2ArmHelper"]), False)]
        for f, want in cases:
            with um.patch.object(diag, "devicectl", f):
                self.assertEqual(install.app_running(self.c, "dev", "b"), want)

    def test_install_mode_job_reads_signing_and_drops_old_setting(self):
        """11-15 导出不再是装法：installMode 作业只回签名身份；旧版留下的 install.json（装法设置）迁移时删掉，之后导不导出只看请求带的 mode"""
        self.assertEqual(install.install_mode_job(self.c), dict(exportAvailable=True, signMode="appleId", signChoice="appleId"))
        self.assertFalse((self.c.private / "install.json").exists(), "旧装法设置迁移一次就删")
        self.assertFalse(install.wants_export(self.c), "没带 mode：一键装")
        self.c.r = dict(self.c.r, mode="export"); self.assertTrue(install.wants_export(self.c))
        self.c.r = dict(self.c.r, mode="device"); self.assertFalse(install.wants_export(self.c))
        (self.c.private / "install.json").write_text("{}")   # 删不掉（Windows 上被占用）：作业照常回签名身份，不失败
        with um.patch("pathlib.Path.unlink", side_effect=PermissionError(13, "in use")):
            self.assertEqual(install.install_mode_job(self.c)["signMode"], "appleId")
    def test_other_signer_install_error_is_named(self):
        """玩家先自签装过同包名的 S2Arm：系统安装服务拒绝覆盖的两种原文都认成 app_other_signer（Mac devicectl / Windows 同一句），不落到笼统的装机失败"""
        import impactor
        for raw in ("ERROR: MismatchedApplicationIdentifierEntitlement",
                    "Upgrade's application-identifier entitlement string (AB12CD34EF.io.s2arm.games.team1) does not match installed application's application-identifier string (ZZ99YY88XX.io.s2arm.games.team1); rejecting upgrade."):
            with self.subTest(raw=raw[:40]):
                self.assertEqual((install.classify(raw, default="device_install_failed"), impactor.explain(raw)), ("app_other_signer", "app_other_signer"))

    def _mac_phone(self, taken):
        """MacPhone 替身：sign_and_install 对 taken 里的包名报 app_id_taken（Apple 9401 原文分类出的码），其余记下装上的包名"""
        ph = object.__new__(install.MacPhone)
        ph.c, ph.team, ph.dev, ph.udid = self.c, dict(id="TEAM1", name="t", free=True), dict(id="D1"), "U1"
        ph.bid = install.container_bundle_id("TEAM1", self.c)
        installed = []
        def sign(c, ipa, team, dev, udid, bid, games, steps=True):
            if bid in taken: raise Failure("An App ID with Identifier '%s' is not available." % bid, code="app_id_taken")
            installed.append(bid)
        p = um.patch.object(install, "sign_and_install", sign); p.start(); self.addCleanup(p.stop)
        return ph, installed

    def test_taken_bundle_id_switches_once_and_is_remembered(self):
        """包名被别处占用（玩家自签时注册过）：自动换 <包名>.s2 装好、记下、黄字说明；之后算包名直接是换过的"""
        ph, installed = self._mac_phone({"io.s2arm.games.team1"})
        ph.install("x.ipa", ["0100A"])
        self.assertEqual((installed, ph.bid), (["io.s2arm.games.team1.s2"], "io.s2arm.games.team1.s2"))
        self.assertEqual(install.container_bundle_id("TEAM1", self.c), "io.s2arm.games.team1.s2")
        self.assertEqual(install.container_bundle_id("TEAM1"), "io.s2arm.games.team1")   # 不给 c（开发用 / 旧调用）照旧
        self.assertEqual([d["code"] for d in self.c.job["diagnostics"] if d["code"] == "bundle_id_switched"], ["bundle_id_switched"])

    def test_alternate_also_taken_reports_app_id_taken(self):
        ph, installed = self._mac_phone({"io.s2arm.games.team1", "io.s2arm.games.team1.s2"})
        with self.assertRaises(Failure) as e: ph.install("x.ipa", ["0100A"])
        self.assertEqual((e.exception.code, installed), ("app_id_taken", []))
        self.assertEqual(install.container_bundle_id("TEAM1", self.c), "io.s2arm.games.team1")   # 没装成不记

    def test_devcert_does_not_switch(self):
        """证书模式的包名由描述文件定，换不了：照报 app_id_taken"""
        ph, installed = self._mac_phone({"io.s2arm.games.team1"})
        with um.patch.object(install.devcert, "active", lambda c: True), self.assertRaises(Failure) as e:
            ph.install("x.ipa", ["0100A"])
        self.assertEqual((e.exception.code, installed), ("app_id_taken", []))

    def _app_gone(self, ph):
        """玩家在手机上删了整个 S2Arm：进不了容器、平台确认这个包名没装（走真的 install.phone_games，不用夹具里的替身）"""
        class Cl:
            def get(self_, *a, **k): return False   # 手机账本 Documents/.s2a-games.json：这些替身里没有（旧 App）
            def call(self, op, rel): return 1, "取不到 x 的容器（App 没装或不是开发签名）：device socket io failed"
        ph.udid = "U1"; ph.app_missing.return_value = True
        for p in (um.patch.object(install, "phone_games", _phone_games), um.patch.object(install, "devpush_client", lambda c, udid, bid, work: Cl())):
            p.start(); self.addCleanup(p.stop)

    def test_commit_phone_ipa_keeps_previous_only_when_shell_changes(self):
        """③ 同壳覆盖 S2Arm.ipa（不动上一版）；换壳时原来的 S2Arm.ipa 留作 S2Arm.previous.ipa；组装用的临时目录删掉"""
        import ipa as packer
        cur, prev = self.c.library / "S2Arm.ipa", self.c.library / "S2Arm.previous.ipa"
        def staged(sid, games):
            return self._ipa(self.c.library / (".container-" + sid + "-" + "-".join(games)) / "S2Arm.ipa", sid, games)
        t1 = staged("SID1", [self.A])
        packer.commit_phone_ipa(self.c, t1, "SID1")                    # 第一次：没有原来的
        self.assertEqual(install_ipa_games(cur), ([self.A], "SID1")); self.assertFalse(prev.exists()); self.assertFalse(t1.parent.exists())
        packer.commit_phone_ipa(self.c, staged("SID1", [self.A, self.B]), "SID1")   # 同壳加款：覆盖
        self.assertEqual(install_ipa_games(cur), ([self.A, self.B], "SID1")); self.assertFalse(prev.exists())
        packer.commit_phone_ipa(self.c, staged("SID2", [self.A]), "SID2")           # 换壳：原来的留作上一版
        self.assertEqual((install_ipa_games(cur), install_ipa_games(prev)), (([self.A], "SID2"), ([self.A, self.B], "SID1")))
        packer.commit_phone_ipa(self.c, staged("SID2", [self.A, self.C]), "SID2")   # 新壳上再加款：上一版不被冲掉
        self.assertEqual((install_ipa_games(cur), install_ipa_games(prev)), (([self.A, self.C], "SID2"), ([self.A, self.B], "SID1")))

    def test_restore_previous_swaps_and_records_failed_shell(self):
        """④ 装回上一版：装的是 S2Arm.previous.ipa；装上后现行与上一版对调；被换下的壳 SID2 记进 failedShells；设备记录壳号改回 SID1、只留上一版里有的款（资源包号保留）"""
        A, B = self.A, self.B
        self._ipa(self.c.library / "S2Arm.ipa", "SID2", [A, B]); self._ipa(self.c.library / "S2Arm.previous.ipa", "SID1", [A])
        layout.save_device(self.c, "dev1", dict(games={A: dict(pack="pa"), B: dict(pack="pb")}, shellId="SID2", bundleId="io.s2arm.games.team1", failedShells=["SID0"]))
        ph, _, signed = self._phone()
        with um.patch.object(install, "phone_for", return_value=ph):
            r = install.restore_previous(self.c)
        self.assertEqual(signed, [([A], ([A], "SID1"))])
        self.assertEqual(install_ipa_games(self.c.library / "S2Arm.ipa"), ([A], "SID1"))
        self.assertEqual(install_ipa_games(self.c.library / "S2Arm.previous.ipa"), ([A, B], "SID2"))
        rec = layout.load_device(self.c, "dev1")
        self.assertEqual((rec["shellId"], rec["failedShells"], rec["games"]), ("SID1", ["SID0", "SID2"], {A: dict(pack="pa")}))
        self.assertEqual((r["restoredShell"], r["games"]), ("SID1", [A]))
        # 再点一次 = 换回 SID2：SID2 从 failedShells 去掉（装回的这一版不算不好用），换下的 SID1 记进去
        with um.patch.object(install, "phone_for", return_value=ph):
            install.restore_previous(self.c)
        rec = layout.load_device(self.c, "dev1")
        self.assertEqual((rec["shellId"], rec["failedShells"]), ("SID2", ["SID0", "SID1"]))
        self.assertEqual(install_ipa_games(self.c.library / "S2Arm.ipa"), ([A, B], "SID2"))
        # 没有上一版：不找设备、不装
        (self.c.library / "S2Arm.previous.ipa").unlink()
        with um.patch.object(install, "phone_for", side_effect=AssertionError("不该找设备")):
            with self.assertRaises(Failure) as e: install.restore_previous(self.c)
        self.assertEqual(e.exception.code, "job_failed")

    def test_remove_from_phone_not_in_container(self):
        """预检清单 #35（11-13）：本机记录里没有这款，问手机——手机上也没有 → 不用删、照成功返回（alreadyAbsent），不装；
        手机上有（别的电脑装的）→ 照删：备份存档、删机上它的数据，不重装 App。期望手写"""
        layout.save_device(self.c, "dev1", dict(games={self.A: {}}))
        ph, _, _ = self._phone()
        self.c.r.update(gameId=self.B)
        with um.patch.object(install, "phone_for", return_value=ph), um.patch.object(install, "phone_games", return_value={self.A}):
            r = install.remove_from_phone(self.c)
        self.assertTrue(r["alreadyAbsent"]); ph.install.assert_not_called(); ph.purge.assert_not_called()
        ph.backup.return_value = None; ph.purge.return_value = (True, ""); ph.app_running.return_value = False
        with um.patch.object(install, "phone_for", return_value=ph), um.patch.object(install, "phone_games", return_value={self.A, self.B}):
            r = install.remove_from_phone(self.c)
        self.assertNotIn("alreadyAbsent", r); ph.install.assert_not_called(); ph.purge.assert_called_once_with(self.B)

    def test_remove_from_phone_last_game(self):
        """最后一款（#227）：备份存档、删手机上它的数据，不装空容器；设备记录摘空"""
        layout.save_device(self.c, "dev1", dict(games={self.A: dict(pack="pa")}))
        ph, _, _ = self._phone(); ph.backup.return_value = None; ph.purge.return_value = (True, "")
        self.c.r.update(gameId=self.A)
        with um.patch.object(install, "phone_for", return_value=ph), um.patch.object(install, "phone_games", return_value={self.A}):
            r = install.remove_from_phone(self.c)
        ph.install.assert_not_called()
        ph.backup.assert_called_once_with(self.A); ph.purge.assert_called_once_with(self.A)
        self.assertEqual((r["games"], layout.load_device(self.c, "dev1")["games"]), ([], {}))

    def test_remove_from_phone_after_app_deleted_does_not_reinstall(self):
        """玩家在手机上删了整个 S2Arm，再在电脑上「从 iPhone 删除」甲：记录里的乙也不在手机上，不现组容器、不把 App 装回去；设备记录摘空"""
        A, B = self.A, self.B
        self._library((A, "甲"), (B, "乙")); ios_fixture(self.c, B)
        layout.save_device(self.c, "dev1", dict(games={A: dict(pack="pa"), B: dict(pack="cafe01")}, shellId="SID1"))
        ph, _, _ = self._phone(); ph.backup.return_value = None; ph.purge.return_value = (False, "取不到容器"); self._app_gone(ph)
        self.c.r.update(gameId=A)
        import ipa as packer, host_update
        self.sid = "SID1"
        shell = self.c.private / "shell"; shell.mkdir(exist_ok=True); (shell / "manifest.json").write_text("{}")
        with um.patch.object(install, "phone_for", return_value=ph), um.patch.object(packer, "current_shell_id", return_value="SID1"), \
             um.patch.object(packer, "container_shell", return_value=shell), um.patch.object(host_update, "engine_compat", return_value=dict(self.COMPAT)), \
             um.patch.object(self.c, "run", self._run([])):
            r = install.remove_from_phone(self.c)
        ph.install.assert_not_called(); ph.purge.assert_not_called()   # App 都没了：不去删数据，只把记录摘空
        self.assertTrue(r["alreadyAbsent"])
        self.assertEqual((r["games"], layout.load_device(self.c, "dev1")["games"]), ([], {}))

def library_pack(c, gid):
    return layout.ios_pack(c, gid)


def img_module():
    """pack/img.py（按文件加载：把 pack/ 放进 sys.path 会让 pack/pack.py 盖住 pack 包）"""
    import importlib.util
    spec = importlib.util.spec_from_file_location("s2a_pack_img", Path(__file__).resolve().parents[2] / "img.py")
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


def install_ipa_games(ipa):
    import ipa as packer
    return packer.ipa_games(ipa)


class ShipExtContainerDropTests(unittest.TestCase):
    """④ ship_ext container-drop：其余款字节不变、catalogue 少一款、最后一款拒绝"""

    def setUp(self):
        self.t = tempfile.mkdtemp()
        import shutil; self.addCleanup(lambda: shutil.rmtree(self.t, ignore_errors=True))
        self.base = Path(self.t) / "base.app"
        self.base.mkdir()

    def _make_container(self, games):
        import struct
        for g in games:
            gdir = self.base / "Games" / g; gdir.mkdir(parents=True, exist_ok=True)
            # 写一个伪可执行文件，带游戏特定字节，用于验证字节不变
            (gdir / "main.npdm").write_bytes(g.encode() + bytes(16))
            (self.base / "Frameworks").mkdir(exist_ok=True)
            (self.base / f"Frameworks/S2AImages-{g}.dylib").write_bytes(g.encode())
        (self.base / "s2a_catalogue.json").write_text(json.dumps({"schema": 1, "games": [{"title_id": g} for g in games]}))
        return self.base

    def _drop(self, game):
        import sys; sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
        import ship_ext as se
        import argparse
        out = Path(self.t) / "out.app"
        a = argparse.Namespace(base=str(self.base), game=game, out=str(out))
        se.cmd_container_drop(a)
        return out

    def test_drop_removes_game_and_keeps_others(self):
        A, B = "0100000000010000", "0100000000020000"
        self._make_container([A, B])
        out = self._drop(A)
        cat = json.loads((out / "s2a_catalogue.json").read_text())
        left = [g["title_id"] for g in cat["games"]]
        self.assertEqual(left, [B])
        # B 的文件字节不变
        self.assertEqual((out / "Games" / B / "main.npdm").read_bytes(), B.encode() + bytes(16))
        # A 的目录已删
        self.assertFalse((out / "Games" / A).exists())

    def test_drop_last_game_leaves_empty_container(self):
        """#227：摘最后一款不再拒，得到 0 款的容器（游戏库侧下次加款的底子），它的目录和镜像包都没了"""
        A = "0100000000010000"
        self._make_container([A])
        out = self._drop(A)
        self.assertEqual(json.loads((out / "s2a_catalogue.json").read_text())["games"], [])
        self.assertFalse((out / "Games" / A).exists()); self.assertFalse((out / f"Frameworks/S2AImages-{A}.dylib").exists())


if __name__ == "__main__":
    unittest.main()


class ApplyFailureReasonTests(unittest.TestCase):
    """手机上解开失败（如 #569 目录 App 写不进）：tools/push_pack.py 按解包日志「解包失败：…」当场报原因，装机作业认这一句、原因原样上屏。
    两边原句对不上会红（期望手写）"""
    def test_job_reads_the_push_tool_sentence(self):
        import install
        tool = (Path(install.__file__).resolve().parents[2] / "tools/push_pack.py").read_text(encoding="utf-8")
        self.assertIn('f"资源包推进去了，但手机上解开失败：{', tool)
        log = "[12:00:09] 【红】资源包推进去了，但手机上解开失败：目录 App 写不进：资源包/.incoming（属主 uid 0、权限 755，App 是 uid 501）\n"
        self.assertEqual(install.APPLY_FAILED.search(log).group(1), "目录 App 写不进：资源包/.incoming（属主 uid 0、权限 755，App 是 uid 501）")


class DevicectlTimeoutTests(unittest.TestCase):
    """#333：devicectl 超时（锁屏 / 隧道断时一直挂）转成非零返回码 124 + 人话原因，不再抛 TimeoutExpired 让装机任务裸 traceback 收场"""
    def test_timeout_becomes_rc_124(self):
        import subprocess, diag
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / "Resources/bin/devicectl"; exe.parent.mkdir(parents=True); exe.write_text("#!/bin/sh\n"); exe.chmod(0o755)
            c = types.SimpleNamespace(private=Path(d), env=lambda: dict(os.environ))
            with um.patch.object(diag, "COREDEVICE", Path(d)), \
                 um.patch.object(diag.subprocess, "run", side_effect=subprocess.TimeoutExpired("devicectl", 120)):
                rc, data, err = diag.devicectl(c, "device", "info", "files", timeout=120)
        self.assertEqual((rc, data, err), (124, {}, "devicectl 等了 120 秒没有返回，已停止等待"))   # 如实说超时，不猜锁屏（剃刀 H21）


class WidgetSignTests(unittest.TestCase):
    """主屏小组件扩展（11-16 小组件 B 半）：Mac 装机按签名方式带 / 去扩展；带不成去掉重签、照常装，黄字说原因"""
    ENGINE = Path(__file__).resolve().parents[3]

    def test_widget_mod_imports_from_the_engine_tree_like_the_mac_app(self):
        """Mac App 里作业层在 Contents/Resources/Jobs、引擎在 Resources/Engine：widget_mod 要按请求的引擎树导入 pack/widget_ext.py。
        0.5.0 正式候选 667ccdb92 按本文件往上两级找（= Contents，没有 pack），装到 iPhone 一律 ModuleNotFoundError。
        新开隔离的 Python（-I：仓库根不在 sys.path），把 install.py 的位置换成 App 里那样，引擎给仓库根"""
        jobs = Path(install.__file__).resolve().parent
        code = ("import sys, types; sys.path.insert(0, sys.argv[1]); import install; "
                "install.__file__ = sys.argv[2]; "
                "m = install.widget_mod(types.SimpleNamespace(engine=sys.argv[3])); print('ok' if hasattr(m, 'retarget') else 'no')")
        fake = self.tmp / "S2Arm.app/Contents/Resources/Jobs/install.py"
        r = subprocess.run([sys.executable, "-I", "-c", code, str(jobs), str(fake), str(self.ENGINE)], capture_output=True, text=True, cwd=self.tmp)
        self.assertEqual((r.returncode, r.stdout.strip()), (0, "ok"), r.stderr[-800:])

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="s2a-widget-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.app = self.tmp / "S2Arm.app"
        x = self.app / "PlugIns/S2ArmWidget.appex"; x.mkdir(parents=True)
        (x / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": "io.s2arm.shell.widget", "CFBundleExecutable": "S2ArmWidget"}))
        (x / "S2ArmWidget").write_bytes(b"bin")
        self.ents = self.tmp / "app.entitlements"
        self.ents.write_bytes(plistlib.dumps({"com.apple.developer.kernel.increased-memory-limit": True}))
        self.log = self.tmp / "install.log"
        self.runs, self.fail_on = [], None
        self.prof = set()   # 本机已有描述文件的包名（替身）：没有时 dev_sign 退出码 3
        def run(args, log=None, env=None, cwd=None, on_line=None, ok_codes=(0,)):
            a = [str(v) for v in args]
            seen = dict(args=a, env=env)
            ent = next((v for v in a if v.endswith(".entitlements")), None)
            if ent: seen["ents"] = plistlib.loads(Path(ent).read_bytes())
            self.runs.append(seen)
            if a[0] != "bash" and 3 in ok_codes and a[3] not in self.prof:
                self.c.last_rc = 3; return
            if self.fail_on and self.fail_on(a):
                self.log.write_text("error: Developer API error 9120: maximum number of App IDs\n", encoding="utf-8")
                raise Failure("tool", code="tool_exit")
            if a[0] == "bash": self.prof.add(a[2])
        self.c = types.SimpleNamespace(engine=self.ENGINE, private=self.tmp, r={}, run=run, diagnostic=um.MagicMock(),
                                       begin=lambda *a, **k: None, env=lambda: {})

    def plain(self):
        return [(r["args"][1].rsplit("/", 1)[-1] if r["args"][0] == "bash" else r["args"][1].rsplit("/", 1)[-1], r["args"][2] if r["args"][0] == "bash" else r["args"][3]) for r in self.runs]

    def test_apple_id_signs_extension_first_with_shared_group(self):
        self.assertTrue(install.widget_prepare(self.c, self.app, "io.s2arm.games.abc"))
        self.assertEqual(plistlib.loads((self.app / "PlugIns/S2ArmWidget.appex/Info.plist").read_bytes())["CFBundleIdentifier"], "io.s2arm.games.abc.widget")
        install.sign_with_apple_id(self.c, self.app, {"id": "TEAM1"}, "io.s2arm.games.abc", self.ents, "UDID", self.log, False, widget=True)
        # 本机还没有两个包名的描述文件：每个都是「先签 → 退出码 3 → 申请 → 再签」，扩展先、主程序后
        self.assertEqual(self.plain(), [("dev_sign.py", "io.s2arm.games.abc.widget"), ("ensure_profile.sh", "io.s2arm.games.abc.widget"), ("dev_sign.py", "io.s2arm.games.abc.widget"),
                                        ("dev_sign.py", "io.s2arm.games.abc"), ("ensure_profile.sh", "io.s2arm.games.abc"), ("dev_sign.py", "io.s2arm.games.abc")])
        group = ["group.s2arm.widget.TEAM1"]
        self.assertEqual(self.runs[4]["ents"], {"com.apple.developer.kernel.increased-memory-limit": True, "com.apple.security.application-groups": group})
        self.assertEqual(self.runs[1]["ents"], {"com.apple.security.application-groups": group}, "扩展只带共享组，不带内存上限")
        self.assertIn("--extension", self.runs[2]["args"]); self.assertNotIn("--extension", self.runs[5]["args"])
        self.assertEqual(self.runs[0]["env"].get("S2A_PROFILE_NEED_UDIDS"), "UDID", "先签时要求描述文件含这台手机")
        self.c.diagnostic.assert_not_called()
        self.assertTrue((self.app / "PlugIns").is_dir())

    def test_quota_full_strips_widget_and_signs_app_as_before(self):
        install.widget_prepare(self.c, self.app, "io.s2arm.games.abc")
        self.fail_on = lambda a: a[0] == "bash" and a[2].endswith(".widget")   # 扩展包名注册被拒（Apple 9120）
        install.sign_with_apple_id(self.c, self.app, {"id": "TEAM1"}, "io.s2arm.games.abc", self.ents, "UDID", self.log, False, widget=True)
        self.assertFalse((self.app / "PlugIns").exists(), "带不成：去掉扩展")
        self.assertEqual(self.plain()[-3:], [("dev_sign.py", "io.s2arm.games.abc"), ("ensure_profile.sh", "io.s2arm.games.abc"), ("dev_sign.py", "io.s2arm.games.abc")])
        self.assertEqual(self.runs[-1]["args"][5], str(self.ents), "重签用原来的权限（不带共享组）")
        msg = self.c.diagnostic.call_args.args[0]
        self.assertEqual(self.c.diagnostic.call_args.kwargs, dict(code="widget_skipped", severity="warning"))
        self.assertTrue(msg.startswith("这次没装小组件：") and "10 个" in msg, msg)

    def test_existing_profile_signs_without_xcodebuild(self):
        # 诊断 D-44DP：本机已有含这台手机的描述文件 → 直接签，一次都不跑 ensure_profile.sh（xcodebuild 看不到这台手机时原先主程序也签不成）
        self.prof = {"io.s2arm.games.abc"}
        install.sign_with_apple_id(self.c, self.app, {"id": "TEAM1"}, "io.s2arm.games.abc", self.ents, "UDID", self.log, False)
        self.assertEqual(self.plain(), [("dev_sign.py", "io.s2arm.games.abc")])

    def test_main_app_failure_after_widget_fallback_still_raises(self):
        install.widget_prepare(self.c, self.app, "io.s2arm.games.abc")
        self.fail_on = lambda a: a[0] == "bash"   # 主包名也申请不到：去掉扩展重来一次还是不成 → 照原样报错，不吞
        with self.assertRaises(Failure) as e:
            install.sign_with_apple_id(self.c, self.app, {"id": "TEAM1"}, "io.s2arm.games.abc", self.ents, "UDID", self.log, False, widget=True)
        self.assertEqual(e.exception.code, "free_appid_limit")

    def test_player_off_and_devcert_strip(self):
        self.c.r["widget"] = False
        self.assertFalse(install.widget_prepare(self.c, self.app, "io.s2arm.games.abc"))
        self.assertFalse((self.app / "PlugIns").exists()); self.c.diagnostic.assert_not_called()
        self.setUp()
        self.assertFalse(install.widget_prepare(self.c, self.app, "io.s2arm.games.abc", devcert=True))
        self.assertFalse((self.app / "PlugIns").exists())
        self.assertEqual(self.c.diagnostic.call_args.kwargs["code"], "widget_skipped")
        self.setUp(); shutil.rmtree(self.app / "PlugIns")
        self.assertFalse(install.widget_prepare(self.c, self.app, "io.s2arm.games.abc"), "没有扩展的包：不带")


class WinWidgetInstallTests(unittest.TestCase):
    """Windows plumesign 装机带小组件（11-16）：带着签不成 → 去掉扩展重装一次、黄字；玩家关了 → 交去掉扩展的包；没扩展的包 → 原样交"""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="s2a-wwidget-")); self.addCleanup(shutil.rmtree, self.tmp, True)
        self.ipa = self.tmp / "S2Arm.ipa"
        with zipfile.ZipFile(self.ipa, "w") as z:
            i = zipfile.ZipInfo("Payload/S2Arm.app/S2Arm"); i.external_attr = 0o755 << 16; z.writestr(i, b"bin")
            z.writestr("Payload/S2Arm.app/PlugIns/S2ArmWidget.appex/Info.plist", b"plist")
            z.writestr("Payload/S2Arm.app/PlugIns/S2ArmWidget.appex/S2ArmWidget", b"ext")
        (self.tmp / "logs").mkdir()

    def run_install(self, fails=(), widget=None, expiry=None, info=None):
        given = []
        class Fail(Exception):
            def __init__(self, msg, err=None): super().__init__(msg); self.err = err
        def install_app(ns, say=None):
            with zipfile.ZipFile(ns.ipa) as z: given.append(sorted(z.namelist()))
            if len(given) <= len(fails): raise Fail("plumesign 退 1", fails[len(given) - 1])
            if expiry: ns.signed_expiry = expiry   # 同 install_win.install_app：签好的包里读到的真实期限
            return "io.s2arm.games.abcde12345"
        W = types.SimpleNamespace(Fail=Fail, install_app=install_app, plumesign_path=lambda x=None: "C:/plumesign.exe",
                                  impactor=types.SimpleNamespace(bundled=lambda r: {}, select_only_account=lambda exe: None))
        r = {} if widget is None else {"widget": widget}
        c = types.SimpleNamespace(r=r, private=self.tmp, logs=self.tmp / "logs", task="t", emit=lambda *a, **k: None, begin=lambda *a, **k: None,
                                  last_log=None, diagnostic=um.MagicMock())
        me = types.SimpleNamespace(W=W, c=c, udid="U", key="dev1", bid=None, backup=lambda g: None, bundle_for_signing=lambda: None, info=info if info is not None else {})
        me.say = lambda m: install.WinPhone.say(me, m)   # install_win 的进度回调（签好后核 IML 的那行也经它）
        self.me = me
        with um.patch.object(install.devcert, "active", return_value=False), um.patch.object(install.layout, "load_device", return_value={}), \
             um.patch.object(install.layout, "save_device"):
            install.WinPhone.install(me, self.ipa, [], steps=False)
        return given, c.diagnostic

    def test_quota_retry_without_widget(self):
        given, diag = self.run_install(fails=("free_appid_limit",))
        self.assertEqual(len(given), 2)
        self.assertIn("Payload/S2Arm.app/PlugIns/S2ArmWidget.appex/S2ArmWidget", given[0], "第一次带着扩展交")
        self.assertEqual(given[1], ["Payload/S2Arm.app/S2Arm"], "重来那次去掉扩展")
        self.assertEqual(diag.call_args.kwargs, dict(code="widget_skipped", severity="warning"))
        self.assertIn("10 个", diag.call_args.args[0])

    def test_second_failure_raises(self):
        with self.assertRaises(Failure):
            self.run_install(fails=("free_appid_limit", "device_not_connected"))

    def test_player_off_and_plain_success(self):
        given, diag = self.run_install(widget=False)
        self.assertEqual(given, [["Payload/S2Arm.app/S2Arm"]]); diag.assert_not_called()
        given, diag = self.run_install()
        self.assertEqual(len(given), 1); self.assertEqual(len(given[0]), 3); diag.assert_not_called()

    def test_real_expiry_sets_free_and_expires(self):
        # #1772：装好后按签好的包里的真实期限记（PlumeMacPhone 原先一律 free=True，付费账号显示 7 天）。期望手写
        self.run_install(expiry=dict(expires=1_900_000_000.0, days=365), info=dict(free=True, device="U"))
        self.assertEqual(self.me.info, dict(free=False, device="U", expires=1_900_000_000.0))
        self.run_install(expiry=dict(expires=1_800_000_000.0, days=7), info=dict(free=True, device="U"))
        self.assertEqual(self.me.info, dict(free=True, device="U", expires=1_800_000_000.0))
        self.run_install(info=dict(free=True, device="U"))   # 读不到：照旧
        self.assertEqual(self.me.info, dict(free=True, device="U"))

    def test_strip_keeps_exec_bit(self):
        out = self.tmp / "o.ipa"
        self.assertTrue(install.ipa_strip_widget(self.ipa, out))
        with zipfile.ZipFile(out) as z: self.assertEqual(z.getinfo("Payload/S2Arm.app/S2Arm").external_attr >> 16, 0o755)
        plain = self.tmp / "plain.ipa"; shutil.copy(out, plain)
        self.assertFalse(install.ipa_strip_widget(plain, self.tmp / "x.ipa")); self.assertFalse((self.tmp / "x.ipa").exists())


class UpdatePrepareTests(unittest.TestCase):
    """「更新 iPhone」电脑侧准备（installUpdatePrepare，update_prepare）：11-17 起 S 级优化不自动，这里不做事。期望按规则手写"""
    A, B, C = "0100000000010000", "0100000000020000", "0100000000030000"

    def test_does_nothing_since_sgrade_is_manual(self):
        # 11-17 维护者：S 级优化不再自动运行。打开启动器起的 installUpdatePrepare 原先逐款 refresh_ios（做的全是 S 级补丁与 _bf），现在一款都不碰、也不找手机
        import rewrite_refresh
        with tempfile.TemporaryDirectory() as t:
            lib = Path(t); (lib / "devices").mkdir()
            (lib / "devices/UDID1.json").write_text(json.dumps({"games": {self.A: {}, self.B: {}}}))
            c = jobcase.fake_context(lib.parent, library=lib, catalog=lambda: {"games": [{"id": self.A}, {"id": self.B}]}, begin=lambda *a: None)
            with um.patch.object(rewrite_refresh, "refresh_ios", side_effect=AssertionError("不该自动补")), \
                    um.patch.object(install, "phone_for", side_effect=AssertionError("准备段不该找手机")):
                self.assertEqual(install.update_prepare(c), {"prepared": [], "updated": [], "manual": True})

    def test_operation_registered(self):
        import core
        src = (Path(__file__).resolve().parents[1] / "main.py").read_text(encoding="utf-8")
        self.assertIn('"installUpdatePrepare": install.update_prepare', src)
        self.assertEqual(core.OPERATION_NAMES["installUpdatePrepare"], "准备更新 iPhone 上的 S2Arm")
