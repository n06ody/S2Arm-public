#!/usr/bin/env python3
"""Android 设备锁：同一台设备同一时刻只有一个局（tools/flock_hold.py 内核锁 + 持有者元数据）。

用法：
  device_lock.py acquire --serial S [--holder NAME] [--pid P] [--wait S] [--note TXT]
  device_lock.py release --serial S
  device_lock.py status  --serial S
  device_lock.py clear   --serial S

锁目录（持久，不放 /tmp）：<锁根>/<serial>/
  device.lock    内核文件锁本体，由 flock_hold.py 的持锁小进程盯持有者 PID 自动收尸
  holder.json    持有者 / 持有者 pid / 持锁小进程 pid / 开始时间（给人和 status 看，不作判据）
  detained.json  起局后还原原安装状态失败时落下；acquire 一律拒，人工 clear 才再放行

判活与仲裁只靠内核 flock + 持有者进程死活，不做「看锁文件在不在」的脆弱判定；
锁文件永不删除（删了重建换 inode 会分裂，见 tools/flock_hold.py 头注）。

锁根：--root > S2A_ANDROID_LOCK_ROOT > $S2A_RUN_ROOT/android-devices > ~/.s2arm/run/android-devices。

acquire 退出码：0 取到；1 到时限没取到；2 起不了持锁进程 / 锁文件打不开；3 设备被留置。
release 退出码：0 已放 / 本就空闲；2 锁被占但找不到持锁小进程（别硬删锁文件，查 ps）。
"""
import argparse, json, os, re, signal, subprocess, sys, time
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


def acquire(root, serial, holder, pid, wait=0, note=""):
    """取锁。(rc, msg)：0=取到 msg=持锁小进程 pid；1=忙；2=起不了锁；3=被留置。"""
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
        r = subprocess.run([sys.executable, str(FLOCK_HOLD), str(lock_file),
                            str(pid), "take", LABEL], capture_output=True, text=True)
        if r.returncode == 0:
            daemon = int(r.stdout.strip().split("\t")[0])
            holder_file.write_text(json.dumps({
                "holder": holder, "pid": int(pid), "daemon": daemon,
                "since": int(time.time()), "serial": serial, "note": note,
            }, ensure_ascii=False) + "\n", encoding="utf-8")
            return 0, str(daemon)
        if r.returncode != 1:
            return 2, (r.stderr or r.stdout).strip() or f"flock_hold take rc={r.returncode}"
        if time.time() >= deadline:
            return 1, f"{serial} 正被占（{holder_file.read_text().strip() if holder_file.is_file() else '?'}）"
        time.sleep(POLL_S)


def daemons(root, serial):
    """本机所有盯着这把锁文件的持锁小进程 pid。"""
    _, lock_file, _, _ = paths(root, serial)
    out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
    return [int(ln.split(None, 1)[0]) for ln in out.splitlines()
            if str(lock_file) in ln and "flock_hold" in ln and "hold" in ln]


def release(root, serial):
    """放锁。(rc, msg)：0 已放/本就空；2 找不到持锁小进程。"""
    d, lock_file, holder_file, detained = paths(root, serial)
    if not d.is_dir():
        return 0, "no lock"
    doc = read_json(holder_file) or {}
    pids = {int(doc["daemon"])} if doc.get("daemon") else set()
    if not pids or not all(alive(p) for p in pids):
        pids |= set(daemons(root, serial))
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
    holder_file.unlink(missing_ok=True)
    return 0, "released"


def status(root, serial):
    d, lock_file, holder_file, detained = paths(root, serial)
    doc = read_json(holder_file) if holder_file.is_file() else None
    if doc:
        doc = dict(doc, alive=alive(doc.get("pid", 0)),
                   daemon_alive=alive(doc.get("daemon", 0)))
    return {"serial": serial, "dir": str(d), "held": held(root, serial),
            "holder": doc, "detained": detained_reason(root, serial)}


def detain(root, serial, reason, holder="", out=""):
    """落下留置标：恢复安装状态失败后，人工核查前不再放行。"""
    d, _, _, detained = paths(root, serial)
    d.mkdir(parents=True, exist_ok=True)
    detained.write_text(json.dumps({"serial": serial, "reason": reason, "holder": holder,
                                    "out": out, "since": int(time.time()),
                                    "time": time.strftime("%Y-%m-%d %H:%M:%S")},
                                   ensure_ascii=False) + "\n", encoding="utf-8")


def clear(root, serial):
    _, _, _, detained = paths(root, serial)
    detained.unlink(missing_ok=True)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("action", choices=["acquire", "release", "status", "clear"])
    p.add_argument("--serial", required=True)
    p.add_argument("--holder", default=os.environ.get("USER", "?"))
    p.add_argument("--pid", type=int, default=os.getppid(),
                   help="持有者进程号（默认父进程：随调用方退出自动放锁）")
    p.add_argument("--wait", type=float, default=0)
    p.add_argument("--note", default="")
    p.add_argument("--root", default=str(lock_root()))
    args = p.parse_args(argv)
    if args.action == "acquire":
        rc, msg = acquire(args.root, args.serial, args.holder, args.pid, args.wait, args.note)
        print(msg if rc else f"locked {args.serial} daemon={msg}")
        return rc
    if args.action == "release":
        rc, msg = release(args.root, args.serial)
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
