using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Nvdec.Image;
using Ryujinx.Graphics.Nvdec.Types.Vp9;
using Ryujinx.Graphics.Nvdec.Vpx;
using Ryujinx.Graphics.Video;
using Ryujinx.Memory;
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using static Ryujinx.Graphics.Nvdec.MemoryExtensions;

namespace Ryujinx.Graphics.Nvdec
{
    // S2Arm: native VP9 decoding for one NVDEC decoder context (docs/设计_原生VP9/design_phase2_B.md §3-§4). Jobs go to
    // libvpx from a key frame on; any job it cannot represent exactly turns native off until the next key frame, and the
    // managed decoder takes that job. Everything native writes to guest memory (picture, MVs, counts) is in the layouts the
    // managed decoder writes, so a switch between the two is exact.
    sealed class Vp9NativePath : IDisposable
    {
        private const int MaxErrors = 3;

        private readonly IVp9NativeBackend _backend;
        private readonly Vp9SlotMap _slots = new();
        private readonly VpxSurface _surface = new(0, 0);
        private readonly HashSet<string> _loggedReasons = [];
        private byte[] _frame = new byte[1 << 20];
        private readonly byte[] _counts = new byte[VpxVp9Backend.CountsSize];
        private bool _active;
        private int _errors;
        private bool _loggedFirst;

        public bool Disabled { get; private set; }

        public Vp9NativePath(IVp9NativeBackend backend)
        {
            _backend = backend;

            if (Unsafe.SizeOf<Vp9EntropyProbs>() != VpxVp9Backend.ProbsSize || Unsafe.SizeOf<Vp9BackwardUpdates>() != VpxVp9Backend.CountsSize)
            {
                Logger.Error?.Print(LogClass.Nvdec, $"【红】VP9 原生解码：概率表 / 计数布局与 libvpx 补丁不符（{Unsafe.SizeOf<Vp9EntropyProbs>()} / {Unsafe.SizeOf<Vp9BackwardUpdates>()}），只走托管解码器");
                Disabled = true;
            }
        }

        public void Deactivate()
        {
            _active = false;
            _slots.Reset();
        }

        private void Fallback(string reason)
        {
            Deactivate();

            if (_loggedReasons.Add(reason))
            {
                Logger.Info?.Print(LogClass.Nvdec, $"VP9 原生解码 → 托管解码器（到下一个关键帧）：{reason}");
            }
        }

        private static Vp9NativeInput Input(ref PictureInfo p, ref NvdecRegisters state)
        {
            return new Vp9NativeInput
            {
                LastWidth = p.LastFrameSize.Width, LastHeight = p.LastFrameSize.Height,
                GoldenWidth = p.GoldenFrameSize.Width, GoldenHeight = p.GoldenFrameSize.Height,
                AltWidth = p.AltFrameSize.Width, AltHeight = p.AltFrameSize.Height,
                Width = p.CurrentFrameSize.Width, Height = p.CurrentFrameSize.Height,
                KeyFrame = p.Flags.HasFlag(FrameFlags.IsKeyFrame),
                IntraOnly = p.Flags.HasFlag(FrameFlags.IntraOnly),
                SignBias1 = p.RefFrameSignBias[1], SignBias2 = p.RefFrameSignBias[2], SignBias3 = p.RefFrameSignBias[3],
                FilterLevel = p.FirstLevel, Sharpness = p.SharpnessLevel, BaseQIndex = p.BaseQIndex,
                YDcDeltaQ = p.YDcDeltaQ, UvDcDeltaQ = p.UvDcDeltaQ, UvAcDeltaQ = p.UvAcDeltaQ,
                Lossless = p.Lossless != 0, TxMode = p.TxMode, ReferenceMode = p.ReferenceMode, InterpFilter = p.InterpFilter,
                AllowHighPrecisionMv = p.AllowHighPrecisionMv != 0,
                Log2TileCols = p.Log2TileCols, Log2TileRows = p.Log2TileRows,
                SegmentationEnabled = p.Seg.Enabled != 0,
                ModeRefDeltaEnabled = p.Lf.ModeRefDeltaEnabled != 0,
                RefDelta0 = p.Lf.RefDeltas[0], RefDelta1 = p.Lf.RefDeltas[1], RefDelta2 = p.Lf.RefDeltas[2], RefDelta3 = p.Lf.RefDeltas[3],
                ModeDelta0 = p.Lf.ModeDeltas[0], ModeDelta1 = p.Lf.ModeDeltas[1],
                BitDepth = (int)p.BitDepth,
                LastLuma = state.SetPictureLumaOffset[0], LastChroma = state.SetPictureChromaOffset[0],
                GoldenLuma = state.SetPictureLumaOffset[1], GoldenChroma = state.SetPictureChromaOffset[1],
                AltLuma = state.SetPictureLumaOffset[2], AltChroma = state.SetPictureChromaOffset[2],
                OutLuma = state.SetPictureLumaOffset[3], OutChroma = state.SetPictureChromaOffset[3],
            };
        }

        // Returns true when this job was decoded natively: the picture is in guest memory, the MVs are in the guest's MV
        // write buffer and the counts are in info.BackwardUpdateCounts (the caller writes them back as for the managed path).
        public unsafe bool TryDecode(ResourceManager rm, ref NvdecRegisters state, ref PictureInfo pictureInfo, ref Vp9PictureInfo info, ReadOnlySpan<byte> tiles)
        {
            if (Disabled)
            {
                return false;
            }

            Vp9NativeInput input = Input(ref pictureInfo, ref state);

            if (!_active)
            {
                if (!input.KeyFrame)
                {
                    return false;
                }

                _active = true;
                _slots.Reset();
            }

            string reason = Vp9NativeHeader.Check(input);
            Span<int> refIdx = stackalloc int[3];
            int target = -1;
            reason ??= _slots.Resolve(input, refIdx, out target);

            if (reason != null)
            {
                Fallback(reason);
                return false;
            }

            if (_frame.Length < tiles.Length + Vp9NativeHeader.MaxHeaderBytes)
            {
                _frame = new byte[(tiles.Length + Vp9NativeHeader.MaxHeaderBytes) * 2];
            }

            int frameLength = Vp9NativeHeader.Write(input, refIdx, target, tiles, _frame);

            int miCols = (input.Width + 7) >> 3;
            int miRows = (input.Height + 7) >> 3;
            int mvBytes = miRows * miCols * VpxVp9Backend.MvRefSize;

            Vp9NativeParams p = new()
            {
                IntraFrame = input.KeyFrame || input.IntraOnly ? 1 : 0,
                TxMode = input.TxMode,
                ReferenceMode = input.ReferenceMode,
                CompFixedRef = pictureInfo.CompFixedRef,
                CompVarRef0 = pictureInfo.CompVarRef[0],
                CompVarRef1 = pictureInfo.CompVarRef[1],
                UsePrevFrameMvs = info.UsePrevInFindMvRefs ? 1 : 0,
                Width = input.Width,
                Height = input.Height,
                ShowFrame = input.IntraOnly ? 0 : 1,   // 与 Vp9NativeHeader.Write 的 show_frame 位同源
            };

            ReadOnlySpan<byte> prevMvs = info.UsePrevInFindMvRefs
                ? rm.MemoryManager.DeviceGetSpan(state.Vp9SetColMvReadBufOffset, mvBytes)
                : ReadOnlySpan<byte>.Empty;
            ReadOnlySpan<byte> probs = MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref info.Entropy, 1));

            Vp9NativeResult result;
            Vp9NativePicture picture;
            string error;

            using (WritableRegion mvsRegion = rm.MemoryManager.GetWritableRegion(ExtendOffset(state.Vp9SetColMvWriteBufOffset), mvBytes))
            {
                result = _backend.Decode(_frame.AsSpan(0, frameLength), probs, p, prevMvs, mvsRegion.Memory.Span, _counts, out picture, out error);
            }

            if (result != Vp9NativeResult.Decoded)
            {
                if (result == Vp9NativeResult.Error)
                {
                    _errors++;
                    Logger.Error?.Print(LogClass.Nvdec, $"【红】VP9 原生解码失败（第 {_errors} 次，{input.Width}x{input.Height}，关键帧 {input.KeyFrame}）：{error ?? "libvpx 未给原因"}；本作业改走托管解码器");

                    if (_errors >= MaxErrors)
                    {
                        Logger.Error?.Print(LogClass.Nvdec, $"【红】VP9 原生解码连续出错 {MaxErrors} 次，本解码上下文改用托管解码器");
                        Disabled = true;
                    }
                }

                Fallback(result == Vp9NativeResult.Error ? "libvpx decode error" : $"libvpx: {result}{(error != null ? " (" + error + ")" : "")}");
                return false;
            }

            _slots.Commit(input, input.KeyFrame ? -1 : target);
            _surface.Attach((byte*)picture.Y, (byte*)picture.U, (byte*)picture.V, picture.YStride, picture.UvStride, picture.Width, picture.Height);
            SurfaceWriter.Write(rm.MemoryManager, _surface, input.OutLuma, input.OutChroma);
            rm.Cache.Invalidate(input.OutLuma, input.OutChroma);
            info.BackwardUpdateCounts = MemoryMarshal.Read<Vp9BackwardUpdates>(_counts);

            if (!_loggedFirst)
            {
                _loggedFirst = true;
                Logger.Info?.Print(LogClass.Nvdec, $"VP9（libvpx 原生，{VpxVp9Backend.Threads} 线程）首帧解出：{input.Width}x{input.Height}");
            }

            return true;
        }

        public void Dispose()
        {
            _backend.Dispose();
        }
    }
}
