// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.function.Function;

/** Android host binding for the shared .s2asave inspection and restore transaction. */
final class SaveImport {
    static final class Failure extends IOException {
        final String code;

        Failure(String code, String message) {
            super(message);
            this.code = code;
        }
    }

    static final class Preview {
        final Path archive;
        final String title, revision, archiveDigest, targetFingerprint;
        final long files, bytes;
        final List<Object> warnings;

        Preview(Path archive, BootContract.Game game, Map<String, Object> preview) throws IOException {
            this.archive = archive;
            title = game.titleId;
            revision = game.contentRoot;
            archiveDigest = string(preview, "archiveSha256");
            targetFingerprint = string(preview, "targetFingerprint");
            files = count(preview, "fileCount");
            bytes = count(preview, "totalBytes");
            warnings = new ArrayList<>(Json.array(preview.get("warnings"), "warnings"));
        }
    }

    static final class Receipt {
        final String title, backup;
        final long files, bytes;

        Receipt(Preview preview, String backup) {
            title = preview.title;
            files = preview.files;
            bytes = preview.bytes;
            this.backup = backup;
        }

        String json() {
            return "{\"titleId\":" + BootSpec.quote(title) + ",\"files\":" + files + ",\"bytes\":" + bytes
                + ",\"previousBackup\":" + BootSpec.quote(backup) + "}";
        }
    }

    private final File privateRoot;
    private final String language;
    private final Function<byte[], byte[]> bridge;

    SaveImport(File privateRoot, String language, Function<byte[], byte[]> bridge) {
        this.privateRoot = privateRoot;
        this.language = language;
        this.bridge = bridge;
    }

    Preview inspect(String title, Path archive) throws IOException, BootContract.BadGame {
        try (GameStorage lease = GameStorage.acquire(privateRoot, title)) {
            return inspect(open(title), archive, false);
        }
    }

    Receipt restore(Preview preview) throws IOException, BootContract.BadGame {
        try (GameStorage lease = GameStorage.acquire(privateRoot, preview.title)) {
            BootContract.Game game = open(preview.title);
            if (!game.contentRoot.equals(preview.revision))
                throw new Failure("save_changed", "Game resources changed after the save inspection.");
            return restore(game, preview, false);
        }
    }

    Receipt restoreUnattended(String title, Path archive) throws IOException, BootContract.BadGame {
        try (GameStorage lease = GameStorage.acquire(privateRoot, title)) {
            BootContract.Game game = open(title);
            return restore(game, inspect(game, archive, true), true);
        }
    }

    /** Caller holds the game storage lease, before prepareState can create a missing data root. */
    void recoverIfNeeded(BootContract.Game game) throws IOException {
        File work = workRoot(privateRoot, game.titleId);
        if (Files.exists(new File(work, "save-transactions/" + game.titleId + ".json").toPath(),
                         LinkOption.NOFOLLOW_LINKS))
            request(game, "recover", "");
    }

    private static File workRoot(File privateRoot, String title) {
        return new File(privateRoot, "save-transfer/" + title);
    }

    private BootContract.Game open(String title) throws IOException, BootContract.BadGame {
        BootContract.Game game = BootContract.open(new File(privateRoot, "games/" + title));
        if (!title.equals(game.titleId))
            throw new Failure("save_wrong_game", "The selected game identity changed.");
        return game;
    }

    private Preview inspect(BootContract.Game game, Path archive, boolean unattended) throws IOException {
        var result = request(game, "inspect", archiveFields(archive, unattended));
        return new Preview(archive, game, result);
    }

    private Receipt restore(BootContract.Game game, Preview preview, boolean unattended) throws IOException {
        var result = request(game, "restore", archiveFields(preview.archive, unattended)
            + ",\"archiveSha256\":" + BootSpec.quote(preview.archiveDigest)
            + ",\"targetFingerprint\":" + BootSpec.quote(preview.targetFingerprint));
        if (!Boolean.TRUE.equals(result.get("committed")))
            throw new Failure("save_bridge", "The save service did not confirm the restore.");
        String backup = string(result, "previousBackup");
        if (!backup.isEmpty()) {
            Path root = privateRoot.getCanonicalFile().toPath();
            Path path = new File(backup).toPath().normalize();
            if (path.startsWith(root)) backup = root.relativize(path).toString();
        }
        return new Receipt(preview, backup);
    }

    private static String archiveFields(Path archive, boolean unattended) {
        return ",\"path\":" + BootSpec.quote(archive.toAbsolutePath().toString())
            + ",\"unattendedRestore\":" + unattended;
    }

    private Map<String, Object> request(BootContract.Game game, String operation, String fields) throws IOException {
        File root = privateRoot.getCanonicalFile();
        String title = BootSpec.quote(game.titleId);
        // The Android revision digest is authoritative on this device. Apple packages use
        // original-container identity, so cross-platform versions cannot be compared by it.
        String identity = BootSpec.quote(game.contentRoot.substring("sha256-".length()));
        String json = "{\"operation\":" + BootSpec.quote(operation) + ",\"language\":" + BootSpec.quote(language)
            + ",\"game\":{\"id\":" + title + ",\"name\":" + title + ",\"version\":0,\"contentIdentity\":" + identity + "}"
            + ",\"androidRevisionIdentity\":true,\"dataRoot\":"
            + BootSpec.quote(GameSession.statePath(root, game.titleId).getPath())
            + ",\"workRoot\":" + BootSpec.quote(workRoot(root, game.titleId).getPath()) + fields + "}";
        try {
            byte[] raw = bridge.apply(json.getBytes(StandardCharsets.UTF_8));
            if (raw == null) throw new IllegalArgumentException("null response");
            var response = Json.object(Json.parseStrict(new String(raw, StandardCharsets.UTF_8)), "save response");
            if (Boolean.TRUE.equals(response.get("ok"))) return Json.object(response.get("result"), "save result");
            var error = Json.object(response.get("error"), "save error");
            throw new Failure(string(error, "code"), string(error, "message"));
        } catch (IllegalArgumentException error) {
            throw new Failure("save_bridge", "The save service returned an invalid result.");
        }
    }

    private static long count(Map<String, Object> value, String key) throws IOException {
        if (!(value.get(key) instanceof Long number) || number < 0)
            throw new Failure("save_bridge", "The save service returned an invalid count.");
        return number;
    }

    private static String string(Map<String, Object> value, String key) throws IOException {
        if (!(value.get(key) instanceof String text))
            throw new Failure("save_bridge", "The save service returned invalid text.");
        return text;
    }
}
