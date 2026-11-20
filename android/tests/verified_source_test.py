#!/usr/bin/env python3
"""Compile the exact production callback against JNI, then drive install -> admission -> mutation -> callback.
Only Android AAsset/log types are stubbed; revision reads, native copying, JNI and Java SHA are real.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ANDROID = Path(__file__).resolve().parents[1]
JAVA = ANDROID/'app/src/main/java/org/s2arm/android'


def main():
    source = (ANDROID/'native/bridge.cpp').read_text()
    callback = source[source.index('static bool verify_game_bytes('):source.index('static void *run_game(')]
    # Desktop JNI exposes void**, Android JNI exposes JNIEnv**. No production behavior is changed.
    callback = callback.replace('AttachCurrentThread(&env, nullptr)', 'AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr)')
    wrapper = (JAVA/'NativeBridge.java').read_text()
    method = next(x.strip() for x in wrapper.splitlines() if x.strip().startswith('static boolean verifyGameBytes('))
    admission = next(x.strip() for x in wrapper.splitlines() if x.strip().startswith('static int gameFileAdmission('))
    # JDK 与安卓整套其余步同一个查找口径（android/tools/jdk17.sh：S2A_JAVAC → JAVA_HOME → java_home -v 17 → Homebrew → 发行版 → 门禁主机副本）；
    #   原先直接读 JAVA_HOME，没设时崩成 KeyError，看不出是缺 JDK（11-18 一台门禁主机缺 JDK 时连红）
    found = subprocess.run(['bash', str(ANDROID/'tools/jdk17.sh')], capture_output=True, text=True)
    if found.returncode != 0 or not found.stdout.strip():
        sys.exit('【红】verified_source_test：找不到 JDK 17（android/tools/jdk17.sh 无结果），装好 JDK 17 或设 JAVA_HOME / S2A_JAVAC 后再跑')
    home = Path(found.stdout.strip()).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='s2a-verified-source-') as directory:
        out = Path(directory)
        (out / 'classes').mkdir()
        (out/'NativeBridge.java').write_text('package org.s2arm.android; final class NativeBridge { ' + method + ' ' + admission + ' }\n')
        cpp = r'''
#include <jni.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <sys/types.h>
using off64_t = off_t;
struct AAsset {};
struct AAssetManager {};
#define AASSET_MODE_BUFFER 0
#define ANDROID_LOG_ERROR 0
static int __android_log_print(int, const char *, const char *, ...) { return 0; }
static AAsset *AAssetManager_open(AAssetManager *, const char *, int) { return nullptr; }
static off_t AAsset_getLength64(AAsset *) { return -1; }
static const void *AAsset_getBuffer(AAsset *) { return nullptr; }
static void AAsset_close(AAsset *) {}
struct s2a_loader_blob { const void *bytes; size_t length; void *lease; };
struct AndroidHost {
    std::string asset_root;
    AAssetManager *assets = nullptr;
    std::unordered_map<std::string,std::string> asset_paths;
};
static JavaVM *g_vm;
static jclass g_bridge;
'''
        cpp += callback
        cpp += r'''
extern "C" JNIEXPORT jint JNICALL Java_org_s2arm_android_VerifiedSourceProbe_read(JNIEnv *env, jclass, jstring path) {
    env->GetJavaVM(&g_vm);
    if (!g_bridge) g_bridge = static_cast<jclass>(env->NewGlobalRef(env->FindClass("org/s2arm/android/NativeBridge")));
    const char *chars = env->GetStringUTFChars(path,nullptr);
    AndroidHost host; host.asset_paths.emplace("main.patched.bin", chars);
    env->ReleaseStringUTFChars(path,chars);
    s2a_loader_blob blob{};
    int rc = read_game_asset(&host,"main.patched.bin",&blob);
    release_game_asset(&host,&blob);
    return rc;
}
'''
        (out/'probe.cpp').write_text(cpp)
        platform = 'darwin' if sys.platform == 'darwin' else 'linux'
        library = out/'probe.so'
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        '-I'+str(home/'include'),'-I'+str(home/'include'/platform), str(out/'probe.cpp'),'-o',str(library)],check=True)
        sources = ['Json', 'BootContract', 'BootSpec', 'RuntimeLibrary', 'GamePack', 'GameFingerprint', 'GameInstall',
                   'SaveArchive', 'ImportReceiver', 'GameStorage', 'GameSession', 'GameResume']
        subprocess.run([str(home/'bin/javac'),'-encoding','UTF-8','-d',str(out/'classes'),
                        *[str(JAVA/(name+'.java')) for name in sources],
                        str(ANDROID/'tests/stubs/android/util/Log.java'),str(out/'NativeBridge.java'),
                        str(ANDROID/'tests/GameSessionAdmissionTest.java'),str(ANDROID/'tests/VerifiedSourceProbe.java')],check=True)
        subprocess.run([str(home/'bin/java'),'-cp',str(out/'classes'),'org.s2arm.android.VerifiedSourceProbe',str(library)],check=True)
        subprocess.run([str(home/'bin/java'),'-cp',str(out/'classes'),'org.s2arm.android.GameSessionAdmissionTest'],check=True)


if __name__ == '__main__':
    main()
