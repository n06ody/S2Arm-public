// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Nvdec assembly contract tests. Spies observe ownership only, not decoding.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Nvdec;
using Ryujinx.Graphics.Video;

internal static class VideoDecoderFactoryContractTests
{
    private static int _checks;
    private static readonly List<string> Errors = new();
    private static void Check(bool value, string name)
    {
        _checks++;
        if (!value) throw new InvalidOperationException(name);
    }

    // Reflection opens only the internal context; no private state is read/reset.
    private sealed class Context : IDisposable
    {
        private static readonly Type Type = typeof(S2AVideoDecoders).Assembly.GetType(
            "Ryujinx.Graphics.Nvdec.NvdecDecoderContext", throwOnError: true)!;
        private readonly object _value = Activator.CreateInstance(Type, nonPublic: true)!;
        public IH264Decoder H264() => (IH264Decoder)Type.GetMethod("GetH264Decoder")!.Invoke(_value, null)!;
        public IVp8Decoder Vp8() => (IVp8Decoder)Type.GetMethod("GetVp8Decoder")!.Invoke(_value, null)!;
        public object ManagedVp9() => Type.GetMethod("GetManagedVp9")!.Invoke(_value, null)!;
        public object? NativeVp9() => Type.GetMethod("GetVp9Native")!.Invoke(_value, null);
        public void Dispose() => ((IDisposable)_value).Dispose();
    }

    private sealed class Spy : IH264Decoder, IVp8Decoder
    {
        public int Disposals { get; private set; }
        public bool IsHardwareAccelerated => false;
        public ISurface CreateSurface(int width, int height) => throw new NotSupportedException("no video surface in factory test");
        public bool Decode(ref H264PictureInfo info, ISurface output, ReadOnlySpan<byte> data) => throw new NotSupportedException("no H264 decoding in factory test");
        public bool Decode(ref Vp8PictureInfo info, ISurface output, ReadOnlySpan<byte> data) => throw new NotSupportedException("no VP8 decoding in factory test");
        public void Dispose() => Disposals++;
    }

    private static void Registered()
    {
        var h = new List<Spy>();
        var v = new List<Spy>();
        S2AVideoDecoders.H264 = () => { var value = new Spy(); h.Add(value); return value; };
        S2AVideoDecoders.Vp8 = () => { var value = new Spy(); v.Add(value); return value; };
        using var first = new Context();
        using var second = new Context();
        Check(h.Count == 0 && v.Count == 0, "factories must be lazy");
        var h1 = first.H264();
        Check(h.Count == 1 && ReferenceEquals(h1, h[0]), "H264 registered factory result");
        Check(ReferenceEquals(h1, first.H264()) && h.Count == 1, "H264 cached per context");
        Check(v.Count == 0, "H264 getter must not create VP8");
        var v1 = first.Vp8();
        Check(v.Count == 1 && ReferenceEquals(v1, v[0]), "VP8 registered factory result");
        Check(ReferenceEquals(v1, first.Vp8()) && v.Count == 1, "VP8 cached per context");
        var h2 = second.H264();
        var v2 = second.Vp8();
        Check(h.Count == 2 && !ReferenceEquals(h1, h2) && ReferenceEquals(h2, h[1]), "H264 separate context ownership");
        Check(v.Count == 2 && !ReferenceEquals(v1, v2) && ReferenceEquals(v2, v[1]), "VP8 separate context ownership");
        first.Dispose();
        Check(h[0].Disposals == 1 && v[0].Disposals == 1, "created decoders disposed by first context");
        Check(h[1].Disposals == 0 && v[1].Disposals == 0, "second context survives first disposal");
        first.Dispose();
        Check(h[0].Disposals == 1 && v[0].Disposals == 1, "repeated context disposal must not repeat decoder disposal");
        second.Dispose();
        Check(h[1].Disposals == 1 && v[1].Disposals == 1, "second context owns its decoders");
        using var lazy = new Context();
        var h3 = (Spy)lazy.H264();
        lazy.Dispose();
        Check(h.Count == 3 && v.Count == 2 && h3.Disposals == 1, "dispose must not instantiate unused VP8");
        Check(Errors.Count == 0, "registered path has no red log");
    }

    private static int Count(string text) => Errors.Count(s => s.Contains(text, StringComparison.Ordinal));

    private static void Fallback(Context context, int instances)
    {
        var h = context.H264();
        var v = context.Vp8();
        Check(ReferenceEquals(h, context.H264()), "H264 fallback cached");
        Check(ReferenceEquals(v, context.Vp8()), "VP8 fallback cached");
        using var hSurface = h.CreateSurface(17, 11);
        using var vSurface = v.CreateSurface(17, 11);
        Check(hSurface.Width == 17 && hSurface.Height == 11 && hSurface.UvWidth == 9 && hSurface.UvHeight == 6,
            "unavailable H264 surface preserves original chroma dimensions");
        Check(vSurface.Width == 17 && vSurface.Height == 11 && vSurface.UvWidth == 9 && vSurface.UvHeight == 6,
            "unavailable VP8 surface preserves original chroma dimensions");
        var hi = default(H264PictureInfo);
        var vi = default(Vp8PictureInfo);
        // Unavailable implementations must reject before touching a surface/bitstream.
        for (int i = 0; i < 3; i++)
        {
            Check(!h.Decode(ref hi, null!, ReadOnlySpan<byte>.Empty), "unavailable H264 Decode false");
            Check(!v.Decode(ref vi, null!, ReadOnlySpan<byte>.Empty), "unavailable VP8 Decode false");
        }
        Check(Count("Nvdec H264 backend=unavailable") == instances, "H264 unavailable log once per decoder instance");
        Check(Count("Nvdec VP8 backend=unavailable") == instances, "VP8 unavailable log once per decoder instance");
    }

    private static void Vp9Managed(Context context, string reason)
    {
        // VP9 没接原生路径（没登记 / 工厂失败 / 模式 managed）时落到进程内托管解码器
        // Ryujinx.Graphics.Nvdec.Vp9.Decoder——与接槽之前同一条路径，不产红日志。
        var n = context.ManagedVp9();
        Check(n != null && n.GetType().FullName == "Ryujinx.Graphics.Nvdec.Vp9.Decoder",
            $"VP9 fallback is the managed decoder ({reason})");
        Check(ReferenceEquals(n, context.ManagedVp9()), $"VP9 managed decoder cached per context ({reason})");
        Check(context.NativeVp9() == null, $"native path stays null ({reason})");
    }

    private static void Unregistered()
    {
        S2AVideoDecoders.H264 = null;
        S2AVideoDecoders.Vp8 = null;
        S2AVideoDecoders.Vp9 = null;
        S2AVideoDecoders.Vp9DefaultMode = "native"; // 原生模式开着也没后端：照样全走托管
        using var first = new Context();
        Fallback(first, 1);
        Vp9Managed(first, "unregistered");
        using var second = new Context();
        Fallback(second, 2);
        Vp9Managed(second, "unregistered");
        Check(Errors.Count == 4, "unregistered two contexts have four unavailable diagnostics");
    }

    private static void AndroidRegistered()
    {
        var platform = typeof(VideoDecoderFactoryContractTests).Assembly;
        var registration = platform.GetType("S2A.Android.Video.AndroidVideo");
        Check(registration != null, "AndroidVideo.Register entry must exist in platform assembly");
        registration!.GetMethod("Register", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);
        using var context = new Context();
        var h = context.H264(); var v = context.Vp8();
        Check(h.GetType().FullName == "Ryujinx.Graphics.Nvdec.Android.AvcH264Decoder", "H264 registered to migrated AvcH264Decoder");
        Check(v.GetType().FullName == "Ryujinx.Graphics.Nvdec.Android.AndroidVp8Decoder", "VP8 registered to migrated AndroidVp8Decoder");
        Check(h.GetType().Assembly == platform && v.GetType().Assembly == platform, "both decoders live in platform assembly");
        Check(h.GetType().Assembly != typeof(S2AVideoDecoders).Assembly, "decoders are outside shared Nvdec assembly");
        Check(ReferenceEquals(h, context.H264()) && ReferenceEquals(v, context.Vp8()), "registered decoders cached per context");
        // VP9: the libvpx backend with S2Arm's NVDEC hook, on by default (docs/设计_原生VP9/design_phase2_B.md); not created here,
        // because creating it is free but decoding needs the native library, which this host test does not load.
        Check(S2AVideoDecoders.Vp9 != null, "VP9 native backend registered");
        Check(S2AVideoDecoders.Vp9DefaultMode == "native", "VP9 native by default on Android");
        // debug.s2a.video_hw 在真机上开、probe 过才有 AMediaCodec；本宿主两者皆无，
        // 工厂必须落到 libvpx 后端——创建本身免费（原生库只到解码才用）。
        var backend = S2AVideoDecoders.Vp9!();
        Check(backend?.GetType().FullName == "Ryujinx.Graphics.Nvdec.Vpx.VpxVp9Backend",
            "host without hw prop/codec: factory falls back to libvpx backend");
        backend?.Dispose();
        Check(Errors.Count == 0, "android-registered path has no red log");
    }

    private static void Throwing()
    {
        int h = 0, v = 0, n = 0;
        S2AVideoDecoders.H264 = () => { h++; throw new InvalidOperationException("synthetic H264 factory failure"); };
        S2AVideoDecoders.Vp8 = () => { v++; throw new InvalidOperationException("synthetic VP8 factory failure"); };
        S2AVideoDecoders.Vp9 = () => { n++; throw new InvalidOperationException("synthetic VP9 factory failure"); };
        S2AVideoDecoders.Vp9DefaultMode = "native"; // 原生模式开着工厂才会被调用
        using var first = new Context();
        Fallback(first, 1);
        Vp9Managed(first, "throwing");
        Check(h == 1 && v == 1 && n == 1, "throwing factories invoked once in first context");
        using var second = new Context();
        Fallback(second, 2);
        Vp9Managed(second, "throwing");
        Check(h == 2 && v == 2 && n == 2, "new context retries each factory once");
        Check(Count("H.264 解码器工厂抛异常") == 1, "H264 factory diagnostic once per process");
        Check(Count("VP8 解码器工厂抛异常") == 1, "VP8 factory diagnostic once per process");
        Check(Count("VP9 解码器工厂抛异常") == 1, "VP9 factory diagnostic once per process");
        Check(Errors.Count == 7, "three factory errors plus four fallback diagnostics");
    }

    private static int Main(string[] args)
    {
        // Current Android fallback is native; that platform is not this host-only proof.
        if (OperatingSystem.IsAndroid() || OperatingSystem.IsIOS() ||
            RuntimeInformation.RuntimeIdentifier.StartsWith("linux-bionic", StringComparison.Ordinal))
        {
            Console.Error.WriteLine("UNSUPPORTED: desktop host contract test, not an Android/iOS run");
            return 75;
        }
        var oldH264 = S2AVideoDecoders.H264;
        var oldVp8 = S2AVideoDecoders.Vp8;
        var oldVp9 = S2AVideoDecoders.Vp9;
        var oldVp9Mode = S2AVideoDecoders.Vp9DefaultMode;
        EventHandler<LogEventArgs> capture = (_, e) => { if (e.Level == LogLevel.Error) Errors.Add(e.Message); };
        Logger.SetEnable(LogLevel.Error, true);
        Logger.SetEnable(LogClass.Nvdec, true);
        Logger.SetConsoleTargetEnabled(false);
        Logger.Updated += capture;
        try
        {
            if (args.Length != 1) throw new ArgumentException("expected registered/unregistered/throwing");
            switch (args[0])
            {
                case "registered": Registered(); break;
                case "unregistered": Unregistered(); break;
                case "throwing": Throwing(); break;
                case "android-registered": AndroidRegistered(); break;
                default: throw new ArgumentException("unknown case");
            }
            Console.WriteLine($"PASS {args[0]}: {_checks} checks; actual Nvdec assembly={typeof(S2AVideoDecoders).Assembly.GetName().Name}");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"FAIL: {error}");
            return 1;
        }
        finally
        {
            S2AVideoDecoders.H264 = oldH264;
            S2AVideoDecoders.Vp8 = oldVp8;
            S2AVideoDecoders.Vp9 = oldVp9;
            S2AVideoDecoders.Vp9DefaultMode = oldVp9Mode;
            Logger.Updated -= capture;
        }
    }
}
