using Ryujinx.HLE;
using Ryujinx.HLE.HOS.Services.Hid;
using Ryujinx.HLE.HOS.Services.Hid.Types.SharedMemory.TouchScreen;
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace S2A.Services
{
    // 手柄采样泵：iOS 输入变化立即提交，16 ms 心跳继续处理反馈；其他宿主沿用定时采样。
    // 输入 = 苹果侧输入源（InputHook，未接时为空）∪ 通用自动玩家的键（测试层，宿主里弱链接，有就取）。
    // 初始触屏按运行模式配置；首次发现外接手柄后偏好独立 Pro 手柄，操作 / 渲染模式保持不变。
    public static unsafe class InputPump
    {
        // 苹果侧手柄 / 触屏输入源（苹果侧接）：填按键位（HidNpadButton 位序）与左右摇杆
        public static delegate*<ref GamepadInput, void> InputHook;
        // 平台登记的手柄反馈入口（震动 / 六轴，runtime/include/s2a_pad.h 同名函数的地址，ControllerFeedback 用）：
        //   安卓宿主库以 RTLD_LOCAL 载入，主程序句柄查不到它的导出（docs/架构_多终端.md：按名查导出的几处接核心时改成函数指针），
        //   由平台装配在起服务前登记（android/core-runtime/AndroidGameRuntime.cs）；没登记的（iOS / Mac）全为 0，照旧按名查主程序导出
        [StructLayout(LayoutKind.Sequential)]
        public struct HostFeedback
        {
            public nint RumbleSession, SetRumblePair, StopRumble, SetMotionEnabled, ReadMotion;
        }
        public static HostFeedback Feedback;
        // Optional platform touch consumer, on this same HID owner thread.
        // Absent on Apple and other hosts; no second sampler or HID writer.
        public static Action<Switch> TouchHook;
        // Debug-only call site: reads the real HID memory after a touch commit.
        public static string DescribeTouch(Switch device)
        {
            ref var entry = ref device.Hid.SharedMemory.TouchScreen.GetCurrentEntryRef();
            var point = entry.Touches.AsSpan()[0];
            return $"HID touch #{entry.SamplingNumber}: count={entry.TouchesCount} first={point.FingerId}:{point.X},{point.Y} attr={point.Attribute}";
        }
        public static string DescribeGesture(Switch device)
        {
            ref var entry = ref device.Hid.SharedMemory.Gesture.GetCurrentEntryRef();
            return $"HID gesture #{entry.SamplingNumber}: {entry.Input.Type} context={entry.Input.ContextNumber} point={entry.Input.X},{entry.Input.Y} count={entry.Input.PointCount}";
        }

        // 自动玩家（tests/autoplay）给一号手柄的键、二号手柄的键（驾驶员答「2:键」）、要的手柄数（1 / 2）。宿主里按导出查；单测直接赋值
        public static delegate* unmanaged<uint> AutoplayKeys1;
        public static delegate* unmanaged<uint> AutoplayKeys2;
        public static delegate* unmanaged<int> AutoplayPads;
        private static Thread _thread;
        private static volatile bool _stop;
        private static volatile PlayerIndex _player = PlayerIndex.Handheld;
        private static volatile ControllerType _type = ControllerType.Handheld;
        private static volatile bool _externalController;
        private static int _configurationGeneration;

        // 虚拟手柄个数：1，或 2（本机双手柄：强制本地合作的款，如双人成行，要两个已连接的手柄才进得了游戏）。
        //   S2A_AP_PADS=2 起局即开；游戏经手柄设置小程序声明至少 2 人时，运行中由 Host.DisplayMessageDialog 开到 2（SetPads）；
        //   自动玩家要二号手柄时（s2a_autoplay_pads = 2），采样泵下一拍开到 2。
        //   第二个手柄镜像第一个的输入（触屏 / 外接手柄同一份），两人的角色同步动；自动玩家接管二号手柄后（s2a_autoplay_pads ≥ 2），
        //   二号只收自动玩家的「2:键」、自动玩家给一号的键不镜像（双人成行选角色：镜像时两人总挪到同一个角色）
        private static readonly int EnvPads = Environment.GetEnvironmentVariable("S2A_AP_PADS") == "2" ? 2 : 1;
        private static volatile int _pads = EnvPads;
        private static volatile PlayerIndex _second = PlayerIndex.Unknown;
        // 手柄设置小程序最近一次给的人数上限（缺省 MaxPads）：真机上多连的手柄不分给游戏，直到下次小程序放宽或这局结束。
        //   #809：游戏要 1 人、连着两个手柄时原先仍配两格，小程序重验「已连 2 > 上限 1」不过、反复重开（一局 2780 次）
        private static volatile int _padCap = MaxPads;
        public const int MaxPads = 8;   // 同原机最多 8 位玩家（#520；Mac 宿主 S2A_MAX_PADS）

        public static PlayerIndex Player => _player;
        public static PlayerIndex SecondPlayer => _second;
        public static ControllerType Type => _type;
        public static int Pads => _pads;
        // 游戏的这个手柄号对应宿主第几个槽位（多手柄时 SetPrimary 从主手柄号起依次配满 n 格，见 macos/front_mac.m s2a_vpad_read_pad）；不是多手柄里的一格回 -1
        // 宿主第 slot 个槽位对应游戏的哪个手柄号（SlotOf 的反向）；不是多手柄里的一格回 Unknown
        public static PlayerIndex PlayerAt(int slot)
        {
            PlayerIndex next = _player;
            if (slot < 0 || slot >= _pads || next == PlayerIndex.Handheld) return slot == 0 ? next : PlayerIndex.Unknown;
            for (int i = 0; i < slot; i++) next = next == PlayerIndex.Player8 ? PlayerIndex.Player1 : next + 1;
            return next;
        }
        public static int SlotOf(PlayerIndex player)
        {
            int pads = _pads;
            PlayerIndex next = _player;
            if (pads <= 1 || next == PlayerIndex.Handheld) return -1;
            for (int i = 0; i < pads; i++)
            {
                if (next == player) return i;
                next = next == PlayerIndex.Player8 ? PlayerIndex.Player1 : next + 1;
            }
            return -1;
        }
        public static bool ExternalController => _externalController;

        public static void Start(Switch device)
        {
            SetPrimary(device, Host.Docked ? PlayerIndex.Player1 : PlayerIndex.Handheld, Host.Docked ? ControllerType.ProController : ControllerType.Handheld);
            // 游戏声明收哪些手柄号 / 样式时，在那次服务调用里就跟随改接（NpadDevices.S2ASupportedChanged）。原先只靠下面采样泵 250 ms 一拍：
            //   11-14 #cef2ab 一款游戏起局只声明 Player1，泵下一拍（0.6–1.2 s，或它后来再声明后的 11 / 22 / 102 s）才把掌机格改接到 Player1 Pro，
            //   这条「已连接」落在游戏初始化中途，它处理连接的线程用了还没建好的对象崩溃（15 局崩溃 15 局都紧跟这一行）。250 ms 一拍保留兜底
            Ryujinx.HLE.HOS.Services.Hid.NpadDevices.S2ASupportedChanged = FollowNow;
        }
        private static readonly object _followSync = new();
        // 改接之后立刻把配置写进手柄共享内存（S2AApplyConfiguration，审计 P1-192）：游戏在这次声明的服务调用返回后读到的就是新连接，不等输入泵下一拍
        internal static void FollowNow(Switch device) { lock (_followSync) { FollowSupportedPlayers(device); FollowSupportedStyles(device); device.Hid.Npads.S2AApplyConfiguration(); } }

        // 游戏声明的手柄号不含掌机格、含 Player1 时（11-02 维护者 手测七龙珠电光炸裂ZERO 010035F022078000：SetSupportedNpadIdType 只给 Player1，
        //   掌机格被 Remap 断开、「No matching controllers found」，触屏手柄按什么都没反应；真机掌机模式下这时握着的 Joy-Con 当 Player1 用）：
        //   主手柄在掌机格就改到 Player1 的 Pro 手柄（样式不符时 Remap 自己换成双 Joy-Con）；之后游戏又声明掌机格、且是这里挪走的，挪回掌机格。
        //   采样泵每 250 ms 调一次；只读手柄号表，不改游戏的声明
        //   用 IsSupportedPlayer 逐格查，不走 GetSupportedPlayers 的迭代器：采样泵稳态零分配
        private static bool _movedOffHandheld;
        internal static void FollowSupportedPlayers(Switch device)
        {
            bool handheld = device.Hid.Npads.IsSupportedPlayer(PlayerIndex.Handheld), player1 = device.Hid.Npads.IsSupportedPlayer(PlayerIndex.Player1);
            if (_player == PlayerIndex.Handheld && !handheld && player1)
            {
                _movedOffHandheld = true;
                Console.Error.WriteLine("S2A 虚拟手柄：游戏只收 Player1、不收掌机格，改到 Player1 的 Pro 手柄");
                SetPrimary(device, PlayerIndex.Player1, ControllerType.ProController);
            }
            else if (_movedOffHandheld && handheld && _player == PlayerIndex.Player1 && _pads == 1 && !_externalController && !Host.Docked)
            {
                _movedOffHandheld = false;
                Console.Error.WriteLine("S2A 虚拟手柄：游戏又收掌机格，挪回掌机格");
                SetPrimary(device, PlayerIndex.Handheld, ControllerType.Handheld);
            }
        }

        // 游戏声明的样式不收主手柄现在的样式时换一个它收的（#359：开局「用你要用的手柄按 A」那一屏只收 掌机 / 单只 Joy-Con / 精灵球手柄，
        //   主机档下虚拟手柄是 Player1 的 Pro 手柄、接了外接手柄也会改成 Pro 手柄；Ryujinx 的 Remap 只会 Pro ↔ 双 Joy-Con 互换，其余直接断开，按什么都没反应）。
        //   挑的顺序：掌机格（不在主机档、游戏收掌机格、只有一个手柄）→ 双 Joy-Con → Pro → 单只右 → 单只左（单只按横握换键，RotateSideways）；
        //   游戏又收回原来的样式就换回去。跟 FollowSupportedPlayers 同一个 250 ms 节拍，只读游戏的声明、不改它
        private static ControllerType _styleBefore = ControllerType.None;
        private static PlayerIndex _playerBefore;
        private static bool Accepts(Switch device, ControllerType styles, ControllerType type) =>
            type == ControllerType.Handheld ? (styles & type) != 0 && !device.System.State.DockedMode
            : device.Hid.Npads.StyleSupported(type, styles) || (type == ControllerType.ProController && (styles & ControllerType.JoyconPair) != 0)
                                   || (type == ControllerType.JoyconPair && (styles & ControllerType.ProController) != 0);   // 这一对 Remap 自己会换
        // 单只 Joy-Con（#520 A，照真机）：连着单只 Joy-Con 2（宿主 s2a_vpad_joycon_style 报它那一侧的 JoyconLeft / JoyconRight）、
        //   游戏收这个样式、只有一个手柄 → 改成 Player1 的这个样式，游戏按 NpadJoyHoldType 自己转竖 / 横；
        //   Joy-Con 断开 / 换成别的、或游戏不收了 → 换回改之前的。返回要改成的 (手柄号, 样式)，不改为 (Unknown, None)。纯函数，单测 --joycon-style
        internal static (PlayerIndex, ControllerType) JoyconPick(ControllerType styles, ControllerType single, int pads, ControllerType current,
                                                                ControllerType before, PlayerIndex playerBefore)
        {
            if (single != ControllerType.None && (styles & single) != 0 && pads == 1)
                return current == single ? (PlayerIndex.Unknown, ControllerType.None) : (PlayerIndex.Player1, single);
            if (before != ControllerType.None && current is ControllerType.JoyconLeft or ControllerType.JoyconRight)
                return (playerBefore, before);
            return (PlayerIndex.Unknown, ControllerType.None);
        }
        private static ControllerType _joyconBefore = ControllerType.None;
        private static PlayerIndex _joyconPlayerBefore;
        private static delegate* unmanaged<int> _joyconStyle;
        private static delegate* unmanaged<int, void> _setStyle;
        private static delegate* unmanaged<int, int, void> _setSlotStyle;   // 每一格的样式（#520：玩家 2 起的单只 Joy-Con 也照它那一格定原始方向 / 横握，同玩家 1）
        private static readonly ControllerType[] _slotTypes = new ControllerType[MaxPads];
        private static delegate* unmanaged<int, int> _slotStyle;   // 每一格的实体手柄样式（Mac 宿主 s2a_vpad_slot_style，#520）：多手柄时照它配格
        // 多手柄时这一格配什么样式（同原机：手柄号的样式由连着的那只手柄定）：实体是单只 Joy-Con（左 / 右只）、游戏收这一侧 → 就用它
        //   （改前一律「一号右只、二号左只」交替或同一种，拆开的左只当 P1 时游戏以为是右只：按 A 确认不了、震动给错侧）；
        //   别的（Pro / 一对 / 系统手柄 / 游戏不收这一侧）照原来的配法 fallback。纯函数，单测 --joycon-style
        internal static ControllerType SlotPick(ControllerType physical, ControllerType fallback, Func<ControllerType, bool> supported) =>
            physical is ControllerType.JoyconLeft or ControllerType.JoyconRight && supported(physical) ? physical : fallback;
        private static delegate* unmanaged<int, int, int, int> _assign;   // 游戏的手柄设置画面交给启动器分配玩家（macos/front_mac.m s2a_vpad_assign，#520 E）
        // 手柄设置画面：Mac 宿主有这个导出就先请启动器面板分配（玩家按 L + R / SL + SR 登记），回登记了几位；没有 / 没人接 / 取消回 0（调用方照旧自动应答）
        public static int AssignViaLauncher(int min, int max, ControllerType styles)
        {
            if (_assign == null && NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_assign", out nint va))   // 平台例外：Mac 游戏窗口专有导出，同 s2a_vpad_set_style
                _assign = (delegate* unmanaged<int, int, int, int>)va;
            return _assign != null ? _assign(min, max, (int)styles) : 0;   // styles：游戏要的样式，启动器先照它自动拆 / 拼，够就不弹面板
        }
        private static bool FollowJoyconStyle(Switch device, ControllerType styles)
        {
            ControllerType single = _joyconStyle != null ? (ControllerType)_joyconStyle() : ControllerType.None;
            (PlayerIndex player, ControllerType type) = JoyconPick(styles, single, _pads, _type, _joyconBefore, _joyconPlayerBefore);
            if (type == ControllerType.None) return single != ControllerType.None && _type == single;
            if (type == single)
            {
                if (_joyconBefore == ControllerType.None) { _joyconBefore = _type; _joyconPlayerBefore = _player; }
                Console.Error.WriteLine($"S2A 虚拟手柄：连着单只 Joy-Con、游戏收 {single}，改成 {player} 的 {type}（游戏按握法自己转）");
            }
            else
            {
                _joyconBefore = ControllerType.None;
                Console.Error.WriteLine($"S2A 虚拟手柄：单只 Joy-Con 不在了或游戏不收 {_type}，换回 {player} 的 {type}");
            }
            SetPrimary(device, player, type);
            return true;
        }
        // 告诉宿主的样式（宿主据此定单只 Joy-Con 报原始方向还是转横握）：Pro / 双 Joy-Con 这一对 Remap 会自己互换，按游戏收不收 Pro 报——
        //   收 Pro 报 Pro（宿主转横握扮 Pro，一只手柄全键可用，同改前）；不收 Pro、只收双 Joy-Con 报 JoyconPair（11-11 实机：一款体育游戏的球类项目：
        //   游戏只收 掌机 / 双 Joy-Con、要玩家直握一只，真机是双手模式只接一侧、按原始方向报）。其余样式原样。纯函数，单测 --joycon-style
        internal static ControllerType ReportedStyle(ControllerType styles, ControllerType type)
        {
            if (styles == ControllerType.None || type is not (ControllerType.ProController or ControllerType.JoyconPair)) return type;
            return (styles & ControllerType.ProController) != 0 ? ControllerType.ProController
                 : (styles & ControllerType.JoyconPair) != 0 ? ControllerType.JoyconPair : type;
        }
        private static void ReportStyle(Switch device)
        {
            ControllerType styles = device.Hid.Npads.SupportedStyleSets;
            if (_setStyle != null) _setStyle((int)ReportedStyle(styles, _type));
            if (_setSlotStyle != null)
                for (int i = 0; i < MaxPads; i++) _setSlotStyle(i, (int)ReportedStyle(styles, i < Math.Max(_pads, 1) ? _slotTypes[i] : ControllerType.None));
        }
        private static bool SlotStylesChanged(Switch device, ControllerType styles)
        {
            if (_slotStyle == null || _pads <= 1 || _type == ControllerType.Handheld) return false;
            // 不用 SlotPick 的委托：lambda 捕获参数时方法一进来就分配闭包（每 250 ms 一次，空闲底噪单测零分配那项红），这里展开成同一判断
            for (int i = 0; i < _pads; i++)
            {
                ControllerType physical = (ControllerType)_slotStyle(i);
                if (physical is not (ControllerType.JoyconLeft or ControllerType.JoyconRight)) continue;
                if (physical != _slotTypes[i] && device.Hid.Npads.StyleSupported(physical, styles)) return true;
            }
            return false;
        }
        internal static void FollowSupportedStyles(Switch device)
        {
            ControllerType styles = device.Hid.Npads.SupportedStyleSets;
            if (styles == ControllerType.None) return;
            ReportStyle(device);   // 游戏改了声明、样式没变时也要更新（改配时 SetPrimary 另报）
            if (FollowJoyconStyle(device, styles)) return;
            if (SlotStylesChanged(device, styles))   // 多手柄：哪一格换了一只（左只 ↔ 右只）就照实体重配
            {
                Console.Error.WriteLine("S2A 虚拟手柄：有一格换了单只 Joy-Con（左 / 右只），照连着的那只重配");
                SetPrimary(device, _player, _type);
                return;
            }
            if (_styleBefore != ControllerType.None && _type != _styleBefore && Accepts(device, styles, _styleBefore))
            {
                Console.Error.WriteLine($"S2A 虚拟手柄：游戏又收 {_styleBefore}，换回去");
                ControllerType back = _styleBefore; _styleBefore = ControllerType.None;
                SetPrimary(device, _playerBefore, back);
                return;
            }
            if (Accepts(device, styles, _type)) return;
            var npads = device.Hid.Npads;
            (PlayerIndex player, ControllerType type) pick =
                _pads == 1 && Accepts(device, styles, ControllerType.Handheld) && npads.IsSupportedPlayer(PlayerIndex.Handheld) ? (PlayerIndex.Handheld, ControllerType.Handheld)
                : (styles & ControllerType.JoyconPair) != 0 ? (PlayerIndex.Player1, ControllerType.JoyconPair)
                : (styles & ControllerType.ProController) != 0 ? (PlayerIndex.Player1, ControllerType.ProController)
                : (styles & ControllerType.JoyconRight) != 0 ? (PlayerIndex.Player1, ControllerType.JoyconRight)
                : (styles & ControllerType.JoyconLeft) != 0 ? (PlayerIndex.Player1, ControllerType.JoyconLeft)
                : (PlayerIndex.Unknown, ControllerType.None);
            if (pick.type == ControllerType.None) return;   // 只收精灵球之类的：没有能扮的，不动
            if (_styleBefore == ControllerType.None) { _styleBefore = _type; _playerBefore = _player; }
            Console.Error.WriteLine($"S2A 虚拟手柄：游戏只收 {styles}，不收 {_type}，改成 {pick.player} 的 {pick.type}");
            SetPrimary(device, pick.player, pick.type);
        }

        // 开到 n 个虚拟手柄（最多 MaxPads，只增不减；收局 Stop 回到 S2A_AP_PADS 给的个数），按当前主手柄重配
        public static void SetPads(Switch device, int n)
        {
            if (n > MaxPads) Red.Report($"游戏要 {n} 个手柄，虚拟手柄最多 {MaxPads} 个");
            n = Math.Min(n, Math.Min(MaxPads, _padCap));   // 小程序给过的人数上限也不超（多连的手柄 / 自动玩家要的二号都不分给游戏）
            if (n <= _pads) return;
            _pads = n;
            Console.Error.WriteLine($"S2A 虚拟手柄：开到 {n} 个（Mac 前端按连接顺序各给一只，二号起不再镜像一号）");
            SetPrimary(device, _player, _type);
        }

        // 手柄设置小程序给的人数上限（PlayerCountMax；≤ 0 当不限）：记下，已开的格数超了就收回到上限、按当前主手柄重配（同真机：只留选中的那几只）
        public static void CapPads(Switch device, int max)
        {
            _padCap = max <= 0 ? MaxPads : Math.Min(max, MaxPads);
            if (_pads <= _padCap) return;
            Console.Error.WriteLine($"S2A 虚拟手柄：游戏这次最多收 {_padCap} 人，从 {_pads} 个收回到 {_padCap} 个（多连的手柄不分给游戏）");
            _pads = _padCap;
            SetPrimary(device, _player, _type);
        }

        // 虚拟手柄改配到 player / type；之后的输入都喂这一格。双手柄时第二格取下一个手柄号、同样式
        //   （掌机格只有一个：两人玩时主手柄离开掌机格改成 Player1 的 Pro 手柄，同真机上拆下 Joy-Con 各拿一个；
        //   Pro / 双 Joy-Con 与游戏声明不符时 Ryujinx 的 Remap 自己换，手柄号不在游戏声明里的那格不连）
        public static void SetPrimary(Switch device, PlayerIndex player, ControllerType type)
        {
            if (_pads > 1 && (type == ControllerType.Handheld || player == PlayerIndex.Handheld))
            {
                (player, type) = (PlayerIndex.Player1, ControllerType.ProController);
            }
            lock (device.Hid.Npads.RumbleSyncRoot)
            {
                PlayerIndex second = _pads > 1 ? (player == PlayerIndex.Player8 ? PlayerIndex.Player1 : player + 1) : PlayerIndex.Unknown;
                if (_pads > 1)
                {
                    // 多手柄：一次配满 n 格（Mac 前端按槽位各喂一只，见 macos/front_mac.m 的 s2a_vpad_read_pad）。
                    //   游戏同时收左、右单只 Joy-Con（派对类横握多人）而主手柄是单只：各格左右交替（一号右只、二号左只……），
                    //   不配成同一种——同种两格时另一只的六轴会被盖掉（#592 lucaAce1024 实机）
                    ControllerType styles = device.Hid.Npads.SupportedStyleSets;
                    bool alternate = (type == ControllerType.JoyconLeft || type == ControllerType.JoyconRight)   // 只声明 Joy-Con 2 左 / 右只位的也算（StyleSupported，#747）
                                     && device.Hid.Npads.StyleSupported(ControllerType.JoyconLeft, styles) && device.Hid.Npads.StyleSupported(ControllerType.JoyconRight, styles);
                    if (alternate) type = ControllerType.JoyconRight;
                    var many = new ControllerConfig[_pads];
                    PlayerIndex next = player;
                    for (int i = 0; i < _pads; i++)
                    {
                        ControllerType fallback = alternate && i % 2 == 1 ? ControllerType.JoyconLeft : type;
                        ControllerType physical = _slotStyle != null ? (ControllerType)_slotStyle(i) : ControllerType.None;
                        many[i] = new ControllerConfig { Player = next, Type = SlotPick(physical, fallback, t => device.Hid.Npads.StyleSupported(t, styles)) };
                        _slotTypes[i] = many[i].Type;
                        next = next == PlayerIndex.Player8 ? PlayerIndex.Player1 : next + 1;
                    }
                    device.Hid.Npads.Configure(many);
                    if (alternate) Console.Error.WriteLine($"S2A 虚拟手柄：左右单只交替配 {_pads} 格（一号右只、二号左只）");
                }
                else
                {
                    device.Hid.Npads.Configure(new ControllerConfig { Player = player, Type = type });
                    _slotTypes[0] = type;
                }
                _device = device;
                _player = type == ControllerType.Handheld ? PlayerIndex.Handheld : player;
                _type = type;
                _second = second;
                _feedback?.SetPlayer(_player);
                Interlocked.Increment(ref _configurationGeneration);
            }
            ReportStyle(device);   // 宿主按这一格的样式决定单只 Joy-Con 报原始方向还是转横握（#520 A）

            if (_thread != null)
            {
                if (_padWake != null) _padWake();
                return;
            }

            // 自动玩家在宿主里是弱链接：运行时查符号，没链就不并
            if (AutoplayKeys1 == null && NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_autoplay_keys", out nint p))   // 平台例外：自动玩家只在开发宿主里弱链接、生产包没有这个导出，进不了 Native.Imports 的 DllImport
            {
                AutoplayKeys1 = (delegate* unmanaged<uint>)p;
            }
            if (AutoplayKeys2 == null && NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_autoplay_keys2", out nint p2))   // 平台例外：自动玩家只在开发宿主里弱链接、生产包没有这个导出，进不了 Native.Imports 的 DllImport
            {
                AutoplayKeys2 = (delegate* unmanaged<uint>)p2;
            }
            if (AutoplayPads == null && NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_autoplay_pads", out nint pn))   // 平台例外：自动玩家只在开发宿主里弱链接、生产包没有这个导出，进不了 Native.Imports 的 DllImport
            {
                AutoplayPads = (delegate* unmanaged<int>)pn;
            }

            // 回放录手柄的摇杆模拟量（tests/autoplay：正式局 MARK 后回放 维护者 录的按键）；录手柄的存档标记（ios/vpad.m，iOS 才有）：游戏存档提交时记一行
            if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_autoplay_sticks", out nint sp))
            {
                _autoplaySticks = (delegate* unmanaged<int*, int>)sp;
            }
            if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_autoplay_touch", out nint tp))
            {
                _autoplayTouch = (delegate* unmanaged<int*, uint*, int>)tp;
            }

            if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_padrec_mark", out nint pm))
            {
                _padrecMark = (delegate* unmanaged<byte*>)pm;
            }

            if (_padrecMark != null)
            {
                Ryujinx.HLE.S2AHooks.SaveCommitted = &SaveCommitted;
            }

            // 主 App 的纯 C 查询不加载可选 BLE 模块；只有真实 READY 会话才创建反馈对象。
            if (Feedback.RumbleSession != 0)
                _rumbleSession = (delegate* unmanaged<ulong>)Feedback.RumbleSession;   // 平台登记（安卓）
            else if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_pad_rumble_session", out nint session))
                _rumbleSession = (delegate* unmanaged<ulong>)session;
            // Mac 宿主只导出六轴、没有震动会话：反馈对象照建，只走六轴
            else _motionOnly = ControllerFeedback.MotionExport(out _);

            // R5 键盘区：宿主喂的原始键态（runtime/src/pad.c s2a_kb_read：HID usage 位图 + 修饰键字段，macos/front_mac.m 喂）。
            //   iOS / 安卓没喂过恒为空 → 键盘区同改前（Activate 时的空状态）。单测经此注入替身
            if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_kb_read", out nint kb))   // 平台例外：runtime/src/pad.c 只进苹果宿主（安卓不编），同手柄读口按符号查、没有即键盘区同改前
                _kbRead = (delegate* unmanaged<ulong*, uint*, void>)kb;

            // iOS 壳的触屏虚拟手柄（ios/vpad.m）：同样按符号查，有就接到 InputHook（Mac 宿主没有这个导出，保持为空）
            if (InputHook == null && NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_read", out nint vp))
            {
                _vpadRead = (delegate* unmanaged<int*, uint>)vp;
                InputHook = _vpadHook = &VPad;
                // 多手柄（本地双人）：Mac 前端（macos/front_mac.m）按连接顺序为每只扩展手柄留一个槽位，另有这两个导出时各喂各的。
                //   iOS 壳 / 旧宿主没导出，保持为空 → 仍旧单手柄 + 第二格镜像。
                if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_read_pad", out nint vpp))   // 平台例外：宿主前端（macos/front_mac.m）专有导出，iOS 壳 / 安卓宿主没有
                    _vpadReadPad = (delegate* unmanaged<int, int*, uint>)vpp;
                if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_pad_count", out nint vpc))   // 平台例外：同上
                    _vpadPadCount = (delegate* unmanaged<int>)vpc;
                // 触屏直通：iOS 壳（ios/vpad.m，仅触屏方案的手指）与 Mac 游戏窗口（macos/front_mac.m，鼠标左键点画面）导出 s2a_vpad_read_touch；没导出的宿主保持为空、不碰触摸屏。
                if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_read_touch", out nint vtouch))
                    _vpadTouch = (delegate* unmanaged<int*, uint*, int>)vtouch;
                // 审计 P1-3：游戏启用触屏时告诉 iOS 壳（按键方案下提示一次怎么切「仅触屏操作」）；Mac 宿主没有这个导出
                bool touchWanted = NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_touch_wanted", out nint vwanted);   // 平台例外：iOS 壳（ios/vpad.m）专有导出，同上面 s2a_vpad_read_touch，Mac / 安卓宿主没有，进不了 Native.Imports 的 DllImport
                if (touchWanted) VPadTouchWanted = (delegate* unmanaged<int>)vwanted;
                // 游戏声明触屏配置时告诉宿主前端：iOS 壳自动切仅触屏（ios/touch_config.m），Mac 游戏窗口在缺省键位下把左键让给触屏（front_mac.m）；平台例外同上，安卓宿主没有这个导出
                if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_touch_configured", out nint vconfigured))   // 平台例外：宿主前端（iOS 壳 touch_config.m / Mac 游戏窗口 front_mac.m）专有导出，同 s2a_vpad_touch_wanted / s2a_vpad_read_touch，安卓与无窗口宿主没有，进不了 Native.Imports 的 DllImport
                    VPadTouchConfigured = (delegate* unmanaged<int, int>)vconfigured;
                // 单只 Joy-Con 原始方向（#520 A）：Mac 游戏窗口（macos/front_mac.m）专有导出；平台例外同 s2a_vpad_touch_configured，iOS / 安卓宿主没有
                if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_joycon_style", out nint vjoycon) &&   // 平台例外：Mac 游戏窗口专有导出，进不了 Native.Imports 的 DllImport
                    NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_set_style", out nint vstyle))   // 平台例外：同上
                {
                    _joyconStyle = (delegate* unmanaged<int>)vjoycon;
                    _setStyle = (delegate* unmanaged<int, void>)vstyle;
                    if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_set_slot_style", out nint vslot))   // 平台例外：同上（Mac 游戏窗口专有）
                        _setSlotStyle = (delegate* unmanaged<int, int, void>)vslot;
                    if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_slot_style", out nint vphys))   // 平台例外：同上（Mac 游戏窗口专有）
                        _slotStyle = (delegate* unmanaged<int, int>)vphys;
                    ReportStyle(device);
                }
                if (NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "s2a_vpad_read_ex", out nint extended))
                {
                    nint program = NativeLibrary.GetMainProgramHandle();
                    _padReadEx = (delegate* unmanaged<PadSnapshot*, void>)extended;
                    _padWait = (delegate* unmanaged<ulong, ulong, int>)NativeLibrary.GetExport(program, "s2a_pad_wait");
                    _padWake = (delegate* unmanaged<void>)NativeLibrary.GetExport(program, "s2a_pad_wake");
                    _padNow = (delegate* unmanaged<ulong>)NativeLibrary.GetExport(program, "s2a_pad_now_us");
                    _padCommit = (delegate* unmanaged<PadSnapshot*, void>)NativeLibrary.GetExport(program, "s2a_pad_record_commit");
                    _padLatency = (delegate* unmanaged<HostInputLatency*, int, void>)NativeLibrary.GetExport(program, "s2a_pad_get_latency");
                }
            }

            _stop = false;
            _thread = new Thread(() => Loop(device)) { Name = "S2A.HID", IsBackground = true };
            _thread.Start();
        }

        public static void Stop()
        {
            if (_thread == Thread.CurrentThread)
                throw new InvalidOperationException("不能在 HID 采样线程上拆除服务会话");
            _stop = true;
            if (_padWake != null) _padWake();
            _thread?.Join();
            // 收局摘回调（审计 P1-192）：之后服务线程的声明不再改这一局已拆掉的手柄；正在跑的那次跟随做完再往下复位（同一把锁）
            Action<Switch> follow = FollowNow;
            if (Ryujinx.HLE.HOS.Services.Hid.NpadDevices.S2ASupportedChanged == follow) Ryujinx.HLE.HOS.Services.Hid.NpadDevices.S2ASupportedChanged = null;
            Monitor.Enter(_followSync);
            try {
            if (_device != null) lock (_device.Hid.Npads.RumbleSyncRoot)
            {
                _feedback?.Stop();
                _feedback = null;
            }
            _thread = null;
            _device = null;
            _externalController = false;
            _configurationGeneration = 0;
            _player = PlayerIndex.Handheld;
            _type = ControllerType.Handheld;
            _pads = EnvPads;
            _padCap = MaxPads;   // 上一局小程序给的上限不带进下一局
            _second = PlayerIndex.Unknown;
            _lastSticks = default;
            _movedOffHandheld = false;   // 审计 P1-94：上一局「挪出掌机格」不带进下一局（同进程重启）
            _styleBefore = ControllerType.None;   // 同上：上一局「按样式换过」不带进下一局
            _joyconBefore = ControllerType.None;   // 同上：上一局「按单只 Joy-Con 换过」不带进下一局
            } finally { Monitor.Exit(_followSync); }
        }

        private static delegate* unmanaged<int*, uint> _vpadRead;
        private static delegate*<ref GamepadInput, void> _vpadHook;
        private static delegate* unmanaged<int*, int> _autoplaySticks;
        private static delegate* unmanaged<byte*> _padrecMark;
        private static Switch _device;
        private static ControllerFeedback _feedback;
        private static delegate* unmanaged<ulong> _rumbleSession;
        private static bool _motionOnly;
        // R5 键盘区原始键态：宿主导出 s2a_kb_read（HID usage 位图 + 修饰键字段），没导出 / 没喂的宿主恒为空
        internal static delegate* unmanaged<ulong*, uint*, void> _kbRead;
        private static delegate* unmanaged<int, int*, uint> _vpadReadPad;   // 多手柄：按槽位读（macos/front_mac.m 导出）
        private static delegate* unmanaged<int> _vpadPadCount;               // 多手柄：当前占用的槽位数（≥ 1）
        private static delegate* unmanaged<PadSnapshot*, void> _padReadEx;
        private static delegate* unmanaged<ulong, ulong, int> _padWait;
        private static delegate* unmanaged<void> _padWake;
        private static delegate* unmanaged<ulong> _padNow;
        private static delegate* unmanaged<PadSnapshot*, void> _padCommit;
        private static delegate* unmanaged<HostInputLatency*, int, void> _padLatency;
        // iOS 壳的「触屏直通」方案：ios/vpad.m 把手指原样写成 Switch 触点，这里每帧问一次喂 Hid.Touchscreen（Mac 游戏窗口的鼠标左键点画面同一个导出，front_mac.m）。
        // 虚拟按键方案（含默认布局）不导出触点，这里恒为 0 个——按 A / 推摇杆不会被游戏当成点按。
        private static delegate* unmanaged<int*, uint*, int> _vpadTouch;
        // 审计 P1-3：iOS 壳的 s2a_vpad_touch_wanted。游戏启用了触屏（ActivateTouchScreen → Hid.Touchscreen.Active）后每次 HID 提交问一次：
        //   壳返回 0 = 还没出首帧（加载页挡着），下次再问；返回 1 = 收下了，清空、之后不再问。只在 HID 线程上读写
        internal static delegate* unmanaged<int> VPadTouchWanted;
        internal static void OfferTouchWanted(bool active)
        {
            if (VPadTouchWanted != null && active && VPadTouchWanted() != 0) VPadTouchWanted = null;
        }
        // 11-04 维护者定：宿主前端的 s2a_vpad_touch_configured（iOS ios/touch_config.m、Mac macos/front_mac.m）。游戏调了 SetTouchScreenConfiguration
        //   （Ryujinx 补丁 hid-touch-config-mode 记进 Hid.Touchscreen.ConfiguredMode，-1 = 没调过）后每次 HID 提交递一次模式：
        //   壳返回 0 = 还没出首帧，下次再递；返回 1 = 收下了（壳自动切仅触屏，玩家手动选过的不切），清空、之后不再递。只在 HID 线程上读写
        internal static delegate* unmanaged<int, int> VPadTouchConfigured;
        internal static void OfferTouchConfigured(int mode)
        {
            if (VPadTouchConfigured != null && mode >= 0 && VPadTouchConfigured(mode) != 0) VPadTouchConfigured = null;
        }
        // 自动玩家的点触（tests/autoplay：路线 / 实时通道 TOUCH:x,y 步，指针 / 点触类页面用）：签名同 _vpadTouch，Mac / iOS 都按符号查，并在真触点之后
        private static delegate* unmanaged<int*, uint*, int> _autoplayTouch;

        [StructLayout(LayoutKind.Sequential)]
        private struct PadSnapshot
        {
            public ulong NotificationSeq, InputEpoch, HostTimestampUs, ReadTimestampUs;
            public uint Buttons, Source;
            public int LeftX, LeftY, RightX, RightY;
        }

        // 只测宿主输入快照 / 读取至 HID 提交，不包含蓝牙传输、游戏采样或显示延迟。
        [StructLayout(LayoutKind.Sequential)]
        public struct HostInputLatency
        {
            public ulong Samples, HostTotalUs, ReadTotalUs, HostMaxUs, ReadMaxUs;
        }

        public static HostInputLatency ReadHostInputLatency(bool reset = false)
        {
            HostInputLatency result = default;
            if (_padLatency != null) _padLatency(&result, reset ? 1 : 0);
            return result;
        }

        private static void RefreshFeedback(Switch device)
        {
            if (_rumbleSession == null)
            {
                if (_motionOnly && _feedback == null) _feedback = new ControllerFeedback(device, _player);
                return;
            }
            // 与改配、逐请求反馈共用 Npad 锁。会话代次由现有对象在每个请求前复核，
            // 不因 READY 到 READY 的代次切换重建对象、清掉刚接受的新帧。
            lock (device.Hid.Npads.RumbleSyncRoot)
            {
                if (_rumbleSession() == 0)
                {
                    _feedback?.Stop();
                    _feedback = null;
                }
                else if (_feedback == null)
                    _feedback = new ControllerFeedback(device, _player);
            }
        }

        // 存档提交（FS 服务线程，游戏的 Commit 返回前）：录制方记一个存档点并给出副本目录，这里把当时的存档整份拷进去（回放局按它带存档起局）
        private static void SaveCommitted()
        {
            if (_padrecMark == null) return;

            byte* dir = _padrecMark();

            if (dir != null && _device != null)
            {
                Saves.Snapshot(_device, Application.ProgramId, Marshal.PtrToStringUTF8((nint)dir));
            }
        }

        // 触屏虚拟手柄 → 手柄状态：按键位同 HidNpadButton；摇杆非零才覆盖（与自动玩家的键按位或并存）
        private static void VPad(ref GamepadInput input)
        {
            int* s = stackalloc int[4];
            input.Buttons |= (ControllerKeys)_vpadRead(s);
            if (s[0] != 0 || s[1] != 0) { input.LStick.Dx = s[0]; input.LStick.Dy = s[1]; }
            if (s[2] != 0 || s[3] != 0) { input.RStick.Dx = s[2]; input.RStick.Dy = s[3]; }
        }

        // R5：键盘区共享内存（原先全库没有真实写入方——IHidServer.ActivateKeyboard 只写一条空状态、上游残留的
        //   Ryujinx.Input 键盘路径不被驱动，读键盘区的游戏按什么都没反应）。把宿主前端的原始键态（s2a_kb_read）
        //   每次 HID 提交写一遍，同触屏口径（没有键也写一条空状态，游戏读到的永远是最新的）；
        //   软键盘开着（TextInput.Open）给全零，同上面手柄清零的口径（玩家在打字，按键不进游戏）
        private static readonly ulong[] _kbState = new ulong[4];
        private static void PushKeyboard(Switch device)
        {
            ulong* bits = stackalloc ulong[4]; uint mods;
            _kbRead(bits, &mods);
            bool text = TextInput.Open;
            for (int i = 0; i < 4; i++) _kbState[i] = text ? 0 : bits[i];
            device.Hid.Keyboard.Update(new KeyboardInput { Modifier = text ? 0 : (int)mods, Keys = _kbState });
        }

        // 触屏直通 → 触摸屏共享内存。每次 HID 提交都写一遍（没有触点也要写一条空状态，跟桌面版鼠标路径一致），
        // 否则游戏只能读到开机时 IHidServer 初始化出来的那几条空状态，自带触屏 UI 永远是死的。
        // 触点坐标已由 native 侧换算到「游戏这一帧真实出图的尺寸」×720（未加宽 / 出图 1280 时就是 1280×720，
        // 加宽出图 1564 宽时就是 1564×720，见 vpad.m 的 switchPoint + nxgal_present_frame）。
        // 游戏把触点归一化用的分母就是它自己的渲染尺寸，所以这个空间不能靠「显示宽是多少」去推。
        // 自动玩家的点触接在真触点之后（无窗口的 Mac 普查局只有它）；每次点下（属性带 Start）从共享内存回读当前一条打一行，两头对（触点进了游戏读的那块）
        // 这里原样写共享内存、不裁剪；上限 16 个手指（触摸屏共享内存 Array16 的容量）。
        private static void PushTouch(Switch device)
        {
            OfferTouchConfigured(device.Hid.Touchscreen.ConfiguredMode);
            OfferTouchWanted(device.Hid.Touchscreen.Active);
            if (_vpadTouch == null && _autoplayTouch == null) return;
            const int MaxTouches = 16;
            int* xy = stackalloc int[MaxTouches * 2];
            uint* attr = stackalloc uint[MaxTouches];
            int count = _vpadTouch != null ? Math.Min(_vpadTouch(xy, attr), MaxTouches) : 0;
            int ap = count < MaxTouches && _autoplayTouch != null ? _autoplayTouch(xy + 2 * count, attr + count) : 0;
            count += ap;
            Span<TouchPoint> points = stackalloc TouchPoint[MaxTouches];
            for (int i = 0; i < count; i++)
            {
                points[i] = new TouchPoint
                {
                    Attribute = (TouchAttribute)attr[i],
                    X = (uint)xy[2 * i], Y = (uint)xy[2 * i + 1],
                    DiameterX = 10, DiameterY = 10, Angle = 90,   // 与 Ryujinx 鼠标路径的占位值一致
                };
            }
            device.Hid.Touchscreen.Update(points[..count]);
            FeedGesture(device, xy, attr, count);
            if (ap != 0 && (attr[count - 1] & (uint)TouchAttribute.Start) != 0)
            {
                ref TouchScreenState cur = ref device.Hid.SharedMemory.TouchScreen.GetCurrentEntryRef();
                TouchState t0 = cur.TouchesCount > 0 ? cur.Touches[(int)cur.TouchesCount - 1] : default;
                Console.Error.WriteLine($"HID：自动玩家点触 ({xy[2 * (count - 1)]}, {xy[2 * (count - 1) + 1]}) → 触摸屏共享内存：触点 {cur.TouchesCount} 个，最后一个 ({t0.X}, {t0.Y}) 属性 {t0.Attribute}，采样号 {cur.SamplingNumber}");
            }
        }

        // 触点 → HID 手势区（#568：太阁立志传Ⅴ DX 调 ActivateGesture 后只看手势区，苹果端原先从不写，纯触屏点了没反应）。
        //   与安卓同一个识别器（TouchGestureState）；只数没抬起的触点（属性带 End 的那一帧已抬手），时间取单调时钟。
        //   第一次有手指按下才建手势区（GestureDevice 构造时清空那块），没碰过屏幕的局不动它；游戏换了 Switch 实例就重建
        private static TouchGestureState _gesture;
        private static Switch _gestureOwner;
        private static void FeedGesture(Switch device, int* xy, uint* attr, int count)
        {
            int down = 0, first = -1;
            for (int i = 0; i < count; i++)
                if ((attr[i] & (uint)TouchAttribute.End) == 0) { if (first < 0) first = i; down++; }
            if (_gesture == null || _gestureOwner != device)
            {
                if (down == 0) return;
                var gestures = new GestureDevice(device);
                _gestureOwner = device;
                _gesture = new TouchGestureState(g => gestures.Update(g), m => Console.Error.WriteLine("HID 手势：" + m));
                Console.Error.WriteLine("HID 手势：开始写手势区（单点 点按 / 长按 / 拖动；多点取消）");
            }
            ulong ns = (ulong)((UInt128)(ulong)System.Diagnostics.Stopwatch.GetTimestamp() * 1_000_000_000UL / (ulong)System.Diagnostics.Stopwatch.Frequency);
            _gesture.FeedContacts(down, first < 0 ? 0u : (uint)first, down == 1 ? (uint)xy[2 * first] : 0u, down == 1 ? (uint)xy[2 * first + 1] : 0u, ns);
        }

        // 自动玩家键 → 手柄状态：位 0–15 是按键，16–23 是左右摇杆四向伪键，换成满偏摇杆（按键位同时保留，Ryujinx 同序）
        public static void Merge(ref GamepadInput input, uint keys)
        {
            const int Full = short.MaxValue;
            input.Buttons |= (ControllerKeys)keys;

            if ((keys & (1u << 16)) != 0) input.LStick.Dx = -Full;
            if ((keys & (1u << 18)) != 0) input.LStick.Dx = Full;
            if ((keys & (1u << 17)) != 0) input.LStick.Dy = Full;
            if ((keys & (1u << 19)) != 0) input.LStick.Dy = -Full;
            if ((keys & (1u << 20)) != 0) input.RStick.Dx = -Full;
            if ((keys & (1u << 22)) != 0) input.RStick.Dx = Full;
            if ((keys & (1u << 21)) != 0) input.RStick.Dy = Full;
            if ((keys & (1u << 23)) != 0) input.RStick.Dy = -Full;
        }

        // 本地多手柄（Mac 游戏窗口双人 / 四人分屏）：宿主导出 s2a_vpad_read_pad、开了 ≥2 格、且确实有 ≥2 只手柄在线
        //   才各喂各的；没导出（iOS 壳 / 安卓 / 旧宿主）保持原路径，第二格继续镜像第一格。
        //   live 必须一起看：_pads 不只由手柄数决定——游戏的手柄设置小程序要「至少 2 人」时会 SetPads(2)，
        //   只接一只手柄 _pads 也是 2。那时若去读槽 1（没人）等于全零，一只手柄就过不了那道界面（原先是镜像）。
        internal static bool UsePerPadInput(bool padExport, int pads, PlayerIndex second, int live)
            => padExport && pads > 1 && live >= 2 && second != PlayerIndex.Unknown;

        // 二号起按槽位各构造一条（槽 0 是玩家 1，外面已填好）。纯逻辑：只读各槽的按键 / 摇杆，
        //   摇杆四向伪键由调用方用 device.Hid.UpdateStickButtons 补，单测因此不需要真机。
        // TODO(多手柄 + 震动)：反馈目前只跟「当前主手柄」走，玩家 2 的震动应该发给 2 号手柄。
        //   等按槽位的震动导出合入后再接，这一版先不扩散改动面。
        internal static void FillPadStates(List<GamepadInput> states, int pads, PlayerIndex second,
                                           delegate* unmanaged<int, int*, uint> readPad, uint keys2)
        {
            while (states.Count > 1) states.RemoveAt(states.Count - 1);
            PlayerIndex slot = second;
            int* ps = stackalloc int[4];
            for (int k = 1; k < pads; k++)
            {
                GamepadInput extra = new() { PlayerId = slot };
                extra.Buttons = (ControllerKeys)readPad(k, ps);
                extra.LStick.Dx = ps[0]; extra.LStick.Dy = ps[1];
                extra.RStick.Dx = ps[2]; extra.RStick.Dy = ps[3];
                if (k == 1 && keys2 != 0) Merge(ref extra, keys2);   // 自动玩家接管二号手柄时照旧并它的「2:键」
                if (states.Count <= k) states.Add(extra); else states[k] = extra;
                slot = slot == PlayerIndex.Player8 ? PlayerIndex.Player1 : slot + 1;   // 与 SetPrimary 同一套回绕
            }
        }

        // 单只 Joy-Con 横握（11-03 归族 E：010036B0034E4000 手柄设置小程序只收单只、游戏 SetNpadJoyHoldType 横握）：HID 报的是这只的物理键，
        //   游戏按横握自己转方向——右只横握（导轨朝上 = 顺时针转 90°）时屏幕右边的「确认」是物理 X、下边是物理 A；左只（逆时针 90°）时 ABXY 是十字键那四个。
        //   触屏手柄与自动玩家给的 ABXY / 摇杆都是「横握后看到的位置」，这里换成物理键 / 物理摇杆方向，人和自动玩家按 A 都是确认
        // 自动玩家的键 / 摇杆并进这一帧的输入，再按横握换成物理键（审计 P1-94）：触屏手柄与自动玩家给的是「横握后看到的位置」，要转；
        //   外接实体手柄（iOS 事件快照来源 1 / 2：BLE / 系统手柄）报的本来就是物理键，原先整份一起转，实体手柄横握时 A 变成 X、摇杆转 90°。
        //   physical = 人手这份来自实体手柄：只转自动玩家那份再并进来（键位或上、它给了的摇杆覆盖）
        internal static void ComposeAutoplay(ref GamepadInput input, bool physical, uint keys, bool sticks, int lx, int ly, int rx, int ry,
                                             ControllerType type, Ryujinx.HLE.HOS.Services.Hid.Types.NpadJoyHoldType hold)
        {
            if (!physical)
            {
                Merge(ref input, keys);
                if (sticks) { input.LStick.Dx = lx; input.LStick.Dy = ly; input.RStick.Dx = rx; input.RStick.Dy = ry; }
                RotateSideways(ref input, type, hold);
                return;
            }
            GamepadInput ap = new() { PlayerId = input.PlayerId };
            Merge(ref ap, keys);
            if (sticks) { ap.LStick.Dx = lx; ap.LStick.Dy = ly; ap.RStick.Dx = rx; ap.RStick.Dy = ry; }
            RotateSideways(ref ap, type, hold);
            input.Buttons |= ap.Buttons;
            if (sticks || (keys & 0xFu << 16) != 0) input.LStick = ap.LStick;
            if (sticks || (keys & 0xFu << 20) != 0) input.RStick = ap.RStick;
        }

        internal static void RotateSideways(ref GamepadInput input, ControllerType type, Ryujinx.HLE.HOS.Services.Hid.Types.NpadJoyHoldType hold)
        {
            if (hold != Ryujinx.HLE.HOS.Services.Hid.Types.NpadJoyHoldType.Horizontal || (type != ControllerType.JoyconRight && type != ControllerType.JoyconLeft)) return;
            const ControllerKeys Face = ControllerKeys.A | ControllerKeys.B | ControllerKeys.X | ControllerKeys.Y;
            ControllerKeys b = input.Buttons, f = 0;
            bool right = type == ControllerType.JoyconRight;
            if ((b & ControllerKeys.A) != 0) f |= right ? ControllerKeys.X : ControllerKeys.DpadDown;
            if ((b & ControllerKeys.B) != 0) f |= right ? ControllerKeys.A : ControllerKeys.DpadLeft;
            if ((b & ControllerKeys.X) != 0) f |= right ? ControllerKeys.Y : ControllerKeys.DpadRight;
            if ((b & ControllerKeys.Y) != 0) f |= right ? ControllerKeys.B : ControllerKeys.DpadUp;
            input.Buttons = (b & ~Face) | f;
            // 横握后的摇杆方向 (x, y) → 物理：右只顺时针转了 90°，物理 = (−y, x)；左只逆时针，物理 = (y, −x)。
            //   右只的物理摇杆是右摇杆（游戏读 AnalogStickR）：触屏 / 自动玩家写在左摇杆槽的，迁到右摇杆再转（#592 lucaAce1024 实机）
            if (right)
            {
                if (input.RStick.Dx == 0 && input.RStick.Dy == 0 && (input.LStick.Dx != 0 || input.LStick.Dy != 0)) { input.RStick = input.LStick; input.LStick = default; }
                var r = input.RStick; int rx = r.Dx, ry = r.Dy;
                (r.Dx, r.Dy) = (-ry, rx);
                input.RStick = r;
            }
            else
            {
                var s = input.LStick; int x = s.Dx, y = s.Dy;
                (s.Dx, s.Dy) = (y, -x);
                input.LStick = s;
            }
        }

        // 摇杆诊断（S2A_TRACE_STICKS=1）：给入的摇杆一变就打一行，并从共享内存（游戏读的就是这块）回读当前样式与掌机 / 全键两路最新一条，两头对一下。
        // 缺省关：真玩时摇杆几乎每帧都变，每次一行长日志（分配 + 控制台写）
        private static readonly bool TraceSticksOn = Environment.GetEnvironmentVariable("S2A_TRACE_STICKS") == "1";
        private static (int, int, int, int) _lastSticks;

        private static void TraceSticks(Switch device, in GamepadInput input)
        {
            (int, int, int, int) now = (input.LStick.Dx, input.LStick.Dy, input.RStick.Dx, input.RStick.Dy);

            if (now == _lastSticks)
            {
                return;
            }

            _lastSticks = now;
            ref var npad = ref device.Hid.SharedMemory.Npads[(int)input.PlayerId].InternalState;
            ref var hh = ref npad.Handheld.GetCurrentEntryRef();
            ref var fk = ref npad.FullKey.GetCurrentEntryRef();
            Console.Error.WriteLine($"HID：给入 左摇杆 Dx={input.LStick.Dx} Dy={input.LStick.Dy} 右摇杆 Dx={input.RStick.Dx} Dy={input.RStick.Dy}；" +
                $"共享内存回读 样式 {npad.StyleSet}、支持样式 {device.Hid.Npads.SupportedStyleSets}、" +
                $"掌机 #{hh.SamplingNumber} 左 ({hh.AnalogStickL.X},{hh.AnalogStickL.Y}) 右 ({hh.AnalogStickR.X},{hh.AnalogStickR.Y}) 属性 {hh.Attributes}、" +
                $"全键 #{fk.SamplingNumber} 左 ({fk.AnalogStickL.X},{fk.AnalogStickL.Y})");
        }

        // 只保存按键的 HID 可见窗；摇杆不经过这里。Commit 后才开始计时。
        private sealed class ButtonVisibility
        {
            private readonly ulong[] _until = new ulong[16];
            private uint _submitted, _next, _newlyPressed;
            private ulong _epoch;
            private int _configuration;

            public uint Apply(uint buttons, ulong epoch, int configuration, ulong now)
            {
                if (_epoch != epoch || _configuration != configuration)
                {
                    Array.Clear(_until);
                    _submitted = 0;
                    _epoch = epoch;
                    _configuration = configuration;
                }
                _newlyPressed = buttons & ~_submitted;
                _next = buttons;
                for (int bit = 0; bit < _until.Length; bit++)
                {
                    uint mask = 1u << bit;
                    if ((_submitted & mask) != 0 && now < _until[bit]) _next |= mask;
                }
                return _next;
            }

            public void Commit(ulong submittedAt)
            {
                for (int bit = 0; bit < _until.Length; bit++)
                {
                    if ((_newlyPressed & (1u << bit)) != 0) _until[bit] = submittedAt + 20_000;
                }
                _submitted = _next;
            }

            public ulong Deadline(uint physicalButtons, ulong now, ulong heartbeat)
            {
                ulong deadline = heartbeat;
                for (int bit = 0; bit < _until.Length; bit++)
                {
                    if ((_submitted & ~physicalButtons & (1u << bit)) != 0 &&
                        _until[bit] < deadline) deadline = Math.Max(_until[bit], now);
                }
                return deadline;
            }
        }

        private static void Loop(Switch device)
        {
            List<GamepadInput> states = new(MaxPads) { default };   // 最多 MaxPads 格（本地多人），一次分配好，不在每帧扩容
            // 每键在第一次 HID 提交后保持 20 ms，让正常 60 Hz 轮询能看到短按。
            // 同键快速连点仍可能合并；不承诺卡帧或低频采样游戏一定读到。
            ButtonVisibility visibility = new();
            ulong nextHeartbeat = 0;
            PadSnapshot uncommitted = default;
            int configuration = Volatile.Read(ref _configurationGeneration);
            int* st = stackalloc int[4];
            long nextFollow = 0;

            while (!_stop)
            {
                Ryujinx.Common.S2AThreadAlloc.Tick("输入");
                Lifecycle.Tick(Host.Device?.System);   // 焦点 / 底座变化转给游戏（接口 v2.15）
                if (AutoplayPads != null && AutoplayPads() > _pads) SetPads(device, AutoplayPads());   // 先改配再取主手柄号：两人时主手柄离开掌机格
                // 本地多手柄：Mac 前端报告接了几只手柄就开到几格（最多 MaxPads），接上第二只手柄即成为玩家 2
                int livePads = _vpadPadCount != null ? _vpadPadCount() : 0;
                if (livePads > _pads && livePads <= MaxPads) SetPads(device, livePads);
                if (Environment.TickCount64 >= nextFollow) { FollowNow(device); nextFollow = Environment.TickCount64 + 250; }

                GamepadInput input = new() { PlayerId = _player };
                PadSnapshot snapshot = default;
                bool eventInput = _padReadEx != null && (nint)InputHook == (nint)_vpadHook;
                bool heartbeat = true;
                uint hostButtons = 0;

                if (eventInput)
                {
                    _padReadEx(&snapshot);
                    if ((OperatingSystem.IsIOS() || OperatingSystem.IsTvOS()) && !_externalController && snapshot.Source is 1 or 2)   // 平台例外：tvOS 同 iOS 走触屏 / 手柄来源判断（Apple TV 一期，遥控器按触屏来源上报）
                    {
                        // 只在首次真实 BLE / 系统手柄出现时改配；断线或后台的触屏快照不撤销偏好。
                        _externalController = true;
                        if (_player == PlayerIndex.Handheld)
                            SetPrimary(device, PlayerIndex.Player1, ControllerType.ProController);
                    }
                    input.PlayerId = _player;
                    if (snapshot.NotificationSeq != uncommitted.NotificationSeq || snapshot.HostTimestampUs != 0)
                        uncommitted = snapshot;
                    ulong now = _padNow();
                    configuration = Volatile.Read(ref _configurationGeneration);
                    hostButtons = visibility.Apply(snapshot.Buttons, snapshot.InputEpoch, configuration, now);
                    input.Buttons = (ControllerKeys)hostButtons;
                    input.LStick.Dx = snapshot.LeftX; input.LStick.Dy = snapshot.LeftY;
                    input.RStick.Dx = snapshot.RightX; input.RStick.Dy = snapshot.RightY;
                    heartbeat = now >= nextHeartbeat;
                    if (heartbeat)
                    {
                        // 固定时间轴；活动输入不能不断推迟反馈心跳。
                        nextHeartbeat = nextHeartbeat == 0 ? now + 16_000 :
                            nextHeartbeat + ((now - nextHeartbeat) / 16_000 + 1) * 16_000;
                    }
                }
                else if (InputHook != null)
                {
                    InputHook(ref input);
                }

                // 自动玩家接管了二号手柄（s2a_autoplay_pads ≥ 2）：二号只听自动玩家的「2:键」，自动玩家给一号的键不镜像过去
                //   （11-01 双人成行选角色：镜像时一号往左二号也往左，两人永远挤在同一个角色）；真人输入（触屏 / 外接手柄）照旧镜像
                GamepadInput human = input;
                uint apKeys = AutoplayKeys1 != null ? AutoplayKeys1() : 0;
                bool apSticks = _autoplaySticks != null && _autoplaySticks(st) != 0;

                // 先选定真实外接手柄槽，再安装反馈 hook；游戏看到新样式时即可同步提交震动，
                // 首次 READY 时保留手柄自己的固件连接反馈。
                RefreshFeedback(device);
                // 摇杆四向按键位（StickL/R 左右上下，位 16–23）按摇杆位置每帧置，同 Ryujinx 输入层（NpadManager → Hid.UpdateStickButtons，半偏阈值）：
                //   真机 HID 也是这样报，只读这几个位的游戏（菜单、2D 移动）推模拟摇杆才有反应。触屏 / 外接手柄 / 摇杆探测都只给模拟量
                ComposeAutoplay(ref input, eventInput && snapshot.Source is 1 or 2, apKeys, apSticks, st[0], st[1], st[2], st[3], _type, device.Hid.Npads.JoyHold);
                input.Buttons |= device.Hid.UpdateStickButtons(input.LStick, input.RStick);
                states[0] = input;
                PlayerIndex second = _second;
                // 本地多手柄：Mac 前端按槽位各喂一只（槽 0 = 玩家 1），二号起读自己的按键 / 摇杆，不再镜像一号
                bool realPads = UsePerPadInput(_vpadReadPad != null, _pads, second, livePads);
                if (realPads)
                {
                    FillPadStates(states, _pads, second, _vpadReadPad, AutoplayKeys2 != null ? AutoplayKeys2() : 0);
                    for (int k = 1; k < states.Count; k++)
                    {
                        GamepadInput extra = states[k];
                        extra.Buttons |= device.Hid.UpdateStickButtons(extra.LStick, extra.RStick);
                        states[k] = extra;
                    }
                }
                else if (second != PlayerIndex.Unknown)
                {
                    // 从按槽位退回镜像（在线手柄掉到 1 只以下）：多出来的格要撤掉——留着会冻在上一帧的输入上，
                    //   游戏以为玩家 3 / 4 还按着。留 2 格，第 2 格原地换成镜像。
                    while (states.Count > 2) states.RemoveAt(states.Count - 1);
                    bool driverOwnsSecond = AutoplayPads != null && AutoplayPads() >= 2;
                    GamepadInput mirror = driverOwnsSecond ? human : input;
                    mirror.PlayerId = second;
                    uint keys2 = AutoplayKeys2 != null ? AutoplayKeys2() : 0;
                    if (driverOwnsSecond || keys2 != 0)
                    {
                        Merge(ref mirror, keys2);
                        mirror.Buttons |= device.Hid.UpdateStickButtons(mirror.LStick, mirror.RStick);
                    }
                    if (states.Count < 2) states.Add(mirror); else states[1] = mirror;
                }
                else while (states.Count > 1) states.RemoveAt(states.Count - 1);
                // 系统键盘开着（TextInput）：玩家在打字，手柄 / 触屏手柄的输入不进游戏（确定键 A 误触会让游戏把半截名字当确定、摇杆误移光标）；
                //   按着的键在开框这一帧起按松开报，收框后照常
                if (TextInput.Open)
                    for (int i = 0; i < states.Count; i++) states[i] = new GamepadInput { PlayerId = states[i].PlayerId };
                device.Hid.Npads.Update(states);
                if (_kbRead != null) PushKeyboard(device);   // R5 键盘区：宿主喂了才写（没喂的宿主同改前）
                Cheats.SetKeys(states[0].Buttons);   // 金手指按键条件码看一号手柄（与 Ryujinx TamperMachine 同口径）
                // A platform hook owns the HID touch ring (Android stable IDs);
                // Apple uses the shared runtime/autoplay source when no hook is installed.
                if (TouchHook != null) TouchHook(device);
                else PushTouch(device);
                if (eventInput)
                {
                    visibility.Commit(_padNow());
                    // 早释放的统计等到真正提交释放；可见窗引入的等待不能被统计隐藏。
                    if (hostButtons == snapshot.Buttons && uncommitted.HostTimestampUs != 0)
                    {
                        _padCommit(&uncommitted);
                        uncommitted.HostTimestampUs = 0;
                    }
                }
                if (heartbeat) _feedback?.Update(input.PlayerId);
                else _feedback?.TickMotion(input.PlayerId);
                if (TraceSticksOn)
                {
                    TraceSticks(device, in input);
                }
                if (eventInput)
                {
                    ulong deadline = visibility.Deadline(snapshot.Buttons, _padNow(), nextHeartbeat);
                    // Stop/改配可能发生在本轮 read 后、wait 前，显式检查并由序号兜住此窗口。
                    if (!_stop && configuration == Volatile.Read(ref _configurationGeneration))
                        _padWait(snapshot.NotificationSeq, deadline);
                }
                else Thread.Sleep(16);
            }
        }
    }
}
