package org.s2arm.android;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.Map;
import java.util.LinkedHashMap;
import java.io.InputStream;
import java.util.zip.ZipFile;
import java.util.zip.ZipEntry;

/** 一次游戏启动的冻结输入；文件校验仍由 BootContract / GameInstall 负责。 */
final class GameSession implements AutoCloseable {
    final BootSpec spec;
    final String stateDir;
    private boolean attempted;
    private boolean stopping;
    private FrozenStart frozenStart;
    private GameStorage storage;
    private Map<String, String> moduleSha256 = Collections.emptyMap();
    private Map<String, GamePack.Entry> admittedFiles = Collections.emptyMap();
    String resourceValidation = "";
    GameFingerprint.Timing bootTiming;
    private static volatile GameSession nativeSession;

    /** 本进程仍活着的会话：已 begin 且未 stop（#1474：缓存 :game 进程被 bare 启动时，
        空 GameActivity 实例据此判定转库页还是维持红屏）。 */
    static boolean running() {
        GameSession session = nativeSession;
        return session != null && !session.stopping;
    }
    /** Debug/cold-start only: delete this app's private direct program cache before opening the session. */
    static volatile boolean clearDirectCache;

    /** PackageManager identity at the Android boundary; APK assets are still bound to the exact APK bytes. */
    static final class InstalledApk {
        final File file;
        final String packageName;
        final long versionCode;
        final List<String> signerSha256;
        final boolean split;
        InstalledApk(File file, String packageName, long versionCode, List<String> signerSha256, boolean split) {
            this.file = file; this.packageName = packageName; this.versionCode = versionCode;
            this.signerSha256 = signerSha256; this.split = split;
        }
    }

    /**
     * Admission before any guest execution. Keep the storage lease until process exit, including native
     * shutdown: a later import cannot prune the live revision. Native module reads also check the copied bytes.
     */
    static GameSession load(BootSpec spec, File privateRoot, InstalledApk installed, boolean debuggable)
            throws IOException, BootContract.BadGame, GamePack.BadPack {
        return load(spec, privateRoot, installed, debuggable, false, null);
    }

    static GameSession load(BootSpec spec, File privateRoot, InstalledApk installed, boolean debuggable,
            boolean debugEntriesOpen)
            throws IOException, BootContract.BadGame, GamePack.BadPack {
        return load(spec, privateRoot, installed, debuggable, debugEntriesOpen, null);
    }

    static GameSession load(BootSpec spec, File privateRoot, InstalledApk installed, boolean debuggable,
            GameFingerprint.Timing timing)
            throws IOException, BootContract.BadGame, GamePack.BadPack {
        return load(spec, privateRoot, installed, debuggable, false, timing);
    }

    static GameSession load(BootSpec spec, File privateRoot, InstalledApk installed, boolean debuggable,
            boolean debugEntriesOpen, GameFingerprint.Timing timing)
            throws IOException, BootContract.BadGame, GamePack.BadPack {
        return load(spec, privateRoot, installed, debuggable, debugEntriesOpen, timing, null);
    }

    interface SaveRecovery {
        void beforeStart(BootContract.Game game) throws IOException;
    }

    static GameSession load(BootSpec spec, File privateRoot, InstalledApk installed, boolean debuggable,
            boolean debugEntriesOpen, GameFingerprint.Timing timing, SaveRecovery saves)
            throws IOException, BootContract.BadGame, GamePack.BadPack {
        long phase = GameFingerprint.Timing.start(timing);
        GameStorage storage = GameStorage.acquire(privateRoot, new File(spec.gameRoot).getName());   // 只锁这一款（不堵门）
        GameFingerprint.Timing.end(timing, "storage_lock", phase);
        try {
            File gameRoot = new File(spec.gameRoot).getCanonicalFile();
            File expected = new File(privateRoot.getCanonicalFile(), "games/" + gameRoot.getName());
            if (!gameRoot.equals(expected) || Files.isSymbolicLink(expected.toPath()))
                throw new IOException("game root is outside the private game library");
            phase = GameFingerprint.Timing.start(timing);
            BootContract.Game game = BootContract.open(gameRoot);
            if (saves != null) saves.beforeStart(game);
            GameFingerprint.Timing.end(timing, "session_open", phase);
            if (game.apk != null)
                BootContract.requireTestApkPermission(game.titleId, game.apk.packageName,
                    installed == null ? "" : installed.packageName, debuggable, debugEntriesOpen);
            phase = GameFingerprint.Timing.start(timing);
            File revision = BootContract.under(gameRoot, game.revisionDir, "revision");
            Map<String, Object> artifact = Json.object(Json.parse(new String(Files.readAllBytes(
                BootContract.under(revision, game.artifact, "artifact").toPath()), java.nio.charset.StandardCharsets.UTF_8)), "artifact");
            Map<String, Object> toolchain = Json.object(artifact.get("toolchain"), "toolchain");
            // 改写 ABI 已在 BootContract（artifact rewriteAbi + 每个模块 fixups 头）核过；这里的工具链标签和运行时版本号只是标签，
            // 不同只记日志、不挡起局（不堵门）
            if (!("runtime/rewriter@" + BootContract.REWRITE_ABI).equals(toolchain.get("rewriter")))
                System.err.println("S2Arm: artifact rewriter label " + toolchain.get("rewriter") + " differs; ABI " + BootContract.REWRITE_ABI + " verified");
            if (!"0.1.0".equals(game.runtimeVersion)) System.err.println("S2Arm: artifact runtime version " + game.runtimeVersion + " (built for 0.1.0)");
            GameFingerprint.Timing.end(timing, "artifact_checks", phase);
            phase = GameFingerprint.Timing.start(timing);
            byte[] index = GamePack.read(BootContract.under(revision, "index.tsv", "index"));
            String indexSha = GamePack.sha256(index);
            Map<String, Object> pkg = Json.object(artifact.get("package"), "package");
            if (!game.contentRoot.equals("sha256-" + indexSha) || !indexSha.equals(pkg.get("indexSha256")))
                throw new IOException("contentRoot / artifact package index identity mismatch");
            List<GamePack.Entry> entries = GamePack.readIndex(
                GamePack.read(BootContract.under(revision, "manifest.json", "manifest")), index);
            GameFingerprint.Timing.end(timing, "index_manifest_read_hash_parse", phase);
            long bytes = 0;
            Map<String, String> hashes = new LinkedHashMap<>();
            Map<String, GamePack.Entry> admitted = new LinkedHashMap<>();
            long resourceStart = System.nanoTime();
            phase = GameFingerprint.Timing.start(timing);
            GameFingerprint.Reference fingerprint = GameFingerprint.currentReference(gameRoot, indexSha);
            GameFingerprint.Timing.end(timing, "fingerprint_reference", phase);
            GameFingerprint.ApkStamp apkStamp = fingerprint == null ? null : fingerprint.apk;
            boolean apkStampChanged = false;
            // 内容文件的根：原地来源是登记的共享存储目录（元数据和指纹表仍在私有版本目录），否则就是版本目录
            File content = game.inPlaceRoot != null ? new File(game.inPlaceRoot) : revision;
            // 原地来源：可执行文件（revision 来源模块的 image / fixups / gates / widen、exefs 下的 npdm）共享存储上谁都能写，
            //   身份（大小 / inode / 修改时间）可以被恢复，所以每次起局整份重算 SHA-256；数据文件照旧按身份命中
            Map<String, String> executables = new LinkedHashMap<>();
            if (game.inPlaceRoot != null) {
                for (BootContract.Module module : game.modules) {
                    if (!"revision".equals(module.source)) continue;   // apk-asset 模块在 APK 里，按 APK 字节绑定
                    String[] kinds = {"image", "fixups", "gates", "widen"};
                    String[] paths = {module.image, module.fixups, module.gates, module.widen};
                    for (int i = 0; i < paths.length; i++)
                        if (paths[i] != null && !paths[i].isEmpty()) executables.put(paths[i], "module " + module.name + " " + kinds[i]);
                }
                executables.put(game.npdm, "npdm");
                for (GamePack.Entry entry : entries)
                    if (entry.path.startsWith("exefs/") && entry.path.endsWith(".npdm")) executables.putIfAbsent(entry.path, "npdm");
            }
            GameFingerprint.Verification checked = GameFingerprint.verify(revision, content, entries, indexSha, fingerprint, timing,
                game.artifact, executables);
            long resourceNanos = System.nanoTime() - resourceStart;
            GameFingerprint.replayStats(timing);
            phase = GameFingerprint.Timing.start(timing);
            // Index paths are already checked clean, so the admitted key is a plain join.
            String admittedPrefix = content.toPath().toAbsolutePath().normalize().toString() + File.separator;
            for (GamePack.Entry entry : entries) {
                hashes.put(entry.path, entry.sha256); bytes += entry.size;
                admitted.put(admittedPrefix + entry.path.replace('/', File.separatorChar), entry);
            }
            if (entries.size() != game.packageFiles || bytes != game.packageBytes)
                throw new IOException("artifact package counts differ from index");
            // Metadata is allowed only as metadata; runtime data must belong to this contentRoot.
            for (String path : new String[]{game.npdm, game.control})
                if (!hashes.containsKey(path)) throw new IOException("runtime file is outside the content index: " + path);
            File need = new File(new File(content, game.npdm).getParentFile(), "addr_need.tsv");
            if (Files.exists(need.toPath(), LinkOption.NOFOLLOW_LINKS)
                    && !admitted.containsKey(need.toPath().toAbsolutePath().normalize().toString()))
                throw new IOException("addr_need.tsv is outside the content index");
            if (game.artifact.contains("/")) throw new IOException("artifact metadata must be at the revision root");
            GameFingerprint.Timing.end(timing, "admitted_maps_checks", phase);

            Map<String, String> modules = new LinkedHashMap<>();
            if (game.hasApkModules()) {
                BootContract.Apk apk = game.apk;
                boolean bound = false;
                if (installed != null && !installed.split && apk.packageName.equals(installed.packageName)
                        && apk.versionCode == installed.versionCode
                        && installed.signerSha256.contains(apk.signingCertSha256)) {
                    phase = GameFingerprint.Timing.start(timing);
                    GameFingerprint.ApkStamp saved = apkStamp;
                    GameFingerprint.Stamp now = null;
                    if (saved != null && (now = GameFingerprint.stat(installed.file)) != null
                            && saved.same(installed.file, now) && saved.sha256.equals(apk.sha256)) {
                        bound = true;
                        GameFingerprint.Timing.end(timing, "apk_sha_stamp_hit", phase);
                    } else {
                        // A stamp miss must re-verify the actual bytes: the per-process cache keys
                        // on (path,size,millis-mtime) without inode, so it cannot stand in for it.
                        bound = RuntimeLibrary.sha256(installed.file).equals(apk.sha256);
                        GameFingerprint.Timing.end(timing, "apk_sha_recalculated", phase);
                        if (bound) {
                            if (now == null) now = GameFingerprint.stat(installed.file);
                            if (now != null && (saved == null || !saved.same(installed.file, now)
                                    || !saved.sha256.equals(apk.sha256))) {
                                apkStamp = GameFingerprint.ApkStamp.of(installed.file, now, apk.sha256);
                                apkStampChanged = true;
                            }
                        }
                    }
                }
                if (!bound) throw new IOException("installed APK identity differs from the artifact");
                phase = GameFingerprint.Timing.start(timing);
                try (ZipFile zip = new ZipFile(installed.file)) {
                    String mainImage = game.modules.get(0).image;
                    int slash = mainImage.lastIndexOf('/');
                    String moduleDir = slash < 0 ? "" : mainImage.substring(0, slash + 1);
                    // 整包一份的可选表（与 bridge.cpp k_optional_tables 一致）：宽屏策略表 + 社区补丁表 cpatch.tsv（#48 D2）
                    for (String policy : new String[]{"widen_enable.handheld.tsv", "widen_enable.docked.tsv", "widen_full.handheld.tsv", "widen_full.docked.tsv", "widen_verified.tsv", "cpatch.tsv"}) {
                        String path = apk.assetRoot + "/" + moduleDir + policy;
                        if (zip.getEntry("assets/" + path) == null) continue;
                        byte[] policyBytes = asset(zip, path);
                        admitted.put(path, new GamePack.Entry(moduleDir + policy, policyBytes.length, GamePack.sha256(policyBytes)));
                    }
                    for (BootContract.Module module : game.modules) {
                        if (!"apk-asset".equals(module.source)) throw new IOException("mixed module sources are not supported");
                        byte[] fixups = asset(zip, apk.assetRoot + "/" + module.fixups);
                        if (!BootContract.REWRITE_ABI.equals(BootContract.fixupHeaders(
                                new String(fixups, java.nio.charset.StandardCharsets.UTF_8), "asset fixups").get("REWRITE_ABI")))
                            throw new IOException("APK module rewrite ABI mismatch");
                        GamePack.requireAndroidTarget(module.fixups, fixups);
                        byte[] gates = asset(zip, apk.assetRoot + "/" + module.gates);
                        admitted.put(apk.assetRoot + "/" + module.fixups,
                            new GamePack.Entry(module.fixups, fixups.length, GamePack.sha256(fixups)));
                        admitted.put(apk.assetRoot + "/" + module.gates,
                            new GamePack.Entry(module.gates, gates.length, GamePack.sha256(gates)));
                        if (module.widen != null && !module.widen.isEmpty()) {   // F1 #1574：宽屏点表与兄弟文件同口径验收进包（原「widen modules are not supported」拒收已撤）
                            byte[] widenTsv = asset(zip, apk.assetRoot + "/" + module.widen);
                            admitted.put(apk.assetRoot + "/" + module.widen,
                                new GamePack.Entry(module.widen, widenTsv.length, GamePack.sha256(widenTsv)));
                        }
                        ZipEntry image = zip.getEntry("assets/" + apk.assetRoot + "/" + module.image);
                        if (image == null || image.isDirectory()) throw new IOException("APK module image is missing");
                        var digest = GamePack.digest();
                        try (InputStream in = zip.getInputStream(image)) {
                            byte[] buffer = new byte[1 << 16];
                            for (int n; (n = in.read(buffer)) > 0;) digest.update(buffer, 0, n);
                        }
                        String sha = GamePack.hex(digest.digest());
                        modules.put(module.name, sha);
                        admitted.put(apk.assetRoot + "/" + module.image, new GamePack.Entry(module.image, image.getSize(), sha));
                    }
                }
                GameFingerprint.Timing.end(timing, "apk_modules", phase);
            } else {
                phase = GameFingerprint.Timing.start(timing);
                for (BootContract.Module module : game.modules) {
                    for (String path : new String[]{module.image, module.fixups, module.gates, module.widen})
                        if (path != null && !path.isEmpty() && !hashes.containsKey(path))
                            throw new IOException("module file is outside the content index: " + path);
                    modules.put(module.name, hashes.get(module.image));
                }
            }
            if (!game.hasApkModules()) GameFingerprint.Timing.end(timing, "revision_modules", phase);
            long updateStart = System.nanoTime();
            phase = GameFingerprint.Timing.start(timing);
            if (checked.changed) {
                GameFingerprint.Reference replacement = GameFingerprint.write(revision, entries, indexSha, checked.stamps);
                if (replacement != null) {
                    replacement = replacement.withApk(apkStamp);
                    GameFingerprint.publish(gameRoot, replacement);
                    fingerprint = replacement;
                }
            } else if (apkStampChanged && fingerprint != null) {
                fingerprint = fingerprint.withApk(apkStamp);
                GameFingerprint.publish(gameRoot, fingerprint);
            }
            if (!GameFingerprint.reserved(entries)) GameFingerprint.clean(revision, fingerprint);
            resourceNanos += System.nanoTime() - updateStart;
            GameFingerprint.Timing.end(timing, "fingerprint_update_clean", phase);
            spec.game = game;
            spec.gameRoot = gameRoot.getPath();
            GameSession session = new GameSession(spec, privateRoot);
            session.storage = storage;
            session.bootTiming = timing;
            session.moduleSha256 = Collections.unmodifiableMap(modules);
            session.admittedFiles = Collections.unmodifiableMap(admitted);
            session.resourceValidation = "资源校验：指纹命中 " + checked.hits + " / 重算 " + checked.recalculated + " / 用时 " + resourceNanos / 1_000_000 + " ms";
            return session;
        } catch (IllegalArgumentException error) {
            storage.close();
            throw new IOException("invalid session admission metadata", error);
        } catch (IOException | BootContract.BadGame | GamePack.BadPack | RuntimeException error) {
            storage.close();
            throw error;
        }
    }

    /** Distinguish an absent optional table from a previously admitted table that disappeared. */
    static int nativeFileAdmission(String path) {
        GameSession session = nativeSession;
        return session == null ? -1 : session.admittedFiles.containsKey(path) ? 1 : 0;
    }

    /** Called on stable native memory, after copying and before handing bytes to the loader. Never reopens a path. */
    static boolean verifyNativeBytes(String path, java.nio.ByteBuffer bytes) {
        GameSession session = nativeSession;
        GamePack.Entry entry = session == null ? null : session.admittedFiles.get(path);
        if (entry == null || bytes == null || entry.size != bytes.remaining()) return false;
        var digest = GamePack.digest(); digest.update(bytes.asReadOnlyBuffer());
        return entry.sha256.equals(GamePack.hex(digest.digest()));
    }

    private void recheckNativeMetadata() throws IOException {
        if (admittedFiles.isEmpty()) return; // Direct constructor is only used by host lifecycle fixtures.
        File npdm = new File(spec.game.contentDir(new File(spec.gameRoot)), spec.game.npdm);
        for (File file : new File[]{npdm, new File(npdm.getParentFile(), "addr_need.tsv")}) {
            String path = file.toPath().toAbsolutePath().normalize().toString();
            GamePack.Entry entry = admittedFiles.get(path);
            if (entry == null && !Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS)) continue;
            if (entry == null || !file.isFile() || file.length() != entry.size || !entry.sha256.equals(RuntimeLibrary.sha256(file)))
                throw new IOException("native metadata changed after admission: " + file.getName());
        }
    }

    private static byte[] asset(ZipFile zip, String path) throws IOException {
        ZipEntry entry = zip.getEntry("assets/" + path);
        if (entry == null || entry.isDirectory()) throw new IOException("APK module asset is missing: " + path);
        try (InputStream in = zip.getInputStream(entry)) { return GamePack.readAll(in); }
    }

    /** Only before native launch (or host tests). A running game releases its lease by exiting the process. */
    @Override public void close() throws IOException { if (nativeSession == this) nativeSession = null; if (storage != null) { storage.close(); storage = null; } }

    /** Immutable values consumed together by HostApi registration, the JSON record and startGame. */
    static final class FrozenStart {
        final String bootJson, sessionId, titleId, language, region, timezone;
        final String assetRoot, revisionDir, npdmPath, stateDir;
        final long programId;
        final BootSpec.ConsoleMode consoleMode;
        final int modePreference;      // 0 auto / 1 generic / 2 direct；与 native s2a_direct_mode_preference 一致
        final byte[] graphicsDescriptor; // null = 包没有 org.s2arm.graphics.v1
        final List<BootContract.Module> modules;
        final Map<String, String> moduleSha256;

        private FrozenStart(BootSpec spec, String stateDir, Map<String, String> moduleSha256, int modePreference) {
            bootJson = spec.toJson();
            consoleMode = spec.consoleMode;
            sessionId = spec.sessionId;
            titleId = spec.game.titleId;
            language = spec.user.language;
            region = spec.user.region == null ? "" : spec.user.region;
            timezone = spec.user.timezone;
            assetRoot = spec.game.hasApkModules() ? spec.game.apk.assetRoot : "";
            revisionDir = spec.game.contentDir(new File(spec.gameRoot)).getPath();   // 原生按它读游戏文件：原地来源是登记的根
            npdmPath = new File(revisionDir, spec.game.npdm).getPath();
            this.stateDir = stateDir;
            programId = Long.parseUnsignedLong(titleId, 16);
            this.modePreference = modePreference;
            graphicsDescriptor = spec.game.graphics == null ? null : spec.game.graphics.descriptor;
            modules = Collections.unmodifiableList(new ArrayList<>(spec.game.modules));
            this.moduleSha256 = moduleSha256;
        }

        int nativeConsoleMode() { return consoleMode.nativeValue; }
    }

    GameSession(BootSpec spec, File privateRoot) throws IOException {
        if (!"game".equals(spec.mode) || spec.game == null
                || spec.game.modules.isEmpty() || !"main".equals(spec.game.modules.get(0).name))
            throw new IOException("game session requires validated input and main-first module order");
        for (BootContract.Module module : spec.game.modules)
            if (!(spec.game.hasApkModules() ? "apk-asset" : "revision").equals(module.source))
                throw new IOException("mixed module sources are not supported");
        this.spec = spec;
        stateDir = prepareState(privateRoot, new File(spec.userRoot), new File(spec.saveRoot)).getAbsolutePath();
        if (clearDirectCache) {
            File cache = new File(stateDir, "direct-cache/nvn-vk");
            try {
                deleteTree(cache);
                System.err.println("S2Arm: cleared private direct program cache: " + cache);   // 纯 Java 类不碰 android.*（test.sh / verified_source_test 在 JVM 上编），System.err 在安卓上照样进 logcat
            } catch (IOException error) {
                System.err.println("S2Arm: could not clear private direct program cache " + cache + ": " + error);
            }
        }
    }

    private static void deleteTree(File file) throws IOException {
        if (!file.exists() && !java.nio.file.Files.isSymbolicLink(file.toPath())) return;
        if (java.nio.file.Files.isSymbolicLink(file.toPath())) {
            java.nio.file.Files.deleteIfExists(file.toPath());
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) deleteTree(child);
        }
        java.nio.file.Files.deleteIfExists(file.toPath());
    }

    /** UI owner only: freeze once when the surface is ready; this does not prove guest execution. */
    FrozenStart begin(boolean foreground, boolean surfaceValid, int width, int height, int dpi, float hz,
                      BootSpec.ConsoleMode consoleMode) throws IOException {
        return begin(foreground, surfaceValid, width, height, dpi, hz, consoleMode, 0);
    }

    FrozenStart begin(boolean foreground, boolean surfaceValid, int width, int height, int dpi, float hz,
                      BootSpec.ConsoleMode consoleMode, int modePreference) throws IOException {
        if (consoleMode == null) throw new IllegalArgumentException("Console mode is required");
        if (modePreference < 0 || modePreference > 2)
            throw new IllegalArgumentException("Graphics mode preference must be 0 auto / 1 generic / 2 direct");
        if (attempted && (frozenStart.consoleMode != consoleMode || frozenStart.modePreference != modePreference))
            throw new IllegalStateException("Console/graphics mode is frozen; use a cold start");
        if (attempted || stopping || !foreground || !surfaceValid || width <= 0 || height <= 0
                || dpi <= 0 || !Float.isFinite(hz) || hz <= 0) return null;
        recheckNativeMetadata();
        nativeSession = this;
        spec.screen.width = width;
        spec.screen.height = height;
        spec.screen.densityDpi = dpi;
        spec.screen.refreshHz = hz;
        spec.consoleMode = consoleMode;
        FrozenStart start = new FrozenStart(spec, stateDir, moduleSha256, modePreference);
        frozenStart = start;
        attempted = true;
        return start;
    }

    boolean attempted() { return attempted; }
    void stop() { stopping = true; }

    private static String apkKey, apkSha256;

    /** The installed base APK's SHA-256, hashed once per process: the runtime-library check and the artifact binding share it. */
    static synchronized String apkSha256(File installedApk) throws IOException {
        return apkSha256(installedApk, null);
    }

    private static synchronized String apkSha256(File installedApk, GameFingerprint.Timing timing) throws IOException {
        long start = GameFingerprint.Timing.start(timing);
        String key = installedApk.getPath() + ":" + installedApk.length() + ":" + installedApk.lastModified();
        boolean hit = key.equals(apkKey);
        if (!hit) {
            apkSha256 = RuntimeLibrary.sha256(installedApk);
            apkKey = key;
        }
        GameFingerprint.Timing.end(timing, hit ? "apk_sha_cache_hit" : "apk_sha_recalculated", start);
        return apkSha256;
    }

    /** A signer/version pair is not a build identity: bind the exact installed base APK bytes. */
    static boolean matchesApk(File installedApk, String expectedSha256) throws IOException {
        return apkSha256(installedApk).equals(expectedSha256);
    }

    private static boolean matchesApk(File installedApk, String expectedSha256, GameFingerprint.Timing timing) throws IOException {
        return apkSha256(installedApk, timing).equals(expectedSha256);
    }

    /** 一款游戏的用户根 / 存档根（BootSpecSource 起局时填 BootSpec 用的同一对路径）。 */
    static File userRoot(File privateRoot) { return new File(privateRoot, "user/0"); }
    static File saveRoot(File privateRoot, String titleId) { return new File(userRoot(privateRoot), "saves/" + titleId); }

    /** 这款起局时交给原生层的 state 目录（安卓的 S2A_DOCS，runtime.c 绑定）：起局与库页导入 mod（GameMods）共用这一处算法。 */
    static File stateDir(File privateRoot, String titleId) throws IOException {
        return prepareState(privateRoot, userRoot(privateRoot), saveRoot(privateRoot, titleId));
    }

    /** state 目录在存档根下的名字（prepareState 建的就是它）。 */
    static final String STATE_DIR = "ryujinx-android";

    /** 同一个 state 目录，只算路径：不建目录、不查链接（库页读游玩时间这类只读展示用，起局与导入 mod 仍走 stateDir）。 */
    static File statePath(File privateRoot, String titleId) {
        return new File(saveRoot(privateRoot, titleId), STATE_DIR);
    }

    /** Ryujinx 使用自己的持久目录；不把用户 saveRoot 当作导入种子，也不迁移或覆盖旧布局。 */
    static File prepareState(File privateRoot, File userRoot, File saveRoot) throws IOException {
        Path app = privateRoot.toPath().toAbsolutePath().normalize();
        Path user = userRoot.toPath().toAbsolutePath().normalize();
        Path save = saveRoot.toPath().toAbsolutePath().normalize();
        if (!user.startsWith(app) || user.equals(app) || !save.startsWith(user) || save.equals(user))
            throw new IOException("persistent root is outside the app's private user directory");
        // Android 自身的 /data/user/0 别名属于 Context 的可信根；只拒绝根下用户数据路径中的链接。
        Path current = privateRoot.getCanonicalFile().toPath();
        for (Path part : app.relativize(save)) {
            current = current.resolve(part);
            if (Files.isSymbolicLink(current)) throw new IOException("persistent path contains a symbolic link");
            if (Files.exists(current, LinkOption.NOFOLLOW_LINKS) && !Files.isDirectory(current, LinkOption.NOFOLLOW_LINKS))
                throw new IOException("persistent path is not a directory");
        }
        // Keep creation, legacy checks and the native-facing state path on the validated root.
        save = current;
        Files.createDirectories(save);
        File[] old = save.toFile().listFiles();
        if (old == null) throw new IOException("cannot inspect save root");
        // 存档根目录里的其他东西（玩家放进来的原生格式存档、旧版布局）不挡起局：原样留着、不迁移不覆盖，只记日志（不堵门）
        for (File entry : old)
            if (!entry.getName().equals(STATE_DIR))
                System.err.println("S2Arm: save root keeps an existing entry untouched (no migration): " + entry.getName());
        Path state = save.resolve(STATE_DIR);
        if (Files.isSymbolicLink(state) || (Files.exists(state, LinkOption.NOFOLLOW_LINKS)
                && !Files.isDirectory(state, LinkOption.NOFOLLOW_LINKS)))
            throw new IOException("Ryujinx state path conflicts with an existing file or link");
        Files.createDirectories(state);
        return state.toFile();
    }
}
