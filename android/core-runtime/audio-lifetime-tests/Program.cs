// 真实 AudioManagers / AudioRendererManager / AudioProcessor + Dummy 驱动；只替换 Horizon 内核事件。
using System;
using System.Collections.Concurrent;
using System.Reflection;
using System.Threading;
using Ryujinx.Audio.Backends.Dummy;
using Ryujinx.Audio.Backends.CompatLayer;
using Ryujinx.Audio.Integration;
using Ryujinx.Horizon;
using Ryujinx.Horizon.Audio;
using S2A.Services;

var lifetime = new HorizonAudioLifetime();
S2AHorizonHooks.AudioCreated = lifetime.Register;
var audioBackend = new RecordingAudioThreadDriver();
var driver = new CompatLayerHardwareDeviceDriver(audioBackend);
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
if (!audioBackend.Threads.ContainsKey(worker.ManagedThreadId) ||
    audioBackend.Threads.Count != (Environment.GetEnvironmentVariable("S2A_AUDIO_INLINE") == "0" ? 2 : 1))
    throw new Exception("owned audio threads did not call the real backend's affinity hook");
lifetime.Dispose();
if (worker.IsAlive) throw new Exception("audio renderer survived owner cleanup");
driver.Dispose();
lifetime.Dispose();
managers.Dispose(); // 服务入口晚到的 Shutdown 幂等。
Console.WriteLine("PASS audio lifetime: real manager/processor joined before driver pause event disposal; repeated cleanup safe");
Console.WriteLine("PASS audio placement hook: owned renderer threads registered with the real backend");

sealed class RecordingAudioThreadDriver : DummyHardwareDeviceDriver, IHardwareDeviceDriver
{
    public readonly ConcurrentDictionary<int, byte> Threads = new();
    void IHardwareDeviceDriver.S2AConfigureRenderThread() => Threads.TryAdd(Environment.CurrentManagedThreadId, 0);
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
