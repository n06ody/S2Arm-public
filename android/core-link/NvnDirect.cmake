# The diagnostic WSI probes and the game frontend use the same Vulkan core.
# Keep its sources, header search paths and per-source options together. Including
# this file only defines the function; apply it after the production target exists.
function(s2a_android_direct_core target)
    # 直连源清单由 LinkSources.cmake 的 s2a_android_direct_sources() 单点维护，
    # 生产构建与 Q19 源码级符号扫描读同一份，避免门禁漏掉新增导出。
    s2a_android_direct_sources(direct_sources)
    target_sources(${target} PRIVATE ${direct_sources})
    set_source_files_properties(
        native/nvn/nvn_vk_bcdec.c native/nvn/nvn_vk_format.c native/nvn/nvn_vk_driver_cache.c
        PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers")
    set_source_files_properties(../graphics/nvn/gfx/nvn/nvn_tex.c ../graphics/nvn/gfx/tex/nvn_deswizzle.c
        ../graphics/nvn/gfx/nvn/nvn_dirty.c PROPERTIES COMPILE_OPTIONS "-Wno-error")
    # Paths are relative to android, as are target_sources above.
    set(includes ../..)
    list(APPEND includes ../graphics/nvn/gfx/nvn ../graphics/nvn/gfx/nvn/include ../graphics/nvn
        ../graphics/nvn/host ../graphics/nvn/gfx/tex native/nvn native/nvn/compat native/direct ../runtime/src
        ../graphics/nvn/replay native)
    target_include_directories(${target} PRIVATE ${includes})
    target_compile_definitions(${target} PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
    target_link_options(${target} PRIVATE -Wl,--wrap=_exit -Wl,--wrap=exit)

    set(S2A_ANDROID_PGO "OFF" CACHE STRING "Android game replay PGO mode: OFF, GEN or USE")
    set_property(CACHE S2A_ANDROID_PGO PROPERTY STRINGS OFF GEN USE)
    set(S2A_ANDROID_PGO_PROFILE "" CACHE FILEPATH "Indexed Clang profile for Android game replay (USE)")
    if(NOT S2A_ANDROID_PGO MATCHES "^(OFF|GEN|USE)$")
        message(FATAL_ERROR "S2A_ANDROID_PGO must be OFF, GEN or USE")
    endif()
    if(NOT S2A_ANDROID_PGO STREQUAL "USE" AND NOT S2A_ANDROID_PGO_PROFILE STREQUAL "")
        message(FATAL_ERROR "S2A_ANDROID_PGO_PROFILE requires S2A_ANDROID_PGO=USE")
    endif()
    if(S2A_ANDROID_PGO STREQUAL "OFF")
        return()
    endif()
    if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a"
            OR NOT S2A_ANDROID_NATIVE_MODE STREQUAL "game" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "S2A_ANDROID_PGO requires an Android arm64-v8a game build with NDK Clang")
    endif()

    # Android trains the new replay and its Vulkan backend with the same source set in both modes.
    set(pgo_sources ${direct_sources})
    list(FILTER pgo_sources INCLUDE REGEX "^([.][.]/graphics/nvn/replay/rp_.*[.]c|native/nvn/backend_vulkan[.]c)$")
    if(S2A_ANDROID_PGO STREQUAL "GEN")
        set_property(SOURCE ${pgo_sources} APPEND PROPERTY COMPILE_OPTIONS -fprofile-instr-generate)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
    else()
        set(profile "${S2A_ANDROID_PGO_PROFILE}")
        if(NOT IS_ABSOLUTE "${profile}" OR NOT EXISTS "${profile}" OR IS_DIRECTORY "${profile}")
            message(FATAL_ERROR "S2A_ANDROID_PGO=USE requires S2A_ANDROID_PGO_PROFILE pointing to an existing indexed profile")
        endif()
        # Keep Clang's profile diagnostics fatal and rebuild these objects when the profile changes.
        set_property(SOURCE ${pgo_sources} APPEND PROPERTY COMPILE_OPTIONS
            "-fprofile-instr-use=${profile}" -Werror=profile-instr-missing
            -Werror=profile-instr-out-of-date -Werror=profile-instr-unprofiled)
        set_property(SOURCE ${pgo_sources} APPEND PROPERTY OBJECT_DEPENDS "${profile}")
    endif()
endfunction()
