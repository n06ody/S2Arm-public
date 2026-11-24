// 整台设备能力形态档下的建设备核对（宿主；假驱动 fake_vk.c 按 S2A_CAPS_SHAPE 回答，run.sh 对三档各跑一遍）。
//   被测是生产代码原样：android/native/direct/vk_device.c（能力 → 启用）、device_caps.c（启用 → s2a-device-caps-v1）、
//   nvn_vk_format.c（深度替换选择）、gs_path.c（几何着色器路径：能力闸先于 capprobe 判定）、
//   backend_vulkan.c 的 array_query_default（run.sh 原样摘出来编进本文件）、dev_facts.c（设备事实 → 设备档案，#1749 M0）。
//   期望值只从形态档数据按规则算（扩展列没列、特性位、格式特性位），不调被测函数求期望。
// 用法：S2A_CAPS_SHAPE=<档名> S2A_TEST_FILES_DIR=<空目录> vk_device_shape_test <caps JSON 输出> <设备档案输出>
//   档名 = android/tests/devprofiles/ 下的文件名（shapes.py names）；设备档案输出由 run.sh 拿去和档案文件逐字段比（shapes.py compare）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "caps_shape.h"
#include "vk_device.h"
#include "device_caps.h"
#include "dev_facts.h"
#include "nvn_vk_format.h"
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

// backend_vulkan.c 的 array_query_default 读 g.dev：摘出来的原文接在这个 g 上
static struct { struct s2a_vk_device *dev; } g;
#include "array_query_default.inc"
// capprobe 判定固定为「支持」（收益闸放行）：几何着色器开不开只剩能力闸——设备不支持就必须关
const char *s2a_capprobe_gs_occ_state(void) { return "caps-shape-test:supported"; }
// bridge.cpp 的 App files 目录：dev_facts.c 的建设备挂点往 <它>/devprofile/ 写档案
const char *s2a_android_gpu_driver_files_dir(void) { return getenv("S2A_TEST_FILES_DIR"); }
static int in_fact_table(VkFormat f) {
    VkFormat g;
    for (size_t i = 0; s2a_dev_facts_format_name(i, &g); i++) if (g == f) return 1;
    return 0;
}
static int write_file(const char *path, const char *text, size_t n) {
    FILE *f = fopen(path, "w");
    return f && fwrite(text, 1, n, f) == n && fclose(f) == 0 ? 0 : -1;
}

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: [%s] ", cs_shape()->name); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int popcount16(uint32_t m) { int n = 0; for (int i = 0; i < 16; i++) n += (m >> i) & 1; return n; }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "用法：S2A_CAPS_SHAPE=<档> S2A_TEST_FILES_DIR=<目录> %s <caps JSON 输出> <设备档案输出>\n", argv[0]); return 2; }
    const struct cs_shape *s = cs_shape();
    // 档案目录里先放两份旧代（一份更旧、一份较新）和一个无关文件：建设备的挂点写新一代后留最近 2 代——
    //   删更旧的、留较新的那份旧代，不碰无关文件
    const char *files = getenv("S2A_TEST_FILES_DIR");
    char dir[512], stale[600], prev[600], keep[600];
    if (!files || snprintf(dir, sizeof dir, "%s/devprofile", files) >= (int)sizeof dir) { printf("FAIL: [%s] 没给 S2A_TEST_FILES_DIR\n", s->name); return 1; }
    snprintf(stale, sizeof stale, "%s/0123456789abcdef.json", dir);
    snprintf(prev, sizeof prev, "%s/fedcba9876543210.json", dir);
    snprintf(keep, sizeof keep, "%s/keep.txt", dir);
    if ((mkdir(dir, 0700) && errno != EEXIST) || write_file(stale, "{}\n", 3) || write_file(prev, "{}\n", 3) || write_file(keep, "x\n", 2)) {
        printf("FAIL: [%s] 备不了档案目录 %s\n", s->name, dir); return 1;
    }
    {
        struct timeval now; gettimeofday(&now, NULL);
        struct timeval old[2] = { { now.tv_sec - 7200, 0 }, { now.tv_sec - 7200, 0 } }, newer[2] = { { now.tv_sec - 60, 0 }, { now.tv_sec - 60, 0 } };
        if (utimes(stale, old) || utimes(prev, newer)) { printf("FAIL: [%s] 改不了旧代档案的修改时间\n", s->name); return 1; }
    }
    struct s2a_vk_device d;
    char why[256] = "";
    const int create_rc = s2a_vk_device_create(&d, why, sizeof why);
    if (create_rc && s->props.apiVersion < VK_API_VERSION_1_1 && strstr(why, "outside the supported range")) {
        // 1.0 设备：后端按版本下限拒绝建设备（预期），但挂点在下限之前、事实照记——核落盘、核不挂扩展查询链，输出生成的档案后结束
        static struct s2a_dev_facts f0; static char pj[64 << 10]; size_t pl = 0; char dg[65] = "", path[640]; struct stat st;
        cs_features2_calls = cs_properties2_calls = 0;
        s2a_dev_facts_collect(d.physical, &f0);
        EXPECT(cs_features2_calls == 0 && cs_properties2_calls == 0, "1.0 设备现查仍调了 Features2 %u 次 / Properties2 %u 次", cs_features2_calls, cs_properties2_calls);
        EXPECT(s2a_dev_profile_json(&f0, pj, sizeof pj, &pl, dg) == 0, "s2a_dev_profile_json 失败");
        snprintf(path, sizeof path, "%s/%.16s.json", dir, dg);
        EXPECT(stat(path, &st) == 0, "1.0 设备被拒之前没落盘档案 %s", path);
        if (write_file(argv[2], pj, pl)) { printf("FAIL: [%s] 写不了 %s\n", s->name, argv[2]); return 1; }
        if (fails) return 1;
        printf("PASS: [%s] 1.0 设备按下限拒绝建设备（%s），事实照记：落盘、未调 Features2 / Properties2\n", s->name, why);
        return 0;
    }
    if (create_rc) {
        printf("FAIL: [%s] 建设备失败：%s%s%s\n", s->name, why, cs_create_device_why[0] ? "；假驱动：" : "", cs_create_device_why);
        return 1;
    }

    EXPECT(!d.eds1_enabled && !d.eds2_enabled && !d.vertex_input_dynamic_enabled,
           "动态状态三项必须缺省关");
    // 格式列表（C2，保 Adreno UBWC 那条）：设备列出 VK_KHR_image_format_list 才走列表模式，没列出走自由 MUTABLE
    const int fmt_list = cs_has_ext(s, "VK_KHR_image_format_list");
    EXPECT(d.image_format_list_enabled == fmt_list, "image_format_list_enabled=%d，形态档列出扩展=%d", d.image_format_list_enabled, fmt_list);
    // 自定义边框色（C3）：扩展 + customBorderColors 特性都有才开；没开时后端就近取内置档
    const int border = cs_has_ext(s, "VK_EXT_custom_border_color") && s->customBorderColors;
    EXPECT(d.custom_border_enabled == border, "custom_border_enabled=%d，期望 %d", d.custom_border_enabled, border);
    // 描述符索引：扩展列出、1.1 设备、四位支持位齐全才整组启用
    const int di = cs_has_ext(s, "VK_EXT_descriptor_indexing") && s->props.apiVersion >= VK_API_VERSION_1_1 &&
                   s->diRuntimeArray && s->diPartiallyBound && s->diSampledNonUniform && s->diStorageNonUniform;
    EXPECT(d.descriptor_indexing_enabled == di, "descriptor_indexing_enabled=%d，期望 %d", d.descriptor_indexing_enabled, di);
    // BC 路由（C6，#1199）：bit i ↔ NVN 格式 0x41+i，逐格式查原生采样位，缺的走计算解码；两者互补。
    //   期望按 NVN 格式映射（nvn_vk_format.c）逐位由形态档的 optimal 采样位推出，不另抄 vk_device.c 的表；半有半无（拼装档 Xclipse 920 的 BC1–3）也覆盖
    uint32_t bc_want = 0;
    for (uint32_t i = 0; i < 16; i++) {
        const VkFormat vf = nvn_vk_format(0x41u + i);
        EXPECT(vf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && vf <= VK_FORMAT_BC7_SRGB_BLOCK, "NVN 格式 0x%02x 映射到 %d，不是 BC 格式", 0x41u + i, (int)vf);
        if (cs_format(s, vf).optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) bc_want |= 1u << i;
    }
    EXPECT(d.bc_native_mask == bc_want, "bc_native_mask=0x%04x，期望 0x%04x（形态档逐格式采样位）", d.bc_native_mask, bc_want);
    EXPECT(d.bc_decode_mask == (~d.bc_native_mask & 0xffffu) && popcount16(d.bc_native_mask) + popcount16(d.bc_decode_mask) == 16,
           "bc_decode_mask=0x%04x 不是原生位的补集", d.bc_decode_mask);
    // 深度替换（C1）：D24S8 不能当深度模板附件、D32S8 能 → 换 D32S8
    const VkFormatFeatureFlags att = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    const int d24_subst = !(cs_format(s, VK_FORMAT_D24_UNORM_S8_UINT).optimalTilingFeatures & att) &&
                          (cs_format(s, VK_FORMAT_D32_SFLOAT_S8_UINT).optimalTilingFeatures & att);
    EXPECT(((d.depth_subst_mask & 1u) != 0) == d24_subst, "depth_subst_mask=0x%x，期望 D24S8 替换=%d", d.depth_subst_mask, d24_subst);
    // 着色器能力位（C4 / C5）：有就启用、没有不启用；64 位与曲面细分本后端从不启用
    EXPECT(d.core_features.shaderInt16 == s->features.shaderInt16, "shaderInt16 启用=%u，支持=%u", d.core_features.shaderInt16, s->features.shaderInt16);
    EXPECT(d.core_features.shaderStorageImageReadWithoutFormat == s->features.shaderStorageImageReadWithoutFormat,
           "shaderStorageImageReadWithoutFormat 启用=%u，支持=%u", d.core_features.shaderStorageImageReadWithoutFormat, s->features.shaderStorageImageReadWithoutFormat);
    EXPECT(d.core_features.vertexPipelineStoresAndAtomics == s->features.vertexPipelineStoresAndAtomics,
           "vertexPipelineStoresAndAtomics 启用=%u，支持=%u", d.core_features.vertexPipelineStoresAndAtomics, s->features.vertexPipelineStoresAndAtomics);
    EXPECT(d.core_features.shaderClipDistance == s->features.shaderClipDistance, "shaderClipDistance 启用=%u，支持=%u",
           d.core_features.shaderClipDistance, s->features.shaderClipDistance);
    EXPECT(!d.core_features.shaderFloat64 && !d.core_features.shaderInt64 && !d.core_features.tessellationShader,
           "64 位 / 曲面细分不该启用（%u %u %u）", d.core_features.shaderFloat64, d.core_features.shaderInt64, d.core_features.tessellationShader);
    // 几何着色器（#1456 / #1482）：判定已放行，开不开 = 设备支不支持
    EXPECT(d.core_features.geometryShader == s->features.geometryShader, "geometryShader 启用=%u，支持=%u（判定放行）",
           d.core_features.geometryShader, s->features.geometryShader);
    // 数组层夹取缺省（N1）：三档都不是要它的驱动，缺省必须关（开了每次数组采样白多一次尺寸查询和夹取）
    g.dev = &d;
    EXPECT(array_query_default() == 0, "array_query_default()=%d：%s 档不该缺省开数组层夹取", array_query_default(), s->name);

    // 设备事实（#1749 M0）：挂点已在建设备时落盘一份；这里再现查一次，两份必须逐字节相同（同一台设备、同一代）
    static struct s2a_dev_facts facts;
    static char profile[64 << 10]; size_t plen = 0; char digest[65] = "";
    EXPECT(s2a_dev_facts_collect(d.physical, &facts) == 0, "s2a_dev_facts_collect 失败");
    EXPECT(s2a_dev_profile_json(&facts, profile, sizeof profile, &plen, digest) == 0, "s2a_dev_profile_json 失败");
    {
        char path[640]; static char disk[64 << 10];
        snprintf(path, sizeof path, "%s/%.16s.json", dir, digest);
        FILE *f = fopen(path, "r");
        size_t n = f ? fread(disk, 1, sizeof disk, f) : 0;
        if (f) fclose(f);
        EXPECT(f && n == plen + 1 && !memcmp(disk, profile, plen) && disk[plen] == '\n',
               "建设备时落盘的 %s 和现查生成的档案不同（%zu / %zu 字节）", path, n, plen + 1);
        struct stat st;
        EXPECT(stat(stale, &st) != 0 && errno == ENOENT, "第三代（更旧的）档案 %s 没被删（只留最近 2 代）", stale);
        EXPECT(stat(prev, &st) == 0, "上一代档案 %s 被删了（应留最近 2 代）", prev);
        EXPECT(stat(keep, &st) == 0, "档案目录里的无关文件 %s 被删了", keep);
    }
    // 摘要行：同一进程同一代只打一次（设备丢失后重建同一台设备不刷屏）——建设备时打过了，再调一次挂点必须不打
    EXPECT(s2a_dev_facts_on_device_create(d.physical) == 0, "同一进程同一代第二次建设备又打了【事实】行");
    // 低于 1.1 的设备：整条扩展查询链挡住，现查不调 Features2 / Properties2
    cs_features2_calls = cs_properties2_calls = 0;
    {
        static struct s2a_dev_facts f2;
        s2a_dev_facts_collect(d.physical, &f2);
        if (s->props.apiVersion < VK_API_VERSION_1_1)
            EXPECT(cs_features2_calls == 0 && cs_properties2_calls == 0, "1.0 设备现查仍调了 Features2 %u 次 / Properties2 %u 次",
                   cs_features2_calls, cs_properties2_calls);
        else
            EXPECT(cs_features2_calls == 1 && cs_properties2_calls == 1, "1.1+ 设备现查应各调 1 次 Features2 / Properties2，实为 %u / %u",
                   cs_features2_calls, cs_properties2_calls);
    }
    // 摘要只收静态事实：假 sysfs 里改 scaling_max_freq（温控 / 省电会改）摘要不变；改 cpuinfo_max_freq（硬件上限）摘要变
    {
        char root[600], d0[700], pth[760];
        snprintf(root, sizeof root, "%s/fake-sysfs", files);
        static const char *const parts[] = { "/sys", "/sys/devices", "/sys/devices/system", "/sys/devices/system/cpu",
                                             "/sys/devices/system/cpu/cpu0", "/sys/devices/system/cpu/cpu0/cpufreq" };
        mkdir(root, 0700);
        for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) { snprintf(d0, sizeof d0, "%s%s", root, parts[i]); mkdir(d0, 0700); }
        #define SYSW(file, text) do { snprintf(pth, sizeof pth, "%s/sys/devices/system/cpu/cpu0/%s", root, file); \
                                      if (write_file(pth, text, strlen(text))) { printf("FAIL: [%s] 写不了假 sysfs %s\n", s->name, pth); return 1; } } while (0)
        SYSW("cpu_capacity", "1024\n"); SYSW("cpufreq/cpuinfo_max_freq", "3000000\n"); SYSW("cpufreq/scaling_max_freq", "3000000\n");
        setenv("S2A_TEST_SYSFS_ROOT", root, 1);
        static struct s2a_dev_facts fs; static char pj[64 << 10]; size_t pl; char da[65], db[65], dc[65];
        s2a_dev_facts_collect(d.physical, &fs); s2a_dev_profile_json(&fs, pj, sizeof pj, &pl, da);
        EXPECT(fs.ncpu > 0 && fs.capacity[0] == 1024 && fs.max_khz[0] == 3000000, "假 sysfs 没读到：cpu0 capacity=%u maxKHz=%u", fs.capacity[0], fs.max_khz[0]);
        SYSW("cpufreq/scaling_max_freq", "1200000\n");
        s2a_dev_facts_collect(d.physical, &fs); s2a_dev_profile_json(&fs, pj, sizeof pj, &pl, db);
        EXPECT(!strcmp(da, db), "scaling_max_freq 一变摘要就变（%.16s → %.16s）：会变的量进了摘要", da, db);
        SYSW("cpufreq/cpuinfo_max_freq", "3200000\n");
        s2a_dev_facts_collect(d.physical, &fs); s2a_dev_profile_json(&fs, pj, sizeof pj, &pl, dc);
        EXPECT(strcmp(da, dc) != 0 && fs.max_khz[0] == 3200000, "cpuinfo_max_freq 变了摘要却没变（%.16s）", dc);
        #undef SYSW
        unsetenv("S2A_TEST_SYSFS_ROOT");
    }
    if (write_file(argv[2], profile, plen)) { printf("FAIL: [%s] 写不了 %s\n", s->name, argv[2]); return 1; }

    // 启用 → caps JSON（编译器 profile 的唯一来源）：限值原样进 JSON
    static char json[16384]; size_t len = 0;
    EXPECT(s2a_vk_device_caps_json(&d, json, sizeof json, &len) == 0, "s2a_vk_device_caps_json 失败");
    char want[96];
    snprintf(want, sizeof want, "\"maxComputeSharedMemorySize\":%u,", s->props.limits.maxComputeSharedMemorySize);
    EXPECT(strstr(json, want) != NULL, "caps JSON 里没有 %s", want);
    snprintf(want, sizeof want, "\"maxPerStageDescriptorUniformBuffers\":%u,", s->props.limits.maxPerStageDescriptorUniformBuffers);
    EXPECT(strstr(json, want) != NULL, "caps JSON 里没有 %s", want);
    snprintf(want, sizeof want, "\"shaderStorageImageReadWithoutFormat\":%s}", s->features.shaderStorageImageReadWithoutFormat ? "true" : "false");
    EXPECT(strstr(json, want) != NULL, "caps JSON 里没有 %s", want);
    // 格式表覆盖：建设备与出 caps JSON 时查过的每个格式（device_caps.c 的查询在上面这步）、NVN 映射（本形态下的深度替换 / 16 位退回）出来的每个格式，都得在设备事实的格式表里
    for (uint32_t i = 0; i < cs_queried_fmt_count; i++)
        EXPECT(in_fact_table(cs_queried_fmt[i]), "建设备时查了格式 %d，设备事实的格式表（dev_facts.c S2A_FACT_FORMATS）里没有", (int)cs_queried_fmt[i]);
    int mapped = 0;
    for (uint32_t f = 0; f < 0x200; f++) {
        VkFormat vf = nvn_vk_format(f);
        if (vf == VK_FORMAT_UNDEFINED) continue;
        mapped++;
        EXPECT(in_fact_table(vf), "NVN 格式 0x%x 映射到 VkFormat %d，设备事实的格式表里没有", f, (int)vf);
    }
    EXPECT(mapped > 100 && cs_queried_fmt_count > 30, "覆盖核对的射程不对：NVN 映射 %d 个、建设备查询 %u 个", mapped, cs_queried_fmt_count);
    FILE *f = fopen(argv[1], "w");
    if (!f || fwrite(json, 1, len, f) != len || fclose(f)) { printf("FAIL: [%s] 写不了 %s\n", s->name, argv[1]); return 1; }

    if (fails) return 1;
    printf("PASS: [%s] 建设备 + 17 项能力断言 + 设备档案 9 项（落盘 = 现查、留最近 2 代、留无关文件、摘要行每代一次、1.0 挡住扩展查询链、scaling_max_freq 不进摘要、cpuinfo_max_freq 进摘要、格式表覆盖 NVN 映射 %d 个 / 建设备查询 %u 个）（格式列表=%d 自定义边框=%d 描述符索引=%d BC 原生=0x%04x D24 换 D32=%d Int16=%u 无格式读=%u 顶点存储=%u 几何=%u 数组层夹取缺省=0；启用扩展 %u 个）\n",
           s->name, mapped, cs_queried_fmt_count, d.image_format_list_enabled, d.custom_border_enabled, d.descriptor_indexing_enabled, d.bc_native_mask,
           d24_subst, d.core_features.shaderInt16, d.core_features.shaderStorageImageReadWithoutFormat,
           d.core_features.vertexPipelineStoresAndAtomics, d.core_features.geometryShader, cs_enabled_ext_count);
    return 0;
}
