// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.regex.MatchResult;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class TouchTraceTest {
    private static final Pattern HEADER = Pattern.compile(
        "touch-trace v1 pid=(\\d+) birth=(\\d+) seq=(\\d+) dropped=(\\d+) mono_ns=(\\d+) bytes=(\\d+) sha256=([0-9a-f]{64})");
    private static final String END = "touch-trace end\n";

    private record Packet(MatchResult header, String text) {
        long field(int index) { return Long.parseLong(header.group(index)); }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static List<Packet> read(Path path) throws Exception {
        byte[] raw = Files.readAllBytes(path);
        check(raw.length <= TouchTrace.MAX_BYTES, "snapshot exceeded the byte cap");
        // Decode one byte at a time for framing, including UTF-8 layout labels.
        String bytes = new String(raw, StandardCharsets.ISO_8859_1);
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        List<Packet> result = new ArrayList<>();
        int offset = 0;
        while (offset < bytes.length()) {
            int newline = bytes.indexOf('\n', offset);
            check(newline >= 0, "published header was truncated");
            Matcher head = HEADER.matcher(bytes.substring(offset, newline));
            check(head.matches(), "invalid header");
            int bodyEnd = newline + 1 + Integer.parseInt(head.group(6));
            check(bodyEnd <= bytes.length() && bytes.startsWith(END, bodyEnd), "body was not committed");
            byte[] body = bytes.substring(newline + 1, bodyEnd).getBytes(StandardCharsets.ISO_8859_1);
            check(HexFormat.of().formatHex(sha.digest(body)).equals(head.group(7)), "body hash mismatch");
            result.add(new Packet(head.toMatchResult(), new String(body, StandardCharsets.UTF_8)));
            offset = bodyEnd + END.length();
        }
        for (int index = 0; index < result.size(); index++) {
            Packet item = result.get(index);
            Packet first = result.get(0);
            check(item.field(1) == first.field(1) && item.field(2) == first.field(2), "mixed process");
            check(item.field(3) == first.field(4) + index + 1 && item.field(4) == first.field(4), "dropped count or seq gap");
            if (index > 0) check(item.field(5) > result.get(index - 1).field(5), "time must advance");
        }
        return result;
    }

    private static List<Packet> await(Path path, long birth, long seq) throws Exception {
        long deadline = System.nanoTime() + 5_000_000_000L;
        do {
            if (Files.exists(path)) {
                List<Packet> packets = read(path);
                if (!packets.isEmpty() && packets.get(0).field(2) == birth
                        && packets.get(packets.size() - 1).field(3) == seq) return packets;
            }
            Thread.sleep(10);
        } while (System.nanoTime() < deadline);
        throw new AssertionError("complete snapshot did not arrive for seq=" + seq);
    }

    public static void main(String[] args) throws Exception {
        Path files = Files.createTempDirectory(Path.of(args[0]), "touch-trace-");
        Path path = files.resolve("diag/touch-trace.log");
        ConcurrentLinkedQueue<Exception> errors = new ConcurrentLinkedQueue<>();
        var trace = new TouchTrace(files.toFile(), 101, 7001, errors::add);
        TouchPad pad = new TouchPad();
        pad.setLayoutTrace(trace::record);
        pad.layout(0, 0, 2400, 1080);
        var layout = await(path, 7001, 1).get(0);
        check(layout.text().startsWith("touchpad layout:") && layout.text().endsWith("generation=1\n")
            && layout.text().contains(" −="), "exact UTF-8 snapshot");
        String input = "input: buttons=0x0000 L=(0,0) R=(7214,0)";
        String neutral = "input: buttons=0x0000 L=(0,0) R=(0,0)";
        check(!pad.traceInput(null), "a neutral event must not consume the first input replay");
        check(pad.traceInput(input), "active input must be paired");
        check(!pad.traceInput(input), "only one first effective input per layout");
        trace.record(null, neutral);
        var first = await(path, 7001, 3);
        check(first.get(1).text().equals(layout.text() + input + "\n"), "layout and first input are in one committed block");
        check(first.get(2).text().equals(neutral + "\n"), "release returns to zero");
        Path sample = files.resolve("writer-sample.log");
        Files.copy(path, sample);

        String terminal = "touch gesture end: action=up down_ms=1000 event_ns=1030000000";
        trace.record(null, terminal);
        var withEnd = await(path, 7001, 4);
        check(withEnd.get(3).text().equals(terminal + "\n"), "gesture end is a separate verified block after zero");

        for (int n = 1; n <= 7500; n++) {
            trace.record(null, "input: buttons=0x0000 L=(0,0) R=(" + n + ",0)");
        }
        var ring = await(path, 7001, 7504);
        check(ring.get(0).field(4) > 0, "rotation must announce evictions");
        check(ring.get(ring.size() - 1).text().endsWith("R=(7500,0)\n"), "rotation retains newest input");

        // A new process can reuse the same PID. Its first publication displaces the old segment.
        var replacement = new TouchTrace(files.toFile(), 101, 7002, errors::add);
        replacement.record(layout.text().stripTrailing(), null);
        var newRun = await(path, 7002, 1);
        check(newRun.size() == 1 && newRun.get(0).field(4) == 0, "reused PID must start a fresh segment");
        check(errors.isEmpty(), "unexpected write failure: " + errors);
        System.out.println("TouchTrace: complete UTF-8 blocks, first input, zero, bounded eviction and PID reuse; fixture " + sample);
    }
}
