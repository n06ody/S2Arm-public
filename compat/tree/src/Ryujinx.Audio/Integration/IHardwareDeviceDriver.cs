using Ryujinx.Audio.Common;
using Ryujinx.Memory;
using System;
using System.Threading;

namespace Ryujinx.Audio.Integration
{
    /// <summary>
    /// Represent an hardware device driver used in <see cref="Output.AudioOutputSystem"/>.
    /// </summary>
    public interface IHardwareDeviceDriver : IDisposable
    {
        public enum Direction
        {
            Input,
            Output,
        }

        float Volume { get; set; }

        IHardwareDeviceSession OpenDeviceSession(Direction direction, IVirtualMemoryManager memoryManager, SampleFormat sampleFormat, uint sampleRate, uint channelCount);

        ManualResetEvent GetUpdateRequiredEvent();
        ManualResetEvent GetPauseEvent();

        bool SupportsDirection(Direction direction);
        bool SupportsSampleRate(uint sampleRate);
        bool SupportsSampleFormat(SampleFormat sampleFormat);
        bool SupportsChannelCount(uint channelCount);

        // S2Arm（#189 后续，5.1 虚拟环绕第二片）：游戏看到的 active device 名字（HLE AudioDevice / VirtualDeviceSessionRegistry）。
        //   后端按玩家显式选择的输出设备类型给名字时返回它（如 Apple 后端：builtin → AudioBuiltInSpeakerOutput、
        //   headphones → AudioStereoJackOutput），没给（null）时保持缺省 AudioTvOutput——缺省路径逐字节不变。
        //   声道数照旧只看 SupportsChannelCount(6)，不走这里
        string? S2AActiveDeviceName() => null;

        // Called once on an owned renderer thread before it starts processing audio.
        void S2AConfigureRenderThread() { }

        static abstract bool IsSupported { get; }

        IHardwareDeviceDriver GetRealDeviceDriver()
        {
            return this;
        }
    }
}
