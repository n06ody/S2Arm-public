# Android 游戏目标直接链接这些生产 core 对象；不链接诊断 native stub。
get_filename_component(S2A_ANDROID_RUNTIME_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
function(s2a_android_game_sources output)
    set(repo "${S2A_ANDROID_RUNTIME_ROOT}")
    # Apple 专用触页优化与诊断不用于 Android；Android 使用实际 fault dispatcher。
    set(core alt ap_host boot caps cpatch crash det dguard direct_host direct_prompt gate layout libc lifecycle loader loadprog logcap logcon lowva nrodefer
        glslc_tool oodle_tool os pack_apply pad patch patch_data selfcheck session svc_mem text_input thread traps vawin widen xstack)
    set(sources)
    foreach(name IN LISTS core)
        list(APPEND sources "${repo}/runtime/src/${name}.c")
    endforeach()
    list(APPEND sources
        "${repo}/runtime/src/entry.S"
        "${repo}/runtime/src/guest_main.c"
        "${repo}/graphics/track/track.c"
        "${repo}/android/native/runtime/runtime.c"
        "${repo}/android/native/runtime/vm/backing.c"
        "${repo}/android/native/runtime/tracking/platform.c"
        "${repo}/android/native/runtime/tracking/readonly_copy.c"
        "${repo}/android/native/fps_provider.c"
        "${repo}/android/native/runtime/tracking/runtime.c"
        "${repo}/android/native/runtime/tracking/registry.c"
        "${repo}/android/native/runtime/fault/fault.c"
        "${repo}/android/native/runtime/fault/trampoline.S"
        "${repo}/android/native/runtime/fault/elf_host.c"
        # #1456 capprobe：GS+遮挡查询探测的 host 单臂运行器（posix_spawn 打包助手；
        # 探测本体 capprobe.c 只编成独立可执行件进 APK lib/，不链进单库）。
        "${repo}/android/native/capprobe/capprobe_host.c"
        "${repo}/android/native/runtime/platform/lifecycle_android.c")
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()
