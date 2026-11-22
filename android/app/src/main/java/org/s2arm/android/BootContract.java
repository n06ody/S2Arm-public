package org.s2arm.android;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;

/**
 * Runtime side of the A0 handoff: read files/games/&lt;titleId&gt;/current.json, resolve the one
 * active revision, validate game-build-artifact.json and the files the loader will open.
 *
 * <p>This does not execute anything and does not re-hash the whole package: the installer has
 * already verified every package file. The runtime checks the activation pointer, the artifact
 * hash, path safety, the rewrite ABI and the module files it is about to map.
 */
final class BootContract {
    static final String REWRITE_ABI = "android-sp-safe-v1";

    static final class BadGame extends Exception {
        private static final long serialVersionUID = 1L;

        BadGame(String message) {
            super(message);
        }
    }

    static final class Module {
        final String name, source, image, fixups, gates, widen, buildId;

        Module(String name, String source, String image, String fixups, String gates, String widen, String buildId) {
            this.name = name;
            this.source = source;
            this.image = image;
            this.fixups = fixups;
            this.gates = gates;
            this.widen = widen;
            this.buildId = buildId;
        }
    }

    static final class Graphics {
        final String manifest, sha256;
        final long bytes;
        final byte[] descriptor;

        Graphics(String manifest, String sha256, long bytes, byte[] descriptor) {
            this.manifest = manifest;
            this.sha256 = sha256;
            this.bytes = bytes;
            this.descriptor = descriptor;
        }
    }

    static final class Apk {
        final String packageName;
        final long versionCode;
        final String sha256, signingCertSha256, assetRoot;

        Apk(String packageName, long versionCode, String sha256, String signingCertSha256, String assetRoot) {
            this.packageName = packageName;
            this.versionCode = versionCode;
            this.sha256 = sha256;
            this.signingCertSha256 = signingCertSha256;
            this.assetRoot = assetRoot;
        }
    }

    static final class Game {
        final String titleId, contentRoot, revisionDir, artifact, artifactSha256;
        final String rewriteAbi, runtimeVersion, backend;
        final String npdm, control, romfs, modulesDir;
        /** 原地来源：游戏文件所在的共享存储根目录（玩家选定、登记在私有版本目录的 in-place.json 里）；null = 文件在私有版本目录里。 */
        final String inPlaceRoot;
        final long packageFiles, packageBytes;
        final List<Module> modules;
        final Apk apk;
        final Graphics graphics;   // null = 没有 org.s2arm.graphics.v1，本局只能 generic
        boolean mapped;

        Game(String titleId, String contentRoot, String revisionDir, String artifact, String artifactSha256,
             String rewriteAbi, String runtimeVersion, String backend, String npdm, String control,
             String romfs, String modulesDir, String inPlaceRoot, long packageFiles, long packageBytes,
             List<Module> modules, Apk apk, Graphics graphics) {
            this.titleId = titleId;
            this.contentRoot = contentRoot;
            this.revisionDir = revisionDir;
            this.artifact = artifact;
            this.artifactSha256 = artifactSha256;
            this.rewriteAbi = rewriteAbi;
            this.runtimeVersion = runtimeVersion;
            this.backend = backend;
            this.npdm = npdm;
            this.control = control;
            this.romfs = romfs;
            this.modulesDir = modulesDir;
            this.inPlaceRoot = inPlaceRoot;
            this.packageFiles = packageFiles;
            this.packageBytes = packageBytes;
            this.modules = modules;
            this.apk = apk;
            this.graphics = graphics;
        }

        /** 内容文件（romfs / exefs / 模块 …）所在目录：原地来源是登记的根目录，否则是私有版本目录。
         *  元数据（manifest / index / artifact / 指纹表）总在私有版本目录 gameRoot/revisionDir。 */
        File contentDir(File gameRoot) {
            return inPlaceRoot != null ? new File(inPlaceRoot) : new File(gameRoot, revisionDir);
        }

        boolean hasApkModules() {
            for (Module m : modules) if ("apk-asset".equals(m.source)) return true;
            return false;
        }

        String summary() {
            String where = hasApkModules() ? "apk-asset" : "revision";
            return (mapped ? "ReadyToPlay " : "Validated ") + titleId + " [" + contentRoot + "] "
                + modules.size() + " module(s) from " + where + ", " + rewriteAbi + ", " + backend;
        }
    }

    private static final int MAX_JSON = 8 << 20;
    private static final Pattern TITLE_ID = Pattern.compile("[0-9A-F]{16}");
    private static final Pattern CONTENT_ROOT = Pattern.compile("[A-Za-z0-9][A-Za-z0-9._:-]{0,127}");
    private static final Pattern MODULE_NAME = Pattern.compile("[A-Za-z0-9_]+");
    private static final Pattern HEX64 = Pattern.compile("[0-9a-f]{64}");
    private static final Pattern BUILD_ID = Pattern.compile("[0-9a-f]{32}|[0-9a-f]{40}");
    private static final Pattern APK_PACKAGE = Pattern.compile("org\\.s2arm\\.game\\.t([0-9a-f]{16})((?:\\.w2)?)");
    private static final Pattern FEATURE_NAME = Pattern.compile("[a-z][a-z0-9_-]*(\\.[a-z][a-z0-9_-]*)+");
    static final String GRAPHICS_FEATURE = "org.s2arm.graphics.v1";
    static final String GRAPHICS_MANIFEST = "shaders/vulkan/direct/graphics.v1.json";
    /** 私有版本目录里的原地登记：{"version":1,"root":"<共享存储上的绝对路径>"}。只有 App 自己写（私有目录），
     *  在就表示内容文件不在本目录、在 root 下原地读；起局只认 root 之下的文件（canonical 不出根）。 */
    static final String IN_PLACE = "in-place.json";
    private static final int MAX_GRAPHICS_DESCRIPTOR = 1 << 20;

    /** Maps the validated module list; the app passes its exact images to NativeBridge.checkModulePaths. */
    interface ModuleMapper {
        String map(Game game) throws Exception;
    }

    /** Structural game identity only; permission still comes from the installed Debug app. */
    static boolean validateApkPackage(String titleId, String packageName) throws BadGame {
        java.util.regex.Matcher match = APK_PACKAGE.matcher(packageName);
        if (!match.matches() || !match.group(1).equals(titleId.toLowerCase(Locale.ROOT)))
            throw new BadGame("artifact apk.packageName does not match its titleId");
        return !match.group(2).isEmpty();
    }

    static String packageTitle(String packageName, boolean debugEntriesOpen) {
        java.util.regex.Matcher match = APK_PACKAGE.matcher(packageName);
        if (!match.matches()) return null;
        if (!match.group(2).isEmpty() && !debugEntriesOpen)
            throw new IllegalArgumentException("test applicationId requires the DebugGate");
        return match.group(1).toUpperCase(Locale.ROOT);
    }

    static void requireTestApkPermission(String titleId, String packageName, String installedPackage,
            boolean debuggable, boolean debugEntriesOpen) throws BadGame {
        if (validateApkPackage(titleId, packageName)
                && (!debuggable || !debugEntriesOpen || !packageName.equals(installedPackage)))
            throw new BadGame("test applicationId requires this exact installed debug APK and the DebugGate");
    }

    /** Run on the exact artifact bytes passed to the installer, before its first write. */
    static void checkTestImport(byte[] bytes, String installedPackage, boolean debuggable,
            boolean debugEntriesOpen) throws BadGame {
        Map<String, Object> art = Json.object(Json.parse(new String(bytes, StandardCharsets.UTF_8)), "artifact");
        if (art.containsKey("apk")) {
            Map<String, Object> apk = object(art, "apk", "artifact");
            requireTestApkPermission(string(art, "titleId", "artifact"),
                string(apk, "packageName", "artifact.apk"), installedPackage, debuggable, debugEntriesOpen);
        }
    }

    private BootContract() {
    }

    static Game open(File gameRoot) throws BadGame {
        try {
            return open0(gameRoot);
        } catch (BadGame e) {
            throw e;
        } catch (IOException | RuntimeException e) {
            throw new BadGame(e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage());
        }
    }

    static Game openAndMap(File gameRoot, ModuleMapper mapper) throws BadGame {
        Game game = open(gameRoot);
        try {
            String result = mapper.map(game);
            if (result == null || !result.startsWith("modules "))
                throw new BadGame("module map failed: " + result);
        } catch (BadGame e) {
            throw e;
        } catch (Exception e) {
            throw new BadGame("module map failed: " + (e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage()));
        }
        game.mapped = true;
        return game;
    }

    /** The activation pointer, parsed but not yet followed. The installer can read it without a revision. */
    static final String OBJECT_STORE_EXTENSION = "org.s2arm.android.objects.v1";

    static final class Current {
        final String titleId, contentRoot, revisionDir, artifact, artifactSha256;
        final boolean managedObjects;
        Current(String titleId, String contentRoot, String revisionDir, String artifact, String artifactSha256,
                boolean managedObjects) {
            this.titleId = titleId;
            this.contentRoot = contentRoot;
            this.revisionDir = revisionDir;
            this.artifact = artifact;
            this.artifactSha256 = artifactSha256;
            this.managedObjects = managedObjects;
        }
    }

    private static Game open0(File gameRoot) throws BadGame, IOException {
        File root = gameRoot.getCanonicalFile();
        Current current = readCurrent(root);
        return validateRevision(root, current.revisionDir, current.contentRoot,
            current.artifact, current.artifactSha256);
    }

    /** Reads and checks current.json, including the title directory and path rules. */
    static Current readCurrent(File gameRoot) throws BadGame, IOException {
        File root = gameRoot.getCanonicalFile();
        Map<String, Object> current = json(new File(root, "current.json"), "current.json");
        checkCompatibility(current, "current.json", false);

        long version = number(current, "version", "current.json");
        if (version != 1) throw new BadGame("current.json version " + version + " is not 1");
        String titleId = string(current, "titleId", "current.json");
        if (!TITLE_ID.matcher(titleId).matches()) throw new BadGame("current.json titleId is not 16 uppercase hex");
        if (!root.getName().equals(titleId))
            throw new BadGame("game directory " + root.getName() + " is not titleId " + titleId);
        String contentRoot = string(current, "contentRoot", "current.json");
        if (!CONTENT_ROOT.matcher(contentRoot).matches()) throw new BadGame("current.json contentRoot is invalid");
        String revisionDir = string(current, "revisionDir", "current.json");
        if (!revisionDir.equals("revisions/" + contentRoot))
            throw new BadGame("current.json revisionDir is not revisions/<contentRoot>");
        String artifactRel = string(current, "artifact", "current.json");
        checkRelative(artifactRel, "current.json artifact");
        if (!artifactRel.endsWith(".json")) throw new BadGame("current.json artifact is not a .json file");
        String artifactSha = string(current, "artifactSha256", "current.json");
        if (!HEX64.matcher(artifactSha).matches()) throw new BadGame("current.json artifactSha256 is not 64 lowercase hex");
        boolean managed = current.containsKey("extensions")
            && object(current, "extensions", "current.json").containsKey(OBJECT_STORE_EXTENSION);
        return new Current(titleId, contentRoot, revisionDir, artifactRel, artifactSha, managed);
    }

    /** Validates one revision without reading current.json. Used by the installer before the atomic rename. */
    static Game validateRevision(File gameRoot, String revisionDir, String contentRoot,
                                 String artifactRel, String artifactSha) throws BadGame, IOException {
        File root = gameRoot.getCanonicalFile();
        String titleId = root.getName();
        if (!TITLE_ID.matcher(titleId).matches())
            throw new BadGame("game directory is not a 16-hex titleId: " + titleId);
        if (!CONTENT_ROOT.matcher(contentRoot).matches())
            throw new BadGame("expected contentRoot is invalid");
        if (!revisionDir.equals("revisions/" + contentRoot) &&
            !revisionDir.equals("revisions/" + contentRoot + ".staging"))
            throw new BadGame("revisionDir is neither revisions/<contentRoot> nor its .staging directory");
        checkRelative(artifactRel, "artifact");
        if (!artifactRel.endsWith(".json")) throw new BadGame("artifact is not a .json file");
        if (!HEX64.matcher(artifactSha).matches()) throw new BadGame("artifactSha256 is not 64 lowercase hex");

        File revision = under(root, revisionDir, "revisionDir");
        if (!revision.isDirectory()) throw new BadGame("revision directory is missing: " + revisionDir);
        File artifact = under(revision, artifactRel, "artifact");
        if (!artifact.isFile()) throw new BadGame("artifact is missing: " + artifactRel);
        if (!sha256(artifact).equals(artifactSha)) throw new BadGame("artifact does not match artifactSha256");

        Map<String, Object> art = json(artifact, artifactRel);
        boolean graphicsRequired = checkCompatibility(art, artifactRel, true);
        long av = number(art, "version", artifactRel);
        if (av != 1) throw new BadGame(artifactRel + " version " + av + " is not 1");
        if (!titleId.equals(string(art, "titleId", artifactRel)))
            throw new BadGame("artifact titleId differs from current.json");
        if (!contentRoot.equals(string(art, "contentRoot", artifactRel)))
            throw new BadGame("artifact contentRoot differs from current.json");
        if (!"android".equals(string(art, "target", artifactRel)))
            throw new BadGame("artifact target is not android");
        if (!"aarch64".equals(string(art, "guestAbi", artifactRel)))
            throw new BadGame("artifact guestAbi is not aarch64");
        String rewriteAbi = string(art, "rewriteAbi", artifactRel);
        if (!REWRITE_ABI.equals(rewriteAbi))
            throw new BadGame("artifact rewriteAbi " + rewriteAbi + " is not " + REWRITE_ABI);

        Map<String, Object> runtime = object(art, "runtime", artifactRel);
        String runtimeVersion = string(runtime, "version", artifactRel + ".runtime");
        String backend = string(runtime, "backend", artifactRel + ".runtime");
        if (!"vulkan".equals(backend)) throw new BadGame("artifact runtime.backend is not vulkan");

        Map<String, Object> pkg = object(art, "package", artifactRel);
        if (!"manifest.json".equals(string(pkg, "manifest", artifactRel + ".package")))
            throw new BadGame("artifact package.manifest is not manifest.json");
        if (!HEX64.matcher(string(pkg, "indexSha256", artifactRel + ".package")).matches())
            throw new BadGame("artifact package.indexSha256 is not 64 lowercase hex");
        long packageFiles = number(pkg, "files", artifactRel + ".package");
        long packageBytes = number(pkg, "bytes", artifactRel + ".package");
        if (packageFiles < 1) throw new BadGame("artifact package.files is not positive");
        if (packageBytes < 1) throw new BadGame("artifact package.bytes is not positive");
        requireFile(revision, "manifest.json", "package manifest");
        requireFile(revision, "index.tsv", "package index");
        String inPlaceRoot = inPlaceRoot(revision, root.getParentFile());
        // 内容文件的根：原地来源是登记的共享存储目录，否则就是版本目录本身
        File content = inPlaceRoot == null ? revision : new File(inPlaceRoot);

        Apk apk = null;
        if (art.containsKey("apk")) {
            Map<String, Object> a = object(art, "apk", artifactRel);
            String packageName = string(a, "packageName", artifactRel + ".apk");
            validateApkPackage(titleId, packageName);
            long versionCode = number(a, "versionCode", artifactRel + ".apk");
            if (versionCode < 1) throw new BadGame("artifact apk.versionCode is not positive");
            String apkSha = string(a, "sha256", artifactRel + ".apk");
            String certSha = string(a, "signingCertSha256", artifactRel + ".apk");
            if (!HEX64.matcher(apkSha).matches()) throw new BadGame("artifact apk.sha256 is not 64 lowercase hex");
            if (!HEX64.matcher(certSha).matches()) throw new BadGame("artifact apk.signingCertSha256 is not 64 lowercase hex");
            String assetRoot = string(a, "assetRoot", artifactRel + ".apk");
            checkAssetRoot(assetRoot);
            if (!"game/modules".equals(assetRoot))
                throw new BadGame("artifact apk.assetRoot is " + assetRoot + ", expected game/modules");
            apk = new Apk(packageName, versionCode, apkSha, certSha, assetRoot);
        }

        String npdm = path(art, "npdm", artifactRel);
        String control = path(art, "control", artifactRel);
        String romfs = path(art, "romfs", artifactRel);
        requireFile(content, npdm, "npdm");
        requireFile(content, control, "control");
        // 镜像形态（make_pack.romfs_image）：artifact 写 romfs.std，是文件；否则是 romfs/ 目录
        if (romfs.equals("romfs.std")) requireFile(content, romfs, "romfs");
        else requireDirectory(content, romfs, "romfs");
        if (art.containsKey("systemData")) requireDirectory(content, path(art, "systemData", artifactRel), "systemData");

        List<Object> modules = array(art, "modules", artifactRel);
        if (modules.isEmpty()) throw new BadGame("artifact has no modules");
        List<Module> out = new ArrayList<>();
        Set<String> names = new HashSet<>();
        for (int i = 0; i < modules.size(); i++) {
            String where = artifactRel + ".modules[" + i + "]";
            Map<String, Object> m = Json.object(modules.get(i), where);
            String name = string(m, "name", where);
            if (!MODULE_NAME.matcher(name).matches()) throw new BadGame(where + ".name is invalid");
            if (!names.add(name)) throw new BadGame(where + ".name duplicates module " + name);
            String source = string(m, "source", where);
            if (!source.equals("apk-asset") && !source.equals("revision"))
                throw new BadGame(where + ".source is neither apk-asset nor revision");
            String image = path(m, "image", where);
            String fixups = path(m, "fixups", where);
            String gates = path(m, "gates", where);
            String widen = m.containsKey("widen") ? path(m, "widen", where) : "";
            if (!image.endsWith(".patched.bin"))
                throw new BadGame(where + ".image is not a *.patched.bin path");
            String base = image.substring(image.lastIndexOf('/') + 1, image.length() - ".patched.bin".length());
            if (!base.equals(name)) throw new BadGame(where + ".image does not match module name " + name);
            if (!fixups.endsWith(".fixups.tsv"))
                throw new BadGame(where + ".fixups is not a *.fixups.tsv path");
            if (!gates.endsWith(".gates.tsv"))
                throw new BadGame(where + ".gates is not a *.gates.tsv path");
            String stem = image.substring(0, image.length() - ".patched.bin".length());
            if (!fixups.equals(stem + ".fixups.tsv") || !gates.equals(stem + ".gates.tsv"))
                throw new BadGame(where + ".fixups / .gates must share the image directory and module name");
            if (!"android".equals(string(m, "target", where)))
                throw new BadGame(where + ".target is not android");
            String buildId = m.containsKey("buildId") ? string(m, "buildId", where) : "";
            if (!buildId.isEmpty() && !BUILD_ID.matcher(buildId).matches())
                throw new BadGame(where + ".buildId is not 32/40 lowercase hex");
            if (source.equals("apk-asset")) {
                if (apk == null) throw new BadGame(where + ".source is apk-asset but artifact has no apk object");
            } else {
                requireFile(content, image, where + ".image");
                File fixupFile = requireFile(content, fixups, where + ".fixups");
                requireFile(content, gates, where + ".gates");
                if (!widen.isEmpty()) requireFile(content, widen, where + ".widen");
                Map<String, String> h = fixupHeaders(readText(fixupFile), where + ".fixups");
                if (!"android".equals(h.get("TARGET")))
                    throw new BadGame(where + ".fixups has no '# TARGET android'");
                String declaredAbi = h.get("REWRITE_ABI");
                if (declaredAbi == null)
                    throw new BadGame(where + ".fixups does not declare rewrite ABI " + rewriteAbi);
                if (!rewriteAbi.equals(declaredAbi))
                    throw new BadGame(where + ".fixups declares " + declaredAbi + ", expected " + rewriteAbi);
            }
            out.add(new Module(name, source, image, fixups, gates, widen, buildId));
        }
        orderModules(out, artifactRel);

        Graphics graphics = null;
        Map<String, Object> extensions = art.containsKey("extensions")
            ? object(art, "extensions", artifactRel) : null;
        Map<String, Object> graphicsExtension = extensions != null && extensions.containsKey(GRAPHICS_FEATURE)
            ? object(extensions, GRAPHICS_FEATURE, artifactRel + ".extensions") : null;
        if (graphicsRequired && graphicsExtension == null)
            throw new BadGame("artifact requires " + GRAPHICS_FEATURE + " but has no matching extension");
        // 扩展在、只是 requiredFeatures 没登记：扩展本身照常核对、照常用，只记日志（不堵门）
        if (!graphicsRequired && graphicsExtension != null)
            System.err.println("S2Arm: " + artifactRel + " has the " + GRAPHICS_FEATURE + " extension without a requiredFeatures entry; using it");
        if (graphicsExtension != null) {
            String where = artifactRel + ".extensions." + GRAPHICS_FEATURE;
            Set<String> expected = new HashSet<>();
            expected.add("manifest");
            expected.add("sha256");
            expected.add("bytes");
            if (!expected.equals(graphicsExtension.keySet()))
                throw new BadGame(where + " must contain exactly manifest, sha256, bytes");
            String manifest = string(graphicsExtension, "manifest", where);
            // 契约固定一个只读位置；不接受任意相对路径，避免把 artifact 指到 shader 产物之外。
            if (!GRAPHICS_MANIFEST.equals(manifest))
                throw new BadGame(where + ".manifest is not " + GRAPHICS_MANIFEST);
            String hash = string(graphicsExtension, "sha256", where);
            if (!HEX64.matcher(hash).matches())
                throw new BadGame(where + ".sha256 is not 64 lowercase hex");
            long bytes = number(graphicsExtension, "bytes", where);
            if (bytes < 1 || bytes > MAX_GRAPHICS_DESCRIPTOR)
                throw new BadGame(where + ".bytes is outside 1.." + MAX_GRAPHICS_DESCRIPTOR);
            File descriptor = requireFile(content, manifest, "graphics manifest");
            if (descriptor.length() != bytes)
                throw new BadGame(where + ".bytes " + bytes + " != file length " + descriptor.length());
            if (!sha256(descriptor).equals(hash))
                throw new BadGame(where + ".sha256 does not match " + manifest);
            graphics = new Graphics(manifest, hash, bytes, read(descriptor, MAX_GRAPHICS_DESCRIPTOR));
        }

        return new Game(titleId, contentRoot, revisionDir, artifactRel, artifactSha,
            rewriteAbi, runtimeVersion, backend, npdm, control, romfs,
            new File(content, "modules").getPath(), inPlaceRoot, packageFiles, packageBytes, out, apk, graphics);
    }

    /**
     * 版本目录的原地登记（in-place.json），没有返回 null。登记的根必须是绝对、规范（canonical 等于自身：路径上没有符号链接、
     * 没有 . / ..）、不含控制字符 / 引号 / 反斜杠的目录，且不在本 App 的游戏库（gamesDir）里：内容文件只按这个根解析，
     * under() 的 canonical 检查保证读不出根。根不在或读不到时具名报错（玩家挪走 / 删了文件夹、没开「所有文件访问」）。
     */
    static String inPlaceRoot(File revision, File gamesDir) throws BadGame, IOException {
        File marker = new File(revision, IN_PLACE);
        if (!java.nio.file.Files.exists(marker.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) return null;
        if (!java.nio.file.Files.isRegularFile(marker.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS))
            throw new BadGame(IN_PLACE + " is not a regular file");
        Map<String, Object> doc = json(marker, IN_PLACE);
        if (number(doc, "version", IN_PLACE) != 1) throw new BadGame(IN_PLACE + " version is not 1");
        String root = string(doc, "root", IN_PLACE);
        checkInPlaceRoot(root, gamesDir);
        File dir = new File(root);
        if (!dir.isDirectory())
            throw new BadGame("in-place game folder is missing or unreadable: " + root);
        if (!dir.getCanonicalPath().equals(root))
            throw new BadGame("in-place game folder is no longer its registered path (moved or replaced by a link): " + root);
        return root;
    }

    /** 登记前 / 起局时都过的同一条路径规则（只看字面 + 不在游戏库里；目录在不在由调用方核）。 */
    static void checkInPlaceRoot(String root, File gamesDir) throws BadGame, IOException {
        if (root.length() < 2 || root.charAt(0) != '/' || root.endsWith("/"))
            throw new BadGame("in-place root is not an absolute directory path: " + root);
        for (int i = 0; i < root.length(); i++) {
            char c = root.charAt(i);
            if (c < 0x20 || c == '"' || c == '\\') throw new BadGame("in-place root contains an unsupported character: " + root);
        }
        checkRelative(root.substring(1), "in-place root");
        if (gamesDir != null) {
            File games = gamesDir.getCanonicalFile();
            if (isUnder(new File(root), games) || isUnder(games, new File(root)))
                throw new BadGame("in-place root overlaps the private game library: " + root);
        }
    }

    // Horizon 装载序（tools/module_order.py / runtime/src/loader.c s2a_module_order_ok 同一份规则）：
    // main 第一，subsdk0..N 按数字升序，sdk 最后；名单外的名排在 subsdk* 与 sdk 之间、按名定序。
    // 装载顺序决定全局符号绑定——先装模块的默认可见性定义胜出；字母序（main → sdk → subsdk*）的旧包在此拒收。
    private static long subsdkIndex(String name) {
        if (!name.startsWith("subsdk") || name.length() == 6) return -1;
        for (int i = 6; i < name.length(); i++) if (!Character.isDigit(name.charAt(i))) return -1;
        try {
            return Long.parseLong(name.substring(6));
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static int moduleOrderGroup(String name) {
        if ("main".equals(name)) return 0;
        if (subsdkIndex(name) >= 0) return 1;
        if ("sdk".equals(name)) return 3;
        return 2;
    }

    private static int moduleOrderCompare(String a, String b) {
        int ga = moduleOrderGroup(a), gb = moduleOrderGroup(b);
        if (ga != gb) return Integer.compare(ga, gb);
        if (ga == 1) return Long.compare(subsdkIndex(a), subsdkIndex(b));
        if (ga == 2) return a.compareTo(b);
        return 0;
    }

    // 顺序不对（旧字母序包 main → sdk → subsdk* 等）不拒收：按 Horizon 装载序排好再交给原生装载器（它照这个列表装、
    //   s2a_module_order_ok 只是复核），记日志（不堵门）。没有 main 才是物理上起不来
    private static void orderModules(List<Module> modules, String artifactRel) throws BadGame {
        boolean hasMain = false;
        for (Module m : modules) if ("main".equals(m.name)) hasMain = true;
        if (!hasMain) throw new BadGame(artifactRel + ".modules has no main module");
        List<Module> sorted = new ArrayList<>(modules);
        sorted.sort((x, y) -> moduleOrderCompare(x.name, y.name));
        boolean same = true;
        for (int i = 0; i < sorted.size(); i++) if (sorted.get(i) != modules.get(i)) same = false;
        if (same) return;
        StringBuilder was = new StringBuilder();
        for (Module m : modules) was.append(was.length() == 0 ? "" : ",").append(m.name);
        System.err.println("S2Arm: " + artifactRel + ".modules reordered to Horizon load order (was " + was + ")");
        modules.clear(); modules.addAll(sorted);
    }

    private static Map<String, Object> json(File file, String what) throws BadGame, IOException {
        String text = new String(read(file, MAX_JSON), StandardCharsets.UTF_8);
        try {
            return Json.object(Json.parse(text), what);
        } catch (IllegalArgumentException e) {
            throw new BadGame(what + ": " + e.getMessage());
        }
    }

    /** Returns whether the artifact requires the graphics feature; rejects every other required feature. */
    private static boolean checkCompatibility(Map<String, Object> doc, String what, boolean allowGraphics)
            throws BadGame {
        if (doc.containsKey("extensions")) {
            Map<String, Object> extensions = object(doc, "extensions", what);
            for (Map.Entry<String, Object> entry : extensions.entrySet()) {
                if (!FEATURE_NAME.matcher(entry.getKey()).matches() || !(entry.getValue() instanceof Map))
                    throw new BadGame(what + ".extensions requires namespaced object entries");
            }
        }
        boolean graphics = false;
        if (doc.containsKey("requiredFeatures")) {
            List<Object> features = array(doc, "requiredFeatures", what);
            for (Object feature : features) {
                if (!(feature instanceof String) || !FEATURE_NAME.matcher((String) feature).matches())
                    throw new BadGame(what + ".requiredFeatures contains an invalid feature name");
                // 只有图形描述是已实现的可选能力；其余 required feature 一律拒绝，不猜行为。
                if (allowGraphics && GRAPHICS_FEATURE.equals(feature)) {
                    graphics = true;
                    continue;
                }
                throw new BadGame(what + " requires unsupported feature " + feature);
            }
        }
        return graphics;
    }

    static Map<String, String> fixupHeaders(String text, String what) throws BadGame {
        Map<String, String> out = new LinkedHashMap<>();
        for (String line : text.split("\n")) {
            if (!line.startsWith("# ")) break;             // header ends at the first table row
            int tab = line.indexOf('\t');
            if (tab < 0) continue;
            String key = line.substring(2, tab).trim();
            String value = line.substring(tab + 1).trim();
            if (out.containsKey(key)) throw new BadGame(what + " declares # " + key + " twice");
            out.put(key, value);
        }
        return out;
    }

    private static void checkRelative(String path, String what) throws BadGame {
        int n = path.length();
        if (n == 0 || path.charAt(0) == '/' || path.indexOf('\\') >= 0 || path.indexOf("//") >= 0)
            throw new BadGame(what + " is not a safe relative path: " + path);
        // Single pass over segments; split("/") drops a trailing empty segment, so "a/" stays legal.
        int start = 0;
        for (int i = 0; i <= n; i++) {
            if (i == n || path.charAt(i) == '/') {
                int len = i - start;
                if ((len == 0 && i < n) || (len == 1 && path.charAt(start) == '.')
                        || (len == 2 && path.charAt(start) == '.' && path.charAt(start + 1) == '.'))
                    throw new BadGame(what + " is not a safe relative path: " + path);
                start = i + 1;
            } else if (path.charAt(i) < 0x20) {
                throw new BadGame(what + " contains a control character");
            }
        }
    }

    private static void checkAssetRoot(String path) throws BadGame {
        if (path.startsWith("/") || path.contains("\\") || path.contains("//"))
            throw new BadGame("artifact apk.assetRoot is not a safe relative path: " + path);
        if (path.isEmpty()) return;
        for (String part : path.split("/")) {
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                throw new BadGame("artifact apk.assetRoot is not a safe relative path: " + path);
            for (int i = 0; i < part.length(); i++)
                if (part.charAt(i) < 0x20) throw new BadGame("artifact apk.assetRoot contains a control character");
        }
    }

    /**
     * Canonical roots resolved once for a batch of under() checks on one base: the base itself and
     * the object-store prefix it may link into. Per entry the only filesystem call left is the
     * canonical resolve of that entry, which still detects symlink escapes.
     */
    static final class UnderRoot {
        private final File base;
        private final String objectsPrefix;   // canonical <gameRoot>/objects/ + separator, or null

        private UnderRoot(File base, String objectsPrefix) {
            this.base = base;
            this.objectsPrefix = objectsPrefix;
        }
    }

    static UnderRoot underRoot(File root) throws IOException {
        File base = root.getCanonicalFile();
        String objectsPrefix = null;
        File revisions = base.getParentFile();
        if (revisions != null && "revisions".equals(revisions.getName())) {
            File gameRoot = revisions.getParentFile();
            if (gameRoot != null)
                objectsPrefix = new File(gameRoot, "objects").getCanonicalFile().getPath() + File.separator;
        }
        return new UnderRoot(base, objectsPrefix);
    }

    // Revision files may be relative symlinks into the game's content-addressed
    // object store: <gameRoot>/objects/<aa>/<sha256>. Resolve the link and allow
    // that canonical target; a link to anywhere else is still rejected.
    static File under(File root, String relative, String what) throws BadGame, IOException {
        return under(underRoot(root), relative, what);
    }

    static File under(UnderRoot root, String relative, String what) throws BadGame, IOException {
        checkRelative(relative, what);
        File file = new File(root.base, relative).getCanonicalFile();
        if (isUnder(file, root.base) || isObjectStoreFile(root, file)) return file;
        throw new BadGame(what + " escapes " + root.base);
    }

    // A link target read via readlink+normalize carries no canonicalization. The CAS
    // object-store shape is accepted textually (content stays bound by stat + SHA-256);
    // every other shape falls back to under() so escapes and in-revision targets keep
    // their canonical resolution semantics.
    static File linkTarget(UnderRoot root, java.nio.file.Path linkParent, String relative,
                           java.nio.file.Path link, String what) throws BadGame, IOException {
        java.nio.file.Path target = linkParent.resolve(link).normalize();
        if (isObjectStorePath(root, target.toString())) return target.toFile();
        return under(root, relative, what);
    }

    private static boolean isUnder(File file, File base) {
        return file.getPath().equals(base.getPath()) || file.getPath().startsWith(base.getPath() + File.separator);
    }

    private static boolean isObjectStoreFile(UnderRoot root, File file) {
        return isObjectStorePath(root, file.getPath());
    }

    // Textual form of isObjectStoreFile for a target that was read off the link itself and
    // normalized without canonicalization. The follow-up stat/hash still binds the content.
    static boolean isObjectStorePath(UnderRoot root, String path) {
        if (root.objectsPrefix == null || !path.startsWith(root.objectsPrefix)) return false;
        String rel = path.substring(root.objectsPrefix.length());
        // objects/<2 lowercase hex>/<64 lowercase hex>, exactly two segments.
        if (rel.length() != 2 + 1 + 64 || rel.charAt(2) != File.separatorChar) return false;
        for (int i = 0; i < rel.length(); i++) {
            if (i == 2) continue;
            char c = rel.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    private static File requireFile(File root, String relative, String what) throws BadGame, IOException {
        File f = under(root, relative, what);
        if (!f.isFile()) throw new BadGame(what + " is missing: " + relative);
        return f;
    }

    private static void requireDirectory(File root, String relative, String what) throws BadGame, IOException {
        File f = under(root, relative, what);
        if (!f.isDirectory()) throw new BadGame(what + " is missing: " + relative);
    }

    private static String path(Map<String, Object> map, String key, String what) throws BadGame {
        String value = string(map, key, what);
        checkRelative(value, what + "." + key);
        return value;
    }

    private static String string(Map<String, Object> map, String key, String what) throws BadGame {
        Object value = map.get(key);
        if (!(value instanceof String)) throw new BadGame(what + "." + key + " is missing or not a string");
        return (String) value;
    }

    private static long number(Map<String, Object> map, String key, String what) throws BadGame {
        Object value = map.get(key);
        if (!(value instanceof Long)) throw new BadGame(what + "." + key + " is missing or not an integer");
        return (Long) value;
    }

    private static Map<String, Object> object(Map<String, Object> map, String key, String what) throws BadGame {
        Object value = map.get(key);
        if (!(value instanceof Map)) throw new BadGame(what + "." + key + " is missing or not an object");
        return Json.object(value, what + "." + key);
    }

    private static List<Object> array(Map<String, Object> map, String key, String what) throws BadGame {
        Object value = map.get(key);
        if (!(value instanceof List)) throw new BadGame(what + "." + key + " is missing or not an array");
        return Json.array(value, what + "." + key);
    }

    private static byte[] read(File file, int max) throws IOException {
        long length = file.length();
        if (length > max) throw new IOException(file + " is larger than " + max + " bytes");
        ByteArrayOutputStream out = new ByteArrayOutputStream((int) length);
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[1 << 16];
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
                if (out.size() > max) throw new IOException(file + " is larger than " + max + " bytes");
            }
        }
        return out.toByteArray();
    }

    private static String readText(File file) throws IOException {
        return new String(read(file, 1 << 20), StandardCharsets.UTF_8);
    }

    private static String sha256(File file) throws IOException {
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("SHA-256 is not available", e);
        }
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[1 << 20];
            for (int n; (n = in.read(buffer)) > 0; ) md.update(buffer, 0, n);
        }
        StringBuilder sb = new StringBuilder(64);
        for (byte b : md.digest()) sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }
}
