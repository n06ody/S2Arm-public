using Ryujinx.Audio;
using Ryujinx.Audio.Common;
using Ryujinx.Audio.Integration;
using Ryujinx.Memory;
using System;
using System.Collections.Concurrent;
using System.Threading;
using static Ryujinx.Audio.Integration.IHardwareDeviceDriver;

namespace S2A.Audio.AAudio
{
    // Same shape as Ryujinx.Audio.Backends.Apple's driver. AAudio takes 16-bit and float PCM in
    // mono or stereo and resamples; Ryujinx's compat layer converts other formats and downmixes 5.1.
    public sealed class AAudioHardwareDeviceDriver : IHardwareDeviceDriver
    {
        private readonly ManualResetEvent _updateRequiredEvent = new(false);
        private readonly ManualResetEvent _pauseEvent = new(true);
        private readonly ConcurrentDictionary<AAudioHardwareDeviceSession, byte> _sessions = new();

        public float Volume { get; set; } = 1f;

        // AAudio is on every device the app installs on (minSdk 29); this driver is only in Android builds.
        public static bool IsSupported => true;

        public IHardwareDeviceSession OpenDeviceSession(Direction direction, IVirtualMemoryManager memoryManager,
            SampleFormat sampleFormat, uint sampleRate, uint channelCount)
        {
            if (direction != Direction.Output)
            {
                throw new NotImplementedException("Input direction is not implemented on the AAudio backend");
            }

            var session = new AAudioHardwareDeviceSession(this, memoryManager, sampleFormat,
                sampleRate == 0 ? Constants.TargetSampleRate : sampleRate, channelCount == 0 ? 2 : channelCount);
            _sessions.TryAdd(session, 0);
            return session;
        }

        internal void Unregister(AAudioHardwareDeviceSession session) => _sessions.TryRemove(session, out _);

        public ManualResetEvent GetUpdateRequiredEvent() => _updateRequiredEvent;
        public ManualResetEvent GetPauseEvent() => _pauseEvent;

        public void S2AConfigureRenderThread() => AAudioSink.s2a_capacity_audio_enter();

        public bool SupportsDirection(Direction direction) => direction == Direction.Output;
        public bool SupportsSampleRate(uint sampleRate) => true;
        public bool SupportsSampleFormat(SampleFormat sampleFormat) => sampleFormat is SampleFormat.PcmInt16 or SampleFormat.PcmFloat;
        public bool SupportsChannelCount(uint channelCount) => channelCount is 1 or 2;

        private bool _disposed;

        public void Dispose()
        {
            // Host.DisposeAudioForShutdown stops the feeder threads before Switch.Dispose;
            // Switch.Dispose then calls this again. Keep the driver idempotent.
            if (_disposed) return;
            _disposed = true;

            foreach (AAudioHardwareDeviceSession session in _sessions.Keys)
            {
                session.Dispose();
            }

            _pauseEvent.Dispose();
        }
    }
}
