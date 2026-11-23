// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace S2A.DshVkLib;

internal static class Repack
{
    // Change the representation without assigning a new translator identity to existing SPIR-V.
    internal static string FromLegacy(string source, string destination)
    {
        source = Path.GetFullPath(source);
        destination = Path.GetFullPath(destination);
        if (Directory.Exists(destination) || File.Exists(destination))
            throw new IOException("Destination already exists: " + destination);
        byte[] index = File.ReadAllBytes(Path.Combine(source, "lib.idx"));
        uint U32(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(index.AsSpan(offset));
        ulong U64(int offset) => BinaryPrimitives.ReadUInt64LittleEndian(index.AsSpan(offset));
        if (index.Length < 48 || !index.AsSpan(0, 8).SequenceEqual("S2ALIB\0\0"u8) ||
            U32(8) != 1 || U32(44) != 0)
            throw new InvalidDataException("Expected a legacy Vulkan library index");
        int count = checked((int)U32(12)), shards = checked((int)U32(16));
        if (shards > 65536 || count == 0 || index.LongLength != 48L + 16L * shards + 24L * count)
            throw new InvalidDataException("Invalid record or shard count");
        string chain = Encoding.ASCII.GetString(index, 24, 12).TrimEnd('\0');
        ulong fingerprint = U64(36);
        var records = new Dictionary<string, (int Shard, long Offset, int Length)>(StringComparer.Ordinal);
        (ulong Key, int Kind) previous = default;
        for (int i = 0; i < count; i++) {
            int p = 48 + 16 * shards + 24 * i;
            ulong key = U64(p);
            int kind = index[p + 8], shard = index[p + 9] | index[p + 10] << 8 | index[p + 11] << 16;
            ulong offset = U64(p + 16);
            uint length = U32(p + 12);
            if (shard >= shards || offset > U64(48 + 16 * shard) || length > U64(48 + 16 * shard) - offset ||
                (i > 0 && (key < previous.Key || (key == previous.Key && kind <= previous.Kind))))
                throw new InvalidDataException("Invalid bounds or key ordering at record " + i);
            previous = (key, kind);
            string suffix = kind switch {
                1 => "_b.json", 52 => "_b.spv",
                >= 112 and <= 116 => $"_vg{kind-112}.json",
                >= 120 and <= 124 => $"_vg{kind-120}.spv",
                _ => throw new InvalidDataException("Unsupported legacy Vulkan record kind " + kind)
            };
            records.Add(key.ToString("x16") + suffix, (shard, checked((long)offset), checked((int)length)));
        }
        byte[] Read(string name) {
            var r = records[name];
            using var stream = File.OpenRead(Path.Combine(source, $"lib.{r.Shard:D3}.pak"));
            if ((ulong)stream.Length != U64(48 + 16 * r.Shard))
                throw new InvalidDataException("Shard size differs from index");
            stream.Position = r.Offset;
            byte[] payload = new byte[r.Length];
            stream.ReadExactly(payload);
            return payload;
        }
        string staging = destination + ".partial-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(staging);
        try {
            var (bad, line) = LibPack.PackFiles(records.Keys, Read, staging, chain, fingerprint.ToString("x16"),
                vkStage: n => StagePack.Encode(n, fingerprint, Read(Path.ChangeExtension(n, ".json")), Read(n)));
            if (bad.Count != 0) throw new InvalidDataException(string.Join(Environment.NewLine, bad));
            Directory.Move(staging, destination);
            return line;
        } finally {
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
        }
    }
}
