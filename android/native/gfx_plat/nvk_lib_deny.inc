// 对照包诊断：程序键出库排除（强制现编）。两路配置，取第一路非空：
//   ① S2A_LIB_DENY=<逗号分隔 16hex 程序键>（本地调试）；
//   ② APK 内 assets/game/lib_deny.txt——经 s2a_android_read_asset 直读进内存
//     （release 清环境但保留包内 asset；不落盘——直编缓存目录冷开局后才由首个缓存写建出，
//       拷盘版在首次取库时 fopen 写目标必 ENOENT、once 又不重试，对照静默失效过一次）。
// 表解析先整表校验再一次发布：合法前缀后接畸形行 = 整表红、0 项生效（部分生效没法当对照证据）。
// 三结局都留行：解析到键（WARN）/ 包内无此件（INFO）/ 读取或解析失败（红）。
// 取证打点（前台提交线程与后台预热都进 libpack_fetch，g_deny_mu 同步）：
//   - lib_deny_hit 枚举本请求全部匹配阶段、返回匹配位图（bit st = 该阶段键被排）；fetch 早退时位图 0。
//   - 前台：位图由调用方带到本请求的同步编译入口，真进编译才合记一行「本请求已绕库 → 进入现编」
//     （全键+阶段+种类+本请求身份；每键有界一次，表 g_deny_pair）——绕过与现编天然同键同身份成对，
//     不依赖跨请求日志排序；没绕过/没真编译的请求零取证行。
//   - 后台/预热（bg=1）：每个被排键打一行「取库绕过（后台）」，表 g_deny_by 单记、不占前台成对名额。
// 另键不受影响、照走库命中。不是产品语义。
extern long long s2a_android_read_asset(const char *name, void *buf, long long cap);
static uint8_t g_deny[64][8]; static unsigned g_deny_n;
static pthread_once_t g_deny_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_deny_mu = PTHREAD_MUTEX_INITIALIZER;
static void lib_deny_parse(const char *s, const char *from) {
    uint8_t tmp[64][8]; unsigned n = 0; int bad = 0;
    while (s && *s) {
        while (*s == ',' || *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') s++;
        if (!*s) break;
        if (n >= 64) { bad = 1; break; }
        for (int i = 0; i < 16; i++) {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { bad = 1; break; }
        }
        if (bad) break;
        for (int i = 0; i < 8; i++) { unsigned x = 0; sscanf(s + i * 2, "%2x", &x); tmp[n][i] = (uint8_t)x; }
        n++; s += 16;
        if (*s && *s != ',' && *s != ' ' && *s != '\n' && *s != '\t' && *s != '\r') { bad = 1; break; }
    }
    char b[1400];
    if (bad) {
        snprintf(b, sizeof b, "出库对照：%s 第 %u 个键起格式错误，整表不发布（0 项生效）", from, n + 1);
        nvn_plat_log(NVN_PLAT_LOG_RED, "shader", b);
        return;
    }
    memcpy(g_deny, tmp, n * 8); g_deny_n = n;
    if (n) {
        snprintf(b, sizeof b, "出库对照：%u 个程序键被 %s 排除出取库，强制现编", n, from);
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
static void lib_deny_key_str(char out[17], const uint8_t k[8]) {
    for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", k[i]);
}
static uint8_t g_deny_by[64][8]; static unsigned g_deny_by_n;    // 后台单记「取库绕过」已打键（每键一次）
static uint8_t g_deny_pair[64][8]; static unsigned g_deny_pair_n; // 前台「绕库+现编」成对行已打键（每键一次）
static int lib_deny_listed(const uint8_t (*tab)[8], unsigned tn, const uint8_t k[8]) {
    for (unsigned j = 0; j < tn; j++) if (!memcmp(tab[j], k, 8)) return 1;
    return 0;
}
// 枚举本请求全部匹配阶段，返回匹配位图（bit st = 该阶段键被排；ckey 恒用 bit0）。
// bg=1（后台/预热）：每个被排键各打一行单记（每键一次），不占前台成对名额；
// bg=0（前台）：不打行——位图由调用方带往本请求编译处成对取证。
static unsigned lib_deny_hit(const uint8_t *identity, const uint8_t (*gkeys)[8],
                             uint32_t stages, const uint8_t *ckey, int bg) {
    pthread_once(&g_deny_once, lib_deny_init);
    if (!g_deny_n) return 0;
    static const char *const stn[3] = {"vs", "fs", "gs"};
    int n = ckey ? 1 : (stages == 0x7u ? 3 : 2);
    unsigned mask = 0;
    for (unsigned i = 0; i < g_deny_n; i++)
        for (int st = 0; st < n; st++) {
            const uint8_t *k = ckey ? ckey : gkeys[st];
            if (memcmp(k, g_deny[i], 8)) continue;
            mask |= 1u << st;
            if (!bg) continue;
            pthread_mutex_lock(&g_deny_mu);
            if (g_deny_by_n < 64 && !lib_deny_listed(g_deny_by, g_deny_by_n, k)) {
                char ks[17], m[192]; lib_deny_key_str(ks, k);
                memcpy(g_deny_by[g_deny_by_n++], k, 8);
                snprintf(m, sizeof m, "出库对照：程序键 %s（%s）取库绕过（后台/预热，身份 %02x%02x%02x%02x…）",
                         ks, ckey ? "cs" : stn[st],
                         identity ? identity[0] : 0, identity ? identity[1] : 0,
                         identity ? identity[2] : 0, identity ? identity[3] : 0);
                nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", m);
            }
            pthread_mutex_unlock(&g_deny_mu);
        }
    return mask;
}
// 真正进入同步编译处调：deny_mask 是本请求 libpack_fetch 的匹配位图（早退/没绕过 = 0 → 零取证行），
// 每个已绕库键合记一行「已绕库 → 进入现编」（每键有界一次、带本请求身份；后台不走这里）。
static void lib_deny_note_compile(const uint8_t *identity, unsigned deny_mask,
                                  const uint8_t (*keys)[8], int nst, const char *kind) {
    if (!deny_mask) return;
    static const char *const stn[3] = {"vs", "fs", "gs"};
    for (int st = 0; st < nst && st < 3; st++) {
        if (!(deny_mask & (1u << st))) continue;
        const uint8_t *k = keys[st];
        pthread_mutex_lock(&g_deny_mu);
        if (g_deny_pair_n < 64 && !lib_deny_listed(g_deny_pair, g_deny_pair_n, k)) {
            char ks[17], m[224]; lib_deny_key_str(ks, k);
            memcpy(g_deny_pair[g_deny_pair_n++], k, 8);
            snprintf(m, sizeof m, "出库对照：程序键 %s（%s，%s）本请求已绕库 → 进入现编（身份 %02x%02x%02x%02x…）",
                     ks, kind, nst == 1 ? "cs" : stn[st],
                     identity ? identity[0] : 0, identity ? identity[1] : 0,
                     identity ? identity[2] : 0, identity ? identity[3] : 0);
            nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", m);
        }
        pthread_mutex_unlock(&g_deny_mu);
    }
}
