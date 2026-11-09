#!/usr/bin/env python3
"""android/tools/device_lock.py 宿主单测（不起局、不碰设备；内核锁走 tools/flock_hold.py）：
  ① 取锁后 holder.json 带持有者 / 开始时间 / 持锁小进程号；同序列号 wait=0 再取被拒；release 后 free
  ② 持有者进程死掉 → 锁自动失效（判活看持有者进程，不靠文件存不存在）
  ③ 恢复失败留置标存在时 acquire 拒取；clear 后才能再取
  ④ 两线程并发抢同一序列号，恰一个拿到（内核 flock 仲裁，不是「看文件在不在」）
  ⑤ release 不删锁文件：删了重建换 inode，同刻两份打开会各自拿锁（见 tools/flock_hold.py 头注）
  ⑥ CLI：acquire / status / release 退出码与输出
"""
import importlib.util, json, os, subprocess, sys, tempfile, threading, time, unittest
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

    def test_acquire_status_release(self):
        rc, msg = device_lock.acquire(self.root, SERIAL, "tester", os.getpid(), wait=0)
        self.assertEqual(0, rc, msg)
        st = device_lock.status(self.root, SERIAL)
        self.assertTrue(st["held"], st)
        self.assertTrue(st["holder"]["alive"], st)
        holder = json.loads((Path(self.root) / SERIAL / "holder.json").read_text())
        self.assertEqual("tester", holder["holder"])
        self.assertEqual(os.getpid(), holder["pid"])
        self.assertGreater(int(holder["daemon"]), 0)
        self.assertGreater(int(holder["since"]), 0)
        self.assertEqual(SERIAL, holder["serial"])
        # ① 同序列号、另一个“活着的持有者”再取 → 立即被拒
        other = spawn_sleeper()
        try:
            rc, msg = device_lock.acquire(self.root, SERIAL, "second", other.pid, wait=0)
            self.assertNotEqual(0, rc)
        finally:
            other.kill(); other.wait()
        # ⑤ release 不删锁文件，放锁后 status free
        lock_file = Path(self.root) / SERIAL / "device.lock"
        self.assertTrue(lock_file.is_file())
        self.assertEqual(0, device_lock.release(self.root, SERIAL)[0])
        self.assertFalse(device_lock.status(self.root, SERIAL)["held"])
        self.assertTrue(lock_file.is_file(), "release 删了锁文件")

    def test_dead_holder_frees_lock(self):
        sleeper = spawn_sleeper()
        try:
            rc, msg = device_lock.acquire(self.root, SERIAL, "sleeper", sleeper.pid, wait=0)
            self.assertEqual(0, rc, msg)
            self.assertTrue(device_lock.status(self.root, SERIAL)["held"])
        finally:
            sleeper.kill(); sleeper.wait()
        # ② 持有者一死，持锁小进程把锁放掉
        self.assertTrue(wait_free(device_lock, self.root, SERIAL), "holder 死后锁未自动释放")
        # 之后另一个持有者能正常取
        rc, msg = device_lock.acquire(self.root, SERIAL, "next", os.getpid(), wait=0)
        self.assertEqual(0, rc, msg)
        self.assertEqual(0, device_lock.release(self.root, SERIAL)[0])

    def test_detained_blocks_acquire(self):
        device_lock.detain(self.root, SERIAL, "恢复原安装状态失败", holder="run", out="/x/out_1")
        rc, msg = device_lock.acquire(self.root, SERIAL, "tester", os.getpid(), wait=0)
        self.assertEqual(3, rc, msg)
        self.assertIn("恢复", msg)
        device_lock.clear(self.root, SERIAL)
        rc, msg = device_lock.acquire(self.root, SERIAL, "tester", os.getpid(), wait=0)
        self.assertEqual(0, rc, msg)
        self.assertEqual(0, device_lock.release(self.root, SERIAL)[0])

    def test_concurrent_acquire_one_winner(self):
        results, barrier = [], threading.Barrier(4)

        def grab(tag):
            barrier.wait()
            rc, _ = device_lock.acquire(self.root, SERIAL, tag, os.getpid(), wait=0)
            results.append((tag, rc))

        threads = [threading.Thread(target=grab, args=(f"t{i}",)) for i in range(4)]
        for t in threads: t.start()
        for t in threads: t.join()
        winners = [tag for tag, rc in results if rc == 0]
        self.assertEqual(1, len(winners), results)
        self.assertEqual(0, device_lock.release(self.root, SERIAL)[0])

    def test_cli(self):
        tool = str(ANDROID / "tools" / "device_lock.py")
        sleeper = spawn_sleeper()
        try:
            r = subprocess.run([sys.executable, tool, "acquire", "--serial", SERIAL,
                                "--pid", str(sleeper.pid), "--root", self.root, "--holder", "cli"],
                               capture_output=True, text=True)
            self.assertEqual(0, r.returncode, r.stderr)
            r = subprocess.run([sys.executable, tool, "status", "--serial", SERIAL, "--root", self.root],
                               capture_output=True, text=True)
            self.assertEqual(0, r.returncode, r.stderr)
            self.assertIn("held", r.stdout)
            r = subprocess.run([sys.executable, tool, "release", "--serial", SERIAL, "--root", self.root],
                               capture_output=True, text=True)
            self.assertEqual(0, r.returncode, r.stderr)
        finally:
            sleeper.kill(); sleeper.wait()
        r = subprocess.run([sys.executable, tool, "status", "--serial", SERIAL, "--root", self.root],
                           capture_output=True, text=True)
        self.assertIn("free", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
