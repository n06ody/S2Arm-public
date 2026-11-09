#nullable disable
using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Video;
using System;
using System.Runtime.InteropServices;

namespace Ryujinx.Graphics.Nvdec.Android
{
    // AMediaCodec 版的 IVp9NativeBackend（#44）：同一契约（一作业一图、
    // NotRepresentable 作业交给托管解码器直到下个关键帧）下接 s2a_mc shim。
    //
    // 表示不了的作业如实回退，不伪造：
    //   · ShowFrame=0（intra-only 隐藏帧）：隐藏帧的像素只进参考槽不出图，MediaCodec 拿不到——
    //     hook 经 frame_to_show 交得出，这里交不出，回 NotRepresentable。
    //   · 供给概率表≠规范默认表：重打包帧 refresh_frame_context=0，AMediaCodec 的 ctx[0] 恒为
    //     规范默认值，逐作业供给表注不进——逐字节对不上就回 NotRepresentable，不出错图。
    //     （SegTreeProb/SegPredProb 不参与比较：segmentation 作业在 header Check 已拒，
    //     这两段不进解码。）
    //   · 尺寸变化的非关键帧：codec 重建丢参考状态，回 NeedsKeyFrame。
    //   · mvsOut / countsOut 是托管解码器才能产出的量，Decoded 时显式清零（与 vpx 的真值不同，
    //     是「全零+文档声明」；宿主观测面，不回读进解码状态）。
    //
    // 管线延迟按「已喂 shown AU − 已出图」记账（_pending）：本作业出图即 Decoded，
    // 有界预算补不齐就 NotRepresentable——这是同步契约下唯一诚实的映射；
    // 连续补齐失败升级 Error 计入路径的禁用阈值。
    internal sealed class McVp9Backend : IVp9NativeBackend
    {
        internal const int ProbsSize = 3087;    // Vp9EntropyProbs
        internal const int CountsSize = 13208;  // Vp9BackwardUpdates
        internal const int MvRefSize = 16;      // Vp9MvRef
        private const int MaxDimension = 8192;  // 与 Vp9NativeHeader.Check 同界
        private const int WaitSliceUs = 10_000; // 每次排空轮询切片
        private const int WaitTotalUs = 60_000; // 一个作业内补齐管线欠账的总预算
        private const int MaxTimeouts = 3;      // 连续欠账升级 Error（死 codec 不空转）

        private McVp9Decoder _decoder;
        private McSurface _surface;
        private int _width, _height;   // codec 当前配置的编码尺寸
        private int _pending;          // 已喂的 show_frame=1 AU 与已收输出的差额
        private int _timeouts;         // 连续预算内没补齐的次数
        private bool _failed;
        private static byte[] _defaultProbs;   // 懒取，进程一份

        public static McVp9Backend TryCreate() => McVp9Decoder.Available() ? new McVp9Backend() : null;

        private static ReadOnlySpan<byte> DefaultProbs()
        {
            if (_defaultProbs == null)
            {
                Vp9EntropyProbs fc = Ryujinx.Graphics.Nvdec.Vp9.Decoder.DefaultProbs();
                _defaultProbs = MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref fc, 1)).ToArray();
            }
            return _defaultProbs;
        }

        // 供给表 == 规范默认表？seg 两个字段不进解码，不算进比较。
        private static bool ProbsAtDefaults(ReadOnlySpan<byte> probs)
        {
            ReadOnlySpan<byte> d = DefaultProbs();
            int segTree = (int)Marshal.OffsetOf<Vp9EntropyProbs>(nameof(Vp9EntropyProbs.SegTreeProb));
            int segEnd = (int)Marshal.OffsetOf<Vp9EntropyProbs>(nameof(Vp9EntropyProbs.SegPredProb)) + 3;
            return probs[..segTree].SequenceEqual(d[..segTree]) &&
                   probs[segEnd..].SequenceEqual(d[segEnd..]);
        }

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
            if (_failed)
            {
                error = "mediacodec sticky failure";
                return Vp9NativeResult.Error;
            }
            // 隐藏帧的像素 MediaCodec 拿不到——如实回退，本作业走托管。
            if (p.ShowFrame == 0)
            {
                return Vp9NativeResult.NotRepresentable;
            }
            if (p.Width < 1 || p.Height < 1 || p.Width > MaxDimension || p.Height > MaxDimension ||
                probs.Length != ProbsSize || countsOut.Length != CountsSize ||
                mvsOut.Length == 0 || mvsOut.Length % MvRefSize != 0)
            {
                return Vp9NativeResult.NotRepresentable;
            }
            // 供给表必须恰好是规范默认值——AMediaCodec 注不进逐作业概率表。
            if (!ProbsAtDefaults(probs))
            {
                return Vp9NativeResult.NotRepresentable;
            }
            // 尺寸变化：codec 重建丢参考状态，非关键帧如实回 NeedsKeyFrame。
            if (_decoder != null && (p.Width != _width || p.Height != _height))
            {
                _decoder.Dispose();
                _decoder = null;
                _pending = 0;
                _surface?.Dispose();
                _surface = null;
                if (p.IntraFrame == 0)
                {
                    return Vp9NativeResult.NeedsKeyFrame;
                }
            }
            _surface ??= new McSurface(p.Width, p.Height);
            _decoder ??= new McVp9Decoder();

            // 进 AU 记账，再按「欠几张补几张」有界排空——VP9 无重排，
            // 第 k 张出图对应第 k 个 shown AU，补到 pending=0 时最后一张就是本作业的。
            _pending++;
            int rc = _decoder.Decode(frame, _surface, out McVp9Decoder.Diagnostic diagnostic);
            if (rc != 0)
            {
                return Sticky("decode", in diagnostic, out error);
            }
            if (diagnostic.Output != 0)
            {
                _pending--;
            }
            for (int waited = 0; _pending > 0 && waited < WaitTotalUs;)
            {
                rc = _decoder.Wait(WaitSliceUs, _surface, out diagnostic);
                if (rc != 0)
                {
                    return Sticky("wait", in diagnostic, out error);
                }
                if (diagnostic.Output != 0)
                {
                    _pending--;
                }
                else
                {
                    waited += WaitSliceUs;
                }
            }
            if (_pending > 0)
            {
                _timeouts++;
                if (_timeouts >= MaxTimeouts)
                {
                    _failed = true;
                    error = $"mediacodec output starved {_timeouts} jobs in a row ({McVp9Decoder.Diag(in diagnostic)})";
                    return Vp9NativeResult.Error;
                }
                return Vp9NativeResult.NotRepresentable;
            }
            _timeouts = 0;
            // 硬件给不出的两个元数据面显式清零（不伪造）。
            mvsOut.Clear();
            countsOut.Clear();
            _width = p.Width;
            _height = p.Height;
            picture = new Vp9NativePicture(
                _surface.YPlane.Pointer, _surface.UPlane.Pointer, _surface.VPlane.Pointer,
                _surface.Stride, _surface.UvStride, p.Width, p.Height);
            return Vp9NativeResult.Decoded;
        }

        private Vp9NativeResult Sticky(string stage, in McVp9Decoder.Diagnostic diagnostic, out string error)
        {
            _failed = true;
            error = $"mediacodec {stage} {McVp9Decoder.Diag(in diagnostic)}";
            return Vp9NativeResult.Error;
        }

        public void Dispose()
        {
            _decoder?.Dispose();
            _decoder = null;
            _surface?.Dispose();
            _surface = null;
        }
    }
}
