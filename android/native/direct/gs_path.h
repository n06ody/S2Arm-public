/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 S2Arm contributors */
#pragma once

/* GS path decision for the direct Vulkan backend (#1482).
 *
 * hw_geometry_shader is the device's feature bit
 * (VkPhysicalDeviceFeatures.geometryShader). Returns 1 when the
 * hardware GS path should be taken (enable geometryShader on the
 * device), 0 for the proven compute-translation fallback. *reason
 * receives a static short string naming the deciding factor.
 *
 * One positive log line is emitted per call —
 * "graphics GS path: hardware|compute reason=<why>" — because the
 * 9400 A/B readings grep the session log for the selected path, not
 * only for a fallback warning.
 *
 * debug.s2a.gs_path={compute|hardware} forces the leg, behind both
 * debug gates (native/debug_gate.h: Debug build's
 * S2A_ANDROID_DEBUG_ENV + the debug.s2a.debug_env=1 property), so it
 * is inert in player builds and cannot be set by ordinary apps.
 *
 * Weak: host link units that do not link this provider see a NULL
 * symbol — "no decision established" — which is the same safe
 * direction as an absent verdict: geometryShader stays off. This keeps
 * probes that link only vk_device.c (the on-device scripts under
 * android/tools) linkable, matching the weak
 * s2a_capprobe_gs_occ_state contract in capprobe.h. */
int s2a_vk_gs_path(int hw_geometry_shader, const char **reason) __attribute__((weak));

/* 诊断（Adreno / Mali 分叉定位，只在调试档生效，同 gs_path 的门）：
 *   debug.s2a.caps_no_vtx_stores=1 → 设备能力里 vertexPipelineStoresAndAtomics 报 false（编译器按没有处理，同 Mali）；
 *   debug.s2a.caps_subgroup=N      → 设备能力里 subgroupSize 报 N（只改编译用的宽度，不改硬件；用于看结果是否随宽度变）。
 * 返回 0 = 不改。弱符号：不链这个文件的宿主单元看到 NULL，等于不改。 */
int s2a_caps_force_no_vertex_stores(void) __attribute__((weak));
unsigned s2a_caps_force_subgroup(void) __attribute__((weak));
