using Ryujinx.Common;
using Ryujinx.Graphics.Device;
using Ryujinx.Graphics.Nvdec.Image;
using Ryujinx.Graphics.Nvdec.Types.Vp9;
using Ryujinx.Graphics.Nvdec.Vp9;
using Ryujinx.Graphics.Video;
using Ryujinx.Memory;
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using static Ryujinx.Graphics.Nvdec.MemoryExtensions;

namespace Ryujinx.Graphics.Nvdec
{
    static class Vp9Decoder
    {
        private static bool _s2aLogged;   // S2Arm: one line on the first decoded frame, so runs show which games take this (managed) VP9 path
        private static int _s2aNativeBroken;  // S2Arm: the native backend's library is missing or broken in this process
        private static int _s2aMvBlockedLogged;  // S2Arm: one line on the first managed job refused for stale guest MVs

        public unsafe static void Decode(NvdecDecoderContext context, ResourceManager rm, ref NvdecRegisters state)
        {
            PictureInfo pictureInfo = rm.MemoryManager.DeviceRead<PictureInfo>(state.SetDrvPicSetupOffset);
            EntropyProbs entropy = rm.MemoryManager.DeviceRead<EntropyProbs>(state.Vp9SetProbTabBufOffset);
            Decoder decoder = context.GetManagedVp9();

            // S2Arm: native libvpx first (Vp9NativePath; docs/设计_原生VP9/design_phase2_B.md §4). It decodes from a key
            // frame on and hands any job it cannot represent exactly to the managed decoder below.
            Vp9NativePath native = Volatile.Read(ref _s2aNativeBroken) == 0 ? context.GetVp9Native() : null;

            if (native != null)
            {
                Vp9PictureInfo nativeInfo = pictureInfo.Convert();
                entropy.Convert(ref nativeInfo.Entropy);
                ReadOnlySpan<byte> tiles = rm.MemoryManager.DeviceGetSpan(state.SetInBufBaseOffset, (int)pictureInfo.BitstreamSize);
                bool done = false;

                try
                {
                    done = native.TryDecode(rm, ref state, ref pictureInfo, ref nativeInfo, tiles);
                }
                catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
                {
                    if (Interlocked.Exchange(ref _s2aNativeBroken, 1) == 0)
                    {
                        Ryujinx.Common.Logging.Logger.Error?.Print(Ryujinx.Common.Logging.LogClass.Nvdec, $"【红】VP9 原生解码库不可用（{e.GetType().Name}：{e.Message}），本进程只走托管解码器");
                    }

                    native.Deactivate();
                }

                if (done)
                {
                    WriteBackwardUpdates(rm.MemoryManager, state.Vp9SetCtxCounterBufOffset, ref nativeInfo.BackwardUpdateCounts);
                    decoder.TrackSegmentMap(nativeInfo.IsKeyFrame || nativeInfo.IntraOnly, pictureInfo.CurrentFrameSize.Width, pictureInfo.CurrentFrameSize.Height);
                    return;
                }
            }

            ISurface Rent(uint lumaOffset, uint chromaOffset, FrameSize size)
            {
                return rm.Cache.Get(decoder, lumaOffset, chromaOffset, size.Width, size.Height);
            }

            ISurface lastSurface = Rent(state.SetPictureLumaOffset[0], state.SetPictureChromaOffset[0], pictureInfo.LastFrameSize);
            ISurface goldenSurface = Rent(state.SetPictureLumaOffset[1], state.SetPictureChromaOffset[1], pictureInfo.GoldenFrameSize);
            ISurface altSurface = Rent(state.SetPictureLumaOffset[2], state.SetPictureChromaOffset[2], pictureInfo.AltFrameSize);
            ISurface currentSurface = Rent(state.SetPictureLumaOffset[3], state.SetPictureChromaOffset[3], pictureInfo.CurrentFrameSize);

            Vp9PictureInfo info = pictureInfo.Convert();

            info.LastReference = lastSurface;
            info.GoldenReference = goldenSurface;
            info.AltReference = altSurface;

            entropy.Convert(ref info.Entropy);

            ReadOnlySpan<byte> bitstream = rm.MemoryManager.DeviceGetSpan(state.SetInBufBaseOffset, (int)pictureInfo.BitstreamSize);

            ReadOnlySpan<Vp9MvRef> mvsIn = ReadOnlySpan<Vp9MvRef>.Empty;

            if (info.UsePrevInFindMvRefs)
            {
                mvsIn = GetMvsInput(rm.MemoryManager, pictureInfo.CurrentFrameSize, state.Vp9SetColMvReadBufOffset);
            }

            int miCols = BitUtils.DivRoundUp(pictureInfo.CurrentFrameSize.Width, 8);
            int miRows = BitUtils.DivRoundUp(pictureInfo.CurrentFrameSize.Height, 8);

            using WritableRegion mvsRegion = rm.MemoryManager.GetWritableRegion(ExtendOffset(state.Vp9SetColMvWriteBufOffset), miRows * miCols * 16);

            Span<Vp9MvRef> mvsOut = MemoryMarshal.Cast<byte, Vp9MvRef>(mvsRegion.Memory.Span);

            uint lumaOffset = state.SetPictureLumaOffset[3];
            uint chromaOffset = state.SetPictureChromaOffset[3];

            // S2Arm: a native backend that cannot produce the NVDEC MV field leaves the guest MV buffer stale;
            // a managed job with use_prev_frame_mvs would decode from it. Refuse the job until a managed decode
            // (or a key frame) re-establishes real MVs — the segment drops to the next key frame rather than
            // handing the managed decoder fabricated state.
            bool mvBlocked = native is { GuestMvStale: true } && info.UsePrevInFindMvRefs;

            if (mvBlocked)
            {
                if (Interlocked.Exchange(ref _s2aMvBlockedLogged, 1) == 0)
                {
                    Ryujinx.Common.Logging.Logger.Info?.Print(Ryujinx.Common.Logging.LogClass.Nvdec, "VP9 原生段 use_prev_frame_mvs 作业到关键帧前不交托管：客体 MV 缓冲里是原生后端产不出的陈旧字段");
                }

                decoder.TrackSegmentMap(info.IsKeyFrame || info.IntraOnly, pictureInfo.CurrentFrameSize.Width, pictureInfo.CurrentFrameSize.Height);
            }
            else if (decoder.Decode(ref info, currentSurface, bitstream, mvsIn, mvsOut))
            {
                SurfaceWriter.Write(rm.MemoryManager, currentSurface, lumaOffset, chromaOffset);
                native?.NoteManagedDecoded();

                if (!_s2aLogged)
                {
                    _s2aLogged = true;
                    Ryujinx.Common.Logging.Logger.Info?.Print(Ryujinx.Common.Logging.LogClass.Nvdec, $"VP9（Ryujinx 托管解码器）首帧解出：{pictureInfo.CurrentFrameSize.Width}x{pictureInfo.CurrentFrameSize.Height}");
                }
            }

            WriteBackwardUpdates(rm.MemoryManager, state.Vp9SetCtxCounterBufOffset, ref info.BackwardUpdateCounts);

            rm.Cache.Put(lastSurface);
            rm.Cache.Put(goldenSurface);
            rm.Cache.Put(altSurface);
            rm.Cache.Put(currentSurface);
        }

        private static ReadOnlySpan<Vp9MvRef> GetMvsInput(DeviceMemoryManager mm, FrameSize size, uint offset)
        {
            int miCols = BitUtils.DivRoundUp(size.Width, 8);
            int miRows = BitUtils.DivRoundUp(size.Height, 8);

            return MemoryMarshal.Cast<byte, Vp9MvRef>(mm.DeviceGetSpan(offset, miRows * miCols * 16));
        }

        private static void WriteBackwardUpdates(DeviceMemoryManager mm, uint offset, ref Vp9BackwardUpdates counts)
        {
            using WritableRegion backwardUpdatesRegion = mm.GetWritableRegion(ExtendOffset(offset), Unsafe.SizeOf<BackwardUpdates>());

            ref BackwardUpdates backwardUpdates = ref MemoryMarshal.Cast<byte, BackwardUpdates>(backwardUpdatesRegion.Memory.Span)[0];

            backwardUpdates = new BackwardUpdates(ref counts);
        }
    }
}
