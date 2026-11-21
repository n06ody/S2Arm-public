// t_lib_deny.c — 出库对照（lib_deny）宿主单测：stub 掉 asset 桥 / 日志 / env，直测
// nvk_lib_deny.inc 的三结局、整表校验发布、匹配位图、后台单记与前台「绕库+现编」成对取证。
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
static int g_log_n; static int g_log_lvl[32]; static char g_log[32][1400];
static void nvn_plat_log(int lvl, const char *tag, const char *msg) {
    (void)tag;
    if (g_log_n < 32) { g_log_lvl[g_log_n] = lvl; snprintf(g_log[g_log_n], sizeof g_log[0], "%s", msg); }
    g_log_n++;
}

static long long g_asset_rc; static char g_asset_buf[1200];
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
    g_deny_n = 0; g_deny_by_n = 0; g_deny_pair_n = 0; g_log_n = 0;
    g_asset_rc = rc;
    snprintf(g_asset_buf, sizeof g_asset_buf, "%s", content ? content : "");
    unsetenv("S2A_LIB_DENY");
}

int main(void) {
    uint8_t ident_a[32] = {0x61, 0x61, 0x61, 0x61};   // 后台预热身份
    uint8_t ident_b[32] = {0x62, 0x62, 0x62, 0x62};   // 前台真编译身份（同键不同身份）
    uint8_t ident_c[32] = {0x63, 0x63, 0x63, 0x63};   // 未进匹配身份
    uint8_t key_c[8] = {0x1b,0x34,0x75,0x78,0x36,0xa5,0xa7,0xd6};   // 表内第三键，留计算段用（未被前面配对消耗）
    uint8_t keys[2][8] = {{0x0d,0x05,0x60,0x78,0xef,0x89,0x7f,0xc6},{0x98,0x78,0x6e,0xbb,0x64,0xc3,0xf0,0x56}};
    uint8_t other[2][8] = {{0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00,0x11},{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}};

    /* 资产读出 6 键（真实包内容）：首个 hit 触发 pthread_once init → 解析 WARN */
    reset(1, "0d056078ef897fc6,98786ebb64c3f056,1b34757836a5a7d6,430d5297e599846d,b54d4232878a4863,a648f5860f2320a6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    /* 反例 A·后台先行：预热身份 A 双阶段命中 → 位图 0x3 + 两行后台单记（不占前台成对名额） */
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 1) == 0x3, "后台 vs+fs 双命中应回位图 0x3");
    CHECK(g_deny_n == 6, "六键应解析 6 个");
    CHECK(g_log_n == 3 && g_log_lvl[0] == NVN_PLAT_LOG_WARN && strstr(g_log[0], "6 个程序键"),
          "成功应有 WARN 出库对照行");
    CHECK(strstr(g_log[1], "0d056078ef897fc6") && strstr(g_log[1], "vs") && strstr(g_log[1], "后台")
          && strstr(g_log[1], "61616161"), "后台 vs 单记行应有全键+阶段+身份");
    CHECK(strstr(g_log[2], "98786ebb64c3f056") && strstr(g_log[2], "fs") && strstr(g_log[2], "后台")
          && strstr(g_log[2], "61616161"), "后台 fs 单记行应有全键+阶段+身份");
    /* 反例 A·前台真局：身份 B 同键再命中 → 位图 0x3、fetch 处不打行；到本请求编译处才合记成对行 */
    CHECK(lib_deny_hit(ident_b, keys, 0x3, NULL, 0) == 0x3, "前台双命中应回位图 0x3");
    CHECK(g_log_n == 3, "前台 fetch 处不打绕过行（成对取证在编译入口）");
    lib_deny_note_compile(ident_b, 0x3, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5 && strstr(g_log[3], "0d056078ef897fc6") && strstr(g_log[3], "已绕库")
          && strstr(g_log[3], "进入现编") && strstr(g_log[3], "62626262") && strstr(g_log[3], "vs")
          && strstr(g_log[3], "图形"), "B 的 vs 应打「已绕库→进入现编」成对行带 B 身份");
    CHECK(strstr(g_log[4], "98786ebb64c3f056") && strstr(g_log[4], "进入现编")
          && strstr(g_log[4], "62626262") && strstr(g_log[4], "fs"),
          "B 的 fs 应打「已绕库→进入现编」成对行带 B 身份（后台单记不得吞前台成对）");
    lib_deny_note_compile(ident_b, 0x3, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5, "成对行每键有界一次");
    /* 反例 B：本请求没进匹配（fetch 早退/未绕过）→ mask 0，键在表里也不该有现编证据 */
    lib_deny_note_compile(ident_c, 0, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5, "mask=0 不该有现编取证行（没绕过不能算成对）");
    /* 前台另一请求同键已配对 → 位图仍命中但不再重打成对行 */
    CHECK(lib_deny_hit(ident_c, keys, 0x3, NULL, 0) == 0x3, "异身份同键位图照中");
    lib_deny_note_compile(ident_c, 0x3, (const uint8_t (*)[8])keys, 2, "图形");
    CHECK(g_log_n == 5, "成对行按键有界一次（不因身份重复打）");
    /* 另键仍放行（库命中不受影响） */
    CHECK(lib_deny_hit(ident_b, other, 0x3, NULL, 0) == 0, "非 deny 键位图 0 不该拦");
    /* 计算键：前台命中 → 位图 bit0 + 编译处成对行（计算/cs） */
    CHECK(lib_deny_hit(ident_b, NULL, 0, key_c, 0) == 1, "计算 ckey 位图 bit0");
    lib_deny_note_compile(ident_b, 1, (const uint8_t (*)[8])&key_c, 1, "计算");
    CHECK(g_log_n == 6 && strstr(g_log[5], "1b34757836a5a7d6") && strstr(g_log[5], "计算")
          && strstr(g_log[5], "进入现编"), "计算键成对行应带种类+cs");

    /* 包内无件（-2）：INFO 行、不拦 */
    reset(-2, NULL);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_INFO && strstr(g_log[0], "无 lib_deny.txt"),
          "无件应有 INFO 行且不启用");
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 0) == 0, "无件不该拦");

    /* 桥失败（-3 冷目录 ENOENT 同型）：红行、不拦、有可见证据 */
    reset(-3, NULL);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_RED && strstr(g_log[0], "-3"),
          "桥失败应报红并带返回码");
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 0) == 0, "桥失败不该拦");

    /* 负控：合法前缀 + 后行畸形 → 整表红、发布 0 项（部分生效不算对照证据） */
    reset(1, "0d056078ef897fc6,98786ebb64c3f056,not-a-key,1b34757836a5a7d6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_RED && strstr(g_log[0], "格式错误")
          && strstr(g_log[0], "0 项"), "合法前缀+坏行应整表红且 0 项");
    CHECK(lib_deny_hit(ident_a, keys, 0x3, NULL, 0) == 0, "整表无效不该拦");
    /* 负控：读到但 0 键可解 */
    reset(5, "not-a-key\n");
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_lvl[0] == NVN_PLAT_LOG_RED, "0 键应报红");

    /* env 优先于包件 */
    reset(1, "0d056078ef897fc6");
    setenv("S2A_LIB_DENY", "aabbccddeeff0011", 1);
    lib_deny_init();
    CHECK(g_deny_n == 1 && memcmp(g_deny[0], "\xaa\xbb\xcc\xdd\xee\xff\x00\x11", 8) == 0,
          "env 键应优先");
    CHECK(lib_deny_hit(ident_a, other, 0x3, NULL, 0) == 1, "env 键应命中");

    if (fails) { printf("【红】%d 项不符\n", fails); return 1; }
    printf("✓ t_lib_deny：解析/整表校验/位图枚举/后台单记不占前台/异身份成对/mask=0 零取证/无件 INFO/桥失败红/畸形整表 0 项 全过\n");
    return 0;
}
