// 对照包诊断：程序键出库排除（强制现编）。两路配置，取第一路非空：
//   ① S2A_LIB_DENY=<逗号分隔 16hex 程序键>（本地调试）；
//   ② APK 内 assets/game/lib_deny.txt——经 s2a_android_read_asset 直读进内存
//     （release 清环境但保留包内 asset；不落盘——直编缓存目录冷开局后才由首个缓存写建出，
//       拷盘版在首次取库时 fopen 写目标必 ENOENT、once 又不重试，对照静默失效过一次）。
// 三结局都留行：解析到键（WARN）/ 包内无此件（INFO）/ 读取或解析失败（红）。不是产品语义。
extern long long s2a_android_read_asset(const char *name, void *buf, long long cap);
static uint8_t g_deny[64][8]; static unsigned g_deny_n;
static void lib_deny_parse(const char *s, const char *from) {
    while (s && *s && g_deny_n < 64) {
        while (*s == ',' || *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') s++;
        if (!*s) break;
        int ok = 1;
        for (int i = 0; i < 8; i++) {
            unsigned x = 0;
            if (sscanf(s + i * 2, "%2x", &x) != 1) { ok = 0; break; }
            g_deny[g_deny_n][i] = (uint8_t)x;
        }
        if (!ok) break;
        g_deny_n++; s += 16;
    }
    char b[1400];
    if (g_deny_n) {
        snprintf(b, sizeof b, "出库对照：%u 个程序键被 %s 排除出取库，强制现编", g_deny_n, from);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", b);
    } else {
        snprintf(b, sizeof b, "出库对照：%s 里没有可解析的程序键，不启用键排除", from);
        nvn_plat_log(NVN_PLAT_LOG_RED, "shader", b);
    }
}
static void lib_deny_init(void) {
    const char *s = getenv("S2A_LIB_DENY");
    if (s && *s) { lib_deny_parse(s, "S2A_LIB_DENY"); return; }
    char buf[64 * 17 + 8];
    long long n = s2a_android_read_asset("game/lib_deny.txt", buf, sizeof buf - 1);
    if (n == -2) { nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", "出库对照：包内无 lib_deny.txt，不启用键排除"); return; }
    if (n <= 0) {
        char m[96]; snprintf(m, sizeof m, "出库对照：lib_deny.txt 读取失败（%lld），不启用键排除", n);
        nvn_plat_log(NVN_PLAT_LOG_RED, "shader", m);
        return;
    }
    buf[n] = 0;
    lib_deny_parse(buf, "lib_deny.txt");
}
static int lib_deny_hit(const uint8_t (*gkeys)[8], uint32_t stages, const uint8_t *ckey) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, lib_deny_init);
    if (!g_deny_n) return 0;
    int n = ckey ? 1 : (stages == 0x7u ? 3 : 2);
    for (unsigned i = 0; i < g_deny_n; i++)
        for (int st = 0; st < n; st++)
            if (!memcmp(ckey ? ckey : gkeys[st], g_deny[i], 8)) return 1;
    return 0;
}
