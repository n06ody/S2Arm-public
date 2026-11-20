# Android single-library native source lists, shared by the production CMake target
# and the source-level link-symbol check (link_symbols.py). One definition, so a
# gate that reads these lists cannot drift from what the build actually compiles.
#
# Paths are relative to android unless a function documents otherwise.
# The mode argument is "diagnostic" or "game"; static_services is "static" when the
# NativeAOT service layer is linked into the same library (S2A_CS_SERVICES=static).

if(NOT COMMAND s2a_android_game_sources)
    include("${CMAKE_CURRENT_LIST_DIR}/Runtime.cmake")
endif()

function(s2a_android_host_sources output)
    set(sources
        native/s2a_savecommit.c native/system_tuning.c native/capacity_affinity.cpp native/adpf_hint.c native/cpu_floor.c
        native/bridge.cpp native/lan_discovery.cpp ../runtime/src/lan_elect.c native/capture_jpeg.cpp native/touch_jni.cpp native/game_touch.c native/window.cpp native/presenter.cpp
        # 手机当掌机的六轴 / 震动（#528 第 10 项）：坐标换算、震动信箱、六轴来源仲裁用共用的 runtime/src 实现
        native/phone_feedback.c ../runtime/src/phone_axes.c ../runtime/src/phone_rumble_mailbox.c ../runtime/src/motion_pick.c
        native/wsi_lease.c native/input.c native/tsd.c native/module.c native/selftest.c native/selftest_gate.S
        native/guest_signal.c native/redlog.c native/redzone.S native/pcm_ring.c native/aaudio_sink.c native/hostapi.c native/screen_metrics.c
        # #48 P8：dm_thermal_now / dm_low_power_now 的安卓强定义（盖过 fps.c / hle_nvn.c 的弱缺省）
        native/power_state.c
        native/host_android.c native/host_ui.c native/host_ui_android.c native/host_ui_jni.cpp native/plat_android.c
        # 直连安全网安卓接线（共用 runtime/src/dguard.c 的武装 / 生命周期出口；游戏目标由
        # Runtime.cmake 提供 dguard.c，诊断目标见 s2a_android_diagnostic_sources）
        native/dguard_android.c
        # 安卓侧共用文档根绑定：runtime.c 起局把 S2A_DOCS 指到本局 state 目录（iOS/Mac 由
        # launchctx 提供，安卓无此环节；dguard 的 wire/direct_guard 等消费者才得以落盘）
        native/docs_android.c
        # AMediaCodec 视频解码边界（#44 第二刀）：纯 NDK API，无钉版源码，直接进单库。
        codecs/s2a_mc.c
        native/direct_vk.c native/direct_session.c native/vk_loader_shim.c native/direct_choice.c native/direct_callback.c native/direct_callback.S)
    if(S2A_ANDROID_EXPERIMENTAL_TOUCH)
        list(APPEND sources native/touch_input.c)
    endif()
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(s2a_android_diagnostic_sources output)
    # 游戏目标由 Runtime.cmake 提供；诊断目标同样链接真实相位和时钟。
    # 静态服务层（DirectPInvoke）引用 graphics/track 的写跟踪符号，诊断目标也编入同一实现，
    # 不再另写空实现：两种目标的行为一致，只是诊断不跑 guest 内存。
    # 直连核心的页级写跟踪（nvn_dirty → nvn_host_hooks）登记到 direct_host 的 dispatcher，
    # 诊断目标同样编入真实实现（游戏目标由 Runtime.cmake 的 core 列表提供）。
    set(sources
        ../runtime/src/os.c ../runtime/src/alt.c ../runtime/src/direct_host.c ../graphics/track/track.c
        # 应用池 s2a_app_pool（Switch 2 9 GiB）：plat_android.c 的 layout_android.inc 经 S2A_HEAP_DEFAULT 读它
        ../runtime/src/layout.c
        native/runtime/tracking/platform.c native/runtime/tracking/runtime.c
        native/runtime/tracking/registry.c
        # #1456 capprobe：诊断单库同样链 bridge.cpp / vk_device.c 引用的探测符号；
        # 探测本体 capprobe.c 只编成独立可执行件（packaged helper），不进单库。
        native/capprobe/capprobe_host.c
        # dguard_android.c / bridge.cpp 引用的共用出口：诊断目标编入同一实现（无武装点，
        # 只靠弱回退的日志与 stub 的 s2a_host_session_state 空转），行为与游戏目标一致。
        ../runtime/src/dguard.c ../runtime/src/direct_prompt.c
        # 即时存档开关（snapsw.c，安卓恒关）：服务层 SnapLog.cs 与 hle_nvn.c 引用，诊断目标同编（游戏目标由 Runtime.cmake 的 core 列表提供）
        ../runtime/src/snapsw.c
        # 诊断目标没有原生会话核心：direct_prompt.c 要的 s2a_host_session_state 等会话入口由 stub 给
        # （恒报 FAILED）。不论服务层是否静态链入都要它，否则缺省 assembleDebug（S2A_CS_SERVICES=0）
        # 在 -z defs 下报 undefined symbol: s2a_host_session_state。
        native/s2a_native_stub.c)
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(s2a_android_static_sources output mode)
    # 单库：诊断目标用链接桩顶住尚未在 Android 上运行的共享核心；游戏目标由真实
    # provider 提供同一组符号。只读 guest 复制只在诊断目标需要。
    if(mode STREQUAL "diagnostic")
        # readonly_copy.c 现在也读背板打洞世代号（#1217 §2.6），诊断单库必须带 backing.c。
        # 链接桩 s2a_native_stub.c 已在 s2a_android_diagnostic_sources 里（不论服务层模式）。
        set(sources native/runtime/tracking/readonly_copy.c native/runtime/vm/backing.c)
    else()
        set(sources)
    endif()
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

# 新回放层（S2ARM_ANDROID_VK_BACKEND=nvk）的直连核心源清单：只有零件 + 回放核心 + nvn_gpu_* 后端 + 平台件。
# 旧回放与绘制源一个都不在这里；这张表里出现 mtl_vk_* / vk_record / nvn_vk_first_draw / nvn_vk_texcache 即是错。
function(s2a_android_nvk_sources output)
    set(sources
        # 会话 ABI、包描述、选择事务（jv.c：离线着色器库读面用的 JSON 取值件）
        native/direct/s2a_direct_abi.c native/direct/json.c native/direct/jv.c native/direct/sha256.c
        native/direct/graphics.c native/direct/attempt.c native/direct/selection.c native/direct/control.c
        # 设备与能力
        native/direct/vk_device.c native/direct/gs_path.c native/direct/device_caps.c native/direct/vk_identity.c
        # 着色器链里与旧回放无关的部分
        native/direct/compiler_bind.c native/direct/source_store.c native/direct/diag.c
        native/direct/compile_worker.c native/direct/program_cache.c native/direct/offline_library.c
        native/direct/libpack_vk.c   # 出包预编库读取（nvk_shader.c 按每阶段键取，同 iOS lib_pack）
        native/direct/compile_request.c native/direct/input_identity.c native/direct/texture_facts.c
        native/nvn/nvn_vk_program_result.c native/nvn/nvn_vk_compute_request.c native/gfx_plat/nvk_shader.c native/gfx_plat/direct_diag.c
        # 首次使用的准备工作下沉（iOS 蓝本 nvn_mtl.m warm_start/pso_pend 的 Vulkan 版）：
        #   nvn_vk_prog_warm  程序预热清单（跨会话程序内容身份 → 读盘/解析/建设备对象 → 发布到 g_progs）
        #   nvn_vk_pipe_warm  管线预热清单（完整 nvn_gpu_pipe_desc 配方 → 后台 nvn_gpu_pipe_get）
        #   nvn_vk_pipe_once  「同一配方只有一份」的双检装载（提交线程与预热线程并发建管线）
        #   nvn_warm          一个有界后台线程按清单顺序喂上面两类工作（先程序后管线）
        native/nvn/nvn_vk_prog_warm.c native/nvn/nvn_vk_pipe_warm.c native/nvn/nvn_vk_pipe_once.c native/nvn/nvn_warm.c
        #   首用管线后台构建（iOS pso_pend / first_skip_ok 的 Vulkan 新回放版）：pending 状态表 + demand 后台线程
        native/nvn/nvn_pipe_pend.c native/nvn/nvn_vk_pipe_demand.c
        #   nvn_pipe_flight  配方级在途表（纯逻辑）：同配方并发需求合并成一份创建，不同配方不互相排队（复核 P1-2）
        native/nvn/nvn_pipe_flight.c
        # nvn_gpu_* 的 Vulkan 后端与它的零件
        native/nvn/backend_vulkan.c native/nvn/nvn_vk_format.c native/nvn/nvn_vk_bcdec.c
        native/nvn/nvn_vk_driver_cache.c   # 驱动管线缓存落盘文件层（backend_vulkan.c 的 plc_open / plc_close 用；SHA-256 用上面的 sha256.c）
        ../graphics/nvn/gfx/nvn/nvn_fmt_expand.c
        # 回放核心（安卓 / 鸿蒙共用）
        ../graphics/nvn/replay/rp_exec.c ../graphics/nvn/replay/rp_copy_t2b.c ../graphics/nvn/replay/rp_clear_tex.c ../graphics/nvn/replay/rp_state.c ../graphics/nvn/replay/rp_entry.c
        ../graphics/nvn/replay/rp_res.c ../graphics/nvn/replay/rp_buf.c ../graphics/nvn/replay/rp_pass.c
        ../graphics/nvn/replay/rp_present.c ../graphics/nvn/replay/rp_draw.c ../graphics/nvn/replay/rp_sdx.c
        ../graphics/nvn/replay/nvn_unimpl.c
        # 安卓平台件
        native/gfx_plat/nvk_session.c native/gfx_plat/plat_nvn.c native/gfx_plat/plat_nvn_vk.c
        native/nvn/nvn_vsync_android.c native/nvn/nvn_vk_lifecycle.c native/nvn/poolahb_alias.c
        # 层外按名字要的两组：C# 的帧日志转储（AndroidGameRuntime）与通用后端的编译事件（S2ACompileEvents）
        native/nvn/nvn_vk_frame_log.c native/nvn/nvn_vk_compile_events.c
        # 共用前端的纯逻辑件
        ../graphics/nvn/gfx/nvn/nvn_tex.c ../graphics/nvn/gfx/tex/nvn_deswizzle.c
        ../graphics/nvn/gfx/nvn/nvn_first_use.c
        ../graphics/nvn/gfx/nvn/nvn_dirty.c native/nvn/nvn_host_hooks.c)
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(s2a_android_direct_sources output)
    # 直连核心 = 新回放层（nvk）的清单。旧回放层（mtl_vk_shim / mtl_vk_adapter / nvn_vk_* 绘制与资源 / vk_record …）
    # 与它独立能链的「引擎关」旧直连清单随旧层一起删除（#1501 owner 11-20 定删）；引擎关的构建在 CMakeLists.txt 配置时报错。
    # 生产构建与 Q19 源码级符号扫描（core-link/link_symbols.py）读同一份。
    s2a_android_nvk_sources(sources)
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(s2a_android_link_sources output mode static_services)
    # Full target source set, used by the gate and available to CMake callers that
    # need the same answer as the production target.
    s2a_android_host_sources(host)
    if(mode STREQUAL "game")
        s2a_android_game_sources(mode_sources)
    else()
        s2a_android_diagnostic_sources(mode_sources)
    endif()
    # 直连核心在两种模式都编进生产目标（见 CMakeLists 的 s2a_android_direct_core）。
    s2a_android_direct_sources(direct_sources)
    set(sources ${host} ${mode_sources} ${direct_sources})
    if(static_services STREQUAL "static")
        s2a_android_static_sources(static_sources "${mode}")
        list(APPEND sources ${static_sources})
    endif()
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()
