#!/usr/bin/env python3
"""Android 真机最小闭环：装包 → 冷启动 → 按时长采集 → 收局 → 还原安装状态，
产出 tools/quality_check.py 安卓分支直接判读的局目录（android.json / shots_ap / frames.tsv /
power.tsv / logcat.txt / gfx_mode.txt / battery.txt / thermal.txt / frame_log.txt；
另有 logcat_d.txt（收局整窗 dump）与 run-manifest.json（原状 / 还原范围 / 失败原因 / 注记））。

    run_game.py --apk game.apk --package org.s2arm.game.t<TITLE> --serial S --out DIR --seconds 60
                [--resources DIR | --pack DIR]     # 资源：走 deploy_game.deploy_resources（不启动）
                [--mode auto|generic|direct]      # s2arm.graphics_mode 冷启动 intent extra
                [--extra s2arm.KEY[=VALUE] ...]   # 其他 intent extra（true/false 走 --ez）
                [--adb "adb"] [--ready-timeout 180] [--wait 0] [--allow-data-loss]
                [--backup-dir DIR] [--lock-root DIR] [--holder NAME] [--note TXT]
                [--push-timeout S] [--push-tar auto|always|never]

锁：取 device_lock.py 的持久内核锁后才动设备，收局按持锁令牌 release；
崩溃 / 强杀 / adb 掉线 = 未正常收局，设备留置不放行（不自动放）。

还原范围（保证不了的一律拒跑，不返回成功兜底）：
- 原 APK：设备已装该包且构建不同 → adb pull 备份、收局 install -r 装回并 sha256 复核；
  分包（split APK）装回不可靠 → 入局即拒。原包 versionCode 低于新包时装回是降级：
  装新包前先验设备 pm 支持 install -d（usage 里的「-d: allow version code downgrade」），
  不支持就拒跑而不是装了回不来；原包高于新包时装测试包本身也带 -d。
- 应用私有数据 / 已并入资源：已装包是 debuggable → 装新包前 run-as tar 把
  /data/data/<pkg>、/sdcard/Android/data|obb/<pkg> 拉到 --backup-dir（默认 <out>/data-backup，
  tar + manifest.json 逐文件 sha256 清单）；收局装回原包后 pm clear、回灌、逐文件核 sha。
  已装包不是 debuggable → 拒跑；显式 --allow-data-loss 才放行且只还原 APK 字节
  （私有数据、已并入资源不还原，manifest 写明）。
- 导入的资源 / 卸载应清的目录：未安装路径落在 /sdcard/Android/data|obb/<pkg>，卸载随删，
  收局逐项验不存在；debuggable 已装路径随数据备份还原。
- 调试属性 debug.s2a.*：起局前置 1，收局恢复原值并逐项读回核对（release 机不让改 → 只记注）。

收局：任何结果（含 SIGTERM / SIGHUP / Ctrl+C / 未捕获异常）都走 finally 还原；
先确认游戏进程与采集进程都停了，再逐项验还原。还原失败 → RESULT=red + 留置标。
安全边界：只经 adb 做 安装/启动/采集/截图——不重启、不刷机、不 root、不改系统设置。
帧表：只写采集窗口 [起, 止) 内的 direct-present 帧——帧记录是 CLOCK_BOOTTIME，窗口边界取设备
/proc/uptime 同一时钟；帧日志块按 logcat 行 PID 绑定当局游戏进程，别的进程的块不进表。
退出码：0 完成且还原成功；2 参数 / 设备 / 起局 / 中断（设备已尽量还原）；3 还原失败（已留置）。
"""
import argparse, datetime, hashlib, json, os, re, shlex, signal, subprocess, sys, tempfile, time
from pathlib import Path

ANDROID = Path(__file__).resolve().parents[1]
REPO = ANDROID.parent
for _p in (ANDROID, ANDROID / "tools", REPO / "tools"):
    sys.path.insert(0, str(_p))
import android_power          # noqa: E402  采样命令与解析：tools/android_power.py
import device_lock            # noqa: E402  本目录设备锁
import dev_runtime            # noqa: E402  Device/stop 复用
import direct_framelog        # noqa: E402  帧日志解析与百分位

ACTIVITY = dev_runtime.ACTIVITY
DEBUG_PROPS = ("debug.s2a.debug_env", "debug.s2a.frame_log")
READY = "Game startup accepted:"
FAIL_RE = re.compile(r"Game startup refused|startup rejected|FATAL EXCEPTION|AndroidRuntime: FATAL")
FRAMELOG_END = "direct frame-log end:"
PKG_RE = re.compile(r"[A-Za-z0-9_.]+")
THREADTIME_PID = re.compile(r"^\d\d-\d\d\s+\d\d:\d\d:\d\d\.\d+\s+(\d+)\s+\d+\s+\w")


class Refuse(Exception):
    pass


class Abort(Exception):
    """SIGTERM / SIGHUP 转成异常：走 finally 照常还原。"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def pm_path_list(d, pkg):
    """pm path 三态（qa-fone 定口径）。已装 = rc=0 且有 package: 行且无 stderr；
    未装 = Android 16 口径的 rc=1 且 stdout/stderr 皆空；其余一切（rc=0 无
    package: 行、rc=0/1 带 stderr、其他 rc）= 查询失败抛 RuntimeError。
    不能用 d.shell：它只回 stdout，拿不到 stderr 区分『未装』与『查询失败』。"""
    r = subprocess.run([*d.argv, "shell", f"pm path {pkg}"],
                       capture_output=True, timeout=60)
    out, err = r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")
    if r.returncode == 0:
        paths = [ln[len("package:"):] for ln in out.splitlines() if ln.startswith("package:")]
        if paths and not err.strip():
            return paths
        raise RuntimeError(f"pm path {pkg} 查询失败 rc=0 但"
                           + (f"带 stderr: {err.strip()[:200]}" if err.strip()
                              else "无 package: 行"))
    if r.returncode == 1 and not out.strip() and not err.strip():
        return []
    raise RuntimeError(f"pm path {pkg} 查询失败 rc={r.returncode}: "
                       f"{(err or out).strip()[:300]}")


def install_state(d, pkg):
    """pm path + dumpsys package → {installed, split, base, sha256, versionCode, debuggable}。
    split 包无法忠实还原，调用方拒；versionCode/debuggable 供降级与 run-as 备份判断。"""
    paths = pm_path_list(d, pkg)
    if not paths:
        return {"installed": False}
    info = d.shell(f"dumpsys package {pkg}", allowed=(0, 1))
    m = re.search(r"versionCode=(\d+)", info)
    meta = {"versionCode": int(m.group(1)) if m else None,
            "debuggable": "DEBUGGABLE" in info}
    base = [p for p in paths if p.endswith("/base.apk")]
    if len(paths) != 1 or len(base) != 1:
        return {"installed": True, "split": True, "paths": paths, **meta}
    return {"installed": True, "base": base[0],
            "sha256": d.shell(f"sha256sum {shlex.quote(base[0])}").split()[0], **meta}


def device_allows_downgrade(d):
    """设备 pm 是否支持 install -d（降级）。pm usage 里有『-d: allow version code downgrade』。"""
    usage = d.shell("pm install --help 2>&1 || pm 2>&1", allowed=(0, 1, 255))
    return bool(re.search(r"-d[^\n]*downgrade", usage, re.I))


DATA_DIRS = (("private", "/data/data/{pkg}"),
             ("extdata", "/sdcard/Android/data/{pkg}"),
             ("obb", "/sdcard/Android/obb/{pkg}"))
SHA_LINE = re.compile(r"^([0-9a-f]{64})\s+\.?/?(.+)$")


def _sha_listing(text):
    """『<sha>  ./relpath』行 → {relpath: sha}（find -exec sha256sum 的输出口径）。"""
    files = {}
    for ln in text.splitlines():
        m = SHA_LINE.match(ln.strip())
        if m:
            files[m.group(2)] = m.group(1)
    return files


def backup_data(d, pkg, dest):
    """debuggable 原包的私有 / 外部数据 → exec-out run-as tar 拉回 + find sha256sum 清单。
    任一目录备份失败即 Refuse（保证不了还原就不入局）。"""
    dest.mkdir(parents=True)
    manifest = {"schema": 1, "package": pkg, "dirs": {}}
    for tag, tpl in DATA_DIRS:
        dir_ = tpl.format(pkg=pkg)
        present = d.run_as(pkg, f"[ -d {shlex.quote(dir_)} ] && echo yes || true").strip()
        if "yes" not in present:
            continue
        tar = dest / f"{tag}.tar"
        # exec-out 不经 pty：二进制流不被 \r\n 改写；run-as 只给 debuggable 包
        r = subprocess.run([*d.argv, "exec-out", "run-as", pkg, "sh", "-c",
                            f"tar -cf - -C {shlex.quote(dir_)} ."],
                           capture_output=True, timeout=600)
        if r.returncode or not r.stdout:
            raise Refuse(f"{dir_} 备份失败 rc={r.returncode}（run-as/tar 不可用就当非 debuggable 拒跑）")
        tar.write_bytes(r.stdout)
        listing = d.run_as(pkg, f"cd {shlex.quote(dir_)} && find . -type f -exec sha256sum {{}} +",
                           timeout=300)
        manifest["dirs"][tag] = {"dir": dir_, "tar": tar.name,
                                 "tar_sha256": sha256(tar), "files": _sha_listing(listing)}
    (dest / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
    return manifest


def restore_data(d, pkg, manifest, backup_dir):
    """先清空再按清单回灌、逐文件 sha 核。返回失败说明串列表（空=成功）。"""
    fails = []
    try:
        d.shell(f"pm clear {pkg}", allowed=(0, 1))
    except (RuntimeError, subprocess.SubprocessError) as error:
        fails.append(f"pm clear 异常：{error}")
    for tag, info in manifest["dirs"].items():
        dir_ = info["dir"]
        try:
            d.run_as(pkg, f"mkdir -p {shlex.quote(dir_)}; find {shlex.quote(dir_)} -mindepth 1 -delete",
                     timeout=120)
            tmp = f"/data/local/tmp/s2a-restore-{tag}.tar"
            d.run("push", str(Path(backup_dir) / info["tar"]), tmp, timeout=300)
            d.run_as(pkg, f"tar -xf {tmp} -C {shlex.quote(dir_)}", timeout=600)
            d.shell(f"rm -f {tmp}", allowed=(0, 1))
            got = _sha_listing(d.run_as(
                pkg, f"cd {shlex.quote(dir_)} && find . -type f -exec sha256sum {{}} +", timeout=300))
            want = info["files"]
            bad = [rel for rel, sha in want.items() if got.get(rel) != sha]
            extra = sorted(set(got) - set(want))
            if bad:
                fails.append(f"{dir_} 还原后 {len(bad)} 个文件 sha 与备份清单不符：{bad[:3]}")
            if extra:
                fails.append(f"{dir_} 还原后多出 {len(extra)} 个未备份文件：{extra[:3]}")
        except (RuntimeError, subprocess.SubprocessError) as error:
            fails.append(f"{dir_} 数据还原 adb 异常：{error}")
    return fails


def uptime_us(d):
    """设备 CLOCK_BOOTTIME 微秒（/proc/uptime 第一项）——与帧记录 ts 同一时钟域。"""
    fields = d.shell("cat /proc/uptime", timeout=10).split()
    return int(float(fields[0]) * 1_000_000)


def session_pids(d, pkg):
    """当局游戏进程：进程名 == pkg 或 pkg:<子进程名>（引擎把游戏跑在 :game 子进程）。"""
    out = d.shell("ps -A -o PID= -o NAME=", allowed=(0, 1), timeout=15)
    pids = {}
    for ln in out.splitlines():
        part = ln.split(None, 1)
        if len(part) == 2 and (part[1] == pkg or part[1].startswith(pkg + ":")):
            pids[int(part[0])] = part[1]
    return pids


def pid_birth(d, pid):
    """进程出生证：/proc/<pid>/stat 第 22 项 starttime（开机 jiffies）。"""
    try:
        stat = d.shell(f"cat /proc/{pid}/stat", allowed=(0, 1), timeout=10).strip()
        tail = stat.rsplit(")", 1)[-1].split()
        return int(tail[19]) if len(tail) > 19 else None
    except (IndexError, RuntimeError, ValueError):
        return None


def frame_log_blocks(text, pids=None):
    """meta→end 切帧日志块；给 pids 时只收这些 PID 的行（logcat 行前缀 PID 绑定当局进程）。
    没前缀 / 别的 PID 的帧日志行一律不进块；末尾被截断的块只坏自己。
    截断块（没等到 end：被新 meta 顶断 / 文末就断了）同样交出——缺了哪些片、
    缺 end/SHA 由 parser 如实报「转储被截断 / 整段未证」，不静默丢。"""
    seg = None
    for line in text.splitlines():
        if "direct frame-log" not in line:
            continue
        if pids is not None:
            m = THREADTIME_PID.match(line)
            if not m or int(m.group(1)) not in pids:
                continue
        if "direct frame-log meta:" in line:
            if seg is not None:
                yield "\n".join(seg)
            seg = []
        if seg is not None:
            seg.append(line)
            if "direct frame-log end:" in line:
                yield "\n".join(seg)
                seg = None
    if seg is not None:
        yield "\n".join(seg)


def _clock_norm(name):
    """帧块 clock 标签规范名：设备写 CLOCK_BOOTTIME，旧替身写 boottime——
    剥 CLOCK_ 前缀、大小写无关后判同一时钟域；别的值照旧拒（对不上窗口时钟）。"""
    name = str(name or "").upper()
    return name[len("CLOCK_"):] if name.startswith("CLOCK_") else name


def write_frames(out, caps, notes, raw_text, window_us=None):
    """frames.tsv：只写采集窗口 [w0,w1) 内的 direct-present 帧。
    帧记录 CLOCK_BOOTTIME，窗口边界同样取设备 /proc/uptime —— 同一时钟裁窗；
    不拿「最大完整块」当有效窗：块里窗口外的帧（启动期 / 收局后）不进表；
    覆盖事实（span / 首尾空档 / 占比）写进返回值，覆盖不足由判读者如实判。"""
    header = "frame\tpresent_ns\tdisp_us\twall_us\n"
    (out / "frames.tsv").write_text(header)
    kept = []
    for cap in caps:
        if _clock_norm(cap["meta"].get("clock")) != "BOOTTIME":
            notes.append(f"块@line{cap['meta']['line']} clock={cap['meta'].get('clock')} 不是 "
                         f"boottime，对不上窗口时钟，整段未证")
            continue
        rec = [(i, t) for i, t in enumerate(cap["ts"])
               if window_us is None or window_us[0] <= t < window_us[1]]
        if rec:
            kept.append((cap, rec))
    rows, cover = 0, None
    if kept:
        cap, rec = max(kept, key=lambda k: len(k[1]))
        first = int(cap["meta"]["first"])
        with (out / "frames.tsv").open("w") as f:
            f.write(header)
            for j in range(1, len(rec)):
                (i0, t0), (i1, t1) = rec[j - 1], rec[j]
                # stats() 同一口径：帧号 = 1 基 present 序号；disp_us = 相邻上屏间隔
                f.write(f"{first + i1 + 1}\t{t1 * 1000}\t{t1 - t0}\t{t1 - t0}\n")
                rows += 1
        if window_us:
            w0, w1 = window_us
            t_first, t_last = rec[0][1], rec[-1][1]
            cover = {"span_us": t_last - t_first,
                     "head_gap_us": t_first - w0, "tail_gap_us": w1 - t_last,
                     "ratio": round((t_last - t_first) / (w1 - w0), 4) if w1 > w0 else None}
            if cover["ratio"] is not None and cover["ratio"] < 0.95:
                notes.append(f"帧表只覆盖采集窗口的 {cover['ratio']:.0%}：窗口 {w1 - w0}us，"
                             f"帧覆盖 {t_last - t_first}us（首部空 {t_first - w0}us、"
                             f"尾部空 {w1 - t_last}us）——按窗口覆盖不足如实判")
    else:
        notes.append("采集窗口内没有 direct-present 帧（正式构建无 debug 门 / 非直连 provider 时为预期）")
    reports = []
    for i, c in enumerate(caps):
        try:
            reports.append(direct_framelog.report(c, raw_text))
        except Exception as error:
            reports.append(f"capture {i + 1}: 统计失败 {error}")
    (out / "frame_log.txt").write_text("\n".join(reports) + "\n")
    return {"source": "direct-presenter（非 SurfaceFlinger 合成时序）",
            "window_boot_us": list(window_us) if window_us else None,
            "cover": cover, "rows": rows, "captures": len(caps)}


def external_dirs_gone(d, pkg):
    """卸载后 /sdcard/Android/data|obb/<pkg> 应随删；逐项验不存在。"""
    fails = []
    for base in ("/sdcard/Android/data", "/sdcard/Android/obb"):
        out = d.shell(f"ls {base}/{pkg} 2>/dev/null", allowed=(0, 1)).strip()
        if out:
            fails.append(f"{base}/{pkg} 卸载后仍有内容：{out.splitlines()[0][:80]}")
    return fails


def restore_state(d, pkg, state0, orig_apk, downgrade=False, data_backup=None, backup_dir=None):
    """还原原安装状态（范围见 docstring）。返回失败说明串列表（空=成功）。
    downgrade=原包 versionCode 低于测试包 → install -r -d；data_backup=入局前的
    run-as 备份清单（debuggable 路径），给时先装回原 APK 再逐文件 sha 核还原数据。"""
    fails = []
    now = install_state(d, pkg)
    if not state0["installed"]:
        if now["installed"]:
            try:
                d.run("uninstall", pkg, timeout=120)
            except RuntimeError as error:
                fails.append(f"卸载 {pkg} 失败：{error}")
            if install_state(d, pkg)["installed"]:
                fails.append("卸载后 pm path 仍显示已安装")
                return fails
        fails += external_dirs_gone(d, pkg)
        return fails
    if state0.get("split"):
        return []  # 入局前已拒，走不到这里；防御性返回
    if now["installed"] and now.get("sha256") == state0["sha256"]:
        pass  # 原包字节已在；只还可能要还原数据
    elif orig_apk is None:
        return ["设备上的包与入局前记录不符，且没有原包备份可装回"]
    else:
        try:
            d.run("install", "-r", *(["-d"] if downgrade else []), str(orig_apk), timeout=600)
        except RuntimeError as error:
            return [f"装回原包失败：{error}"]
        now = install_state(d, pkg)
        if not now["installed"] or now.get("sha256") != state0["sha256"]:
            return ["装回原包后 sha256 与入局前记录不符"]
    if data_backup is not None:
        fails += restore_data(d, pkg, data_backup, backup_dir)
    return fails


def verify_stopped(d, pkg, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not d.shell(f"pidof {pkg}", allowed=(0, 1)).strip():
            return True
        time.sleep(0.5)
    return False


def frame_dump_done(d, pkg, since):
    """优雅收局触发的帧日志转储落定判据：logcat 见到 end 行，或当局进程
    （pkg / pkg:*）全退——都退了转储不会再长，截断由帧块解析如实报；
    adb 坏了也当落定，不能在这里等死。"""
    try:
        if FRAMELOG_END in d.shell(f"logcat -d -v brief -T {shlex.quote(since)} -s S2Arm:I"):
            return True
        return not session_pids(d, pkg)
    except (RuntimeError, subprocess.SubprocessError):
        return True


POWER_LOG = "/data/local/tmp/s2a-power-samples.log"


def start_power_sampler(d, supply):
    """设备侧采样守护：mksh 后台 while 循环 1Hz 写 `===` 分隔的采样块到
    /data/local/tmp——adb 掉线只断拉取、不断设备上的采样（兄弟项目
    「应用私存数据、拔电期间 adb 断开也不丢」同语义）。整串必须走 sh -c。"""
    dest = POWER_LOG
    inner = (f"rm -f {dest}; ( while :; do {android_power.sample_command(supply)}; "
             f"printf 'capacity=%s\\n' \"$(cat /sys/class/power_supply/{supply}/capacity 2>/dev/null)\"; "
             f"printf '{android_power.BLOCK_SEP}\\n'; sleep 1; done )"
             f" >{dest} 2>/dev/null </dev/null & echo $!")
    out = d.shell("sh -c " + shlex.quote(inner), timeout=15).strip()
    return dest, out.splitlines()[-1].strip()


def power_rows(d, dest):
    """cat 设备采样文件 → [(row, level)]；末段未封闭的尾巴不算（下轮再收）。"""
    out = []
    text = d.shell(f"cat {dest}", allowed=(0, 1), timeout=20)
    for block in android_power.power_log_blocks(text):
        try:
            row = android_power.parse_sample(block + "\n")
        except (RuntimeError, ValueError):
            continue  # 坏块跳过（设备文件原样留着，不丢证据）
        level = (re.search(r"^capacity=(\d+)$", block, re.M) or [None, ""])[1]
        out.append((row, level))
    return out


def stop_power_sampler(d, dest, pid):
    """杀守护 → 拉回全量 → 清设备文件；返回 [(row, level)]。"""
    d.shell(f"kill {pid}", allowed=(0, 1), timeout=10)
    rows = power_rows(d, dest)
    d.shell(f"rm -f {dest}", allowed=(0, 1), timeout=10)
    return rows


def shot(adb_argv, dest):
    r = subprocess.run([*adb_argv, "exec-out", "screencap", "-p"],
                       capture_output=True, timeout=30)
    data = r.stdout
    if not data.startswith(b"\x89PNG"):  # 老设备经 pty 会把 \n 换成 \r\n
        data = r.stdout.replace(b"\r\n", b"\n")
    if r.returncode or not data.startswith(b"\x89PNG"):
        raise Refuse(f"screencap 失败 rc={r.returncode} {r.stderr[:120]!r}")
    dest.write_bytes(data)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--apk", required=True, type=Path)
    p.add_argument("--package", help="包名（给了 --resources/--pack 时可省略，按 artifact 取）")
    p.add_argument("--serial", required=True)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--seconds", type=int, required=True)
    src = p.add_mutually_exclusive_group()
    src.add_argument("--resources")
    src.add_argument("--pack")
    p.add_argument("--mode", choices=["auto", "generic", "direct"], default="auto")
    p.add_argument("--extra", action="append", default=[], help="s2arm.KEY[=VALUE]，可多次")
    p.add_argument("--allow-data-loss", action="store_true",
                   help="设备已装该包也照跑：只还原 APK 字节，私有数据与已并入资源不还原（显式接受数据丢失）")
    p.add_argument("--backup-dir",
                   help="已装 debuggable 包的私有/外部数据备份落点（默认 <out>/data-backup）；"
                        "tar + manifest.json（逐文件 sha256 清单），收局按它回灌核对")
    p.add_argument("--adb", default="adb")
    p.add_argument("--push-timeout", type=int, default=None,
                   help="资源推送超时秒数；缺省按资源字节+文件数估（约 30MB/s + 20ms/文件、2 倍余量）")
    p.add_argument("--push-tar", choices=["auto", "always", "never"], default="auto",
                   help="资源推送方式：auto 小文件密集自动 tar 单推解开；always 强制；never 整树推")
    p.add_argument("--ready-timeout", type=int, default=180)
    p.add_argument("--discharge-settle", type=int, default=30,
                   help="采集窗开始前要连续物理放电的秒数（能耗口径；0=不等）")
    p.add_argument("--discharge-timeout", type=int, default=300,
                   help="放电稳定门的等待上限秒数（0=等到够为止；超时照常采集、能耗记没数据）")
    p.add_argument("--power-interval", type=float, default=1.0,
                   help="拉取设备侧电池采样文件的间隔秒")
    p.add_argument("--wait", type=float, default=0)
    p.add_argument("--lock-root", default=str(device_lock.lock_root()))
    p.add_argument("--holder", default=os.environ.get("USER", "?"))
    p.add_argument("--note", default="")
    args = p.parse_args(argv)

    try:
        device_lock.serial_ok(args.serial)
    except ValueError as error:
        p.error(str(error))
    if not args.apk.is_file():
        p.error(f"{args.apk} 不存在")
    if not 1 <= args.seconds <= 600:
        p.error("--seconds 在 1 到 600 之间")
    if args.package and not PKG_RE.fullmatch(args.package):
        p.error(f"非法包名 {args.package!r}")
    if not args.package and not (args.resources or args.pack):
        p.error("apk 直装需要 --package（带 --resources/--pack 时按 artifact 取包名）")
    for e in args.extra:
        if not dev_runtime.EXTRA.fullmatch(e):
            p.error(f"--extra 只收 s2arm.KEY[=VALUE]：{e!r}")

    d = dev_runtime.Device(args.adb, args.serial)
    adb_argv = d.argv
    lock = Path(args.lock_root)

    rc, token = device_lock.acquire(lock, args.serial, args.holder, os.getpid(),
                                    wait=args.wait, note=args.note)
    if rc:
        print(token, file=sys.stderr)
        return rc

    # SIGTERM / SIGHUP → Abort 异常：照常走 finally 还原；SIGKILL / 宿主断电 = 未正常收局，
    # 内核锁自动放但 holder.json 留下 → 下一手取锁按留置拒（不自动放）
    def _abort(sig, _frame):
        raise Abort(f"收到信号 {sig}")
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, _abort)

    out, notes = args.out, []
    log_path = out / "run.log"
    t0 = datetime.datetime.now(datetime.timezone.utc)
    manifest = {"schema": 1, "tool": "android/tools/run_game.py", "serial": args.serial,
                "started_utc": t0.isoformat(), "holder": args.holder,
                "lock": {"pid": os.getpid(), "dir": str(lock / args.serial)}}
    state0, orig_tmp, launched = None, None, False
    data_backup, backup_dir, downgrade_back = None, None, False
    props_prev = {}
    logcat_proc, log_file = None, None
    sampler, seen_boots, power_f = None, set(), None
    result, reason = "red", "未跑完"

    def log(line):
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        text = f"{stamp} {line}"
        print(text)
        if log_path.parent.is_dir():
            with log_path.open("a", encoding="utf-8") as f:
                f.write(text + "\n")

    try:
        if out.exists():
            raise Refuse(f"{out} 已存在；每局一个全新目录")
        out.mkdir(parents=True)
        (out / "shots_ap").mkdir()
        if d.run("get-state").strip() != "device":
            raise Refuse(f"adb 设备 {args.serial} 不在线")

        pkg = args.package
        if not pkg:
            doc = json.loads((Path(args.resources or args.pack) / "game-build-artifact.json").read_text())
            pkg = doc["apk"]["packageName"]
        our_sha = sha256(args.apk)
        manifest.update(package=pkg, apk_sha256=our_sha, mode=args.mode, extras=args.extra,
                        allow_data_loss=bool(args.allow_data_loss))

        # ---- 记录原安装状态；保证不了还原的拒跑 ----
        state0 = install_state(d, pkg)
        manifest["prior_install"] = state0
        if state0.get("split"):
            raise Refuse(f"{pkg} 是分包安装（{state0['paths']}），无法忠实还原；不动这台设备")
        backup_dir = Path(args.backup_dir) if args.backup_dir else out / "data-backup"
        if state0["installed"] and not args.allow_data_loss and not state0["debuggable"]:
            raise Refuse(f"{pkg} 设备上已安装但不是 debuggable：run-as 备份私有数据做不了，"
                         f"局内私有数据与已并入资源会被改写且 adb 层无法还原；"
                         f"先手动卸了它再跑，或显式 --allow-data-loss（只还原 APK 字节）")
        # ---- versionCode / 降级：装新包与装回原包两边都可能是 downgrade，任一要向 -d 而 ----
        # ---- 设备不支持就连装都不装（拒跑而不是装上去装不回来，manifest 记决策依据）----
        downgrade_fwd = downgrade_back = False
        if state0["installed"] and state0["sha256"] != our_sha:
            import game_apk  # 延迟导入：只在这条路径要读新包 versionCode
            apk_pkg, new_vc, _dbg = game_apk.apk_identity(args.apk)
            if apk_pkg != pkg:
                raise Refuse(f"--apk 的包名 {apk_pkg} 与 --package {pkg} 不符，装错了会丢数据")
            orig_vc = state0.get("versionCode")
            manifest["versions"] = {"orig_versionCode": orig_vc, "new_versionCode": new_vc}
            if orig_vc is None:
                raise Refuse("原包 versionCode 读不到：装回方向判不了降级，不动这台设备")
            downgrade_fwd = orig_vc > new_vc   # 装低 versionCode 测试包
            downgrade_back = orig_vc < new_vc  # 收局装回低 versionCode 原包
            if (downgrade_fwd or downgrade_back) and not device_allows_downgrade(d):
                raise Refuse(f"versionCode 原 {orig_vc} vs 新 {new_vc}：装回方向要降级但设备 pm 不支持 "
                             f"install -d，装了原包装不回来；不动这台设备")
            orig_tmp = Path(tempfile.mkdtemp(prefix="s2a-orig-")) / "orig.apk"
            d.run("pull", state0["base"], str(orig_tmp), timeout=300)
            if sha256(orig_tmp) != state0["sha256"]:
                raise Refuse("备份原包 sha256 与设备上读到的不符，放弃起局")
            log(f"已备份原包 sha256={state0['sha256'][:12]}"
                + ("；还原装回要走 install -d 降级" if downgrade_back else ""))
        else:
            log(f"原安装状态：{'未安装' if not state0['installed'] else '与本包同 sha256'}")
        # ---- debuggable 原包：run-as tar 全量备份私有/外部数据（装新包之前；任一目录失败即拒）----
        data_backup = None
        if state0["installed"] and state0["debuggable"] and not args.allow_data_loss:
            data_backup = backup_data(d, pkg, backup_dir)
            nfiles = sum(len(v["files"]) for v in data_backup["dirs"].values())
            manifest["data_backup"] = {"dir": str(backup_dir), "dirs": len(data_backup["dirs"]),
                                       "files": nfiles,
                                       "tars": {k: v["tar_sha256"] for k, v in data_backup["dirs"].items()}}
            log(f"已备份原包数据 {len(data_backup['dirs'])} 处目录 / {nfiles} 文件 → {backup_dir}")
        manifest["restore_scope"] = {
            "apk": ("已装不同构建→备份后 install -r" + (" -d" if downgrade_back else "")
                    + " 装回并 sha256 复核（分包拒跑）") if state0["installed"]
                   else "未安装→收局卸载并复核 pm path 已清",
            "private_data": ("debuggable→run-as tar 备份至 data-backup/，收局 pm clear+回灌+逐文件 sha 核"
                             if data_backup is not None
                             else "不还原（--allow-data-loss 已声明接受数据丢失）" if state0["installed"]
                                  else "卸载随 /data/data 清"),
            "resources": "落在 /sdcard/Android/data|obb/<pkg>，卸载随删并逐项验不存在"
                        if not state0["installed"]
                        else "随外部数据备份还原（extdata/obb 目录在清单内）" if data_backup is not None
                             else "不还原（已并入原应用数据）",
        }

        # ---- 安装（资源走 deploy_game.deploy_resources，不启动）----
        if args.pack or args.resources:
            import deploy_game, game_apk  # 延迟导入：纯 apk 路径不碰 aapt2 / 资源流水线
            deploy_work = out / "deploy"
            deploy_work.mkdir()
            adb = deploy_game.Adb(args.serial, args.adb)
            if args.pack:
                tmp = Path(tempfile.mkdtemp(prefix="s2a-pack-"))
                frozen = tmp / "game.apk"
                frozen.write_bytes(args.apk.read_bytes())
                res = tmp / "resources"
                game_apk.resources(str(args.pack), str(frozen), str(res))
            else:
                frozen, res = args.apk, Path(args.resources)
            if downgrade_fwd:  # deploy_resources 内部装不了降级：先自己把包降上去
                d.run("install", "-r", "-d", str(frozen), timeout=600)
                log("已降级安装测试包（install -r -d）")
            deploy_game.deploy_resources(str(frozen), str(res), adb, str(deploy_work),
                                         mode="auto", timeout=300, start=False,
                                         push_timeout=args.push_timeout,
                                         push_tar={"auto": None, "always": True,
                                                   "never": False}[args.push_tar],
                                         log=log)
            log("资源与包已部署（deploy_resources，未启动）")
        elif not state0["installed"] or state0["sha256"] != our_sha:
            d.run("install", "-r", *(["-d"] if downgrade_fwd else []), str(args.apk), timeout=600)
            log("已安装测试包" + ("（install -d 降级）" if downgrade_fwd else ""))
        cur = install_state(d, pkg)
        if not cur["installed"] or cur.get("sha256") != our_sha:
            raise Refuse("安装后 sha256 校验不过")

        # ---- 调试门 + 帧日志开关（记原值，收局恢复并逐项读回；release 机不让改只记注）----
        for key in DEBUG_PROPS:
            props_prev[key] = d.shell(f"getprop {key}", allowed=(0, 1)).strip()
            d.shell(f"setprop {key} 1", allowed=(0, 1))
        if any(d.shell(f"getprop {k}", allowed=(0, 1)).strip() != "1" for k in DEBUG_PROPS):
            notes.append("debug.s2a.* 没置上（release 设备/构建不让改调试属性）：帧日志不会有，frames.tsv 只有表头")
            log("debug.s2a.* 没置上，帧日志不会有")
        else:
            log("debug.s2a.debug_env / debug.s2a.frame_log 已置 1（收局恢复）")

        # ---- 冷启动 ----
        d.shell(f"am force-stop {pkg}", allowed=(0, 1))
        since = d.shell("date '+%m-%d %H:%M:%S.000'").strip()
        log_file = (out / "logcat.txt").open("wb")
        logcat_proc = subprocess.Popen([*adb_argv, "logcat", "-v", "threadtime", "-T", since],
                                       stdout=log_file, stderr=subprocess.DEVNULL)
        launch = ["am", "start", "-W", "-n", f"{pkg}/{ACTIVITY}"]
        if args.mode != "auto":
            launch += ["--es", "s2arm.graphics_mode", args.mode]
        for e in args.extra:
            name, _, value = e.partition("=")
            launch += (["--ez", name, value or "true"] if value in ("", "true", "false")
                       else ["--es", name, value])
        launch_str = " ".join(shlex.quote(a) for a in launch)
        started = d.shell(launch_str, timeout=120)
        if re.search(r"Error|Exception", started):  # am start 组件不存在也 rc=0，只看退出码会漏
            raise Refuse(f"am start 报错：{started.strip()[:200]}")
        launched = True
        log(f"冷启动：{launch_str}")

        # ---- 等 READY ----
        deadline = time.monotonic() + args.ready_timeout
        ready, seen_running = False, False
        while time.monotonic() < deadline:
            text = d.shell(f"logcat -d -v brief -T {shlex.quote(since)} -s S2Arm:I")
            if READY in text:
                ready = True
                break
            fail = FAIL_RE.search(text)
            if fail:
                raise Refuse(f"启动被拒：{fail.group(0)}")
            running = bool(d.shell(f"pidof {pkg}", allowed=(0, 1)).strip())
            seen_running = seen_running or running
            if seen_running and not running:
                raise Refuse("READY 前进程已退出")
            time.sleep(0.5)
        if not ready:
            raise Refuse(f"{args.ready_timeout}s 内没见到「{READY}」")
        log("READY")

        # ---- 当局绑定：游戏进程（pkg / pkg:*）+ 出生证，帧日志块只收这些 PID 的行 ----
        pids = session_pids(d, pkg)
        manifest["session"] = {"pids": {str(pid): {"name": name, "birth_ticks": pid_birth(d, pid)}
                                        for pid, name in pids.items()},
                               "ready_line": READY}
        if not pids:
            notes.append("ps 里没找到当局游戏进程，帧日志无法按 PID 绑定（帧表只留表头）")

        # ---- 按时长采集：窗口边界取设备 /proc/uptime（与帧记录同一 CLOCK_BOOTTIME）----
        # 电池采样走设备侧守护：1Hz 写 /data/local/tmp 私有文件（=== 分块），host 按
        # --power-interval 拉回——adb 中途掉线只断拉取不断设备上的采样，收局/中断照样
        # 把已落的行拉回来（兄弟项目「私存不丢」同语义）。
        supply = "battery"
        dest, sampler_pid = start_power_sampler(d, supply)
        sampler = (dest, sampler_pid)
        manifest["power_sampler"] = {"dest": dest, "pid": sampler_pid,
                                     "settle_required_s": args.discharge_settle}
        power_f = (out / "power.tsv").open("w")
        power_f.write("t\t" + "\t".join(android_power.FIELDS) + "\tlevel\n")

        def flush_power(rows):
            for row, level in rows:
                boot = row.get("boot_us")
                if boot is None or boot in seen_boots:
                    continue
                seen_boots.add(boot)
                power_f.write("\t".join([str(len(seen_boots) - 1)]
                                        + [str(row[k]) for k in android_power.FIELDS]
                                        + [level]) + "\n")
            power_f.flush()

        def pull_rows(tag):
            try:
                rows = power_rows(d, dest)
            except (RuntimeError, subprocess.SubprocessError) as error:
                notes.append(f"{tag} 拉取设备采样文件失败（行留在设备上，重连后可拉）：{error}")
                return []
            flush_power(rows)
            return [row for row, _ in rows]

        # 放电稳定门（兄弟项目口径）：尾部一串「连续物理放电」采样满
        # --discharge-settle 秒才进采集窗；上限 --discharge-timeout（0=不限）。
        # settle=0 就是「不等」：不做任何预拉取，窗口边界直接从 col0 起。
        settle_us = args.discharge_settle * 1_000_000
        wait0 = time.monotonic()
        settled = True
        if settle_us > 0:
            rows = pull_rows("放电稳定门")
            settled = bool(rows) and android_power.discharge_streak_us(rows) >= settle_us
            while not settled:
                if args.discharge_timeout and time.monotonic() - wait0 > args.discharge_timeout:
                    notes.append(f"放电未稳满 {args.discharge_settle}s 就开采（{args.discharge_timeout}s 内没等到，能耗按没数据处理）")
                    break
                time.sleep(args.power_interval)
                rows = pull_rows("放电稳定门")
                settled = bool(rows) and android_power.discharge_streak_us(rows) >= settle_us
        manifest["power_sampler"]["settle_ok"] = settled
        manifest["power_sampler"]["waited_s"] = round(time.monotonic() - wait0, 1)

        col0_us = uptime_us(d)
        shot(adb_argv, out / "shots_ap" / "01_000s.png")
        for i in range(args.seconds):
            tick = time.monotonic()
            pull_rows(f"第 {i}s")
            time.sleep(max(0, args.power_interval - (time.monotonic() - tick)))
        col1_us = uptime_us(d)
        try:
            rows = stop_power_sampler(d, dest, sampler_pid)
            sampler = None
            flush_power(rows)
        except (RuntimeError, subprocess.SubprocessError) as error:
            notes.append(f"收采样守护失败（杀/拉/清理）：{error}")
        power_f.close()
        power_f = None
        shot(adb_argv, out / "shots_ap" / f"02_{args.seconds:03d}s.png")
        manifest["frame_window_us"] = [col0_us, col1_us]
        log(f"采集 {args.seconds}s 完成（窗口 boot_us {col0_us}..{col1_us}）")

        # ---- 收局 ----
        # 优雅收局触发适配器 shutdown 帧日志转储（meta→分片→end+sha）：先等
        # end 落进 logcat（有上限）再验死/强杀——8E 二局 meta 声明 225 片只收到
        # 1–201（:game 在转储写完前就死了）。等不到也照常停，截断如实报。
        manifest["stop"] = dev_runtime.stop(d, pkg,
                                           wait_done=lambda: frame_dump_done(d, pkg, since))
        manifest["stop_verified"] = verify_stopped(d, pkg)
        if not manifest["stop_verified"]:
            notes.append("收局后游戏进程仍存活（收局确认未过，按还原失败路径处理）")
        # 帧日志在适配器关闭时转储：等它落进 logcat（正式构建没这步，有界等待即可）
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if FRAMELOG_END in d.shell(f"logcat -d -v brief -T {shlex.quote(since)} -s S2Arm:I"):
                break
            time.sleep(0.5)

        # ---- 归置产物 ----
        if logcat_proc:
            logcat_proc.terminate()
            try:
                logcat_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                logcat_proc.kill()
        if log_file:
            log_file.close()
        logcat_proc, log_file = None, None
        logcat = (out / "logcat.txt").read_text(errors="replace")
        dump = subprocess.run([*adb_argv, "logcat", "-d", "-v", "threadtime", "-T", since],
                              capture_output=True, timeout=120).stdout.decode(errors="replace")
        (out / "logcat_d.txt").write_text(dump)

        direct = "direct provider prepared" in (logcat + dump)
        caps, seen = [], set()
        if pids:
            for text in (logcat, dump):
                for block in frame_log_blocks(text, pids=pids):
                    try:
                        for cap in direct_framelog.parse(block):
                            key = (cap["meta"]["first"], cap["meta"]["recorded"], len(cap["ts"]))
                            if key not in seen:
                                seen.add(key)
                                caps.append(cap)
                    except direct_framelog.FrameLogError as error:
                        notes.append(f"帧日志块截断/不完整被弃（整段未证）：{str(error)[:500]}")
        if not direct:
            notes.append("logcat 没有「direct provider prepared」：非直连 provider（SurfaceFlinger 路），"
                         "帧表不写帧（direct present 与 SF 时序不混算）")
            caps = []
        manifest["frame_log"] = write_frames(out, caps, notes, logcat,
                                             window_us=(col0_us, col1_us))
        if direct:
            (out / "gfx_mode.txt").write_text("直连（依据 logcat「direct provider prepared」）\n")
        elif "Gpu LoadShaders" in (logcat + dump):
            (out / "gfx_mode.txt").write_text("通用（依据 logcat「Gpu LoadShaders」）\n")
        else:
            (out / "gfx_mode.txt").write_text("\n")
            notes.append("logcat 没认到直连 / 通用特征行")
        (out / "battery.txt").write_text(d.shell("dumpsys battery"))
        (out / "thermal.txt").write_text(d.shell("dumpsys thermalservice"))
        (out / "android.json").write_text(json.dumps({
            "schema": 1, "package": pkg, "serial_model": d.shell("getprop ro.product.model").strip(),
            "soc": d.shell("getprop ro.soc.model").strip() or d.shell("getprop ro.hardware").strip(),
            "android": d.shell("getprop ro.build.version.release").strip(),
            "launch": launch_str, "seconds": args.seconds, "mark": 0,
            "mark_note": "安卓没有自动巡线 MARK；整窗计开局",
            "started_utc": t0.isoformat(), "ended_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "apk_sha256": our_sha, "graphics_mode": args.mode, "extras": args.extra,
            "frame_log": manifest["frame_log"], "session": manifest["session"],
            "frame_window_us": manifest["frame_window_us"], "ready": ready,
            "restore_scope": manifest["restore_scope"],
        }, ensure_ascii=False, indent=1) + "\n")
        if not manifest["stop_verified"]:
            raise Refuse("收局后游戏进程仍存活（收局确认未过）")
        result, reason = "done", ""
    except (KeyboardInterrupt, Abort) as error:
        reason = f"被中止：{error}" if isinstance(error, Abort) else "被中断"
        log(f"【红】{reason}，照常还原")
    except (Refuse, RuntimeError, subprocess.SubprocessError, OSError, ValueError,
            json.JSONDecodeError, Exception) as error:
        reason = str(error)
        log(f"【红】{reason}")
    finally:
        # ---- 还原（任何结果都跑：SIGTERM / 未捕获异常也走到）----
        fails = []
        pkg = manifest.get("package", args.package or "")
        if sampler:
            try:  # 中断路径同样收采样守护：杀循环 → 拉回设备上已落的行 → 清理文件
                for row, level in stop_power_sampler(d, sampler[0], sampler[1]):
                    boot = row.get("boot_us")
                    if boot is None or boot in seen_boots or power_f is None:
                        continue
                    seen_boots.add(boot)
                    power_f.write("\t".join([str(len(seen_boots) - 1)]
                                            + [str(row[k]) for k in android_power.FIELDS]
                                            + [level]) + "\n")
            except (RuntimeError, subprocess.SubprocessError, OSError) as error:
                fails.append(f"收电池采样守护异常：{error}")
        if power_f is not None:
            power_f.close()
        if launched:
            try:  # 中断路径同样先收局（优雅 s2arm.finish → force-stop 兜底），再验死
                dev_runtime.stop(d, pkg, wait_done=lambda: frame_dump_done(d, pkg, since))
            except (RuntimeError, subprocess.SubprocessError) as error:
                fails.append(f"收局 adb 异常：{error}")
            try:
                if not verify_stopped(d, pkg):
                    fails.append("游戏进程在收局后仍存活")
            except (RuntimeError, subprocess.SubprocessError) as error:
                fails.append(f"收局确认 adb 异常：{error}")
        try:
            for key, prev in props_prev.items():
                d.shell(f"setprop {key} {shlex.quote(prev)}", allowed=(0, 1))
                back = d.shell(f"getprop {key}", allowed=(0, 1)).strip()
                if back != prev:
                    fails.append(f"调试属性 {key} 恢复失败：期望 {prev!r} 实得 {back!r}")
        except (RuntimeError, subprocess.SubprocessError) as error:
            fails.append(f"调试属性恢复 adb 异常：{error}")
        if state0 is not None:
            try:
                fails += restore_state(d, manifest.get("package", args.package or ""), state0,
                                       orig_tmp, downgrade=downgrade_back,
                                       data_backup=data_backup, backup_dir=backup_dir)
            except (RuntimeError, subprocess.SubprocessError) as error:
                fails.append(f"还原过程 adb 异常：{error}")
        if logcat_proc:
            logcat_proc.terminate()
            try:
                logcat_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                logcat_proc.kill()
            if logcat_proc.poll() is None:
                fails.append("logcat 采集进程没能停掉")
        if log_file:
            log_file.close()
        if fails:
            reason = (reason + "；" if reason and reason != "未跑完" else "") + "；".join(fails)
            device_lock.detain(lock, args.serial, reason, holder=args.holder, out=str(out))
            log(f"【红】还原失败，设备已留置：{reason}")
            result = "red"

    manifest.update(result=result, reason=reason, notes=notes,
                    ended_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    if out.is_dir():
        (out / "run-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
    device_lock.release(lock, args.serial, token=token)
    print(f"RESULT={result} out={out}" + (f" reason={reason}" if reason else ""))
    return 0 if result == "done" else (3 if (lock / args.serial / "detained.json").exists() else 2)


if __name__ == "__main__":
    sys.exit(main())
