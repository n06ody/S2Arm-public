// C1：managed 侧的 direct 会话生命周期。顺序固定：bind compiler → activate → … → request_stop → destroy。
// 只做契约规定的顺序与错误映射：负码一律当失败，不把 Unsupported/Driver/Retirement 当成功，也不静默降级到 generic。
// 编译指针由调用方给（生产是 AOT 的 s2a_direct_compile_v1），这样这个类只依赖 DirectAbi，能单独做宿主测试。
using System;
using System.Runtime.InteropServices;

namespace S2A.Android.Aot
{
    public sealed unsafe class DirectSession
    {
        private DirectApiV1 _table;
        private bool _bound, _activated, _stopped, _destroyed;

        // native 的 s2a_direct_bind_compiler_v1(context, compile)；测试里注入假实现。
        public delegate int Binder(nint context, nint compile);

        public DirectSession(in DirectApiV1 table)
        {
            int check = DirectAbi.TableCheck(table);
            if (check != DirectAbi.Ok) throw new ArgumentException($"direct table is invalid ({check})", nameof(table));
            _table = table;
        }

        public DirectApiV1 Table => _table;
        public bool Bound => _bound;
        public bool Activated => _activated;
        public bool Stopped => _stopped;
        public bool Destroyed => _destroyed;
        public int LastError { get; private set; }

        /// <summary>把编译 provider 登记给 native（Activate 之前必须成功）。compile 为 0 一律当参数错误。</summary>
        public int BindCompiler(nint compile, Binder binder = null)
        {
            if (_destroyed) return Fail(DirectAbi.Stopped, "bind after destroy");
            if (compile == 0) return Fail(DirectAbi.Argument, "bind without a compile entry");
            binder ??= NativeBind;
            int rc = binder(_table.Context, compile);
            if (rc != DirectAbi.Ok) return Fail(rc, "bind compiler");
            _bound = true;
            return DirectAbi.Ok;
        }

        public int BindBinaryCompiler(nint compile, nint encode)
        {
            if (_destroyed || !_bound || _activated || compile == 0 || encode == 0)
                return Fail(DirectAbi.Argument, "bind binary compiler");
            try
            {
                int rc = BindBinaryCompilerNative(_table.Context, compile, encode);
                return rc == DirectAbi.Ok ? rc : Fail(rc, "bind binary compiler");
            }
            catch (EntryPointNotFoundException)
            {
                // Older native owners use the already registered JSON compiler.
                return DirectAbi.Ok;
            }
        }

        public int Activate()
        {
            if (_destroyed) return Fail(DirectAbi.Stopped, "activate after destroy");
            if (!_bound) return Fail(DirectAbi.Argument, "activate before bind");
            if (_activated) return Fail(DirectAbi.Argument, "activate twice");
            int rc = ((delegate* unmanaged<nint, int>)_table.Activate)(_table.Context);
            if (rc != DirectAbi.Ok) return Fail(rc, "activate");
            _activated = true;
            return DirectAbi.Ok;
        }

        public int Query(out DirectStatusV1 status)
        {
            status = default;
            if (_table.Query == 0) { LastError = DirectAbi.Argument; return DirectAbi.Argument; }
            DirectStatusV1 local = default;
            if (DirectAbi.StatusInit(ref local) != DirectAbi.Ok) { LastError = DirectAbi.Internal; return DirectAbi.Internal; }
            int rc = ((delegate* unmanaged<nint, DirectStatusV1*, int>)_table.Query)(_table.Context, &local);
            if (rc != DirectAbi.Ok) { LastError = rc; return rc; }
            int check = DirectAbi.StatusCheck(local);
            status = local;
            if (check != DirectAbi.Ok) { LastError = check; return check; }
            LastError = DirectAbi.Ok;
            return DirectAbi.Ok;
        }

        public int RequestStop()
        {
            if (_destroyed) return Fail(DirectAbi.Stopped, "stop after destroy");
            if (_stopped) return DirectAbi.Stopped;
            int rc = ((delegate* unmanaged<nint, int>)_table.RequestStop)(_table.Context);
            if (rc != DirectAbi.Ok) return Fail(rc, "request_stop");
            _stopped = true;
            return DirectAbi.Ok;
        }

        /// <summary>
        /// owner 控制片（provider.poll）：只有 owner 循环调用，budget 最多 10 ms。
        /// 返回 Ok/Pending/Stopped；Pending 是“仍有在途需求”，不是失败；Stopped 是 Stop 已收尾的
        /// 正常终点；其余负码按失败记录并保留首错。请求 Stop 之后仍允许继续 poll，直到 native 返回
        /// Stopped —— 这正是“Stop 期间 owner 继续控制轮询”的调用点。
        /// </summary>
        public int Poll(uint budgetMs = DirectOwnerLoop.SliceMs)
        {
            if (_destroyed) return Fail(DirectAbi.Stopped, "poll after destroy");
            if (!_activated) return Fail(DirectAbi.Argument, "poll before activate");
            if (_table.Poll == 0) return Fail(DirectAbi.Argument, "poll without an owner entry");
            if (budgetMs > 10) budgetMs = 10;
            int rc = ((delegate* unmanaged<nint, uint, int>)_table.Poll)(_table.Context, budgetMs);
            if (rc == DirectAbi.Ok || rc == DirectAbi.Pending) { LastError = DirectAbi.Ok; return rc; }
            if (rc == DirectAbi.Stopped) { LastError = DirectAbi.Stopped; return rc; }
            return Fail(rc, "poll");
        }

        public int Destroy()
        {
            if (_destroyed) return DirectAbi.Ok;
            int rc = ((delegate* unmanaged<nint, int>)_table.Destroy)(_table.Context);
            if (rc != DirectAbi.Ok) return Fail(rc, "destroy");
            _destroyed = true;
            return DirectAbi.Ok;
        }

        private int Fail(int rc, string what)
        {
            LastError = rc;
            Console.Error.WriteLine($"[s2a] direct session: {what} failed ({rc})");
            return rc;
        }

        private static int NativeBind(nint context, nint compile)
        {
            try { return BindCompilerNative(context, compile); }
            catch (DllNotFoundException) { return DirectAbi.Unsupported; }
            catch (EntryPointNotFoundException) { return DirectAbi.Unsupported; }
        }

        [DllImport("libs2arm_android.so", EntryPoint = "s2a_direct_bind_compiler_v1")]
        private static extern int BindCompilerNative(nint context, nint compile);

        [DllImport("libs2arm_android.so", EntryPoint = "s2a_direct_bind_binary_compiler_v1")]
        private static extern int BindBinaryCompilerNative(nint context, nint compile, nint encode);
    }
}
