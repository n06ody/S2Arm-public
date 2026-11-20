package org.s2arm.android;

import java.io.File;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

/** Cross-process exclusion: installation/sweep and a live game cannot retire each other's revision.
 *  一款游戏一把锁（games/<程序号>.lock，不堵门）：装 / 清某一款只会动这一款的版本目录，原先整库一把锁让任何一款在跑时别的游戏都导不进来。 */
final class GameStorage implements AutoCloseable {
    static final class Busy extends IOException {
        Busy() { super("this game is busy (running or being installed); try again after it exits"); }
    }

    private static final java.util.Set<Path> HELD = new java.util.HashSet<>();
    private final FileChannel channel;
    private final FileLock lock;
    private final Path key;
    private boolean closed;

    private GameStorage(FileChannel channel, FileLock lock, Path key) { this.channel = channel; this.lock = lock; this.key = key; }

    /** The lock of one game: privateRoot/games/<titleId>.lock. */
    static GameStorage acquire(File privateRoot, String titleId) throws IOException {
        return acquireIn(new File(privateRoot.getCanonicalFile(), "games"), titleId);
    }

    static synchronized GameStorage acquireIn(File gamesDir, String titleId) throws IOException {
        if (titleId == null || !titleId.matches("[0-9A-F]{16}")) throw new IOException("invalid game identity for the storage lock");
        Path games = gamesDir.getCanonicalFile().toPath();
        if (Files.isSymbolicLink(games)) throw new IOException("games directory is a symbolic link");
        Files.createDirectories(games);
        Path key = games.resolve(titleId + ".lock");
        // POSIX locks belong to the process: even closing an unsuccessful second fd can release the
        // first channel's OS lock. Refuse local contention BEFORE opening another descriptor.
        if (HELD.contains(key)) throw new Busy();
        FileChannel channel = FileChannel.open(key, StandardOpenOption.CREATE,
            StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
        try {
            FileLock lock = channel.tryLock();
            if (lock == null) throw new Busy();
            HELD.add(key);
            return new GameStorage(channel, lock, key);
        } catch (IOException | RuntimeException error) {
            channel.close();
            if (error instanceof OverlappingFileLockException)
                throw new Busy();
            throw error;
        }
    }

    @Override public void close() throws IOException {
        synchronized (GameStorage.class) {
            if (closed) return;
            closed = true;
            try { try { lock.release(); } finally { channel.close(); } }
            finally { HELD.remove(key); }
        }
    }
}
