// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace S2A.DshVkLib;

internal static class StagePack
{
    [DllImport("s2astage", CallingConvention = CallingConvention.Cdecl)]
    private static extern int s2a_pack_stage(byte[] key, uint stage, int topology, ulong fingerprint,
        byte[] reflection, nuint reflectionSize, byte[] spirv, nuint spirvSize, out IntPtr record, out nuint size);
    [DllImport("s2astage", CallingConvention = CallingConvention.Cdecl)]
    private static extern void s2a_pack_stage_free(IntPtr record);

    internal static byte[] Read(string directory, string name, ulong fingerprint)
    {
        byte[] json = File.ReadAllBytes(Path.Combine(directory, Path.ChangeExtension(name, ".json")));
        byte[] spv = File.ReadAllBytes(Path.Combine(directory, name));
        using var doc = JsonDocument.Parse(json);
        uint stage = doc.RootElement.GetProperty("stage").GetString() switch {
            "vs" => 0, "ps" => 1, "gs" => 2, "cs" => 5, _ => throw new InvalidDataException("Unsupported offline shader stage")
        };
        int topology = name.AsSpan(17).StartsWith("vg") ? name[19] - '0' : -1;
        if ((stage == 2) != (topology >= 0) || topology > 4)
            throw new InvalidDataException("Offline shader topology does not match filename");
        byte[] key = Convert.FromHexString(name[..16]);
        int status = s2a_pack_stage(key, stage, topology, fingerprint, json, (nuint)json.Length,
            spv, (nuint)spv.Length, out IntPtr bytes, out nuint length);
        if (status != 0 || bytes == IntPtr.Zero) throw new InvalidDataException("Cannot encode stage " + name);
        try {
            var result = new byte[checked((int)length)];
            Marshal.Copy(bytes, result, 0, result.Length);
            return result;
        } finally { s2a_pack_stage_free(bytes); }
    }
}
