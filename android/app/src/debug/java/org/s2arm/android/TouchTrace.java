// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/** Debug game process trace. Each publication contains the most recent complete records. */
final class TouchTrace {
    static final int MAX_BYTES = 1024 * 1024;
    private static final int PUBLISH_DELAY_MS = 100;
    private static final byte[] END = utf8("touch-trace end\n");
    private static TouchTrace instance;

    private final Path file;
    private final String identity;
    private final Consumer<IOException> failure;
    private final ScheduledExecutorService worker;
    private final MessageDigest sha256;
    private final ArrayDeque<Block> blocks = new ArrayDeque<>();
    private long lastSequence, lastTime, dropped;
    private int bytes;
    private boolean scheduled, writeFailed;

    static synchronized TouchTrace open(File files, int pid, Consumer<IOException> failure) {
        if (instance == null) {
            try {
                String stat = Files.readString(Path.of("/proc/self/stat"), StandardCharsets.UTF_8);
                instance = new TouchTrace(files, pid, ProcessBirth.startTicks(stat, pid), failure);
            } catch (IOException error) {
                failure.accept(error);
            }
        }
        return instance;
    }

    TouchTrace(File files, int pid, long birth, Consumer<IOException> failure) {
        file = files.toPath().resolve("diag/touch-trace.log");
        identity = "touch-trace v1 pid=" + pid + " birth=" + birth + " seq=";
        this.failure = failure;
        try {
            sha256 = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException("SHA-256 unavailable", error);
        }
        worker = Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "s2a-touch-trace");
            thread.setDaemon(true);
            return thread;
        });
    }

    /** Called on input publication or a layout change. Disk I/O runs on the worker. */
    synchronized void record(String layout, String input) {
        // On Android nanoTime is the same monotonic clock family as input uptime.
        // Preserve ordering even when two callbacks land on the same clock tick.
        lastTime = Math.max(System.nanoTime(), lastTime + 1);
        byte[] body = utf8((layout == null ? "" : layout + "\n") + (input == null ? "" : input + "\n"));
        Block block = new Block(utf8(identity + ++lastSequence + " dropped="),
            utf8(" mono_ns=" + lastTime + " bytes=" + body.length + " sha256=" + digest(body) + "\n"), body);
        blocks.addLast(block);
        bytes += block.maxSize();
        while (bytes > MAX_BYTES) {
            bytes -= blocks.removeFirst().maxSize();
            dropped++;
        }
        if (!scheduled) {
            scheduled = true;
            worker.schedule(this::publish, PUBLISH_DELAY_MS, TimeUnit.MILLISECONDS);
        }
    }

    private void publish() {
        Block[] snapshot;
        byte[] droppedText;
        synchronized (this) {
            snapshot = blocks.toArray(new Block[0]);
            droppedText = utf8(Long.toString(dropped));
            scheduled = false;
        }
        Path parent = file.getParent();
        Path temporary = parent.resolve(".touch-trace.tmp");
        try {
            Files.createDirectories(parent);
            try (OutputStream out = new BufferedOutputStream(Files.newOutputStream(temporary))) {
                for (Block block : snapshot) {
                    out.write(block.head);
                    out.write(droppedText);
                    out.write(block.tail);
                    out.write(block.body);
                    out.write(END);
                }
            }
            Files.move(temporary, file, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            writeFailed = false;
        } catch (IOException error) {
            if (!writeFailed) failure.accept(error);
            writeFailed = true;
        }
    }

    private String digest(byte[] body) {
        StringBuilder hex = new StringBuilder(64);
        for (byte value : sha256.digest(body)) {
            hex.append(Character.forDigit((value & 0xff) >>> 4, 16));
            hex.append(Character.forDigit(value & 15, 16));
        }
        return hex.toString();
    }

    private static byte[] utf8(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    private static final class Block {
        final byte[] head, tail, body;

        Block(byte[] head, byte[] tail, byte[] body) {
            this.head = head;
            this.tail = tail;
            this.body = body;
        }

        int maxSize() {
            return head.length + 19 + tail.length + body.length + END.length;
        }
    }
}
