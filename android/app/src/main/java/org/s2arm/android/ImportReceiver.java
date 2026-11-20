package org.s2arm.android;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * 资源包直传（电脑 → App 私有目录），不经共享存储：电脑端 android/tools/deploy_game.py 经
 * `adb forward tcp:N localabstract:<socketName>` 连到 LibraryActivity 开的本地套接字，逐个文件把字节流进来。
 * 原先先 adb push 到 /sdcard/Android/data/…/import 再由 App 读进私有目录：vivo 上 App 经共享存储（FUSE）
 * 读 1.7 GB 的大文件会停在中途不返回（11-13 奥日导入卡死）；iOS 是 devicectl 直接写进 App 沙盒，没有这一层。
 *
 * 协议（每行 UTF-8、以 \n 结尾）：App 先发 READY 1（电脑据此确认套接字已开：adb forward 在设备端没人监听时也先接 TCP、随后才断）；电脑发
 *   S2A-IMPORT 1 &lt;名字&gt;
 *   F &lt;字节数&gt; &lt;相对路径&gt;    后面紧跟这么多字节，可重复
 *   END
 * 回一行：OK &lt;文件数&gt; &lt;字节数&gt; 或 ERR &lt;原因&gt;。
 * 原地登记（资源目录电脑已 adb push 到共享存储，不传字节）：电脑发一行 S2A-INPLACE 1 &lt;绝对路径&gt;，回 OK in-place，
 * 之后由 GameLibrary.registerInPlace 原地登记（GameInstall.installInPlace）。
 * 存档恢复：S2A-SAVE 1 &lt;程序号&gt; &lt;字节数&gt;，紧跟 .s2asave 字节和 END 行。
 * 暂存完由调用方交给共用存档服务，恢复提交后才回 OK 和回执。
 * 收进 parent/.&lt;名字&gt;.part，收完整才改名成 parent/&lt;名字&gt;（parent = 私有 files/import），
 * 收的同时逐文件算 SHA-256，收完把「相对路径 → SHA-256」登记在进程内（verified，别的 App 改不了内存）；
 * 之后照常走 GameLibrary.scanImports：安装时大小与 SHA-256 都对得上索引的文件直接改名挪进对象库（GameInstall.Source.adopt），
 * 不再读一遍、复制一遍；对不上的照旧复制并核对。
 * 本类不碰安卓 API，套接字与对端身份（只认 adb 的 shell uid）在 LibraryActivity。
 */
final class ImportReceiver {
    private ImportReceiver() { }

    static final int VERSION = 1;
    private static final java.util.Map<String, java.util.Map<String, String>> VERIFIED = new java.util.concurrent.ConcurrentHashMap<>();

    /** 这个目录是本进程刚收进来的：返回边收边算的「相对路径 → SHA-256」，否则 null。 */
    static java.util.Map<String, String> verified(File dir) {
        try { return VERIFIED.get(dir.getCanonicalPath()); } catch (IOException e) { return null; }
    }
    private static final int MAX_LINE = 4096;

    /** 一次连接的结果。只有 saveTitle 非空时 dir 指向暂存的存档文件。 */
    static final class Received {
        final File dir;
        final boolean inPlace;
        final String saveTitle;
        Received(File dir, boolean inPlace) { this(dir, inPlace, null); }
        Received(File dir, boolean inPlace, String saveTitle) {
            this.dir = dir;
            this.inPlace = inPlace;
            this.saveTitle = saveTitle;
        }
    }

    /** 只收直传（原地登记请求当错报）。 */
    static File receive(InputStream rawIn, OutputStream out, File parent) throws IOException {
        Received got = receiveAny(rawIn, out, parent);
        if (got.inPlace) throw new IOException("in-place request where a transfer was expected");
        if (got.saveTitle != null) {
            Files.deleteIfExists(got.dir.toPath());
            throw new IOException("save request where a resource transfer was expected");
        }
        return got.dir;
    }

    /** 收一次传输或一条原地登记请求；成功返回结果，失败抛异常（已回 ERR、临时目录已删）。 */
    static Received receiveAny(InputStream rawIn, OutputStream out, File parent) throws IOException {
        return receiveAny(rawIn, out, parent, parent);
    }

    static Received receiveAny(InputStream rawIn, OutputStream out, File parent, File saveStaging) throws IOException {
        InputStream in = new BufferedInputStream(rawIn, 1 << 20);
        File part = null;
        try {
            reply(out, "READY " + VERSION);
            String header = line(in);
            if (header.startsWith("S2A-SAVE ")) return receiveSave(header, in, saveStaging);
            String[] head = header.split(" ", 3);
            if (head.length == 3 && head[0].equals("S2A-INPLACE") && head[1].equals(String.valueOf(VERSION))) {
                String root = head[2];
                if (!root.startsWith("/")) throw new IOException("in-place path is not absolute: " + root);
                safePath(root.substring(1));
                reply(out, "OK in-place");
                return new Received(new File(root), true);
            }
            if (head.length != 3 || !head[0].equals("S2A-IMPORT") || !head[1].equals(String.valueOf(VERSION)))
                throw new IOException("bad header");
            String name = head[2];
            if (!name.matches("[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}")) throw new IOException("bad package name: " + name);
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            part = new File(parent, "." + name + ".part");
            GameInstall.deleteTree(part);
            if (!part.mkdirs()) throw new IOException("cannot create " + part);
            long files = 0, bytes = 0;
            byte[] buf = new byte[1 << 20];
            java.util.Map<String, String> sums = new java.util.HashMap<>();
            while (true) {
                String l = line(in);
                if (l.equals("END")) break;
                String[] f = l.split(" ", 3);
                if (f.length != 3 || !f[0].equals("F")) throw new IOException("bad entry line");
                long size;
                try { size = Long.parseLong(f[1]); } catch (NumberFormatException e) { throw new IOException("bad size"); }
                if (size < 0) throw new IOException("bad size");
                File target = new File(part, safePath(f[2]));
                File dir = target.getParentFile();
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
                java.security.MessageDigest md = GamePack.digest();
                try (FileOutputStream os = new FileOutputStream(target)) {
                    for (long left = size; left > 0; ) {
                        int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                        if (n < 0) throw new IOException("stream ended inside " + f[2]);
                        md.update(buf, 0, n); os.write(buf, 0, n); left -= n;
                    }
                }
                sums.put(f[2], GamePack.hex(md.digest()));
                files++; bytes += size;
            }
            File done = new File(parent, name);
            VERIFIED.remove(done.getCanonicalPath());
            GameInstall.deleteTree(done);
            if (!part.renameTo(done)) throw new IOException("cannot move " + part + " into place");
            part = null;
            VERIFIED.put(done.getCanonicalPath(), sums);
            reply(out, "OK " + files + " " + bytes);
            return new Received(done, false);
        } catch (IOException | RuntimeException e) {
            if (part != null) GameInstall.deleteTree(part);
            try { reply(out, "ERR " + String.valueOf(e.getMessage()).replace('\n', ' ')); } catch (IOException ignored) { }
            throw e;
        }
    }

    private static Received receiveSave(String header, InputStream in, File parent) throws IOException {
        String[] fields = header.split(" ", -1);
        if (fields.length != 4 || !fields[1].equals(String.valueOf(VERSION))
                || !fields[2].matches("[0-9A-F]{16}") || !fields[3].matches("[0-9]{1,11}"))
            throw new IOException("save_header");
        Path archive = SaveArchive.stage(parent.toPath(), in, Long.parseLong(fields[3]));
        try {
            if (!"END".equals(line(in))) throw new IOException("save_trailing_data");
            return new Received(archive.toFile(), false, fields[2]);
        } catch (IOException | RuntimeException error) {
            try { Files.deleteIfExists(archive); }
            catch (IOException cleanup) { error.addSuppressed(cleanup); }
            throw error;
        }
    }

    // 相对路径：不许绝对、不许 . / .. / 空段、不许反斜杠
    static String safePath(String p) throws IOException {
        if (p.isEmpty() || p.startsWith("/") || p.indexOf('\\') >= 0 || p.indexOf('\0') >= 0) throw new IOException("bad path: " + p);
        for (String seg : p.split("/", -1))
            if (seg.isEmpty() || seg.equals(".") || seg.equals("..")) throw new IOException("bad path: " + p);
        return p;
    }

    private static String line(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        for (int c; (c = in.read()) != '\n'; ) {
            if (c < 0) throw new IOException("stream ended");
            if (b.size() >= MAX_LINE) throw new IOException("line too long");
            b.write(c);
        }
        return new String(b.toByteArray(), StandardCharsets.UTF_8);
    }

    static void reply(OutputStream out, String s) throws IOException {
        out.write((s + "\n").getBytes(StandardCharsets.UTF_8));
        out.flush();
    }
}
