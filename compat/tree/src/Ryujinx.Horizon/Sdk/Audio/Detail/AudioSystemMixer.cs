using Ryujinx.Audio;
using Ryujinx.Audio.Common;
using Ryujinx.Audio.Integration;
using Ryujinx.Audio.Output;
using Ryujinx.Common;
using Ryujinx.Horizon.Common;
using Ryujinx.Horizon.Sdk.Applet;
using Ryujinx.Horizon.Sdk.Sf;
using Ryujinx.Horizon.Sdk.Sf.Hipc;
using Ryujinx.Memory;
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Ryujinx.Horizon.Sdk.Audio.Detail
{
    // S2Arm：Switch 2 的 audsmx:u（nn::audio::sysmix，系统混音器的外部流）。上游 Ryujinx 没有它，形状与语义从游戏自带 sdk 的客户端反推
    //   （时之笛 sdk：sysmix::detail::*Impl + CmifProxy 虚表 0xe63098 / 0xe631a0 / 0xe632b8 + 组包函数，见 compat/改动清单.md 本条）。
    //   nn::audio::OutputStreamImpl::Open = devmgr 300 / 401 → sysmix CreateStream → OpenStream →（有事件时）AcquireStreamDataEvent；
    //   Start / Stop = devmgr 700 AttachStream / 800 DetachStream（AudioDeviceManager.cs）；Close = ReleaseStreamDataEvent → CloseStream → 销毁对象。
    //   客户端连上就转域（CmifDomainProxyKind），所以本服务所在的服务器要开域（AudioUserIpcServer.cs）。
    //   声音数据不走 IPC：客户端把游戏给的工作缓冲排成「控制块 + 环形缓冲」，WriteOutputStream 只往环里抄样本、推写指针；
    //   服务端从同一块游戏内存读、推读指针（ExternalOutputStream）。
    //   只实现输出流开局路径要的命令；其余命令不进命令表（按未知命令报红），已实现的命令遇到 SDK 输出路径从不发的取值也报红，不回成功兜底。
    //   输入流（麦克风，#2151）同一套命令、方向相反：nn::audio::InputStreamImpl::Open = devmgr 300 / 401（功能 2）→ CreateStream（标志 2）→ OpenStream
    //   →（有事件时）AcquireStreamDataEvent；Start / Stop = devmgr 700 / 800；服务端往环里写、推写指针（ExternalInputStream）。

    // nn::audio::StreamFormat（12 字节）：+0 采样格式（1..5，与 Ryujinx SampleFormat 同号）、+4 声道数、+8 采样率。
    //   依据：sysmix::detail::OpenStreamImpl 拿 +0 减 1 查每样本字节表 {1,2,4,4,4}、+4 乘进帧长；OutputStreamImpl::Open 拿 +8 × 设备延迟 ÷ 1e9 算最少帧数
    [StructLayout(LayoutKind.Sequential, Size = 0xC, Pack = 4)]
    struct StreamFormat
    {
        public uint SampleFormat;
        public int ChannelCount;
        public int SampleRate;

        public override readonly string ToString() => $"格式 {SampleFormat}、{ChannelCount} 声道、{SampleRate} Hz";
    }

    // 一条外部流（输出 / 输入）对 devmgr 与 sysmix 命令的公共面
    interface IExternalStream : IDisposable
    {
        bool IsInput { get; }
        bool IsOpen { get; }
        bool IsAttached { get; }
        AudioEvent DataEvent { get; set; }
        void Open(StreamFormat format, int frameCount);
        void SetGain(float gain, long fadeNs);
        void Attach();
        void Detach();
        void Close();
    }

    // devmgr 与 sysmix 共用的流登记：sysmix CreateStream 发流号，devmgr 700 / 800 按流号挂上 / 摘下虚拟设备
    sealed class ExternalStreamRegistry
    {
        private readonly Lock _lock = new();
        private readonly Dictionary<uint, IExternalStream> _streams = new();
        private uint _nextId;

        public AudioOutputManager OutputManager { get; }

        // 输入流直接向宿主声音驱动要收音会话（AudioInputManager / AudioInputSystem 把单声道改成立体声，外部输入流要游戏给的声道数原样）
        public IHardwareDeviceDriver InputDriver { get; }

        public ExternalStreamRegistry(AudioOutputManager outputManager, IHardwareDeviceDriver inputDriver)
        {
            OutputManager = outputManager;
            InputDriver = inputDriver;
        }

        public uint Add(IExternalStream stream)
        {
            lock (_lock)
            {
                uint id = ++_nextId;
                _streams.Add(id, stream);
                return id;
            }
        }

        public void Remove(uint id)
        {
            lock (_lock)
            {
                _streams.Remove(id);
            }
        }

        public IExternalStream Get(uint id)
        {
            lock (_lock)
            {
                return _streams.GetValueOrDefault(id);
            }
        }
    }

    // 一条外部输出流的服务端：读游戏工作缓冲里的环，按块交给宿主声音输出（AudioOutputSystem，同 audout:u 用的会话），宿主播完交还才推读指针。
    //   工作缓冲布局（sysmix::detail 0x2b38d0 / 0x2b3c00 / GetStreamFreeFrameCountImpl 反推；B = 缓冲起点，页对齐）：
    //     B+0x0000 控制块 1（0x40 字节；客户端初始化时写 +0 字节 = 1，语义不知道，服务端不碰）
    //     B+0x0040 控制块 2（服务端的）：+4 = 读指针（帧号，0 ≤ 读 < 容量）；+0 客户端统计时取差值累加（语义不知道，服务端不写）
    //     B+0x1000 控制块 3（客户端的）：+4 = 写指针；+0 = 写不下丢掉的帧数（客户端自己累加）
    //     B+0x1040 环：容量 = OpenStream 的帧数 + 1 帧，帧长 = 每样本字节 × 声道数，交错存放
    //   （OpenStream 标志位 0 置位时环前还有 0x80 字节的时间戳表、写入走另一套带时间戳的协议，本实现不支持、报红）
    //   可写帧数 = 容量 − 1 − ((写 − 读) mod 容量)：读指针只在宿主交还（播完）后推进，游戏不会改写还在宿主手里的样本。
    sealed class ExternalOutputStream : IExternalStream
    {
        public const ulong ServerBlockOffset = 0x40;
        public const ulong ClientBlockOffset = 0x1000;
        public const ulong RingOffset = 0x1040;
        public const ulong IndexOffset = 4;

        // 服务端搬运周期：每 5 ms 收一次宿主交还、交一次新样本（Switch 硬件一次取 240 帧 = 5 ms）。
        //   devmgr 401 回的设备延迟 = 2 个周期：交样本与收交还各最多晚一个周期，游戏缓冲短于它必断（OutputStreamImpl::Open 按它拒绝过短的缓冲）
        public const int PumpPeriodMs = 5;
        public const ulong MinimumLatencyNs = 2 * PumpPeriodMs * 1_000_000UL;

        private readonly Lock _lock = new();
        private readonly AudioOutputManager _outputManager;
        private readonly IVirtualMemoryManager _memory;
        private readonly Queue<(ulong Tag, int Frames)> _inFlight = new();
        private readonly ulong[] _released = new ulong[Constants.AudioDeviceBufferCountMax];

        private AudioOutputSystem _output;
        private int _capacity;
        private int _frameBytes;
        private int _chunkFrames;
        private int _read;
        private int _queued;
        private ulong _tag;
        private bool _attached;
        private bool _broken;
        private Thread _pump;
        private volatile bool _stopPump;
        // 增益（IExternalStreamSession 4 SetGain）：线性倍数，缺省 1；渐变按墙钟线性走，搬运线程每拍推进（见 StepGainLocked）
        private float _gainFrom = 1f, _gainTo = 1f, _gainApplied = 1f;
        private long _gainStartNs, _gainFadeNs;

        public ulong Buffer { get; }
        public ulong Size { get; }
        public string Name { get; }
        public bool IsInput => false;
        public bool IsOpen => _output != null;
        public bool IsAttached => _attached;

        // 数据事件（AcquireStreamDataEvent 领走）：服务端每次推进读指针就触发
        public AudioEvent DataEvent { get; set; }

        public ExternalOutputStream(AudioOutputManager outputManager, IVirtualMemoryManager memory, ulong buffer, ulong size, string name)
        {
            _outputManager = outputManager;
            _memory = memory;
            Buffer = buffer;
            Size = size;
            Name = name;
        }

        // 宿主声音输出能播的格式：48000 Hz、立体声或 5.1（AudioOutputSystem 只收这些；单声道会被它当立体声，不收），
        //   采样格式 PcmInt8 / PcmInt16 / PcmInt32 / PcmFloat：设备不收的格式由兼容层先解成 PcmInt16 再转（CompatLayerHardwareDeviceSession、
        //   PcmHelper.DecodeToPcm16）。PcmInt24 不收：sysmix 每样本按 4 字节存它（OpenStreamImpl 的字节表 {1,2,4,4,4}），24 位放在哪 3 字节不知道
        public static bool IsHostSupported(StreamFormat format)
        {
            return GuestSampleBytes(format.SampleFormat) > 0 && format.SampleRate == Constants.TargetSampleRate && format.ChannelCount is 2 or 6;
        }

        // 环里每样本字节数（与宿主 BackendHelper.GetSampleSize 一致的那几种）；不支持的格式回 0
        private static int GuestSampleBytes(uint sampleFormat)
        {
            return (SampleFormat)sampleFormat switch
            {
                SampleFormat.PcmInt8 => 1,
                SampleFormat.PcmInt16 => 2,
                SampleFormat.PcmInt32 or SampleFormat.PcmFloat => 4,
                _ => 0,
            };
        }

        public void Open(StreamFormat format, int frameCount)
        {
            if (!IsHostSupported(format))
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 0：{format}（宿主声音输出只播 PcmInt8 / 16 / 32 / Float、48000 Hz、2 或 6 声道）");
            }

            int frameBytes = GuestSampleBytes(format.SampleFormat) * format.ChannelCount;
            ulong ringEnd = RingOffset + (ulong)(frameCount + 1) * (ulong)frameBytes;

            if (frameCount < 1 || (Buffer & 0xFFF) != 0 || ringEnd > Size)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 0：{frameCount} 帧的环放不进工作缓冲 0x{Buffer:X} + 0x{Size:X}（客户端自己会先拒绝，到这里说明布局没对上）");
            }

            AudioInputConfiguration configuration = new() { SampleRate = (uint)format.SampleRate, ChannelCount = (ushort)format.ChannelCount };

            ResultCode rc = _outputManager.OpenAudioOut(out _, out _, out AudioOutputSystem output, _memory, string.Empty, (SampleFormat)format.SampleFormat, ref configuration);

            if (rc != ResultCode.Success)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 0：宿主声音输出打不开（{rc}），不知道真机此时回哪个结果码");
            }

            lock (_lock)
            {
                _output = output;
                _capacity = frameCount + 1;
                _frameBytes = frameBytes;
                _chunkFrames = format.SampleRate * PumpPeriodMs / 1000;
                _read = 0;
                _queued = 0;
                _gainApplied = GainAtLocked(PerformanceCounter.ElapsedNanoseconds);
                _output.SetVolume(_gainApplied);
            }
        }

        // SetGain：从此刻的增益起，fadeNs 内线性走到 gain（0 = 立即）。客户端（sysmix::SetStreamGain）已限定 0 ≤ gain ≤ 128、fade ≤ 5 秒
        public void SetGain(float gain, long fadeNs)
        {
            lock (_lock)
            {
                long now = PerformanceCounter.ElapsedNanoseconds;
                _gainFrom = GainAtLocked(now);
                _gainTo = gain;
                _gainStartNs = now;
                _gainFadeNs = Math.Max(0, fadeNs);
                StepGainLocked(now);
            }
        }

        private float GainAtLocked(long now)
        {
            if (_gainFadeNs <= 0 || now - _gainStartNs >= _gainFadeNs)
            {
                return _gainTo;
            }

            return _gainFrom + (_gainTo - _gainFrom) * (float)((double)(now - _gainStartNs) / _gainFadeNs);
        }

        private void StepGainLocked(long now)
        {
            float gain = GainAtLocked(now);

            if (_output != null && gain != _gainApplied)
            {
                _gainApplied = gain;
                _output.SetVolume(gain);
            }
        }

        public void Attach()
        {
            lock (_lock)
            {
                _queued = _read;
                _broken = false;
                _output.Start();
                _attached = true;
            }

            _stopPump = false;
            _pump = new Thread(PumpLoop) { Name = "S2A.audsmx.Pump", IsBackground = true };
            _pump.Start();
        }

        public void Detach()
        {
            _stopPump = true;
            _pump?.Join();
            _pump = null;

            lock (_lock)
            {
                if (!_attached)
                {
                    return;
                }

                CollectReleased();
                // 还在宿主手里没播完的块作废：读指针停在已播完处，下次挂上从那里重交
                _output.FlushBuffers();
                _output.Stop();
                _output.GetReleasedBuffer(_released, out _);
                _inFlight.Clear();
                _queued = _read;
                _attached = false;
            }
        }

        private void PumpLoop()
        {
            // S2Arm：与 audren 驱动线程同一套（AudioRendererManager.SendCommands、AudioProcessor.Work）：本线程每 5 ms 交一次样本、
            //   收一次交还，实时调度（超计算预算被内核降级；S2A_AUDIO_SCHED=utility / interactive 只设 QoS）；
            //   周期尾睡到绝对时限（Apple 上一次内核唤醒，mach_wait_until）替代 Thread.Sleep。原先普通线程（QoS 缺省）
            //   负载下几十 ms 才被调度一次（10-22 audren 实测：交接等调度每周期超期 27–34 ms、整批丢周期出杂音，见 S2ARealtime）；
            //   5 ms 网格只需平均守住：个别超期由游戏环（打开时须长于设备延迟 = 2 周期）+ 会话在途深度吸收
            long periodNs = PumpPeriodMs * 1_000_000L;
            Ryujinx.Common.S2ARealtime.Promote("audsmx 搬运线程", periodNs, periodNs / 2, periodNs);

            long next = PerformanceCounter.ElapsedNanoseconds;

            while (!_stopPump)
            {
                Ryujinx.Common.S2AThreadAlloc.Tick("audsmx 搬运"); // S2Arm: per-thread allocation accounting

                Pump();

                // 睡到本周期结束（绝对时限：网格不随每圈搬运工作量与调度延迟漂移）；错过周期就重锚网格——
                //   下一圈 Pump 本就把环里积攒的样本收拢成块一次交出，不连发补泵
                next += periodNs;
                long now = PerformanceCounter.ElapsedNanoseconds;

                if (now < next)
                {
                    Ryujinx.Common.PreciseSleep.PreciseSleepHelper.SleepUntilTimePointSingleWake(
                        PerformanceCounter.ElapsedTicks + (next - now) * PerformanceCounter.TicksPerMillisecond / 1_000_000);
                }
                else
                {
                    next = now;
                }
            }
        }

        // 一轮搬运：收宿主交还的块 → 推读指针、触发数据事件；读写指针 → 新写的样本按块交宿主
        public void Pump()
        {
            lock (_lock)
            {
                if (!_attached || _broken)
                {
                    return;
                }

                StepGainLocked(PerformanceCounter.ElapsedNanoseconds);
                CollectReleased();

                uint write = _memory.Read<uint>(Buffer + ClientBlockOffset + IndexOffset);

                if (write >= (uint)_capacity)
                {
                    _broken = true;
                    Console.Error.WriteLine($"[声] 【红】audsmx:u 外部流「{Name}」：写指针 {write} 不在环里（容量 {_capacity} 帧），停止搬运");
                    return;
                }

                int available = ((int)write - _queued + _capacity) % _capacity;

                while (available > 0 && _inFlight.Count < _released.Length)
                {
                    int frames = Math.Min(Math.Min(available, _capacity - _queued), _chunkFrames);
                    AudioUserBuffer buffer = new()
                    {
                        Data = Buffer + RingOffset + (ulong)_queued * (ulong)_frameBytes,
                        DataSize = (ulong)frames * (ulong)_frameBytes,
                    };
                    buffer.Capacity = buffer.DataSize;

                    if (_output.AppendBuffer(++_tag, ref buffer) != ResultCode.Success)
                    {
                        break;
                    }

                    _inFlight.Enqueue((_tag, frames));
                    _queued = (_queued + frames) % _capacity;
                    available -= frames;
                }
            }
        }

        private void CollectReleased()
        {
            _output.GetReleasedBuffer(_released, out uint count);

            int frames = 0;

            for (int i = 0; i < count; i++)
            {
                if (!_inFlight.TryDequeue(out (ulong Tag, int Frames) block) || block.Tag != _released[i])
                {
                    _broken = true;
                    Console.Error.WriteLine($"[声] 【红】audsmx:u 外部流「{Name}」：宿主交还的块 {_released[i]} 不是最早交出的那块，停止搬运");
                    return;
                }

                frames += block.Frames;
            }

            if (frames > 0)
            {
                _read = (_read + frames) % _capacity;
                _memory.Write(Buffer + ServerBlockOffset + IndexOffset, (uint)_read);
                DataEvent?.Signal();
            }
        }

        public void Close()
        {
            Detach();

            lock (_lock)
            {
                _output?.Dispose();
                _output = null;
            }
        }

        public void Dispose()
        {
            Close();
        }
    }

    // 一条外部输入流（麦克风，#2151）的服务端：宿主收音会话把样本直接写进游戏工作缓冲的环，一块写满就推写指针、触发数据事件。
    //   输入方向两块控制块的角色与输出相反（时之笛 sdk 反推：GetStreamFilledFrameCountImpl 0x2b21d0 输入方向作废 B+0x1000 的缓存、
    //   读环公共段 0x2b4760 读 B+0x1000+4 为写、B+0x40+4 为读，读完回写 B+0x40）：
    //     B+0x1000 归服务端：+4 = 写指针（本类写）；+8 位 0 置位时客户端把环当空（本类不置）
    //     B+0x0040 归客户端：+4 = 读指针（ReadStream 读完推）；+0 客户端累加「要读没读到」的帧数
    //     环同输出流：B+0x1040 起、容量 = OpenStream 的帧数 + 1、帧长 = 每样本字节 × 声道数；已填 = (写 − 读) mod 容量
    //   清空（PurgeInputStream）在客户端本地推读指针，不走 IPC。
    //   节奏按墙钟：每 5 ms 至多交出墙钟应得的帧数。真麦克风按实时写满交还；没麦克风 / 没授权时宿主给的是立即交还的会话（兼容层的 Dummy、
    //   Apple 会话的静音模式），按墙钟限速后游戏照样按实时读到帧，不会被一口气灌满。每块交出前先清零：会话没写到的是静音，不是上一轮的旧声音
    sealed class ExternalInputStream : IExternalStream
    {
        // 同时交给宿主会话的块数上限：8 × 5 ms = 40 ms，够吸收搬运线程个别周期的调度延迟
        private const int MaxInFlight = 8;

        private readonly Lock _lock = new();
        private readonly IHardwareDeviceDriver _driver;
        private readonly IVirtualMemoryManager _memory;
        private readonly Queue<(AudioBuffer Buffer, int Frames)> _inFlight = new();

        private IHardwareDeviceSession _session;
        private int _capacity;
        private int _frameBytes;
        private int _chunkFrames;
        private int _sampleRate;
        private int _write;
        private int _queued;
        private long _startNs;
        private long _grantedFrames;
        private ulong _tag;
        private bool _attached;
        private bool _broken;
        private Thread _pump;
        private volatile bool _stopPump;

        public ulong Buffer { get; }
        public ulong Size { get; }
        public string Name { get; }
        public bool IsInput => true;
        public bool IsOpen => _session != null;
        public bool IsAttached => _attached;
        public AudioEvent DataEvent { get; set; }

        public ExternalInputStream(IHardwareDeviceDriver driver, IVirtualMemoryManager memory, ulong buffer, ulong size, string name)
        {
            _driver = driver;
            _memory = memory;
            Buffer = buffer;
            Size = size;
            Name = name;
        }

        // 宿主收音能给的格式（Apple 收音会话：AudioQueue 输入按游戏格式出数，系统自己重采样）：48000 Hz、1 或 2 声道、PcmInt16 / PcmInt32 / PcmFloat
        public static bool IsHostSupported(StreamFormat format)
        {
            return format.SampleRate == Constants.TargetSampleRate && format.ChannelCount is 1 or 2 &&
                (SampleFormat)format.SampleFormat is SampleFormat.PcmInt16 or SampleFormat.PcmInt32 or SampleFormat.PcmFloat;
        }

        public void Open(StreamFormat format, int frameCount)
        {
            if (!IsHostSupported(format))
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 0（输入流）：{format}（宿主收音只给 PcmInt16 / 32 / Float、48000 Hz、1 或 2 声道）");
            }

            int frameBytes = ((SampleFormat)format.SampleFormat == SampleFormat.PcmInt16 ? 2 : 4) * format.ChannelCount;
            ulong ringEnd = ExternalOutputStream.RingOffset + (ulong)(frameCount + 1) * (ulong)frameBytes;

            if (frameCount < 1 || (Buffer & 0xFFF) != 0 || ringEnd > Size)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 0（输入流）：{frameCount} 帧的环放不进工作缓冲 0x{Buffer:X} + 0x{Size:X}（客户端自己会先拒绝，到这里说明布局没对上）");
            }

            IHardwareDeviceSession session = _driver.OpenDeviceSession(IHardwareDeviceDriver.Direction.Input, _memory, (SampleFormat)format.SampleFormat, (uint)format.SampleRate, (uint)format.ChannelCount);

            lock (_lock)
            {
                _session = session;
                _capacity = frameCount + 1;
                _frameBytes = frameBytes;
                _sampleRate = format.SampleRate;
                _chunkFrames = format.SampleRate * ExternalOutputStream.PumpPeriodMs / 1000;
                _write = 0;
                _queued = 0;
                // 写指针归服务端：从 0 起（客户端读指针也从 0 起：OpenStream 的写指针初值参数输入方向恒 0）
                _memory.Write(Buffer + ExternalOutputStream.ClientBlockOffset + ExternalOutputStream.IndexOffset, 0u);
            }
        }

        // 输入流的 SetGain：SDK 里只见输出流调（OutputStreamImpl::SetGain → sysmix::SetStreamGain）；输入流语义不知道，报红
        public void SetGain(float gain, long fadeNs)
        {
            throw new NotImplementedException($"audsmx:u IExternalStreamSession 4（输入流）：增益 {gain}、渐变 {fadeNs} ns（只见输出流调，输入流语义不知道）");
        }

        public void Attach()
        {
            lock (_lock)
            {
                _broken = false;
                _queued = _write;
                _startNs = PerformanceCounter.ElapsedNanoseconds;
                _grantedFrames = 0;
                _session.Start();
                _attached = true;
            }

            _stopPump = false;
            _pump = new Thread(PumpLoop) { Name = "S2A.audsmx.InPump", IsBackground = true };
            _pump.Start();
        }

        public void Detach()
        {
            _stopPump = true;
            _pump?.Join();
            _pump = null;

            lock (_lock)
            {
                if (!_attached)
                {
                    return;
                }

                CollectFilled();
                _session.Stop();

                // 没写满的块作废：写指针停在已写满处，下次挂上从那里接着写
                while (_inFlight.TryDequeue(out (AudioBuffer Buffer, int Frames) block))
                {
                    _session.UnregisterBuffer(block.Buffer);
                }

                _queued = _write;
                _attached = false;
            }
        }

        private void PumpLoop()
        {
            // 与输出流搬运线程同一套：实时调度、5 ms 绝对时限网格（ExternalOutputStream.PumpLoop）
            long periodNs = ExternalOutputStream.PumpPeriodMs * 1_000_000L;
            Ryujinx.Common.S2ARealtime.Promote("audsmx 收音搬运线程", periodNs, periodNs / 2, periodNs);

            long next = PerformanceCounter.ElapsedNanoseconds;

            while (!_stopPump)
            {
                Pump();

                next += periodNs;
                long now = PerformanceCounter.ElapsedNanoseconds;

                if (now < next)
                {
                    Ryujinx.Common.PreciseSleep.PreciseSleepHelper.SleepUntilTimePointSingleWake(
                        PerformanceCounter.ElapsedTicks + (next - now) * PerformanceCounter.TicksPerMillisecond / 1_000_000);
                }
                else
                {
                    next = now;
                }
            }
        }

        // 一轮搬运：收宿主写满的块 → 推写指针、触发数据事件；按游戏读指针与墙钟应得帧数 → 清零后交新块给宿主写
        public void Pump()
        {
            Pump(PerformanceCounter.ElapsedNanoseconds);
        }

        public void Pump(long nowNs)
        {
            lock (_lock)
            {
                if (!_attached || _broken)
                {
                    return;
                }

                CollectFilled();

                uint read = _memory.Read<uint>(Buffer + ExternalOutputStream.ServerBlockOffset + ExternalOutputStream.IndexOffset);

                if (read >= (uint)_capacity)
                {
                    _broken = true;
                    Console.Error.WriteLine($"[声] 【红】audsmx:u 外部输入流「{Name}」：读指针 {read} 不在环里（容量 {_capacity} 帧），停止收音");
                    return;
                }

                // 墙钟应得帧数；游戏不读（环满）时欠账封顶一环，不攒成之后一口气灌满
                long due = (nowNs - _startNs) * _sampleRate / 1_000_000_000L - _grantedFrames;

                if (due > _capacity)
                {
                    _grantedFrames += due - _capacity;
                    due = _capacity;
                }

                int free = _capacity - 1 - ((_queued - (int)read + _capacity) % _capacity);

                while (free > 0 && due > 0 && _inFlight.Count < MaxInFlight)
                {
                    int frames = (int)Math.Min(Math.Min(free, _capacity - _queued), Math.Min(_chunkFrames, due));
                    ulong address = Buffer + ExternalOutputStream.RingOffset + (ulong)_queued * (ulong)_frameBytes;
                    ulong bytes = (ulong)frames * (ulong)_frameBytes;

                    _memory.Fill(address, bytes, 0);

                    AudioBuffer buffer = new() { BufferTag = ++_tag, DataPointer = address, DataSize = bytes };

                    if (!_session.RegisterBuffer(buffer))
                    {
                        break;
                    }

                    _session.QueueBuffer(buffer);
                    _inFlight.Enqueue((buffer, frames));
                    _queued = (_queued + frames) % _capacity;
                    _grantedFrames += frames;
                    free -= frames;
                    due -= frames;
                }
            }
        }

        private void CollectFilled()
        {
            int frames = 0;

            while (_inFlight.TryPeek(out (AudioBuffer Buffer, int Frames) block) && _session.WasBufferFullyConsumed(block.Buffer))
            {
                _inFlight.Dequeue();
                _session.UnregisterBuffer(block.Buffer);
                frames += block.Frames;
            }

            if (frames > 0)
            {
                _write = (_write + frames) % _capacity;
                _memory.Write(Buffer + ExternalOutputStream.ClientBlockOffset + ExternalOutputStream.IndexOffset, (uint)_write);
                DataEvent?.Signal();
            }
        }

        public void Close()
        {
            Detach();

            lock (_lock)
            {
                _session?.Dispose();
                _session = null;
            }
        }

        public void Dispose()
        {
            Close();
        }
    }

    partial class AudioSystemMixerUserSessionManager : IServiceObject
    {
        private readonly ExternalStreamRegistry _registry;

        public AudioSystemMixerUserSessionManager(ExternalStreamRegistry registry)
        {
            _registry = registry;
        }

        // IUserSessionManager 0：带 pid、复制句柄 1 个（客户端本进程句柄，nn::dd::GetCurrentProcessHandle）、in u32 @0、u64 @8；回 IUserSession（域对象）。
        //   SDK 的 sysmix::detail::InitializeImpl 只发「u32 = 1、u64 = 0」，两者语义不知道，别的取值报红。
        //   进程句柄用来读写游戏工作缓冲（外部流的环在游戏内存里）
        [CmifCommand(0)]
        public Result OpenUserSession(out AudioSystemMixerUserSession session, [CopyHandle] int processHandle, uint kind, ulong reserved, [ClientProcessId] ulong pid)
        {
            if (kind != 1 || reserved != 0)
            {
                HorizonStatic.Syscall.CloseHandle(processHandle);

                throw new NotImplementedException($"audsmx:u IUserSessionManager 0：u32 {kind}、u64 0x{reserved:X}（SDK 只发 1、0，别的语义不知道）");
            }

            IVirtualMemoryManager memory = HorizonStatic.Syscall.GetMemoryManagerByProcessHandle(processHandle);

            session = new AudioSystemMixerUserSession(_registry, memory, processHandle);

            return Result.Success;
        }
    }

    partial class AudioSystemMixerUserSession : IServiceObject, IDisposable
    {
        // CreateStream 的标志：低 3 位 = 4 是「客户端写、服务端读」的输出流（客户端 WriteStreamImpl 要求 (标志 & 7) == 4）；
        //   0x20：OutputStreamImpl::Open 在声道数 < 6 或 OutputStreamFlag 位 0 置位时带上，语义不知道（本实现不因它改变行为）；
        //   0x40 / 0x80 来自 OutputStreamFlag 位 1 / 位 16，语义不知道 → 报红。时之笛两处开流（音频框架、动画播放）都是 0x24
        private const uint FlagClientWrites = 0x4;
        private const uint FlagKnownMask = 0x24;

        // 输入流（InputStreamImpl::Open 0x1f6984：CreateStream 标志恒 2）：低 3 位 = 2 是「服务端写、客户端读」（客户端 ReadStream 只认输入方向）。
        //   时之笛只发 2；别的位语义不知道 → 报红
        private const uint FlagServerWrites = 0x2;

        private readonly ExternalStreamRegistry _registry;
        private readonly IVirtualMemoryManager _memory;
        private int _processHandle;

        public AudioSystemMixerUserSession(ExternalStreamRegistry registry, IVirtualMemoryManager memory, int processHandle)
        {
            _registry = registry;
            _memory = memory;
            _processHandle = processHandle;
        }

        // 1 CreateStream：A 缓冲（流名，长度 = 字符串长）；带 pid；in u32 设备 @0（devmgr 401 回的档案 +0x220）、u32 标志 @4、aruid @8、
        //   u64 工作缓冲地址 @0x10、u64 长度 @0x18；回 u32 流号 @0（devmgr 700 / 800 用）+ IExternalStreamSession（域对象）
        [CmifCommand(1)]
        public Result CreateStream(
            out uint streamId,
            out AudioSystemMixerExternalStreamSession stream,
            [Buffer(HipcBufferFlags.In | HipcBufferFlags.MapAlias)] ReadOnlySpan<byte> name,
            uint device,
            uint flags,
            AppletResourceUserId appletResourceId,
            ulong buffer,
            ulong size,
            [ClientProcessId] ulong pid)
        {
            bool output = device == AudioDeviceManagerUserSession.VirtualOutputDeviceToken && (flags & 7) == FlagClientWrites && (flags & ~FlagKnownMask) == 0;
            bool input = device == AudioDeviceManagerUserSession.VirtualInputDeviceToken && flags == FlagServerWrites;

            if (!output && !input)
            {
                throw new NotImplementedException($"audsmx:u IUserSession 1：设备 {device}、标志 0x{flags:X}（只认 devmgr 401 回的本机虚拟输出设备 {AudioDeviceManagerUserSession.VirtualOutputDeviceToken} + 输出流 0x4 / 0x24、" +
                    $"虚拟输入设备 {AudioDeviceManagerUserSession.VirtualInputDeviceToken} + 输入流 0x2，其余组合语义不知道）");
            }

            int length = name.IndexOf((byte)0);
            string streamName = Encoding.ASCII.GetString(length < 0 ? name : name[..length]);
            IExternalStream impl = output
                ? new ExternalOutputStream(_registry.OutputManager, _memory, buffer, size, streamName)
                : new ExternalInputStream(_registry.InputDriver, _memory, buffer, size, streamName);

            streamId = _registry.Add(impl);
            stream = new AudioSystemMixerExternalStreamSession(_registry, streamId, impl);

            return Result.Success;
        }

        public void Dispose()
        {
            if (_processHandle != 0)
            {
                HorizonStatic.Syscall.CloseHandle(_processHandle);
                _processHandle = 0;
            }
        }
    }

    partial class AudioSystemMixerExternalStreamSession : IServiceObject, IDisposable
    {
        // AcquireStreamDataEvent 的通知方式：输出流 / 输入流 / 注入都发 2，OutputRecorder 发 1
        private const uint NotificationModeStream = 2;

        private readonly ExternalStreamRegistry _registry;
        private readonly uint _id;
        private readonly IExternalStream _impl;
        private bool _disposed;

        public AudioSystemMixerExternalStreamSession(ExternalStreamRegistry registry, uint id, IExternalStream impl)
        {
            _registry = registry;
            _id = id;
            _impl = impl;
        }

        // 0 OpenStream：in StreamFormat @0、s32 帧数 @0xc（环容量 − 1）、s32 @0x10（客户端拿它当写指针初值，输出 / 输入流都恒 0）、u32 标志 @0x14；只回结果码
        [CmifCommand(0)]
        public Result OpenStream(StreamFormat format, int frameCount, int initialWriteIndex, uint flags)
        {
            if (initialWriteIndex != 0 || flags != 0)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 0：写指针初值 {initialWriteIndex}、标志 0x{flags:X}（输出流只发 0、0；标志位 0 = 带时间戳表的另一套环协议，没实现）");
            }

            if (_impl.IsOpen)
            {
                throw new NotImplementedException("audsmx:u IExternalStreamSession 0：流已打开又打开（不知道真机回哪个结果码）");
            }

            _impl.Open(format, frameCount);

            return Result.Success;
        }

        // 1 CloseStream：无入参，只回结果码（客户端先 devmgr 800 摘下再关）
        [CmifCommand(1)]
        public Result CloseStream()
        {
            if (!_impl.IsOpen || _impl.IsAttached)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 1：流{(_impl.IsOpen ? "还挂在设备上" : "没打开")}就关（不知道真机回哪个结果码）");
            }

            _impl.Close();

            return Result.Success;
        }

        // 4 SetGain：in f32 增益 @0、s64 渐变时长 ns @8（TimeSpan）；只回结果码。OutputStreamImpl::SetGain → sysmix::SetStreamGain：
        //   客户端先拒 gain < 0、gain > 128、fade > 5 秒，再经代理 vtable +0x40 发命令 4（11-23 Switch 2 sdk 反汇编：0x26f20c 打包 w1 = 增益位、x2 = 渐变）。
        //   返回错游戏就 Abort（D-KY7F）
        [CmifCommand(4)]
        public Result SetGain(float gain, long fadeNs)
        {
            if (!_impl.IsOpen)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 4：流没打开就设增益 {gain}（不知道真机回哪个结果码）");
            }

            _impl.SetGain(gain, fadeNs);

            return Result.Success;
        }

        // 7 AcquireStreamDataEvent：in u32 通知方式 @0、s32 @4（OutputStreamImpl::Open 传游戏给的第 2 个帧数，游戏都给「缓冲帧数 − 1~5 ms」）；回复制句柄 1 个。
        //   方式 2 的确切触发条件不知道（可写帧数到阈值？每消费阈值帧？）。本实现：服务端每推进一次读指针就触发——是任一种真机条件的超集：
        //   等它的游戏（时之笛音频框架线程无超时 WaitSystemEvent 后查可写帧数）只会多醒、不会少醒。输入流同样发方式 2（InputStreamImpl::Open 0x1f69c0），
        //   本实现每推进一次写指针就触发。别的方式报红
        [CmifCommand(7)]
        public Result AcquireStreamDataEvent([CopyHandle] out int eventHandle, uint mode, int threshold)
        {
            if (mode != NotificationModeStream || !_impl.IsOpen || _impl.DataEvent != null)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 7：方式 {mode}、阈值 {threshold}、流{(_impl.IsOpen ? "" : "没")}打开、{(_impl.DataEvent != null ? "已领过事件" : "未领事件")}（只实现输出 / 输入流的方式 2）");
            }

            _impl.DataEvent = new AudioEvent();
            eventHandle = _impl.DataEvent.GetReadableHandle();

            return Result.Success;
        }

        // 8 ReleaseStreamDataEvent：无入参，只回结果码
        [CmifCommand(8)]
        public Result ReleaseStreamDataEvent()
        {
            if (_impl.DataEvent == null || _impl.IsAttached)
            {
                throw new NotImplementedException($"audsmx:u IExternalStreamSession 8：{(_impl.DataEvent == null ? "没领过事件" : "流还挂在设备上")}");
            }

            AudioEvent dataEvent = _impl.DataEvent;
            _impl.DataEvent = null;
            dataEvent.Dispose();

            return Result.Success;
        }

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _registry.Remove(_id);
            _impl.Dispose();
            _impl.DataEvent?.Dispose();
            _impl.DataEvent = null;
        }
    }
}
