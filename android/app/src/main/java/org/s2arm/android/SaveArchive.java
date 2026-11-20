// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;

/** Stages one archive for the shared save service without exposing a caller-supplied path. */
final class SaveArchive {
    // SaveTransferArchive.cs: MaxBytes + ZIP overhead.
    static final long MAX_BYTES = (32L << 30) + (64L << 20);

    private SaveArchive() { }

    /** A size of -1 reads a SAF document to EOF. A framed transfer reads exactly its declared size. */
    static Path stage(Path directory, InputStream source, long size) throws IOException {
        if (size < -1 || size == 0 || size > MAX_BYTES) throw new IOException("save_size");
        Files.createDirectories(directory);
        Path archive = Files.createTempFile(directory, ".save-", ".s2asave");
        try {
            long count = 0;
            byte[] buffer = new byte[1 << 20];
            try (OutputStream output = Files.newOutputStream(archive)) {
                while (size == -1 || count < size) {
                    int limit = size == -1 ? buffer.length : (int) Math.min(buffer.length, size - count);
                    int read = source.read(buffer, 0, limit);
                    if (read < 0) {
                        if (size != -1) throw new EOFException("save_truncated");
                        break;
                    }
                    if (read == 0) throw new IOException("save_read");
                    count += read;
                    if (count > MAX_BYTES) throw new IOException("save_size");
                    output.write(buffer, 0, read);
                }
                if (count == 0) throw new IOException("save_size");
            }
            return archive;
        } catch (IOException | RuntimeException error) {
            try { Files.deleteIfExists(archive); }
            catch (IOException cleanup) { error.addSuppressed(cleanup); }
            throw error;
        }
    }
}
