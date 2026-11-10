#!/usr/bin/env python3
"""Exercise the production Java loader with real, deliberately incompatible JNI libraries."""
import os
import pathlib
import subprocess
import sys
import tempfile

ANDROID = pathlib.Path(__file__).resolve().parents[1]
JAVAC = pathlib.Path(subprocess.check_output(['bash', str(ANDROID / 'tools/jdk17.sh')], text=True).strip()).resolve()
JDK = JAVAC.parent.parent
JAVA = JDK / 'bin/java'

STUBS = {
    'android/view/Surface.java': 'package android.view; public class Surface {}',
    'android/content/res/AssetManager.java': 'package android.content.res; public class AssetManager {}',
    'android/util/Log.java': '''package android.util; public class Log {
        public static final int WARN=5, INFO=4;
        public static int println(int p,String a,String b){return 0;} public static int w(String a,String b){return 0;}
    }''',
    'org/s2arm/android/Stubs.java': '''package org.s2arm.android;
        class TouchInputBridge {}
        class GameSession { static boolean verifyNativeBytes(String a, java.nio.ByteBuffer b) { return true; } static int nativeFileAdmission(String p) { return 1; } }
        class MainActivity { static String requestText(String a, String b, int c) { return null; }
            static void dguardPrompt(String s) {} }
    ''',
    'org/s2arm/android/RuntimeLibrary.java': '''package org.s2arm.android;
        import java.io.File;
        final class RuntimeLibrary {
            final File file;
            RuntimeLibrary(String path) { file = new File(path); }
            boolean override() { return true; }
            String line() { return file.getAbsolutePath(); }
            static class Rejected extends Exception { Rejected(String s) { super(s); } }
            static String jniAbi(Class<?>... ignored) { return "good"; }
            static void verifyLoaded(RuntimeLibrary l, String actual, String a, String[] b, String c) throws Rejected {
                try { if (!l.file.getCanonicalPath().equals(new File(actual).getCanonicalPath()))
                    throw new Rejected("loaded path belongs to prior library");
                } catch (java.io.IOException e) { throw new Rejected(e.toString()); }
            }
        }
    ''',
    'org/s2arm/android/Probe.java': '''package org.s2arm.android;
        public class Probe {
            public static void main(String[] args) throws Exception {
                RuntimeLibrary first = new RuntimeLibrary(args[1]);
                if (args[0].equals("bad")) {
                    try { NativeBridge.load(first, "", null, ""); throw new AssertionError("bad ABI accepted"); }
                    catch (RuntimeLibrary.Rejected e) {
                        if (!e.getMessage().contains("JNI ABI bad")) throw e;
                    }
                    if (NativeBridge.loaded() != null) throw new AssertionError("failed load was accepted");
                    try { NativeBridge.load(new RuntimeLibrary(args[2]), "", null, "");
                        throw new AssertionError("retry accepted");
                    } catch (RuntimeLibrary.Rejected e) {
                        if (!e.getMessage().contains("previous native library load")) throw e;
                        System.out.println("retry refused before another library is loaded");
                    }
                } else if (args[0].equals("missing")) {
                    try { NativeBridge.load(first, "", null, ""); throw new AssertionError("missing library accepted"); }
                    catch (UnsatisfiedLinkError expected) { }
                    try { NativeBridge.load(new RuntimeLibrary(args[2]), "", null, "");
                        throw new AssertionError("retry accepted after load failure");
                    } catch (RuntimeLibrary.Rejected e) {
                        if (!e.getMessage().contains("previous native library load")) throw e;
                        System.out.println("retry after load error refused");
                    }
                } else {
                    NativeBridge.load(first, "", null, "");
                    NativeBridge.load(first, "", null, "");
                    if (NativeBridge.loaded() != first) throw new AssertionError("successful load not retained");
                    System.out.println("valid load and same-library reuse passed");
                }
            }
        }
    ''',
}
NATIVE = r'''
#define _GNU_SOURCE
#include <jni.h>
#include <stdio.h>
#include <dlfcn.h>
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *p) {
    (void)vm; (void)p; puts("JNI_OnLoad " LABEL); fflush(stdout); return JNI_VERSION_1_6;
}
JNIEXPORT jstring JNICALL Java_org_s2arm_android_NativeBridge_loadedPath(JNIEnv *e, jclass c) {
    (void)c; Dl_info i; dladdr((void *)Java_org_s2arm_android_NativeBridge_loadedPath, &i);
    return (*e)->NewStringUTF(e, i.dli_fname);
}
JNIEXPORT jstring JNICALL Java_org_s2arm_android_NativeBridge_jniAbi(JNIEnv *e, jclass c) {
    (void)c; return (*e)->NewStringUTF(e, LABEL);
}
'''


def run():
    with tempfile.TemporaryDirectory(prefix='s2a-jni-load-') as temp:
        root = pathlib.Path(temp).resolve()
        src, classes = root / 'src', root / 'classes'
        for relative, text in STUBS.items():
            path = src / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        production = ANDROID / 'app/src/main/java/org/s2arm/android/NativeBridge.java'
        subprocess.run([str(JAVAC), '-d', str(classes), str(production),
                        *map(str, sorted(src.rglob('*.java')))], check=True)
        code = root / 'fake.c'
        code.write_text(NATIVE)
        system = 'darwin' if sys.platform == 'darwin' else 'linux'
        link = ['-dynamiclib'] if system == 'darwin' else ['-shared', '-fPIC']
        libraries = []
        for label in ('bad', 'good'):
            lib = root / (label + ('.dylib' if system == 'darwin' else '.so'))
            subprocess.run([os.environ.get('CC', 'cc'), *link, '-I', str(JDK / 'include'),
                            '-I', str(JDK / 'include' / system), '-DLABEL="' + label + '"',
                            str(code), '-o', str(lib), *(['-ldl'] if system == 'linux' else [])], check=True)
            libraries.append(lib)
        def probe(mode, first, second):
            result = subprocess.run([str(JAVA), '-cp', str(classes), 'org.s2arm.android.Probe', mode,
                                     str(first), str(second)], text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            if result.returncode:
                raise AssertionError('JNI probe failed:\n' + result.stdout)
            return result.stdout
        negative = probe('bad', *libraries)
        assert 'JNI_OnLoad bad' in negative and 'JNI_OnLoad good' not in negative, negative
        assert 'retry refused before another library is loaded' in negative, negative
        missing = probe('missing', root / 'missing.so', libraries[1])
        assert 'retry after load error refused' in missing and 'JNI_OnLoad good' not in missing, missing
        positive = probe('good', libraries[1], libraries[0])
        assert positive.count('JNI_OnLoad good') == 1 and 'valid load and same-library reuse passed' in positive, positive
        print('native bridge: failed ABI refuses another load; verified library reuse succeeds')


if __name__ == '__main__':
    run()
