#!/usr/bin/env python3
"""存档原子态提示的 Java 轮询（MainActivity.saveAtomicSample）方法边界回归（#961）。
原样抽出生产里的 Runnable 字段编进脚手架；NativeBridge / Handler / Toast / Log 是记录型替身，
只模拟 pollSaveAtomic 的返回值，不模拟 Android 界面或设备。期望按口径手写：
正数 = 原因位 → 提示一次、继续轮询；0 → 不提示、继续轮询；负数 = 状态未知 → 不提示、记一条错、本局不再轮询。
另跑一个负对照：删掉生产里处理负数的那一行，「未知」用例必须重新变红。
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

FIELD = '    private final Runnable saveAtomicSample = new Runnable() {'
UNKNOWN_LINE = '            if (bits < 0) {'


def extract(source):
    if source.count(FIELD) != 1:
        raise ValueError('expected exactly one production saveAtomicSample field')
    begin = source.index(FIELD)
    brace = source.index('{', begin + len('    private final Runnable saveAtomicSample = new Runnable()'))
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if source[end] != ';':
        raise ValueError('saveAtomicSample field must end with };')
    return source[begin:end + 1]


SCAFFOLD = r'''
import java.util.*;
class Log {
    static List<String> errors = new ArrayList<>();
    static int e(String tag, String message) { errors.add(message); return 0; }
    static int i(String tag, String message) { return 0; }
}
class NativeBridge {
    static int value, calls;
    static int pollSaveAtomic() { calls++; return value; }
}
class android {
    static class os {
        static class Looper { static Looper getMainLooper() { return new Looper(); } }
        static class Handler {
            static int posts;
            Handler(Looper looper) { }
            boolean postDelayed(Runnable action, long delay) {
                MainActivity.check(delay == 1000, "probe must repost after one second");
                posts++; return true;
            }
        }
    }
    static class widget {
        static class Toast {
            static final int LENGTH_LONG = 1;
            static int shown;
            static Toast makeText(MainActivity owner, String text, int duration) {
                MainActivity.check(text.equals(MainActivity.SAVE_ATOMIC_NOTICE) && duration == LENGTH_LONG, "notice text");
                return new Toast();
            }
            void show() { shown++; }
        }
    }
}
class MainActivity {
    static final String SAVE_ATOMIC_NOTICE = "notice";
    boolean gameMode = true, gameStopping, runtimeLoaded = true;
    private final android.os.Handler saveAtomicProbe = new android.os.Handler(android.os.Looper.getMainLooper());
@@FIELD@@
    static int fails;
    static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    static void run(String name, int value, boolean stopping, int calls, int toasts, int posts, int errors) {
        NativeBridge.value = value; NativeBridge.calls = 0; android.widget.Toast.shown = 0;
        android.os.Handler.posts = 0; Log.errors.clear();
        MainActivity activity = new MainActivity(); activity.gameStopping = stopping;
        activity.saveAtomicSample.run();
        boolean ok = NativeBridge.calls == calls && android.widget.Toast.shown == toasts
            && android.os.Handler.posts == posts && Log.errors.size() == errors;
        System.out.println((ok ? "  ✓ " : "✗ ") + name + ": calls=" + NativeBridge.calls + "/" + calls
            + " toasts=" + android.widget.Toast.shown + "/" + toasts + " posts=" + android.os.Handler.posts + "/" + posts
            + " errors=" + Log.errors.size() + "/" + errors);
        if (!ok) fails++;
    }
    public static void main(String[] args) {
        run("原因位：提示一次、继续轮询", 2, false, 1, 1, 1, 0);
        run("安全 0：不提示、继续轮询", 0, false, 1, 0, 1, 0);
        run("未知：不提示、记错、停止轮询", -1, false, 1, 0, 0, 1);
        run("收局中：不问", 2, true, 0, 0, 0, 0);
        System.out.println(fails == 0 ? "✓ save_atomic_probe：4 例全符" : "✗ save_atomic_probe：" + fails + " 例不符");
        System.exit(fails == 0 ? 0 : 1);
    }
}
'''


def run(source_text, out, javac, java):
    out.mkdir(parents=True, exist_ok=True)
    harness = out / 'MainActivity.java'
    harness.write_text(SCAFFOLD.replace('@@FIELD@@', extract(source_text)))
    subprocess.run([javac, '-encoding', 'UTF-8', '-d', str(out), str(harness)], check=True)
    return subprocess.run([java, '-cp', str(out), 'MainActivity']).returncode


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path,
                   default=Path(__file__).resolve().parents[1] / 'app/src/main/java/org/s2arm/android/MainActivity.java')
    p.add_argument('--out', type=Path)
    p.add_argument('--javac', default=shutil.which('javac') or 'javac')
    p.add_argument('--java', default=shutil.which('java') or 'java')
    p.add_argument('--no-negative-control', action='store_true', help='only run the given source (pre-fix red)')
    a = p.parse_args()
    source = a.source.read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory() as tmp:
        out = a.out or Path(tmp)
        rc = run(source, out / 'candidate', a.javac, a.java)
        if rc or a.no_negative_control:
            return rc
        if source.count(UNKNOWN_LINE) != 1:
            raise ValueError('expected exactly one production unknown-state line')
        mutant = '\n'.join(l for l in source.split('\n') if not l.startswith(UNKNOWN_LINE))
        if not run(mutant, out / 'mutant', a.javac, a.java):
            print('✗ save_atomic_probe 负对照：删掉负数处理后仍全绿')
            return 1
        print('✓ save_atomic_probe 负对照：删掉负数处理 → 未知用例红')
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
