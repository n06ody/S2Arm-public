using Ryujinx.Graphics.Video;
using System;
using System.Runtime.InteropServices;

namespace Ryujinx.Graphics.Nvdec.Vpx
{
    // S2Arm: NVDEC VP9 jobs through libvpx (static in the host, services/vpx: libvpx + S2Arm patch + shim s2a_vp9.c,
    // entry s2a_vp9_nvdec; interface vpx/s2a_vp9_nvdec.h). One instance per decoder context; not thread-safe.
    public sealed unsafe partial class VpxVp9Backend : IVp9NativeBackend
    {
        private const string Lib = "s2a_vpx";

        // libvpx decode threads (row-MT on when > 1). design_phase2_B.md §5: the one place this lives.
        public static int Threads = 2;

        [StructLayout(LayoutKind.Sequential)]
        private struct Io
        {
            public byte* Probs;
            public int IntraFrame, TxMode, ReferenceMode, CompFixedRef, CompVarRef0, CompVarRef1, UsePrevFrameMvs, MiCount;
            public byte* PrevMvs;
            public byte* Mvs;
            public byte* Counts;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Picture
        {
            public byte* Y, U, V;
            public int YStride, UStride, VStride, Width, Height;
        }

        [LibraryImport(Lib)] private static partial nint s2a_vp9_open(int threads);
        [LibraryImport(Lib)] private static partial void s2a_vp9_close(nint h);
        [LibraryImport(Lib)] private static partial int s2a_vp9_nvdec(nint h, byte* frame, uint size, Io* io, Picture* pic, byte** error);

        public const int ProbsSize = 3087;
        public const int CountsSize = 13208;
        public const int MvRefSize = 16;

        private nint _handle;

        // The libvpx hook fills the guest MV write buffer and the backward-update counts with real values.
        public bool ProducesGuestMetadata => true;

        public Vp9NativeResult Decode(
            ReadOnlySpan<byte> frame,
            ReadOnlySpan<byte> probs,
            in Vp9NativeParams p,
            ReadOnlySpan<byte> prevMvs,
            Span<byte> mvsOut,
            Span<byte> countsOut,
            out Vp9NativePicture picture,
            out string error)
        {
            picture = default;
            error = null;

            if (probs.Length != ProbsSize || countsOut.Length != CountsSize || mvsOut.Length == 0 || mvsOut.Length % MvRefSize != 0 ||
                (p.UsePrevFrameMvs != 0 && prevMvs.Length != mvsOut.Length))
            {
                return Vp9NativeResult.NotRepresentable;
            }

            if (_handle == 0)
            {
                _handle = s2a_vp9_open(Threads);

                if (_handle == 0)
                {
                    error = "s2a_vp9_open failed";
                    return Vp9NativeResult.Error;
                }
            }

            Picture pic;
            byte* err = null;
            int rc;

            fixed (byte* f = frame)
            fixed (byte* pr = probs)
            fixed (byte* prev = prevMvs)
            fixed (byte* mvs = mvsOut)
            fixed (byte* counts = countsOut)
            {
                Io io = new()
                {
                    Probs = pr,
                    IntraFrame = p.IntraFrame,
                    TxMode = p.TxMode,
                    ReferenceMode = p.ReferenceMode,
                    CompFixedRef = p.CompFixedRef,
                    CompVarRef0 = p.CompVarRef0,
                    CompVarRef1 = p.CompVarRef1,
                    UsePrevFrameMvs = p.UsePrevFrameMvs,
                    MiCount = mvsOut.Length / MvRefSize,
                    PrevMvs = p.UsePrevFrameMvs != 0 ? prev : null,
                    Mvs = mvs,
                    Counts = counts,
                };

                rc = s2a_vp9_nvdec(_handle, f, (uint)frame.Length, &io, &pic, &err);
            }

            if (rc == 0)
            {
                picture = new Vp9NativePicture((nint)pic.Y, (nint)pic.U, (nint)pic.V, pic.YStride, pic.UStride, pic.Width, pic.Height);
                return Vp9NativeResult.Decoded;
            }

            if (err != null)
            {
                error = Marshal.PtrToStringUTF8((nint)err);
            }

            return rc switch
            {
                1 => Vp9NativeResult.NotRepresentable,
                2 => Vp9NativeResult.NeedsKeyFrame,
                _ => Vp9NativeResult.Error,
            };
        }

        public void Dispose()
        {
            if (_handle != 0)
            {
                s2a_vp9_close(_handle);
                _handle = 0;
            }
        }
    }
}
