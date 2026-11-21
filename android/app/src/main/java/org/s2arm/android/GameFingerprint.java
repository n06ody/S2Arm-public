package org.s2arm.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.FileTime;
import java.time.Instant;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.regex.Pattern;

/** Device-local performance cache, never the identity of the package itself.
 * An in-place modification that restores size and mtime and keeps the inode is not detected;
 * this relies on the app-private revision's single-writer contract, not on authentication.
 *
 * <p>原地来源（BootContract.IN_PLACE）：表仍在私有版本目录（revision），核对的是共享存储上的内容根（content）。
 * 共享存储没有单写者约定，玩家 / 别的 App 可以改：身份（大小、dev、inode、修改时间纳秒）任何一项变了就按索引重算
 * SHA-256，对不上具名拒（「in-place file changed」），缺了具名拒（「in-place file is missing」）。和私有目录一样，
 * 保留大小 / 修改时间 / inode 的原地改写测不出，这是缓存不是认证。所以原地来源的可执行文件（revision 来源模块的
 * image / fixups / gates / widen、exefs 下的 npdm：GameSession 给的 executables）每次起局都整份重算 SHA-256，不走身份命中；
 * 数据文件（romfs 等）被改等于玩家打 mod，照旧按身份命中。 */
final class GameFingerprint {
    private static final String PREFIX = ".s2a-fingerprint-";
    private static final Pattern TABLE = Pattern.compile("\\.s2a-fingerprint-[0-9a-f]{64}\\.tsv");
    private static final Pattern TEMP = Pattern.compile("\\.s2a-fingerprint-tmp-[0-9a-f]{32}\\.tsv");
    private static final Pattern HEX = Pattern.compile("[0-9a-f]{64}");
    private static final int MAX_TABLE = 128 << 20;
    interface StatProvider { Stamp read(File file) throws IOException; }
    interface StatProbe { void read(File file) throws IOException; }
    private static volatile StatProvider statProvider = GameFingerprint::nioStat;
    private static volatile StatProbe statProbe = file -> stat(file);
    static void setStatProbe(StatProbe probe) {
        if (probe == null) throw new IllegalArgumentException("missing stat probe");
        statProbe = probe;
    }
    static void setStatProvider(StatProvider provider) {
        if (provider == null) throw new IllegalArgumentException("missing stat provider");
        statProvider = provider;
    }

    static final class Stamp {
        final long size, dev, ino, seconds;
        final int nanos;
        Stamp(long size, long dev, long ino, long seconds, int nanos) {
            this.size=size; this.dev=dev; this.ino=ino; this.seconds=seconds; this.nanos=nanos;
        }
        boolean same(Stamp other) {
            return other != null && size == other.size && dev == other.dev && ino == other.ino &&
                   seconds == other.seconds && nanos == other.nanos;
        }
    }
    /** Persisted (size, dev, ino, mtime) stamp of the installed APK, bound to its measured SHA-256. */
    static final class ApkStamp {
        final String path, sha256;
        final long size, dev, ino, seconds;
        final int nanos;
        ApkStamp(String path, String sha256, long size, long dev, long ino, long seconds, int nanos) {
            this.path=path; this.sha256=sha256; this.size=size; this.dev=dev; this.ino=ino;
            this.seconds=seconds; this.nanos=nanos;
        }
        static ApkStamp of(File file, Stamp stamp, String sha256) {
            return new ApkStamp(file.getPath(), sha256, stamp.size, stamp.dev, stamp.ino, stamp.seconds, stamp.nanos);
        }
        boolean same(File file, Stamp stamp) {
            return stamp != null && path.equals(file.getPath()) && size == stamp.size && dev == stamp.dev
                   && ino == stamp.ino && seconds == stamp.seconds && nanos == stamp.nanos;
        }
        Map<String,Object> object() {
            Map<String,Object> value=new LinkedHashMap<>();
            value.put("path",path); value.put("sha256",sha256); value.put("size",size); value.put("dev",dev);
            value.put("ino",ino); value.put("seconds",seconds); value.put("nanos",(long)nanos);
            return value;
        }
    }
    static final class Reference {
        final String path, sha256, indexSha256;
        final ApkStamp apk;
        Reference(String path, String sha256, String indexSha256) { this(path, sha256, indexSha256, null); }
        Reference(String path, String sha256, String indexSha256, ApkStamp apk) {
            this.path=path; this.sha256=sha256; this.indexSha256=indexSha256; this.apk=apk;
        }
        Reference withApk(ApkStamp apk) { return new Reference(path, sha256, indexSha256, apk); }
        Map<String,Object> object() {
            Map<String,Object> value=new LinkedHashMap<>();
            value.put("version",1L); value.put("path",path); value.put("sha256",sha256); value.put("indexSha256",indexSha256);
            if (apk != null) value.put("apk",apk.object());
            return value;
        }
    }
    static final class Verification {
        final List<Stamp> stamps;
        final int hits, recalculated;
        final java.util.BitSet hashed;
        final boolean changed;
        Verification(List<Stamp> stamps, int hits, int recalculated, java.util.BitSet hashed, boolean changed) {
            this.stamps=stamps; this.hits=hits; this.recalculated=recalculated; this.changed=changed;
            this.hashed=hashed;
        }
    }
    /** Per-admission diagnostics. Constructed only when the debug log gate is enabled. */
    static final class Timing {
        static final int SAMPLE_INTERVAL = 128;
        private final Map<String, Long> phases = new LinkedHashMap<>();
        private final List<Long> paths = new ArrayList<>(), resolves = new ArrayList<>(), stats = new ArrayList<>();
        private final List<File> replayPaths = new ArrayList<>();
        private int pathErrors, resolveErrors, statErrors, replayCount, replayErrors;
        volatile long readyNanos;
        static long start(Timing timing) { return timing == null ? 0 : System.nanoTime(); }
        static void end(Timing timing, String phase, long start) {
            if (timing != null) timing.phases.put(phase, System.nanoTime() - start);
        }
        void sample(List<Long> values, long start) { values.add(System.nanoTime() - start); }
        private static String distribution(String name, List<Long> values, int errors) {
            List<Long> sorted = new ArrayList<>(values);
            java.util.Collections.sort(sorted);
            int n = sorted.size();
            long p50 = n == 0 ? 0 : sorted.get((n - 1) / 2);
            long p95 = n == 0 ? 0 : sorted.get((int) Math.ceil(n * 0.95) - 1);
            return "sample=" + name + " interval=" + SAMPLE_INTERVAL + " count=" + n
                + " errors=" + errors + " p50_ns=" + p50 + " p95_ns=" + p95;
        }
        List<String> lines() {
            List<String> lines = new ArrayList<>();
            for (Map.Entry<String, Long> e : phases.entrySet())
                lines.add("phase=" + e.getKey() + " ns=" + e.getValue());
            lines.add(distribution("under", paths, pathErrors));
            lines.add(distribution("resolve", resolves, resolveErrors));
            lines.add(distribution("stat", stats, statErrors));
            lines.add("replay=lstat count=" + replayCount + " errors=" + replayErrors);
            return lines;
        }
    }
    private GameFingerprint() { }

    // A null stamp means that this filesystem/JVM cannot provide trustworthy inode metadata.
    // Never turn an unsupported attribute into zero and then treat it as a cache hit.
    private static Stamp nioStat(File file) throws IOException {
        try {
            Map<String,Object> a=Files.readAttributes(file.toPath(), "unix:dev,ino,size,lastModifiedTime,isRegularFile", LinkOption.NOFOLLOW_LINKS);
            if (!Boolean.TRUE.equals(a.get("isRegularFile"))) throw new IOException("indexed target is not a regular file");
            Object d=a.get("dev"), i=a.get("ino"), s=a.get("size"), t=a.get("lastModifiedTime");
            if (!(d instanceof Number) || !(i instanceof Number) || !(s instanceof Number) || !(t instanceof FileTime)) return null;
            long ino=((Number)i).longValue(); if (ino == 0) return null;
            Instant m=((FileTime)t).toInstant();
            return new Stamp(((Number)s).longValue(),((Number)d).longValue(),ino,m.getEpochSecond(),m.getNano());
        } catch (UnsupportedOperationException | IllegalArgumentException unsupported) {
            return null;
        }
    }
    static Stamp stat(File file) throws IOException { return statProvider.read(file); }

    static boolean metadataName(String name) { return TABLE.matcher(name).matches() || TEMP.matcher(name).matches(); }
    static boolean metadataFile(Path path) { return metadataName(path.getFileName().toString()) && Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS); }
    static boolean reserved(List<GamePack.Entry> entries) {
        for (GamePack.Entry e: entries) if (e.path.startsWith(PREFIX)) return true;
        return false;
    }
    private static Reference reference(Object value, String indexSha) {
        try {
            Map<String,Object> r=Json.object(value,"fingerprint");
            if (!(r.get("version") instanceof Long) || ((Long)r.get("version")) != 1L ||
                !(r.get("path") instanceof String) || !(r.get("sha256") instanceof String) ||
                !(r.get("indexSha256") instanceof String)) return null;
            String path=(String)r.get("path"), sha=(String)r.get("sha256"), index=(String)r.get("indexSha256");
            if (!HEX.matcher(sha).matches() || !index.equals(indexSha) || !path.equals(PREFIX+sha+".tsv")) return null;
            return new Reference(path,sha,index,apkStamp(r.get("apk")));
        } catch (IllegalArgumentException ex) { return null; }
    }
    private static ApkStamp apkStamp(Object value) {
        try {
            Map<String,Object> a=Json.object(value,"fingerprint.apk");
            if (!(a.get("path") instanceof String) || !(a.get("sha256") instanceof String)
                || !(a.get("size") instanceof Long) || !(a.get("dev") instanceof Long)
                || !(a.get("ino") instanceof Long) || !(a.get("seconds") instanceof Long)
                || !(a.get("nanos") instanceof Long)) return null;
            String sha=(String)a.get("sha256");
            long nanos=(Long)a.get("nanos");
            if (!HEX.matcher(sha).matches() || nanos < 0 || nanos >= 1_000_000_000L) return null;
            return new ApkStamp((String)a.get("path"),sha,(Long)a.get("size"),(Long)a.get("dev"),
                (Long)a.get("ino"),(Long)a.get("seconds"),(int)nanos);
        } catch (IllegalArgumentException ex) { return null; }
    }
    static Reference currentReference(File gameRoot, String indexSha) throws IOException {
        try {
            Map<String,Object> current=Json.object(Json.parse(new String(Files.readAllBytes(new File(gameRoot,"current.json").toPath()),StandardCharsets.UTF_8)),"current");
            return reference(current.get("fingerprint"),indexSha);
        } catch (IllegalArgumentException e) { return null; }
    }

    private static List<Stamp> read(File revision, List<GamePack.Entry> entries, String indexSha, Reference ref) {
        if (ref == null || reserved(entries)) return null;
        Path p=revision.toPath().resolve(ref.path);
        try {
            if (!Files.isRegularFile(p,LinkOption.NOFOLLOW_LINKS) || Files.size(p)>MAX_TABLE) return null;
            MessageDigest digest=GamePack.digest();
            try (DigestInputStream input=new DigestInputStream(Files.newInputStream(p,LinkOption.NOFOLLOW_LINKS),digest);
                 BufferedReader rows=new BufferedReader(new InputStreamReader(input,StandardCharsets.UTF_8))) {
                if (!("s2a-boot-fingerprint-v1\t"+indexSha+"\t"+entries.size()).equals(rows.readLine())) return null;
                List<Stamp> stamps=new ArrayList<>(entries.size());
                for (int n=0;n<entries.size();n++) {
                    String row=rows.readLine(); if (row==null) return null;
                    Stamp stamp=parseRow(row,entries.get(n));
                    if (stamp==null) return null;
                    stamps.add(stamp);
                }
                if (rows.readLine()!=null || !GamePack.hex(digest.digest()).equals(ref.sha256)) return null;
                return stamps;
            }
        } catch (IOException | RuntimeException invalid) { return null; }
    }

    // A delta has already matched the old index by path, size and SHA. Read the old table
    // without retaining a second index, and trust matches only after verifying its whole digest.
    static Stamp[] readSelected(File revision, List<GamePack.Entry> entries, String oldIndexSha, Reference ref,
                                java.util.function.ToIntFunction<String> match) {
        if (ref == null || reserved(entries)) return null;
        Path path = revision.toPath().resolve(ref.path);
        try {
            if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS) || Files.size(path) > MAX_TABLE) return null;
            MessageDigest digest = GamePack.digest();
            try (DigestInputStream input = new DigestInputStream(Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS), digest);
                 BufferedReader rows = new BufferedReader(new InputStreamReader(input, StandardCharsets.UTF_8))) {
                String prefix = "s2a-boot-fingerprint-v1\t" + oldIndexSha + "\t";
                String header = rows.readLine();
                if (header == null || !header.startsWith(prefix)) return null;
                int remaining = Integer.parseInt(header.substring(prefix.length()));
                if (remaining < 0) return null;
                Stamp[] stamps = new Stamp[entries.size()];
                for (String row; (row = rows.readLine()) != null; ) {
                    int tab = row.indexOf('\t');
                    if (tab < 0 || --remaining < 0) return null;
                    int i = match.applyAsInt(row.substring(0, tab));
                    if (i < 0) continue;
                    Stamp stamp = parseRow(row, entries.get(i));
                    if (stamp == null || stamps[i] != null) return null;
                    stamps[i] = stamp;
                }
                return remaining == 0 && GamePack.hex(digest.digest()).equals(ref.sha256) ? stamps : null;
            }
        } catch (IOException | RuntimeException invalid) {
            return null;
        }
    }

    // One table row "path\tsize\tdev\tino\tsec\tnanos": single scan, no column array allocation.
    private static Stamp parseRow(String row, GamePack.Entry e) {
        String p=e.path;
        if (!row.startsWith(p) || row.length() < p.length()+2 || row.charAt(p.length())!='\t') return null;
        long[] v=new long[5]; int pos=p.length();
        for (int i=0;i<5;i++) {
            int from=pos+1;
            int to=i<4 ? row.indexOf('\t',from) : row.length();
            if (to<0 || to==from) return null;
            long n=0;
            for (int c=from;c<to;c++) {
                char ch=row.charAt(c);
                if (ch<'0'||ch>'9'||to-from>18) return null;
                n=n*10+(ch-'0');
            }
            v[i]=n; pos=to;
        }
        if (v[0]!=e.size || v[2]==0 || v[4]>=1_000_000_000L) return null;
        return new Stamp(v[0],v[1],v[2],v[3],(int)v[4]);
    }

    static Verification verify(File revision, List<GamePack.Entry> entries, String indexSha, Reference ref)
            throws IOException, BootContract.BadGame {
        return verify(revision, entries, indexSha, ref, null, null);
    }

    static Verification verify(File revision, List<GamePack.Entry> entries, String indexSha, Reference ref, Timing timing)
            throws IOException, BootContract.BadGame {
        return verify(revision, entries, indexSha, ref, timing, null);
    }

    static Verification verify(File revision, List<GamePack.Entry> entries, String indexSha, Reference ref,
                               Timing timing, String artifactName)
            throws IOException, BootContract.BadGame {
        return verify(revision, revision, entries, indexSha, ref, timing, artifactName);
    }

    // Single no-follow enumeration of the whole revision tree, replacing the previous
    // per-entry canonicalization loop plus the separate checkFileSet traversal. With
    // NOFOLLOW_LINKS an enumerated name maps 1:1 to its real on-disk path (directory
    // links reject instead of being traversed), so admission needs no realpath per file.
    // artifactName == null keeps install-time staging semantics: strays are ignored.
    // revision holds the table; content is the tree that is walked (the same directory unless in place).
    static Verification verify(File revision, File content, List<GamePack.Entry> entries, String indexSha, Reference ref,
                               Timing timing, String artifactName)
            throws IOException, BootContract.BadGame {
        return verify(revision, content, entries, indexSha, ref, timing, artifactName, java.util.Collections.emptyMap());
    }

    // executables：路径 → 名字（如「module main image」）。这些文件身份命中也整份重算，对不上按名字报
    static Verification verify(File revision, File content, List<GamePack.Entry> entries, String indexSha, Reference ref,
                               Timing timing, String artifactName, Map<String,String> executables)
            throws IOException, BootContract.BadGame {
        long phase = Timing.start(timing);
        List<Stamp> saved=read(revision,entries,indexSha,ref);
        Timing.end(timing, "fingerprint_table_read", phase);
        return verify(content, entries, saved, timing, artifactName, !content.equals(revision), executables);
    }

    // Install-time staging: placed[n] is the stamp of a file whose size and SHA-256 were checked
    // while it was placed (null: not checked by this install, so it is hashed here).
    static Verification verifyPlaced(File revision, List<GamePack.Entry> entries, List<Stamp> placed)
            throws IOException, BootContract.BadGame {
        return verifyPlaced(revision, false, entries, placed);
    }

    // 原地登记：content 是共享存储上的根，placed 是刚按索引核过 SHA-256 的文件身份
    static Verification verifyPlaced(File content, boolean inPlace, List<GamePack.Entry> entries, List<Stamp> placed)
            throws IOException, BootContract.BadGame {
        if (placed.size() != entries.size()) throw new IllegalArgumentException("placed stamps do not match the index");
        return verify(content, entries, placed, null, null, inPlace, java.util.Collections.emptyMap());
    }

    /** 按索引里的大小读够就停的 SHA-256：不为确认末尾多读一次（vivo 上经共享存储 FUSE 读大文件，「读到末尾」那次 read
     *  曾一直不返回，见 GameInstall.store）。读不够 size 字节返回 null（文件比索引短）。 */
    static String sha256Exact(File file, long size, java.util.function.LongConsumer progress) throws IOException {
        MessageDigest md=GamePack.digest();
        byte[] buf=new byte[1<<20];
        long got=0;
        try (java.io.InputStream in=new java.io.FileInputStream(file)) {
            for (int n; got<size && (n=in.read(buf,0,(int)Math.min(buf.length,size-got)))>0; ) {
                md.update(buf,0,n); got+=n;
                if (progress!=null) progress.accept(n);
            }
        }
        return got==size ? GamePack.hex(md.digest()) : null;
    }

    private static Verification verify(File revision, List<GamePack.Entry> entries, List<Stamp> saved,
                                       Timing timing, String artifactName, boolean inPlace,
                                       Map<String,String> executables)
            throws IOException, BootContract.BadGame {
        BootContract.UnderRoot scope=BootContract.underRoot(revision);
        long phase = Timing.start(timing);
        Map<String,Integer> ord=new LinkedHashMap<>(entries.size()*2);
        for (int n=0;n<entries.size();n++) ord.put(entries.get(n).path,n);
        Stamp[] fresh=new Stamp[entries.size()];
        boolean[] seen=new boolean[entries.size()];
        java.util.BitSet hashedRows = new java.util.BitSet(entries.size());
        int[] counters={0,0,0}; // hits, recalculated, seq
        boolean[] changed={saved==null};
        BootContract.BadGame[] badGame={null};
        Path root=revision.toPath();
        long walkPhase=Timing.start(timing);
        Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path path,
                    BasicFileAttributes attrs) throws IOException {
                if (attrs.isSymbolicLink() && Files.isDirectory(path))
                    throw new IOException("revision directory is a symbolic link: " + root.relativize(path));
                String name=root.relativize(path).toString().replace(File.separatorChar,'/');
                Integer n=ord.get(name);
                if (n==null && !name.contains("/") && attrs.isRegularFile() && metadataName(name))
                    return FileVisitResult.CONTINUE;
                if (n==null) {
                    if (artifactName!=null && !name.equals("manifest.json") && !name.equals("index.tsv")
                            && !name.equals(artifactName)) {
                        // 版本目录根上的索引外文件（换 artifact 时没删掉的旧 artifact 等）游戏读不到，不挡起局：跳过、记日志（不堵门）。
                        // 内容目录（romfs / exefs / systemdata …）里多出来的文件游戏读得到，照旧拒
                        if (!name.contains("/") && attrs.isRegularFile()) {
                            System.err.println("S2Arm: revision root file outside the content index ignored: "+name);
                            return FileVisitResult.CONTINUE;
                        }
                        throw new IOException((inPlace ? "in-place file is outside the content index: " : "revision file is outside the content index: ")+name);
                    }
                    return FileVisitResult.CONTINUE;
                }
                seen[n]=true;
                GamePack.Entry e=entries.get(n);
                boolean sampled=timing!=null && counters[2]++%Timing.SAMPLE_INTERVAL==0;
                long sampleStart=sampled?System.nanoTime():0;
                File file;
                try {
                    if (attrs.isSymbolicLink()) {
                        // CAS file links stay legal: a readlink + textual objects-shape match
                        // avoids realpath; anything else keeps canonical resolution.
                        file=BootContract.linkTarget(scope,path.getParent(),name,
                            Files.readSymbolicLink(path),"indexed file");
                    } else {
                        // Enumerated path is the real path (NOFOLLOW walk proved every
                        // ancestor a real directory); no path resolution needed at all.
                        file=path.toFile();
                    }
                } catch (BootContract.BadGame error) {
                    badGame[0]=error; return FileVisitResult.TERMINATE;
                } catch (IOException | RuntimeException error) {
                    if (sampled) timing.resolveErrors++;
                    if (error instanceof IOException) throw (IOException)error;
                    throw (RuntimeException)error;
                } finally { if (sampled) timing.sample(timing.resolves,sampleStart); }
                Stamp before;
                sampleStart=sampled?System.nanoTime():0;
                try {
                    // NOFOLLOW stat still refuses a final component swapped to a symlink
                    // meanwhile (S_ISREG inside the provider).
                    before=stat(file);
                } catch (IOException | RuntimeException error) {
                    if (sampled) timing.statErrors++;
                    if (error instanceof IOException) throw (IOException)error;
                    throw (RuntimeException)error;
                } finally { if (sampled) timing.sample(timing.stats,sampleStart); }
                if (before != null ? before.size != e.size : (!file.isFile() || file.length()!=e.size))
                    throw new IOException(inPlace ? "in-place file changed (size differs from index.tsv): "+e.path
                                                  : "revision file fails size / SHA-256: "+e.path);
                if (timing != null) timing.replayPaths.add(file);
                boolean hit=saved!=null && before!=null && before.same(saved.get(n));
                String executable=executables.get(e.path);
                if (hit && executable==null) {
                    counters[0]++;
                } else {
                    hashedRows.set(n);
                    counters[1]++; if (!hit) changed[0]=true;
                    // Full recalculation still resolves through under(): hashing opens the
                    // canonical file, same as the old per-entry flow.
                    File hashed;
                    sampleStart=sampled?System.nanoTime():0;
                    try { hashed=BootContract.under(scope,e.path,"indexed file"); }
                    catch (BootContract.BadGame error) {
                        badGame[0]=error; return FileVisitResult.TERMINATE;
                    } catch (IOException | RuntimeException error) {
                        if (sampled) timing.pathErrors++;
                        if (error instanceof IOException) throw (IOException)error;
                        throw (RuntimeException)error;
                    } finally { if (sampled) timing.sample(timing.paths,sampleStart); }
                    if (!e.sha256.equals(sha256Exact(hashed,e.size,null)))
                        throw new IOException(executable != null ? "in-place executable changed (SHA-256 differs from index.tsv): "+executable+" "+e.path
                                            : inPlace ? "in-place file changed (SHA-256 differs from index.tsv): "+e.path
                                                      : "revision file fails size / SHA-256: "+e.path);
                    Stamp after=stat(hashed);
                    if (before!=null && !before.same(after))
                        throw new IOException("revision file changed while hashing: "+e.path);
                    before=after;
                }
                fresh[n]=before;
                return FileVisitResult.CONTINUE;
            }
        });
        Timing.end(timing,"verify_walk",walkPhase);
        if (badGame[0]!=null) throw badGame[0];
        for (int n=0;n<entries.size();n++)
            if (!seen[n]) throw new IOException(inPlace ? "in-place file is missing: "+entries.get(n).path+" (under "+revision+")"
                                                        : "revision file fails size / SHA-256: "+entries.get(n).path);
        Timing.end(timing,"fingerprint_verify_loop",phase);
        return new Verification(java.util.Arrays.asList(fresh),counters[0],counters[1],hashedRows,changed[0]);
    }

    // Replays only already-resolved paths, outside the normal resource timer. Its result is
    // diagnostic only; the preceding verification remains the sole admission decision.
    static void replayStats(Timing timing) {
        if (timing == null) return;
        long start = System.nanoTime();
        for (File file : timing.replayPaths) {
            timing.replayCount++;
            try { statProbe.read(file); }
            catch (IOException | RuntimeException error) { timing.replayErrors++; }
        }
        Timing.end(timing, "lstat_replay", start);
        timing.replayPaths.clear();
    }

    static Reference write(File revision, List<GamePack.Entry> entries, String indexSha, List<Stamp> stamps) throws IOException {
        if (reserved(entries) || stamps.size()!=entries.size()) return null;
        for (Stamp s : stamps) if (s==null) return null;
        // 边写边算 SHA-256，不在内存里拼整张表：大包 30 万行约 32 MB，拼成一个 String 再取字节会撑爆
        //   App 的 256 MB Java 堆（11-14 真机导入收尾 OutOfMemoryError，进程崩、导入作废）。
        // Own the temporary name format so interrupted writes remain recognizable on Android
        // regardless of the platform's createTempFile implementation.
        Path temp=revision.toPath().resolve(PREFIX+"tmp-"+UUID.randomUUID().toString().replace("-","")+".tsv");
        Files.createFile(temp);
        try {
            MessageDigest md=GamePack.digest();
            long size;
            try (FileOutputStream file=new FileOutputStream(temp.toFile());
                 java.security.DigestOutputStream digest=new java.security.DigestOutputStream(file,md);
                 java.io.BufferedOutputStream buf=new java.io.BufferedOutputStream(digest,1<<16)) {
                byte[] head=("s2a-boot-fingerprint-v1\t"+indexSha+"\t"+entries.size()+"\n").getBytes(StandardCharsets.UTF_8);
                buf.write(head); size=head.length;
                StringBuilder line=new StringBuilder(256);
                for (int i=0;i<entries.size();i++) {
                    Stamp s=stamps.get(i);
                    line.setLength(0);
                    line.append(entries.get(i).path).append('\t').append(s.size).append('\t').append(s.dev).append('\t')
                        .append(s.ino).append('\t').append(s.seconds).append('\t').append(s.nanos).append('\n');
                    byte[] bytes=line.toString().getBytes(StandardCharsets.UTF_8);
                    size+=bytes.length;
                    if (size>MAX_TABLE) return null;
                    buf.write(bytes);
                }
                buf.flush(); file.getFD().sync();
            }
            String sha=GamePack.hex(md.digest()), name=PREFIX+sha+".tsv";
            Path target=revision.toPath().resolve(name);
            if (Files.exists(target,LinkOption.NOFOLLOW_LINKS)) {
                if (!Files.isRegularFile(target,LinkOption.NOFOLLOW_LINKS))
                    throw new IOException("fingerprint target is not a regular file");
                if (Files.size(target)<=MAX_TABLE && RuntimeLibrary.sha256(target.toFile()).equals(sha))
                    return new Reference(name,sha,indexSha);
            }
            Files.move(temp,target,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
            return new Reference(name,sha,indexSha);
        } finally { Files.deleteIfExists(temp); }
    }

    // Called only under GameStorage. A SHA-named new table is in place before current switches.
    static void publish(File gameRoot, Reference ref) throws IOException {
        Path current=new File(gameRoot,"current.json").toPath();
        Map<String,Object> doc;
        try { doc=Json.object(Json.parse(new String(Files.readAllBytes(current),StandardCharsets.UTF_8)),"current"); }
        catch (IllegalArgumentException e) { throw new IOException("invalid current metadata",e); }
        if (ref==null) doc.remove("fingerprint"); else doc.put("fingerprint",ref.object());
        Path tmp=new File(gameRoot,"current.json.tmp").toPath();
        if (Files.exists(tmp,LinkOption.NOFOLLOW_LINKS) && !Files.isRegularFile(tmp,LinkOption.NOFOLLOW_LINKS))
            throw new IOException("current temporary file is not regular");
        try (FileOutputStream out=new FileOutputStream(tmp.toFile())) {
            out.write(json(doc).getBytes(StandardCharsets.UTF_8)); out.getFD().sync();
        }
        Files.move(tmp,current,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    static void clean(File revision, Reference keep) throws IOException {
        try (DirectoryStream<Path> paths=Files.newDirectoryStream(revision.toPath(),PREFIX+"*")) {
            for (Path p:paths) {
                String name=p.getFileName().toString();
                if (!metadataName(name)) continue;
                if (!Files.isRegularFile(p,LinkOption.NOFOLLOW_LINKS)) throw new IOException("fingerprint metadata is not a regular file");
                if (keep==null || !name.equals(keep.path)) Files.delete(p);
            }
        }
    }
    private static String json(Object value) {
        if (value==null) return "null";
        if (value instanceof String) {
            StringBuilder b=new StringBuilder("\"");
            for (char c:((String)value).toCharArray()) {
                switch(c) { case '"': b.append("\\\""); break; case '\\': b.append("\\\\"); break;
                    case '\b': b.append("\\b"); break; case '\f': b.append("\\f"); break;
                    case '\n': b.append("\\n"); break; case '\r': b.append("\\r"); break; case '\t': b.append("\\t"); break;
                    default: if(c<0x20) b.append(String.format("\\u%04x",(int)c)); else b.append(c); }
            }
            return b.append('"').toString();
        }
        if (value instanceof Number || value instanceof Boolean) return value.toString();
        if (value instanceof List) {
            StringBuilder b=new StringBuilder("["); boolean first=true;
            for(Object v:(List<?>)value) { if(!first)b.append(','); first=false; b.append(json(v)); }
            return b.append(']').toString();
        }
        if (value instanceof Map) {
            StringBuilder b=new StringBuilder("{"); boolean first=true;
            for(Map.Entry<?,?> e:((Map<?,?>)value).entrySet()) { if(!first)b.append(','); first=false; b.append(json((String)e.getKey())).append(':').append(json(e.getValue())); }
            return b.append('}').toString();
        }
        throw new IllegalArgumentException("unsupported JSON value");
    }
}
