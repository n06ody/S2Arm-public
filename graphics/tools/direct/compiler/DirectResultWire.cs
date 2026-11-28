// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;

namespace S2A.DirectCompiler
{
    // A result-only interface. The request and persisted s2a-direct-program-v1/v2
    // still use the canonical JSON writer. See nvk_result_view.h for the wire layout.
    internal sealed class DirectResultWriter : IDisposable
    {
        internal enum Type : byte { Object = 1, Array, Text, Unsigned, Signed, False, True, Null, Spirv }
        internal static ReadOnlySpan<byte> Magic => "S2ADRB1\0"u8;
        internal const int HeaderSize = 16;
        internal const int MaxSize = 64 << 20;
        private static readonly UTF8Encoding Utf8 = new(false, true);

        private static class Fields
        {
            internal static readonly byte[][] Names = Load();
            internal static readonly Dictionary<string, byte> Ids = Index();

            private static byte[][] Load()
            {
                using var stream = typeof(DirectResultWriter).Assembly.GetManifestResourceStream("S2A.DirectCompiler.ResultFields")
                    ?? throw new InvalidDataException("missing live result field table");
                using var reader = new StreamReader(stream);
                var names = new List<byte[]> { Array.Empty<byte>() };
                const string prefix = "RESULT_FIELD(\"";
                const string suffix = "\")";
                while (reader.ReadLine() is string line)
                {
                    if (line.StartsWith(prefix, StringComparison.Ordinal) && line.EndsWith(suffix, StringComparison.Ordinal))
                        names.Add(Utf8.GetBytes(line[prefix.Length..^suffix.Length]));
                }
                if (names.Count < 2 || names.Count > 256) throw new InvalidDataException("invalid live result field table");
                return names.ToArray();
            }

            private static Dictionary<string, byte> Index()
            {
                var ids = new Dictionary<string, byte>(StringComparer.Ordinal);
                for (int i = 1; i < Names.Length; i++)
                    ids.Add(Utf8.GetString(Names[i]), (byte)i);
                return ids;
            }
        }


        private readonly MemoryStream _stream = new();
        private readonly Utf8JsonWriter _json;
        private readonly Stack<int> _containers;
        private readonly bool _frame;
        private string _property;

        public DirectResultWriter(bool binary, bool frame = false)
        {
            _frame = frame;
            if (binary)
            {
                _containers = new Stack<int>();
                if (frame)
                {
                    _stream.Write(Magic);
                    _stream.Write(new byte[8]);
                }
            }
            else
                _json = new Utf8JsonWriter(_stream, new JsonWriterOptions { Indented = false });
        }

        private static int Pad(int n) => checked((n + 3) & ~3);

        private Span<byte> Append(int length)
        {
            int start = checked((int)_stream.Position);
            int end = checked(start + length);
            if (end > MaxSize) throw new DirectCompileException("RESOURCE_LIMIT", "live result exceeds byte limit");
            _stream.SetLength(end);
            _stream.Position = end;
            return _stream.GetBuffer().AsSpan(start, length);
        }

        private int Token(Type kind, int length)
        {
            string key = _property;
            _property = null;
            int offset = checked((int)_stream.Position);
            byte field = key == null ? (byte)0 : Fields.Ids[key];
            Span<byte> token = Append(8);
            token.Clear();
            BinaryPrimitives.WriteUInt32LittleEndian(token, (uint)length);
            token[4] = (byte)kind;
            token[5] = field;
            return offset;
        }

        public void WritePropertyName(string name)
        {
            if (_json != null) _json.WritePropertyName(name);
            else _property = name;
        }

        private void Start(Type kind)
        {
            if (_json == null) _containers.Push(Token(kind, 0));
            else if (kind == Type.Object) _json.WriteStartObject();
            else _json.WriteStartArray();
        }

        private void End()
        {
            int start = _containers.Pop();
            int length = checked((int)_stream.Position - start - 8);
            BinaryPrimitives.WriteUInt32LittleEndian(_stream.GetBuffer().AsSpan(start), (uint)length);
        }

        public void WriteStartObject() => Start(Type.Object);
        public void WriteStartObject(string name) { WritePropertyName(name); WriteStartObject(); }
        public void WriteStartArray() => Start(Type.Array);
        public void WriteStartArray(string name) { WritePropertyName(name); WriteStartArray(); }
        public void WriteEndObject() { if (_json != null) _json.WriteEndObject(); else End(); }
        public void WriteEndArray() { if (_json != null) _json.WriteEndArray(); else End(); }

        public void WriteString(string name, string value)
        {
            if (_json != null) { _json.WriteString(name, value); return; }
            WritePropertyName(name);
            if (value == null) { WriteNullValue(); return; }
            int length = Utf8.GetByteCount(value);
            Token(Type.Text, length);
            Span<byte> text = Append(Pad(length));
            Utf8.GetBytes(value, text[..length]);
            text[length..].Clear();
        }

        public void WriteBoolean(string name, bool value)
        {
            if (_json != null) { _json.WriteBoolean(name, value); return; }
            WritePropertyName(name);
            Token(value ? Type.True : Type.False, 0);
        }

        public void WriteNullValue()
        {
            if (_json != null) _json.WriteNullValue();
            else Token(Type.Null, 0);
        }

        private void Number(Type kind, ulong bits)
        {
            bool use32 = kind == Type.Signed ? unchecked((long)bits) >= int.MinValue : bits <= uint.MaxValue;
            Token(kind, use32 ? 4 : 8);
            if (use32) BinaryPrimitives.WriteUInt32LittleEndian(Append(4), unchecked((uint)bits));
            else BinaryPrimitives.WriteUInt64LittleEndian(Append(8), bits);
        }

        public void WriteNumber(string name, uint value) => WriteNumber(name, (ulong)value);
        public void WriteNumber(string name, ulong value)
        {
            if (_json != null) { _json.WriteNumber(name, value); return; }
            WritePropertyName(name);
            Number(Type.Unsigned, value);
        }

        public void WriteNumber(string name, int value)
        {
            if (_json != null) { _json.WriteNumber(name, value); return; }
            WritePropertyName(name);
            WriteNumberValue(value);
        }

        public void WriteNumberValue(int value)
        {
            if (_json != null) _json.WriteNumberValue(value);
            else Number(value < 0 ? Type.Signed : Type.Unsigned, unchecked((ulong)value));
        }

        public void WriteBase64String(string name, ReadOnlySpan<byte> value)
        {
            if (_json != null) { _json.WriteBase64String(name, value); return; }
            WritePropertyName(name);
            if ((value.Length & 3) != 0) throw new InvalidOperationException("unaligned SPIR-V");
            Token(Type.Spirv, value.Length);
            value.CopyTo(Append(value.Length));
        }

        public void WriteRawValue(ReadOnlySpan<byte> value)
        {
            if (_json != null) { _json.WriteRawValue(value); return; }
            // WriteContent supplied an unnamed object token. Its payload is already
            // a validated sequence of records; replace just the outer property name.
            Token(Type.Object, value.Length - 8);
            value[8..].CopyTo(Append(value.Length - 8));
        }

        public byte[] Finish()
        {
            _json?.Flush();
            if (_frame)
                BinaryPrimitives.WriteUInt32LittleEndian(_stream.GetBuffer().AsSpan(8), (uint)_stream.Length);
            return _stream.ToArray();
        }

        public void Dispose()
        {
            _json?.Dispose();
            _stream.Dispose();
        }

        private readonly ref struct Record
        {
            public readonly Type Kind;
            public readonly ReadOnlySpan<byte> Key, Payload;
            public readonly int Total;

            public Record(ReadOnlySpan<byte> source)
            {
                if (source.Length < 8 || source[6] != 0 || source[7] != 0)
                    throw new InvalidDataException("truncated result record");
                int length = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(source));
                int field = source[5];
                Total = checked(8 + Pad(length));
                if (field >= Fields.Names.Length || Total > source.Length)
                    throw new InvalidDataException("result record extent invalid");
                Kind = (Type)source[4];
                Key = Fields.Names[field];
                Payload = source.Slice(8, length);
            }
        }

        // Runs on the existing cache writer. Recreate the canonical JSON writer's
        // property order and content/SPIR-V digests from the same immutable result.
        internal static byte[] LegacyJson(ReadOnlySpan<byte> frame)
        {
            if (frame.Length < HeaderSize || frame.Length > MaxSize || !frame[..8].SequenceEqual(Magic) ||
                BinaryPrimitives.ReadUInt32LittleEndian(frame[8..]) != frame.Length ||
                BinaryPrimitives.ReadUInt32LittleEndian(frame[12..]) != 0)
                throw new InvalidDataException("live result header invalid");
            var root = new Record(frame[HeaderSize..]);
            if (root.Kind != Type.Object || !root.Key.IsEmpty || root.Total != frame.Length - HeaderSize)
                throw new InvalidDataException("live result envelope invalid");
            using var stream = new MemoryStream();
            using (var writer = new Utf8JsonWriter(stream))
                WriteLegacy(writer, root, 0);
            return stream.ToArray();
        }

        private static void WriteLegacy(Utf8JsonWriter writer, Record record, int depth)
        {
            if (depth > 24) throw new InvalidDataException("live result nesting exceeds limit");
            if (!record.Key.IsEmpty)
                writer.WritePropertyName(record.Key);
            switch (record.Kind)
            {
                case Type.Object:
                case Type.Array:
                    if (record.Kind == Type.Object) writer.WriteStartObject(); else writer.WriteStartArray();
                    var items = record.Payload;
                    while (!items.IsEmpty)
                    {
                        var item = new Record(items);
                        if (depth == 0 && item.Key.SequenceEqual("content"u8))
                        {
                            using var stream = new MemoryStream();
                            using (var content = new Utf8JsonWriter(stream))
                            {
                                content.WriteStartObject();
                                var members = item.Payload;
                                while (!members.IsEmpty)
                                {
                                    var member = new Record(members);
                                    WriteLegacy(content, member, depth + 2);
                                    members = members[member.Total..];
                                }
                                content.WriteEndObject();
                            }
                            ReadOnlySpan<byte> bytes = stream.GetBuffer().AsSpan(0, (int)stream.Length);
                            writer.WriteString("contentHash", "sha256-" + Convert.ToHexStringLower(Sha256.HashData(bytes)));
                            writer.WritePropertyName("content");
                            writer.WriteRawValue(bytes);
                        }
                        else
                            WriteLegacy(writer, item, depth + 1);
                        items = items[item.Total..];
                    }
                    if (record.Kind == Type.Object) writer.WriteEndObject(); else writer.WriteEndArray();
                    break;
                case Type.Text: writer.WriteStringValue(record.Payload); break;
                case Type.Unsigned: writer.WriteNumberValue(record.Payload.Length == 4 ? BinaryPrimitives.ReadUInt32LittleEndian(record.Payload) : BinaryPrimitives.ReadUInt64LittleEndian(record.Payload)); break;
                case Type.Signed: writer.WriteNumberValue(record.Payload.Length == 4 ? BinaryPrimitives.ReadInt32LittleEndian(record.Payload) : BinaryPrimitives.ReadInt64LittleEndian(record.Payload)); break;
                case Type.True: writer.WriteBooleanValue(true); break;
                case Type.False: writer.WriteBooleanValue(false); break;
                case Type.Null: writer.WriteNullValue(); break;
                case Type.Spirv:
                    if (!record.Key.SequenceEqual("spirv"u8)) throw new InvalidDataException("unexpected binary result field");
                    writer.WriteBase64StringValue(record.Payload);
                    writer.WriteString("spirvSha256", Convert.ToHexStringLower(Sha256.HashData(record.Payload)));
                    break;
                default: throw new InvalidDataException("unknown live result record type");
            }
        }
    }
}
