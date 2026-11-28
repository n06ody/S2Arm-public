using Ryujinx.Audio.Integration;
using Ryujinx.HLE.HOS.Services.Hid;
using Ryujinx.HLE.HOS.SystemState;
using Ryujinx.Memory;
using Ryujinx.Graphics.Gpu;
using Ryujinx.Graphics.Vulkan;
using S2A.Audio.AAudio;
using S2A.Services;
using S2Arm.Backends.Vulkan.Runtime;
using S2Arm.Backends.Vulkan.Wsi;
using System;
using System.IO;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace S2A.Android.Aot
{
    // Called by the native game composition only after its real module/memory
    // initialization. Diagnostic self-tests do not call this entry.
    public static unsafe class AndroidGameRuntime
    {
        private static readonly AndroidServiceState State = new();
        private static AndroidHostApi _host;
        private static AndroidVulkanRuntime _gpu;
        private static DirectSession _directSession;   // 非空 = 本局走 native NVN direct（不建 generic GPU）
        private static DirectOwnerLoop _directOwner;   // F4.4：provider.poll 的 managed 调用者
        private static DirectRenderer _directRenderer;
        private static AndroidRunMetrics _metrics;
        private static readonly AndroidPresentCounter PresentCounter = new();
        private static bool _direct;                   // main：冻结的 vk-direct 模式（只看 Native.GfxMode）
        [DllImport("s2a_native")] private static extern int s2a_plat_android_system_language(nint api, byte* output, nuint capacity);
        [DllImport("s2a_native")] private static extern long s2a_plat_android_memory_footprint();
        [DllImport("s2a_native")] private static extern int s2a_android_vk_set_layer_query(delegate* unmanaged<long> query);
        [DllImport("s2a_native")] private static extern int s2a_android_vk_session_stop();
        [DllImport("s2a_native")] private static extern ulong s2a_android_vk_present_count();
        [DllImport("s2a_native")] private static extern ulong s2a_android_nvn_present_count();
        // t-direct-frametimes：逐帧帧时间局末转储（generic 经此触发；direct 已由 adapter shutdown 转过，空记录无输出）。
        [DllImport("s2a_native")] private static extern void s2a_frame_log_dump();
        // 手机六轴 / 震动（#528 第 10 项，android/native/phone_feedback.c）：宿主函数地址登记给服务层（主程序句柄查不到本库导出）
        [DllImport("s2a_native")] private static extern void s2a_android_pad_feedback(InputPump.HostFeedback* hooks);

        [UnmanagedCallersOnly]
        [DynamicDependency("get_SurfaceFlinger", typeof(Ryujinx.HLE.HOS.Horizon))]
        [DynamicDependency("get_RenderLayerId", "Ryujinx.HLE.HOS.Services.SurfaceFlinger.SurfaceFlinger", "Ryujinx.HLE")]
        private static long DirectLayer()
        {
            try
            {
                var device = Host.Device;
                if (device == null) return 0;
                // Read-only adapter for existing internal HLE state. Explicit metadata
                // roots make this work under NativeAOT without changing shared HLE.
                object flinger = typeof(Ryujinx.HLE.HOS.Horizon).GetMethod("get_SurfaceFlinger", BindingFlags.Instance | BindingFlags.NonPublic)
                    ?.Invoke(device.System, null) ?? throw new MissingMethodException("HLE SurfaceFlinger getter");
                return (long)(flinger.GetType().GetMethod("get_RenderLayerId")?.Invoke(flinger, null)
                    ?? throw new MissingMethodException("HLE render layer getter"));
            }
            catch (Exception error) { Fatal(ServiceError.Host, error); return 0; }
        }

        [UnmanagedCallersOnly(EntryPoint = "s2a_android_services_present_count")]
        public static long GetPresentCount()
        {
            if (_directSession != null) return unchecked((long)s2a_android_nvn_present_count());
            return _direct ? unchecked((long)s2a_android_vk_present_count()) : PresentCounter.Count;
        }
        [UnmanagedCallersOnly(EntryPoint = "s2a_android_services_request_applet_exit")]
        public static int RequestAppletExit(ulong session)
        {
            var snapshot = State.Snapshot();
            if (!_direct || snapshot.Session != session || snapshot.State != (uint)ServiceState.Ready || Host.Device == null) return -1;
            try
            {
                DirectAppletExit.Request(Host.Device.System);
                Console.Error.WriteLine("[vk-direct] 已发送正式Applet Exit消息，等待游戏清理");
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("[vk-direct] Applet Exit不可用：" + error);
                return -1;
            }
        }
        private static readonly object CleanupLock = new();
        private static bool _cleanupDone;
        private static Exception _cleanupFailure;
        private static int _stopSent;
        [ThreadStatic] private static ulong _attachedSession;
        [ThreadStatic] private static int _attachedThreadId;

        // v2：direct 唯一入口。generic 仍等价于 v1（图形表必须为空、mode=0）。
        // direct 走 native NVN 前端 + d2r 引擎（托管侧不建 generic GPU）；身份/表校验失败一律报错，不静默降级。
        [UnmanagedCallersOnly(EntryPoint = "s2a_android_services_start_v2")]
        public static int StartV2(DirectServicesRequestV2* request, AndroidServicesStatus* result)
        {
            if (!OutputValid(result)) return (int)ServiceError.Argument;
            if (request == null || request->Size < DirectAbi.ServicesRequestV2Size)
                return Write(result, ServiceError.Argument);
            if (request->Version != 2) return Write(result, ServiceError.Version);
            if (request->Flags != 0 || request->GraphicsMode > (uint)GraphicsMode.Direct)
                return Write(result, ServiceError.Argument);
            if (request->GraphicsMode == (uint)GraphicsMode.Generic && request->Graphics != 0)
                return Write(result, ServiceError.Argument);
            if (request->Session == 0 || request->NativeOwner == 0 || request->HostApi == 0 ||
                request->LeaseApi == 0 || request->ConsoleMode > 1 || request->Reserved != 0)
                return Write(result, ServiceError.Argument);
            if (request->GraphicsMode == (uint)GraphicsMode.Direct)
            {
                if (request->Graphics == 0) return Write(result, ServiceError.Argument);
                DirectApiV1 table = *(DirectApiV1*)request->Graphics;
                int check = DirectAbi.TableCheck(table);
                if (check != DirectAbi.Ok)
                {
                    Console.Error.WriteLine($"[s2a] direct services rejected: table check {check} (size={table.Size} version={table.Version})");
                    return Write(result, check == DirectAbi.VersionMismatch ? ServiceError.Version : ServiceError.Argument);
                }
                if (table.Session != request->Session)
                {
                    Console.Error.WriteLine($"[s2a] direct services rejected: table session {table.Session} != request {request->Session}");
                    return Write(result, ServiceError.Argument);
                }
                // direct 会话：先 binder（AOT 的 s2a_direct_compile_v1）→ Activate（native 侧的引擎初始化）。
                // 只有两步都成功才算 direct 起局；负码一律当失败，绝不静默降级成 generic。
                DirectSession session;
                try { session = new DirectSession(in table); }
                catch (ArgumentException error)
                {
                    Console.Error.WriteLine($"[s2a] direct services rejected: {error.Message}");
                    return Write(result, ServiceError.Argument);
                }
                nint compile = 0;
                unsafe { compile = (nint)(delegate* unmanaged<ulong, ulong, DirectBytes, nint, nint, int>)&DirectCompileNative.Compile; }
                int bind = session.BindCompiler(compile);
                if (bind == DirectAbi.Ok)
                {
                    unsafe
                    {
                        bind = session.BindBinaryCompiler(
                            (nint)(delegate* unmanaged<ulong, ulong, DirectBytes, nint, nint, int>)&DirectCompileNative.CompileBinary,
                            (nint)(delegate* unmanaged<DirectBytes, nint, nint, int>)&DirectCompileNative.EncodeLegacy);
                    }
                }
                if (bind != DirectAbi.Ok)
                {
                    Console.Error.WriteLine($"[s2a] direct services rejected: compiler bind failed ({bind})");
                    session.Destroy();
                    return Write(result, bind == DirectAbi.Unsupported ? ServiceError.Provider : ServiceError.Argument);
                }
                int activate = session.Activate();
                if (activate != DirectAbi.Ok)
                {
                    Console.Error.WriteLine($"[s2a] direct services rejected: activate failed ({activate})");
                    session.RequestStop();
                    session.Destroy();
                    return Write(result, ServiceError.Provider);
                }
                Console.Error.WriteLine("[s2a] direct session active: compiler bound, engine activated");
                if (session.Query(out DirectStatusV1 directStatus) == DirectAbi.Ok)
                    Console.Error.WriteLine($"[s2a] direct session status: state={directStatus.State} blockers={directStatus.Blockers} error={directStatus.Error}");
                // 走 direct composition：HLE/服务照常起，但托管侧不建 generic GPU，图形归 native。
                AndroidServicesRequest directRequest = V1Request(request);
                return StartCore(&directRequest, result, session);
            }
            AndroidServicesRequest v1 = V1Request(request);
            return StartCore(&v1, result);
        }

        // v2 的 v1 前缀与 v1 同布局：direct/generic 都从同一份字段构造，避免两处漂移。
        private static AndroidServicesRequest V1Request(DirectServicesRequestV2* request)
        {
            AndroidServicesRequest v1 = default;
            v1.Size = (uint)sizeof(AndroidServicesRequest);
            v1.Version = 1;
            v1.Session = request->Session;
            v1.NativeOwner = request->NativeOwner;
            v1.HostApi = request->HostApi;
            v1.GameDataDirectory = request->GameDataDirectory;
            v1.StateDirectory = request->StateDirectory;
            v1.LeaseApi = request->LeaseApi;
            v1.ConsoleMode = request->ConsoleMode;
            v1.Reserved = 0;
            return v1;
        }

        [UnmanagedCallersOnly(EntryPoint = "s2a_android_services_start")]
        public static int Start(AndroidServicesRequest* request, AndroidServicesStatus* result)
            => StartCore(request, result);

        internal static int StartCore(AndroidServicesRequest* request, AndroidServicesStatus* result)
            => StartCore(request, result, null);

        private static void DiscardRejectedDirect(DirectSession directSession)
        {
            if (directSession == null) return;
            int stop = directSession.RequestStop();
            int destroy = directSession.Destroy();
            Console.Error.WriteLine($"[s2a] direct session rejected before start: stop={stop} destroy={destroy}");
        }

        // direct != null：本局的图形归 native（NVN 前端 + d2r 引擎），托管侧不创建第二套 GPU。
        internal static int StartCore(AndroidServicesRequest* request, AndroidServicesStatus* result, DirectSession directSession)
        {
            // A rejected second Start must not mutate the live session's graphics mode.
            // Publish _directSession/_direct only after State.Begin accepts this request.
            if (!OutputValid(result))
            {
                DiscardRejectedDirect(directSession);
                return (int)ServiceError.Argument;
            }
            if (request == null || request->Size < sizeof(AndroidServicesRequest))
            {
                DiscardRejectedDirect(directSession);
                return Write(result, ServiceError.Argument);
            }
            if (request->Version != 1)
            {
                DiscardRejectedDirect(directSession);
                return Write(result, ServiceError.Version);
            }
            if (request->Session == 0 || request->NativeOwner == 0 || request->HostApi == 0 ||
                request->LeaseApi == 0 || request->ConsoleMode > 1 || request->Reserved != 0)
            {
                DiscardRejectedDirect(directSession);
                return Write(result, ServiceError.Argument);
            }
            // #1422：把本局的原生 provider 句柄登记为「兜底绑定 owner」。接口里没有这个语境的消费者
            // （例如早期服务初始化）会用无参 Native.UseLinked() 兜底；安卓上主程序句柄看不见 .so 的
            // 可选导出，登记之后兜底就绑正确句柄。第二个参数 true = 本平台必须登记：若仍有兜底发生在
            // 这次登记之前，共用层会报一条红，提醒装配点要更早（这是可检查的前提，不靠运气）。
            S2A.Services.Native.RegisterFallbackOwner(request->NativeOwner, true);
            ServiceError begin = State.Begin(request->Session);
            if (begin != ServiceError.Ok)
            {
                DiscardRejectedDirect(directSession);
                return Write(result, begin);
            }
            _directSession = directSession;
            _direct = false;
            Exports.CancelWaitsHandler = RequestServiceCancellation;

            ServiceError stage = ServiceError.Host;
            try
            {
                // Native has already bound fd 1/2 to this session's private log.
                // Console may retain handles cached before that dup2; replace
                // its writers before any HostApi/provider/GPU consumer logs.
                Console.SetOut(new LogcatTee(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true }));
                Console.SetError(new LogcatTee(new StreamWriter(Console.OpenStandardError()) { AutoFlush = true }));
                _host = new AndroidHostApi(request->HostApi, request->ConsoleMode);
                // The host table is frozen before Boot; retain the returned tag for this session.
                Span<byte> languageBytes = stackalloc byte[256];
                languageBytes.Fill(0xff);
                fixed (byte* languageOut = languageBytes)
                    if (s2a_plat_android_system_language(request->HostApi, languageOut, (nuint)languageBytes.Length) != 0)
                        throw new InvalidOperationException("Platform language query failed");
                int languageEnd = languageBytes.IndexOf((byte)0);
                if (languageEnd <= 0) throw new InvalidOperationException("Platform language is empty or unterminated");
                string languageTag = new System.Text.UTF8Encoding(false, true).GetString(languageBytes[..languageEnd]);
                if (languageTag != _host.Language) throw new InvalidOperationException("Frozen HostApi language changed during Boot");
                var hostUi = AndroidHostUi.Create(_host);
                // Validate the actual Surface before admission. The final guest aspect is
                // assigned by Exports.Initialize from the native 1080-width baseline, shared
                // with the widen provider; narrow screens keep the 16:9 guest width.
                AndroidHostApi.DisplayInfo display = _host.ReadDisplay();
                string game = AndroidHostApi.ReadPath(request->GameDataDirectory);
                string state = AndroidHostApi.ReadPath(request->StateDirectory);
                _metrics = AndroidRunMetrics.TryStart(state, request->Session);
                var configuration = new HostConfiguration(game, state,
                    SystemLocale.Map(languageTag) ?? SystemLanguage.AmericanEnglish,
                    Region(_host.Region), _host.TimeZone, request->ConsoleMode == 1,
                    hostUi,
                    MemoryBlock.ReserveShared, AndroidScreenshotEncoder.Encode,
                    saveDirectory: state, systemLanguageTag: () => languageTag,
                    memoryFootprint: s2a_plat_android_memory_footprint);
                EnsureStarting();

                stage = ServiceError.Provider;
                Native.UseLinked(request->NativeOwner);
                // UseLinked also serves diagnostics, where all optional exports
                // may be absent. A game requires this complete owner group before
                // constructing the GPU/HLE composition.
                if (Native.GuestTick == null || Native.DetMode == null || Native.DetHostWrite == null)
                    throw new InvalidOperationException("Game provider lacks its clock or deterministic memory exports");
                stage = ServiceError.Gpu;
                // Boot thread only, before constructing any GPU or renderer.
                S2AGpuCaps.WritebackRequireProvider = true;
                // VK_MAKE_API_VERSION(0, 1, 1, 0); the composition only needs the encoded value.
                S2AVulkanCaps.MinimumApiVersion = (1u << 22) | (1u << 12);
                S2AVulkanCaps.UnsupportedMessage = SystemLocale.Map(_host.Language) switch
                {
                    SystemLanguage.SimplifiedChinese => "此设备的 Vulkan 版本过低，需要 Vulkan 1.1 或更高版本。",
                    SystemLanguage.TraditionalChinese => "此裝置的 Vulkan 版本過低，需要 Vulkan 1.1 或更新版本。",
                    _ => "Android's Vulkan backend requires device API 1.1 or newer.",
                };
                Console.Error.WriteLine($"[s2a] GPU caps: writebackRequireProvider={S2AGpuCaps.WritebackRequireProvider}, vulkanMinimum=0x{S2AVulkanCaps.MinimumApiVersion:x}, unsupportedMessage={S2AVulkanCaps.UnsupportedMessage}");
                int mode = Native.GfxMode == null ? 0 : Native.GfxMode();
                if (mode is not (0 or 1)) throw new InvalidOperationException("Invalid frozen graphics mode");
                // v2 NVN direct 优先；_direct 只表示 main 的冻结 vk-direct 模式。
                _direct = directSession == null && mode == 1;
                if (directSession != null)
                {
                    // direct：guest 的图形走 native 的 NVN 前端与 d2r 引擎，托管侧不许再建一套 GPU。
                    // HLE 在 Initialize 里会真的构造渲染器，所以给一个"什么都不画"的 direct 渲染器：
                    // 真正要建 GPU 资源/出图的成员一律抛 NotSupportedException（带成员名），不静默成功。
                    // F4.4：owner 控制循环从此刻起实际调用 provider.poll；起不来就按 Provider 失败收局，
                    // 不留一个“没有控制调用者”的 direct 会话。
                    _directOwner = new DirectOwnerLoop();
                    int ownerRc = _directOwner.Start(directSession, OnDirectOwnerFailure);
                    if (ownerRc != DirectAbi.Ok)
                        throw new InvalidOperationException($"direct owner control loop failed to start ({ownerRc})");
                    Console.Error.WriteLine("[s2a] direct owner loop started: provider.poll is driven by the managed owner");
                    Exports.RendererFactory = () => _directRenderer ??= new DirectRenderer();
                    Console.Error.WriteLine("[s2a] direct composition: managed GPU disabled, guest graphics go through the native direct table");
                }
                else if (_direct)
                {
                    if (_metrics != null) throw new NotSupportedException("Direct per-frame file metrics are not wired yet");
                    int registered = s2a_android_vk_set_layer_query(&DirectLayer);
                    if (registered != 0) throw new InvalidOperationException("Direct HLE layer callback rejected: " + registered);
                    Exports.RendererFactory = static () => new DirectServiceRenderer();
                }
                else
                {
                    // Debug-only evidence; native gate is identical to frame_log. Missing or
                    // incomplete capture stays unverified and must not block rendering.
                    if (!Ryujinx.Common.S2ACompileEvents.BeginSession(state, request->Session))
                        Console.Error.WriteLine("【红】compile-events 初始化失败，编译证据未证");
                    bool transferFullClears = File.Exists(Path.Combine(state, "vulkan-transfer-clears.enable"));
                    Console.Error.WriteLine($"[s2a] Vulkan full-color transfer clears: {transferFullClears}");
                    S2AFrameTrace.InitializeAndroid();
                    _gpu = new AndroidVulkanRuntime(request->LeaseApi, request->Session, error => Fatal(ServiceError.Gpu, error), game, transferFullClears, state);
                    if (_metrics != null) _gpu.FramePresented = _metrics.Present;
                    _gpu.FramePresented += PresentCounter.Observe;
                    Exports.RendererFactory = _gpu.CreateRenderer;
                }
                Exports.AfterBoot = (device, tracking) =>
                {
                    if (_direct)
                    {
                        Console.Error.WriteLine("[vk-direct] HLE/输入就绪；通用GPU线程不启动");
                        return;
                    }
                    // 选用户：共用 HostUi 自己读 AccountManager.LastOpenedUser，平台不再注入
                    // 游戏请求的出图间隔：只有拆线程 auto 判「呈现低于请求」要用时才接给运行时（0 / 1 不接，与基线一致）；metrics 开着时同一个回调也给它。
                    Action<float> frameInterval = _gpu is { WantsFrameInterval: true } ? _gpu.ObserveFrameInterval : null;
                    if (_metrics != null)
                    {
                        _metrics.SetRefresh(device.TargetVSyncInterval);
                        frameInterval += _metrics.FrameInterval;
                    }
                    Ryujinx.HLE.HOS.Services.SurfaceFlinger.S2AFramePacing.FrameIntervalChanged = frameInterval;
                    _gpu?.Start(device, tracking); // synchronous: actual initialization and tracking/worker readiness
                };
                Exports.BeforeShutdown = StopGpu;
                Exports.ShutdownHandler = ShutdownFromNative;
                Exports.ServiceFailure = OnServiceFailure;
                Exports.AudioFactory = &CreateAudio;
                Console.Error.WriteLine("[s2a] 音频驱动登记：AAudio");
                InputPump.InputHook = &ReadInput;
                InputPump.HostFeedback feedback;
                s2a_android_pad_feedback(&feedback);
                InputPump.Feedback = feedback;
                S2A.Android.Video.AndroidVideo.Register();   // 平台 adapter P1（#1069）：Nvdec 只走 S2AVideoDecoders 登记的工厂

                stage = ServiceError.Initialize;
                EnsureStarting();
                Exports.Initialize(configuration);
                Console.Error.WriteLine($"[s2a] 游戏显示宽高比：Surface {display.Width}x{display.Height} → {Ryujinx.HLE.S2ADisplay.Aspect:0.0000}（高度按主机模式）");
                _gpu?.ThrowIfFailed();
                ServiceError complete = State.Complete();
                if (complete != ServiceError.Ok) throw new OperationCanceledException("Session stopped or failed during initialization");
                return Write(result, ServiceError.Ok);
            }
            catch (Exception error)
            {
                if (State.Snapshot().State != (uint)ServiceState.StopRequested) State.Fail(stage);
                Console.Error.WriteLine($"Android services start failed (stage={stage}, error={(int)stage}): " + error);
                NotifyHostStop(3, "Android service initialization failed");
                // No guest was released by this entry. Stop the objects actually
                // created; GPU failure retains its resources for process teardown.
                try { Cleanup(); }
                catch (Exception cleanup)
                {
                    State.Fail(ServiceError.Cleanup);
                    Console.Error.WriteLine("Android services cleanup requires process teardown: " + cleanup);
                }
                return Write(result, (ServiceError)State.Snapshot().Error);
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "s2a_android_services_status")]
        public static int Status(ulong session, AndroidServicesStatus* result)
        {
            if (!OutputValid(result)) return (int)ServiceError.Argument;
            AndroidServicesStatus snapshot = State.Snapshot();
            if (session == 0 || snapshot.Session != session) return Write(result, ServiceError.Argument);
            *result = snapshot;
            return 0;
        }

        // Native must call this directly on each real guest pthread, after its
        // xstack setup and before that same thread's final native fault probe.
        // Returning success proves managed attachment/service state only; the
        // native owner still arbitrates stop and the final guest admission.
        [UnmanagedCallersOnly(EntryPoint = "s2a_android_services_attach_thread")]
        public static int AttachThread(ulong session, AndroidServicesStatus* result)
        {
            if (!OutputValid(result)) return (int)ServiceError.Argument;
            ServiceError ready = ThreadReady(session, out AndroidServicesStatus snapshot);
            if (ready != ServiceError.Ok)
            {
                *result = snapshot;
                return (int)ready;
            }
            try
            {
                AttachCurrentThread(session);
                ready = ThreadReady(session, out snapshot);
                *result = snapshot;
                return (int)ready;
            }
            catch (Exception error)
            {
                Fatal(ServiceError.Initialize, error);
                return Write(result, (ServiceError)State.Snapshot().Error);
            }
        }

        private static ServiceError ThreadReady(ulong session, out AndroidServicesStatus snapshot)
        {
            snapshot = State.Snapshot();
            if (session == 0 || snapshot.Session != session) return ServiceError.Argument;
            if (snapshot.Error != 0) return (ServiceError)snapshot.Error;
            if (snapshot.State != (uint)ServiceState.Ready) return ServiceError.Initialize;
            return ServiceError.Ok;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void AttachCurrentThread(ulong session)
        {
            // The reverse P/Invoke prologue attaches this native thread. Force
            // actual managed thread and execution-stack access on that thread;
            // another initialization worker cannot satisfy these checks for it.
            int thread = Thread.CurrentThread.ManagedThreadId;
            RuntimeHelpers.EnsureSufficientExecutionStack();
            if (thread <= 0 || (_attachedSession != 0 &&
                (_attachedSession != session || _attachedThreadId != thread)))
                throw new InvalidOperationException("Managed guest thread belongs to a different session or thread");
            _attachedSession = session;
            _attachedThreadId = thread;
        }

        [UnmanagedCallersOnly(EntryPoint = "s2a_android_services_request_stop")]
        public static int RequestStop(ulong session, AndroidServicesStatus* result)
        {
            if (!OutputValid(result)) return (int)ServiceError.Argument;
            ServiceError error = State.RequestStop(session);
            if (error == ServiceError.Ok && State.Snapshot().State != (uint)ServiceState.Stopped)
            {
                NotifyHostStop(0, "Android session stop requested");
                RequestServiceCancellation();
            }
            return Write(result, error); // Request only. Native owns guest quiescence.
        }

        private static bool OutputValid(AndroidServicesStatus* result) =>
            result != null && result->Size >= sizeof(AndroidServicesStatus) && result->Version == 1;

        private static void EnsureStarting()
        {
            if (State.Snapshot().State != (uint)ServiceState.Starting)
                throw new OperationCanceledException("Session stopped or failed before service initialization");
        }

        private static int Write(AndroidServicesStatus* result, ServiceError error)
        {
            *result = State.Snapshot();
            return (int)error;
        }

        private static IHardwareDeviceDriver CreateAudio() => new AAudioHardwareDeviceDriver();

        private static void ReadInput(ref GamepadInput input)
        {
            try
            {
                AndroidHostApi.InputState state = _host.ReadInput();
                input.Buttons = (ControllerKeys)state.Buttons;
                input.LStick.Dx = state.LeftX; input.LStick.Dy = state.LeftY;
                input.RStick.Dx = state.RightX; input.RStick.Dy = state.RightY;
            }
            catch (Exception error)
            {
                Fatal(ServiceError.Host, error);
                InputPump.Stop(); // On its own thread: prevents further invalid snapshots.
            }
        }

        private static void Fatal(ServiceError code, Exception error)
        {
            State.Fail(code);
            Console.Error.WriteLine(WithGpuFailureContext("Android session failed: " + error, error));
            NotifyHostStop(3, "Android runtime dependency failed");
            RequestServiceCancellation();
        }

        private static string WithGpuFailureContext(string message, Exception error)
        {
            // Reuse Fatal's existing single write. Reading optional metadata must not
            // replace the original exception or introduce another diagnostic sink.
            try
            {
                string marker = error.Data[nameof(AndroidVulkanSurface)] as string;
                string backing = error.Data[nameof(AndroidVulkanRuntime)] as string;
                return message + (marker == null ? string.Empty : Environment.NewLine + marker) +
                    (backing == null ? string.Empty : Environment.NewLine + backing);
            }
            catch { return message; }
        }

        private static void OnServiceFailure(Exception error) => Fatal(ServiceError.Initialize, error);

        // owner 循环的 poll 失败（Retirement/Driver/…）：按 GPU 阶段失败走同一条既有路径，
        // 只通知一次 HostApi stop 并请求取消；不在这里同步销毁 native 会话（StopGpu 统一收尾）。
        private static void OnDirectOwnerFailure(int code, string message)
        {
            Fatal(ServiceError.Gpu, new InvalidOperationException(message));
        }

        private static void NotifyHostStop(uint reason, string message)
        {
            AndroidHostApi host = Volatile.Read(ref _host);
            if (host != null && Interlocked.CompareExchange(ref _stopSent, 1, 0) == 0)
                host.RequestStop(reason, message);
        }

        private static void RequestServiceCancellation()
        {
            if (State.Snapshot().State == (uint)ServiceState.Stopped) return;
            try { Ryujinx.Common.S2AShutdown.Request(); }
            catch (Exception error)
            {
                // A service wake callback may fail. Still notify the native owner;
                // never let that managed exception cross the unmanaged stop entry.
                State.Fail(ServiceError.Cleanup);
                NotifyHostStop(3, "Android service cancellation failed");
                Console.Error.WriteLine("Android service cancellation failed: " + error);
            }
        }

        private static void StopGpu()
        {
            // 拆线程 auto 接过的出图间隔回调先解挂（metrics 的留给 Cleanup 原样处理），收局后 SurfaceFlinger 不再回调旧运行时
            if (_gpu is { WantsFrameInterval: true })
                Ryujinx.HLE.HOS.Services.SurfaceFlinger.S2AFramePacing.FrameIntervalChanged = _metrics != null ? _metrics.FrameInterval : null;
            try
            {
                if (_direct)
                {
                    int result = s2a_android_vk_session_stop();
                    if (result != 0) throw new InvalidOperationException("Direct Vulkan resources remain live: " + result);
                }
                else if (_gpu != null)
                {
                    _gpu.Stop();
                    S2AFrameTrace.Disable();
                    if (!Ryujinx.Common.S2ACompileEvents.EndSession())
                        Console.Error.WriteLine("【红】compile-events 未完整收尾，编译证据未证");
                    // t-direct-frametimes：generic 逐帧帧时间在 GPU 结算后转储。只在 generic GPU 真跑过时调：记录只来自它的
                    // AndroidWindow.Present（s2a_frame_log_present），没起 GPU 的收局没有记录可转，也不该依赖原生库
                    //（11-05 宿主关机单测无 s2a_native，无条件调用 → DllNotFoundException → Cleanup 失败、整进程 rc=134）。
                    // direct 已由 adapter shutdown 转过，不走这里。
                    s2a_frame_log_dump();
                }
            }
            catch (Exception error) { Fatal(ServiceError.Cleanup, error); throw; }
            // direct：先让 HLE/guest 停下来，再按契约 request_stop → owner 控制轮询 → destroy 收掉 native 会话。
            if (_directSession != null)
            {
                int stop = _directSession.RequestStop();
                // F4.4：owner 循环是 provider.poll 的唯一调用者，必须带预算收尾。退出不了（例如驱动
                // 调用卡住）就不能销毁它仍可能进入的 native context：保留上下文/设备，如实报红。
                bool ownerStopped = _directOwner == null || _directOwner.Stop(DirectOwnerLoop.DefaultStopBudgetMs);
                // Stop 失败（Retirement/Driver/…）不解散 native 会话：请求 native 保留上下文/device，
                // 进程退出回收。只有 Stop 明确成功且 owner 循环确实退出，才允许 Destroy。
                int destroy = DirectAbi.Stopped;
                if (stop == DirectAbi.Ok && ownerStopped) destroy = _directSession.Destroy();
                long slices = _directOwner?.Slices ?? 0;
                long pending = _directOwner?.PendingSlices ?? 0;
                Console.Error.WriteLine(
                    $"[s2a] direct session closed: stop={stop} destroy={destroy} ownerSlices={slices} ownerPending={pending}");
                if (!ownerStopped)
                    Console.Error.WriteLine(
                        "【红】direct owner loop did not stop within its budget; native session/destroy retained (process teardown)");
                // #860 / #907 这一片：前端停稳还没有证明手段，活跃会话停局一律保留到进程退出、返回 Retirement——这是既定的正常收局，
                // 不是故障，记普通信息；其余非 Ok（驱动 / 内部错误等）照旧报红。
                if (stop == DirectAbi.Retirement && ownerStopped)
                    Console.Error.WriteLine(
                        "[s2a] direct stop: session retained until process exit (frontend retirement not proven yet; #860)");
                else if (stop != DirectAbi.Ok)
                    Console.Error.WriteLine(
                        "【红】direct stop did not prove completion; native context retained, no normal-stop claim");
                _directOwner = null;
                _directSession = null;
            }
        }

        private static void ShutdownFromNative()
        {
            State.RequestStop(State.Snapshot().Session);
            try
            {
                Cleanup();
                State.CleanupComplete();
            }
            catch (Exception error) { Fatal(ServiceError.Cleanup, error); }
        }

        private static void Cleanup()
        {
            lock (CleanupLock)
            {
                if (_cleanupFailure != null)
                    throw new InvalidOperationException("Prior cleanup failed; process teardown is required", _cleanupFailure);
                if (_cleanupDone) return;
                try
                {
                    Ryujinx.Common.S2AShutdown.Request();
                    InputPump.Stop();
                    StopGpu(); // If this cannot quiesce, keep HLE memory and owner alive. generic 逐帧帧时间在 StopGpu 里转储
                    if (_metrics != null)
                    {
                        Ryujinx.HLE.HOS.Services.SurfaceFlinger.S2AFramePacing.FrameIntervalChanged = null;
                        _metrics.Dispose();
                    }
                    // 原生停机已 park Horizon 服务线程，其入口走不到 AudioUserIpcServer.Shutdown。
                    // 先停并 Join 音频 worker，再让 Switch.Dispose 关闭驱动的 pause event。
                    Host.DisposeAudioForShutdown();
                    Host.Device?.Dispose();
                    _cleanupDone = true;
                }
                catch (Exception error)
                {
                    _cleanupFailure = error;
                    throw;
                }
            }
        }

        private static RegionCode Region(string country) => country switch
        {
            "JP" => RegionCode.Japan,
            "CN" => RegionCode.China,
            "KR" => RegionCode.Korea,
            "TW" or "HK" or "MO" => RegionCode.Taiwan,
            "AU" or "NZ" => RegionCode.Australia,
            "AT" or "BE" or "BG" or "CH" or "CY" or "CZ" or "DE" or "DK" or "EE" or "ES" or
            "FI" or "FR" or "GB" or "GR" or "HR" or "HU" or "IE" or "IS" or "IT" or "LI" or
            "LT" or "LU" or "LV" or "MT" or "NL" or "NO" or "PL" or "PT" or "RO" or "SE" or
            "SI" or "SK" => RegionCode.Europe,
            _ => RegionCode.USA, // BootSpec permits an absent region; shared default remains USA.
        };
    }
}
