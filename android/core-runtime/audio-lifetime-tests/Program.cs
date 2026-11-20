// 真实 AudioManagers / AudioRendererManager / AudioProcessor + Dummy 驱动；只替换 Horizon 内核事件。
using System;
using System.Reflection;
using System.Threading;
using Ryujinx.Audio.Backends.Dummy;
using Ryujinx.Audio.Backends.CompatLayer;
using Ryujinx.Audio.Common;
using Ryujinx.Audio.Integration;
using Ryujinx.Horizon;
using Ryujinx.Horizon.Audio;
using Ryujinx.Memory;
using S2A.Services;

if (args.Length != 0 && args[0] == "partial-start")
{
    using var partialDriver = new PartialStartDriver();
    using var partial = new Ryujinx.Audio.Renderer.Server.AudioRendererManager(null);
    partial.Initialize(Array.Empty<IWritableEvent>(), new CompatLayerHardwareDeviceDriver(partialDriver));
    var start = partial.GetType().GetMethod("StartLocked", BindingFlags.Instance | BindingFlags.NonPublic);
    try
    {
        start.Invoke(partial, null);
        throw new Exception("the second audio output unexpectedly opened");
    }
    catch (TargetInvocationException e) when (e.InnerException?.Message == PartialStartDriver.Failure)
    {
        if (partialDriver.Opens != 2)
            throw new Exception("the first output did not open before the injected failure");
    }
    partial.StopSendingCommands();
    if (partial.Processor.OutputDevices != null)
        throw new Exception("partially opened audio outputs were not cleaned up");
    start.Invoke(partial, null);
    partial.StopSendingCommands();
    if (partial.Processor.OutputDevices != null)
        throw new Exception("audio outputs were not cleaned up after retry");
    Console.WriteLine("PASS partial audio start cleanup and retry");
    return;
}

var lifetime = new HorizonAudioLifetime();
S2AHorizonHooks.AudioCreated = lifetime.Register;
var driver = new CompatLayerHardwareDeviceDriver(new DummyHardwareDeviceDriver());
var managers = new AudioManagers(driver, null);
var renderer = managers.AudioRendererManager;
const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
// 没有 guest 音频 session 也执行同一管理线程和 DSP 周期；确定性地在下一次 Signal 之前关驱动事件。
var sessionLock = (System.Threading.Lock)renderer.GetType().GetField("_sessionLock", flags).GetValue(renderer);
Thread worker;
lock (sessionLock)
{
    renderer.GetType().GetMethod("StartLocked", flags).Invoke(renderer, null);
    worker = (Thread)renderer.GetType().GetField("_workerThread", flags).GetValue(renderer);
    if (!SpinWait.SpinUntil(() => (worker.ThreadState & ThreadState.WaitSleepJoin) != 0, 1000))
        throw new Exception("worker did not reach locked SendCommands boundary");
    if (args.Length != 0 && args[0] == "old-order") driver.Dispose();
}
if (args.Length != 0 && args[0] == "old-order")
{
    Thread.Sleep(1000); // 未处理的异常应先终止子进程，不能把程序正常结束当作红。
    throw new Exception("missing expected AudioProcessor.Signal failure");
}
lifetime.Dispose();
if (worker.IsAlive) throw new Exception("audio renderer survived owner cleanup");
driver.Dispose();
lifetime.Dispose();
managers.Dispose(); // 服务入口晚到的 Shutdown 幂等。
Console.WriteLine("PASS audio lifetime: real manager/processor joined before driver pause event disposal; repeated cleanup safe");

sealed class PartialStartDriver : DummyHardwareDeviceDriver, IHardwareDeviceDriver
{
    public const string Failure = "injected failure on the second audio output";
    public int Opens { get; private set; }

    IHardwareDeviceSession IHardwareDeviceDriver.OpenDeviceSession(IHardwareDeviceDriver.Direction direction,
        IVirtualMemoryManager memoryManager, SampleFormat sampleFormat, uint sampleRate, uint channelCount)
    {
        if (++Opens == 2)
            throw new InvalidOperationException(Failure);
        return base.OpenDeviceSession(direction, memoryManager, sampleFormat, sampleRate, channelCount);
    }
}

namespace Ryujinx.Horizon
{
    static class S2AHorizonHooks { public static Action<IDisposable> AudioCreated; }
}
namespace Ryujinx.Horizon.Sdk.Audio
{
    class AudioEvent : IWritableEvent
    {
        public void Signal() { }
        public void Clear() { }
    }
}
