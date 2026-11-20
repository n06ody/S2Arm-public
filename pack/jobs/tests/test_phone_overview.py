# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""0.6.0 通用壳「iPhone 总控台」的只读总览（pack/jobs/host_update.py phone_overview）。游戏库与设备记录是真的（library.setup + layout），
本版启动器的值用替身给定（改写器指纹 RIDNOW、约定号 2、着色器链 chainNOW、壳号 SNOW），期望按记录手写。"""
import builtins
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import host_update  # noqa: E402
import ipa as packer  # noqa: E402
import layout  # noqa: E402
import program_pack  # noqa: E402
import rewrite_refresh  # noqa: E402
import tests.test_jobs as tj  # noqa: E402
from tests.jobcase import ios_fixture  # noqa: E402

NOW = dict(hostAbi=2, dmshChain="chainNOW")
T = 1_800_000_000.0
OK_PROG = dict(sha="s", team="TEAMX", host_abi=2, rewriter_id="RIDNOW", dir="v-s-pc")


class PhoneOverview(unittest.TestCase):
    setUp = tj.JobsTests.setUp; tearDown = tj.JobsTests.tearDown
    A, B, C, D, E = "0100000000010000", "0100000000020000", "0100000000030000", "0100000000040000", "0100000000050000"

    def fixture(self):
        ok = dict(rewriterId="RIDNOW", hostAbi=2, dmshChain="chainNOW", libGen=host_update.LIB_GEN)   # 出好的款带当前直连库内容代数（不带 = 旧库，进「建议更新」原因 libGen）
        for g in (self.A, self.C, self.D):
            ios_fixture(self.c, g, compat=ok)
        ios_fixture(self.c, self.B, compat=dict(ok, rewriterId="RIDOLD"))   # 游戏库这款的程序要按新改写器更新
        (layout.image_ios(self.c, self.A) / ".pack/game.img").write_bytes(b"\0" * 4096)
        (layout.image_ios(self.c, self.C) / ".pack/game.img").write_bytes(b"\0" * 4096)
        ids = [self.A, self.B, self.C, self.D, self.E]
        self.c.catalog = lambda: {"games": [{"id": g, "name": g} for g in ids]}
        # 手机 1：本版壳、付费账号；A 最新、B 建议更新（游戏库记录旧）、C 只推了程序（要游戏数据）、D 推过但手机上的程序约定号旧、E 没推
        layout.save_device(self.c, "PHONE-1", dict(shellId="SNOW", bundleId="io.example.one", at=T + 60, signed=dict(at=T, free=False, device="手机甲"), games={
            self.A: dict(program=OK_PROG, pack="P"), self.B: dict(program=OK_PROG, pack="P"), self.C: dict(program=OK_PROG), self.E: dict(program=OK_PROG),
            self.D: dict(program=dict(OK_PROG, host_abi=1), pack="P")}))
        # 手机 2：旧壳、免费账号、一款没推
        layout.save_device(self.c, "PHONE-2", dict(shellId="SOLD", bundleId="io.example.two", signed=dict(at=T, free=True, device="手机乙"), games={}))

    def overview(self, on=True, probe=False, online=None, owners=None, names=None, cands=None):
        """owners：{UDID: (谁装的, 包名)}，连着的手机现查的结果（install.s2arm_owner 的替身；没给的按列不出 (None, None)）；names：现查到的设备名"""
        if probe:
            self.c.r["probe"] = True
        def no_probe(c, names=None):
            raise AssertionError("没带 probe 不查连接")
        def fake_online(c, out=None):
            if out is not None:
                out.update(names or {})
            return online or {}
        import install
        with patch.object(rewrite_refresh, "rewriter_id", lambda c: "RIDNOW"), \
                patch.object(host_update, "engine_compat", lambda c: NOW), patch.object(packer, "current_shell_id", lambda c: "SNOW"), \
                patch.object(install, "s2arm_owner", lambda c, udid, rec: (owners or {}).get(udid, (None, None))), \
                patch.object(install, "s2arm_candidates", lambda c, udid, rec: (cands or {}).get(udid, [])), \
                patch.object(host_update, "_online", fake_online if probe else no_probe):
            return host_update.phone_overview(self.c)

    def test_states_per_phone_and_game(self):
        self.fixture()
        r = self.overview()
        self.assertNotIn("enabled", r, "0.6.0 起没有开关，结果不带 enabled")
        p1, p2 = sorted(r["phones"], key=lambda p: p["device"])
        self.assertEqual({k: p1[k] for k in ("device", "name", "installed", "bundleId", "shellCurrent", "signedAt", "free", "expiresAtEstimate")},
                         dict(device="PHONE-1", name="手机甲", installed=True, bundleId="io.example.one", shellCurrent=True, signedAt=T, free=False,
                              expiresAtEstimate=T + 365 * 86400))
        # C：电脑上 game.img 在（放一个）→ 推一下就行；E：程序推过、游戏库里没有这款的 game.img → 要原件
        self.assertEqual(p1["games"], [dict(gameId=self.A, state="latest"), dict(gameId=self.B, state="suggested", reasons=["rewriter"]),
                                       dict(gameId=self.C, state="needsData", dataOnComputer=True), dict(gameId=self.D, state="suggested", reasons=["hostAbi"]),
                                       dict(gameId=self.E, state="needsData", dataOnComputer=False)])
        self.assertEqual((p1["lastSeen"], p2["lastSeen"]), (T + 60, None))
        self.assertEqual((p2["shellCurrent"], p2["free"], p2["expiresAtEstimate"]), (False, True, T + 7 * 86400))
        # 剩余天数按估的到期时刻往上取整（还剩 6 天 1 秒 → 7）；过期了给 0
        import time as _t
        for now, want in ((T + 86400 - 1, 7), (T + 7 * 86400 + 5, 0)):
            with patch.object(_t, "time", lambda n=now: n):
                p = next(x for x in self.overview()["phones"] if x["device"] == "PHONE-2")
            self.assertEqual(p["remainingDays"], want)
        self.assertEqual({g["state"] for g in p2["games"]}, {"notPushed"})   # 手机 2 一款都没推
        # 没推过的也带电脑上资源在不在（A、C 放了 game.img）：界面提前说「要原件」
        self.assertEqual({g["gameId"]: g["dataOnComputer"] for g in p2["games"]}, {self.A: True, self.B: False, self.C: True, self.D: False, self.E: False})
        self.assertNotIn("online", p1, "没带 probe：不查连接、不出 online")

    def test_real_expiry_beats_estimate(self):
        # #1772：装机时从签好的包里读到的真实到期（signed.expires）优先；没有才按 7 天 / 365 天估。期望手写
        layout.save_device(self.c, "P-REAL", dict(shellId="S", signed=dict(at=T, free=False, expires=T + 360 * 86400), games={}))
        layout.save_device(self.c, "P-EST", dict(shellId="S", signed=dict(at=T, free=True), games={}))
        import time as _t
        with patch.object(_t, "time", lambda: T + 86400):
            got = {p["device"]: (p["expiresAtEstimate"], p["remainingDays"], p["expiresKnown"]) for p in self.overview()["phones"]}
        self.assertEqual(got, {"P-REAL": (T + 360 * 86400, 359, True), "P-EST": (T + 7 * 86400, 6, False)})

    def test_probe_reports_connection(self):
        self.fixture()
        r = self.overview(probe=True, online={"PHONE-1": "usb"})
        on = {p["device"]: p["online"] for p in r["phones"]}
        self.assertEqual(on, {"PHONE-1": "usb", "PHONE-2": None})

    def test_reads_records_only(self):
        # 只读记录：不打开游戏大文件（game.img、资源包里的文件）
        self.fixture()
        opened, real_open, real_io = [], builtins.open, io.open
        def spy(f, *a, **k):
            opened.append(str(f)); return real_open(f, *a, **k)
        def spy_io(f, *a, **k):
            opened.append(str(f)); return real_io(f, *a, **k)
        with patch.object(builtins, "open", spy), patch.object(io, "open", spy_io):
            self.overview()
        self.assertEqual([p for p in opened if "game.img" in p or "/.pack/" in p], [])
        self.assertTrue(any(p.endswith(".json") for p in opened))

    def test_sign_mode(self):
        # 我们签的记 signed.mode；旧记录（只有 free）按现在的签名方式；推过游戏却没有签名记录 = 别的工具装的；什么都没有 = 判不出
        import devcert
        layout.save_device(self.c, "P-NEW", dict(shellId="S", signed=dict(at=T, free=False, mode="devCert"), games={}))
        layout.save_device(self.c, "P-OLD", dict(shellId="S", signed=dict(at=T, free=True), games={}))
        layout.save_device(self.c, "P-EXT", dict(games={self.A: dict(pack="P")}))
        layout.save_device(self.c, "P-NONE", dict(at=T))
        self.c.catalog = lambda: {"games": []}
        for active, old in ((False, "appleId"), (True, "devCert")):
            with patch.object(devcert, "active", lambda c, a=active: a):
                got = {p["device"]: p["signMode"] for p in self.overview()["phones"]}
            self.assertEqual(got, {"P-NEW": "devCert", "P-OLD": old, "P-EXT": "external", "P-NONE": None})

    def test_old_container_games_need_program(self):
        # 0.5.x 整包 App 装的款：设备记录只有 pack、没有 program → 建议更新（只补程序），不是「最新」
        self.fixture()
        layout.save_device(self.c, "PHONE-3", dict(shellId="SNOW", signed=dict(at=T, free=True), games={self.A: dict(pack="P")}))
        p3 = next(p for p in self.overview()["phones"] if p["device"] == "PHONE-3")
        self.assertEqual(p3["games"][0], dict(gameId=self.A, state="suggested", reasons=["missing"]))

    def test_can_sign(self):
        # 交过身份 → True；我们装过 App 却没交过 → False；只有推过游戏、没有壳号（判不出 App 是谁装的）→ None
        self.c.catalog = lambda: {"games": []}
        layout.save_device(self.c, "P-ID", dict(shellId="S", identity=dict(team="T", key_fp="k", cert_sha1="c")))
        layout.save_device(self.c, "P-NO", dict(shellId="S"))
        layout.save_device(self.c, "P-UNK", dict(games={self.A: dict(pack="P")}))
        got = {p["device"]: p["canSign"] for p in self.overview()["phones"]}
        self.assertEqual(got, {"P-ID": True, "P-NO": False, "P-UNK": None})

    def test_external_phone_from_live_probe(self):
        """结构 5（11-19 走查）：连着、没有设备记录、装的是 AltStore 装的 S2Arm 的手机也列出来，给它自己的四个按钮、不出「安装 S2Arm」（期望手写）"""
        self.fixture()
        alt = "io.s2arm.games.example01"
        r = self.overview(probe=True, online={"PHONE-1": "usb", "ALT-1": "usb"},
                          owners={"PHONE-1": ("launcher", "io.example.one"), "ALT-1": ("external", alt)}, names={"ALT-1": "备用手机"})
        got = {p["device"]: p for p in r["phones"]}
        a = got["ALT-1"]
        self.assertEqual((a["installed"], a["installedBy"], a["bundleId"], a["signMode"], a["shellCurrent"], a["name"], a["online"]),
                         (True, "external", alt, "external", None, "备用手机", "usb"))
        self.assertEqual(a["actions"], ["exportShellIpa", "pushS2arm", "exportS2arm", "handOverIdentity"])
        self.assertEqual({g["state"] for g in a["games"]}, {"notPushed"}, "没推过：每款都是没推")
        p1 = got["PHONE-1"]
        self.assertEqual((p1["installedBy"], p1["bundleId"], p1["shellCurrent"]), ("launcher", "io.example.one", True))
        self.assertNotIn("actions", p1, "启动器装的手机照旧由前端按原来的按钮出")
        self.assertEqual(got["PHONE-2"]["installedBy"], "launcher", "没连着：按设备记录（有壳号 = 启动器装的）")

    def test_external_phone_from_record_without_probe(self):
        """推过一次（push_game 记下 externalBundleId、没有壳号）后，不查手机也认得出是别的工具装的"""
        layout.save_device(self.c, "ALT-2", dict(externalBundleId="io.s2arm.games.abc", games={}))
        layout.save_device(self.c, "NEW-1", dict(games={}))
        got = {p["device"]: p for p in self.overview()["phones"]}
        self.assertEqual((got["ALT-2"]["installedBy"], got["ALT-2"]["bundleId"], got["ALT-2"]["actions"][0]), ("external", "io.s2arm.games.abc", "exportShellIpa"))
        self.assertEqual((got["NEW-1"]["installedBy"], got["NEW-1"]["installed"]), (None, False), "两样都没有：判不出谁装的")
        self.assertNotIn("actions", got["NEW-1"])

    def test_launcher_app_removed_now_external(self):
        """启动器装过（有壳号），现查发现手机上只剩 AltStore 装的那份：按现查算别的工具装的"""
        self.fixture()
        r = self.overview(probe=True, online={"PHONE-2": "usb"}, owners={"PHONE-2": ("external", "io.s2arm.games.zz")})
        p2 = next(p for p in r["phones"] if p["device"] == "PHONE-2")
        self.assertEqual((p2["installedBy"], p2["bundleId"], p2["shellCurrent"]), ("external", "io.s2arm.games.zz", None))



    def test_choose_lists_candidates(self):
        """装着不止一个、没选过：needsChoice、候选、只给「选用哪个」，不出安装也不出四个按钮"""
        cand = [dict(bundleId="io.s2arm.games.a", team="TA", byThisLauncher=False, identityTeam="TA"),
                dict(bundleId="io.s2arm.games.b", team="TB", byThisLauncher=False, identityTeam=None)]
        r = self.overview(probe=True, online={"TWO-1": "usb"}, owners={"TWO-1": ("choose", None)}, cands={"TWO-1": cand})
        p = next(x for x in r["phones"] if x["device"] == "TWO-1")
        self.assertEqual((p["installed"], p["installedBy"], p["needsChoice"], p["bundleId"], p["actions"], p["candidates"]),
                         (True, None, True, "", ["chooseApp"], cand))

    def test_chosen_still_lists_candidates(self):
        """选过了：照常给别的工具装的那四个按钮，候选照样带上（可以换），actions 里不加 chooseApp"""
        layout.save_device(self.c, "TWO-2", dict(chosenBundleId="io.s2arm.games.a", games={}))
        cand = [dict(bundleId="io.s2arm.games.a", team="TA", byThisLauncher=False, identityTeam="TA"),
                dict(bundleId="io.s2arm.games.b", team="TB", byThisLauncher=False, identityTeam=None)]
        r = self.overview(probe=True, online={"TWO-2": "usb"}, owners={"TWO-2": ("external", "io.s2arm.games.a")}, cands={"TWO-2": cand})
        p = next(x for x in r["phones"] if x["device"] == "TWO-2")
        self.assertEqual((p["installedBy"], p["bundleId"], p["actions"], p["candidates"], p.get("needsChoice")),
                         ("external", "io.s2arm.games.a", ["exportShellIpa", "pushS2arm", "exportS2arm", "handOverIdentity"], cand, None))


if __name__ == "__main__":
    unittest.main()
