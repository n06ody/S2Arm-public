using Ryujinx.Audio.Integration;
using Ryujinx.Audio.Renderer.Dsp;
using Ryujinx.Audio.Renderer.Parameter;
using Ryujinx.Common.Logging;
using Ryujinx.Cpu;
using Ryujinx.Memory;
using System;
using System.Diagnostics;
using System.Linq;
using System.Threading;

namespace Ryujinx.Audio.Renderer.Server
{
    /// <summary>
    /// The audio renderer manager.
    /// </summary>
    public class AudioRendererManager : IDisposable
    {
        /// <summary>
        /// Lock used for session allocation.
        /// </summary>
        private readonly Lock _sessionLock = new();

        /// <summary>
        /// Lock used to control the <see cref="AudioProcessor"/> running state.
        /// </summary>
        private readonly Lock _audioProcessorLock = new();

        /// <summary>
        /// The session ids allocation table.
        /// </summary>
        private readonly int[] _sessionIds;

        /// <summary>
        /// The events linked to each session.
        /// </summary>
        private IWritableEvent[] _sessionsSystemEvent;

        /// <summary>
        /// The <see cref="AudioRenderSystem"/> sessions instances.
        /// </summary>
        private readonly AudioRenderSystem[] _sessions;
        // S2Arm 即时存档：会话表与会话锁（持锁时工作线程停在两次处理之间）
        public AudioRenderSystem[] S2ASessions => _sessions;
        public Lock S2ASessionLock => _sessionLock;

        /// <summary>
        /// The count of active sessions.
        /// </summary>
        private int _activeSessionCount;

        /// <summary>
        /// The worker thread used to run <see cref="SendCommands"/>.
        /// </summary>
        private Thread _workerThread;

        /// <summary>
        /// Indicate if the worker thread and <see cref="AudioProcessor"/> are running.
        /// </summary>
        private bool _isRunning;

        /// <summary>
        /// The audio device driver to create audio outputs.
        /// </summary>
        private IHardwareDeviceDriver _deviceDriver;

        /// <summary>
        /// Tick source used to measure elapsed time.
        /// </summary>
        public ITickSource TickSource { get; }

        /// <summary>
        /// The <see cref="AudioProcessor"/> instance associated to this manager.
        /// </summary>
        public AudioProcessor Processor { get; }

        /// <summary>
        /// The dispose state.
        /// </summary>
        private int _disposeState;

        /// <summary>
        /// Create a new <see cref="AudioRendererManager"/>.
        /// </summary>
        /// <param name="tickSource">Tick source used to measure elapsed time.</param>
        public AudioRendererManager(ITickSource tickSource)
        {
            Processor = new AudioProcessor();
            TickSource = tickSource;
            _sessionIds = new int[Constants.AudioRendererSessionCountMax];
            _sessions = new AudioRenderSystem[Constants.AudioRendererSessionCountMax];
            _activeSessionCount = 0;

            for (int i = 0; i < _sessionIds.Length; i++)
            {
                _sessionIds[i] = i;
            }
        }

        /// <summary>
        /// Initialize the <see cref="AudioRendererManager"/>.
        /// </summary>
        /// <param name="sessionSystemEvents">The events associated to each session.</param>
        /// <param name="deviceDriver">The device driver to use to create audio outputs.</param>
        public void Initialize(IWritableEvent[] sessionSystemEvents, IHardwareDeviceDriver deviceDriver)
        {
            _sessionsSystemEvent = sessionSystemEvents;
            _deviceDriver = deviceDriver;
        }

        /// <summary>
        /// Get the work buffer size required by a session.
        /// </summary>
        /// <param name="parameter">The user configuration</param>
        /// <returns>The work buffer size required by a session.</returns>
        public static ulong GetWorkBufferSize(ref AudioRendererConfiguration parameter)
        {
            return AudioRenderSystem.GetWorkBufferSize(ref parameter);
        }

        /// <summary>
        /// Acquire a new session id.
        /// </summary>
        /// <returns>A new session id.</returns>
        private int AcquireSessionId()
        {
            lock (_sessionLock)
            {
                int index = _activeSessionCount;

                Debug.Assert(index < _sessionIds.Length);

                int sessionId = _sessionIds[index];

                _sessionIds[index] = -1;

                _activeSessionCount++;

                Logger.Info?.Print(LogClass.AudioRenderer, $"Registered new renderer ({sessionId})");

                return sessionId;
            }
        }

        /// <summary>
        /// Release a given <paramref name="sessionId"/>.
        /// </summary>
        /// <param name="sessionId">The session id to release.</param>
        private void ReleaseSessionId(int sessionId)
        {
            lock (_sessionLock)
            {
                Debug.Assert(_activeSessionCount > 0);

                int newIndex = --_activeSessionCount;

                _sessionIds[newIndex] = sessionId;
            }

            Logger.Info?.Print(LogClass.AudioRenderer, $"Unregistered renderer ({sessionId})");
        }

        /// <summary>
        /// Check if there is any audio renderer active.
        /// </summary>
        /// <returns>Returns true if there is any audio renderer active.</returns>
        private bool HasAnyActiveRendererLocked()
        {
            foreach (AudioRenderSystem renderer in _sessions)
            {
                if (renderer != null)
                {
                    return true;
                }
            }

            return false;
        }

        /// <summary>
        /// Start the <see cref="AudioProcessor"/> and worker thread.
        /// </summary>
        private void StartLocked()
        {
            try
            {
                // TODO: virtual device mapping (IAudioDevice)
                Processor.Start(_deviceDriver);

                Thread worker = new(SendCommands)
                {
                    Name = "AudioRendererManager.Worker",
                };
                _isRunning = true;
                worker.Start();
                _workerThread = worker;
            }
            catch
            {
                _isRunning = false;
                Processor.Stop();
                throw;
            }
        }

        /// <summary>
        /// Stop the <see cref="AudioProcessor"/> and worker thread.
        /// </summary>
        private void StopLocked()
        {
            _isRunning = false;

            _workerThread?.Join();
            _workerThread = null;
            Processor.Stop();

            Logger.Info?.Print(LogClass.AudioRenderer, "Stopped audio renderer");
        }

        /// <summary>
        /// Stop sending commands to the <see cref="AudioProcessor"/> without stopping the worker thread.
        /// </summary>
        public void StopSendingCommands()
        {
            lock (_sessionLock)
            {
                foreach (AudioRenderSystem renderer in _sessions)
                {
                    renderer?.Disable();
                }
            }

            lock (_audioProcessorLock)
            {
                if (_isRunning)
                {
                    StopLocked();
                }
            }
        }

        /// <summary>
        /// Worker main function. This is used to dispatch audio renderer commands to the <see cref="AudioProcessor"/>.
        /// </summary>
        private void SendCommands()
        {
            Logger.Info?.Print(LogClass.AudioRenderer, "Starting audio renderer");

            // S2Arm: this thread runs the 5 ms period loop (and the DSP itself when inline, the default): real-time scheduling,
            // budget half a period (render ≤ 3 ms on Mac at the worst seen), see Ryujinx.Common.S2ARealtime
            long period = Ryujinx.Audio.Constants.AudioProcessorMaxUpdateTimeTarget;
            Ryujinx.Common.S2ARealtime.Promote("声音渲染线程", period, period / 2, period);

            Processor.Wait();

            long s2aCmd = 0;

            while (_isRunning)
            {
                Ryujinx.Common.S2AThreadAlloc.Tick("声音渲染调度"); // S2Arm: per-thread allocation accounting

                long s2aT0 = System.Diagnostics.Stopwatch.GetTimestamp();
                AudioRenderSystem.S2ARenderLockWaitTicks = 0;
                Volatile.Write(ref AudioRenderSystem.S2AUpdateHoldMaxTicks, 0);

                lock (_sessionLock)
                {
                    long s2aT1 = System.Diagnostics.Stopwatch.GetTimestamp();

                    foreach (AudioRenderSystem renderer in _sessions)
                    {
                        renderer?.SendCommands();
                    }

                    s2aT0 = s2aT1 - s2aT0;   // session lock wait
                    s2aCmd = System.Diagnostics.Stopwatch.GetTimestamp() - s2aT1;
                }

                long s2aT2 = System.Diagnostics.Stopwatch.GetTimestamp();
                S2ATaskFaults.Read(out long s2aF0, out long s2aP0);
                Processor.Signal();
                // S2Arm (#1598): the wait for a frozen guest to resume is not DSP time (inline Signal waits on the pause event first)
                long s2aDsp = System.Diagnostics.Stopwatch.GetTimestamp() - s2aT2 - Processor.S2ALastPauseWaitTicks;
                S2ATaskFaults.Read(out long s2aF1, out long s2aP1);
                S2ALogSlowPeriod(s2aT0, s2aCmd, s2aDsp, s2aF1 - s2aF0, s2aP1 - s2aP0);
                Processor.Wait();
            }
        }

        // S2Arm (11-13 断音): a period that took > 10 ms on this thread (the output holds ~30 ms): where it went. Session lock, building
        //   the command lists (of which: waiting for a renderer lock the game's RequestUpdate holds; longest Update hold that ended in the
        //   period), the DSP itself. At most 40 lines a run
        private static int s_s2aSlowLines;

        //   faults / pageins: whole-process fault and page-in counts during the DSP pass (#1240: a DSP pass of 0.3–2 s with no lock wait and no
        //   command time on an 8 GB Mac, where guest memory is file-backed; page-ins here say the pass waited for pages)
        private static void S2ALogSlowPeriod(long sessionWait, long cmd, long dsp, long faults, long pageins)
        {
            long f = System.Diagnostics.Stopwatch.Frequency;
            long total = sessionWait + cmd + dsp;

            if (total * 1000 < 10 * f)
            {
                return;
            }

            long hold = Interlocked.Exchange(ref AudioRenderSystem.S2AUpdateHoldMaxTicks, 0);

            if (Interlocked.Increment(ref s_s2aSlowLines) > 40)
            {
                return;
            }

            Console.Error.WriteLine($"[声] 渲染一拍用了 {total * 1000.0 / f:F1} ms：等会话锁 {sessionWait * 1000.0 / f:F1} ms、生成命令 {cmd * 1000.0 / f:F1} ms" +
                                    $"（其中等游戏 Update 占着的锁 {AudioRenderSystem.S2ARenderLockWaitTicks * 1000.0 / f:F1} ms；这一拍里游戏 Update 最长持锁 {hold * 1000.0 / f:F1} ms）、DSP {dsp * 1000.0 / f:F1} ms（这段里全进程缺页 {faults} 次、读盘换入 {pageins} 次）");
        }

        /// <summary>
        /// Register a new <see cref="AudioRenderSystem"/>.
        /// </summary>
        /// <param name="renderer">The <see cref="AudioRenderSystem"/> to register.</param>
        private void Register(AudioRenderSystem renderer)
        {
            lock (_sessionLock)
            {
                _sessions[renderer.GetSessionId()] = renderer;
            }

            lock (_audioProcessorLock)
            {
                if (!_isRunning)
                {
                    StartLocked();
                }
            }
        }

        /// <summary>
        /// Unregister a new <see cref="AudioRenderSystem"/>.
        /// </summary>
        /// <param name="renderer">The <see cref="AudioRenderSystem"/> to unregister.</param>
        internal void Unregister(AudioRenderSystem renderer)
        {
            lock (_sessionLock)
            {
                int sessionId = renderer.GetSessionId();

                _sessions[renderer.GetSessionId()] = null;

                ReleaseSessionId(sessionId);
            }

            lock (_audioProcessorLock)
            {
                if (_isRunning && !HasAnyActiveRendererLocked())
                {
                    StopLocked();
                }
            }
        }

        /// <summary>
        /// Open a new <see cref="AudioRenderSystem"/>
        /// </summary>
        /// <param name="renderer">The new <see cref="AudioRenderSystem"/></param>
        /// <param name="memoryManager">The memory manager that will be used for all guest memory operations.</param>
        /// <param name="parameter">The user configuration</param>
        /// <param name="appletResourceUserId">The applet resource user id of the application.</param>
        /// <param name="workBufferAddress">The guest work buffer address.</param>
        /// <param name="workBufferSize">The guest work buffer size.</param>
        /// <param name="processHandle">The process handle of the application.</param>
        /// <returns>A <see cref="ResultCode"/> reporting an error or a success.</returns>
        public ResultCode OpenAudioRenderer(
            out AudioRenderSystem renderer,
            IVirtualMemoryManager memoryManager,
            ref AudioRendererConfiguration parameter,
            ulong appletResourceUserId,
            ulong workBufferAddress,
            ulong workBufferSize,
            uint processHandle)
        {
            int sessionId = AcquireSessionId();

            AudioRenderSystem audioRenderer = new(this, _sessionsSystemEvent[sessionId]);

            // TODO: Eventually, we should try to use the guest supplied work buffer instead of allocating
            // our own. However, it was causing problems on some applications that would unmap the memory
            // before the audio renderer was fully disposed.
            // S2Arm: native memory owned by the renderer (freed on dispose) and only as much as the renderer needs, instead of a pinned
            // managed array of the game-supplied size (iPhone low address space is scarce; the game may pass a much larger buffer)
            ulong needed = Math.Min(workBufferSize, AudioRenderSystem.GetWorkBufferSize(ref parameter));
            Memory<byte> workBufferMemory = audioRenderer.AllocateOwnedWorkBuffer((int)needed);

            ResultCode result = audioRenderer.Initialize(
                ref parameter,
                processHandle,
                workBufferMemory,
                workBufferAddress,
                workBufferSize,
                sessionId,
                appletResourceUserId,
                memoryManager);

            if (result == ResultCode.Success)
            {
                try
                {
                    Register(audioRenderer);
                    renderer = audioRenderer;
                }
                catch
                {
                    audioRenderer.Dispose();
                    throw;
                }
            }
            else
            {
                ReleaseSessionId(sessionId);

                audioRenderer.FreeOwnedWorkBuffer();

                renderer = null;
            }

            return result;
        }

        public void Dispose()
        {
            GC.SuppressFinalize(this);

            if (Interlocked.CompareExchange(ref _disposeState, 1, 0) == 0)
            {
                Dispose(true);
            }
        }

        protected virtual void Dispose(bool disposing)
        {
            if (disposing)
            {
                // Clone the sessions array to dispose them outside the lock.
                AudioRenderSystem[] sessions;

                lock (_sessionLock)
                {
                    sessions = _sessions.ToArray();
                }

                foreach (AudioRenderSystem renderer in sessions)
                {
                    renderer?.Dispose();
                }

                lock (_audioProcessorLock)
                {
                    if (_isRunning)
                    {
                        StopLocked();
                    }
                }

                Processor.Dispose();
            }
        }
    }
}
