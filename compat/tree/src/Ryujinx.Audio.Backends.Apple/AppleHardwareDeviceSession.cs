using Ryujinx.Audio.Backends.Common;
using Ryujinx.Audio.Common;
using Ryujinx.Memory;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Runtime.Versioning;
using Ryujinx.Audio.Backends.Apple.Native;   // 平台例外：Apple 后端平台实现内部引用本平台 Native 布局类型
using static Ryujinx.Audio.Backends.Apple.Native.AudioToolbox;

namespace Ryujinx.Audio.Backends.Apple
{
    [SupportedOSPlatform("macos")]
    [SupportedOSPlatform("ios")]
    class AppleHardwareDeviceSession : HardwareDeviceSessionOutputBase, Ryujinx.Audio.Integration.IS2AOutputDepth   // 平台例外：苹果会话自己实现中性的在途深度接口，共享代码只认接口、不引用苹果类型
    {
        // S2Arm: a pool of 32 device buffers of up to 50 ms each, filled as soon as samples are queued (DeviceBufferPump), instead of
        // 3 buffers of 10 ms refilled only from the device callback. 50 ms holds a whole game buffer (witcher3 audout: 1024 frames =
        // 21.3 ms; at 20 ms each one went out as 960 + 64 frames, two hand-backs per buffer)
        private const int NumBuffers = 32;
        private const int BufferMilliseconds = 50;
        // S2Arm (10-22 杂音): 起播前预充的在途下限（毫秒，约 3 个游戏缓冲 21.3 ms）。凑够才让设备开播，一开声就有深度；
        // 安静游戏凑不够时 StartFallbackMilliseconds 后照常开播。
        private const int PrimeMilliseconds = 64;
        private const long StartFallbackMilliseconds = 100;
        // S2Arm (11-09): renderer output (audren; AudioBuffer.Data holds the samples, see AudioBuffer.cs) is a steady 5 ms
        // producer and does not have audout's small-queue start underruns, which the 64 ms pre-roll was added for. A renderer
        // session lowers the start gate to 0 on its first buffer: with the push pump the 64 ms pre-roll otherwise becomes
        // permanent in-flight depth (Mac built-in speakers, synthetic audren producer: 115 ms with 64 ms pre-roll vs 55 ms with 0). The cushion
        // depth and the push / anti-GC behavior are unchanged. S2A_AUDIO_RENDERER_PRIME_MS=<ms> for A/B (default 0; 64 = old).
        private const int RendererPrimeMilliseconds = 0;
        private static readonly int _rendererPrimeMilliseconds = ParsePrime(Environment.GetEnvironmentVariable("S2A_AUDIO_RENDERER_PRIME_MS"));

        private readonly AppleHardwareDeviceDriver _driver;
        // S2Arm: queued buffers as (identifier, sample count) in a ring under _queueLock, instead of ConcurrentQueue<AppleAudioBuffer>:
        // that allocated an object per buffer, and its TryPeek marks the segment as preserved so dequeues never free slots
        // and every few buffers a new segment is allocated. Only the head can be partially played (_headPlayed).
        private (ulong DriverIdentifier, ulong SampleCount)[] _queued = new (ulong, ulong)[16];
        private int _queuedHead;
        private int _queuedCount;
        private ulong _headPlayed;
        private readonly object _queueLock = new();
        // S2Arm: queued samples (guest buffers pulled on demand, renderer output copied into at most 1 s of audio), instead of a
        // DynamicRingBuffer copy of everything queued; see AudioSampleQueue. The pump moves them into the device buffers
        private Pump _pump;
        private AudioSampleQueue _samples;
        // S2Arm (11-02 Pokopia iPhone 断续): queued samples count as played for the game one device pull ahead of the hand-backs
        //   (see PlaybackCredit); bursts of hand-backs ≤ 2 ms apart are one pull; the lead is at most 100 ms
        private PlaybackCredit _credit;
        // S2Arm (11-06 时之笛 iPhone 读档后永久静音): the queue's timeline ran ahead of the samples after it ran dry once (see QueueTimelineWatch).
        //   Counted on the device callback (hand-backs that left nothing in flight, bytes handed back), read by the watchdog every tick
        private QueueTimelineWatch _timeline;
        private int _zeroReturns;
        private long _returnedBytes, _timelineTicks;
        // S2Arm (#1921): frames enqueued since the queue was created or last stopped = the end of what it was given on its own timeline
        //   (QueueTimelineWatch.PastSubmitted). Written by the pump (one writer at a time), reset with the queue
        private long _enqueuedFrames;
        // 这 10 秒里交还判据起疑、但队列时钟没越过交付量（零余量按时播）的次数，与时钟减交付量的最大值；看门狗线程独占
        private int _suspectCalm;
        private long _suspectMaxPast = long.MinValue, _suspectWindowTicks;
        // S2Arm (11-02 Pokopia iPhone 杂音): releases each queued buffer when the credit's play head reaches its end, one per buffer
        //   like a Switch (see PlaybackCredit), instead of a whole device pull at each hand-back. Woken by hand-backs and new buffers
        private readonly AutoResetEvent _paceEvent = new(false);
        private Thread _paceThread;   // S2Arm (11-19): Dispose waits for it to leave; one stuck in a hung game's buffer keeps the driver's shared events alive
        /// <summary>S2Arm (11-19): the pace thread is still running after Dispose (stuck reading a hung game's buffer). The driver then keeps the
        /// shared update / pause events instead of disposing them: the thread may still Set them when it comes back.</summary>
        public bool S2APaceThreadAlive => _paceThread?.IsAlive == true;
        // S2Arm (11-14 iPhone 实机 声音服务卡死): slots the device handed back, one bit each (NumBuffers = 32), set by OutputCallback and
        //   drained by the pace thread (S2ADrainReturned). The AudioQueue callback thread belongs to the audio system: it must never
        //   wait on our locks or read guest memory. It did both (pump lock, then Pump → AudioSampleQueue.Read → ReadSamples from the
        //   game's audio-out buffers): a stalled game (iPhone 实机 第 17515 帧, guest memory file-backed) can block it for good, and a
        //   process killed with the audio system's thread stuck in our code left the phone's audio service broken until a reboot
        //   (every later launch: first enqueue 11 s, repeated media-services resets, session activation failing)
        private int _returnedMask;
        // S2Arm (D-J7ZC 巫师3 iOS 中断后中止): 无设备播放。iOS 音频中断（来电、闹钟、Siri、别的 App 抢声音）期间系统停了输出队列，设备不再交还，
        //   游戏交出的声音一直不算播完；游戏照跑（只是失去焦点），它的音频线程约 1 秒收不到交还就自己关掉音频输出再重开（诊断报告 410 次中断里
        //   31 次紧跟「Unregistered output」，间隔都约 1.03 秒）。多数游戏重开没事；巫师3 重开路径没先销毁上一个 SystemEvent，
        //   nn::audio::OpenDefaultAudioOut 在 AttachReadableHandleToSystemEvent 前查到事件已初始化就中止（sdk 反汇编：OpenDefaultAudioOut+0x110
        //   ldrb [事件+0x28] 非 0 → AbortImpl）。Switch 的声音硬件从不停交还，这条路径真机上走不到。
        //   所以中断期间不停交还：停掉真队列（在途的收回、照常记作播完），之后泵照常把声音交出来但不进 AudioQueue，按采样率在墙钟上逐块
        //   「交还」（送数线程 PaceLoop 到点置 _returnedMask，与设备回调同一条路），游戏看到的播放节奏与有设备时一样，只是没声。
        //   中断结束 / 媒体服务重置走原来的重启（S2ARecover 在停泵期间退出无设备播放、收回其在途块，再交真队列开播）。
        //   进入：看门狗见「中断开始」通知（≤250 ms）；任何原因（11-23 起不限系统中断：路由切换、失焦未冻结……）设备超过 VirtualAfterMs 不回调都进。
        //   退出也不只靠「中断结束」通知（会漏）：无设备期间每 10 秒试一次重新激活音频会话，成功即重启真队列；中断还在时激活失败、不动
        private bool _virtual;
        private bool _virtualForInterrupt;   // 这次无设备播放是不是因系统中断进的：是就等「中断结束」/ 每 10 秒试重新激活，不是就 VirtualProbeMs 后试回设备
        private readonly object _virtualLock = new();
        private readonly int[] _virtualSlot = new int[NumBuffers];
        private readonly long[] _virtualDue = new long[NumBuffers];   // 这块按采样率播完的时刻（Stopwatch）
        private int _virtualHead, _virtualCount;
        private long _virtualEnd, _virtualFrames;   // 末块播完的时刻；本次无设备期间交还的帧数（只给日志）
        private long _virtualPausedAt;   // 游戏 Stop 的时刻（0 = 没停）：停着不交还，Start 时余下的块整体顺延
        private bool _closedStuck;   // S2Arm (11-14): Dispose left the queue allocated (TrySuspend failed): a Pump returning later submits nothing
        private const int CreditBurstGapMicroseconds = 2000;
        private const int CreditMaxLeadMilliseconds = 100;
        private readonly ManualResetEvent _updateRequiredEvent;

        private readonly AudioQueueOutputCallback _callbackDelegate;
        // Free must clear the actual field: calling it on a readonly GCHandle frees
        // a defensive copy, leaving a stale slot that another object can reuse.
        private GCHandle _gcHandle;
        private bool _disposed;

        private nint _audioQueue;
        private readonly nint[] _audioQueueBuffers = new nint[NumBuffers];
        // S2Arm (11-12 #767): 交还回调的缓冲→槽号查表——建队时随缓冲整表建好、原子发布（见 CreateQueue），回调免锁只读
        //   （见 OutputCallback）；回调是实时线程、每次交还都走，不该每次对 32 槽线性扫
        private Dictionary<nint, int> _bufferSlots;

        // S2Arm (#189): guest bytes/frame (6-channel PCM16) and device bytes/frame (stereo after the explicit downmix
        //   stage). Equal for every other configuration.
        private readonly int _guestBytesPerFrame;
        private readonly int _deviceChannelCount;
        // S2Arm (#189): null when the 6-channel stream goes to the queue as is (native, the default) or the session is not 6-channel.
        private readonly AppleOutputProcessor _processor;
        private readonly ConvertingReader _convertingReader;
        private byte[] _convertedData;

        private readonly int _bytesPerFrame;

        private ulong _playedSampleCount;
        private bool _started;
        private bool _primeConfigured;   // S2Arm (11-09): the first queued buffer decided the renderer/audout start gate (see ConfigurePrimeFloor)
        private bool _rendererProducer;  // S2Arm (11-12 功耗): 同一个首缓冲定性——渲染器（audren）缓冲样本在 Data 里、audout 客体缓冲 Data 空，用于播完唤醒的门（见 ProcessPlayedSamples）
        // S2Arm (10-22 杂音): AudioQueue 是否已 AudioQueueStart（预充满足或兜底后才启播）；_startTicks = Start 时刻
        private bool _queueRunning;
        private long _startTicks;
        private float _volume = 1f;

        private readonly object _lock = new();

        // S2Arm: underrun counters / S2A_AUDIO_WAV capture, see S2AOutputDiag
        private readonly S2AOutputDiag _diag;

        // S2Arm (10-23 维护者 塞尔达「杂音后没声」): 输出流自愈。开着播、在途有数据，设备却超过 VirtualAfterMs 不回调 = 设备停了
        //   （系统停了队列：会话被打断 / 路由切换 / 媒体服务重置……，我们不会收到任何通知，之前就此永久没声）：
        //   iOS 重新激活音频会话 → 同步停队列 → 收回在途缓冲 → 重新排进去 → 开播。看门狗线程每 250 ms 查一次已登记会话。
        //   S2A_AUDIO_HEAL=0 关（同包 A/B）；S2A_AUDIO_TEST_STALL_AT=<秒>：开播后到点暂停一次队列（模拟系统停播，测试用）
        // S2Arm (11-23 D-GJCX / D-4T8M 宝可梦剑中止)：交还永不停。Switch 的声音硬件从不停交还；游戏在跑、设备却不回调时（系统中断、路由切换、
        //   失焦而客机还没冻结……），游戏约 0.8–1 秒收不到交还就自己关掉音频输出再重开，有的游戏重开路径不销毁上一个 SystemEvent，
        //   nn::audio::OpenDefaultAudioOut 查到事件已初始化就中止（sdk：注册缓冲事件后 ldrb [事件+0x28] 非 0 → Abort；CloseAudioOut 不碰这个事件，
        //   销毁归游戏，真机上这条重开路径走不到）。所以不论原因，设备超过 VirtualAfterMs 不回调就改走无设备播放（按采样率照常交还），
        //   远早于游戏的阈值；系统中断时等「中断结束」/ 每 10 秒试重新激活，其余原因 VirtualProbeMs 后就试回设备，回不去再进无设备播放
        private const int VirtualAfterMs = 200;
        private const int VirtualProbeMs = 500;
        private const int HealBackoffMs = 2000;
        // #1921：队列时钟越过交付量多少帧算真跑前（10 ms @48k；正常播放时时钟不会越过交付量，留一点读数时差）
        private const long AheadToleranceFrames = 480;
        // S2Arm (#1598 Mac 无声)：一次交设备（AudioQueueEnqueueBuffer，问音频服务进程）卡这么久 = 输出队列卡死，看门狗重启它。
        //   正常是微秒级；iPhone 上问音频服务的调用偶发最长约 50 ms（见 Pump 头注释）。玩家 M2 局：渲染一拍里「DSP」从 10 ms 级跳到 340 ms → 2.3 / 4.9 / 6.7 s、
        //   随后停流全无声，切窗口走解冻路径重启输出队列后立刻回到 10 ms 级——慢的是交设备这一步而不是混音计算（快路径是进程级静态开关，重启队列碰不到它）。
        //   原兜底只看「设备多久没回调、在途有数据」，交设备本身卡住时生产方被拖住、在途见底，兜底不触发；iOS 有回调静默兜底，Mac 这条从来接不住
        private const int EnqueueStuckMs = 250;
        private long _enqStuckTicks;   // 上次看门狗以来最长的一次交设备（Submit 写，看门狗取走清零）
        // 测试钩子：S2A_AUDIO_TEST_ENQ_STALL_AT=<秒>:<毫秒> —— 开播后到点，一次交设备在计时窗口里睡这么久，模拟输出队列卡死（服务层单测 --apple-audio-enqueue-stuck）
        private static readonly (int AtS, int Ms) _testEnqStall = ParseProdStall(Environment.GetEnvironmentVariable("S2A_AUDIO_TEST_ENQ_STALL_AT"));
        private bool _testEnqStallDone;
        private static readonly bool _healOn = Environment.GetEnvironmentVariable("S2A_AUDIO_HEAL") != "0";
        // S2Arm (#1632 / #1598)：重启输出队列只看返回码，不知道设备有没有真的重新播。重启后按退避窗口核一次：窗口里设备交还的帧数不到墙钟应播的一半、
        //   而在途还有数据（不缺声音）、期间没有失焦冻结 = 没恢复 → 再重启，窗口翻倍（2、4、8… 秒，封顶 30 秒）；够一半 = 恢复，打一行。
        //   玩家 M2 局：重启后 10 秒回调 994 / 653 次（正常约 2000）、真缺口 5.0 / 6.7 秒，回调没断，「设备 1 秒没回调」那条兜底接不住
        private const int VerifyMaxMs = 30000;
        private long _verifyTicks, _verifyPlayed0, _verifySeq;
        private int _verifyFails;
        // 测试钩子：S2A_AUDIO_TEST_SLOW_AFTER_HEAL=<n> —— 第 n 次重启之后、下一次重启之前，设备每交还 4 块只有 1 块记成「播完」，另外 3 块当作没播就交还
        //   （iOS 队列时间轴跑前就是这个样子：有回调、交还速率正常，声音没出来）。按交还事件数，不按看门狗拍点：11-21 门禁机负载高时原先「看门狗每拍轮流播 1 停 3」
        //   拍点漂移、没压住（2174 ms 交还 100800 帧，应约 104397），重启后核对误判「已恢复」。服务层单测 --apple-audio-recover-verify
        private static readonly int _testSlowAfterHeal = int.TryParse(Environment.GetEnvironmentVariable("S2A_AUDIO_TEST_SLOW_AFTER_HEAL"), out int sh) ? sh : 0;
        private int _testSlowTick, _testSlowSaid;
        private static readonly int _testStallAtS = int.TryParse(Environment.GetEnvironmentVariable("S2A_AUDIO_TEST_STALL_AT"), out int s) ? s : 0;
        private static readonly AppleHardwareDeviceSession[] _watched = new AppleHardwareDeviceSession[8];
        private static int _watchdogStarted;
        private long _lastCallbackTicks;
        private long _lastHealTicks;
        // S2Arm (#1384): 已处理过的客机冻结次数（S2AGuestFreeze.Seq）。建会话时取当前值，之前的冻结不算
        private long _thawSeq = Interlocked.Read(ref Ryujinx.Audio.Integration.S2AGuestFreeze.Seq);
        // S2Arm (D-HTHG 11-22)：解冻宽限。iOS 局切后台 / 回前台：客机冻结期间进程多半被系统挂起、看门狗不跑，回前台第一拍拿「上次回调」算停播，
        //   把整段后台时间（玩家局 26680 / 250589 / 1768335 ms）当成「设备没有回调」重启输出队列；回前台时系统重新激活音频会话，
        //   一次交设备卡约 500 ms（三局 499 / 518 / 520 ms，都紧挨着回前台时间线）被当成 #1598 的「输出队列卡死」重启。两种都是一次多余的断音。
        //   冻结中与解冻后 ThawGraceMs 内：停播起算点跟着刷新，交设备一次慢不重启（之后再慢照旧按 #1598 重启）。Mac 解冻本来就重启一次（#1384），不走这里
        private const int ThawGraceMs = 1500;
        private long _graceSeq = Interlocked.Read(ref Ryujinx.Audio.Integration.S2AGuestFreeze.Seq), _thawTicks;
        private int _heals;
        private bool _testStallDone;
        // 测试钩子：S2A_AUDIO_TEST_PRODUCER_STALL_AT=<秒>:<毫秒> —— 开播后到点在送声音的入口（QueueBuffer）睡一次，模拟 GC 停顿卡住生产方、吃光缓冲垫
        private static readonly (int AtS, int Ms) _testProdStall = ParseProdStall(Environment.GetEnvironmentVariable("S2A_AUDIO_TEST_PRODUCER_STALL_AT"));
        private bool _testProdStallDone;
        private static (int, int) ParseProdStall(string v)
        {
            if (string.IsNullOrEmpty(v)) return (0, 0);
            string[] p = v.Split(':');
            return p.Length == 2 && int.TryParse(p[0], out int a) && int.TryParse(p[1], out int m) ? (a, m) : (0, 0);
        }

        private static int ParsePrime(string v)
        {
            return int.TryParse(v, out int ms) && ms >= 0 ? ms : RendererPrimeMilliseconds;
        }
        // S2A_AUDIO_TEST_NOTE_AT=<秒>：began → 停队列 → 立即 ended；<秒>:<毫秒>：中断持续这么久才投递 ended（服务层单测 --apple-audio-interrupt-virtual）
        private static readonly (int AtS, int Ms) _testNote = ParseNote(Environment.GetEnvironmentVariable("S2A_AUDIO_TEST_NOTE_AT"));
        private static (int, int) ParseNote(string v)
        {
            if (int.TryParse(v, out int at))
            {
                return (at, 0);
            }

            return ParseProdStall(v);
        }
        private bool _testNoteEndDone;
        private static readonly int _testResetAtS = int.TryParse(Environment.GetEnvironmentVariable("S2A_AUDIO_TEST_RESET_AT"), out int r) ? r : 0;
        private bool _testNoteDone, _testResetDone;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void AudioQueueOutputCallback(
            nint userData,
            nint audioQueue,
            nint buffer);

        public AppleHardwareDeviceSession(
            AppleHardwareDeviceDriver driver,
            IVirtualMemoryManager memoryManager,
            SampleFormat requestedSampleFormat,
            uint requestedSampleRate,
            uint requestedChannelCount)
            : base(memoryManager, requestedSampleFormat, requestedSampleRate, requestedChannelCount)
        {
            _driver = driver;
            _updateRequiredEvent = driver.GetUpdateRequiredEvent();
            _callbackDelegate = OutputCallback;
            _guestBytesPerFrame = BackendHelper.GetSampleSize(requestedSampleFormat) * (int)requestedChannelCount;
            _deviceChannelCount = (int)requestedChannelCount;

            // S2Arm (#189): 显式下混——S2A_AUDIO_OUT=sys 时 6 声道混音在交给 AudioQueue 前由系统 AudioConverter 折成立体声。
            //   渲染器自己的回落下混（游戏没给 DownMixParameter 时 [1.0, 0.707, 0.251, 0.707]）没有归一化：FL+FC 各 0.9
            //   折到 1.536（2632/4800 采样削波）、六路全 0.9 折到 2.399（3484/4800 削波）；系统的显式下混归一化
            //   （前 0.4142、中置 / 同侧环绕 0.2929、LFE 丢弃，最坏 0.4142+2×0.2929=1.0）不削波。帧数不变，
            //   泵 / 播放信用 / 缓冲释放节奏不变，换档不改延迟；缺省 native 不走这里，行为同现状（渲染器自己下混）
            // S2Arm（#189 后续，5.1 虚拟环绕）：产品路径——用户选的设备类型 + 对应 VS 开（S2A_AUDIO_DEVICE=builtin /
            //   headphones + S2A_AUDIO_VS_SPEAKERS / S2A_AUDIO_VS_HEADPHONES）走 AUSM 虚拟环绕档（显式输出类型；
            //   .auto 在 5.1 路径上三路由实测全退化为 ExternalSpeakers，见 docs/坑记录.md）。external 永不虚拟环绕：
            //   驱动 ≥6 才报 5.1，这里走现有 6ch 直通队列 + 5.1 布局（无处理器）。优先级：CH2 / STEREO >
            //   S2A_AUDIO_OUT=sys / matrix（内部 A/B）> 设备类型 + VS > 缺省 builtin + VS 关（2ch，不套 AUSM）
            if (requestedChannelCount == 6)
            {
                string configuredMode = Environment.GetEnvironmentVariable("S2A_AUDIO_OUT");
                AppleOutputMode mode = AppleOutputModeSelector.FromEnvironment();
                AppleAudioDeviceType deviceType = AppleAudioDeviceTypeSelector.FromEnvironment();
                AppleOutputMode? spatialMode = deviceType switch
                {
                    AppleAudioDeviceType.BuiltInSpeakers when AppleAudioDeviceTypeSelector.VirtualSurround(deviceType) => AppleOutputMode.SpatialSpeakers,
                    AppleAudioDeviceType.Headphones when AppleAudioDeviceTypeSelector.VirtualSurround(deviceType) => AppleOutputMode.SpatialHeadphones,
                    _ => null,
                };

                if (mode != AppleOutputMode.Native)
                {
                    if (requestedSampleFormat != SampleFormat.PcmInt16)
                    {
                        Console.Error.WriteLine($"[声] 【红】S2A_AUDIO_OUT={mode} 只支持 PCM16 的 5.1（本次 {requestedSampleFormat}）：按 native 直交 AudioQueue");
                    }
                    else
                    {
                        _deviceChannelCount = 2;
                        _processor = AppleOutputProcessor.Create(mode, (int)requestedSampleRate, MaxFrames);
                        Console.Error.WriteLine($"[声] 5.1→2.0 档「{AppleOutputModeSelector.ModeName(_processor.EffectiveMode)}」：6 声道 FL FR FC LFE BL BR → 2 声道（S2A_AUDIO_OUT；一片最多 {MaxFrames} 帧）" +
                            (_processor.InitError != null ? $"；【红】器件没建起来，这一路按矩阵下混：{_processor.InitError}" : ""));
                    }
                }
                else if (spatialMode != null)
                {
                    if (requestedSampleFormat != SampleFormat.PcmInt16)
                    {
                        Console.Error.WriteLine($"[声] 【红】设备类型 {AppleAudioDeviceTypeSelector.DeviceName(deviceType)} + VS 开只支持 PCM16 的 5.1（本次 {requestedSampleFormat}）：按 native 直交 AudioQueue");
                    }
                    else
                    {
                        _deviceChannelCount = 2;
                        _processor = AppleOutputProcessor.Create(spatialMode.Value, (int)requestedSampleRate, MaxFrames);
                        Console.Error.WriteLine($"[声] 5.1→2.0 虚拟环绕档「{AppleOutputModeSelector.ModeName(_processor.EffectiveMode)}」（设备类型 {AppleAudioDeviceTypeSelector.DeviceName(deviceType)}、VS 开、{requestedSampleFormat}）：6 声道 FL FR FC LFE BL BR → AUSM → 2 声道（一片最多 {MaxFrames} 帧）" +
                            (_processor.InitError != null ? $"；【红】器件没建起来，这一路按矩阵下混：{_processor.InitError}" : ""));
                    }
                }
                else if (!string.IsNullOrWhiteSpace(configuredMode))
                {
                    Console.Error.WriteLine("[声] 5.1→2.0 档「native」：显式保持现状（渲染器自己下混）");
                }
            }

            _bytesPerFrame = BackendHelper.GetSampleSize(requestedSampleFormat) * _deviceChannelCount;
            _diag = S2AOutputDiag.Create(_bytesPerFrame, _deviceChannelCount, (int)requestedSampleRate, BackendHelper.GetSampleSize(requestedSampleFormat) * 8);
            _convertingReader = _processor != null ? new ConvertingReader(this, _processor) : null;

            _gcHandle = GCHandle.Alloc(this, GCHandleType.Normal);

            SetupAudioQueue();
            S2AWatch(this, true);
            _paceThread = new Thread(PaceLoop) { IsBackground = true, Name = "S2A.AudioPace", Priority = ThreadPriority.Highest };
            _paceThread.Start();
        }

        // S2Arm: 登记 / 注销看门狗（固定 8 槽，看门狗线程扫数组不分配）
        private static void S2AWatch(AppleHardwareDeviceSession s, bool add)
        {
            lock (_watched)
            {
                for (int i = 0; i < _watched.Length; i++)
                {
                    if (add ? _watched[i] == null : _watched[i] == s)
                    {
                        _watched[i] = add ? s : null;
                        break;
                    }
                }
            }

            if (add && Interlocked.Exchange(ref _watchdogStarted, 1) == 0)
            {
                new Thread(S2AWatchdogLoop) { IsBackground = true, Name = "S2A.AudioHeal" }.Start();
            }
        }

        private static void S2AWatchdogLoop()
        {
            S2AAudioNotes.Install();

            while (true)
            {
                Thread.Sleep(250);

                long now = Stopwatch.GetTimestamp();
                int notes = S2AAudioNotes.TakePending();

                // Removing a session during Dispose waits for a watchdog pass that already
                // selected it, so it cannot recover or touch a disposed AudioQueue.
                lock (_watched)
                for (int i = 0; i < _watched.Length; i++)
                {
                    AppleHardwareDeviceSession s = Volatile.Read(ref _watched[i]);

                    if (s == null)
                    {
                        continue;
                    }

                    s.S2ATestHooks(now);
                    s.S2APrimeFallback();

                    // 主路径：系统通知。中断结束 → 重新激活 + 重启；媒体服务重置 → 重建队列
                    if ((notes & (S2AAudioNotes.Ended | S2AAudioNotes.Reset)) != 0)
                    {
                        s.S2ARecover((notes & S2AAudioNotes.Reset) != 0 ? "系统通知：媒体服务重置" : "系统通知：中断结束", (notes & S2AAudioNotes.Reset) != 0, now, false);
                    }
                    else if ((notes & S2AAudioNotes.Began) != 0)
                    {
                        s.S2AEnterVirtual("系统通知：中断开始");
                    }
                    else if (Volatile.Read(ref s._virtual))
                    {
                        s.S2AVirtualProbe(now);
                    }
                    else
                    {
                        s.S2ACheckStall(now);
                    }
                }
            }
        }

        // 测试钩子（缺省全关）：S2A_AUDIO_TEST_STALL_AT 外部停队列（只能靠兜底接）；S2A_AUDIO_TEST_NOTE_AT 模拟一次中断
        //   （投递 began → 像系统那样停队列 → 投递 ended，走主路径）；S2A_AUDIO_TEST_RESET_AT 投递一次媒体服务重置
        private void S2ATestHooks(long now)
        {
            long since = now - _startTicks;

            // 持续一段的模拟中断：到点投递 ended（无设备播放时真队列没开着，放在下面的门前）
            if (_testNote.Ms > 0 && _testNoteDone && !_testNoteEndDone && since >= (long)_testNote.AtS * Stopwatch.Frequency + (long)_testNote.Ms * Stopwatch.Frequency / 1000)
            {
                _testNoteEndDone = true;
                Console.Error.WriteLine($"[声] 测试钩子：输出 #{_diag.Index} 模拟中断 {_testNote.Ms} ms 到点，投递 ended");
                S2AAudioNotes.PostForTest("AVAudioSessionInterruptionNotification", 0);
            }

            if (!_started || !_queueRunning)
            {
                return;
            }

            if (_testStallAtS > 0 && !_testStallDone && since >= (long)_testStallAtS * Stopwatch.Frequency)
            {
                _testStallDone = true;
                int rc = AudioQueuePause(_audioQueue);
                Console.Error.WriteLine($"[声] 测试钩子：输出 #{_diag.Index} 开播后第 {_testStallAtS} 秒外部暂停输出队列（不发任何通知，模拟系统悄悄停播），rc={rc}");
            }

            if (_testNote.AtS > 0 && !_testNoteDone && since >= (long)_testNote.AtS * Stopwatch.Frequency)
            {
                _testNoteDone = true;
                Console.Error.WriteLine($"[声] 测试钩子：输出 #{_diag.Index} 开播后第 {_testNote.AtS} 秒模拟中断（投递 began → 停队列 → {(_testNote.Ms > 0 ? $"{_testNote.Ms} ms 后" : "")}投递 ended）");
                S2AAudioNotes.PostForTest("AVAudioSessionInterruptionNotification", 1);
                AudioQueuePause(_audioQueue);

                if (_testNote.Ms == 0)
                {
                    S2AAudioNotes.PostForTest("AVAudioSessionInterruptionNotification", 0);
                }
            }

            if (_testSlowAfterHeal > 0 && _heals == _testSlowAfterHeal && Interlocked.Exchange(ref _testSlowSaid, 1) == 0)
            {
                Console.Error.WriteLine($"[声] 测试钩子：输出 #{_diag.Index} 第 {_testSlowAfterHeal} 次重启后每交还 4 块只记 1 块播完（设备有回调、交还照常，只算播了约四分之一）");
            }

            if (_testResetAtS > 0 && !_testResetDone && since >= (long)_testResetAtS * Stopwatch.Frequency)
            {
                _testResetDone = true;
                Console.Error.WriteLine($"[声] 测试钩子：输出 #{_diag.Index} 开播后第 {_testResetAtS} 秒模拟媒体服务重置");
                S2AAudioNotes.PostForTest("AVAudioSessionMediaServicesWereResetNotification", -1);
            }
        }

        // S2Arm (10-24 #164 动森开场视频黑屏): 预充期的兜底启播要有人按时检查。原先只在 Start / 交下一块缓冲时检查：
        //   视频播放器经 AudioOut 只交一块 1024 帧（21 ms < 预充下限 64 ms）、等它播完才交下一块，没人再来检查，设备永不开播，
        //   游戏线程空转等这块播完、视频按声音时钟一帧不要（iPad 局：输出 #2 回调 0 次、交设备 1024 帧，整段视频全黑）。看门狗每 250 ms 补一次检查
        private void S2APrimeFallback()
        {
            if (!Volatile.Read(ref _started) || _queueRunning)
            {
                return;
            }

            lock (_lock)
            {
                MaybeStartQueue();
            }
        }

        // 无设备播放中（见 _virtual）：每 10 秒试一次重新激活音频会话（「中断结束」通知会漏）。中断还在时激活失败、不动；成功即回到设备
        private void S2AVirtualProbe(long now)
        {
            // 不是因系统中断进的（路由切换、失焦未冻结、设备悄悄停播……）：设备多半马上能用，VirtualProbeMs 后就试回设备；回不去（重新开播失败 /
            //   随后又不回调）再进无设备播放，游戏这期间一直收到交还
            if (!_virtualForInterrupt)
            {
                if (now - _lastHealTicks >= (long)VirtualProbeMs * Stopwatch.Frequency / 1000)
                {
                    S2ARecover($"无设备播放 {(now - _lastHealTicks) * 1000 / Stopwatch.Frequency} ms 后试回设备（不在系统中断里，最近系统通知 {S2AAudioNotes.Last}）", false, now, false);
                }

                return;
            }

            if (now - _lastHealTicks < (long)HealBackoffMs * 5 * Stopwatch.Frequency / 1000)
            {
                return;
            }

            _lastHealTicks = now;

            if ((OperatingSystem.IsIOS() || OperatingSystem.IsTvOS()) && _driver.S2AReactivateSession())   // 平台例外：tvOS 与 iOS 走同一分支；Mac 没有音频会话、不会因中断进来
            {
                S2ARecover("无设备播放中重新激活音频会话成功", false, now, false);
            }
        }

        // 解冻宽限的判定（纯函数，服务层单测 --apple-audio-thaw-grace 直接测）：frozen / seq = 客机冻结状态与冻结次数；graceSeq / thawTicks = 上次见到的冻结次数、
        //   上次解冻时刻（调用方保存）。返回这一拍在不在宽限内（冻结中或解冻后 ThawGraceMs 内）；rebase = 停播起算点要刷新到 now（冻结中、或刚看到一次新的解冻）
        internal static bool S2AThawGraceStep(bool frozen, long seq, ref long graceSeq, ref long thawTicks, long now, long freq, out bool rebase)
        {
            rebase = frozen || seq != graceSeq;
            if (!frozen && seq != graceSeq)
            {
                graceSeq = seq;
                thawTicks = now;
            }

            return frozen || (thawTicks != 0 && now - thawTicks < (long)ThawGraceMs * freq / 1000);
        }

        // 兜底：开着播、在途有数据，设备却超过 VirtualAfterMs 不回调 → 无设备播放（见 VirtualAfterMs）。日志带上最近一条系统通知，免得悄悄盖住别的毛病
        private void S2ACheckStall(long now)
        {
            int zero = Interlocked.Exchange(ref _zeroReturns, 0);
            long returned = Interlocked.Exchange(ref _returnedBytes, 0), elapsed = now - _timelineTicks;
            long enq = Interlocked.Exchange(ref _enqStuckTicks, 0);

            _timelineTicks = now;

            if (!_started || !_queueRunning)
            {
                _timeline.Reset();
                _verifyTicks = 0;   // 游戏停播 / 没开播：这次重启不下结论

                return;
            }

            bool grace = false;
            if (!OperatingSystem.IsMacOS())   // 平台例外：苹果后端内部，Mac 解冻走 #1384 的重启（见下）
            {
                grace = S2AThawGraceStep(Ryujinx.Audio.Integration.S2AGuestFreeze.Frozen != 0, Interlocked.Read(ref Ryujinx.Audio.Integration.S2AGuestFreeze.Seq),
                                         ref _graceSeq, ref _thawTicks, now, Stopwatch.Frequency, out bool rebase);
                if (rebase)
                {
                    Volatile.Write(ref _lastCallbackTicks, now);   // 冻结 / 挂起的时间不算「设备没有回调」
                }

                if (grace && enq * 1000 >= (long)EnqueueStuckMs * Stopwatch.Frequency)
                {
                    Console.Error.WriteLine($"[声] 【黄】输出 #{_diag.Index}：解冻后 {(now - _thawTicks) * 1000 / Stopwatch.Frequency} ms 内一次交设备卡了 {enq * 1000 / Stopwatch.Frequency} ms（回前台时系统重新激活音频会话），按过渡处理、不重启输出队列");
                }
            }

            // #1598：交设备卡死（见 EnqueueStuckMs）→ 与解冻路径同一个动作：停队列、收回在途、重新开播。各平台都接，退避同下面的兜底
            if (!grace && enq * 1000 >= (long)EnqueueStuckMs * Stopwatch.Frequency && _healOn && now - _lastHealTicks >= (long)HealBackoffMs * Stopwatch.Frequency / 1000)
            {
                _timeline.Reset();
                S2ARecover($"兜底：一次交设备（AudioQueueEnqueueBuffer）卡了 {enq * 1000 / Stopwatch.Frequency} ms，输出队列卡死", false, now, true);

                return;
            }

            // S2Arm (#1384): 客机冻结（失焦暂停）放开后，macOS 也重置时间轴、重启一次输出队列。11-13 的前提「macOS 队列自己恢复」在部分外接输出上不成立：
            //   玩家外接 DP 显示器（48 kHz）失焦暂停切回后画面恢复、一直无声；他在解冻时重置时间轴 + 重启输出队列，同机两轮都好了。
            //   每次冻结只做一次，在解冻后第一次检查时做；iOS / tvOS 照旧走下面的时间轴自愈
            long freezeSeq = Interlocked.Read(ref Ryujinx.Audio.Integration.S2AGuestFreeze.Seq);
            if (freezeSeq != _thawSeq && Ryujinx.Audio.Integration.S2AGuestFreeze.Frozen == 0)
            {
                _thawSeq = freezeSeq;

                if (OperatingSystem.IsMacOS())   // 平台例外：苹果后端内部，iOS / tvOS 解冻由下面的时间轴自愈接住
                {
                    _timeline.Reset();
                    S2ARecover($"客机解冻（失焦暂停放开，第 {freezeSeq} 次）：重置时间轴、重启输出队列", false, now, false);

                    return;
                }
            }

            if (S2AVerifyRecover(now, freezeSeq))
            {
                return;
            }

            // S2Arm (11-13 断音): iOS only. A macOS queue recovers by itself (QueueTimelineWatch), and its criterion cannot tell a timeline
            //   ahead from a queue playing at zero margin: 0400486024DEC000 Mac 局第 26 秒 a 30 ms render stall left zero margin, this fired, and the
            //   synchronous stop / restart only cut the audio once more (zero-in-flight hand-backs 24 → 39 in the next window)
            // #1921：交还判据只算起疑，队列自己的时钟越过交付量才是真跑前（QueueTimelineWatch.PastSubmitted）；没越过 = 零余量按时播，不重启
            //   （重启本身就是一次断音，D-G8YT 40 秒 13 次）。时钟读不到按旧判据重启（永久静音那条不能漏）
            if ((OperatingSystem.IsIOS() || OperatingSystem.IsTvOS()) && _timeline.Tick(zero, returned, elapsed) && now - _lastHealTicks >= (long)HealBackoffMs * Stopwatch.Frequency / 1000)   // 平台例外：苹果后端内部，iOS 队列跑前要重启、macOS 队列自己恢复
            {
                long past = S2AQueuePastSubmitted();

                if (past == long.MinValue || QueueTimelineWatch.PastSubmitted(past, 0, AheadToleranceFrames))
                {
                    S2ARecover($"队列时间轴跑到声音前面（这 {elapsed * 1000 / Stopwatch.Frequency} ms 交还 {returned / _bytesPerFrame} 帧、{zero} 次交还后在途为 0；" +
                               (past == long.MinValue ? "队列时钟读不到，按交还判" : $"队列时钟越过交付量 {past} 帧") + "：欠载后交的缓冲排在已过去的时刻，没播就交还）",
                               false, now, false);

                    return;
                }

                _suspectCalm++;
                _suspectMaxPast = Math.Max(_suspectMaxPast, past);
            }

            if (_suspectCalm > 0 && now - _suspectWindowTicks >= 10 * Stopwatch.Frequency)
            {
                Console.Error.WriteLine($"[声] 输出 #{_diag.Index} 10 秒：交还判据起疑 {_suspectCalm} 次，队列时钟都没越过交付量（时钟 − 交付量 最大 {_suspectMaxPast} 帧，容差 {AheadToleranceFrames}）：零余量按时播，不重启");
                _suspectCalm = 0;
                _suspectMaxPast = long.MinValue;
                _suspectWindowTicks = now;
            }

            long stall = now - Volatile.Read(ref _lastCallbackTicks);

            if (stall < (long)VirtualAfterMs * Stopwatch.Frequency / 1000 || _pump.InFlightBytes == 0)
            {
                return;
            }

            if (!_healOn)
            {
                if (_heals++ == 0)
                {
                    Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：设备 {stall * 1000 / Stopwatch.Frequency} ms 没有回调（在途 {_pump.InFlightBytes / _bytesPerFrame} 帧），自愈已关（S2A_AUDIO_HEAL=0）");
                }

                return;
            }

            // Full thread dumps suspend every other thread while collecting stacks.
            // Opt in explicitly for diagnosis; normal audio recovery must not pause the game.
            if (s_stallDumpOn && Interlocked.Increment(ref s_stallDumps) <= 3)
            {
                try
                {
                    s2a_dump_threads($"声音设备 {stall * 1000 / Stopwatch.Frequency} ms 没有回调");
                }
                catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException)
                {
                    // 宿主之外（服务层单测直接跑托管代码）没有原生库：现场只是诊断，不影响下面的无设备播放
                    Console.Error.WriteLine($"[声] 全线程现场不可用（{e.GetType().Name}）");
                }
            }

            // 交还永不停（见 VirtualAfterMs）：不论系统中断、路由切换还是别的原因，先改走无设备播放让游戏照常收到交还，回设备交给 S2AVirtualProbe
            S2AEnterVirtual($"兜底：设备 {stall * 1000 / Stopwatch.Frequency} ms 没有回调{(S2AAudioNotes.Interrupted ? "（系统中断中）" : "")}，最近系统通知 {S2AAudioNotes.Last}");
        }

        private static readonly bool s_stallDumpOn = Environment.GetEnvironmentVariable("S2A_AUDIO_STALL_DUMP") == "1";
        private static int s_stallDumps;

        [DllImport("s2a_native")]
        private static extern int s2a_dump_threads(string why);

        [DllImport("s2a_native")]
        private static extern int s2a_audio_volume();   // runtime/src/game_volume.c：游戏音量 0–100（#1186）

        private static bool s_gameVolumeGone;   // 原生层没有这个导出（旧宿主 / 只链托管的单测）：按 100，不再试

        private static int S2AGameVolume()
        {
            if (s_gameVolumeGone)
                return 100;
            try { return s2a_audio_volume(); }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException) { s_gameVolumeGone = true; return 100; }
        }   // runtime/src/tdump.c：全线程宿主栈（自动玩家卡住、崩溃时同一份）

        // 重启后核对（见 VerifyMaxMs）：窗口到点才下结论；返回 true = 这一拍又重启了
        private bool S2AVerifyRecover(long now, long freezeSeq)
        {
            int windowMs = (int)Math.Min((long)HealBackoffMs << Math.Min(_verifyFails, 8), VerifyMaxMs);
            if (_verifyTicks == 0 || now - _verifyTicks < (long)windowMs * Stopwatch.Frequency / 1000)
            {
                return false;
            }

            long elapsed = now - _verifyTicks;
            long played = _diag.FramesPlayed - _verifyPlayed0, want = elapsed * RequestedSampleRate / Stopwatch.Frequency;
            _verifyTicks = 0;

            if (freezeSeq != _verifySeq || Ryujinx.Audio.Integration.S2AGuestFreeze.Frozen != 0)
            {
                return false;   // 期间失焦冻结过：交还少是因为游戏停了，解冻那条路径会重启，这里不下结论
            }

            long ms = elapsed * 1000 / Stopwatch.Frequency;
            if (played * 2 >= want)
            {
                Console.Error.WriteLine($"[声] 输出 #{_diag.Index}：重启后设备恢复播放（{ms} ms 交还 {played} 帧，应约 {want} 帧）{(_verifyFails > 0 ? $"，此前连续 {_verifyFails} 次没恢复" : "")}");
                _verifyFails = 0;

                return false;
            }

            if (_pump.InFlightBytes == 0)
            {
                return false;   // 设备手上没声音可播：交还少是游戏没送，不是设备没播
            }

            _verifyFails++;
            S2ARecover($"重启后设备没恢复播放：{ms} ms 交还 {played} 帧（应约 {want} 帧，不到一半），再重启（连续第 {_verifyFails} 次没恢复，" +
                       $"下次 {Math.Min((long)HealBackoffMs << Math.Min(_verifyFails, 8), VerifyMaxMs) / 1000} 秒后再核）", false, now, true);

            return true;
        }

        // #1921：队列时钟（播到的采样时刻）减开播 / 上次停队列以来交给它的帧数；队列没在播或读不到 → long.MinValue。只在看门狗线程、起疑时调
        private unsafe long S2AQueuePastSubmitted()
        {
            nint q = _audioQueue;

            if (q == nint.Zero || !Volatile.Read(ref _queueRunning))
            {
                return long.MinValue;
            }

            long* ts = stackalloc long[8];   // AudioTimeStamp 64 字节，mSampleTime（double）在偏移 0

            return AudioQueueGetCurrentTime(q, nint.Zero, ts, nint.Zero) == 0
                ? (long)*(double*)ts - Interlocked.Read(ref _enqueuedFrames)
                : long.MinValue;
        }

        // 恢复输出流：iOS 重新激活会话 → 同步停队列（重置时重建）→ 收回在途 → 重新排进去 → 开播
        private void S2ARecover(string why, bool rebuild, long now, bool red)
        {
            if (!_healOn)
            {
                Console.Error.WriteLine($"[声] {why}；自愈已关（S2A_AUDIO_HEAL=0），不处理");

                return;
            }

            lock (_lock)
            {
                if (_audioQueue == nint.Zero)
                {
                    return;
                }

                _lastHealTicks = now;
                long inFlightFrames = _pump.InFlightBytes / _bytesPerFrame;
                bool active = _driver.S2AReactivateSession();
                int stopRc = 0, newRc = 0;
                // 停 / 重建期间泵不交缓冲（审计 P1-27）：同步停队列时缓冲经回调交还，回调和游戏线程的 Pump 此时什么都不交；
                //   原先它们照交，收回又把队列仍持有的缓冲放回池里、这里再交一次 = 同一块缓冲入队两次。停失败（缓冲可能还在队列里）不收回
                bool wasVirtual = Volatile.Read(ref _virtual);
                long virtualFrames = _virtualFrames;
                int reclaimed = _pump.Restart(() =>
                {
                    // 退出无设备播放（见 _virtual）：真队列进入时已停空、此后没交过，无设备在途的槽随下面的收回一并放回
                    if (wasVirtual)
                    {
                        S2AVirtualClearLocked();
                        Volatile.Write(ref _virtual, false);
                    }

                    stopRc = AudioQueueStop(_audioQueue, true);

                    if (!rebuild)
                    {
                        // S2Arm (11-14): hand-backs noted during the stop are reclaimed with the rest (Restart); a bit left over would
                        //   recycle the slot again after it is resubmitted. A failed stop reclaims nothing: keep them
                        if (stopRc == 0)
                        {
                            Interlocked.Exchange(ref _returnedMask, 0);
                            Interlocked.Exchange(ref _enqueuedFrames, 0);   // 停队列时间轴归零，收回的缓冲随后重新交
                        }

                        return stopRc;
                    }

                    AudioQueueDispose(_audioQueue, true);
                    Interlocked.Exchange(ref _returnedMask, 0);
                    newRc = CreateQueue();

                    return 0;   // 旧队列连同它的缓冲已销毁：在途的槽全部收回
                });

                // 收回的真实样本没播，但当作已播完：让游戏的缓冲照常放回去（丢掉不到 1.6 秒声音），内部补垫不计入完成额度
                if (reclaimed > 0)
                {
                    _credit.Reclaimed(reclaimed);
                    S2ACredit();
                }

                int startRc = -1;

                // 游戏自己停着播（Stop）就只恢复好队列，等它 Start
                if (_started && newRc == 0)
                {
                    _pump.Pump();
                    S2ACredit();
                    startRc = AudioQueueStart(_audioQueue, nint.Zero);
                    _queueRunning = startRc == 0;
                }
                else
                {
                    _queueRunning = false;
                }

                // 重启后核对（S2AVerifyRecover）：开播成功才核，从此刻起算
                _verifyTicks = startRc == 0 ? Stopwatch.GetTimestamp() : 0;
                _verifyPlayed0 = _diag.FramesPlayed;
                _verifySeq = Interlocked.Read(ref Ryujinx.Audio.Integration.S2AGuestFreeze.Seq);

                Volatile.Write(ref _lastCallbackTicks, Stopwatch.GetTimestamp());

                Console.Error.WriteLine($"[声] {(red ? "【红】" : "")}输出 #{_diag.Index}：{why}（在途 {inFlightFrames} 帧{(wasVirtual ? $"；结束无设备播放，期间按采样率交还 {virtualFrames} 帧" : "")}）→ {(rebuild ? "重建" : "重启")}输出队列（第 {++_heals} 次）：" +
                    $"{((OperatingSystem.IsIOS() || OperatingSystem.IsTvOS()) ? $"重新激活音频会话 {(active ? "成功" : "失败")}、" : "")}停队列 rc={stopRc}、收回 {reclaimed / _bytesPerFrame} 帧" +   // 平台例外：tvOS 与 iOS 走同一分支
                    $"{(rebuild ? $"、新建队列 rc={newRc}" : "")}、重新开播 rc={startRc}{(startRc == 0 ? "" : "（没开播：游戏停着或开播失败，下一笔声音到时再试）")}");

                // 开播失败（系统中断还在、路由还没切好……）：回到无设备播放，别让游戏等交还（见 VirtualAfterMs）
                if (_started && startRc != 0)
                {
                    S2AEnterVirtual($"重新开播失败（rc={startRc}{(S2AAudioNotes.Interrupted ? "，系统中断中" : "")}）");
                }
            }
        }

        private void SetupAudioQueue()
        {
            lock (_lock)
            {
                int result = CreateQueue();

                if (result != 0)
                {
                    throw new InvalidOperationException($"AudioQueueNewOutput failed: {result}");
                }

                _pump = new Pump(this, (int)BufferBytes);
                _samples = _pump.Samples;
                _credit = new PlaybackCredit(_bytesPerFrame, (long)_bytesPerFrame * RequestedSampleRate,
                                             (long)_bytesPerFrame * RequestedSampleRate * CreditMaxLeadMilliseconds / 1000,
                                             Stopwatch.Frequency * CreditBurstGapMicroseconds / 1_000_000);
                _timeline = new QueueTimelineWatch((long)_bytesPerFrame * RequestedSampleRate);
            }
        }

        private uint BufferBytes => RequestedSampleRate * BufferMilliseconds / 1000 * (uint)_bytesPerFrame;

        // S2Arm (#189): a downmix slice holds at most one device buffer worth of frames
        private int MaxFrames => (int)(RequestedSampleRate * BufferMilliseconds / 1000);

        // S2Arm (#189): frame count of a queued buffer in device bytes. Equivalent to the base helper for native
        //   sessions; for a folded 6-channel session DataSize holds stereo bytes, so RequestedChannelCount must not be used.
        private ulong GetFrameCount(AudioBuffer buffer) => buffer.DataSize / (ulong)_bytesPerFrame;

        // S2Arm: 建 AudioQueue 和它的 32 个缓冲（起会话与媒体服务重置后重建共用）。缓冲放回同一个数组：泵按槽号取，换队列不用换泵
        private int CreateQueue()
        {
            // S2Arm (#189): the queue is in the device format — stereo when the explicit downmix folds the 6-channel
            //   stream first (the 6-channel PCM16 the renderer produces is _deviceChannelCount's business), 6-channel otherwise
            AudioStreamBasicDescription format = AppleHardwareDeviceDriver.GetAudioFormat(RequestedSampleFormat, RequestedSampleRate, (uint)_deviceChannelCount);   // 平台例外：Apple 后端平台实现内部的队列格式（#189）
            nint callbackPtr = Marshal.GetFunctionPointerForDelegate(_callbackDelegate);
            nint userData = GCHandle.ToIntPtr(_gcHandle);
            Interlocked.Exchange(ref _enqueuedFrames, 0);
            int result = AudioQueueNewOutput(ref format, callbackPtr, userData, nint.Zero, nint.Zero, 0, out _audioQueue);

            if (result != 0)
            {
                _audioQueue = nint.Zero;

                return result;
            }

            // S2Arm (11-14 设备缓冲): 设备 IO 缓冲是出声延迟的地板——建队（起会话与自愈重建共用）先请求它，
            //   再挂布局 / 分配缓冲。失败按原样继续（见 S2AApplyDeviceBufferFrames）
            S2AApplyDeviceBufferFrames();
            S2ALogOutputDevice();

            // S2Arm（#189）：6 声道队列声明 5.1 布局（起会话与重建共用这条路径）：不声明时系统隐式 6→2 只复制
            //   FL/FR，中置（对白）与环绕整段丢失；真 5.1 设备声明后按布局直通。设置失败按无布局继续（打出【红】，
            //   行为同旧），真 5.1 设备探建时已验过布局可用。显式下混的队列是 2 声道（折叠已在前），不设布局
            if (_deviceChannelCount == 6)
            {
                AudioChannelLayout layout = new AudioChannelLayout
                {
                    AudioChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A,
                    AudioChannelBitmap = 0,
                    NumberChannelDescriptions = 0
                };

                int layoutResult = AudioQueueSetProperty(
                    _audioQueue,
                    kAudioQueueProperty_ChannelLayout,
                    ref layout,
                    (uint)Marshal.SizeOf<AudioChannelLayout>());

                if (layoutResult != 0)
                {
                    Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：声明 5.1 布局失败 rc={layoutResult}（按无布局继续，中置 / 环绕可能丢失）");
                }
            }

            // S2Arm (11-12 #767): 缓冲→槽号表随缓冲一起整表建好再原子发布：重建期间旧队列迟到的交还
            //   查旧表命中 / 查新表落空，都同旧数组扫描的「查不到就返回」，见不到半张表
            Dictionary<nint, int> bufferSlots = new(NumBuffers);

            for (int i = 0; i < NumBuffers; i++)
            {
                AudioQueueAllocateBuffer(_audioQueue, BufferBytes, out _audioQueueBuffers[i]);
                bufferSlots[_audioQueueBuffers[i]] = i;
            }

            Volatile.Write(ref _bufferSlots, bufferSlots);

            return 0;
        }

        // S2Arm (11-14 设备缓冲)：S2A_AUDIO_BUFFER_FRAMES = 16 的倍数（16–512）时，把输出设备的
        //   kAudioDevicePropertyBufferFrameSize 请求成这个值。上游从不请求，设备回落自己的默认 IO 周期
        //   （Mac 内置扬声器 512 帧 ≈ 10.7 ms @48k），这一个 IO 周期就是出声延迟的地板；本机实测设备收 16 帧
        //   （≈ 0.33 ms，合法范围 15–4096）声音干净，交还节奏变密、audren 链路的延迟随之下降（PlaybackCredit
        //   按实测 burst 自适应，不用改）。建队与自愈重建都走这里（路由切走后旧请求留在旧设备上，下次建队重新请求）。
        //   11-14 实测：这个值按进程算——我们持有 32 帧时别的进程读到的仍是设备默认 512，我们退出也不留改动，不影响其他 App。
        //   代价只在本进程：IO 周期变短、CPU 占用上升，机器忙时可能爆音（启动设置「声音延迟」的说明照此写）。
        //   失败（读设备号 / 设 / 读回）按设备默认继续（【红】记录，行为同旧）。
        //   仅 macOS：iOS 没有 HAL 设备属性（IO 缓冲走 AVAudioSession 的 S2A_AUDIO_IO20 通道）。
        private void S2AApplyDeviceBufferFrames()
        {
            if (!OperatingSystem.IsMacOS())   // 平台例外：设备 IO 缓冲是 macOS HAL 属性，iOS 走 AVAudioSession
            {
                return;
            }

            int frames = AppleDeviceBufferFrames.FromEnvironment();

            if (frames == 0)
            {
                return;
            }

            // 设备号：队列不选设备，AudioQueueNewOutput 缺省绑系统默认输出设备（'dOut'）。本机 macOS 26 实测
            //   队列侧 kAudioQueueProperty_CurrentDevice（'aqcd'）读不出（-66683），从 AudioObject 系统对象取
            AudioObjectPropertyAddress defaultOut = new()
            {
                Selector = kAudioHardwarePropertyDefaultOutputDevice,
                Scope = kAudioObjectPropertyScopeGlobal,
                Element = kAudioObjectPropertyElementMain
            };
            uint device = 0;
            uint deviceSize = sizeof(uint);

            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, ref defaultOut, 0, nint.Zero, ref deviceSize, ref device) != 0 || device == 0)
            {
                Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：读默认输出设备号失败，设备缓冲 {frames} 帧没请求（按设备默认继续）");
                return;
            }

            AudioObjectPropertyAddress address = new()
            {
                Selector = kAudioDevicePropertyBufferFrameSize,
                Scope = kAudioObjectPropertyScopeGlobal,
                Element = kAudioObjectPropertyElementMain
            };
            uint requested = (uint)frames;
            int setRc = AudioObjectSetPropertyData(device, ref address, 0, nint.Zero, sizeof(uint), ref requested);
            uint granted = 0;
            uint readSize = sizeof(uint);
            int getRc = AudioObjectGetPropertyData(device, ref address, 0, nint.Zero, ref readSize, ref granted);

            if (setRc != 0 || getRc != 0 || granted == 0)
            {
                Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：设备缓冲请求 {frames} 帧失败（set rc={setRc}、读回 rc={getRc}、值 {granted}）：按设备默认继续");
                return;
            }

            Console.Error.WriteLine($"[声] 输出 #{_diag.Index}：设备缓冲请求 {frames} 帧，设备实给 {granted} 帧 ≈ {granted * 1000.0 / RequestedSampleRate:F1} ms（只作用于本进程，其他 App 不受影响）");
        }

        // S2Arm (11-16 声音诊断)：建队时记一行系统默认输出设备的传输方式、IO 周期、标称采样率、静音与音量（#1308 爆音 / #1245、诊断#a51362 无声：
        //   交设备的声音非零，判不了是设备静音、走了蓝牙 / 虚拟设备，还是内容本身）。只读，不改设备；仅 macOS（iOS 没有 HAL 设备属性）
        private void S2ALogOutputDevice()
        {
            if (!OperatingSystem.IsMacOS())   // 平台例外：HAL 设备属性只有 macOS
            {
                return;
            }

            AudioObjectPropertyAddress a = new() { Selector = kAudioHardwarePropertyDefaultOutputDevice, Scope = kAudioObjectPropertyScopeGlobal, Element = kAudioObjectPropertyElementMain };
            uint device = 0, size = sizeof(uint);

            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, ref a, 0, nint.Zero, ref size, ref device) != 0 || device == 0)
            {
                Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：读不到系统默认输出设备");
                return;
            }

            string U32(uint selector, uint scope)
            {
                AudioObjectPropertyAddress q = new() { Selector = selector, Scope = scope, Element = kAudioObjectPropertyElementMain };
                uint v = 0, n = sizeof(uint);
                return AudioObjectGetPropertyData(device, ref q, 0, nint.Zero, ref n, ref v) == 0 ? v.ToString() : null;
            }

            AudioObjectPropertyAddress t = new() { Selector = kAudioDevicePropertyTransportType, Scope = kAudioObjectPropertyScopeGlobal, Element = kAudioObjectPropertyElementMain };
            uint tran = 0, ts = sizeof(uint);
            string transport = AudioObjectGetPropertyData(device, ref t, 0, nint.Zero, ref ts, ref tran) == 0
                ? (tran switch
                {
                    0x626C746E => "内置", 0x626C7565 => "蓝牙", 0x626C6561 => "蓝牙低功耗", 0x75736220 => "USB", 0x68646D69 => "HDMI", 0x64707274 => "DisplayPort",
                    0x61697270 => "AirPlay", 0x67727570 => "聚合", 0x76697274 => "虚拟", 0x7468756E => "雷雳", 0x70636920 => "PCI", _ => $"其他 0x{tran:x8}",
                })
                : "读不到";
            AudioObjectPropertyAddress r = new() { Selector = kAudioDevicePropertyNominalSampleRate, Scope = kAudioObjectPropertyScopeGlobal, Element = kAudioObjectPropertyElementMain };
            double rate = 0; uint rs = sizeof(double);
            string rateText = AudioObjectGetPropertyDataDouble(device, ref r, 0, nint.Zero, ref rs, ref rate) == 0 ? $"{rate:F0} Hz" : "读不到";
            AudioObjectPropertyAddress vq = new() { Selector = kAudioDevicePropertyVolumeScalar, Scope = kAudioDevicePropertyScopeOutput, Element = kAudioObjectPropertyElementMain };
            float vol = 0; uint vs = sizeof(float);
            string volText = AudioObjectGetPropertyDataFloat(device, ref vq, 0, nint.Zero, ref vs, ref vol) == 0 ? $"{vol:F2}" : "主元素读不到（多为按声道调音量的设备）";
            string frames = U32(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
            string mute = U32(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput);
            Console.Error.WriteLine($"[声] 输出 #{_diag.Index}：系统默认输出设备 {device}，传输方式 {transport}、IO 周期 {frames ?? "读不到"} 帧、标称采样率 {rateText}（本会话 {RequestedSampleRate} Hz）、静音 {mute ?? "读不到"}、音量 {volText}");
        }

        // S2Arm: the device side of DeviceBufferPump: pool buffer i is _audioQueueBuffers[i]
        private sealed unsafe class Pump : DeviceBufferPump
        {
            private readonly AppleHardwareDeviceSession _s;

            public Pump(AppleHardwareDeviceSession s, int bufferBytes)
                // S2Arm (10-22 杂音): 起播前预充 PrimeMilliseconds 的在途下限（约 3 个游戏缓冲），凑够才让设备开播
                : base(NumBuffers, bufferBytes, s._bytesPerFrame, s._bytesPerFrame * (int)s.RequestedSampleRate,
                       (long)s._bytesPerFrame * s.RequestedSampleRate * PrimeMilliseconds / 1000)
            {
                _s = s;
            }

            // S2Arm (11-03 太鼓 iPhone 断续): 不取设备播放时钟（DeviceClock 留缺省 MinValue：补垫不补）。11-02 太鼓爆音修复在每次交还时调
            //   AudioQueueGetCurrentTime（问音频服务进程）判补垫，iPhone 上这个调用本身打乱输出：调用每 10 秒有 5–18 次超过 10 ms（最长约 50 ms），
            //   每次都碰上设备交还和游戏交声音一起停 40–50 ms、设备吃空断音约 55 ms。测试设备1 太鼓同包 A/B 各 74 秒：每 100 ms 调一次 → 交设备
            //   >30 ms 空档 42 次、回调最长间隔 55–67 ms；完全不调 → 0 次、21.6–21.8 ms（= 一拉，同修复前的别的款）。
            //   补垫原为欠载后重建缓冲深度（10-23 马车8）；放回提前量（PlaybackCredit，11-02）让客人始终比交还多送一拉，欠载后深度随之回来

            protected override Span<byte> SlotData(int slot)
            {
                AudioQueueBuffer* buffer = (AudioQueueBuffer*)_s._audioQueueBuffers[slot];

                return new Span<byte>((void*)buffer->AudioData, (int)buffer->AudioDataBytesCapacity);
            }

            protected override void Submit(int slot, int bytes)
            {
                if (Volatile.Read(ref _s._closedStuck))
                {
                    return;
                }

                nint bufferPtr = _s._audioQueueBuffers[slot];
                AudioQueueBuffer* buffer = (AudioQueueBuffer*)bufferPtr;

                _s.ApplyVolume(buffer->AudioData, bytes);
                buffer->AudioDataByteSize = (uint)bytes;
                _s._diag.OnSubmit(new ReadOnlySpan<byte>((void*)buffer->AudioData, bytes), InFlightBytes - bytes);

                // 无设备播放（见 _virtual）：不进 AudioQueue，排到按采样率播完的时刻由送数线程交还。泵锁内调用（进出无设备都在停泵期间切换）
                if (Volatile.Read(ref _s._virtual))
                {
                    _s.S2AVirtualSubmit(slot, bytes);

                    return;
                }

                long t0 = Stopwatch.GetTimestamp();
                if (_testEnqStall.AtS > 0 && !_s._testEnqStallDone && _s._started && t0 - _s._startTicks >= (long)_testEnqStall.AtS * Stopwatch.Frequency)
                {
                    _s._testEnqStallDone = true;
                    Console.Error.WriteLine($"[声] 测试钩子：输出 #{_s._diag.Index} 开播后第 {_testEnqStall.AtS} 秒一次交设备卡 {_testEnqStall.Ms} ms（模拟输出队列卡死）");
                    Thread.Sleep(_testEnqStall.Ms);
                }
                int enqRc = AudioQueueEnqueueBuffer(_s._audioQueue, bufferPtr, 0, nint.Zero);
                long dt = Stopwatch.GetTimestamp() - t0;

                if (enqRc == 0)
                {
                    Interlocked.Add(ref _s._enqueuedFrames, bytes / _s._bytesPerFrame);
                }

                // 泵锁内调用，同一时刻只有一个写方；看门狗用 Interlocked.Exchange 取走
                _s._diag.OnEnqueue(dt);
                if (dt > Volatile.Read(ref _s._enqStuckTicks))
                {
                    Volatile.Write(ref _s._enqStuckTicks, dt);
                }
            }

            protected override void OnReturned(int bytes, long inFlightAfter)
            {
                _s._diag.OnReturned(bytes, inFlightAfter);
                if (_testSlowAfterHeal > 0 && _s._heals == _testSlowAfterHeal && (++_s._testSlowTick & 3) != 0)
                {
                    _s._diag.TestUnplay(bytes);   // 测试钩子（见 _testSlowAfterHeal）：4 块里 3 块不记成播完
                }

                Interlocked.Add(ref _s._returnedBytes, bytes);

                if (inFlightAfter == 0)
                {
                    Interlocked.Increment(ref _s._zeroReturns);
                }
            }

            // S2Arm (10-23 马车8 杂音): 欠载后补静音垫。这里在游戏的 AppendBuffer 线程上，只记数（零分配，服务层零分配单测守着）；
            //   行由 S2AOutputDiag 的 10 秒汇报线程打
            protected override void OnCushion(int bytes, long zeroTicks, long lateFrames) => _s._diag.OnCushion(bytes / _s._bytesPerFrame, zeroTicks, lateFrames);
        }

        // 无设备播放（见 _virtual）：这块从上一块播完起（或现在，取晚的）按采样率播完
        private void S2AVirtualSubmit(int slot, int bytes)
        {
            lock (_virtualLock)
            {
                long now = Stopwatch.GetTimestamp();
                _virtualEnd = Math.Max(now, _virtualEnd) + (long)(bytes / _bytesPerFrame) * Stopwatch.Frequency / RequestedSampleRate;
                int i = (_virtualHead + _virtualCount) % NumBuffers;
                _virtualSlot[i] = slot;
                _virtualDue[i] = _virtualEnd;
                _virtualCount++;
                _virtualFrames += bytes / _bytesPerFrame;
            }

            _paceEvent.Set();
        }

        // 送数线程：到点的无设备块交还（同设备回调：置位、记回调时刻）；返回下一块还要等的 Stopwatch 刻数，没有在途块 / 游戏停着 long.MaxValue
        private long S2AVirtualReturn(long now)
        {
            lock (_virtualLock)
            {
                if (_virtualPausedAt != 0)
                {
                    return long.MaxValue;
                }

                while (_virtualCount > 0 && _virtualDue[_virtualHead] <= now)
                {
                    Interlocked.Or(ref _returnedMask, 1 << _virtualSlot[_virtualHead]);
                    _virtualHead = (_virtualHead + 1) % NumBuffers;
                    _virtualCount--;
                    Volatile.Write(ref _lastCallbackTicks, now);
                }

                return _virtualCount > 0 ? _virtualDue[_virtualHead] - now : long.MaxValue;
            }
        }

        // 停泵期间调（Restart 的回调里）：清掉无设备在途记录（泵随后把这些槽一并收回）
        private void S2AVirtualClearLocked()
        {
            lock (_virtualLock)
            {
                _virtualHead = _virtualCount = 0;
                _virtualEnd = _virtualPausedAt = 0;
            }
        }

        // 游戏 Stop / Start（_lock 内）：同真队列的暂停——停着不交还，续播时余下的块按停了多久整体顺延
        private void S2AVirtualPause(bool pause)
        {
            lock (_virtualLock)
            {
                long now = Stopwatch.GetTimestamp();

                if (pause)
                {
                    _virtualPausedAt = _virtualPausedAt == 0 ? now : _virtualPausedAt;

                    return;
                }

                if (_virtualPausedAt == 0)
                {
                    return;
                }

                long shift = now - _virtualPausedAt;
                _virtualPausedAt = 0;

                for (int k = 0; k < _virtualCount; k++)
                {
                    _virtualDue[(_virtualHead + k) % NumBuffers] += shift;
                }

                _virtualEnd = _virtualCount > 0 ? _virtualEnd + shift : 0;
            }

            _paceEvent.Set();
        }

        // 进入无设备播放（见 _virtual）：停真队列、收回在途（记作播完），之后泵往无设备交。停不下来（缓冲可能还在队列里）就不进
        private void S2AEnterVirtual(string why)
        {
            lock (_lock)
            {
                if (_audioQueue == nint.Zero || Volatile.Read(ref _virtual) || !_healOn)
                {
                    return;
                }

                long inFlightFrames = _pump.InFlightBytes / _bytesPerFrame;
                int stopRc = 0;
                int reclaimed = _pump.Restart(() =>
                {
                    stopRc = AudioQueueStop(_audioQueue, true);

                    if (stopRc == 0)
                    {
                        Interlocked.Exchange(ref _returnedMask, 0);
                        Interlocked.Exchange(ref _enqueuedFrames, 0);
                        S2AVirtualClearLocked();
                        _virtualFrames = 0;
                        _virtualForInterrupt = S2AAudioNotes.Interrupted;
                        Volatile.Write(ref _virtual, true);
                    }

                    return stopRc;
                });

                if (stopRc != 0)
                {
                    Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：{why}，停输出队列失败 rc={stopRc}，没改走无设备播放");

                    return;
                }

                if (reclaimed > 0)
                {
                    _credit.Reclaimed(reclaimed);
                    S2ACredit();
                }

                _queueRunning = false;
                _lastHealTicks = Stopwatch.GetTimestamp();

                if (_started)
                {
                    _pump.Pump();
                    S2ACredit();
                }

                _paceEvent.Set();
                Console.Error.WriteLine($"[声] 输出 #{_diag.Index}：{why}（在途 {inFlightFrames} 帧）→ 停输出队列、收回 {reclaimed / _bytesPerFrame} 帧，改走无设备播放" +
                    "（游戏的声音按采样率当作播完交还，没声；设备能用了再回到设备）");
            }
        }

        private void OutputCallback(nint userData, nint audioQueue, nint bufferPtr)
        {
            Ryujinx.Common.S2AThreadAlloc.Tick("CoreAudio 回调"); // S2Arm: per-thread allocation accounting
            Volatile.Write(ref _lastCallbackTicks, Stopwatch.GetTimestamp()); // S2Arm: 停播看门狗（S2ACheckStall）

            if (bufferPtr == nint.Zero)
                return;

            // S2Arm (11-12 #767): 槽号改查表（建表见 CreateQueue）：实时回调里不再对 32 槽线性扫；
            //   查不到（重建后旧队列的迟到交还）照旧直接返回——本回调从不解引用未认出的指针，同旧扫描
            if (!Volatile.Read(ref _bufferSlots).TryGetValue(bufferPtr, out int bufferIndex))
                return;

            // S2Arm (11-14): only note the slot and wake the pace thread, which recycles it and refills the device (see _returnedMask)
            Interlocked.Or(ref _returnedMask, 1 << bufferIndex);
            _paceEvent.Set();
        }

        // S2Arm (11-14): on the pace thread. Recycle every returned device buffer, but advance guest progress only for its queued-sample
        //   bytes, one device pull ahead of the hand-backs (PlaybackCredit); then refill the device from what the game queued
        private void S2ADrainReturned()
        {
            int mask = Interlocked.Exchange(ref _returnedMask, 0);

            if (mask == 0)
            {
                return;
            }

            long now = Stopwatch.GetTimestamp();

            while (mask != 0)
            {
                int slot = System.Numerics.BitOperations.TrailingZeroCount(mask);
                mask &= mask - 1;
                _credit.Returned(_pump.Returned(slot), now);
            }

            _diag.SetLead(_credit.LeadBytes / _bytesPerFrame);

            if (_started)
            {
                _pump.Pump();
            }
        }

        // S2Arm (11-02): release what now counts as played (handed back, or submitted and passed by the credit's play head)
        private void S2ACredit()
        {
            int bytes = _credit.Take(_pump.SubmittedSampleBytes, Stopwatch.GetTimestamp());

            if (bytes > 0)
            {
                ProcessPlayedSamples(bytes);
            }
        }

        // S2Arm (11-02 Pokopia iPhone 杂音): sleep until the head buffer is due (its unplayed rest passed by the play head), release it,
        //   repeat; with nothing queued, or the head beyond the cap, wait for a hand-back or a new buffer
        private void PaceLoop()
        {
            while (!Volatile.Read(ref _disposed))
            {
                long virtualWait = S2AVirtualReturn(Stopwatch.GetTimestamp());
                S2ADrainReturned();
                S2ACredit();

                long rest;
                lock (_queueLock)
                {
                    rest = _queuedCount == 0 ? -1 : (long)(_queued[_queuedHead].SampleCount - _headPlayed) * _bytesPerFrame;
                }

                long ticks = Math.Min(rest < 0 ? long.MaxValue : _credit.TicksUntil(rest, _pump.SubmittedSampleBytes, Stopwatch.GetTimestamp()), virtualWait);

                if (ticks == long.MaxValue)
                {
                    _paceEvent.WaitOne();
                }
                else if (ticks > 0)
                {
                    _paceEvent.WaitOne((int)Math.Min(1000, (ticks * 1000 + Stopwatch.Frequency - 1) / Stopwatch.Frequency));
                }
            }
        }

        private void ProcessPlayedSamples(int bytesPlayed)
        {
            // S2Arm (#189): device bytes (stereo after the explicit downmix); the base helper would use the requested
            //   6 channels. Frame counts are preserved by the fold, so guest and device frames agree.
            ulong samplesPlayed = (ulong)bytesPlayed / (ulong)_bytesPerFrame;
            ulong remaining = samplesPlayed;
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

            // S2Arm (11-12 功耗): 播完需要唤醒 AudioManager.Worker 的只有 audout 的客体缓冲——Worker 醒来只做
            //   AudioOutputManager.Update / AudioInputManager.Update（AudioManagers 的接线），即 audout / audin 会话的
            //   缓冲释放；audren 游戏自己的更新事件在 5 ms 渲染线程上直接 Signal（AudioRenderSystem.SendCommands）。
            //   渲染器会话每个 5 ms 块播完也置一次事件：纯 audren 游戏把 Worker 每秒叫醒约 200 次，只为扫两圈空
            //   会话数组。生产方定性来自首个缓冲（见 ConfigurePrimeFloor）；audout / audsmx 客体缓冲照旧唤醒。
            if (needUpdate && !_rendererProducer)
            {
                _updateRequiredEvent.Set();
            }
        }

        private unsafe void ApplyVolume(nint dataPtr, int byteSize)
        {
            // S2A：游戏音量（#1186，runtime/src/game_volume.c；按游戏存、局中齿轮菜单可改）乘进来；100 时同原来整段跳过
            float volume = Math.Clamp(_volume * _driver.Volume * (S2AGameVolume() / 100f), 0f, 1f);
            if (volume >= 0.999f)
                return;

            int sampleCount = byteSize / BackendHelper.GetSampleSize(RequestedSampleFormat);

            switch (RequestedSampleFormat)
            {
                case SampleFormat.PcmInt16:
                    short* s16 = (short*)dataPtr;
                    for (int i = 0; i < sampleCount; i++)
                        s16[i] = (short)(s16[i] * volume);
                    break;

                case SampleFormat.PcmFloat:
                    float* f32 = (float*)dataPtr;
                    for (int i = 0; i < sampleCount; i++)
                        f32[i] *= volume;
                    break;

                case SampleFormat.PcmInt32:
                    int* s32 = (int*)dataPtr;
                    for (int i = 0; i < sampleCount; i++)
                        s32[i] = (int)(s32[i] * volume);
                    break;

                case SampleFormat.PcmInt8:
                    sbyte* s8 = (sbyte*)dataPtr;
                    for (int i = 0; i < sampleCount; i++)
                        s8[i] = (sbyte)(s8[i] * volume);
                    break;
            }
        }

        // S2Arm (11-09): the first buffer tells the producer apart. Renderer output (audren) carries its samples in Data;
        // an AudioOut guest buffer keeps them in guest memory (Data == null, SampleReader set by RegisterBuffer). The latter
        // keeps the 64 ms pre-roll that fixed audout's start underruns; the former starts on its first 5 ms block.
        // S2Arm (11-12 功耗): 定性顺带记给播完唤醒的门用（见 ProcessPlayedSamples）。
        private void ConfigurePrimeFloor(AudioBuffer buffer)
        {
            if (_primeConfigured)
            {
                return;
            }

            _primeConfigured = true;
            _rendererProducer = buffer.Data != null;

            if (buffer.Data == null || _rendererPrimeMilliseconds == PrimeMilliseconds)
            {
                return;
            }

            _pump.SetPrimeFloor((long)_bytesPerFrame * RequestedSampleRate * _rendererPrimeMilliseconds / 1000);
        }

        // S2Arm (#189): guest buffers (no Data array) are folded while the device reads them (AudioSampleQueue ->
        //   ReadSamples through SampleReader), so the samples stay in guest memory and the session's frame accounting,
        //   credit and release cadence are unchanged. The buffer's size is told in device bytes from here on.
        public override bool RegisterBuffer(AudioBuffer buffer)
        {
            if (buffer.DataPointer == 0)
            {
                return false;
            }

            if (_processor == null)
            {
                return base.RegisterBuffer(buffer);
            }

            ulong frames = buffer.DataSize / (ulong)_guestBytesPerFrame;

            buffer.DataSize = frames * (ulong)_bytesPerFrame;
            _convertingReader.Inner = buffer.SampleReader ?? this;
            buffer.SampleReader = _convertingReader;

            return true;
        }

        public override void QueueBuffer(AudioBuffer buffer)
        {
            ConfigurePrimeFloor(buffer);

            if (_testProdStall.AtS > 0 && !_testProdStallDone && _started && Stopwatch.GetTimestamp() - _startTicks >= (long)_testProdStall.AtS * Stopwatch.Frequency)
            {
                _testProdStallDone = true;
                Console.Error.WriteLine($"[声] 测试钩子：输出 #{_diag.Index} 开播后第 {_testProdStall.AtS} 秒生产方停 {_testProdStall.Ms} ms（模拟 GC 停顿）");
                Thread.Sleep(_testProdStall.Ms);
            }

            // S2Arm (#189): renderer buffers (Data) are copied in by AudioSampleQueue right below, so one staging
            //   array can be reused; the queue never reads it after Enqueue returns. Guest bytes (6-channel) are
            //   folded to device bytes (stereo) first; the frame count is unchanged.
            if (_processor != null && buffer.Data != null)
            {
                int frames = buffer.Data.Length / _guestBytesPerFrame;
                int outputBytes = frames * _bytesPerFrame;

                if (_convertedData == null || _convertedData.Length != outputBytes)
                {
                    _convertedData = new byte[outputBytes];
                }

                _processor.Process(buffer.Data.AsSpan(0, frames * _guestBytesPerFrame), _convertedData);
                buffer.Data = _convertedData;
                buffer.DataSize = (ulong)outputBytes;
            }

            ulong sampleCount = GetFrameCount(buffer);

            // S2Arm (11-02): register before its samples can be submitted: submitted samples may count as played at once (PlaybackCredit),
            //   and played samples must find their buffer in _queued
            lock (_queueLock)
            {
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
                _pump.Pump();
                S2ACredit();
                _paceEvent.Set();
                // S2Arm (10-22 杂音): 预充期排进设备但未启播——凑够下限的这一笔到了就开播
                if (!_queueRunning)
                {
                    lock (_lock)
                    {
                        MaybeStartQueue();
                    }
                }
            }
        }

        public override void Start()
        {
            lock (_lock)
            {
                if (_started)
                    return;

                _started = true;
                _diag.SetStarted(true);
                _startTicks = Stopwatch.GetTimestamp();
                _pump.Pump();
                S2ACredit();

                Volatile.Write(ref _lastCallbackTicks, _startTicks); // S2Arm: 停播看门狗从开播时刻起算

                if (Volatile.Read(ref _virtual))
                {
                    // 无设备播放中（见 _virtual）：泵已往无设备交，不碰真队列；Stop 时停住的块续上
                    S2AVirtualPause(false);
                }
                else if (_queueRunning)
                {
                    // 从 Stop 的暂停恢复：队列已预充过，直接续播
                    AudioQueueStart(_audioQueue, nint.Zero);
                }
                else
                {
                    // 首次启播：先 pre-roll，在途凑够下限（或兜底超时）再 AudioQueueStart
                    MaybeStartQueue();
                }
            }
        }

        // S2Arm (10-22 杂音): 在途达预充下限（或 Start 后超过兜底时限）才让设备开播，起播有深度不掐边欠载。_lock 内调。
        private void MaybeStartQueue()
        {
            if (_queueRunning || !_started || Volatile.Read(ref _virtual))
            {
                return;
            }

            if (!_pump.PrimedForStart &&
                (Stopwatch.GetTimestamp() - _startTicks) * 1000 / Stopwatch.Frequency >= StartFallbackMilliseconds)
            {
                _pump.ForcePrimed();
            }

            if (_pump.PrimedForStart)
            {
                _queueRunning = true;
                Volatile.Write(ref _lastCallbackTicks, Stopwatch.GetTimestamp()); // S2Arm: 停播看门狗从真开播起算
                AudioQueueStart(_audioQueue, nint.Zero);
            }
        }

        public override void Stop()
        {
            lock (_lock)
            {
                if (!_started)
                    return;

                _started = false;
                _diag.SetStarted(false);
                AudioQueuePause(_audioQueue);
                if (Volatile.Read(ref _virtual))
                {
                    S2AVirtualPause(true);
                }
                _pump.ResetClock();   // 暂停期间播放时钟怎么走不确定：续播后重新学交还相位
            }
        }

        public override ulong GetPlayedSampleCount()
            => Interlocked.Read(ref _playedSampleCount);

        public override float GetVolume() => _volume;
        public override void SetVolume(float volume) => _volume = volume;

        public override bool WasBufferFullyConsumed(AudioBuffer buffer)
        {
            lock (_queueLock)
            {
                return _queuedCount == 0 || _queued[_queuedHead].DriverIdentifier != buffer.DataPointer;
            }
        }

        // S2Arm (11-13 断音): what the renderer paces by (AudioProcessor.S2ADepthWait): queued + submitted, not handed back yet.
        //   Only while the device plays: in pre-roll the queue must fill to the prime floor on the wall clock
        public long S2ABufferedFrames => Volatile.Read(ref _queueRunning) && !Volatile.Read(ref _disposed)
            ? (_pump.InFlightBytes + _samples.Length) / _bytesPerFrame
            : -1;

        public override void PrepareToClose() { }
        // S2Arm: the buffer goes back to the game (Stop / FlushBuffers / Dispose) possibly before it was played: keep what it held
        public override void UnregisterBuffer(AudioBuffer buffer) => _samples.Release(buffer);

        protected virtual void Dispose(bool disposing)
        {
            if (disposing)
            {
                // Match the watchdog's lock order (_watched, then _lock), and wait
                // for a pass that already selected this session before closing it.
                S2AWatch(this, false);
                lock (_lock)
                {
                    if (_disposed) return;
                    Stop();
                    _queueRunning = false;

                    bool leaked = false;

                    if (_audioQueue != nint.Zero)
                    {
                        // S2Arm (11-14 iPhone 实机 声音服务卡死): the pace thread may be stuck inside a Pump reading a stalled guest buffer (the game
                        //   hung; S2ADrainReturned). Disposing would free the buffers (and the downmix stage) it is writing through: stop the
                        //   queue (its callbacks only set a bit) and leave both allocated instead. Bounded: closing never waits on the game
                        if (_pump.TrySuspend(TimeSpan.FromMilliseconds(200)))
                        {
                            AudioQueueStop(_audioQueue, true);
                            int result = AudioQueueDispose(_audioQueue, true);
                            if (result != 0)
                                throw new InvalidOperationException($"AudioQueueDispose failed: {result}; callback handle retained");
                            _audioQueue = nint.Zero;
                        }
                        else
                        {
                            Volatile.Write(ref _closedStuck, true);
                            AudioQueueStop(_audioQueue, true);
                            leaked = true;
                            Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：释放时送数线程卡在读游戏内存（等了 200 ms），队列已停、不释放（防止释放后它再往缓冲里写）");
                        }
                    }

                    if (!leaked)
                    {
                        // S2Arm（#189 后续，评审 11-16）：VS / 下混档收局统计——渲染失败片数与截幅采样累计，
                        //   判断「VS 开了声音发破 / 偶尔没声」是不是 AUSM 档的原因（只在有记录时打一行）
                        if (_processor != null && (_processor.FailureCount > 0 || _processor.ClippedSampleCount > 0))
                        {
                            Console.Error.WriteLine($"[声] 输出 #{_diag.Index} 收局：5.1→2.0 档「{AppleOutputModeSelector.ModeName(_processor.EffectiveMode)}」渲染失败 {_processor.FailureCount} 片、截幅 {_processor.ClippedSampleCount} 采样（int16 满幅计）");
                        }

                        _processor?.Dispose();   // S2Arm (#189): the explicit downmix stage (AudioConverter) goes with the queue
                    }

                    // Synchronous queue disposal completes callbacks before their
                    // handle is released. A repeated Dispose cannot free a reused slot. A queue left allocated keeps its handle.
                    if (_gcHandle.IsAllocated && !leaked) _gcHandle.Free();
                    _diag.Dispose();
                    Volatile.Write(ref _disposed, true);
                    _paceEvent.Set();
                }

                // S2Arm (11-19): wait for the pace thread to leave before returning. The driver disposes its shared update event right after
                //   disposing the sessions; a pace pass already inside ProcessPlayedSamples would Set the disposed event and the unhandled
                //   ObjectDisposedException took the process down (services suite under load, exit 134; the same teardown runs on an
                //   in-process restart). Bounded: a pace thread stuck reading a hung game's buffer is left behind with a red line, and the
                //   driver sees it through S2APaceThreadAlive and keeps the shared events alive.
                //   Unregister only once it left: a session still in the driver's table with its pace thread alive keeps the shared events.
                Thread pace = _paceThread;
                if (pace != null && pace != Thread.CurrentThread && !pace.Join(1000))
                {
                    Console.Error.WriteLine($"[声] 【红】输出 #{_diag.Index}：释放时送数线程 1 秒内没退出（卡在读游戏内存？），先放开");
                }
                else
                {
                    _driver.Unregister(this);
                }
            }
        }

        public override void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        // S2Arm (#189): the SampleReader the audio queue uses for guest buffers. It maps the device byte offset
        //   (stereo after folding) back to the guest stream (6-channel) and folds only the frames the queue asks for.
        private sealed class ConvertingReader : HardwareDeviceSessionOutputBase
        {
            private readonly AppleOutputProcessor _processor;
            private readonly int _guestBytesPerFrame;
            private readonly int _deviceBytesPerFrame;
            private HardwareDeviceSessionOutputBase _inner;
            private byte[] _scratch = Array.Empty<byte>();

            public ConvertingReader(AppleHardwareDeviceSession owner, AppleOutputProcessor processor)   // 平台例外：Apple 后端平台实现内部的下混读代理（#189）
                : base(owner.MemoryManager, owner.RequestedSampleFormat, owner.RequestedSampleRate, owner.RequestedChannelCount)
            {
                _processor = processor;
                _guestBytesPerFrame = owner._guestBytesPerFrame;
                _deviceBytesPerFrame = owner._bytesPerFrame;
            }

            public HardwareDeviceSessionOutputBase Inner
            {
                set => _inner = value;
            }

            public override void ReadSamples(AudioBuffer buffer, ulong offset, Span<byte> destination)
            {
                int frames = destination.Length / _deviceBytesPerFrame;
                int outputBytes = frames * _deviceBytesPerFrame;

                if (frames == 0)
                {
                    destination.Clear();

                    return;
                }

                int inputBytes = frames * _guestBytesPerFrame;

                if (_scratch.Length < inputBytes)
                {
                    _scratch = new byte[inputBytes];
                }

                ReadOnlySpan<byte> input = _scratch.AsSpan(0, inputBytes);
                ulong guestOffset = offset / (ulong)_deviceBytesPerFrame * (ulong)_guestBytesPerFrame;

                _inner.ReadSamples(buffer, guestOffset, _scratch.AsSpan(0, inputBytes));
                _processor.Process(input, destination[..outputBytes]);

                if (destination.Length > outputBytes)
                {
                    destination[outputBytes..].Clear();
                }
            }

            public override void Dispose() => throw new NotSupportedException();
            public override void PrepareToClose() => throw new NotSupportedException();
            public override void QueueBuffer(AudioBuffer buffer) => throw new NotSupportedException();
            public override void SetVolume(float volume) => throw new NotSupportedException();
            public override float GetVolume() => throw new NotSupportedException();
            public override void Start() => throw new NotSupportedException();
            public override void Stop() => throw new NotSupportedException();
            public override ulong GetPlayedSampleCount() => throw new NotSupportedException();
            public override bool WasBufferFullyConsumed(AudioBuffer buffer) => throw new NotSupportedException();
        }
    }
}
