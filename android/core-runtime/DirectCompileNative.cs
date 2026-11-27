// C2：NativeAOT 侧的直连编译 provider。与离线 CLI 调用同一个 DirectShaderCompiler.Compile；
// 只做跨界参数校验、异常转换和 emit 复制，不在这里另写一套翻译。
using S2A.DirectCompiler;
using System;
using System.Diagnostics;
using System.Threading;
using System.Runtime.InteropServices;

namespace S2A.Android.Aot
{
    public static unsafe class DirectCompileNative
    {
        private const int InfoPriority = 4;
        private const int ClockMonotonic = 1;
        private const int ClockThreadCpu = 3;
        private static readonly Lazy<bool> PhaseEnabled = new(PhaseRequested);
        private static int _phaseSamples;

        [StructLayout(LayoutKind.Sequential)]
        private struct Timespec { public long Seconds, Nanoseconds; }

        [DllImport("libc.so", EntryPoint = "__system_property_get")]
        private static extern int PropertyGet([MarshalAs(UnmanagedType.LPStr)] string name, byte* value);

        [DllImport("libc.so", EntryPoint = "clock_gettime")]
        private static extern int ClockGettime(int clock, out Timespec time);

        [DllImport("liblog.so", EntryPoint = "__android_log_write")]
        private static extern int LogWrite(int priority, string tag, string text);

        private static bool PhaseRequested()
        {
            if (IntPtr.Size != 8) return false;
            try
            {
                byte* value = stackalloc byte[92]; // Android PROP_VALUE_MAX
                return PropertyGet("debug.s2a.nvk_compile_phases", value) == 1 && value[0] == '1';
            }
            catch (DllNotFoundException) { return false; }
            catch (EntryPointNotFoundException) { return false; }
        }

        private static long TimeNs(int clock) =>
            ClockGettime(clock, out Timespec time) == 0 ? time.Seconds * 1_000_000_000 + time.Nanoseconds : 0;

        private static long CpuNow() => TimeNs(ClockThreadCpu);

        private static string Pair((long WallTicks, long CpuNs) value) =>
            $"{(long)(value.WallTicks * (1e9 / Stopwatch.Frequency))}/{Math.Max(0, value.CpuNs)}";

        private static void Report(DirectShaderCompiler.PhaseProbe p, ulong session, ulong generation, long mono)
        {
            string line = $"compile_phase phase=managed session={session} generation={generation} mono_ns={mono} " +
                $"req={p.RequestSha ?? "-"} units=wall_ns/cpu_ns copy={Pair(p.InputCopy)} request={Pair(p.Parse)} " +
                $"translate={Pair(p.Translate)} spirv={Pair((p.SpirvWallTicks, p.SpirvCpuNs))} reflect={Pair(p.Reflect)} " +
                $"serialize={Pair(p.Serialize)} emit={Pair(p.Emit)}";
            try { LogWrite(InfoPriority, "S2Arm", line); }
            catch (DllNotFoundException) { }
            catch (EntryPointNotFoundException) { }
        }

        // 对应 s2a_direct_api_v1 之外的管理侧 provider：native 在 Activate 前把它登记给
        // s2a_direct_bind_compiler_v1。返回码沿用 s2a_direct_result。
        [UnmanagedCallersOnly(EntryPoint = "s2a_direct_compile_v1")]
        public static int Compile(ulong session, ulong generation, DirectBytes request, nint emit, nint sink)
        {
            if (session == 0 || generation == 0 || request.Data == 0 || request.Bytes == 0 ||
                request.Bytes > DirectShaderCompiler.MaxRequestBytes || emit == 0)
                return DirectAbi.Argument;
            DirectShaderCompiler.PhaseProbe probe = PhaseEnabled.Value &&
                Volatile.Read(ref _phaseSamples) < 128 && Interlocked.Increment(ref _phaseSamples) <= 128
                ? new DirectShaderCompiler.PhaseProbe(CpuNow) : null;
            long mono = probe == null ? 0 : TimeNs(ClockMonotonic);
            var copyStart = probe?.Now() ?? default;
            byte[] input;
            try
            {
                input = new ReadOnlySpan<byte>((void*)request.Data, checked((int)request.Bytes)).ToArray();
            }
            catch (Exception) { return DirectAbi.Argument; }
            if (probe != null) probe.InputCopy = probe.Since(copyStart);
            byte[] output;
            try
            {
                output = DirectShaderCompiler.Compile(input, probe);
            }
            catch (DirectCompileException error)
            {
                if (probe != null) Report(probe, session, generation, mono);
                Console.Error.WriteLine($"[s2a] direct compile rejected ({error.Code}): {error.Message}");
                EmitError(emit, sink, error.Code, error.Message);   // 让 native 能记下原因（ABI 只有状态码）
                // 身份不匹配必须单独回 IDENTITY：归到 Argument(-1) 会让上层只看到“参数不对/无结果”。
                if (error.Code == "IDENTITY") return DirectAbi.Identity;
                return error.Code is "INVALID_JSON" or "DUPLICATE_KEY" or "MISSING_FIELD" or "INVALID_BASE64"
                    or "RESOURCE_LIMIT" or "MISSING_FACT" or "UNSUPPORTED_VERSION"
                    or "UNSUPPORTED_PROFILE" or "UNSUPPORTED_PROGRAM"
                    ? DirectAbi.Argument : DirectAbi.Compile;
            }
            catch (Exception error)
            {
                if (probe != null) Report(probe, session, generation, mono);
                Console.Error.WriteLine("[s2a] direct compile internal error: " + error);
                EmitError(emit, sink, "INTERNAL", error.GetType().Name + ": " + error.Message);
                return DirectAbi.Internal;
            }
            var callback = (delegate* unmanaged<nint, DirectBytes, int>)emit;
            fixed (byte* data = output)
            {
                var bytes = new DirectBytes { Data = (nint)data, Bytes = (ulong)output.Length };
                var emitStart = probe?.Now() ?? default;
                int result = callback(sink, bytes);
                if (probe != null)
                {
                    probe.Emit = probe.Since(emitStart);
                    Report(probe, session, generation, mono);
                }
                return result == 0 ? DirectAbi.Ok : DirectAbi.Compile;
            }
        }

        // 编译失败时用同一个 emit 通道回一段最小错误 JSON。native 侧解析后写进 red.log/logcat：
        // s2a_direct_compile_fn 这个 ABI 只回状态码，没有错误字符串，只靠 Console.Error 在安卓上看不到。
        private static void EmitError(nint emit, nint sink, string code, string message)
        {
            if (emit == 0) return;
            string json = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"" + Json(code) +
                          "\",\"message\":\"" + Json(message) + "\"}";
            byte[] bytesIn = System.Text.Encoding.UTF8.GetBytes(json);
            var callback = (delegate* unmanaged<nint, DirectBytes, int>)emit;
            fixed (byte* data = bytesIn)
                callback(sink, new DirectBytes { Data = (nint)data, Bytes = (ulong)bytesIn.Length });
        }

        private static string Json(string text)
        {
            if (string.IsNullOrEmpty(text)) return "";
            var sb = new System.Text.StringBuilder(text.Length + 8);
            foreach (char c in text)
            {
                if (c == '"' || c == '\\') sb.Append('\\').Append(c);
                else if (c == '\n') sb.Append("\\n");
                else if (c < 0x20) sb.Append(' ');
                else sb.Append(c);
            }
            return sb.ToString();
        }
    }
}
