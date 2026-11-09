#nullable disable
using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Video;
using System;
using System.Runtime.InteropServices;

namespace Ryujinx.Graphics.Nvdec.Android
{
    // AMediaCodec VP9 硬件解码（#44 第二刀）：native shim 把比特流交给 MediaCodec，
    // 出图直接写进 McSurface 持有的三平面（NV12 在这里解成 I420 形）。
    // 硬件给不出的 VP9 元数据在这里显式清零：mvsOut（下一帧参考 MV）与
    // BackwardUpdateCounts（熵更新计数）都是托管解码器才能产出的量。
    internal sealed unsafe partial class McVp9Decoder : IVp9Decoder
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
        [LibraryImport(Lib)] private static partial int s2a_mc_destroy(nint handle, ref Diagnostic diagnostic);
        [LibraryImport(Lib)] private static partial nint s2a_mc_provenance();

        private static readonly bool AbiLayoutValid = sizeof(Config) == 24 && sizeof(Diagnostic) == 64 &&
            sizeof(DstPlane) == 24 && Marshal.OffsetOf<DstPlane>(nameof(DstPlane.Capacity)) == 8;
        private nint _handle;
        private uint _token, _width, _height;
        private bool _failed, _disposed;
        // 上次 Decode 是「输入已收、帧在管线里」的可恢复态：对调用方报 false（不写表面），
        // 但对 AndroidVp9Decoder 不算失败、不换后端——硬解输出延迟与 show_frame=0 都走这条路。
        internal bool PendingOutput { get; private set; }
        public bool IsHardwareAccelerated => true;
        public ISurface CreateSurface(int width, int height) => new McSurface(width, height);

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

        private bool Fail(string reason, Diagnostic diagnostic = default)
        {
            if (!_failed)
            {
                Logger.Error?.Print(LogClass.Nvdec, $"【红】Nvdec VP9 backend=mediacodec stage={diagnostic.Stage} reason={reason} rid={RuntimeInformation.RuntimeIdentifier} code={diagnostic.Code} status={diagnostic.MediaStatus} token={diagnostic.InputToken}/{diagnostic.OutputToken} size={diagnostic.Width}x{diagnostic.Height} color=0x{diagnostic.ColorFormat:x} stride={diagnostic.OutStride} slice={diagnostic.OutSliceH}");
            }
            _failed = true;
            return false;
        }

        private bool Ensure(uint width, uint height)
        {
            if (_handle != 0 && _width == width && _height == height) return true;
            Diagnostic diagnostic = Diagnostic.New();
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
            if (Marshal.PtrToStringUTF8(s2a_mc_provenance()) != "S2ARM_AMEDIACODEC") return Fail("provenance");
            return true;
        }

        public bool Decode(ref Vp9PictureInfo info, ISurface output, ReadOnlySpan<byte> bitstream,
            ReadOnlySpan<Vp9MvRef> mvsIn, Span<Vp9MvRef> mvsOut)
        {
            // 硬件产不出的两个 VP9 元数据面显式清零（不伪造）：colMV 与熵更新计数。
            mvsOut.Clear();
            info.BackwardUpdateCounts = default;
            PendingOutput = false;
            if (_failed) return false;
            if (_disposed || output is not McSurface surface || surface.Disposed || bitstream.IsEmpty)
            {
                return Fail("unsupported input or decoder/surface state");
            }
            try
            {
                if (!AbiLayoutValid) return Fail("ABI layout");
                if (!Ensure((uint)surface.Width, (uint)surface.Height)) return false;
                Diagnostic diagnostic = Diagnostic.New();
                DstPlane* dst = stackalloc DstPlane[3]
                {
                    new() { Data = surface.YPlane.Pointer, Capacity = (ulong)surface.YPlane.Length, Stride = (uint)surface.Stride },
                    new() { Data = surface.UPlane.Pointer, Capacity = (ulong)surface.UPlane.Length, Stride = (uint)surface.UvStride },
                    new() { Data = surface.VPlane.Pointer, Capacity = (ulong)surface.VPlane.Length, Stride = (uint)surface.UvStride },
                };
                int rc;
                fixed (byte* data = bitstream)
                {
                    rc = s2a_mc_decode(_handle, data, checked((uint)bitstream.Length),
                        (uint)surface.Width, (uint)surface.Height, dst, ++_token, ref diagnostic);
                }
                if (rc != 0) return Fail("decode", diagnostic);
                // 输入已入队但本调用没等到帧：可恢复（管线延迟 / show_frame=0），返回 false 不写表面。
                PendingOutput = diagnostic.Output == 0;
                return diagnostic.Output != 0;
            }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or OverflowException or OutOfMemoryException or IndexOutOfRangeException)
            {
                return Fail(error.GetType().Name);
            }
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            nint handle = _handle;
            _handle = 0;
            if (handle == 0) return;
            try
            {
                Diagnostic diagnostic = Diagnostic.New();
                if (s2a_mc_destroy(handle, ref diagnostic) != 0) Fail("destroy", diagnostic);
            }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
            {
                Fail(error.GetType().Name);
            }
        }
    }

    // 出图三平面由调用方持有（unmanaged 分配，免 pin）。托管解码器与硬件解码器
    // 都经 ISurface 的 Plane 指针写——选错后端回退时同一块存储照样可写。
    internal sealed unsafe class McSurface : ISurface
    {
        private nint _mem;
        private readonly int _yBytes, _uvBytes;
        public int Width { get; }
        public int Height { get; }
        public int Stride => Width;
        public int UvWidth => (Width + 1) / 2;
        public int UvHeight => (Height + 1) / 2;
        public int UvStride => UvWidth;
        public FrameField Field => FrameField.Progressive;
        internal bool Disposed { get; private set; }
        public Plane YPlane => new(_mem, _yBytes);
        public Plane UPlane => new(_mem + _yBytes, _uvBytes);
        public Plane VPlane => new(_mem + _yBytes + _uvBytes, _uvBytes);
        public McSurface(int width, int height)
        {
            Width = width;
            Height = height;
            _yBytes = checked(Stride * Height);
            _uvBytes = checked(UvStride * UvHeight);
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
