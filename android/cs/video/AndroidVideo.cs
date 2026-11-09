#nullable disable
using Ryujinx.Graphics.Nvdec;
using Ryujinx.Graphics.Nvdec.Android;

namespace S2A.Android.Video
{
    // 安卓装配登记视频解码器（平台 adapter P1，#1069）：Exports.Initialize（Host.Boot）之前调一次，之后只读。
    public static class AndroidVideo
    {
        public static void Register()
        {
            S2AVideoDecoders.H264 = static () => new AvcH264Decoder();
            S2AVideoDecoders.Vp8 = static () => new AndroidVp8Decoder();
            // VP9（#44）：选 AMediaCodec 硬件（debug.s2a.video_hw）或回树内托管，由包装器选。
            S2AVideoDecoders.Vp9 = static () => new AndroidVp9Decoder();
        }
    }
}
