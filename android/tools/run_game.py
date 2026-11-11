#!/usr/bin/env python3
"""Android 真机最小闭环：装包 → 冷启动 → 按时长采集 → 收局 → 还原安装状态，
产出 tools/quality_check.py 安卓分支直接判读的局目录（android.json / shots_ap / frames.tsv /
power.tsv / logcat.txt / gfx_mode.txt / battery.txt / thermal.txt / frame_log.txt；
另有 logcat_d.txt（收局整窗 dump）与 run-manifest.json（原状 / 还原范围 / 失败原因 / 注记））。

    run_game.py --apk game.apk --package org.s2arm.game.t<TITLE> --serial S --out DIR --seconds 60
                [--resources DIR | --pack DIR]     # 资源：走 deploy_game.deploy_resources（不启动）
                [--mode auto|generic|direct]      # s2arm.graphics_mode 冷启动 intent extra；
                                                  # 起局后按本局 PID 的「graphics selection:」行
                                                  # 断言实际路径——缺行/解析不出/与 --mode 不符都
                                                  # RESULT=red，auto 只把实际值记进 manifest
                [--extra s2arm.KEY[=VALUE] ...]   # 其他 intent extra（true/false 走 --ez）
                [--adb "adb"] [--ready-timeout 180] [--wait 0] [--allow-data-loss]
                [--backup-dir DIR] [--lock-root DIR] [--holder NAME] [--note TXT]
                [--push-timeout S] [--push-tar auto|always|never]

锁：取 device_lock.py 的持久内核锁后才动设备，收局按持锁令牌 release；
崩溃 / 强杀 / adb 掉线 = 未正常收局，设备留置不放行（不自动放）。

还原范围（保证不了的一律拒跑，不返回成功兜底）：
- 原 APK：设备已装该包且构建不同 → adb pull 备份到 <out>/orig-backup、收局 install -r
  装回并 sha256 复核（还原全过备份即清；还原未全过留下供人工救，manifest 记路径）；
  分包（split APK）装回不可靠 → 入局即拒。versionCode 双向裁决：装测试包方向的
  降级（原包 > 新包）由 `install -r -d` 实测（失败即拒，装失败不卸旧包）；装回方向
  的降级（原包 < 新包）入局前证不了能装回 → 连拉/装都不做，直接拒跑（qa-fone：
  先升级入局、收局再试降级 = 装了赌能降回去）。
- 应用私有数据 / 已并入资源：已装包是 debuggable → 装新包前 tar 流式拉回
  /data/data/<pkg>、/sdcard/Android/data|obb/<pkg> 到 --backup-dir（默认 <out>/data-backup，
  tar + manifest.json 全量清单：文件 sha/软链目标/目录含空目录/权限位）。访问通道
  按目录归属：私有树 run-as（应用 uid 才够权），外部目录 shell（9000 实测 run-as
  碰 /sdcard/Android/* 被 SELinux 拒）；流式分块落盘边算 sha，不整进宿主内存
  （9000 私有树 ~7.3GB，capture_output 会爆）。归档先解包与源清单逐项核对，
  对不上拒跑；收局装回原包后归档复核过才 pm clear、回灌、设备端全量复核。
  存在但进不去的目录报错拒跑，不当不存在。
  已装包不是 debuggable → 拒跑；显式 --allow-data-loss 才放行且只还原 APK 字节
  （私有数据、已并入资源不还原，manifest 写明）。
- 导入的资源 / 卸载应清的目录：未安装路径落在 /sdcard/Android/data|obb/<pkg>，卸载随删，
  收局逐项验不存在；debuggable 已装路径随数据备份还原。
- 调试属性 debug.s2a.*：起局前置 1，收局恢复原值并逐项读回核对（release 机不让改 → 只记注）。

收局：任何结果（含 SIGTERM / SIGHUP / Ctrl+C / 未捕获异常）都走 finally 还原；
先确认游戏进程与采集进程都停了，再逐项验还原。还原失败 → RESULT=red + 留置标。
安全边界：只经 adb 做 安装/启动/采集/截图——不重启、不刷机、不 root、不改系统设置。
帧表：只写采集窗口 [起, 止) 内的 direct-present 帧——帧记录是 CLOCK_BOOTTIME，窗口边界取设备
/proc/uptime 同一时钟；帧日志块先按 logcat 行 PID 圈当局进程，再按 meta uuid= 硬绑本局
（运行器起局前 setprop debug.s2a.session_uuid，缺/不符的块不收），manifest.frame_log.binding 记账。
私有证据：收局后、还原清私有目录前 run-as 拉回 host-*-<pid>-*.log 与原生直写
frame-log-<pid>.log（与 logcat 同文双写，logcat 分片被截断时它是完整源）→ out/host_logs/。
退出码：0 完成且还原成功；2 参数 / 设备 / 起局 / 中断（设备已尽量还原）；3 还原失败（已留置）。
"""
import argparse, datetime, hashlib, json, os, posixpath, re, selectors, shlex, shutil, signal, subprocess, sys, tarfile, tempfile, time, uuid
from pathlib import Path

ANDROID = Path(__file__).resolve().parents[1]
REPO = ANDROID.parent
for _p in (ANDROID, ANDROID / "tools", REPO / "tools"):
    sys.path.insert(0, str(_p))
import device_lock            # noqa: E402  本目录设备锁
import dev_runtime            # noqa: E402  Device/stop 复用
import direct_framelog        # noqa: E402  帧日志解析与百分位

ACTIVITY = dev_runtime.ACTIVITY
DEBUG_PROPS = ("debug.s2a.debug_env", "debug.s2a.frame_log")
# 本局身份：每局一个 uuid，起局前 setprop 给游戏——os.c 会话日志首行与
# nvn_vk_frame_log meta 行都写它，取证按文件内容绑本局（不拿 mtime 判）。
SESSION_PROP = "debug.s2a.session_uuid"
READY = "Game startup accepted:"
FAIL_RE = re.compile(r"Game startup refused|startup rejected|FATAL EXCEPTION|AndroidRuntime: FATAL")
FRAMELOG_END = "direct frame-log end:"
PKG_RE = re.compile(r"[A-Za-z0-9_.]+")
THREADTIME_PID = re.compile(r"^\d\d-\d\d\s+\d\d:\d\d:\d\d\.\d+\s+(\d+)\s+\d+\s+\w")


# ---- 真机功耗：设备侧采样协议与判读 -------------------------------------
# #163 把 tools/android_power.py 换成 host 逐点采样后，运行器自有的「设备侧
# 循环采样 + === 分块日志 + 放电门判读」协议归入本文件（quality_check.py
# 的能耗判读同源引用）。判读口径照搬 switch-gpu-mapper 的
# docs/power-comparison.md + scripts/analyze_power.py：
# - 电流取幅值积分（abs(current)×voltage 梯形），原始符号留在 rawCurrentSign——
#   部分 OEM 放电为正；窗口内符号翻转即作废。
# - 分析窗对每块剔首 15s / 尾 5s（trim_start_us / trim_end_us）。
# - 窗内每个采样点都必须物理放电（plugged==0 且 status=Discharging）、屏幕交互、
#   非省电、thermal_status<3、电流/电压/电量计读数合法、采样间隔 ≤3s、
#   窗口边界有覆盖——充电 / 掉线 / 降频即整块作废（fail closed 无数据）。
# - charge counter 整局交叉核对：等差递减量子 q → 端点差 ±q 区间；细粒计数走
#   0.5–1.5 比值门；向下跳变 <2 次 → 不证。核对不过 = 功耗判读未证，不是绝对标定。

POWER_FIELDS = ("boot_us", "status", "current_ua", "voltage_uv", "energy_uwh", "charge_uah",
                "temp_tenths_c", "source", "ac_powered", "usb_powered", "wireless_powered",
                "dock_powered", "plugged", "interactive", "power_save", "thermal_status")
POWER_SYSFS = {"current_now": "current_ua", "voltage_now": "voltage_uv", "energy_now": "energy_uwh",
               "charge_counter": "charge_uah", "temp": "temp_tenths_c", "status": "status"}
POWERED_FLAGS = ("ac_powered", "usb_powered", "wireless_powered", "dock_powered")
POWER_BLOCK_SEP = "==="


def power_parse_sample(text):
    raw = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    row = {key: "" for key in POWER_FIELDS}
    for source, key in {"boot_us": "boot_us", **POWER_SYSFS}.items():
        value = raw.get(source, "").strip()
        if key == "status":
            row[key] = value if value in ("Charging", "Discharging", "Full", "Not charging", "Unknown") else ""
        elif re.fullmatch(r"-?[0-9]+", value):
            row[key] = int(value)
    row["source"] = "sysfs"
    # 屏幕交互 / 省电 / 热档（dumpsys power / thermalservice）：整块作废门的证据。
    for key in ("interactive", "power_save"):
        value = raw.get(key, "").strip()
        row[key] = "1" if value == "true" else "0" if value == "false" else ""
    value = raw.get("thermal_status", "").strip()
    row["thermal_status"] = int(value) if re.fullmatch(r"[0-9]+", value) else ""
    # 每次采样都带 dumpsys 旗标段：sysfs 行也只有拿到四路 powered 旗标，
    # power_battery_window 才能证「未接电」（旗标缺失/未知 fail closed）。
    # UPDATES STOPPED 是模拟置/拔电的冻结态：旗标不可证，留空。
    dump = text.partition("service_dump_begin\n")[2].partition("service_dump_end")[0]
    fields = dict(re.findall(r"^[ \t]*([A-Za-z ]+):[ \t]*([^\r\n]+)$", dump, re.M))
    if "UPDATES STOPPED" not in dump:
        for source, key in (("AC powered", "ac_powered"), ("USB powered", "usb_powered"),
                            ("Wireless powered", "wireless_powered"), ("Dock powered", "dock_powered")):
            value = fields.get(source, "").strip()
            row[key] = value if value in ("true", "false") else ""
    if row["current_ua"] == "" or row["voltage_uv"] == "":
        # BatteryService current_now is uA; dumpsys battery voltage is mV.
        # Parse only exact field lines, never the historical broadcast voltage.
        current = text.partition("service_current_now=")[2].partition("\nservice_dump_begin")[0].strip()
        voltage = fields.get("voltage", "").strip()
        if re.fullmatch(r"-?[0-9]+", current) and re.fullmatch(r"[0-9]+", voltage):
            row["current_ua"], row["voltage_uv"] = int(current), int(voltage) * 1000
            row["source"] = "battery-service"
            # BatteryManager status 码照实映射、供电旗标留原列，不坍缩（8E 首局
            # status=4 NOT_CHARGING + AC powered=true 曾被映成 Charging 丢真值）；
            # 状态与旗标矛盾时两边都留原文，接没接电由判读者看旗标列，这里不猜。
            row["status"] = {"2": "Charging", "3": "Discharging", "4": "Not charging",
                             "5": "Full"}.get(fields.get("status", "").strip(), "Unknown")
            temp = fields.get("temperature", "").strip()
            if re.fullmatch(r"-?[0-9]+", temp): row["temp_tenths_c"] = int(temp)
            # AOSP BatteryService.dumpInternal marks set/unplug simulation with
            # UPDATES STOPPED. Keep the numbers as raw evidence, never integrate
            # that frozen service state as a measured discharge sample.
            if "UPDATES STOPPED" in dump:
                row["status"] = "Unknown"
                row["source"] = "battery-service-frozen"
        else:
            row["source"] = "unavailable"
    flags = [row[key] for key in POWERED_FLAGS]
    row["plugged"] = ("1" if "true" in flags else
                      "0" if all(value == "false" for value in flags) else "")
    return row


def power_sample_command(supply):
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", supply):
        raise ValueError("非法 power_supply 名称")
    return ("good=1; for n in status current_now voltage_now energy_now charge_counter temp; do "
            "v=$(cat /sys/class/power_supply/" + supply + "/\"$n\" 2>/dev/null); "
            "printf '%s=%s\\n' \"$n\" \"$v\"; "
            "if [ \"$n\" = current_now ] || [ \"$n\" = voltage_now ]; then "
            "case \"$v\" in ''|*[!0-9-]*) good=0;; esac; fi; done; "
            "if [ \"$good\" = 0 ]; then printf 'service_current_now='; "
            "cmd battery get -f current_now 2>/dev/null; printf '\\n'; fi; "
            "printf 'service_dump_begin\\n'; dumpsys battery 2>/dev/null; printf '\\nservice_dump_end\\n'; "
            "dp=$(dumpsys power 2>/dev/null); "
            "printf 'interactive=%s\\n' \"$(printf '%s' \"$dp\" | sed -n 's/.*mInteractive=\\([a-z]*\\).*/\\1/p' | head -1)\"; "
            "printf 'power_save=%s\\n' \"$(printf '%s' \"$dp\" | sed -n 's/.*mPowerSaveMode=\\([a-z]*\\).*/\\1/p' | head -1)\"; "
            "printf 'thermal_status=%s\\n' \"$(dumpsys thermalservice 2>/dev/null | "
            "sed -n 's/.*Current Status:[ ]*\\([0-9]*\\).*/\\1/p' | head -1)\"; "
            "printf 'boot_us='; awk '{printf \"%.0f\\n\", $1*1000000}' /proc/uptime")


def _power_num(row, key):
    try:
        return int(str(row.get(key, "")).strip())
    except (TypeError, ValueError):
        return None


def power_discharging(row):
    """物理放电证据行：BatteryService 放电状态 + 四路供电旗标全证 false（plugged==0）。"""
    return row.get("status") == "Discharging" and str(row.get("plugged")) == "0"


def discharge_streak_us(rows, end_us=None):
    """截至 end_us（缺省=最后一条观测）尾部那一串「连续物理放电」采样覆盖的时长。
    尾部末行须在 end_us 的 3s 内且行间隔 ≤3s、行序单调；断链即只计尾段。"""
    obs = sorted((_power_num(r, "boot_us"), r) for r in rows if _power_num(r, "boot_us") is not None)
    if not obs:
        return 0
    if end_us is None:
        end_us = obs[-1][0]
    pre = [(b, r) for b, r in obs if b <= end_us]
    if not pre or end_us - pre[-1][0] > 3_000_000 or not power_discharging(pre[-1][1]):
        return 0
    span, prev = 0, pre[-1]
    for b, r in reversed(pre[:-1]):
        if not power_discharging(r) or prev[0] - b > 3_000_000:
            break
        span += prev[0] - b
        prev = (b, r)
    return span


def _power_slope(points):
    """最小二乘斜率（x 单位 µs → 每 µs 值变化）。"""
    xs = [x for x, _ in points]
    ys = [y for _, y in points]
    xm, ym = sum(xs) / len(xs), sum(ys) / len(ys)
    denom = sum((x - xm) ** 2 for x in xs)
    return sum((x - xm) * (y - ym) for x, y in points) / denom if denom else 0.0


def power_battery_window(rows, begin_us, end_us, check_charge=True,
                         trim_start_us=15_000_000, trim_end_us=5_000_000):
    """电池侧梯形积分：分析窗 = [begin+trim_start, end-trim_end]（每块剔首 15s/尾 5s）。
    返回 dict：averageW/energyJ/durationSeconds + rawCurrentSign + chargeCounterW/
    chargeToCurrentRatio + samples + tempRangeC + reason；任一作废门不过 → averageW=None。"""
    result = {"scope": "battery", "comparableToIosWholeSystem": False, "averageW": None,
              "rawCurrentSign": None, "reason": ""}

    def fail(reason):
        result["reason"] = reason
        return result

    if begin_us is None or end_us is None:
        return fail("缺少窗口边界时钟")
    begin, end = begin_us + trim_start_us, end_us - trim_end_us
    if end <= begin:
        return fail("剔首尾后分析窗为空（块太短）")
    obs = [{"boot": _power_num(r, "boot_us"), "current": _power_num(r, "current_ua"),
            "voltage": _power_num(r, "voltage_uv"), "charge": _power_num(r, "charge_uah"),
            "status": r.get("status"), "plugged": str(r.get("plugged") or ""),
            "interactive": str(r.get("interactive") or ""), "power_save": str(r.get("power_save") or ""),
            "thermal": _power_num(r, "thermal_status"), "temp": _power_num(r, "temp_tenths_c"),
            "raw": r} for r in rows]
    if len(obs) < 2 or any(o["boot"] is None for o in obs):
        return fail("boot_us 缺行/不可读")
    if any(b["boot"] <= a["boot"] for a, b in zip(obs, obs[1:])):
        return fail("电池采样时间重复或倒退")
    pairs = [(a, b) for a, b in zip(obs, obs[1:]) if a["boot"] < end and b["boot"] > begin]
    if not pairs or pairs[0][0]["boot"] > begin or pairs[-1][1]["boot"] < end:
        return fail("窗口边界无采样覆盖")
    points = [pairs[0][0]] + [b for a, b in pairs]
    signs = set()
    for o in points:
        if not (o["plugged"] == "0" and o["status"] == "Discharging"):
            return fail("窗口内存在接电/非放电行（plugged 或 status 证不出放电，整块作废）")
        if o["interactive"] != "1" or o["power_save"] != "0":
            return fail("屏幕交互/省电态在窗口内变化或读不出（整块作废）")
        if o["thermal"] is None or o["thermal"] >= 3:
            return fail("热档严重降频或读不出（thermal_status≥3，整块作废）")
        if o["current"] is None or not 0 < abs(o["current"]) <= 30_000_000:
            return fail("电流读数缺失或为 0 或越界（整块作废）")
        if o["voltage"] is None or not 2_500_000 <= o["voltage"] <= 10_000_000:
            return fail("电压读数缺失或越界 2.5–10V（整块作废）")
        if o["charge"] is None or o["charge"] <= 0:
            return fail("电量计读数缺失（charge_counter≤0，整块作废）")
        signs.add(1 if o["current"] > 0 else -1)
    if len(signs) != 1:
        return fail("窗口内电流符号翻转（放电方向不自洽，整块作废）")
    energy_ws, covered = 0.0, 0
    for a, b in pairs:
        if b["boot"] - a["boot"] > 3_000_000:
            return fail("超过3秒采样空洞（掉线，整块作废）")
        lo, hi = max(begin, a["boot"]), min(end, b["boot"])
        wa, wb = abs(a["current"]) * a["voltage"] / 1e12, abs(b["current"]) * b["voltage"] / 1e12
        wl = wa + (wb - wa) * (lo - a["boot"]) / (b["boot"] - a["boot"])
        wr = wa + (wb - wa) * (hi - a["boot"]) / (b["boot"] - a["boot"])
        energy_ws += (wl + wr) / 2 * (hi - lo) / 1e6
        covered += hi - lo
    if covered != end - begin:
        return fail("采样未覆盖完整分析窗")
    watts = energy_ws / (covered / 1e6)
    charge_slope = _power_slope([(o["boot"], o["charge"]) for o in points])
    charge_w = -charge_slope * sum(o["voltage"] for o in points) / len(points) * 3.6e-3
    ratio = charge_w / watts if watts else None
    if check_charge and not (ratio is not None and 0.5 <= ratio <= 1.5):
        return fail(f"电流积分与电量计斜率趋势不合（ratio={ratio}，要 0.5–1.5）")
    temps = [o["temp"] for o in points if o["temp"] is not None]
    result.update(averageW=watts, energyJ=energy_ws, durationSeconds=covered / 1e6,
                  rawCurrentSign=next(iter(signs)), samples=len(points),
                  chargeCounterW=charge_w, chargeToCurrentRatio=ratio,
                  tempRangeC=[min(temps) / 10, max(temps) / 10] if temps else None,
                  reason="电池侧电流幅值×电压梯形积分（剔首尾），未经外部校准")
    return result


def power_charge_corroboration(rows):
    """整局电量计交叉核对（兄弟项目 charge_corroboration 同式移植）：
    电量计须单调下降；≥2 次等差递减 → 量子 q，端点差 ±q 区间收电流积分；
    否则走整局 0.5–1.5 比值门（也要 ≥2 次向下跳变）。这是趋势一致性核对，
    不是绝对功率标定；不 corroborate → 功耗判读未证。"""
    obs = [{"boot": _power_num(r, "boot_us"), "current": _power_num(r, "current_ua"),
            "charge": _power_num(r, "charge_uah"), "row": r} for r in rows]
    base = {"corroborated": False, "precise_calibration": False}
    if len(obs) < 3:
        return {**base, "error": "电量观测不足 3 行"}
    if any(o["boot"] is None or o["charge"] is None or o["current"] is None for o in obs):
        return {**base, "error": "电量/电流/时钟读数缺行"}
    if any(b["boot"] <= a["boot"] for a, b in zip(obs, obs[1:])):
        return {**base, "error": "电量计观测时钟非单调"}
    deltas = [a["charge"] - b["charge"] for a, b in zip(obs, obs[1:])]
    nonzero = [v for v in deltas if v != 0]
    if not nonzero or not all(v > 0 for v in nonzero):
        return {**base, "error": "电量计无单调放电趋势"}
    consumed = obs[0]["charge"] - obs[-1]["charge"]
    integrated = 0.0
    for a, b in zip(obs, obs[1:]):
        seconds = (b["boot"] - a["boot"]) / 1e6
        if not 0 < seconds <= 3:
            return {**base, "error": "电量计观测采样间隔 >3s（掉线）"}
        if not (power_discharging(a["row"]) and power_discharging(b["row"])):
            return {**base, "error": "电量计核对要求整段物理放电（有接电/非放电行）"}
        integrated += (abs(a["current"]) + abs(b["current"])) / 2 * seconds / 3600
    out = {**base, "transitions": len(nonzero), "consumed_uah": consumed,
           "integrated_current_uah": integrated}
    if len(nonzero) >= 2 and len(set(nonzero)) == 1:
        quantum = nonzero[0]
        low, high = max(0, consumed - quantum), consumed + quantum
        return {**out, "method": "observed_constant_quantum_endpoint_interval",
                "quantum_uah": quantum, "counter_interval_uah": [low, high],
                "corroborated": low <= integrated <= high}
    ratio = consumed / integrated if integrated else None
    return {**out, "method": "whole_run_charge_change", "charge_to_current_ratio": ratio,
            "corroborated": len(nonzero) >= 2 and ratio is not None and 0.5 <= ratio <= 1.5}


def power_log_blocks(text):
    """设备侧采样文件 → 完整采样块列表（=== 分隔；末段未封闭的尾巴不算）。"""
    return [block for block in text.split("\n" + POWER_BLOCK_SEP + "\n")[:-1]]


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


# 目录清单三件套：find -exec {} + 会把全部路径摊进一次 exec——4 万文件的私有树
# 触发 toybox E2BIG（Argument list too long，9000 实测；与 deploy_game 核验同病
# 同治）：改 find -print0 | xargs -0 -n 256 分批。三道凭据缺一不可：① mksh
# `set -o pipefail` + 左组 `echo S2A_FRC` 报 find 自己的 rc（mksh 无 PIPESTATUS，
# 管道 rc 单独记 S2A_RC）——没有它 find 中途被拒（stderr Permission denied）
# 管道照样 rc=0 吞掉失败（9000 实证复现）；② S2A_N=find 计数（连计数管道自己的
# rc S2A_CRC）；③ stderr 全捕进变量、逐行 S2A_ERR: 前缀回投——adb 会把设备
# stderr 混进 stdout 流，靠前缀区分，不靠宿主 stderr。任一 rc≠0、计数非数字、
# stderr 非白名单 = 清单未证，半截不当全量。
LISTING_STDERR_OK = ()          # 明确无害的 stderr 行白名单（目前为空：有 stderr 即未证）
TAR_STDERR_OK = ()              # 备份 tar 的 stderr 白名单（同上：有 stderr 即备份未证）


def _listing_cmd(dir_, find_, xargs_, count_):
    """分批清单命令：fd3 旁路把清单与标记送 stdout，stderr 全捕 err 变量
    逐行 S2A_ERR: 前缀回投（mksh 无 PIPESTATUS/process-sub 的兼容写法）。
    计数先行，计数=0 不跑 find|xargs——toybox xargs 无 -r 时默认空输入也
    跑一次命令（sha256sum 空参读 stdin 出幻影行，空树会误拒）。"""
    return (f"cd {shlex.quote(dir_)} && {{ set -o pipefail; exec 3>&1; err=$( {{ "
            f"n=$({count_} | wc -l); echo \"S2A_N=${{n##* }} S2A_CRC=$?\" >&3; "
            f"if [ \"${{n##* }}\" -gt 0 ] 2>/dev/null; then "
            f"{{ {find_}; echo \"S2A_FRC=$?\" >&3; }} | {xargs_} >&3; "
            "echo \"S2A_RC=$?\" >&3; "
            "else echo \"S2A_FRC=0\" >&3; echo \"S2A_RC=0\" >&3; fi; "
            "} 2>&1 ); exec 3>&-; "
            "[ -z \"$err\" ] || printf '%s\\n' \"$err\" | sed 's/^/S2A_ERR: /'; }}")


def _stat_map(stat_text):
    """stat -c '%a|%F|%n' 清单 → {rel: {t, mode}}。rel 去掉 ./ 前缀。"""
    entries = {}
    for ln in stat_text.splitlines():
        ln = ln.strip()
        if not ln or "|" not in ln:
            continue
        mode, ftype, name = ln.split("|", 2)
        if not name.strip():
            continue                       # 空名行不成条目（normpath("") 会归一成 "."）
        rel = posixpath.normpath(name)     # "." = 根自身：收进清单核根权限（P2）
        # toybox stat %F 类型名（8E 实测：0 字节文件出 "regular empty file"，
        # 不是 "regular file"）；fifo/socket/字符/块设备映各自码，仍被 != f 拒
        t = {"regular file": "f", "regular empty file": "f",
             "directory": "d", "symbolic link": "l",
             "fifo": "p", "socket": "s",
             "character special file": "c",
             "block special file": "b"}.get(ftype, ftype)
        try:
            mode_i = int(mode, 8)
        except ValueError:
            raise RuntimeError(f"stat 清单未证：mode={mode!r} 非八进制")
        entries[rel] = {"t": t, "mode": mode_i}
    return entries


def _link_map(link_text):
    """软链清单（path<TAB>target 行）→ {rel: target}。"""
    links = {}
    for ln in link_text.splitlines():
        if "\t" not in ln:
            continue
        name, target = ln.split("\t", 1)
        rel = posixpath.normpath(name.strip())
        if rel in ("", "."):
            continue
        links[rel] = target
    return links


def _entry_map(sha_map, stat_map, link_map):
    """三份设备清单 → {rel: {t: f/d/l, sha?, mode, target?}}（stat 为主集）。"""
    entries = {rel: dict(e) for rel, e in stat_map.items()}
    for rel, sha in sha_map.items():
        if rel in entries:
            entries[rel]["sha"] = sha
    for rel, target in link_map.items():
        if rel in entries:
            entries[rel]["target"] = target
    return entries


def _listing_parts(text, what):
    """分批清单输出 → (清单文本, find 计数)。凭据：S2A_FRC=find rc、
    S2A_RC=管道 rc（pipefail）、S2A_N=find 计数、S2A_CRC=计数管道 rc、
    S2A_ERR:=stderr 行。任一 rc 非零/缺失、计数非数字、stderr 非白名单
    → RuntimeError 清单未证（半截/未证清单不当全量，fail closed）。"""
    lines, rcs, want, err = [], {}, None, []
    for ln in text.splitlines():
        if ln.startswith("S2A_ERR: "):
            err.append(ln[9:])
        elif ln.startswith("S2A_FRC="):
            rcs["find"] = ln[8:]
        elif ln.startswith("S2A_RC="):
            rcs["xargs"] = ln[7:]
        elif ln.startswith("S2A_N="):
            m = re.match(r"^S2A_N=(\S+) S2A_CRC=(\S+)$", ln)
            if m:
                want, rcs["count"] = m.groups()
        else:
            lines.append(ln)
    try:
        want_n = int(want) if want is not None else None
    except (TypeError, ValueError):
        want_n = None
    bad_err = [l for l in err if l not in LISTING_STDERR_OK]
    if (want_n is None or len(rcs) != 3 or any(v != "0" for v in rcs.values())
            or bad_err):
        raise RuntimeError(f"{what}清单未证（rc={rcs}，计数={want!r}，"
                           f"stderr={err[:3]!r}）：半截不当全量")
    return "\n".join(lines), want_n


def device_entries(d, pkg, dir_, raw=None):
    """设备目录全量清单：文件 sha + 类型 + 权限位 + 软链目标（解包核对与还原复核共用）。
    三份清单各走 -print0|xargs -n 256 分批（E2BIG），每批 rc=0 且解析条目数与
    find 计数一致才算完整。raw 传 dict 时回填三份设备端原始输出（核对失败落盘归因用）。"""
    sha_text = _owner_sh(d, pkg, dir_, _listing_cmd(
        dir_, "find . -type f -print0",
        "xargs -0 -n 256 sha256sum", "find . -type f"), timeout=300)
    stat_text = _owner_sh(d, pkg, dir_, _listing_cmd(
        dir_, "find . -mindepth 0 -print0",      # mindepth 0 含根目录自身：
        "xargs -0 -n 256 stat -c '%a|%F|%n'", "find . -mindepth 0"), timeout=300)
    link_text = _owner_sh(d, pkg, dir_, _listing_cmd(
        dir_, "find . -type l -print0",
        "xargs -0 -n 256 sh -c 'for p do printf \"%s\\t%s\\n\" \"$p\" "
        "\"$(readlink \"$p\")\"; done' _", "find . -type l"), timeout=300)
    if raw is not None:
        raw.update(sha256sum=sha_text, stat=stat_text, links=link_text)
    sha_text, sha_want = _listing_parts(sha_text, "文件 sha ")
    stat_text, stat_want = _listing_parts(stat_text, "stat ")
    link_text, link_want = _listing_parts(link_text, "软链 ")
    sha_map, stat_map, link_map = (_sha_listing(sha_text),
                                   _stat_map(stat_text), _link_map(link_text))
    bad = [f"{w}{have}≠{want}" for w, have, want in
           (("文件 sha ", len(sha_map), sha_want),
            ("stat ", len(stat_map), stat_want),
            ("软链 ", len(link_map), link_want)) if have != want]
    if bad:
        raise RuntimeError(f"{dir_} 目录清单不完整（{'；'.join(bad)}）：半截不当全量")
    return _entry_map(sha_map, stat_map, link_map)


def tar_entries(tar_path):
    """宿主机把归档解开核对：tar 成员 → 与 device_entries 同形的 {rel: entry}。"""
    entries = {}
    with tarfile.open(tar_path) as tf:
        for mem in tf.getmembers():
            # toybox `tar -cf - -C dir .` 首成员是 ./（tarfile 读出名归一成 .）：
            # 根目录也进清单（rel="."），与设备端 stat mindepth 0 同口径——
            # 根权限还原核验靠这条对拍，双侧原来都跳过是漏洞
            rel = posixpath.normpath(mem.name)
            if mem.isfile():
                h = hashlib.sha256()
                with tf.extractfile(mem) as ef:
                    for block in iter(lambda: ef.read(1 << 20), b""):
                        h.update(block)          # 大成员分块算，不整驻宿主内存
                entries[rel] = {"t": "f", "mode": mem.mode & 0o7777,
                                "sha": h.hexdigest()}
            elif mem.isdir():
                entries[rel] = {"t": "d", "mode": mem.mode & 0o7777}
            elif mem.issym():
                entries[rel] = {"t": "l", "mode": mem.mode & 0o7777, "target": mem.linkname}
            else:
                entries[rel] = {"t": "other", "mode": mem.mode & 0o7777}
    return entries


def entries_diff(want, got):
    """逐项对清单：缺/多/类型/ sha/权限/软链目标。返回人类可读差异列表。"""
    diffs = []
    for rel in sorted(set(want) - set(got)):
        diffs.append(f"缺 {rel}")
    for rel in sorted(set(got) - set(want)):
        diffs.append(f"多 {rel}")
    for rel in sorted(set(want) & set(got)):
        w, g = want[rel], got[rel]
        if w["t"] != g["t"]:
            diffs.append(f"{rel} 类型 {w['t']}≠{g['t']}")
        elif w["t"] == "f" and w.get("sha") != g.get("sha"):
            diffs.append(f"{rel} sha 不符")
        elif w["t"] == "l" and w.get("target") != g.get("target"):
            diffs.append(f"{rel} 软链目标 {w.get('target')!r}≠{g.get('target')!r}")
        elif w.get("mode") is not None and g.get("mode") is not None and w["mode"] != g["mode"]:
            diffs.append(f"{rel} 权限 {oct(w['mode'])}≠{oct(g['mode'])}")
    return diffs


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


def _is_private_tree(pkg, dir_):
    """应用私有树 = /data/data/<pkg> 及其子路径——只有它走 run-as。"""
    root = f"/data/data/{pkg}"
    return dir_ == root or dir_.startswith(root + "/")


def _owner_sh(d, pkg, dir_, cmd, timeout=120):
    """数据目录访问通道：私有树走 run-as（应用 uid 才够权）；
    其余目录（/sdcard/Android/* 外部数据）走 shell——9000 实测 run-as 碰外部
    目录被 SELinux 拒，而 shell 有外部存储全权。命令本体与判读口径不变。"""
    if _is_private_tree(pkg, dir_):
        return d.run_as(pkg, cmd, timeout=timeout)
    return d.shell(cmd, timeout=timeout)


def _dir_probe(d, pkg, dir_):
    """三态探针：ok=可访问 / absent=不存在 / denied=证不了（进不去或父目录
    不可搜索）。`[ -e ]` 是假三态——父目录不可搜索时它和「不存在」一样返回
    假（qa-fone 受限父目录实测）：必须用 stat 接 stderr 分 EACCES/ENOENT，
    分辨不了的一律 denied，绝不静默跳过。"""
    q = shlex.quote(dir_)
    out = _owner_sh(d, pkg, dir_, f"e=$(stat -c %F {q} 2>&1); "
                    f"if [ $? -eq 0 ]; then if [ -d {q} ] && [ -r {q} ] && [ -x {q} ]; "
                    f"then echo ok; else echo denied; fi; else "
                    f"case \"$e\" in *[Pp]ermission*|*EACCES*) echo denied;; "
                    f"*\"No such\"*|*ENOENT*) echo absent;; *) echo denied;; esac; fi",
                    timeout=30).strip().splitlines()
    return out[-1].strip() if out else "denied"


def _verify_tar(manifest, backup_dir, tag):
    """归档先解包核对：tar 成员逐项（文件 sha/软链目标/目录/权限）对源清单。
    返回差异列表（空=归档可信）——任何破坏性操作前必过此门。"""
    info = manifest["dirs"][tag]
    try:
        return entries_diff(info["entries"], tar_entries(Path(backup_dir) / info["tar"]))
    except (OSError, tarfile.TarError, KeyError) as error:
        return [f"归档解不开：{error}"]


def _exec_out_stream(d, cmd_args, dest, timeout=600):
    """exec-out 字节流分块落盘、边写边算 sha——不把整份备份读进宿主内存
    （9000 私有树实测 ~7.3GB，capture_output 直接爆内存）。stderr 落临时文件，
    防设备端报错输出填满管道互锁。超时/异常杀子进程、删半截归档再抛。
    返回 (字节数, sha256, rc, stderr 文本)。"""
    sel = selectors.DefaultSelector()
    err_f = tempfile.TemporaryFile()
    p = subprocess.Popen([*d.argv, "exec-out", *cmd_args],
                         stdout=subprocess.PIPE, stderr=err_f)
    h, nbytes = hashlib.sha256(), 0
    deadline = time.monotonic() + timeout
    try:
        sel.register(p.stdout, selectors.EVENT_READ)
        with open(dest, "wb") as fh:
            while True:
                if not sel.select(max(0.0, deadline - time.monotonic())):
                    raise subprocess.TimeoutExpired(cmd_args, timeout)
                chunk = p.stdout.read1(1 << 22)
                if not chunk:
                    break
                fh.write(chunk)
                h.update(chunk)
                nbytes += len(chunk)
        rc = p.wait(timeout=max(1.0, deadline - time.monotonic()))
    except BaseException:
        p.kill()
        p.wait()
        dest.unlink(missing_ok=True)
        raise
    finally:
        sel.close()
        p.stdout.close()
    err_f.seek(0)
    err = err_f.read().decode(errors="replace").strip()
    err_f.close()
    return nbytes, h.hexdigest(), rc, err


def backup_data(d, pkg, dest, left=None):
    """debuggable 原包的私有 / 外部数据 → exec-out tar 流式拉回 + 全量清单
    （文件 sha + 软链目标 + 目录含空目录 + 权限位）。通道按目录归属：私有树
    run-as（应用 uid），外部目录 shell（9000 实测 run-as 碰 /sdcard/Android/*
    被 SELinux 拒）。归档落盘后先解包与源清单逐项核对——对不上即 Refuse
    （保证不了还原就不入局）；存在但不可访问的目录报错而不是当不存在。"""
    dest.mkdir(parents=True)
    manifest = {"schema": 2, "package": pkg, "dirs": {}}
    for tag, tpl in DATA_DIRS:
        dir_ = tpl.format(pkg=pkg)
        state = _dir_probe(d, pkg, dir_)
        if state == "absent":
            continue
        if state != "ok":
            raise Refuse(f"{dir_} 目录状态证不了（存在但进不去或父目录不可搜索）："
                         f"证不了也备不了，不动这台设备")
        tar = dest / f"{tag}.tar"
        # exec-out 不经 pty：二进制流不被 \r\n 改写；run-as 只给私有树 debuggable 包。
        # exec-out 会把设备 stderr 混进归档字节流（9000 实证 ext.tar 尾部带
        # "tar: … Permission denied / tar: had errors"，宿主 stderr 反而是空的）——
        # stderr/rc 落设备侧本局唯一凭据：/data/local/tmp/s2a-tar-<tag>-<uuid>-{err,rc}
        # （被备份树之外——树里放凭据会删到用户同名文件还得靠 tar --exclude 排除，
        #   qa-fone #329620；/data/local/tmp 是 shell 域，预建 0666 让 run-as
        #   应用 uid 也能 2>/> 写）。预建先证不存在，存在即停不覆盖；拉完读回
        # 再核删；创建即登记待清——流传输异常/超时的任何路径都先核凭据删除
        # （qa-fone #329632），删不净 = 备份未证且凭据登记保留 → 收局并入留置。
        aux = f"/data/local/tmp/s2a-tar-{tag}-{uuid.uuid4().hex[:12]}"
        en, rn = aux + "-err", aux + "-rc"
        try:
            d.shell(f"[ ! -e {en} ] && [ ! -e {rn} ] && : > {en} && : > {rn} "
                    f"&& chmod 0666 {en} {rn} && test -w {en} && test -w {rn}",
                    timeout=30)
        except (RuntimeError, subprocess.SubprocessError) as error:
            raise Refuse(f"备份凭据 {aux}-* 预建失败或路径已存在（{error}）："
                         f"不覆盖既有文件，不动这台设备") from error
        if left is not None:
            left.append((en, rn))
        q = shlex.quote(dir_)
        try:
            cmd_ = f"cd {q} && tar -cpf - . 2> {en}; echo $? > {rn}"
            args_ = (["run-as", pkg, "sh", "-c", cmd_] if _is_private_tree(pkg, dir_)
                     else ["sh", "-c", cmd_])
            nbytes, digest, rc, _err = _exec_out_stream(d, args_, tar, timeout=600)
            try:
                dev_rc = d.shell(f"cat {rn}", timeout=30).strip()
                dev_err = d.shell(f"cat {en}", timeout=30)
            except (RuntimeError, subprocess.SubprocessError) as error:
                raise Refuse(f"{dir_} 备份 tar 的 rc/err 凭据读不回（{error}）：备份未证") \
                    from error
            bad_err = [l for l in dev_err.splitlines() if l not in TAR_STDERR_OK]
            if rc or dev_rc != "0" or not nbytes or bad_err:
                raise Refuse(f"{dir_} 备份未证（exec-out rc={rc}，tar rc={dev_rc!r}，"
                             f"stderr={bad_err[:3]!r}）：不拿未证的归档动这台设备")
        finally:
            # 凭据核删：流传输/取证任何出口都走到；删不净登记保留 → 收局留置
            try:
                d.shell(f"rm -f {en} {rn} && test ! -e {en} && test ! -e {rn}",
                        timeout=30)
            except (RuntimeError, subprocess.SubprocessError) as error:
                raise Refuse(f"{dir_} 备份 tar 凭据删不净（{error}）：备份未证") from error
            if left is not None:
                left.remove((en, rn))
        manifest["dirs"][tag] = {"dir": dir_, "tar": tar.name, "tar_sha256": digest,
                                 "entries": device_entries(d, pkg, dir_)}
        bad = _verify_tar(manifest, dest, tag)
        if bad:
            raise Refuse(f"{dir_} 备份归档与源清单对不上（{bad[:3]}）：回灌不可信，不动这台设备")
    for tag in manifest["dirs"]:
        manifest["dirs"][tag]["files"] = {
            rel: e["sha"] for rel, e in manifest["dirs"][tag]["entries"].items()
            if e["t"] == "f" and "sha" in e}
    (dest / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
    return manifest


def restore_data(d, pkg, manifest, backup_dir):
    """顺序是硬门：归档先解包核对（与源清单逐项一致）→ pm clear → 回灌 →
    设备端全量复核。核对不过一个字符都不许清。返回失败说明串列表（空=成功）。"""
    fails = []
    for tag in manifest["dirs"]:
        bad = _verify_tar(manifest, backup_dir, tag)
        if bad:
            fails.append(f"{manifest['dirs'][tag]['dir']} 归档核对未过：{bad[:3]}")
    if fails:
        return fails + ["归档核对未过：pm clear 未执行，设备数据未动"]
    try:
        # rc=0 且输出 Success 才算清掉；清不掉不回灌不删目录——设备数据原样留置
        out_ = d.shell(f"pm clear {pkg}", timeout=120)
        if "Success" not in out_:
            fails.append(f"pm clear 未返回 Success（{out_.strip()[:120] or '无输出'}）："
                         f"数据回灌不执行")
    except Exception as error:
        fails.append(f"pm clear 异常：{error}")
    if fails:
        return fails
    for tag, info in manifest["dirs"].items():
        dir_ = info["dir"]
        try:
            _owner_sh(d, pkg, dir_,
                      f"mkdir -p {shlex.quote(dir_)}; find {shlex.quote(dir_)} -mindepth 1 -delete",
                      timeout=120)
            tmp = f"/data/local/tmp/s2a-restore-{tag}.tar"
            d.run("push", str(Path(backup_dir) / info["tar"]), tmp, timeout=300)
            _owner_sh(d, pkg, dir_, f"tar -xpf {tmp} -C {shlex.quote(dir_)}", timeout=600)
            d.shell(f"rm -f {tmp}", allowed=(0, 1))
            bad = entries_diff(info["entries"], device_entries(d, pkg, dir_))
            if bad:
                fails.append(f"{dir_} 还原后与备份清单不符 {len(bad)} 项：{bad[:3]}")
        except Exception as error:
            # 回灌后核验链上任何异常（含清单解析错误）都进 fails → 留置，
            # 不许逸出跳过 manifest/release 收尾（qa-fone P1：ValueError 曾越过）
            fails.append(f"{dir_} 数据还原 adb 异常：{error}")
    return fails


def apply_checkpoint(d, pkg, cp_dir, manifest, log, out):
    """s2a-checkpoint/1 → 部署后、首启前解进存档根（BootSpec 冻结：
    files/user/0/saves/<titleId>/ryujinx-android；S2A_SAVE_DIR 在 typed BootSpec 下不生效）。
    写设备前宿主全验：schema/包名/titleId（规范 16 位大写十六进制）/meta.files 每项
    [长度, sha256] 对（含重复键、64-hex 校验）/tar sha/成员白名单（绝对路径、..、
    软链硬链、设备节点、重复成员名拒）/成员↔清单严格一致（含声明长度↔成员长度）。
    过门 push → run-as tar 解 → 逐文件 sha 复核。任一门不过记 verify=fail 抛 Refuse；
    临时 tar 删除失败记 tmp_left，收局并入还原失败 → 留置。"""
    block = manifest.setdefault("checkpoint", {"name": cp_dir.name})
    block.setdefault("name", cp_dir.name)

    def deny(reason):
        block["verify"] = "fail"
        block["fail"] = reason
        raise Refuse(f"检查点 {cp_dir.name} 不可用：{reason}")

    try:
        raw = json.loads((cp_dir / "meta.json").read_text(),
                         object_pairs_hook=lambda p: p)
    except (OSError, json.JSONDecodeError) as error:
        deny(f"meta.json 读不了：{error}")
    try:
        meta = dict(raw)
    except (TypeError, ValueError):
        deny("meta.json 顶层不是对象表")
    if meta.get("schema") != "s2a-checkpoint/1":
        deny(f"schema 不是 s2a-checkpoint/1（{meta.get('schema')}）")
    if meta.get("package") != pkg:
        deny(f"meta.package={meta.get('package')} 与本局 {pkg} 不符")
    title_id = str(meta.get("title_id") or "")
    if not re.fullmatch(r"[0-9A-F]{16}", title_id):
        deny(f"meta.title_id={title_id!r} 不是规范 16 位大写十六进制")
    mtid = re.search(r"\.t([0-9a-fA-F]{16})$", pkg)
    if mtid and title_id.lower() != mtid.group(1).lower():
        deny(f"meta.title_id={title_id} 与包名 {pkg} 的 titleId 不符")
    pairs = meta.get("files")
    if not isinstance(pairs, list) or not pairs:
        deny("meta.files 空或不是清单表")
    files_map = {}
    for item in pairs:
        if not (isinstance(item, (list, tuple)) and len(item) == 2
                and isinstance(item[0], str)):
            deny("meta.files 不是 {路径: [长度, sha256]} 清单表")
        rel, want = item[0], item[1]
        rel = posixpath.normpath(rel)
        if rel in files_map:
            deny(f"meta.files 重复声明 {rel}")
        if not (isinstance(want, list) and len(want) == 2
                and isinstance(want[0], int) and want[0] >= 0
                and isinstance(want[1], str)
                and re.fullmatch(r"[0-9a-f]{64}", want[1])):
            deny(f"meta.files[{rel}] 不是 [长度≥0, sha256(64hex)] 对")
        files_map[posixpath.normpath(rel)] = want
    tar_p = cp_dir / "saves.tar"
    try:
        want_tar_sha = (cp_dir / "saves.tar.sha256").read_text().split()[0]
    except (OSError, IndexError) as error:
        deny(f"saves.tar.sha256 读不了：{error}")
    got_tar_sha = sha256(tar_p)
    if got_tar_sha != want_tar_sha:
        deny(f"saves.tar sha256 与 .sha256 不符（{got_tar_sha[:12]}≠{want_tar_sha[:12]}）")
    block["source_sha256"] = want_tar_sha
    tar_shas = {}
    try:
        with tarfile.open(tar_p) as tf:
            members = tf.getmembers()
            # 写设备前逐文件算 tar 内容 sha：只核长度会放过同长异内容的成员
            for mem in members:
                if mem.isfile():
                    fh = tf.extractfile(mem)
                    tar_shas[posixpath.normpath(mem.name)] = hashlib.sha256(
                        fh.read() if fh else b"").hexdigest()
    except (tarfile.TarError, OSError, EOFError) as error:
        deny(f"saves.tar 打不开/截断：{error}")
    tar_files, tar_sizes, seen = {}, {}, set()
    for mem in members:
        if (mem.name.startswith("/") or posixpath.isabs(mem.name)
                or ".." in mem.name.split("/")):
            deny(f"tar 成员越界：{mem.name}")
        rel = posixpath.normpath(mem.name)
        if rel in seen:
            deny(f"tar 成员重复：{rel}")
        seen.add(rel)
        if mem.isfile():
            tar_files[rel] = True
            tar_sizes[rel] = mem.size
        elif not mem.isdir():
            deny(f"tar 成员类型不收（{mem.name}）：只收普通文件与目录")
    extra = sorted(rel for rel in tar_files if rel not in files_map)
    missing = sorted(rel for rel in files_map if rel not in tar_files)
    if extra or missing:
        deny(f"tar 成员与 meta.files 不一致：多 {extra[:3] or '无'} 缺 {missing[:3] or '无'}")
    bad_size = sorted(rel for rel, want in files_map.items()
                      if tar_sizes[rel] != want[0])
    if bad_size:
        deny(f"声明长度与 tar 成员不符 {len(bad_size)} 项：{bad_size[:3]}")
    bad_sha = sorted(rel for rel, want in files_map.items()
                     if tar_shas.get(rel) != want[1])
    if bad_sha:
        deny(f"tar 成员内容 sha 与 meta.files 不符 {len(bad_sha)} 项：{bad_sha[:3]}")
    state_dir = f"/data/data/{pkg}/files/user/0/saves/{title_id}/ryujinx-android"
    tmp = f"/data/local/tmp/s2a-checkpoint-{uuid.uuid4().hex[:12]}.tar"
    # 先登记待清，确认设备上已删除才撤销：push 中途失败、rm 失败或被中断
    # （SIGTERM / KeyboardInterrupt）都留下登记，收局并入还原失败 → 留置
    block["tmp_left"] = tmp
    try:
        d.run("push", str(tar_p), tmp, timeout=300)
        d.run_as(pkg, f"mkdir -p {shlex.quote(state_dir)}", timeout=60)
        d.run_as(pkg, f"tar -xpf {tmp} -C {shlex.quote(state_dir)}", timeout=600)
    finally:
        try:
            d.shell(f"rm -f {tmp}")          # rc≠0 抛错；rc=0（rm -f）即已不在
            block.pop("tmp_left", None)
        except (RuntimeError, subprocess.SubprocessError) as error:
            block["tmp_cleanup_error"] = str(error)[:160]
    if block.get("tmp_left"):
        deny(f"临时归档 {tmp} 清不掉或未证已删，停起局并留置")
    raw = {}
    entries = device_entries(d, pkg, state_dir, raw=raw)
    bad = [rel for rel, want in files_map.items()
           if entries.get(rel, {}).get("t") != "f" or entries[rel].get("sha") != want[1]]
    if bad:
        # verify=fail 归因：设备端原始 stat/sha/软链清单落 out/checkpoint-device/
        dump = out / "checkpoint-device"
        dump.mkdir(parents=True, exist_ok=True)
        for name, text in raw.items():
            (dump / f"{name}.txt").write_text(text)
        block["device_listing"] = "checkpoint-device/"
        deny(f"解入后核对不符 {len(bad)} 项：{bad[:3]}"
             f"（设备原始清单已存 {dump.name}/）")
    block.update(scene=meta.get("scene"), title_id=title_id,
                 files=len(files_map),
                 extra_files=sum(1 for rel, e in entries.items()
                                 if rel != "." and rel not in files_map
                                 and e.get("t") != "d"),
                 extra_dirs=sum(1 for rel, e in entries.items()
                                if rel != "." and rel not in files_map
                                and e.get("t") == "d"),
                 verify="ok")
    log(f"检查点 {cp_dir.name} 已解入 saves/{title_id}/ryujinx-android（{len(files_map)} 文件 sha 复核过）")


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


USER_HZ = 100     # Android 内核 jiffies 用户态口径恒 100


def device_timebase(d):
    """设备墙钟（date +%s）+ 开机秒（/proc/uptime）一对：把 birth_ticks 折成墙钟
    （birth_wall_s = wall_now -（uptime_now - ticks/USER_HZ）），与文件 mtime 同一
    CLOCK_REALTIME 可比。取不到返回 (None, None)。"""
    try:
        wall_s = int(d.shell("date +%s", timeout=10).strip())
        uptime_s = float(d.shell("cat /proc/uptime", timeout=10).split()[0])
        return wall_s, uptime_s
    except (RuntimeError, subprocess.SubprocessError, ValueError, IndexError):
        return None, None


def frame_log_blocks(text, pids=None):
    """meta→end 切帧日志块；给 pids 时只收这些 PID 的行（logcat 行前缀 PID 绑定当局进程）。
    没前缀 / 别的 PID 的帧日志行一律不进块；块按行前缀 PID 各自维护——当局是
    多进程（pkg / pkg:*）时两进程的转储在 logcat 里逐行交错也各成各的完整块，
    只有同 PID 的新 meta 才截断同 PID 的旧块（异 PID 行插进来不算顶断、不拼块）。
    没前缀时（私有文件整文）单块照旧。末尾被截断的块只坏自己。
    截断块（没等到 end：被同 PID 新 meta 顶断 / 文末就断了）同样交出——缺了
    哪些片、缺 end/SHA 由 parser 如实报「转储被截断 / 整段未证」，不静默丢。"""
    segs = {}
    for line in text.splitlines():
        if "direct frame-log" not in line:
            continue
        lp = None
        if pids is not None:
            m = THREADTIME_PID.match(line)
            if not m or int(m.group(1)) not in pids:
                continue
            lp = int(m.group(1))
        if "direct frame-log meta:" in line:
            if segs.get(lp):
                yield "\n".join(segs[lp])
            segs[lp] = []
        if lp in segs:
            segs[lp].append(line)
            if "direct frame-log end:" in line:
                yield "\n".join(segs.pop(lp))
    for seg in segs.values():
        yield "\n".join(seg)


GFX_SEL = "graphics selection:"
# runtime.c 起局唯一一次选择后打「graphics selection: direct|generic (原因), devices=…」——
# fprintf(stderr) 经 StdErrAdapter 泵进 logcat（tag S2Arm、PID=当局进程）；v1 会话不打此行。
GFX_SEL_MODE = re.compile(r"graphics selection: (direct|generic)\s*\(")


def graphics_selection_lines(text, pids):
    """本局 PID 行里的 graphics selection 行原文（logcat 行前缀 PID 绑当局）。"""
    lines = []
    for ln in text.splitlines():
        if GFX_SEL not in ln:
            continue
        m = THREADTIME_PID.match(ln)
        if not m or int(m.group(1)) not in pids:
            continue
        lines.append(ln)
    return lines


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
    header = "frame\tpresent_ns\tpresent_us\twall_us\tdisp_us\tdisp_seq\n"
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
        # v5 记录的 disp_us/disp_seq：displayTiming 已确认的相邻帧上屏间隔，disp_seq
        # 标明间隔属于哪个较新帧（与帧号同域 = first+i+1 基 present 序号）；读数比所属
        # 帧晚到（落在当时正在写的记录上），按 seq 对号不按行序；未确认/旧版本（v2–v4）
        # 留空——CPU present 间隔不冒充确认读数。
        disp_by_seq = {}
        if cap.get("disp_us") and cap.get("disp_seq"):
            for i, (us, seq) in enumerate(zip(cap["disp_us"], cap["disp_seq"])):
                if us is not None and seq is not None:
                    disp_by_seq[seq] = us
        with (out / "frames.tsv").open("w") as f:
            f.write(header)
            for j in range(1, len(rec)):
                (i0, t0), (i1, t1) = rec[j - 1], rec[j]
                # stats() 同一口径：帧号 = 1 基 present 序号；present_us/wall_us =
                # 相邻 present 间隔（宿主算，CPU 口径）；disp_us/disp_seq 只写实测确认值。
                n = first + i1 + 1
                du = disp_by_seq.get(n)
                f.write(f"{n}\t{t1 * 1000}\t{t1 - t0}\t{t1 - t0}"
                        + (f"\t{du}\t{n}\n" if du is not None else "\t\t\n"))
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
            "binding": "meta uuid=本局 setprop + logcat 行 PID",
            "window_boot_us": list(window_us) if window_us else None,
            "cover": cover, "rows": rows, "captures": len(caps)}


PRIV_LOG_NAME = re.compile(r"^(?:host-(\d+)-(\d+)-[A-Za-z0-9]+\.log|frame-log-(\d+)\.log)$")
# host-*.log 首行（os.c s2a_android_session_log_start 的 dprintf）：
#   [s2a] Android 会话日志：session=<seg> pid=<pid> file=<path>
# 名实互核：文件名 <seg>/<pid> 与首行写明的对不上 = 改名/拼接过的文件，不证归属。
PRIV_SESSION_LINE = re.compile(rb"session=(\d+) pid=(\d+)(?: uuid=(\S+))?")
PRIV_FRAME_UUID = re.compile(rb"direct frame-log meta:[^\n]*? uuid=(\S+)")
# 帧块同源绑：logcat 行 / 私有文件里的 meta 行都是 nvn_vk_frame_log.c 同文——
# uuid=<本局 setprop> 才进帧表；字段缺（旧构建/属性没置上）或值不符（别局
# 转储/拼接）整段不收，PID+时间窗单独不足以证本局（同 PID 旧局会撞）。
FRAME_UUID = re.compile(r"direct frame-log meta:[^\n]*? uuid=(\S+)")


def collect_private_logs(d, pkg, pids, out, notes, birth_walls=None,
                         ready_timebase=None, session_uuid=None):
    """当局进程留在应用私有目录的证据：native 直写的 frame-log-<pid>.log
    （nvn_vk_frame_log.c 与 logcat 同文双写——logcat 分片被截断时它是完整源）
    与 saveRoot/ryujinx-android/host-*-<pid>-*.log（进程宿主日志，含 NUL 原始字节）。

    归属与完整性（qa-fone 复审口径）：
    - bound 要实证，fail closed：PID 在 session_pids、stat mtime 实读、
      进程 birth 实读（/proc/<pid>/stat 折墙钟；拿不到一律 unproven）、
      墙钟跳变复核过；host-*.log 另要首行 `session=<seg> pid=<pid>` 与文件名
      逐段对上；**本局锚 = 文件内容里的 session_uuid**：运行器起局前
      setprop debug.s2a.session_uuid，os.c 会话日志首行 uuid= 与帧日志 meta
      行尾 uuid= 都写它——等于本局才 bound；字段缺（旧构建/属性没置上）、
      值不符（别局文件）→ unproven。mtime 只记注不裁决：stat 整秒 vs birth
      浮点判不了先后，判不了先后时收证标 unproven（uuid 在才证先后）；
      birth 缺、时基缺、stat 读不了、名实对不上 → 证据照拉但
      session=unproven + 注记写明哪一环证不了；
    - 完整性：先 run-as sha256sum + wc -c 记源端字节/哈希（manifest 记双侧对拍），
      exec-out 拉回后逐字节核对；不符或拉回空数据标 verified=False
      （rc=0 空读不能当成功）；
    - pids 为空（READY 前异常：进程死了没绑到当局 PID）→ 退化按文件名 PID 收证、
      session=unproven 如实标——死前宿主日志正是失败现场证据；
      连候选都没有也写明未证；
    - run-as 只给 debuggable 包——进不去记注（证据缺 ≠ 还原失败）。
    必须在收局还原（pm clear/卸载清私有目录）前调用。
    返回（manifest 记录列表, frame-log 原文列表）。"""
    records, frame_texts = [], []
    try:
        found = d.run_as(pkg, f"find /data/data/{pkg} -type f -name '*.log'", timeout=60)
    except (RuntimeError, subprocess.SubprocessError) as error:
        notes.append(f"私有日志目录进不去（非 debuggable/包已卸）就没拉：{str(error)[:200]}")
        return records, frame_texts
    candidates = []
    for path in sorted(ln.strip() for ln in found.splitlines() if ln.strip()):
        m = PRIV_LOG_NAME.match(path.rsplit("/", 1) [-1])
        if m:
            candidates.append((path, int(m.group(2) or m.group(3))))
    if pids:
        candidates = [c for c in candidates if c[1] in pids]
        if not candidates:
            notes.append("私有目录下没有匹配当局 PID 的日志文件（零匹配如实记）")
    elif candidates:
        notes.append("没有当局进程绑定（READY 前异常）：私有日志退化按起局时间窗"
                     "+文件名 PID 收证，归属未证（session=unproven）")
    else:
        notes.append("READY 前异常且私有目录下没有任何日志可收：失败现场证据未证")
    # 时钟跳变复核：READY 记下的 (wall0, uptime0) 锚点对与收证时再读的对得上——
    # wall2 应 ≈ wall0 + (uptime2 - uptime0)。墙钟被人改/NTP 大步跳变、READY 时基
    # 不全、复核再读失败 → mtime 归属整域不可靠，一律 unproven（fail closed）。
    clock_ok = None
    if pids:
        w0, u0 = (ready_timebase or (None, None))
        if w0 is None or u0 is None:
            clock_ok = False
        else:
            w2, u2 = device_timebase(d)
            clock_ok = (w2 is not None and u2 is not None
                        and abs(w2 - (w0 + (u2 - u0))) <= 5)
        if clock_ok is False:
            notes.append("设备墙钟与开机钟对不上（跳变/READY 时基不全/复核读不了）："
                         "mtime 归属不可靠，私有证据一律按 unproven 记账")
    dest = out / "host_logs"
    for path, pid in candidates:
        name = path.rsplit("/", 1)[-1]
        # fail-closed 归属：每一环实证了才升 bound，证不了照拉但标 unproven
        session, attr = "unproven", None
        if pids:
            if clock_ok is False:
                attr = "墙钟跳变或 READY 时基不全，mtime 归属不可靠"
            bound_s = (birth_walls or {}).get(pid)  # 进程出生墙钟：唯一归属锚
            if attr is None and bound_s is None:
                attr = "进程 birth 拿不到（/proc/<pid>/stat 缺/读拒），起局窗不充数"
            elif attr is None:
                try:
                    mtime = int(d.run_as(pkg, f"stat -c %Y {shlex.quote(path)}",
                                         timeout=30).strip())
                except (RuntimeError, subprocess.SubprocessError, ValueError):
                    mtime = None
                if mtime is None:
                    attr = "stat 拿不到 mtime"
                elif mtime < int(bound_s):         # mtime 早于进程出生（stat 整秒
                    notes.append(f"私有日志 {name}（PID {pid}）mtime 早于进程出生："  # 判得旧）/同秒
                                 f"照拉收证——先后由文件内容的本局 uuid 裁决")          # 判不了先后：不跳过不判 bound
        # 源端字节/SHA 先记账：exec-out rc=0 空读/截断传输出核对不上就标未证
        want_sha = want_bytes = None
        try:
            meta_out = d.run_as(pkg, f"sha256sum {shlex.quote(path)}; wc -c < {shlex.quote(path)}",
                                timeout=60)
            lines = [ln.strip() for ln in meta_out.splitlines() if ln.strip()]
            if len(lines) >= 2 and re.fullmatch(r"[0-9a-f]{64}", lines[0].split()[0]):
                want_sha, want_bytes = lines[0].split()[0], int(lines[-1].split()[0])
        except (RuntimeError, subprocess.SubprocessError, ValueError, IndexError):
            pass
        # exec-out 不经 pty：host 日志含 NUL，原始字节不被 \r\n 改写（同 run-as 备份）
        r = subprocess.run([*d.argv, "exec-out", "run-as", pkg, "sh", "-c",
                            f"cat {shlex.quote(path)}"], capture_output=True, timeout=300)
        if r.returncode:
            notes.append(f"私有日志 {path} 拉取失败 rc={r.returncode}："
                         f"{r.stderr.decode(errors='replace')[:160]}")
            continue
        got_sha, got_bytes = hashlib.sha256(r.stdout).hexdigest(), len(r.stdout)
        verified = bool(got_bytes) and want_sha == got_sha and want_bytes == got_bytes
        if not verified:
            why = "拉回空数据" if not got_bytes else \
                  f"源 {want_bytes}B/{str(want_sha)[:12]} ≠ 拉回 {got_bytes}B/{got_sha[:12]}"
            notes.append(f"私有日志 {name} 拉取校验不符（{why}）：标未证")
        # 本局锚：文件内容里的 session_uuid（运行器 setprop 的本局号，os.c 首行 /
        # 帧日志 meta 都写）——等于本局且前置佐证全过才 bound。旧文件造不出
        # 本局随机号，uuid 是「属这局」的内容级实证。
        file_uuid = None
        if attr is None and pids:
            if name.startswith("host-"):
                m = PRIV_LOG_NAME.match(name)
                head = PRIV_SESSION_LINE.search(r.stdout[:4096])
                if m is None or head is None \
                        or int(head.group(1)) != int(m.group(1)) or int(head.group(2)) != pid:
                    attr = "首行会话标记与文件名 session/pid 对不上"
                elif head.group(3):
                    file_uuid = head.group(3).decode(errors="replace")
            elif name.startswith("frame-log-"):
                um = PRIV_FRAME_UUID.search(r.stdout[:8192])
                file_uuid = um.group(1).decode(errors="replace") if um else None
            if attr is None and file_uuid != session_uuid:
                attr = ("文件内容没有 session_uuid 字段（旧构建/属性没置上）：本局身份未证"
                        if file_uuid is None else
                        f"文件 uuid={file_uuid} 与本局 session_uuid 不符：别局文件不冒充本局")
            elif attr is None:
                session = "bound"
        if attr:
            notes.append(f"私有日志 {name} 归属未证（{attr}）：按 unproven 记账")
        dest.mkdir(exist_ok=True)
        (dest / name).write_bytes(r.stdout)
        records.append({"name": name, "pid": pid, "bytes": got_bytes, "sha256": got_sha,
                        "device_bytes": want_bytes, "device_sha256": want_sha,
                        "verified": verified, "session": session})
        if name.startswith("frame-log-") and verified and session == "bound":
            frame_texts.append((pid, r.stdout.decode(errors="replace")))
    return records, frame_texts


def external_dirs_gone(d, pkg):
    """卸载后 /sdcard/Android/data|obb/<pkg> 应随删；逐项验不存在。
    stat 分辨三态（EACCES=读不进、ENOENT=真没有），读不进 ≠ 已清。"""
    fails = []
    for base in ("/sdcard/Android/data", "/sdcard/Android/obb"):
        q = shlex.quote(f"{base}/{pkg}")
        out = d.shell(f"e=$(stat -c %F {q} 2>&1); "
                      f"if [ $? -eq 0 ]; then if [ -r {q} ] && [ -x {q} ]; then ls {q}; "
                      f"else echo __denied__; fi; else "
                      f"case \"$e\" in *[Pp]ermission*|*EACCES*) echo __denied__;; "
                      f"*\"No such\"*|*ENOENT*) :;; *) echo __denied__;; esac; fi",
                      allowed=(0, 1)).strip()
        if "__denied__" in out:
            fails.append(f"{base}/{pkg} 存在但读不进：证不了卸载已清")
        elif out:
            fails.append(f"{base}/{pkg} 卸载后仍有内容：{out.splitlines()[0][:80]}")
    return fails


def restore_state(d, pkg, state0, orig_apk, data_backup=None, backup_dir=None):
    """还原原安装状态（范围见 docstring）。返回失败说明串列表（空=成功）。
    data_backup=入局前的 run-as 备份清单（debuggable 路径），给时先装回原 APK
    再逐文件 sha 核还原数据。装回方向的降级（原 vc < 新 vc）入局前已拒跑，
    装回只剩升级/同版——install -r 就够，不走 -d。"""
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
            d.run("install", "-r", str(orig_apk), timeout=600)
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


SAMPLER_TAG = "s2a-power-sampler"


def start_power_sampler(d, supply):
    """设备侧采样守护：mksh 后台 while 循环 1Hz 写 `===` 分隔的采样块到
    /data/local/tmp——adb 掉线只断拉取、不断设备上的采样（兄弟项目
    「应用私存数据、拔电期间 adb 断开也不丢」同语义）。整串必须走 sh -c。
    后台进程必须三 fd 全脱离（adbd 语义：有任何 fd 挂着传输通道，adb shell
    就不退，8E 实测 15s 超时、孤儿守护继续写）——setsid 另开会话脱控制终端，
    </dev/null + 输出进日志文件 + stderr 并入；PID 文件由守护自己写（setsid
    可能 fork，$! 不一定就是守护号），并留 SAMPLER_TAG 供 pgrep 兜底。"""
    dest = POWER_LOG
    loop = (f"echo $$ >{dest}.pid; while :; do : {SAMPLER_TAG}; "
            f"{power_sample_command(supply)}; "
            f"printf 'capacity=%s\\n' \"$(cat /sys/class/power_supply/{supply}/capacity 2>/dev/null)\"; "
            f"printf '{POWER_BLOCK_SEP}\\n'; sleep 1; done")
    inner = (f"rm -f {dest} {dest}.pid; "
             f"setsid sh -c {shlex.quote(loop)} >{dest} 2>&1 </dev/null & echo $!")
    out = d.shell("sh -c " + shlex.quote(inner), timeout=15).strip()
    pid = out.splitlines()[-1].strip() if out.splitlines() else ""
    if not re.fullmatch(r"[0-9]+", pid):
        # 空回/半截回执（sh 报错文本、通道被掐后的残缺输出）= spawn 未证：
        # 守护可能已起在写——按启动失败交调用方扫孤儿，不拿脏串当 PID。
        raise RuntimeError(f"采样守护启动回执不可信：{out[-120:]!r}")
    return dest, pid


def power_rows(d, dest):
    """cat 设备采样文件 → [(row, level)]；末段未封闭的尾巴不算（下轮再收）。"""
    out = []
    text = d.shell(f"cat {dest}", allowed=(0, 1), timeout=20)
    for block in power_log_blocks(text):
        try:
            row = power_parse_sample(block + "\n")
        except (RuntimeError, ValueError):
            continue  # 坏块跳过（设备文件原样留着，不丢证据）
        level = (re.search(r"^capacity=(\d+)$", block, re.M) or [None, ""])[1]
        out.append((row, level))
    return out


def stop_power_sampler(d, dest, pid=None):
    """杀守护 → 退场复验 → 拉回全量 → 清设备文件；返回 [(row, level)]。
    pid 拿不到（起守护超时 / 中断在赋值前）也照扫：PID 文件与 SAMPLER_TAG
    进程标记两路兜底，设备上不留孤儿循环。杀后仍有守护存活 → 抛错留证
    （日志已拉回、设备文件不清）。"""
    # kill 前按 /proc/<pid>/cmdline 核标记——PID 被回收成别家进程不许误杀；
    # 只等真杀过的 pid 退场（/proc/<pid> 没了才算死），杀不死的报 LINGER。
    out = d.shell(
        f"pids=\"{pid or ''} $(cat {dest}.pid 2>/dev/null) "
        f"$(pgrep -f {SAMPLER_TAG} 2>/dev/null)\"; killed=; "
        f"for p in $pids; do grep -aqs {SAMPLER_TAG} /proc/$p/cmdline 2>/dev/null "
        f"&& kill $p 2>/dev/null && killed=\"$killed $p\"; done; "
        f"for p in $killed; do i=0; "
        f"while [ -d /proc/$p ] && [ $i -lt 10 ]; do sleep 0.2; i=$((i+1)); done; "
        f"[ -d /proc/$p ] && echo LINGER:$p; done; "
        f"rm -f {dest}.pid",
        allowed=(0, 1), timeout=15)
    rows = power_rows(d, dest)
    linger = [w.split(":", 1)[1] for w in out.split() if w.startswith("LINGER:")]
    if linger:
        raise RuntimeError(
            f"采样守护 {','.join(linger)} 杀掉后仍存活，设备文件留 {dest} 未清")
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


def read_debug_props(device):
    """按 Android getprop 无参输出读取全量快照；不完整或无法判读就拒跑。"""
    r = subprocess.run([*device.argv, "shell", "getprop"], capture_output=True, timeout=60)
    if r.returncode or r.stderr:
        detail = (r.stderr or r.stdout).decode("utf-8", errors="replace")[:300]
        raise Refuse(f"getprop 全量读取失败（rc={r.returncode}）：{detail}")
    try:
        data = r.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise Refuse(f"getprop 全量输出不是 UTF-8：{error}") from error
    if not data or not data.endswith("\n"):
        raise Refuse("getprop 全量输出为空或不完整")
    props = {}
    seen = set()
    for line in data.splitlines():
        match = re.fullmatch(r"\[([^]\r\n]+)\]: \[(.*)\]", line)
        if not match or match.group(1) in seen:
            raise Refuse(f"getprop 全量输出格式错误或键重复：{line!r}")
        key, value = match.groups()
        seen.add(key)
        if key.startswith("debug.s2a."):
            props[key] = value
    return props


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
    p.add_argument("--checkpoint", type=Path, default=None, metavar="DIR",
                   help="s2a-checkpoint/1 检查点目录：部署后、首启前 run-as 解进存档根并逐文件 sha 核对")
    p.add_argument("--mode", choices=["auto", "generic", "direct"], default="auto")
    p.add_argument("--extra", action="append", default=[], help="s2arm.KEY[=VALUE]，可多次")
    p.add_argument("--ab-prop", action="append", default=[], metavar="debug.s2a.KEY=VALUE",
                   help="A/B 局用：起局前在锁内置位一个 debug.s2a.* 属性（记原值、读回核对、收局与运行器自管属性同一条恢复链），可多次")
    p.add_argument("--allow-prop", action="append", default=[], metavar="debug.s2a.KEY",
                   help="显式允许一个已有的非空 debug.s2a.* 属性（不修改其值），可多次")
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
    p.add_argument("--discharge-settle", type=int, default=None,
                   help="采集窗开始前要连续物理放电的秒数（能耗口径；缺省 30，"
                        "接电/充电下自动判不适用不等；显式给了照等，0=不等）")
    p.add_argument("--discharge-timeout", type=int, default=300,
                   help="放电稳定门的等待上限秒数（0=等到够为止；超时照常采集、能耗记没数据）")
    p.add_argument("--power-interval", type=float, default=1.0,
                   help="拉取设备侧电池采样文件的间隔秒")
    p.add_argument("--no-power-sampler", nargs="?", const="", default=None,
                   metavar="原因",
                   help="不起设备端电池采样守护（接电功能局/帧时性能局不需要能耗"
                        "数据；可附原因记账）。run-manifest 记 power_sampler="
                        "disabled+原因，quality_check 能耗报「没数据」")
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
        # s2arm.debug_env 的值是 S2A_KEY=VALUE 形式（本身带 =），按调试环境批次口径单验；
        # 其余 extra 照旧 dev_runtime.EXTRA 的值字符集
        if e.startswith("s2arm.debug_env="):
            if not re.fullmatch(r"s2arm\.debug_env=S2A_[A-Za-z0-9_]+=[^\x00]{1,8192}", e):
                p.error(f"s2arm.debug_env 只收 S2A_KEY=VALUE：{e!r}")
        elif not dev_runtime.EXTRA.fullmatch(e):
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
    data_backup, backup_dir = None, None
    backup_left, backup_started = [], False   # 备份凭据登记/中途失败标（qa-fone #329632）
    props_prev = {}
    ab_strict = set()              # --ab-prop 已读到原值、即将 / 已经 setprop 的键：恢复时一律只认 rc=0
    logcat_proc, log_file = None, None
    sampler, seen_boots, power_f = None, set(), None
    pids, priv_done, birth_walls = {}, False, {}
    wall_now = uptime_now = None   # READY 时基锚点对；READY 前异常保持 None → 跳变复核 fail closed
    ready_uuid = None              # READY 行里的本局 session（设备端生成）；READY 前异常保持 None
    run_uuid = None                # 本局 session_uuid（运行器生成、setprop 下发）；起局前异常保持 None
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

        # 全量读取必须发生在设备锁内、安装或任何属性写入前；只把本项目命名空间记入 manifest。
        ab_values = []
        ab_keys = set()
        for spec in args.ab_prop:
            key, sep, val = spec.partition("=")
            if not sep or not re.fullmatch(r"debug\.s2a\.[a-z0-9_]+", key) or not re.fullmatch(r"[A-Za-z0-9_.-]{0,64}", val):
                raise Refuse(f"--ab-prop 格式不对（只收 debug.s2a.<名>=<值>）：{spec}")
            if key in ab_keys or key == SESSION_PROP or key in DEBUG_PROPS:
                raise Refuse(f"--ab-prop 与运行器自管属性或其他 --ab-prop 重名：{key}")
            ab_keys.add(key)
            ab_values.append((key, val))
        for key in args.allow_prop:
            if not re.fullmatch(r"debug\.s2a\.[A-Za-z0-9_.-]+", key):
                raise Refuse(f"--allow-prop 只收完整 debug.s2a.<名>：{key}")
        manifest["debug_props_allow"] = sorted(set(args.allow_prop))
        manifest["debug_props_ab_keys"] = sorted(ab_keys)
        manifest["debug_props_snapshot"] = read_debug_props(d)
        allowed = set(DEBUG_PROPS) | {SESSION_PROP} | ab_keys | set(args.allow_prop)
        residual = {k: v for k, v in manifest["debug_props_snapshot"].items()
                    if v and k not in allowed}
        if residual:
            raise Refuse("发现未许可的 debug.s2a.* 残留属性：" +
                         ", ".join(f"{k}={v!r}" for k, v in sorted(residual.items())))

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
        if args.checkpoint:
            manifest["checkpoint"] = {"name": args.checkpoint.name}
            if state0["installed"]:
                manifest["checkpoint"].update(
                    verify="fail", fail="设备已装该包：检查点会清/盖已有数据")
                raise Refuse("检查点只收原未安装的局：设备已装该包，解入会动已有数据——"
                             "先人工卸了再跑，或拿掉 --checkpoint")
        # ---- versionCode / 降级：装新包方向（原>新）由 install -r -d 实测；装回方向 ----
        # ---- （原<新）证不了能装回 → 入局前直接拒跑（manifest 记决策依据）----
        downgrade_fwd = False
        if state0["installed"] and state0["sha256"] != our_sha:
            import game_apk  # 延迟导入：只在这条路径要读新包 versionCode
            apk_pkg, new_vc, _dbg = game_apk.apk_identity(args.apk)
            if apk_pkg != pkg:
                raise Refuse(f"--apk 的包名 {apk_pkg} 与 --package {pkg} 不符，装错了会丢数据")
            orig_vc = state0.get("versionCode")
            manifest["versions"] = {"orig_versionCode": orig_vc, "new_versionCode": new_vc}
            if orig_vc is None:
                raise Refuse("原包 versionCode 读不到：装回方向判不了降级，不动这台设备")
            downgrade_fwd = orig_vc > new_vc   # 装低 versionCode 测试包（真降级=装它本身）
            if orig_vc < new_vc:
                # 装回方向是降级：先升级、收局才试降级 = 装了赌能降回去（qa-fone）。
                # 动包之前证不了能装回 → 拒跑让人处理，不动这台设备。
                raise Refuse(f"versionCode 原 {orig_vc} 低于新 {new_vc}：装回方向是降级，"
                             f"装了回不来——入局前证不了能恢复就不动这台设备"
                             f"（人工卸/升原包，或换 versionCode ≤ 原包的测试包）")
            orig_tmp = out / "orig-backup" / "orig.apk"   # 备份落在本次 out 下
            orig_tmp.parent.mkdir(parents=True, exist_ok=True)
            d.run("pull", state0["base"], str(orig_tmp), timeout=300)
            if sha256(orig_tmp) != state0["sha256"]:
                raise Refuse("备份原包 sha256 与设备上读到的不符，放弃起局")
            log(f"已备份原包 sha256={state0['sha256'][:12]}")
        else:
            log(f"原安装状态：{'未安装' if not state0['installed'] else '与本包同 sha256'}")
        # ---- debuggable 原包：run-as tar 全量备份私有/外部数据（装新包之前；任一目录失败即拒）----
        data_backup = None
        if state0["installed"] and state0["debuggable"] and not args.allow_data_loss:
            backup_started = True            # 备份没返回 = 还原链未证 → 收局必留置
            data_backup = backup_data(d, pkg, backup_dir, left=backup_left)
            nfiles = sum(len(v["files"]) for v in data_backup["dirs"].values())
            manifest["data_backup"] = {"dir": str(backup_dir), "dirs": len(data_backup["dirs"]),
                                       "files": nfiles,
                                       "tars": {k: v["tar_sha256"] for k, v in data_backup["dirs"].items()}}
            log(f"已备份原包数据 {len(data_backup['dirs'])} 处目录 / {nfiles} 文件 → {backup_dir}")
        manifest["restore_scope"] = {
            "apk": ("已装不同构建→备份后 install -r 装回并 sha256 复核（分包拒跑；"
                    "装回方向降级已在入局前拒跑）") if state0["installed"]
                   else "未安装→收局卸载并复核 pm path 已清",
            "private_data": ("debuggable→run-as tar 备份至 data-backup/，收局归档核对过才 "
                             "pm clear+回灌+设备端全量复核"
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
                tmp = deploy_work / "pack"          # 中间产物落在本次 out/deploy 下
                tmp.mkdir()
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
            if args.pack:
                try:
                    shutil.rmtree(tmp)      # pack 中间产物是再生材料，部署完即清
                except OSError as error:    # 清不掉照走：留证记注，不当中断
                    notes.append(f"pack 中间产物 {tmp} 清理失败：{error}")
        elif not state0["installed"] or state0["sha256"] != our_sha:
            d.run("install", "-r", *(["-d"] if downgrade_fwd else []), str(args.apk), timeout=600)
            log("已安装测试包" + ("（install -d 降级）" if downgrade_fwd else ""))
        cur = install_state(d, pkg)
        if not cur["installed"] or cur.get("sha256") != our_sha:
            raise Refuse("安装后 sha256 校验不过")

        # ---- 检查点：部署后、首启前解进存档根，核对不过不起局 ----
        if args.checkpoint:
            apply_checkpoint(d, pkg, args.checkpoint, manifest, log, out)

        # ---- 调试门 + 帧日志开关（记原值，收局恢复并逐项读回；release 机不让改只记注）----
        run_uuid = uuid.uuid4().hex   # 本局号：os.c 首行/帧 meta 都写它——内容级归属锚
        for key in DEBUG_PROPS:
            props_prev[key] = d.shell(f"getprop {key}", allowed=(0, 1)).strip()
            d.shell(f"setprop {key} 1", allowed=(0, 1))
        for key, val in ab_values:
            # 原值 / 置位 / 读回都要 rc=0：读不到原值就无法证明能恢复，直接拒跑
            props_prev[key] = d.shell(f"getprop {key}", allowed=(0,)).strip()
            ab_strict.add(key)     # 先登记、再 setprop：setprop 生效后读回失败，恢复也按严格口径
            d.shell(f"setprop {key} {shlex.quote(val)}", allowed=(0,))
            got = d.shell(f"getprop {key}", allowed=(0,)).strip()
            if got != val:
                raise Refuse(f"--ab-prop {key} 没置上（读回 {got!r}）")
            manifest.setdefault("ab_props", {})[key] = {"value": val, "prev": props_prev[key]}
        props_prev[SESSION_PROP] = d.shell(f"getprop {SESSION_PROP}", allowed=(0, 1)).strip()
        d.shell(f"setprop {SESSION_PROP} {run_uuid}", allowed=(0, 1))
        if d.shell(f"getprop {SESSION_PROP}", allowed=(0, 1)).strip() != run_uuid:
            notes.append(f"{SESSION_PROP} 没置上：私有证据本局锚缺，归属一律 unproven")
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
        ready_uuid = None
        while time.monotonic() < deadline:
            text = d.shell(f"logcat -d -v brief -T {shlex.quote(since)} -s S2Arm:I")
            if READY in text:
                ready = True
                for ln in text.splitlines():
                    if READY in ln:  # 「… session <UUID> …」= 本局设备生成的会话号
                        m_ = re.search(r"\bsession (\S+)", ln)
                        ready_uuid = m_.group(1) if m_ else None
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
        # ---- birth_ticks 折成墙钟：私有日志按 mtime ≥ birth_wall 绑到本次进程出生 ----
        pids = session_pids(d, pkg)
        births = {pid: pid_birth(d, pid) for pid in pids}
        wall_now, uptime_now = device_timebase(d)
        birth_walls = {pid: wall_now - max(0.0, uptime_now - ticks / USER_HZ)
                       for pid, ticks in births.items()
                       if ticks is not None and wall_now is not None and uptime_now is not None}
        manifest["session"] = {"pids": {str(pid): {"name": name,
                                                 "birth_ticks": births[pid],
                                                 "birth_wall_s": birth_walls.get(pid)}
                                        for pid, name in pids.items()},
                               "ready_line": READY, "uuid": ready_uuid, "run_uuid": run_uuid}
        if not pids:
            notes.append("ps 里没找到当局游戏进程，帧日志无法按 PID 绑定（帧表只留表头）")

        # ---- 按时长采集：窗口边界取设备 /proc/uptime（与帧记录同一 CLOCK_BOOTTIME）----
        # 电池采样走设备侧守护：1Hz 写 /data/local/tmp 私有文件（=== 分块），host 按
        # --power-interval 拉回——adb 中途掉线只断拉取不断设备上的采样，收局/中断照样
        # 把已落的行拉回来（兄弟项目「私存不丢」同语义）。
        supply = "battery"
        dest = POWER_LOG
        settle_s = 30 if args.discharge_settle is None else args.discharge_settle
        if args.no_power_sampler is not None:
            # --no-power-sampler：接电功能局/帧时性能局不要能耗读数——连守护都
            # 不起（也就没有孤儿可留）；门没跑不写 settle_ok，能耗如实没数据。
            manifest["power_sampler"] = {
                "disabled": True,
                "reason": args.no_power_sampler or "调用方声明本局不需要能耗数据"}
        else:
            try:
                dest, sampler_pid = start_power_sampler(d, supply)
                sampler = (dest, sampler_pid)
                manifest["power_sampler"] = {"dest": dest, "pid": sampler_pid,
                                             "settle_required_s": settle_s}
                power_f = (out / "power.tsv").open("w")
                power_f.write("t\t" + "\t".join(POWER_FIELDS) + "\tlevel\n")
            except (RuntimeError, subprocess.SubprocessError) as error:
                # 起守护失败/超时（adb shell 卡到超时才炸）：设备端可能已有孤儿
                # 循环在写日志——sampler 变量还没赋值，按 PID 文件/进程标记扫掉。
                try:
                    stop_power_sampler(d, dest, None)
                except (RuntimeError, subprocess.SubprocessError) as sweep_error:
                    notes.append(f"采样守护起失败后的孤儿清理也没走通：{sweep_error}")
                notes.append(f"电池采样守护起不来（{str(error)[:160]}）：本局能耗按没数据处理")
                manifest["power_sampler"] = {"spawn_error": str(error)[:200]}
            except BaseException:
                # 启动中断（SIGINT/KeyboardInterrupt 等在 d.shell 里炸的异常）：
                # 守护可能已起——也照扫孤儿再放行，不能只靠收局兜底
                try:
                    stop_power_sampler(d, dest, None)
                except Exception:
                    pass
                raise

        def flush_power(rows):
            for row, level in rows:
                boot = row.get("boot_us")
                if boot is None or boot in seen_boots:
                    continue
                seen_boots.add(boot)
                power_f.write("\t".join([str(len(seen_boots) - 1)]
                                        + [str(row[k]) for k in POWER_FIELDS]
                                        + [level]) + "\n")
            power_f.flush()

        def pull_rows(tag):
            if sampler is None:
                return []
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
        # 接电功能局默认不该先等放电：未显式给 --discharge-settle 且最新采样
        # 证在接电/充电 → 门判不适用直接进窗（显式给了照等，能耗局要真放电）。
        settle_us = settle_s * 1_000_000
        wait0 = time.monotonic()
        settled = True
        if settle_us > 0 and sampler is not None:
            rows = pull_rows("放电稳定门")
            if args.discharge_settle is None:
                # 首读竞态（8E：setsid 即回但守护首块还没落盘）——先补拉几轮，
                # 仍无样本就用同口径 dumpsys 现态判接电；接电局不许被首读空
                # 拖进整条放电等待。
                first = rows[-1] if rows else None
                for _ in range(3):
                    if first is not None:
                        break
                    time.sleep(args.power_interval)
                    rows = pull_rows("放电稳定门")
                    first = rows[-1] if rows else None
                if first is None:
                    try:
                        first = power_parse_sample(
                            d.shell(power_sample_command(supply),
                                    timeout=20) + "\n")
                    except (RuntimeError, subprocess.SubprocessError) as error:
                        notes.append(f"放电稳定门首读无样本、dumpsys 现态判读也没走通：{error}")
                charging = bool(first) and (
                    str(first.get("plugged")) == "1"
                    or first.get("status") in ("Charging", "Full"))
            else:
                charging = False
            if charging:
                manifest["power_sampler"]["settle_na"] = \
                    "设备在接电/充电：放电稳定门不适用，直接进窗"
            else:
                settled = bool(rows) and discharge_streak_us(rows) >= settle_us
                while not settled:
                    if args.discharge_timeout and time.monotonic() - wait0 > args.discharge_timeout:
                        notes.append(f"放电未稳满 {settle_s}s 就开采（{args.discharge_timeout}s 内没等到，能耗按没数据处理）")
                        break
                    time.sleep(args.power_interval)
                    rows = pull_rows("放电稳定门")
                    settled = bool(rows) and discharge_streak_us(rows) >= settle_us
        if ("power_sampler" in manifest
                and "spawn_error" not in manifest["power_sampler"]
                and "disabled" not in manifest["power_sampler"]):
            manifest["power_sampler"]["settle_ok"] = settled
            manifest["power_sampler"]["waited_s"] = round(time.monotonic() - wait0, 1)

        col0_us = uptime_us(d)
        shot(adb_argv, out / "shots_ap" / "01_000s.png")
        for i in range(args.seconds):
            tick = time.monotonic()
            pull_rows(f"第 {i}s")
            time.sleep(max(0, args.power_interval - (time.monotonic() - tick)))
        col1_us = uptime_us(d)
        if sampler is not None:
            try:
                rows = stop_power_sampler(d, dest, sampler[1])
                sampler = None
                flush_power(rows)
            except (RuntimeError, subprocess.SubprocessError) as error:
                notes.append(f"收采样守护失败（杀/拉/清理）：{error}")
        if power_f is not None:
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

        # ---- 私有证据（还原清私有目录之前拉回）：native 直写 frame-log-<pid>.log ----
        # ---- 是 logcat 转储被截断时的完整源；host-*-<pid>-*.log 是当局宿主日志 ----
        priv_records, priv_frames = collect_private_logs(
            d, pkg, pids, out, notes, birth_walls=birth_walls,
            ready_timebase=(wall_now, uptime_now), session_uuid=run_uuid)
        manifest["private_logs"] = priv_records
        priv_done = True

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

        # 本局实际走的图形路径：只认本局 PID 的「graphics selection:」行——
        # --mode 传的只是 intent，intent 下去后哪条路真跑了要拿运行器自己的行对。
        # stream 与 dump 是同一事件的两份副本：按行文本去重后并集。
        sel_lines, sel_seen = [], set()
        for ln in graphics_selection_lines(logcat, pids) + graphics_selection_lines(dump, pids):
            if ln not in sel_seen:
                sel_seen.add(ln)
                sel_lines.append(ln)
        sel_modes = {sm.group(1) for ln in sel_lines
                     for sm in [GFX_SEL_MODE.search(ln)] if sm}
        manifest["graphics_selection"] = {
            "requested": args.mode,
            "actual": sel_modes.pop() if len(sel_modes) == 1 else None,
            "lines": sel_lines}

        direct = "direct provider prepared" in (logcat + dump)
        caps, seen = [], set()

        def collect_caps(text, src_pid=None, **kw):
            for block in frame_log_blocks(text, **kw):
                bpid = src_pid
                if bpid is None:
                    m = THREADTIME_PID.match(block)
                    bpid = int(m.group(1)) if m else None
                mu = FRAME_UUID.search(block)
                if not mu or not run_uuid or mu.group(1) != run_uuid:
                    notes.append(f"帧日志块本局 uuid 对不上（{mu.group(1) if mu else '字段缺'} "
                                 f"≠ {run_uuid or '本局未生成'}）：不证属本局，整段不收")
                    continue
                # end 行的载荷 sha（parser 已验过与分片实算一致）做去重内容键：
                # logcat / dump / 私有文件是同一进程同一转储的同文复制才算重复——
                # 同计数不同载荷（另一进程重转 / 别局改过的块）两份都保留
                sm = re.search(r"sha256=([0-9a-fA-F]+)", block)
                sha = sm.group(1).lower() if sm else None
                try:
                    for cap in direct_framelog.parse(block):
                        key = (bpid, cap["meta"]["first"], cap["meta"]["recorded"],
                               len(cap["ts"]), sha)
                        if key not in seen:
                            seen.add(key)
                            caps.append(cap)
                except direct_framelog.FrameLogError as error:
                    notes.append(f"帧日志块截断/不完整被弃（整段未证）：{str(error)[:500]}")

        if pids:
            for text in (logcat, dump):
                collect_caps(text, pids=pids)
            for fpid, text in priv_frames:  # 私有帧文件按文件名绑 PID（行无 logcat 前缀）
                collect_caps(text, src_pid=fpid)
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
            "apk_sha256": our_sha, "graphics_mode": args.mode,
            "graphics_selection": manifest["graphics_selection"], "extras": args.extra,
            "frame_log": manifest["frame_log"], "session": manifest["session"],
            "frame_window_us": manifest["frame_window_us"], "ready": ready,
            "power_sampler": manifest.get("power_sampler"),
            "restore_scope": manifest["restore_scope"],
        }, ensure_ascii=False, indent=1) + "\n")
        # 断言本局实际图形路径（迟到处：产物都落盘后再判，红局也留完整证据）：
        # 缺行（v1 会话不打选择行 / stderr 没进 logcat）、解析不出唯一模式、
        # 与 --mode 不符——都是 RESULT=red；auto 只记账不比值。
        sel = manifest["graphics_selection"]
        if not sel["lines"]:
            raise Refuse("本局 PID 的 logcat 没有「graphics selection:」行：实际走的图形路径未证")
        if sel["actual"] is None:
            raise Refuse("「graphics selection:」行解析不出唯一模式："
                         f"{sel['lines'][0].strip()[:160]}")
        if args.mode != "auto" and sel["actual"] != args.mode:
            raise Refuse(f"graphics selection 与 --mode 不符：请求 {args.mode}、"
                         f"本局实际 {sel['actual']}（{sel['lines'][0].strip()[:160]}）")
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
        ck_tmp = manifest.get("checkpoint", {}).get("tmp_left")
        if ck_tmp:
            fails.append(f"检查点临时文件没删掉：{ck_tmp}"
                         f"（{manifest['checkpoint'].get('tmp_cleanup_error', '')}）——"
                         f"/data/local/tmp 有残留，设备状态未证")
        for aux in backup_left:
            fails.append(f"备份凭据 {aux} 残留没核删干净：/data/local/tmp 有本局改动，"
                         f"设备状态未证")
        if backup_started and data_backup is None:
            fails.append("数据备份中途失败：还原链未证，不得正常放锁")
        if sampler:
            try:  # 中断路径同样收采样守护：杀循环 → 拉回设备上已落的行 → 清理文件
                for row, level in stop_power_sampler(d, sampler[0], sampler[1]):
                    boot = row.get("boot_us")
                    if boot is None or boot in seen_boots or power_f is None:
                        continue
                    seen_boots.add(boot)
                    power_f.write("\t".join([str(len(seen_boots) - 1)]
                                            + [str(row[k]) for k in POWER_FIELDS]
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
        if launched and not priv_done:
            try:  # 中断路径同样在还原（pm clear/卸载清私有目录）前把私有证据拉回
                manifest["private_logs"], _ = collect_private_logs(
                    d, pkg, pids, out, notes,
                    birth_walls=birth_walls,
                    ready_timebase=(wall_now, uptime_now),
                    session_uuid=run_uuid)
                priv_done = True
            except (RuntimeError, subprocess.SubprocessError, OSError) as error:
                notes.append(f"私有日志拉取异常（证据缺不算还原失败）：{error}")
        try:
            ab_keys = ab_strict | set(manifest.get("ab_props", {}))
            for key, prev in props_prev.items():
                # --ab-prop 的属性：setprop 与读回都必须 rc=0（权限拒绝 rc=1 会读成空串，
                # 原值恰为空时会被误认已恢复）；逐项单独 try，一项失败不跳过其余项
                ok_rc = (0,) if key in ab_keys else (0, 1)
                try:
                    d.shell(f"setprop {key} {shlex.quote(prev)}", allowed=ok_rc)
                    back = d.shell(f"getprop {key}", allowed=ok_rc).strip()
                except (RuntimeError, subprocess.SubprocessError) as error:
                    fails.append(f"调试属性 {key} 恢复 adb 异常：{error}")
                    continue
                if back != prev:
                    fails.append(f"调试属性 {key} 恢复失败：期望 {prev!r} 实得 {back!r}")
        except (RuntimeError, subprocess.SubprocessError) as error:
            fails.append(f"调试属性恢复 adb 异常：{error}")
        if state0 is not None:
            try:
                fails += restore_state(d, manifest.get("package", args.package or ""), state0,
                                       orig_tmp, data_backup=data_backup, backup_dir=backup_dir)
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
        # 原包备份：所有还原核验（进程停验/属性恢复/APK+数据还原/采集进程停）全过
        # 才算可弃——任一环节没收好都留下供人工救；清理失败记账不中断后续收尾
        if orig_tmp is not None and orig_tmp.exists():
            if fails:
                manifest["orig_apk_backup"] = str(orig_tmp)
                notes.append(f"还原未全过：原包备份留 {orig_tmp} 供人工恢复")
            else:
                try:
                    orig_tmp.unlink()
                except OSError as error:
                    manifest["orig_apk_backup"] = str(orig_tmp)
                    notes.append(f"原包备份清理失败：{error}（{orig_tmp} 留在 out）")
                else:
                    try:
                        orig_tmp.parent.rmdir()
                    except OSError as error:
                        manifest["orig_backup_dir"] = str(orig_tmp.parent)
                        notes.append(f"orig.apk 已清，orig-backup 目录留存（{error}）")
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
