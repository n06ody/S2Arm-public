#!/usr/bin/env bash
# Device-free CPU tests: C/C++, CMake 3.18+, JDK 17, Android NDK 29.0.14206865,
# pinned offline rewriter sources and Python Capstone 5.0.7 / LZ4 4.4.5.
set -euo pipefail
android_dir=$(cd "$(dirname "$0")" && pwd)
repo_dir=$(cd "$android_dir/.." && pwd)
# 超时用仓库的 perl 版 tools/bin/timeout（#591，同 tools/toolchain.sh / graphics/gate.sh）：直接 bash android/test.sh 时
# 子脚本（如 core-runtime/tests/run_shutdown_thread.sh）裸调 timeout，macOS 没有 GNU timeout 就 127。
source "$repo_dir/tools/_timeout.sh"
# 输出目录按运行隔离（#919）：同树两份门禁并行、或上一次运行被中断时，
# 固定目录 build/android-host-tests 里的半写 .class / 半写 boot-spec 会被当成输入，
# 红成 java.lang.ClassFormatError: Truncated class file / JSONDecodeError。
# 这里每次一个独有目录，退出即删；不重试、不放宽超时。
source "$android_dir/tools/host_out.sh"
out=$(s2a_host_out) || { echo '【红】建不出主机测试输出目录（S2A_HOST_OUT_ROOT / TMPDIR 不可写）' >&2; exit 1; }
trap 'rm -rf "$out"' EXIT
# 原生段的 CMake：$CMAKE → PATH 上的 cmake → SDK cmake/*/bin/cmake 里版本最高的（tools/cmake_pick.sh，#1568 定）
source "$android_dir/tools/cmake_pick.sh"
# 与 gate.sh / 共享改写器门禁同选固定解释器；PY 保留显式开发入口。
# gate.sh 已前置核验版本和扩展，所有 Python 子进程沿用同一个选择。
if [ "${S2A_PY+x}" ]; then
    PY=$S2A_PY
elif [ ! "${PY+x}" ]; then
    PY="$HOME/.s2arm/venv/bin/python3"
fi
export PY S2A_PY="$PY"
# The platform gate installs this helper; direct test.sh keeps original streams.
source "$android_dir/tools/gate_step.sh"
# Audio focus policy: lifecycle, denial and late callbacks without Android hardware.
s2a_gate_step mkdir -p "$out/java"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/AudioFocusPolicy.java" \
    "$android_dir/tests/AudioFocusPolicyTest.java"
s2a_gate_step java -ea -cp "$out/java" org.s2arm.android.AudioFocusPolicyTest
# Diagnostic archive preflight: synthetic names and sizes only; never reads logs.
s2a_gate_step bash "$android_dir/tests/diagnostics/run.sh"
# Capacity selection is shared by the production Android host implementation.
s2a_gate_step "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror "$android_dir/tests/capacity/policy_test.cpp" -o "$out/capacity-policy-test"
s2a_gate_step "$out/capacity-policy-test"
# Linux 专项验证真实亲和、继承及音频路径；macOS 由 Linux 门禁执行。
if [ "$(uname -s)" = Linux ]; then
    s2a_gate_step bash "$android_dir/tests/capacity/run_linux.sh" "$out/capacity"
fi
s2a_gate_step bash android/cs/video/tests/run.sh
# T117: production scratch leases and MemoryOwner fallback, CPU-only / offline.
s2a_gate_step bash "$android_dir/core-link/runtime-tests/buffer-scratch/run.sh"
# Buffer 正常上传读取（生产 Buffer.SyncAction → LoadRegion → SetData → Memmove，Mac VM 原生夹具；只在 macOS 跑，11-01 #894 接线）
s2a_gate_step bash "$android_dir/core-link/runtime-tests/buffer-read/run.sh"
# 原生 provider 归属与可选导出（显式绑定 / 苹果老入口共 11 个隔离进程，含补时两导出同一查找口径；11-01 #894 接线）
s2a_gate_step bash "$android_dir/core-link/provider-tests/run.sh"
# 内存层（宿主，fake Vulkan，无设备）：能力探测 / UPLOAD 分配与 flush-invalidate 语义 / host 导入五类拒绝 /
# 错误路径不留半分配 / destroy 顺序 / 双向自检（含坏拷贝负对照）。
s2a_gate_step bash "$android_dir/tools/vk_membuf_test.sh"
# 资源账本 + VM 替页事务（宿主，确定性反例）：地址/后备身份/内容版本、单调提交号幂等保活、
# 退休/在途引用、事务失败逐阶段回滚并恢复字节与权限。
s2a_gate_step bash "$android_dir/tools/vk_resid_test.sh"
# 遮挡查询环形页边界（宿主，Vulkan 调桩）：reset 只重置高水位 / 同批多 pass 累计在 pass 边界合法换页 /
# 段日志压缩 / 长期不 Reset / 会话重建。这是「进世界约 20 s 必崩」那类窗口失效的回归护栏。
s2a_gate_step bash "$android_dir/tools/nvn_vk_query_edges.sh"
# 描述符写入初始化（宿主）：三路描述符必须整结构清零、dstArrayElement=0；「漏字段」反例必须被抓到。
# 曾经漏填 dstArrayElement，真机第一笔 draw 就把 Adreno 驱动写崩。
s2a_gate_step bash "$android_dir/tools/nvn_vk_desc_write_test.sh"
# 反馈快照池复用/淘汰判据：在途帧的槽不可复用、不可淘汰；完成证明到手后才可复用。
s2a_gate_step bash "$android_dir/tools/nvn_vk_snap_pool_test.sh"
# 描述符 set 帧内复用：命中条件必须严格（不同 offset/range、布局/池代际变化、跨帧、哈希碰撞都不命中）。
s2a_gate_step bash "$android_dir/tools/nvn_vk_desc_cache_test.sh"
# 纹理子分配：块归属（释放 B 不能回填到 A）、对齐（512 请求不能返回 256）、切分余量、跨块不串、满块失败。
# 第一版生产代码在这里错过：先搬末条覆盖本条、再读本条偏移 → 两张在用的纹理落到同一段内存。
s2a_gate_step bash "$android_dir/tools/nvn_vk_tex_suballoc_test.sh"
# 程序结果里 bindings 超出容量必须准确失败（F1：绝不静默截断）；含撤守卫的反例说明。
s2a_gate_step bash "$android_dir/tools/nvn_vk_program_binding_cap.sh"
# AHB 池接入（宿主，替身只给外部依赖）：迁移必须有 owner 证明、整池迁移后可命中、提交号单调 / 不同号互不影响 /
# 同号塌成一项 / 池删除后在途引用清空前不销毁 / 重建不继承 done/failed / 早退也消费 owner 授权 / 统计口径自洽。
s2a_gate_step bash "$android_dir/tools/vk_poolahb_core_test.sh"
# NVN ReadTexels 的几何/打包换算（纯逻辑，宿主）：区域/子资源/行距层距/块对齐/越界原因。
s2a_gate_step bash "$android_dir/tools/nvn_vk_readback_plan_test.sh"
# 批 A：内容版本与 CPU/GPU 交接（真 Vulkan / lavapipe）：GPU 清红 → CPU 整图写蓝 / 局部写蓝、内容 7→11
# 的两次 draw 各看各的版本；子资源状态、读回不写回 guest、账本不可用时 fail-closed、1 MiB 保护块不是写粒度。
# （nvn_vk_texcache_content_test.sh 要 lavapipe，挪到 android/linux_ctest.sh 末段在 Linux 跑，11-06）
# 反馈采样快照的子资源拷贝（真 Vulkan / lavapipe）：3D 深度切片（含 mip≠0）、2D 数组层、
# pass/快照交集；旧实现把 3D 切片当 array layer 选 → 越界/读错片。
# （nvn_vk_feedback_snapshot_test.sh 要 lavapipe，挪到 android/linux_ctest.sh 末段在 Linux 跑，11-06）
# 压缩纹理作为渲染目标（块=像素影子）的字节往返（真 Vulkan / lavapipe）：BC4 3D 深度切片、
# 2D 数组 mip/层、影子直写；Vulkan 不能把压缩格式当颜色附件，也不能建压缩↔无压缩视图。
# （nvn_vk_bcrt_test.sh 要 lavapipe，挪到 android/linux_ctest.sh 末段在 Linux 跑，11-06）
# 可写 graphics storage buffer 的帧内影子 / 退休回写 / 零附件 pass / 驱动 c0 布局（真 Vulkan / lavapipe）。
# （nvn_vk_ssbo_guest_test.sh 要 lavapipe，挪到 android/linux_ctest.sh 末段在 Linux 跑，11-06）
# 覆盖判定（哪些 guest 背板可替页）、选池策略、受控别名登记。
s2a_gate_step bash "$android_dir/tools/vk_poolahb_cover_test.sh"
s2a_gate_step bash "$android_dir/tools/vk_poolahb_policy_test.sh"
s2a_gate_step bash "$android_dir/tools/vk_poolahb_alias_test.sh"
# 帧时间线（宿主，确定性）：事件按 frame ID 配对、缺样本「未测」、重复/跨帧丢弃计数、五段闭环。
s2a_gate_step bash "$android_dir/tools/nvn_vk_timeline_test.sh"
# 直连逐帧帧时间（宿主，确定性）：环形语义、丢弃/计数可见、分片转储字节级核回。
s2a_gate_step bash "$android_dir/tools/nvn_vk_frame_log_test.sh"
# B2 VK_GOOGLE_display_timing（宿主，确定性）：挂 id/回读、乱序对号、丢帧 UNKNOWN、
# 迟到补账、门关闭、ring 环覆、detach 复位、last_gap 最新确认对。
s2a_gate_step bash "$android_dir/tools/nvn_vk_dt_test.sh"
# 直连逐帧 TSV（宿主，确定性）：列掩码/差分/gpuq 账/开关与目录门。
s2a_gate_step bash "$android_dir/tools/nvn_vk_frame_tsv_test.sh"
s2a_gate_step bash "$android_dir/tools/generic_frame_trace_test.sh"
# #48 A5（宿主，确定性）：BatteryManager 净放电换算与不可用状态、采样器停采/迟到回调/读数开关。
s2a_gate_step bash "$android_dir/tools/system_power_test.sh"
# #48 A3+A4（宿主，确定性）：Surface.setFrameRate 投票边界（去重/未知清票/API<30）、
# 轮询接线（周期→票、省电档→cap、开关、停后迟到回调不采）、生产 present_period 的封顶与登记。
s2a_gate_step bash "$android_dir/tools/frame_rate_vote_test.sh"
s2a_gate_step bash "$android_dir/tools/system_tuning_test.sh"
# #48 P8（宿主，确定性）：热状态映射与 dm_thermal_now / dm_low_power_now 强定义、采样器起停 / 只在变化时推。
s2a_gate_step bash "$android_dir/tools/power_state_test.sh"
# 转储解析器（宿主，确定性）：分片拼回/完整性拒绝、全窗统计、与 16 帧聚合行对拍、C→python 对拍。
s2a_gate_step "$PY" "$android_dir/tests/direct_framelog_test.py"
# 设备锁与起局最小闭环（宿主，确定性；adb 替身不起真机）：锁并发/失效/留置、还原失败保留锁、
# 局目录被 tools/quality_check.py 安卓分支判读。
s2a_gate_step "$PY" "$android_dir/tests/device_lock_test.py"
s2a_gate_step "$PY" "$android_dir/tests/run_game_test.py"
# run_game_test 的用例收场后不留临时目录（通过 / 失败 / setUp 中途出错三种收场）。
s2a_gate_step "$PY" "$android_dir/tests/run_game_tmp_cleanup_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_game_mode_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_refresh_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_touch_generation_test.py"
# 通用模式编译事件：原生文件生成、丢弃负控及读取器完整性校验。
s2a_gate_step "$PY" "$android_dir/tests/generic_compile_events_test.py"
s2a_gate_step "$PY" "$android_dir/tests/compile_events_reader_test.py"
# 每线程缓冲（备忘槽/暂存的堆化）：线程退出后归还、同线程复用、扩容保留内容。
s2a_gate_step bash "$android_dir/tools/nvn_vk_threadbuf_test.sh"
# CMake 挑选规则的宿主单测（假 cmake，可移植，始终跑）
s2a_gate_step bash "$android_dir/tools/cmake_pick_test.sh"
# S2A_TEST_NO_NATIVE 只跳过宿主 ctest 阶段；可移植改写器矩阵始终执行。
# 非 Linux 宿主：CMakeLists 按 CMAKE_SYSTEM_NAME 门掉 Linux 专属 API 的目标
# （memory.c 的 mremap；guest_signal.c 的 SIGRTMIN/sigcontext.h/ucontext；
# host_android.c 的 eventfd；wsi_lease.c 的 pthread_condattr_setclock；
# direct/selection.c 的 getrandom；tsd.c 与 module_test.c 的 /proc/self/maps），
# 这里显式登记未测项，不静默当绿。
if [ -z "${S2A_TEST_NO_NATIVE:-}" ]; then
    s2a_cmake_pick || { s2a_gate_note "【红】安卓宿主测试找不到 CMake（见上一行）"; exit 1; }
    echo "CMake：${S2A_CMAKE}（${S2A_CMAKE_FROM}）；ctest：$S2A_CTEST"
    cmake_bin=$S2A_CMAKE
    # 原生段的子测试有的自己按 PATH 找 cmake（core-link/test_audit.py）：只给 CMake 这三步、只多 cmake / ctest
    #   两个名字（垫片目录）。不把选中目录整个提前、也不改后面各步的 PATH——它可能是 /usr/bin，会换掉 JDK 17 / venv
    s2a_cmake_shim "$out/cmake-shim" || exit 1
    cmake_env=(env "PATH=$out/cmake-shim:$PATH")
    s2a_gate_step "${cmake_env[@]}" "$cmake_bin" -S "$android_dir" -B "$out" -DCMAKE_BUILD_TYPE=Debug
    s2a_gate_step "${cmake_env[@]}" "$cmake_bin" --build "$out" --parallel 2
    s2a_gate_step "${cmake_env[@]}" "$cmake_bin" -E chdir "$out" "$S2A_CTEST" --output-on-failure --timeout 30
    [ "$(uname -s)" = Linux ] ||
        s2a_gate_note "未测：android_memory / android_plat（mremap）、android_guest_signal（SIGRTMIN）、android_host_android（eventfd）、android_wsi_lease（pthread_condattr_setclock）、android_direct_selection（getrandom）、android_tsd / android_module（/proc/self/maps）需 Linux"
fi
s2a_gate_step "$PY" "$android_dir/tests/debug_environment_test.py"
s2a_gate_step "$PY" "$android_dir/tests/debug_gate_test.py"
# 通用层零专用配置（#1357）：native / gfx 源码不得出现具体游戏包名
s2a_gate_step "$PY" "$android_dir/tests/common_layer_no_package_test.py"
s2a_gate_step mkdir -p "$out/java"
s2a_gate_step javac -encoding UTF-8 -Xlint:all -Werror -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/LanDiscoverySession.java" \
    "$android_dir/tests/LanDiscoverySessionTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.LanDiscoverySessionTest
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_browser_test.py"
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_publisher_test.py"
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_jni_test.py"
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_provider_test.py"
# Pure screenshot pixel contract uses the same JDK 17; no SDK/native/Skia dependency.
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/CapturePixels.java" \
    "$android_dir/tests/capture/CapturePixelsTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.CapturePixelsTest
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ThreadedRendererConfig.java" \
    "$android_dir/tests/ThreadedRendererConfigTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.ThreadedRendererConfigTest
# Exercise the production keyboard lifecycle against a controllable host UI.
s2a_gate_step mkdir -p "$out/host-keyboard"
s2a_gate_step javac -encoding UTF-8 -d "$out/host-keyboard" \
    "$android_dir"/tests/host_keyboard_stubs/android/{app,os,text,view,widget,util}/*.java \
    "$android_dir"/tests/host_keyboard_stubs/org/{json,s2arm/android}/*.java \
    "$android_dir/app/src/main/java/org/s2arm/android/HostKeyboard.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/LegacyTextInput.java" \
    "$android_dir/tests/HostKeyboardLifecycleTest.java" \
    "$android_dir/tests/LegacyTextInputTest.java"
s2a_gate_step java -cp "$out/host-keyboard" org.s2arm.android.HostKeyboardLifecycleTest
s2a_gate_step java -cp "$out/host-keyboard" org.s2arm.android.LegacyTextInputTest
# #1456 capprobe：判定合同 + 缓存编排（known-bad / inconclusive 固化 / runtime-lost 回写 / 保留上限）。
# CapProbe 引用 NativeBridge，宿主编译用 tests/stubs 里的替身。
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/tests/stubs/org/s2arm/android/NativeBridge.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/CapProbeVerdict.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/CapProbe.java" \
    "$android_dir/tests/CapProbeVerdictTest.java" \
    "$android_dir/tests/CapProbeStoreTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.CapProbeVerdictTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.CapProbeStoreTest
# #1456 capprobe 进程模型（宿主，无设备）：父进程另一线程持锁跑探测，posix_spawn
# 经系统 linker 起的助手仍在限时内出合同行；同场景下 fork 出的子进程拿不到锁
# 必卡死（负对照，对应门检反馈）。fake linker 断言 exec 目标=linker、argv[1]
# 为「apk!/<entry>」包内 spec（#1482 owner 判据 b）。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror \
    "$android_dir/tests/capprobe_fake_helper.c" -o "$out/capprobe-fake-helper"
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror \
    "$android_dir/tests/capprobe_fake_linker.c" -o "$out/capprobe-fake-linker"
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -I "$android_dir/native/capprobe" \
    "$android_dir/tests/capprobe_spawn_test.c" "$android_dir/native/capprobe/capprobe_host.c" \
    -o "$out/capprobe-spawn-test" -lpthread
s2a_gate_step bash -c '
    mkdir -p "'"$out"'/fakeapk/lib/arm64-v8a" &&
    cp "'"$out"'/capprobe-fake-helper" "'"$out"'/fakeapk/lib/arm64-v8a/libs2a_capprobe.so"'
s2a_gate_step env "CAPPROBE_FAKE_APK=$out/fakeapk" "CAPPROBE_FAKE_LINKER=$out/capprobe-fake-linker" \
    "$out/capprobe-spawn-test"
# #1482 GS 路径决策 + debug.s2a.gs_path 强制开关：两变体——debug 变体带
# S2A_ANDROID_DEBUG_ENV（两道门都活，开关生效），release 变体没有（编译期
# 门死，开关不生效）。属性表与 logcat 由测试桩出，门是真门。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -D__ANDROID__ \
    -DS2A_ANDROID_DEBUG_ENV -I "$android_dir/tests/hostvk" \
    "$android_dir/tests/gs_path_test.c" "$android_dir/native/direct/gs_path.c" \
    "$android_dir/native/capprobe/capprobe_host.c" \
    -o "$out/gs-path-test-debug" -lpthread
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -D__ANDROID__ \
    -I "$android_dir/tests/hostvk" \
    "$android_dir/tests/gs_path_test.c" "$android_dir/native/direct/gs_path.c" \
    "$android_dir/native/capprobe/capprobe_host.c" \
    -o "$out/gs-path-test-release" -lpthread
s2a_gate_step "$out/gs-path-test-debug"
s2a_gate_step "$out/gs-path-test-release"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamepadState.java" \
    "$android_dir/tests/GamepadStateTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GamepadStateTest
# B3（#48）：buffer_geometry 下 SurfaceView 的同比例 frame 取整，与 native publish_touch 同口径
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BufferGeometry.java" \
    "$android_dir/tests/BufferGeometryTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.BufferGeometryTest
# Vulkan 款直连三态（#274）：intent 取值、缺省不是手动直连、与 native direct_choice.h 同值
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/VulkanDirectChoice.java" \
    "$android_dir/tests/VulkanDirectChoiceTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.VulkanDirectChoiceTest "$android_dir/native/direct_choice.h"
s2a_gate_step bash "$android_dir/tests/exit_order_test.sh"
# A0 端到端（合成包 → 改写器 → stage_a0 → current.json → 契约校验 → BootContract）：要 lz4 / capstone（用门禁同一个 ${PY}）+ JDK
PY="$PY" s2a_gate_step bash "$android_dir/tests/stage_a0_test.sh"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameTouch.java" \
    "$android_dir/tests/GameTouchTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameTouchTest
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ThreadedRendererConfig.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/Json.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchLayout.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchPad.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchLayoutStore.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchRouting.java" \
    "$android_dir/tests/TouchPadTest.java" \
    "$android_dir/tests/TouchRoutingTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.TouchPadTest "$repo_dir/ios/layouts/classic-reference.json"
s2a_gate_step java -cp "$out/java" org.s2arm.android.TouchRoutingTest
# 触屏菜单文案：英文默认和 values-zh 必须成对，MainActivity 不再硬编码（#528 第 11 项）
s2a_gate_step "$PY" "$android_dir/tests/touch_strings_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/Json.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootContract.java" \
    "$android_dir/tests/BootContractTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.BootContractTest
# B6-1 F01 data/editor model: reads the existing iOS reference; no SDK or device evidence.
s2a_gate_step "$PY" "$android_dir/tests/strings_translation_test.py"
s2a_gate_step "$PY" "$android_dir/tests/touch_layout_model_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootSpec.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ProcessBirth.java" \
    "$android_dir/tests/BootSpecTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.BootSpecTest "$out/boot-spec"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/RuntimeLibrary.java" \
    "$android_dir/tests/JniAbiFixture.java" \
    "$android_dir/tests/RuntimeLibraryTest.java"
# The JNI ABI hash from source (native/jni_abi.py, baked into the library) must equal the reflection one.
s2a_gate_step java -cp "$out/java" org.s2arm.android.RuntimeLibraryTest \
    "$("$PY" "$android_dir/native/jni_abi.py" "$android_dir/tests/JniAbiFixture.java" | tail -1)"
s2a_gate_step "$PY" "$android_dir/native/jni_abi.py" "$android_dir/app/src/main/java/org/s2arm/android/NativeBridge.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchInputBridge.java" > /dev/null
# Compile the actual Android host-field adapter with a frozen HostApi fixture.
s2a_gate_step "${CC:-cc}" -std=gnu11 -DS2A_UNIT_TEST -DS2A_HOST_FIELDS_ONLY -I "$repo_dir/runtime/include" \
    "$android_dir/tests/host_fields_test.c" "$android_dir/native/plat_android.c" -o "$out/host-fields-test"
s2a_gate_step "$out/host-fields-test"
# NVN→Vulkan 格式映射纯函数：ASTC 全 14 尺寸 × UNORM/SRGB + 压缩块表，未知格式必须 UNDEFINED。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror \
    -I "$android_dir/native/nvn" -I "$repo_dir/graphics/nvn/gfx/nvn" \
    -I "$android_dir/tests/vkhost" -I "$repo_dir/graphics/nvn/third_party/vulkan-headers/include" \
    "$android_dir/tests/nvn_vk_format_test.c" "$android_dir/native/nvn/nvn_vk_format.c" -o "$out/nvn-vk-format-test"
s2a_gate_step "$out/nvn-vk-format-test"
# 时间戳待写队列的纯记账（不依赖 Vulkan）：帧序号 FIFO 退休 / 跨帧槽位环 / validBits 回绕。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -I "$android_dir/native/nvn" \
    "$android_dir/tests/ts_queue_test.c" -o "$out/ts-queue-test"
s2a_gate_step "$out/ts-queue-test"
# VM 夹具端点核对（宿主源码级，mac 也跑）：svc_*.c 新引用的 core 符号漏登记当场红，不等 Linux 真链
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/vm/check_endpoints.py" --out "$out/vm-endpoints"
# 宽屏 screen_metrics 的宿主两步（screen-widen-test / widen-loader-test）11-01 挪进合入前层：tests/android_screen_host_test.sh
case "$(uname -s)" in
  Darwin) s2a_gate_step bash "$repo_dir/graphics/track/tests/drain_readflush/build_run.sh" "$out/drain-readflush" ;;
  Linux)
    s2a_gate_step "$PY" "$android_dir/native/runtime/tracking/tests/run.py" --out "$out/tracking-precise"
    s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/vm/run.py" --out "$out/vm-mirror" ;;

  *) s2a_gate_note "【红】unsupported host for READFLUSH precise regression"; exit 1 ;;
esac
s2a_gate_step "$PY" "$android_dir/tests/runtime_library_activity_test.py"
# #1474：LAUNCHER 无界面跳板——存活→:game task 回前台 / 无局→库页 / 恒 finish 不出 UI；负对照：恒走库页必须红。
s2a_gate_step "$PY" "$android_dir/tests/icon_entry_test.py"
# W6.0 全账解析：已知长帧/GPU 完成后读/present 四态/retained 负例，缺字段必须保持 unavailable。
s2a_gate_step "$PY" "$android_dir/tests/w6_account_test.py"
s2a_gate_step "$PY" "$android_dir/tests/native_bridge_load_test.py"
s2a_gate_step "$PY" "$android_dir/tests/jni_abi_test.py"
s2a_gate_step "$PY" "$android_dir/tests/dev_runtime_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_batch_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_prepare_test.py"
s2a_gate_step "$PY" "$android_dir/tests/host_link_test.py"   # 宿主单测链接闭包：自动补链 / 弱引用放行 / 无提供方点名红 / 替身优先
# Graphics REQUEST: real producer/canonical identity/writer -> unchanged importer, synthetic host only.
s2a_gate_step "$PY" "$android_dir/tests/graphics_request_name_test.py"
# 新回放层：真实图形/计算请求生产入口的贴图池容量、超限与调试读数。
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --withdraw
# GS 脏位、阶段绑定、管线和原始图元拒绝的独立回退反证。
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/gs_replay_mutation_test.py"
# 原创 Maxwell 经生产请求、真实编译器与生产结果解析；GPU 像素由 Vulkan 门禁另验。
s2a_gate_step "$PY" "$android_dir/tests/gs_compiler_chain_test.py"
# W2 compute: real C components with synthetic input / Vulkan mocks, never GPU or device evidence.
s2a_gate_step "$PY" "$android_dir/tests/direct_compute_identity_test.py"
# P0-3：constantBufferUse 宽掩码的 S2AI1 v3/v4 分叉现由计算与图形共用——正例 + 两处撤改必红（纯宿主 C 编译，秒级）。
s2a_gate_step "$PY" "$android_dir/tests/direct_input_identity_mutation_test.py"
# GS 扩容前后 VS / VS+FS / CS 规范身份逐字节一致，96 条事实不截断。
s2a_gate_step "$PY" "$android_dir/tests/direct_input_identity_compat_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_program_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_lookup_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_request_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_pipeline_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_bridge_test.py"
s2a_gate_step "$PY" "$android_dir/tests/compile_export_test.py"
s2a_gate_step "$PY" "$android_dir/tests/compile_export_w2_test.py"
# kindCheck 调试开关只走请求 JSON（DirectShaderCompiler.cs:39 只读 JSON、不读进程 env）：默认构建不带该键，
# 调试门开 + debug.s2a.kind_check=0 才写 "kindCheck":false；撤成 execute 里 setenv 或删掉该键必须红（宿主 C 编译，秒级）。
s2a_gate_step "$PY" "$android_dir/tests/direct_compile_request_host_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_integration_test.py"
# W2 single missing coordinate: standalone synthetic host contract, no provider/device wiring.
s2a_gate_step "$PY" "$android_dir/tests/w2_single_query_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_frozen_producer_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_frozen_chain_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_request_worker_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_adapter_capture_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_observation_core_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_observation_compute_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_observation_draw_test.py"
# C4a 首用判定环：真 nvn_vk_draw.c + 真 compile_worker/nvn_first_use，可控假编译器。
s2a_gate_step "$PY" "$android_dir/tests/nvk_fu_decision_test.py"
# C4b 管线异步缝：真 nvn_vk_first_draw.c TU + 真 pipe_worker/nvn_first_use，假 Vulkan。
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_pipe_async_draw_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_observation_lifecycle_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_observation_adapter_test.py"
# vk_record host regression links the real Linux Vulkan loader; the dispatch itself is mocked.
case "$(uname -s)" in
  Linux)
    s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_clear_enqueue_ownership_test.py"
    s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_dispatch_fq_test.py"
    s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_clear_fv_test.py"
    s2a_gate_step "$PY" "$android_dir/tests/w2_dispatch_snapshot_test.py"
    s2a_gate_step "$PY" "$android_dir/tests/w2_record_chain_test.py" ;;
  *) s2a_gate_note "clear queue ownership / dispatch fq / clear FV lifetime / W2 dispatch snapshot / record chain：未运行（仅 Linux 主机 libvulkan.so.1 组件测试）" ;;
esac
# vk_record 生产入口探针（P0-0b DrawTexture 准确拒绝 / P0-0b' Deferred 句柄执行时读取）：只链生产
# vk_record.c 与其直接依赖，vk* 导入由宿主 Vulkan loader 解析（不建 device、不开 ICD）。只在
# Linux 且 loader 存在时跑；其它机器记「未运行」并写原因（直接跑脚本会退 2），不静默过。
case "$(uname -s)" in
  Linux)
    if [ -n "$(ldconfig -p 2>/dev/null | grep 'libvulkan\.so\.1' | head -1)" ]; then
      s2a_gate_step bash "$android_dir/tools/vk_record_drawtex_reject_test.sh"
      s2a_gate_step bash "$android_dir/tools/vk_record_deferred_bind_test.sh"
      s2a_gate_step bash "$android_dir/tools/vk_record_bind_program_test.sh"
    else
      s2a_gate_note "vk_record 生产入口探针：未运行（Linux 但缺宿主 libvulkan.so.1，探针只需它解析符号）"
    fi ;;
  *) s2a_gate_note "vk_record 生产入口探针：未运行（仅 Linux + 宿主 libvulkan.so.1）" ;;
esac
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/tests/stubs/android/util/Log.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamePack.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameFingerprint.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameStorage.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameResume.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameSession.java" \
    "$android_dir/tests/GameSessionTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameSessionTest
# The shell's BootSpec against boot-spec.v1.schema.json (with and without a region).
s2a_gate_step "$PY" - "$android_dir/contracts" "$out/boot-spec" <<'PY'
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("validate", os.path.join(sys.argv[1], "validate.py"))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)
schema = v.load(os.path.join(sys.argv[1], "boot-spec.v1.schema.json"))
for name in sorted(os.listdir(sys.argv[2])):
    v.check_document(v.load(os.path.join(sys.argv[2], name)), schema, name)
print("BootSpec schema: ok")
PY
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamePack.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameInstall.java" \
    "$android_dir/tests/GamePackTest.java" \
    "$android_dir/tests/GameInstallTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GamePackTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameInstallTest
s2a_gate_step "$PY" "$android_dir/tests/deploy_game_test.py"
s2a_gate_step "$PY" "$android_dir/tests/test_app_id_test.py"
s2a_gate_step "$PY" "$android_dir/tests/debug_app_admission_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" "$android_dir/tests/GameSessionAdmissionTest.java" "$android_dir/tests/GameFingerprintTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameSessionAdmissionTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameFingerprintTest
s2a_gate_step "$PY" "$android_dir/tests/android_file_stamp_test.py"
s2a_gate_step "$PY" "$android_dir/tests/verified_source_test.py"
s2a_gate_step "$PY" "$android_dir/../graphics/vulkan/wsi/tests/startup_surface_test.py"
# 11-01 接线（扫描分母按 tests 目录后查出没人调）：离线宿主回归 + 固定 NDK 只编译不执行，本机负载 30 时合计约 40 秒。
s2a_gate_step "$PY" "$android_dir/core-link/alt-tests/run.py" --out "$out/alt-tests"
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/writers-gate/renew_model.py" --out "$out/renew-model"
s2a_gate_step "$PY" "$repo_dir/runtime/tests/support/test_thread_verdict.py"
ndk_cc=$(. "$android_dir/tools/ndk_toolchain.sh" && s2a_ndk_clang "$(s2a_ndk_resolve)" 29) || { echo '【红】找不到固定 NDK 29（android/tools/ndk_toolchain.sh）' >&2; exit 1; }
s2a_gate_step "$PY" "$repo_dir/runtime/tests/support/build_pause_wait_android.py" --cc "$ndk_cc" --out "$out/pause-wait-android"
s2a_gate_step "$PY" "$android_dir/native/runtime/fault/tests/run.py" --objects-only --cc "$ndk_cc" --out "$out/fault-objects"
s2a_gate_step "$PY" "$android_dir/native/runtime/platform/tests/shutdown_wait_test.py"
s2a_gate_step "$PY" "$android_dir/native/direct-vk-tests/run_shutdown_contract.py"
# App 纯 Java 层整体编译：抓 import 漏缺 / 签名不匹配这类编译错（#38 GameLibrary 漏 import 让 main 编不过就是这类）。
# SDK 依赖的类（GameLibrary / MainActivity / NativeBridge / BootSpecSource / TouchPadView 等 import android.*）需 Android SDK，
# 留给带 SDK 的 Gradle 构建，不在无设备门禁编。新增纯 Java 类时补到这份清单。
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/tests/stubs/android/util/Log.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchPad.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchLayoutStore.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameMetadata.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameSession.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/RuntimeLibrary.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamepadState.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/Json.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootContract.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootSpec.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamePack.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameFingerprint.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameInstall.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/NativeRewriter.java"
# J09-S1：纯合成密钥；长空白回归由 PlayerKeysTest 启独立 JVM，10 秒限时并回收自身子进程。
# 同入口区分系统祖先软链与 App 数据根/files/中间目录软链，并核旧权限恢复及 hardlink 不改 inode。
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/PlayerKeys.java" \
    "$android_dir/tests/PlayerKeysTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.PlayerKeysTest
echo "App 纯 Java 类编译：ok"
# Portable preparation regressions are required by the platform gate. The
# NO_NATIVE flag above only excludes the Linux runtime CTest/mremap tests.
s2a_gate_step "$PY" "$android_dir/tests/native_rewriter_test.py"
s2a_gate_step "$PY" "$android_dir/tests/native_rewriter_packaging_test.py" --format-fixture
s2a_gate_step "$PY" "$android_dir/contracts/validate.py"
s2a_gate_step "$PY" "$android_dir/tests/contract_validation_test.py"
s2a_gate_step "$PY" "$android_dir/tests/device_retirement_test.py"
s2a_gate_step "$PY" "$android_dir/tests/copy_3d_depth_test.py"
# A3：记录快照的对象身份复核 + 有界退休墓碑 + 附件解析类型化失败（宿主 mock，--source-ref 红）。
s2a_gate_step "$PY" "$android_dir/tests/tex_identity_state_test.py"
s2a_gate_step "$PY" "$android_dir/tests/tex_identity_state_test.py" --withdraw
# 批次 1/A：命令流拍平（A2 延迟参数在所有权转移点入记录 + A4 记录内引用重定位 + 拍不到即失败）。
# 默认全绿；--source-ref 旧源码必须红；--withdraw 的 5 条行为撤回变异必须各自转红。
s2a_gate_step "$PY" "$android_dir/tests/record_flat_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_replayed_fence_test.py"
# 回放状态只在第一次提交前复位（#539）：mtl_sub_begin 生产函数体，三次提交只入队一次复位作业。
s2a_gate_step "$PY" "$android_dir/tests/replay_state_keep_test.py"
s2a_gate_step "$PY" "$android_dir/tests/record_flat_test.py" --withdraw
s2a_gate_step "$PY" "$android_dir/core-runtime/metrics-tests/cli_test.py"
s2a_gate_step bash "$android_dir/core-runtime/fps-tests/test.sh"
s2a_gate_step "$PY" "$android_dir/tests/run_manifest_test.py"
s2a_gate_step "$PY" "$android_dir/tests/game_process_manifest_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/androidTest/java/org/s2arm/android/SessionLogEvidence.java" \
    "$android_dir/app/src/androidTest/java/org/s2arm/android/SessionExitEvidence.java" \
    "$android_dir/tests/SessionExitEvidenceTest.java" \
    "$android_dir/tests/SessionLogEvidenceTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.SessionLogEvidenceTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.SessionExitEvidenceTest
s2a_gate_step "$PY" "$android_dir/tests/job_contract_test.py"
s2a_gate_step "$PY" "$android_dir/tests/sysdata_pack_test.py"
s2a_gate_step "$PY" "$android_dir/tests/pack_layout_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_pack_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_library_test.py"
s2a_gate_step "$PY" "$android_dir/tests/shader_pack_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_qa_compare_test.py"
s2a_gate_step "$PY" "$android_dir/tests/first_draw_diag_bounds_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_first_draw_release_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_fd_fence_wait_test.py"   # 逐帧 drain 账：fd_fence_wait 双账挂接（含变异控制）
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_session_release_order_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_adapter_release_order_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvn_event_completion_test.py"   # P0-4 事件信号写值前必须有完成证明（含变异控制）
s2a_gate_step "$PY" "$android_dir/tests/mutex_timedlock_test.py"   # macOS 无 pthread_mutex_timedlock：可移植实现的超时 / 获锁 / 不空转
# D 接口（完成 token，批次 1）：5 态完成事实 + 批次持有/放行 + GPU 完成后退休回调 + 换代/Stop 保留引用。
# 宿主假完成源，不建 device、不碰真队列；--source-ref 先红后绿。
s2a_gate_step "$PY" "$android_dir/tests/nvk_completion_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvk_completion_test.py" --withdraw
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_query_terminal_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_deferred_slot_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_indirect_args_test.py"   # 间接参数的权威字节（影子同步 / 在途帧退休）+ 变异控制
s2a_gate_step "$PY" "$android_dir/tests/nvn_vk_sampler_mapping_test.py"   # P0-2 采样器映射判定表 + 变异控制
# 真 Vulkan 收尾探针：macOS 上要 docker；门禁派 gate-host 时作为子项 vkreal 跳过、本机另跑（android/gate.sh 头注）
[ "${S2A_ANDROID_SKIP_VKREAL:-0}" = 1 ] || s2a_gate_step bash "$android_dir/tests/nvn_vk_real_teardown_test.sh"
s2a_gate_step "$PY" "$android_dir/tests/draw_log_channel_test.py"
s2a_gate_step "$PY" "$android_dir/tests/vulkan_modules_test.py"
s2a_gate_step "$PY" "$android_dir/tests/filecopy_test.py"

# 拆线程主机套件（PR #498）：counter/audio/metrics/Runtime.Tests 跨平台；run_lifecycle 仅 Linux。
# 不进默认门禁的 control-tests 十连、writers-gate、run_thread 见 tests/gate_not_default.tsv。
s2a_gate_step "$PY" "$android_dir/tests/verify_apk_test.py"
s2a_gate_step "$PY" "$android_dir/tests/threaded_python_selection_test.py"
s2a_gate_step "$PY" "$android_dir/tests/gate_failure_summary_test.py"
# 主机测试输出目录隔离（#919）：并行门禁与上一次被中断的残留都不得成为本次输入。
s2a_gate_step "$PY" "$android_dir/tests/host_output_isolation_test.py"
s2a_gate_step bash "$android_dir/tests/threaded_host_suites.sh"
# APK 内存装载器（macOS ARM64 + 真实 Apple TSD 夹具；#1309 接线，只在 macOS 跑）
if [ "$(uname -s)" = Darwin ]; then
    s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/apk-loader/run.py" --out "$out/apk-loader"
fi
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/readonly-copy/texture_read_test.py"
# 共享 guest 入口的主线程 fault 状态准入顺序（真实 guest_main.c；fault/SDK 端点为合成夹具；11-01 #1309 接线）。
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/run_guest_entry.py" --out "$out/guest-entry"
# Q19/#530：C# 绑定入口 vs Android 单库实际链接定义（fixture + 当前树正例）。
s2a_gate_step python3 "$android_dir/core-link/link_symbols.py"
s2a_gate_step "$PY" "$android_dir/core-link/tests/t_link_symbols.py"
# Cross-compile and partially link the real game loader/provider with the pinned NDK.
s2a_gate_step "$PY" "$android_dir/core-link/test_game_core_closure.py"
# #1309：fault 的 10 个真实 Bionic API29 对象只编不链（crash.c/thread.c 按生产 CMake 带 _GNU_SOURCE）；固定 NDK，不上设备。
s2a_gate_step "$PY" "$android_dir/native/runtime/fault/tests/run_objects.py" --out "$out/fault-owner-objects"
# 真实 NDK 共享链接 / 符号可见性回归（#1309）：entry.S 惰性取核钩子引用的
# g_lazy_core / s2a_guest_return 由测试夹具补齐，生产新增外部符号时这里同步跟平。
# NDK 由测试自己按 link_symbols.find_ndk() 固定解析 29.0.14206865，不信任只按目录判断的环境变量。
s2a_gate_step "$PY" "$android_dir/native/runtime/fault/tests/run_xstack_link.py" --out "$out/xstack-link"
# Versioned managed-probe ELF contract; same pinned NDK, no AOT or device.
s2a_gate_step "$PY" "$android_dir/cs/savecommit/tests/apk/unit/managed_elf_test.py"

# qa.sh 自测：验证门禁脚本自身的编排逻辑与 preflight 拒绝路径。纯 fixture，不需要 Docker。
s2a_gate_step bash "$android_dir/tests/qa_self_test.sh"
# 10-29 基础设施线接线：其余四支已在上方运行；这里保留新的设备回合契约。
s2a_gate_step "$PY" "$android_dir/tests/device_round_test.py"
# #1449 等价比对工具：纯主机 Python，合成帧 + 负对照，不接触设备。
s2a_gate_step "$PY" "$android_dir/tests/s2a_yuvdiff_test.py"
s2a_gate_step bash "$android_dir/core-runtime/keyboard-tests/run.sh"

# This portable real-backend matrix always runs, including the macOS gate.
# Clear binary overrides only for this test: its private cache must build the
# current source. Production and direct developer tests retain CLI overrides.
# Missing CMake/pinned offline dependencies are errors, never skips/downloads.
echo "Android package C/Python backend matrix: running real rewrites and Java consumers"
(
    unset S2A_REWRITER_C S2A_REWRITER_BUNDLED
    s2a_gate_step "$PY" "$android_dir/tests/make_pack_rewriter_test.py" -v
)

# Offline codec build/packaging regression (source preparation is explicit).
s2a_gate_step python3 "$android_dir/tests/game_library_import_test.py"
s2a_gate_step python3 "$android_dir/codecs/tests/build_test.py"
s2a_gate_step "$PY" "$android_dir/codecs/tests/managed_test.py"
# #1131：宿主原生构建契约——后端路由 / NativeAOT 静态归档校验 / docker↔host 产物等价核对。
s2a_gate_step "$PY" "$android_dir/tests/build_backend_test.py"
s2a_gate_step "$PY" "$android_dir/tests/archive_check_test.py"
s2a_gate_step "$PY" "$android_dir/tests/compare_apks_test.py"
s2a_gate_step "$PY" "$android_dir/tools/test_compare_apks.py"
s2a_gate_step "$PY" "$android_dir/cs/test_nuget_verify.py"
# doctor 源码布局与缓存契约（版本表、NDK 宿主目录、codec recipe-key 缓存）。
s2a_gate_step "$PY" -B "$android_dir/doctor/test_doctor.py"

# op31 独立计划器（未接 record）：逐行字节/默认视图/局部区域与图像依赖合同，含四种功能负控。
s2a_gate_step "$PY" "$android_dir/tests/buffer_texture_plan_test.py"
s2a_gate_step "$PY" "$android_dir/tests/buffer_texture_exec_test.py"
