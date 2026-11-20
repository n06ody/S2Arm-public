// compile_request 宿主测试：请求结构、字段来源标记、emit 复制语义。
// 不建 Vulkan、不跑游戏；用合成字节验证 native 唯一生产者的 JSON 形状与缺口清单。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "direct/compile_request.h"
#include "direct/kind_check.h"
#include "direct/xlate_debug.h"
#include "direct/sha256.h"
#include "direct/s2a_direct.h"

// kindCheck 调试开关的宿主矩阵：开发者包里“调试构建 + debug.s2a.debug_env=1 + debug.s2a.kind_check=0”
// 才在请求 JSON 带 kindCheck:false；门关或属性缺省一律不带该键（校验开着）。
// 宿主没有 bionic 的系统属性，要测这两支必须自己给桩（-I <桩目录> 里放 sys/system_properties.h）：
//   cc -D__ANDROID__ -DS2A_ANDROID_DEBUG_ENV=1 -DS2A_TEST_KIND_CHECK_STUB …   # 调试构建：门②可开
//   cc -D__ANDROID__ -DS2A_TEST_KIND_CHECK_STUB …                            # 正式构建：编译期门就关着
// 真机探针（tools/direct_c1_probe.sh）不定义 S2A_TEST_KIND_CHECK_STUB，走 bionic 自己的实现，
// 所以这里的桩只在显式打开时参与链接。
#ifdef S2A_TEST_KIND_CHECK_STUB
#if !defined(__ANDROID__)
#error "S2A_TEST_KIND_CHECK_STUB 需要 -D__ANDROID__ 模拟 bionic 的系统属性读取"
#endif
static const char *g_debug_env = "0";   // debug.s2a.debug_env 的桩值（门②）
static const char *g_kind_check = "";   // debug.s2a.kind_check 的桩值（缺省 = 不设）
static const char *g_xlate_debug = "";  // debug.s2a.xlate_debug 的桩值（缺省 = 不设）
int __system_property_get(const char *name, char *out) {
    const char *v = !strcmp(name, "debug.s2a.debug_env") ? g_debug_env :
                    !strcmp(name, "debug.s2a.kind_check") ? g_kind_check :
                    !strcmp(name, "debug.s2a.xlate_debug") ? g_xlate_debug : "";
    strcpy(out, v);
    return (int)strlen(v);
}
#endif

static int fails;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static int contains(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

// 开关从 native 判定端到端到请求 JSON：编译器只读 JSON 的 kindCheck（DirectShaderCompiler.cs:39），
// 所以“关掉”必须体现成 JSON 里的 false 键，“开着”必须是不带这个键（OptionalBool 缺省 true）。
// dump_path 非空时把这笔请求 JSON 原样落盘，供宿主门禁（direct_compile_request_host_test.py）
// 用 json.loads 复核键的位置/类型——只做证据输出，缺省不影响断言。
static void check_kind_check_contract(const struct s2a_compile_request_input *in, const char *label,
                                      const char *dump_path) {
    char *json = NULL;
    size_t len = 0;
    char gaps[512] = "";
    CHECK(s2a_compile_request_build(in, &json, &len, gaps, sizeof gaps) == 0, label);
    if (!json) return;
    if (s2a_direct_kind_check_enabled()) {
        CHECK(!contains(json, "kindCheck"), "kind check on: request JSON carries no kindCheck key");
    } else {
        CHECK(contains(json, "\"kindCheck\":false"), "kind check off: request JSON carries kindCheck:false");
    }
    // 可选键是条件写的，拼错分隔符会整段 JSON 非法（编译器的严格解析直接拒），不是开关问题。
    CHECK(!strstr(json, ",,"), "conditional kindCheck key keeps the JSON separators valid");
    // debugMode 恒显式写（含缺省 false）：sha256 覆盖整个请求 JSON，不带键的旧格式请求与
    // 「编译器 DebugMode 恒开」时期逐字节相同，会命中旧的膨胀产物缓存——显式写 false 让翻译旗
    // 口径落进身份、旧缓存条目自然 miss（「旧格式请求在新版本下不被命中」的机制层证明）。
    if (s2a_direct_xlate_debug_enabled()) {
        CHECK(contains(json, "\"debugMode\":true"), "xlate debug on: request JSON carries debugMode:true");
    } else {
        CHECK(contains(json, "\"debugMode\":false"), "xlate debug off: request JSON carries debugMode:false");
    }
    if (dump_path && *dump_path) {
        FILE *f = fopen(dump_path, "wb");
        if (f) { fwrite(json, 1, len, f); fclose(f); }
    }
    free(json);
}

// execute 只转发请求：开关不许走进程环境（PR #1603 曾在 execute 里 setenv，编译器不读 env → 开关失效）。
// 先复制成快照再比：getenv 返回的指针会被后续 setenv/getenv 复用，存指针在“回退真的 setenv”时反而是悬空的。
struct kind_env_state { int present; char value[128]; };

static void kind_env_capture(struct kind_env_state *st) {
    const char *now = getenv("S2A_DIRECT_KIND_CHECK");
    st->present = now != NULL;
    st->value[0] = 0;
    if (now) snprintf(st->value, sizeof st->value, "%s", now);
}

static int kind_env_same(const struct kind_env_state *st) {
    const char *now = getenv("S2A_DIRECT_KIND_CHECK");
    if ((now != NULL) != st->present) return 0;
    return now == NULL || !strcmp(now, st->value);
}

static uint8_t g_result[] = "{\"schema\":\"s2a-direct-program-v1\",\"transport\":{\"session\":7,\"generation\":3,\"requestId\":\"nvn-test\"},\"contentHash\":\"sha256-00\",\"content\":{\"contract\":\"s2a-direct-program-content-v1\",\"stages\":[]}}";

static int32_t echo_compile(uint64_t session, uint64_t generation, s2a_direct_bytes request,
                            s2a_direct_emit_fn emit, void *sink) {
    if (!session || !generation || !request.data || !request.bytes) return S2A_DIRECT_RES_ARGUMENT;
    s2a_direct_bytes out = { g_result, sizeof g_result - 1 };
    return emit(sink, out);
}

static int32_t bad_compile(uint64_t session, uint64_t generation, s2a_direct_bytes request,
                           s2a_direct_emit_fn emit, void *sink) {
    (void)session; (void)generation; (void)request; (void)emit; (void)sink;
    return S2A_DIRECT_RES_COMPILE;
}

int main(int argc, char **argv) {
    static uint8_t code[0x200], control[0x880];
    memset(code, 0x5a, sizeof code);
    memset(control, 0x11, sizeof control);   // +0x744 保留为 0：本测试用输入里的 tess_mode
    const char *profile = "{\"directABI\":1,\"translatorClosure\":\"synthetic-closure-v1\","
                          "\"runtimeCompilerABI\":\"s2a-direct-compile-v1\",\"profileHash\":\"sha256-aa\"}";
    struct s2a_compile_request_input in;
    memset(&in, 0, sizeof in);
    in.session = 7;
    in.generation = 3;
    in.request_id = "nvn-test";
    in.profile_json = profile;
    in.profile_bytes = strlen(profile);
    in.direct_abi = 1;
    in.translator_closure = "synthetic-closure-v1";
    in.stages[0] = (struct s2a_compile_stage_src){ "vs", code, sizeof code, control, sizeof control, NULL, 0, 0x5 };
    in.stages[1] = (struct s2a_compile_stage_src){ "fs", code, sizeof code, control, sizeof control, NULL, 0, 0 };
    in.stage_count = 2;
    in.gfx.topology = "Triangles";
    in.gfx.tess_mode = 0x100 | 1 | 0x20;    // 顺时针 / Triangles / FractionalOddSpacing
    in.gfx.has_tess_mode = 1;
    in.gfx.rt_count = 1;
    for (int i = 0; i < 32; i++) in.gfx.attribute_types[i] = i < 3 ? "Float" : "Float";
    for (int i = 0; i < 8; i++) in.gfx.fragment_output_types[i] = "Float";
    in.texture_array_length = 64;
    in.sampler_array_length = 16;
    // F4.2B：真实纹理事实（stage/handle=8+2*slot/cbufSlot=-1 与编译器查询坐标一致）。
    in.textures[0] = (struct s2a_compile_texture_src){ "fs", 8, -1, "2d", "R8G8B8A8Unorm", 0, 1, 0, 1 };
    in.textures[1] = (struct s2a_compile_texture_src){ "fs", 10, -1, "2d-array", "R16G16B16A16Float", 0, 1, 0, 4 };
    in.texture_count = 2;
    in.transform_feedback_enabled = 0;
    // 与 nvk_first_compile_request 相同的真实/模型事实（真机首绘那笔的形态）。
    in.gfx.has_alpha_to_coverage = 1;
    in.gfx.alpha_to_coverage_enable = 0;
    in.gfx.alpha_to_coverage_dither = 0;
    in.gfx.has_alpha_test = 1;
    in.gfx.alpha_test_compare = "Always";
    in.gfx.has_blend = 1;
    in.gfx.dual_source_blend = 0;
    in.gfx.has_depth_mode = 1;
    in.gfx.depth_mode = 0;
    in.gfx.has_y_negate = 1;
    in.gfx.y_negate = 1;
    in.gfx.has_origin = 1;
    in.gfx.origin_upper_left = 1;
    in.gfx.has_halve_primitive_id = 1;
    in.gfx.halve_primitive_id = 0;
    in.gfx.has_point_size = 1;
    in.gfx.program_point_size_enable = 1;
    in.gfx.point_size = 1.0f;
    in.gfx.has_alpha_test_reference = 1;
    in.gfx.alpha_test_reference = 0.0f;
    in.gfx.has_cb_draw_params = 1;
    in.gfx.cb_draw_params = 0;
    in.gfx.has_unaligned_ssbo = 1;
    in.gfx.unaligned_ssbo = 0;

    char *json = NULL;
    size_t len = 0;
    char gaps[2048] = {0};
    CHECK(s2a_compile_request_build(&in, &json, &len, gaps, sizeof gaps) == 0, "build succeeds");
    CHECK(json && len > 100, "json has bytes");
    // 「旧格式请求的缓存条目在新版本下不被命中」：抠掉 debugMode 键得到旧格式字节，
    // 同一 sha256（程序缓存身份覆盖整个请求 JSON，compile_request.c s2a_compile_request_execute）
    // 必须不同——旧 sha 槽位对新请求自然 miss。
    if (json) {
        const char *needle = strstr(json, ",\"debugMode\":");
        CHECK(needle != NULL, "new-format request carries explicit debugMode");
        if (needle) {
            char new_sha[65], old_sha[65];
            s2a_sha256 ctx;
            uint8_t digest[32];
            s2a_sha256_init(&ctx); s2a_sha256_update(&ctx, json, len); s2a_sha256_final(&ctx, digest);
            s2a_sha256_hex(digest, new_sha);
            char *oldfmt = malloc(len + 1);
            memcpy(oldfmt, json, len + 1);
            char *hit = strstr(oldfmt, ",\"debugMode\":");
            // 键值段 = ,\"debugMode\": + true/false（尾键，值后紧跟 }）：整段抠掉。
            char *colon = strchr(hit, ':') + 1;
            size_t vlen = strncmp(colon, "true", 4) == 0 ? 4 : 5;
            memmove(hit, colon + vlen, strlen(colon + vlen) + 1);
            size_t old_len = strlen(oldfmt);
            s2a_sha256_init(&ctx); s2a_sha256_update(&ctx, oldfmt, old_len); s2a_sha256_final(&ctx, digest);
            s2a_sha256_hex(digest, old_sha);
            CHECK(strcmp(new_sha, old_sha) != 0, "old-format request sha differs from new (old cache entry misses)");
            free(oldfmt);
        }
    }
    if (json) {
        char text[64];
        snprintf(text, sizeof text, "\"session\":%d", 7);
        CHECK(contains(json, text), "session carried");
        snprintf(text, sizeof text, "\"generation\":%d", 3);
        CHECK(contains(json, text), "generation carried");
        CHECK(contains(json, "\"schema\":\"s2a-direct-compile-v1\""), "request schema");
        CHECK(contains(json, "\"runtimeCompilerABI\":\"s2a-direct-compile-v1\""), "compiler abi");
        CHECK(contains(json, "\"translatorClosure\":\"synthetic-closure-v1\""), "closure");
        CHECK(contains(json, profile), "profile copied verbatim");
        CHECK(contains(json, "\"kind\":\"graphics\""), "graphics program");
        CHECK(contains(json, "\"stage\":\"vs\""), "vs stage");
        CHECK(contains(json, "\"stage\":\"fs\""), "fs stage");
        CHECK(contains(json, "\"constantBufferUse\":5"), "cb use from draw snapshot");
        CHECK(contains(json, "\"topology\":\"Triangles\""), "topology from draw primitive");
        CHECK(contains(json, "\"tessCw\":true"), "tess winding from control");
        CHECK(contains(json, "\"tessPatchType\":\"Triangles\""), "tess patch type");
        CHECK(contains(json, "\"tessSpacing\":\"FractionalOddSpacing\""), "tess spacing");
        CHECK(contains(json, "\"textureArrayLength\":64"), "texture pool capacity");
        CHECK(contains(json, "\"samplerArrayLength\":16"), "sampler pool capacity");
        CHECK(contains(json, "\"transformFeedbackEnabled\":false"), "tf state");
        CHECK(contains(json, "\"textures\":[{\"stage\":\"fs\",\"handle\":8,\"cbufSlot\":-1,"
                              "\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\","
                              "\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}"),
              "first texture fact is a real source");
        CHECK(contains(json, "\"handle\":10,\"cbufSlot\":-1,\"samplerType\":\"2d-array\","
                              "\"format\":\"R16G16B16A16Float\""), "second texture fact (array/srgb)");
        CHECK(!contains(json, "\"textures\":[]"), "textures array is not empty when facts exist");
        CHECK(!contains(gaps, "program.textures[]"), "no textures gap when facts exist");
        CHECK(contains(json, "\"earlyZForce\":false"), "earlyZForce derived from the NVN API surface");
        CHECK(contains(json, "\"viewportTransformDisable\":false"), "viewport transform always enabled in NVN");
        CHECK(contains(json, "\"attributeTypes\":["), "attributeTypes from vertex formats");
        CHECK(contains(json, "\"fragmentOutputTypes\":["), "fragment output types from attachments");
        // 没有颜色附件的图形 draw（rt_count==0）：编译器仍要求 8 项 fragmentOutputTypes，
        // 中性 Float；只写 storage 的“编码”pass 就是这种。
        in.gfx.rt_count = 0;
        {
            char *json0 = NULL;
            size_t len0 = 0;
            char gaps0[512] = "";
            CHECK(s2a_compile_request_build(&in, &json0, &len0, gaps0, sizeof gaps0) == 0 && json0,
                  "request with rt_count=0 is available");
            CHECK(json0 && contains(json0, "\"fragmentOutputTypes\":[") &&
                  contains(json0, "\"Float\",\"Float\",\"Float\",\"Float\",\"Float\",\"Float\",\"Float\",\"Float\""),
                  "rt_count=0 still emits eight neutral fragment output types");
            CHECK(!contains(gaps0, "fragmentOutputTypes"), "rt_count=0 reports no fragmentOutputTypes gap");
            free(json0);
        }
        in.gfx.rt_count = 1;
        CHECK(!contains(gaps, "graphicsState.programPointSizeEnable/pointSize"), "point size sourced");
        CHECK(!contains(gaps, "graphicsState.hasUnalignedStorageBuffer"), "unaligned ssbo sourced");
        CHECK(!contains(gaps, "graphicsState.attributeTypes[32]"), "attributeTypes no longer a gap");
        CHECK(!contains(gaps, "program.textures[]"), "no textures gap once real facts are supplied");
        CHECK(contains(gaps, "program.stages[].c1"), "gap names c1");
        if (argc > 1) {
            FILE *f = fopen(argv[1], "wb");
            if (f) { fwrite(json, 1, len, f); fclose(f); }
        }
    }

    // 主线缺省路径：调试门关（正式构建 / 非调试）时请求里不带 kindCheck，校验保持开着。
    check_kind_check_contract(&in, "default request build succeeds", NULL);

    // #1398：SSBO 对齐事实有来源时值必须进请求；没有来源时整项省略并记缺口。
    {
        struct s2a_compile_request_input u = in;
        u.gfx.unaligned_ssbo = 1;
        char *uj = NULL;
        size_t ul = 0;
        char ug[2048] = {0};
        CHECK(s2a_compile_request_build(&u, &uj, &ul, ug, sizeof ug) == 0, "unaligned build succeeds");
        CHECK(uj && contains(uj, "\"hasUnalignedStorageBuffer\":true"), "unaligned fact carried");
        CHECK(!contains(ug, "graphicsState.hasUnalignedStorageBuffer"), "unaligned fact is not a gap");
        free(uj);

        u.gfx.has_unaligned_ssbo = 0;
        uj = NULL;
        ul = 0;
        memset(ug, 0, sizeof ug);
        CHECK(s2a_compile_request_build(&u, &uj, &ul, ug, sizeof ug) == 0, "unknown-unaligned build succeeds");
        CHECK(uj && !contains(uj, "\"hasUnalignedStorageBuffer\""), "unknown unaligned field is omitted");
        CHECK(contains(ug, "graphicsState.hasUnalignedStorageBuffer"), "unknown unaligned is a gap");
        free(uj);
    }

    // 池容量未知：字段必须整项省略，不能留下 ",," 之类的非法 JSON。
    {
        struct s2a_compile_request_input unknown = in;
        unknown.texture_array_length = 0;
        unknown.sampler_array_length = 0;
        char *uj = NULL;
        size_t ul = 0;
        char ug[2048] = {0};
        CHECK(s2a_compile_request_build(&unknown, &uj, &ul, ug, sizeof ug) == 0, "unknown-pool build");
        CHECK(uj && !strstr(uj, ",,"), "no doubled comma when pool sizes are unknown");
        CHECK(uj && !strstr(uj, "ArrayLength"), "unknown pool lengths are omitted");
        CHECK(contains(ug, "program.samplerArrayLength"), "unknown sampler length is a gap");
        CHECK(contains(ug, "program.textureArrayLength"), "unknown texture length is a gap");
        if (uj && argc > 2) {
            FILE *f = fopen(argv[2], "wb");
            if (f) { fwrite(uj, 1, ul, f); fclose(f); }
        }
        free(uj);
    }

    // kindCheck 矩阵（宿主模拟系统属性，见文件头的桩说明）：只有“调试门两道都开 + 属性=0”
    // 才关校验；门关着时 adb 设 debug.s2a.kind_check=0 也不许关（玩家包不能靠属性改调试行为）。
#ifdef S2A_TEST_KIND_CHECK_STUB
    {
#ifdef S2A_ANDROID_DEBUG_ENV
        g_debug_env = "0"; g_kind_check = "0";
        CHECK(s2a_direct_kind_check_enabled() != 0, "gate off: property alone cannot disable kind check");
        check_kind_check_contract(&in, "gate off + prop=0 build succeeds", NULL);

        g_debug_env = "1"; g_kind_check = "";
        CHECK(s2a_direct_kind_check_enabled() != 0, "gate open + unset property keeps kind check on");
        check_kind_check_contract(&in, "gate open + unset property build succeeds", NULL);

        g_debug_env = "1"; g_kind_check = "0";
        CHECK(s2a_direct_kind_check_enabled() == 0, "gate open + prop=0 disables kind check");
        check_kind_check_contract(&in, "gate open + prop=0 build succeeds", argc > 3 ? argv[3] : NULL);
        // 保持“本应关掉”的状态进入 execute：下面断言 execute 不写进程环境。

        g_debug_env = "0"; g_xlate_debug = "1";
        CHECK(s2a_direct_xlate_debug_enabled() == 0, "gate off: property alone cannot enable xlate debug");
        check_kind_check_contract(&in, "gate off + xlate prop=1 build succeeds", NULL);

        g_debug_env = "1"; g_xlate_debug = "";
        CHECK(s2a_direct_xlate_debug_enabled() == 0, "gate open + unset property keeps xlate debug off");

        g_debug_env = "1"; g_xlate_debug = "1";
        CHECK(s2a_direct_xlate_debug_enabled() != 0, "gate open + prop=1 enables xlate debug");
        check_kind_check_contract(&in, "gate open + xlate prop=1 build succeeds", NULL);
        g_xlate_debug = "";
#else
        // 正式构建：S2A_ANDROID_DEBUG_ENV 没定义，门①编译期就关着，adb 把两个属性都设上也打不开。
        g_debug_env = "1"; g_kind_check = "0"; g_xlate_debug = "1";
        CHECK(s2a_direct_kind_check_enabled() != 0, "release build: properties cannot open the debug gate");
        CHECK(s2a_direct_xlate_debug_enabled() == 0, "release build: properties cannot enable xlate debug");
        check_kind_check_contract(&in, "release build + props set keeps the request free of kindCheck", NULL);
#endif
    }

    // 阶段口径：含几何段的图形请求恒 debugMode:true（#1774 — 顶点/片元关、几何保留，
    // 组合翻译一条程序一个旗没法只给 gs 留）。xlate 属性不设也要 true。
    {
        struct s2a_compile_request_input gsin = in;
        gsin.stages[0] = (struct s2a_compile_stage_src){ "vs", code, sizeof code, control, sizeof control, NULL, 0, 0x5 };
        gsin.stages[1] = (struct s2a_compile_stage_src){ "fs", code, sizeof code, control, sizeof control, NULL, 0, 0 };
        gsin.stages[2] = (struct s2a_compile_stage_src){ "gs", code, sizeof code, control, sizeof control, NULL, 0, 0 };
        gsin.stage_count = 3;
        char *gjson = NULL;
        size_t glen = 0;
        char gaps[512] = "";
        CHECK(s2a_compile_request_build(&gsin, &gjson, &glen, gaps, sizeof gaps) == 0,
              "vs+fs+gs request builds");
        if (gjson) {
            CHECK(contains(gjson, "\"debugMode\":true"),
                  "gs-containing request keeps DebugMode regardless of xlate prop");
            free(gjson);
        }
    }
#endif

    // 未绑定：明确失败，不产生结果。
    struct kind_env_state kind_env;
    kind_env_capture(&kind_env);
    struct s2a_compile_outcome out;
    CHECK(s2a_compile_request_execute(7, 3, json, len, &out) != 0, "no compiler -> failure");
    CHECK(contains(out.summary, "未绑定"), "failure names the missing binding");
    CHECK(out.bytes == NULL, "no result without a compiler");
    CHECK(kind_env_same(&kind_env), "execute keeps the kind check switch out of the environment");
    s2a_compile_outcome_free(&out);

    // 已绑定：结果由 emit 复制到后端自有内存，入参缓冲随后可释放。
    CHECK(s2a_direct_bind_compiler_v1((void *)0x1234, echo_compile) == 0, "bind fake compiler");
    char *copy = malloc(len);
    memcpy(copy, json, len);
    CHECK(s2a_compile_request_execute(7, 3, copy, len, &out) == 0, "bound compiler -> result");
    CHECK(kind_env_same(&kind_env), "successful execute does not setenv S2A_DIRECT_KIND_CHECK");
    CHECK(out.bytes && out.bytes_len == sizeof g_result - 1, "result copied");
    CHECK(out.bytes && !memcmp(out.bytes, g_result, out.bytes_len), "result bytes match");
    CHECK(contains(out.summary, "s2a-direct-program-v1"), "summary has result schema");
    CHECK(contains(out.summary, "sha256-00"), "summary has content hash");
    memset(copy, 0, len);
    free(copy);
    CHECK(out.bytes && out.bytes[0] == '{', "result independent of the request buffer");
    s2a_compile_outcome_free(&out);

    // 同一 context 换指针一律拒绝（W2.6 身份稳定）；要换先解绑。
    CHECK(s2a_direct_bind_compiler_v1((void *)0x1234, bad_compile) == S2A_DIRECT_RES_IDENTITY,
          "rebinding a different compiler is rejected");
    CHECK(s2a_direct_bound_compile((void *)0x1234) == echo_compile, "rejected rebind keeps the original");
    CHECK(s2a_direct_unbind_compiler_v1((void *)0x1234) == 0, "unbind the working compiler");
    CHECK(s2a_direct_bind_compiler_v1((void *)0x5678, bad_compile) == 0, "bind failing compiler on a fresh context");
    CHECK(s2a_compile_request_execute(7, 3, json, len, &out) == S2A_DIRECT_RES_COMPILE, "failure rc surfaces");
    CHECK(out.bytes == NULL, "no result on failure");
    CHECK(contains(out.summary, "rc=-6"), "failure summary has rc");
    s2a_compile_outcome_free(&out);
    free(json);

    if (fails) { printf("direct compile request test: %d FAILED\n", fails); return 1; }
    printf("direct compile request test: ok\n");
    return 0;
}
