// SPDX-License-Identifier: GPL-3.0-or-later
package org.s2arm.android;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;

/** Persists a real private directory without following a replaced final path component. */
final class AndroidDirectorySync {
    private AndroidDirectorySync() { }

    static void sync(File directory) throws IOException {
        try {
            FileDescriptor fd = Os.open(directory.getPath(),
                OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW | OsConstants.O_CLOEXEC, 0);
            try {
                if (!OsConstants.S_ISDIR(Os.fstat(fd).st_mode)) throw new IOException("sync target is not a directory");
                Os.fsync(fd);
            } finally {
                Os.close(fd);
            }
        } catch (ErrnoException error) {
            throw new IOException("cannot sync object install directory", error);
        }
    }
}
