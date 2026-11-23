using Ryujinx.Audio.Integration;
using Ryujinx.Audio.Renderer.Dsp.Command;
using Ryujinx.Audio.Renderer.Utils;
using Ryujinx.Common;
using Ryujinx.Common.Logging;
using System;
using System.Threading;

namespace Ryujinx.Audio.Renderer.Dsp
{
    public class AudioProcessor : IDisposable
    {
        private const int MaxBufferedFrames = 5;
        private const int TargetBufferedFrames = 3;

        private enum MailboxMessage : uint
        {
            Start,
            Stop,
            RenderStart,
            RenderEnd,
        }

        private class RendererSession
        {
            public CommandList CommandList;
            public int RenderingLimit;
            public ulong AppletResourceId;
        }

        private Mailbox<MailboxMessage> _mailbox;
        private RendererSession[] _sessionCommandList;
        private Thread _workerThread;

        public IHardwareDevice[] OutputDevices { get; private set; }

        private long _lastTime;
        private long _playbackEnds;

        private ManualResetEvent _pauseEvent;

        // S2Arm（能耗，帧率能耗线 10-20）：S2A_AUDIO_INLINE=1 时 DSP 在调用方（AudioRendererManager 线程）上同步做，不起 AudioProcessor.Worker、不走信箱。
        // 原两线程是严格同步交接（管理线程 Signal 后即 Wait 到 RenderEnd，无并行），同步执行顺序不变；
        // 测试设备1 线程表：两条线程合计约 1.9 万运行段/秒（每段 2.5–7.6 µs），设计每 5 ms 一次交接
        // 10-22 起缺省开（杂音：两线程每周期一次交接，负载下交接等调度 27–34 ms，渲染器整批丢周期）；S2A_AUDIO_INLINE=0 退回上游两线程
        private static readonly bool S2AInline = Environment.GetEnvironmentVariable("S2A_AUDIO_INLINE") != "0";

        // S2Arm: diagnostics read (and reset) by the output backend's reporter: periods dropped by the catch-up skip below, and the
        // worst lateness of a period start (how long after its due time the loop got to it)
        public static long S2ASkippedPeriods;
        public static long S2AMaxLateNs;

        public AudioProcessor()
        {
        }

        private static uint GetHardwareChannelCount(IHardwareDeviceDriver deviceDriver)
        {
            // Get the real device driver (In case the compat layer is on top of it).
            deviceDriver = deviceDriver.GetRealDeviceDriver();

            if (deviceDriver.SupportsChannelCount(6))
            {
                return 6;
            }

            // NOTE: We default to stereo as this will get downmixed to mono by the compat layer if it's not compatible.
            return 2;
        }

        public void Start(IHardwareDeviceDriver deviceDriver)
        {
            OutputDevices = new IHardwareDevice[Constants.AudioRendererSessionCountMax];

            uint channelCount = GetHardwareChannelCount(deviceDriver);

            for (int i = 0; i < OutputDevices.Length; i++)
            {
                // TODO: Don't hardcode sample rate.
                OutputDevices[i] = new HardwareDeviceImpl(deviceDriver, channelCount, Constants.TargetSampleRate);
            }

            _mailbox = new Mailbox<MailboxMessage>();
            _sessionCommandList = new RendererSession[Constants.AudioRendererSessionCountMax];
            _lastTime = PerformanceCounter.ElapsedNanoseconds;
            _pauseEvent = deviceDriver.GetPauseEvent();

            if (S2AInline)
            {
                Console.Error.WriteLine("[S2A 声音] DSP 在管理线程同步执行（不起 AudioProcessor.Worker；S2A_AUDIO_INLINE=0 退回两线程）");
                return;
            }

            StartThread();

            _mailbox.SendMessage(MailboxMessage.Start);

            if (_mailbox.ReceiveResponse() != MailboxMessage.Start)
            {
                throw new InvalidOperationException("Audio Processor Start response was invalid!");
            }
        }

        public void Stop()
        {
            if (_workerThread?.IsAlive == true)
            {
                _mailbox.SendMessage(MailboxMessage.Stop);
                MailboxMessage response = _mailbox.ReceiveResponse();
                // Startup rollback may leave the DSP's first RenderEnd unread.
                if (response == MailboxMessage.RenderEnd)
                {
                    response = _mailbox.ReceiveResponse();
                }

                if (response != MailboxMessage.Stop)
                {
                    throw new InvalidOperationException("Audio Processor Stop response was invalid!");
                }
                _workerThread.Join();
            }
            _workerThread = null;

            _mailbox?.Dispose();
            _mailbox = null;
            IHardwareDevice[] devices = OutputDevices;
            OutputDevices = null;
            if (devices != null)
            {
                foreach (IHardwareDevice device in devices)
                {
                    device?.Dispose();
                }
            }
        }

        // S2Arm: one session record per slot, reused: Send only happens after the processor cleared the slot
        // (HasRemainingCommands), so a record is never shared between two frames (was allocated every 5 ms)
        private readonly RendererSession[] _s2aSessionRecords = new RendererSession[Constants.AudioRendererSessionCountMax];

        public void Send(int sessionId, CommandList commands, int renderingLimit, ulong appletResourceId)
        {
            RendererSession session = _s2aSessionRecords[sessionId] ??= new RendererSession();

            session.CommandList = commands;
            session.RenderingLimit = renderingLimit;
            session.AppletResourceId = appletResourceId;

            Volatile.Write(ref _sessionCommandList[sessionId], session);
        }

        public bool HasRemainingCommands(int sessionId)
        {
            return _sessionCommandList[sessionId] != null;
        }

        // S2Arm (#1598): ticks the last inline Signal spent waiting on the pause event (guest frozen / app unfocused) before rendering.
        //   The manager's slow-period line subtracts it: a 7 s freeze used to be logged as one 7 s DSP period
        public long S2ALastPauseWaitTicks;

        public void Signal()
        {
            if (S2AInline)
            {
                long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
                _pauseEvent?.WaitOne();
                S2ALastPauseWaitTicks = System.Diagnostics.Stopwatch.GetTimestamp() - t0;
                Render();
                return;
            }

            _mailbox.SendMessage(MailboxMessage.RenderStart);
        }

        public void Wait()
        {
            if (!S2AInline && _mailbox.ReceiveResponse() != MailboxMessage.RenderEnd)
            {
                throw new InvalidOperationException("Audio Processor Wait response was invalid!");
            }

            long increment = Constants.AudioProcessorMaxUpdateTimeTarget;

            long timeNow = PerformanceCounter.ElapsedNanoseconds;

            // S2Arm (11-13 断音): pace by what the output still holds when it can tell (see S2ADepthWait); the wall clock below otherwise
            if (S2ADepthFrames > 0 && S2ADepthWait(increment, timeNow))
            {
                return;
            }

            if (timeNow > _playbackEnds)
            {
                // Playback has restarted.
                _playbackEnds = timeNow;
            }

            _playbackEnds += increment;

            // The number of frames we are behind where the timer says we should be.
            long framesBehind = (timeNow - _lastTime) / increment;

            // The number of frames yet to play on the backend.
            long bufferedFrames = (_playbackEnds - timeNow) / increment + framesBehind;

            // If we've entered a situation where a lot of buffers will be queued on the backend,
            // Skip some audio frames so that playback can catch up.
            if (bufferedFrames > MaxBufferedFrames)
            {
                // Skip a few frames so that we're not too far behind. (the target number of frames)
                _lastTime += increment * (bufferedFrames - TargetBufferedFrames);
                Interlocked.Add(ref S2ASkippedPeriods, bufferedFrames - TargetBufferedFrames);
            }

            long s2aLate = timeNow - (_lastTime + increment);

            if (s2aLate > Volatile.Read(ref S2AMaxLateNs))
            {
                Volatile.Write(ref S2AMaxLateNs, s2aLate);
            }

            // S2Arm (11-14 卡顿诊断探针): 本周期比节拍晚到的纳秒累计给宿主 TSV 的 @aud_wait_ms 列（hle_nvn.c 每帧取走清零）。
            // 游戏线程等的声音更新事件由本线程 Signal（AudioRendererManager.SendCommands）——这里晚到，等事件的游戏线程就同量晚醒
            if (s2aLate > 0)
            {
                S2AAudLate.Note(s2aLate);
            }

            // S2Arm: one sleep to the end of the period instead of polling WaitOne(1) (3.4 wakeups per 5 ms period → 1)
            if (timeNow < _lastTime + increment)
            {
                Ryujinx.Common.PreciseSleep.PreciseSleepHelper.SleepUntilTimePointSingleWake(
                    PerformanceCounter.ElapsedTicks + (_lastTime + increment - timeNow) * PerformanceCounter.TicksPerMillisecond / 1_000_000);
            }

            _lastTime += increment;
        }

        // S2Arm (11-13 断音，0400486024DEC000 Mac 局第 26 秒): upstream paces rendering by the wall clock and, once more than 5 periods behind,
        //   throws the excess periods away (keeps 3). The output then holds only what it held before: about 480 frames (10 ms) plus
        //   the device's own ~2.5 ms. A render thread late by ~30 ms (with a 68 ms game frame) dropped 4 periods = 20.1 ms of real gap,
        //   and the lost depth never came back (the queue played at zero margin, 24–39 zero-in-flight hand-backs per window after).
        //   Eden paces its renderer by the sink instead (ring of 4 × 5 ms, WaitFreeSpace): render whenever the output holds less than
        //   the target, back to back after a stall, nothing dropped. Same here: below S2ADepthFrames render at once; at or above it,
        //   sleep until the output drains to it. Bounds: with the output near the target, the wall-clock due time still renders (a
        //   device slightly slower than the clock); a device that stops draining holds the loop at most 4 periods past due (the game
        //   waits on this loop's event; never block it on a dead device). S2A_AUDIO_DEPTH_MS=0: upstream wall clock.
        private static readonly long S2ADepthFrames = S2ADepthMs() * Constants.TargetSampleRate / 1000;
        private readonly long[] _s2aSeenAppends = new long[Constants.AudioRendererSessionCountMax];

        private static long S2ADepthMs()
        {
            string v = Environment.GetEnvironmentVariable("S2A_AUDIO_DEPTH_MS");

            return long.TryParse(v, out long ms) && ms >= 0 ? ms : 30;
        }

        // Frames held by the least-filled output fed by the last render; -1 when none was fed or none can tell
        private long S2AFedBufferedFrames(bool update)
        {
            long min = -1;

            for (int i = 0; i < OutputDevices.Length; i++)
            {
                if (OutputDevices[i] is not HardwareDeviceImpl d)
                {
                    continue;
                }

                long appends = d.S2AAppends;

                if (appends == _s2aSeenAppends[i])
                {
                    continue;
                }

                if (update)
                {
                    _s2aSeenAppends[i] = appends;
                }

                long b = d.S2ABufferedFrames;

                if (b >= 0 && (min < 0 || b < min))
                {
                    min = b;
                }
            }

            return min;
        }

        // Rendered audio's position on the wall clock (ns): advances one period per render. Caps rendering at the depth ahead of the
        //   wall clock: an output that hands buffers back unplayed (iOS queue timeline ahead, QueueTimelineWatch) never fills, and must
        //   not make this loop render as fast as it can until the watchdog restarts the queue
        private long _s2aVirt;

        // S2Arm (#1957 Mac 一款开放世界游戏玩 3～4 小时后爆音不停，诊断 D-EZH5): the cap above holds rendering to the wall clock, but the
        //   device plays by its own clock. One a few ppm faster eats the depth slowly (start of that run: at least 1200 frames in flight;
        //   3 hours later: 0, 165 → 920 zero-in-flight hand-backs every 10 s, rendering exactly 480240 frames per 10 s = the wall clock
        //   while below the target, i.e. held by the cap). So while the output holds less than half the target and has been fed, a render may
        //   pass the cap without moving _s2aVirt (the device is ahead of the wall clock: follow it). At most one such render per
        //   S2ACatchUpEvery renders (+3.1%, far above any real clock drift — that run: ~75 ms of headroom gone in ~3 h ≈ 7 ppm): an output that
        //   hands buffers back unplayed (never fills) gets at most 3.1% more than the wall clock, not a spin
        private const int S2ACatchUpEvery = 32;
        private int _s2aCatchUp, _s2aCatchUpTick;
        public static long S2ACatchUps;   // 诊断：追设备的渲染次数（10 秒行读取清零）

        // True: paced by depth (the next render may start now). False: no fed output can tell its depth: use the wall clock
        private bool S2ADepthWait(long increment, long timeNow)
        {
            long buffered = S2AFedBufferedFrames(false);

            if (buffered < 0)
            {
                S2AFedBufferedFrames(true);
                _s2aVirt = 0;

                return false;
            }

            // Lateness = how far past one period after the last render start the loop got here (a stalled thread)
            if (timeNow - (_lastTime + increment) > Volatile.Read(ref S2AMaxLateNs))
            {
                Volatile.Write(ref S2AMaxLateNs, timeNow - (_lastTime + increment));
            }

            long period = Constants.TargetSampleCount;
            long depthNs = S2ADepthFrames * 1_000_000_000L / Constants.TargetSampleRate;

            // What a long stall left unrendered is a gap already heard: owe at most 8 periods, never more
            _s2aVirt = Math.Max(_s2aVirt, timeNow - 8 * increment);

            bool catchUp = false;

            if (++_s2aCatchUpTick >= S2ACatchUpEvery)
            {
                _s2aCatchUpTick = 0;
                _s2aCatchUp = Math.Min(_s2aCatchUp + 1, 4);
            }

            while (true)
            {
                bool below = buffered < S2ADepthFrames;
                long capAt = _s2aVirt - depthNs - increment;   // rendering earlier than this runs ahead of the wall clock by more than the depth

                if (below && timeNow >= capAt)
                {
                    break;
                }

                if (buffered < S2ADepthFrames / 2 && _s2aCatchUp > 0)
                {
                    catchUp = true;

                    break;
                }

                // Wall clock: the output is near the target (a device a little slower than the clock), or it stopped draining (bounded wait)
                if ((timeNow >= _s2aVirt && buffered < S2ADepthFrames + 2 * period) || timeNow >= _s2aVirt + 4 * increment)
                {
                    break;
                }

                long wake = _s2aVirt + 4 * increment;

                if (below)
                {
                    wake = Math.Min(wake, capAt);
                }
                else
                {
                    wake = Math.Min(wake, timeNow + (buffered - S2ADepthFrames + 1) * 1_000_000_000L / Constants.TargetSampleRate);
                }

                if (buffered < S2ADepthFrames + 2 * period)
                {
                    wake = Math.Min(wake, _s2aVirt);
                }

                Ryujinx.Common.PreciseSleep.PreciseSleepHelper.SleepUntilTimePointSingleWake(
                    PerformanceCounter.ElapsedTicks + Math.Max(0, wake - timeNow) * PerformanceCounter.TicksPerMillisecond / 1_000_000);

                timeNow = PerformanceCounter.ElapsedNanoseconds;
                buffered = Math.Max(0, S2AFedBufferedFrames(false));
            }

            S2AFedBufferedFrames(true);

            if (catchUp)
            {
                _s2aCatchUp--;
                Interlocked.Increment(ref S2ACatchUps);
            }
            else
            {
                _s2aVirt += increment;
            }

            _lastTime = timeNow;
            _playbackEnds = timeNow;

            return true;
        }

        private void StartThread()
        {
            _workerThread = new Thread(Work)
            {
                Name = "AudioProcessor.Worker",
            };

            _workerThread.Start();
        }

        private void Work()
        {
            if (_mailbox.ReceiveMessage() != MailboxMessage.Start)
            {
                throw new InvalidOperationException("Audio Processor Start message was invalid!");
            }

            _mailbox.SendResponse(MailboxMessage.Start);
            _mailbox.SendResponse(MailboxMessage.RenderEnd);

            Logger.Info?.Print(LogClass.AudioRenderer, "Starting audio processor");

            // S2Arm: real-time like the manager thread (see Ryujinx.Common.S2ARealtime); only used with S2A_AUDIO_INLINE=0
            S2ARealtime.Promote("声音 DSP 线程", Constants.AudioProcessorMaxUpdateTimeTarget, Constants.AudioProcessorMaxUpdateTimeTarget / 2, Constants.AudioProcessorMaxUpdateTimeTarget);

            while (true)
            {
                Ryujinx.Common.S2AThreadAlloc.Tick("声音 DSP"); // S2Arm: per-thread allocation accounting

                MailboxMessage message = _mailbox.ReceiveMessage();

                if (message == MailboxMessage.Stop)
                {
                    break;
                }

                if (message == MailboxMessage.RenderStart)
                {
                    _pauseEvent?.WaitOne();
                    Render();

                    _mailbox.SendResponse(MailboxMessage.RenderEnd);
                }
            }

            Logger.Info?.Print(LogClass.AudioRenderer, "Stopping audio processor");
            _mailbox.SendResponse(MailboxMessage.Stop);
        }

        private void Render()
        {
            long startTicks = PerformanceCounter.ElapsedNanoseconds;

            for (int i = 0; i < _sessionCommandList.Length; i++)
            {
                RendererSession session = _sessionCommandList[i];

                if (session != null)
                {
                    // S2Arm: 命令级异常已在 CommandList.Process 内按拍隔离；这里再兜一层非命令异常，
                    //   保证会话槽一定清空、下一拍还能继续（inline 模式下异常原本会打穿渲染线程）
                    try
                    {
                        session.CommandList.Process(OutputDevices[i]);
                    }
                    catch (Exception ex)
                    {
                        Logger.Error?.Print(LogClass.AudioRenderer, $"DSP session {i} failed; dropping period: {ex}");
                    }
                    finally
                    {
                        session.CommandList.Dispose();
                        _sessionCommandList[i] = null;
                    }
                }
            }

            long endTicks = PerformanceCounter.ElapsedNanoseconds;

            long elapsedTime = endTicks - startTicks;

            if (Constants.AudioProcessorMaxUpdateTime < elapsedTime)
            {
                Logger.Debug?.Print(LogClass.AudioRenderer, $"DSP too slow (exceeded by {elapsedTime - Constants.AudioProcessorMaxUpdateTime}ns)");
            }
        }

        public void Dispose()
        {
            GC.SuppressFinalize(this);
            Dispose(true);
        }

        protected virtual void Dispose(bool disposing)
        {
            if (disposing)
            {
                _mailbox?.Dispose();
            }
        }
    }
}
