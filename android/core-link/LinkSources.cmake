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
        native/s2a_savecommit.c native/capacity_affinity.cpp native/adpf_hint.c
        native/bridge.cpp native/lan_discovery.cpp ../runtime/src/lan_elect.c native/capture_jpeg.cpp native/touch_jni.cpp native/game_touch.c native/window.cpp native/presenter.cpp
        native/wsi_lease.c native/input.c native/tsd.c native/module.c native/selftest.c native/selftest_gate.S
        native/guest_signal.c native/redlog.c native/redzone.S native/pcm_ring.c native/aaudio_sink.c native/hostapi.c native/screen_metrics.c
        native/host_android.c native/host_ui.c native/host_ui_android.c native/host_ui_jni.cpp native/plat_android.c
        native/direct_vk.c native/direct_session.c native/direct_choice.c native/direct_callback.c native/direct_callback.S)
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
        native/capprobe/capprobe_host.c)
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(s2a_android_static_sources output mode)
    # 单库：诊断目标用链接桩顶住尚未在 Android 上运行的共享核心；游戏目标由真实
    # provider 提供同一组符号。只读 guest 复制只在诊断目标需要。
    if(mode STREQUAL "diagnostic")
        # readonly_copy.c 现在也读背板打洞世代号（#1217 §2.6），诊断单库必须带 backing.c。
        set(sources native/s2a_native_stub.c native/runtime/tracking/readonly_copy.c native/runtime/vm/backing.c)
    else()
        set(sources)
    endif()
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(s2a_android_direct_sources output)
    # NVN→Vulkan 直连核心：由 NvnDirect.cmake 的 s2a_android_direct_core() 编进生产目标。
    # 列在这里是为了让 Q19（link_symbols.py）的源码级符号扫描与实际编译源一致，
    # 新增直连导出时只需改这一处，门禁不会漏看。
    set(sources
        native/direct/s2a_direct_abi.c native/direct/json.c native/direct/sha256.c
        native/direct/graphics.c native/direct/attempt.c native/direct/selection.c
        native/direct/control.c native/direct/vk_device.c native/direct/gs_path.c
        native/direct/vk_sync.c native/direct/vk_track.c
        native/direct/vk_membuf.c native/direct/vk_resid.c
        native/nvn/nvn_vk_poolahb.c native/nvn/poolahb_cover.c native/nvn/poolahb_policy.c native/nvn/poolahb_alias.c
        native/nvn/nvn_vk_timeline.c native/nvn/nvn_vk_threadbuf.c
        # 逐帧上屏间隔 + 每帧 fence 等待（诊断默认关）：direct/generic 同一份环形缓冲与
        # 转储格式（t-direct-frametimes），generic 经 DllImport 调 s2a_frame_log_present/dump，必须随直连核心编入。
        native/nvn/nvn_vk_frame_log.c
        native/nvn/nvn_vk_compile_events.c
        native/direct/vk_clear.c
        native/nvn/nvn_vk_swapchain.c native/nvn/nvn_vk_surface.c native/nvn/nvn_vk_session.c
        native/nvn/nvn_vk_lifecycle.c native/nvn/nvn_vk_pipeline.c native/nvn/nvn_vk_texcache.c
        native/nvn/nvn_vk_bcdec.c
        # NVN ReadTexels：纯几何换算（宿主单测同源）+ guest 内存路径 + GPU 图像路径。
        native/nvn/nvn_vk_readback_plan.c native/nvn/nvn_vk_readback.c native/nvn/nvn_vk_readback_gpu.c
        native/nvn/mtl_vk_adapter.c native/direct/vk_record.c native/direct/t2t_plan.c
        # 批次 1/A：命令流拍平（A2 延迟参数入记录 + A4 记录内引用重定位）；shim 的 clone_append 调用它。
        native/direct/record_flat.c
        native/nvn/nvn_vk_sampler_cache.c native/nvn/nvn_vk_sampler.c native/nvn/nvn_vk_format.c
        native/nvn/nvn_vk_mrt.c
        native/nvn/nvn_vk_program.c native/nvn/nvn_vk_draw.c native/nvn/nvn_vk_compile_facts.c
        native/nvn/nvn_vk_pipe_recipe.c native/nvn/nvn_vk_driver_cache.c
        native/nvn/nvn_vk_first_draw.c native/nvn/nvn_vk_draw_diag.c native/nvn/nvn_vk_program_result.c native/nvn/nvn_vk_query.c
        native/nvn/nvn_vk_compute.c native/nvn/nvn_vk_compute_texture.c native/nvn/nvn_vk_compute_request.c
        native/nvn/nvn_vk_compute_lookup.c native/nvn/nvn_vk_compute_bridge.c
        native/nvn/nvn_vk_pipe_manifest.c native/nvn/nvn_vk_pcprof.c
        native/nvn/nvn_vk_exec_owner.c native/nvn/nvn_vk_pipe_worker.c
        # 批次 1/D：提交完成事实与批次持有（纯状态机，不碰 Vulkan；批次 2/3 接入消费）。
        native/nvn/nvn_vk_completion.c
        native/direct/compiler_bind.c native/direct/source_store.c native/direct/device_caps.c
        native/direct/vk_identity.c
        native/direct/compile_request.c native/direct/diag.c native/direct/compile_export.c native/direct/compile_export_w2.c native/direct/w2_single_query.c native/direct/input_identity.c
        native/direct/texture_facts.c native/direct/compile_worker.c
        native/direct/bind_defer.c
        native/direct/program_cache.c native/direct/offline_library.c
        # Pure texture helpers are needed by the core even without the game frontend.
        ../graphics/nvn/gfx/nvn/nvn_tex.c ../graphics/nvn/gfx/tex/nvn_deswizzle.c
        # 纹理缓存（nvn_vk_texcache.c）按页级写跟踪判脏，前端关也引用：nvn_dirty 与它的
        # Android 宿主钩子（dm_* → direct_host）随直连核心编入，不再只挂在前端源列表里。
        ../graphics/nvn/gfx/nvn/nvn_dirty.c native/nvn/nvn_host_hooks.c)
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
