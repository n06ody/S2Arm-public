#!/usr/bin/env python3
"""Linux ARM64 lifecycle ordering/failure tests, not a guest quiescence proof."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[5]
PLATFORM = ROOT / "android/native/runtime/platform"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--session-marker-only", action="store_true",
                        help="只编同一个夹具、只跑会话标记那组（#528 第 15 项；android/CMakeLists.txt 的 android_session_marker）")
    parser.add_argument("--baseline-root", type=Path,
                        help="目录内放基线的 runtime/src/lifecycle.c 与 android/native/runtime/platform/lifecycle_android.{c,h}：单线程日志须与它逐行一致")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    records = []
    source = ROOT / "runtime/src/lifecycle.c"
    helper = PLATFORM / "lifecycle_android.c"
    fixture = PLATFORM / "tests/lifecycle.c"

    def command(argv, name, expect=0, env=None):
        start = time.monotonic()
        result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=15, env=env)
        elapsed = time.monotonic() - start
        output = result.stdout + result.stderr
        (out / (name + ".log")).write_text(output)
        records.append({"name": name, "command": list(map(str, argv)),
                        "exit": result.returncode, "seconds": elapsed})
        assert result.returncode == expect, (name, result.returncode, expect, output)
        return output, elapsed

    def build(path, name, helper=helper, gen=False):
        binary = out / name
        gen_flags = ["-DS2A_ANDROID_PGO_GEN=1"] if gen else []
        command([os.environ.get("CC", "cc"), "-D_GNU_SOURCE", "-D__ANDROID__", "-DS2A_UNIT_TEST",
                 *gen_flags,
                 "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden",
                 "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT),
                 "-I" + str(ROOT / "android/tests/hostvk"),
                 "-I" + str(ROOT / "runtime/src"), "-I" + str(ROOT / "runtime/include"),
                 path, helper, fixture, "-pthread", "-ldl", "-Wl,--gc-sections",
                 "-Wl,--wrap=pthread_create,--wrap=pipe2", "-o", binary], "build-" + name)
        return binary

    binary = build(source, "lifecycle")
    # 原样抽出的源文（变异都写到源树外，保留生产头文件路径）；会话标记那组的 dlsym 变异也用它
    original = source.read_text()
    absolute = original.replace('"../../android/native/runtime/platform/lifecycle_android.h"',
                                '"' + str(PLATFORM / "lifecycle_android.h") + '"')

    def session_marker():
        # 每跑一次一个新目录：ctest 反复用同一个 --out，目录 / 链接那两例第二次会撞上上次留下的
        run_dir = Path(tempfile.mkdtemp(prefix="sesmark-", dir=out))
        # 会话标记（#528 第 15 项）：真 lifecycle.c 的安卓支在优雅收尾最后一步按链接期弱引用调真 lifecycle_android.c 的钩子。
        #   期望按外壳 LastSession.java 的约定手写：干净收尾 Java 写 "end\n"、报错停局留 "fg\n"；只删恰好 "end\n"，收尾没走完（125 / 124）一律留着。
        def sesmark_dir(name, content):
            d = run_dir / name
            d.mkdir(parents=True, exist_ok=True)
            m = d / "s2a_session.mark"
            if content is not None:
                m.write_bytes(content)
            return d, m
        def files_env(value):
            env = dict(os.environ)
            env.pop("S2A_TEST_FILES_DIR", None)
            if value is not None:
                env["S2A_TEST_FILES_DIR"] = str(value)
            return env
        for mode in ("normal", "signal", "normal-threaded"):
            d, m = sesmark_dir(mode, b"end\n")
            log, _ = command([binary, mode], "sesmark-" + mode, env=files_env(d))
            assert not m.exists() and "会话标记：干净收尾已走完" in log, (mode, log)
            assert log.index("会话标记") < log.index("优雅退出完成"), log
        d, m = sesmark_dir("error-stop", b"fg\n")
        log, _ = command([binary, "normal"], "sesmark-error-stop", env=files_env(d))
        assert m.is_file() and m.read_bytes() == b"fg\n" and "会话标记" not in log, ("error stop must keep fg", log)
        for mode, code in (("inflight", 125), ("writers-fail", 125), ("cleanup-fail", 125), ("hang", 124)):
            d, m = sesmark_dir(mode, b"end\n")
            log, _ = command([binary, mode], "sesmark-" + mode, code, env=files_env(d))
            assert m.is_file() and m.read_bytes() == b"end\n" and "会话标记" not in log, ("failed shutdown must keep end", mode, log)
        for name, (content, want, kept) in {
                "end": (b"end\n", 1, False), "no-newline": (b"end", 0, True), "partial": (b"en", 0, True),
                "trailing": (b"end\nx", 0, True), "fg": (b"fg\n", 0, True), "bg": (b"bg\n", 0, True),
                "empty": (b"", 0, True), "missing": (None, 0, False)}.items():
            d, m = sesmark_dir("case-" + name, content)
            log, _ = command([binary, "sesmark"], "sesmark-case-" + name, env=files_env(d))
            assert ("sesmark=%d\n" % want) in log and m.exists() == kept, (name, log)
        d, m = sesmark_dir("case-directory", None)
        m.mkdir()
        log, _ = command([binary, "sesmark"], "sesmark-case-directory", env=files_env(d))
        assert "sesmark=0\n" in log and m.is_dir(), log
        d, m = sesmark_dir("case-symlink", None)
        target = d / "elsewhere"
        target.write_bytes(b"end\n")
        m.symlink_to(target)
        log, _ = command([binary, "sesmark"], "sesmark-case-symlink", env=files_env(d))
        assert "sesmark=0\n" in log and m.is_symlink() and target.read_bytes() == b"end\n", log
        for name, value in (("unset", None), ("relative", "relative/dir")):
            log, _ = command([binary, "sesmark"], "sesmark-dir-" + name, env=files_env(value))
            assert "sesmark=-1\n" in log, (name, log)
        # 外壳没调 configureGpuDriver（来源在、但从没设过 = 空串）：end 删不掉，之后每次正常退局都会误报，所以必须报红
        log, _ = command([binary, "normal"], "sesmark-unconfigured", env=files_env(""))
        assert "【红】会话标记：不知道 files 目录" in log and "优雅退出完成" in log, log
        # 负控：安卓支退回 dlsym(RTLD_DEFAULT) 取钩子 → 同库内的隐藏符号取不到，end 留着（docs/坑记录.md 那个坑）
        weak_hook = "    { extern void s2a_sesmark_clear_home(void) __attribute__((weak)); if (s2a_sesmark_clear_home) s2a_sesmark_clear_home(); }"
        assert absolute.count(weak_hook) == 1
        path = out / "dlsym-sesmark.c"
        path.write_text(absolute.replace(weak_hook,
            '    { void (*clr)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_sesmark_clear_home"); if (clr) clr(); }'))
        mutant = build(path, "dlsym-sesmark")
        d, m = sesmark_dir("reject-dlsym", b"end\n")
        log, _ = command([mutant, "normal"], "reject-dlsym-sesmark", env=files_env(d))
        assert m.is_file() and m.read_bytes() == b"end\n" and "优雅退出完成" in log and "会话标记" not in log, ("dlsym mutant must miss the hook", log)

    def receipt(scope):
        (out / "results.json").write_text(json.dumps({
            "scope": scope,
            "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in (source, helper, fixture, PLATFORM / "lifecycle_android.h")},
            "records": records}, indent=2))

    if args.session_marker_only:
        # #528 第 15 项：只跑会话标记那组（android/CMakeLists.txt 的 android_session_marker ctest 项，只在 Linux）。
        #   整套里它挂在 threaded_host_suites.sh 末尾，macOS 跳过、Linux 常被前面挡住；组内又排在 hang 等别的断言后面
        session_marker()
        receipt("session marker only (#528 item 15): clean end deletes end only; failed / stuck shutdowns and dlsym keep it")
        print("PASS: session marker (clean end deletes end only; failed / stuck shutdowns and dlsym keep it)")
        return

    symbols, _ = command(["nm", "-D", binary], "dynamic-symbols")
    assert "S2A_CancelWaits" not in symbols  # Exactly the static archive visibility boundary.
    def marks(log):
        return [line for line in log.splitlines() if line and not line.startswith("[")]
    pgo = build(source, "lifecycle-pgo-gen", gen=True)
    for mode in ("normal", "signal", "normal-threaded"):
        log, _ = command([pgo, mode], "pgo-" + mode)
        events = marks(log)
        assert events.count("pgo-write") == 1 and events.index("cleanup-result") < events.index("pgo-write") < events.index("summary"), log
    for mode, code in (("writers-fail", 125), ("cleanup-fail", 125), ("error-stop", 86), ("hang", 124)):
        log, _ = command([pgo, mode], "pgo-" + mode, code)
        assert "pgo-write" not in marks(log), log
    for mode in ("normal", "signal"):
        # 单线程（基线）：地址余量在取消之前、不限时，没有原生进度行与黄
        log, _ = command([binary, mode], mode)
        assert "pgo-write" not in marks(log), log
        assert marks(log)[:8] == ["unfreeze", "vk-drain", "vm-low", "cancel", "park", "writers", "cs-shutdown", "cleanup-result"], log
        assert log.count("cs-shutdown") == 1 and "优雅退出完成" in log and "收局进度" not in log and "【黄】" not in log, log
        # 拆线程：地址余量在服务层 Cleanup / 结果确认之后，带读 maps 用时
        log, _ = command([binary, mode + "-threaded"], mode + "-threaded")
        assert marks(log)[:8] == ["unfreeze", "vk-drain", "cancel", "park", "writers", "cs-shutdown", "cleanup-result", "vm-low"], log
        assert "优雅退出完成" in log and "读 maps 用时" in log, log
    # 拆线程读 maps 卡住（mmap_lock 竞争）：收局照常完成，只打一行黄
    log, elapsed = command([binary, "maps-stuck-threaded"], "maps-stuck-threaded")
    assert "优雅退出完成" in log and "【黄】地址余量 [收局]" in log and elapsed < 1.3, log
    assert log.index("cs-shutdown") < log.index("vm-low"), log
    # 单线程读 maps 卡住：与基线相同（在取消之前卡住，1.5 s 兜底结束）
    log, _ = command([binary, "maps-stuck"], "maps-stuck", 124)
    assert "cancel" not in marks(log) and "【黄】" not in log, log
    log, _ = command([binary, "vk-unconfigured"], "vk-unconfigured")
    assert "vk-drain" not in log and "优雅退出完成" in log, log
    # 收局只问“停车通道装过没有”：installed=0（从未安装）→ 跳过停稳等待并照常收局；
    # installed=1（含“装过但处理器被覆盖”）→ 仍走停稳等待（见上面 writers-fail 期望 125）。
    log, _ = command([binary, "writers-never-installed"], "writers-never-installed")
    assert "writers\n" not in log and "优雅退出完成" in log and "从未安装" in log, log
    for mode in ("inflight", "writers-fail", "watchdog-fail", "vk-drain-fail"):
        log, _ = command([binary, mode], mode, 125)
        assert "cs-shutdown" not in log and "优雅退出完成" not in log
        if mode != "vk-drain-fail":
            expected = {"inflight": "services still in flight", "writers-fail": "guest/fault writers not settled",
                        "watchdog-fail": "shutdown watchdog start failed"}[mode]
            assert "[exit125] " + expected in log, (mode, log)
    log, _ = command([binary, "vk-drain-fail"], "vk-drain-fail-order", 125)
    assert "vk-drain\n" in log and not any(s in log for s in ("cancel\n", "park\n", "writers\n")), log
    log, _ = command([binary, "cleanup-fail"], "cleanup-fail", 125)
    assert "cs-shutdown\ncleanup-result\n" in log and "优雅退出完成" not in log
    assert "[exit125] services cleanup not confirmed" in log, log
    for mode in ("hang", "full-stderr", "hang-threaded", "full-stderr-threaded"):
        log, elapsed = command([binary, mode], mode, 124)
        assert "优雅退出完成" not in log and 1.3 < elapsed < 3.0
        # 兜底结束进程前有一行明确标识（stderr 管道满时写不进也不阻塞，照常 124）
        assert ("原生兜底" in log) == mode.startswith("hang"), log
        progress = [line for line in log.splitlines() if "收局进度 +" in line and "（原生）" in line]
        if mode == "hang-threaded":
            # 收局进度（原生诊断，拆线程）：兜底结束进程前每 250 ms 一行，带收局线程的内核态与 guest 写者快照
            assert len(progress) >= 4, log
            assert all("收局线程 tid" in line and "系统调用" in line and "guest 写者：" in line for line in progress), log
        elif mode == "hang":
            assert not progress, log   # 单线程保持基线：没有进度线程
    for mode in ("term-thread-fail", "term-pipe-fail"):
        command([binary, mode], mode)

    # 真实 host 日志是普通文件：O_NONBLOCK 不能防止存储阻塞。只阻塞标识写入，最终兜底仍须独立退出。
    def blocked_file(helper_source, name):
        target = out / name
        cmd = next(r["command"][:] for r in records if r["name"] == ("build-baseline-lifecycle" if name.startswith("baseline-") else "build-lifecycle"))
        if str(helper) in cmd: cmd[cmd.index(str(helper))] = str(helper_source)
        cmd[cmd.index("-o") + 1] = str(target)
        cmd += [str(PLATFORM / "tests/watchdog_write.c"), "-Wl,--wrap=write"]
        command(cmd, "build-" + name)
        for mode in ("hang", "hang-threaded"):
            start = time.monotonic()
            with (out / (name + "-" + mode + ".stderr")).open("w") as log:
                result = subprocess.run([str(target), mode], stdout=subprocess.PIPE, stderr=log, text=True, timeout=3)
            elapsed = time.monotonic() - start
            records.append({"name": name + "-" + mode, "exit": result.returncode, "seconds": elapsed, "stdout": result.stdout})
            assert result.returncode == 124 and 1.3 < elapsed < 1.9, (name, mode, elapsed, result.stdout)
        return target
    blocked_file(helper, "blocked-file")

    mutants = {
        "optional-cancel": ("    S2A_CancelWaits();",
                            '    { void (*cancel)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "S2A_CancelWaits"); if (cancel) cancel(); }', 93),
        "skip-writers": ("int error = tracking_installed ? android_shutdown_writers_begin() : 0;", "int error = 0;", 92),
    }
    for name, (before, after, failure) in mutants.items():
        assert absolute.count(before) == 1, name
        path = out / (name + ".c")
        mutant_source = absolute.replace(before, after)
        if name == "skip-writers":
            # Removing the only call must fail at the runtime ownership assertion,
            # not at -Werror for the now-unused private production helper.
            mutant_source = mutant_source.replace("static int android_shutdown_writers_begin(void)",
                "static __attribute__((unused)) int android_shutdown_writers_begin(void)")
        path.write_text(mutant_source)
        mutant = build(path, name)
        log, _ = command([mutant, "normal"], "reject-" + name, failure)
        assert "cs-shutdown" not in log and "优雅退出完成" not in log
    # 负控：拆线程也把地址余量留在收局前段（S2A_CancelWaits 之前）→ 读 maps 卡住时取消请求发不出去，1.5 s 兜底结束（124）
    before = '    if (!s2a_android_shutdown_threaded()) { void s2a_vm_low(const char *); s2a_vm_low("收局"); }\n'
    assert absolute.count(before) == 1
    path = out / "vm-low-before-cancel.c"
    path.write_text(absolute.replace(before, '    { void s2a_vm_low(const char *); s2a_vm_low("收局"); }\n'))
    mutant = build(path, "vm-low-before-cancel")
    log, _ = command([mutant, "maps-stuck-threaded"], "reject-vm-low-before-cancel", 124)
    assert "cancel" not in marks(log) and "cs-shutdown" not in log and "优雅退出完成" not in log, log
    session_marker()

    # 单线程与基线逐行一致（数字归一：用时）
    if args.baseline_root:
        import re
        base_source = args.baseline_root / "runtime/src/lifecycle.c"
        base_helper = args.baseline_root / "android/native/runtime/platform/lifecycle_android.c"
        base_header = args.baseline_root / "android/native/runtime/platform/lifecycle_android.h"
        staged = out / "baseline"
        (staged / "runtime/src").mkdir(parents=True, exist_ok=True)
        (staged / "platform").mkdir(parents=True, exist_ok=True)
        (staged / "platform/lifecycle_android.h").write_text(base_header.read_text())
        (staged / "platform/lifecycle_android.c").write_text(base_helper.read_text())
        (staged / "runtime/src/lifecycle.c").write_text(base_source.read_text().replace(
            '"../../android/native/runtime/platform/lifecycle_android.h"', '"' + str(staged / "platform/lifecycle_android.h") + '"'))
        baseline = build(staged / "runtime/src/lifecycle.c", "baseline-lifecycle", staged / "platform/lifecycle_android.c")
        blocked_file(staged / "platform/lifecycle_android.c", "baseline-blocked-file")
        normalize = lambda text: re.sub(r"[0-9]+\.[0-9]+ 秒", "N 秒", text)
        for mode, expect in (("normal", 0), ("signal", 0), ("inflight", 125), ("writers-fail", 125), ("cleanup-fail", 125), ("maps-stuck", 124)):
            ours, _ = command([binary, mode], "false-" + mode, expect)
            theirs, _ = command([baseline, mode], "baseline-" + mode, expect)
            if expect == 124:
                # 唯一的差别：兜底结束进程前的标识行
                marker = [line for line in ours.splitlines() if "原生兜底" in line]
                assert len(marker) == 1, ours
                ours = "\n".join(line for line in ours.splitlines() if "原生兜底" not in line) + ("\n" if ours.endswith("\n") else "")
            assert normalize(ours) == normalize(theirs), (mode, ours, theirs)
    receipt("lifecycle order and error propagation only; fixture writers are not production quiescence")
    print("PASS: lifecycle failure/order cases, threaded shutdown progress + bounded maps read after StopGpu, single-thread baseline order, three rejected mutants, and the session marker (clean end deletes end only; failed / stuck shutdowns and dlsym keep it)" + ("; single-thread logs equal the baseline" if args.baseline_root else ""))


if __name__ == "__main__":
    main()
