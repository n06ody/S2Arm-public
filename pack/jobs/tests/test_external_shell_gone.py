# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""换了装 App 的工具以后，电脑别再按「这台电脑装的旧壳」提示更新、去签（D-KBN6，pack/jobs/install.py）。期望按场景手写：
  ① forget_gone_shell：设备记录里这台电脑装的那份（shellId + bundleId）已不在手机上 → 去掉 shellId 写回；手机上只剩一个 S2Arm 时记下它的
     externalBundleId；那份还在 / 列不出 / 记录本来没有壳号 → 原样、不写
  ② owner_for_push：推送前现查到这台电脑装的那份没了 → 记录里的壳号去掉（同 ①）
  ③ shell_outdated：玩家在总控台选用的是别的工具装的那份（chosenBundleId ≠ 这台电脑装的包名）→ 不算「壳旧了」；选的就是这台电脑装的（大小写不同）照算
  ④ phone_updates（游戏库「更新 iPhone 上的 S2Arm」）：壳号去掉之后、或选了别的那份时，这台不再列出；记录里壳旧、没选别的照列
  ⑤ _kept_pull_on（两分钟一轮的收回）：那份不在了 → shellUpdate 为假、不自动去签；还在 → 照旧
反证（手跑核过）：去掉 shell_outdated 里 chosenBundleId 那一判 → ③④ 红；owner_for_push 不调 forget_gone_shell → ② 红；_kept_pull_on 不核 → ⑤ 红"""
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import install  # noqa: E402

OWN = "io.s2arm.games.team1"
ALT = "io.s2arm.games.example01.ALTTEAM1"
GID = "0100000000010000"
LAUNCHER = dict(shellId="S_OLD", bundleId=OWN, games={GID: dict(program={})})


def app(bid):
    return dict(bundleId=bid, team=None, version=None)


class Store:
    def __init__(self, rec):
        self.recs = {"UDID1": dict(rec)}
        self.saves = 0

    def save(self, c, k, r):
        self.saves += 1
        self.recs[k] = r


def forget(rec, apps):
    st = Store(rec)
    c = types.SimpleNamespace()
    with patch.object(install.layout, "save_device", st.save), patch.object(install, "trace", lambda *a, **k: None):
        out = install.forget_gone_shell(c, "UDID1", st.recs["UDID1"], apps)
    return out, st


class ForgetGoneShell(unittest.TestCase):
    def test_gone_one_other(self):
        out, st = forget(LAUNCHER, [app(ALT)])
        self.assertNotIn("shellId", out, "这台电脑装的那份不在了：壳号去掉")
        self.assertEqual(out["bundleId"], OWN, "包名留着")
        self.assertEqual(out["externalBundleId"], ALT, "只剩 AltStore 那份：记成别的工具装的")
        self.assertEqual(st.saves, 1)
        self.assertEqual(st.recs["UDID1"], out, "写回设备记录")

    def test_gone_none_left(self):
        out, st = forget(LAUNCHER, [])
        self.assertNotIn("shellId", out)
        self.assertNotIn("externalBundleId", out, "一个都没装：不编别的工具的包名")
        self.assertEqual(st.saves, 1)

    def test_gone_two_left(self):
        out, st = forget(LAUNCHER, [app(ALT), app("io.s2arm.games.other.T2")])
        self.assertNotIn("shellId", out)
        self.assertNotIn("externalBundleId", out, "剩两个：不替玩家挑（总控台让玩家选）")

    def test_still_there(self):
        rec = dict(LAUNCHER)
        out, st = forget(rec, [app(OWN.upper()), app(ALT)])
        self.assertIs(out, st.recs["UDID1"], "那份还在（包名大小写不同也算）：原样")
        self.assertEqual(out["shellId"], "S_OLD")
        self.assertEqual(st.saves, 0)

    def test_unlisted_or_no_shell(self):
        out, st = forget(LAUNCHER, None)
        self.assertEqual((out["shellId"], st.saves), ("S_OLD", 0), "列不出：不动")
        out, st = forget(dict(bundleId=OWN), [app(ALT)])
        self.assertEqual((out, st.saves), (dict(bundleId=OWN), 0), "记录本来没有壳号：不动")


class OwnerForPush(unittest.TestCase):
    def test_push_forgets_gone_shell(self):
        st = Store(LAUNCHER)
        with tempfile.TemporaryDirectory() as d:
            c = types.SimpleNamespace(private=d)
            cl = types.SimpleNamespace(apps=lambda: [(ALT, "S2Arm", "ALTTEAM1")], get=lambda rel, dst: False)
            with patch.object(install, "devpush_client", lambda c, udid, bid, w: cl), patch.object(install, "main_executable", lambda c: "S2Arm"), \
                    patch.object(install.layout, "load_device", lambda c, k: dict(st.recs.get(k, {}))), \
                    patch.object(install.layout, "save_device", st.save), patch.object(install, "trace", lambda *a, **k: None):
                who = install.owner_for_push(c, "UDID1", "UDID1")
        self.assertEqual(who, ("external", ALT))
        self.assertNotIn("shellId", st.recs["UDID1"], "推送前现查到这台电脑装的那份没了：壳号去掉")
        self.assertEqual(st.recs["UDID1"]["externalBundleId"], ALT)


def catalog_c():
    return types.SimpleNamespace(catalog=lambda: dict(games=[dict(id=GID, name="Game")]))


class ShellOutdated(unittest.TestCase):
    def outdated(self, rec):
        with patch("ipa.usable_kit", lambda *a, **k: object()):
            return install.shell_outdated(catalog_c(), rec, "S_NEW")

    def test_chosen_other(self):
        self.assertIsNone(self.outdated(dict(LAUNCHER, chosenBundleId=ALT)), "选用的是 AltStore 那份：这台电脑的旧壳与玩家无关")

    def test_chosen_own_or_none(self):
        got = self.outdated(dict(LAUNCHER, chosenBundleId=OWN.upper()))
        self.assertEqual((got or {}).get("fromShell"), "S_OLD", "选的就是这台电脑装的：照算壳旧")
        self.assertEqual((self.outdated(LAUNCHER) or {}).get("toShell"), "S_NEW", "没选过：照算")


class PhoneUpdates(unittest.TestCase):
    def listed(self, rec):
        c = catalog_c()
        with patch.object(install.layout, "devices", lambda c: [("UDID1", rec)]), patch("ipa.current_shell_id", lambda c: "S_NEW"), \
                patch("ipa.usable_kit", lambda *a, **k: object()), patch("rewrite_refresh.kept_todo", lambda *a, **k: False), patch.object(install.layout, "ios_kit", lambda c, g: None), \
                patch.object(install, "online_devices", lambda c: set()):
            return [u["device"] for u in install.phone_updates(c)]

    def test_listing(self):
        self.assertEqual(self.listed(LAUNCHER), ["UDID1"], "记录里壳旧、没选别的：照列")
        gone, _ = forget(LAUNCHER, [app(ALT)])
        self.assertEqual(self.listed(gone), [], "这台电脑装的那份不在了：不再提示更新")
        self.assertEqual(self.listed(dict(LAUNCHER, chosenBundleId=ALT)), [], "选用 AltStore 那份：不再提示更新")


class KeptPull(unittest.TestCase):
    """两分钟一轮的收回（phoneKeptPull）：记录说壳旧了时先核那份还在不在；不在 → shellUpdate 为假（不自动去签），记录壳号去掉"""
    def pull(self, rows):
        st = Store(LAUNCHER)
        import contextlib
        c = types.SimpleNamespace(idle=lambda what: contextlib.nullcontext(True), catalog=catalog_c().catalog)
        ph = types.SimpleNamespace(key="UDID1", udid="UDID1", pull_kept=lambda games: 0, app_running=lambda: False)
        with patch.object(install.layout, "load_device", lambda c, k: dict(st.recs.get(k, {}))), patch.object(install.layout, "save_device", st.save), \
                patch.object(install, "pull_shader_counts", lambda *a, **k: {}), patch.object(install.layout, "ios_kit", lambda c, g: None), \
                patch("rewrite_refresh.kept_todo", lambda *a, **k: False), patch("ipa.current_shell_id", lambda c: "S_NEW"), \
                patch("ipa.usable_kit", lambda *a, **k: object()), patch.object(install, "installed_s2arm_on", lambda c, udid, rec=None: rows), \
                patch.object(install, "trace", lambda *a, **k: None):
            return install._kept_pull_on(c, ph), st

    def test_gone_no_auto_update(self):
        out, st = self.pull([app(ALT)])
        self.assertFalse(out["shellUpdate"], "这台电脑装的那份不在了：不自动去签")
        self.assertNotIn("shellId", st.recs["UDID1"])

    def test_still_there_updates(self):
        out, st = self.pull([app(OWN)])
        self.assertTrue(out["shellUpdate"], "那份还在、壳旧了：照旧自动更新")
        self.assertEqual(st.recs["UDID1"]["shellId"], "S_OLD")


if __name__ == "__main__":
    unittest.main()
