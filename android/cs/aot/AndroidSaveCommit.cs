// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using Ryujinx.HLE.FileSystem;
using S2A.Services;
using S2A.Android.SaveCommit;
using System.Runtime.CompilerServices;

namespace S2A.Android.Aot;

// Runs before any entry point in the Android AOT composition can call Host.Boot.
// Platform selection is explicit here, never discovered in the shared filesystem.
internal static class AndroidSaveCommit
{
    [ModuleInitializer]
    internal static void Init()
    {
        S2AHostFileSystem.Transaction = new AndroidDirectoryTransaction();
        // The library imports saves before the game session installs its console tee.
        SaveTransfer.DiagnosticLog = LogcatTee.WriteError;
    }
}
