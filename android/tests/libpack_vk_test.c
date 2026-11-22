// #1434 宿主回归：离线 Vulkan 直连库（LibPack）POSIX 段读器 + _b.json→v1 适配（native/direct/libpack_vk.c）。
//
// 反例基线（任一成立，运行时就会把不属于本键/已被破坏的字节当命中采纳）：
//   * 把“lib.idx 在/记录在”当命中，不核 dm_ctl、stage、SPIR-V 头；
//   * _b.json 的 bindings 逐字段不核对（arg_id→binding=arg×6+阶段号、tex reg→句柄表字偏移、
//     Smplr/samp* 半边不出行、cbuf→slot+3、sbuf→nvnSlot、宿主辅助无 guest 槽）；
//   * 缺段/坏段/段越界静默当缺席——miss 与 reject 必须分清，reject 不许冒充命中；
//   * 缺 v1 必填字段（storage rw、uniform slot）的合成字节能被 first_draw 判据接受。
// 夹具在 mkdtemp 里现写 lib.idx + lib.000.pak（与 iOS 同一种容器），记录按 (key,kind) 升序。
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "direct/libpack_vk.h"
#include "nvn/nvn_vk_program_result.h"
#include "libpack_texture_spirv.h"
#include "../../graphics/nvn/gfx/nvn/include/nvn.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", (what)); fails++; } } while (0)

#define CHAIN "0be3f28548fd"   // 与 graphics/nvn/gfx/nvn/dmsh_chain.h DMSH_CHAIN 同值

#define TMPL_CAP 320   // main() 里临时目录模板 tmpl 的容量（拼出来超长时 main 直接退出）
// g_dir 依次放 tmpl、tmpl/bad、tmpl/buffer：按最长的「tmpl + "/buffer"」给够，snprintf 不会截断。
static char g_dir[TMPL_CAP + 32];   // the template plus the longest per-case suffix ("/sepsamp<int>", "/sup<int>"): gcc -Wformat-truncation checks it

static void wr(void *dst, const void *src, size_t n, size_t *at) { memcpy((uint8_t *)dst + *at, src, n); *at += n; }
static void wr32(void *dst, uint32_t v, size_t *at) { wr(dst, &v, 4, at); }
static void wr64(void *dst, uint64_t v, size_t *at) { wr(dst, &v, 8, at); }

// 一条记录 → 24 字节（u64 key; u8 kind; u8 shard; u16 0; u32 len; u64 off）
struct trec { uint64_t key; uint8_t kind, shard; uint32_t len; uint64_t off; };
static int trec_cmp(const void *a, const void *b) {
    const struct trec *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->kind < y->kind ? -1 : x->kind > y->kind;
}
// 写 <g_dir>/lib.000.pak（段字节顺序拼接）与 lib.idx；recs[i].off 按拼接回填。
static int mk_pack(struct trec *recs, uint32_t n, const uint8_t *payloads[], size_t plens[], uint32_t nshard) {
    char p[400];
    snprintf(p, sizeof p, "%s/lib.000.pak", g_dir);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    uint64_t off = 0;
    for (uint32_t i = 0; i < n; i++) { recs[i].shard = 0; recs[i].off = off; fwrite(payloads[i], 1, plens[i], f); off += plens[i]; }
    fclose(f);
    qsort(recs, n, sizeof *recs, trec_cmp);
    uint8_t idx[48 + 16 + 24 * 64];
    size_t at = 0;
    wr(idx, "S2ALIB\0\0", 8, &at);
    wr32(idx, 1, &at); wr32(idx, n, &at); wr32(idx, nshard ? nshard : 1, &at); wr32(idx, 0, &at);
    char chain[12] = {0}; memcpy(chain, CHAIN, 12);
    wr(idx, chain, 12, &at); memset(idx + at, 0, 48 - at); at = 48;
    for (uint32_t s = 0; s < (nshard ? nshard : 1); s++) { wr64(idx, s == 0 ? off : 0, &at); wr64(idx, 0, &at); }
    for (uint32_t i = 0; i < n; i++) {
        wr64(idx, recs[i].key, &at); idx[at++] = recs[i].kind; idx[at++] = recs[i].shard;
        idx[at++] = 0; idx[at++] = 0;
        wr32(idx, recs[i].len, &at); wr64(idx, recs[i].off, &at);
    }
    snprintf(p, sizeof p, "%s/lib.idx", g_dir);
    f = fopen(p, "wb");
    if (!f) return -1;
    fwrite(idx, 1, at, f);
    fclose(f);
    return 0;
}

static const char *VS_JSON =
    "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
    "{\"msl_name\":\"vp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"vp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":7,\"rw\":true,\"arg_id\":1},"
    "{\"msl_name\":\"vp_t_tcb_8\",\"kind\":\"tex\",\"reg\":5,\"arg_id\":2},"
    "{\"msl_name\":\"vp_t_tcb_8Smplr\",\"kind\":\"samp_tex\",\"reg\":5,\"arg_id\":3},"
    "{\"msl_name\":\"vp_c4\",\"kind\":\"cbuf\",\"reg\":1,\"arg_id\":4},"
    "{\"msl_name\":\"gal_ubo_len\",\"kind\":\"ubolen\",\"arg_id\":5}],"
    "\"vertex_inputs\":[{\"location\":1},{\"location\":3}]}";

static const char *PS_JSON =
    "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
    "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"fp_t_sep\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
    "{\"msl_name\":\"fp_i0\",\"kind\":\"img\",\"reg\":2,\"arg_id\":2}]}";

// 同一个片元键，分离贴图带采样器半边：samp_tex（Dsh.cs 句柄类型 0：采样器就是同一个字）给槽；
//   samp（分离采样器槽 3）v1 表达不了，不给槽（消费端如实拒，不按同字高位绑错采样器）
static const char *PS_SAMPTEX_JSON =
    "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
    "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"fp_t_sep\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
    "{\"msl_name\":\"fp_t_sepSmplr\",\"kind\":\"samp_tex\",\"reg\":128,\"arg_id\":3},"
    "{\"msl_name\":\"fp_i0\",\"kind\":\"img\",\"reg\":2,\"arg_id\":2}]}";
static const char *PS_SEPSAMP_JSON =
    "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
    "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"fp_t_sep\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
    "{\"msl_name\":\"fp_t_sepSmplr\",\"kind\":\"samp\",\"reg\":3,\"arg_id\":3},"
    "{\"msl_name\":\"fp_i0\",\"kind\":\"img\",\"reg\":2,\"arg_id\":2}]}";
// 0x1056805A: 类型 1，c2 字 90（分离贴图 #128）与 c2 字 346（分离采样器 #0）。
static const char *PS_SEPSAMP_C2_JSON =
    "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
    "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"fp_t_cb196610_1056805A\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
    "{\"msl_name\":\"fp_t_cb196610_1056805ASmplr\",\"kind\":\"samp\",\"reg\":0,\"arg_id\":3},"
    "{\"msl_name\":\"fp_i0\",\"kind\":\"img\",\"reg\":2,\"arg_id\":2}]}";

static const char *VS_BUFFER_JSON =
    "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
    "{\"msl_name\":\"vp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"vp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":7,\"rw\":true,\"arg_id\":1},"
    "{\"msl_name\":\"vp_t_tcb_8\",\"kind\":\"tbuf\",\"reg\":5,\"arg_id\":2},"
    "{\"msl_name\":\"vp_t_tcb_8Smplr\",\"kind\":\"samp_tex\",\"reg\":5,\"arg_id\":3},"
    "{\"msl_name\":\"vp_c4\",\"kind\":\"cbuf\",\"reg\":1,\"arg_id\":4},"
    "{\"msl_name\":\"gal_ubo_len\",\"kind\":\"ubolen\",\"arg_id\":5}],"
    "\"vertex_inputs\":[{\"location\":1},{\"location\":3}]}";

// 计算键：c0 驱动常量段 / 用户 cbuf / 三种存储来源 / 组合纹理 + 采样器半边 / support / Metal 专用 ubolen，
//   外加一条拿不到 guest 映射的 gmem_map（另一键，用来核「如实拒」）。arg_id 0..7 → binding = arg×6+5。
static const char *CS_JSON =
    "{\"key\":\"c0c1c2c3c4c5c6c7_b\",\"stage\":\"cs\",\"dm_ctl\":\"c0c1c2c3c4c5c6c7\",\"bindings\":["
    "{\"msl_name\":\"cp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":0},"
    "{\"msl_name\":\"cp_c4\",\"kind\":\"cbuf\",\"reg\":1,\"arg_id\":1},"
    "{\"msl_name\":\"cp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":2,\"rw\":true,\"arg_id\":2},"
    "{\"msl_name\":\"cp_s1\",\"kind\":\"sbuf_ubo\",\"index\":1,\"reg\":1,\"off\":32,\"rw\":false,\"arg_id\":3},"
    "{\"msl_name\":\"cp_s2\",\"kind\":\"ubo_ptr\",\"index\":2,\"reg\":3,\"rw\":true,\"arg_id\":4},"
    "{\"msl_name\":\"cp_t_tcb_8\",\"kind\":\"tex\",\"reg\":4,\"arg_id\":5},"
    "{\"msl_name\":\"cp_t_tcb_8Smplr\",\"kind\":\"samp_tex\",\"reg\":4,\"arg_id\":6},"
    "{\"msl_name\":\"support_buffer\",\"kind\":\"support\",\"arg_id\":7},"
    "{\"msl_name\":\"gal_ubo_len\",\"kind\":\"ubolen\",\"arg_id\":8}]}";
static const char *CS_GMEM_JSON =
    "{\"key\":\"d0d1d2d3d4d5d6d7_b\",\"stage\":\"cs\",\"dm_ctl\":\"d0d1d2d3d4d5d6d7\",\"bindings\":["
    "{\"msl_name\":\"cp_s0\",\"kind\":\"gmem_map\",\"index\":0,\"rw\":true,\"arg_id\":0}]}";
// 最小合法 SPIR-V 头（magic / 1.0 / generator 0 / bound 1 / schema 0）：计算解析只核头；纹理分类证不出时按 source 缺省当普通纹理
static const uint32_t CS_SPV[5] = { 0x07230203u, 0x00010000u, 0u, 1u, 0u };

// 计算键 .spv 声明 support_buffer（绑定 47、组 0）而 _b.json 不列 support（11-18 扫 13 份按款预编库：一款开放世界游戏 72 个计算程序里
//   52 个 .spv 用到 support_buffer、_b.json 不列）：只含 OpName + 两条 OpDecorate，lp_spv_bindings 只读这三样
static const char *CS_NOSUP_JSON =
    "{\"key\":\"e0e1e2e3e4e5e6e7_b\",\"stage\":\"cs\",\"dm_ctl\":\"e0e1e2e3e4e5e6e7\",\"bindings\":["
    "{\"msl_name\":\"cp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":0}]}";
static const uint32_t CS_SUP_SPV[] = {
    0x07230203u, 0x00010000u, 0u, 3u, 0u,
    0x00060005u, 2u, 0x70707573u, 0x5f74726fu, 0x66667562u, 0x00007265u,   // OpName %2 "support_buffer"
    0x00040047u, 2u, 33u, 47u,                                             // OpDecorate %2 Binding 47
    0x00040047u, 2u, 34u, 0u,                                              // OpDecorate %2 DescriptorSet 0
};

// 能力核用例：批翻链按「宿主有 FP64」翻出的模块带 OpCapability Float64（Maleoon 920 转换器拒、计算管线建不出）。
//   CAP_UNUSED：Shader + Float64 + Int64，模块里没有 64 位类型 → 两条都删、留 Shader；CAP_F64：真有 OpTypeFloat 64 → 拒；
//   CAP_I64A：Int64Atomics → 拒
static const uint32_t CAP_UNUSED_SPV[] = { 0x07230203u, 0x00010000u, 0u, 3u, 0u,
    0x00020011u, 1u, 0x00020011u, 10u, 0x00040015u, 1u, 32u, 0u, 0x00020011u, 11u, 0x00030016u, 2u, 32u };
static const uint32_t CAP_F64_SPV[] = { 0x07230203u, 0x00010000u, 0u, 3u, 0u,
    0x00020011u, 1u, 0x00020011u, 10u, 0x00030016u, 2u, 64u };
// 只有 64 位常量类型、没有运算：OpTypeFloat %2 64 只被 OpConstant %3 = 1.0 引用（双字字面量 0x3ff00000_00000000）。
//   判据以类型声明为准：照样算用到 Float64 → 拒收，不删能力声明（删了模块就不合法）
static const uint32_t CAP_F64_CONST_SPV[] = { 0x07230203u, 0x00010000u, 0u, 4u, 0u,
    0x00020011u, 1u, 0x00020011u, 10u, 0x00030016u, 2u, 64u, 0x0005002bu, 2u, 3u, 0u, 0x3ff00000u };
static const uint32_t CAP_I64A_SPV[] = { 0x07230203u, 0x00010000u, 0u, 3u, 0u, 0x00020011u, 1u, 0x00020011u, 12u };
static const char *CS_F64_JSON =
    "{\"key\":\"f0f1f2f3f4f5f6f7_b\",\"stage\":\"cs\",\"dm_ctl\":\"f0f1f2f3f4f5f6f7\",\"bindings\":[]}";

// 图形 c0（驱动常量段）夹具：glslang 编出的真 SPIR-V（-V --target-env vulkan1.1），变量 c0 = set 0 的 std140 Uniform Block { uvec4 data[160] }（0xA00 字节，
//   同回放给的驱动段大小）。片元读 data[96].x（0x600）、binding 22 = arg_id 3 × 6 + 片元阶段号 4；顶点读 data[96].x、binding 6 = arg_id 1 × 6 + 阶段号 0。
static const uint32_t libpack_c0_frag_spv[] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000019, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x0000006f, 0x00030005,
    0x0000000e, 0x00003043, 0x00050006, 0x0000000e, 0x00000000, 0x61746164,
    0x00000000, 0x00030005, 0x00000010, 0x00003063, 0x00040047, 0x00000009,
    0x0000001e, 0x00000000, 0x00040047, 0x0000000d, 0x00000006, 0x00000010,
    0x00030047, 0x0000000e, 0x00000002, 0x00050048, 0x0000000e, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000010, 0x00000021, 0x00000016,
    0x00040047, 0x00000010, 0x00000022, 0x00000000, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008,
    0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    0x00040015, 0x0000000a, 0x00000020, 0x00000000, 0x00040017, 0x0000000b,
    0x0000000a, 0x00000004, 0x0004002b, 0x0000000a, 0x0000000c, 0x000000a0,
    0x0004001c, 0x0000000d, 0x0000000b, 0x0000000c, 0x0003001e, 0x0000000e,
    0x0000000d, 0x00040020, 0x0000000f, 0x00000002, 0x0000000e, 0x0004003b,
    0x0000000f, 0x00000010, 0x00000002, 0x00040015, 0x00000011, 0x00000020,
    0x00000001, 0x0004002b, 0x00000011, 0x00000012, 0x00000000, 0x0004002b,
    0x00000011, 0x00000013, 0x00000060, 0x0004002b, 0x0000000a, 0x00000014,
    0x00000000, 0x00040020, 0x00000015, 0x00000002, 0x0000000a, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x00070041, 0x00000015, 0x00000016, 0x00000010, 0x00000012, 0x00000013,
    0x00000014, 0x0004003d, 0x0000000a, 0x00000017, 0x00000016, 0x0006000c,
    0x00000007, 0x00000018, 0x00000001, 0x00000040, 0x00000017, 0x0003003e,
    0x00000009, 0x00000018, 0x000100fd, 0x00010038,
};
static const uint32_t libpack_c0_vert_spv[] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000022, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005,
    0x0000000b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006,
    0x0000000b, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006,
    0x0000000b, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000,
    0x00070006, 0x0000000b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x0000000b, 0x00000003, 0x435f6c67, 0x446c6c75,
    0x61747369, 0x0065636e, 0x00030005, 0x0000000d, 0x00000000, 0x00030005,
    0x00000013, 0x00003043, 0x00050006, 0x00000013, 0x00000000, 0x61746164,
    0x00000000, 0x00030005, 0x00000015, 0x00003063, 0x00030047, 0x0000000b,
    0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b, 0x00000000,
    0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048,
    0x0000000b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000000b,
    0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x00000012, 0x00000006,
    0x00000010, 0x00030047, 0x00000013, 0x00000002, 0x00050048, 0x00000013,
    0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000015, 0x00000021,
    0x00000006, 0x00040047, 0x00000015, 0x00000022, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015,
    0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009,
    0x00000001, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009, 0x0006001e,
    0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a, 0x00040020,
    0x0000000c, 0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d,
    0x00000003, 0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b,
    0x0000000e, 0x0000000f, 0x00000000, 0x00040017, 0x00000010, 0x00000008,
    0x00000004, 0x0004002b, 0x00000008, 0x00000011, 0x000000a0, 0x0004001c,
    0x00000012, 0x00000010, 0x00000011, 0x0003001e, 0x00000013, 0x00000012,
    0x00040020, 0x00000014, 0x00000002, 0x00000013, 0x0004003b, 0x00000014,
    0x00000015, 0x00000002, 0x0004002b, 0x0000000e, 0x00000016, 0x00000060,
    0x0004002b, 0x00000008, 0x00000017, 0x00000000, 0x00040020, 0x00000018,
    0x00000002, 0x00000008, 0x0004002b, 0x00000006, 0x0000001d, 0x00000000,
    0x0004002b, 0x00000006, 0x0000001e, 0x3f800000, 0x00040020, 0x00000020,
    0x00000003, 0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x00070041, 0x00000018, 0x00000019,
    0x00000015, 0x0000000f, 0x00000016, 0x00000017, 0x0004003d, 0x00000008,
    0x0000001a, 0x00000019, 0x000500c7, 0x00000008, 0x0000001b, 0x0000001a,
    0x00000009, 0x00040070, 0x00000006, 0x0000001c, 0x0000001b, 0x00070050,
    0x00000007, 0x0000001f, 0x0000001c, 0x0000001d, 0x0000001d, 0x0000001e,
    0x00050041, 0x00000020, 0x00000021, 0x0000000d, 0x0000000f, 0x0003003e,
    0x00000021, 0x0000001f, 0x000100fd, 0x00010038,
};
static int has(const uint8_t *j, const char *sub) { return j && strstr((const char *)j, sub) != NULL; }

// 原生几何阶段（DshVk.CompileGeometry 的 _vg<t>.json / _vg<t>.spv，种类 112+t / 120+t）：三角输入（t = 3）。
//   json：gp_c3 → cbuf reg0 arg0、gp_t_tcb_8 → tex reg0 arg1（采样器半边 arg2 不出行）→ 绑定号 = arg×6+3 = 3 / 9。
//   .spv（spirv-as 汇编、spirv-val --target-env vulkan1.1 过）：Geometry 入口、Triangles / OutputTriangleStrip，
//   gp_c3 绑定 3、gp_t_tcb_8 绑定 9（与 json 手算一致）；_viewport 那份多写 BuiltIn ViewportIndex（现编对带几何的程序整支拒）
static const char *GS_JSON =
    "{\"key\":\"6060606060606060_vg3\",\"stage\":\"gs\",\"dm_ctl\":\"6060606060606060\",\"gs_input_topology\":3,\"bindings\":["
    "{\"msl_name\":\"gp_c3\",\"msl_type\":\"constant S2AGsBlock_gp_c3&\",\"arg_id\":0,\"kind\":\"cbuf\",\"reg\":0},"
    "{\"msl_name\":\"gp_t_tcb_8\",\"msl_type\":\"texture2d<float>\",\"arg_id\":1,\"kind\":\"tex\",\"reg\":0},"
    "{\"msl_name\":\"gp_t_tcb_8Smplr\",\"msl_type\":\"sampler\",\"arg_id\":2,\"kind\":\"samp_tex\",\"reg\":0}]}";
// 同一支几何着色器、json 自称点输入（t = 0）却放在三角那一格：记录与自己的图元对不上 → reject
static const char *GS_WRONGTOPO_JSON =
    "{\"key\":\"6060606060606060_vg3\",\"stage\":\"gs\",\"dm_ctl\":\"6060606060606060\",\"gs_input_topology\":0,\"bindings\":["
    "{\"msl_name\":\"gp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"gp_t_tcb_8\",\"kind\":\"tex\",\"reg\":0,\"arg_id\":1}]}";
static const uint32_t libpack_geom_spv[] = {
    0x07230203u, 0x00010300u, 0x00070000u, 0x0000001eu, 0x00000000u, 0x00020011u, 0x00000002u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0006000fu, 0x00000003u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00000002u,
    0x00030010u, 0x00000001u, 0x00000016u, 0x00040010u, 0x00000001u, 0x00000000u, 0x00000001u, 0x00030010u,
    0x00000001u, 0x0000001du, 0x00040010u, 0x00000001u, 0x0000001au, 0x00000003u, 0x00040005u, 0x00000003u,
    0x635f7067u, 0x00000033u, 0x00050005u, 0x00000004u, 0x745f7067u, 0x6263745fu, 0x0000385fu, 0x00030047u,
    0x00000005u, 0x00000002u, 0x00040047u, 0x00000006u, 0x00000006u, 0x00000010u, 0x00050048u, 0x00000005u,
    0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000003u, 0x00000022u, 0x00000000u, 0x00040047u,
    0x00000003u, 0x00000021u, 0x00000003u, 0x00040047u, 0x00000004u, 0x00000022u, 0x00000000u, 0x00040047u,
    0x00000004u, 0x00000021u, 0x00000009u, 0x00040047u, 0x00000002u, 0x0000000bu, 0x00000000u, 0x00020013u,
    0x00000007u, 0x00030021u, 0x00000008u, 0x00000007u, 0x00030016u, 0x00000009u, 0x00000020u, 0x00040017u,
    0x0000000au, 0x00000009u, 0x00000004u, 0x00040017u, 0x0000000bu, 0x00000009u, 0x00000002u, 0x00040015u,
    0x0000000cu, 0x00000020u, 0x00000000u, 0x0004002bu, 0x0000000cu, 0x0000000du, 0x00000004u, 0x0004001cu,
    0x00000006u, 0x0000000au, 0x0000000du, 0x0003001eu, 0x00000005u, 0x00000006u, 0x00040020u, 0x0000000eu,
    0x00000002u, 0x00000005u, 0x0004003bu, 0x0000000eu, 0x00000003u, 0x00000002u, 0x00090019u, 0x0000000fu,
    0x00000009u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu,
    0x00000010u, 0x0000000fu, 0x00040020u, 0x00000011u, 0x00000000u, 0x00000010u, 0x0004003bu, 0x00000011u,
    0x00000004u, 0x00000000u, 0x00040020u, 0x00000012u, 0x00000003u, 0x0000000au, 0x0004003bu, 0x00000012u,
    0x00000002u, 0x00000003u, 0x00040015u, 0x00000013u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000013u,
    0x00000014u, 0x00000000u, 0x00040020u, 0x00000015u, 0x00000002u, 0x0000000au, 0x0004002bu, 0x00000009u,
    0x00000016u, 0x00000000u, 0x0005002cu, 0x0000000bu, 0x00000017u, 0x00000016u, 0x00000016u, 0x00050036u,
    0x00000007u, 0x00000001u, 0x00000000u, 0x00000008u, 0x000200f8u, 0x00000018u, 0x00060041u, 0x00000015u,
    0x00000019u, 0x00000003u, 0x00000014u, 0x00000014u, 0x0004003du, 0x0000000au, 0x0000001au, 0x00000019u,
    0x0004003du, 0x00000010u, 0x0000001bu, 0x00000004u, 0x00070058u, 0x0000000au, 0x0000001cu, 0x0000001bu,
    0x00000017u, 0x00000002u, 0x00000016u, 0x00050081u, 0x0000000au, 0x0000001du, 0x0000001au, 0x0000001cu,
    0x0003003eu, 0x00000002u, 0x0000001du, 0x000100dau, 0x000100dbu, 0x000100fdu, 0x00010038u,
};
static const uint32_t libpack_geom_viewport_spv[] = {
    0x07230203u, 0x00010300u, 0x00070000u, 0x00000020u, 0x00000000u, 0x00020011u, 0x00000002u, 0x00020011u,
    0x00000039u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000003u, 0x00000001u, 0x6e69616du,
    0x00000000u, 0x00000002u, 0x00000003u, 0x00030010u, 0x00000001u, 0x00000016u, 0x00040010u, 0x00000001u,
    0x00000000u, 0x00000001u, 0x00030010u, 0x00000001u, 0x0000001du, 0x00040010u, 0x00000001u, 0x0000001au,
    0x00000003u, 0x00040005u, 0x00000004u, 0x635f7067u, 0x00000033u, 0x00050005u, 0x00000005u, 0x745f7067u,
    0x6263745fu, 0x0000385fu, 0x00030047u, 0x00000006u, 0x00000002u, 0x00040047u, 0x00000007u, 0x00000006u,
    0x00000010u, 0x00050048u, 0x00000006u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000004u,
    0x00000022u, 0x00000000u, 0x00040047u, 0x00000004u, 0x00000021u, 0x00000003u, 0x00040047u, 0x00000005u,
    0x00000022u, 0x00000000u, 0x00040047u, 0x00000005u, 0x00000021u, 0x00000009u, 0x00040047u, 0x00000002u,
    0x0000000bu, 0x00000000u, 0x00040047u, 0x00000003u, 0x0000000bu, 0x0000000au, 0x00020013u, 0x00000008u,
    0x00030021u, 0x00000009u, 0x00000008u, 0x00030016u, 0x0000000au, 0x00000020u, 0x00040017u, 0x0000000bu,
    0x0000000au, 0x00000004u, 0x00040017u, 0x0000000cu, 0x0000000au, 0x00000002u, 0x00040015u, 0x0000000du,
    0x00000020u, 0x00000000u, 0x0004002bu, 0x0000000du, 0x0000000eu, 0x00000004u, 0x0004001cu, 0x00000007u,
    0x0000000bu, 0x0000000eu, 0x0003001eu, 0x00000006u, 0x00000007u, 0x00040020u, 0x0000000fu, 0x00000002u,
    0x00000006u, 0x0004003bu, 0x0000000fu, 0x00000004u, 0x00000002u, 0x00090019u, 0x00000010u, 0x0000000au,
    0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu, 0x00000011u,
    0x00000010u, 0x00040020u, 0x00000012u, 0x00000000u, 0x00000011u, 0x0004003bu, 0x00000012u, 0x00000005u,
    0x00000000u, 0x00040020u, 0x00000013u, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x00000013u, 0x00000002u,
    0x00000003u, 0x00040015u, 0x00000014u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000014u, 0x00000015u,
    0x00000000u, 0x00040020u, 0x00000016u, 0x00000003u, 0x00000014u, 0x0004003bu, 0x00000016u, 0x00000003u,
    0x00000003u, 0x00040020u, 0x00000017u, 0x00000002u, 0x0000000bu, 0x0004002bu, 0x0000000au, 0x00000018u,
    0x00000000u, 0x0005002cu, 0x0000000cu, 0x00000019u, 0x00000018u, 0x00000018u, 0x00050036u, 0x00000008u,
    0x00000001u, 0x00000000u, 0x00000009u, 0x000200f8u, 0x0000001au, 0x00060041u, 0x00000017u, 0x0000001bu,
    0x00000004u, 0x00000015u, 0x00000015u, 0x0004003du, 0x0000000bu, 0x0000001cu, 0x0000001bu, 0x0004003du,
    0x00000011u, 0x0000001du, 0x00000005u, 0x00070058u, 0x0000000bu, 0x0000001eu, 0x0000001du, 0x00000019u,
    0x00000002u, 0x00000018u, 0x00050081u, 0x0000000bu, 0x0000001fu, 0x0000001cu, 0x0000001eu, 0x0003003eu,
    0x00000002u, 0x0000001fu, 0x0003003eu, 0x00000003u, 0x00000015u, 0x000100dau, 0x000100dbu, 0x000100fdu,
    0x00010038u,
};

// 本局 effective profile 的 capabilities 片段（现编 DirectProfile 同字段名）：几何开 / 没开
static const char *PROFILE_GS_ON = "{\"schema\":\"x\",\"capabilities\":{\"supportsGeometryShader\":true,\"supportsGeometryShaderPassthrough\":false}}";
static const char *PROFILE_GS_OFF = "{\"schema\":\"x\",\"capabilities\":{\"supportsGeometryShader\":false,\"supportsGeometryShaderPassthrough\":false}}";

// 片元输出映射用例（spirv-as 汇编、spirv-val 过）：FO1 = location 1 vec4 + location 3 vec2 带 Component 1 + FragDepth 内建；
//   FO2 = location 0 的 vec4[2] 数组输出（读不懂）
static const uint32_t FO1_SPV[] = {
    0x07230203u, 0x00010300u, 0x00070000u, 0x0000000eu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0008000fu, 0x00000004u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00000002u,
    0x00000003u, 0x00000004u, 0x00030010u, 0x00000001u, 0x00000007u, 0x00040047u, 0x00000002u, 0x0000001eu,
    0x00000001u, 0x00040047u, 0x00000003u, 0x0000001eu, 0x00000003u, 0x00040047u, 0x00000003u, 0x0000001fu,
    0x00000001u, 0x00040047u, 0x00000004u, 0x0000000bu, 0x00000016u, 0x00020013u, 0x00000005u, 0x00030021u,
    0x00000006u, 0x00000005u, 0x00030016u, 0x00000007u, 0x00000020u, 0x00040017u, 0x00000008u, 0x00000007u,
    0x00000004u, 0x00040017u, 0x00000009u, 0x00000007u, 0x00000002u, 0x00040020u, 0x0000000au, 0x00000003u,
    0x00000008u, 0x00040020u, 0x0000000bu, 0x00000003u, 0x00000009u, 0x00040020u, 0x0000000cu, 0x00000003u,
    0x00000007u, 0x0004003bu, 0x0000000au, 0x00000002u, 0x00000003u, 0x0004003bu, 0x0000000bu, 0x00000003u,
    0x00000003u, 0x0004003bu, 0x0000000cu, 0x00000004u, 0x00000003u, 0x00050036u, 0x00000005u, 0x00000001u,
    0x00000000u, 0x00000006u, 0x000200f8u, 0x0000000du, 0x000100fdu, 0x00010038u,
};
static const uint32_t FO2_SPV[] = {
    0x07230203u, 0x00010300u, 0x00070000u, 0x00000013u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00000002u,
    0x00030010u, 0x00000001u, 0x00000007u, 0x00040047u, 0x00000002u, 0x0000001eu, 0x00000000u, 0x00040047u,
    0x00000003u, 0x0000001eu, 0x00000001u, 0x00040047u, 0x00000004u, 0x0000001eu, 0x00000003u, 0x00040047u,
    0x00000004u, 0x0000001fu, 0x00000001u, 0x00040047u, 0x00000005u, 0x0000000bu, 0x00000016u, 0x00020013u,
    0x00000006u, 0x00030021u, 0x00000007u, 0x00000006u, 0x00030016u, 0x00000008u, 0x00000020u, 0x00040017u,
    0x00000009u, 0x00000008u, 0x00000004u, 0x00040017u, 0x0000000au, 0x00000008u, 0x00000002u, 0x00040020u,
    0x0000000bu, 0x00000003u, 0x00000009u, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000au, 0x00040020u,
    0x0000000du, 0x00000003u, 0x00000008u, 0x0004003bu, 0x0000000bu, 0x00000003u, 0x00000003u, 0x0004003bu,
    0x0000000cu, 0x00000004u, 0x00000003u, 0x0004003bu, 0x0000000du, 0x00000005u, 0x00000003u, 0x00040015u,
    0x0000000eu, 0x00000020u, 0x00000000u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000002u, 0x0004001cu,
    0x00000010u, 0x00000009u, 0x0000000fu, 0x00040020u, 0x00000011u, 0x00000003u, 0x00000010u, 0x0004003bu,
    0x00000011u, 0x00000002u, 0x00000003u, 0x00050036u, 0x00000006u, 0x00000001u, 0x00000000u, 0x00000007u,
    0x000200f8u, 0x00000012u, 0x000100fdu, 0x00010038u,
};
// 图形 c0 用例：写一份 vs+fs 容器（g_dir）→ 打开 → 合成 vs+fs 程序 → 关。返回 s2a_libpack_program_v1 的 rc。
static int c0_program(const char *vs_json, const void *vs_spv, size_t vs_n, const char *ps_json, const void *ps_spv, size_t ps_n,
                      uint8_t **j, size_t *jl, char *why, size_t why_size) {
    struct trec r[4] = {
        { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(vs_json) },
        { .key = 0x0123456789abcdef, .kind = 52, .len = (uint32_t)vs_n },
        { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(ps_json) },
        { .key = 0x1032547698badcfe, .kind = 52, .len = (uint32_t)ps_n },
    };
    const uint8_t *p[4] = { (const uint8_t *)vs_json, vs_spv, (const uint8_t *)ps_json, ps_spv };
    size_t n[4] = { strlen(vs_json), vs_n, strlen(ps_json), ps_n };
    uint8_t keys[6][8] = {{0}};
    const uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef }, kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
    memcpy(keys[0], kv, 8); memcpy(keys[1], kp, 8);
    if (mk_pack(r, 4, p, n, 1)) return -9;
    struct s2a_libpack *lp = NULL;
    if (s2a_libpack_open(&lp, g_dir) != 1 || !lp) return -9;
    *j = NULL; *jl = 0;
    int rc = s2a_libpack_program_v1(lp, keys, 0x3, NULL, 1, "h", "c", j, jl, why, why_size);
    s2a_libpack_close(lp);
    return rc;
}
// 把 c0 片元 SPIR-V 改成「不能证实」的几种：0 = Block 改 BufferBlock（旧式存储缓冲）、1 = Uniform 存储类改 StorageBuffer、2 = 描述符集改 1、
//   3 = 输出变量 o 也带 Binding 22（同一绑定号两个变量，指不出是哪一个）
static void c0_patch_spv(uint32_t *w, size_t nw, int what) {
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (what == 0 && op == 71 && wc == 3 && w[i + 2] == 2) w[i + 2] = 3;
        if (what == 1 && op == 32 && w[i + 2] == 2) w[i + 2] = 12;
        if (what == 1 && op == 59 && w[i + 3] == 2) w[i + 3] = 12;
        if (what == 2 && op == 71 && wc == 4 && w[i + 2] == 34) w[i + 3] = 1;
        if (what == 3 && op == 71 && wc == 4 && w[i + 2] == 30) { w[i + 2] = 33; w[i + 3] = 22; }   // OpDecorate %o Location 0 → Binding 22
        i += wc;
    }
}

int main(void) {
    const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    char tmpl[TMPL_CAP];
    if (snprintf(tmpl, sizeof tmpl, "%s/libpack_vk_test.XXXXXX", tmp) >= (int)sizeof tmpl || !mkdtemp(tmpl)) return 1;
    snprintf(g_dir, sizeof g_dir, "%s", tmpl);

    // ---- 缺席目录 / 坏 lib.idx ----
    {
        struct s2a_libpack *lp = (void *)1;
        char absent[400]; snprintf(absent, sizeof absent, "%s/absent", tmpl);
        CHECK(s2a_libpack_open(&lp, absent) == 0, "absent dir → open 0 (none)");
        snprintf(g_dir, sizeof g_dir, "%s/bad", tmpl);
        mkdir(g_dir, 0700);
        char p[400]; snprintf(p, sizeof p, "%s/lib.idx", g_dir);
        FILE *f = fopen(p, "wb"); fwrite("NOTALIB!", 1, 8, f); fclose(f);
        CHECK(s2a_libpack_open(&lp, g_dir) == -1, "8-byte bad lib.idx → open -1 (broken)");
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- 正常容器：两键 × (json+spv)，外加「只有 json 无 spv」的第三键 ----
    const char *ORPHAN_JSON =
        "{\"key\":\"aabbccddeeff0011_b\",\"stage\":\"vs\",\"dm_ctl\":\"aabbccddeeff0011\",\"bindings\":[]}";
    struct trec recs[5] = {
        { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(VS_JSON) },
        { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
        { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(PS_JSON) },
        { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof libpack_frag_spv },
        { .key = 0xaabbccddeeff0011, .kind = 1,  .len = (uint32_t)strlen(ORPHAN_JSON) },
    };
    const uint8_t *pl[5] = { (const uint8_t *)VS_JSON, libpack_vert_spv, (const uint8_t *)PS_JSON, libpack_frag_spv, (const uint8_t *)ORPHAN_JSON };
    size_t pln[5] = { strlen(VS_JSON), sizeof libpack_vert_spv, strlen(PS_JSON), sizeof libpack_frag_spv, strlen(ORPHAN_JSON) };
    CHECK(mk_pack(recs, 5, pl, pln, 1) == 0, "fixture pack written");

    struct s2a_libpack *lp = NULL;
    CHECK(s2a_libpack_open(&lp, g_dir) == 1 && lp, "open fixture pack → 1");

    // ---- s2a_libpack_get：命中 / 缺席 ----
    {
        uint8_t *b = NULL; size_t n = 0; char w[192] = "";
        uint8_t k[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        CHECK(s2a_libpack_get(lp, k, 1, &b, &n, w, sizeof w) == 0 && n == strlen(VS_JSON) && !memcmp(b, VS_JSON, n),
              "get kind1 → bytes identical");
        free(b); b = NULL;
        CHECK(s2a_libpack_get(lp, k, 52, &b, &n, w, sizeof w) == 0 && n == sizeof libpack_vert_spv && !memcmp(b, libpack_vert_spv, n),
              "get kind52 → spv bytes identical");
        free(b); b = NULL;
        CHECK(s2a_libpack_get(lp, k, 0, &b, &n, w, sizeof w) == 1 && !b, "absent kind → MISS");
        uint8_t ka[8] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33 };
        CHECK(s2a_libpack_get(lp, ka, 1, &b, &n, w, sizeof w) == 1, "absent key → MISS");
    }

    // ---- program_v1：vs+fs 命中（v1 接受口径只有这一形），逐字段核对合成字节 ----
    {
        uint8_t keys[6][8] = {{0}};
        uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        uint8_t kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        memcpy(keys[0], kv, 8);
        memcpy(keys[1], kp, 8);
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        uint64_t h0 = s2a_libpack_hits();
        int rc = s2a_libpack_program_v1(lp, keys, 0x3 /* vs|fs */, NULL, 1, "sha256-aaa", "closure-x", &j, &jl, w, sizeof w);
        CHECK(rc == 0 && j && jl == strlen((const char *)j), "vs+fs program → HIT, NUL-terminated v1 bytes");
        CHECK(s2a_libpack_hits() == h0 + 1, "hit counter +1");
        CHECK(has(j, "\"schema\":\"s2a-direct-program-v1\""), "v1 schema tag");
        CHECK(has(j, "\"contract\":\"s2a-direct-program-content-v1\""), "content contract tag");
        CHECK(has(j, "\"kind\":\"graphics\""), "graphics kind");
        CHECK(has(j, "\"stage\":\"vs\""), "vs stage entry");
        CHECK(has(j, "\"attributeReadMask\":10"), "attributeReadMask = bit1|bit3 = 10");
        CHECK(has(j, "\"spirv\":\""), "base64 spirv present");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"vs\",\"set\":0,\"binding\":0,\"slot\":3"),
              "cbuf reg0 → uniform binding=0 slot=3");
        CHECK(has(j, "\"kind\":\"storage\",\"stage\":\"vs\",\"set\":0,\"binding\":6,\"nvnSlot\":7,\"rw\":true"),
              "sbuf reg7 → storage binding=6 nvnSlot=7 rw=true");
        CHECK(has(j, "\"kind\":\"texture\",\"stage\":\"vs\",\"set\":0,\"binding\":12,\"slot\":18"),
              "tex reg5 → texture binding=12 slot=8+5*2=18");
        CHECK(!has(j, "\"slot\":10") && !has(j, "\"slot\":5,") && !has(j, "\"slot\":5}"),
              "负控：不是 reg*2(=10) 也不是 reg 原值(=5)");
        CHECK(!has(j, "\"binding\":30"),
              "ubolen（Metal 专用越界长度表，Vulkan 版 .spv 不声明）不出行");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"vs\",\"set\":0,\"binding\":24,\"slot\":4"),
              ".spv 声明的绑定 24 → _b.json arg 4 的 cbuf reg1 → uniform slot 4");
        CHECK(!has(j, "Smplr") && !has(j, "samp_tex"), "Smplr 采样器半边不出行");
        // ps→fs 改名、阶段号 4、sep-tex/img 槽（同一程序里的 fs 键）
        CHECK(has(j, "\"stage\":\"fs\""), "ps → v1 fs 改名");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"fs\",\"set\":0,\"binding\":4,\"slot\":3"),
              "fs cbuf → binding=0*6+4=4");
        CHECK(has(j, "\"kind\":\"texture\",\"stage\":\"fs\",\"set\":0,\"binding\":10,\"slot\":90"),
              "sep-tex reg128 → binding=6+4=10 slot=0x5A=90");
        CHECK(has(j, "\"kind\":\"image\",\"stage\":\"fs\",\"set\":0,\"binding\":16,\"slot\":76"),
              "img reg2 → binding=12+4=16 slot=0x48+2*2=76");
        // 采纳门：合成字节必须过与现编结果同一套 s2a-direct-program-v1 解析
        {
            struct fd_program p; char pw[192] = "";
            memset(&p, 0, sizeof p);
            CHECK(nvn_vk_program_parse_graphics(j, jl, &p, pw, sizeof pw) == 0,
                  "合成 v1 过 nvn_vk_program_parse_graphics");
            CHECK(p.stage[0].present && p.stage[1].present &&
                  p.stage[0].spirv_bytes == sizeof libpack_vert_spv && p.stage[0].attr_mask == 10,
                  "fd vs+fs present + spirv + attr_mask=10");
            CHECK(p.stage[1].fragment_output_map == 0xFu, "合成 v1 的片元阶段带 info.fragmentOutputMap（location 0 vec4 → 0xF，不是缺省全 1）");
            CHECK(p.binding_count == 7, "6+3 条反射 -1 采样器半边 -1 ubolen = 7 条 v1 绑定");
            struct fd_binding *b0 = NULL, *b6 = NULL, *b12 = NULL, *b24 = NULL, *b10 = NULL, *b16 = NULL;
            for (int i = 0; i < p.binding_count; i++) {
                if (p.bindings[i].binding == 0) b0 = &p.bindings[i];
                if (p.bindings[i].binding == 6) b6 = &p.bindings[i];
                if (p.bindings[i].binding == 12) b12 = &p.bindings[i];
                if (p.bindings[i].binding == 24) b24 = &p.bindings[i];
                if (p.bindings[i].binding == 10) b10 = &p.bindings[i];
                if (p.bindings[i].binding == 16) b16 = &p.bindings[i];
            }
            CHECK(b0 && b0->kind == 0 && b0->slot == 3 && b0->ubo_slot == 0 && b0->stage_idx == 0,
                  "fd cbuf → uniform slot3 ubo0 vs");
            CHECK(b6 && b6->kind == 1 && b6->storage_kind == 1 && b6->storage_slot == 7 && b6->storage_writable == 1,
                  "fd sbuf → storage_kind1 slot7 writable");
            CHECK(b12 && b12->kind == 2 && b12->tex_slot == 5 && !b12->texel_buffer,
                  "fd tex → texture tex_slot=5（slot18→byte0x48→(0x48-0x20)/8 回环）");
            CHECK(b24 && b24->kind == 0 && b24->slot == 4, "fd 绑定 24 = cbuf slot 4");
            // sep-tex：slot 90 = c2 分离贴图表第 0 项 → 分离贴图槽 128（nvn_vk_program_result.c 立即句柄读分离表的那条），
            //   没有采样器半边：采样器取同字高位（samp_slot = -1，同组合槽；Dsh.cs 句柄类型 0 的语义）
            CHECK(b10 && b10->kind == 2 && b10->tex_slot == 128 && b10->samp_slot == -1 && b10->stage_idx == 1,
                  "fd sep-tex → 分离贴图槽 128、samp_slot=-1、stage_idx=fs");
            CHECK(b16 && b16->kind == 3 && b16->slot == 76 && b16->stage_idx == 1,
                  "fd img → kind3 slot76 原样、stage_idx=fs");
            nvn_vk_program_free(&p);
        }
        free(j);
    }

    // The offline adapter emits kind=texture for both tex and tbuf, with no
    // source field. A real Buffer SPIR-V descriptor must still classify as TBUF.
    {
        snprintf(g_dir, sizeof g_dir, "%s/buffer", tmpl);
        CHECK(mkdir(g_dir, 0700) == 0, "create separate buffer pack directory");
        struct trec rb[4] = {
            { .key = 0x0123456789abcdef, .kind = 1, .len = (uint32_t)strlen(VS_BUFFER_JSON) },
            { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_buffer_vert_spv },
            { .key = 0x1032547698badcfe, .kind = 1, .len = (uint32_t)strlen(PS_JSON) },
            { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof libpack_frag_spv },
        };
        const uint8_t *pb[4] = { (const uint8_t *)VS_BUFFER_JSON, libpack_buffer_vert_spv,
                                 (const uint8_t *)PS_JSON, libpack_frag_spv };
        size_t nb[4] = { strlen(VS_BUFFER_JSON), sizeof libpack_buffer_vert_spv,
                         strlen(PS_JSON), sizeof libpack_frag_spv };
        CHECK(mk_pack(rb, 4, pb, nb, 1) == 0, "buffer pack written");
        struct s2a_libpack *buflp = NULL; uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        uint8_t keys[6][8] = {{0}};
        const uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        const uint8_t kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        memcpy(keys[0], kv, 8); memcpy(keys[1], kp, 8);
        CHECK(s2a_libpack_open(&buflp, g_dir) == 1 && buflp, "open buffer pack");
        int rc = buflp ? s2a_libpack_program_v1(buflp, keys, 3, NULL, 1, "h", "c", &j, &jl, w, sizeof w) : -1;
        CHECK(rc == 0 && j && !has(j, "\"source\":"), "buffer reflection becomes source-free v1 program");
        struct fd_program parsed = {0}; char why[192] = "";
        int parsed_rc = j ? nvn_vk_program_parse_graphics(j, jl, &parsed, why, sizeof why) : -1;
        CHECK(parsed_rc == 0, "real source-free libpack buffer parses");
        const struct fd_binding *buffer = NULL;
        for (int i = 0; i < parsed.binding_count; i++)
            if (!strcmp(parsed.bindings[i].stage, "vs") && parsed.bindings[i].set == 0 && parsed.bindings[i].binding == 12)
                buffer = &parsed.bindings[i];
        CHECK(buffer && buffer->kind == 2 && buffer->texel_buffer && buffer->tex_slot == 5 && buffer->count == 1,
              "real offline TBUF preserved at VS combined texture slot 5");
        nvn_vk_program_free(&parsed); free(j); s2a_libpack_close(buflp);
        char path[400]; snprintf(path, sizeof path, "%s/lib.idx", g_dir); unlink(path);
        snprintf(path, sizeof path, "%s/lib.000.pak", g_dir); unlink(path); rmdir(g_dir);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // 同字采样器仍是 v1；有真类型/来源/声明证明的独立采样器才是 v2。
    enum { SAME_WORD, UNKNOWN_HANDLE, SEPARATE_C2, TYPE_2, MISSING_HALF, DUP_HALF, WRONG_HALF_SLOT,
           WRONG_SET, WRONG_BINDING, DESCRIPTOR_ARRAY, EXTRA_SAMPLER, SKIPPED_KIND,
           SUPPORT_FROM_SPV, SUPPORT_LISTED, SUPPORT_WRONG_OWNER, MIXED_V2, MIXED_V2_WRONG_SET, MIXED_V1, SUPPORT_ON_IMAGE, UNNAMED_SUPPORT_UBO, NAMED_SUPPORT_IMAGE, SEP_CASES };
    for (int variant = SAME_WORD; variant < SEP_CASES; variant++) {
        char reflection[1024];
        snprintf(reflection, sizeof reflection, "%s", (variant == SAME_WORD || variant == MIXED_V1) ? PS_SAMPTEX_JSON :
                 variant == UNKNOWN_HANDLE ? PS_SEPSAMP_JSON : PS_SEPSAMP_C2_JSON);
        uint32_t mutated[sizeof libpack_frag_spv / sizeof(uint32_t)];
        const uint8_t *fragment = libpack_frag_spv;
        size_t fragment_bytes = sizeof libpack_frag_spv;
        if (variant == TYPE_2) {
            char *handle = strstr(reflection, "1056805A");
            *handle = '2'; *strstr(handle + 1, "1056805A") = '2';
        } else if (variant == MISSING_HALF || variant == DUP_HALF || variant == WRONG_HALF_SLOT) {
            char *half = strstr(reflection, "{\"msl_name\":\"fp_t_cb196610_1056805ASmplr\"");
            size_t half_bytes = (size_t)(strchr(half, '}') + 2 - half);
            if (variant == MISSING_HALF) memmove(half, half + half_bytes, strlen(half + half_bytes) + 1);
            else if (variant == DUP_HALF) memmove(half + half_bytes, half, strlen(half) + 1);
            else strstr(half, "\"reg\":0")[6] = '1';
        } else if (variant == WRONG_SET || variant == WRONG_BINDING) {
            memcpy(mutated, libpack_frag_spv, fragment_bytes);
            uint32_t target = 0;
            size_t n = fragment_bytes / sizeof(uint32_t);
            for (size_t i = 5; i < n; i += mutated[i] >> 16) {
                if (mutated[i] != 0x00040047 || mutated[i + 2] != 33 || mutated[i + 3] != 10) continue;
                target = mutated[i + 1];
                if (variant == WRONG_BINDING) mutated[i + 3] = 12;
            }
            CHECK(target != 0, "fixture has descriptor binding 10");
            if (variant == WRONG_SET) for (size_t i = 5; i < n; i += mutated[i] >> 16)
                if (mutated[i] == 0x00040047 && mutated[i + 1] == target && mutated[i + 2] == 34) mutated[i + 3] = 1;
            fragment = (const uint8_t *)mutated;
        } else if (variant == DESCRIPTOR_ARRAY) {
            fragment = (const uint8_t *)libpack_array_spv; fragment_bytes = sizeof libpack_array_spv;
        } else if (variant == EXTRA_SAMPLER || variant == SKIPPED_KIND) {
            fragment = (const uint8_t *)libpack_half_spv; fragment_bytes = sizeof libpack_half_spv;
            if (variant == SKIPPED_KIND) {
                char *half = strstr(reflection, "1056805ASmplr\"");
                strstr(half, "\"arg_id\":3")[9] = '5';
                snprintf(reflection + strlen(reflection) - 2, sizeof reflection - strlen(reflection) + 2,
                         ",{\"msl_name\":\"fp_sampler_only\",\"kind\":\"samp\",\"reg\":0,\"arg_id\":3}]}");
            }
        }
        if (variant >= SUPPORT_FROM_SPV && variant <= SUPPORT_WRONG_OWNER) {
            fragment = (const uint8_t *)libpack_support_v2_frag_spv;
            fragment_bytes = sizeof libpack_support_v2_frag_spv;
            if (variant != SUPPORT_FROM_SPV) {
                const char *member = variant == SUPPORT_LISTED ?
                    ",{\"msl_name\":\"support_buffer\",\"kind\":\"support\",\"arg_id\":4}]}" :
                    ",{\"msl_name\":\"fp_c3_wrong_owner\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":4}]}";
                size_t end = strlen(reflection) - 2;
                snprintf(reflection + end, sizeof reflection - end, "%s", member);
            }
        }
        uint32_t unnamed_support[sizeof libpack_support_v2_frag_spv / sizeof(uint32_t)];
        if (variant == SUPPORT_ON_IMAGE || variant == NAMED_SUPPORT_IMAGE) {
            // 只改反射的同号资源；原 SPIR-V 仍实际用 imageLoad 读取 b16。
            size_t end = (size_t)(strstr(reflection, "{\"msl_name\":\"fp_i0\"") - reflection);
            snprintf(reflection + end, sizeof reflection - end,
                     "{\"msl_name\":\"support_buffer\",\"kind\":\"support\",\"arg_id\":2}]}");
            if (variant == NAMED_SUPPORT_IMAGE) {
                fragment = (const uint8_t *)libpack_named_image_spv;
                fragment_bytes = sizeof libpack_named_image_spv;
            }
        } else if (variant == UNNAMED_SUPPORT_UBO) {
            size_t end = strlen(reflection) - 2;
            snprintf(reflection + end, sizeof reflection - end,
                     ",{\"msl_name\":\"support_buffer\",\"kind\":\"support\",\"arg_id\":4}]}");
            memcpy(unnamed_support, libpack_support_v2_frag_spv, sizeof unnamed_support);
            for (size_t i = 5; i < sizeof unnamed_support / sizeof *unnamed_support; i += unnamed_support[i] >> 16) {
                if (unnamed_support[i] != 0x00060005u ||
                    strcmp((const char *)&unnamed_support[i + 2], "support_buffer")) continue;
                unnamed_support[i + 2] ^= 0x20u;   // OpName 首字母换大写，描述符布局及用法不变。
            }
            fragment = (const uint8_t *)unnamed_support;
            fragment_bytes = sizeof unnamed_support;
        }
        const uint8_t *vertex = libpack_vert_spv;
        uint32_t vertex_set[sizeof libpack_vert_spv / sizeof(uint32_t)];
        if (variant == MIXED_V2_WRONG_SET) {
            memcpy(vertex_set, libpack_vert_spv, sizeof vertex_set);
            for (size_t i = 5; i < sizeof vertex_set / sizeof *vertex_set; i += vertex_set[i] >> 16)
                if (vertex_set[i] == 0x00040047 && vertex_set[i + 2] == 34) vertex_set[i + 3] = 1;
            vertex = (const uint8_t *)vertex_set;
        }
        char vs_reflection[1024];
        snprintf(vs_reflection, sizeof vs_reflection, "%s", VS_JSON);
        if (variant == MIXED_V2 || variant == MIXED_V1) {
            // 同号采样器行会被跳过，VS SPIR-V 仍读取 b12 的组合纹理。
            char *kind = strstr(vs_reflection, "\"kind\":\"tex\"") + strlen("\"kind\":\"");
            memmove(kind + 4, kind + 3, strlen(kind + 3) + 1);
            memcpy(kind, "samp", 4);
            uint64_t missing = 0;
            CHECK(nvn_vk_spirv_link_ok(libpack_vert_spv, sizeof libpack_vert_spv, fragment, fragment_bytes, &missing),
                  "mixed VS/FS modules link independently of descriptor reflection");
        }
        const char *ps = reflection;
        snprintf(g_dir, sizeof g_dir, "%s/sepsamp%d", tmpl, variant);
        CHECK(mkdir(g_dir, 0700) == 0, "create separate-sampler pack directory");
        struct trec rs[4] = {
            { .key = 0x0123456789abcdef, .kind = 1, .len = (uint32_t)strlen(vs_reflection) },
            { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
            { .key = 0x1032547698badcfe, .kind = 1, .len = (uint32_t)strlen(ps) },
            { .key = 0x1032547698badcfe, .kind = 52, .len = (uint32_t)fragment_bytes },
        };
        const uint8_t *pr[4] = { (const uint8_t *)vs_reflection, vertex, (const uint8_t *)ps, fragment };
        size_t nr[4] = { strlen(vs_reflection), sizeof libpack_vert_spv, strlen(ps), fragment_bytes };
        CHECK(mk_pack(rs, 4, pr, nr, 1) == 0, "separate-sampler pack written");
        struct s2a_libpack *slp = NULL; uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        uint8_t keys[6][8] = {{0}};
        const uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        const uint8_t kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        memcpy(keys[0], kv, 8); memcpy(keys[1], kp, 8);
        CHECK(s2a_libpack_open(&slp, g_dir) == 1 && slp, "open separate-sampler pack");
        int rc = slp ? s2a_libpack_program_v1(slp, keys, 3, NULL, 1, "h", "c", &j, &jl, w, sizeof w) : -1;
        if (variant == SEPARATE_C2 || variant == SUPPORT_FROM_SPV || variant == SUPPORT_LISTED) {
            struct fd_program parsed = {0}; char why[192] = "";
            CHECK(rc == 0 && j && has(j, "\"schema\":\"s2a-direct-program-v2\""),
                  "type-1 c2 separate sampler produces a v2 program");
            int parsed_rc = j ? nvn_vk_program_parse_graphics(j, jl, &parsed, why, sizeof why) : -1;
            const struct fd_binding *binding = NULL;
            int sampler_half = 0, support = 0;
            for (int i = 0; i < parsed.binding_count; i++) {
                const struct fd_binding *b = &parsed.bindings[i];
                if (b->binding == 10) binding = b;
                if (b->binding == 22) sampler_half++;
                if (b->binding == 28 && b->is_support && b->set == 0) support++;
            }
            if (variant == SEPARATE_C2) {
                CHECK(parsed_rc == 0 && binding && binding->set == 0 && binding->count == 1 &&
                      binding->tex_slot == 128 && binding->samp_slot == 0 && !sampler_half,
                      "v2 preserves the single combined descriptor with independent sampler slot 0");
            } else {
                if (parsed_rc) fprintf(stderr, "graphics v2 support parse: %s\n", why);
                CHECK(parsed_rc == 0 && binding && binding->tex_slot == 128 && binding->samp_slot == 0 && support == 1,
                      variant == SUPPORT_LISTED ? "graphics v2 explicit support is emitted once" :
                                                  "graphics v2 unlisted support is recovered from SPIR-V");
            }
            nvn_vk_program_free(&parsed);
        } else if (variant == SUPPORT_WRONG_OWNER || variant == MIXED_V2) {
            const char *reason = variant == SUPPORT_WRONG_OWNER ? "binding 28 is owned by support_buffer" :
                                                               "binding 12 has no mappable";
            CHECK(rc == S2A_LIBPACK_REJECT && !j && strstr(w, reason),
                  variant == SUPPORT_WRONG_OWNER ? "graphics v2 rejects guest uniform ownership of support binding" :
                                                   "graphics v2 validates emitted descriptors across both stages");
        } else if (variant == MIXED_V2_WRONG_SET) {
            CHECK(rc == S2A_LIBPACK_REJECT && !j && strstr(w, "descriptor outside set 0"),
                  "graphics v2 rejects a known binding in the wrong set of another stage");
        } else if (variant >= SUPPORT_ON_IMAGE) {
            const char *reason = variant == NAMED_SUPPORT_IMAGE ? "support_buffer is not a set 0 uniform block" :
                                 variant == UNNAMED_SUPPORT_UBO ? "binding 28 is not declared as support_buffer" :
                                                                  "binding 16 is not declared as support_buffer";
            CHECK(rc == S2A_LIBPACK_REJECT && !j && strstr(w, reason),
                  variant == SUPPORT_ON_IMAGE ? "support reflection cannot own unnamed storage image" :
                  variant == UNNAMED_SUPPORT_UBO ? "uniform block without the support name does not establish ownership" :
                                                    "support name cannot turn a storage image into a uniform block");
        } else if (variant != SAME_WORD && variant != MIXED_V1) {
            const char *reason = variant == DUP_HALF ? "duplicate sampler half" :
                    variant >= EXTRA_SAMPLER ? "binding 22 has no" :
                    variant >= WRONG_SET ? "single combined image sampler at set 0 binding 10" : "no consumable guest source";
            if (rc != S2A_LIBPACK_REJECT || j || !strstr(w, reason))
                fprintf(stderr, "separate sampler variant %d returned %d: %s\n", variant, rc, w);
            CHECK(rc == S2A_LIBPACK_REJECT && !j && strstr(w, reason), "invalid separate sampler shape is rejected before assembly");
        } else {
            CHECK(rc == 0 && j, "separate-sampler pack yields a v1 program");
            CHECK(has(j, "\"kind\":\"texture\",\"stage\":\"fs\",\"set\":0,\"binding\":10,\"slot\":90"),
                  "sep-tex + samp_tex（同字采样器）→ 槽 90");
            struct fd_program parsed = {0}; char why[192] = "";
            int parsed_rc = j ? nvn_vk_program_parse_graphics(j, jl, &parsed, why, sizeof why) : -1;
            const struct fd_binding *t10 = NULL;
            for (int i = 0; i < parsed.binding_count; i++) if (parsed.bindings[i].binding == 10) t10 = &parsed.bindings[i];
            CHECK(parsed_rc == 0 && t10 && t10->kind == 2 && t10->tex_slot == 128 && t10->samp_slot == -1,
                  "sep-tex + samp_tex：分离贴图槽 128、采样器取同字");
            if (variant == MIXED_V1) {
                int vs12 = 0;
                for (int i = 0; i < parsed.binding_count; i++) vs12 += parsed.bindings[i].binding == 12;
                CHECK(parsed_rc == 0 && has(j, "\"schema\":\"s2a-direct-program-v1\"") && !vs12,
                      "legacy v1 mixed fixture retains its reflection-based behavior");
            }
            nvn_vk_program_free(&parsed);
        }
        free(j); s2a_libpack_close(slp);
        char path[400]; snprintf(path, sizeof path, "%s/lib.idx", g_dir); unlink(path);
        snprintf(path, sizeof path, "%s/lib.000.pak", g_dir); unlink(path); rmdir(g_dir);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // 计算着色器实际读取组合纹理与支撑缓冲，分别核对反射列出和缺席两种输入。
    for (int variant = SUPPORT_FROM_SPV; variant <= SUPPORT_WRONG_OWNER; variant++) {
        const uint8_t key[8] = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7 };
        char reflection[512];
        snprintf(reflection, sizeof reflection,
                 "{\"key\":\"c0c1c2c3c4c5c6c7_b\",\"stage\":\"cs\",\"dm_ctl\":\"c0c1c2c3c4c5c6c7\",\"bindings\":["
                 "{\"msl_name\":\"cp_t_cb196610_1056805A\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
                 "{\"msl_name\":\"cp_t_cb196610_1056805ASmplr\",\"kind\":\"samp\",\"reg\":0,\"arg_id\":2},"
                 "{\"msl_name\":\"cp_i0\",\"kind\":\"img\",\"reg\":0,\"arg_id\":4}%s]}",
                 variant == SUPPORT_FROM_SPV ? "" : variant == SUPPORT_LISTED ?
                 ",{\"msl_name\":\"support_buffer\",\"kind\":\"support\",\"arg_id\":3}" :
                 ",{\"msl_name\":\"cp_c3_wrong_owner\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":3}");
        snprintf(g_dir, sizeof g_dir, "%s/v2cs%d", tmpl, variant);
        CHECK(mkdir(g_dir, 0700) == 0, "create compute v2 pack directory");
        size_t sizes[2] = { strlen(reflection), sizeof libpack_support_v2_comp_spv };
        const uint8_t *payloads[2] = { (const uint8_t *)reflection, (const uint8_t *)libpack_support_v2_comp_spv };
        struct trec records[2] = {
            { .key = 0xc0c1c2c3c4c5c6c7, .kind = 1, .len = (uint32_t)sizes[0] },
            { .key = 0xc0c1c2c3c4c5c6c7, .kind = 52, .len = (uint32_t)sizes[1] },
        };
        CHECK(mk_pack(records, 2, payloads, sizes, 1) == 0, "compute v2 pack written");
        struct s2a_libpack *cl = NULL;
        CHECK(s2a_libpack_open(&cl, g_dir) == 1 && cl, "open compute v2 pack");
        uint8_t *j = NULL; size_t jl = 0; char why[192] = "";
        int rc = cl ? s2a_libpack_compute_v1(cl, key, 1, "h", "c", 32768, &j, &jl, why, sizeof why) : -1;
        if (variant == SUPPORT_WRONG_OWNER) {
            CHECK(rc == S2A_LIBPACK_REJECT && !j && strstr(why, "binding 23 is owned by support_buffer"),
                  "compute v2 rejects guest uniform ownership of support binding");
        } else {
            CHECK(rc == 0 && has(j, "\"schema\":\"s2a-direct-program-v2\""), "compute separate sampler produces a v2 program");
            static struct nvn_vk_compute_program parsed;
            int parsed_rc = j ? nvn_vk_program_parse_compute(j, jl, &parsed, why, sizeof why) : -1;
            int support = 0, texture = 0;
            for (int i = 0; !parsed_rc && i < parsed.binding_count; i++) {
                const struct fd_binding *b = &parsed.bindings[i];
                support += b->binding == 23 && b->is_support && b->set == 0 && b->count == 1;
                texture += b->binding == 11 && b->tex_slot == 128 && b->samp_slot == 0;
            }
            if (parsed_rc) fprintf(stderr, "compute v2 support parse: %s\n", why);
            CHECK(parsed_rc == 0 && texture == 1 && support == 1,
                  variant == SUPPORT_LISTED ? "compute v2 explicit support is emitted once" :
                           "compute v2 unlisted support is recovered from SPIR-V");
            if (!parsed_rc) nvn_vk_program_free_compute(&parsed);
        }
        free(j); s2a_libpack_close(cl);
        char path[400]; snprintf(path, sizeof path, "%s/lib.idx", g_dir); unlink(path);
        snprintf(path, sizeof path, "%s/lib.000.pak", g_dir); unlink(path); rmdir(g_dir);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- miss：缺席键 / 非 vs、vs+fs、vs+gs+fs 形状 / 缺 spv 的键 ----
    {
        uint8_t keys[6][8] = {{0}};
        uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        uint64_t m0 = s2a_libpack_misses();
        memcpy(keys[0], kv, 8); keys[1][0] = 0xde; keys[1][1] = 0xad;
        CHECK(s2a_libpack_program_v1(lp, keys, 0x3, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j,
              "fs 键缺席 → MISS（半程序不命中）");
        keys[1][0] = 0xaa; keys[1][1] = 0xbb; keys[1][2] = 0xcc; keys[1][3] = 0xdd;
        keys[1][4] = 0xee; keys[1][5] = 0xff; keys[1][6] = 0x00; keys[1][7] = 0x11;
        CHECK(s2a_libpack_program_v1(lp, keys, 0x3, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == 1,
              "fs 键只有 _b.json 缺 _b.spv → MISS（半程序不命中）");
        CHECK(s2a_libpack_program_v1(lp, keys, 0x2 /* 只有 fs */, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j && strstr(w, "is not a vs"),
              "fs-only 形状（没有 vs）→ MISS 不产半成品");
        memcpy(keys[1], kv, 8);
        CHECK(s2a_libpack_program_v1(lp, keys, 0x5 /* vs|gs */, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == 1,
              "vs|gs 没有 fs（现编也不接这一形）→ MISS 不冒充命中");
        CHECK(s2a_libpack_misses() == m0 + 4, "miss counter +4");
    }

    // The existing VS and FS keys are present. A tessellation stage must make the whole program miss.
    {
        const uint8_t keys[6][8] = {
            {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef},
            {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe},
            {0x11}, {0x22},
        };
        const uint32_t vf = NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT;
        const uint32_t tcs = NVN_SHADER_STAGE_TESS_CONTROL_BIT, tes = NVN_SHADER_STAGE_TESS_EVALUATION_BIT;
        const uint32_t masks[] = {vf | tcs, vf | tes, vf | tcs | tes};
        const uint64_t m0 = s2a_libpack_misses();
        for (size_t i = 0; i < sizeof masks / sizeof masks[0]; i++) {
            uint8_t *j = NULL; size_t jl = 1; char w[192] = "";
            int rc = s2a_libpack_program_v1(lp, keys, masks[i], NULL, 1, "h", "c", &j, &jl, w, sizeof w);
            CHECK(rc == S2A_LIBPACK_MISS && !j && !jl, "tessellation stage → MISS, no partial program");
            free(j);
        }
        CHECK(s2a_libpack_misses() == m0 + 3, "tessellation masks count as three misses");
    }

    // ---- 只有 vs 的程序（0x1，只写深度的绘制）：只取真 vs 一段 + 它的真绑定，不造 fs ----
    // 期望（手算）：vs 键 0123456789abcdef 在夹具库里有 _b.json + _b.spv → HIT；结果的 stages 只有一项 "vs"、没有任何 fs 项
    //   （JSON 里 "fs" 字样一处都没有）；解析口径：allow_vs_only=1 接受（stage[1] 不在），allow_vs_only=0（原 vs+fs 接口）仍拒；
    //   负控：vs 键缺席 → MISS；拿 fs 的键当 vs → REJECT（库里那条 stage 是 ps）；只有 fs（0x2）/ vs|gs（0x5）→ MISS。
    {
        uint8_t keys[6][8] = {{0}};
        const uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef }, kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        memcpy(keys[0], kv, 8);
        const uint64_t h0 = s2a_libpack_hits(), m0 = s2a_libpack_misses(), r0 = s2a_libpack_rejects();
        int rc = s2a_libpack_program_v1(lp, keys, 0x1, NULL, 1, "h", "c", &j, &jl, w, sizeof w);
        if (rc != 0) printf("  vs-only: rc=%d why=%s\n", rc, w);
        CHECK(rc == 0 && j, "vs-only：真 vs 键 → HIT");
        CHECK(has(j, "\"kind\":\"graphics\",\"stages\":[{\"stage\":\"vs\",\"spirv\":\"") && !has(j, "\"stage\":\"fs\"") && !has(j, "\"fs\""),
              "vs-only 结果只有 vs 一段，没有造 fs");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"vs\",\"set\":0,\"binding\":0,\"slot\":3}"), "vs-only 带 vs 的真绑定（cbuf reg 0 → binding 0 slot 3）");
        if (j) {
            struct fd_program p1; char pw[192] = ""; memset(&p1, 0, sizeof p1);
            CHECK(nvn_vk_program_parse_graphics_stages(j, jl, &p1, 1, pw, sizeof pw) == 0 && p1.stage[0].present && !p1.stage[1].present && !p1.stage[2].present,
                  "allow_vs_only=1 解析：只有 vs 一段");
            nvn_vk_program_free(&p1);
            struct fd_program p2; memset(&p2, 0, sizeof p2); pw[0] = 0;
            CHECK(nvn_vk_program_parse_graphics(j, jl, &p2, pw, sizeof pw) != 0, "原 vs+fs 接口（allow_vs_only=0）仍拒只有 vs 的程序");
            nvn_vk_program_free(&p2);
        }
        free(j); j = NULL;
        // 负控：vs 键缺席
        { uint8_t k2[6][8] = {{0}}; k2[0][0] = 0xde; k2[0][1] = 0xad;
          rc = s2a_libpack_program_v1(lp, k2, 0x1, NULL, 1, "h", "c", &j, &jl, w, sizeof w);
          CHECK(rc == 1 && !j && !jl, "vs-only：vs 键缺席 → MISS"); }
        // 负控：错阶段（fs 的键当 vs 取：库里那条 _b.json 的 stage 是 ps）
        { uint8_t k3[6][8] = {{0}}; memcpy(k3[0], kp, 8); w[0] = 0;
          rc = s2a_libpack_program_v1(lp, k3, 0x1, NULL, 1, "h", "c", &j, &jl, w, sizeof w);
          CHECK(rc == -1 && !j && strstr(w, "stage is 'ps' not 'vs'"), "vs-only：拿 fs 键当 vs → REJECT（阶段不符）"); }
        // 负控：别的形状照旧 MISS，不冒充
        w[0] = 0;
        CHECK(s2a_libpack_program_v1(lp, keys, 0x2, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j, "fs-only（0x2）→ MISS");
        CHECK(s2a_libpack_program_v1(lp, keys, 0x5, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j, "vs|gs（0x5）→ MISS");
        CHECK(s2a_libpack_hits() == h0 + 1 && s2a_libpack_rejects() == r0 + 1 && s2a_libpack_misses() == m0 + 3, "vs-only 计数：命中 +1、拒 +1、miss +3");
    }

    // ---- reject：dm_ctl 不符 / spv 头坏 / 记录越界（vs 键是好的，坏在 fs 键——一段坏整个程序 reject）----
    {
        char w[192] = "";
        uint8_t keys[6][8] = {{0}};
        uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        uint8_t kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        memcpy(keys[0], kv, 8);
        memcpy(keys[1], kp, 8);
        uint8_t *j = NULL; size_t jl = 0;
        // fs 键的 dm_ctl 不符
        const char *bad = "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"ffffffffffffffff\",\"bindings\":[]}";
        struct trec r2[4] = {
            { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(VS_JSON) },
            { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
            { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(bad) },
            { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof libpack_frag_spv },
        };
        const uint8_t *p2[4] = { (const uint8_t *)VS_JSON, libpack_vert_spv, (const uint8_t *)bad, libpack_frag_spv };
        size_t n2[4] = { strlen(VS_JSON), sizeof libpack_vert_spv, strlen(bad), sizeof libpack_frag_spv };
        CHECK(mk_pack(r2, 4, p2, n2, 1) == 0, "dm_ctl-mismatch pack written");
        struct s2a_libpack *l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open mismatch pack");
        CHECK(s2a_libpack_program_v1(l2, keys, 0x3, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == -1 && !j,
              "fs 段 dm_ctl 不符 → 程序 REJECT 不采纳");
        s2a_libpack_close(l2);

        // fs 键的 spv 头坏（magic 不对）
        uint8_t badspv[20]; memcpy(badspv, libpack_frag_spv, sizeof badspv); badspv[0] = 0xAA;
        struct trec r3[4] = {
            { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(VS_JSON) },
            { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
            { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(PS_JSON) },
            { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof badspv },
        };
        const uint8_t *p3[4] = { (const uint8_t *)VS_JSON, libpack_vert_spv, (const uint8_t *)PS_JSON, badspv };
        size_t n3[4] = { strlen(VS_JSON), sizeof libpack_vert_spv, strlen(PS_JSON), sizeof badspv };
        CHECK(mk_pack(r3, 4, p3, n3, 1) == 0, "bad-spv pack written");
        l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open bad-spv pack");
        CHECK(s2a_libpack_program_v1(l2, keys, 0x3, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == -1,
              "fs 段 SPIR-V 头坏 → 程序 REJECT");
        s2a_libpack_close(l2);

        // 记录 off+len 越出分片（ssize 只写了真实长度，记录声称更长）
        {
            struct trec r4[1] = { { .key = 0x0123456789abcdef, .kind = 1, .len = 0x7fffffff } };
            const uint8_t *p4[1] = { (const uint8_t *)VS_JSON };
            size_t n4[1] = { strlen(VS_JSON) };
            CHECK(mk_pack(r4, 1, p4, n4, 1) == 0, "overrun pack written");
            l2 = NULL;
            CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open overrun pack");
            uint8_t *b = NULL; size_t n = 0;
            CHECK(s2a_libpack_get(l2, kv, 1, &b, &n, w, sizeof w) == -1 && !b,
                  "记录越出分片 → REJECT（不是按长度盲读）");
            s2a_libpack_close(l2);
        }

        // 尺寸表 ABI 2（离线翻译器认为宿主有贴图尺寸表）：支撑缓冲要 3776 字节，安卓不填表、整块清零 → 着色器走倍率 1 的旧路径。
        // 期望（手算，见 graphics/nvn/gfx/nvn/nvn_size_abi.inc）：2608 + 73 × 16 = 3776；没有这一项的程序仍是缺省 2608；不认识的 ABI 拒收。
        {
            static const char *PS_ABI2 =   // 标准片元夹具 + 缩放访问的贴图与尺寸表 ABI 2（绑定与 .spv 对得上）
                "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
                "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
                "{\"msl_name\":\"fp_t_sep\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
                "{\"msl_name\":\"fp_i0\",\"kind\":\"img\",\"reg\":2,\"arg_id\":2}],"
                "\"tex_scales\":[[2,\"tex\",1]],\"resolution_scale_abi\":2}";
            static const char *PS_ABI9 =
                "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":[],\"resolution_scale_abi\":9}";
            const char *ps[3] = { PS_JSON, PS_ABI2, PS_ABI9 };
            const uint32_t want[3] = { 2608, 3776, 0 };
            for (int t = 0; t < 3; t++) {
                struct trec r5[4] = {
                    { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(VS_JSON) },
                    { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
                    { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(ps[t]) },
                    { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof libpack_frag_spv },
                };
                const uint8_t *p5[4] = { (const uint8_t *)VS_JSON, libpack_vert_spv, (const uint8_t *)ps[t], libpack_frag_spv };
                size_t n5[4] = { strlen(VS_JSON), sizeof libpack_vert_spv, strlen(ps[t]), sizeof libpack_frag_spv };
                CHECK(mk_pack(r5, 4, p5, n5, 1) == 0, "size-abi pack written");
                l2 = NULL;
                CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open size-abi pack");
                j = NULL; jl = 0;
                int rc5 = s2a_libpack_program_v1(l2, keys, 0x3, NULL, 1, "h", "c", &j, &jl, w, sizeof w);
                if (!want[t]) {
                    CHECK(rc5 == -1 && !j, "resolution_scale_abi 9（不认识）→ REJECT");
                } else {
                    CHECK(rc5 == 0 && j, t ? "ABI 2 + tex_scales 程序 → HIT（不再拒收）" : "无尺寸表 ABI 的程序 → HIT");
                    CHECK(t ? has(j, "\"supportLayout\":{\"requiredSize\":3776}") : !has(j, "supportLayout"),
                          t ? "ABI 2 → v1 带 supportLayout.requiredSize 3776" : "无 ABI → v1 不带 supportLayout");
                    struct fd_program p5p; char pw5[192] = ""; memset(&p5p, 0, sizeof p5p);
                    CHECK(nvn_vk_program_parse_graphics(j, jl, &p5p, pw5, sizeof pw5) == 0 && p5p.support_bytes == want[t],
                          t ? "解析后支撑缓冲 3776 字节（2608 + 73×16）" : "解析后支撑缓冲缺省 2608 字节");
                    nvn_vk_program_free(&p5p);
                }
                free(j); j = NULL;
                s2a_libpack_close(l2);
            }
        }
    }

    // ---- .spv 声明的绑定为准（11-14 真机：图形预编全部漏绑 support_buffer，整屏画黑） ----
    // 期望（手算）：片元 .spv 在绑定 52 = 8×6+4 声明 support_buffer、_b.json 不列它 → v1 补一行 program 级 support（解析成 is_support）；
    //   _b.json 自己列了 support（arg 8）→ 不重复补；.spv 的绑定 24 在 _b.json 只有 ubolen 对应 / 完全没有对应 → 整个程序 REJECT。
    {
        static const char *PS_SUP_JSON =
            "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
            "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0}]}";
        static const char *PS_SUP_LISTED_JSON =
            "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
            "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
            "{\"msl_name\":\"support_buffer\",\"kind\":\"support\",\"arg_id\":8}]}";
        static const char *VS_UBOLEN_DECLARED =   // 绑定 24 在 .spv 里有，_b.json 只说它是 ubolen（v1 不出行的宿主表）
            "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
            "{\"msl_name\":\"vp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
            "{\"msl_name\":\"vp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":7,\"rw\":true,\"arg_id\":1},"
            "{\"msl_name\":\"vp_t_tcb_8\",\"kind\":\"tex\",\"reg\":5,\"arg_id\":2},"
            "{\"msl_name\":\"gal_ubo_len\",\"kind\":\"ubolen\",\"arg_id\":4}]}";
        static const char *VS_MISSING =           // 绑定 24 在 _b.json 里完全没有
            "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
            "{\"msl_name\":\"vp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
            "{\"msl_name\":\"vp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":7,\"rw\":true,\"arg_id\":1},"
            "{\"msl_name\":\"vp_t_tcb_8\",\"kind\":\"tex\",\"reg\":5,\"arg_id\":2}]}";
        struct { const char *vs, *ps; const uint8_t *fspv; size_t fn; int want; const char *what; } cs[4] = {
            { VS_JSON, PS_SUP_JSON, libpack_support_frag_spv, sizeof libpack_support_frag_spv, 0, "_b.json 不列 support" },
            { VS_JSON, PS_SUP_LISTED_JSON, libpack_support_frag_spv, sizeof libpack_support_frag_spv, 0, "_b.json 自己列了 support" },
            { VS_UBOLEN_DECLARED, PS_SUP_JSON, libpack_support_frag_spv, sizeof libpack_support_frag_spv, -1, "绑定 24 只有 ubolen 对应" },
            { VS_MISSING, PS_SUP_JSON, libpack_support_frag_spv, sizeof libpack_support_frag_spv, -1, "绑定 24 在 _b.json 里没有" },
        };
        uint8_t keys[6][8] = {{0}};
        const uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef }, kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        memcpy(keys[0], kv, 8); memcpy(keys[1], kp, 8);
        for (int t = 0; t < 4; t++) {
            snprintf(g_dir, sizeof g_dir, "%s/sup%d", tmpl, t);
            CHECK(mkdir(g_dir, 0700) == 0, "create support pack directory");
            struct trec r[4] = {
                { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(cs[t].vs) },
                { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
                { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(cs[t].ps) },
                { .key = 0x1032547698badcfe, .kind = 52, .len = (uint32_t)cs[t].fn },
            };
            const uint8_t *pl2[4] = { (const uint8_t *)cs[t].vs, libpack_vert_spv, (const uint8_t *)cs[t].ps, cs[t].fspv };
            size_t n2[4] = { strlen(cs[t].vs), sizeof libpack_vert_spv, strlen(cs[t].ps), cs[t].fn };
            CHECK(mk_pack(r, 4, pl2, n2, 1) == 0, "support pack written");
            struct s2a_libpack *sl = NULL;
            CHECK(s2a_libpack_open(&sl, g_dir) == 1, "open support pack");
            uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
            int rc = s2a_libpack_program_v1(sl, keys, 0x3, NULL, 1, "h", "c", &j, &jl, w, sizeof w);
            if (cs[t].want) {
                CHECK(rc == -1 && !j, cs[t].what);
            } else {
                const char *sup = "\"kind\":\"support\",\"stage\":\"program\",\"set\":0,\"binding\":52}";
                const char *a = j ? strstr((const char *)j, sup) : NULL;
                CHECK(rc == 0 && a && !strstr(a + 1, sup), cs[t].what);   // 恰好一行
                struct fd_program p; char pw[192] = ""; memset(&p, 0, sizeof p);
                int found = 0;
                if (j && nvn_vk_program_parse_graphics(j, jl, &p, pw, sizeof pw) == 0)
                    for (int i = 0; i < p.binding_count; i++) if (p.bindings[i].binding == 52 && p.bindings[i].is_support) found++;
                CHECK(found == 1, "解析后绑定 52 是 program 级支撑缓冲（恰好一个）");
                nvn_vk_program_free(&p);
            }
            free(j); s2a_libpack_close(sl);
        }
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- 计算键：合成 kind "compute" 的 v1，过 nvn_vk_program_parse_compute，绑定号与槽按手算 ----
    {
        snprintf(g_dir, sizeof g_dir, "%s/cs", tmpl);
        CHECK(mkdir(g_dir, 0700) == 0, "create compute pack directory");
        struct trec rc4[4] = {
            { .key = 0xc0c1c2c3c4c5c6c7, .kind = 1,  .len = (uint32_t)strlen(CS_JSON) },
            { .key = 0xc0c1c2c3c4c5c6c7, .kind = 52, .len = sizeof CS_SPV },
            { .key = 0xd0d1d2d3d4d5d6d7, .kind = 1,  .len = (uint32_t)strlen(CS_GMEM_JSON) },
            { .key = 0xd0d1d2d3d4d5d6d7, .kind = 52, .len = sizeof CS_SPV },
        };
        const uint8_t *pc[4] = { (const uint8_t *)CS_JSON, (const uint8_t *)CS_SPV, (const uint8_t *)CS_GMEM_JSON, (const uint8_t *)CS_SPV };
        size_t nc[4] = { strlen(CS_JSON), sizeof CS_SPV, strlen(CS_GMEM_JSON), sizeof CS_SPV };
        CHECK(mk_pack(rc4, 4, pc, nc, 1) == 0, "compute pack written");
        struct s2a_libpack *cl = NULL;
        CHECK(s2a_libpack_open(&cl, g_dir) == 1 && cl, "open compute pack");
        const uint8_t kc[8] = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7 };
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        uint64_t h0 = s2a_libpack_hits();
        int rc = cl ? s2a_libpack_compute_v1(cl, kc, 1, "h", "c", 32768, &j, &jl, w, sizeof w) : -9;
        CHECK(rc == 0 && j && jl == strlen((const char *)j), "cs key → HIT");
        CHECK(s2a_libpack_hits() == h0 + 1, "compute hit counter +1");
        CHECK(has(j, "\"kind\":\"compute\"") && has(j, "\"hasUnalignedStorageBuffer\":false")
              && has(j, "\"c0Reads\":null") && has(j, "\"supportReads\":null")
              && has(j, "\"supportLayout\":{\"requiredSize\":2608}"), "compute 顶层四事实：false / null / null / 2608");
        // 手算：binding = arg×6+5 → 5,11,17,23,29,35,(41 半边不出),47,(53 ubolen 不出)
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"cs\",\"set\":0,\"binding\":5,\"count\":1,\"slot\":0}"), "drvcb bank0 → uniform b5 slot0");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"cs\",\"set\":0,\"binding\":11,\"count\":1,\"slot\":4}"), "cbuf reg1 → uniform b11 slot4");
        CHECK(has(j, "\"binding\":17,\"count\":1,\"cbSlot\":0,\"cbOffset\":816,\"nvnSlot\":2,\"rw\":true"), "sbuf reg2 → b17 c0+0x330(816) nvnSlot2 rw");
        CHECK(has(j, "\"binding\":23,\"count\":1,\"cbSlot\":4,\"cbOffset\":32,\"uboSlot\":1,\"uboOffset\":32,\"rw\":false"), "sbuf_ubo reg1 off32 → b23 c4+32");
        CHECK(has(j, "\"binding\":29,\"count\":1,\"cbSlot\":0,\"cbOffset\":576,\"uboPtr\":3,\"rw\":true"), "ubo_ptr reg3 → b29 c0+0x240(576)");
        CHECK(has(j, "\"kind\":\"texture\",\"stage\":\"cs\",\"set\":0,\"binding\":35,\"count\":1,\"slot\":16}"), "tex reg4 → b35 slot 8+4×2=16");
        CHECK(has(j, "\"kind\":\"support\",\"stage\":\"program\",\"set\":0,\"binding\":47,\"count\":1}"), "support → b47 program");
        CHECK(!has(j, "\"binding\":41") && !has(j, "\"binding\":53"), "采样器半边 / ubolen 不出行");
        {
            static struct nvn_vk_compute_program cp; char pw[192] = "";
            int prc = j ? nvn_vk_program_parse_compute(j, jl, &cp, pw, sizeof pw) : -9;
            if (prc) printf("  parse_compute: %s\n", pw);
            CHECK(prc == 0, "合成计算 v1 过 nvn_vk_program_parse_compute");
            CHECK(cp.binding_count == 7, "9 条反射 -1 半边 -1 ubolen = 7 条");
            CHECK(cp.c0_reads_unknown == 1 && cp.support_reads_unknown == 1 && !cp.has_unaligned_storage_buffer
                  && cp.support_bytes == 2608 && cp.stage.present && cp.stage.spirv_bytes == sizeof CS_SPV,
                  "解析：c0/support 读集未知、对齐 false、support 2608、cs 段在");
            int seen = 0;
            for (int i = 0; i < cp.binding_count; i++) {
                const struct fd_binding *b = &cp.bindings[i]; const struct nvn_vk_compute_storage *sg = &cp.storage[i];
                if (b->binding == 5 && b->kind == 0 && b->slot == 0 && !b->is_support) seen |= 1;
                if (b->binding == 11 && b->kind == 0 && b->slot == 4 && b->ubo_slot == 1) seen |= 2;
                if (b->binding == 17 && b->kind == 1 && sg->mapping == NVK_STORAGE_NVN && sg->index == 2 && sg->writable) seen |= 4;
                if (b->binding == 23 && b->kind == 1 && sg->mapping == NVK_STORAGE_UBO_ADDRESS && sg->index == 1 && sg->offset == 32 && !sg->writable) seen |= 8;
                if (b->binding == 29 && b->kind == 1 && sg->mapping == NVK_STORAGE_UBO_PTR && sg->index == 3 && sg->writable) seen |= 16;
                if (b->binding == 35 && b->kind == 2 && b->tex_slot == 4 && !b->texel_buffer) seen |= 32;
                if (b->binding == 47 && b->is_support && b->stage_idx == -1) seen |= 64;
            }
            CHECK(seen == 127, "解析后七条逐条：c0 槽0 / ubo1 / NVN 存储槽2 / UBO 内指针 c4+32 / UBO 本体 3 / 纹理槽4 / support");
            nvn_vk_program_free_compute(&cp);
        }
        free(j); j = NULL;
        // 拿不到 guest 映射的存储（gmem_map）出库即拒：不产 slot -1 死绑定让解析器再拒（#1774）
        const uint8_t kg[8] = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7 };
        const uint64_t rg0 = s2a_libpack_rejects();
        rc = cl ? s2a_libpack_compute_v1(cl, kg, 1, "h", "c", 32768, &j, &jl, w, sizeof w) : -9;
        CHECK(rc == -1 && !j && strstr(w, "no consumable guest source") && strstr(w, "gmem_map"),
              "gmem_map 存储无 guest 映射 → REJECT（不产 slot -1 死绑定）");
        CHECK(s2a_libpack_rejects() == rg0 + 1, "dead binding reject counted +1");
        s2a_libpack_close(cl); cl = NULL;
        // .spv 声明 support_buffer、_b.json 不列 → 照图形口径按 .spv 补出 program 级 support 行（计算带 count 1），解析成 is_support；
        //   修前计算程序不补，后端描述符布局缺这个绑定（.spv 用到的绑定不在布局里）
        {
            struct trec rs[2] = {
                { .key = 0xe0e1e2e3e4e5e6e7, .kind = 1,  .len = (uint32_t)strlen(CS_NOSUP_JSON) },
                { .key = 0xe0e1e2e3e4e5e6e7, .kind = 52, .len = sizeof CS_SUP_SPV },
            };
            const uint8_t *ps[2] = { (const uint8_t *)CS_NOSUP_JSON, (const uint8_t *)CS_SUP_SPV };
            size_t ns[2] = { strlen(CS_NOSUP_JSON), sizeof CS_SUP_SPV };
            CHECK(mk_pack(rs, 2, ps, ns, 1) == 0, "compute support-from-spv pack written");
            CHECK(s2a_libpack_open(&cl, g_dir) == 1 && cl, "open compute support-from-spv pack");
            const uint8_t ke[8] = { 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7 };
            rc = cl ? s2a_libpack_compute_v1(cl, ke, 1, "h", "c", 32768, &j, &jl, w, sizeof w) : -9;
            if (rc) printf("  compute_v1: %s\n", w);
            CHECK(rc == 0 && has(j, "\"kind\":\"support\",\"stage\":\"program\",\"set\":0,\"binding\":47,\"count\":1}"),
                  ".spv 声明 support_buffer b47、_b.json 不列 → 补出 support b47 count 1");
            static struct nvn_vk_compute_program cp; char pw[192] = "";
            int prc = j ? nvn_vk_program_parse_compute(j, jl, &cp, pw, sizeof pw) : -9;
            if (prc) printf("  parse_compute: %s\n", pw);
            int sup = 0;
            for (int i = 0; !prc && i < cp.binding_count; i++) sup += cp.bindings[i].binding == 47 && cp.bindings[i].is_support;
            CHECK(prc == 0 && sup == 1 && cp.support_reads_unknown == 1, "解析：b47 is_support 恰一条、读集未知（运行时整块填恒等值）");
            if (!prc) nvn_vk_program_free_compute(&cp);
            free(j); j = NULL;
            s2a_libpack_close(cl); cl = NULL;
            CHECK(mk_pack(rc4, 4, pc, nc, 1) == 0, "compute pack rewritten");
            CHECK(s2a_libpack_open(&cl, g_dir) == 1 && cl, "reopen compute pack");
        }
        // 能力核接进取库：模块真用 64 位浮点 → 整个计算程序 REJECT（调用方退回现编），原因带 Float64
        {
            struct trec rf[2] = {
                { .key = 0xf0f1f2f3f4f5f6f7, .kind = 1,  .len = (uint32_t)strlen(CS_F64_JSON) },
                { .key = 0xf0f1f2f3f4f5f6f7, .kind = 52, .len = sizeof CAP_F64_SPV },
            };
            const uint8_t *pf[2] = { (const uint8_t *)CS_F64_JSON, (const uint8_t *)CAP_F64_SPV };
            size_t nf[2] = { strlen(CS_F64_JSON), sizeof CAP_F64_SPV };
            s2a_libpack_close(cl); cl = NULL;
            CHECK(mk_pack(rf, 2, pf, nf, 1) == 0, "compute float64 pack written");
            CHECK(s2a_libpack_open(&cl, g_dir) == 1 && cl, "open compute float64 pack");
            const uint8_t kf[8] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7 };
            w[0] = 0;
            const uint64_t rj0 = s2a_libpack_rejects();
            rc = cl ? s2a_libpack_compute_v1(cl, kf, 1, "h", "c", 32768, &j, &jl, w, sizeof w) : -9;
            CHECK(rc == -1 && !j && strstr(w, "fp64_type_used") && strstr(w, "Float64"), "模块用 OpTypeFloat 64 → 计算程序 REJECT（退回现编），原因 fp64_type_used");
            CHECK(s2a_libpack_rejects() == rj0 + 1, "能力核拒收走库拒收计数 +1");
            s2a_libpack_close(cl); cl = NULL;
            CHECK(mk_pack(rc4, 4, pc, nc, 1) == 0, "compute pack rewritten after float64 case");
            CHECK(s2a_libpack_open(&cl, g_dir) == 1 && cl, "reopen compute pack after float64 case");
        }
        // 负控：拿图形键（stage vs）当计算键 → REJECT；缺席键 → MISS
        uint64_t r0 = s2a_libpack_rejects(), m0 = s2a_libpack_misses();
        const uint8_t ka[8] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33 };
        CHECK(cl && s2a_libpack_compute_v1(cl, ka, 1, "h", "c", 32768, &j, &jl, w, sizeof w) == 1 && !j, "缺席 cs 键 → MISS");
        CHECK(s2a_libpack_misses() == m0 + 1, "compute miss counter +1");
        s2a_libpack_close(cl);
        const uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        // 用自带一份 vs json+spv 的新容器（前面 reject 段已把顶层目录的 lib.idx 改写过）
        snprintf(g_dir, sizeof g_dir, "%s/cs", tmpl);
        struct trec rv[2] = {
            { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(VS_JSON) },
            { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
        };
        const uint8_t *pv[2] = { (const uint8_t *)VS_JSON, libpack_vert_spv };
        size_t nv[2] = { strlen(VS_JSON), sizeof libpack_vert_spv };
        CHECK(mk_pack(rv, 2, pv, nv, 1) == 0, "vs-only pack written");
        cl = NULL; w[0] = 0;
        CHECK(s2a_libpack_open(&cl, g_dir) == 1 && s2a_libpack_compute_v1(cl, kv, 1, "h", "c", 32768, &j, &jl, w, sizeof w) == -1 && !j
              && strstr(w, "stage is 'vs' not 'cs'"), "vs 键当 cs 取 → stage 不符 REJECT");
        s2a_libpack_close(cl);
        CHECK(s2a_libpack_rejects() == r0 + 1, "compute reject counter +1");
        char path[400]; snprintf(path, sizeof path, "%s/cs/lib.idx", tmpl); unlink(path);
        snprintf(path, sizeof path, "%s/cs/lib.000.pak", tmpl); unlink(path);
        snprintf(path, sizeof path, "%s/cs", tmpl); rmdir(path);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- 死绑定出库即拒（#1774：不再产 slot -1 绑定让消费端再拒一遍）----
    {
        snprintf(g_dir, sizeof g_dir, "%s/dead", tmpl);
        CHECK(mkdir(g_dir, 0700) == 0, "create dead-binding pack directory");
        static const char *CS_TEX36 =   // tex reg36：图像表同址区 32..39 → c2 字 72+2×(36−32)=80，出库给槽（正样）
            "{\"key\":\"0808080808080808_b\",\"stage\":\"cs\",\"dm_ctl\":\"0808080808080808\",\"bindings\":["
            "{\"msl_name\":\"cp_t36\",\"kind\":\"tex\",\"reg\":36,\"arg_id\":0}]}";
        static const char *CS_TEX40 =   // tex reg40：贴图槽域出界——32..39 已接入，40..127 仍无映射拒收
            "{\"key\":\"0101010101010101_b\",\"stage\":\"cs\",\"dm_ctl\":\"0101010101010101\",\"bindings\":["
            "{\"msl_name\":\"cp_t40\",\"kind\":\"tex\",\"reg\":40,\"arg_id\":0}]}";
        static const char *CS_TEX64 =   // tex reg64：c2 无对应字——修前出 wrap 垃圾槽（compute guest slot invalid）
            "{\"key\":\"0202020202020202_b\",\"stage\":\"cs\",\"dm_ctl\":\"0202020202020202\",\"bindings\":["
            "{\"msl_name\":\"cp_t64\",\"kind\":\"tex\",\"reg\":64,\"arg_id\":0}]}";
        static const char *CS_TEX256 =  // tex reg256：出分离贴图表域
            "{\"key\":\"0303030303030303_b\",\"stage\":\"cs\",\"dm_ctl\":\"0303030303030303\",\"bindings\":["
            "{\"msl_name\":\"cp_t256\",\"kind\":\"tex\",\"reg\":256,\"arg_id\":0}]}";
        static const char *CS_TEXPOOL = // tex_pool：v1 不产池绑定字段
            "{\"key\":\"0404040404040404_b\",\"stage\":\"cs\",\"dm_ctl\":\"0404040404040404\",\"bindings\":["
            "{\"msl_name\":\"cp_pool\",\"kind\":\"tex_pool\",\"reg\":4,\"arg_id\":0}]}";
        static const char *CS_IMG9 =    // img reg9：NVN 图像槽只 0..7
            "{\"key\":\"0505050505050505_b\",\"stage\":\"cs\",\"dm_ctl\":\"0505050505050505\",\"bindings\":["
            "{\"msl_name\":\"cp_i9\",\"kind\":\"img\",\"reg\":9,\"arg_id\":0}]}";
        static const char *CS_SBUF16 =  // sbuf reg16：存储槽只 0..15
            "{\"key\":\"0606060606060606_b\",\"stage\":\"cs\",\"dm_ctl\":\"0606060606060606\",\"bindings\":["
            "{\"msl_name\":\"cp_s16\",\"kind\":\"sbuf\",\"reg\":16,\"rw\":true,\"arg_id\":0}]}";
        static const char *CS_DRVCB3 =  // drvcb bank3：非 c0/c2 的驱动常量段无 guest 槽
            "{\"key\":\"0707070707070707_b\",\"stage\":\"cs\",\"dm_ctl\":\"0707070707070707\",\"bindings\":["
            "{\"msl_name\":\"cp_cb3\",\"kind\":\"drvcb\",\"bank\":3,\"arg_id\":0}]}";
        const char *CS_JSONS[8] = { CS_TEX40, CS_TEX64, CS_TEX256, CS_TEXPOOL, CS_IMG9, CS_SBUF16, CS_DRVCB3, CS_TEX36 };
        struct trec rd[16];
        const uint8_t *pd[16]; size_t nd[16];
        for (int i = 0; i < 8; i++) {
            uint64_t k = (uint64_t)(i + 1) * 0x0101010101010101ULL;
            rd[i * 2]     = (struct trec){ .key = k, .kind = 1,  .len = (uint32_t)strlen(CS_JSONS[i]) };
            rd[i * 2 + 1] = (struct trec){ .key = k, .kind = 52, .len = sizeof CS_SPV };
            pd[i * 2] = (const uint8_t *)CS_JSONS[i]; nd[i * 2] = strlen(CS_JSONS[i]);
            pd[i * 2 + 1] = (const uint8_t *)CS_SPV;  nd[i * 2 + 1] = sizeof CS_SPV;
        }
        CHECK(mk_pack(rd, 16, pd, nd, 1) == 0, "dead-binding pack written");
        struct s2a_libpack *dl = NULL;
        CHECK(s2a_libpack_open(&dl, g_dir) == 1 && dl, "open dead-binding pack");
        for (int i = 0; i < 7; i++) {
            uint64_t k64 = (uint64_t)(i + 1) * 0x0101010101010101ULL;
            uint8_t k[8]; memcpy(k, &k64, 8);
            uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
            int rc = dl ? s2a_libpack_compute_v1(dl, k, 1, "h", "c", 32768, &j, &jl, w, sizeof w) : -9;
            if (!(rc == -1 && !j && strstr(w, "no consumable guest source"))) printf("  dead binding %d: rc=%d why=%s\n", i, rc, w);
            CHECK(rc == -1 && !j && strstr(w, "no consumable guest source"), "dead binding → REJECT with named reason");
            free(j);
        }
        {   // 正样：第 8 键 CS_TEX36——tex reg36 落图像表同址区 32..39 → c2 字 72+2×(36−32)=80，出库给槽不拒
            uint64_t k64 = 8 * 0x0101010101010101ULL;
            uint8_t k[8]; memcpy(k, &k64, 8);
            uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
            int rc = dl ? s2a_libpack_compute_v1(dl, k, 1, "h", "c", 32768, &j, &jl, w, sizeof w) : -9;
            if (rc != 0) printf("  tex36: rc=%d why=%s\n", rc, w);
            CHECK(rc == 0 && j && has(j, "\"kind\":\"texture\",\"stage\":\"cs\",\"set\":0,\"binding\":5,\"count\":1,\"slot\":80}"),
                  "tex reg36 → b5 slot 72+2×4=80");
            free(j);
        }
        s2a_libpack_close(dl);
        // 图形侧：drvcb bank 2（句柄表，图形只映射 bank 0）与 img reg≥8 同样出库即拒——用 vs 塞死绑定 + 正常 fs 的程序对验证整程序 REJECT
        static const char *VS_DRVCB_JSON =
            "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
            "{\"msl_name\":\"vp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
            "{\"msl_name\":\"vp_cb\",\"kind\":\"drvcb\",\"bank\":2,\"arg_id\":1},"
            "{\"msl_name\":\"vp_t0\",\"kind\":\"tex\",\"reg\":0,\"arg_id\":2},"
            "{\"msl_name\":\"vp_c4\",\"kind\":\"cbuf\",\"reg\":1,\"arg_id\":4}]}";   // 绑定 12/24 各有覆盖——死点只剩 drvcb bank 2
        static const char *VS_IMG9_JSON =
            "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
            "{\"msl_name\":\"vp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
            "{\"msl_name\":\"vp_i9\",\"kind\":\"img\",\"reg\":9,\"arg_id\":1},"
            "{\"msl_name\":\"vp_t0\",\"kind\":\"tex\",\"reg\":0,\"arg_id\":2},"
            "{\"msl_name\":\"vp_c4\",\"kind\":\"cbuf\",\"reg\":1,\"arg_id\":4}]}";
        const char *gvs[2] = { VS_DRVCB_JSON, VS_IMG9_JSON };
        uint8_t gkeys[6][8] = {{0}};
        const uint8_t gkv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef }, gkp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        memcpy(gkeys[0], gkv, 8); memcpy(gkeys[1], gkp, 8);
        for (int t = 0; t < 2; t++) {
            struct trec rg[4] = {
                { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(gvs[t]) },
                { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof libpack_vert_spv },
                { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(PS_JSON) },
                { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof libpack_frag_spv },
            };
            const uint8_t *pg[4] = { (const uint8_t *)gvs[t], libpack_vert_spv, (const uint8_t *)PS_JSON, libpack_frag_spv };
            size_t ng[4] = { strlen(gvs[t]), sizeof libpack_vert_spv, strlen(PS_JSON), sizeof libpack_frag_spv };
            CHECK(mk_pack(rg, 4, pg, ng, 1) == 0, "graphics dead-binding pack written");
            dl = NULL; char w[192] = ""; uint8_t *j = NULL; size_t jl = 0;
            CHECK(s2a_libpack_open(&dl, g_dir) == 1, "open graphics dead pack");
            int rc = dl ? s2a_libpack_program_v1(dl, gkeys, 0x3, NULL, 1, "h", "c", &j, &jl, w, sizeof w) : -9;
            if (!(rc == -1 && !j && strstr(w, "no consumable guest source"))) printf("  graphics dead binding %d: rc=%d why=%s\n", t, rc, w);
            CHECK(rc == -1 && !j && strstr(w, "no consumable guest source"), "graphics dead binding → REJECT");
            free(j); s2a_libpack_close(dl);
        }
        char path[400]; snprintf(path, sizeof path, "%s/lib.idx", g_dir); unlink(path);
        snprintf(path, sizeof path, "%s/lib.000.pak", g_dir); unlink(path); rmdir(g_dir);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- 图形 c0 驱动常量段（drvcb bank 0）：_b.json 出处（bank 0）+ .spv 证实（同一绑定号是 set 0 的 Uniform Block）才映射成 slot 0（消费端 NVN_RB_DRV0）----
    // 期望（手算）：fp_c0 arg_id 3 → 绑定 3×6+4 = 22；vp_c0 arg_id 1 → 绑定 1×6+0 = 6。
    //   正例：出 {kind uniform, stage fs, binding 22, slot 0}，解析后 slot 0、非 support。
    //   负控：bank 2 / bank 3（图形只映射 bank 0）→ REJECT「no consumable guest source」；arg_id 4（绑定 28，.spv 没有）、
    //   StorageBuffer 存储类、BufferBlock（非 Block）、描述符集 1、同一绑定号两个变量 → REJECT「not a set 0 Uniform Block」；都不出半个程序。
    {
        snprintf(g_dir, sizeof g_dir, "%s/c0", tmpl);
        CHECK(mkdir(g_dir, 0700) == 0, "create graphics c0 pack directory");
        static const char *PS_C0_FMT =
            "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
            "{\"msl_name\":\"fp_c%u\",\"kind\":\"drvcb\",\"bank\":%u,\"arg_id\":%u}]}";
        static const char *VS_C0_JSON =
            "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
            "{\"msl_name\":\"vp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":1}]}";
        char ps[512];
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        snprintf(ps, sizeof ps, PS_C0_FMT, 0u, 0u, 3u);
        int rc = c0_program(VS_JSON, libpack_vert_spv, sizeof libpack_vert_spv, ps, libpack_c0_frag_spv, sizeof libpack_c0_frag_spv, &j, &jl, w, sizeof w);
        if (rc != 0) printf("  fs c0 bank 0: rc=%d why=%s\n", rc, w);
        CHECK(rc == 0 && j, "graphics fs c0 bank 0 + Uniform Block at binding 22 → HIT");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"fs\",\"set\":0,\"binding\":22,\"slot\":0}"), "fs c0 → uniform binding 22 slot 0");
        if (j) {
            struct fd_program prog; char pw[192] = ""; memset(&prog, 0, sizeof prog);
            int found = 0;
            CHECK(nvn_vk_program_parse_graphics(j, jl, &prog, pw, sizeof pw) == 0, "fs c0 program parses");
            for (int i = 0; i < prog.binding_count; i++)
                if (prog.bindings[i].binding == 22) {
                    found = 1;
                    CHECK(prog.bindings[i].kind == 0 && prog.bindings[i].slot == 0 && !prog.bindings[i].is_support &&
                          !strcmp(prog.bindings[i].stage, "fs"), "parsed c0 binding: uniform, slot 0 (→ NVN_RB_DRV0), fs, not support");
                }
            CHECK(found, "parsed program has the c0 binding 22");
            nvn_vk_program_free(&prog);
        }
        free(j); j = NULL;
        // 顶点阶段同口径
        rc = c0_program(VS_C0_JSON, libpack_c0_vert_spv, sizeof libpack_c0_vert_spv, PS_JSON, libpack_frag_spv, sizeof libpack_frag_spv, &j, &jl, w, sizeof w);
        if (rc != 0) printf("  vs c0 bank 0: rc=%d why=%s\n", rc, w);
        CHECK(rc == 0 && has(j, "\"kind\":\"uniform\",\"stage\":\"vs\",\"set\":0,\"binding\":6,\"slot\":0}"), "graphics vs c0 bank 0 → uniform binding 6 slot 0");
        free(j); j = NULL;
        // 负控：异 bank
        for (unsigned bank = 2; bank <= 3; bank++) {
            snprintf(ps, sizeof ps, PS_C0_FMT, bank, bank, 3u);
            w[0] = 0;
            rc = c0_program(VS_JSON, libpack_vert_spv, sizeof libpack_vert_spv, ps, libpack_c0_frag_spv, sizeof libpack_c0_frag_spv, &j, &jl, w, sizeof w);
            if (!(rc == -1 && !j && strstr(w, "no consumable guest source"))) printf("  fs drvcb bank %u: rc=%d why=%s\n", bank, rc, w);
            CHECK(rc == -1 && !j && strstr(w, "no consumable guest source"), "graphics drvcb bank != 0 → REJECT (no consumable guest source)");
            free(j); j = NULL;
        }
        // 负控：错绑定号（_b.json 说 arg_id 4 = 绑定 28，.spv 在 28 没有声明）
        snprintf(ps, sizeof ps, PS_C0_FMT, 0u, 0u, 4u);
        w[0] = 0;
        rc = c0_program(VS_JSON, libpack_vert_spv, sizeof libpack_vert_spv, ps, libpack_c0_frag_spv, sizeof libpack_c0_frag_spv, &j, &jl, w, sizeof w);
        if (!(rc == -1 && !j && strstr(w, "not a set 0 Uniform Block"))) printf("  wrong binding: rc=%d why=%s\n", rc, w);
        CHECK(rc == -1 && !j && strstr(w, "not a set 0 Uniform Block"), "c0 at a binding the SPIR-V does not declare → REJECT");
        free(j); j = NULL;
        // 负控：绑定号对、类型不对
        snprintf(ps, sizeof ps, PS_C0_FMT, 0u, 0u, 3u);
        const char *what[4] = { "BufferBlock (not a Block)", "StorageBuffer storage class", "descriptor set 1", "two variables on binding 22" };
        for (int t = 0; t < 4; t++) {
            uint32_t bad[sizeof libpack_c0_frag_spv / sizeof(uint32_t)];
            memcpy(bad, libpack_c0_frag_spv, sizeof bad);
            c0_patch_spv(bad, sizeof bad / sizeof bad[0], t);
            w[0] = 0;
            rc = c0_program(VS_JSON, libpack_vert_spv, sizeof libpack_vert_spv, ps, bad, sizeof bad, &j, &jl, w, sizeof w);
            if (!(rc == -1 && !j && strstr(w, "not a set 0 Uniform Block"))) printf("  wrong type %s: rc=%d why=%s\n", what[t], rc, w);
            CHECK(rc == -1 && !j && strstr(w, "not a set 0 Uniform Block"), "c0 binding the SPIR-V cannot prove (type / set / ambiguous) → REJECT");
            free(j); j = NULL;
        }
        char path[400]; snprintf(path, sizeof path, "%s/lib.idx", g_dir); unlink(path);
        snprintf(path, sizeof path, "%s/lib.000.pak", g_dir); unlink(path); rmdir(g_dir);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- 计算共享内存闸：_b.json 带 spill 标记时核 .spv 的 Workgroup 字节 vs 设备 maxComputeSharedMemorySize ----
    {
        snprintf(g_dir, sizeof g_dir, "%s/wg", tmpl);
        CHECK(mkdir(g_dir, 0700) == 0, "create spill pack directory");
        static const char *CS_SPILL_JSON =
            "{\"key\":\"a0a1a2a3a4a5a6a7_b\",\"stage\":\"cs\",\"dm_ctl\":\"a0a1a2a3a4a5a6a7\","
            "\"spill\":45056,\"spill_slot\":27,\"bindings\":["
            "{\"msl_name\":\"cp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":0},"
            "{\"msl_name\":\"support_buffer\",\"kind\":\"support\",\"arg_id\":1}]}";
        // 最小 SPV + 一个 Workgroup 数组（u32[11264] = 45056B，与 _b.json 的 spill 值对应）
        static const uint32_t CS_WG_SPV[] = {
            0x07230203u, 0x00010000u, 0u, 7u, 0u,
            0x00020011u, 1u,                     // OpCapability Shader
            0x00040015u, 1u, 32u, 1u,            // OpTypeInt %1 32 signed
            0x0004002bu, 1u, 2u, 11264u,         // OpConstant %1 %2 11264
            0x0004001cu, 3u, 1u, 2u,             // OpTypeArray %3 %1 %2
            0x00040020u, 4u, 4u, 3u,             // OpTypePointer %4 Workgroup %3
            0x0004003bu, 4u, 5u, 4u,             // OpVariable %4 %5 Workgroup
        };
        struct trec rw_[2] = {
            { .key = 0xa0a1a2a3a4a5a6a7, .kind = 1,  .len = (uint32_t)strlen(CS_SPILL_JSON) },
            { .key = 0xa0a1a2a3a4a5a6a7, .kind = 52, .len = sizeof CS_WG_SPV },
        };
        const uint8_t *pw_[2] = { (const uint8_t *)CS_SPILL_JSON, (const uint8_t *)CS_WG_SPV };
        size_t nw_[2] = { strlen(CS_SPILL_JSON), sizeof CS_WG_SPV };
        CHECK(mk_pack(rw_, 2, pw_, nw_, 1) == 0, "spill pack written");
        struct s2a_libpack *sl_ = NULL;
        CHECK(s2a_libpack_open(&sl_, g_dir) == 1 && sl_, "open spill pack");
        const uint8_t ks[8] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7 };
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        // 上限 32768 < Workgroup 45056：出库即拒、原因带两边数值（退回现编由调用方做）
        int rc = sl_ ? s2a_libpack_compute_v1(sl_, ks, 1, "h", "c", 32768, &j, &jl, w, sizeof w) : -9;
        if (!(rc == -1 && !j && strstr(w, "compute shared memory 45056 exceeds device limit 32768"))) printf("  spill gate: rc=%d why=%s\n", rc, w);
        CHECK(rc == -1 && !j && strstr(w, "compute shared memory 45056 exceeds device limit 32768"), "spill WG>limit → REJECT 具名");
        // 上限 65536 ≥ 45056：同一份记录照常出库（spill 是批翻链 MSL 侧事实，.spv 原样可用）
        rc = sl_ ? s2a_libpack_compute_v1(sl_, ks, 1, "h", "c", 65536, &j, &jl, w, sizeof w) : -9;
        CHECK(rc == 0 && j && has(j, "\"kind\":\"compute\""), "WG≤limit → HIT（spill 标记不影响可用的 .spv）");
        free(j);
        s2a_libpack_close(sl_);
        char path[400]; snprintf(path, sizeof path, "%s/lib.idx", g_dir); unlink(path);
        snprintf(path, sizeof path, "%s/lib.000.pak", g_dir); unlink(path); rmdir(g_dir);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- 几何程序（vs+fs+gs，stages 0x7）：原生几何阶段命中 / 没开 geometryShader 照旧 miss / 图元挑份 / 视口号拒 ----
    {
        snprintf(g_dir, sizeof g_dir, "%s/gs", tmpl);
        CHECK(mkdir(g_dir, 0700) == 0, "create geometry pack directory");
        // 键 6060…：三角那一格（112+3 / 120+3）是好的；键 7070…：同一格的 .spv 写视口号；键 8080…：json 自称点输入
        struct trec rg[10] = {
            { .key = 0x0123456789abcdef, .kind = 1,   .len = (uint32_t)strlen(VS_JSON) },
            { .key = 0x0123456789abcdef, .kind = 52,  .len = sizeof libpack_vert_spv },
            { .key = 0x1032547698badcfe, .kind = 1,   .len = (uint32_t)strlen(PS_JSON) },
            { .key = 0x1032547698badcfe, .kind = 52,  .len = sizeof libpack_frag_spv },
            { .key = 0x6060606060606060, .kind = 115, .len = (uint32_t)strlen(GS_JSON) },
            { .key = 0x6060606060606060, .kind = 123, .len = sizeof libpack_geom_spv },
            { .key = 0x7070707070707070, .kind = 115, .len = (uint32_t)strlen(GS_JSON) },
            { .key = 0x7070707070707070, .kind = 123, .len = sizeof libpack_geom_viewport_spv },
            { .key = 0x8080808080808080, .kind = 115, .len = (uint32_t)strlen(GS_WRONGTOPO_JSON) },
            { .key = 0x8080808080808080, .kind = 123, .len = sizeof libpack_geom_spv },
        };
        // 7070… / 8080… 的 json 里 dm_ctl 要是自己的键：按键改写 GS_JSON 的那 16 位
        char gs7[1024], gs8[1024];
        snprintf(gs7, sizeof gs7, "%s", GS_JSON); snprintf(gs8, sizeof gs8, "%s", GS_WRONGTOPO_JSON);
        for (char *q = gs7; (q = strstr(q, "6060606060606060")); q += 16) memcpy(q, "7070707070707070", 16);
        for (char *q = gs8; (q = strstr(q, "6060606060606060")); q += 16) memcpy(q, "8080808080808080", 16);
        const uint8_t *pg[10] = { (const uint8_t *)VS_JSON, libpack_vert_spv, (const uint8_t *)PS_JSON, libpack_frag_spv,
                                  (const uint8_t *)GS_JSON, (const uint8_t *)libpack_geom_spv,
                                  (const uint8_t *)gs7, (const uint8_t *)libpack_geom_viewport_spv,
                                  (const uint8_t *)gs8, (const uint8_t *)libpack_geom_spv };
        size_t ng[10] = { strlen(VS_JSON), sizeof libpack_vert_spv, strlen(PS_JSON), sizeof libpack_frag_spv,
                          strlen(GS_JSON), sizeof libpack_geom_spv, strlen(gs7), sizeof libpack_geom_viewport_spv,
                          strlen(gs8), sizeof libpack_geom_spv };
        CHECK(mk_pack(rg, 10, pg, ng, 1) == 0, "geometry pack written");
        struct s2a_libpack *glp = NULL;
        CHECK(s2a_libpack_open(&glp, g_dir) == 1 && glp, "open geometry pack");
        uint8_t keys[6][8] = {{0}};
        const uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        const uint8_t kp[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
        memcpy(keys[0], kv, 8); memcpy(keys[1], kp, 8); memset(keys[2], 0x60, 8);   // 紧排：vs / fs / gs（NVN 位序 0 / 1 / 2）
        const struct s2a_libpack_gfx tri_on = { "Triangles", PROFILE_GS_ON, strlen(PROFILE_GS_ON), 0, 0 };
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";

        // 命中：三段齐、gs 绑定号 = arg×6+3、宿主解析出第 2 段（gs）
        uint64_t h0 = s2a_libpack_hits();
        int rc = glp ? s2a_libpack_program_v1(glp, keys, 0x7, &tri_on, 1, "h", "c", &j, &jl, w, sizeof w) : -9;
        CHECK(rc == 0 && j, "vs+fs+gs、三角、geometryShader 开 → HIT");
        CHECK(s2a_libpack_hits() == h0 + 1, "geometry hit counter +1");
        CHECK(has(j, "\"stage\":\"vs\"") && has(j, "\"stage\":\"fs\"") && has(j, "\"stage\":\"gs\",\"spirv\":\""), "三段 stage 项齐（vs / fs / gs）");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"gs\",\"set\":0,\"binding\":3,\"slot\":3"), "gs cbuf reg0 → uniform binding=0×6+3=3 slot=3");
        CHECK(has(j, "\"kind\":\"texture\",\"stage\":\"gs\",\"set\":0,\"binding\":9,\"slot\":8"), "gs tex reg0 → texture binding=1×6+3=9 slot=8");
        CHECK(!has(j, "\"stage\":\"gs\",\"set\":0,\"binding\":4,") && !has(j, "\"stage\":\"gs\",\"set\":0,\"binding\":15,"),
              "负控：gs 绑定号不是按 ps 的阶段号 4 换算（0×6+4 / 1×6+4 / 采样器半边 2×6+3=15 都不出）");
        {
            struct fd_program p; char pw[192] = "";
            memset(&p, 0, sizeof p);
            CHECK(j && nvn_vk_program_parse_graphics(j, jl, &p, pw, sizeof pw) == 0, "合成的 vs+fs+gs v1 过 nvn_vk_program_parse_graphics");
            CHECK(p.stage[0].present && p.stage[1].present && p.stage[2].present && p.stage[2].spirv_bytes == sizeof libpack_geom_spv,
                  "fd 三段 present，gs 段 = _vg3.spv 原字节长");
            int g3 = 0, g9 = 0;
            for (int i = 0; i < p.binding_count; i++) {
                if (p.bindings[i].binding == 3 && p.bindings[i].stage_idx == 2 && p.bindings[i].kind == 0 && p.bindings[i].slot == 3) g3 = 1;
                if (p.bindings[i].binding == 9 && p.bindings[i].stage_idx == 2 && p.bindings[i].kind == 2 && p.bindings[i].tex_slot == 0) g9 = 1;
            }
            CHECK(g3 && g9, "fd gs 绑定：3 = uniform slot3、9 = texture tex_slot 0，stage_idx = gs");
            CHECK(p.binding_count == 7 + 2, "vs+fs 的 7 条 + gs 2 条（采样器半边不出行）= 9 条");
            nvn_vk_program_free(&p);
        }
        free(j); j = NULL;

        // 设备没开 geometryShader（profile 同字段 false）：照旧 miss，不合成
        uint64_t m0 = s2a_libpack_misses();
        const struct s2a_libpack_gfx tri_off = { "Triangles", PROFILE_GS_OFF, strlen(PROFILE_GS_OFF), 0, 0 };
        w[0] = 0;
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x7, &tri_off, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j
              && strstr(w, "geometryShader"), "geometryShader 没开 → MISS（原因写明）");
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x7, NULL, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j,
              "没给绘制事实（gfx = NULL）→ 几何程序 MISS");
        const struct s2a_libpack_gfx noprof = { "Triangles", NULL, 0, 0, 0 };
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x7, &noprof, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j,
              "profile 读不出 → 按没开 MISS");
        // 图元：线输入没有 _vg1 → miss；邻接离线不出 → miss
        const struct s2a_libpack_gfx lines = { "Lines", PROFILE_GS_ON, strlen(PROFILE_GS_ON), 0, 0 };
        const struct s2a_libpack_gfx adj = { "TrianglesAdjacency", PROFILE_GS_ON, strlen(PROFILE_GS_ON), 0, 0 };
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x7, &lines, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j,
              "线输入、库里只有三角那份 → MISS");
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x7, &adj, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j,
              "邻接输入 → MISS（离线不出邻接）");
        CHECK(s2a_libpack_misses() == m0 + 5, "geometry miss counter +5");
        CHECK(s2a_libpack_profile_geometry_shader(PROFILE_GS_ON, strlen(PROFILE_GS_ON)) == 1
              && s2a_libpack_profile_geometry_shader(PROFILE_GS_OFF, strlen(PROFILE_GS_OFF)) == 0
              && s2a_libpack_profile_geometry_shader("{\"supportsGeometryShader\":true}", strlen("{\"supportsGeometryShader\":true}")) == 0,
              "profile 判据只认 capabilities.supportsGeometryShader（顶层同名字段不算）");

        // reject：几何段写视口号；json 的输入图元与格子不符
        uint64_t r0 = s2a_libpack_rejects();
        memset(keys[2], 0x70, 8); w[0] = 0;
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x7, &tri_on, 1, "h", "c", &j, &jl, w, sizeof w) == -1 && !j
              && strstr(w, "viewport"), "几何段写 ViewportIndex → REJECT（现编同口径具名拒）");
        memset(keys[2], 0x80, 8); w[0] = 0;
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x7, &tri_on, 1, "h", "c", &j, &jl, w, sizeof w) == -1 && !j
              && strstr(w, "gs_input_topology"), "_vg3 里的 json 自称点输入 → REJECT");
        CHECK(s2a_libpack_rejects() == r0 + 2, "geometry reject counter +2");
        // vs+fs 程序在同一容器照常命中，不受几何事实影响（gfx 给了也不读）
        CHECK(glp && s2a_libpack_program_v1(glp, keys, 0x3, &tri_off, 1, "h", "c", &j, &jl, w, sizeof w) == 0 && j && !has(j, "\"stage\":\"gs\""),
              "同容器 vs+fs 程序照常 HIT、不带 gs 段");
        free(j);
        s2a_libpack_close(glp);
        char path[400]; snprintf(path, sizeof path, "%s/lib.idx", g_dir); unlink(path);
        snprintf(path, sizeof path, "%s/lib.000.pak", g_dir); unlink(path); rmdir(g_dir);
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    s2a_libpack_close(lp);
    char cleanup[400];
    snprintf(cleanup, sizeof cleanup, "%s/lib.idx", tmpl); unlink(cleanup);
    snprintf(cleanup, sizeof cleanup, "%s/lib.000.pak", tmpl); unlink(cleanup);
    snprintf(cleanup, sizeof cleanup, "%s/bad/lib.idx", tmpl); unlink(cleanup);
    snprintf(cleanup, sizeof cleanup, "%s/bad", tmpl); rmdir(cleanup);
    rmdir(tmpl);
    {   // 片元输出映射：期望值按汇编源手算（location 1 vec4 = 0xF0；location 3 分量 1..2 = 0x6000；FragDepth 不占颜色目标）
        uint32_t m = 0;
        CHECK(s2a_libpack_spv_frag_outputs((const uint8_t *)FO1_SPV, sizeof FO1_SPV, &m) == 0 && m == 0x60F0u, "片元输出映射：loc1 vec4 + loc3.yz → 0x60F0，内建深度不占");
        m = 0;
        CHECK(s2a_libpack_spv_frag_outputs(libpack_frag_spv, sizeof libpack_frag_spv, &m) == 0 && m == 0xFu, "片元输出映射：样本片元 location 0 vec4 → 0xF");
        CHECK(s2a_libpack_spv_frag_outputs((const uint8_t *)FO2_SPV, sizeof FO2_SPV, &m) != 0, "片元输出映射：数组输出读不懂 → 不出");
        CHECK(s2a_libpack_spv_frag_outputs(libpack_frag_spv, 12, &m) != 0, "片元输出映射：截断模块拒");
    }
    {   // 能力核：用不上的 Float64 / Int64 声明删掉（其余字不动），真用到的 / Int64Atomics 拒
        uint32_t m[sizeof CAP_UNUSED_SPV / 4]; memcpy(m, CAP_UNUSED_SPV, sizeof m);
        size_t n = sizeof m; char cw[128] = "";
        const uint64_t c0 = s2a_libpack_caps_stripped();
        const uint32_t want[] = { 0x07230203u, 0x00010000u, 0u, 3u, 0u, 0x00020011u, 1u, 0x00040015u, 1u, 32u, 0u, 0x00030016u, 2u, 32u };
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)m, &n, cw, sizeof cw) == 0 && n == sizeof want && !memcmp(m, want, sizeof want),
              "能力核：无 64 位类型 → 删 Float64 + Int64 两条，留 Shader 与类型");
        CHECK(s2a_libpack_caps_stripped() == c0 + 1, "能力核：删过能力声明的模块计 1 条（libpack_caps_stripped）");
        size_t n2 = n;
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)m, &n2, cw, sizeof cw) == 0 && n2 == n, "能力核：再核一遍不变（幂等）");
        CHECK(s2a_libpack_caps_stripped() == c0 + 1, "能力核：再核一遍没删东西，不再计");
        uint32_t f[sizeof CAP_F64_SPV / 4]; memcpy(f, CAP_F64_SPV, sizeof f); n = sizeof f; cw[0] = 0;
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)f, &n, cw, sizeof cw) == -1 && strstr(cw, "fp64_type_used") && strstr(cw, "Float64") && n == sizeof f,
              "能力核：OpTypeFloat 64 → 拒，原因 fp64_type_used");
        uint32_t fc[sizeof CAP_F64_CONST_SPV / 4]; memcpy(fc, CAP_F64_CONST_SPV, sizeof fc); n = sizeof fc; cw[0] = 0;
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)fc, &n, cw, sizeof cw) == -1 && strstr(cw, "fp64_type_used") && n == sizeof fc
              && !memcmp(fc, CAP_F64_CONST_SPV, sizeof fc), "能力核：只有 64 位常量类型、没有运算 → 拒收（不是删能力），模块原样");
        uint32_t a[sizeof CAP_I64A_SPV / 4]; memcpy(a, CAP_I64A_SPV, sizeof a); n = sizeof a; cw[0] = 0;
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)a, &n, cw, sizeof cw) == -1 && strstr(cw, "int64_atomics_used") && strstr(cw, "Int64Atomics"), "能力核：Int64Atomics → 拒，原因 int64_atomics_used");
        CHECK(s2a_libpack_caps_stripped() == c0 + 1, "能力核：拒收的模块不计入删能力计数");
        uint32_t ok[sizeof CS_SPV / 4]; memcpy(ok, CS_SPV, sizeof ok); n = sizeof ok;
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)ok, &n, cw, sizeof cw) == 0 && n == sizeof CS_SPV, "能力核：无能力声明的模块原样过");
    }
    {   // 片元像素锁：声明（能力 / 扩展 / 执行模式）一律删；真有临界区的两条指令也删、按模块计 libpack_interlock_dropped。期望值按 SPIR-V 编码手写
        enum { MAXW = 64 };
        uint32_t m[MAXW]; size_t k = 0;
        #define W(x) (m[k++] = (x))
        #define EXT(str) do { const char s_[] = str; const uint32_t nw_ = (uint32_t)((sizeof s_ + 3) / 4); W((nw_ + 1) << 16 | 10u); \
                              memset(&m[k], 0, nw_ * 4); memcpy(&m[k], s_, sizeof s_); k += nw_; } while (0)
        W(0x07230203u); W(0x00010000u); W(0u); W(3u); W(0u);
        W(0x00020011u); W(1u);                       // OpCapability Shader
        W(0x00020011u); W(5378u);                    // OpCapability FragmentShaderPixelInterlockEXT
        EXT("SPV_EXT_fragment_shader_interlock");    // 34 字节 → 9 字 + 头 = 10 字
        EXT("SPV_X");                                // 别的扩展：保留
        W(0x00030010u); W(1u); W(5366u);             // OpExecutionMode %1 PixelInterlockOrderedEXT
        W(0x00040015u); W(1u); W(32u); W(0u);        // OpTypeInt 32 0
        const size_t decl_end = k;
        W(0x000114F4u); W(0x000114F5u);              // OpBeginInvocationInterlockEXT / OpEndInvocationInterlockEXT
        const size_t full = k;
        uint32_t want[MAXW]; size_t wk = 0;
        want[wk++] = 0x07230203u; want[wk++] = 0x00010000u; want[wk++] = 0u; want[wk++] = 3u; want[wk++] = 0u;
        want[wk++] = 0x00020011u; want[wk++] = 1u;
        want[wk++] = 3u << 16 | 10u; { const char x[] = "SPV_X"; memset(&want[wk], 0, 8); memcpy(&want[wk], x, sizeof x); wk += 2; }
        want[wk++] = 0x00040015u; want[wk++] = 1u; want[wk++] = 32u; want[wk++] = 0u;
        #undef W
        #undef EXT
        uint32_t d[MAXW]; memcpy(d, m, decl_end * 4); size_t n = decl_end * 4; char cw[128] = "";
        const uint64_t c0 = s2a_libpack_caps_stripped(), i0 = s2a_libpack_interlock_dropped();
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)d, &n, cw, sizeof cw) == 0 && n == wk * 4 && !memcmp(d, want, wk * 4),
              "像素锁：只有声明 → 删能力 / 扩展名 / 执行模式三样，别的扩展与类型原样");
        CHECK(s2a_libpack_caps_stripped() == c0 + 1 && s2a_libpack_interlock_dropped() == i0, "像素锁：只有声明 → 记删能力 1 条，不记丢互锁");
        size_t n2 = n;
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)d, &n2, cw, sizeof cw) == 0 && n2 == n && s2a_libpack_caps_stripped() == c0 + 1, "像素锁：再核一遍不变、不再计");
        uint32_t f[MAXW]; memcpy(f, m, full * 4); n = full * 4;
        CHECK(s2a_libpack_spv_admit_caps((uint8_t *)f, &n, cw, sizeof cw) == 0 && n == wk * 4 && !memcmp(f, want, wk * 4),
              "像素锁：有临界区 → 不拒收，两条互锁指令也删，结果同上");
        CHECK(s2a_libpack_interlock_dropped() == i0 + 1 && s2a_libpack_caps_stripped() == c0 + 2, "像素锁：有临界区 → 丢互锁计 1 条（libpack_interlock_dropped）");
    }
    if (fails) { printf("libpack_vk_test: %d FAIL\n", fails); return 1; }
    printf("libpack_vk_test: all checks passed\n");
    return 0;
}
