package org.s2arm.android;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Android game package (android/tools/make_pack.py): the 资源包 v2 data directory
 * (pack/pack.py: manifest.json + index.tsv + files) with the S2A_TARGET=android modules under
 * modules/. The checks follow pack/verify.py, plus what the Android runtime needs. No Android
 * types, so the unit test runs on a plain JDK.
 */
final class GamePack {
    /** A0 build artifact (tools/make_pack.py) beside manifest.json; it names index.tsv's hash, so is not in it. */
    static final String ARTIFACT = "game-build-artifact.json";

    static final class BadPack extends Exception {
        private static final long serialVersionUID = 1L;
        BadPack(String message) { super(message); }
    }

    static final class Entry {
        final String path, sha256;
        final long size;
        Entry(String path, long size, String sha256) { this.path = path; this.size = size; this.sha256 = sha256; }
    }

    /** What the panel shows for an installed game. */
    static final class Info {
        String titleId = "", name = "", version = "";
        int files;
        long bytes;
        final List<String> modules = new ArrayList<>();
    }

    interface Progress { void update(long done, long total); }

    private static final Pattern MANIFEST_FIELD = Pattern.compile("\"(version|files|total|index_sha256)\"\\s*:\\s*(\"([0-9a-f]{64})\"|(\\d+))");

    static String sha256(byte[] data) {
        MessageDigest md = digest();
        md.update(data);
        return hex(md.digest());
    }

    static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
    }

    static String hex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }

    /** manifest.json + index.tsv: version 2, index hash, counts; then every entry's path. */
    static List<Entry> readIndex(byte[] manifest, byte[] index) throws BadPack {
        Map<String, String> m = new LinkedHashMap<>();
        Matcher f = MANIFEST_FIELD.matcher(new String(manifest, StandardCharsets.UTF_8));
        while (f.find()) m.put(f.group(1), f.group(3) != null ? f.group(3) : f.group(4));
        if (!"2".equals(m.get("version"))) throw new BadPack("manifest.json is not version 2");
        if (!sha256(index).equals(m.get("index_sha256"))) throw new BadPack("index.tsv does not match manifest index_sha256");
        List<Entry> entries = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        long total = 0;
        String text = new String(index, StandardCharsets.UTF_8);
        int pos = 0, length = text.length();
        while (pos < length) {
            int nl = text.indexOf('\n', pos);
            int end = nl < 0 ? length : nl;
            if (end > pos) {
                Entry e = indexEntry(text, pos, end);
                if (!seen.add(e.path)) throw new BadPack("index.tsv lists " + e.path + " twice");
                total += e.size;
                entries.add(e);
            }
            pos = end + 1;
        }
        // index.tsv 已按 manifest 里的哈希核过，文件清单以它为准；manifest 里的两个汇总数对不上只记日志（不堵门）
        if (!String.valueOf(entries.size()).equals(m.get("files")) || !String.valueOf(total).equals(m.get("total")))
            System.err.println("S2Arm: manifest.json file count / total (" + m.get("files") + " / " + m.get("total")
                + ") differs from index.tsv (" + entries.size() + " / " + total + "); index.tsv is authoritative");
        requireLayout(seen);
        return entries;
    }

    static Entry indexEntry(String row) throws BadPack { return indexEntry(row, 0, row.length()); }

    private static Entry indexEntry(String text, int start, int end) throws BadPack {
        int t1 = text.indexOf('\t', start), t2 = t1 < 0 ? -1 : text.indexOf('\t', t1 + 1);
        int t3 = t2 < 0 ? -1 : text.indexOf('\t', t2 + 1);
        if (t1 < 0 || t1 >= end || t2 < 0 || t2 >= end || (t3 >= 0 && t3 < end)
                || !shaColumn(text, t2 + 1, end) || !sizeColumn(text, t1 + 1, t2))
            throw new BadPack("index.tsv line is not path<TAB>bytes<TAB>sha256: " + text.substring(start, end));
        String path = text.substring(start, t1);
        checkPath(path);
        return new Entry(path, Long.parseLong(text.substring(t1 + 1, t2)), text.substring(t2 + 1, end));
    }

    private static boolean shaColumn(String text, int from, int to) {
        if (to - from != 64) return false;
        for (int i = from; i < to; i++) {
            char c = text.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    private static boolean sizeColumn(String text, int from, int to) {
        if (to - from < 1 || to - from > 18) return false;
        for (int i = from; i < to; i++) {
            char c = text.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    // Relative, '/'-separated, no '.' / '..' components, nothing the importer could write outside the game.
    private static void checkPath(String p) throws BadPack {
        int n = p.length();
        if (n == 0 || p.charAt(0) == '/' || p.indexOf('\\') >= 0 || p.indexOf("//") >= 0
            || p.equals("manifest.json") || p.equals("index.tsv") || p.equals(ARTIFACT))
            throw new BadPack("unsafe path in index.tsv: " + p);
        // Single pass over segments; split("/") drops a trailing empty segment, so "a/" stays legal.
        int start = 0;
        for (int i = 0; i <= n; i++) {
            if (i == n || p.charAt(i) == '/') {
                int len = i - start;
                if ((len == 0 && i < n) || (len == 1 && p.charAt(start) == '.')
                        || (len == 2 && p.charAt(start) == '.' && p.charAt(start + 1) == '.'))
                    throw new BadPack("unsafe path in index.tsv: " + p);
                start = i + 1;
            } else if (p.charAt(i) < 0x20) {
                throw new BadPack("unsafe path in index.tsv: " + p);
            }
        }
    }

    // A package carries its modules (source "revision"), or none: a per-game APK's resource package,
    // whose modules are in the APK (source "apk-asset"; BootContract checks the artifact's choice).
    private static void requireLayout(Set<String> paths) throws BadPack {
        boolean modules = paths.stream().anyMatch(p -> p.startsWith("modules/"));
        String[] needs = modules
            ? new String[] {"exefs/main.npdm", "ctl/control.nacp", "modules/main.npdm",
                            "modules/main.patched.bin", "modules/main.fixups.tsv", "modules/main.gates.tsv"}
            : new String[] {"exefs/main.npdm", "ctl/control.nacp"};
        for (String need : needs)
            if (!paths.contains(need)) throw new BadPack("package has no " + need);
        // RomFS：romfs/ 下的散文件，或一份标准 RomFS 镜像 romfs.std（出包缺省形态；服务层 Application.cs 直接读它）
        if (!paths.contains("romfs.std") && paths.stream().noneMatch(p -> p.startsWith("romfs/")))
            throw new BadPack("package has no romfs/ files or romfs.std");
        for (String p : paths) {
            if (!p.startsWith("modules/") || !p.endsWith(".patched.bin")) continue;
            String base = p.substring(0, p.length() - ".patched.bin".length());
            if (!paths.contains(base + ".fixups.tsv") || !paths.contains(base + ".gates.tsv"))
                throw new BadPack(base + " lacks its .fixups.tsv / .gates.tsv");
        }
    }

    /** Rewritten modules for another target would read the wrong thread register. */
    static void requireAndroidTarget(String path, byte[] fixups) throws BadPack {
        for (String line : new String(fixups, StandardCharsets.UTF_8).split("\n"))
            if (line.equals("# TARGET\tandroid")) return;
        throw new BadPack(path + " was not rewritten for Android (no '# TARGET android'; rewrite with S2A_TARGET=android)");
    }

    /** Title id, name and version from legacy or bounded raw-DEFLATE NACP names. */
    static void describe(Info info, byte[] nacp) throws BadPack {
        if (nacp.length < 0x3070) throw new BadPack("ctl/control.nacp is too short");
        byte[] names = nacp;
        int count = 16;
        int length = (nacp[0] & 0xff) | ((nacp[1] & 0xff) << 8);
        boolean candidate = length > 0 && length <= 0x2ffe;
        if (candidate) {
            Inflater inflater = new Inflater(true);
            try {
                inflater.setInput(nacp, 2, length);
                byte[] uncompressed = new byte[0x6001];
                int used = 0;
                while (used < uncompressed.length && !inflater.finished()) {
                    int n = inflater.inflate(uncompressed, used, uncompressed.length - used);
                    if (n == 0) break;
                    used += n;
                }
                if (used == 0x6000 && inflater.finished() && inflater.getRemaining() == 0) {
                    names = uncompressed;
                    count = 32;
                }
            } catch (DataFormatException invalid) {
                // DEFLATE has no magic. The legacy fallback below must still
                // be valid UTF-8, contain no controls and be zero-padded.
            } finally {
                inflater.end();
            }
        }
        boolean legacyCandidate = candidate && names == nacp;
        // 名字和版本号只用来显示：解不出（不是 UTF-8、带控制字符、压缩格式认不出）不挡导入，名字退回程序号、版本留空，记日志（不堵门）
        for (int i = 0; i < count && info.name.isEmpty(); i++) {
            try { info.name = cString(names, i * 0x300, 0x200, legacyCandidate); }
            catch (BadPack unreadable) { System.err.println("S2Arm: NACP name " + i + " unreadable, skipped: " + unreadable.getMessage()); }
        }
        long id = 0;
        for (int i = 7; i >= 0; i--) id = (id << 8) | (nacp[0x3038 + i] & 0xff);
        info.titleId = String.format(Locale.ROOT, "%016X", id);
        try { info.version = cString(nacp, 0x3060, 0x10, false); }
        catch (BadPack unreadable) { info.version = ""; System.err.println("S2Arm: NACP version unreadable: " + unreadable.getMessage()); }
        if (id == 0) throw new BadPack("ctl/control.nacp has no application id");
        if (info.name.isEmpty()) info.name = info.titleId;
    }

    private static String cString(byte[] b, int off, int max, boolean padded) throws BadPack {
        int n = 0;
        while (n < max && b[off + n] != 0) n++;
        if (padded && n < max) {
            for (int i = n + 1; i < max; i++)
                if (b[off + i] != 0) throw new BadPack("invalid legacy or compressed NACP name");
        }
        final String text;
        try {
            text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(b, off, n)).toString();
        } catch (CharacterCodingException invalid) {
            throw new BadPack("NACP name is not valid UTF-8");
        }
        for (int i = 0; i < text.length(); i++) {
            int ch = text.charAt(i);
            if ((ch < 32 && ch != 9 && ch != 10 && ch != 13) || ch == 0xfffe || ch == 0xffff)
                throw new BadPack("NACP name has an invalid control character");
        }
        return text.trim();
    }

    static byte[] read(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) { return readAll(in); }
    }

    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        return out.toByteArray();
    }

    /**
     * Full check of a package directory: index, every file's size and SHA-256, no files
     * outside the index, module targets; returns what it describes.
     */
    static Info verifyDirectory(File dir, Progress progress) throws BadPack, IOException {
        List<Entry> entries = readIndex(read(new File(dir, "manifest.json")), read(new File(dir, "index.tsv")));
        Info info = new Info();
        long total = 0, done = 0;
        for (Entry e : entries) total += e.size;
        byte[] buf = new byte[1 << 20];
        Set<String> listed = new HashSet<>();
        for (Entry e : entries) {
            listed.add(e.path);
            File f = new File(dir, e.path);
            if (!f.isFile() || f.length() != e.size) throw new BadPack(e.path + " is missing or has the wrong size");
            MessageDigest md = digest();
            try (InputStream in = new FileInputStream(f)) {
                for (int n; (n = in.read(buf)) > 0; ) { md.update(buf, 0, n); done += n; if (progress != null) progress.update(done, total); }
            }
            if (!hex(md.digest()).equals(e.sha256)) throw new BadPack(e.path + " does not match its SHA-256");
            if (e.path.startsWith("modules/") && e.path.endsWith(".fixups.tsv")) {
                requireAndroidTarget(e.path, read(f));
                String name = e.path.substring("modules/".length(), e.path.length() - ".fixups.tsv".length());
                info.modules.add(name);
            }
        }
        List<String> stray = new ArrayList<>();
        collectStray(dir, "", listed, stray);
        if (!stray.isEmpty()) throw new BadPack("files outside index.tsv: " + stray.subList(0, Math.min(3, stray.size())));
        describe(info, read(new File(dir, "ctl/control.nacp")));
        info.files = entries.size();
        info.bytes = total;
        return info;
    }

    private static void collectStray(File dir, String prefix, Set<String> listed, List<String> out) {
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File c : children) {
            String rel = prefix.isEmpty() ? c.getName() : prefix + "/" + c.getName();
            if (c.isDirectory()) collectStray(c, rel, listed, out);
            else if (!listed.contains(rel) && !rel.equals("manifest.json") && !rel.equals("index.tsv")
                     && !rel.equals(ARTIFACT)) out.add(rel);   // the artifact names the index, so sits outside it
        }
    }
}
