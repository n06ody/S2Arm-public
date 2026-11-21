// t_lib_deny.c — 出库对照（lib_deny）宿主单测：stub 掉 asset 桥 / 日志 / env，直测
// nvk_lib_deny.inc 的三结局与键命中。不修产品语义，修的是「拷盘版在冷目录上静默失效」。
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define NVN_PLAT_LOG_INFO 1
#define NVN_PLAT_LOG_WARN 2
#define NVN_PLAT_LOG_RED 3
static int g_log_n; static int g_log_lvl[8]; static char g_log[8][1400];
static void nvn_plat_log(int lvl, const char *tag, const char *msg) {
    (void)tag;
    if (g_log_n < 8) { g_log_lvl[g_log_n] = lvl; snprintf(g_log[g_log_n], sizeof g_log[0], "%s", msg); }
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
    g_deny_n = 0; g_log_n = 0;
    g_asset_rc = rc;
    snprintf(g_asset_buf, sizeof g_asset_buf, "%s", content ? content : "");
    unsetenv("S2A_LIB_DENY");
}

int main(void) {
    uint8_t key_a[8] = {0x0d,0x05,0x60,0x78,0xef,0x89,0x7f,0xc6};
    uint8_t keys[2][8] = {{0x0d,0x05,0x60,0x78,0xef,0x89,0x7f,0xc6},{0x98,0x78,0x6e,0xbb,0x64,0xc3,0xf0,0x56}};
    uint8_t other[2][8] = {{0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00,0x11},{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}};

    /* 资产读出 6 键（真实包内容）：解析 6 个、WARN 行、deny 命中逐键 */
    reset(1, "0d056078ef897fc6,98786ebb64c3f056,1b34757836a5a7d6,430d5297e599846d,b54d4232878a4863,a648f5860f2320a6\n");
    g_asset_rc = (long long)strlen(g_asset_buf);
    lib_deny_init();
    CHECK(g_deny_n == 6, "六键应解析 6 个");
    CHECK(g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_WARN && strstr(g_log[0], "6 个程序键"),
          "成功应有 WARN 出库对照行");
    CHECK(lib_deny_hit(keys, 0x3, NULL) == 1, "deny 键应命中取库拦截");
    CHECK(lib_deny_hit(other, 0x3, NULL) == 0, "非 deny 键不该拦");
    CHECK(lib_deny_hit(NULL, 0, key_a) == 1, "计算 ckey 也应命中");

    /* 包内无件（-2）：INFO 行、不拦 */
    reset(-2, NULL);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_INFO && strstr(g_log[0], "无 lib_deny.txt"),
          "无件应有 INFO 行且不启用");
    CHECK(lib_deny_hit(keys, 0x3, NULL) == 0, "无件不该拦");

    /* 桥失败（-3 冷目录 ENOENT 同型）：红行、不拦、有可见证据 */
    reset(-3, NULL);
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_RED && strstr(g_log[0], "-3"),
          "桥失败应报红并带返回码");
    CHECK(lib_deny_hit(keys, 0x3, NULL) == 0, "桥失败不该拦");

    /* 读到但无键可解：红行（配置件在包里却解析不出，不能静默） */
    reset(5, "not-a-key\n");
    lib_deny_init();
    CHECK(g_deny_n == 0 && g_log_n == 1 && g_log_lvl[0] == NVN_PLAT_LOG_RED && strstr(g_log[0], "没有可解析"),
          "解析 0 键应报红");

    /* env 优先于包件 */
    reset(1, "0d056078ef897fc6");
    setenv("S2A_LIB_DENY", "aabbccddeeff0011", 1);
    lib_deny_init();
    CHECK(g_deny_n == 1 && memcmp(g_deny[0], "\xaa\xbb\xcc\xdd\xee\xff\x00\x11", 8) == 0,
          "env 键应优先");
    CHECK(lib_deny_hit(other, 0x3, NULL) == 1, "env 键应命中");

    if (fails) { printf("【红】%d 项不符\n", fails); return 1; }
    printf("✓ t_lib_deny：解析/命中/无件 INFO/桥失败红/零键红/env 优先 全过\n");
    return 0;
}
