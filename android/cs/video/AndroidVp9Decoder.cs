#nullable disable
using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Video;
using System;
using System.Runtime.InteropServices;

namespace Ryujinx.Graphics.Nvdec.Android
{
    // Android VP9 选择边界（#44 第二刀）：优先 AMediaCodec 硬件（debug.s2a.video_hw 开着
    // 且平台能建 codec），不行回树内托管 Vp9.Decoder——与接槽前同一条路径。
    // 后端一旦成功出过一帧就绑定：之后失败按粘性失败处理，不中途换后端；
    // 只有首帧未出前的建/配/解失败才回退托管（能力边界，一次性记录）。
    internal sealed class AndroidVp9Decoder : IVp9Decoder
    {
        private IVp9Decoder _inner;
        private bool _hwConfirmed, _failed, _disposed, _fallbackLogged;
        public bool IsHardwareAccelerated => _inner?.IsHardwareAccelerated ?? false;
        // 出图始终用 McSurface：硬件写它，退回托管时托管解码器经同一组 Plane 指针照样能写。
        public ISurface CreateSurface(int width, int height) => new McSurface(width, height);

        private IVp9Decoder Select()
        {
            if (_inner != null) return _inner;
            _inner = McVp9Decoder.Available() ? new McVp9Decoder() : (IVp9Decoder)new Vp9.Decoder();
            return _inner;
        }

        private void ToManaged(string reason)
        {
            if (!_fallbackLogged)
            {
                _fallbackLogged = true;
                Logger.Info?.Print(LogClass.Nvdec, $"Nvdec VP9 backend=managed fallback reason={reason} rid={RuntimeInformation.RuntimeIdentifier}");
            }
            _inner?.Dispose();
            _inner = new Vp9.Decoder();
        }

        public bool Decode(ref Vp9PictureInfo info, ISurface output, ReadOnlySpan<byte> bitstream,
            ReadOnlySpan<Vp9MvRef> mvsIn, Span<Vp9MvRef> mvsOut)
        {
            if (_failed || _disposed) return false;
            IVp9Decoder decoder = Select();
            if (decoder.Decode(ref info, output, bitstream, mvsIn, mvsOut))
            {
                if (decoder is McVp9Decoder) _hwConfirmed = true;
                return true;
            }
            // 输入已入队、帧还在硬件管线里：可恢复态，不判败不换后端，本帧不写表面。
            if (decoder is McVp9Decoder mc && mc.PendingOutput) return false;
            if (decoder is not McVp9Decoder || _hwConfirmed)
            {
                // 托管失败或已确认硬件的半途失败：维持失败语义，不换后端。
                if (_hwConfirmed) _failed = true;
                return false;
            }
            // 硬件尚未出过帧就失败：能力边界，回退托管再试一次。
            ToManaged("decode before first frame");
            return _inner.Decode(ref info, output, bitstream, mvsIn, mvsOut);
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            try { _inner?.Dispose(); }
            catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
            {
                Logger.Error?.Print(LogClass.Nvdec, $"【红】Nvdec VP9 backend=managed stage=dispose reason={error.GetType().Name}");
            }
        }
    }
}
