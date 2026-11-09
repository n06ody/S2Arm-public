cmake_minimum_required(VERSION 3.16)
project(s2arm_android LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(S2A_ANDROID_EXPERIMENTAL_TOUCH "Build opt-in touch transport and HID consumer" OFF)
# 性能构建：APK 仍走 Debug 安装流程（-g/断言/符号），但游戏 native 热点必须优化。
# 不能只优化探针 target 或 Java；NativeAOT 已是 Release。
if(S2A_ANDROID_NATIVE_MODE STREQUAL "game")
    string(APPEND CMAKE_C_FLAGS_DEBUG " -O2")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -O2")
endif()
# 共享 NVN 前端 + 生产 direct provider + S2Arm 自己 Vulkan 后端的编译门。选项名沿用历史
# S2A_ANDROID_VK_ENGINE（外部脚本与文档都用 S2ARM_ANDROID_VK_ENGINE=1 打开）：名字里的 engine
# 指已删除的 d2r NVN->Vulkan 回放引擎副本，现在这里只编自有后端。
# 默认关闭，只有显式开启时才进包（体积 + 编译时间都明显增加）。
option(S2A_ANDROID_VK_ENGINE "Build the shared NVN frontend + direct provider + nvn_vk backend into the APK" OFF)
# 前端 mtl_* 的后端实现只剩 nvn_vk（nvn_vk_* + mtl_vk_adapter + mtl_vk_shim.c）。旧值 engine
# 已删除；传旧值直接报错，不静默回退到别的后端。
set(S2ARM_ANDROID_VK_BACKEND "nvn_vk" CACHE STRING "NVN backend behind the shared frontend: nvn_vk")
set_property(CACHE S2ARM_ANDROID_VK_BACKEND PROPERTY STRINGS nvn_vk)
if(NOT S2ARM_ANDROID_VK_BACKEND STREQUAL "nvn_vk")
    message(FATAL_ERROR "S2ARM_ANDROID_VK_BACKEND=${S2ARM_ANDROID_VK_BACKEND}: the old 'engine' backend (d2r Vulkan replay engine) was deleted; use nvn_vk")
endif()
set(S2A_ANDROID_VAMM_LAYOUT "alias" CACHE STRING "Guest virtual memory layout: alias or balanced")
set_property(CACHE S2A_ANDROID_VAMM_LAYOUT PROPERTY STRINGS alias balanced)
if(NOT S2A_ANDROID_VAMM_LAYOUT MATCHES "^(alias|balanced)$")
    message(FATAL_ERROR "S2A_ANDROID_VAMM_LAYOUT must be alias or balanced")
endif()
option(S2A_ANDROID_REWRITER "Build the experimental standalone preparation JNI library" OFF)
# Native-only link probe used by core-link/link_symbols.py: a generated C file
# references every C# native entry, so ld.lld -z defs is the final judge even
# when the NativeAOT archive is absent. Empty = normal build.
set(S2A_ANDROID_LINK_PROBE "" CACHE FILEPATH "Optional generated C file with link-symbol references")
set(S2A_ANDROID_LINK_AVC_ARCHIVE "" CACHE FILEPATH "Real libavc archive for the link-symbol probe")
set(S2A_CS_STATIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/android-cs/static" CACHE PATH
    "NativeAOT static service output (cs/build.sh), overridable for isolated link probes")

add_library(s2arm_android_memory STATIC native/memory.c)
set_target_properties(s2arm_android_memory PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(s2arm_android_memory PUBLIC native)
target_compile_definitions(s2arm_android_memory PRIVATE _GNU_SOURCE)
target_compile_options(s2arm_android_memory PRIVATE -Wall -Wextra -Werror)

if(ANDROID)
    if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "The native-execution Android host requires arm64-v8a")
    endif()
    include(core-link/CoreLink.cmake)
    enable_language(ASM)
    # One source list for the build and the symbol gate (core-link/LinkSources.cmake).
    include(core-link/LinkSources.cmake)
    s2a_android_host_sources(host_sources)
    add_library(s2arm_android SHARED ${host_sources})
    # Independent native boundary: never enabled in Release/RelWithDebInfo/MinSizeRel.
    target_compile_definitions(s2arm_android PRIVATE $<$<CONFIG:Debug>:S2A_ANDROID_DEBUG_ENV=1>)
    # 窗口/呈现始终用 Android Vulkan surface；direct 后端只是额外的消费者。
    target_compile_definitions(s2arm_android PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
    include(core-link/NvnDirect.cmake)
    s2a_android_direct_core(s2arm_android)
    if(S2A_ANDROID_VK_ENGINE)
        # 一次只编一种后端：nvn_vk = S2Arm 自己的 Vulkan 后端（nvn_vk_* + mtl_vk_adapter +
        # mtl_vk_shim）。旧 d2r 引擎分支随目录删除，不再有第二种后端。
        # NVN 名字表从 names.txt 生成（与 nvn_count.h 的 NVN_NFUNC 同源），不手抄。
        set(s2a_nvn_names "${CMAKE_CURRENT_SOURCE_DIR}/../graphics/nvn/gfx/nvn/names.txt")
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nvn_names_android.c"
            COMMAND "${CMAKE_COMMAND}" -D "NAMES=${s2a_nvn_names}"
                    -D "OUT=${CMAKE_CURRENT_BINARY_DIR}/nvn_names_android.c"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/native/nvn/gen_nvn_names.cmake"
            DEPENDS "${s2a_nvn_names}" "${CMAKE_CURRENT_SOURCE_DIR}/native/nvn/gen_nvn_names.cmake"
            VERBATIM)
        set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/nvn_names_android.c" APPEND PROPERTY
            INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../graphics/nvn/gfx/nvn")
        set(s2a_frontend_c
            native/nvn/portable.c native/nvn/s2a_vp_alias.c ../graphics/nvn/gfx/nvn/hle_nvn.c
            # Switch 2（NVN2）：hle_nvn.c 调 nvn2_on / nvn2_key / nvn2_device_*（graphics/nvn/gfx/nvn/nvn2.c，SHA-256 走 portable.c 的 CommonCrypto 兼容层）
            ../graphics/nvn/gfx/nvn/nvn2.c
            ../graphics/nvn/gfx/nvn/nvn_fps.c
            "${CMAKE_CURRENT_BINARY_DIR}/nvn_names_android.c" native/nvn/nvn_be_android.c
            # App 已经提供 now_ns/guest_ns/module_of，所以用去掉这几项的探针宿主 shim
            # （frontend_test_host.c 直接编进来会 duplicate symbol）。
            native/nvn/vk_engine_host_shim.c
            native/nvn/shader_survey.c)
        # 共享前端同样不跟本仓 -Werror（格式化串/聚合初始化在 Android 上按 64 位 long 处理）。
        set_source_files_properties(${s2a_frontend_c} PROPERTIES COMPILE_OPTIONS "-Wno-error")
        # 共享前端的后端入口名是 main 的 nvn_be_*；本线 Android 后端仍以 mtl_* 实现。
        # 本兼容头把标识符在编译期对齐（同一批函数、同一套签名），不改任何一方的函数体。
        target_compile_options(s2arm_android PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/native/nvn/nvn_be_compat.h>")
        target_sources(s2arm_android PRIVATE ${s2a_frontend_c}
            native/nvn/mtl_vk_shim.c
            # 帧时统计与 longframe 归因：Vulkan 侧模块，shim 只交本帧量到的数。
            native/nvn/nvn_vk_frame_stats.c
            # D 接口（批次 1）：提交完成事实/令牌持有（纯状态机，不碰 Vulkan；消费者在批次 2/3 接入）。
            native/nvn/nvn_vk_completion.c
            native/direct/provider_vk.c native/direct/profile_compose.c)
        target_compile_definitions(s2arm_android PRIVATE S2A_ANDROID_VK_ENGINE=1 S2A_ANDROID_VK_BACKEND_NVN_VK=1)
    endif()
    # The JNI ABI this library is built against (native/jni_abi.py over the Java native declarations), a
    # build-time output: a change of either Java source regenerates it before bridge.cpp compiles.
    # NativeBridge.load refuses a library whose hash differs from the installed APK's Java side, and
    # verify-apk.py fails an APK whose library does not carry the hash of these sources.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(s2a_jni_java "${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/java/org/s2arm/android/NativeBridge.java"
        "${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/java/org/s2arm/android/TouchInputBridge.java")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/s2a_jni_abi.h"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/native/jni_abi.py"
            --header "${CMAKE_CURRENT_BINARY_DIR}/s2a_jni_abi.h" ${s2a_jni_java}
        DEPENDS ${s2a_jni_java} "${CMAKE_CURRENT_SOURCE_DIR}/native/jni_abi.py"
        COMMENT "JNI ABI of NativeBridge / TouchInputBridge" VERBATIM)
    # A build step of its own, not a target source (the core-link audit reads the target's source list);
    # bridge.cpp's compiler depfile makes it recompile whenever the header's bytes change.
    add_custom_target(s2a_jni_abi DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/s2a_jni_abi.h")
    add_dependencies(s2arm_android s2a_jni_abi)
    target_include_directories(s2arm_android PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    if(S2A_ANDROID_NATIVE_MODE STREQUAL "game")
        s2a_android_game_sources(game_sources)
        target_sources(s2arm_android PRIVATE ${game_sources})
        # Bionic exposes process_vm_readv (used by crash.c's safe frame read)
        # only with GNU feature declarations; Android minSdk 29 exceeds API 23.
        set_property(SOURCE "${S2A_ANDROID_RUNTIME_ROOT}/runtime/src/crash.c" APPEND PROPERTY COMPILE_DEFINITIONS _GNU_SOURCE)
        # thread.c 用到 pthread_getname_np（bionic 在 _GNU_SOURCE 下才声明），与 crash.c 同一处理。
        set_property(SOURCE "${S2A_ANDROID_RUNTIME_ROOT}/runtime/src/thread.c" APPEND PROPERTY COMPILE_DEFINITIONS _GNU_SOURCE)
        target_include_directories(s2arm_android PRIVATE .. ../runtime/src native ../graphics/track)
        target_compile_definitions(s2arm_android PRIVATE S2A_ANDROID_GAME_RUNTIME=1)
        # Reuse the shared layout policy. Balanced trades alias capacity for a larger heap;
        # keep the existing alias-first default for games requiring the large alias range.
        target_compile_definitions(s2arm_android PRIVATE
            S2A_VAMM_BALANCED=$<STREQUAL:${S2A_ANDROID_VAMM_LAYOUT},balanced>)
    else()
        s2a_android_diagnostic_sources(diagnostic_sources)
        target_sources(s2arm_android PRIVATE ${diagnostic_sources})
        target_include_directories(s2arm_android PRIVATE .. ../graphics/track native/runtime/tracking)
    endif()
    target_include_directories(s2arm_android PRIVATE ../runtime/include)
    target_compile_options(s2arm_android PRIVATE -Wall -Wextra -Werror)
    # main 的 NVN 记录 helper 用指定初始化器且省略零字段（Apple 构建没开这条 -Werror）。
    # 必须在 -Wall -Wextra 之后再关：-Wextra 会把组里的告警重新打开。语义不变。
    target_compile_options(s2arm_android PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-designated-field-initializers>")
    target_link_libraries(s2arm_android PRIVATE s2arm_android_memory android log vulkan aaudio)
    if(S2A_ANDROID_EXPERIMENTAL_TOUCH)
        target_compile_definitions(s2arm_android PRIVATE S2A_ANDROID_EXPERIMENTAL_TOUCH=1)
    endif()
    # NDK 29 defaults to 16 KB ELF alignment; keep it explicit for this library.
    target_link_options(s2arm_android PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,defs)
    target_compile_definitions(s2arm_android PRIVATE $<$<CONFIG:Debug>:S2A_CAPACITY_WRAP=1>)
    target_link_options(s2arm_android PRIVATE $<$<CONFIG:Debug>:-Wl,--wrap=pthread_create>)
    # Single-library build (S2ARM_ANDROID_CS_SERVICES=static, passed by Gradle): the NativeAOT
    # service layer and the runtime pieces the SDK listed (cs/build.sh) link into this library,
    # with s2a_native bound to the explicitly selected diagnostic or real game provider.
    #
    # S2A_ANDROID_LINK_PROBE is the gate-only variant: it links the same native sources and
    # provider selection plus a generated object that references the C# entry points, so
    # ld.lld -z defs checks the provider set without a NativeAOT archive or an APK.
    if(S2A_ANDROID_LINK_PROBE)
        s2a_android_static_sources(static_sources "${S2A_ANDROID_NATIVE_MODE}")
        target_sources(s2arm_android PRIVATE ${static_sources} "${S2A_ANDROID_LINK_PROBE}")
        if(S2A_ANDROID_LINK_AVC_ARCHIVE)
            target_link_libraries(s2arm_android PRIVATE "${S2A_ANDROID_LINK_AVC_ARCHIVE}")
        endif()
        target_include_directories(s2arm_android PRIVATE ../runtime/include)
        target_compile_definitions(s2arm_android PRIVATE S2A_CS_STATIC=1)
    elseif(S2A_CS_SERVICES STREQUAL "static")
        set(cs_static "${S2A_CS_STATIC_DIR}")
        if(NOT EXISTS "${cs_static}/link.txt" OR NOT EXISTS "${cs_static}/libs2a_cs.a")
            message(FATAL_ERROR "Run cs/build.sh with S2ARM_ANDROID_CS_SERVICES=static first")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cs_static}/link.txt")
        include(core-link/StaticLinkInputs.cmake)
        s2a_read_native_link_inputs("${cs_static}/link.txt" cs_link_inputs)
        s2a_android_static_sources(static_sources "${S2A_ANDROID_NATIVE_MODE}")
        if(static_sources)
            # 只读 guest 复制依赖 s2a_mem_*，与静态服务的诊断 provider 一起编入。
            # 普通 / 动态服务诊断不运行 guest；game 由 Runtime.cmake 提供真实 VM 和复制实现。
            target_sources(s2arm_android PRIVATE ${static_sources})
        endif()
        target_include_directories(s2arm_android PRIVATE ../runtime/include)
        target_compile_definitions(s2arm_android PRIVATE S2A_CS_STATIC=1)
        # Archive symbols (managed code, runtime) stay inside this library.
        target_link_libraries(s2arm_android PRIVATE "${cs_static}/libs2a_cs.a" ${cs_link_inputs} -Wl,--exclude-libs,ALL)
        # NativeAOT 的模块注册段用 __start_/__stop___modules 这种封装符号。较新的 lld（NDK r29）
        # 默认按 start-stop-gc 语义把这类符号连同段一起丢掉，链接就报 undefined symbol __stop___modules；
        # 老版本 lld 没这个开关，所以先探测再用，不假设工具链版本。
        include(CheckLinkerFlag)
        check_linker_flag(C "-Wl,-z,nostart-stop-gc" S2A_LINKER_HAS_NOSTART_STOP_GC)
        if(S2A_LINKER_HAS_NOSTART_STOP_GC)
            target_link_options(s2arm_android PRIVATE -Wl,-z,nostart-stop-gc)
        endif()
        s2a_audit_diagnostic_core_link(s2arm_android "${cs_static}/libs2a_cs.a"
            "${CMAKE_CURRENT_SOURCE_DIR}/../services/S2A.Services" "${cs_static}/link.txt" "${S2A_ANDROID_NATIVE_MODE}")
    endif()
    if(S2A_ANDROID_REWRITER)
        add_subdirectory(rewriter)
    endif()
else()
    enable_testing()
    add_executable(dma_read_config_test tests/dma_read_config_test.c)
    target_include_directories(dma_read_config_test PRIVATE native)
    target_compile_options(dma_read_config_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_dma_read_config COMMAND dma_read_config_test)
    add_executable(touch_input_test tests/touch_input_test.c native/touch_input.c)
    target_include_directories(touch_input_test PRIVATE native ../runtime/include)
    target_compile_options(touch_input_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(touch_input_test PRIVATE pthread)
    add_test(NAME android_touch_input COMMAND touch_input_test)
    add_executable(game_touch_test tests/game_touch_test.c native/game_touch.c native/touch_input.c)
    target_include_directories(game_touch_test PRIVATE native ../runtime/include)
    target_compile_definitions(game_touch_test PRIVATE S2A_ANDROID_EXPERIMENTAL_TOUCH=1)
    target_compile_options(game_touch_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(game_touch_test PRIVATE pthread)
    add_test(NAME android_game_touch COMMAND game_touch_test)
    find_package(Threads REQUIRED)
    # Linux errno/fd/filesystem contracts; reached by linux_ctest.sh, including
    # Darwin's Linux VM route. These are not Android device durability evidence.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(savecommit_host_test cs/savecommit/tests/native_host_test.c native/s2a_savecommit.c)
        target_compile_options(savecommit_host_test PRIVATE -Wall -Wextra -Werror -Wpedantic)
        target_link_options(savecommit_host_test PRIVATE
            -Wl,--wrap=fsetxattr -Wl,--wrap=fgetxattr -Wl,--wrap=mkdirat
            -Wl,--wrap=fsync -Wl,--wrap=ioctl -Wl,--wrap=write
            -Wl,--wrap=syscall -Wl,--wrap=s2a_savecommit_exchange)
        target_link_libraries(savecommit_host_test PRIVATE Threads::Threads)
        add_test(NAME android_savecommit_host COMMAND savecommit_host_test "${CMAKE_CURRENT_BINARY_DIR}")
        add_executable(savecommit_openread_test cs/savecommit/tests/open_read_host_test.c native/s2a_savecommit.c)
        target_compile_options(savecommit_openread_test PRIVATE -Wall -Wextra -Werror -Wpedantic)
        target_link_options(savecommit_openread_test PRIVATE
            -Wl,--wrap=open -Wl,--wrap=fstat -Wl,--wrap=s2a_savecommit_open_read)
        add_test(NAME android_savecommit_openread COMMAND savecommit_openread_test "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(android_savecommit_host android_savecommit_openread PROPERTIES TIMEOUT 30)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME android_static_link_inputs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core-link/test_static_link_inputs.py"
            --cmake "${CMAKE_COMMAND}")
    add_test(NAME android_core_link_audit
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core-link/test_audit.py")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME android_nativeaot_startup_roots
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core-link/test_startup_roots.py"
                --cc "${CMAKE_C_COMPILER}" --linker "${CMAKE_LINKER}"
                --nm "${CMAKE_NM}" --readelf "${CMAKE_READELF}" --ar "${CMAKE_AR}")
    endif()
    add_executable(memory_test tests/memory_test.c)
    target_compile_definitions(memory_test PRIVATE _GNU_SOURCE)
    target_compile_options(memory_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(memory_test PRIVATE s2arm_android_memory)
    add_test(NAME android_memory COMMAND memory_test)
    add_executable(input_test tests/input_test.c native/input.c)
    target_include_directories(input_test PRIVATE native)
    target_compile_options(input_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(input_test PRIVATE Threads::Threads)
    add_test(NAME android_input COMMAND input_test)
    add_executable(tsd_test tests/tsd_test.c native/tsd.c)
    target_include_directories(tsd_test PRIVATE native)
    target_compile_options(tsd_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_tsd COMMAND tsd_test)
    add_executable(module_test tests/module_test.c native/module.c)
    target_include_directories(module_test PRIVATE native)
    target_compile_options(module_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_module COMMAND module_test)
    add_executable(guest_signal_test tests/guest_signal_test.c native/guest_signal.c native/redlog.c)
    target_include_directories(guest_signal_test PRIVATE native ../runtime/include)
    target_compile_options(guest_signal_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(guest_signal_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME android_guest_signal COMMAND guest_signal_test)
    # 红日志落盘（#1357）：注入目录后 red() 写 <dir>/red.log 与宿主日志 fd 各一行，abort 前 fsync。
    add_executable(redlog_test tests/redlog_test.c native/redlog.c)
    target_include_directories(redlog_test PRIVATE native ../runtime/include ../runtime/src)
    target_compile_options(redlog_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_redlog COMMAND redlog_test)
    add_executable(hostapi_test tests/hostapi_test.c native/hostapi.c)
    target_include_directories(hostapi_test PRIVATE native ../runtime/include)
    target_compile_options(hostapi_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_hostapi COMMAND hostapi_test)
    add_executable(host_ui_test w4-tests/host_ui_test.c w4-tests/wait_probe.c native/host_ui.c native/hostapi.c)
    target_include_directories(host_ui_test PRIVATE native ../runtime/include)
    target_compile_options(host_ui_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(host_ui_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME android_host_ui COMMAND host_ui_test)
    add_executable(keyboard_owner_test w4-tests/keyboard_owner_test.c native/host_ui.c native/host_ui_android.c)
    target_include_directories(keyboard_owner_test PRIVATE native ../runtime/include)
    target_compile_options(keyboard_owner_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(keyboard_owner_test PRIVATE Threads::Threads)
    add_test(NAME android_keyboard_owner COMMAND keyboard_owner_test)
    add_executable(host_android_test tests/host_android_test.c native/host_android.c native/hostapi.c native/screen_metrics.c native/input.c native/wsi_lease.c native/host_ui.c native/host_ui_android.c)
    target_include_directories(host_android_test PRIVATE native ../runtime/include)
    target_compile_options(host_android_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(host_android_test PRIVATE Threads::Threads)
    add_test(NAME android_host_android COMMAND host_android_test)
    # 宿主 plat_test 不跑 JNI/WSI 探针（JNI 探针随诊断 Activity 一起移除）。
    add_executable(plat_test tests/plat_test.c native/plat_android.c native/guest_signal.c native/redlog.c native/memory.c
        native/direct/selection.c native/direct/attempt.c native/direct/graphics.c native/direct/json.c
        native/direct/sha256.c native/direct/s2a_direct_abi.c
        # 应用池 s2a_app_pool：plat_android.c 的 layout_android.inc 经 S2A_HEAP_DEFAULT 读它
        ../runtime/src/layout.c)
    target_include_directories(plat_test PRIVATE native tests/hostvk ../runtime/include)
    set_source_files_properties(native/memory.c PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)
    target_compile_definitions(plat_test PRIVATE S2A_UNIT_TEST)
    target_compile_options(plat_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(plat_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME android_plat COMMAND plat_test)
    # Android fatal 处理器收尾（写 fatal 行后恢复处置、同步故障重放 / SIGABRT 重 raise）：宿主单测，不需要设备。
    add_executable(crash_android_fatal_test ../runtime/tests/t_crash_android_fatal.c)
    target_include_directories(crash_android_fatal_test PRIVATE ../runtime/include ../runtime/src)
    target_compile_options(crash_android_fatal_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_crash_android_fatal COMMAND crash_android_fatal_test)
    add_executable(fps_provider_test tests/fps_provider_test.c native/fps_provider.c)
    target_compile_options(fps_provider_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(fps_provider_test PRIVATE Threads::Threads)
    add_test(NAME android_fps_provider COMMAND fps_provider_test)
    # Linux host regression: real production SVC + real page protections; the
    # thread/tracker boundaries are explicit test providers, not GPU evidence.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(fps_write_integration_test tests/fps_write_integration_test.c ../runtime/src/svc_mem.c
                       ../runtime/src/layout.c ../graphics/track/track.c native/runtime/tracking/platform.c
                       native/runtime/vm/backing.c native/runtime/tracking/runtime.c native/runtime/tracking/registry.c native/plat_android.c native/memory.c)
        target_compile_definitions(fps_write_integration_test PRIVATE S2A_UNIT_TEST _GNU_SOURCE)
        target_include_directories(fps_write_integration_test PRIVATE native ../runtime/include ../runtime/src)
        target_compile_options(fps_write_integration_test PRIVATE -Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
        target_link_options(fps_write_integration_test PRIVATE -Wl,--gc-sections)
        target_link_libraries(fps_write_integration_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME android_fps_write_integration COMMAND fps_write_integration_test)
        set_tests_properties(android_fps_write_integration PROPERTIES TIMEOUT 130)
        add_executable(guest_page_test tests/guest_page_test.c ../runtime/src/svc_mem.c native/runtime/tracking/platform.c
                       native/runtime/vm/backing.c ../runtime/src/det.c ../runtime/src/layout.c native/plat_android.c native/memory.c)
        # svc_mem.c 的 __ANDROID__ 是「走安卓平台实现」（plat_vm_alias / 跟踪运行时），本测试正是链 plat_android.c 测这条路；
        # det.c 按「苹果 / Linux × aarch64 / x86_64」自己选，不冒充安卓（冒充后 x86_64 宿主走进 aarch64 寄存器分支，#1082）
        set_source_files_properties(../runtime/src/svc_mem.c PROPERTIES COMPILE_DEFINITIONS "__ANDROID__;S2A_UNIT_TEST;_GNU_SOURCE")
        set_source_files_properties(../runtime/src/det.c PROPERTIES COMPILE_DEFINITIONS "S2A_UNIT_TEST;_GNU_SOURCE")
        target_compile_definitions(guest_page_test PRIVATE S2A_UNIT_TEST S2A_PAGE_TEST_REAL_DET)
        target_include_directories(guest_page_test PRIVATE native ../runtime/include ../runtime/src)
        target_compile_options(guest_page_test PRIVATE -Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
        target_link_options(guest_page_test PRIVATE -Wl,--gc-sections)
        target_link_libraries(guest_page_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME android_guest_page COMMAND guest_page_test)
        set_tests_properties(android_guest_page PROPERTIES SKIP_RETURN_CODE 77)
        add_executable(alias_poison_test tests/alias_poison_test.c native/runtime/tracking/platform.c native/plat_android.c)
        target_compile_definitions(alias_poison_test PRIVATE S2A_UNIT_TEST _GNU_SOURCE)
        target_include_directories(alias_poison_test PRIVATE ../runtime/include ../graphics/track ../ ../platform)
        target_compile_options(alias_poison_test PRIVATE -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
        target_link_options(alias_poison_test PRIVATE -Wl,--gc-sections -Wl,--wrap=plat_vm_alias -Wl,--wrap=munmap)
        target_link_libraries(alias_poison_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME android_alias_poison COMMAND alias_poison_test)
    endif()
    add_executable(pcm_ring_test tests/pcm_ring_test.c native/pcm_ring.c)
    target_include_directories(pcm_ring_test PRIVATE native)
    target_compile_options(pcm_ring_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(pcm_ring_test PRIVATE Threads::Threads)
    add_test(NAME android_pcm_ring COMMAND pcm_ring_test)
    # Fake only the Android API; compile and exercise the production audio sink.
    add_executable(aaudio_format_test tests/aaudio_format_test.c native/aaudio_sink.c native/pcm_ring.c)
    target_include_directories(aaudio_format_test PRIVATE tests/aaudio_stubs native)
    target_compile_options(aaudio_format_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_aaudio_format COMMAND aaudio_format_test)
    # ADPF's Android API/clock/thread IDs are the only fake boundary.
    add_executable(adpf_hint_test tests/adpf_hint_test.c native/adpf_hint.c)
    target_include_directories(adpf_hint_test PRIVATE native)
    target_compile_definitions(adpf_hint_test PRIVATE S2A_ADPF_TEST)
    target_compile_options(adpf_hint_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(adpf_hint_test PRIVATE Threads::Threads)
    add_test(NAME android_adpf_hint COMMAND adpf_hint_test)
    add_executable(wsi_lease_test tests/wsi_lease_test.c native/wsi_lease.c)
    target_include_directories(wsi_lease_test PRIVATE native)
    target_compile_options(wsi_lease_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(wsi_lease_test PRIVATE Threads::Threads)
    add_test(NAME android_wsi_lease COMMAND wsi_lease_test)
    add_test(NAME android_direct_touch_generation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_touch_generation_test.py")
    # C0：direct ABI 布局/校验/NVN 风格调用。ARM64 真机探针由 tools/direct_abi_check.sh
    # 用同一份源编译；这里只证明主机 LP64 与头文件一致，不能代替 ARM64。
    add_executable(direct_abi_test tests/direct_abi_test.c native/direct/s2a_direct_abi.c)
    target_include_directories(direct_abi_test PRIVATE native ../runtime/include)
    target_compile_options(direct_abi_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_abi COMMAND direct_abi_test)
    # C1：严格 JSON / graphics 描述 / attempt 持久锁 / 选择事务。
    add_executable(direct_json_test tests/direct_json_test.c native/direct/json.c)
    target_include_directories(direct_json_test PRIVATE native ../runtime/include)
    target_compile_options(direct_json_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_json_test PRIVATE m)
    add_test(NAME android_direct_json COMMAND direct_json_test)
    add_executable(direct_graphics_test tests/direct_graphics_test.c
        native/direct/graphics.c native/direct/json.c native/direct/sha256.c native/direct/s2a_direct_abi.c)
    target_include_directories(direct_graphics_test PRIVATE native ../runtime/include)
    target_compile_options(direct_graphics_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_graphics_test PRIVATE m)
    add_test(NAME android_direct_graphics COMMAND direct_graphics_test)
    add_executable(direct_attempt_test tests/direct_attempt_test.c native/direct/attempt.c native/direct/json.c)
    target_include_directories(direct_attempt_test PRIVATE native ../runtime/include)
    target_compile_definitions(direct_attempt_test PRIVATE S2A_DIRECT_TEST_FAULTS=1)
    target_compile_options(direct_attempt_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_attempt_test PRIVATE m)
    add_test(NAME android_direct_attempt COMMAND direct_attempt_test)
    add_executable(direct_selection_test tests/direct_selection_test.c
        native/direct/selection.c native/direct/attempt.c native/direct/graphics.c
        native/direct/json.c native/direct/sha256.c native/direct/s2a_direct_abi.c)
    target_include_directories(direct_selection_test PRIVATE native ../runtime/include)
    target_compile_definitions(direct_selection_test PRIVATE S2A_DIRECT_TEST_FAULTS=1)
    target_compile_options(direct_selection_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_selection_test PRIVATE m)
    add_test(NAME android_direct_selection COMMAND direct_selection_test)
    # #274：direct-vulkan.txt 唯一解析；就绪 / AUTO / 解析三处同一结论，AUTO 回通用点名文件，attempt 标记。
    add_executable(direct_choice_test tests/direct_choice_test.c native/direct_choice.c)
    target_include_directories(direct_choice_test PRIVATE native)
    target_compile_options(direct_choice_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_choice
        COMMAND direct_choice_test "${CMAKE_CURRENT_SOURCE_DIR}/native/direct_session.c")
    # F4.1：编译诊断只能写私有目录；安装 revision 的逐文件清单/摘要前后不变。
    add_executable(direct_diag_test tests/direct_diag_test.c native/direct/diag.c native/direct/sha256.c)
    add_executable(direct_program_cache_test tests/direct_program_cache_test.c
        native/direct/program_cache.c native/direct/sha256.c)
    target_include_directories(direct_program_cache_test PRIVATE native ../runtime/include)
    target_compile_definitions(direct_program_cache_test PRIVATE _GNU_SOURCE S2A_DIRECT_TEST_FAULTS)
    target_compile_options(direct_program_cache_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_program_cache_test PRIVATE pthread)
    add_test(NAME android_direct_program_cache COMMAND direct_program_cache_test)
    # F5.3：安装 revision 的只读离线库消费（命中/缺失/改名/截断/链接/只读 revision）。
    add_executable(direct_offline_library_test tests/direct_offline_library_test.c
        native/direct/offline_library.c native/direct/program_cache.c native/direct/sha256.c)
    target_include_directories(direct_offline_library_test PRIVATE native ../runtime/include)
    target_compile_definitions(direct_offline_library_test PRIVATE _GNU_SOURCE)
    target_compile_options(direct_offline_library_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_offline_library_test PRIVATE pthread)
    add_test(NAME android_direct_offline_library COMMAND direct_offline_library_test)
    # W2.6：编译 provider 的登记身份——同指针幂等、换指针拒绝、Activate 冻结、销毁解绑、表满不覆盖。
    add_executable(direct_compiler_bind_test tests/direct_compiler_bind_test.c native/direct/compiler_bind.c)
    target_include_directories(direct_compiler_bind_test PRIVATE native ../runtime/include)
    target_compile_options(direct_compiler_bind_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_compiler_bind_test PRIVATE pthread)
    add_test(NAME android_direct_compiler_bind COMMAND direct_compiler_bind_test)
    target_include_directories(direct_diag_test PRIVATE native ../runtime/include)
    target_compile_options(direct_diag_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_diag COMMAND direct_diag_test)
    # F4.3：查找/编译共用完整规范输入的身份编码（含 constantBufferUse 反例）。
    # input_identity 的纹理事实走 texture_facts.c 的定长索引映射（F4.2B），必须一起链接。
    add_executable(direct_input_identity_test tests/direct_input_identity_test.c
        native/direct/input_identity.c native/direct/texture_facts.c native/direct/sha256.c)
    target_include_directories(direct_input_identity_test PRIVATE native ../runtime/include
        ../graphics/nvn/gfx/nvn/include)
    target_compile_options(direct_input_identity_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_input_identity COMMAND direct_input_identity_test)
    # F4.2A/F4.3：源快照的完整内容摘要、短 key 一致性、按代际 pin/release。
    add_executable(direct_source_store_test tests/direct_source_store_test.c
        native/direct/source_store.c native/direct/sha256.c)
    target_include_directories(direct_source_store_test PRIVATE native ../runtime/include)
    # 宿主夹具用小的 pinned 额外槽位跑“全 pin 后 keep 失败”，生产默认 16384。
    target_compile_definitions(direct_source_store_test PRIVATE S2A_SOURCE_MAX_PINNED=64)
    target_compile_options(direct_source_store_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_source_store_test PRIVATE Threads::Threads)
    add_test(NAME android_direct_source_store COMMAND direct_source_store_test)
    # F4.3/P1：生产接缝回归——Draw 快照 → 编译事实/规范身份/P1 备忘键必须覆盖同一组事实。
    # 头文件经 vk_record.h 引 Vulkan 类型，但只用枚举；tests/vkhost 提供宿主侧 vulkan.h 入口。
    add_executable(direct_nvn_compile_facts_test tests/direct_nvn_compile_facts_test.c
        native/nvn/nvn_vk_compile_facts.c native/nvn/nvn_vk_format.c native/nvn/nvn_vk_mrt.c
        native/direct/input_identity.c native/direct/texture_facts.c
        native/direct/source_store.c native/direct/sha256.c)
    target_include_directories(direct_nvn_compile_facts_test PRIVATE native native/direct native/nvn
        tests/vkhost ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include
        ../graphics/nvn/third_party/vulkan-headers/include)
    target_compile_options(direct_nvn_compile_facts_test PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
    target_link_libraries(direct_nvn_compile_facts_test PRIVATE Threads::Threads)
    add_test(NAME android_direct_nvn_compile_facts COMMAND direct_nvn_compile_facts_test)
    # #1403：图形 storage buffer 的编译结果 provenance（nvnSlot / uboSlot / uboPtr / rw）
    # 与负控；原样链接生产 parser，不另写模型。
    # compute storage 地址来源解析（NVN / UBO_PTR / UBO_ADDRESS + 来源反例 + supportReads/c0Reads null）
    add_executable(direct_nvn_compute_storage_test tests/direct_nvn_compute_storage_test.c
        native/nvn/nvn_vk_program_result.c native/direct/json.c)
    target_include_directories(direct_nvn_compute_storage_test PRIVATE native native/direct native/nvn
        ../graphics/nvn/gfx/nvn)
    target_compile_options(direct_nvn_compute_storage_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_nvn_compute_storage COMMAND direct_nvn_compute_storage_test)
    add_executable(direct_nvn_program_result_test tests/direct_nvn_program_result_test.c
        native/nvn/nvn_vk_program_result.c native/direct/json.c)
    target_include_directories(direct_nvn_program_result_test PRIVATE native native/direct native/nvn)
    target_compile_options(direct_nvn_program_result_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_nvn_program_result COMMAND direct_nvn_program_result_test)
    # READY 的 GPU 身份行：direct provider 必须打与 generic 同语义的真实设备行。
    add_executable(direct_vk_identity_test tests/direct_vk_identity_test.c native/direct/vk_identity.c)
    target_include_directories(direct_vk_identity_test PRIVATE native native/direct)
    target_compile_options(direct_vk_identity_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_vk_identity COMMAND direct_vk_identity_test)
    # #968：真实 first_draw 附件拒绝路径不清理未创建 trace 资源。
    # 夹具依赖 Linux POSIX/ELF；Mac 由 linux_ctest.sh 经 colima 跑，不伪装 Darwin 通过。
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Compile during the build phase, not inside linux_ctest.sh's 30-second test budget.
        add_custom_target(direct_nvn_trace_init_test ALL
            COMMAND "${CMAKE_COMMAND}" -E env
                "CC=${CMAKE_C_COMPILER}"
                "S2A_TRACE_TEST_BUILD=${CMAKE_CURRENT_BINARY_DIR}/trace_init"
                bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/trace_init_test.sh" --build-only
            VERBATIM)
        add_test(NAME android_direct_nvn_trace_init
            COMMAND "${CMAKE_COMMAND}" -E env
                "CC=${CMAKE_C_COMPILER}"
                "S2A_TRACE_TEST_BUILD=${CMAKE_CURRENT_BINARY_DIR}/trace_init"
                bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/trace_init_test.sh" --run-only)
    endif()
    # §8.5.11：管线对象缓存的完整规范配方 + 碰撞反例（hash 相同也必须逐字节相等）。
    add_executable(direct_nvn_pipe_recipe_test tests/direct_nvn_pipe_recipe_test.c
        native/nvn/nvn_vk_pipe_recipe.c)
    target_include_directories(direct_nvn_pipe_recipe_test PRIVATE native native/direct native/nvn
        tests/vkhost ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include
        ../graphics/nvn/third_party/vulkan-headers/include)
    target_compile_options(direct_nvn_pipe_recipe_test PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
    add_test(NAME android_direct_nvn_pipe_recipe COMMAND direct_nvn_pipe_recipe_test)
    # #1382：直连 MRT 的槽位计划（2/3/4 槽、空槽 VK_ATTACHMENT_UNUSED、混合格式、depth-only、
    # 重复槽/尺寸不一致/不可作颜色附件的格式准确报红）与逐槽颜色混合状态。
    # 负控：测试内嵌“旧实现只接槽 0”的规则复刻，同一批正例在旧规则下必须全部报红。
    add_executable(direct_nvn_mrt_test tests/direct_nvn_mrt_test.c
        native/nvn/nvn_vk_mrt.c native/nvn/nvn_vk_format.c)
    target_include_directories(direct_nvn_mrt_test PRIVATE native native/direct native/nvn
        tests/vkhost ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include
        ../graphics/nvn/third_party/vulkan-headers/include)
    target_compile_options(direct_nvn_mrt_test PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
    add_test(NAME android_direct_nvn_mrt COMMAND direct_nvn_mrt_test)
    # §8.5.11：驱动管线缓存落盘/恢复的文件层（UUID/版本隔离、损坏拒绝、原子覆盖）。
    add_executable(direct_nvn_driver_cache_test tests/direct_nvn_driver_cache_test.c
        native/nvn/nvn_vk_driver_cache.c native/direct/sha256.c)
    target_include_directories(direct_nvn_driver_cache_test PRIVATE native native/direct native/nvn)
    target_compile_options(direct_nvn_driver_cache_test PRIVATE -Wall -Wextra -Werror -D_GNU_SOURCE)
    add_test(NAME android_direct_nvn_driver_cache COMMAND direct_nvn_driver_cache_test)
    # F4.2B：NVN 纹理格式/目标 → 编译器 TextureFormat/SamplerType（未映射一律拒绝）。
    add_executable(direct_texture_facts_test tests/direct_texture_facts_test.c native/direct/texture_facts.c)
    target_include_directories(direct_texture_facts_test PRIVATE native ../runtime/include
        ../graphics/nvn/gfx/nvn/include)
    target_compile_options(direct_texture_facts_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_texture_facts COMMAND direct_texture_facts_test)
    # Bind*Deferred 句柄记录的回放解析（bh_fresh 语义）：重基、基址尾校验、读失败准确报红。
    add_executable(direct_bind_defer_test tests/direct_bind_defer_test.c native/direct/bind_defer.c)
    target_include_directories(direct_bind_defer_test PRIVATE native ../runtime/include
        ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include)
    target_compile_options(direct_bind_defer_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_bind_defer COMMAND direct_bind_defer_test)
    # OP_COPY_T2T 的回放计划（flags/视图/区域 → copy 或 blit）：8E Wonder 在 READY+73.8s
    # 撞上 op=35 unsupported；全部组合只按计划器语义过/拒，不兜底。
    add_executable(direct_t2t_plan_test tests/direct_t2t_plan_test.c native/direct/t2t_plan.c)
    target_include_directories(direct_t2t_plan_test PRIVATE native ../runtime/include
        ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include)
    target_compile_options(direct_t2t_plan_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_t2t_plan COMMAND direct_t2t_plan_test)
    # op31 独立复制计划合同；尚未接 record，四类变异在 test.sh 的 Python 入口跑。
    add_executable(buffer_texture_plan_test tests/buffer_texture_plan_test.c
        native/direct/buffer_texture_plan.c native/nvn/nvn_vk_format.c)
    target_include_directories(buffer_texture_plan_test PRIVATE native native/nvn tests/vkhost
        ../graphics/nvn/gfx/nvn ../graphics/nvn/third_party/vulkan-headers/include)
    target_compile_options(buffer_texture_plan_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_buffer_texture_plan COMMAND buffer_texture_plan_test)
    # F4.4：有界需求编译 worker（去重/单槽/pause/Stop/超时/重启）。
    add_executable(direct_compile_worker_test tests/direct_compile_worker_test.c native/direct/compile_worker.c)
    target_include_directories(direct_compile_worker_test PRIVATE native ../runtime/include)
    target_compile_options(direct_compile_worker_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(direct_compile_worker_test PRIVATE Threads::Threads)
    add_test(NAME android_direct_compile_worker COMMAND direct_compile_worker_test)
    add_executable(direct_control_test tests/direct_control_test.c native/direct/control.c)
    target_include_directories(direct_control_test PRIVATE native ../runtime/include)
    target_compile_options(direct_control_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_direct_control COMMAND direct_control_test)
    add_executable(ui_session_test tests/ui_session_test.c native/host_ui.c native/host_ui_android.c)
    target_include_directories(ui_session_test PRIVATE native ../runtime/include)
    target_compile_options(ui_session_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(ui_session_test PRIVATE pthread)
    add_test(NAME android_ui_session COMMAND ui_session_test)
    # 程序生命周期 pin 记账（与 hle_nvn.c 共用 header-only 实现）：去重、owner 拷贝、换 SetShaders、已释放 serial。
    add_executable(nvn_program_pins_test tests/nvn_program_pins_test.c)
    target_include_directories(nvn_program_pins_test PRIVATE ../graphics/nvn/gfx/nvn)
    target_compile_options(nvn_program_pins_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_nvn_program_pins COMMAND nvn_program_pins_test)
    # 呈现/窗口生命周期纯决策：无 surface、generation 变化、旋转重建、呈现退休预算。
    add_executable(nvn_vk_lifecycle_test tests/nvn_vk_lifecycle_test.c native/nvn/nvn_vk_lifecycle.c)
    target_include_directories(nvn_vk_lifecycle_test PRIVATE native/nvn)
    target_compile_options(nvn_vk_lifecycle_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_nvn_vk_lifecycle COMMAND nvn_vk_lifecycle_test)
    # 真实呈现退休路径的宿主回归：fake Vulkan 驱动生产 nvn_vk_swapchain.c。
    # 覆盖 present fence 与渲染 fence 分离、无后续帧 Stop 保留、present/渲染失败保留、
    # acquire 超时跳过、maintenance1 准入；旋转/换代判定在 nvn_vk_lifecycle_test。
    # 真机驱动行为仍需设备验证。
    add_executable(nvn_vk_swapchain_host_test tests/nvn_vk_swapchain_host_test.c
        native/nvn/nvn_vk_swapchain.c native/nvn/nvn_vk_surface.c native/game_touch.c)
    target_include_directories(nvn_vk_swapchain_host_test PRIVATE
        native native/nvn native/direct ../runtime/include tests/hostvk tests/vkhost
        ../graphics/nvn/third_party/vulkan-headers/include
        ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include ../graphics/nvn
        ../graphics/nvn/host ../graphics/nvn/gfx/tex native/nvn/compat ../runtime/src)
    target_link_libraries(nvn_vk_swapchain_host_test PRIVATE pthread)
    target_compile_definitions(nvn_vk_swapchain_host_test PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
    target_compile_options(nvn_vk_swapchain_host_test PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
    add_test(NAME android_nvn_vk_swapchain_host COMMAND nvn_vk_swapchain_host_test)
    # F4.4：执行包 owner 控制（单包/挂起/恢复/Stop/换代/协议错误）。纯状态机，生产 adapter 共用。
    add_executable(nvn_vk_exec_owner_test tests/nvn_vk_exec_owner_test.c native/nvn/nvn_vk_exec_owner.c)
    target_include_directories(nvn_vk_exec_owner_test PRIVATE native/nvn)
    target_compile_options(nvn_vk_exec_owner_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(nvn_vk_exec_owner_test PRIVATE Threads::Threads)
    add_test(NAME android_nvn_vk_exec_owner COMMAND nvn_vk_exec_owner_test)
    # NVNtextureView → Vulkan 子资源视图的纯决策（无 Vulkan 调用）：
    # 3D depth slice 附件 2D 视图、array/cube 单层视图、越界仍准确拒绝。
    add_executable(nvn_vk_texview_plan_test tests/nvn_vk_texview_plan_test.c)
    target_include_directories(nvn_vk_texview_plan_test PRIVATE
        native/nvn ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include
        tests/vkhost ../graphics/nvn/third_party/vulkan-headers/include)
    target_compile_options(nvn_vk_texview_plan_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_nvn_vk_texview_plan COMMAND nvn_vk_texview_plan_test)
    # F4.4：driver pipeline worker（worker 创建、owner take、配方去重、设备代际、有界 Stop、cache 锁）。
    # fake Vulkan 直接驱动生产实现；热路径接线不在宿主覆盖范围，见报告。
    add_executable(nvn_vk_pipe_worker_test tests/nvn_vk_pipe_worker_test.c
        native/nvn/nvn_vk_pipe_worker.c native/nvn/nvn_vk_pipe_recipe.c)
    target_include_directories(nvn_vk_pipe_worker_test PRIVATE
        native native/direct native/nvn tests/vkhost
        ../graphics/nvn/third_party/vulkan-headers/include
        ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include)
    target_compile_definitions(nvn_vk_pipe_worker_test PRIVATE _GNU_SOURCE)
    target_compile_options(nvn_vk_pipe_worker_test PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
    target_link_libraries(nvn_vk_pipe_worker_test PRIVATE Threads::Threads)
    add_test(NAME android_nvn_vk_pipe_worker COMMAND nvn_vk_pipe_worker_test)
    # #784 D2–D7 回放核心夹具（宿主，无 GPU）：.in/.exp 样例解析、格式往返、
    # 字节级比对器（自比绿 / 翻一字节报位 / 截断报错）。D2–D7 后端拿它当契约回放。
    add_executable(replay_fixture_test tests/replay_fixture/t_rfx.c tests/replay_fixture/rfx.c)
    target_include_directories(replay_fixture_test PRIVATE tests/replay_fixture)
    target_compile_options(replay_fixture_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME android_replay_fixture COMMAND replay_fixture_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_fixture/samples")
    # 金样可重现：rfx_gen 重出样例与提交的 samples/ 逐字节一致。
    add_test(NAME android_replay_fixture_repro
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_fixture/run_test.sh")
endif()

