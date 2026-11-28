using Ryujinx.Cpu;
using Ryujinx.Memory;
using Ryujinx.Memory.Range;
using Ryujinx.Memory.Tracking;
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace S2A.Services
{
    // 游戏内存的 Ryujinx 视图：游戏地址 = 宿主地址（方案 §3-3，接口 §4.5），原地读写、零复制。
    // 例外是远端（runtime/src/far.c，乙布局一律开）：别名区伸出宿主窗口的那段游戏地址没有宿主映射、内容在影子区。访问入口按原生层的目录
    //（接口 v2.13 s2a_far_desc）换成影子地址，不再靠缺页代执行兜底；跟踪事件与写跟踪钩子仍用游戏地址。
    // 映射 / 解映射归原生层，这里不做；写跟踪的数据结构用 Ryujinx 自己的 MemoryTracking，
    // 「让游戏写触发事件」的保护动作（TrackingReprotect）交给帧率能耗线的 16 KB 页精确跟踪，经 ReprotectHook 挂入，未挂时不保护。
    public sealed unsafe class GuestMemory : IVirtualMemoryManagerTracked
    {
        public const int PageSize = 0x1000;

        // 帧率能耗线挂：(地址, 长度, 保护, 是否游戏侧) → 设页保护
        public static delegate*<ulong, ulong, MemoryPermission, bool, void> ReprotectHook;

        // 帧率能耗线挂（NxTrack.WriteUntracked）：不经跟踪的写由跟踪模块代为复制（GPU 回写 flush=true，信号量 / 报告值 flush=false）
        public static delegate*<ulong, ReadOnlySpan<byte>, bool, void> WriteUntrackedHook;

        // 托管代码直接写游戏内存之前调（帧率能耗线 NxTrack.Write）：页若被游戏侧写跟踪保护为只读，先按 CPU 写解开并记脏，
        // 否则托管写会在 C# 里缺页。只剩调用方拿到引用 / 区域后自己写的两处用它：GetRef、GetWritableRegion(tracked)；不跟踪的 GetWritableRegion 走 WriteQuietHook。
        // Write / Write<T> / WriteWithRedundancyCheck 已同步发过跟踪事件，内容交 WriteUntrackedHook 代写（不再由写跟踪在下次排空时当 CPU 写重报）。
        // GPU 自己的不跟踪写（贴图回写、清缓冲）不经这里：Ryujinx PhysicalMemory 给副本、Dispose 时经 WriteUntracked 写回（补丁 0110）
        public static delegate*<ulong, ulong, void> WriteHook;

        // 不跟踪托管写（GetWritableRegion(tracked: false)：服务写 IPC 缓冲等，多数调用方不 Dispose、直接当游戏内存视图用）真正写之前调
        //（帧率能耗线 NxTrack.WriteQuiet）：只解开只读页，不触发「GPU 改过的页先回写」——原版不跟踪写不发跟踪事件；
        // 双线程渲染时回写在渲染线程上执行，再触发会与等这次回写的 GPU 模拟线程死锁
        public static delegate*<ulong, ulong, void> WriteQuietHook;

        // Optional direct owner for guest addresses which escape to host code.
        // The notification precedes exposure of a naked pointer or span.
        public static delegate* unmanaged<ulong, ulong, int, void> HostAddressHook;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static void PinHostAddress(ulong address, ulong size, bool write)
        {
            var pin = HostAddressHook;
            if (pin != null && size != 0)
            {
                pin(address, size, write ? 1 : 0);
            }
        }

        // 帧率能耗线挂（NxTrack.GpuModified）：[va, va+size) 涉及的 4 KB 子页里有没有 GPU 改过、还没回写的（Ryujinx 设了 None 保护）
        public static delegate*<ulong, ulong, bool> GpuModifiedHook;

        // 收口回写（#199 歧路旅人 II 崩溃线）：不跟踪的托管读碰 GPU 改过、还没回写的子页前，发与客机缺页同一个读
        // 跟踪事件（MemoryTracking.S2AReadFlush：句柄回写动作在本线程跑完、页恢复可读再取数）。信号侧不接管：
        // NativeAOT 托管缺页处在协作上下文，回写要走托管跟踪层，不能在信号里做。失败原样上抛：不填零、不吞错、不给旧字节。
        // Mac 没挂 GpuModifiedHook、或子页没标过时零代价直读，行为逐字节不变
        private static long _readFlush;

        private void ReadFlushIfGpuModified(ulong va, ulong size)
        {
            if (GpuModifiedHook == null || size == 0 || !GpuModifiedHook(va, size))
            {
                return;
            }

            Tracking.S2AReadFlush(va, size);

            long n = Interlocked.Increment(ref _readFlush);
            if (n <= 3 || (n & (n - 1)) == 0)
            {
                Console.Error.WriteLine($"收口回写 {n} 次");
            }
        }

        // 确定性档（原生层 s2a_det_mode，起局读一次）：开着时 C# 往游戏内存的写一律经 s2a_det_host_write（记录档按序进日志、重放档按日志写），
        // 关着时照旧直接写——只多一个分支
        internal static bool Det;

        public static void InitDet()
        {
            Det = Native.DetMode != null && Native.DetMode() != 0;

            if (Det)
            {
                Console.Error.WriteLine("S2A 确定性档：C# 写游戏内存经 s2a_det_host_write（写跟踪的 WriteUntracked 挂钩此时不走）");
            }
        }

        private static void DetWrite(ulong va, ReadOnlySpan<byte> data)
        {
            fixed (byte* p = data)
            {
                Native.DetHostWrite(va, p, (ulong)data.Length);
            }
        }

        // 副本缓冲：每线程 16 块原生缓冲轮转（覆盖嵌套使用，同区域环），只增不减。
        //   确定性档、远端读副本与区域副本各用一套；完整退休后回收已退出线程的原生缓冲。
        private sealed class Scratch
        {
            public const int Count = 16;
            private readonly byte*[] _buf = new byte*[Count];
            private readonly int[] _cap = new int[Count];
            private int _next;

            public void Dispose()
            {
                for (int i = 0; i < Count; i++) { NativeMemory.Free(_buf[i]); _buf[i] = null; }
            }

            public byte* Take(int size)
            {
                int k = _next++ & (Count - 1);

                if (_cap[k] < size)
                {
                    // 先分配、成功了再换上并放旧块：分配抛内存不足时槽里仍是旧块和旧容量，不留悬空指针（审计 #814 F-scratch-uaf：
                    //   原先先放后分，抛错后槽指向已放的块、容量还是旧值，线程还在，下一次取不超旧容量就直接拿到它）
                    byte* nb = (byte*)NativeMemory.Alloc((nuint)size);
                    NativeMemory.Free(_buf[k]);
                    _buf[k] = nb;
                    _cap[k] = size;
                }

                return _buf[k];
            }
        }

        private static ThreadLocal<Scratch> s_det = new(() => new Scratch(), trackAllValues: true);
        private static ThreadLocal<Scratch> s_farRead = new(() => new Scratch(), trackAllValues: true);
        private static ThreadLocal<Scratch> s_farRegion = new(() => new Scratch(), trackAllValues: true);

        // 仅在 guest、服务和 GPU 全部退休后调用。静态访存入口与实例共用这组逐局缓冲。
        internal static void ResetSession()
        {
            foreach (ThreadLocal<Scratch> pool in new[] { s_det, s_farRead, s_farRegion })
            {
                foreach (Scratch scratch in pool.Values) scratch.Dispose();
                pool.Dispose();
            }
            s_det = new(() => new Scratch(), trackAllValues: true);
            s_farRead = new(() => new Scratch(), trackAllValues: true);
            s_farRegion = new(() => new Scratch(), trackAllValues: true);
            s_regions.Dispose();
            s_regions = new(() => new Regions());
            t_preciseVa = 0;
            t_preciseLen = 0;
        }

        // 确定性档 GetWritableRegion 副本的写回目标（跟踪已在取区域时报过，这里只写）
        private sealed class DetBlock : IWritableBlock
        {
            public static readonly DetBlock Instance = new();

            public void Write(ulong va, ReadOnlySpan<byte> data) => DetWrite(va, data);
        }

        private static void BeforeWrite(ulong va, ulong size)
        {
            if (WriteHook != null)
            {
                WriteHook(va, size);
            }
        }

        // ---- 远端翻译（接口 v2.13，runtime/src/far.c）----
        // 目录覆盖远端起点往上 2^36（64 GiB），2 MiB 一块；块内「影子 − 游戏」相同，相邻块的影子不保证相邻
        private const int FarShift = 21;
        private const ulong FarChunk = 1UL << FarShift;
        private const ulong FarSpan = 1UL << 36;

        // 目录（null = 远端没开）。每次访问只读一次：原生层布局时写一次（早于 S2A_Init）、此后不变
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static long* FarDir() => (long*)Volatile.Read(ref Native.Far->Dir);

        // 从 a 起最长 n 字节的一段（同 far.c piece）：回段长，host = 这段的宿主地址。远端之下 / 之上原样；远端内到块尾为止、加该块差值
        //（差值 0 = 这块没映射：原样，碰到即缺页走原生层兜底 / 报红，同修前）
        // 音频 DSP 在实时线程上读游戏挂给音频渲染器的内存池（采样数据）。客体内存走文件后备时（8 GB Mac 缺省），内存吃紧系统先赶文件页，
        //   池里久没播的音效被赶出去，下次播时 DSP 在实时线程上从盘读回、单拍卡几百毫秒。池挂着期间锁进物理内存（mlock），摘下时解锁（按段，远端分块逐块）；
        //   锁不上（超系统上限等）照常用、记一行。Ryujinx.Audio 在池映射 / 解映射时调（S2AAudioPoolResident.Hook，服务层启动时挂）
        [DllImport("libc", SetLastError = true)]
        private static extern int mlock(void* addr, nuint len);

        [DllImport("libc", SetLastError = true)]
        private static extern int munlock(void* addr, nuint len);

        public static void AudioPoolResident(ulong va, ulong size, bool on)
        {
            PinHostAddress(va, size, false);
            long* dir = FarDir();
            ulong lo = dir != null ? Native.Far->Lo : 0;
            int err = 0;

            for (ulong a = va, left = size; left != 0;)
            {
                ulong h = a, k = dir != null ? FarPiece(dir, lo, a, left, out h) : left;

                if (k == 0)
                {
                    break;
                }

                if ((on ? mlock((void*)h, (nuint)k) : munlock((void*)h, (nuint)k)) != 0 && err == 0)
                {
                    err = Marshal.GetLastPInvokeError();
                }

                a += k;
                left -= k;
            }

            if (on)
            {
                Console.Error.WriteLine(err == 0
                    ? $"[声] 音频内存池 {size / 1024} KB 已锁进物理内存（实时音频线程读采样不会因内存吃紧从盘读回）"
                    : $"[声] 【黄】音频内存池 {size / 1024} KB 锁不进物理内存（errno {err}），照常用；内存吃紧时音频可能断续");
            }
        }

        private static ulong FarPiece(long* dir, ulong lo, ulong a, ulong n, out ulong host)
        {
            ulong off = a - lo;

            if (off >= FarSpan)
            {
                host = a;
                return a < lo && lo - a < n ? lo - a : n;
            }

            ulong k = FarChunk - (off & (FarChunk - 1));
            host = a + (ulong)Volatile.Read(ref dir[off >> FarShift]);
            return k < n ? k : n;
        }

        // [va, va+size) 在宿主上是一整段连续的（远端没开 / 不碰远端 / 碰到的块差值都相同）：回 true，host = 起点的宿主地址。远端没开只多一次判空
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static bool Contiguous(ulong va, ulong size, out ulong host)
        {
            long* dir = FarDir();

            if (dir == null)
            {
                host = va;
                return true;
            }

            return ContiguousFar(dir, va, size, out host);
        }

        private static bool ContiguousFar(long* dir, ulong va, ulong size, out ulong host)
        {
            ulong lo = Native.Far->Lo;
            ulong k = FarPiece(dir, lo, va, size, out host);

            for (ulong a = va + k, left = size - k; left != 0; a += k, left -= k)
            {
                k = FarPiece(dir, lo, a, left, out ulong h);

                if (h - a != host - va)
                {
                    return false;
                }
            }

            return true;
        }

        // 整段不需要换算（宿主地址 = 游戏地址）：写跟踪代写只认这种（它按传入地址自己复制，远端游戏地址是不可访问的围栏）
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static bool Near(ulong va, ulong size) => Contiguous(va, size, out ulong h) && h == va;

        // 游戏内存 [va, va + dst.Length) → dst，按块分段
        private static void CopyOut(ulong va, Span<byte> dst)
        {
            long* dir = FarDir();

            if (dir == null)
            {
                new ReadOnlySpan<byte>((void*)va, dst.Length).CopyTo(dst);
                return;
            }

            ulong lo = Native.Far->Lo;

            for (int done = 0; done < dst.Length;)
            {
                int k = (int)FarPiece(dir, lo, va + (ulong)done, (ulong)(dst.Length - done), out ulong h);
                new ReadOnlySpan<byte>((void*)h, k).CopyTo(dst.Slice(done, k));
                done += k;
            }
        }

        // src → 游戏内存 [va, va + src.Length)，按块分段；直接写，不经钩子（调用方已按游戏地址报过）
        private static void CopyIn(ulong va, ReadOnlySpan<byte> src)
        {
            long* dir = FarDir();

            if (dir == null)
            {
                src.CopyTo(new Span<byte>((void*)va, src.Length));
                return;
            }

            ulong lo = Native.Far->Lo;

            for (int done = 0; done < src.Length;)
            {
                int k = (int)FarPiece(dir, lo, va + (ulong)done, (ulong)(src.Length - done), out ulong h);
                src.Slice(done, k).CopyTo(new Span<byte>((void*)h, k));
                done += k;
            }
        }

        // 游戏内存 [va, va + data.Length) 与 data 逐字节相同
        private static bool GuestEquals(ulong va, ReadOnlySpan<byte> data)
        {
            if (Contiguous(va, (ulong)data.Length, out ulong h))
            {
                return new ReadOnlySpan<byte>((void*)h, data.Length).SequenceEqual(data);
            }

            long* dir = FarDir();
            ulong lo = Native.Far->Lo;

            for (int done = 0; done < data.Length;)
            {
                int k = (int)FarPiece(dir, lo, va + (ulong)done, (ulong)(data.Length - done), out ulong p);

                if (!new ReadOnlySpan<byte>((void*)p, k).SequenceEqual(data.Slice(done, k)))
                {
                    return false;
                }

                done += k;
            }

            return true;
        }

        // 跨块且影子不相邻时的只读副本（每线程 16 块轮转，同区域环：调用方在同一线程再取 16 次之前用完）
        private static byte* FarCopy(ulong va, int size)
        {
            byte* buf = s_farRead.Value.Take(size);
            CopyOut(va, new Span<byte>(buf, size));
            return buf;
        }

        // 远端跨块区域副本的写回目标：分段直接写进影子（同修前「区域就是游戏内存」：跟踪事件 / 写前钩子已在取区域时按游戏地址报过，这里只写）
        private sealed class FarBlock : IWritableBlock
        {
            public static readonly FarBlock Instance = new();

            public void Write(ulong va, ReadOnlySpan<byte> data) => CopyIn(va, data);
        }

        public MemoryTracking Tracking { get; }

        public GuestMemory()
        {
            Tracking = new MemoryTracking(this, PageSize);
        }

        public bool UsesPrivateAllocations => false;

        public void Map(ulong va, ulong pa, ulong size, MemoryMapFlags flags) => throw new NotSupportedException("S2Arm：游戏地址空间归原生层");
        public void MapForeign(ulong va, nuint hostPointer, ulong size) => throw new NotSupportedException("S2Arm：游戏地址空间归原生层");
        public void Unmap(ulong va, ulong size) => throw new NotSupportedException("S2Arm：游戏地址空间归原生层");
        public void Reprotect(ulong va, ulong size, MemoryPermission protection) => throw new NotSupportedException("S2Arm：页权限归原生层");

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public T Read<T>(ulong va) where T : unmanaged
        {
            PinHostAddress(va, (ulong)sizeof(T), false);
            ReadFlushIfGpuModified(va, (ulong)sizeof(T));

            if (Contiguous(va, (ulong)sizeof(T), out ulong h))
            {
                return Unsafe.ReadUnaligned<T>((void*)h);
            }

            T value = default;
            CopyOut(va, new Span<byte>(&value, sizeof(T)));
            return value;
        }

        public T ReadTracked<T>(ulong va) where T : unmanaged
        {
            SignalMemoryTracking(va, (ulong)sizeof(T), false);
            return Read<T>(va);
        }

        public void Read(ulong va, Span<byte> data)
        {
            PinHostAddress(va, (ulong)data.Length, false);
            ReadFlushIfGpuModified(va, (ulong)data.Length);
            CopyOut(va, data);
        }

        public bool TryReadUnsafe(ulong va, int length, out Span<byte> data)
        {
            if (length > 0)
            {
                PinHostAddress(va, (ulong)length, true);
                ReadFlushIfGpuModified(va, (ulong)length);
            }

            if (Contiguous(va, (ulong)length, out ulong h))
            {
                data = new Span<byte>((void*)h, length);
                return true;
            }

            // 远端跨块且两块影子不相邻：给不出一段连续视图，调用方按 Ryujinx 约定退回 Read 拷贝（INvDrvServices）
            data = default;
            return false;
        }

        public void Write<T>(ulong va, T value) where T : unmanaged
        {
            PinHostAddress(va, (ulong)sizeof(T), true);
            SignalManagedWrite(va, (ulong)sizeof(T));
            WriteSignalled(va, new ReadOnlySpan<byte>(&value, sizeof(T)));
        }

        public void Write(ulong va, ReadOnlySpan<byte> data)
        {
            if (data.Length == 0)
            {
                return;
            }

            PinHostAddress(va, (ulong)data.Length, true);
            SignalManagedWrite(va, (ulong)data.Length);
            WriteSignalled(va, data);
        }

        // 托管写的跟踪事件发到游戏侧区域（guest: true）：我们只有一份真实映射，托管写与游戏写落在同一份页上。
        // 发到这一域，句柄记脏后 Ryujinx 按游戏侧区域改保护（TrackingReprotect guest: true → 写跟踪放掉该子页的保护 / 观察），
        // 同步后再按这一域重设——写跟踪的保护意图始终与句柄状态一致。若发到普通域（原版托管访存的域），句柄照样记脏，
        // 但游戏侧区域的保护不变，写跟踪仍按要保护 / 观察处理这个子页，紧跟的每笔写都要更新基线（10-21 暗黑3 常量缓冲内联更新）。
        // 两域区域按同一地址范围建（非单字节跟踪），覆盖的句柄相同
        private void SignalManagedWrite(ulong va, ulong size)
        {
            Tracking.VirtualMemoryEvent(va, size, true, precise: false, exemptId: null, guest: true);
        }

        // 跟踪事件已同步发给 Ryujinx 之后的写（照原版：托管写先 SignalMemoryTracking 再写，写本身不再被游戏侧页保护当 CPU 写捕获）：
        // 有写跟踪时由它代写（flush = false：解开只读页、写完的内容当基线，不在下次排空时重报）。
        // 修前经 WriteHook 记成 CPU 写、下次排空再报一遍：其间 Ryujinx 若给同一 4 KB 挂了回写动作（GPU 改过同页别的字节），这次重报就触发一次等 GPU 的回写
        //（10-21 怪猎：常量缓冲内联更新 FlushUboDirty 与着色器写的存储缓冲同页，每帧多次）；重报晚于同批 GPU 命令，还会把 GPU 之后写的字节盖回
        private static void WriteSignalled(ulong va, ReadOnlySpan<byte> data)
        {
            if (Det)
            {
                BeforeWrite(va, (ulong)data.Length);
                DetWrite(va, data);
                return;
            }

            // 写跟踪代写按传入地址自己复制：只交给它不碰远端的段；碰远端的直接分段写影子（远端游戏地址是不可访问的围栏）
            if (WriteUntrackedHook != null && Near(va, (ulong)data.Length))
            {
                WriteUntrackedHook(va, data, false);
                return;
            }

            CopyIn(va, data);
        }

        // GPU 自己写客体内存（信号量 / 报告值 / DMA / 内联上传）走 Ryujinx PhysicalMemory.WriteTrackedResource：
        // 先 SignalMemoryTracking(precise, write) 再对同一区间 WriteUntracked —— 这一次传 flush=false（不是回写，不许把
        // 游戏未报的字节放回旧值，否则信号量值会丢）；其余 WriteUntracked 是缓冲 / 贴图 Flush 回写，传 flush=true
        [ThreadStatic] private static ulong t_preciseVa;
        [ThreadStatic] private static int t_preciseLen;

        public void WriteUntracked(ulong va, ReadOnlySpan<byte> data) => WriteUntrackedStatic(va, data);

        private static void WriteUntrackedStatic(ulong va, ReadOnlySpan<byte> data)
        {
            PinHostAddress(va, (ulong)data.Length, true);
            bool flush = !(t_preciseLen != 0 && va == t_preciseVa && data.Length == t_preciseLen);
            t_preciseLen = 0;

            if (Det)
            {
                DetWrite(va, data);
                return;
            }

            // 同 WriteSignalled：碰远端的段不交给写跟踪代写，直接分段写影子
            if (WriteUntrackedHook != null && Near(va, (ulong)data.Length))
            {
                WriteUntrackedHook(va, data, flush);
                return;
            }

            CopyIn(va, data);
        }

        // 原版（MemoryManagerHostTracked）：先按读发跟踪事件——同 4 KB 有 GPU 改过、还没回写的数据就先等 GPU 回写，再比、变了才写，调用方（常量缓冲内联更新）对变了的区间 ForceDirty。
        // 这里：区间涉及的子页没有 GPU 改过的数据时，读事件本就什么都不做，照原版比、变了才写（发普通写事件，见下）；
        // 有时不回写，改发精确写事件——缓冲按字节把这段从「GPU 改过」里打穿并记脏（Ryujinx Buffer.PreciseAction，与先回写再写结果逐字节相同：
        // 区间内以新值为准、区间外 GPU 数据留在 GPU 侧），贴图等没有精确动作的句柄照旧回写；返回 true（客体内存旧字节可能是回写前的，不能拿来比）。
        // 10-21 怪猎：着色器写的计数 / 存储缓冲与常量缓冲同页，按原版每次内联更新都等 GPU 回写（Mac 局读回的近半数）
        public bool WriteWithRedundancyCheck(ulong va, ReadOnlySpan<byte> data)
        {
            if (data.Length == 0)
            {
                return false;
            }

            if (GpuModifiedHook == null || !GpuModifiedHook(va, (ulong)data.Length))
            {
                PinHostAddress(va, (ulong)data.Length, true);
                if (GuestEquals(va, data))
                {
                    return false;
                }

                // 常态（没有 GPU 改过的数据，没有回写动作可触发）：普通写事件发到游戏侧区域——句柄记脏、Ryujinx 把该子页改成不跟踪，写跟踪随之放掉保护 / 观察，
                // 同子页紧跟的内联更新都是直写级的代价。若发精确写（句柄不改保护）或发到普通域（游戏侧区域保护不变），子页一直观察中，每笔都要更新基线
                //（10-21 暗黑3 GPU 模拟线程写跟踪 2.3 秒 / 20 秒，增量基线后 0.55 秒，普通域写事件 0.83 秒）
                SignalManagedWrite(va, (ulong)data.Length);
                WriteSignalled(va, data);
                return true;
            }

            SignalMemoryTracking(va, (ulong)data.Length, true, precise: true);
            WriteUntrackedStatic(va, data);
            return true;
        }

        public ReadOnlySequence<byte> GetReadOnlySequence(ulong va, int size, bool tracked = false)
        {
            PinHostAddress(va, (ulong)size, false);
            if (tracked)
            {
                SignalMemoryTracking(va, (ulong)size, false);
            }
            else
            {
                ReadFlushIfGpuModified(va, (ulong)size);
            }

            // 只在本次请求内读（Horizon 输入缓冲等）：同区域一样按线程轮转复用，稳态零分配（原每次新建 NativeMemoryManager）
            Regions r = s_regions.Value;
            PointerMemory m = r.SeqManagers[r.NextSeq++ & (Regions.Count - 1)];
            // 远端跨块且两块影子不相邻：给一份只读副本（每线程轮转）
            m.Reset(Contiguous(va, (ulong)size, out ulong h) ? (byte*)h : FarCopy(va, size), size);
            return new ReadOnlySequence<byte>(m.Memory);
        }

        public ReadOnlySpan<byte> GetSpan(ulong va, int size, bool tracked = false)
        {
            PinHostAddress(va, (ulong)size, false);
            if (tracked)
            {
                SignalMemoryTracking(va, (ulong)size, false);
            }
            else
            {
                ReadFlushIfGpuModified(va, (ulong)size);
            }

            // 远端跨块且两块影子不相邻：给一份只读副本（每线程轮转）
            return Contiguous(va, (ulong)size, out ulong h) ? new ReadOnlySpan<byte>((void*)h, size) : new ReadOnlySpan<byte>(FarCopy(va, size), size);
        }

        public WritableRegion GetWritableRegion(ulong va, int size, bool tracked = false)
        {
            PinHostAddress(va, (ulong)size, true);
            if (tracked)
            {
                SignalMemoryTracking(va, (ulong)size, true);
                BeforeWrite(va, (ulong)size);
            }
            else
            {
                // 区域副本按块读回写后的游戏内存：先收口回写再解保护（不跟踪写不发事件，照旧 WriteQuietHook）
                ReadFlushIfGpuModified(va, (ulong)size);

                if (WriteQuietHook != null)
                {
                    WriteQuietHook(va, (ulong)size);
                }
            }

            // 确定性档：给一份副本，Dispose 时经 WriteUntracked → s2a_det_host_write 写回（不让调用方直接写游戏内存）
            //   副本放每线程 16 块轮转复用的原生缓冲（按需加大），不按区域长度建托管数组（10-20 普查：大读在 iPhone 低区地址上 OOM）
            if (Det)
            {
                //   写回仍按游戏地址交 s2a_det_host_write（日志按游戏地址记 / 重放）；读副本按块换算
                byte* buf = s_det.Value.Take(size);
                CopyOut(va, new Span<byte>(buf, size));
                return new WritableRegion(DetBlock.Instance, va, new NativeMemoryManager<byte>(buf, size).Memory);
            }

            // 游戏内存可直接寻址，区域不需要回写：每线程 16 个区域轮转复用（覆盖嵌套使用），稳态零分配
            Regions r = s_regions.Value;

            if (!Contiguous(va, (ulong)size, out ulong h))
            {
                // 远端跨块且两块影子不相邻：给副本，Dispose 时经 FarBlock 分段写回影子（区域与副本同样每线程 16 个轮转）。
                //   跟踪事件 / 写前钩子上面已按游戏地址报过；不 Dispose、把区域当游戏内存视图直接写的调用方，这种区域的写会丢
                int j = r.NextFar++ & (Regions.Count - 1);
                byte* buf = s_farRegion.Value.Take(size);
                CopyOut(va, new Span<byte>(buf, size));
                r.FarManagers[j].Reset(buf, size);
                r.FarItems[j].S2AReuse(FarBlock.Instance, va, r.FarManagers[j].Memory);
                return r.FarItems[j];
            }

            int i = r.Next++ & (Regions.Count - 1);
            r.Managers[i].Reset((byte*)h, size);
            r.Va[i] = va;
            r.Items[i].S2AReuse(r.Managers[i].Memory);
            return r.Items[i];
        }

        // 视图指针 → 游戏地址（11-01 审计 P1-17）：[host, host+size) 落在本线程给出的哪个区域视图里，按那块区域取时的游戏地址换算。
        //   远端连续块的视图是影子页，指针不是游戏地址；写钩子只收游戏地址。不在任何区域里：远端没开时宿主地址就是游戏地址，开着则给不出（回 false）
        internal static bool TryGuestAddress(ulong host, ulong size, out ulong va)
        {
            Regions r = s_regions.IsValueCreated ? s_regions.Value : null;

            for (int i = 0; r != null && i < Regions.Count; i++)
            {
                ulong p = (ulong)r.Managers[i].Pointer, n = (ulong)r.Managers[i].Length;

                if (p != 0 && host >= p && size <= n && host - p <= n - size)
                {
                    va = r.Va[i] + (host - p);
                    return true;
                }
            }

            va = host;
            return FarDir() == null;
        }

        private sealed class Regions
        {
            public const int Count = 16;
            public readonly WritableRegion[] Items = new WritableRegion[Count];
            public readonly PointerMemory[] Managers = new PointerMemory[Count];
            public readonly PointerMemory[] SeqManagers = new PointerMemory[Count];
            public readonly ulong[] Va = new ulong[Count];   // Items[i] 取时的游戏地址（Managers[i] 是它的宿主视图）
            // 远端跨块副本区域（带写回块；与 Items 分开：S2AReuse 无写回块的版本不收挂过写回块的区域）
            public readonly WritableRegion[] FarItems = new WritableRegion[Count];
            public readonly PointerMemory[] FarManagers = new PointerMemory[Count];
            public int Next;
            public int NextSeq;
            public int NextFar;

            public Regions()
            {
                for (int i = 0; i < Count; i++)
                {
                    Managers[i] = new PointerMemory();
                    SeqManagers[i] = new PointerMemory();
                    Items[i] = new WritableRegion(null, 0, Memory<byte>.Empty);
                    FarManagers[i] = new PointerMemory();
                    FarItems[i] = new WritableRegion(null, 0, Memory<byte>.Empty);
                }
            }
        }

        private static ThreadLocal<Regions> s_regions = new(() => new Regions());

        // 可重设指针的 MemoryManager（Ryujinx NativeMemoryManager 的指针只读）
        private sealed class PointerMemory : System.Buffers.MemoryManager<byte>
        {
            private byte* _ptr;
            private int _len;

            public void Reset(byte* ptr, int len)
            {
                _ptr = ptr;
                _len = len;
            }

            public byte* Pointer => _ptr;
            public int Length => _len;

            public override Span<byte> GetSpan() => new(_ptr, _len);

            public override System.Buffers.MemoryHandle Pin(int elementIndex = 0) => new(_ptr + elementIndex);

            public override void Unpin()
            {
            }

            protected override void Dispose(bool disposing)
            {
            }
        }

        public ref T GetRef<T>(ulong va) where T : unmanaged
        {
            if (Det)
            {
                Red.Report("确定性档下 GetRef 直接写游戏内存（不经 s2a_det_host_write，重放会分叉）", $"0x{va:X}");
            }

            // 一个 T 跨 2 MiB 远端块且两块影子不相邻：给不出引用。调用方取的是对齐的小结构，不该跨块；出现即错，不发跟踪事件就抛
            if (!Contiguous(va, (ulong)sizeof(T), out ulong h))
            {
                throw new InvalidMemoryRegionException($"S2Arm：GetRef<{typeof(T).Name}> 0x{va:X} 跨远端 2 MiB 块且两块影子不相邻，给不出引用");
            }

            PinHostAddress(va, (ulong)sizeof(T), true);
            SignalMemoryTracking(va, (ulong)sizeof(T), true);
            BeforeWrite(va, (ulong)sizeof(T));
            return ref Unsafe.AsRef<T>((void*)h);
        }

        // 渲染器零拷贝的依据，必须是真实宿主地址：远端按块切段，宿主上首尾相接的段并成一段（一次映射请求的影子按游戏地址升序连续，far.c s2a_far_map）
        public IEnumerable<HostMemoryRange> GetHostRegions(ulong va, ulong size)
        {
            PinHostAddress(va, size, true);
            long* dir = FarDir();

            if (dir == null)
            {
                return [new HostMemoryRange((nuint)va, size)];
            }

            return FarHostRegions(dir, va, size);
        }

        private static List<HostMemoryRange> FarHostRegions(long* dir, ulong va, ulong size)
        {
            ulong lo = Native.Far->Lo;
            ulong k = FarPiece(dir, lo, va, size, out ulong start), len = k;
            List<HostMemoryRange> list = new();

            for (ulong a = va + k, left = size - k; left != 0; a += k, left -= k)
            {
                k = FarPiece(dir, lo, a, left, out ulong h);

                if (h == start + len)
                {
                    len += k;
                    continue;
                }

                list.Add(new HostMemoryRange((nuint)start, len));
                start = h;
                len = k;
            }

            list.Add(new HostMemoryRange((nuint)start, len));
            return list;
        }

        public IEnumerable<MemoryRange> GetPhysicalRegions(ulong va, ulong size)
        {
            PinHostAddress(va, size, true);
            return [new MemoryRange(va, size)];
        }

        public bool IsMapped(ulong va) => IsRangeMapped(va, 1);

        public bool IsRangeMapped(ulong va, ulong size)
        {
            // 原生层直接判区间（10-21 帧率能耗线采样：暗黑3 渲染线程上逐段 MemQuery 并段占 20 秒 622 ms）；单测假原生层没有时退回逐段查
            if (Native.MemMapped != null)
            {
                return Native.MemMapped(va, size) != 0;
            }

            if (Native.MemQuery == null)
            {
                return va != 0;
            }

            ulong end = va + size;

            while (va < end)
            {
                MemInfo info;

                // 段尾必须越过 va（11-01 审计 P2-25：回的段在 va 之前或回绕时原先原地打转）
                if (Native.MemQuery(va, &info) != 0 || info.Perm == 0 || info.Size == 0 || info.Base + info.Size <= va)
                {
                    return false;
                }

                va = info.Base + info.Size;
            }

            return true;
        }

        public void SignalMemoryTracking(ulong va, ulong size, bool write, bool precise = false, int? exemptId = null)
        {
            if (precise && write)
            {
                t_preciseVa = va;
                t_preciseLen = (int)size;
            }

            Tracking.VirtualMemoryEvent(va, size, write, precise, exemptId);
        }

        public void TrackingReprotect(ulong va, ulong size, MemoryPermission protection, bool guest)
        {
            if (ReprotectHook != null)
            {
                ReprotectHook(va, size, protection, guest);
            }
        }

        public RegionHandle BeginTracking(ulong address, ulong size, int id, RegionFlags flags = RegionFlags.None)
            => Tracking.BeginTracking(address, size, id, flags);

        public MultiRegionHandle BeginGranularTracking(ulong address, ulong size, IEnumerable<IRegionHandle> handles, ulong granularity, int id, RegionFlags flags = RegionFlags.None)
            => Tracking.BeginGranularTracking(address, size, handles, granularity, id, flags);

        public SmartMultiRegionHandle BeginSmartGranularTracking(ulong address, ulong size, ulong granularity, int id)
            => Tracking.BeginSmartGranularTracking(address, size, granularity, id);
    }
}
