#!/usr/bin/env python3
"""Android 设备锁：同一台设备同一时刻只有一个局（tools/flock_hold.py 内核锁 + 持有者元数据）。

用法：
  device_lock.py acquire --serial S [--holder NAME] [--pid P] [--wait S] [--note TXT]   # 打 token=<持锁令牌>
  device_lock.py release --serial S --token T                                        # 令牌不符拒放
  device_lock.py status  --serial S
  device_lock.py clear   --serial S          # 人工核查后重置：清留置标 + 未正常结束的标记并放锁

锁目录（持久，不放 /tmp）：<锁根>/<serial>/
  device.lock    内核文件锁本体，由 flock_hold.py 的持锁小进程盯持有者 PID 自动收尸
  holder.json    持有者 / 持有者 pid / 持锁小进程 pid / 开始时间 / 持锁令牌（release 要它）
  detained.json  留置标：还原失败或上一局未正常结束时落下；acquire 一律拒，人工 clear 才再放行

判活与仲裁只靠内核 flock + 持有者进程死活，不做「看锁文件在不在」的脆弱判定；
锁文件永不删除（删了重建换 inode 会分裂，见 tools/flock_hold.py 头注）。

隔离规则（宁可留置误报，不自动放行）：
- 取到内核 flock 后还要复查 detained.json 与 holder.json：等锁期间落下的留置 / 上一局未正常结束的
  标记都算数，抢到的锁自己放掉再拒——「先查后取」在取锁与查标记之间留竞态窗口。
- 持有者进程死了（崩溃 / 强杀 / adb 掉线拖死）内核锁会被自动放掉，但 holder.json 还在 = 上一局
  没走到正常收局：设备状态未证，下一手取锁判留置（rc=3），不自动放行。
- release 要持锁令牌（acquire 生成并记进 holder.json）：令牌不符拒放，防「别的会话顺手清场」。

锁根：--root > S2A_ANDROID_LOCK_ROOT > $S2A_RUN_ROOT/android-devices > ~/.s2arm/run/android-devices。

acquire 退出码：0 取到；1 到时限没取到；2 起不了持锁进程 / 锁文件打不开；3 设备被留置。
release 退出码：0 已放 / 本就空闲；2 令牌不符或锁被占但找不到持锁小进程（别硬删锁文件，查 ps）。
"""
import argparse, json, os, re, secrets, signal, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FLOCK_HOLD = REPO / "tools" / "flock_hold.py"
LABEL = "s2a_devlock"
POLL_S = float(os.environ.get("S2A_DEVLOCK_POLL_S", "1"))
SERIAL_RE = re.compile(r"[A-Za-z0-9._:-]+")


def lock_root():
    if os.environ.get("S2A_ANDROID_LOCK_ROOT"):
        return Path(os.environ["S2A_ANDROID_LOCK_ROOT"])
    base = os.environ.get("S2A_RUN_ROOT") or os.path.expanduser("~/.s2arm/run")
    return Path(base) / "android-devices"


def serial_ok(serial):
    if not SERIAL_RE.fullmatch(serial):
        raise ValueError(f"非法设备序列号 {serial!r}")
    return serial


def paths(root, serial):
    d = Path(root) / serial_ok(serial)
    return d, d / "device.lock", d / "holder.json", d / "detained.json"


def alive(pid):
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, ValueError):
        return False


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def held(root, serial):
    """内核视角：锁文件被持锁小进程占着？（probe 尝试 LOCK_EX|NB，拿不到=被占）"""
    _, lock_file, _, _ = paths(root, serial)
    if not lock_file.is_file():
        return False
    r = subprocess.run([sys.executable, str(FLOCK_HOLD), str(lock_file),
                        str(os.getpid()), "probe", LABEL],
                       capture_output=True, text=True)
    return r.returncode == 1


def detained_reason(root, serial):
    _, _, _, detained = paths(root, serial)
    doc = read_json(detained) if detained.is_file() else None
    return doc


def detain(root, serial, reason, holder="", out=""):
    """落下留置标：还原失败 / 上一局未正常结束时，人工核查前不再放行。"""
    d, _, _, detained = paths(root, serial)
    d.mkdir(parents=True, exist_ok=True)
    detained.write_text(json.dumps({"serial": serial, "reason": reason, "holder": holder,
                                    "out": out, "since": int(time.time()),
                                    "time": time.strftime("%Y-%m-%d %H:%M:%S")},
                                   ensure_ascii=False) + "\n", encoding="utf-8")


def _unclean(root, serial, hd):
    """holder.json 还在但局没正常结束：落下留置标并报告原因。"""
    hp = hd.get("pid", 0)
    reason = (f"上一局未正常结束：持有者 {hd.get('holder', '?')}(pid {hp}) 已退出，锁未正常释放"
              if not alive(hp) else
              f"上一局异常：持有者 {hd.get('holder', '?')}(pid {hp}) 还在但内核锁已丢（会话已失控）")
    detain(root, serial, reason + "；设备状态未证，人工核查后 clear", holder=str(hd.get("holder", "")),
           out=str(hd.get("out", "")))
    return (f"{serial} 被留置：{reason}；人工核查设备后 "
            f"`device_lock.py clear --serial {serial}`")


def _self_release(root, serial):
    """抢到了锁但复查发现问题：把自己的持锁小进程放掉再走。"""
    for pid in daemons(root, serial):
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    deadline = time.time() + 3
    while held(root, serial) and time.time() < deadline:
        time.sleep(0.05)


def acquire(root, serial, holder, pid, wait=0, note=""):
    """取锁。(rc, token_or_msg)：0=取到并返回持锁令牌；1=忙；2=起不了锁；3=被留置。"""
    d, lock_file, holder_file, detained = paths(root, serial)
    doc = read_json(detained) if detained.is_file() else None
    if doc:
        return 3, (f"{serial} 被留置：{doc.get('reason', '?')}（{doc.get('since', '?')} 由 "
                   f"{doc.get('holder', '?')}）；人工核查设备后 `device_lock.py clear --serial {serial}`")
    if not alive(pid):
        return 2, f"持有者进程 {pid} 已死，无法看守"
    d.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + wait
    while True:
        hd = read_json(holder_file) if holder_file.is_file() else None
        if hd:
            # 有标记：活的局（锁也在）= 忙；持有者死了或锁已丢 = 未正常收局 → 留置不放行
            if alive(hd.get("pid", 0)) and held(root, serial):
                if time.time() >= deadline:
                    return 1, f"{serial} 正被占（{hd.get('holder', '?')} pid {hd.get('pid')}）"
                time.sleep(POLL_S)
                continue
            return 3, _unclean(root, serial, hd)
        r = subprocess.run([sys.executable, str(FLOCK_HOLD), str(lock_file),
                            str(pid), "take", LABEL], capture_output=True, text=True)
        if r.returncode == 0:
            daemon = int(r.stdout.strip().split("\t")[0])
            # 拿到内核锁后复查（等/抢期间落下的留置标、上一局未正常结束的标记都算数）
            doc = read_json(detained) if detained.is_file() else None
            hd2 = read_json(holder_file) if holder_file.is_file() else None
            if doc is None and hd2 and hd2.get("pid") != pid:
                _self_release(root, serial)
                return 3, _unclean(root, serial, hd2)
            if doc is not None:
                _self_release(root, serial)
                return 3, (f"{serial} 被留置：{doc.get('reason', '?')}（等锁期间落下）"
                           f"；人工核查设备后 `device_lock.py clear --serial {serial}`")
            token = secrets.token_hex(8)
            holder_file.write_text(json.dumps({
                "holder": holder, "pid": int(pid), "daemon": daemon, "token": token,
                "since": int(time.time()), "serial": serial, "note": note,
            }, ensure_ascii=False) + "\n", encoding="utf-8")
            return 0, token
        if r.returncode != 1:
            return 2, (r.stderr or r.stdout).strip() or f"flock_hold take rc={r.returncode}"
        # flock 被占但 holder.json 不在：对方正在取锁（标记未写）或正在放锁（标记已摘）→ 等下一轮
        if time.time() >= deadline:
            return 1, f"{serial} 正被占（锁被持有但持有者标记还没出现 / 刚被摘）"
        time.sleep(POLL_S)


def daemons(root, serial):
    """本机所有盯着这把锁文件的持锁小进程 pid。"""
    _, lock_file, _, _ = paths(root, serial)
    out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
    return [int(ln.split(None, 1)[0]) for ln in out.splitlines()
            if str(lock_file) in ln and "flock_hold" in ln and "hold" in ln]


def release(root, serial, token=None):
    """放锁。(rc, msg)：0 已放/本就空；2 令牌不符 / 锁被占但找不到持锁小进程。"""
    d, lock_file, holder_file, _ = paths(root, serial)
    if not d.is_dir():
        return 0, "no lock"
    doc = read_json(holder_file)
    if doc and doc.get("token") and token != doc["token"]:
        return 2, (f"{serial} 令牌不符，拒放（只有持锁者能放；人工介入用 "
                   f"`device_lock.py clear --serial {serial}`）")
    pids = {int(doc["daemon"])} if doc and doc.get("daemon") else set()
    if not pids or not all(alive(p) for p in pids):
        pids |= set(daemons(root, serial))
    # 先摘持有者标记再放内核锁：放锁中途被看到 =「free 且无标记」，取锁方不会把干净释放当崩溃局
    holder_file.unlink(missing_ok=True)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    deadline = time.time() + 3
    while held(root, serial) and time.time() < deadline:
        time.sleep(0.05)
    if held(root, serial):
        return 2, f"{serial} 的锁仍被占且找不到持锁小进程（holder.json: {doc}）"
    return 0, "released"


def status(root, serial):
    d, lock_file, holder_file, detained = paths(root, serial)
    doc = read_json(holder_file) if holder_file.is_file() else None
    if doc:
        doc = dict(doc, alive=alive(doc.get("pid", 0)),
                   daemon_alive=alive(doc.get("daemon", 0)), token="<隐藏>")
    return {"serial": serial, "dir": str(d), "held": held(root, serial),
            "holder": doc, "detained": detained_reason(root, serial)}


def clear(root, serial):
    """人工核查后重置：清留置标；有未正常结束的标记就连锁一起放（正常在跑的局照样放）。"""
    d, _, holder_file, detained = paths(root, serial)
    detained.unlink(missing_ok=True)
    doc = read_json(holder_file)
    pids = {int(doc["daemon"])} if doc and doc.get("daemon") else set()
    pids |= set(daemons(root, serial))
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    holder_file.unlink(missing_ok=True)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("action", choices=["acquire", "release", "status", "clear"])
    p.add_argument("--serial", required=True)
    p.add_argument("--holder", default=os.environ.get("USER", "?"))
    p.add_argument("--pid", type=int, default=os.getppid(),
                   help="持有者进程号（默认父进程：随调用方退出自动放内核锁；标记留下，下一手按未正常结束留置）")
    p.add_argument("--wait", type=float, default=0)
    p.add_argument("--note", default="")
    p.add_argument("--token", default=None, help="release：acquire 打出的持锁令牌")
    p.add_argument("--root", default=str(lock_root()))
    args = p.parse_args(argv)
    if args.action == "acquire":
        rc, msg = acquire(args.root, args.serial, args.holder, args.pid, args.wait, args.note)
        print(msg if rc else f"locked {args.serial} token={msg}")
        return rc
    if args.action == "release":
        rc, msg = release(args.root, args.serial, token=args.token)
        print(msg)
        return rc
    if args.action == "clear":
        clear(args.root, args.serial)
        print("cleared")
        return 0
    st = status(args.root, args.serial)
    print(f"lock: {'held' if st['held'] else 'free'}  serial={st['serial']}  dir={st['dir']}")
    if st["holder"]:
        h = st["holder"]
        print(f"holder: {h.get('holder')} pid={h.get('pid')} alive={h.get('alive')} "
              f"daemon={h.get('daemon')} since={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(h.get('since', 0)))}"
              + (f" note={h['note']}" if h.get("note") else ""))
    if st["detained"]:
        d_ = st["detained"]
        print(f"DETAINED: {d_.get('reason')}（{d_.get('time')} by {d_.get('holder')}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
