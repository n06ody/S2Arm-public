#!/usr/bin/env python3
"""android/tools/run_game.py 宿主单测（adb 替身，不碰真机；模拟真实平台行为）：
  ① 空机起局：记录原状（未安装）→ 安装 → 冷启动 → READY → 按时长采集 → 收局 → 卸载还原；
    局目录有 tools/quality_check.py 安卓分支要的 android.json / shots_ap / frames.tsv / power.tsv / logcat.txt
  ② 帧日志 → frames.tsv：只写采集窗口内的帧（boot_us 同时钟裁窗），块先按 logcat 行 PID
    圈进程、再按 meta uuid= 硬绑本局（缺/不符整段不收），manifest.frame_log.binding 记账
  ③ 设备已装该包：拒跑（私有数据保证不了还原）；--allow-data-loss 才放行且只还原 APK 字节
  ④ 还原失败：uninstall 失败 → RESULT=red、设备被留置
  ⑤ --mode direct：launch 行带 --es s2arm.graphics_mode direct
  ⑥ SIGTERM：采集中途被 TERM → finally 照常收局还原（卸载），不留置
  ⑦ 调试属性恢复读回不符 → 留置；游戏收局后仍存活 → 留置；卸载后 data/obb 有残留 → 留置
  ⑧ 能耗：放电状态/符号/覆盖按 run_game 功耗段口径——合法放电 →「未证」（不套 iPhone 门槛假绿），
    接电 / 未知 / 符号不一致 / 窗口不齐 →「没数据」
  ⑨ 已装 debuggable 包：run-as tar 全量备份私有/外部数据（文件 sha/软链/空目录/权限清单）
    → 收局装回原包再逐项核还原；非 debuggable 已装 → 拒跑（--allow-data-loss 才放行）
  ⑩ 降级：原包 > 新包 → 装测试包本身是真降级由 install -r -d 实测（失败不卸旧包）；
    原包 < 新包 → 装回方向是降级，入局前证不了能装回就拒跑（不拉不装不动数据）；
    替身按 versionCode 排序区分同版/真降级
  ⑪ 还原后数据 sha 与备份清单不符 → 留置
  ㉕ 数据保护硬门：目录三态探针（存在但进不去 = 报错拒跑，不当不存在）；备份/回灌两处
    tar 解包逐项核对（文件 sha/软链目标/目录/权限），pm clear 排在归档核对之后且
    必须 rc=0+Success；推回途中篡改 → 设备端复核逮住 → 留置；卸载后外部目录读不进 → 留置
  ㉖ 私有证据本局锚：文件内容里的 session_uuid——运行器起局前 setprop
    debug.s2a.session_uuid，os.c 会话日志首行 uuid= 与帧日志 meta 行尾 uuid= 都写它；
    等于本局才 bound，字段缺/值不符 → unproven。文件名 seg 只做与首行名实互核；
    stat -c %Y 整秒 vs birth 浮点判不了先后——不跳过照收证，先后由内容 uuid 裁决
"""
import base64, contextlib, hashlib, importlib.util, io, json, os, re, shlex, signal, struct, subprocess, sys, tarfile, tempfile, time, unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

ANDROID = Path(__file__).resolve().parents[1]
REPO = ANDROID.parent
spec = importlib.util.spec_from_file_location("run_game", ANDROID / "tools" / "run_game.py")
run_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_game)

PKG = "org.s2arm.game.t0123456789abcdef"
SERIAL = "TESTSERIAL01"
FIELDS = ("boot_us", "status", "current_ua", "voltage_uv", "energy_uwh", "charge_uah",
          "temp_tenths_c", "source", "ac_powered", "usb_powered", "wireless_powered",
          "dock_powered", "plugged", "interactive", "power_save", "thermal_status")


class FixtureClock:
    """只替换被测模块的等待；真实子进程、信号和推送超时仍用系统时钟。"""
    def __init__(self):
        self.offset = 0.0

    def monotonic(self):
        return time.monotonic() + self.offset

    def sleep(self, seconds):
        if seconds < 0:
            raise ValueError("sleep length must be non-negative")
        self.offset += seconds

FAKE_ADB = '''#!/usr/bin/env python3
import hashlib, io, json, os, re, shlex, sys, tarfile, time
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

DATA_DIRS = st.setdefault("data_dirs", {})          # dir -> {relpath: 内容 hex}
DATA_LINKS = st.setdefault("data_links", {})        # dir -> {relpath: 软链目标}
DATA_SUBDIRS = st.setdefault("data_subdirs", {})    # dir -> [relpath,...] 显式目录（含空目录）
DATA_MODES = st.setdefault("data_modes", {})        # dir -> {relpath: 八进制串} 权限覆盖
DATA_DENIED = st.setdefault("data_denied", {})      # dir -> True：存在但 run-as 进不去
DATA_PAD = st.setdefault("data_pad", {})            # dir -> {relpath: 字节数} 合成大文件
                                                    #（全零填充：tar 备份按声明长度真流出来，
                                                    #  测宿主是否整流驻内存；清单/sha 按合成算）
DEV_TMP = st.setdefault("dev_tmp", {})              # 树外凭据 /data/local/tmp/s2a-tar-*-{err,rc}
                                                    #（exec-out 混流后设备侧 stderr/rc 的取证通道）

def zeros_sha(n):
    # sha256(N 个零字节)：分块算不整驻内存（替身侧也守同一规矩）
    h = hashlib.sha256()
    blk = b"\\0" * (1 << 20)
    for off in range(0, n, len(blk)):
        h.update(blk[:min(len(blk), n - off)])
    return h.hexdigest()


def dir_entries(dir_):
    """目录全量成员：文件 + 显式目录 + 文件/软链隐含的父目录 + 软链。"""
    files = DATA_DIRS.get(dir_, {})
    links = DATA_LINKS.get(dir_, {})
    pads = DATA_PAD.get(dir_, {})
    dirs = set(DATA_SUBDIRS.get(dir_, []))
    for rel in list(files) + list(links) + list(pads):
        parts = rel.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            dirs.add("/".join(parts[:i]))
    modes = DATA_MODES.get(dir_, {})
    out_e = {}
    for rel in dirs:
        out_e[rel] = (modes.get(rel, "775"), "directory")
    for rel in files:
        # toybox stat %F 真口径：0 字节出 "regular empty file"（8E 实测）；
        # dev_stat_types 旋钮可把某路径报成 fifo/socket 等非普通类型（负对照）
        ftype = "regular empty file" if not files[rel] else "regular file"
        ftype = st.get("dev_stat_types", {}).get(dir_, {}).get(rel, ftype)
        out_e[rel] = (modes.get(rel, "664"), ftype)
    for rel in pads:
        ftype = st.get("dev_stat_types", {}).get(dir_, {}).get(rel, "regular file")
        out_e[rel] = (modes.get(rel, "664"), ftype)
    for rel in links:
        out_e[rel] = (modes.get(rel, "777"), "symbolic link")
    return out_e
PUSHED = st.setdefault("pushed", {})                # 设备路径 -> 本机路径（adb push 单文件）
DEV_FS = st.setdefault("dev_fs", {})                # 设备绝对路径 -> 内容 hex（push 目录/tar 解开/mv/rm 都动它）

def dev_put_dir(local, dev):
    # adb push <dir> <dev>：本地整树逐文件登记；push_slow_s 让进度轮询能看到中间态
    import os as _os, time as _t
    items = []
    for root_, _, files_ in _os.walk(local):
        for fn in files_:
            lp = _os.path.join(root_, fn)
            rel = _os.path.relpath(lp, local).replace(_os.sep, "/")
            items.append((rel, open(lp, "rb").read().hex()))
    delay = float(st.get("push_slow_s") or 0)
    half = len(items) // 2 if delay else len(items)
    for rel, hx in items[:half]:
        DEV_FS[dev.rstrip("/") + "/" + rel] = hx
    save()
    if delay:
        _t.sleep(delay)
    for rel, hx in items[half:]:
        DEV_FS[dev.rstrip("/") + "/" + rel] = hx
    save()

def file_sha(b):
    return hashlib.sha256(b).hexdigest()

def sha_listing(dir_):
    # find . -type f 的输出口径：只收普通文件——dev_stat_types 报成
    # fifo/socket 的不出（真机 -type f 同口径）
    types = st.get("dev_stat_types", {}).get(dir_, {})
    lines = []
    for rel, hx in sorted(DATA_DIRS.get(dir_, {}).items()):
        if types.get(rel, "regular file").startswith("regular"):
            lines.append(f"{file_sha(bytes.fromhex(hx))}  ./{rel}")
    for rel, n in sorted(DATA_PAD.get(dir_, {}).items()):
        if types.get(rel, "regular file").startswith("regular"):
            lines.append(f"{zeros_sha(n)}  ./{rel}")
    return "\\n".join(lines) + ("\\n" if lines else "")

def run_as(cmdline, via_shell=False):
    # run-as <pkg> sh -c '<cmdline>'：应用 uid 访问自己的数据目录；
    # via_shell=True = 同一组命令但走 shell 通道（外部数据目录：9000 实测
    # run-as 摸 /sdcard/Android/* 被 SELinux 拒，shell 有外部存储全权）
    import re as _re, fnmatch as _fm
    if not via_shell:
        if not st.get("apk_debuggable", True):
            fail("run-as: package not debuggable")    # 已装包非 debuggable：run-as 直接拒
        if st.get("run_as_ext_denied") and "/sdcard/" in cmdline:
            fail("run-as: " + cmdline + ": Permission denied")   # 9000 SELinux 拒
    m = _re.match(r"^find (\\S+) -type f -name '([\\w.*-]+)'$", cmdline)
    if m:
        # 私有证据收集：列 <root> 下 basename 匹配（含子目录 rel 路径）
        root_, pat_ = m.group(1).rstrip("/"), m.group(2)
        for rel in sorted(DATA_DIRS.get(root_, {})):
            if _fm.fnmatch(rel.rsplit("/", 1)[-1], pat_):
                out(root_ + "/" + rel + "\\n")
        return
    m = _re.match(r"^cat (\\S+)$", cmdline)
    if m:
        p = m.group(1)          # exec-out 收私有日志：DATA_DIRS 原样吐字节（含 NUL）
        for root_, files_ in DATA_DIRS.items():
            if p.startswith(root_.rstrip("/") + "/"):
                rel = p[len(root_.rstrip("/")) + 1:]
                if rel in files_:
                    data = bytes.fromhex(files_[rel])
                    if st.get("priv_empty"):
                        data = b""                             # 空传输：rc=0 但零字节
                    elif st.get("priv_truncate"):
                        data = data[:max(0, len(data) // 2)]   # 截断传输：rc=0 但短读
                    sys.stdout.buffer.write(data); return
        fail("cat: " + p + ": No such file or directory")
    m = _re.match(r"^stat -c %Y (\\S+)$", cmdline)
    if m:
        if st.get("priv_stat_denied"):       # 归属证据读不了：stat 整条被拒
            fail("stat: " + m.group(1) + ": Permission denied")
        p = m.group(1)          # 文件 mtime（秒）：data_mtime 旋钮可给旧局文件配老时间
        for root_, files_ in DATA_DIRS.items():
            if p.startswith(root_.rstrip("/") + "/") and \
                    p[len(root_.rstrip("/")) + 1:] in files_:
                # mtime 缺省 = 当前墙钟（随 uptime 推进，与 date +%s 同式）
                wall = st["wall_s"] + (st["uptime_us"] - st["wall_ref_us"]) // 1_000_000
                if st.get("wall_jump") and st["uptime_us"] > 20_000_000:
                    wall += st["wall_jump"]
                out(str(st.get("data_mtime", {}).get(p, wall)) + "\\n"); return
        fail("stat: " + p + ": No such file or directory")
    m = _re.match(r"^sha256sum (\\S+); wc -c < (\\S+)$", cmdline)
    if m:
        p = m.group(1)          # 源端字节/SHA 记账：拉取校验的对照值
        for root_, files_ in DATA_DIRS.items():
            if p.startswith(root_.rstrip("/") + "/"):
                rel = p[len(root_.rstrip("/")) + 1:]
                if rel in files_:
                    blob = bytes.fromhex(files_[rel])
                    out(f"{file_sha(blob)}  {p}\\n{len(blob)}\\n"); return
        fail("sha256sum: " + p + ": No such file or directory")
    m = _re.match(r"^if \\[ -e (\\S+) \\]; then echo ok; else echo absent; fi$", cmdline)
    if m:
        # [ -e ] 真义：父目录不可搜索时同样返回假——和「不存在」分不开（负对照用）
        dir_ = m.group(1)
        if dir_ in st.get("data_parent_denied", {}) or dir_ in DATA_DENIED:
            out("absent\\n"); return
        out(("ok" if dir_entries(dir_) or dir_ in DATA_DIRS else "absent") + "\\n"); return
    m = _re.match(r"^e=\\$\\(stat -c %F (\\S+) 2>&1\\); if", cmdline)
    if m:
        # 三态探针（stat 接 stderr 分 EACCES/ENOENT）：父目录不可搜索 → stat 拒 → denied；
        # 自身进不去 → denied；真没有 → absent
        dir_ = m.group(1)
        if dir_ in st.get("data_parent_denied", {}):
            out("denied\\n"); return
        if dir_ in DATA_DENIED:
            out("denied\\n"); return
        out(("ok" if dir_entries(dir_) or dir_ in DATA_DIRS or dir_ in DATA_LINKS
             or dir_ in DATA_SUBDIRS else "absent") + "\\n"); return
    def _emit_batched(dir_, lines, want):
        # 分批清单收尾凭据：S2A_N/S2A_CRC 计数与计数管道 rc、S2A_FRC find 自报 rc、
        # S2A_RC 管道 rc、S2A_ERR: 前缀 stderr 行——旋钮逐门复现设备残缺态
        n = int(st.get("data_listing_short", {}).get(dir_, 0))
        if n: lines = lines[:-n]              # xargs rc=0 但某批输出丢了 → 条数不符
        frc = rc = crc = "0"
        if st.get("data_xargs_fail"):
            lines, rc = lines[:len(lines) // 2], "126"   # 中途批次挂：半截 + 非零 rc
        err_lines = list(st.get("data_listing_err", {}).get(dir_, []))
        fs = st.get("data_find_fail", {}).get(dir_)
        if fs:
            # find 中途被拒：pipefail 下管道 rc 传 find rc；masked 变体模拟
            # 管道 rc 仍是 0（xargs 吞了上游失败）——那时只有 S2A_FRC 一门能逮
            frc = str(fs.get("rc", 1))
            if not fs.get("masked"): rc = crc = frc
            lines = lines[:1]
            err_lines += [fs.get("err", "find: Permission denied")]
        mangle = st.get("data_listing_mangle", {}).get(dir_) if st.get("pm_cleared") else None
        n_str = str(want)
        if mangle == "empty_n": n_str = ""
        if mangle == "bad_n": n_str = "abc"
        if mangle == "bad_mode":
            lines = [("zzz" + l[l.index("|"):]) if "|" in l else l for l in lines]
        if st.get("data_xargs_phantom", {}).get(dir_):
            # xargs 无 -r 空输入仍跑一次：sha256sum 空参读 stdin 出幻影行
            lines += ["e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  -"]
        out("\\n".join(lines) + ("\\n" if lines else ""))
        out(f"S2A_N={n_str} S2A_CRC={crc}\\nS2A_FRC={frc}\\nS2A_RC={rc}\\n")
        for l_ in err_lines: out(f"S2A_ERR: {l_}\\n")
        return
    m = _re.match(r"^cd (\\S+) && \\{ set -o pipefail; exec 3>&1; err=", cmdline)
    if m:
        dir_ = m.group(1)
        if "find . -type f -print0" in cmdline:
            want = sum(1 for ft in (e[1] for e in dir_entries(dir_).values())
                       if ft.startswith("regular"))
            _emit_batched(dir_, sha_listing(dir_).splitlines(), want); return
        if "find . -mindepth 0 -print0" in cmdline:
            root_mode = st.get("data_root_mode", {}).get(dir_, "775")
            lines = [f"{root_mode}|directory|."] + [
                f"{mode}|{ftype}|./{rel}"
                for rel, (mode, ftype) in sorted(dir_entries(dir_).items())]
            _emit_batched(dir_, lines, len(dir_entries(dir_)) + 1); return
        if "find . -type l -print0" in cmdline:
            lines = [f"./{rel}\\t{tgt}"
                     for rel, tgt in sorted(DATA_LINKS.get(dir_, {}).items())]
            _emit_batched(dir_, lines, len(lines)); return
    m = _re.match(r"^cd (\\S+) && tar -cpf - \\. 2> (\\S+); echo \\$\\? > (\\S+)$", cmdline)
    if m:
        dir_, en, rn = m.group(1), m.group(2), m.group(3)
        if dir_ in DATA_DENIED:
            # run-as 拒权：归档零字节流出，stderr/rc 落凭据文件由宿主另行取证
            DEV_TMP[en] = "tar: .: Permission denied\\n"
            DEV_TMP[rn] = "1\\n"
            save(); return
        if st.get("data_stream_hang"):
            time.sleep(int(st.get("data_stream_hang_s", 30)))   # 流中途挂：宿主超时杀
            return
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tf:
            # 真机 toybox `tar -cf - -C dir .` 首成员是 ./（8E 实测）——替身同款
            root_ = tarfile.TarInfo("./")
            root_.type, root_.mode = tarfile.DIRTYPE, 0o775
            tf.addfile(root_)
            for rel, (mode, ftype) in sorted(dir_entries(dir_).items()):
                info = tarfile.TarInfo("./" + rel)
                info.mode = int(mode, 8)
                if ftype == "regular file":
                    if rel in DATA_PAD.get(dir_, {}):
                        info.size = DATA_PAD[dir_][rel]
                        tf.addfile(info, io.BytesIO(b"\\0" * info.size))
                    else:
                        blob = bytes.fromhex(DATA_DIRS[dir_][rel])
                        info.size = len(blob)
                        tf.addfile(info, io.BytesIO(blob))
                elif ftype == "directory":
                    info.type = tarfile.DIRTYPE
                    tf.addfile(info)
                elif ftype == "symbolic link":
                    info.type = tarfile.SYMTYPE
                    info.linkname = DATA_LINKS[dir_][rel]
                    tf.addfile(info)
            if st.get("corrupt_backup_tar"):
                # 备份核对门负控：归档比源清单多出成员 → 解包核对不过 → 拒跑
                blob = b"planted"
                info = tarfile.TarInfo("./planted.evil")
                info.size = len(blob)
                tf.addfile(info, io.BytesIO(blob))
        # 设备侧 stderr/rc 凭据：exec-out 把 tar stderr 混进 stdout 归档流，
        # 凭据落 /data/local/tmp/s2a-tar-<tag>-<uuid>-{err,rc}（被备份树之外）
        DEV_TMP[en] = st.get("data_tar_err", {}).get(dir_, "")
        DEV_TMP[rn] = st.get("data_tar_rc", {}).get(dir_, "0") + "\\n"
        sys.stdout.buffer.write(buf.getvalue()); save(); return
    m = _re.match(r"^(?:mkdir -p (\\S+); )?find (\\S+) -mindepth 1 -delete$", cmdline)
    if m:
        if m.group(1): DATA_DIRS.setdefault(m.group(1), {})
        for tab in (DATA_DIRS, DATA_LINKS, DATA_SUBDIRS, DATA_MODES, DATA_PAD):
            tab[m.group(2)] = {} if tab is not DATA_SUBDIRS else []
        save(); return
    m = _re.match(r"^mkdir -p (\\S+)$", cmdline)      # 检查点：建存档根（含父链）
    if m:
        DATA_DIRS.setdefault(m.group(1), {}); save(); return
    m = _re.match(r"^tar -xpf (\\S+) -C (\\S+)$", cmdline)
    if m:
        src = PUSHED.get(m.group(1))
        if not src: fail("run-as: tar 源没 push 过 " + m.group(1))
        dir_ = m.group(2)
        DATA_DIRS.setdefault(dir_, {})
        DATA_LINKS.setdefault(dir_, {})
        DATA_SUBDIRS.setdefault(dir_, [])
        with tarfile.open(src) as tf:
            for mem in tf.getmembers():
                rel = mem.name[2:] if mem.name.startswith("./") else mem.name
                if rel in ("", "."):  # ./ 根成员：find -mindepth 1 不出它，不登记
                    continue
                DATA_MODES.setdefault(dir_, {})[rel] = format(mem.mode & 0o7777, "o")
                if mem.isfile():
                    if mem.size >= (1 << 20):
                        # 大成员按合成档登记（与 data_pad 同口径：不驻 hex）
                        DATA_PAD.setdefault(dir_, {})[rel] = mem.size
                        continue
                    blob = tf.extractfile(mem).read()
                    if st.get("corrupt_restore") and len(DATA_DIRS[dir_]) == 0:
                        blob = blob + b"x"    # 还原第一个文件就被改写 → sha 对不上
                    DATA_DIRS[dir_][rel] = blob.hex()
                elif mem.issym():
                    DATA_LINKS[dir_][rel] = mem.linkname
                elif mem.isdir() and rel not in DATA_SUBDIRS[dir_]:
                    DATA_SUBDIRS[dir_].append(rel)
        save(); return
    fail("fake adb: unhandled run-as " + cmdline)

def shell_data_cmd(text):
    """外部数据目录 shell 通道分流（9000 SELinux 拒 run-as 摸 /sdcard）：
    命中才接管给 run_as 同组处理器——tar -xpf/find -delete/cd&&find 只认
    「目标目录已注册进 data_dirs」的（deploy 的 tar/find 落 DEV_FS，不接管）；
    探针按 "echo ok" 圈 _dir_probe 形（__denied__/ls 残留探针不接管）。"""
    if re.match(r"^e=\\$\\(stat ", text):
        return "echo ok" in text
    m = re.match(r"^cd (\\S+) && ", text) or \
        re.match(r"^tar -xpf \\S+ -C (\\S+)$", text) or \
        re.match(r"^(?:mkdir -p \\S+; )?find (\\S+) -mindepth 1 -delete$", text)
    return bool(m) and m.group(1).rstrip("/") in DATA_DIRS

cmd = argv[0]
if cmd == "get-state":
    out("device"); sys.exit(0)
if cmd == "install":
    apk = argv[-1]
    if st.get("fail_install"): fail("Failure [INSTALL_FAILED]")
    # 真 Android versionCode 语义（qa-fone #14 替身对齐）：apk_vc < 已装 vc 是
    # 真降级——不带 -d 直接拒；带 -d 才放行，pm_no_downgrade = 设备连 -d 都不放。
    # 同版/升级装与 -d 无关（同版 install -r -d 成功证不了降级能力）。
    blob = open(apk, "rb").read()
    apk_vc = st["orig_vc"] if blob == bytes.fromhex(st["orig_apk_hex"]) \
        else st.get("test_apk_vc", st["orig_vc"])
    if st["installed"] and apk_vc < st.get("installed_vc", st["orig_vc"]):
        if "-d" not in argv or st.get("pm_no_downgrade"):
            fail("Failure [INSTALL_FAILED_VERSION_DOWNGRADE]")
    st["installed"] = True
    st["installed_vc"] = apk_vc
    st["installed_sha"] = hashlib.sha256(blob).hexdigest()
    st.setdefault("installs", []).append({"apk": apk, "flags": [a for a in argv[1:-1] if a.startswith("-")]})
    save(); out("Success"); sys.exit(0)
if cmd == "uninstall":
    if st.get("fail_uninstall"): fail("Failure [DELETE_FAILED_INTERNAL]")
    st["installed"] = False; st["installed_sha"] = ""; save(); out("Success"); sys.exit(0)
if cmd == "pull":
    if st.get("pull_dir"):
        os.makedirs(argv[-1], exist_ok=True)   # 负控：备份路径落成目录 → 收局 unlink 必抛 OSError
    else:
        open(argv[-1], "wb").write(bytes.fromhex(st["orig_apk_hex"]))
    if st.get("pull_stray"):
        open(argv[-1] + ".stray", "w").write("x")   # 负控：orig.apk 能删、父目录非空 rmdir 必抛
    out(f"{argv[1]}: 1 file pulled, 0 skipped\\n"); sys.exit(0)
if cmd == "push":
    if os.path.isdir(argv[1]):
        dev_put_dir(argv[1], argv[2]); sys.exit(0)
    PUSHED[argv[2]] = argv[1]
    if argv[1].endswith(".tar"):
        st["tar_pushed"] = argv[2]
        st["tar_pushed_src"] = argv[1]          # 宿主侧 tar 源：钉「中间产物在 out 下」
        if st.get("corrupt_backup_before_restore"):
            # 归档核对门负控：备份时完好、回灌前宿主机归档被塞了异物 → 检出才许 pm clear
            with tarfile.open(argv[1], "a") as tf:
                blob = b"planted"
                info = tarfile.TarInfo("./planted.evil")
                info.size = len(blob)
                tf.addfile(info, io.BytesIO(blob))
    save(); sys.exit(0)
if cmd == "exec-out":
    if len(argv) > 2 and argv[1] == "run-as":
        run_as(argv[-1]); sys.exit(0)
    if len(argv) > 3 and argv[1] == "sh" and argv[2] == "-c":
        # shell 通道 exec-out（外部数据目录 tar 流：不经 run-as）
        run_as(argv[3], via_shell=True); sys.exit(0)
    sys.stdout.buffer.write(open(st["png"], "rb").read()); sys.exit(0)
if cmd == "logcat":
    if "-d" in argv:
        out(open(st["logcat_dump"]).read()); sys.exit(0)
    out(open(st["logcat_stream"]).read()); sys.stdout.flush()
    time.sleep(60); sys.exit(0)
if cmd == "shell":
    text = argv[1]
    sh = shlex.split(text)
    if len(sh) == 1 and " " in sh[0]:
        # 真实语义：adb.shell('整串') 经 shlex.join 单引号包成一个词，
        # 设备 shell 把它当文件名 exec → rc=127（8E r5 qa-fone 实测）
        sys.stderr.write("sh: " + sh[0] + ": command not found\\n"); sys.exit(127)
    piped = False
    if sh[:2] == ["sh", "-c"] and len(sh) > 2:
        text = sh[2]; sh = shlex.split(text)
        piped = " | " in text                 # 脚本里裸 | 才是真管道；'|' 词只是参数
    if shell_data_cmd(text):
        # 外部数据目录 shell 通道：赶在 c=="tar"/"mkdir" 等按首词分发前接管
        run_as(text, via_shell=True); sys.exit(0)
    # 备份 tar 树外凭据（/data/local/tmp/s2a-tar-<tag>-<uuid>-{err,rc}，qa-fone
    # #329620/#329632）：预建先证不存在、cat 取证、rm+test 核删——全走 shell 通道
    m = re.match(r"^\\[ ! -e (\\S+) \\] && \\[ ! -e (\\S+) \\] && : > \\S+ && : > \\S+ "
                 r"&& chmod 0666 \\S+ \\S+ && test -w \\S+ && test -w \\S+$", text)
    if m:
        en, rn = m.group(1), m.group(2)
        if st.get("data_aux_preexist") or en in DEV_TMP or rn in DEV_TMP:
            sys.stderr.write("[ ! -e ] : File exists\\n"); save(); sys.exit(1)
        DEV_TMP[en] = ""; DEV_TMP[rn] = ""; save(); sys.exit(0)
    m = re.match(r"^cat (/data/local/tmp/s2a-tar-\\S+)$", text)
    if m:
        if m.group(1) not in DEV_TMP:
            fail("cat: " + m.group(1) + ": No such file or directory")
        out(DEV_TMP[m.group(1)]); sys.exit(0)
    m = re.match(r"^rm -f (/data/local/tmp/s2a-tar-\\S+) (/data/local/tmp/s2a-tar-\\S+) "
                 r"&& test ! -e \\S+ && test ! -e \\S+$", text)
    if m:
        if st.get("data_tar_aux_left"):
            sys.stderr.write("test: File exists\\n"); save(); sys.exit(1)  # 删不净
        DEV_TMP.pop(m.group(1), None); DEV_TMP.pop(m.group(2), None)
        save(); sys.exit(0)
    c = sh[0] if sh else ""
    if "s2a-power-samples.log" in text and "echo $!" in text:
        # 采样守护 spawn（setsid sh -c '循环' >日志 2>&1 </dev/null & echo $!）。
        # 真 adbd 语义：后台进程只要还有 fd 挂在传输通道上 adb shell 就不退——
        # stdin </dev/null + stderr 2>&1/2>/dev/null + stdout >日志 三个都得引走，
        # 缺一个（哪怕守护其实起了、还在写日志）通道就一直开着（8E 实测 15s 超时）。
        # 三 fd 全引但没有 setsid 也照挂：守护继承控制终端（r25 真串实证）。
        detached = ("setsid" in text and "</dev/null" in text
                    and ("2>&1" in text or "2>/dev/null" in text))
        if not detached or st.get("sampler_spawn_hang"):
            st["sampler_on"] = True; st["sampler_ticks"] = 0   # 守护其实起了（孤儿在写）
            DEV_FS.pop("/data/local/tmp/s2a-power-samples.log", None)
            DEV_FS["/data/local/tmp/s2a-power-samples.log.pid"] = "7777".encode().hex()
            save()
            time.sleep(int(st.get("sampler_hang_s", 30))); sys.exit(0)
        # 替身不真跑循环——记 sampler_on，之后每次 cat 日志推进一拍（1s/块）；
        # kill 停、rm 清（adb 掉线不丢采样的替身对应物：块已落在 DEV_FS 里）
        st["sampler_on"] = True; st["sampler_ticks"] = 0
        DEV_FS.pop("/data/local/tmp/s2a-power-samples.log", None)
        DEV_FS["/data/local/tmp/s2a-power-samples.log.pid"] = "7777".encode().hex()
        if st.get("sampler_spawn_silent"):
            # 空回执：守护起了但 adb 回包是空——spawn 未证，宿主侧要按失败扫孤儿
            save(); sys.exit(0)
        save(); out("7777\\n"); sys.exit(0)
    if "s2a-power-samples.log" in text and ("pgrep" in text or ".pid" in text) \
            and "echo $!" not in text:
        # 孤儿清理（for p in $! $(cat 日志.pid) $(pgrep -f s2a-power-sampler); do kill $p;
        # done）：守护在 → 杀掉记账（orphan_swept）；日志.pid 一并清。
        st["orphan_swept"] = st.get("sampler_on", False)
        DEV_FS.pop("/data/local/tmp/s2a-power-samples.log.pid", None)
        if st.get("sampler_linger"):
            # 杀不掉：守护还活着，.pid 照脚本已清，回 LINGER 给宿主退场复验
            save(); out("LINGER:7777\\n"); sys.exit(0)
        st["sampler_on"] = False
        save(); sys.exit(0)
    if "power_supply" in text:
        st["uptime_us"] += 1000000
        st["sampler_ticks"] = st.get("sampler_ticks", 0) + 1
        charge = st["charge_uah"] - st["sampler_ticks"] * st["charge_step"]
        save()
        out(st["power_sample"].replace("charge_counter=", f"charge_counter={charge}")
            + f"boot_us={st['uptime_us']}\\n" + "capacity=80\\n"); sys.exit(0)
    if "/proc/uptime" in text:
        out(f"{st['uptime_us'] / 1e6:.2f} 20.00\\n"); sys.exit(0)
    if "/stat" in text and "/proc/" in text:
        if st.get("no_proc_stat"):          # 出生证拿不到：/proc/<pid>/stat 读拒
            fail("cat: /proc/stat: Permission denied")
        # starttime（开机 jiffies）：出生 = 当前 uptime - birth_lag（缺省 2s 前起）
        ticks = int(st["uptime_us"] / 1e6 * 100) - st.get("birth_lag_ticks", 200)
        out("4242 (org.s2arm.game) S " + " ".join(["0"] * 18) + f" {ticks}\\n"); sys.exit(0)
    if c == "getprop":
        if len(sh) == 1:
            import fcntl
            lock_file = st.get("probe_lock_file")
            if lock_file:
                with open(lock_file, "r") as lock:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        st["getprop_lock_held"] = False
                        fcntl.flock(lock, fcntl.LOCK_UN)
                    except BlockingIOError:
                        st["getprop_lock_held"] = True
            st["getprop_before_install"] = not st["installed"] and not st.get("props_set_once")
            save()
            if st.get("getprop_all_fail"):
                fail("getprop: permission denied")
            if st.get("getprop_all_stderr"):
                sys.stderr.write("getprop: partial listing\\n")
            out(st.get("getprop_all_raw", "".join(
                f"[{k}]: [{v}]\\n" for k, v in sorted(st["props"].items())))); sys.exit(0)
        key = " ".join(sh[1:])
        # getprop_deny：指定属性读被拒（rc=1、stderr permission denied、stdout 空）；
        # getprop_deny_after_launch：只在起局之后拒（模拟收局读回失败）
        # getprop_deny_after_set：该属性被 setprop 过之后读一律被拒（setprop 生效、紧接着的读回失败）
        if (key in st.get("getprop_deny", [])
                or (key in st.get("getprop_deny_after_launch", []) and st.get("props_at_launch") is not None)
                or (key in st.get("getprop_deny_after_set", []) and key in st.get("props_set_once", []))):
            sys.stderr.write("getprop: permission denied\\n"); save(); sys.exit(1)
        out(st["props"].get(key, "") + "\\n"); sys.exit(0)
    if c == "setprop":
        # props_jam：写空值的 setprop 被设备静默拒（有的构建不让清 debug 属性）→ 旧值还在
        if st.get("props_jam") and sh[1].startswith("debug.s2a.") and (len(sh) <= 2 or not sh[2]):
            sys.exit(0)
        st.setdefault("props_set_once", []).append(sh[1])
        st["props"][sh[1]] = sh[2] if len(sh) > 2 else ""; save(); sys.exit(0)
    if c == "pm":
        if len(sh) == 1 or (len(sh) > 1 and sh[1] == "install" and "--help" in sh):
            usage = "usage: pm install [-l] [-r] [-t] [-i INSTALLER_PACKAGE_NAME] \\\\n"
            usage += "       [-d: allow version code downgrade]\\n" if not st.get("pm_no_downgrade") else ""
            out(usage); sys.exit(255)
        if len(sh) > 1 and sh[1] == "path":
            # Android 16 真机语义：未装包 rc=1、stdout/stderr 皆空；已装 rc=0 带 package: 行
            st["pm_path_calls"] = st.get("pm_path_calls", 0) + 1; save()
            if st.get("pm_path_fail_at") == st["pm_path_calls"]:
                sys.stderr.write("error: device offline\\n"); sys.exit(st.get("pm_path_fail_rc", 255))   # 查询失败：stderr + 非 0
            if st.get("pm_path_empty_at") == st["pm_path_calls"]:
                sys.exit(0)                                        # 异常：rc=0 但无任何输出
            if st.get("pm_path_warn_at") == st["pm_path_calls"]:
                sys.stderr.write("warning: pm flush pending\\n")   # 异常：rc=0 但带 stderr
            if st["installed"]:
                out("package:" + st["base_path"] + "\\n"); sys.exit(0)
            sys.exit(1)
        if len(sh) > 1 and sh[1] == "clear":
            st["pm_cleared"] = st.get("pm_cleared", 0) + 1    # 记账：pm clear 跑没跑过
            if st.get("pm_clear_fails"):
                save(); sys.stderr.write("Failed\\n"); sys.exit(1)
            if st.get("pm_clear_silent"):
                save(); sys.exit(0)                  # rc=0 但无 Success：照样算失败
            for tab in (DATA_DIRS, DATA_LINKS, DATA_SUBDIRS, DATA_MODES, DATA_PAD):
                for k in list(tab):
                    tab[k] = {} if tab is not DATA_SUBDIRS else []
            save(); out("Success"); sys.exit(0)
        if len(sh) > 1 and sh[1] == "uninstall":
            if st.get("fail_uninstall"): fail("Failure [DELETE_FAILED_INTERNAL]")
            st["installed"] = False; save(); out("Success"); sys.exit(0)
        if len(sh) > 1 and sh[1] == "list":
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
        entries = sorted(p for p in DEV_FS if p == dir_ or p.startswith(dir_ + "/"))
        if piped:
            # 有界分批核验：sh -c 'find ... -print0 | xargs -0 -n 256 <sha256sum|chmod>'
            if "chmod" in sh: sys.exit(0)
            fail_batch = st.get("sha_batch_fail_at")      # 第 N 批（256/批）挂：截断输出 + 短 stderr + rc=126
            for i, p in enumerate(entries):
                if fail_batch is not None and i >= int(fail_batch) * 256:
                    sys.stderr.write("xargs: sha256sum: " + p + ": 中途批次失败\\n"); sys.exit(126)
                out(file_sha(bytes.fromhex(DEV_FS[p])) + "  " + p + "\\n")
            sys.exit(0)
        if "-exec" in sh:
            # toybox E2BIG：-exec {} + 把全部路径摊进一次 exec，超上限就 Argument list too long
            if len(entries) > int(st.get("find_exec_max", 512)):
                sys.stderr.write("find: -exec: Argument list too long\\n"); sys.exit(126)
            if "sha256sum" in sh:
                out("".join(file_sha(bytes.fromhex(DEV_FS[p])) + "  " + p + "\\n"
                            for p in entries)); sys.exit(0)
            sys.exit(0)     # -type d -exec chmod o=x 之类：无副作用
        if "-type" in sh:
            out("".join(p + "\\n" for p in entries)); sys.exit(0)
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
                        blob = blob + b"x"    # 解开第一个文件就被改写 → SHA 核对应对不上
                    DEV_FS[dir_ + "/" + rel] = blob.hex()
        st["tar_extracted"] = len(DEV_FS); save(); sys.exit(0)
    if c == "rm":
        if st.get("checkpoint_rm_fail") and any("s2a-checkpoint" in a for a in sh[1:]):
            sys.stderr.write("rm: Permission denied\\n"); save(); sys.exit(1)
        for p in [a for a in sh[1:] if not a.startswith("-")]:
            for k in list(DEV_FS):
                if k == p.rstrip("/") or k.startswith(p.rstrip("/") + "/"): del DEV_FS[k]
            PUSHED.pop(p, None)
        save(); sys.exit(0)
    if c == "pidof":
        if st.get("die_before_ready") and st.get("running") and not st.get("_die_seen"):
            st["_die_seen"] = True; save()                       # 起过 → 下一拍就死
            out(st.get("pid", "4242") + "\\n"); sys.exit(0)
        if st.get("die_before_ready") and st.get("_die_seen"):
            st["running"] = False; save(); out(""); sys.exit(0)
        out((st.get("pid", "4242") + "\\n") if st.get("running") else ""); sys.exit(0)
    if c == "ps":
        if st.get("running"):
            out("    1 init\\n 4242 " + st["package"] + "\\n 4243 " + st["package"] + ":game\\n")
        else:
            out("    1 init\\n")
        sys.exit(0)
    if c == "am":
        if "start" in sh and "s2arm.finish" not in text:
            st["props_at_launch"] = dict(st["props"])   # 起局那一刻的属性快照（--ab-prop 必须先于游戏出生）
        if "start" in sh:
            if "s2arm.finish" in text:
                if not st.get("unstoppable"):
                    st["running"] = False
                    # 适配器 shutdown 帧日志转储：native 同文双写 filesDir/frame-log-<pid>.log
                    # （emit 同时 fprintf + logcat，nvn_vk_frame_log.c）。替身缺省从
                    # logcat_dump 剥行前缀派生；priv_frame_log 旋钮给显式文件内容
                    # （比如 logcat 分片被截断但文件完整的救场局）。
                    dd = DATA_DIRS.setdefault("/data/data/" + st["package"], {})
                    if st.get("no_priv_seed"):
                        pass    # 零匹配负控：finish 不落 frame-log
                    elif st.get("priv_frame_log") is not None:
                        # 显式文件内容：<SESSION_UUID> 占位符按 setprop 本局号替换
                        u = ("-" if st.get("no_log_uuid") else
                             st.get("log_uuid") or
                             st["props"].get("debug.s2a.session_uuid") or "-")
                        dd["files/frame-log-4242.log"] = \
                            st["priv_frame_log"].replace("<SESSION_UUID>", u).encode().hex()
                    else:
                        segs = {}
                        for ln in open(st["logcat_dump"]).read().splitlines():
                            if "direct frame-log" not in ln: continue
                            f_ = ln.split(None, 3)
                            if len(f_) < 4 or f_[2] not in ("4242", "4243"): continue
                            segs.setdefault(f_[2], []).append(ln.split("S2Arm: ", 1)[1])
                        for pid_, lines_ in segs.items():
                            text_ = "\\n".join(lines_) + "\\n"
                            # 真机：meta 行尾带 setprop 的本局 session_uuid（同 host 首行）
                            if not st.get("no_log_uuid"):
                                u = st.get("log_uuid") or \
                                    st["props"].get("debug.s2a.session_uuid") or "-"
                                text_ = re.sub(r"(direct frame-log meta:[^\\n]*)",
                                               r"\\1 uuid=" + u, text_)
                            dd["files/frame-log-" + pid_ + ".log"] = text_.encode().hex()
                    # emit 同文双写：logcat 侧的 meta 行尾同样带本局 uuid——回填进
                    # dump/stream 文件（no_log_uuid=旧构建不带字段；与私有文件同源）
                    if not st.get("no_log_uuid"):
                        u2 = st.get("log_uuid") or \
                            st["props"].get("debug.s2a.session_uuid") or "-"
                        for f2 in ("logcat_dump", "logcat_stream"):
                            t2 = open(st[f2]).read()
                            t2 = re.sub(r"(direct frame-log meta:[^\\n]*)",
                                        r"\\1 uuid=" + u2, t2)
                            open(st[f2], "w").write(t2)
            else:
                st["running"] = True
                st["launch_cmd"] = text
                # 游戏起局：宿主日志落 files/ryujinx-android/host-<uptime>-<pid>-<hex>.log
                # 首行 = os.c s2a_android_session_log_start 的会话标记（名实互核的锚点）
                if not st.get("no_priv_seed"):
                    dd = DATA_DIRS.setdefault("/data/data/" + st["package"], {})
                    seg = st["uptime_us"] // 1000000
                    hn = f"host-{seg}-4242-Ab12.log"
                    # 真机：os.c 首行写 setprop 的本局 session_uuid（没置上 → uuid=-）；
                    # no_log_uuid=旧构建没这字段；log_uuid=显式覆盖（别局号负控）
                    uuid_tok = ""
                    if not st.get("no_log_uuid"):
                        u = st.get("log_uuid") or \
                            st["props"].get("debug.s2a.session_uuid") or "-"
                        uuid_tok = " uuid=" + u
                    hdr = (f"[s2a] Android 会话日志：session={seg} pid=4242{uuid_tok} "
                           f"file=/data/data/{st['package']}/files/ryujinx-android/{hn}\\n")
                    dd[f"files/ryujinx-android/{hn}"] = \
                        (hdr + st.get("host_log", "host log run\\n")).encode().hex()
                    # Java 起局写的 runtime-library.json：真机照样在，但 r20 起
                    # 本局锚已是文件内容 session_uuid——它只是背景噪声，不参判
                    if not st.get("no_runtime_marker"):
                        dd["files/ryujinx-android/runtime-library.json"] = json.dumps(
                            {"pid": 4242,
                             "sessionId": st.get("marker_uuid", "t1"),
                             "runtimeLibrary": {"fake": True}}).encode().hex()
                # 游戏起局即改写自己的数据（存档/新文件）——还原不还原从这里才分得出来
                for dir_ in list(DATA_DIRS):
                    if DATA_DIRS[dir_]:
                        DATA_DIRS[dir_]["files/save.dat"] = b"SAVE-RUN-MODIFIED".hex()
                        DATA_DIRS[dir_]["files/run-scratch.tmp"] = b"scratch".hex()
            save(); out("Status: ok\\nLaunchState: COLD\\nTotalTime: 200\\nWaitTime: 210\\n"); sys.exit(0)
        if "force-stop" in sh:
            if not st.get("unstoppable"): st["running"] = False
            save(); sys.exit(0)
        sys.exit(0)
    if c == "date":
        if "+%s" in text and st.get("no_wall"):   # 墙钟拿不到：归属窗/birth 折算都缺
            fail("date: command failed")
        if "+%s" in text:
            # 墙钟随开机钟推进（真机同律）；wall_jump 旋钮 = 局中墙钟跳变
            wall = st["wall_s"] + (st["uptime_us"] - st["wall_ref_us"]) // 1_000_000
            if st.get("wall_jump") and st["uptime_us"] > 20_000_000:
                wall += st["wall_jump"]
            out(str(wall) + "\\n"); sys.exit(0)
        out("11-09 12:00:00.000\\n"); sys.exit(0)
    if c == "dumpsys":
        if len(sh) > 1 and sh[1] == "battery": out(st["dumpsys_battery"]); sys.exit(0)
        if len(sh) > 1 and sh[1] == "thermalservice": out(st["dumpsys_thermal"]); sys.exit(0)
        if len(sh) > 1 and sh[1] == "package":
            if st["installed"]:
                flags = "pkgFlags=[ HAS_CODE ALLOW_CLEAR_USER_DATA" + (" DEBUGGABLE" if st.get("debuggable") else "") + " ]"
                out(f"versionCode={st.get('installed_vc', st['orig_vc'])}\\n    {flags}\\n"); sys.exit(0)
            out(""); sys.exit(0)
        out(""); sys.exit(0)
    if c == "run-as" and len(sh) > 4 and sh[2] == "sh" and sh[3] == "-c":
        run_as(sh[4]); sys.exit(0)
    if c == "logcat":
        if "-c" in sh: sys.exit(0)
        if "-d" in sh:
            dump = open(st["logcat_dump"]).read()
            if st.get("die_before_ready"):   # READY 前死：logcat 里不会有 READY 行
                dump = "\\n".join(ln for ln in dump.splitlines()
                                 if "Game startup accepted:" not in ln) + "\\n"
            out(dump); sys.exit(0)
        sys.exit(0)
    if text.startswith("e=$(stat -c %F"):
        # external_dirs_gone 探针：存在+可读 → ls 内容；读不进/父目录不可搜索 → __denied__
        m = re.search(r"stat -c %F (\\S+)", text)
        p = m.group(1).strip("'")
        mine = p.endswith("/" + st["package"])
        if mine and (st.get("ext_denied") or st.get("ext_parent_denied")):
            out("__denied__\\n"); sys.exit(0)
        if st.get("data_residue") and ("Android/data" in p or "Android/obb" in p):
            out("files\\n"); sys.exit(0)
        sys.exit(0)
    if c == "ls":
        if st.get("data_residue") and ("Android/data" in text or "Android/obb" in text):
            out("files\\n"); sys.exit(0)
        out(""); sys.exit(0)
    if c == "cat" and len(sh) > 1:
        p = sh[1]
        if p == "/data/local/tmp/s2a-power-samples.log" and st.get("sampler_on"):
            if st.get("sampler_empty_reads", 0) > 0:
                # 首读竞态：守护刚起、首块未落盘——文件已被 > 建出但 cat 回空
                st["sampler_empty_reads"] -= 1; save(); out(""); sys.exit(0)
            # 每拉一次守护多产一拍（替身按拉取节奏推进，不按墙钟）
            st["uptime_us"] += 1_000_000; st["sampler_ticks"] += 1
            charge = st["charge_uah"] - st["sampler_ticks"] * st["charge_step"]
            DEV_FS[p] = DEV_FS.get(p, "") + (
                st["power_sample"].replace("charge_counter=", f"charge_counter={charge}")
                + f"boot_us={st['uptime_us']}\\ncapacity=80\\n===\\n").encode().hex()
            save()
        if p in DEV_FS:
            out(bytes.fromhex(DEV_FS[p]).decode()); sys.exit(0)
        sys.exit(1)
    if c == "kill":
        st["sampler_on"] = False; save(); sys.exit(0)
    if c in ("wm", "input", "id", "cat", "mkdir"):
        out(""); sys.exit(0)
    fail("fake adb: unhandled shell " + text)
fail("fake adb: unhandled " + " ".join(argv))
'''


def make_state(root, **kw):
    state = {
        "package": PKG, "installed": False, "installed_sha": "", "running": False,
        "pid": "4242", "base_path": f"/data/app/{PKG}-Ab12/base.apk",
        "orig_apk_hex": "aabbccdd", "uptime_us": 10_000_000, "wall_s": 1_700_000_000,
        "debuggable": False, "orig_vc": 200, "data_dirs": {}, "pushed": {},
        "props": {
            "ro.product.model": "PixelTest", "ro.soc.model": "TEST_SOC",
            "ro.build.version.release": "15",
            "debug.s2a.debug_env": "", "debug.s2a.frame_log": "",
        },
        "dumpsys_battery": ("Current Battery Service state:\n  AC powered: false\n  USB powered: false\n"
                            "  Wireless powered: false\n  Dock powered: false\n"
                            "  status: 3\n  health: 2\n  level: 80\n  voltage: 4000\n  temperature: 350\n"),
        "dumpsys_thermal": "Current Status:0\nType{Name,Forecast}:CPU{CPU,0.5}\n",
        # power_sample_command 每次采样都附 dumpsys 旗标段（Android 16 四路 powered 全发）——
        # 没有它 battery_window 证不出「未接电」，能耗永远没数据；交互/省电/热档同理。
        # charge_counter 由替身每拍按 charge_step 递减（800mA ≈ 222µAh/s）。
        "charge_uah": 5_000_000, "charge_step": 222,
        "power_sample": ("status=Discharging\ncurrent_now=-800000\nvoltage_now=4000000\n"
                         "energy_now=\ncharge_counter=\ntemp=350\n"
                         "service_dump_begin\nCurrent Battery Service state:\n  AC powered: false\n"
                         "  USB powered: false\n  Wireless powered: false\n  Dock powered: false\n"
                         "  status: 3\n  health: 2\n  level: 80\n  voltage: 4000\n  temperature: 350\n"
                         "\nservice_dump_end\ninteractive=true\npower_save=false\nthermal_status=0\n"),
        "png": str(root / "shot.png"),
        "logcat_dump": str(root / "logcat_dump.txt"),
        "logcat_stream": str(root / "logcat_stream.txt"),
    }
    state.update(kw)
    state.setdefault("wall_ref_us", state["uptime_us"])   # 墙钟随开机钟推进的锚点
    path = root / "state.json"
    path.write_text(json.dumps(state))
    return path


def frame_log_text(recorded=200, step_us=16667, first=0, base_ts=10_500_000, pid=4242,
                   clock="CLOCK_BOOTTIME", disp=None):
    """v5 帧日志块（镜像 nvn_vk_frame_log.c：40B 记录 = ts_us + fence + acquire + present
    + gpu_us/gpu_seq + disp_us/disp_seq——无确认写 0xffffffff，照真机编码；
    meta 行 clock=CLOCK_BOOTTIME 照真机原文——此前写小写 boottime 是替身比真机松的假样）。
    disp=((记录下标, 帧号, 间隔us), …)：真机 disp 读数落在「确认到达时正在写的」记录上，
    可晚于所属帧、可乱序——宿主按 disp_seq 对号。"""
    marks = {i: (us, seq) for i, seq, us in (disp or ())}
    recs = []
    for i in range(recorded):
        us, seq = marks.get(i, (0xffffffff, 0xffffffff))
        recs.append(struct.pack("<QIIIIIIII", base_ts + i * step_us, 0, 0, 900, 1200,
                                0xffffffff, 0xffffffff, us, seq))
    blob = b"".join(recs)
    b64 = base64.b64encode(blob).decode()
    parts = [b64[i:i + 700] for i in range(0, len(b64), 700)]
    pre = f"11-09 12:00:01.000  {pid}  {pid} I S2Arm: "
    lines = [pre + f"direct frame-log meta: v=5 clock={clock} unit=us endian=le rec=40 "
                   f"cap=32768 recorded={recorded} presents={first + recorded} dropped=0 "
                   f"first={first} bytes={len(blob)} chunks={len(parts)}"]
    lines += [pre + f"direct frame-log[{i + 1}/{len(parts)}]:{p}" for i, p in enumerate(parts)]
    lines.append(pre + f"direct frame-log end: frames={recorded} dropped=0 "
                       f"sha256={hashlib.sha256(blob).hexdigest()}")
    return "\n".join(lines) + "\n"


LOG_HEAD = """11-09 12:00:00.100  4242  4242 I S2Arm: direct provider prepared: code=2334 gpu=MaliTest sym=251 apis=0x3
11-09 12:00:00.200  4242  4242 I S2Arm: [s2a] graphics selection: direct (graphics.v1.json admitted), devices=1 queues=1 workers=1 consumers=2
11-09 12:00:00.500  4242  4242 I S2Arm: Game startup accepted: 0123456789ABCDEF session t1
"""


def power_tsv(status="Discharging", currents=(-800000,), boots=None, powered="false",
              plugged=None, interactive="1", power_save="0", thermal_status="0",
              charge0=5_000_000, charge_step=222, charges=None):
    """按 run_game 写出的列序拼 power.tsv（t + FIELDS + level）。
    默认 30 行 ×1s：剔首尾 15s/5s 后分析窗还有 8s；电量计每拍 -222µAh（800mA 口径）。"""
    rows = ["t\t" + "\t".join(FIELDS) + "\tlevel"]
    boots = boots if boots is not None else tuple(11e6 + i * 1e6 for i in range(30))
    for i, b in enumerate(boots):
        cur = currents[i % len(currents)]
        charge = charges[i] if charges is not None else charge0 - i * charge_step
        pl = plugged if plugged is not None else ("1" if powered == "true" else "0")
        rows.append("\t".join([str(i), str(int(b)), status, str(cur), "4000000", "",
                               str(charge), "350", "sysfs", powered, powered, powered, powered,
                               pl, interactive, power_save, thermal_status, "80"]))
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

    def run_tool(self, state=None, argv=None, seconds=3, new_vc=200, out_name="out_1",
                 discharge_settle="0"):
        st_kw = {"test_apk_vc": new_vc}
        st_kw.update(state or {})
        state = make_state(self.tmp, **st_kw)
        out = self.tmp / out_name
        fake = self.tmp / "fake_adb.py"
        fake.write_text(FAKE_ADB)
        old_home = os.environ.get("ANDROID_HOME")
        os.environ["ANDROID_HOME"] = str(self._aapt2(new_vc))
        args = ["--apk", str(self.apk), "--package", PKG, "--serial", SERIAL,
                "--out", str(out), "--seconds", str(seconds),
                "--adb", f"{sys.executable} {fake} {state}",
                "--lock-root", str(self.lock_root), "--ready-timeout", "5",
                "--power-interval", "0"]
        if discharge_settle is not None:   # None = 命令行不带旗标（跑默认口径）
            args += ["--discharge-settle", str(discharge_settle)]
        args += argv or []
        try:
            clock = FixtureClock()
            with mock.patch.object(run_game, "time", clock), \
                    mock.patch.object(run_game.dev_runtime, "time", clock):
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
        rc, out, _ = self.run_tool(seconds=30)
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
        # ② 帧表 = 采集窗口内的帧：窗口 [10M, 40M) us（uptime 起步 10M、30 次拉取各 +1s），
        #   帧记录 10.5M 起 16667us 步长 ×200 → 全部落在窗口内 → 199 行间隔；别进程（9999）的块不进来
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual("frame\tpresent_ns\tpresent_us\twall_us\tdisp_us\tdisp_seq", rows[0])
        self.assertEqual(199, len(rows) - 1)
        pres = [int(r.split("\t")[1]) for r in rows[1:]]
        self.assertTrue(all(10_000_000_000 <= p < 40_000_000_000 for p in pres),
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

    def test_refuses_when_installed_not_debuggable(self):
        """③⑨ 已装但非 debuggable：run-as 备份做不了，私有数据保证不了还原 → 拒跑。"""
        rc, out, state = self.run_tool(state=self._installed_state(debuggable=False))
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "拒跑不该有任何安装动作")
        self.assertFalse(st["running"], "拒跑不该启动游戏")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("私有数据", man["reason"])
        self.assertIn("debuggable", man["reason"])

    def test_pm_path_query_failure_refuses_before_run(self):
        """⑫ pm path 查询失败（stderr + 非 0）→ 红、起局前就停，不留置（还没动过设备）。"""
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
        """⑫ rc=1 但带 stderr = adb 层查询失败，不能当『未安装』放行起局。"""
        rc, out, state = self.run_tool(state={"pm_path_fail_at": 1, "pm_path_fail_rc": 1})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "rc=1 带 stderr 必须判查询失败")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("pm path", man["reason"])

    def test_pm_path_failure_during_restore_recheck_detains(self):
        """⑫ 收局卸载后复查 pm path 查询失败 → 卸载结果未证 → 还原失败留置。"""
        # 未装路径 pm path 调用序：① 入局 install_state ② 收局 restore_state 入口 ③ 卸载后复查
        rc, out, state = self.run_tool(state={"pm_path_fail_at": 3})
        self.assertEqual(3, rc, "复查失败应留置（exit 3）")
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists())

    def test_pm_path_rc0_without_package_lines_is_failure(self):
        """⑫ rc=0 但没有 package: 行 = 查询失败（不能当未装放行起局）。"""
        rc, out, state = self.run_tool(state={"pm_path_empty_at": 1})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "rc=0 无 package: 行必须判查询失败")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("pm path", man["reason"])

    def test_pm_path_rc0_with_stderr_is_failure(self):
        """⑫ rc=0 有 package: 行但带 stderr = 查询失败（stderr 必须查）。"""
        rc, out, state = self.run_tool(
            state={**self._installed_state(), "pm_path_warn_at": 1})
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "rc=0 带 stderr 必须判查询失败")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("pm path", man["reason"])

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

    def _installed_state(self, **kw):
        orig_sha = hashlib.sha256(bytes.fromhex("aabbccdd")).hexdigest()
        base = {"installed": True, "installed_sha": orig_sha}
        base.update(kw)
        return base

    def _data_dirs(self):
        priv = f"/data/data/{PKG}"
        ext = f"/sdcard/Android/data/{PKG}"
        obb = f"/sdcard/Android/obb/{PKG}"
        return {priv: {"shared_prefs/settings.xml": b"<prefs/>".hex(),
                       "files/save.dat": b"SAVE-V1".hex(),
                       "files/profiles/active.bin": b"PROFILE".hex()},
                ext: {"files/import/index.tsv": b"idx".hex()},
                obb: {"main.obb": b"OBB".hex()}}

    def _data_state(self):
        """_data_dirs + 软链/空目录/权限覆盖：全量清单（非仅文件）的还原对象。"""
        priv = f"/data/data/{PKG}"
        return {"data_dirs": self._data_dirs(),
                "data_links": {priv: {"files/profile.bin": "files/profiles/active.bin"}},
                "data_subdirs": {priv: ["files/empty_cache", "files/empty_cache/deep"]},
                "data_modes": {priv: {"files/save.dat": "600"}}}

    def test_debuggable_installed_backs_up_and_restores_data(self):
        """⑨ 已装 debuggable：备份私有+外部数据（文件/软链/空目录/权限全量清单）
        → 收局装回原包后逐项核还原。"""
        want = self._data_state()
        dirs = want["data_dirs"]
        rc, out, state = self.run_tool(state=self._installed_state(debuggable=True, **want))
        self.assertEqual(0, rc, "debuggable 已装应能跑并全还原")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual(dirs, st["data_dirs"], "还原后设备数据应与入局前逐字节一致")
        self.assertEqual(want["data_links"][f"/data/data/{PKG}"],
                         st["data_links"][f"/data/data/{PKG}"], "软链目标应还原")
        self.assertTrue(set(want["data_subdirs"][f"/data/data/{PKG}"])
                        <= set(st["data_subdirs"][f"/data/data/{PKG}"]),
                        "显式空目录应还原（解开的 tar 会顺带重建隐式父目录）")
        orig_sha = hashlib.sha256(bytes.fromhex("aabbccdd")).hexdigest()
        self.assertEqual(orig_sha, st["installed_sha"], "还原后设备上应是原包")
        backup = out / "data-backup"
        self.assertTrue((backup / "manifest.json").is_file(), "备份清单没落盘")
        man = json.loads((backup / "manifest.json").read_text())
        self.assertEqual(3, len(man["dirs"]), "三处数据目录都该备份")
        self.assertEqual(3, len(man["dirs"]["private"]["files"]))
        ent = man["dirs"]["private"]["entries"]
        self.assertEqual("l", ent["files/profile.bin"]["t"])
        self.assertEqual("files/profiles/active.bin", ent["files/profile.bin"]["target"])
        self.assertEqual("d", ent["files/empty_cache"]["t"])
        self.assertEqual(0o600, ent["files/save.dat"]["mode"])
        for tag in man["dirs"]:
            self.assertTrue((backup / man["dirs"][tag]["tar"]).is_file())
        run_man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("data-backup", run_man.get("data_backup", {}).get("dir", ""))
        self.assertFalse((out / "orig-backup" / "orig.apk").exists(),
                         "还原全过：原包备份按保留策略清掉（再生材料不留 out）")

    def test_denied_dir_refuses_before_backup(self):
        """㉕ 数据目录存在但 run-as 进不去：报错拒跑而不是当不存在悄悄跳过。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_denied"] = {priv: True}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("进不去", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "备不了原数据就不该装新包")

    def test_parent_denied_dir_refuses_before_backup(self):
        """㉕ 父目录不可搜索：[ -e ] 假三态会报 absent 静默跳过（qa-fone 受限
        父目录实测）——stat 接 stderr 后此类一律 denied → 拒跑。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_parent_denied"] = {priv: True}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("进不去", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st)

    def test_backup_archive_mismatch_refuses(self):
        """㉕ 备份归档解包核对不过（tar 比源清单多成员）→ 拒跑，不入局。"""
        state = self._data_state()
        state["corrupt_backup_tar"] = True
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("对不上", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st)

    def test_backup_tar_streams_without_memory_balloon(self):
        """㊶ 备份 tar 流式落盘边算 sha——>阈值流不整进宿主内存（9000 私有树
        实测 ~7.3GB，capture_output 会把宿主内存吃爆）。tracemalloc 峰值断言。"""
        priv = f"/data/data/{PKG}"
        want = self._data_state()
        want["data_pad"] = {priv: {"files/blob.bin": 24 << 20}}
        import tracemalloc
        tracemalloc.start()
        try:
            rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **want))
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(0, rc)
        self.assertLess(peak, 12 << 20,
                        f"24MB 备份流应分块落盘，宿主峰值 {peak >> 20}MB 超阈")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual(24 << 20, st["data_pad"][priv]["files/blob.bin"],
                         "大文件备份-回灌应保持（替身按合成档登记）")

    def test_backup_external_dirs_use_shell_channel(self):
        """㊷ 9000 实测 run-as 摸 /sdcard/Android/* 被 SELinux 拒——外部目录走
        shell 可读通道（探测/清单/tar/回灌同命令，仅通道换），run-as 只留私有树。"""
        state = self._data_state()
        state["run_as_ext_denied"] = True
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertEqual(0, rc, "run-as 被拒不该挡外部目录备份（应走 shell 通道）")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual(state["data_dirs"], st["data_dirs"], "还原后数据逐字节一致")

    def test_data_listing_batches_and_counts(self):
        """㊹ device_entries 分批清单（find -print0 | xargs -0 -n 256 替代
        -exec {} +——4 万文件私有树 toybox E2BIG，9000 实测）：>256 文件多批走通，
        条目数与 find 计数一致才算完整。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_dirs"][priv].update({f"bulk/f{i:04d}.bin": "aa" for i in range(600)})
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertEqual(0, rc)
        man = json.loads((out / "data-backup" / "manifest.json").read_text())
        self.assertIn("bulk/f0599.bin", man["dirs"]["private"]["entries"])
        self.assertEqual(600 + 3, len(man["dirs"]["private"]["files"]))

    def test_data_listing_truncated_fails(self):
        """㊹ 半截清单（xargs rc=0 但某批输出丢了）：条目数≠find 计数 → 拒，
        不拿半截当全量（失败签名=清单不完整，唯计数门所出）。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_listing_short"] = {priv: 1}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("清单不完整", man["reason"])

    def test_data_xargs_batch_fail_refuses(self):
        """㊹ xargs 中途批次挂（rc=126）：S2A_RC≠0 → 清单未证拒跑
        （失败签名=清单未证，唯 rc 门所出）。"""
        state = self._data_state()
        state["data_xargs_fail"] = True
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("清单未证", man["reason"])

    def test_listing_find_fail_and_stderr_refuses(self):
        """㊾ find 中途被拒：出一行就挂 + Permission denied + find rc=1——
        旧管线 xargs rc=0 会把它吞成全量清单（9000 复现签名）；find 自报 rc、
        管道 rc、stderr 前缀行三门各自逮住 → 清单未证拒跑。"""
        priv = f"/data/data/{PKG}"
        for i, masked in enumerate((False, True)):
            self.lock_root = self.tmp / f"locks_ff{i}"   # 每变体全新锁根：上局已留置
            state = self._data_state()
            state["data_find_fail"] = {priv: {"rc": 1, "masked": masked,
                                             "err": "find: ./x: Permission denied"}}
            rc, out, _ = self.run_tool(
                state=self._installed_state(debuggable=True, **state),
                out_name=f"out_ff{i}")
            self.assertNotEqual(0, rc, f"masked={masked}")
            man = json.loads((out / "run-manifest.json").read_text())
            self.assertIn("清单未证", man["reason"], man["reason"])
            self.assertIn("Permission denied", man["reason"], "设备 stderr 要进 reason 归因")
            if masked:
                # xargs rc=0 吞了上游失败——只剩 find 自报 rc 一门独逮
                self.assertIn("'find': '1'", man["reason"], man["reason"])
                self.assertIn("'xargs': '0'", man["reason"], man["reason"])

    def test_listing_stderr_refuses(self):
        """㊾ 清单 stderr 非白名单（白名单现为空：有 stderr 即未证）→ 拒。
        设备 stderr 经 S2A_ERR: 前缀行取回——adb 会把设备 stderr 混进 stdout，
        靠宿主侧 stderr 判不了。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_listing_err"] = {priv: ["xargs: sha256sum: exited with status 126"]}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("清单未证", man["reason"], man["reason"])

    def test_backup_tar_remote_err_refuses(self):
        """㊾ #329000：exec-out 把设备 tar stderr 混进归档字节流、宿主 stderr
        是空的——stderr/rc 落设备侧 .s2a-tar-{err,rc} 凭据另行取证；
        tar stderr 非白名单 → 备份未证拒跑（不拿未证归档动设备）。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_tar_err"] = {priv: "tar: ./x: Permission denied\ntar: had errors\n"}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("备份未证", man["reason"], man["reason"])
        self.assertIn("Permission denied", man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st)

    def test_backup_tar_remote_rc_and_aux_cleanup(self):
        """㊾ tar rc 凭据非 0 → 备份未证；凭据文件删不净 → 同门拒
        （读不回/删不净都是未证，不拿未证归档动设备）。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_tar_rc"] = {priv: "1"}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("备份未证", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st)
        state = self._data_state()
        state["data_tar_aux_left"] = True
        self.lock_root = self.tmp / "locks_aux"
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state),
                                   out_name="out_aux")
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("未证", man["reason"], man["reason"])

    def test_restore_mangled_listing_detains(self):
        """㊾ #328940：pm clear 之后清单坏值——S2A_N 空 / 非数字 / stat mode
        非八进制——必须受控进 fails → manifest 落盘 + 留置；不许 ValueError
        类异常逸出、跳过还原收尾与锁释放链。"""
        priv = f"/data/data/{PKG}"
        for i, mangle in enumerate(("empty_n", "bad_n", "bad_mode")):
            with self.subTest(mangle=mangle):
                self.lock_root = self.tmp / f"locks_m{i}"    # 每个子用例全新锁根
                state = self._data_state()
                state["data_listing_mangle"] = {priv: mangle}
                rc, out, _ = self.run_tool(
                    state=self._installed_state(debuggable=True, **state),
                    out_name=f"out_m{i}")
                self.assertNotEqual(0, rc, mangle)
                self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                                f"{mangle}：坏值必须进留置链")
                man = json.loads((out / "run-manifest.json").read_text())
                self.assertIn("未证", man["reason"], f"{mangle}: {man['reason']}")

    def test_root_dir_mode_compared(self):
        """㊾ 根目录自身权限进对拍：设备 stat 清单含 "."（find -mindepth 0）、
        归档 ./ 根成员进 tar_entries——两归档仅根权限不同对拍必非空。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_root_mode"] = {priv: "777"}      # 设备根 777 ≠ 归档根 ./ 775
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc, "根权限不符必须逮住")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("权限", man["reason"], man["reason"])
        # 正路：根 mode 一致时 "." 条目在 manifest 里、对拍过
        self.lock_root = self.tmp / "locks_rootok"
        rc, out, _ = self.run_tool(state=self._installed_state(
            debuggable=True, **self._data_state()), out_name="out_rootok")
        self.assertEqual(0, rc)
        man = json.loads((out / "data-backup" / "manifest.json").read_text())
        ent = man["dirs"]["private"]["entries"]
        self.assertEqual(0o775, ent["."]["mode"], "根目录自身权限要记进清单")

    def test_empty_data_dir_passes_and_phantom_refuses(self):
        """㊾ 空输入 xargs（toybox 无 -r 默认空输入仍跑一次）：计数先行、
        n=0 跳过 find|xargs——纯空数据目录不误拒、照样备份/回灌/复核；
        幻影行（sha256sum 空参读 stdin 的 <空 sha>  -）若真出现 →
        条目数≠计数 → 计数门逮住不误当全量。"""
        ext = f"/sdcard/Android/data/{PKG}"
        obb = f"/sdcard/Android/obb/{PKG}"
        state = self._data_state()
        state["data_dirs"][ext] = {}                 # 目录在、里面空
        state["data_dirs"][obb] = {}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertEqual(0, rc, "纯空数据目录不该被拒")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual({}, st["data_dirs"].get(ext) or {}, "空目录还原后仍空")
        # 负控：替身强行吐幻影行（空输入却跑了一次 xargs）→ 拒
        self.lock_root = self.tmp / "locks_ph"
        state = self._data_state()
        state["data_dirs"][obb] = {}
        state["data_xargs_phantom"] = {obb: True}
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state),
                                   out_name="out_ph")
        self.assertNotEqual(0, rc, "幻影清单行必须被计数门逮住")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("不完整", man["reason"], man["reason"])

    def test_backup_aux_outside_tree_user_names_kept(self):
        """㊿ #329620：用户树里同名 .s2a-tar-err/.s2a-tar-rc 是普通数据——
        凭据已移到树外 /data/local/tmp/s2a-tar-<tag>-<uuid>-*，备份不带
        --exclude、开头不 rm：两文件照进清单/归档、回灌逐字节还原。"""
        priv = f"/data/data/{PKG}"
        state = self._data_state()
        state["data_dirs"][priv][".s2a-tar-err"] = b"user-err".hex()
        state["data_dirs"][priv][".s2a-tar-rc"] = b"user-rc".hex()
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual(b"user-err".hex(), st["data_dirs"][priv][".s2a-tar-err"],
                         "同名用户文件回灌后逐字节一致")
        self.assertEqual(b"user-rc".hex(), st["data_dirs"][priv][".s2a-tar-rc"])
        man = json.loads((out / "data-backup" / "manifest.json").read_text())
        self.assertIn(".s2a-tar-err", man["dirs"]["private"]["entries"])
        self.assertIn(".s2a-tar-rc", man["dirs"]["private"]["entries"])
        self.assertEqual({}, st["dev_tmp"], "树外凭据跑完全清干净")

    def test_backup_aux_preexist_refuses_and_detains(self):
        """㊿ #329620：凭据路径被占（本局 uuid 名仍撞/残留）→ [ ! -e ] 预建
        失败即拒、不覆盖既有文件；备份中途失败 → 还原链未证 → 留置。"""
        state = self._data_state()
        state["data_aux_preexist"] = True
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("预建", man["reason"], man["reason"])
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "备份中途失败必须留置")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st)

    def test_backup_stream_timeout_detains_and_cleans(self):
        """㊿ #329632：exec-out 流中途超时（设备 tar 挂了）——凭据已建即
        登记待清，超时出口照走核删；备份没返回 = 还原链未证 → 留置，
        data_backup=None 不许正常放锁。"""
        state = self._data_state()
        state["data_stream_hang"] = True
        state["data_stream_hang_s"] = 30
        orig = run_game._exec_out_stream
        def fast_timeout(d, args_, dest, timeout=600):
            return orig(d, args_, dest, timeout=0.5)   # 压短宿主超时免真等 600s
        run_game._exec_out_stream = fast_timeout
        try:
            rc, out, _ = self.run_tool(
                state=self._installed_state(debuggable=True, **state))
        finally:
            run_game._exec_out_stream = orig
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "流超时后必须留置（还原链未证）")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("中途失败", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual({}, st["dev_tmp"], "超时出口凭据已核删干净")
        self.assertNotIn("installs", st)

    def test_backup_stream_writefail_detains_and_cleans(self):
        """㊿ #329632：归档写盘失败（磁盘满/IO 错）——流函数抛错出口同样
        走凭据核删；备份没返回 → 留置不放锁。"""
        state = self._data_state()
        orig = run_game._exec_out_stream
        def write_fail(d, args_, dest, timeout=600):
            raise OSError("No space left on device")      # 模拟写盘失败
        run_game._exec_out_stream = write_fail
        try:
            rc, out, _ = self.run_tool(
                state=self._installed_state(debuggable=True, **state))
        finally:
            run_game._exec_out_stream = orig
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "写盘失败后必须留置（还原链未证）")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("中途失败", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual({}, st["dev_tmp"], "异常出口凭据已核删干净")

    def test_pushed_tamper_caught_by_post_verify(self):
        """㉕ 推回途中归档被塞异物：本地核对过时还干净 → 设备解开多成员 →
        设备端全量复核仍逮住 → 留置。"""
        state = self._data_state()
        state["corrupt_backup_before_restore"] = True
        rc, out, _ = self.run_tool(state=self._installed_state(debuggable=True, **state))
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists())
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("不符", man["reason"], man["reason"])

    def test_verify_gate_blocks_pm_clear_unit(self):
        """㉕ 归档核对是 pm clear 的硬门（单测直验顺序）：tar 与清单一致 →
        塞异物 → restore_data 返回失败且 pm clear 一次都没被调。"""
        bdir = self.tmp / "bk"
        bdir.mkdir()
        tar = bdir / "private.tar"
        blob = b"SAVE-V1"
        with tarfile.open(tar, "w") as tf:
            dmem = tarfile.TarInfo("./files")
            dmem.type, dmem.mode = tarfile.DIRTYPE, 0o775
            tf.addfile(dmem)
            info = tarfile.TarInfo("./files/save.dat")
            info.size, info.mode = len(blob), 0o600
            tf.addfile(info, io.BytesIO(blob))
        entries = {"files": {"t": "d", "mode": 0o775},
                   "files/save.dat": {"t": "f", "mode": 0o600,
                                      "sha": hashlib.sha256(blob).hexdigest()}}
        man = {"dirs": {"private": {"dir": "/data/data/x", "tar": "private.tar",
                                    "entries": entries}}}
        # 干净时核对应过（正向锚：门不是无脑拒）
        self.assertEqual([], run_game._verify_tar(man, bdir, "private"))
        with tarfile.open(tar, "a") as tf:
            evil = tarfile.TarInfo("./planted.evil")
            blob2 = b"x"
            evil.size = len(blob2)
            tf.addfile(evil, io.BytesIO(blob2))
        calls = []

        class Dev:
            argv = []

            def shell(self, cmd, **kw):
                calls.append(cmd); return ""

            def run_as(self, pkg, cmd, **kw):
                calls.append(cmd); return ""

            def run(self, *a, **kw):
                calls.append(" ".join(a))

        fails = run_game.restore_data(Dev(), PKG, man, bdir)
        self.assertTrue(fails)
        self.assertTrue(any("归档核对未过" in f for f in fails), fails)
        self.assertFalse(any("pm clear" in c for c in calls),
                         "核对未过 pm clear 一次都不许跑")

    def test_ext_dir_denied_detains(self):
        """㉕ 卸载后外部目录存在但读不进：报不可访问 ≠ 已清 → 留置。"""
        rc, out, _ = self.run_tool(state={"ext_denied": True})
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists())
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("读不进", man["reason"], man["reason"])

    def test_backward_downgrade_refuses_before_mutation(self):
        """⑩ qa-fone：原包 versionCode < 新包——装回方向是降级，先升级收局才试
        降级 = 装了赌能降回去。入局前证不了能装回 → 直接拒跑，设备原样。"""
        dirs = self._data_dirs()
        rc, out, state = self.run_tool(state=self._installed_state(
            debuggable=True, orig_vc=100, data_dirs=dict(dirs)), new_vc=300)
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st, "证不了能装回就一笔都不许装")
        self.assertFalse(st["running"], "拒在冷启动之前")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("降级", man["reason"], man["reason"])
        self.assertEqual(dirs, st["data_dirs"], "拒跑不该碰设备数据")
        self.assertFalse((out / "orig-backup" / "orig.apk").exists(),
                         "连拉都没拉：拒在动包之前")

    def test_pm_clear_failure_detains_and_keeps_data(self):
        """㉕ qa-fone：pm clear 必须 rc=0 且输出 Success 才往下走；clear 失败
        → 留置报红，find -delete/回灌都不执行，设备数据原样。"""
        dirs = self._data_dirs()
        rc, out, state = self.run_tool(state=self._installed_state(
            debuggable=True, data_dirs=dict(dirs), pm_clear_fails=True))
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists())
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("pm clear", man["reason"], man["reason"])
        self.assertIn("files/profiles/active.bin",
                      json.loads((self.tmp / "state.json").read_text())
                      ["data_dirs"].get(f"/data/data/{PKG}", {}),
                      "pm clear 失败不回灌：目录内容原样")

    def test_pm_clear_no_success_output_detains(self):
        """㉕ rc=0 但没有 Success（老设备沉默失败形态）→ 同样算失败、留置。"""
        dirs = self._data_dirs()
        rc, out, state = self.run_tool(state=self._installed_state(
            debuggable=True, data_dirs=dict(dirs), pm_clear_silent=True))
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("pm clear", man["reason"], man["reason"])
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists())

    def test_fake_install_dash_d_same_version_ok_downgrade_refused(self):
        """③-5 替身语义直验：同版 `install -r -d` 成功（同 versionCode 不触发
        版本排序，证不了降级能力）；apk_vc < 已装 vc 才是真降级——pm_no_downgrade
        时连 -d 都拒。"""
        state = make_state(self.tmp, **self._installed_state(orig_vc=200,
                                                             pm_no_downgrade=True))
        fake = self.tmp / "fake_adb.py"
        fake.write_text(FAKE_ADB)
        orig = self.tmp / "orig.apk"
        orig.write_bytes(bytes.fromhex("aabbccdd"))   # 与 orig_apk_hex 同文 = 同版
        same = subprocess.run([sys.executable, str(fake), str(state),
                               "install", "-r", "-d", str(orig)],
                              capture_output=True, text=True)
        self.assertEqual(0, same.returncode, "同版 -d 要被收（flag 解析层）")
        (self.tmp / "orig.apk").unlink()
        st = json.loads(state.read_text())
        st["orig_apk_hex"] = "ccdd"                    # 另一个字节包 = 低 vc 版
        state.write_text(json.dumps({**st, "orig_vc": 100}))
        old = self.tmp / "old.apk"
        old.write_bytes(bytes.fromhex("ccdd"))
        down = subprocess.run([sys.executable, str(fake), str(state),
                               "install", "-r", "-d", str(old)],
                              capture_output=True, text=True)
        self.assertNotEqual(0, down.returncode,
                            "真降级在 pm_no_downgrade 下必须拒（装不回的形态）")

    def test_forward_downgrade_installs_with_dash_d(self):
        """⑩ 原包 versionCode > 新包：装测试包本身就是降级要带 -d；还原是升级不带。"""
        rc, out, state = self.run_tool(state=self._installed_state(orig_vc=500),
                                       argv=["--allow-data-loss"], new_vc=100)
        self.assertEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        test_install = st["installs"][0]
        self.assertTrue(test_install["apk"].endswith("game.apk"), "fwd 方向真降级=装测试包本身")
        self.assertIn("-d", test_install["flags"], "装低 versionCode 测试包要 -d")
        self.assertNotIn("-d", st["installs"][-1]["flags"], "装回高 versionCode 原包不需要 -d")

    def test_forward_downgrade_unsupported_refuses(self):
        """⑩ 原包 > 新包且不支持 -d：连装新包都不行，拒跑。"""
        rc, out, state = self.run_tool(state=self._installed_state(orig_vc=500, pm_no_downgrade=True),
                                     argv=["--allow-data-loss"], new_vc=100)
        self.assertNotEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("installs", st)

    def test_restore_sha_mismatch_detains(self):
        """⑪ 数据还原后 sha 与备份清单不符（还原被改写）→ 留置。"""
        dirs = self._data_dirs()
        rc, out, state = self.run_tool(state=self._installed_state(
            debuggable=True, data_dirs=dict(dirs), corrupt_restore=True))
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "数据还原 sha 不符应留置")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("sha", man["reason"])
        self.assertTrue((out / "orig-backup" / "orig.apk").exists(),
                        "还原失败：原包备份留 out 供人工救")
        self.assertIn("orig-backup", man.get("orig_apk_backup", ""),
                      "manifest 记原包备份路径")

    def test_orig_backup_kept_when_verify_fails(self):
        """⑪ qa-fone ②：备份清理必须排在所有还原核验之后——属性恢复读回
        不符（props_jam）→ 留置报红，orig.apk 不许已删。"""
        rc, out, state = self.run_tool(state=self._installed_state(
            debuggable=True, data_dirs=dict(self._data_dirs()), props_jam=True))
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists())
        self.assertTrue((out / "orig-backup" / "orig.apk").exists(),
                        "核验未过时备份已被删 = 人工救不回来")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("orig-backup", man.get("orig_apk_backup", ""))

    def test_orig_backup_unlink_failure_still_finishes(self):
        """⑪ qa-fone ③：orig_tmp.unlink() 抛 OSError 不许吞掉后续收尾——
        注记记账、manifest 照写、logcat/锁照常收。"""
        rc, out, _ = self.run_tool(state=self._installed_state(
            debuggable=True, data_dirs=dict(self._data_dirs()), pull_dir=True))
        # pull 把备份路径落成目录 → sha 比对不过（拒跑）且收局 unlink 必抛 OSError
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(any("清理失败" in n for n in man["notes"]), man["notes"])
        self.assertIn("orig-backup", man.get("orig_apk_backup", ""),
                      "清理失败的路径要记进 manifest")
        self.assertTrue((out / "run-manifest.json").exists(), "manifest 必须落盘")

    def test_orig_backup_dir_kept_when_rmdir_fails(self):
        """qa-fone ③：orig.apk 删成、orig-backup 目录非空 rmdir 抛——文件与目录
        分开记账：orig_backup_dir 记目录、orig_apk_backup 不记（文件已清），
        结果照绿不冤枉文件还在。"""
        rc, out, _ = self.run_tool(state=self._installed_state(
            debuggable=True, data_dirs=dict(self._data_dirs()), pull_stray=True))
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertNotIn("orig_apk_backup", man, "文件已清就不能再记它留存")
        self.assertIn("orig_backup_dir", man)
        self.assertFalse((out / "orig-backup" / "orig.apk").exists())
        self.assertTrue((out / "orig-backup" / "orig.apk.stray").exists())
        self.assertTrue(any("目录留存" in n for n in man["notes"]), man["notes"])

    def test_frame_block_uuid_missing_dropped(self):
        """qa-fone ②：logcat 帧块 meta 无 uuid 字段（旧构建/属性没置上）→
        PID+时间窗单独证不了本局，整段不收；manifest.binding 如实记证据链。"""
        rc, out, _ = self.run_tool(state={"no_log_uuid": True})
        self.assertEqual(0, rc)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(1, len(rows), "无 uuid 的块一帧都不进表")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual(0, man["frame_log"]["captures"])
        self.assertIn("uuid", man["frame_log"]["binding"])
        self.assertTrue(any("uuid" in n and "不收" in n for n in man["notes"]), man["notes"])

    def test_frame_block_uuid_mismatch_dropped(self):
        """qa-fone ②：帧块 meta uuid 是别局号（同 PID 旧局转储）→ 整段不收，
        不凭 PID 把它误并本局帧表。"""
        rc, out, _ = self.run_tool(state={"log_uuid": "OTHER-SESSION"})
        self.assertEqual(0, rc)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(1, len(rows), "别局 uuid 的块一帧都不进表")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual(0, man["frame_log"]["captures"])
        self.assertTrue(any("OTHER-SESSION" in n for n in man["notes"]), man["notes"])

    def test_frame_block_cross_pid_splice_rejected(self):
        """qa-fone ①：meta 来自 4242（uuid 本局）、分片+end 全来自 4243——
        同 session 两进程的行不许拼成一块：4242 只剩 meta 按截断如实报，
        4243 的残体无 meta 不成块，captures=0。"""
        a = frame_log_text(recorded=200, pid=4242, first=0, base_ts=10_500_000)
        b = frame_log_text(recorded=200, pid=4243, first=0, base_ts=10_500_000)
        splice = a.splitlines()[0] + "\n" + "\n".join(b.splitlines()[1:]) + "\n"
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + splice)
        (self.tmp / "logcat_stream.txt").write_text(LOG_HEAD + splice)
        rc, out, _ = self.run_tool()
        self.assertEqual(0, rc)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(1, len(rows), "跨 PID 拼出的块一帧都不进表")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual(0, man["frame_log"]["captures"])
        self.assertTrue(any("截断" in n or "不完整" in n for n in man["notes"]), man["notes"])

    def test_frame_block_interleaved_pids_split(self):
        """qa-fone ① 正对照+负控：两进程完整转储逐行交错——块按各自行前缀
        PID 分开维护，只有同 PID 的新 meta 才截断旧块；no_priv_seed 关掉
        私有文件回退，纯 logcat 两块都必须完整（旧单 seg 版丢 4242 一块）。"""
        a = frame_log_text(recorded=200, pid=4242, first=0, base_ts=10_500_000)
        b = frame_log_text(recorded=150, pid=4243, first=0, base_ts=11_000_000)
        la, lb = a.splitlines(), b.splitlines()
        inter = "\n".join(x for i in range(max(len(la), len(lb)))
                          for x in la[i:i + 1] + lb[i:i + 1]) + "\n"
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + inter)
        (self.tmp / "logcat_stream.txt").write_text(LOG_HEAD + inter)
        rc, out, _ = self.run_tool(state={"no_priv_seed": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual(2, man["frame_log"]["captures"],
                         "交错转储两 PID 各成完整块，谁也不顶断谁")
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(149, len(rows) - 1, "覆盖最大块（4242 窗内 150 帧）出 149 行")

    def test_frame_block_dedup_same_count_diff_payload_kept(self):
        """qa-fone ②：去重键含 PID+载荷 SHA——同 PID 两块 first/recorded/ts 数
        全同但载荷不同（重转内容变了）不是重复，两份都保留；旧键当重复吞。"""
        a = frame_log_text(recorded=200, pid=4242, first=0, base_ts=10_500_000)
        c = frame_log_text(recorded=200, pid=4242, first=0, base_ts=11_200_000,
                           step_us=16668)
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + a + c)
        (self.tmp / "logcat_stream.txt").write_text(LOG_HEAD + a + c)
        rc, out, _ = self.run_tool(state={"no_priv_seed": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual(2, man["frame_log"]["captures"],
                         "同计数不同载荷的两块都是证据，一份不吞")

    def test_frames_tsv_disp_seq_joined_by_sequence(self):
        """v5 disp_us/disp_seq 按帧号对号：真机确认读数落在「到达时正在写的」记录
        上（可晚于所属帧、到达序可乱序）——宿主按 disp_seq 归帧，不按记录行序；
        未确认记录与 v<5（无 disp 域）disp 列留空，CPU present 间隔只在
        present_us/wall_us，不冒充确认读数。"""
        out = self.tmp / "frames-out"
        out.mkdir()
        # seq45/70/55 的确认分别落在记录 90/120/150 上：到达序与帧号序不一致（乱序迟到）
        caps = run_game.direct_framelog.parse(frame_log_text(
            recorded=200, first=0, base_ts=10_500_000,
            disp=((90, 45, 33200), (120, 70, 16500), (150, 55, 16800))))
        notes = []
        run_game.write_frames(out, caps, notes, "", window_us=None)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual("frame\tpresent_ns\tpresent_us\twall_us\tdisp_us\tdisp_seq", rows[0])
        by_frame = {int(r.split("\t")[0]): r.split("\t") for r in rows[1:]}
        self.assertEqual(("33200", "45"), (by_frame[45][4], by_frame[45][5]),
                         "记录 90 携带的 seq45 确认必须归到帧 45 行")
        self.assertEqual(("16500", "70"), (by_frame[70][4], by_frame[70][5]))
        self.assertEqual(("16800", "55"), (by_frame[55][4], by_frame[55][5]))
        self.assertEqual(("", ""), (by_frame[46][4], by_frame[46][5]),
                         "无确认帧 disp 列留空，不拿 0 或 CPU 间隔冒充")
        # 全未确认（disp 全 0xffffffff）→ disp 列整列空
        caps = run_game.direct_framelog.parse(frame_log_text(recorded=60, first=0, base_ts=10_500_000))
        run_game.write_frames(out, caps, notes, "", window_us=None)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertTrue(all(r.split("\t")[4] == "" and r.split("\t")[5] == ""
                            for r in rows[1:]), "v5 未确认 → disp_us/disp_seq 全空")
        # v<5 记录（disp 域缺席 = None）→ 同样全空，旧版证据不造确认读数
        caps = [{"meta": {"first": 0, "line": 1, "clock": "CLOCK_BOOTTIME"},
                 "ts": [10_500_000 + i * 16667 for i in range(30)],
                 "disp_us": None, "disp_seq": None}]
        run_game.write_frames(out, caps, notes, "", window_us=None)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertTrue(all(r.split("\t")[4] == "" and r.split("\t")[5] == ""
                            for r in rows[1:]), "v<5 → disp 列整列空")
        self.assertEqual("16667", rows[1].split("\t")[2],
                         "present_us 列恒为 CPU 相邻 present 间隔")

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
             "--out", str(out), "--seconds", "30", "--discharge-settle", "0",
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
        # ㉑ 中断路径同样在还原前拉回私有证据（pm clear/卸载会清掉它）
        self.assertTrue((out / "host_logs" / "host-10-4242-Ab12.log").is_file(),
                        "中断路径 host-*-<pid>-*.log 也该拉回")
        self.assertFalse((self.lock_root / SERIAL / "detained.json").exists(),
                         "还原成功不该留置")

    def test_mode_direct_marks_launch_line(self):
        self.run_tool(argv=["--mode", "direct"], state={})
        meta = json.loads((self.tmp / "out_1" / "android.json").read_text())
        self.assertIn("--es s2arm.graphics_mode direct", meta["launch"])

    def test_graphics_selection_asserts_actual_path(self):
        """--mode 断言本局实际路径：本局 PID 的「graphics selection:」行为准。
        符合→done+actual 记账；不符→RESULT=red；缺行（含别 PID 行顶包）→red；
        auto→不比值只如实记。"""
        log_sel = [ln for ln in LOG_HEAD.splitlines() if "graphics selection:" in ln]
        self.assertEqual(1, len(log_sel), "夹具应恰含一条选择行")

        # ① 符合：--mode direct + 夹具 actual=direct → 绿，manifest/android.json 记原文
        rc, out, _ = self.run_tool(argv=["--mode", "direct"], state={})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        sel = man["graphics_selection"]
        self.assertEqual("direct", sel["requested"])
        self.assertEqual("direct", sel["actual"])
        self.assertEqual(log_sel, sel["lines"])
        meta = json.loads((out / "android.json").read_text())
        self.assertEqual(sel, meta["graphics_selection"])

        # ② 不符：--mode generic + actual=direct → 红，原因写清请求 vs 实际
        rc, out2, _ = self.run_tool(argv=["--mode", "generic"], state={}, out_name="out_2")
        self.assertEqual(2, rc)
        man2 = json.loads((out2 / "run-manifest.json").read_text())
        self.assertIn("graphics selection 与 --mode 不符", man2["reason"])
        self.assertIn("请求 generic、本局实际 direct", man2["reason"])

        # ③ 缺行：本局 PID 没有选择行（9999 的别进程行不顶包）→ 红
        stray = ("11-09 12:00:00.200  9999  9999 I S2Arm: "
                 "[s2a] graphics selection: direct (stale), devices=1 queues=1\n")
        no_sel_dump = (self.tmp / "logcat_nosel_dump.txt")
        no_sel_stream = (self.tmp / "logcat_nosel_stream.txt")
        for src, dst in ((self.tmp / "logcat_dump.txt", no_sel_dump),
                         (self.tmp / "logcat_stream.txt", no_sel_stream)):
            dst.write_text(src.read_text().replace(log_sel[0] + "\n", stray))
        rc, out3, _ = self.run_tool(state={"logcat_dump": str(no_sel_dump),
                                           "logcat_stream": str(no_sel_stream)},
                                    out_name="out_3")
        self.assertEqual(2, rc)
        man3 = json.loads((out3 / "run-manifest.json").read_text())
        self.assertIn("没有「graphics selection:」行", man3["reason"])

        # ④ auto：不比值、如实记实际
        rc, out4, _ = self.run_tool(state={}, out_name="out_4")
        self.assertEqual(0, rc)
        man4 = json.loads((out4 / "run-manifest.json").read_text())
        self.assertEqual({"requested": "auto", "actual": "direct", "lines": log_sel},
                         man4["graphics_selection"])

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
            fh.write("frame\tpresent_ns\tpresent_us\twall_us\tdisp_us\tdisp_seq\n")
            for i in range(4000):
                fh.write(f"{i + 1}\t{10_000_000_000 + i * 16_667_000}\t16667\t16667\t16667\t{i + 1}\n")
        self.assertEqual("过", self._qc(d)["帧率"])

    def test_qc_window_coverage_insufficient_is_no_data(self):
        """⑯ 8E 首局场景：61.1s 采集窗，块首帧晚 ~4.7s、窗内只覆盖 56.4s（92%）。
        sum(disp_us)≥55s 的旧判据会放它进统计——要如实判「窗口覆盖不足」没数据。"""
        d = self.tmp / "qc-cover"
        d.mkdir()
        w0, w1 = 10_000_000, 71_080_000                    # 61.08s 窗口
        (d / "android.json").write_text(json.dumps({"frame_window_us": [w0, w1]}))
        first = w0 + 4_720_000                             # 块首帧晚 4.72s
        n = int(56_360_000 / 16667)                        # 窗内覆盖 56.36s
        with open(d / "frames.tsv", "w") as fh:
            fh.write("frame\tpresent_ns\tpresent_us\twall_us\tdisp_us\tdisp_seq\n")
            for i in range(1, n + 1):
                fh.write(f"{i}\t{(first + i * 16667) * 1000}\t16667\t16667\t16667\t{i}\n")
        r = self._qc(d)
        self.assertEqual("没数据", r["帧率"])
        self.assertIn("窗口覆盖不足", r["读数"])
        # 同一窗口贴边满覆盖（60s/61.08s ≈ 98%）→ 覆盖判据放行，走正常帧率统计
        with open(d / "frames.tsv", "w") as fh:
            fh.write("frame\tpresent_ns\tpresent_us\twall_us\tdisp_us\tdisp_seq\n")
            for i in range(1, 3601):
                fh.write(f"{i}\t{(w0 + i * 16667) * 1000}\t16667\t16667\t16667\t{i}\n")
        self.assertEqual("过", self._qc(d)["帧率"])

    def test_power_energy_verdicts(self):
        """⑧ 能耗口径（兄弟项目判读门）：物理放电+交互屏+非省电+热档<3+电量计互证 → 未证；
        接电 / 未知态 / 符号翻转 / 时钟倒退 / plugged 未知 / 熄屏 / 省电 / 热档 ≥3 /
        电量计缺行或不互证 → 没数据。"""
        self.assertEqual("未证", self._qc_energy(power_tsv()),
                         "整窗放电+电量计互证：诊断积分应记未证")
        self.assertEqual("没数据", self._qc_energy(power_tsv(status="Unknown")),
                         "未知充电状态不能当未接电")
        self.assertEqual("没数据", self._qc_energy(power_tsv(status="Charging")),
                         "窗口内接过电不算")
        self.assertEqual("没数据", self._qc_energy(power_tsv(powered="true")),
                         "plugged=1：状态写放电但旗标证接电，整块作废")
        self.assertEqual("没数据", self._qc_energy(power_tsv(plugged="")),
                         "plugged 读不出 = 证不了未接电，fail closed")
        self.assertEqual("没数据", self._qc_energy(power_tsv(interactive="0")),
                         "熄屏/不交互：状态变了整块作废")
        self.assertEqual("没数据", self._qc_energy(power_tsv(power_save="1")),
                         "省电模式开启：功率态变了整块作废")
        self.assertEqual("没数据", self._qc_energy(power_tsv(thermal_status="3")),
                         "thermal_status≥3 严重降频：整块作废")
        self.assertEqual("没数据", self._qc_energy(power_tsv(currents=(-800000, 800000))),
                         "电流符号翻转 = 放电方向不自洽")
        self.assertEqual("没数据", self._qc_energy(power_tsv(currents=(0,))),
                         "电流全零不是合法放电读数")
        self.assertEqual("没数据", self._qc_energy(power_tsv(boots=(11e6, 12e6, 11e6))),
                         "采样时钟倒退，窗口不成立")
        self.assertEqual("没数据", self._qc_energy(power_tsv(charges=[None] * 30)),
                         "电量计缺列/缺行：证不出交叉核对")
        # 电量计整局不互证：断崖放在被剔掉的首 15s（rows 0-3 狂掉）——窗内斜率照常过，
        # 但整局掉电 275,550µAh ≫ 电流积分 6,444µAh → 交叉核对不成立，积分不采信
        cliff_head = [5_000_000 - i * 90_000 for i in range(4)] + \
                     [4_730_000 - (i - 4) * 222 for i in range(4, 30)]
        self.assertEqual("没数据", self._qc_energy(power_tsv(charges=cliff_head)),
                         "电量计跳变与电流积分不互证：积分不采信")
        # 采样空洞 >3s：中间删 5 拍
        boots_gap = tuple(11e6 + (i if i < 15 else i + 5) * 1e6 for i in range(25))
        self.assertEqual("没数据", self._qc_energy(power_tsv(boots=boots_gap)),
                         "超过 3 秒采样空洞（掉线）整块作废")

    def test_discharge_settle_gate(self):
        """⑳ 放电稳定门 + 设备侧采样守护：settle 段采样（boot<窗）也落 power.tsv；
        manifest.power_sampler 记 dest/settle_ok；接电设备等不到连续放电 →
        超时照采、settle_ok=False + notes 留痕。"""
        rc, out, _ = self.run_tool(seconds=3, argv=["--discharge-settle", "3",
                                                    "--discharge-timeout", "10"])
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual("/data/local/tmp/s2a-power-samples.log",
                         man["power_sampler"]["dest"])
        self.assertTrue(man["power_sampler"]["settle_ok"])
        pw = (out / "power.tsv").read_text().splitlines()
        self.assertGreater(len(pw) - 1, 3, "settle 段的采样行（boot<窗）也该落盘")
        # 替身守护日志已清理：设备侧不留采样文件（收局收净）
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("/data/local/tmp/s2a-power-samples.log", st.get("dev_fs", {}))
        # 接电设备（AC powered=true → plugged=1）：放电永不稳定 → 超时照采
        plugged_sample = json.loads(make_state(self.tmp).read_text())["power_sample"].replace(
            "AC powered: false", "AC powered: true")
        rc, out, _ = self.run_tool(state={"power_sample": plugged_sample}, out_name="out_2",
                                   argv=["--discharge-settle", "3",
                                         "--discharge-timeout", "1"])
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertFalse(man["power_sampler"]["settle_ok"])
        self.assertTrue(any("放电未稳" in n for n in man["notes"]), man["notes"])

    def test_sampler_spawn_requires_full_detach(self):
        """① 受控负控（8E 红根因）：后台守护没真脱离时 adbd 不退——
        r25 真实旧命令串（三 fd 全引但没有 setsid，bd2082cc）卡死；
        setsid + 三 fd 全脱离立即回 PID。"""
        st = make_state(self.tmp)
        fake = self.tmp / "fake_adb.py"; fake.write_text(FAKE_ADB)
        adb = [sys.executable, str(fake), str(st)]
        # r25 真串逐字重建（run_game.py @ bd2082cc）：( while …; done ) >log
        # 2>/dev/null </dev/null & echo $! —— fd 引齐了但守护还挂控制终端
        supply = "battery"
        ap = run_game
        legacy_inner = (
            f"rm -f /data/local/tmp/s2a-power-samples.log; "
            f"( while :; do {ap.power_sample_command(supply)}; "
            f"printf 'capacity=%s\\n' \"$(cat /sys/class/power_supply/{supply}/capacity 2>/dev/null)\"; "
            f"printf '{ap.POWER_BLOCK_SEP}\\n'; sleep 1; done )"
            f" >/data/local/tmp/s2a-power-samples.log 2>/dev/null </dev/null & echo $!")
        legacy = "sh -c " + shlex.quote(legacy_inner)
        with self.assertRaises(subprocess.TimeoutExpired):
            subprocess.run([*adb, "shell", legacy], capture_output=True, timeout=4)
        fixed = ("sh -c 'rm -f /data/local/tmp/s2a-power-samples.log "
                 "/data/local/tmp/s2a-power-samples.log.pid; setsid sh -c "
                 "\"echo $$ >/data/local/tmp/s2a-power-samples.log.pid; while :; do "
                 ": s2a-power-sampler; sleep 1; done\" "
                 ">/data/local/tmp/s2a-power-samples.log 2>&1 </dev/null & echo $!'")
        r = subprocess.run([*adb, "shell", fixed], capture_output=True, timeout=5)
        self.assertEqual(0, r.returncode)
        self.assertEqual(b"7777\n", r.stdout)

    def test_sampler_spawn_empty_output_sweeps(self):
        """①b 启动空回执也算 spawn 未证：守护可能已起在写——脏输出不充 PID，
        照样按 PID 文件/标记扫孤儿；manifest 记 spawn_error。"""
        rc, out, _ = self.run_tool(state={"sampler_spawn_silent": True})
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("spawn_error", man["power_sampler"], man["power_sampler"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue(st.get("orphan_swept"), "空回执也要扫孤儿")
        self.assertFalse(st.get("sampler_on"), "孤儿循环不许活着")
        self.assertNotIn("/data/local/tmp/s2a-power-samples.log", st.get("dev_fs", {}))
        self.assertNotIn("/data/local/tmp/s2a-power-samples.log.pid", st.get("dev_fs", {}))

    def test_sampler_linger_files_kept(self):
        """② 退场复验：kill 后 /proc/<pid> 还在 → 回 LINGER 记 notes，
        设备文件不清（留证），杀不掉的守护记还在。"""
        rc, out, _ = self.run_tool(state={"sampler_linger": True})
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertNotEqual(0, rc)
        self.assertTrue(any("仍存活" in n or "收采样守护失败" in n
                            for n in man["notes"]), man["notes"])
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "杀不掉的守护 = 设备有残留未证 → 留置")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue(st.get("sampler_on"), "杀不掉的守护还在")
        self.assertIn("/data/local/tmp/s2a-power-samples.log", st.get("dev_fs", {}),
                      "杀不掉时日志文件不许删，留证")

    def test_settle_first_read_race_probe(self):
        """③ 接电首读竞态：守护刚起首块未落盘——补拉几轮仍空就用同口径
        dumpsys 现态判接电；接电局不许被首读空拖进放电等待。"""
        plugged = json.loads(make_state(self.tmp).read_text())["power_sample"].replace(
            "AC powered: false", "AC powered: true")
        rc, out, _ = self.run_tool(
            state={"power_sample": plugged, "sampler_empty_reads": 3},
            discharge_settle=None, argv=["--discharge-timeout", "5"])
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("settle_na", man["power_sampler"], man["power_sampler"])
        self.assertLess(man["power_sampler"]["waited_s"], 15,
                        "接电首读空不许拖进放电等待")

    def test_sampler_spawn_timeout_sweeps_orphan(self):
        """② spawn 超时：sampler 变量没赋值前炸也要扫孤儿——按 PID 文件/进程
        标记杀掉设备端守护、清日志文件；本局能耗按没数据处理。"""
        rc, out, _ = self.run_tool(
            state={"sampler_spawn_hang": True, "sampler_hang_s": 20})
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("spawn_error", man["power_sampler"])
        self.assertTrue(any("守护起不来" in n or "采样守护" in n
                            for n in man["notes"]), man["notes"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue(st.get("orphan_swept"), "孤儿守护要按 pid 文件/标记扫掉")
        self.assertFalse(st.get("sampler_on"), "孤儿循环不许活着")
        self.assertNotIn("/data/local/tmp/s2a-power-samples.log", st.get("dev_fs", {}))
        self.assertNotIn("/data/local/tmp/s2a-power-samples.log.pid", st.get("dev_fs", {}))

    def test_charging_discharge_settle_na(self):
        """③ 接电功能局默认不等放电：未显式给 --discharge-settle 且最新采样
        证在接电 → 门判不适用直接进窗（settle_na 记明原因）；显式给了照等。"""
        plugged = json.loads(make_state(self.tmp).read_text())["power_sample"].replace(
            "AC powered: false", "AC powered: true")
        rc, out, _ = self.run_tool(state={"power_sample": plugged},
                                   discharge_settle=None)
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("settle_na", man["power_sampler"], man["power_sampler"])
        self.assertTrue(man["power_sampler"]["settle_ok"])
        self.assertLess(man["power_sampler"]["waited_s"], 30,
                        "接电下不许先等 30s 放电才进窗")

    def test_no_power_sampler_disabled(self):
        """④ --no-power-sampler：接电功能局/帧时性能局不需要能耗数据——
        不起设备端采样守护（sampler_spawn_hang 下也照过，证明真没起）、
        manifest 记 power_sampler.disabled + 原因、不写 settle_ok（门没跑）、
        power.tsv 不落；quality_check 能耗报「没数据」读数写明未开采样。"""
        rc, out, _ = self.run_tool(
            state={"sampler_spawn_hang": True, "sampler_hang_s": 20},
            argv=["--no-power-sampler", "帧时性能局不接功耗"])
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        ps = man["power_sampler"]
        self.assertTrue(ps.get("disabled"), ps)
        self.assertEqual("帧时性能局不接功耗", ps["reason"])
        self.assertNotIn("settle_ok", ps, "门没跑不许记 settle_ok")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertFalse(st.get("sampler_on"), "flag 局不许起采样守护")
        self.assertNotIn("/data/local/tmp/s2a-power-samples.log", st.get("dev_fs", {}))
        self.assertFalse((out / "power.tsv").exists())
        meta = json.loads((out / "android.json").read_text())
        self.assertTrue(meta["power_sampler"]["disabled"], "android.json 要带供判读")
        qrow = self._qc(out)
        self.assertEqual("没数据", qrow["能耗"])
        self.assertIn("帧时性能局不接功耗", qrow["读数"], qrow["读数"])

    # ---- 检查点（s2a-checkpoint/1）：部署后首启前 run-as 解进存档根 + 逐文件 sha 核对 ----
    _CK_TID = "0123456789ABCDEF"          # PKG 尾 16 位十六进制大写
    _CK_FILES = {
        "bis/user/save/0000000000000001/1/game_data.sav": b"SAVE-DATA",
        # 0 字节 .lock：toybox stat %F 报 "regular empty file"（8E 预演真红根因）
        "bis/user/save/0000000000000001/1/.lock": b"",
        "bis/system/save/8000000000000000/1/lastPublishedId": b"12345678",
    }

    def _checkpoint(self, name="w1-node", files=None, meta_extra=None,
                    tar_members=None, trunc=False):
        """造检查点目录：saves.tar + meta.json + saves.tar.sha256。
        files={rel: bytes} 与 meta.files 一一对应；tar_members 可加声明外成员；
        trunc=截断 tar（sha 按截后实算——过 sha 门、倒在 tarfile 读）。"""
        cp = self.tmp / name
        cp.mkdir()
        files = dict(files if files is not None else self._CK_FILES)
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tf:
            for rel, blob in files.items():
                info = tarfile.TarInfo(rel)
                info.size = len(blob)
                tf.addfile(info, io.BytesIO(blob))
            for rel, blob in (tar_members or {}).items():
                info = tarfile.TarInfo(rel)
                info.size = len(blob)
                tf.addfile(info, io.BytesIO(blob))
        data = buf.getvalue()
        (cp / "saves.tar").write_bytes(data[:60] if trunc else data)
        blob = (cp / "saves.tar").read_bytes()
        (cp / "saves.tar.sha256").write_text(
            hashlib.sha256(blob).hexdigest() + "  saves.tar\n")
        meta = {"schema": "s2a-checkpoint/1", "game": "wonder",
                "title_id": self._CK_TID, "package": PKG, "scene": name,
                "files": {rel: [len(b), hashlib.sha256(b).hexdigest()]
                          for rel, b in files.items()}}
        meta.update(meta_extra or {})
        (cp / "meta.json").write_text(json.dumps(meta))
        return cp

    def test_debug_prop_preflight_rejects_residue(self):
        props = {"ro.product.model": "PixelTest", "debug.s2a.query_wait": "1",
                 "debug.s2a.mem_shared": "1", "debug.s2a.fixmask": "31",
                 "debug.s2a.empty": ""}
        rc, out, _ = self.run_tool(state={"props": props})
        man = json.loads((out / "run-manifest.json").read_text())
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual(2, rc, man)
        self.assertEqual({k: v for k, v in props.items() if k.startswith("debug.s2a.")},
                         man["debug_props_snapshot"])
        for key in ("debug.s2a.query_wait", "debug.s2a.mem_shared", "debug.s2a.fixmask"):
            self.assertIn(key, man["reason"])
            self.assertIn(repr(props[key]), man["reason"])
        self.assertTrue(st["getprop_before_install"])
        self.assertNotIn("launch_cmd", st)
        self.assertFalse(st["installed"])

    def test_debug_prop_preflight_clean_is_locked(self):
        props = {"ro.product.model": "PixelTest", "debug.s2a.debug_env": "1",
                 "debug.s2a.frame_log": "1", "debug.s2a.session_uuid": "old",
                 "debug.s2a.empty": ""}
        lock_file = str(self.lock_root / SERIAL / "device.lock")
        rc, out, _ = self.run_tool(state={"props": props, "probe_lock_file": lock_file})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual({k: v for k, v in props.items() if k.startswith("debug.s2a.")},
                         man["debug_props_snapshot"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertTrue(st["getprop_lock_held"])
        self.assertTrue(st["getprop_before_install"])

    def test_debug_prop_preflight_ab_override(self):
        props = {"debug.s2a.query_wait": "1"}
        rc, out, _ = self.run_tool(state={"props": props},
                                   argv=["--ab-prop", "debug.s2a.query_wait=0"])
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual(props, man["debug_props_snapshot"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual("0", st["props_at_launch"]["debug.s2a.query_wait"])
        self.assertEqual("1", st["props"]["debug.s2a.query_wait"])

    def test_debug_prop_preflight_explicit_allow(self):
        rc, out, _ = self.run_tool(state={"props": {"debug.s2a.mem_shared": "1", "debug.s2a.Trace-mode.sub": "yes"}},
                                   argv=["--allow-prop", "debug.s2a.mem_shared",
                                         "--allow-prop", "debug.s2a.Trace-mode.sub"])
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual({"debug.s2a.mem_shared": "1", "debug.s2a.Trace-mode.sub": "yes"},
                         man["debug_props_snapshot"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual("1", st["props_at_launch"]["debug.s2a.mem_shared"])

    def test_debug_prop_preflight_getprop_failure(self):
        for kind, fault in (("rc", {"getprop_all_fail": True}),
                            ("stderr", {"getprop_all_stderr": True}),
                            ("malformed", {"getprop_all_raw": "[ro.x]: [ok]\n[debug.s2a.bad]: [1\n"}),
                            ("duplicate", {"getprop_all_raw": "[ro.x]: [a]\n[ro.x]: [b]\n"}),
                            ("truncated", {"getprop_all_raw": "[ro.x]: [ok]"}),
                            ("empty", {"getprop_all_raw": ""})):
            with self.subTest(kind=kind):
                rc, out, _ = self.run_tool(state=fault, out_name="out_prop_" + kind)
                man = json.loads((out / "run-manifest.json").read_text())
                st = json.loads((self.tmp / "state.json").read_text())
                self.assertEqual(2, rc, man)
                self.assertIn("getprop", man["reason"])
                self.assertNotIn("launch_cmd", st)
                self.assertFalse(st["installed"])

    def test_debug_prop_preflight_allow_requires_exact_key(self):
        rc, out, _ = self.run_tool(state={"props": {"debug.s2a.query_wait": "1"}},
                                   argv=["--allow-prop", "debug.s2a.*"])
        self.assertEqual(2, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("--allow-prop", man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st)
        self.assertFalse(st["installed"])

    def test_ab_prop_set_before_launch_and_restored(self):
        """--ab-prop：起局前锁内置位（am start 那一刻已是新值），manifest 记值与原值，
        收局恢复成原值；格式不对 / 与运行器自管属性重名 → 拒跑、不起局。"""
        st0 = {"props": {"debug.s2a.vsync": "orig"}}
        rc, out, _ = self.run_tool(state=st0, argv=["--ab-prop", "debug.s2a.vsync=1"], out_name="out_ab")
        self.assertEqual(0, rc)
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertEqual("1", st["props_at_launch"].get("debug.s2a.vsync"), "起局时必须已是新值")
        self.assertEqual("orig", st["props"].get("debug.s2a.vsync"), "收局必须恢复原值")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual({"value": "1", "prev": "orig"}, man["ab_props"]["debug.s2a.vsync"])
        for bad, tag in (("debug.s2a.vsync", "noeq"), ("persist.sys.x=1", "ns"),
                         ("debug.s2a.vsync=1;reboot", "inj"), ("debug.s2a.session_uuid=x", "dup")):
            _, outb, _ = self.run_tool(argv=["--ab-prop", bad], out_name="out_ab_" + tag)
            manb = json.loads((outb / "run-manifest.json").read_text())
            self.assertIn("--ab-prop", manb["reason"], f"{tag}: {manb['reason']}")
            stb = json.loads((self.tmp / "state.json").read_text())
            self.assertNotIn("launch_cmd", stb, f"{tag}: 格式不对不许起局")

    def test_ab_prop_getprop_denied_fails_closed(self):
        """--ab-prop 属性读被拒（rc=1、空输出）：起局前 → 拒跑不起局；收局读回被拒 → 不得当成已恢复，
        还原失败、设备留置（原值为空时 rc=1 的空串恰与原值相等，最容易假绿）。"""
        _, out, _ = self.run_tool(state={"getprop_deny": ["debug.s2a.vsync"]},
                                  argv=["--ab-prop", "debug.s2a.vsync=1"], out_name="out_ab_deny")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st, "读不到原值不许起局")
        rc, out2, _ = self.run_tool(state={"getprop_deny_after_launch": ["debug.s2a.vsync"]},
                                    argv=["--ab-prop", "debug.s2a.vsync=1"], out_name="out_ab_deny2")
        self.assertNotEqual(0, rc)
        man = json.loads((out2 / "run-manifest.json").read_text())
        self.assertIn("debug.s2a.vsync", man["reason"], man["reason"])
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(), "恢复读回失败要留置")

    def test_ab_prop_readback_fail_after_set_still_strict(self):
        """setprop 已生效、紧接着读回被拒：manifest.ab_props 还没写，恢复仍必须按严格 rc=0 口径，
        原值为空时不得因 rc=1 的空串假绿 → 还原失败、留置、不起局。"""
        rc, out, _ = self.run_tool(state={"getprop_deny_after_set": ["debug.s2a.vsync"]},
                                   argv=["--ab-prop", "debug.s2a.vsync=1"], out_name="out_ab_mid")
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st, "读回失败不许起局")
        self.assertNotEqual(0, rc)
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(), "严格恢复读回失败要留置")

    def test_checkpoint_apply_and_verify(self):
        """检查点正路：宿主核过 → push → run-as tar 解进 saves/<tid>/ryujinx-android
        → 逐文件 sha 复核 → 照起局；manifest.checkpoint 记名/源 sha/核对 ok。"""
        cp = self._checkpoint()
        rc, out, _ = self.run_tool(argv=["--checkpoint", str(cp)])
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        ck = man["checkpoint"]
        self.assertEqual("ok", ck["verify"], ck)
        self.assertEqual("w1-node", ck["name"])
        self.assertEqual(self._CK_TID, ck["title_id"])
        self.assertEqual(3, ck["files"])
        self.assertTrue(re.fullmatch(r"[0-9a-f]{64}", ck["source_sha256"]))
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertIn("am start", st.get("launch_cmd", ""))
        state_dir = ("/data/data/" + PKG + "/files/user/0/saves/"
                     + self._CK_TID + "/ryujinx-android")
        dd = st["data_dirs"].get(state_dir, {})
        for rel, blob in self._CK_FILES.items():
            self.assertEqual(blob.hex(), dd.get(rel), rel)

    def test_checkpoint_files_sha_mismatch(self):
        """清单不符：meta.files 声明 sha 与实解文件不符 → 不起局（Refuse→red），
        manifest 记 verify=fail；另路：meta 声明的文件 tar 里没有同样拒。"""
        cp = self._checkpoint(meta_extra={
            "files": {"bis/user/save/0000000000000001/1/game_data.sav":
                      [9, "0" * 64],
                      "bis/user/save/0000000000000001/1/.lock":
                      [0, hashlib.sha256(b"").hexdigest()],
                      "bis/system/save/8000000000000000/1/lastPublishedId":
                      [8, hashlib.sha256(b"12345678").hexdigest()]}})
        _, out, _ = self.run_tool(argv=["--checkpoint", str(cp)], out_name="out_bad")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual("fail", man["checkpoint"]["verify"])
        # 声明 sha 与成员内容不符现在在宿主写设备前就拒（不再等解入后核）
        self.assertIn("内容 sha", man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st, "核对不过不许起局")
        cp2 = self._checkpoint(
            name="w2", meta_extra={"files": {"bis/user/save/nope": [1, "0" * 64]}})
        _, out2, _ = self.run_tool(argv=["--checkpoint", str(cp2)], out_name="out_miss")
        man2 = json.loads((out2 / "run-manifest.json").read_text())
        self.assertIn("不一致", man2["reason"])
        st2 = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st2)

    def test_checkpoint_member_content_sha_checked_before_push(self):
        """同长异内容：tar 成员长度与 meta 声明相同、内容 sha 不同 → 写设备前就拒，
        不 push、不起局（只核长度会放过它）。"""
        files = dict(self._CK_FILES)
        rel = "bis/user/save/0000000000000001/1/game_data.sav"
        cp = self._checkpoint(name="w-samelen", meta_extra={"files": {
            r: [len(b), hashlib.sha256(b if r != rel else b"X" * len(b)).hexdigest()]
            for r, b in files.items()}})
        _, out, _ = self.run_tool(argv=["--checkpoint", str(cp)], out_name="out_samelen")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("内容 sha", man["reason"], man["reason"])
        self.assertEqual("fail", man["checkpoint"]["verify"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st, "内容 sha 不符不许起局")
        self.assertNotIn("s2a-checkpoint", st.get("tar_pushed", ""),
                         "宿主内容核对不过不许 push 到设备")

    def test_checkpoint_strict_member_match(self):
        """成员↔清单严格一致（两向）：tar 多出未声明成员 → 拒；声明长度与成员
        实际长度不符 → 拒；meta.files 重复声明同一路径 → 拒（JSON 重复键不静默）。"""
        cp = self._checkpoint(tar_members={"bis/user/save/extra.und": b"x"})
        _, out, _ = self.run_tool(argv=["--checkpoint", str(cp)], out_name="out_extra")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("多", man["reason"], man["reason"])
        cp2 = self._checkpoint(name="w2", meta_extra={
            "files": {"bis/user/save/0000000000000001/1/game_data.sav":
                      [999, hashlib.sha256(b"SAVE-DATA").hexdigest()],
                      "bis/user/save/0000000000000001/1/.lock":
                      [0, hashlib.sha256(b"").hexdigest()],
                      "bis/system/save/8000000000000000/1/lastPublishedId":
                      [8, hashlib.sha256(b"12345678").hexdigest()]}})
        _, out2, _ = self.run_tool(argv=["--checkpoint", str(cp2)], out_name="out_sz")
        man2 = json.loads((out2 / "run-manifest.json").read_text())
        self.assertIn("长度", man2["reason"], man2["reason"])
        cp3 = self._checkpoint(name="w3dup")
        (cp3 / "meta.json").write_text(
            '{"schema":"s2a-checkpoint/1","title_id":"' + self._CK_TID + '",'
            '"package":"' + PKG + '","files":{'
            '"bis/user/save/0000000000000001/1/game_data.sav":[9,"'
            + hashlib.sha256(b"SAVE-DATA").hexdigest() + '"],'
            '"bis/user/save/0000000000000001/1/game_data.sav":[9,"'
            + hashlib.sha256(b"SAVE-DATA").hexdigest() + '"]}}')
        _, out3, _ = self.run_tool(argv=["--checkpoint", str(cp3)], out_name="out_dup")
        man3 = json.loads((out3 / "run-manifest.json").read_text())
        self.assertIn("重复声明", man3["reason"], man3["reason"])
        buf = io.BytesIO()                                  # tar 内重复成员名同拒
        with tarfile.open(fileobj=buf, mode="w") as tf:
            for blob in (b"A", b"B"):
                info = tarfile.TarInfo("bis/user/save/dup")
                info.size = len(blob)
                tf.addfile(info, io.BytesIO(blob))
        cp4 = self.tmp / "w4dup"
        cp4.mkdir()
        (cp4 / "saves.tar").write_bytes(buf.getvalue())
        (cp4 / "saves.tar.sha256").write_text(
            hashlib.sha256(buf.getvalue()).hexdigest() + "  saves.tar\n")
        (cp4 / "meta.json").write_text(json.dumps({
            "schema": "s2a-checkpoint/1", "title_id": self._CK_TID, "package": PKG,
            "files": {"bis/user/save/dup": [1, hashlib.sha256(b"A").hexdigest()]}}))
        _, out4, _ = self.run_tool(argv=["--checkpoint", str(cp4)], out_name="out_dup2")
        man4 = json.loads((out4 / "run-manifest.json").read_text())
        self.assertIn("成员重复", man4["reason"], man4["reason"])

    def test_checkpoint_title_id_canonical(self):
        """titleId 规范：meta.title_id 必须 16 位大写十六进制（BootContract 口径），
        小写/缺/非十六进制都拒——写设备前的畸形 meta 门。"""
        for bad, tag in (("0123456789abcdef", "low"), ("", "empty"), ("0123456789ABCDEG", "bad")):
            cp = self._checkpoint(name="w-" + tag,
                                  meta_extra={"title_id": bad} if bad else {"title_id": ""})
            _, out, _ = self.run_tool(argv=["--checkpoint", str(cp)],
                                      out_name="out_tid_" + tag)
            man = json.loads((out / "run-manifest.json").read_text())
            self.assertIn("大写十六进制", man["reason"], f"{tag}: {man['reason']}")
            self.assertEqual("fail", man["checkpoint"]["verify"])

    def test_checkpoint_refuses_installed(self):
        """已装即拒：设备已装该包时 --checkpoint 直接拒（不清已有数据、不动包），
        连安装步骤都不进；manifest 记 verify=fail。"""
        orig = self._installed_state(
            debuggable=True, orig_vc=200,
            data_dirs={"/data/data/" + PKG: {"files/save.dat": b"ORIG".hex()}})
        _, out, _ = self.run_tool(state=orig, argv=["--checkpoint",
                                                  str(self._checkpoint())],
                                  out_name="out_inst")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("已装", man["reason"], man["reason"])
        self.assertEqual("fail", man["checkpoint"]["verify"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st)
        self.assertEqual(b"ORIG".hex(),
                         st["data_dirs"]["/data/data/" + PKG]["files/save.dat"],
                         "已装数据不许被检查点动")

    def test_checkpoint_tmp_cleanup_failure_detains(self):
        """临时 tar 删除失败 → 收局并入还原失败：RESULT=red + 留置（设备有残留未证）。
        rm 在 push/解包两路 finally 都跑；失败记 checkpoint.tmp_left。"""
        rc, out, _ = self.run_tool(state={"checkpoint_rm_fail": True},
                                   argv=["--checkpoint", str(self._checkpoint())],
                                   out_name="out_rmfail")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertNotEqual(0, rc)
        self.assertIn("没删掉", man["reason"], man["reason"])
        self.assertIn("tmp_left", man["checkpoint"], man["checkpoint"])
        self.assertTrue((self.lock_root / SERIAL / "detained.json").exists(),
                        "临时文件删不掉要留置")

    def test_checkpoint_nonregular_rejected_raw_dump(self):
        """负对照：设备端报非普通文件（fifo）→ 仍拒不起局；verify=fail 时设备原始
        stat/sha/软链清单落 out/checkpoint-device/ 供归因（8E 空文件红那次就是
        靠 stat 类型名才定到根因）。"""
        state_dir = ("/data/data/" + PKG + "/files/user/0/saves/"
                     + self._CK_TID + "/ryujinx-android")
        rc, out, _ = self.run_tool(
            state={"dev_stat_types": {state_dir: {
                "bis/user/save/0000000000000001/1/game_data.sav": "fifo"}}},
            argv=["--checkpoint", str(self._checkpoint())], out_name="out_fifo")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertNotEqual(0, rc)
        self.assertEqual("fail", man["checkpoint"]["verify"])
        self.assertIn("核对不符", man["reason"], man["reason"])
        self.assertEqual("checkpoint-device/",
                         man["checkpoint"].get("device_listing"))
        stat_txt = (out / "checkpoint-device" / "stat.txt").read_text()
        self.assertIn("fifo", stat_txt)
        self.assertIn("game_data.sav", stat_txt)
        self.assertTrue((out / "checkpoint-device" / "sha256sum.txt").exists())
        self.assertTrue((out / "checkpoint-device" / "links.txt").exists())
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st, "非普通文件不许起局")

    def test_checkpoint_path_traversal(self):
        """路径越界：tar 成员含 .. 段 → 宿主门拒（push 都没发生），不起局。"""
        cp = self._checkpoint(tar_members={"bis/../../evil": b"x"})
        _, out, _ = self.run_tool(argv=["--checkpoint", str(cp)], out_name="out_trav")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual("fail", man["checkpoint"]["verify"])
        self.assertIn("越界", man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st)
        self.assertFalse(any("s2a-checkpoint" in k for k in st.get("pushed", {})),
                         "越界成员不许 push 到设备")

    def test_checkpoint_truncated_tar(self):
        """tar 截断：sha 按截后实算（过 sha 门）→ tarfile 读不出 → 拒，不起局。"""
        cp = self._checkpoint(trunc=True)
        _, out, _ = self.run_tool(argv=["--checkpoint", str(cp)], out_name="out_trunc")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("截断", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st)

    def test_checkpoint_wrong_package(self):
        """包名不符：meta.package 与本局包不同 → 拒（装错检查点比不装更糟）。"""
        cp = self._checkpoint(meta_extra={"package": "org.s2arm.game.t9999999999999999"})
        _, out, _ = self.run_tool(argv=["--checkpoint", str(cp)], out_name="out_wpkg")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("与本局", man["reason"], man["reason"])
        st = json.loads((self.tmp / "state.json").read_text())
        self.assertNotIn("launch_cmd", st)

    def test_private_logs_pulled_before_restore(self):
        """㉑ 私有证据收证：收局后、还原（pm clear/卸载清私有目录）前把当局进程的
        host-*-<pid>-*.log 与原生直写 frame-log-<pid>.log 拉回 out/host_logs/；
        异 PID 的 host 日志不拉；manifest.private_logs 记账；与 logcat 同文的
        私有帧块归并去重、不重复计帧。"""
        foreign = {"data_dirs": {"/data/data/" + PKG: {
            "files/ryujinx-android/host-9-9999-Xx99.log": b"foreign\n".hex()}}}
        rc, out, _ = self.run_tool(seconds=30, state=foreign)
        self.assertEqual(0, rc)
        logs = sorted(p.name for p in (out / "host_logs").iterdir())
        self.assertIn("host-10-4242-Ab12.log", logs)      # am start 落的宿主日志
        self.assertIn("frame-log-4242.log", logs)         # finish 落的原生帧文件
        self.assertNotIn("host-9-9999-Xx99.log", logs)    # 别局/别进程的日志不拉
        man = json.loads((out / "run-manifest.json").read_text())
        ru = man["session"]["run_uuid"]
        want = (f"[s2a] Android 会话日志：session=10 pid=4242 uuid={ru} "
                f"file=/data/data/{PKG}/files/ryujinx-android/host-10-4242-Ab12.log\n"
                "host log run\n").encode()
        self.assertEqual(want, (out / "host_logs/host-10-4242-Ab12.log").read_bytes())
        self.assertEqual(logs, sorted(r["name"] for r in man["private_logs"]))
        host_rec = next(r for r in man["private_logs"] if r["name"].startswith("host-"))
        self.assertEqual({"name": "host-10-4242-Ab12.log", "pid": 4242,
                          "bytes": len(want), "sha256": hashlib.sha256(want).hexdigest(),
                          "device_bytes": len(want),
                          "device_sha256": hashlib.sha256(want).hexdigest(),
                          "verified": True, "session": "bound"}, host_rec,
                         "manifest 记账要带上源端核对的字节/SHA/归属（双侧对拍）")
        sess = man["session"]["pids"]["4242"]
        wall = json.loads((self.tmp / "state.json").read_text())["wall_s"]
        self.assertEqual(800, sess["birth_ticks"], "出生证 jiffies 要记账")
        self.assertEqual(wall - 2, sess["birth_wall_s"],
                         "birth_ticks 要折成墙钟（wall - (uptime - ticks/100)）")
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(199, len(rows) - 1, "私有帧块与 logcat 同文去重后帧数不变")

    def test_private_frame_log_rescues_logcat_truncation(self):
        """㉑ logcat 转储块被截断（8E 二局形态：meta 声明的分片没传完）→ filesDir
        的 frame-log-<pid>.log 完整同文块把帧救回来：帧表有帧、截断注记仍如实留。"""
        full = frame_log_text(200, base_ts=10_500_000)
        priv = "".join(ln.split("S2Arm: ", 1)[1] + "\n" for ln in full.splitlines())
        priv = re.sub(r" chunks=\d+\n", lambda m: m.group(0)[:-1] + " uuid=<SESSION_UUID>\n",
                      priv, count=1)
        stray = frame_log_text(400, base_ts=10_050_000, pid=9999)  # end 行让转储等待不空转
        trunc = "\n".join(full.splitlines()[:-2]) + "\n"           # 4242 块砍尾 = 截断
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + stray + trunc)
        (self.tmp / "logcat_stream.txt").write_text(LOG_HEAD + stray + trunc)
        rc, out, _ = self.run_tool(seconds=30, state={"priv_frame_log": priv})
        self.assertEqual(0, rc)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(199, len(rows) - 1, "私有帧文件应把 logcat 截断掉的帧救回来")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(any("截断" in n for n in man["notes"]),
                        "logcat 侧截断要如实留注记（救回 ≠ 掩盖）")
        want = priv.replace("<SESSION_UUID>", man["session"]["run_uuid"])
        self.assertEqual(want, (out / "host_logs/frame-log-4242.log").read_text())

    def test_private_logs_not_debuggable_is_note_not_failure(self):
        """㉑ 已装包非 debuggable：run-as 进不去私有目录 → 记注不拉，跑局照常过
        （证据缺 ≠ 还原失败）。"""
        rc, out, _ = self.run_tool(seconds=3, state={"apk_debuggable": False})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual([], man["private_logs"])
        self.assertTrue(any("私有日志" in n for n in man["notes"]), man["notes"])

    def test_private_logs_stale_same_pid_not_bound(self):
        """㉒ 同 PID 旧局文件：mtime 早于进程出生 → 照拉收证但本局身份未证
        （无本局 uuid → unproven），本局新落的同 PID 文件 bound。"""
        stale = "/data/data/" + PKG + "/files/ryujinx-android/host-7-4242-Zz00.log"
        state = {"data_dirs": {"/data/data/" + PKG: {
                     "files/ryujinx-android/host-7-4242-Zz00.log": b"old run\n".hex()}},
                 "data_mtime": {stale: 1_699_900_000}}   # wall_s=1700000000 之前 = 旧局
        rc, out, _ = self.run_tool(seconds=30, state=state)
        self.assertEqual(0, rc)
        logs = sorted(p.name for p in (out / "host_logs").iterdir())
        self.assertIn("host-7-4242-Zz00.log", logs,
                      "判不了先后照收证——漏证据比标 unproven 更糟")
        man = json.loads((out / "run-manifest.json").read_text())
        rec = next(r for r in man["private_logs"] if r["name"] == "host-7-4242-Zz00.log")
        self.assertEqual("unproven", rec["session"], "旧局文件不冒充本局 bound")
        self.assertTrue(any("早于进程出生" in n for n in man["notes"]), man["notes"])

    def test_private_logs_stat_denied_attribution_unproven(self):
        """㉓ qa-fone 负控：同 PID 文件、stat 被拒 → mtime 读不了，归属证不了 →
        证据照拉但 session=unproven + 注记；绝不拿『pids 非空』冒充 bound。"""
        rc, out, _ = self.run_tool(seconds=30, state={"priv_stat_denied": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(man["private_logs"], "stat 拒不该挡证据拉回")
        self.assertTrue(all(r["session"] == "unproven" for r in man["private_logs"]),
                        man["private_logs"])
        self.assertTrue(all(r["verified"] for r in man["private_logs"]),
                        "传输完整性照样核实——归属未证 ≠ 字节没对上")
        self.assertTrue(any("stat 拿不到 mtime" in n for n in man["notes"]), man["notes"])

    def test_private_logs_mtime_below_birth_collected_unproven(self):
        """㉗ qa-fone 边界：stat -c %Y 整秒 vs birth 浮点判不了先后——不跳过、
        不判 bound：照拉收证，本局身份交给文件内容 uuid 裁决。"""
        stale = "/data/data/" + PKG + "/files/ryujinx-android/host-7-4242-Zz00.log"
        wall = 1_700_000_000
        state = {"data_dirs": {"/data/data/" + PKG: {
                     "files/ryujinx-android/host-7-4242-Zz00.log": b"old run\n".hex()}},
                 "data_mtime": {stale: wall - 3}}   # 替身 birth=wall-2：早于出生
        rc, out, _ = self.run_tool(seconds=30, state=state)
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        rec = next(r for r in man["private_logs"] if r["name"] == "host-7-4242-Zz00.log")
        self.assertEqual("unproven", rec["session"],
                         "判不了先后照收但标 unproven——不许判 bound 也不许漏证")
        self.assertTrue(any("早于进程出生" in n for n in man["notes"]), man["notes"])

    def test_private_logs_uuid_missing_unproven(self):
        """㉖ qa-fone 本局锚：文件内容没有 session_uuid 字段（旧构建没写/属性
        没置上）→ 文件名/首行/PID/birth 全自洽也只能 unproven——「属本局」
        要文件自述本局号，不是文件年龄看着像。"""
        rc, out, _ = self.run_tool(seconds=30, state={"no_log_uuid": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual({"frame-log-4242.log", "host-10-4242-Ab12.log"},
                         {r["name"] for r in man["private_logs"]},
                         "证据照样收——归属未证不等于文件没拉回")
        for rec in man["private_logs"]:
            self.assertEqual("unproven", rec["session"], f"{rec['name']} 无 uuid 不许 bound")
            self.assertTrue(rec["verified"], "传输完整性照样核实")
        self.assertTrue(any("session_uuid" in n for n in man["notes"]), man["notes"])

    def test_private_logs_uuid_mismatch_unproven(self):
        """㉖ qa-fone 反例钉死：同 PID、名实自洽、mtime 够新的文件，内容 uuid
        与本局号不符 → unproven——别局文件造不出本局随机号。"""
        rc, out, _ = self.run_tool(seconds=30, state={"log_uuid": "OTHER-SESSION"})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        for rec in man["private_logs"]:
            self.assertEqual("unproven", rec["session"], f"{rec['name']} uuid 不符不许 bound")
        self.assertTrue(any("不符" in n for n in man["notes"]), man["notes"])

    def test_private_logs_bound_content_uuid(self):
        """㉖ 正对照：本局 run_uuid 写进 host 首行 / 帧 meta → bound；manifest
        记 run_uuid，bound 记录的内容里必须真含本局号（判据没被绕过）。"""
        rc, out, _ = self.run_tool(seconds=30)
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        bound = [r for r in man["private_logs"] if r["session"] == "bound"]
        self.assertEqual({"frame-log-4242.log", "host-10-4242-Ab12.log"},
                         {r["name"] for r in bound})
        ru = man["session"]["run_uuid"].encode()
        for rec in bound:
            body = (out / "host_logs" / rec["name"]).read_bytes()
            self.assertIn(ru, body, f"{rec['name']} bound 但内容没有本局 uuid：判据被绕过")

    def test_private_logs_no_birth_attribution_unproven(self):
        """㉔ qa-fone 负控：墙钟正常但 /proc/<pid>/stat 读拒 → 进程 birth 证不了，
        起局墙钟不充数 → 证据照拉但一律 unproven + 注记，绝不升 bound。"""
        rc, out, _ = self.run_tool(seconds=30, state={"no_proc_stat": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(man["private_logs"], "birth 缺不该挡证据拉回")
        self.assertTrue(all(r["session"] == "unproven" for r in man["private_logs"]),
                        man["private_logs"])
        self.assertTrue(any("birth" in n and "归属未证" in n for n in man["notes"]),
                        man["notes"])

    def test_private_logs_no_timebase_attribution_unproven(self):
        """㉓ qa-fone 负控：birth(/proc/stat 拒）与墙钟（date +%s 拒）两头都缺 →
        没有可比窗 → unproven + 注记，不空放 bound。"""
        rc, out, _ = self.run_tool(seconds=30,
                                   state={"no_wall": True, "no_proc_stat": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(man["private_logs"])
        self.assertTrue(all(r["session"] == "unproven" for r in man["private_logs"]),
                        man["private_logs"])
        self.assertTrue(any("归属未证" in n for n in man["notes"]), man["notes"])

    def test_private_logs_wall_clock_jump_unproven(self):
        """㉔ 局中墙钟跳变（人改钟/NTP 大步）：READY 锚点对与收证复核对不上 →
        mtime 归属整域不可靠 → 一律 unproven + 注记（fail closed）。"""
        rc, out, _ = self.run_tool(seconds=30, state={"wall_jump": 600})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(man["private_logs"], "跳变不该挡证据拉回")
        self.assertTrue(all(r["session"] == "unproven" for r in man["private_logs"]),
                        man["private_logs"])
        self.assertTrue(any("墙钟" in n and "对不上" in n for n in man["notes"]),
                        man["notes"])

    def test_private_logs_header_mismatch_unproven(self):
        """㉓ host-<seg>-<pid>-*.log 名实互核：文件首行 session/pid 与文件名对不上
        （改名/拼接过的证据）→ 照拉但 unproven + 注记。"""
        bad = (b"[s2a] Android \xe4\xbc\x9a\xe8\xaf\x9d\xe6\x97\xa5\xe5\xbf\x97"
               b"\xef\xbc\x9asession=999 pid=4242 file=/x/host-5-4242-Cc33.log\n"
               b"body\n")
        state = {"data_dirs": {"/data/data/" + PKG: {
            "files/ryujinx-android/host-5-4242-Cc33.log": bad.hex()}}}
        rc, out, _ = self.run_tool(seconds=30, state=state)
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        bad_rec = next(r for r in man["private_logs"] if r["name"] == "host-5-4242-Cc33.log")
        self.assertEqual("unproven", bad_rec["session"], bad_rec)
        self.assertTrue(any("名实" in n or "会话标记" in n for n in man["notes"]),
                        man["notes"])
        good = next(r for r in man["private_logs"] if r["name"] == "host-10-4242-Ab12.log")
        self.assertEqual("bound", good["session"], "名实相符的当局文件照样 bound")

    def test_private_logs_truncated_pull_marked_unverified(self):
        """㉒ exec-out rc=0 但短读（截断传输）：源端 sha/wc 对不上 → verified=False
        记注标未证，不拿半截字节冒充完整证据。"""
        rc, out, _ = self.run_tool(seconds=30, state={"priv_truncate": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(man["private_logs"])
        self.assertTrue(all(not r["verified"] for r in man["private_logs"]),
                        man["private_logs"])
        self.assertTrue(any("校验不符" in n for n in man["notes"]), man["notes"])
        unv = man["private_logs"][0]
        self.assertEqual(unv["bytes"], len((out / "host_logs" / unv["name"]).read_bytes()))

    def test_private_logs_empty_pull_marked_unverified(self):
        """㉒ exec-out rc=0 但拉回零字节：qa-fone 点名『空数据不能当成功』→
        verified=False 记注标未证。"""
        rc, out, _ = self.run_tool(seconds=30, state={"priv_empty": True})
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(man["private_logs"])
        self.assertTrue(all(not r["verified"] for r in man["private_logs"]),
                        man["private_logs"])
        self.assertTrue(any("空数据" in n for n in man["notes"]), man["notes"])

    def test_private_logs_zero_match_noted(self):
        """㉒ 当局 PID 在私有目录下零匹配（设备没落任何相关文件）→ 显式记注，
        不静默略过。"""
        foreign = {"no_priv_seed": True, "data_dirs": {"/data/data/" + PKG: {
            "files/ryujinx-android/host-9-9999-Xx99.log": b"foreign\n".hex()}}}
        rc, out, _ = self.run_tool(seconds=30, state=foreign)
        self.assertEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertEqual([], man["private_logs"])
        self.assertTrue(any("零匹配" in n for n in man["notes"]), man["notes"])

    def test_ready_failure_still_collects_private_evidence(self):
        """㉒ READY 前进程已退出（pids 空）：finally 仍按文件名+起局窗收证、session 标
        unproven——死前宿主日志正是失败现场证据；连候选都没有写明未证。"""
        rc, out, _ = self.run_tool(state={"die_before_ready": True})
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("READY 前进程已退出", man["reason"])
        pulled = [r for r in man["private_logs"] if r["name"].startswith("host-")]
        self.assertTrue(pulled, "READY 前失败也要把死前宿主日志拉回")
        self.assertTrue(all(r["session"] == "unproven" for r in pulled))
        self.assertTrue((out / "host_logs" / "host-10-4242-Ab12.log").is_file())
        # 零候选变体：没落任何日志 → 失败现场证据明确记未证，不静默
        rc2, out2, _ = self.run_tool(state={"die_before_ready": True, "no_priv_seed": True},
                                     out_name="out_2")
        self.assertNotEqual(0, rc2)
        man2 = json.loads((out2 / "run-manifest.json").read_text())
        self.assertEqual([], man2["private_logs"])
        self.assertTrue(any("未证" in n for n in man2["notes"]), man2["notes"])

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
        pushed_src = Path(st["tar_pushed_src"])
        self.assertEqual(out / "deploy", pushed_src.parent,
                         "推送 tar 中间产物必须落本次 out/deploy 下，不进系统临时目录")
        self.assertFalse(pushed_src.exists(), "核对过后 tar 中间产物清掉（再生材料）")
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

    def test_resources_tar_verify_uses_bounded_batches(self):
        """⑭ 大资源树（>find -exec 参数上限）：设备端 SHA 核验走 print0|xargs 有界分批。
        替身对 -exec {} + 超 512 文件报 Argument list too long（8E r4 真机 toybox E2BIG）——
        实现退回 -exec 就会红。"""
        res, artifact = self._resources_dir(npayload=600)   # 609 文件 > find_exec_max 512
        self._with_receipt(artifact)
        rc, out, state = self.run_tool(
            argv=["--resources", str(res), "--push-tar", "always"])
        self.assertEqual(0, rc)
        self.assertIn("SHA 核对通过", (out / "run.log").read_text())

    def test_truncated_frame_block_reports_missing_chunks(self):
        """⑱ 转储截断如实报：meta 声明 N 片只收到 N-2 片且无 end（镜像 8E 二局
        meta chunks=225 只到 1–201 的断法）——manifest notes 必须写「截断」+
        缺了哪些片，不能静默丢成「没有 direct-present 帧」。"""
        lines = frame_log_text().splitlines(keepends=True)
        truncated = "".join(lines[:-3])          # 丢末 2 个分片行 + end 行
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + truncated)
        (self.tmp / "logcat_stream.txt").write_text(LOG_HEAD + truncated)
        rc, out, _ = self.run_tool()
        self.assertEqual(0, rc)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(1, len(rows), "截断块一帧都不该进表")
        man = json.loads((out / "run-manifest.json").read_text())
        trunc = [n for n in man["notes"] if "截断" in n]
        self.assertTrue(trunc, f"notes 没有截断报告：{man['notes']}")
        self.assertIn("缺分片", trunc[0])

    @mock.patch.object(run_game.dev_runtime, "time", new_callable=FixtureClock)
    def test_stop_waits_dump_end_before_force_stop(self, clock):
        """⑲ 收局时序：wait_done 没见 end 前不得 force-stop（有上限照走）；
        见 end 后进程还赖着才强杀；进程在转储窗口内先退 → 不强杀。"""
        calls = []
        class StubD:
            def __init__(self, alive_polls):
                self.pid_calls = 0
                self.alive_polls = alive_polls
            def shell(self, cmd, allowed=(0,), timeout=None):
                calls.append(cmd)
                if cmd.startswith("pidof"):
                    self.pid_calls += 1
                    return "4242\n" if self.pid_calls <= self.alive_polls else ""
                return ""
        # A：wait_done 第 3 次轮询才报定——force-stop 只能在它之后发出
        d = StubD(alive_polls=10**9)
        polls = [0]
        def done():
            polls[0] += 1
            return polls[0] >= 3
        res = run_game.dev_runtime.stop(d, PKG, wait_done=done, wait_seconds=30)
        self.assertEqual(3, polls[0], "没见 end 就该继续等，不能提前往下走")
        self.assertGreaterEqual(clock.offset, 6, "转储轮询和强杀前等待仍须走满虚拟时钟")
        self.assertTrue(res["forced_stop"], "转储落定后进程还赖着 → 才强杀")
        self.assertTrue(res["dump_done"])
        self.assertEqual("am force-stop " + PKG, calls[-1])
        # B：wait_done 到点不报 → 照常往下走；进程在窗口内先退 → 不强杀
        d = StubD(alive_polls=2)
        res = run_game.dev_runtime.stop(d, PKG, wait_done=lambda: False, wait_seconds=0)
        self.assertFalse(res["dump_done"])
        self.assertFalse(res["forced_stop"])
        # C：不给 wait_done → 旧行为：pidof 一空即回
        d = StubD(alive_polls=1)
        res = run_game.dev_runtime.stop(d, PKG)
        self.assertFalse(res["forced_stop"])
        self.assertNotIn("dump_done", res)

    def test_wrong_clock_block_rejected_not_windowed(self):
        """⑰ clock 非 boottime 的块（墙钟/CLOCK_REALTIME）与窗口时钟对不上：
        整块未证、frames.tsv 只留表头，不混进窗口统计。CLOCK_BOOTTIME 真机原文
        被认下由全套件证（frame_log_text 缺省就发 CLOCK_BOOTTIME）。"""
        stray = frame_log_text(400, base_ts=10_050_000, pid=9999)
        wrong = frame_log_text(clock="CLOCK_REALTIME")
        (self.tmp / "logcat_dump.txt").write_text(LOG_HEAD + stray + wrong)
        (self.tmp / "logcat_stream.txt").write_text(LOG_HEAD + wrong)
        rc, out, state = self.run_tool()
        self.assertEqual(0, rc)
        rows = (out / "frames.tsv").read_text().splitlines()
        self.assertEqual(1, len(rows), "错时钟块一帧都不该进表")
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertTrue(any("CLOCK_REALTIME" in n and "整段未证" in n for n in man["notes"]))

    def test_fake_adb_shell_single_word_is_127(self):
        """⑮ 替身忠实性负控：adb.shell('整串') 经 shlex.join 单引号包成一个词，
        设备 shell 当文件名 exec → rc=127（8E r5 qa-fone 实测）。替身若放宽解包，
        大树分批用例会假绿——这里钉死。"""
        state = make_state(self.tmp)
        fake = self.tmp / "fake_adb.py"; fake.write_text(FAKE_ADB)
        adb = [sys.executable, str(fake), str(state), "-s", SERIAL]
        bad = subprocess.run(adb + ["shell", "'find /x -type f -print0 | xargs -0 sha256sum'"],
                             capture_output=True, text=True)
        self.assertEqual(127, bad.returncode,
                         "单引号整串是一个词：设备当文件名 exec，替身必须 127")
        good = subprocess.run(adb + ["shell", "sh -c 'getprop s2a.anything'"],
                              capture_output=True, text=True)
        self.assertEqual(0, good.returncode, "sh -c <脚本> 必须解包执行到 getprop")

    def test_resources_tar_verify_mid_batch_failure_is_short(self):
        """⑭ 核验中途某批挂：报错只带短 stderr 尾，不把几千条文件清单灌进 manifest。"""
        res, artifact = self._resources_dir(npayload=600)
        self._with_receipt(artifact)
        rc, out, state = self.run_tool(
            state={"sha_batch_fail_at": 1},
            argv=["--resources", str(res), "--push-tar", "always"])
        self.assertNotEqual(0, rc)
        man = json.loads((out / "run-manifest.json").read_text())
        self.assertIn("SHA 核验失败", man["reason"])
        self.assertLess(len(man["reason"]), 600,
                        "只留短 stderr 尾，不能把核验输出整份灌进 reason")


def run_isolated_suite(options=()):
    """Each worker gets its own environment and module state; no device is used."""
    names = unittest.defaultTestLoader.getTestCaseNames(RunGameTest)
    if not names:
        print("no run_game tests discovered", file=sys.stderr)
        return 1
    groups = [names[i::4] for i in range(min(4, len(names)))]
    started = time.monotonic()

    def run_group(group):
        return subprocess.run(
            [sys.executable, __file__, *options, *("RunGameTest." + n for n in group)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    with ThreadPoolExecutor(max_workers=len(groups)) as pool:
        results = list(pool.map(run_group, groups))
    failed = False
    for group, result in zip(groups, results):
        print(result.stdout, end="")
        match = re.search(r"(?m)^Ran (\d+) tests? in ", result.stdout)
        if result.returncode or not match or int(match.group(1)) != len(group):
            print(f"worker incomplete: expected {len(group)}, rc={result.returncode}", file=sys.stderr)
            failed = True
    print(f"run_game: {len(names)} tests in {len(groups)} isolated processes; "
          f"wall {time.monotonic() - started:.3f}s")
    return int(failed)


if __name__ == "__main__":
    if len(sys.argv) == 1 or sys.argv[1:] == ["--durations", "0"]:
        sys.exit(run_isolated_suite(sys.argv[1:]))
    unittest.main(verbosity=2)
