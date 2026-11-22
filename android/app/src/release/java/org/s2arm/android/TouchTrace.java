// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import java.io.File;
import java.io.IOException;
import java.util.function.Consumer;

/** Build-time counterpart for MainActivity; the writer exists only in debug. */
final class TouchTrace {
    static TouchTrace open(File files, int pid, Consumer<IOException> failure) { return null; }
    void record(String layout, String input) { }
}
