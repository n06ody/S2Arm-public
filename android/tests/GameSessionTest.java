package org.s2arm.android;
import java.io.*;
import java.nio.file.*;
import java.util.*;
public final class GameSessionTest {
    static int count;
    static void check(boolean value, String message) {
        count++; if (!value) throw new AssertionError(message);
    }
    interface Throwing { void run() throws Exception; }
    static void rejects(Throwing action, String message) throws Exception {
        count++; try { action.run(); } catch (IOException expected) { return; }
        throw new AssertionError(message);
    }
    static void rejectsState(Throwing action, String message) throws Exception {
        count++; try { action.run(); } catch (IllegalStateException expected) { return; }
        throw new AssertionError(message);
    }
    static BootSpec spec(Path root) {
        BootSpec s = new BootSpec(); s.mode = "game";
        s.sessionId = "3f2c1a9e-7b6d-4c8f-9a10-2d4e6f8a0b1c";
        s.gameRoot = root.resolve("games/0100000000000000").toString();
        s.userRoot = root.resolve("user/0").toString();
        s.saveRoot = root.resolve("user/0/saves/0100000000000000").toString();
        s.cacheRoot = root.resolve("cache").toString();
        String hash = "00".repeat(32), content = "sha256-" + hash;
        BootContract.Module main = new BootContract.Module("main", "apk-asset", "main.patched.bin", "main.fixups.tsv", "main.gates.tsv", null, "0000");
        BootContract.Apk apk = new BootContract.Apk("org.s2arm.game.t0100000000000000", 1, hash, hash, "game/modules");
        s.game = new BootContract.Game("0100000000000000", content, "revisions/"+content, "game-build-artifact.json", hash,
            BootContract.REWRITE_ABI, "0.1.0", "vulkan", "exefs/main.npdm", "ctl/control.nacp", "romfs", "modules", null, 1, 1,
            new ArrayList<>(Collections.singletonList(main)), apk, null);
        return s;
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("session-test-");
        try {
            run(root);
        } finally {
            // Only authored fixtures under this test-owned root; do not follow links.
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                for (Path path : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator)
                    Files.delete(path);
            }
        }
    }
    private static void run(Path root) throws Exception {
        BootSpec spec = spec(root); GameSession s = new GameSession(spec, root.toFile());
        Path state = Path.of(s.stateDir);
        check(Files.isDirectory(state), "persistent state created");
        Path cache = Files.createDirectories(state.resolve("direct-cache/nvn-vk"));
        Path program = Files.write(cache.resolve("programs-v1-test.bin"), new byte[]{1});
        Path pipe = Files.write(cache.resolve("pipes-v1-352.bin"), new byte[]{2});
        Path saved = Files.write(state.resolve("retained.bin"), new byte[]{3});
        try (GameSession keep = new GameSession(spec, root.toFile())) {
            check(Files.exists(program) && Files.exists(pipe), "cache retained with clear disabled");
        }
        GameSession.clearDirectCache = true;
        try (GameSession cold = new GameSession(spec, root.toFile())) {
            check(!Files.exists(cache) && Files.exists(saved), "cold launch removes only direct-cache/nvn-vk");
        } finally {
            GameSession.clearDirectCache = false;
        }
        System.out.println("PASS cold session clears both program and pipeline sentinels; saved sentinel retained");
        check(s.begin(false,true,1280,720,320,60,BootSpec.ConsoleMode.HANDHELD) == null, "background no admission");
        check(s.begin(true,false,1280,720,320,60,BootSpec.ConsoleMode.HANDHELD) == null, "missing surface no admission");
        check(s.begin(true,true,0,720,320,60,BootSpec.ConsoleMode.HANDHELD) == null, "zero extent no admission");
        check(s.begin(true,true,1280,720,320,Float.NaN,BootSpec.ConsoleMode.HANDHELD) == null, "invalid display no admission");
        check(!s.attempted(), "invalid attempts leave startup unconsumed");
        GameSession.FrozenStart frozen = s.begin(true,true,1280,720,320,60,BootSpec.ConsoleMode.HANDHELD);
        check(frozen != null, "valid foreground admits");
        check(s.begin(true,true,1920,1080,480,120,BootSpec.ConsoleMode.HANDHELD) == null, "single process startup once");
        rejectsState(() -> s.begin(true,true,1280,720,320,60,BootSpec.ConsoleMode.DOCKED), "frozen session rejects mode change");
        check(spec.screen.width==1280 && spec.screen.refreshHz==60, "admitted screen frozen");
        frozenCases(root);
        Path save = state.resolve("keep.bin"); byte[] contents={1,4,7}; Files.write(save,contents);
        GameSession restored = new GameSession(spec(root), root.toFile());
        check(Arrays.equals(contents,Files.readAllBytes(save)), "existing state preserved");
        restored.stop(); check(restored.begin(true,true,1280,720,320,60,BootSpec.ConsoleMode.HANDHELD) == null, "stop prevents admission");
        Path legacy = Files.createDirectory(root.resolve("legacy")); BootSpec ls=spec(legacy);
        Files.createDirectories(Path.of(ls.saveRoot)); Files.writeString(Path.of(ls.saveRoot,"save.bin"),"keep");
        new GameSession(ls,legacy.toFile());   // 存档根目录里的旧文件不挡起局（不堵门）
        check(Files.readString(Path.of(ls.saveRoot,"save.bin")).equals("keep")
            && Files.isDirectory(Path.of(ls.saveRoot,"ryujinx-android")), "legacy kept untouched; session state beside it");
        Path linked = Files.createDirectory(root.resolve("linked")); Files.createSymbolicLink(linked.resolve("user"),root.resolve("user"));
        rejects(()->new GameSession(spec(linked),linked.toFile()),"child symlink rejected");
        Path aliases = Files.createDirectory(root.resolve("aliases")); Files.createSymbolicLink(aliases.resolve("app"), root);
        GameSession alias = new GameSession(spec(aliases.resolve("app")), aliases.resolve("app").toFile());
        check(Path.of(alias.stateDir).toRealPath().equals(state.toRealPath()), "trusted OS app root alias accepted");
        canonicalStateCases(root);
        Path conflict = Files.createDirectory(root.resolve("conflict")); BootSpec cs=spec(conflict);
        Files.createDirectories(Path.of(cs.saveRoot)); Files.writeString(Path.of(cs.saveRoot,"ryujinx-android"),"do not replace");
        rejects(()->new GameSession(cs,conflict.toFile()),"state file conflict rejected");
        BootSpec outside=spec(root);outside.saveRoot=root.resolve("elsewhere").toString();
        rejects(()->new GameSession(outside,root.toFile()),"save outside user rejected");
        BootSpec diagnostic=spec(root);diagnostic.mode="diagnostic";
        rejects(()->new GameSession(diagnostic,root.toFile()),"diagnostic not game");
        Path installedApk = root.resolve("base.apk");
        Files.writeString(installedApk,"abc");
        String sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        check(GameSession.matchesApk(installedApk.toFile(),sha), "exact installed APK bytes accepted");
        Files.writeString(installedApk,"abd");
        check(!GameSession.matchesApk(installedApk.toFile(),sha), "changed build bytes rejected even with same package metadata");
        rejects(()->GameSession.matchesApk(root.resolve("missing.apk").toFile(),sha),"missing installed APK rejected");
        System.out.println("GameSession: " + count + " checks PASS (host only; no runtime/game execution)");
    }

    private static void canonicalStateCases(Path root) throws Exception {
        Path real = Files.createDirectory(root.resolve("canonical-root")).toRealPath();
        Path link = root.resolve("canonical-alias");
        Files.createSymbolicLink(link, real);
        // The trusted Context root may have a linked ancestor, not just be a link itself.
        Path app = Files.createDirectory(real.resolve("app"));
        Path alias = link.resolve("app");
        BootSpec source = spec(alias);
        Path expected = Path.of(spec(app).saveRoot, "ryujinx-android");
        GameSession fresh = new GameSession(source, alias.toFile());
        check(fresh.stateDir.equals(expected.toString()), "new state path itself is canonical");
        check(Files.isDirectory(expected), "canonical state created under trusted root alias");
        Path marker = expected.resolve("keep.bin");
        byte[] contents = {9, 2, 6, 5}; Files.write(marker, contents);
        Object stateKey = Files.readAttributes(expected, java.nio.file.attribute.BasicFileAttributes.class).fileKey();
        Object markerKey = Files.readAttributes(marker, java.nio.file.attribute.BasicFileAttributes.class).fileKey();
        GameSession restored = new GameSession(spec(alias), alias.toFile());
        check(restored.stateDir.equals(expected.toString()), "existing state path itself is canonical");
        check(Files.isSameFile(Path.of(restored.stateDir), expected), "alias returns same state directory");
        check(Objects.equals(stateKey, Files.readAttributes(expected, java.nio.file.attribute.BasicFileAttributes.class).fileKey())
            && Objects.equals(markerKey, Files.readAttributes(marker, java.nio.file.attribute.BasicFileAttributes.class).fileKey()),
            "existing state and file identity preserved");
        check(Arrays.equals(contents, Files.readAllBytes(marker)), "trusted root alias preserves original contents");
        check(Files.isSymbolicLink(link) && Files.readSymbolicLink(link).equals(real), "trusted root alias unchanged");

        Path childApp = Files.createDirectory(real.resolve("child-app"));
        Path childAlias = link.resolve("child-app");
        Files.createSymbolicLink(childApp.resolve("user"), app.resolve("user"));
        rejects(() -> new GameSession(spec(childAlias), childAlias.toFile()), "child link under trusted alias rejected");
        check(Files.isSymbolicLink(childApp.resolve("user")), "rejected child link untouched");

        Path stateApp = Files.createDirectory(real.resolve("state-app"));
        Path stateAlias = link.resolve("state-app");
        Path save = Files.createDirectories(Path.of(spec(stateApp).saveRoot));
        Path stateLink = Files.createSymbolicLink(save.resolve("ryujinx-android"), expected);
        rejects(() -> new GameSession(spec(stateAlias), stateAlias.toFile()), "state link under trusted alias rejected");
        check(Files.isSymbolicLink(stateLink) && Arrays.equals(contents, Files.readAllBytes(marker)),
            "rejected state link and target untouched");

        Path legacyApp = Files.createDirectory(real.resolve("legacy-app"));
        Path legacyAlias = link.resolve("legacy-app");
        Path legacySave = Files.createDirectories(Path.of(spec(legacyApp).saveRoot));
        Path old = legacySave.resolve("save.bin"); Files.write(old, contents);
        new GameSession(spec(legacyAlias), legacyAlias.toFile());   // 旧文件不挡起局（不堵门）
        check(Arrays.equals(contents, Files.readAllBytes(old))
            && Files.isDirectory(legacySave.resolve("ryujinx-android")), "legacy content kept untouched; session state created beside it");
    }

    private static void frozenCases(Path root) throws Exception {
        for (BootSpec.ConsoleMode mode : BootSpec.ConsoleMode.values()) {
            BootSpec source = spec(root);
            source.user.language = "zh-Hans-CN"; source.user.region = "CN"; source.user.timezone = "UTC";
            GameSession session = new GameSession(source, root.toFile());
            GameSession.FrozenStart start = session.begin(true,true,1280,720,320,60,mode);
            String expected = mode == BootSpec.ConsoleMode.DOCKED ? "docked" : "handheld";
            check(expected.equals(Json.object(Json.parse(start.bootJson), "BootSpec").get("consoleMode"))
                && start.nativeConsoleMode() == (mode == BootSpec.ConsoleMode.DOCKED ? 1 : 0),
                "JSON and native consumer share exact mode: " + expected);
            String json = start.bootJson;
            source.consoleMode = mode == BootSpec.ConsoleMode.DOCKED ? BootSpec.ConsoleMode.HANDHELD : BootSpec.ConsoleMode.DOCKED;
            source.user.language = "ja"; source.user.region = "JP"; source.user.timezone = "Asia/Tokyo";
            source.screen.width = 99; source.sessionId = "changed"; source.gameRoot = "/changed";
            source.game.modules.clear();
            check(start.bootJson.equals(json) && start.consoleMode == mode
                && start.nativeConsoleMode() == (mode == BootSpec.ConsoleMode.DOCKED ? 1 : 0),
                "source mutation cannot change frozen JSON or native mode: " + expected);
            check(start.language.equals("zh-Hans-CN") && start.region.equals("CN") && start.timezone.equals("UTC")
                && start.sessionId.equals("3f2c1a9e-7b6d-4c8f-9a10-2d4e6f8a0b1c"), "HostApi values frozen");
            check(start.revisionDir.startsWith(root.resolve("games").toString())
                && start.npdmPath.endsWith("exefs/main.npdm") && start.modules.size() == 1
                && start.programId == 0x0100000000000000L, "native start values frozen");
            try { start.modules.clear(); throw new AssertionError("frozen module list writable"); }
            catch (UnsupportedOperationException expectedError) { count++; }
        }
        BootSpec bad = spec(root); bad.cacheRoot = null;
        GameSession retry = new GameSession(bad, root.toFile());
        try {
            retry.begin(true,true,1280,720,320,60,BootSpec.ConsoleMode.DOCKED);
            throw new AssertionError("incomplete snapshot admitted");
        } catch (NullPointerException expected) { count++; }
        check(!retry.attempted(), "failed snapshot does not consume admission");
        bad.cacheRoot = root.resolve("cache").toString();
        check(retry.begin(true,true,1280,720,320,60,BootSpec.ConsoleMode.DOCKED).nativeConsoleMode() == 1,
            "complete snapshot admits after preparation failure");
    }
}
