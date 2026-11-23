package org.s2arm.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Plain Java checks of the install transaction (GameInstall): first install, re-install, upgrade
 * with the previous revision kept, pruning, rollback, refusals that leave current.json and the
 * revisions untouched, the startup sweep, checked-copy fallback and late activation failure/retry.
 * Optional arg: a tools/make_pack.py package, --transaction-boundaries, or --locked-imports.
 * Queue locking uses real filesystem leases and child JVMs, not Android permission emulation.
 */
public final class GameInstallTest {
    private static final String PACK_KEY = "d739b700f46aaec822fb22988732a943bee11c227006fdca038ab18e1c17cd10";
    private static final String PACK_DIR = "shaders/vulkan/generic/" + PACK_KEY;
    private static final String PACK_MANIFEST = "{\"schema\":\"s2a-vulkan-pack-v1\"}\n";
    private static int failures, assertions;

    private static void check(boolean ok, String what) {
        assertions++;
        if (!ok) { System.err.println("FAILED: " + what); failures++; }
    }

    private static byte[] nacp(long titleId) {
        byte[] n = new byte[0x4000];
        byte[] name = "S2Arm install test".getBytes(StandardCharsets.UTF_8);
        System.arraycopy(name, 0, n, 0, name.length);
        for (int i = 0; i < 8; i++) n[0x3038 + i] = (byte) (titleId >>> (8 * i));
        n[0x3060] = '1';
        return n;
    }

    /** A package as tools/make_pack.py writes it; romfs text varies the content root. */
    private static File pack(String romfs, long artifactTitle, boolean withArtifact) throws IOException {
        return pack(romfs, artifactTitle, withArtifact, java.util.Collections.emptyMap());
    }

    private static File pack(String romfs, long artifactTitle, boolean withArtifact,
                             Map<String, byte[]> extra) throws IOException {
        Map<String, byte[]> files = new LinkedHashMap<>(extra);
        files.put("exefs/main.npdm", new byte[128]);
        files.put("romfs/readme.txt", romfs.getBytes(StandardCharsets.UTF_8));
        files.put("ctl/control.nacp", nacp(0x0100000000ABCDEFL));
        files.put("modules/main.patched.bin", new byte[4096]);
        files.put("modules/main.fixups.tsv", ("# BUILD_ID\t5332412d73656c667465737400000000\n# TARGET\tandroid\n# REWRITE_ABI\t"
            + BootContract.REWRITE_ABI + "\n# PATCH_OFF\t0x10000\n").getBytes(StandardCharsets.UTF_8));
        files.put("modules/main.gates.tsv", "0x100f0\tsvc\t1\t0x128\n".getBytes(StandardCharsets.UTF_8));
        files.put("modules/main.npdm", new byte[128]);
        // Offline shader pack: the validator refuses links at every component of these paths.
        files.put(PACK_DIR + "/manifest.json", PACK_MANIFEST.getBytes(StandardCharsets.UTF_8));
        files.put(PACK_DIR + "/profile.json", "{\"profile\":true}\n".getBytes(StandardCharsets.UTF_8));
        files.put(PACK_DIR + "/shcache/0100000000abcdef/guest.data", new byte[4096]);
        File dir = Files.createTempDirectory("s2a-install-pack").toFile();
        StringBuilder index = new StringBuilder();
        long total = 0;
        for (Map.Entry<String, byte[]> e : files.entrySet()) {
            File f = new File(dir, e.getKey());
            f.getParentFile().mkdirs();
            Files.write(f.toPath(), e.getValue());
            index.append(e.getKey()).append('\t').append(e.getValue().length).append('\t').append(GamePack.sha256(e.getValue())).append('\n');
            total += e.getValue().length;
        }
        byte[] ix = index.toString().getBytes(StandardCharsets.UTF_8);
        String indexSha = GamePack.sha256(ix);
        Files.write(new File(dir, "index.tsv").toPath(), ix);
        if (withArtifact)
            Files.write(new File(dir, GamePack.ARTIFACT).toPath(), String.format(
                "{\n \"version\": 1,\n \"titleId\": \"%016X\",\n \"contentRoot\": \"sha256-%s\",\n \"target\": \"android\",\n"
                + " \"guestAbi\": \"aarch64\",\n \"rewriteAbi\": \"%s\",\n \"runtime\": {\"version\": \"0.1.0\", \"backend\": \"vulkan\"},\n"
                + " \"package\": {\"manifest\": \"manifest.json\", \"indexSha256\": \"%s\", \"files\": %d, \"bytes\": %d},\n"
                + " \"npdm\": \"exefs/main.npdm\",\n \"control\": \"ctl/control.nacp\",\n \"romfs\": \"romfs\",\n"
                + " \"modules\": [{\"name\": \"main\", \"source\": \"revision\", \"image\": \"modules/main.patched.bin\","
                + " \"fixups\": \"modules/main.fixups.tsv\", \"gates\": \"modules/main.gates.tsv\", \"target\": \"android\"}]\n}\n",
                artifactTitle, indexSha, BootContract.REWRITE_ABI, indexSha, files.size(), total).getBytes(StandardCharsets.UTF_8));
        Files.write(new File(dir, "manifest.json").toPath(), String.format(
            "{\n \"version\": 2,\n \"files\": %d,\n \"total\": %d,\n \"index_sha256\": \"%s\"\n}", files.size(), total, indexSha)
            .getBytes(StandardCharsets.UTF_8));
        return dir;
    }

    private static File pack(String romfs) throws IOException { return pack(romfs, 0x0100000000ABCDEFL, true); }

    private static String root(File pack) throws IOException {
        return "sha256-" + GamePack.sha256(Files.readAllBytes(new File(pack, "index.tsv").toPath()));
    }

    private static String[] revisions(File gameRoot) {
        String[] names = new File(gameRoot, "revisions").list();
        if (names == null) return new String[0];
        Arrays.sort(names);
        return names;
    }

    private static String current(File gameRoot) {
        try {
            return BootContract.readCurrent(gameRoot).contentRoot;
        } catch (Exception e) {
            return "unreadable: " + e.getMessage();
        }
    }

    private static void refused(File games, File pack, String why, String expect, String activeBefore) throws IOException {
        File gameRoot = new File(games, "0100000000ABCDEF");
        String[] before = revisions(gameRoot);
        try {
            GameInstall.install(games, GameInstall.directory(pack), null);
            check(false, why + ": installed");
        } catch (GamePack.BadPack | BootContract.BadGame e) {
            check(e.getMessage().contains(expect), why + ": " + e.getMessage());
        }
        check(current(gameRoot).equals(activeBefore), why + ": current.json changed to " + current(gameRoot));
        check(Arrays.equals(revisions(gameRoot), before), why + ": revisions changed to " + Arrays.toString(revisions(gameRoot)));
    }

    private static void importInboxes() throws Exception {
        File base = Files.createTempDirectory("s2a-inboxes").toFile();
        File internal = new File(base, "private"), external = new File(base, "external");
        File games = new File(internal, "games"), game = new File(games, "0100000000ABCDEF");
        File[] queues = GameInstall.importDirectories(internal, external);
        check(queues.length == 2 && queues[0].equals(new File(internal, "import"))
            && queues[1].equals(new File(external, "import")), "private inbox remains selectable with external storage");
        check(GameInstall.importDirectories(internal, null).length == 1, "no external: one private inbox");
        check(GameInstall.importDirectories(internal, internal).length == 1, "identical roots listed once");
        for (File q : queues) q.mkdirs();
        int[] events = new int[2];
        GameInstall.ImportListener listener = new GameInstall.ImportListener() {
            public void installed(File source, GameInstall.Result result) { events[0]++; }
            public void refused(File source, Exception error) { events[1]++; }
        };
        File staged = new File(queues[0], "first");
        check(pack("private first").renameTo(staged), "stage synthetic input");
        File manifest = new File(staged, "manifest.json"), pending = new File(staged, "manifest.pending");
        check(manifest.renameTo(pending), "hold manifest until last");
        GameInstall.importDirectory(internal, queues, staged.getAbsolutePath(), null, listener);
        check(events[0] == 0 && events[1] == 0 && staged.isDirectory() && !new File(game, "current.json").exists(),
            "manifest-last: incomplete input is retained without activation");
        String expected = root(staged);
        check(pending.renameTo(manifest), "commit manifest");
        GameInstall.importDirectory(internal, queues, staged.getAbsolutePath(), null, listener);
        check(events[0] == 1 && events[1] == 0 && !staged.exists() && current(game).equals(expected)
            && BootContract.open(game).contentRoot.equals(expected), "private queue executes real installer and consumes only success");
        byte[] pointer = Files.readAllBytes(new File(game, "current.json").toPath());

        File broken = new File(queues[0], "bad-hash");
        check(pack("different").renameTo(broken), "stage bad hash");
        Files.write(new File(broken, "romfs/readme.txt").toPath(), new byte[] {9});
        GameInstall.importDirectory(internal, queues, broken.getAbsolutePath(), null, listener);
        check(events[1] == 1 && broken.isDirectory()
            && Arrays.equals(pointer, Files.readAllBytes(new File(game, "current.json").toPath())),
            "hash refusal retains source and previous active pointer");
        GameInstall.deleteTree(broken);

        File identity = new File(queues[0], "wrong-title");
        check(pack("wrong identity", 0x0100000000000001L, true).renameTo(identity), "stage wrong title");
        GameInstall.importDirectory(internal, queues, identity.getAbsolutePath(), null, listener);
        check(events[1] == 2 && identity.isDirectory() && current(game).equals(expected), "package identity refusal stays inactive");
        GameInstall.deleteTree(identity);

        File interrupted = new File(queues[0], "interrupted");
        check(pack("interrupted").renameTo(interrupted), "stage interrupted input");
        GameInstall.importDirectory(internal, queues, interrupted.getAbsolutePath(), (done, total) -> { throw new IllegalStateException("synthetic interruption"); }, listener);
        check(events[1] == 3 && interrupted.isDirectory() && current(game).equals(expected)
            && !new File(game, "revisions/" + root(interrupted) + ".staging").exists(), "interruption preserves input/active, removes unfinished revision");
        GameInstall.importDirectory(internal, queues, interrupted.getAbsolutePath(), null, listener);
        check(events[0] == 2 && !interrupted.exists(), "retry after interruption completes");
        expected = current(game);

        File outside = pack("outside");
        byte[] outsideManifest = Files.readAllBytes(new File(outside, "manifest.json").toPath());
        File link = new File(queues[0], "linked-package");
        Files.createSymbolicLink(link.toPath(), outside.toPath());
        GameInstall.importDirectory(internal, queues, link.getAbsolutePath(), null, listener);
        check(events[1] == 4 && outside.isDirectory() && current(game).equals(expected), "package directory symlink refused");
        GameInstall.deleteTree(link);
        check(outside.isDirectory() && Arrays.equals(outsideManifest, Files.readAllBytes(new File(outside, "manifest.json").toPath())),
            "deleting source link never traverses its target");
        File linkedFile = new File(queues[0], "linked-file");
        check(pack("linked-file").renameTo(linkedFile), "stage linked payload");
        File payload = new File(linkedFile, "romfs/readme.txt");
        Files.delete(payload.toPath());
        Files.createSymbolicLink(payload.toPath(), new File(outside, "romfs/readme.txt").toPath());
        GameInstall.importDirectory(internal, queues, linkedFile.getAbsolutePath(), null, listener);
        check(events[1] == 5 && linkedFile.exists() && current(game).equals(expected), "payload symlink refused");
        GameInstall.deleteTree(linkedFile);
        check(new File(outside, "romfs/readme.txt").isFile(), "failure cleanup preserves external payload");

        for (String path : new String[] {"../outside", outside.getAbsolutePath()}) {
            try {
                GameInstall.directory(outside).open(path);
                check(false, "directory source accepted escaping path");
            } catch (IOException ok) { check(ok.getMessage().contains("escapes"), "escaping path reports refusal"); }
        }
        File externalPack = new File(queues[1], "external");
        check(pack("external").renameTo(externalPack), "stage external input");
        GameInstall.importDirectory(internal, queues, externalPack.getAbsolutePath(), null, listener);
        check(events[0] == 3 && !externalPack.exists(), "existing external queue still imports");

        try {
            GameInstall.install(games, path -> { throw new IOException("source-denied"); }, null);
            check(false, "source IOException swallowed");
        } catch (IOException denied) { check("source-denied".equals(denied.getMessage()), "source error remains an error"); }
        File linkedQueue = new File(base, "linked-queue");
        Files.createSymbolicLink(linkedQueue.toPath(), queues[1].toPath());
        GameInstall.importDirectory(internal, queues, new File(linkedQueue, "synthetic").getAbsolutePath(), null, listener);
        check(events[1] == 6, "queue symlink refused");
        GameInstall.deleteTree(base);
        GameInstall.deleteTree(outside);
    }

    /** Real filesystem failures, not Android UID / SELinux / FUSE emulation. */
    private static void transactionBoundaries() throws Exception {
        File base = Files.createTempDirectory("s2a-install-boundaries").toFile();
        try {
            File games = new File(base, "games"), inbox = new File(base, "import");
            File game = new File(games, "0100000000ABCDEF"), objects = new File(game, "objects");
            check(inbox.mkdirs(), "boundaries: create private synthetic inbox");
            int[] events = new int[2];
            GameInstall.Result[] installed = {null};
            Exception[] error = {null};
            GameInstall.ImportListener listener = new GameInstall.ImportListener() {
                public void installed(File source, GameInstall.Result result) { events[0]++; installed[0] = result; }
                public void refused(File source, Exception failure) { events[1]++; error[0] = failure; }
            };

            File first = new File(inbox, "copy-first");
            check(pack("copy first\n").renameTo(first), "boundaries: stage first copy package");
            String r1 = root(first);
            java.util.List<GamePack.Entry> entries = GamePack.readIndex(
                Files.readAllBytes(new File(first, "manifest.json").toPath()),
                Files.readAllBytes(new File(first, "index.tsv").toPath()));
            // A nonempty directory makes the real symlink probe fail, while object temp-file
            // writes remain possible. No mocked installer, permissions or source exceptions.
            File probe = new File(objects, "tmp/link-probe"), marker = new File(probe, "keep");
            check(probe.mkdirs(), "boundaries: block link probe with a real directory");
            Files.write(marker.toPath(), new byte[] {1});
            GameInstall.importDirectory(base, new File[] {inbox}, first.getAbsolutePath(), null, listener);
            check(events[0] == 1 && events[1] == 0 && installed[0] != null && !installed[0].linked,
                "copy fallback: real probe refusal selects checked copies");
            check(!first.exists() && current(game).equals(r1) && BootContract.open(game).contentRoot.equals(r1),
                "copy fallback: validated revision activates and consumes source");
            long bytes = 0;
            for (GamePack.Entry entry : entries) {
                File file = new File(game, "revisions/" + r1 + "/" + entry.path);
                byte[] content = Files.readAllBytes(file.toPath());
                check(Files.isRegularFile(file.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS) && content.length == entry.size
                    && GamePack.sha256(content).equals(entry.sha256), "copy fallback: checked regular file " + entry.path);
                bytes += entry.size;
            }
            check(installed[0].storedBytes == bytes && installed[0].sharedBytes == 0,
                "copy fallback: every payload byte stored, none shared");
            check(objects.isDirectory() && objects.list().length == 0, "copy fallback: no object or temporary files retained");

            byte[] oldCurrent = Files.readAllBytes(new File(game, "current.json").toPath());
            File second = new File(inbox, "copy-update");
            check(pack("copy update\n").renameTo(second), "boundaries: stage a new content root");
            String r2 = root(second);
            File payload = new File(second, "romfs/readme.txt");
            byte[] original = Files.readAllBytes(payload.toPath()), changed = original.clone();
            changed[0] ^= 1;
            Files.write(payload.toPath(), changed);
            check(probe.mkdirs(), "boundaries: block next link probe");
            Files.write(marker.toPath(), new byte[] {1});
            GameInstall.importDirectory(base, new File[] {inbox}, second.getAbsolutePath(), null, listener);
            check(events[0] == 1 && events[1] == 1 && error[0] instanceof GamePack.BadPack
                && error[0].getMessage().contains("does not match index.tsv"), "copy fallback: bad hash is rejected");
            check(second.isDirectory() && Arrays.equals(changed, Files.readAllBytes(payload.toPath()))
                && Arrays.equals(oldCurrent, Files.readAllBytes(new File(game, "current.json").toPath())),
                "copy fallback refusal: source and old current bytes preserved");
            File active = new File(game, "revisions/" + r2), staging = new File(game, "revisions/" + r2 + ".staging");
            check(!active.exists() && !staging.exists(), "copy fallback refusal: no partial revision published");

            Files.write(payload.toPath(), original);
            Map<String, byte[]> sourceBefore = new LinkedHashMap<>();
            for (GamePack.Entry entry : entries)
                sourceBefore.put(entry.path, Files.readAllBytes(new File(second, entry.path).toPath()));
            for (String path : new String[] {"manifest.json", "index.tsv", GamePack.ARTIFACT})
                sourceBefore.put(path, Files.readAllBytes(new File(second, path).toPath()));
            // Fail after validateRevision and staging.renameTo(active), at writeCurrent's real
            // non-regular temporary-pointer guard. Unlike mid-copy interruption, the revision is complete.
            File pointerBlock = new File(game, "current.json.tmp");
            check(pointerBlock.mkdir(), "boundaries: block activation pointer write");
            Files.write(new File(pointerBlock, "keep").toPath(), new byte[] {1});
            GameInstall.importDirectory(base, new File[] {inbox}, second.getAbsolutePath(), null, listener);
            check(events[0] == 1 && events[1] == 2 && error[0] instanceof IOException
                && "current temporary file is not regular".equals(error[0].getMessage()),
                "late activation refusal: reports the non-regular pointer guard");
            check(active.isDirectory() && !staging.exists(), "late activation refusal: staged revision was renamed before failure");
            BootContract.validateRevision(game, "revisions/" + r2, r2, GamePack.ARTIFACT,
                GamePack.sha256(Files.readAllBytes(new File(second, GamePack.ARTIFACT).toPath())));
            check(Arrays.equals(oldCurrent, Files.readAllBytes(new File(game, "current.json").toPath()))
                && BootContract.open(game).contentRoot.equals(r1), "late activation refusal: old current bytes and runtime selection preserved");
            boolean sourceUnchanged = second.isDirectory();
            for (Map.Entry<String, byte[]> entry : sourceBefore.entrySet())
                sourceUnchanged &= Arrays.equals(entry.getValue(), Files.readAllBytes(new File(second, entry.getKey()).toPath()));
            check(sourceUnchanged, "late activation refusal: all source resource and metadata bytes retained for retry");

            GameInstall.deleteTree(pointerBlock); // Remove only this test's deliberate failure.
            GameInstall.importDirectory(base, new File[] {inbox}, second.getAbsolutePath(), null, listener);
            check(events[0] == 2 && events[1] == 2 && !second.exists(), "late activation retry: success alone consumes source");
            check(current(game).equals(r2) && BootContract.open(game).contentRoot.equals(r2)
                && r1.equals(installed[0].previousContentRoot) && !installed[0].linked,
                "late activation retry: new copied revision active, previous root recorded");
            String pointer = Files.readString(new File(game, "current.json").toPath());
            check(pointer.contains("\"previousContentRoot\": \"" + r1 + "\"")
                && new File(game, "revisions/" + r1).isDirectory() && active.isDirectory()
                && !staging.exists() && !pointerBlock.exists(), "late activation retry: previous revision kept, temporary state cleaned");
        } finally {
            GameInstall.deleteTree(base);
        }
    }

    private static Map<String, String> snapshot(File root) throws IOException {
        Map<String, String> hashes = new LinkedHashMap<>();
        try (var paths = Files.walk(root.toPath())) {
            for (var path : paths.filter(Files::isRegularFile).sorted().toList())
                hashes.put(root.toPath().relativize(path).toString(), GamePack.sha256(Files.readAllBytes(path)));
        }
        return hashes;
    }

    private static void expectNoopImportProcess(File internal, File external) throws Exception {
        Process child = new ProcessBuilder(new File(System.getProperty("java.home"), "bin/java").getPath(),
            "-cp", System.getProperty("java.class.path"), GameInstallTest.class.getName(),
            "--locked-import-child", internal.getPath(), external.getPath()).redirectErrorStream(true).start();
        if (!child.waitFor(15, java.util.concurrent.TimeUnit.SECONDS)) {
            child.destroyForcibly();
            throw new AssertionError("competing import process did not exit within 15 seconds");
        }
        String output = new String(child.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        check(child.exitValue() == 0 && output.lines().anyMatch("SCANNED"::equals)
            && output.lines().noneMatch(l -> l.startsWith("INSTALLED") || l.startsWith("REFUSED")),
            "locked queues: a consumed target is a no-op for a competing real JVM import, got " + child.exitValue() + " " + output);
    }

    private static void expectBusyImportProcess(File internal, File external) throws Exception {
        Process child = new ProcessBuilder(new File(System.getProperty("java.home"), "bin/java").getPath(),
            "-cp", System.getProperty("java.class.path"), GameInstallTest.class.getName(),
            "--locked-import-child", internal.getPath(), external.getPath()).redirectErrorStream(true).start();
        if (!child.waitFor(15, java.util.concurrent.TimeUnit.SECONDS)) {
            child.destroyForcibly();
            throw new AssertionError("competing import process did not exit within 15 seconds");
        }
        String output = new String(child.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        check(child.exitValue() == 2 && output.lines().anyMatch("BUSY"::equals),
            "locked queues: competing real JVM import must report BUSY, got " + child.exitValue() + " " + output);
    }

    /** Execute the product single-target entry with actual OS locks and two independently staged inputs. */
    private static void lockedImports() throws Exception {
        File base = Files.createTempDirectory("s2a-locked-imports").toFile();
        File initial = null;
        try {
            File internal = new File(base, "private"), external = new File(base, "external");
            File games = new File(internal, "games"), game = new File(games, "0100000000ABCDEF");
            File[] queues = GameInstall.importDirectories(internal, external);
            for (File queue : queues) check(queue.mkdirs(), "locked queues: create synthetic inbox " + queue.getName());
            initial = pack("locked old\n");
            String oldRoot = root(initial);
            GameInstall.install(games, GameInstall.directory(initial), null);
            File privateStage = new File(queues[0], "private-update"), externalStage = new File(queues[1], "external-update");
            check(pack("locked private\n").renameTo(privateStage), "locked queues: stage private update");
            check(pack("locked external\n").renameTo(externalStage), "locked queues: stage external update");
            String privateRoot = root(privateStage), externalRoot = root(externalStage);
            Map<String, String> privateBefore = snapshot(privateStage), externalBefore = snapshot(externalStage);
            Map<String, String> gameBefore = snapshot(game);
            int[] events = new int[2]; String[] refusal = {""};
            java.util.List<String> activated = new java.util.ArrayList<>();
            GameInstall.ImportListener listener = new GameInstall.ImportListener() {
                public void installed(File source, GameInstall.Result result) {
                    events[0]++;
                    activated.add(result.contentRoot);
                    check(!source.exists(), "locked queues: successful source cleanup precedes callback");
                    check(result.contentRoot.equals(current(game)), "locked queues: callback observes actual activation");
                    // 锁按款、只覆盖 install 本身（不堵门）：回调时锁已放、源包已消费，并发的子进程找不到目标、什么都不装、正常退出
                    try {
                        expectNoopImportProcess(internal, external);
                    } catch (Exception error) { throw new AssertionError("child import probe failed", error); }
                    if (events[0] == 1) {
                        check(result.contentRoot.equals(privateRoot) && oldRoot.equals(result.previousContentRoot)
                            && externalStage.isDirectory(), "locked queues: private queue commits first while external remains staged");
                    } else {
                        check(result.contentRoot.equals(externalRoot) && privateRoot.equals(result.previousContentRoot),
                            "locked queues: external queue commits after private under its own explicit-target lease");
                    }
                }
                public void refused(File source, Exception error) { events[1]++; refusal[0] = String.valueOf(error.getMessage()); }
            };
            // 锁按游戏分（不堵门）：这一款被占着时，导入走到读出程序号后才报忙，经「拒收」回调告诉调用方，源包与版本都不动
            try (GameStorage storage = GameStorage.acquire(internal, game.getName())) {
                GameInstall.importDirectory(internal, queues, privateStage.getAbsolutePath(), null, listener);
                check(events[1] == 1 && refusal[0].contains("busy"), "locked queues: same-game contention is refused as busy");
                expectBusyImportProcess(internal, external);
                check(events[0] == 0, "locked queues: busy game installs nothing");
                events[1] = 0;
                check(privateBefore.equals(snapshot(privateStage)) && externalBefore.equals(snapshot(externalStage)),
                    "locked queues: same and cross-JVM refusal preserve both complete source trees");
                check(gameBefore.equals(snapshot(game)) && oldRoot.equals(current(game)),
                    "locked queues: contention leaves old current and all revision bytes unchanged");
            }
            GameInstall.importDirectory(internal, queues, privateStage.getAbsolutePath(), null, listener);
            check(externalStage.isDirectory(), "explicit private target leaves external input untouched");
            GameInstall.importDirectory(internal, queues, externalStage.getAbsolutePath(), null, listener);
            check(events[0] == 2 && events[1] == 0 && activated.equals(Arrays.asList(privateRoot, externalRoot)),
                "locked queues: after release both sources activate in private/external order");
            check(!privateStage.exists() && !externalStage.exists() && externalRoot.equals(current(game)),
                "locked queues: both successful stages consumed and external revision current");
            check(new File(game, "revisions/" + privateRoot).isDirectory()
                && !new File(game, "revisions/" + oldRoot).exists(), "locked queues: previous retained and older revision pruned");
            try (GameStorage storage = GameStorage.acquire(internal, game.getName())) {
                check(true, "locked queues: successful import releases the lease");
            }

            File bad = new File(queues[0], "bad-hash");
            check(pack("locked corrupt\n").renameTo(bad), "locked queues: stage refused input");
            Files.write(new File(bad, "romfs/readme.txt").toPath(), new byte[] {1});
            byte[] pointer = Files.readAllBytes(new File(game, "current.json").toPath());
            GameInstall.importDirectory(internal, queues, bad.getAbsolutePath(), null, listener);
            check(events[0] == 2 && events[1] == 1 && bad.isDirectory()
                && Arrays.equals(pointer, Files.readAllBytes(new File(game, "current.json").toPath())),
                "locked queues: package refusal preserves active pointer and source");
            try (GameStorage storage = GameStorage.acquire(internal, game.getName())) {
                check(true, "locked queues: refused input also releases the lease");
            }
        } finally {
            if (initial != null) GameInstall.deleteTree(initial);
            GameInstall.deleteTree(base);
        }
    }

    // 本次放进来的文件放时已核过、不再重读；以前装过的共享对象当年只核了大小，新版本复用时照旧重算：
    //   同样大小的坏内容被拒，current 不动
    private static void sharedObjectRehashed() throws Exception {
        File games = Files.createTempDirectory("s2a-install-shared").toFile();
        File gameRoot = new File(games, "0100000000ABCDEF");
        GameInstall.Result first = GameInstall.install(games, GameInstall.directory(pack("one\n")), null);
        check(first.linked, "shared store in use (hard links work here)");
        String sha = GamePack.sha256(new byte[4096]);
        File object = new File(gameRoot, "objects/" + sha.substring(0, 2) + "/" + sha);
        check(object.isFile() && object.setWritable(true), "shared object present");
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(object, "rw")) { f.write(1); }
        check(object.length() == 4096, "corruption keeps the size");
        try {
            GameInstall.install(games, GameInstall.directory(pack("two\n")), null);
            check(false, "corrupt shared object accepted");
        } catch (IOException e) {
            check(e.getMessage().contains("fails size / SHA-256"), "corrupt shared object: " + e.getMessage());
        }
        check(current(gameRoot).equals(first.contentRoot), "refused install leaves current unchanged");
        GameInstall.deleteTree(games);
    }

    private static void crossRootDeltaWithNoLinks() throws Exception {
        File games = Files.createTempDirectory("s2a-games-delta").toFile();
        File initial = pack("delta old"), delta = pack("delta new");
        try {
            GameInstall.Linker noLinks = (created, existing) -> { throw new IOException("links denied"); };
            GameInstall.Result first = GameInstall.install(games, GameInstall.directory(initial), null, noLinks);
            File gameRoot = new File(games, first.titleId);
            BootContract.Current before = BootContract.readCurrent(gameRoot);
            String next = root(delta);
            java.nio.file.Path marker = new File(delta, "game-resource-delta.tsv").toPath();
            String proof = "s2a-resource-delta-v1\t" + before.contentRoot + "\t" + before.artifactSha256 + "\t" + next + "\n";
            Files.delete(new File(delta, "exefs/main.npdm").toPath());
            Files.writeString(marker, proof.replace(before.artifactSha256, "0".repeat(64)));
            refused(games, delta, "delta wrong base binding", "base root or binding", before.contentRoot);
            Files.writeString(marker, proof.replace(before.contentRoot, "sha256-" + "0".repeat(64)));
            refused(games, delta, "delta wrong base root", "base root or binding", before.contentRoot);
            Files.writeString(marker, proof);
            java.nio.file.Path changed = new File(delta, "romfs/readme.txt").toPath();
            Files.delete(changed);
            refused(games, delta, "delta missing changed payload", "lacks changed file", before.contentRoot);
            Files.writeString(changed, "bad bytes");
            refused(games, delta, "delta corrupt changed payload", "does not match index.tsv", before.contentRoot);
            Files.writeString(changed, "delta new");
            File oldNpdm = new File(gameRoot, before.revisionDir + "/exefs/main.npdm");
            byte[] correct = Files.readAllBytes(oldNpdm.toPath());
            byte[] broken = correct.clone();
            broken[0] = 1;
            check(oldNpdm.setWritable(true), "test can perturb private base bytes");
            Files.write(oldNpdm.toPath(), broken);
            try {
                GameInstall.install(games, GameInstall.directory(delta), null, noLinks);
                check(false, "delta accepted corrupted fallback bytes");
            } catch (GamePack.BadPack error) {
                check(error.getMessage().contains("exefs/main.npdm does not match"), error.getMessage());
            }
            check(current(gameRoot).equals(before.contentRoot), "failed delta leaves the old activation pointer");
            Files.write(oldNpdm.toPath(), correct);
            try {
                java.io.ByteArrayOutputStream wire = new java.io.ByteArrayOutputStream();
                wire.write("S2A-IMPORT 1 delta\n".getBytes(StandardCharsets.UTF_8));
                int bytes = 0;
                String[] paths = {"manifest.json", "index.tsv", "game-build-artifact.json",
                                  "ctl/control.nacp", "romfs/readme.txt", "game-resource-delta.tsv"};
                for (String path : paths) {
                    byte[] data = Files.readAllBytes(new File(delta, path).toPath());
                    wire.write(("F " + data.length + " " + path + "\n").getBytes(StandardCharsets.UTF_8));
                    wire.write(data);
                    bytes += data.length;
                }
                wire.write("END\n".getBytes(StandardCharsets.UTF_8));
                java.io.ByteArrayOutputStream reply = new java.io.ByteArrayOutputStream();
                File received = ImportReceiver.receive(new java.io.ByteArrayInputStream(wire.toByteArray()), reply,
                    new File(games, "inbox"));
                check(reply.toString(StandardCharsets.UTF_8).equals("READY 1\nOK 6 " + bytes + "\n"),
                    "receiver acknowledges six delta wire entries");
                GameInstall.Result updated = GameInstall.install(games, GameInstall.directory(received), null, noLinks);
                check(next.equals(updated.contentRoot) && next.equals(current(gameRoot)) && !updated.linked && updated.resourceDelta,
                    "delta fills omitted unchanged file from the old revision when hard links fail");
                check(!new File(received, "romfs/readme.txt").exists(), "verified changed data is adopted from receiver staging");
                GameInstall.Result repeated = GameInstall.install(games, GameInstall.directory(delta), null, noLinks);
                check(repeated.alreadyActive && repeated.resourceDelta, "replaying the complete installed delta is idempotent");
                Files.writeString(marker, proof.replace(next, "sha256-" + "f".repeat(64)));
                refused(games, delta, "delta retry wrong target", "target root does not match", next);
            } catch (GamePack.BadPack | BootContract.BadGame error) {
                check(false, "delta fills omitted unchanged file: " + error.getMessage());
            }
        } finally {
            GameInstall.deleteTree(games);
            GameInstall.deleteTree(initial);
            GameInstall.deleteTree(delta);
        }
    }

    private static void markDelta(File pack, BootContract.Current base) throws IOException {
        Files.writeString(new File(pack, "game-resource-delta.tsv").toPath(),
            "s2a-resource-delta-v1\t" + base.contentRoot + "\t" + base.artifactSha256 + "\t" + root(pack) + "\n");
        Files.delete(new File(pack, "exefs/main.npdm").toPath());
    }

    private static void plainMigration(boolean withdraw) throws Exception {
        File games = Files.createTempDirectory("s2a-games-plain-migration").toFile();
        File first = pack("original plain"), next = pack("changed"), last = pack("changed again");
        try {
            GameInstall.Result seed = GameInstall.install(games, GameInstall.directory(first), null, NO_HARD_LINKS);
            File game = new File(games, seed.titleId);
            BootContract.Current a = BootContract.readCurrent(game);
            File oldLeaf = new File(game, a.revisionDir + "/exefs/main.npdm");
            GameFingerprint.Stamp inode = GameFingerprint.stat(oldLeaf);
            check(!a.managedObjects && !Files.isSymbolicLink(oldLeaf.toPath()) && inode != null,
                "migration starts with an ordinary private installation");
            markDelta(next, a);
            GameInstall.Result b = withdraw
                ? GameInstall.install(games, GameInstall.directory(next), null, NO_HARD_LINKS)
                : GameInstall.install(games, GameInstall.directory(next), null);
            BootContract.Current active = BootContract.readCurrent(game);
            File newLeaf = new File(game, active.revisionDir + "/exefs/main.npdm");
            System.out.printf("plain migration: managed=%s stored=%d shared=%d unchanged-read=%d unchanged-write=%d%n",
                active.managedObjects, b.storedBytes, b.sharedBytes, b.unchangedReadBytes, b.unchangedWriteBytes);
            check(b.resourceDelta && active.contentRoot.equals(root(next)), "delta activates the verified target root");
            check(active.managedObjects && b.managedObjects, "plain migration publishes a managed target");
            check(Files.isSymbolicLink(oldLeaf.toPath()) && Files.isSymbolicLink(newLeaf.toPath())
                && Files.isSameFile(oldLeaf.toPath(), newLeaf.toPath()) && inode != null
                && inode.same(GameFingerprint.stat(newLeaf.getCanonicalFile())), "migration reuses the original inode from both revisions");
            check(b.storedBytes == "changed".length() && b.sharedBytes == 25015 && b.unchangedWriteBytes == 0,
                "migration stores only changed payload bytes");
            markDelta(last, active);
            GameInstall.Result c = GameInstall.install(games, GameInstall.directory(last), null);
            System.out.printf("after plain migration: managed=%s stored=%d shared=%d unchanged-read=%d unchanged-write=%d%n",
                c.managedObjects, c.storedBytes, c.sharedBytes, c.unchangedReadBytes, c.unchangedWriteBytes);
            check(c.resourceDelta && c.managedObjects && c.storedBytes == "changed again".length()
                && c.sharedBytes == b.sharedBytes && c.unchangedReadBytes == 0 && c.unchangedWriteBytes == 0,
                "subsequent delta shares all unchanged bytes without rereading or rewriting them");
            verifyInstalledObjectRevision(game, root(last), "changed again");
        } finally {
            GameInstall.deleteTree(games);
            GameInstall.deleteTree(first);
            GameInstall.deleteTree(next);
            GameInstall.deleteTree(last);
        }
    }

    private static final GameInstall.Linker NO_HARD_LINKS = (link, target) -> {
        throw new IOException("hard links refused");
    };

    private static void relativeLink(File link, File target) throws IOException {
        Files.createSymbolicLink(link.toPath(), link.getParentFile().toPath().relativize(target.toPath()));
    }

    private static void syncDirectory(File directory) throws IOException {
        try (java.nio.channels.FileChannel channel = java.nio.channels.FileChannel.open(directory.toPath(),
                java.nio.file.StandardOpenOption.READ, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            channel.force(true);
        }
    }

    private static File oldNpdm(File game, BootContract.Current current) {
        return new File(game, current.revisionDir + "/exefs/main.npdm");
    }

    private static File npdmObject(File game) {
        String sha = GamePack.sha256(new byte[128]);
        return new File(game, "objects/" + sha.substring(0, 2) + "/" + sha);
    }

    private static void plainMigrationNoSpace() throws Exception {
        for (boolean afterMove : new boolean[]{false, true}) {
            File games = Files.createTempDirectory("s2a-plain-enospc").toFile();
            File old = pack("original plain"), next = pack("changed");
            try {
                GameInstall.Result seed = GameInstall.install(games, GameInstall.directory(old), null, NO_HARD_LINKS);
                File game = new File(games, seed.titleId).getCanonicalFile();
                BootContract.Current before = BootContract.readCurrent(game);
                markDelta(next, before);
                File leaf = oldNpdm(game, before), object = npdmObject(game);
                GameFingerprint.Stamp original = GameFingerprint.stat(leaf);
                boolean[] raised = {false};
                if (afterMove) {
                    GameInstall.setDirectorySync(dir -> {
                        if (!raised[0] && dir.equals(object.getParentFile()) && !leaf.exists() && object.isFile()) {
                            raised[0] = true;
                            throw new java.nio.file.FileSystemException(dir.getPath(), null, "No space left on device");
                        }
                        syncDirectory(dir);
                    });
                } else {
                    GameInstall.setObjectLink((link, target) -> {
                        if (!raised[0] && link.toPath().startsWith(new File(game, ".plain-migration").toPath())
                                && Files.isSymbolicLink(leaf.toPath())) {
                            raised[0] = true;
                            throw new java.nio.file.FileSystemException(link.getPath(), null, "No space left on device");
                        }
                        relativeLink(link, target);
                    });
                }
                GameInstall.Result result = GameInstall.install(games, GameInstall.directory(next), null);
                System.out.printf("plain migration ENOSPC after-move=%s injected=%s stored=%d shared=%d%n",
                    afterMove, raised[0], result.storedBytes, result.sharedBytes);
                check(raised[0] && result.resourceDelta && !result.linked && !result.managedObjects,
                    "metadata ENOSPC falls back to checked copies");
                check(result.storedBytes == 25022 && result.sharedBytes == 0, "fallback copies all target bytes");
                check(original != null && original.same(GameFingerprint.stat(leaf.getCanonicalFile())),
                    "fallback keeps the original inode readable");
                check(!new File(game, ".plain-migration").exists() && (afterMove != Files.isSymbolicLink(leaf.toPath())),
                    "fallback restores a move in flight and keeps a completed alias");
                File retained = new File(game, before.revisionDir);
                var retainedEntries = GamePack.readIndex(Files.readAllBytes(new File(retained, "manifest.json").toPath()),
                    Files.readAllBytes(new File(retained, "index.tsv").toPath()));
                var verified = GameFingerprint.verifyPlaced(retained, retainedEntries,
                    java.util.Collections.nCopies(retainedEntries.size(), null));
                check(verified.recalculated == retainedEntries.size(), "fallback retains every original indexed file");
                verifyInstalledObjectRevision(game, root(next), "changed");
            } finally {
                GameInstall.setDirectorySync(GameInstallTest::syncDirectory);
                GameInstall.setObjectLink(GameInstallTest::relativeLink);
                GameInstall.deleteTree(games);
                GameInstall.deleteTree(old);
                GameInstall.deleteTree(next);
            }
        }
    }

    private static void plainMigrationRejectsCorruption() throws Exception {
        for (boolean corruptObject : new boolean[]{false, true}) {
            File games = Files.createTempDirectory("s2a-plain-corruption").toFile();
            File old = pack("original plain"), next = pack("changed");
            try {
                GameInstall.Result seed = GameInstall.install(games, GameInstall.directory(old), null, NO_HARD_LINKS);
                File game = new File(games, seed.titleId);
                BootContract.Current before = BootContract.readCurrent(game);
                markDelta(next, before);
                File target = corruptObject ? npdmObject(game) : oldNpdm(game, before);
                Files.createDirectories(target.getParentFile().toPath());
                byte[] damage = new byte[128];
                damage[0] = 1;
                if (target.exists()) check(target.setWritable(true), "test can perturb old bytes");
                Files.write(target.toPath(), damage);
                try {
                    GameInstall.install(games, GameInstall.directory(next), null);
                    check(false, "migration accepted corrupt same-size bytes");
                } catch (IOException expected) {
                    check(expected.getMessage().contains("fails size / SHA-256"), "same-size corruption fails the SHA check");
                }
                check(before.contentRoot.equals(current(game)), "corruption leaves the source root active");
                check(!Files.isSymbolicLink(oldNpdm(game, before).toPath()), "corruption does not replace the original inode");
                check(Arrays.equals(Files.readAllBytes(target.toPath()), damage), "corruption is never overwritten by migration");
            } finally {
                GameInstall.deleteTree(games);
                GameInstall.deleteTree(old);
                GameInstall.deleteTree(next);
            }
        }
    }

    private static void plainMigrationKillChild(File games, File next) throws Exception {
        File game = new File(games, "0100000000ABCDEF").getCanonicalFile();
        File leaf = oldNpdm(game, BootContract.readCurrent(game)), object = npdmObject(game);
        GameInstall.setDirectorySync(dir -> {
            syncDirectory(dir);
            if (dir.equals(object.getParentFile()) && !leaf.exists() && object.isFile())
                waitForKill("plain-moved");
        });
        GameInstall.install(games, GameInstall.directory(next), null);
        throw new IOException("expected checkpoint after moving the original inode");
    }

    private static void plainMigrationKillRecovery() throws Exception {
        File games = Files.createTempDirectory("s2a-plain-kill").toFile();
        File old = pack("original plain"), next = pack("changed");
        try {
            GameInstall.Result seed = GameInstall.install(games, GameInstall.directory(old), null, NO_HARD_LINKS);
            File game = new File(games, seed.titleId).getCanonicalFile();
            BootContract.Current before = BootContract.readCurrent(game);
            markDelta(next, before);
            File leaf = oldNpdm(game, before), object = npdmObject(game);
            GameFingerprint.Stamp inode = GameFingerprint.stat(leaf);
            String javaBin = new File(System.getProperty("java.home"), "bin/java").getPath();
            Process child = new ProcessBuilder(javaBin, "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                "-cp", System.getProperty("java.class.path"), GameInstallTest.class.getName(),
                "--plain-kill-child", games.getPath(), next.getPath()).redirectErrorStream(true).start();
            java.util.concurrent.ExecutorService reader = java.util.concurrent.Executors.newSingleThreadExecutor();
            try {
                java.util.concurrent.Future<Boolean> reached = reader.submit(() -> {
                    try (var output = child.inputReader()) {
                        for (String line; (line = output.readLine()) != null; ) {
                            if (line.equals("CHECKPOINT plain-moved")) return true;
                            System.out.println(line);
                        }
                    }
                    return false;
                });
                check(reached.get(20, java.util.concurrent.TimeUnit.SECONDS), "child moved the inode before linking");
            } finally {
                child.destroyForcibly();
                check(child.waitFor(10, java.util.concurrent.TimeUnit.SECONDS) && child.exitValue() != 0,
                    "installer process killed between the two renames");
                reader.shutdownNow();
            }
            check(!leaf.exists() && object.isFile() && inode != null && inode.same(GameFingerprint.stat(object)),
                "interrupted original inode survives in the object store");
            GameInstall.recoverBeforeOpen(games, seed.titleId);
            check(Files.isSymbolicLink(leaf.toPath()) && Files.isSameFile(leaf.toPath(), object.toPath()),
                "selected game startup restores the old leaf from the durable journal");
            check(!new File(game, ".plain-migration").exists(), "recovery clears the journal");
            verifyInstalledObjectRevision(game, before.contentRoot, "original plain");
            GameInstall.Result retry = GameInstall.install(games, GameInstall.directory(next), null);
            check(retry.managedObjects && retry.storedBytes == 7, "retry after process death stores only the changed payload");
            verifyInstalledObjectRevision(game, root(next), "changed");
            System.out.println("plain migration kill: original inode preserved, old revision boots, retry managed");
        } finally {
            GameInstall.deleteTree(games);
            GameInstall.deleteTree(old);
            GameInstall.deleteTree(next);
        }
    }

    private static void managedDelta() throws Exception {
        managedDelta(Files.createTempDirectory("s2a-games-managed").toFile());
    }

    private static void managedDelta(File games) throws Exception {
        File oldPack = pack("managed old"), nextPack = pack("managed new"), later = pack("managed third");
        try {
            GameInstall.Result first = GameInstall.install(games, GameInstall.directory(oldPack), null);
            File game = new File(games, first.titleId);
            BootContract.Current before = BootContract.readCurrent(game);
            File oldLeaf = new File(game, before.revisionDir + "/exefs/main.npdm");
            check(Files.isSymbolicLink(oldLeaf.toPath()), "managed installation publishes a relative object leaf");
            String next = root(nextPack);
            Files.writeString(new File(nextPack, "game-resource-delta.tsv").toPath(),
                "s2a-resource-delta-v1\t" + before.contentRoot + "\t" + before.artifactSha256 + "\t" + next + "\n");
            Files.delete(new File(nextPack, "exefs/main.npdm").toPath());
            GameInstall.Result update = GameInstall.install(games, GameInstall.directory(nextPack), null);
            File newLeaf = new File(game, "revisions/" + next + "/exefs/main.npdm");
            check(next.equals(current(game)) && update.resourceDelta, "managed delta publishes the new root");
            check(Files.isSymbolicLink(newLeaf.toPath()) && Files.isSameFile(oldLeaf.toPath(), newLeaf.toPath()),
                "both revisions resolve the unchanged leaf to the same object inode");
            check(update.storedBytes == "managed new".length(), "managed delta writes only the changed payload bytes");
            check(update.unchangedReadBytes == 0 && update.unchangedWriteBytes == 0,
                "managed delta neither reads nor writes unchanged payload");

            BootContract.Current installed = BootContract.readCurrent(game);
            Files.writeString(new File(later, "game-resource-delta.tsv").toPath(),
                "s2a-resource-delta-v1\t" + installed.contentRoot + "\t" + installed.artifactSha256 + "\t"
                    + root(later) + "\n");
            Files.delete(new File(later, "exefs/main.npdm").toPath());
            File object = newLeaf.getCanonicalFile();
            byte[] sound = Files.readAllBytes(object.toPath());
            byte[] corrupt = sound.clone();
            corrupt[0] ^= 1;
            check(object.setWritable(true), "test can perturb an installed object");
            Files.write(object.toPath(), corrupt);
            Files.setLastModifiedTime(object.toPath(), java.nio.file.attribute.FileTime.fromMillis(
                Files.getLastModifiedTime(object.toPath()).toMillis() + 2000));
            try {
                GameInstall.install(games, GameInstall.directory(later), null);
                check(false, "managed delta accepted a stale fingerprint for corrupted bytes");
            } catch (IOException error) {
                check(error.getMessage().contains("fails size / SHA-256"), "stale object stamp triggers hashing");
            }
            check(current(game).equals(next), "corrupt object leaves current pointer unchanged");
            Files.write(object.toPath(), sound);
            GameInstall.Result rechecked = GameInstall.install(games, GameInstall.directory(later), null);
            check(rechecked.unchangedReadBytes >= sound.length && rechecked.unchangedWriteBytes == 0
                && root(later).equals(current(game)), "unchanged bytes with a stale stamp are read and never rewritten");
        } finally {
            GameInstall.deleteTree(games);
            GameInstall.deleteTree(oldPack);
            GameInstall.deleteTree(nextPack);
            GameInstall.deleteTree(later);
        }
    }

    private static void managedParentAliases() throws Exception {
        Path root = Files.createTempDirectory("s2a-managed-alias");
        File source = pack("link guards");
        try {
            Path actual = Files.createDirectory(root.resolve("actual"));
            Path systemAlias = Files.createSymbolicLink(root.resolve("system-alias"), actual);
            managedDelta(systemAlias.resolve("games").toFile());
            Path outside = Files.createDirectory(root.resolve("outside"));
            Path objects = Files.createDirectories(actual.resolve("games/0100000000ABCDEF/objects"));
            String digest = GamePack.sha256(Files.readAllBytes(new File(source, "exefs/main.npdm").toPath()));
            Files.createSymbolicLink(objects.resolve(digest.substring(0, 2)), outside);
            try {
                GameInstall.install(systemAlias.resolve("games").toFile(), GameInstall.directory(source), null);
                check(false, "managed install accepted an object bucket pointing outside");
            } catch (IOException error) {
                check(error.getMessage().contains("object store component is not a directory"),
                    "object bucket link outside the library is rejected");
            }
            try (var children = Files.list(outside)) {
                check(children.findAny().isEmpty() && !Files.exists(objects.getParent().resolve("current.json")),
                    "external target and activation pointer remain untouched");
            }
        } finally {
            GameInstall.deleteTree(source);
            GameInstall.deleteTree(root.toFile());
        }
    }

    private static void waitForKill(String checkpoint) throws IOException {
        System.out.println("CHECKPOINT " + checkpoint);
        System.out.flush();
        try {
            new java.util.concurrent.CountDownLatch(1).await();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IOException("expected external process kill", interrupted);
        }
    }

    private static void managedKillChild(String phase, File games, File pack) throws Exception {
        String next = root(pack);
        File game = new File(games, "0100000000ABCDEF").getCanonicalFile();
        String incoming = GamePack.sha256(Files.readAllBytes(new File(pack, "romfs/readme.txt").toPath()));
        int[] leaves = {0};
        GameInstall.setObjectLink((link, target) -> {
            boolean projection = !link.getName().equals("link-probe");
            if (projection && phase.equals("object") && incoming.equals(target.getName())) {
                if (!target.isFile() || link.exists()) throw new IOException("expected object before its alias");
                waitForKill(phase);
            }
            Files.createSymbolicLink(link.toPath(), link.getParentFile().toPath().relativize(target.toPath()));
            if (projection && ++leaves[0] == 1 && phase.equals("partial")) waitForKill(phase);
        });
        GameInstall.setDirectorySync(directory -> {
            try (java.nio.channels.FileChannel channel = java.nio.channels.FileChannel.open(directory.toPath(),
                    java.nio.file.StandardOpenOption.READ, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
                channel.force(true);
            }
            if (phase.equals("revision") && directory.getName().equals("revisions")
                    && new File(directory, next).isDirectory() && !next.equals(current(game))) waitForKill(phase);
            if (phase.equals("pointer") && directory.equals(game) && next.equals(current(game))) waitForKill(phase);
        });
        GameInstall.install(games, GameInstall.directory(pack), null);
        throw new IOException("expected checkpoint " + phase);
    }

    private static void verifyInstalledObjectRevision(File game, String expected, String payload) throws Exception {
        BootContract.Game boot = BootContract.open(game);
        check(expected.equals(boot.contentRoot), "recovered pointer identifies a complete revision");
        File revision = new File(game, boot.revisionDir);
        byte[] index = Files.readAllBytes(new File(revision, "index.tsv").toPath());
        var entries = GamePack.readIndex(Files.readAllBytes(new File(revision, "manifest.json").toPath()), index);
        check(Files.readString(new File(revision, "romfs/readme.txt").toPath()).equals(payload),
            "recovered revision has the expected payload");
        GameFingerprint.verify(revision, entries, expected.substring(7),
            GameFingerprint.currentReference(game, expected.substring(7)));
    }

    private static void managedKillBoundaries() throws Exception {
        for (String phase : new String[]{"object", "partial", "revision", "pointer"}) {
            File games = Files.createTempDirectory("s2a-object-kill").toFile();
            File prior = pack("prior payload"), next = pack("next payload");
            boolean retainsTarget = phase.equals("revision");
            try {
                String nextRoot = root(next);
                if (retainsTarget) GameInstall.install(games, GameInstall.directory(next), null);
                GameInstall.Result before = GameInstall.install(games, GameInstall.directory(prior), null);
                File game = new File(games, before.titleId);
                File retained = new File(game, "revisions/" + nextRoot + "/retained-root-sentinel");
                if (retainsTarget) Files.writeString(retained.toPath(), "preserve previous view");
                String javaBin = new File(System.getProperty("java.home"), "bin/java").getPath();
                Process child = new ProcessBuilder(javaBin, "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                    "-cp", System.getProperty("java.class.path"),
                    GameInstallTest.class.getName(), "--managed-kill-child", phase, games.getPath(), next.getPath())
                    .redirectErrorStream(true).start();
                java.util.concurrent.ExecutorService reader = java.util.concurrent.Executors.newSingleThreadExecutor();
                try {
                    java.util.concurrent.Future<Boolean> reached = reader.submit(() -> {
                        try (var output = child.inputReader()) {
                            for (String line; (line = output.readLine()) != null; ) {
                                if (line.equals("CHECKPOINT " + phase)) return true;
                                System.out.println(line);
                            }
                        }
                        return false;
                    });
                    check(reached.get(20, java.util.concurrent.TimeUnit.SECONDS), "real child reached " + phase);
                } finally {
                    child.destroyForcibly();
                    check(child.waitFor(10, java.util.concurrent.TimeUnit.SECONDS) && child.exitValue() != 0,
                        "child process killed at " + phase);
                    reader.shutdownNow();
                }
                boolean committed = phase.equals("pointer");
                verifyInstalledObjectRevision(game, committed ? nextRoot : before.contentRoot,
                    committed ? "next payload" : "prior payload");
                GameInstall.sweep(games);
                if (retainsTarget) check(Files.readString(retained.toPath()).equals("preserve previous view"),
                    "sweep restores the replaced previous revision while old current is active");
                GameInstall.install(games, GameInstall.directory(next), null);
                verifyInstalledObjectRevision(game, nextRoot, "next payload");
                check(new File(game, "revisions/" + before.contentRoot).isDirectory(),
                    "retry retains the old revision after " + phase);
            } finally {
                GameInstall.deleteTree(games);
                GameInstall.deleteTree(prior);
                GameInstall.deleteTree(next);
            }
        }
    }

    /** Feed the native warm reader the installed revision, including the actual object links. */
    private static void installWarmSeeds(Path source, Path games, Path result) throws Exception {
        Map<String, byte[]> extra = new LinkedHashMap<>();
        try (java.util.stream.Stream<Path> paths = Files.walk(source)) {
            for (Path path : (Iterable<Path>) paths.filter(Files::isRegularFile)::iterator)
                extra.put(source.relativize(path).toString(), Files.readAllBytes(path));
        }
        File input = pack("native warm reader", 0x0100000000ABCDEFL, true, extra);
        try {
            GameInstall.Result install = GameInstall.install(games.toFile(), GameInstall.directory(input), null);
            Path game = games.resolve(install.titleId);
            Path revision = game.resolve(BootContract.readCurrent(game.toFile()).revisionDir);
            for (String name : extra.keySet())
                if (!Files.isSymbolicLink(revision.resolve(name))) throw new IOException("expected object link: " + name);
            Files.writeString(result, revision.toString());
        } finally {
            GameInstall.deleteTree(input);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 4 && "--install-warm-seeds".equals(args[0])) {
            installWarmSeeds(Path.of(args[1]), Path.of(args[2]), Path.of(args[3]));
            return;
        }
        if (args.length == 3 && "--plain-kill-child".equals(args[0])) {
            plainMigrationKillChild(new File(args[1]), new File(args[2]));
            return;
        }
        if (args.length == 1 && ("--plain-migration".equals(args[0]) || "--withdraw-plain-migration".equals(args[0]))) {
            boolean withdraw = "--withdraw-plain-migration".equals(args[0]);
            plainMigration(withdraw);
            if (!withdraw) {
                plainMigrationNoSpace();
                plainMigrationRejectsCorruption();
                plainMigrationKillRecovery();
            }
            if (failures != 0) System.exit(1);
            System.out.println("GameInstall plain migration: ok (" + assertions + " checks)");
            return;
        }
        if (args.length == 4 && "--managed-kill-child".equals(args[0])) {
            managedKillChild(args[1], new File(args[2]), new File(args[3]));
            return;
        }
        if (args.length == 1 && "--managed-object-fs".equals(args[0])) {
            managedDelta();
            managedParentAliases();
            managedKillBoundaries();
            if (failures != 0) System.exit(1);
            System.out.println("GameInstall managed objects: ok (" + assertions + " checks)");
            return;
        }
        if (args.length == 3 && "--locked-import-child".equals(args[0])) {
            File internal = new File(args[1]), external = new File(args[2]);
            try {
                GameInstall.importDirectory(internal, GameInstall.importDirectories(internal, external),
                    new File(internal, "import/private-update").getAbsolutePath(), null,
                    new GameInstall.ImportListener() {
                        public void installed(File source, GameInstall.Result result) { System.out.println("INSTALLED " + result.contentRoot); }
                        public void refused(File source, Exception error) {
                            if (String.valueOf(error.getMessage()).contains("busy")) { System.out.println("BUSY"); System.exit(2); }
                            System.out.println("REFUSED " + error);
                        }
                    });
                System.out.println("SCANNED");
            } catch (IOException busy) {
                if (!busy.getMessage().contains("busy")) throw busy;
                System.out.println("BUSY");
                System.exit(2);
            }
            return;
        }
        if (args.length == 1 && "--locked-imports".equals(args[0])) {
            lockedImports();
            if (failures > 0) System.exit(1);
            System.out.println("GameInstall locked queues: ok (" + assertions + " checks)");
            return;
        }
        transactionBoundaries();
        if (args.length == 1 && "--transaction-boundaries".equals(args[0])) {
            if (failures > 0) System.exit(1);
            System.out.println("GameInstall transaction boundaries: ok (" + assertions + " checks)");
            return;
        }
        lockedImports();
        importInboxes();
        File games = Files.createTempDirectory("s2a-games").toFile();
        File gameRoot = new File(games, "0100000000ABCDEF");
        File v1 = pack("one\n"), v2 = pack("two\n"), v3 = pack("three\n");
        String r1 = root(v1), r2 = root(v2), r3 = root(v3);

        GameInstall.Result a = GameInstall.install(games, GameInstall.directory(v1), null);
        check(!a.alreadyActive && a.contentRoot.equals(r1) && a.previousContentRoot == null, "first install");
        check(current(gameRoot).equals(r1) && Arrays.equals(revisions(gameRoot), new String[] {r1}), "first install layout");
        check(BootContract.open(gameRoot).modules.size() == 1, "BootContract opens the installed game");
        check(!new File(gameRoot, "current.json.tmp").exists(), "no current.json.tmp left");

        check(GameInstall.install(games, GameInstall.directory(v1), null).alreadyActive, "same package again: already active");

        // Same files, new artifact (a per-game APK upgrade): the artifact is refreshed in place of the old one.
        File v1b = Files.createTempDirectory("s2a-install-pack").toFile();
        for (String rel : new String[] {"manifest.json", "index.tsv", GamePack.ARTIFACT, "exefs/main.npdm", "romfs/readme.txt",
                                        "ctl/control.nacp", "modules/main.patched.bin", "modules/main.fixups.tsv",
                                        "modules/main.gates.tsv", "modules/main.npdm"}) {
            File to = new File(v1b, rel);
            to.getParentFile().mkdirs();
            Files.copy(new File(v1, rel).toPath(), to.toPath());
        }
        String art = new String(Files.readAllBytes(new File(v1b, GamePack.ARTIFACT).toPath()), StandardCharsets.UTF_8);
        Files.write(new File(v1b, GamePack.ARTIFACT).toPath(),
                    art.replace("\n}\n", ",\n \"createdAt\": \"2026-10-23T00:00:00Z\"\n}\n").getBytes(StandardCharsets.UTF_8));
        String oldArtifact = BootContract.readCurrent(gameRoot).artifact;
        // APK-only deployment sends metadata, not another copy of RomFS/modules.
        GameInstall.Source refreshOnly = path -> Arrays.asList("manifest.json", "index.tsv", GamePack.ARTIFACT,
            "ctl/control.nacp").contains(path) ? GameInstall.directory(v1b).open(path) : null;
        GameInstall.Result refreshed = GameInstall.install(games, refreshOnly, null);
        BootContract.Current now = BootContract.readCurrent(gameRoot);
        check(refreshed.artifactRefreshed && !refreshed.alreadyActive && now.contentRoot.equals(r1) && !now.artifact.equals(oldArtifact)
              && !new File(gameRoot, "revisions/" + r1 + "/" + oldArtifact).exists() && BootContract.open(gameRoot) != null,
              "artifact refresh: " + now.artifact);
        check(GameInstall.install(games, refreshOnly, null).alreadyActive, "refreshed artifact again: already active");
        File emptyGames = Files.createTempDirectory("s2a-refresh-first-install").toFile();
        try {
            GameInstall.install(emptyGames, refreshOnly, null);
            check(false, "metadata-only refresh must not act as a first install");
        } catch (GamePack.BadPack | IOException expected) {
            check(!new File(emptyGames, a.titleId + "/current.json").exists(), "failed refresh does not activate incomplete content");
        } finally {
            GameInstall.deleteTree(emptyGames);
        }

        GameInstall.Result b = GameInstall.install(games, GameInstall.directory(v2), null);
        check(r1.equals(b.previousContentRoot) && current(gameRoot).equals(r2), "upgrade records the previous revision");
        // Only romfs/readme.txt differs: the update stores just that file, the rest are the same objects.
        check(b.linked && b.storedBytes == 4 && b.sharedBytes > 4096,
              "update stores only the changed file: stored " + b.storedBytes + ", shared " + b.sharedBytes + ", linked " + b.linked);
        java.nio.file.Path link = new File(gameRoot, "revisions/" + r2 + "/modules/main.patched.bin").toPath();
        check(Files.isSymbolicLink(link) && !Files.readSymbolicLink(link).isAbsolute(),
              "revision file is a relative object link");
        String moduleSha = GamePack.sha256(new byte[4096]);
        check(Files.isSameFile(link, new File(gameRoot, "objects/" + moduleSha.substring(0, 2) + "/" + moduleSha).toPath()),
              "revision file shares the objects/ inode");
        check(Files.isSameFile(new File(gameRoot, "revisions/" + r1 + "/modules/main.patched.bin").toPath(),
                               new File(gameRoot, "revisions/" + r2 + "/modules/main.patched.bin").toPath()), "unchanged file is one inode");

        // The offline shader pack must read as ordinary files: its validator rejects a link at any
        // path component. Hard links give the same bytes without a symbolic link.
        java.nio.file.Path packFile = new File(gameRoot, "revisions/" + r2 + "/" + PACK_DIR + "/manifest.json").toPath();
        String packSha = GamePack.sha256(PACK_MANIFEST.getBytes(StandardCharsets.UTF_8));
        check(!Files.isSymbolicLink(packFile) && Files.isRegularFile(packFile, java.nio.file.LinkOption.NOFOLLOW_LINKS),
              "offline pack file is a regular file, not a symbolic link");
        check(Files.isSameFile(packFile, new File(gameRoot, "objects/" + packSha.substring(0, 2) + "/" + packSha).toPath()),
              "offline pack file shares the objects/ inode");
        check(Arrays.equals(Files.readAllBytes(packFile), PACK_MANIFEST.getBytes(StandardCharsets.UTF_8)),
              "offline pack file has the package bytes");
        boolean packClean = !Files.isSymbolicLink(new File(gameRoot, "revisions/" + r2 + "/shaders").toPath());
        for (java.nio.file.Path p = packFile; p.startsWith(new File(gameRoot, "revisions/" + r2).toPath()); p = p.getParent())
            packClean &= !Files.isSymbolicLink(p);
        check(packClean, "no symbolic link in any offline pack path component");
        check(!new File(gameRoot, "revisions/" + r2 + "/romfs/readme.txt").canWrite(), "stored objects are read-only");
        String[] both = {r1, r2};
        Arrays.sort(both);
        check(Arrays.equals(revisions(gameRoot), both), "upgrade keeps the previous revision: " + Arrays.toString(revisions(gameRoot)));

        GameInstall.install(games, GameInstall.directory(v3), null);
        String[] kept = {r2, r3};
        Arrays.sort(kept);
        check(current(gameRoot).equals(r3) && Arrays.equals(revisions(gameRoot), kept), "older than previous pruned: " + Arrays.toString(revisions(gameRoot)));
        String oneSha = GamePack.sha256("one\n".getBytes(StandardCharsets.UTF_8));
        check(!new File(gameRoot, "objects/" + oneSha.substring(0, 2) + "/" + oneSha).exists(), "object only the pruned revision used is gone");
        check(new File(gameRoot, "objects").list().length > 0 && !new File(gameRoot, "objects/tmp").exists(), "object store kept, no temp files");

        // Refusals leave everything as it was.
        File tampered = pack("four\n");
        byte[] rom = Files.readAllBytes(new File(tampered, "romfs/readme.txt").toPath());
        rom[0] ^= 1;
        Files.write(new File(tampered, "romfs/readme.txt").toPath(), rom);
        refused(games, tampered, "changed byte", "does not match index.tsv", r3);
        refused(games, pack("five\n", 0x0100000000000001L, true), "artifact for another title", "titleId", r3);
        refused(games, pack("six\n", 0x0100000000ABCDEFL, false), "no artifact", GamePack.ARTIFACT, r3);

        // Rollback: the previous revision is installed again as a package and becomes current.
        GameInstall.Result back = GameInstall.install(games, GameInstall.directory(v2), null);
        check(current(gameRoot).equals(r2) && r3.equals(back.previousContentRoot) && Arrays.equals(revisions(gameRoot), kept),
              "rollback to the previous revision");
        check(back.storedBytes == 0, "rollback stores nothing new: " + back.storedBytes);

        // A killed install leaves a .staging and maybe current.json.tmp; the startup sweep removes them.
        new File(gameRoot, "revisions/" + r1 + ".staging/romfs").mkdirs();
        Files.write(new File(gameRoot, "current.json.tmp").toPath(), new byte[] {'{'});
        File orphan = new File(gameRoot, "objects/ab/ab" + "0".repeat(62));   // stored by an install that then failed
        orphan.getParentFile().mkdirs();
        Files.write(orphan.toPath(), new byte[] {1});
        check(GameInstall.sweep(games) == 1 && !new File(gameRoot, "current.json.tmp").exists(), "sweep");
        check(!orphan.exists() && !orphan.getParentFile().exists(), "sweep removes objects no revision uses");
        check(current(gameRoot).equals(r2) && Arrays.equals(revisions(gameRoot), kept), "sweep keeps the active game");

        // One path refuses hard links after the probe succeeded: that file is copied, the rest stay shared.
        File copyGames = Files.createTempDirectory("s2a-games-copy-fallback").toFile();
        GameInstall.Linker refusesPack = (created, existing) -> {
            if (created.getPath().contains("shaders/vulkan/generic")) throw new IOException("injected hard link refusal");
            Files.createLink(created.toPath(), existing.toPath());
        };
        GameInstall.Result partial = GameInstall.install(copyGames, GameInstall.directory(v1), null, refusesPack);
        File copyRoot = new File(copyGames, "0100000000ABCDEF");
        java.nio.file.Path copiedPack = new File(copyRoot, "revisions/" + r1 + "/" + PACK_DIR + "/manifest.json").toPath();
        check(partial.linked && !Files.isSymbolicLink(copiedPack)
              && Files.isRegularFile(copiedPack, java.nio.file.LinkOption.NOFOLLOW_LINKS),
              "a refused hard link falls back to a regular file");
        check(!Files.isSameFile(copiedPack, new File(copyRoot, "objects/" + packSha.substring(0, 2) + "/" + packSha).toPath()),
              "fallback copy is a separate inode");
        check(Arrays.equals(Files.readAllBytes(copiedPack), PACK_MANIFEST.getBytes(StandardCharsets.UTF_8)),
              "fallback copy has the verified bytes");
        check(Files.isSameFile(new File(copyRoot, "revisions/" + r1 + "/modules/main.patched.bin").toPath(),
                               new File(copyRoot, "objects/" + moduleSha.substring(0, 2) + "/" + moduleSha).toPath()),
              "other revision files still share their objects");
        GameInstall.deleteTree(copyGames);

        // The storage refuses hard links entirely: every file is copied into the revision, no objects.
        File plainGames = Files.createTempDirectory("s2a-games-plain").toFile();
        GameInstall.Result plain = GameInstall.install(plainGames, GameInstall.directory(v1), null,
            (created, existing) -> { throw new IOException("injected hard link refusal"); });
        File plainRoot = new File(plainGames, "0100000000ABCDEF");
        java.nio.file.Path plainPack = new File(plainRoot, "revisions/" + r1 + "/" + PACK_DIR + "/manifest.json").toPath();
        check(!plain.linked && !Files.isSymbolicLink(plainPack)
              && Files.isRegularFile(plainPack, java.nio.file.LinkOption.NOFOLLOW_LINKS),
              "no hard links at all: the revision is a checked copy");
        check(!new File(plainRoot, "objects/" + packSha.substring(0, 2)).exists(), "no object store when linking is refused");
        check(BootContract.open(plainRoot).modules.size() == 1, "copied revision opens");
        GameInstall.deleteTree(plainGames);
        sharedObjectRehashed();
        crossRootDeltaWithNoLinks();
        plainMigration(false);
        plainMigrationNoSpace();
        plainMigrationRejectsCorruption();
        plainMigrationKillRecovery();
        managedDelta();
        managedParentAliases();
        managedKillBoundaries();

        if (args.length == 1) {   // a package from tools/make_pack.py
            File real = Files.createTempDirectory("s2a-games-real").toFile();
            GameInstall.Result r = GameInstall.install(real, GameInstall.directory(new File(args[0])), null);
            BootContract.Game g = BootContract.open(new File(real, r.titleId));
            check(g.modules.size() >= 1, "make_pack.py package installs and opens");
            System.out.println("make_pack.py package: " + r.info.name + " " + r.info.version + " [" + r.titleId + "] -> " + r.contentRoot
                + ", " + g.modules.size() + " module(s)");
        }
        GameInstall.deleteTree(games);
        if (failures > 0) System.exit(1);
        System.out.println("GameInstall: ok (" + assertions + " checks)");
    }
}
