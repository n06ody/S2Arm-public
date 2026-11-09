#!/usr/bin/env python3
"""android/tools/device_lock.py 宿主单测（不起局、不碰设备；内核锁走 tools/flock_hold.py）：
  ① 取锁后 holder.json 带持有者 / 开始时间 / 持锁小进程号 / 令牌；同序列号 wait=0 再取被拒（rc=1）；
    release 要令牌：错令牌 rc=2 且不放开，对令牌才 free
  ② 持有者进程死掉（崩溃 / 强杀 / 掉线）：锁内核自动放，但设备状态未证 → 下一手取锁判留置（rc=3）、
    落 detained.json；人工 clear 后才放行（不自动放）
  ③ 恢复失败留置标存在时 acquire 拒取；clear 后才能再取
  ④ 两线程并发抢同一序列号，恰一个拿到（内核 flock 仲裁，不是「看文件在不在」）
  ⑤ release 不删锁文件：删了重建换 inode，同刻两份打开会各自拿锁（见 tools/flock_hold.py 头注）
  ⑥ CLI：acquire / status / release 退出码与输出（release 带 --token）
  ⑦ 取锁与留置复查的竞态：等锁期间设备被留置 → 取到 flock 也要复查出来、自己放掉并 rc=3
"""
import importlib.util, json, os, re, subprocess, sys, tempfile, threading, time, unittest
from pathlib import Path

ANDROID = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("device_lock", ANDROID / "tools" / "device_lock.py")
device_lock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device_lock)

SERIAL = "TESTSERIAL01"


def spawn_sleeper():
    return subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])


def wait_free(lock, root, serial, seconds=10.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if not lock.status(root, serial)["held"]:
            return True
        time.sleep(0.1)
    return False


class DeviceLockTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="s2a-dlock-")

    def test_acquire_status_release_token(self):
        rc, token = device_lock.acquire(self.root, SERIAL, "tester", os.getpid(), wait=0)
        self.assertEqual(0, rc, token)
        st = device_lock.status(self.root, SERIAL)
        self.assertTrue(st["held"], st)
        self.assertTrue(st["holder"]["alive"], st)
        holder = json.loads((Path(self.root) / SERIAL / "holder.json").read_text())
        self.assertEqual("tester", holder["holder"])
        self.assertEqual(os.getpid(), holder["pid"])
        self.assertGreater(int(holder["daemon"]), 0)
        self.assertGreater(int(holder["since"]), 0)
        self.assertEqual(SERIAL, holder["serial"])
        self.assertTrue(re.fullmatch(r"[0-9a-f]{8,}", str(holder["token"])), holder)
        self.assertEqual(token, holder["token"])
        # ① 同序列号、另一个“活着的持有者”再取 → 忙（rc=1）
        other = spawn_sleeper()
        try:
            rc, _ = device_lock.acquire(self.root, SERIAL, "second", other.pid, wait=0)
            self.assertEqual(1, rc)
        finally:
            other.kill(); other.wait()
        # ⑤ release 不删锁文件；① 错令牌拒放、对令牌才放
        lock_file = Path(self.root) / SERIAL / "device.lock"
        self.assertTrue(lock_file.is_file())
        rc, msg = device_lock.release(self.root, SERIAL, token="deadbeef")
        self.assertEqual(2, rc, msg)
        self.assertTrue(device_lock.status(self.root, SERIAL)["held"], "错令牌把锁放了")
        rc, _ = device_lock.release(self.root, SERIAL, token=token)
        self.assertEqual(0, rc)
        self.assertFalse(device_lock.status(self.root, SERIAL)["held"])
        self.assertTrue(lock_file.is_file(), "release 删了锁文件")

    def test_dead_holder_detains_not_autofree(self):
        sleeper = spawn_sleeper()
        try:
            rc, _ = device_lock.acquire(self.root, SERIAL, "sleeper", sleeper.pid, wait=0)
            self.assertEqual(0, rc)
            self.assertTrue(device_lock.status(self.root, SERIAL)["held"])
        finally:
            sleeper.kill(); sleeper.wait()
        # ② 持有者一死，内核锁被持锁小进程放掉；但当局未正常收局 → 留置，不放行下一手
        self.assertTrue(wait_free(device_lock, self.root, SERIAL), "holder 死后内核锁未自动释放")
        rc, msg = device_lock.acquire(self.root, SERIAL, "next", os.getpid(), wait=0)
        self.assertEqual(3, rc, msg)
        self.assertTrue((Path(self.root) / SERIAL / "detained.json").exists(), "崩溃局应留置")
        # 人工 clear → 才能再取
        device_lock.clear(self.root, SERIAL)
        rc, token = device_lock.acquire(self.root, SERIAL, "next", os.getpid(), wait=0)
        self.assertEqual(0, rc)
        self.assertEqual(0, device_lock.release(self.root, SERIAL, token=token)[0])

    def test_detained_blocks_acquire(self):
        device_lock.detain(self.root, SERIAL, "恢复原安装状态失败", holder="run", out="/x/out_1")
        rc, msg = device_lock.acquire(self.root, SERIAL, "tester", os.getpid(), wait=0)
        self.assertEqual(3, rc, msg)
        self.assertIn("恢复", msg)
        device_lock.clear(self.root, SERIAL)
        rc, token = device_lock.acquire(self.root, SERIAL, "tester", os.getpid(), wait=0)
        self.assertEqual(0, rc)
        self.assertEqual(0, device_lock.release(self.root, SERIAL, token=token)[0])

    def test_detained_lands_during_wait(self):
        """⑦ 等锁时设备被留置：即便随后抢到 flock 也要复查出来，自己放掉并拒（竞态负控）。"""
        rc, token = device_lock.acquire(self.root, SERIAL, "first", os.getpid(), wait=0)
        self.assertEqual(0, rc)
        result = []

        def second():
            result.append(device_lock.acquire(self.root, SERIAL, "second", os.getpid(), wait=5))

        t = threading.Thread(target=second)
        t.start()
        time.sleep(0.4)
        device_lock.detain(self.root, SERIAL, "等锁期间还原失败", holder="first")
        device_lock.release(self.root, SERIAL, token=token)
        t.join(10)
        self.assertEqual(3, result[0][0], result)
        self.assertFalse(device_lock.status(self.root, SERIAL)["held"], "复查出留置后应把抢到的锁放掉")

    def test_concurrent_acquire_one_winner(self):
        results, barrier = [], threading.Barrier(4)

        def grab(tag):
            barrier.wait()
            results.append(device_lock.acquire(self.root, SERIAL, tag, os.getpid(), wait=0))

        threads = [threading.Thread(target=grab, args=(f"t{i}",)) for i in range(4)]
        for t in threads: t.start()
        for t in threads: t.join()
        winners = [r for r, _token in results if r == 0]
        self.assertEqual(1, len(winners), results)
        token = next(t for r, t in results if r == 0)
        self.assertEqual(0, device_lock.release(self.root, SERIAL, token=token)[0])

    def test_cli(self):
        tool = str(ANDROID / "tools" / "device_lock.py")
        sleeper = spawn_sleeper()
        try:
            r = subprocess.run([sys.executable, tool, "acquire", "--serial", SERIAL,
                                "--pid", str(sleeper.pid), "--root", self.root, "--holder", "cli"],
                               capture_output=True, text=True)
            self.assertEqual(0, r.returncode, r.stderr)
            m = re.search(r"token=([0-9a-f]+)", r.stdout)
            self.assertTrue(m, r.stdout)
            r = subprocess.run([sys.executable, tool, "status", "--serial", SERIAL, "--root", self.root],
                               capture_output=True, text=True)
            self.assertEqual(0, r.returncode, r.stderr)
            self.assertIn("held", r.stdout)
            # ① 不带令牌 release → 拒
            r = subprocess.run([sys.executable, tool, "release", "--serial", SERIAL, "--root", self.root],
                               capture_output=True, text=True)
            self.assertEqual(2, r.returncode)
            r = subprocess.run([sys.executable, tool, "release", "--serial", SERIAL, "--root", self.root,
                                "--token", m.group(1)], capture_output=True, text=True)
            self.assertEqual(0, r.returncode, r.stderr)
        finally:
            sleeper.kill(); sleeper.wait()
        r = subprocess.run([sys.executable, tool, "status", "--serial", SERIAL, "--root", self.root],
                           capture_output=True, text=True)
        self.assertIn("free", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
