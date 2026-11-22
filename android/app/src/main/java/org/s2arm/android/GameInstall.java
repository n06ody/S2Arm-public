package org.s2arm.android;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.BitSet;
import java.util.Comparator;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TimeZone;

/**
 * The install transaction for a game package (implementation plan §6.5, A0 contracts): build
 * games/&lt;title id&gt;/revisions/&lt;content root&gt;.staging, validate that revision with BootContract,
 * rename it into place, then switch current.json with a temp file and a rename. A failure or a
 * killed process at any point leaves the previous current.json and its revision as they were;
 * stale .staging directories are swept at startup. Keeps the active revision and the one it
 * replaced (previousContentRoot, for rollback), removes older ones.
 *
 * <p>Revisions share storage: every file is stored once under its SHA-256 in
 * games/&lt;title id&gt;/objects/ after size and SHA verification. Managed revisions expose these
 * objects through relative file links, which survive the staging rename and relocation of the
 * app's data. Generic shader packs require regular files, so those entries keep a hard link or
 * checked copy. A delta from an older plain revision keeps the existing checked-copy transaction.
 * Objects no remaining revision references are collected under the game lock.
 *
 * <p>原地来源（installInPlace）：资源目录已经在共享存储上（玩家拷进去 / 电脑 adb push），内容文件不复制、不进
 * objects/。版本目录只放元数据：manifest.json、index.tsv、构建描述、指纹表和 in-place.json（登记的根）；
 * 登记时按 index 把每个文件的大小与 SHA-256 核一遍（读够索引大小就停），同时记下文件身份写进指纹表，之后起局靠
 * 身份命中、不重算。删版本目录 / 清理对象库都只动私有目录，从不碰原地文件。
 */
final class GameInstall {
    /** Where the package files come from: a directory (adb import/) or a picked document tree. */
    interface Source {
        /** null when the package has no such file. */
        InputStream open(String path) throws IOException;
        /** 这份文件的内容已核过（SHA-256 与大小都等于索引）时，直接改名挪到 target、返回 true，不读不复制；
         *  做不到返回 false，调用方照常复制并核对。直传收进来的文件走这条（ImportReceiver 边写边算过 SHA）。 */
        default boolean adopt(String path, String sha256, long size, File target) throws IOException { return false; }
    }

    interface Progress { void update(long done, long total); }

    /** Creates one hard link. The host test substitutes a failing implementation to exercise copies. */
    interface Linker { void link(File link, File target) throws IOException; }

    private static final Linker HARD_LINK = (link, target) -> Files.createLink(link.toPath(), target.toPath());
    private static final Linker RELATIVE_LINK = (link, target) -> Files.createSymbolicLink(
        link.toPath(), link.getParentFile().toPath().relativize(target.toPath()));
    interface DirectorySync { void sync(File directory) throws IOException; }
    private static volatile DirectorySync directorySync = GameInstall::syncDirectoryOnHost;
    private static volatile Linker objectLink = RELATIVE_LINK;

    static void setDirectorySync(DirectorySync sync) { directorySync = sync; }
    static void setObjectLink(Linker link) { objectLink = link; }

    static final String ARTIFACT = GamePack.ARTIFACT;
    private static final String DELTA = "game-resource-delta.tsv";

    static final class Result {
        final String titleId, contentRoot, previousContentRoot;
        final GamePack.Info info;
        final boolean alreadyActive;
        boolean artifactRefreshed;       // same files, new artifact (e.g. a per-game APK upgrade)
        boolean resourceDelta;
        boolean managedObjects;
        long unchangedReadBytes, unchangedWriteBytes;
        long planMs, placeMs, verifyMs, publishMs, collectMs;
        long storedBytes, sharedBytes;   // copied in this time / already stored (unchanged files)
        boolean linked;                  // revision files are hard links to shared objects (else plain copies)
        String inPlaceRoot;              // 原地来源的根（内容没复制），否则 null
        Result(String titleId, String contentRoot, String previousContentRoot, GamePack.Info info, boolean alreadyActive) {
            this.titleId = titleId; this.contentRoot = contentRoot; this.previousContentRoot = previousContentRoot;
            this.info = info; this.alreadyActive = alreadyActive;
        }
    }

    private GameInstall() { }

    /** Both queues use the same installer. Internal files stay private, including in debug builds. */
    static File[] importDirectories(File internalFiles, File externalFiles) {
        File internal = new File(internalFiles, "import");
        return externalFiles == null || internalFiles.equals(externalFiles)
            ? new File[] {internal} : new File[] {internal, new File(externalFiles, "import")};
    }

    interface ImportListener {
        void installed(File source, Result result);
        void refused(File source, Exception error);
        default void cleanupDeferred(File source, IOException error) { }
    }

    interface ImportSourceCheck {
        Source checked(Source source) throws IOException;
    }

    static Source checkedImportSource(Source source, String installedPackage,
            boolean debuggable, boolean debugEntriesOpen) {
        return new Source() {
            @Override public InputStream open(String path) throws IOException {
                java.io.InputStream input = source.open(path);
                if (input == null || !ARTIFACT.equals(path)) return input;
                byte[] bytes;
                try (java.io.InputStream in = input) { bytes = GamePack.readAll(in); }
                try { BootContract.checkTestImport(bytes, installedPackage, debuggable, debugEntriesOpen); }
                catch (BootContract.BadGame invalid) { throw new IOException(invalid.getMessage(), invalid); }
                return new java.io.ByteArrayInputStream(bytes);
            }
            @Override public boolean adopt(String path, String sha256, long size, File target) throws IOException {
                return !ARTIFACT.equals(path) && source.adopt(path, sha256, size, target);   // 启动描述只走 open（要过准入检查）
            }
        };
    }

    /** One explicit target only; validation, activation and cleanup share the session lease. */
    static void importDirectory(File privateRoot, File[] inboxes, String target,
            Progress progress, ImportListener listener) throws IOException {
        importDirectory(privateRoot, inboxes, target,
            source -> checkedImportSource(source, null, false, false), progress, listener);
    }

    /** Apply admission to the exact artifact stream before installation writes, under the lease. */
    static void importDirectory(File privateRoot, File[] inboxes, String target,
            ImportSourceCheck sourceCheck, Progress progress, ImportListener listener) throws IOException {
        if (target == null) return;
        {   // 不再取整库锁：install 拿到程序号后只锁那一款
            File dir = new File(target);
            try {
                Path path = dir.toPath();
                if (!path.isAbsolute()) throw new IOException("import target must be an absolute path");
                for (Path part : path)
                    if (part.toString().equals("..")) throw new IOException("import target contains ..");
                boolean allowed = false;
                for (File inbox : inboxes) {
                    File canonicalInbox = inbox.getCanonicalFile();
                    if (!Files.isSymbolicLink(inbox.toPath())
                            && canonicalInbox.equals(dir.getCanonicalFile().getParentFile())) {
                        // System aliases above the Context root (for example /sdcard) are valid.
                        // Never let a descendant symlink return to that root and masquerade as
                        // a direct child: the first occurrence of the root must be the parent.
                        boolean nestedAlias = false;
                        for (File ancestor = dir.getParentFile().getParentFile(); ancestor != null;
                                ancestor = ancestor.getParentFile()) {
                            for (File root : inboxes)
                                if (root.getCanonicalFile().equals(ancestor.getCanonicalFile())) nestedAlias = true;
                        }
                        if (!nestedAlias) allowed = true;
                    }
                }
                if (!allowed || Files.isSymbolicLink(path) || Files.isSymbolicLink(path.getParent()))
                    throw new IOException("import target escapes its staging parent or is a symbolic link");
                // A consumed target is a no-op. Incomplete packages wait for manifest-last staging.
                if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return;
                if (!dir.isDirectory()) throw new IOException("import target is not a directory");
                if (!new File(dir, "manifest.json").isFile()) return;
                Result result = install(new File(privateRoot, "games"), sourceCheck.checked(directory(dir)), progress);
                try {
                    Files.walkFileTree(path, new java.nio.file.SimpleFileVisitor<Path>() {
                        @Override public java.nio.file.FileVisitResult visitFile(Path file,
                                java.nio.file.attribute.BasicFileAttributes attrs) throws IOException {
                            Files.delete(file);
                            return java.nio.file.FileVisitResult.CONTINUE;
                        }
                        @Override public java.nio.file.FileVisitResult postVisitDirectory(Path directory,
                                IOException error) throws IOException {
                            if (error != null) throw error;
                            Files.delete(directory);
                            return java.nio.file.FileVisitResult.CONTINUE;
                        }
                    });
                } catch (IOException cleanup) {
                    // Activation has committed. Do not report cleanup as an installation failure.
                    listener.cleanupDeferred(dir, cleanup);
                }
                listener.installed(dir, result);
            } catch (GamePack.BadPack | BootContract.BadGame | IOException | RuntimeException error) {
                listener.refused(dir, error);
            }
        }
    }

    static Source directory(File dir) {
        Path root = dir.toPath().toAbsolutePath().normalize();
        return new Source() {
            @Override public InputStream open(String path) throws IOException {
                Path file = resolve(path);
                return Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS) ? Files.newInputStream(file) : null;
            }
            @Override public boolean adopt(String path, String sha256, long size, File target) throws IOException {
                java.util.Map<String, String> got = ImportReceiver.verified(dir);
                if (got == null || !sha256.equals(got.get(path))) return false;
                Path file = resolve(path);
                if (!Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS) || Files.size(file) != size) return false;
                target.getParentFile().mkdirs();
                Files.move(file, target.toPath(), java.nio.file.StandardCopyOption.ATOMIC_MOVE);
                target.setReadOnly();
                return true;
            }
            private Path resolve(String path) throws IOException {
            Path relative = root.getFileSystem().getPath(path);
            if (relative.isAbsolute()) throw new IOException("package path escapes its directory");
            for (Path segment : relative)
                if (segment.toString().equals("..")) throw new IOException("package path escapes its directory");
            Path file = root.resolve(relative).normalize();
            if (!file.startsWith(root) || file.equals(root))
                throw new IOException("package path escapes its directory");
            // Source packages contain regular files. Installed revisions may still use the
            // installer's checked object-store links; those are validated separately by A0.
            Path part = root;
            if (Files.isSymbolicLink(part)) throw new IOException("package directory is a symbolic link");
            for (Path segment : root.relativize(file)) {
                part = part.resolve(segment);
                if (Files.isSymbolicLink(part)) throw new IOException("package source contains a symbolic link");
            }
            return file;
            }
        };
    }

    static Result install(File gamesDir, Source source, Progress progress)
            throws GamePack.BadPack, BootContract.BadGame, IOException {
        return install(gamesDir, source, progress, HARD_LINK);
    }

    static Result install(File gamesDir, Source source, Progress progress, Linker linker)
            throws GamePack.BadPack, BootContract.BadGame, IOException {
        File library = libraryDirectory(gamesDir);
        byte[] manifest = required(source, "manifest.json");
        byte[] index = required(source, "index.tsv");
        byte[] artifact;
        try (InputStream in = source.open(ARTIFACT)) {
            if (in == null) throw new GamePack.BadPack("package has no " + ARTIFACT + " (build it with the current tools/make_pack.py)");
            artifact = GamePack.readAll(in);
        }
        List<GamePack.Entry> entries = GamePack.readIndex(manifest, index);
        String contentRoot = "sha256-" + GamePack.sha256(index);

        // The title id names the directory, so read control.nacp first (checked against the index like every file).
        GamePack.Info info = new GamePack.Info();
        GamePack.describe(info, verified(source, entry(entries, "ctl/control.nacp")));
        // 只锁这一款（不堵门）：别的游戏在跑不挡本款导入；本款正在跑 / 正在装才报忙
        try (GameStorage lease = GameStorage.acquireIn(library, info.titleId)) {
            return installLocked(library, source, progress, linker, manifest, index, artifact, entries, contentRoot, info);
        }
    }

    private static File libraryDirectory(File gamesDir) throws IOException {
        File absolute = gamesDir.getAbsoluteFile();
        File parent = absolute.getParentFile();
        if (parent == null) throw new IOException("game library has no private parent");
        // The supplied private parent can have a system alias. Managed paths under it
        // still have to be real directories and leaf links still have to name objects.
        File library = new File(parent.getCanonicalFile(), absolute.getName());
        if (Files.isSymbolicLink(library.toPath())) throw new IOException("games directory is a symbolic link");
        return library;
    }

    private static final class DeltaSource implements Source {
        final Source incoming, base;
        final List<GamePack.Entry> entries;
        final BitSet unchanged;

        private DeltaSource(Source incoming, Source base, List<GamePack.Entry> entries) {
            this.incoming = incoming;
            this.base = base;
            this.entries = new ArrayList<>(entries);
            this.entries.sort(Comparator.comparing(e -> e.path));
            unchanged = new BitSet(entries.size());
        }

        private int find(String path) {
            int lo = 0, hi = entries.size() - 1;
            while (lo <= hi) {
                int mid = (lo + hi) >>> 1;
                int cmp = entries.get(mid).path.compareTo(path);
                if (cmp == 0) return mid;
                if (cmp < 0) lo = mid + 1;
                else hi = mid - 1;
            }
            return -1;
        }

        boolean unchanged(String path) {
            int i = find(path);
            return i >= 0 && unchanged.get(i);
        }

        int unchangedOrdinal(String path) {
            int i = find(path);
            return i >= 0 && unchanged.get(i) ? i : -1;
        }

        @Override public InputStream open(String path) throws IOException {
            InputStream in = incoming.open(path);
            return in == null && unchanged(path) ? base.open(path) : in;
        }

        @Override public boolean adopt(String path, String sha256, long size, File target) throws IOException {
            // The previous revision stays intact. Only adopt incoming bytes.
            return incoming.adopt(path, sha256, size, target);
        }
    }

    private static String[] readDelta(Source incoming, List<GamePack.Entry> entries, String nextRoot)
            throws GamePack.BadPack, IOException {
        // An indexed file is ordinary game content, even if it has the same name as the marker.
        for (GamePack.Entry e : entries)
            if (DELTA.equals(e.path)) return null;
        String marker;
        try (InputStream stream = incoming.open(DELTA)) {
            if (stream == null) return null;
            byte[] bytes = new byte[256];
            int length = 0;
            for (int value; (value = stream.read()) != -1; ) {
                if (length == bytes.length) throw new GamePack.BadPack("game resource delta marker is too long");
                bytes[length++] = (byte) value;
            }
            marker = new String(bytes, 0, length, StandardCharsets.US_ASCII);
        }
        if (!marker.matches("s2a-resource-delta-v1\\tsha256-[0-9a-f]{64}\\t[0-9a-f]{64}\\tsha256-[0-9a-f]{64}\\n"))
            throw new GamePack.BadPack("invalid game resource delta marker");
        String[] fields = marker.substring(0, marker.length() - 1).split("\t");
        if (!fields[3].equals(nextRoot) || fields[1].equals(nextRoot))
            throw new GamePack.BadPack("game resource delta target root does not match");
        return fields;
    }

    private static DeltaSource deltaSource(File gameRoot, BootContract.Current current, Source incoming,
            List<GamePack.Entry> entries, String[] proof) throws GamePack.BadPack, IOException {
        if (current == null || !proof[1].equals(current.contentRoot) || !proof[2].equals(current.artifactSha256))
            throw new GamePack.BadPack("game resource delta base root or binding does not match");
        File previous = new File(gameRoot, current.revisionDir);
        if (inPlace(previous)) throw new GamePack.BadPack("game resource delta requires a private installed base");
        Source base = directory(previous);
        if (!GamePack.sha256(required(base, current.artifact)).equals(current.artifactSha256))
            throw new GamePack.BadPack("game resource delta base artifact has changed");
        DeltaSource delta = new DeltaSource(incoming, base, entries);
        MessageDigest digest = GamePack.digest();
        // Stream the old index instead of holding two potentially large indices in memory.
        try (InputStream stream = base.open("index.tsv")) {
            if (stream == null) throw new GamePack.BadPack("game resource delta base index missing");
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    new DigestInputStream(stream, digest), StandardCharsets.UTF_8))) {
                for (String line; (line = reader.readLine()) != null; ) {
                    if (line.isEmpty()) continue;
                    String[] row = line.split("\t", -1);
                    if (row.length != 3) throw new GamePack.BadPack("invalid game resource delta base index");
                    int ordinal = delta.find(row[0]);
                    if (ordinal < 0) continue;
                    GamePack.Entry entry = delta.entries.get(ordinal);
                    if (Long.parseLong(row[1]) == entry.size && row[2].equals(entry.sha256))
                        delta.unchanged.set(ordinal);
                }
            }
        }
        if (!("sha256-" + GamePack.hex(digest.digest())).equals(current.contentRoot))
            throw new GamePack.BadPack("game resource delta base index has changed");
        for (int i = delta.unchanged.nextClearBit(0); i < delta.entries.size(); i = delta.unchanged.nextClearBit(i + 1)) {
            String path = delta.entries.get(i).path;
            try (InputStream stream = incoming.open(path)) {
                if (stream == null) throw new GamePack.BadPack("game resource delta lacks changed file: " + path);
            }
        }
        return delta;
    }

    private static Result installLocked(File gamesDir, Source source, Progress progress, Linker linker, byte[] manifest, byte[] index,
            byte[] artifact, List<GamePack.Entry> entries, String contentRoot, GamePack.Info info)
            throws GamePack.BadPack, BootContract.BadGame, IOException {
        long start = System.nanoTime();
        File gameRoot = new File(gamesDir, info.titleId);
        File revisions = new File(gameRoot, "revisions");
        File active = new File(revisions, contentRoot), staging = new File(revisions, contentRoot + ".staging");
        BootContract.Current current = readCurrent(gameRoot);
        recoverRetired(gameRoot, current);
        String previous = current == null ? null : current.contentRoot;
        String[] proof = readDelta(source, entries, contentRoot);
        // 同一内容根、但当前是原地登记：这次是复制进私有目录，照常装（换掉原地登记）
        boolean samePrivateRevision = current != null && contentRoot.equals(previous) && active.isDirectory() && !inPlace(active);
        if (samePrivateRevision && GamePack.sha256(artifact).equals(current.artifactSha256)) {
            Result same = new Result(info.titleId, contentRoot, previous, info, true);
            same.resourceDelta = proof != null;
            return same;
        }
        DeltaSource delta = proof == null ? null : deltaSource(gameRoot, current, source, entries, proof);
        if (samePrivateRevision)
            return refreshArtifact(gameRoot, current, artifact, info);
        if (delta != null) source = delta;

        Result result = new Result(info.titleId, contentRoot, contentRoot.equals(previous) ? null : previous, info, false);
        result.resourceDelta = delta != null;
        File objects = new File(gameRoot, "objects");
        // An existing plain revision needs a separate migration. Continue its checked
        // delta path until then. The explicit hard-link injector exercises the legacy path.
        boolean oldManaged = current != null && current.managedObjects;
        boolean managed = linker == HARD_LINK && !contentRoot.equals(previous) && (delta == null || oldManaged);
        if (managed) {
            requireRealDirectory(objects.toPath());
            requireRealDirectory(objects.toPath().resolve("tmp"));
            managed = canLink(objects, objectLink);
            if (!managed && oldManaged) throw new IOException("cannot publish relative object links in this revision");
        }
        result.managedObjects = managed;
        GameFingerprint.Stamp[] reference = null;
        if (managed && delta != null) {
            String oldHash = previous.substring(7);
            reference = GameFingerprint.readSelected(new File(gameRoot, current.revisionDir), delta.entries, oldHash,
                GameFingerprint.currentReference(gameRoot, oldHash), delta::unchangedOrdinal);
        }
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        result.linked = managed || canLink(objects, linker);
        result.planMs = elapsedMs(start);
        GameFingerprint.Reference fingerprint;
        try {
            long total = 0;
            for (GamePack.Entry e : entries) total += e.size;
            long[] done = {0};
            byte[] buf = new byte[1 << 20];
            // 本次放进来、放的同时已核过大小与 SHA-256 的文件（直传收下的 / 复制时边写边核的）：记下它们落盘后的身份，
            //   下面的指纹核对按身份命中、不再把整包重读一遍（大包 16 GB 重读要十几分钟）。只有以前装过、当年只核了大小的共享对象，
            //   和链接被拒后另复制出来的文件，才重算 SHA-256
            GameFingerprint.Stamp[] checked = new GameFingerprint.Stamp[entries.size()];
            Set<File> objectDirs = new HashSet<>();
            long placing = System.nanoTime();
            for (int i = 0; i < entries.size(); i++) {
                GamePack.Entry e = entries.get(i);
                File out = new File(staging, e.path);
                out.getParentFile().mkdirs();
                if (managed) {
                    checked[i] = projectObject(source, delta, e, out, objects, reference, objectDirs,
                        buf, progress, done, total, result);
                    continue;
                }
                if (!result.linked || (delta != null && !delta.unchanged(e.path))) {
                    // With no links, copy from the incoming or verified base source.
                    // A changed delta entry must be verified from this upload even if a CAS object exists.
                    put(source, e, out, new File(objects, "tmp"), buf, progress, done, total);
                    result.storedBytes += e.size;
                    checked[i] = GameFingerprint.stat(out);
                    continue;
                }
                File object = new File(objects, e.sha256.substring(0, 2) + "/" + e.sha256);
                boolean fresh = false;
                if (object.isFile() && object.length() == e.size) {   // stored by an earlier install, checked then
                    result.sharedBytes += e.size;
                    done[0] += e.size;
                    if (progress != null) progress.update(done[0], total);
                } else {
                    put(source, e, object, new File(objects, "tmp"), buf, progress, done, total);
                    result.storedBytes += e.size;
                    fresh = true;
                }
                try {
                    linker.link(out, object);
                    if (fresh) checked[i] = GameFingerprint.stat(out);
                } catch (IOException | UnsupportedOperationException | SecurityException linkFailure) {
                    // This path refuses a hard link (e.g. a SELinux label) although the probe
                    // worked: keep the install usable by copying the already verified object.
                    copyObject(object, out);
                }
            }
            result.placeMs = elapsedMs(placing);
            long verifying = System.nanoTime();
            writeSynced(new File(staging, "manifest.json"), manifest);
            writeSynced(new File(staging, "index.tsv"), index);
            writeSynced(new File(staging, ARTIFACT), artifact);
            BootContract.validateRevision(gameRoot, "revisions/" + contentRoot + ".staging", contentRoot, ARTIFACT, GamePack.sha256(artifact));
            // Existing CAS objects were formerly only size-checked. Verify the actual
            // installed bytes before a fingerprint can attest to any of them; files checked
            // while they were placed above are matched by their stamp instead of re-read.
            GameFingerprint.Verification verified = GameFingerprint.verifyPlaced(staging, entries, java.util.Arrays.asList(checked));
            fingerprint = GameFingerprint.write(staging, entries, contentRoot.substring(7), verified.stamps);
            if (managed && delta != null) {
                for (int i = verified.hashed.nextSetBit(0); i >= 0; i = verified.hashed.nextSetBit(i + 1))
                    if (delta.unchanged(entries.get(i).path)) result.unchangedReadBytes += entries.get(i).size;
            }
            result.verifyMs = elapsedMs(verifying);
            long publishing = System.nanoTime();
            if (managed) publishObjectRevision(staging, active, objects, objectDirs);
            else {
                deleteTree(active);
                if (!staging.renameTo(active)) throw new IOException("cannot move the staged revision into place");
            }
            result.publishMs = elapsedMs(publishing);
        } catch (GamePack.BadPack | BootContract.BadGame | IOException | RuntimeException e) {
            deleteTree(staging);
            throw e;
        }
        long activation = System.nanoTime();
        writeCurrent(gameRoot, info.titleId, contentRoot, ARTIFACT, GamePack.sha256(artifact), result.previousContentRoot,
            fingerprint, managed);
        BootContract.open(gameRoot);   // what the runtime will read
        result.publishMs += elapsedMs(activation);
        long collecting = System.nanoTime();
        prune(gameRoot, contentRoot, result.previousContentRoot);
        result.collectMs = elapsedMs(collecting);
        return result;
    }

    private static long elapsedMs(long start) { return (System.nanoTime() - start) / 1_000_000; }

    private static void requireRealDirectory(Path dir) throws IOException {
        if (Files.exists(dir, LinkOption.NOFOLLOW_LINKS) && !Files.isDirectory(dir, LinkOption.NOFOLLOW_LINKS))
            throw new IOException("object store component is not a directory: " + dir.getFileName());
        Files.createDirectories(dir);
    }

    static void syncDirectoryOnHost(File directory) throws IOException {
        try (FileChannel channel = FileChannel.open(directory.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            channel.force(true);
        }
    }

    private static GameFingerprint.Stamp projectObject(Source source, DeltaSource delta, GamePack.Entry e, File out,
            File objects, GameFingerprint.Stamp[] reference, Set<File> objectDirs,
            byte[] buffer, Progress progress, long[] done, long total, Result result)
            throws IOException, GamePack.BadPack {
        File parent = new File(objects, e.sha256.substring(0, 2));
        boolean firstInBucket = objectDirs.add(parent);
        if (firstInBucket) requireRealDirectory(parent.toPath());
        File object = new File(parent, e.sha256);
        int prior = delta == null ? -1 : delta.unchangedOrdinal(e.path);
        boolean exists = Files.exists(object.toPath(), LinkOption.NOFOLLOW_LINKS);
        GameFingerprint.Stamp checked = null;
        if (exists) {
            if (!Files.isRegularFile(object.toPath(), LinkOption.NOFOLLOW_LINKS) || object.length() != e.size)
                throw new IOException("existing object fails size / SHA-256: " + e.path);
            if (prior >= 0 && reference != null) checked = reference[prior];
            if (delta != null && prior < 0) {
                // The delta promised the changed bytes on the wire. Verify them even
                // when an older revision already happens to have the same object.
                File incoming = Files.createTempFile(objects.toPath().resolve("tmp"), "upload", null).toFile();
                try {
                    put(source, e, incoming, new File(objects, "tmp"), buffer, progress, done, total);
                } finally {
                    Files.deleteIfExists(incoming.toPath());
                }
            } else {
                done[0] += e.size;
                if (progress != null) progress.update(done[0], total);
            }
            result.sharedBytes += e.size;
        } else {
            if (prior >= 0) throw new IOException("managed delta object is missing: " + e.path);
            put(source, e, object, new File(objects, "tmp"), buffer, progress, done, total);
            // adopt() can have renamed a fully hashed but still unflushed receive file.
            try (FileChannel channel = FileChannel.open(object.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
                channel.force(true);
            }
            checked = GameFingerprint.stat(object);
            result.storedBytes += e.size;
        }
        if (e.path.startsWith("shaders/vulkan/generic/")) {
            // The generic pack validator requires every component to be free of symlinks.
            try {
                HARD_LINK.link(out, object);
            } catch (IOException | UnsupportedOperationException | SecurityException denied) {
                store(path -> Files.newInputStream(object.toPath(), LinkOption.NOFOLLOW_LINKS),
                    e, out, new File(objects, "tmp"), buffer, null, new long[]{0}, total);
                if (exists) result.sharedBytes -= e.size;
                result.storedBytes += e.size;
                if (prior >= 0) {
                    result.unchangedReadBytes += e.size;
                    result.unchangedWriteBytes += e.size;
                }
                return GameFingerprint.stat(out);
            }
        } else {
            objectLink.link(out, object);
        }
        return checked;
    }

    private static void publishObjectRevision(File staging, File active, File objects, Set<File> buckets)
            throws IOException {
        for (File bucket : buckets) directorySync.sync(bucket);
        directorySync.sync(objects);
        Files.walkFileTree(staging.toPath(), new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult postVisitDirectory(Path dir, IOException error) throws IOException {
                if (error != null) throw error;
                directorySync.sync(dir.toFile());
                return FileVisitResult.CONTINUE;
            }
        });
        File revisions = active.getParentFile(), gameRoot = revisions.getParentFile();
        // A retained target can be replaced only after its old name is recoverable.
        if (Files.exists(active.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            if (!Files.isDirectory(active.toPath(), LinkOption.NOFOLLOW_LINKS))
                throw new IOException("retained revision is not a directory");
            Files.move(active.toPath(), new File(revisions, ".retired-" + active.getName()).toPath(),
                StandardCopyOption.ATOMIC_MOVE);
            directorySync.sync(revisions);
        }
        Files.move(staging.toPath(), active.toPath(), StandardCopyOption.ATOMIC_MOVE);
        directorySync.sync(revisions);
        directorySync.sync(gameRoot);
        directorySync.sync(gameRoot.getParentFile());
        File privateRoot = gameRoot.getParentFile().getParentFile();
        if (privateRoot != null) directorySync.sync(privateRoot);
    }

    private static void recoverRetired(File gameRoot, BootContract.Current current) throws IOException {
        File revisions = new File(gameRoot, "revisions");
        if (Files.isSymbolicLink(gameRoot.toPath()) || Files.isSymbolicLink(revisions.toPath()))
            throw new IOException("object revision directory is a symbolic link");
        File[] retired = revisions.listFiles(f -> f.getName().matches("\\.retired-sha256-[0-9a-f]{64}"));
        if (retired == null) return;
        if (retired.length > 0 && current == null
                && Files.exists(new File(gameRoot, "current.json").toPath(), LinkOption.NOFOLLOW_LINKS))
            throw new IOException("cannot recover a retired revision with an unreadable current pointer");
        for (File saved : retired) {
            if (!Files.isDirectory(saved.toPath(), LinkOption.NOFOLLOW_LINKS))
                throw new IOException("retired object revision is not a directory");
            String root = saved.getName().substring(".retired-".length());
            if (current != null && root.equals(current.contentRoot)) {
                directorySync.sync(gameRoot);
                deleteTree(saved);
            } else {
                File target = new File(revisions, root);
                deleteTree(target);
                Files.move(saved.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
            }
            directorySync.sync(revisions);
        }
    }

    /**
     * 原地登记一个共享存储上的资源目录：读它的 manifest / index / 构建描述，按 index 核每个文件的大小与 SHA-256（读够就停），
     * 只把元数据和指纹表写进私有版本目录，内容文件留在原处。root 由玩家选定（文件夹选择器）或电脑经 adb 指定，
     * 登记的是它的 canonical 路径；之后起局只认这个根之下的文件。
     */
    static Result installInPlace(File gamesDir, File root, ImportSourceCheck check, Progress progress)
            throws GamePack.BadPack, BootContract.BadGame, IOException {
        File library = libraryDirectory(gamesDir);
        if (!root.isAbsolute()) throw new IOException("in-place root must be an absolute path: " + root);
        if (!root.isDirectory()) throw new IOException("in-place game folder is missing or unreadable: " + root);
        File canonical = root.getCanonicalFile();
        BootContract.checkInPlaceRoot(canonical.getPath(), library);
        Source source = check.checked(directory(canonical));
        byte[] manifest = required(source, "manifest.json");
        byte[] index = required(source, "index.tsv");
        byte[] artifact = required(source, ARTIFACT);
        List<GamePack.Entry> entries = GamePack.readIndex(manifest, index);
        String contentRoot = "sha256-" + GamePack.sha256(index);
        GamePack.Info info = new GamePack.Info();
        GamePack.describe(info, verified(source, entry(entries, "ctl/control.nacp")));
        try (GameStorage lease = GameStorage.acquireIn(library, info.titleId)) {
            return installInPlaceLocked(library, canonical, progress, manifest, index, artifact, entries, contentRoot, info);
        }
    }

    private static Result installInPlaceLocked(File gamesDir, File root, Progress progress, byte[] manifest, byte[] index,
            byte[] artifact, List<GamePack.Entry> entries, String contentRoot, GamePack.Info info)
            throws GamePack.BadPack, BootContract.BadGame, IOException {
        File gameRoot = new File(gamesDir, info.titleId);
        File revisions = new File(gameRoot, "revisions");
        File active = new File(revisions, contentRoot), staging = new File(revisions, contentRoot + ".staging");
        BootContract.Current current = readCurrent(gameRoot);
        recoverRetired(gameRoot, current);
        String previous = current == null ? null : current.contentRoot;
        String artifactSha = GamePack.sha256(artifact);
        if (current != null && contentRoot.equals(previous) && active.isDirectory()
                && root.getPath().equals(inPlaceRoot(active))) {
            // 同一个根、同一份内容：文件核对交给起局（指纹命中 / 变了的重算）；构建描述变了照常换
            Result same = artifactSha.equals(current.artifactSha256)
                ? new Result(info.titleId, contentRoot, previous, info, true) : refreshArtifact(gameRoot, current, artifact, info);
            same.inPlaceRoot = root.getPath();
            return same;
        }
        Result result = new Result(info.titleId, contentRoot, contentRoot.equals(previous) ? null : previous, info, false);
        result.inPlaceRoot = root.getPath();
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        GameFingerprint.Reference fingerprint;
        try {
            long sum = 0;
            for (GamePack.Entry e : entries) sum += e.size;
            final long total = sum;
            long[] done = {0};
            // 首次登记：逐个文件按索引核大小与 SHA-256（只读不复制），前后两次 stat 身份一致才记进指纹表
            GameFingerprint.Stamp[] checked = new GameFingerprint.Stamp[entries.size()];
            for (int i = 0; i < entries.size(); i++) {
                GamePack.Entry e = entries.get(i);
                File file = BootContract.under(root, e.path, "in-place file");
                if (!file.isFile()) throw new GamePack.BadPack("in-place file is missing: " + e.path + " (under " + root + ")");
                GameFingerprint.Stamp before = GameFingerprint.stat(file);
                if ((before != null ? before.size : file.length()) != e.size)
                    throw new GamePack.BadPack("in-place file does not match index.tsv (size): " + e.path);
                String sha = GameFingerprint.sha256Exact(file, e.size, n -> {
                    done[0] += n;
                    if (progress != null) progress.update(done[0], total);
                });
                if (!e.sha256.equals(sha))
                    throw new GamePack.BadPack("in-place file does not match index.tsv (SHA-256): " + e.path);
                GameFingerprint.Stamp after = GameFingerprint.stat(file);
                if (before != null && !before.same(after)) throw new IOException("in-place file changed while hashing: " + e.path);
                checked[i] = after;
            }
            writeSynced(new File(staging, "manifest.json"), manifest);
            writeSynced(new File(staging, "index.tsv"), index);
            writeSynced(new File(staging, ARTIFACT), artifact);
            // 路径已过 checkInPlaceRoot（无引号 / 反斜杠 / 控制字符），原样放进 JSON 字符串
            writeSynced(new File(staging, BootContract.IN_PLACE),
                ("{\"version\": 1, \"root\": \"" + root.getPath() + "\"}\n").getBytes(StandardCharsets.UTF_8));
            BootContract.validateRevision(gameRoot, "revisions/" + contentRoot + ".staging", contentRoot, ARTIFACT, artifactSha);
            GameFingerprint.Verification verified = GameFingerprint.verifyPlaced(root, true, entries, java.util.Arrays.asList(checked));
            fingerprint = GameFingerprint.write(staging, entries, contentRoot.substring(7), verified.stamps);
            deleteTree(active);   // 同内容根的旧版本（复制进来的那份 / 旧登记）：换成这次的原地登记
            if (!staging.renameTo(active)) throw new IOException("cannot move the staged revision into place");
        } catch (GamePack.BadPack | BootContract.BadGame | IOException | RuntimeException e) {
            deleteTree(staging);
            throw e;
        }
        String kept = result.previousContentRoot;
        writeCurrent(gameRoot, info.titleId, contentRoot, ARTIFACT, artifactSha, kept, fingerprint, false);
        BootContract.open(gameRoot);
        prune(gameRoot, contentRoot, result.previousContentRoot);
        return result;
    }

    /** 这个版本目录是原地登记（内容不在本目录）。 */
    static boolean inPlace(File revision) {
        return Files.exists(new File(revision, BootContract.IN_PLACE).toPath(), LinkOption.NOFOLLOW_LINKS);
    }

    private static String inPlaceRoot(File revision) {
        try { return BootContract.inPlaceRoot(revision, null); }
        catch (BootContract.BadGame | IOException e) { return null; }
    }

    // 已核过的直接挪（adopt），否则复制并核对（store）
    private static void put(Source source, GamePack.Entry e, File target, File tmpDir, byte[] buf,
                            Progress progress, long[] done, long total) throws IOException, GamePack.BadPack {
        if (source.adopt(e.path, e.sha256, e.size, target)) {
            done[0] += e.size;
            if (progress != null) progress.update(done[0], total);
            return;
        }
        store(source, e, target, tmpDir, buf, progress, done, total);
    }

    // One package file into the object store: copied with its size and SHA-256 checked, flushed,
    // renamed into place and made read-only (objects are never changed, only linked and removed).
    private static void store(Source source, GamePack.Entry e, File object, File tmpDir, byte[] buf,
                              Progress progress, long[] done, long total) throws IOException, GamePack.BadPack {
        tmpDir.mkdirs();
        File tmp = File.createTempFile("object", null, tmpDir);
        try {
            MessageDigest md = GamePack.digest();
            long size = 0;
            try (InputStream in = source.open(e.path); FileOutputStream os = new FileOutputStream(tmp)) {
                if (in == null) throw new GamePack.BadPack("package has no " + e.path);
                // 按索引里的大小读够就停，不再为确认文件末尾多读一次：vivo 上 App 经共享存储（FUSE）读 1.7 GB 的大文件，
                //   读完全部字节后那次「到末尾」的读曾一直不返回（11-13 一款 Unity 游戏导入超时）。存进去的就是索引那么长、按索引核过大小与 SHA-256 的内容；
                //   源文件比索引长出来的部分不读（存的内容不受影响），短了照样报包坏（下面校验）
                for (int n; size < e.size && (n = in.read(buf, 0, (int) Math.min(buf.length, e.size - size))) > 0; ) {
                    md.update(buf, 0, n); os.write(buf, 0, n); size += n; done[0] += n;
                    if (progress != null) progress.update(done[0], total);
                }
                os.getFD().sync();
            }
            if (size != e.size || !GamePack.hex(md.digest()).equals(e.sha256))
                throw new GamePack.BadPack(e.path + " does not match index.tsv (size / SHA-256)");
            object.getParentFile().mkdirs();
            if (!tmp.renameTo(object)) throw new IOException("cannot store " + e.path);
            object.setReadOnly();
        } finally {
            tmp.delete();   // gone already after the rename
        }
    }

    // A hard link refused for one file after the probe succeeded: copy the stored object into the
    // revision. The object was verified when it was stored, so its bytes are already trusted.
    private static void copyObject(File object, File out) throws IOException {
        try (InputStream in = new java.io.FileInputStream(object); FileOutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
            os.getFD().sync();
        }
        out.setReadOnly();
    }

    // Whether this storage lets the app create hard links (probed once per install).
    private static boolean canLink(File objects, Linker linker) {
        File probe = new File(objects, "tmp/link-probe"), target = new File(objects, "tmp/link-target");
        try {
            probe.getParentFile().mkdirs();
            probe.delete();
            target.delete();
            try (FileOutputStream out = new FileOutputStream(target)) { out.write(0); }
            linker.link(probe, target);
            return true;
        } catch (UnsupportedOperationException | SecurityException | IOException e) {
            return false;
        } finally {
            probe.delete();
            target.delete();
        }
    }

    /**
     * Startup: a .staging revision is an install that never finished; nothing points at it.
     * Objects it stored stay until no revision needs them (a retry reuses them), then go too.
     */
    static int sweep(File gamesDir) {
        int removed = 0;
        File[] games = gamesDir.listFiles(File::isDirectory);
        if (games != null) for (File g : games) {
            // 逐款取锁：这一款正在跑 / 正在装就跳过（下次再清），不碰它的半成品
            try (GameStorage lease = GameStorage.acquireIn(gamesDir, g.getName())) {
                recoverRetired(g, readCurrent(g));
                File[] staged = new File(g, "revisions").listFiles(f -> f.getName().endsWith(".staging"));
                if (staged != null) for (File s : staged) { deleteTree(s); removed++; }
                new File(g, "current.json.tmp").delete();
                collect(g);
            } catch (IOException busyOrForeign) {
                System.err.println("S2Arm: library cleanup skipped " + g.getName() + ": " + busyOrForeign.getMessage());
            }
        }
        return removed;
    }

    // The active version, or null (none yet, or a current.json BootContract cannot read).
    private static BootContract.Current readCurrent(File gameRoot) {
        if (!new File(gameRoot, "current.json").isFile()) return null;
        try {
            return BootContract.readCurrent(gameRoot);
        } catch (BootContract.BadGame | IOException e) {
            return null;
        }
    }

    // current.json's previousContentRoot (kept across an artifact refresh), or null.
    private static String previousRoot(File gameRoot) {
        try {
            Object v = Json.object(Json.parse(new String(GamePack.read(new File(gameRoot, "current.json")), StandardCharsets.UTF_8)),
                                   "current.json").get("previousContentRoot");
            return v instanceof String ? (String) v : null;
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /*
     * Same files, new artifact: a per-game APK upgrade changes apk.versionCode / sha256 without
     * touching the resource files, so the content root is the same. The new artifact goes beside
     * the old one under its own name, is validated, and current.json switches to it; the old one
     * is removed after. A failure leaves the old artifact active.
     */
    private static Result refreshArtifact(File gameRoot, BootContract.Current current, byte[] artifact, GamePack.Info info)
            throws BootContract.BadGame, IOException {
        String sha = GamePack.sha256(artifact);
        String name = "game-build-artifact." + sha.substring(0, 16) + ".json";
        File revision = new File(gameRoot, current.revisionDir), file = new File(revision, name);
        writeSynced(file, artifact);
        try {
            BootContract.validateRevision(gameRoot, current.revisionDir, current.contentRoot, name, sha);
        } catch (BootContract.BadGame | IOException | RuntimeException e) {
            file.delete();
            throw e;
        }
        String previous = previousRoot(gameRoot);
        GameFingerprint.Reference fingerprint = GameFingerprint.currentReference(gameRoot, current.contentRoot.substring(7));
        if (current.managedObjects) directorySync.sync(revision);
        writeCurrent(gameRoot, info.titleId, current.contentRoot, name, sha, previous, fingerprint,
            current.managedObjects);
        BootContract.open(gameRoot);
        if (!current.artifact.equals(name)) new File(revision, current.artifact).delete();
        Result r = new Result(info.titleId, current.contentRoot, previous, info, false);
        r.artifactRefreshed = true;
        return r;
    }

    private static void writeCurrent(File gameRoot, String titleId, String contentRoot, String artifactRel, String artifactSha,
                                     String previous, GameFingerprint.Reference fingerprint, boolean managedObjects) throws IOException {
        SimpleDateFormat utc = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
        utc.setTimeZone(TimeZone.getTimeZone("UTC"));
        // Every value is a fixed word, hex or a timestamp: no JSON escaping needed.
        String json = "{\n \"version\": 1,\n \"titleId\": \"" + titleId + "\",\n \"contentRoot\": \"" + contentRoot + "\",\n"
            + " \"revisionDir\": \"revisions/" + contentRoot + "\",\n \"artifact\": \"" + artifactRel + "\",\n"
            + " \"artifactSha256\": \"" + artifactSha + "\",\n \"activatedAt\": \"" + utc.format(new Date()) + "\",\n"
            + " \"previousContentRoot\": " + (previous == null ? "null" : "\"" + previous + "\"")
            + (managedObjects ? ",\n \"extensions\": {\"" + BootContract.OBJECT_STORE_EXTENSION + "\":{}}" : "")
            + (fingerprint == null ? "" : ",\n \"fingerprint\": {\"version\":1,\"path\":\"" + fingerprint.path
                + "\",\"sha256\":\"" + fingerprint.sha256 + "\",\"indexSha256\":\"" + fingerprint.indexSha256 + "\"}") + "\n}\n";
        File tmp = new File(gameRoot, "current.json.tmp");
        if (Files.exists(tmp.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)
                && !Files.isRegularFile(tmp.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS))
            throw new IOException("current temporary file is not regular");
        writeSynced(tmp, json.getBytes(StandardCharsets.UTF_8));
        if (!tmp.renameTo(new File(gameRoot, "current.json"))) throw new IOException("cannot activate current.json");
        if (managedObjects) directorySync.sync(gameRoot);
    }

    // Keep the active revision and the one it replaced; then the objects only removed ones used.
    private static void prune(File gameRoot, String keep, String previous) {
        File[] all = new File(gameRoot, "revisions").listFiles(File::isDirectory);
        if (all != null) for (File r : all)
            if (!r.getName().equals(keep) && !r.getName().equals(previous)) deleteTree(r);
        collect(gameRoot);
    }

    // Removes objects no remaining revision lists (and object temp files). An unreadable index
    // keeps everything: better a leftover object than a revision missing a file.
    private static void collect(File gameRoot) {
        Set<String> used = new HashSet<>();
        // 原地登记的版本目录不占对象：它的 index 不算引用（内容在共享存储上）
        File[] kept = new File(gameRoot, "revisions").listFiles(f -> f.isDirectory() && !f.getName().endsWith(".staging") && !inPlace(f));
        if (kept != null) for (File r : kept) {
            // 逐行读：大包 index 30 万行 / 32 MB，整读成一个 String 再 split 会撑爆 App 的 256 MB 堆（11-14 真机安装收尾 OOM）
            try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(
                    new java.io.FileInputStream(new File(r, "index.tsv")), StandardCharsets.UTF_8), 1 << 16)) {
                for (String line; (line = in.readLine()) != null; ) {
                    String[] col = line.split("\t");
                    if (col.length == 3) used.add(col[2]);
                }
            } catch (IOException e) {
                return;
            }
        }
        File objects = new File(gameRoot, "objects");
        deleteTree(new File(objects, "tmp"));
        File[] buckets = objects.listFiles(File::isDirectory);
        if (buckets != null) for (File b : buckets) {
            File[] files = b.listFiles();
            if (files != null) for (File o : files) if (!used.contains(o.getName())) o.delete();
            b.delete();   // only when empty
        }
    }

    private static GamePack.Entry entry(List<GamePack.Entry> entries, String path) throws GamePack.BadPack {
        for (GamePack.Entry e : entries) if (e.path.equals(path)) return e;
        throw new GamePack.BadPack("package has no " + path);
    }

    private static byte[] verified(Source source, GamePack.Entry e) throws IOException, GamePack.BadPack {
        byte[] data = required(source, e.path);
        if (data.length != e.size || !GamePack.sha256(data).equals(e.sha256))
            throw new GamePack.BadPack(e.path + " does not match index.tsv (size / SHA-256)");
        return data;
    }

    private static byte[] required(Source source, String path) throws IOException, GamePack.BadPack {
        try (InputStream in = source.open(path)) {
            if (in == null) throw new GamePack.BadPack("package has no " + path);
            return GamePack.readAll(in);
        }
    }

    // Written and flushed to storage before the rename that makes it count.
    private static void writeSynced(File f, byte[] data) throws IOException {
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(data);
            out.getFD().sync();
        }
    }

    static void deleteTree(File f) {
        File[] children = Files.isSymbolicLink(f.toPath()) ? null : f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }
}
