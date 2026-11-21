using Ryujinx.Audio.Backends.Common;
using Ryujinx.Audio.Common;
using Ryujinx.Memory;
using System;
using System.Threading;

namespace S2A.Audio.AAudio
{
    // Queue accounting as in Ryujinx.Audio.Backends.Apple's session (S2Arm): queued buffers as
    // (identifier, sample count), only the head partly played, the update event set when one
    // finishes. The device side differs: AAudio pulls from a native ring in its real-time C
    // callback (native/aaudio_sink.c) and never waits for managed code. Samples go into the ring
    // as soon as they are queued, from the queueing thread or the feeder thread, which also
    // turns the frames the callback took into played samples. A GC pause of the feeder delays
    // that bookkeeping; playback continues from the ring.
    // The feeder sleeps until the callback reports that the head buffer finished (the frame
    // position it arms), like the Apple session's per-buffer hand-back: while paused or with
    // nothing queued it does not wake, except for an idle timeout.
    public sealed unsafe class AAudioHardwareDeviceSession : HardwareDeviceSessionOutputBase
    {
        // More than everything a game keeps queued (witcher3: about 85 ms; the Apple pool takes 32 x 50 ms).
        private const int RingMilliseconds = 1000;
        // Safety nets only: wakeups normally come from the callback or from queueing.
        private const int ArmedTimeoutMilliseconds = 250;
        private const int IdleTimeoutMilliseconds = 1000;

        private readonly AAudioHardwareDeviceDriver _driver;
        private readonly AudioSampleQueue _samples;
        private readonly int _bytesPerFrame;
        private readonly Lock _lock = new();   // ring writer, sink lifetime
        private nint _sink;

        private (ulong DriverIdentifier, ulong SampleCount)[] _queued = new (ulong, ulong)[16];
        private int _queuedHead;
        private int _queuedCount;
        private ulong _headPlayed;
        private readonly Lock _queueLock = new();

        private readonly Thread _feeder;
        private volatile bool _closing;
        private bool _armed;
        private long _wakeups;
        private ulong _framesAccounted;
        private ulong _playedSampleCount;
        private bool _started;
        private float _volume = 1f;

        public AAudioHardwareDeviceSession(AAudioHardwareDeviceDriver driver, IVirtualMemoryManager memoryManager,
            SampleFormat sampleFormat, uint sampleRate, uint channelCount)
            : base(memoryManager, sampleFormat, sampleRate, channelCount)
        {
            _driver = driver;
            int format = sampleFormat switch
            {
                SampleFormat.PcmInt16 => AAudioSink.FormatI16,
                SampleFormat.PcmFloat => AAudioSink.FormatF32,
                _ => throw new ArgumentException($"Unsupported sample format {sampleFormat}"),
            };
            _bytesPerFrame = BackendHelper.GetSampleSize(sampleFormat) * (int)channelCount;
            _samples = new AudioSampleQueue((int)sampleRate * _bytesPerFrame);   // renderer output: at most 1 s

            nint sink;
            int result = AAudioSink.s2a_aaudio_open(format, (int)sampleRate, (int)channelCount, RingMilliseconds, &sink);
            if (result != 0)
            {
                throw new InvalidOperationException($"AAudio output stream could not be opened: {result}");
            }

            _sink = sink;
            _feeder = new Thread(FeedLoop) { IsBackground = true, Name = "S2A.AAudio" };
            _feeder.Start();
        }

        /// <summary>Times the feeder thread woke up (diagnostics).</summary>
        public long Wakeups => Interlocked.Read(ref _wakeups);

        public AAudioSink.Stats GetStats()
        {
            AAudioSink.Stats stats = default;
            lock (_lock)
            {
                if (_sink != 0)
                {
                    AAudioSink.s2a_aaudio_stats(_sink, &stats);
                }
            }

            return stats;
        }

        private void FeedLoop()
        {
            AAudioSink.s2a_capacity_audio_enter();
            while (!_closing)
            {
                // The sink outlives this thread (Dispose joins it before closing the sink).
                AAudioSink.s2a_aaudio_wait(_sink, _armed ? ArmedTimeoutMilliseconds : IdleTimeoutMilliseconds);
                Interlocked.Increment(ref _wakeups);
                if (_closing)
                {
                    return;
                }

                ulong played;
                lock (_lock)
                {
                    if (_sink == 0)
                    {
                        return;
                    }

                    AAudioSink.s2a_aaudio_service(_sink);
                    AAudioSink.s2a_aaudio_set_volume(_sink, Math.Clamp(_volume * _driver.Volume, 0f, 1f));
                    FeedLocked();
                    AAudioSink.Stats stats;
                    AAudioSink.s2a_aaudio_stats(_sink, &stats);
                    played = stats.FramesOut - _framesAccounted;
                    _framesAccounted = stats.FramesOut;
                }

                if (played > 0)
                {
                    ProcessPlayedSamples(played * (ulong)_bytesPerFrame);
                }

                Arm();
            }
        }

        // Ask the callback to wake this thread when the head buffer has played, or when the
        // ring has drained to half if queued samples are still waiting for room in it.
        private void Arm()
        {
            ulong at = ulong.MaxValue;
            lock (_queueLock)
            {
                if (_started && _queuedCount > 0)
                {
                    at = _framesAccounted + (_queued[_queuedHead].SampleCount - _headPlayed);
                }
            }

            lock (_lock)
            {
                if (_sink == 0)
                {
                    return;
                }

                if (_started && _samples.Length > 0)
                {
                    ulong ringFrames = (ulong)(RingMilliseconds * RequestedSampleRate / 1000);
                    ulong filled = ringFrames - AAudioSink.s2a_aaudio_space(_sink) / (ulong)_bytesPerFrame;
                    at = Math.Min(at, _framesAccounted + (filled > ringFrames / 2 ? filled - ringFrames / 2 : 0));
                }

                _armed = at != ulong.MaxValue;
                AAudioSink.s2a_aaudio_notify_at(_sink, at);
            }
        }

        // Move queued samples into the ring while it has room (whole frames only).
        private void FeedLocked()
        {
            Span<byte> chunk = stackalloc byte[4096];
            while (true)
            {
                int room = (int)Math.Min(AAudioSink.s2a_aaudio_space(_sink), (uint)chunk.Length);
                int n = Math.Min(room, _samples.Length) / _bytesPerFrame * _bytesPerFrame;
                if (n == 0)
                {
                    return;
                }

                int read = _samples.Read(chunk[..n]);
                if (read <= 0)
                {
                    return;
                }

                fixed (byte* p = chunk)
                {
                    AAudioSink.s2a_aaudio_write(_sink, p, (uint)read);
                }
            }
        }

        private void ProcessPlayedSamples(ulong bytesPlayed)
        {
            ulong remaining = GetSampleCount((int)Math.Min(bytesPlayed, int.MaxValue));
            bool needUpdate = false;

            lock (_queueLock)
            {
                while (remaining > 0 && _queuedCount > 0)
                {
                    ulong sampleCount = _queued[_queuedHead].SampleCount;
                    ulong take = Math.Min(sampleCount - _headPlayed, remaining);

                    _headPlayed += take;
                    remaining -= take;

                    if (_headPlayed == sampleCount)
                    {
                        _queuedHead = (_queuedHead + 1) % _queued.Length;
                        _queuedCount--;
                        _headPlayed = 0;
                        needUpdate = true;
                    }

                    Interlocked.Add(ref _playedSampleCount, take);
                }
            }

            if (needUpdate)
            {
                _driver.GetUpdateRequiredEvent().Set();
            }
        }

        public override void QueueBuffer(AudioBuffer buffer)
        {
            // Register before the samples become visible to the feeder: once they are in _samples another
            // thread can push them into the ring, and a played-bytes callback that finds no record would drop
            // that accounting and never release the buffer (same order as the Apple and OpenHarmony sessions).
            ulong sampleCount = GetSampleCount(buffer);
            bool wasEmpty;

            lock (_queueLock)
            {
                wasEmpty = _queuedCount == 0;
                if (_queuedCount == _queued.Length)
                {
                    var grown = new (ulong, ulong)[_queued.Length * 2];
                    for (int i = 0; i < _queuedCount; i++)
                    {
                        grown[i] = _queued[(_queuedHead + i) % _queued.Length];
                    }

                    _queued = grown;
                    _queuedHead = 0;
                }

                _queued[(_queuedHead + _queuedCount) % _queued.Length] = (buffer.DataPointer, sampleCount);
                _queuedCount++;
            }

            _samples.Enqueue(buffer);

            if (_started)
            {
                lock (_lock)
                {
                    if (_sink != 0)
                    {
                        FeedLocked();
                        if (wasEmpty)
                        {
                            AAudioSink.s2a_aaudio_wake(_sink);   // nothing was armed: let the feeder arm this buffer
                        }
                    }
                }
            }
        }

        public override void Start()
        {
            lock (_lock)
            {
                if (_started || _sink == 0)
                {
                    return;
                }

                _started = true;
                FeedLocked();
                AAudioSink.s2a_aaudio_start(_sink);
                AAudioSink.s2a_aaudio_wake(_sink);
            }
        }

        public override void Stop()
        {
            lock (_lock)
            {
                if (!_started)
                {
                    return;
                }

                _started = false;
                if (_sink != 0)
                {
                    AAudioSink.s2a_aaudio_pause(_sink);
                    AAudioSink.s2a_aaudio_notify_at(_sink, ulong.MaxValue);
                }
            }
        }

        public override ulong GetPlayedSampleCount() => Interlocked.Read(ref _playedSampleCount);
        public override float GetVolume() => _volume;
        public override void SetVolume(float volume) => _volume = volume;

        public override bool WasBufferFullyConsumed(AudioBuffer buffer)
        {
            lock (_queueLock)
            {
                return _queuedCount == 0 || _queued[_queuedHead].DriverIdentifier != buffer.DataPointer;
            }
        }

        public override void PrepareToClose() { }

        // The buffer goes back to the game (Stop / FlushBuffers / Dispose) possibly before it was played: keep what it held.
        public override void UnregisterBuffer(AudioBuffer buffer) => _samples.Release(buffer);

        // Both the driver and the audio system may dispose a session.
        public override void Dispose()
        {
            if (_closing)
            {
                return;
            }

            Stop();
            _closing = true;
            lock (_lock)
            {
                if (_sink != 0)
                {
                    AAudioSink.s2a_aaudio_wake(_sink);
                }
            }

            _feeder.Join();
            lock (_lock)
            {
                if (_sink != 0)
                {
                    AAudioSink.s2a_aaudio_close(_sink);
                    _sink = 0;
                }
            }

            _driver.Unregister(this);
        }
    }
}
