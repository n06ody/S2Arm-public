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

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", (what)); fails++; } } while (0)

#define CHAIN "0be3f28548fd"   // 与 graphics/nvn/gfx/nvn/dmsh_chain.h DMSH_CHAIN 同值

static char g_dir[320];

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

static const uint8_t SPV[20] = { 0x03, 0x02, 0x23, 0x07, 0, 0, 1, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0 };

static const char *VS_JSON =
    "{\"key\":\"0123456789abcdef_b\",\"stage\":\"vs\",\"dm_ctl\":\"0123456789abcdef\",\"bindings\":["
    "{\"msl_name\":\"vp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"vp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":7,\"rw\":true,\"arg_id\":1},"
    "{\"msl_name\":\"vp_t_tcb_8\",\"kind\":\"tex\",\"reg\":5,\"arg_id\":2},"
    "{\"msl_name\":\"vp_t_tcb_8Smplr\",\"kind\":\"samp_tex\",\"reg\":5,\"arg_id\":3},"
    "{\"msl_name\":\"gal_ubo_len\",\"kind\":\"ubolen\",\"arg_id\":4}],"
    "\"vertex_inputs\":[{\"location\":1},{\"location\":3}]}";

static const char *PS_JSON =
    "{\"key\":\"1032547698badcfe_b\",\"stage\":\"ps\",\"dm_ctl\":\"1032547698badcfe\",\"bindings\":["
    "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"fp_t_sep\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
    "{\"msl_name\":\"fp_i0\",\"kind\":\"img\",\"reg\":2,\"arg_id\":2}]}";

// #1434 第二笔 compute 夹具：CS_JSON 仿 t_dsh_vklib 真出 cs `_b.json` 词汇
//   （cbuf/sbuf/tex/samp_tex/ubo_ptr/sbuf_ubo + ubolen/sbsize 宿主表条目）；
//   SPV_CS 是逐条 OpName/OpDecorate 手工铺的最小模块——绑定映射的真值侧是 .spv
//   声明（OpName ↔ msl_name 联接），不是 _b.json 的 arg_id。
static const char *CS_JSON =
    "{\"key\":\"400a08fca29679ef_b\",\"stage\":\"cs\",\"dm_ctl\":\"400a08fca29679ef\",\"bindings\":["
    "{\"msl_name\":\"cp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
    "{\"msl_name\":\"cp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":0,\"rw\":true,\"arg_id\":1},"
    "{\"msl_name\":\"cp_t\",\"kind\":\"tex\",\"reg\":3,\"arg_id\":2},"
    "{\"msl_name\":\"cp_tSmplr\",\"kind\":\"samp_tex\",\"reg\":3,\"arg_id\":3},"
    "{\"msl_name\":\"cp_ub\",\"kind\":\"ubo_ptr\",\"reg\":2,\"rw\":true,\"arg_id\":6},"
    "{\"msl_name\":\"cp_sb\",\"kind\":\"sbuf_ubo\",\"reg\":4,\"off\":32,\"rw\":false,\"arg_id\":7},"
    "{\"msl_name\":\"gal_ubo_len\",\"kind\":\"ubolen\",\"arg_id\":8},"
    "{\"msl_name\":\"spvBufferSizeConstants\",\"kind\":\"sbsize\",\"arg_id\":9}]}";

// .spv 词流补丁：OpName(id,name) + OpDecorate(id,Binding=b) + OpDecorate(id,DescriptorSet=0)。
static uint32_t *spv_var(uint32_t *w, uint32_t id, const char *name, uint32_t binding) {
    size_t nb = strlen(name) + 1, nw = (nb + 3) / 4;
    *w++ = (uint32_t)((2 + nw) << 16 | 5); *w++ = id;
    memset(w, 0, nw * 4); memcpy(w, name, nb); w += nw;
    *w++ = 4 << 16 | 71; *w++ = id; *w++ = 33; *w++ = binding;
    *w++ = 4 << 16 | 71; *w++ = id; *w++ = 34; *w++ = 0;
    return w;
}
static size_t mk_spv_cs(uint8_t *out, int smplr_decl) {
    uint32_t *w = (uint32_t *)out;
    w[0] = 0x07230203; w[1] = 0x10000; w[2] = 0; w[3] = 64; w[4] = 0; w += 5;
    w = spv_var(w, 10, "cp_c3", 5);           // arg0 cbuf
    w = spv_var(w, 11, "cp_s0", 11);          // arg1 sbuf
    w = spv_var(w, 12, "cp_t", 17);           // arg2 tex
    if (smplr_decl) w = spv_var(w, 13, "cp_tSmplr", 23);   // arg3 采样器半边
    w = spv_var(w, 14, "support_buffer", 29); // arg4 支撑缓冲（json 无名，按名接）
    w = spv_var(w, 15, "cp_c0", 35);          // arg5 c0 驱动常量块（json 无名，_c0 解）
    w = spv_var(w, 16, "cp_ub", 41);          // arg6 ubo_ptr
    w = spv_var(w, 17, "cp_sb", 47);          // arg7 sbuf_ubo
    return (size_t)((uint8_t *)w - out);
}
static const uint8_t CS_KEY[8] = { 0x40, 0x0a, 0x08, 0xfc, 0xa2, 0x96, 0x79, 0xef };

static int has(const uint8_t *j, const char *sub) { return j && strstr((const char *)j, sub) != NULL; }

int main(void) {
    char tmpl[] = "/tmp/libpack_vk_test.XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL, "mkdtemp");
    snprintf(g_dir, sizeof g_dir, "%s", tmpl);

    // ---- 缺席目录 / 坏 lib.idx ----
    {
        struct s2a_libpack *lp = (void *)1;
        CHECK(s2a_libpack_open(&lp, "/tmp/definitely-no-such-libpack-dir") == 0, "absent dir → open 0 (none)");
        snprintf(g_dir, sizeof g_dir, "%s/bad", tmpl);
        mkdir(g_dir, 0700);
        char p[400]; snprintf(p, sizeof p, "%s/lib.idx", g_dir);
        FILE *f = fopen(p, "wb"); fwrite("NOTALIB!", 1, 8, f); fclose(f);
        CHECK(s2a_libpack_open(&lp, g_dir) == -1, "8-byte bad lib.idx → open -1 (broken)");
        snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }

    // ---- 正常容器：两键 × (json+spv)，外加「只有 json 无 spv」的第三键，外加 cs 键 ----
    const char *ORPHAN_JSON =
        "{\"key\":\"aabbccddeeff0011_b\",\"stage\":\"vs\",\"dm_ctl\":\"aabbccddeeff0011\",\"bindings\":[]}";
    uint8_t spv_cs[512];
    size_t spv_cs_len = mk_spv_cs(spv_cs, 1);
    struct trec recs[7] = {
        { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(VS_JSON) },
        { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof SPV },
        { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(PS_JSON) },
        { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof SPV },
        { .key = 0xaabbccddeeff0011, .kind = 1,  .len = (uint32_t)strlen(ORPHAN_JSON) },
        { .key = 0x400a08fca29679ef, .kind = 1,  .len = (uint32_t)strlen(CS_JSON) },
        { .key = 0x400a08fca29679ef, .kind = 52, .len = (uint32_t)spv_cs_len },
    };
    const uint8_t *pl[7] = { (const uint8_t *)VS_JSON, SPV, (const uint8_t *)PS_JSON, SPV,
                             (const uint8_t *)ORPHAN_JSON, (const uint8_t *)CS_JSON, spv_cs };
    size_t pln[7] = { strlen(VS_JSON), sizeof SPV, strlen(PS_JSON), sizeof SPV,
                      strlen(ORPHAN_JSON), strlen(CS_JSON), spv_cs_len };
    CHECK(mk_pack(recs, 7, pl, pln, 1) == 0, "fixture pack written");

    struct s2a_libpack *lp = NULL;
    CHECK(s2a_libpack_open(&lp, g_dir) == 1 && lp, "open fixture pack → 1");

    // ---- s2a_libpack_get：命中 / 缺席 ----
    {
        uint8_t *b = NULL; size_t n = 0; char w[192] = "";
        uint8_t k[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        CHECK(s2a_libpack_get(lp, k, 1, &b, &n, w, sizeof w) == 0 && n == strlen(VS_JSON) && !memcmp(b, VS_JSON, n),
              "get kind1 → bytes identical");
        free(b); b = NULL;
        CHECK(s2a_libpack_get(lp, k, 52, &b, &n, w, sizeof w) == 0 && n == sizeof SPV && !memcmp(b, SPV, n),
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
        int rc = s2a_libpack_program_v1(lp, keys, 0x3 /* vs|fs */, 1, "sha256-aaa", "closure-x", &j, &jl, w, sizeof w);
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
        CHECK(has(j, "\"binding\":24}") && !has(j, "\"binding\":24,\"slot\""),
              "ubolen → uniform binding=24 无 guest 槽");
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
                  p.stage[0].spirv_bytes == 20 && p.stage[0].attr_mask == 10,
                  "fd vs+fs present + spirv + attr_mask=10");
            CHECK(p.binding_count == 7, "5+3 条反射 -1 采样器半边 = 7 条 v1 绑定");
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
            CHECK(b12 && b12->kind == 2 && b12->tex_slot == 5,
                  "fd tex → texture tex_slot=5（slot18→byte0x48→(0x48-0x20)/8 回环）");
            CHECK(b24 && b24->kind == 0 && b24->slot == -1, "fd ubolen → uniform 无槽（slot=-1）");
            // sep-tex 回环：slot90→byte0x168→int8 128 溢出负值 → tex_slot<0（独立纹理不属
            //   组合槽映射，如实不挂槽，不是静默错绑）
            CHECK(b10 && b10->kind == 2 && b10->tex_slot < 0 && b10->stage_idx == 1,
                  "fd sep-tex → tex_slot<0 不挂组合槽、stage_idx=fs");
            CHECK(b16 && b16->kind == 3 && b16->slot == 76 && b16->stage_idx == 1,
                  "fd img → kind3 slot76 原样、stage_idx=fs");
            nvn_vk_program_free(&p);
        }
        free(j);
    }

    // ---- miss：缺席键 / 非 vs+fs 形状 / 缺 spv 的键 ----
    {
        uint8_t keys[6][8] = {{0}};
        uint8_t kv[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        uint8_t *j = NULL; size_t jl = 0; char w[192] = "";
        uint64_t m0 = s2a_libpack_misses();
        memcpy(keys[0], kv, 8); keys[1][0] = 0xde; keys[1][1] = 0xad;
        CHECK(s2a_libpack_program_v1(lp, keys, 0x3, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j,
              "fs 键缺席 → MISS（半程序不命中）");
        keys[1][0] = 0xaa; keys[1][1] = 0xbb; keys[1][2] = 0xcc; keys[1][3] = 0xdd;
        keys[1][4] = 0xee; keys[1][5] = 0xff; keys[1][6] = 0x00; keys[1][7] = 0x11;
        CHECK(s2a_libpack_program_v1(lp, keys, 0x3, 1, "h", "c", &j, &jl, w, sizeof w) == 1,
              "fs 键只有 _b.json 缺 _b.spv → MISS（半程序不命中）");
        CHECK(s2a_libpack_program_v1(lp, keys, 0x1 /* 只有 vs */, 1, "h", "c", &j, &jl, w, sizeof w) == 1,
              "vs-only 形状：v1 接受口径是 vs+fs → MISS 不产半成品");
        memcpy(keys[1], kv, 8);
        CHECK(s2a_libpack_program_v1(lp, keys, 0x5 /* vs|gs */, 1, "h", "c", &j, &jl, w, sizeof w) == 1,
              "程序含 gs（vk 库不出、v1 不接受）→ MISS 不冒充命中");
        CHECK(s2a_libpack_misses() == m0 + 4, "miss counter +4");
    }

    // ---- compute_v1：cs 键命中 → compute v1，逐字段核对合成字节 + 真 parse_compute 采纳门 ----
    {
        uint8_t *j = NULL; size_t jl = 0; char w[256] = "";
        uint64_t h0 = s2a_libpack_hits();
        int rc = s2a_libpack_compute_v1(lp, CS_KEY, 0 /* aligned */, 2608,
                                        7, "sha256-bbb", "closure-cs", &j, &jl, w, sizeof w);
        CHECK(rc == 0 && j && jl == strlen((const char *)j), "cs program → HIT, NUL-terminated v1 bytes");
        if (rc) printf("why=%s\n", w);
        CHECK(s2a_libpack_hits() == h0 + 1, "compute hit counter +1");
        CHECK(has(j, "\"kind\":\"compute\""), "compute kind");
        CHECK(has(j, "\"hasUnalignedStorageBuffer\":false"), "unaligned 实值=false 回写");
        CHECK(has(j, "\"c0Reads\":null"), "c0Reads null → unknown 兜底不伪造");
        CHECK(has(j, "\"supportReads\":null"), "supportReads null → unknown 兜底");
        CHECK(has(j, "\"supportLayout\":{\"requiredSize\":2608}"), "supportLayout 传 live profile 值");
        CHECK(has(j, "\"stage\":\"cs\",\"spirv\":\""), "单 cs 阶段");
        CHECK(has(j, "\"attributeReadMask\":0"), "attributeReadMask=0");
        CHECK(has(j, "\"runtimeCompilerABI\":\"s2a-direct-compile-v1\""), "identity 带 accept 合同字段");
        // .spv 声明序出行（名联接）：cp_c3→uniform slot3、cp_s0→storage nvnSlot0/cb 0x310、
        // cp_t→texture slot14、support_buffer→program 支撑、cp_c0→uniform slot0、
        // cp_ub→storage uboPtr2/cb 0x230、cp_sb→storage uboSlot4+uboOffset32/cbSlot7。
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"cs\",\"set\":0,\"binding\":5,\"count\":1,\"slot\":3}"),
              "cp_c3 → uniform binding5 slot3");
        CHECK(has(j, "\"kind\":\"storage\",\"stage\":\"cs\",\"set\":0,\"binding\":11,\"count\":1,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0,\"rw\":true}"),
              "cp_s0 → storage binding11 cbSlot0 cbOffset0x310=784 nvnSlot0 rw");
        CHECK(has(j, "\"kind\":\"texture\",\"stage\":\"cs\",\"set\":0,\"binding\":17,\"count\":1,\"slot\":14}"),
              "cp_t → texture binding17 slot 8+3*2=14");
        CHECK(has(j, "\"kind\":\"support\",\"stage\":\"program\",\"set\":0,\"binding\":29,\"count\":1}"),
              "support_buffer → support program binding29（.spv 名接非 json arg_id）");
        CHECK(has(j, "\"kind\":\"uniform\",\"stage\":\"cs\",\"set\":0,\"binding\":35,\"count\":1,\"slot\":0}"),
              "cp_c0 → uniform slot0（_c0 名解，进 c0 填充）");
        CHECK(has(j, "\"kind\":\"storage\",\"stage\":\"cs\",\"set\":0,\"binding\":41,\"count\":1,\"cbSlot\":0,\"cbOffset\":560,\"uboPtr\":2,\"rw\":true}"),
              "cp_ub → storage uboPtr2 cbOffset0x210+2*16=560");
        CHECK(has(j, "\"kind\":\"storage\",\"stage\":\"cs\",\"set\":0,\"binding\":47,\"count\":1,\"cbSlot\":7,\"cbOffset\":32,\"uboSlot\":4,\"uboOffset\":32,\"rw\":false}"),
              "cp_sb → storage uboSlot4 uboOffset32 cbSlot 4+3=7");
        CHECK(!has(j, "Smplr") && !has(j, "samp_tex"), "采样器半边不出行");
        CHECK(!has(j, "ubolen") && !has(j, "sbsize"), "宿主表 json 条目 .spv 未声明 → 不出行");
        // 采纳门：合成字节过与现编结果同一套 compute v1 解析
        {
            struct nvn_vk_compute_program p; char pw[192] = "";
            memset(&p, 0, sizeof p);
            CHECK(nvn_vk_program_parse_compute(j, jl, &p, pw, sizeof pw) == 0,
                  "合成 compute v1 过 nvn_vk_program_parse_compute");
            if (pw[0]) printf("parse why=%s\n", pw);
            CHECK(p.stage.spirv_bytes == spv_cs_len && p.stage.attr_mask == 0, "cs spirv + mask 0");
            CHECK(p.c0_reads_unknown == 1 && p.support_reads_unknown == 1, "unknown 兜底标志位");
            CHECK(p.has_unaligned_storage_buffer == 0 && p.support_bytes == 2608, "对齐事实 + support 尺寸");
            CHECK(p.binding_count == 7, "8 描述符 -1 采样器半边 = 7 条绑定");
            struct fd_binding *b5 = NULL, *b11 = NULL, *b17 = NULL, *b29 = NULL, *b35 = NULL;
            for (int i = 0; i < p.binding_count; i++) {
                switch (p.bindings[i].binding) {
                    case 5: b5 = &p.bindings[i]; break; case 11: b11 = &p.bindings[i]; break;
                    case 17: b17 = &p.bindings[i]; break; case 29: b29 = &p.bindings[i]; break;
                    case 35: b35 = &p.bindings[i]; break; default: break;
                }
            }
            CHECK(b5 && b5->kind == 0 && b5->slot == 3 && b5->ubo_slot == 0 && b5->stage_idx == 5,
                  "parsed cbuf → uniform slot3 ubo0 stage_idx5");
            CHECK(b11 && b11->kind == 1 && b11->slot == -1, "parsed sbuf → storage 无 slot");
            int s11 = -1, s41 = -1, s47 = -1;
            for (int i = 0; i < p.binding_count; i++) {
                if (p.bindings[i].binding == 11) s11 = i;
                if (p.bindings[i].binding == 41) s41 = i;
                if (p.bindings[i].binding == 47) s47 = i;
            }
            CHECK(s11 >= 0 && p.storage[s11].mapping == NVK_STORAGE_NVN && p.storage[s11].index == 0 &&
                  p.storage[s11].cb_slot == 0 && p.storage[s11].cb_offset == 0x310 && p.storage[s11].writable == 1,
                  "parsed sbuf → NVK_STORAGE_NVN slot0 cb 0x310 writable");
            CHECK(b17 && b17->kind == 2 && b17->tex_slot == 3, "parsed tex → texture tex_slot3");
            CHECK(b29 && b29->is_support && b29->stage_idx == -1, "parsed support → program 无阶段");
            CHECK(b35 && b35->kind == 0 && b35->slot == 0 && b35->ubo_slot == -3, "parsed c0 → uniform slot0");
            CHECK(s41 >= 0 && p.storage[s41].mapping == NVK_STORAGE_UBO_PTR && p.storage[s41].index == 2 &&
                  p.storage[s41].cb_offset == 0x230, "parsed ubo_ptr → UBO_PTR idx2 cb 0x230");
            CHECK(s47 >= 0 && p.storage[s47].mapping == NVK_STORAGE_UBO_ADDRESS && p.storage[s47].index == 4 &&
                  p.storage[s47].offset == 32 && p.storage[s47].cb_slot == 7 && p.storage[s47].writable == 0,
                  "parsed sbuf_ubo → UBO_ADDRESS slot4 off32 cbSlot7 readonly");
            nvn_vk_program_free_compute(&p);
        }
        free(j);
    }

    // ---- compute_v1 负控：miss 与 reject 分清，不静默兜底 ----
    {
        uint8_t *j = NULL; size_t jl = 0; char w[256] = "";
        uint64_t m0 = s2a_libpack_misses(), r0 = s2a_libpack_rejects();
        uint8_t ka[8] = { 0xde, 0xad, 0xbe, 0xef, 0, 1, 2, 3 };
        CHECK(s2a_libpack_compute_v1(lp, ka, 0, 2608, 1, "h", "c", &j, &jl, w, sizeof w) == 1 && !j,
              "cs 键缺席 → MISS");
        CHECK(s2a_libpack_compute_v1(lp, CS_KEY, 0, 0, 1, "h", "c", &j, &jl, w, sizeof w) == -2,
              "support_required=0 → INVALID（必填字段不给空值放行）");
        CHECK(s2a_libpack_misses() == m0 + 1 && s2a_libpack_rejects() == r0, "miss+1 reject 不变");

        // stage 不符：json 是 vs 段、按 cs 键查 → REJECT
        const char *badstage = "{\"key\":\"500a08fca29679ef_b\",\"stage\":\"vs\",\"dm_ctl\":\"500a08fca29679ef\",\"bindings\":[]}";
        struct trec rb[2] = {
            { .key = 0x500a08fca29679ef, .kind = 1, .len = (uint32_t)strlen(badstage) },
            { .key = 0x500a08fca29679ef, .kind = 52, .len = (uint32_t)spv_cs_len },
        };
        const uint8_t *pb[2] = { (const uint8_t *)badstage, spv_cs };
        size_t nb[2] = { strlen(badstage), spv_cs_len };
        CHECK(mk_pack(rb, 2, pb, nb, 1) == 0, "bad-stage pack written");
        struct s2a_libpack *l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open bad-stage pack");
        uint8_t kbad[8] = { 0x50, 0x0a, 0x08, 0xfc, 0xa2, 0x96, 0x79, 0xef };
        CHECK(s2a_libpack_compute_v1(l2, kbad, 0, 2608, 1, "h", "c", &j, &jl, w, sizeof w) == -1,
              "json stage=vs 按 cs 查 → REJECT");
        s2a_libpack_close(l2);

        // .spv 声明了 json/名都接不了的描述符 → REJECT（不静默丢绑定）
        uint8_t spv_bad[512];
        uint32_t *wp = (uint32_t *)spv_bad;
        wp[0] = 0x07230203; wp[1] = 0x10000; wp[2] = 0; wp[3] = 64; wp[4] = 0; wp += 5;
        wp = spv_var(wp, 10, "cp_c3", 5);
        wp = spv_var(wp, 18, "mystery_image", 11);   // arg1：名不在 json、非 _cN 解码
        size_t spv_bad_len = (uint8_t *)wp - spv_bad;
        const char *minimal_cs = "{\"key\":\"600a08fca29679ef_b\",\"stage\":\"cs\",\"dm_ctl\":\"600a08fca29679ef\",\"bindings\":[]}";
        struct trec rc2[2] = {
            { .key = 0x600a08fca29679ef, .kind = 1, .len = (uint32_t)strlen(minimal_cs) },
            { .key = 0x600a08fca29679ef, .kind = 52, .len = (uint32_t)spv_bad_len },
        };
        const uint8_t *pc[2] = { (const uint8_t *)minimal_cs, spv_bad };
        size_t nc[2] = { strlen(minimal_cs), spv_bad_len };
        CHECK(mk_pack(rc2, 2, pc, nc, 1) == 0, "unmappable-desc pack written");
        l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open unmappable pack");
        uint8_t kc[8] = { 0x60, 0x0a, 0x08, 0xfc, 0xa2, 0x96, 0x79, 0xef };
        CHECK(s2a_libpack_compute_v1(l2, kc, 0, 2608, 1, "h", "c", &j, &jl, w, sizeof w) == -1 && !j,
              ".spv 声明不可映射描述符 → REJECT 不静默丢绑定");
        s2a_libpack_close(l2);

        // storage 缺 rw → REJECT（出处字段缺省不造）
        const char *norw = "{\"key\":\"700a08fca29679ef_b\",\"stage\":\"cs\",\"dm_ctl\":\"700a08fca29679ef\",\"bindings\":["
            "{\"msl_name\":\"cp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":0,\"arg_id\":1}]}";
        uint8_t spv_norw[512];
        wp = (uint32_t *)spv_norw;
        wp[0] = 0x07230203; wp[1] = 0x10000; wp[2] = 0; wp[3] = 64; wp[4] = 0; wp += 5;
        wp = spv_var(wp, 11, "cp_s0", 11);
        size_t spv_norw_len = (uint8_t *)wp - spv_norw;
        struct trec rd[2] = {
            { .key = 0x700a08fca29679ef, .kind = 1, .len = (uint32_t)strlen(norw) },
            { .key = 0x700a08fca29679ef, .kind = 52, .len = (uint32_t)spv_norw_len },
        };
        const uint8_t *pd[2] = { (const uint8_t *)norw, spv_norw };
        size_t nd[2] = { strlen(norw), spv_norw_len };
        CHECK(mk_pack(rd, 2, pd, nd, 1) == 0, "no-rw pack written");
        l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open no-rw pack");
        uint8_t kd[8] = { 0x70, 0x0a, 0x08, 0xfc, 0xa2, 0x96, 0x79, 0xef };
        CHECK(s2a_libpack_compute_v1(l2, kd, 0, 2608, 1, "h", "c", &j, &jl, w, sizeof w) == -1,
              "storage 缺 rw → REJECT");
        s2a_libpack_close(l2);

        // arg_id 与 .spv arg 漂移 → REJECT（双源不一致不猜用谁）
        const char *drift = "{\"key\":\"710a08fca29679ef_b\",\"stage\":\"cs\",\"dm_ctl\":\"710a08fca29679ef\",\"bindings\":["
            "{\"msl_name\":\"cp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":9}]}";
        uint8_t spv_drift[512];
        wp = (uint32_t *)spv_drift;
        wp[0] = 0x07230203; wp[1] = 0x10000; wp[2] = 0; wp[3] = 64; wp[4] = 0; wp += 5;
        wp = spv_var(wp, 10, "cp_c3", 5);   // .spv arg0 vs json arg_id9
        size_t spv_drift_len = (uint8_t *)wp - spv_drift;
        struct trec re[2] = {
            { .key = 0x710a08fca29679ef, .kind = 1, .len = (uint32_t)strlen(drift) },
            { .key = 0x710a08fca29679ef, .kind = 52, .len = (uint32_t)spv_drift_len },
        };
        const uint8_t *pe[2] = { (const uint8_t *)drift, spv_drift };
        size_t ne[2] = { strlen(drift), spv_drift_len };
        CHECK(mk_pack(re, 2, pe, ne, 1) == 0, "arg-drift pack written");
        l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open drift pack");
        uint8_t ke[8] = { 0x71, 0x0a, 0x08, 0xfc, 0xa2, 0x96, 0x79, 0xef };
        CHECK(s2a_libpack_compute_v1(l2, ke, 0, 2608, 1, "h", "c", &j, &jl, w, sizeof w) == -1,
              "json arg_id 与 .spv arg 漂移 → REJECT");
        s2a_libpack_close(l2);
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
            { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof SPV },
            { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(bad) },
            { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof SPV },
        };
        const uint8_t *p2[4] = { (const uint8_t *)VS_JSON, SPV, (const uint8_t *)bad, SPV };
        size_t n2[4] = { strlen(VS_JSON), sizeof SPV, strlen(bad), sizeof SPV };
        CHECK(mk_pack(r2, 4, p2, n2, 1) == 0, "dm_ctl-mismatch pack written");
        struct s2a_libpack *l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open mismatch pack");
        CHECK(s2a_libpack_program_v1(l2, keys, 0x3, 1, "h", "c", &j, &jl, w, sizeof w) == -1 && !j,
              "fs 段 dm_ctl 不符 → 程序 REJECT 不采纳");
        s2a_libpack_close(l2);

        // fs 键的 spv 头坏（magic 不对）
        uint8_t badspv[20]; memcpy(badspv, SPV, sizeof SPV); badspv[0] = 0xAA;
        struct trec r3[4] = {
            { .key = 0x0123456789abcdef, .kind = 1,  .len = (uint32_t)strlen(VS_JSON) },
            { .key = 0x0123456789abcdef, .kind = 52, .len = sizeof SPV },
            { .key = 0x1032547698badcfe, .kind = 1,  .len = (uint32_t)strlen(PS_JSON) },
            { .key = 0x1032547698badcfe, .kind = 52, .len = sizeof badspv },
        };
        const uint8_t *p3[4] = { (const uint8_t *)VS_JSON, SPV, (const uint8_t *)PS_JSON, badspv };
        size_t n3[4] = { strlen(VS_JSON), sizeof SPV, strlen(PS_JSON), sizeof badspv };
        CHECK(mk_pack(r3, 4, p3, n3, 1) == 0, "bad-spv pack written");
        l2 = NULL;
        CHECK(s2a_libpack_open(&l2, g_dir) == 1, "open bad-spv pack");
        CHECK(s2a_libpack_program_v1(l2, keys, 0x3, 1, "h", "c", &j, &jl, w, sizeof w) == -1,
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
    }

    s2a_libpack_close(lp);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl); (void)system(cmd);
    if (fails) { printf("libpack_vk_test: %d FAIL\n", fails); return 1; }
    printf("libpack_vk_test: all checks passed\n");
    return 0;
}
