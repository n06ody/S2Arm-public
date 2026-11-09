#!/usr/bin/env python3
"""android/tools/run_game.py 宿主单测（adb 替身，不碰真机；模拟真实平台行为）：
  ① 空机起局：记录原状（未安装）→ 安装 → 冷启动 → READY → 按时长采集 → 收局 → 卸载还原；
    局目录有 tools/quality_check.py 安卓分支要的 android.json / shots_ap / frames.tsv / power.tsv / logcat.txt，
    且 quality_check 能直接判出 帧率 / 满屏 / 能耗
  ② 原机已装不同构建：先 adb pull 备份原包 → 起局 → adb install -r <备份> 还原
  ③ 还原失败：uninstall 失败 → RESULT=red、设备被留置（device_lock.acquire 拒取）
  ④ --mode direct：launch 行带 --es s2arm.graphics_mode direct（quality_check 不认它是「默认启动」）
  ⑤ 帧日志 → frames.tsv：行数 = recorded-1、disp_us 是相邻 present 间隔、frame_log.txt 有 p50/p95/p99
"""
import base64, hashlib, importlib.util, json, os, struct, sys, tempfile, unittest
from pathlib import Path

ANDROID = Path(__file__).resolve().parents[1]
REPO = ANDROID.parent
spec = importlib.util.spec_from_file_location("run_game", ANDROID / "tools" / "run_game.py")
run_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_game)

PKG = "org.s2arm.game.t0123456789abcdef"
SERIAL = "TESTSERIAL01"

FAKE_ADB = '''#!/usr/bin/env python3
import hashlib, json, shlex, sys, time
state_path, argv = sys.argv[1], sys.argv[2:]
if argv and argv[0] == "-s":
    argv = argv[2:]
st = json.load(open(state_path))
def save():
    json.dump(st, open(state_path, "w"))
def out(s):
    sys.stdout.write(s)
def fail(msg):
    sys.stderr.write(msg + "\\n"); sys.exit(1)
cmd = argv[0]
if cmd == "get-state":
    out("device"); sys.exit(0)
if cmd == "install":
    apk = argv[-1]
    if st.get("fail_install"): fail("Failure [INSTALL_FAILED]")
    st["installed"] = True
    st["installed_sha"] = hashlib.sha256(open(apk, "rb").read()).hexdigest()
    st.setdefault("installs", []).append(apk)
    save(); out("Success"); sys.exit(0)
if cmd == "uninstall":
    if st.get("fail_uninstall"): fail("Failure [DELETE_FAILED_INTERNAL]")
    st["installed"] = False; st["installed_sha"] = ""; save(); out("Success"); sys.exit(0)
if cmd == "pull":
    open(argv[-1], "wb").write(bytes.fromhex(st["orig_apk_hex"]))
    out(f"{argv[1]}: 1 file pulled, 0 skipped\\n"); sys.exit(0)
if cmd == "exec-out":
    sys.stdout.buffer.write(open(st["png"], "rb").read()); sys.exit(0)
if cmd == "logcat":
    if "-d" in argv:
        out(open(st["logcat_dump"]).read()); sys.exit(0)
    out(open(st["logcat_stream"]).read()); sys.stdout.flush()
    time.sleep(60); sys.exit(0)
if cmd == "shell":
    text = argv[1]
    sh = shlex.split(text)
    c = sh[0] if sh else ""
    if "power_supply" in text:
        out(st["power_sample"]); sys.exit(0)
    if c == "getprop":
        out(st["props"].get(" ".join(sh[1:]), "") + "\\n"); sys.exit(0)
    if c == "setprop":
        st["props"][sh[1]] = sh[2] if len(sh) > 2 else ""; save(); sys.exit(0)
    if c == "pm" and len(sh) > 1:
        if sh[1] == "path":
            out(("package:" + st["base_path"] + "\\n") if st["installed"] else ""); sys.exit(0)
        if sh[1] == "uninstall":
            if st.get("fail_uninstall"): fail("Failure [DELETE_FAILED_INTERNAL]")
            st["installed"] = False; save(); out("Success"); sys.exit(0)
        if sh[1] == "list":
            out("package:" + st["package"] + "\\n" if st["installed"] else ""); sys.exit(0)
    if c == "sha256sum":
        out((st["installed_sha"] or "0" * 64) + "  " + sh[-1] + "\\n"); sys.exit(0)
    if c == "pidof":
        out((st.get("pid", "4242") + "\\n") if st.get("running") else ""); sys.exit(0)
    if c == "am":
        if "start" in sh:
            if "s2arm.finish" in text:
                st["running"] = False
            else:
                st["running"] = True
                st["launch_cmd"] = text
            save(); out("Status: ok\\nLaunchState: COLD\\nTotalTime: 200\\nWaitTime: 210\\n"); sys.exit(0)
        if "force-stop" in sh:
            st["running"] = False; save(); sys.exit(0)
        sys.exit(0)
    if c == "date":
        out("11-09 12:00:00.000\\n"); sys.exit(0)
    if c == "dumpsys":
        if len(sh) > 1 and sh[1] == "battery": out(st["dumpsys_battery"]); sys.exit(0)
        if len(sh) > 1 and sh[1] == "thermalservice": out(st["dumpsys_thermal"]); sys.exit(0)
        out(""); sys.exit(0)
    if c == "logcat":
        if "-c" in sh: sys.exit(0)
        if "-d" in sh: out(open(st["logcat_dump"]).read()); sys.exit(0)
        sys.exit(0)
    if c in ("wm", "input", "id", "ls", "cat", "mkdir", "rm"):
        out(""); sys.exit(0)
    fail("fake adb: unhandled shell " + text)
fail("fake adb: unhandled " + " ".join(argv))
'''


def make_state(root, **kw):
    state = {
        "package": PKG, "installed": False, "installed_sha": "", "running": False,
        "pid": "4242", "base_path": f"/data/app/{PKG}-Ab12/base.apk",
        "orig_apk_hex": "aabbccdd",
        "props": {
            "ro.product.model": "PixelTest", "ro.soc.model": "TEST_SOC",
            "ro.build.version.release": "15",
            "debug.s2a.debug_env": "", "debug.s2a.frame_log": "",
        },
        "dumpsys_battery": ("Current Battery Service state:\n  AC powered: false\n  USB powered: false\n"
                            "  status: 3\n  health: 2\n  level: 80\n  voltage: 4000\n  temperature: 350\n"),
        "dumpsys_thermal": "Current Status:0\nType{Name,Forecast}:CPU{CPU,0.5}\n",
        "power_sample": ("status=Discharging\ncurrent_now=-800000\nvoltage_now=4000000\n"
                         "energy_now=\ncharge_counter=\ntemp=350\nboot_us=123456\nac_online=0\n"
                         "usb_online=0\nwireless_online=0\ndock_online=0\ncapacity=80\n"),
        "png": str(root / "shot.png"),
        "logcat_dump": str(root / "logcat_dump.txt"),
        "logcat_stream": str(root / "logcat_stream.txt"),
    }
    state.update(kw)
    path = root / "state.json"
    path.write_text(json.dumps(state))
    return path


def frame_log_text(recorded=4000, step_us=16667, first=0):
    """v3 帧日志块（镜像 nvn_vk_frame_log.c：24B 记录 = ts_us + fence_wait_us + fence_n + acquire_us + present_us）。"""
    recs = []
    t = 1_000_000
    for i in range(recorded):
        recs.append(struct.pack("<QIIII", t + i * step_us, 0, 0, 900, 1200))
    blob = b"".join(recs)
    b64 = base64.b64encode(blob).decode()
    parts = [b64[i:i + 700] for i in range(0, len(b64), 700)]
    pre = "11-09 12:00:01.000  4242  4242 I S2Arm: "
    lines = [pre + f"direct frame-log meta: v=3 clock=boottime unit=us endian=le rec=24 "
                   f"cap=32768 recorded={recorded} presents={first + recorded} dropped=0 "
                   f"first={first} bytes={len(blob)} chunks={len(parts)}"]
    lines += [pre + f"direct frame-log[{i + 1}/{len(parts)}]:{p}" for i, p in enumerate(parts)]
    lines.append(pre + f"direct frame-log end: frames={recorded} dropped=0 "
                       f"sha256={hashlib.sha256(blob).hexdigest()}")
    return "\n".join(lines) + "\n"


LOG_HEAD = """11-09 12:00:00.100  4242  4242 I S2Arm: direct provider prepared: code=2334 gpu=MaliTest sym=251 apis=0x3
11-09 12:00:00.500  4242  4242 I S2Arm: Game startup accepted: 0123456789ABCDEF session t1
"""


class RunGameTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="s2a-rungame-"))
        (self.tmp / "shot.png").write_bytes(self._png())
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + frame_log_text())
        (self.tmp / "logcat_stream.txt").write_text(LOG_HEAD + frame_log_text())
        self.apk = self.tmp / "game.apk"
        self.apk.write_bytes(b"PKAPKTEST" * 100)
        self.lock_root = self.tmp / "locks"

    def _png(self):
        try:
            from PIL import Image
            buf = self.tmp / "_full.png"
            Image.new("RGB", (1000, 460), (90, 120, 60)).save(buf)
            return buf.read_bytes()
        except ImportError:
            # 最小的非纯黑 PNG：1x1 也行，但满屏判定需要 PIL 读图；没 PIL 时相关断言自己跳过
            return base64.b64decode(
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==")

    def run_tool(self, state=None, argv=None, seconds=3):
        state = make_state(self.tmp, **(state or {}))
        out = self.tmp / "out_1"
        fake = self.tmp / "fake_adb.py"
        fake.write_text(FAKE_ADB)
        args = ["--apk", str(self.apk), "--package", PKG, "--serial", SERIAL,
                "--out", str(out), "--seconds", str(seconds),
                "--adb", f"{sys.executable} {fake} {state}",
                "--lock-root", str(self.lock_root), "--ready-timeout", "5"]
        args += argv or []
        rc = run_game.main(args)
        return rc, out, state

    def test_clean_install_run_and_qc_accepts_dir(self):
        rc, out, _ = self.run_tool()
        self.assertEqual(0, rc)
        for name in ("android.json", "logcat.txt", "frames.tsv", "power.tsv",
                     "battery.txt", "thermal.txt", "gfx_mode.txt", "frame_log.txt"):
            self.assertTrue((out / name).is_file(), name)
        shots = sorted((out / "shots_ap").glob("*.png"))
        self.assertGreaterEqual(len(shots), 2)
        meta = json.loads((out / "android.json").read_text())
        self.assertEqual(PKG, meta["package"])
        self.assertEqual("PixelTest", meta["serial_model"])
        # ⑤ 帧日志 → frames.tsv
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual("frame\tpresent_ns\tdisp_us\twall_us", rows[0])
        self.assertEqual(3999, len(rows) - 1)
        disp = [int(r.split("\t")[2]) for r in rows[1:]]
        self.assertTrue(all(d == 16667 for d in disp))
        self.assertIn("p95", (out / "frame_log.txt").read_text())
        self.assertIn("p99", (out / "frame_log.txt").read_text())
        # ① quality_check 直接判这个目录
        try:
            from PIL import Image  # noqa
            sys.path.insert(0, str(REPO / "tools"))
            import quality_check
            verdicts = quality_check.check(str(out), "direct", ai=False)
            self.assertEqual("过", verdicts["帧率"])
            self.assertEqual("过", verdicts["满屏"])
            self.assertEqual("过", verdicts["能耗"])
        except ImportError:
            self.skipTest("Pillow 不在，跑不了 quality_check 判读")
        # 跑完锁已放、设备无留置
        sys.path.insert(0, str(ANDROID / "tools"))
        import device_lock
        st = device_lock.status(str(self.lock_root), SERIAL)
        self.assertFalse(st["held"], st)
        self.assertFalse((self.lock_root / SERIAL / "detained.json").exists())

    def test_restores_original_apk_when_installed(self):
        orig_sha = hashlib.sha256(bytes.fromhex("aabbccdd")).hexdigest()
        rc, out, state = self.run_tool(state={
            "installed": True, "installed_sha": orig_sha,
        })
        self.assertEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue(st["installed"])
        self.assertEqual(orig_sha, st["installed_sha"], "还原后设备上应是原包")
        self.assertGreaterEqual(len(st["installs"]), 2, "应装过测试包再装回原包")

    def test_restore_failure_detains_device(self):
        rc, out, _ = self.run_tool(state={"fail_uninstall": True})
        self.assertNotEqual(0, rc)
        sys.path.insert(0, str(ANDROID / "tools"))
        import device_lock
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "还原失败应留置设备")
        rrc, msg = device_lock.acquire(str(self.lock_root), SERIAL, "next", os.getpid(), wait=0)
        self.assertEqual(3, rrc, msg)
        device_lock.clear(str(self.lock_root), SERIAL)

    def test_mode_direct_marks_launch_line(self):
        self.run_tool(argv=["--mode", "direct"], state={})
        meta = json.loads((self.tmp / "out_1" / "android.json").read_text())
        self.assertIn("--es s2arm.graphics_mode direct", meta["launch"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
