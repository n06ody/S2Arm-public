// t_lib_deny.c — 出库对照（lib_deny）宿主单测：stub 掉 asset 桥 / 日志 / env，直测
// nvk_lib_deny.inc 的三结局、整表校验发布、any/group/group-allow 三模式、匹配位图、
// 后台单记与前台「绕库+现编」成对取证、库命中首中逐键行（去重含前台/预热）。
// 取证口径：前台位图由本请求 libpack_fetch 带往本请求同步编译入口（早退/没绕过 = mask 0 → 零行）；
// 后台/预热单记「取库绕过」、不占前台成对名额。不修产品语义。
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define NVN_PLAT_LOG_INFO 1
#define NVN_PLAT_LOG_WARN 2
#define NVN_PLAT_LOG_RED 3
static int g_log_n; static int g_log_lvl[256]; static char g_log[256][1400];
static void nvn_plat_log(int lvl, const char *tag, const char *msg) {
    (void)tag;
    if (g_log_n < 256) { g_log_lvl[g_log_n] = lvl; snprintf(g_log[g_log_n], sizeof g_log[0], "%s", msg); }
    g_log_n++;
}

static long long g_asset_rc; static char g_asset_buf[600 * 1024];
long long s2a_android_read_asset(const char *name, void *buf, long long cap) {
    if (strcmp(name, "game/lib_deny.txt")) return -9;
    if (g_asset_rc < 0) return g_asset_rc;
    long long n = (long long)strlen(g_asset_buf);
    if (n > cap) n = cap;
    memcpy(buf, g_asset_buf, (size_t)n);
    return n;
}

#include "../native/gfx_plat/nvk_lib_deny.inc"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { fails++; printf("【红】%s\n", msg); } } while (0)

static void reset(long long rc, const char *content) {
    g_deny_n = 0; g_deny_mode = LIB_DENY_ANY; g_deny_by_n = 0; g_deny_pair_n = 0;
    g_hit_fg_n = 0; g_hit_bg_n = 0; g_hit_ovf[0] = g_hit_ovf[1] = 0; g_log_n = 0;
    g_asset_rc = rc;
    if (content) snprintf(g_asset_buf, sizeof g_asset_buf, "%s", content); else g_asset_buf[0] = 0;
    unsetenv("S2A_LIB_DENY");
}

int main(void) {
    uint8_t ident_a[32] = {0x61, 0x61, 0x61, 0x61};   // 后台预热身份
    uint8_t ident_b[32] = {0x62, 0x62, 0x62, 0x62};   // 前台真编译身份（同键不同身份）
    uint8_t ident_c[32] = {0x63, 0x63, 0x63, 0x63};   // 未进匹配身份
    uint8_t key_c[8] = {0x1b,0x34,0x75,0x78,0x36,0xa5,0xa7,0xd6};   // 表内第三键，留计算段用（未被前面配对消耗）
    uint8_t keys[2][8] = {{0x0d,0x05,0x60,0x78,0xef,0x89,0x7f,0xc6},{0x98,0x78,0x6e,0xbb,0x64,0xc3,0xf0,0x56}};
    uint8_t other[2][8] = {{0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00,0x11},{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}};

    /* ===== any 模式（r3 旧语义，缺省）：6 键逗号表 ===== */
    reset(1, "0d056078ef897fc6,98786ebb64c3f056,1b34757836a5a7d6,430d5297e599846d,b54d4232878a4863,a648f5860f2320a6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 1) == 0x3, "any：后台 vs+fs 双命中应回位图 0x3");
    CHECK(g_deny_n == 6 && g_deny_mode == LIB_DENY_ANY, "any：六键应解析 6 个、模式 any");
    CHECK(g_log_n == 3 && g_log_lvl[0] == NVN_PLAT_LOG_WARN && strstr(g_log[0], "6 个程序键"),
          "any：成功应有 WARN 出库对照行");
    CHECK(strstr(g_log[1], "0d056078ef897fc6") && strstr(g_log[1], "vs") && strstr(g_log[1], "后台")
          && strstr(g_log[1], "61616161") && strstr(g_log[1], "组 0d056078ef897fc6"), "any：后台 vs 单记行应有全键+阶段+身份+组");
    CHECK(strstr(g_log[2], "98786ebb64c3f056") && strstr(g_log[2], "fs") && strstr(g_log[2], "后台")
          && strstr(g_log[2], "61616161"), "any：后台 fs 单记行应有全键+阶段+身份");
    CHECK(lib_deny_hit(ident_b, keys, 0x3, NULL, 0) == 0x3, "any：前台双命中应回位图 0x3");
    CHECK(g_log_n == 3, "any：前台 fetch 处不打绕过行（成对取证在编译入口）");
    lib_deny_note_compile(ident_b, 0x3, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5 && strstr(g_log[3], "0d056078ef897fc6") && strstr(g_log[3], "已绕库")
          && strstr(g_log[3], "进入现编") && strstr(g_log[3], "62626262") && strstr(g_log[3], "vs")
          && strstr(g_log[3], "图形"), "any：B 的 vs 应打成对行带 B 身份");
    CHECK(strstr(g_log[4], "98786ebb64c3f056") && strstr(g_log[4], "进入现编")
          && strstr(g_log[4], "62626262") && strstr(g_log[4], "fs"),
          "any：B 的 fs 应打成对行带 B 身份（后台单记不得吞前台成对）");
    lib_deny_note_compile(ident_b, 0x3, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5, "any：成对行每键有界一次");
    lib_deny_note_compile(ident_c, 0, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5, "any：mask=0 不该有现编取证行（没绕过不能算成对）");
    CHECK(lib_deny_hit(ident_c, keys, 0x3, NULL, 0) == 0x3, "any：异身份同键位图照中");
    lib_deny_note_compile(ident_c, 0x3, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5, "any：成对行按键有界一次（不因身份重复打）");
    CHECK(lib_deny_hit(ident_b, other, 0x3, NULL, 0) == 0, "any：非 deny 键位图 0 不该拦");
    CHECK(lib_deny_hit(ident_b, NULL, 0, key_c, 0) == 1, "any：计算 ckey 位图 bit0");
    lib_deny_note_compile(ident_b, 1, (const uint8_t (*)[8])&key_c, 1, "计算");
    CHECK(g_log_n == 6 && strstr(g_log[5], "1b34757836a5a7d6") && strstr(g_log[5], "计算")
          && strstr(g_log[5], "进入现编"), "any：计算键成对行应带种类+cs");

    /* ===== group 模式：组键（vs）在表 → 整请求绕；fs 键在表不拦 ===== */
    reset(1, "mode=group\n0d056078ef897fc6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(lib_deny_hit(ident_b, keys, 0x3, NULL, 0) == 0x3,
          "group：vs 组键在表 → vs+fs 整请求位图 0x3（同 VS/多 FS 全归一半）");
    CHECK(g_deny_n == 1 && g_deny_mode == LIB_DENY_GROUP, "group：应解析 1 键、模式 group");
    /* 负控：表里只有 fs 键 → 组键(vs)不在表 → 不拦（any 模式同表会拦 fs——这正是分组的意义） */
    reset(1, "mode=group\n98786ebb64c3f056\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(lib_deny_hit(ident_b, keys, 0x3, NULL, 0) == 0,
          "group：fs 键在表不算组键 → 不拦（负控：同表 any 模式会拦）");
    /* group 模式下计算键 = 自身组键 */
    reset(1, "mode=group\n1b34757836a5a7d6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(lib_deny_hit(ident_b, NULL, 0, key_c, 0) == 1, "group：cs 组键在表 → 位图 bit0");
    /* group 后台：整请求被排 → vs+fs 各打一行单记，行带组键 */
    reset(1, "mode=group\n0d056078ef897fc6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 1) == 0x3, "group：后台组键命中 → 位图 0x3");
    CHECK(g_log_n == 3 && strstr(g_log[1], "0d056078ef897fc6") && strstr(g_log[1], "后台")
          && strstr(g_log[2], "98786ebb64c3f056") && strstr(g_log[2], "组 0d056078ef897fc6"),
          "group：后台被排应 vs/fs 各一行、行带组键");

    /* ===== group-allow 模式：组键不在表 → 整请求绕（小集合白名单） ===== */
    reset(1, "mode=group-allow\n0d056078ef897fc6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(lib_deny_hit(ident_b, keys, 0x3, NULL, 0) == 0, "allow：组键在表 → 放行取库");
    CHECK(lib_deny_hit(ident_b, other, 0x3, NULL, 0) == 0x3, "allow：组键不在表 → 整请求位图 0x3");
    CHECK(lib_deny_hit(ident_b, NULL, 0, key_c, 0) == 1, "allow：cs 组键不在表 → 位图 bit0");

    /* ===== 库命中首中逐键行（同 libpack_note_hit 条件）：去重含前台/预热 ===== */
    reset(1, "mode=group\naabbccddeeff0011\n");   // deny 表发布才启用首中登记
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    int base_n = g_log_n;
    lib_deny_note_hit_gfx((const uint8_t (*)[8])other, 2, 0);
    CHECK(g_log_n == base_n + 2 && strstr(g_log[base_n], "库命中 程序键 aabbccddeeff0011（vs，前台，组 aabbccddeeff0011）")
          && strstr(g_log[base_n + 1], "1122334455667788（fs，前台，组 aabbccddeeff0011）"),
          "首中：vs+fs 各一行、行含全键+阶段+前台+组键");
    lib_deny_note_hit_gfx((const uint8_t (*)[8])other, 2, 0);
    CHECK(g_log_n == base_n + 2, "首中：同键同侧去重不重复打");
    lib_deny_note_hit_gfx((const uint8_t (*)[8])other, 2, 1);
    CHECK(g_log_n == base_n + 4 && strstr(g_log[base_n + 2], "（vs，预热，组 aabbccddeeff0011）"),
          "首中：去重键含前台/预热——同键预热侧再各一行");
    lib_deny_note_hit(key_c, "cs", key_c, 0);
    CHECK(g_log_n == base_n + 5 && strstr(g_log[base_n + 4], "1b34757836a5a7d6（cs，前台，组 1b34757836a5a7d6）"),
          "首中：计算键单行（阶段 cs、组=自身）");
    /* deny 未发布 → 不登记（产品路径零开销零日志） */
    reset(-2, NULL);
    lib_deny_init();
    lib_deny_note_hit(other[0], "vs", other[0], 0);
    CHECK(g_log_n == 1, "未发布 deny 表时首中登记静默");

    /* ===== 三结局与整表校验（v2 同口径） ===== */
    reset(-2, NULL);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_INFO && strstr(g_log[0], "无 lib_deny.txt"),
          "无件应有 INFO 行且不启用");
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 0) == 0, "无件不该拦");

    reset(-3, NULL);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_RED && strstr(g_log[0], "-3"),
          "桥失败应报红并带返回码");
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 0) == 0, "桥失败不该拦");

    /* 负控：合法前缀 + 后行畸形 → 整表红、发布 0 项 */
    reset(1, "mode=group\n0d056078ef897fc6,not-a-key,1b34757836a5a7d6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_RED && strstr(g_log[0], "格式错误")
          && strstr(g_log[0], "0 项"), "合法前缀+坏行应整表红且 0 项");
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 0) == 0, "整表无效不该拦");

    /* 负控：重复键 → 整表红 */
    reset(1, "mode=group\n0d056078ef897fc6\n0d056078ef897fc6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_lvl[0] == NVN_PLAT_LOG_RED && strstr(g_log[0], "0 项"), "重复键应整表红");

    /* 负控：第二条 mode 指令 / mode 在键后 → 格式错误整表红 */
    reset(1, "mode=group\nmode=any\n0d056078ef897fc6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_lvl[0] == NVN_PLAT_LOG_RED, "第二条 mode 应整表红");
    reset(1, "0d056078ef897fc6\nmode=group\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_lvl[0] == NVN_PLAT_LOG_RED, "键后 mode 应整表红");

    /* 容量：4,530 键组表解析 + 二分查找命中首尾（二分语义下顺序无关，顺带验证乱序输入排序） */
    reset(1, NULL);
    {
        int off = snprintf(g_asset_buf, sizeof g_asset_buf, "mode=group\n");
        for (int i = 4529; i >= 0; i--)
            off += snprintf(g_asset_buf + off, sizeof g_asset_buf - off, "%08x%08x\n", 0x10000000u + (unsigned)i, 0x5a5a5a5au + (unsigned)i);
        g_asset_rc = (long long)strlen(g_asset_buf);
        lib_deny_init();
        CHECK(g_deny_n == 4530 && g_log_lvl[0] == NVN_PLAT_LOG_WARN, "4530 键应全量解析发布（逆序输入排序后发布）");
        uint8_t first[8] = {0x10,0x00,0x00,0x00,0x5a,0x5a,0x5a,0x5a}, last[8] = {0x10,0x00,0x11,0xa1,0x5a,0x5a,0x6b,0xfb};
        uint8_t gk[2][8]; memcpy(gk[0], last, 8); memcpy(gk[1], other[1], 8);
        CHECK(lib_deny_hit(ident_b, (const uint8_t (*)[8])gk, 0x3, NULL, 0) == 0x3, "组表末键应命中（二分不丢）");
        CHECK(lib_deny_tab_has(g_deny, g_deny_n, first), "组表首键应命中");
    }

    /* env 优先于包件（缺省 any） */
    reset(1, "0d056078ef897fc6");
    setenv("S2A_LIB_DENY", "aabbccddeeff0011", 1);
    lib_deny_init();
    CHECK(g_deny_n == 1 && g_deny_mode == LIB_DENY_ANY && memcmp(g_deny[0], "\xaa\xbb\xcc\xdd\xee\xff\x00\x11", 8) == 0,
          "env 键应优先且缺省 any");
    CHECK(lib_deny_hit(ident_a, other, 0x3, NULL, 0) == 1, "env 键应命中");

    if (fails) { printf("【红】%d 项不符\n", fails); return 1; }
    printf("✓ t_lib_deny：any/group/group-allow 三模式、整表校验、位图枚举、后台单记不占前台、异身份成对、\n"
           "  mask=0 零取证、库命中首中（去重含前台/预热+组键）、无件 INFO、桥失败红、畸形/重复/二指令整表 0 项、4530 键二分 全过\n");
    return 0;
}
