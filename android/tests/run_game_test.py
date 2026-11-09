#!/usr/bin/env python3
"""android/tools/run_game.py 宿主单测（adb 替身，不碰真机；模拟真实平台行为）：
  ① 空机起局：记录原状（未安装）→ 安装 → 冷启动 → READY → 按时长采集 → 收局 → 卸载还原；
    局目录有 tools/quality_check.py 安卓分支要的 android.json / shots_ap / frames.tsv / power.tsv / logcat.txt
  ② 帧日志 → frames.tsv：只写采集窗口内的帧（boot_us 同时钟裁窗），块按 PID 绑定当局进程
  ③ 设备已装该包：拒跑（私有数据保证不了还原）；--allow-data-loss 才放行且只还原 APK 字节
  ④ 还原失败：uninstall 失败 → RESULT=red、设备被留置
  ⑤ --mode direct：launch 行带 --es s2arm.graphics_mode direct
  ⑥ SIGTERM：采集中途被 TERM → finally 照常收局还原（卸载），不留置
  ⑦ 调试属性恢复读回不符 → 留置；游戏收局后仍存活 → 留置；卸载后 data/obb 有残留 → 留置
  ⑧ 能耗：放电状态/符号/覆盖按 android_power 口径——合法放电 →「未证」（不套 iPhone 门槛假绿），
    接电 / 未知 / 符号不一致 / 窗口不齐 →「没数据」
"""
import base64, contextlib, hashlib, importlib.util, io, json, os, signal, struct, subprocess, sys, tempfile, time, unittest
from pathlib import Path

ANDROID = Path(__file__).resolve().parents[1]
REPO = ANDROID.parent
spec = importlib.util.spec_from_file_location("run_game", ANDROID / "tools" / "run_game.py")
run_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_game)

PKG = "org.s2arm.game.t0123456789abcdef"
SERIAL = "TESTSERIAL01"
FIELDS = ("boot_us", "status", "current_ua", "voltage_uv", "energy_uwh", "charge_uah",
          "temp_tenths_c", "source", "ac_powered", "usb_powered", "wireless_powered", "dock_powered")

FAKE_ADB = '''#!/usr/bin/env python3
import hashlib, io, json, os, shlex, sys, tarfile, time
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

PUSHED = st.setdefault("pushed", {})                # 设备路径 -> 本机路径（adb push 单文件）
DEV_FS = st.setdefault("dev_fs", {})                # 设备绝对路径 -> 内容 hex（push 目录/tar 解开/mv/rm 都动它）

def file_sha(b):
    return hashlib.sha256(b).hexdigest()

def dev_put_dir(local, dev):
    # adb push <dir> <dev>：本地整树逐文件登记；push_slow_s 让进度轮询能看到中间态
    items = []
    for root_, _, files_ in os.walk(local):
        for fn in files_:
            lp = os.path.join(root_, fn)
            rel = os.path.relpath(lp, local).replace(os.sep, "/")
            items.append((rel, open(lp, "rb").read().hex()))
    delay = float(st.get("push_slow_s") or 0)
    half = len(items) // 2 if delay else len(items)
    for rel, hx in items[:half]:
        DEV_FS[dev.rstrip("/") + "/" + rel] = hx
    save()
    if delay:
        time.sleep(delay)
    for rel, hx in items[half:]:
        DEV_FS[dev.rstrip("/") + "/" + rel] = hx
    save()

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
if cmd == "push":
    if os.path.isdir(argv[1]):
        dev_put_dir(argv[1], argv[2]); sys.exit(0)
    PUSHED[argv[2]] = argv[1]
    if argv[1].endswith(".tar"):
        st["tar_pushed"] = argv[2]
    save(); sys.exit(0)
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
        st["uptime_us"] += 1000000; save()
        out(st["power_sample"] + f"boot_us={st['uptime_us']}\\n" + "capacity=80\\n"); sys.exit(0)
    if "/proc/uptime" in text:
        out(f"{st['uptime_us'] / 1e6:.2f} 20.00\\n"); sys.exit(0)
    if "/stat" in text and "/proc/" in text:
        out("4242 (org.s2arm.game) S " + " ".join(["0"] * 18) + " 55555\\n"); sys.exit(0)
    if c == "getprop":
        key = " ".join(sh[1:])
        out(st["props"].get(key, "") + "\\n"); sys.exit(0)
    if c == "setprop":
        # props_jam：写空值的 setprop 被设备静默拒（有的构建不让清 debug 属性）→ 旧值还在
        if st.get("props_jam") and sh[1].startswith("debug.s2a.") and (len(sh) <= 2 or not sh[2]):
            sys.exit(0)
        st["props"][sh[1]] = sh[2] if len(sh) > 2 else ""; save(); sys.exit(0)
    if c == "pm" and len(sh) > 1:
        if sh[1] == "path":
            # Android 16 真机语义：未装包 rc=1、stdout/stderr 皆空；已装 rc=0 带 package: 行
            st["pm_path_calls"] = st.get("pm_path_calls", 0) + 1; save()
            if st.get("pm_path_fail_at") == st["pm_path_calls"]:
                sys.stderr.write("error: device offline\\n"); sys.exit(st.get("pm_path_fail_rc", 255))
            if st.get("pm_path_empty_at") == st["pm_path_calls"]:
                sys.exit(0)                                        # 异常：rc=0 但无任何输出
            if st.get("pm_path_warn_at") == st["pm_path_calls"]:
                sys.stderr.write("warning: pm flush pending\\n")   # 异常：rc=0 但带 stderr
            if st["installed"]:
                out("package:" + st["base_path"] + "\\n"); sys.exit(0)
            sys.exit(1)
        if sh[1] == "uninstall":
            if st.get("fail_uninstall"): fail("Failure [DELETE_FAILED_INTERNAL]")
            st["installed"] = False; save(); out("Success"); sys.exit(0)
        if sh[1] == "list":
            out("package:" + st["package"] + "\\n" if st["installed"] else ""); sys.exit(0)
    if c == "sha256sum":
        p = sh[-1]
        if p == st["base_path"]:
            out(st["installed_sha"] + "  " + p + "\\n"); sys.exit(0)
        if p in DEV_FS:
            out(file_sha(bytes.fromhex(DEV_FS[p])) + "  " + p + "\\n"); sys.exit(0)
        out("0" * 64 + "  " + p + "\\n"); sys.exit(0)
    if c == "find":
        dir_ = sh[1].rstrip("/")
        if "-exec" in sh:
            if "sha256sum" in sh:
                lines = [file_sha(bytes.fromhex(hx)) + "  " + p
                         for p, hx in sorted(DEV_FS.items()) if p == dir_ or p.startswith(dir_ + "/")]
                out("".join(ln + "\\n" for ln in lines)); sys.exit(0)
            sys.exit(0)
        if "-type" in sh:
            out("".join(p + "\\n" for p in sorted(DEV_FS)
                        if p == dir_ or p.startswith(dir_ + "/"))); sys.exit(0)
        sys.exit(0)
    if c == "du":
        dir_ = sh[-1].rstrip("/")
        total = sum(len(bytes.fromhex(hx)) for p, hx in DEV_FS.items()
                    if p == dir_ or p.startswith(dir_ + "/"))
        out(str((total + 1023) // 1024) + "\\t" + dir_ + "\\n"); sys.exit(0)
    if c == "mv":
        src_, dst_ = sh[1].rstrip("/"), sh[2].rstrip("/")
        for p in list(DEV_FS):
            if p == src_ or p.startswith(src_ + "/"):
                DEV_FS[dst_ + p[len(src_):]] = DEV_FS.pop(p)
        save(); sys.exit(0)
    if c == "chmod":
        sys.exit(0)
    if c == "tar":
        src_ = PUSHED.get(sh[2]); dir_ = sh[4].rstrip("/")
        if not src_: fail("tar 源没 push 过 " + sh[2])
        with tarfile.open(src_) as tf:
            for mem in tf.getmembers():
                if mem.isfile():
                    rel = mem.name[2:] if mem.name.startswith("./") else mem.name
                    blob = tf.extractfile(mem).read()
                    if st.get("corrupt_extract") and len(DEV_FS) == 0:
                        blob = blob + b"x"
                    DEV_FS[dir_ + "/" + rel] = blob.hex()
        st["tar_extracted"] = len(DEV_FS); save(); sys.exit(0)
    if c == "rm":
        for p in [a for a in sh[1:] if not a.startswith("-")]:
            for k in list(DEV_FS):
                if k == p.rstrip("/") or k.startswith(p.rstrip("/") + "/"): del DEV_FS[k]
            PUSHED.pop(p, None)
        save(); sys.exit(0)
    if c == "pidof":
        out((st.get("pid", "4242") + "\\n") if st.get("running") else ""); sys.exit(0)
    if c == "ps":
        if st.get("running"):
            out("    1 init\\n 4242 " + st["package"] + "\\n 4243 " + st["package"] + ":game\\n")
        else:
            out("    1 init\\n")
        sys.exit(0)
    if c == "am":
        if "start" in sh:
            if "s2arm.finish" in text:
                if not st.get("unstoppable"): st["running"] = False
            else:
                st["running"] = True
                st["launch_cmd"] = text
            save(); out("Status: ok\\nLaunchState: COLD\\nTotalTime: 200\\nWaitTime: 210\\n"); sys.exit(0)
        if "force-stop" in sh:
            if not st.get("unstoppable"): st["running"] = False
            save(); sys.exit(0)
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
    if c == "ls":
        if st.get("data_residue") and ("Android/data" in text or "Android/obb" in text):
            out("files\\n"); sys.exit(0)
        out(""); sys.exit(0)
    if c in ("wm", "input", "id", "cat", "mkdir"):
        out(""); sys.exit(0)
    fail("fake adb: unhandled shell " + text)
fail("fake adb: unhandled " + " ".join(argv))
'''


def make_state(root, **kw):
    state = {
        "package": PKG, "installed": False, "installed_sha": "", "running": False,
        "pid": "4242", "base_path": f"/data/app/{PKG}-Ab12/base.apk",
        "orig_apk_hex": "aabbccdd", "uptime_us": 10_000_000,
        "props": {
            "ro.product.model": "PixelTest", "ro.soc.model": "TEST_SOC",
            "ro.build.version.release": "15",
            "debug.s2a.debug_env": "", "debug.s2a.frame_log": "",
        },
        "dumpsys_battery": ("Current Battery Service state:\n  AC powered: false\n  USB powered: false\n"
                            "  status: 3\n  health: 2\n  level: 80\n  voltage: 4000\n  temperature: 350\n"),
        "dumpsys_thermal": "Current Status:0\nType{Name,Forecast}:CPU{CPU,0.5}\n",
        "power_sample": ("status=Discharging\ncurrent_now=-800000\nvoltage_now=4000000\n"
                         "energy_now=\ncharge_counter=\ntemp=350\n"),
        "png": str(root / "shot.png"),
        "logcat_dump": str(root / "logcat_dump.txt"),
        "logcat_stream": str(root / "logcat_stream.txt"),
    }
    state.update(kw)
    path = root / "state.json"
    path.write_text(json.dumps(state))
    return path


def frame_log_text(recorded=200, step_us=16667, first=0, base_ts=10_500_000, pid=4242):
    """v3 帧日志块（镜像 nvn_vk_frame_log.c：24B 记录 = ts_us + fence + acquire + present）。"""
    recs = [struct.pack("<QIIII", base_ts + i * step_us, 0, 0, 900, 1200) for i in range(recorded)]
    blob = b"".join(recs)
    b64 = base64.b64encode(blob).decode()
    parts = [b64[i:i + 700] for i in range(0, len(b64), 700)]
    pre = f"11-09 12:00:01.000  {pid}  {pid} I S2Arm: "
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


def power_tsv(status="Discharging", currents=(-800000,), boots=(11e6, 12e6, 13e6), powered="false"):
    """按 run_game 写出的列序拼 power.tsv（t + FIELDS + level）。"""
    rows = ["t\t" + "\t".join(FIELDS) + "\tlevel"]
    for i, b in enumerate(boots):
        cur = currents[i % len(currents)]
        rows.append("\t".join([str(i), str(int(b)), status, str(cur), "4000000", "", "",
                               "350", "sysfs", powered, powered, powered, powered, "80"]))
    return "\n".join(rows) + "\n"


class RunGameTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="s2a-rungame-"))
        (self.tmp / "shot.png").write_bytes(self._png())
        # dump 里混一个别进程（9999）的完整帧日志块：比主块还长，不绑定 PID 就会被它顶进帧表
        stray = frame_log_text(400, base_ts=10_050_000, pid=9999)
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + stray + frame_log_text())
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
            return base64.b64decode(
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==")

    def _aapt2(self, version_code=200):
        """aapt2 替身：apk_identity 只读 badging 的 package/versionCode 行。"""
        tool = self.tmp / "sdk" / "build-tools" / "36.0.0" / "aapt2"
        tool.parent.mkdir(parents=True, exist_ok=True)
        tool.write_text("#!/bin/sh\necho \"package: name='" + PKG + "' versionCode='" +
                        str(version_code) + "' versionName='1.0'\"\n")
        tool.chmod(0o755)
        return self.tmp / "sdk"

    def run_tool(self, state=None, argv=None, seconds=3, new_vc=200):
        state = make_state(self.tmp, **(state or {}))
        out = self.tmp / "out_1"
        fake = self.tmp / "fake_adb.py"
        fake.write_text(FAKE_ADB)
        old_home = os.environ.get("ANDROID_HOME")
        os.environ["ANDROID_HOME"] = str(self._aapt2(new_vc))
        args = ["--apk", str(self.apk), "--package", PKG, "--serial", SERIAL,
                "--out", str(out), "--seconds", str(seconds),
                "--adb", f"{sys.executable} {fake} {state}",
                "--lock-root", str(self.lock_root), "--ready-timeout", "5"]
        args += argv or []
        try:
            rc = run_game.main(args)
        finally:
            if old_home is None:
                os.environ.pop("ANDROID_HOME", None)
            else:
                os.environ["ANDROID_HOME"] = old_home
        return rc, out, state

    def _qc(self, out):
        try:
            from PIL import Image  # noqa
            sys.path.insert(0, str(REPO / "tools"))
            import quality_check
            return quality_check.check(str(out), "direct", ai=False)
        except ImportError:
            self.skipTest("Pillow 不在，跑不了 quality_check 判读")

    def test_clean_install_run_and_qc_accepts_dir(self):
        rc, out, _ = self.run_tool()
        self.assertEqual(0, rc)
        for name in ("android.json", "logcat.txt", "frames.tsv", "power.tsv",
                     "battery.txt", "thermal.txt", "gfx_mode.txt", "frame_log.txt",
                     "run-manifest.json"):
            self.assertTrue((out / name).is_file(), name)
        self.assertGreaterEqual(len(list((out / "shots_ap").glob("*.png"))), 2)
        meta = json.loads((out / "android.json").read_text())
        self.assertEqual(PKG, meta["package"])
        self.assertEqual("PixelTest", meta["serial_model"])
        self.assertEqual("direct-presenter（非 SurfaceFlinger 合成时序）", meta["frame_log"]["source"])
        # ② 帧表 = 采集窗口内的帧：窗口 [10M, 13M) us（uptime 起步 10M、3 次采样各 +1s），
        #   帧记录 10.5M 起 16667us 步长 → 只收窗口内 150 条 → 149 行间隔；别进程（9999）的块不进来
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual("frame\tpresent_ns\tdisp_us\twall_us", rows[0])
        self.assertEqual(149, len(rows) - 1)
        pres = [int(r.split("\t")[1]) for r in rows[1:]]
        self.assertTrue(all(10_000_000_000 <= p < 13_000_000_000 for p in pres),
                        "窗口外的帧进了 frames.tsv")
        disp = [int(r.split("\t")[2]) for r in rows[1:]]
        self.assertTrue(all(d == 16667 for d in disp))
        self.assertIn("p95", (out / "frame_log.txt").read_text())
        # power.tsv 全字段：status/source/供电旗标都在，不是 plugged 0/1
        pw = (out / "power.tsv").read_text().splitlines()
        self.assertEqual("t\t" + "\t".join(FIELDS) + "\tlevel", pw[0])
        body = [r.split("\t") for r in pw[1:] if r.strip("\t")]
        self.assertTrue(all(r[2] == "Discharging" and r[8] == "sysfs" for r in body))
        # ① quality_check 直接判这个目录：3 秒窗裁出的帧不足 55 秒判据窗，帧率只能没数据（诚实结果，
        #   不能拿最大完整块冒充有效窗）；帧率=过的验收在 test_qc_fps_accepts_full_window 用 55s+ 目录
        verdicts = self._qc(out)
        self.assertEqual("没数据", verdicts["帧率"])
        self.assertEqual("过", verdicts["满屏"])
        self.assertEqual("未证", verdicts["能耗"], "无设备校准的电池侧积分应记未证（不能假绿套 iPhone 门槛）")
        # 跑完锁已放、设备无留置
        sys.path.insert(0, str(ANDROID / "tools"))
        import device_lock
        st = device_lock.status(str(self.lock_root), SERIAL)
        self.assertFalse(st["held"], st)
        self.assertFalse((self.lock_root / SERIAL / "detained.json").exists())

    def test_refuses_when_already_installed(self):
        """③ 已装该包 = 私有数据保证不了还原 → 直接拒跑（装/启动都不该发生）。"""
        orig_sha = hashlib.sha256(bytes.fromhex("aabbccdd")).hexdigest()
        rc, out, state = self.run_tool(state={"installed": True, "installed_sha": orig_sha})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "拒跑不该有任何安装动作")
        self.assertFalse(st["running"], "拒跑不该启动游戏")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("私有数据", man["reason"])

    def test_pm_path_query_failure_refuses_before_run(self):
        """pm path 查询失败（stderr + 非 0）→ 红、起局前就停，不留置（还没动过设备）。"""
        rc, out, state = self.run_tool(state={"pm_path_fail_at": 1})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "查询失败不该有任何安装动作")
        self.assertFalse(st["running"], "查询失败不该启动游戏")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("pm path", man["reason"])
        self.assertFalse((self.lock_root / SERIAL / "detained.json").exists(),
                         "设备还没被碰过，查询失败不该留置")

    def test_pm_path_rc1_with_stderr_is_failure_not_uninstalled(self):
        """rc=1 但带 stderr = adb 层查询失败，不能当『未安装』放行起局。"""
        rc, out, state = self.run_tool(state={"pm_path_fail_at": 1, "pm_path_fail_rc": 1})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "rc=1 带 stderr 必须判查询失败")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("pm path", man["reason"])

    def test_pm_path_failure_during_restore_recheck_detains(self):
        """收局卸载后复查 pm path 查询失败 → 卸载结果未证 → 还原失败留置。"""
        # 未装路径 pm path 调用序：① 入局 install_state ② 收局 restore_state 入口 ③ 卸载后复查
        rc, out, state = self.run_tool(state={"pm_path_fail_at": 3})
        self.assertEqual(3, rc, "复查失败应留置（exit 3）")
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists())

    def test_restores_original_apk_with_data_loss_flag(self):
        """③ --allow-data-loss 放行：只还原 APK 字节（装回原包复核 sha）。"""
        orig_sha = hashlib.sha256(bytes.fromhex("aabbccdd")).hexdigest()
        rc, out, state = self.run_tool(state={"installed": True, "installed_sha": orig_sha},
                                     argv=["--allow-data-loss"])
        self.assertEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue(st["installed"])
        self.assertEqual(orig_sha, st["installed_sha"], "还原后设备上应是原包")
        self.assertGreaterEqual(len(st["installs"]), 2, "应装过测试包再装回原包")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("不还原", man["restore_scope"]["private_data"])

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

    def test_prop_restore_mismatch_detains(self):
        """⑦ 调试属性恢复读回不符（设备把值卡死在别处）→ 留置。"""
        rc, out, _ = self.run_tool(state={"props_jam": True})
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "属性读回不符应留置")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("恢复失败", man["reason"])

    def test_unstoppable_game_detains(self):
        """⑦ 游戏收局后仍存活（stop 止不住）→ 收局确认未过 → 留置。"""
        rc, out, _ = self.run_tool(state={"unstoppable": True})
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "游戏未停应留置")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("仍存活", man["reason"])

    def test_data_residue_detains(self):
        """⑦ 卸载后 /sdcard/Android/data|obb/<pkg> 有残留 → 还原没保证 → 留置。"""
        rc, out, _ = self.run_tool(state={"data_residue": True})
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "目录残留应留置")

    def test_sigterm_still_restores(self):
        """⑥ SIGTERM 落在采集窗口里：finally 照常收局 + 卸载还原，设备不留置。"""
        state = make_state(self.tmp)
        fake = self.tmp / "fake_adb.py"
        fake.write_text(FAKE_ADB)
        out = self.tmp / "sig_out"
        proc = subprocess.Popen(
            [sys.executable, str(ANDROID / "tools" / "run_game.py"),
             "--apk", str(self.apk), "--package", PKG, "--serial", SERIAL,
             "--out", str(out), "--seconds", "30",
             "--adb", f"{sys.executable} {fake} {state}",
             "--lock-root", str(self.lock_root), "--ready-timeout", "5"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.time() + 30
        while time.time() < deadline and not (
                (out / "run.log").exists() and "READY" in (out / "run.log").read_text()):
            time.sleep(0.2)
        self.assertTrue((out / "run.log").exists() and "READY" in (out / "run.log").read_text(),
                        "30s 内没等到 READY，测试环境有问题")
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=60)
        self.assertEqual(2, proc.returncode, "SIGTERM 应退 2（中断但已还原）")
        st = json.loads(state.read_text())
        self.assertFalse(st["installed"], "SIGTERM 后测试包应被卸载还原")
        self.assertFalse((self.lock_root / SERIAL / "detained.json").exists(),
                         "还原成功不该留置")

    def test_mode_direct_marks_launch_line(self):
        self.run_tool(argv=["--mode", "direct"], state={})
        meta = json.loads((self.tmp / "out_1" / "android.json").read_text())
        self.assertIn("--es s2arm.graphics_mode direct", meta["launch"])

    def test_debug_env_extra_reaches_launch(self):
        """s2arm.debug_env 的值自带 =（S2A_KEY=VALUE）：放行并进启动行；非法值拒绝。"""
        self.run_tool(argv=["--extra", "s2arm.debug_env=S2A_DEBUG_CAPACITY_AFFINITY=0"], state={})
        meta = json.loads((self.tmp / "out_1" / "android.json").read_text())
        self.assertIn("--es s2arm.debug_env S2A_DEBUG_CAPACITY_AFFINITY=0", meta["launch"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertIn("s2arm.debug_env S2A_DEBUG_CAPACITY_AFFINITY=0", st["launch_cmd"])
        err = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(err):
            run_game.main(["--apk", str(self.apk), "--package", PKG, "--serial", SERIAL,
                           "--out", str(self.tmp / "bad_out"), "--seconds", "1",
                           "--adb", "adb", "--lock-root", str(self.lock_root),
                           "--extra", "s2arm.debug_env=not-s2a-key=0"])
        self.assertIn("S2A_KEY=VALUE", err.getvalue())

    def _qc_energy(self, tsv):
        """最小局目录只给 power.tsv / android.json，单独验能耗判据。"""
        d = self.tmp / "qc-energy"
        if d.exists():
            for x in d.iterdir():
                x.unlink()
        else:
            d.mkdir()
        (d / "android.json").write_text("{}")
        (d / "power.tsv").write_text(tsv)
        return self._qc(d)["能耗"]

    def test_qc_fps_accepts_full_window(self):
        """帧率判据窗 ≥55s：合成 4000 行 16667us 帧表 → quality_check 判「过」。"""
        d = self.tmp / "qc-fps"
        d.mkdir()
        (d / "android.json").write_text("{}")
        with open(d / "frames.tsv", "w") as fh:
            fh.write("frame\tpresent_ns\tdisp_us\twall_us\n")
            for i in range(4000):
                fh.write(f"{i + 1}\t{10_000_000_000 + i * 16_667_000}\t16667\t{10_000_000_000 + i * 16_667_000}\n")
        self.assertEqual("过", self._qc(d)["帧率"])

    def test_power_energy_verdicts(self):
        """⑧ 能耗口径：合法放电 → 未证；未知 / 接电 / 符号不一致 / 时钟不齐 → 没数据。"""
        self.assertEqual("未证", self._qc_energy(power_tsv()),
                         "整窗放电且符号一致：诊断积分应记未证")
        self.assertEqual("没数据", self._qc_energy(power_tsv(status="Unknown")),
                         "未知充电状态不能当未接电")
        self.assertEqual("没数据", self._qc_energy(power_tsv(status="Charging")),
                         "窗口内接过电不算")
        self.assertEqual("没数据", self._qc_energy(power_tsv(currents=(-800000, 800000))),
                         "电流符号不一致 = 放电方向未校准")
        self.assertEqual("没数据", self._qc_energy(power_tsv(currents=(0,))),
                         "电流全零推不出放电方向")
        self.assertEqual("没数据", self._qc_energy(power_tsv(boots=(11e6, 12e6, 11e6))),
                         "采样时钟倒退，窗口不成立")

    # ---- Android 16 pm path rc=0 收紧：rc=0 必须有 package: 行且无 stderr ----

    def test_pm_path_rc0_empty_is_failure(self):
        """rc=0 但无输出（设备异常应答）：必须判查询失败、不继续，不当未装放行。"""
        rc, out, state = self.run_tool(state={"pm_path_empty_at": 1})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "查询失败不该有任何安装动作")
        self.assertFalse((self.lock_root / SERIAL / "detained.json").exists(),
                         "设备还没被动过，不该留置")

    def test_pm_path_rc0_stderr_is_failure(self):
        """rc=0 有 package: 行但带 stderr（adb 警告/异常通道）：必须判查询失败、不继续。"""
        rc, out, state = self.run_tool(
            state={"installed": True, "installed_sha": "0" * 64, "pm_path_warn_at": 1})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "带 stderr 的 rc=0 不能当已装/未装放行")

    # ---- 资源部署：整树推送进度 / 超时 / tar 单推 + SHA 核对 ----

    def _resources_dir(self, npayload=6):
        """能过 deploy_game.validate() 的最小资源目录：artifact/index/manifest 身份自洽。"""
        res = self.tmp / "resources"
        res.mkdir()
        for i in range(npayload):
            sub = res / f"romfs/dir{i % 2}"
            sub.mkdir(parents=True, exist_ok=True)
            (sub / f"f{i}.bin").write_bytes(f"payload-{i}".encode() * (20 * (i + 1)))
        index = res / "index.tsv"
        index.write_text("rel\tsha256\n" + "".join(f"romfs/{i}\t{i:064x}\n" for i in range(npayload)))
        idx_sha = hashlib.sha256(index.read_bytes()).hexdigest()
        artifact = {"titleId": "0123456789ABCDEF",
                    "apk": {"packageName": PKG, "versionCode": 200,
                            "sha256": hashlib.sha256(self.apk.read_bytes()).hexdigest()},
                    "contentRoot": "sha256-" + idx_sha}
        (res / "game-build-artifact.json").write_text(json.dumps(artifact))
        (res / "manifest.json").write_text(json.dumps({"index_sha256": idx_sha}))
        return res, artifact

    def _with_receipt(self, artifact):
        """logcat -d 要含导入成功行 + require_current 回执（staging 名 = deploy-<work> = deploy-deploy）。"""
        art_sha = hashlib.sha256(
            (self.tmp / "resources" / "game-build-artifact.json").read_bytes()).hexdigest()
        name = "deploy-deploy"
        (self.tmp / "logcat_dump.txt").write_text(
            LOG_HEAD
            + f"11-09 12:00:02.000  4242  4242 I S2Arm: Game build artifact updated from import/{name}\n"
            + f"11-09 12:00:02.100  4242  4242 I S2Arm: Game import receipt: {artifact['titleId']} "
              f"contentRoot={artifact['contentRoot']} artifactSha256={art_sha} from import/{name}\n"
            + frame_log_text())

    def test_resources_tree_push_progress_logged(self):
        """资源整树推送：文件数+字节+速率进度打进 run.log；--push-timeout 照传。"""
        res, artifact = self._resources_dir()
        self._with_receipt(artifact)
        rc, out, state = self.run_tool(
            state={"push_slow_s": 6},
            argv=["--resources", str(res), "--push-timeout", "600"])
        self.assertEqual(0, rc)
        runlog = (out / "run.log").read_text()
        self.assertIn("整树推送", runlog)
        self.assertIn("推送中", runlog, "慢推送中段应有进度行（文件数/字节/速率）")
        self.assertIn("推送完成", runlog)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("tar_pushed", st, "默认整树推送不该推 tar")

    def test_resources_push_timeout_is_enforced(self):
        """--push-timeout 真传到推送：推送卡住到点被杀，错误带最后进度。"""
        res, artifact = self._resources_dir()
        self._with_receipt(artifact)
        rc, out, state = self.run_tool(
            state={"push_slow_s": 30},
            argv=["--resources", str(res), "--push-timeout", "2"])
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("推送超时", man["reason"])
        self.assertIn("最后进度", man["reason"], "超时错误要留最后进度，看得出卡在哪")

    def test_resources_push_tar_extracts_and_verifies(self):
        """--push-tar always：打一个 tar 推一次、设备端解开、逐文件 SHA 核对写进 run.log。"""
        res, artifact = self._resources_dir()
        self._with_receipt(artifact)
        rc, out, state = self.run_tool(
            argv=["--resources", str(res), "--push-tar", "always"])
        self.assertEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue(st.get("tar_pushed", "").endswith(".tar"), "应推的是 tar 归档")
        runlog = (out / "run.log").read_text()
        self.assertIn("tar 单推", runlog)
        self.assertIn("SHA 核对通过", runlog)

    def test_resources_push_tar_corrupt_extract_fails(self):
        """tar 解开被改写 → 设备端逐文件 SHA 核对必须拦下（负对照：核对在起作用）。"""
        res, artifact = self._resources_dir()
        self._with_receipt(artifact)
        rc, out, state = self.run_tool(
            state={"corrupt_extract": True},
            argv=["--resources", str(res), "--push-tar", "always"])
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("SHA 核对失败", man["reason"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
