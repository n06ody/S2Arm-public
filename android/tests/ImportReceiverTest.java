package org.s2arm.android;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/** 资源包直传接收（ImportReceiver）：协议、路径防越界、收不全不落地。期望按协议手算。 */
public final class ImportReceiverTest {
    private static int failures, checks;
    private static void check(boolean ok, String what) { checks++; if (!ok) { failures++; System.err.println("FAIL: " + what); } }

    private static byte[] stream(String... parts) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        for (String p : parts) b.write(p.getBytes(StandardCharsets.ISO_8859_1));
        return b.toByteArray();
    }

    public static void main(String[] args) throws Exception {
        File parent = Files.createTempDirectory("s2a-recv").toFile();
        // ① 正常：两个文件（一个空），收完改名成 parent/pkg，回 READY 再回 OK 2 5
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        File done = ImportReceiver.receive(new ByteArrayInputStream(stream(
            "S2A-IMPORT 1 pkg\n", "F 5 a/b.bin\n", "hello", "F 0 empty\n", "END\n")), out, parent);
        check(done.equals(new File(parent, "pkg")), "lands at parent/pkg");
        check(Arrays.equals(Files.readAllBytes(new File(done, "a/b.bin").toPath()), "hello".getBytes()), "content byte-exact");
        check(new File(done, "empty").isFile() && new File(done, "empty").length() == 0, "empty file kept");
        check(out.toString().equals("READY 1\nOK 2 5\n"), "replies READY then OK (" + out + ")");
        check(!new File(parent, ".pkg.part").exists(), "no .part left");

        // ② 越界路径：拒，回 ERR，不留临时目录，不落 parent/evil
        for (String bad : new String[] {"../x", "/abs", "a//b", "a/./b", "a\\b"}) {
            out = new ByteArrayOutputStream(); String err = "";
            try { ImportReceiver.receive(new ByteArrayInputStream(stream("S2A-IMPORT 1 evil\n", "F 1 " + bad + "\n", "x", "END\n")), out, parent); }
            catch (IOException e) { err = e.getMessage(); }
            check(err.startsWith("bad path"), "refuses path " + bad + " (" + err + ")");
            check(out.toString().startsWith("READY 1\nERR bad path"), "replies ERR for " + bad);
            check(!new File(parent, "evil").exists() && !new File(parent, ".evil.part").exists(), "nothing left for " + bad);
        }

        // ③ 流在文件中途断：拒，不落地，旧的 parent/pkg 不被替换
        out = new ByteArrayOutputStream(); String err = "";
        try { ImportReceiver.receive(new ByteArrayInputStream(stream("S2A-IMPORT 1 pkg\n", "F 10 a/b.bin\n", "abc")), out, parent); }
        catch (IOException e) { err = e.getMessage(); }
        check(err.contains("stream ended"), "truncated stream refused (" + err + ")");
        check(Arrays.equals(Files.readAllBytes(new File(parent, "pkg/a/b.bin").toPath()), "hello".getBytes()), "previous package untouched");

        // ④ 名字不合法 / 头不对：拒
        for (String head : new String[] {"S2A-IMPORT 1 ../up\n", "S2A-IMPORT 2 pkg\n", "HELLO\n"}) {
            err = "";
            try { ImportReceiver.receive(new ByteArrayInputStream(stream(head, "END\n")), new ByteArrayOutputStream(), parent); }
            catch (IOException e) { err = e.getMessage(); }
            check(!err.isEmpty(), "refuses header " + head.trim());
        }
        // ⑤ 收进来的文件直接挪（adopt）：SHA-256 与大小对得上 → 改名到 target、内容不变；对不上 / 不是本进程收的目录 → false、原地不动
        String helloSha = GamePack.hex(java.security.MessageDigest.getInstance("SHA-256").digest("hello".getBytes()));
        ImportReceiver.receive(new ByteArrayInputStream(stream("S2A-IMPORT 1 adopt\n", "F 5 a/b.bin\n", "hello", "F 5 c.bin\n", "world", "END\n")),
            new ByteArrayOutputStream(), parent);
        File got = new File(parent, "adopt");
        GameInstall.Source src = GameInstall.directory(got);
        File target = new File(parent, "objects/he/" + helloSha);
        check(src.adopt("a/b.bin", helloSha, 5, target), "verified file adopted");
        check(Arrays.equals(Files.readAllBytes(target.toPath()), "hello".getBytes()) && !new File(got, "a/b.bin").exists(),
              "adopted = moved, content unchanged");
        check(!src.adopt("c.bin", helloSha, 5, new File(parent, "objects/x")), "sha mismatch not adopted");
        check(new File(got, "c.bin").isFile(), "mismatched file stays for the checked copy");
        File plain = new File(parent, "plain"); plain.mkdirs(); Files.write(new File(plain, "a.bin").toPath(), "hello".getBytes());
        check(!GameInstall.directory(plain).adopt("a.bin", helloSha, 5, new File(parent, "objects/y")), "not received here: never adopted");
        // ⑥ 原地登记请求：不传字节，回 OK in-place，返回电脑给的绝对路径；相对 / 带 .. 的路径拒；只收直传的入口把它当错
        out = new ByteArrayOutputStream();
        ImportReceiver.Received req = ImportReceiver.receiveAny(new ByteArrayInputStream(stream(
            "S2A-INPLACE 1 /storage/emulated/0/S2Arm/Games/0100000000ABCDEF\n")), out, parent);
        check(req.inPlace && req.dir.getPath().equals("/storage/emulated/0/S2Arm/Games/0100000000ABCDEF"), "in-place request path");
        check(out.toString().equals("READY 1\nOK in-place\n"), "in-place replies OK in-place (" + out + ")");
        for (String bad : new String[] {"S2A-INPLACE 1 relative/x\n", "S2A-INPLACE 1 /a/../b\n", "S2A-INPLACE 2 /a\n"}) {
            err = "";
            try { ImportReceiver.receiveAny(new ByteArrayInputStream(stream(bad)), new ByteArrayOutputStream(), parent); }
            catch (IOException e) { err = e.getMessage(); }
            check(!err.isEmpty(), "refuses in-place request " + bad.trim());
        }
        err = "";
        try { ImportReceiver.receive(new ByteArrayInputStream(stream("S2A-INPLACE 1 /a\n")), new ByteArrayOutputStream(), parent); }
        catch (IOException e) { err = e.getMessage(); }
        check(err.startsWith("in-place request where a transfer"), "transfer-only entry refuses in-place (" + err + ")");
        // 存档直传只暂存原始包，存档服务完成恢复后才能回成功。
        out = new ByteArrayOutputStream();
        try {
            req = ImportReceiver.receiveAny(new ByteArrayInputStream(stream(
                "S2A-SAVE 1 0100000000ABCDEF 4\n", "test", "END\n")), out, parent);
            check(req.dir.isFile() && Arrays.equals(Files.readAllBytes(req.dir.toPath()), "test".getBytes()),
                "save archive bytes reach the application staging directory");
            check("0100000000ABCDEF".equals(req.saveTitle), "save destination title passes through");
            check(out.toString().equals("READY 1\n"), "transport does not report a successful restore");
            Files.delete(req.dir.toPath());
        } catch (IOException e) {
            check(false, "save archive reaches the application staging directory (" + e.getMessage() + ")");
        }
        for (String[] bad : new String[][] {
            {"S2A-SAVE 1 ../../bad 4\ntestEND\n", "save_header"},
            {"S2A-SAVE 1 0100000000ABCDEF 99999999999\n", "save_size"},
            {"S2A-SAVE 1 0100000000ABCDEF 4\nte", "save_truncated"},
            {"S2A-SAVE 1 0100000000ABCDEF 4\ntestmoreEND\n", "save_trailing_data"}
        }) {
            out = new ByteArrayOutputStream();
            err = "";
            try { ImportReceiver.receiveAny(new ByteArrayInputStream(stream(bad[0])), out, parent); }
            catch (IOException error) { err = error.getMessage(); }
            check(err.equals(bad[1]) && out.toString().equals("READY 1\nERR " + bad[1] + "\n"),
                "rejects bad save frame (" + bad[1] + ")");
            try (var entries = Files.list(parent.toPath())) {
                check(entries.noneMatch(p -> p.getFileName().toString().endsWith(".s2asave")), "failed save leaves no archive");
            }
        }
        GameInstall.deleteTree(parent);
        if (failures > 0) { System.err.println(failures + " / " + checks + " failed"); System.exit(1); }
        System.out.println("ImportReceiver: ok (" + checks + " checks)");
    }
}
