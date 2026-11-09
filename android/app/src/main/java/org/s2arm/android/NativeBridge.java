package org.s2arm.android;

import android.view.Surface;

/** All host lifecycle calls are serialized by the Activity's main thread. */
final class NativeBridge {
    private static volatile RuntimeLibrary loaded;
    // A failed System.load or post-load verification may leave native code mapped.
    // The process cannot safely make another selection after that attempt.
    private static boolean loadAttempted;

    private NativeBridge() { }

    /**
     * The only place libs2arm_android.so is loaded (TouchInputBridge and the rest bind to it): main
     * thread, before anything calls a native method. A process keeps the library it loaded first,
     * so a later launch that selects another one is refused (cold start needed). After the load the
     * library must be the selected file and carry the JNI ABI this Java side was compiled with.
     */
    static synchronized void load(RuntimeLibrary library, String apkPath, String[] splitPaths, String nativeLibraryDir)
            throws RuntimeLibrary.Rejected {
        if (loaded == library) return;   // RuntimeLibrary.select handed back what this process runs
        if (loaded != null)
            throw new RuntimeLibrary.Rejected("this process already runs \"" + loaded.line() + "\"; cold start needed");
        if (loadAttempted)
            throw new RuntimeLibrary.Rejected("previous native library load failed; restart the process before retrying");
        loadAttempted = true;
        long start = System.nanoTime();
        if (library.override()) System.load(library.file.getAbsolutePath());
        else System.loadLibrary("s2arm_android");
        long ms = (System.nanoTime() - start) / 1_000_000;   // carries the .NET runtime in the single-library build
        RuntimeLibrary.verifyLoaded(library, loadedPath(), apkPath, splitPaths, nativeLibraryDir);
        String java = RuntimeLibrary.jniAbi(NativeBridge.class, TouchInputBridge.class), lib = jniAbi();
        if (!java.equals(lib))
            throw new RuntimeLibrary.Rejected("JNI ABI " + lib + " of the library is not this APK's " + java
                + " (native declarations changed: rebuild both)");
        loaded = library;
        android.util.Log.println(library.override() ? android.util.Log.WARN : android.util.Log.INFO, "S2Arm",
            library.line() + "; libs2arm_android.so loaded in " + ms + " ms");
    }

    /** The verified selection; null before success, including a failed mapped library. */
    static RuntimeLibrary loaded() { return loaded; }

    /** The file the dynamic linker loaded this library from (dladdr), as it recorded it. */
    private static native String loadedPath();
    /** native/jni_abi.py's hash of the native declarations the library was built against. */
    private static native String jniAbi();
    /**
     * red.log 的落盘目录：当前应用 getExternalFilesDir()（取不到 / 不可写时 Java 侧退
     * getFilesDir()）。native 只拼文件名，不写死包名（#1357）。启动加载库后调用一次。
     */
    static native int configureRedLogDir(String dir);
    /**
     * 直连编译诊断的根目录：当前应用 getExternalFilesDir()（外部私有目录，adb pull
     * 免 run-as）。native 在其下拼 direct-diagnostics，不写死包名。失败不阻断，
     * 诊断仍写会话内部目录。启动加载库后调用一次。
     */
    static native int configureDiagDir(String dir);
    static native int configureFrameLogDir(String dir);
    static native int configureLanDiscovery(Object provider);
    static native void lanSnapshot(long generation, String ownName, String[] names, String[] addresses, int[] ports);
    static native void lanUnavailable(long generation);
    static native int configureDebugEnvironment(byte[][] entries);
    /**
     * GS+遮挡查询运行时探测（#1456）：dir 为 files/capprobe 目录，badlist 为
     * known-bad.txt 的文本（assets/capprobe/known-bad.txt），appVersion 写进结果文件，
     * helperPath 为 APK 内探测助手的装载 spec（"&lt;sourceDir&gt;!/lib/arm64-v8a/
     * libs2a_capprobe.so"；native 以系统 linker64 posix_spawn 它；空串表示
     * 助手缺失，探测按不可发起处理，方向安全）。
     * 只在 :game 进程、startGame 前调用一次；native 在图形选择前读这些值并驱动 Java 探测。
     */
    static native void configureCapProbe(String dir, String badlist, String appVersion, String helperPath);
    /** CapProbe 的臂运行器：一臂一独立进程，返回一条合同 JSON 行；由 native 探测编排经 JNI 调回 Java 后回调进 native。 */
    static native String capProbeArm(String arm, int timeoutMs);
    /** 能力层结果文件指针：把本指纹的判定文件路径写进 S2A_CAPPROBE_RESULT，通用与直连都读它。 */
    static native void capProbePublish(String resultPath);

    static native long attachHostUi();
    static native void detachHostUi(long generation);
    static native byte[] takeHostUi(long generation, long[] requestId);
    static native int completeHostUi(long generation, long requestId, byte[] reply);

    /** @param moduleDir where tools/device_selftest.sh imports the rewritten self-test module */
    static native long create(String moduleDir);
    static native boolean gameRuntimeAvailable();
    static native int configureInlineSmeContext(boolean enabled);
    static native int configureDmaPreserveRead(boolean enabled);
    static native int configureReadonlyRangeCheck(boolean enabled);
    /** Vulkan 款直连的冷启动选择：VulkanDirectChoice.GENERIC / DIRECT / AUTO（#274）。冻结后改选返回 EBUSY。 */
    static native int configureVulkanDirect(int choice);
    /**
     * 镜像写跟踪生产开关（#1184，owner #1285）：缺省关，宿主创建前设一次。
     * 冷启动 --es s2arm.mirror on|off 经这里到原生；用户版设备只用启动参数，不读 debug 属性。
     */
    static native int configureMirror(boolean enabled);
    static native int configureBufferReadScratch(boolean enabled);
    /** 存储缓冲越界记录诊断（缺省关）：设 S2A_SSBO_OOB_DIAG，宿主创建前才能改。 */
    static native int configureSsboOobDiag(boolean enabled);
    /** 翻译规则/profile 冻结诊断（缺省关），只能在宿主创建前设置。 */
    static native int configureProfileFreeze(boolean enabled);
    /** 拆线程：GAL 套 ThreadedRenderer，GPU 模拟与 Vulkan 录制提交分两条线程。mode = "0"（单线程基线）| "1" | "auto"（按本游戏上局记录）；只能在宿主创建前设置。 */
    static native int configureThreadedRenderer(String mode);
    /** 游戏宿主只提供 lease；不会启动诊断 presenter 或执行诊断自测。 */
    static native long createGame();
    /** 返回 0 表示启动 worker 已接收冻结输入，不代表 guest 已运行。 */
    /** assetRoot=game/modules: exact APK assets; empty: admitted revision paths relative to revisionDir. No mixed sources. */
    /** Native loader checks stable copied bytes against admission; never reopens a path here. */
    static boolean verifyGameBytes(String path, java.nio.ByteBuffer bytes) { return GameSession.verifyNativeBytes(path, bytes); }
    static int gameFileAdmission(String path) { return GameSession.nativeFileAdmission(path); }

    /** intent extra s2arm.draw_log=1|0 → 原生 draw_log 覆盖；-1 不动（按属性）。 */
    static native void setDrawLog(int value);
    static native int startGame(long host, android.content.res.AssetManager assets, String assetRoot,
                               String[] names, String[] images, String[] fixups, String[] gates, String[] widen,
                               String npdm, String gameDataDir, String stateDir, long programId,
                               String bootSpec, int consoleMode, int modePreference,
                               byte[] graphicsDescriptor);
    /** 锁存停止并交给普通 native 退出线程；不销毁仍可能被 guest/GPU 使用的宿主。 */
    static native void stopGame(long host, String reason, boolean normal);
    static native String describe(long host);
    /** Actual game queue-present count; -1 for the diagnostic renderer. */
    static native long gamePresentCount();
    /**
     * B3 buffer_geometry: 交换链已按游戏输出尺寸出图时返回 1 并把输出尺寸写进 out[0]/out[1]
     * （游戏像素）；否则返回 0、out 写 {0,0}。MainActivity 用它把 SurfaceView 布成同比例居中。
     */
    static native int bufferGeometry(int[] out);
    /**
     * Registers the process's HostApi (runtime/include/s2a_hostapi.h) with the session's frozen
     * BootSpec user values; once per process (later calls keep the first values). Returns what the
     * core reads back through it.
     */
    static native String registerHost(String language, String region, String timezone, int dpiX, int dpiY, int consoleMode);
    /** Debug: asks for a session stop through the HostApi stop table from the calling thread, as the core does. */
    static native void debugRequestStop(int reason, String detail);

    /** The shell's side of HostApi stop: the runtime asks to end the session (main thread). */
    interface SessionStopListener { void onSessionStop(int reason, String detail); }
    private static SessionStopListener stopListener;
    private static int pendingReason = -1;
    private static String pendingDetail;

    /** Main thread. A request that came while no Activity listened is delivered now. */
    static void setStopListener(SessionStopListener listener) {
        stopListener = listener;
        if (listener != null && pendingReason >= 0) {
            int reason = pendingReason;
            pendingReason = -1;
            listener.onSessionStop(reason, pendingDetail);
        }
    }

    static void clearStopListener(SessionStopListener listener) {
        if (stopListener == listener) stopListener = null;
    }

    /** From the native looper callback on the main thread; at most once per process. */
    private static void onStopRequested(int reason, byte[] detail) {
        String text = new String(detail, java.nio.charset.StandardCharsets.UTF_8);
        android.util.Log.w("S2Arm", "HostApi stop requested: " + stopReason(reason) + (text.isEmpty() ? "" : " (" + text + ")"));
        if (stopListener != null) stopListener.onSessionStop(reason, text);
        else { pendingReason = reason; pendingDetail = text; }
    }

    /** S2A_HOST_STOP_* as words. */
    static String stopReason(int reason) {
        switch (reason) {
            case 0: return "exit requested";
            case 1: return "low memory";
            case 2: return "GPU device lost";
            case 3: return "unrecoverable error";
            default: return "reason " + reason;
        }
    }
    /** surfaceCreated: offers the surface to the render thread as a new window generation; never waits. */
    static native String offerSurface(long host, Surface surface);
    /** surfaceChanged / insets: a new revision of the current window (Surface.ROTATION_*, pixels, Hz); never waits. */
    static native String updateSurface(long host, int width, int height, int rotation,
                                       int left, int top, int right, int bottom, float refreshHz);
    /** Foreground state for the render thread; never waits. */
    static native void setForeground(long host, boolean foreground);
    /** Focus/lifecycle gate; the native owner thread applies stream changes. */
    static native void setAudioOutputAllowed(boolean allowed);
    /** 运行中切掌机 / 主机（接口 v2.15，会话在跑才调）：0=已应用；<0=宿主拒绝。 */
    static native int setConsoleMode(long host, int mode);
    /**
     * surfaceDestroyed only: returns once the render thread no longer uses the surface. Bounded:
     * if the render thread misses the deadline the process aborts instead of returning.
     */
    static native String releaseSurface(long host);
    /** Debug: the render thread waits this long before letting go of a released surface. */
    static native void setReleaseStall(long host, int ms);
    /** Debug: the render thread waits this long before its teardown (destroy). */
    static native void setStopStall(long host, int ms);
    static native void input(int buttons, int lx, int ly, int rx, int ry);
    /** Maps each rewritten module in an installed game's modules/ (load check; nothing runs). Any thread. */
    /** Native -> Java: the guest's software keyboard request (blocked guest thread). */
    static String requestText(String title, String initial, int maxChars) {
        return MainActivity.requestText(title, initial, maxChars);
    }

    static native String checkModules(String modulesDir);
    /** Maps exactly these rewritten module images (load check; nothing runs). Any thread. */
    static native String checkModulePaths(String[] modulePaths);
    static native void destroy(long host);
    /** Per-game APK: maps the rewritten modules under assets/dir (read in place) and reports. */
    static native String checkAssetModules(android.content.res.AssetManager assets, String dir);
    /** A0 apk-asset modules: maps exactly these images (paths under assetRoot) from this APK's assets. */
    static native String checkAssetModulePaths(android.content.res.AssetManager assets, String assetRoot, String[] images);
}

