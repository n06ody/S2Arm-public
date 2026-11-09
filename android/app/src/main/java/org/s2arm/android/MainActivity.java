package org.s2arm.android;

import android.app.Activity;
import java.io.File;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Android 外壳：诊断应用与已验证的单游戏 APK 共用输入和 surface 生命周期。 */
public class MainActivity extends Activity
        implements SurfaceHolder.Callback, InputManager.InputDeviceListener, NativeBridge.SessionStopListener {
    private final GamepadState gamepad = new GamepadState();
    private SurfaceView surface;
    private FrameLayout root;
    private TextView status;
    private LinearLayout panel;   // diagnostic text over the surface
    private long host;
    private boolean resumed;
    private final int[] windowInsets = new int[4];   // left, top, right, bottom: system bars and cutout
    private int surfaceWidth, surfaceHeight;
    private String capabilities = "";
    private InputManager inputManager;
    private TouchPadView touchPad;      // on-screen gamepad over the surface
    private String touchMode = "auto";  // auto: shown unless a physical gamepad is connected
    private boolean touchLog;           // debug: log every change of the published input
    private String lastInputLog = "";
    private int loggedLayout;
    private static final int PICK_PACKAGE = 1;
    private GameLibrary library;   // installed game packages (import + verification)
    private HostKeyboard hostKeyboard;
    private LegacyTextInput legacyTextInput;
    private FrameRateOverlay frameRate;
    private TextView games;
    private boolean gameMode;
    private GameSession gameSession;
    private boolean gameStopping;
    private GameAudioFocus gameAudioFocus;
    private boolean previousCacheExported;
    private static final String PREFS = "s2a";
    private static final String PREF_CONSOLE = "consoleMode";
    private static final String PREF_GAMEPAD_MAPPING = "gamepadMapping";
    private static final String GAMEPAD_MAPPING_EXTRA = "s2arm.gamemap";
    /** The resumed shell, for native-side UI requests (software keyboard) that arrive on a guest thread. */
    static volatile MainActivity current;
    private BootSpec.ConsoleMode coldConsoleMode = BootSpec.ConsoleMode.HANDHELD;
    private static final String CONSOLE_MODE_EXTRA = "s2arm.console_mode";
    // 镜像写跟踪生产开关（#1184）：冷启动 --es s2arm.mirror on|off，缺省 off；局内冻结。
    private static final String MIRROR_EXTRA = "s2arm.mirror";
    private BootSpec.Mirror coldMirror = BootSpec.Mirror.OFF;
    // 0 auto / 1 generic / 2 direct；仅冷启动 intent 可改，局内冻结。
    private int graphicsModePreference;
    // Vulkan 款直连的冷启动选择（VulkanDirectChoice：通用 / 直连 / AUTO，#274）；与上面 NVN 的 graphicsModePreference 编号不同
    private int coldVulkanChoice = VulkanDirectChoice.DEFAULT;
    private static final String VULKAN_DIRECT_EXTRA = "s2arm.experimental_vulkan_direct";
    /** Extras a running Activity handles in onNewIntent (requestOrientation / requestDebugLifecycle / updateFrameRate). */
    private static final java.util.Set<String> CONTROL_EXTRAS = new java.util.HashSet<>(java.util.Arrays.asList(
        "s2arm.finish", "s2arm.fps", "s2arm.orientation", "s2arm.panel", "s2arm.touchpad", "s2arm.touchlog", "s2arm.gamemap",
        DebugEnvironment.EXTRA, "s2arm.experimental_touch", "s2arm.release_stall_ms", "s2arm.stop_stall_ms", "s2arm.request_stop",
        GameActivity.ICON_RESUME_EXTRA));
    /** NativeBridge.load succeeded; otherwise the Activity only shows why, and nothing native runs. */
    private boolean runtimeLoaded;
    private final android.os.Handler lifecycleProbe = new android.os.Handler(android.os.Looper.getMainLooper());
    private long lastLifecyclePresent;
    private final Runnable lifecycleSample = this::sampleLifecyclePresent;
    // B3（debug.s2a.buffer_geometry）：交换链缓冲按游戏输出尺寸出图时，native 把
    // 缓冲尺寸报上来；这里按它把 surface/touchPad 的 frame 布成同比例居中矩形，
    // 合成器的 SCALE_TO_WINDOW 拉伸才不变形（native_window 没有 SCALE_FIT）。
    private final Runnable geometrySample = this::sampleBufferGeometry;
    private int bufferGeomW, bufferGeomH;
    private android.window.OnBackInvokedCallback gameBackCallback;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        DebugGate.scrub(getIntent());   // 调试参数两道门（DebugGate）：门没开，后面一律读不到
        gameMode = this instanceof GameActivity;
        if (gameMode) {
            if (GameActivity.libraryRoute(getIntent(), GameSession.running())) {
                startActivity(new android.content.Intent(this, LibraryActivity.class)
                    .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
                finish();
                return;
            }
            String titleId = getIntent().getStringExtra(GameActivity.TITLE_EXTRA);
            if (titleId == null || !titleId.matches("[0-9A-F]{16}")) {
                TextView failure = new TextView(this); failure.setTextColor(0xFFFF5252);
                failure.setText("Game admission failed: selected titleId is required"); setContentView(failure);
                return;
            }
            gameLifecycle("created");
            if (Build.VERSION.SDK_INT >= 24) getWindow().setSustainedPerformanceMode(true);
            if (Build.VERSION.SDK_INT >= 31) {
                android.app.GameManager manager = getSystemService(android.app.GameManager.class);
                if (manager != null) android.util.Log.i("S2Arm", "game mode=" + manager.getGameMode());
            }
            GameSession.clearDirectCache = getIntent().getBooleanExtra("s2arm.clear_direct_cache", false);
            if (Build.VERSION.SDK_INT >= 30 && getIntent().getBooleanExtra(GameActivity.LIFECYCLE_TEST_EXTRA,false)
                    && (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
                try {
                    int pid = android.os.Process.myPid();
                    String stat = new String(java.nio.file.Files.readAllBytes(new File("/proc/self/stat").toPath()),
                        java.nio.charset.StandardCharsets.UTF_8);
                    long elapsed = android.os.Process.getStartElapsedRealtime();
                    ProcessBirth birth = new ProcessBirth(pid,ProcessBirth.startTicks(stat,pid),elapsed,
                        System.currentTimeMillis() - (android.os.SystemClock.elapsedRealtime() - elapsed));
                    getSystemService(android.app.ActivityManager.class).setProcessStateSummary(birth.summary());
                    java.nio.file.Files.write(new File(getCacheDir(),"game-process-" + pid + ".json").toPath(),
                        birth.json().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    gameLifecycle("process-birth " + birth.json());
                } catch (java.io.IOException | RuntimeException error) {
                    gameLifecycle("process-birth-failed " + error);
                }
            }
            if (Build.VERSION.SDK_INT >= 33) {
                gameBackCallback = this::onGameBack;
                getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, gameBackCallback);
            }
        }
        if (!gameMode) {
            String packageTitle;
            try { packageTitle = BootContract.packageTitle(getPackageName(),
                (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0 && DebugGate.open()); }
            catch (IllegalArgumentException closed) {
                Log.e("S2Arm", "Game launch refused: " + closed.getMessage());
                finish();
                return;
            }
            if (getIntent().hasExtra(GameActivity.TITLE_EXTRA) || packageTitle != null) {
                String titleId = getIntent().hasExtra(GameActivity.TITLE_EXTRA)
                    ? getIntent().getStringExtra(GameActivity.TITLE_EXTRA) : packageTitle;
                startActivity(GameActivity.launch(this, getIntent(), titleId));
                finish();
                return;
            }
        }
        String runtimeRejected = loadRuntime(getIntent());
        runtimeLoaded = runtimeRejected == null;
        if (gameMode) dumpPreviousSessionLog();
        root = new FrameLayout(this);
        surface = new SurfaceView(this);
        if (runtimeLoaded) surface.getHolder().addCallback(this);   // rejected: no surface callbacks reach native
        root.addView(surface);
        panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(24 * getResources().getDisplayMetrics().density);
        panel.setPadding(pad, pad, pad, pad);
        panel.setBackgroundColor(0xE6101820);
        TextView title = new TextView(this);
        title.setText(R.string.preview_title); title.setTextSize(24);
        title.setTextColor(Color.WHITE); panel.addView(title);
        TextView description = new TextView(this);
        description.setText(R.string.preview_description);
        description.setTextColor(Color.LTGRAY); panel.addView(description);
        status = new TextView(this); status.setTextColor(0xFF80CBC4);
        status.setTextSize(16); panel.addView(status);
        games = new TextView(this); games.setTextColor(0xFFFFD180);
        games.setTextSize(16); panel.addView(games);
        android.widget.Button importButton = new android.widget.Button(this);
        importButton.setText(R.string.import_game);
        // The player's own package folder (tools/make_pack.py); the app gets read access to that folder only.
        importButton.setOnClickListener(v -> startActivityForResult(
            new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_PACKAGE));
        panel.addView(importButton, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(panel, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT));
        // Views whose layout, touch or timer paths call native code (TouchPadView: onSizeChanged -> relayout ->
        // touchViewportRevoke; FrameRateOverlay: gamePresentCount) exist only when the library is loaded.
        FrameLayout.LayoutParams fpsLayout = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            android.view.Gravity.TOP | android.view.Gravity.CENTER_HORIZONTAL);
        if (runtimeLoaded) {
            touchPad = new TouchPadView(this, changed -> publishInput());
            root.addView(touchPad, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            frameRate = new FrameRateOverlay(this);
            root.addView(frameRate, fpsLayout);
            updateFrameRate(getIntent());
        }
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(
                    android.view.WindowInsets.Type.systemBars() | android.view.WindowInsets.Type.displayCutout());
                windowInsets[0] = bars.left; windowInsets[1] = bars.top; windowInsets[2] = bars.right; windowInsets[3] = bars.bottom;
            } else {
                windowInsets[0] = insets.getSystemWindowInsetLeft(); windowInsets[1] = insets.getSystemWindowInsetTop();
                windowInsets[2] = insets.getSystemWindowInsetRight(); windowInsets[3] = insets.getSystemWindowInsetBottom();
            }
            panel.setPadding(pad + windowInsets[0], pad + windowInsets[1], pad + windowInsets[2], pad);
            if (touchPad != null) touchPad.setInsets(windowInsets[0], windowInsets[1], windowInsets[2], windowInsets[3]);
            fpsLayout.topMargin = windowInsets[1];
            if (frameRate != null) frameRate.setLayoutParams(fpsLayout);
            publishSurface();   // insets are part of the window state the renderer sees
            return insets;
        });
        setContentView(root);
        if (!runtimeLoaded) {
            status.setTextColor(0xFFFF5252);
            status.setText("Startup failed: " + runtimeRejected);
            return;
        }
        if (gameMode) gameAudioFocus = new GameAudioFocus(this, NativeBridge::setAudioOutputAllowed);
        hostKeyboard = new HostKeyboard(this);
        legacyTextInput = new LegacyTextInput(this);
        applyGameWindow();
        library = new GameLibrary(getApplicationContext(), new GameLibrary.Listener() {
            @Override public void onStatus(String text) { runOnUiThread(() -> {
                if (isDestroyed()) return;
                games.setText(text);
                if (text.startsWith("Game admission failed:")) {
                    panel.setVisibility(android.view.View.VISIBLE);
                    games.setTextColor(0xFFFF5252);
                }
            }); }
            @Override public void onGameReady(GameSession ready) {
                runOnUiThread(() -> {
                    if (!gameMode || gameStopping || isFinishing() || isDestroyed()) return;
                    if (gameSession == null) {
                        gameSession = ready;
                        try { GameResume.publish(getCacheDir(), android.os.Process.myPid(), ready.spec); }
                        catch (java.io.IOException error) { status.setText("Game startup failed: " + error.getMessage()); stopGame("session receipt failed", false); return; }
                    }
                    tryStartGame();
                });
            }
        }, gameMode ? getIntent().getStringExtra(GameActivity.TITLE_EXTRA) : null,
            gameMode && getIntent().getBooleanExtra(GameActivity.REVISION_EXTRA, false));
        graphicsModePreference = graphicsModeFromIntent(getIntent());
        requestOrientation(getIntent());
        updateTouchPad();
        inputManager = getSystemService(InputManager.class);
        inputManager.registerInputDeviceListener(this, null);
        try {
            coldConsoleMode = consoleModeFromIntent(getIntent(), true);
            coldMirror = mirrorFromIntent(getIntent(), true, gameMode);
            if (gameMode && !NativeBridge.gameRuntimeAvailable())
                throw new IllegalStateException("This APK does not include the game runtime");
            if (gameMode) {
                coldVulkanChoice = vulkanChoice(getIntent(), VulkanDirectChoice.DEFAULT);
                int directError = NativeBridge.configureVulkanDirect(coldVulkanChoice);
                if (directError != 0) throw new IllegalStateException("Graphics mode configuration failed: " + directError);
                Log.i("S2Arm", "Cold Vulkan direct choice: " + VulkanDirectChoice.name(coldVulkanChoice));
                // 镜像开关必须赶在宿主创建前设好（和图形模式同一条冷启动通道）。
                int mirrorError = NativeBridge.configureMirror(coldMirror == BootSpec.Mirror.ON);
                if (mirrorError != 0) throw new IllegalStateException("Mirror switch configuration failed: " + mirrorError);
                Log.i("S2Arm", "Mirror switch: " + coldMirror.value);
                boolean inlineSme = getIntent() != null
                    && getIntent().getBooleanExtra("s2arm.experimental_inline_sme_context", false);
                int error = NativeBridge.configureInlineSmeContext(inlineSme);
                if (error != 0) throw new IllegalStateException("Signal context experiment configuration failed: " + error);
                Log.i("S2Arm", "Inline SME signal context experiment: " + inlineSme);
                // R's DMA path preserves target reads by default. An explicit legacy
                // request may confirm it, but disabling it is unsupported.
                if (getIntent() != null && getIntent().hasExtra("s2arm.experimental_dma_preserve_read")) {
                    boolean dmaPreserveRead = getIntent().getBooleanExtra("s2arm.experimental_dma_preserve_read", false);
                    error = NativeBridge.configureDmaPreserveRead(dmaPreserveRead);
                    if (error != 0) throw new IllegalStateException("DMA preserve-read request unsupported: " + error);
                }
                Log.i("S2Arm", "DMA target preserve-read: enabled by default (unless borrowed-read diagnostic is set)");
                boolean boundedRead = getIntent() == null
                    || getIntent().getBooleanExtra("s2arm.bounded_readonly_check", true);
                error = NativeBridge.configureReadonlyRangeCheck(boundedRead);
                if (error != 0) throw new IllegalStateException("Readonly range check configuration failed: " + error);
                Log.i("S2Arm", "Bounded readonly range check: " + boundedRead);
                boolean bufferReadScratch = getIntent() != null
                    && getIntent().getBooleanExtra("s2arm.experimental_buffer_read_scratch", false);
                error = NativeBridge.configureBufferReadScratch(bufferReadScratch);
                if (error != 0) throw new IllegalStateException("Buffer read scratch configuration failed: " + error);
                Log.i("S2Arm", "Buffer read scratch experiment: " + bufferReadScratch);
                boolean ssboOobDiag = getIntent() != null
                    && getIntent().getBooleanExtra("s2arm.diag_ssbo_oob", false);
                error = NativeBridge.configureSsboOobDiag(ssboOobDiag);
                if (error != 0) throw new IllegalStateException("SSBO OOB diagnostic configuration failed: " + error);
                Log.i("S2Arm", "SSBO out-of-window record diagnostic: " + ssboOobDiag);
                boolean profileFreeze = getIntent() != null
                    && getIntent().getBooleanExtra("s2arm.diag_profile_freeze", false);
                error = NativeBridge.configureProfileFreeze(profileFreeze);
                if (error != 0) throw new IllegalStateException("Profile freeze configuration failed: " + error);
                Log.i("S2Arm", "Vulkan translation profile freeze: " + profileFreeze);
                // 拆线程缺省开；true|false（--ez 或 --es）、1|0（--es）显式开 / 关，auto（--es）按上局记录。
                // 每次都显式设 0|1|auto，不留给 native 缺省；别的值报错，不按关处理。
                boolean threadedExtra = getIntent() != null && getIntent().hasExtra(ThreadedRendererConfig.EXTRA);
                String threadedMode = ThreadedRendererConfig.resolve(threadedExtra,
                    threadedExtra ? getIntent().getExtras().get(ThreadedRendererConfig.EXTRA) : null);
                error = NativeBridge.configureThreadedRenderer(threadedMode);
                if (error != 0) throw new IllegalStateException("Threaded renderer configuration failed: " + error);
                Log.i("S2Arm", "Threaded renderer (GPU emulation / Vulkan split): " + threadedMode
                    + (threadedExtra ? " (extra)" : " (default)"));
            }
            // Last configuration before host creation; the shared services read this
            // same process environment on their normal startup path.
            if (BuildConfig.DEBUG && getIntent() != null && getIntent().hasExtra(DebugEnvironment.EXTRA)) {
                byte[][] entries = DebugEnvironment.parse(getIntent().getExtras().get(DebugEnvironment.EXTRA));
                int error = NativeBridge.configureDebugEnvironment(entries);
                if (error != 0) throw new IllegalStateException("Debug environment configuration failed: " + error);
            }
            host = gameMode ? NativeBridge.createGame()
                : NativeBridge.create(new java.io.File(getFilesDir(), "selftest").getAbsolutePath());
            if (host == 0) throw new IllegalStateException("native host allocation failed");
            capabilities = NativeBridge.loaded().line() + "\n" + NativeBridge.describe(host);
            if (!gameMode) capabilities += "\n" + registerHost();
            NativeBridge.setStopListener(this);
            String game = gameMode ? null : apkGame();
            if (game != null) capabilities += "\n" + game;
            status.setText(capabilities);
            requestDebugLifecycle(getIntent());
        } catch (LinkageError | RuntimeException e) {
            Log.e("S2Arm", "Native host startup failed", e);
            status.setText("Startup failed: " + e.getMessage());
        }
    }

    /**
     * Loads libs2arm_android.so before anything native runs (NativeBridge.load is the only entry).
     * A launch that names a development runtime gets exactly that one or none: null when loaded,
     * otherwise the reason, and the APK's library is not loaded in its place.
     */
    private String loadRuntime(android.content.Intent intent) {
        android.content.pm.ApplicationInfo app = getApplicationInfo();
        String requested = requestedRuntime(intent);
        try {
            long start = System.nanoTime();
            RuntimeLibrary library = RuntimeLibrary.select(requested,
                (app.flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0, getFilesDir(), getCodeCacheDir(),
                android.os.Process.myPid(), NativeBridge.loaded(), () -> GameSession.apkSha256(new File(app.sourceDir)));
            if (!library.override()) {
                if (RuntimeLibrary.present(getFilesDir()) > 0)
                    Log.i("S2Arm", "Development runtime libraries in files/" + RuntimeLibrary.DIRECTORY
                        + " are not used: this launch has no " + RuntimeLibrary.EXTRA + " extra");
            } else if (library != NativeBridge.loaded()) {   // main thread: base APK hash + copy, cost on record
                Log.i("S2Arm", "Development runtime checks took " + (System.nanoTime() - start) / 1_000_000 + " ms");
            }
            NativeBridge.load(library, app.sourceDir, app.splitSourceDirs, app.nativeLibraryDir);
            configureRedLogDir();
            configureDiagDir();
            try {
                int frameLogRc = NativeBridge.configureFrameLogDir(getFilesDir().getAbsolutePath());
                if (frameLogRc != 0) Log.w("S2Arm", "frame-log 目录不可用（errno=" + frameLogRc + "）");
            } catch (UnsatisfiedLinkError oldRuntime) {
                Log.w("S2Arm", "运行库不支持 frame-log 文件；仅 logcat 可用");
            }
            LanDiscovery.bind(getApplicationContext());
            return null;
        } catch (RuntimeLibrary.Rejected | LinkageError error) {
            Log.e("S2Arm", "Runtime library load failed: " + error.getMessage(), error);
            return error.getMessage();
        }
    }

    /** RuntimeLibrary.requested for an intent: --es only; another type of the extra is refused, not ignored. */
    private static String requestedRuntime(android.content.Intent intent) {
        boolean has = intent != null && intent.hasExtra(RuntimeLibrary.EXTRA);
        return RuntimeLibrary.requested(has, has ? intent.getStringExtra(RuntimeLibrary.EXTRA) : null);
    }

    /**
     * 红日志目录按当前应用派生（#1357）：默认外部私有目录 getExternalFilesDir(null)，取不到或
     * 预打开失败再退 getFilesDir()。native 只拼 red.log，不写死任何包名。失败只降级到宿主
     * 日志 fd，不阻断启动。
     */
    private void configureRedLogDir() {
        java.io.File external = getExternalFilesDir(null);
        int rc = external == null ? -1 : NativeBridge.configureRedLogDir(external.getAbsolutePath());
        if (rc != 0) rc = NativeBridge.configureRedLogDir(getFilesDir().getAbsolutePath());
        if (rc != 0) Log.w("S2Arm", "red.log 落盘目录不可用（errno=" + rc + "）；致命原因仍写宿主日志");
    }

    /**
     * GS+遮挡查询运行时探测（#1456）：把结果目录、known-bad 名单文本、版本号
     * 与探测助手路径交给 native；探测本体在原生图形选择前由 native 逐臂
     * posix_spawn 该助手执行，结果文件供能力层读。名单缺失时探测仍跑（9000
     * 这类设备会由 occ 臂实测出 unsupported），但缺名单会让它真撞一次
     * DEVICE_LOST，所以要记警告。
     */
    private void configureCapProbe() {
        String badlist = "";
        try (java.io.InputStream in = getAssets().open("capprobe/known-bad.txt")) {
            byte[] buf = new byte[8192];
            int n = in.read(buf);
            badlist = n > 0 ? new String(buf, 0, n, java.nio.charset.StandardCharsets.UTF_8) : "";
        } catch (java.io.IOException e) {
            Log.w("S2Arm", "capprobe: known-bad.txt asset unavailable: " + e);
        }
        String version = "";
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            Log.w("S2Arm", "capprobe: app version unavailable: " + e);
        }
        NativeBridge.configureCapProbe(
            new java.io.File(getFilesDir(), "capprobe").getAbsolutePath(), badlist, version,
            capProbeHelperPath());
    }

    /**
     * 探测助手按 lib/<abi>/libs2a_capprobe.so 打进 APK（jniLibs 源目录——AGP 只打包
     * shared library，可执行件借 lib*.so 名字；STORED+16 KB 页对齐留在包内）。
     * 返回给 native 的不是落盘路径而是「APK 内 spec」：
     * {@code <sourceDir>!/lib/arm64-v8a/libs2a_capprobe.so}——native 以系统
     * linker64 为 exec 目标 posix_spawn 它（bionic Android 10 起认 apk!/ 路径，
     * 库按普通 .so 从包内映射）。exec 落在系统文件上，不受应用私有目录
     * execute_no_trans 禁令限制，也不要求全包 extractNativeLibs（#1482：
     * files/ 解包 exec 真机 exit:127；nativeLibraryDir 落盘方案改全局打包，
     * owner 否决）。包文件不存在时返回 ""，native 按 helper-unset 报
     * error 行——指纹不可得、探测跳过，方向安全。
     */
    private String capProbeHelperPath() {
        java.io.File apk = new java.io.File(getApplicationInfo().sourceDir);
        if (!apk.isFile()) {
            Log.w("S2Arm", "capprobe: apk missing at " + apk.getAbsolutePath());
            return "";
        }
        return apk.getAbsolutePath() + "!/lib/arm64-v8a/libs2a_capprobe.so";
    }

    /**
     * 直连编译诊断目录（F4.1）：外部私有目录 getExternalFilesDir(null) 下的
     * direct-diagnostics，adb pull 免 run-as——编译请求/结果封套的现场实据直接可取。
     * 取不到或注入失败只告警，诊断仍走会话内部目录。
     */
    private void configureDiagDir() {
        java.io.File external = getExternalFilesDir(null);
        int rc = external == null ? -1 : NativeBridge.configureDiagDir(external.getAbsolutePath());
        if (rc != 0) Log.w("S2Arm", "直连诊断外部目录不可用（errno=" + rc + "）；诊断仍写会话内部目录");
    }

    // HostApi (A2): the core reads language / region / time zone from the session's BootSpec values,
    // never from the system later; the display table follows the window lease. Diagnostics retain
    // handheld. Games freeze the optional cold-start mode with their BootSpec below.
    private String registerHost() {
        BootSpec spec = new BootSpec();
        BootSpecSource.user(spec);
        android.util.DisplayMetrics metrics = getResources().getDisplayMetrics();
        return NativeBridge.registerHost(spec.user.language, spec.user.region == null ? "" : spec.user.region, spec.user.timezone,
            Math.round(metrics.xdpi), Math.round(metrics.ydpi), spec.consoleMode.nativeValue);
    }

    private static int debugSystemInt(String key) {
        try {
            Class<?> properties = Class.forName("android.os.SystemProperties");
            Object value = properties.getMethod("get", String.class).invoke(null, key);
            return value instanceof String ? Integer.parseInt((String) value) : 0;
        } catch (ReflectiveOperationException | NumberFormatException ignored) {
            return 0;
        }
    }

    /** 冷启动 intent：s2arm.draw_log=1|0 → 原生 draw_log 覆盖（生产通道；debug.s2a.draw_log
        属性在 user 版设备上 untrusted_app 读不到，extra 经本 Activity 落到 :game 进程）。 */
    private static int drawLogFromIntent(android.content.Intent intent) {
        String value = intent == null ? null : intent.getStringExtra("s2arm.draw_log");
        if (value == null || value.isEmpty()) return -1;
        if ("1".equals(value)) return 1;
        if ("0".equals(value)) return 0;
        Log.e("S2Arm", "Unknown s2arm.draw_log '" + value + "'; ignoring");
        return -1;
    }

    private void tryStartGame() {
        if (!gameMode || gameStopping || host == 0 || gameSession == null) return;
        NativeBridge.setDrawLog(drawLogFromIntent(getIntent()));
        exportPreviousShaderCache();
        android.view.Display display = surface.getDisplay();
        android.util.DisplayMetrics metrics = getResources().getDisplayMetrics();
        float refresh = display == null ? 0 : display.getRefreshRate();
        // Test lever: let the guest window/framebuffer use a lower render size while the Surface stays full.
        // The backend still presents to the real Surface; this only changes what the guest is told.
        int renderW = debugSystemInt("debug.s2a.render_width");
        int renderH = debugSystemInt("debug.s2a.render_height");
        int beginW = renderW > 0 && renderH > 0 ? renderW : surfaceWidth;
        int beginH = renderW > 0 && renderH > 0 ? renderH : surfaceHeight;
        try {
            GameSession.FrozenStart frozen = gameSession.begin(resumed, surface.getHolder().getSurface().isValid(),
                beginW, beginH, metrics.densityDpi, refresh, coldConsoleMode,
                graphicsModePreference);
            if (frozen == null) return;
            String registered = NativeBridge.registerHost(frozen.language, frozen.region, frozen.timezone,
                Math.round(metrics.xdpi), Math.round(metrics.ydpi), frozen.nativeConsoleMode());
            if (!registered.startsWith("HostApi v1")) throw new IllegalStateException(registered);
            try (java.io.OutputStream out = new java.io.FileOutputStream(
                    new java.io.File(getCacheDir(), "boot-spec-" + frozen.titleId + ".json"))) {
                out.write(frozen.bootJson.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            // Next to this session's host log and metrics (state dir): which library runs, bound by PID, for
            // metrics.py and receipts (a dev-override session is no reading).
            java.io.File marker = new java.io.File(frozen.stateDir, "runtime-library.json");
            java.io.File markerTmp = new java.io.File(frozen.stateDir, "runtime-library.json.tmp");
            try (java.io.OutputStream out = new java.io.FileOutputStream(markerTmp)) {
                out.write(("{\"pid\": " + android.os.Process.myPid() + ", \"sessionId\": \"" + frozen.sessionId
                    + "\", \"runtimeLibrary\": " + NativeBridge.loaded().json() + "}\n")
                    .getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            if (!markerTmp.renameTo(marker)) throw new java.io.IOException("cannot write " + marker);
            for (java.util.Map.Entry<String, String> module : frozen.moduleSha256.entrySet())
                Log.i("S2Arm", "Admitted module " + module.getKey() + " sha256=" + module.getValue()
                    + " source=" + (frozen.assetRoot.isEmpty() ? "revision" : "apk-asset"));
            int count = frozen.modules.size();
            String[] names = new String[count], images = new String[count], fixups = new String[count];
            String[] gates = new String[count], widen = new String[count];
            for (int i = 0; i < count; i++) {
                BootContract.Module module = frozen.modules.get(i);
                names[i] = module.name; images[i] = module.image; fixups[i] = module.fixups;
                gates[i] = module.gates; widen[i] = module.widen;
            }
            configureCapProbe();
            int rc = NativeBridge.startGame(host, getAssets(), frozen.assetRoot,
                names, images, fixups, gates, widen, frozen.npdmPath,
                frozen.revisionDir, frozen.stateDir, frozen.programId,
                frozen.bootJson, frozen.nativeConsoleMode(), frozen.modePreference,
                frozen.graphicsDescriptor);
            if (rc != 0) throw new IllegalStateException("game launch rejected: " + rc);
            Log.i("S2Arm", "Game startup accepted: " + frozen.titleId + " session " + frozen.sessionId
                + " consoleMode " + frozen.consoleMode.value);
            if (gameSession.bootTiming != null && gameSession.bootTiming.readyNanos != 0)
                Log.d("S2ArmBootTiming", "phase=ready_to_accepted ns="
                    + (System.nanoTime() - gameSession.bootTiming.readyNanos));
            gameLifecycle("started");
            panel.setVisibility(android.view.View.GONE);
        } catch (java.io.IOException | RuntimeException | LinkageError error) {
            Log.e("S2Arm", "Game startup failed", error);
            panel.setVisibility(android.view.View.VISIBLE);
            status.setText("Game startup failed: " + error.getMessage());
            stopGame("game startup failed", false);
        }
    }

    // normal 只表示“这是一次正常收局”（用户/游戏主动停止），异常清理（启动失败、
    // Activity 被销毁但没有正常结束会话）不能清 attempt 锁，否则下一局 AUTO 会误回 direct。
    private void stopGame(String why, boolean normal) {
        if (!gameMode || gameStopping) return;
        gameStopping = true;
        if (gameAudioFocus != null) gameAudioFocus.pause();
        try { GameResume.clear(getCacheDir()); }
        catch (java.io.IOException error) { Log.e("S2Arm", "Cannot retire active session receipt", error); }
        gameLifecycle("stop-requested");
        if (surface != null) surface.setKeepScreenOn(false);
        maybeExportShaderCache("session");
        if (gameSession != null) gameSession.stop();
        // 先让原生收尾在 Surface 仍然有效时开始：Activity 一 finish，BufferQueue 就撤销，
        // GPU/present fence 的排空会和 surfaceDestroyed 抢同一份资源（真机上表现为收尾超时或崩溃）。
        // 进程最终由原生 _exit 结束；这里的延迟 finish 只是「原生没能结束进程」时的兜底。
        if (host != 0) NativeBridge.stopGame(host, why, normal);
        else if (this instanceof GameActivity) android.os.Process.killProcess(android.os.Process.myPid());
        // 原生收尾有自己的硬预算并会 _exit；这里只兜底「原生没能结束进程」的情况。
        // 提前 finish 会撤销 BufferQueue，surfaceDestroyed 的 1 秒期限随即打断正在排空的 GPU 收尾。
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(this::finishAndRemoveTask, 10000);
    }

    // Per-game APK (tools/game_apk.py): the game this app was built for, and a load check of the
    // rewritten modules it carries. null for the diagnostic app, which carries none.
    private String apkGame() {
        java.util.Properties game = new java.util.Properties();
        try (java.io.InputStream in = getAssets().open("game/game.properties")) {
            game.load(in);
        } catch (java.io.IOException e) {
            return null;
        }
        Log.i("S2Arm", "Game in this APK: " + game.getProperty("titleId") + " (" + getPackageName() + ")");
        return "Game in this APK: " + game.getProperty("name") + " " + game.getProperty("version") + " ["
            + game.getProperty("titleId") + "]; " + NativeBridge.checkAssetModules(getAssets(), "game/modules");
    }

    @Override protected void onResume() {
        super.onResume(); resumed = true;
        // A rejected cold-start configuration never owns a native game host.
        // Do not acquire the game storage lease for a session that cannot start.
        if (!runtimeLoaded || (gameMode && host == 0)) return;
        if (gameMode && !gameStopping && gameAudioFocus != null) gameAudioFocus.resume();
        hostKeyboard.resume();
        legacyTextInput.resume();
        frameRate.setResumed(true);
        current = this;
        applyGameWindow();
        library.scanImports();   // report the library; imports require an explicit staging target
        if (host != 0) NativeBridge.setForeground(host, true);   // the window, if any, is still the render thread's
        tryStartGame();
        if (gameMode) lifecycleProbe.postDelayed(geometrySample, 500);   // B3 输出尺寸轮询（开关关时全部回落 MATCH_PARENT）
        if (gameMode && host != 0 && !gameStopping && getIntent().getBooleanExtra(GameActivity.LIFECYCLE_TEST_EXTRA, false)
                && (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            lastLifecyclePresent = NativeBridge.gamePresentCount();
            lifecycleProbe.postDelayed(lifecycleSample, 100);
        }
    }
    // Test opt-in only: a real newly queued present is stronger than startGame's worker-accepted receipt.
    private void sampleLifecyclePresent() {
        if (!resumed || !gameMode || !runtimeLoaded || host == 0 || gameStopping) return;
        long count = NativeBridge.gamePresentCount();
        if (count > lastLifecyclePresent) {
            lastLifecyclePresent = count;
            gameLifecycle("presented " + count);
        }
        lifecycleProbe.postDelayed(lifecycleSample, 100);
    }
    // B3：500 ms 轮询交换链输出尺寸；未开/未出图时持续按 MATCH_PARENT 收尾，旋转后重排也走这里。
    private void sampleBufferGeometry() {
        if (!resumed || !gameMode || !runtimeLoaded) return;
        int[] dims = new int[2];
        NativeBridge.bufferGeometry(dims);
        if (dims[0] != bufferGeomW || dims[1] != bufferGeomH) {
            bufferGeomW = dims[0]; bufferGeomH = dims[1];
        }
        layoutForBufferGeometry();
        lifecycleProbe.postDelayed(geometrySample, 500);
    }
    private void layoutForBufferGeometry() {
        int w = FrameLayout.LayoutParams.MATCH_PARENT, h = FrameLayout.LayoutParams.MATCH_PARENT;
        int gravity = android.view.Gravity.CENTER;
        if (bufferGeomW > 0 && bufferGeomH > 0 && root != null && root.getWidth() > 0 && root.getHeight() > 0) {
            int[] fit = BufferGeometry.fit(bufferGeomW, bufferGeomH, root.getWidth(), root.getHeight());
            w = fit[0]; h = fit[1];
        }
        surface.setLayoutParams(new FrameLayout.LayoutParams(w, h, gravity));
        if (touchPad != null) touchPad.setLayoutParams(new FrameLayout.LayoutParams(w, h, gravity));
    }
    @Override protected void onPause() {
        resumed = false;
        if (gameAudioFocus != null) gameAudioFocus.pause();
        if (legacyTextInput != null) legacyTextInput.pause();
        lifecycleProbe.removeCallbacks(lifecycleSample);
        lifecycleProbe.removeCallbacks(geometrySample);
        if (runtimeLoaded) {
            clearInput();
            hostKeyboard.pause();
            frameRate.setResumed(false);
            // Rendering stops; the surface stays with the render thread until surfaceDestroyed.
            if (host != 0) NativeBridge.setForeground(host, false);
        }
        super.onPause();
    }
    @Override protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        if (runtimeLoaded && request == PICK_PACKAGE && result == RESULT_OK && data != null && data.getData() != null)
            library.importTree(data.getData());
    }
    @Override protected void onNewIntent(android.content.Intent intent) {
        DebugGate.scrub(intent);
        super.onNewIntent(intent);
        if (gameMode && intent != null && intent.hasExtra(GameResume.EXTRA)
                && (gameStopping || gameSession == null || !gameSession.spec.sessionId.equals(intent.getStringExtra(GameResume.EXTRA)))) {
            android.widget.Toast.makeText(this, "Game resume refused: session is retiring or changed", android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        if (!runtimeLoaded) {   // nothing native runs; only the finish request is honoured
            if (intent != null && intent.getBooleanExtra("s2arm.finish", false)) finishAndRemoveTask();
            return;
        }
        // The library is bound to the process: a launch (singleTop) asking for another one, or for none while a
        // development library runs, is refused and ends this session instead of silently keeping the old one.
        // Control intents (only CONTROL_EXTRAS) steer the running session whatever library it runs.
        android.os.Bundle extras = intent == null ? null : intent.getExtras();
        boolean launch = RuntimeLibrary.isLaunch(extras == null ? java.util.Collections.emptySet() : extras.keySet(),
            CONTROL_EXTRAS);
        String requested = requestedRuntime(intent);
        if (launch && !NativeBridge.loaded().selectedBy(requested)) {
            String why = "Runtime library mismatch: this process runs \"" + NativeBridge.loaded().line()
                + "\", the launch asks for " + (requested == null ? "the APK's library" : RuntimeLibrary.EXTRA + "=" + requested)
                + "; cold start needed";
            Log.e("S2Arm", why);
            panel.setVisibility(android.view.View.VISIBLE);
            status.setTextColor(0xFFFF5252);
            status.setText(why);
            if (gameMode) stopGame("runtime library mismatch", true);
            else finish();
            return;
        }
        if (gameMode && intent != null && intent.hasExtra(GameActivity.TITLE_EXTRA)) {
            String incoming = intent.getStringExtra(GameActivity.TITLE_EXTRA);
            if (!java.util.Objects.equals(incoming, getIntent().getStringExtra(GameActivity.TITLE_EXTRA))) {
                status.setTextColor(0xFFFF5252);
                status.setText("Game launch refused: previous session still owns this process; exit it first");
                panel.setVisibility(android.view.View.VISIBLE);
                return;
            }
        }
        if (gameMode && intent != null && intent.hasExtra(GameActivity.ICON_RESUME_EXTRA))
            Log.i("S2Arm", "icon resume: existing session kept (control intent)");
        try {
            if (BuildConfig.DEBUG && intent != null && intent.hasExtra(DebugEnvironment.EXTRA))
                throw new IllegalStateException("Debug environment requires a fresh process");
            consoleModeFromIntent(intent, false);
            if (intent != null && intent.hasExtra("s2arm.experimental_dma_preserve_read"))
                throw new IllegalStateException("DMA preserve-read request requires a cold start");
            if (intent != null && intent.hasExtra(VULKAN_DIRECT_EXTRA)
                    && vulkanChoice(intent, coldVulkanChoice) != coldVulkanChoice)
                throw new IllegalStateException("Graphics mode is frozen; use a cold start");
            if (intent != null && intent.hasExtra(MIRROR_EXTRA))
                throw new IllegalStateException("Mirror switch is frozen; use a cold start");
        } catch (RuntimeException error) {
            Log.e("S2Arm", "Console mode change rejected", error);
            status.setText(error.getMessage());
            android.widget.Toast.makeText(this, error.getMessage(), android.widget.Toast.LENGTH_LONG).show();
        }
        requestOrientation(intent); requestDebugLifecycle(intent);
        updateFrameRate(intent);
    }

    private void updateFrameRate(android.content.Intent intent) {
        android.content.SharedPreferences prefs = getSharedPreferences("display", MODE_PRIVATE);
        if (intent != null && intent.hasExtra("s2arm.fps")) {
            String value = intent.getStringExtra("s2arm.fps");
            if ("on".equals(value) || "off".equals(value))
                prefs.edit().putBoolean("fps", "on".equals(value)).apply();
        }
        frameRate.setEnabledForGame(gameMode && prefs.getBoolean("fps", false));
    }

    /** 冷启动 intent：s2arm.graphics_mode=auto|generic|direct；未知值记错误并按 auto。 */
    private static int graphicsModeFromIntent(android.content.Intent intent) {
        String value = intent == null ? null : intent.getStringExtra("s2arm.graphics_mode");
        if (value == null || value.isEmpty() || "auto".equals(value)) return 0;
        if ("generic".equals(value)) return 1;
        if ("direct".equals(value)) return 2;
        Log.e("S2Arm", "Unknown s2arm.graphics_mode '" + value + "'; using auto");
        return 0;
    }

    @SuppressWarnings("deprecation")
    private static int vulkanChoice(android.content.Intent intent, int fallback) {
        if (intent == null || !intent.hasExtra(VULKAN_DIRECT_EXTRA)) return fallback;
        return VulkanDirectChoice.parse(intent.getExtras().get(VULKAN_DIRECT_EXTRA));
    }

    /** 冷启动 intent：s2arm.mirror=on|off（缺省 off）。类型或取值不对直接拒绝，不按关兜底。 */
    @SuppressWarnings("deprecation")
    private static BootSpec.Mirror mirrorFromIntent(android.content.Intent intent, boolean coldStart, boolean game) {
        boolean present = intent != null && intent.hasExtra(MIRROR_EXTRA);
        Object value = present ? intent.getExtras().get(MIRROR_EXTRA) : null;
        return BootSpec.Mirror.forLaunch(coldStart, game, present, value);
    }

    // Test entry: `am start ... --es s2arm.console_mode handheld|docked` (legacy alias
    // `s2arm.console`). The choice is persisted per install and reused by later cold starts;
    // while a session runs it is also pushed to the host (interface v2.15).
    @SuppressWarnings("deprecation")
    private BootSpec.ConsoleMode consoleModeFromIntent(android.content.Intent intent, boolean coldStart) {
        boolean modern = intent != null && intent.hasExtra(CONSOLE_MODE_EXTRA);
        boolean legacy = intent != null && intent.hasExtra("s2arm.console");
        boolean present = modern || legacy;
        Object value = modern ? intent.getExtras().get(CONSOLE_MODE_EXTRA)
            : (legacy ? intent.getStringExtra("s2arm.console") : null);
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (gameMode && !present) {
            String stored = prefs.getString(PREF_CONSOLE, null);
            if (stored != null) {
                for (BootSpec.ConsoleMode mode : BootSpec.ConsoleMode.values())
                    if (mode.value.equals(stored)) return mode;
            }
        }
        if (present && !coldStart) {
            // 接口 v2.15：会话在跑就把切换推给宿主（setConsoleMode → display->set_console_mode →
            //   s2a_console_mode_set → ChangeDockedModeState）；没在跑就只持久化、冷启生效——不静默。
            //   coldConsoleMode 不跟改：begin 仍拿冻结的冷启值，重进不踩「mode frozen」。
            BootSpec.ConsoleMode mode = BootSpec.ConsoleMode.forRuntime(gameMode, value);
            prefs.edit().putString(PREF_CONSOLE, mode.value).apply();
            if (host != 0 && gameSession != null && gameSession.attempted() && !gameStopping) {
                int rc = NativeBridge.setConsoleMode(host, mode.nativeValue);
                Log.i("S2Arm", rc == 0 ? "console mode switched at runtime: " + mode.value
                    : "console mode " + mode.value + " saved; runtime switch refused (rc=" + rc
                        + "), takes effect on cold start");
            } else {
                Log.i("S2Arm", "console mode " + mode.value
                    + " saved; takes effect on cold start (no running session)");
            }
            return mode;
        }
        BootSpec.ConsoleMode mode = BootSpec.ConsoleMode.forLaunch(coldStart, gameMode, present, value);
        if (gameMode && present) prefs.edit().putString(PREF_CONSOLE, mode.value).apply();
        return mode;
    }
    // Debug: `--ei s2arm.release_stall_ms <ms>` makes the render thread hold a released surface that long,
    // standing in for a stuck driver call: under 1000 ms surfaceDestroyed waits it out, over that the
    // process aborts with a tombstone (the timeout path). 0 turns it off.
    // `--ei s2arm.stop_stall_ms <ms>` does the same before the render thread's teardown (onDestroy's
    // deadline), and `--ez s2arm.finish true` finishes the activity (BACK only backgrounds it on
    // Android 12+), so the teardown path can be run on a device. `--ei s2arm.request_stop <reason>`
    // (S2A_HOST_STOP_*) asks for a session stop through the HostApi stop table from a background
    // thread, as the core does; the first request of the process is the one that counts.
    private void requestDebugLifecycle(android.content.Intent intent) {
        if (host == 0 || intent == null) return;
        if (intent.hasExtra("s2arm.experimental_touch")) {
            boolean enabled = intent.getBooleanExtra("s2arm.experimental_touch", false);
            try {
                touchPad.setGameTouchEnabled(enabled);
                updateTouchPad();
                String state = enabled
                    ? "Experimental touch armed; waiting for final game viewport"
                    : "Experimental touch disabled";
                Log.i("S2Arm", state);
                status.setText(state);
            } catch (IllegalStateException e) {
                Log.e("S2Arm", e.getMessage());
                status.setText(e.getMessage());
            }
        }
        if (intent.hasExtra("s2arm.release_stall_ms"))
            NativeBridge.setReleaseStall(host, intent.getIntExtra("s2arm.release_stall_ms", 0));
        if (intent.hasExtra("s2arm.stop_stall_ms"))
            NativeBridge.setStopStall(host, intent.getIntExtra("s2arm.stop_stall_ms", 0));
        if (intent.getBooleanExtra("s2arm.finish", false)) {
            // 终止命令只消费一次，不让重建的 Activity 或后续控制重复处理它。
            intent.removeExtra("s2arm.finish");
            if (getIntent() != null) getIntent().removeExtra("s2arm.finish");
            if (gameMode) stopGame("finish requested", true);
            else finish();
            return;
        }
        if (intent.hasExtra("s2arm.request_stop")) {
            int reason = intent.getIntExtra("s2arm.request_stop", 3);
            new Thread(() -> NativeBridge.debugRequestStop(reason, "debug intent"), "s2arm-debug-stop").start();
        }
    }

    // HostApi stop: the runtime ended the session. Tell the player why and leave; the teardown
    // (render thread, surface) is onDestroy's, as for any finish.
    @Override public void onSessionStop(int reason, String detail) {
        String why = NativeBridge.stopReason(reason) + (detail.isEmpty() ? "" : ": " + detail);
        status.setText("Session ended: " + why);
        android.widget.Toast.makeText(this, "S2Arm session ended: " + why, android.widget.Toast.LENGTH_LONG).show();
        if (gameMode) stopGame(why, true);
        else finish();
    }
    // Debug: `am start ... --es s2arm.orientation landscape|reverse-landscape|portrait` rotates this activity only;
    // `--es s2arm.panel off|on` hides the diagnostic panel so screenshots show the surface;
    // `--es s2arm.touchpad on|off|auto` forces the on-screen gamepad, `--es s2arm.touchlog on` logs input changes.
    // `--es s2arm.gamemap position|label` selects and saves physical face-button mapping.
    // Debug-only orientation lock (intent extra); normal runs never lock the orientation.
    @android.annotation.SuppressLint("SourceLockedOrientationActivity")
    private void requestOrientation(android.content.Intent intent) {
        String panelMode = intent == null ? null : intent.getStringExtra("s2arm.panel");
        if (panel != null && panelMode != null) panel.setVisibility("off".equals(panelMode) ? android.view.View.GONE : android.view.View.VISIBLE);
        String touch = intent == null ? null : intent.getStringExtra("s2arm.touchpad");
        if (touch != null) { touchMode = touch; updateTouchPad(); }
        if (intent != null && intent.hasExtra("s2arm.touchlog")) {
            touchLog = "on".equals(intent.getStringExtra("s2arm.touchlog"));
            touchPad.setTrace(touchLog);
        }
        updateGamepadMapping(intent);
        String o = intent == null ? null : intent.getStringExtra("s2arm.orientation");
        if ("landscape".equals(o)) setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        else if ("portrait".equals(o)) setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
        else if ("reverse-landscape".equals(o)) setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE);
    }
    @SuppressWarnings("deprecation")
    private void updateGamepadMapping(android.content.Intent intent) {
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String mode = prefs.getString(PREF_GAMEPAD_MAPPING, "position");
        if (intent != null && intent.hasExtra(GAMEPAD_MAPPING_EXTRA)) {
            Object value = intent.getExtras().get(GAMEPAD_MAPPING_EXTRA);
            if (!"position".equals(value) && !"label".equals(value)) {
                String why = "s2arm.gamemap must be position or label";
                Log.e("S2Arm", why);
                status.setText(why);
                android.widget.Toast.makeText(this, why, android.widget.Toast.LENGTH_LONG).show();
            } else {
                mode = (String) value;
                prefs.edit().putString(PREF_GAMEPAD_MAPPING, mode).apply();
                // Recreation must not reapply an older choice from the original launch intent.
                android.content.Intent launchIntent = getIntent();
                if (launchIntent != null) launchIntent.putExtra(GAMEPAD_MAPPING_EXTRA, mode);
            }
        }
        gamepad.setMapping(mode);
        publishInput();
    }
    @Override protected void onDestroy() {
        lifecycleProbe.removeCallbacks(lifecycleSample);
        if (frameRate != null) frameRate.setResumed(false);
        if (hostKeyboard != null) hostKeyboard.pause();
        if (legacyTextInput != null) legacyTextInput.pause();
        if (host != 0) touchPad.setGameTouchEnabled(false);
        NativeBridge.clearStopListener(this);
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        if (gameMode) stopGame("activity destroyed", false);
        else if (host != 0) NativeBridge.destroy(host);
        host = 0;
        if (library != null) library.close();
        if (Build.VERSION.SDK_INT >= 33 && gameBackCallback != null)
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(gameBackCallback);
        if (current == this) current = null;
        super.onDestroy();
    }
    private void onGameBack() { stopGame("back requested", true); }

    // API 29-32 use onBackPressed; API 33+ is registered with the predictive-back dispatcher above.
    @android.annotation.SuppressLint("GestureBackNavigation")
    @Override public void onBackPressed() {
        if (gameMode) stopGame("back requested", true);
        else super.onBackPressed();
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (!focused) clearInput();
        else applyGameWindow();
    }

    // Only the foreground game owns immersive presentation and the screen-awake
    // request. Preserve the renderer's aspect ratio; bars can be revealed by a
    // system gesture and controls still use the actual cutout/system insets.
    @SuppressWarnings("deprecation")
    private void applyGameWindow() {
        if (!gameMode || gameStopping || surface == null) return;
        surface.setKeepScreenOn(true);
        android.view.Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(
                    android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(android.view.WindowInsets.Type.systemBars());
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }
    @Override public void onInputDeviceAdded(int id) { updateTouchPad(); }
    @Override public void onInputDeviceChanged(int id) { clearInput(); updateTouchPad(); }
    @Override public void onInputDeviceRemoved(int id) { clearInput(); updateTouchPad(); }
    private static boolean gamepadConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && !device.isVirtual() && controller(device.getSources())) return true;
        }
        return false;
    }
    private void updateTouchPad() {
        if (touchPad == null) return;
        boolean show = "on".equals(touchMode) || ("auto".equals(touchMode) && !gamepadConnected());
        touchPad.setControlsVisible(show);
        touchPad.setVisibility(show || touchPad.gameTouchEnabled() ? android.view.View.VISIBLE : android.view.View.GONE);
    }
    // Window lease (implementation plan §7.3): surfaceCreated offers a new window generation,
    // surfaceChanged and insets publish revisions of it, surfaceDestroyed revokes it. Hand-over
    // only: the render thread builds the surface and swapchain; nothing here waits for the GPU.
    @Override public void surfaceCreated(SurfaceHolder holder) {
        if (host == 0) return;
        String result = NativeBridge.offerSurface(host, holder.getSurface());
        if (gameMode) gameLifecycle("surface-offered");
        status.setText(getString(R.string.capability_status, capabilities, result));
    }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        touchPad.revokeGameViewport();
        surfaceWidth = w; surfaceHeight = h;
        publishSurface();
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        touchPad.retireGameViewport();
        surfaceWidth = surfaceHeight = 0;
        // Bounded wait: the surface must be unused before this returns; a missed deadline aborts the process.
        if (host != 0) {
            String result = NativeBridge.releaseSurface(host);
            if (gameMode) gameLifecycle("surface-detached " + result);
        }
    }

    /** Public lifecycle receipt for instrumentation/support. A timeout aborts before a detached row exists. */
    private void gameLifecycle(String event) {
        if (!gameMode) return;
        File receipt = new File(getCacheDir(), "game-lifecycle-" + android.os.Process.myPid() + ".tsv");
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(receipt, !"created".equals(event))) {
            out.write((android.os.SystemClock.elapsedRealtime() + "\t" + event + "\n")
                .getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException error) { Log.e("S2Arm", "Lifecycle receipt failed", error); }
    }
    private void publishSurface() {
        if (host == 0 || surfaceWidth == 0 || !surface.getHolder().getSurface().isValid()) return;
        android.view.Display display = surface.getDisplay();
        int rotation = display != null ? display.getRotation() : 0;
        float refresh = display != null ? display.getRefreshRate() : 0f;
        String result = NativeBridge.updateSurface(host, surfaceWidth, surfaceHeight, rotation,
            windowInsets[0], windowInsets[1], windowInsets[2], windowInsets[3], refresh);
        status.setText(getString(R.string.capability_status, capabilities, result));
        tryStartGame();
    }
    private void clearInput() {
        gamepad.reset();
        if (touchPad != null) touchPad.release();   // publishes
        else publishInput();
    }
    private static long magnitude(int x, int y) { return (long) x * x + (long) y * y; }
    // Physical controller and on-screen pad together: buttons OR'ed, each stick from whichever is deflected more.
    private void publishInput() {
        int buttons = gamepad.buttons(), lx = gamepad.leftX(), ly = gamepad.leftY(), rx = gamepad.rightX(), ry = gamepad.rightY();
        if (touchPad != null && touchPad.getVisibility() == android.view.View.VISIBLE) {
            TouchPad t = touchPad.pad();
            buttons |= t.buttons();
            if (magnitude(t.leftX(), t.leftY()) > magnitude(lx, ly)) { lx = t.leftX(); ly = t.leftY(); }
            if (magnitude(t.rightX(), t.rightY()) > magnitude(rx, ry)) { rx = t.rightX(); ry = t.rightY(); }
        }
        if (touchLog && touchPad != null && touchPad.pad().layouts != loggedLayout) {
            loggedLayout = touchPad.pad().layouts;
            StringBuilder sb = new StringBuilder("touchpad layout:");
            for (TouchPad.Control c : touchPad.pad().controls())
                sb.append(String.format(java.util.Locale.ROOT, " %s=(%.0f,%.0f,r%.0f)",
                    c.kind == TouchPad.STICK ? (c.bit == 0 ? "LSTICK" : "RSTICK") : c.kind == TouchPad.DPAD ? "DPAD" : c.label, c.x, c.y, c.radius));
            Log.i("S2Arm", sb.toString());
        }
        if (touchLog) {
            String line = String.format(java.util.Locale.ROOT, "input: buttons=0x%04x L=(%d,%d) R=(%d,%d)", buttons, lx, ly, rx, ry);
            if (!line.equals(lastInputLog)) { Log.i("S2Arm", line); lastInputLog = line; }
        }
        if (host != 0) NativeBridge.input(buttons, lx, ly, rx, ry);
    }
    private static boolean controller(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (!controller(event.getSource()) || !resumed) return super.dispatchKeyEvent(event);
        int bit;
        // Positional layout: Android's bottom A button is Switch B.
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_BUTTON_A: bit = 1; break;
            case KeyEvent.KEYCODE_BUTTON_B: bit = 0; break;
            case KeyEvent.KEYCODE_BUTTON_X: bit = 3; break;
            case KeyEvent.KEYCODE_BUTTON_Y: bit = 2; break;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: bit = 4; break;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: bit = 5; break;
            case KeyEvent.KEYCODE_BUTTON_L1: bit = 6; break;
            case KeyEvent.KEYCODE_BUTTON_R1: bit = 7; break;
            case KeyEvent.KEYCODE_BUTTON_L2: bit = 8; break;
            case KeyEvent.KEYCODE_BUTTON_R2: bit = 9; break;
            case KeyEvent.KEYCODE_BUTTON_START: bit = 10; break;
            case KeyEvent.KEYCODE_BUTTON_SELECT: bit = 11; break;
            case KeyEvent.KEYCODE_DPAD_LEFT: bit = 12; break;
            case KeyEvent.KEYCODE_DPAD_UP: bit = 13; break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: bit = 14; break;
            case KeyEvent.KEYCODE_DPAD_DOWN: bit = 15; break;
            default: return super.dispatchKeyEvent(event);
        }
        if (event.getAction() != KeyEvent.ACTION_MULTIPLE) {
            gamepad.button(bit, event.getAction() == KeyEvent.ACTION_DOWN); publishInput();
        }
        return true;
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (!controller(event.getSource()) || !resumed || event.getAction() != MotionEvent.ACTION_MOVE)
            return super.onGenericMotionEvent(event);
        gamepad.motion(event.getAxisValue(MotionEvent.AXIS_X), event.getAxisValue(MotionEvent.AXIS_Y),
            event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ),
            event.getAxisValue(MotionEvent.AXIS_HAT_X), event.getAxisValue(MotionEvent.AXIS_HAT_Y),
            event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_RTRIGGER), 0.15f);
        publishInput(); return true;
    }

    /**
     * 上一局崩在原生 abort 时，控制台镜像（logcat）看不到原因；会话日志在应用私有目录里，
     * 这里把最近一局的尾部转发到 logcat，便于无 run-as/root 的真机取证。仅游戏 APK。
     */
    private void dumpPreviousSessionLog() {
        File newest = null;
        java.util.ArrayDeque<File> pending = new java.util.ArrayDeque<>();
        pending.add(getFilesDir());
        for (int guard = 0; guard < 4096 && !pending.isEmpty(); guard++) {
            File dir = pending.poll();
            File[] children = dir.listFiles();
            if (children == null) continue;
            for (File child : children) {
                if (child.isDirectory()) { pending.add(child); continue; }
                if (!child.getName().startsWith("host-") || !child.getName().endsWith(".log")) continue;
                if (newest == null || child.lastModified() > newest.lastModified()) newest = child;
            }
        }
        if (newest == null) return;
        try (java.io.RandomAccessFile file = new java.io.RandomAccessFile(newest, "r")) {
            long length = file.length();
            long start = Math.max(0, length - (256 << 10));
            file.seek(start);
            byte[] data = new byte[(int) (length - start)];
            file.readFully(data);
            String[] lines = new String(data, java.nio.charset.StandardCharsets.UTF_8).split("\n");
            int from = Math.max(0, lines.length - 400);
            Log.i("S2Arm", "Previous session log " + newest.getName() + " (" + length + " bytes), tail:");
            for (int i = from; i < lines.length; i++) Log.i("S2Arm", "prev| " + lines[i]);
        } catch (java.io.IOException e) {
            Log.w("S2Arm", "cannot read previous session log: " + e.getMessage());
        }
    }

    // The background shader-cache writer only publishes a record after its data is flushed, and
    // stopGame() exports before NativeBridge.stopGame() closes that writer. The complete copy of
    // the previous session is therefore taken here, on the next launch, before this session can
    // write to the same cache; `--ez exportShaderCache true` arms both timings.
    private void exportPreviousShaderCache() {
        if (previousCacheExported) return;
        previousCacheExported = true;
        maybeExportShaderCache("previous");
    }

    private void maybeExportShaderCache(String tag) {
        if (!getIntent().getBooleanExtra("exportShaderCache", false) || gameSession == null) return;
        String title = gameSession.spec.game.titleId.toLowerCase(java.util.Locale.ROOT);
        File source = new File(new File(gameSession.stateDir, "games"), title + "/cache/shader");
        File external = getExternalFilesDir(null);
        if (external == null) { Log.w("S2Arm", "Shader cache export: no external files dir"); return; }
        File target = new File(external, "s2a-export/shader-" + tag + "-" + System.currentTimeMillis());
        try {
            long bytes = copyTree(source, target);
            if (bytes > 0) Log.i("S2Arm", "Shader cache exported (" + tag + "): " + target + " (" + bytes + " bytes)");
            else Log.w("S2Arm", "Shader cache export (" + tag + "): " + source + " is absent");
        } catch (java.io.IOException e) {
            Log.e("S2Arm", "Shader cache export (" + tag + ") failed: " + e);
        }
    }

    private static long copyTree(File source, File target) throws java.io.IOException {
        if (!source.isDirectory()) return 0;
        if (!target.isDirectory() && !target.mkdirs()) throw new java.io.IOException("cannot create " + target);
        File[] children = source.listFiles();
        if (children == null) return 0;
        long total = 0;
        byte[] buffer = new byte[1 << 20];
        for (File child : children) {
            File destination = new File(target, child.getName());
            if (child.isDirectory()) { total += copyTree(child, destination); continue; }
            try (java.io.FileInputStream in = new java.io.FileInputStream(child);
                 java.io.FileOutputStream out = new java.io.FileOutputStream(destination)) {
                int n;
                while ((n = in.read(buffer)) != -1) { out.write(buffer, 0, n); total += n; }
            }
        }
        return total;
    }

    /**
     * 客体的软件键盘（svc 0x21 / swkbd）：guest 线程阻塞在这里，主线程弹一个带输入框的
     * AlertDialog。取消或没有前台 Activity 时返回 null，调用方按"取消"处理，不伪造输入。
     */
    static String requestText(String title, String initial, int maxChars) {
        final MainActivity activity = current;
        if (activity == null || activity.legacyTextInput == null) return null;
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) return null;
        final long started = System.nanoTime();
        Log.i("S2Arm", "Legacy keyboard request started");
        String result = activity.legacyTextInput.request(title, initial, maxChars);
        Log.i("S2Arm", "Legacy keyboard completed elapsed_ms=" + (System.nanoTime() - started) / 1_000_000 + " accepted=" + (result != null));
        return result;
    }
}
