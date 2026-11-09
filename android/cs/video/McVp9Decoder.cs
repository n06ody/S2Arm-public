#nullable disable
using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Video;
using System;
using System.Runtime.InteropServices;

namespace Ryujinx.Graphics.Nvdec.Android
{
    // AMediaCodec VP9 硬件解码（#44）：s2a_mc shim 的薄封装——进一个重打包帧、出图直接写进
    // McSurface 持有的三平面（NV12 在这里解成 I420 形）。同步契约下的管线延迟由
    // McVp9Backend 用 Wait 排空补齐，这一层只管「进 AU / 排空 / 拿诊断」。
    internal sealed unsafe partial class McVp9Decoder : IDisposable
    {
        private const string Lib = "s2arm_android";
        private const uint CodecVp9 = 1;
        [StructLayout(LayoutKind.Sequential)]
        internal struct Config { public uint Abi, Size, Codec, Width, Height, Reserved; }
        [StructLayout(LayoutKind.Sequential)]
        internal struct Diagnostic
        {
            public uint Abi, Size, Stage;
            public int Code, MediaStatus;
            public uint Consumed, Decoded, Output, InputToken, OutputToken, Width, Height, Generation;
            public int ColorFormat;
            public uint OutStride, OutSliceH;
            public static Diagnostic New() => new() { Abi = 1, Size = (uint)sizeof(Diagnostic) };
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct DstPlane
        {
            public nint Data;
            public ulong Capacity;
            public uint Stride, Reserved;
        }
        [LibraryImport(Lib)] private static partial int s2a_mc_probe(uint abi, uint codec, uint width, uint height, ref Diagnostic diagnostic);
        [LibraryImport(Lib)] private static partial int s2a_mc_create(in Config config, out nint handle, ref Diagnostic diagnostic);
        [LibraryImport(Lib)] private static partial int s2a_mc_decode(nint handle, byte* au, uint length, uint width, uint height, DstPlane* dst, uint token, ref Diagnostic diagnostic);
        [LibraryImport(Lib)] private static partial int s2a_mc_wait(nint handle, DstPlane* dst, uint budgetUs, ref Diagnostic diagnostic);
        [LibraryImport(Lib)] private static partial int s2a_mc_destroy(nint handle, ref Diagnostic diagnostic);
        [LibraryImport(Lib)] private static partial nint s2a_mc_provenance();

        private static readonly bool AbiLayoutValid = sizeof(Config) == 24 && sizeof(Diagnostic) == 64 &&
            sizeof(DstPlane) == 24 && Marshal.OffsetOf<DstPlane>(nameof(DstPlane.Capacity)) == 8;
        private nint _handle;
        private uint _token, _width, _height;
        private bool _failed, _disposed;

        internal static bool Available()
        {
            if (!AbiLayoutValid) return false;
            try
            {
                Diagnostic diagnostic = Diagnostic.New();
                return s2a_mc_probe(1, CodecVp9, 64, 64, ref diagnostic) == 0;
            }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
            {
                return false;
            }
        }

        internal static string Diag(in Diagnostic d) =>
            $"stage={d.Stage} code={d.Code} status={d.MediaStatus} token={d.InputToken}/{d.OutputToken} size={d.Width}x{d.Height} color=0x{d.ColorFormat:x} stride={d.OutStride} slice={d.OutSliceH}";

        private bool Fail(string reason, in Diagnostic diagnostic)
        {
            if (!_failed)
            {
                Logger.Error?.Print(LogClass.Nvdec, $"【红】Nvdec VP9 backend=mediacodec {Diag(in diagnostic)} reason={reason} rid={RuntimeInformation.RuntimeIdentifier}");
            }
            _failed = true;
            return false;
        }

        private bool Ensure(uint width, uint height, ref Diagnostic diagnostic)
        {
            if (_handle != 0 && _width == width && _height == height) return true;
            if (_handle != 0)
            {
                s2a_mc_destroy(_handle, ref diagnostic);
                _handle = 0;
            }
            Config config = new() { Abi = 1, Size = (uint)sizeof(Config), Codec = CodecVp9, Width = width, Height = height };
            if (s2a_mc_create(in config, out _handle, ref diagnostic) != 0 || _handle == 0)
            {
                _handle = 0;
                return Fail("create", diagnostic);
            }
            _width = width;
            _height = height;
            if (Marshal.PtrToStringUTF8(s2a_mc_provenance()) != "S2ARM_AMEDIACODEC") return Fail("provenance", diagnostic);
            return true;
        }

        // 进一个重打包帧并排空一次：diag.Output=1 表示 dst 里拿到了一张新图，0 = 输入已收、帧在管线。
        internal int Decode(ReadOnlySpan<byte> au, McSurface surface, out Diagnostic diagnostic)
        {
            diagnostic = Diagnostic.New();
            if (_disposed || _failed || surface.Disposed || au.IsEmpty) return -2;
            try
            {
                if (!AbiLayoutValid) { Fail("ABI layout", diagnostic); return -2; }
                if (!Ensure((uint)surface.Width, (uint)surface.Height, ref diagnostic)) return -8;
                DstPlane* dst = stackalloc DstPlane[3]
                {
                    new() { Data = surface.YPlane.Pointer, Capacity = (ulong)surface.YPlane.Length, Stride = (uint)surface.Stride },
                    new() { Data = surface.UPlane.Pointer, Capacity = (ulong)surface.UPlane.Length, Stride = (uint)surface.UvStride },
                    new() { Data = surface.VPlane.Pointer, Capacity = (ulong)surface.VPlane.Length, Stride = (uint)surface.UvStride },
                };
                int rc;
                fixed (byte* data = au)
                {
                    rc = s2a_mc_decode(_handle, data, checked((uint)au.Length),
                        (uint)surface.Width, (uint)surface.Height, dst, ++_token, ref diagnostic);
                }
                if (rc != 0) Fail("decode", diagnostic);
                return rc;
            }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or OverflowException or OutOfMemoryException or IndexOutOfRangeException)
            {
                Fail(error.GetType().Name, diagnostic);
                return -8;
            }
        }

        // 不进输入、只排空：budgetUs=0 单次轮询；超帧/管线欠账由调用方按序多拿。
        internal int Wait(uint budgetUs, McSurface surface, out Diagnostic diagnostic)
        {
            diagnostic = Diagnostic.New();
            if (_disposed || _failed || _handle == 0 || surface.Disposed) return -2;
            try
            {
                DstPlane* dst = stackalloc DstPlane[3]
                {
                    new() { Data = surface.YPlane.Pointer, Capacity = (ulong)surface.YPlane.Length, Stride = (uint)surface.Stride },
                    new() { Data = surface.UPlane.Pointer, Capacity = (ulong)surface.UPlane.Length, Stride = (uint)surface.UvStride },
                    new() { Data = surface.VPlane.Pointer, Capacity = (ulong)surface.VPlane.Length, Stride = (uint)surface.UvStride },
                };
                int rc = s2a_mc_wait(_handle, dst, budgetUs, ref diagnostic);
                if (rc != 0) Fail("wait", diagnostic);
                return rc;
            }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or IndexOutOfRangeException)
            {
                Fail(error.GetType().Name, diagnostic);
                return -8;
            }
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            nint handle = _handle;
            _handle = 0;
            if (handle == 0) return;
            Diagnostic diagnostic = Diagnostic.New();
            try
            {
                s2a_mc_destroy(handle, ref diagnostic);
            }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
            {
                Fail(error.GetType().Name, diagnostic);
            }
        }
    }

    // 出图三平面由后端持有（unmanaged 分配，免 pin）。Vp9NativePicture 的借视图
    // 指向同一块存储，「有效到下一次 Decode」的契约自然成立。
    internal sealed unsafe class McSurface : IDisposable
    {
        private nint _mem;
        private readonly int _yBytes, _uvBytes;
        public int Width { get; }
        public int Height { get; }
        public int Stride => Width;
        public int UvStride => (Width + 1) / 2;
        internal bool Disposed { get; private set; }
        public Plane YPlane => new(_mem, _yBytes);
        public Plane UPlane => new(_mem + _yBytes, _uvBytes);
        public Plane VPlane => new(_mem + _yBytes + _uvBytes, _uvBytes);
        public McSurface(int width, int height)
        {
            Width = width;
            Height = height;
            _yBytes = checked(Stride * Height);
            _uvBytes = checked(UvStride * ((Height + 1) / 2));
            _mem = (nint)NativeMemory.Alloc((nuint)(_yBytes + 2 * _uvBytes));
        }
        public void Dispose()
        {
            if (Disposed) return;
            Disposed = true;
            NativeMemory.Free((void*)_mem);
            _mem = 0;
        }
    }
}
