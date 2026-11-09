using System;
using System.Runtime.InteropServices;

namespace Ryujinx.Graphics.Video
{
    // S2Arm: native decode of one NVDEC VP9 job (libvpx with S2Arm's hook, services/vpx/patches; design
    // docs/设计_原生VP9/design_phase2_B.md). Registered by the platform through S2AVideoDecoders.Vp9.
    public enum Vp9NativeResult
    {
        Decoded = 0,
        NotRepresentable = 1, // nothing changed; decode this job with the managed decoder
        NeedsKeyFrame = 2,    // not representable after the header was read; native needs a key frame
        Error = -1,           // decode error; native needs a key frame
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Vp9NativeParams
    {
        public int IntraFrame;
        public int TxMode;
        public int ReferenceMode;
        public int CompFixedRef;
        public int CompVarRef0;
        public int CompVarRef1;
        public int UsePrevFrameMvs;
        // Job display semantics for hardware-backed decoders that size a codec per decode: the repacked
        // frame's coded dimensions and its show_frame bit (0 = intra-only hidden job — backend still feeds
        // it for reference state but surfaces the previous picture, not a new frame).
        public int Width;
        public int Height;
        public int ShowFrame;
    }

    public readonly struct Vp9NativePicture
    {
        public readonly nint Y, U, V;
        public readonly int YStride, UvStride, Width, Height;

        public Vp9NativePicture(nint y, nint u, nint v, int yStride, int uvStride, int width, int height)
        {
            Y = y;
            U = u;
            V = v;
            YStride = yStride;
            UvStride = uvStride;
            Width = width;
            Height = height;
        }
    }

    public interface IVp9NativeBackend : IDisposable
    {
        // frame: uncompressed header + 1 byte + tile data. probs: Vp9EntropyProbs bytes (3087). prevMvs: the guest's MV
        // read buffer (empty when UsePrevFrameMvs is 0). mvsOut: the guest's MV write buffer (16 bytes per 8x8), written
        // on Decoded. countsOut: Vp9BackwardUpdates bytes (13208), written on Decoded. The picture stays valid until the
        // next call.
        Vp9NativeResult Decode(
            ReadOnlySpan<byte> frame,
            ReadOnlySpan<byte> probs,
            in Vp9NativeParams p,
            ReadOnlySpan<byte> prevMvs,
            Span<byte> mvsOut,
            Span<byte> countsOut,
            out Vp9NativePicture picture,
            out string error);
    }
}
