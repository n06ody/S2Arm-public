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
    // 重打包帧的压缩头只是 1 个占位字节 0x00（Vp9NativeHeader.Write 末尾）：
    // 硬件照它解出 tx_mode=ONLY_4X4、reference_mode=SINGLE、全部概率增量=无
    // （use_prev_frame_mvs 位也落在占位字节首位=0）。tx_mode / ref_mode /
    // 逐作业概率表本由 hook 带外注入 libvpx——MediaCodec 没有这条通道，
    // 超出占位语义的真实字段一律如实回退，不然硬件按错语义解 tile 出花屏。
    //
    // 表示不了的作业如实回退，不伪造：
    //   · ShowFrame=0（intra-only 隐藏帧）：隐藏帧的像素只进参考槽不出图，MediaCodec 拿不到——
    //     hook 经 frame_to_show 交得出，这里交不出，回 NotRepresentable。
    //   · 供给概率表≠规范默认表：重打包帧 refresh_frame_context=0，AMediaCodec 的 ctx[0] 恒为
    //     规范默认值，逐作业供给表注不进——逐字节对不上就回 NotRepresentable，不出错图。
    //     （SegTreeProb/SegPredProb 不参与比较：segmentation 作业在 header Check 已拒，
    //     这两段不进解码。）
    //   · inter 作业 TxMode≠ONLY_4X4 / ReferenceMode≠SINGLE：占位头解不出这些
    //     语义（intra 帧规范本就强制 ONLY_4X4+SINGLE，不算违例），回 NotRepresentable。
    //   · UsePrevFrameMvs≠0 / prevMvs 非空：重打包头从不发 prev-MV 位（硬件只能
    //     按不复用解，tile 数据按复用编的→错），且客体 MV 缓冲里装不下本帧真实
    //     MV 字段——托管接手只会拿到陈旧值。回 NeedsKeyFrame：关键帧重置 MV
    //     字段，是这条依赖边能安全闭合的唯一点。
    //   · 尺寸变化的非关键帧：codec 重建丢参考状态，回 NeedsKeyFrame。
    //   · mvsOut / countsOut 是硬件产不出的量：一概不写（客体缓冲保持上游真值；
    //     路径的 _counts 维持全零，恰是重打包帧 refresh_frame_context=0 的忠实值——
    //     不产生反向更新），不写「全零」这种具体假值。
    //
    // 管线延迟按「已喂 shown AU − 已出图」记账（_pending）：本作业出图即 Decoded，
    // 有界预算补不齐就 NotRepresentable——这是同步契约下唯一诚实的映射；
    // 连续补齐失败升级 Error 计入路径的禁用阈值。
    //
    // 结局记账（真机验证用）：每 300 作业与 Dispose 各打一行各结局分布。
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

        // 结局记账（真机覆盖率验证，B 项）：每 300 作业与 Dispose 各打一行分布。
        private int _jobs, _decoded, _nrHidden, _nrHeader, _nrMv, _nrProbs, _nrSize, _starved, _errors;

        private Vp9NativeResult Done(Vp9NativeResult r, ref int bucket)
        {
            bucket++;
            if (++_jobs % 300 == 0) Report();
            return r;
        }

        private void Report() => Logger.Info?.Print(LogClass.Nvdec,
            $"mc-vp9 outcome: jobs={_jobs} decoded={_decoded} hidden={_nrHidden} header={_nrHeader} prevMv={_nrMv} probs={_nrProbs} size/key={_nrSize} starved={_starved} error={_errors}");

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
                return Done(Vp9NativeResult.Error, ref _errors);
            }
            // 隐藏帧的像素 MediaCodec 拿不到——如实回退，本作业走托管。
            if (p.ShowFrame == 0)
            {
                return Done(Vp9NativeResult.NotRepresentable, ref _nrHidden);
            }
            if (p.IntraFrame == 0)
            {
                // 占位压缩头解不出 tx_mode / reference_mode / prev-MV 的真实语义
                // （intra 帧规范强制 ONLY_4X4+SINGLE 不查）；超出的作业回托管。
                if (p.TxMode != 0 || p.ReferenceMode != 0)
                {
                    return Done(Vp9NativeResult.NotRepresentable, ref _nrHeader);
                }
                if (p.UsePrevFrameMvs != 0 || !prevMvs.IsEmpty)
                {
                    // 客体 MV 缓冲写不出本帧真 MV 字段：托管接手只会拿到陈旧值。
                    // NeedsKeyFrame——关键帧重置 MV 字段，依赖边在那里闭合。
                    return Done(Vp9NativeResult.NeedsKeyFrame, ref _nrMv);
                }
            }
            if (p.Width < 1 || p.Height < 1 || p.Width > MaxDimension || p.Height > MaxDimension ||
                probs.Length != ProbsSize || countsOut.Length != CountsSize ||
                mvsOut.Length == 0 || mvsOut.Length % MvRefSize != 0)
            {
                return Done(Vp9NativeResult.NotRepresentable, ref _nrHeader);
            }
            // 供给表必须恰好是规范默认值——AMediaCodec 注不进逐作业概率表。
            if (!ProbsAtDefaults(probs))
            {
                return Done(Vp9NativeResult.NotRepresentable, ref _nrProbs);
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
                    return Done(Vp9NativeResult.NeedsKeyFrame, ref _nrSize);
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
                    return Done(Vp9NativeResult.Error, ref _errors);
                }
                return Done(Vp9NativeResult.NotRepresentable, ref _starved);
            }
            _timeouts = 0;
            // mvsOut / countsOut 是硬件产不出的量：不写（客体缓冲保持上游真值），不伪造。
            _width = p.Width;
            _height = p.Height;
            picture = new Vp9NativePicture(
                _surface.YPlane.Pointer, _surface.UPlane.Pointer, _surface.VPlane.Pointer,
                _surface.Stride, _surface.UvStride, p.Width, p.Height);
            return Done(Vp9NativeResult.Decoded, ref _decoded);
        }

        private Vp9NativeResult Sticky(string stage, in McVp9Decoder.Diagnostic diagnostic, out string error)
        {
            _failed = true;
            error = $"mediacodec {stage} {McVp9Decoder.Diag(in diagnostic)}";
            return Done(Vp9NativeResult.Error, ref _errors);
        }

        public void Dispose()
        {
            Report();
            _decoder?.Dispose();
            _decoder = null;
            _surface?.Dispose();
            _surface = null;
        }
    }
}
